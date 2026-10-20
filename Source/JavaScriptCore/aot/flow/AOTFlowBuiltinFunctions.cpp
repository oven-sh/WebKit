/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTForwardedCalls.h"

namespace JSC { namespace AOT { namespace Typed {

// The records of an overload that only forwards a call say which operand arrives at which position of the callee. The function of the library copies the operands in a loop, which says less.
RuntimeFunctionID analyzedFunctionOf(const BuiltinOverload& overload)
{
    if (overload.code.model)
        return overload.code.model;
    if (forwardsCall(overload))
        return { };
    return overload.libraryFunction();
}

FunctionID functionOf(FlowSolver& solver, RuntimeFunctionID id, BuiltinID user, const Witness& witness)
{
    if (const RuntimeFunctionEntry* entry = findRuntimeFunction(solver.program(), id))
        return entry->function;

    solver.build().diagnostics().add(InternalError {
        .kind = InternalErrorKind::FlowRuntimeFunctionMissing,
        .function = hasFunction(witness) ? FunctionID(witness.function) : FunctionID(),
        .value = witness.value != Wire::none ? ValueID(witness.value) : ValueID(),
        .count = user ? user.index() : 0,
        .location = witness.location,
    });
    return { };
}

// The call reaches this function through the entry's own function object: on the slow path of inline code, where a call with a typed result is not expanded, and where nothing is declared and the call stays a call of that object.
static RuntimeFunctionID functionCalledByValue(const BuiltinOverload& overload)
{
    const BuiltinImplementation& implementation = overload.code.implementation;
    if ((overload.isUndeclared() || forwardsCall(overload)) && implementation.kind == BuiltinImplementationKind::Compiled)
        return implementation.function;
    return overload.slowPathFunction();
}

// A call by value enters the function the way native code does. The records of the overload still say what the call does with its operands.
void enterFunctionCalledByValue(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowBuiltinCall& call = overloadCall.call;
    RuntimeFunctionID id = functionCalledByValue(overloadCall.overload);
    if (!id)
        return;

    Witness witness = witnessOf(solver, call);
    if (FunctionID function = functionOf(solver, id, call.builtin, witness))
        enterFromNativeCode(solver, function, FlowObjectID(), witness);
}

FlowEnteredImplementation enterImplementation(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowBuiltinCall& call = overloadCall.call;
    RuntimeFunctionID id = analyzedFunctionOf(overloadCall.overload);
    if (!id)
        return FlowEnteredImplementation::None;
    FunctionID function = functionOf(solver, id, call.builtin, witnessOf(solver, call));
    if (!function)
        return FlowEnteredImplementation::None;

    FlowCallRequest inner = call.request;
    if (call.mode == BuiltinCallMode::Construct && !inner.operands.newTarget && !inner.operands.newTargetObject)
        inner.operands.newTargetObject = ensureBuiltinObject(solver, call.builtin);

    FlowContext context = contextOf(solver, inner, call.arguments, FlowObjectSet());
    linkFunctionCall(solver, { inner, call.arguments, function, FlowObjectID(), context, FlowInstancePolicy::AlwaysAtCallSite });
    return FlowEnteredImplementation::Entered;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
