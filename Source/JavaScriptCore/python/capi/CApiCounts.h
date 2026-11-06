/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCell.h"
#include <wtf/HashMap.h>

namespace JSC {
namespace Python {
namespace CApi {

// How many references C owns, for each cell of which it owns any (or has owned any, once nothing is let go of any more: see remove()).
//
// A function that returns a new reference adds one, Py_DECREF and a function that steals a reference take one away, and with the last one the
// entry goes (as long as anything is let go of: see remove()). Nothing else happens then: the object lives on if the program refers to it, and a collection finds out if not.
//
// WHEN A COLLECTION HAS MARKED ALL IT WILL, EVERY KEY OF THE TABLE IS MARKED (markWhatCHolds(), asserted in didFinishMarking()). So no
// key is the address of a cell that has been freed, whatever an extension does or fails to do.
//
// The program changes the table with the GIL (retain() and release() refuse a thread without it). The collector reads it with the world stopped, and at
// no other time; so it empties it, if it is the collector that ends the interpreter (runtimeIsEnding()). The thread that runs the program is never
// stopped from outside: it stops where it allocates in the heap or gives up its access to it, and no function here does either. So the collector never
// sees the table in the middle of a change, and there is no lock.
class CountTable {
    WTF_MAKE_NONCOPYABLE(CountTable);
public:
    CountTable() = default;

    struct Counts {
        uint32_t owned { 0 };
        // For the collector, while it evaluates: how many of them tp_traverse has accounted for. See markWhatCHolds().
        uint32_t accountedFor { 0 };
    };

    ALWAYS_INLINE void add(JSCell* cell)
    {
        Counts& counts = m_counts.add(cell, Counts { }).iterator->value;
        if (counts.owned != forEver) [[likely]]
            ++counts.owned;
    }

    // `letsGo`: the entry goes with the last reference. If not, it stays, with a count of 0: a key is what the collector marks, so the object is held, and the number is still what C owns.
    ALWAYS_INLINE void remove(JSCell* cell, bool letsGo)
    {
        auto entry = m_counts.find(cell);
        // More references were given back than taken. In CPython the object would have been freed under its owners. Here nothing hangs on it.
        if (entry == m_counts.end() || !entry->value.owned) [[unlikely]]
            return;
        if (entry->value.owned == forEver) [[unlikely]]
            return;
        if (!--entry->value.owned && letsGo) [[likely]]
            m_counts.remove(entry);
    }

    void clear() { m_counts.clear(); }
    void forget(JSCell* cell) { m_counts.remove(cell); }
    uint32_t get(JSCell* cell) const { return m_counts.get(cell).owned; }
    // Whether it is a key: then it is a cell, and lives. (The count may be 0: see remove().)
    bool contains(JSCell* cell) const { return m_counts.contains(cell); }
    unsigned size() const { return m_counts.size(); }
    // Null: C owns no reference to it.
    Counts* find(JSCell* cell)
    {
        auto entry = m_counts.find(cell);
        return entry == m_counts.end() ? nullptr : &entry->value;
    }

    template<typename Function>
    void forEach(const Function& function)
    {
        for (auto& entry : m_counts)
            function(entry.key, entry.value);
    }

private:
    // A count that has got this far is no longer kept.
    static constexpr uint32_t forEver = std::numeric_limits<uint32_t>::max();

    struct CountsTraits : WTF::GenericHashTraits<Counts> {
        static constexpr bool emptyValueIsZero = true;
    };
    UncheckedKeyHashMap<JSCell*, Counts, DefaultHash<JSCell*>, HashTraits<JSCell*>, CountsTraits> m_counts;
};

} // namespace CApi
} // namespace Python
} // namespace JSC
