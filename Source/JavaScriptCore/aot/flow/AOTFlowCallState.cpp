/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowOrderInternal.h"

namespace JSC { namespace AOT { namespace Typed {

std::unique_ptr<FlowCallState, FlowCallStateDeleter> createFlowCallState(FlowSolver& solver)
{
    const Program& program = solver.program();
    BuildContext& build = solver.build();

    std::unique_ptr<FlowCallState, FlowCallStateDeleter> state(new FlowCallState);
    state->limits = { build.limit(LimitID::MaxFlowInstanceValues), build.limit(LimitID::MaxPositionalParts), build.limit(LimitID::MaxDispatchCases) };
    state->modules.moduleOfRuntimeModule = indexRuntimeModules(program);
    state->order.accessesByFunction.grow(program.functions.size());
    return state;
}

void FlowCallStateDeleter::operator()(FlowCallState* state) const
{
    delete state;
}

void FlowScopeAccessesDeleter::operator()(FlowScopeAccesses* accesses) const
{
    delete accesses;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
