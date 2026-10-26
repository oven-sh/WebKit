/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrder.h"

#if ENABLE(AOT)

#include "AOTGrouping.h"
#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static bool evaluatesTarget(ImportPhase phase)
{
    switch (phase) {
    case ImportPhase::Evaluation:
        return true;
    case ImportPhase::Defer:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool completesBeforeRequester(const Module& requester, const ModuleRequest& request, const BitVector& modulesInCycles)
{
    if (!request.completes || !evaluatesTarget(request.phase))
        return false;
    switch (request.kind) {
    case ModuleRequestKind::Import:
        // The loader evaluates a graph when all of it is loaded. A CommonJS file that requires an ES module that imports the file is loaded from then on, while its body runs.
        return !requester.flags.contains(ModuleFlags::IsLoadedByLoader) || !modulesInCycles.get(request.target.index());
    case ModuleRequestKind::Require:
        return !modulesInCycles.get(request.target.index());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FlowModuleOrder::completesBefore(ModuleID first, ModuleID second) const
{
    if (first == second || first.index() >= componentOf.size() || second.index() >= componentOf.size() || modulesInCycles.get(second.index()))
        return false;
    uint32_t firstComponent = componentOf[first.index()];
    uint32_t secondComponent = componentOf[second.index()];
    return firstComponent != secondComponent && componentsCompletedBefore[secondComponent].get(firstComponent);
}

FlowModuleOrder computeModuleOrder(const FlowOrderInputs& inputs)
{
    const Program& program = inputs.program;
    uint32_t numberOfModules = program.modules.size();
    uint32_t anyModule = numberOfModules;
    unsigned numberOfNodes = anyModule + 2 + program.functions.size();
    auto nodeOf = [&](FunctionID function) {
        return function == inputs.host() ? anyModule : anyModule + 1 + function.index();
    };

    Vector<ModuleID> moduleWithBody(program.functions.size());
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        if (FunctionID body = program.modules[module].body)
            moduleWithBody[body.index()] = ModuleID(module);
    }

    BitVector modulesVisitedByLoader = inputs.evaluatedModules;
    Vector<uint32_t, 32> pending;
    for (size_t module : modulesVisitedByLoader)
        pending.append(static_cast<uint32_t>(module));
    while (!pending.isEmpty()) {
        for (const ModuleRequest& request : program.modules[pending.takeLast()].requests) {
            if (request.kind == ModuleRequestKind::Import && !modulesVisitedByLoader.set(request.target.index()))
                pending.append(request.target.index());
        }
    }

    Vector<uint32_t> firstSuccessor;
    Vector<uint32_t> successors;
    groupByIndex(numberOfNodes, firstSuccessor, successors, [&](const auto& addEdge) {
        for (size_t module : modulesVisitedByLoader) {
            for (const ModuleRequest& request : program.modules[module].requests)
                addEdge(static_cast<uint32_t>(module), request.target.index());
        }
        auto load = inputs.moduleLoads.begin();
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            ModuleID file = program.functions[index].module;
            ModuleID evaluatedModule = moduleWithBody[index];
            if (evaluatedModule)
                addEdge(evaluatedModule.index(), nodeOf(FunctionID(index)));
            bool canSuspendEvaluation = file && program.at(file).flags.contains(ModuleFlags::HasTopLevelAwait);
            for (; load != inputs.moduleLoads.end() && load->function.index() == index; ++load) {
                if (!load->isSynchronous && !canSuspendEvaluation)
                    continue;
                if (file)
                    addEdge(file.index(), load->target.index());
                if (evaluatedModule && evaluatedModule != file)
                    addEdge(evaluatedModule.index(), load->target.index());
                if (load->isSynchronous)
                    addEdge(nodeOf(FunctionID(index)), load->target.index());
            }
        }
        for (size_t module : inputs.modulesLoadedByHost)
            addEdge(anyModule, static_cast<uint32_t>(module));
        for (const FlowCall& call : inputs.calls) {
            bool isEvaluationByLoader = call.caller == inputs.host() && moduleWithBody[call.callee.index()];
            if (!isEvaluationByLoader)
                addEdge(nodeOf(call.caller), nodeOf(call.callee));
        }
        for (size_t module : inputs.modulesLoadedByName)
            addEdge(anyModule, static_cast<uint32_t>(module));
        for (size_t module : inputs.evaluatedModules)
            addEdge(static_cast<uint32_t>(module), anyModule);
    });
    StronglyConnectedComponents components = computeStronglyConnectedComponents(numberOfNodes, [&](unsigned node) {
        return entriesOfGroup(firstSuccessor, successors, node);
    });

    Vector<uint32_t> modulesByComponent;
    modulesByComponent.reserveInitialCapacity(numberOfModules);
    for (uint32_t module = 0; module < numberOfModules; ++module)
        modulesByComponent.append(module);
    AOT::sort(modulesByComponent, [&](uint32_t left, uint32_t right) {
        return std::pair(components.componentOf[left], left) < std::pair(components.componentOf[right], right);
    });

    FlowModuleOrder order;
    order.componentOf = Vector<uint32_t>(FillWith { }, numberOfModules, 0u);
    uint32_t numberOfComponents = 0;
    for (uint32_t position = 0; position < numberOfModules; ++position) {
        uint32_t module = modulesByComponent[position];
        if (!position || components.componentOf[module] != components.componentOf[modulesByComponent[position - 1]])
            ++numberOfComponents;
        order.componentOf[module] = numberOfComponents - 1;
        if (components.isInCycle.get(module))
            order.modulesInCycles.set(module);
    }

    order.componentsCompletedBefore = Vector<BitVector>(numberOfComponents);
    for (uint32_t module : modulesByComponent) {
        if (!modulesVisitedByLoader.get(module))
            continue;
        uint32_t component = order.componentOf[module];
        BitVector& completed = order.componentsCompletedBefore[component];
        for (const ModuleRequest& request : program.modules[module].requests) {
            uint32_t target = order.componentOf[request.target.index()];
            if (!completesBeforeRequester(program.modules[module], request, order.modulesInCycles) || target == component || completed.get(target))
                continue;
            completed.set(target);
            completed.merge(order.componentsCompletedBefore[target]);
        }
    }
    return order;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
