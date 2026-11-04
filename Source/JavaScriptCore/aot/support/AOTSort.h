/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <ranges>
#include <utility>
#include <wtf/Compiler.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

inline constexpr std::ptrdiff_t largestRangeSortedByInsertion = 32;

template<std::ranges::random_access_range Range, typename IsLess, typename Projection>
void sortKeepingEqualElementsInOrder(Range&& range, IsLess isLess, Projection projection)
{
    auto first = std::ranges::begin(range);
    auto last = std::ranges::next(first, std::ranges::end(range));
    auto next = std::ranges::is_sorted_until(first, last, isLess, projection);
    if (next == last)
        return;
    if (last - first > largestRangeSortedByInsertion) {
        std::ranges::stable_sort(first, last, isLess, projection);
        return;
    }
    for (; next != last; ++next) {
        if (!std::invoke(isLess, std::invoke(projection, *next), std::invoke(projection, *std::ranges::prev(next))))
            continue;
        std::iter_value_t<decltype(first)> element = std::ranges::iter_move(next);
        auto place = next;
        do {
            *place = std::ranges::iter_move(std::ranges::prev(place));
            --place;
        } while (place != first && std::invoke(isLess, std::invoke(projection, element), std::invoke(projection, *std::ranges::prev(place))));
        *place = std::move(element);
    }
}

// All four leave equal elements in the order they came in, so that libstdc++ and libc++ give the same bytes wherever a sorted list reaches the image.
// That does not make an order total: elements that arrive in thread order still need a key that tells them all apart.

template<std::ranges::random_access_range Range, typename IsLess = std::ranges::less, typename Projection = std::identity>
void sort(Range&& range, IsLess isLess = { }, Projection projection = { })
{
    sortKeepingEqualElementsInOrder(range, isLess, projection);
}

template<std::ranges::random_access_range Range, typename IsLess = std::ranges::less, typename Projection = std::identity>
void sortInLine(Range&& range, IsLess isLess = { }, Projection projection = { })
{
    sortKeepingEqualElementsInOrder(range, isLess, projection);
}

template<std::ranges::random_access_range Range, typename IsLess = std::ranges::less, typename Projection = std::identity>
void stableSort(Range&& range, IsLess isLess = { }, Projection projection = { })
{
    sortKeepingEqualElementsInOrder(range, isLess, projection);
}

template<std::ranges::random_access_range Range, typename IsLess = std::ranges::less, typename Projection = std::identity>
void stableSortInLine(Range&& range, IsLess isLess = { }, Projection projection = { })
{
    sortKeepingEqualElementsInOrder(range, isLess, projection);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
