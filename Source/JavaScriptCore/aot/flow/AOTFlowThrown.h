/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowCalls.h"

namespace JSC { namespace AOT { namespace Typed {

struct FlowThrownState {
    FlowObjectID runtimeError;
};

FlowObjectID runtimeErrorObject(FlowSolver&);
void throwRuntimeError(FlowSolver&, FlowNodeID thrown, const Witness&);

inline FlowNodeID thrownNodeOrNativeCode(const FlowSolver& solver, FlowNodeID thrown)
{
    return thrown ? thrown : solver.graph().nativeCode();
}

void connectThrown(FlowSolver&, FlowNodeID from, FlowNodeID thrown, const Witness&);
void throwUnknownValue(FlowSolver&, FlowNodeID thrown, const Witness&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
