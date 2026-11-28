/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTStubsCoroutines.h"

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfPromiseIntrinsics()
{
    return { Opcode::NewPromise, Opcode::CreatePromise, Opcode::ResolvePromise, Opcode::RejectPromise };
}

void StraightEmitter::generatePromiseIntrinsic(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::NewPromise:
        callStub(value, AOT::newPromise(), { });
        define(value);
        return;
    case Opcode::CreatePromise:
        callOperation(value, RuntimeOperation::CreatePromise, { value.constructorOperand() });
        define(value);
        return;
    case Opcode::ResolvePromise:
        callOperation(value, RuntimeOperation::ResolvePromise, { value.promiseOperand(), value.valueOperand() });
        return;
    case Opcode::RejectPromise:
        callOperation(value, RuntimeOperation::RejectPromise, { value.promiseOperand(), value.valueOperand() });
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
