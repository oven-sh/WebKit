/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowSolver.h"
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowSolution {
    const Program& program;
    BuildContext& build;
    std::span<const FlowSummary> summaries;
    const FlowObjectTable& objects;
    const FlowGraph& graph;
    const FlowRecords& records;
    const FlowObjectState& objectState;
    const FlowPropertyState& propertyState;
    const FlowCallState& callState;

    const FlowValueTable& values() const { return graph.values(); }
    const BuiltinTable& builtins() const { return program.builtins(); }
    const FlowObjectSet& objectsInNativeCode() const LIFETIME_BOUND { return graph.valueOf(graph.nativeCode()).objects; }
    const FlowObjectSet& objectsInUnknownJavaScript() const LIFETIME_BOUND { return graph.valueOf(graph.unknownJavaScript()).objects; }
    bool isPastWorkLimit(FunctionID function) const { return records.functionsPastWorkLimit().get(function.index()); }
};

inline void forEachPossibleObject(const FlowSolution& solution, const FlowObjectSet& objects, NOESCAPE const Invocable<void(FlowObjectID)> auto& functor)
{
    objects.forEach(functor);
    if (objects.containsUnknownObject())
        solution.objectsInNativeCode().forEachNotIn(objects, functor);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
