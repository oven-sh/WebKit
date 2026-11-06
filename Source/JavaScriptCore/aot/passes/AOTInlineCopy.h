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
bool takesFactOfEntry(const Program&, const Value&);
// A cast without a fact of what the function is handed. It holds from the entry of the function on, so in a frame of another function it names the same fact.
bool narrowsWhatFunctionIsHanded(const Program&, const Function&, const Value&);
// Every such read in the blocks names the fact, or gives a word that may be anything to a cast that names it.
void nameFactOfEntry(const Program&, Function&, TypeArena&, std::span<const BlockID>, ValueID fact, bool calleeIsPredicted);
// For a pass that has put reads where the program looks at a variable, or at what was taken from it: in a frame of another function what the variable holds is known from there on, so each such read gets a fact of its own.
void nameFactsWhereReadsStand(const Program&, Function&, TypeArena&);
// The read gives a word that may be anything, and a cast that names the fact takes its place. Answers the read, which the caller puts in front of the cast or ahead of it.
ValueID readWordAndAssumeInPlace(Function&, TypeArena&, ValueID read, ValueID fact);
void cloneBlocksIntoInlineFrame(IRBuilder&, RegionCloner&, std::span<const BlockID> region, uint32_t firstFrame);
// A function that is constructed stays on the stack until it has seen what its last call returns: no call of the frame counts as a tail call.
void keepFrameBelowTailCalls(Function&, std::span<const BlockID>, uint32_t frame);

InlineDecision canInline(const FunctionPassContext&, const InliningSummary& callee, ValueID call);

enum class InlinedPart : uint8_t { WholeBody, Head, LongHead };

struct InlineRequest {
    ProgramPoint point;
    ValueID call;
    InlinedPart part { InlinedPart::WholeBody };
    const InliningSummary& callee;
    IRBuilder::DeferredResultReads* deferredResultReads { nullptr };
    const BitVector* longHeadBlocks { nullptr };
};

// Keeps every id of the caller valid. narrowValueTypes() may renumber them, so typing the copy again is left to the caller's last step.
void copyIn(FunctionPassContext&, IRBuilder&, const InlineRequest&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
