/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrder.h"

#if ENABLE(AOT)

#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <span>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ModuleGraph {
    Vector<uint32_t> offsets;
    Vector<uint32_t> targets;

    std::span<const uint32_t> successorsOf(unsigned node) const
    {
        return targets.subspan(offsets[node], offsets[node + 1] - offsets[node]);
    }
};

uint32_t anyModuleNode(const FlowOrderInputs& inputs)
{
    return inputs.program().modules.size();
}

// What the host calls can run while any module is being evaluated.
uint32_t nodeOf(const FlowOrderInputs& inputs, FlowInstanceID instance)
{
    if (inputs.isHost(instance))
        return anyModuleNode(inputs);
    return anyModuleNode(inputs) + 1 + instance.index();
}

unsigned numberOfNodes(const FlowOrderInputs& inputs)
{
    return anyModuleNode(inputs) + 1 + inputs.numberOfInstances();
}

bool evaluatesTarget(ImportPhase phase)
{
    switch (phase) {
    case ImportPhase::Evaluation:
        return true;
    case ImportPhase::Defer:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool evaluatesSynchronously(FlowModuleOperationKind kind)
{
    switch (kind) {
    case FlowModuleOperationKind::Require:
    case FlowModuleOperationKind::Evaluate:
    case FlowModuleOperationKind::RequireDynamic:
    case FlowModuleOperationKind::RequireBuiltinModule:
        return true;
    case FlowModuleOperationKind::Namespace:
    case FlowModuleOperationKind::Import:
    case FlowModuleOperationKind::ImportDynamic:
    case FlowModuleOperationKind::ResolveDynamic:
    case FlowModuleOperationKind::Use:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool evaluatesAsynchronously(FlowModuleOperationKind kind)
{
    switch (kind) {
    case FlowModuleOperationKind::Import:
    case FlowModuleOperationKind::ImportDynamic:
        return true;
    case FlowModuleOperationKind::Namespace:
    case FlowModuleOperationKind::Require:
    case FlowModuleOperationKind::Evaluate:
    case FlowModuleOperationKind::RequireDynamic:
    case FlowModuleOperationKind::RequireBuiltinModule:
    case FlowModuleOperationKind::ResolveDynamic:
    case FlowModuleOperationKind::Use:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void forEachLoadedModule(const FlowModuleOperation& operation, std::span<const ModuleID> moduleOfRuntimeModule, NOESCAPE const Invocable<void(ModuleID)> auto& functor)
{
    switch (operation.kind) {
    case FlowModuleOperationKind::Namespace:
    case FlowModuleOperationKind::Import:
    case FlowModuleOperationKind::Require:
    case FlowModuleOperationKind::Evaluate:
        functor(operation.module);
        return;
    case FlowModuleOperationKind::ImportDynamic:
    case FlowModuleOperationKind::RequireDynamic:
    case FlowModuleOperationKind::ResolveDynamic:
    case FlowModuleOperationKind::Use:
        return;
    case FlowModuleOperationKind::RequireBuiltinModule: {
        uint32_t index = operation.runtimeModule.index();
        if (index < moduleOfRuntimeModule.size() && moduleOfRuntimeModule[index])
            functor(moduleOfRuntimeModule[index]);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void forEachInstanceEdge(const FlowOrderInputs& inputs, FlowInstanceID instance, std::span<const ModuleID> moduleOfRuntimeModule, std::span<const ModuleID> moduleWithBody, NOESCAPE const Invocable<void(uint32_t, uint32_t)> auto& functor)
{
    if (inputs.isHost(instance))
        return;

    const Program& program = inputs.program();
    FunctionID id = inputs.functionOf(instance);
    const Function& function = program.at(id);
    uint32_t node = nodeOf(inputs, instance);
    ModuleID evaluatedModule = moduleWithBody[id.index()];
    if (evaluatedModule)
        functor(evaluatedModule.index(), node);

    bool canSuspendEvaluation = function.module && program.at(function.module).flags.contains(ModuleFlags::HasTopLevelAwait);
    for (const FlowModuleOperation& operation : inputs.moduleOperationsOf(id)) {
        bool isSynchronous = evaluatesSynchronously(operation.kind);
        if (!isSynchronous && !(canSuspendEvaluation && evaluatesAsynchronously(operation.kind)))
            continue;
        forEachLoadedModule(operation, moduleOfRuntimeModule, [&](ModuleID target) {
            if (function.module)
                functor(function.module.index(), target.index());
            if (evaluatedModule && evaluatedModule != function.module)
                functor(evaluatedModule.index(), target.index());
            if (isSynchronous)
                functor(node, target.index());
        });
    }
}

// The loader goes through a module that is not evaluated to the modules that it imports. What such a module requires nobody visits: no code of it runs.
BitVector modulesVisitedByLoader(const FlowOrderInputs& inputs)
{
    const Program& program = inputs.program();
    BitVector visited = inputs.evaluatedModules();
    Vector<uint32_t, 32> pending;
    for (size_t module : visited)
        pending.append(static_cast<uint32_t>(module));
    while (!pending.isEmpty()) {
        for (const ModuleRequest& request : program.modules[pending.takeLast()].requests) {
            if (request.kind == ModuleRequestKind::Import && !visited.set(request.target.index()))
                pending.append(request.target.index());
        }
    }
    return visited;
}

void forEachModuleEdge(const FlowOrderInputs& inputs, const BitVector& visitedModules, std::span<const ModuleID> moduleOfRuntimeModule, std::span<const ModuleID> moduleWithBody, NOESCAPE const Invocable<void(uint32_t, uint32_t)> auto& functor)
{
    const Program& program = inputs.program();
    const BitVector& evaluatedModules = inputs.evaluatedModules();
    uint32_t anyModule = anyModuleNode(inputs);

    for (size_t module : visitedModules) {
        for (const ModuleRequest& request : program.modules[module].requests)
            functor(static_cast<uint32_t>(module), request.target.index());
    }
    for (unsigned instance = 0; instance < inputs.numberOfInstances(); ++instance)
        forEachInstanceEdge(inputs, FlowInstanceID(instance), moduleOfRuntimeModule, moduleWithBody, functor);
    for (const FlowModuleLoadRecord& load : inputs.moduleLoads()) {
        functor(nodeOf(inputs, load.caller), load.module.index());
        if (inputs.isHost(load.caller))
            continue;
        if (ModuleID loader = program.at(inputs.functionOf(load.caller)).module)
            functor(loader.index(), load.module.index());
    }
    for (const FlowCallLink& link : inputs.callLinks()) {
        bool isEvaluationByLoader = link.kind == FlowCallLinkKind::Host && moduleWithBody[link.callee.index()];
        if (!isEvaluationByLoader)
            functor(nodeOf(inputs, link.callerInstance), nodeOf(inputs, link.calleeInstance));
    }
    for (uint32_t module = 0; module < anyModule; ++module) {
        if (inputs.mayBeLoadedByName(ModuleID(module)))
            functor(anyModule, module);
    }
    for (size_t module : evaluatedModules)
        functor(static_cast<uint32_t>(module), anyModule);
}

ModuleGraph buildModuleGraph(const FlowOrderInputs& inputs, const BitVector& visitedModules, std::span<const ModuleID> moduleOfRuntimeModule, std::span<const ModuleID> moduleWithBody)
{
    unsigned nodeCount = numberOfNodes(inputs);
    ModuleGraph graph;
    graph.offsets.fill(0, nodeCount + 1);
    forEachModuleEdge(inputs, visitedModules, moduleOfRuntimeModule, moduleWithBody, [&](uint32_t source, uint32_t) {
        ++graph.offsets[source + 1];
    });
    for (unsigned node = 0; node < nodeCount; ++node)
        graph.offsets[node + 1] += graph.offsets[node];

    graph.targets.grow(graph.offsets.last());
    Vector<uint32_t> nextTarget = graph.offsets;
    forEachModuleEdge(inputs, visitedModules, moduleOfRuntimeModule, moduleWithBody, [&](uint32_t source, uint32_t target) {
        graph.targets[nextTarget[source]++] = target;
    });
    return graph;
}

Vector<uint32_t> sortModulesByComponent(const StronglyConnectedComponents& components, unsigned numberOfModules)
{
    const Vector<uint32_t>& componentOf = components.componentOf;
    Vector<uint32_t> modules;
    modules.reserveInitialCapacity(numberOfModules);
    for (unsigned module = 0; module < numberOfModules; ++module)
        modules.append(module);
    std::ranges::sort(modules, [&](uint32_t left, uint32_t right) {
        return std::pair(componentOf[left], left) < std::pair(componentOf[right], right);
    });
    return modules;
}

Vector<uint32_t> numberModuleComponents(const StronglyConnectedComponents& components, std::span<const uint32_t> modulesByComponent)
{
    Vector<uint32_t> componentOf(FillWith { }, modulesByComponent.size(), 0u);
    uint32_t component = 0;
    for (size_t position = 0; position < modulesByComponent.size(); ++position) {
        uint32_t module = modulesByComponent[position];
        if (position && components.componentOf[module] != components.componentOf[modulesByComponent[position - 1]])
            ++component;
        componentOf[module] = component;
    }
    return componentOf;
}

// An import has run its target to the end before the body of the importer starts, also a target that lies on a cycle of its own.
bool completesBeforeRequester(const ModuleRequest& request, const BitVector& modulesInCycles)
{
    if (!request.completes || !evaluatesTarget(request.phase))
        return false;
    switch (request.kind) {
    case ModuleRequestKind::Import:
        return true;
    case ModuleRequestKind::Require:
        return !modulesInCycles.get(request.target.index());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<BitVector> computeReachedComponents(const FlowOrderInputs& inputs, const BitVector& visitedModules, const BitVector& modulesInCycles, const Vector<uint32_t>& componentOf, std::span<const uint32_t> modulesByComponent)
{
    size_t numberOfComponents = modulesByComponent.empty() ? 0 : componentOf[modulesByComponent.back()] + 1;
    Vector<BitVector> reachedComponents(numberOfComponents);
    for (uint32_t module : modulesByComponent) {
        if (!visitedModules.get(module))
            continue;
        uint32_t component = componentOf[module];
        BitVector& reached = reachedComponents[component];
        for (const ModuleRequest& request : inputs.program().modules[module].requests) {
            uint32_t target = componentOf[request.target.index()];
            if (!completesBeforeRequester(request, modulesInCycles) || target == component || reached.get(target))
                continue;
            reached.set(target);
            reached.merge(reachedComponents[target]);
        }
    }
    return reachedComponents;
}

} // anonymous namespace

Vector<ModuleID> indexModuleBodies(const Program& program)
{
    Vector<ModuleID> moduleWithBody(program.functions.size());
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (FunctionID body = program.modules[index].body)
            moduleWithBody[body.index()] = ModuleID(index);
    }
    return moduleWithBody;
}

FlowModuleOrdering FlowModuleOrder::orderingOf(ModuleID first, ModuleID second) const
{
    if (first == second || first.index() >= componentOf.size() || second.index() >= componentOf.size() || modulesInCycles.get(second.index()))
        return FlowModuleOrdering::Unknown;
    uint32_t firstComponent = componentOf[first.index()];
    uint32_t secondComponent = componentOf[second.index()];
    if (firstComponent == secondComponent)
        return FlowModuleOrdering::Unknown;
    return componentsCompletedBefore[secondComponent].get(firstComponent) ? FlowModuleOrdering::CompletesBefore : FlowModuleOrdering::Unknown;
}

bool FlowModuleOrder::runsAfterRequests(ModuleID module) const
{
    return module.index() < componentOf.size() && !modulesInCycles.get(module.index());
}

void FlowModuleOrder::dump(PrintStream& out) const
{
    for (unsigned module = 0; module < componentOf.size(); ++module) {
        uint32_t component = componentOf[module];
        out.print("m", module, ": component ", component, modulesInCycles.get(module) ? " cycle" : "", " after {");
        CommaPrinter comma;
        for (size_t completed : componentsCompletedBefore[component])
            out.print(comma, completed);
        out.println("}");
    }
}

FlowModuleOrder computeModuleOrder(const FlowOrderInputs& inputs)
{
    unsigned numberOfModules = inputs.program().modules.size();
    Vector<ModuleID> moduleOfRuntimeModule = indexRuntimeModules(inputs.program());
    Vector<ModuleID> moduleWithBody = indexModuleBodies(inputs.program());
    BitVector visitedModules = modulesVisitedByLoader(inputs);
    ModuleGraph graph = buildModuleGraph(inputs, visitedModules, moduleOfRuntimeModule.span(), moduleWithBody.span());
    StronglyConnectedComponents components = computeStronglyConnectedComponents(numberOfNodes(inputs), [&](unsigned node) {
        return graph.successorsOf(node);
    });
    Vector<uint32_t> modulesByComponent = sortModulesByComponent(components, numberOfModules);

    FlowModuleOrder order;
    order.componentOf = numberModuleComponents(components, modulesByComponent.span());
    for (unsigned module = 0; module < numberOfModules; ++module) {
        if (components.isInCycle.get(module))
            order.modulesInCycles.set(module);
    }
    order.componentsCompletedBefore = computeReachedComponents(inputs, visitedModules, order.modulesInCycles, order.componentOf, modulesByComponent.span());
    return order;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
