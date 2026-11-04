/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <span>

namespace JSC { namespace AOT {

// What std::upper_bound answers, as an index: the first element that the key lies before, or the size of the list.
template<typename T, typename Key, typename IsBefore>
size_t upperBoundFrom(size_t& position, std::span<const T> ascending, const Key& key, const IsBefore& isBefore)
{
    size_t low = position;
    if (low && isBefore(key, ascending[low - 1])) {
        position = std::upper_bound(ascending.begin(), ascending.begin() + (low - 1), key, isBefore) - ascending.begin();
        return position;
    }
    // The answer does not lie in front of `low`. A key that does not lie before the last of the next `width` elements lies before none of them.
    size_t width = 1;
    while (width <= ascending.size() - low && !isBefore(key, ascending[low + width - 1])) {
        low += width;
        width *= 2;
    }
    auto first = ascending.begin() + low;
    position = std::upper_bound(first, first + std::min(width - 1, ascending.size() - low), key, isBefore) - ascending.begin();
    return position;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
