/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutinePlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowCoroutines.h"
#include "AOTFunctionEditing.h"
#include "AOTPassRunner.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct StaleCalls {
    FunctionID caller;
    Vector<ValueID, 4> calls;
};

} // anonymous namespace

static Vector<CoroutinePlan> findCoroutines(const Program& program)
{
    Vector<CoroutinePlan> plans;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.isUnsplitCoroutine() || !function.hasBody())
            continue;
        CoroutinePlan plan;
        plan.record.kind = *coroutineKindOf(function);
        plan.record.ramp = FunctionID(index);
        plans.append(WTF::move(plan));
    }
    return plans;
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
        plan.bodySlot = &editor.at(record.body);
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
        Function ramp = buildCoroutineRamp(context, arenas.types, types, function, plan);
        context.id = plan.record.body;
        *plan.bodySlot = buildCoroutineBody(context, arenas.types, types, WTF::move(function), plan);
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
            Value& call = caller.at(id);
            call.type = resultTypeOf(program, caller, call, arenas.types);
            call.setWritten(writtenTypeOf(program, caller, call, arenas.types));
            call.setPrediction(predictionOf(program, caller, call, arenas.types));
        }
    });
}

void splitCoroutines(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    Vector<CoroutinePlan> plans = findCoroutines(editor.program());
    if (plans.isEmpty())
        return;

    BuildContext& build = runner.build();
    CoroutineTypes types = internCoroutineTypes(editor.types());
    planCoroutines(editor, types, plans);
    buildFunctions(editor, inputs, build, types, plans);
    retypeCallers(editor, build, plans);
    build.statistics().count(StatisticID::CoroutinesSplit, plans.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
