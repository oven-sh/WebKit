/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

enum class SharedAnswers : uint8_t { AboutTypes, AboutProgram };

inline constexpr size_t numberOfSharedAnswerKinds = 2;

class SharedTypeMemo {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SharedTypeMemo);
    WTF_MAKE_NONCOPYABLE(SharedTypeMemo);
public:
    enum class Relation : uint32_t { Union = 1, Intersection, Join };

    struct Key {
        uint64_t first { 0 };
        uint32_t second { 0 };
    };

    struct Counts {
        uint64_t entries { 0 };
        uint64_t found { 0 };
        uint64_t notFound { 0 };
        unsigned fullShards { 0 };
    };

    static constexpr Key keyOf(Relation relation, TypeID first, TypeID second)
    {
        return { static_cast<uint64_t>(first.index()) << 32 | second.index(), static_cast<uint32_t>(relation) };
    }

    static constexpr Key keyOf(uint64_t firstHash, uint64_t secondHash)
    {
        return { firstHash ? firstHash : 1, static_cast<uint32_t>(secondHash >> 32) | hashedKeyBit };
    }

    AOT_CLASS_THRESHOLD(size_t, defaultMaxSlotsInShard, 1 << 18);

    SharedTypeMemo(bool verifiesAnswers, bool countsLookups, size_t maxSlotsInShard = defaultMaxSlotsInShard)
        : m_verifiesAnswers(verifiesAnswers)
        , m_countsLookups(countsLookups)
        , m_maxSlotsInShard(maxSlotsInShard)
    {
    }

    bool verifiesAnswers() const { return m_verifiesAnswers; }

    TypeID find(Key key) const
    {
        uint64_t position = positionOf(key);
        const Shard& shard = m_shards[shardIndexOf(position)];
        const Table* table = shard.table.load(std::memory_order_acquire);
        TypeID type = table ? table->find(key, position) : TypeID();
        if (m_countsLookups) [[unlikely]]
            (type ? shard.found : shard.notFound).fetch_add(1, std::memory_order_relaxed);
        return type;
    }

    void add(Key key, TypeID type)
    {
        RELEASE_ASSERT(key.first && type && !(type.index() & provisionalBit));
        uint64_t position = positionOf(key);
        m_shards[shardIndexOf(position)].add(key, position, type, m_maxSlotsInShard);
    }

    Counts counts()
    {
        Counts total;
        for (Shard& shard : m_shards) {
            Locker locker { shard.lock };
            total.entries += shard.size;
            total.found += shard.found.load(std::memory_order_relaxed);
            total.notFound += shard.notFound.load(std::memory_order_relaxed);
            total.fullShards += shard.isFull.load(std::memory_order_relaxed);
        }
        return total;
    }

private:
    static constexpr unsigned shardBits = 6;
    static constexpr size_t initialSlotsInShard = 64;
    static constexpr size_t growthFactor = 4;
    static constexpr uint32_t hashedKeyBit = 1u << 31;
    static constexpr uint32_t provisionalBit = 1u << 31;

    struct Slot {
        std::atomic<uint64_t> first { 0 };
        std::atomic<uint64_t> secondAndType { 0 };
    };

    struct Table {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Table);

        explicit Table(size_t size)
            : slots(size)
        {
        }

        TypeID find(Key key, uint64_t position) const
        {
            size_t mask = slots.size() - 1;
            for (size_t index = position & mask; ; index = (index + 1) & mask) {
                const Slot& slot = slots[index];
                uint64_t first = slot.first.load(std::memory_order_acquire);
                if (!first)
                    return { };
                if (first != key.first)
                    continue;
                uint64_t secondAndType = slot.secondAndType.load(std::memory_order_relaxed);
                if (static_cast<uint32_t>(secondAndType >> 32) == key.second)
                    return TypeID(static_cast<uint32_t>(secondAndType));
            }
        }

        void insert(Key key, uint64_t position, uint32_t type)
        {
            size_t mask = slots.size() - 1;
            size_t index = position & mask;
            while (slots[index].first.load(std::memory_order_relaxed))
                index = (index + 1) & mask;
            slots[index].secondAndType.store(static_cast<uint64_t>(key.second) << 32 | type, std::memory_order_relaxed);
            slots[index].first.store(key.first, std::memory_order_release);
        }

        Vector<Slot> slots;
    };

    struct Shard {
        void add(Key key, uint64_t position, TypeID type, size_t maxSlotsInShard)
        {
            if (isFull.load(std::memory_order_relaxed))
                return;
            Locker locker { lock };
            Table* current = table.load(std::memory_order_relaxed);
            if (current && current->find(key, position))
                return;
            if (!current || (size + 1) * 4 > current->slots.size() * 3) {
                if (current && current->slots.size() >= maxSlotsInShard) {
                    isFull.store(true, std::memory_order_relaxed);
                    return;
                }
                current = &grow();
            }
            current->insert(key, position, type.index());
            ++size;
        }

        Table& grow() WTF_REQUIRES_LOCK(lock)
        {
            auto larger = makeUnique<Table>(tables.isEmpty() ? initialSlotsInShard : tables.last()->slots.size() * growthFactor);
            if (!tables.isEmpty()) {
                for (const Slot& slot : tables.last()->slots) {
                    uint64_t first = slot.first.load(std::memory_order_relaxed);
                    if (!first)
                        continue;
                    uint64_t secondAndType = slot.secondAndType.load(std::memory_order_relaxed);
                    Key key { first, static_cast<uint32_t>(secondAndType >> 32) };
                    larger->insert(key, positionOf(key), static_cast<uint32_t>(secondAndType));
                }
            }
            tables.append(WTF::move(larger));
            table.store(tables.last().get(), std::memory_order_release);
            return *tables.last();
        }

        Lock lock;
        std::atomic<Table*> table { nullptr };
        std::atomic<bool> isFull { false };
        mutable std::atomic<uint64_t> found { 0 };
        mutable std::atomic<uint64_t> notFound { 0 };
        Vector<std::unique_ptr<Table>> tables WTF_GUARDED_BY_LOCK(lock);
        size_t size WTF_GUARDED_BY_LOCK(lock) { 0 };
    };

    static constexpr uint64_t positionOf(Key key)
    {
        uint64_t mixed = (key.first ^ key.second) * 0x9e3779b97f4a7c15ull;
        return mixed ^ mixed >> 32;
    }

    static constexpr unsigned shardIndexOf(uint64_t position) { return static_cast<unsigned>(position >> (64 - shardBits)); }

    const bool m_verifiesAnswers;
    const bool m_countsLookups;
    const size_t m_maxSlotsInShard;
    std::array<Shard, 1u << shardBits> m_shards;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
