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

ALWAYS_INLINE const uint8_t* skipJSONString(const uint8_t* cursor, bool& hasEscapes)
{
    for (;;) {
        cursor = findEndOfPlainJSONCharacters(cursor);
        if (*cursor == '"') [[likely]]
            return cursor + 1;
        if (*cursor != '\\')
            return nullptr;
        unsigned length = jsonEscapeLength(cursor);
        if (!length)
            return nullptr;
        hasEscapes = true;
        cursor += length;
    }
}

ALWAYS_INLINE const uint8_t* skipJSONNumber(const uint8_t* cursor)
{
    cursor += *cursor == '-';
    if (!isASCIIDigit(*cursor))
        return nullptr;
    if (*cursor++ != '0') {
        while (isASCIIDigit(*cursor))
            ++cursor;
    }
    if (*cursor == '.') {
        if (!isASCIIDigit(*++cursor))
            return nullptr;
        while (isASCIIDigit(*cursor))
            ++cursor;
    }
    if ((*cursor | 0x20) == 'e') {
        ++cursor;
        cursor += *cursor == '-' || *cursor == '+';
        if (!isASCIIDigit(*cursor))
            return nullptr;
        while (isASCIIDigit(*cursor))
            ++cursor;
    }
    return cursor;
}

inline const uint8_t* skipJSONValue(const uint8_t* cursor)
{
    uint64_t objectDepthBits = 0;
    static_assert(maxSkippedJSONDepth <= 8 * sizeof(objectDepthBits));
    unsigned depth = 0;
    bool hasEscapes = false;
    for (;;) {
        bool expectsKey = false;
        switch (*cursor) {
        case '"':
            cursor = skipJSONString(cursor + 1, hasEscapes);
            if (!cursor)
                return nullptr;
            break;
        case '{':
        case '[': {
            bool isObject = *cursor == '{';
            cursor = skipJSONWhitespaceCharacters(cursor + 1);
            if (*cursor == (isObject ? '}' : ']')) {
                ++cursor;
                break;
            }
            if (depth == maxSkippedJSONDepth)
                return nullptr;
            objectDepthBits = objectDepthBits << 1 | isObject;
            ++depth;
            if (!isObject)
                continue;
            expectsKey = true;
            break;
        }
        case 't':
            if (WTF::unalignedLoad<uint32_t>(cursor) != ('t' | 'r' << 8 | 'u' << 16 | 'e' << 24))
                return nullptr;
            cursor += 4;
            break;
        case 'f':
            if (WTF::unalignedLoad<uint32_t>(cursor + 1) != ('a' | 'l' << 8 | 's' << 16 | 'e' << 24))
                return nullptr;
            cursor += 5;
            break;
        case 'n':
            if (WTF::unalignedLoad<uint32_t>(cursor) != ('n' | 'u' << 8 | 'l' << 16 | 'l' << 24))
                return nullptr;
            cursor += 4;
            break;
        default:
            cursor = skipJSONNumber(cursor);
            if (!cursor)
                return nullptr;
            break;
        }
        while (!expectsKey) {
            if (!depth)
                return cursor;
            cursor = skipJSONWhitespaceCharacters(cursor);
            bool isObject = objectDepthBits & 1;
            if (*cursor == ',') {
                cursor = skipJSONWhitespaceCharacters(cursor + 1);
                expectsKey = isObject;
                break;
            }
            if (*cursor != (isObject ? '}' : ']'))
                return nullptr;
            ++cursor;
            objectDepthBits >>= 1;
            --depth;
        }
        if (!expectsKey)
            continue;
        if (*cursor != '"')
            return nullptr;
        cursor = skipJSONString(cursor + 1, hasEscapes);
        if (!cursor)
            return nullptr;
        cursor = skipJSONWhitespaceCharacters(cursor);
        if (*cursor != ':')
            return nullptr;
        cursor = skipJSONWhitespaceCharacters(cursor + 1);
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

ALWAYS_INLINE ParsedJSONInteger parseJSONInteger(const uint8_t* start)
{
    bool isNegative = *start == '-';
    const uint8_t* digits = start + isNegative;
    unsigned digit = *digits - '0';
    if (digit > 9) [[unlikely]]
        return { };
    const uint8_t* cursor = digits + 1;
    uint64_t magnitude = digit;
    unsigned next = *cursor - '0';
    if (digit) {
        for (; next <= 9 && cursor - digits < maxDigitsOfJSONInteger; next = *++cursor - '0')
            magnitude = magnitude * 10 + next;
    }
    if (next <= 9 || *cursor == '.' || (*cursor | 0x20) == 'e' || magnitude > static_cast<uint64_t>(maxJSONInteger) || (isNegative && !magnitude)) [[unlikely]]
        return { };
    int64_t integer = static_cast<int64_t>(magnitude);
    return { isNegative ? -integer : integer, cursor };
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

ALWAYS_INLINE bool isShortTextLatin1AfterDecoding(const JSONWords& words, const uint8_t* text, unsigned length)
{
    constexpr size_t stride = SIMD::stride<uint8_t>;
    static_assert(2 * stride <= jsonSourcePadding);
    ASSERT(length <= 2 * stride);
    constexpr simde_uint8x16_t lowIndices { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    constexpr simde_uint8x16_t highIndices { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    auto lengths = SIMD::splat<uint8_t>(length);
    auto largestUnchanged = SIMD::splat<uint8_t>(words.sourceEncoding == JSONSourceEncoding::UTF8 ? 0x7f : saturatedJSONCharacter - 1);
    auto low = SIMD::bitAnd(SIMD::lessThan(largestUnchanged, SIMD::load(text)), SIMD::lessThan(lowIndices, lengths));
    auto high = SIMD::bitAnd(SIMD::lessThan(largestUnchanged, SIMD::load(text + stride)), SIMD::lessThan(highIndices, lengths));
    return !SIMD::isNonZero(SIMD::bitOr(low, high));
}

ALWAYS_INLINE std::optional<ParsedJSONString> findCachedJSONString(const JSONWords& words, VM& vm, const uint8_t* quote)
{
    static_assert(JSONCache::maxNameTextLength <= 2 * SIMD::stride<uint8_t> && JSONCache::maxNameTextLength <= jsonSourcePadding);
    auto* entry = vm.jsonCache().findString(std::span<const Latin1Character> { byteCast<Latin1Character>(quote), jsonSourcePadding });
    if (!entry || (words.sourceEncoding != JSONSourceEncoding::Latin1 && !entry->textIsASCII && !isShortTextLatin1AfterDecoding(words, quote, entry->textLength)))
        return std::nullopt;
    return ParsedJSONString { entry->string, quote + entry->textLength };
}

ALWAYS_INLINE std::optional<ParsedJSONValue> tryParseJSONScalarAt(Instance& instance, VM& vm, const uint8_t* start)
{
    const JSONWords& words = instance.json;
    auto endsAfter = [&](size_t length, JSValue value) {
        return ParsedJSONValue { value, start + length };
    };
    switch (*start) {
    case '"': {
        std::optional<ParsedJSONString> cached = findCachedJSONString(words, vm, start);
        ParsedJSONString parsed = cached ? *cached : makeUncachedJSONString(instance, vm, start + 1);
        return parsed.end ? ParsedJSONValue { parsed.string, parsed.end } : ParsedJSONValue { };
    }
    case 't':
        if (WTF::unalignedLoad<uint32_t>(start) == ('t' | 'r' << 8 | 'u' << 16 | 'e' << 24))
            return endsAfter(4, jsBoolean(true));
        return std::nullopt;
    case 'f':
        if (WTF::unalignedLoad<uint32_t>(start + 1) == ('a' | 'l' << 8 | 's' << 16 | 'e' << 24))
            return endsAfter(5, jsBoolean(false));
        return std::nullopt;
    case 'n':
        if (WTF::unalignedLoad<uint32_t>(start) == ('n' | 'u' << 8 | 'l' << 16 | 'l' << 24))
            return endsAfter(4, jsNull());
        return std::nullopt;
    case '{':
    case '[':
        return std::nullopt;
    default:
        if (ParsedJSONNumber parsed = parseJSONNumber(start, words.sourceEnd); parsed.end)
            return ParsedJSONValue { jsNumber(parsed.number), parsed.end };
        return std::nullopt;
    }
}

JSONKeyComparison compareJSONKey(const JSONWords& words, const uint8_t* key, const uint8_t* closingQuote, bool hasEscapes, NOESCAPE const Invocable<bool(StringView)> auto& hasText)
{
    auto compareWith = [&](StringView text) {
        return hasText(text) ? JSONKeyComparison::Equal : JSONKeyComparison::Different;
    };
    if (hasEscapes) [[unlikely]] {
        StringBuilder builder(OverflowPolicy::RecordOverflow);
        if (!decodeJSONString(words, key, findEndOfPlainJSONCharacters(key), builder) || builder.hasOverflowed())
            return JSONKeyComparison::Unknown;
        return compareWith(builder);
    }
    std::span<const uint8_t> characters { key, closingQuote };
    if (isLatin1(words, characters)) [[likely]]
        return compareWith(StringView { byteCast<Latin1Character>(characters) });
    if (words.sourceEncoding == JSONSourceEncoding::UTF16)
        return compareWith(StringView { original16BitCharacters(words, characters) });
    if (!StringImpl::isValidLength<char16_t>(characters.size()))
        return JSONKeyComparison::Unknown;
    String decoded = String::fromUTF8(characters);
    if (decoded.isNull())
        return JSONKeyComparison::Unknown;
    return compareWith(decoded);
}

ALWAYS_INLINE ComparedJSONMember compareJSONMemberKey(const JSONWords& words, const uint8_t* cursor, NOESCAPE const Invocable<bool(StringView)> auto& hasText)
{
    if (*cursor != '"')
        return { };
    const uint8_t* key = cursor + 1;
    bool hasEscapes = false;
    const uint8_t* afterKey = skipJSONString(key, hasEscapes);
    if (!afterKey)
        return { };
    const uint8_t* colon = skipJSONWhitespaceCharacters(afterKey);
    if (*colon != ':')
        return { };
    return { compareJSONKey(words, key, afterKey - 1, hasEscapes, hasText), skipJSONWhitespaceCharacters(colon + 1) };
}

FoundJSONMember findJSONMember(const JSONWords& words, const uint8_t* cursor, NOESCAPE const Invocable<bool(StringView)> auto& hasText)
{
    cursor = skipJSONWhitespaceCharacters(cursor);
    if (*cursor == '}')
        return { Typed::JSONMemberSearch::Absent };
    for (;;) {
        ComparedJSONMember member = compareJSONMemberKey(words, cursor, hasText);
        switch (member.comparison) {
        case JSONKeyComparison::Unknown:
            return { };
        case JSONKeyComparison::Equal:
            return { Typed::JSONMemberSearch::Found, member.value };
        case JSONKeyComparison::Different:
            break;
        }
        cursor = skipJSONValue(member.value);
        if (!cursor)
            return { };
        cursor = skipJSONWhitespaceCharacters(cursor);
        if (*cursor == '}')
            return { Typed::JSONMemberSearch::Absent };
        if (*cursor != ',')
            return { };
        cursor = skipJSONWhitespaceCharacters(cursor + 1);
    }
}

bool jsonMemberRepeats(const JSONWords& words, const uint8_t* cursor, NOESCAPE const Invocable<bool(StringView)> auto& hasText)
{
    while (cursor) {
        cursor = skipJSONWhitespaceCharacters(cursor);
        if (*cursor == '}')
            return false;
        if (*cursor != ',')
            return true;
        ComparedJSONMember member = compareJSONMemberKey(words, skipJSONWhitespaceCharacters(cursor + 1), hasText);
        if (member.comparison != JSONKeyComparison::Different)
            return true;
        cursor = skipJSONValue(member.value);
    }
    return true;
}

inline bool jsonKeyRepeats(const JSONWords& words, const uint8_t* key, const uint8_t* value)
{
    bool mayRepeat = true;
    compareJSONMemberKey(words, key, [&](StringView name) {
        mayRepeat = jsonMemberRepeats(words, skipJSONValue(value), [&](StringView text) {
            return text == name;
        });
        return true;
    });
    return mayRepeat;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
