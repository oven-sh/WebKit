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
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <wtf/ParkingLot.h>

namespace JSC { namespace AOT { namespace Typed {

// A thread takes the last of its functions: the largest, which take longest, begin first.
static bool isProcessedLater(const QueuedFunction& a, const QueuedFunction& b)
{
    if (a.size != b.size)
        return a.size < b.size;
    return a.id.index() > b.id.index();
}

enum class Half : uint8_t { WithLargest, WithoutLargest };

// Every other one, so that both halves begin with large ones.
static Vector<QueuedFunction> takeHalf(Vector<QueuedFunction>& functions, Half half)
{
    Vector<QueuedFunction> taken;
    taken.reserveInitialCapacity((functions.size() + 1) / 2);
    size_t size = functions.size();
    size_t numberKept = 0;
    for (size_t index = 0; index < size; ++index) {
        bool isInHalfWithLargest = !((size - 1 - index) % 2);
        if (isInHalfWithLargest == (half == Half::WithLargest))
            taken.append(functions[index]);
        else
            functions[numberKept++] = functions[index];
    }
    functions.shrink(numberKept);
    return taken;
}

FunctionQueue::FunctionQueue(const ParallelLoops& loops, unsigned numberOfFunctions)
    : m_loops(loops)
    , m_workers(makeUniqueArray<Worker>(loops.width()))
    , m_states(numberOfFunctions)
{
    for (unsigned index = 0; index < loops.width(); ++index)
        m_workers[index].queue = this;
    for (std::atomic<uint8_t>& state : m_states)
        state.store(0, std::memory_order_relaxed);
}

FunctionQueue::~FunctionQueue()
{
    for (auto& segment : m_isProvisionalQueued)
        delete segment.load(std::memory_order_relaxed);
}

auto FunctionQueue::speculativeFunction(FunctionID id) -> SpeculativeFunction&
{
    auto function = std::ranges::lower_bound(m_speculativeFunctions, id, { }, &SpeculativeFunction::id);
    RELEASE_ASSERT(function != m_speculativeFunctions.end() && function->id == id);
    return *function;
}

// Only a confirmed function queues a provisional function, so one bit is enough for it.
auto FunctionQueue::confirm(FunctionID id) -> Confirmation
{
    if (ProvisionalFunctions::contains(id)) {
        uint32_t wordIndex = ProvisionalFunctions::ordinalOf(id) / bitsPerWord;
        RELEASE_ASSERT(wordIndex / wordsPerSegment < numberOfSegments);
        std::atomic<Segment*>& slot = m_isProvisionalQueued[wordIndex / wordsPerSegment];
        Segment* segment = slot.load(std::memory_order_acquire);
        if (!segment) {
            auto created = makeUniqueWithoutFastMallocCheck<Segment>();
            if (slot.compare_exchange_strong(segment, created.get(), std::memory_order_release, std::memory_order_acquire))
                segment = created.release();
        }
        Word& word = (*segment)[wordIndex % wordsPerSegment];
        uint64_t bit = 1ull << (id.index() % bitsPerWord);
        if (word.load(std::memory_order_relaxed) & bit || word.fetch_or(bit, std::memory_order_relaxed) & bit)
            return Confirmation::AlreadyQueued;
        return Confirmation::FirstQueued;
    }

    constexpr uint8_t queuedAndConfirmed = Queued | Confirmed;
    std::atomic<uint8_t>& state = m_states[id.index()];
    if ((state.load(std::memory_order_relaxed) & queuedAndConfirmed) == queuedAndConfirmed)
        return Confirmation::AlreadyQueued;
    uint8_t previous = state.fetch_or(queuedAndConfirmed, std::memory_order_acq_rel);
    if (!(previous & Queued))
        return Confirmation::FirstQueued;
    if (previous & Confirmed || !(previous & Done))
        return Confirmation::AlreadyQueued;
    return Confirmation::FinishedSpeculatively;
}

void FunctionQueue::confirmFoundFunctions(FoundFunctions& found)
{
    size_t numberToProcess = 0;
    for (size_t index = 0; index < found.size(); ++index) {
        QueuedFunction function = found[index];
        switch (confirm(function.id)) {
        case Confirmation::AlreadyQueued:
            break;
        case Confirmation::FirstQueued:
            found[numberToProcess++] = function;
            break;
        case Confirmation::FinishedSpeculatively: {
            (*m_didConfirm)(function.id);
            Vector<QueuedFunction> foundSpeculatively = std::exchange(speculativeFunction(function.id).found, { });
            found.append(foundSpeculatively.span());
            break;
        }
        }
    }
    found.shrink(numberToProcess);
}

// Whichever thread sets the second of Confirmed and Done queues the functions that the speculative function found.
bool FunctionQueue::finishSpeculativeFunction(FunctionID id, const FoundFunctions& found)
{
    std::atomic<uint8_t>& state = m_states[id.index()];
    if (!(state.load(std::memory_order_relaxed) & Confirmed)) {
        SpeculativeFunction& function = speculativeFunction(id);
        function.found = Vector<QueuedFunction>(found.span());
        if (!(state.fetch_or(Done, std::memory_order_acq_rel) & Confirmed))
            return false;
        function.found.clear();
    }
    (*m_didConfirm)(id);
    return true;
}

bool FunctionQueue::wasQueued(FunctionID id) const
{
    if (!ProvisionalFunctions::contains(id))
        return m_states[id.index()].load(std::memory_order_relaxed) & Queued;
    uint32_t word = ProvisionalFunctions::ordinalOf(id) / bitsPerWord;
    if (word / wordsPerSegment >= numberOfSegments)
        return false;
    const Segment* segment = m_isProvisionalQueued[word / wordsPerSegment].load(std::memory_order_acquire);
    return segment && (*segment)[word % wordsPerSegment].load(std::memory_order_relaxed) & 1ull << (id.index() % bitsPerWord);
}

bool FunctionQueue::wasConfirmed(FunctionID id) const
{
    if (!ProvisionalFunctions::contains(id))
        return m_states[id.index()].load(std::memory_order_relaxed) & Confirmed;
    return wasQueued(id);
}

BitVector FunctionQueue::takeQueuedProvisionalOrdinals()
{
    BitVector ordinals;
    for (unsigned segmentIndex = 0; segmentIndex < numberOfSegments; ++segmentIndex) {
        const Segment* segment = m_isProvisionalQueued[segmentIndex].load(std::memory_order_acquire);
        if (!segment)
            continue;
        for (unsigned wordIndex = 0; wordIndex < wordsPerSegment; ++wordIndex) {
            uint64_t firstOrdinal = (static_cast<uint64_t>(segmentIndex) * wordsPerSegment + wordIndex) * bitsPerWord;
            for (uint64_t bits = (*segment)[wordIndex].load(std::memory_order_relaxed); bits; bits &= bits - 1)
                ordinals.set(firstOrdinal + WTF::ctz(bits));
        }
    }
    return ordinals;
}

bool FunctionQueue::takeSpareThread()
{
    unsigned spare = m_numberOfSpareThreads.load(std::memory_order_relaxed);
    while (spare) {
        if (m_numberOfSpareThreads.compare_exchange_weak(spare, spare - 1, std::memory_order_acquire, std::memory_order_relaxed))
            return true;
    }
    return false;
}

// Whoever has taken a spare thread finds a worker: a worker is idle before its thread counts as spare.
auto FunctionQueue::takeIdleWorker() -> Worker&
{
    for (;;) {
        for (unsigned index = 0; index < m_loops.width(); ++index) {
            Worker& worker = m_workers[index];
            if (!worker.isBusy.load(std::memory_order_relaxed) && !worker.isBusy.exchange(true, std::memory_order_acquire))
                return worker;
        }
    }
}

void FunctionQueue::start(Vector<QueuedFunction>&& functions)
{
    Worker& worker = takeIdleWorker();
    worker.setFunctions(WTF::move(functions));
    m_numberOfBusyWorkers.exchangeAdd(1);
    m_loops.schedule([](void* argument) {
        Worker& worker = *static_cast<Worker*>(argument);
        worker.queue->processShare(worker);
    }, &worker);
}

// A list whose owner is at it is passed over: the owner hands over half itself as soon as it sees a spare thread.
bool FunctionQueue::takeHalfOfAnother(Worker& own)
{
    unsigned numberOfWorkers = m_loops.width();
    unsigned ownIndex = static_cast<unsigned>(&own - m_workers.get());
    for (unsigned distance = 1; distance < numberOfWorkers; ++distance) {
        Worker& other = m_workers[(ownIndex + distance) % numberOfWorkers];
        if (!other.numberOfFunctions.load(std::memory_order_relaxed) || !other.lock.tryLock())
            continue;
        Vector<QueuedFunction> taken;
        {
            Locker locker { AdoptLock, other.lock };
            taken = takeHalf(other.functions, Half::WithLargest);
            other.didChangeFunctions();
        }
        if (taken.isEmpty())
            continue;
        own.setFunctions(WTF::move(taken));
        return true;
    }
    return false;
}

void FunctionQueue::processShare(Worker& own)
{
    FoundFunctions found;
    Vector<Vector<QueuedFunction>, 4> handedOver;
    for (;;) {
        std::optional<QueuedFunction> next;
        {
            Locker locker { own.lock };
            while (own.functions.size() > 1 && takeSpareThread())
                handedOver.append(takeHalf(own.functions, Half::WithoutLargest));
            if (!own.functions.isEmpty())
                next = own.functions.takeLast();
            own.didChangeFunctions();
        }
        for (Vector<QueuedFunction>& functions : handedOver)
            start(WTF::move(functions));
        handedOver.shrink(0);
        if (!next) {
            if (takeHalfOfAnother(own))
                continue;
            break;
        }

        (*m_process)(next->id, found);
        bool isSpeculative = !ProvisionalFunctions::contains(next->id) && m_states[next->id.index()].load(std::memory_order_relaxed) & Speculative;
        if (isSpeculative && !finishSpeculativeFunction(next->id, found)) {
            found.shrink(0);
            continue;
        }

        confirmFoundFunctions(found);
        AOT::sortInLine(found, isProcessedLater);
        if (!found.isEmpty()) {
            Locker locker { own.lock };
            own.functions.append(found.span());
            own.didChangeFunctions();
        }
        found.shrink(0);
    }

    own.isBusy.store(false, std::memory_order_release);
    m_numberOfSpareThreads.fetch_add(1, std::memory_order_release);
    if (m_numberOfBusyWorkers.exchangeSub(1) == 1)
        ParkingLot::unparkAll(&m_numberOfBusyWorkers);
}

void FunctionQueue::processEach(std::span<const QueuedFunction> functions, NOESCAPE const Process& process)
{
    processEach(functions, { }, process, [](FunctionID) { });
}

void FunctionQueue::processEach(std::span<const QueuedFunction> functions, std::span<const QueuedFunction> speculativeFunctions, NOESCAPE const Process& process, NOESCAPE const DidConfirm& didConfirm)
{
    RELEASE_ASSERT(!m_process);
    m_process = &process;
    m_didConfirm = &didConfirm;

    Vector<QueuedFunction> first;
    first.reserveInitialCapacity(functions.size() + speculativeFunctions.size());
    for (QueuedFunction function : functions) {
        if (confirm(function.id) == Confirmation::FirstQueued)
            first.append(function);
    }
    for (QueuedFunction function : speculativeFunctions) {
        RELEASE_ASSERT(!ProvisionalFunctions::contains(function.id));
        std::atomic<uint8_t>& state = m_states[function.id.index()];
        if (state.load(std::memory_order_relaxed) & Queued)
            continue;
        state.store(Queued | Speculative, std::memory_order_relaxed);
        first.append(function);
        m_speculativeFunctions.append(SpeculativeFunction { function.id, { } });
    }
    AOT::sortInLine(m_speculativeFunctions, { }, &SpeculativeFunction::id);
    parallelSort(m_loops, first.mutableSpan(), isProcessedLater);

    ParallelRegionScope parallelRegion;
    m_numberOfSpareThreads.store(m_loops.width() - 1, std::memory_order_relaxed);
    m_numberOfBusyWorkers.store(1);
    Worker& own = takeIdleWorker();
    own.setFunctions(WTF::move(first));
    processShare(own);
    if (m_numberOfBusyWorkers.load()) {
        BusyThreads::WaitScope waitScope;
        for (uint32_t busy = m_numberOfBusyWorkers.load(); busy; busy = m_numberOfBusyWorkers.load())
            ParkingLot::compareAndPark(&m_numberOfBusyWorkers, busy);
    }
    m_process = nullptr;
    m_didConfirm = nullptr;
}

// Select replaces operations by others whose effects it checks against those of the operations they replace.
static Effects effectsOfConversionsIn(const Function& function)
{
    Effects effects = Effects::none();
    for (const Value& value : function.values) {
        if (familyOf(value.opcode) == OperationFamily::Representation)
            effects.merge(declaredBoundOf(value.opcode));
        else if (value.representation == Representation::Cell32 || value.representation == Representation::LiteralString)
            effects.merge(declaredBoundOf(Opcode::Decode));
    }
    return effects;
}

static bool holdsExpectedTypeEqualToType(const Function& function)
{
    return std::ranges::any_of(function.values, [](const Value& value) {
        return value.expected().predicted() && value.expected().predicted() == value.type;
    });
}

namespace {

// Functions that go through the steps of a loop together: small enough to stay in the cache from one step to the next, and many enough for the code of a step to stay there too.
struct Batch {
    uint32_t first { 0 };
    uint32_t size { 0 };
};

} // anonymous namespace

static Vector<Batch> createBatches(std::span<const uint64_t> largestFirst, bool hasSeveralSteps)
{
    AOT_THRESHOLD(uint32_t, maxFunctionsInBatch, 64);
    AOT_THRESHOLD(uint64_t, maxValuesInBatch, 4096);
    if (!hasSeveralSteps) {
        return Vector<Batch>(largestFirst.size(), [](size_t position) {
            return Batch { static_cast<uint32_t>(position), 1 };
        });
    }
    Vector<Batch> batches;
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
            keys.append(static_cast<uint64_t>(UINT32_MAX - numberOfValuesIn(program, FunctionID(index))) << 32 | index);
    });
    parallelSort(loops, order.mutableSpan(), std::less<uint64_t> { });
    Vector<Batch> batches = createBatches(order.span(), reader);
    Vector<uint8_t> didChange(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasImpureConversions(FillWith { }, schedule == FunctionSchedule::Represent ? count : 0, static_cast<uint8_t>(0));
    // Of a function that has changed. The task of a function has the number of the function. A task notes every provisional id that it uses: where it has noted none, the body holds none.
    Vector<uint8_t> awaitsEndOfLoop(FillWith { }, reader ? count : 0, static_cast<uint8_t>(0));
    auto phases = makeUnique<InterningPhases>(program, count);
    BodyIndex& bodyIndex = analyses.bodyIndex();
    bodyIndex.growTo(count);
    didFinish("order functions, open phases"_s);
    {
        ProgramCaches caches(program, runner.build());
        // A pass frees on the thread that runs it what another thread allocated, and the allocator keeps that until the thread that allocated it looks.
        // Selection, where a build holds the most, makes each function anew.
        const unsigned batchesBetweenRequests = std::max<unsigned>(batches.size() / 4, 1);
        auto runBatch = [&](unsigned batchIndex) {
            if (schedule == FunctionSchedule::Select && batchIndex && !(batchIndex % batchesBetweenRequests))
                loops.askThreadsAtWorkForFreeMemory();
            std::span<const uint64_t> functionsInBatch = order.subspan(batches[batchIndex].first, batches[batchIndex].size);
            for (uint64_t key : functionsInBatch) {
                unsigned i = static_cast<unsigned>(key);
                if (steps.inFront)
                    (*steps.inFront)(FunctionID(i), program.functions[i]);
                TaskArenas arenas(*phases, i);
                // The step in front may have written to the function behind its row.
                std::optional<BodyContents> contents = steps.inFront ? std::nullopt : bodyIndex.contentsOf(FunctionID(i));
                didChange[i] = runner.runSchedule(schedule, program, inputs, FunctionID(i), program.functions[i], arenas, contents) == PassOutcome::Changed;
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
                    awaitsEndOfLoop[i] = true;
            }
            for (const ReaderOfScheduledFunction* read : steps.behind) {
                for (uint64_t key : functionsInBatch) {
                    unsigned i = static_cast<unsigned>(key);
                    if (!awaitsEndOfLoop[i])
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
        if (didChange[i] && (!reader || awaitsEndOfLoop[i]))
            changedAndNotFinal.append(FunctionID(i));
    }
    if (!remaps.isIdentity()) {
        forEachIndexInParallel(loops, changedAndNotFinal.size(), [&](unsigned position) {
            Function& function = program.at(changedAndNotFinal[position]);
            unsigned task = changedAndNotFinal[position].index();
            if (!remaps.types.isIdentityFor(task) || !remaps.strings.isIdentityFor(task)) {
                applyRemaps(remaps, function);
                return;
            }
            // All that remapIDs() does to a body without provisional ids.
            for (Value& value : function.editAllValues()) {
                if (value.expected().predicted() == value.type)
                    value.setExpected({ });
            }
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
    // Nobody reads a finished copy until all are finished.
    ParkedBody body;
};

} // anonymous namespace

// The size estimates how long a function takes to compile. A function with warm code runs the long schedule and grows through inlining.
static QueuedFunction createQueuedFunction(const Program& program, FunctionID id, const Heat* heat = nullptr)
{
    if (ProvisionalFunctions::contains(id))
        return { id, 0 };
    AOT_THRESHOLD(uint64_t, warmCodeCostFactor, 6);
    uint64_t size = numberOfValuesIn(program, id);
    if (heat && heat->hasWarmCode(program, id).predicted())
        size *= warmCodeCostFactor;
    return { id, static_cast<uint32_t>(std::min<uint64_t>(size, UINT32_MAX)) };
}

static void runOnCopies(const ScheduleDescriptor& schedule, Program& programToEdit, const OptimizationInputs& inputs, const PassRunner& runner, ProgramAnalyses& analyses)
{
    using FoundFunctions = FunctionQueue::FoundFunctions;
    RELEASE_ASSERT(inputs.derivedFunctions);
    DerivedFunctions& derivedFunctions = *inputs.derivedFunctions;
    // Only the const overload of Program::at answers for a provisional id.
    const Program& program = programToEdit;
    BuildContext& build = runner.build();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    auto phases = makeUnique<InterningPhases>(programToEdit, count);
    {
        DerivedFunctionPhase derived(derivedFunctions, programToEdit, inputs.flow, *phases);
        FunctionQueue queue(loops, count);
        // Of the functions that have changed.
        Vector<std::unique_ptr<Function>> optimized(count);
        Vector<uint8_t> areValuesTypedAsDefined(FillWith { }, count, static_cast<uint8_t>(0));
        // Of a copy that has changed.
        Vector<std::unique_ptr<FinishedCopy>> finished(count);
        const Heat* heat = schedule.id == FunctionSchedule::Optimize ? &inputs.heat : nullptr;
        // A function is found only when one that names it is finished, so long functions that name each other would run one after the other.
        Vector<QueuedFunction> startedEarly;
        if (schedule.functions == ScheduledFunctions::Reached) {
            AOT_THRESHOLD(size_t, minValuesOfLargeFunction, 4096);
            AOT_THRESHOLD(size_t, minValuesWithWarmCode, 64);
            startedEarly = collectInParallel<QueuedFunction>(loops, count, [&](unsigned index, Vector<QueuedFunction>& found) {
                FunctionID id(index);
                size_t numberOfValues = numberOfValuesIn(program, id);
                if (numberOfValues >= minValuesOfLargeFunction || (heat && numberOfValues >= minValuesWithWarmCode && heat->hasWarmCode(program, id).predicted()))
                    found.append(createQueuedFunction(program, id, heat));
            });
        }

        auto reach = [&](FunctionID target, FunctionReferenceKind kind, FoundFunctions& found) {
            found.append(createQueuedFunction(program, target, heat));
            switch (kind) {
            case FunctionReferenceKind::Call:
            case FunctionReferenceKind::Dispatch:
                return;
            case FunctionReferenceKind::Closure:
            case FunctionReferenceKind::Constructor:
                break;
            }
            if (schedule.functions != ScheduledFunctions::Reached)
                return;
            FunctionRole role = ProvisionalFunctions::contains(target) ? derivedFunctions.keyOf(target).role : program.at(target).role;
            if (!canBeCalledByValueOf(role))
                return;
            found.append(createQueuedFunction(program, derivedFunctions.ensure(derivedFunctionKey(FunctionRole::GenericEntrypoint, target))));
            // Asked about its instances, a bound function asks its target. Where it captures no closure of the target, the run time makes one, which the tables name by its entrypoint.
            if (role == FunctionRole::BoundFunction) {
                FunctionID boundTarget = ProvisionalFunctions::contains(target) ? derivedFunctions.keyOf(target).target : program.at(target).derivation.source;
                found.append(createQueuedFunction(program, derivedFunctions.ensure(derivedFunctionKey(FunctionRole::GenericEntrypoint, boundTarget))));
            }
        };

        // Many bodies may name one callee set. Its functions are queued by the first body that names it and is known to be reached: what a body finds that was only started early may never be queued.
        // A set that a pass makes has a number of its task.
        Vector<uint8_t> isCalleeSetRead(FillWith { }, program.types.numberOfCalleeSets(), static_cast<uint8_t>(0));
        auto shouldReadFor = [&](bool isReached, CalleeSetID set) {
            if (!isReached || set.index() >= isCalleeSetRead.size())
                return true;
            AtomicRef flag(isCalleeSetRead[set.index()]);
            return !flag.load(std::memory_order_relaxed) && !flag.exchange(1, std::memory_order_relaxed);
        };

        {
            ProgramCaches caches(program, build);
            Vector<QueuedFunction> scheduled;
            switch (schedule.functions) {
            case ScheduledFunctions::All:
                scheduled = collectInParallel<QueuedFunction>(loops, count, [&](unsigned index, Vector<QueuedFunction>& functionsWithBody) {
                    if (hasBody(program, FunctionID(index)))
                        functionsWithBody.append(createQueuedFunction(program, FunctionID(index)));
                });
                break;
            case ScheduledFunctions::Reached:
                scheduled = collectInParallel<QueuedFunction>(loops, inputs.callGraph.roots().size(), [&](unsigned position, Vector<QueuedFunction>& roots) {
                    roots.append(createQueuedFunction(program, inputs.callGraph.roots()[position], heat));
                });
                break;
            case ScheduledFunctions::OneAtATime:
                RELEASE_ASSERT_NOT_REACHED();
            }
            queue.processEach(scheduled.span(), startedEarly.span(), [&](FunctionID id, FoundFunctions& found) {
                const Function& source = program.at(id);
                if (!source.hasBody())
                    return;
                bool isProvisional = ProvisionalFunctions::contains(id);
                auto copy = makeUniqueWithoutFastMallocCheck<Function>(source);
                if (source.valuesAreTypedAsDefined())
                    copy->setValuesAreTypedAsDefined();
                TaskArenas arenas(*phases, isProvisional ? derived.taskOf(id) : id.index());
                if (isProvisional)
                    adoptIDs(*copy, arenas.types);
                DerivedFunctionRequester requester(derived, id, queue.wasConfirmed(id));
                BodyIndex* index = program.bodyIndex();
                PassOutcome outcome = runner.runSchedule(schedule.id, program, inputs, id, *copy, arenas, index && !isProvisional ? index->contentsOf(id) : std::nullopt);
                if (build.options().verifiesAfterEveryPass) [[unlikely]]
                    RELEASE_ASSERT(!numberOfForeignIDs(*copy, arenas.types));
                shrinkToFit(*copy);
                // An All schedule has already queued every function with a body.
                if (schedule.functions == ScheduledFunctions::Reached) {
                    bool isReached = queue.wasConfirmed(id);
                    forEachFunctionReference(program, *copy, [&](FunctionID target, FunctionReferenceKind kind) {
                        reach(target, kind, found);
                    }, [&](CalleeSetID set) {
                        return shouldReadFor(isReached, set);
                    });
                }
                if (isProvisional)
                    derived.setOptimizedBody(id, WTF::move(copy));
                else {
                    areValuesTypedAsDefined[id.index()] = copy->valuesAreTypedAsDefined();
                    // A copy that no pass has changed dies here.
                    if (outcome == PassOutcome::Changed) {
                        finished[id.index()] = makeUnique<FinishedCopy>(*copy);
                        if (schedule.id == FunctionSchedule::Optimize)
                            finished[id.index()]->body = ParkedBody(copy->storageOfBlocks(), copy->storageOfValues());
                        optimized[id.index()] = WTF::move(copy);
                    }
                }
            }, [&](FunctionID id) {
                derived.confirmRequestsOf(id);
            });
        }

        if (build.diagnostics().hasErrors())
            phases->discard();
        else {
            bool removesUnreachedBodies = schedule.id == FunctionSchedule::Optimize;
            DerivedFunctionCommit commit = derived.commit(queue.takeQueuedProvisionalOrdinals());
            Vector<unsigned> taskOrder;
            taskOrder.reserveInitialCapacity(count + commit.tasks.size());
            for (unsigned task = 0; task < count; ++task) {
                if (queue.wasConfirmed(FunctionID(task)))
                    taskOrder.append(task);
            }
            taskOrder.appendVector(commit.tasks);
            BodyRemaps remaps = phases->finish(taskOrder.span(), WTF::move(commit.remap), &loops);

            // The list of functions moves when it grows. All threads take the page faults of where it moves to, and there is room for what the passes behind the loop add.
            if (size_t numberOfFunctions = count + commit.functions.size(); numberOfFunctions > program.functions.capacity())
                reserveCapacityInParallel(loops, programToEdit.functions, numberOfFunctions + numberOfFunctions / 4);
            ProgramEditor editor(programToEdit);
            FunctionID firstAdded = editor.reserveFunctions(commit.functions.size());
            RELEASE_ASSERT(firstAdded == commit.firstID && firstAdded.index() == count);

            BodyIndex& bodyIndex = analyses.bodyIndex();
            std::atomic<uint64_t> numberOfRemovedBodies { 0 };
            // A thread that frees what another thread has allocated cannot use that room. So the bodies from before the loop go first, and each thread takes in what was freed of its own.
            forEachIndexInParallel(loops, count, [&](unsigned index) {
                FunctionID id(index);
                if (queue.wasConfirmed(id)) {
                    if (optimized[index] && finished[index]->body) {
                        programToEdit.functions[index].storageOfBlocks() = { };
                        programToEdit.functions[index].storageOfValues() = { };
                    }
                    return;
                }
                if (!removesUnreachedBodies || !program.functions[index].hasBody())
                    return;
                editor.removeBody(id);
                numberOfRemovedBodies.fetch_add(1, std::memory_order_relaxed);
            });
            if (removesUnreachedBodies)
                build.giveFreedMemoryBack();
            forEachIndexInParallel(loops, count, [&](unsigned index) {
                FunctionID id(index);
                if (queue.wasConfirmed(id)) {
                    if (auto& copy = optimized[index]) {
                        FinishedCopy& finishedCopy = *finished[index];
                        if (finishedCopy.body)
                            finishedCopy.body.takeInto(copy->storageOfBlocks(), copy->storageOfValues());
                        if (!finishedCopy.remapSites.isEmpty()) {
                            applyRemaps(remaps, *copy, finishedCopy.remapSites);
                            finishedCopy.row.applyRemaps(remaps.types, remaps.functions, finishedCopy.remapSites.record ? copy.get() : nullptr);
                        }
                        std::swap(editor.at(id), *copy);
                        bodyIndex.write(id, WTF::move(finishedCopy.row));
                        copy = nullptr;
                    } else if (areValuesTypedAsDefined[index])
                        programToEdit.functions[index].setValuesAreTypedAsDefined();
                }
            });
            forEachIndexInParallel(loops, commit.functions.size(), [&](unsigned position) {
                Function& function = programToEdit.functions[count + position];
                function = WTF::move(*commit.functions[position]);
                applyRemaps(remaps, function);
                bodyIndex.write(FunctionID(count + position), function, BodyIDs::Final);
            });

            if (removesUnreachedBodies) {
                build.statistics().count(StatisticID::UnreachableFunctionsRemoved, numberOfRemovedBodies.load(std::memory_order_relaxed));
                build.statistics().count(StatisticID::FunctionsStartedEarly, startedEarly.size());
                build.statistics().count(StatisticID::FunctionsStartedEarlyButNotReached, static_cast<uint64_t>(std::ranges::count_if(startedEarly, [&](QueuedFunction function) {
                    return !queue.wasConfirmed(function.id);
                })));
                derivedFunctions.forgetFunctionsWithoutBody(program);
            }
            analyses.invalidate(resultsAboutBodies, editor);
            build.resolveFunctionToDump(program);
            build.destroyLater(WTF::move(optimized));
            build.destroyLater(WTF::move(finished));
            build.destroyLater(WTF::move(commit.functions));
        }
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
