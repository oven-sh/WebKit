/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/Noncopyable.h>
#include <wtf/SegmentedVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename Entry>
class SharedMemo {
    WTF_MAKE_NONCOPYABLE(SharedMemo);
public:
    SharedMemo() = default;

    const Entry* find(uint32_t hash, NOESCAPE const Invocable<bool(const Entry&)> auto& isWanted) const LIFETIME_BOUND
    {
        return m_shards[shardIndexOf(hash)].find(hash, isWanted);
    }

    const Entry& add(uint32_t hash, Entry&& entry, NOESCAPE const Invocable<bool(const Entry&)> auto& isWanted) LIFETIME_BOUND
    {
        return m_shards[shardIndexOf(hash)].add(hash, WTF::move(entry), isWanted);
    }

private:
    static constexpr unsigned shardBits = 6;
    static constexpr size_t initialTableSize = 16;

    struct Node {
        Node(uint32_t hash, Entry&& entry)
            : hash(hash)
            , entry(WTF::move(entry))
        {
        }

        uint32_t hash;
        Entry entry;
    };

    struct Table {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Table);

        explicit Table(size_t size)
            : slots(size)
        {
            for (auto& slot : slots)
                slot.store(nullptr, std::memory_order_relaxed);
        }

        const Entry* find(uint32_t hash, NOESCAPE const Invocable<bool(const Entry&)> auto& isWanted) const
        {
            size_t mask = slots.size() - 1;
            for (size_t index = hash & mask; const Node* node = slots[index].load(std::memory_order_acquire); index = (index + 1) & mask) {
                if (node->hash == hash && isWanted(node->entry))
                    return &node->entry;
            }
            return nullptr;
        }

        void insert(const Node& node)
        {
            size_t mask = slots.size() - 1;
            size_t index = node.hash & mask;
            while (slots[index].load(std::memory_order_relaxed))
                index = (index + 1) & mask;
            slots[index].store(&node, std::memory_order_release);
        }

        Vector<std::atomic<const Node*>> slots;
    };

    struct Shard {
        const Entry* find(uint32_t hash, NOESCAPE const Invocable<bool(const Entry&)> auto& isWanted) const
        {
            const Table* current = table.load(std::memory_order_acquire);
            return current ? current->find(hash, isWanted) : nullptr;
        }

        const Entry& add(uint32_t hash, Entry&& entry, NOESCAPE const Invocable<bool(const Entry&)> auto& isWanted)
        {
            Locker locker { lock };
            Table* current = table.load(std::memory_order_relaxed);
            if (current) {
                if (const Entry* existing = current->find(hash, isWanted))
                    return *existing;
            }
            if (!current || (nodes.size() + 1) * 2 > current->slots.size())
                current = &grow();
            Node& node = nodes.alloc(hash, WTF::move(entry));
            current->insert(node);
            return node.entry;
        }

        Table& grow() WTF_REQUIRES_LOCK(lock)
        {
            auto larger = makeUnique<Table>(tables.isEmpty() ? initialTableSize : tables.last()->slots.size() * 2);
            for (Node& node : nodes)
                larger->insert(node);
            tables.append(WTF::move(larger));
            table.store(tables.last().get(), std::memory_order_release);
            return *tables.last();
        }

        Lock lock;
        std::atomic<Table*> table { nullptr };
        Vector<std::unique_ptr<Table>> tables WTF_GUARDED_BY_LOCK(lock);
        SegmentedVector<Node, 16> nodes WTF_GUARDED_BY_LOCK(lock);
    };

    static constexpr unsigned shardIndexOf(uint32_t hash) { return hash >> (32 - shardBits); }

    std::array<Shard, 1u << shardBits> m_shards;
};

inline uint32_t hashOfLayouts(std::span<const LayoutID> layouts)
{
    uint64_t hash = layouts.size();
    for (LayoutID layout : layouts)
        hash = (hash ^ layout.index()) * 0x9E3779B97F4A7C15ull;
    return static_cast<uint32_t>(hash >> 32);
}

class SharedLayoutLists {
    WTF_MAKE_NONCOPYABLE(SharedLayoutLists);
public:
    using ListID = uint64_t;

    SharedLayoutLists() = default;

    ListID idOf(std::span<const LayoutID> layouts)
    {
        uint32_t hash = hashOfLayouts(layouts);
        auto isWanted = [&](const List& list) {
            return std::ranges::equal(list.layouts.span(), layouts);
        };
        if (const List* known = m_lists.find(hash, isWanted))
            return known->id;
        ListID id = firstContentID + m_numberOfLists.fetch_add(1, std::memory_order_relaxed);
        return m_lists.add(hash, List { Vector<LayoutID>(layouts), id }, isWanted).id;
    }

    ListID idOf(const TypeTable& types, LayoutSetID set)
    {
        if (!TypeTable::isProvisional(set))
            return set.index();

        uint64_t phase = TypeInterningPhase::currentPhaseSerialNumber(types);
        uint32_t hash = set.hash();
        auto isWanted = [&](const ProvisionalSet& known) {
            return known.set == set && known.phase == phase;
        };
        if (const ProvisionalSet* known = m_provisionalSets.find(hash, isWanted))
            return known->list;
        return m_provisionalSets.add(hash, ProvisionalSet { set, phase, idOf(types.at(set).written().span()) }, isWanted).list;
    }

private:
    static constexpr ListID firstContentID = 1ull << 32;

    struct List {
        Vector<LayoutID> layouts;
        ListID id { 0 };
    };

    struct ProvisionalSet {
        LayoutSetID set;
        uint64_t phase { 0 };
        ListID list { 0 };
    };

    SharedMemo<List> m_lists;
    SharedMemo<ProvisionalSet> m_provisionalSets;
    std::atomic<uint64_t> m_numberOfLists { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
