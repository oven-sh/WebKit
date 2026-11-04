/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Counters and tallies of loops are AOTWalkLoopCounters.cpp's and AOTWalkLoopTallies.cpp's. The two questions that AOTWalkNumberForms.h declares claim nothing, and nobody is hooked to ask them.

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

TypeID rangeOfTallyInLook(Walker&, const BodyIndex&, LoopID, uint32_t, std::span<const TypeID>, TypeID)
{
    return { };
}

std::optional<TestOfSteppedCounter> findTestOfSteppedCounter(const BodyIndex&, const BodyTerminator&)
{
    return std::nullopt;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
