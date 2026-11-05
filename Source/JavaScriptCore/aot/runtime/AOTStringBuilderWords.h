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
    uint32_t capacityOfLastLargeBuffer;
};

static_assert(std::is_trivially_default_constructible_v<StringBuilderWords>);

inline constexpr uint32_t maxStringBuilderReservation = 4096;

inline constexpr int64_t stackReservedBelowJSONLevels = 64 * 1024;

inline constexpr uint32_t bitsOfJSONPosition = 2;
constexpr uint32_t decodeJSONPosition(uint32_t encoded) { return encoded & ((1u << bitsOfJSONPosition) - 1); }
constexpr uint32_t decodeJSONLevelsAround(uint32_t encoded) { return encoded >> bitsOfJSONPosition; }

inline constexpr uint32_t jsonIntegerBytes = 24;
inline constexpr uint32_t jsonNumberBytes = 40;

static_assert(jsonNumberBytes <= maxStringBuilderReservation);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
