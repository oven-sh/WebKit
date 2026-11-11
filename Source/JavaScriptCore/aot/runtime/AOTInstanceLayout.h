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

#if defined(BUN_HARDENED) || CPU(ARM64)
#define AOT_BOXED_CALLS_LIE_OUTSIDE_INSTANCE 1
#else
#define AOT_BOXED_CALLS_LIE_OUTSIDE_INSTANCE 0
#endif

namespace JSC { namespace AOT {

struct InstanceSizes {
    uint32_t sizeOfStatics;
    uint32_t numberOfStringCells;
    uint32_t numberOfGlobalBindings;
    uint32_t numberOfSiteCaches;
    uint32_t sizeOfMethodTable;
};
static_assert(sizeof(InstanceSizes) == 20 && isImageRecord<InstanceSizes>);

struct InstanceLayout {
#if CPU(ARM64) && OS(LINUX)
    static constexpr size_t largestPageSize = 64 * KB;
    static constexpr size_t distanceToPageEdge = 16 * KB;
#else
    static constexpr size_t largestPageSize = 16 * KB;
    static constexpr size_t distanceToPageEdge = 0;
#endif
    static constexpr size_t bytesReservedForLoopPollPage = largestPageSize;
    static constexpr size_t bytesReservedForPrefixAndPollPage = 1 * MB + (distanceToPageEdge ? largestPageSize - distanceToPageEdge : 0);
    static constexpr size_t shiftAboveLoopPollPage = largestPageSize - 16 * KB;
    static constexpr size_t granuleBelowInstance = 64 * KB;
    static constexpr size_t granuleAboveInstance = 16 * KB;
    static constexpr unsigned cardShift = 8;
    static constexpr size_t sizeOfStringCell = sizeof(uint32_t);
    static constexpr unsigned stringCellCardShift = 7;
    static constexpr size_t sizeOfGlobalBinding = 24;
    static constexpr size_t sizeOfSiteCache = 16;
    static constexpr size_t sizeOfSiteAnswers = 16;
    static constexpr unsigned sizeOfIndexOfSiteAnswersShift = 1;
    static constexpr size_t sizeOfIndexOfSiteAnswers = 1 << sizeOfIndexOfSiteAnswersShift;
    static constexpr uint16_t nameOfSiteStandsBehind = UINT16_MAX;
    static constexpr uint32_t numbersOfSiteWhoseNameStandsBehind = 3;
    static constexpr size_t offsetOfStringCells = 384 * KB + shiftAboveLoopPollPage;
#if defined(BUN_HARDENED)
    static constexpr size_t offsetOfWordsWrittenOnce = 16 * KB;
#endif
#if AOT_BOXED_CALLS_LIE_OUTSIDE_INSTANCE
#if defined(BUN_HARDENED) && CPU(ARM64) && OS(LINUX)
    static constexpr ptrdiff_t offsetOfBoxedCalls = 32 * KB;
#else
    static constexpr ptrdiff_t offsetOfBoxedCalls = -static_cast<ptrdiff_t>(32 * KB);
#endif
    static constexpr size_t bytesReservedForBoxedCalls = 16 * KB;
#endif
    static constexpr ptrdiff_t offsetOfKeyedValuePlaces = -static_cast<ptrdiff_t>(80 * KB);
    static constexpr size_t bytesReservedForKeyedValuePlaces = 32 * KB;
    static constexpr size_t bytesReservedForSiteAnswers = sizeof(uint64_t) << (8 * sizeOfIndexOfSiteAnswers);
    static constexpr ptrdiff_t offsetOfSiteAnswers = offsetOfKeyedValuePlaces - static_cast<ptrdiff_t>(bytesReservedForSiteAnswers);
    static constexpr size_t bytesReservedForMissesOfPlaces = size_t { 1 } << 16;
    static constexpr ptrdiff_t offsetOfMissesOfPlaces = offsetOfSiteAnswers - static_cast<ptrdiff_t>(bytesReservedForMissesOfPlaces);
    static constexpr ptrdiff_t offsetOfCopiesWithoutFrame = -static_cast<ptrdiff_t>(48 * KB);
    static constexpr size_t bytesReservedForCopiesWithoutFrame = 16 * KB;
    static constexpr size_t offsetOfTablesAboveInstance = 32 * KB + shiftAboveLoopPollPage;
    static constexpr size_t offsetOfBuiltinCells = 128 * KB + shiftAboveLoopPollPage;
    static constexpr size_t offsetOfBuiltinValues = 256 * KB + shiftAboveLoopPollPage;

#if CPU(ARM64) && !defined(BUN_HARDENED)
    static constexpr ptrdiff_t offsetOfLoopPollPage() { return static_cast<ptrdiff_t>(offsetOfTablesAboveInstance - bytesReservedForLoopPollPage); }
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

    static constexpr bool holdsBuiltinCells(size_t numberOfKeys) { return numberOfKeys * sizeof(void*) <= offsetOfBuiltinValues - offsetOfBuiltinCells; }

    static constexpr size_t offsetOfStringCell(uint32_t number) { return offsetOfStringCells + number * sizeOfStringCell; }
    static constexpr size_t offsetOfGlobalBindings(InstanceSizes sizes) { return roundUpToMultipleOf<sizeof(uint64_t)>(offsetOfStringCell(sizes.numberOfStringCells)); }
    static constexpr size_t endOfGlobalBindings(InstanceSizes sizes) { return offsetOfGlobalBindings(sizes) + sizes.numberOfGlobalBindings * sizeOfGlobalBinding; }
    static constexpr bool beginsAtPage(ptrdiff_t offset) { return !((offset - static_cast<ptrdiff_t>(distanceToPageEdge)) % static_cast<ptrdiff_t>(largestPageSize)); }
    static constexpr size_t firstPageAt(size_t offset) { return roundUpToMultipleOf<largestPageSize>(offset - distanceToPageEdge) + distanceToPageEdge; }
    static constexpr size_t offsetOfMethodTable(InstanceSizes sizes) { return firstPageAt(endOfGlobalBindings(sizes)); }
    static constexpr size_t bytesReservedForMethodTable(InstanceSizes sizes) { return roundUpToMultipleOf<largestPageSize>(sizes.sizeOfMethodTable); }
    static constexpr size_t offsetOfSiteCaches(InstanceSizes sizes) { return offsetOfMethodTable(sizes) + bytesReservedForMethodTable(sizes); }
    static constexpr size_t offsetOfSiteCache(InstanceSizes sizes, uint32_t number) { return offsetOfSiteCaches(sizes) + number * sizeOfSiteCache; }
    static constexpr size_t offsetOfIndicesOfSiteAnswers(InstanceSizes sizes) { return firstPageAt(offsetOfSiteCache(sizes, sizes.numberOfSiteCaches)); }
    static constexpr size_t offsetOfIndexOfSiteAnswers(InstanceSizes sizes, uint32_t number) { return offsetOfIndicesOfSiteAnswers(sizes) + number * sizeOfIndexOfSiteAnswers; }
    static constexpr size_t bytesReservedFromInstance(InstanceSizes sizes) { return firstPageAt(offsetOfIndexOfSiteAnswers(sizes, sizes.numberOfSiteCaches)); }

    static constexpr size_t reachAboveInstance = static_cast<size_t>(std::numeric_limits<int32_t>::max());
    static constexpr size_t reachBelowInstance = static_cast<size_t>(std::numeric_limits<int32_t>::max());
    static constexpr bool isAddressable(InstanceSizes sizes)
    {
        return bytesReservedFromInstance(sizes) <= reachAboveInstance && bytesReservedBelowInstance(sizes) <= reachBelowInstance;
    }

private:
    static constexpr size_t bytesReservedForStatics(InstanceSizes sizes) { return roundUpToMultipleOf<granuleBelowInstance>(std::max<size_t>(sizes.sizeOfStatics, 1)); }
};

static_assert(InstanceLayout::largestPageSize <= InstanceLayout::granuleBelowInstance && !(InstanceLayout::largestPageSize % InstanceLayout::granuleAboveInstance));
static_assert(InstanceLayout::beginsAtPage(-static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForPrefixAndPollPage)) && InstanceLayout::beginsAtPage(InstanceLayout::offsetOfLoopPollPage()));
static_assert(!(InstanceLayout::offsetOfStringCells % InstanceLayout::granuleAboveInstance));
#if defined(BUN_HARDENED)
static_assert(InstanceLayout::beginsAtPage(InstanceLayout::offsetOfWordsWrittenOnce) && InstanceLayout::beginsAtPage(InstanceLayout::offsetOfTablesAboveInstance));
static_assert(InstanceLayout::offsetOfBoxedCalls < 0
    ? InstanceLayout::beginsAtPage(InstanceLayout::offsetOfBoxedCalls) && InstanceLayout::beginsAtPage(InstanceLayout::offsetOfBoxedCalls + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForBoxedCalls))
    : static_cast<size_t>(InstanceLayout::offsetOfBoxedCalls) + InstanceLayout::bytesReservedForBoxedCalls <= InstanceLayout::offsetOfTablesAboveInstance);
#endif
#if AOT_BOXED_CALLS_LIE_OUTSIDE_INSTANCE
static_assert(!(InstanceLayout::offsetOfBoxedCalls % static_cast<ptrdiff_t>(InstanceLayout::granuleAboveInstance)) && !(InstanceLayout::bytesReservedForBoxedCalls % InstanceLayout::granuleAboveInstance));
static_assert(-static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForPrefixAndPollPage - InstanceLayout::bytesReservedForLoopPollPage) <= InstanceLayout::offsetOfBoxedCalls);
#endif
static_assert(-static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForPrefixAndPollPage - InstanceLayout::bytesReservedForLoopPollPage) <= InstanceLayout::offsetOfKeyedValuePlaces);
static_assert(InstanceLayout::offsetOfKeyedValuePlaces + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForKeyedValuePlaces) <= InstanceLayout::offsetOfCopiesWithoutFrame);
static_assert(-static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForPrefixAndPollPage - InstanceLayout::bytesReservedForLoopPollPage) <= InstanceLayout::offsetOfSiteAnswers && !(InstanceLayout::offsetOfSiteAnswers % static_cast<ptrdiff_t>(InstanceLayout::sizeOfSiteAnswers)));
static_assert(-static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForPrefixAndPollPage - InstanceLayout::bytesReservedForLoopPollPage) <= InstanceLayout::offsetOfMissesOfPlaces);
static_assert(InstanceLayout::offsetOfCopiesWithoutFrame + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForCopiesWithoutFrame) <= -static_cast<ptrdiff_t>(32 * KB));
static_assert(!(InstanceLayout::offsetOfLoopPollPage() % static_cast<ptrdiff_t>(InstanceLayout::granuleAboveInstance)) && !(InstanceLayout::bytesReservedForLoopPollPage % InstanceLayout::granuleAboveInstance));
static_assert(InstanceLayout::offsetOfLoopPollPage() < 0 || InstanceLayout::offsetOfLoopPollPage() + InstanceLayout::bytesReservedForLoopPollPage <= InstanceLayout::offsetOfTablesAboveInstance);
static_assert(InstanceLayout::offsetOfTablesAboveInstance <= InstanceLayout::offsetOfBuiltinCells);
static_assert(InstanceLayout::offsetOfStringCells - InstanceLayout::offsetOfBuiltinValues == InstanceLayout::offsetOfBuiltinValues - InstanceLayout::offsetOfBuiltinCells);
static_assert(InstanceLayout::offsetOfStringCell(1) <= InstanceLayout::bytesReservedFromInstance({ 0, 1, 0 }) && InstanceLayout::beginsAtPage(static_cast<ptrdiff_t>(InstanceLayout::bytesReservedFromInstance({ 0, 1, 0 }))));
static_assert(InstanceLayout::offsetOfStringCell(1) <= InstanceLayout::offsetOfGlobalBindings({ 0, 1, 1 }) && !(InstanceLayout::offsetOfGlobalBindings({ 0, 1, 1 }) % sizeof(uint64_t)) && !(InstanceLayout::sizeOfGlobalBinding % sizeof(uint64_t)));
static_assert(InstanceLayout::endOfGlobalBindings({ 0, 1, 1 }) <= InstanceLayout::bytesReservedFromInstance({ 0, 1, 1 }) && InstanceLayout::beginsAtPage(static_cast<ptrdiff_t>(InstanceLayout::bytesReservedFromInstance({ 0, 1, 1 }))));
static_assert(!(InstanceLayout::granuleAboveInstance % InstanceLayout::sizeOfSiteCache) && InstanceLayout::endOfGlobalBindings({ 0, 1, 1, 1 }) <= InstanceLayout::offsetOfSiteCaches({ 0, 1, 1, 1 }));
static_assert(InstanceLayout::offsetOfSiteCache({ 0, 1, 1, 1 }, 1) <= InstanceLayout::bytesReservedFromInstance({ 0, 1, 1, 1 }) && InstanceLayout::beginsAtPage(static_cast<ptrdiff_t>(InstanceLayout::bytesReservedFromInstance({ 0, 1, 1, 1 }))));
static_assert(InstanceLayout::beginsAtPage(static_cast<ptrdiff_t>(InstanceLayout::offsetOfMethodTable({ 0, 1, 1, 1, 4 }))) && InstanceLayout::endOfGlobalBindings({ 0, 1, 1, 1, 4 }) <= InstanceLayout::offsetOfMethodTable({ 0, 1, 1, 1, 4 }));
static_assert(InstanceLayout::offsetOfMethodTable({ 0, 1, 1, 1, 4 }) + 4 <= InstanceLayout::offsetOfSiteCaches({ 0, 1, 1, 1, 4 }) && InstanceLayout::beginsAtPage(static_cast<ptrdiff_t>(InstanceLayout::offsetOfSiteCaches({ 0, 1, 1, 1, 4 }))));
static_assert(InstanceLayout::offsetOfSiteCache({ 0, 1, 1, 1, 4 }, 1) <= InstanceLayout::bytesReservedFromInstance({ 0, 1, 1, 1, 4 }) && InstanceLayout::beginsAtPage(static_cast<ptrdiff_t>(InstanceLayout::bytesReservedFromInstance({ 0, 1, 1, 1, 4 }))));
static_assert(InstanceLayout::isAddressable({ 64 * MB, 4 * MB, MB, 4 * MB, 16 * MB }) && !InstanceLayout::isAddressable({ 0, 0, 0, 128 * MB, 0 }) && !InstanceLayout::isAddressable({ UINT32_MAX, 0, 0, 0, 0 }));
static_assert(InstanceLayout::offsetOfCards({ 0, 0, 0 }) < InstanceLayout::offsetOfStatics({ 0, 0, 0 }) && InstanceLayout::offsetOfStatics({ 0, 0, 0 }) < InstanceLayout::offsetOfLoopPollPage(), "The collector reads a word of cards even when the statics are empty, and the loop poll page can be inaccessible.");
static_assert(-InstanceLayout::offsetOfCards({ 1, 0, 0 }) <= static_cast<ptrdiff_t>(InstanceLayout::bytesReservedBelowInstance({ 1, 0, 0 })));
static_assert(InstanceLayout::isAddressable({ 0, 0, 0 }));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
