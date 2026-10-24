/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPromiseIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTStubsCoroutines.h"
#include "LinkTimeConstant.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(awaitedHasPromiseStructure.word == AwaitConditionWord { AwaitConditionBase::AwaitedCellHeader, JSCell::structureIDOffset(), sizeof(uint32_t), false });
static_assert(awaitedHasPromiseStructure.other == AwaitConditionWord { AwaitConditionBase::Instance, offsetOfPromiseHeader, sizeof(uint32_t), true });
static_assert(awaitedHasPromiseStructure.test == AwaitConditionTest::EqualsOther);

PromiseIntrinsicEmitter::PromiseIntrinsicEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

CellPointer PromiseIntrinsicEmitter::newPromise(const OperationContext& context)
{
    return CellPointer(callStub(context, AOT::newPromise(), { }).first);
}

void PromiseIntrinsicEmitter::generateNewPromise(const ValueContext& value)
{
    define(value, newPromise(value));
}

Condition PromiseIntrinsicEmitter::isPromiseConstructor(Boxed value)
{
    // The word is empty until operationAOTCreatePromise or operationAOTLinkTimeConstant fills it, and no value is empty.
    constexpr ptrdiff_t offset = AOT_INSTANCE_OFFSET(realm.linkTimeConstants) + static_cast<size_t>(LinkTimeConstant::Promise) * sizeof(EncodedJSValue);
    return equal(value, Boxed(m_out.load64(mutableInstanceField(offset))));
}

void PromiseIntrinsicEmitter::generateCreatePromise(const ValueContext& value)
{
    Boxed constructor = in<Boxed>(value->constructorOperand());
    if (value.shape() == CodeShape::Compact) {
        define(value, callOperation<CellPointer>(value, RuntimeOperation::CreatePromise, { constructor }));
        return;
    }

    define(value, either<CellPointer>(isPromiseConstructor(constructor), [&] {
        return newPromise(value);
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::CreatePromise, { constructor }, { .isOnRarePath = true });
    }));
}

Condition PromiseIntrinsicEmitter::hasPromiseStructure(CellPointer cell)
{
    return equal(loadStructureID(cell), loadImmutable32(offsetOfPromiseHeader));
}

void PromiseIntrinsicEmitter::branchUnlessFulfillable(const OperationContext& context, CellPointer promise, Boxed value, const Type& valueType, LBasicBlock otherwise)
{
    branchUnless(isZero(loadField<Integer>(promise, m_heaps.JSPromise_packed)), otherwise);
    if (valueType.hasObjects())
        branchUnless(emitter<GuardEmitter>().isNotThenable(context, value, valueType), otherwise);
}

void PromiseIntrinsicEmitter::fulfillWithoutBarrier(CellPointer promise, Boxed value)
{
    storeField(promise, m_heaps.JSPromise_slot, value);
    storeField(promise, m_heaps.JSPromise_packed, integer(static_cast<int64_t>(fulfilledPromiseBits)));
}

void PromiseIntrinsicEmitter::fulfillAtStore(const ValueContext& store, CellPointer promise, Boxed value, const Type& valueType)
{
    fulfillWithoutBarrier(promise, value);
    barrierAfterStore(store, promise, value, valueType);
}

void PromiseIntrinsicEmitter::generateResolvePromise(const ValueContext& value)
{
    CellPointer promise = in<CellPointer>(value->promiseOperand());
    Boxed resolution = in<Boxed>(value->valueOperand());
    const Type& resolutionType = typeOf(value->valueOperand());
    if (value.shape() == CodeShape::Compact) {
        callVoidOperation(value, RuntimeOperation::ResolvePromise, { promise, resolution });
        return;
    }

    LBasicBlock otherwise = newBlock();
    LBasicBlock continuation = newBlock();
    branchUnlessFulfillable(value, promise, resolution, resolutionType, otherwise);
    fulfillAtStore(value, promise, resolution, resolutionType);
    m_out.jump(continuation);

    m_out.appendTo(otherwise);
    callVoidOperation(value, RuntimeOperation::ResolvePromise, { promise, resolution });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void PromiseIntrinsicEmitter::generateRejectPromise(const ValueContext& value)
{
    CellPointer promise = in<CellPointer>(value->promiseOperand());
    Boxed reason = in<Boxed>(value->valueOperand());
    callVoidOperation(value, RuntimeOperation::RejectPromise, { promise, reason });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
