/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <wtf/Compiler.h>

namespace JSC { namespace AOT {

class JSONRuntime;

inline constexpr size_t jsonSourcePadding = 64;

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
    // No source waits below the open one and the run time owns no text for it, so clearing this and sourceOwnerStackPointer ends it.
    bool canEndSourceInline;
    // The parser of untyped values has failed in the open source. It would fail again on every value around that place.
    bool valueParserFailed;

    ALWAYS_INLINE void didUnwindTo(void* handlerStackPointer)
    {
        if (sourceOwnerStackPointer && std::bit_cast<uintptr_t>(sourceOwnerStackPointer) < std::bit_cast<uintptr_t>(handlerStackPointer)) [[unlikely]]
            dropSourcesBelow(handlerStackPointer);
    }

private:
    void dropSourcesBelow(void* stackPointer);
};

static_assert(std::is_trivially_default_constructible_v<JSONWords> && std::is_standard_layout_v<JSONWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
