/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirDuplicateReturns.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirFrameEmission.h"
#include "AirInstInlines.h"
#include "AirOpcodeUtils.h"
#include "AirPhaseScope.h"

namespace JSC { namespace B3 { namespace Air {

static bool isWorthDuplicating(Code& code, BasicBlock* target)
{
    if (!isReturn(target->last().kind.opcode))
        return false;
    if (!code.policy().frame.emitsFrame)
        return target->size() == 1 && isX86() && code.calleeSaveRegisters().isEmpty();
    if (target->size() == 1)
        return true;
    if (target->size() != 2 || target->at(0).kind.opcode != LeaveFrame)
        return false;
    constexpr unsigned bytesOfReturn = isX86() ? 1 : 4;
    constexpr unsigned bytesOfShortJump = isX86() ? 2 : 4;
    return maxBytesOfFrameTeardown(code) + bytesOfReturn <= bytesOfShortJump;
}

bool duplicateReturns(Code& code)
{
    PhaseScope phaseScope(code, "duplicateReturns"_s);
    bool didChange = false;
    for (BasicBlock* block : code) {
        if (block->last().kind.opcode != Jump)
            continue;
        BasicBlock* target = block->successorBlock(0);
        if (target == code.findNextBlock(block) || !isWorthDuplicating(code, target))
            continue;
        block->insts().removeLast();
        for (const Inst& inst : *target)
            block->appendInst(inst);
        block->successors().clear();
        didChange = true;
    }
    if (didChange)
        code.resetReachability();
    return didChange;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
