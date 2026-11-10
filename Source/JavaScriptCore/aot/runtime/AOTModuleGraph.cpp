/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleGraph.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
#include <ranges>
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

enum class FollowedRequests : uint8_t { All, Imports };

struct FollowedTargets {
    std::span<const ModuleRequestData> requests;
    uint32_t importer;
    FollowedRequests followed;

    size_t size() const { return requests.size(); }
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

    bool resolvesNames(unsigned module) const
    {
        const ModuleData& record = modulesAndEndRecord[module];
        const ModuleData& nextRecord = modulesAndEndRecord[module + 1];
        if (record.capturesExports())
            return false;
        return record.firstSyntheticExport != nextRecord.firstSyntheticExport || record.firstRequiredName != nextRecord.firstRequiredName;
    }

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

Typed::StronglyConnectedComponents computeModuleFlags(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests)
{
    RELEASE_ASSERT(!modulesAndEndRecord.empty());
    ModuleGraph graph { modulesAndEndRecord, requests };
    for (unsigned module = 0; module < graph.size(); ++module) {
        for (const ModuleRequestData& request : graph.requestsOf(module)) {
            if (request.isDynamicStarExport && graph[module].kind != ModuleKind::CommonJs)
                graph[module].add(ModuleData::Flag::HasDynamicStarExport);
            if (request.isDeferred && graph[module].kind != ModuleKind::CommonJs)
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
    return components;
}

LinkRequestsOfModules contractLinkRequests(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, const Typed::StronglyConnectedComponents& components)
{
    constexpr uint32_t none = UINT32_MAX;
    constexpr uint32_t several = UINT32_MAX - 1;
    RELEASE_ASSERT(!modulesAndEndRecord.empty());
    ModuleGraph graph { modulesAndEndRecord, requests };
    uint32_t numberOfModules = graph.size();

    auto mayOnlyPassLinkOn = [&](uint32_t index) {
        const ModuleData& module = graph[index];
        return module.isESModuleOfProgram() && !module.isFacade() && !module.capturesExports() && !graph.resolvesNames(index)
            && module.isSynchronous() && !module.reachesUncompiledCode() && components.membersOf(components.componentOf[index]).size() == 1;
    };

    Vector<uint32_t> leaderOfImporters(FillWith { }, numberOfModules, none);
    Vector<uint32_t> leaderOf(FillWith { }, numberOfModules, none);
    for (uint32_t component = components.numberOfComponents; component--;) {
        for (uint32_t module : components.membersOf(component)) {
            uint32_t above = leaderOfImporters[module];
            if (graph[module].needsLink())
                leaderOf[module] = above != none && above != several && mayOnlyPassLinkOn(module) ? above : module;
        }
        for (uint32_t module : components.membersOf(component)) {
            if (!graph[module].needsLink() || !graph[module].linkFollowsRequests())
                continue;
            for (const ModuleRequestData& request : graph.requestsOf(module)) {
                uint32_t& above = leaderOfImporters[request.target()];
                above = above == none || above == leaderOf[module] ? leaderOf[module] : several;
            }
        }
    }

    struct Frame {
        uint32_t nextRequest;
        uint32_t endOfRequests;
    };
    LinkRequestsOfModules result;
    result.firstRequestOf.fill(0, numberOfModules + 1);
    Vector<Frame, 64> path;
    Vector<uint32_t> lastLeaderThatCameTo(FillWith { }, numberOfModules, none);
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        result.firstRequestOf[module] = static_cast<uint32_t>(result.requests.size());
        if (!graph[module].needsLink())
            continue;
        if (leaderOf[module] != module) {
            result.requests.append(leaderOf[module]);
            result.modulesThatOnlyPassLinkOn.append(module);
            continue;
        }
        if (!graph[module].linkFollowsRequests())
            continue;
        lastLeaderThatCameTo[module] = module;
        path.append(Frame { graph[module].firstRequest, graph[module + 1].firstRequest });
        while (!path.isEmpty()) {
            Frame& frame = path.last();
            if (frame.nextRequest == frame.endOfRequests) {
                path.removeLast();
                continue;
            }
            uint32_t target = requests[frame.nextRequest++].target();
            if (!graph[target].needsLink() || std::exchange(lastLeaderThatCameTo[target], module) == module)
                continue;
            if (leaderOf[target] == target) {
                result.requests.append(target);
                continue;
            }
            RELEASE_ASSERT(leaderOf[target] == module);
            path.append(Frame { graph[target].firstRequest, graph[target + 1].firstRequest });
        }
    }
    result.firstRequestOf[numberOfModules] = static_cast<uint32_t>(result.requests.size());
    return result;
}

void layLinkRequests(std::span<ModuleData> modulesAndEndRecord, const LinkRequestsOfModules& linkRequests)
{
    RELEASE_ASSERT(modulesAndEndRecord.size() == linkRequests.firstRequestOf.size());
    for (size_t module = 0; module < modulesAndEndRecord.size(); ++module)
        modulesAndEndRecord[module].firstLinkRequest = Number24(linkRequests.firstRequestOf[module]);
    for (uint32_t module : linkRequests.modulesThatOnlyPassLinkOn)
        modulesAndEndRecord[module].add(ModuleData::Flag::OnlyPassesLinkOn);
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

BitVector findESModulesOfProgram(std::span<const ModuleData> modulesAndEndRecord)
{
    BitVector result(modulesAndEndRecord.size());
    for (size_t module = 0; module + 1 < modulesAndEndRecord.size(); ++module) {
        if (modulesAndEndRecord[module].isESModuleOfProgram())
            result.quickSet(module);
    }
    return result;
}

Vector<uint32_t> groupModulesLoadedTogether(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, const BitVector& isGrouped, uint32_t endOfNumbers)
{
    using Place = ModuleData::PlaceInLoadGroup;
    constexpr uint32_t none = UINT32_MAX;
    constexpr uint32_t several = UINT32_MAX - 1;
    RELEASE_ASSERT(!modulesAndEndRecord.empty());
    ModuleGraph graph { modulesAndEndRecord, requests };
    uint32_t numberOfModules = graph.size();
    if (numberOfModules > endOfNumbers) {
        for (size_t module : isGrouped)
            graph[static_cast<unsigned>(module)].referrerRuntimeModuleOrLoadGroup = ModuleData::encodeLoadGroup(Place::BelowHead, 0);
        return { };
    }

    BitVector beginsLoads(numberOfModules);
    BitVector requestsUncompiledCode(numberOfModules);
    Vector<uint32_t> firstImportOf(FillWith { }, numberOfModules + 1, 0);
    Vector<uint32_t> imports;
    imports.reserveInitialCapacity(requests.size());
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        firstImportOf[module] = static_cast<uint32_t>(imports.size());
        for (const ModuleRequestData& request : graph.requestsOf(module)) {
            uint32_t target = request.target();
            bool isImport = !request.isRequire && !request.isHoisted && isGrouped.quickGet(module);
            if (isImport && isGrouped.quickGet(target))
                imports.append(target);
            else if (isGrouped.quickGet(target))
                beginsLoads.quickSet(target);
            else if (isImport && graph[target].isUncompiledCode())
                requestsUncompiledCode.quickSet(module);
        }
    }
    firstImportOf[numberOfModules] = static_cast<uint32_t>(imports.size());
    auto importsOf = [&](uint32_t module) {
        return imports.span().subspan(firstImportOf[module], firstImportOf[module + 1] - firstImportOf[module]);
    };

    Typed::StronglyConnectedComponents components = Typed::computeStronglyConnectedComponents(numberOfModules, importsOf);
    Vector<uint32_t> headOfImporters(FillWith { }, components.numberOfComponents, none);
    Vector<uint32_t> headOf(FillWith { }, numberOfModules, none);
    Vector<uint32_t> numberOfMembersUpTo(FillWith { }, numberOfModules + 1, 0);
    for (uint32_t component = components.numberOfComponents; component--;) {
        std::span<const uint32_t> members = components.membersOf(component);
        if (!isGrouped.quickGet(members.front()))
            continue;
        bool everyMemberIsHead = std::ranges::any_of(members, [&](uint32_t member) { return requestsUncompiledCode.quickGet(member); });
        auto root = std::ranges::find_if(members, [&](uint32_t member) { return beginsLoads.quickGet(member); });
        uint32_t head = headOfImporters[component];
        if (root != members.end())
            head = *root;
        else if (head == none || head == several)
            head = members.front();
        for (uint32_t member : members) {
            headOf[member] = everyMemberIsHead ? member : head;
            ++numberOfMembersUpTo[headOf[member] + 1];
        }
        for (uint32_t member : members) {
            for (uint32_t imported : importsOf(member)) {
                uint32_t below = components.componentOf[imported];
                if (below == component)
                    continue;
                uint32_t& above = headOfImporters[below];
                above = above == none || above == headOf[member] ? headOf[member] : several;
            }
        }
    }

    for (uint32_t module = 0; module < numberOfModules; ++module)
        numberOfMembersUpTo[module + 1] += numberOfMembersUpTo[module];
    Vector<uint32_t> membersByHead(FillWith { }, numberOfMembersUpTo[numberOfModules], 0);
    Vector<uint32_t> nextPlaceOf = numberOfMembersUpTo;
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        if (headOf[module] != none)
            membersByHead[nextPlaceOf[headOf[module]]++] = module;
    }

    Vector<uint32_t> headsImported;
    Vector<uint32_t> firstHeadImportedBy(FillWith { }, numberOfModules + 1, 0);
    Vector<uint32_t> lastHeadThatImports(FillWith { }, numberOfModules, none);
    for (uint32_t head = 0; head < numberOfModules; ++head) {
        firstHeadImportedBy[head] = static_cast<uint32_t>(headsImported.size());
        if (headOf[head] != head)
            continue;
        for (uint32_t member : membersByHead.span().subspan(numberOfMembersUpTo[head], numberOfMembersUpTo[head + 1] - numberOfMembersUpTo[head])) {
            if (member != head)
                graph[member].referrerRuntimeModuleOrLoadGroup = ModuleData::encodeLoadGroup(components.componentOf[head] == components.componentOf[member] ? Place::InCycleWithHead : Place::BelowHead, head);
            for (uint32_t imported : importsOf(member)) {
                uint32_t other = headOf[imported];
                if (other != head && std::exchange(lastHeadThatImports[other], head) != head)
                    headsImported.append(other);
            }
        }
    }
    firstHeadImportedBy[numberOfModules] = static_cast<uint32_t>(headsImported.size());
    auto headsImportedBy = [&](uint32_t head) {
        return headsImported.span().subspan(firstHeadImportedBy[head], firstHeadImportedBy[head + 1] - firstHeadImportedBy[head]);
    };

    Vector<uint32_t> headAbove(FillWith { }, numberOfModules, none);
    for (uint32_t head = 0; head < numberOfModules; ++head) {
        for (uint32_t other : headsImportedBy(head)) {
            if (components.componentOf[other] != components.componentOf[head] && (headAbove[other] == none || components.componentOf[head] < components.componentOf[headAbove[other]]))
                headAbove[other] = head;
        }
    }
    Vector<uint32_t> numberOfHeadsFromItDown(FillWith { }, numberOfModules, 1);
    for (uint32_t module : components.members) {
        if (headOf[module] == module && headAbove[module] != none)
            numberOfHeadsFromItDown[headAbove[module]] += numberOfHeadsFromItDown[module];
    }
    Vector<uint32_t> placeInForest(FillWith { }, numberOfModules, 0);
    Vector<uint32_t> nextPlaceBelow(FillWith { }, numberOfModules, 0);
    uint32_t placeOfNextTree = 0;
    for (uint32_t module : components.members | std::views::reverse) {
        if (headOf[module] != module)
            continue;
        uint32_t& next = headAbove[module] == none ? placeOfNextTree : nextPlaceBelow[headAbove[module]];
        placeInForest[module] = next;
        nextPlaceBelow[module] = next + 1;
        next += numberOfHeadsFromItDown[module];
    }

    Vector<uint32_t> lists;
    for (uint32_t head = 0; head < numberOfModules; ++head) {
        if (headOf[head] != head)
            continue;
        std::span<uint32_t> imported = headsImported.mutableSpan().subspan(firstHeadImportedBy[head], firstHeadImportedBy[head + 1] - firstHeadImportedBy[head]);
        AOT::sort(imported, { }, [&](uint32_t other) { return placeInForest[other]; });
        size_t placeOfLength = lists.size();
        lists.append(0);
        uint32_t endOfWhatLastListedLeadsTo = 0;
        for (uint32_t other : imported) {
            if (placeInForest[other] < endOfWhatLastListedLeadsTo)
                continue;
            lists.append(other);
            endOfWhatLastListedLeadsTo = placeInForest[other] + numberOfHeadsFromItDown[other];
        }
        lists[placeOfLength] = static_cast<uint32_t>(lists.size() - placeOfLength - 1);
        if (!lists[placeOfLength])
            lists.removeLast();
        bool hasList = lists.size() > placeOfLength;
        if (!hasList || placeOfLength + 1 < endOfNumbers) {
            graph[head].referrerRuntimeModuleOrLoadGroup = ModuleData::encodeLoadGroup(requestsUncompiledCode.quickGet(head) ? Place::HeadThatRequestsUncompiledCode : Place::Head, hasList ? static_cast<uint32_t>(placeOfLength + 1) : 0);
            continue;
        }
        lists.shrink(placeOfLength);
        for (uint32_t member : membersByHead.span().subspan(numberOfMembersUpTo[head], numberOfMembersUpTo[head + 1] - numberOfMembersUpTo[head]))
            graph[member].referrerRuntimeModuleOrLoadGroup = ModuleData::encodeLoadGroup(Place::BelowHead, head);
    }
    return lists;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
