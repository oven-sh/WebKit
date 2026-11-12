/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpSummary.h"

#if ENABLE(AOT)

#include "AOTRegExpRecord.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeIDs.h"
#include "AOTVector.h"
#include "RegExp.h"
#include "YarrPattern.h"
#include "YarrSyntaxChecker.h"
#include <algorithm>
#include <utility>
#include <wtf/StackBounds.h>
#include <wtf/StackPointer.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpSummaries);

static_assert(ParallelLoops::workerStackSize >= regExpCompilerStackSize + 64 * KB);

static size_t remainingStackOnThisThread()
{
    return static_cast<char*>(currentStackPointer()) - static_cast<char*>(Thread::currentSingleton().stack().end());
}

bool hasStackForRegExpCompiler()
{
    return remainingStackOnThisThread() >= regExpCompilerStackSize + 16 * KB;
}

static bool hasUnicodePropertyLookup(StringView pattern, OptionSet<Yarr::Flags> flags, const Yarr::SyntaxSummary& syntax)
{
    if (syntax.hasUnicodePropertyLookup)
        return true;
    if (flags.containsAny({ Yarr::Flags::Unicode, Yarr::Flags::UnicodeSets }))
        return false;
    return Yarr::checkSyntax(pattern, flags | Yarr::Flags::Unicode).hasUnicodePropertyLookup
        || Yarr::checkSyntax(pattern, flags | Yarr::Flags::UnicodeSets).hasUnicodePropertyLookup;
}

static bool exceedsDepth(const Yarr::YarrPattern& pattern, unsigned limit)
{
    Vector<std::pair<const Yarr::PatternDisjunction*, unsigned>, 16> worklist;
    worklist.append({ pattern.m_body, 0 });
    while (!worklist.isEmpty()) {
        auto [disjunction, depth] = worklist.takeLast();
        for (auto& alternative : disjunction->m_alternatives) {
            for (const Yarr::PatternTerm& term : alternative->m_terms) {
                switch (term.type) {
                case Yarr::PatternTerm::Type::ParenthesesSubpattern:
                case Yarr::PatternTerm::Type::ParentheticalAssertion:
                    if (depth >= limit)
                        return true;
                    worklist.append({ term.parentheses.disjunction, depth + 1 });
                    break;
                case Yarr::PatternTerm::Type::AssertionBOL:
                case Yarr::PatternTerm::Type::AssertionEOL:
                case Yarr::PatternTerm::Type::AssertionWordBoundary:
                case Yarr::PatternTerm::Type::PatternCharacter:
                case Yarr::PatternTerm::Type::CharacterClass:
                case Yarr::PatternTerm::Type::NumberedBackReference:
                case Yarr::PatternTerm::Type::NamedBackReference:
                case Yarr::PatternTerm::Type::NumberedForwardReference:
                case Yarr::PatternTerm::Type::NamedForwardReference:
                case Yarr::PatternTerm::Type::DotStarEnclosure:
                    break;
                }
            }
        }
    }
    return false;
}

static Vector<RegExpNamedGroupSummary> sortedNamedGroups(const Yarr::YarrPattern& pattern)
{
    Vector<RegExpNamedGroupSummary> groups;
    groups.reserveInitialCapacity(pattern.m_namedGroupToParenIndices.size());
    for (auto& [name, parenIndices] : pattern.m_namedGroupToParenIndices)
        groups.append({ name, parenIndices });
    AOT::sort(groups, [](const RegExpNamedGroupSummary& a, const RegExpNamedGroupSummary& b) {
        return codePointCompareLessThan(a.name, b.name);
    });
    return groups;
}

static bool refersToGroup(const Yarr::PatternDisjunction& disjunction)
{
    for (auto& alternative : disjunction.m_alternatives) {
        for (const Yarr::PatternTerm& term : alternative->m_terms) {
            switch (term.type) {
            case Yarr::PatternTerm::Type::NumberedBackReference:
            case Yarr::PatternTerm::Type::NamedBackReference:
            case Yarr::PatternTerm::Type::NumberedForwardReference:
            case Yarr::PatternTerm::Type::NamedForwardReference:
                return true;
            case Yarr::PatternTerm::Type::ParenthesesSubpattern:
            case Yarr::PatternTerm::Type::ParentheticalAssertion:
                if (refersToGroup(*term.parentheses.disjunction))
                    return true;
                break;
            case Yarr::PatternTerm::Type::AssertionBOL:
            case Yarr::PatternTerm::Type::AssertionEOL:
            case Yarr::PatternTerm::Type::AssertionWordBoundary:
            case Yarr::PatternTerm::Type::PatternCharacter:
            case Yarr::PatternTerm::Type::CharacterClass:
            case Yarr::PatternTerm::Type::DotStarEnclosure:
                break;
            }
        }
    }
    return false;
}

enum class LookaroundContents : bool { Ignored, Checked };

static bool consumesOnly8BitCharacters(const Yarr::PatternDisjunction& disjunction, LookaroundContents lookaroundContents)
{
    for (auto& alternative : disjunction.m_alternatives) {
        for (const Yarr::PatternTerm& term : alternative->m_terms) {
            switch (term.type) {
            case Yarr::PatternTerm::Type::AssertionBOL:
            case Yarr::PatternTerm::Type::AssertionEOL:
            case Yarr::PatternTerm::Type::AssertionWordBoundary:
            case Yarr::PatternTerm::Type::NumberedBackReference:
            case Yarr::PatternTerm::Type::NamedBackReference:
            case Yarr::PatternTerm::Type::NumberedForwardReference:
            case Yarr::PatternTerm::Type::NamedForwardReference:
                break;
            case Yarr::PatternTerm::Type::ParentheticalAssertion:
                if (lookaroundContents == LookaroundContents::Checked && !consumesOnly8BitCharacters(*term.parentheses.disjunction, lookaroundContents))
                    return false;
                break;
            case Yarr::PatternTerm::Type::PatternCharacter:
                if (term.patternCharacter > 0xff)
                    return false;
                break;
            case Yarr::PatternTerm::Type::CharacterClass: {
                const Yarr::CharacterClass& characterClass = *term.characterClass;
                bool holdsWideCharacter = characterClass.hasStrings()
                    || std::ranges::any_of(characterClass.m_matches32, [](char32_t character) { return character > 0xff; })
                    || std::ranges::any_of(characterClass.m_ranges32, [](const Yarr::CharacterRange& range) { return range.end > 0xff; });
                if (term.invert() || holdsWideCharacter)
                    return false;
                break;
            }
            case Yarr::PatternTerm::Type::ParenthesesSubpattern:
                if (!consumesOnly8BitCharacters(*term.parentheses.disjunction, lookaroundContents))
                    return false;
                break;
            case Yarr::PatternTerm::Type::DotStarEnclosure:
                return false;
            }
        }
    }
    return true;
}

static bool matchesOnlyWhole8BitSubjects(const Yarr::YarrPattern& pattern)
{
    if (pattern.multiline() || pattern.ignoreCase() || pattern.m_containsModifiers)
        return false;
    for (auto& alternative : pattern.m_body->m_alternatives) {
        auto& terms = alternative->m_terms;
        if (terms.size() < 2 || terms.first().type != Yarr::PatternTerm::Type::AssertionBOL || terms.last().type != Yarr::PatternTerm::Type::AssertionEOL)
            return false;
    }
    return consumesOnly8BitCharacters(*pattern.m_body, refersToGroup(*pattern.m_body) ? LookaroundContents::Checked : LookaroundContents::Ignored);
}

static void addOtherCases(WTF::BitSet<256>& characters)
{
    for (unsigned letter = 'a'; letter <= 'z'; ++letter) {
        unsigned upper = toASCIIUpper(letter);
        if (characters.get(letter) || characters.get(upper)) {
            characters.set(letter);
            characters.set(upper);
        }
    }
}

struct FirstCharacters {
    WTF::BitSet<256> characters { };
    bool areLatin1 { true };
};

static bool addCharactersOfClass(const Yarr::PatternTerm& term, FirstCharacters& found)
{
    const Yarr::CharacterClass& characterClass = *term.characterClass;
    if (characterClass.hasStrings())
        return false;
    WTF::BitSet<256> held;
    auto add = [&](char32_t first, char32_t last) {
        for (char32_t character = first; character <= std::min<char32_t>(last, 0xff); ++character)
            held.set(character);
    };
    if (characterClass.m_anyCharacter)
        add(0, 0xff);
    for (char32_t character : characterClass.m_matches8)
        add(character, character);
    for (char32_t character : characterClass.m_matches32)
        add(character, character);
    for (const Yarr::CharacterRange& range : characterClass.m_ranges8)
        add(range.begin, range.end);
    for (const Yarr::CharacterRange& range : characterClass.m_ranges32)
        add(range.begin, range.end);
    bool holdsWideCharacter = std::ranges::any_of(characterClass.m_matches32, [](char32_t character) { return character > 0xff; })
        || std::ranges::any_of(characterClass.m_ranges32, [](const Yarr::CharacterRange& range) { return range.end > 0xff; });
    found.areLatin1 &= !term.invert() && !characterClass.m_anyCharacter && !holdsWideCharacter && !term.ignoreCase();
    if (term.ignoreCase()) {
        if (held.findBit(0x80, true) < 256)
            return false;
        addOtherCases(held);
    }
    if (term.invert())
        held.invert();
    found.characters.merge(held);
    return true;
}

static bool addFirstCharacters(const Yarr::PatternAlternative& alternative, size_t firstTerm, FirstCharacters& found)
{
    for (const Yarr::PatternTerm& term : alternative.m_terms.span().subspan(firstTerm)) {
        switch (term.type) {
        case Yarr::PatternTerm::Type::PatternCharacter: {
            char32_t character = term.patternCharacter;
            if (term.ignoreCase()) {
                if (!isASCII(character))
                    return false;
                found.characters.set(toASCIILower(character));
                found.characters.set(toASCIIUpper(character));
                found.areLatin1 = false;
            } else if (character <= 0xff)
                found.characters.set(character);
            else
                found.areLatin1 = false;
            break;
        }
        case Yarr::PatternTerm::Type::CharacterClass:
            if (!addCharactersOfClass(term, found))
                return false;
            break;
        case Yarr::PatternTerm::Type::ParenthesesSubpattern:
            for (auto& inner : term.parentheses.disjunction->m_alternatives) {
                if (!addFirstCharacters(*inner, 0, found))
                    return false;
            }
            break;
        case Yarr::PatternTerm::Type::AssertionBOL:
        case Yarr::PatternTerm::Type::AssertionEOL:
        case Yarr::PatternTerm::Type::AssertionWordBoundary:
        case Yarr::PatternTerm::Type::NumberedBackReference:
        case Yarr::PatternTerm::Type::NamedBackReference:
        case Yarr::PatternTerm::Type::NumberedForwardReference:
        case Yarr::PatternTerm::Type::NamedForwardReference:
        case Yarr::PatternTerm::Type::ParentheticalAssertion:
        case Yarr::PatternTerm::Type::DotStarEnclosure:
            return false;
        }
        if (term.quantityMinCount)
            return true;
    }
    return false;
}

static std::optional<FirstCharacters> firstCharactersOf(const Yarr::YarrPattern& pattern)
{
    if (pattern.multiline() || pattern.m_containsModifiers)
        return std::nullopt;
    FirstCharacters found;
    for (auto& alternative : pattern.m_body->m_alternatives) {
        auto& terms = alternative->m_terms;
        if (terms.isEmpty() || terms.first().type != Yarr::PatternTerm::Type::AssertionBOL || !addFirstCharacters(*alternative, 1, found))
            return std::nullopt;
    }
    return found;
}

static uint64_t groupsPresentInEveryMatch(const Yarr::PatternDisjunction& disjunction)
{
    if (disjunction.m_alternatives.size() != 1)
        return 0;
    uint64_t groups = 0;
    for (const Yarr::PatternTerm& term : disjunction.m_alternatives[0]->m_terms) {
        if (term.type != Yarr::PatternTerm::Type::ParenthesesSubpattern || term.parentheses.isCopy || term.quantityMinCount != 1 || term.quantityMaxCount != 1)
            continue;
        if (term.capture() && term.parentheses.subpatternId < 64)
            groups |= uint64_t(1) << term.parentheses.subpatternId;
        groups |= groupsPresentInEveryMatch(*term.parentheses.disjunction);
    }
    return groups;
}

RegExpSummary summarizeRegExp(StringView pattern, OptionSet<Yarr::Flags> flags, unsigned parenthesesDepthLimit)
{
    RegExpSummary summary;
    Yarr::SyntaxSummary syntax = Yarr::checkSyntax(pattern, flags);
    summary.syntaxError = syntax.error;
    summary.hasUnicodePropertyLookup = hasUnicodePropertyLookup(pattern, flags, syntax);
    summary.patternHash = pattern.hash();
    if (!summary.isValid())
        return summary;

    String text = pattern.toString();
    summary.numSubpatterns = syntax.numSubpatterns;
    summary.source = RegExp::escapedPattern(text);
    summary.sourceIsPattern = summary.source == text;

    if (syntax.maxParenthesesDepth > parenthesesDepthLimit) {
        summary.exceedsParenthesesDepth = true;
        return summary;
    }

    RELEASE_ASSERT(remainingStackOnThisThread() >= regExpCompilerStackSize);
    Yarr::YarrPattern yarrPattern(pattern, flags, summary.constructionError, Yarr::ExecutionMode::IncludeSubpatterns, Yarr::PatternAnalysis::CharacterClassForms);
    if (Yarr::hasError(summary.constructionError))
        return summary;

    if (exceedsDepth(yarrPattern, parenthesesDepthLimit)) {
        summary.exceedsParenthesesDepth = true;
        return summary;
    }

    RELEASE_ASSERT(summary.numSubpatterns == yarrPattern.m_numSubpatterns);
    summary.minimumSize = yarrPattern.m_body->m_minimumSize;
    summary.numDuplicateNamedCaptureGroups = yarrPattern.m_numDuplicateNamedCaptureGroups;
    summary.groupsPresentInEveryMatch = yarrPattern.m_numDuplicateNamedCaptureGroups ? 0 : groupsPresentInEveryMatch(*yarrPattern.m_body);
    if (std::optional<FirstCharacters> first = firstCharactersOf(yarrPattern)) {
        summary.firstCharacters = first->characters;
        summary.firstCharactersAreLatin1 = first->areLatin1;
    }
    summary.specificPattern = yarrPattern.m_specificPattern;
    summary.matchesOnlyWhole8BitSubjects = matchesOnlyWhole8BitSubjects(yarrPattern);
    summary.namedGroups = sortedNamedGroups(yarrPattern);
    summary.atom = WTF::move(yarrPattern.m_atom);
    summary.characterSet = WTF::move(yarrPattern.m_specificCharacterClass);
    summary.captureGroupNames = WTF::move(yarrPattern.m_captureGroupNames);
    summary.metadata = RegExpMetadata::Known;
    return summary;
}

RegExpSummaries::RegExpSummaries(unsigned parenthesesDepthLimit)
    : m_parenthesesDepthLimit(parenthesesDepthLimit)
{
}

auto RegExpSummaries::keyOf(StringID pattern, OptionSet<Yarr::Flags> flags) -> Key
{
    return static_cast<uint64_t>(pattern.index()) << 16 | ImageRegExp::significantFlags(flags).toRaw();
}

static std::unique_ptr<const RegExpSummary> summarizeRegExpWithEnoughStack(StringView patternText, OptionSet<Yarr::Flags> flags, unsigned parenthesesDepthLimit)
{
    std::unique_ptr<const RegExpSummary> summary;
    auto summarize = [&] {
        summary = makeUnique<RegExpSummary>(summarizeRegExp(patternText, ImageRegExp::significantFlags(flags), parenthesesDepthLimit));
    };
    if (hasStackForRegExpCompiler())
        summarize();
    else
        runOnBuildThread(summarize);
    return summary;
}

const RegExpSummary& RegExpSummaries::summaryOf(StringID pattern, OptionSet<Yarr::Flags> flags, StringView patternText)
{
    if (Typed::liesInThread(pattern))
        return summaryOfText(patternText, flags);

    Key key = keyOf(pattern, flags);
    {
        Locker locker { m_lock };
        auto found = m_summaries.find(key);
        if (found != m_summaries.end())
            return *found->value;
    }

    std::unique_ptr<const RegExpSummary> summary = summarizeRegExpWithEnoughStack(patternText, flags, m_parenthesesDepthLimit);
    Locker locker { m_lock };
    return *m_summaries.add(key, WTF::move(summary)).iterator->value;
}

const RegExpSummary& RegExpSummaries::summaryOfText(StringView patternText, OptionSet<Yarr::Flags> flags)
{
    RegExpKey key(ImageRegExp::significantFlags(flags), patternText.toString());
    {
        Locker locker { m_lock };
        auto found = m_summariesByText.find(key);
        if (found != m_summariesByText.end())
            return *found->value;
    }

    std::unique_ptr<const RegExpSummary> summary = summarizeRegExpWithEnoughStack(patternText, flags, m_parenthesesDepthLimit);
    Locker locker { m_lock };
    return *m_summariesByText.add(WTF::move(key), WTF::move(summary)).iterator->value;
}

const RegExpMatchPlan* RegExpSummaries::matchPlanOf(StringID pattern, OptionSet<Yarr::Flags> flags, StringView patternText)
{
    const RegExpSummary* summary = &summaryOf(pattern, flags, patternText);
    if (!summary->canBeCreated())
        return nullptr;
    {
        Locker locker { m_lock };
        auto found = m_matchPlans.find(summary);
        if (found != m_matchPlans.end())
            return found->value.get();
    }

    std::unique_ptr<const RegExpMatchPlan> plan;
    auto planMatch = [&] {
        Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
        Yarr::YarrPattern yarrPattern(patternText, ImageRegExp::significantFlags(flags), error, Yarr::ExecutionMode::IncludeSubpatterns, Yarr::PatternAnalysis::CharacterClassForms);
        RELEASE_ASSERT(!Yarr::hasError(error));
        plan = planRegExpMatch(yarrPattern);
    };
    if (hasStackForRegExpCompiler())
        planMatch();
    else
        runOnBuildThread(planMatch);
    Locker locker { m_lock };
    return m_matchPlans.add(summary, WTF::move(plan)).iterator->value.get();
}

const RegExpSummary* RegExpSummaries::summaryIfComputed(StringID pattern, OptionSet<Yarr::Flags> flags) const
{
    Locker locker { m_lock };
    auto found = m_summaries.find(keyOf(pattern, flags));
    return found == m_summaries.end() ? nullptr : found->value.get();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
