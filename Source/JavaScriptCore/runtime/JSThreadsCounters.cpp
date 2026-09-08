/*
 * Copyright (C) 2026 the WebKit project authors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSThreadsCounters.h"

#include "ConcurrentButterfly.h"

#include <mutex>
#include <string.h>
#include <wtf/DataLog.h>
#include <wtf/NeverDestroyed.h>

namespace JSC {

JSThreadsCounters& JSThreadsCounters::singleton()
{
    static LazyNeverDestroyed<JSThreadsCounters> counters;
    static std::once_flag once;
    std::call_once(once, [] { counters.construct(); });
    return counters.get();
}

void JSThreadsCounters::countNamed(const char* name, uint64_t nanoseconds)
{
    if (!enabled())
        return;
    auto& c = singleton();
    if (!name)
        name = "(no context)";
    for (unsigned i = 0; i < namedSlots; ++i) {
        const char* existing = c.named[i].name.load(std::memory_order_acquire);
        if (!existing) {
            const char* expected = nullptr;
            if (!c.named[i].name.compare_exchange_strong(expected, name) && expected != name && strcmp(expected, name))
                continue;
            existing = name;
        }
        if (existing == name || !strcmp(existing, name)) {
            c.named[i].count.fetch_add(1, std::memory_order_relaxed);
            c.named[i].nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
            return;
        }
    }
    c.named[namedSlots - 1].count.fetch_add(1, std::memory_order_relaxed); // Overflow bucket.
}

uint64_t JSThreadsCounters::valueByName(const char* name)
{
    auto& c = singleton();
#define JSTHREADS_COUNTER_BY_NAME(field) \
    if (!strcmp(name, #field)) \
        return c.field.load(std::memory_order_relaxed);
    FOR_EACH_JSTHREADS_COUNTER(JSTHREADS_COUNTER_BY_NAME)
#undef JSTHREADS_COUNTER_BY_NAME
    for (unsigned i = 0; i < namedSlots; ++i) {
        const char* existing = c.named[i].name.load(std::memory_order_acquire);
        if (existing && !strcmp(existing, name))
            return c.named[i].count.load(std::memory_order_relaxed);
    }
    return 0;
}

void JSThreadsCounters::dump()
{
    auto& c = singleton();
    dataLogLn("JSThreads counters:");
    for (unsigned i = 0; i < namedSlots; ++i) {
        const char* name = c.named[i].name.load();
        if (!name)
            break;
        dataLogLn("  stw[", name, "]: ", c.named[i].count.load(), " (", c.named[i].nanoseconds.load() / 1e6, " ms)");
    }
#define JSTHREADS_COUNTER_DUMP(name) \
    if (uint64_t value = c.name.load(std::memory_order_relaxed)) \
        dataLogLn("  ", #name, ": ", value);
    FOR_EACH_JSTHREADS_COUNTER(JSTHREADS_COUNTER_DUMP)
#undef JSTHREADS_COUNTER_DUMP
    dataLogLn("  lockedTransition(total): ", lockedTransitionCount());
    dataLogLn("  parkMs: ", c.parkNanoseconds.load() / 1e6);
    dataLogLn("  stwMs: ", c.stwNanoseconds.load() / 1e6);
    dataLogLn("  gcStoppedMs: ", c.gcStoppedNanoseconds.load() / 1e6);
    dataLogLn("  gcSyncFullSweepMs: ", c.gcSyncFullSweepNanoseconds.load() / 1e6);
}

void JSThreadsCounters::registerDumpAtExit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (Options::reportJSThreadsCounters())
            std::atexit([] { JSThreadsCounters::dump(); });
    });
}

} // namespace JSC
