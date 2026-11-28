/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE Typed::StronglyConnectedComponents computeModuleFlags(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests);

struct LinkRequestsOfModules {
    Vector<uint32_t> requests;
    Vector<uint32_t> firstRequestOf;
    Vector<uint32_t> modulesThatOnlyPassLinkOn;
};
JS_EXPORT_PRIVATE LinkRequestsOfModules contractLinkRequests(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, const Typed::StronglyConnectedComponents& componentsOfRequests);
JS_EXPORT_PRIVATE void layLinkRequests(std::span<ModuleData> modulesAndEndRecord, const LinkRequestsOfModules&);

JS_EXPORT_PRIVATE Vector<LinkPlanStepData> planLinkOfEntryPoint(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, ModuleID entryPoint);
JS_EXPORT_PRIVATE BitVector findESModulesOfProgram(std::span<const ModuleData> modulesAndEndRecord);
JS_EXPORT_PRIVATE Vector<uint32_t> groupModulesLoadedTogether(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, const BitVector& esModulesOfProgram, uint32_t endOfNumbers = ModuleData::endOfNumbersInLoadGroups);

template<typename Graph>
void noteGroupsLoadedWith(const Graph& graph, uint32_t root, Vector<uint64_t>& loadedGroups, BitVector& modulesLoadedBesideGroups)
{
    using Place = ModuleData::PlaceInLoadGroup;
    Vector<uint32_t, 32> heads;
    Vector<uint32_t, 32> modulesBelowHeads;
    auto loadComesTo = [&](uint32_t module) {
        const ModuleData& moduleData = graph.data(module);
        uint32_t head = moduleData.isHeadOfLoadGroup() ? module : moduleData.numberInLoadGroup();
        uint64_t& word = loadedGroups[head / 64];
        uint64_t bit = 1ull << (head % 64);
        if (word & bit)
            return;
        if (moduleData.placeInLoadGroup() != Place::BelowHead) {
            word |= bit;
            heads.append(head);
        } else if (!modulesLoadedBesideGroups.set(module))
            modulesBelowHeads.append(module);
    };
    auto loadComesToImportsOf = [&](uint32_t module, bool ofUncompiledCodeAlone) {
        for (const ModuleRequestData& request : graph.requests(module)) {
            if (request.isRequire || request.isHoisted)
                continue;
            bool isUncompiled = graph.data(request.target()).isUncompiledCode();
            if (ofUncompiledCodeAlone && !isUncompiled)
                continue;
            if (uint32_t imported = isUncompiled ? graph.moduleLoadedBy(request) : request.target(); graph.data(imported).isESModuleOfProgram())
                loadComesTo(imported);
        }
    };
    loadComesTo(root);
    while (!heads.isEmpty() || !modulesBelowHeads.isEmpty()) {
        if (heads.isEmpty()) {
            loadComesToImportsOf(modulesBelowHeads.takeLast(), false);
            continue;
        }
        uint32_t head = heads.takeLast();
        for (uint32_t other : graph.groupsLoadedWith(head)) {
            if (!(loadedGroups[other / 64] >> (other % 64) & 1))
                loadComesTo(other);
        }
        if (graph.data(head).placeInLoadGroup() == Place::HeadThatRequestsUncompiledCode)
            loadComesToImportsOf(head, true);
    }
}

enum class StepOfLinkWalk : uint8_t { CapturesExports, IsUncompiled, Enter, Leave };

template<typename World, typename Steps>
void walkModulesToLink(const World& world, uint32_t root, Steps& steps)
{
    struct Frame {
        uint32_t enter;
        uint32_t nextRequest;
        uint32_t endOfRequests;
        bool goesByAllRequests;
        bool resolvesNames;
    };

    Vector<Frame, 64> path;
    auto record = [&](uint32_t module) {
        const ModuleData& moduleData = world.data(module);
        if (moduleData.linkFollowsRequests() && world.isUnlinked(module)) {
            if (moduleData.onlyPassesLinkOn()) [[unlikely]]
                world.didGetStateOfItsOwn(module);
            if (moduleData.onlyPassesLinkOn() || world.goesByAllRequests(module)) [[unlikely]]
                path.append(Frame { steps.size(), moduleData.firstRequest, world.endOfRequests(module), true, world.resolvesNames(module) });
            else
                path.append(Frame { steps.size(), moduleData.firstLinkRequest, world.endOfLinkRequests(module), false, world.resolvesNames(module) });
            steps.append(module, StepOfLinkWalk::Enter, false);
        }
        if (moduleData.capturesExports())
            steps.append(module, StepOfLinkWalk::CapturesExports, false);
        else if (moduleData.isUncompiledCode())
            steps.append(module, StepOfLinkWalk::IsUncompiled, false);
    };

    if (world.isSettledForLinks(root))
        return;
    record(root);
    if (path.isEmpty())
        return;

    bool leavesOutModulesWithNothingToDo = !world.loadsModulesIndividually() && !world.data(root).reachesUncompiledCode();
    ModuleNumbers linkRequests = world.linkRequests();
    std::span<const ModuleRequestData> allRequests = world.requests();
    Vector<uint64_t> passedOver;
    std::span<const uint64_t> modulesPassedOver = world.modulesSettledForLinks();
    auto passOverFromNowOn = [&](uint32_t module) ALWAYS_INLINE_LAMBDA {
        passedOver[module / 64] |= 1ull << (module % 64);
    };
    while (!path.isEmpty()) {
        Frame& frame = path.last();
        uint32_t next = frame.nextRequest;
        uint32_t end = frame.endOfRequests;
        bool goesByAllRequests = frame.goesByAllRequests;
        size_t depth = path.size();
        while (true) {
            uint32_t target;
            if (goesByAllRequests) [[unlikely]] {
                while (next != end && (modulesPassedOver[allRequests[next].target() / 64] >> (allRequests[next].target() % 64) & 1))
                    ++next;
                if (next == end)
                    break;
                target = allRequests[next++].target();
            } else {
                next = static_cast<uint32_t>(linkRequests.findFirstOutside(modulesPassedOver, next, end));
                if (next == end)
                    break;
                target = linkRequests[next++];
            }
            if (passedOver.isEmpty()) {
                passedOver = modulesPassedOver;
                modulesPassedOver = passedOver.span();
                passOverFromNowOn(root);
                if (target == root)
                    continue;
            }
            passOverFromNowOn(target);
            if (world.isSettledForLinks(target))
                continue;
            const ModuleData& targetData = world.data(target);
            if (leavesOutModulesWithNothingToDo && !targetData.capturesExports() && !targetData.isUncompiledCode() && !targetData.onlyPassesLinkOn() && !world.goesByAllRequests(target)) [[likely]] {
                uint32_t endOfTarget = world.endOfLinkRequests(target);
                uint32_t firstOfTarget = static_cast<uint32_t>(linkRequests.findFirstOutside(modulesPassedOver, targetData.firstLinkRequest, endOfTarget));
                bool targetResolvesNames = world.resolvesNames(target);
                if (firstOfTarget == endOfTarget && !targetResolvesNames) {
                    steps.onlyBecomesLinkedAlone(target);
                    continue;
                }
                path[depth - 1].nextRequest = next;
                path.append(Frame { steps.size(), firstOfTarget, endOfTarget, false, targetResolvesNames });
                steps.append(target, StepOfLinkWalk::Enter, false);
                break;
            }
            path[depth - 1].nextRequest = next;
            record(target);
            if (path.size() != depth)
                break;
        }
        if (path.size() != depth)
            continue;

        uint32_t enter = path.last().enter;
        bool resolvesNames = path.last().resolvesNames;
        if (leavesOutModulesWithNothingToDo && enter && steps.size() == enter + 1 && !resolvesNames)
            steps.onlyBecomesLinked(enter);
        else
            steps.leave(enter, resolvesNames);
        path.removeLast();
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
