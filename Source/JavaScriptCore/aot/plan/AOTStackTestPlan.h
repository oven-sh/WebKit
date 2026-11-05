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
    BitVector bodiesCoveredByCallers;
    BitVector slowBodiesCoveredByCallers;
    BitVector coroutineBodiesCoveredByCallers;
    BitVector bodiesWhoseCallsAreCountedBelowCallers;
    Vector<uint8_t> coveredFramesOfCall;
    Vector<std::pair<FunctionID, FunctionID>> callsCountedAtBareSites;

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
    return isGenericEntry(unit.role) || unit.role == CodeRole::ParameterGuard || unit.role == CodeRole::TableEntry;
}

inline bool StackTestPlan::hasCountedAtBareSite(FunctionID caller, FunctionID callee) const
{
    return std::ranges::binary_search(callsCountedAtBareSites, std::pair { caller, callee });
}

JS_EXPORT_PRIVATE StackTestPlan planStackTests(const Program&, const Facts&, const Plan&);
void checkStackTests(const Program&, const Facts&, const Plan&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
