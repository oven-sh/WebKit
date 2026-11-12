/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpRequests.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTNumbering.h"
#include "AOTRegExpRecord.h"
#include "AOTRegExpSummary.h"
#include "AOTStringTable.h"
#include "AOTThreads.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringView.h>

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpRequests);

using RegExpRequestKey = std::pair<uint32_t, uint16_t>;

static RegExpRequestKey keyOfRegExpRequest(const RegExpRequest& request)
{
    return { request.pattern.index(), request.flags.toRaw() };
}

static bool isAscendingListOfRegExpRequests(std::span<const RegExpRequest> requests, bool holdsLiterals)
{
    return std::ranges::adjacent_find(requests, std::ranges::greater_equal { }, keyOfRegExpRequest) == requests.end()
        && std::ranges::all_of(requests, [&](const RegExpRequest& request) {
            return request.flags == ImageRegExp::significantFlags(request.flags) && (request.origin == RegExpRequestOrigin::Literal) == holdsLiterals;
        });
}

RegExpRequests::RegExpRequests(Vector<RegExpRequest>&& literals, Vector<RegExpRequest>&& predictedTexts)
    : m_requests(WTF::move(literals))
    , m_numberOfLiterals(m_requests.size())
{
    RELEASE_ASSERT(isAscendingListOfRegExpRequests(m_requests.span(), true));
    RELEASE_ASSERT(isAscendingListOfRegExpRequests(predictedTexts.span(), false));
    RELEASE_ASSERT(std::ranges::none_of(predictedTexts, [&](const RegExpRequest& request) {
        return std::ranges::binary_search(m_requests, keyOfRegExpRequest(request), { }, keyOfRegExpRequest);
    }));
    m_requests.appendVector(WTF::move(predictedTexts));
}

RegExpRecordID RegExpRequests::recordOf(StringID pattern, OptionSet<Yarr::Flags> flags) const
{
    RegExpRequestKey key { pattern.index(), ImageRegExp::significantFlags(flags).toRaw() };
    std::span<const RegExpRequest> sortedRequests = literals();
    auto found = std::ranges::lower_bound(sortedRequests, key, { }, keyOfRegExpRequest);
    if (found == sortedRequests.end() || keyOfRegExpRequest(*found) != key)
        return { };
    return RegExpRecordID(static_cast<size_t>(found - sortedRequests.begin()));
}

static Vector<RegExpRequest> requestsForMentionedRegExps(const NumberedKeys& mentionedKeys, const ParallelLoops& loops)
{
    Vector<RegExpRequestKey> keys;
    Vector<RegExpRequestKey> keysWithMatcher;
    keys.reserveInitialCapacity(mentionedKeys.size());
    for (uint32_t index = 0; index < mentionedKeys.size(); ++index) {
        std::span<const uint32_t> words = mentionedKeys.keyAt(index);
        RELEASE_ASSERT(words.size() == 2);
        auto flags = OptionSet<Yarr::Flags>::fromRaw(safeCast<uint16_t>(words[1]));
        keys.append(RegExpRequestKey { words[0], ImageRegExp::significantFlags(flags).toRaw() });
        if (flags.containsAll(MentionList::flagsOfMatcher))
            keysWithMatcher.append(keys.last());
    }
    sortAndRemoveDuplicates(loops, keys);
    sortAndRemoveDuplicates(loops, keysWithMatcher);

    return keys.map([&](const RegExpRequestKey& key) {
        return RegExpRequest { .pattern = StringID(key.first), .flags = OptionSet<Yarr::Flags>::fromRaw(key.second), .needsMatcher = std::ranges::binary_search(keysWithMatcher, key) };
    });
}

static Vector<RegExpRequest> requestsForPredictedRegExpTexts(const Typed::Facts& facts, Typed::StringTable& strings, std::span<const RegExpRequest> sortedLiterals, const ParallelLoops& loops)
{
    using PredictedKey = std::tuple<uint32_t, uint16_t, RegExpRequestOrigin>;
    static_assert(RegExpRequestOrigin::Evaluated < RegExpRequestOrigin::Candidate);

    auto originOf = [](bool isOneOfAllAtSomeCall) {
        return isOneOfAllAtSomeCall ? RegExpRequestOrigin::Evaluated : RegExpRequestOrigin::Candidate;
    };
    Vector<PredictedKey> keys;
    keys.reserveInitialCapacity(facts.regExpTextsInSight().size() + facts.regExpTextsWorkedOut().size());
    for (const Typed::RegExpTextInSight& text : facts.regExpTextsInSight())
        keys.append(PredictedKey { text.pattern.index(), text.flags, originOf(text.isOneOfAllAtSomeCall) });
    for (const Typed::RegExpTextWorkedOut& text : facts.regExpTextsWorkedOut())
        keys.append(PredictedKey { strings.intern(StringView(text.pattern)).index(), text.flags, originOf(text.isOneOfAllAtSomeCall) });
    sortAndRemoveDuplicates(loops, keys);

    Vector<RegExpRequest> requests;
    for (auto [pattern, flags, origin] : keys) {
        RegExpRequestKey key { pattern, flags };
        bool repeatsStrongerRequest = !requests.isEmpty() && keyOfRegExpRequest(requests.last()) == key;
        if (repeatsStrongerRequest || std::ranges::binary_search(sortedLiterals, key, { }, keyOfRegExpRequest))
            continue;
        requests.append(RegExpRequest { .pattern = StringID(pattern), .flags = OptionSet<Yarr::Flags>::fromRaw(flags), .origin = origin });
    }
    return requests;
}

static bool areValidRegExpFlags(OptionSet<Yarr::Flags> flags)
{
    Yarr::FlagsString characters = Yarr::flagsString(flags);
    std::optional<OptionSet<Yarr::Flags>> parsedFlags = Yarr::parseFlags(StringView { characters.span() });
    return parsedFlags && *parsedFlags == flags;
}

static Vector<const RegExpSummary*> summarizeRequestedRegExps(std::span<const RegExpRequest> requests, const Typed::StringTable& strings, BuildContext& context)
{
    Vector<const RegExpSummary*> summaries;
    summaries.fill(nullptr, requests.size());
    forEachIndexInParallel(context.parallelLoops(), safeCast<unsigned>(requests.size()), [&](unsigned index) {
        const RegExpRequest& request = requests[index];
        if (!areValidRegExpFlags(request.flags))
            return;
        Vector<char16_t> patternBuffer;
        StringView pattern = strings.viewOf(request.pattern, patternBuffer);
        summaries[index] = &context.regExpSummaries().summaryOf(request.pattern, request.flags, pattern);
    });
    return summaries;
}

static bool canHaveRegExpRecord(const RegExpSummary* summary)
{
    return summary && summary->canBeCreated();
}

static void internStringsOfRegExpMetadata(RegExpRequest& request, const RegExpSummary& summary, Typed::StringTable& strings)
{
    if (!summary.atom.isNull())
        request.atom = strings.intern(StringView(summary.atom));
    request.captureGroupNames = summary.captureGroupNames.map([&](const String& name) {
        return name.isNull() ? StringID() : strings.intern(StringView(name));
    });
    request.namedGroups = summary.namedGroups.map([&](const RegExpNamedGroupSummary& group) {
        return RegExpNamedGroupStrings { strings.intern(StringView(group.name)), group.parenIndices };
    });
}

void ImageNumbersBuilder::collectRegExpRequests()
{
    RELEASE_ASSERT(!ParallelLoops::isInsideLoop());

    Typed::StringTable& strings = inputs.program.strings;
    Vector<RegExpRequest> literals = requestsForMentionedRegExps(mentions.keys(NumberSpace::RegExp), context.parallelLoops());
    Vector<RegExpRequest> predictedTexts = requestsForPredictedRegExpTexts(inputs.facts, strings, literals.span(), context.parallelLoops());
    Vector<const RegExpSummary*> summariesOfLiterals = summarizeRequestedRegExps(literals.span(), strings, context);
    Vector<const RegExpSummary*> summariesOfPredictedTexts = summarizeRequestedRegExps(predictedTexts.span(), strings, context);

    for (size_t index = 0; index < literals.size(); ++index) {
        RegExpRequest& request = literals[index];
        if (!canHaveRegExpRecord(summariesOfLiterals[index])) {
            context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::InvalidRegExpLiteral, .text = request.pattern });
            continue;
        }
        internStringsOfRegExpMetadata(request, *summariesOfLiterals[index], strings);
    }

    Vector<RegExpRequest> predictedTextsWithRecord;
    for (size_t index = 0; index < predictedTexts.size(); ++index) {
        if (!canHaveRegExpRecord(summariesOfPredictedTexts[index]))
            continue;
        internStringsOfRegExpMetadata(predictedTexts[index], *summariesOfPredictedTexts[index], strings);
        predictedTextsWithRecord.append(WTF::move(predictedTexts[index]));
    }

    numbers.m_regExpRequests = makeUnique<RegExpRequests>(WTF::move(literals), WTF::move(predictedTextsWithRecord));
}

} } // namespace JSC::AOT

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
