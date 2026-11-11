/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineNumbering.h"

#if ENABLE(AOT)

#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static constexpr uint32_t machineNumberingSmallestNumberOfBuckets = 64;

template<typename EntryType>
static ALWAYS_INLINE void machineNumberingMakeFirstBuckets(Arena& arena, ArenaVector<EntryType>& entries, ArenaVector<uint32_t>& buckets)
{
    ASSERT(buckets.isEmpty() && entries.isEmpty());
    buckets.fill(arena, machineNumberingSmallestNumberOfBuckets, 0);
    entries.reserve(arena, machineNumberingSmallestNumberOfBuckets);
}

template<typename EntryType>
static void machineNumberingGrowBuckets(Arena& arena, ArenaVector<EntryType>& entries, ArenaVector<uint32_t>& buckets)
{
    uint32_t numberOfBuckets = buckets.size() * 2;
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
    : m_procedure(procedure)
    , m_arena(procedure.arena())
    , m_heapTree(procedure.heapTree())
    , m_savesFrameOnSuspend(procedure.traits().savesFrameOnSuspend)
{
    machineNumberingMakeFirstBuckets(m_arena, m_log, m_buckets);
    machineNumberingMakeFirstBuckets(m_arena, m_withoutPosition, m_bucketsWithoutPosition);
}

void Numbering::makeTimesOfWrites()
{
    m_timesOfWrites = m_arena.createSpan<TimesOfWrites>(m_heapTree.size());
    m_timesOfWrites[rootHeapIndex].toHeap = m_timeOfLastWriteToAllHeaps;
}

void Numbering::growBucketsWithoutPosition()
{
    machineNumberingGrowBuckets(m_arena, m_withoutPosition, m_bucketsWithoutPosition);
    m_procedure.noteListThatBuildingLeaves(Procedure::ListThatBuildingLeaves::OperationsWithoutPosition, m_withoutPosition);
    m_procedure.noteListThatBuildingLeaves(Procedure::ListThatBuildingLeaves::BucketsWithoutPosition, m_bucketsWithoutPosition);
}

Operation* Numbering::findPurePatch(const NumberingKeyOfPurePatch& key) const
{
    if (m_floor == m_log.size())
        return nullptr;

    uint32_t hash = hashOf(key);
    for (uint32_t link = m_buckets[hash & (m_buckets.size() - 1)]; link > m_floor; link = m_log[link - 1].previousInBucket) {
        const Entry& entry = m_log[link - 1];
        ASSERT(entry.previousInBucket < link);
        if (entry.hash != hash)
            continue;
        if (entry.operation->opcode() == Phi && !key.operands.empty()) {
            const ReadThatPhiStandsFor* read = readThatPhiStandsFor(entry.operation);
            if (!read || entry.operation->type() != key.type || entry.payload != key.id || key.operands.size() != 1 || read->operand != key.operands[0])
                continue;
            if (entry.clock <= m_timeOfLastSuspension || entry.clock <= timeOfLastWriteTo(read->heap))
                return nullptr;
            return entry.operation;
        }
        if (!matches(key, entry))
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
    if (m_savesFrameOnSuspend && std::ranges::any_of(patch->operands(), isAddressInFrame))
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

void Numbering::addInPlaceOfReadOfMemory(const NumberingKeyOfPurePatch& key, Operation* phi, HeapIndex heap)
{
    RELEASE_ASSERT(key.operands.size() == 1 && phi->opcode() == Phi && phi->type() == key.type);
    if (m_savesFrameOnSuspend && isAddressInFrame(key.operands[0]))
        return;

    ++m_numberOfEntriesThatWritesEnd;
    m_readsThatPhisStandFor.append(m_arena, ReadThatPhiStandsFor { phi, key.operands[0], heap });
    appendToLog(phi, hashOf(key), key.id);
}

const Numbering::ReadThatPhiStandsFor* Numbering::readThatPhiStandsFor(const Operation* phi) const
{
    for (uint32_t index = m_readsThatPhisStandFor.size(); index--;) {
        if (m_readsThatPhisStandFor[index].phi == phi)
            return &m_readsThatPhisStandFor[index];
    }
    return nullptr;
}

void Numbering::didWriteListedHeaps(std::span<const HeapIndex> list)
{
    if (list.empty())
        return;
    if (m_numberOfEntriesThatWritesEnd == m_numberOfEntriesEndedByLastListWritten && list.data() == m_lastListWritten.data() && list.size() == m_lastListWritten.size())
        return;
    m_numberOfEntriesEndedByLastListWritten = m_numberOfEntriesThatWritesEnd;
    m_lastListWritten = list;

    uint32_t time = timeOfNewWrite();
    if (m_timesOfWrites.empty()) [[unlikely]]
        makeTimesOfWrites();
    TimesOfWrites* const times = m_timesOfWrites.data();
    const size_t numberOfHeaps = m_timesOfWrites.size();
    const HeapIndex* const parents = m_heapTree.parents().data();
    RELEASE_ASSERT(numberOfHeaps <= m_heapTree.size());
    for (HeapIndex heap : list) {
        RELEASE_ASSERT(heap < numberOfHeaps);
        times[heap].toHeap = time;
        while (heap != rootHeapIndex) {
            heap = parents[heap];
            if (times[heap].belowHeap == time)
                break;
            times[heap].belowHeap = time;
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
    m_procedure.noteListThatBuildingLeaves(Procedure::ListThatBuildingLeaves::NumberedOperations, m_log);
    m_procedure.noteListThatBuildingLeaves(Procedure::ListThatBuildingLeaves::BucketsOfNumberedOperations, m_buckets);
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
