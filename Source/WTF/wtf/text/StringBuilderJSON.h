/*
 * Copyright (C) 2010-2018 Apple Inc. All rights reserved.
 * Copyright (C) 2012 Google Inc. All rights reserved.
 * Copyright (C) 2017 Yusuke Suzuki <utatane.tea@gmail.com>. All rights reserved.
 * Copyright (C) 2017 Mozilla Foundation. All rights reserved.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <wtf/SIMDHelpers.h>
#include <wtf/text/EscapedFormsForJSON.h>
#include <wtf/text/ParsingUtilities.h>
#include <wtf/text/StringBuilderInternals.h>
#include <wtf/text/WTFString.h>

namespace WTF {

// Appends the first character of the input, which is not empty, escaped if it has to be. (Two, if they are a surrogate pair.)
template<typename OutputCharacterType, typename InputCharacterType>
ALWAYS_INLINE static bool appendOneEscapedJSONCharacter(std::span<OutputCharacterType>& output, std::span<const InputCharacterType>& input)
{
    auto character = input.front();
    if (character <= 0xFF) [[likely]] {
        auto escaped = escapedFormsForJSON[character];
        if (!escaped) [[likely]] {
            consume(output) = character;
            skip(input, 1);
            return true;
        }

        output[0] = '\\';
        output[1] = escaped;
        skip(output, 2);
        if (escaped == 'u') [[unlikely]] {
            output[0] = '0';
            output[1] = '0';
            output[2] = upperNibbleToLowercaseASCIIHexDigit(character);
            output[3] = lowerNibbleToLowercaseASCIIHexDigit(character);
            skip(output, 4);
        }
        skip(input, 1);
        return true;
    }

    // We can end up calling appendEscapedJSONStringContent if we've already proven the string has only Latin1 characters when stringifying JSONs.
    // This optimization prevents us from bailing out mid-stream just because we saw e.g. a UTF-16 substring that was actually Latin1.
    if constexpr (std::same_as<OutputCharacterType, Latin1Character>)
        return false;

    if (!U16_IS_SURROGATE(character)) [[likely]] {
        consume(output) = character;
        skip(input, 1);
        return true;
    }

    if (input.size() > 1) {
        auto next = input[1];
        bool isValidSurrogatePair = U16_IS_SURROGATE_LEAD(character) && U16_IS_TRAIL(next);
        if (isValidSurrogatePair) {
            output[0] = character;
            output[1] = next;
            skip(output, 2);
            skip(input, 2);
            return true;
        }
    }

    uint8_t upper = static_cast<uint32_t>(character) >> 8;
    uint8_t lower = static_cast<uint8_t>(character);
    output[0] = '\\';
    output[1] = 'u';
    output[2] = upperNibbleToLowercaseASCIIHexDigit(upper);
    output[3] = lowerNibbleToLowercaseASCIIHexDigit(upper);
    output[4] = upperNibbleToLowercaseASCIIHexDigit(lower);
    output[5] = lowerNibbleToLowercaseASCIIHexDigit(lower);
    skip(output, 6);
    skip(input, 1);
    return true;
}

// The output has room for six characters for each one of the input.
template<typename OutputCharacterType, typename InputCharacterType>
ALWAYS_INLINE static bool appendEscapedJSONStringContent(std::span<OutputCharacterType>& output, std::span<const InputCharacterType> input)
{
#if (CPU(ARM64) || CPU(X86_64)) && COMPILER(CLANG)
    // Text that has something to escape in it is still mostly text that goes as it is. That is copied a vector at a time, up to the next character that wants looking at.
    using InputLane = SameSizeUnsignedInteger<InputCharacterType>;
    using OutputLane = SameSizeUnsignedInteger<OutputCharacterType>;
    constexpr size_t stride = SIMD::stride<InputLane>;
    constexpr auto quoteMask = SIMD::splat<InputLane>('"');
    constexpr auto escapeMask = SIMD::splat<InputLane>('\\');
    constexpr auto controlMask = SIMD::splat<InputLane>(' ');
    while (input.size() >= stride) {
        auto vector = SIMD::load(std::bit_cast<const InputLane*>(input.data()));
        auto wantsLookingAt = SIMD::bitOr(SIMD::equal(vector, quoteMask), SIMD::equal(vector, escapeMask), SIMD::lessThan(vector, controlMask));
        if constexpr (sizeof(InputCharacterType) != 1) {
            if constexpr (sizeof(OutputCharacterType) == 1)
                wantsLookingAt = SIMD::bitOr(wantsLookingAt, SIMD::greaterThan(vector, SIMD::splat<InputLane>(0xff)));
            else
                wantsLookingAt = SIMD::bitOr(wantsLookingAt, SIMD::equal(SIMD::bitAnd(vector, SIMD::splat<InputLane>(0xf800)), SIMD::splat<InputLane>(0xd800)));
        }
        // (All of it: what comes after a character that wants looking at is written over.)
        auto* destination = std::bit_cast<OutputLane*>(output.data());
        if constexpr (sizeof(InputCharacterType) == sizeof(OutputCharacterType))
            SIMD::store(vector, destination);
        else if constexpr (sizeof(InputCharacterType) == 1)
            simde_vst2q_u8(std::bit_cast<uint8_t*>(destination), (simde_uint8x16x2_t { vector, SIMD::splat<uint8_t>(0) }));
        else
            simde_vst1_u8(destination, simde_vmovn_u16(vector));
        auto index = SIMD::findFirstNonZeroIndex(wantsLookingAt);
        if (!index) [[likely]] {
            skip(input, stride);
            skip(output, stride);
            continue;
        }
        skip(input, *index);
        skip(output, *index);
        if (!appendOneEscapedJSONCharacter(output, input)) [[unlikely]]
            return false;
    }
#endif
    while (!input.empty()) {
        if (!appendOneEscapedJSONCharacter(output, input)) [[unlikely]]
            return false;
    }
    return true;
}

} // namespace WTF
