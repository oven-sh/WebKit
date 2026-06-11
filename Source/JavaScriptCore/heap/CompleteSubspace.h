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

private:
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

} // namespace JSC

