/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>

namespace JSC { namespace AOT {

enum class TrimmedEnds : uint8_t { Start, End, Both };

// Both bounds of an element that an array does not have, or that is undefined, where the elements are delivered as bounds in a string.
constexpr int32_t absentBound = -1;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
