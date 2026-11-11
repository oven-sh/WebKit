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

ALWAYS_INLINE std::optional<double> valueOfTextOfFewDigits(JSValue value)
{
    if (!value.isString())
        return std::nullopt;
    const StringImpl* impl = asString(value)->tryGetValueImpl();
    if (!impl || !impl->is8Bit())
        return std::nullopt;
    return valueOfFewDigits(impl->span8());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
