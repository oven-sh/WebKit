/*
 * Copyright (C) 2021-2025 Apple Inc. All rights reserved.
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

#include "VM.h"

namespace JSC {

ALWAYS_INLINE VM& VMTraps::vm() const
{
    // UNGIL §A.2.2 item 3b (AB-17): the `this - VM::offsetOfTraps()`
    // arithmetic is valid ONLY for the VM-embedded instance. A per-lite
    // instance (embedded in VMLite::threadContext) carries its owner VM in
    // m_liteOwnerVM, stamped once at VMLiteRegistry::registerLite (the sole
    // writer of lite.vm) before the lite is installable — consult it FIRST.
    // GIL-on / flag-off: m_liteOwnerVM is null on the VM-embedded instance,
    // byte-identical behavior.
    if (m_liteOwnerVM) [[unlikely]]
        return *m_liteOwnerVM;
    return *std::bit_cast<VM*>(std::bit_cast<uintptr_t>(this) - VM::offsetOfTraps());
}

inline void VMTraps::deferTermination(DeferAction deferAction)
{
    auto originalCount = m_deferTerminationCount++;
    ASSERT(m_deferTerminationCount < UINT_MAX);
    // Strictly speaking, we're only interested in vm.hasPendingTerminationException() here.
    // However, vm.exception() is a necessary condition for vm.hasPendingTerminationException().
    // Since this checks is intended to be cheap, we'll just do the cheaper check of vm.exception()
    // which itself rarely returns true. We'll let the slow path do the full
    // vm.hasPendingTerminationException() check instead.
    if (!originalCount && vm().exception()) [[unlikely]]
        deferTerminationSlow(deferAction);
}

inline void VMTraps::undoDeferTermination(DeferAction deferAction)
{
    ASSERT(m_deferTerminationCount > 0);
    ASSERT(!m_suspendedTerminationException || vm().hasTerminationRequest());
    if (!--m_deferTerminationCount && vm().hasTerminationRequest()) [[unlikely]]
        undoDeferTerminationSlow(deferAction);
}

ALWAYS_INLINE DeferTraps::DeferTraps(VM& vm)
    // FIX (stw-watchdog-timeout, root cause B): deferral is a property of the
    // DEFERRING THREAD's stack ("we can't jettison the code THIS thread is
    // about to run"), so GIL-off it lives in the current thread's per-lite
    // VMTraps instance (the trapsForCurrentThread() mode-split, same storage
    // domain as the per-lite trap word). Keying it on the shared VM-level
    // instance was wrong twice over with N mutators: (a) one thread's narrow
    // deferral blinded EVERY sibling's handleTraps poll — in particular a
    // sibling spinning at a poll site could not service NeedStopTheWorld and
    // park, wedging a §A.3 conductor into the 30s watchdog fail-stop; and
    // (b) the save/restore pair below raced across threads on the one flag
    // (A-ctor, B-ctor saves true, A-dtor clears, B-dtor restores true),
    // leaving the flag stuck true with no scope open — permanently
    // trap-blinding the whole VM. GIL-on / flag-off: trapsForCurrentThread()
    // == vm.traps(), byte-identical.
    : m_traps(vm.trapsForCurrentThread())
    , m_previousTrapsDeferred(m_traps.m_trapsDeferred)
{
    m_traps.m_trapsDeferred = true;
}

ALWAYS_INLINE DeferTraps::~DeferTraps()
{
    m_traps.m_trapsDeferred = m_previousTrapsDeferred;
}

inline void VMTraps::notifyGrabAllLocks()
{
    if (needHandling(AsyncEvents))
        invalidateCodeBlocksOnStack();
}

inline void VMTraps::setStackSoftLimit(void* newLimit)
{
    m_stack.setStackSoftLimit(newLimit);
}

inline void VMTraps::registerMirror(Mirror& mirror)
{
    m_stack.registerMirror(mirror);
}

inline void VMTraps::unregisterMirror(Mirror& mirror)
{
    m_stack.unregisterMirror(mirror);
}

inline void VMTraps::requestStop()
{
    m_stack.requestStop();
}

inline void VMTraps::cancelStop()
{
    m_stack.cancelStop();
}

} // namespace JSC
