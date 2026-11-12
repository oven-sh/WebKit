/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSString.h"
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <wtf/UnalignedAccess.h>

namespace JSC { namespace AOT {

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

ALWAYS_INLINE bool areDigitValues(uint64_t bytes)
{
    return !((bytes | (bytes + 0x7676767676767676ULL)) & 0x8080808080808080ULL);
}

ALWAYS_INLINE uint32_t valueOfEightDigits(uint64_t digits)
{
    constexpr uint64_t lowByteOfEachHalf = 0x000000ff000000ffULL;
    digits = digits * 10 + (digits >> 8);
    return ((digits & lowByteOfEachHalf) * (100 + (1000000ULL << 32)) + ((digits >> 16) & lowByteOfEachHalf) * (1 + (10000ULL << 32))) >> 32;
}

ALWAYS_INLINE std::optional<double> valueOfFewDigits(std::span<const Latin1Character> characters)
{
    static_assert(std::endian::native == std::endian::little);
    constexpr size_t maxNumberOfDigits = 15;
    constexpr uint64_t zeroInEachByte = 0x3030303030303030ULL;
    const Latin1Character* start = characters.data();
    size_t length = characters.size();
    if (length - 4 <= 4) {
        uint64_t bytes = WTF::unalignedLoad<uint32_t>(start) | static_cast<uint64_t>(WTF::unalignedLoad<uint32_t>(start + length - 4)) << 8 * (length - 4);
        uint64_t digits = (bytes ^ zeroInEachByte) << 8 * (8 - length);
        if (!areDigitValues(digits))
            return std::nullopt;
        return static_cast<double>(static_cast<int32_t>(valueOfEightDigits(digits)));
    }
    if (!length || length > maxNumberOfDigits)
        return std::nullopt;
    if (length > 8) {
        static constexpr std::array<uint32_t, 8> powersOfTen { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000 };
        unsigned bitsOfOverlap = 8 * (16 - length);
        uint64_t digits = WTF::unalignedLoad<uint64_t>(start) ^ zeroInEachByte;
        uint64_t lastDigits = (WTF::unalignedLoad<uint64_t>(start + length - 8) ^ zeroInEachByte) >> bitsOfOverlap << bitsOfOverlap;
        if (!areDigitValues(digits) || !areDigitValues(lastDigits))
            return std::nullopt;
        return static_cast<double>(static_cast<int64_t>(static_cast<uint64_t>(valueOfEightDigits(digits)) * powersOfTen[length - 8] + valueOfEightDigits(lastDigits)));
    }
    int32_t whole = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned digit = start[i] - '0';
        if (digit > 9)
            return std::nullopt;
        whole = whole * 10 + digit;
    }
    return static_cast<double>(whole);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

ALWAYS_INLINE std::optional<std::span<const Latin1Character>> latin1CharactersWithoutResolving(const JSString* string)
{
    if (const StringImpl* impl = string->tryGetValueImpl()) [[likely]]
        return impl->is8Bit() ? std::optional(impl->span8()) : std::nullopt;
    if (!string->isSubstring())
        return std::nullopt;
    auto* part = static_cast<const JSRopeString*>(string);
    const StringImpl* base = part->substringBase()->tryGetValueImpl();
    return base->is8Bit() ? std::optional(base->span8().subspan(part->substringOffset(), part->length())) : std::nullopt;
}

ALWAYS_INLINE std::optional<double> valueOfTextOfFewDigits(JSValue value)
{
    if (!value.isString())
        return std::nullopt;
    auto characters = latin1CharactersWithoutResolving(asString(value));
    return characters ? valueOfFewDigits(*characters) : std::nullopt;
}

ALWAYS_INLINE std::optional<double> valueOfShortDecimal(std::span<const Latin1Character> characters)
{
    constexpr size_t maxNumberOfDigits = 15;
    size_t start = !characters.empty() && characters[0] == '-';
    if (characters.size() > maxNumberOfDigits)
        return std::nullopt;
    int64_t digits = 0;
    int64_t divisor = 0;
    size_t numberOfDigits = 0;
    for (Latin1Character character : characters.subspan(start)) {
        unsigned digit = character - '0';
        if (digit > 9) {
            if (character != '.' || divisor)
                return std::nullopt;
            divisor = 1;
            continue;
        }
        digits = digits * 10 + digit;
        divisor *= 10;
        ++numberOfDigits;
    }
    if (!numberOfDigits)
        return std::nullopt;
    double value = divisor ? static_cast<double>(digits) / static_cast<double>(divisor) : static_cast<double>(digits);
    return start ? -value : value;
}

ALWAYS_INLINE std::optional<double> numberOfShortText(std::span<const Latin1Character> characters)
{
    if (std::optional<double> value = valueOfFewDigits(characters))
        return value;
    if (characters.empty())
        return 0;
    if (isASCIIAlpha(characters.front()) && characters.front() != 'I')
        return PNaN;
    return valueOfShortDecimal(characters);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
