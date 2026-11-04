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

// What makes two operations the same value: everything but the origin.
struct NumberingKey {
    Opcode opcode;
    Type type;
    // Null where there is none.
    Operation* children[Operation::maxInlineChildren] { nullptr, nullptr, nullptr };
    // Of a load: uint32_t(offset) | uint64_t(heap) << 32. Of a Div on ARM64: 1 if it is chill.
    // Zero of every other operation. It is compared whatever the opcode.
    uint64_t payload { 0 };
};

struct NumberingKeyOfPurePatch {
    Type type;
    // PureFunction::id.
    uint64_t id;
    // Of a pure Patch that depends on its children.
    std::span<Operation* const> operands { };
};

// A place in the log. What was numbered behind it is forgotten when the scope comes back to it.
struct ScopeMark {
    uint32_t logSize { 0 };
    uint32_t floor { 0 };
    uint32_t numberOfSuspendedBlocks { 0 };
    uint32_t endOfInheritedEntries { 0 };
    // The newest entry that it counts. Null where it counts none.
    const Operation* newestOperation { nullptr };
};

// Pure operations, and loads by the time of the last write to their heap, within a scope. The scope is a log: it grows by add() and shrinks by restore().
// Operations without position are numbered for the whole procedure.
class Numbering {
    WTF_MAKE_NONCOPYABLE(Numbering);
public:
    JS_EXPORT_PRIVATE explicit Numbering(Procedure&);

    // Const32, Const64, ConstFloat, ConstDouble by their bits, FramePointer, SlotBase by the index of the slot, a pinned ArgumentReg by the index of the register,
    // AddressOf, NumberOf, InstanceAddressOf by space << 48 | entry << 32 | index. An Add without position by its constant, with the index of its base in `base`.
    static uint32_t hashWithoutPosition(Opcode opcode, uint64_t payload, uint32_t base = UINT32_MAX)
    {
        return combine(payload, static_cast<uint64_t>(opcode) | static_cast<uint64_t>(base) << 32);
    }
    Operation* findWithoutPosition(Opcode opcode, uint64_t payload, uint32_t hash, uint32_t base = UINT32_MAX) const
    {
        for (uint32_t link = m_bucketsWithoutPosition[hash & (m_bucketsWithoutPosition.size() - 1)]; link; link = m_withoutPosition[link - 1].previousInBucket) {
            const Entry& entry = m_withoutPosition[link - 1];
            ASSERT(entry.previousInBucket < link);
            if (entry.hash == hash && entry.payload == payload && entry.operation->m_opcode == opcode && baseOf(entry.operation) == base)
                return entry.operation;
        }
        return nullptr;
    }
    ALWAYS_INLINE void addWithoutPosition(Operation* operation, uint64_t payload, uint32_t hash)
    {
        RELEASE_ASSERT(operation->hasNoPosition());
        ASSERT(hasNoPosition(operation->opcode()) || operation->opcode() == ArgumentReg || operation->opcode() == Add);
        ASSERT(hash == hashWithoutPosition(operation->opcode(), payload, baseOf(operation)));
        ASSERT(!findWithoutPosition(operation->opcode(), payload, hash, baseOf(operation)));

        if (m_withoutPosition.size() >= m_bucketsWithoutPosition.size()) [[unlikely]]
            growBucketsWithoutPosition();
        uint32_t& bucket = m_bucketsWithoutPosition[hash & (m_bucketsWithoutPosition.size() - 1)];
        m_withoutPosition.appendWithinCapacity(Entry { operation, hash, bucket, m_clock, payload });
        bucket = m_withoutPosition.size();
    }

    enum class Reach : bool { WholeScope, OwnEntries };
    struct Found {
        Operation* operation { nullptr };
        // It was numbered ahead of the last inheritScope().
        bool isInherited { false };
    };
    // Which entry a request finds hangs on the keys alone: the hash only says where to look.
    static uint32_t hashOf(const NumberingKey& key)
    {
        uint64_t head = static_cast<uint64_t>(key.opcode) | static_cast<uint64_t>(key.type.kind()) << 8 | static_cast<uint64_t>(key.children[0]->index()) << 32;
        uint64_t rest = key.payload;
        if (key.children[1])
            rest ^= key.children[1]->index();
        if (key.children[2])
            rest ^= static_cast<uint64_t>(key.children[2]->index()) << 32;
        return combine(head, rest);
    }
    // An operation that the current scope holds and, for a load from a mutable heap or a Patch with PureFunction::dependsOnMemory, that no write has ended.
    Found lookUp(const NumberingKey& key, uint32_t hash, Reach reach = Reach::WholeScope) const
    {
        ASSERT(!isLoad(key.opcode));
        return lookUpOperation<false>(key, hash, reach);
    }
    Found lookUpLoad(const NumberingKey& key, uint32_t hash, Reach reach = Reach::WholeScope) const
    {
        ASSERT(isLoad(key.opcode));
        return lookUpOperation<true>(key, hash, reach);
    }
    JS_EXPORT_PRIVATE Operation* findPurePatch(const NumberingKeyOfPurePatch&) const;
    // Whether the scope holds this load under the key and no write has ended it, whatever was numbered under the key since.
    JS_EXPORT_PRIVATE bool isStillValid(const NumberingKey&, const Operation* load) const;
    ALWAYS_INLINE void add(const NumberingKey& key, uint32_t hash, Operation* operation)
    {
        RELEASE_ASSERT(operation->m_owner && !operation->traps() && countsChildrenAtFirstUse(operation->m_opcode));
        if (m_savesFrameOnSuspend && isComputedFromAddressInFrame(operation)) [[unlikely]]
            return;
        appendToLog(operation, hash, key.payload);
        ASSERT(matches(key, m_log.last()));
    }
    // It has its heap and OperationFlag::ReadsImmutableHeap by now: lookUpLoad() reads both off the operation.
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
    // The key is that of a pure Patch without operands, and the Phi holds what that Patch would give: one arm has read it, the other has had it made. It is found under the key up to the next suspension.
    JS_EXPORT_PRIVATE void addInPlaceOfPurePatch(const NumberingKeyOfPurePatch&, Operation* phi);

    // Ends the loads from that heap, from the heaps above it and from those below it, in every scope.
    // The clock follows the order in which the emitters append, not the order in which the code runs. No restore() turns it back,
    // so a write in one arm also ends loads for the other arm: more loads end than control flow asks for, never fewer.
    void didWrite(HeapIndex heap)
    {
        if (m_numberOfEntriesThatWritesEnd == m_numberOfEntriesEndedByWriteToAllHeaps)
            return;
        uint32_t time = timeOfNewWrite();
        m_timesOfWrites[heap].toHeap = time;
        while (heap != rootHeapIndex) {
            heap = m_heapTree.parentOf(heap);
            m_timesOfWrites[heap].belowHeap = time;
        }
    }
    JS_EXPORT_PRIVATE void didWrite(const HeapSet&);
    // Ends what stands in place of a pure Patch: found behind the suspension, it would have to be kept in the saved frame.
    void didSuspend()
    {
        m_timeOfLastSuspension = m_clock++;
        RELEASE_ASSERT(m_clock);
    }

    ScopeMark mark() const { return { m_log.size(), m_floor, m_suspendedBlocks.size(), m_endOfInheritedEntries, m_log.isEmpty() ? nullptr : m_log.last().operation }; }
    void restore(const ScopeMark& mark)
    {
        // A mark does not outlive a return to a block that was left ahead of it: the entries that it counts are gone, and others may stand in their places.
        // While a block waits, every scope begins at or behind the end of its log.
        RELEASE_ASSERT(mark.floor <= mark.endOfInheritedEntries && mark.endOfInheritedEntries <= mark.logSize && mark.logSize <= m_log.size());
        RELEASE_ASSERT(mark.numberOfSuspendedBlocks <= m_suspendedBlocks.size());
        RELEASE_ASSERT(!mark.numberOfSuspendedBlocks || m_suspendedBlocks[mark.numberOfSuspendedBlocks - 1].mark.logSize <= mark.floor);

        uint32_t bucketMask = m_buckets.size() - 1;
        while (m_log.size() > mark.logSize) {
            const Entry& entry = m_log.last();
            uint32_t& bucket = m_buckets[entry.hash & bucketMask];
            ASSERT(bucket == m_log.size());
            bucket = entry.previousInBucket;
            m_log.shrink(m_log.size() - 1);
        }
        m_floor = mark.floor;
        m_endOfInheritedEntries = mark.endOfInheritedEntries;

        if (m_suspendedBlocks.size() > mark.numberOfSuspendedBlocks) [[unlikely]]
            forgetSuspendedBlocksFrom(mark.numberOfSuspendedBlocks);
    }
    // Of a mark that was kept while other blocks were filled: its scope holds something, and nothing has come back to a place ahead of it since it was taken. So restore() may take it.
    JS_EXPORT_PRIVATE bool canRestore(const ScopeMark&) const;
    // restore(), and every entry that is left counts as inherited.
    JS_EXPORT_PRIVATE void inheritScope(const ScopeMark&);

    // The emitters leave `from` for `to`. If `to` was left before and nothing has come back to a block that was left ahead of it, its scope comes back.
    // Otherwise it starts with an empty scope, and the scope of `from` waits for a return.
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
    // `to` goes on in the scope of `from`, which is done.
    void continueIn(Block* from, Block* to)
    {
        ASSERT_UNUSED(from, from->isTerminated());
        RELEASE_ASSERT(to->numberOfIncomingEdges() == 1);
    }

private:
    struct Entry {
        Operation* operation;
        uint32_t hash;
        // One more than the index in the log of the entry that the bucket held before, or zero.
        uint32_t previousInBucket;
        // The clock when it was added.
        uint32_t clock;
        uint64_t payload;
    };
    struct SuspendedBlock {
        Block* block;
        ScopeMark mark;
    };
    // When the heap was last written itself, and when anything below it was.
    struct TimesOfWrites {
        uint32_t toHeap;
        uint32_t belowHeap;
    };

    // The upper half of a product hangs on every bit of both factors.
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
    JS_EXPORT_PRIVATE static bool isComputedFromAddressInFrame(const Operation*);

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
            // An older entry of the same key was added no later, so the write that ended this one has ended it too.
            if constexpr (isKeyOfLoad) {
                if (!operation->has(OperationFlag::ReadsImmutableHeap) && entry.clock <= m_timeOfLastWrite && entry.clock <= timeOfLastWriteTo(operation->heap()))
                    return { };
            }
            // A saved frame is visited by its map, which lists what the emitter of the suspension knew to be live. A narrow reference that a later request found again would live across the suspension and be listed nowhere.
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
        uint32_t time = std::max(m_timesOfWrites[heap].toHeap, m_timesOfWrites[heap].belowHeap);
        while (heap != rootHeapIndex) {
            heap = m_heapTree.parentOf(heap);
            time = std::max(time, m_timesOfWrites[heap].toHeap);
        }
        return time;
    }
    // Of a pure Patch: it depends on memory, and something that it reads has been written since it was added.
    bool patchWasEndedByWrite(const Entry&) const;
    JS_EXPORT_PRIVATE void growBuckets();
    void growBucketsWithoutPosition();
    void switchBlocksThatMayWait(Block* from, Block* to);
    JS_EXPORT_PRIVATE void forgetSuspendedBlocksFrom(uint32_t numberKept);

    Arena& m_arena;
    const HeapTree& m_heapTree;
    const bool m_savesFrameOnSuspend;
    ArenaVector<Entry> m_log;
    // One more than the index in the log of the newest entry, or zero. A power of two.
    ArenaVector<uint32_t> m_buckets;
    // An entry below it is out of scope. A chain runs from new to old, so a search ends at the first such entry.
    uint32_t m_floor { 0 };
    // At or above m_floor.
    uint32_t m_endOfInheritedEntries { 0 };
    uint32_t m_clock { 1 };
    uint32_t m_timeOfLastSuspension { 0 };
    uint32_t m_timeOfLastWrite { 0 };
    uint32_t m_numberOfEntriesThatWritesEnd { 0 };
    uint32_t m_numberOfEntriesEndedByWriteToAllHeaps { 0 };
    uint32_t m_numberOfEntriesEndedByLastListWritten { 0 };
    std::span<const HeapIndex> m_lastListWritten;
    // By the index of the heap.
    std::span<TimesOfWrites> m_timesOfWrites;
    ArenaVector<SuspendedBlock> m_suspendedBlocks;
    // By the index of the block: one more than its place in m_suspendedBlocks, or zero.
    ArenaVector<uint32_t> m_placeOfSuspendedBlock;

    ArenaVector<Entry> m_withoutPosition;
    ArenaVector<uint32_t> m_bucketsWithoutPosition;
};

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
