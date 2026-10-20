/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowThrown.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"

namespace JSC { namespace AOT { namespace Typed {

FlowObjectID runtimeErrorObject(FlowSolver& solver)
{
    FlowObjectID& object = solver.callState().thrown.runtimeError;
    if (!object) {
        object = allocateAtCallSite(solver, {
            nativeCodeSite(solver),
            { .kind = FlowSiteKind::Opaque, .origin = FlowSiteOrigin::RuntimeError, .builtinClass = BuiltinClasses::Error },
        });
    }
    return object;
}

void throwRuntimeError(FlowSolver& solver, FlowNodeID thrown, const Witness& witness)
{
    if (!thrown)
        return;
    solver.joinObject(thrown, runtimeErrorObject(solver), witness);
}

void connectThrown(FlowSolver& solver, FlowNodeID from, FlowNodeID thrown, const Witness& witness)
{
    solver.connect(from, thrownNodeOrNativeCode(solver, thrown), { }, witness);
}

void throwUnknownValue(FlowSolver& solver, FlowNodeID thrown, const Witness& witness)
{
    if (!thrown)
        return;
    solver.joinUnknown(thrown, witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
