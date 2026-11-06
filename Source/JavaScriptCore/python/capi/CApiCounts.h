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

// How many references C owns, for each cell of which it owns any.
//
// A function that returns a new reference adds one, Py_DECREF and a function that steals a reference take one away, and with the last one the
// entry goes. Nothing else happens then: the object lives on if the program refers to it, and a collection finds out if not.
//
// WHEN A COLLECTION HAS MARKED ALL IT WILL, EVERY KEY OF THE TABLE IS MARKED (markWhatCHolds(), asserted in didFinishMarking()). So no
// key is the address of a cell that has been freed, whatever an extension does or fails to do.
//
// The program changes the table with the GIL (retain() and release() refuse a thread without it). The collector reads it with the world stopped, and
// at no other time. The thread that runs the program is never stopped from outside: it stops where it allocates in the heap or gives up its access to
// it, and no function here does either. So the collector never sees the table in the middle of a change, and there is no lock.
class CountTable {
    WTF_MAKE_NONCOPYABLE(CountTable);
public:
    CountTable() = default;

    struct Counts {
        uint32_t owned { 0 };
        // It is a tuple that PyTuple_New() has made, and C has not given it away since: see PyTuple_SetItem().
        bool isBeingFilled { false };
    };

    ALWAYS_INLINE void add(JSCell* cell)
    {
        Counts& counts = m_counts.add(cell, Counts { }).iterator->value;
        if (counts.owned != forEver) [[likely]]
            ++counts.owned;
    }

    ALWAYS_INLINE void remove(JSCell* cell)
    {
        auto entry = m_counts.find(cell);
        // More references were given back than taken. In CPython the object would have been freed under its owners. Here nothing hangs on it.
        if (entry == m_counts.end()) [[unlikely]]
            return;
        if (entry->value.owned == forEver) [[unlikely]]
            return;
        if (!--entry->value.owned)
            m_counts.remove(entry);
    }

    // The object goes with the reference: somebody else may see it from now on.
    ALWAYS_INLINE void giveAway(JSCell* cell, bool countFalls)
    {
        auto entry = m_counts.find(cell);
        if (entry == m_counts.end()) [[unlikely]]
            return;
        entry->value.isBeingFilled = false;
        if (!countFalls || entry->value.owned == forEver) [[unlikely]]
            return;
        if (!--entry->value.owned)
            m_counts.remove(entry);
    }

    void clear() { m_counts.clear(); }
    uint32_t get(JSCell* cell) const { return m_counts.get(cell).owned; }
    unsigned size() const { return m_counts.size(); }
    // Null: C owns no reference to it.
    Counts* find(JSCell* cell)
    {
        auto entry = m_counts.find(cell);
        return entry == m_counts.end() ? nullptr : &entry->value;
    }

    template<typename Function>
    void forEachCell(const Function& function) const
    {
        for (JSCell* cell : m_counts.keys())
            function(cell);
    }

private:
    // A count that has got this far is no longer kept.
    static constexpr uint32_t forEver = std::numeric_limits<uint32_t>::max();

    UncheckedKeyHashMap<JSCell*, Counts> m_counts;
};

} // namespace CApi
} // namespace Python
} // namespace JSC
