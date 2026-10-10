/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <atomic>
#include <wtf/NeverDestroyed.h>
#include <wtf/NumberOfCores.h>
#include <wtf/ParallelHelperPool.h>

namespace JSC { namespace AOT { namespace Typed {

inline ParallelHelperPool& helpers()
{
    static NeverDestroyed<Ref<ParallelHelperPool>> pool = [] {
        Ref pool = ParallelHelperPool::create("AOT optimizer"_s);
        pool->ensureThreads(WTF::numberOfProcessorCores() - 1);
        return pool;
    }();
    return pool.get().get();
}

// What is done for one index writes nothing that is read or written for another, so the outcome does not depend on the threads.
// A helper that has not woken by the time the indices are gone is not waited for.
template<typename Functor>
void forEachIndexInParallel(unsigned count, const Functor& functor)
{
    std::atomic<unsigned> next { 0 };
    auto work = [&] {
        for (unsigned i = next++; i < count; i = next++)
            functor(i);
    };
    if (count < 2)
        return work();
    ParallelHelperClient client(&helpers());
    client.runFunctionInParallel(work);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
