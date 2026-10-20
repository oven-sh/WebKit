/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

// A counting sort. The entries of group g are entries[firstEntry[g]] up to entries[firstEntry[g + 1]], in the order they were visited in.
// forEachEntry runs twice. It calls its argument with (group, entry), for the same entries in the same order both times.
template<typename T, typename ForEachEntry>
void groupByIndex(size_t numberOfGroups, Vector<uint32_t>& firstEntry, Vector<T>& entries, NOESCAPE const ForEachEntry& forEachEntry)
{
    firstEntry.fill(0, numberOfGroups + 1);
    forEachEntry([&](uint32_t group, const T&) {
        ++firstEntry[group + 1];
    });
    for (size_t group = 0; group < numberOfGroups; ++group)
        firstEntry[group + 1] += firstEntry[group];

    entries.resize(firstEntry.last());
    Vector<uint32_t> nextEntry = firstEntry;
    forEachEntry([&](uint32_t group, const T& entry) {
        entries[nextEntry[group]++] = entry;
    });
}

template<typename T>
std::span<const T> entriesOfGroup(const Vector<uint32_t>& firstEntry, const Vector<T>& entries LIFETIME_BOUND, uint32_t group)
{
    uint32_t first = firstEntry[group];
    return entries.subspan(first, firstEntry[group + 1] - first);
}

template<typename Record, typename Key, typename Projection>
std::span<const Record> recordsWith(std::span<const Record> sorted, const Key& key, const Projection& projection)
{
    auto range = std::ranges::equal_range(sorted, key, { }, projection);
    return sorted.subspan(range.begin() - sorted.begin(), range.size());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
