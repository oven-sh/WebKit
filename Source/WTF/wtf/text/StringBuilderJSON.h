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

// Per-character JSON escaping loop. Takes and returns the output span by value so the
// hot state stays in registers regardless of inlining decisions; returns the remaining
// output span. When the output is Latin1 and a non-Latin1 character is encountered,
// returns a default-constructed (null data) span; output may have been partially
// written in that case.
//
// standaloneInstantiation gives the vectorized path its own copy of this function.
// That keeps the copy handling short strings in appendEscapedJSONStringContent
// single-use, so it stays inlined into callers even when ALWAYS_INLINE is only a
// hint (see Compiler.h).
template<typename OutputCharacterType, typename InputCharacterType, bool standaloneInstantiation = false>
ALWAYS_INLINE static std::span<OutputCharacterType> appendEscapedJSONStringContentScalar(std::span<OutputCharacterType> output, std::span<const InputCharacterType> input)
{
    for (; !input.empty(); skip(input, 1)) {
        auto character = input.front();
        if (character <= 0xFF) [[likely]] {
            auto escaped = escapedFormsForJSON[character];
            if (!escaped) [[likely]] {
                consume(output) = character;
                continue;
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
            continue;
        }

        // We can end up calling appendEscapedJSONStringContent if we've already proven the string has only Latin1 characters when stringifying JSONs.
        // This optimization prevents us from bailing out mid-stream just because we saw e.g. a UTF-16 substring that was actually Latin1.
        if constexpr (std::same_as<OutputCharacterType, Latin1Character>)
            return { };

        if (!U16_IS_SURROGATE(character)) [[likely]] {
            consume(output) = character;
            continue;
        }

        if (input.size() > 1) {
            auto next = input[1];
            bool isValidSurrogatePair = U16_IS_SURROGATE_LEAD(character) && U16_IS_TRAIL(next);
            if (isValidSurrogatePair) {
                output[0] = character;
                output[1] = next;
                skip(output, 2);
                skip(input, 1);
                continue;
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
    }

    return output;
}

#if (CPU(ARM64) || CPU(X86_64)) && COMPILER(CLANG)

// True for characters the vectorized scan in appendEscapedJSONStringContentVector stops
// on: characters needing a JSON escape, plus (for 16-bit input) surrogates, which need
// pair validation. Keep in sync with the vector masks below.
template<typename InputCharacterType>
ALWAYS_INLINE static bool requiresJSONSpecialHandling(InputCharacterType character)
{
    if (character <= 0xFF)
        return !!escapedFormsForJSON[character];
    return U16_IS_SURROGATE(character);
}

// Process a vector stride at a time: copy the stride to output, scan it for characters
// needing special handling, and only drop to the per-character loop for runs of such
// characters, resuming the vectorized copy right after each run. This keeps strings
// containing a few escapes or surrogate pairs close to memcpy speed instead of handling
// every character individually.
//
// A separate NEVER_INLINE function (with spans passed and returned by value) so the
// vector code doesn't degrade the code generation of callers appending short strings.
template<typename OutputCharacterType, typename InputCharacterType>
NEVER_INLINE static std::span<OutputCharacterType> appendEscapedJSONStringContentVector(std::span<OutputCharacterType> output, std::span<const InputCharacterType> input)
{
    // The Latin1 output fed 16-bit input case never takes this path, so the scalar loop
    // below can never hit its non-Latin1 bail and return a null span.
    static_assert(!(std::same_as<OutputCharacterType, Latin1Character> && sizeof(InputCharacterType) == 2));
    using UnsignedType = SameSizeUnsignedInteger<InputCharacterType>;
    constexpr size_t stride = SIMD::stride<InputCharacterType>;
    constexpr auto quoteMask = SIMD::splat<UnsignedType>('"');
    constexpr auto escapeMask = SIMD::splat<UnsignedType>('\\');
    constexpr auto controlMask = SIMD::splat<UnsignedType>(' ');
    while (input.size() >= stride) {
        auto chunk = SIMD::load(std::bit_cast<const UnsignedType*>(input.data()));
        // Copy before checking: callers guarantee output has room for the worst-case
        // expansion, so writing a full stride is safe, and on a hit the clean prefix
        // of the stride is already in place.
        if constexpr (sizeof(OutputCharacterType) == sizeof(InputCharacterType))
            SIMD::store(chunk, std::bit_cast<UnsignedType*>(output.data()));
        else {
            static_assert(std::same_as<InputCharacterType, Latin1Character> && std::same_as<OutputCharacterType, char16_t>);
            constexpr auto zeros = SIMD::splat<UnsignedType>(0);
            simde_vst2q_u8(std::bit_cast<UnsignedType*>(output.data()), (simde_uint8x16x2_t { chunk, zeros }));
        }
        auto quotes = SIMD::equal(chunk, quoteMask);
        auto escapes = SIMD::equal(chunk, escapeMask);
        auto controls = SIMD::lessThan(chunk, controlMask);
        auto mask = SIMD::bitOr(quotes, escapes, controls);
        if constexpr (sizeof(InputCharacterType) != 1) {
            constexpr auto surrogateMask = SIMD::splat<UnsignedType>(0xf800);
            constexpr auto surrogateCheckMask = SIMD::splat<UnsignedType>(0xd800);
            mask = SIMD::bitOr(mask, SIMD::equal(SIMD::bitAnd(chunk, surrogateMask), surrogateCheckMask));
        }
        auto index = SIMD::findFirstNonZeroIndex(mask);
        if (!index) [[likely]] {
            skip(input, stride);
            skip(output, stride);
            continue;
        }
        // The first *index characters of the stride were clean and are already copied.
        skip(input, *index);
        skip(output, *index);
        // Hand the whole run of characters needing special handling to the per-character
        // loop. A surrogate pair never straddles the end of the run, because a trail
        // surrogate is itself a character needing special handling.
        size_t runLength = 1;
        while (runLength < input.size() && requiresJSONSpecialHandling(input[runLength]))
            ++runLength;
        output = appendEscapedJSONStringContentScalar<OutputCharacterType, InputCharacterType, true>(output, input.first(runLength));
        skip(input, runLength);
    }
    return appendEscapedJSONStringContentScalar<OutputCharacterType, InputCharacterType, true>(output, input);
}

#endif

// Writes input to output, escaping as needed for JSON. Callers must guarantee that
// output has room for the worst-case expansion: 6 output characters per input
// character; the bulk stores in the vectorized path rely on that headroom to write
// a full vector stride even when the scan stops before the end of the stride.
// Returns false only when the output is Latin1 and a non-Latin1 character is
// encountered; output may have been partially written in that case.
template<typename OutputCharacterType, typename InputCharacterType>
ALWAYS_INLINE static bool appendEscapedJSONStringContent(std::span<OutputCharacterType>& output, std::span<const InputCharacterType> input)
{
    if (input.empty())
        return true;
#if (CPU(ARM64) || CPU(X86_64)) && COMPILER(CLANG)
    // The 16-bit input to Latin1 output case stays on the per-character loop: it needs a
    // narrowing store, and it almost always fails over to the 16-bit stringifier anyway.
    if constexpr (!(std::same_as<OutputCharacterType, Latin1Character> && sizeof(InputCharacterType) == 2)) {
        if (input.size() >= SIMD::stride<InputCharacterType>) {
            output = appendEscapedJSONStringContentVector(output, input);
            return true;
        }
    }
#endif
    auto result = appendEscapedJSONStringContentScalar(output, input);
    if (!result.data()) [[unlikely]]
        return false;
    output = result;
    return true;
}

} // namespace WTF
