/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRole.h"
#include "AOTID.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Facts;
class Plan;
class Program;

struct StackTestPlan {
    // By function: the Body sets up a frame of at most maxFrameSizeWithoutStackCheck bytes without a test. Of a coroutine it is the ramp. Empty: every one tests.
    BitVector bodiesCoveredByCallers;
    // By function: the same of the SlowBody, which runs the function as it is written. Of a coroutine it is a body of the coroutine, and goes with the CoroutineBody, never with the ramp.
    BitVector slowBodiesCoveredByCallers;
    // Only beside FunctionConvention::coroutineNeverSuspends.
    BitVector coroutineBodiesCoveredByCallers;
    // By function: what the body calls is counted below whoever has the function counted, so a copy of the body brings no call that is not counted. Empty: a call in a copy is never counted.
    BitVector bodiesWhoseCallsAreCountedBelowCallers;
    // By function: how many covered frames a call of it may put below the frame that makes the call. At most maxNestedFunctionsWithoutStackCheck.
    Vector<uint8_t> coveredFramesOfCall;
    // (the body that holds a bare site, the body that a step there calls), ascending, each once.
    Vector<std::pair<FunctionID, FunctionID>> callsCountedAtBareSites;

    // Of an entrance: yes. Each goes on to another unit of its function, and room is kept for its frame beside the frames that are counted.
    bool isCoveredByCallers(CodeUnit) const;
    bool countsCallsOfBodyBelowCallers(FunctionID id) const { return bodiesWhoseCallsAreCountedBelowCallers.get(id.index()); }
    bool hasCountedAtBareSite(FunctionID caller, FunctionID callee) const;
};

inline bool StackTestPlan::isCoveredByCallers(CodeUnit unit) const
{
    if (unit.role == CodeRole::Body)
        return bodiesCoveredByCallers.get(unit.function.index());
    if (unit.role == CodeRole::SlowBody)
        return slowBodiesCoveredByCallers.get(unit.function.index());
    if (unit.role == CodeRole::CoroutineBody)
        return coroutineBodiesCoveredByCallers.get(unit.function.index());
    // Any other role tests, a role that comes to the list later too: nobody has counted its frame.
    return isGenericEntry(unit.role) || unit.role == CodeRole::ParameterGuard || unit.role == CodeRole::TableEntry;
}

inline bool StackTestPlan::hasCountedAtBareSite(FunctionID caller, FunctionID callee) const
{
    return std::ranges::binary_search(callsCountedAtBareSites, std::pair { caller, callee });
}

// Counted below a function: the callees of the calls that linked() or held() lists under it, and the steps at its bare sites.
JS_EXPORT_PRIVATE StackTestPlan planStackTests(const Program&, const Facts&, const Plan&);
// Heights once more, by the longest way over covered bodies: InternalErrorKind::PlacementCheckFailed where one passes the limit or a ring of covered bodies has no member that tests.
void checkStackTests(const Program&, const Facts&, const Plan&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
