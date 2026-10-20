/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandOwnKeysInternal.h"

#if ENABLE(AOT)

#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PlannedWalk {
    OwnKeyWalkSite site;
    std::optional<OwnKeyWalkPlan> plan;
    std::optional<AssignPlan> assignPlan;
};

} // anonymous namespace

static bool isExpandedByPlan(OwnKeyWalk walk)
{
    switch (walk) {
    case OwnKeyWalk::Values:
    case OwnKeyWalk::Entries:
    case OwnKeyWalk::Assign:
    case OwnKeyWalk::Spread:
        return true;
    case OwnKeyWalk::Stringify:
    case OwnKeyWalk::StringifyIntoText:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<PlannedWalk, 4> planOwnKeyWalks(FunctionPassContext& context)
{
    Vector<OwnKeyWalkSite, 4> sites = findOwnKeyWalkSites(context);
    sites.removeAllMatching([&](const OwnKeyWalkSite& site) {
        return !isWorthExpanding(context, site);
    });
    Vector<OwnKeyWalkSite, 4> sitesExpandedByPlan;
    for (const OwnKeyWalkSite& site : sites) {
        if (isExpandedByPlan(site.walk))
            sitesExpandedByPlan.append(site);
    }
    UnchangedAllocations unchanged(context, sitesExpandedByPlan.span());

    Vector<PlannedWalk, 4> walks;
    for (const OwnKeyWalkSite& site : sites) {
        if (!isExpandedByPlan(site.walk)) {
            walks.append(PlannedWalk { site, std::nullopt, std::nullopt });
            continue;
        }
        if (site.walk == OwnKeyWalk::Assign) {
            if (std::optional<AssignPlan> plan = planAssign(context, unchanged, site))
                walks.append(PlannedWalk { site, std::nullopt, WTF::move(plan) });
            continue;
        }
        if (std::optional<OwnKeyWalkPlan> plan = planOwnKeyWalk(context, unchanged, site))
            walks.append(PlannedWalk { site, WTF::move(plan), std::nullopt });
    }
    return walks;
}

PassOutcome expandOwnKeys(FunctionPassContext& context)
{
    EmissionInputs inputs(context);
    IRBuilder builder(inputs.analysisContext(), context.function, context.types, &context.analyses);
    bool changed = false;

    OwnKeyWalkExpander expander(builder, inputs, context);
    // The sites are listed last first: an expansion moves only the values after its point.
    for (const PlannedWalk& walk : planOwnKeyWalks(context)) {
        uint64_t numberOfWalks = 1;
        if (walk.assignPlan) {
            expander.expandAssign(walk.site, *walk.assignPlan);
            numberOfWalks = walk.assignPlan->walkedSources.size();
        } else if (walk.plan)
            expander.expand(walk.site, *walk.plan);
        else if (!expander.expandStringify(walk.site))
            continue;
        context.build.statistics().count(StatisticID::OwnKeyWalksExpanded, numberOfWalks);
        changed = true;
    }

    changed |= expandForInLoops(context, builder, inputs);
    changed |= foldKeyOrderTests(context);
    return changed ? PassOutcome::Changed : PassOutcome::Unchanged;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
