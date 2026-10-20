/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPromiseBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTGuardEmitter.h"
#include "AOTPromiseIntrinsicEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

PromiseBuiltinEmitter::PromiseBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

Boxed PromiseBuiltinEmitter::argumentOrUndefined(const InlineCall& call, unsigned index)
{
    ValueID id = call.argumentOrNull(index);
    return id ? boxedOperand(call.context(), id) : undefined();
}

void PromiseBuiltinEmitter::generatePromiseResolve(const InlineCall& call)
{
    PromiseIntrinsicEmitter& promises = emitter<PromiseIntrinsicEmitter>();
    Boxed resolution = argumentOrUndefined(call, 0);
    LBasicBlock notThenable = newBlock();
    Join<CellPointer> join(m_out);
    if (!call.canMiss())
        m_out.jump(notThenable);
    else {
        LBasicBlock isCellBlock = newBlock();
        LBasicBlock isPromise = newBlock();
        LBasicBlock isOther = newBlock();
        m_out.branch(isCell(resolution).value(), normalEdge(isCellBlock), normalEdge(notThenable));

        m_out.appendTo(isCellBlock);
        CellPointer cell = unboxCell(resolution);
        m_out.branch(promises.hasPromiseStructure(cell).value(), normalEdge(isPromise), normalEdge(isOther));

        m_out.appendTo(isPromise);
        join.add(cell);

        m_out.appendTo(isOther);
        Condition isNotThenable = emitter<GuardEmitter>().isNotThenable(call.context(), resolution, typeOf(call.argument(0)));
        m_out.branch(isNotThenable.value(), normalEdge(notThenable), rareEdge(missBlockOf(call)));
    }

    m_out.appendTo(notThenable);
    CellPointer promise = promises.newPromise(call.context());
    promises.fulfillWithoutBarrier(promise, resolution);
    join.add(promise);
    finishInlineCall(call, join.finish());
}

void PromiseBuiltinEmitter::generatePromiseThen(const InlineCall& call)
{
    CellPointer promise = in<CellPointer>(call.receiver());
    Boxed onFulfilled = argumentOrUndefined(call, 0);
    Boxed onRejected = argumentOrUndefined(call, 1);
    CellPointer derived = emitter<PromiseIntrinsicEmitter>().newPromise(call.context());
    callVoidOperation(call.context(), RuntimeOperation::PromiseThen, { promise, onFulfilled, onRejected, derived });
    finishInlineCall(call, derived);
}

void PromiseBuiltinEmitter::generatePromiseCatch(const InlineCall& call)
{
    CellPointer promise = in<CellPointer>(call.receiver());
    finishInlineCall(call, callOperation<CellPointer>(call.context(), RuntimeOperation::PromiseCatch, { promise, argumentOrUndefined(call, 0) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
