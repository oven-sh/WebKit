/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "YarrRequiredText.h"

#include "YarrPattern.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <unicode/utf16.h>
#include <wtf/ASCIICType.h>
#include <wtf/SIMDHelpers.h>
#include <wtf/SetForScope.h>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Yarr {

namespace {

constexpr unsigned unbounded = RequiredText::unbounded;

unsigned sum(unsigned one, unsigned other)
{
    return one == unbounded || other == unbounded || one + other < one ? unbounded : one + other;
}

unsigned product(unsigned one, unsigned other)
{
    if (!one || !other)
        return 0;
    uint64_t result = static_cast<uint64_t>(one) * other;
    return one == unbounded || other == unbounded || result >= unbounded ? unbounded : static_cast<unsigned>(result);
}

// In code units.
struct Extent {
    Extent operator+(Extent other) const { return { sum(least, other.least) == unbounded ? least : least + other.least, sum(greatest, other.greatest) }; }
    Extent times(unsigned fewest, unsigned most) const
    {
        unsigned leastProduct = product(least, fewest);
        return { leastProduct == unbounded ? least : leastProduct, product(greatest, most) };
    }

    unsigned least { 0 };
    unsigned greatest { 0 };
};

// A place in a match that takes one code unit.
struct Place {
    bool isEmpty() const { return !asciiMembers[0] && !asciiMembers[1] && !matchesAllNonASCII; }
    unsigned count() const { return std::popcount(asciiMembers[0]) + std::popcount(asciiMembers[1]); }
    unsigned first() const { return asciiMembers[0] ? std::countr_zero(asciiMembers[0]) : 64 + std::countr_zero(asciiMembers[1]); }
    bool isOneCharacter() const { return !matchesAllNonASCII && count() == 1; }
    bool isLetterInBothCases() const
    {
        if (matchesAllNonASCII || asciiMembers[0] || count() != 2)
            return false;
        unsigned upper = first();
        return isASCIIUpper(upper) && asciiMembers[1] >> (toASCIILower(upper) - 64) & 1;
    }
    bool isExact() const { return isOneCharacter() || isLetterInBothCases(); }

    void add(const Place& other)
    {
        asciiMembers[0] |= other.asciiMembers[0];
        asciiMembers[1] |= other.asciiMembers[1];
        matchesAllNonASCII |= other.matchesAllNonASCII;
    }

    std::array<uint64_t, 2> asciiMembers { };
    bool matchesAllNonASCII { false };
};

// Places side by side. The distance is that of the first from the start of what was walked.
struct Run {
    Vector<Place, 8> places;
    Extent distance;
    // What a match can hold between its start and the first place. Without a value: anything.
    std::optional<Place> inFront;
};

struct Runs {
    void close() { lastIsOpen = false; }
    void append(const Place& place, Extent distance, const std::optional<Place>& inFront = std::nullopt)
    {
        if (!lastIsOpen || list.last().places.size() >= RequiredText::maxLength) {
            list.append(Run { { }, distance, inFront });
            lastIsOpen = true;
        }
        list.last().places.append(place);
    }

    Vector<Run, 4> list;
    bool lastIsOpen { false };
};

// Of a thousand code units of program text, logs and prose, about how many are this one.
constexpr unsigned commonnessOf(unsigned unit)
{
    if (isASCIILower(unit)) {
        for (char common : { 'e', 't', 'a', 'o', 'i', 'n', 's', 'r', 'l' }) {
            if (unit == static_cast<unsigned>(common))
                return 48;
        }
        return 16;
    }
    if (isASCIIDigit(unit))
        return 12;
    if (isASCIIUpper(unit))
        return 5;
    switch (unit) {
    case ' ':
        return 120;
    case '\n':
    case '"':
        return 20;
    case ',':
    case '.':
    case '-':
    case '_':
    case '/':
    case ':':
        return 12;
    case '=':
    case '(':
    case ')':
    case ';':
    case '\'':
    case '\t':
    case '{':
    case '}':
    case '[':
    case ']':
    case '<':
    case '>':
    case '\\':
        return 6;
    default:
        return 2;
    }
}
constexpr unsigned commonnessOfAllNonASCII = 24;

unsigned commonnessOf(const Place& place)
{
    unsigned commonness = place.matchesAllNonASCII ? commonnessOfAllNonASCII : 0;
    for (unsigned word = 0; word < 2; ++word) {
        for (uint64_t rest = place.asciiMembers[word]; rest; rest &= rest - 1)
            commonness += commonnessOf(word * 64 + std::countr_zero(rest));
    }
    return commonness;
}

unsigned rarityOf(const Place& place)
{
    unsigned commonness = commonnessOf(place);
    return commonness >= 1024 ? 0 : std::bit_width(1024 / std::max(commonness, 1u)) - 1;
}

// A walk back from the end of the subject stops at what no match holds: at one unit in ten or so, or it is as long as the search that it saves.
constexpr unsigned leastCommonnessOfWhatEndsWalk = 100;

constexpr unsigned maxNestingDepth = 24;

bool beginsAtInputStartAlone(const PatternAlternative& alternative)
{
    return !alternative.m_terms.isEmpty() && alternative.m_terms[0].type == PatternTerm::Type::AssertionBOL && !alternative.m_terms[0].multiline();
}

// Of the body's alternatives: no match of it begins anywhere but at the first place that a search tries.
bool isTriedAtFirstPlaceAlone(const PatternAlternative& alternative)
{
    return alternative.m_onceThrough || beginsAtInputStartAlone(alternative);
}

class Finder {
public:
    explicit Finder(const YarrPattern& pattern)
        : m_pattern(pattern)
    {
    }

    bool hasFailed() const { return m_hasFailed; }

    // Of the body: the alternatives that are tried at one place after another.
    Extent walk(const PatternDisjunction& disjunction, Runs& runs, unsigned depth)
    {
        Vector<const PatternAlternative*, 8> alternatives;
        for (auto& alternative : disjunction.m_alternatives) {
            if (depth || !isTriedAtFirstPlaceAlone(*alternative))
                alternatives.append(alternative.get());
        }
        if (depth > maxNestingDepth || alternatives.isEmpty())
            return { 0, unbounded };
        if (alternatives.size() == 1) {
            SetForScope inFront(m_inFront, depth ? std::nullopt : std::optional<Place> { Place { } });
            Extent end = walk(*alternatives[0], { }, runs, depth);
            runs.close();
            return end;
        }
        SetForScope inFront(m_inFront, std::nullopt);

        // A match goes through one of them: it holds what one place of each can hold.
        Extent whole { unbounded, 0 };
        Place anyOf;
        Extent distance { unbounded, 0 };
        bool everyAlternativeHasPlace = true;
        for (auto* alternative : alternatives) {
            Runs own;
            Extent end = walk(*alternative, { }, own, depth);
            whole = { std::min(whole.least, end.least), std::max(whole.greatest, end.greatest) };
            const Place* rarest = nullptr;
            Extent rarestDistance;
            unsigned greatestRarity = 0;
            for (auto& run : own.list) {
                for (unsigned at = 0; at < run.places.size(); ++at) {
                    unsigned rarity = rarityOf(run.places[at]);
                    if (rarest && rarity <= greatestRarity)
                        continue;
                    rarest = &run.places[at];
                    rarestDistance = run.distance + Extent { at, at };
                    greatestRarity = rarity;
                }
            }
            if (!rarest) {
                everyAlternativeHasPlace = false;
                continue;
            }
            anyOf.add(*rarest);
            distance = { std::min(distance.least, rarestDistance.least), std::max(distance.greatest, rarestDistance.greatest) };
        }
        runs.close();
        if (everyAlternativeHasPlace) {
            runs.append(anyOf, distance);
            runs.close();
        }
        return whole;
    }

    // Whether every match of the alternatives ends at the end of the subject. Adds the units that they can take.
    bool collectUnitsOfMatchesAtEnd(const PatternDisjunction& body, Place& units)
    {
        for (auto& alternative : body.m_alternatives) {
            if (isTriedAtFirstPlaceAlone(*alternative))
                continue;
            auto& terms = alternative->m_terms;
            if (terms.isEmpty() || terms.last().type != PatternTerm::Type::AssertionEOL || terms.last().multiline() || !collectUnits(*alternative, units, 0))
                return false;
        }
        return true;
    }

private:
    bool collectUnits(const PatternAlternative& alternative, Place& units, unsigned depth)
    {
        for (auto& term : alternative.m_terms) {
            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
            case PatternTerm::Type::AssertionEOL:
            case PatternTerm::Type::AssertionWordBoundary:
            case PatternTerm::Type::ParentheticalAssertion:
                break;
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass: {
                auto atom = placeOf(term);
                if (!atom)
                    return false;
                units.add(atom->first);
                break;
            }
            case PatternTerm::Type::ParenthesesSubpattern:
                if (term.matchDirection() != Forward || depth >= maxNestingDepth)
                    return false;
                for (auto& nested : term.parentheses.disjunction->m_alternatives) {
                    if (!collectUnits(*nested, units, depth + 1))
                        return false;
                }
                break;
            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
            case PatternTerm::Type::DotStarEnclosure:
                return false;
            }
        }
        return true;
    }

    // What one repetition of an atom takes, and the place that its first unit fills.
    std::optional<std::pair<Place, Extent>> placeOf(const PatternTerm& term) const
    {
        if (term.m_matchedSet == CharacterSetID::Unknown || term.matchDirection() != Forward)
            return std::nullopt;
        Place place;
        bool hasOneUnit = false;
        bool hasTwoUnits = false;
        for (auto [begin, end] : m_pattern.m_firstFollowSets.set(term.m_matchedSet).ranges) {
            for (char32_t unit = begin; unit <= end && unit < 128; ++unit)
                place.asciiMembers[unit >> 6] |= 1ull << (unit & 63);
            place.matchesAllNonASCII |= end >= 128;
            hasOneUnit |= begin <= 0xffff;
            hasTwoUnits |= end > 0xffff;
        }
        if (!m_pattern.eitherUnicode())
            hasTwoUnits = false;
        return std::pair { place, Extent { hasOneUnit || !hasTwoUnits ? 1u : 2u, hasTwoUnits ? 2u : 1u } };
    }

    // position: how far from the start of what is walked the alternative begins. Returns where it ends.
    Extent walk(const PatternAlternative& alternative, Extent position, Runs& runs, unsigned depth)
    {
        auto passUnknown = [&] {
            runs.close();
            position.greatest = unbounded;
            m_inFront = std::nullopt;
        };
        auto pass = [&](const Place& place) {
            if (m_inFront)
                m_inFront->add(place);
        };
        for (auto& term : alternative.m_terms) {
            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
            case PatternTerm::Type::AssertionEOL:
            case PatternTerm::Type::AssertionWordBoundary:
            case PatternTerm::Type::ParentheticalAssertion:
                break;
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass: {
                auto atom = placeOf(term);
                if (!atom) {
                    passUnknown();
                    break;
                }
                auto [place, extent] = *atom;
                unsigned most = term.quantityMaxCount == quantifyInfinite ? unbounded : term.quantityMaxCount.value();
                if (term.quantityType != QuantifierType::FixedCount) {
                    runs.close();
                    position = position + extent.times(0, most);
                    pass(place);
                    break;
                }
                if (extent.least != 1 || extent.greatest != 1) {
                    runs.close();
                    if (most) {
                        runs.append(place, position, m_inFront);
                        runs.close();
                    }
                } else {
                    for (unsigned i = 0; i < std::min(most, RequiredText::maxLength); ++i) {
                        runs.append(place, position + Extent { i, i }, m_inFront);
                        pass(place);
                    }
                    if (most > RequiredText::maxLength)
                        runs.close();
                }
                position = position + extent.times(most, most);
                pass(place);
                break;
            }
            case PatternTerm::Type::ParenthesesSubpattern: {
                if (term.matchDirection() != Forward) {
                    passUnknown();
                    break;
                }
                auto& nested = *term.parentheses.disjunction;
                unsigned fewest = term.quantityMinCount.value();
                unsigned most = term.quantityMaxCount == quantifyInfinite ? unbounded : term.quantityMaxCount.value();
                if (fewest == 1 && most == 1 && nested.m_alternatives.size() == 1 && depth < maxNestingDepth) {
                    position = walk(*nested.m_alternatives[0], position, runs, depth + 1);
                    break;
                }
                // The first turn begins here. Where later ones do is not looked into.
                runs.close();
                Runs ofFirstTurn;
                Extent turn = walk(nested, ofFirstTurn, depth + 1);
                Place unitsOfGroup;
                bool knowsUnitsOfGroup = m_inFront && std::ranges::all_of(nested.m_alternatives, [&](auto& inner) {
                    return collectUnits(*inner, unitsOfGroup, depth + 1);
                });
                if (knowsUnitsOfGroup)
                    pass(unitsOfGroup);
                else
                    m_inFront = std::nullopt;
                if (fewest) {
                    for (auto& run : ofFirstTurn.list) {
                        run.distance = position + run.distance;
                        runs.list.append(WTF::move(run));
                    }
                }
                position = position + turn.times(fewest, most);
                break;
            }
            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
                passUnknown();
                break;
            case PatternTerm::Type::DotStarEnclosure:
                m_hasFailed = true;
                break;
            }
        }
        return position;
    }

    const YarrPattern& m_pattern;
    // What the match can hold between its start and the term that is walked. Without a value: anything.
    std::optional<Place> m_inFront;
    bool m_hasFailed { false };
};

struct Candidate {
    const Run* run;
    unsigned begin;
    unsigned end;
    unsigned rarity;
};

Place unitsInFrontOf(const Candidate& candidate, unsigned place)
{
    Place inFront = *candidate.run->inFront;
    for (unsigned i = 0; i < place; ++i)
        inFront.add(candidate.run->places[i]);
    return inFront;
}

// Whether a walk back from the text, over what can stand between the start of a match and it, is short: it stops at one unit in ten or so.
bool endsWalkSoon(const Candidate& candidate)
{
    if (!candidate.run->inFront)
        return false;
    Place inFront = unitsInFrontOf(candidate, candidate.end);
    return commonnessOf(Place { { ~inFront.asciiMembers[0], ~inFront.asciiMembers[1] }, !inFront.matchesAllNonASCII }) >= leastCommonnessOfWhatEndsWalk;
}

RequiredText makeRequiredText(const Candidate& candidate)
{
    auto places = candidate.run->places.span().subspan(candidate.begin, candidate.end - candidate.begin);
    unsigned rarest = 0;
    for (unsigned i = 1; i < places.size(); ++i) {
        if (rarityOf(places[i]) > rarityOf(places[rarest]))
            rarest = i;
    }
    Extent distance = candidate.run->distance + Extent { candidate.begin + rarest, candidate.begin + rarest };
    RequiredText result;
    result.asciiMembers = places[rarest].asciiMembers;
    result.matchesAllNonASCII = places[rarest].matchesAllNonASCII;
    result.leastDistance = distance.least;
    result.greatestDistance = distance.greatest;
    // At a fixed distance the place of the unit says where the match begins.
    if (endsWalkSoon(candidate) && distance.least != distance.greatest) {
        Place inFront = unitsInFrontOf(candidate, candidate.begin + rarest);
        result.asciiMembersInFront = inFront.asciiMembers;
        result.allNonASCIICanStandInFront = inFront.matchesAllNonASCII;
        result.knowsUnitsInFront = true;
    }
    if (places.size() > 1) {
        result.indexInText = rarest;
        for (auto& place : places) {
            bool isInBothCases = place.isLetterInBothCases();
            result.text.append(static_cast<Latin1Character>(isInBothCases ? toASCIILower(place.first()) : place.first()));
            result.caseMask.append(isInBothCases ? 0x20 : 0);
        }
    }
    return result;
}

template<typename CharType>
ALWAYS_INLINE size_t findMember(const RequiredText& required, std::span<const CharType> subject, size_t from)
{
    auto rest = subject.subspan(from);
    auto [low, high] = required.asciiMembers;
    if (!required.matchesAllNonASCII && std::popcount(low) + std::popcount(high) == 1) {
        size_t found = WTF::find(rest, static_cast<CharType>(low ? std::countr_zero(low) : 64 + std::countr_zero(high)));
        return found == notFound ? subject.size() : from + found;
    }

    alignas(16) static constexpr uint8_t bitOfHighBits[16] { 1, 2, 4, 8, 16, 32, 64, 128, 0, 0, 0, 0, 0, 0, 0, 0 };
    auto rowsVector = SIMD::load(required.rowsOfLowBits.data());
    auto bitsVector = SIMD::load(bitOfHighBits);
    auto leastNonASCII = SIMD::splat8(required.matchesAllNonASCII ? 0x80 : 0xff);
    auto membersOf = [&](simde_uint8x16_t units) {
        auto isInRow = simde_vtstq_u8(simde_vqtbl1q_u8(rowsVector, simde_vandq_u8(units, SIMD::splat8(0xf))), simde_vqtbl1q_u8(bitsVector, simde_vshrq_n_u8(units, 4)));
        return required.matchesAllNonASCII ? simde_vorrq_u8(isInRow, SIMD::greaterThanOrEqual(units, leastNonASCII)) : isInRow;
    };
    auto vectorMatch = [&](auto units) -> std::optional<uint8_t> {
        if constexpr (sizeof(CharType) == 1)
            return SIMD::findFirstNonZeroIndex(membersOf(units));
        else {
            auto narrowed = simde_vqmovn_u16(units);
            return SIMD::findFirstNonZeroIndex(membersOf(simde_vcombine_u8(narrowed, narrowed)));
        }
    };
    auto scalarMatch = [&](CharType unit) {
        return required.isMember(unit);
    };
    return SIMD::find(rest, vectorMatch, scalarMatch) - subject.data();
}

template<typename CharType>
ALWAYS_INLINE size_t findText(const RequiredText& required, std::span<const CharType> subject, size_t from)
{
    size_t length = subject.size();
    if (required.text.isEmpty())
        return findMember(required, subject, from);
    size_t before = required.indexInText;
    size_t behind = required.text.size() - before;
    for (;; ++from) {
        if (from >= length)
            return length;
        from = findMember(required, subject, from);
        if (length - from < behind)
            return length;
        if (from < before)
            continue;
        const CharType* candidate = subject.data() + (from - before);
        bool isSame = true;
        for (unsigned i = 0; i < required.text.size() && isSame; ++i)
            isSame = (candidate[i] | required.caseMask[i]) == required.text[i];
        if (isSame)
            return from;
    }
}

template<typename CharType>
ALWAYS_INLINE unsigned nextStartOfRepeatedAlternatives(const RequiredTexts& texts, std::span<const CharType> subject, unsigned start, RequiredTextPlaces& places)
{
    size_t length = subject.size();
    for (unsigned i = 0; i < texts.size(); ++i) {
        const RequiredText& required = texts[i];
        if (required.isAllThatMatchesAtEndHold) {
            if (!places[i]) {
                size_t first = length;
                while (first > start && required.isMember(subject[first - 1]))
                    --first;
                places[i] = first + 1;
            }
            start = std::max(start, places[i] - 1);
            continue;
        }
        // Each turn looks for the text farther behind.
        for (;;) {
            size_t earliest = static_cast<size_t>(start) + required.leastDistance;
            bool isNewPlace = earliest >= places[i];
            if (isNewPlace)
                places[i] = (earliest < length ? findText(required, subject, earliest) : length) + 1;
            size_t found = places[i] - 1;
            if (found >= length)
                return length + 1;
            if (found - start > required.greatestDistance) {
                start = found - required.greatestDistance;
                if (sizeof(CharType) > 1 && required.decodesSurrogatePairs && U16_IS_TRAIL(subject[start]) && U16_IS_LEAD(subject[start - 1]))
                    --start;
            }
            if (!isNewPlace || !required.knowsUnitsInFront)
                break;
            size_t first = found;
            while (first > start && required.canStandInFront(subject[first - 1]))
                --first;
            start = first;
            if (start + required.leastDistance <= found)
                break;
        }
    }
    return start;
}

template<typename CharType>
ALWAYS_INLINE unsigned nextStart(const RequiredTexts& texts, std::span<const CharType> subject, unsigned start, RequiredTextPlaces& places)
{
    bool isFirstAttempt = !places[0];
    if (!isFirstAttempt || !texts[0].isFirstAttemptAlwaysMade)
        return nextStartOfRepeatedAlternatives(texts, subject, start, places);
    // The search waits until that attempt has failed: every later one begins behind place 0, and so looks for the text.
    if (texts[0].isAllThatMatchesAtEndHold)
        nextStartOfRepeatedAlternatives(texts, subject, start, places);
    else
        places[0] = 1;
    return start;
}

RequiredTexts find(const YarrPattern& pattern)
{

    Finder finder(pattern);
    Place unitsAtEnd;
    if (!pattern.hasEndAnchoredFixedSize() && finder.collectUnitsOfMatchesAtEnd(*pattern.m_body, unitsAtEnd)
        && commonnessOf(Place { { ~unitsAtEnd.asciiMembers[0], ~unitsAtEnd.asciiMembers[1] }, false }) >= leastCommonnessOfWhatEndsWalk) {
        RequiredText result;
        result.asciiMembers = unitsAtEnd.asciiMembers;
        result.matchesAllNonASCII = unitsAtEnd.matchesAllNonASCII;
        result.isAllThatMatchesAtEndHold = true;
        return { WTF::move(result) };
    }

    Runs runs;
    finder.walk(*pattern.m_body, runs, 0);
    if (finder.hasFailed())
        return { };

    std::optional<Candidate> rarest;
    std::optional<Candidate> nearest;
    auto consider = [&](Candidate candidate) {
        if (candidate.rarity < leastRarityWorthSearchingFor)
            return;
        if (!rarest || candidate.rarity > rarest->rarity)
            rarest = candidate;
        if ((candidate.run->distance.greatest != unbounded || endsWalkSoon(candidate)) && (!nearest || candidate.rarity > nearest->rarity))
            nearest = candidate;
    };
    for (auto& run : runs.list) {
        for (unsigned begin = 0; begin < run.places.size();) {
            if (run.places[begin].isEmpty())
                return { };
            unsigned end = begin + 1;
            unsigned rarity = rarityOf(run.places[begin]);
            if (run.places[begin].isExact()) {
                for (; end < run.places.size() && run.places[end].isExact(); ++end)
                    rarity += rarityOf(run.places[end]);
            }
            consider({ &run, begin, end, rarity });
            begin = end;
        }
    }

    RequiredTexts result;
    if (rarest)
        result.append(makeRequiredText(*rarest));
    if (nearest && rarest->run->distance.greatest == unbounded && !endsWalkSoon(*rarest))
        result.append(makeRequiredText(*nearest));
    return result;
}

} // namespace

RequiredTexts findRequiredTexts(const YarrPattern& pattern)
{
    if (pattern.m_analysis != PatternAnalysis::Performed || pattern.sticky())
        return { };
    // What is tried at one place alone fails there sooner than a search ends.
    auto& alternatives = pattern.m_body->m_alternatives;
    auto isTriedOnce = [](auto& alternative) {
        return isTriedAtFirstPlaceAlone(*alternative);
    };
    if (std::ranges::all_of(alternatives, isTriedOnce))
        return { };
    RequiredTexts result = find(pattern);
    for (auto& required : result) {
        for (unsigned word = 0; word < 2; ++word) {
            for (uint64_t rest = required.asciiMembers[word]; rest; rest &= rest - 1) {
                unsigned unit = word * 64 + std::countr_zero(rest);
                required.rowsOfLowBits[unit & 15] |= 1 << (unit >> 4);
            }
        }
        required.decodesSurrogatePairs = pattern.eitherUnicode();
        required.isFirstAttemptAlwaysMade = std::ranges::any_of(alternatives, isTriedOnce);
        required.isSilentOnSomeRepeatedAlternative = std::ranges::any_of(alternatives, [](auto& alternative) {
            return !alternative->m_onceThrough && beginsAtInputStartAlone(*alternative);
        });
    }
    return result;
}

unsigned rarityOfUnits(const std::array<uint64_t, 2>& asciiMembers, bool matchesAllNonASCII)
{
    return rarityOf(Place { asciiMembers, matchesAllNonASCII });
}

unsigned nextStartWithRequiredTexts(const RequiredTexts& texts, std::span<const Latin1Character> subject, unsigned start, RequiredTextPlaces& places)
{
    return nextStart(texts, subject, start, places);
}

unsigned nextStartWithRequiredTexts(const RequiredTexts& texts, std::span<const char16_t> subject, unsigned start, RequiredTextPlaces& places)
{
    return nextStart(texts, subject, start, places);
}

} } // namespace JSC::Yarr

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
