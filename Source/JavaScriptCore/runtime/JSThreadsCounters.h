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

#pragma once

#include "Options.h"
#include <atomic>
#include <wtf/MonotonicTime.h>

namespace JSC {

// Diagnostic event counters for the JS-threads cost ledger
// (docs/threads/PERF-RESULTS.md). Off unless --reportJSThreadsCounters=1 (count and
// dump at exit) or --countJSThreadsCounters=1 (count only; tests read them through $vm);
// every site is one predicted-untaken option byte test. Dumped at process
// exit. Not a protocol component: nothing reads a counter for a decision.
#define FOR_EACH_JSTHREADS_COUNTER(v) \
    v(stwRequest) \
    v(stwRequestInline) \
    v(park) \
    v(parkForGC) \
    v(watchpointFireAll) \
    v(f1SharedWriteFire) \
    v(f2ThreadLocalSetFire) \
    v(structureTransition) \
    v(lockedTransition) \
    v(relabelOwnerLeg) \
    v(gcEden) \
    v(gcFull) \
    v(gcSyncFullSweep) \
    v(gcShrink) \
    v(markedBlockAllocated) \
    v(markedBlockFreed) \
    v(compileBaseline) \
    v(compileDFG) \
    v(compileFTL) \
    v(osrExitDFGOperation) \
    v(osrExitDFGCompile) \
    v(osrExitFTLOperation) \
    v(osrExitFTLCompile) \
    v(jettison) \
    v(icGetByIdOptimize) \
    v(icGetByIdGaveUp) \
    v(icGetByIdGeneric) \
    v(icGetByIdMegamorphicMiss) \
    v(icPutByIdOptimize) \
    v(icPutByIdGaveUp) \
    v(icPutByIdMegamorphicMiss) \
    v(icGetByValOptimize) \
    v(icGetByValGaveUp) \
    v(icGetByValGeneric) \
    v(icPutByValGaveUp) \
    v(icInByGaveUp) \
    v(callVirtualSlow) \
    v(callLinkSlow) \
    v(callLinkPolymorphic) \
    v(createThis) \
    v(syncToStopGeneration) \
    v(syncToStopGenerationFenced) \
    v(updateThreadStopRequest) \
    v(gilOffCompilationLock) \
    v(gilOffCompilationLockContended) \
    v(allocateSlowCase) \
    v(allocatorRefillFromTLC) \
    v(writeBarrierSlowPath) \
    v(ropeResolveGILOff) \
    v(atomStringAddSlow) \
    v(mapSetAddGILOff) \
    v(mapReadLockFreeGILOff) \
    v(mapReadLockFreeFallback) \
    v(regExpCompileCheckLocked) \
    v(generatorClaimResume) \
    v(exceptionThrown) \
    v(hashTableIntrinsicRefusedGILOff) \
    v(icDictionaryStructureRefused) \
    v(icFlattenSkippedGILOff) \
    v(icRetryWithoutProgressGaveUp) \
    v(dictionaryFlatten) \
    v(arrayAllocationProfileLeftDoubleGILOff) \
    v(watchpointFireWatcherless) \
    v(deleteLostLaneRestart)

struct JSThreadsCounters {
#define JSTHREADS_COUNTER_FIELD(name) std::atomic<uint64_t> name { 0 };
    FOR_EACH_JSTHREADS_COUNTER(JSTHREADS_COUNTER_FIELD)
#undef JSTHREADS_COUNTER_FIELD
    std::atomic<uint64_t> parkNanoseconds { 0 };
    std::atomic<uint64_t> gcStoppedNanoseconds { 0 };
    std::atomic<uint64_t> gcSyncFullSweepNanoseconds { 0 };
    std::atomic<uint64_t> stwNanoseconds { 0 };

    // Stop-the-world requests by requester description (pointer-keyed:
    // descriptions are string literals).
    static constexpr unsigned namedSlots = 48;
    struct Named {
        std::atomic<const char*> name { nullptr };
        std::atomic<uint64_t> count { 0 };
        std::atomic<uint64_t> nanoseconds { 0 };
    };
    Named named[namedSlots];
    JS_EXPORT_PRIVATE static void countNamed(const char* name, uint64_t nanoseconds);

    JS_EXPORT_PRIVATE static uint64_t valueByName(const char*); // 0 for an unknown name
    JS_EXPORT_PRIVATE static JSThreadsCounters& singleton();
    JS_EXPORT_PRIVATE static void dump();
    JS_EXPORT_PRIVATE static void registerDumpAtExit();
    static bool enabled() { return Options::reportJSThreadsCounters() || Options::countJSThreadsCounters(); }
};

#define JSTHREADS_COUNT(name) do { \
        if (JSC::JSThreadsCounters::enabled()) [[unlikely]] \
            JSC::JSThreadsCounters::singleton().name.fetch_add(1, std::memory_order_relaxed); \
    } while (false)

#define JSTHREADS_COUNT_ADD(name, amount) do { \
        if (JSC::JSThreadsCounters::enabled()) [[unlikely]] \
            JSC::JSThreadsCounters::singleton().name.fetch_add((amount), std::memory_order_relaxed); \
    } while (false)

// RAII: adds the elapsed wall time to a nanosecond counter when enabled.
class JSThreadsCountedDuration {
public:
    explicit JSThreadsCountedDuration(std::atomic<uint64_t>& slot)
        : m_slot(slot)
    {
        if (JSThreadsCounters::enabled()) [[unlikely]]
            m_start = MonotonicTime::now();
    }
    ~JSThreadsCountedDuration()
    {
        if (m_start)
            m_slot.fetch_add(static_cast<uint64_t>((MonotonicTime::now() - m_start).nanoseconds()), std::memory_order_relaxed);
    }
private:
    std::atomic<uint64_t>& m_slot;
    MonotonicTime m_start;
};

} // namespace JSC
