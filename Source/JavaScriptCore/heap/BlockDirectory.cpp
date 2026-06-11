/*
 * Copyright (C) 2012-2024 Apple Inc. All rights reserved.
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
#include "BlockDirectory.h"

#include "BlockDirectoryInlines.h"
#include "FreeList.h"
#include "Heap.h"
#include "HeapInlines.h"
#include "MarkedSpaceInlines.h"
#include "SubspaceInlines.h"
#include "SuperSampler.h"

#include <wtf/FunctionTraits.h>
#include <wtf/Lock.h>
#include <wtf/SimpleStats.h>

namespace JSC {

// UNGIL §A.3 (U-T5) cross-TU seams — defined in runtime/VMManager.cpp;
// declaration pattern matches heap/Heap.cpp:151 and
// bytecode/JSThreadsSafepoint.cpp:71/77. Signatures must stay byte-identical.
bool jsThreadsThreadGranularWorldIsStopped(); // §A.3.2 post-quiescence depth.
bool jsThreadsCurrentThreadIsStopConductor(); // §A.3.3 tenure check.

namespace BlockDirectoryInternal {
static constexpr bool verbose = false;
}

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(BlockDirectory);


BlockDirectory::BlockDirectory(Heap& heap, size_t cellSize)
    : m_heap(heap)
    , m_cellSize(static_cast<unsigned>(cellSize))
{
    // THREADS/TSAN: see the member declarations.
    WTF::atomicStore(&m_nextDirectory, static_cast<BlockDirectory*>(nullptr), std::memory_order_relaxed);
    WTF::atomicStore(&m_nextDirectoryInSubspace, static_cast<BlockDirectory*>(nullptr), std::memory_order_relaxed);
    WTF::atomicStore(&m_nextDirectoryInAlignedMemoryAllocator, static_cast<BlockDirectory*>(nullptr), std::memory_order_relaxed);
}

BlockDirectory::~BlockDirectory()
{
    Locker locker { m_localAllocatorsLock };
    while (!m_localAllocators.isEmpty())
        m_localAllocators.begin()->remove();
}

void BlockDirectory::detachLocalAllocator(LocalAllocator& allocator)
{
    // SharedGC (§5.3 teardown/I9; THREADS T4): called from
    // GCThreadLocalCache::stopAllocatingForGood() with the server MSPL held
    // when the server is shared (lock order 7 -> 8).
    ASSERT(&allocator.directory() == this);
    Locker locker { m_localAllocatorsLock };
    if (allocator.isOnList())
        allocator.remove();
}

void BlockDirectory::setSubspace(Subspace* subspace)
{
    m_attributes = subspace->attributes();
    m_subspace = subspace;
}

void BlockDirectory::updatePercentageOfPagedOutPages(SimpleStats& stats)
{
    // FIXME: We should figure out a solution for Windows and PlayStation.
    // QNX doesn't have mincore(), though the information can be had. But since all mapped
    // pages are resident, does it matter?
#if OS(UNIX) && !PLATFORM(PLAYSTATION) && !OS(QNX) && !OS(HAIKU)
    size_t pageSize = WTF::pageSize();
    ASSERT(!(MarkedBlock::blockSize % pageSize));
    auto numberOfPagesInMarkedBlock = MarkedBlock::blockSize / pageSize;
    // For some reason this can be unsigned char or char on different OSes...
    using MincoreBufferType = std::remove_pointer_t<FunctionTraits<decltype(mincore)>::ArgumentType<2>>;
    static_assert(std::is_same_v<std::make_unsigned_t<MincoreBufferType>, unsigned char>);
    Vector<MincoreBufferType, 16> pagedBits(FillWith { }, numberOfPagesInMarkedBlock, MincoreBufferType { });

    for (auto* handle : m_blocks) {
        if (!handle)
            continue;

        auto* pageStart = handle->pageStart();
        auto markedBlockSizeInBytes = handle->backingStorageSize();
        RELEASE_ASSERT(markedBlockSizeInBytes / pageSize <= numberOfPagesInMarkedBlock);
        // We could cache this in bulk (e.g. 25 MB chunks) but we haven't seen any data that it actually matters.
        auto result = mincore(pageStart, markedBlockSizeInBytes, pagedBits.mutableSpan().data());
        RELEASE_ASSERT(!result);
        constexpr unsigned pageIsResidentAndNotCompressed = 1;
        for (unsigned i = 0; i < numberOfPagesInMarkedBlock; ++i)
            stats.add(!(pagedBits[i] & pageIsResidentAndNotCompressed));
    }
#else
    UNUSED_PARAM(stats);
#endif
}

MarkedBlock::Handle* BlockDirectory::findEmptyBlockToSteal()
{
    Locker locker(bitvectorLock());
    m_emptyCursor = (emptyBits() & ~inUseBits()).findBit(m_emptyCursor, true);
    if (m_emptyCursor >= m_blocks.size())
        return nullptr;
    dataLogLnIf(BlockDirectoryInternal::verbose, "Setting block ", m_emptyCursor, " in use (findEmptyBlockToSteal) for ", *this);
    setIsInUse(m_emptyCursor, true);
    return m_blocks[m_emptyCursor];
}

MarkedBlock::Handle* BlockDirectory::findBlockForAllocation(LocalAllocator& allocator)
{
    Locker locker(bitvectorLock());
    for (;;) {
        allocator.m_allocationCursor = (canAllocateBits() & ~inUseBits()).findBit(allocator.m_allocationCursor, true);
        if (allocator.m_allocationCursor >= m_blocks.size())
            return nullptr;
        
        unsigned blockIndex = allocator.m_allocationCursor++;
        MarkedBlock::Handle* result = m_blocks[blockIndex];
        setIsCanAllocate(blockIndex, false);
        dataLogLnIf(BlockDirectoryInternal::verbose, "Setting block ", blockIndex, " in use (findBlockForAllocation) for ", *this);
        setIsInUse(blockIndex, true);
        return result;
    }
}

MarkedBlock::Handle* BlockDirectory::tryAllocateBlock(const AbstractLocker& mutatorSlowPathLocker, JSC::Heap& heap)
{
    // SharedGC (§5.2(3)): mutatorSlowPathLocker is the caller's MSPL token
    // (LocalAllocator::allocateSlowCase); when shared, block creation and the
    // didAddBlock registration are serialized server-side.
    UNUSED_PARAM(mutatorSlowPathLocker);
    ASSERT(!heap.isSharedServer() || heap.mutatorSlowPathLock().isHeld());

    MarkedBlock::Handle* handle = MarkedBlock::tryCreate(heap, subspace()->alignedMemoryAllocator());
    if (!handle)
        return nullptr;

    markedSpace().didAddBlock(handle);

    return handle;
}

void BlockDirectory::addBlock(MarkedBlock::Handle* block)
{
#if ASSERT_ENABLED
    {
        // SharedGC (§5.2(5)/I5b): when shared, callers hold the server MSPL —
        // the m_blocks/m_bits resize below must not race other mutators'
        // bitvector readers that only hold this directory's BVL transiently
        // (m_bits reallocation is the I5b writer).
        JSC::Heap& heap = markedSpace().heap();
        ASSERT(!heap.isSharedServer() || heap.mutatorSlowPathLock().isHeld() || heap.worldIsStoppedForAllClients());
    }
#endif
    Locker locker { m_bitvectorLock };
    unsigned index;
    if (m_freeBlockIndices.isEmpty()) {
        index = m_blocks.size();

        size_t oldCapacity = m_blocks.capacity();
        m_blocks.append(block);
        if (m_blocks.capacity() != oldCapacity) {
            ASSERT(m_bits.numBits() == oldCapacity);
            ASSERT(m_blocks.capacity() > oldCapacity);
            
            subspace()->didResizeBits(m_blocks.capacity());
            m_bits.resize(m_blocks.capacity());
        }
    } else {
        index = m_freeBlockIndices.takeLast();
        ASSERT(!m_blocks[index]);
        m_blocks[index] = block;
    }
    
    forEachBitVector(
        [&](auto vectorRef) {
            ASSERT_UNUSED(vectorRef, !vectorRef[index]);
        });

    // This is the point at which the block learns of its cellSize() and attributes().
    block->didAddToDirectory(this, index);
    
    setIsLive(index, true);
    setIsEmpty(index, true);
    dataLogLnIf(BlockDirectoryInternal::verbose, "Setting block ", index, " in use (addBlock) for ", *this);
    setIsInUse(index, true);
}

void BlockDirectory::removeBlock(MarkedBlock::Handle* block, WillDeleteBlock willDelete)
{
    assertIsMutatorOrMutatorIsStopped();
    ASSERT(block->directory() == this);
    ASSERT(m_blocks[block->index()] == block);
    ASSERT(isInUse(block));
    
    subspace()->didRemoveBlock(block->index());
    
    m_blocks[block->index()] = nullptr;
    m_freeBlockIndices.append(block->index());
    
    releaseAssertAcquiredBitVectorLock();
    Locker locker(bitvectorLock());
    forEachBitVector(
        [&](auto vectorRef) {
            vectorRef[block->index()] = false;
        });

    if (willDelete == WillDeleteBlock::No)
        block->didRemoveFromDirectory();
}

void BlockDirectory::stopAllocating()
{
    dataLogLnIf(BlockDirectoryInternal::verbose, RawPointer(this), ": BlockDirectory::stopAllocating!");
    // SharedGC (review round 2): a NOT-YET-REGISTERED client constructs its
    // LocalAllocators into this shared directory's list from its owning
    // thread — LocalAllocator's ctor appends under m_localAllocatorsLock
    // BEFORE GCClient::Heap's ctor reaches clientSet().add(), so neither the
    // legacy stop protocol nor the §10.4 access barrier excludes that thread
    // yet. This traversal must therefore hold the lock (rank 8) or it races
    // the append (torn SentinelLinkedList walk). Lock order stays acyclic:
    // the per-allocator work below only takes BVL/block-internal locks
    // (ranks 9/9b), and the appending ctor takes nothing inside rank 8. A
    // just-appended allocator is necessarily empty (its thread has never
    // allocated through it), so stopping it is a no-op — the lock is about
    // list integrity, not allocator contents. Taken unconditionally: this is
    // a collection-time path and the lock is uncontended single-threaded.
    // Same reasoning for prepareForAllocation / resumeAllocating /
    // stopAllocatingForGood below.
    {
        Locker locker { m_localAllocatorsLock };
        if (Options::validateFreeListStructure()) [[unlikely]]
            FreeList::setStructureValidationContext("dirflush"); // Conductor step-5 flush provenance.
        m_localAllocators.forEach(
            [&] (LocalAllocator* allocator) {
                allocator->stopAllocating();
            });
        if (Options::validateFreeListStructure()) [[unlikely]]
            FreeList::setStructureValidationContext("other");
    }

#if ASSERT_ENABLED
    assertIsMutatorOrMutatorIsStopped();
    if (!inUseBitsView().isEmpty()) [[unlikely]] {
        dataLogLn("Not all inUse bits are clear at stopAllocating");
        dataLogLn(*this);
        dumpBits();
        RELEASE_ASSERT_NOT_REACHED();
    }
#endif
}

void BlockDirectory::prepareForAllocation()
{
    // SharedGC (review round 2): locked traversal — see stopAllocating().
    {
        Locker locker { m_localAllocatorsLock };
        m_localAllocators.forEach(
            [&] (LocalAllocator* allocator) {
                allocator->prepareForAllocation();
            });
    }

    m_unsweptCursor = 0;
    m_emptyCursor = 0;
    
    assertSweeperIsSuspended();
    edenBits().clearAll();

    if (Options::useImmortalObjects()) [[unlikely]] {
        // FIXME: Make this work again.
        // https://bugs.webkit.org/show_bug.cgi?id=162296
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void BlockDirectory::stopAllocatingForGood()
{
    dataLogLnIf(BlockDirectoryInternal::verbose, RawPointer(this), ": BlockDirectory::stopAllocatingForGood!");

    // SharedGC (review round 2): locked traversal — see stopAllocating().
    // One critical section covers the per-allocator stop AND the unlink.
    Locker locker { m_localAllocatorsLock };
    if (Options::validateFreeListStructure()) [[unlikely]]
        FreeList::setStructureValidationContext("dirSAFG");
    m_localAllocators.forEach(
        [&] (LocalAllocator* allocator) {
            allocator->stopAllocatingForGood();
        });
    if (Options::validateFreeListStructure()) [[unlikely]]
        FreeList::setStructureValidationContext("other");

    while (!m_localAllocators.isEmpty())
        m_localAllocators.begin()->remove();
}

void BlockDirectory::lastChanceToFinalize()
{
    forEachBlock(
        [&] (MarkedBlock::Handle* block) {
            block->lastChanceToFinalize();
        });
}

void BlockDirectory::resumeAllocating()
{
    dataLogLnIf(BlockDirectoryInternal::verbose, RawPointer(this), ": BlockDirectory::resumeAllocating!");
    // SharedGC (review round 2): locked traversal — see stopAllocating().
    Locker locker { m_localAllocatorsLock };
    m_localAllocators.forEach(
        [&] (LocalAllocator* allocator) {
            allocator->resumeAllocating();
        });
}

void BlockDirectory::beginMarkingForFullCollection()
{
    assertSweeperIsSuspended();

    // Mark bits are sticky and so is our summary of mark bits. We only clear these during full
    // collections, so if you survived the last collection you will survive the next one so long
    // as the next one is eden.
    markingNotEmptyBits().clearAll();
    markingRetiredBits().clearAll();
}

void BlockDirectory::endMarking()
{
    assertSweeperIsSuspended();

    allocatedBits().clearAll();
    
#if ASSERT_ENABLED
    if (!inUseBitsView().isEmpty()) [[unlikely]] {
        dataLogLn("Block is inUse at end marking.");
        dataLogLn(*this);
        dumpBits();
        RELEASE_ASSERT_NOT_REACHED();
    }
#endif

    // It's surprising and frustrating to comprehend, but the end-of-marking flip does not need to
    // know what kind of collection it is. That knowledge is already encoded in the m_markingXYZ
    // vectors.
    
    // Sweeper is suspended so we don't need the lock here.
    emptyBits() = liveBits() & ~markingNotEmptyBits();
    canAllocateBits() = liveBits() & ~markingRetiredBits();

    switch (m_attributes.destruction) {
    case NeedsDestruction: {
        // There are some blocks that we didn't allocate out of in the last cycle, but we swept them. This
        // will forget that we did that and we will end up sweeping them again and attempting to call their
        // destructors again. That's fine because of zapping. The only time when we cannot forget is when
        // we just allocate a block or when we move a block from one size class to another. That doesn't
        // happen here.
        destructibleBits() = liveBits();
        break;
    }

    case MayNeedDestruction: {
        // When this destruction mode is specified, each cell notifies whether this MarkedBlock needs destructor runs conservatively.
        // The bit will be set from the mutator and we use this bit to decide whether we run a destructor.
        // Until we clear the MarkedBlock completely, once this bit is set, this bit is stickily set to the MarkedBlock.
        break;
    }

    case DoesNotNeedDestruction:
        break;
    }

    if (BlockDirectoryInternal::verbose) {
        dataLogLn("Bits for ", m_cellSize, ", ", m_attributes, " after endMarking:");
        dumpBits(WTF::dataFile());
    }
}

void BlockDirectory::snapshotUnsweptForEdenCollection()
{
    assertSweeperIsSuspended();
    unsweptBits() |= edenBits();
}

void BlockDirectory::snapshotUnsweptForFullCollection()
{
    assertSweeperIsSuspended();
    unsweptBits() = liveBits();
}

MarkedBlock::Handle* BlockDirectory::findBlockToSweep(unsigned& unsweptCursor)
{
    Locker locker(bitvectorLock());
    unsweptCursor = (unsweptBits() & ~inUseBits()).findBit(unsweptCursor, true);
    if (unsweptCursor >= m_blocks.size())
        return nullptr;
    dataLogLnIf(BlockDirectoryInternal::verbose, "Setting block ", unsweptCursor, " in use (findBlockToSweep) for ", *this);
    setIsInUse(unsweptCursor, true);
    return m_blocks[unsweptCursor];
}

void BlockDirectory::sweep()
{
    // SharedGC (T8 audit): reached via MarkedSpace::sweepBlocks() (asserts
    // WSAC v MSPL v !ISS; Heap::sweepSynchronously holds MSPL) and via
    // Subspace::sweepBlocks() from IsoSubspace::sweep /
    // Heap::sweepInFinalize — conductor-side inside the stop window once
    // shared. The in-loop bitvector scans below hold the BVL (safe against
    // addBlock's resize) and the dropped-lock block->sweep calls satisfy I5b
    // through the caller's context (asserted in MarkedBlock::Handle::sweep).
    // We need to be careful of a weird race where while we are sweeping a block
    // the concurrent sweeper comes along and takes the inUse bit for a block
    // in the same bit vector word as we're currently scanning. If we did't
    // refresh our view into the word we could see stale data and try to scan
    // a block already in use.

    Locker locker(bitvectorLock());
    for (size_t index = 0; index < m_blocks.size(); ++index) {
        index = (unsweptBits() & ~inUseBits()).findBit(index, true);
        if (index >= m_blocks.size())
            break;

        MarkedBlock::Handle* block = m_blocks[index];
        ASSERT(!isInUse(index));

        // SharedGC (review round 4) — weak-bearing carve-out (rationale at
        // WeakSet::sweep / LocalAllocator::tryAllocateIn): when this full
        // sweep runs mutator-concurrently (Heap::sweepSynchronously under
        // MSPL, world running), skip blocks whose WeakSet has WeakBlocks —
        // sweeping them would race the owning client's lock-free Weak<>
        // deallocation and run weak finalizers under another client's feet.
        // The block stays unswept (lazy-sweep semantics) until the next
        // world-stopped sweep. The head() read is stable: WeakSet::allocate
        // mutates it only under MSPL, which our caller holds in this mode.
        if (heap().isSharedServer() && !heap().worldIsStoppedForAllClients() && block->weakSet().head()) [[unlikely]]
            continue;

        dataLogLnIf(BlockDirectoryInternal::verbose, "Setting block ", index, " in use (sweep) for ", *this);
        setIsInUse(index, true);
        {
            DropLockForScope scope(locker);
            block->sweep(nullptr);
        }
        ASSERT(!isUnswept(index));
        setIsInUse(index, false);
    }
}

void BlockDirectory::shrink()
{
    // SharedGC (T8 audit, MC-SAFE S4): reached via MarkedSpace::shrink() —
    // world-stopped only once shared (Heap::sweepSynchronously gates its
    // shrink leg on worldIsStoppedForAllClients(); MarkedSpace::shrink
    // asserts it). Block frees here unlink WeakSets from the active lists
    // (§5.2(2)), mutate MarkedSpace::m_blocks, and physically free the
    // block — the last is only safe with no concurrently-running sibling
    // mutators (SPEC-heap §11 world-stopped reclamation; I5/I16
    // deviation-4 precedent). The scans below hold the BVL.
    // We need to be careful of a weird race where while we are sweeping a block
    // the concurrent sweeper comes along and takes the inUse bit for a block
    // in the same bit vector word as we're currently scanning. If we did't
    // refresh our view into the word we could see stale data and try to scan
    // a block already in use.

    Locker locker(bitvectorLock());
    for (size_t index = 0; index < m_blocks.size(); ++index) {
        index = (emptyBits() & ~destructibleBits() & ~inUseBits()).findBit(index, true);
        if (index >= m_blocks.size())
            break;

        ASSERT(!isInUse(index));
        dataLogLnIf(BlockDirectoryInternal::verbose, "Setting block ", index, " in use (shrink) for ", *this);
        setIsInUse(index, true);
        {
            DropLockForScope scope(locker);
            markedSpace().freeBlock(m_blocks[index]);
        }
        setIsInUse(index, false);
    }
}

// FIXME: rdar://139998916
MarkedBlock::Handle* BlockDirectory::findMarkedBlockHandleDebug(MarkedBlock* block)
{
    for (size_t index = 0; index < m_blocks.size(); ++index) {
        MarkedBlock::Handle* handle = m_blocks[index];
        if (handle && &handle->block() == block)
            return handle;
    }
    return nullptr;
}

void BlockDirectory::assertNoUnswept()
{
    if (!ASSERT_ENABLED)
        return;

    // SharedGC (T8/I5b, FIX-3): cross-client unswept state is never
    // assertable from a non-conductor thread, and neither of the old
    // un-skip conditions identified the conductor:
    //  - mutatorSlowPathLock().isHeld() is WTF::Lock's "held by ANYONE" —
    //    another mutator's allocation slow path holding MSPL would un-skip
    //    us while we read bits we do not own;
    //  - worldIsStoppedForAllClients() means SOME conductor is mid-cycle,
    //    and that cycle's snapshotUnswept() legitimately set unswept bits.
    // Moreover, non-empty unswept is the designed shared-mode steady state:
    // BlockDirectory::sweep's weak-bearing carve-out skips blocks with
    // WeakBlocks when sweeping mutator-concurrently (world running, under
    // MSPL), so even collectNow(Sync)'s own sweep deterministically leaves
    // unswept bits; they are swept lazily in-lock (the IncrementalSweeper
    // is disabled when isSharedServer()). Skip whenever shared. A future
    // re-enable for a conductor inside its own stop window needs both
    // conductor-identity tracking on Heap and an exclusion for the
    // weak-carve-out leftovers.
    {
        auto& heap = markedSpace().heap();
        if (heap.isSharedServer())
            return;
    }

    assertIsMutatorOrMutatorIsStopped();

    if (unsweptBitsView().isEmpty())
        return;
    
    dataLog("Assertion failed: unswept not empty in ", *this, ".\n");
    dumpBits();
    ASSERT_NOT_REACHED();
}

void BlockDirectory::didFinishUsingBlock(MarkedBlock::Handle* handle)
{
    Locker locker(bitvectorLock());
    didFinishUsingBlock(locker, handle);
}

void BlockDirectory::didFinishUsingBlock(AbstractLocker&, MarkedBlock::Handle* handle)
{
    if (!isInUse(handle)) [[unlikely]] {
        dataLogLn("Finish using on a block that's not in use: ", handle->index());
        dumpBits();
        RELEASE_ASSERT_NOT_REACHED();
    }

    dataLogLnIf(BlockDirectoryInternal::verbose, "Setting block ", handle->index(), " not in use (didFinishUsingBlock) for ", *this);
    setIsInUse(handle, false);
}

RefPtr<SharedTask<MarkedBlock::Handle*()>> BlockDirectory::parallelNotEmptyBlockSource()
{
    class Task final : public SharedTask<MarkedBlock::Handle*()> {
    public:
        Task(BlockDirectory& directory)
            : m_directory(directory)
        {
        }
        
        MarkedBlock::Handle* run() final
        {
            if (m_done)
                return nullptr;
            Locker locker { m_lock };
            // SharedGC (T8 audit, I5b): parallel marking helpers run only
            // inside the stop window once shared (deviation 4), so this
            // lock-free markingNotEmpty scan sees a stable m_bits.
            m_directory.assertIsMutatorOrMutatorIsStopped();
            m_index = m_directory.m_bits.markingNotEmpty().findBit(m_index, true);
            if (m_index >= m_directory.m_blocks.size()) {
                m_done = true;
                return nullptr;
            }
            return m_directory.m_blocks[m_index++];
        }
        
    private:
        BlockDirectory& m_directory WTF_GUARDED_BY_LOCK(m_lock);
        size_t m_index WTF_GUARDED_BY_LOCK(m_lock) { 0 };
        Lock m_lock;
        bool m_done { false };
    };
    
    return adoptRef(new Task(*this));
}

void BlockDirectory::dump(PrintStream& out) const
{
    out.print(RawPointer(this), ":", m_cellSize, "/", m_attributes);
}

void BlockDirectory::dumpBits(PrintStream& out)
{
    unsigned maxNameLength = 0;
    forEachBitVectorWithName(
        [&](auto vectorRef, const char* name) {
            UNUSED_PARAM(vectorRef);
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
            unsigned length = strlen(name);
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
            maxNameLength = std::max(maxNameLength, length);
        });
    
    forEachBitVectorWithName(
        [&](auto vectorRef, const char* name) {
            out.print("    ", name, ": ");
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
            for (unsigned i = maxNameLength - strlen(name); i--;)
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
                out.print(" ");
            out.print(vectorRef, "\n");
        });
}

MarkedSpace& BlockDirectory::markedSpace() const
{
    return m_subspace->space();
}

#if ASSERT_ENABLED
void BlockDirectory::assertIsMutatorOrMutatorIsStopped() const
{
    auto& heap = markedSpace().heap();

    // SharedGC (I5b, T8 audit): every lock-free BlockDirectoryBits read/write
    // funnels through this assertion. Once the server is shared, "I am the
    // mutator" is no longer a single-thread statement — another client's slow
    // path may be reallocating m_bits in addBlock (the sole I5b writer,
    // §5.2(5)) — so a lock-free access is sound only when:
    //   (a) the world is stopped for all clients (the conductor and its
    //       parallel marking/sweeping helpers, I5), or
    //   (b) the current critical section holds the server's mutator-slow-path
    //       lock (MSPL, §5.2/§5.3/§5.6), which excludes every other mutator's
    //       slow path including addBlock's resize.
    // Accesses that hold this directory's bitvector lock do not come through
    // here; they are safe against the resize because addBlock also holds it.
    if (heap.isSharedServer()) {
        // AB18-D (V3, jit/int-gate-epoch-reclaim): also accept the §A.3
        // thread-granular stop conductor — its window parks every entered
        // mutator outside MSPL/BVL holds and holds the GCL bracket, so the
        // conductor is the sole possible directory mutator, but the window
        // sets neither WSAC nor MSPL (the heap witnesses this assert knows).
        // Conductor-thread-only AND post-quiescence only
        // (s_jsThreadsWorldStoppedDepth bumps after the §A.3.2 predicate is
        // satisfied) — the Heap.cpp:5781 / notifyVMStop conductor-exemption
        // shape: a pre-quiescence touch or a third thread escaping the park
        // must still trip.
        ASSERT(heap.worldIsStoppedForAllClients() || heap.mutatorSlowPathLock().isHeld() || (jsThreadsThreadGranularWorldIsStopped() && jsThreadsCurrentThreadIsStopConductor()));
        return;
    }

    if (!heap.worldIsStopped()) {
        if (Options::useSharedGCHeap()) [[unlikely]] {
            // SharedGC (T8/T9): option on, pre-sticky — a single registered
            // client, possibly standalone (§12.1), where vm() is asserted.
            // The single mutator thread is the one that attached the client
            // (its §10A.1 TLS slot is stamped) or, for the VM-coupled client,
            // the legacy access holder.
            ASSERT(GCClient::Heap::currentThreadClient() || heap.hasAccess());
            return;
        }
        if (auto owner = heap.vm().apiLock().ownerThread())
            ASSERT(owner->get() == &Thread::currentSingleton());
        else {
            // FIXME: It feels like heap access should be tied to holding the API lock.
            ASSERT(heap.hasAccess());
        }
    }
}

void BlockDirectory::assertSweeperIsSuspended() const
{
    // SharedGC (T8): once the server is shared the IncrementalSweeper is
    // disabled entirely (deviation 4: no mutator-concurrent sweeping; see
    // IncrementalSweeper::doWork/doWorkUntil and
    // Heap::notifyIncrementalSweeper), so the I5b rule asserted by
    // assertIsMutatorOrMutatorIsStopped() is exactly the suspension predicate.
    assertIsMutatorOrMutatorIsStopped();
}
#endif
} // namespace JSC

