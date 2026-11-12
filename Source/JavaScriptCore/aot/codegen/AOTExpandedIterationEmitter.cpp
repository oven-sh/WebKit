/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandedIterationEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTForInEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsArrays.h"
#include "AOTStubsCollections.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include "Butterfly.h"
#include "JSArrayBufferView.h"
#include "JSObject.h"
#include "JSPropertyNameEnumerator.h"

namespace JSC { namespace AOT { namespace Typed {

void ExpandedIterationEmitter::generateDestructureIterable(const ValueContext& value)
{
    uint32_t count = value->count();
    Boxed iterable = in<Boxed>(value->iterableOperand());
    auto takeOutOfLine = [&] {
        Pointer buffer = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(count);
        if (value->readsListsByIndex())
            callStub(value, destructureIterable(), { iterable.value(), m_out.constInt32(count), buffer.value() }, { .writesArgumentBuffer = true });
        else
            callVoidOperation(value, RuntimeOperation::DestructureIterable, { iterable, word32(count), buffer });
        Vector<Operand, 8> results;
        for (uint32_t i = 0; i < count; ++i)
            results.append(Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, buffer.value(), i * sizeof(EncodedJSValue)))));
        return results;
    };
    static_assert(formDependsOnLoopOf(Opcode::DestructureIterable) && namedFormOf(Opcode::DestructureIterable, StateOfKnowledge::Always, Place::Loop) == Form::Open);
    if (!value->readsListsByIndex() || !count || !generator().usesLoopForms(value.block())) {
        defineResults(value, takeOutOfLine().span());
        return;
    }
    m_out.noteForm(Form::Open, StateOfKnowledge::Always, Place::Loop);

    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    LBasicBlock outOfLine = newBlock();
    LBasicBlock continuation = newBlock();
    if (typeOf(value->iterableOperand()).primitives & ~cellPrimitives)
        branchUnless(isCell(iterable), outOfLine);
    CellPointer list = unboxCell(iterable);
    branchUnless(hasCellType(list, ArrayType), outOfLine);
    branchUnless(elements.isPristineArray(list, { PristineAspects::Storage, PristineAspects::Names }), outOfLine);
    branchUnless(elements.holdsBoxedElements(list), outOfLine);
    ArrayElements storage = elements.elementsOf(list);
    branchUnless(aboveOrEqual(elements.publicLength(storage), integer(count)), outOfLine);
    Vector<ValueFromBlock, 8> takenInLine;
    Vector<Boxed, 8> loaded;
    for (uint32_t i = 0; i < count; ++i) {
        loaded.append(elements.loadBoxedElement(storage, integer(i)));
        branchUnless(isNotEmpty(loaded.last()), outOfLine);
    }
    for (Boxed element : loaded)
        takenInLine.append(m_out.anchor(element.value()));
    m_out.jump(continuation);

    m_out.appendTo(outOfLine);
    Vector<ValueFromBlock, 8> takenOutOfLine;
    for (const Operand& result : takeOutOfLine())
        takenOutOfLine.append(m_out.anchor(result.value));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    Vector<Operand, 8> results;
    for (uint32_t i = 0; i < count; ++i)
        results.append(Boxed(m_out.phi(Machine::Int64, takenInLine[i], takenOutOfLine[i])));
    defineResults(value, results.span());
}

void ExpandedIterationEmitter::generateAppendIterable(const ValueContext& value)
{
    if (generator().m_literalLeftToSpreadRightBehind == value->arrayOperand()) [[unlikely]] {
        ArrayAllocationEmitter(generator()).bearLiteralHoldingList(value, in<Boxed>(value->iterableOperand()).value(), appendIterable());
        return;
    }
    if (value->readsListsByIndex()) {
        callStub(value, appendIterable(), { in<CellPointer>(value->arrayOperand()).value(), in<Boxed>(value->iterableOperand()).value() });
        return;
    }
    callVoidOperation(value, RuntimeOperation::AppendIterable, { in<CellPointer>(value->arrayOperand()), in<Boxed>(value->iterableOperand()) });
}

void ExpandedIterationEmitter::generateNextSetElement(const ValueContext& value)
{
    define(value, Boxed(callStub(value, nextSetElement(), { in<Boxed>(value->iteratorOperand()).value() }).first));
}

void GuardEmitter::generateIsEnumeratorCurrent(const ValueContext& value)
{
    ForInEmitter& forIn = emitter<ForInEmitter>();
    CellPointer enumerator = in<CellPointer>(value->enumeratorOperand());
    ForInEmitter::EnumeratorFields fields = forIn.loadFields(enumerator);
    Integer nameIndex = forIn.nameIndexOf(fields, integerOf(value->positionOperand()));
    Condition isInStructurePart = forIn.isInStructurePart(enumerator, nameIndex);
    if (value->currentNamesOperand()) {
        defineTestByBranching(value, [&](BranchTargets targets) {
            branch(logicalAnd(isInStructurePart, below(nameIndex, integerOf(value->currentNamesOperand()))), targets);
        });
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer object = cellOrElse(value, value->objectOperand(), targets.notTaken);
        branch(logicalAnd(isInStructurePart, forIn.hasCachedKeys(enumerator, fields, object)), targets);
    });
}

void ExpandedIterationEmitter::generateEnumeratorGetByVal(const ValueContext& value)
{
    ForInEmitter& forIn = emitter<ForInEmitter>();
    Boxed boxedObject = in<Boxed>(value->objectOperand());
    Boxed key = in<Boxed>(value->propertyOperand());
    CellPointer object = unboxCell(boxedObject);
    CellPointer enumerator = in<CellPointer>(value->enumeratorOperand());
    Integer position = integerOf(value->positionOperand());
    ForInEmitter::EnumeratorFields fields = forIn.loadFields(enumerator);
    Integer index = forIn.nameIndexOf(fields, position);
    if (emitter<ElementAccessEmitter>().checksBounds()) {
        emitter<ElementAccessEmitter>().stopUnlessInside(value, isCell(boxedObject));
        emitter<ElementAccessEmitter>().stopUnlessInside(value, forIn.hasCachedKeys(enumerator, fields, object));
    }
    const Machine::AbstractHeap& properties = m_heaps.entire(AbstractHeap::NamedProperties);
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
    join.add(Boxed(m_out.load64(m_out.baseIndex(properties, object.value(), index.value(), ScaleEight, JSObject::offsetOfInlineStorage()))));

    m_out.appendTo(isOutOfLine);
    constexpr ptrdiff_t offsetOfFirstProperty = offsetInButterfly(firstOutOfLineOffset) * static_cast<ptrdiff_t>(sizeof(EncodedJSValue));
    Pointer butterfly = loadField<Pointer>(object, m_heaps.JSObject_butterfly);
    Integer indexInButterfly = sub(inlineCapacity, index);
    Boxed outOfLineProperty(m_out.load64(m_out.baseIndex(properties, butterfly.value(), indexInButterfly.value(), ScaleEight, offsetOfFirstProperty)));
    ensureStillAliveHere(object);
    join.add(outOfLineProperty);

    m_out.appendTo(isCompiledObject);
    join.add(Boxed(callStub(value, getByValCachedCells(), { boxedObject.value(), key.value() }).first));

    define(value, join.finish());
}

void ExpandedIterationEmitter::generateCreateAsyncFromSyncIterator(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::CreateAsyncFromSyncIterator, { in<CellPointer>(value->iteratorOperand()), in<Boxed>(value->nextOperand()) }));
}

void ExpandedIterationEmitter::generateRequireAttachedTypedArray(const ValueContext& value)
{
    CellPointer view = in<CellPointer>(value->viewOperand());
    Condition hasFixedLength = hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask);
    Condition hasMemory = isNotZero(Integer(loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector).value()));
    LBasicBlock mayBeDetached = newBlock();
    LBasicBlock done = newBlock();
    ScopeFork fork = m_out.forkScope();
    branchUnless(logicalAnd(hasFixedLength, hasMemory), mayBeDetached);
    m_out.jump(done);

    m_out.appendToJoin(mayBeDetached, fork);
    callVoidOperation(value, RuntimeOperation::ThrowIfTypedArrayIsDetached, { view }, { .isOnRarePath = true });
    m_out.jump(done);

    m_out.appendToJoin(done, fork);
}

void ExpandedIterationEmitter::generateRequireLengthFitsList(const ValueContext& value)
{
    Boxed source = in<Boxed>(value->sourceOperand());
    auto goOn = [] { };
    auto askRunTime = [&] {
        callVoidOperation(value, RuntimeOperation::ThrowIfTooLongForList, { unboxCell(source) }, { .isOnRarePath = true });
    };
    if (value->lengthOperand()) {
        ifElse(lessThanOrEqual(integerOf(value->lengthOperand()), IndexingHeader::maximumLength), goOn, askRunTime);
        return;
    }

    ifElse(isCell(source), [&] {
        CellPointer cell = unboxCell(source);
        Condition isText = hasCellType(cell, StringType);
        Condition isView = hasCellTypeIn(cell, { static_cast<JSType>(FirstTypedArrayType), static_cast<JSType>(LastTypedArrayTypeExcludingDataView) });
        ifElse(logicalNot(logicalOr(isText, isView)), goOn, askRunTime);
    }, goOn);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
