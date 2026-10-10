/*
 * Copyright (C) 2026 Peter Steinberger <steipete@gmail.com>. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

// Verify that a passive collector progresses when its mutator starts waiting.
// Observe existing engine state without production hooks or state mutation.
#include "config.h"

#include "APICast.h"
#include "Collector.h"
#include "GCRequest.h"
#include "Heap.h"
#include "HeapHelperPool.h"
#include "HeapInlines.h"
#include "InitializeThreading.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "JavaScript.h"
#include "Options.h"
#include "VM.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <wtf/Atomics.h>
#include <wtf/Condition.h>
#include <wtf/MainThread.h>
#include <wtf/ParallelHelperPool.h>
#include <wtf/ParkingLot.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

namespace {

// Explicit-instantiation arguments are exempt from member access checks. These
// probe-only accessors expose observation addresses, never mutate heap state.
template<typename Tag, typename Tag::Type member>
struct ObserveMember {
    friend typename Tag::Type observe(Tag) { return member; }
};

struct CollectorCondition {
    using Type = WTF::Condition JSC::Collector::*;
    friend Type observe(CollectorCondition);
};
template struct ObserveMember<CollectorCondition, &JSC::Collector::m_markingConditionVariable>;

struct HeapWorldState {
    using Type = WTF::Atomic<unsigned> JSC::Heap::*;
    friend Type observe(HeapWorldState);
};
template struct ObserveMember<HeapWorldState, &JSC::Heap::m_worldState>;

struct ConditionWaitAddress {
    using Type = WTF::Atomic<bool> WTF::Condition::*;
    friend Type observe(ConditionWaitAddress);
};
template struct ObserveMember<ConditionWaitAddress, &WTF::Condition::m_hasWaiters>;

template<typename Tag, unsigned value>
struct ObserveConstant {
    friend unsigned observe(Tag) { return value; }
};
struct MutatorWaitingBit {
    friend unsigned observe(MutatorWaitingBit);
};
template struct ObserveConstant<MutatorWaitingBit, JSC::Heap::mutatorWaitingBit>;

class HeldHelpers {
public:
    void enter()
    {
        std::unique_lock lock(m_mutex);
        ++m_entered;
        m_changed.notify_all();
        m_changed.wait(lock, [&] {
            return m_released;
        });
    }

    bool waitUntilEntered(unsigned expected)
    {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return m_entered == expected;
        });
    }

    void release()
    {
        std::lock_guard lock(m_mutex);
        m_released = true;
        m_changed.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    unsigned m_entered { 0 };
    bool m_released { false };
};

struct Parked {
    bool collector { false };
    bool mutator { false };
    uintptr_t collectorThread { 0 };
    uintptr_t mutatorThread { 0 };
};

Parked inspectParking(const void* conditionAddress, const void* worldAddress, uintptr_t expectedMutator)
{
    Parked result;
    // The callback runs under ParkingLot locks: do not allocate, park, or log.
    WTF::ParkingLot::forEach([&](uintptr_t thread, const void* address) {
        if (address == conditionAddress) {
            result.collector = true;
            result.collectorThread = thread;
        }
        if (address == worldAddress && thread == expectedMutator) {
            result.mutator = true;
            result.mutatorThread = thread;
        }
    });
    return result;
}

bool evaluate(JSGlobalContextRef context, const char* source, double* number = nullptr)
{
    JSStringRef script = JSStringCreateWithUTF8CString(source);
    JSValueRef exception = nullptr;
    JSValueRef value = JSEvaluateScript(context, script, nullptr, nullptr, 1, &exception);
    JSStringRelease(script);
    if (exception || !value)
        return false;
    if (number)
        *number = JSValueToNumber(context, value, &exception);
    return !exception;
}

} // namespace

int main()
{
    WTF::initializeMainThread();
    JSC::initialize();
    // Zero pause and utilization=1 force concurrent scheduling without an RNG
    // outcome or allocation-time threshold. These are probe settings only.
    if (!JSC::Options::setOptions("useConcurrentGC=true useStochasticMutatorScheduler=true minimumGCPauseMS=0 gcPauseScale=0 minimumMutatorUtilization=1 maximumMutatorUtilization=1 numberOfGCMarkers=2")) {
        SAFE_FPRINTF(stderr, "probe options rejected\n");
        return 2;
    }
    JSGlobalContextRef context = JSGlobalContextCreate(nullptr);
    if (!context || !evaluate(context, "globalThis.gcProgressRoots = Array.from({length: 32768}, (_, i) => ({index:i, child:{value:i}})); gcProgressRoots.length")) {
        SAFE_FPRINTF(stderr, "probe heap setup failed\n");
        return 2;
    }

    auto& vm = toJS(context)->vm();
    bool foundPassiveCollector = false;
    bool foundLateWaitStall = false;
    bool timedOut = false;
    bool rootsSurvived = false;
    bool completedBeforeHelperRelease = false;
    unsigned helperCount = 0;
    unsigned stalledWorldState = 0;
    Parked stalledThreads;
    const void* conditionAddress = nullptr;
    const void* worldAddress = nullptr;
    double elapsedMs = 0;
    {
        JSC::JSLockHolder lock(vm);
        auto& heap = vm.heap;
        // Settle setup collection while helpers are available. No prior cycle or
        // unrelated VM is allowed to account for the later observed wait.
        heap.collectNow(JSC::Sync, JSC::CollectionScope::Full);
        auto& condition = heap.collector().*observe(CollectorCondition { });
        auto& world = heap.*observe(HeapWorldState { });
        conditionAddress = &(condition.*observe(ConditionWaitAddress { }));
        worldAddress = &world;
        const unsigned waitingBit = observe(MutatorWaitingBit { });
        const uintptr_t mutatorParkingThread = WTF::ParkingLot::currentThreadID();

        WTF::RefPtr<WTF::ParallelHelperPool> sharedPool = &JSC::heapHelperPool();
        helperCount = sharedPool->numberOfThreads();
        if (!helperCount) {
            SAFE_FPRINTF(stderr, "probe requires parallel GC helpers\n");
            return 2;
        }
        HeldHelpers gate;
        WTF::ParallelHelperClient blocker(WTF::move(sharedPool));
        blocker.setFunction([&] {
            gate.enter();
        });
        if (!gate.waitUntilEntered(helperCount)) {
            gate.release();
            blocker.finish();
            SAFE_FPRINTF(stderr, "could not occupy every shared helper\n");
            return 2;
        }

        std::atomic<bool> invokeWait { false };
        std::atomic<bool> completed { false };
        std::atomic<bool> giveUp { false };
        auto begin = std::chrono::steady_clock::now();
        std::thread observer([&] {
            const auto deadline = begin + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                auto parked = inspectParking(conditionAddress, worldAddress, mutatorParkingThread);
                if (!foundPassiveCollector && parked.collector && !(world.load() & waitingBit)) {
                    foundPassiveCollector = true;
                    invokeWait.store(true, std::memory_order_release);
                }
                if (completed.load(std::memory_order_acquire)) {
                    completedBeforeHelperRelease = true;
                    gate.release();
                    return;
                }
                if (foundPassiveCollector && parked.collector && parked.mutator && (world.load() & waitingBit)) {
                    // This is an observed wait cycle, not a timing inference.
                    foundLateWaitStall = true;
                    stalledThreads = parked;
                    stalledWorldState = world.load();
                    gate.release(); // Recovery is not counted as successful proof.
                    return;
                }
                std::this_thread::yield();
            }
            timedOut = true;
            giveUp.store(true, std::memory_order_release);
            gate.release();
        });

        heap.collectAsync(JSC::CollectionScope::Full);
        // This thread retains real mutator heap access. Service the engine's
        // stop handshake until the observer sees the collector actually parked.
        while (!invokeWait.load(std::memory_order_acquire) && !giveUp.load(std::memory_order_acquire)) {
            heap.stopIfNecessary();
            heap.relinquishConn();
            std::this_thread::yield();
        }
        if (invokeWait.load(std::memory_order_acquire)) {
            heap.preventCollection();
            heap.allowCollection();
            completed.store(true, std::memory_order_release);
        }
        observer.join();
        blocker.finish();
        // Also settle the failure/control recovery before releasing the VM.
        heap.collectNow(JSC::Sync, JSC::CollectionScope::Full);
        double checksum = 0;
        rootsSurvived = evaluate(context, "gcProgressRoots.length + gcProgressRoots[32767].child.value", &checksum)
            && checksum == 65535;
        elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        // Final VM destruction must retain the API lock and its active atom string table.
        JSGlobalContextRelease(context);
    }
    const bool passed = foundPassiveCollector && completedBeforeHelperRelease && !foundLateWaitStall && !timedOut && rootsSurvived;
    SAFE_PRINTF("{\"passed\":%s,\"passiveCollectorObserved\":%s,\"lateWaitStallObserved\":%s,\"completedBeforeHelperRelease\":%s,\"timedOut\":%s,\"rootsSurvived\":%s,\"helperCount\":%u,\"worldStateAtStall\":%u,\"collectorParkingThreadId\":%llu,\"mutatorParkingThreadId\":%llu,\"conditionAddress\":\"%p\",\"worldAddress\":\"%p\",\"elapsedMs\":%.3f}\n",
        passed ? "true"_s : "false"_s, foundPassiveCollector ? "true"_s : "false"_s, foundLateWaitStall ? "true"_s : "false"_s,
        completedBeforeHelperRelease ? "true"_s : "false"_s, timedOut ? "true"_s : "false"_s, rootsSurvived ? "true"_s : "false"_s,
        helperCount, stalledWorldState, static_cast<unsigned long long>(stalledThreads.collectorThread),
        static_cast<unsigned long long>(stalledThreads.mutatorThread), conditionAddress, worldAddress, elapsedMs);
    return passed ? 0 : foundLateWaitStall && rootsSurvived ? 1 : 2;
}
