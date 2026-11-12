/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachine.h"
#include <algorithm>
#include <bit>

namespace JSC { namespace AOT { namespace Machine {

struct NumberingKey {
    Opcode opcode;
    Type type;
    Operation* children[Operation::maxInlineChildren] { nullptr, nullptr, nullptr };
    uint64_t payload { 0 };
};

struct NumberingKeyOfPurePatch {
    Type type;
    uint64_t id;
    std::span<Operation* const> operands { };
};

struct ScopeMark {
    uint32_t logSize { 0 };
    uint32_t floor { 0 };
    uint32_t numberOfSuspendedBlocks { 0 };
    uint32_t endOfInheritedEntries { 0 };
    const Operation* newestOperation { nullptr };
};

class Numbering {
    WTF_MAKE_NONCOPYABLE(Numbering);
public:
    JS_EXPORT_PRIVATE explicit Numbering(Procedure&);

    static uint32_t hashWithoutPosition(Opcode opcode, uint64_t payload, uint32_t base = UINT32_MAX)
    {
        return combine(payload, static_cast<uint64_t>(opcode) | static_cast<uint64_t>(base) << 32);
    }
    ALWAYS_INLINE Operation* findWithoutPosition(Opcode opcode, uint64_t payload, uint32_t hash, uint32_t base = UINT32_MAX) const
    {
        for (uint32_t link = m_bucketsWithoutPosition[hash & (m_bucketsWithoutPosition.size() - 1)]; link; link = m_withoutPosition[link - 1].previousInBucket) {
            const Entry& entry = m_withoutPosition[link - 1];
            ASSERT(entry.previousInBucket < link);
            if (entry.hash == hash && entry.payload == payload && entry.operation->m_opcode == opcode && baseOf(entry.operation) == base)
                return entry.operation;
        }
        return nullptr;
    }
    bool hasRoomWithoutPosition() const { return m_withoutPosition.size() < m_bucketsWithoutPosition.size(); }
    ALWAYS_INLINE void addWithoutPositionWhereThereIsRoom(Operation* operation, uint64_t payload, uint32_t hash)
    {
        RELEASE_ASSERT(operation->hasNoPosition());
        ASSERT(hasNoPosition(operation->opcode()) || operation->opcode() == ArgumentReg || operation->opcode() == Add);
        ASSERT(hash == hashWithoutPosition(operation->opcode(), payload, baseOf(operation)));
        ASSERT(!findWithoutPosition(operation->opcode(), payload, hash, baseOf(operation)));
        ASSERT(hasRoomWithoutPosition());

        uint32_t& bucket = m_bucketsWithoutPosition[hash & (m_bucketsWithoutPosition.size() - 1)];
        m_withoutPosition.appendWithinCapacity(Entry { operation, hash, bucket, m_clock, payload });
        bucket = m_withoutPosition.size();
    }
    ALWAYS_INLINE void addWithoutPosition(Operation* operation, uint64_t payload, uint32_t hash)
    {
        if (!hasRoomWithoutPosition()) [[unlikely]]
            growBucketsWithoutPosition();
        addWithoutPositionWhereThereIsRoom(operation, payload, hash);
    }

    enum class Reach : bool { WholeScope, OwnEntries };
    struct Found {
        Operation* operation { nullptr };
        bool isInherited { false };
    };
    ALWAYS_INLINE static uint32_t hashOf(const NumberingKey& key)
    {
        uint64_t head = static_cast<uint64_t>(key.opcode) | static_cast<uint64_t>(key.type.kind()) << 8 | static_cast<uint64_t>(key.children[0]->index()) << 32;
        uint64_t rest = key.payload;
        if (key.children[1])
            rest ^= key.children[1]->index();
        if (key.children[2])
            rest ^= static_cast<uint64_t>(key.children[2]->index()) << 32;
        return combine(head, rest);
    }
    ALWAYS_INLINE Found lookUp(const NumberingKey& key, uint32_t hash, Reach reach = Reach::WholeScope) const
    {
        ASSERT(!isLoad(key.opcode));
        return lookUpOperation<false>(key, hash, reach);
    }
    ALWAYS_INLINE Found lookUpLoad(const NumberingKey& key, uint32_t hash, Reach reach = Reach::WholeScope) const
    {
        ASSERT(isLoad(key.opcode));
        return lookUpOperation<true>(key, hash, reach);
    }
    JS_EXPORT_PRIVATE Operation* findPurePatch(const NumberingKeyOfPurePatch&) const;
    JS_EXPORT_PRIVATE bool isStillValid(const NumberingKey&, const Operation* load) const;
    ALWAYS_INLINE void add(const NumberingKey& key, uint32_t hash, Operation* operation)
    {
        RELEASE_ASSERT(operation->m_owner && !operation->traps() && countsChildrenAtFirstUse(operation->m_opcode));
        if (m_savesFrameOnSuspend && computationReadsAddressInFrame(operation)) [[unlikely]]
            return;
        appendToLog(operation, hash, key.payload);
        ASSERT(matches(key, m_log.last()));
    }
    ALWAYS_INLINE void addLoad(const NumberingKey& key, uint32_t hash, Operation* load)
    {
        ASSERT(isLoad(key.opcode));
        RELEASE_ASSERT(mayHold(load));
        if (!load->has(OperationFlag::ReadsImmutableHeap))
            ++m_numberOfEntriesThatWritesEnd;
        appendToLog(load, hash, key.payload);
        ASSERT(matches(key, m_log.last()));
    }
    JS_EXPORT_PRIVATE void addPurePatch(const NumberingKeyOfPurePatch&, PatchOperation*);
    JS_EXPORT_PRIVATE void addInPlaceOfPurePatch(const NumberingKeyOfPurePatch&, Operation* phi);
    JS_EXPORT_PRIVATE void addInPlaceOfReadOfMemory(const NumberingKeyOfPurePatch&, Operation* phi, HeapIndex);

    void didWrite(HeapIndex heap)
    {
        if (m_numberOfEntriesThatWritesEnd == m_numberOfEntriesEndedByWriteToAllHeaps)
            return;
        uint32_t time = timeOfNewWrite();
        if (m_timesOfWrites.empty()) [[unlikely]]
            makeTimesOfWrites();
        m_timesOfWrites[heap].toHeap = time;
        while (heap != rootHeapIndex) {
            heap = m_heapTree.parentOf(heap);
            m_timesOfWrites[heap].belowHeap = time;
        }
    }
    void didWrite(const HeapSet& heaps)
    {
        if (m_numberOfEntriesThatWritesEnd == m_numberOfEntriesEndedByWriteToAllHeaps)
            return;
        if (heaps.isTop()) {
            uint32_t time = timeOfNewWrite();
            m_timeOfLastWriteToAllHeaps = time;
            if (!m_timesOfWrites.empty())
                m_timesOfWrites[rootHeapIndex].toHeap = time;
            m_numberOfEntriesEndedByWriteToAllHeaps = m_numberOfEntriesThatWritesEnd;
            return;
        }
        didWriteListedHeaps(heaps.heaps());
    }
    void didSuspend()
    {
        m_timeOfLastSuspension = m_clock++;
        RELEASE_ASSERT(m_clock);
    }

    ScopeMark mark() const { return { m_log.size(), m_floor, m_suspendedBlocks.size(), m_endOfInheritedEntries, m_log.isEmpty() ? nullptr : m_log.last().operation }; }
    ALWAYS_INLINE void restore(const ScopeMark& mark)
    {
        RELEASE_ASSERT(mark.floor <= mark.endOfInheritedEntries && mark.endOfInheritedEntries <= mark.logSize && mark.logSize <= m_log.size());
        RELEASE_ASSERT(mark.numberOfSuspendedBlocks <= m_suspendedBlocks.size());
        RELEASE_ASSERT(!mark.numberOfSuspendedBlocks || m_suspendedBlocks[mark.numberOfSuspendedBlocks - 1].mark.logSize <= mark.floor);

        uint32_t bucketMask = m_buckets.size() - 1;
        uint32_t logSizeOfMark = mark.logSize;
        uint32_t logSize = m_log.size();
        while (logSize > logSizeOfMark) {
            const Entry& entry = m_log[logSize - 1];
            uint32_t& bucket = m_buckets[entry.hash & bucketMask];
            ASSERT(bucket == logSize);
            bucket = entry.previousInBucket;
            --logSize;
        }
        m_log.shrink(logSize);
        m_floor = mark.floor;
        m_endOfInheritedEntries = mark.endOfInheritedEntries;

        if (m_suspendedBlocks.size() > mark.numberOfSuspendedBlocks) [[unlikely]]
            forgetSuspendedBlocksFrom(mark.numberOfSuspendedBlocks);
    }
    ALWAYS_INLINE bool canRestore(const ScopeMark& mark) const
    {
        if (mark.floor == mark.logSize || mark.logSize > m_log.size())
            return false;
        return m_log[mark.logSize - 1].operation == mark.newestOperation;
    }
    ALWAYS_INLINE void inheritScope(const ScopeMark& mark)
    {
        restore(mark);
        m_endOfInheritedEntries = m_log.size();
    }

    void switchBlocks(Block* from, Block* to)
    {
        ASSERT(from != to);
        if (!m_suspendedBlocks.isEmpty() || !from->isTerminated()) {
            switchBlocksThatMayWait(from, to);
            return;
        }
        m_floor = m_log.size();
        m_endOfInheritedEntries = m_floor;
    }
    void continueIn(Block* from, Block* to)
    {
        ASSERT_UNUSED(from, from->isTerminated());
        RELEASE_ASSERT(to->numberOfIncomingEdges() == 1);
    }

private:
    struct Entry {
        Operation* operation;
        uint32_t hash;
        uint32_t previousInBucket;
        uint32_t clock;
        uint64_t payload;
    };
    struct SuspendedBlock {
        Block* block;
        ScopeMark mark;
    };
    struct TimesOfWrites {
        uint32_t toHeap;
        uint32_t belowHeap;
    };

    static uint64_t combineWords(uint64_t hash, uint64_t word)
    {
        return (std::rotr(hash, 32) ^ word) * 0x9E3779B97F4A7C15ull;
    }
    static uint32_t combine(uint64_t first, uint64_t second)
    {
        return static_cast<uint32_t>(combineWords(combineWords(0, first), second) >> 32);
    }
    static uint32_t hashOf(const NumberingKeyOfPurePatch&);
    static uint32_t baseOf(const Operation* operation)
    {
        return operation->m_opcode == Add ? operation->m_children[0]->index() : UINT32_MAX;
    }
    static bool mayHold(const Operation* operation)
    {
        return operation->m_owner && !operation->traps() && !hasTrait(operation->m_opcode, OpcodeTrait::HasEffects | OpcodeTrait::HasNoPosition | OpcodeTrait::IsNeverNumbered);
    }
    static bool matches(const NumberingKey& key, const Entry& entry)
    {
        const Operation* operation = entry.operation;
        return operation->m_opcode == key.opcode && operation->m_type == key.type && entry.payload == key.payload
            && operation->m_children[0] == key.children[0] && operation->m_children[1] == key.children[1] && operation->m_children[2] == key.children[2];
    }
    static bool matches(const NumberingKeyOfPurePatch&, const Entry&);
    static bool isAddressInFrame(const Operation* operation)
    {
        if (operation->m_owner)
            return false;
        if (operation->m_opcode == Add)
            operation = operation->m_children[0];
        return operation->m_opcode == FramePointer || operation->m_opcode == SlotBase;
    }
    static bool computationReadsAddressInFrame(const Operation* computation)
    {
        ASSERT(countsChildrenAtFirstUse(computation->m_opcode));
        for (const Operation* child : std::span<Operation* const>(computation->m_children, computation->m_auxiliary)) {
            if (isAddressInFrame(child))
                return true;
        }
        return false;
    }

    template<bool isKeyOfLoad>
    ALWAYS_INLINE Found lookUpOperation(const NumberingKey& key, uint32_t hash, Reach reach) const
    {
        uint32_t floor = reach == Reach::OwnEntries ? m_endOfInheritedEntries : m_floor;
        if (floor == m_log.size())
            return { };

        for (uint32_t link = m_buckets[hash & (m_buckets.size() - 1)]; link > floor; link = m_log[link - 1].previousInBucket) {
            const Entry& entry = m_log[link - 1];
            ASSERT(entry.previousInBucket < link);
            if (entry.hash != hash || !matches(key, entry))
                continue;

            Operation* operation = entry.operation;
            if constexpr (isKeyOfLoad) {
                if (!operation->has(OperationFlag::ReadsImmutableHeap) && entry.clock <= m_timeOfLastWrite && entry.clock <= timeOfLastWriteTo(operation->heap()))
                    return { };
            }
            if (operation->isNarrowReference() && entry.clock <= m_timeOfLastSuspension)
                return { };
            return { operation, link <= m_endOfInheritedEntries };
        }
        return { };
    }

    ALWAYS_INLINE void appendToLog(Operation* operation, uint32_t hash, uint64_t payload)
    {
        if (m_log.size() >= m_buckets.size()) [[unlikely]]
            growBuckets();
        uint32_t& bucket = m_buckets[hash & (m_buckets.size() - 1)];
        m_log.appendWithinCapacity(Entry { operation, hash, bucket, m_clock, payload });
        bucket = m_log.size();
    }

    uint32_t timeOfNewWrite()
    {
        uint32_t time = m_clock++;
        RELEASE_ASSERT(m_clock);
        m_timeOfLastWrite = time;
        return time;
    }
    uint32_t timeOfLastWriteTo(HeapIndex heap) const
    {
        if (m_timesOfWrites.empty())
            return m_timeOfLastWriteToAllHeaps;
        uint32_t time = std::max(m_timesOfWrites[heap].toHeap, m_timesOfWrites[heap].belowHeap);
        while (heap != rootHeapIndex) {
            heap = m_heapTree.parentOf(heap);
            time = std::max(time, m_timesOfWrites[heap].toHeap);
        }
        return time;
    }
    bool patchWasEndedByWrite(const Entry&) const;
    struct ReadThatPhiStandsFor {
        const Operation* phi;
        const Operation* operand;
        HeapIndex heap;
    };
    const ReadThatPhiStandsFor* readThatPhiStandsFor(const Operation* phi) const;
    JS_EXPORT_PRIVATE void makeTimesOfWrites();
    JS_EXPORT_PRIVATE void didWriteListedHeaps(std::span<const HeapIndex>);
    JS_EXPORT_PRIVATE void growBuckets();
    void growBucketsWithoutPosition();
    JS_EXPORT_PRIVATE void switchBlocksThatMayWait(Block* from, Block* to);
    JS_EXPORT_PRIVATE void forgetSuspendedBlocksFrom(uint32_t numberKept);

    Procedure& m_procedure;
    Arena& m_arena;
    const HeapTree& m_heapTree;
    const bool m_savesFrameOnSuspend;
    ArenaVector<Entry> m_log;
    ArenaVector<uint32_t> m_buckets;
    uint32_t m_floor { 0 };
    uint32_t m_endOfInheritedEntries { 0 };
    uint32_t m_clock { 1 };
    uint32_t m_timeOfLastSuspension { 0 };
    uint32_t m_timeOfLastWrite { 0 };
    uint32_t m_timeOfLastWriteToAllHeaps { 0 };
    uint32_t m_numberOfEntriesThatWritesEnd { 0 };
    uint32_t m_numberOfEntriesEndedByWriteToAllHeaps { 0 };
    uint32_t m_numberOfEntriesEndedByLastListWritten { 0 };
    std::span<const HeapIndex> m_lastListWritten;
    std::span<TimesOfWrites> m_timesOfWrites;
    ArenaVector<SuspendedBlock> m_suspendedBlocks;
    ArenaVector<uint32_t> m_placeOfSuspendedBlock;
    ArenaVector<ReadThatPhiStandsFor> m_readsThatPhisStandFor;

    ArenaVector<Entry> m_withoutPosition;
    ArenaVector<uint32_t> m_bucketsWithoutPosition;
};

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
