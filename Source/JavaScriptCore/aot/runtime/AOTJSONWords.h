/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace JSC { namespace AOT {

class JSONRuntime;

inline constexpr size_t jsonSourcePadding = 64;
inline constexpr int64_t stackReservedForJSONParsers = 64 * 1024;
inline constexpr unsigned numberOfJSONShapeBits = 256;

enum class JSONSourceEncoding : uint8_t {
    Latin1,
    UTF16,
};

struct JSONWords {
    const uint8_t* sourceStart;
    const uint8_t* sourceEnd;
    void* sourceOwnerStackPointer;
    JSONRuntime* runtime;
    JSONSourceEncoding sourceEncoding;
    bool canEndSourceInline;
    bool valueParserFailed;
    uint32_t shapesSeenWithExtraMembers[numberOfJSONShapeBits / 32];
};

static_assert(std::is_trivially_default_constructible_v<JSONWords> && std::is_standard_layout_v<JSONWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
