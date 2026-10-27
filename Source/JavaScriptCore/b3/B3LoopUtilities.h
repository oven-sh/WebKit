/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3ValueInlines.h"
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

inline bool isInnermostLoop(const NaturalLoops& loops, const NaturalLoop& loop)
{
    for (unsigned i = loop.size(); i--;) {
        if (loops.innerMostLoopOf(loop.at(i)) != &loop)
            return false;
    }
    return true;
}

inline bool isLoopPoll(const Value* value)
{
    auto* patchpoint = value->as<PatchpointValue>();
    return patchpoint && patchpoint->isLoopPoll;
}

// A patchpoint that is given no register has nothing to compute with: it only holds its operands alive.
inline bool onlyKeepsValuesAlive(const Value* value)
{
    auto* patchpoint = value->as<PatchpointValue>();
    if (!patchpoint || patchpoint->type() != Void || patchpoint->numGPScratchRegisters || patchpoint->numFPScratchRegisters || patchpoint->isLoopPoll)
        return false;
    if (!patchpoint->earlyClobbered().isEmpty() || !patchpoint->lateClobbered().isEmpty())
        return false;
    for (unsigned index = 0; index < patchpoint->numChildren(); ++index) {
        ValueRep::Kind kind = patchpoint->constrainedChild(index).rep().kind();
        if (kind != ValueRep::ColdAny && kind != ValueRep::LateColdAny)
            return false;
    }
    return true;
}

inline bool canCloneForLoop(const Value* value)
{
    if (value->kind().isCloningForbidden() || value->type().isTuple())
        return false;
    return value->type() == Void || !value->effects().terminal;
}

inline bool emitsInstructions(const Value* value)
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

inline FrequencyClass likeliestSuccessorClass(const BasicBlock* block)
{
    FrequencyClass result = FrequencyClass::Rare;
    for (const FrequentedBlock& successor : block->successors())
        result = maxFrequency(result, successor.frequency());
    return result;
}

inline bool isLikelySuccessor(FrequencyClass likeliestClassOfBlock, const FrequentedBlock& successor)
{
    return !successor.isRare() && !isRarerThan(successor.frequency(), likeliestClassOfBlock);
}

inline bool isLikelySuccessor(const BasicBlock* block, const FrequentedBlock& successor)
{
    if (successor.isLikely())
        return true;
    return isLikelySuccessor(likeliestSuccessorClass(block), successor);
}

JS_EXPORT_PRIVATE BasicBlock* findPreHeader(const NaturalLoops&, const NaturalLoop&);
JS_EXPORT_PRIVATE IndexSet<BasicBlock*> findLikelyBlocks(const NaturalLoops&, const NaturalLoop&);
JS_EXPORT_PRIVATE bool hasLikelyBackEdge(const NaturalLoop&, const IndexSet<BasicBlock*>& likelyBlocks);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
