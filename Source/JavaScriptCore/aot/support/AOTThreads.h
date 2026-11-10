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
#include <wtf/Atomics.h>
#include <wtf/Condition.h>
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

struct HostThreadPool {
    void (*schedule)(void (*run)(void* argument), void* argument);
};

class BytesSharedByThreads {
public:
    BytesSharedByThreads() = default;
    explicit BytesSharedByThreads(size_t size) { m_bytes.fill(0, size); }

    size_t size() const { return m_bytes.size(); }
    uint8_t at(size_t index) const { return WTF::atomicLoad(&m_bytes[index], std::memory_order_relaxed); }
    void set(size_t index, uint8_t value) { WTF::atomicStore(&m_bytes[index], value, std::memory_order_relaxed); }

private:
    mutable Vector<uint8_t> m_bytes;
};

class ParallelLoops {
    WTF_MAKE_NONCOPYABLE(ParallelLoops);
    WTF_MAKE_NONMOVABLE(ParallelLoops);
public:
    static constexpr size_t workerStackSize = 64 * MB;

    JS_EXPORT_PRIVATE explicit ParallelLoops(unsigned width, const HostThreadPool* = nullptr);

    unsigned width() const { return m_width; }
    const HostThreadPool* hostPool() const { return m_hostPool; }

    JS_EXPORT_PRIVATE void schedule(void (*run)(void* argument), void* argument) const;

    JS_EXPORT_PRIVATE static bool isInsideLoop();

    JS_EXPORT_PRIVATE const ParallelLoops& ofCurrentThread() const;
    bool areThoseOfCurrentThread() const { return this == &m_loopsOfBuild->ofCurrentThread(); }

private:
    friend class Lanes;

    enum LaneTag { ForLane };
    ParallelLoops(LaneTag, const ParallelLoops& loopsOfStarter);

    const unsigned m_width;
    const HostThreadPool* const m_hostPool;
    const ParallelLoops* const m_loopsOfBuild;
    RefPtr<WorkerPool> m_ownPool;
};

JS_EXPORT_PRIVATE void runOnBuildThread(NOESCAPE const ScopedLambda<void()>&);

JS_EXPORT_PRIVATE void forEachIndexInParallel(const ParallelLoops&, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index)>&);
JS_EXPORT_PRIVATE void forEachIndexInParallel(const ParallelLoops&, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index, unsigned worker)>&);
JS_EXPORT_PRIVATE void runTasksInParallel(const ParallelLoops&, std::initializer_list<ScopedLambda<void()>>);

extern JS_EXPORT_PRIVATE constinit thread_local void* t_whatHelpersInherit;

JS_EXPORT_PRIVATE void runLanes(const ParallelLoops&, unsigned count, NOESCAPE const ScopedLambda<void(unsigned lane, const ParallelLoops& loopsOfLane)>&);
JS_EXPORT_PRIVATE void runLanes(const ParallelLoops&, std::initializer_list<ScopedLambda<void()>>);

class BackgroundJobs final : public ThreadSafeRefCounted<BackgroundJobs> {
public:
    using Job = Function<void(const ParallelLoops&)>;

    JS_EXPORT_PRIVATE static Ref<BackgroundJobs> create(const ParallelLoops&);

    JS_EXPORT_PRIVATE void add(Job&&);

    JS_EXPORT_PRIVATE size_t numberOfJobsWaiting() const;

    JS_EXPORT_PRIVATE void waitUntilAllHaveRun();

private:
    explicit BackgroundJobs(const ParallelLoops&);

    void runJobs();

    const ParallelLoops* const m_buildLoops;
    const HostThreadPool m_hostPool;
    const ParallelLoops m_jobLoops;
    mutable Lock m_lock;
    Deque<Job> m_jobs WTF_GUARDED_BY_LOCK(m_lock);
    bool m_isRunning WTF_GUARDED_BY_LOCK(m_lock) { false };
    Condition m_allHaveRun;
};

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

    Vector<unsigned> firstIndexOf;
};

JS_EXPORT_PRIVATE ChunksOfIndices divideIntoChunks(unsigned count);

template<typename T>
Vector<Vector<T>> collectChunksInParallel(const ParallelLoops& loops, unsigned count, const Invocable<void(unsigned, Vector<T>&)> auto& collect)
{
    ChunksOfIndices division = divideIntoChunks(count);
    Vector<Vector<T>> chunks(division.size());
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk) {
        Vector<T>& collected = chunks[chunk];
        unsigned end = division.firstIndexOf[chunk + 1];
        for (unsigned index = division.firstIndexOf[chunk]; index < end; ++index)
            collect(index, collected);
    });
    return chunks;
}

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

static constexpr size_t indicesInRangeOfLoop = 4096;
constexpr size_t numberOfRangesOfIndices(size_t count) { return (count + indicesInRangeOfLoop - 1) / indicesInRangeOfLoop; }

void forEachRangeOfIndicesInParallel(const ParallelLoops& loops, size_t count, const Invocable<void(size_t first, size_t end)> auto& functor)
{
    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfRangesOfIndices(count)), [&](unsigned range) {
        size_t first = static_cast<size_t>(range) * indicesInRangeOfLoop;
        functor(first, std::min(count, first + indicesInRangeOfLoop));
    });
}

template<typename T, typename Less>
void sortOnPool(const ParallelLoops& loops, std::span<T> list, const Less& isLess)
{
    size_t count = list.size();
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
