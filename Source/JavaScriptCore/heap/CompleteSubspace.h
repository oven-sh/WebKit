/*
 * Copyright (C) 2017-2021 Apple Inc. All rights reserved.
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

#include "Subspace.h"
#include <JavaScriptCore/AllocatorForMode.h>
#include <atomic>

namespace JSC {

namespace GCClient {
class CompleteSubspaceView;
class GCThreadLocalCache;
class Heap;
}

class CompleteSubspace final : public Subspace {
public:
    JS_EXPORT_PRIVATE CompleteSubspace(CString name, Heap&, const HeapCellType&, AlignedMemoryAllocator*);
    JS_EXPORT_PRIVATE ~CompleteSubspace() final;

    // In some code paths, we need it to be a compile error to call the virtual version of one of
    // these functions. That's why we do final methods the old school way.

    // FIXME: Currently subspaces speak of BlockDirectories as "allocators", but that's temporary.
    // https://bugs.webkit.org/show_bug.cgi?id=181559
    Allocator allocatorFor(size_t, AllocatorForMode);
    Allocator allocatorForNonInline(size_t, AllocatorForMode);

    void* allocate(VM&, size_t, GCDeferralContext*, AllocationFailureMode);
    // SharedGC (SPEC-heap.md §12.1 seam; THREADS T4): TLC-routed allocation
    // for a specific client. Skips the VM-coupled preludes (verifyCanGC /
    // sanitizeStackForVM) — standalone harness clients have no VM — but
    // collectIfNecessaryOrDefer/stopIfNecessary ARE reached on the slow
    // paths. The VM-taking overloads delegate here when
    // Options::useSharedGCHeap(). Defined in CompleteSubspaceInlines.h.
    void* allocateForClient(GCClient::Heap&, size_t, GCDeferralContext*, AllocationFailureMode);
    void* reallocatePreciseAllocationNonVirtual(VM&, HeapCell*, size_t, GCDeferralContext*, AllocationFailureMode);

    static constexpr ptrdiff_t offsetOfAllocatorForSizeStep() { return OBJECT_OFFSETOF(CompleteSubspace, m_allocatorForSizeStep); }

    std::span<Allocator, MarkedSpace::numSizeClasses> allocatorsForSizeSteps() { return m_allocatorForSizeStep; }

    void prepareAllAllocators();

    // SharedGC (§5.3; T4): base of this subspace's contiguous
    // numSizeClasses-slot range in every client's GCThreadLocalCache flat
    // table; size class i's slot = tlcIndexBase() + i. Reserved under
    // MarkedSpace::m_directoryLock at first directory creation;
    // BlockDirectory::invalidTlcIndex until then (benign-race readers go
    // through the TLC slow path, which takes the lock).
    unsigned tlcIndexBase() const { return m_tlcIndexBase.load(std::memory_order_acquire); }

    // SharedGC (§5.3; T4): directory lookup/creation independent of server
    // Allocators (with the option on those are never materialized, §5.5).
    // directoryLock-only — JIT-thread-safe, no MSPL. Returns null when the
    // size step has no size class (callers take the precise path).
    BlockDirectory* ensureDirectoryForSizeStep(size_t sizeStepIndex);

    // SharedGC (§5.5; T4): RELEASE_ASSERTs that no server-side Allocator /
    // LocalAllocator was ever materialized for this subspace. Called for
    // every CompleteSubspace at the second-client attach
    // (Heap::verifyServerNonIsoAllocatorsNeverMaterialized()).
    void verifyNoAllocatorsMaterialized();

    // H-GCCLIENT-COMPLETESUBSPACE-WRAPPER (SHAREDHEAP-ALLOC-EVIDENCE.md §41
    // T1): reserve this subspace's contiguous numSizeClasses-slot range
    // WITHOUT creating any directory. Same locking + write-once semantics as
    // the in-ensureDirectoryForSizeClass reservation (CompleteSubspace.cpp:
    // 88-89), so a later directory creation just observes the
    // already-reserved base. directoryLock-only (rank 7b) — no MSPL, no GC
    // dependency; idempotent. Called from GCThreadLocalCache's ctor under
    // Options::useSharedGCHeap() so every per-client slotBase
    // (= tlc.m_table + tlcIndexBase()) is computable at attach with no eager
    // BlockDirectory allocation. Flag-off: never reached.
    void ensureTlcIndexBaseReserved();

private:
    friend class GCClient::CompleteSubspaceView; // wrapper calls allocateSlowForClient on the miss path.
    JS_EXPORT_PRIVATE Allocator allocatorForSlow(size_t);
    JS_EXPORT_PRIVATE BlockDirectory* ensureDirectoryForSizeStepSlow(size_t sizeStepIndex);
    // Creates (or returns) the directory serving sizeClass; caller holds
    // MarkedSpace::m_directoryLock (the locker token). Never materializes a
    // server LocalAllocator.
    BlockDirectory* ensureDirectoryForSizeClass(const AbstractLocker& directoryLocker, size_t sizeClass);

    // These slow paths are concerned with large allocations and allocator creation.
    JS_EXPORT_PRIVATE void* allocateSlow(VM&, size_t, GCDeferralContext*, AllocationFailureMode);
    void* tryAllocateSlow(VM&, size_t, GCDeferralContext*);
    JS_EXPORT_PRIVATE void* allocateSlowForClient(GCClient::Heap&, size_t, GCDeferralContext*, AllocationFailureMode);
    void* tryAllocateSlowForClient(GCClient::Heap&, size_t, GCDeferralContext*);

    std::array<Allocator, MarkedSpace::numSizeClasses> m_allocatorForSizeStep;
    // SharedGC (§5.3; T4): per-size-step directory table, populated at
    // directory creation in BOTH modes (under directoryLock). Entries are
    // published with release stores and read lock-free with acquire loads
    // (ensureDirectoryForSizeStep fast path), so unlocked readers observe a
    // fully initialized BlockDirectory.
    std::array<std::atomic<BlockDirectory*>, MarkedSpace::numSizeClasses> m_directoryForSizeStep { };
    std::atomic<unsigned> m_tlcIndexBase { BlockDirectory::invalidTlcIndex };
    Vector<std::unique_ptr<BlockDirectory>> m_directories;
    Vector<std::unique_ptr<LocalAllocator>> m_localAllocators;
};

ALWAYS_INLINE Allocator CompleteSubspace::allocatorFor(size_t size, AllocatorForMode mode)
{
    if (size <= MarkedSpace::largeCutoff) {
        Allocator result = m_allocatorForSizeStep[MarkedSpace::sizeClassToIndex(size)];
        switch (mode) {
        case AllocatorForMode::MustAlreadyHaveAllocator:
            // SharedGC (§5.5 T4 audit): forbidden when the heap may be shared
            // — with Options::useSharedGCHeap() the server table is never
            // populated, so this RELEASE_ASSERT would always fail. No in-tree
            // caller passes this mode today (audit recorded in
            // INTEGRATE-heap.md, T4); the assert keeps the invariant for any
            // future caller.
            RELEASE_ASSERT(result);
            break;
        case AllocatorForMode::EnsureAllocator:
            if (!result)
                return allocatorForSlow(size);
            break;
        case AllocatorForMode::AllocatorIfExists:
            break;
        }
        return result;
    }
    RELEASE_ASSERT(mode != AllocatorForMode::MustAlreadyHaveAllocator);
    return Allocator();
}

inline void CompleteSubspace::ensureTlcIndexBaseReserved()
{
    if (m_tlcIndexBase.load(std::memory_order_acquire) != BlockDirectory::invalidTlcIndex)
        return;
    Locker locker { m_space.directoryLock() };
    if (m_tlcIndexBase.load(std::memory_order_relaxed) == BlockDirectory::invalidTlcIndex)
        m_tlcIndexBase.store(m_space.reserveThreadLocalCacheIndices(locker), std::memory_order_release);
}

ALWAYS_INLINE BlockDirectory* CompleteSubspace::ensureDirectoryForSizeStep(size_t sizeStepIndex)
{
    ASSERT(sizeStepIndex < MarkedSpace::numSizeClasses);
    // Acquire pairs with the release stores in ensureDirectoryForSizeClass so
    // a published pointer implies a fully initialized directory. This path
    // only runs on a TLC slot miss, so the acquire cost is irrelevant.
    if (BlockDirectory* directory = m_directoryForSizeStep[sizeStepIndex].load(std::memory_order_acquire))
        return directory;
    return ensureDirectoryForSizeStepSlow(sizeStepIndex);
}

namespace GCClient {

// H-GCCLIENT-COMPLETESUBSPACE-WRAPPER (SHAREDHEAP-ALLOC-EVIDENCE.md §41 T1;
// estMs=150 of the +1912ms W=1 GIL-off intcs tax): per-client view of one
// server CompleteSubspace, mirroring the GCClient::IsoSubspace pattern. Holds
// a precomputed `m_slotBase = client.threadLocalCache().m_table +
// server.tlcIndexBase()` so allocate() collapses to one indexed load + null
// test — byte-for-byte the GIL-on `m_allocatorForSizeStep[idx]` shape
// (CompleteSubspace.h:130) — eliminating ALL of hop-2: the tlcIndexBase
// acquire-load, the invalid-base check, the client.threadLocalCache() member
// chase, the m_table pointer deref, and the bound-check
// (GCThreadLocalCache.cpp allocatorForSizeStep). Distinct from
// H-SOLECLIENT-SUBSPACE-MIRROR (which writes the W=1 sole client's allocators
// into the SERVER m_allocatorForSizeStep): this is a per-client view, correct
// at any W.
//
// m_slotBase validity REQUIRES the TLC table never reallocs after bind() —
// guaranteed by the H-TLC-FIXEDTABLE-NOREALLOC pre-grow in
// GCThreadLocalCache's ctor (full numCompleteSubspaces × numSizeClasses
// capacity, useSharedGCHeap-gated; growTable() RELEASE_ASSERTs it never
// re-enters its realloc leg). bind() is idempotent and is invoked from
// Heap::setCurrentThreadClient (GCThreadLocalCache.cpp), which already runs
// at every {attach, A36C carrier swap, main-client adoption} on the owning
// thread (I2).
//
// STAGING NOTE (this round): the VM.h CompleteSubspace accessors are NOT yet
// rerouted through allocationClientForCurrentThread().<name>Client — that
// flip is source-incompatible with the JIT-side `CompleteSubspace&`/
// `CompleteSubspace*` consumers (AssemblyHelpers::emitAllocateVariableSized,
// FTL allocatorForSize, DFG tlcSlotForSubspace) which are outside this
// hypothesis's owned-file set. The wrapper, the fixed table, and the bind
// hook land here as INFRA so that flip is a single self-contained change in a
// round that owns those call sites. Until then this class has no caller and
// imposes ZERO codegen / behavior delta on either flag state beyond the
// useSharedGCHeap-gated fixed-table pre-grow (which is itself a correctness
// hardening for the in-flight H-TLS-TABLE / H-VMLITE-TLCPTR snapshots — the
// table pointer they cache can no longer go stale across a free).
//
// Named CompleteSubspaceView (not CompleteSubspace) because
// GCThreadLocalCache.h — outside this hypothesis's owned-file set — uses
// unqualified `CompleteSubspace` from inside namespace GCClient
// (allocatorForSizeStep's parameter); introducing a same-named GCClient class
// would shadow that to a different type depending on include order (ODR
// hazard + build break). The iso analogue is GCClient::IsoSubspace; rename to
// GCClient::CompleteSubspace once GCThreadLocalCache.h qualifies its
// reference as JSC::CompleteSubspace.
class CompleteSubspaceView {
    WTF_MAKE_NONCOPYABLE(CompleteSubspaceView);
public:
    CompleteSubspaceView() = default;

    // Idempotent; owner thread only (I2). slotBase may be null when the
    // server's tlcIndexBase is still unreserved (never the case once
    // ensureTlcIndexBaseReserved() has run for all server subspaces in the
    // TLC ctor) — allocate() then unconditionally takes the slow path.
    void bind(JSC::CompleteSubspace& server, GCClient::Heap& client, Allocator* slotBase)
    {
        m_server = &server;
        m_client = &client;
        m_slotBase = slotBase;
    }

    JSC::CompleteSubspace& server() const { ASSERT(m_server); return *m_server; }
    // Implicit conversion so reference-taking JIT-side helpers
    // (emitAllocateVariableSized, allocatorForSize) keep compiling once the
    // VM.h reroute lands. NOT sufficient for address-of call sites — those
    // need the explicit .server() in the rerouting round.
    operator JSC::CompleteSubspace&() const { return server(); }

    // One indexed load on the 99%+ interval-bump path. Defined in
    // CompleteSubspaceInlines.h (needs VM/Heap complete).
    void* allocate(VM&, size_t, GCDeferralContext*, AllocationFailureMode);

    // Forwarders for the remaining server-surface uses observed at the VM.h
    // accessor call sites (.allocatorFor / .allocatorForNonInline /
    // .reallocatePreciseAllocationNonVirtual): the wrapper is a transparent
    // per-client front for allocate() and otherwise IS the server subspace.
    Allocator allocatorFor(size_t size, AllocatorForMode mode) { return server().allocatorFor(size, mode); }
    Allocator allocatorForNonInline(size_t size, AllocatorForMode mode) { return server().allocatorForNonInline(size, mode); }
    void* reallocatePreciseAllocationNonVirtual(VM& vm, HeapCell* cell, size_t size, GCDeferralContext* ctx, AllocationFailureMode mode)
    {
        return server().reallocatePreciseAllocationNonVirtual(vm, cell, size, ctx, mode);
    }

private:
    JSC::CompleteSubspace* m_server { nullptr };
    GCClient::Heap* m_client { nullptr };
    Allocator* m_slotBase { nullptr }; // = client TLC m_table + server.tlcIndexBase(); never dangles (fixed table).
};

} // namespace GCClient

} // namespace JSC

