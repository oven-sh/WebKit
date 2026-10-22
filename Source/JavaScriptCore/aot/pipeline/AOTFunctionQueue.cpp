/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionQueue.h"

#if ENABLE(AOT)

#include "AOTThreads.h"
#include <algorithm>
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

auto FunctionQueue::ensureProvisionalWord(FunctionID id) -> Word&
{
    uint32_t word = ProvisionalFunctions::ordinalOf(id) / bitsPerWord;
    RELEASE_ASSERT(word / wordsPerSegment < numberOfSegments);
    std::atomic<Segment*>& slot = m_isProvisionalQueued[word / wordsPerSegment];
    Segment* segment = slot.load(std::memory_order_acquire);
    if (!segment) {
        auto created = makeUniqueWithoutFastMallocCheck<Segment>();
        if (slot.compare_exchange_strong(segment, created.get(), std::memory_order_release, std::memory_order_acquire))
            segment = created.release();
    }
    return (*segment)[word % wordsPerSegment];
}

auto FunctionQueue::provisionalWord(FunctionID id) const -> const Word*
{
    uint32_t word = ProvisionalFunctions::ordinalOf(id) / bitsPerWord;
    if (word / wordsPerSegment >= numberOfSegments)
        return nullptr;
    const Segment* segment = m_isProvisionalQueued[word / wordsPerSegment].load(std::memory_order_acquire);
    return segment ? &(*segment)[word % wordsPerSegment] : nullptr;
}

bool FunctionQueue::isSpeculative(FunctionID id) const
{
    return !ProvisionalFunctions::contains(id) && m_states[id.index()].load(std::memory_order_relaxed) & Speculative;
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
        Word& word = ensureProvisionalWord(id);
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
    const Word* word = provisionalWord(id);
    return word && word->load(std::memory_order_relaxed) & 1ull << (id.index() % bitsPerWord);
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
    {
        Locker locker { worker.lock };
        RELEASE_ASSERT(worker.functions.isEmpty());
        worker.functions = WTF::move(functions);
        worker.didChangeFunctions();
    }
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
        Locker locker { own.lock };
        RELEASE_ASSERT(own.functions.isEmpty());
        own.functions = WTF::move(taken);
        own.didChangeFunctions();
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
        if (isSpeculative(next->id) && !finishSpeculativeFunction(next->id, found)) {
            found.shrink(0);
            continue;
        }

        confirmFoundFunctions(found);
        std::ranges::sort(found, isProcessedLater);
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
    std::ranges::sort(m_speculativeFunctions, { }, &SpeculativeFunction::id);
    std::ranges::sort(first, isProcessedLater);

    ParallelRegionScope parallelRegion;
    m_numberOfSpareThreads.store(m_loops.width() - 1, std::memory_order_relaxed);
    m_numberOfBusyWorkers.store(1);
    Worker& own = takeIdleWorker();
    {
        Locker locker { own.lock };
        own.functions = WTF::move(first);
        own.didChangeFunctions();
    }
    processShare(own);
    if (m_numberOfBusyWorkers.load()) {
        BusyThreads::WaitScope waitScope;
        for (uint32_t busy = m_numberOfBusyWorkers.load(); busy; busy = m_numberOfBusyWorkers.load())
            ParkingLot::compareAndPark(&m_numberOfBusyWorkers, busy);
    }
    m_process = nullptr;
    m_didConfirm = nullptr;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
