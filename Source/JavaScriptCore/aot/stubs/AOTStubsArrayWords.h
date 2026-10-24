/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreshold.h"
#include <cstdint>
#include <type_traits>

namespace JSC { namespace AOT {

struct ArrayWords {
    // An array with storage for any value that is asked for the storage of numbers is looked through, in vain where it holds anything else. A realm that has looked in vain at so many elements looks no more.
    AOT_CLASS_THRESHOLD(uint32_t, maxElementsLookedAtInVain, 4096);
    // New storage pays for itself in an array that is read again and again, or once from end to end if it is long.
    AOT_CLASS_THRESHOLD(uint32_t, minLengthThatPaysAtOnce, 32);
    AOT_CLASS_THRESHOLD(uint32_t, maxShortYoungArraysConverted, 16);
    static constexpr uint32_t numberOfCounts = 64;

    uint64_t distanceFromProfileToLastArray;
    uint32_t elementsLookedAtInVain;
    uint32_t placeThatConverts;
    uint8_t shortYoungArraysConverted[numberOfCounts];
};

static_assert(sizeof(ArrayWords) == 16 + ArrayWords::numberOfCounts);
static_assert(std::is_trivially_default_constructible_v<ArrayWords> && std::is_standard_layout_v<ArrayWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
