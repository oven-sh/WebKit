/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleGraph.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

namespace {

enum class ModuleProperty : uint8_t {
    IsAsynchronous = 1 << 0,
    HasLinkWork = 1 << 1,
    IsUncompiledCode = 1 << 2,
    HasEvaluationWork = 1 << 3,
};

struct ComponentSummary {
    OptionSet<ModuleProperty> reachedProperties;
    uint32_t numberOfModules { 0 };
};

struct RequestTargets {
    std::span<const ModuleRequestData> requests;

    size_t size() const { return requests.size(); }
    uint32_t operator[](size_t index) const { return requests[index].target(); }
};

struct ImportTargets {
    std::span<const ModuleRequestData> requests;
    uint32_t importer;

    size_t size() const { return requests.size(); }
    // A require() leads nowhere.
    uint32_t operator[](size_t index) const { return requests[index].isRequire ? importer : requests[index].target(); }
};

struct ModuleGraph {
    std::span<ModuleData> modulesAndEndRecord;
    std::span<const ModuleRequestData> requests;

    unsigned size() const { return static_cast<unsigned>(modulesAndEndRecord.size() - 1); }
    ModuleData& operator[](unsigned module) const { return modulesAndEndRecord[module]; }
    ModuleData& endRecord() const { return modulesAndEndRecord.back(); }

    std::span<const ModuleRequestData> requestsOf(unsigned module) const
    {
        uint32_t first = modulesAndEndRecord[module].firstRequest;
        return requests.subspan(first, modulesAndEndRecord[module + 1].firstRequest - first);
    }

    // The synthetic exports of a module that captures exports are the names it captures.
    bool resolvesNames(unsigned module) const
    {
        const ModuleData& record = modulesAndEndRecord[module];
        const ModuleData& nextRecord = modulesAndEndRecord[module + 1];
        if (record.capturesExports())
            return false;
        return record.firstSyntheticExport != nextRecord.firstSyntheticExport || record.firstRequiredName != nextRecord.firstRequiredName;
    }

    bool hasRequire() const
    {
        return std::ranges::any_of(requests, [](const ModuleRequestData& request) {
            return !!request.isRequire;
        });
    }

    // A component has a higher number than every component it requests. A link follows every request.
    Typed::StronglyConnectedComponents computeComponents() const
    {
        return Typed::computeStronglyConnectedComponents(size(), [&](unsigned module) {
            return RequestTargets { requestsOf(module) };
        });
    }

    // An evaluation follows the imports: what a module requires runs inside its body.
    Typed::StronglyConnectedComponents computeComponentsOfImports() const
    {
        return Typed::computeStronglyConnectedComponents(size(), [&](unsigned module) {
            return ImportTargets { requestsOf(module), module };
        });
    }
};

enum class FollowedRequests : uint8_t { All, Imports };

enum class RoleInWalk : uint8_t {
    NeverVisited,
    VisitedWithoutList,
    Visited,
    OnlyPassesWalkOn,
    Contracted,
};

struct ListRange {
    uint32_t start { 0 };
    uint32_t size { 0 };
};

} // anonymous namespace

static OptionSet<ModuleProperty> ownPropertiesOf(const ModuleGraph& graph, unsigned index)
{
    const ModuleData& module = graph[index];
    OptionSet<ModuleProperty> properties;
    if (module.hasTopLevelAwait() || module.isUncompiledCode())
        properties.add(ModuleProperty::IsAsynchronous);
    if (module.capturesExports() || module.isUncompiledCode() || graph.resolvesNames(index))
        properties.add(ModuleProperty::HasLinkWork);
    if (module.isUncompiledCode())
        properties.add(ModuleProperty::IsUncompiledCode);
    // A module of the library runs when its importer is linked.
    bool isFileOfProgram = module.hasExportTable() || module.kind == ModuleKind::CommonJs;
    if (module.isUncompiledCode() || (isFileOfProgram && (module.bodyFunction() || module.hasTopLevelAwait())))
        properties.add(ModuleProperty::HasEvaluationWork);
    return properties;
}

static bool isFollowed(const ModuleRequestData& request, FollowedRequests followed)
{
    return followed == FollowedRequests::All || !request.isRequire;
}

static Vector<ComponentSummary> summarizeComponents(const ModuleGraph& graph, const Typed::StronglyConnectedComponents& components, FollowedRequests followed)
{
    const Vector<uint32_t>& componentOf = components.componentOf;
    Vector<ComponentSummary> summaries(components.numberOfComponents);
    for (uint32_t module : components.members) {
        ComponentSummary& summary = summaries[componentOf[module]];
        ++summary.numberOfModules;
        summary.reachedProperties.add(ownPropertiesOf(graph, module));
        for (const ModuleRequestData& request : graph.requestsOf(module)) {
            if (!isFollowed(request, followed))
                continue;
            summary.reachedProperties.add(summaries[componentOf[request.target()]].reachedProperties);
            if (request.isDeferred)
                summary.reachedProperties.add(ModuleProperty::HasEvaluationWork);
        }
    }
    return summaries;
}

static void addComponentFlags(ModuleData& module, const ComponentSummary& component)
{
    if (!component.reachedProperties.contains(ModuleProperty::IsAsynchronous))
        module.add(ModuleData::Flag::IsSynchronous);
    if (component.reachedProperties.contains(ModuleProperty::IsUncompiledCode))
        module.add(ModuleData::Flag::ReachesUncompiledCode);
    if (component.reachedProperties.contains(ModuleProperty::HasEvaluationWork))
        module.add(ModuleData::Flag::NeedsEvaluation);
    if (module.hasExportTable() && !module.hasTopLevelAwait() && component.numberOfModules == 1)
        module.add(ModuleData::Flag::IsSimple);
}

void computeModuleFlags(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests)
{
    RELEASE_ASSERT(!modulesAndEndRecord.empty());
    ModuleGraph graph { modulesAndEndRecord, requests };
    for (unsigned module = 0; module < graph.size(); ++module) {
        for (const ModuleRequestData& request : graph.requestsOf(module)) {
            if (request.isDynamicStarExport)
                graph[module].add(ModuleData::Flag::HasDynamicStarExport);
            if (request.isDeferred)
                graph[module].add(ModuleData::Flag::HasDeferredRequest);
        }
    }

    auto addNeedsLink = [&](const Typed::StronglyConnectedComponents& components, const Vector<ComponentSummary>& summaries) {
        for (unsigned module = 0; module < graph.size(); ++module) {
            if (summaries[components.componentOf[module]].reachedProperties.contains(ModuleProperty::HasLinkWork))
                graph[module].add(ModuleData::Flag::NeedsLink);
        }
    };

    Typed::StronglyConnectedComponents componentsOfImports = graph.computeComponentsOfImports();
    Vector<ComponentSummary> reachedByImports = summarizeComponents(graph, componentsOfImports, FollowedRequests::Imports);
    for (unsigned module = 0; module < graph.size(); ++module)
        addComponentFlags(graph[module], reachedByImports[componentsOfImports.componentOf[module]]);
    if (!graph.hasRequire()) {
        addNeedsLink(componentsOfImports, reachedByImports);
        return;
    }
    Typed::StronglyConnectedComponents components = graph.computeComponents();
    addNeedsLink(components, summarizeComponents(graph, components, FollowedRequests::All));
}

// An uncompiled module can import back a module that the table shows no cycle for, and its link asks which modules are on the path.
static RoleInWalk roleInLink(const ModuleGraph& graph, const Typed::StronglyConnectedComponents& components, unsigned index)
{
    const ModuleData& module = graph[index];
    if (!module.needsLink())
        return RoleInWalk::NeverVisited;
    // A file of the program that captures exports requests other files.
    if (module.capturesExportsBehindEvaluation())
        return RoleInWalk::Visited;
    if (module.capturesExports() || module.isUncompiledCode())
        return RoleInWalk::VisitedWithoutList;
    bool isAloneInComponent = components.membersOf(components.componentOf[index]).size() == 1;
    if (isAloneInComponent && !module.hasTopLevelAwait() && !graph.resolvesNames(index) && !module.reachesUncompiledCode())
        return RoleInWalk::OnlyPassesWalkOn;
    return RoleInWalk::Visited;
}

// What a module with a deferred request evaluates depends on what has been evaluated by then.
static RoleInWalk roleInEvaluation(const ModuleGraph& graph, const BitVector& modulesNamedByRequire, unsigned index)
{
    const ModuleData& module = graph[index];
    if (!module.needsEvaluation())
        return RoleInWalk::NeverVisited;
    // What a CommonJS module requests runs inside its body.
    if (module.isUncompiledCode() || module.kind == ModuleKind::CommonJs)
        return RoleInWalk::VisitedWithoutList;
    if (!module.hasExportTable())
        return RoleInWalk::NeverVisited;
    if (module.hasDeferredRequest())
        return RoleInWalk::VisitedWithoutList;
    if (module.isSimple() && module.isSynchronous() && !module.bodyFunction() && !modulesNamedByRequire.get(index))
        return RoleInWalk::OnlyPassesWalkOn;
    return RoleInWalk::Visited;
}

// A module that only passes the walk on is alone in its component, so nothing it reaches requests it.
template<typename RoleFunctor>
static Vector<uint32_t> contractRequests(const ModuleGraph& graph, std::span<const uint32_t> modulesBehindTheirRequests, FollowedRequests followed, uint32_t maxContractedRequests, Number24 ModuleData::* firstRequest, Vector<uint32_t>& modulesOverLimit, const RoleFunctor& roleOf)
{
    unsigned numberOfModules = graph.size();
    Vector<RoleInWalk> roles(numberOfModules, [&](size_t module) {
        return roleOf(static_cast<unsigned>(module));
    });
    Vector<ListRange> ranges(numberOfModules);
    Vector<uint32_t> lastImporter(FillWith { }, numberOfModules, UINT32_MAX);
    Vector<uint32_t> lists;

    for (uint32_t module : modulesBehindTheirRequests) {
        if (roles[module] != RoleInWalk::Visited && roles[module] != RoleInWalk::OnlyPassesWalkOn)
            continue;

        uint32_t start = lists.size();
        auto appendOnce = [&](uint32_t target) {
            if (lastImporter[target] == module)
                return;
            lastImporter[target] = module;
            lists.append(target);
        };
        for (const ModuleRequestData& request : graph.requestsOf(module)) {
            uint32_t target = request.target();
            if (target == module || !isFollowed(request, followed))
                continue;
            switch (roles[target]) {
            case RoleInWalk::NeverVisited:
                continue;
            case RoleInWalk::VisitedWithoutList:
            case RoleInWalk::Visited:
                appendOnce(target);
                continue;
            case RoleInWalk::Contracted:
                for (uint32_t offset = 0; offset < ranges[target].size; ++offset)
                    appendOnce(lists[ranges[target].start + offset]);
                continue;
            case RoleInWalk::OnlyPassesWalkOn:
                break;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
        ranges[module] = { start, static_cast<uint32_t>(lists.size()) - start };

        if (roles[module] != RoleInWalk::OnlyPassesWalkOn)
            continue;
        if (ranges[module].size <= maxContractedRequests) {
            roles[module] = RoleInWalk::Contracted;
            continue;
        }
        roles[module] = RoleInWalk::Visited;
        modulesOverLimit.append(module);
    }

    Vector<uint32_t> listsByModule;
    listsByModule.reserveInitialCapacity(lists.size());
    for (unsigned module = 0; module < numberOfModules; ++module) {
        graph[module].*firstRequest = Number24(listsByModule.size());
        listsByModule.append(lists.subspan(ranges[module].start, ranges[module].size));
    }
    graph.endRecord().*firstRequest = Number24(listsByModule.size());
    return listsByModule;
}

ContractedModuleRequests contractModuleRequests(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, const BitVector& modulesNamedByRequire, uint32_t maxContractedRequests)
{
    RELEASE_ASSERT(!modulesAndEndRecord.empty());
    ModuleGraph graph { modulesAndEndRecord, requests };
    Typed::StronglyConnectedComponents components = graph.computeComponents();
    Typed::StronglyConnectedComponents componentsOfImports = graph.computeComponentsOfImports();

    ContractedModuleRequests result;
    result.linkRequests = contractRequests(graph, components.members.span(), FollowedRequests::All, maxContractedRequests, &ModuleData::firstLinkRequest, result.modulesOverLimit, [&](unsigned module) {
        return roleInLink(graph, components, module);
    });
    result.evaluationRequests = contractRequests(graph, componentsOfImports.members.span(), FollowedRequests::Imports, maxContractedRequests, &ModuleData::firstEvaluationRequest, result.modulesOverLimit, [&](unsigned module) {
        return roleInEvaluation(graph, modulesNamedByRequire, module);
    });
    return result;
}

enum class PlannedRoot : uint8_t { EntryPoint, OtherModule };

static Vector<LinkPlanStepData> planLink(ModuleGraph& graph, std::span<const ModuleRequestData> requests, ModuleID entryPoint, PlannedRoot plannedRoot)
{
    struct Frame {
        uint32_t module;
        uint32_t nextRequest;
        uint32_t endOfRequests;
    };

    RELEASE_ASSERT(entryPoint.index() < graph.size());
    Vector<LinkPlanStepData> plan;
    const ModuleData& root = graph[entryPoint.index()];
    if (!root.needsLink() || root.capturesExports() || root.reachesUncompiledCode())
        return plan;

    Vector<Frame, 64> path;
    auto visit = [&](uint32_t index) {
        ModuleData& module = graph[index];
        if (!module.needsLink())
            return;
        RELEASE_ASSERT(!module.isUncompiledCode());
        if (module.capturesExports()) {
            plan.append(LinkPlanStepData { index, false });
            if (!module.capturesExportsBehindEvaluation())
                return;
        }
        if (plannedRoot == PlannedRoot::EntryPoint && (index == entryPoint.index() || !module.isEntryPoint()))
            module.add(ModuleData::Flag::IsLinkedWithEntryPoint);
        path.append(Frame { index, module.firstRequest, graph[index + 1].firstRequest });
    };

    BitVector seen(graph.size());
    seen.quickSet(entryPoint.index());
    visit(entryPoint.index());
    while (!path.isEmpty()) {
        Frame& frame = path.last();
        if (frame.nextRequest != frame.endOfRequests) {
            uint32_t target = requests[frame.nextRequest++].target();
            if (!seen.quickSet(target))
                visit(target);
            continue;
        }
        if (graph.resolvesNames(frame.module))
            plan.append(LinkPlanStepData { frame.module, true });
        path.removeLast();
    }
    return plan;
}

Vector<LinkPlanStepData> planLinkOfEntryPoint(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, ModuleID entryPoint)
{
    RELEASE_ASSERT(!modulesAndEndRecord.empty());
    ModuleGraph graph { modulesAndEndRecord, requests };
    return planLink(graph, requests, entryPoint, PlannedRoot::EntryPoint);
}

LinkPlansOfRoots planLinksOfRoots(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, std::span<const ModuleImportSite> unsortedImportSites, std::optional<ModuleID> plannedEntryPoint)
{
    struct Cycle {
        uint32_t numberOfModules { 0 };
        uint32_t component { 0 };
    };
    // Below this a walk costs about what the steps of a plan cost.
    AOT_THRESHOLD(uint32_t, minModulesInCycle, 64);

    RELEASE_ASSERT(!modulesAndEndRecord.empty());
    ModuleGraph graph { modulesAndEndRecord, requests };
    Typed::StronglyConnectedComponents components = graph.computeComponents();
    const Vector<uint32_t>& componentOf = components.componentOf;

    Vector<Cycle> largestCycleReachedFrom(components.numberOfComponents);
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        std::span<const uint32_t> members = components.membersOf(component);
        Cycle largest;
        if (graph[members.front()].needsLink())
            largest = { static_cast<uint32_t>(members.size()), component };
        for (uint32_t member : members) {
            for (const ModuleRequestData& request : graph.requestsOf(member)) {
                const Cycle& reached = largestCycleReachedFrom[componentOf[request.target()]];
                if (reached.numberOfModules > largest.numberOfModules)
                    largest = reached;
            }
        }
        largestCycleReachedFrom[component] = largest;
    }

    Vector<ModuleImportSite> importSites(unsortedImportSites);
    AOT::sort(importSites, { }, [](const ModuleImportSite& site) {
        return std::pair { site.target, site.importer };
    });
    BitVector isRequested(graph.size());
    for (const ModuleRequestData& request : requests)
        isRequested.quickSet(request.target());

    LinkPlansOfRoots plans;
    // The plans of roots that reach the same cycle repeat each other. All of them together get four steps for each module.
    size_t maxNumberOfSteps = 4 * static_cast<size_t>(graph.size());
    for (unsigned index = 0; index < graph.size(); ++index) {
        const Cycle& cycle = largestCycleReachedFrom[componentOf[index]];
        if (cycle.numberOfModules < minModulesInCycle || !graph[index].hasExportTable() || (plannedEntryPoint && plannedEntryPoint->index() == index))
            continue;
        auto sites = std::ranges::equal_range(importSites, index, { }, &ModuleImportSite::target);
        if (sites.empty() && isRequested.quickGet(index))
            continue;
        // The code of a module of the cycle runs when the cycle is linked.
        bool isNamedOnlyInsideCycle = !sites.empty() && std::ranges::all_of(sites, [&](const ModuleImportSite& site) {
            return componentOf[site.importer] == cycle.component;
        });
        if (isNamedOnlyInsideCycle)
            continue;
        Vector<LinkPlanStepData> plan = planLink(graph, requests, ModuleID(index), PlannedRoot::OtherModule);
        if (plan.isEmpty() || plans.steps.size() + plan.size() > maxNumberOfSteps)
            continue;
        plans.roots.append(LinkPlanRootData { index, static_cast<uint32_t>(plans.steps.size()), std::ranges::min(components.membersOf(cycle.component)) });
        plans.steps.appendVector(plan);
    }
    if (!plans.roots.isEmpty())
        plans.roots.append(LinkPlanRootData { graph.size(), static_cast<uint32_t>(plans.steps.size()), 0 });
    return plans;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
