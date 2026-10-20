/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowAnalysis.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowAnalysisInternal.h"
#include "AOTFlowCallOperations.h"
#include "AOTFlowObjectOperations.h"
#include "AOTPhaseTimes.h"
#include "AOTThreads.h"
#include <memory>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

Vector<FlowSummary> buildFlowSummaries(const Program& program, BuildContext& build)
{
    FlowSummaryInputs inputs { program, build };
    unsigned hostIndex = program.functions.size();
    Vector<FlowSummary> summaries(hostIndex + 1);
    forEachIndexInParallel(build.parallelLoops(), hostIndex + 1, [&](unsigned task) {
        // The host summary is linear in the variables of the program, so it starts first.
        unsigned index = task ? task - 1 : hostIndex;
        if (index == hostIndex) {
            summaries[index] = buildHostSummary(inputs);
            return;
        }
        FunctionID function(index);
        if (program.at(function).hasBody())
            summaries[index] = buildFlowSummary(inputs, function);
        else
            summaries[index].function = function;
    });
    return summaries;
}

FlowSolvedProgram::FlowSolvedProgram(const Program& program, TypeTable& table, BuildContext& build)
    : program(program)
    , build(build)
    , types(table)
{
}

FlowSolvedProgram::~FlowSolvedProgram() = default;

FlowSolution FlowSolvedProgram::solution() const
{
    return { program, build, summaries.span(), *objects, *graph, records, *states.objects, *states.properties, *states.calls };
}

static bool isWrittenByProgram(const Module& module)
{
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        return !module.flags.contains(ModuleFlags::IsLibrary);
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportLoadsByUnknownName(const Program& program, const FlowRecords& records, BuildContext& build)
{
    for (const Witness& witness : records.loadsByUnknownName()) {
        FunctionID function(witness.function);
        if (!isWrittenByProgram(program.at(program.at(function).module)))
            continue;
        build.diagnostics().add(UserDiagnostic {
            .kind = UserDiagnosticKind::CodeOutsideImageIsRefused,
            .function = function,
            .value = witness.value == Wire::none ? ValueID() : ValueID(witness.value),
            .location = witness.location,
        });
    }
}

static void reportFixpointChanges(const FlowGraph& graph, std::span<const FlowFixpointChange> changes, BuildContext& build)
{
    for (const FlowFixpointChange& change : changes) {
        dataLogLn("AOT flow fixpoint: "_s, change.description);
        bool isInFunction = change.site.instance && !graph.isHost(change.site.instance);
        build.diagnostics().add(InternalError {
            .kind = InternalErrorKind::FlowNotAtFixpoint,
            .function = isInFunction ? graph.at(change.site.instance).function : FunctionID(),
            .value = isInFunction ? change.origin : ValueID(),
            .count = change.node ? change.node.index() : 0,
        });
    }
}

std::unique_ptr<FlowSolvedProgram> solveFlow(const Program& program, TypeTable& table, BuildContext& build)
{
    PhaseTimes times(build, PassID::AnalyzeFlow);
    auto solved = makeUnique<FlowSolvedProgram>(program, table, build);

    solved->summaries = buildFlowSummaries(program, build);
    times.didFinish("summaries"_s);

    solved->objects = makeUnique<FlowObjectTable>(program);
    createAllocationClasses(*solved->objects, solved->summaries.span(), build);
    times.didFinish("allocation classes"_s);

    solved->values = makeUnique<FlowValueTable>(program, *solved->objects, solved->types, build);
    solved->graph = makeUnique<FlowGraph>(program, *solved->objects, solved->summaries.span(), *solved->values);
    {
        FlowSolver solver(program, *solved->objects, *solved->graph, build);
        solver.solve();
        solved->fixpointChanges = solver.takeFixpointChanges();
        solved->records = solver.takeRecords();
        solved->states = solver.takeStates();
    }
    reportLoadsByUnknownName(program, solved->records, build);
    reportFixpointChanges(*solved->graph, solved->fixpointChanges.span(), build);
    times.didFinish("solve"_s);
    return solved;
}

FlowResults analyzeFlow(Program& program, TypeTable& types, BuildContext& build)
{
    RELEASE_ASSERT(&types == &program.types);
    RELEASE_ASSERT(program.form() == IRForm::AsRead);

    std::unique_ptr<FlowSolvedProgram> solved = solveFlow(program, types, build);
    FlowResults results = computeFlowResults(solved->solution(), program, *solved->objects, types);
    build.destroyLater(WTF::move(solved));
    return results;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
