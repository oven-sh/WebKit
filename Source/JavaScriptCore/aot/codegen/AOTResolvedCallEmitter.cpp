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

ResolvedCallEmitter::ResolvedCallEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void ResolvedCallEmitter::generateResolveCallTarget(const ValueContext& value)
{
    uint32_t count = value->argumentCount();
    RELEASE_ASSERT(count <= numberOfArgumentGPRs);
    CallOptions options { .pureFunction = pureFunction(PureOperation::ResolveCallTarget, count, true) };
    LValue target = callStub(value, resolveCallValue(count), { in<Boxed>(value->calleeOperand()).value() }, options).first;
    define(value, Operand(value.representation(), target));
}

void ResolvedCallEmitter::generateCallResolved(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    std::span<const ValueID> arguments = value->argumentsOperands();
    RELEASE_ASSERT(arguments.size() <= numberOfArgumentGPRs);

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
    define(value, emitter<CallEmitter>().callByConvention(value, call, { .throwSite = CallEmitter::siteThatNamesCallee(value) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
