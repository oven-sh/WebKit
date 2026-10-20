/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionLoop.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTDerivedFunctions.h"
#include "AOTFunctionQueue.h"
#include "AOTInterningPhases.h"
#include "AOTPassRunner.h"
#include "AOTProgramAnalyses.h"
#include "AOTThreads.h"
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static uint64_t largestFirstKey(const Function& function, unsigned index)
{
    return static_cast<uint64_t>(UINT32_MAX - function.values.size()) << 32 | index;
}

static void runInPlace(FunctionSchedule schedule, Program& program, const OptimizationInputs& inputs, const PassRunner& runner, ProgramAnalyses& analyses)
{
    const ParallelLoops& loops = runner.build().parallelLoops();
    unsigned count = program.functions.size();
    Vector<uint64_t> order = collectInParallel<uint64_t>(loops, count, [&](unsigned index, Vector<uint64_t>& keys) {
        if (program.functions[index].hasBody())
            keys.append(largestFirstKey(program.functions[index], index));
    });
    parallelSort(loops, order.mutableSpan(), std::less<uint64_t> { });
    Vector<uint8_t> didChange(FillWith { }, count, static_cast<uint8_t>(0));
    auto phases = makeUnique<InterningPhases>(program, count);
    forEachIndexInParallel(loops, order.size(), [&](unsigned position) {
        unsigned i = static_cast<unsigned>(order[position]);
        TaskArenas arenas(*phases, i);
        didChange[i] = runner.runSchedule(schedule, program, inputs, FunctionID(i), program.functions[i], arenas) == PassOutcome::Changed;
    });
    BodyRemaps remaps = phases->finish();
    runner.build().destroyLater(WTF::move(phases));
    Vector<FunctionID> changed;
    for (unsigned i = 0; i < count; ++i) {
        if (didChange[i])
            changed.append(FunctionID(i));
    }
    if (!remaps.isIdentity()) {
        forEachIndexInParallel(loops, changed.size(), [&](unsigned position) {
            applyRemaps(remaps, program.at(changed[position]));
        });
    }
    if (!changed.isEmpty())
        analyses.invalidate(resultsAboutBodies);
}

namespace {

struct CopyLoop {
    const ScheduleDescriptor& schedule;
    Program& program;
    const OptimizationInputs& inputs;
    const PassRunner& runner;
    InterningPhases& phases;
    DerivedFunctionPhase& derived;
    FunctionQueue& queue;
    Vector<std::unique_ptr<Function>> optimized;
};

using FoundFunctions = FunctionQueue::FoundFunctions;

} // anonymous namespace

static QueuedFunction createQueuedFunction(const Program& program, FunctionID id)
{
    if (ProvisionalFunctions::contains(id))
        return { id, 0 };
    return { id, static_cast<uint32_t>(program.at(id).values.size()) };
}

static Vector<QueuedFunction> collectScheduledFunctions(const CopyLoop& loop)
{
    const Program& program = loop.program;
    Vector<QueuedFunction> functions;
    switch (loop.schedule.functions) {
    case ScheduledFunctions::All:
        functions = collectInParallel<QueuedFunction>(loop.runner.build().parallelLoops(), program.functions.size(), [&](unsigned index, Vector<QueuedFunction>& functionsWithBody) {
            if (program.functions[index].hasBody())
                functionsWithBody.append(createQueuedFunction(program, FunctionID(index)));
        });
        break;
    case ScheduledFunctions::Reached:
        functions.reserveInitialCapacity(loop.inputs.callGraph.roots().size());
        for (FunctionID root : loop.inputs.callGraph.roots())
            functions.append(createQueuedFunction(program, root));
        break;
    case ScheduledFunctions::OneAtATime:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return functions;
}

static void reach(CopyLoop& loop, FunctionID target, FunctionReferenceKind kind, FoundFunctions& found)
{
    const Program& program = loop.program;
    found.append(createQueuedFunction(program, target));
    switch (kind) {
    case FunctionReferenceKind::Call:
    case FunctionReferenceKind::Dispatch:
        return;
    case FunctionReferenceKind::Closure:
    case FunctionReferenceKind::Constructor:
        break;
    }
    if (loop.schedule.functions != ScheduledFunctions::Reached)
        return;
    DerivedFunctions& derivedFunctions = *loop.inputs.derivedFunctions;
    FunctionRole role = ProvisionalFunctions::contains(target) ? derivedFunctions.keyOf(target).role : program.at(target).role;
    if (!canBeCalledByValueOf(role))
        return;
    found.append(createQueuedFunction(program, derivedFunctions.ensure(derivedFunctionKey(FunctionRole::GenericEntrypoint, target))));
}

static void optimizeCopy(CopyLoop& loop, FunctionID id, FoundFunctions& found)
{
    // Only the const overload of Program::at answers for a provisional id.
    const Program& program = loop.program;
    const Function& source = program.at(id);
    if (!source.hasBody())
        return;
    bool isProvisional = ProvisionalFunctions::contains(id);
    auto copy = makeUniqueWithoutFastMallocCheck<Function>(source);
    TaskArenas arenas(loop.phases, isProvisional ? loop.derived.taskOf(id) : id.index());
    loop.runner.runSchedule(loop.schedule.id, program, loop.inputs, id, *copy, arenas);
    copy->values.shrinkToFit();
    copy->blocks.shrinkToFit();
    forEachFunctionReference(program, *copy, [&](FunctionID target, FunctionReferenceKind kind) {
        reach(loop, target, kind, found);
    });
    if (isProvisional)
        loop.derived.setOptimizedBody(id, WTF::move(copy));
    else
        loop.optimized[id.index()] = WTF::move(copy);
}

static void commitCopies(CopyLoop& loop, ProgramAnalyses& analyses)
{
    Program& program = loop.program;
    BuildContext& build = loop.runner.build();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = loop.optimized.size();
    bool removesUnreachedBodies = loop.schedule.functions == ScheduledFunctions::Reached;

    DerivedFunctionCommit commit = loop.derived.commit(loop.queue.takeQueuedProvisionalOrdinals());
    Vector<unsigned> taskOrder;
    taskOrder.reserveInitialCapacity(count + commit.tasks.size());
    for (unsigned task = 0; task < count; ++task)
        taskOrder.append(task);
    taskOrder.appendVector(commit.tasks);
    BodyRemaps remaps = loop.phases.finish(taskOrder.span(), WTF::move(commit.remap));

    ProgramEditor editor(program);
    FunctionID firstAdded = editor.reserveFunctions(commit.functions.size());
    RELEASE_ASSERT(firstAdded == commit.firstID && firstAdded.index() == count);

    std::atomic<uint64_t> numberOfRemovedBodies { 0 };
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        if (auto& copy = loop.optimized[index]) {
            applyRemaps(remaps, *copy);
            std::swap(program.functions[index], *copy);
            return;
        }
        FunctionID id(index);
        if (!removesUnreachedBodies || !program.functions[index].hasBody() || loop.queue.wasQueued(id))
            return;
        editor.removeBody(id);
        numberOfRemovedBodies.fetch_add(1, std::memory_order_relaxed);
    });
    forEachIndexInParallel(loops, commit.functions.size(), [&](unsigned position) {
        Function& function = program.functions[count + position];
        function = WTF::move(*commit.functions[position]);
        applyRemaps(remaps, function);
    });

    if (removesUnreachedBodies) {
        build.statistics().count(StatisticID::UnreachableFunctionsRemoved, numberOfRemovedBodies.load(std::memory_order_relaxed));
        loop.inputs.derivedFunctions->forgetFunctionsWithoutBody(program);
    }
    analyses.invalidate(resultsAboutBodies);
    build.resolveFunctionToDump(program);
    build.destroyLater(WTF::move(loop.optimized));
    build.destroyLater(WTF::move(commit.functions));
}

static void runOnCopies(const ScheduleDescriptor& schedule, Program& program, const OptimizationInputs& inputs, const PassRunner& runner, ProgramAnalyses& analyses)
{
    RELEASE_ASSERT(inputs.derivedFunctions);
    BuildContext& build = runner.build();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    auto phases = makeUnique<InterningPhases>(program, count);
    {
        DerivedFunctionPhase derived(*inputs.derivedFunctions, program, inputs.flow, *phases);
        FunctionQueue queue(loops, count);
        CopyLoop loop { schedule, program, inputs, runner, *phases, derived, queue, Vector<std::unique_ptr<Function>>(count) };

        queue.processEach(collectScheduledFunctions(loop).span(), [&](FunctionID id, FoundFunctions& found) {
            optimizeCopy(loop, id, found);
        });
        if (build.diagnostics().hasErrors())
            phases->discard();
        else
            commitCopies(loop, analyses);
    }
    build.destroyLater(WTF::move(phases));
}

void runOnFunctions(FunctionSchedule schedule, Program& program, const OptimizationInputs& inputs, const PassRunner& runner, ProgramAnalyses& analyses)
{
    const ScheduleDescriptor& descriptor = descriptorOf(schedule);
    RELEASE_ASSERT(descriptor.functions != ScheduledFunctions::OneAtATime);
    switch (descriptor.mode) {
    case ScheduleMode::InPlace:
        runInPlace(schedule, program, inputs, runner, analyses);
        return;
    case ScheduleMode::OnCopies:
        runOnCopies(descriptor, program, inputs, runner, analyses);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
