/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlock.h"
#include "B3PatchpointValue.h"
#include "B3ValueInlines.h"
#include "GPRInfo.h"

namespace JSC { namespace B3 {

static constexpr unsigned minCallClobberedArgumentGPRs = 4;
static constexpr double maxRatioOfHeaderToLikelyBlock = 16;

inline bool isRare(const FrequentedBlock& successor, BasicBlock* from)
{
    return successor.frequency() == FrequencyClass::Rare || successor.block()->frequency() * maxRatioOfHeaderToLikelyBlock < from->frequency();
}

inline bool isRareForChainPosition(const FrequentedBlock& successor, BasicBlock* from)
{
    if (from->last()->opcode() != Switch || successor.frequency() == FrequencyClass::Rare)
        return isRare(successor, from);
    unsigned numberOfCases = from->numSuccessors() - 1;
    double shareInChain = 0;
    double caseShare = 1;
    for (unsigned i = 0; i < numberOfCases; ++i) {
        caseShare /= 2;
        if (from->successorBlock(i) == successor.block())
            shareInChain += caseShare;
    }
    if (from->successorBlock(numberOfCases) == successor.block())
        shareInChain += caseShare;
    return successor.block()->frequency() * maxRatioOfHeaderToLikelyBlock < from->frequency() * std::min(shareInChain, 1.0 / from->numSuccessors());
}

inline bool isCall(Value* value)
{
    if (value->opcode() == CCall)
        return true;
    auto* patchpoint = value->as<PatchpointValue>();
    if (!patchpoint)
        return false;
    unsigned numberOfClobberedArgumentGPRs = 0;
    for (unsigned i = 0; i < GPRInfo::numberOfArgumentRegisters; ++i)
        numberOfClobberedArgumentGPRs += patchpoint->lateClobbered().contains(GPRInfo::toArgumentRegister(i), IgnoreVectors);
    return numberOfClobberedArgumentGPRs >= minCallClobberedArgumentGPRs;
}

inline bool canCloneForLoop(Value* value)
{
    if (value->kind().isCloningForbidden() || value->type().isTuple())
        return false;
    switch (value->opcode()) {
    case Patchpoint: {
        Effects effects = value->effects();
        return (!effects.terminal || value->type() == Void) && !effects.exitsSideways && !effects.writes && !effects.writesPinned && !effects.fence;
    }
    case CCall:
    case Check:
    case CheckAdd:
    case CheckSub:
    case CheckMul:
        return false;
    default:
        return true;
    }
}

inline bool emitsInstructions(Value* value)
{
    switch (value->opcode()) {
    case Phi:
    case Upsilon:
    case Nop:
    case Identity:
    case Const32:
    case Const64:
    case ConstDouble:
    case ConstFloat:
    case Jump:
        return false;
    default:
        return true;
    }
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
