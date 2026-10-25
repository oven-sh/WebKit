/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <optional>
#include <wtf/HashFunctions.h>
#include <wtf/HashTraits.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace WTF {

// A hash map whose entries are grouped into layers, where dropping every entry added since the
// most recent startLayer() costs only as much as the number of those entries. A client that pairs
// startLayer()/dropLastLayer() with descent and ascent of the dominator tree therefore holds
// exactly the entries defined in dominating blocks, so a lookup needs no dominance test.
//
// add() and findOrAdd() never replace a present entry.
// overlay() shadows any existing entry until the current layer is dropped.
template<typename KeyArg, typename MappedArg, typename HashArg = DefaultHash<KeyArg>, typename KeyTraitsArg = HashTraits<KeyArg>, typename MappedTraitsArg = HashTraits<MappedArg>>
class LayeredHashMap {
    WTF_MAKE_NONCOPYABLE(LayeredHashMap);
public:
    using KeyType = KeyArg;
    using MappedType = MappedArg;
    using HashFunctions = HashArg;
    using KeyTraits = KeyTraitsArg;
    using MappedTraits = MappedTraitsArg;

    LayeredHashMap()
    {
        static_assert(KeyTraits::minimumTableSize > 0);
        static_assert(hasOneBitSet(KeyTraits::minimumTableSize));
        m_table.fill(emptyIndex, KeyTraits::minimumTableSize);
        m_mask = KeyTraits::minimumTableSize - 1;
    }

    void startLayer()
    {
        m_layerStarts.append(m_entries.size());
    }

    void dropLastLayer()
    {
        ASSERT(layerCount());
        unsigned start = m_layerStarts.takeLast();
        unsigned* table = m_table.mutableSpan().data();
        const Entry* entries = m_entries.span().data();
        for (unsigned index = m_entries.size(); index-- > start;)
            table[entries[index].hash & m_mask] = entries[index].next;
        m_entries.shrink(start);
    }

    unsigned layerCount() const { return m_layerStarts.size(); }

    void clearEntries()
    {
        if (m_entries.isEmpty())
            return;
        m_table.fill(emptyIndex);
        m_entries.shrink(0);
        m_layerStarts.fill(0);
    }

    void clear()
    {
        if (!m_entries.isEmpty())
            m_table.fill(emptyIndex);
        m_entries.shrink(0);
        m_layerStarts.shrink(0);
    }

    std::optional<MappedType> find(const KeyType& key)
    {
        unsigned index = indexOf(key, HashFunctions::hash(key));
        if (index == emptyIndex)
            return std::nullopt;
        return m_entries.span().data()[index].value;
    }

    std::optional<MappedType> findOrAdd(const KeyType& key, const MappedType& value)
    {
        ASSERT(layerCount());
        unsigned hash = HashFunctions::hash(key);
        unsigned index = indexOf(key, hash);
        if (index != emptyIndex)
            return m_entries.span().data()[index].value;
        append(key, value, hash);
        return std::nullopt;
    }

    bool add(const KeyType& key, const MappedType& value)
    {
        return !findOrAdd(key, value);
    }

    void overlay(const KeyType& key, const MappedType& value)
    {
        ASSERT(layerCount());
        unsigned hash = HashFunctions::hash(key);
        unsigned index = indexOf(key, hash);
        if (index != emptyIndex && index >= m_layerStarts.last()) {
            m_entries.mutableSpan().data()[index].value = value;
            return;
        }
        append(key, value, hash);
    }

private:
    static constexpr unsigned emptyIndex = UINT_MAX;

    struct Entry {
        KeyType key;
        MappedType value;
        unsigned hash;
        unsigned next;
    };

    ALWAYS_INLINE unsigned indexOf(const KeyType& key, unsigned hash) const
    {
        const Entry* entries = m_entries.span().data();
        for (unsigned index = m_table.span().data()[hash & m_mask]; index != emptyIndex; index = entries[index].next) {
            if (entries[index].hash == hash && HashFunctions::equal(entries[index].key, key))
                return index;
        }
        return emptyIndex;
    }

    ALWAYS_INLINE void append(const KeyType& key, const MappedType& value, unsigned hash)
    {
        unsigned index = m_entries.size();
        if (index > m_mask) [[unlikely]]
            grow();
        unsigned& head = m_table.mutableSpan().data()[hash & m_mask];
        m_entries.append(Entry { key, value, hash, head });
        head = index;
    }

    NEVER_INLINE void grow()
    {
        unsigned capacity = m_table.size() * 4;
        m_table.fill(emptyIndex, capacity);
        m_mask = capacity - 1;
        m_entries.reserveCapacity(capacity);
        for (unsigned index = 0; index < m_entries.size(); ++index) {
            Entry& entry = m_entries[index];
            entry.next = std::exchange(m_table[entry.hash & m_mask], index);
        }
    }

    unsigned m_mask { 0 };
    Vector<unsigned> m_table;
    Vector<Entry> m_entries;
    Vector<unsigned, 16> m_layerStarts;
};

} // namespace WTF

using WTF::LayeredHashMap;
