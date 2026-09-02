/*
 * Copyright (C) 2016-2022 Apple Inc. All rights reserved.
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

#include "BlockDirectory.h"
#include "JSCast.h"
#include "MarkedBlock.h"
#include "MarkedSpace.h"
#include "Scribble.h"
#include "SuperSampler.h"
#include "VM.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

inline unsigned MarkedBlock::Handle::cellsPerBlock()
{
    return MarkedSpace::blockPayload / cellSize();
}

inline bool MarkedBlock::isNewlyAllocatedStale() const
{
    return header().m_newlyAllocatedVersion != space()->newlyAllocatedVersion();
}

inline bool MarkedBlock::hasAnyNewlyAllocated()
{
    return !isNewlyAllocatedStale();
}

// SharedGC (T9): conductor-context OK — round-trip via the main VM back to
// the server heap (see MarkedBlock::vm(), MarkedBlock.h); thread-agnostic.
inline JSC::Heap* MarkedBlock::heap() const
{
    return &vm().heap;
}

inline MarkedSpace* MarkedBlock::space() const
{
    return &heap()->objectSpace();
}

inline MarkedSpace* MarkedBlock::Handle::space() const
{
    return &heap()->objectSpace();
}

inline bool MarkedBlock::marksConveyLivenessDuringMarking(HeapVersion markingVersion)
{
    return marksConveyLivenessDuringMarking(header().m_markingVersion, markingVersion);
}

inline bool MarkedBlock::marksConveyLivenessDuringMarking(HeapVersion myMarkingVersion, HeapVersion markingVersion)
{
    // This returns true if any of these is true:
    // - We just created the block and so the bits are clear already.
    // - This block has objects marked during the last GC, and so its version was up-to-date just
    //   before the current collection did beginMarking(). This means that any objects that have
    //   their mark bit set are valid objects that were never deleted, and so are candidates for
    //   marking in any conservative scan. Using our jargon, they are "live".
    // - We did ~2^32 collections and rotated the version back to null, so we needed to hard-reset
    //   everything. If the marks had been stale, we would have cleared them. So, we can be sure that
    //   any set mark bit reflects objects marked during last GC, i.e. "live" objects.
    // It would be absurd to use this method when not collecting, since this special "one version
    // back" state only makes sense when we're in a concurrent collection and have to be
    // conservative.
    ASSERT(space()->isMarking());
    if (heap()->collectionScope() != CollectionScope::Full)
        return false;
    return myMarkingVersion == MarkedSpace::nullVersion
        || MarkedSpace::nextVersion(myMarkingVersion) == markingVersion;
}

inline bool MarkedBlock::Handle::isAllocated()
{
    // SharedGC (T8 audit, I5b): lock-free directory-bit read — the assert
    // restricts shared-mode callers to WSAC v MSPL (see
    // BlockDirectory::assertIsMutatorOrMutatorIsStopped).
    m_directory->assertIsMutatorOrMutatorIsStopped();
    return m_directory->isAllocated(this);
}

ALWAYS_INLINE bool MarkedBlock::Handle::isLive(HeapVersion markingVersion, HeapVersion newlyAllocatedVersion, bool isMarking, const HeapCell* cell)
{
    m_directory->assertIsMutatorOrMutatorIsStopped();
    if (m_directory->isAllocated(this))
        return true;

    // We need to do this while holding the lock because marks might be stale. In that case, newly
    // allocated will not yet be valid. Consider this interleaving.
    //
    // One thread is doing this:
    //
    // 1) IsLiveChecksNewlyAllocated: We check if newly allocated is valid. If it is valid, and the bit is
    //    set, we return true. Let's assume that this executes atomically. It doesn't have to in general,
    //    but we can assume that for the purpose of seeing this bug.
    //
    // 2) IsLiveChecksMarks: Having failed that, we check the mark bits. This step implies the rest of
    //    this function. It happens under a lock so it's atomic.
    //
    // Another thread is doing:
    //
    // 1) AboutToMarkSlow: This is the entire aboutToMarkSlow function, and let's say it's atomic. It
    //    sorta is since it holds a lock, but that doesn't actually make it atomic with respect to
    //    IsLiveChecksNewlyAllocated, since that does not hold a lock in our scenario.
    //
    // The harmful interleaving happens if we start out with a block that has stale mark bits that
    // nonetheless convey liveness during marking (the off-by-one version trick). The interleaving is
    // just:
    //
    // IsLiveChecksNewlyAllocated AboutToMarkSlow IsLiveChecksMarks
    //
    // We started with valid marks but invalid newly allocated. So, the first part doesn't think that
    // anything is live, but dutifully drops down to the marks step. But in the meantime, we clear the
    // mark bits and transfer their contents into newlyAllocated. So IsLiveChecksMarks also sees nothing
    // live. Ooops!
    //
    // Fortunately, since this is just a read critical section, we can use a CountingLock.
    //
    // Probably many users of CountingLock could use its lambda-based and locker-based APIs. But here, we
    // need to ensure that everything is ALWAYS_INLINE. It's hard to do that when using lambdas. It's
    // more reliable to write it inline instead. Empirically, it seems like how inline this is has some
    // impact on perf - around 2% on splay if you get it wrong.

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
                // A marker's concurrentTestAndSet races this optimistic read. The
                // fencelessValidate re-check tolerates the race; the relaxed atomic
                // read makes the word access defined. Codegen is identical.
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

inline bool MarkedBlock::Handle::isLiveCell(HeapVersion markingVersion, HeapVersion newlyAllocatedVersion, bool isMarking, const void* p)
{
    if (!m_block->isAtom(p))
        return false;
    return isLive(markingVersion, newlyAllocatedVersion, isMarking, static_cast<const HeapCell*>(p));
}

inline bool MarkedBlock::Handle::isLive(const HeapCell* cell)
{
    return isLive(space()->markingVersion(), space()->newlyAllocatedVersion(), space()->isMarking(), cell);
}

inline bool MarkedBlock::Handle::isLiveCell(const void* p)
{
    return isLiveCell(space()->markingVersion(), space()->newlyAllocatedVersion(), space()->isMarking(), p);
}

inline bool MarkedBlock::Handle::areMarksStaleForSweep()
{
    return marksMode() == MarksStale;
}

// The following has to be true for specialization to kick in:
//
// sweepMode == SweepToFreeList
// scribbleMode == DontScribble
// newlyAllocatedMode == DoesNotHaveNewlyAllocated
//
// emptyMode = IsEmpty
//     destructionMode = DoesNotNeedDestruction
//         marksMode = MarksNotStale (1)
//         marksMode = MarksStale (2)
// emptyMode = NotEmpty
//     destructionMode = DoesNotNeedDestruction
//         marksMode = MarksNotStale (3)
//         marksMode = MarksStale (4)
//     destructionMode = NeedsDestruction
//         marksMode = MarksNotStale (5)
//         marksMode = MarksStale (6)
//
// Only the DoesNotNeedDestruction one should be specialized by MarkedBlock.

template<bool specialize, MarkedBlock::Handle::EmptyMode specializedEmptyMode, MarkedBlock::Handle::SweepMode specializedSweepMode, MarkedBlock::Handle::SweepDestructionMode specializedDestructionMode, MarkedBlock::Handle::ScribbleMode specializedScribbleMode, MarkedBlock::Handle::NewlyAllocatedMode specializedNewlyAllocatedMode, MarkedBlock::Handle::MarksMode specializedMarksMode, typename DestroyFunc>
void MarkedBlock::Handle::specializedSweep(FreeList* freeList, MarkedBlock::Handle::EmptyMode emptyMode, MarkedBlock::Handle::SweepMode sweepMode, MarkedBlock::Handle::SweepDestructionMode destructionMode, MarkedBlock::Handle::ScribbleMode scribbleMode, MarkedBlock::Handle::NewlyAllocatedMode newlyAllocatedMode, MarkedBlock::Handle::MarksMode marksMode, const DestroyFunc& destroyFunc)
{
    constexpr bool verbose = false;
    if (specialize) {
        emptyMode = specializedEmptyMode;
        sweepMode = specializedSweepMode;
        destructionMode = specializedDestructionMode;
        scribbleMode = specializedScribbleMode;
        newlyAllocatedMode = specializedNewlyAllocatedMode;
        marksMode = specializedMarksMode;
    }

    RELEASE_ASSERT(!(destructionMode == BlockHasNoDestructors && sweepMode == SweepOnly));

    SuperSamplerScope superSamplerScope(false);

    MarkedBlock& block = this->block();
    MarkedBlock::Header& header = block.header();

    dataLogLnIf(verbose, RawPointer(this), "/", RawPointer(&block), ": MarkedBlock::Handle::specializedSweep!");

    unsigned cellSize = this->cellSize();
    char* payloadEnd = std::bit_cast<char*>(block.atoms() + numberOfAtoms);
    char* payloadBegin = std::bit_cast<char*>(block.atoms() + m_startAtom);
    RELEASE_ASSERT(static_cast<size_t>(payloadEnd - payloadBegin) <= payloadSize, payloadBegin, payloadEnd, &block, cellSize, m_startAtom);

    // vm is the main VM (server-owned block); destroyFunc takes the VM as the
    // conventional destroy argument (cell destructors are VM-global, not
    // calling-thread-coupled).
    VM& vm = this->vm();
    bool isMarking = space()->isMarking();
    // Only GIL-off shared-server mutators sweep different directories
    // concurrently (per-directory stripes); WeakRandom::getUint64() mutates
    // its state, so only that shape takes the lock. Flag-off and GIL-on
    // sweeps are serialized by the JSLock or the stop window.
    const bool sweepsMayRunConcurrently = m_directory->heap().isSharedServer() && g_jscConfig.gilOffProcess;
    uint64_t secret;
    if (sweepsMayRunConcurrently) [[unlikely]]
        secret = vm.heapRandomUint64Concurrent();
    else
        secret = vm.heapRandom().getUint64();

    auto destroy = [&] (void* cell) {
        JSCell* jsCell = static_cast<JSCell*>(cell);
        if (!jsCell->isZapped()) {
            destroyFunc(vm, jsCell);
            jsCell->zap(HeapCell::Destruction);
        }
    };

    auto setBits = [&] (bool isEmpty) ALWAYS_INLINE_LAMBDA {
        // SharedGC (T8 audit, I5b): bit flips under the bitvector lock —
        // safe against addBlock's m_bits resize regardless of whether this
        // sweep runs under MSPL (mutator slow path) or on the conductor.
        Locker locker { m_directory->bitvectorLock() };
        bool wasUnswept = m_directory->isUnswept(this);
        m_directory->setIsUnswept(this, false);
        // SharedGC (I5b): re-derive the destructible bit entirely under the
        // BVL held above. The old "destructionMode != BlockHasNoDestructors"
        // conjunct baked in the lock-free isDestructible read from
        // MarkedBlock::sweep's needsDestruction decision; with mutators
        // allowed to flip the bit under the BVL alone
        // (Handle::setIsDestructible), that stale-false decision could erase
        // a concurrent flip here with a perfectly ordered locked write.
        // Dropping it is behavior-preserving when serialized (if the sweep
        // specialized to BlockHasNoDestructors, the bit was false at decision
        // time and, absent a concurrent flip, still is under the lock) and
        // makes the bit genuinely monotone-toward-true between
        // destructor-running sweeps.
        m_directory->setIsDestructible(this, m_attributes.destruction == DestructionMode::MayNeedDestruction && !isEmpty && m_directory->isDestructible(this));
        // T2-bimodal32: keep the per-Handle hint in lockstep with the
        // re-derived directory bit so the next mutator phase starts with a
        // correct (possibly false) hint. The only true->false transition here
        // is isEmpty (the other two conjuncts are per-block-lifetime stable
        // resp. monotone under the BVL), and isEmpty means no live cell in
        // this block — so no concurrent lock-free hint reader can exist for a
        // true->false write. true->true / false->* writes are trivially safe
        // against a concurrent relaxed reader (stale-false falls through to
        // the BVL). Gated isSharedServer() && gilOffProcess so flag-off / W=1
        // sweep codegen is untouched AND the hint is only ever written where
        // ISS is process-lifetime sticky (Heap::pollIssRevertIfNeeded
        // early-returns under gilOffProcess; under !gilOffProcess a §10D
        // revert would otherwise leave a stale-true hint that survives the
        // isEmpty directory-bit clear here and permanently shadows a later
        // 2nd-epoch notifyNeedsDestruction → JSString::destroy never runs,
        // StringImpl ref leak).
        if (sweepsMayRunConcurrently) [[unlikely]]
            WTF::atomicStore(&m_isDestructibleHint, m_directory->isDestructible(this), std::memory_order_relaxed);
        m_directory->setIsEmpty(this, false);
        if (sweepMode == SweepToFreeList)
            m_isFreeListed = true;
        else if (isEmpty)
            m_directory->setIsEmpty(this, true);
        return wasUnswept;
    };

    if (emptyMode == IsEmpty || (marksMode != MarksNotStale && newlyAllocatedMode != HasNewlyAllocated)) {
        // This is an incredibly powerful assertion that checks the sanity of our block bits.
        if (marksMode == MarksNotStale && !header.m_marks.isEmpty()) [[unlikely]] {
            WTF::dataFile().atomically(
                [&] (PrintStream& out) {
                    out.print("Block ", RawPointer(&block), ": marks not empty!\n");
                    out.print("Block lock is held: ", header.m_lock.isHeld(), "\n");
                    out.print("Marking version of block: ", header.m_markingVersion, "\n");
                    out.print("Marking version of heap: ", space()->markingVersion(), "\n");
                    UNREACHABLE_FOR_PLATFORM();
                });
        }

        // We only want to discard the newlyAllocated bits if we're creating a FreeList,
        // otherwise we would lose information on what's currently alive.
        if (sweepMode == SweepToFreeList && newlyAllocatedMode == HasNewlyAllocated)
            header.m_newlyAllocatedVersion = MarkedSpace::nullVersion;

        bool wasUnswept = setBits(true);
        if (isMarking)
            header.m_lock.unlock();
        if (destructionMode == BlockHasDestructors) {
            if (wasUnswept) {
                for (char* cell = payloadBegin; cell < payloadEnd; cell += cellSize)
                    destroy(cell);
            }
        }
        if (sweepMode == SweepToFreeList) {
            if (scribbleMode == Scribble) [[unlikely]]
                scribble(payloadBegin, payloadEnd - payloadBegin);
            FreeCell* interval = reinterpret_cast_ptr<FreeCell*>(payloadBegin);
            interval->makeLast(payloadEnd - payloadBegin, secret);
            freeList->initialize(interval, secret, payloadEnd - payloadBegin);
        }
        dataLogLnIf(verbose, "Quickly swept block ", RawPointer(this), " with cell size ", cellSize, " and attributes ", m_attributes, ": ", pointerDump(freeList), " isMarking: ", isMarking, " sweepMode: ", sweepMode);
        return;
    }

    WTF::BitSet<atomsPerBlock> live;
    if (marksMode == MarksNotStale && newlyAllocatedMode == HasNewlyAllocated) {
        // TSAN (JSC GIL-off, family marks-bitset-plain-readers): m_marks
        // words are concurrently CASed by SlotVisitor::testAndSetMarked on
        // another thread; the plain struct copy lowers to __tsan_memcpy and
        // is UB against that atomic writer. Snapshot via word-wise relaxed
        // loads — the m_markingVersion fence protocol already orders the
        // versions; this only fixes the read shape. Identical x86 codegen.
        live.concurrentCopyFrom(header.m_marks);
        live.merge(header.m_newlyAllocated);
    } else if (marksMode == MarksNotStale)
        live.concurrentCopyFrom(header.m_marks);
    else
        live = header.m_newlyAllocated;

    // We only want to discard the newlyAllocated bits if we're creating a FreeList,
    // otherwise we would lose information on what's currently alive.
    if (sweepMode == SweepToFreeList && newlyAllocatedMode == HasNewlyAllocated)
        header.m_newlyAllocatedVersion = MarkedSpace::nullVersion;

    bool wasUnswept = setBits(false);

    // We captured a snapshot of liveness information, so we no longer need to hold a lock!
    // Only thing we need at this point is just |live| BitSet.
    if (isMarking)
        header.m_lock.unlock();

    auto sweepBlock = [&]<bool needsDestruction>() ALWAYS_INLINE_LAMBDA {
        size_t freedBytes = 0;
        FreeCell* head = nullptr;
        FreeCell* cursor = nullptr;
        size_t cursorIntervalBytes = 0;
        auto pushInterval = [&](FreeCell* cell, size_t intervalBytes) {
            if constexpr (needsDestruction) {
                for (char* target = std::bit_cast<char*>(cell); target < (std::bit_cast<char*>(cell) + intervalBytes); target += cellSize)
                    destroy(target);
            }

            if (sweepMode == SweepToFreeList) {
                if (scribbleMode == Scribble) [[unlikely]]
                    scribble(cell, intervalBytes);

                if (!head)
                    head = cell;

                if (cursor) [[likely]]
                    cursor->setNext(cell, cursorIntervalBytes, secret);

                cursor = cell;
                cursorIntervalBytes = intervalBytes;
                freedBytes += intervalBytes;
            }
        };

        unsigned potentiallyFreeCell = m_startAtom;
        auto handleLiveCell = [&](unsigned index) {
            ASSERT(!((index - m_startAtom) % m_atomsPerCell));
            if (potentiallyFreeCell != index) {
                FreeCell* cell = std::bit_cast<FreeCell*>(&block.atoms()[potentiallyFreeCell]);
                pushInterval(cell, (index - potentiallyFreeCell) * atomSize);
            }
            potentiallyFreeCell = index + m_atomsPerCell;
        };
        live.forEachSetBit([&](unsigned index) {
            handleLiveCell(index);
        });
        handleLiveCell(endAtom);

        if (sweepMode == SweepToFreeList) {
            if (cursor)
                cursor->makeLast(cursorIntervalBytes, secret);
            freeList->initialize(head, secret, freedBytes);
        }
    };

    if (destructionMode == BlockHasNoDestructors || !wasUnswept)
        sweepBlock.template operator()</* needsDestruction */ false>();
    else
        sweepBlock.template operator()</* needsDestruction */ true>();

    dataLogLnIf(verbose, "Slowly swept block ", RawPointer(&block), " with cell size ", cellSize, " and attributes ", m_attributes, ": ", pointerDump(freeList), " isMarking: ", isMarking, " sweepMode: ", sweepMode);
}

template<typename DestroyFunc>
void MarkedBlock::Handle::finishSweepKnowingHeapCellType(FreeList* freeList, const DestroyFunc& destroyFunc)
{
    SweepMode sweepMode = freeList ? SweepToFreeList : SweepOnly;
    SweepDestructionMode destructionMode = this->sweepDestructionMode();
    EmptyMode emptyMode = this->emptyMode();
    ScribbleMode scribbleMode = this->scribbleMode();
    NewlyAllocatedMode newlyAllocatedMode = this->newlyAllocatedMode();
    MarksMode marksMode = this->marksMode();

    auto trySpecialized = [&] () -> bool {
        if (scribbleMode != DontScribble)
            return false;
        if (newlyAllocatedMode != DoesNotHaveNewlyAllocated)
            return false;
        if (destructionMode != BlockHasDestructors)
            return false;

        switch (emptyMode) {
        case IsEmpty:
            switch (sweepMode) {
            case SweepOnly:
                switch (marksMode) {
                case MarksNotStale:
                    specializedSweep<true, IsEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale>(freeList, IsEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, destroyFunc);
                    return true;
                case MarksStale:
                    specializedSweep<true, IsEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale>(freeList, IsEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, destroyFunc);
                    return true;
                }
                RELEASE_ASSERT_NOT_REACHED();
            case SweepToFreeList:
                switch (marksMode) {
                case MarksNotStale:
                    specializedSweep<true, IsEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale>(freeList, IsEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, destroyFunc);
                    return true;
                case MarksStale:
                    specializedSweep<true, IsEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale>(freeList, IsEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, destroyFunc);
                    return true;
                }
            }
            RELEASE_ASSERT_NOT_REACHED();
        case NotEmpty:
            switch (sweepMode) {
            case SweepOnly:
                switch (marksMode) {
                case MarksNotStale:
                    specializedSweep<true, NotEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale>(freeList, NotEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, destroyFunc);
                    return true;
                case MarksStale:
                    specializedSweep<true, NotEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale>(freeList, NotEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, destroyFunc);
                    return true;
                }
                RELEASE_ASSERT_NOT_REACHED();
            case SweepToFreeList:
                switch (marksMode) {
                case MarksNotStale:
                    specializedSweep<true, NotEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale>(freeList, NotEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, destroyFunc);
                    return true;
                case MarksStale:
                    specializedSweep<true, NotEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale>(freeList, NotEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, destroyFunc);
                    return true;
                }
            }
        }

        return false;
    };

    if (trySpecialized())
        return;

    // The template arguments don't matter because the first one is false.
    specializedSweep<false, IsEmpty, SweepOnly, BlockHasNoDestructors, DontScribble, HasNewlyAllocated, MarksStale>(freeList, emptyMode, sweepMode, destructionMode, scribbleMode, newlyAllocatedMode, marksMode, destroyFunc);
}

inline MarkedBlock::Handle::SweepDestructionMode MarkedBlock::Handle::sweepDestructionMode()
{
    if (m_attributes.destruction != DoesNotNeedDestruction)
        return BlockHasDestructors;
    return BlockHasNoDestructors;
}

inline bool MarkedBlock::Handle::isEmpty()
{
    m_directory->assertIsMutatorOrMutatorIsStopped();
    return m_directory->isEmpty(this);
}

inline void MarkedBlock::Handle::setIsDestructible(bool value)
{
    // SharedGC (I5b): holding this directory's bitvector lock is by itself a
    // sanctioned bitvector-access discipline — addBlock's m_bits resize (the
    // sole I5b writer) also takes the BVL, so the locked flip below cannot
    // race it. assertIsMutatorOrMutatorIsStopped()'s shared-server arm checks
    // the LOCK-FREE disciplines only (world-stopped or MSPL), and an ordinary
    // mutator legitimately reaches here with neither: e.g.
    // JSRopeString::convertToNonRope -> HeapCell::notifyNeedsDestruction when
    // rope resolution allocates a destructible backing store. So only run the
    // mutator-or-stopped assert when the heap is not a shared server; the
    // releaseAssertAcquiredBitVectorLock() keeps thread-safety-analysis
    // capability state identical on both branches, and the assert runs before
    // the Locker (the removeBlock ordering) so we never release a shared
    // capability while holding the lock exclusively. The whole gate sits
    // under ASSERT_ENABLED so release builds gain no isSharedServer() check.
    // The lock-free destructible read in MarkedBlock::sweep stays sound
    // against a concurrent BVL-held flip because the setBits lambda in
    // specializedSweep re-derives the bit from isDestructible(this) under the
    // BVL (no stale destructionMode conjunct), making the bit
    // monotone-toward-true between destructor-running sweeps: a stale-false
    // read only skips destructors for a sweep that cannot reclaim the
    // still-live newly-converted cell, and the next sweep honors the bit.
#if ASSERT_ENABLED
    if (!m_directory->heap().isSharedServer()) {
        m_directory->assertIsMutatorOrMutatorIsStopped();
        m_directory->releaseAssertAcquiredBitVectorLock();
    }
#endif
    // GIL-off shared server: N mutators resolving ropes into the same block
    // would otherwise all take the directory's single bitvector lock to
    // re-set an already-true bit, so consult the per-Handle hint first. The
    // bit is monotone-toward-true between destructor-running sweeps (setBits
    // in specializedSweep re-derives bit and hint together under the BVL), so
    // an observed hint==true means the directory bit is true for the rest of
    // this mutator phase. The hint is kept in lockstep with the bit for both
    // values, published only after the locked write. It lives on the Handle
    // rather than being a lock-free m_bits read because this caller holds
    // neither MSPL nor BVL and could race a same-directory addBlock resize.
    // isSharedServer() is false at flag-off and with one client, so those
    // take the plain locked path; the gilOffProcess conjunct confines the
    // hint to the regime where the shared server is process-lifetime sticky
    // (a revert would strand a stale-true hint across the isEmpty directory-
    // bit clear and a later notifyNeedsDestruction would never run
    // JSString::destroy).
    if (m_directory->heap().isSharedServer() && g_jscConfig.gilOffProcess) [[unlikely]] {
        if (value && WTF::atomicLoad(&m_isDestructibleHint, std::memory_order_relaxed))
            return;
        Locker locker { m_directory->bitvectorLock() };
        m_directory->setIsDestructible(this, value);
        // Relaxed store: the only consumer is the early return above, whose
        // stale-false outcome is the safe locked fallback.
        WTF::atomicStore(&m_isDestructibleHint, value, std::memory_order_relaxed);
        return;
    }
    Locker locker { m_directory->bitvectorLock() };
    return m_directory->setIsDestructible(this, value);
}

inline MarkedBlock::Handle::EmptyMode MarkedBlock::Handle::emptyMode()
{
    // It's not obvious, but this is the only way to know if the block is empty. It's the only
    // bit that captures these caveats:
    // - It's true when the block is freshly allocated.
    // - It's true if the block had been swept in the past, all destructors were called, and that
    //   sweep proved that the block is empty.
    return isEmpty() ? IsEmpty : NotEmpty;
}

inline MarkedBlock::Handle::ScribbleMode MarkedBlock::Handle::scribbleMode()
{
    return scribbleFreeCells() ? Scribble : DontScribble;
}

inline MarkedBlock::Handle::NewlyAllocatedMode MarkedBlock::Handle::newlyAllocatedMode()
{
    return block().hasAnyNewlyAllocated() ? HasNewlyAllocated : DoesNotHaveNewlyAllocated;
}

inline MarkedBlock::Handle::MarksMode MarkedBlock::Handle::marksMode()
{
    HeapVersion markingVersion = space()->markingVersion();
    bool marksAreUseful = !block().areMarksStale(markingVersion);
    if (space()->isMarking())
        marksAreUseful |= block().marksConveyLivenessDuringMarking(markingVersion);
    return marksAreUseful ? MarksNotStale : MarksStale;
}

template <typename Functor>
inline IterationStatus MarkedBlock::Handle::forEachLiveCell(const Functor& functor)
{
    // FIXME: This is not currently efficient to use in the constraint solver because isLive() grabs a
    // lock to protect itself from concurrent calls to aboutToMarkSlow(). But we could get around this by
    // having this function grab the lock before and after the iteration, and check if the marking version
    // changed. If it did, just run again. Inside the loop, we only need to ensure that if a race were to
    // happen, we will just overlook objects. I think that because of how aboutToMarkSlow() does things,
    // a race ought to mean that it just returns false when it should have returned true - but this is
    // something that would have to be verified carefully.
    //
    // NOTE: Some users of forEachLiveCell require that their callback is called exactly once for
    // each live cell. We could optimize this function for those users by using a slow loop if the
    // block is in marks-mean-live mode. That would only affect blocks that had partial survivors
    // during the last collection and no survivors (yet) during this collection.
    //
    // https://bugs.webkit.org/show_bug.cgi?id=180315

    HeapCell::Kind kind = m_attributes.cellKind;
    for (size_t i = m_startAtom; i < endAtom; i += m_atomsPerCell) {
        HeapCell* cell = reinterpret_cast_ptr<HeapCell*>(&m_block->atoms()[i]);
        if (!isLive(cell))
            continue;

        if (functor(i, cell, kind) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

template <typename Functor>
inline IterationStatus MarkedBlock::Handle::forEachDeadCell(const Functor& functor)
{
    HeapCell::Kind kind = m_attributes.cellKind;
    for (size_t i = m_startAtom; i < endAtom; i += m_atomsPerCell) {
        HeapCell* cell = reinterpret_cast_ptr<HeapCell*>(&m_block->atoms()[i]);
        if (isLive(cell))
            continue;

        if (functor(cell, kind) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

template <typename Functor>
inline IterationStatus MarkedBlock::Handle::forEachMarkedCell(const Functor& functor)
{
    HeapCell::Kind kind = m_attributes.cellKind;
    MarkedBlock& block = this->block();
    bool areMarksStale = block.areMarksStale();
    WTF::loadLoadFence();
    if (areMarksStale)
        return IterationStatus::Continue;
    IterationStatus result = IterationStatus::Continue;
    auto visit = [&](size_t i) ALWAYS_INLINE_LAMBDA {
        HeapCell* cell = reinterpret_cast_ptr<HeapCell*>(&m_block->atoms()[i]);
        result = functor(i, cell, kind);
        return result;
    };
#if TSAN_ENABLED
    // Parallel markers CAS m_marks while the Output constraint walks it (as
    // upstream does; the constraint runs again until nothing changes). Under
    // TSAN, walk a copy made with relaxed loads, as specializedSweep does.
    std::remove_cvref_t<decltype(block.header().m_marks)> marks;
    marks.concurrentCopyFrom(block.header().m_marks);
    marks.forEachSetBit(visit);
#else
    block.header().m_marks.forEachSetBit(visit);
#endif
    return result;
}

inline void MarkedBlock::Handle::shrink()
{
    m_weakSet.shrink();
}

inline size_t MarkedBlock::Handle::markCount()
{
    return m_block->markCount();
}

inline size_t MarkedBlock::Handle::size()
{
    return markCount() * cellSize();
}

inline void MarkedBlock::noteMarked()
{
    // This is racy by design. We don't want to pay the price of an atomic increment!
    // FIXME: We could probably make this relaxed atomics on Apple ARM64E since it's mostly free for those chips.
    MarkCountBiasType biasedMarkCount = header().m_biasedMarkCount;
    ++biasedMarkCount;
    header().m_biasedMarkCount = biasedMarkCount;
    if (!biasedMarkCount) [[unlikely]]
        noteMarkedSlow();
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
