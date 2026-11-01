/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTOutgrownTable.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

enum class TestSitePlanner : uint8_t { FieldOrGetter = 16, CalledMember, MemberCall, AccessUnderTests };

class SiteContent {
public:
    explicit SiteContent(uint32_t planner) { add(planner); }

    void add(uint32_t word)
    {
        m_first = (m_first ^ word) * 0x9e3779b97f4a7c15ull;
        m_first ^= m_first >> 32;
        m_second = (m_second + word) * 0xc2b2ae3d27d4eb4full;
        m_second ^= m_second >> 29;
    }
    void add(const void* pointer)
    {
        uint64_t bits = reinterpret_cast<uintptr_t>(pointer);
        add(static_cast<uint32_t>(bits));
        add(static_cast<uint32_t>(bits >> 32));
    }
    template<typename Tag> void add(ID<Tag> id)
    {
        m_holdsOnlyIDsOfProgram &= !TypeTable::isProvisional(id);
        add(id.index());
    }
    void addTypesOf(const Function& function, ValueID id)
    {
        if (!id) {
            add(UINT32_MAX - 1);
            return;
        }
        const Value& value = function.at(id);
        add(value.type);
        add(value.expected().predicted());
    }

    bool holdsOnlyIDsOfProgram() const { return m_holdsOnlyIDsOfProgram; }
    uint64_t first() const { return m_first; }
    uint64_t second() const { return m_second; }

private:
    uint64_t m_first { 0x243f6a8885a308d3ull };
    uint64_t m_second { 0x13198a2e03707344ull };
    bool m_holdsOnlyIDsOfProgram { true };
};

class RefusedContents {
    WTF_MAKE_NONCOPYABLE(RefusedContents);
public:
    RefusedContents() = default;

    std::optional<uint32_t> find(const SiteContent& content) const
    {
        const Table* table = shardOf(content).table.load(std::memory_order_acquire);
        if (!table)
            return std::nullopt;
        return findIn(*table, keyOf(content), content.second());
    }

    void add(const SiteContent& content, uint32_t remark = 0)
    {
        RELEASE_ASSERT(remark <= remarkMask);
        Shard& shard = shardOf(content);
        Locker locker { shard.lock };
        Table* table = shard.table.load(std::memory_order_relaxed);
        if (table && findIn(*table, keyOf(content), content.second()))
            return;
        if (!table || (shard.numberOfEntries + 1) * 2 > table->size())
            table = &shard.grow();
        insertInto(*table, keyOf(content) | remark, content.second());
        ++shard.numberOfEntries;
    }

private:
    static constexpr unsigned shardBits = 6;
    static constexpr size_t initialTableSize = 64;
    static constexpr uint64_t remarkMask = 0xff;
    static constexpr uint64_t isInUse = 0x100;

    struct Slot {
        std::atomic<uint64_t> keyAndRemark { 0 };
        std::atomic<uint64_t> second { 0 };
    };

    using Table = Vector<Slot>;

    static std::optional<uint32_t> findIn(const Table& slots, uint64_t key, uint64_t second)
    {
        size_t mask = slots.size() - 1;
        for (size_t index = second & mask; uint64_t keyAndRemark = slots[index].keyAndRemark.load(std::memory_order_acquire); index = (index + 1) & mask) {
            if ((keyAndRemark & ~remarkMask) == key && slots[index].second.load(std::memory_order_relaxed) == second)
                return static_cast<uint32_t>(keyAndRemark & remarkMask);
        }
        return std::nullopt;
    }

    static void insertInto(Table& slots, uint64_t keyAndRemark, uint64_t second)
    {
        size_t mask = slots.size() - 1;
        size_t index = second & mask;
        while (slots[index].keyAndRemark.load(std::memory_order_relaxed))
            index = (index + 1) & mask;
        slots[index].second.store(second, std::memory_order_relaxed);
        slots[index].keyAndRemark.store(keyAndRemark, std::memory_order_release);
    }

    struct Shard {
        Table& grow() WTF_REQUIRES_LOCK(lock)
        {
            auto larger = makeUnique<Table>(tables.isEmpty() ? initialTableSize : tables.last()->size() * 2);
            if (!tables.isEmpty()) {
                for (const Slot& slot : *tables.last()) {
                    if (uint64_t keyAndRemark = slot.keyAndRemark.load(std::memory_order_relaxed))
                        insertInto(*larger, keyAndRemark, slot.second.load(std::memory_order_relaxed));
                }
            }
            tables.append(WTF::move(larger));
            table.store(tables.last().get(), std::memory_order_release);
            if (tables.size() > 1)
                letGoOfPagesOfOutgrownTable(tables[tables.size() - 2]->mutableSpan());
            return *tables.last();
        }

        Lock lock;
        std::atomic<Table*> table { nullptr };
        Vector<std::unique_ptr<Table>> tables WTF_GUARDED_BY_LOCK(lock);
        size_t numberOfEntries WTF_GUARDED_BY_LOCK(lock) { 0 };
    };

    static uint64_t keyOf(const SiteContent& content) { return (content.first() & ~(remarkMask | isInUse)) | isInUse; }
    const Shard& shardOf(const SiteContent& content) const { return m_shards[content.first() >> (64 - shardBits)]; }
    Shard& shardOf(const SiteContent& content) { return m_shards[content.first() >> (64 - shardBits)]; }

    std::array<Shard, 1u << shardBits> m_shards;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
