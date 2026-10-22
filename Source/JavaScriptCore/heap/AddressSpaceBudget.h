/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <algorithm>
#include <limits>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>

#if OS(LINUX)
#include <stdio.h>
#include <sys/resource.h>
#endif

namespace JSC {

// malloc, stacks and mapped files get 1/N of the address space still free under RLIMIT_AS.
static constexpr unsigned unreservedAddressSpaceDivisor = 2;

// Returns SIZE_MAX when there is no limit, and on every system but Linux.
inline size_t addressSpaceToReserve()
{
#if OS(LINUX)
    static const size_t result = [] {
        struct rlimit limit;
        if (getrlimit(RLIMIT_AS, &limit) || limit.rlim_cur == RLIM_INFINITY)
            return std::numeric_limits<size_t>::max();
        size_t mappedPages = 0;
        if (FILE* file = fopen("/proc/self/statm", "r")) {
            if (fscanf(file, "%zu", &mappedPages) != 1)
                mappedPages = 0;
            fclose(file);
        }
        size_t mapped = mappedPages * pageSize();
        size_t free = limit.rlim_cur > mapped ? limit.rlim_cur - mapped : 0;
        return free - free / unreservedAddressSpaceDivisor;
    }();
    return result;
#else
    return std::numeric_limits<size_t>::max();
#endif
}

// The structure heap gets 1/N of addressSpaceToReserve().
static constexpr unsigned addressSpaceDivisorForStructures = 8;

// All cages of the process together get the rest.
inline size_t addressSpaceForMarkedBlockCages()
{
    size_t whole = addressSpaceToReserve();
    return whole - whole / addressSpaceDivisorForStructures;
}

// No share is so small that the structure heap gets less than this.
static constexpr size_t smallestStructureHeapSize = MB;
// mimalloc refuses a range that holds none of its 32 MB chunks, and the first block of the structure heap isn't handed to it.
static constexpr size_t smallestStructureHeapSizeForMimalloc = 64 * MB;

// Only processes with Options::useAOT() keep to the budget: they also reserve the cages.
inline size_t structureHeapSizeWithinBudget(size_t preferredSize)
{
    size_t budget = addressSpaceToReserve() / addressSpaceDivisorForStructures;
    size_t size = preferredSize;
    while (size > budget && size > smallestStructureHeapSize)
        size /= 2;
    return size;
}

} // namespace JSC
