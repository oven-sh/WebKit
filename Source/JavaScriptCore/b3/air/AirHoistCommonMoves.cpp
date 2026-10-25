/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirHoistCommonMoves.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInstInlines.h"
#include "AirOpcodeUtils.h"
#include "AirPhaseScope.h"

namespace JSC { namespace B3 { namespace Air {

bool hoistCommonMoves(Code& code)
{
    PhaseScope phaseScope(code, "hoistCommonMoves"_s);

    bool changed = false;
    for (BasicBlock* block : code) {
        if (block->numSuccessors() != 2 || block->last().kind.opcode == Patch)
            continue;
        if (usesFlags(block->last().kind.opcode) && !preservesFlags(Move))
            continue;
        BasicBlock* taken = block->successorBlock(0);
        BasicBlock* notTaken = block->successorBlock(1);
        if (taken == notTaken || taken == block || notTaken == block || taken->numPredecessors() != 1 || notTaken->numPredecessors() != 1 || code.isEntrypoint(taken) || code.isEntrypoint(notTaken))
            continue;
        while (taken->size() > 1 && notTaken->size() > 1) {
            Inst move = taken->at(0);
            Inst& other = notTaken->at(0);
            if (move.kind != other.kind || move.kind.opcode != Move || move.args().size() != 2 || move.args()[0] != other.args()[0] || move.args()[1] != other.args()[1])
                break;
            if (!move.args()[1].isTmp() || !(move.args()[0].isTmp() || move.args()[0].isZeroReg()))
                break;
            bool mattersToBranch = false;
            block->last().forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                mattersToBranch |= tmp == move.args()[1].tmp() || (Arg::isAnyDef(role) && move.args()[0].isTmp() && tmp == move.args()[0].tmp());
            });
            if (mattersToBranch)
                break;
            block->insts().insert(block->size() - 1, WTF::move(move));
            taken->insts().removeAt(0);
            notTaken->insts().removeAt(0);
            changed = true;
        }
    }
    return changed;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
