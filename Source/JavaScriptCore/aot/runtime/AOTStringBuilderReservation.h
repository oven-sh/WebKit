/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>

namespace JSC { namespace AOT {

inline constexpr uint32_t maxStringBuilderReservation = 4096;

inline constexpr uint32_t jsonTokenBytes = 8;
inline constexpr uint32_t jsonBooleanBytes = 8;
inline constexpr uint32_t jsonIntegerBytes = 24;
inline constexpr uint32_t jsonNumberBytes = 32;
inline constexpr uint32_t jsonDateBytes = 32;

static_assert(jsonDateBytes <= maxStringBuilderReservation && jsonNumberBytes <= maxStringBuilderReservation);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
