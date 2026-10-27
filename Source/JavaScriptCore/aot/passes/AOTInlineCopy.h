/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInlineCostModel.h"
#include "AOTRegionCloner.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

size_t inlineFrameCountOf(const Function&);

uint32_t appendInlineFrames(Function& caller, const InlineFrame& callFrame, const Function& callee);

ValueID upcastToParameterType(IRBuilder&, const Value& parameter, ValueID argument);

// A read whose type holds from the entry of its function on names no fact there. In a frame of another function it names one that says where the frame begins.
bool takesFactOfEntry(const Program&, const Value&, bool calleeIsPredicted);
void cloneBlocksIntoInlineFrame(IRBuilder&, RegionCloner&, std::span<const BlockID> region, uint32_t firstFrame);
// A function that is constructed stays on the stack until it has seen what its last call returns: no call of the frame counts as a tail call.
void keepFrameBelowTailCalls(Function&, std::span<const BlockID>, uint32_t frame);

InlineDecision canInline(const FunctionPassContext&, const InliningSummary& callee, ValueID call);

enum class InlinedPart : uint8_t { WholeBody, Head };

struct InlineRequest {
    ProgramPoint point;
    ValueID call;
    InlinedPart part { InlinedPart::WholeBody };
    const InliningSummary& callee;
    IRBuilder::DeferredResultReads* deferredResultReads { nullptr };
};

// Keeps every id of the caller valid. narrowValueTypes() may renumber them, so typing the copy again is left to the caller's last step.
void copyIn(FunctionPassContext&, IRBuilder&, const InlineRequest&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
