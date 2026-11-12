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
#include "AOTStubsCollections.h"
#include "AOTStubsElements.h"
#include "AOTTypeTestEmitter.h"
#include "ArrayConventions.h"
#include "JSArray.h"
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
    return elements().convertRawElement(call.context(), form, rawElement, representation, resultTypeIDOf(call));
}

Operand ArrayBuiltinEmitter::undefinedResult(const InlineCall& call)
{
    Representation representation = resultRepresentationOf(call);
    if (representation == Representation::Void)
        return { };
    return elements().absentElement(call.context(), representation);
}

TypeID ArrayBuiltinEmitter::elementTypeOf(const InlineCall& call, TypeLevel level)
{
    const PositionTypes& positions = PositionTypes::knowingNothing();
    TypingContext context { m_program, types(), positions, level };
    return typeOfContentsRead(context, typeIDOf(call.receiver()), ContentsSlot::Element);
}

ArrayStorageFormList ArrayBuiltinEmitter::candidateFormsOf(const InlineCall& call)
{
    TypeID contents = elementTypeOf(call, TypeLevel::Expected);
    const ArrayStorageForm& expected = elements().expectedFormOf(call.receiver());
    bool expectsContents = !isNeverType(contents);
    ArrayStorageFormList forms { &expected };
    for (const ArrayStorageForm& form : arrayStorageForms) {
        if (form.kind == expected.kind)
            continue;
        if (!expectsContents || types().overlap(widestTypeOf(form.element), contents))
            forms.append(&form);
    }
    return forms;
}

ElementAccessEmitter::FirstForm ArrayBuiltinEmitter::firstCandidateFormOf(const InlineCall& call)
{
    return types().at(elementTypeOf(call, TypeLevel::Expected)).isAnything() ? ElementAccessEmitter::FirstForm::IsLikeTheOthers : ElementAccessEmitter::FirstForm::IsExpected;
}

void ArrayBuiltinEmitter::forEachFormOrMiss(const InlineCall& call, CellPointer array, IndexingType mask, const ScopedLambda<void(const ArrayStorageForm&)>& functor)
{
    ElementAccessEmitter::ShapeTest test = mask == IndexingShapeAndWritabilityMask ? &ElementAccessEmitter::hasWritableShape : &ElementAccessEmitter::hasShape;
    Word32 mode = elements().indexingMode(array, mask);
    elements().forEachFormByShape(mode, test, candidateFormsOf(call), missBlockOf(call), functor, firstCandidateFormOf(call));
}

void ArrayBuiltinEmitter::forEachReadForm(const InlineCall& call, CellPointer array, IndexingType mask, const ScopedLambda<void(const ArrayStorageForm&)>& functor)
{
    ArrayStorageFormList others = candidateFormsOf(call);
    const ArrayStorageForm& values = arrayStorageFormOf(ArrayStorage::Contiguous);
    const ArrayStorageForm& integers = arrayStorageFormOf(ArrayStorage::Int32);
    Representation representation = resultRepresentationOf(call);
    bool isBoxed = representation == Representation::JSValue || representation == Representation::Void;
    if (!isBoxed || others.first() != &values || !others.contains(&integers)) {
        forEachFormOrMiss(call, array, mask, functor);
        return;
    }
    others.removeFirst(&values);
    others.removeFirst(&integers);
    ElementAccessEmitter::ShapeTest test = mask == IndexingShapeAndWritabilityMask ? &ElementAccessEmitter::hasWritableShape : &ElementAccessEmitter::hasShape;
    Word32 mode = elements().indexingMode(array, mask);
    LBasicBlock holdsValues = newBlock();
    LBasicBlock holdsNoValues = newBlock();
    LBasicBlock holdsOthers = newBlock();
    bool othersAreRare = others.isEmpty() || firstCandidateFormOf(call) == ElementAccessEmitter::FirstForm::IsExpected;
    m_out.branch((elements().*test)(mode, values).value(), normalEdge(holdsValues), normalEdge(holdsNoValues));
    m_out.appendTo(holdsNoValues);
    m_out.branch((elements().*test)(mode, integers).value(), normalEdge(holdsValues), othersAreRare ? rareEdge(holdsOthers) : normalEdge(holdsOthers));
    m_out.appendTo(holdsValues);
    functor(values);
    m_out.appendTo(holdsOthers);
    elements().forEachFormByShape(mode, test, others, missBlockOf(call), functor);
}

void ArrayBuiltinEmitter::generateArrayIterator(const InlineCall& call)
{
    LValue kind = m_out.constInt64(static_cast<int64_t>(call.parameterWord(0)));
    finish(call, CellPointer(callStub(call.context(), newIterator(IteratedClass::Array), { receiverOf(call).value(), kind }).first));
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
    forEachReadForm(call, array, arrayStorageTestMask, [&](const ArrayStorageForm& form) {
        Operand raw = elements().loadRawElement(form, storage, relative);
        branchUnless(logicalNot(elements().isHole(form, raw)), isAbsent);
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
    static_assert(arrayStorageForms.size() == 3 && Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape && !(arrayStorageTestMask & 1));
    Word32 shape = elements().indexingMode(array, arrayStorageTestMask);
    missUnless(call, belowOrEqual(sub(shape, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)));
    CellPointer separator = call.numberOfArguments() ? cellOperand(call.argument(0)) : null;
    CellPointer joined = callOperation<CellPointer>(call.context(), RuntimeOperation::ArrayJoin, { array, separator });
    missUnless(call, notEqual(joined, null));
    finish(call, joined);
}

Operand ArrayBuiltinEmitter::removeFirstElement(const InlineCall& call, const ArrayStorageForm& form, const ArrayElements& storage, LBasicBlock isAbsent)
{
    Operand first = elements().loadRawElement(form, storage, integer(0));
    branchUnless(logicalNot(elements().isHole(form, first)), isAbsent);
    callStub(call.context(), shiftFewElements(form.kind == ArrayStorage::Double), { storage.array.value() });
    if (form.holdsCells)
        emitter<BarrierEmitter>().barrier(call.context(), storage.array);
    return first;
}

Operand ArrayBuiltinEmitter::shiftLongArray(const InlineCall& call, const ArrayStorageForm& form, const ArrayElements& storage, Integer length)
{
    Operand first = elements().loadRawElement(form, storage, integer(0));
    missUnless(call, logicalNot(elements().isHole(form, first)));
    callStub(call.context(), shiftArrayStart(), { storage.array.value() });
    return first;
}

void ArrayBuiltinEmitter::removeElementAtEnd(const InlineCall& call, bool removesFirst)
{
    if (!checkChosenForm(call, call.canMiss()))
        return;
    CellPointer array = receiverOf(call);
    bool mayHaveNoElements = resultTypeOf(call).primitives & Primitive::Undefined;
    LBasicBlock hasNoElements = mayHaveNoElements ? newBlock() : nullptr;
    OperandJoin join(m_out, resultRepresentationOf(call));
    ArrayElements storage = elements().elementsOf(array);
    Integer length = elements().publicLength(storage);
    Integer last = removesFirst ? Integer() : sub(length, integer(1));
    forEachReadForm(call, array, IndexingShapeAndWritabilityMask, [&](const ArrayStorageForm& form) {
        if (mayHaveNoElements)
            branchUnless(isNotZero(length), hasNoElements);
        if (removesFirst) {
            Condition areFew = belowOrEqual(length, integer(maxLengthForElementwiseMove));
            if (!generator().paysForCodeInLine(call.context().block()))
                missUnless(call, areFew);
            else {
                LBasicBlock few = newBlock();
                LBasicBlock many = newBlock();
                branch(areFew, { normalEdge(few), normalEdge(many) });
                m_out.appendTo(many);
                join.add(resultFromRawElement(call, form, shiftLongArray(call, form, storage, length)));
                m_out.appendTo(few);
            }
            join.add(resultFromRawElement(call, form, removeFirstElement(call, form, storage, missBlockOf(call))));
            return;
        }
        Operand raw = elements().loadRawElement(form, storage, last);
        missUnless(call, logicalNot(elements().isHole(form, raw)));
        elements().storeRawElement(form, storage, last, elements().rawHole(form));
        elements().setPublicLength(storage, last);
        join.add(resultFromRawElement(call, form, raw));
    });

    if (mayHaveNoElements) {
        m_out.appendTo(hasNoElements);
        join.add(undefinedResult(call));
    }
    finishInlineCall(call, join.finish());
}

bool ArrayBuiltinEmitter::resultGetsNamedProperties(const InlineCall& call)
{
    const Code& code = generator().code();
    std::span<const ValueID> continuationParameters = call.terminator() ? code.at(code.at((*call.terminator())->done()).to).parameters() : std::span<const ValueID>();
    ValueID made = call.value() ? call.value()->id() : continuationParameters.empty() ? ValueID() : continuationParameters.front();
    return made && generator().isStoredIntoByName(made);
}

bool ArrayBuiltinEmitter::resultIsExpectedToHoldDoubles(const InlineCall& call)
{
    const Type& result = resultTypeOf(call);
    if (!result.hasObjects())
        return false;
    return arrayStorageDeclaredBy(m_program, types(), types().intern(result)).predicted() == ArrayStorage::Double;
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
    bool makesDoubles = !withPropertyRoom && resultIsExpectedToHoldDoubles(call);
    Integer start = call.numberOfArguments() ? wholeNumberOrElse(call.argument(0), missBlockOf(call)) : Integer();
    Integer end = call.numberOfArguments() > 1 ? wholeNumberOrElse(call.argument(1), missBlockOf(call)) : Integer();
    StubCallResult copy;
    if (!start)
        copy = callStub(call.context(), makesDoubles ? arrayCopyAsDoubles() : arrayCopy(withPropertyRoom), { array.value() });
    else
        copy = callStub(call.context(), makesDoubles ? arraySliceAsDoubles() : arraySlice(withPropertyRoom), { array.value(), start.value(), (end ? end : integer(arraySliceToEnd)).value() });
    missUnless(call, isNotEmpty(Boxed(copy.first)));
    finish(call, CellPointer(copy.first));
}

Boxed ArrayBuiltinEmitter::copyWithPropertyRoomInFrontOfClosedCall(const OperationContext& context, ValueID list, LBasicBlock closed)
{
    Boxed receiver = in<Boxed>(list);
    branchUnless(isCell(receiver), closed);
    CellPointer array = unboxCell(receiver);
    branchUnless(hasCellType(array, ArrayType), closed);
    branchUnless(emitter<TypeTestEmitter>().isPristine(array, BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names }), closed);
    Boxed copy(callStub(context, arrayCopy(true), { array.value() }).first);
    branchUnless(isNotEmpty(copy), closed);
    return copy;
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

void ArrayBuiltinEmitter::scanShortVector(const InlineCall& call, const ArrayStorageForm& form, CellPointer array, Operand searchedRawElement, std::optional<Integer> first, Join<Integer>& found)
{
    LBasicBlock isShort = newBlock();
    LBasicBlock isLong = newBlock();
    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock isEqual = newBlock();
    LBasicBlock isDifferent = newBlock();
    LBasicBlock isAbsent = newBlock();
    ArrayElements storage = elements().elementsOf(array);
    Integer length = elements().publicLength(storage);
    branch(belowOrEqual(length, integer(maxInlineSearchedElements)), { normalEdge(isShort), normalEdge(isLong) });

    m_out.appendTo(isShort);
    ValueFromBlock start = m_out.anchor(first.value_or(integer(0)).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer index(m_out.phi(Machine::Int64, start));
    branch(below(index, length), { normalEdge(element), normalEdge(isAbsent) });

    m_out.appendTo(element);
    Operand raw = elements().loadRawElement(form, storage, index);
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
    if (first) {
        m_out.jump(missBlockOf(call));
        m_out.appendTo(newBlock());
    }
}

std::optional<Integer> ArrayBuiltinEmitter::firstIndexOfSearch(const InlineCall& call)
{
    if (call.numberOfArguments() < 2)
        return std::nullopt;
    Integer first = wholeNumberOrElse(call.argument(1), missBlockOf(call));
    missUnless(call, below(first, integer(1ll << 32)));
    return first;
}

Integer ArrayBuiltinEmitter::indexOfNumber(const InlineCall& call, CellPointer array)
{
    if (representationOf(call.argument(0)) == Representation::Int32)
        return indexOfInt32(call, array);
    Number searched = numberOperand(call.argument(0));
    std::optional<Integer> first = firstIndexOfSearch(call);
    Join<Integer> join(m_out);
    auto searchDoubles = [&] {
        scanShortVector(call, arrayStorageFormOf(ArrayStorage::Double), array, searched, first, join);
        join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfDouble, { array, searched }));
    };
    auto searchIntegers = [&] {
        LBasicBlock isInt32 = newBlock();
        LBasicBlock isNoInt32 = newBlock();
        Word32 truncated = emitter<ConversionEmitter>().truncateToWord(searched);
        branch(equal(toNumber(truncated), searched), { normalEdge(isInt32), normalEdge(isNoInt32) });

        m_out.appendTo(isInt32);
        Boxed boxed = boxInt32(truncated);
        scanShortVector(call, arrayStorageFormOf(ArrayStorage::Int32), array, boxed, first, join);
        join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfInt32, { array, boxed }));

        m_out.appendTo(isNoInt32);
        join.add(integer(-1));
    };
    Word32 shape = elements().indexingMode(array, arrayStorageTestMask);
    LBasicBlock holdsDoubles = newBlock();
    LBasicBlock holdsNoDoubles = newBlock();
    branch(elements().hasShape(shape, arrayStorageFormOf(ArrayStorage::Double)), { normalEdge(holdsDoubles), normalEdge(holdsNoDoubles) });

    m_out.appendTo(holdsDoubles);
    searchDoubles();

    m_out.appendTo(holdsNoDoubles);
    missUnless(call, elements().hasShape(shape, arrayStorageFormOf(ArrayStorage::Int32)));
    searchIntegers();
    return join.finish();
}

Integer ArrayBuiltinEmitter::indexOfInt32(const InlineCall& call, CellPointer array)
{
    Boxed searched = boxedOperand(call.context(), call.argument(0));
    return indexOfBoxedInt32(call, array, searched, firstIndexOfSearch(call));
}

Integer ArrayBuiltinEmitter::indexOfOptionalInt32(const InlineCall& call, CellPointer array)
{
    Boxed searched = boxedOperand(call.context(), call.argument(0));
    std::optional<Integer> first = firstIndexOfSearch(call);
    missUnless(call, isInt32(searched));
    return indexOfBoxedInt32(call, array, searched, first);
}

Integer ArrayBuiltinEmitter::indexOfBoxedInt32(const InlineCall& call, CellPointer array, Boxed searched, std::optional<Integer> first)
{
    Join<Integer> join(m_out);
    Word32 shape = elements().indexingMode(array, arrayStorageTestMask);
    LBasicBlock holdsInt32s = newBlock();
    LBasicBlock holdsNoInt32s = newBlock();
    branch(elements().hasShape(shape, arrayStorageFormOf(ArrayStorage::Int32)), { normalEdge(holdsInt32s), normalEdge(holdsNoInt32s) });

    m_out.appendTo(holdsInt32s);
    scanShortVector(call, arrayStorageFormOf(ArrayStorage::Int32), array, searched, first, join);
    join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfInt32, { array, searched }));

    m_out.appendTo(holdsNoInt32s);
    missUnless(call, elements().hasShape(shape, arrayStorageFormOf(ArrayStorage::Double)));
    Number number = toNumber(unboxInt32(searched));
    scanShortVector(call, arrayStorageFormOf(ArrayStorage::Double), array, number, first, join);
    join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfDouble, { array, number }));
    return join.finish();
}

Integer ArrayBuiltinEmitter::indexOfValueComparedByIdentity(const InlineCall& call, CellPointer array, HoleIs hole)
{
    const ArrayStorageForm& form = arrayStorageFormOf(ArrayStorage::Contiguous);
    uint16_t primitives = typeOf(call.argument(0)).primitives;
    Boxed searched = boxedOperand(call.context(), call.argument(0));
    std::optional<Integer> first = firstIndexOfSearch(call);
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

    scanShortVector(call, form, array, searched, first, join);
    join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfInt32, { array, searched }));
    return join.finish();
}

Integer ArrayBuiltinEmitter::indexOfString(const InlineCall& call, CellPointer array, CellPointer searched)
{
    if (call.numberOfArguments() == 1) {
        StubKey stub = arrayIndexOfString(SearchedElements::Values);
        return Integer(callStub(call.context(), stub, { array.value(), searched.value() }).first);
    }
    Integer start = wholeNumberOrElse(call.argument(1), missBlockOf(call));
    missUnless(call, below(start, integer(1ll << 32)));
    StubKey stub = arrayIndexOfString(SearchedElements::ValuesFromStart);
    return Integer(callStub(call.context(), stub, { array.value(), searched.value(), start.value() }).first);
}

Integer ArrayBuiltinEmitter::indexOfSearched(const InlineCall& call, HoleIs hole)
{
    if (typeOf(call.argument(0)).isNumber())
        return indexOfNumber(call, receiverOf(call));
    if (typeOf(call.argument(0)).isAnyString()) {
        CellPointer array = receiverOf(call);
        return indexOfString(call, array, cellOperand(call.argument(0)));
    }
    if (typeOf(call.argument(0)).isInt32OrNullish() && typeOf(call.argument(0)).primitives & Primitive::Integer)
        return indexOfOptionalInt32(call, receiverOf(call));
    return indexOfValueComparedByIdentity(call, receiverOf(call), hole);
}

bool ArrayBuiltinEmitter::handlesSearch(const InlineCall& call)
{
    if (call.numberOfArguments() == 2)
        return call.canMiss() && typeOf(call.argument(1)).isNumber();
    return call.numberOfArguments() == 1 && (call.canMiss() || typeOf(call.argument(0)).isAnyString());
}

void ArrayBuiltinEmitter::generateArrayIndexOf(const InlineCall& call)
{
    if (!checkChosenForm(call, handlesSearch(call)))
        return;
    bool maySearchForString = typeOf(call.argument(0)).primitives & Primitive::AnyString;
    finish(call, indexOfSearched(call, HoleIs::Nothing), Range { -1, maySearchForString ? static_cast<int64_t>(MAX_ARRAY_INDEX) : INT32_MAX });
}

void ArrayBuiltinEmitter::generateArrayIncludes(const InlineCall& call)
{
    if (!checkChosenForm(call, handlesSearch(call)))
        return;
    finish(call, notEqual(indexOfSearched(call, HoleIs::Undefined), -1));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
