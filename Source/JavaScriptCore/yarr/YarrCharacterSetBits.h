/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "YarrPattern.h"
#include <array>
#include <span>
#include <type_traits>
#include <wtf/FastMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/Latin1Character.h>

namespace JSC { namespace Yarr {

struct CharacterSetBits {
    std::array<uint64_t, 4> bits { };
    uint32_t firstWideRange { 0 };
    uint32_t wideRangeCount : 31 { 0 };
    uint32_t invertsWideRanges : 1 { 0 };

    friend bool operator==(const CharacterSetBits&, const CharacterSetBits&) = default;
};
static_assert(sizeof(CharacterSetBits) == 40);
static_assert(std::has_unique_object_representations_v<CharacterSetBits>);
static_assert(std::is_trivially_copyable_v<CharacterRange> && sizeof(CharacterRange) == 2 * sizeof(char32_t));

inline void addLatin1Range(std::array<uint64_t, 4>& bits, unsigned begin, unsigned end)
{
    ASSERT(begin <= end && end <= 0xff);
    for (unsigned word = begin >> 6; word <= end >> 6; ++word) {
        unsigned low = word == begin >> 6 ? begin & 63 : 0;
        unsigned high = word == end >> 6 ? end & 63 : 63;
        bits[word] |= (~0ull >> (63 - high)) & (~0ull << low);
    }
}

JS_EXPORT_PRIVATE CharacterSetBits buildCharacterSetBits(const CharacterClass&, bool isInverted, Vector<CharacterRange>& wideRanges);
JS_EXPORT_PRIVATE CharacterSetBits buildCharacterSetBits(char32_t first, char32_t second, Vector<CharacterRange>& wideRanges);

JS_EXPORT_PRIVATE bool wideRangesContain(std::span<const CharacterRange> wideRanges, const CharacterSetBits&, char32_t);

ALWAYS_INLINE bool contains(const CharacterSetBits& set, std::span<const CharacterRange>, Latin1Character character)
{
    return set.bits[character >> 6] & (1ull << (character & 63));
}

ALWAYS_INLINE bool contains(const CharacterSetBits& set, std::span<const CharacterRange> wideRanges, char16_t character)
{
    if (character > 0xff) [[unlikely]]
        return wideRangesContain(wideRanges, set, character);
    return set.bits[character >> 6] & (1ull << (character & 63));
}

struct CharacterSetWithRanges {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CharacterSetWithRanges);

    CharacterSetBits bits;
    Vector<CharacterRange> wideRanges;
};

class CharacterSetView {
public:
    CharacterSetView(const CharacterSetBits& bits, std::span<const CharacterRange> wideRanges)
        : m_bits(&bits)
        , m_wideRanges(wideRanges)
    {
    }

    explicit CharacterSetView(const CharacterSetWithRanges& set)
        : CharacterSetView(set.bits, set.wideRanges.span())
    {
    }

    ALWAYS_INLINE bool contains(Latin1Character character) const { return Yarr::contains(*m_bits, m_wideRanges, character); }
    ALWAYS_INLINE bool contains(char16_t character) const { return Yarr::contains(*m_bits, m_wideRanges, character); }

    const CharacterSetBits& bits() const { return *m_bits; }
    std::span<const CharacterRange> wideRanges() const { return m_wideRanges; }

private:
    const CharacterSetBits* m_bits;
    std::span<const CharacterRange> m_wideRanges;
};

} } // namespace JSC::Yarr
