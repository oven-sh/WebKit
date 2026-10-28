/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinCalls.h"
#include "AOTConvention.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsCalls.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

Boxed CallEmitter::callWithList(const OperationContext& context, Boxed callee, Boxed thisValue, Word32 count, Pointer list, const CallOptions& options)
{
    return Boxed(callStub(context, AOT::callWithList(), { callee.value(), thisValue.value(), zeroExtended(count).value(), list.value() }, options).first);
}

void CallEmitter::generateCallList(const ValueContext& value)
{
    Boxed callee = in<Boxed>(value->calleeOperand());
    emitter<AccessEmitter>().recordArrival(value, callee);
    Boxed thisValue = thisOrUndefined(value->thisValueOperand());
    LValue list = in<CellPointer>(value->listOperand()).value();
    ConventionCall call {
        .stub = callWithArray(),
        .stubOperands = singleElementSpan(list),
        .callee = callee.value(),
        .thisValue = thisValue.value(),
    };
    if (!value.returnCall()) {
        define(value, callByConvention(value, call, { .throwSite = siteThatNamesCallee(value) }));
        return;
    }

    requireCallableBeforeTailCall(value, callee, thisValue);
    tailCallByConvention(value, call);
}

Boxed CallEmitter::constructValue(const OperationContext& context, ConstructSyntax syntax, Boxed callee, Boxed newTarget, std::span<const ValueID> argumentsAfterNewTarget, std::optional<ThrowSite> siteThatNamesCallee)
{
    Vector<LValue, 8> arguments { newTarget.value() };
    for (ValueID argument : argumentsAfterNewTarget)
        arguments.append(in<Boxed>(argument).value());
    LValue count = m_out.constInt64(static_cast<int64_t>(arguments.size()));

    ConventionCall call {
        .stub = AOT::constructValue(syntax),
        .stubOperands = singleElementSpan(count),
        .callee = callee.value(),
        .thisValue = undefined().value(),
        .arguments = arguments.span(),
    };
    return callByConvention(context, call, { .throwSite = siteThatNamesCallee, .constructs = true });
}

Boxed CallEmitter::superConstructWithList(const OperationContext& context, Boxed parent, Boxed newTarget, Word32 count, Pointer list)
{
    return Boxed(callStub(context, AOT::superConstructWithList(), { parent.value(), newTarget.value(), zeroExtended(count).value(), list.value() }, { .constructs = true }).first);
}

void CallEmitter::generateConstruct(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    Boxed callee = in<Boxed>(value->calleeOperand());
    emitter<AccessEmitter>().recordArrival(value, callee);
    Boxed newTarget = inOptional<Boxed>(value->newTargetOperand()).value_or(callee);
    define(value, constructValue(value, ConstructSyntax::NewExpression, callee, newTarget, value->argumentsOperands(), siteThatNamesCallee(value)));
}

void CallEmitter::generateConstructList(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    Boxed callee = in<Boxed>(value->calleeOperand());
    emitter<AccessEmitter>().recordArrival(value, callee);
    Boxed newTarget = inOptional<Boxed>(value->newTargetOperand()).value_or(callee);
    Boxed list = boxCell(in<CellPointer>(value->listOperand()));
    define(value, callOperation<Boxed>(value, RuntimeOperation::ConstructWithArray, { callee, newTarget, list }, { .constructs = true }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
