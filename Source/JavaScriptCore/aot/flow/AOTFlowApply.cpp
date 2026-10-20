/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTFlowAnalysis.h"
#include "AOTFunctionEditing.h"
#include "AOTThreads.h"

namespace JSC { namespace AOT { namespace Typed {

void applyPosition(DeclaredType& declared, const FlowPositionUpdate& update)
{
    if (update.type) {
        if (!declared.written)
            declared.written = declared.type;
        declared.type = update.type;
    }
    if (update.predicted)
        declared.predicted = update.predicted;
}

static Vector<FunctionID> collectBodiesLargestFirst(const Program& program, size_t numberOfFunctions, const ParallelLoops& loops)
{
    Vector<FunctionID> functions = collectInParallel<FunctionID>(loops, numberOfFunctions, [&](unsigned index, Vector<FunctionID>& functionsWithBody) {
        if (program.functions[index].hasBody())
            functionsWithBody.append(FunctionID(index));
    });
    parallelSort(loops, functions.mutableSpan(), [&](FunctionID a, FunctionID b) {
        size_t sizeOfA = program.functions[a.index()].values.size();
        size_t sizeOfB = program.functions[b.index()].values.size();
        return sizeOfA != sizeOfB ? sizeOfA > sizeOfB : a < b;
    });
    return functions;
}

// No function counts its runs. In a validating build the first run of a function that runs at most once fills a variable, and a run that finds it filled ends the process.
static Vector<VariableID> createFirstRunMarks(const FlowApplyContext& context, ProgramEditor& editor)
{
    const Program& program = context.program;
    const FlowVersionPlan& versions = context.application.versions;
    Vector<VariableID> marks(context.application.bodies.size());
    if (!context.build.validates())
        return marks;

    Vector<VariableID> markOfBody(versions.functionCountAsRead);
    TypeID markType = editor.types().intern(Type::anything().withPrimitives(Primitive::Absent));
    for (uint32_t index = 0; index < marks.size(); ++index) {
        FunctionID id(index);
        FunctionID body = versions.bodyOf(id);
        if (!program.at(id).hasBody() || !context.results.runsAtMostOnce(body))
            continue;
        VariableID& mark = markOfBody[body.index()];
        if (!mark) {
            Variable variable;
            variable.module = program.at(body).module;
            variable.declared.type = markType;
            mark = editor.addVariable(WTF::move(variable));
        }
        marks[index] = mark;
    }
    return marks;
}

// As the mark of a first run, filled by the store of a variable that is stored at most once and by the definition of a class that is evaluated at most once.
// Every copy of the operation shares the mark.
static FlowFirstTimeMarks createFirstTimeMarks(const FlowApplyContext& context, ProgramEditor& editor)
{
    FlowFirstTimeMarks marks;
    if (!context.build.validates())
        return marks;

    const Program& program = context.program;
    marks.storesByVariable.grow(program.variables.size());
    marks.evaluationsByLayout.grow(program.layouts.size());
    TypeID markType = editor.types().intern(Type::anything().withPrimitives(Primitive::Absent));
    for (uint32_t index = 0; index < context.application.bodies.size(); ++index) {
        FunctionID id(index);
        FunctionID body = context.application.versions.bodyOf(id);
        for (const Value& value : program.at(id).values) {
            VariableID* mark = nullptr;
            if (value.opcode == Opcode::PutModuleVariable && !program.at(value.variable()).owner && program.at(value.variable()).flow.stores == StoreCount::AtMostOnce)
                mark = &marks.storesByVariable[value.variable().index()];
            else if (value.opcode == Opcode::NewClass && program.at(value.layout()).classFlow.evaluations == ExecutionCount::AtMostOnce)
                mark = &marks.evaluationsByLayout[value.layout().index()];
            if (!mark || *mark)
                continue;
            Variable variable;
            variable.module = program.at(body).module;
            variable.declared.type = markType;
            *mark = editor.addVariable(WTF::move(variable));
        }
    }
    return marks;
}

void applyBodyUpdates(const FlowApplyContext& context, ProgramEditor& editor)
{
    FlowFirstTimeMarks firstTimeMarks = createFirstTimeMarks(context, editor);
    Vector<VariableID> firstRunMarks = createFirstRunMarks(context, editor);
    Vector<FunctionID> functions = collectBodiesLargestFirst(context.program, context.application.bodies.size(), context.build.parallelLoops());
    Vector<FlowBodyCounts> countsByTask(functions.size());
    editFunctionsInParallel(editor, context.build, functions.span(), [&](unsigned task, FunctionID id, Function& function, TaskArenas& arenas) {
        countsByTask[task] = applyBodyUpdate(FlowBodyApplication { context.program, context.results, context.application, id, function, arenas.types, context.build, firstRunMarks[id.index()], &firstTimeMarks });
    });

    FlowBodyCounts total;
    for (const FlowBodyCounts& counts : countsByTask) {
        total.removedClaims += counts.removedClaims;
        total.remainingClaims += counts.remainingClaims;
    }
    context.build.statistics().count(StatisticID::FlowRemovedClaims, total.removedClaims);
    context.build.statistics().count(StatisticID::FlowRemainingClaims, total.remainingClaims);
}

// Past form AsRead no Claim may stay on a terminator, and nothing tests the result of a tail call.
static void applyUnknownResults(Program& program, BuildContext& build)
{
    keepExpectedIntegersInSlots(program, build);
    ProgramEditor editor(program);
    Vector<FunctionID> functions = collectBodiesLargestFirst(program, program.functions.size(), build.parallelLoops());
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        demoteTailCallsWithClaims(program, id, function, arenas.types, build);
    });
}

void applyFlowResults(Program& program, const FlowResults& results, BuildContext& build)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);
    if (!results.hasResults()) {
        applyUnknownResults(program, build);
        return;
    }
    const FlowApplication* application = FlowApplyAccess::applicationOf(results);
    RELEASE_ASSERT(application);
    RELEASE_ASSERT(application->versions.functionCountAsRead == program.functions.size());

    FlowApplyContext context { program, results, *application, build };
    ProgramEditor editor(program);
    applyLayoutUpdates(context);
    applyVariableUpdates(context);
    createVersions(context, editor);
    applySignatureUpdates(context);
    predictReceiversOfMembers(context, editor);
    createBoundFunctions(context, editor);
    removeUnreachedBodies(context, editor);
    applyBodyUpdates(context, editor);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
