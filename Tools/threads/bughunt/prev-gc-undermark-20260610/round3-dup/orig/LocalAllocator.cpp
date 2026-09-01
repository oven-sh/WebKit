/*
 * Copyright (C) 2018-2019 Apple Inc. All rights reserved.
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
#include "LocalAllocator.h"

#include "AllocatingScope.h"
#include "FreeListInlines.h"
#include "GCDeferralContext.h"
#include "LocalAllocatorInlines.h"
#include "MarkedSpaceInlines.h"
#include "Options.h"
#include "RaceAmplifier.h"
#include "ResourceExhaustion.h"
#include "SuperSampler.h"
#include <cstring>

namespace JSC {

// UNGIL §A.3 (U-T5) cross-TU seams — defined in runtime/VMManager.cpp;
// declaration pattern matches heap/Heap.cpp:151 and
// bytecode/JSThreadsSafepoint.cpp:71/77 (VMManager.h is outside the writable
// file set). Signatures must stay byte-identical.
bool jsThreadsThreadGranularWorldIsStopped(); // §A.3.2 post-quiescence depth.
bool jsThreadsCurrentThreadIsStopConductor(); // §A.3.3 tenure check.

#if ASSERT_ENABLED
// SharedGC (§5.2(4)): stop/resume/prepare/teardown may mutate an allocator's
// FreeList/current block only while the world is stopped for all clients
// (conductor-side, I2 exception), under MSPL (§5.3 TLC teardown), or when the
// server isn't shared (today's single-mutator rules; I10).
static void assertSharedAllocatorMutationIsSafe(BlockDirectory* directory)
{
    if (!directory)
        return;
    JSC::Heap& heap = directory->markedSpace().heap();
    if (!heap.isSharedServer())
        return;
    // AB18-D (V3, jit/int-gate-epoch-reclaim): a §A.3 thread-granular
    // JSThreads stop (jsThreadsThreadGranularStopTheWorldAndRun) is a stopped
    // world for allocator purposes — its §A.3.2 predicate parks every entered
    // mutator at a poll site, all of which sit OUTSIDE MSPL sections and BVL
    // holds (see allocateSlowCase's "never inside the lock" rule), and its
    // JSThreadsStopScope GCL bracket + STW-forbidden counter exclude any
    // shared GC — but it publishes neither heap witness (WSAC stays false;
    // no MSPL is taken). Accept the window's CONDUCTOR thread only, and only
    // once the §A.3.2 predicate is satisfied (s_jsThreadsWorldStoppedDepth is
    // bumped post-quiescence, after the access re-acquire) — the same
    // conductor-exemption shape as Heap.cpp:5781 and notifyVMStop's park leg
    // — so a pre-quiescence conductor touch or any non-conductor thread
    // mutating mid-window still trips.
    ASSERT(heap.worldIsStoppedForAllClients() || heap.mutatorSlowPathLock().isHeld() || (jsThreadsThreadGranularWorldIsStopped() && jsThreadsCurrentThreadIsStopConductor()));
}
#endif

LocalAllocator::LocalAllocator(BlockDirectory* directory)
    : m_directory(directory)
    , m_freeList(directory->m_cellSize)
{
    Locker locker { directory->m_localAllocatorsLock };
    directory->m_localAllocators.append(this);
}

void LocalAllocator::reset()
{
    m_freeList.clear();
    m_currentBlock = nullptr;
    m_lastActiveBlock = nullptr;
    m_allocationCursor = 0;
}

LocalAllocator::~LocalAllocator()
{
    if (isOnList()) {
        Locker locker { m_directory->m_localAllocatorsLock };
        remove();
    }
    
    bool ok = true;
    if (!m_freeList.allocationWillFail()) {
        dataLog("FATAL: ", RawPointer(this), "->~LocalAllocator has non-empty free-list.\n");
        ok = false;
    }
    if (m_currentBlock) {
        dataLog("FATAL: ", RawPointer(this), "->~LocalAllocator has non-null current block.\n");
        ok = false;
    }
    if (m_lastActiveBlock) {
        dataLog("FATAL: ", RawPointer(this), "->~LocalAllocator has non-null last active block.\n");
        ok = false;
    }
    RELEASE_ASSERT(ok);
}

void LocalAllocator::stopAllocating()
{
#if ASSERT_ENABLED
    assertSharedAllocatorMutationIsSafe(m_directory); // §5.2(4).
#endif
    ASSERT(!m_lastActiveBlock);
    if (!m_currentBlock) {
        ASSERT(m_freeList.allocationWillFail());
        return;
    }
    
    if (Options::validateFreeListStructure()) [[unlikely]] {
        RELEASE_ASSERT(m_freeList.isStructurallySoundWithin("stopAllocating",
            std::bit_cast<char*>(m_currentBlock->start()), std::bit_cast<char*>(m_currentBlock->end()),
            m_directory->cellSize(), MarkedBlock::atomsPerBlock));
    }
    m_currentBlock->stopAllocating(m_freeList);
    m_lastActiveBlock = m_currentBlock;
    m_currentBlock = nullptr;
    m_freeList.clear();
}

void LocalAllocator::resumeAllocating()
{
#if ASSERT_ENABLED
    assertSharedAllocatorMutationIsSafe(m_directory); // §5.2(4).
#endif
    if (!m_lastActiveBlock)
        return;

    m_lastActiveBlock->resumeAllocating(m_freeList);
    m_currentBlock = m_lastActiveBlock;
    m_lastActiveBlock = nullptr;
}

void LocalAllocator::prepareForAllocation()
{
#if ASSERT_ENABLED
    assertSharedAllocatorMutationIsSafe(m_directory); // §5.2(4).
#endif
    reset();
}

void LocalAllocator::stopAllocatingForGood()
{
    if (Options::validateFreeListStructure()) [[unlikely]] {
        // Provenance for the validator: tag teardown-path flushes unless a
        // more specific caller tag is already set.
        const char* previous = FreeList::structureValidationContext();
        if (!strcmp(previous, "other")) {
            FreeList::setStructureValidationContext("SAFG-other");
            stopAllocating();
            reset();
            FreeList::setStructureValidationContext(previous);
            return;
        }
    }
    stopAllocating();
    reset();
}

void* LocalAllocator::allocateSlowCase(JSC::Heap& heap, size_t cellSize, GCDeferralContext* deferralContext, AllocationFailureMode failureMode)
{
    SuperSamplerScope superSamplerScope(false);
    // SharedGC (§5.2(1)/I2): access-based ownership, not thread-pinned. !ISS
    // this is today's API-lock predicate (I10); once shared it checks the
    // §10A.1 TLS-stamped client's TLC membership.
    ASSERT(heap.currentThreadIsAllocatorOwner(this));
    doTestCollectionsIfNeeded(heap, deferralContext);

    ASSERT(!m_directory->markedSpace().isIterating());
    heap.didAllocate(m_freeList.originalSize());

    didConsumeFreeList();

    AllocatingScope helpingHeap(heap);

    heap.collectIfNecessaryOrDefer(deferralContext);

    // Goofy corner case: the GC called a callback and now this directory has a currentBlock. This only
    // happens when running WebKit tests, which inject a callback into the GC's finalization.
    // (Re-entering allocate() here is lock-free: the MutatorSlowPathLocker
    // below is intentionally not yet taken, so a recursive slow-path call
    // cannot self-deadlock on MSPL.)
    if (m_currentBlock) [[unlikely]]
        return allocate(heap, cellSize, deferralContext, failureMode);

    // SharedGC (§5.2): once the server is shared, the entire block-handout
    // slow path — bitvector cursor searches, empty-block steals (I8),
    // lower-tier precise allocation (§5.6), tryAllocateBlock and addBlock
    // (incl. its m_bits resize, I5b) — is serialized by the server's
    // mutator-slow-path lock (MSPL, rank 7). Resolves the old "FIXME
    // GlobalGC: Need to synchronize here when allocating from the
    // BlockDirectory in the server" (https://bugs.webkit.org/show_bug.cgi?id=181635):
    // one per-server lock over all mutator allocation slow paths, exactly the
    // protocol the FIXME proposed. Taken only AFTER collectIfNecessaryOrDefer
    // returns (L2) — never held across a collection request or stop. Option
    // off / single client: no-op locker, today's code (I10). In-lock block
    // sweeps are an accepted phase-1 carve-out (§3.7).
    // T10 amplifier hook (AMPLIFIER.md): widen the block-handout window
    // between collectIfNecessaryOrDefer returning and the MSPL acquisition —
    // a stop requested right here must park us via the next AHA/SINFAC poll,
    // never inside the lock.
    RaceAmplifier::perturb();

    MutatorSlowPathLocker mutatorSlowPathLocker(heap);

    void* result = tryAllocateWithoutCollecting(cellSize);

    if (result) [[likely]]
        return result;

    Subspace* subspace = m_directory->m_subspace;
    if (subspace->isIsoSubspace()) {
        // §5.2: MSPL also serializes tryAllocateLowerTierPrecise (it mutates
        // the precise registry; §5.6/I16).
        if (void* result = static_cast<IsoSubspace*>(subspace)->tryAllocateLowerTierPrecise(cellSize))
            return result;
    }

    ASSERT(!subspace->isPreciseOnly());
    ASSERT_WITH_MESSAGE(cellSize == m_directory->cellSize(), "non-preciseOnly allocations should match allocator's the size class");
    MarkedBlock::Handle* block = m_directory->tryAllocateBlock(mutatorSlowPathLocker, heap);
    if (!block) [[unlikely]] {
        RELEASE_ASSERT_RESOURCE_AVAILABLE(failureMode != AllocationFailureMode::Assert, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
        return nullptr;
    }
    m_directory->addBlock(block);
    result = allocateIn(block, cellSize);
    ASSERT(result);
    return result;
}

void LocalAllocator::didConsumeFreeList()
{
    if (m_currentBlock)
        m_currentBlock->didConsumeFreeList();
    
    m_freeList.clear();
    m_currentBlock = nullptr;
}

void* LocalAllocator::tryAllocateWithoutCollecting(size_t cellSize)
{
    // SharedGC (§5.2): the concurrency protocol the old FIXME here asked for
    // (https://bugs.webkit.org/show_bug.cgi?id=181635) is the single
    // per-server mutator-slow-path lock (MSPL), taken by our sole caller
    // allocateSlowCase() when the server is shared. It covers both cases the
    // FIXME enumerated: concurrent allocationCursor searches over one
    // directory's bitvectors (the per-search BVL below still publishes the
    // inUse/canAllocate flips, F1/I1), and steals through the subspace /
    // AlignedMemoryAllocator directory list (sweep + removeFromDirectory +
    // addBlock as one atomic step, I8).

    SuperSamplerScope superSamplerScope(false);

    ASSERT(!m_currentBlock);
    ASSERT(m_freeList.allocationWillFail());

#if ASSERT_ENABLED
    assertSharedAllocatorMutationIsSafe(m_directory); // MSPL held when shared (§5.2).
#endif

    for (;;) {
        MarkedBlock::Handle* block = m_directory->findBlockForAllocation(*this);
        if (!block)
            break;

        if (void* result = tryAllocateIn(block, cellSize))
            return result;
    }
    
    if (Options::stealEmptyBlocksFromOtherAllocators()) {
        if (MarkedBlock::Handle* block = m_directory->m_subspace->findEmptyBlockToSteal()) {
            RELEASE_ASSERT(block->alignedMemoryAllocator() == m_directory->m_subspace->alignedMemoryAllocator());

            // SharedGC (review round 4) — weak-bearing carve-out (rationale
            // at tryAllocateIn below / WeakSet::sweep): an EMPTY block can
            // still carry Dead-but-unfinalized WeakImpls; sweeping it here
            // (MSPL-held, world running) would run their finalizers
            // concurrently with the owning client's lock-free Weak<>
            // teardown. Decline the steal (clear inUse on the block's OWN
            // directory — findEmptyBlockToSteal set it there, which may not
            // be m_directory) and fall through to fresh-block allocation;
            // the block is reclaimed by the next world-stopped sweep.
            {
                JSC::Heap& heap = m_directory->markedSpace().heap();
                if (heap.isSharedServer() && !heap.worldIsStoppedForAllClients() && block->weakSet().head()) [[unlikely]] {
                    block->directory()->didFinishUsingBlock(block);
                    return nullptr;
                }
            }

            // T10 amplifier hook (AMPLIFIER.md): widen the steal window (I8)
            // — the block is chosen but not yet swept/re-homed. Deliberately
            // perturbs while MSPL is held (when shared): stressing the
            // single-lock handout protocol IS the point (AMPLIFIER.md rule 3).
            RaceAmplifier::perturb();

            block->sweep(nullptr);

            block->removeFromDirectory();
            m_directory->addBlock(block);
            return allocateIn(block, cellSize);
        }
    }
    
    return nullptr;
}

void* LocalAllocator::allocateIn(MarkedBlock::Handle* block, size_t cellSize)
{
    void* result = tryAllocateIn(block, cellSize);
    RELEASE_ASSERT(result);
    return result;
}

void* LocalAllocator::tryAllocateIn(MarkedBlock::Handle* block, size_t cellSize)
{
    ASSERT(block);
    ASSERT(!block->isFreeListed());
    m_directory->assertIsMutatorOrMutatorIsStopped();
    ASSERT(m_directory->isInUse(block));

    // SharedGC (review round 4) — weak-bearing carve-out: a mutator-
    // concurrent (MSPL-held, world running) sweep must not touch a block
    // whose WeakSet has any WeakBlocks. The sweep below would re-sweep that
    // WeakSet (rewriting WeakImpl states/freelists the API-lock-holding
    // client reads via Weak<>::clear, which is deliberately lock-free) and
    // run weak FINALIZERS on this thread while the owning client may be
    // concurrently deallocating the very handle being finalized. Such
    // blocks stay unswept and park until the next world-stopped sweep
    // (conducted cycle / teardown); canAllocate was already cleared by
    // findBlockForAllocation, so the caller's loop moves on — no livelock.
    // Reading weakSet().head() is stable here: WeakSet::allocate mutates it
    // only under MSPL, which we hold (weak-mutation protocol,
    // WeakSet::sweep). Unreachable via allocateIn(): fresh blocks have no
    // WeakBlocks and the steal path pre-checks the same predicate under the
    // same MSPL hold.
    {
        JSC::Heap& heap = m_directory->markedSpace().heap();
        if (heap.isSharedServer() && !heap.worldIsStoppedForAllClients() && block->weakSet().head()) [[unlikely]] {
            m_directory->didFinishUsingBlock(block);
            return nullptr;
        }
    }

    block->sweep(&m_freeList);

    // It's possible to stumble on a completely full block. Marking tries to retire these, but
    // that algorithm is racy and may forget to do it sometimes.
    if (m_freeList.allocationWillFail()) {
        ASSERT(block->isFreeListed());
        block->unsweepWithNoNewlyAllocated();
        ASSERT(!block->isFreeListed());
        ASSERT(!m_directory->isEmpty(block));
        ASSERT(!m_directory->isCanAllocate(block));
        return nullptr;
    }
    
    m_currentBlock = block;

    void* result = m_freeList.allocateWithCellSize(
        []() -> HeapCell* {
            RELEASE_ASSERT_NOT_REACHED();
            return nullptr;
        }, cellSize);

    if (m_directory->markedSpace().heap().isSharedServer()) [[unlikely]] {
        // SharedGC (§5.2(2)/I5b/F1): the eden flip publishes via the
        // directory's bitvector lock — the sweeper and other clients' cursor
        // searches read these words concurrently once shared.
        Locker locker { m_directory->bitvectorLock() };
        m_directory->setIsEden(m_currentBlock->index(), true);
    } else {
        // FIXME: We should make this work with thread safety analysis.
        m_directory->m_bits.setIsEden(m_currentBlock->index(), true);
    }
    // §5.2(2): this call is inside allocateSlowCase's MSPL section when
    // shared (didAllocateInBlock debug-asserts that).
    m_directory->markedSpace().didAllocateInBlock(m_currentBlock);
    if (Options::validateFreeListStructure()) [[unlikely]] {
        RELEASE_ASSERT(m_freeList.isStructurallySoundWithin("refill",
            std::bit_cast<char*>(m_currentBlock->start()), std::bit_cast<char*>(m_currentBlock->end()),
            m_directory->cellSize(), MarkedBlock::atomsPerBlock));
    }
    return result;
}

void LocalAllocator::doTestCollectionsIfNeeded(JSC::Heap& heap, GCDeferralContext* deferralContext)
{
    if (!Options::slowPathAllocsBetweenGCs()) [[likely]]
        return;

    static unsigned allocationCount = 0;
    if (!allocationCount) {
        if (!heap.isDeferred()) {
            if (deferralContext)
                deferralContext->m_shouldGC = true;
            else
                heap.collectNow(Sync, CollectionScope::Full);
        }
    }
    if (++allocationCount >= Options::slowPathAllocsBetweenGCs())
        allocationCount = 0;
}

} // namespace JSC

