/*
 * Copyright (C) 2013-2023 Apple Inc. All rights reserved.
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

#include "config.h"
#include "VMEntryScope.h"

#include "ConcurrentButterflyOperations.h"
#include "Options.h"
#include "SamplingProfiler.h"
#include "VM.h"
#include "VMLite.h"
#include "VMLiteShared.h" // VMLiteRegistry: the gilOff entered-record stores run under its lock.
#include "VMEntryScopeInlines.h"
#include "WasmCapabilities.h"
#include "WasmMachineThreads.h"
#include "Watchdog.h"
#include <atomic>

namespace JSC {

void VMEntryScope::setUpSlow()
{
    // GIL-off, the entered record lives only on the current lite: with N
    // concurrently-entered threads a VM-wide shadow would be a last-writer-wins
    // race, and a stale shadow would hide a sibling's exit from VM-wide
    // consumers (VM::isEntered / isAnyThreadEntered walk the registry;
    // VM::currentThreadEntryScope reads the lite). The inline ctor/dtor gate
    // this body on the current lite's record, so it runs exactly once per
    // outermost per-thread entry and the asserts are genuine tripwires.
    // Nothing here refuses a concurrent entry: a stop-the-world window is
    // closed to new mutators by GCClient::Heap::acquireHeapAccess, whose
    // stop-word gate parks an entering thread until the conductor resumes.
    if (m_vm.gilOff()) [[unlikely]] {
        VMLite& lite = VMLite::current();
        RELEASE_ASSERT(lite.vm == &m_vm);
        // The store runs under the registry lock (a leaf lock) so the
        // cross-thread walks keyed on it (anyOtherLiteOfVMEntered, the
        // termination fan-out, isAnyThreadEntered) are serialized against
        // entry/exit transitions. See the field's comment in VMLite.h.
        Locker locker { VMLiteRegistry::singleton().lock };
        RELEASE_ASSERT(!lite.entryScope.load(std::memory_order_relaxed));
        // The entering thread's per-lite soft stack limit is published by its
        // own pass through updateStackLimits (JSLock::didAcquireLock ->
        // setStackPointerAtVMEntry) before any JS runs; generated code must
        // never run against a never-published limit.
        RELEASE_ASSERT_WITH_MESSAGE(lite.threadContext.traps().softStackLimit(),
            "GIL-off entry refused: entering lite's per-thread soft stack limit was never published");
        lite.entryScope.store(this, std::memory_order_relaxed);
    } else
        m_vm.entryScope = this;

#if ASSERT_ENABLED
    // SPEC-vmstate I14: an installed VMLite always belongs to the VM whose
    // JSLock this thread holds.
    if (Options::useVMLite()) {
        if (VMLite* lite = VMLite::currentIfExists())
            ASSERT(lite->vm == &m_vm);
    }
    // SPEC-jit I19: the per-thread butterfly TID tag must be coherent before
    // any JS runs on this thread (CS3; zero-init is correct only for the
    // main thread).
    if (Options::useJSThreads()) [[unlikely]]
        assertButterflyTIDTagCoherent();
#endif

    auto& thread = Thread::currentSingleton();
    if (!thread.isJSThread()) [[unlikely]] {
        Thread::registerJSThread(thread);

        if (Wasm::isSupported())
            Wasm::startTrackingCurrentThread();
#if HAVE(MACH_EXCEPTIONS)
        registerThreadForMachExceptionHandling(thread);
#endif
    }

    if (m_vm.hasAnyEntryScopeServiceRequest()) [[unlikely]]
        m_vm.executeEntryScopeServicesOnEntry();
}

void VMEntryScope::tearDownSlow()
{
    ASSERT_WITH_MESSAGE(!m_vm.hasCheckpointOSRSideState(), "Exitting the VM but pending checkpoint side state still available");

    // UNGIL §A.1.5 (U-T5): GIL-off, clear ONLY the per-lite record (the
    // CURRENT lite is the one this scope was recorded on — the dtor runs on
    // the ctor's thread). The VM-member shadow is dropped (see setUpSlow);
    // GIL-on keeps the landed single write.
    if (m_vm.gilOff()) [[unlikely]] {
        VMLite& lite = VMLite::current();
        RELEASE_ASSERT(lite.vm == &m_vm);
        // Registry-lock-serialized for the same reason as setUpSlow's store
        // (see VMLite.h's entryScope comment).
        Locker locker { VMLiteRegistry::singleton().lock };
        RELEASE_ASSERT(lite.entryScope.load(std::memory_order_relaxed) == this);
        lite.entryScope.store(nullptr, std::memory_order_relaxed);
    } else
        m_vm.entryScope = nullptr;

    // §A.1.5: executeEntryScopeServicesOnExit uses the CURRENT lite's bits
    // when gilOff (VM::has/clearEntryScopeService route there).
    if (m_vm.hasAnyEntryScopeServiceRequest()) [[unlikely]]
        m_vm.executeEntryScopeServicesOnExit();
}

} // namespace JSC
