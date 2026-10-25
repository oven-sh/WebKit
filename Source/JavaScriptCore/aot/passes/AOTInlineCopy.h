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

void cloneBlocksIntoInlineFrame(IRBuilder&, RegionCloner&, std::span<const BlockID> region, uint32_t firstFrame);

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
