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
#include "YarrFirstFollowSets.h"

#include "YarrCanonicalize.h"
#include "YarrPattern.h"
#include <algorithm>
#include <wtf/ASCIICType.h>
#include <wtf/Hasher.h>

namespace JSC { namespace Yarr {

using CharacterRanges = Vector<std::pair<char32_t, char32_t>, 4>;
using Continuation = CharacterSetWalker::Continuation;
using Outcome = CharacterSetWalker::Outcome;

static constexpr Continuation endOfGroup { nullptr, 0, nullptr, nullptr };

static Outcome buildCharacterSpans(const YarrPattern& pattern, const PatternTerm& term, CharacterRanges& spans)
{
    constexpr char32_t lastCodePoint = 0x10ffff;
    if (term.type == PatternTerm::Type::PatternCharacter) {
        char32_t ch = term.patternCharacter;
        CanonicalMode mode = pattern.eitherUnicode() ? CanonicalMode::Unicode : CanonicalMode::UCS2;
        if (!term.ignoreCase() || isCanonicallyUnique(ch, mode)) {
            spans.append({ ch, ch });
            return Outcome::Done;
        }
        if (mode != CanonicalMode::UCS2 || !isASCIIAlpha(ch))
            return Outcome::GaveUp;
        spans.append({ toASCIIUpper(ch), toASCIIUpper(ch) });
        spans.append({ toASCIILower(ch), toASCIILower(ch) });
        return Outcome::Done;
    }

    const CharacterClass& characterClass = *term.characterClass;
    if (characterClass.hasStrings())
        return Outcome::GaveUp;
    CharacterRanges members;
    if (characterClass.m_anyCharacter)
        members.append({ 0, lastCodePoint });
    else if (!characterClass.hasSingleCharacters()) {
        if (characterClass.m_table)
            return Outcome::GaveUp;
    } else {
        for (char32_t ch : characterClass.m_matches8)
            members.append({ ch, ch });
        for (char32_t ch : characterClass.m_matches32)
            members.append({ ch, ch });
        for (auto range : characterClass.m_ranges8)
            members.append({ range.begin, range.end });
        for (auto range : characterClass.m_ranges32)
            members.append({ range.begin, range.end });
        std::ranges::sort(members);
    }

    char32_t nextExcluded = 0;
    bool coversLastCodePoint = false;
    for (auto [begin, end] : members) {
        if (term.invert()) {
            if (begin > nextExcluded)
                spans.append({ nextExcluded, begin - 1 });
        } else if (!spans.isEmpty() && begin <= spans.last().second + 1)
            spans.last().second = std::max(spans.last().second, end);
        else
            spans.append({ begin, end });
        if (end >= lastCodePoint) {
            coversLastCodePoint = true;
            break;
        }
        nextExcluded = std::max<char32_t>(nextExcluded, end + 1);
    }
    if (term.invert() && !coversLastCodePoint)
        spans.append({ nextExcluded, lastCodePoint });
    return Outcome::Done;
}

static bool spansOverlap(const CharacterRanges& one, const CharacterRanges& other)
{
    const CharacterRanges& shorter = one.size() <= other.size() ? one : other;
    const CharacterRanges& longer = one.size() <= other.size() ? other : one;
    for (auto [begin, end] : shorter) {
        auto found = std::lower_bound(longer.begin(), longer.end(), begin, [](const auto& span, char32_t value) {
            return span.second < value;
        });
        if (found != longer.end() && found->first <= end)
            return true;
    }
    return false;
}

static unsigned hashTableKey(const CharacterSet& characterSet)
{
    // 0 and UINT_MAX are the empty and deleted keys of the table.
    return (computeHash(characterSet.ranges, characterSet.includesEndOfInput) >> 1) + 1;
}

const CharacterSet& FirstFollowSets::set(CharacterSetID id) const
{
    RELEASE_ASSERT(id != CharacterSetID::Unknown);
    return m_sets[std::to_underlying(id)];
}

bool FirstFollowSets::areDisjoint(CharacterSetID one, CharacterSetID other) const
{
    if (one == CharacterSetID::Unknown || other == CharacterSetID::Unknown || one == other)
        return false;
    const CharacterSet& oneSet = set(one);
    const CharacterSet& otherSet = set(other);
    if (oneSet.includesEndOfInput && otherSet.includesEndOfInput)
        return false;
    return !spansOverlap(oneSet.ranges, otherSet.ranges);
}

WordCharacterKnowledge FirstFollowSets::wordCharacterKnowledge(CharacterSetID id, WordClassKind kind) const
{
    CharacterSetID wordCharacters = m_wordCharacters[std::to_underlying(kind)];
    CharacterSetID notWordCharacters = m_notWordCharacters[std::to_underlying(kind)];
    if (id == CharacterSetID::Unknown || wordCharacters == CharacterSetID::Unknown || notWordCharacters == CharacterSetID::Unknown)
        return WordCharacterKnowledge::Unknown;
    bool hasNoWordCharacter = areDisjoint(id, wordCharacters);
    bool hasOnlyWordCharacters = areDisjoint(id, notWordCharacters) && !set(id).includesEndOfInput;
    if (hasOnlyWordCharacters && !hasNoWordCharacter)
        return WordCharacterKnowledge::WordCharacter;
    if (hasNoWordCharacter && !hasOnlyWordCharacters)
        return WordCharacterKnowledge::NotWordCharacter;
    return WordCharacterKnowledge::Unknown;
}

CharacterSetID FirstFollowSets::intern(CharacterSet&& characterSet)
{
    ASSERT(std::ranges::all_of(characterSet.ranges, [](const auto& range) { return range.first <= range.second; }));
    ASSERT(std::ranges::adjacent_find(characterSet.ranges, [](const auto& range, const auto& next) { return range.second + 1 >= next.first; }) == characterSet.ranges.end());

    auto& ids = m_idsByHash.add(hashTableKey(characterSet), Vector<CharacterSetID, 1>()).iterator->value;
    for (CharacterSetID id : ids) {
        if (set(id) == characterSet)
            return id;
    }
    auto id = static_cast<CharacterSetID>(m_sets.size());
    characterSet.ranges.shrinkToFit();
    m_sets.append(WTF::move(characterSet));
    ids.append(id);
    return id;
}

WordClassKind wordClassKind(const YarrPattern& pattern, const PatternTerm& term)
{
    return pattern.eitherUnicode() && term.ignoreCase() ? WordClassKind::UnicodeIgnoreCase : WordClassKind::Plain;
}

struct CharacterSetWalker::Walk {
    Vector<CharacterSetID, 8> members;
    bool includesEndOfInput { false };
    bool reachedEndOfGroup { false };
    bool reachedEndOfPattern { false };
    unsigned budget { maxTermsVisitedPerSet };
    std::optional<WordClassKind> failingWordBoundary;
};

CharacterSetWalker::CharacterSetWalker(YarrPattern& pattern, StackCheck& stackCheck)
    : m_pattern(pattern)
    , m_stackCheck(stackCheck)
{
}

FirstFollowSets& CharacterSetWalker::sets()
{
    return m_pattern.m_firstFollowSets;
}

Outcome CharacterSetWalker::chargeVisit(Walk& walk)
{
    if (!walk.budget) {
        sets().countLimitHit();
        return Outcome::GaveUp;
    }
    --walk.budget;
    return Outcome::Done;
}

Outcome CharacterSetWalker::collect(const PatternAlternative& alternative, unsigned firstTermIndex, const Continuation* continuation, bool passedAssertion, Walk& walk)
{
    if (!m_stackCheck.isSafeToRecurse()) [[unlikely]] {
        sets().countStackExhaustion();
        return Outcome::GaveUp;
    }
    if (alternative.matchDirection() != Forward)
        return Outcome::GaveUp;

    auto& terms = alternative.m_terms;
    if (firstTermIndex >= terms.size() && chargeVisit(walk) == Outcome::GaveUp)
        return Outcome::GaveUp;
    for (unsigned i = firstTermIndex; i < terms.size(); ++i) {
        const PatternTerm& term = terms[i];
        if (chargeVisit(walk) == Outcome::GaveUp)
            return Outcome::GaveUp;
        switch (term.type) {
        case PatternTerm::Type::AssertionWordBoundary:
            if (!term.invert() && walk.failingWordBoundary == wordClassKind(m_pattern, term))
                return Outcome::Done;
            [[fallthrough]];
        case PatternTerm::Type::AssertionBOL:
            passedAssertion = true;
            continue;
        case PatternTerm::Type::ParentheticalAssertion:
            if (!term.invert() && term.matchDirection() == Forward) {
                if (collectLookahead(term, walk) == Outcome::Done)
                    return Outcome::Done;
                if (!walk.budget)
                    return Outcome::GaveUp;
            }
            passedAssertion = true;
            continue;
        case PatternTerm::Type::AssertionEOL:
            walk.includesEndOfInput = true;
            if (term.multiline()) {
                if (sets().m_lineTerminators == CharacterSetID::Unknown)
                    return Outcome::GaveUp;
                walk.members.append(sets().m_lineTerminators);
            }
            return Outcome::Done;
        case PatternTerm::Type::PatternCharacter:
        case PatternTerm::Type::CharacterClass:
            if (term.matchDirection() != Forward)
                return Outcome::GaveUp;
            if (!term.quantityMaxCount)
                continue;
            if (term.m_matchedSet == CharacterSetID::Unknown)
                return Outcome::GaveUp;
            walk.members.append(term.m_matchedSet);
            if (term.quantityMinCount)
                return Outcome::Done;
            continue;
        case PatternTerm::Type::ParenthesesSubpattern: {
            if (term.matchDirection() != Forward)
                return Outcome::GaveUp;
            if (!term.quantityMaxCount)
                continue;
            Continuation afterGroup { &alternative, i + 1, term.quantityMaxCount > 1 ? &term : nullptr, continuation };
            for (auto& nested : term.parentheses.disjunction->m_alternatives) {
                if (collect(*nested, 0, &afterGroup, passedAssertion, walk) == Outcome::GaveUp)
                    return Outcome::GaveUp;
            }
            if (term.quantityMinCount)
                return Outcome::Done;
            continue;
        }
        case PatternTerm::Type::NumberedForwardReference:
        case PatternTerm::Type::NamedForwardReference:
            continue;
        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
        case PatternTerm::Type::DotStarEnclosure:
            return Outcome::GaveUp;
        }
    }

    if (!continuation) {
        walk.reachedEndOfPattern = true;
        return passedAssertion ? Outcome::GaveUp : Outcome::Done;
    }
    if (!continuation->alternative) {
        walk.reachedEndOfGroup = true;
        return Outcome::Done;
    }
    if (continuation->repeatedGroup) {
        for (auto& nested : continuation->repeatedGroup->parentheses.disjunction->m_alternatives) {
            if (collect(*nested, 0, &endOfGroup, passedAssertion, walk) == Outcome::GaveUp)
                return Outcome::GaveUp;
        }
    }
    return collect(*continuation->alternative, continuation->nextTermIndex, continuation->outer, passedAssertion, walk);
}

Outcome CharacterSetWalker::collectLookahead(const PatternTerm& lookahead, Walk& walk)
{
    Walk body;
    body.budget = walk.budget;
    body.failingWordBoundary = walk.failingWordBoundary;
    Outcome outcome = Outcome::Done;
    for (auto& nested : lookahead.parentheses.disjunction->m_alternatives) {
        outcome = collect(*nested, 0, &endOfGroup, false, body);
        if (outcome == Outcome::GaveUp)
            break;
    }
    walk.budget = body.budget;
    if (outcome == Outcome::GaveUp || body.reachedEndOfGroup)
        return Outcome::GaveUp;
    walk.members.appendVector(body.members);
    walk.includesEndOfInput |= body.includesEndOfInput;
    return Outcome::Done;
}

CharacterSetWalker::NextCharacters CharacterSetWalker::internMembers(Walk& walk)
{
    bool reachesEnd = walk.reachedEndOfGroup || walk.reachedEndOfPattern;
    std::ranges::sort(walk.members);
    removeRepeatedElements(walk.members);
    if (walk.members.size() == 1 && !walk.includesEndOfInput)
        return { walk.members[0], reachesEnd };

    size_t rangeCount = 0;
    for (CharacterSetID member : walk.members)
        rangeCount += sets().set(member).ranges.size();
    if (rangeCount > maxRangesPerSet) {
        sets().countLimitHit();
        return { };
    }

    CharacterSet result;
    result.includesEndOfInput = walk.includesEndOfInput;
    CharacterRanges& ranges = result.ranges;
    ranges.reserveInitialCapacity(rangeCount);
    for (CharacterSetID member : walk.members)
        ranges.appendVector(sets().set(member).ranges);
    std::ranges::sort(ranges);
    size_t mergedCount = 0;
    for (auto [begin, end] : ranges) {
        if (mergedCount && begin <= ranges[mergedCount - 1].second + 1)
            ranges[mergedCount - 1].second = std::max(ranges[mergedCount - 1].second, end);
        else
            ranges[mergedCount++] = std::pair { begin, end };
    }
    ranges.shrink(mergedCount);
    return { sets().intern(WTF::move(result)), reachesEnd };
}

CharacterSetWalker::NextCharacters CharacterSetWalker::firstSet(const PatternAlternative& alternative)
{
    Walk walk;
    if (collect(alternative, 0, &endOfGroup, false, walk) == Outcome::GaveUp)
        return { };
    return internMembers(walk);
}

CharacterSetWalker::NextCharacters CharacterSetWalker::followSet(const PatternAlternative& alternative, unsigned nextTermIndex, const Continuation* continuation, std::optional<WordClassKind> failingWordBoundary)
{
    Walk walk;
    walk.failingWordBoundary = failingWordBoundary;
    if (collect(alternative, nextTermIndex, continuation, false, walk) == Outcome::GaveUp)
        return { };
    return internMembers(walk);
}

static WordCharacterKnowledge sameOrUnknown(WordCharacterKnowledge one, WordCharacterKnowledge other)
{
    return one == other ? one : WordCharacterKnowledge::Unknown;
}

WordCharacterKnowledge CharacterSetWalker::groupLastCharacterKnowledge(const PatternTerm& group, WordClassKind kind, unsigned& budget)
{
    if (group.matchDirection() != Forward)
        return WordCharacterKnowledge::Unknown;
    WordCharacterKnowledge result = WordCharacterKnowledge::Unknown;
    for (auto& alternative : group.parentheses.disjunction->m_alternatives) {
        auto edge = lastCharacterKnowledge(*alternative, alternative->m_terms.size(), kind, budget);
        if (edge == WordCharacterKnowledge::Unknown || (result != WordCharacterKnowledge::Unknown && result != edge))
            return WordCharacterKnowledge::Unknown;
        result = edge;
    }
    return result;
}

WordCharacterKnowledge CharacterSetWalker::lastCharacterKnowledge(const PatternAlternative& alternative, unsigned endTermIndex, WordClassKind kind, unsigned& budget)
{
    if (!endTermIndex)
        return WordCharacterKnowledge::Unknown;
    if (!m_stackCheck.isSafeToRecurse()) [[unlikely]] {
        sets().countStackExhaustion();
        return WordCharacterKnowledge::Unknown;
    }
    if (!budget) [[unlikely]] {
        sets().countLimitHit();
        return WordCharacterKnowledge::Unknown;
    }
    --budget;
    const PatternTerm& term = alternative.m_terms[endTermIndex - 1];
    WordCharacterKnowledge own = WordCharacterKnowledge::Unknown;
    switch (term.type) {
    case PatternTerm::Type::AssertionBOL:
    case PatternTerm::Type::AssertionEOL:
    case PatternTerm::Type::AssertionWordBoundary:
    case PatternTerm::Type::ParentheticalAssertion:
    case PatternTerm::Type::NumberedForwardReference:
    case PatternTerm::Type::NamedForwardReference:
        return lastCharacterKnowledge(alternative, endTermIndex - 1, kind, budget);
    case PatternTerm::Type::PatternCharacter:
    case PatternTerm::Type::CharacterClass:
        if (term.matchDirection() != Forward)
            return WordCharacterKnowledge::Unknown;
        own = sets().wordCharacterKnowledge(term.m_matchedSet, kind);
        if (term.quantityType == QuantifierType::FixedCount && term.quantityMaxCount)
            return own;
        break;
    case PatternTerm::Type::ParenthesesSubpattern:
        own = groupLastCharacterKnowledge(term, kind, budget);
        if (term.quantityMinCount)
            return own;
        break;
    case PatternTerm::Type::NumberedBackReference:
    case PatternTerm::Type::NamedBackReference:
    case PatternTerm::Type::DotStarEnclosure:
        return WordCharacterKnowledge::Unknown;
    }
    if (own == WordCharacterKnowledge::Unknown)
        return WordCharacterKnowledge::Unknown;
    return sameOrUnknown(own, lastCharacterKnowledge(alternative, endTermIndex - 1, kind, budget));
}

WordCharacterKnowledge CharacterSetWalker::lastCharacterKnowledge(const PatternAlternative& alternative, unsigned endTermIndex, WordClassKind kind)
{
    unsigned budget = maxTermsVisitedBeforeWordBoundary;
    return lastCharacterKnowledge(alternative, endTermIndex, kind, budget);
}

class PatternAnnotator {
public:
    PatternAnnotator(YarrPattern& pattern, StackCheck& stackCheck)
        : m_pattern(pattern)
        , m_stackCheck(stackCheck)
        , m_walker(pattern, stackCheck)
    {
    }

    void annotate(PatternAlternative&, const Continuation*);

private:
    static CharacterSetID bounded(CharacterSetWalker::NextCharacters next)
    {
        return next.reachesEnd ? CharacterSetID::Unknown : next.set;
    }

    YarrPattern& m_pattern;
    StackCheck& m_stackCheck;
    CharacterSetWalker m_walker;
};

void PatternAnnotator::annotate(PatternAlternative& alternative, const Continuation* continuation)
{
    FirstFollowSets& sets = m_pattern.m_firstFollowSets;
    if (!m_stackCheck.isSafeToRecurse()) [[unlikely]] {
        sets.countStackExhaustion();
        return;
    }
    if (alternative.matchDirection() != Forward)
        return;

    alternative.m_firstSet = bounded(m_walker.firstSet(alternative));
    auto& terms = alternative.m_terms;
    for (unsigned i = 0; i < terms.size(); ++i) {
        PatternTerm& term = terms[i];
        auto followSet = [&] {
            return bounded(m_walker.followSet(alternative, i + 1, continuation));
        };
        switch (term.type) {
        case PatternTerm::Type::PatternCharacter:
        case PatternTerm::Type::CharacterClass:
            if (term.matchDirection() == Forward && term.quantityType != QuantifierType::FixedCount)
                term.m_followSet = followSet();
            break;
        case PatternTerm::Type::ParenthesesSubpattern: {
            if (term.matchDirection() != Forward)
                break;
            Continuation afterGroup { &alternative, i + 1, term.quantityMaxCount > 1 ? &term : nullptr, continuation };
            for (auto& nested : term.parentheses.disjunction->m_alternatives)
                annotate(*nested, &afterGroup);
            break;
        }
        case PatternTerm::Type::ParentheticalAssertion:
            for (auto& nested : term.parentheses.disjunction->m_alternatives)
                annotate(*nested, nullptr);
            break;
        case PatternTerm::Type::AssertionWordBoundary: {
            WordClassKind kind = wordClassKind(m_pattern, term);
            term.m_nextCharacterKnowledge = sets.wordCharacterKnowledge(followSet(), kind);
            term.m_previousCharacterKnowledge = m_walker.lastCharacterKnowledge(alternative, i, kind);
            break;
        }
        case PatternTerm::Type::AssertionBOL:
        case PatternTerm::Type::AssertionEOL:
        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
        case PatternTerm::Type::NumberedForwardReference:
        case PatternTerm::Type::NamedForwardReference:
        case PatternTerm::Type::DotStarEnclosure:
            break;
        }
    }
}

void computeFirstFollowSets(YarrPattern& pattern, StackCheck& stackCheck)
{
    FirstFollowSets& sets = pattern.m_firstFollowSets;
    sets = { };
    sets.m_sets.append(CharacterSet());

    HashMap<uint64_t, CharacterSetID> atomSets;
    auto termSet = [&](const PatternTerm& term) {
        uint64_t key = term.type == PatternTerm::Type::PatternCharacter
            ? 1ull << 63 | static_cast<uint64_t>(term.patternCharacter) << 1 | term.ignoreCase()
            : static_cast<uint64_t>(std::bit_cast<uintptr_t>(term.characterClass)) << 1 | term.invert();
        auto result = atomSets.add(key, CharacterSetID::Unknown);
        if (result.isNewEntry) {
            CharacterSet characterSet;
            if (buildCharacterSpans(pattern, term, characterSet.ranges) == Outcome::Done)
                result.iterator->value = sets.intern(WTF::move(characterSet));
        }
        return result.iterator->value;
    };
    auto atomSet = [&](CharacterClass* characterClass, bool invert) {
        return termSet(PatternTerm(characterClass, invert, pattern.m_flags));
    };

    for (auto& disjunction : pattern.m_disjunctions) {
        for (auto& alternative : disjunction->m_alternatives) {
            for (auto& term : alternative->m_terms) {
                if (term.type == PatternTerm::Type::PatternCharacter || term.type == PatternTerm::Type::CharacterClass)
                    term.m_matchedSet = termSet(term);
            }
        }
    }

    sets.m_lineTerminators = atomSet(sharedCharacterClass(SharedCharacterClass::Newline), false);
    auto setWordCharacters = [&](WordClassKind kind, SharedCharacterClass wordClass) {
        sets.m_wordCharacters[std::to_underlying(kind)] = atomSet(sharedCharacterClass(wordClass), false);
        sets.m_notWordCharacters[std::to_underlying(kind)] = atomSet(sharedCharacterClass(wordClass), true);
    };
    setWordCharacters(WordClassKind::Plain, SharedCharacterClass::WordChar);
    setWordCharacters(WordClassKind::UnicodeIgnoreCase, SharedCharacterClass::WordUnicodeIgnoreCaseChar);

    PatternAnnotator annotator(pattern, stackCheck);
    for (auto& alternative : pattern.m_body->m_alternatives)
        annotator.annotate(*alternative, nullptr);
}

} } // namespace JSC::Yarr
