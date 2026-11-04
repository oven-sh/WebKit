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

// A program may have very much of one thing: members in one class, cases in one switch, statements in one body. Each of them has to cost the same however many there are.
// Where a piece of the compiler can be asked alone, its test asks so often that a square does not end. Where that takes too much memory, or only a whole stage can be run, its test compares
// the instructions that are retired at a size and at four times the size: work that grows as the program does takes four times as many, with a logarithm five times, and a square sixteen times.

// Nothing where the system has no counter of instructions, or refuses one.
std::optional<uint64_t> instructionsOfCurrentThreadIn(NOESCAPE const Invocable<void()> auto& functor)
{
    std::optional<uint64_t> before = InstructionCounts::ofCurrentThread();
    functor();
    std::optional<uint64_t> after = InstructionCounts::ofCurrentThread();
    if (!before || !after)
        return std::nullopt;
    return *after - *before;
}

// Of the calling thread and of every thread that has taken a task of a pool since InstructionCounts::begin().
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

// True where nothing was counted.
inline bool growsAsSizeDoes(std::optional<uint64_t> instructionsAtSize, std::optional<uint64_t> instructionsAtFourTimesSize)
{
    return !instructionsAtSize || !instructionsAtFourTimesSize || *instructionsAtFourTimesSize < 8 * *instructionsAtSize;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
