/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPromiseIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTBuiltinTable.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTStubsCoroutines.h"
#include "AOTSweep.h"
#include "LinkTimeConstant.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(awaitedHasPromiseStructure.word == AwaitConditionWord { AwaitConditionBase::AwaitedCellHeader, JSCell::structureIDOffset(), sizeof(uint32_t), false });
static_assert(awaitedHasPromiseStructure.other == AwaitConditionWord { AwaitConditionBase::Instance, offsetOfPromiseHeader, sizeof(uint32_t), true });
static_assert(awaitedHasPromiseStructure.test == AwaitConditionTest::EqualsOther);

CellPointer PromiseIntrinsicEmitter::newPromise(const OperationContext& context)
{
    return CellPointer(callStub(context, AOT::newPromise(), { }).first);
}

void PromiseIntrinsicEmitter::generateNewPromise(const ValueContext& value)
{
    if (value->endsReactionChain() && !isPromiseOfReactionChain(value.valueID())) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    define(value, newPromise(value));
}

bool PromiseIntrinsicEmitter::isPromiseOfReactionChain(ValueID promise)
{
    for (uint32_t index = promise.index() + 1; index < m_code.numberOfValues(); ++index) {
        ValueID id(index);
        if (!m_code.isConstant(id) && m_code.at(id).opcode == Opcode::StartReactionChain && underlyingCellOf(m_code, m_code.at(id).promiseOperand()) == promise)
            return true;
    }
    return false;
}

void PromiseIntrinsicEmitter::generateCreatePromise(const ValueContext& value)
{
    const Operation& definition = operationAt(value->constructorOperand());
    if (definition.opcode == Opcode::Builtin && m_builtins.canonicalID(definition.builtin()) == m_builtins.canonicalID(m_builtins.at(BuiltinClasses::Promise).constructor)) {
        define(value, newPromise(value));
        return;
    }

    Boxed constructor = in<Boxed>(value->constructorOperand());
    constexpr ptrdiff_t offset = AOT_INSTANCE_OFFSET(realm.linkTimeConstants) + static_cast<size_t>(LinkTimeConstant::Promise) * sizeof(EncodedJSValue);
    Condition isPromiseConstructor = equal(constructor, Boxed(m_out.load64(instanceField(m_heaps.AOTInstance_fieldsFilledOnce, offset))));
    define(value, either<CellPointer>(isPromiseConstructor, [&] {
        return newPromise(value);
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::CreatePromise, { constructor });
    }));
}

Condition PromiseIntrinsicEmitter::hasPromiseStructure(CellPointer cell)
{
    Word32 structureID = loadStructureID(cell);
    return equal(structureID, loadImmutable32(offsetOfPromiseHeader));
}

void PromiseIntrinsicEmitter::fulfillWithoutBarrier(CellPointer promise, Boxed value, bool mayStoreCell)
{
    CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap(mayStoreCell));
    storeField(promise, m_heaps.JSPromise_slot, value);
    storeField(promise, m_heaps.JSPromise_packed, integer(static_cast<int64_t>(fulfilledPromiseBits)));
}

void PromiseIntrinsicEmitter::fulfillAtStore(const ValueContext& store, CellPointer promise, Boxed value, const Type& valueType)
{
    fulfillWithoutBarrier(promise, value, valueType.hasCells());
    barrierAfterStore(store, promise, valueType);
}

void PromiseIntrinsicEmitter::generateResolvePromise(const ValueContext& value)
{
    CellPointer promise = in<CellPointer>(value->promiseOperand());
    Boxed resolution = in<Boxed>(value->valueOperand());
    if (value->valueHasFailedTestForNoThen()) {
        m_out.noteForm(Form::Runtime, StateOfKnowledge::FailsTest);
        callVoidOperation(value, RuntimeOperation::ResolvePromise, { promise, resolution });
        return;
    }
    m_out.noteForm(Form::Open, StateOfKnowledge::NeedsTest);
    const Type& resolutionType = typeOf(value->valueOperand());
    LBasicBlock otherwise = newBlock();
    LBasicBlock continuation = newBlock();
    branchUnless(isZero(loadField<Integer>(promise, m_heaps.JSPromise_packed)), otherwise);
    if (resolutionType.hasObjects())
        branchUnless(emitter<GuardEmitter>().isNotThenable(value, resolution, resolutionType), otherwise);
    fulfillAtStore(value, promise, resolution, resolutionType);
    m_out.jump(continuation);

    m_out.appendTo(otherwise);
    callVoidOperation(value, RuntimeOperation::ResolvePromise, { promise, resolution });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void PromiseIntrinsicEmitter::generateFulfillFreshPromise(const ValueContext& value)
{
    ValueID fulfillment = value->valueOperand();
    CellPointer promise = in<CellPointer>(value->promiseOperand());
    fulfillAtStore(value, promise, in<Boxed>(fulfillment), typeOf(fulfillment));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
