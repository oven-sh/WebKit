/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsInternal.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTSplitUnions.h"
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

// The expansion of a body whose await stands in another block than the call splits that block, and the places of the other sites in it no longer hold.
static bool splitsBlockOf(const TypedResultSite& site, const TypedResultSite& other)
{
    return site.await && site.awaitBlock != site.block && (other.block == site.awaitBlock || other.awaitBlock == site.awaitBlock);
}

static Vector<TypedResultSite, 2> collectSites(FunctionPassContext& context)
{
    Vector<TypedResultSite, 2> sites;
    context.forEachSiteIn(SiteList::TypedResultCalls, [&](const ValueSite& position) {
        if (!mayHaveTypedResult(context.program, context.function.at(position.value)))
            return;
        TypedResultChoice choice = chooseTypedResult(context, position.block, position.index);
        // The next round finds a site that is left out here.
        bool waitsForNextRound = choice.site && sites.containsIf([&](const TypedResultSite& earlier) {
            return splitsBlockOf(earlier, *choice.site) || splitsBlockOf(*choice.site, earlier);
        });
        if (choice.site && !waitsForNextRound)
            sites.append(*choice.site);
        if (choice.declineReason != TypedResultDeclineReason::None)
            context.addDecline(Wire::DeclineKind::TypedResultNotSelected, position.value, choice.declineReason);
    });
    return sites;
}

PassOutcome selectTypedResults(FunctionPassContext& context)
{
    Vector<TypedResultSite, 2> sites = collectSites(context);
    if (sites.isEmpty())
        return PassOutcome::Unchanged;

    EmissionInputs inputs(context);
    IRBuilder builder(inputs.analysisContext(), context.function, context.types, &context.analyses);
    TypedResultExpander expander(builder, inputs, context);
    for (const TypedResultSite& site : sites | std::views::reverse)
        expander.expand(site);

    context.build.statistics().count(StatisticID::TypedResultsSelected, sites.size());
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
