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

static Vector<TypedResultSite, 2> collectSites(FunctionPassContext& context)
{
    Vector<TypedResultSite, 2> sites;
    forEachValueSite(context.analyses, context.function, [&](const ValueSite& position) {
        if (!mayHaveTypedResult(context.program, context.function.at(position.value)))
            return;
        TypedResultChoice choice = chooseTypedResult(context, position.block, position.index);
        if (choice.site)
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
