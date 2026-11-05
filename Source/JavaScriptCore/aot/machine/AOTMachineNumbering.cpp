/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineNumbering.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Machine {

static constexpr uint32_t machineNumberingSmallestNumberOfBuckets = 64;

static bool machineNumberingIsAddressInFrame(const Operation* operation)
{
    if (!operation->hasNoPosition())
        return false;
    if (operation->opcode() == Add)
        operation = operation->child(0);
    return operation->opcode() == FramePointer || operation->opcode() == SlotBase;
}

bool Numbering::isComputedFromAddressInFrame(const Operation* operation)
{
    ASSERT(!isLoad(operation->opcode()));
    return std::ranges::any_of(operation->children(), machineNumberingIsAddressInFrame);
}

template<typename EntryType>
static void machineNumberingGrowBuckets(Arena& arena, ArenaVector<EntryType>& entries, ArenaVector<uint32_t>& buckets)
{
    uint32_t numberOfBuckets = buckets.isEmpty() ? machineNumberingSmallestNumberOfBuckets : buckets.size() * 2;
    RELEASE_ASSERT(numberOfBuckets);
    buckets.fill(arena, numberOfBuckets, 0);
    entries.reserve(arena, numberOfBuckets);

    uint32_t mask = numberOfBuckets - 1;
    for (uint32_t index = 0; index < entries.size(); ++index) {
        EntryType& entry = entries[index];
        uint32_t& bucket = buckets[entry.hash & mask];
        entry.previousInBucket = bucket;
        bucket = index + 1;
    }
}

Numbering::Numbering(Procedure& procedure)
    : m_arena(procedure.arena())
    , m_heapTree(procedure.heapTree())
    , m_savesFrameOnSuspend(procedure.traits().savesFrameOnSuspend)
    , m_timesOfWrites(procedure.arena().createSpan<TimesOfWrites>(procedure.heapTree().size()))
{
    machineNumberingGrowBuckets(m_arena, m_log, m_buckets);
    machineNumberingGrowBuckets(m_arena, m_withoutPosition, m_bucketsWithoutPosition);
}

void Numbering::growBucketsWithoutPosition()
{
    machineNumberingGrowBuckets(m_arena, m_withoutPosition, m_bucketsWithoutPosition);
}

Operation* Numbering::findPurePatch(const NumberingKeyOfPurePatch& key) const
{
    if (m_floor == m_log.size())
        return nullptr;

    uint32_t hash = hashOf(key);
    for (uint32_t link = m_buckets[hash & (m_buckets.size() - 1)]; link > m_floor; link = m_log[link - 1].previousInBucket) {
        const Entry& entry = m_log[link - 1];
        ASSERT(entry.previousInBucket < link);
        if (entry.hash != hash || !matches(key, entry))
            continue;

        Operation* operation = entry.operation;
        if (operation->isNarrowReference() && entry.clock <= m_timeOfLastSuspension)
            return nullptr;
        if (operation->opcode() == Phi)
            return entry.clock <= m_timeOfLastSuspension ? nullptr : operation;
        if (patchWasEndedByWrite(entry))
            return nullptr;
        return operation;
    }
    return nullptr;
}

bool Numbering::isStillValid(const NumberingKey& key, const Operation* load) const
{
    ASSERT(isLoad(key.opcode));
    if (m_floor == m_log.size())
        return false;

    uint32_t hash = hashOf(key);
    for (uint32_t link = m_buckets[hash & (m_buckets.size() - 1)]; link > m_floor; link = m_log[link - 1].previousInBucket) {
        const Entry& entry = m_log[link - 1];
        if (entry.operation == load)
            return load->has(OperationFlag::ReadsImmutableHeap) || entry.clock > timeOfLastWriteTo(load->heap());
    }
    return false;
}

void Numbering::addPurePatch(const NumberingKeyOfPurePatch& key, PatchOperation* patch)
{
    RELEASE_ASSERT(!patch->hasNoPosition() && !patch->traps() && patch->opcode() == Patch && patch->pureFunction && !patch->effects.terminal);
    if (m_savesFrameOnSuspend && isComputedFromAddressInFrame(patch))
        return;

    ++m_numberOfEntriesThatWritesEnd;
    appendToLog(patch, hashOf(key), key.id);
    ASSERT(matches(key, m_log.last()));
}

void Numbering::addInPlaceOfPurePatch(const NumberingKeyOfPurePatch& key, Operation* phi)
{
    RELEASE_ASSERT(key.operands.empty() && phi->opcode() == Phi && phi->type() == key.type);

    appendToLog(phi, hashOf(key), key.id);
    ASSERT(matches(key, m_log.last()));
}

void Numbering::didWrite(const HeapSet& heaps)
{
    if (m_numberOfEntriesThatWritesEnd == m_numberOfEntriesEndedByWriteToAllHeaps)
        return;
    if (heaps.isTop()) {
        m_timesOfWrites[rootHeapIndex].toHeap = timeOfNewWrite();
        m_numberOfEntriesEndedByWriteToAllHeaps = m_numberOfEntriesThatWritesEnd;
        return;
    }

    std::span<const HeapIndex> list = heaps.heaps();
    if (list.empty())
        return;
    if (m_numberOfEntriesThatWritesEnd == m_numberOfEntriesEndedByLastListWritten && list.data() == m_lastListWritten.data() && list.size() == m_lastListWritten.size())
        return;
    m_numberOfEntriesEndedByLastListWritten = m_numberOfEntriesThatWritesEnd;
    m_lastListWritten = list;

    uint32_t time = timeOfNewWrite();
    for (HeapIndex heap : list) {
        m_timesOfWrites[heap].toHeap = time;
        while (heap != rootHeapIndex) {
            heap = m_heapTree.parentOf(heap);
            if (m_timesOfWrites[heap].belowHeap == time)
                break;
            m_timesOfWrites[heap].belowHeap = time;
        }
    }
}

void Numbering::forgetSuspendedBlocksFrom(uint32_t numberKept)
{
    while (m_suspendedBlocks.size() > numberKept) {
        uint32_t place = m_suspendedBlocks.size();
        uint32_t& placeOfBlock = m_placeOfSuspendedBlock[m_suspendedBlocks.takeLast().block->index()];
        if (placeOfBlock == place)
            placeOfBlock = 0;
    }
}

bool Numbering::canRestore(const ScopeMark& mark) const
{
    if (mark.floor == mark.logSize || mark.logSize > m_log.size())
        return false;
    return m_log[mark.logSize - 1].operation == mark.newestOperation;
}

void Numbering::inheritScope(const ScopeMark& mark)
{
    restore(mark);
    m_endOfInheritedEntries = m_log.size();
}

void Numbering::switchBlocksThatMayWait(Block* from, Block* to)
{
    uint32_t placeOfTarget = to->index() < m_placeOfSuspendedBlock.size() ? m_placeOfSuspendedBlock[to->index()] : 0;
    if (placeOfTarget) {
        ASSERT(m_suspendedBlocks[placeOfTarget - 1].block == to);
        ScopeMark markOfTarget = m_suspendedBlocks[placeOfTarget - 1].mark;
        ASSERT(markOfTarget.numberOfSuspendedBlocks == placeOfTarget - 1);
        restore(markOfTarget);
        return;
    }

    if (!from->isTerminated()) {
        if (from->index() >= m_placeOfSuspendedBlock.size())
            m_placeOfSuspendedBlock.grow(m_arena, std::max(from->index() + 1, m_placeOfSuspendedBlock.size() * 2));
        m_suspendedBlocks.append(m_arena, SuspendedBlock { from, mark() });
        m_placeOfSuspendedBlock[from->index()] = m_suspendedBlocks.size();
    }
    m_floor = m_log.size();
    m_endOfInheritedEntries = m_floor;
}

uint32_t Numbering::hashOf(const NumberingKeyOfPurePatch& key)
{
    uint64_t hash = combineWords(0, static_cast<uint64_t>(Patch) | static_cast<uint64_t>(key.type.kind()) << 8 | static_cast<uint64_t>(key.type.tupleIndex()) << 16);
    hash = combineWords(hash, key.id);
    for (const Operation* operand : key.operands)
        hash = combineWords(hash, operand->index());
    return static_cast<uint32_t>(hash >> 32);
}

bool Numbering::matches(const NumberingKeyOfPurePatch& key, const Entry& entry)
{
    const Operation* operation = entry.operation;
    if (operation->opcode() == Phi)
        return operation->type() == key.type && entry.payload == key.id && key.operands.empty();
    if (operation->opcode() != Patch || operation->type() != key.type || entry.payload != key.id)
        return false;

    const PatchOperation* patch = static_cast<const PatchOperation*>(operation);
    if (!patch->pureFunction->dependsOnChildren)
        return key.operands.empty();
    return std::ranges::equal(patch->operands(), key.operands);
}

bool Numbering::patchWasEndedByWrite(const Entry& entry) const
{
    const PatchOperation* patch = entry.operation->as<PatchOperation>();
    if (!patch->pureFunction->dependsOnMemory)
        return false;
    const HeapSet& reads = patch->effects.reads;
    if (reads.isTop())
        return entry.clock <= timeOfLastWriteTo(rootHeapIndex);
    return std::ranges::any_of(reads.heaps(), [&](HeapIndex heap) {
        return entry.clock <= timeOfLastWriteTo(heap);
    });
}

void Numbering::growBuckets()
{
    machineNumberingGrowBuckets(m_arena, m_log, m_buckets);
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
