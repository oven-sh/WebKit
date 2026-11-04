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
#include "AOTTypeTestEmitter.h"
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

ArrayStorage ArrayBuiltinEmitter::storageNamedByFactsFor(const Type& array)
{
    if (!array.hasNamedObjects())
        return ArrayStorage::Other;

    ArrayStorage common = ArrayStorage::Other;
    bool agree = types().at(array.objects).allOf([&](ObjectID object) {
        ArrayStorage named = m_facts.object(object).arrayStorage;
        if (common == ArrayStorage::Other)
            common = named;
        return hasArrayStorageForm(named) && named == common;
    });
    return agree ? common : ArrayStorage::Other;
}

const ArrayStorageForm* ArrayBuiltinEmitter::provenFormOf(ValueID array)
{
    if (!typeOf(array).isObject() || !m_facts.everyObjectHas(types(), typeIDOf(array), ObjectFact::HasDecidedStorage))
        return nullptr;
    return findArrayStorageForm(storageNamedByFactsFor(typeOf(array)));
}

const ArrayStorageForm* ArrayBuiltinEmitter::provenFormOf(const InlineCall& call)
{
    if (const ArrayStorageForm* stamped = findArrayStorageForm(call.storageOfReceiver()))
        return stamped;
    return provenFormOf(call.receiver());
}

const ArrayStorageForm* ArrayBuiltinEmitter::provenFormOfArgument(const InlineCall& call, ValueID list)
{
    for (unsigned index = 0; index < call.numberOfArguments(); ++index) {
        if (call.argument(index) != list)
            continue;
        if (const ArrayStorageForm* stamped = findArrayStorageForm(call.storageOfArgument(index)))
            return stamped;
    }
    return provenFormOf(list);
}

TypeID ArrayBuiltinEmitter::elementTypeOf(const InlineCall& call, TypeLevel level)
{
    const PositionTypes& positions = PositionTypes::knowingNothing();
    TypingContext context { m_program, types(), positions, level };
    return typeOfContentsRead(context, typeIDOf(call.receiver()), ContentsSlot::Element);
}

// What is expected says which kind is tested for first and which kinds get an arm. Every arm stands behind the test for its kind, and an array of a kind without an arm misses.
ArrayStorageFormList ArrayBuiltinEmitter::candidateFormsOf(const InlineCall& call)
{
    if (const ArrayStorageForm* proven = provenFormOf(call))
        return { proven };
    TypeID contents = elementTypeOf(call, TypeLevel::Expected);
    const ArrayStorageForm* named = findArrayStorageForm(storageNamedByFactsFor(typeOf(call.receiver())));
    const ArrayStorageForm& expected = named ? *named : ArrayAllocationEmitter(generator()).formAtAllocation(typeOf(call.receiver()));
    // new Array(n) declares an array that holds nothing, which says nothing about what is stored later.
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

// A list that may hold anything is as likely to have one kind of storage as another, and a loop in rare code pays for every turn.
ElementAccessEmitter::FirstForm ArrayBuiltinEmitter::firstCandidateFormOf(const InlineCall& call)
{
    if (hasArrayStorageForm(storageNamedByFactsFor(typeOf(call.receiver()))))
        return ElementAccessEmitter::FirstForm::IsExpected;
    return types().at(elementTypeOf(call, TypeLevel::Expected)).isAnything() ? ElementAccessEmitter::FirstForm::IsLikeTheOthers : ElementAccessEmitter::FirstForm::IsExpected;
}

void ArrayBuiltinEmitter::forEachFormOrMiss(const InlineCall& call, CellPointer array, IndexingType mask, const ScopedLambda<void(const ArrayStorageForm&)>& functor)
{
    // The storage kind does not tell whether the storage is shared with a literal.
    if (const ArrayStorageForm* proven = provenFormOf(call); proven && mask != IndexingShapeAndWritabilityMask)
        return functor(*proven);
    ElementAccessEmitter::ShapeTest test = mask == IndexingShapeAndWritabilityMask ? &ElementAccessEmitter::hasWritableShape : &ElementAccessEmitter::hasShape;
    Word32 mode = elements().indexingMode(array, mask);
    elements().forEachFormByShape(mode, test, candidateFormsOf(call), missBlockOf(call), functor, firstCandidateFormOf(call));
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

// The operation answers null, and has run nothing, where the text of an element may come from code of the program.
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
    Integer index(m_out.phi(Machine::Int64, start));
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
    // The result rule leaves undefined out only where the call names a length of at least 1. The result may then have a form that holds no undefined.
    bool mayHaveNoElements = resultTypeOf(call).primitives & Primitive::Undefined;
    LBasicBlock hasNoElements = mayHaveNoElements ? newBlock() : nullptr;
    OperandJoin join(m_out, resultRepresentationOf(call));
    ArrayElements storage = elements().elementsOf(array);
    Integer length = elements().publicLength(storage);
    Integer last = removesFirst ? Integer() : sub(length, integer(1));
    forEachFormOrMiss(call, array, IndexingShapeAndWritabilityMask, [&](const ArrayStorageForm& form) {
        if (mayHaveNoElements)
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

    if (mayHaveNoElements) {
        m_out.appendTo(hasNoElements);
        join.add(undefinedResult(call));
    }
    finishInlineCall(call, join.finish());
}

// Somewhere the program stores under a name into an array that the call makes.
bool ArrayBuiltinEmitter::resultGetsNamedProperties(const InlineCall& call)
{
    const Code& code = generator().code();
    std::span<const ValueID> handedOver = call.terminator() ? code.at(code.at((*call.terminator())->done()).to).parameters() : std::span<const ValueID>();
    if (ValueID made = call.value() ? call.value()->id() : handedOver.empty() ? ValueID() : handedOver.front(); made && generator().isStoredIntoByName(made))
        return true;
    const Type& result = resultTypeOf(call);
    if (!result.hasNamedObjects())
        return false;
    return !types().at(result.objects).allOf([&](ObjectID object) {
        AllocationClassID allocationClass = m_facts.object(object).allocationClass;
        return !allocationClass || m_facts.addedKeysOf(allocationClass).empty();
    });
}

// The readers and the stores of the arrays test for the kind that the written element type gives them, or that Facts names for them.
bool ArrayBuiltinEmitter::resultIsExpectedToHoldDoubles(const InlineCall& call)
{
    const Type& result = resultTypeOf(call);
    if (!result.hasObjects())
        return false;
    TypeID resultID = types().intern(result);
    if (storageNamedByFactsFor(result) == ArrayStorage::Double)
        return true;
    // A kind that Facts has decided is read without a test.
    return !m_facts.someObjectHas(types(), resultID, ObjectFact::HasDecidedStorage) && arrayStorageDeclaredBy(m_program, types(), resultID).predicted() == ArrayStorage::Double;
}

bool ArrayBuiltinEmitter::mayMakeArrayOf(const InlineCall& call, ArrayStorage made)
{
    const Type& result = resultTypeOf(call);
    if (!result.hasObjects() || !m_facts.someObjectHas(types(), types().intern(result), ObjectFact::HasDecidedStorage))
        return true;
    return storageNamedByFactsFor(result) == made;
}

// The stub makes the kind of storage that the function makes: that of the receiver.
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
    // Under the name it finds what its class gives it, and its elements lie where the class lays them.
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

void ArrayBuiltinEmitter::scanShortVector(const ArrayStorageForm& form, CellPointer array, Operand searchedRawElement, Join<Integer>& found)
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
    ValueFromBlock start = m_out.anchor(integer(0).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer index(m_out.phi(Machine::Int64, start));
    branch(below(index, length), { normalEdge(element), normalEdge(isAbsent) });

    m_out.appendTo(element);
    Operand raw = elements().loadRawElement(form, storage, index);
    // A hole equals nothing that is searched: it is a NaN among doubles, and the empty word among boxed values. +0 equals -0 among doubles, as both strict equality and SameValueZero want.
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

// Storage of doubles holds no NaN and storage of int32 no fraction, so a number that is searched there in vain is in the array under no other spelling.
Integer ArrayBuiltinEmitter::indexOfNumber(const InlineCall& call, CellPointer array)
{
    Number searched = numberOperand(call.argument(0));
    Join<Integer> join(m_out);
    auto searchDoubles = [&] {
        scanShortVector(arrayStorageFormOf(ArrayStorage::Double), array, searched, join);
        join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfDouble, { array, searched }));
    };
    auto searchIntegers = [&] {
        LBasicBlock isInt32 = newBlock();
        LBasicBlock isNoInt32 = newBlock();
        Word32 truncated = low32(emitter<ConversionEmitter>().toInteger(searched));
        branch(equal(toNumber(truncated), searched), { normalEdge(isInt32), normalEdge(isNoInt32) });

        m_out.appendTo(isInt32);
        Boxed boxed = boxInt32(truncated);
        scanShortVector(arrayStorageFormOf(ArrayStorage::Int32), array, boxed, join);
        join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfInt32, { array, boxed }));

        m_out.appendTo(isNoInt32);
        join.add(integer(-1));
    };
    const ArrayStorageForm* proven = provenFormOf(call);
    if (proven && proven->kind == ArrayStorage::Double) {
        searchDoubles();
        return join.finish();
    }
    if (proven && proven->kind == ArrayStorage::Int32) {
        searchIntegers();
        return join.finish();
    }

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
    if (const ArrayStorageForm* proven = provenFormOf(call); !proven || proven->kind != form.kind)
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

    scanShortVector(form, array, searched, join);
    join.add(callOperation<Integer>(call.context(), RuntimeOperation::ArrayIndexOfInt32, { array, searched }));
    return join.finish();
}

Integer ArrayBuiltinEmitter::indexOfString(const InlineCall& call, CellPointer array, CellPointer searched)
{
    bool elementsAreStrings = types().at(elementTypeOf(call, TypeLevel::Proven)).isAnyString() && elements().isDense(call.receiver());
    if (call.numberOfArguments() == 1) {
        StubKey stub = arrayIndexOfString(elementsAreStrings ? SearchedElements::Strings : SearchedElements::Values);
        return Integer(callStub(call.context(), stub, { array.value(), searched.value() }).first);
    }
    // Below 2^32: a negative start counts from the end, and the stub multiplies this by the size of an element. The function behind the call answers both.
    Integer start = wholeNumberOrElse(call.argument(1), missBlockOf(call));
    missUnless(call, below(start, integer(1ll << 32)));
    StubKey stub = arrayIndexOfString(elementsAreStrings ? SearchedElements::StringsFromStart : SearchedElements::ValuesFromStart);
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
    return indexOfValueComparedByIdentity(call, receiverOf(call), hole);
}

bool ArrayBuiltinEmitter::handlesSearch(const InlineCall& call)
{
    if (call.numberOfArguments() == 2)
        return call.canMiss() && typeOf(call.argument(0)).isAnyString() && typeOf(call.argument(1)).isNumber();
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
