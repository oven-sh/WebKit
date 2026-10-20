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

// malloc, stacks and mapped files get at least 1/N of the address space still free under RLIMIT_AS.
static constexpr unsigned unreservedAddressSpaceDivisor = 4;
// They also get at least this many bytes.
static constexpr size_t unreservedAddressSpaceFloor = GB;
// The floor is capped at 1/N of the free address space.
static constexpr unsigned unreservedAddressSpaceFloorDivisor = 2;

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
        size_t unreserved = std::max(free / unreservedAddressSpaceDivisor, std::min(unreservedAddressSpaceFloor, free / unreservedAddressSpaceFloorDivisor));
        return free - unreserved;
    }();
    return result;
#else
    return std::numeric_limits<size_t>::max();
#endif
}

// All cages of the process together get 1/N of addressSpaceToReserve().
static constexpr unsigned addressSpaceDivisorForMarkedBlockCages = 2;
// The structure heap gets 1/N of addressSpaceToReserve().
static constexpr unsigned addressSpaceDivisorForStructures = 8;
// The two shares add up to at most the whole.
static_assert(addressSpaceDivisorForMarkedBlockCages + addressSpaceDivisorForStructures <= addressSpaceDivisorForMarkedBlockCages * addressSpaceDivisorForStructures);

} // namespace JSC
