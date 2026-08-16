/*
 * Copyright (C) 2016-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/URLParser.h>

#include <array>
#include <functional>
#include <wtf/SIMDHelpers.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/CodePointIterator.h>
#include <wtf/text/MakeString.h>

namespace WTF {

#define URL_PARSER_DEBUGGING 0

#if URL_PARSER_DEBUGGING
#define URL_PARSER_LOG(...) WTFLogAlways(__VA_ARGS__)
#else
#define URL_PARSER_LOG(...)
#endif

ALWAYS_INLINE static void appendCodePoint(Vector<char16_t>& destination, char32_t codePoint)
{
    if (U_IS_BMP(codePoint)) {
        destination.append(static_cast<char16_t>(codePoint));
        return;
    }
    destination.appendList({ U16_LEAD(codePoint), U16_TRAIL(codePoint) });
}

enum URLCharacterClass {
    UserInfoEncode = 0x1,
    PathEncode = 0x2,
    ForbiddenHost = 0x4,
    ForbiddenDomain = 0x8,
    QueryEncode = 0x10,
    SlashQuestionOrHash = 0x20,
    ValidScheme = 0x40,
};

static constexpr std::array<uint8_t, 256> characterClassTable {
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenHost | ForbiddenDomain, // 0x0
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x1
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x2
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x3
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x4
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x5
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x6
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x7
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x8
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenHost | ForbiddenDomain, // 0x9
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenHost | ForbiddenDomain, // 0xA
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0xB
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0xC
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenHost | ForbiddenDomain, // 0xD
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0xE
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0xF
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x10
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x11
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x12
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x13
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x14
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x15
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x16
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x17
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x18
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x19
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x1A
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x1B
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x1C
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x1D
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x1E
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenDomain, // 0x1F
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenHost | ForbiddenDomain, // ' '
    0, // '!'
    UserInfoEncode | PathEncode | QueryEncode, // '"'
    UserInfoEncode | PathEncode | QueryEncode | SlashQuestionOrHash | ForbiddenHost | ForbiddenDomain, // '#'
    0, // '$'
    ForbiddenDomain, // '%'
    0, // '&'
    0, // '\''
    0, // '('
    0, // ')'
    0, // '*'
    ValidScheme, // '+'
    0, // ','
    ValidScheme, // '-'
    ValidScheme, // '.'
    UserInfoEncode | SlashQuestionOrHash | ForbiddenHost | ForbiddenDomain, // '/'
    ValidScheme, // '0'
    ValidScheme, // '1'
    ValidScheme, // '2'
    ValidScheme, // '3'
    ValidScheme, // '4'
    ValidScheme, // '5'
    ValidScheme, // '6'
    ValidScheme, // '7'
    ValidScheme, // '8'
    ValidScheme, // '9'
    UserInfoEncode | ForbiddenHost | ForbiddenDomain, // ':'
    UserInfoEncode, // ';'
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenHost | ForbiddenDomain, // '<'
    UserInfoEncode, // '='
    UserInfoEncode | PathEncode | QueryEncode | ForbiddenHost | ForbiddenDomain, // '>'
    UserInfoEncode | PathEncode | SlashQuestionOrHash | ForbiddenHost | ForbiddenDomain, // '?'
    UserInfoEncode | ForbiddenHost | ForbiddenDomain, // '@'
    ValidScheme, // 'A'
    ValidScheme, // 'B'
    ValidScheme, // 'C'
    ValidScheme, // 'D'
    ValidScheme, // 'E'
    ValidScheme, // 'F'
    ValidScheme, // 'G'
    ValidScheme, // 'H'
    ValidScheme, // 'I'
    ValidScheme, // 'J'
    ValidScheme, // 'K'
    ValidScheme, // 'L'
    ValidScheme, // 'M'
    ValidScheme, // 'N'
    ValidScheme, // 'O'
    ValidScheme, // 'P'
    ValidScheme, // 'Q'
    ValidScheme, // 'R'
    ValidScheme, // 'S'
    ValidScheme, // 'T'
    ValidScheme, // 'U'
    ValidScheme, // 'V'
    ValidScheme, // 'W'
    ValidScheme, // 'X'
    ValidScheme, // 'Y'
    ValidScheme, // 'Z'
    UserInfoEncode | ForbiddenHost | ForbiddenDomain, // '['
    UserInfoEncode | SlashQuestionOrHash | ForbiddenHost | ForbiddenDomain, // '\\'
    UserInfoEncode | ForbiddenHost | ForbiddenDomain, // ']'
    UserInfoEncode | PathEncode | ForbiddenHost | ForbiddenDomain, // '^'
    0, // '_'
    UserInfoEncode | PathEncode, // '`'
    ValidScheme, // 'a'
    ValidScheme, // 'b'
    ValidScheme, // 'c'
    ValidScheme, // 'd'
    ValidScheme, // 'e'
    ValidScheme, // 'f'
    ValidScheme, // 'g'
    ValidScheme, // 'h'
    ValidScheme, // 'i'
    ValidScheme, // 'j'
    ValidScheme, // 'k'
    ValidScheme, // 'l'
    ValidScheme, // 'm'
    ValidScheme, // 'n'
    ValidScheme, // 'o'
    ValidScheme, // 'p'
    ValidScheme, // 'q'
    ValidScheme, // 'r'
    ValidScheme, // 's'
    ValidScheme, // 't'
    ValidScheme, // 'u'
    ValidScheme, // 'v'
    ValidScheme, // 'w'
    ValidScheme, // 'x'
    ValidScheme, // 'y'
    ValidScheme, // 'z'
    UserInfoEncode | PathEncode, // '{'
    UserInfoEncode | ForbiddenHost | ForbiddenDomain, // '|'
    UserInfoEncode | PathEncode, // '}'
    0, // '~'
    QueryEncode | ForbiddenDomain, // 0x7F
    QueryEncode, // 0x80
    QueryEncode, // 0x81
    QueryEncode, // 0x82
    QueryEncode, // 0x83
    QueryEncode, // 0x84
    QueryEncode, // 0x85
    QueryEncode, // 0x86
    QueryEncode, // 0x87
    QueryEncode, // 0x88
    QueryEncode, // 0x89
    QueryEncode, // 0x8A
    QueryEncode, // 0x8B
    QueryEncode, // 0x8C
    QueryEncode, // 0x8D
    QueryEncode, // 0x8E
    QueryEncode, // 0x8F
    QueryEncode, // 0x90
    QueryEncode, // 0x91
    QueryEncode, // 0x92
    QueryEncode, // 0x93
    QueryEncode, // 0x94
    QueryEncode, // 0x95
    QueryEncode, // 0x96
    QueryEncode, // 0x97
    QueryEncode, // 0x98
    QueryEncode, // 0x99
    QueryEncode, // 0x9A
    QueryEncode, // 0x9B
    QueryEncode, // 0x9C
    QueryEncode, // 0x9D
    QueryEncode, // 0x9E
    QueryEncode, // 0x9F
    QueryEncode, // 0xA0
    QueryEncode, // 0xA1
    QueryEncode, // 0xA2
    QueryEncode, // 0xA3
    QueryEncode, // 0xA4
    QueryEncode, // 0xA5
    QueryEncode, // 0xA6
    QueryEncode, // 0xA7
    QueryEncode, // 0xA8
    QueryEncode, // 0xA9
    QueryEncode, // 0xAA
    QueryEncode, // 0xAB
    QueryEncode, // 0xAC
    QueryEncode, // 0xAD
    QueryEncode, // 0xAE
    QueryEncode, // 0xAF
    QueryEncode, // 0xB0
    QueryEncode, // 0xB1
    QueryEncode, // 0xB2
    QueryEncode, // 0xB3
    QueryEncode, // 0xB4
    QueryEncode, // 0xB5
    QueryEncode, // 0xB6
    QueryEncode, // 0xB7
    QueryEncode, // 0xB8
    QueryEncode, // 0xB9
    QueryEncode, // 0xBA
    QueryEncode, // 0xBB
    QueryEncode, // 0xBC
    QueryEncode, // 0xBD
    QueryEncode, // 0xBE
    QueryEncode, // 0xBF
    QueryEncode, // 0xC0
    QueryEncode, // 0xC1
    QueryEncode, // 0xC2
    QueryEncode, // 0xC3
    QueryEncode, // 0xC4
    QueryEncode, // 0xC5
    QueryEncode, // 0xC6
    QueryEncode, // 0xC7
    QueryEncode, // 0xC8
    QueryEncode, // 0xC9
    QueryEncode, // 0xCA
    QueryEncode, // 0xCB
    QueryEncode, // 0xCC
    QueryEncode, // 0xCD
    QueryEncode, // 0xCE
    QueryEncode, // 0xCF
    QueryEncode, // 0xD0
    QueryEncode, // 0xD1
    QueryEncode, // 0xD2
    QueryEncode, // 0xD3
    QueryEncode, // 0xD4
    QueryEncode, // 0xD5
    QueryEncode, // 0xD6
    QueryEncode, // 0xD7
    QueryEncode, // 0xD8
    QueryEncode, // 0xD9
    QueryEncode, // 0xDA
    QueryEncode, // 0xDB
    QueryEncode, // 0xDC
    QueryEncode, // 0xDD
    QueryEncode, // 0xDE
    QueryEncode, // 0xDF
    QueryEncode, // 0xE0
    QueryEncode, // 0xE1
    QueryEncode, // 0xE2
    QueryEncode, // 0xE3
    QueryEncode, // 0xE4
    QueryEncode, // 0xE5
    QueryEncode, // 0xE6
    QueryEncode, // 0xE7
    QueryEncode, // 0xE8
    QueryEncode, // 0xE9
    QueryEncode, // 0xEA
    QueryEncode, // 0xEB
    QueryEncode, // 0xEC
    QueryEncode, // 0xED
    QueryEncode, // 0xEE
    QueryEncode, // 0xEF
    QueryEncode, // 0xF0
    QueryEncode, // 0xF1
    QueryEncode, // 0xF2
    QueryEncode, // 0xF3
    QueryEncode, // 0xF4
    QueryEncode, // 0xF5
    QueryEncode, // 0xF6
    QueryEncode, // 0xF7
    QueryEncode, // 0xF8
    QueryEncode, // 0xF9
    QueryEncode, // 0xFA
    QueryEncode, // 0xFB
    QueryEncode, // 0xFC
    QueryEncode, // 0xFD
    QueryEncode, // 0xFE
    QueryEncode, // 0xFF
};

bool isForbiddenHostCodePoint(char16_t character)
{
    return character <= 0x7F && characterClassTable[character] & ForbiddenHost;
}

template<typename CharacterType> ALWAYS_INLINE static bool isC0Control(CharacterType character) { return character <= 0x1F; }
template<typename CharacterType> ALWAYS_INLINE static bool isC0ControlOrSpace(CharacterType character) { return character <= 0x20; }
template<typename CharacterType> ALWAYS_INLINE static bool isTabOrNewline(CharacterType character) { return character <= 0xD && ((1u << character) & ((1u << '\t') | (1u << '\n') | (1u << '\r'))); }
template<typename CharacterType> ALWAYS_INLINE static bool isInC0ControlEncodeSet(CharacterType character) { return character > 0x7E || isC0Control(character); }
template<typename CharacterType> ALWAYS_INLINE static bool isInFragmentEncodeSet(CharacterType character) { return character > 0x7E || character == '`' || ((characterClassTable[character] & QueryEncode) && character != '#'); }
template<typename CharacterType> ALWAYS_INLINE static bool isInPathEncodeSet(CharacterType character) { return character > 0x7E || characterClassTable[character] & PathEncode; }
template<typename CharacterType> ALWAYS_INLINE static bool isInUserInfoEncodeSet(CharacterType character) { return character > 0x7E || characterClassTable[character] & UserInfoEncode; }
template<typename CharacterType> ALWAYS_INLINE static bool isPercentOrNonASCII(CharacterType character) { return !isASCII(character) || character == '%'; }
template<typename CharacterType> ALWAYS_INLINE static bool isSlashQuestionOrHash(CharacterType character) { return character <= '\\' && characterClassTable[character] & SlashQuestionOrHash; }
template<typename CharacterType> ALWAYS_INLINE static bool isValidSchemeCharacter(CharacterType character) { return character <= 'z' && characterClassTable[character] & ValidScheme; }
template<typename CharacterType> ALWAYS_INLINE static bool isSpecialCharacterForFragmentDirective(CharacterType character) { return !isASCII(character) || character == ',' || character == '-'; }

// Classes used by the run scanners below. A set bit means "the per-code-point
// state machine must look at this character"; a clear bit means the state would
// simply copy the character through unchanged.
enum ScanClass : uint16_t {
    SchemeContinue = 1 << 0, // [a-z0-9+-.]: a scheme character that needs no lowercasing.
    PathStop = 1 << 1, // Path state: needs encoding, or is / \ ? #, or non-ASCII.
    QueryStop = 1 << 2, // UTF8Query state: needs encoding (incl. ' for special schemes), or is #, or non-ASCII.
    FragmentStop = 1 << 3, // Fragment state: needs encoding, or non-ASCII.
    OpaquePathStop = 1 << 4, // OpaquePath state: C0, space, ? #, or non-ASCII. ('/' is handled in the scanner.)
    HostStop = 1 << 5, // Authority scan: / \ ? # @ end the scan.
    HostNotPlain = 1 << 6, // Authority scan: anything other than [a-z0-9-._~!$&'()*+,;=] etc; i.e. forbidden-domain, upper case, '%', '[', ':', non-ASCII.
    HostPercentOrNonASCII = 1 << 7,
    NonSpecialHostNotPlain = 1 << 8, // Authority scan of a non-special URL: forbidden host code point, ':', '%', C0 control or non-ASCII.
    HostNotDomainCharacter = 1 << 9, // HostNotPlain other than ASCII upper case: not even a domain character after lowercasing.
    IPv4NumberCharacter = 1 << 10, // Can appear in an IPv4 piece: hex digits and x/X.
};

static constexpr std::array<uint16_t, 256> scanClassTable = [] {
    std::array<uint16_t, 256> table { };
    for (unsigned c = 0; c < 256; ++c) {
        uint16_t bits = 0;
        if (isASCIILower(c) || isASCIIDigit(c) || c == '+' || c == '-' || c == '.')
            bits |= SchemeContinue;
        if (c > 0x7E || (characterClassTable[c] & PathEncode) || c == '/' || c == '\\' || c == '?' || c == '#')
            bits |= PathStop;
        if (c > 0x7E || (characterClassTable[c] & QueryEncode) || c == '\'' || c == '#')
            bits |= QueryStop;
        if (c > 0x7E || c == '`' || ((characterClassTable[c] & QueryEncode) && c != '#'))
            bits |= FragmentStop;
        if (c > 0x7E || c <= 0x20 || c == '?' || c == '#')
            bits |= OpaquePathStop;
        if (c == '/' || c == '\\' || c == '?' || c == '#' || c == '@')
            bits |= HostStop;
        if (c > 0x7E || c <= 0x20 || (characterClassTable[c] & ForbiddenDomain) || c == '[' || c == ']' || c == ':')
            bits |= HostNotPlain | HostNotDomainCharacter;
        if (isASCIIUpper(c))
            bits |= HostNotPlain;
        if (isASCIIHexDigit(c) || c == 'x' || c == 'X')
            bits |= IPv4NumberCharacter;
        if (c > 0x7F || c == '%')
            bits |= HostPercentOrNonASCII;
        if (c > 0x7E || c <= 0x20 || (characterClassTable[c] & ForbiddenHost) || c == '%')
            bits |= NonSpecialHostNotPlain;
        // The authority scanners accumulate the class of the stop character too, so keep it from looking like a host character class.
        if (bits & HostStop)
            bits &= ~(HostNotPlain | HostPercentOrNonASCII | NonSpecialHostNotPlain | HostNotDomainCharacter);
        table[c] = bits;
    }
    return table;
}();

template<typename CharacterType> ALWAYS_INLINE static bool charactersAreHTTP(const CharacterType* characters)
{
#if CPU(LITTLE_ENDIAN)
    if constexpr (sizeof(CharacterType) == 1)
        return unalignedLoad<uint32_t>(characters) == 0x70747468; // "http"
    else
        return unalignedLoad<uint64_t>(characters) == 0x0070007400740068;
#else
    return characters[0] == 'h' && characters[1] == 't' && characters[2] == 't' && characters[3] == 'p';
#endif
}

template<typename CharacterType> ALWAYS_INLINE static uint16_t scanClass(CharacterType character)
{
    if constexpr (sizeof(CharacterType) == 1)
        return scanClassTable[character];
    // Every non-Latin-1 character has the same classes as U+00FF.
    return character <= 0xFF ? scanClassTable[character] : scanClassTable[0xFF];
}
static_assert(scanClassTable[0xFF] == (PathStop | QueryStop | FragmentStop | OpaquePathStop | HostNotPlain | HostPercentOrNonASCII | NonSpecialHostNotPlain | HostNotDomainCharacter));

// Narrows 16 UTF-16 code units to bytes for classification. Code units that do not fit saturate to 0xFF (or to 0 on x86,
// where the pack is signed); both are stop characters for every scanner below, as is every character above 0x7E.
ALWAYS_INLINE static simde_uint8x16_t narrowForClassification(simde_uint16x8_t low, simde_uint16x8_t high)
{
#if CPU(X86_SSE2)
    return simde_uint8x16_from_m128i(_mm_packus_epi16(simde_uint16x8_to_m128i(low), simde_uint16x8_to_m128i(high)));
#else
    return simde_vcombine_u8(simde_vqmovn_u16(low), simde_vqmovn_u16(high));
#endif
}

// Finds the first character in [begin, end) whose scanClass() has any of the stopClass bits, 16 characters at a time.
// vectorStop computes, for 16 bytes, a mask of the lanes that are stop characters; it must treat 0, 0xFF and everything
// above 0x7E as stops.
template<typename CharacterType>
ALWAYS_INLINE static simde_uint8x16_t loadForClassification(const CharacterType* p)
{
    if constexpr (sizeof(CharacterType) == 1)
        return SIMD::load(std::bit_cast<const uint8_t*>(p));
    else
        return narrowForClassification(SIMD::load(std::bit_cast<const uint16_t*>(p)), SIMD::load(std::bit_cast<const uint16_t*>(p + 8)));
}

template<uint16_t stopClass, char additionalStopCharacter = 0, typename CharacterType, typename VectorStop>
ALWAYS_INLINE static const CharacterType* findStopCharacter(const CharacterType* begin, const CharacterType* end, const VectorStop& vectorStop)
{
    constexpr size_t stride = 16;
    auto loadBlock = [&](const CharacterType* p) ALWAYS_INLINE_LAMBDA {
        return loadForClassification(p);
    };
    size_t length = end - begin;
    auto* cursor = begin;
    if (length >= stride) {
        for (; cursor + stride <= end; cursor += stride) {
            if (auto index = SIMD::findFirstNonZeroIndex(vectorStop(loadBlock(cursor))))
                return cursor + *index;
        }
        if (cursor < end) {
            if (auto index = SIMD::findFirstNonZeroIndex(vectorStop(loadBlock(end - stride))))
                return end - stride + *index;
        }
        return end;
    }
    for (; cursor != end; ++cursor) {
        if ((scanClass(*cursor) & stopClass) || (additionalStopCharacter && *cursor == additionalStopCharacter))
            return cursor;
    }
    return end;
}

ALWAYS_INLINE static simde_uint8x16_t controlSpaceOrNonASCII(simde_uint8x16_t input)
{
    return SIMD::bitOr(SIMD::lessThan(input, SIMD::splat8(0x21)), SIMD::greaterThan(input, SIMD::splat8(0x7E)));
}

// These mirror the PathStop, QueryStop, FragmentStop and OpaquePathStop bits of scanClassTable. In a path, '/' only ends the
// run if it may start a dot segment (it is followed by '.' or '%') or is the last character examined; other slashes are part
// of the run and the last of them is reported.
template<typename CharacterType>
ALWAYS_INLINE static const CharacterType* findPathRunEnd(const CharacterType* begin, const CharacterType* end, const CharacterType*& lastSlash)
{
    lastSlash = nullptr;
    constexpr size_t stride = 16;
    size_t length = end - begin;
    if (length >= stride) {
        constexpr simde_uint8x16_t laneIndices { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
        auto step = [&](const CharacterType* block) ALWAYS_INLINE_LAMBDA -> const CharacterType* {
            auto input = loadForClassification(block);
            auto slashes = SIMD::equal<'/'>(input);
            auto dotOrPercent = SIMD::equal<'.', '%'>(input);
            auto nextIsDotOrPercent = simde_vextq_u8(dotOrPercent, SIMD::splat8(0xFF), 1);
            auto stops = SIMD::bitOr(controlSpaceOrNonASCII(input), SIMD::equal<'"', '#', '<', '>', '?', '\\', '^', '`', '{', '}'>(input), SIMD::bitAnd(slashes, nextIsDotOrPercent));
            if (auto index = SIMD::findFirstNonZeroIndex(stops)) {
                if (auto slash = SIMD::findLastNonZeroIndex(SIMD::bitAnd(slashes, SIMD::lessThan(laneIndices, SIMD::splat8(*index)))))
                    lastSlash = block + *slash;
                return block + *index;
            }
            if (auto slash = SIMD::findLastNonZeroIndex(slashes))
                lastSlash = block + *slash;
            return nullptr;
        };
        auto* cursor = begin;
        for (; cursor + stride <= end; cursor += stride) {
            if (auto* stop = step(cursor))
                return stop;
        }
        if (cursor < end) {
            if (auto* stop = step(end - stride))
                return stop;
        }
        return end;
    }
    for (auto* cursor = begin; cursor != end; ++cursor) {
        if (!(scanClass(*cursor) & PathStop)) [[likely]]
            continue;
        if (*cursor == '/' && end - cursor > 1 && cursor[1] != '.' && cursor[1] != '%') {
            lastSlash = cursor;
            continue;
        }
        return cursor;
    }
    return end;
}

template<typename CharacterType>
ALWAYS_INLINE static const CharacterType* findQueryStopCharacter(const CharacterType* begin, const CharacterType* end)
{
    return findStopCharacter<QueryStop>(begin, end, [](simde_uint8x16_t input) ALWAYS_INLINE_LAMBDA {
        return SIMD::bitOr(controlSpaceOrNonASCII(input), SIMD::equal<'"', '#', '\'', '<', '>'>(input));
    });
}

template<typename CharacterType>
ALWAYS_INLINE static const CharacterType* findFragmentStopCharacter(const CharacterType* begin, const CharacterType* end)
{
    return findStopCharacter<FragmentStop>(begin, end, [](simde_uint8x16_t input) ALWAYS_INLINE_LAMBDA {
        return SIMD::bitOr(controlSpaceOrNonASCII(input), SIMD::equal<'"', '<', '>', '`'>(input));
    });
}

template<typename CharacterType>
ALWAYS_INLINE static const CharacterType* findOpaquePathStopCharacterOrSlash(const CharacterType* begin, const CharacterType* end)
{
    return findStopCharacter<OpaquePathStop, '/'>(begin, end, [](simde_uint8x16_t input) ALWAYS_INLINE_LAMBDA {
        return SIMD::bitOr(controlSpaceOrNonASCII(input), SIMD::equal<'#', '?', '/'>(input));
    });
}

// Skips characters that are plain in a host for both special and non-special schemes (16 at a time), stopping at the
// first character that has HostNotPlain or HostStop, or at end.
template<typename CharacterType>
ALWAYS_INLINE static const CharacterType* findHostCharacterOfInterest(const CharacterType* begin, const CharacterType* end)
{
    return findStopCharacter<HostNotPlain | HostStop>(begin, end, [](simde_uint8x16_t input) ALWAYS_INLINE_LAMBDA {
        // C0 and space, DEL and non-ASCII, '@' through '^' (upper case and @ [ \\ ] ^), and # % / : < > ? |.
        return SIMD::bitOr(controlSpaceOrNonASCII(input),
            SIMD::lessThanOrEqual(SIMD::sub(input, SIMD::splat8('@')), SIMD::splat8('^' - '@')),
            SIMD::equal<'#', '%', '/', ':', '<', '>', '?', '|'>(input));
    });
}

// Finds the end of a special-scheme authority component (one of / \\ ? # @ or the end) starting from a character that
// already makes the host not plain, accumulating whether a '%' or non-ASCII character was seen on the way.
template<typename CharacterType>
ALWAYS_INLINE static const CharacterType* findEndOfSpecialAuthority(const CharacterType* p, const CharacterType* end, uint16_t& classes)
{
    while (true) {
        p = findStopCharacter<HostStop | HostPercentOrNonASCII>(p, end, [](simde_uint8x16_t input) ALWAYS_INLINE_LAMBDA {
            return SIMD::bitOr(SIMD::greaterThan(input, SIMD::splat8(0x7E)), SIMD::equal<'/', '?', '#', '\\', '@', '%', 0>(input));
        });
        if (p == end)
            return p;
        auto characterClass = scanClass(*p);
        classes |= characterClass;
        if (characterClass & HostStop)
            return p;
        ++p;
    }
}

template<typename CharacterType>
ALWAYS_INLINE bool URLParser::isForbiddenHostCodePoint(CharacterType character)
{
    ASSERT(!m_urlIsSpecial);
    return WTF::isForbiddenHostCodePoint(character);
}

template<typename CharacterType>
ALWAYS_INLINE bool URLParser::isForbiddenDomainCodePoint(CharacterType character)
{
    ASSERT(m_urlIsSpecial);
    return character <= 0x7F && characterClassTable[character] & ForbiddenDomain;
}

ALWAYS_INLINE static bool shouldPercentEncodeQueryByte(uint8_t byte, bool urlIsSpecial)
{
    if (characterClassTable[byte] & QueryEncode)
        return true;
    if (byte == '\'' && urlIsSpecial)
        return true;
    return false;
}

bool URLParser::isInUserInfoEncodeSet(char16_t c)
{
    return WTF::isInUserInfoEncodeSet(c);
}

bool URLParser::isSpecialCharacterForFragmentDirective(char16_t c)
{
    return WTF::isSpecialCharacterForFragmentDirective(c);
}

template<typename CharacterType, URLParser::ReportSyntaxViolation reportSyntaxViolation>
ALWAYS_INLINE void URLParser::advance(CodePointIterator<CharacterType>& iterator, const CodePointIterator<CharacterType>& iteratorForSyntaxViolationPosition)
{
    ++iterator;
    while (!iterator.atEnd() && isTabOrNewline(*iterator)) [[unlikely]] {
        if constexpr (reportSyntaxViolation == ReportSyntaxViolation::Yes)
            syntaxViolation(iteratorForSyntaxViolationPosition);
        ++iterator;
    }
}

template<typename CharacterType>
bool URLParser::takesTwoAdvancesUntilEnd(CodePointIterator<CharacterType> iterator)
{
    if (iterator.atEnd())
        return false;
    advance<CharacterType, ReportSyntaxViolation::No>(iterator);
    if (iterator.atEnd())
        return false;
    advance<CharacterType, ReportSyntaxViolation::No>(iterator);
    return iterator.atEnd();
}

template<typename CharacterType>
ALWAYS_INLINE bool URLParser::isWindowsDriveLetter(CodePointIterator<CharacterType> iterator)
{
    // https://url.spec.whatwg.org/#start-with-a-windows-drive-letter
    if (iterator.atEnd() || !isASCIIAlpha(*iterator))
        return false;
    advance<CharacterType, ReportSyntaxViolation::No>(iterator);
    if (iterator.atEnd())
        return false;
    if (*iterator != ':' && *iterator != '|')
        return false;
    advance<CharacterType, ReportSyntaxViolation::No>(iterator);
    return iterator.atEnd() || *iterator == '/' || *iterator == '\\' || *iterator == '?' || *iterator == '#';
}

ALWAYS_INLINE void URLParser::appendToASCIIBuffer(char32_t codePoint)
{
    ASSERT(isASCII(codePoint));
    if (m_didSeeSyntaxViolation) [[unlikely]]
        m_asciiBuffer.append(codePoint);
}

ALWAYS_INLINE void URLParser::appendToASCIIBuffer(std::span<const Latin1Character> characters)
{
    if (m_didSeeSyntaxViolation) [[unlikely]]
        m_asciiBuffer.append(characters);
}

ALWAYS_INLINE void URLParser::appendToASCIIBuffer(std::span<const char16_t> characters)
{
    if (!m_didSeeSyntaxViolation) [[likely]]
        return;
    size_t length = characters.size();
    auto* source = characters.data();
    if (length <= 4) {
        for (auto character : characters) {
            ASSERT(isASCII(character));
            m_asciiBuffer.append(static_cast<Latin1Character>(character));
        }
        return;
    }
    size_t oldSize = m_asciiBuffer.size();
    m_asciiBuffer.grow(oldSize + length);
    auto* destination = std::bit_cast<uint8_t*>(m_asciiBuffer.mutableSpan().data()) + oldSize;
    if (length < 16) {
        for (size_t i = 0; i < length; ++i) {
            ASSERT(isASCII(source[i]));
            destination[i] = source[i];
        }
        return;
    }
    size_t i = 0;
    for (; i + 16 <= length; i += 16)
        SIMD::store(narrowForClassification(SIMD::load(std::bit_cast<const uint16_t*>(source + i)), SIMD::load(std::bit_cast<const uint16_t*>(source + i + 8))), destination + i);
    if (i < length)
        SIMD::store(narrowForClassification(SIMD::load(std::bit_cast<const uint16_t*>(source + length - 16)), SIMD::load(std::bit_cast<const uint16_t*>(source + length - 8))), destination + length - 16);
}

template<typename CharacterType>
ALWAYS_INLINE void URLParser::appendToASCIIBufferLowercased(std::span<const CharacterType> characters)
{
    if (m_didSeeSyntaxViolation) [[unlikely]] {
        size_t oldSize = m_asciiBuffer.size();
        m_asciiBuffer.grow(oldSize + characters.size());
        auto destination = m_asciiBuffer.mutableSpan().subspan(oldSize);
        for (size_t i = 0; i < characters.size(); ++i) {
            ASSERT(isASCII(characters[i]));
            destination[i] = toASCIILower(characters[i]);
        }
    }
}

template<typename CharacterType>
void URLParser::appendWindowsDriveLetter(CodePointIterator<CharacterType>& iterator)
{
    auto lengthWithOnlyOneSlashInPath = m_url.m_hostEnd + m_url.m_portLength + 1;
    if (m_url.m_pathAfterLastSlash > lengthWithOnlyOneSlashInPath) {
        syntaxViolation(iterator);
        m_url.m_pathAfterLastSlash = lengthWithOnlyOneSlashInPath;
        m_asciiBuffer.resize(lengthWithOnlyOneSlashInPath);
    }
    ASSERT(isWindowsDriveLetter(iterator));
    appendToASCIIBuffer(*iterator);
    advance(iterator);
    ASSERT(!iterator.atEnd());
    ASSERT(*iterator == ':' || *iterator == '|');
    if (*iterator == '|')
        syntaxViolation(iterator);
    appendToASCIIBuffer(':');
    advance(iterator);
}

bool URLParser::copyBaseWindowsDriveLetter(const URL& base)
{
    if (base.protocolIsFile()) {
        RELEASE_ASSERT(base.m_hostEnd + base.m_portLength < base.m_string.length());
        if (base.m_string.is8Bit()) {
            auto characters = base.m_string.span8();
            CodePointIterator c { characters.subspan(base.m_hostEnd + base.m_portLength + 1) };
            if (isWindowsDriveLetter(c)) {
                appendWindowsDriveLetter(c);
                return true;
            }
        } else {
            auto characters = base.m_string.span16();
            CodePointIterator c { characters.subspan(base.m_hostEnd + base.m_portLength + 1) };
            if (isWindowsDriveLetter(c)) {
                appendWindowsDriveLetter(c);
                return true;
            }
        }
    }
    return false;
}

template<typename CharacterType>
bool URLParser::shouldCopyFileURL(CodePointIterator<CharacterType> iterator)
{
    if (!isWindowsDriveLetter(iterator))
        return true;
    if (iterator.atEnd())
        return false;
    advance(iterator);
    if (iterator.atEnd())
        return true;
    advance(iterator);
    if (iterator.atEnd())
        return true;
    return !isSlashQuestionOrHash(*iterator);
}

static void percentEncodeByte(uint8_t byte, Vector<Latin1Character>& buffer)
{
    buffer.append('%');
    buffer.append(upperNibbleToASCIIHexDigit(byte));
    buffer.append(lowerNibbleToASCIIHexDigit(byte));
}

ALWAYS_INLINE void URLParser::percentEncodeByte(uint8_t byte)
{
    ASSERT(m_didSeeSyntaxViolation);
    m_asciiBuffer.appendList<Latin1Character>({ '%', static_cast<Latin1Character>(upperNibbleToASCIIHexDigit(byte)), static_cast<Latin1Character>(lowerNibbleToASCIIHexDigit(byte)) });
}

static constexpr auto replacementCharacterUTF8PercentEncoded = "%EF%BF%BD"_s;

template<bool(*isInCodeSet)(char32_t), typename CharacterType>
ALWAYS_INLINE void URLParser::utf8PercentEncode(const CodePointIterator<CharacterType>& iterator)
{
    ASSERT(!iterator.atEnd());
    char32_t codePoint = *iterator;
    if (isASCII(codePoint)) [[likely]] {
        if (isInCodeSet(codePoint)) [[unlikely]] {
            syntaxViolation(iterator);
            percentEncodeByte(codePoint);
        } else
            appendToASCIIBuffer(codePoint);
        return;
    }
    ASSERT_WITH_MESSAGE(isInCodeSet(codePoint), "isInCodeSet should always return true for non-ASCII characters");
    syntaxViolation(iterator);

    std::array<uint8_t, U8_MAX_LENGTH> buffer;
    int32_t offset = 0;
    UBool isError = false;
    U8_APPEND(buffer, offset, U8_MAX_LENGTH, codePoint, isError);
    if (isError) {
        appendToASCIIBuffer(replacementCharacterUTF8PercentEncoded.span8());
        return;
    }
    for (int32_t i = 0; i < offset; ++i)
        percentEncodeByte(buffer[i]);
}

template<typename CharacterType>
ALWAYS_INLINE void URLParser::utf8QueryEncode(const CodePointIterator<CharacterType>& iterator)
{
    ASSERT(!iterator.atEnd());
    char32_t codePoint = *iterator;
    if (isASCII(codePoint)) [[likely]] {
        if (shouldPercentEncodeQueryByte(codePoint, m_urlIsSpecial)) [[unlikely]] {
            syntaxViolation(iterator);
            percentEncodeByte(codePoint);
        } else
            appendToASCIIBuffer(codePoint);
        return;
    }

    syntaxViolation(iterator);

    std::array<uint8_t, U8_MAX_LENGTH> buffer;
    int32_t offset = 0;
    UBool isError = false;
    U8_APPEND(buffer, offset, U8_MAX_LENGTH, codePoint, isError);
    if (isError) {
        appendToASCIIBuffer(replacementCharacterUTF8PercentEncoded.span8());
        return;
    }
    for (int32_t i = 0; i < offset; ++i) {
        auto byte = buffer[i];
        if (shouldPercentEncodeQueryByte(byte, m_urlIsSpecial))
            percentEncodeByte(byte);
        else
            appendToASCIIBuffer(byte);
    }
}

template<typename CharacterType>
void URLParser::encodeNonUTF8Query(const Vector<char16_t>& source, const URLTextEncoding& encoding, CodePointIterator<CharacterType> iterator)
{
    auto encoded = encoding.encodeForURLParsing(source.span());
    size_t length = encoded.size();
    
    if (!length == !iterator.atEnd()) {
        syntaxViolation(iterator);
        return;
    }
    
    size_t i = 0;
    for (; i < length; ++i) {
        ASSERT(!iterator.atEnd());
        uint8_t byte = encoded[i];
        if (byte != *iterator) [[unlikely]] {
            syntaxViolation(iterator);
            break;
        }
        if (shouldPercentEncodeQueryByte(byte, m_urlIsSpecial)) [[unlikely]] {
            syntaxViolation(iterator);
            break;
        }
        appendToASCIIBuffer(byte);
        ++iterator;
    }
    while (!iterator.atEnd() && isTabOrNewline(*iterator))
        ++iterator;
    ASSERT((i == length) == iterator.atEnd());
    for (; i < length; ++i) {
        ASSERT(m_didSeeSyntaxViolation);
        uint8_t byte = encoded[i];
        if (shouldPercentEncodeQueryByte(byte, m_urlIsSpecial))
            percentEncodeByte(byte);
        else
            appendToASCIIBuffer(byte);
    }
}

std::optional<uint16_t> URLParser::defaultPortForProtocol(StringView scheme)
{
    static constexpr uint16_t ftpPort = 21;
    static constexpr uint16_t httpPort = 80;
    static constexpr uint16_t httpsPort = 443;
    static constexpr uint16_t wsPort = 80;
    static constexpr uint16_t wssPort = 443;
    
    auto length = scheme.length();
    if (!length)
        return std::nullopt;
    switch (scheme[0]) {
    case 'w':
        switch (length) {
        case 2:
            if (scheme[1] == 's')
                return wsPort;
            return std::nullopt;
        case 3:
            if (scheme[1] == 's'
                && scheme[2] == 's')
                return wssPort;
            return std::nullopt;
        default:
            return std::nullopt;
        }
    case 'h':
        switch (length) {
        case 4:
            if (scheme[1] == 't'
                && scheme[2] == 't'
                && scheme[3] == 'p')
                return httpPort;
            return std::nullopt;
        case 5:
            if (scheme[1] == 't'
                && scheme[2] == 't'
                && scheme[3] == 'p'
                && scheme[4] == 's')
                return httpsPort;
            return std::nullopt;
        default:
            return std::nullopt;
        }
    case 'f':
        if (length == 3
            && scheme[1] == 't'
            && scheme[2] == 'p')
            return ftpPort;
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

enum class Scheme {
    WS,
    WSS,
    File,
    FTP,
    HTTP,
    HTTPS,
    NonSpecial
};

template<typename CharactersType>
ALWAYS_INLINE static Scheme schemeType(const CharactersType& scheme, size_t length)
{
    if (!length)
        return Scheme::NonSpecial;
    switch (scheme[0]) {
    case 'f':
        switch (length) {
        case 3:
            if (scheme[1] == 't'
                && scheme[2] == 'p')
                return Scheme::FTP;
            return Scheme::NonSpecial;
        case 4:
            if (scheme[1] == 'i'
                && scheme[2] == 'l'
                && scheme[3] == 'e')
                return Scheme::File;
            return Scheme::NonSpecial;
        default:
            return Scheme::NonSpecial;
        }
    case 'h':
        switch (length) {
        case 4:
            if (scheme[1] == 't'
                && scheme[2] == 't'
                && scheme[3] == 'p')
                return Scheme::HTTP;
            return Scheme::NonSpecial;
        case 5:
            if (scheme[1] == 't'
                && scheme[2] == 't'
                && scheme[3] == 'p'
                && scheme[4] == 's')
                return Scheme::HTTPS;
            return Scheme::NonSpecial;
        default:
            return Scheme::NonSpecial;
        }
    case 'w':
        switch (length) {
        case 2:
            if (scheme[1] == 's')
                return Scheme::WS;
            return Scheme::NonSpecial;
        case 3:
            if (scheme[1] == 's'
                && scheme[2] == 's')
                return Scheme::WSS;
            return Scheme::NonSpecial;
        default:
            return Scheme::NonSpecial;
        }
    default:
        return Scheme::NonSpecial;
    }
}

ALWAYS_INLINE static Scheme scheme(StringView scheme)
{
    if (scheme.is8Bit())
        return schemeType(scheme.span8(), scheme.length());
    return schemeType(scheme.span16(), scheme.length());
}

std::optional<String> URLParser::maybeCanonicalizeScheme(StringView scheme)
{
    if (scheme.isEmpty())
        return std::nullopt;

    size_t i = 0;
    while (i < scheme.length() && isTabOrNewline(scheme[i]))
        ++i;

    if (i >= scheme.length() || !isASCIIAlpha(scheme[i++]))
        return std::nullopt;

    for (; i < scheme.length(); ++i) {
        if (isASCIIAlphanumeric(scheme[i]) || scheme[i] == '+' || scheme[i] == '-' || scheme[i] == '.' || isTabOrNewline(scheme[i]))
            continue;
        return std::nullopt;
    }

    return scheme.convertToASCIILowercase().removeCharacters([](auto character) {
        return isTabOrNewline(character);
    });
}

bool URLParser::isSpecialScheme(StringView schemeArg)
{
    return scheme(schemeArg) != Scheme::NonSpecial;
}

enum class URLParser::URLPart {
    SchemeEnd,
    UserStart,
    UserEnd,
    PasswordEnd,
    HostEnd,
    PortEnd,
    PathAfterLastSlash,
    PathEnd,
    QueryEnd,
};

size_t URLParser::urlLengthUntilPart(const URL& url, URLPart part)
{
    switch (part) {
    case URLPart::QueryEnd:
        return url.m_queryEnd;
    case URLPart::PathEnd:
        return url.m_pathEnd;
    case URLPart::PathAfterLastSlash:
        return url.m_pathAfterLastSlash;
    case URLPart::PortEnd:
        return url.m_hostEnd + url.m_portLength;
    case URLPart::HostEnd:
        return url.m_hostEnd;
    case URLPart::PasswordEnd:
        return url.m_passwordEnd;
    case URLPart::UserEnd:
        return url.m_userEnd;
    case URLPart::UserStart:
        return url.m_userStart;
    case URLPart::SchemeEnd:
        return url.m_schemeEnd;
    }
    ASSERT_NOT_REACHED();
    return 0;
}

void URLParser::copyASCIIStringUntil(const String& string, size_t length)
{
    RELEASE_ASSERT(length <= string.length());
    if (string.isNull())
        return;
    ASSERT(m_asciiBuffer.isEmpty());
    if (string.is8Bit())
        appendToASCIIBuffer(string.span8().first(length));
    else {
        auto characters = string.span16().first(length);
        ASSERT_WITH_SECURITY_IMPLICATION(charactersAreAllASCII(characters));
        appendToASCIIBuffer(characters);
    }
}

template<typename CharacterType>
void URLParser::copyURLPartsUntil(const URL& base, URLPart part, const CodePointIterator<CharacterType>& iterator, const URLTextEncoding*& nonUTF8QueryEncoding)
{
    syntaxViolation(iterator);

    m_asciiBuffer.clear();
    copyASCIIStringUntil(base.m_string, urlLengthUntilPart(base, part));
    switch (part) {
    case URLPart::QueryEnd:
        m_url.m_queryEnd = base.m_queryEnd;
        [[fallthrough]];
    case URLPart::PathEnd:
        m_url.m_pathEnd = base.m_pathEnd;
        [[fallthrough]];
    case URLPart::PathAfterLastSlash:
        m_url.m_pathAfterLastSlash = base.m_pathAfterLastSlash;
        [[fallthrough]];
    case URLPart::PortEnd:
        m_url.m_portLength = base.m_portLength;
        [[fallthrough]];
    case URLPart::HostEnd:
        m_url.m_hostEnd = base.m_hostEnd;
        [[fallthrough]];
    case URLPart::PasswordEnd:
        m_url.m_passwordEnd = base.m_passwordEnd;
        [[fallthrough]];
    case URLPart::UserEnd:
        m_url.m_userEnd = base.m_userEnd;
        [[fallthrough]];
    case URLPart::UserStart:
        m_url.m_userStart = base.m_userStart;
        [[fallthrough]];
    case URLPart::SchemeEnd:
        m_url.m_isValid = base.m_isValid;
        m_url.m_protocolIsInHTTPFamily = base.m_protocolIsInHTTPFamily;
        m_url.m_schemeEnd = base.m_schemeEnd;
    }

    switch (scheme(m_asciiBuffer.subspan(0, m_url.m_schemeEnd))) {
    case Scheme::WS:
    case Scheme::WSS:
        nonUTF8QueryEncoding = nullptr;
        m_urlIsSpecial = true;
        return;
    case Scheme::File:
        m_urlIsFile = true;
        [[fallthrough]];
    case Scheme::FTP:
    case Scheme::HTTP:
    case Scheme::HTTPS:
        m_urlIsSpecial = true;
        return;
    case Scheme::NonSpecial:
        m_urlIsSpecial = false;
        nonUTF8QueryEncoding = nullptr;
        auto pathStart = m_url.m_hostEnd + m_url.m_portLength;
        if (pathStart + 2 < m_asciiBuffer.size()
            && m_asciiBuffer[pathStart] == '/'
            && m_asciiBuffer[pathStart + 1] == '.'
            && m_asciiBuffer[pathStart + 2] == '/') {
            m_asciiBuffer.removeAt(pathStart + 1, 2);
            m_url.m_pathAfterLastSlash = std::max(2u, m_url.m_pathAfterLastSlash) - 2;
            m_url.m_pathEnd = std::max(2u, m_url.m_pathEnd) - 2;
            m_url.m_queryEnd = std::max(2u, m_url.m_queryEnd) - 2;
        }
        return;
    }
    ASSERT_NOT_REACHED();
}

constexpr std::array<uint8_t, 2> dotASCIICode { '2', 'e' };

template<typename CharacterType>
ALWAYS_INLINE bool URLParser::isSingleDotPathSegment(CodePointIterator<CharacterType> c)
{
    if (c.atEnd())
        return false;
    if (*c == '.') {
        advance<CharacterType, ReportSyntaxViolation::No>(c);
        return c.atEnd() || isSlashQuestionOrHash(*c);
    }
    if (*c != '%')
        return false;
    advance<CharacterType, ReportSyntaxViolation::No>(c);
    if (c.atEnd() || *c != dotASCIICode[0])
        return false;
    advance<CharacterType, ReportSyntaxViolation::No>(c);
    if (c.atEnd())
        return false;
    if (isASCIIAlphaCaselessEqual(*c, dotASCIICode[1])) {
        advance<CharacterType, ReportSyntaxViolation::No>(c);
        return c.atEnd() || isSlashQuestionOrHash(*c);
    }
    return false;
}

template<typename CharacterType>
ALWAYS_INLINE bool URLParser::isDoubleDotPathSegment(CodePointIterator<CharacterType> c)
{
    if (c.atEnd())
        return false;
    if (*c == '.') {
        advance<CharacterType, ReportSyntaxViolation::No>(c);
        return isSingleDotPathSegment(c);
    }
    if (*c != '%')
        return false;
    advance<CharacterType, ReportSyntaxViolation::No>(c);
    if (c.atEnd() || *c != dotASCIICode[0])
        return false;
    advance<CharacterType, ReportSyntaxViolation::No>(c);
    if (c.atEnd())
        return false;
    if (isASCIIAlphaCaselessEqual(*c, dotASCIICode[1])) {
        advance<CharacterType, ReportSyntaxViolation::No>(c);
        return isSingleDotPathSegment(c);
    }
    return false;
}

template<typename CharacterType>
void URLParser::consumeSingleDotPathSegment(CodePointIterator<CharacterType>& c)
{
    ASSERT(isSingleDotPathSegment(c));
    if (*c == '.') {
        advance(c);
        if (!c.atEnd()) {
            if (*c == '/' || *c == '\\')
                advance(c);
            else
                ASSERT(*c == '?' || *c == '#');
        }
    } else {
        ASSERT(*c == '%');
        advance(c);
        ASSERT(*c == dotASCIICode[0]);
        advance(c);
        ASSERT(isASCIIAlphaCaselessEqual(*c, dotASCIICode[1]));
        advance(c);
        if (!c.atEnd()) {
            if (*c == '/' || *c == '\\')
                advance(c);
            else
                ASSERT(*c == '?' || *c == '#');
        }
    }
}

template<typename CharacterType>
void URLParser::consumeDoubleDotPathSegment(CodePointIterator<CharacterType>& c)
{
    ASSERT(isDoubleDotPathSegment(c));
    if (*c == '.')
        advance(c);
    else {
        ASSERT(*c == '%');
        advance(c);
        ASSERT(*c == dotASCIICode[0]);
        advance(c);
        ASSERT(isASCIIAlphaCaselessEqual(*c, dotASCIICode[1]));
        advance(c);
    }
    consumeSingleDotPathSegment(c);
}

bool URLParser::shouldPopPath(unsigned newPathAfterLastSlash)
{
    ASSERT(m_didSeeSyntaxViolation);
    if (!m_urlIsFile)
        return true;

    ASSERT(m_url.m_pathAfterLastSlash <= m_asciiBuffer.size());
    CodePointIterator<Latin1Character> componentToPop(m_asciiBuffer.subspan(newPathAfterLastSlash, m_url.m_pathAfterLastSlash - newPathAfterLastSlash));
    if (newPathAfterLastSlash == m_url.m_hostEnd + m_url.m_portLength + 1 && isWindowsDriveLetter(componentToPop))
        return false;
    return true;
}

void URLParser::popPath()
{
    ASSERT(m_didSeeSyntaxViolation);
    if (m_url.m_pathAfterLastSlash > m_url.m_hostEnd + m_url.m_portLength + 1) {
        auto newPathAfterLastSlash = m_url.m_pathAfterLastSlash - 1;
        if (m_asciiBuffer[newPathAfterLastSlash] == '/')
            newPathAfterLastSlash--;
        while (newPathAfterLastSlash > m_url.m_hostEnd + m_url.m_portLength && m_asciiBuffer[newPathAfterLastSlash] != '/')
            newPathAfterLastSlash--;
        newPathAfterLastSlash++;
        if (shouldPopPath(newPathAfterLastSlash))
            m_url.m_pathAfterLastSlash = newPathAfterLastSlash;
    }
    m_asciiBuffer.resize(m_url.m_pathAfterLastSlash);
}

template<typename CharacterType>
ALWAYS_INLINE void URLParser::syntaxViolation(const CodePointIterator<CharacterType>& iterator)
{
    if (!m_didSeeSyntaxViolation) [[unlikely]]
        beginSyntaxViolation(iterator);
}

template<typename CharacterType>
NEVER_INLINE void URLParser::beginSyntaxViolation(const CodePointIterator<CharacterType>& iterator)
{
    ASSERT(!m_didSeeSyntaxViolation);
    m_didSeeSyntaxViolation = true;
    
    ASSERT(m_asciiBuffer.isEmpty());
    size_t codeUnitsToCopy = iterator.codeUnitsSince(reinterpret_cast<const CharacterType*>(m_inputBegin));
    RELEASE_ASSERT(codeUnitsToCopy <= m_inputString.length());
    // Most syntax violations change the length by a few characters at most.
    m_asciiBuffer.reserveCapacity(m_inputString.length() + 8);
    if (m_inputString.is8Bit())
        m_asciiBuffer.append(m_inputString.span8().first(codeUnitsToCopy));
    else
        m_asciiBuffer.append(m_inputString.span16().first(codeUnitsToCopy));
}

void URLParser::failure()
{
    m_url.invalidate();
    m_url.m_string = releaseInputString();
}

template<typename CharacterType>
bool URLParser::checkLocalhostCodePoint(CodePointIterator<CharacterType>& iterator, char32_t codePoint)
{
    if (iterator.atEnd() || toASCIILower(*iterator) != codePoint)
        return false;
    advance<CharacterType, ReportSyntaxViolation::No>(iterator);
    return true;
}

template<typename CharacterType>
bool URLParser::isAtLocalhost(CodePointIterator<CharacterType> iterator)
{
    if (!checkLocalhostCodePoint(iterator, 'l'))
        return false;
    if (!checkLocalhostCodePoint(iterator, 'o'))
        return false;
    if (!checkLocalhostCodePoint(iterator, 'c'))
        return false;
    if (!checkLocalhostCodePoint(iterator, 'a'))
        return false;
    if (!checkLocalhostCodePoint(iterator, 'l'))
        return false;
    if (!checkLocalhostCodePoint(iterator, 'h'))
        return false;
    if (!checkLocalhostCodePoint(iterator, 'o'))
        return false;
    if (!checkLocalhostCodePoint(iterator, 's'))
        return false;
    if (!checkLocalhostCodePoint(iterator, 't'))
        return false;
    return iterator.atEnd();
}

bool URLParser::isLocalhost(StringView view)
{
    if (view.is8Bit())
        return isAtLocalhost<Latin1Character>(view.span8());
    return isAtLocalhost<char16_t>(view.span16());
}

ALWAYS_INLINE StringView URLParser::parsedDataView(size_t start, size_t length) LIFETIME_BOUND
{
    if (m_didSeeSyntaxViolation) [[unlikely]] {
        ASSERT(start + length <= m_asciiBuffer.size());
        return m_asciiBuffer.subspan(start, length);
    }
    ASSERT(start + length <= m_inputString.length());
    return StringView(m_inputString).substring(start, length);
}

ALWAYS_INLINE char16_t URLParser::parsedDataView(size_t position)
{
    if (m_didSeeSyntaxViolation) [[unlikely]]
        return m_asciiBuffer[position];
    return m_inputString[position];
}

template<typename CharacterType>
ALWAYS_INLINE size_t URLParser::currentPosition(const CodePointIterator<CharacterType>& iterator)
{
    if (m_didSeeSyntaxViolation) [[unlikely]]
        return m_asciiBuffer.size();

    return iterator.codeUnitsSince(reinterpret_cast<const CharacterType*>(m_inputBegin));
}

template<typename CharacterType>
ALWAYS_INLINE size_t URLParser::currentPosition(const CharacterType* position)
{
    if (m_didSeeSyntaxViolation) [[unlikely]]
        return m_asciiBuffer.size();
    return position - reinterpret_cast<const CharacterType*>(m_inputBegin);
}

URLParser::URLParser(URL& result, String&& input, const URL& base, const URLTextEncoding* nonUTF8QueryEncoding)
    : m_url(result)
    , m_ownedInputString(WTF::move(input))
    , m_inputString(m_ownedInputString)
{
    parse(base, nonUTF8QueryEncoding);
}

URLParser::URLParser(URL& result, const String& input, const URL& base, const URLTextEncoding* nonUTF8QueryEncoding)
    : m_url(result)
    , m_inputString(input)
{
    parse(base, nonUTF8QueryEncoding);
}

String URLParser::releaseInputString()
{
    if (&m_inputString == &m_ownedInputString)
        return WTF::move(m_ownedInputString);
    return m_inputString;
}

void URLParser::parse(const URL& base, const URLTextEncoding* nonUTF8QueryEncoding)
{
    ASSERT(!m_url.isValid());
    ASSERT(m_url.m_string.isNull());
    if (m_inputString.isNull()) {
        if (base.isValid() && !base.m_hasOpaquePath) {
            m_url = base;
            m_url.removeFragmentIdentifier();
        }
        return;
    }

#if ASSERT_ENABLED
    String inputString = m_inputString;
#endif

    if (m_inputString.is8Bit()) {
        auto characters = m_inputString.span8();
        m_inputBegin = characters.data();
        parse(characters, base, nonUTF8QueryEncoding);
    } else {
        auto characters = m_inputString.span16();
        m_inputBegin = characters.data();
        parse(characters, base, nonUTF8QueryEncoding);
    }

    ASSERT(!m_url.m_isValid
        || m_didSeeSyntaxViolation == (m_url.string() != inputString)
        || (inputString.containsOnly<isC0ControlOrSpace>() && m_url.m_string == base.m_string.left(base.m_queryEnd))
        || (base.isValid() && base.protocolIsFile()));
    ASSERT(internalValuesConsistent(m_url));
#if ASSERT_ENABLED
    if (!m_didSeeSyntaxViolation) {
        // Force a syntax violation at the beginning to make sure we get the same result.
        URL parsed;
        URLParser parser(parsed, makeString(' ', inputString), base, nonUTF8QueryEncoding);
        if (parsed.isValid())
            ASSERT(allValuesEqual(parsed, m_url));
    }
#endif // ASSERT_ENABLED

    if (needsNonSpecialDotSlash()) [[unlikely]]
        addNonSpecialDotSlash();
}

template<typename CharacterType>
void URLParser::parse(std::span<const CharacterType> input, const URL& base, const URLTextEncoding* nonUTF8QueryEncoding)
{
    URL_PARSER_LOG("Parsing URL <%s> base <%s>", String(input).utf8().data(), base.string().utf8().data());
    ASSERT(!m_url.isValid() && m_url.m_string.isNull());
    ASSERT(m_asciiBuffer.isEmpty());

    Vector<char16_t> queryBuffer;

    auto endIndex = input.size();
    if (nonUTF8QueryEncoding == URLTextEncodingSentinelAllowingC0AtEnd) [[unlikely]]
        nonUTF8QueryEncoding = nullptr;
    else {
        while (endIndex && isC0ControlOrSpace(input[endIndex - 1])) [[unlikely]] {
            syntaxViolation<CharacterType>(input);
            endIndex--;
        }
    }
    CodePointIterator<CharacterType> c(input.first(endIndex));
    CodePointIterator<CharacterType> authorityOrHostBegin;
    CodePointIterator<CharacterType> queryBegin;
    while (!c.atEnd() && isC0ControlOrSpace(*c)) [[unlikely]] {
        syntaxViolation(c);
        ++c;
    }
    const CharacterType* beginAfterControlAndSpace = input.data() + c.codeUnitsSince(input.data());

    const CharacterType* const inputBegin = input.data();
    const CharacterType* const inputEnd = inputBegin + endIndex;
    auto positionOf = [&](const CodePointIterator<CharacterType>& iterator) ALWAYS_INLINE_LAMBDA {
        return inputBegin + iterator.codeUnitsSince(inputBegin);
    };
    auto iteratorAt = [&](const CharacterType* position) ALWAYS_INLINE_LAMBDA {
        return CodePointIterator<CharacterType>(std::span<const CharacterType>(position, inputEnd));
    };
    // https://url.spec.whatwg.org/#ends-in-a-number-checker can only be true when this is:
    // the last label is non-empty, starts with an ASCII digit and consists of hex digits and x/X.
    auto lastLabelMayBeANumber = [](const CharacterType* start, const CharacterType* end) ALWAYS_INLINE_LAMBDA {
        if (end == start)
            return false;
        auto last = end[-1];
        if (!(scanClass(last) & IPv4NumberCharacter) && last != '.') [[likely]]
            return false;
        if (last == '.')
            --end;
        auto* label = end;
        while (label != start && (scanClass(label[-1]) & IPv4NumberCharacter))
            --label;
        if (label != start && label[-1] != '.')
            return false;
        return label != end && isASCIIDigit(*label);
    };

    enum class State : uint8_t {
        SchemeStart,
        Scheme,
        NoScheme,
        SpecialRelativeOrAuthority,
        PathOrAuthority,
        Relative,
        RelativeSlash,
        SpecialAuthoritySlashes,
        SpecialAuthorityIgnoreSlashes,
        AuthorityOrHost,
        Host,
        File,
        FileSlash,
        FileHost,
        FilePathStart,
        PathStart,
        Path,
        OpaquePath,
        UTF8Query,
        NonUTF8Query,
        Fragment,
    };

#define LOG_STATE(x) URL_PARSER_LOG("State %s, code point %c, parsed data <%s> size %zu", x, *c, parsedDataView(0, currentPosition(c)).utf8().data(), currentPosition(c))
#define LOG_FINAL_STATE(x) URL_PARSER_LOG("Final State: %s", x)

    State state = State::SchemeStart;

    // Straight-line pass over the common shape "special-scheme://host[/path][?query][#fragment]" for input that is
    // already canonical. It stops at the first code unit that needs anything more than copying, leaving state, c and
    // m_url exactly as the state machine below has them at that point, and the state machine takes over from there.
    do {
        if (m_didSeeSyntaxViolation || c.atEnd())
            break;
        auto* p = positionOf(c);
        size_t schemeEnd;
        Scheme urlSchemeType;
        if (inputEnd - p >= 6 && charactersAreHTTP(p) && p[4 + (p[4] == 's')] == ':') [[likely]] {
            urlSchemeType = p[4] == 's' ? Scheme::HTTPS : Scheme::HTTP;
            schemeEnd = 4 + (p[4] == 's');
            p += schemeEnd;
        } else {
            if (!isASCIILower(*p))
                break;
            ++p;
            while (p != inputEnd && (scanClass(*p) & SchemeContinue))
                ++p;
            if (p == inputEnd || *p != ':')
                break;
            schemeEnd = p - inputBegin;
            urlSchemeType = schemeType(inputBegin, schemeEnd);
            if (urlSchemeType == Scheme::NonSpecial || urlSchemeType == Scheme::File || schemeEnd > URL::maxSchemeLength) {
                c = iteratorAt(p);
                state = State::Scheme;
                break;
            }
        }
        ASSERT(*p == ':' && urlSchemeType == schemeType(inputBegin, schemeEnd));
        m_url.m_schemeEnd = schemeEnd;
        m_url.m_protocolIsInHTTPFamily = urlSchemeType == Scheme::HTTP || urlSchemeType == Scheme::HTTPS;
        m_urlIsSpecial = true;
        if (urlSchemeType == Scheme::WS || urlSchemeType == Scheme::WSS)
            nonUTF8QueryEncoding = nullptr;
        if (inputEnd - p <= 3 || p[1] != '/' || p[2] != '/' || (scanClass(p[3]) & HostStop) || isTabOrNewline(p[3])) [[unlikely]] {
            if (base.isValid() && base.protocolIs(StringView(m_inputString).left(schemeEnd)))
                state = State::SpecialRelativeOrAuthority;
            else
                state = State::SpecialAuthoritySlashes;
            c = iteratorAt(p + 1);
            break;
        }
        p += 3;
        m_url.m_userStart = p - inputBegin;
        authorityOrHostBegin = iteratorAt(p);

        auto* hostStart = p;
        uint16_t classes = 0;
        p = findHostCharacterOfInterest(p, inputEnd);
        if (p != inputEnd) {
            classes = scanClass(*p);
            if (!(classes & HostStop))
                p = findEndOfSpecialAuthority(p + 1, inputEnd, classes);
        }
        if ((p != inputEnd && *p == '@') || (classes & HostNotPlain) || lastLabelMayBeANumber(hostStart, p)) [[unlikely]] {
            if (classes & HostPercentOrNonASCII)
                m_hostHasPercentOrNonASCII = true;
            state = State::AuthorityOrHost;
            c = iteratorAt(p);
            break;
        }
        ASSERT(p != hostStart);
        m_url.m_userEnd = m_url.m_userStart;
        m_url.m_passwordEnd = m_url.m_userStart;
        m_url.m_hostEnd = p - inputBegin;
        m_url.m_portLength = 0;
        state = State::Path;
        if (p == inputEnd) {
            // "scheme://host" -> "scheme://host/". Common enough to construct the result directly rather than through m_asciiBuffer.
            m_didSeeSyntaxViolation = true;
            size_t length = p - inputBegin;
            std::span<Latin1Character> buffer;
            m_url.m_string = StringImpl::createUninitialized(length + 1, buffer);
            StringImpl::copyCharacters(buffer, std::span(inputBegin, p));
            buffer[length] = '/';
            m_url.m_pathAfterLastSlash = length + 1;
            m_url.m_pathEnd = length + 1;
            m_url.m_queryEnd = length + 1;
            m_url.m_isValid = true;
            return;
        }
        if (*p != '/') [[unlikely]] {
            c = iteratorAt(p);
            if (*p != '\\') {
                syntaxViolation(c);
                appendToASCIIBuffer('/');
                m_url.m_pathAfterLastSlash = currentPosition(c);
            }
            break;
        }
        ++p;
        m_url.m_pathAfterLastSlash = p - inputBegin;

        while (p != inputEnd) {
            if ((*p == '.' || *p == '%') && (isSingleDotPathSegment(iteratorAt(p)) || isDoubleDotPathSegment(iteratorAt(p)))) [[unlikely]]
                break;
            const CharacterType* lastSlash;
            p = findPathRunEnd(p, inputEnd, lastSlash);
            if (lastSlash)
                m_url.m_pathAfterLastSlash = lastSlash + 1 - inputBegin;
            if (p == inputEnd || *p != '/')
                break;
            ++p;
            m_url.m_pathAfterLastSlash = p - inputBegin;
        }
        c = iteratorAt(p);
        if (p == inputEnd)
            break;
        if (*p == '?') {
            m_url.m_pathEnd = p - inputBegin;
            ++p;
            c = iteratorAt(p);
            if (nonUTF8QueryEncoding) [[unlikely]] {
                queryBegin = c;
                state = State::NonUTF8Query;
                break;
            }
            state = State::UTF8Query;
            p = findQueryStopCharacter(p, inputEnd);
            c = iteratorAt(p);
            if (p == inputEnd || *p != '#')
                break;
            m_url.m_queryEnd = p - inputBegin;
        } else if (*p == '#') {
            m_url.m_pathEnd = p - inputBegin;
            m_url.m_queryEnd = m_url.m_pathEnd;
        } else
            break;
        ASSERT(*p == '#');
        state = State::Fragment;
        p = findFragmentStopCharacter(p, inputEnd);
        c = iteratorAt(p);
    } while (false);

    while (!c.atEnd()) {
        if (isTabOrNewline(*c)) [[unlikely]] {
            syntaxViolation(c);
            ++c;
            continue;
        }

        switch (state) {
        case State::SchemeStart: {
            LOG_STATE("SchemeStart");
            bool reachedSchemeEnd = false;
            if (isASCIIAlpha(*c)) [[likely]] {
                auto* start = positionOf(c);
                auto* p = start;
                const CharacterType* firstUppercase = nullptr;
                for (; p != inputEnd; ++p) {
                    if (scanClass(*p) & SchemeContinue) [[likely]]
                        continue;
                    if (!isASCIIUpper(*p))
                        break;
                    if (!firstUppercase)
                        firstUppercase = p;
                }
                if (p != inputEnd && *p == ':') [[likely]] {
                    if (!firstUppercase) [[likely]]
                        appendToASCIIBuffer(std::span(start, p));
                    else {
                        appendToASCIIBuffer(std::span(start, firstUppercase));
                        syntaxViolation(iteratorAt(firstUppercase));
                        appendToASCIIBufferLowercased(std::span(firstUppercase, p));
                    }
                    c = iteratorAt(p);
                    state = State::Scheme;
                    reachedSchemeEnd = true;
                }
            }
            if (!reachedSchemeEnd) {
                if (isASCIIAlpha(*c)) {
                    if (isASCIIUpper(*c)) [[unlikely]]
                        syntaxViolation(c);
                    appendToASCIIBuffer(toASCIILower(*c));
                    advance(c);
                    if (c.atEnd()) {
                        m_asciiBuffer.clear();
                        state = State::NoScheme;
                        c = iteratorAt(beginAfterControlAndSpace);
                        break;
                    }
                    state = State::Scheme;
                } else
                    state = State::NoScheme;
                break;
            }
            ASSERT(*c == ':');
            [[fallthrough]];
        }
        case State::Scheme:
            LOG_STATE("Scheme");
            if (isValidSchemeCharacter(*c)) {
                if (isASCIIUpper(*c)) [[unlikely]]
                    syntaxViolation(c);
                appendToASCIIBuffer(toASCIILower(*c));
            } else if (*c == ':') {
                unsigned schemeEnd = currentPosition(c);
                if (schemeEnd > URL::maxSchemeLength) {
                    failure();
                    return;
                }
                m_url.m_schemeEnd = schemeEnd;
                appendToASCIIBuffer(':');
                auto urlSchemeType = m_didSeeSyntaxViolation ? schemeType(m_asciiBuffer.span().data(), schemeEnd) : schemeType(inputBegin, schemeEnd);
                m_url.m_protocolIsInHTTPFamily = urlSchemeType == Scheme::HTTP || urlSchemeType == Scheme::HTTPS;
                switch (urlSchemeType) {
                case Scheme::File:
                    m_urlIsSpecial = true;
                    m_urlIsFile = true;
                    state = State::File;
                    ++c;
                    break;
                case Scheme::WS:
                case Scheme::WSS:
                    nonUTF8QueryEncoding = nullptr;
                    [[fallthrough]];
                case Scheme::HTTP:
                case Scheme::HTTPS:
                case Scheme::FTP: {
                    m_urlIsSpecial = true;
                    auto* p = positionOf(c);
                    if (inputEnd - p > 3 && p[1] == '/' && p[2] == '/' && !isTabOrNewline(p[3]) && p[3] != '/' && p[3] != '\\') [[likely]] {
                        // Equivalent to passing through SpecialAuthoritySlashes or SpecialRelativeOrAuthority, then SpecialAuthorityIgnoreSlashes.
                        appendToASCIIBuffer("//"_span8);
                        c = iteratorAt(p + 3);
                        m_url.m_userStart = currentPosition(c);
                        authorityOrHostBegin = c;
                        state = State::AuthorityOrHost;
                        break;
                    }
                    if (base.isValid() && base.protocolIs(parsedDataView(0, m_url.m_schemeEnd)))
                        state = State::SpecialRelativeOrAuthority;
                    else
                        state = State::SpecialAuthoritySlashes;
                    ++c;
                    break;
                }
                case Scheme::NonSpecial:
                    nonUTF8QueryEncoding = nullptr;
                    auto maybeSlash = c;
                    advance(maybeSlash);
                    if (!maybeSlash.atEnd() && *maybeSlash == '/') {
                        appendToASCIIBuffer('/');
                        c = maybeSlash;
                        state = State::PathOrAuthority;
                        ASSERT(*c == '/');
                        ++c;
                        m_url.m_userStart = currentPosition(c);
                    } else {
                        ++c;
                        m_url.m_userStart = currentPosition(c);
                        m_url.m_userEnd = m_url.m_userStart;
                        m_url.m_passwordEnd = m_url.m_userStart;
                        m_url.m_hostEnd = m_url.m_userStart;
                        m_url.m_portLength = 0;
                        m_url.m_pathAfterLastSlash = m_url.m_userStart;
                        m_url.m_hasOpaquePath = true;
                        state = State::OpaquePath;
                    }
                    break;
                }
                break;
            } else {
                m_asciiBuffer.clear();
                state = State::NoScheme;
                c = iteratorAt(beginAfterControlAndSpace);
                break;
            }
            advance(c);
            if (c.atEnd()) {
                m_asciiBuffer.clear();
                state = State::NoScheme;
                c = iteratorAt(beginAfterControlAndSpace);
            }
            break;
        case State::NoScheme:
            LOG_STATE("NoScheme");
            if (!base.isValid() || (base.m_hasOpaquePath && *c != '#')) {
                failure();
                return;
            }
            if (base.m_hasOpaquePath && *c == '#') {
                copyURLPartsUntil(base, URLPart::QueryEnd, c, nonUTF8QueryEncoding);
                state = State::Fragment;
                appendToASCIIBuffer('#');
                ++c;
                break;
            }
            if (!base.protocolIsFile()) {
                m_urlIsSpecial = isSpecialScheme(base.protocol());
                state = State::Relative;
                break;
            }
            state = State::File;
            break;
        case State::SpecialRelativeOrAuthority:
            LOG_STATE("SpecialRelativeOrAuthority");
            if (*c == '/') {
                appendToASCIIBuffer('/');
                advance(c);
                if (!c.atEnd() && *c == '/') {
                    appendToASCIIBuffer('/');
                    state = State::SpecialAuthorityIgnoreSlashes;
                    ++c;
                } else
                    state = State::RelativeSlash;
            } else
                state = State::Relative;
            break;
        case State::PathOrAuthority:
            LOG_STATE("PathOrAuthority");
            if (*c == '/') {
                appendToASCIIBuffer('/');
                state = State::AuthorityOrHost;
                advance(c);
                m_url.m_userStart = currentPosition(c);
                authorityOrHostBegin = c;
            } else {
                ASSERT(parsedDataView(currentPosition(c) - 1) == '/');
                m_url.m_userStart = currentPosition(c) - 1;
                m_url.m_userEnd = m_url.m_userStart;
                m_url.m_passwordEnd = m_url.m_userStart;
                m_url.m_hostEnd = m_url.m_userStart;
                m_url.m_portLength = 0;
                m_url.m_pathAfterLastSlash = m_url.m_userStart + 1;
                state = State::Path;
            }
            break;
        case State::Relative:
            LOG_STATE("Relative");
            switch (*c) {
            case '/':
                state = State::RelativeSlash;
                ++c;
                break;
            case '?':
                copyURLPartsUntil(base, URLPart::PathEnd, c, nonUTF8QueryEncoding);
                appendToASCIIBuffer('?');
                ++c;
                if (nonUTF8QueryEncoding) {
                    queryBegin = c;
                    state = State::NonUTF8Query;
                } else
                    state = State::UTF8Query;
                break;
            case '#':
                copyURLPartsUntil(base, URLPart::QueryEnd, c, nonUTF8QueryEncoding);
                appendToASCIIBuffer('#');
                state = State::Fragment;
                ++c;
                break;
            case '\\':
                if (m_urlIsSpecial) {
                    state = State::RelativeSlash;
                    ++c;
                    break;
                }
                [[fallthrough]];
            default:
                copyURLPartsUntil(base, URLPart::PathAfterLastSlash, c, nonUTF8QueryEncoding);
                if ((currentPosition(c) && parsedDataView(currentPosition(c) - 1) != '/')
                    || (base.host().isEmpty() && base.path().isEmpty())) {
                    appendToASCIIBuffer('/');
                    m_url.m_pathAfterLastSlash = currentPosition(c);
                }
                state = State::Path;
                break;
            }
            break;
        case State::RelativeSlash:
            LOG_STATE("RelativeSlash");
            if (*c == '/' || (*c == '\\' && m_urlIsSpecial)) {
                ++c;
                copyURLPartsUntil(base, URLPart::SchemeEnd, c, nonUTF8QueryEncoding);
                appendToASCIIBuffer("://"_span8);
                if (m_urlIsSpecial)
                    state = State::SpecialAuthorityIgnoreSlashes;
                else {
                    m_url.m_userStart = currentPosition(c);
                    state = State::AuthorityOrHost;
                    authorityOrHostBegin = c;
                }
            } else {
                copyURLPartsUntil(base, URLPart::PortEnd, c, nonUTF8QueryEncoding);
                appendToASCIIBuffer('/');
                m_url.m_pathAfterLastSlash = base.m_hostEnd + base.m_portLength + 1;
                state = State::Path;
            }
            break;
        case State::SpecialAuthoritySlashes:
            LOG_STATE("SpecialAuthoritySlashes");
            if (*c == '/' || *c == '\\') [[likely]] {
                if (*c == '\\') [[unlikely]]
                    syntaxViolation(c);
                appendToASCIIBuffer('/');
                advance(c);
                if (!c.atEnd() && (*c == '/' || *c == '\\')) [[likely]] {
                    if (*c == '\\') [[unlikely]]
                        syntaxViolation(c);
                    ++c;
                    appendToASCIIBuffer('/');
                } else {
                    syntaxViolation(c);
                    appendToASCIIBuffer('/');
                }
            } else {
                syntaxViolation(c);
                appendToASCIIBuffer("//"_span8);
            }
            state = State::SpecialAuthorityIgnoreSlashes;
            break;
        case State::SpecialAuthorityIgnoreSlashes:
            LOG_STATE("SpecialAuthorityIgnoreSlashes");
            if (*c == '/' || *c == '\\') {
                syntaxViolation(c);
                ++c;
            } else {
                m_url.m_userStart = currentPosition(c);
                state = State::AuthorityOrHost;
                authorityOrHostBegin = c;
            }
            break;
        case State::AuthorityOrHost:
            if (c == authorityOrHostBegin) [[likely]] {
                auto* start = positionOf(c);
                auto* p = findHostCharacterOfInterest(start, inputEnd);
                uint16_t classes = 0;
                if (m_urlIsSpecial) {
                    if (p != inputEnd) {
                        classes = scanClass(*p);
                        if (!(classes & HostStop))
                            p = findEndOfSpecialAuthority(p + 1, inputEnd, classes);
                    }
                } else {
                    for (; p != inputEnd; ++p) {
                        uint16_t characterClass = scanClass(*p);
                        classes |= characterClass;
                        if (!(characterClass & HostStop)) [[likely]]
                            continue;
                        if (*p == '\\') [[unlikely]] {
                            classes |= NonSpecialHostNotPlain;
                            continue;
                        }
                        break;
                    }
                }
                if (p != inputEnd && *p == '@') [[unlikely]] {
                    auto* lastAt = p;
                    for (auto* q = p + 1; q != inputEnd; ++q) {
                        if (!(scanClass(*q) & HostStop)) [[likely]]
                            continue;
                        if (*q == '@')
                            lastAt = q;
                        else if (*q != '\\' || m_urlIsSpecial)
                            break;
                    }
                    parseAuthority(CodePointIterator<CharacterType>(authorityOrHostBegin, iteratorAt(lastAt)));
                    c = iteratorAt(lastAt);
                    advance(c);
                    authorityOrHostBegin = c;
                    state = State::Host;
                    m_hostHasPercentOrNonASCII = false;
                    break;
                }
                bool hostIsPlain = p != start && (m_urlIsSpecial ? !(classes & HostNotPlain) && !lastLabelMayBeANumber(start, p) : !(classes & NonSpecialHostNotPlain));
                if (hostIsPlain) [[likely]] {
                    m_url.m_userEnd = currentPosition(authorityOrHostBegin);
                    m_url.m_passwordEnd = m_url.m_userEnd;
                    appendToASCIIBuffer(std::span(start, p));
                    m_url.m_hostEnd = currentPosition(p);
                    m_url.m_portLength = 0;
                    c = iteratorAt(p);
                    if (p == inputEnd || (*p != '/' && *p != '\\')) {
                        if (m_urlIsSpecial) {
                            syntaxViolation(c);
                            appendToASCIIBuffer('/');
                        }
                        m_url.m_pathAfterLastSlash = currentPosition(c);
                    }
                    state = State::Path;
                    break;
                }
                if (classes & HostPercentOrNonASCII)
                    m_hostHasPercentOrNonASCII = true;
                c = iteratorAt(p);
                if (c.atEnd())
                    break;
                // The terminator is handled below.
            }
            do {
                LOG_STATE("AuthorityOrHost");
                if (*c == '@') {
                    auto lastAt = c;
                    auto findLastAt = c;
                    while (!findLastAt.atEnd()) {
                        URL_PARSER_LOG("Finding last @: %c", *findLastAt);
                        if (*findLastAt == '@')
                            lastAt = findLastAt;
                        bool isSlash = *findLastAt == '/' || (m_urlIsSpecial && *findLastAt == '\\');
                        if (isSlash || *findLastAt == '?' || *findLastAt == '#')
                            break;
                        ++findLastAt;
                    }
                    parseAuthority(CodePointIterator<CharacterType>(authorityOrHostBegin, lastAt));
                    c = lastAt;
                    advance(c);
                    authorityOrHostBegin = c;
                    state = State::Host;
                    m_hostHasPercentOrNonASCII = false;
                    break;
                }
                bool isSlash = *c == '/' || (m_urlIsSpecial && *c == '\\');
                if (isSlash || *c == '?' || *c == '#') {
                    auto iterator = CodePointIterator<CharacterType>(authorityOrHostBegin, c);
                    if (iterator.atEnd()) {
                        if (m_urlIsSpecial)
                            return failure();
                        m_url.m_userEnd = currentPosition(c);
                        m_url.m_passwordEnd = m_url.m_userEnd;
                        m_url.m_hostEnd = m_url.m_userEnd;
                        m_url.m_portLength = 0;
                        m_url.m_pathAfterLastSlash = m_url.m_userEnd;
                    } else {
                        m_url.m_userEnd = currentPosition(authorityOrHostBegin);
                        m_url.m_passwordEnd = m_url.m_userEnd;
                        if (parseHostAndPort(iterator) == HostParsingResult::InvalidHost) {
                            failure();
                            return;
                        }
                        if (!isSlash) [[unlikely]] {
                            if (m_urlIsSpecial) {
                                syntaxViolation(c);
                                appendToASCIIBuffer('/');
                            }
                            m_url.m_pathAfterLastSlash = currentPosition(c);
                        }
                    }
                    state = State::Path;
                    break;
                }
                if (isPercentOrNonASCII(*c))
                    m_hostHasPercentOrNonASCII = true;
                ++c;
            } while (!c.atEnd());
            break;
        case State::Host:
            if (c == authorityOrHostBegin) [[likely]] {
                auto* start = positionOf(c);
                auto* p = findHostCharacterOfInterest(start, inputEnd);
                uint16_t classes = 0;
                while (p != inputEnd) {
                    uint16_t characterClass = scanClass(*p);
                    classes |= characterClass;
                    if (characterClass & HostStop) {
                        if (*p != '@' && (*p != '\\' || m_urlIsSpecial))
                            break;
                        classes |= HostNotPlain | NonSpecialHostNotPlain;
                    }
                    ++p;
                    if (m_urlIsSpecial)
                        p = findEndOfSpecialAuthority(p, inputEnd, classes);
                }
                bool hostIsPlain = p != start && (m_urlIsSpecial ? !(classes & HostNotPlain) && !lastLabelMayBeANumber(start, p) : !(classes & NonSpecialHostNotPlain));
                if (hostIsPlain) [[likely]] {
                    appendToASCIIBuffer(std::span(start, p));
                    m_url.m_hostEnd = currentPosition(p);
                    m_url.m_portLength = 0;
                    c = iteratorAt(p);
                    if (p == inputEnd) {
                        if (m_urlIsSpecial) {
                            syntaxViolation(c);
                            appendToASCIIBuffer('/');
                        }
                        m_url.m_pathAfterLastSlash = currentPosition(c);
                    } else if (*p == '?' || *p == '#') {
                        // FIXME: This inserts a '/' for non-special schemes too, unlike the AuthorityOrHost state; kept for now to preserve behavior.
                        syntaxViolation(c);
                        appendToASCIIBuffer('/');
                        m_url.m_pathAfterLastSlash = currentPosition(c);
                    }
                    state = State::Path;
                    break;
                }
                if (classes & HostPercentOrNonASCII)
                    m_hostHasPercentOrNonASCII = true;
                c = iteratorAt(p);
                if (c.atEnd())
                    break;
                // The terminator is handled below.
            }
            do {
                LOG_STATE("Host");
                bool isSlash = *c == '/' || (m_urlIsSpecial && *c == '\\');
                if (isSlash || *c == '?' || *c == '#') {
                    if (parseHostAndPort(CodePointIterator<CharacterType>(authorityOrHostBegin, c)) == HostParsingResult::InvalidHost) {
                        failure();
                        return;
                    }
                    if (*c == '?' || *c == '#') {
                        syntaxViolation(c);
                        appendToASCIIBuffer('/');
                        m_url.m_pathAfterLastSlash = currentPosition(c);
                    }
                    state = State::Path;
                    break;
                }
                if (isPercentOrNonASCII(*c))
                    m_hostHasPercentOrNonASCII = true;
                ++c;
            } while (!c.atEnd());
            break;
        case State::File:
            LOG_STATE("File");
            switch (*c) {
            case '\\':
                syntaxViolation(c);
                [[fallthrough]];
            case '/':
                appendToASCIIBuffer('/');
                state = State::FileSlash;
                ++c;
                break;
            case '?':
                syntaxViolation(c);
                if (base.isValid() && base.protocolIsFile()) {
                    copyURLPartsUntil(base, URLPart::PathEnd, c, nonUTF8QueryEncoding);
                    appendToASCIIBuffer('?');
                    ++c;
                } else {
                    appendToASCIIBuffer("///?"_span8);
                    ++c;
                    m_url.m_userStart = currentPosition(c) - 2;
                    m_url.m_userEnd = m_url.m_userStart;
                    m_url.m_passwordEnd = m_url.m_userStart;
                    m_url.m_hostEnd = m_url.m_userStart;
                    m_url.m_portLength = 0;
                    m_url.m_pathAfterLastSlash = m_url.m_userStart + 1;
                    m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
                }
                if (nonUTF8QueryEncoding) {
                    queryBegin = c;
                    state = State::NonUTF8Query;
                } else
                    state = State::UTF8Query;
                break;
            case '#':
                syntaxViolation(c);
                if (base.isValid() && base.protocolIsFile()) {
                    copyURLPartsUntil(base, URLPart::QueryEnd, c, nonUTF8QueryEncoding);
                    appendToASCIIBuffer('#');
                } else {
                    appendToASCIIBuffer("///#"_span8);
                    m_url.m_userStart = currentPosition(c) - 2;
                    m_url.m_userEnd = m_url.m_userStart;
                    m_url.m_passwordEnd = m_url.m_userStart;
                    m_url.m_hostEnd = m_url.m_userStart;
                    m_url.m_portLength = 0;
                    m_url.m_pathAfterLastSlash = m_url.m_userStart + 1;
                    m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
                    m_url.m_queryEnd = m_url.m_pathAfterLastSlash;
                }
                state = State::Fragment;
                ++c;
                break;
            default:
                syntaxViolation(c);
                if (base.isValid() && base.protocolIsFile() && shouldCopyFileURL(c))
                    copyURLPartsUntil(base, URLPart::PathAfterLastSlash, c, nonUTF8QueryEncoding);
                else {
                    bool copiedHost = false;
                    if (base.isValid() && base.protocolIsFile()) {
                        if (base.host().isEmpty()) {
                            copyURLPartsUntil(base, URLPart::SchemeEnd, c, nonUTF8QueryEncoding);
                            appendToASCIIBuffer(":///"_span8);
                        } else {
                            copyURLPartsUntil(base, URLPart::PortEnd, c, nonUTF8QueryEncoding);
                            appendToASCIIBuffer('/');
                            copiedHost = true;
                        }
                    } else
                        appendToASCIIBuffer("///"_span8);
                    if (!copiedHost) {
                        m_url.m_userStart = currentPosition(c) - 1;
                        m_url.m_userEnd = m_url.m_userStart;
                        m_url.m_passwordEnd = m_url.m_userStart;
                        m_url.m_hostEnd = m_url.m_userStart;
                        m_url.m_portLength = 0;
                    }
                    m_url.m_pathAfterLastSlash = m_url.m_hostEnd + 1;
                }
                if (isWindowsDriveLetter(c))
                    appendWindowsDriveLetter(c);
                state = State::Path;
                break;
            }
            break;
        case State::FileSlash:
            LOG_STATE("FileSlash");
            if (*c == '/' || *c == '\\') [[likely]] {
                if (*c == '\\') [[unlikely]]
                    syntaxViolation(c);
                if (base.isValid() && base.protocolIsFile()) {
                    copyURLPartsUntil(base, URLPart::SchemeEnd, c, nonUTF8QueryEncoding);
                    appendToASCIIBuffer(":/"_span8);
                }
                appendToASCIIBuffer('/');
                advance(c);
                m_url.m_userStart = currentPosition(c);
                m_url.m_userEnd = m_url.m_userStart;
                m_url.m_passwordEnd = m_url.m_userStart;
                m_url.m_hostEnd = m_url.m_userStart;
                m_url.m_portLength = 0;
                authorityOrHostBegin = c;
                state = State::FileHost;
                break;
            }
            {
                bool copiedHost = false;
                if (base.isValid() && base.protocolIsFile()) {
                    if (base.host().isEmpty()) {
                        copyURLPartsUntil(base, URLPart::SchemeEnd, c, nonUTF8QueryEncoding);
                        appendToASCIIBuffer(":///"_span8);
                    } else {
                        copyURLPartsUntil(base, URLPart::PortEnd, c, nonUTF8QueryEncoding);
                        appendToASCIIBuffer('/');
                        copiedHost = true;
                    }
                } else {
                    syntaxViolation(c);
                    appendToASCIIBuffer("//"_span8);
                }
                if (!copiedHost) {
                    m_url.m_userStart = currentPosition(c) - 1;
                    m_url.m_userEnd = m_url.m_userStart;
                    m_url.m_passwordEnd = m_url.m_userStart;
                    m_url.m_hostEnd = m_url.m_userStart;
                    m_url.m_portLength = 0;
                }
            }
            if (isWindowsDriveLetter(c)) {
                appendWindowsDriveLetter(c);
                m_url.m_pathAfterLastSlash = m_url.m_hostEnd + 1;
            } else if (copyBaseWindowsDriveLetter(base)) {
                appendToASCIIBuffer('/');
                m_url.m_pathAfterLastSlash = m_url.m_hostEnd + 4;
            } else
                m_url.m_pathAfterLastSlash = m_url.m_hostEnd + 1;
            state = State::Path;
            break;
        case State::FileHost:
            do {
                LOG_STATE("FileHost");
                if (isSlashQuestionOrHash(*c)) {
                    bool windowsQuirk = takesTwoAdvancesUntilEnd(CodePointIterator<CharacterType>(authorityOrHostBegin, c))
                        && isWindowsDriveLetter(authorityOrHostBegin);
                    if (windowsQuirk) {
                        syntaxViolation(authorityOrHostBegin);
                        appendToASCIIBuffer('/');
                        appendWindowsDriveLetter(authorityOrHostBegin);
                    }
                    if (windowsQuirk || authorityOrHostBegin == c) {
                        ASSERT(windowsQuirk || parsedDataView(currentPosition(c) - 1) == '/');
                        if (*c == '?') [[unlikely]] {
                            syntaxViolation(c);
                            appendToASCIIBuffer("/?"_span8);
                            ++c;
                            if (nonUTF8QueryEncoding) {
                                queryBegin = c;
                                state = State::NonUTF8Query;
                            } else
                                state = State::UTF8Query;
                            m_url.m_pathAfterLastSlash = currentPosition(c) - 1;
                            m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
                            break;
                        }
                        if (*c == '#') [[unlikely]] {
                            syntaxViolation(c);
                            appendToASCIIBuffer("/#"_span8);
                            ++c;
                            m_url.m_pathAfterLastSlash = currentPosition(c) - 1;
                            m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
                            m_url.m_queryEnd = m_url.m_pathAfterLastSlash;
                            state = State::Fragment;
                            break;
                        }
                        state = authorityOrHostBegin == c ? State::FilePathStart : State::Path;
                        break;
                    }
                    if (parseHostAndPort(CodePointIterator<CharacterType>(authorityOrHostBegin, c)) == HostParsingResult::InvalidHost) {
                        failure();
                        return;
                    }
                    if (isLocalhost(parsedDataView(m_url.m_passwordEnd, currentPosition(c) - m_url.m_passwordEnd))) [[unlikely]] {
                        syntaxViolation(c);
                        m_asciiBuffer.shrink(m_url.m_passwordEnd);
                        m_url.m_hostEnd = currentPosition(c);
                        m_url.m_portLength = 0;
                    }
                    
                    state = State::PathStart;
                    break;
                }
                if (isPercentOrNonASCII(*c))
                    m_hostHasPercentOrNonASCII = true;
                ++c;
            } while (!c.atEnd());
            break;
        case State::FilePathStart:
            LOG_STATE("FilePathStart");
            if (*c == '/' || *c == '\\') {
                if (m_urlIsSpecial && *c == '\\') [[unlikely]]
                    syntaxViolation(c);
                appendToASCIIBuffer('/');
                advance(c);
                m_url.m_pathAfterLastSlash = currentPosition(c);
                if (isWindowsDriveLetter(c)
                    && currentPosition(c) == m_url.m_hostEnd + 1)
                    appendWindowsDriveLetter(c);
            }
            state = State::Path;
            break;
        case State::PathStart:
            LOG_STATE("PathStart");
            if (*c != '/' && *c != '\\') {
                syntaxViolation(c);
                appendToASCIIBuffer('/');
            }
            m_url.m_pathAfterLastSlash = currentPosition(c);
            state = State::Path;
            break;
        case State::Path: {
            LOG_STATE("Path");
            auto* p = positionOf(c);
            bool afterSlash = m_didSeeSyntaxViolation ? (!m_asciiBuffer.isEmpty() && m_asciiBuffer.last() == '/') : (p != inputBegin && p[-1] == '/');
            ASSERT(afterSlash == (currentPosition(c) && parsedDataView(currentPosition(c) - 1) == '/'));
            while (true) {
                ASSERT(p != inputEnd);
                if (afterSlash && (*p == '.' || *p == '%')) [[unlikely]] {
                    c = iteratorAt(p);
                    bool isDoubleDot = isDoubleDotPathSegment(c);
                    if (isDoubleDot || isSingleDotPathSegment(c)) {
                        syntaxViolation(c);
                        if (isDoubleDot) {
                            consumeDoubleDotPathSegment(c);
                            popPath();
                        } else
                            consumeSingleDotPathSegment(c);
                        ASSERT(m_didSeeSyntaxViolation);
                        afterSlash = !m_asciiBuffer.isEmpty() && m_asciiBuffer.last() == '/';
                        p = positionOf(c);
                        if (p == inputEnd)
                            break;
                        continue;
                    }
                }
                auto* runStart = p;
                const CharacterType* lastSlash;
                p = findPathRunEnd(p, inputEnd, lastSlash);
                appendToASCIIBuffer(std::span(runStart, p));
                if (lastSlash)
                    m_url.m_pathAfterLastSlash = currentPosition(p) - (p - lastSlash - 1);
                if (p != inputEnd && (*p == '/' || (m_urlIsSpecial && *p == '\\'))) {
                    if (*p == '\\') [[unlikely]]
                        syntaxViolation(iteratorAt(p));
                    appendToASCIIBuffer('/');
                    ++p;
                    m_url.m_pathAfterLastSlash = currentPosition(p);
                    afterSlash = true;
                    if (p == inputEnd)
                        break;
                    continue;
                }
                break;
            }
            c = iteratorAt(p);
            if (c.atEnd() || isTabOrNewline(*c))
                break;
            if (*c == '?') {
                m_url.m_pathEnd = currentPosition(c);
                appendToASCIIBuffer('?');
                ++c;
                if (nonUTF8QueryEncoding) {
                    queryBegin = c;
                    state = State::NonUTF8Query;
                } else
                    state = State::UTF8Query;
                break;
            }
            if (*c == '#') {
                m_url.m_pathEnd = currentPosition(c);
                m_url.m_queryEnd = m_url.m_pathEnd;
                state = State::Fragment;
                break;
            }
            utf8PercentEncode<isInPathEncodeSet>(c);
            ++c;
            break;
        }
        case State::OpaquePath: {
            LOG_STATE("OpaquePath");
            auto* start = positionOf(c);
            auto* p = start;
            const CharacterType* afterLastSlash = nullptr;
            while (true) {
                p = findOpaquePathStopCharacterOrSlash(p, inputEnd);
                if (p == inputEnd || *p != '/')
                    break;
                afterLastSlash = ++p;
            }
            if (afterLastSlash) {
                appendToASCIIBuffer(std::span(start, afterLastSlash));
                m_url.m_pathAfterLastSlash = currentPosition(afterLastSlash);
                start = afterLastSlash;
            }
            appendToASCIIBuffer(std::span(start, p));
            c = iteratorAt(p);
            if (c.atEnd() || isTabOrNewline(*c))
                break;
            if (*c == '?') {
                m_url.m_pathEnd = currentPosition(c);
                appendToASCIIBuffer('?');
                ++c;
                if (nonUTF8QueryEncoding) {
                    queryBegin = c;
                    state = State::NonUTF8Query;
                } else
                    state = State::UTF8Query;
            } else if (*c == '#') {
                m_url.m_pathEnd = currentPosition(c);
                m_url.m_queryEnd = m_url.m_pathEnd;
                state = State::Fragment;
            } else if (*c == '/') {
                appendToASCIIBuffer('/');
                ++c;
                m_url.m_pathAfterLastSlash = currentPosition(c);
            } else if (*c == ' ') {
                auto nextC = c;
                advance<CharacterType, ReportSyntaxViolation::No>(nextC);
                ASSERT(!nextC.atEnd());
                if (*nextC == '?' || *nextC == '#') {
                    syntaxViolation(c);
                    percentEncodeByte(' ');
                } else
                    appendToASCIIBuffer(' ');
                ++c;
            } else {
                utf8PercentEncode<isInC0ControlEncodeSet>(c);
                ++c;
            }
            break;
        }
        case State::UTF8Query: {
            LOG_STATE("UTF8Query");
            ASSERT(queryBegin == CodePointIterator<CharacterType>());
            auto* start = positionOf(c);
            auto* p = findQueryStopCharacter(start, inputEnd);
            appendToASCIIBuffer(std::span(start, p));
            c = iteratorAt(p);
            if (c.atEnd() || isTabOrNewline(*c))
                break;
            if (*c == '#') {
                m_url.m_queryEnd = currentPosition(c);
                state = State::Fragment;
                break;
            }
            ASSERT(!nonUTF8QueryEncoding);
            utf8QueryEncode(c);
            ++c;
            break;
        }
        case State::NonUTF8Query:
            do {
                LOG_STATE("NonUTF8Query");
                ASSERT(queryBegin != CodePointIterator<CharacterType>());
                if (*c == '#') {
                    encodeNonUTF8Query(queryBuffer, *nonUTF8QueryEncoding, CodePointIterator<CharacterType>(queryBegin, c));
                    m_url.m_queryEnd = currentPosition(c);
                    state = State::Fragment;
                    break;
                }
                appendCodePoint(queryBuffer, *c);
                advance(c, queryBegin);
            } while (!c.atEnd());
            break;
        case State::Fragment: {
            URL_PARSER_LOG("State Fragment");
            auto* start = positionOf(c);
            auto* p = findFragmentStopCharacter(start, inputEnd);
            appendToASCIIBuffer(std::span(start, p));
            c = iteratorAt(p);
            if (c.atEnd() || isTabOrNewline(*c))
                break;
            utf8PercentEncode<isInFragmentEncodeSet>(c);
            ++c;
            break;
        }
        }
    }

    switch (state) {
    case State::SchemeStart:
        LOG_FINAL_STATE("SchemeStart");
        if (!currentPosition(c) && base.isValid() && !base.m_hasOpaquePath) {
            m_url = base;
            m_url.removeFragmentIdentifier();
            return;
        }
        failure();
        return;
    case State::Scheme:
        LOG_FINAL_STATE("Scheme");
        failure();
        return;
    case State::NoScheme:
        LOG_FINAL_STATE("NoScheme");
        RELEASE_ASSERT_NOT_REACHED();
    case State::SpecialRelativeOrAuthority:
        LOG_FINAL_STATE("SpecialRelativeOrAuthority");
        copyURLPartsUntil(base, URLPart::QueryEnd, c, nonUTF8QueryEncoding);
        break;
    case State::PathOrAuthority:
        LOG_FINAL_STATE("PathOrAuthority");
        ASSERT(m_url.m_userStart);
        ASSERT(m_url.m_userStart == currentPosition(c));
        ASSERT(parsedDataView(currentPosition(c) - 1) == '/');
        m_url.m_userStart--;
        m_url.m_userEnd = m_url.m_userStart;
        m_url.m_passwordEnd = m_url.m_userStart;
        m_url.m_hostEnd = m_url.m_userStart;
        m_url.m_portLength = 0;
        m_url.m_pathAfterLastSlash = m_url.m_userStart + 1;
        m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
        m_url.m_queryEnd = m_url.m_pathAfterLastSlash;
        break;
    case State::Relative:
        LOG_FINAL_STATE("Relative");
        RELEASE_ASSERT_NOT_REACHED();
    case State::RelativeSlash:
        LOG_FINAL_STATE("RelativeSlash");
        copyURLPartsUntil(base, URLPart::PortEnd, c, nonUTF8QueryEncoding);
        appendToASCIIBuffer('/');
        m_url.m_pathAfterLastSlash = m_url.m_hostEnd + m_url.m_portLength + 1;
        m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
        m_url.m_queryEnd = m_url.m_pathAfterLastSlash;
        break;
    case State::SpecialAuthoritySlashes:
        LOG_FINAL_STATE("SpecialAuthoritySlashes");
        failure();
        return;
    case State::SpecialAuthorityIgnoreSlashes:
        LOG_FINAL_STATE("SpecialAuthorityIgnoreSlashes");
        failure();
        return;
    case State::AuthorityOrHost:
        LOG_FINAL_STATE("AuthorityOrHost");
        m_url.m_userEnd = currentPosition(authorityOrHostBegin);
        m_url.m_passwordEnd = m_url.m_userEnd;
        if (authorityOrHostBegin.atEnd()) {
            m_url.m_userEnd = m_url.m_userStart;
            m_url.m_passwordEnd = m_url.m_userStart;
            m_url.m_hostEnd = m_url.m_userStart;
            m_url.m_portLength = 0;
            m_url.m_pathEnd = m_url.m_userStart;
        } else if (parseHostAndPort(authorityOrHostBegin) == HostParsingResult::InvalidHost) {
            failure();
            return;
        } else {
            if (m_urlIsSpecial) {
                syntaxViolation(c);
                appendToASCIIBuffer('/');
                m_url.m_pathEnd = m_url.m_hostEnd + m_url.m_portLength + 1;
            } else
                m_url.m_pathEnd = m_url.m_hostEnd + m_url.m_portLength;
        }
        m_url.m_pathAfterLastSlash = m_url.m_pathEnd;
        m_url.m_queryEnd = m_url.m_pathEnd;
        break;
    case State::Host:
        LOG_FINAL_STATE("Host");
        if (parseHostAndPort(authorityOrHostBegin) == HostParsingResult::InvalidHost) {
            failure();
            return;
        }
        if (m_urlIsSpecial) {
            syntaxViolation(c);
            appendToASCIIBuffer('/');
            m_url.m_pathEnd = m_url.m_hostEnd + m_url.m_portLength + 1;
        } else
            m_url.m_pathEnd = m_url.m_hostEnd + m_url.m_portLength;
        m_url.m_pathAfterLastSlash = m_url.m_pathEnd;
        m_url.m_queryEnd = m_url.m_pathEnd;
        break;
    case State::File:
        LOG_FINAL_STATE("File");
        if (base.isValid() && base.protocolIsFile()) {
            copyURLPartsUntil(base, URLPart::QueryEnd, c, nonUTF8QueryEncoding);
            break;
        }
        syntaxViolation(c);
        appendToASCIIBuffer("///"_span8);
        m_url.m_userStart = currentPosition(c) - 1;
        m_url.m_userEnd = m_url.m_userStart;
        m_url.m_passwordEnd = m_url.m_userStart;
        m_url.m_hostEnd = m_url.m_userStart;
        m_url.m_portLength = 0;
        m_url.m_pathAfterLastSlash = m_url.m_userStart + 1;
        m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
        m_url.m_queryEnd = m_url.m_pathAfterLastSlash;
        break;
    case State::FileSlash:
        LOG_FINAL_STATE("FileSlash");
        syntaxViolation(c);
        {
            bool copiedHost = false;
            if (base.isValid() && base.protocolIsFile()) {
                if (base.host().isEmpty()) {
                    copyURLPartsUntil(base, URLPart::SchemeEnd, c, nonUTF8QueryEncoding);
                    appendToASCIIBuffer(":/"_span8);
                } else {
                    copyURLPartsUntil(base, URLPart::PortEnd, c, nonUTF8QueryEncoding);
                    appendToASCIIBuffer('/');
                    copiedHost = true;
                }
            }
            if (!copiedHost) {
                m_url.m_userStart = currentPosition(c) + 1;
                appendToASCIIBuffer("//"_span8);
                m_url.m_userEnd = m_url.m_userStart;
                m_url.m_passwordEnd = m_url.m_userStart;
                m_url.m_hostEnd = m_url.m_userStart;
                m_url.m_portLength = 0;
            }
        }
        if (copyBaseWindowsDriveLetter(base)) {
            appendToASCIIBuffer('/');
            m_url.m_pathAfterLastSlash = m_url.m_hostEnd + 4;
        } else
            m_url.m_pathAfterLastSlash = m_url.m_hostEnd + 1;
        m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
        m_url.m_queryEnd = m_url.m_pathAfterLastSlash;
        break;
    case State::FileHost:
        LOG_FINAL_STATE("FileHost");
        if (takesTwoAdvancesUntilEnd(CodePointIterator<CharacterType>(authorityOrHostBegin, c))
            && isWindowsDriveLetter(authorityOrHostBegin)) {
            syntaxViolation(authorityOrHostBegin);
            appendToASCIIBuffer('/');
            appendWindowsDriveLetter(authorityOrHostBegin);
            m_url.m_pathAfterLastSlash = currentPosition(c);
            m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
            m_url.m_queryEnd = m_url.m_pathAfterLastSlash;
            break;
        }
        
        if (authorityOrHostBegin == c) {
            syntaxViolation(c);
            appendToASCIIBuffer('/');
            m_url.m_userStart = currentPosition(c) - 1;
            m_url.m_userEnd = m_url.m_userStart;
            m_url.m_passwordEnd = m_url.m_userStart;
            m_url.m_hostEnd = m_url.m_userStart;
            m_url.m_portLength = 0;
            m_url.m_pathAfterLastSlash = m_url.m_userStart + 1;
            m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
            m_url.m_queryEnd = m_url.m_pathAfterLastSlash;
            break;
        }

        if (parseHostAndPort(CodePointIterator<CharacterType>(authorityOrHostBegin, c)) == HostParsingResult::InvalidHost) {
            failure();
            return;
        }

        syntaxViolation(c);
        if (isLocalhost(parsedDataView(m_url.m_passwordEnd, currentPosition(c) - m_url.m_passwordEnd))) {
            m_asciiBuffer.shrink(m_url.m_passwordEnd);
            m_url.m_hostEnd = currentPosition(c);
            m_url.m_portLength = 0;
        }
        appendToASCIIBuffer('/');
        m_url.m_pathAfterLastSlash = m_url.m_hostEnd + m_url.m_portLength + 1;
        m_url.m_pathEnd = m_url.m_pathAfterLastSlash;
        m_url.m_queryEnd = m_url.m_pathAfterLastSlash;
        break;
    case State::PathStart:
        LOG_FINAL_STATE("PathStart");
        RELEASE_ASSERT_NOT_REACHED();
    case State::FilePathStart:
        [[fallthrough]];
    case State::Path:
        LOG_FINAL_STATE("Path");
        m_url.m_pathEnd = currentPosition(c);
        m_url.m_queryEnd = m_url.m_pathEnd;
        break;
    case State::OpaquePath:
        LOG_FINAL_STATE("OpaquePath");
        m_url.m_pathEnd = currentPosition(c);
        m_url.m_queryEnd = m_url.m_pathEnd;
        break;
    case State::UTF8Query:
        LOG_FINAL_STATE("UTF8Query");
        ASSERT(queryBegin == CodePointIterator<CharacterType>());
        m_url.m_queryEnd = currentPosition(c);
        break;
    case State::NonUTF8Query:
        LOG_FINAL_STATE("NonUTF8Query");
        ASSERT(queryBegin != CodePointIterator<CharacterType>());
        encodeNonUTF8Query(queryBuffer, *nonUTF8QueryEncoding, CodePointIterator<CharacterType>(queryBegin, c));
        m_url.m_queryEnd = currentPosition(c);
        break;
    case State::Fragment:
        LOG_FINAL_STATE("Fragment");
        break;
    }

    if (!m_didSeeSyntaxViolation) [[likely]] {
        m_url.m_string = releaseInputString();
        ASSERT(m_asciiBuffer.isEmpty());
    } else
        m_url.m_string = String::adopt(WTF::move(m_asciiBuffer));
    m_url.m_isValid = true;
    URL_PARSER_LOG("Parsed URL <%s>\n\n", m_url.m_string.utf8().data());
}

template<typename CharacterType>
void URLParser::parseAuthority(CodePointIterator<CharacterType> iterator)
{
    if (iterator.atEnd()) [[unlikely]] {
        syntaxViolation(iterator);
        m_url.m_userEnd = currentPosition(iterator);
        m_url.m_passwordEnd = m_url.m_userEnd;
        return;
    }
    auto* end = iterator.position() + iterator.remainingCodeUnits().size();
    // Skips a run of characters that are copied unchanged, leaving iterator where advancing from the last of them would.
    auto copyPlainCharacters = [&] ALWAYS_INLINE_LAMBDA {
        auto* runStart = iterator.position();
        auto* p = runStart;
        while (p != end && !WTF::isInUserInfoEncodeSet(*p))
            ++p;
        if (p == runStart)
            return;
        appendToASCIIBuffer(std::span(runStart, p));
        iterator = CodePointIterator<CharacterType>(std::span(p - 1, end));
        advance(iterator);
    };
    for (; !iterator.atEnd(); advance(iterator)) {
        copyPlainCharacters();
        if (iterator.atEnd())
            break;
        if (*iterator == ':') {
            m_url.m_userEnd = currentPosition(iterator);
            auto iteratorAtColon = iterator;
            ++iterator;
            bool tabOrNewlineAfterColon = false;
            while (!iterator.atEnd() && isTabOrNewline(*iterator)) [[unlikely]] {
                tabOrNewlineAfterColon = true;
                ++iterator;
            }
            if (iterator.atEnd()) [[unlikely]] {
                syntaxViolation(iteratorAtColon);
                m_url.m_passwordEnd = m_url.m_userEnd;
                if (m_url.m_userEnd > m_url.m_userStart)
                    appendToASCIIBuffer('@');
                return;
            }
            if (tabOrNewlineAfterColon)
                syntaxViolation(iteratorAtColon);
            appendToASCIIBuffer(':');
            break;
        }
        utf8PercentEncode<WTF::isInUserInfoEncodeSet>(iterator);
    }
    for (; !iterator.atEnd(); advance(iterator)) {
        copyPlainCharacters();
        if (iterator.atEnd())
            break;
        utf8PercentEncode<WTF::isInUserInfoEncodeSet>(iterator);
    }
    m_url.m_passwordEnd = currentPosition(iterator);
    if (!m_url.m_userEnd)
        m_url.m_userEnd = m_url.m_passwordEnd;
    appendToASCIIBuffer('@');
}

template<typename UnsignedIntegerType>
void URLParser::appendNumberToASCIIBuffer(UnsignedIntegerType number)
{
    constexpr size_t bufferSize = sizeof(UnsignedIntegerType) * 3 + 1;
    std::array<Latin1Character, bufferSize> buffer;
    size_t index = bufferSize;
    do {
        buffer[--index] = static_cast<char>((number % 10) + '0');
        number /= 10;
    } while (number);
    appendToASCIIBuffer(std::span { buffer }.subspan(index));
}

void URLParser::serializeIPv4(IPv4Address address)
{
    appendNumberToASCIIBuffer<uint8_t>(address >> 24);
    appendToASCIIBuffer('.');
    appendNumberToASCIIBuffer<uint8_t>(address >> 16);
    appendToASCIIBuffer('.');
    appendNumberToASCIIBuffer<uint8_t>(address >> 8);
    appendToASCIIBuffer('.');
    appendNumberToASCIIBuffer<uint8_t>(address);
}
    
static size_t NODELETE zeroSequenceLength(const std::array<uint16_t, 8>& address, size_t begin)
{
    size_t end = begin;
    for (; end < 8; end++) {
        if (address[end])
            break;
    }
    return end - begin;
}

static std::optional<size_t> NODELETE findLongestZeroSequence(const std::array<uint16_t, 8>& address)
{
    std::optional<size_t> longest;
    size_t longestLength = 0;
    for (size_t i = 0; i < 8; i++) {
        size_t length = zeroSequenceLength(address, i);
        if (length) {
            if (length > 1 && (!longest || longestLength < length)) {
                longest = i;
                longestLength = length;
            }
            i += length;
        }
    }
    return longest;
}

void URLParser::serializeIPv6Piece(uint16_t piece)
{
    bool printed = false;
    if (auto nibble0 = piece >> 12) {
        appendToASCIIBuffer(lowerNibbleToLowercaseASCIIHexDigit(nibble0));
        printed = true;
    }
    auto nibble1 = piece >> 8 & 0xF;
    if (printed || nibble1) {
        appendToASCIIBuffer(lowerNibbleToLowercaseASCIIHexDigit(nibble1));
        printed = true;
    }
    auto nibble2 = piece >> 4 & 0xF;
    if (printed || nibble2)
        appendToASCIIBuffer(lowerNibbleToLowercaseASCIIHexDigit(nibble2));
    appendToASCIIBuffer(lowerNibbleToLowercaseASCIIHexDigit(piece & 0xF));
}

void URLParser::serializeIPv6(URLParser::IPv6Address address)
{
    appendToASCIIBuffer('[');
    auto compressPointer = findLongestZeroSequence(address);
    for (size_t piece = 0; piece < 8; piece++) {
        if (compressPointer && compressPointer.value() == piece) {
            ASSERT(!address[piece]);
            if (piece)
                appendToASCIIBuffer(':');
            else
                appendToASCIIBuffer("::"_span8);
            while (piece < 8 && !address[piece])
                piece++;
            if (piece == 8)
                break;
        }
        serializeIPv6Piece(address[piece]);
        if (piece < 7)
            appendToASCIIBuffer(':');
    }
    appendToASCIIBuffer(']');
}

ALWAYS_INLINE static uint64_t pow256(size_t exponent)
{
    RELEASE_ASSERT(exponent <= 4);
    static constexpr std::array<uint64_t, 5> values { 1, 256, 256 * 256, 256 * 256 * 256, 256ull * 256 * 256 * 256 };
    return values[exponent];
}

enum class URLParser::IPv4ParsingError {
    Failure,
    NotIPv4,
};

// https://url.spec.whatwg.org/#concept-ipv4-parser
template<typename CharacterTypeForSyntaxViolation, typename CharacterType>
Expected<URLParser::IPv4Address, URLParser::IPv4ParsingError> URLParser::parseIPv4Host(const CodePointIterator<CharacterTypeForSyntaxViolation>& iteratorForSyntaxViolationPosition, std::span<const CharacterType> host)
{
    auto* p = host.data();
    auto* end = p + host.size();

    // Four decimal pieces without leading zeros need none of the bookkeeping below.
    {
        IPv4Address address = 0;
        auto* q = p;
        for (unsigned piece = 0; ; ++piece) {
            if (q == end || !isASCIIDigit(*q))
                break;
            unsigned value = *q++ - '0';
            if (value) {
                while (q != end && isASCIIDigit(*q) && value <= 255)
                    value = value * 10 + *q++ - '0';
                if (value > 255)
                    break;
            }
            address = address << 8 | value;
            if (piece == 3) {
                if (q == end)
                    return address;
                break;
            }
            if (q == end || *q != '.')
                break;
            ++q;
        }
    }

    std::array<uint32_t, 4> pieces;
    unsigned pieceCount = 0;
    bool didSeeSyntaxViolation = false;
    bool didSeeOverflow = false;
    if (p != end && *p == '.')
        return makeUnexpected(IPv4ParsingError::NotIPv4);
    while (p != end) {
        if (isTabOrNewline(*p)) [[unlikely]] {
            didSeeSyntaxViolation = true;
            ++p;
            continue;
        }
        if (pieceCount >= 4 || *p == '.')
            return makeUnexpected(IPv4ParsingError::NotIPv4);

        uint64_t value = 0;
        bool pieceDidOverflow = false;
        constexpr uint64_t maxValue = std::numeric_limits<uint32_t>::max();
        if (*p == '0') [[unlikely]] {
            ++p;
            while (p != end && isTabOrNewline(*p)) [[unlikely]] {
                didSeeSyntaxViolation = true;
                ++p;
            }
            if (p != end && *p != '.') {
                didSeeSyntaxViolation = true;
                if (*p == 'x' || *p == 'X') {
                    for (++p; p != end; ++p) {
                        auto character = *p;
                        if (isASCIIHexDigit(character)) [[likely]] {
                            value = value * 16 + toASCIIHexValue(character);
                            if (value > maxValue) [[unlikely]] {
                                pieceDidOverflow = true;
                                break;
                            }
                        } else if (character == '.')
                            break;
                        else if (isTabOrNewline(character)) [[unlikely]]
                            didSeeSyntaxViolation = true;
                        else
                            return makeUnexpected(IPv4ParsingError::NotIPv4);
                    }
                } else {
                    for (; p != end; ++p) {
                        auto character = *p;
                        if (character >= '0' && character <= '7') [[likely]] {
                            value = value * 8 + (character - '0');
                            if (value > maxValue) [[unlikely]] {
                                pieceDidOverflow = true;
                                break;
                            }
                        } else if (character == '.')
                            break;
                        else if (isTabOrNewline(character)) [[unlikely]]
                            didSeeSyntaxViolation = true;
                        else
                            return makeUnexpected(IPv4ParsingError::NotIPv4);
                    }
                }
            }
        } else {
            for (; p != end; ++p) {
                auto character = *p;
                if (isASCIIDigit(character)) [[likely]] {
                    value = value * 10 + (character - '0');
                    if (value > maxValue) [[unlikely]] {
                        pieceDidOverflow = true;
                        break;
                    }
                } else if (character == '.')
                    break;
                else if (isTabOrNewline(character)) [[unlikely]]
                    didSeeSyntaxViolation = true;
                else
                    return makeUnexpected(IPv4ParsingError::NotIPv4);
            }
        }
        // On overflow the overflowing digit is left unconsumed and starts a new piece, so what follows can only decide between Failure and NotIPv4.
        if (pieceDidOverflow) [[unlikely]] {
            didSeeOverflow = true;
            pieces[pieceCount++] = 0;
            continue;
        }
        pieces[pieceCount++] = static_cast<uint32_t>(value);
        if (p != end && *p == '.') {
            ++p;
            if (p == end)
                didSeeSyntaxViolation = true;
            else if (*p == '.')
                return makeUnexpected(IPv4ParsingError::NotIPv4);
        }
    }
    if (!pieceCount || pieceCount > 4)
        return makeUnexpected(IPv4ParsingError::NotIPv4);
    if (didSeeOverflow)
        return makeUnexpected(IPv4ParsingError::Failure);
    for (unsigned i = 0; i + 1 < pieceCount; i++) {
        if (pieces[i] > 255)
            return makeUnexpected(IPv4ParsingError::Failure);
    }
    if (pieces[pieceCount - 1] >= pow256(5 - pieceCount))
        return makeUnexpected(IPv4ParsingError::Failure);

    if (didSeeSyntaxViolation || pieceCount != 4 || pieces[pieceCount - 1] > 255) [[unlikely]]
        syntaxViolation(iteratorForSyntaxViolationPosition);

    IPv4Address ipv4 = pieces[pieceCount - 1];
    for (unsigned counter = 0; counter + 1 < pieceCount; ++counter)
        ipv4 += pieces[counter] * pow256(3 - counter);
    return ipv4;
}

template<typename CharacterType>
std::optional<uint32_t> URLParser::parseIPv4PieceInsideIPv6(std::span<const CharacterType>& remaining)
{
    auto* p = remaining.data();
    auto* end = p + remaining.size();
    if (p == end)
        return std::nullopt;
    uint32_t piece = 0;
    bool leadingZeros = false;
    while (p != end) {
        if (!isASCIIDigit(*p))
            return std::nullopt;
        if (!piece && *p == '0') {
            if (leadingZeros)
                return std::nullopt;
            leadingZeros = true;
        }
        piece = piece * 10 + *p - '0';
        if (piece > 255)
            return std::nullopt;
        ++p;
        if (p == end)
            break;
        if (*p == '.')
            break;
    }
    remaining = std::span(p, end);
    if (piece && leadingZeros)
        return std::nullopt;
    return piece;
}

template<typename CharacterType>
std::optional<URLParser::IPv4Address> URLParser::parseIPv4AddressInsideIPv6(std::span<const CharacterType> remaining)
{
    IPv4Address address = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (std::optional<uint32_t> piece = parseIPv4PieceInsideIPv6(remaining))
            address = (address << 8) + piece.value();
        else
            return std::nullopt;
        if (i < 3) {
            if (remaining.empty())
                return std::nullopt;
            if (remaining.front() != '.')
                return std::nullopt;
            remaining = remaining.subspan(1);
        } else if (!remaining.empty())
            return std::nullopt;
    }
    ASSERT(remaining.empty());
    return address;
}

// https://url.spec.whatwg.org/#concept-ipv6-parser
// `address` is what is between the brackets, with tabs and newlines already removed (and reported).
template<typename CharacterType>
std::optional<URLParser::IPv6Address> URLParser::parseIPv6Host(CodePointIterator<CharacterType> hostBegin, std::span<const CharacterType> addressCharacters)
{
    auto* p = addressCharacters.data();
    auto* end = p + addressCharacters.size();
    if (p == end)
        return std::nullopt;

    IPv6Address address = {{0, 0, 0, 0, 0, 0, 0, 0}};
    size_t piecePointer = 0;
    std::optional<size_t> compressPointer;
    bool previousValueWasZero = false;
    bool immediatelyAfterCompress = false;

    if (*p == ':') {
        ++p;
        if (p == end)
            return std::nullopt;
        if (*p != ':')
            return std::nullopt;
        ++p;
        ++piecePointer;
        compressPointer = piecePointer;
        immediatelyAfterCompress = true;
    }

    while (p != end) {
        if (piecePointer == 8)
            return std::nullopt;
        if (*p == ':') {
            if (compressPointer)
                return std::nullopt;
            ++p;
            ++piecePointer;
            compressPointer = piecePointer;
            immediatelyAfterCompress = true;
            if (previousValueWasZero)
                syntaxViolation(hostBegin);
            continue;
        }
        if (piecePointer == 6 || (compressPointer && piecePointer < 6)) {
            if (std::optional<IPv4Address> ipv4Address = parseIPv4AddressInsideIPv6(std::span(p, end))) {
                if (compressPointer && piecePointer == 5)
                    return std::nullopt;
                syntaxViolation(hostBegin);
                address[piecePointer++] = ipv4Address.value() >> 16;
                address[piecePointer++] = ipv4Address.value() & 0xFFFF;
                p = end;
                break;
            }
        }
        uint16_t value = 0;
        size_t length = 0;
        bool leadingZeros = false;
        bool sawUppercase = false;
        for (; length < 4 && p != end; ++length, ++p) {
            unsigned character = *p;
            unsigned digitValue;
            if (isASCIIDigit(character))
                digitValue = character - '0';
            else {
                unsigned lowercased = character | 0x20;
                if (lowercased < 'a' || lowercased > 'f')
                    break;
                digitValue = lowercased - 'a' + 10;
                sawUppercase |= !(character & 0x20);
            }
            if (!length)
                leadingZeros = character == '0';
            value = value * 0x10 + digitValue;
        }
        if (sawUppercase) [[unlikely]]
            syntaxViolation(hostBegin);

        previousValueWasZero = !value;
        if ((value && leadingZeros) || (previousValueWasZero && (length > 1 || immediatelyAfterCompress))) [[unlikely]]
            syntaxViolation(hostBegin);

        address[piecePointer++] = value;
        if (p == end)
            break;
        if (piecePointer == 8 || *p != ':')
            return std::nullopt;
        ++p;
        if (p == end)
            syntaxViolation(hostBegin);

        immediatelyAfterCompress = false;
    }

    if (p != end)
        return std::nullopt;

    if (compressPointer) {
        size_t swaps = piecePointer - compressPointer.value();
        piecePointer = 7;
        while (swaps)
            std::swap(address[piecePointer--], address[compressPointer.value() + swaps-- - 1]);
    } else if (piecePointer != 8)
        return std::nullopt;

    std::optional<size_t> possibleCompressPointer = findLongestZeroSequence(address);
    if (possibleCompressPointer)
        possibleCompressPointer.value()++;
    if (compressPointer != possibleCompressPointer) [[unlikely]]
        syntaxViolation(hostBegin);

    return address;
}

// FIXME: This function should take span<const char8_t>, since it requires UTF-8.
template<typename SyntaxViolationHandler>
URLParser::Latin1Buffer URLParser::percentDecodeImpl(std::span<const Latin1Character> input, SyntaxViolationHandler&& syntaxViolationHandler)
{
    Latin1Buffer output;
    output.grow(input.size());
    size_t outputLength = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        uint8_t byte = input[i];
        if (byte == '%' && i + 2 < input.size() && isASCIIHexDigit(input[i + 1]) && isASCIIHexDigit(input[i + 2])) [[unlikely]] {
            syntaxViolationHandler();
            byte = toASCIIHexValue(input[i + 1], input[i + 2]);
            i += 2;
        }
        output[outputLength++] = byte;
    }
    output.shrink(outputLength);
    return output;
}

template<typename CharacterType>
URLParser::Latin1Buffer URLParser::percentDecode(std::span<const Latin1Character> input, const CodePointIterator<CharacterType>& iteratorForSyntaxViolationPosition)
{
    return percentDecodeImpl(input, [&] { syntaxViolation(iteratorForSyntaxViolationPosition); });
}

URLParser::Latin1Buffer URLParser::percentDecode(std::span<const Latin1Character> input)
{
    return percentDecodeImpl(input, [] { });
}

bool URLParser::needsNonSpecialDotSlash() const
{
    auto pathStart = m_url.m_hostEnd + m_url.m_portLength;
    return !m_urlIsSpecial
        && pathStart == m_url.m_schemeEnd + 1U
        && pathStart + 1 < m_url.m_string.length()
        && m_url.m_string[pathStart] == '/'
        && m_url.m_string[pathStart + 1] == '/';
}

void URLParser::addNonSpecialDotSlash()
{
    auto oldPathStart = m_url.m_hostEnd + m_url.m_portLength;
    auto& oldString = m_url.m_string;
    m_url.m_string = makeString(StringView(oldString).left(oldPathStart + 1), "./"_s, StringView(oldString).substring(oldPathStart + 1));
    m_url.m_pathAfterLastSlash += 2;
    m_url.m_pathEnd += 2;
    m_url.m_queryEnd += 2;
}

template<typename CharacterType> std::optional<URLParser::Latin1Buffer> URLParser::domainToASCII(StringView domain, const CodePointIterator<CharacterType>& iteratorForSyntaxViolationPosition)
{
    Latin1Buffer ascii;
    if (domain.containsOnlyASCII()) {
        ascii.grow(domain.length());
        bool sawUppercase = false;
        auto lowercase = [&](auto characters) {
            for (size_t i = 0; i < characters.size(); ++i) {
                auto character = characters[i];
                sawUppercase |= isASCIIUpper(character);
                ascii[i] = toASCIILower(character);
            }
        };
        if (domain.is8Bit())
            lowercase(domain.span8());
        else
            lowercase(domain.span16());
        if (sawUppercase) [[unlikely]]
            syntaxViolation(iteratorForSyntaxViolationPosition);
        return ascii;
    }

    std::array<char16_t, hostnameBufferLength> hostnameBuffer;
    UErrorCode error = U_ZERO_ERROR;
    UIDNAInfo processingDetails = UIDNA_INFO_INITIALIZER;
    size_t numCharactersConverted = uidna_nameToASCII(&internationalDomainNameTranscoder(), domain.upconvertedCharacters(), domain.length(), hostnameBuffer.data(), hostnameBufferLength, &processingDetails, &error);

    if (U_SUCCESS(error) && !(processingDetails.errors & ~allowedNameToASCIIErrors) && numCharactersConverted) {
#if ASSERT_ENABLED
        for (size_t i = 0; i < numCharactersConverted; ++i) {
            ASSERT(isASCII(hostnameBuffer[i]));
            ASSERT(!isASCIIUpper(hostnameBuffer[i]));
        }
#else
        UNUSED_PARAM(numCharactersConverted);
#endif // ASSERT_ENABLED
        ascii.append(std::span { hostnameBuffer }.first(numCharactersConverted));
        if (!m_didSeeSyntaxViolation && domain != StringView(ascii.span()))
            syntaxViolation(iteratorForSyntaxViolationPosition);
        return ascii;
    }
    return std::nullopt;
}

bool URLParser::hasForbiddenHostCodePoint(const URLParser::Latin1Buffer& asciiDomain)
{
    for (auto character : asciiDomain) {
        if (isForbiddenDomainCodePoint(character))
            return true;
    }
    return false;
}

template<typename CharacterType>
bool URLParser::parsePort(CodePointIterator<CharacterType>& iterator)
{
    if (m_urlIsFile) [[unlikely]]
        return false;

    ASSERT(*iterator == ':');
    auto colonIterator = iterator;
    auto* p = iterator.position() + 1;
    auto* end = iterator.position() + iterator.remainingCodeUnits().size();
    iterator = CodePointIterator<CharacterType>(std::span(end, end));
    for (; p != end && isTabOrNewline(*p); ++p)
        syntaxViolation(colonIterator);
    uint32_t port = 0;
    if (p == end) [[unlikely]] {
        unsigned portLength = currentPosition(colonIterator) - m_url.m_hostEnd;
        RELEASE_ASSERT(portLength <= URL::maxPortLength);
        m_url.m_portLength = portLength;
        syntaxViolation(colonIterator);
        return true;
    }
    size_t digitCount = 0;
    bool leadingZeros = false;
    for (; p != end; ++p) {
        if (isTabOrNewline(*p)) [[unlikely]] {
            syntaxViolation(colonIterator);
            continue;
        }
        if (isASCIIDigit(*p)) {
            if (*p == '0' && !digitCount)
                leadingZeros = true;
            ++digitCount;
            port = port * 10 + *p - '0';
            if (port > std::numeric_limits<uint16_t>::max())
                return false;
        } else
            return false;
    }

    if (port && leadingZeros)
        syntaxViolation(colonIterator);

    if (!port && digitCount > 1)
        syntaxViolation(colonIterator);

    ASSERT(port == static_cast<uint16_t>(port));
    if (defaultPortForProtocol(parsedDataView(0, m_url.m_schemeEnd)) == static_cast<uint16_t>(port)) [[unlikely]]
        syntaxViolation(colonIterator);
    else {
        appendToASCIIBuffer(':');
        ASSERT(port <= std::numeric_limits<uint16_t>::max());
        appendNumberToASCIIBuffer<uint16_t>(static_cast<uint16_t>(port));
    }

    unsigned portLength = currentPosition(end) - m_url.m_hostEnd;
    RELEASE_ASSERT(portLength <= URL::maxPortLength);
    m_url.m_portLength = portLength;
    return true;
}

static bool dnsNameEndsInNumber(StringView name)
{
    // https://url.spec.whatwg.org/#ends-in-a-number-checker
    auto containsOctalDecimalOrHexNumber = [] (StringView segment) {
        const auto segmentLength = segment.length();
        if (!segmentLength) [[unlikely]]
            return false;
        auto firstCodeUnit = segment[0];
        if (!isASCIIDigit(firstCodeUnit)) [[likely]]
            return false;
        if (segmentLength == 1)
            return true;
        auto secondCodeUnit = segment[1];
        if ((secondCodeUnit == 'x' || secondCodeUnit == 'X') && firstCodeUnit == '0')
            return segment.find(std::not_fn(isASCIIHexDigit<char16_t>), 2) == notFound;
        return !segment.contains(std::not_fn(isASCIIDigit<char16_t>));
    };

    size_t lastDotLocation = name.reverseFind('.');
    if (lastDotLocation == notFound)
        return containsOctalDecimalOrHexNumber(name);
    size_t lastSegmentEnd = name.length();
    if (lastDotLocation == lastSegmentEnd - 1) {
        lastSegmentEnd = lastDotLocation;
        lastDotLocation = name.reverseFind('.', lastDotLocation - 1);
    }
    StringView lastPart = name.substring(lastDotLocation == notFound ? 0 : lastDotLocation + 1, lastSegmentEnd - lastDotLocation - 1);
    return containsOctalDecimalOrHexNumber(lastPart);
}

template<typename CharacterType>
auto URLParser::parseHostAndPort(CodePointIterator<CharacterType> iterator) -> HostParsingResult
{
    if (iterator.atEnd())
        return HostParsingResult::InvalidHost;
    if (*iterator == ':')
        return HostParsingResult::InvalidHost;
    if (*iterator == '[') {
        auto* addressBegin = iterator.position() + 1;
        auto* end = iterator.position() + iterator.remainingCodeUnits().size();
        auto* addressEnd = addressBegin;
        bool hasTabOrNewline = false;
        for (; addressEnd != end && *addressEnd != ']'; ++addressEnd)
            hasTabOrNewline |= isTabOrNewline(*addressEnd);
        if (addressEnd == end)
            return HostParsingResult::InvalidHost;
        auto ipv6End = CodePointIterator<CharacterType>(std::span(addressEnd, end));
        std::span<const CharacterType> addressCharacters(addressBegin, addressEnd);
        // The longest valid address is 0000:0000:0000:0000:0000:0000:000.000.000.000.
        std::array<CharacterType, 45> buffer;
        if (hasTabOrNewline) [[unlikely]] {
            syntaxViolation(iterator);
            size_t length = 0;
            for (auto character : addressCharacters) {
                if (isTabOrNewline(character))
                    continue;
                if (length == buffer.size())
                    return HostParsingResult::InvalidHost;
                buffer[length++] = character;
            }
            addressCharacters = std::span<const CharacterType>(buffer).first(length);
        }
        if (auto address = parseIPv6Host(iterator, addressCharacters)) {
            if (m_didSeeSyntaxViolation) [[unlikely]]
                serializeIPv6(address.value());
            if (!ipv6End.atEnd()) {
                advance(ipv6End);
                m_url.m_hostEnd = currentPosition(ipv6End);
                if (!ipv6End.atEnd() && *ipv6End == ':')
                    return parsePort(ipv6End) ? HostParsingResult::IPv6WithPort : HostParsingResult::InvalidHost;
                m_url.m_portLength = 0;
                return ipv6End.atEnd() ? HostParsingResult::IPv6WithoutPort : HostParsingResult::InvalidHost;
            }
            m_url.m_hostEnd = currentPosition(ipv6End);
            return HostParsingResult::IPv6WithoutPort;
        }
        return HostParsingResult::InvalidHost;
    }

    if (!m_urlIsSpecial) {
        for (; !iterator.atEnd(); ++iterator) {
            if (isTabOrNewline(*iterator)) [[unlikely]] {
                syntaxViolation(iterator);
                continue;
            }
            if (*iterator == ':')
                break;
            if (isForbiddenHostCodePoint(*iterator) && *iterator != '%') [[unlikely]]
                return HostParsingResult::InvalidHost;
            utf8PercentEncode<isInC0ControlEncodeSet>(iterator);
        }
        m_url.m_hostEnd = currentPosition(iterator);
        if (iterator.atEnd()) {
            m_url.m_portLength = 0;
            return HostParsingResult::NonSpecialHostWithoutPort;
        }
        return parsePort(iterator) ? HostParsingResult::NonSpecialHostWithPort : HostParsingResult::InvalidHost;
    }
    
    if (!m_hostHasPercentOrNonASCII) [[likely]] {
        auto hostIterator = iterator;
        auto* hostBegin = iterator.position();
        auto* end = hostBegin + iterator.remainingCodeUnits().size();
        auto* hostEnd = findHostCharacterOfInterest(hostBegin, end);
        bool hasTabOrNewline = false;
        uint16_t domainCharacterClasses = 0;
        for (; hostEnd != end; ++hostEnd) {
            auto character = *hostEnd;
            auto characterClass = scanClass(character);
            if (!(characterClass & (HostNotDomainCharacter | HostStop))) [[likely]] {
                domainCharacterClasses |= characterClass;
                continue;
            }
            if (character == ':')
                break;
            if (isTabOrNewline(character)) [[unlikely]]
                hasTabOrNewline = true;
            else {
                ASSERT(isForbiddenDomainCodePoint(character));
                return HostParsingResult::InvalidHost;
            }
        }
        bool hasUppercase = domainCharacterClasses & HostNotPlain;
        iterator = CodePointIterator<CharacterType>(std::span(hostEnd, end));

        // parseIPv4Host() can only return something other than NotIPv4, and dnsNameEndsInNumber() can only be true,
        // when the last label starts with a digit and is made of characters that can occur in a number.
        bool mayBeIPv4OrEndInANumber = hasTabOrNewline;
        if (!hasTabOrNewline) {
            auto* labelEnd = hostEnd;
            if (labelEnd != hostBegin && labelEnd[-1] == '.')
                --labelEnd;
            auto* label = labelEnd;
            while (label != hostBegin && (scanClass(label[-1]) & IPv4NumberCharacter))
                --label;
            mayBeIPv4OrEndInANumber = (label == hostBegin || label[-1] == '.') && label != labelEnd && isASCIIDigit(*label);
        }

        if (mayBeIPv4OrEndInANumber) [[unlikely]] {
            auto address = parseIPv4Host(hostIterator, std::span(hostBegin, hostEnd));
            if (address) {
                if (m_didSeeSyntaxViolation) [[unlikely]]
                    serializeIPv4(address.value());
                m_url.m_hostEnd = currentPosition(iterator);
                if (iterator.atEnd()) {
                    m_url.m_portLength = 0;
                    return HostParsingResult::IPv4WithoutPort;
                }
                return parsePort(iterator) ? HostParsingResult::IPv4WithPort : HostParsingResult::InvalidHost;
            }
            if (address.error() == IPv4ParsingError::Failure)
                return HostParsingResult::InvalidHost;
        }
        if (!hasUppercase && !hasTabOrNewline) [[likely]]
            appendToASCIIBuffer(std::span(hostBegin, hostEnd));
        else if (!hasTabOrNewline) {
            auto* firstUppercase = hostBegin;
            while (!isASCIIUpper(*firstUppercase))
                ++firstUppercase;
            appendToASCIIBuffer(std::span(hostBegin, firstUppercase));
            syntaxViolation(CodePointIterator(std::span(firstUppercase, hostEnd)));
            appendToASCIIBufferLowercased(std::span(firstUppercase, hostEnd));
        } else {
            for (; hostIterator != iterator; ++hostIterator) {
                if (isTabOrNewline(*hostIterator)) [[unlikely]] {
                    syntaxViolation(hostIterator);
                    continue;
                }
                if (isASCIIUpper(*hostIterator)) [[unlikely]]
                    syntaxViolation(hostIterator);
                appendToASCIIBuffer(toASCIILower(*hostIterator));
            }
        }
        m_url.m_hostEnd = currentPosition(iterator);
        if (mayBeIPv4OrEndInANumber) [[unlikely]] {
            auto hostStart = m_url.hostStart();
            if (dnsNameEndsInNumber(parsedDataView(hostStart, m_url.m_hostEnd - hostStart))) [[unlikely]]
                return HostParsingResult::InvalidHost;
        }
        if (!iterator.atEnd())
            return parsePort(iterator) ? HostParsingResult::DNSNameWithPort : HostParsingResult::InvalidHost;
        m_url.m_portLength = 0;
        return HostParsingResult::DNSNameWithoutPort;
    }
    
    const auto hostBegin = iterator;
    auto* hostCharactersBegin = iterator.position();
    auto* end = hostCharactersBegin + iterator.remainingCodeUnits().size();
    auto* hostEnd = hostCharactersBegin;
    bool hasNonASCII = false;
    bool hasPercent = false;
    bool hasTabOrNewline = false;
    for (; hostEnd != end; ++hostEnd) {
        auto character = *hostEnd;
        if (!(scanClass(character) & (HostNotPlain | HostPercentOrNonASCII))) [[likely]]
            continue;
        if (character == ':')
            break;
        if (character == '%')
            hasPercent = true;
        else if (isTabOrNewline(character)) [[unlikely]]
            hasTabOrNewline = true;
        else if (!isASCII(character))
            hasNonASCII = true;
    }
    iterator = CodePointIterator<CharacterType>(std::span(hostEnd, end));
    std::span<const CharacterType> host(hostCharactersBegin, hostEnd);
    if (hasTabOrNewline || hasNonASCII)
        syntaxViolation(hostBegin);

    std::optional<Latin1Buffer> asciiDomain;
    if (!hasPercent) [[likely]] {
        Vector<char16_t, 256> buffer;
        std::span<const char16_t> domain;
        bool needsCopy = true;
        if constexpr (std::is_same_v<CharacterType, char16_t>) {
            if (!hasTabOrNewline) [[likely]] {
                domain = host;
                needsCopy = false;
            }
        }
        if (needsCopy) {
            buffer.reserveInitialCapacity(host.size());
            for (auto character : host) {
                if (!isTabOrNewline(character)) [[likely]]
                    buffer.append(character);
            }
            domain = buffer.span();
        }
        if constexpr (std::is_same_v<CharacterType, char16_t>) {
            if (hasUnpairedSurrogate(domain)) [[unlikely]]
                return HostParsingResult::InvalidHost;
        }
        asciiDomain = domainToASCII(domain, hostBegin);
    } else if (!hasNonASCII && !hasTabOrNewline) [[likely]] {
        // Percent-decode, lowercase and validate in one pass; equivalent to percentDecode(), String::fromUTF8() and
        // domainToASCII() below as long as the decoded bytes are ASCII, which is checked.
        Latin1Buffer ascii;
        ascii.grow(host.size());
        size_t asciiLength = 0;
        bool didDecode = false;
        bool sawUppercase = false;
        bool sawNonASCIIByte = false;
        for (size_t i = 0; i < host.size(); ++i) {
            uint8_t byte = host[i];
            if (byte == '%' && i + 2 < host.size() && isASCIIHexDigit(host[i + 1]) && isASCIIHexDigit(host[i + 2])) {
                byte = toASCIIHexValue(host[i + 1], host[i + 2]);
                i += 2;
                didDecode = true;
                sawNonASCIIByte |= !isASCII(byte);
            }
            sawUppercase |= isASCIIUpper(byte);
            ascii[asciiLength++] = toASCIILower(byte);
        }
        ascii.shrink(asciiLength);
        if (!sawNonASCIIByte) [[likely]] {
            if (didDecode || sawUppercase)
                syntaxViolation(hostBegin);
            asciiDomain = WTF::move(ascii);
        } else {
            Latin1Buffer utf8Encoded;
            utf8Encoded.append(host);
            Latin1Buffer percentDecoded = percentDecode(utf8Encoded.span(), hostBegin);
            String domain = String::fromUTF8(percentDecoded.span());
            if (domain.isNull())
                return HostParsingResult::InvalidHost;
            syntaxViolation(hostBegin);
            asciiDomain = domainToASCII(domain, hostBegin);
        }
    } else {
        Latin1Buffer utf8Encoded;
        for (auto codePoints = CodePointIterator<CharacterType>(host); !codePoints.atEnd(); ++codePoints) {
            if (isTabOrNewline(*codePoints)) [[unlikely]]
                continue;
            std::array<uint8_t, U8_MAX_LENGTH> buffer;
            size_t offset = 0;
            UBool isError = false;
            U8_APPEND(buffer, offset, U8_MAX_LENGTH, *codePoints, isError);
            if (isError)
                return HostParsingResult::InvalidHost;
            utf8Encoded.append(std::span { buffer }.first(offset));
        }
        Latin1Buffer percentDecoded = percentDecode(utf8Encoded.span(), hostBegin);
        if (charactersAreAllASCII(percentDecoded.span()))
            asciiDomain = domainToASCII(percentDecoded.span(), hostBegin);
        else {
            String domain = String::fromUTF8(percentDecoded.span());
            if (domain.isNull())
                return HostParsingResult::InvalidHost;
            syntaxViolation(hostBegin);
            asciiDomain = domainToASCII(domain, hostBegin);
        }
    }
    if (!asciiDomain || hasForbiddenHostCodePoint(asciiDomain.value()))
        return HostParsingResult::InvalidHost;
    Latin1Buffer& asciiDomainValue = asciiDomain.value();

    bool mayBeIPv4OrEndInANumber;
    {
        auto* begin = asciiDomainValue.begin();
        auto* labelEnd = asciiDomainValue.end();
        if (labelEnd != begin && labelEnd[-1] == '.')
            --labelEnd;
        auto* label = labelEnd;
        while (label != begin && (scanClass(label[-1]) & IPv4NumberCharacter))
            --label;
        mayBeIPv4OrEndInANumber = (label == begin || label[-1] == '.') && label != labelEnd && isASCIIDigit(*label);
    }
    if (mayBeIPv4OrEndInANumber) [[unlikely]] {
        auto address = parseIPv4Host(hostBegin, std::span<const Latin1Character>(asciiDomainValue.span()));
        if (address) {
            serializeIPv4(address.value());
            m_url.m_hostEnd = currentPosition(iterator);
            if (iterator.atEnd()) {
                m_url.m_portLength = 0;
                return HostParsingResult::IPv4WithoutPort;
            }
            return parsePort(iterator) ? HostParsingResult::IPv4WithPort : HostParsingResult::InvalidHost;
        }
        if (address.error() == IPv4ParsingError::Failure)
            return HostParsingResult::InvalidHost;
    }

    appendToASCIIBuffer(asciiDomainValue.span());
    m_url.m_hostEnd = currentPosition(iterator);
    if (mayBeIPv4OrEndInANumber) [[unlikely]] {
        auto hostStart = m_url.hostStart();
        if (dnsNameEndsInNumber(parsedDataView(hostStart, m_url.m_hostEnd - hostStart))) [[unlikely]]
            return HostParsingResult::InvalidHost;
    }
    if (!iterator.atEnd())
        return parsePort(iterator) ? HostParsingResult::DNSNameWithPort : HostParsingResult::InvalidHost;

    m_url.m_portLength = 0;
    return HostParsingResult::DNSNameWithoutPort;
}

std::optional<String> URLParser::formURLDecode(StringView input)
{
    auto utf8 = input.utf8(StrictConversion);
    if (utf8.isNull())
        return std::nullopt;
    auto percentDecoded = percentDecode(byteCast<Latin1Character>(utf8.span()));
    return String::fromUTF8ReplacingInvalidSequences(percentDecoded.span());
}

// https://url.spec.whatwg.org/#concept-urlencoded-parser
auto URLParser::parseURLEncodedForm(StringView input) -> URLEncodedForm
{
    URLEncodedForm output;
    for (StringView bytes : input.split('&')) {
        if (auto nameAndValue = parseQueryNameAndValue(bytes))
            output.append(WTF::move(*nameAndValue));
    }
    return output;
}

std::optional<KeyValuePair<String, String>> URLParser::parseQueryNameAndValue(StringView bytes)
{
    auto equalIndex = bytes.find('=');
    if (equalIndex == notFound) {
        auto name = formURLDecode(makeStringByReplacingAll(bytes, '+', ' '));
        if (name)
            return { { WTF::move(*name), emptyString() } };
    } else {
        auto name = formURLDecode(makeStringByReplacingAll(bytes.left(equalIndex), '+', ' '));
        auto value = formURLDecode(makeStringByReplacingAll(bytes.substring(equalIndex + 1), '+', ' '));
        if (name && value)
            return { { WTF::move(*name), WTF::move(*value) } };
    }
    return std::nullopt;
}

static void serializeURLEncodedForm(const String& input, Vector<Latin1Character>& output)
{
    auto utf8 = input.utf8(StrictConversion);
    for (char byte : utf8.span()) {
        if (byte == 0x20)
            output.append(0x2B);
        else if (byte == 0x2A
            || byte == 0x2D
            || byte == 0x2E
            || (byte >= 0x30 && byte <= 0x39)
            || (byte >= 0x41 && byte <= 0x5A)
            || byte == 0x5F
            || (byte >= 0x61 && byte <= 0x7A)) // FIXME: Put these in the characterClassTable to avoid branches.
            output.append(byte);
        else
            percentEncodeByte(byte, output);
    }
}
    
String URLParser::serialize(const URLEncodedForm& tuples)
{
    if (tuples.isEmpty())
        return { };

    Vector<Latin1Character> output;
    for (auto& tuple : tuples) {
        if (!output.isEmpty())
            output.append('&');
        serializeURLEncodedForm(tuple.key, output);
        output.append('=');
        serializeURLEncodedForm(tuple.value, output);
    }
    return String::adopt(WTF::move(output));
}

const UIDNA& URLParser::internationalDomainNameTranscoder()
{
    static UIDNA* encoder = [] {
        UErrorCode error = U_ZERO_ERROR;
        auto* encoder = uidna_openUTS46(UIDNA_CHECK_BIDI | UIDNA_CHECK_CONTEXTJ | UIDNA_NONTRANSITIONAL_TO_UNICODE | UIDNA_NONTRANSITIONAL_TO_ASCII, &error);
        if (U_FAILURE(error)) [[unlikely]]
            CRASH_WITH_INFO(error);
        RELEASE_ASSERT(encoder);
        return encoder;
    }();
    return *encoder;
}

bool URLParser::allValuesEqual(const URL& a, const URL& b)
{
    URL_PARSER_LOG("%d %d %d %d %d %d %d %d %d %d %d %d %s\n%d %d %d %d %d %d %d %d %d %d %d %d %s",
        a.m_isValid,
        a.m_hasOpaquePath,
        a.m_protocolIsInHTTPFamily,
        a.m_schemeEnd,
        a.m_userStart,
        a.m_userEnd,
        a.m_passwordEnd,
        a.m_hostEnd,
        a.m_hostEnd + a.m_portLength,
        a.m_pathAfterLastSlash,
        a.m_pathEnd,
        a.m_queryEnd,
        a.m_string.utf8().data(),
        b.m_isValid,
        b.m_hasOpaquePath,
        b.m_protocolIsInHTTPFamily,
        b.m_schemeEnd,
        b.m_userStart,
        b.m_userEnd,
        b.m_passwordEnd,
        b.m_hostEnd,
        b.m_hostEnd + b.m_portLength,
        b.m_pathAfterLastSlash,
        b.m_pathEnd,
        b.m_queryEnd,
        b.m_string.utf8().data());

    return a.m_string == b.m_string
        && a.m_isValid == b.m_isValid
        && a.m_hasOpaquePath == b.m_hasOpaquePath
        && a.m_protocolIsInHTTPFamily == b.m_protocolIsInHTTPFamily
        && a.m_schemeEnd == b.m_schemeEnd
        && a.m_userStart == b.m_userStart
        && a.m_userEnd == b.m_userEnd
        && a.m_passwordEnd == b.m_passwordEnd
        && a.m_hostEnd == b.m_hostEnd
        && a.m_portLength == b.m_portLength
        && a.m_pathAfterLastSlash == b.m_pathAfterLastSlash
        && a.m_pathEnd == b.m_pathEnd
        && a.m_queryEnd == b.m_queryEnd;
}

bool URLParser::internalValuesConsistent(const URL& url)
{
    return url.m_schemeEnd <= url.m_userStart
        && url.m_userStart <= url.m_userEnd
        && url.m_userEnd <= url.m_passwordEnd
        && url.m_passwordEnd <= url.m_hostEnd
        && url.m_hostEnd + url.m_portLength <= url.m_pathAfterLastSlash
        && url.m_pathAfterLastSlash <= url.m_pathEnd
        && url.m_pathEnd <= url.m_queryEnd
        && url.m_queryEnd <= url.m_string.length();
}

} // namespace WTF
