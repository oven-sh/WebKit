/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "AOTStringBuilderStorage.h"
#include "AOTStringBuilderWords.h"
#include "DateTimeWriter.h"
#include <charconv>
#include <cmath>
#include <wtf/MathExtras.h>
#include <wtf/dtoa.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

inline constexpr unsigned maxJSONIntegerLength = 20;
inline constexpr unsigned maxJSONNumberLength = numberToShortestStringBufferSize;
inline constexpr unsigned maxJSONDateLength = maxISODateTimeLength + 2;

static_assert(maxJSONIntegerLength <= jsonIntegerBytes && maxJSONNumberLength <= jsonNumberBytes && maxJSONDateLength <= jsonDateBytes);

inline constexpr char jsonDigitPairs[] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

ALWAYS_INLINE Latin1Character* appendFourJSONDigits(Latin1Character* cursor, uint32_t value)
{
    memcpy(cursor, jsonDigitPairs + 2 * (value / 100), 2);
    memcpy(cursor + 2, jsonDigitPairs + 2 * (value % 100), 2);
    return cursor + 4;
}

ALWAYS_INLINE Latin1Character* appendUpToFourJSONDigits(Latin1Character* cursor, uint32_t value)
{
    uint32_t high = value / 100;
    uint32_t low = value % 100;
    if (value < 100) {
        if (value < 10) {
            *cursor = '0' + value;
            return cursor + 1;
        }
        memcpy(cursor, jsonDigitPairs + 2 * low, 2);
        return cursor + 2;
    }
    if (value < 1000) {
        *cursor = '0' + high;
        memcpy(cursor + 1, jsonDigitPairs + 2 * low, 2);
        return cursor + 3;
    }
    return appendFourJSONDigits(cursor, value);
}

ALWAYS_INLINE Latin1Character* appendJSONInteger(Latin1Character* cursor, int64_t integer)
{
    if (static_cast<uint64_t>(integer) < 10000) [[likely]]
        return appendUpToFourJSONDigits(cursor, static_cast<uint32_t>(integer));
    if (static_cast<uint64_t>(integer) < 100000000) {
        uint32_t value = static_cast<uint32_t>(integer);
        return appendFourJSONDigits(appendUpToFourJSONDigits(cursor, value / 10000), value % 10000);
    }
    char* to = std::bit_cast<char*>(cursor);
    return std::bit_cast<Latin1Character*>(std::to_chars(to, to + maxJSONIntegerLength, integer).ptr);
}

ALWAYS_INLINE Latin1Character* appendJSONLiteral(Latin1Character* cursor, ASCIILiteral text)
{
    memcpy(cursor, text.characters(), text.length());
    return cursor + text.length();
}

ALWAYS_INLINE Latin1Character* appendJSONBoolean(Latin1Character* cursor, bool value)
{
    memcpy(cursor, value ? "true" : "fals", 4);
    cursor[4] = 'e';
    return cursor + 5 - value;
}

ALWAYS_INLINE Latin1Character* appendJSONNumber(Latin1Character* cursor, double number)
{
    if (number >= 0 && number < 10000) [[likely]] {
        uint32_t truncated = static_cast<uint32_t>(number);
        if (static_cast<double>(truncated) == number) [[likely]]
            return appendUpToFourJSONDigits(cursor, truncated);
    }
    if (auto int32 = tryConvertToStrictInt32(number))
        return appendJSONInteger(cursor, *int32);
    if (!std::isfinite(number))
        return appendJSONLiteral(cursor, "null"_s);
    if (!number)
        return appendJSONLiteral(cursor, "0"_s);
    return cursor + numberToShortestString(number, std::span<char, numberToShortestStringBufferSize> { std::bit_cast<char*>(cursor), numberToShortestStringBufferSize });
}

// Behind a long text comes the rest of what holds it, which is mostly short.
constexpr size_t roomBehindJSONText(size_t length) { return length / 8; }

constexpr size_t maxLengthOfEscapedJSONString(size_t length) { return 6 * length + 2; }

// The cursor behind the quoted text, with maxStringBuilderReservation bytes of room. Null with an exception.
Latin1Character* appendEscapedJSONString(JSGlobalObject*, StringBuilderStorage&, Latin1Character* cursor, std::span<const Latin1Character>);

ALWAYS_INLINE Latin1Character* appendJSONTimeValue(Latin1Character* cursor, double timeValue)
{
    if (!std::isfinite(timeValue)) [[unlikely]]
        return appendJSONLiteral(cursor, "null"_s);
    *cursor++ = '"';
    cursor = writeISODateTime(cursor, timeValue);
    *cursor++ = '"';
    return cursor;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
