/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTFunctionEditing.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramCaches.h"
#include "AOTSchemaWalkers.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

bool provesEverything(const BuildContext& build)
{
    return build.recordedOptions().has(TestingSwitch::ProveEverything);
}

static bool isOfRuntimeModule(const Program& program, ModuleID module)
{
    return module && program.at(module).kind == ModuleKind::RuntimeSource;
}

bool provesEverythingIn(const FlowBodyApplication& body)
{
    return provesEverything(body.build) && !isOfRuntimeModule(body.program, body.function.module);
}

// Flow takes members away from the type of a position, and turns a type that was only expected into one that is proven. About a position that it does not prove it says nothing.
void applyPosition(DeclaredType& declared, const FlowPositionUpdate& update)
{
    if (update.type)
        declared.type = update.type;
    if (update.isProven)
        declared.flags.remove(PositionFlags::IsExpected);
}

static void proveEveryPosition(Program& program, BuildContext& build)
{
    auto prove = [](DeclaredType& declared) {
        declared.flags.remove(PositionFlags::IsExpected);
    };
    for (Layout& layout : program.layouts) {
        if (isOfRuntimeModule(program, layout.module))
            continue;
        for (Field& field : layout.fields)
            prove(field.declared);
    }
    for (Variable& variable : program.variables) {
        if (!isOfRuntimeModule(program, variable.module))
            prove(variable.declared);
    }
    for (Function& function : program.functions) {
        if (isOfRuntimeModule(program, function.module))
            continue;
        std::ranges::for_each(function.captures, prove);
        prove(function.thisType);
        std::ranges::for_each(function.parameters, prove);
        std::ranges::for_each(function.results, prove);
    }
    markEnforcedFields(program, build);
    // The tables by name hold what the fields of a family give.
    program.indexNames();
}

static Vector<FunctionID> collectBodiesLargestFirst(const Program& program, size_t numberOfFunctions, const ParallelLoops& loops)
{
    Vector<FunctionID> functions = collectInParallel<FunctionID>(loops, numberOfFunctions, [&](unsigned index, Vector<FunctionID>& functionsWithBody) {
        if (program.functions[index].hasBody())
            functionsWithBody.append(FunctionID(index));
    });
    sortLargestFirst(program, loops, functions);
    return functions;
}

static VariableID addMark(ProgramEditor& editor, ModuleID module, TypeID markType)
{
    Variable variable;
    variable.module = module;
    variable.declared.type = markType;
    return editor.addVariable(WTF::move(variable));
}

// No function counts its runs. In a validating build the first run of a function that runs at most once fills a variable, and a run that finds it filled ends the process.
static Vector<VariableID> createFirstRunMarks(const FlowApplyContext& context, ProgramEditor& editor)
{
    const Program& program = context.program;
    Vector<VariableID> marks(context.application.bodies.size());
    if (!context.build.validates())
        return marks;

    TypeID markType = editor.types().intern(Type::anything().withPrimitives(Primitive::Absent));
    for (uint32_t index = 0; index < marks.size(); ++index) {
        FunctionID id(index);
        if (program.at(id).hasBody() && context.results.runsAtMostOnce(id))
            marks[index] = addMark(editor, program.at(id).module, markType);
    }
    return marks;
}

// As the mark of a first run, filled by the store of a variable that is stored at most once and by the definition of a class that is evaluated at most once.
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
        for (const Value& value : program.at(id).values) {
            VariableID* mark = nullptr;
            if (value.opcode == Opcode::PutModuleVariable && !program.at(value.variable()).owner && program.at(value.variable()).flow.stores == StoreCount::AtMostOnce)
                mark = &marks.storesByVariable[value.variable().index()];
            else if (value.opcode == Opcode::NewClass && program.at(value.layout()).classFlow.evaluations == ExecutionCount::AtMostOnce)
                mark = &marks.evaluationsByLayout[value.layout().index()];
            if (mark && !*mark)
                *mark = addMark(editor, program.at(id).module, markType);
        }
    }
    return marks;
}

static Vector<BitVector> findExpectedFields(const Program& program, const ParallelLoops& loops)
{
    Vector<BitVector> expectedFieldsOfLayouts(program.layouts.size());
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        const Vector<Field>& fields = program.layouts[index].fields;
        BitVector expectedFields(fields.size());
        for (size_t field = 0; field < fields.size(); ++field) {
            if (fields[field].declared.flags.contains(PositionFlags::IsExpected))
                expectedFields.quickSet(field);
        }
        expectedFieldsOfLayouts[index] = WTF::move(expectedFields);
    });
    return expectedFieldsOfLayouts;
}

void applyBodyUpdates(const FlowApplyContext& context, ProgramEditor& editor, const Vector<BitVector>& fieldsExpectedAsRead)
{
    FlowFirstTimeMarks firstTimeMarks = createFirstTimeMarks(context, editor);
    Vector<VariableID> firstRunMarks = createFirstRunMarks(context, editor);
    Vector<FunctionID> functions = collectBodiesLargestFirst(context.program, context.application.bodies.size(), context.build.parallelLoops());
    Vector<FlowBodyCounts> countsByTask(functions.size());
    editFunctionsInParallel(editor, context.build, BehindEdit::Nothing, functions.span(), [&](unsigned task, FunctionID id, Function& function, TaskArenas& arenas) {
        countsByTask[task] = applyBodyUpdate(FlowBodyApplication { context.program, context.results, context.application, id, function, arenas.types, context.build, fieldsExpectedAsRead, firstRunMarks[id.index()], &firstTimeMarks });
    });

    FlowBodyCounts total;
    for (const FlowBodyCounts& counts : countsByTask) {
        total.removedChecks += counts.removedChecks;
        total.remainingChecks += counts.remainingChecks;
    }
    context.build.statistics().count(StatisticID::FlowRemovedChecks, total.removedChecks);
    context.build.statistics().count(StatisticID::FlowRemainingChecks, total.remainingChecks);
}

// Past form AsRead no Check may stay on a terminator, and nothing tests the result of a tail call.
static void applyUnknownResults(Program& program, BuildContext& build)
{
    ProgramEditor editor(program);
    Vector<FunctionID> functions = collectBodiesLargestFirst(program, program.functions.size(), build.parallelLoops());
    editFunctionsInParallel(editor, build, BehindEdit::Nothing, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        demoteTailCallsWithChecks(program, id, function, arenas.types, build);
    });
}

// The bodies have their turn in applyBodyUpdate().
static void nameObjectInPlaceOfDictionaryLayouts(Program& program, const FlowApplication& application, const ParallelLoops& loops)
{
    if (application.typesBehindDictionaries.isEmpty())
        return;
    DictionaryTypeRewriter rewriter(application);
    auto rewriteList = [&](auto& list) {
        forEachIndexInParallel(loops, list.size(), [&](unsigned index) {
            forEachReference(list[index], rewriter);
        });
    };
    forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
        forEachReference(static_cast<FunctionRecord&>(program.functions[index]), rewriter);
    });
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        forEachReference(static_cast<LayoutRecord&>(program.layouts[index]), rewriter);
    });
    rewriteList(program.typeSites);
    rewriteList(program.expectedResults);
    rewriteList(program.variables);
    rewriteList(program.modules);
    rewriteList(program.moduleFailures);
    rewriteList(program.evalScopes);
    rewriteList(program.data);
    rewriteList(program.entries);
    rewriteList(program.runtimeFunctions);
    rewriteList(program.webAssemblyModules);
}

void applyFlowResults(Program& program, FlowResults& results, BuildContext& build)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);
    if (!results.hasResults()) {
        applyUnknownResults(program, build);
        return;
    }
    const FlowApplication* application = FlowApplyAccess::applicationOf(results);
    RELEASE_ASSERT(application);
    RELEASE_ASSERT(application->functionCountAsRead == program.functions.size());

    FlowApplyContext context { program, results, *application, build };
    ProgramEditor editor(program);
    PhaseTimes times(build, PassID::ApplyFlowResults);
    Vector<BitVector> fieldsExpectedAsRead = findExpectedFields(program, build.parallelLoops());
    nameObjectInPlaceOfDictionaryLayouts(program, *application, build.parallelLoops());
    applyLayoutUpdates(context);
    times.didFinish("applyLayoutUpdates"_s);
    applyVariableUpdates(context);
    applySignatureUpdates(context);
    if (provesEverything(build))
        proveEveryPosition(program, build);
    times.didFinish("applySignatureUpdates"_s);
    createBoundFunctions(context, editor);
    removeUnreachedBodies(context, editor);
    dropDataOfUnreachedCode(context);
    narrowContentsOfData(context);
    times.didFinish("removeUnreachedBodies"_s);
    applyBodyUpdates(context, editor, fieldsExpectedAsRead);
    cutCallsOfUnreachedVersions(context);
    times.didFinish("applyBodyUpdates"_s);
    ProgramCaches caches(program, build);
    computeOperandSummaries(program, results, build);
    times.didFinish("computeOperandSummaries"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
