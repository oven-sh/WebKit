/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutinePlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowCoroutines.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTPassRunner.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct StaleCalls {
    FunctionID caller;
    Vector<ValueID, 4> calls;
};

} // anonymous namespace

static Vector<CoroutinePlan> findCoroutines(const Program& program, const ParallelLoops& loops)
{
    Vector<CoroutinePlan> plans;
    auto coroutines = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        return function.isUnsplitCoroutine() && function.hasBody();
    });
    for (uint32_t index : coroutines) {
        const Function& function = program.functions[index];
        CoroutinePlan plan;
        plan.record.kind = *coroutineKindOf(function);
        plan.record.ramp = FunctionID(index);
        plans.append(WTF::move(plan));
    }
    return plans;
}

static bool maySuspend(const AnalysisContext& context, const Function& function)
{
    FunctionAnalyses analyses(context, function);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        if (analyses.effectsOfTerminator(block).onAnyEdge().maySuspend)
            return true;
        for (ValueID id : function.at(block).values) {
            if (analyses.maySuspendOnAnyEdge(id))
                return true;
        }
    }
    return false;
}

// An async function without an await needs no frame that can be saved: it is an ordinary function whose result goes into a promise.
static void findFunctionsThatNeverSuspend(const Program& program, const OptimizationInputs& inputs, BuildContext& build, Vector<CoroutinePlan>& plans)
{
    forEachIndexInParallel(build.parallelLoops(), plans.size(), [&](unsigned index) {
        CoroutinePlan& plan = plans[index];
        if (plan.record.kind != CoroutineKind::AsyncFunction)
            return;
        FunctionID id = plan.record.ramp;
        plan.neverSuspends = !maySuspend(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, program.at(id));
    });
}

static CoroutineTypes internCoroutineTypes(TypeTable& types)
{
    auto internResultOf = [&](CoroutineKind kind) {
        return types.intern(Type::builtinClass(resultClassOf(kind)));
    };

    CoroutineTypes coroutineTypes;
    coroutineTypes.promise = internResultOf(CoroutineKind::AsyncFunction);
    coroutineTypes.promiseOrUndefined = types.intern(types.at(coroutineTypes.promise).withPrimitives(Primitive::Undefined));
    coroutineTypes.generator = internResultOf(CoroutineKind::Generator);
    coroutineTypes.asyncGenerator = internResultOf(CoroutineKind::AsyncGenerator);
    return coroutineTypes;
}

static void planCoroutines(ProgramEditor& editor, const CoroutineTypes& types, Vector<CoroutinePlan>& plans)
{
    // The list of functions grows here and not again in this pass, so a pointer to a body stays valid.
    uint32_t nextFunction = editor.reserveFunctions(plans.size()).index();

    const Program& program = editor.program();
    for (CoroutinePlan& plan : plans) {
        CreatorRecord::Coroutine& record = plan.record;
        record.body = FunctionID(nextFunction++);
        plan.bodySlot = &editor.at(record.body);
        if (plan.neverSuspends) {
            record.promiseType = types.promise;
            record.worker = record.body;
            continue;
        }

        record.promiseParameter = program.at(record.ramp).numberOfEntryParameters();
        switch (record.kind) {
        case CoroutineKind::Generator:
            break;
        case CoroutineKind::AsyncFunction:
            record.promiseType = types.promise;
            record.entryWithoutPromise = record.body;
            break;
        case CoroutineKind::AsyncGenerator:
            record.promiseType = types.promise;
            break;
        }
    }
}

static void buildFunctions(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build, const CoroutineTypes& types, Vector<CoroutinePlan>& plans)
{
    Vector<FunctionID> ramps = plans.map([](const CoroutinePlan& plan) {
        return plan.record.ramp;
    });
    Vector<FunctionID> bodies = plans.map([](const CoroutinePlan& plan) {
        return plan.record.body;
    });

    const Program& program = editor.program();
    editFunctionsInParallel(editor, build, ramps.span(), bodies.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        CoroutinePlan& plan = plans[index];
        AnalysisContext context { build, program, &inputs.flow, &inputs.effects, id };
        Function ramp = plan.neverSuspends ? buildRampOfWorker(context, arenas.types, types, function, plan) : buildCoroutineRamp(context, arenas.types, types, function, plan);
        context.id = plan.record.body;
        *plan.bodySlot = plan.neverSuspends ? buildAsyncWorker(context, arenas.types, WTF::move(function), plan) : buildCoroutineBody(context, arenas.types, types, WTF::move(function), plan);
        function = WTF::move(ramp);
    });
}

static bool isStaleCall(const Program& program, const BitVector& narrowedRamps, const Value& value)
{
    if (value.opcode != Opcode::CallDirect || !narrowedRamps.get(value.function().index()))
        return false;
    TypeID resultType = program.at(value.function()).result().type;
    return resultType != value.type && program.at(resultType).isSubsetOf(program.at(value.type), program);
}

static Vector<StaleCalls> findStaleCalls(const Program& program, const ParallelLoops& loops, const BitVector& narrowedRamps)
{
    return collectInParallel<StaleCalls>(loops, program.functions.size(), [&](unsigned index, Vector<StaleCalls>& found) {
        const Function& caller = program.functions[index];
        Vector<ValueID, 4> calls;
        for (const Block& block : caller.blocks) {
            for (ValueID id : block.values) {
                if (isStaleCall(program, narrowedRamps, caller.at(id)))
                    calls.append(id);
            }
        }
        if (!calls.isEmpty())
            found.append(StaleCalls { FunctionID(index), WTF::move(calls) });
    });
}

static void retypeCallers(ProgramEditor& editor, BuildContext& build, const Vector<CoroutinePlan>& plans)
{
    BitVector narrowedRamps;
    for (const CoroutinePlan& plan : plans) {
        if (plan.didNarrowResult)
            narrowedRamps.set(plan.record.ramp.index());
    }
    if (narrowedRamps.isEmpty())
        return;

    const Program& program = editor.program();
    Vector<StaleCalls> staleCalls = findStaleCalls(program, build.parallelLoops(), narrowedRamps);
    Vector<FunctionID> callers = staleCalls.map([](const StaleCalls& stale) {
        return stale.caller;
    });
    editFunctionsInParallel(editor, build, callers.span(), [&](unsigned index, FunctionID, Function& caller, TaskArenas& arenas) {
        for (ValueID id : staleCalls[index].calls) {
            Value& call = caller.edit(id);
            call.type = resultTypeOf(program, caller, call, arenas.types);
            call.setExpected(expectedTypeOf(program, caller, call, arenas.types));
        }
    });
}

void splitCoroutines(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    Vector<CoroutinePlan> plans = findCoroutines(editor.program(), runner.build().parallelLoops());
    if (plans.isEmpty())
        return;

    BuildContext& build = runner.build();
    findFunctionsThatNeverSuspend(editor.program(), inputs, build, plans);
    CoroutineTypes types = internCoroutineTypes(editor.types());
    planCoroutines(editor, types, plans);
    buildFunctions(editor, inputs, build, types, plans);
    retypeCallers(editor, build, plans);
    build.statistics().count(StatisticID::CoroutinesSplit, plans.size());
    build.statistics().count(StatisticID::AsyncWorkersMade, static_cast<uint64_t>(std::ranges::count_if(plans, &CoroutinePlan::neverSuspends)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
