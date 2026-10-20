/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowAnalysis.h"
#include "AOTFlowSolution.h"
#include <memory>
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowSolvedProgram {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowSolvedProgram);
    WTF_MAKE_NONCOPYABLE(FlowSolvedProgram);

    FlowSolvedProgram(const Program&, TypeTable&, BuildContext&);
    ~FlowSolvedProgram();

    FlowSolution solution() const;

    const Program& program;
    BuildContext& build;
    TypeArena types;
    Vector<FlowSummary> summaries;
    std::unique_ptr<FlowObjectTable> objects;
    std::unique_ptr<FlowValueTable> values;
    std::unique_ptr<FlowGraph> graph;
    FlowRecords records;
    FlowSolverStates states;
    Vector<FlowFixpointChange> fixpointChanges;
};

Vector<FlowSummary> buildFlowSummaries(const Program&, BuildContext&);
std::unique_ptr<FlowSolvedProgram> solveFlow(const Program&, TypeTable&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
