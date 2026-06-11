/*
 * Copyright (C) 2017-2024 Apple Inc. All rights reserved.
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
#include "CompleteSubspace.h"

#include "AlignedMemoryAllocator.h"
#include "AllocatorInlines.h"
#include "JSCellInlines.h"
#include "LocalAllocatorInlines.h"
#include "MarkedSpaceInlines.h"
#include "RaceAmplifier.h"
#include "ResourceExhaustion.h"
#include "SubspaceInlines.h"
#include <wtf/RAMSize.h>

namespace JSC {

CompleteSubspace::CompleteSubspace(CString name, JSC::Heap& heap, const HeapCellType& heapCellType, AlignedMemoryAllocator* alignedMemoryAllocator)
    : Subspace(SubspaceKind::CompleteSubspace, name, heap)
{
    initialize(heapCellType, alignedMemoryAllocator);
}

CompleteSubspace::~CompleteSubspace() = default;

Allocator CompleteSubspace::allocatorForNonInline(size_t size, AllocatorForMode mode)
{
    return allocatorFor(size, mode);
}

BlockDirectory* CompleteSubspace::ensureDirectoryForSizeStepSlow(size_t sizeStepIndex)
{
    ASSERT(sizeStepIndex < MarkedSpace::numSizeClasses);
    size_t sizeClass = MarkedSpace::s_sizeClassForSizeStep[sizeStepIndex];
    if (!sizeClass)
        return nullptr;
    Locker locker { m_space.directoryLock() };
    return ensureDirectoryForSizeClass(locker, sizeClass);
}

BlockDirectory* CompleteSubspace::ensureDirectoryForSizeClass(const AbstractLocker& locker, size_t sizeClass)
{
    // This is written in such a way that it's OK for the JIT threads to end up here if they want
    // to generate code that uses some allocator that hadn't been used yet. Note that a possibly-
    // just-as-good solution would be to return null if we're in the JIT since the JIT treats null
    // allocator as "please always take the slow path". But, that could lead to performance
    // surprises and the algorithm here is pretty easy. Only this code has to hold the lock, to
    // prevent simultaneously BlockDirectory creations from multiple threads. This code ensures
    // that any "forEachAllocator" traversals will only see this allocator after it's initialized
    // enough: it will have
    ASSERT(sizeClass);
    size_t index = MarkedSpace::sizeClassToIndex(sizeClass);
    ASSERT(MarkedSpace::s_sizeClassForSizeStep[index] == sizeClass);
    // Relaxed is sufficient: all writers hold directoryLock, which supplies
    // happens-before for this locked reader.
    if (BlockDirectory* directory = m_directoryForSizeStep[index].load(std::memory_order_relaxed))
        return directory;

    if (false)
        dataLog("Creating BlockDirectory for ", m_name, ", ", attributes(), ", ", sizeClass, ".\n");

    // SharedGC (§5.3; T4): reserve this subspace's contiguous TLC slot range
    // on first directory creation (monotonic, never reused; directoryLock
    // only — JIT-thread-safe, no MSPL).
    if (m_tlcIndexBase.load(std::memory_order_relaxed) == BlockDirectory::invalidTlcIndex)
        m_tlcIndexBase.store(m_space.reserveThreadLocalCacheIndices(locker), std::memory_order_release);

    std::unique_ptr<BlockDirectory> uniqueDirectory = makeUnique<BlockDirectory>(m_space.heap(), sizeClass);
    BlockDirectory* directory = uniqueDirectory.get();
    m_directories.append(WTF::move(uniqueDirectory));

    directory->setSubspace(this);
    directory->setTlcIndex(m_tlcIndexBase.load(std::memory_order_relaxed) + static_cast<unsigned>(index));
    m_space.addBlockDirectory(locker, directory);

    // Publish the per-size-step directory entries only after the directory is
    // fully initialized: the TLC slow path reads m_directoryForSizeStep
    // without the lock (null => it takes this lock). Per-entry release stores
    // pair with the acquire load in ensureDirectoryForSizeStep, ordering all
    // of the initialization above before each entry becomes visible.
    size_t fillIndex = index;
    for (;;) {
        if (MarkedSpace::s_sizeClassForSizeStep[fillIndex] != sizeClass)
            break;
        m_directoryForSizeStep[fillIndex].store(directory, std::memory_order_release);
        if (!fillIndex--)
            break;
    }

    directory->setNextDirectoryInSubspace(m_firstDirectory);
    m_alignedMemoryAllocator->registerDirectory(m_space.heap(), directory);
    WTF::storeStoreFence();
    m_firstDirectory = directory;
    return directory;
}

Allocator CompleteSubspace::allocatorForSlow(size_t size)
{
    size_t index = MarkedSpace::sizeClassToIndex(size);
    size_t sizeClass = MarkedSpace::s_sizeClassForSizeStep[index];
    if (!sizeClass)
        return Allocator();

    Locker locker { m_space.directoryLock() };

    if (Options::useSharedGCHeap()) [[unlikely]] {
        // SharedGC (§5.5 never-populate rule; T4): with the shared-heap
        // option on, no server-side non-iso Allocator/LocalAllocator is EVER
        // materialized — m_allocatorForSizeStep stays null, so every JS-tier
        // inline-allocation emitter bakes/loads null and slow-paths into
        // CompleteSubspace::allocate, which routes through the caller's TLC
        // (§5.3). The directory itself is still created (JIT threads may land
        // here wanting code for a not-yet-used size class);
        // verifyNoAllocatorsMaterialized() audits at the second-client attach.
        ensureDirectoryForSizeClass(locker, sizeClass);
        return Allocator();
    }

    if (Allocator allocator = m_allocatorForSizeStep[index])
        return allocator;

    BlockDirectory* directory = ensureDirectoryForSizeClass(locker, sizeClass);
    ASSERT(directory);

    if (false)
        dataLog("Creating LocalAllocator for ", m_name, ", ", attributes(), ", ", sizeClass, ".\n");

    std::unique_ptr<LocalAllocator> uniqueLocalAllocator =
        makeUnique<LocalAllocator>(directory);
    LocalAllocator* localAllocator = uniqueLocalAllocator.get();
    m_localAllocators.append(WTF::move(uniqueLocalAllocator));

    Allocator allocator(localAllocator);

    index = MarkedSpace::sizeClassToIndex(sizeClass);
    for (;;) {
        if (MarkedSpace::s_sizeClassForSizeStep[index] != sizeClass)
            break;

        m_allocatorForSizeStep[index] = allocator;

        if (!index--)
            break;
    }

    return allocator;
}

void CompleteSubspace::verifyNoAllocatorsMaterialized()
{
    // SharedGC (§5.5; T4): RELEASE_ASSERTed at the second-client attach.
    Locker locker { m_space.directoryLock() };
    for (size_t i = 0; i < MarkedSpace::numSizeClasses; ++i)
        RELEASE_ASSERT(!m_allocatorForSizeStep[i]);
    RELEASE_ASSERT(m_localAllocators.isEmpty());
}

void* CompleteSubspace::allocateSlow(VM& vm, size_t size, GCDeferralContext* deferralContext, AllocationFailureMode failureMode)
{
    void* result = tryAllocateSlow(vm, size, deferralContext);
    if (!result) [[unlikely]]
        RELEASE_ASSERT_RESOURCE_AVAILABLE(failureMode != AllocationFailureMode::Assert, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
    return result;
}

void* CompleteSubspace::tryAllocateSlow(VM& vm, size_t size, GCDeferralContext* deferralContext)
{
    if constexpr (validateDFGDoesGC)
        vm.verifyCanGC();

    sanitizeStackForVM(vm);

    // SharedGC (§5.3/§5.5; T4): with the option on, the server allocator
    // table is never populated — route through the calling thread's client
    // TLC (the VM-coupled preludes above already ran; the client overload
    // skips them for standalone clients, §12.1). SPEC-ungil §B / I4: GIL-off
    // the CURRENT thread's client, not unconditionally vm.clientHeap
    // (Heap::allocationClientForCurrentThread is identity GIL-on/flag-off);
    // the currentThreadClient() ASSERT in tryAllocateSlowForClient is the
    // per-site verification anchor.
    if (Options::useSharedGCHeap()) [[unlikely]]
        return tryAllocateSlowForClient(Heap::allocationClientForCurrentThread(vm, vm.clientHeap), size, deferralContext);

    if (Allocator allocator = allocatorForNonInline(size, AllocatorForMode::EnsureAllocator))
        return allocator.allocate(vm.heap, allocator.cellSize(), deferralContext, AllocationFailureMode::ReturnNull);
    
    if (size <= Options::preciseAllocationCutoff()
        && size <= MarkedSpace::largeCutoff) {
        dataLog("FATAL: attampting to allocate small object using large allocation.\n");
        dataLog("Requested allocation size: ", size, "\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    vm.heap.collectIfNecessaryOrDefer(deferralContext);
    if (Options::maxHeapSizeAsRAMSizeMultiple()) [[unlikely]] {
        if (vm.heap.capacity() > static_cast<uint64_t>(Options::maxHeapSizeAsRAMSizeMultiple()) * static_cast<uint64_t>(WTF::ramSize()))
            return nullptr;
    }

    size = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(size);

    // SharedGC (§5.6/I16): precise-allocation creation + registration is one
    // MSPL critical section when the server is shared — the indexInSpace
    // passed to tryCreate is read from m_preciseAllocations.size() and must
    // stay consistent through registerPreciseAllocation. Locked only AFTER
    // collectIfNecessaryOrDefer above (L2). No-op when !isSharedServer (I10).
    MutatorSlowPathLocker mutatorSlowPathLocker(vm.heap);

    // T10 amplifier hook (AMPLIFIER.md): widen the precise-registration
    // window (I16) — indexInSpace is about to be read from the registry size
    // and must stay consistent through registerPreciseAllocation. Perturbs
    // under MSPL when shared (stressing §5.6's critical section is the
    // point; AMPLIFIER.md rule 3).
    RaceAmplifier::perturb();

    PreciseAllocation* allocation = PreciseAllocation::tryCreate(vm.heap, size, this, m_space.m_preciseAllocations.size());
    if (!allocation)
        return nullptr;

    m_preciseAllocations.append(allocation);
    m_space.registerPreciseAllocation(allocation, /* isNewAllocation */ true);
    return allocation->cell();
}

void* CompleteSubspace::allocateSlowForClient(GCClient::Heap& client, size_t size, GCDeferralContext* deferralContext, AllocationFailureMode failureMode)
{
    void* result = tryAllocateSlowForClient(client, size, deferralContext);
    if (!result) [[unlikely]]
        RELEASE_ASSERT_RESOURCE_AVAILABLE(failureMode != AllocationFailureMode::Assert, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
    return result;
}

void* CompleteSubspace::tryAllocateSlowForClient(GCClient::Heap& client, size_t size, GCDeferralContext* deferralContext)
{
    // SharedGC (§12.1 seam; T4): no VM-coupled preludes here — verifyCanGC /
    // sanitizeStackForVM run in the VM-taking overload before it delegates;
    // standalone harness clients have no VM. collectIfNecessaryOrDefer IS
    // called (I17 consults the calling client's deferral depth via the
    // §10A.1 TLS stamp).
    ASSERT(Options::useSharedGCHeap());
    JSC::Heap& heap = client.server();
    ASSERT(&heap == &m_space.heap());
    ASSERT(!heap.isSharedServer() || GCClient::Heap::currentThreadClient() == &client);

    if (size <= MarkedSpace::largeCutoff) {
        // "Ensure" semantics (§5.3): the TLC materializes this client's
        // LocalAllocator over the shared directory on first use; null only
        // when the size step has no size class (precise path below).
        if (Allocator allocator = client.threadLocalCache().allocatorForSizeStep(*this, MarkedSpace::sizeClassToIndex(size)))
            return allocator.allocate(heap, allocator.cellSize(), deferralContext, AllocationFailureMode::ReturnNull);
    }

    if (size <= Options::preciseAllocationCutoff()
        && size <= MarkedSpace::largeCutoff) {
        dataLog("FATAL: attampting to allocate small object using large allocation.\n");
        dataLog("Requested allocation size: ", size, "\n");
        RELEASE_ASSERT_NOT_REACHED();
    }

    heap.collectIfNecessaryOrDefer(deferralContext);
    if (Options::maxHeapSizeAsRAMSizeMultiple()) [[unlikely]] {
        if (heap.capacity() > static_cast<uint64_t>(Options::maxHeapSizeAsRAMSizeMultiple()) * static_cast<uint64_t>(WTF::ramSize()))
            return nullptr;
    }

    size = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(size);

    // SharedGC (§5.6/I16): precise-allocation creation + registration is one
    // MSPL critical section when the server is shared; locked only AFTER
    // collectIfNecessaryOrDefer above (L2). No-op when !isSharedServer (I10).
    MutatorSlowPathLocker mutatorSlowPathLocker(heap);

    // T10 amplifier hook: same §5.6/I16 window as tryAllocateSlow above, on
    // the §12.1 client seam.
    RaceAmplifier::perturb();

    PreciseAllocation* allocation = PreciseAllocation::tryCreate(heap, size, this, m_space.m_preciseAllocations.size());
    if (!allocation)
        return nullptr;

    m_preciseAllocations.append(allocation);
    m_space.registerPreciseAllocation(allocation, /* isNewAllocation */ true);
    return allocation->cell();
}

void* CompleteSubspace::reallocatePreciseAllocationNonVirtual(VM& vm, HeapCell* oldCell, size_t size, GCDeferralContext* deferralContext, AllocationFailureMode failureMode)
{
    if constexpr (validateDFGDoesGC)
        vm.verifyCanGC();

    // The following conditions are met in Butterfly for example.
    ASSERT(oldCell->isPreciseAllocation());

    PreciseAllocation* oldAllocation = &oldCell->preciseAllocation();
    ASSERT(oldAllocation->cellSize() <= size);
    ASSERT(oldAllocation->weakSet().isTriviallyDestructible());
    ASSERT(oldAllocation->attributes().destruction == DoesNotNeedDestruction);
    ASSERT(oldAllocation->attributes().cellKind == HeapCell::Auxiliary);
    ASSERT(size > MarkedSpace::largeCutoff);

    sanitizeStackForVM(vm);

    if (size <= Options::preciseAllocationCutoff() && size <= MarkedSpace::largeCutoff) [[unlikely]] {
        dataLog("FATAL: attampting to allocate small object using large allocation.\n");
        dataLog("Requested allocation size: ", size, "\n");
        RELEASE_ASSERT_NOT_REACHED();
    }

    vm.heap.collectIfNecessaryOrDefer(deferralContext);

    // SharedGC (§5.6/I16): the reallocation below mutates the precise
    // registry (subspace list removal/re-append, preciseAllocationSet
    // remove/add, m_preciseAllocations[index] re-point, indexInSpace stamps)
    // — one MSPL critical section when shared, locked after the
    // collectIfNecessaryOrDefer above (L2).
    MutatorSlowPathLocker mutatorSlowPathLocker(vm.heap);

    size = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(size);
    size_t difference = size - oldAllocation->cellSize();
    unsigned oldIndexInSpace = oldAllocation->indexInSpace();
    if (oldAllocation->isOnList())
        oldAllocation->remove();

    PreciseAllocation* allocation = oldAllocation->tryReallocate(size, this);
    if (!allocation) [[unlikely]] {
        RELEASE_ASSERT_RESOURCE_AVAILABLE(failureMode != AllocationFailureMode::Assert, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
        m_preciseAllocations.append(oldAllocation);
        return nullptr;
    }
    ASSERT(oldIndexInSpace == allocation->indexInSpace());

    // If reallocation changes the address, we should update HashSet.
    if (oldAllocation != allocation) {
        if (auto& set = m_space.preciseAllocationSet()) {
            set->remove(oldAllocation->cell());
            set->add(allocation->cell());
        }
    }

    m_space.m_preciseAllocations[oldIndexInSpace] = allocation;
    vm.heap.didAllocate(difference);
    m_space.m_capacity.fetch_add(difference, std::memory_order_relaxed); // §5.4/F3.

    m_preciseAllocations.append(allocation);

    return allocation->cell();
}

void CompleteSubspace::prepareAllAllocators()
{
    // SharedGC (§5.5; T4): wasm BBQ/OMG inline allocation memcpys server
    // LocalAllocator*s out of m_allocatorForSizeStep (sole caller:
    // JSWebAssemblyInstance's hasGCObjectTypes() ctor block) and its emitters
    // assume non-null — unsupported when the heap may be shared. The
    // INTEGRATE-heap.md manifest item 11 rejects wasm-GC instantiation under
    // the option; this backstops any other caller.
    RELEASE_ASSERT(!Options::useSharedGCHeap());

    for (unsigned i = MarkedSpace::numSizeClasses - 1; i--;) {
        if (!m_allocatorForSizeStep[i])
            allocatorForSlow(MarkedSpace::s_sizeClassForSizeStep[i]);
        ASSERT(m_allocatorForSizeStep[i]);
    }
}

} // namespace JSC

