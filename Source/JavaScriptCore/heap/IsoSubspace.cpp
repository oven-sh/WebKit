/*
 * Copyright (C) 2017-2023 Apple Inc. All rights reserved.
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
#include "IsoSubspace.h"

#include "FastMallocAlignedMemoryAllocator.h"
#include "IsoCellSetInlines.h"
#include "JSCellInlines.h"
#include "MarkedSpaceInlines.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(IsoSubspace);

IsoSubspace::IsoSubspace(CString name, JSC::Heap& heap, const HeapCellType& heapCellType, size_t size, uint8_t numberOfLowerTierPreciseCells, std::unique_ptr<AlignedMemoryAllocator>&& allocator)
    : Subspace(SubspaceKind::IsoSubspace, name, heap)
    , m_directory(heap, WTF::roundUpToMultipleOf<MarkedBlock::atomSize>(size))
    , m_allocator(allocator ? WTF::move(allocator) : makeUnique<FastMallocAlignedMemoryAllocator>())
{
    m_remainingLowerTierPreciseCount = numberOfLowerTierPreciseCells;
    ASSERT(WTF::roundUpToMultipleOf<MarkedBlock::atomSize>(size) == cellSize());
    ASSERT(m_remainingLowerTierPreciseCount <= MarkedBlock::maxNumberOfLowerTierPreciseCells);

    initialize(heapCellType, m_allocator.get());

    Locker locker { m_space.directoryLock() };
    m_directory.setSubspace(this);
    m_space.addBlockDirectory(locker, &m_directory);
    m_alignedMemoryAllocator->registerDirectory(heap, &m_directory);
    m_firstDirectory = &m_directory;
}

IsoSubspace::~IsoSubspace() = default;

void IsoSubspace::didResizeBits(unsigned blockIndex)
{
    m_cellSets.forEach(
        [&] (IsoCellSet* set) {
            set->didResizeBits(blockIndex);
        });
}

void IsoSubspace::didRemoveBlock(unsigned blockIndex)
{
    m_cellSets.forEach(
        [&] (IsoCellSet* set) {
            set->didRemoveBlock(blockIndex);
        });
}

void IsoSubspace::didBeginSweepingToFreeList(MarkedBlock::Handle* block)
{
    m_cellSets.forEach(
        [&] (IsoCellSet* set) {
            set->sweepToFreeList(block);
        });
}

void* IsoSubspace::tryAllocateLowerTierPrecise(size_t size)
{
    // SharedGC (§5.2/§5.6/I16): mutates the precise registry and the
    // lower-tier free list; when the server is shared this runs inside
    // LocalAllocator::allocateSlowCase's MSPL section (the sole mutator-path
    // caller) — assert rather than re-lock (MSPL is not recursive).
    // T7-mspl-per-directory: the caller's MSPL token may now be a
    // per-directory STRIPE. The per-iso-subspace state below
    // (m_lowerTierPreciseFreeList, m_remainingLowerTierPreciseCount,
    // Subspace::m_preciseAllocations) is single-directory — two clients
    // refilling the SAME iso directory serialize on m_directory.m_refillLock,
    // and refills of OTHER directories never reach this subspace's state. The
    // CROSS-directory MarkedSpace precise registry
    // (registerPreciseAllocation: m_preciseAllocations vector +
    // m_preciseAllocationSet + indexInSpace stamps) IS shared, so leaf-lock
    // it under Heap::m_markedSpaceRegistryLock (rank 7r). The other precise
    // registrars (CompleteSubspace::tryAllocateSlow*, PreciseSubspace,
    // reallocatePreciseAllocationNonVirtual, enablePreciseAllocationTracking)
    // all hold the EXCLUSIVE facade side, which excludes every stripe and so
    // excludes us — the registry lock here only contends with sibling
    // stripes' lower-tier-precise (rare: bounded by
    // numberOfLowerTierPreciseCells per subspace, then never again) and with
    // didAddBlock / didAllocateInBlock. Flag-off / !isSharedServer(): not
    // taken.
    ASSERT(!m_space.heap().isSharedServer() || m_space.heap().mutatorSlowPathLock().isHeld() || m_space.heap().worldIsStoppedForAllClients());

    if (m_space.heap().isSharedServer()) [[unlikely]] {
        Locker locker { m_space.heap().markedSpaceRegistryLock() };
        return tryAllocateLowerTierPreciseImpl(size);
    }
    return tryAllocateLowerTierPreciseImpl(size);
}

void* IsoSubspace::tryAllocateLowerTierPreciseImpl(size_t size)
{
    auto revive = [&] (PreciseAllocation* allocation) {
        // Lower-tier cells never report capacity. This is intentional since it will not be freed until VM dies.
        // Whether we will do GC or not does not affect on the used memory by lower-tier cells. So we should not
        // count them in capacity since it is not interesting to decide whether we should do GC.
        m_preciseAllocations.append(allocation);
        m_space.registerPreciseAllocation(allocation, /* isNewAllocation */ false);
        ASSERT(allocation->indexInSpace() == m_space.m_preciseAllocations.size() - 1);
        return allocation->cell();
    };

    ASSERT_WITH_MESSAGE(cellSize() == size, "non-preciseOnly IsoSubspaces shouldn't have variable size");
    if (!m_lowerTierPreciseFreeList.isEmpty()) {
        PreciseAllocation* allocation = &*m_lowerTierPreciseFreeList.begin();
        allocation->remove();
        return revive(allocation);
    }
    if (m_remainingLowerTierPreciseCount) {
        PreciseAllocation* allocation = PreciseAllocation::tryCreateForLowerTierPrecise(m_space.heap(), size, this, --m_remainingLowerTierPreciseCount);
        if (allocation)
            return revive(allocation);
    }
    return nullptr;
}

void IsoSubspace::sweepLowerTierPreciseCell(PreciseAllocation* preciseAllocation)
{
    preciseAllocation = preciseAllocation->reuseForLowerTierPrecise();
    m_lowerTierPreciseFreeList.append(preciseAllocation);
}

void IsoSubspace::destroyLowerTierPreciseFreeList()
{
    m_lowerTierPreciseFreeList.forEach([&](PreciseAllocation* allocation) {
        allocation->destroy();
    });
}

namespace GCClient {

// SharedGC (T9) audit vs the GCClient::Heap::vm() standalone assert
// (HeapInlines.h): GCClient::IsoSubspace never touches its owning client's
// vm() — this ctor binds only the server-side BlockDirectory, allocate(VM&)
// is the VM-coupled entry (a VM exists by definition there), and
// allocateForClient() (§12.1 seam, IsoSubspaceInlines.h) reaches the server
// via client.server(). Standalone (markStandalone()) clients can therefore
// materialize and use iso LocalAllocators without tripping the vm()
// RELEASE_ASSERT; the §10A.1 m_perDirectory registration (T4) is likewise
// vm()-free.
IsoSubspace::IsoSubspace(JSC::IsoSubspace& server)
    : m_localAllocator(&server.m_directory)
{
}

unsigned IsoSubspace::tlcSlot() const
{
    // H-ISO-TLCSLOT: server reach-through. m_localAllocator.directory() is the
    // server's single BlockDirectory (bound at ctor above); its subspace() is
    // the server JSC::IsoSubspace. The slot is write-once (stamped by the
    // first GCThreadLocalCache ctor, serially during VM construction) and read
    // here only at JIT compile time behind a vm.gilOff() codegen gate, so a
    // plain load suffices — the first client's stamp happens-before any
    // concurrent compile (compilation requires an entered VM; entry implies
    // the stamping client exists). Subspaces never enumerated by
    // FOR_EACH_JSC_ISO_SUBSPACE return invalidTlcIndex and the caller
    // degrades to the legacy null-bake.
    Subspace* server = m_localAllocator.directory().subspace();
    ASSERT(server && server->isIsoSubspace());
    return static_cast<const JSC::IsoSubspace*>(server)->tlcSlot();
}

} // namespace GCClient

} // namespace JSC

