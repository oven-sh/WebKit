/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include "AOTStubsArrays.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

ArrayBuiltinEmitter::ArrayBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

bool ArrayBuiltinEmitter::checkChosenForm(const InlineCall& call, bool isHandled)
{
    if (isHandled)
        return true;
    fail(call.context(), InternalErrorKind::OpcodeWithoutCode);
    finishInlineCall(call, emitter<ConstantEmitter>().zeroOf(resultRepresentationOf(call)));
    return false;
}

CellPointer ArrayBuiltinEmitter::receiverOf(const InlineCall& call)
{
    return in<CellPointer>(call.receiver());
}

Operand ArrayBuiltinEmitter::resultFromRawElement(const InlineCall& call, const ArrayStorageForm& form, Operand rawElement)
{
    Representation representation = resultRepresentationOf(call);
    if (representation == Representation::Void)
        return { };
    return emitter<ElementAccessEmitter>().convertRawElement(form, rawElement, representation, resultTypeOf(call));
}

Operand ArrayBuiltinEmitter::undefinedResult(const InlineCall& call)
{
    Representation representation = resultRepresentationOf(call);
    if (representation == Representation::Void)
        return { };
    return emitter<ElementAccessEmitter>().absentElement(call.context(), representation);
}

ArrayStorageFormList ArrayBuiltinEmitter::candidateFormsOf(const InlineCall& call)
{
    const Type& array = typeOf(call.receiver());
    Type contents = typeOfContentsRead({ m_program, m_function, types(), TypeLevel::Predicted }, array, ContentsSlot::Element);
    const ArrayStorageForm& expected = ArrayAllocationEmitter(generator()).formAtAllocation(array);
    // new Array(n) declares an array that holds nothing, which predicts nothing about what is stored later.
    bool predictsContents = !contents.isNever();
    ArrayStorageFormList forms { &expected };
    for (const ArrayStorageForm& form : arrayStorageForms) {
        if (form.kind == expected.kind)
            continue;
        if (!predictsContents || widestTypeOf(form.element).overlaps(contents, m_program))
            forms.append(&form);
    }
    return forms;
}

void ArrayBuiltinEmitter::forEachFormOrMiss(const InlineCall& call, Word32 maskedMode, const ScopedLambda<void(const ArrayStorageForm&)>& functor)
{
    emitter<ElementAccessEmitter>().forEachFormByWritableShape(maskedMode, candidateFormsOf(call), missBlockOf(call), functor);
}

void ArrayBuiltinEmitter::generateArrayIterator(const InlineCall& call)
{
    Word32 kind = word32(static_cast<int32_t>(call.parameterWord(0)));
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::NewArrayIterator, { receiverOf(call), kind }));
}

void ArrayBuiltinEmitter::generateArrayAt(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss() && call.numberOfArguments() && typeOf(call.argument(0)).isNumber()))
        return;
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer array = receiverOf(call);
    ValueID position = call.argument(0);
    LBasicBlock isAbsent = newBlock();
    OperandJoin join(m_out, resultRepresentationOf(call));
    Integer index = typeOf(position).isInteger() ? integerOperand(position) : elements.indexOrElse(position, false, missBlockOf(call));
    ArrayElements storage = elements.elementsOf(array);
    Integer length = elements.publicLength(storage);
    Integer relative = select(lessThan(index, integer(0)), add(index, length), index);
    branchUnless(below(relative, length), isAbsent);
    forEachFormOrMiss(call, elements.indexingMode(array, arrayStorageTestMask), [&](const ArrayStorageForm& form) {
        Operand raw = elements.loadRawElement(form, storage, relative);
        branchUnless(logicalNot(elements.isHoleIn(call.receiver(), form, raw)), isAbsent);
        join.add(resultFromRawElement(call, form, raw));
    });

    m_out.appendTo(isAbsent);
    join.add(undefinedResult(call));
    finishInlineCall(call, join.finish());
}

CellPointer ArrayBuiltinEmitter::joinOrMiss(const InlineCall& call)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer array = receiverOf(call);
    CellPointer null(m_out.intPtrZero);
    Word32 shape = elements.indexingMode(array, arrayStorageTestMask);
    Condition hasForm;
    for (const ArrayStorageForm& form : arrayStorageForms)
        hasForm = logicalOr(hasForm, elements.hasShape(shape, form));
    missUnless(call, hasForm);
    CellPointer separator = call.numberOfArguments() ? cellOperand(call.argument(0)) : null;
    CellPointer joined = callOperation<CellPointer>(call.context(), RuntimeOperation::ArrayJoin, { array, separator });
    missUnless(call, notEqual(joined, null));
    return joined;
}

void ArrayBuiltinEmitter::generateArrayJoin(const InlineCall& call)
{
    bool hasStringSeparator = call.numberOfArguments() == 1 && typeOf(call.argument(0)).isAnyString();
    if (!checkChosenForm(call, call.canMiss() && (!call.numberOfArguments() || hasStringSeparator)))
        return;
    finish(call, joinOrMiss(call));
}

Operand ArrayBuiltinEmitter::removeFirstElement(const InlineCall& call, const ArrayStorageForm& form, const ArrayElements& storage, LBasicBlock isAbsent)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    Integer last = sub(elements.publicLength(storage), integer(1));
    Operand first = elements.loadRawElement(form, storage, integer(0));
    branchUnless(logicalNot(elements.isHoleIn(call.receiver(), form, first)), isAbsent);

    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock moved = newBlock();
    ValueFromBlock start = m_out.anchor(integer(0).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer index(m_out.phi(B3::Int64, start));
    branch(below(index, last), { normalEdge(element), normalEdge(moved) });

    m_out.appendTo(element);
    Integer next = add(index, integer(1));
    elements.storeRawElement(form, storage, index, elements.loadRawElement(form, storage, next));
    m_out.addIncomingToPhi(index.value(), m_out.anchor(next.value()));
    m_out.jump(loop);

    m_out.appendTo(moved);
    elements.storeRawElement(form, storage, last, elements.rawHole(form));
    elements.setPublicLength(storage, last);
    if (form.holdsCells)
        emitter<BarrierEmitter>().barrier(call.context(), storage.array);
    return first;
}

void ArrayBuiltinEmitter::generateArrayShift(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss()))
        return;
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer array = receiverOf(call);
    LBasicBlock hasNoElements = newBlock();
    OperandJoin join(m_out, resultRepresentationOf(call));
    ArrayElements storage = elements.elementsOf(array);
    Integer length = elements.publicLength(storage);
    forEachFormOrMiss(call, elements.indexingMode(array, IndexingShapeAndWritabilityMask), [&](const ArrayStorageForm& form) {
        branchUnless(isNotZero(length), hasNoElements);
        missUnless(call, belowOrEqual(length, integer(maxLengthForElementwiseMove)));
        join.add(resultFromRawElement(call, form, removeFirstElement(call, form, storage, missBlockOf(call))));
    });

    m_out.appendTo(hasNoElements);
    join.add(undefinedResult(call));
    finishInlineCall(call, join.finish());
}

void ArrayBuiltinEmitter::generateArrayPop(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss()))
        return;
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer array = receiverOf(call);
    LBasicBlock hasNoElements = newBlock();
    OperandJoin join(m_out, resultRepresentationOf(call));
    ArrayElements storage = elements.elementsOf(array);
    Integer length = elements.publicLength(storage);
    Integer last = sub(length, integer(1));
    forEachFormOrMiss(call, elements.indexingMode(array, IndexingShapeAndWritabilityMask), [&](const ArrayStorageForm& form) {
        branchUnless(isNotZero(length), hasNoElements);
        Operand raw = elements.loadRawElement(form, storage, last);
        missUnless(call, logicalNot(elements.isHoleIn(call.receiver(), form, raw)));
        elements.storeRawElement(form, storage, last, elements.rawHole(form));
        elements.setPublicLength(storage, last);
        join.add(resultFromRawElement(call, form, raw));
    });

    m_out.appendTo(hasNoElements);
    join.add(undefinedResult(call));
    finishInlineCall(call, join.finish());
}

void ArrayBuiltinEmitter::generateArraySlice(const InlineCall& call)
{
    bool hasIntegerBounds = call.numberOfArguments() <= 2 && std::ranges::all_of(call.arguments(), [&](ValueID bound) {
        return typeOf(bound).isInteger();
    });
    if (!checkChosenForm(call, call.canMiss() && hasIntegerBounds))
        return;
    CellPointer array = receiverOf(call);
    StubCallResult copy;
    if (!call.numberOfArguments())
        copy = callStub(call.context(), arrayCopy(), { array.value() });
    else {
        Integer start = integerOperand(call.argument(0));
        Integer end = call.numberOfArguments() > 1 ? integerOperand(call.argument(1)) : integer(arraySliceToEnd);
        copy = callStub(call.context(), arraySlice(), { array.value(), start.value(), end.value() });
    }
    Boxed copied(copy.first);
    missUnless(call, isNotEmpty(copied));
    finish(call, copied);
}

void ArrayBuiltinEmitter::generateArrayUnshift(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss() && call.numberOfArguments() == 1))
        return;
    Boxed item = boxedOperand(call.context(), call.argument(0));
    Boxed newLength = callOperation<Boxed>(call.context(), RuntimeOperation::ArrayUnshift, { receiverOf(call), item });
    missUnless(call, isNotEmpty(newLength));
    finish(call, newLength);
}

Integer ArrayBuiltinEmitter::indexOfNumber(const InlineCall& call, CellPointer array)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    Number searched = numberOperand(call.argument(0));
    Word32 shape = elements.indexingMode(array, arrayStorageTestMask);
    LBasicBlock holdsDoubles = newBlock();
    LBasicBlock holdsNoDoubles = newBlock();
    Join<Integer> join(m_out);
    branch(elements.hasShape(shape, arrayStorageFormOf(ArrayStorage::Double)), { normalEdge(holdsDoubles), normalEdge(holdsNoDoubles) });

    m_out.appendTo(holdsDoubles);
    join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfDouble, { array, searched }));

    m_out.appendTo(holdsNoDoubles);
    missUnless(call, elements.hasShape(shape, arrayStorageFormOf(ArrayStorage::Int32)));
    Word32 truncated = low32(emitter<ConversionEmitter>().toInteger(searched));
    join.add(either<Integer>(equal(toNumber(truncated), searched), [&] {
        return callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfInt32, { array, boxInt32(truncated) });
    }, [&] {
        return integer(-1);
    }));
    return join.finish();
}

void ArrayBuiltinEmitter::generateArrayIndexOf(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss() && call.numberOfArguments() == 1 && typeOf(call.argument(0)).isNumber()))
        return;
    finish(call, indexOfNumber(call, receiverOf(call)), Range { -1, INT32_MAX });
}

void ArrayBuiltinEmitter::generateArrayIncludes(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss() && call.numberOfArguments() == 1 && typeOf(call.argument(0)).isNumber()))
        return;
    finish(call, notEqual(indexOfNumber(call, receiverOf(call)), integer(-1)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
