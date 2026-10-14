/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedThreads.h"

#if ENABLE(AOT)

#include "Options.h"
#include <atomic>
#include <wtf/NeverDestroyed.h>
#include <wtf/NumberOfCores.h>
#include <wtf/ParallelHelperPool.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

bool saysClocks()
{
    static const bool result = Options::verboseAOTCompilation();
    return result;
}

static ParallelHelperPool& helpers()
{
    static NeverDestroyed<Ref<ParallelHelperPool>> pool = [] {
        Ref pool = ParallelHelperPool::create("AOT optimizer"_s);
        pool->ensureThreads(WTF::numberOfProcessorCores() - 1);
        return pool;
    }();
    return pool.get().get();
}

void forEachIndexInParallel(unsigned count, NOESCAPE const ScopedLambda<void(unsigned)>& functor)
{
    constexpr unsigned longestRun = 256;
    constexpr unsigned wordsOfSlot = 16;
    constexpr uint64_t moveBegins = 1;
    constexpr uint64_t moveEnds = 1ull << 32;
    const unsigned numberOfSlots = WTF::numberOfProcessorCores();
    const unsigned shares = 4 * numberOfSlots;
    Vector<std::atomic<uint64_t>> slots(count < 2 ? 0 : numberOfSlots * wordsOfSlot);
    for (unsigned k = 0; k < slots.size(); k += wordsOfSlot)
        slots[k].store(0);
    alignas(128) std::atomic<unsigned> next { 0 };
    alignas(128) std::atomic<uint64_t> moves { 0 };
    alignas(128) std::atomic<unsigned> numberOfThreads { 0 };
    auto firstOf = [](uint64_t range) { return static_cast<unsigned>(range); };
    auto endOf = [](uint64_t range) { return static_cast<unsigned>(range >> 32); };
    auto rangeOf = [](unsigned first, unsigned end) { return static_cast<uint64_t>(end) << 32 | first; };
    auto work = [&] {
        if (count < 2) {
            if (count)
                functor(0);
            return;
        }
        unsigned self = numberOfThreads++;
        if (self >= numberOfSlots)
            return;
        auto& own = slots[self * wordsOfSlot];
        unsigned run = 1;
        for (;;) {
            for (uint64_t range = own.load(); firstOf(range) < endOf(range);) {
                if (!own.compare_exchange_weak(range, range + 1))
                    continue;
                functor(firstOf(range));
                range = own.load();
            }
            uint64_t movesBefore = moves.load();
            unsigned taken = count;
            if (unsigned first = next.load(); first < count) {
                moves.fetch_add(moveBegins);
                while (first < count && taken == count) {
                    unsigned end = first + std::max(std::min(run, (count - first) / shares), 1u);
                    if (!next.compare_exchange_weak(first, end))
                        continue;
                    own.store(rangeOf(first + 1, end));
                    run = std::min(run * 2, longestRun);
                    taken = first;
                }
                moves.fetch_add(moveEnds);
            }
            for (unsigned k = 1; k < numberOfSlots && taken == count; ++k) {
                auto& other = slots[(self + k) % numberOfSlots * wordsOfSlot];
                uint64_t range = other.load();
                if (firstOf(range) >= endOf(range))
                    continue;
                moves.fetch_add(moveBegins);
                while (firstOf(range) < endOf(range) && taken == count) {
                    unsigned middle = firstOf(range) + (endOf(range) - firstOf(range)) / 2;
                    if (!other.compare_exchange_weak(range, rangeOf(firstOf(range), middle)))
                        continue;
                    own.store(rangeOf(middle + 1, endOf(range)));
                    taken = middle;
                }
                moves.fetch_add(moveEnds);
            }
            if (taken != count) {
                functor(taken);
                continue;
            }
            if (firstOf(movesBefore) == endOf(movesBefore) && moves.load() == movesBefore)
                return;
            Thread::yield();
        }
    };
    if (count < 2)
        return work();
    ParallelHelperClient client(&helpers());
    client.runFunctionInParallel(work);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
