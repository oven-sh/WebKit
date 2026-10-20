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
    uint64_t distanceFromProfileToLastArray;
};

static_assert(sizeof(ArrayWords) == 8);
static_assert(std::is_trivially_default_constructible_v<ArrayWords> && std::is_standard_layout_v<ArrayWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
