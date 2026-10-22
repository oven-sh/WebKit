/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <algorithm>
#include <array>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Seconds.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/WorkerPool.h>

namespace JSC { namespace AOT {

// The thread pool of the program that the engine is part of. Its threads have at least half of ParallelLoops::workerStackSize.
struct HostThreadPool {
    // Calls run(argument) once, on a thread of the pool, and does not wait for it.
    void (*schedule)(void (*run)(void* argument), void* argument);
};

// Foreground is the critical path of the build. Background is work that a later stage waits for. Nothing waits for Idle work.
// A thread takes the pending task with the highest priority, and a helper of a loop leaves it when a task with a higher priority is pending.
enum class TaskPriority : uint8_t { Foreground, Background, Idle };
static constexpr unsigned numberOfTaskPriorities = 3;

class ParallelLoops {
    WTF_MAKE_NONCOPYABLE(ParallelLoops);
    WTF_MAKE_NONMOVABLE(ParallelLoops);
public:
    static constexpr size_t workerStackSize = 8 * MB;
    static constexpr Seconds defaultRestTime = Seconds::fromMilliseconds(100);

    // Without a host pool, the loops run on a pool of their own. A thread of the pool that has run out of work waits for more for the rest time
    // before it goes back to the pool.
    JS_EXPORT_PRIVATE explicit ParallelLoops(unsigned width, const HostThreadPool* = nullptr, Seconds restTime = defaultRestTime);
    JS_EXPORT_PRIVATE ~ParallelLoops();

    unsigned width() const { return m_width; }
    JS_EXPORT_PRIVATE unsigned numberOfRestingThreads() const;
    void sendRestingThreadsBack() const;

    // Loops and tasks that a task starts have its priority unless they name another.
    JS_EXPORT_PRIVATE static TaskPriority currentPriority();
    void schedule(void (*run)(void* argument), void* argument) const { schedule(currentPriority(), run, argument); }
    JS_EXPORT_PRIVATE void schedule(TaskPriority, void (*run)(void* argument), void* argument) const;
    bool shouldYield(TaskPriority) const;

    // For a stretch of many short loops. Waking a thread that sleeps takes the waker a system call and the sleeper tens of microseconds, which is
    // as long as such a loop. So while a scope is alive, a thread that has run out of work looks for more for a moment before it sleeps, and so
    // does a thread that waits for the helpers of its loop. Narrow loops belong to a small program, which has no time to gain: there it does nothing.
    class ShortLoopsScope {
        WTF_MAKE_NONCOPYABLE(ShortLoopsScope);
        WTF_MAKE_NONMOVABLE(ShortLoopsScope);
    public:
        JS_EXPORT_PRIVATE explicit ShortLoopsScope(const ParallelLoops&);
        JS_EXPORT_PRIVATE ~ShortLoopsScope();

    private:
        const ParallelLoops& m_loops;
    };
    bool expectsShortLoops() const;

private:
    class RestingThreads;

    const unsigned m_width;
    const HostThreadPool* const m_hostPool;
    RefPtr<WorkerPool> m_ownPool;
    const Ref<RestingThreads> m_restingThreads;
};

JS_EXPORT_PRIVATE void runOnBuildThread(NOESCAPE const ScopedLambda<void()>&);

// While one is alive, the CPU time of the current thread counts as spent in a parallel region. Only a build that logs its phase times counts.
class ParallelRegionScope {
    WTF_MAKE_NONCOPYABLE(ParallelRegionScope);
public:
    JS_EXPORT_PRIVATE ParallelRegionScope();
    JS_EXPORT_PRIVATE ~ParallelRegionScope();

    static Seconds cpuTimeOfCurrentThread();

private:
    const bool m_isCounted;
};

// For how long how many threads of the build were at work. Only a build that logs its phase times counts.
class BusyThreads {
public:
    static constexpr unsigned numberOfBuckets = 5;
    static constexpr std::array<unsigned, numberOfBuckets> maxThreadsOfBucket { 1, 4, 8, 12, std::numeric_limits<unsigned>::max() };
    using Times = std::array<Seconds, numberOfBuckets>;

    static void countCurrentThread();
    static Times times();

    // While one is alive, the current thread waits for other threads.
    class WaitScope {
        WTF_MAKE_NONCOPYABLE(WaitScope);
    public:
        JS_EXPORT_PRIVATE WaitScope();
        JS_EXPORT_PRIVATE ~WaitScope();

    private:
        const bool m_wasCounted;
    };
};

JS_EXPORT_PRIVATE void forEachIndexInParallel(const ParallelLoops&, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index)>&);
JS_EXPORT_PRIVATE void forEachIndexInParallel(const ParallelLoops&, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index, unsigned worker)>&);
// For a loop in which a few indices take long: they come first, in the order given. A loop lasts as long as its longest index from the moment that one begins.
JS_EXPORT_PRIVATE void forEachIndexInParallel(const ParallelLoops&, unsigned count, std::span<const uint32_t> longestFirst, NOESCAPE const ScopedLambda<void(unsigned index)>&);
JS_EXPORT_PRIVATE void runTasksInParallel(const ParallelLoops&, std::initializer_list<ScopedLambda<void()>>);
// produce(index) runs on any thread. commit(index) runs on the calling thread, in index order, as soon as every index up to it is produced, while later indices are still being produced.
JS_EXPORT_PRIVATE void forEachIndexInParallelWithOrderedCommit(const ParallelLoops&, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index)>& produce, NOESCAPE const ScopedLambda<void(unsigned index)>& commit);

// An array whose elements the threads of a loop create.
template<typename T>
class ParallelArray {
    WTF_MAKE_NONCOPYABLE(ParallelArray);
public:
    ParallelArray() = default;
    ParallelArray(ParallelArray&&) = default;

    ParallelArray(const ParallelLoops& loops, unsigned size, const Invocable<T(unsigned)> auto& create)
        : ParallelArray(loops, size, { }, create)
    {
    }

    ParallelArray(const ParallelLoops& loops, unsigned size, std::span<const uint32_t> longestFirst, const Invocable<T(unsigned)> auto& create)
        : m_elements(MallocSpan<T>::malloc(size * sizeof(T)))
    {
        static_assert(alignof(T) <= 16);
        forEachIndexInParallel(loops, size, longestFirst, [&](unsigned index) {
            new (NotNull, &m_elements[index]) T(create(index));
        });
    }

    static ParallelArray concatenationOf(const ParallelLoops& loops, Vector<Vector<T>>&& lists)
    {
        static_assert(alignof(T) <= 16);
        Vector<size_t> firstOfList(lists.size());
        size_t size = 0;
        for (size_t index = 0; index < lists.size(); ++index)
            firstOfList[index] = std::exchange(size, size + lists[index].size());
        ParallelArray result;
        result.m_elements = MallocSpan<T>::malloc(size * sizeof(T));
        forEachIndexInParallel(loops, static_cast<unsigned>(lists.size()), [&](unsigned index) {
            size_t position = firstOfList[index];
            for (T& element : lists[index])
                new (NotNull, &result.m_elements[position++]) T(WTF::move(element));
            lists[index] = { };
        });
        return result;
    }

    ~ParallelArray() { std::ranges::destroy(m_elements.mutableSpan()); }

    ParallelArray& operator=(ParallelArray&& other)
    {
        ParallelArray previous { WTF::move(*this) };
        m_elements = WTF::move(other.m_elements);
        return *this;
    }

    unsigned size() const { return static_cast<unsigned>(m_elements.span().size()); }
    std::span<const T> span() const LIFETIME_BOUND { return m_elements.span(); }
    std::span<T> mutableSpan() LIFETIME_BOUND { return m_elements.mutableSpan(); }
    T& operator[](size_t index) LIFETIME_BOUND { return m_elements[index]; }
    const T& operator[](size_t index) const LIFETIME_BOUND { return m_elements[index]; }

private:
    MallocSpan<T> m_elements;
};

// Memory that comes from the system costs whoever writes to a page first a fault, and a fault costs as much as writing the page many times over.
// A list with an entry for each function of a large program is dozens of megabytes. Where one thread makes it, the others wait for its faults.
// So all threads write to the pages first, and the one thread constructs on pages that are there.
inline void takePageFaultsInParallel(const ParallelLoops& loops, std::span<uint8_t> bytes)
{
    constexpr size_t bytesPerTask = 256 * KB;
    if (bytes.size() < 4 * bytesPerTask)
        return;
    forEachIndexInParallel(loops, static_cast<unsigned>((bytes.size() + bytesPerTask - 1) / bytesPerTask), [&](unsigned task) {
        zeroSpan(bytes.subspan(task * bytesPerTask, std::min(bytesPerTask, bytes.size() - task * bytesPerTask)));
    });
}

// An empty vector with room for `capacity` elements on pages that are there.
template<typename T>
Vector<T> createVectorWithCapacityInParallel(const ParallelLoops& loops, size_t capacity)
{
    Vector<T> vector;
    vector.reserveInitialCapacity(capacity);
    // AddressSanitizer is told that nobody writes behind the end of a vector.
#if !ASAN_ENABLED
    takePageFaultsInParallel(loops, unsafeMakeSpan(reinterpret_cast<uint8_t*>(vector.mutableSpan().data()), capacity * sizeof(T)));
#else
    UNUSED_PARAM(loops);
#endif
    return vector;
}

// What is collected comes in the order of the indices, whatever the order they are visited in. An index that takes long is a chunk of its own.
template<typename T>
Vector<Vector<T>> collectChunksInParallel(const ParallelLoops& loops, unsigned count, std::span<const uint32_t> longestFirst, const Invocable<void(unsigned, Vector<T>&)> auto& collect)
{
    constexpr unsigned chunkSize = 256;
    Vector<uint32_t> ascending(longestFirst);
    std::ranges::sort(ascending);
    Vector<uint32_t> firstIndexOf;
    firstIndexOf.reserveInitialCapacity(count / chunkSize + 2 * ascending.size() + 2);
    Vector<uint32_t> chunkOfLongIndex;
    chunkOfLongIndex.reserveInitialCapacity(ascending.size());
    uint32_t nextIndex = 0;
    auto addChunksUpTo = [&](uint32_t end) {
        for (; nextIndex < end; nextIndex = std::min(end, nextIndex + chunkSize))
            firstIndexOf.append(nextIndex);
    };
    for (uint32_t longIndex : ascending) {
        addChunksUpTo(longIndex);
        chunkOfLongIndex.append(firstIndexOf.size());
        addChunksUpTo(longIndex + 1);
    }
    addChunksUpTo(count);
    unsigned numberOfChunks = firstIndexOf.size();
    firstIndexOf.append(count);

    Vector<uint32_t> longestChunksFirst(longestFirst.size(), [&](size_t position) {
        size_t rank = std::ranges::lower_bound(ascending, longestFirst[position]) - ascending.begin();
        return chunkOfLongIndex[rank];
    });
    Vector<Vector<T>> chunks(numberOfChunks);
    forEachIndexInParallel(loops, numberOfChunks, longestChunksFirst.span(), [&](unsigned chunk) {
        for (unsigned index = firstIndexOf[chunk]; index < firstIndexOf[chunk + 1]; ++index)
            collect(index, chunks[chunk]);
    });
    return chunks;
}

template<typename T>
Vector<T> collectInParallel(const ParallelLoops& loops, unsigned count, std::span<const uint32_t> longestFirst, const Invocable<void(unsigned, Vector<T>&)> auto& collect)
{
    Vector<Vector<T>> chunks = collectChunksInParallel<T>(loops, count, longestFirst, collect);
    size_t size = 0;
    for (auto& chunk : chunks)
        size += chunk.size();
    Vector<T> result = createVectorWithCapacityInParallel<T>(loops, size);
    for (auto& chunk : chunks)
        result.appendVector(WTF::move(chunk));
    constexpr size_t fewestChunksFreedOnPool = 64;
    if (chunks.size() >= fewestChunksFreedOnPool) {
        forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
            chunks[chunk] = { };
        });
    }
    return result;
}

template<typename T>
Vector<T> collectInParallel(const ParallelLoops& loops, unsigned count, const Invocable<void(unsigned, Vector<T>&)> auto& collect)
{
    return collectInParallel<T>(loops, count, { }, collect);
}

template<typename T>
ParallelArray<T> collectIntoArrayInParallel(const ParallelLoops& loops, unsigned count, std::span<const uint32_t> longestFirst, const Invocable<void(unsigned, Vector<T>&)> auto& collect)
{
    return ParallelArray<T>::concatenationOf(loops, collectChunksInParallel<T>(loops, count, longestFirst, collect));
}

Vector<uint32_t> findIndicesInParallel(const ParallelLoops& loops, unsigned count, const Invocable<bool(unsigned)> auto& matches)
{
    return collectInParallel<uint32_t>(loops, count, [&](unsigned index, Vector<uint32_t>& found) {
        if (matches(index))
            found.append(index);
    });
}

template<typename T, typename Less>
void parallelSort(const ParallelLoops& loops, std::span<T> list, const Less& isLess)
{
    constexpr size_t numberOfRuns = 16;
    constexpr size_t fewestForPool = 1 << 13;
    size_t count = list.size();
    if (count < fewestForPool) {
        std::ranges::sort(list, isLess);
        return;
    }
    Vector<T> scratchBuffer = createVectorWithCapacityInParallel<T>(loops, count);
    scratchBuffer.grow(count);
    std::span<T> from = list;
    std::span<T> to = scratchBuffer.mutableSpan();
    auto runOf = [&](std::span<T> all, size_t run, size_t runs) {
        size_t start = count * run / runs;
        return all.subspan(start, count * (run + 1) / runs - start);
    };
    forEachIndexInParallel(loops, numberOfRuns, [&](unsigned run) {
        std::ranges::sort(runOf(from, run, numberOfRuns), isLess);
    });
    for (size_t runs = numberOfRuns; runs > 1; runs /= 2) {
        forEachIndexInParallel(loops, runs / 2, [&](unsigned pair) {
            std::ranges::merge(runOf(from, 2 * pair, runs), runOf(from, 2 * pair + 1, runs), runOf(to, pair, runs / 2).begin(), isLess);
        });
        std::swap(from, to);
    }
    static_assert(numberOfRuns == 16, "an even number of rounds of merging leaves the result in the list");
}

template<typename T, size_t inlineCapacity>
void sortAndRemoveDuplicates(const ParallelLoops& loops, Vector<T, inlineCapacity>& list, const auto& isLess, const auto& isEqual)
{
    parallelSort(loops, list.mutableSpan(), isLess);
    removeRepeatedElements(list, isEqual);
}

template<typename T, size_t inlineCapacity>
void sortAndRemoveDuplicates(const ParallelLoops& loops, Vector<T, inlineCapacity>& list)
{
    sortAndRemoveDuplicates(loops, list, std::less<T> { }, std::equal_to<T> { });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
