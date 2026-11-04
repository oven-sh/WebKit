/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <span>

namespace JSC { namespace AOT {

// A counting sort.
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
