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
        return free - std::max(free / 4, std::min<size_t>(GB, free / 2));
    }();
    return result;
#else
    return std::numeric_limits<size_t>::max();
#endif
}

static constexpr unsigned addressSpaceDivisorForMarkedBlockCages = 2;
static constexpr unsigned addressSpaceDivisorForNumberBoxes = 4;
static constexpr unsigned addressSpaceDivisorForStructures = 8;

} // namespace JSC
