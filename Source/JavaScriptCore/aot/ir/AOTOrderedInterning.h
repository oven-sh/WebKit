/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include <array>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// Interns a batch of canonical records on all threads.
template<typename Record, typename RecordID, typename Table>
class OrderedInterning {
    WTF_MAKE_NONCOPYABLE(OrderedInterning);
public:
    OrderedInterning(Table& table, const ParallelLoops& loops, std::span<Record> records)
        : m_table(table)
        , m_loops(loops)
        , m_records(records)
        , m_hashes(records.size())
        , m_classes(records.size())
        , m_firstPositionInClass(records.size())
        , m_ids(records.size())
    {
    }

    void setHash(size_t position, uint32_t hash) { m_hashes[position] = hash; }

    // After classify(): the id of an equal record that the table had before the batch, if any.
    RecordID existingID(size_t position) const { return m_ids[position]; }
    // After classify(), without an existingID(): the same number for equal records, and a position of the batch.
    uint32_t equivalenceClass(size_t position) const { return m_classes[position]; }

    void classifyAll()
    {
        Vector<uint32_t> positions(m_records.size(), [](size_t position) {
            return static_cast<uint32_t>(position);
        });
        classify(positions.span());
    }

    void classify(std::span<const uint32_t> positions)
    {
        constexpr size_t minCountForThreads = 2048;
        size_t count = positions.size();
        if (count < minCountForThreads) {
            for (uint32_t position : positions)
                classifyInShard(m_shards[shardOf(m_hashes[position])], position);
            return;
        }

        size_t positionsPerChunk = (count + numberOfChunks() - 1) / numberOfChunks();
        size_t chunkCount = (count + positionsPerChunk - 1) / positionsPerChunk;
        Vector<std::array<uint32_t, numberOfShards>> offsets(chunkCount);
        auto forEachPositionInChunk = [&](unsigned chunk, const auto& functor) {
            size_t end = std::min(count, (static_cast<size_t>(chunk) + 1) * positionsPerChunk);
            for (size_t index = static_cast<size_t>(chunk) * positionsPerChunk; index < end; ++index)
                functor(positions[index]);
        };
        forEachIndexInParallel(m_loops, static_cast<unsigned>(chunkCount), [&](unsigned chunk) {
            offsets[chunk].fill(0);
            forEachPositionInChunk(chunk, [&](uint32_t position) {
                ++offsets[chunk][shardOf(m_hashes[position])];
            });
        });
        std::array<uint32_t, numberOfShards + 1> firstInShard;
        uint32_t next = 0;
        for (size_t shard = 0; shard < numberOfShards; ++shard) {
            firstInShard[shard] = next;
            for (auto& offsetsOfChunk : offsets) {
                uint32_t numberInChunk = offsetsOfChunk[shard];
                offsetsOfChunk[shard] = next;
                next += numberInChunk;
            }
        }
        firstInShard[numberOfShards] = next;
        Vector<uint32_t> positionsByShard(count);
        forEachIndexInParallel(m_loops, static_cast<unsigned>(chunkCount), [&](unsigned chunk) {
            forEachPositionInChunk(chunk, [&](uint32_t position) {
                positionsByShard[offsets[chunk][shardOf(m_hashes[position])]++] = position;
            });
        });
        forEachIndexInParallel(m_loops, numberOfShards, [&](unsigned shard) {
            for (uint32_t position : positionsByShard.subspan(firstInShard[shard], firstInShard[shard + 1] - firstInShard[shard]))
                classifyInShard(m_shards[shard], position);
        });
    }

    // Call once, after every position has been classified. Afterwards idAt() is final for every position.
    void assignIDs()
    {
        RELEASE_ASSERT(!m_hasAssignedIDs);
        size_t count = m_records.size();
        m_firstNewID = m_table.m_records.size();
        size_t positionsPerChunk = std::max<size_t>(1024, (count + numberOfChunks() - 1) / numberOfChunks());
        size_t chunkCount = (count + positionsPerChunk - 1) / positionsPerChunk;
        Vector<uint32_t> firstIDOfChunk(chunkCount);
        auto forEachPositionInChunk = [&](unsigned chunk, const auto& functor) {
            size_t end = std::min(count, (static_cast<size_t>(chunk) + 1) * positionsPerChunk);
            for (size_t position = static_cast<size_t>(chunk) * positionsPerChunk; position < end; ++position)
                functor(position);
        };
        forEachIndexInParallel(m_loops, static_cast<unsigned>(chunkCount), [&](unsigned chunk) {
            uint32_t numberOfNewRecords = 0;
            forEachPositionInChunk(chunk, [&](size_t position) {
                numberOfNewRecords += !m_ids[position] && isFirstInClass(position);
            });
            firstIDOfChunk[chunk] = numberOfNewRecords;
        });
        uint32_t next = m_firstNewID;
        for (uint32_t& first : firstIDOfChunk) {
            uint32_t numberInChunk = first;
            first = next;
            next += numberInChunk;
        }
        m_numberOfNewRecords = next - m_firstNewID;
        forEachIndexInParallel(m_loops, static_cast<unsigned>(chunkCount), [&](unsigned chunk) {
            uint32_t id = firstIDOfChunk[chunk];
            forEachPositionInChunk(chunk, [&](size_t position) {
                if (!m_ids[position] && isFirstInClass(position))
                    m_ids[position] = RecordID(id++);
            });
        });
        forEachIndexInParallel(m_loops, static_cast<unsigned>(chunkCount), [&](unsigned chunk) {
            forEachPositionInChunk(chunk, [&](size_t position) {
                if (!m_ids[position])
                    m_ids[position] = m_ids[m_firstPositionInClass[m_classes[position]]];
            });
        });
        m_hasAssignedIDs = true;
    }

    RecordID idAt(size_t position) const { return m_ids[position]; }
    // After assignIDs(): whether commit() moves this record into the table.
    bool isNew(size_t position) const { return m_ids[position].index() >= m_firstNewID && isFirstInClass(position); }
    Vector<RecordID> takeIDs() { return WTF::move(m_ids); }

    // Moves the new records into the table. Their hashes must be those of the records as they are now.
    void commit()
    {
        RELEASE_ASSERT(m_hasAssignedIDs);
        if (!m_numberOfNewRecords)
            return;
        constexpr size_t positionsPerTask = 512;
        size_t count = m_records.size();
        m_table.m_records.growWithoutConstructing(m_firstNewID + m_numberOfNewRecords);
        m_table.m_index.reserve(m_loops, m_numberOfNewRecords);
        forEachIndexInParallel(m_loops, static_cast<unsigned>((count + positionsPerTask - 1) / positionsPerTask), [&](unsigned task) {
            size_t end = std::min(count, (static_cast<size_t>(task) + 1) * positionsPerTask);
            for (size_t position = static_cast<size_t>(task) * positionsPerTask; position < end; ++position) {
                if (!isNew(position))
                    continue;
                m_table.m_records.constructAt(m_ids[position].index(), WTF::move(m_records[position]));
                m_table.m_index.addConcurrently(m_hashes[position], m_ids[position].index());
            }
        });
        m_table.m_index.didAddConcurrently(m_numberOfNewRecords);
    }

private:
    static constexpr size_t numberOfShards = 64;

    struct Shard {
        Vector<uint32_t> slots;
        size_t size { 0 };
    };

    static size_t shardOf(uint32_t hash) { return (hash * 0x9e3779b1u) >> 26; }
    static size_t firstSlotOf(uint32_t hash, size_t mask) { return (hash ^ hash >> 15) & mask; }
    size_t numberOfChunks() const { return static_cast<size_t>(m_loops.width()) * 4; }
    bool isFirstInClass(size_t position) const { return m_firstPositionInClass[m_classes[position]] == position; }

    void growShard(Shard& shard)
    {
        size_t numberOfSlots = std::max<size_t>(64, shard.slots.size() * 2);
        Vector<uint32_t> oldSlots = std::exchange(shard.slots, Vector<uint32_t>(FillWith { }, numberOfSlots, 0u));
        size_t mask = numberOfSlots - 1;
        for (uint32_t positionPlusOne : oldSlots) {
            if (!positionPlusOne)
                continue;
            size_t slot = firstSlotOf(m_hashes[positionPlusOne - 1], mask);
            while (shard.slots[slot])
                slot = (slot + 1) & mask;
            shard.slots[slot] = positionPlusOne;
        }
    }

    void classifyInShard(Shard& shard, uint32_t position)
    {
        uint32_t hash = m_hashes[position];
        const Record& record = m_records[position];
        if (auto existing = m_table.find(record, hash)) {
            m_ids[position] = *existing;
            m_classes[position] = position;
            m_firstPositionInClass[position] = position;
            return;
        }
        if ((shard.size + 1) * 2 > shard.slots.size())
            growShard(shard);
        size_t mask = shard.slots.size() - 1;
        size_t slot = firstSlotOf(hash, mask);
        for (; shard.slots[slot]; slot = (slot + 1) & mask) {
            uint32_t other = shard.slots[slot] - 1;
            if (m_hashes[other] != hash || m_records[other] != record)
                continue;
            m_classes[position] = other;
            m_firstPositionInClass[other] = std::min(m_firstPositionInClass[other], position);
            return;
        }
        shard.slots[slot] = position + 1;
        ++shard.size;
        m_classes[position] = position;
        m_firstPositionInClass[position] = position;
    }

    Table& m_table;
    const ParallelLoops& m_loops;
    std::span<Record> m_records;
    Vector<uint32_t> m_hashes;
    Vector<uint32_t> m_classes;
    Vector<uint32_t> m_firstPositionInClass;
    Vector<RecordID> m_ids;
    std::array<Shard, numberOfShards> m_shards;
    uint32_t m_firstNewID { 0 };
    uint32_t m_numberOfNewRecords { 0 };
    bool m_hasAssignedIDs { false };
};

// The records must be canonical.
template<typename Record, typename RecordID>
Vector<RecordID> internAllInOrder(TypeTable& types, const ParallelLoops& loops, std::span<Record> records)
{
    constexpr size_t recordsPerTask = 256;
    auto& table = types.recordsForOrderedInterning<Record>();
    OrderedInterning<Record, RecordID, std::remove_reference_t<decltype(table)>> interning(table, loops, records);
    forEachIndexInParallel(loops, static_cast<unsigned>((records.size() + recordsPerTask - 1) / recordsPerTask), [&](unsigned task) {
        size_t end = std::min(records.size(), (static_cast<size_t>(task) + 1) * recordsPerTask);
        for (size_t position = static_cast<size_t>(task) * recordsPerTask; position < end; ++position)
            interning.setHash(position, computeHash(records[position]));
    });
    interning.classifyAll();
    interning.assignIDs();
    interning.commit();
    return interning.takeIDs();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
