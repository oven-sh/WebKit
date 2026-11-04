/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <span>
#include <utility>
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/RefPtr.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WorkerPool.h>

namespace JSC { namespace AOT {

// The thread pool of the program that the engine is part of. Its threads have at least half of ParallelLoops::workerStackSize. What needs more than that goes through runOnBuildThread().
// The function is called, and what it was handed runs, also when the build that was given the pool has returned: BackgroundJobs. So the pool lives as long as the process.
struct HostThreadPool {
    // Calls run(argument) once, on a thread of the pool, and does not wait for it.
    void (*schedule)(void (*run)(void* argument), void* argument);
};

// Only one thread at a time may start a loop that is not nested in another loop of the same ParallelLoops.
class ParallelLoops {
    WTF_MAKE_NONCOPYABLE(ParallelLoops);
    WTF_MAKE_NONMOVABLE(ParallelLoops);
public:
    static constexpr size_t workerStackSize = 8 * MB;

    // Without a host pool, the loops run on a pool of their own.
    JS_EXPORT_PRIVATE explicit ParallelLoops(unsigned width, const HostThreadPool* = nullptr);

    unsigned width() const { return m_width; }
    const HostThreadPool* hostPool() const { return m_hostPool; }

    // The width is above one.
    JS_EXPORT_PRIVATE void schedule(void (*run)(void* argument), void* argument) const;

    // Whether the current thread is at one of several indices of a loop or at one of several tasks, whatever the width.
    JS_EXPORT_PRIVATE static bool isInsideLoop();

    // The loops that the current thread starts its loops on, of these and of those that runLanes() has made for lanes begun on these or in such a lane:
    // those of the loop that the thread is in, so that a helper keeps its worker number in a nested loop. Else those of the lane that the thread runs. Else these.
    JS_EXPORT_PRIVATE const ParallelLoops& ofCurrentThread() const;
    // False for loops that somebody asked for on one thread and starts a loop on in a lane on another.
    bool areThoseOfCurrentThread() const { return this == &m_loopsOfBuild->ofCurrentThread(); }

private:
    friend class Lanes;

    // For a lane that runs beside the thread that starts its loops on `loopsOfStarter`, over the same pool.
    enum LaneTag { ForLane };
    ParallelLoops(LaneTag, const ParallelLoops& loopsOfStarter);

    const unsigned m_width;
    const HostThreadPool* const m_hostPool;
    // Itself, unless runLanes() has made it.
    const ParallelLoops* const m_loopsOfBuild;
    RefPtr<WorkerPool> m_ownPool;
};

// On a new thread, which has a stack of ParallelLoops::workerStackSize and is inside no loop. Returns when the functor has returned.
// Where the caller is inside a loop, the functor starts no loop of the same ParallelLoops: it would be a second loop beside the caller's, and both would hand out the same worker numbers.
// ParallelLoops::ofCurrentThread() answers there what it answers the caller.
JS_EXPORT_PRIVATE void runOnBuildThread(NOESCAPE const ScopedLambda<void()>&);

// The indices are visited in no particular order. A loop started where isInsideLoop() holds runs entirely on the thread that starts it.
JS_EXPORT_PRIVATE void forEachIndexInParallel(const ParallelLoops&, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index)>&);
// `worker` is less than width(). No two threads running loops of the same ParallelLoops at the same time get the same number, so a stage can index its per-thread state by it.
// A thread keeps its number in a loop nested in another loop of the same ParallelLoops. The thread that starts any other loop is worker 0.
JS_EXPORT_PRIVATE void forEachIndexInParallel(const ParallelLoops&, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index, unsigned worker)>&);
JS_EXPORT_PRIVATE void runTasksInParallel(const ParallelLoops&, std::initializer_list<ScopedLambda<void()>>);

// Rows of steps of which none reads what another writes. Unlike a task, a lane starts loops that have all threads.
// The first lane runs on the calling thread, on the loops given. Each other is left to the pool of those loops, the host's or their own, and has loops of its own over that pool, so no two loops of one ParallelLoops run at once.
// What a lane indexes by worker number it makes itself: a number of its loops says nothing beside a number of another lane's.
// A lane that no thread of the pool has begun when the caller has ended its own is the caller's, in the order of the lanes. So nobody waits for a thread that the pool does not have. Returns when every lane has returned.
// At a width of one and where isInsideLoop() holds, the lanes run on the calling thread, one behind the other.
JS_EXPORT_PRIVATE void runLanes(const ParallelLoops&, unsigned count, NOESCAPE const ScopedLambda<void(unsigned lane, const ParallelLoops& loopsOfLane)>&);
// A lane finds its loops through ParallelLoops::ofCurrentThread(), which is what BuildContext::parallelLoops() answers.
JS_EXPORT_PRIVATE void runLanes(const ParallelLoops&, std::initializer_list<ScopedLambda<void()>>);

// Work that nobody waits for: what a stage has left is destroyed while the build goes on, and what is left at the end while the caller of the build goes on.
// The jobs run on the host's pool, one at a time and in the order in which they were added. Each gets loops of its own over that pool.
// A job may run when the build is over, so it owns what it touches and refers to nothing of the build.
// A destructor touches too: that of a TypeArena that keeps InProgram writes to the TypeTable, and TypeTable::freeze() relies on no other thread being at the table.
// So nothing whose destructor reads or writes the context or the program's tables is handed over: its owner resets it on the build thread and hands over the flat lists.
// Without a host pool, and at a width of one, add() runs the job itself, with what the loops that create() was given answer as ofCurrentThread().
class BackgroundJobs final : public ThreadSafeRefCounted<BackgroundJobs> {
public:
    using Job = Function<void(const ParallelLoops&)>;

    JS_EXPORT_PRIVATE static Ref<BackgroundJobs> create(const ParallelLoops&);

    // By a thread that may start a loop of the ParallelLoops that create() was given or of a lane's, while the former live.
    JS_EXPORT_PRIVATE void add(Job&&);

    // Without the job that is running.
    JS_EXPORT_PRIVATE size_t numberOfJobsWaiting() const;

private:
    explicit BackgroundJobs(const ParallelLoops&);

    void runJobs();

    // Null: the jobs run on the host's pool.
    const ParallelLoops* const m_buildLoops;
    // A copy: the build's own may be gone.
    const HostThreadPool m_hostPool;
    const ParallelLoops m_jobLoops;
    mutable Lock m_lock;
    Deque<Job> m_jobs WTF_GUARDED_BY_LOCK(m_lock);
    bool m_isRunning WTF_GUARDED_BY_LOCK(m_lock) { false };
};

// On one thread. What has many parts that each own memory goes through a job of its own, which has loops.
template<typename T>
void destroyInBackground(BackgroundJobs& jobs, T&& value)
{
    static_assert(!std::is_reference_v<T>, "the job owns the value");
    jobs.add([value = WTF::move(value)](const ParallelLoops&) {
        UNUSED_PARAM(value);
    });
}

struct ChunksOfIndices {
    unsigned size() const { return static_cast<unsigned>(firstIndexOf.size() - 1); }

    // [chunk], and one more for the end.
    Vector<unsigned> firstIndexOf;
};

// By the count alone, so the chunks are the same at any number of threads.
JS_EXPORT_PRIVATE ChunksOfIndices divideIntoChunks(unsigned count);

// What is collected comes in the order of the indices, whatever the order they are visited in.
template<typename T>
Vector<Vector<T>> collectChunksInParallel(const ParallelLoops& loops, unsigned count, const Invocable<void(unsigned, Vector<T>&)> auto& collect)
{
    ChunksOfIndices division = divideIntoChunks(count);
    Vector<Vector<T>> chunks(division.size());
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk) {
        for (unsigned index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index)
            collect(index, chunks[chunk]);
    });
    return chunks;
}

// Compiled once for a type of element, not once for everyone who collects.
template<typename T>
NEVER_INLINE Vector<T> joinChunks(const ParallelLoops& loops, Vector<Vector<T>>&& chunks)
{
    size_t size = 0;
    for (auto& chunk : chunks)
        size += chunk.size();
    Vector<T> result;
    result.reserveInitialCapacity(size);
    for (auto& chunk : chunks)
        result.appendVector(WTF::move(chunk));
    static constexpr size_t fewestChunksFreedOnPool = 64;
    if (chunks.size() >= fewestChunksFreedOnPool) {
        forEachIndexInParallel(loops, static_cast<unsigned>(chunks.size()), [&](unsigned chunk) {
            chunks[chunk] = { };
        });
    }
    return result;
}

template<typename T>
Vector<T> collectInParallel(const ParallelLoops& loops, unsigned count, const Invocable<void(unsigned, Vector<T>&)> auto& collect)
{
    return joinChunks<T>(loops, collectChunksInParallel<T>(loops, count, collect));
}

Vector<uint32_t> findIndicesInParallel(const ParallelLoops& loops, unsigned count, const Invocable<bool(unsigned)> auto& matches)
{
    return collectInParallel<uint32_t>(loops, count, [&](unsigned index, Vector<uint32_t>& found) {
        if (matches(index))
            found.append(index);
    });
}

// For a step on each index that is shorter than a call. The ranges go by the count alone, so they are the same at any number of threads.
static constexpr size_t indicesInRangeOfLoop = 4096;
constexpr size_t numberOfRangesOfIndices(size_t count) { return (count + indicesInRangeOfLoop - 1) / indicesInRangeOfLoop; }

// `first` is a multiple of indicesInRangeOfLoop.
void forEachRangeOfIndicesInParallel(const ParallelLoops& loops, size_t count, const Invocable<void(size_t first, size_t end)> auto& functor)
{
    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfRangesOfIndices(count)), [&](unsigned range) {
        size_t first = static_cast<size_t>(range) * indicesInRangeOfLoop;
        functor(first, std::min(count, first + indicesInRangeOfLoop));
    });
}

// Equal elements stay in the order they came in, as in AOT::sort(). The runs go by the size of the list alone.
template<typename T, typename Less>
void sortOnPool(const ParallelLoops& loops, std::span<T> list, const Less& isLess)
{
    size_t count = list.size();
    // A run is sorted by one thread, so a long list has more of them than a machine has threads. A power of four: an even number of rounds of merging leaves the result in the list.
    // However many there are, the result is the same: there is one order that leaves equal elements as they came.
    const size_t numberOfRuns = count < (1 << 17) ? 16 : count < (1 << 21) ? 64 : 256;
    Vector<T> scratchBuffer;
    scratchBuffer.grow(count);
    std::span<T> from = list;
    std::span<T> to = scratchBuffer.mutableSpan();
    auto runOf = [&](std::span<T> all, size_t run, size_t runs) {
        size_t start = count * run / runs;
        return all.subspan(start, count * (run + 1) / runs - start);
    };
    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfRuns), [&](unsigned run) {
        AOT::sortInLine(runOf(from, run, numberOfRuns), isLess);
    });
    // The last round merges two runs: one thread would go through the whole list, and two through the round in front of it.
    const size_t partsPerRound = 4 * numberOfRuns;
    for (size_t runs = numberOfRuns; runs > 1; runs /= 2) {
        size_t partsPerMerge = partsPerRound / (runs / 2);
        forEachIndexInParallel(loops, static_cast<unsigned>(partsPerRound), [&](unsigned index) {
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
}

template<typename T, typename Less>
void parallelSort(const ParallelLoops& loops, std::span<T> list, const Less& isLess)
{
    static constexpr size_t fewestForPool = 1 << 13;
    if (list.size() < fewestForPool) {
        AOT::sortInLine(list, isLess);
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
