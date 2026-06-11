/*
 *  Copyright (C) 2003-2024 Apple Inc. All rights reserved.
 *  Copyright (C) 2007 Eric Seidel <eric@webkit.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include "MarkedSpace.h"

#include "BlockDirectoryInlines.h"
#include "HeapInlines.h"
#include "IncrementalSweeper.h"
#include "MarkedBlockInlines.h"
#include "MarkedSpaceInlines.h"
#include "WeakSetInlines.h"
#include <wtf/ListDump.h>
#include <wtf/SimpleStats.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// UNGIL §A.3 (AB-10) cross-TU seams — defined in runtime/VMManager.cpp;
// declaration pattern matches heap/Heap.cpp:151, heap/LocalAllocator.cpp:45
// and heap/BlockDirectory.cpp:45. Signatures must stay byte-identical.
bool jsThreadsThreadGranularWorldIsStopped(); // §A.3.2 post-quiescence depth.
bool jsThreadsCurrentThreadIsStopConductor(); // §A.3.3 tenure check.

std::array<unsigned, MarkedSpace::numSizeClasses> MarkedSpace::s_sizeClassForSizeStep;

namespace {

static Vector<size_t> sizeClasses()
{
    Vector<size_t> result;

    if (Options::dumpSizeClasses()) [[unlikely]] {
        dataLog("Block size: ", MarkedBlock::blockSize, "\n");
        dataLog("Header size: ", sizeof(MarkedBlock::Header), "\n");
    }
    
    auto add = [&] (size_t sizeClass) {
        sizeClass = WTF::roundUpToMultipleOf<MarkedBlock::atomSize>(sizeClass);
        dataLogLnIf(Options::dumpSizeClasses(), "Adding JSC MarkedSpace size class: ", sizeClass);
        // Perform some validation as we go.
        RELEASE_ASSERT(!(sizeClass % MarkedSpace::sizeStep));
        if (result.isEmpty())
            RELEASE_ASSERT(sizeClass == MarkedSpace::sizeStep);
        result.append(sizeClass);
    };
    
    // This is a definition of the size classes in our GC. It must define all of the
    // size classes from sizeStep up to largeCutoff.

    // Have very precise size classes for the small stuff. This is a loop to make it easy to reduce
    // atomSize.
    for (size_t size = MarkedSpace::sizeStep; size < MarkedSpace::preciseCutoff; size += MarkedSpace::sizeStep)
        add(size);
    
    // We want to make sure that the remaining size classes minimize internal fragmentation (i.e.
    // the wasted space at the tail end of a MarkedBlock) while proceeding roughly in an exponential
    // way starting at just above the precise size classes to four cells per block.
    
    dataLogLnIf(Options::dumpSizeClasses(), "    Marked block payload size: ", static_cast<size_t>(MarkedSpace::blockPayload));
    
    for (unsigned i = 0; ; ++i) {
        double approximateSize = MarkedSpace::preciseCutoff * pow(Options::sizeClassProgression(), i);
        dataLogLnIf(Options::dumpSizeClasses(), "    Next size class as a double: ", approximateSize);

        size_t approximateSizeInBytes = static_cast<size_t>(approximateSize);
        dataLogLnIf(Options::dumpSizeClasses(), "    Next size class as bytes: ", approximateSizeInBytes);

        // Make sure that the computer did the math correctly.
        RELEASE_ASSERT(approximateSizeInBytes >= MarkedSpace::preciseCutoff);
        
        if (approximateSizeInBytes > MarkedSpace::largeCutoff)
            break;
        
        size_t sizeClass =
            WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(approximateSizeInBytes);
        dataLogLnIf(Options::dumpSizeClasses(), "    Size class: ", sizeClass);
        
        // Optimize the size class so that there isn't any slop at the end of the block's
        // payload.
        unsigned cellsPerBlock = MarkedSpace::blockPayload / sizeClass;
        size_t possiblyBetterSizeClass = (MarkedSpace::blockPayload / cellsPerBlock) & ~(MarkedSpace::sizeStep - 1);
        dataLogLnIf(Options::dumpSizeClasses(), "    Possibly better size class: ", possiblyBetterSizeClass);

        // The size class we just came up with is better than the other one if it reduces
        // total wastage assuming we only allocate cells of that size.
        size_t originalWastage = MarkedSpace::blockPayload - cellsPerBlock * sizeClass;
        size_t newWastage = (possiblyBetterSizeClass - sizeClass) * cellsPerBlock;
        dataLogLnIf(Options::dumpSizeClasses(), "    Original wastage: ", originalWastage, ", new wastage: ", newWastage);
        
        size_t betterSizeClass;
        if (newWastage > originalWastage)
            betterSizeClass = sizeClass;
        else
            betterSizeClass = possiblyBetterSizeClass;
        
        dataLogLnIf(Options::dumpSizeClasses(), "    Choosing size class: ", betterSizeClass);
        
        if (betterSizeClass == result.last()) {
            // Defense for when expStep is small.
            continue;
        }
        
        // This is usually how we get out of the loop.
        if (betterSizeClass > MarkedSpace::largeCutoff
            || betterSizeClass > Options::preciseAllocationCutoff())
            break;
        
        add(betterSizeClass);
    }

    // Manually inject size classes for objects we know will be allocated in high volume.
    // FIXME: All of these things should have IsoSubspaces.
    // https://bugs.webkit.org/show_bug.cgi?id=179876
    add(256);

    {
        // Sort and deduplicate.
        std::ranges::sort(result);
        auto it = std::unique(result.begin(), result.end());
        result.shrinkCapacity(it - result.begin());
    }

    dataLogLnIf(Options::dumpSizeClasses(), "JSC Heap MarkedSpace size class dump: ", listDump(result));

    // We have an optimization in MarkedSpace::optimalSizeFor() that assumes things about
    // the size class table. This checks our results against that function's assumptions.
    for (size_t size = MarkedSpace::sizeStep, i = 0; size <= MarkedSpace::preciseCutoff; size += MarkedSpace::sizeStep, i++)
        RELEASE_ASSERT(result.at(i) == size);

    return result;
}

template<typename TableType, typename SizeClassCons, typename DefaultCons>
void buildSizeClassTable(TableType& table, const SizeClassCons& cons, const DefaultCons& defaultCons)
{
    size_t nextIndex = 0;
    for (size_t sizeClass : sizeClasses()) {
        auto entry = cons(sizeClass);
        size_t index = MarkedSpace::sizeClassToIndex(sizeClass);
        for (size_t i = nextIndex; i <= index; ++i)
            table[i] = entry;
        nextIndex = index + 1;
    }
    ASSERT(MarkedSpace::sizeClassToIndex(MarkedSpace::largeCutoff - 1) < MarkedSpace::numSizeClasses);
    for (size_t i = nextIndex; i < MarkedSpace::numSizeClasses; ++i)
        table[i] = defaultCons(MarkedSpace::indexToSizeClass(i));
}

} // anonymous namespace

void MarkedSpace::initializeSizeClassForStepSize()
{
    static std::once_flag flag;
    std::call_once(
        flag,
        [] {
            buildSizeClassTable(
                s_sizeClassForSizeStep,
                [&] (size_t sizeClass) -> size_t {
                    RELEASE_ASSERT(sizeClass <= UINT32_MAX);
                    return sizeClass;
                },
                [&] (size_t sizeClass) -> size_t {
                    RELEASE_ASSERT(sizeClass <= UINT32_MAX);
                    return sizeClass;
                });
        });
}

MarkedSpace::MarkedSpace(JSC::Heap* heap)
{
    ASSERT_UNUSED(heap, heap == &this->heap());
    initializeSizeClassForStepSize();
}

MarkedSpace::~MarkedSpace()
{
    ASSERT(!m_blocks.set().size());
}

void MarkedSpace::freeMemory()
{
    // SharedGC (T8): server teardown — Heap::lastChanceToFinalize holds MSPL
    // across this (a stale sticky-ISS flag may outlive the last secondary
    // client until the §10D revert poll).
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients() || heap().mutatorSlowPathLock().isHeld());
    forEachBlock(
        [&] (MarkedBlock::Handle* block) {
            freeBlock(block);
        });
    for (PreciseAllocation* allocation : m_preciseAllocations)
        allocation->destroy();
    forEachSubspace([&](Subspace& subspace) {
        if (subspace.isIsoSubspace())
            static_cast<IsoSubspace&>(subspace).destroyLowerTierPreciseFreeList();
        return IterationStatus::Continue;
    });
}

void MarkedSpace::lastChanceToFinalize()
{
    // SharedGC (T8): server teardown — under MSPL (Heap::lastChanceToFinalize)
    // or stopped; the per-block bit writes inside are I5b writers.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients() || heap().mutatorSlowPathLock().isHeld());
    forEachDirectory(
        [&] (BlockDirectory& directory) -> IterationStatus {
            directory.lastChanceToFinalize();
            return IterationStatus::Continue;
        });
    for (PreciseAllocation* allocation : m_preciseAllocations)
        allocation->lastChanceToFinalize();
    // We do not call lastChanceToFinalize for lower-tier swept cells since we need nothing to do.
}

void MarkedSpace::sweepBlocks()
{
    // SharedGC (T8/I5b): synchronous full sweeps touch every directory's bits
    // and free blocks; once shared they must hold MSPL (Heap::
    // sweepSynchronously takes it) or run while the world is stopped for all
    // clients.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients() || heap().mutatorSlowPathLock().isHeld());
    heap().sweeper().stopSweeping();
    forEachDirectory(
        [&] (BlockDirectory& directory) -> IterationStatus {
            directory.sweep();
            return IterationStatus::Continue;
        });
}

void MarkedSpace::registerPreciseAllocation(PreciseAllocation* allocation, bool isNewAllocation)
{
    // SharedGC (§5.6/I16): once the server is shared, the precise registry
    // (m_preciseAllocations, m_preciseAllocationSet, indexInSpace stamps) is
    // mutated only under MSPL or while the world is stopped for all clients.
    // Mutator-path callers covered (T3b audit): CompleteSubspace::
    // tryAllocateSlow and PreciseSubspace::tryAllocate take MSPL themselves;
    // IsoSubspace::tryAllocateLowerTierPrecise runs inside LocalAllocator::
    // allocateSlowCase's MSPL section (§5.2).
    ASSERT(!heap().isSharedServer() || heap().mutatorSlowPathLock().isHeld() || heap().worldIsStoppedForAllClients());

    // FIXME: This is a bit of a mess we should really consolidate setting all the bits to here.
    allocation->setIndexInSpace(m_preciseAllocations.size());
    allocation->m_hasValidCell = true;
    ASSERT(allocation->isNewlyAllocated());
    ASSERT(!allocation->isMarked());
    m_preciseAllocations.append(allocation);
    if (auto& set = preciseAllocationSet())
        set->add(allocation->cell());
    if (isNewAllocation) {
        // Existing code's ordering is calling `didAllocate` and increasing capacity.
        size_t size = allocation->cellSize();
        heap().didAllocate(size);
        m_capacity.fetch_add(size, std::memory_order_relaxed); // §5.4/F3.
    }
}

void MarkedSpace::sweepPreciseAllocations()
{
    // SharedGC (I5/I16): in shared mode, precise-allocation sweeping runs only
    // on the conductor while the world is stopped (mutator-concurrent
    // sweeping is a deviation-4 disabled feature).
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients());
    RELEASE_ASSERT(m_preciseAllocationsNurseryOffset == m_preciseAllocations.size());
    unsigned srcIndex = m_preciseAllocationsNurseryOffsetForSweep;
    unsigned dstIndex = srcIndex;
    while (srcIndex < m_preciseAllocations.size()) {
        PreciseAllocation* allocation = m_preciseAllocations[srcIndex++];
        allocation->sweep();
        if (allocation->isEmpty()) {
            if (auto& set = preciseAllocationSet())
                set->remove(allocation->cell());
            if (allocation->isLowerTierPrecise())
                static_cast<IsoSubspace*>(allocation->subspace())->sweepLowerTierPreciseCell(allocation);
            else {
                m_capacity.fetch_sub(allocation->cellSize(), std::memory_order_relaxed); // §5.4/F3.
                allocation->destroy();
            }
            continue;
        }
        allocation->setIndexInSpace(dstIndex);
        m_preciseAllocations[dstIndex++] = allocation;
    }
    m_preciseAllocations.shrinkCapacity(dstIndex);
    m_preciseAllocationsNurseryOffset = m_preciseAllocations.size();
}

void MarkedSpace::prepareForAllocation()
{
    ASSERT(!Thread::mayBeGCThread() || heap().worldIsStopped());
    // SharedGC (I5, T8): prepare iteration (per-allocator resets, cursor
    // resets, eden-bit clears) and the m_newActiveWeakSets splice below run
    // only on the conductor while the world is stopped for all clients.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients());
    for (Subspace* subspace : m_subspaces)
        subspace->prepareForAllocation();

    m_activeWeakSets.takeFrom(m_newActiveWeakSets);
    
    if (heap().collectionScope() == CollectionScope::Eden)
        m_preciseAllocationsNurseryOffsetForSweep = m_preciseAllocationsNurseryOffset;
    else
        m_preciseAllocationsNurseryOffsetForSweep = 0;
    m_preciseAllocationsNurseryOffset = m_preciseAllocations.size();
}

void MarkedSpace::enablePreciseAllocationTracking()
{
    // SharedGC (§5.6/I16, T3b audit): called from the mutator (e.g. the
    // sampling profiler); when shared it both reads the precise vector and
    // installs the set that registerPreciseAllocation subsequently adds to —
    // take MSPL so it cannot interleave with another client's registration.
    MutatorSlowPathLocker mutatorSlowPathLocker(heap());
    m_preciseAllocationSet = UncheckedKeyHashSet<HeapCell*> { };
    for (auto* allocation : m_preciseAllocations)
        m_preciseAllocationSet->add(allocation->cell());
}

void MarkedSpace::reapWeakSets()
{
    // SharedGC (I5/§5.2(2), T8): active-weak-set iteration is conductor-side
    // while the world is stopped for all clients.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients());
    auto visit = [&] (WeakSet* weakSet) {
        weakSet->reap();
    };
    
    m_newActiveWeakSets.forEach(visit);
    
    if (heap().collectionScope() == CollectionScope::Full)
        m_activeWeakSets.forEach(visit);
}

void MarkedSpace::stopAllocating()
{
    ASSERT(!isIterating());
    // SharedGC (§5.2(4)/I5, T8): the directory iteration below flushes EVERY
    // client's LocalAllocators (they are all linked into the shared
    // directories' m_localAllocators lists) — this is the §10 step-5 flush
    // when called from stopThePeriphery() on the conductor. The MSPL
    // disjunct below is the teardown carve-out (§5.3): MSPL alone does NOT
    // license flushing while other client threads run — their inline/LLInt
    // fast paths pop their FreeLists without any lock (I2). Outside teardown,
    // shared-mode callers must be the conductor while WSAC.
    // UNGIL §K.5 class-4 (AB-10): a §A.3 thread-granular window's CONDUCTOR
    // is also licensed — its §A.3.2 predicate parks every other entered
    // mutator at poll sites outside MSPL/BVL holds (the AB18-D argument at
    // LocalAllocator.cpp:assertSharedAllocatorMutationIsSafe), so no client
    // FreeList fast path can be in flight; haveABadTime's conversion-walk
    // HeapIterationScope reaches here from inside that window.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients() || heap().mutatorSlowPathLock().isHeld() || (jsThreadsThreadGranularWorldIsStopped() && jsThreadsCurrentThreadIsStopConductor()));
    forEachDirectory(
        [&] (BlockDirectory& directory) -> IterationStatus {
            directory.stopAllocating();
            return IterationStatus::Continue;
        });
}

void MarkedSpace::stopAllocatingForGood()
{
    ASSERT(!isIterating());
    // SharedGC (§5.2(4)/§5.3 teardown, T8): server teardown holds MSPL
    // (Heap::lastChanceToFinalize); per-client teardown goes through
    // GCThreadLocalCache::stopAllocatingForGood (also under MSPL).
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients() || heap().mutatorSlowPathLock().isHeld());
    forEachDirectory(
        [&] (BlockDirectory& directory) -> IterationStatus {
            directory.stopAllocatingForGood();
            return IterationStatus::Continue;
        });
}

void MarkedSpace::prepareForConservativeScan()
{
    // SharedGC (I5/I16, T8): sorts and re-stamps the precise vector tail;
    // conductor-only while stopped for all clients once shared.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients());
    if (m_conservativeScanIsPrepared)
        return;
    m_conservativeScanIsPrepared = true;

    m_preciseAllocationsForThisCollectionBegin = m_preciseAllocations.begin() + m_preciseAllocationsOffsetForThisCollection;
    m_preciseAllocationsForThisCollectionSize = m_preciseAllocations.size() - m_preciseAllocationsOffsetForThisCollection;
    m_preciseAllocationsForThisCollectionEnd = m_preciseAllocations.end();
    RELEASE_ASSERT(m_preciseAllocationsForThisCollectionEnd == m_preciseAllocationsForThisCollectionBegin + m_preciseAllocationsForThisCollectionSize);
    
    std::sort(
        m_preciseAllocationsForThisCollectionBegin, m_preciseAllocationsForThisCollectionEnd,
        [&] (PreciseAllocation* a, PreciseAllocation* b) {
            return a < b;
        });
    unsigned index = m_preciseAllocationsOffsetForThisCollection;
    for (auto* start = m_preciseAllocationsForThisCollectionBegin; start != m_preciseAllocationsForThisCollectionEnd; ++start, ++index) {
        (*start)->setIndexInSpace(index);
        ASSERT(m_preciseAllocations[index] == *start);
        ASSERT(m_preciseAllocations[index]->indexInSpace() == index);
    }
}

void MarkedSpace::prepareForMarking()
{
    // SharedGC (I5, T8): marking-start work is conductor-only once shared.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients());
    if (heap().collectionScope() == CollectionScope::Eden)
        m_preciseAllocationsOffsetForThisCollection = m_preciseAllocationsNurseryOffset;
    else
        m_preciseAllocationsOffsetForThisCollection = 0;
}

void MarkedSpace::resumeAllocating()
{
    // SharedGC (§5.2(4), T8): resume is conductor-side (still inside the stop
    // window — §10 step 8 strictly precedes the VMM resume) or a
    // MSPL-holding iterator's didFinishIterating.
    // UNGIL §K.5 class-4 (AB-10): the §A.3 conductor disjunct mirrors
    // stopAllocating() above — didFinishIterating from haveABadTime's
    // in-window HeapIterationScope resumes here before the window closes.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients() || heap().mutatorSlowPathLock().isHeld() || (jsThreadsThreadGranularWorldIsStopped() && jsThreadsCurrentThreadIsStopConductor()));
    m_conservativeScanIsPrepared = false;
    forEachDirectory(
        [&] (BlockDirectory& directory) -> IterationStatus {
            directory.resumeAllocating();
            return IterationStatus::Continue;
        });
    // Nothing to do for PreciseAllocations.
}

bool MarkedSpace::isPagedOut()
{
    // SharedGC (T8 audit): iterates m_blocks lock-free. Reached from the
    // full-GC activity callback (never fired once shared, §5.4) and from
    // collection bookkeeping on the conductor.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients());
    SimpleStats pagedOutPagesStats;

    forEachDirectory(
        [&] (BlockDirectory& directory) -> IterationStatus {
            directory.updatePercentageOfPagedOutPages(pagedOutPagesStats);
            return IterationStatus::Continue;
        });
    // FIXME: Consider taking PreciseAllocations into account here.
    double maxHeapGrowthFactor = VM::isInMiniMode() ? Options::miniVMHeapGrowthFactor() : Options::largeHeapGrowthFactor();
    double bailoutPercentage = Options::customFullGCCallbackBailThreshold() == -1.0 ? maxHeapGrowthFactor - 1 : Options::customFullGCCallbackBailThreshold();
    return pagedOutPagesStats.mean() > pagedOutPagesStats.count() * bailoutPercentage;
}

// FIXME: rdar://139998916
MarkedBlock::Handle* MarkedSpace::findMarkedBlockHandleDebug(MarkedBlock* block)
{
    MarkedBlock::Handle* result = nullptr;
    forEachDirectory(
        [&](BlockDirectory& directory) -> IterationStatus {
            if (MarkedBlock::Handle* handle = directory.findMarkedBlockHandleDebug(block)) {
                result = handle;
                return IterationStatus::Done;
            }
            return IterationStatus::Continue;
        });
    return result;
}

void MarkedSpace::freeBlock(MarkedBlock::Handle* block)
{
    m_capacity.fetch_sub(MarkedBlock::blockSize, std::memory_order_relaxed); // §5.4/F3.
    m_blocks.remove(&block->block());
    delete block;
}

void MarkedSpace::shrink()
{
    // SharedGC (T8, MC-SAFE S4): frees empty blocks (registry + weak-set
    // unlinks + physical fastFree of the block). WORLD-STOPPED ONLY once
    // shared: MSPL is not a sufficient shield because sibling mutators
    // perform lock-free reads that may transiently touch dead-verdicted
    // cells (fiberConcurrently et al.); physical reclamation rides the
    // world-stopped reclaim point (SPEC-heap §11; same world-stopped-only
    // rule as sweepPreciseAllocations' I5/I16 deviation-4 precedent).
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients());
    forEachDirectory(
        [&] (BlockDirectory& directory) -> IterationStatus {
            directory.shrink();
            return IterationStatus::Continue;
        });
}

void MarkedSpace::beginMarking()
{
    // SharedGC (I5, T8): marking-start (version bumps, mark-bit summary
    // clears, precise flips) is conductor-only while stopped for all clients.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients());
    switch (heap().collectionScope().value()) {
    case CollectionScope::Eden: {
        m_edenVersion = nextVersion(m_edenVersion);
        break;
    }
    case CollectionScope::Full: {
        forEachDirectory(
            [&] (BlockDirectory& directory) -> IterationStatus {
                directory.beginMarkingForFullCollection();
                return IterationStatus::Continue;
            });

        if (nextVersion(m_markingVersion) == initialVersion) [[unlikely]] {
            forEachBlock(
                [&] (MarkedBlock::Handle* handle) {
                    handle->block().resetMarks();
                });
        }

        m_markingVersion = nextVersion(m_markingVersion);

        for (PreciseAllocation* allocation : m_preciseAllocations)
            allocation->flip();

        break;
    }
    }

    if (ASSERT_ENABLED) {
        forEachBlock(
            [&] (MarkedBlock::Handle* block) {
                if (block->areMarksStale())
                    return;
                ASSERT(!block->isFreeListed());
            });
    }
    
    m_isMarking = true;
}

void MarkedSpace::endMarking()
{
    // SharedGC (I5, T8): end-of-marking bit flips (BlockDirectory::endMarking
    // recomputes empty/canAllocate lock-free) are conductor-only while
    // stopped for all clients.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients());
    if (nextVersion(m_newlyAllocatedVersion) == initialVersion) [[unlikely]] {
        forEachBlock(
            [&] (MarkedBlock::Handle* handle) {
                handle->block().resetAllocated();
            });
    }
    
    m_newlyAllocatedVersion = nextVersion(m_newlyAllocatedVersion);
    
    for (unsigned i = m_preciseAllocationsOffsetForThisCollection; i < m_preciseAllocations.size(); ++i)
        m_preciseAllocations[i]->clearNewlyAllocated();

    if (ASSERT_ENABLED) {
        for (PreciseAllocation* allocation : m_preciseAllocations)
            ASSERT_UNUSED(allocation, !allocation->isNewlyAllocated());
    }

    forEachDirectory(
        [&] (BlockDirectory& directory) -> IterationStatus {
            directory.endMarking();
            return IterationStatus::Continue;
        });
    
    m_isMarking = false;
}

void MarkedSpace::willStartIterating()
{
    ASSERT(!isIterating());
    // SharedGC (T8): once shared, a HeapIterationScope is legal only while
    // the world is stopped for all clients (conductor / JSThreadsStopScope
    // holders that enqueue a GC, §10C) or during teardown when no other
    // client thread can run (under MSPL for the I5b bit reads). It is NOT
    // legal from a mutator while other clients run: the stopAllocating()
    // below would flush their live FreeLists out from under their lock-free
    // fast paths (I2). Its assert is the audit gate.
    stopAllocating();
    m_isIterating = true;
}

void MarkedSpace::didFinishIterating()
{
    ASSERT(isIterating());
    resumeAllocating();
    m_isIterating = false;
}

size_t MarkedSpace::objectCount()
{
    size_t result = 0;
    forEachBlock(
        [&] (MarkedBlock::Handle* block) {
            result += block->markCount();
        });
    for (PreciseAllocation* allocation : m_preciseAllocations) {
        if (allocation->isMarked())
            result++;
    }
    return result;
}

size_t MarkedSpace::size()
{
    size_t result = 0;
    forEachBlock(
        [&] (MarkedBlock::Handle* block) {
            result += block->markCount() * block->cellSize();
        });
    for (PreciseAllocation* allocation : m_preciseAllocations) {
        if (allocation->isMarked())
            result += allocation->cellSize();
    }
    return result;
}

size_t MarkedSpace::capacity()
{
    return m_capacity.load(std::memory_order_relaxed); // §5.4/F3: exact at safepoints (I7).
}

void MarkedSpace::addActiveWeakSet(WeakSet* weakSet)
{
    // SharedGC (§5.2(2), T8 audit; review round 4): sole caller is
    // WeakSet::addAllocator, reached only from WeakSet::allocate — which now
    // holds MSPL itself when shared (the weak-mutation protocol,
    // WeakSetInlines.h), covering this sentinel-list append along with the
    // rest of the per-WeakSet state. Taking MSPL HERE would self-deadlock
    // (WTF::Lock is not recursive), so this became assert-only. All other
    // accesses to this list are conductor-side while the world is stopped
    // for all clients (prepareForAllocation's takeFrom, reapWeakSets,
    // visiting) or block-free paths already under MSPL/WSAC
    // (WeakSet::remove via shrink/free).
    ASSERT(!heap().isSharedServer() || heap().mutatorSlowPathLock().isHeld() || heap().worldIsStoppedForAllClients());

    // We conservatively assume that the WeakSet should belong in the new set. In fact, some weak
    // sets might contain new weak handles even though they are tied to old objects. This slightly
    // increases the amount of scanning that an eden collection would have to do, but the effect
    // ought to be small.
    m_newActiveWeakSets.append(weakSet);
}

void MarkedSpace::didAddBlock(MarkedBlock::Handle* block)
{
    // WARNING: This function is called before block is fully initialized. The block will not know
    // its cellSize() or attributes(). The latter implies that you can't ask things like
    // needsDestruction().
    // SharedGC (§5.2): mutator-path calls (BlockDirectory::tryAllocateBlock)
    // hold MSPL when shared; m_blocks (a MarkedBlockSet with its own internal
    // bloom-filter/set) additions are thereby serialized against each other.
    m_capacity.fetch_add(MarkedBlock::blockSize, std::memory_order_relaxed); // §5.4/F3.
    m_blocks.add(&block->block());
}

void MarkedSpace::didAllocateInBlock(MarkedBlock::Handle* block)
{
    // SharedGC (§5.2(2)): mutator calls arrive inside LocalAllocator::
    // allocateSlowCase's MSPL section; all other m_newActiveWeakSets accesses
    // are conductor-side while the world is stopped for all clients (I5).
    ASSERT(!heap().isSharedServer() || heap().mutatorSlowPathLock().isHeld() || heap().worldIsStoppedForAllClients());
    if (block->weakSet().isOnList()) {
        block->weakSet().remove();
        m_newActiveWeakSets.append(&block->weakSet());
    }
}

void MarkedSpace::snapshotUnswept()
{
    // SharedGC (I5, T8): sweep scheduling (unswept snapshots) is
    // conductor-only while stopped for all clients.
    ASSERT(!heap().isSharedServer() || heap().worldIsStoppedForAllClients());
    if (heap().collectionScope() == CollectionScope::Eden) {
        forEachDirectory(
            [&] (BlockDirectory& directory) -> IterationStatus {
                directory.snapshotUnsweptForEdenCollection();
                return IterationStatus::Continue;
            });
    } else {
        forEachDirectory(
            [&] (BlockDirectory& directory) -> IterationStatus {
                directory.snapshotUnsweptForFullCollection();
                return IterationStatus::Continue;
            });
    }
}

void MarkedSpace::assertNoUnswept()
{
    if (!ASSERT_ENABLED)
        return;
    forEachDirectory(
        [&] (BlockDirectory& directory) -> IterationStatus {
            directory.assertNoUnswept();
            return IterationStatus::Continue;
        });
}

void MarkedSpace::dumpBits(PrintStream& out)
{
    forEachDirectory(
        [&] (BlockDirectory& directory) -> IterationStatus {
            directory.assertIsMutatorOrMutatorIsStopped();
            out.print("Bits for ", directory, ":\n");
            directory.dumpBits(out);
            return IterationStatus::Continue;
        });
}

unsigned MarkedSpace::reserveThreadLocalCacheIndices(const AbstractLocker&)
{
    // SharedGC (§5.3; T4): monotonic, never reused; caller holds
    // m_directoryLock (the locker token).
    unsigned base = m_nextTlcIndexBase;
    unsigned next = base + static_cast<unsigned>(numSizeClasses);
    RELEASE_ASSERT(next > base); // Overflow would alias TLC slots across subspaces.
    m_nextTlcIndexBase = next;
    return base;
}

void MarkedSpace::addBlockDirectory(const AbstractLocker&, BlockDirectory* directory)
{
    directory->setNextDirectory(nullptr);
    
    WTF::storeStoreFence();

    m_directories.append(std::mem_fn(&BlockDirectory::setNextDirectory), directory);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
