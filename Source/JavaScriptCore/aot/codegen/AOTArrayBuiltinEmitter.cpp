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
#include "AOTVector.h"
#include "ArrayConventions.h"
#include "JSType.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

bool ArrayBuiltinEmitter::checkChosenForm(const InlineCall& call, bool isHandled)
{
    if (isHandled)
        return true;
    fail(call.context(), InternalErrorKind::OpcodeWithoutCode);
    finishInlineCall(call, emitter<ConstantEmitter>().zeroOf(resultRepresentationOf(call)));
    return false;
}

Operand ArrayBuiltinEmitter::resultFromRawElement(const InlineCall& call, const ArrayStorageForm& form, Operand rawElement)
{
    Representation representation = resultRepresentationOf(call);
    if (representation == Representation::Void)
        return { };
    return elements().convertRawElement(form, rawElement, representation, resultTypeOf(call));
}

Operand ArrayBuiltinEmitter::undefinedResult(const InlineCall& call)
{
    Representation representation = resultRepresentationOf(call);
    if (representation == Representation::Void)
        return { };
    return elements().absentElement(call.context(), representation);
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

void ArrayBuiltinEmitter::forEachFormOrMiss(const InlineCall& call, CellPointer array, IndexingType mask, const ScopedLambda<void(const ArrayStorageForm&)>& functor)
{
    ElementAccessEmitter::ShapeTest test = mask == IndexingShapeAndWritabilityMask ? &ElementAccessEmitter::hasWritableShape : &ElementAccessEmitter::hasShape;
    Word32 mode = elements().indexingMode(array, mask);
    elements().forEachFormByShape(mode, test, candidateFormsOf(call), missBlockOf(call), functor);
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
    CellPointer array = receiverOf(call);
    ValueID position = call.argument(0);
    LBasicBlock isAbsent = newBlock();
    OperandJoin join(m_out, resultRepresentationOf(call));
    Integer index = typeOf(position).isInteger() ? integerOperand(position) : elements().indexOrElse(position, false, missBlockOf(call));
    ArrayElements storage = elements().elementsOf(array);
    Integer length = elements().publicLength(storage);
    Condition countsFromEnd = lessThan(index, 0);
    Integer relative = select(countsFromEnd, add(index, length), index);
    branchUnless(below(relative, length), isAbsent);
    forEachFormOrMiss(call, array, arrayStorageTestMask, [&](const ArrayStorageForm& form) {
        Operand raw = elements().loadRawElement(form, storage, relative);
        branchUnless(logicalNot(elements().isHoleIn(call.receiver(), form, raw)), isAbsent);
        join.add(resultFromRawElement(call, form, raw));
    });

    m_out.appendTo(isAbsent);
    join.add(undefinedResult(call));
    finishInlineCall(call, join.finish());
}

void ArrayBuiltinEmitter::generateArrayJoin(const InlineCall& call)
{
    bool hasStringSeparator = call.numberOfArguments() == 1 && typeOf(call.argument(0)).isAnyString();
    if (!checkChosenForm(call, call.canMiss() && (!call.numberOfArguments() || hasStringSeparator)))
        return;
    CellPointer array = receiverOf(call);
    CellPointer null(m_out.intPtrZero);
    Word32 shape = elements().indexingMode(array, arrayStorageTestMask);
    Condition hasForm;
    for (const ArrayStorageForm& form : arrayStorageForms)
        hasForm = logicalOr(hasForm, elements().hasShape(shape, form));
    missUnless(call, hasForm);
    CellPointer separator = call.numberOfArguments() ? cellOperand(call.argument(0)) : null;
    CellPointer joined = callOperation<CellPointer>(call.context(), RuntimeOperation::ArrayJoin, { array, separator });
    missUnless(call, notEqual(joined, null));
    finish(call, joined);
}

Operand ArrayBuiltinEmitter::removeFirstElement(const InlineCall& call, const ArrayStorageForm& form, const ArrayElements& storage, LBasicBlock isAbsent)
{
    Integer last = sub(elements().publicLength(storage), 1);
    Operand first = elements().loadRawElement(form, storage, integer(0));
    branchUnless(logicalNot(elements().isHoleIn(call.receiver(), form, first)), isAbsent);

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
    elements().storeRawElement(form, storage, index, elements().loadRawElement(form, storage, next));
    m_out.addIncomingToPhi(index.value(), m_out.anchor(next.value()));
    m_out.jump(loop);

    m_out.appendTo(moved);
    elements().storeRawElement(form, storage, last, elements().rawHole(form));
    elements().setPublicLength(storage, last);
    if (form.holdsCells)
        emitter<BarrierEmitter>().barrier(call.context(), storage.array);
    return first;
}

void ArrayBuiltinEmitter::removeElementAtEnd(const InlineCall& call, bool removesFirst)
{
    if (!checkChosenForm(call, call.canMiss()))
        return;
    CellPointer array = receiverOf(call);
    LBasicBlock hasNoElements = newBlock();
    OperandJoin join(m_out, resultRepresentationOf(call));
    ArrayElements storage = elements().elementsOf(array);
    Integer length = elements().publicLength(storage);
    Integer last = removesFirst ? Integer() : sub(length, integer(1));
    forEachFormOrMiss(call, array, IndexingShapeAndWritabilityMask, [&](const ArrayStorageForm& form) {
        branchUnless(isNotZero(length), hasNoElements);
        if (removesFirst) {
            missUnless(call, belowOrEqual(length, integer(maxLengthForElementwiseMove)));
            join.add(resultFromRawElement(call, form, removeFirstElement(call, form, storage, missBlockOf(call))));
            return;
        }
        Operand raw = elements().loadRawElement(form, storage, last);
        missUnless(call, logicalNot(elements().isHoleIn(call.receiver(), form, raw)));
        elements().storeRawElement(form, storage, last, elements().rawHole(form));
        elements().setPublicLength(storage, last);
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
    bool hasNumberBounds = call.numberOfArguments() <= 2 && std::ranges::all_of(call.arguments(), [&](ValueID bound) {
        return typeOf(bound).isNumber();
    });
    if (!checkChosenForm(call, call.canMiss() && hasNumberBounds))
        return;
    CellPointer array = receiverOf(call);
    bool withPropertyRoom = resultGetsNamedProperties(call);
    StubCallResult copy;
    if (!call.numberOfArguments())
        copy = callStub(call.context(), arrayCopy(withPropertyRoom), { array.value() });
    else {
        Integer start = wholeNumberOrElse(call.argument(0), missBlockOf(call));
        Integer end = call.numberOfArguments() > 1 ? wholeNumberOrElse(call.argument(1), missBlockOf(call)) : integer(arraySliceToEnd);
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
    Boxed newLength = callOperation<Boxed>(call.context(), RuntimeOperation::ArrayUnshift, { receiverOf(call), item }, { .calledBuiltin = functionOf(call.overload()) });
    missUnless(call, isNotEmpty(newLength));
    finish(call, newLength);
}

void ArrayBuiltinEmitter::scanShortVector(const InlineCall& call, const ArrayStorageForm& form, CellPointer array, Operand searchedRawElement, Join<Integer>& found)
{
    if (call.context().shape() == CodeShape::Compact)
        return;
    LBasicBlock isLong = newBlock();
    ArrayElements storage = elements().elementsOf(array);
    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock isEqual = newBlock();
    LBasicBlock isDifferent = newBlock();
    LBasicBlock isAbsent = newBlock();
    Integer length = elements().publicLength(storage);
    branchUnless(belowOrEqual(length, integer(static_cast<int64_t>(build().limit(LimitID::MaxInlineSearchedElements)))), isLong);
    ValueFromBlock start = m_out.anchor(integer(0).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer index(m_out.phi(B3::Int64, start));
    branch(below(index, length), { normalEdge(element), normalEdge(isAbsent) });

    m_out.appendTo(element);
    Operand raw = elements().loadRawElement(form, storage, index);
    // A hole equals nothing that is searched: it is a NaN among doubles, and the empty word among boxed integers. +0 equals -0 among doubles, as both strict equality and SameValueZero want.
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

    m_out.appendTo(isLong);
}

Integer ArrayBuiltinEmitter::indexOfNumber(const InlineCall& call, CellPointer array)
{
    Number searched = numberOperand(call.argument(0));
    Word32 shape = elements().indexingMode(array, arrayStorageTestMask);
    LBasicBlock holdsDoubles = newBlock();
    LBasicBlock holdsNoDoubles = newBlock();
    Join<Integer> join(m_out);
    branch(elements().hasShape(shape, arrayStorageFormOf(ArrayStorage::Double)), { normalEdge(holdsDoubles), normalEdge(holdsNoDoubles) });

    m_out.appendTo(holdsDoubles);
    scanShortVector(call, arrayStorageFormOf(ArrayStorage::Double), array, searched, join);
    join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfDouble, { array, searched }));

    m_out.appendTo(holdsNoDoubles);
    missUnless(call, elements().hasShape(shape, arrayStorageFormOf(ArrayStorage::Int32)));
    Word32 truncated = low32(emitter<ConversionEmitter>().toInteger(searched));
    join.add(either<Integer>(equal(toNumber(truncated), searched), [&] {
        Boxed boxed = boxInt32(truncated);
        scanShortVector(call, arrayStorageFormOf(ArrayStorage::Int32), array, boxed, join);
        return callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfInt32, { array, boxed });
    }, [&] {
        return integer(-1);
    }));
    return join.finish();
}

// An object, a function, a symbol, a boolean, undefined or null equals what has the same bits and nothing else. Whatever else is searched leaves for the general form.
Integer ArrayBuiltinEmitter::indexOfValueComparedByIdentity(const InlineCall& call, CellPointer array, HoleIs hole)
{
    const ArrayStorageForm& form = arrayStorageFormOf(ArrayStorage::Contiguous);
    uint16_t primitives = typeOf(call.argument(0)).primitives;
    Boxed searched = boxedOperand(call.context(), call.argument(0));
    Join<Integer> join(m_out);
    if (primitives & Primitive::AnyString) {
        LBasicBlock isCellBlock = newBlock();
        LBasicBlock isStringBlock = newBlock();
        LBasicBlock isNotString = newBlock();
        branch(isCell(searched), { normalEdge(isCellBlock), normalEdge(isNotString) });
        m_out.appendTo(isCellBlock);
        branch(hasCellType(unboxCell(searched), StringType), { normalEdge(isStringBlock), normalEdge(isNotString) });
        m_out.appendTo(isStringBlock);
        join.add(indexOfString(call, array, unboxCell(searched)));
        m_out.appendTo(isNotString);
    }
    missUnless(call, elements().hasShape(elements().indexingMode(array, arrayStorageTestMask), form));
    if (primitives & Primitive::Number)
        missUnless(call, logicalNot(isNumber(searched)));
    // A hole is the empty word, which equals nothing that is searched.
    if (hole == HoleIs::Undefined && primitives & Primitive::Undefined)
        missUnless(call, notEqual(searched, undefined()));
    if (primitives & Primitive::BigInt) {
        LBasicBlock isCellBlock = newBlock();
        LBasicBlock isComparedByBits = newBlock();
        branch(isCell(searched), { normalEdge(isCellBlock), normalEdge(isComparedByBits) });
        m_out.appendTo(isCellBlock);
        missUnless(call, hasCellTypeAbove(unboxCell(searched), static_cast<JSType>(LastValueCompareCellType)));
        m_out.jump(isComparedByBits);
        m_out.appendTo(isComparedByBits);
    }

    scanShortVector(call, form, array, searched, join);
    join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfInt32, { array, searched }));
    return join.finish();
}

Integer ArrayBuiltinEmitter::indexOfString(const InlineCall& call, CellPointer array, CellPointer searched)
{
    Type contents = typeOfContentsRead({ m_program, m_function, types(), TypeLevel::Proven }, typeOf(call.receiver()), ContentsSlot::Element);
    bool elementsAreStrings = contents.isAnyString() && elements().isDense(call.receiver()) && !generator().validatesOrChecksProofs();
    StubKey stub = arrayIndexOfString(elementsAreStrings ? SearchedElements::Strings : SearchedElements::Values);
    return Integer(callStub(call.context(), stub, { array.value(), searched.value() }).first);
}

Integer ArrayBuiltinEmitter::indexOfSearched(const InlineCall& call, HoleIs hole)
{
    if (typeOf(call.argument(0)).isNumber())
        return indexOfNumber(call, receiverOf(call));
    if (typeOf(call.argument(0)).isAnyString()) {
        CellPointer array = receiverOf(call);
        return indexOfString(call, array, cellOperand(call.argument(0)));
    }
    return indexOfValueComparedByIdentity(call, receiverOf(call), hole);
}

void ArrayBuiltinEmitter::generateArrayIndexOf(const InlineCall& call)
{
    if (!checkChosenForm(call, call.numberOfArguments() == 1 && (call.canMiss() || typeOf(call.argument(0)).isAnyString())))
        return;
    bool maySearchForString = typeOf(call.argument(0)).primitives & Primitive::AnyString;
    finish(call, indexOfSearched(call, HoleIs::Nothing), Range { -1, maySearchForString ? static_cast<int64_t>(MAX_ARRAY_INDEX) : INT32_MAX });
}

void ArrayBuiltinEmitter::generateArrayIncludes(const InlineCall& call)
{
    if (!checkChosenForm(call, call.numberOfArguments() == 1 && (call.canMiss() || typeOf(call.argument(0)).isAnyString())))
        return;
    finish(call, notEqual(indexOfSearched(call, HoleIs::Undefined), -1));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
