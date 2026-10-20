/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTGrouping.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SiteGroups {
    Vector<uint32_t> firstMember;
    Vector<AllocationSiteID> members;

    std::span<const AllocationSiteID> membersOf(AllocationSiteID representative) const LIFETIME_BOUND { return entriesOfGroup(firstMember, members, representative.index()); }
};

} // namespace

static void numberSites(FlowObjectTable& table, std::span<const FlowSummary> summariesByFunction)
{
    Vector<uint32_t> firstSiteByFunction;
    firstSiteByFunction.reserveInitialCapacity(summariesByFunction.size() + 1);
    for (const FlowSummary& summary : summariesByFunction) {
        RELEASE_ASSERT(summary.function.index() == firstSiteByFunction.size());
        firstSiteByFunction.append(table.numberOfSites());
        for (const FlowSiteDescription& description : summary.sites)
            table.appendSite(FlowSite { summary.function, description, { } });
    }
    firstSiteByFunction.append(table.numberOfSites());
    table.setFirstSiteOfFunctions(WTF::move(firstSiteByFunction));
}

static SiteGroups groupSites(std::span<const AllocationSiteID> representatives)
{
    SiteGroups groups;
    groupByIndex(representatives.size(), groups.firstMember, groups.members, [&](const auto& visit) {
        for (size_t index = 0; index < representatives.size(); ++index)
            visit(representatives[index].index(), AllocationSiteID(index));
    });
    return groups;
}

static void addCollapsedSites(FlowObjectTable& table, std::span<const AllocationSiteID> representatives, const BitVector& collapsedGroups)
{
    if (collapsedGroups.isEmpty())
        return;

    for (size_t index = 0; index < representatives.size(); ++index) {
        if (!collapsedGroups.get(representatives[index].index()))
            continue;
        AllocationSiteID site(index);
        if (FlowObjectID layoutObject = table.layoutObject(table.at(site).description.layout))
            addSiteToLayoutObject(table, site, layoutObject);
        else
            table.mutableAt(site).sharedObject = unknownFlowObject;
    }
}

void createAllocationClasses(FlowObjectTable& table, std::span<const FlowSummary> summariesByFunction, BuildContext& build)
{
    const Program& program = table.program();
    RELEASE_ASSERT(summariesByFunction.size() == program.functions.size() + 1);
    RELEASE_ASSERT(table.size() == 1 && !table.numberOfSites());
    table.indexes().build = &build;

    numberSites(table, summariesByFunction);
    Vector<AllocationSiteID> representatives = uniteSites(table, summariesByFunction);
    RELEASE_ASSERT(representatives.size() == table.numberOfSites());
    SiteGroups groups = groupSites(representatives.span());

    auto appendToTable = [&](const FlowObject& record) {
        return table.append(record);
    };
    FlowObjectAppender append = appendToTable;
    uint64_t limit = build.limit(LimitID::MaxSitesPerLayout);
    Vector<uint32_t> classCountByLayout(FillWith { }, program.layouts.size(), 0u);
    BitVector collapsedGroups(representatives.size());

    for (size_t index = 0; index < representatives.size(); ++index) {
        AllocationSiteID site(index);
        if (representatives[index] != site)
            continue;

        FlowSiteDescription description = table.at(site).description;
        if (description.kind == FlowSiteKind::Layout) {
            uint32_t& classCount = classCountByLayout[description.layout.index()];
            if (classCount >= limit) {
                ensureLayoutObject(table, build, description.layout, append);
                collapsedGroups.set(index);
                continue;
            }
            ++classCount;
        }
        appendAllocationClassFor(table, groups.membersOf(site), append);
    }

    addCollapsedSites(table, representatives.span(), collapsedGroups);
}

FlowObjectID appendAllocationClassFor(FlowObjectTable& table, std::span<const AllocationSiteID> ascendingSites, NOESCAPE const FlowObjectAppender& append)
{
    RELEASE_ASSERT(!ascendingSites.empty());
    FlowSiteDescription description = table.at(ascendingSites[0]).description;

    FlowObject record = makeFlowObject(table.program(), description);
    record.site = ascendingSites[0];
    FlowObjectID object = append(record);

    if (object != unknownFlowObject) {
        FlowAllocationClassMembers members = makeAllocationClassMembers(description);
        members.sharedObject = object;
        members.sites.append(ascendingSites);
        AllocationClassID allocationClass = table.appendAllocationClass(WTF::move(members));
        table.mutableAt(object).allocationClass = allocationClass;
    }

    for (AllocationSiteID site : ascendingSites)
        table.mutableAt(site).sharedObject = object;
    return object;
}

FlowObjectID ensureLayoutObject(FlowObjectTable& table, BuildContext& build, LayoutID layout, NOESCAPE const FlowObjectAppender& append)
{
    if (FlowObjectID existing = table.layoutObject(layout))
        return existing;

    build.gaveUp(LimitID::MaxSitesPerLayout, Subject { .layout = layout });

    FlowSiteDescription description;
    description.kind = FlowSiteKind::Layout;
    description.layout = layout;

    FlowObject record = makeFlowObject(table.program(), description);
    record.standsForLayout = true;
    FlowObjectID object = append(record);
    if (object == unknownFlowObject)
        return object;

    FlowAllocationClassMembers members = makeAllocationClassMembers(description);
    members.sharedObject = object;
    AllocationClassID allocationClass = table.appendAllocationClass(WTF::move(members));
    table.mutableAt(object).allocationClass = allocationClass;
    return object;
}

void addSiteToLayoutObject(FlowObjectTable& table, AllocationSiteID site, FlowObjectID layoutObject)
{
    FlowObject& record = table.mutableAt(layoutObject);
    table.mutableAt(record.allocationClass).sites.append(site);
    table.mutableAt(site).sharedObject = layoutObject;
    if (!record.site)
        record.site = site;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
