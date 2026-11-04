/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTLosses.h"
#include "AOTPass.h"
#include "AOTVector.h"
#include <span>
#include <wtf/ScopedLambda.h>

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

Expected<unsigned> sinkableAllocationsAfterInlining(FunctionPassContext&, BlockID blockOfCall, ValueID call, NOESCAPE const ScopedLambda<unsigned()>& freshAllocationsReturnedByCallee);

void convertListCall(Value& call, std::span<const ValueID> elements);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
