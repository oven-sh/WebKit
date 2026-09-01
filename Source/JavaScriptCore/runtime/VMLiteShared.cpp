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

#include "config.h"
#include "VMLiteShared.h"

#include "GCDeferralContextInlines.h"
#include "Heap.h"
#include "Options.h"
#include "ThreadManager.h"
#include "VM.h"
// Intra-WS dependency (SPEC-vmstate §11): VMLite.h lands with task 6 (W3
// struct). VMLiteRegistry::registerLite below is the sole writer of
// VMLite::vm (§6.5.1), which needs the complete type.
#include "VMLite.h"
#include <wtf/Atomics.h>
#include <wtf/NeverDestroyed.h>

namespace JSC {

// ---- SharedVMState (SPEC-vmstate §5.2) -------------------------------------

SharedVMState& SharedVMState::singleton()
{
    static NeverDestroyed<SharedVMState> shared;
    return shared;
}

// Set while this thread holds m_structureAllocationLock. WTF::Lock tracks no
// owner, so a nested StructureAllocationLocker on the same thread would park
// forever inside lock(); the ctor tests this flag first and fails stop there.
static thread_local bool t_holdsStructureAllocationLock { false };

SharedVMState::StructureAllocationLocker::StructureAllocationLocker(VM& vm)
{
    // Acquisition sites construct the locker only with the option on (see the
    // header); a direct construction with it off stays a no-op whose
    // deferralContext() is null.
    if (!Options::useStructureAllocationLock()) [[likely]]
        return;

    auto& shared = singleton();

    // Rank 7a (§7): may be taken below JSLock/GC locks (1-6) and is held
    // across the heap allocation locks (7-10). Recursive acquisition is
    // forbidden (§5.2): the lock itself cannot detect it (it would hang), so
    // the same-thread flag above is checked before locking.
    RELEASE_ASSERT(!t_holdsStructureAllocationLock);
    shared.m_structureAllocationLock.lock();
    t_holdsStructureAllocationLock = true;
    m_vm = &vm;

    // I8: never two threads simultaneously inside a Structure-cell-allocating
    // region. The counter is only ever touched under the lock, so > 1 here
    // can only mean lock-bypass or counter corruption.
    uint32_t previous = shared.m_inStructureAllocationRegion.fetch_add(1, std::memory_order_relaxed);
    RELEASE_ASSERT(!previous);

    // SPEC-heap I14: a holder of this lock must never initiate, join or wait
    // for a stop-the-world; Heap::collectIfNecessaryOrDefer defers a stop
    // poll reached inside the scope instead.
    vm.heap.incrementSTWForbiddenScope();

    // N4: GC deferral is SPEC-heap L5/I14 verbatim — this stack-local
    // GCDeferralContext is threaded into the cell allocation by the M7 sites
    // via deferralContext(). Allocations under the lock may slow-path into
    // fresh blocks but never trigger a synchronous collection (S1).
    m_deferralContext.emplace(vm);
}

SharedVMState::StructureAllocationLocker::~StructureAllocationLocker()
{
    if (!m_vm)
        return;
    ASSERT(m_deferralContext);

    // F5 (§5.4): order the in-region initialization stores before any
    // publishing store this thread issues AFTER the region (e.g. the
    // StructureID written into a cell header, consumed on other threads via
    // dependency-carrying loads: cell header -> decode() -> deref is
    // dependency-ordered on arm64; x86-64 is TSO). Scope note: publications
    // that happen under their own lock — the transition-table inserts at the
    // M7 sites — do NOT rely on this fence; writer and (useJSThreads)
    // readers both hold the source Structure's m_lock, which is the
    // happens-before edge there. See the StructureAllocationLocker class
    // comment in VMLiteShared.h for the site-by-site evidence.
    WTF::storeStoreFence();

    m_vm->heap.decrementSTWForbiddenScope();

    auto& shared = singleton();
    uint32_t previous = shared.m_inStructureAllocationRegion.fetch_sub(1, std::memory_order_relaxed);
    RELEASE_ASSERT(previous == 1); // I8

    t_holdsStructureAllocationLock = false;
    shared.m_structureAllocationLock.unlock();

    // m_deferralContext is the FIRST declared member (frozen, §5.2), so its
    // destructor runs LAST — strictly after the unlock above. A deferred
    // collection (~GCDeferralContext -> collectIfNecessaryOrDefer) therefore
    // never runs while the structure allocation lock is held (S1: a holder
    // always runs to release; no STW cycle).
}

// ---- VMLiteRegistry (SPEC-vmstate §6.5.1) ----------------------------------

VMLiteRegistry& VMLiteRegistry::singleton()
{
    static NeverDestroyed<VMLiteRegistry> registry;
    return registry;
}

void VMLiteRegistry::registerLite(VMLite& lite, VM& vm)
{
    // Lock rank (§A.2.2 item 3c (h)): the registry lock is no longer a leaf —
    // the gilOff registration backfill below drives the fresh lite's own
    // trap bookkeeping (per-lite m_trapSignalingLock -> per-lite
    // StackManager::m_mirrorLock) under it.
    assertNoPerLiteTrapSignalingLockHeldOnCurrentThread();
    Locker locker { lock };
    ASSERT(!lites.contains(&lite));
    ASSERT(!lite.vm); // Sole writer (§6.5.1); was null, immutable after.
    lite.vm = &vm;
    lites.append(&lite);

    // UNGIL §A.2.2 items 2 + 3b (AB-17): stamp the per-lite traps instance's
    // owner VM (VMTraps::vm() consults it before the VM-embedded offset
    // arithmetic) and thread kind, then backfill pending VM-wide async bits
    // into the fresh per-lite word and derive its stop request — under THIS
    // registry-lock hold, so a concurrent rule-3 fan-out either sees the
    // appended lite (and fans into it) or precedes the backfill (which then
    // copies the raised bits). Registration runs on the lite's owner thread
    // in all three paths (VM ctor / JSLock.cpp carrier / ThreadObject.cpp
    // spawn), so the thread-kind probe identifies the lite's kind (TERM1.4).
    if (vm.gilOff() && lite.gilOff) [[unlikely]] {
        lite.threadContext.traps().setLiteOwnerVM(&vm, ThreadManager::isJSThreadCurrent());
        vm.traps().backfillVMWideTrapBitsAtLiteRegistration(lite, locker);
        // The per-thread soft reserved zone size starts at the VM's resting
        // value: no ErrorHandlingScope is open on a thread that is registering.
        lite.softReservedZoneSize = vm.softReservedZoneSize();
    }
}

void VMLiteRegistry::unregisterLite(VMLite& lite)
{
    Locker locker { lock };
    // Lifetime contract (§6.5.1): the lite is still registered — it is
    // unregistered exactly once, before destruction and before its thread's
    // teardown setCurrent(nullptr) (N8: under the final JSLock hold).
    bool removed = lites.removeFirst(&lite);
    ASSERT_UNUSED(removed, removed);
}

} // namespace JSC
