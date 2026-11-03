/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrder.h"

#if ENABLE(AOT)

#include "AOTEffectsCache.h"
#include "AOTGrouping.h"
#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <utility>
#include <wtf/StdLibExtras.h>

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

static bool mayRunOtherCode(const Function& body)
{
    return std::ranges::any_of(body.values, [](const Value& value) {
        return effectFactsOf(value.opcode).traits.atMost.mayRunOtherCode();
    });
}

// An edge says: while the first is unfinished the second may begin.
FlowModuleOrder computeModuleOrder(const FlowOrderInputs& inputs)
{
    const Program& program = inputs.program;
    uint32_t numberOfModules = program.modules.size();
    uint32_t anyTime = numberOfModules;
    uint32_t whereJobsRun = anyTime + 1;
    uint32_t firstFunction = whereJobsRun + 1;
    // A list of many callees stands between its callers and its members: n calls of any of n functions would be n * n edges.
    uint32_t firstCalleeList = firstFunction + program.functions.size();
    unsigned numberOfNodes = firstCalleeList + inputs.calleeLists.size();
    auto nodeOf = [&](FunctionID function) {
        return function == inputs.host() ? anyTime : firstFunction + function.index();
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

    // The host gets a turn only inside an operation that leaves the code of the file.
    BitVector modulesThatLetTheHostRun(numberOfModules);
    for (size_t module : inputs.evaluatedModules) {
        if (mayRunOtherCode(program.at(program.modules[module].body)))
            modulesThatLetTheHostRun.quickSet(module);
    }

    auto isAwaitedInTurn = [&](const FlowModuleLoad& load) {
        ModuleID file = program.at(load.function).module;
        return load.isAwaitedAtOnceInBodyOfFile && file && program.at(file).flags.contains(ModuleFlags::HasTopLevelAwait);
    };
    Vector<Vector<uint32_t, 2>> modulesLoadedInBody(numberOfModules);
    BitVector modulesBegunElsewhere(numberOfModules);
    BitVector modulesThatMayRunTwice(numberOfModules);
    bool someImportIsAwaitedInTurn = std::ranges::any_of(inputs.moduleLoads, isAwaitedInTurn);
    auto forEachModuleLoadedWith = [&](Vector<uint32_t, 32>&& toVisit, const Invocable<void(uint32_t)> auto& functor) {
        BitVector isVisited(numberOfModules);
        for (uint32_t module : toVisit)
            isVisited.quickSet(module);
        auto visit = [&](uint32_t module) {
            if (!isVisited.quickSet(module))
                toVisit.append(module);
        };
        while (!toVisit.isEmpty()) {
            uint32_t module = toVisit.takeLast();
            functor(module);
            for (const ModuleRequest& request : program.modules[module].requests)
                visit(request.target.index());
            for (uint32_t loaded : modulesLoadedInBody[module])
                visit(loaded);
        }
    };
    if (someImportIsAwaitedInTurn) {
        BitVector begunFirst = inputs.modulesLoadedByName;
        begunFirst.merge(inputs.modulesNamedByOperationsNobodyKnows);
        begunFirst.merge(inputs.modulesLoadedByHost);
        for (uint32_t module = 0; module < numberOfModules; ++module) {
            if (program.modules[module].flags.containsAny({ ModuleFlags::MayBeEvicted, ModuleFlags::RunsAgain })) {
                modulesThatMayRunTwice.quickSet(module);
                begunFirst.set(module);
            }
        }
        for (const FlowModuleLoad& load : inputs.moduleLoads) {
            ModuleID loader = moduleWithBody[load.function.index()];
            if (loader && (load.way == FlowWayIntoFile::Require || isAwaitedInTurn(load)))
                modulesLoadedInBody[loader.index()].append(load.target.index());
            else
                begunFirst.set(load.target.index());
        }
        Vector<uint32_t, 32> toVisit;
        for (size_t module : begunFirst)
            toVisit.append(static_cast<uint32_t>(module));
        forEachModuleLoadedWith(WTF::move(toVisit), [&](uint32_t module) {
            modulesBegunElsewhere.quickSet(module);
        });
        for (const Entry& entry : program.entries)
            modulesBegunElsewhere.quickSet(entry.module.index());
    }

    BitVector somebodySaysWhenItBegins(numberOfModules);
    bool somebodyRunsJobs = false;
    Vector<uint32_t> firstSuccessor;
    Vector<uint32_t> successors;
    groupByIndex(numberOfNodes, firstSuccessor, successors, [&](const auto& addEdge) {
        auto begins = [&](uint32_t unfinished, ModuleID file) {
            somebodySaysWhenItBegins.set(file.index());
            addEdge(unfinished, file.index());
        };
        auto runsJobs = [&](uint32_t unfinished) {
            somebodyRunsJobs = true;
            addEdge(unfinished, whereJobsRun);
        };
        auto forEachRequest = [&](const Invocable<void(ModuleID, const ModuleRequest&)> auto& functor) {
            for (size_t module : modulesVisitedByLoader) {
                for (const ModuleRequest& request : program.modules[module].requests)
                    functor(ModuleID(static_cast<uint32_t>(module)), request);
            }
        };
        auto forEachLoad = [&](FlowWayIntoFile way, const Invocable<void(const FlowModuleLoad&, ModuleID, ModuleID)> auto& functor) {
            for (const FlowModuleLoad& load : inputs.moduleLoads) {
                if (load.way == way)
                    functor(load, program.at(load.function).module, moduleWithBody[load.function.index()]);
            }
        };
        auto beginsInsideFilesOf = [&](const FlowModuleLoad& load, ModuleID file, ModuleID evaluatedModule) {
            if (file)
                begins(file.index(), load.target);
            if (evaluatedModule && evaluatedModule != file)
                begins(evaluatedModule.index(), load.target);
        };

        for (FlowWayIntoFile way : allWaysIntoFile) {
            switch (way) {
            case FlowWayIntoFile::EntryPoint:
                // First in its realm: nothing is unfinished.
                for (const Entry& entry : program.entries)
                    somebodySaysWhenItBegins.set(entry.module.index());
                break;
            case FlowWayIntoFile::StaticImport:
                // A file that imports itself has begun, and begins once.
                forEachRequest([&](ModuleID requester, const ModuleRequest& request) {
                    if (request.kind == ModuleRequestKind::Import && request.target != requester)
                        begins(requester.index(), request.target);
                });
                break;
            case FlowWayIntoFile::DeferredImport:
                // At the first look at its namespace. The loader refuses the look while a file that the deferred one needs is unfinished.
                forEachLoad(way, [&](const FlowModuleLoad& load, ModuleID, ModuleID) {
                    somebodySaysWhenItBegins.set(load.target.index());
                });
                break;
            case FlowWayIntoFile::Require:
                forEachRequest([&](ModuleID requester, const ModuleRequest& request) {
                    if (request.kind == ModuleRequestKind::Require)
                        begins(requester.index(), request.target);
                });
                forEachLoad(way, [&](const FlowModuleLoad& load, ModuleID file, ModuleID evaluatedModule) {
                    beginsInsideFilesOf(load, file, evaluatedModule);
                    begins(nodeOf(load.function), load.target);
                });
                break;
            case FlowWayIntoFile::ImportCall:
                forEachLoad(way, [&](const FlowModuleLoad& load, ModuleID file, ModuleID evaluatedModule) {
                    if (!isAwaitedInTurn(load))
                        begins(whereJobsRun, load.target);
                    if (file && program.at(file).flags.contains(ModuleFlags::HasTopLevelAwait))
                        beginsInsideFilesOf(load, file, evaluatedModule);
                });
                break;
            case FlowWayIntoFile::NameMadeAtRunTime:
                for (size_t module : inputs.modulesLoadedByName)
                    begins(inputs.namesMadeAtRunTimeAreOnlyImported ? whereJobsRun : anyTime, ModuleID(static_cast<uint32_t>(module)));
                if (inputs.namesMadeAtRunTimeAreOnlyImported) {
                    for (FunctionID holder : inputs.functionsThatImportByName) {
                        ModuleID file = program.at(holder).module;
                        if (!file || !program.at(file).flags.contains(ModuleFlags::HasTopLevelAwait))
                            continue;
                        for (size_t module : inputs.modulesLoadedByName)
                            beginsInsideFilesOf(FlowModuleLoad { holder, ModuleID(static_cast<uint32_t>(module)) }, file, moduleWithBody[holder.index()]);
                    }
                }
                for (size_t module : inputs.modulesNamedByOperationsNobodyKnows)
                    begins(anyTime, ModuleID(static_cast<uint32_t>(module)));
                break;
            case FlowWayIntoFile::Host:
                for (size_t module : inputs.modulesLoadedByHost)
                    begins(anyTime, ModuleID(static_cast<uint32_t>(module)));
                break;
            }
        }
        for (size_t module : inputs.evaluatedModules) {
            if (!somebodySaysWhenItBegins.get(module))
                addEdge(anyTime, static_cast<uint32_t>(module));
        }

        for (FlowRunnerOfJobs runner : allRunnersOfJobs) {
            switch (runner) {
            case FlowRunnerOfJobs::EndOfTurn:
                // Only a file that awaits is unfinished then. Whoever imports it waits for its end, but what an import() written in that file loads.
                break;
            case FlowRunnerOfJobs::RequireOfESModule:
                forEachLoad(FlowWayIntoFile::Require, [&](const FlowModuleLoad& load, ModuleID, ModuleID) {
                    if (program.at(load.target).kind == ModuleKind::SourceText)
                        runsJobs(nodeOf(load.function));
                });
                break;
            case FlowRunnerOfJobs::NameMadeAtRunTime:
                if ((!inputs.modulesLoadedByName.isEmpty() && !inputs.namesMadeAtRunTimeAreOnlyImported) || !inputs.modulesNamedByOperationsNobodyKnows.isEmpty())
                    runsJobs(anyTime);
                break;
            case FlowRunnerOfJobs::Builtin:
                if (inputs.hostRunsWaitingJobs)
                    runsJobs(anyTime);
                break;
            }
        }

        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (ModuleID evaluatedModule = moduleWithBody[index])
                addEdge(evaluatedModule.index(), nodeOf(FunctionID(index)));
        }
        for (const FlowCall& call : inputs.calls) {
            bool isEvaluationByLoader = call.caller == inputs.host() && moduleWithBody[call.callee.index()];
            if (!isEvaluationByLoader)
                addEdge(nodeOf(call.caller), nodeOf(call.callee));
        }
        for (const FlowCallOfList& call : inputs.callsOfLists) {
            RELEASE_ASSERT(call.caller != inputs.host());
            addEdge(nodeOf(call.caller), firstCalleeList + call.calleeList);
        }
        for (uint32_t list = 0; list < inputs.calleeLists.size(); ++list) {
            for (FunctionID callee : inputs.calleeLists[list])
                addEdge(firstCalleeList + list, nodeOf(callee));
        }
        for (size_t module : modulesThatLetTheHostRun)
            addEdge(static_cast<uint32_t>(module), anyTime);
    });
    StronglyConnectedComponents components = computeStronglyConnectedComponents(numberOfNodes, [&](unsigned node) {
        return entriesOfGroup(firstSuccessor, successors, node);
    });

    BitVector leadsToJobs(components.numberOfComponents);
    if (somebodyRunsJobs) {
        leadsToJobs.set(components.componentOf[whereJobsRun]);
        for (uint32_t component = components.componentOf[whereJobsRun] + 1; component < components.numberOfComponents; ++component) {
            bool leads = std::ranges::any_of(components.membersOf(component), [&](uint32_t member) {
                return std::ranges::any_of(entriesOfGroup(firstSuccessor, successors, member), [&](uint32_t successor) {
                    return leadsToJobs.get(components.componentOf[successor]);
                });
            });
            if (leads)
                leadsToJobs.set(component);
        }
    }

    BitVector modulesThatMayBeSeenUnfinished(numberOfModules);
    for (const FlowModuleLoad& load : inputs.moduleLoads) {
        if (!isAwaitedInTurn(load))
            continue;
        forEachModuleLoadedWith({ load.target.index() }, [&](uint32_t module) {
            if (modulesBegunElsewhere.quickGet(module) && (modulesThatMayRunTwice.quickGet(module) || leadsToJobs.get(components.componentOf[module])))
                modulesThatMayBeSeenUnfinished.quickSet(module);
        });
    }

    Vector<uint32_t> modulesByComponent;
    modulesByComponent.reserveInitialCapacity(numberOfModules);
    for (uint32_t module = 0; module < numberOfModules; ++module)
        modulesByComponent.append(module);
    AOT::sortInLine(modulesByComponent, [&](uint32_t left, uint32_t right) {
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
        if (components.isInCycle.get(module) || modulesThatMayBeSeenUnfinished.quickGet(module))
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
            if (modulesThatMayBeSeenUnfinished.quickGet(request.target.index()))
                continue;
            if (!completesBeforeRequester(program.modules[module], request, order.modulesInCycles) || target == component || completed.get(target))
                continue;
            completed.set(target);
            completed.merge(order.componentsCompletedBefore[target]);
        }
    }

    if (somebodyRunsJobs) {
        for (uint32_t module = 0; module < numberOfModules; ++module) {
            if (leadsToJobs.get(components.componentOf[module]))
                order.modulesThatJobsMayInterrupt.set(module);
        }
    }
    return order;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
