/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTComparisonChains.h"
#include "AOTSelectionContext.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SwitchMatch {
    SelectionGroup group;
    ComparisonChain chain;
};

} // anonymous namespace

static void tryAddMatch(SelectionContext& context, SelectionRuleID rule, ComparisonChain&& chain, Vector<SwitchMatch>& matches)
{
    if (chain.comparisons.isEmpty())
        return;

    Vector<SelectionGroup, 8> groups;
    groups.reserveInitialCapacity(chain.comparisons.size());
    for (ValueID comparison : chain.comparisons)
        groups.append(groupOfOne(comparison));
    if (context.tryClaimAll(rule, groups.span()) != ClaimResult::Claimed)
        return;

    matches.append(SwitchMatch {
        .group = WTF::move(groups.first()),
        .chain = WTF::move(chain),
    });
}

static void selectComparisonChains(SelectionContext& context, SelectionRuleID rule, ChainKind kind, LimitID maxCases)
{
    if (context.candidates(Opcode::StrictEq).empty())
        return;

    ChainLimits limits {
        .minCases = context.pass().limit(LimitID::MinSwitchCases),
        .maxCases = std::max<uint64_t>(context.pass().limit(maxCases), 1),
        .maxCasesLimit = maxCases,
    };
    auto mayJoinChain = [&](ValueID comparison) {
        return !context.isClaimed(comparison) && !context.isOnSlowPath(comparison);
    };

    Vector<SwitchMatch> matches;
    for (ComparisonChain& chain : findComparisonChains(context.analyses(), kind, limits, mayJoinChain))
        tryAddMatch(context, rule, WTF::move(chain), matches);

    context.rewriteGroups(rule, matches, [&](GroupRewriter& rewriter, const SwitchMatch& match) {
        convertChainToSwitch(rewriter.builder(), context.analyses(), kind, match.chain);
    });
}

void selectSwitchString(SelectionContext& context)
{
    selectComparisonChains(context, SelectionRuleID::SwitchString, ChainKind::Strings, LimitID::MaxSwitchStringCases);
}

void selectSwitchNumber(SelectionContext& context)
{
    selectComparisonChains(context, SelectionRuleID::SwitchNumber, ChainKind::Numbers, LimitID::MaxSwitchCases);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
