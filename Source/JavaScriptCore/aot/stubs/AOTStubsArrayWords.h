/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>
#include <type_traits>

namespace JSC { namespace AOT {

struct ArrayWords {
    static constexpr uint32_t numberOfStructuresWithoutNames = 4;

    uint64_t distanceFromProfileToLastArray;
    const void* chainsAboveListsWithoutNames[numberOfStructuresWithoutNames];
    uint32_t structuresOfListsWithoutNames[numberOfStructuresWithoutNames];
};

static_assert(sizeof(ArrayWords) == 8 + 12 * ArrayWords::numberOfStructuresWithoutNames);
static_assert(std::is_trivially_default_constructible_v<ArrayWords> && std::is_standard_layout_v<ArrayWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
