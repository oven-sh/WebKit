/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandedIterationEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayBufferViewBuiltinEmitter.h"
#include "AOTForInEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "Butterfly.h"
#include "JSObject.h"
#include "JSPropertyNameEnumerator.h"

namespace JSC { namespace AOT { namespace Typed {

ExpandedIterationEmitter::ExpandedIterationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void ExpandedIterationEmitter::generateDestructureIterable(const ValueContext& value)
{
    uint32_t count = value->count();
    Pointer buffer = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(count);
    callVoidOperation(value, RuntimeOperation::DestructureIterable, { in<Boxed>(value->iterableOperand()), word32(count), buffer });
    Vector<Operand, 8> results;
    for (uint32_t i = 0; i < count; ++i)
        results.append(Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, buffer.value(), i * sizeof(EncodedJSValue)))));
    defineResults(value, results.span());
}

void ExpandedIterationEmitter::generateEnumeratorGetByVal(const ValueContext& value)
{
    ForInEmitter& forIn = emitter<ForInEmitter>();
    Boxed boxedObject = in<Boxed>(value->objectOperand());
    CellPointer object = unboxCell(boxedObject);
    CellPointer enumerator = in<CellPointer>(value->enumeratorOperand());
    Integer position = integerOf(value->positionOperand());
    Integer index = forIn.nameIndexOf(forIn.loadFields(enumerator), position);
    const B3::AbstractHeap& properties = m_heaps.entire(AbstractHeap::NamedProperties);
    LBasicBlock isOrdinaryObject = newBlock();
    LBasicBlock isCompiledObject = newBlock();
    LBasicBlock isInline = newBlock();
    LBasicBlock isOutOfLine = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(forIn.describesOrdinaryObject(enumerator).value(), normalEdge(isOrdinaryObject), normalEdge(isCompiledObject));

    m_out.appendTo(isOrdinaryObject);
    Integer inlineCapacity = zeroExtended(loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedInlineCapacity));
    m_out.branch(below(index, inlineCapacity).value(), normalEdge(isInline), normalEdge(isOutOfLine));

    m_out.appendTo(isInline);
    join.add(Boxed(m_out.load64(m_out.baseIndex(properties, object.value(), index.value(), FTL::ScaleEight, JSObject::offsetOfInlineStorage()))));

    m_out.appendTo(isOutOfLine);
    constexpr ptrdiff_t offsetOfFirstProperty = offsetInButterfly(firstOutOfLineOffset) * static_cast<ptrdiff_t>(sizeof(EncodedJSValue));
    Pointer butterfly = loadField<Pointer>(object, m_heaps.JSObject_butterfly);
    Integer indexInButterfly = sub(inlineCapacity, index);
    Boxed outOfLineProperty(m_out.load64(m_out.baseIndex(properties, butterfly.value(), indexInButterfly.value(), FTL::ScaleEight, offsetOfFirstProperty)));
    ensureStillAliveHere(object);
    join.add(outOfLineProperty);

    m_out.appendTo(isCompiledObject);
    Boxed key = in<Boxed>(value->propertyOperand());
    if (value.shape() == CodeShape::Compact)
        join.add(callOperation<Boxed>(value, RuntimeOperation::EnumeratorGetByVal, { boxedObject, key, enumerator }));
    else
        join.add(Boxed(callStub(value, getByValCachedCells(), { boxedObject.value(), key.value() }).first));

    define(value, join.finish());
}

void ExpandedIterationEmitter::generateRequireAttachedTypedArray(const ValueContext& value)
{
    ArrayBufferViewBuiltinEmitter& views = emitter<ArrayBufferViewBuiltinEmitter>();
    CellPointer view = in<CellPointer>(value->viewOperand());
    LBasicBlock mayBeDetached = newBlock();
    LBasicBlock continuation = newBlock();
    Condition hasVector = notEqual(views.vectorOf(view), pointerConstant(0));
    m_out.branch(logicalAnd(views.hasFixedLength(view), hasVector).value(), normalEdge(continuation), rareEdge(mayBeDetached));

    m_out.appendTo(mayBeDetached);
    callVoidOperation(value, RuntimeOperation::ThrowIfTypedArrayIsDetached, { view }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void ExpandedIterationEmitter::generateCreateAsyncFromSyncIterator(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::CreateAsyncFromSyncIterator, { in<CellPointer>(value->iteratorOperand()), in<Boxed>(value->nextOperand()) }));
}

void ExpandedIterationEmitter::generateThrowNotIterable(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ThrowNotIterable, { in<Boxed>(value->iterableOperand()), word32(value->isSpread()) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
