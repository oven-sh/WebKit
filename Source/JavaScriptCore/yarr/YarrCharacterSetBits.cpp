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

#include "config.h"
#include "YarrCharacterSetBits.h"

#include <algorithm>

namespace JSC { namespace Yarr {

static void addRange(CharacterSetBits& set, Vector<CharacterRange, 8>& wide, char32_t begin, char32_t end)
{
    if (begin <= 0xff)
        addLatin1Range(set.bits, begin, std::min<char32_t>(end, 0xff));
    if (end > 0xff)
        wide.append(CharacterRange(std::max<char32_t>(begin, 0x100), end));
}

static CharacterSetBits appendWideRanges(CharacterSetBits set, Vector<CharacterRange, 8>& wide, Vector<CharacterRange>& wideRanges)
{
    set.firstWideRange = wideRanges.size();
    std::ranges::sort(wide, { }, &CharacterRange::begin);
    for (auto& range : wide) {
        if (wideRanges.size() > set.firstWideRange && range.begin <= wideRanges.last().end + 1)
            wideRanges.last().end = std::max(wideRanges.last().end, range.end);
        else
            wideRanges.append(range);
    }
    size_t count = wideRanges.size() - set.firstWideRange;
    set.wideRangeCount = count;
    RELEASE_ASSERT(set.wideRangeCount == count);
    return set;
}

CharacterSetBits buildCharacterSetBits(const CharacterClass& characterClass, bool isInverted, Vector<CharacterRange>& wideRanges)
{
    ASSERT(!characterClass.hasStrings());

    CharacterSetBits set;
    Vector<CharacterRange, 8> wide;
    set.invertsWideRanges = isInverted;
    if (characterClass.m_anyCharacter) {
        set.bits.fill(~0ull);
        set.invertsWideRanges = !isInverted;
    } else {
        for (char32_t character : characterClass.m_matches8)
            addRange(set, wide, character, character);
        for (auto& range : characterClass.m_ranges8)
            addRange(set, wide, range.begin, range.end);
        for (char32_t character : characterClass.m_matches32)
            addRange(set, wide, character, character);
        for (auto& range : characterClass.m_ranges32)
            addRange(set, wide, range.begin, range.end);
    }
    if (isInverted) {
        for (auto& word : set.bits)
            word = ~word;
    }
    return appendWideRanges(set, wide, wideRanges);
}

CharacterSetBits buildCharacterSetBits(char32_t first, char32_t second, Vector<CharacterRange>& wideRanges)
{
    CharacterSetBits set;
    Vector<CharacterRange, 8> wide;
    addRange(set, wide, first, first);
    addRange(set, wide, second, second);
    return appendWideRanges(set, wide, wideRanges);
}

bool wideRangesContain(std::span<const CharacterRange> wideRanges, const CharacterSetBits& set, char32_t character)
{
    ASSERT(character > 0xff);
    auto ranges = wideRanges.subspan(set.firstWideRange, set.wideRangeCount);
    auto found = std::upper_bound(ranges.begin(), ranges.end(), character, [](char32_t value, const CharacterRange& range) {
        return value < range.begin;
    });
    bool isInSlice = found != ranges.begin() && character <= (found - 1)->end;
    return isInSlice != set.invertsWideRanges;
}

} } // namespace JSC::Yarr
