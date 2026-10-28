/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include "AOTSummarizeForInlining.h"
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

enum class InlineDecision : uint8_t {
    Unknown,
    Inline,
    InlineHead,
    Barrier,
    SignatureMismatch,
    ClosesCycle,
    DepthLimit,
    CallerSizeLimit,
    CostExceedsBenefit,
};

enum class InlineBenefit : uint8_t {
    CallOverhead = 1 << 0,
    FoldedCode = 1 << 1,
    KnownCalleeArgument = 1 << 2,
    KnownReceiver = 1 << 3,
    SunkAllocation = 1 << 4,
    Wrapper = 1 << 5,
    KnownKey = 1 << 6,
};

struct InlineVerdict {
    InlineDecision decision { InlineDecision::Unknown };
    OptionSet<InlineBenefit> benefits;
    uint32_t costBytes { 0 };
    uint32_t benefitBytes { 0 };
    uint32_t budgetBytes { 0 };
};

InlineVerdict computeInlineVerdict(FunctionPassContext&, const InliningSummary& callee, BlockID blockOfCall, ValueID call);

unsigned inlineDepthOf(const Program&, const Function& caller, uint32_t inlineFrame);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
