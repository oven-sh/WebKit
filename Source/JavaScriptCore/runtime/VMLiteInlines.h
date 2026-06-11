/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#pragma once

// Inline helpers for the per-thread VMLite carriers (SPEC-vmstate §6.3/§6.5).
//
// Phase A: carriers are inert (§6.1.4) — these helpers are exercised only by
// unit tests (JSTests/threads/vmstate/**); no interpreter/JIT/runtime path
// calls them. VM::queueMicrotask/drainMicrotasks are NOT rerouted (§6.5);
// Phase B routes them to the current thread's queue.
//
// I11 (enforced here): a per-thread MicrotaskQueue is enqueued/drained only
// by its owner — every helper debug-asserts isInstalledOnCurrentThread().

#include "MicrotaskQueueInlines.h" // MicrotaskQueue::enqueue + performMicrotaskCheckpoint.
#include "VM.h"                    // currentThreadIsHoldingAPILock (I14 asserts).
#include "VMLite.h"

namespace JSC {

// I11 substrate: a per-thread facility (microtask queue, regexp allocator,
// scratch buffers) may only be touched by the thread the carrier is installed
// on. Reads the same TLS slot setCurrent writes (L4).
ALWAYS_INLINE bool VMLite::isInstalledOnCurrentThread() const
{
    return currentIfExists() == this;
}

// Group 4 lazy init (§6.3): the per-thread regexp BumpPointerAllocator
// mirrors VM::m_regExpAllocator. Phase A: nothing routes regexp execution
// here (the VM member stays authoritative under the GIL); owner-only by I11.
ALWAYS_INLINE BumpPointerAllocator& VMLite::ensureRegExpAllocator()
{
    ASSERT(isInstalledOnCurrentThread());
    if (!regExpAllocator) [[unlikely]]
        regExpAllocator = makeUnique<BumpPointerAllocator>();
    return *regExpAllocator;
}

// §6.5 Group 6 enqueue helper (I11). Lazily creates the default queue on
// first use (ensureDefaultMicrotaskQueue registers it on
// VM::m_microtaskQueues for GC visibility — M12-locked append). Phase A:
// unit-test only; nothing drains this queue implicitly, so a test that
// enqueues must also drain (or let ~VM's force-removal clear it).
ALWAYS_INLINE void VMLite::enqueueMicrotaskToDefaultQueue(QueuedTask&& task)
{
    ASSERT(isInstalledOnCurrentThread()); // I11.
    ensureDefaultMicrotaskQueue().enqueue(WTF::move(task));
}

// §6.5 Group 6 drain helper (I11). Runs a full microtask checkpoint on the
// per-thread default queue; no-op when the queue was never created. Enters
// the VM (VMEntryScope inside performMicrotaskCheckpoint), so the owner must
// hold the JSLock — which also makes the I14 invariant checkable here.
ALWAYS_INLINE void VMLite::drainDefaultMicrotaskQueue()
{
    ASSERT(isInstalledOnCurrentThread()); // I11.
    if (!defaultMicrotaskQueue)
        return;
    ASSERT(vm); // Registered (§6.5.1) before any facility use.
    ASSERT(vm->currentThreadIsHoldingAPILock()); // I14.
    // UNGIL review fix (GIL-removal round 5): mirror VM::drainMicrotasks'
    // two guards. (1) DrainMicrotaskDelayScope: GIL-off every non-main
    // carrier and spawned thread's enqueues are rerouted to its per-lite
    // queue, so without this check the embedder's delay-scope contract
    // (VM.h — the API Bun uses to suppress drains during host calls) was
    // dead on every non-main thread: user JS ran at JSLockHolder
    // destruction inside an open scope. Defer, exactly like the VM-level
    // drain; the scope-closing thread's exit re-drains. (2)
    // executionForbidden: the GIL-on semantics are CLEAR-the-queue, not
    // run; running here would execute user JS after termination latched.
    if (vm->microtaskDrainIsDelayed()) [[unlikely]]
        return;
    if (vm->executionForbidden()) [[unlikely]] {
        defaultMicrotaskQueue->clear();
        return;
    }
    // No globalObject-switch bookkeeping (that is the embedder's
    // drainMicrotasks concern); pass a no-op callback. useCallOnEachMicrotask
    // = TRUE (GIL-removal review round): this drain is no longer
    // unit-test-only — it is the spawned depth-0 token-release drain and the
    // carrier willReleaseLock per-lite drain (JSLock.cpp), and the §E.1b.4
    // host-hook disposition table (JSGlobalObject.cpp) rules
    // VM::m_onEachMicrotaskTick "INLINE on the draining thread (spawned
    // drains included)"; every other drain path (VM::drainMicrotasks, both
    // arms) already fires the hook per task, so <false> here silently
    // bypassed the embedder's per-tick callback at unlock-time drains.
    defaultMicrotaskQueue->performMicrotaskCheckpoint<true>(*vm,
        [](JSGlobalObject*, JSGlobalObject*) { });
}

} // namespace JSC
