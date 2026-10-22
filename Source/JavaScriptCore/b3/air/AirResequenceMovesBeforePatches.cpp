/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirResequenceMovesBeforePatches.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"

namespace JSC { namespace B3 { namespace Air {

bool resequenceMovesBeforePatches(Code& code)
{
    PhaseScope phaseScope(code, "resequenceMovesBeforePatches"_s);

    auto isSimpleMove = [](Inst& inst) {
        if ((inst.kind.opcode != Move && inst.kind.opcode != MoveDouble) || inst.args().size() != 2 || !inst.args()[1].isReg())
            return false;
        if (inst.args()[0].isReg())
            return true;
        return inst.kind.opcode == Move && (inst.args()[0].isSomeImm() || inst.args()[0].isZeroReg());
    };
    struct PendingMove {
        Arg source;
        Reg destination;
        Inst* original;
    };
    bool changed = false;
    for (BasicBlock* block : code) {
        bool didChange = false;
        for (unsigned end = 0; end < block->size(); ++end) {
            Inst& patch = block->at(end);
            if (patch.kind.opcode != Patch)
                continue;
            // A tail call's frame teardown stands between the moves and the patch. It reads no register.
            Inst* leaveFrame = end && block->at(end - 1).kind.opcode == LeaveFrame ? &block->at(end - 1) : nullptr;
            const unsigned endOfMoves = end - !!leaveFrame;
            unsigned begin = endOfMoves;
            while (begin && block->at(begin - 1) && isSimpleMove(block->at(begin - 1)))
                --begin;
            if (endOfMoves - begin < 2)
                continue;
            Vector<PendingMove, 8> pending;
            bool forwardsWideImmediate = false;
            auto valueOf = [&](Arg arg) {
                if (!arg.isReg())
                    return arg;
                for (auto& move : pending) {
                    if (move.destination == arg.reg()) {
                        forwardsWideImmediate |= move.source.isBigImm();
                        return move.source;
                    }
                }
                return arg;
            };
            for (unsigned i = begin; i < endOfMoves; ++i) {
                Inst& inst = block->at(i);
                Arg source = valueOf(inst.args()[0]);
                Reg destination = inst.args()[1].reg();
                pending.removeAllMatching([&](auto& move) { return move.destination == destination; });
                pending.append({ source, destination, &inst });
            }
            if (forwardsWideImmediate)
                continue;
            RegisterSet used;
            RegisterSet overwritten = patch.extraEarlyClobberedRegs();
            overwritten.merge(patch.extraClobberedRegs());
            patch.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                if (!tmp.isReg())
                    return;
                if (Arg::isAnyUse(role))
                    used.add(tmp.reg(), IgnoreVectors);
                if (Arg::isAnyDef(role))
                    overwritten.add(tmp.reg(), IgnoreVectors);
            });
            if (leaveFrame) {
                leaveFrame->forEachTmpFast([&](Tmp& tmp) {
                    overwritten.add(tmp.reg(), IgnoreVectors);
                    used.remove(tmp.reg());
                });
            }
            pending.removeAllMatching([&](auto& move) {
                if (move.source.isReg() && move.source.reg() == move.destination)
                    return true;
                return overwritten.contains(move.destination, IgnoreVectors) && !used.contains(move.destination, IgnoreVectors);
            });
            if (pending.size() >= endOfMoves - begin)
                continue;
            Vector<Inst, 8> ordered;
            while (!pending.isEmpty()) {
                size_t next = pending.findIf([&](auto& candidate) {
                    return !pending.containsIf([&](auto& other) {
                        return &other != &candidate && other.source.isReg() && other.source.reg() == candidate.destination;
                    });
                });
                if (next == notFound)
                    break;
                auto& move = pending[next];
                ordered.append(Inst(move.destination.isGPR() ? Move : MoveDouble, move.original->origin, move.source, Tmp(move.destination)));
                pending.removeAt(next);
            }
            if (!pending.isEmpty())
                continue;
            for (unsigned i = begin; i < endOfMoves; ++i)
                block->at(i) = i - begin < ordered.size() ? ordered[i - begin] : Inst();
            didChange = true;
            changed = true;
        }
        if (didChange)
            block->insts().removeAllMatching([](const Inst& inst) { return !inst; });
    }
    return changed;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
