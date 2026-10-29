/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <array>
#include <limits>
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/Latin1Character.h>

namespace JSC { namespace Yarr {

struct YarrPattern;

// Something that every match holds, read off the syntax tree. A subject without it has no match, and no match begins farther in front of it than the pattern reaches.
struct RequiredText {
    static constexpr unsigned unbounded = std::numeric_limits<unsigned>::max();
    static constexpr unsigned maxLength = 32;

    bool isMember(unsigned unit) const { return unit < 128 ? asciiMembers[unit >> 6] >> (unit & 63) & 1 : matchesAllNonASCII; }
    bool canStandInFront(unsigned unit) const { return unit < 128 ? asciiMembersInFront[unit >> 6] >> (unit & 63) & 1 : allNonASCIICanStandInFront; }

    // Every match holds one of these code units. With matchesAllNonASCII every unit from 128 up counts as one.
    std::array<uint64_t, 2> asciiMembers { };
    bool matchesAllNonASCII { false };
    // The same for sixteen units at once. A row for the low four bits of a unit: bit i is set where the unit whose high four bits are i is one of them.
    alignas(16) std::array<uint8_t, 16> rowsOfLowBits { };
    // How many code units lie between the start of a match and that unit.
    unsigned leastDistance { 0 };
    unsigned greatestDistance { unbounded };
    // With knowsUnitsInFront: between the start of a match and that unit lie none but these, so no match begins in front of the last other unit in front of it.
    std::array<uint64_t, 2> asciiMembersInFront { };
    bool allNonASCIICanStandInFront { false };
    bool knowsUnitsInFront { false };
    // That unit is text[indexInText], and the rest of the text stands around it. A letter that matches in both cases is in lower case and has 0x20 in caseMask. Empty: the unit is all that is known.
    Vector<Latin1Character, 16> text;
    Vector<Latin1Character, 16> caseMask;
    // The first eight units of the text and of the mask, for a text of no more than eight.
    uint64_t firstUnitsOfText { 0 };
    uint64_t firstUnitsOfCaseMask { 0 };
    unsigned indexInText { 0 };
    // Of a pattern in unicode mode: no attempt begins between the halves of a surrogate pair.
    bool decodesSurrogatePairs { false };
    // Every match ends where the subject ends and holds nothing but the units above: none begins in front of the last unit of the subject that is no such unit. The distances and the text say nothing.
    bool isAllThatMatchesAtEndHold { false };
    // The pattern has alternatives that are tried at the first place alone, of which all this says nothing.
    bool isFirstAttemptAlwaysMade { false };
    // One of those is none the less tried at every place.
    bool isSilentOnSomeRepeatedAlternative { false };
};

// The rarest first. A second one is nearer to the start of a match: its distance has a bound, the first one's has none.
using RequiredTexts = Vector<RequiredText>;
inline constexpr unsigned maxRequiredTexts = 2;

// Of the body's alternatives that are tried at one place after another. None for a sticky pattern, whose one attempt costs less than a search.
RequiredTexts findRequiredTexts(const YarrPattern&);

// How many times the share of the places in a subject that hold one of these units halves: 10 is one place in a thousand. By what program text, logs and prose hold.
unsigned rarityOfUnits(const std::array<uint64_t, 2>& asciiMembers, bool matchesAllNonASCII);
// A search that stops at one place in thirty-two or fewer saves more than it costs.
inline constexpr unsigned leastRarityWorthSearchingFor = 5;

// For one search through one subject: one more than the place where each text was last found. 0: it has not been looked for.
using RequiredTextPlaces = std::array<unsigned, maxRequiredTexts>;

// Whether the places where the texts were last found say that a match can begin at `start`. Where they do not, nextStartWithRequiredTexts() answers.
ALWAYS_INLINE bool canBeginByKnownPlaces(const RequiredTexts& texts, unsigned length, unsigned start, const RequiredTextPlaces& places)
{
    for (unsigned i = 0; i < texts.size(); ++i) {
        const RequiredText& required = texts[i];
        if (required.isAllThatMatchesAtEndHold) {
            if (start + 1 < places[i] || !places[i])
                return false;
            continue;
        }
        uint64_t earliest = static_cast<uint64_t>(start) + required.leastDistance;
        if (earliest >= places[i] || places[i] > length || places[i] - 1 - start > required.greatestDistance)
            return false;
    }
    return true;
}

// The first place from `start` on where a match can begin. Past the end: none.
unsigned nextStartWithRequiredTexts(const RequiredTexts&, std::span<const Latin1Character> subject, unsigned start, RequiredTextPlaces&);
unsigned nextStartWithRequiredTexts(const RequiredTexts&, std::span<const char16_t> subject, unsigned start, RequiredTextPlaces&);

} } // namespace JSC::Yarr
