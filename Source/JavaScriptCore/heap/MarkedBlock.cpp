/*
 * Copyright (C) 2011-2025 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "MarkedBlock.h"

#include "AlignedMemoryAllocator.h"
#include "FreeListInlines.h"
#include "JSCJSValueInlines.h"
#include "MarkedBlockInlines.h"
#include "SweepingScope.h"
#include "VMManager.h"
#include "WeakSetInlines.h"
#include <wtf/CommaPrinter.h>

#if PLATFORM(COCOA)
#include <wtf/cocoa/CrashReporter.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// NEVER_INLINE to prevent LTO from inlining this function, which can break
// compiler barriers (loadLoadFence/compilerFence) on x86_64.
NEVER_INLINE bool MarkedBlock::isMarked(HeapVersion markingVersion, const void* p)
{
    HeapVersion version;
    Dependency dependency = Dependency::loadAndFence(&header().m_markingVersion, version);
    if (version != markingVersion) [[unlikely]]
        return false;
    return header().m_marks.concurrentGet(atomNumber(p), dependency);
}

// NEVER_INLINE to prevent LTO from inlining this function, which can break
// compiler barriers (Dependency::fence/loadLoadFence/compilerFence) on x86_64.
NEVER_INLINE bool MarkedBlock::Handle::isLive(HeapVersion markingVersion, HeapVersion newlyAllocatedVersion, bool isMarking, const HeapCell* cell)
{
    m_directory->assertIsMutatorOrMutatorIsStopped();
    if (m_directory->isAllocated(this))
        return true;

    MarkedBlock& block = this->block();
    MarkedBlock::Header& header = block.header();

    auto count = header.m_lock.tryOptimisticFencelessRead();
    if (count.value) {
        Dependency fenceBefore = Dependency::fence(count.input);
        MarkedBlock& fencedBlock = *fenceBefore.consume(&block);
        MarkedBlock::Header& fencedHeader = fencedBlock.header();
        MarkedBlock::Handle* fencedThis = fenceBefore.consume(this);

        ASSERT_UNUSED(fencedThis, !fencedThis->isFreeListed());

        HeapVersion myNewlyAllocatedVersion = fencedHeader.m_newlyAllocatedVersion;
        if (myNewlyAllocatedVersion == newlyAllocatedVersion) {
            bool result = fencedBlock.isNewlyAllocated(cell);
            if (header.m_lock.fencelessValidate(count.value, Dependency::fence(result)))
                return result;
        } else {
            HeapVersion myMarkingVersion = fencedHeader.m_markingVersion;
            if (myMarkingVersion != markingVersion
                && (!isMarking || !fencedBlock.marksConveyLivenessDuringMarking(myMarkingVersion, markingVersion))) {
                if (header.m_lock.fencelessValidate(count.value, Dependency::fence(myMarkingVersion)))
                    return false;
            } else {
                // TSAN r12 (report 7): concurrentGet — a marker's
                // concurrentTestAndSet CAS races this optimistic read; the
                // CountingLock fencelessValidate re-check (upstream protocol)
                // already tolerates the race, the relaxed atomic read just
                // makes the word access defined. Codegen identical.
                bool result = fencedHeader.m_marks.concurrentGet(block.atomNumber(cell), fenceBefore);
                if (header.m_lock.fencelessValidate(count.value, Dependency::fence(result)))
                    return result;
            }
        }
    }

    Locker locker { header.m_lock };

    ASSERT(!isFreeListed());

    HeapVersion myNewlyAllocatedVersion = header.m_newlyAllocatedVersion;
    if (myNewlyAllocatedVersion == newlyAllocatedVersion)
        return block.isNewlyAllocated(cell);

    if (block.areMarksStale(markingVersion)) {
        if (!isMarking)
            return false;
        if (!block.marksConveyLivenessDuringMarking(markingVersion))
            return false;
    }

    return header.m_marks.get(block.atomNumber(cell));
}

// NEVER_INLINE to prevent LTO from inlining this function, which can break
// compiler barriers on x86_64.
NEVER_INLINE bool MarkedBlock::Handle::isLive(const HeapCell* cell)
{
    return isLive(space()->markingVersion(), space()->newlyAllocatedVersion(), space()->isMarking(), cell);
}

namespace MarkedBlockInternal {
static constexpr bool verbose = false;
}

static constexpr bool computeBalance = false;
static size_t balance;

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(MarkedBlock);
DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(MarkedBlockHandle);

MarkedBlock::Handle* MarkedBlock::tryCreate(JSC::Heap& heap, AlignedMemoryAllocator* alignedMemoryAllocator)
{
    if (computeBalance) {
        balance++;
        if (!(balance % 10))
            dataLog("MarkedBlock Balance: ", balance, "\n");
    }
    void* blockSpace = alignedMemoryAllocator->tryAllocateAlignedMemory(blockSize, blockSize);
    if (!blockSpace)
        return nullptr;
    if (scribbleFreeCells())
        scribble(blockSpace, blockSize);
    return new Handle(heap, alignedMemoryAllocator, blockSpace);
}

// SharedGC (T9): any-client OK — blocks are server-owned; heap.vm() stamps
// the main VM (deviation 3) into the WeakSet/header at construction (see
// MarkedBlock::vm(), MarkedBlock.h). Construction runs under MSPL once
// shared (tryAllocateBlock, §5.2(3)); the stamp itself is thread-neutral.
MarkedBlock::Handle::Handle(JSC::Heap& heap, AlignedMemoryAllocator* alignedMemoryAllocator, void* blockSpace)
    : m_alignedMemoryAllocator(alignedMemoryAllocator)
    , m_weakSet(heap.vm())
    , m_block(new (NotNull, blockSpace) MarkedBlock(heap.vm(), *this))
{
    heap.didAllocateBlock(blockSize);
}

MarkedBlock::Handle::~Handle()
{
    JSC::Heap& heap = *this->heap();
    if (computeBalance) {
        balance--;
        if (!(balance % 10))
            dataLog("MarkedBlock Balance: ", balance, "\n");
    }
    m_directory->removeBlock(this, BlockDirectory::WillDeleteBlock::Yes);
    m_block->~MarkedBlock();
    m_alignedMemoryAllocator->freeAlignedMemory(m_block);
    heap.didFreeBlock(blockSize);
}

MarkedBlock::MarkedBlock(VM& vm, Handle& handle)
{
    new (&header()) Header(vm, handle);
    if (MarkedBlockInternal::verbose)
        dataLog(RawPointer(this), ": Allocated.\n");
}

MarkedBlock::~MarkedBlock()
{
    header().~Header();
}

MarkedBlock::Header::Header(VM& vm, Handle& handle)
    : m_handle(handle)
    , m_vm(&vm)
    , m_markingVersion(MarkedSpace::nullVersion)
    , m_newlyAllocatedVersion(MarkedSpace::nullVersion)
{
    // TSAN r12 (report 3, residual-2 "HeapCell::vm on recycled MarkedBlock"):
    // publication choke point — pairs with the HAPPENS_AFTER in
    // MarkedBlock::vmConcurrentProbe() (NOT plain vm(); narrowed at the
    // thread-closeout final review — see the probe accessor's comment for why
    // the universal getter must stay unannotated).
    // The header's const-init writes (m_vm is `VM* const`,
    // it cannot become an atomic) are ordered before any cross-thread cell
    // probe by the block hand-out protocol (directory locks / consume-style
    // cell publication), which TSAN cannot fully model; stale probes of a
    // RECYCLED block are blessed by the wave-7 staleness adjudication. No-op
    // outside TSAN.
    TSAN_ANNOTATE_HAPPENS_BEFORE(this);
}

MarkedBlock::Header::~Header() = default;

void MarkedBlock::Handle::unsweepWithNoNewlyAllocated()
{
    RELEASE_ASSERT(m_isFreeListed);
    m_isFreeListed = false;
    m_directory->didFinishUsingBlock(this);
}

void MarkedBlock::Handle::stopAllocating(const FreeList& freeList)
{
    Locker locker { blockHeader().m_lock };
    
    if (MarkedBlockInternal::verbose)
        dataLog(RawPointer(this), ": MarkedBlock::Handle::stopAllocating!\n");
    m_directory->assertIsMutatorOrMutatorIsStopped();
    ASSERT(!m_directory->isAllocated(this));

    if (!isFreeListed()) {
        if (MarkedBlockInternal::verbose)
            dataLog("There ain't no newly allocated.\n");
        // This means that we either didn't use this block at all for allocation since last GC,
        // or someone had already done stopAllocating() before.
        ASSERT(freeList.allocationWillFail());
        return;
    }
    
    if (MarkedBlockInternal::verbose)
        dataLog("Free list: ", freeList, "\n");
    
    // Roll back to a coherent state for Heap introspection. Cells newly
    // allocated from our free list are not currently marked, so we need another
    // way to tell what's live vs dead. 
    
    blockHeader().m_newlyAllocated.clearAll();
    blockHeader().m_newlyAllocatedVersion = heap()->objectSpace().newlyAllocatedVersion();

    forEachCell(
        [&] (size_t, HeapCell* cell, HeapCell::Kind) -> IterationStatus {
            block().setNewlyAllocated(cell);
            return IterationStatus::Continue;
        });

    freeList.forEach(
        [&] (HeapCell* cell) {
            if constexpr (MarkedBlockInternal::verbose)
                dataLog("Free cell: ", RawPointer(cell), "\n");
            if (m_attributes.destruction != DoesNotNeedDestruction)
                cell->zap(HeapCell::StopAllocating);
            block().clearNewlyAllocated(cell);
        });
    
    m_isFreeListed = false;
    directory()->didFinishUsingBlock(this);
}

void MarkedBlock::Handle::lastChanceToFinalize()
{
    // Concurrent sweeper is shut down at this point.
    m_directory->assertSweeperIsSuspended();
    m_directory->setIsAllocated(this, false);
    m_directory->setIsDestructible(this, true);
    m_directory->setIsUnswept(this, true);
    blockHeader().m_marks.clearAll();
    block().clearHasAnyMarked();
    blockHeader().m_markingVersion = heap()->objectSpace().markingVersion();
    m_weakSet.lastChanceToFinalize();
    blockHeader().m_newlyAllocated.clearAll();
    blockHeader().m_newlyAllocatedVersion = heap()->objectSpace().newlyAllocatedVersion();
    m_directory->setIsInUse(this, true);
    sweep(nullptr);
}

void MarkedBlock::Handle::resumeAllocating(FreeList& freeList)
{
    BlockDirectory* directory = this->directory();
    directory->assertSweeperIsSuspended();
    {
        Locker locker { blockHeader().m_lock };
        
        if (MarkedBlockInternal::verbose)
            dataLog(RawPointer(this), ": MarkedBlock::Handle::resumeAllocating!\n");


        ASSERT(!directory->isAllocated(this));
        ASSERT(!isFreeListed());
        
        if (!block().hasAnyNewlyAllocated()) {
            if (MarkedBlockInternal::verbose)
                dataLog("There ain't no newly allocated.\n");
            // This means we had already exhausted the block when we stopped allocation.
            freeList.clear();
            return;
        }
    }

    directory->setIsInUse(this, true);

    // Re-create our free list from before stopping allocation. Note that this may return an empty
    // freelist, in which case the block will still be Marked!
    sweep(&freeList);
}

#if ENABLE(MARKEDBLOCK_TEST_DUMP_INFO)

inline void MarkedBlock::setupTestForDumpInfoAndCrash()
{
    static std::atomic<uint64_t> count = 0;
    char* blockMem = reinterpret_cast<char*>(this);

    // Option set to 0 disables testing.
    if (++count == Options::markedBlockDumpInfoCount()) {
        switch (Options::markedBlockDumpInfoCount() & 0xf) {
        case 1: // Test null VM pointer.
            dataLogLn("Zeroing MarkedBlock::Header::m_vm");
            *const_cast<VM**>(&header().m_vm) = nullptr;
            break;
        case 2: // Test non-null invalid VM pointer.
            dataLogLn("Corrupting MarkedBlock::Header::m_vm");
            *const_cast<VM**>(&header().m_vm) = std::bit_cast<VM*>(0xdeadbeefdeadbeef);
            break;
        case 3: // Test contiguous and total zero byte counts: start and end zeroed.
            dataLogLn("Zeroing start and end of MarkedBlock");
            memset(blockMem, 0, blockSize / 4);
            memset(blockMem + 3 * blockSize / 4, 0, blockSize / 4);
            break;
        case 4: // Test contiguous and total zero byte counts: entire block zeroed.
            dataLogLn("Zeroing MarkedBlock");
            memset(blockMem, 0, blockSize);
            break;
        case 5: // Test already freed block (test this case with --useConcurrentGC=0)
            dataLogLn("Simulating freed MarkedBlock");
            space()->blocks().remove(this);
            handle().removeFromDirectory();
            break;
        }
        *reinterpret_cast<uintptr_t*>(&header()) = 0;
    }
}

#else

inline void MarkedBlock::setupTestForDumpInfoAndCrash() { }

#endif // ENABLE(MARKEDBLOCK_TEST_DUMP_INFO)

void MarkedBlock::aboutToMarkSlow(HeapVersion markingVersion, HeapCell* cell)
{
    // SharedGC (T8 audit, I5b): marker-helper path. Once shared, marking runs
    // only inside the stop window (deviation 4; I11 note), and every
    // directory-bit access below (isAllocated read, setIsMarkingNotEmpty)
    // holds the bitvector lock — safe against addBlock's m_bits resize even
    // though helpers don't hold MSPL.
    ASSERT(vm().heap.objectSpace().isMarking());
    setupTestForDumpInfoAndCrash();

    Locker locker { header().m_lock };
    
    if (!areMarksStale(markingVersion))
        return;

    MarkedBlock::Handle* handle = header().handlePointerForNullCheck();
    if (!handle) [[unlikely]]
        analyzeInvalidHandleAndCrash(locker, cell);

    BlockDirectory* directory = handle->directory();
    bool isAllocated;
    {
        Locker bitLocker { directory->bitvectorLock() };
        isAllocated = directory->isAllocated(handle);
    }

    if (isAllocated || !marksConveyLivenessDuringMarking(markingVersion)) {
        if (MarkedBlockInternal::verbose)
            dataLog(RawPointer(this), ": Clearing marks without doing anything else.\n");
        // We already know that the block is full and is already recognized as such, or that the
        // block did not survive the previous GC. So, we can clear mark bits the old fashioned
        // way. Note that it's possible for such a block to have newlyAllocated with an up-to-
        // date version! If it does, then we want to leave the newlyAllocated alone, since that
        // means that we had allocated in this previously empty block but did not fill it up, so
        // we created a newlyAllocated.
        header().m_marks.clearAll();
    } else {
        if (MarkedBlockInternal::verbose)
            dataLog(RawPointer(this), ": Doing things.\n");
        HeapVersion newlyAllocatedVersion = space()->newlyAllocatedVersion();
        if (header().m_newlyAllocatedVersion == newlyAllocatedVersion) {
            // When do we get here? The block could not have been filled up. The newlyAllocated bits would
            // have had to be created since the end of the last collection. The only things that create
            // them are aboutToMarkSlow, lastChanceToFinalize, and stopAllocating. If it had been
            // aboutToMarkSlow, then we shouldn't be here since the marks wouldn't be stale anymore. It
            // cannot be lastChanceToFinalize. So it must be stopAllocating. That means that we just
            // computed the newlyAllocated bits just before the start of an increment. When we are in that
            // mode, it seems as if newlyAllocated should subsume marks.
            ASSERT(header().m_newlyAllocated.subsumes(header().m_marks));
            header().m_marks.clearAll();
        } else {
            header().m_newlyAllocated.setAndClear(header().m_marks);
            header().m_newlyAllocatedVersion = newlyAllocatedVersion;
        }
    }
    clearHasAnyMarked();
    WTF::storeStoreFence();
    header().m_markingVersion = markingVersion;
    
    // Workaround for a clang regression <rdar://111818130>.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
#endif
    // This means we're the first ones to mark any object in this block.
    Locker bitLocker { directory->bitvectorLock() };
    directory->setIsMarkingNotEmpty(handle, true);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}

void MarkedBlock::sharedGCWindowWitnessSnapshot(HeapVersion markingVersion, Vector<HeapCell*>& candidates)
{
    // SharedGC "Wlr" read protocol (round-7 F1): the Wlr core marking
    // constraint runs on the conductor while parallel marker helpers
    // concurrently execute aboutToMarkSlow() above, which under
    // header().m_lock clears the whole marks bitmap (clearAll), can fold
    // marks into m_newlyAllocated (setAndClear) AND bump
    // m_newlyAllocatedVersion mid-marking, then storeStoreFence + version
    // store. The previous Wlr loop hoisted areMarksStale() once per block
    // and read isMarkedRaw()/isNewlyAllocatedStale() lock-free and
    // dependency-free, violating the documented protocol (see the isMarked()
    // comment in MarkedBlock.h) and racing all of those writes — TSAN-dirty,
    // and on weak memory models a load-load reorder could pair a current
    // version with a pre-clear stale marks word. Take the SAME header lock
    // for the whole per-block snapshot so the witness judgment is one
    // consistent view; the caller appends the candidates to the visitor only
    // AFTER this lock is dropped (appendJSCellOrAuxiliary can re-enter this
    // lock via aboutToMark -> aboutToMarkSlow). Lock order matches
    // aboutToMarkSlow: header lock outer, directory bitvector lock inner.
    //
    // Soundness lemma (previously UNSTATED, load-bearing for the old racy
    // reads; recorded here because the EVIDENCE.md §13 stale-mark-skip
    // narrowing candidate depends on it): a window-witnessed cell always has
    // stale mark bit == 0 — it came off a sweep-built free list and only
    // unmarked cells are free-listed — and its NA bit is stamped pre-stop
    // (MarkedBlock::Handle::stopAllocating, happens-before marking per Wlr
    // lemma L1) and is never CLEARED during marking. Under the lock the
    // snapshot is correct without the lemma; any future narrowing keyed on
    // STALE mark bits must keep both this lock protocol and this lemma, or
    // it silently reopens the under-retention hole.
    Locker locker { header().m_lock };
    bool naCurrent = !isNewlyAllocatedStale();
    BlockDirectory* directory = handle().directory();
    bool allocBit;
    {
        Locker bitLocker { directory->bitvectorLock() };
        allocBit = directory->isAllocated(&handle());
    }
    if (!naCurrent && !allocBit)
        return;
    bool marksStale = areMarksStale(markingVersion);
    handle().forEachCell(
        [&] (size_t, HeapCell* cell, HeapCell::Kind) -> IterationStatus {
            if (!marksStale && isMarkedRaw(cell))
                return IterationStatus::Continue;
            bool isNA = naCurrent && isNewlyAllocated(cell);
            if (!isNA && !allocBit)
                return IterationStatus::Continue; // No window witness: genuinely dead.
            candidates.append(cell);
            return IterationStatus::Continue;
        });
}

void MarkedBlock::resetAllocated()
{
    header().m_newlyAllocated.clearAll();
    header().m_newlyAllocatedVersion = MarkedSpace::nullVersion;
}

void MarkedBlock::resetMarks()
{
    // We want aboutToMarkSlow() to see what the mark bits were after the last collection. It uses
    // the version number to distinguish between the marks having already been stale before
    // beginMarking(), or just stale now that beginMarking() bumped the version. If we have a version
    // wraparound, then we will call this method before resetting the version to null. When the
    // version is null, aboutToMarkSlow() will assume that the marks were not stale as of before
    // beginMarking(). Hence the need to whip the marks into shape.
    if (areMarksStale())
        header().m_marks.clearAll();
    header().m_markingVersion = MarkedSpace::nullVersion;
}

// SharedGC (T9): the vm() uses in assertMarksNotStale/areMarksStale/isMarked
// below are thread-agnostic round-trips to the SERVER's marking version
// (block -> main VM -> vm.heap == server, deviation 3) — conductor-context
// OK and reachable from any client/marker thread.
#if ASSERT_ENABLED
void MarkedBlock::assertMarksNotStale()
{
    ASSERT(header().m_markingVersion == vm().heap.objectSpace().markingVersion());
}
#endif // ASSERT_ENABLED

bool MarkedBlock::areMarksStale()
{
    return areMarksStale(vm().heap.objectSpace().markingVersion());
}

bool MarkedBlock::Handle::areMarksStale()
{
    return m_block->areMarksStale();
}

bool MarkedBlock::isMarked(const void* p)
{
    return isMarked(vm().heap.objectSpace().markingVersion(), p);
}

void MarkedBlock::Handle::didConsumeFreeList()
{
    // SharedGC (T8 audit, I5b): called from LocalAllocator::didConsumeFreeList
    // — including the call at the top of allocateSlowCase BEFORE MSPL is
    // taken — and from stopAllocating paths. Safe without MSPL: the bit
    // writes below hold the directory's bitvector lock (addBlock's m_bits
    // resize also holds it), and m_isFreeListed/handle state are confined to
    // the handle's owner (I1: this thread holds the block via inUse).
    Locker locker { blockHeader().m_lock };
    if (MarkedBlockInternal::verbose)
        dataLog(RawPointer(this), ": MarkedBlock::Handle::didConsumeFreeList!\n");
    ASSERT(isFreeListed());
    m_isFreeListed = false;
    Locker bitLocker(m_directory->bitvectorLock());
    m_directory->setIsAllocated(this, true);
    m_directory->didFinishUsingBlock(bitLocker, this);
}

size_t MarkedBlock::markCount()
{
    return areMarksStale() ? 0 : header().m_marks.count();
}

void MarkedBlock::clearHasAnyMarked()
{
    header().m_biasedMarkCount = header().m_markCountBias;
}

void MarkedBlock::noteMarkedSlow()
{
    // SharedGC (T8 audit, I5b): marker-helper path; bit write under the
    // bitvector lock (see aboutToMarkSlow).
    BlockDirectory* directory = handle().directory();
    Locker locker { directory->bitvectorLock() };
    directory->setIsMarkingRetired(&handle(), true);
}

void MarkedBlock::Handle::removeFromDirectory()
{
    if (!m_directory)
        return;
    
    m_directory->removeBlock(this);
}

void MarkedBlock::Handle::didAddToDirectory(BlockDirectory* directory, unsigned index)
{
    ASSERT(m_index == std::numeric_limits<unsigned>::max());
    ASSERT(!m_directory);
    
    RELEASE_ASSERT(directory->subspace()->alignedMemoryAllocator() == m_alignedMemoryAllocator);
    
    m_index = index;
    m_directory = directory;
    blockHeader().m_subspace = directory->subspace();
    
    size_t cellSize = directory->cellSize();
    m_atomsPerCell = (cellSize + atomSize - 1) / atomSize;

    // Discount the payload atoms at the front so that m_startAtom can start on an atom such that
    // m_atomsPerCell increments from m_startAtom will get us exactly to endAtom when we have filled
    // up the payload region using bump allocation. This makes simplifies the computation of the
    // termination condition for iteration later.
    size_t numberOfUnallocatableAtoms = numberOfPayloadAtoms % m_atomsPerCell;
    m_startAtom = firstPayloadRegionAtom + numberOfUnallocatableAtoms;
    ASSERT(m_startAtom < firstPayloadRegionAtom + m_atomsPerCell);

    m_attributes = directory->attributes();

    if (!isJSCellKind(m_attributes.cellKind))
        RELEASE_ASSERT(m_attributes.destruction == DoesNotNeedDestruction);
    
    double markCountBias = -(Options::minMarkedBlockUtilization() * cellsPerBlock());
    
    // The mark count bias should be comfortably within this range.
    RELEASE_ASSERT(markCountBias > static_cast<double>(std::numeric_limits<int16_t>::min()));
    RELEASE_ASSERT(markCountBias < 0);
    
    // This means we haven't marked anything yet.
    blockHeader().m_biasedMarkCount = blockHeader().m_markCountBias = static_cast<int16_t>(markCountBias);
}

void MarkedBlock::Handle::didRemoveFromDirectory()
{
    ASSERT(m_index != std::numeric_limits<unsigned>::max());
    ASSERT(m_directory);
    
    m_index = std::numeric_limits<unsigned>::max();
    m_directory = nullptr;
    blockHeader().m_subspace = nullptr;
}

#if ASSERT_ENABLED
void MarkedBlock::assertValidCell(VM& vm, HeapCell* cell) const
{
    // SharedGC (T9): conductor-context OK — identity check against the one
    // main VM (every block on the shared server stamps the same VM).
    RELEASE_ASSERT(&vm == &this->vm());
    RELEASE_ASSERT(const_cast<MarkedBlock*>(this)->handle().cellAlign(cell) == cell);
}
#endif // ASSERT_ENABLED

void MarkedBlock::Handle::dumpState(PrintStream& out)
{
    CommaPrinter comma;
    Locker locker { directory()->bitvectorLock() };
    directory()->forEachBitVectorWithName(
        [&](auto vectorRef, const char* name) {
            out.print(comma, name, ":"_s, vectorRef[index()] ? "YES"_s : "no"_s);
        });
}

Subspace* MarkedBlock::Handle::subspace() const
{
    return directory()->subspace();
}

void MarkedBlock::Handle::sweep(FreeList* freeList)
{
    // SharedGC (T8 audit, I5b): legal shared-mode contexts — allocation slow
    // paths and synchronous sweeps under MSPL, or the conductor while the
    // world is stopped for all clients (the IncrementalSweeper is disabled
    // once shared). The assert below enforces exactly that; the lock-free
    // isInUse/isDestructible/isEmpty reads in this function and the
    // specializedSweep bit flips (which take the bitvector lock) are sound in
    // those contexts.
    //
    // Review round 4 — the m_weakSet.sweep() below: when shared, mutator-
    // concurrent (MSPL-held, world running) callers NEVER reach this with a
    // weak-bearing block — the weak-bearing carve-out skips such blocks at
    // all three MSPL sweep sites (LocalAllocator::tryAllocateIn, the steal
    // path, BlockDirectory::sweep) because MSPL does not exclude the
    // lock-free WeakSet::deallocate or weak finalizer-vs-owner lifetime
    // races. Weak-bearing blocks are therefore weak-swept only world-stopped
    // (conducted cycles) or at teardown (Heap::lastChanceToFinalize: MSPL
    // held AND no other mutator left). WeakSet::sweep asserts the lock/stop
    // half of this protocol.
    SweepingScope sweepingScope(*heap());
    m_directory->assertIsMutatorOrMutatorIsStopped();
    ASSERT(m_directory->isInUse(this));

    SweepMode sweepMode = freeList ? SweepToFreeList : SweepOnly;
    bool needsDestruction = m_attributes.destruction != DoesNotNeedDestruction && m_directory->isDestructible(this);

    m_weakSet.sweep();

    // If we don't "release" our read access without locking then the ThreadSafetyAnalysis code gets upset with the locker below.
    m_directory->releaseAssertAcquiredBitVectorLock();

    if (sweepMode == SweepOnly && !needsDestruction) {
        Locker locker(m_directory->bitvectorLock());
        m_directory->setIsUnswept(this, false);
        return;
    }

    if (m_isFreeListed) [[unlikely]] {
        dataLog("FATAL: ", RawPointer(this), "->sweep: block is free-listed.\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    if (isAllocated()) [[unlikely]] {
        dataLog("FATAL: ", RawPointer(this), "->sweep: block is allocated.\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    if (space()->isMarking())
        blockHeader().m_lock.lock();
    
    subspace()->didBeginSweepingToFreeList(this);
    
    if (needsDestruction) {
        subspace()->finishSweep(*this, freeList);
        return;
    }
    
    // Handle the no-destructor specializations here, since we have the most of those. This
    // ensures that they don't get re-specialized for every destructor space.
    
    EmptyMode emptyMode = this->emptyMode();
    ScribbleMode scribbleMode = this->scribbleMode();
    NewlyAllocatedMode newlyAllocatedMode = this->newlyAllocatedMode();
    MarksMode marksMode = this->marksMode();
    
    auto trySpecialized = [&] () -> bool {
        if (sweepMode != SweepToFreeList)
            return false;
        if (scribbleMode != DontScribble)
            return false;
        if (newlyAllocatedMode != DoesNotHaveNewlyAllocated)
            return false;
        
        switch (emptyMode) {
        case IsEmpty:
            switch (marksMode) {
            case MarksNotStale:
                specializedSweep<true, IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale>(freeList, IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, [] (VM&, JSCell*) { });
                return true;
            case MarksStale:
                specializedSweep<true, IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale>(freeList, IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, [] (VM&, JSCell*) { });
                return true;
            }
            break;
        case NotEmpty:
            switch (marksMode) {
            case MarksNotStale:
                specializedSweep<true, NotEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale>(freeList, IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, [] (VM&, JSCell*) { });
                return true;
            case MarksStale:
                specializedSweep<true, NotEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale>(freeList, IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, [] (VM&, JSCell*) { });
                return true;
            }
            break;
        }
        
        return false;
    };
    
    if (trySpecialized())
        return;

    // The template arguments don't matter because the first one is false.
    specializedSweep<false, IsEmpty, SweepOnly, BlockHasNoDestructors, DontScribble, HasNewlyAllocated, MarksStale>(freeList, emptyMode, sweepMode, BlockHasNoDestructors, scribbleMode, newlyAllocatedMode, marksMode, [] (VM&, JSCell*) { });
}

NO_RETURN_DUE_TO_CRASH NEVER_INLINE static void crashDueToGarbageCollectorClientDanglingReference_CheckRootsAndBarriers(HeapCell* heapCell, uint64_t cellFirst8Bytes, uint64_t zeroCounts, uint64_t bitfield, uint64_t subspaceHash, VM* blockVM, VM* actualVM)
{
#if PLATFORM(COCOA)
    StringPrintStream out;
    out.printf("JavaScriptCore garbage collector detected a dangling reference to cell %p. "
        "The referenced object was collected because it was not properly kept alive. "
        "JSC API clients: do not call JSValueUnprotect on values still in use, "
        "do not store JSValueRef in heap-allocated memory without calling JSValueProtect, "
        "do not use values after their JSContext has been destroyed, "
        "and do not use values across different JSContextGroups (JSVirtualMachines). "
        "WebKit developers: check for missing write barriers, incomplete visitChildren implementations, "
        "or unrooted GC objects.",
        heapCell);
    auto message = out.toCString();
    WTF::setCrashLogMessage(message.data());
    dataLogLn(message.data());
#endif
    CRASH_WITH_INFO(heapCell, cellFirst8Bytes, zeroCounts, bitfield, subspaceHash, blockVM, actualVM);
}

NO_RETURN_DUE_TO_CRASH NEVER_INLINE void MarkedBlock::dumpInfoAndCrashForInvalidHandleV2(HeapCell* heapCell, uint64_t cellFirst8Bytes, uint64_t zeroCounts, uint64_t bitfield, uint64_t subspaceHash, VM* blockVM, VM* actualVM)
{
    CRASH_WITH_INFO(heapCell, cellFirst8Bytes, zeroCounts, bitfield, subspaceHash, blockVM, actualVM);
}

NO_RETURN_DUE_TO_CRASH NEVER_INLINE void MarkedBlock::analyzeInvalidHandleAndCrash(AbstractLocker&, HeapCell* heapCell)
{
    VM* blockVM = header().m_vm;
    VM* actualVM = nullptr;
    bool isBlockVMValid = false;
    bool isBlockInSet = false;
    bool isBlockInDirectory = false;
    bool foundInBlockVM = false;
    size_t contiguousZeroBytesHeadOfBlock = 0;
    size_t totalZeroBytesInBlock = 0;
    uint64_t cellFirst8Bytes = 0;
    unsigned subspaceHash = 0;
    MarkedBlock::Handle* handle = nullptr;

    if (heapCell) {
        uint64_t* p = std::bit_cast<uint64_t*>(heapCell);
        cellFirst8Bytes = *p;
    }

    auto updateCrashLogMsg = [&](int line) {
#if PLATFORM(COCOA)
        StringPrintStream out;
        out.printf("Suspected memory corruption: invalid handle [line=%d]: markedBlock=%p; heapCell=%p; cellFirst8Bytes=%#llx; subspaceHash=%#x; contiguousZeros=%lu; totalZeros=%lu; blockVM=%p; actualVM=%p; isBlockVMValid=%d; isBlockInSet=%d; isBlockInDir=%d; foundInBlockVM=%d;",
            line, this, heapCell, cellFirst8Bytes, subspaceHash, contiguousZeroBytesHeadOfBlock, totalZeroBytesInBlock, blockVM, actualVM, isBlockVMValid, isBlockInSet, isBlockInDirectory, foundInBlockVM);
        auto message = out.toCString();
        WTF::setCrashLogMessage(message.data());
        dataLogLn(message.data());
#else
        UNUSED_PARAM(line);
#endif
    };
    updateCrashLogMsg(__LINE__);

    char* blockStart = std::bit_cast<char*>(this);
    bool sawNonZero = false;
    for (auto mem = blockStart; mem < blockStart + MarkedBlock::blockSize; mem++) {
        // Exclude the MarkedBlock::Header::m_lock from the zero scan since taking the lock writes a non-zero value.
        auto isMLockBytes = [blockStart](char* p) ALWAYS_INLINE_LAMBDA {
            constexpr size_t lockOffset = offsetOfHeader + OBJECT_OFFSETOF(MarkedBlock::Header, m_lock);
            size_t offset = p - blockStart;
            return lockOffset <= offset && offset < lockOffset + sizeof(MarkedBlock::Header::m_lock);
        };
        bool byteIsZero = !*mem;
        if (byteIsZero || isMLockBytes(mem)) {
            totalZeroBytesInBlock++;
            if (!sawNonZero)
                contiguousZeroBytesHeadOfBlock++;
        } else
            sawNonZero = true;
    }
    updateCrashLogMsg(__LINE__);

    isBlockVMValid = VMManager::findMatchingVM([&] (VM& vm) {
        return blockVM == &vm;
    });
    updateCrashLogMsg(__LINE__);

    if (isBlockVMValid) {
        MarkedSpace& objectSpace = blockVM->heap.objectSpace();
        isBlockInSet = objectSpace.blocks().set().contains(this);
        handle = objectSpace.findMarkedBlockHandleDebug(this);
        isBlockInDirectory = !!handle;
        foundInBlockVM = isBlockInSet || isBlockInDirectory;
        updateCrashLogMsg(__LINE__);
    }

    if (!foundInBlockVM) {
        // Search all VMs to see if this block belongs to any VM.
        VMManager::forEachVM([&](VM& vm) {
            if (!vm.isInService())
                return IterationStatus::Continue;

            MarkedSpace& objectSpace = vm.heap.objectSpace();
            isBlockInSet = objectSpace.blocks().set().contains(this);
            handle = objectSpace.findMarkedBlockHandleDebug(this);
            isBlockInDirectory = !!handle;
            // Either of them is true indicates that the block belongs to the VM.
            if (isBlockInSet || isBlockInDirectory) {
                actualVM = &vm;
                updateCrashLogMsg(__LINE__);
                return IterationStatus::Done;
            }
            return IterationStatus::Continue;
        });
    }
    updateCrashLogMsg(__LINE__);

    if (handle && handle->directory() && handle->directory()->subspace())
        subspaceHash = handle->directory()->subspace()->nameHash();
    updateCrashLogMsg(__LINE__);

    uint64_t bitfield = 0xab00ab01ab020000;
    if (!isBlockVMValid)
        bitfield |= 1 << 7;
    if (!isBlockInSet)
        bitfield |= 1 << 6;
    if (!isBlockInDirectory)
        bitfield |= 1 << 5;
    if (!foundInBlockVM)
        bitfield |= 1 << 4;

    static_assert(MarkedBlock::blockSize < (1ull << 32));
    uint64_t zeroCounts = contiguousZeroBytesHeadOfBlock | (static_cast<uint64_t>(totalZeroBytesInBlock) << 32);

    // If the block isn't attached to any VM's directory or block set, then the block was either already freed or the heapCell isn't
    // really a heapCell. Assume either of these cases are due to a GC client bug not keeping this cell or the cell pointing at this cell alive.
    if (!foundInBlockVM && !isBlockInSet && !isBlockInDirectory)
        crashDueToGarbageCollectorClientDanglingReference_CheckRootsAndBarriers(heapCell, cellFirst8Bytes, zeroCounts, bitfield, subspaceHash, blockVM, actualVM);

    // Otherwise, the block is attached to some VM yet in some inconsistent state that is probably due to general memory corruption.
    dumpInfoAndCrashForInvalidHandleV2(heapCell, cellFirst8Bytes, zeroCounts, bitfield, subspaceHash, blockVM, actualVM);
}

} // namespace JSC

namespace WTF {

void printInternal(PrintStream& out, JSC::MarkedBlock::Handle::SweepMode mode)
{
    switch (mode) {
    case JSC::MarkedBlock::Handle::SweepToFreeList:
        out.print("SweepToFreeList");
        return;
    case JSC::MarkedBlock::Handle::SweepOnly:
        out.print("SweepOnly");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace WTF

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
