/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "CollectionScope.h"
#include "RememberedSlot.h"
#include <cstring>
#include <span>
#include <wtf/NotFound.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// Read here, 64 bytes at a step, and not by the library's memcmp(): that of musl goes byte by byte, and every collection asks this of whole tables.
inline bool holdsOnlyZeros(std::span<const uint8_t> bytes)
{
    using Words = uint64_t __attribute__((vector_size(32)));
    const uint8_t* at = bytes.data();
    const uint8_t* end = at + bytes.size();
    for (; end - at >= static_cast<ptrdiff_t>(2 * sizeof(Words)); at += 2 * sizeof(Words)) {
        Words first;
        Words second;
        memcpy(&first, at, sizeof(Words));
        memcpy(&second, at + sizeof(Words), sizeof(Words));
        Words either = first | second;
        if (either[0] | either[1] | either[2] | either[3])
            return false;
    }
    for (; end - at >= static_cast<ptrdiff_t>(sizeof(uint64_t)); at += sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, at, sizeof(word));
        if (word)
            return false;
    }
    for (; at < end; ++at) {
        if (*at)
            return false;
    }
    return true;
}

// Whatever the system does with the pages, and whenever it does it, they read as zeros: they hold zeros now.
inline void decommitPagesOfZeros(void* pages, size_t size)
{
    OSAllocator::decommit(pages, size);
#if OS(WINDOWS)
    // There a decommitted page cannot be touched.
    OSAllocator::commit(pages, size, true, false);
#endif
}

// Where the entries of a table died it holds zeros, on pages that stay the process's own.
inline size_t decommitEmptyPages(std::span<uint8_t> table, RememberedSlotSet& writtenRegions)
{
    size_t sizeOfPage = pageSize();
    size_t regionsPerPage = sizeOfPage >> writtenRegions.regionShift();
    if (!regionsPerPage || !isPageAligned(table.data()))
        return 0;

    struct PageRange {
        size_t begin;
        size_t end;
    };
    // A page between two empty ones that was only read costs no memory, and a fault if it goes back.
    Vector<PageRange, 16> emptyPages;
    size_t numberOfWholePages = table.size() / sizeOfPage;
    size_t currentPage = notFound;
    bool currentPageIsEmpty = false;
    auto finishCurrentPage = [&] {
        if (!currentPageIsEmpty)
            return;
        if (!emptyPages.isEmpty() && emptyPages.last().end == currentPage)
            emptyPages.last().end = currentPage + 1;
        else
            emptyPages.append({ currentPage, currentPage + 1 });
    };
    writtenRegions.forEachRegionToSweep(CollectionScope::Full, [&](size_t beginOffset, size_t endOffset) {
        size_t page = beginOffset / sizeOfPage;
        if (page != currentPage) {
            finishCurrentPage();
            currentPage = page;
            currentPageIsEmpty = page < numberOfWholePages;
        }
        currentPageIsEmpty = currentPageIsEmpty && holdsOnlyZeros(table.subspan(beginOffset, endOffset - beginOffset));
    });
    finishCurrentPage();

    size_t numberOfBytes = 0;
    for (auto [begin, end] : emptyPages) {
        size_t size = (end - begin) * sizeOfPage;
        decommitPagesOfZeros(table.data() + begin * sizeOfPage, size);
        memset(writtenRegions.cards() + begin * regionsPerPage, RememberedSlotSet::neverWrittenCard, (end - begin) * regionsPerPage);
        numberOfBytes += size;
    }
    return numberOfBytes;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
