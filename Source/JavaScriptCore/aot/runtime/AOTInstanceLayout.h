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
    // In bytes.
    uint32_t sizeOfMethodTable;
};
static_assert(sizeof(InstanceSizes) == 20 && isImageRecord<InstanceSizes>);

struct InstanceLayout {
    // What is protected or mapped by itself begins at a page. A kernel for Linux on ARM64 may have pages of 64 KB, and a poll reaches only 32 KB up from the instance.
    // So the instance does not begin at a page there: it ends 16 KB in front of one, and that page is the page of the polls.
    // A hardened engine polls a word. There that page holds what is read-only once the instance has been created.
#if CPU(ARM64) && OS(LINUX)
    static constexpr size_t largestPageSize = 64 * KB;
    static constexpr size_t distanceToPageEdge = 16 * KB;
#else
    static constexpr size_t largestPageSize = 16 * KB;
    static constexpr size_t distanceToPageEdge = 0;
#endif
    static constexpr size_t bytesReservedForLoopPollPage = largestPageSize;
    static constexpr size_t bytesReservedForPrefixAndPollPage = 1 * MB + (distanceToPageEdge ? largestPageSize - distanceToPageEdge : 0);
    // The tables above the page of the polls lie this much higher than where that page has 16 KB.
    static constexpr size_t shiftAboveLoopPollPage = largestPageSize - 16 * KB;
    static constexpr size_t granuleBelowInstance = 64 * KB;
    static constexpr size_t granuleAboveInstance = 16 * KB;
    static constexpr unsigned cardShift = 8;
    static constexpr size_t sizeOfStringCell = sizeof(uint32_t);
    // One card says whether a cell was written among this many bytes of string cells.
    static constexpr unsigned stringCellCardShift = 7;
    static constexpr size_t sizeOfGlobalBinding = 24;
    static constexpr size_t sizeOfSiteCache = 16;
    static constexpr size_t sizeOfSiteAnswers = 16;
    // Of each place of the program that may have SiteAnswers: where they lie, in words of eight bytes from the first. Zero: it has none, and finds those that fit nothing.
    static constexpr unsigned sizeOfIndexOfSiteAnswersShift = 1;
    static constexpr size_t sizeOfIndexOfSiteAnswers = 1 << sizeOfIndexOfSiteAnswersShift;
    static constexpr size_t offsetOfStringCells = 384 * KB + shiftAboveLoopPollPage;
#if defined(BUN_HARDENED)
    // From here to the tables above the instance: what nothing writes once the instance has been created. The pages are read-only from then on.
    static constexpr size_t offsetOfWordsWrittenOnce = 16 * KB;
#endif
#if AOT_BOXED_CALLS_LIE_OUTSIDE_INSTANCE
    // The table of boxed calls, with room for an entry for every number that a byte holds. In a hardened engine it is read-only once the instance has been created.
#if defined(BUN_HARDENED) && CPU(ARM64) && OS(LINUX)
    // Behind the words written once, in their page: a page of 64 KB below the instance would hold the instance too.
    static constexpr ptrdiff_t offsetOfBoxedCalls = 32 * KB;
#else
    static constexpr ptrdiff_t offsetOfBoxedCalls = -static_cast<ptrdiff_t>(32 * KB);
#endif
    static constexpr size_t bytesReservedForBoxedCalls = 16 * KB;
#endif
    // KeyedValuePlace, RestOfPlaceForStores and AcceptedKindOfValue. A stub reaches a place by its distance from the instance.
    static constexpr ptrdiff_t offsetOfKeyedValuePlaces = -static_cast<ptrdiff_t>(80 * KB);
    static constexpr size_t bytesReservedForKeyedValuePlaces = 32 * KB;
    // SiteAnswers, given out from the first on as places are filled, so that they cost memory as they come to be. As many as an index reaches.
    static constexpr size_t bytesReservedForSiteAnswers = sizeof(uint64_t) << (8 * sizeOfIndexOfSiteAnswers);
    static constexpr ptrdiff_t offsetOfSiteAnswers = offsetOfKeyedValuePlaces - static_cast<ptrdiff_t>(bytesReservedForSiteAnswers);
    // AllocationWords::numberOfCopiesWithoutFrame pointers, reached the same way.
    static constexpr ptrdiff_t offsetOfCopiesWithoutFrame = -static_cast<ptrdiff_t>(48 * KB);
    static constexpr size_t bytesReservedForCopiesWithoutFrame = 16 * KB;
    // Between the instance and the string cells.
    static constexpr size_t offsetOfTablesAboveInstance = 32 * KB + shiftAboveLoopPollPage;
    // Small tables of which compiled code reads one word at a time.
    static constexpr size_t offsetOfBuiltinCells = 128 * KB + shiftAboveLoopPollPage;
    static constexpr size_t offsetOfBuiltinValues = 256 * KB + shiftAboveLoopPollPage;

#if CPU(ARM64) && !defined(BUN_HARDENED)
    // A poll is one load, and the immediate offset of a load reaches from the instance to 32 KB above it: the page lies between the instance and the tables above it.
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

    // A value has the number of its key, as a cell has, so each of the two tables has a word for every key.
    static constexpr bool holdsBuiltinCells(size_t numberOfKeys) { return numberOfKeys * sizeof(void*) <= offsetOfBuiltinValues - offsetOfBuiltinCells; }

    static constexpr size_t offsetOfStringCell(uint32_t number) { return offsetOfStringCells + number * sizeOfStringCell; }
    // Behind the string cells: as many records as the program binds names.
    static constexpr size_t offsetOfGlobalBindings(InstanceSizes sizes) { return roundUpToMultipleOf<sizeof(uint64_t)>(offsetOfStringCell(sizes.numberOfStringCells)); }
    static constexpr size_t endOfGlobalBindings(InstanceSizes sizes) { return offsetOfGlobalBindings(sizes) + sizes.numberOfGlobalBindings * sizeOfGlobalBinding; }
    static constexpr bool beginsAtPage(ptrdiff_t offset) { return !((offset - static_cast<ptrdiff_t>(distanceToPageEdge)) % static_cast<ptrdiff_t>(largestPageSize)); }
    static constexpr size_t firstPageAt(size_t offset) { return roundUpToMultipleOf<largestPageSize>(offset - distanceToPageEdge) + distanceToPageEdge; }
    // Behind the records of the bindings, in pages of its own: a copy of the image's method table, which is read-only but while the run time repoints an entry. AOTClassGuards.h.
    static constexpr size_t offsetOfMethodTable(InstanceSizes sizes) { return firstPageAt(endOfGlobalBindings(sizes)); }
    static constexpr size_t bytesReservedForMethodTable(InstanceSizes sizes) { return roundUpToMultipleOf<largestPageSize>(sizes.sizeOfMethodTable); }
    // Last: a Slot for each site. The pages of sites that nothing has filled are given back.
    // On ARM64 what lies beyond 16 MB costs a load more (isFarInstanceWord()). The sites take the most room by far, and a look at one is several loads or a call of a stub, where each of the others is one load in the midst of typed code.
    static constexpr size_t offsetOfSiteCaches(InstanceSizes sizes) { return offsetOfMethodTable(sizes) + bytesReservedForMethodTable(sizes); }
    static constexpr size_t offsetOfSiteCache(InstanceSizes sizes, uint32_t number) { return offsetOfSiteCaches(sizes) + number * sizeOfSiteCache; }
    // Behind them, in pages of their own: the SiteAnswers of each site. A site has the one or the other, and a page that nothing was written to costs nothing.
    static constexpr size_t offsetOfIndicesOfSiteAnswers(InstanceSizes sizes) { return firstPageAt(offsetOfSiteCache(sizes, sizes.numberOfSiteCaches)); }
    static constexpr size_t offsetOfIndexOfSiteAnswers(InstanceSizes sizes, uint32_t number) { return offsetOfIndicesOfSiteAnswers(sizes) + number * sizeOfIndexOfSiteAnswers; }
    static constexpr size_t bytesReservedFromInstance(InstanceSizes sizes) { return firstPageAt(offsetOfIndexOfSiteAnswers(sizes, sizes.numberOfSiteCaches)); }

    // The image states the distance from the instance to a word as a signed number of 32 bits. Nothing else bounds the instance.
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
