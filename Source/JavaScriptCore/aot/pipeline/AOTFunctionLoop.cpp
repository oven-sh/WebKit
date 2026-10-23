/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionLoop.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTFunctionQueue.h"
#include "AOTInterningPhases.h"
#include "AOTPassRunner.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramAnalyses.h"
#include "AOTProgramCaches.h"
#include "AOTLargeVectors.h"
#include "AOTThreads.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static uint64_t largestFirstKey(size_t numberOfValues, unsigned index)
{
    return static_cast<uint64_t>(UINT32_MAX - numberOfValues) << 32 | index;
}

// Select replaces operations by others whose effects it checks against those of the operations they replace.
static Effects effectsOfConversionsIn(const Function& function)
{
    Effects effects = Effects::none();
    for (const Value& value : function.values) {
        if (familyOf(value.opcode) == OperationFamily::Representation)
            effects.merge(declaredBoundOf(value.opcode));
        else if (function.operandsAreConvertedAtFetch && (value.representation == Representation::Cell32 || value.representation == Representation::LiteralString))
            effects.merge(declaredBoundOf(Opcode::Decode));
    }
    return effects;
}

// All that remapIDs() does to a body without provisional ids.
static void dropExpectedTypesEqualToTypes(Function& function)
{
    for (Value& value : function.editAllValues()) {
        if (value.expected() == value.type)
            value.setExpected({ });
    }
}

static bool holdsExpectedTypeEqualToType(const Function& function)
{
    return std::ranges::any_of(function.values, [](const Value& value) {
        return value.expected() && value.expected() == value.type;
    });
}

namespace {

// Functions that go through the steps of a loop together: small enough to stay in the cache from one step to the next, and many enough for the code of a step to stay there too.
struct Batch {
    uint32_t first { 0 };
    uint32_t size { 0 };
};

enum class StateBehindSchedule : uint8_t { IsFinal, AwaitsEndOfLoop };

} // anonymous namespace

static Vector<Batch> createBatches(std::span<const uint64_t> largestFirst, bool hasSeveralSteps)
{
    constexpr uint32_t maxFunctionsInBatch = 64;
    constexpr uint64_t maxValuesInBatch = 4096;
    Vector<Batch> batches;
    if (!hasSeveralSteps) {
        batches = Vector<Batch>(largestFirst.size(), [](size_t position) {
            return Batch { static_cast<uint32_t>(position), 1 };
        });
        return batches;
    }
    uint64_t valuesInBatch = 0;
    for (uint32_t position = 0; position < largestFirst.size(); ++position) {
        uint64_t values = UINT32_MAX - (largestFirst[position] >> 32);
        if (batches.isEmpty() || batches.last().size == maxFunctionsInBatch || valuesInBatch + values > maxValuesInBatch) {
            batches.append(Batch { position, 0 });
            valuesInBatch = 0;
        }
        ++batches.last().size;
        valuesInBatch += values;
    }
    return batches;
}

static void runInPlace(FunctionSchedule schedule, Program& program, const OptimizationInputs& inputs, const PassRunner& runner, ProgramAnalyses& analyses, const StepsAroundSchedule& steps)
{
    const bool reader = !steps.behind.empty();
    std::optional<PhaseTimes> times;
    if (schedule == FunctionSchedule::Select || schedule == FunctionSchedule::Represent)
        times.emplace(runner.build(), schedule == FunctionSchedule::Select ? PassID::Select : PassID::ChooseRepresentations);
    auto didFinish = [&](ASCIILiteral step) {
        if (times)
            times->didFinish(step);
    };
    const ParallelLoops& loops = runner.build().parallelLoops();
    unsigned count = program.functions.size();
    Vector<uint64_t> order = collectInParallel<uint64_t>(loops, count, [&](unsigned index, Vector<uint64_t>& keys) {
        if (hasBody(program, FunctionID(index)))
            keys.append(largestFirstKey(numberOfValuesIn(program, FunctionID(index)), index));
    });
    parallelSort(loops, order.mutableSpan(), std::less<uint64_t> { });
    Vector<Batch> batches = createBatches(order.span(), reader);
    Vector<uint8_t> didChange(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasImpureConversions(FillWith { }, schedule == FunctionSchedule::Represent ? count : 0, static_cast<uint8_t>(0));
    // Of a function that has changed. The task of a function has the number of the function. A task notes every provisional id that it uses: where it has noted none, the body holds none.
    Vector<StateBehindSchedule> stateBehindSchedule(FillWith { }, reader ? count : 0, StateBehindSchedule::IsFinal);
    auto phases = makeUnique<InterningPhases>(program, count);
    BodyIndex& bodyIndex = analyses.bodyIndex();
    bodyIndex.growTo(count);
    didFinish("order functions, open phases"_s);
    {
        ProgramCaches caches(program, runner.build());
        auto runBatch = [&](unsigned batchIndex) {
            std::span<const uint64_t> functionsInBatch = order.subspan(batches[batchIndex].first, batches[batchIndex].size);
            for (uint64_t key : functionsInBatch) {
                unsigned i = static_cast<unsigned>(key);
                if (steps.inFront)
                    (*steps.inFront)(FunctionID(i), program.functions[i]);
                TaskArenas arenas(*phases, i);
                didChange[i] = runner.runSchedule(schedule, program, inputs, FunctionID(i), program.functions[i], arenas) == PassOutcome::Changed;
                if (didChange[i]) {
                    if (schedule == FunctionSchedule::Represent)
                        hasImpureConversions[i] = !effectsOfConversionsIn(program.functions[i]).isPure();
                    shrinkToFit(program.functions[i]);
                    bodyIndex.didGiveOut(FunctionID(i));
                    bodyIndex.write(FunctionID(i), program.functions[i]);
                    bodyIndex.didEndEdits(FunctionID(i));
                }
                if (runner.build().options().verifiesAfterEveryPass) [[unlikely]]
                    RELEASE_ASSERT(!numberOfForeignIDs(program.functions[i], arenas.types));
                if (reader && didChange[i] && (arenas.types.hasUsedProvisionalRecords() || arenas.strings.hasUsedProvisionalStrings() || holdsExpectedTypeEqualToType(program.functions[i])))
                    stateBehindSchedule[i] = StateBehindSchedule::AwaitsEndOfLoop;
            }
            for (const ReaderOfScheduledFunction* read : steps.behind) {
                for (uint64_t key : functionsInBatch) {
                    unsigned i = static_cast<unsigned>(key);
                    if (stateBehindSchedule[i] == StateBehindSchedule::IsFinal)
                        (*read)(FunctionID(i));
                }
            }
        };
        if (steps.beside) {
            runTasksInParallel(loops, {
                [&] { forEachIndexInParallel(loops, batches.size(), runBatch); },
                [&] { (*steps.beside)(); },
            });
        } else
            forEachIndexInParallel(loops, batches.size(), runBatch);
    }
    didFinish("functions"_s);
    BodyRemaps remaps = phases->finish(&loops);
    didFinish("merge interned records"_s);
    runner.build().destroyLater(WTF::move(phases));
    Vector<FunctionID> changed;
    for (unsigned i = 0; i < count; ++i) {
        if (didChange[i])
            changed.append(FunctionID(i));
    }
    // The largest first, as in the loop above: the readers of a large function last long.
    Vector<FunctionID> changedAndNotFinal;
    for (uint64_t key : order) {
        unsigned i = static_cast<unsigned>(key);
        if (didChange[i] && (!reader || stateBehindSchedule[i] == StateBehindSchedule::AwaitsEndOfLoop))
            changedAndNotFinal.append(FunctionID(i));
    }
    if (!remaps.isIdentity()) {
        forEachIndexInParallel(loops, changedAndNotFinal.size(), [&](unsigned position) {
            Function& function = program.at(changedAndNotFinal[position]);
            unsigned task = changedAndNotFinal[position].index();
            if (remaps.types.isIdentityFor(task) && remaps.strings.isIdentityFor(task))
                dropExpectedTypesEqualToTypes(function);
            else
                applyRemaps(remaps, function);
        });
    }
    if (reader && !changedAndNotFinal.isEmpty()) {
        TypeInterningPhase scratch(program.types, 0u, TypeInterningPhase::Scratch);
        {
            ProgramCaches caches(program, runner.build());
            forEachIndexInParallel(loops, changedAndNotFinal.size(), [&](unsigned position) {
                for (const ReaderOfScheduledFunction* read : steps.behind)
                    (*read)(changedAndNotFinal[position]);
            });
        }
        scratch.discard();
    }
    didFinish("functions that held provisional ids"_s);
    if (changed.isEmpty())
        return;
    switch (schedule) {
    case FunctionSchedule::Select:
        analyses.didLower(changed.span(), { });
        didFinish("tell analyses"_s);
        return;
    case FunctionSchedule::Represent: {
        Vector<AddedEffects> addedEffects = collectInParallel<AddedEffects>(loops, changed.size(), [&](unsigned position, Vector<AddedEffects>& found) {
            if (!hasImpureConversions[changed[position].index()])
                return;
            Effects effects = effectsOfConversionsIn(program.at(changed[position]));
            if (!effects.isPure())
                found.append(AddedEffects { changed[position], effects });
        });
        analyses.didLower(changed.span(), addedEffects.span());
        didFinish("tell analyses"_s);
        return;
    }
    case FunctionSchedule::Type:
    case FunctionSchedule::Simplify:
    case FunctionSchedule::Prepare:
    case FunctionSchedule::Resolve:
    case FunctionSchedule::Optimize:
        analyses.invalidate(resultsAboutBodies, changed.span());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

// What is taken from a copy while it is in the cache, so that the commit reads as little of it as it can.
struct FinishedCopy {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FinishedCopy);

    explicit FinishedCopy(const Function& copy)
        : remapSites(findRemapSites(copy))
        , row(copy, BodyIDs::Final)
    {
    }

    RemapSites remapSites;
    BodyIndex::Row row;
};

struct CopyLoop {
    const ScheduleDescriptor& schedule;
    Program& program;
    const OptimizationInputs& inputs;
    const PassRunner& runner;
    InterningPhases& phases;
    DerivedFunctionPhase& derived;
    FunctionQueue& queue;
    Vector<std::unique_ptr<Function>> optimized;
    // Of a copy that has changed.
    Vector<std::unique_ptr<FinishedCopy>> finished;
    Vector<uint8_t> didChange;
    Vector<QueuedFunction> startedEarly;
};

using FoundFunctions = FunctionQueue::FoundFunctions;

} // anonymous namespace

static QueuedFunction createQueuedFunction(const Program& program, FunctionID id)
{
    if (ProvisionalFunctions::contains(id))
        return { id, 0 };
    return { id, static_cast<uint32_t>(numberOfValuesIn(program, id)) };
}

static Vector<QueuedFunction> collectScheduledFunctions(const CopyLoop& loop)
{
    const Program& program = loop.program;
    Vector<QueuedFunction> functions;
    switch (loop.schedule.functions) {
    case ScheduledFunctions::All:
        functions = collectInParallel<QueuedFunction>(loop.runner.build().parallelLoops(), program.functions.size(), [&](unsigned index, Vector<QueuedFunction>& functionsWithBody) {
            if (hasBody(program, FunctionID(index)))
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

// A function is found only when one that names it is finished, so long functions that name each other would run one after the other.
static Vector<QueuedFunction> collectFunctionsToStartEarly(const ScheduleDescriptor& schedule, const Program& program, const ParallelLoops& loops)
{
    if (schedule.functions != ScheduledFunctions::Reached)
        return { };
    return largeFunctions(program, loops).map([&](uint32_t index) {
        return createQueuedFunction(program, FunctionID(index));
    });
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
    // Asked about its instances, a bound function asks its target. Where it captures no closure of the target, the run time makes one, which the tables name by its entrypoint.
    if (role == FunctionRole::BoundFunction) {
        FunctionID boundTarget = ProvisionalFunctions::contains(target) ? derivedFunctions.keyOf(target).target : program.at(target).derivation.source;
        found.append(createQueuedFunction(program, derivedFunctions.ensure(derivedFunctionKey(FunctionRole::GenericEntrypoint, boundTarget))));
    }
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
    if (source.valuesAreTypedAsDefined())
        copy->setValuesAreTypedAsDefined();
    TaskArenas arenas(loop.phases, isProvisional ? loop.derived.taskOf(id) : id.index());
    if (isProvisional)
        adoptIDs(*copy, arenas.types);
    DerivedFunctionRequester requester(loop.derived, id, loop.queue.wasConfirmed(id));
    PassOutcome outcome = loop.runner.runSchedule(loop.schedule.id, program, loop.inputs, id, *copy, arenas);
    if (loop.runner.build().options().verifiesAfterEveryPass) [[unlikely]]
        RELEASE_ASSERT(!numberOfForeignIDs(*copy, arenas.types));
    shrinkToFit(*copy);
    // An All schedule has already queued every function with a body.
    if (loop.schedule.functions == ScheduledFunctions::Reached) {
        forEachFunctionReference(program, *copy, [&](FunctionID target, FunctionReferenceKind kind) {
            reach(loop, target, kind, found);
        });
    }
    if (isProvisional)
        loop.derived.setOptimizedBody(id, WTF::move(copy));
    else {
        loop.didChange[id.index()] = outcome == PassOutcome::Changed;
        if (outcome == PassOutcome::Changed)
            loop.finished[id.index()] = makeUnique<FinishedCopy>(*copy);
        loop.optimized[id.index()] = WTF::move(copy);
    }
}

static void commitCopies(CopyLoop& loop, ProgramAnalyses& analyses)
{
    Program& program = loop.program;
    BuildContext& build = loop.runner.build();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = loop.optimized.size();
    bool removesUnreachedBodies = loop.schedule.id == FunctionSchedule::Optimize;

    DerivedFunctionCommit commit = loop.derived.commit(loop.queue.takeQueuedProvisionalOrdinals());
    Vector<unsigned> taskOrder;
    taskOrder.reserveInitialCapacity(count + commit.tasks.size());
    for (unsigned task = 0; task < count; ++task) {
        if (loop.queue.wasConfirmed(FunctionID(task)))
            taskOrder.append(task);
    }
    taskOrder.appendVector(commit.tasks);
    BodyRemaps remaps = loop.phases.finish(taskOrder.span(), WTF::move(commit.remap), &build.parallelLoops());

    // The list of functions moves when it grows. All threads take the page faults of where it moves to, and there is room for what the passes behind the loop add.
    if (size_t numberOfFunctions = count + commit.functions.size(); numberOfFunctions > program.functions.capacity())
        reserveCapacityInParallel(loops, program.functions, numberOfFunctions + numberOfFunctions / 4);
    ProgramEditor editor(program);
    FunctionID firstAdded = editor.reserveFunctions(commit.functions.size());
    RELEASE_ASSERT(firstAdded == commit.firstID && firstAdded.index() == count);

    BodyIndex& bodyIndex = analyses.bodyIndex();
    std::atomic<uint64_t> numberOfRemovedBodies { 0 };
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        FunctionID id(index);
        if (loop.queue.wasConfirmed(id)) {
            if (auto& copy = loop.optimized[index]; copy && loop.didChange[index]) {
                FinishedCopy& finished = *loop.finished[index];
                if (!finished.remapSites.isEmpty()) {
                    applyRemaps(remaps, *copy, finished.remapSites);
                    finished.row.applyRemaps(remaps.types, remaps.functions, finished.remapSites.record ? copy.get() : nullptr);
                }
                std::swap(editor.at(id), *copy);
                bodyIndex.write(id, WTF::move(finished.row));
            } else if (copy && copy->valuesAreTypedAsDefined())
                program.functions[index].setValuesAreTypedAsDefined();
            return;
        }
        if (!removesUnreachedBodies || !program.functions[index].hasBody())
            return;
        editor.removeBody(id);
        numberOfRemovedBodies.fetch_add(1, std::memory_order_relaxed);
    });
    forEachIndexInParallel(loops, commit.functions.size(), [&](unsigned position) {
        Function& function = program.functions[count + position];
        function = WTF::move(*commit.functions[position]);
        applyRemaps(remaps, function);
        bodyIndex.write(FunctionID(count + position), function, BodyIDs::Final);
    });

    if (removesUnreachedBodies) {
        build.statistics().count(StatisticID::UnreachableFunctionsRemoved, numberOfRemovedBodies.load(std::memory_order_relaxed));
        build.statistics().count(StatisticID::FunctionsStartedEarly, loop.startedEarly.size());
        build.statistics().count(StatisticID::FunctionsStartedEarlyButNotReached, static_cast<uint64_t>(std::ranges::count_if(loop.startedEarly, [&](QueuedFunction function) {
            return !loop.queue.wasConfirmed(function.id);
        })));
        loop.inputs.derivedFunctions->forgetFunctionsWithoutBody(program);
    }
    analyses.invalidate(resultsAboutBodies, editor);
    build.resolveFunctionToDump(program);
    build.destroyLater(WTF::move(loop.optimized));
    build.destroyLater(WTF::move(loop.finished));
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
        CopyLoop loop { schedule, program, inputs, runner, *phases, derived, queue, Vector<std::unique_ptr<Function>>(count), Vector<std::unique_ptr<FinishedCopy>>(count), Vector<uint8_t>(FillWith { }, count, static_cast<uint8_t>(0)), collectFunctionsToStartEarly(schedule, program, loops) };

        {
            ProgramCaches caches(program, build);
            queue.processEach(collectScheduledFunctions(loop).span(), loop.startedEarly.span(), [&](FunctionID id, FoundFunctions& found) {
                optimizeCopy(loop, id, found);
            }, [&](FunctionID id) {
                derived.confirmRequestsOf(id);
            });
        }
        if (build.diagnostics().hasErrors())
            phases->discard();
        else
            commitCopies(loop, analyses);
    }
    build.destroyLater(WTF::move(phases));
}

void runOnFunctions(FunctionSchedule schedule, Program& program, const OptimizationInputs& inputs, const PassRunner& runner, ProgramAnalyses& analyses, const StepsAroundSchedule& steps)
{
    const ScheduleDescriptor& descriptor = descriptorOf(schedule);
    RELEASE_ASSERT(descriptor.functions != ScheduledFunctions::OneAtATime);
    switch (descriptor.mode) {
    case ScheduleMode::InPlace:
        runInPlace(schedule, program, inputs, runner, analyses, steps);
        return;
    case ScheduleMode::OnCopies:
        RELEASE_ASSERT(!steps.inFront && steps.behind.empty() && !steps.beside);
        runOnCopies(descriptor, program, inputs, runner, analyses);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
