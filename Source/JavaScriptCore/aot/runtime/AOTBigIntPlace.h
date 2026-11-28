/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstddef>
#include <cstdint>

namespace JSC { namespace AOT {

inline constexpr unsigned bigIntSlotLength = 9;
inline constexpr unsigned productSlotLength = 2 * bigIntSlotLength;
inline constexpr size_t bigIntSlotDigitsOffset = 2 * sizeof(uint64_t);
inline constexpr uintptr_t remainderMayBeSum = 1;
constexpr size_t bigIntSlotSize(unsigned length) { return bigIntSlotDigitsOffset + length * sizeof(uint64_t); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
