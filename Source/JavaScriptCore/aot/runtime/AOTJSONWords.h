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
// A compiled parser calls itself for what its type holds. It enters a container only while this much stack is left, and else leaves the value to the parser of untyped values.
inline constexpr int64_t stackReservedForJSONParsers = 64 * 1024;
// Shapes share a bit where their numbers are equal modulo this. A bit that is set for another shape costs a call that finds nothing.
inline constexpr unsigned numberOfJSONShapeBits = 256;

enum class JSONSourceEncoding : uint8_t {
    Latin1,
    UTF16,
    UTF8,
};

struct JSONWords {
    const uint8_t* sourceStart;
    const uint8_t* sourceEnd;
    void* sourceOwnerStackPointer;
    JSONRuntime* runtime;
    JSONSourceEncoding sourceEncoding;
    // The run time owns no text for the open source, so clearing this and sourceOwnerStackPointer ends it.
    bool canEndSourceInline;
    // The parser of untyped values has failed in the open source. It would fail again on every value around that place, so nobody asks it again: a text of n levels is not read n times.
    bool valueParserFailed;
    // A text has given an object of the shape members that its type does not declare, and the run time has not given up on the texts of the shape since. Only then does compiled code ask it whether it has parsed an object of the same form before.
    uint32_t shapesSeenWithExtraMembers[numberOfJSONShapeBits / 32];
};

static_assert(std::is_trivially_default_constructible_v<JSONWords> && std::is_standard_layout_v<JSONWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
