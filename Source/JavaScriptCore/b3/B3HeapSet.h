/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3HeapRange.h"
#include <span>
#include <wtf/EmbeddedFixedVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/RangeSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UniqueRef.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class HeapSetTable;

class HeapSet {
public:
    using RangeList = EmbeddedFixedVector<HeapRange>;

    constexpr HeapSet() = default;

    constexpr HeapSet(const HeapRange& range)
        : m_bounds(range)
    {
    }

    static constexpr HeapSet top() { return HeapSet(HeapRange::top()); }

    explicit constexpr operator bool() const { return !!m_bounds; }

    constexpr const HeapRange& bounds() const LIFETIME_BOUND { return m_bounds; }
    constexpr operator const HeapRange&() const LIFETIME_BOUND { return m_bounds; }

    bool isSingleRange() const { return !m_ranges; }

    std::span<const HeapRange> ranges() const LIFETIME_BOUND
    {
        if (m_ranges)
            return m_ranges->span();
        if (!m_bounds)
            return { };
        return singleElementSpan(m_bounds);
    }

    bool overlaps(const HeapRange& range) const
    {
        if (!m_bounds.overlaps(range))
            return false;
        return !m_ranges || overlapsSlow(range);
    }

    bool overlaps(const HeapSet& other) const
    {
        if (!m_bounds.overlaps(other.m_bounds))
            return false;
        if (!m_ranges)
            return other.overlaps(m_bounds);
        if (!other.m_ranges)
            return overlapsSlow(other.m_bounds);
        return overlapsSlow(other);
    }

    bool overlaps(const RangeSet<HeapRange>& set) const
    {
        for (const HeapRange& range : ranges()) {
            if (set.overlaps(range))
                return true;
        }
        return false;
    }

    JS_EXPORT_PRIVATE bool isSubsetOf(const HeapSet&) const;

    void addTo(RangeSet<HeapRange>& set) const
    {
        for (const HeapRange& range : ranges())
            set.add(range);
    }

    friend constexpr bool operator==(const HeapSet& a, const HeapSet& b) { return a.m_bounds == b.m_bounds && a.m_ranges == b.m_ranges; }
    friend constexpr bool operator==(const HeapSet& a, const HeapRange& b) { return !a.m_ranges && a.m_bounds == b; }

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    friend class HeapSetTable;

    HeapSet(const HeapRange& bounds, const RangeList* ranges)
        : m_bounds(bounds)
        , m_ranges(ranges)
    {
    }

    JS_EXPORT_PRIVATE bool overlapsSlow(const HeapRange&) const;
    JS_EXPORT_PRIVATE bool overlapsSlow(const HeapSet&) const;

    HeapRange m_bounds;
    const RangeList* m_ranges { nullptr };
};

class HeapSetTable {
    WTF_MAKE_NONCOPYABLE(HeapSetTable);
public:
    HeapSetTable() = default;

    JS_EXPORT_PRIVATE HeapSet create(std::span<const HeapRange>);
    JS_EXPORT_PRIVATE HeapSet unite(const HeapSet&, const HeapSet&);

    unsigned numberOfLists() const { return m_lists.size(); }

private:
    HeapSet intern(Vector<HeapRange, 8>&& sortedDisjointRanges);

    Vector<UniqueRef<HeapSet::RangeList>> m_lists;
    UncheckedKeyHashMap<unsigned, Vector<unsigned, 1>, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_listsByHash;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
