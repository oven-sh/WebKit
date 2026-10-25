/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3HeapSet.h"

#if ENABLE(B3_JIT)

#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/HashFunctions.h>

namespace JSC { namespace B3 {

static const HeapRange* findFirstRangeEndingAfter(std::span<const HeapRange> ranges, uint64_t point)
{
    return std::partition_point(ranges.data(), ranges.data() + ranges.size(), [&](const HeapRange& range) {
        return range.end() <= point;
    });
}

bool HeapSet::overlapsSlow(const HeapRange& range) const
{
    std::span<const HeapRange> ranges = m_ranges->span();
    const HeapRange* candidate = findFirstRangeEndingAfter(ranges, range.begin());
    return candidate != ranges.data() + ranges.size() && candidate->begin() < range.end();
}

bool HeapSet::overlapsSlow(const HeapSet& other) const
{
    std::span<const HeapRange> left = m_ranges->span();
    std::span<const HeapRange> right = other.m_ranges->span();
    size_t leftIndex = 0;
    size_t rightIndex = 0;
    while (leftIndex < left.size() && rightIndex < right.size()) {
        if (left[leftIndex].overlaps(right[rightIndex]))
            return true;
        if (left[leftIndex].end() <= right[rightIndex].end())
            ++leftIndex;
        else
            ++rightIndex;
    }
    return false;
}

bool HeapSet::isSubsetOf(const HeapSet& other) const
{
    std::span<const HeapRange> otherRanges = other.ranges();
    for (const HeapRange& range : ranges()) {
        const HeapRange* candidate = findFirstRangeEndingAfter(otherRanges, range.begin());
        if (candidate == otherRanges.data() + otherRanges.size() || candidate->begin() > range.begin() || candidate->end() < range.end())
            return false;
    }
    return true;
}

void HeapSet::dump(PrintStream& out) const
{
    if (!m_ranges) {
        out.print(m_bounds);
        return;
    }
    CommaPrinter comma;
    out.print("{"_s);
    for (const HeapRange& range : *m_ranges)
        out.print(comma, range);
    out.print("}"_s);
}

HeapSet HeapSetTable::create(std::span<const HeapRange> ranges)
{
    Vector<HeapRange, 8> sorted;
    for (const HeapRange& range : ranges) {
        if (range)
            sorted.append(range);
    }
    std::ranges::sort(sorted, { }, &HeapRange::begin);

    Vector<HeapRange, 8> merged;
    for (const HeapRange& range : sorted) {
        if (!merged.isEmpty() && range.begin() <= merged.last().end())
            merged.last() |= range;
        else
            merged.append(range);
    }
    if (merged.isEmpty())
        return HeapSet();
    if (merged.size() == 1)
        return HeapSet(merged[0]);
    return intern(WTF::move(merged));
}

HeapSet HeapSetTable::unite(const HeapSet& a, const HeapSet& b)
{
    if (!a || a == b)
        return b;
    if (!b)
        return a;
    Vector<HeapRange, 8> ranges;
    ranges.append(a.ranges());
    ranges.append(b.ranges());
    return create(ranges.span());
}

HeapSet HeapSetTable::intern(Vector<HeapRange, 8>&& ranges)
{
    unsigned hash = 0;
    for (const HeapRange& range : ranges)
        hash = WTF::pairIntHash(hash, WTF::pairIntHash(WTF::intHash(range.begin()), WTF::intHash(range.end())));
    if (hash >= std::numeric_limits<unsigned>::max() - 1)
        hash = 0;

    HeapRange bounds(ranges.first().begin(), ranges.last().end());
    Vector<unsigned, 1>& candidates = m_listsByHash.add(hash, Vector<unsigned, 1>()).iterator->value;
    for (unsigned index : candidates) {
        if (std::ranges::equal(m_lists[index]->span(), ranges.span()))
            return HeapSet(bounds, m_lists[index].ptr());
    }
    candidates.append(m_lists.size());
    m_lists.append(HeapSet::RangeList::create(ranges.span()));
    return HeapSet(bounds, m_lists.last().ptr());
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
