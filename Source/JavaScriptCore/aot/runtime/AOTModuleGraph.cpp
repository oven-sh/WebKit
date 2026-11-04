/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleGraph.h"

#if ENABLE(AOT)

#include "AOTStronglyConnectedComponents.h"
#include "AOTVector.h"
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

// A link follows every request. An evaluation follows the imports: what a module requires runs inside its body.
enum class FollowedRequests : uint8_t { All, Imports };

struct FollowedTargets {
    std::span<const ModuleRequestData> requests;
    uint32_t importer;
    FollowedRequests followed;

    size_t size() const { return requests.size(); }
    // A request that is not followed leads nowhere.
    uint32_t operator[](size_t index) const { return followed == FollowedRequests::Imports && requests[index].isRequire ? importer : requests[index].target(); }
};

struct ModuleGraph {
    std::span<ModuleData> modulesAndEndRecord;
    std::span<const ModuleRequestData> requests;

    unsigned size() const { return static_cast<unsigned>(modulesAndEndRecord.size() - 1); }
    ModuleData& operator[](unsigned module) const { return modulesAndEndRecord[module]; }

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

    // A component has a higher number than every component it requests.
    Typed::StronglyConnectedComponents computeComponents(FollowedRequests followed) const
    {
        return Typed::computeStronglyConnectedComponents(size(), [&](unsigned module) {
            return FollowedTargets { requestsOf(module), module, followed };
        });
    }
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

static Vector<OptionSet<ModuleProperty>> propertiesReachedFromComponents(const ModuleGraph& graph, const Typed::StronglyConnectedComponents& components, FollowedRequests followed)
{
    const Vector<uint32_t>& componentOf = components.componentOf;
    Vector<OptionSet<ModuleProperty>> reached(components.numberOfComponents);
    for (uint32_t module : components.members) {
        OptionSet<ModuleProperty>& properties = reached[componentOf[module]];
        properties.add(ownPropertiesOf(graph, module));
        for (const ModuleRequestData& request : graph.requestsOf(module)) {
            if (!isFollowed(request, followed))
                continue;
            properties.add(reached[componentOf[request.target()]]);
            if (request.isDeferred)
                properties.add(ModuleProperty::HasEvaluationWork);
        }
    }
    return reached;
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
            if (!request.isRequire && !request.isHoisted)
                graph[request.target()].importedByModule = true;
        }
    }

    Typed::StronglyConnectedComponents componentsOfImports = graph.computeComponents(FollowedRequests::Imports);
    Vector<OptionSet<ModuleProperty>> reachedByImports = propertiesReachedFromComponents(graph, componentsOfImports, FollowedRequests::Imports);
    Typed::StronglyConnectedComponents components = graph.computeComponents(FollowedRequests::All);
    Vector<OptionSet<ModuleProperty>> reachedByRequests = propertiesReachedFromComponents(graph, components, FollowedRequests::All);
    for (unsigned index = 0; index < graph.size(); ++index) {
        ModuleData& module = graph[index];
        uint32_t componentOfImports = componentsOfImports.componentOf[index];
        if (!reachedByImports[componentOfImports].contains(ModuleProperty::IsAsynchronous))
            module.add(ModuleData::Flag::IsSynchronous);
        if (reachedByImports[componentOfImports].contains(ModuleProperty::IsUncompiledCode))
            module.add(ModuleData::Flag::ReachesUncompiledCode);
        if (reachedByImports[componentOfImports].contains(ModuleProperty::HasEvaluationWork))
            module.add(ModuleData::Flag::NeedsEvaluation);
        if (module.hasExportTable() && !module.hasTopLevelAwait() && componentsOfImports.membersOf(componentOfImports).size() == 1)
            module.add(ModuleData::Flag::IsSimple);
        if (reachedByRequests[components.componentOf[index]].contains(ModuleProperty::HasLinkWork))
            module.add(ModuleData::Flag::NeedsLink);
    }
}

static Vector<LinkPlanStepData> planLink(const ModuleGraph& graph, ModuleID entryPoint)
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
        if (index == entryPoint.index() || !module.isEntryPoint())
            module.add(ModuleData::Flag::IsLinkedWithEntryPoint);
        path.append(Frame { index, module.firstRequest, graph[index + 1].firstRequest });
    };

    BitVector seen(graph.size());
    seen.quickSet(entryPoint.index());
    visit(entryPoint.index());
    while (!path.isEmpty()) {
        Frame& frame = path.last();
        if (frame.nextRequest != frame.endOfRequests) {
            uint32_t target = graph.requests[frame.nextRequest++].target();
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
    return planLink(graph, entryPoint);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
