/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolvedCallEmitter.h"

#if ENABLE(AOT)

#include "AOTCallEmitter.h"
#include "AOTConvention.h"
#include "AOTPureOperations.h"
#include "AOTStubsCalls.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

void ResolvedCallEmitter::generateResolveCallTarget(const ValueContext& value)
{
    StubKey stub = resolveCallValue(value->argumentCount());
    CallOptions options { .pureFunction = pureFunction(PureOperation::ResolveCallTarget, stub.count(), true) };
    define(value, Pointer(callStub(value, stub, { in<Boxed>(value->calleeOperand()).value() }, options).first));
}

void ResolvedCallEmitter::generateCallResolved(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    std::span<const ValueID> arguments = value->argumentsOperands();

    Boxed thisValue = value->thisValueOperand() ? in<Boxed>(value->thisValueOperand()) : undefined();
    Vector<LValue, 8> values;
    for (ValueID argument : arguments)
        values.append(in<Boxed>(argument).value());

    ConventionCall call {
        .callee = in<Boxed>(value->calleeOperand()).value(),
        .thisValue = thisValue.value(),
        .arguments = values.span(),
    };
    CallEmitter& calls = emitter<CallEmitter>();
    define(value, calls.callAddressByConvention(value, in<Pointer>(value->targetOperand()), call, { .throwSite = calls.siteThatNamesCallee(value) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
