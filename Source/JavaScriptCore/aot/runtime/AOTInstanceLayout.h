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

// In a hardened engine the table of boxed calls has pages of its own, which are read-only. On ARM64 the instance ends below the loop poll page without it.
#if defined(BUN_HARDENED) || CPU(ARM64)
#define AOT_BOXED_CALLS_LIE_BELOW_INSTANCE 1
#else
#define AOT_BOXED_CALLS_LIE_BELOW_INSTANCE 0
#endif

namespace JSC { namespace AOT {

struct InstanceSizes {
    uint32_t sizeOfStatics;
    uint32_t numberOfStringCells;
    uint32_t numberOfSiteCaches;
    uint32_t sizeOfMethodTable;
};
static_assert(sizeof(InstanceSizes) == 16 && isImageRecord<InstanceSizes>);

struct InstanceLayout {
    static constexpr size_t bytesReservedForPrefixAndPollPage = 1 * MB;
    static constexpr size_t bytesReservedForLoopPollPage = 16 * KB;
    static constexpr size_t granuleBelowInstance = 64 * KB;
    static constexpr size_t granuleAboveInstance = 16 * KB;
    static constexpr unsigned cardShift = 8;
    static constexpr size_t sizeOfStringCell = sizeof(uint32_t);
    // One card says whether a cell was written among this many bytes of string cells.
    static constexpr unsigned stringCellCardShift = 7;
    static constexpr size_t sizeOfSiteCache = 16;
    static constexpr size_t offsetOfStringCells = 384 * KB;
#if defined(BUN_HARDENED)
    // From here to the key order steps: what nothing writes once the instance has been created. The pages are read-only from then on.
    static constexpr size_t offsetOfWordsWrittenOnce = 16 * KB;
#endif
#if AOT_BOXED_CALLS_LIE_BELOW_INSTANCE
    // The table of boxed calls, with room for an entry for every number that a byte holds. In a hardened engine it is read-only once the instance has been created.
    static constexpr ptrdiff_t offsetOfBoxedCalls = -static_cast<ptrdiff_t>(32 * KB);
    static constexpr size_t bytesReservedForBoxedCalls = 16 * KB;
#endif
    // KeyedValuePlace. A stub reaches a place by its distance from the instance.
    static constexpr ptrdiff_t offsetOfKeyedValuePlaces = -static_cast<ptrdiff_t>(80 * KB);
    static constexpr size_t bytesReservedForKeyedValuePlaces = 32 * KB;
    // AllocationWords::numberOfCopiesWithoutFrame pointers, reached the same way.
    static constexpr ptrdiff_t offsetOfCopiesWithoutFrame = -static_cast<ptrdiff_t>(48 * KB);
    static constexpr size_t bytesReservedForCopiesWithoutFrame = 16 * KB;
    // Between the instance and the string cells.
    static constexpr size_t offsetOfKeyOrderSteps = 32 * KB;
    // Small tables of which compiled code reads one word at a time.
    static constexpr size_t offsetOfBuiltinLocationStates = 64 * KB;
    static constexpr size_t offsetOfGlobalBindings = 80 * KB;
    static constexpr size_t offsetOfBuiltinCells = 128 * KB;
    static constexpr size_t offsetOfBuiltinValues = 256 * KB;

#if CPU(ARM64) && !defined(BUN_HARDENED)
    // A poll is one load, and the immediate offset of a load reaches from the instance to 32 KB above it: the page lies between the instance and the key order steps.
    static constexpr ptrdiff_t offsetOfLoopPollPage() { return static_cast<ptrdiff_t>(offsetOfKeyOrderSteps - bytesReservedForLoopPollPage); }
#else
    static constexpr ptrdiff_t offsetOfLoopPollPage() { return -static_cast<ptrdiff_t>(bytesReservedForPrefixAndPollPage); }
#endif
    static constexpr ptrdiff_t offsetOfStatics(InstanceSizes sizes) { return -static_cast<ptrdiff_t>(bytesReservedForPrefixAndPollPage + bytesReservedForStatics(sizes)); }
    static constexpr ptrdiff_t offsetOfCards(InstanceSizes sizes) { return offsetOfStatics(sizes) - static_cast<ptrdiff_t>(bytesReservedForStatics(sizes) >> cardShift); }
    static constexpr size_t bytesReservedBelowInstance(InstanceSizes sizes)
    {
        size_t statics = bytesReservedForStatics(sizes);
        return bytesReservedForPrefixAndPollPage + statics + roundUpToMultipleOf<granuleBelowInstance>(statics >> cardShift);
    }

    static constexpr bool holdsBuiltinLocationStates(size_t bytes) { return bytes <= offsetOfGlobalBindings - offsetOfBuiltinLocationStates; }
    static constexpr bool holdsGlobalBindings(size_t bytes) { return bytes <= offsetOfBuiltinCells - offsetOfGlobalBindings; }
    // A value has the number of its key, as a cell has, so each of the two tables has a word for every key.
    static constexpr bool holdsBuiltinCells(size_t numberOfKeys) { return numberOfKeys * sizeof(void*) <= offsetOfBuiltinValues - offsetOfBuiltinCells; }

    static constexpr size_t offsetOfStringCell(uint32_t number) { return offsetOfStringCells + number * sizeOfStringCell; }
    static constexpr size_t offsetOfSiteCaches(InstanceSizes sizes) { return offsetOfStringCells + roundUpToMultipleOf<granuleAboveInstance>(sizes.numberOfStringCells * sizeOfStringCell); }
    static constexpr size_t offsetOfSiteCache(InstanceSizes sizes, uint32_t number) { return offsetOfSiteCaches(sizes) + number * sizeOfSiteCache; }
    // The instance's own copy of the method table: a copy-on-write clone of the section of the image, so it begins at a page.
    static constexpr size_t offsetOfMethodTable(InstanceSizes sizes) { return offsetOfSiteCaches(sizes) + roundUpToMultipleOf<granuleAboveInstance>(sizes.numberOfSiteCaches * sizeOfSiteCache); }
    static constexpr size_t bytesReservedForMethodTable(InstanceSizes sizes) { return roundUpToMultipleOf<granuleAboveInstance>(sizes.sizeOfMethodTable); }
    static constexpr size_t bytesReservedFromInstance(InstanceSizes sizes) { return offsetOfMethodTable(sizes) + bytesReservedForMethodTable(sizes); }

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
#if defined(BUN_HARDENED)
static_assert(!(InstanceLayout::offsetOfWordsWrittenOnce % InstanceLayout::granuleAboveInstance) && !(InstanceLayout::offsetOfKeyOrderSteps % InstanceLayout::granuleAboveInstance));
#endif
#if AOT_BOXED_CALLS_LIE_BELOW_INSTANCE
static_assert(!(InstanceLayout::offsetOfBoxedCalls % static_cast<ptrdiff_t>(InstanceLayout::granuleAboveInstance)) && !(InstanceLayout::bytesReservedForBoxedCalls % InstanceLayout::granuleAboveInstance));
static_assert(InstanceLayout::offsetOfCopiesWithoutFrame + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForCopiesWithoutFrame) <= InstanceLayout::offsetOfBoxedCalls);
#endif
static_assert(-static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForPrefixAndPollPage - InstanceLayout::bytesReservedForLoopPollPage) <= InstanceLayout::offsetOfKeyedValuePlaces);
static_assert(!(InstanceLayout::offsetOfLoopPollPage() % static_cast<ptrdiff_t>(InstanceLayout::granuleAboveInstance)) && !(InstanceLayout::bytesReservedForLoopPollPage % InstanceLayout::granuleAboveInstance));
static_assert(InstanceLayout::offsetOfLoopPollPage() < 0 || InstanceLayout::offsetOfLoopPollPage() + InstanceLayout::bytesReservedForLoopPollPage <= InstanceLayout::offsetOfKeyOrderSteps);
static_assert(InstanceLayout::offsetOfKeyedValuePlaces + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForKeyedValuePlaces) <= InstanceLayout::offsetOfCopiesWithoutFrame);
static_assert(InstanceLayout::offsetOfCopiesWithoutFrame + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForCopiesWithoutFrame) <= -static_cast<ptrdiff_t>(32 * KB));
static_assert(InstanceLayout::offsetOfStringCells - InstanceLayout::offsetOfBuiltinValues == InstanceLayout::offsetOfBuiltinValues - InstanceLayout::offsetOfBuiltinCells);
static_assert(!(InstanceLayout::granuleAboveInstance % InstanceLayout::sizeOfSiteCache));
static_assert(InstanceLayout::offsetOfSiteCaches({ 0, 1, 0, 0 }) == InstanceLayout::offsetOfStringCells + InstanceLayout::granuleAboveInstance);
static_assert(InstanceLayout::offsetOfCards({ 0, 0, 0, 0 }) < InstanceLayout::offsetOfStatics({ 0, 0, 0, 0 }) && InstanceLayout::offsetOfStatics({ 0, 0, 0, 0 }) < InstanceLayout::offsetOfLoopPollPage(), "The collector reads a word of cards even when the statics are empty, and the loop poll page can be inaccessible.");
static_assert(-InstanceLayout::offsetOfCards({ 1, 0, 0, 0 }) <= static_cast<ptrdiff_t>(InstanceLayout::bytesReservedBelowInstance({ 1, 0, 0, 0 })));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
