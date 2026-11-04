/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpSummary.h"

#if ENABLE(AOT)

#include "AOTRegExpRecord.h"
#include "AOTSort.h"
#include "AOTStringTable.h"
#include "AOTThreads.h"
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

// RegExp.prototype[@@split] and [@@matchAll] parse the source again with whatever the `flags` property of the receiver returns.
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

    // Not hasStackForRegExpCompiler(): the 16 KB it adds are for the frames between a caller that tests it and this one.
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
    summary.specificPattern = yarrPattern.m_specificPattern;
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

const RegExpSummary& RegExpSummaries::summaryOf(StringID pattern, OptionSet<Yarr::Flags> flags, StringView patternText)
{
    // A provisional StringID names another string in the next interning phase.
    if (Typed::StringTable::isProvisional(pattern))
        return summaryOfText(patternText, flags);

    Key key = keyOf(pattern, flags);
    {
        Locker locker { m_lock };
        auto found = m_summaries.find(key);
        if (found != m_summaries.end())
            return *found->value;
    }

    std::unique_ptr<const RegExpSummary> summary = makeUnique<RegExpSummary>(summarizeRegExp(patternText, ImageRegExp::significantFlags(flags), m_parenthesesDepthLimit));

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

    std::unique_ptr<const RegExpSummary> summary = makeUnique<RegExpSummary>(summarizeRegExp(patternText, key.flagsValue, m_parenthesesDepthLimit));

    Locker locker { m_lock };
    return *m_summariesByText.add(WTF::move(key), WTF::move(summary)).iterator->value;
}

const RegExpSummary* RegExpSummaries::summaryIfComputed(StringID pattern, OptionSet<Yarr::Flags> flags) const
{
    Locker locker { m_lock };
    auto found = m_summaries.find(keyOf(pattern, flags));
    return found == m_summaries.end() ? nullptr : found->value.get();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
