/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CellContainerInlines.h"
#include "GCSegmentedArrayInlines.h"
#include "HeapInlines.h"
#include "MarkedBlockCage.h"
#include "MarkedBlockInlines.h"
#include "SlotVisitorInlines.h"
#include "StructureInlines.h"
#include "WriteBarrier.h"
#if ENABLE(AOT)
#include "AOTHeapHooks.h"
#endif
#include <bit>
#include <type_traits>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

static_assert(Structure::markingStampFor(MarkedSpace::nextVersion(std::numeric_limits<HeapVersion>::max())) != Structure::markingStampFor(std::numeric_limits<HeapVersion>::max()));

// Pass an appender only to functions that are always inlined: once its address escapes, the cursor and the counts leave registers.
// x86-64 has too few registers to hold them through the visits anyway. There the test of the mark is in line, and what follows for a cell without a mark is one function for all visits.
class FastMarkAppender {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(FastMarkAppender);
public:
    explicit FastMarkAppender(SlotVisitor&);
    ~FastMarkAppender();

    template<typename T, typename Traits> void append(const WriteBarrierBase<T, Traits>&);
    void appendValues(const WriteBarrierBase<Unknown>*, size_t count);
    void appendValuesHidden(const WriteBarrierBase<Unknown>*, size_t count);
    void appendUnbarriered(JSCell*);
    void appendUnbarriered(JSValue);
    void markAuxiliary(const void* base);
    void reportExtraMemoryVisited(size_t);
    CollectionScope collectionScope() const;
    HeapVersion markingVersion() const;
    bool mutatorsAreStopped() const;
    bool isAnalyzingHeap() const;
    bool visitsInline() const;
    size_t nonCellVisitCount() const;
    void didRace(JSCell*, const char* reason);

    void setCurrentCell(const JSCell*);
    void appendStructure(Structure*);
    // For a cell that lies in the cell that is being visited: that one is marked, so the marks of the block are those of this cycle. True if nobody has marked the inner cell before: then the caller visits it, here and now.
    bool markInnerCellOfVisitedCell(JSCell*);
    // For a cell that is marked and that nobody has visited or will.
    void appendMarkedCell(JSCell*);
    ALWAYS_INLINE MarkStackArray::Cursor& cursor() LIFETIME_BOUND { return m_cursor; }

private:
    void appendCell(JSCell*);
    void markAndPush(JSCell*);
    void pushNewlyMarkedCell(JSCell*, MarkedBlock&);
#if CPU(X86_64)
    NEVER_INLINE void markAndPushOutOfLine(JSCell*);
    NEVER_INLINE void markAndPushCellWithoutMark(JSCell*);
    NEVER_INLINE void pushNewlyMarkedCellOutOfLine(JSCell*);
#endif
    void count(JSC::Heap&, size_t bytes, size_t nonCellBytes);
    void flushCounts();
    static NEVER_INLINE void appendOutOfLine(SlotVisitor&, JSCell*);
    static NEVER_INLINE void appendToFullSegment(MarkStackArray&, const JSCell*);

    SlotVisitor& m_visitor;
    MarkStackArray::Cursor m_cursor;
    const HeapVersion m_markingVersion;
    const uint8_t m_markingStamp;
    const bool m_mutatorsAreStopped;
    const bool m_visitsInline;
    const JSCell* m_currentCell { nullptr };
#if !CPU(X86_64)
    JSC::Heap* m_heap;
    size_t m_visitCount { 0 };
    size_t m_bytesMarked { 0 };
    size_t m_nonCellVisitCount { 0 };
#endif
};

static_assert(std::is_same_v<std::remove_cvref_t<decltype(std::declval<MarkedBlock&>().marks().storage()[0])>, uint64_t>);
static_assert(MarkedBlock::atomSize == 16 && MarkedBlock::atomsPerBlock % 64 == 0);
static_assert(!(JSValue::NotCellMask & 0x0001fffffffffff8ull) && (JSValue::NotCellMask & 7) && !JSValue::ValueEmpty);

ALWAYS_INLINE FastMarkAppender::FastMarkAppender(SlotVisitor& visitor)
    : m_visitor(visitor)
    , m_cursor(visitor.m_collectorStack)
    , m_markingVersion(visitor.m_markingVersion)
    , m_markingStamp(Structure::markingStampFor(visitor.m_markingVersion))
    , m_mutatorsAreStopped(visitor.mutatorsAreStopped())
    , m_visitsInline(!visitor.m_isAnalyzingHeap)
#if !CPU(X86_64)
    , m_heap(visitor.m_bytesMarkedHeap)
#endif
{
}

ALWAYS_INLINE FastMarkAppender::~FastMarkAppender()
{
    flushCounts();
}

ALWAYS_INLINE void FastMarkAppender::flushCounts()
{
#if !CPU(X86_64)
    m_visitor.m_visitCount += m_visitCount;
    if (m_bytesMarked)
        m_visitor.noteBytesMarked(*m_heap, m_bytesMarked);
    m_visitor.m_nonCellVisitCount += m_nonCellVisitCount;
    m_visitCount = 0;
    m_bytesMarked = 0;
    m_nonCellVisitCount = 0;
#endif
}

ALWAYS_INLINE void FastMarkAppender::count(JSC::Heap& heap, size_t bytes, size_t nonCellBytes)
{
#if CPU(X86_64)
    ++m_visitor.m_visitCount;
    m_visitor.noteBytesMarked(heap, bytes);
    m_visitor.m_nonCellVisitCount += nonCellBytes;
#else
    if (&heap != m_heap) [[unlikely]] {
        flushCounts();
        m_heap = &heap;
    }
    ++m_visitCount;
    m_bytesMarked += bytes;
    m_nonCellVisitCount += nonCellBytes;
#endif
}

inline NEVER_INLINE void FastMarkAppender::appendOutOfLine(SlotVisitor& visitor, JSCell* cell)
{
    visitor.appendHiddenUnbarriered(cell);
}

inline NEVER_INLINE void FastMarkAppender::appendToFullSegment(MarkStackArray& stack, const JSCell* cell)
{
    stack.append(cell);
}

ALWAYS_INLINE void FastMarkAppender::markAndPush(JSCell* cell)
{
    ASSERT(cell && !cell->isPreciseAllocation() && !m_visitor.m_isAnalyzingHeap);
    using Word = uint64_t;
    MarkedBlock& block = cell->markedBlock();
    HeapVersion version = block.markingVersion();
    uintptr_t dependency = 0;
#if CPU(ARM64)
    // Always zero. Adding it to the address of the mark word orders that load after the load of the version, without a fence.
    __asm__ volatile("eor %w[dependency], %w[version], %w[version]" : [dependency] "=r"(dependency), [version] "+r"(version));
#else
    WTF::loadLoadFence();
#endif
    if (version != m_markingVersion) [[unlikely]] {
        m_cursor.store();
        appendOutOfLine(m_visitor, cell);
        m_cursor.load();
        return;
    }
    uintptr_t atom = std::bit_cast<uintptr_t>(cell) / MarkedBlock::atomSize;
    auto* word = std::bit_cast<Atomic<Word>*>(std::bit_cast<uintptr_t>(block.marks().storage().data() + atom / 64 % (MarkedBlock::atomsPerBlock / 64)) + dependency);
    if (word->loadRelaxed() >> (atom % 64) & 1)
        return;
    Word mask = static_cast<Word>(1) << (atom % 64);
    if (word->exchangeOr(mask, std::memory_order_relaxed) & mask) [[unlikely]]
        return;
    pushNewlyMarkedCell(cell, block);
}

ALWAYS_INLINE void FastMarkAppender::pushNewlyMarkedCell(JSCell* cell, MarkedBlock& block)
{
    ASSERT(!cell->isZapped());
    cell->setCellState(CellState::PossiblyGrey);
    HeapCell* outer = block.outerCellIfInnerCell(cell);
    if (!outer) [[likely]] {
        block.noteMarked();
        count(block.vm().heap, block.cellSizeInHeader(), 0);
    }
    if (m_cursor.next == m_cursor.end) [[unlikely]] {
        m_cursor.store();
        appendToFullSegment(m_visitor.m_collectorStack, cell);
        m_cursor.load();
    } else
        *m_cursor.next++ = cell;
    // Whoever holds an inner cell holds the cell that it lies in. No field says so.
    if (outer) [[unlikely]] {
        if (block.marks().get(block.atomNumber(outer)))
            return;
        m_cursor.store();
        appendOutOfLine(m_visitor, static_cast<JSCell*>(outer));
        m_cursor.load();
    }
}

#if CPU(X86_64)
inline NEVER_INLINE void FastMarkAppender::markAndPushOutOfLine(JSCell* cell)
{
    markAndPush(cell);
}

inline NEVER_INLINE void FastMarkAppender::pushNewlyMarkedCellOutOfLine(JSCell* cell)
{
    pushNewlyMarkedCell(cell, cell->markedBlock());
}

// For a cell in a block with the marks of this cycle. It calls nothing but as its last step, so it needs no frame: whatever is rare is decided first and left to another function.
inline NEVER_INLINE void FastMarkAppender::markAndPushCellWithoutMark(JSCell* cell)
{
    MarkedBlock& block = cell->markedBlock();
    uintptr_t atom = std::bit_cast<uintptr_t>(cell) / MarkedBlock::atomSize;
    auto* word = std::bit_cast<Atomic<uint64_t>*>(block.marks().storage().data() + atom / 64 % (MarkedBlock::atomsPerBlock / 64));
    uint64_t mask = static_cast<uint64_t>(1) << (atom % 64);
    if (word->exchangeOr(mask, std::memory_order_relaxed) & mask) [[unlikely]]
        return;
    if (block.hasInnerCells() || m_cursor.next == m_cursor.end || &block.vm().heap != m_visitor.m_bytesMarkedHeap) [[unlikely]]
        return pushNewlyMarkedCellOutOfLine(cell);
    ASSERT(!cell->isZapped());
    cell->setCellState(CellState::PossiblyGrey);
    ++m_visitor.m_visitCount;
    m_visitor.m_bytesMarkedInHeap += block.cellSizeInHeader();
    *m_cursor.next++ = cell;
    block.noteMarked();
}

ALWAYS_INLINE void FastMarkAppender::appendCell(JSCell* cell)
{
    MarkedBlock& block = cell->markedBlock();
    if (block.markingVersion() != m_markingVersion) [[unlikely]] {
        markAndPushOutOfLine(cell);
        return;
    }
    WTF::loadLoadFence();
    uintptr_t atom = std::bit_cast<uintptr_t>(cell) / MarkedBlock::atomSize;
    if (std::bit_cast<Atomic<uint64_t>*>(block.marks().storage().data() + atom / 64 % (MarkedBlock::atomsPerBlock / 64))->loadRelaxed() >> (atom % 64) & 1)
        return;
    markAndPushCellWithoutMark(cell);
}
#else
ALWAYS_INLINE void FastMarkAppender::appendCell(JSCell* cell)
{
    markAndPush(cell);
}
#endif

ALWAYS_INLINE void FastMarkAppender::appendUnbarriered(JSCell* cell)
{
    if (!cell)
        return;
    if (cell->isPreciseAllocation()) [[unlikely]] {
        m_cursor.store();
        appendOutOfLine(m_visitor, cell);
        m_cursor.load();
    } else
        appendCell(cell);
}

ALWAYS_INLINE void FastMarkAppender::appendUnbarriered(JSValue value)
{
    // One test for "not a cell, or empty": the tag bits land above bit 44, and subtracting one from the empty value wraps around.
    if ((std::rotr(static_cast<uint64_t>(JSValue::encode(value)), 3) - 1) >> 45)
        return;
    if (value.asCell()->isPreciseAllocation()) [[unlikely]] {
        m_cursor.store();
        appendOutOfLine(m_visitor, value.asCell());
        m_cursor.load();
    } else
        appendCell(value.asCell());
}

template<typename T, typename Traits>
ALWAYS_INLINE void FastMarkAppender::append(const WriteBarrierBase<T, Traits>& slot)
{
    appendUnbarriered(slot.get());
}

ALWAYS_INLINE void FastMarkAppender::appendValues(const WriteBarrierBase<Unknown>* barriers, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        append(barriers[i]);
}

ALWAYS_INLINE void FastMarkAppender::appendValuesHidden(const WriteBarrierBase<Unknown>* barriers, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        append(barriers[i]);
}

ALWAYS_INLINE void FastMarkAppender::appendStructure(Structure* structure)
{
    uint8_t& stamp = structure->markingStamp();
    if (stamp == m_markingStamp) [[likely]]
        return;
    appendCell(structure);
    stamp = m_markingStamp;
}

ALWAYS_INLINE bool FastMarkAppender::markInnerCellOfVisitedCell(JSCell* cell)
{
    using Word = uint64_t;
    uintptr_t atom = std::bit_cast<uintptr_t>(cell) / MarkedBlock::atomSize;
    auto* word = std::bit_cast<Atomic<Word>*>(cell->markedBlock().marks().storage().data() + atom / 64 % (MarkedBlock::atomsPerBlock / 64));
    Word mask = static_cast<Word>(1) << (atom % 64);
    if (word->loadRelaxed() & mask)
        return false;
    if (word->exchangeOr(mask, std::memory_order_relaxed) & mask) [[unlikely]]
        return false;
    cell->setCellState(CellState::PossiblyBlack);
    if (!m_mutatorsAreStopped)
        WTF::storeLoadFence();
    return true;
}

ALWAYS_INLINE void FastMarkAppender::appendMarkedCell(JSCell* cell)
{
    cell->setCellState(CellState::PossiblyGrey);
    if (m_cursor.next == m_cursor.end) [[unlikely]] {
        m_cursor.store();
        appendToFullSegment(m_visitor.m_collectorStack, cell);
        m_cursor.load();
    } else
        *m_cursor.next++ = cell;
}

ALWAYS_INLINE void FastMarkAppender::markAuxiliary(const void* base)
{
    HeapCell* cell = std::bit_cast<HeapCell*>(base);
    if (cell->isPreciseAllocation()) [[unlikely]] {
        m_visitor.markAuxiliary(cell);
        return;
    }
    MarkedBlock& block = cell->markedBlock();
    // Storage that lies in the cell of its owner lives and is counted with that cell.
    if (block.hasInnerCells()) [[unlikely]]
        return;
    Dependency dependency = block.aboutToMark(m_markingVersion, cell);
    if (block.testAndSetMarked(cell, dependency))
        return;
    // Storage that lies in the cell of its owner is counted with that cell.
    if (block.isInnerCell(cell)) [[unlikely]]
        return;
    block.noteMarked();
    size_t size = block.cellSizeInHeader();
    count(block.vm().heap, size, size);
}

ALWAYS_INLINE void FastMarkAppender::setCurrentCell(const JSCell* cell)
{
    m_currentCell = cell;
}

ALWAYS_INLINE void FastMarkAppender::reportExtraMemoryVisited(size_t size)
{
    if (!m_visitor.m_isFirstVisit)
        return;
    ASSERT(m_currentCell);
    JSC::Heap& heap = *m_currentCell->heap();
    if (&heap != m_visitor.m_extraMemoryHeap) [[unlikely]] {
        m_visitor.flushExtraMemoryVisited();
        m_visitor.m_extraMemoryHeap = &heap;
    }
#if CPU(X86_64)
    m_visitor.m_nonCellVisitCount += size;
#else
    m_nonCellVisitCount += size;
#endif
    m_visitor.m_extraMemoryInHeap += size;
}

ALWAYS_INLINE CollectionScope FastMarkAppender::collectionScope() const
{
    return m_visitor.collectionScope();
}

ALWAYS_INLINE HeapVersion FastMarkAppender::markingVersion() const
{
    return m_markingVersion;
}

ALWAYS_INLINE bool FastMarkAppender::mutatorsAreStopped() const
{
    return m_mutatorsAreStopped;
}

ALWAYS_INLINE bool FastMarkAppender::isAnalyzingHeap() const
{
    return m_visitor.isAnalyzingHeap();
}

ALWAYS_INLINE bool FastMarkAppender::visitsInline() const
{
    return m_visitsInline;
}

ALWAYS_INLINE size_t FastMarkAppender::nonCellVisitCount() const
{
#if CPU(X86_64)
    return m_visitor.m_nonCellVisitCount;
#else
    return m_visitor.m_nonCellVisitCount + m_nonCellVisitCount;
#endif
}

ALWAYS_INLINE void FastMarkAppender::didRace(JSCell* cell, const char* reason)
{
    m_visitor.didRace(cell, reason);
}

template<typename Visitor, typename Functor>
ALWAYS_INLINE void appendRoots(Visitor& visitor, const Functor& functor)
{
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        if (!visitor.isAnalyzingHeap()) [[likely]] {
            FastMarkAppender appender(visitor);
            functor(appender);
            return;
        }
    }
    functor(visitor);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
