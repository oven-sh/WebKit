/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "HeapCompaction.h"

#include "AbstractSlotVisitorInlines.h"
#include "BlockDirectoryInlines.h"
#include "CollectorInlines.h"
#include "ConservativeRoots.h"
#include "GCSegmentedArrayInlines.h"
#include "GetterSetter.h"
#include "HeapCompactionInterrupt.h"
#include "HeapCompactionSlots.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSMap.h"
#include "JSSet.h"
#include "JSWeakMap.h"
#include "JSWeakSet.h"
#include "MachineStackMarker.h"
#include "MarkedBlockInlines.h"
#include "MarkedSpaceInlines.h"
#include "MarkingConstraintSet.h"
#include "SubspaceInlines.h"
#include "WeakMapImplInlines.h"
#include <wtf/Deque.h>
#include <wtf/OSAllocator.h>
#include <wtf/SetForScope.h>

#if OS(DARWIN)
#include <sys/mman.h>
#elif OS(LINUX)
#include <fcntl.h>
#include <unistd.h>
#endif

#if ENABLE(AOT)
#include "AOTClosure.h"
#include "AOTCompactTable.h"
#include "AOTEnvironment.h"
#include "AOTHeapHooks.h"
#include "AOTObject.h"
#include "AOTSlimClosure.h"
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

std::atomic<bool> HeapCompaction::s_active { false };
thread_local HeapCompaction::Pass* HeapCompaction::t_pass { nullptr };

static bool (*isThreadWantedDuringHeapCompaction)() = [] { return false; };

void setHeapCompactionInterruptCheck(bool (*isThreadWanted)())
{
    isThreadWantedDuringHeapCompaction = isThreadWanted;
}

#if ENABLE(AOT) && USE(BUN_JSC_ADDITIONS) && !ASAN_ENABLED

namespace HeapCompactionInternal {

static thread_local AbstractSlotVisitor* t_recorder;
static thread_local void (*t_record)(void* slot, bool isNarrow);
static thread_local bool t_willBeRecorded;

// Outside of the allocator, whose memory is scanned for pointers.
template<typename T>
class PageBackedArray {
    WTF_MAKE_NONCOPYABLE(PageBackedArray);
public:
    explicit PageBackedArray(size_t capacity)
        : m_bytes(roundUpToMultipleOf(pageSize(), std::max<size_t>(capacity, 1) * sizeof(T)))
        , m_start(static_cast<T*>(OSAllocator::tryReserveAndCommit(m_bytes)))
        , m_capacity(m_start ? capacity : 0)
    {
    }

    ~PageBackedArray()
    {
        if (m_start)
            OSAllocator::decommitAndRelease(m_start, m_bytes);
    }

    bool tryAppend(T value)
    {
        if (m_size == m_capacity)
            return false;
        m_start[m_size++] = value;
        return true;
    }

    size_t size() const { return m_size; }
    size_t capacity() const { return m_capacity; }
    void shrink(size_t size) { m_size = size; }
    T& operator[](size_t index) { return m_start[index]; }
    std::span<T> from(size_t first) { return { m_start + first, m_size - first }; }

private:
    size_t m_bytes;
    T* m_start;
    size_t m_capacity;
    size_t m_size { 0 };
};

// Leaves out the pages that were never used: most of what compiled code reserves for its tables.
class TouchedPages {
    WTF_MAKE_NONCOPYABLE(TouchedPages);
public:
    TouchedPages() = default;
#if OS(LINUX)
    ~TouchedPages()
    {
        if (m_pageMap >= 0)
            close(m_pageMap);
    }
#endif

    void forEachSpan(const void* begin, const void* end, NOESCAPE const auto& scan)
    {
        forEachSpan(static_cast<const uint8_t*>(begin), static_cast<const uint8_t*>(end), scan);
    }

    bool hasFailed() const { return m_hasFailed; }

private:
    void forEachSpan(const uint8_t* begin, const uint8_t* end, NOESCAPE const auto& scan)
    {
        if (static_cast<size_t>(end - begin) < 8 * pageSize()) {
            scan(begin, end);
            return;
        }
        auto* page = std::bit_cast<const uint8_t*>(std::bit_cast<uintptr_t>(begin) & ~(pageSize() - 1));
        while (page < end) {
            size_t count = std::min<size_t>(m_states.size(), (end - page + pageSize() - 1) / pageSize());
            if (!ask(page, count)) {
                m_hasFailed = true;
                return;
            }
            for (size_t i = 0; i < count; ++i, page += pageSize()) {
                if (isTouched(m_states[i]))
                    scan(std::max(page, begin), std::min(page + pageSize(), end));
            }
        }
    }

    bool m_hasFailed { false };
#if OS(DARWIN)
    // A page that was compressed counts as paged out.
    static bool isTouched(char state) { return state & (MINCORE_INCORE | MINCORE_PAGED_OUT); }
    bool ask(const uint8_t* page, size_t count) { return !mincore(page, count * pageSize(), m_states.data()); }
    std::array<char, 512> m_states;
#elif OS(LINUX)
    // Present or swapped out, which mincore() does not tell from never used, and not as read from a file.
    static bool isTouched(uint64_t state) { return state >> 62 && !(state >> 61 & 1); }
    bool ask(const uint8_t* page, size_t count)
    {
        size_t bytes = count * sizeof(uint64_t);
        return pread(m_pageMap, m_states.data(), bytes, std::bit_cast<uintptr_t>(page) / pageSize() * sizeof(uint64_t)) == static_cast<ssize_t>(bytes);
    }
    std::array<uint64_t, 512> m_states;
    int m_pageMap { open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC) };
#else
    static bool isTouched(char) { return true; }
    bool ask(const uint8_t*, size_t) { return false; }
    std::array<char, 512> m_states;
#endif
};

// The word that points at the elements of a short array, which lie in the array's own cell.
static uintptr_t* pointerIntoOwnCell(JSCell* cell, const void* cellWhenWritten, size_t cellSize)
{
    if (cell->type() != ArrayType)
        return nullptr;
    auto* word = std::bit_cast<uintptr_t*>(std::bit_cast<uint8_t*>(cell) + JSObjectWithButterfly::butterflyOffset());
    return *word - std::bit_cast<uintptr_t>(cellWhenWritten) - 1 < cellSize ? word : nullptr;
}

// What such a cell refers to may move: it holds addresses as plain words, in itself or in its auxiliary storage, and nothing else
// about it depends on them. (A CompactTable is indexed again.)
static bool canBeUpdated(JSCell* cell)
{
    switch (cell->type()) {
    case CompiledObjectType:
    case CompiledEnvironmentType:
    case CompiledClosureType:
    case SlimClosureType:
    case SlimClosureWithPropertiesType:
    case FinalObjectType:
    case ArrayType:
    case GetterSetterType:
    case JSCellButterflyType:
        return true;
    default:
        const ClassInfo* info = cell->classInfo();
        return info == JSBoundFunction::info() || info == AOT::CompactTable::info() || info == JSSet::info() || info == JSMap::info();
    }
}

// Every word that such a cell reads, but the one for its butterfly, is a JSValue: one that it does not report is unused room.
static bool reportsEveryWordInUse(JSCell* cell)
{
    return cell->type() == ArrayType || cell->type() == FinalObjectType || cell->type() == JSCellButterflyType;
}

template<typename Map>
struct WeakTable {
    using Bucket = typename Map::BucketType;
    template<typename T> T& at(ptrdiff_t offset) { return *std::bit_cast<T*>(std::bit_cast<uint8_t*>(map) + offset); }
    std::span<Bucket> buckets() { return { at<Bucket*>(Map::offsetOfBuffer()), at<uint32_t>(Map::offsetOfCapacity()) }; }

    void didMoveKeys()
    {
        Vector<std::array<uint8_t, sizeof(Bucket)>> entries;
        for (Bucket& bucket : buckets()) {
            if (!bucket.isEmpty() && !bucket.isDeleted()) {
                entries.grow(entries.size() + 1);
                memcpy(entries.last().data(), &bucket, sizeof(Bucket));
            }
        }
        zeroSpan(asWritableBytes(buckets()));
        size_t mask = buckets().size() - 1;
        for (auto& entry : entries) {
            size_t index = jsWeakMapHash(std::bit_cast<Bucket*>(entry.data())->key()) & mask;
            while (!buckets()[index].isEmpty())
                index = (index + 1) & mask;
            memcpy(static_cast<void*>(&buckets()[index]), entry.data(), sizeof(Bucket));
        }
        at<uint32_t>(Map::offsetOfDeleteCount()) = 0;
    }

    Map* map;
};

// Such a cell is its bytes: it has no destructor, and nothing in it but pointerIntoOwnCell() points into it.
static bool canBeMoved(JSCell* cell)
{
    switch (cell->type()) {
    case CompiledEnvironmentType:
    case SlimClosureType:
        return true;
    case CompiledObjectType:
    case SlimClosureWithPropertiesType:
    case FinalObjectType:
    case ArrayType:
        return !asObject(cell)->mayBePrototype();
    default:
        const ClassInfo* info = cell->classInfo();
        return info == AOT::CompactTable::info() || ((info == JSBoundFunction::info() || info == JSSet::info() || info == JSMap::info()) && !asObject(cell)->mayBePrototype());
    }
}

} // namespace HeapCompactionInternal

// Runs in the End phase of a full collection, on the mutator's thread, when the marks are exactly the live cells.
//
// A cell moves only if every reference to it is a word that this pass rewrites. What the collector knows of is found the way the
// collector finds it: every root and every live cell is visited, and a reference counts as rewritable if the visited cell is of
// a kind that canBeUpdated() and the address is found in that cell's memory as often as it was reported. What holds a cell weakly
// asks whether it is marked, which pins it. What is left, an untraced copy of a traced pointer, is looked for conservatively.
class HeapCompaction::Pass final : public AbstractSlotVisitor {
public:
    Pass(Heap& heap, const ForEachNativeSpan& forEachNativeSpan)
        : AbstractSlotVisitor(*heap.m_collector, "Compaction"_s, heap.m_collector->m_opaqueRoots)
        , m_heap(heap)
        , m_forEachNativeSpan(forEachNativeSpan)
    {
    }

    Heap& heap() const { return m_heap; }
    Result& result() { return m_result; }
    bool hasBegun() const { return !!m_blockStates; }

    void begin()
    {
        auto indices = m_heap.cage().usedIndices();
        m_cageBase = indices.base;
        m_start = (indices.base + (static_cast<uintptr_t>(indices.first) << MarkedBlockCage::shift)) & MarkedBlock::blockMask;
        m_size = (static_cast<uintptr_t>(indices.count) << MarkedBlockCage::shift) + 2 * MarkedBlock::blockSize;
        m_blockStates.emplace(m_size / MarkedBlock::blockSize + 1);
        if (!m_blockStates->capacity()) {
            m_blockStates.reset();
            return;
        }
        m_result.skipped = { };
        chooseCandidates();
    }

    void finish()
    {
        MonotonicTime start = MonotonicTime::now();
        size_t numberOfCells = m_heap.objectSpace().objectCount();
        m_slots.emplace(4 * numberOfCells);
        m_reported.emplace(1 << 20);
        m_found.emplace(1 << 20);
        m_tables.emplace(numberOfCells / 4);
        m_slotsOutsideCells.emplace(numberOfCells);
        SetForScope records(HeapCompactionInternal::t_recorder, static_cast<AbstractSlotVisitor*>(this));
        HeapCompactionInternal::t_record = [](void* slot, bool isNarrow) {
            auto* pass = static_cast<Pass*>(HeapCompactionInternal::t_recorder);
            isNarrow ? pass->recordNarrow(static_cast<uint32_t*>(slot)) : pass->record(static_cast<EncodedJSValue*>(slot));
        };
        auto phase = [&](ASCIILiteral name, auto&& work) {
            MonotonicTime before = MonotonicTime::now();
            size_t pinnedBefore = m_result.pinnedBlocks;
            if (!isInterrupted())
                work();
            dataLogLnIf(Options::logGC() || Options::verboseHeapCompaction(), "[compaction ", name, ": ", (MonotonicTime::now() - before).milliseconds(), " ms, pinned ", m_result.pinnedBlocks - pinnedBefore, "]");
        };
        dataLogLnIf(Options::logGC() || Options::verboseHeapCompaction(), "[compaction: ", m_result.candidateBlocks, " candidates, ", m_result.pinnedBlocks, " held weakly]");
        phase("stack"_s, [&] { pinFromStack(); });
        phase("roots"_s, [&] { pinFromRoots(); });
        phase("cells"_s, [&] { visitCells(); });
        std::ranges::sort(m_slotsOutsideCells->from(0));
        phase("tables"_s, [&] { pinFromTables(); });
        phase("native"_s, [&] { pinFromNativeMemory(); });
        if (isInterrupted())
            m_result.skipped = "interrupted"_s;
        else if (m_hasOverflowed || m_touchedPages.hasFailed())
            m_result.skipped = "not every reference was seen"_s;
        else {
            m_canBeInterrupted = false;
            phase("move"_s, [&] { moveCells(); });
            phase("update"_s, [&] { updateSlots(); });
            phase("empty"_s, [&] { emptyBlocks(); });
        }
        m_result.duration = MonotonicTime::now() - start;
        if (Options::verboseHeapCompaction()) [[unlikely]] {
            for (auto& [info, count] : m_pinsByHolder)
                dataLogLnIf(count > 4, "[pinned by ", info == Structure::info() ? "roots, or Structure"_s : info->className, info == AOT::Object::info() ? " (compiled)" : "", " ", count, "]");
            for (auto& [info, count] : m_immovables)
                dataLogLnIf(count > 4, "[cannot move ", info->className, " ", count, "]");
        }
    }

    ALWAYS_INLINE void pinCell(const void* cell)
    {
        if (isInCandidate(std::bit_cast<uintptr_t>(cell)))
            pinBlockOf(std::bit_cast<uintptr_t>(cell));
    }

    void pinCellIfMarked(const void* cell)
    {
        if (isInCandidate(std::bit_cast<uintptr_t>(cell)) && MarkedBlock::blockFor(cell)->isMarkedRaw(cell))
            pinBlockOf(std::bit_cast<uintptr_t>(cell));
    }

    void record(EncodedJSValue* slot)
    {
        if (isInCandidate(*slot))
            recordSlotOutsideCells(std::bit_cast<uintptr_t>(slot));
    }

    void recordNarrow(uint32_t* slot)
    {
        if (isInCandidate(addressOf(*slot)))
            recordSlotOutsideCells(std::bit_cast<uintptr_t>(slot) | narrowSlot);
    }

    bool isInterrupted()
    {
        m_isInterrupted = m_canBeInterrupted && (m_isInterrupted || isThreadWantedDuringHeapCompaction());
        return m_isInterrupted;
    }

private:
    bool isInterruptedAfter(size_t bytes)
    {
        m_bytesSinceInterruptCheck += bytes;
        if (m_bytesSinceInterruptCheck < 256 * KB)
            return m_isInterrupted;
        m_bytesSinceInterruptCheck = 0;
        return isInterrupted();
    }

    void recordSlotOutsideCells(uintptr_t slot)
    {
        m_hasOverflowed |= !m_slots->tryAppend(slot) || !m_slotsOutsideCells->tryAppend(slot);
    }

    bool isRecorded(uintptr_t slot) { return std::ranges::binary_search(m_slotsOutsideCells->from(0), slot); }

    enum class BlockState : uint8_t { Stays, Candidate, Emptied };
    static constexpr uintptr_t narrowSlot = 1;
    static constexpr uintptr_t addressMask = (1ull << OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH)) - 1;

    ALWAYS_INLINE BlockState& stateOf(uintptr_t address) { return (*m_blockStates)[(address - m_start) / MarkedBlock::blockSize]; }
    ALWAYS_INLINE bool isIn(uintptr_t address, BlockState state) { return address - m_start < m_size && stateOf(address) == state; }
    ALWAYS_INLINE bool isInCandidate(uintptr_t address) { return isIn(address, BlockState::Candidate); }

    NEVER_INLINE void pinBlockOf(uintptr_t address)
    {
        stateOf(address) = BlockState::Stays;
        m_result.pinnedBlocks++;
        if (Options::verboseHeapCompaction()) [[unlikely]]
            m_pinsByHolder.add(m_holder ? m_holder->classInfo() : Structure::info(), 0).iterator->value++;
    }

    void pinIfInLiveCell(uintptr_t address)
    {
        MarkedBlock* block = MarkedBlock::blockFor(std::bit_cast<void*>(address));
        MarkedBlock::Handle& handle = block->handle();
        if (handle.contains(std::bit_cast<void*>(address)) && block->isMarkedRaw(handle.cellAlign(std::bit_cast<void*>(address))))
            pinBlockOf(address);
    }

    ALWAYS_INLINE bool mayPointAtCandidate(uintptr_t address) { return isInCandidate(address) || (!(address & ~MarkedBlock::blockMask) && isInCandidate(address - 1)); }

    void pinIfInOrJustPastLiveCell(uintptr_t address)
    {
        for (uintptr_t inCell : { address, address - 1 }) {
            if (isInCandidate(inCell))
                pinIfInLiveCell(inCell);
        }
    }

    void pinFromNarrowReferences(const void* begin, const void* end)
    {
        pinFromNarrowReferences(begin, end, [](const uint32_t*) { return false; });
    }

    void pinFromNarrowReferences(const void* begin, const void* end, NOESCAPE const auto& isIgnored)
    {
        for (auto* word = std::bit_cast<const uint32_t*>(roundUpToMultipleOf<sizeof(uint32_t)>(std::bit_cast<uintptr_t>(begin))); word + 1 <= std::bit_cast<const uint32_t*>(end); ++word) {
            if (uintptr_t address = addressOf(*word); isInCandidate(address) && !isIgnored(word)) [[unlikely]]
                pinIfInLiveCell(address);
        }
    }

    // Tags are in the bits above the address, or below the alignment of a cell.
    void pinFromSpan(const void* begin, const void* end)
    {
        pinFromSpan(begin, end, [](const uintptr_t*) { return false; });
    }

    void pinFromSpan(const void* begin, const void* end, NOESCAPE const auto& isIgnored)
    {
        auto* word = std::bit_cast<const uintptr_t*>(roundUpToMultipleOf<sizeof(uintptr_t)>(std::bit_cast<uintptr_t>(begin)));
        for (; word + 1 <= std::bit_cast<const uintptr_t*>(end); ++word) {
            uintptr_t address = *word & addressMask;
            if (mayPointAtCandidate(address) && !isIgnored(word)) [[unlikely]]
                pinIfInOrJustPastLiveCell(address);
        }
    }

    bool holdsMovableCells(BlockDirectory& directory)
    {
        return directory.attributes().cellKind == HeapCell::JSCell && directory.attributes().destruction == DoesNotNeedDestruction;
    }

    // A weak handle names a live cell, or has a finalizer that is still to see a dead one.
    static bool hasWeakHandles(MarkedBlock::Handle& handle)
    {
        bool result = false;
        handle.weakSet().forEachBlock([&](WeakBlock& block) {
            result |= !block.isEmpty() && !block.hasOnlyFinalizedHandles();
        });
        return result;
    }

    void chooseCandidates()
    {
        m_heap.objectSpace().forEachBlock([&](MarkedBlock::Handle* handle) {
            uintptr_t address = std::bit_cast<uintptr_t>(&handle->block());
            if (address - m_start >= m_size || !holdsMovableCells(*handle->directory()) || handle->areMarksStale() || hasWeakHandles(*handle))
                return;
            size_t count = handle->markCount();
            if (!count || count > handle->cellsPerBlock() * Options::maximumOccupancyForHeapCompaction())
                return;
            bool canBeEmptied = true;
            handle->forEachMarkedCell([&](size_t, HeapCell* cell, HeapCell::Kind) {
                canBeEmptied = HeapCompactionInternal::canBeMoved(static_cast<JSCell*>(cell));
                if (!canBeEmptied && Options::verboseHeapCompaction()) [[unlikely]]
                    m_immovables.add(static_cast<JSCell*>(cell)->classInfo(), 0).iterator->value++;
                return canBeEmptied ? IterationStatus::Continue : IterationStatus::Done;
            });
            if (!canBeEmptied)
                return;
            stateOf(address) = BlockState::Candidate;
            m_result.candidateBlocks++;
        });
    }

    void pinFromStack()
    {
        auto scan = [&](void* begin, void* end) {
            if (begin > end)
                std::swap(begin, end);
            pinFromSpan(begin, end);
            pinFromNarrowReferences(begin, end);
        };
        auto scanThreadState = [&](CurrentThreadState& state) {
            scan(state.registerState, state.registerState + 1);
            scan(state.stackTop, state.stackOrigin);
        };
        callWithCurrentThreadState(scanThreadState);
#if ENABLE(DFG_JIT)
        m_heap.vm().forEachConservativelyScannedBuffer(scan);
#endif
    }

    void pinFromRoots()
    {
        SetForScope visitsEverything(m_collector.m_isMarkingForGCVerifier, true);
        m_collector.m_constraintSet->executeAllSynchronously(*this);
        while (!m_tasks.isEmpty())
            m_tasks.takeFirst()->run(*this);
        for (const JSCell* cell : *m_heap.m_mutatorMarkStack)
            pinCell(cell);
    }

    void pinFromNativeMemory()
    {
        m_collector.forEachSlotVisitor([](SlotVisitor& visitor) {
            visitor.collectorMarkStack().zeroUnusedCapacity();
            visitor.mutatorMarkStack().zeroUnusedCapacity();
        });
        for (MarkStackArray* stack : { m_collector.m_sharedCollectorMarkStack.get(), m_collector.m_sharedMutatorMarkStack.get(), m_collector.m_raceMarkStack.get(), m_heap.m_mutatorMarkStack.get() })
            stack->zeroUnusedCapacity();
        // Some of the heap comes from the allocator, and has been visited.
        Vector<std::pair<uintptr_t, size_t>> preciseCells;
        for (PreciseAllocation* allocation : m_heap.objectSpace().preciseAllocations())
            preciseCells.append({ std::bit_cast<uintptr_t>(allocation->cell()), allocation->cellSize() });
        std::ranges::sort(preciseCells);
        auto isAccountedFor = [&](const uintptr_t* word) {
            if (isRecorded(std::bit_cast<uintptr_t>(word)))
                return true;
            auto next = std::ranges::upper_bound(preciseCells, std::bit_cast<uintptr_t>(word), { }, &std::pair<uintptr_t, size_t>::first);
            return next != preciseCells.begin() && std::bit_cast<uintptr_t>(word) - next[-1].first < next[-1].second;
        };
        auto scan = [&](const void* begin, const void* end) {
            if (m_heap.cage().contains(begin) || std::bit_cast<uintptr_t>(begin) - g_jscConfig.startOfStructureHeap < g_jscConfig.sizeOfStructureHeap)
                return;
            m_touchedPages.forEachSpan(begin, end, [&](const uint8_t* begin, const uint8_t* end) {
                while (begin < end && !isInterruptedAfter(std::min<size_t>(end - begin, MarkedBlock::blockSize))) {
                    MarkedBlock* block = MarkedBlock::blockFor(begin);
                    const uint8_t* next = std::min(end, std::bit_cast<const uint8_t*>(block) + MarkedBlock::blockSize);
                    if (!m_heap.objectSpace().blocks().set().contains(block))
                        pinFromSpan(begin, next, isAccountedFor);
                    begin = next;
                }
            });
        };
        m_forEachNativeSpan(scan);
    }

    void pinFromTables()
    {
        AOT::HeapHooks::forEachTableSpan(m_heap.vm(), [&](const void* begin, const void* end) {
            m_touchedPages.forEachSpan(begin, end, [&](const uint8_t* begin, const uint8_t* end) {
                if (isInterruptedAfter(end - begin))
                    return;
                pinFromSpan(begin, end, [&](const uintptr_t* word) { return isRecorded(std::bit_cast<uintptr_t>(word)); });
                pinFromNarrowReferences(begin, end, [&](const uint32_t* word) { return isRecorded(std::bit_cast<uintptr_t>(word) | narrowSlot); });
            });
        });
    }

    void visitCells()
    {
        SetForScope visitsEverything(m_collector.m_isMarkingForGCVerifier, true);
        m_heap.objectSpace().forEachBlock([&](MarkedBlock::Handle* handle) {
            if (!isJSCellKind(handle->cellKind()) || isInterruptedAfter(MarkedBlock::blockSize))
                return;
            size_t cellSize = handle->cellSize();
            handle->forEachMarkedCell([&](size_t, HeapCell* cell, HeapCell::Kind) {
                visitCell(static_cast<JSCell*>(cell), cellSize);
                return IterationStatus::Continue;
            });
        });
        for (PreciseAllocation* allocation : m_heap.objectSpace().preciseAllocations()) {
            if (isJSCellKind(allocation->attributes().cellKind) && allocation->isMarked() && !isInterruptedAfter(allocation->cellSize()))
                visitCell(static_cast<JSCell*>(allocation->cell()), allocation->cellSize());
        }
    }

    uintptr_t addressOf(uint32_t narrow) const { return m_cageBase + (static_cast<uintptr_t>(narrow) << MarkedBlockCage::shift); }

    struct Found {
        uintptr_t address;
        uintptr_t slot;
    };

    void visitCell(JSCell* cell, size_t cellSize)
    {
        if (cell->type() == StringType) {
            if (!asString(cell)->isRope())
                return;
        }
        SetForScope holder(m_holder, cell);
        auto* bytes = std::bit_cast<uint8_t*>(cell);
        if (cell->type() == JSWeakMapType)
            return visitWeakTable(uncheckedDowncast<JSWeakMap>(cell), m_weakMaps);
        if (cell->type() == JSWeakSetType)
            return visitWeakTable(uncheckedDowncast<JSWeakSet>(cell), m_weakSets);
        if (auto* map = dynamicDowncast<JSMap>(cell))
            pinFromOrdinaryStorage(map->storage());
        else if (auto* set = dynamicDowncast<JSSet>(cell))
            pinFromOrdinaryStorage(set->storage());
        if (!HeapCompactionInternal::canBeUpdated(cell)) {
            cell->methodTable()->visitChildren(cell, *this);
            pinFromSpan(bytes, bytes + cellSize);
            if (cell->subspace() != &m_heap.stringSpace && !cell->inherits<Structure>())
                pinFromNarrowReferences(bytes + sizeof(JSCell), bytes + cellSize);
            return;
        }

        m_reported->shrink(0);
        m_found->shrink(0);
        m_storage.shrink(0);
        if (cell->type() == SlimClosureType) {
            auto captures = static_cast<AOT::SlimClosure*>(cell)->captures();
            m_storage.append({ bytes + AOT::SlimClosure::offsetOfCaptures(), captures.size_bytes() });
        } else
            m_storage.append({ bytes + sizeof(JSCell), cellSize - sizeof(JSCell) });
        {
            SetForScope collects(m_collects, true);
            cell->methodTable()->visitChildren(cell, *this);
        }
        uintptr_t* movesWithCell = HeapCompactionInternal::pointerIntoOwnCell(cell, cell, cellSize);
        for (auto storage : m_storage) {
            for (auto& word : spanReinterpretCast<uintptr_t>(storage)) {
                if (mayPointAtCandidate(word & addressMask) && &word != movesWithCell) [[unlikely]]
                    m_hasOverflowed |= !m_found->tryAppend({ word, std::bit_cast<uintptr_t>(&word) });
            }
        }
        if (!m_reported->size() && !m_found->size())
            return;
        // Also when every reference was found: what was found can be a leftover of the cell that was here before.
        if (m_reported->size() && !HeapCompactionInternal::reportsEveryWordInUse(cell)) {
            for (auto storage : m_storage) {
                for (auto& word : spanReinterpretCast<uint32_t>(storage)) {
                    if (uintptr_t address = addressOf(word); isInCandidate(address)) [[unlikely]]
                        m_hasOverflowed |= !m_found->tryAppend({ address, std::bit_cast<uintptr_t>(&word) | narrowSlot });
                }
            }
        }

        size_t firstSlot = m_slots->size();
        if (!tryMatchInOrder())
            match();
        if (m_slots->size() != firstSlot && cell->classInfo() == AOT::CompactTable::info())
            m_hasOverflowed |= !m_tables->tryAppend(uncheckedDowncast<AOT::CompactTable>(cell));
    }

    // Its keys are hashed by address.
    void pinFromOrdinaryStorage(JSCell* storage)
    {
        if (storage && storage->type() == JSCellButterflyType)
            storage->methodTable()->visitChildren(storage, *this);
    }

    template<typename Map>
    void visitWeakTable(Map* map, Vector<Map*>& maps)
    {
        map->methodTable()->visitChildren(map, *this);
        size_t firstSlot = m_slots->size();
        for (auto& bucket : HeapCompactionInternal::WeakTable<Map> { map }.buckets()) {
            if (bucket.isEmpty() || bucket.isDeleted())
                continue;
            for (auto& word : spanReinterpretCast<EncodedJSValue>(asWritableBytes(singleElementSpan(bucket))))
                record(&word);
        }
        if (m_slots->size() != firstSlot)
            maps.append(map);
    }

    bool tryMatchInOrder()
    {
        auto reported = m_reported->from(0);
        auto found = m_found->from(0);
        if (reported.size() != found.size())
            return false;
        for (size_t i = 0; i < found.size(); ++i) {
            if (reported[i] != found[i].address)
                return false;
        }
        for (auto& it : found)
            m_hasOverflowed |= !m_slots->tryAppend(it.slot);
        return true;
    }

    void match()
    {
        auto reported = m_reported->from(0);
        std::ranges::sort(reported);
        std::ranges::sort(m_found->from(0), { }, &Found::address);
        auto countOf = [](auto range) { return static_cast<size_t>(range.end() - range.begin()); };

        auto found = m_found->from(0);
        for (uintptr_t address : reported) {
            auto slots = std::ranges::equal_range(found, address, { }, &Found::address);
            size_t numberReported = countOf(std::ranges::equal_range(reported, address));
            // A number can look like a narrow reference.
            bool mayBeNumber = countOf(slots) > numberReported && std::ranges::any_of(slots, [](auto& it) { return it.slot & narrowSlot; });
            if (countOf(slots) < numberReported || mayBeNumber)
                pinCell(std::bit_cast<void*>(address));
        }
        bool ignoresUnreported = HeapCompactionInternal::reportsEveryWordInUse(m_holder);
        for (auto& it : found) {
            if (std::ranges::binary_search(reported, it.address))
                m_hasOverflowed |= !m_slots->tryAppend(it.slot);
            else if (!(it.slot & narrowSlot) && (!ignoresUnreported || it.slot == std::bit_cast<uintptr_t>(m_holder) + sizeof(JSCell)))
                pinIfInOrJustPastLiveCell(it.address & addressMask);
        }
    }

    void moveCells()
    {
        struct Block {
            MarkedBlock::Handle* handle;
            unsigned live;
            bool isCandidate;
        };
        Vector<Block> blocks;
        for (BlockDirectory* directory = m_heap.objectSpace().firstDirectory(); directory; directory = directory->nextDirectory()) {
            if (!holdsMovableCells(*directory))
                continue;
            blocks.shrink(0);
            directory->forEachBlock([&](MarkedBlock::Handle* handle) {
                uintptr_t address = std::bit_cast<uintptr_t>(&handle->block());
                if (address - m_start >= m_size || handle->areMarksStale() || hasWeakHandles(*handle))
                    return;
                unsigned live = handle->markCount();
                if (live && live < handle->cellsPerBlock())
                    blocks.append({ handle, live, isInCandidate(address) });
            });
            // What stays is filled first, then the densest of the rest. The sparsest are emptied.
            std::ranges::sort(blocks, { }, [](const Block& block) {
                return std::pair { block.isCandidate, -static_cast<int>(block.live) };
            });

            size_t firstSource = blocks.size();
            size_t room = 0;
            for (auto& block : blocks)
                room += block.handle->cellsPerBlock() - block.live;
            while (firstSource && blocks[firstSource - 1].isCandidate) {
                Block& source = blocks[firstSource - 1];
                size_t roomInSource = source.handle->cellsPerBlock() - source.live;
                if (room < roomInSource + source.live)
                    break;
                room -= roomInSource + source.live;
                firstSource--;
            }

            size_t cellSize = directory->cellSize();
            size_t target = 0;
            uint8_t* hole = nullptr;
            auto nextHole = [&] {
                for (;; target++, hole = nullptr) {
                    RELEASE_ASSERT(target < firstSource);
                    MarkedBlock::Handle& handle = *blocks[target].handle;
                    if (!hole) {
                        handle.recommitPages();
                        hole = static_cast<uint8_t*>(handle.start());
                    }
                    for (; hole + cellSize <= handle.end(); hole += cellSize) {
                        if (!handle.block().isMarkedRaw(hole))
                            return hole;
                    }
                }
            };
            for (size_t i = firstSource; i < blocks.size(); ++i) {
                blocks[i].handle->forEachMarkedCell([&](size_t, HeapCell* cell, HeapCell::Kind) {
                    uint8_t* copy = nextHole();
                    memcpy(copy, cell, cellSize);
                    if (uintptr_t* word = HeapCompactionInternal::pointerIntoOwnCell(std::bit_cast<JSCell*>(copy), cell, cellSize))
                        *word += copy - std::bit_cast<uint8_t*>(cell);
                    MarkedBlock::blockFor(copy)->didMoveCellIn(copy);
                    *std::bit_cast<uint8_t**>(cell) = copy;
                    m_result.movedCells++;
                    return IterationStatus::Continue;
                });
                stateOf(std::bit_cast<uintptr_t>(&blocks[i].handle->block())) = BlockState::Emptied;
                m_emptied.append(blocks[i].handle);
            }
        }
        m_result.emptiedBlocks = m_emptied.size();
    }

    template<typename T>
    ALWAYS_INLINE T* currentAddressOf(T* address)
    {
        if (!isIn(std::bit_cast<uintptr_t>(address), BlockState::Emptied))
            return address;
        auto* cell = static_cast<uint8_t*>(MarkedBlock::blockFor(address)->handle().cellAlign(address));
        return std::bit_cast<T*>(*std::bit_cast<uint8_t**>(cell) + (std::bit_cast<uint8_t*>(address) - cell));
    }

    void updateSlots()
    {
        for (uintptr_t slot : m_slots->from(0)) {
            if (slot & narrowSlot) {
                uint32_t* word = currentAddressOf(std::bit_cast<uint32_t*>(slot - narrowSlot));
                if (uintptr_t address = addressOf(*word); isIn(address, BlockState::Emptied))
                    *word = MarkedBlockCage::encode(m_cageBase, *std::bit_cast<void**>(address));
                continue;
            }
            uintptr_t* word = currentAddressOf(std::bit_cast<uintptr_t*>(slot));
            if (isIn(*word, BlockState::Emptied))
                *word = *std::bit_cast<uintptr_t*>(*word);
        }
        for (AOT::CompactTable* table : m_tables->from(0))
            currentAddressOf(table)->didMoveKeys();
        for (JSWeakMap* map : m_weakMaps)
            HeapCompactionInternal::WeakTable<JSWeakMap> { map }.didMoveKeys();
        for (JSWeakSet* set : m_weakSets)
            HeapCompactionInternal::WeakTable<JSWeakSet> { set }.didMoveKeys();
    }

    void emptyBlocks()
    {
        for (MarkedBlock::Handle* handle : m_emptied) {
            if (Options::verboseHeapCompaction()) [[unlikely]]
                std::ranges::fill(std::span { static_cast<uint64_t*>(handle->start()), static_cast<uint64_t*>(handle->end()) }, 0xbadbeef0);
            handle->block().didMoveEveryCellOut();
        }
    }

    void append(const ConservativeRoots& roots) final
    {
        for (HeapCell* cell : std::span { roots.roots(), roots.size() })
            pinCell(cell);
    }

    void appendUnbarriered(JSCell* cell) final
    {
        uintptr_t address = std::bit_cast<uintptr_t>(cell);
        if (!isInCandidate(address))
            return;
        if (m_collects)
            m_hasOverflowed |= !m_reported->tryAppend(address);
        else
            pinBlockOf(address);
    }

    void appendHiddenUnbarriered(JSCell* cell) final { appendUnbarriered(cell); }

    void markAuxiliary(const void* base) final
    {
        auto* cell = std::bit_cast<HeapCell*>(base);
        auto* start = static_cast<uint8_t*>(cell->isPreciseAllocation() ? static_cast<void*>(cell) : cell->markedBlock().handle().cellAlign(cell));
        if (cell->subspace() == &m_heap.primitiveGigacageAuxiliarySpace)
            return;
        if (m_collects)
            m_storage.append({ start, cell->cellSize() });
        else {
            pinFromSpan(start, start + cell->cellSize());
            pinFromNarrowReferences(start, start + cell->cellSize());
        }
    }

    bool isMarked(const void* cell) const final
    {
        const_cast<Pass*>(this)->pinCellIfMarked(cell);
        return Heap::isMarked(m_heap.objectSpace().markingVersion(), cell);
    }
    bool isMarked(MarkedBlock&, HeapCell* cell) const final { return isMarked(cell); }
    bool isMarked(PreciseAllocation& allocation, HeapCell*) const final { return allocation.isMarked(); }

    void visitAsConstraint(const JSCell* cell) final
    {
        SetForScope pins(m_collects, false);
        cell->methodTable()->visitChildren(const_cast<JSCell*>(cell), *this);
    }

    void addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>> task) final { m_tasks.append(WTF::move(task)); }
    NO_RETURN_DUE_TO_CRASH void addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>>) final { RELEASE_ASSERT_NOT_REACHED(); }
    bool isFirstVisit() const final { return false; }
    void reportExtraMemoryVisited(size_t) final { }
#if ENABLE(RESOURCE_USAGE)
    void reportExternalMemoryVisited(size_t) final { }
#endif
    bool mutatorsAreStopped() const final { return true; }
    void didRace(const VisitRaceKey&) final { m_hasOverflowed = true; }

    Heap& m_heap;
    const ForEachNativeSpan& m_forEachNativeSpan;
    Result m_result { "did not begin"_s };
    uintptr_t m_cageBase { 0 };
    uintptr_t m_start { 0 };
    size_t m_size { 0 };
    bool m_collects { false };
    bool m_hasOverflowed { false };
    bool m_canBeInterrupted { true };
    bool m_isInterrupted { false };
    size_t m_bytesSinceInterruptCheck { 0 };
    JSCell* m_holder { nullptr };
    std::optional<HeapCompactionInternal::PageBackedArray<BlockState>> m_blockStates;
    std::optional<HeapCompactionInternal::PageBackedArray<uintptr_t>> m_slots;
    std::optional<HeapCompactionInternal::PageBackedArray<uintptr_t>> m_reported;
    std::optional<HeapCompactionInternal::PageBackedArray<Found>> m_found;
    std::optional<HeapCompactionInternal::PageBackedArray<AOT::CompactTable*>> m_tables;
    std::optional<HeapCompactionInternal::PageBackedArray<uintptr_t>> m_slotsOutsideCells;
    Vector<JSWeakMap*> m_weakMaps;
    Vector<JSWeakSet*> m_weakSets;
    HeapCompactionInternal::TouchedPages m_touchedPages;
    Vector<std::span<uint8_t>, 4> m_storage;
    Vector<MarkedBlock::Handle*> m_emptied;
    Deque<RefPtr<SharedTask<void(AbstractSlotVisitor&)>>> m_tasks;
    UncheckedKeyHashMap<const ClassInfo*, unsigned> m_pinsByHolder;
    UncheckedKeyHashMap<const ClassInfo*, unsigned> m_immovables;
};

HeapCompaction::Result HeapCompaction::collectAndCompact(Heap& heap, NOESCAPE const ForEachNativeSpan& forEachNativeSpan)
{
    if (!Options::useHeapCompaction() || !Options::runsCompiledImage() || Options::useJIT() || !MarkedBlockCage::isRequired())
        return { "not for this program"_s };
    if (ASCIILiteral reason = heap.reasonNotToEvacuateAuxiliaryBlocksNow()) {
        dataLogLnIf(Options::verboseHeapCompaction(), "[compaction: ", reason, "]");
        return { reason };
    }
    VM& vm = heap.vm();
    if (vm.entryScope || vm.heapProfiler() || heap.m_pinballCompletionSpace || heap.m_verifier)
        return { "something remembers addresses"_s };
    if (s_active.exchange(true))
        return { "another heap is being compacted"_s };

    Pass pass(heap, forEachNativeSpan);
    t_pass = &pass;
    // This thread keeps the conn, so it ends the collection, and is free to leave while the helpers mark.
    heap.collectAsync(CollectionScope::Full);
    for (heap.stopIfNecessary(); heap.m_collector->hasPendingGCRequest(); heap.stopIfNecessary()) {
        if (pass.isInterrupted()) {
            pass.result().skipped = "interrupted while marking"_s;
            break;
        }
        sleep(500_us);
    }
    t_pass = nullptr;
    s_active = false;
    Result& result = pass.result();
    result.didCollect = true;
    dataLogLnIf(Options::logGC() || Options::verboseHeapCompaction(), "[compaction: ", result.skipped ? result.skipped : "done"_s, ", emptied ", result.emptiedBlocks, " of ", result.candidateBlocks, " blocks, ", result.pinnedBlocks, " pinned, ", result.movedCells, " cells, ", result.duration.milliseconds(), " ms]");
    return result;
}

size_t HeapCompaction::bytesInHoles(Heap& heap)
{
    size_t capacity = heap.objectSpace().capacity();
    return capacity - std::min(capacity, heap.objectSpace().size());
}

// References are pruned on the thread that conducts.
void HeapCompaction::didAskWhetherMarked(const void* cell)
{
    if (t_pass && t_pass->hasBegun() && !HeapCompactionInternal::t_willBeRecorded)
        t_pass->pinCellIfMarked(cell);
}

void HeapCompaction::willPruneDeadReferences(Collector& collector, Heap& heap, bool mutatorConducts)
{
    if (t_pass && &t_pass->heap() == &heap && mutatorConducts && collector.m_heaps.size() == 1 && heap.collectionScope() == CollectionScope::Full && !t_pass->hasBegun() && !t_pass->isInterrupted())
        t_pass->begin();
}

void HeapCompaction::didPruneDeadReferences(Collector&, Heap& heap)
{
    if (t_pass && &t_pass->heap() == &heap && t_pass->hasBegun())
        std::exchange(t_pass, nullptr)->finish();
}

bool HeapCompactionSlots::areRecordedBy(const AbstractSlotVisitor& visitor) { return &visitor == HeapCompactionInternal::t_recorder; }
void HeapCompactionSlots::record(AbstractSlotVisitor&, EncodedJSValue* slot) { HeapCompactionInternal::t_record(slot, false); }
void HeapCompactionSlots::recordNarrow(AbstractSlotVisitor&, uint32_t* slot) { HeapCompactionInternal::t_record(slot, true); }
HeapCompactionSlots::WillBeRecorded::WillBeRecorded() { HeapCompactionInternal::t_willBeRecorded = true; }
HeapCompactionSlots::WillBeRecorded::~WillBeRecorded() { HeapCompactionInternal::t_willBeRecorded = false; }

#else

bool HeapCompactionSlots::areRecordedBy(const AbstractSlotVisitor&) { return false; }
void HeapCompactionSlots::record(AbstractSlotVisitor&, EncodedJSValue*) { }
void HeapCompactionSlots::recordNarrow(AbstractSlotVisitor&, uint32_t*) { }
HeapCompactionSlots::WillBeRecorded::WillBeRecorded() = default;
HeapCompactionSlots::WillBeRecorded::~WillBeRecorded() = default;

HeapCompaction::Result HeapCompaction::collectAndCompact(Heap&, NOESCAPE const ForEachNativeSpan&) { return { "not in this build"_s }; }
size_t HeapCompaction::bytesInHoles(Heap&) { return 0; }
void HeapCompaction::didAskWhetherMarked(const void*) { }
void HeapCompaction::willPruneDeadReferences(Collector&, Heap&, bool) { }
void HeapCompaction::didPruneDeadReferences(Collector&, Heap&) { }

#endif

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
