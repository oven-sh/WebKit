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
#include "YarrBacktrackingAnalysis.h"

#include "YarrFirstFollowSets.h"
#include "YarrPattern.h"
#include <algorithm>
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace Yarr {

class BacktrackingAnalysis {
public:
    BacktrackingAnalysis(YarrPattern& pattern, StackCheck& stackCheck)
        : m_pattern(pattern)
        , m_stackCheck(stackCheck)
        , m_walker(pattern, stackCheck)
    {
    }

    void run()
    {
        markLookaheadTails();
        for (auto& alternative : m_pattern.m_body->m_alternatives)
            markPossessiveTerms(*alternative, nullptr);
        if (m_pattern.m_containsBackreferences)
            findRepeatedLookaheads(m_pattern.m_body, nullptr);
    }

private:
    friend void sortAlternativesLongestFirst(PatternDisjunction&);

    using Continuation = CharacterSetWalker::Continuation;
    using NextCharacters = CharacterSetWalker::NextCharacters;

    FirstFollowSets& sets() { return m_pattern.m_firstFollowSets; }

    static void markAssertionLastTerm(PatternAlternative* alternative)
    {
        for (;;) {
            if (alternative->m_terms.isEmpty())
                return;
            auto& last = alternative->m_terms.last();
            if (last.matchDirection() != Forward)
                return;
            if (last.type == PatternTerm::Type::PatternCharacter || last.type == PatternTerm::Type::CharacterClass) {
                last.m_isNeverBacktrackedInto = last.quantityType != QuantifierType::FixedCount;
                return;
            }
            if (last.type != PatternTerm::Type::ParenthesesSubpattern || !isMatchedExactlyOnce(last) || last.parentheses.disjunction->m_alternatives.size() != 1)
                return;
            alternative = last.parentheses.disjunction->m_alternatives[0].get();
        }
    }

    void markLookaheadTails()
    {
        for (auto& disjunction : m_pattern.m_disjunctions) {
            for (auto& alternative : disjunction->m_alternatives) {
                for (auto& term : alternative->m_terms) {
                    if (term.type != PatternTerm::Type::ParentheticalAssertion || term.matchDirection() != Forward)
                        continue;
                    for (auto& inner : term.parentheses.disjunction->m_alternatives)
                        markAssertionLastTerm(inner.get());
                }
            }
        }
    }

    struct PatternPosition {
        const PatternAlternative* alternative;
        size_t precedingTermCount;
        const PatternPosition* groupPosition;
    };

    static bool isMatchedExactlyOnce(const PatternTerm& term)
    {
        return term.quantityType == QuantifierType::FixedCount && term.quantityMinCount == 1 && term.quantityMaxCount == 1;
    }

    static bool isCapturingLookahead(const PatternTerm& term, unsigned subpatternId)
    {
        if (term.type != PatternTerm::Type::ParentheticalAssertion || term.invert() || term.matchDirection() != Forward || !isMatchedExactlyOnce(term) || term.parentheses.disjunction->m_alternatives.size() != 1)
            return false;
        auto& inner = term.parentheses.disjunction->m_alternatives[0]->m_terms;
        return inner.size() == 1 && inner[0].type == PatternTerm::Type::ParenthesesSubpattern && inner[0].capture() && inner[0].parentheses.subpatternId == subpatternId && isMatchedExactlyOnce(inner[0]);
    }

    const PatternTerm* findRepeatedLookahead(PatternPosition position, unsigned subpatternId, unsigned groupsLeft)
    {
        for (;;) {
            if (!position.precedingTermCount) {
                if (!position.groupPosition)
                    return nullptr;
                position = *position.groupPosition;
                continue;
            }
            auto& previous = position.alternative->m_terms[position.precedingTermCount - 1];
            --position.precedingTermCount;
            switch (previous.type) {
            case PatternTerm::Type::AssertionBOL:
            case PatternTerm::Type::AssertionEOL:
            case PatternTerm::Type::AssertionWordBoundary:
                continue;
            case PatternTerm::Type::ParentheticalAssertion:
                if (isCapturingLookahead(previous, subpatternId))
                    return &previous;
                if (previous.containsAnyCaptures() && previous.parentheses.subpatternId <= subpatternId && subpatternId <= previous.parentheses.lastSubpatternId)
                    return nullptr;
                continue;
            case PatternTerm::Type::ParenthesesSubpattern: {
                if (!isMatchedExactlyOnce(previous) || previous.matchDirection() != Forward || previous.parentheses.disjunction->m_alternatives.size() != 1)
                    return nullptr;
                if (!groupsLeft) {
                    sets().countLimitHit();
                    return nullptr;
                }
                auto& inner = *previous.parentheses.disjunction->m_alternatives[0];
                return findRepeatedLookahead({ &inner, inner.m_terms.size(), &position }, subpatternId, groupsLeft - 1);
            }
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass:
            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
            case PatternTerm::Type::DotStarEnclosure:
                return nullptr;
            }
        }
    }

    void findRepeatedLookaheads(PatternDisjunction* disjunction, const PatternPosition* groupPosition)
    {
        if (!m_stackCheck.isSafeToRecurse()) [[unlikely]] {
            sets().countStackExhaustion();
            return;
        }

        for (auto& alternative : disjunction->m_alternatives) {
            auto& terms = alternative->m_terms;
            for (size_t i = 0; i < terms.size(); ++i) {
                auto& term = terms[i];
                PatternPosition position { alternative.get(), i, groupPosition };
                switch (term.type) {
                case PatternTerm::Type::ParenthesesSubpattern:
                case PatternTerm::Type::ParentheticalAssertion:
                    findRepeatedLookaheads(term.parentheses.disjunction, isMatchedExactlyOnce(term) && term.matchDirection() == Forward && alternative->matchDirection() == Forward ? &position : nullptr);
                    break;
                case PatternTerm::Type::NumberedBackReference:
                case PatternTerm::Type::NamedBackReference: {
                    bool mayBeKnown = isMatchedExactlyOnce(term) && term.matchDirection() == Forward && alternative->matchDirection() == Forward && !m_pattern.m_numDuplicateNamedCaptureGroups;
                    term.m_repeatedLookahead = mayBeKnown ? findRepeatedLookahead(position, term.backReferenceSubpatternId, maxGroupsCrossedToRepeatedLookahead) : nullptr;
                    break;
                }
                case PatternTerm::Type::AssertionBOL:
                case PatternTerm::Type::AssertionEOL:
                case PatternTerm::Type::AssertionWordBoundary:
                case PatternTerm::Type::PatternCharacter:
                case PatternTerm::Type::CharacterClass:
                case PatternTerm::Type::NumberedForwardReference:
                case PatternTerm::Type::NamedForwardReference:
                case PatternTerm::Type::DotStarEnclosure:
                    break;
                }
            }
        }
    }

    static bool isFixedCharacterTerm(const PatternTerm& term)
    {
        return (term.type == PatternTerm::Type::PatternCharacter || term.type == PatternTerm::Type::CharacterClass) && term.quantityType == QuantifierType::FixedCount && term.quantityMaxCount && term.matchDirection() == Forward;
    }

    std::optional<WordClassKind> wordBoundaryExcludedBetween(const PatternTerm& previous, const PatternTerm& term)
    {
        if (!isFixedCharacterTerm(previous))
            return std::nullopt;
        WordClassKind kind = wordClassKind(m_pattern, term);
        WordCharacterKnowledge knowledge = sets().wordCharacterKnowledge(previous.m_matchedSet, kind);
        if (knowledge == WordCharacterKnowledge::Unknown || knowledge != sets().wordCharacterKnowledge(term.m_matchedSet, kind))
            return std::nullopt;
        return kind;
    }

    bool fixedPrefixesDiffer(const PatternAlternative& one, const PatternAlternative& other)
    {
        auto isAssertion = [](const PatternTerm& term) {
            return term.type == PatternTerm::Type::AssertionBOL || term.type == PatternTerm::Type::AssertionEOL || term.type == PatternTerm::Type::AssertionWordBoundary;
        };
        unsigned index = 0;
        unsigned otherIndex = 0;
        unsigned count = 0;
        unsigned otherCount = 0;
        for (unsigned position = 0; position < maxFixedPrefixCompared; ++position) {
            while (index < one.m_terms.size() && isAssertion(one.m_terms[index]))
                ++index;
            while (otherIndex < other.m_terms.size() && isAssertion(other.m_terms[otherIndex]))
                ++otherIndex;
            if (index == one.m_terms.size() || otherIndex == other.m_terms.size())
                return false;
            const PatternTerm& term = one.m_terms[index];
            const PatternTerm& otherTerm = other.m_terms[otherIndex];
            if (!isFixedCharacterTerm(term) || !isFixedCharacterTerm(otherTerm))
                return false;
            if (term.m_matchedSet == CharacterSetID::Unknown || otherTerm.m_matchedSet == CharacterSetID::Unknown)
                return false;
            if (sets().areDisjoint(term.m_matchedSet, otherTerm.m_matchedSet))
                return true;
            if (term.quantityMaxCount == ++count) {
                ++index;
                count = 0;
            }
            if (otherTerm.quantityMaxCount == ++otherCount) {
                ++otherIndex;
                otherCount = 0;
            }
        }
        sets().countLimitHit();
        return false;
    }

    static std::optional<unsigned> alternativeFixedSize(const PatternAlternative& alternative)
    {
        unsigned size = 0;
        for (auto& term : alternative.m_terms) {
            if (!isFixedCharacterTerm(term) || term.quantityMaxCount > maxFixedPrefixCompared)
                return std::nullopt;
            size += term.quantityMaxCount.value();
        }
        return size;
    }

    void countFixedSizeLimitHit(const PatternAlternative& alternative)
    {
        if (std::ranges::all_of(alternative.m_terms, isFixedCharacterTerm))
            sets().countLimitHit();
    }

    static const PatternTerm* fixedCharacterTermAt(const PatternAlternative& alternative, unsigned position)
    {
        for (auto& term : alternative.m_terms) {
            if (!isFixedCharacterTerm(term))
                return nullptr;
            if (position < term.quantityMaxCount.value())
                return &term;
            position -= term.quantityMaxCount.value();
        }
        return nullptr;
    }

    struct GroupFollowSets {
        std::optional<NextCharacters>& slot(std::optional<WordClassKind> failingWordBoundary)
        {
            if (!failingWordBoundary)
                return unconditional;
            switch (*failingWordBoundary) {
            case WordClassKind::Plain:
                return plain;
            case WordClassKind::UnicodeIgnoreCase:
                return unicodeIgnoreCase;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }

        std::optional<NextCharacters> unconditional;
        std::optional<NextCharacters> plain;
        std::optional<NextCharacters> unicodeIgnoreCase;
    };

    AlternativeDeterminism alternativeDeterminism(const PatternDisjunction& disjunction, const Continuation& afterGroup, const PatternTerm* termBeforeGroup)
    {
        auto& alternatives = disjunction.m_alternatives;
        if (alternatives.size() > maxAlternativesForDeterminism) {
            sets().countLimitHit();
            return AlternativeDeterminism::Unknown;
        }
        Vector<CharacterSetID, 8> firstCharacters;
        Vector<std::optional<unsigned>, 8> fixedSizes;
        for (auto& nested : alternatives) {
            firstCharacters.append(nested->m_firstSet);
            fixedSizes.append(alternativeFixedSize(*nested));
        }

        GroupFollowSets followSets;
        auto continuationRejects = [&](const PatternTerm& next, const PatternTerm* previous) {
            if (next.m_matchedSet == CharacterSetID::Unknown)
                return false;
            auto assumption = previous ? wordBoundaryExcludedBetween(*previous, next) : std::nullopt;
            auto& following = followSets.slot(assumption);
            if (!following) {
                const PatternAlternative& anyAlternative = *alternatives[0];
                following = m_walker.followSet(anyAlternative, anyAlternative.m_terms.size(), &afterGroup, assumption);
            }
            return !following->reachesEnd && sets().areDisjoint(next.m_matchedSet, following->set);
        };

        bool needsLongestFirst = false;
        auto pairNeverNeedsRetry = [&](unsigned one, unsigned other) {
            unsigned earlier = std::min(one, other);
            unsigned later = std::max(one, other);
            if (fixedPrefixesDiffer(*alternatives[earlier], *alternatives[later]))
                return true;
            if (!fixedSizes[later]) {
                countFixedSizeLimitHit(*alternatives[later]);
                return false;
            }
            if (fixedSizes[earlier] && *fixedSizes[earlier] == *fixedSizes[later])
                return true;
            bool earlierIsShorter = fixedSizes[earlier] && *fixedSizes[earlier] < *fixedSizes[later];
            unsigned shorter = earlierIsShorter ? earlier : later;
            unsigned longer = earlierIsShorter ? later : earlier;
            unsigned size = *fixedSizes[shorter];
            const PatternTerm* next = fixedCharacterTermAt(*alternatives[longer], size);
            if (!next)
                return false;
            const PatternTerm* previous = size ? fixedCharacterTermAt(*alternatives[shorter], size - 1) : termBeforeGroup;
            if (!continuationRejects(*next, previous))
                return false;
            needsLongestFirst |= earlierIsShorter;
            return true;
        };

        if (alternatives.size() <= maxAlternativesComparedPairwise) {
            for (unsigned i = 1; i < alternatives.size(); ++i) {
                for (unsigned j = 0; j < i; ++j) {
                    if (sets().areDisjoint(firstCharacters[i], firstCharacters[j]))
                        continue;
                    if (!pairNeverNeedsRetry(i, j))
                        return AlternativeDeterminism::Unknown;
                }
            }
        } else if (!checkOverlappingPairs(firstCharacters, pairNeverNeedsRetry))
            return AlternativeDeterminism::Unknown;

        if (!needsLongestFirst)
            return AlternativeDeterminism::Deterministic;
        for (unsigned i = 0; i < fixedSizes.size(); ++i) {
            if (!fixedSizes[i]) {
                countFixedSizeLimitHit(*alternatives[i]);
                return AlternativeDeterminism::Unknown;
            }
        }
        return AlternativeDeterminism::DeterministicIfLongestFirst;
    }

    template<typename PairPredicate>
    bool checkOverlappingPairs(const Vector<CharacterSetID, 8>& firstCharacters, const PairPredicate& pairNeverNeedsRetry)
    {
        struct OwnedSpan {
            char32_t begin;
            char32_t end;
            unsigned alternative;
        };
        unsigned pairsCompared = 0;
        auto checkPair = [&](unsigned one, unsigned other) {
            if (++pairsCompared > maxSpansSweptForDeterminism) {
                sets().countLimitHit();
                return false;
            }
            return pairNeverNeedsRetry(one, other);
        };

        Vector<OwnedSpan> spans;
        for (unsigned i = 0; i < firstCharacters.size(); ++i) {
            if (firstCharacters[i] == CharacterSetID::Unknown || sets().set(firstCharacters[i]).includesEndOfInput) {
                for (unsigned j = 0; j < firstCharacters.size(); ++j) {
                    if (j != i && !checkPair(i, j))
                        return false;
                }
                continue;
            }
            for (auto [begin, end] : sets().set(firstCharacters[i]).ranges)
                spans.append({ begin, end, i });
            if (spans.size() > maxSpansSweptForDeterminism) {
                sets().countLimitHit();
                return false;
            }
        }
        std::ranges::sort(spans, { }, &OwnedSpan::begin);
        Vector<OwnedSpan, 8> open;
        for (auto& span : spans) {
            open.removeAllMatching([&](auto& other) {
                return other.end < span.begin;
            });
            for (auto& other : open) {
                if (other.alternative == span.alternative)
                    continue;
                if (!checkPair(span.alternative, other.alternative))
                    return false;
            }
            open.append(span);
        }
        return true;
    }

    bool isGroupDisjointFrom(const PatternTerm& group, CharacterSetID following)
    {
        return std::ranges::all_of(group.parentheses.disjunction->m_alternatives, [&](auto& nested) {
            return sets().areDisjoint(nested->m_firstSet, following);
        });
    }

    AlternativeDeterminism markPossessiveTerms(PatternAlternative& alternative, const Continuation* continuation)
    {
        if (!m_stackCheck.isSafeToRecurse()) [[unlikely]] {
            sets().countStackExhaustion();
            return AlternativeDeterminism::Unknown;
        }
        if (alternative.matchDirection() != Forward)
            return AlternativeDeterminism::Unknown;

        bool isDeterministic = true;
        auto& terms = alternative.m_terms;
        for (unsigned i = 0; i < terms.size(); ++i) {
            PatternTerm& term = terms[i];
            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
            case PatternTerm::Type::AssertionEOL:
            case PatternTerm::Type::AssertionWordBoundary:
                break;
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass: {
                if (term.quantityType == QuantifierType::FixedCount)
                    break;
                if (term.quantityType == QuantifierType::Greedy && term.matchDirection() == Forward && !term.m_possessive && term.m_matchedSet != CharacterSetID::Unknown) {
                    auto assumption = i ? wordBoundaryExcludedBetween(terms[i - 1], term) : std::nullopt;
                    // reachesEnd does not matter here: where the pattern can end, the match succeeds and never backtracks into the run.
                    term.m_possessive = sets().areDisjoint(term.m_matchedSet, m_walker.followSet(alternative, i + 1, continuation, assumption).set);
                }
                isDeterministic &= term.m_possessive;
                break;
            }
            case PatternTerm::Type::ParentheticalAssertion:
                for (auto& nested : term.parentheses.disjunction->m_alternatives)
                    markPossessiveTerms(*nested, nullptr);
                break;
            case PatternTerm::Type::ParenthesesSubpattern: {
                Continuation afterGroup { &alternative, i + 1, term.quantityMaxCount > 1 ? &term : nullptr, continuation };
                bool groupIsDeterministic = term.matchDirection() == Forward;
                for (auto& nested : term.parentheses.disjunction->m_alternatives)
                    groupIsDeterministic &= markPossessiveTerms(*nested, &afterGroup) == AlternativeDeterminism::Deterministic;
                AlternativeDeterminism determinism = AlternativeDeterminism::Deterministic;
                if (groupIsDeterministic && term.parentheses.disjunction->m_alternatives.size() > 1) {
                    determinism = alternativeDeterminism(*term.parentheses.disjunction, afterGroup, i && term.quantityMaxCount == 1 ? &terms[i - 1] : nullptr);
                    groupIsDeterministic = determinism != AlternativeDeterminism::Unknown;
                }
                if (groupIsDeterministic && term.quantityType != QuantifierType::FixedCount)
                    groupIsDeterministic = term.quantityType == QuantifierType::Greedy && isGroupDisjointFrom(term, m_walker.followSet(alternative, i + 1, continuation).set);
                if (groupIsDeterministic && determinism == AlternativeDeterminism::DeterministicIfLongestFirst)
                    sortAlternativesLongestFirst(*term.parentheses.disjunction);
                if (groupIsDeterministic
                    && term.quantityType == QuantifierType::Greedy
                    && term.quantityMinCount <= 1
                    && term.quantityMaxCount == quantifyInfinite
                    && (term.capture() ? term.parentheses.lastSubpatternId == term.parentheses.subpatternId : !term.containsAnyCaptures())
                    && !term.parentheses.isCopy
                    && !term.parentheses.isTerminal
                    && !term.parentheses.isStringList)
                    term.m_possessive = true;
                if (groupIsDeterministic
                    && term.quantityMaxCount == 1
                    && (term.quantityType == QuantifierType::Greedy || term.parentheses.disjunction->m_alternatives.size() > 1)
                    && (term.capture() ? term.parentheses.lastSubpatternId == term.parentheses.subpatternId : !term.containsAnyCaptures())
                    && !term.parentheses.isCopy
                    && !term.parentheses.isStringList)
                    term.m_possessive = true;
                isDeterministic &= groupIsDeterministic;
                break;
            }
            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
            case PatternTerm::Type::DotStarEnclosure:
                isDeterministic = false;
                break;
            }
        }
        return isDeterministic ? AlternativeDeterminism::Deterministic : AlternativeDeterminism::Unknown;
    }

    YarrPattern& m_pattern;
    StackCheck& m_stackCheck;
    CharacterSetWalker m_walker;
};

void analyzeBacktracking(YarrPattern& pattern, StackCheck& stackCheck)
{
    BacktrackingAnalysis(pattern, stackCheck).run();
}

void sortAlternativesLongestFirst(PatternDisjunction& disjunction)
{
    auto& alternatives = disjunction.m_alternatives;
    std::stable_sort(alternatives.begin(), alternatives.end(), [](auto& one, auto& other) {
        return *BacktrackingAnalysis::alternativeFixedSize(*one) > *BacktrackingAnalysis::alternativeFixedSize(*other);
    });
    for (auto& nested : alternatives)
        nested->m_isLastAlternative = false;
    alternatives.last()->m_isLastAlternative = true;
}

} } // namespace JSC::Yarr
