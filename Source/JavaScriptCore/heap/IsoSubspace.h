/*
 * Copyright (C) 2017-2025 Apple Inc. All rights reserved.
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

#include "AlignedMemoryAllocator.h"
#include "BlockDirectory.h"
#include "Subspace.h"
#include "SubspaceAccess.h"
#include <wtf/Atomics.h>
#include <wtf/SinglyLinkedListWithTail.h>
#include <JavaScriptCore/AllocatorForMode.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class IsoCellSet;

namespace GCClient {
class Heap;
class IsoSubspace;
}

class IsoSubspace final : public Subspace {
    WTF_MAKE_TZONE_ALLOCATED_EXPORT(IsoSubspace, JS_EXPORT_PRIVATE);
public:
    JS_EXPORT_PRIVATE IsoSubspace(CString name, Heap&, const HeapCellType&, size_t, uint8_t numberOfLowerTierPreciseCells, std::unique_ptr<AlignedMemoryAllocator>&& = nullptr);
    JS_EXPORT_PRIVATE ~IsoSubspace() final;

    size_t cellSize() { return m_directory.cellSize(); }

    void sweepLowerTierPreciseCell(PreciseAllocation*);
    void clearIsoCellSetBit(PreciseAllocation*);

    void* tryAllocateLowerTierPrecise(size_t cellSize);
    void destroyLowerTierPreciseFreeList();

    // H-ISO-TLCSLOT (GILOFF-TAX §42 follow-on; iso analogue of
    // CompleteSubspace::tlcIndexBase): one fixed slot in every client's
    // GCThreadLocalCache flat table holding that client's LocalAllocator* for
    // this iso subspace. Stamped write-once by the first GCThreadLocalCache
    // ctor (under Options::useSharedGCHeap(); the first client is constructed
    // serially during VM ctor so the stamp is race-free); subsequent clients
    // observe the same value (asserted). The slot value is
    //   JSC::Heap::numCompleteSubspaces × MarkedSpace::numSizeClasses + ordinal
    // where `ordinal` is this subspace's FOR_EACH_JSC_ISO_SUBSPACE position —
    // a per-type process-wide constant, so JIT inline-allocate emitters bake it
    // and resolve `lite->tlcTable[slot]` exactly as the §42 CompleteSubspace
    // arm does (the IT-9 "iso → null Allocator → unconditional thunk" hole:
    // 36.4M MakeRope lazy-slow-path traversals on intcs W=1, ~910ms). Iso
    // subspaces NOT enumerated by FOR_EACH_JSC_ISO_SUBSPACE (the 4 SpaceAndSet
    // statics, every dynamic iso) keep invalidTlcIndex and stay lookup-only via
    // m_perDirectory — they are never on a JIT inline-allocate path. The
    // BlockDirectory's own m_tlcIndex is deliberately left invalidTlcIndex
    // (the §5.3 "iso = lookup-only" predicate in GCThreadLocalCache::
    // allocatorFor / materializeAllocator remains intact). Flag-off /
    // !useSharedGCHeap: never stamped, never read (every reader is behind a
    // vm.gilOff() codegen gate); the field is one trailing unsigned with no
    // JIT-consumed-offset effect.
    // TSAN tlc-slot-stamp: spawned-thread GCThreadLocalCache ctors re-stamp the
    // identical value concurrently with each other and with DFG-compile-thread
    // reads (tlcSlotForConcurrentlyWithIso). The first stamp is serial (VM ctor)
    // so every concurrent read observes the final value; relaxed atomics here
    // close the C++ data-race UB only — no ordering required, flag-off never
    // touches the field.
    unsigned tlcSlot() const { return WTF::atomicLoad(const_cast<unsigned*>(&m_tlcSlot), std::memory_order_relaxed); }
    void stampTlcSlot(unsigned slot)
    {
        ASSERT(slot != BlockDirectory::invalidTlcIndex);
#if ASSERT_ENABLED
        unsigned current = WTF::atomicLoad(&m_tlcSlot, std::memory_order_relaxed);
        ASSERT_UNUSED(current, current == BlockDirectory::invalidTlcIndex || current == slot);
#endif
        WTF::atomicStore(&m_tlcSlot, slot, std::memory_order_relaxed);
    }

private:
    void* tryAllocateLowerTierPreciseImpl(size_t); // T7-mspl-per-directory: factored body; caller takes the registry lock when shared.
public:

    void sweep();

    template<typename Func> void forEachLowerTierPreciseFreeListedPreciseAllocation(const Func&);

private:
    friend class IsoCellSet;
    friend class GCClient::IsoSubspace;
    
    void didResizeBits(unsigned newSize) final;
    void didRemoveBlock(unsigned blockIndex) final;
    void didBeginSweepingToFreeList(MarkedBlock::Handle*) final;

    BlockDirectory m_directory;
    std::unique_ptr<AlignedMemoryAllocator> m_allocator;
    SentinelLinkedList<PreciseAllocation, BasicRawSentinelNode<PreciseAllocation>> m_lowerTierPreciseFreeList;
    SentinelLinkedList<IsoCellSet, BasicRawSentinelNode<IsoCellSet>> m_cellSets;
    unsigned m_tlcSlot { BlockDirectory::invalidTlcIndex }; // H-ISO-TLCSLOT (see tlcSlot()).
};


namespace GCClient {

class IsoSubspace {
    WTF_MAKE_NONCOPYABLE(IsoSubspace);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(IsoSubspace);
public:
    JS_EXPORT_PRIVATE IsoSubspace(JSC::IsoSubspace&);
    JS_EXPORT_PRIVATE ~IsoSubspace() = default;

    size_t cellSize() { return m_localAllocator.cellSize(); }

    Allocator allocatorFor(size_t, AllocatorForMode);

    void* allocate(VM&, size_t, GCDeferralContext*, AllocationFailureMode);
    // SharedGC (SPEC-heap.md §12.1 seam; THREADS T4): same LocalAllocator as
    // allocate(VM&) — iso allocators are already per-client; the seam only
    // skips the VM coupling. Defined in IsoSubspaceInlines.h.
    void* allocateForClient(GCClient::Heap&, size_t, GCDeferralContext*, AllocationFailureMode);

    // SharedGC (§5.3; T4): registered lookup-only in the owning client's
    // GCThreadLocalCache m_perDirectory at materialization (covers iso for
    // the §10A.1 ownership predicate and §5.3 teardown).
    LocalAllocator& localAllocator() LIFETIME_BOUND { return m_localAllocator; }

    // H-ISO-TLCSLOT: server's stamped table slot, reached via the shared
    // BlockDirectory (m_localAllocator.directory().subspace() is the server
    // JSC::IsoSubspace; LocalAllocator binds it at ctor, IsoSubspace.cpp). Read
    // by tlcSlotForConcurrentlyWithIso<T> on JIT compilation threads — the
    // value is process-wide constant once stamped (first-client TLC ctor) and
    // identical across every client of the same server, so the IT-9 carve-out
    // (compile thread → vm.clientHeap's view) is harmless: any client's view
    // returns the same slot. Out-of-line (IsoSubspace.cpp) so this header need
    // not pull LocalAllocator.h.
    JS_EXPORT_PRIVATE unsigned tlcSlot() const;

private:
    LocalAllocator m_localAllocator;
};

ALWAYS_INLINE Allocator IsoSubspace::allocatorFor(size_t size, AllocatorForMode)
{
    RELEASE_ASSERT(size <= cellSize());
    return Allocator(&m_localAllocator);
}

} // namespace GCClient

#define ISO_SUBSPACE_INIT(heap, heapCellType, type) \
    ISO_SUBSPACE_INIT_WITH_NAME(heap, heapCellType, type, #type ""_s)

#define ISO_SUBSPACE_INIT_WITH_NAME(heap, heapCellType, type, name) (name, (heap), (heapCellType), sizeof(type), type::numberOfLowerTierPreciseCells)

} // namespace JSC

