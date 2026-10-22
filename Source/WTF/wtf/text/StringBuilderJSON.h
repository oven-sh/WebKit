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

#if CPU(ARM64) && COMPILER(CLANG)
// A shuffle table for eight input characters, indexed by the mask of the characters that need a backslash. Each output byte selects
// its source. 0 to 7: the characters. 8 to 15: the escaped form of each character, which follows its backslash. 16: a backslash.
struct JSONEscapeExpansion {
    alignas(16) uint8_t shuffle[256][16];
    uint8_t length[256];
};
inline constexpr JSONEscapeExpansion jsonEscapeExpansion = [] {
    JSONEscapeExpansion result { };
    for (unsigned mask = 0; mask < 256; ++mask) {
        unsigned length = 0;
        for (unsigned i = 0; i < 8; ++i) {
            if (mask >> i & 1) {
                result.shuffle[mask][length++] = 16;
                result.shuffle[mask][length++] = 8 + i;
            } else
                result.shuffle[mask][length++] = i;
        }
        result.length[mask] = length;
        while (length < 16)
            result.shuffle[mask][length++] = 0xff;
    }
    return result;
}();

// As many whole vectors as there are in the first `count` characters. Text has a line break every few dozen characters: whether a vector has something to escape in it is not to be
// guessed, so nothing hangs on it. skipsCleanPages: for text that has next to nothing to escape, where it is.
template<bool skipsCleanPages, typename OutputCharacterType>
ALWAYS_INLINE static void appendEscapedJSONVectors(std::span<OutputCharacterType>& output, std::span<const Latin1Character>& input, size_t count)
{
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    alignas(16) static constexpr uint8_t shortForms[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 'b', 't', 'n', 0, 'f', 'r', 0, 0 };
    alignas(16) static constexpr uint8_t weights[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    const auto backslashes = simde_vdupq_n_u8('\\');
    const auto forms = simde_vld1q_u8(shortForms);
    const auto weight = simde_vld1q_u8(weights);
    const uint8_t* from = std::bit_cast<const uint8_t*>(input.data());
    const uint8_t* end = from + (count & ~static_cast<size_t>(15));
    OutputCharacterType* to = output.data();
    auto put = [&](simde_uint8x16_t characters) ALWAYS_INLINE_LAMBDA {
        if constexpr (sizeof(OutputCharacterType) == 1)
            simde_vst1q_u8(std::bit_cast<uint8_t*>(to), characters);
        else
            simde_vst2q_u8(std::bit_cast<uint8_t*>(to), (simde_uint8x16x2_t { characters, simde_vdupq_n_u8(0) }));
    };
    for (; from != end; from += 16) {
        auto characters = simde_vld1q_u8(from);
        auto controls = simde_vcltq_u8(characters, simde_vdupq_n_u8(' '));
        auto escaped = simde_vorrq_u8(simde_vorrq_u8(simde_vceqq_u8(characters, simde_vdupq_n_u8('"')), simde_vceqq_u8(characters, backslashes)), controls);
        auto bits = simde_vandq_u8(escaped, weight);
        unsigned low = simde_vaddv_u8(simde_vget_low_u8(bits));
        unsigned high = simde_vaddv_u8(simde_vget_high_u8(bits));
        if constexpr (skipsCleanPages) {
            if (!(low | high)) {
                put(characters);
                to += 16;
                continue;
            }
        }
        // The form of each character after its backslash: the character itself, or its escape letter. (Most control characters have
        // none.)
        auto written = simde_vbslq_u8(controls, simde_vqtbl1q_u8(forms, characters), characters);
        if (simde_vmaxvq_u8(simde_vandq_u8(controls, simde_vceqzq_u8(written)))) [[unlikely]] {
            std::span<OutputCharacterType> room { to, 16 * 6 };
            std::span<const Latin1Character> these { std::bit_cast<const Latin1Character*>(from), 16 };
            while (!these.empty())
                appendOneEscapedJSONCharacter(room, these);
            to = room.data();
            continue;
        }
        put(simde_vqtbl2q_u8((simde_uint8x16x2_t { simde_vcombine_u8(simde_vget_low_u8(characters), simde_vget_low_u8(written)), backslashes }), simde_vld1q_u8(jsonEscapeExpansion.shuffle[low])));
        to += jsonEscapeExpansion.length[low];
        put(simde_vqtbl2q_u8((simde_uint8x16x2_t { simde_vcombine_u8(simde_vget_high_u8(characters), simde_vget_high_u8(written)), backslashes }), simde_vld1q_u8(jsonEscapeExpansion.shuffle[high])));
        to += jsonEscapeExpansion.length[high];
    }
    skip(output, to - output.data());
    skip(input, from - std::bit_cast<const uint8_t*>(input.data()));
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
}
#endif

#if CPU(X86_64) && COMPILER(CLANG)
// JSON inside JSON, markup with attributes: a quote every few characters. Looking for the next one and copying up to it costs thirty instructions a quote.
// Here eight characters are spread at once: a shuffle for each of the 256 ways in which they may want a backslash. Lanes 0 to 7 are the characters, lane 8 is a backslash.
struct BackslashExpansion {
    alignas(16) uint8_t shuffle[256][16];
    uint8_t length[256];
};
inline constexpr BackslashExpansion backslashExpansion = [] {
    BackslashExpansion result { };
    for (unsigned mask = 0; mask < 256; ++mask) {
        unsigned length = 0;
        for (unsigned i = 0; i < 8; ++i) {
            if (mask >> i & 1)
                result.shuffle[mask][length++] = 8;
            result.shuffle[mask][length++] = i;
        }
        result.length[mask] = length;
        while (length < 16)
            result.shuffle[mask][length++] = 0;
    }
    return result;
}();

// Whole vectors. A quote and a backslash follow their backslash as they are, a line break and the four other control characters with a letter of their own as that letter.
// A vector that holds another control character is left to the caller.
static ALWAYS_INLINE void appendVectorsWithBackslashes(std::span<Latin1Character>& output, std::span<const Latin1Character>& input)
{
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    constexpr size_t stride = SIMD::stride<uint8_t>;
    constexpr auto quotes = SIMD::splat<uint8_t>('"');
    constexpr auto backslashes = SIMD::splat<uint8_t>('\\');
    constexpr auto spaces = SIMD::splat<uint8_t>(' ');
    alignas(16) static constexpr uint8_t lettersOfControlCharacters[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 'b', 't', 'n', 0, 'f', 'r', 0, 0 };
    const uint8_t* from = std::bit_cast<const uint8_t*>(input.data());
    const uint8_t* end = from + (input.size() & ~(stride - 1));
    uint8_t* to = std::bit_cast<uint8_t*>(output.data());
    for (; from != end; from += stride) {
        auto characters = SIMD::load(from);
        unsigned mask = static_cast<uint16_t>(simde_mm_movemask_epi8(simde_uint8x16_to_m128i(SIMD::bitOr(SIMD::equal(characters, quotes), SIMD::equal(characters, backslashes)))));
        auto controls = SIMD::lessThan(characters, spaces);
        if (SIMD::isNonZero(controls)) [[unlikely]] {
            // (The shuffle goes by the low four bits: 0x18 would find the letter of 0x08.)
            simde__m128i letters = simde_mm_shuffle_epi8(simde_mm_load_si128(std::bit_cast<const simde__m128i*>(&lettersOfControlCharacters[0])), simde_uint8x16_to_m128i(characters));
            auto hasNoLetter = SIMD::bitAnd(controls, SIMD::bitOr(SIMD::equal(simde_uint8x16_from_m128i(letters), SIMD::splat<uint8_t>(0)), SIMD::greaterThan(characters, SIMD::splat<uint8_t>(0x0f))));
            if (SIMD::isNonZero(hasNoLetter)) [[unlikely]]
                break;
            characters = simde_uint8x16_from_m128i(simde_mm_blendv_epi8(simde_uint8x16_to_m128i(characters), letters, simde_uint8x16_to_m128i(controls)));
            mask |= static_cast<uint16_t>(simde_mm_movemask_epi8(simde_uint8x16_to_m128i(controls)));
        }
        if (!mask) {
            SIMD::store(characters, to);
            to += stride;
            continue;
        }
        unsigned low = mask & 0xff;
        unsigned high = mask >> 8;
        auto spread = [&](simde__m128i eightAndBackslashes, unsigned wantsBackslash) ALWAYS_INLINE_LAMBDA {
            simde__m128i shuffle = simde_mm_load_si128(std::bit_cast<const simde__m128i*>(&backslashExpansion.shuffle[wantsBackslash][0]));
            simde_mm_storeu_si128(std::bit_cast<simde__m128i*>(to), simde_mm_shuffle_epi8(eightAndBackslashes, shuffle));
            to += backslashExpansion.length[wantsBackslash];
        };
        simde__m128i sixteen = simde_uint8x16_to_m128i(characters);
        simde__m128i fill = simde_uint8x16_to_m128i(backslashes);
        spread(simde_mm_unpacklo_epi64(sixteen, fill), low);
        spread(simde_mm_unpackhi_epi64(sixteen, fill), high);
    }
    skip(output, to - std::bit_cast<uint8_t*>(output.data()));
    skip(input, from - std::bit_cast<const uint8_t*>(input.data()));
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
}
#endif

#if CPU(X86_64) && COMPILER(CLANG)
// The same for an output of 16 bits a character: four characters are spread at once. Lanes 0 to 3 are the characters, lane 4 is a backslash.
struct WideBackslashExpansion {
    alignas(16) uint8_t shuffle[16][16];
    uint8_t length[16];
};
inline constexpr WideBackslashExpansion wideBackslashExpansion = [] {
    WideBackslashExpansion result { };
    for (unsigned mask = 0; mask < 16; ++mask) {
        unsigned length = 0;
        auto put = [&](unsigned lane) {
            result.shuffle[mask][2 * length] = 2 * lane;
            result.shuffle[mask][2 * length + 1] = 2 * lane + 1;
            ++length;
        };
        for (unsigned i = 0; i < 4; ++i) {
            if (mask >> i & 1)
                put(4);
            put(i);
        }
        result.length[mask] = length;
    }
    return result;
}();

// Eight characters at a time. Those that hold a surrogate, or a control character without a letter of its own, are left to the caller.
template<typename InputCharacterType>
static ALWAYS_INLINE void appendWideVectorsWithBackslashes(std::span<char16_t>& output, std::span<const InputCharacterType>& input)
{
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    constexpr size_t stride = 8;
    alignas(16) static constexpr uint8_t lettersOfControlCharacters[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 'b', 't', 'n', 0, 'f', 'r', 0, 0 };
    const simde__m128i zero = simde_mm_setzero_si128();
    const simde__m128i quotes = simde_mm_set1_epi16('"');
    const simde__m128i backslashes = simde_mm_set1_epi16('\\');
    const InputCharacterType* from = input.data();
    const InputCharacterType* end = from + (input.size() & ~(stride - 1));
    char16_t* to = output.data();
    for (; from != end; from += stride) {
        simde__m128i characters;
        simde__m128i surrogates = zero;
        if constexpr (sizeof(InputCharacterType) == 1)
            characters = simde_mm_unpacklo_epi8(simde_mm_loadl_epi64(std::bit_cast<const simde__m128i*>(from)), zero);
        else {
            characters = simde_mm_loadu_si128(std::bit_cast<const simde__m128i*>(from));
            surrogates = simde_mm_cmpeq_epi16(simde_mm_and_si128(characters, simde_mm_set1_epi16(static_cast<int16_t>(0xf800))), simde_mm_set1_epi16(static_cast<int16_t>(0xd800)));
        }
        simde__m128i wantsBackslash = simde_mm_or_si128(simde_mm_cmpeq_epi16(characters, quotes), simde_mm_cmpeq_epi16(characters, backslashes));
        simde__m128i controls = simde_mm_cmpeq_epi16(simde_mm_min_epu16(characters, simde_mm_set1_epi16(0x1f)), characters);
        simde__m128i wantsLookingAt = simde_mm_or_si128(simde_mm_or_si128(wantsBackslash, controls), surrogates);
        if (simde_mm_test_all_zeros(wantsLookingAt, wantsLookingAt)) {
            simde_mm_storeu_si128(std::bit_cast<simde__m128i*>(to), characters);
            to += stride;
            continue;
        }
        if (!simde_mm_test_all_zeros(surrogates, surrogates)) [[unlikely]]
            break;
        if (!simde_mm_test_all_zeros(controls, controls)) {
            // (The shuffle goes by the low four bits: 0x18 would find the letter of 0x08. A byte above 0x7f finds nothing, and is no control character.)
            simde__m128i letters = simde_mm_unpacklo_epi8(simde_mm_shuffle_epi8(simde_mm_load_si128(std::bit_cast<const simde__m128i*>(&lettersOfControlCharacters[0])), simde_mm_packus_epi16(characters, characters)), zero);
            simde__m128i hasNoLetter = simde_mm_and_si128(controls, simde_mm_or_si128(simde_mm_cmpeq_epi16(letters, zero), simde_mm_cmpgt_epi16(characters, simde_mm_set1_epi16(0x0f))));
            if (!simde_mm_test_all_zeros(hasNoLetter, hasNoLetter)) [[unlikely]]
                break;
            characters = simde_mm_blendv_epi8(characters, letters, controls);
            wantsBackslash = simde_mm_or_si128(wantsBackslash, controls);
        }
        unsigned mask = static_cast<uint8_t>(simde_mm_movemask_epi8(simde_mm_packs_epi16(wantsBackslash, zero)));
        auto spread = [&](simde__m128i fourAndBackslashes, unsigned wants) ALWAYS_INLINE_LAMBDA {
            simde__m128i shuffle = simde_mm_load_si128(std::bit_cast<const simde__m128i*>(&wideBackslashExpansion.shuffle[wants][0]));
            simde_mm_storeu_si128(std::bit_cast<simde__m128i*>(to), simde_mm_shuffle_epi8(fourAndBackslashes, shuffle));
            to += wideBackslashExpansion.length[wants];
        };
        spread(simde_mm_unpacklo_epi64(characters, backslashes), mask & 15);
        spread(simde_mm_unpackhi_epi64(characters, backslashes), mask >> 4);
    }
    skip(output, to - output.data());
    skip(input, from - input.data());
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
}
#endif

// The output has room for six characters for each one of the input.
template<typename OutputCharacterType, typename InputCharacterType>
ALWAYS_INLINE static bool appendEscapedJSONStringContent(std::span<OutputCharacterType>& output, std::span<const InputCharacterType> input)
{
#if CPU(ARM64) && COMPILER(CLANG)
    if constexpr (sizeof(InputCharacterType) == 1) {
        // (How the last stretch was says how the next is likely to be.)
        bool hasMuchToEscape = false;
        while (input.size() >= 16) {
            size_t count = std::min<size_t>(input.size(), 512);
            size_t before = input.size();
            auto* start = output.data();
            if (hasMuchToEscape)
                appendEscapedJSONVectors<false>(output, input, count);
            else
                appendEscapedJSONVectors<true>(output, input, count);
            size_t taken = before - input.size();
            hasMuchToEscape = (static_cast<size_t>(output.data() - start) - taken) * 64 > taken;
        }
        while (!input.empty())
            appendOneEscapedJSONCharacter(output, input);
        return true;
    }
#endif
#if CPU(X86_64) && COMPILER(CLANG)
    if constexpr (sizeof(InputCharacterType) == 1 && sizeof(OutputCharacterType) == 1) {
        constexpr size_t stride = SIMD::stride<uint8_t>;
        while (input.size() >= stride) {
            appendVectorsWithBackslashes(output, input);
            // The vector that stopped it, one character at a time.
            for (size_t i = 0; i < stride && !input.empty(); ++i)
                appendOneEscapedJSONCharacter(output, input);
        }
        while (!input.empty())
            appendOneEscapedJSONCharacter(output, input);
        return true;
    }
    if constexpr (sizeof(OutputCharacterType) == 2) {
        constexpr size_t stride = 8;
        while (input.size() >= stride) {
            appendWideVectorsWithBackslashes(output, input);
            // What stopped it, one character at a time. (A pair of surrogates is two.)
            size_t rest = input.size() > stride ? input.size() - stride : 0;
            while (input.size() > rest)
                appendOneEscapedJSONCharacter(output, input);
        }
        while (!input.empty())
            appendOneEscapedJSONCharacter(output, input);
        return true;
    }
#endif
#if (CPU(ARM64) || CPU(X86_64)) && COMPILER(CLANG)
    // Text that needs escaping still consists mostly of characters that are copied unchanged. Those are copied a vector at a time,
    // up to the next character that needs attention.
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
