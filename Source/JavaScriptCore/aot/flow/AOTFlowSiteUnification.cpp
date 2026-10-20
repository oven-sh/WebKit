/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include <algorithm>
#include <span>
#include <utility>
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnionFind.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using MeetingNodeID = ID<struct MeetingNodeIDTag>;

struct MeetingNode {
    MeetingNodeID id;
    FunctionID functionOfLocal;
    AllocationSiteID siteOfPart;

    explicit operator bool() const { return !!id; }
};

class SiteUnifier {
    WTF_MAKE_NONCOPYABLE(SiteUnifier);
public:
    SiteUnifier(const FlowObjectTable&, std::span<const FlowSummary> summariesByFunction);

    Vector<AllocationSiteID> run();

private:
    struct SiteSet : UnionFind<SiteSet> { };

    struct PartCounts {
        uint32_t declared { 0 };
        uint16_t positions { 0 };
    };

    struct Arrival {
        MeetingNode node;
        LayoutID layout;
        AllocationSiteID site;
    };

    using ArrivalsAtPart = Vector<std::pair<LayoutID, AllocationSiteID>, 1>;
    using LinkedSites = Vector<AllocationSiteID, 1>;

    void numberNodes();
    MeetingNode nodeOf(FunctionID, FlowLocalID) const;
    MeetingNode partOf(AllocationSiteID, uint32_t part) const;
    uint32_t partNodeCountOf(AllocationSiteID site) const { return m_firstPartOfSite[site.index() + 1] - m_firstPartOfSite[site.index()]; }
    bool canBeUnited(AllocationSiteID) const;

    void collectExtraEdges();
    void enqueueSeeds();
    void enqueueSeed(const FlowSummary&, const FlowSeed&);
    void enqueue(const MeetingNode&, LayoutID, AllocationSiteID);
    void arrive(const Arrival&);
    void unite(AllocationSiteID, AllocationSiteID);
    void replayArrivals(const MeetingNode& from, const MeetingNode& to);
    Vector<AllocationSiteID> computeRepresentatives();

    void forEachDirectCallEdge(const FlowSummary& caller, const FlowDirectCall& call, NOESCAPE const Invocable<void(const MeetingNode&, const MeetingNode&)> auto& functor) const
    {
        const FlowSummary& callee = m_summaries[call.function.index()];
        if (!callee.hasBody())
            return;

        std::span<const FlowLocalID> arguments = caller.statements.list(call.arguments);
        uint32_t numberOfArguments = std::min(static_cast<uint32_t>(arguments.size()), callee.numberOfEntryParameters);
        for (uint32_t index = 0; index < numberOfArguments; ++index)
            functor(nodeOf(caller.function, arguments[index]), nodeOf(callee.function, callee.entryParameter(index)));

        if (callee.coroutine.resultSite)
            return;

        std::span<const FlowLocalID> results = caller.statements.list(call.results);
        uint32_t numberOfResults = std::min(static_cast<uint32_t>(results.size()), callee.numberOfResults);
        for (uint32_t index = 0; index < numberOfResults; ++index)
            functor(nodeOf(callee.function, callee.result(index)), nodeOf(caller.function, results[index]));
        functor(nodeOf(callee.function, callee.thrown()), nodeOf(caller.function, call.thrown));
    }

    void forEachExtraEdge(NOESCAPE const Invocable<void(const MeetingNode&, const MeetingNode&)> auto& functor) const
    {
        auto visit = [&](const MeetingNode& from, const MeetingNode& to) {
            if (from && to)
                functor(from, to);
        };
        for (const FlowSummary& summary : m_summaries) {
            for (auto [from, to] : summary.copiesFromExternals)
                visit(nodeOf(summary.function, from), nodeOf(summary.function, to));
            for (const FlowFilter& filter : summary.statements.filters)
                visit(nodeOf(summary.function, filter.from), nodeOf(summary.function, filter.to));
            for (const FlowDirectCall& call : summary.statements.directCalls)
                forEachDirectCallEdge(summary, call, visit);
        }
    }

    void forEachSuccessor(const MeetingNode& node, NOESCAPE const Invocable<void(const MeetingNode&)> auto& functor) const
    {
        if (FunctionID function = node.functionOfLocal) {
            FlowLocalID local(node.id.index() - m_firstLocalOfFunction[function.index()]);
            for (FlowLocalID target : m_summaries[function.index()].successors.of(local))
                functor(nodeOf(function, target));
        }

        uint32_t firstExtraEdge = m_firstExtraEdge[node.id.index()];
        for (const MeetingNode& target : m_extraEdgeTargets.subspan(firstExtraEdge, m_firstExtraEdge[node.id.index() + 1] - firstExtraEdge))
            functor(target);

        if (!node.siteOfPart)
            return;
        auto linkedSites = m_linkedSites.find(node.siteOfPart);
        if (linkedSites == m_linkedSites.end())
            return;
        uint32_t part = node.id.index() - m_firstPartOfSite[node.siteOfPart.index()];
        for (AllocationSiteID linkedSite : linkedSites->value)
            functor(partOf(linkedSite, part));
    }

    const FlowObjectTable& m_table;
    const Program& m_program;
    std::span<const FlowSummary> m_summaries;

    Vector<uint32_t> m_firstLocalOfFunction;
    uint32_t m_firstVariable { 0 };
    uint32_t m_firstHeritage { 0 };
    Vector<uint32_t> m_firstPartOfSite;
    Vector<PartCounts> m_partCounts;
    Vector<uint32_t> m_siteCountByLayout;
    uint32_t m_numberOfNodes { 0 };

    Vector<uint32_t> m_firstExtraEdge;
    Vector<MeetingNode> m_extraEdgeTargets;
    HashMap<AllocationSiteID, LinkedSites> m_linkedSites;

    Vector<SiteSet> m_sets;
    HashMap<std::pair<MeetingNodeID, LayoutID>, AllocationSiteID> m_reached;
    HashMap<MeetingNodeID, ArrivalsAtPart> m_arrivalsAtParts;
    Deque<Arrival> m_pending;
};

SiteUnifier::SiteUnifier(const FlowObjectTable& table, std::span<const FlowSummary> summariesByFunction)
    : m_table(table)
    , m_program(table.program())
    , m_summaries(summariesByFunction)
    , m_siteCountByLayout(FillWith { }, table.program().layouts.size(), 0u)
    , m_sets(table.numberOfSites())
{
}

void SiteUnifier::numberNodes()
{
    uint64_t numberOfNodes = 0;

    m_firstLocalOfFunction.reserveInitialCapacity(m_summaries.size() + 1);
    for (const FlowSummary& summary : m_summaries) {
        m_firstLocalOfFunction.append(static_cast<uint32_t>(numberOfNodes));
        numberOfNodes += summary.numberOfLocals;
    }
    m_firstLocalOfFunction.append(static_cast<uint32_t>(numberOfNodes));

    m_firstVariable = static_cast<uint32_t>(numberOfNodes);
    numberOfNodes += m_program.variables.size();
    m_firstHeritage = static_cast<uint32_t>(numberOfNodes);
    numberOfNodes += m_program.layouts.size();

    unsigned numberOfSites = m_table.numberOfSites();
    m_firstPartOfSite.reserveInitialCapacity(numberOfSites + 1);
    m_partCounts.reserveInitialCapacity(numberOfSites);
    for (unsigned index = 0; index < numberOfSites; ++index) {
        const FlowSiteDescription& description = m_table.at(AllocationSiteID(index)).description;
        FlowObject record = makeFlowObject(m_program, description);
        m_firstPartOfSite.append(static_cast<uint32_t>(numberOfNodes));
        m_partCounts.append(PartCounts { record.numberOfDeclaredParts, record.numberOfPositions });
        numberOfNodes += record.numberOfParts();
        if (description.kind == FlowSiteKind::Layout)
            ++m_siteCountByLayout[description.layout.index()];
    }
    m_firstPartOfSite.append(static_cast<uint32_t>(numberOfNodes));

    RELEASE_ASSERT(numberOfNodes < UINT32_MAX - 1);
    m_numberOfNodes = static_cast<uint32_t>(numberOfNodes);
}

MeetingNode SiteUnifier::nodeOf(FunctionID function, FlowLocalID local) const
{
    if (!local)
        return { };

    const FlowSummary& summary = m_summaries[function.index()];
    if (!summary.isExternal(local))
        return { MeetingNodeID(m_firstLocalOfFunction[function.index()] + local.index()), function, { } };

    const FlowExternalNode& external = summary.externalAt(local);
    AllocationSiteID site = external.isPartOfSite() ? m_table.siteOf(function, external.site()) : AllocationSiteID();
    switch (external.kind()) {
    case FlowExternalKind::Variable:
        return { MeetingNodeID(m_firstVariable + external.variable().index()), { }, { } };
    case FlowExternalKind::Heritage:
        return { MeetingNodeID(m_firstHeritage + external.layout().index()), { }, { } };
    case FlowExternalKind::NativeCode:
    case FlowExternalKind::UnknownJavaScript:
        return { };
    case FlowExternalKind::SiteField:
    case FlowExternalKind::SiteCapture:
        return partOf(site, external.index());
    case FlowExternalKind::SiteSlot:
        return partOf(site, m_program.builtins().at(external.slot()).indexInClass);
    case FlowExternalKind::SitePosition:
        return partOf(site, m_partCounts[site.index()].declared + external.index());
    case FlowExternalKind::SiteUndeclaredPart:
        return partOf(site, m_partCounts[site.index()].declared + m_partCounts[site.index()].positions);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

MeetingNode SiteUnifier::partOf(AllocationSiteID site, uint32_t part) const
{
    RELEASE_ASSERT(part < partNodeCountOf(site));
    return { MeetingNodeID(m_firstPartOfSite[site.index()] + part), { }, site };
}

bool SiteUnifier::canBeUnited(AllocationSiteID site) const
{
    const FlowSiteDescription& description = m_table.at(site).description;
    return description.kind == FlowSiteKind::Layout && m_siteCountByLayout[description.layout.index()] > 1;
}

void SiteUnifier::collectExtraEdges()
{
    m_firstExtraEdge = Vector<uint32_t>(FillWith { }, static_cast<size_t>(m_numberOfNodes) + 2, 0u);
    forEachExtraEdge([&](const MeetingNode& from, const MeetingNode&) {
        ++m_firstExtraEdge[from.id.index() + 2];
    });
    for (size_t index = 2; index < m_firstExtraEdge.size(); ++index)
        m_firstExtraEdge[index] += m_firstExtraEdge[index - 1];

    // Entry n + 1 is the cursor of node n while the targets are filled in, and ends up as the first edge of node n + 1.
    m_extraEdgeTargets.grow(m_firstExtraEdge.last());
    forEachExtraEdge([&](const MeetingNode& from, const MeetingNode& to) {
        m_extraEdgeTargets[m_firstExtraEdge[from.id.index() + 1]++] = to;
    });
}

void SiteUnifier::enqueueSeeds()
{
    for (const FlowSummary& summary : m_summaries) {
        for (const FlowSeed& seed : summary.seeds)
            enqueueSeed(summary, seed);
        for (const FlowConditionalSeed& conditionalSeed : summary.statements.conditionalSeeds)
            enqueueSeed(summary, conditionalSeed.seed);
    }
}

void SiteUnifier::enqueueSeed(const FlowSummary& summary, const FlowSeed& seed)
{
    if (seed.kind() != FlowSeedKind::Site)
        return;

    AllocationSiteID site = m_table.siteOf(summary.function, seed.site());
    if (canBeUnited(site))
        enqueue(nodeOf(summary.function, seed.node()), m_table.at(site).description.layout, site);
}

void SiteUnifier::enqueue(const MeetingNode& node, LayoutID layout, AllocationSiteID site)
{
    if (node)
        m_pending.append(Arrival { node, layout, site });
}

void SiteUnifier::arrive(const Arrival& arrival)
{
    auto result = m_reached.add(std::pair { arrival.node.id, arrival.layout }, arrival.site);
    if (!result.isNewEntry) {
        AllocationSiteID firstSite = result.iterator->value;
        unite(firstSite, arrival.site);
        return;
    }

    if (arrival.node.siteOfPart && canBeUnited(arrival.node.siteOfPart)) {
        m_arrivalsAtParts.ensure(arrival.node.id, [] {
            return ArrivalsAtPart();
        }).iterator->value.append({ arrival.layout, arrival.site });
    }

    forEachSuccessor(arrival.node, [&](const MeetingNode& target) {
        enqueue(target, arrival.layout, arrival.site);
    });
}

void SiteUnifier::unite(AllocationSiteID a, AllocationSiteID b)
{
    if (m_sets[a.index()].find() == m_sets[b.index()].find())
        return;
    m_sets[a.index()].unify(&m_sets[b.index()]);

    auto addLink = [&](AllocationSiteID from, AllocationSiteID to) {
        m_linkedSites.ensure(from, [] {
            return LinkedSites();
        }).iterator->value.append(to);
    };
    addLink(a, b);
    addLink(b, a);

    uint32_t partNodeCount = partNodeCountOf(a);
    RELEASE_ASSERT(partNodeCount == partNodeCountOf(b));
    for (uint32_t part = 0; part < partNodeCount; ++part) {
        replayArrivals(partOf(a, part), partOf(b, part));
        replayArrivals(partOf(b, part), partOf(a, part));
    }
}

void SiteUnifier::replayArrivals(const MeetingNode& from, const MeetingNode& to)
{
    auto arrivals = m_arrivalsAtParts.find(from.id);
    if (arrivals == m_arrivalsAtParts.end())
        return;
    for (auto [layout, site] : arrivals->value)
        enqueue(to, layout, site);
}

Vector<AllocationSiteID> SiteUnifier::computeRepresentatives()
{
    size_t numberOfSites = m_sets.size();
    Vector<AllocationSiteID> lowestOfRoot(numberOfSites);
    Vector<AllocationSiteID> representatives(numberOfSites);
    for (size_t index = 0; index < numberOfSites; ++index) {
        AllocationSiteID& lowest = lowestOfRoot[m_sets[index].find() - m_sets.begin()];
        if (!lowest)
            lowest = AllocationSiteID(index);
        representatives[index] = lowest;
    }
    return representatives;
}

Vector<AllocationSiteID> SiteUnifier::run()
{
    numberNodes();
    collectExtraEdges();
    enqueueSeeds();
    while (!m_pending.isEmpty())
        arrive(m_pending.takeFirst());
    return computeRepresentatives();
}

} // namespace

Vector<AllocationSiteID> uniteSites(const FlowObjectTable& table, std::span<const FlowSummary> summariesByFunction)
{
    return SiteUnifier(table, summariesByFunction).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
