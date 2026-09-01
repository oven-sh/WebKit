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

#include "GCDeferralContext.h"
#include "JSExportMacros.h"
#include <atomic>
#include <optional>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/ThreadSafetyAnalysis.h>
#include <wtf/Vector.h>

namespace JSC {

class VM;
class VMLite;

// Shared (process-global) VM state for N threads in one logical VM
// (SPEC-vmstate §5, frozen interface §5.2).
//
// W2: Structure cell allocation + ID-creating transitions are serialized by
// one process-global lock. StructureIDs are pure VA arithmetic
// (StructureID::decode() == m_bits + structureIDBase(); no table to
// synchronize) — ONLY allocation synchronizes; IDs baked into JIT code stay
// valid forever.
//
// Lock ranking (SPEC-vmstate §7, subordinate to SPEC-heap §6):
// structureAllocationLock is rank 7a — below JSLock/GC locks (rows 1-6), held
// ACROSS the allocation locks (rows 7-10), never waits on STW. While it is
// held (S1-S3):
//   - the holder is inside the heap's STW-forbidden scope; allocations pass
//     the locker's GCDeferralContext — they may slow-path into fresh blocks
//     but never trigger a synchronous collection or VMManager stop-the-world;
//   - no safepoint poll, no collectSync/handshake;
//   - allocator internals (LocalAllocator, BlockDirectory, libpas) must NEVER
//     acquire it (S2).
class SharedVMState {
    WTF_MAKE_NONCOPYABLE(SharedVMState);
public:
    SharedVMState() = default; // Construction reserved to singleton() (NeverDestroyed).

    JS_EXPORT_PRIVATE static SharedVMState& singleton(); // NeverDestroyed

    // Rank: SPEC-heap §6 rank 7a (§7). Recursive acquisition forbidden —
    // nesting two StructureAllocationLockers self-deadlocks (§5.3: the
    // integrator audits acquisition sites and never nests them).
    Lock& structureAllocationLock() { return m_structureAllocationLock; }

    // RAII; no-op unless Options::useStructureAllocationLock() (I10: flag off
    // compiles to one predictable branch on a latched option).
    //
    // Ctor (frozen order, §5.2): lock; incrementSTWForbiddenScope() (N7 shim;
    // SPEC-heap I14); emplace m_deferralContext(vm).
    // Dtor: F5 storeStoreFence; decrementSTWForbiddenScope(); unlock; THEN
    // ~GCDeferralContext runs (the deferred collection, if any, happens
    // strictly after unlock — S1).
    //
    // F5 fence scope (precise — do not overclaim): the fence orders stores
    // made INSIDE the locker region before any publishing store the holder
    // makes AFTER the region (e.g. storing the new StructureID into a cell
    // header, consumed on other threads via dependency-carrying loads / under
    // locks). It does NOT — and need not — order publications that happen
    // inside a later region or inside other locks: in particular, the
    // transition-table insert that publishes a new Structure is NOT covered
    // by this fence and does not rely on it. That insert runs under the
    // source Structure's m_lock (GCSafeConcurrentJSLocker at every M7 insert
    // site: Structure.cpp:708/725, 907/919, 1052/1062, 1258/1270, 2302/2307),
    // and under useJSThreads EVERY mutator transition-table lookup holds the
    // same m_lock (StructureInlines.h:689-690 routes all mutator callers to
    // the Concurrently variant; compiler threads already lock — SPEC-
    // objectmodel L6(i)/I37). Writer-unlock -> reader-lock is the
    // happens-before edge that publishes the fully initialized Structure to
    // table readers; the fence here only backstops post-region, lock-free
    // consumption of the StructureID itself. M7 audit step 5 keeps this
    // invariant checked (any new lock-free transition-table reader is a
    // blocker).
    class StructureAllocationLocker {
        WTF_MAKE_NONCOPYABLE(StructureAllocationLocker);
        WTF_FORBID_HEAP_ALLOCATION;
    public:
        JS_EXPORT_PRIVATE explicit StructureAllocationLocker(VM&) WTF_IGNORES_THREAD_SAFETY_ANALYSIS;
        JS_EXPORT_PRIVATE ~StructureAllocationLocker() WTF_IGNORES_THREAD_SAFETY_ANALYSIS;

        // Null when inactive (flag off). M7 acquisition sites pass it into
        // the Structure cell allocation (SPEC-heap L5/I14 — N4: GC deferral is
        // the stack-local GCDeferralContext, threaded into the allocator).
        GCDeferralContext* deferralContext() { return m_deferralContext ? &*m_deferralContext : nullptr; }

    private:
        // FROZEN member order (§5.2): m_deferralContext FIRST, lock state
        // after. Members destroy in reverse declaration order, so
        // ~GCDeferralContext runs LAST — after the dtor body has fenced,
        // left the STW-forbidden scope, and released the lock.
        std::optional<GCDeferralContext> m_deferralContext;
        VM* m_vm { nullptr }; // Lock state: non-null iff the lock is held by this locker.
    };

    // I8 observability: number of threads currently inside a
    // Structure-cell-allocating region. The locker RELEASE_ASSERTs it never
    // exceeds 1 (flag on => never two threads simultaneously in-region).
    uint32_t structureAllocationRegionDepth() const { return m_inStructureAllocationRegion.load(std::memory_order_relaxed); }

private:
    Lock m_structureAllocationLock;
    std::atomic<uint32_t> m_inStructureAllocationRegion { 0 }; // I8 counter.
};

// Process-global registry of live VMLite carriers (SPEC-vmstate §6.5.1,
// frozen; deliberately NOT part of the §6.3 VMLite body).
//
// Lock ranking (§7): leaf. Nothing may be acquired while holding it. Taken by
// mutators (registerLite/unregisterLite; MicrotaskQueue ctor append / dtor
// removal — M12; ~VM force-removal — M11) and by GC markers (iteration of
// VM::m_microtaskQueues in beginMarking/visitAggregateImpl — M11; markers
// hold no other lock there). The same lock guards VM::m_microtaskQueues.
//
// Lifetime (frozen): a lite is unregistered before it is destroyed and before
// its thread's teardown setCurrent(nullptr); a VM must not die while a
// registered lite's vm points at it (§6.4.4 ~VM assert, under this lock).
// N8 (api r11 4.6.1/5.2): unregister + setCurrent(nullptr) + tag clear run
// UNDER the final JSLock hold, pre-release (this lock is a leaf, so taking it
// under the JSLock is sound); the lite is destroyed after JSLock release.
struct VMLiteRegistry {
    JS_EXPORT_PRIVATE static VMLiteRegistry& singleton(); // NeverDestroyed

    Lock lock;                                       // leaf rank (§7)
    Vector<VMLite*> lites WTF_GUARDED_BY_LOCK(lock); // fastMalloc only

    // Takes lock; asserts the lite is absent; stores lite.vm = &vm (asserts
    // it was null) — sole writer of VMLite::vm; immutable after (§6.3).
    JS_EXPORT_PRIVATE void registerLite(VMLite&, VM&);
    // Takes lock; asserts the lite is present; removes it.
    JS_EXPORT_PRIVATE void unregisterLite(VMLite&);
};

} // namespace JSC
