/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTLosses.h"
#include "AOTPass.h"
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

enum class SunkAllocationOutcome : uint8_t {
    Unknown,
    NotACandidate,
    Abandoned,
    Removed,
    Sunk,
};

struct SunkAllocation {
    ValueID allocation;
    SunkAllocationOutcome outcome { SunkAllocationOutcome::Unknown };
    unsigned numberOfBuilds { 0 };
    LimitID limit { LimitID::None };
};

Vector<SunkAllocation> predictSinking(FunctionAnalyses&);

unsigned sinkableAllocationsAfterInlining(FunctionPassContext&, ValueID call, NOESCAPE const ScopedLambda<bool()>& calleeReturnsFreshAggregate);

void convertListCall(Value& call, std::span<const ValueID> elements);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
