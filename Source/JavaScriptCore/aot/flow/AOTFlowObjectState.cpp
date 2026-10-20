/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectState.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowObjectsInternal.h"
#include "AOTFlowSiteData.h"

namespace JSC { namespace AOT { namespace Typed {

FlowObjectState::FlowObjectState(const Program& program, BuildContext& build)
    : maxSitesPerLayout(build.limit(LimitID::MaxSitesPerLayout))
    , functionsUsingSharedObjects(program.functions.size() + 1)
{
}

void FlowObjectStateDeleter::operator()(FlowObjectState* state) const
{
    delete state;
}

std::unique_ptr<FlowObjectState, FlowObjectStateDeleter> createFlowObjectState(FlowSolver& solver)
{
    return std::unique_ptr<FlowObjectState, FlowObjectStateDeleter>(new FlowObjectState(solver.program(), solver.build()));
}

FlowObjectID findCallSiteAllocation(const FlowSolution& solution, FlowInstanceID instance, const FlowSiteDescription& description)
{
    if (!instance)
        return { };

    AllocationSiteID site = solution.objectState.solveSites.get(FlowSolveSiteKey { solution.graph.at(instance).function, description });
    if (!site)
        return { };

    FlowObjectID shared = solution.objects.at(site).sharedObject;
    if (shared == unknownFlowObject || solution.objects.at(shared).standsForLayout)
        return shared;
    return solution.objectState.solveObjects.get(std::pair { site, instance });
}

FlowObjectID findCoroutineResult(const FlowSolution& solution, FlowInstanceID caller, ValueID origin, FlowInstanceID calleeInstance)
{
    if (!caller)
        return { };
    return solution.objectState.coroutineResults.get(std::tuple { caller, origin, calleeInstance });
}

std::span<const DataNode* const> dataNodesInPreorder(FlowObjectState& state, const Program& program, DataID data)
{
    return state.dataNodesInPreorder.ensure(data, [&] {
        Vector<const DataNode*> nodes;
        forEachDataNodeInPreorder(program.at(data).root, [&](uint32_t ordinal, const DataNode& node) {
            ASSERT_UNUSED(ordinal, ordinal == nodes.size());
            nodes.append(&node);
        });
        return nodes;
    }).iterator->value.span();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
