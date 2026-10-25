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
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

bool eliminateRedundantPhiCycles(Procedure& proc)
{
    PhaseScope phaseScope(proc, "eliminateRedundantPhiCycles"_s);
    PhiChildren phiChildren(proc);
    const auto& phis = phiChildren.phis();
    if (phis.isEmpty())
        return false;

    IndexMap<Value*, Value*> representatives(proc.values().size());
    auto settle = [&] {
        bool changed = true;
        while (changed) {
            changed = false;
            for (Value* phi : phis) {
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
                    changed = true;
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
    while (true) {
        settle();
        bool gaveUp = false;
        for (Value* phi : phis) {
            Value* representative = representatives[phi];
            if (representative == phi)
                continue;
            if (!representative || !dominators.strictlyDominates(representative->owner, phi->owner)) {
                representatives[phi] = phi;
                gaveUp = true;
            }
        }
        if (!gaveUp)
            break;
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

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
