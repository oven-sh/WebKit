/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
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
#include <wtf/WorkerPool.h>

namespace JSC { namespace AOT {

// The thread pool of the program that the engine is part of. Its threads have at least half of ParallelLoops::workerStackSize.
struct HostThreadPool {
    // Calls run(argument) once, on a thread of the pool, and does not wait for it.
    void (*schedule)(void (*run)(void* argument), void* argument);
    // Gives what the allocator keeps for the current thread, and has no use for, back to the system.
    void (*giveFreeMemoryOfCurrentThreadBack)();
};

// Foreground is the critical path of the build.
enum class TaskPriority : uint8_t { Foreground, Background, Idle };
static constexpr unsigned numberOfTaskPriorities = 3;

class ParallelLoops {
    WTF_MAKE_NONCOPYABLE(ParallelLoops);
    WTF_MAKE_NONMOVABLE(ParallelLoops);
public:
    static constexpr size_t workerStackSize = 8 * MB;
    static constexpr Seconds defaultRestTime = Seconds::fromMilliseconds(100);

    // Without a host pool, the loops run on a pool of their own.
    JS_EXPORT_PRIVATE explicit ParallelLoops(unsigned width, const HostThreadPool* = nullptr, Seconds restTime = defaultRestTime);
    JS_EXPORT_PRIVATE ~ParallelLoops();

    unsigned width() const { return m_width; }
    JS_EXPORT_PRIVATE unsigned numberOfRestingThreads() const;
    void sendRestingThreadsBack() const;
    // Memory that one thread allocated and another freed stays with the first until that thread looks. Every thread looks before its next task, and the resting ones are woken for it.
    // Nothing waits: what comes next runs beside it.
    JS_EXPORT_PRIVATE void giveFreeMemoryBack() const;
    // The same for the threads at work: each looks between the runs of a loop.
    JS_EXPORT_PRIVATE void askThreadsAtWorkForFreeMemory() const;

    // Loops and tasks that a task starts have its priority unless they name another.
    JS_EXPORT_PRIVATE static TaskPriority currentPriority();
    void schedule(void (*run)(void* argument), void* argument) const { schedule(currentPriority(), run, argument); }
    JS_EXPORT_PRIVATE void schedule(TaskPriority, void (*run)(void* argument), void* argument) const;
    bool shouldYield(TaskPriority) const;

    // Whether the current thread is at one of several indices of a loop or at one of several tasks, whatever the width.
    JS_EXPORT_PRIVATE static bool isInsideLoop();

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
template<typename T, size_t inlineCapacity = 0>
Vector<T, inlineCapacity> createVectorWithCapacityInParallel(const ParallelLoops& loops, size_t capacity)
{
    Vector<T, inlineCapacity> vector;
    vector.reserveInitialCapacity(capacity);
    // AddressSanitizer is told that nobody writes behind the end of a vector.
#if !ASAN_ENABLED
    takePageFaultsInParallel(loops, unsafeMakeSpan(reinterpret_cast<uint8_t*>(vector.mutableSpan().data()), capacity * sizeof(T)));
#else
    UNUSED_PARAM(loops);
#endif
    return vector;
}

struct ChunksOfIndices {
    unsigned size() const { return firstIndexOf.size() - 1; }

    Vector<uint32_t> firstIndexOf;
    Vector<uint32_t> longestFirst;
};

// An index that takes long is a chunk of its own.
JS_EXPORT_PRIVATE ChunksOfIndices divideIntoChunks(unsigned count, std::span<const uint32_t> longestFirst);

// What is collected comes in the order of the indices, whatever the order they are visited in.
template<typename T>
Vector<Vector<T>> collectChunksInParallel(const ParallelLoops& loops, unsigned count, std::span<const uint32_t> longestFirst, const Invocable<void(unsigned, Vector<T>&)> auto& collect)
{
    ChunksOfIndices division = divideIntoChunks(count, longestFirst);
    Vector<Vector<T>> chunks(division.size());
    forEachIndexInParallel(loops, division.size(), division.longestFirst.span(), [&](unsigned chunk) {
        for (unsigned index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index)
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
    AOT_THRESHOLD(size_t, fewestChunksFreedOnPool, 64);
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

JS_EXPORT_PRIVATE void sortRunsAndMerge(const ParallelLoops&, const SortSteps&, char* list, size_t count);

template<typename T, typename Less>
void sortOnPool(const ParallelLoops& loops, std::span<T> list, const Less& isLess)
{
    size_t count = list.size();
    if constexpr (sortsAsBytes<T>) {
        std::decay_t<Less> order = isLess;
        std::identity identity;
        sortRunsAndMerge(loops, MergingSteps<PartitioningSteps<T, std::decay_t<Less>, std::identity>>(order, identity), reinterpret_cast<char*>(list.data()), count);
    } else {
        constexpr size_t numberOfRuns = 16;
        Vector<T> scratchBuffer = createVectorWithCapacityInParallel<T>(loops, count);
        scratchBuffer.grow(count);
        std::span<T> from = list;
        std::span<T> to = scratchBuffer.mutableSpan();
        auto runOf = [&](std::span<T> all, size_t run, size_t runs) {
            size_t start = count * run / runs;
            return all.subspan(start, count * (run + 1) / runs - start);
        };
        forEachIndexInParallel(loops, numberOfRuns, [&](unsigned run) {
            AOT::sort(runOf(from, run, numberOfRuns), isLess);
        });
        // The last round merges two runs: one thread would go through the whole list, and two through the round in front of it.
        constexpr size_t partsPerRound = 4 * numberOfRuns;
        for (size_t runs = numberOfRuns; runs > 1; runs /= 2) {
            size_t partsPerMerge = partsPerRound / (runs / 2);
            forEachIndexInParallel(loops, partsPerRound, [&](unsigned index) {
                size_t pair = index / partsPerMerge;
                std::span<T> first = runOf(from, 2 * pair, runs);
                std::span<T> second = runOf(from, 2 * pair + 1, runs);
                auto beginOf = [&](size_t part) {
                    if (part == partsPerMerge)
                        return std::pair { first.size(), second.size() };
                    size_t inFirst = first.size() * part / partsPerMerge;
                    size_t inSecond = part && !first.empty() ? static_cast<size_t>(std::ranges::lower_bound(second, first[inFirst], isLess) - second.begin()) : 0;
                    return std::pair { inFirst, inSecond };
                };
                auto [firstBegin, secondBegin] = beginOf(index % partsPerMerge);
                auto [firstEnd, secondEnd] = beginOf(index % partsPerMerge + 1);
                std::ranges::merge(first.subspan(firstBegin, firstEnd - firstBegin), second.subspan(secondBegin, secondEnd - secondBegin), runOf(to, pair, runs / 2).begin() + (firstBegin + secondBegin), isLess);
            });
            std::swap(from, to);
        }
        static_assert(numberOfRuns == 16, "an even number of rounds of merging leaves the result in the list");
    }
}

template<typename T, typename Less>
void parallelSort(const ParallelLoops& loops, std::span<T> list, const Less& isLess)
{
    AOT_THRESHOLD(size_t, fewestForPool, 1 << 13);
    if (list.size() < fewestForPool) {
        AOT::sort(list, isLess);
        return;
    }
    sortOnPool(loops, list, isLess);
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
