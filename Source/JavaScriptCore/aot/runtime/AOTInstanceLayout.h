/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include <algorithm>
#include <limits>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

struct InstanceSizes {
    uint32_t sizeOfStatics;
    uint32_t numberOfStringCells;
    uint32_t numberOfSiteCaches;
};
static_assert(sizeof(InstanceSizes) == 12 && isImageRecord<InstanceSizes>);

struct InstanceLayout {
    static constexpr size_t bytesReservedForPrefixAndPollPage = 1 * MB;
    static constexpr size_t bytesReservedForLoopPollPage = 16 * KB;
    static constexpr size_t granuleBelowInstance = 64 * KB;
    static constexpr size_t granuleAboveInstance = 16 * KB;
    static constexpr unsigned cardShift = 8;
    static constexpr size_t sizeOfStringCell = sizeof(uint32_t);
    static constexpr size_t sizeOfSiteCache = 16;
    static constexpr size_t offsetOfStringCells = 64 * KB;

    static constexpr ptrdiff_t offsetOfLoopPollPage() { return -static_cast<ptrdiff_t>(bytesReservedForPrefixAndPollPage); }
    static constexpr ptrdiff_t offsetOfStatics(InstanceSizes sizes) { return offsetOfLoopPollPage() - static_cast<ptrdiff_t>(bytesReservedForStatics(sizes)); }
    static constexpr ptrdiff_t offsetOfCards(InstanceSizes sizes) { return offsetOfStatics(sizes) - static_cast<ptrdiff_t>(bytesReservedForStatics(sizes) >> cardShift); }
    static constexpr size_t bytesReservedBelowInstance(InstanceSizes sizes)
    {
        size_t statics = bytesReservedForStatics(sizes);
        return bytesReservedForPrefixAndPollPage + statics + roundUpToMultipleOf<granuleBelowInstance>(statics >> cardShift);
    }

    static constexpr size_t offsetOfStringCell(uint32_t number) { return offsetOfStringCells + number * sizeOfStringCell; }
    static constexpr size_t offsetOfSiteCaches(InstanceSizes sizes) { return offsetOfStringCells + roundUpToMultipleOf<granuleAboveInstance>(sizes.numberOfStringCells * sizeOfStringCell); }
    static constexpr size_t offsetOfSiteCache(InstanceSizes sizes, uint32_t number) { return offsetOfSiteCaches(sizes) + number * sizeOfSiteCache; }
    static constexpr size_t bytesReservedFromInstance(InstanceSizes sizes) { return offsetOfSiteCaches(sizes) + roundUpToMultipleOf<granuleAboveInstance>(sizes.numberOfSiteCaches * sizeOfSiteCache); }

    static constexpr bool isAddressable(InstanceSizes sizes)
    {
        return bytesReservedFromInstance(sizes) <= static_cast<size_t>(std::numeric_limits<int32_t>::max())
            && bytesReservedBelowInstance(sizes) <= static_cast<size_t>(std::numeric_limits<int32_t>::max());
    }

private:
    static constexpr size_t bytesReservedForStatics(InstanceSizes sizes) { return roundUpToMultipleOf<granuleBelowInstance>(std::max<size_t>(sizes.sizeOfStatics, 1)); }
};

static_assert(!(InstanceLayout::bytesReservedForPrefixAndPollPage % InstanceLayout::granuleBelowInstance));
static_assert(!(InstanceLayout::offsetOfStringCells % InstanceLayout::granuleAboveInstance));
static_assert(!(InstanceLayout::granuleAboveInstance % InstanceLayout::sizeOfSiteCache));
static_assert(InstanceLayout::offsetOfSiteCaches({ 0, 1, 0 }) == InstanceLayout::offsetOfStringCells + InstanceLayout::granuleAboveInstance);
static_assert(InstanceLayout::offsetOfCards({ 0, 0, 0 }) < InstanceLayout::offsetOfStatics({ 0, 0, 0 }) && InstanceLayout::offsetOfStatics({ 0, 0, 0 }) < InstanceLayout::offsetOfLoopPollPage(), "The collector reads a word of cards even when the statics are empty, and the loop poll page can be inaccessible.");
static_assert(-InstanceLayout::offsetOfCards({ 1, 0, 0 }) <= static_cast<ptrdiff_t>(InstanceLayout::bytesReservedBelowInstance({ 1, 0, 0 })));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
