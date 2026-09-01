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
// GIL-off, VM::queueMicrotask/drainMicrotasks and JSGlobalObject::queueMicrotask
// route a spawned or non-main carrier's microtasks to that carrier's queue
// through these helpers; flag-off/GIL-on nothing calls them.
//
// I11 (enforced here): a per-thread MicrotaskQueue is enqueued/drained only
// by its owner — every helper debug-asserts isInstalledOnCurrentThread().

#include "MicrotaskQueueInlines.h" // MicrotaskQueue::enqueue + performMicrotaskCheckpoint.
#include "VM.h"                    // currentThreadIsHoldingAPILock (I14 asserts).
#include "VMLite.h"

namespace JSC {

// I11 substrate: a per-thread facility (microtask queue, scratch buffers) may
// only be touched by the thread the carrier is installed on. Reads the same
// TLS slot setCurrent writes (L4).
ALWAYS_INLINE bool VMLite::isInstalledOnCurrentThread() const
{
    return currentIfExists() == this;
}

// §6.5 Group 6 enqueue helper (I11). Lazily creates the default queue on
// first use (ensureDefaultMicrotaskQueue registers it on
// VM::m_microtaskQueues for GC visibility — M12-locked append). The owner's
// depth-0 JSLock release and VM::drainMicrotasks drain it.
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
    // The two guards of VM::drainMicrotasks, per-thread. (1) A
    // DrainMicrotaskDelayScope opened on this thread defers the drain; the
    // scope's close on this thread drains. A scope on another thread does not
    // count: no other thread can drain this queue, so deferring on it would
    // strand the tasks at thread close. (2) executionForbidden: the queue is
    // cleared, not run, once termination has latched.
    if (drainMicrotaskDelayScopeCount) [[unlikely]]
        return;
    if (vm->executionForbidden()) [[unlikely]] {
        defaultMicrotaskQueue->clear();
        return;
    }
    // No globalObject-switch bookkeeping (that is the embedder's
    // drainMicrotasks concern); pass a no-op callback. useCallOnEachMicrotask
    // is true: this is the spawned depth-0 token-release drain and the
    // carrier willReleaseLock per-lite drain (JSLock.cpp), and
    // VM::m_onEachMicrotaskTick fires inline on the draining thread for every
    // drain path (JSGlobalObject.cpp host-hook disposition), so it must fire
    // here too.
    defaultMicrotaskQueue->performMicrotaskCheckpoint<true>(*vm,
        [](JSGlobalObject*, JSGlobalObject*) { });
}

} // namespace JSC
