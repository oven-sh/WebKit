/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3EliminateRedundantPhiCycles.h"

#if ENABLE(B3_JIT)

#include "B3Dominators.h"
#include "B3PhaseScope.h"
#include "B3PhiChildren.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include <algorithm>
#include <functional>
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

static bool foldPhisThatFollowTheirUpsilon(Procedure& proc)
{
    IndexSet<Value*> folded;
    Vector<UpsilonValue*, 8> upsilonsOfBlock;
    for (BasicBlock* block : proc) {
        upsilonsOfBlock.shrink(0);
        for (Value* value : *block) {
            if (auto* upsilon = value->as<UpsilonValue>()) {
                upsilonsOfBlock.append(upsilon);
                continue;
            }
            if (value->opcode() != Phi)
                continue;
            size_t last = upsilonsOfBlock.reverseFindIf([&](UpsilonValue* upsilon) { return upsilon->phi() == value; });
            if (last == notFound || upsilonsOfBlock[last]->child(0) == value)
                continue;
            value->replaceWithIdentity(upsilonsOfBlock[last]->child(0));
            folded.add(value);
        }
    }
    if (folded.isEmpty())
        return false;
    for (Value* value : proc.values()) {
        if (auto* upsilon = value->as<UpsilonValue>(); upsilon && folded.contains(upsilon->phi()))
            upsilon->replaceWithNop();
    }
    for (Value* value : proc.values())
        value->performSubstitution();
    return true;
}

static bool eliminateCycles(Procedure& proc)
{
    PhiChildren phiChildren(proc);
    const auto& phis = phiChildren.phis();
    if (phis.isEmpty())
        return false;

    IndexMap<Value*, Value*> representatives(proc.values().size());
    IndexMap<Value*, unsigned> indexOfPhi(proc.values().size());
    for (unsigned index = 0; index < phis.size(); ++index)
        indexOfPhi[phis[index]] = index;

    auto forEachPhiChild = [&](Value* phi, const auto& functor) {
        for (Value* child : phiChildren[phi].values()) {
            if (child->opcode() == Phi)
                functor(indexOfPhi[child]);
        }
    };
    Vector<unsigned> firstReader(FillWith { }, phis.size() + 1, 0u);
    for (Value* phi : phis)
        forEachPhiChild(phi, [&](unsigned child) { ++firstReader[child + 1]; });
    for (unsigned index = 0; index < phis.size(); ++index)
        firstReader[index + 1] += firstReader[index];
    Vector<unsigned> readers(firstReader.last());
    {
        Vector<unsigned> next = firstReader;
        for (unsigned index = 0; index < phis.size(); ++index)
            forEachPhiChild(phis[index], [&](unsigned child) { readers[next[child]++] = index; });
    }

    // Sweep over the phis in order until nothing changes. A sweep only visits phis that read a phi that changed since their last visit.
    Vector<unsigned> sweep;
    Vector<unsigned> nextSweep(phis.size(), [](size_t index) { return static_cast<unsigned>(index); });
    Vector<bool> isInSweep(FillWith { }, phis.size(), false);
    Vector<bool> isInNextSweep(FillWith { }, phis.size(), true);
    Vector<unsigned> changedPhis;
    auto enqueueReadersOf = [&](unsigned index, bool isSweeping) {
        for (unsigned position = firstReader[index]; position < firstReader[index + 1]; ++position) {
            unsigned reader = readers[position];
            if (isSweeping && reader > index) {
                if (std::exchange(isInSweep[reader], true))
                    continue;
                sweep.append(reader);
                std::push_heap(sweep.begin(), sweep.end(), std::greater<unsigned>());
            } else if (!std::exchange(isInNextSweep[reader], true))
                nextSweep.append(reader);
        }
    };
    auto settle = [&] {
        while (!nextSweep.isEmpty()) {
            sweep = std::exchange(nextSweep, { });
            for (unsigned index : sweep) {
                isInNextSweep[index] = false;
                isInSweep[index] = true;
            }
            std::make_heap(sweep.begin(), sweep.end(), std::greater<unsigned>());
            while (!sweep.isEmpty()) {
                std::pop_heap(sweep.begin(), sweep.end(), std::greater<unsigned>());
                unsigned index = sweep.takeLast();
                isInSweep[index] = false;
                Value* phi = phis[index];
                if (representatives[phi] == phi)
                    continue;
                Value* result = nullptr;
                for (Value* child : phiChildren[phi].values()) {
                    Value* value = child->opcode() == Phi ? representatives[child] : child;
                    if (!value || value == phi)
                        continue;
                    if (!result) {
                        result = value;
                        continue;
                    }
                    if (result != value) {
                        result = phi;
                        break;
                    }
                }
                if (result && representatives[phi] != result) {
                    representatives[phi] = result;
                    changedPhis.append(index);
                    enqueueReadersOf(index, true);
                }
            }
        }
    };

    settle();
    bool mayChange = false;
    for (Value* phi : phis)
        mayChange |= representatives[phi] && representatives[phi] != phi;
    if (!mayChange)
        return false;
    proc.resetValueOwners();
    Dominators& dominators = proc.dominators();
    // Only phis whose representative changed need to be checked again.
    Vector<unsigned> phisToCheck(phis.size(), [](size_t index) { return static_cast<unsigned>(index); });
    while (true) {
        bool gaveUp = false;
        for (unsigned index : phisToCheck) {
            Value* phi = phis[index];
            Value* representative = representatives[phi];
            if (representative == phi)
                continue;
            if (!representative || !dominators.strictlyDominates(representative->owner, phi->owner)) {
                representatives[phi] = phi;
                enqueueReadersOf(index, false);
                gaveUp = true;
            }
        }
        if (!gaveUp)
            break;
        changedPhis.shrink(0);
        settle();
        phisToCheck = std::exchange(changedPhis, { });
    }

    bool changed = false;
    for (Value* phi : phis) {
        Value* representative = representatives[phi];
        if (representative == phi)
            continue;
        for (Value* upsilon : phiChildren[phi])
            upsilon->replaceWithNop();
        phi->replaceWithIdentity(representative);
        changed = true;
    }
    if (!changed)
        return false;
    for (Value* value : proc.values())
        value->performSubstitution();
    return true;
}

bool eliminateRedundantPhiCycles(Procedure& proc)
{
    PhaseScope phaseScope(proc, "eliminateRedundantPhiCycles"_s);
    bool changed = foldPhisThatFollowTheirUpsilon(proc);
    changed |= eliminateCycles(proc);
    return changed;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
