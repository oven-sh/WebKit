/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <algorithm>
#include <bmalloc/bmalloc.h>
#include <limits>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>

#if OS(LINUX)
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#if defined(__GLIBC__)
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#endif

namespace JSC {

// Returns SIZE_MAX when there is no limit, and on every system but Linux. Allocates nothing.
inline size_t addressSpaceLeft()
{
#if OS(LINUX)
    struct rlimit limit;
    if (getrlimit(RLIMIT_AS, &limit) || limit.rlim_cur == RLIM_INFINITY)
        return std::numeric_limits<size_t>::max();
    size_t mappedPages = 0;
    int file = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (file >= 0) {
        char text[32];
        ssize_t length = read(file, text, sizeof(text));
        for (ssize_t i = 0; i < length && text[i] >= '0' && text[i] <= '9'; ++i)
            mappedPages = mappedPages * 10 + (text[i] - '0');
        close(file);
    }
    size_t mapped = mappedPages * pageSize();
    return limit.rlim_cur > mapped ? limit.rlim_cur - mapped : 0;
#else
    return std::numeric_limits<size_t>::max();
#endif
}

// What was left when it was first asked.
inline size_t availableAddressSpace()
{
    static const size_t result = addressSpaceLeft();
    return result;
}

inline bool hasAddressSpaceLimit()
{
    return availableAddressSpace() != std::numeric_limits<size_t>::max();
}

// What requests to malloc that may not fail fall back on when all the address space has been taken: bmalloc::api::keepAddressSpaceBallast().
static constexpr size_t sizeOfAddressSpaceBallast = 8 * MB;

// Called where address space may have come back: a thread has started or ended, a collection has freed storage.
inline void keepAddressSpaceBallast()
{
#if USE(MIMALLOC)
    if (hasAddressSpaceLimit() && !bmalloc::api::hasWholeAddressSpaceBallast())
        bmalloc::api::keepAddressSpaceBallast(sizeOfAddressSpaceBallast, addressSpaceLeft());
#endif
}

// malloc, stacks and mapped files get 1/N of the address space still free under RLIMIT_AS.
static constexpr unsigned unreservedAddressSpaceDivisor = 2;

inline size_t addressSpaceToReserve()
{
    size_t available = availableAddressSpace();
    if (!hasAddressSpaceLimit())
        return available;
    return available - available / unreservedAddressSpaceDivisor;
}

// glibc reserves 64 MB for the malloc of each thread that has a heap of its own.
inline void shareCLibraryHeapBetweenThreads()
{
#if defined(__GLIBC__)
    const char* tunables = getenv("GLIBC_TUNABLES");
    if (getenv("MALLOC_ARENA_MAX") || (tunables && strstr(tunables, "glibc.malloc.arena_max")))
        return;
    mallopt(M_ARENA_MAX, 1);
#endif
}

// The structure heap gets 1/N of addressSpaceToReserve().
static constexpr unsigned addressSpaceDivisorForStructures = 4;

// All cages of the process together get the rest.
inline size_t addressSpaceForMarkedBlockCages()
{
    size_t whole = addressSpaceToReserve();
    return whole - whole / addressSpaceDivisorForStructures;
}

static constexpr unsigned initialCageSegmentDivisor = 8;
static constexpr size_t maxInitialCageSegmentSize = GB;

inline size_t initialCageSegmentSize(size_t minSize)
{
    size_t share = availableAddressSpace() / initialCageSegmentDivisor;
    size_t size = maxInitialCageSegmentSize;
    while (size > share && size > minSize)
        size /= 2;
    return std::max(size, minSize);
}

// No share is so small that the structure heap gets less than this.
static constexpr size_t smallestStructureHeapSize = MB;
// mimalloc refuses a range that holds none of its 32 MB chunks, and the first block of the structure heap isn't handed to it.
static constexpr size_t smallestStructureHeapSizeForMimalloc = 64 * MB;

// Only processes with Options::runsCompiledImage() keep to the budget: they also reserve the cages.
inline size_t structureHeapSizeWithinBudget(size_t preferredSize)
{
    size_t budget = addressSpaceToReserve() / addressSpaceDivisorForStructures;
    size_t size = preferredSize;
    while (size > budget && size > smallestStructureHeapSize)
        size /= 2;
    return size;
}

// What a compiled image maps from its first instruction to the first statement of its program.
static constexpr size_t smallestCellRangeSize = 64 * MB;
#if ENABLE(AOT_GUARDED_PAGES)
static constexpr size_t sizeOfTablesOfFirstThread = 64 * MB;
#else
static constexpr size_t sizeOfTablesOfFirstThread = 12 * MB;
#endif
// The stack of the first thread as it grows, and what the host maps for itself.
static constexpr size_t sizeOfStackAndMapsOfHost = 16 * MB;
// malloc takes this much whatever the program is, and this many times the size of the program.
static constexpr size_t sizeOfAllocationsAtStart = 32 * MB;
static constexpr unsigned allocationsAtStartPerByteOfProgram = 2;

// The structure heap takes its share of whatever is free.
inline size_t withShareOfStructureHeap(size_t size)
{
    constexpr unsigned divisor = unreservedAddressSpaceDivisor * addressSpaceDivisorForStructures;
    return size + size / (divisor - 1) + smallestStructureHeapSize;
}

inline size_t addressSpaceToStartBesideAllocations()
{
    return withShareOfStructureHeap(smallestCellRangeSize + sizeOfTablesOfFirstThread + sizeOfStackAndMapsOfHost);
}

inline size_t addressSpaceToStart(size_t sizeOfProgram)
{
    return withShareOfStructureHeap(smallestCellRangeSize + sizeOfTablesOfFirstThread + sizeOfStackAndMapsOfHost + sizeOfAllocationsAtStart + allocationsAtStartPerByteOfProgram * sizeOfProgram);
}

} // namespace JSC
