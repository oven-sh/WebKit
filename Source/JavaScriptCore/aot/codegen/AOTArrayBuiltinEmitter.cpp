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
#include "AOTEdgeResults.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTFunctionAnalyses.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include "AOTStubsArrays.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
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
    Type contents = typeOfContentsRead({ m_program, m_function, types(), TypeLevel::Expected }, array, ContentsSlot::Element);
    const ArrayStorageForm& expected = ArrayAllocationEmitter(generator()).formAtAllocation(array);
    // new Array(n) declares an array that holds nothing, which says nothing about what is stored later.
    bool expectsContents = !contents.isNever();
    ArrayStorageFormList forms { &expected };
    for (const ArrayStorageForm& form : arrayStorageForms) {
        if (form.kind == expected.kind)
            continue;
        if (!expectsContents || widestTypeOf(form.element).overlaps(contents, m_program))
            forms.append(&form);
    }
    return forms;
}

void ArrayBuiltinEmitter::forEachReadableFormOrMiss(const InlineCall& call, CellPointer array, const ScopedLambda<void(const ArrayStorageForm&)>& functor)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    elements.forEachFormByReadableShape(elements.indexingMode(array, arrayStorageTestMask), candidateFormsOf(call), missBlockOf(call), functor);
}

void ArrayBuiltinEmitter::forEachWritableFormOrMiss(const InlineCall& call, CellPointer array, const ScopedLambda<void(const ArrayStorageForm&)>& functor)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    elements.forEachFormByWritableShape(elements.indexingMode(array, IndexingShapeAndWritabilityMask), candidateFormsOf(call), missBlockOf(call), functor);
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
    forEachReadableFormOrMiss(call, array, [&](const ArrayStorageForm& form) {
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
    forEachWritableFormOrMiss(call, array, [&](const ArrayStorageForm& form) {
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
    forEachWritableFormOrMiss(call, array, [&](const ArrayStorageForm& form) {
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

// Whether this function goes on to store a named property into the array that the call makes.
bool ArrayBuiltinEmitter::resultGetsNamedProperties(const InlineCall& call)
{
    constexpr unsigned maxValuesFollowed = 16;
    const UseLists& useLists = analyses().useLists();
    Vector<ValueID, maxValuesFollowed> aliases;
    auto follow = [&](ValueID alias) {
        if (alias && aliases.size() < maxValuesFollowed && !aliases.contains(alias))
            aliases.append(alias);
    };
    if (call.value())
        follow(call.value()->id());
    else if (BlockID from = call.terminator()->block(); numberOfEdgeResults(m_function, from, 0))
        follow(m_function.at((*call.terminator())->done().block).parameters[firstEdgeResultParameter(m_function, from, 0)]);
    for (unsigned index = 0; index < aliases.size(); ++index) {
        for (const Use& use : useLists.usesOf(aliases[index])) {
            switch (use.kind) {
            case Use::Kind::Operand: {
                const Value& user = m_function.at(use.user);
                if (user.opcode == Opcode::PropSet && use.operandIndex == OperandIndex::PropSet::object)
                    return true;
                if (isCast(user.opcode))
                    follow(use.user);
                break;
            }
            case Use::Kind::EdgeArgument: {
                auto& parameters = m_function.at(targetOf(m_function, use.edge()).block).parameters;
                if (use.operandIndex < parameters.size())
                    follow(parameters[use.operandIndex]);
                break;
            }
            case Use::Kind::TerminatorOperand:
            case Use::Kind::HandlerArgument:
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
    return false;
}

void ArrayBuiltinEmitter::generateArraySlice(const InlineCall& call)
{
    bool hasIntegerBounds = call.numberOfArguments() <= 2 && std::ranges::all_of(call.arguments(), [&](ValueID bound) {
        return typeOf(bound).isInteger();
    });
    if (!checkChosenForm(call, call.canMiss() && hasIntegerBounds))
        return;
    CellPointer array = receiverOf(call);
    bool withPropertyRoom = resultGetsNamedProperties(call);
    StubCallResult copy;
    if (!call.numberOfArguments())
        copy = callStub(call.context(), arrayCopy(withPropertyRoom), { array.value() });
    else {
        Integer start = integerOperand(call.argument(0));
        Integer end = call.numberOfArguments() > 1 ? integerOperand(call.argument(1)) : integer(arraySliceToEnd);
        copy = callStub(call.context(), arraySlice(withPropertyRoom), { array.value(), start.value(), end.value() });
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

void ArrayBuiltinEmitter::scanShortVector(const ArrayStorageForm& form, const ArrayElements& storage, Operand searchedRawElement, Join<Integer>& found, LBasicBlock isLong)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock isEqual = newBlock();
    LBasicBlock isDifferent = newBlock();
    LBasicBlock isAbsent = newBlock();
    Integer length = elements.publicLength(storage);
    branchUnless(belowOrEqual(length, integer(static_cast<int64_t>(build().limit(LimitID::MaxInlineSearchedElements)))), isLong);
    ValueFromBlock start = m_out.anchor(integer(0).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer index(m_out.phi(B3::Int64, start));
    branch(below(index, length), { normalEdge(element), normalEdge(isAbsent) });

    m_out.appendTo(element);
    Operand raw = elements.loadRawElement(form, storage, index);
    // A hole equals nothing that is searched: it is a NaN among doubles, and the empty word among boxed integers.
    // +0 equals -0 among doubles, as both strict equality and SameValueZero want.
    Condition isFound = form.element == Representation::Double
        ? equal(raw.as<Number>(), searchedRawElement.as<Number>())
        : equal(raw.as<Boxed>(), searchedRawElement.as<Boxed>());
    branch(isFound, { normalEdge(isEqual), normalEdge(isDifferent) });

    m_out.appendTo(isDifferent);
    m_out.addIncomingToPhi(index.value(), m_out.anchor(add(index, integer(1)).value()));
    m_out.jump(loop);

    m_out.appendTo(isEqual);
    found.add(index);

    m_out.appendTo(isAbsent);
    found.add(integer(-1));
}

Integer ArrayBuiltinEmitter::indexOfNumber(const InlineCall& call, CellPointer array)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    Number searched = numberOperand(call.argument(0));
    bool scansShortVectors = call.context().shape() != CodeShape::Compact;
    Word32 shape = elements.indexingMode(array, arrayStorageTestMask);
    LBasicBlock holdsDoubles = newBlock();
    LBasicBlock holdsNoDoubles = newBlock();
    Join<Integer> join(m_out);
    branch(elements.hasShape(shape, arrayStorageFormOf(ArrayStorage::Double)), { normalEdge(holdsDoubles), normalEdge(holdsNoDoubles) });

    m_out.appendTo(holdsDoubles);
    if (scansShortVectors) {
        LBasicBlock isLong = newBlock();
        scanShortVector(arrayStorageFormOf(ArrayStorage::Double), elements.elementsOf(array), searched, join, isLong);
        m_out.appendTo(isLong);
    }
    join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfDouble, { array, searched }));

    m_out.appendTo(holdsNoDoubles);
    missUnless(call, elements.hasShape(shape, arrayStorageFormOf(ArrayStorage::Int32)));
    Word32 truncated = low32(emitter<ConversionEmitter>().toInteger(searched));
    join.add(either<Integer>(equal(toNumber(truncated), searched), [&] {
        Boxed boxed = boxInt32(truncated);
        if (scansShortVectors) {
            LBasicBlock isLong = newBlock();
            scanShortVector(arrayStorageFormOf(ArrayStorage::Int32), elements.elementsOf(array), boxed, join, isLong);
            m_out.appendTo(isLong);
        }
        return callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfInt32, { array, boxed });
    }, [&] {
        return integer(-1);
    }));
    return join.finish();
}

// An object, a function, a symbol, a boolean, undefined or null equals what has the same bits and nothing else. Whatever else is searched leaves for the general form.
Integer ArrayBuiltinEmitter::indexOfValueComparedByIdentity(const InlineCall& call, CellPointer array, HoleIs hole)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    const ArrayStorageForm& form = arrayStorageFormOf(ArrayStorage::Contiguous);
    uint16_t primitives = typeOf(call.argument(0)).primitives;
    Boxed searched = boxedOperand(call.context(), call.argument(0));
    missUnless(call, elements.hasShape(elements.indexingMode(array, arrayStorageTestMask), form));
    if (primitives & Primitive::Number)
        missUnless(call, logicalNot(isNumber(searched)));
    // A hole is the empty word, which equals nothing that is searched.
    if (hole == HoleIs::Undefined && primitives & Primitive::Undefined)
        missUnless(call, notEqual(searched, undefined()));
    if (primitives & (Primitive::AnyString | Primitive::BigInt)) {
        LBasicBlock isCellBlock = newBlock();
        LBasicBlock isComparedByBits = newBlock();
        branch(isCell(searched), { normalEdge(isCellBlock), normalEdge(isComparedByBits) });
        m_out.appendTo(isCellBlock);
        missUnless(call, above(loadCellType(unboxCell(searched)), Word32(m_out.constInt32(LastValueCompareCellType))));
        m_out.jump(isComparedByBits);
        m_out.appendTo(isComparedByBits);
    }

    Join<Integer> join(m_out);
    if (call.context().shape() != CodeShape::Compact) {
        LBasicBlock isLong = newBlock();
        scanShortVector(form, elements.elementsOf(array), searched, join, isLong);
        m_out.appendTo(isLong);
    }
    join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfInt32, { array, searched }));
    return join.finish();
}

Integer ArrayBuiltinEmitter::indexOfSearched(const InlineCall& call, HoleIs hole)
{
    if (typeOf(call.argument(0)).isNumber())
        return indexOfNumber(call, receiverOf(call));
    return indexOfValueComparedByIdentity(call, receiverOf(call), hole);
}

void ArrayBuiltinEmitter::generateArrayIndexOf(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss() && call.numberOfArguments() == 1))
        return;
    finish(call, indexOfSearched(call, HoleIs::Nothing), Range { -1, INT32_MAX });
}

void ArrayBuiltinEmitter::generateArrayIncludes(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss() && call.numberOfArguments() == 1))
        return;
    finish(call, notEqual(indexOfSearched(call, HoleIs::Undefined), integer(-1)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
