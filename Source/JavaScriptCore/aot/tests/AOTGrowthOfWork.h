/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPhaseTimes.h"
#include <optional>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

std::optional<uint64_t> instructionsOfCurrentThreadIn(NOESCAPE const Invocable<void()> auto& functor)
{
    std::optional<uint64_t> before = InstructionCounts::ofCurrentThread();
    functor();
    std::optional<uint64_t> after = InstructionCounts::ofCurrentThread();
    if (!before || !after)
        return std::nullopt;
    return *after - *before;
}

std::optional<uint64_t> instructionsOfAllThreadsIn(NOESCAPE const Invocable<void()> auto& functor)
{
    InstructionCounts::countOnCurrentThread();
    std::optional<uint64_t> before = InstructionCounts::ofAllThreads();
    functor();
    std::optional<uint64_t> after = InstructionCounts::ofAllThreads();
    if (!before || !after)
        return std::nullopt;
    return *after - *before;
}

inline bool growsAsSizeDoes(std::optional<uint64_t> instructionsAtSize, std::optional<uint64_t> instructionsAtFourTimesSize)
{
    return !instructionsAtSize || !instructionsAtFourTimesSize || *instructionsAtFourTimesSize < 8 * *instructionsAtSize;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
