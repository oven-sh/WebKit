/*
 * Copyright (c) 2026 Anthropic, PBC. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "TestHarness.h"

#if PAS_ENABLE_ISO

#include "iso_heap.h"
#include "pas_allocator_scavenge_action.h"
#include "pas_deallocator_scavenge_action.h"
#include "pas_scavenger.h"
#include "pas_thread_local_cache.h"
#include "pas_thread_suspender.h"
#if PAS_OS(DARWIN)
#include <mach/mach.h>
#endif
#include <atomic>
#include <chrono>
#include <thread>

using namespace std;

namespace {

void* const sentinel = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234567812345678ull));

atomic<unsigned> currentThreadCalls;
atomic<unsigned> beginSuspendCalls;
atomic<unsigned> endSuspendCalls;
atomic<void*> lastSuspendHandle;

pas_embedder_thread_handle mockCurrentThread()
{
    currentThreadCalls.fetch_add(1);
    return sentinel;
}

bool mockBeginSuspend(pas_embedder_thread_handle handle)
{
    lastSuspendHandle.store(handle);
    beginSuspendCalls.fetch_add(1);
    return true;
}

void mockEndSuspend(pas_embedder_thread_handle handle)
{
    CHECK_EQUAL(handle, lastSuspendHandle.load());
    endSuspendCalls.fetch_add(1);
}

const pas_thread_suspender mockSuspender = {
    mockCurrentThread,
    mockBeginSuspend,
    mockEndSuspend,
    nullptr
};

void testInstallAndHandleStorage()
{
    pas_scavenger_suspend();
    pas_install_thread_suspender(&mockSuspender);

    currentThreadCalls.store(0);

    void* observedHandle = nullptr;
    thread t([&] {
        void* p = iso_allocate_common_primitive(64, pas_non_compact_allocation_mode);
        CHECK(p);
        pas_thread_local_cache* cache = pas_thread_local_cache_try_get();
        CHECK(cache);
        observedHandle = cache->embedder_thread_handle;
    });
    t.join();

    CHECK_GREATER_EQUAL(currentThreadCalls.load(), 1u);
    CHECK_EQUAL(observedHandle, sentinel);
}

// The mock doesn't actually freeze the target, so it must be a real suspension to make
// stop_allocator's is_in_use read safe. On Darwin we wrap Mach so the freeze is real and
// the test still drives the embedder code path; on Linux this test only runs once the
// real WTF-backed suspender is wired in via testForceStopUsesEmbedder's #if guard.
#if PAS_OS(DARWIN)
bool realBeginSuspend(pas_embedder_thread_handle handle)
{
    lastSuspendHandle.store(handle);
    beginSuspendCalls.fetch_add(1);
    return thread_suspend(pthread_mach_thread_np(reinterpret_cast<pthread_t>(handle)))
        == KERN_SUCCESS;
}

void realEndSuspend(pas_embedder_thread_handle handle)
{
    CHECK_EQUAL(handle, lastSuspendHandle.load());
    endSuspendCalls.fetch_add(1);
    thread_resume(pthread_mach_thread_np(reinterpret_cast<pthread_t>(handle)));
}

pas_embedder_thread_handle realCurrentThread()
{
    currentThreadCalls.fetch_add(1);
    return reinterpret_cast<pas_embedder_thread_handle>(pthread_self());
}

const pas_thread_suspender realSuspender = {
    realCurrentThread,
    realBeginSuspend,
    realEndSuspend,
    nullptr
};
#endif

void testForceStopUsesEmbedder()
{
    pas_scavenger_suspend();
#if PAS_OS(DARWIN)
    pas_install_thread_suspender(&realSuspender);
    pas_thread_suspender_override_native = true;
#else
    pas_install_thread_suspender(&mockSuspender);
#endif

    beginSuspendCalls.store(0);
    endSuspendCalls.store(0);

    atomic<bool> threadReady{false};
    atomic<bool> threadShouldExit{false};

    thread t([&] {
        for (unsigned i = 0; i < 4; ++i) {
            void* p = iso_allocate_common_primitive(64u << i, pas_non_compact_allocation_mode);
            CHECK(p);
        }
        threadReady.store(true);
        while (!threadShouldExit.load())
            this_thread::sleep_for(chrono::milliseconds(1));
    });

    while (!threadReady.load())
        this_thread::sleep_for(chrono::milliseconds(1));

    pas_thread_local_cache_for_all(
        pas_allocator_scavenge_force_stop_action,
        pas_deallocator_scavenge_no_action,
        pas_thread_local_cache_decommit_no_action);

    CHECK_GREATER_EQUAL(beginSuspendCalls.load(), 1u);
    CHECK_EQUAL(beginSuspendCalls.load(), endSuspendCalls.load());

    threadShouldExit.store(true);
    t.join();
}

} // anonymous namespace

#endif // PAS_ENABLE_ISO

void addThreadSuspenderTests()
{
#if PAS_ENABLE_ISO
    ADD_TEST(testInstallAndHandleStorage());
    ADD_TEST(testForceStopUsesEmbedder());
#endif // PAS_ENABLE_ISO
}
