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
#include <array>

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

    std::array<LValue, 1> stubOperands { in<Pointer>(value->targetOperand()).value() };
    Boxed thisValue = value->thisValueOperand() ? in<Boxed>(value->thisValueOperand()) : undefined();
    Vector<LValue, 8> values;
    for (ValueID argument : arguments)
        values.append(in<Boxed>(argument).value());

    ConventionCall call {
        .stub = callResolvedValue(),
        .stubOperands = stubOperands,
        .callee = in<Boxed>(value->calleeOperand()).value(),
        .thisValue = thisValue.value(),
        .count = m_out.constInt64(static_cast<int64_t>(arguments.size())),
        .arguments = values.span(),
    };
    CallEmitter& calls = emitter<CallEmitter>();
    define(value, calls.callByConvention(value, call, { .throwSite = calls.siteThatNamesCallee(value) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
