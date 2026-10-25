/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>
#include <type_traits>

namespace JSC { namespace AOT {

class StringBuilderStorage;

struct StringBuilderWords {
    StringBuilderStorage* freeStorage;
    bool isPoolRegistered;
};

static_assert(std::is_trivially_default_constructible_v<StringBuilderWords>);

inline constexpr uint32_t maxStringBuilderReservation = 4096;

inline constexpr uint32_t jsonTokenBytes = 8;
inline constexpr uint32_t jsonBooleanBytes = 8;
inline constexpr uint32_t jsonIntegerBytes = 24;
inline constexpr uint32_t jsonNumberBytes = 40;
inline constexpr uint32_t jsonDateBytes = 32;

static_assert(jsonDateBytes <= maxStringBuilderReservation && jsonNumberBytes <= maxStringBuilderReservation);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
