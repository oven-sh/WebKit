/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpRequests.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTCFG.h"
#include "AOTFlowResults.h"
#include "AOTIR.h"
#include "AOTPhaseTimes.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpRecord.h"
#include "AOTRegExpSummary.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpRequests);

static_assert(Typed::Value::opcodesWithPattern == Typed::Value::opcodesWithRegExpFlags);
static_assert(Typed::Value::opcodesWithPattern == Typed::Value::opcodesWithRegExpRecord);

namespace {

struct CollectionInputs {
    const Typed::Program& program;
    const Typed::FlowResults& flow;
    BuildContext& context;
};

struct FoundRegExp {
    RegExpRequestOrigin origin;
    StringID pattern;
    OptionSet<Yarr::Flags> flags;
    RegExpSite site;
};

struct RegExpConstructorCall {
    Typed::BuiltinCall builtinCall;
    Typed::AccessSiteID accessSite;
    RegExpSite site;
    Typed::BlockID block;
    SourceLocation location;
};

using FlagSets = Vector<OptionSet<Yarr::Flags>, 8>;

} // anonymous namespace

static auto keyOf(const RegExpRequest& request)
{
    return std::pair { request.pattern.index(), request.flags.toRaw() };
}

static auto orderKey(const FoundRegExp& found)
{
    return std::tuple { found.origin, found.pattern, found.flags.toRaw(), found.site };
}

static bool isAscendingList(std::span<const RegExpRequest> requests, bool holdsLiterals)
{
    bool hasOrigin = std::ranges::all_of(requests, [&](const RegExpRequest& request) {
        return (request.origin == RegExpRequestOrigin::Literal) == holdsLiterals;
    });
    return hasOrigin && std::ranges::adjacent_find(requests, std::ranges::greater_equal { }, keyOf) == requests.end();
}

static std::optional<size_t> findRequest(std::span<const RegExpRequest> ascendingRequests, StringID pattern, OptionSet<Yarr::Flags> significantFlags)
{
    std::pair key { pattern.index(), significantFlags.toRaw() };
    auto found = std::ranges::lower_bound(ascendingRequests, key, { }, keyOf);
    if (found == ascendingRequests.end() || keyOf(*found) != key)
        return std::nullopt;
    return static_cast<size_t>(found - ascendingRequests.begin());
}

RegExpRequests::RegExpRequests(Vector<RegExpRequest>&& literals, Vector<RegExpRequest>&& candidates)
    : m_requests(WTF::move(literals))
    , m_numberOfLiterals(static_cast<uint32_t>(m_requests.size()))
{
    RELEASE_ASSERT(isAscendingList(m_requests.span(), true));
    RELEASE_ASSERT(isAscendingList(candidates.span(), false));
    m_requests.appendVector(WTF::move(candidates));
}

RegExpRecordID RegExpRequests::recordOf(StringID pattern, OptionSet<Yarr::Flags> flags) const
{
    std::optional<size_t> index = findRequest(literals(), pattern, ImageRegExp::significantFlags(flags));
    RELEASE_ASSERT(index);
    return RegExpRecordID(*index);
}

static bool callsRegExpConstructor(const Typed::Program& program, BuiltinOverloadID overload)
{
    const BuiltinTable& builtins = program.builtins();
    return builtins.canonicalID(builtins.at(overload).builtin) == Builtins::RegExp;
}

static bool mayPassArgument(const Typed::BuiltinCall& call, uint32_t argumentIndex)
{
    switch (call.form) {
    case Typed::BuiltinCallForm::List:
        return true;
    case Typed::BuiltinCallForm::Whole:
    case Typed::BuiltinCallForm::Fields:
    case Typed::BuiltinCallForm::Prefix:
    case Typed::BuiltinCallForm::AnyReceiver:
        return argumentIndex < call.arguments.size();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlagSets parseFlagSets(const Typed::Program& program, const Typed::SinkCandidates& candidates)
{
    FlagSets flagSets;
    // Unknown flags may be "", "g" or "d", which all find the record without significant flags.
    if (candidates.mayBeUndefined || !candidates.isComplete)
        flagSets.append(OptionSet<Yarr::Flags> { });
    Vector<char16_t> buffer;
    for (StringID text : candidates.strings) {
        std::optional<OptionSet<Yarr::Flags>> flags = Yarr::parseFlags(program.strings.viewOf(text, buffer));
        if (!flags)
            continue;
        flagSets.appendIfNotContains(ImageRegExp::significantFlags(*flags));
    }
    return flagSets;
}

static void reportTextKnownAtRunTime(BuildContext& context, RegExpSite site, Typed::BlockID block, SourceLocation location)
{
    context.diagnostics().add(UserDiagnostic {
        .kind = UserDiagnosticKind::RegExpWithoutCode,
        .function = site.function,
        .block = block,
        .value = site.value,
        .count = static_cast<uint32_t>(RegExpDeclineReason::TextKnownAtRunTime),
        .location = location,
    });
}

// Plain characters are searched for without code, and the run time sees that they are plain when it parses them.
static bool mayHaveCode(const RegExpSummary& summary)
{
    return summary.isValid() && summary.atom.isNull();
}

static void addCandidates(const Typed::Program& program, BuildContext& context, RegExpRequestOrigin origin, StringID pattern, const FlagSets& flagSets, RegExpSite site, Vector<FoundRegExp>& found)
{
    Vector<char16_t> buffer;
    StringView text = program.strings.viewOf(pattern, buffer);
    for (OptionSet<Yarr::Flags> flagSet : flagSets) {
        if (mayHaveCode(context.regExpSummaries().summaryOf(pattern, flagSet, text)))
            found.append(FoundRegExp { origin, pattern, flagSet, site });
    }
}

static void addCandidatesOfFlow(const CollectionInputs& inputs, const Typed::SinkCandidates& patterns, const FlagSets& flagSets, RegExpSite site, Vector<FoundRegExp>& found)
{
    const Typed::StringTable& strings = inputs.program.strings;
    auto addPattern = [&](StringID pattern) {
        addCandidates(inputs.program, inputs.context, RegExpRequestOrigin::Candidate, pattern, flagSets, site, found);
    };
    for (StringID pattern : patterns.strings)
        addPattern(pattern);
    // The record of an operand that flow knows nothing about says mayBeUndefined too.
    bool flowSawUndefined = patterns.mayBeUndefined && patterns != Typed::SinkCandidates { };
    if (flowSawUndefined && !patterns.strings.contains(strings.emptyString()))
        addPattern(strings.emptyString());
}

static void collectCandidates(const CollectionInputs& inputs, const RegExpConstructorCall& call, Vector<FoundRegExp>& found)
{
    const Typed::SinkCandidates notPassed { .strings = { }, .mayBeUndefined = true, .isComplete = true };
    auto candidatesOf = [&](uint32_t argumentIndex) -> const Typed::SinkCandidates& {
        return mayPassArgument(call.builtinCall, argumentIndex) ? inputs.flow.sinkCandidates(call.accessSite, argumentIndex) : notPassed;
    };
    const Typed::SinkCandidates& patterns = candidatesOf(0);
    const Typed::SinkCandidates& flags = candidatesOf(1);
    FlagSets flagSets = parseFlagSets(inputs.program, flags);

    Subject subject { .function = call.site.function, .block = call.block, .value = call.site.value, .location = call.location };
    // A crowd of texts at one call says little about the text that the call gets.
    bool takesTextsOfFlow = !inputs.context.exceeds(LimitID::RegExpCandidatesPerSite, patterns.strings.size(), subject);
    if (takesTextsOfFlow)
        addCandidatesOfFlow(inputs, patterns, flagSets, call.site, found);
    if (!takesTextsOfFlow || !patterns.isComplete || !flags.isComplete)
        reportTextKnownAtRunTime(inputs.context, call.site, call.block, call.location);
}

static void collectFromFunction(const CollectionInputs& inputs, FunctionID id, Vector<FoundRegExp>& found)
{
    const Typed::Program& program = inputs.program;
    const Typed::Function& function = program.at(id);

    Vector<RegExpConstructorCall, 4> calls;
    // Placement asks for the record of a literal in every block, reachable or not.
    forEachListedValue(program, id, Typed::Value::opcodesWithPattern | Typed::builtinCallOpcodes, [&](Typed::BlockID blockID, uint32_t, ValueID valueID, const Typed::Value& value) {
        bool isLiteral = Typed::Value::opcodesWithPattern.contains(value.opcode);
        if (!isLiteral && !callsRegExpConstructor(program, value.overload()))
            return;
        RegExpSite site { id, valueID, function.locationOf(valueID) };
        if (isLiteral)
            found.append(FoundRegExp { RegExpRequestOrigin::Literal, value.pattern(), ImageRegExp::significantFlags(toYarrFlags(value.regExpFlags())), site });
        else
            calls.append(RegExpConstructorCall { builtinCallOf(program, value), value.accessSite(), site, blockID, site.location });
    });
    if (calls.isEmpty())
        return;

    Typed::CFG cfg(function);
    for (const RegExpConstructorCall& call : calls) {
        if (cfg.isReachable(call.block))
            collectCandidates(inputs, call, found);
    }
}

static void addToRequests(Vector<RegExpRequest>& ascendingRequests, const FoundRegExp& found)
{
    if (ascendingRequests.isEmpty() || ascendingRequests.last().pattern != found.pattern || ascendingRequests.last().flags != found.flags)
        ascendingRequests.append(RegExpRequest { .pattern = found.pattern, .flags = found.flags, .origin = found.origin });
    Vector<RegExpSite, 1>& sites = ascendingRequests.last().sites;
    if (sites.isEmpty() || sites.last() != found.site)
        sites.append(found.site);
}

static void moveSitesOfRequestsAlsoIn(Vector<RegExpRequest>& strongerRequests, Vector<RegExpRequest>& requests)
{
    requests.removeAllMatching([&](const RegExpRequest& request) {
        std::optional<size_t> stronger = findRequest(strongerRequests.span(), request.pattern, request.flags);
        if (!stronger)
            return false;
        for (const RegExpSite& site : request.sites)
            strongerRequests[*stronger].sites.appendIfNotContains(site);
        return true;
    });
}

static void internMetadataStrings(RegExpRequest& request, const Typed::Program& program, Typed::StringArena& strings, BuildContext& context)
{
    Vector<char16_t> buffer;
    StringView text = program.strings.viewOf(request.pattern, buffer);
    const RegExpSummary& summary = context.regExpSummaries().summaryOf(request.pattern, request.flags, text);
    switch (summary.metadata) {
    case RegExpMetadata::Unknown:
        return;
    case RegExpMetadata::Known:
        break;
    }

    if (!summary.atom.isNull())
        request.atom = StringView(summary.atom) == text ? request.pattern : strings.intern(StringView(summary.atom));
    request.captureGroupNames = summary.captureGroupNames.map([&](const String& name) {
        return name.isNull() ? StringID() : strings.intern(StringView(name));
    });
    request.namedGroups = summary.namedGroups.map([&](const RegExpNamedGroupSummary& group) {
        return RegExpNamedGroupStrings { strings.intern(StringView(group.name)), group.parenIndices };
    });
}

RegExpRequests collectRegExpRequests(const Typed::Program& program, const Typed::FlowResults& flow, Typed::StringArena& strings, BuildContext& context)
{
    PhaseTimes times(context, PassID::CollectRegExpLiterals);
    CollectionInputs inputs { program, flow, context };
    Vector<FoundRegExp> found = collectInParallel<FoundRegExp>(context.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FoundRegExp>& foundInFunction) {
        collectFromFunction(inputs, FunctionID(index), foundInFunction);
    });
    times.didFinish("collect from functions"_s);
    parallelSort(context.parallelLoops(), found.mutableSpan(), [](const FoundRegExp& a, const FoundRegExp& b) {
        return orderKey(a) < orderKey(b);
    });

    Vector<RegExpRequest> literals;
    Vector<RegExpRequest> candidates;
    for (const FoundRegExp& entry : found) {
        switch (entry.origin) {
        case RegExpRequestOrigin::Literal:
            addToRequests(literals, entry);
            break;
        case RegExpRequestOrigin::Candidate:
            addToRequests(candidates, entry);
            break;
        }
    }
    moveSitesOfRequestsAlsoIn(literals, candidates);

    for (RegExpRequest& request : literals)
        internMetadataStrings(request, program, strings, context);
    for (RegExpRequest& request : candidates)
        internMetadataStrings(request, program, strings, context);

    context.statistics().countChanges(PassID::CollectRegExpLiterals, literals.size() + candidates.size());
    times.didFinish("requests and their strings"_s);
    return RegExpRequests(WTF::move(literals), WTF::move(candidates));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
