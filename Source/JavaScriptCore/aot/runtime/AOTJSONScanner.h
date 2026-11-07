/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTJSONSource.h"
#include "AOTSchemaEnums.h"
#include "JSCJSValue.h"
#include "JSONCacheInlines.h"
#include "VM.h"
#include <optional>
#include <wtf/ASCIICType.h>
#include <wtf/SIMDHelpers.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

inline constexpr unsigned maxSkippedJSONDepth = 64;
inline constexpr unsigned maxExactMantissaDigits = 15;
inline constexpr ptrdiff_t maxDigitsOfSmallJSONInteger = 9;
inline constexpr ptrdiff_t maxDigitsOfJSONInteger = 16;
inline constexpr int64_t maxJSONInteger = 9007199254740991;

struct ParsedJSONNumber {
    double number { 0 };
    const uint8_t* end { nullptr };
};

struct ParsedJSONInteger {
    int64_t integer { 0 };
    const uint8_t* end { nullptr };
};

struct ParsedJSONString {
    JSString* string;
    const uint8_t* end { nullptr };
};

struct ParsedJSONValue {
    JSValue value { jsUndefined() };
    const uint8_t* end { nullptr };
};

enum class JSONKeyComparison : uint8_t { Unknown, Different, Equal };

struct ComparedJSONMember {
    JSONKeyComparison comparison { JSONKeyComparison::Unknown };
    const uint8_t* value { nullptr };
};

struct FoundJSONMember {
    Typed::JSONMemberSearch search { Typed::JSONMemberSearch::Unknown };
    const uint8_t* value { nullptr };
};

ParsedJSONNumber parseJSONNumberSlow(const uint8_t* start, const uint8_t* sourceEnd);
const uint8_t* decodeJSONString(const JSONWords&, const uint8_t* afterQuote, const uint8_t* firstSpecial, StringBuilder&);
ParsedJSONString makeJSONStringWithEscapes(Instance&, VM&, const uint8_t* afterQuote, const uint8_t* firstSpecial);
ParsedJSONString makeUncachedJSONString(Instance&, VM&, const uint8_t* afterQuote);
static constexpr unsigned maxLengthOfJSONStringOfEitherWidth = 16;

ALWAYS_INLINE const uint8_t* skipJSONWhitespaceCharacters(const uint8_t* cursor)
{
    while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')
        ++cursor;
    return cursor;
}

ALWAYS_INLINE const uint8_t* findEndOfPlainJSONCharacters(const uint8_t* cursor)
{
    constexpr auto quotes = SIMD::splat<uint8_t>('"');
    constexpr auto escapes = SIMD::splat<uint8_t>('\\');
    constexpr auto spaces = SIMD::splat<uint8_t>(' ');
    for (;; cursor += SIMD::stride<uint8_t>) {
        auto input = SIMD::load(cursor);
        auto found = SIMD::bitOr(SIMD::equal(input, quotes), SIMD::equal(input, escapes), SIMD::lessThan(input, spaces));
        if (auto index = SIMD::findFirstNonZeroIndex(found))
            return cursor + *index;
    }
}

ALWAYS_INLINE unsigned jsonEscapeLength(const uint8_t* backslash)
{
    switch (backslash[1]) {
    case '"':
    case '\\':
    case '/':
    case 'b':
    case 'f':
    case 'n':
    case 'r':
    case 't':
        return 2;
    case 'u':
        return isASCIIHexDigit(backslash[2]) && isASCIIHexDigit(backslash[3]) && isASCIIHexDigit(backslash[4]) && isASCIIHexDigit(backslash[5]) ? 6 : 0;
    default:
        return 0;
    }
}

ALWAYS_INLINE ParsedJSONNumber parseJSONNumber(const uint8_t* start, const uint8_t* sourceEnd)
{
    const uint8_t* cursor = start;
    bool isNegative = *cursor == '-';
    cursor += isNegative;
    const uint8_t* digits = cursor;
    unsigned digit = *cursor - '0';
    if (digit > 9) [[unlikely]]
        return { };
    uint64_t mantissa = 0;
    if (!digit) {
        if (isASCIIDigit(*++cursor)) [[unlikely]]
            return { };
    } else {
        do {
            mantissa = mantissa * 10 + digit;
            digit = *++cursor - '0';
        } while (digit <= 9);
    }
    size_t count = cursor - digits;
    size_t fractionDigitCount = 0;
    uint64_t scale = 1;
    if (*cursor == '.') {
        const uint8_t* fraction = ++cursor;
        digit = *cursor - '0';
        if (digit > 9) [[unlikely]]
            return { };
        do {
            mantissa = mantissa * 10 + digit;
            scale *= 10;
            digit = *++cursor - '0';
        } while (digit <= 9);
        fractionDigitCount = cursor - fraction;
    }
    if (count + fractionDigitCount > maxExactMantissaDigits || (*cursor | 0x20) == 'e') [[unlikely]]
        return parseJSONNumberSlow(start, sourceEnd);
    double number = static_cast<double>(static_cast<int64_t>(mantissa));
    if (fractionDigitCount)
        number /= static_cast<double>(static_cast<int64_t>(scale));
    return { isNegative ? -number : number, cursor };
}

ALWAYS_INLINE const uint8_t* tryParseSmallJSONInteger(const uint8_t* start, JSValue& value)
{
    unsigned digit = *start - '0';
    if (digit > 9)
        return nullptr;
    const uint8_t* cursor = start + 1;
    uint32_t number = digit;
    unsigned next = *cursor - '0';
    if (digit) {
        for (; next <= 9; next = *++cursor - '0')
            number = number * 10 + next;
    }
    if (next <= 9 || cursor - start > maxDigitsOfSmallJSONInteger || *cursor == '.' || (*cursor | 0x20) == 'e') [[unlikely]]
        return nullptr;
    value = jsNumber(static_cast<int32_t>(number));
    return cursor;
}

ALWAYS_INLINE bool isShortTextLatin1AfterDecoding(const uint8_t* text, unsigned length)
{
    constexpr size_t stride = SIMD::stride<uint8_t>;
    static_assert(2 * stride <= jsonSourcePadding);
    ASSERT(length <= 2 * stride);
    constexpr simde_uint8x16_t lowIndices { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    constexpr simde_uint8x16_t highIndices { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    auto lengths = SIMD::splat<uint8_t>(length);
    auto largestUnchanged = SIMD::splat<uint8_t>(saturatedJSONCharacter - 1);
    auto low = SIMD::bitAnd(SIMD::lessThan(largestUnchanged, SIMD::load(text)), SIMD::lessThan(lowIndices, lengths));
    auto high = SIMD::bitAnd(SIMD::lessThan(largestUnchanged, SIMD::load(text + stride)), SIMD::lessThan(highIndices, lengths));
    return !SIMD::isNonZero(SIMD::bitOr(low, high));
}

ALWAYS_INLINE std::optional<ParsedJSONString> findCachedJSONString(const JSONWords& words, VM& vm, const uint8_t* quote)
{
    static_assert(JSONCache::maxNameTextLength <= 2 * SIMD::stride<uint8_t> && JSONCache::maxNameTextLength <= jsonSourcePadding);
    auto* entry = vm.jsonCache().findString(std::span<const Latin1Character> { byteCast<Latin1Character>(quote), jsonSourcePadding });
    if (!entry)
        return std::nullopt;
    if (words.sourceEncoding != JSONSourceEncoding::Latin1) {
        constexpr unsigned quotes = 2;
        if (entry->textLength > maxLengthOfJSONStringOfEitherWidth + quotes || (!entry->textIsASCII && !isShortTextLatin1AfterDecoding(quote, entry->textLength)))
            return std::nullopt;
    }
    return ParsedJSONString { entry->string, quote + entry->textLength };
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
