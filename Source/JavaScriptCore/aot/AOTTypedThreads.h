/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <span>
#include <wtf/MonotonicTime.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

void forEachIndexInParallel(unsigned count, NOESCAPE const ScopedLambda<void(unsigned)>&);

bool shouldPrintTimings();

class ParallelLoopScope {
public:
    ParallelLoopScope();
    ~ParallelLoopScope();

private:
    Seconds m_start;
    bool m_isOutermost { false };
};

class PhaseTimes {
public:
    explicit PhaseTimes(ASCIILiteral stage);
    void didFinish(ASCIILiteral phase);

private:
    ASCIILiteral m_stage;
    MonotonicTime m_wallTime;
    Seconds m_cpuTimeOutsideParallelLoops;
};

template<typename T>
Vector<T> collectInParallel(unsigned count, const Invocable<void(unsigned, Vector<T>&)> auto& collect)
{
    constexpr unsigned chunkSize = 256;
    Vector<Vector<T>> chunks((count + chunkSize - 1) / chunkSize);
    forEachIndexInParallel(chunks.size(), [&](unsigned chunk) {
        unsigned end = std::min(count, (chunk + 1) * chunkSize);
        for (unsigned index = chunk * chunkSize; index < end; ++index)
            collect(index, chunks[chunk]);
    });
    size_t size = 0;
    for (auto& chunk : chunks)
        size += chunk.size();
    Vector<T> result;
    result.reserveInitialCapacity(size);
    for (auto& chunk : chunks)
        result.appendVector(WTF::move(chunk));
    return result;
}

Vector<uint32_t> findIndicesInParallel(unsigned count, const Invocable<bool(unsigned)> auto& matches)
{
    return collectInParallel<uint32_t>(count, [&](unsigned index, Vector<uint32_t>& found) {
        if (matches(index))
            found.append(index);
    });
}

template<typename T, typename Less>
void sortOnPool(std::span<T> list, const Less& isAhead)
{
    constexpr size_t numberOfRuns = 16;
    constexpr size_t fewestForPool = 1 << 13;
    size_t count = list.size();
    if (count < fewestForPool) {
        std::ranges::sort(list, isAhead);
        return;
    }
    Vector<T> room(count);
    std::span<T> from = list;
    std::span<T> to = room.mutableSpan();
    auto runOf = [&](std::span<T> all, size_t run, size_t runs) {
        size_t start = count * run / runs;
        return all.subspan(start, count * (run + 1) / runs - start);
    };
    forEachIndexInParallel(numberOfRuns, [&](unsigned run) {
        std::ranges::sort(runOf(from, run, numberOfRuns), isAhead);
    });
    for (size_t runs = numberOfRuns; runs > 1; runs /= 2) {
        forEachIndexInParallel(runs / 2, [&](unsigned pair) {
            std::ranges::merge(runOf(from, 2 * pair, runs), runOf(from, 2 * pair + 1, runs), runOf(to, pair, runs / 2).begin(), isAhead);
        });
        std::swap(from, to);
    }
    static_assert(numberOfRuns == 16, "an even number of rounds of merging leaves the result in the list");
}

template<typename T>
void destroyOnHelperThread(T&& value)
{
    static_assert(!std::is_lvalue_reference_v<T>);
    Thread::create("AOT destroyer"_s, [value = WTF::move(value)]() mutable {
        T gone = WTF::move(value);
    })->detach();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
