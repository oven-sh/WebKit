/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElementAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTGuessedAccessEmitter.h"
#include "AOTStubsArrayWords.h"
#include "AOTStubsArrays.h"
#include <ranges>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ElementConversion : uint8_t {
    NeverFits,
    UseBoxed,
    BoxInt32,
    BoxDouble,
    BoxDoubleAsInt32,
    BoxDoubleIfInt32,
    BoxInt64IfInt32,
    Int64ToDouble,
    BoxInt64,
    CheckBoxedInt32,
    Int32ToDouble,
    UseDouble,
    UnboxNumber,
};

} // anonymous namespace

static ElementConversion elementConversionTo(const ArrayStorageForm& form, Representation stored, const Type& type)
{
    bool isUnboxedDouble = stored == Representation::Double || stored == Representation::OptionalDouble;
    // The counter of a loop over a list. No integer that is held so is beyond 2^53.
    if (stored == Representation::Int64)
        return form.element == Representation::Int32 ? ElementConversion::BoxInt64IfInt32 : form.element == Representation::Double ? ElementConversion::Int64ToDouble : ElementConversion::BoxInt64;
    if (form.element == Representation::Int32) {
        if (stored == Representation::Int32)
            return ElementConversion::BoxInt32;
        if (stored == Representation::JSValue && type.primitives & Primitive::Integer)
            return ElementConversion::CheckBoxedInt32;
        // A value keeps the representation it was built with, so a number that a test narrowed to int32 still arrives as a double.
        if (isUnboxedDouble && type.isInt32())
            return ElementConversion::BoxDoubleAsInt32;
        // A whole number that is held as a double lies there as the int32 that it is. Whatever else the double is gives the list another kind.
        if (isUnboxedDouble && type.primitives & Primitive::Integer)
            return ElementConversion::BoxDoubleIfInt32;
        return ElementConversion::NeverFits;
    }
    if (form.element == Representation::Double) {
        if (stored == Representation::Int32)
            return ElementConversion::Int32ToDouble;
        if (!(type.primitives & Primitive::Number))
            return ElementConversion::NeverFits;
        if (isUnboxedDouble)
            return ElementConversion::UseDouble;
        return stored == Representation::JSValue ? ElementConversion::UnboxNumber : ElementConversion::NeverFits;
    }
    RELEASE_ASSERT(form.element == Representation::JSValue);
    if (stored == Representation::JSValue)
        return ElementConversion::UseBoxed;
    if (stored == Representation::Int32)
        return ElementConversion::BoxInt32;
    return isUnboxedDouble ? ElementConversion::BoxDouble : ElementConversion::NeverFits;
}

static ArrayStorage storageExpectedToHold(Representation stored)
{
    return stored == Representation::Double ? ArrayStorage::Double : ArrayStorage::Contiguous;
}

ArrayStorageFormList ElementAccessEmitter::candidateFormsFor(ValueID stored, ArrayStorage expected)
{
    Representation representation = representationOf(stored);
    const Type& type = typeOf(stored);
    ArrayStorageFormList forms;
    auto appendIfPossible = [&](const ArrayStorageForm& form) {
        if (elementConversionTo(form, representation, type) != ElementConversion::NeverFits)
            forms.appendIfNotContains(&form);
    };
    if (const ArrayStorageForm* form = findArrayStorageForm(expected))
        appendIfPossible(*form);
    for (const ArrayStorageForm& form : arrayStorageForms | std::views::reverse)
        appendIfPossible(form);
    return forms;
}

// `number[]` declares no kind: a list that has taken numbers alone has storage for int32 or for doubles, whichever its maker and the numbers so far have given it. So the arm is chosen by the kind that the list has.
// First the kind that a value of the type leaves as it is. Storage for any value under a list of numbers is what `new Array(n)` and a few functions of the library make: it has an arm where the store comes again and again.
ArrayStorageFormList ElementAccessEmitter::formsOfListOfNumbersFor(ValueID stored, bool hasArmForEveryKind)
{
    bool isWhole = representationOf(stored) == Representation::Int32 || representationOf(stored) == Representation::Int64 || typeOf(stored).isNumberWithoutFraction();
    ArrayStorageFormList forms = candidateFormsFor(stored, isWhole ? ArrayStorage::Int32 : ArrayStorage::Double);
    size_t placeOfAnyValue = forms.findIf([](const ArrayStorageForm* form) {
        return form->holdsCells;
    });
    if (placeOfAnyValue == notFound)
        return forms;
    const ArrayStorageForm* forAnyValue = forms[placeOfAnyValue];
    forms.removeAt(placeOfAnyValue);
    if (hasArmForEveryKind)
        forms.append(forAnyValue);
    return forms;
}

bool ElementAccessEmitter::goesByKindOfListOfNumbers(ValueID list, ValueID stored)
{
    return typeOf(stored).isNumber() && isDeclaredToHoldNumbersAlone(m_program, types(), typeIDOf(list));
}

ArrayStorageFormList ElementAccessEmitter::formsThatMayHold(ValueID list, ValueID stored)
{
    return goesByKindOfListOfNumbers(list, stored) ? formsOfListOfNumbersFor(stored, true) : candidateFormsFor(stored, ArrayStorage::Other);
}

ArrayStorageFormList ElementAccessEmitter::formExpectedToHold(ValueID list, ValueID stored)
{
    if (goesByKindOfListOfNumbers(list, stored))
        return formsOfListOfNumbersFor(stored, false);
    ArrayStorageFormList forms = candidateFormsFor(stored, storageExpectedToHold(representationOf(stored)));
    if (forms.size() > 1)
        forms.shrink(1);
    return forms;
}

bool ElementAccessEmitter::hasOnlyExpectedArmInLine(const ValueContext& store)
{
    if (store.opcode() != Opcode::PutArrayElement)
        return false;
    // openIn = Loop of the row.
    return !generator().takesFormOfLoops(store);
}

void ElementAccessEmitter::forEachFormByShape(Word32 mode, ShapeTest hasShapeOf, const ArrayStorageFormList& forms, LBasicBlock hasNoForm, const ScopedLambda<void(const ArrayStorageForm&)>& emitArm, FirstForm firstForm)
{
    for (const ArrayStorageForm* form : forms) {
        LBasicBlock hasForm = newBlock();
        LBasicBlock hasOtherForm = newBlock();
        bool othersAreRare = form == forms.last() || (firstForm == FirstForm::IsExpected && form == forms.first());
        m_out.branch((this->*hasShapeOf)(mode, *form).value(), normalEdge(hasForm), othersAreRare ? rareEdge(hasOtherForm) : normalEdge(hasOtherForm));
        m_out.appendTo(hasForm);
        emitArm(*form);
        m_out.appendTo(hasOtherForm);
    }
    m_out.jump(hasNoForm);
}

std::optional<Operand> ElementAccessEmitter::rawElementToStore(const ArrayStorageForm& form, ValueID stored, LBasicBlock doesNotFit)
{
    const Type& type = typeOf(stored);
    Operand operand = get(stored);
    auto numberToStore = [&](Number number) -> Operand {
        if (!canStoreWithoutConversion(types(), form.kind, typeIDOf(stored)))
            branchUnless(equal(number, number), doesNotFit);
        return number;
    };

    switch (elementConversionTo(form, operand.representation, type)) {
    case ElementConversion::NeverFits:
        return std::nullopt;
    case ElementConversion::UseBoxed:
        return operand;
    case ElementConversion::BoxInt32:
        return Operand(boxInt32(operand.as<Word32>()));
    case ElementConversion::BoxDouble: {
        Number number(operand.value);
        if (operand.is<OptionalNumber>())
            branchUnless(equal(number, number), doesNotFit);
        return Operand(emitter<ConversionEmitter>().boxNumber(operand.is<OptionalNumber>() ? Operand(number) : operand));
    }
    case ElementConversion::BoxDoubleAsInt32:
        return Operand(boxInt32(emitter<ConversionEmitter>().toInt32(Number(operand.value))));
    case ElementConversion::BoxDoubleIfInt32:
        return Operand(boxInt32(emitter<ConversionEmitter>().int32OrElse(Number(operand.value), doesNotFit)));
    case ElementConversion::BoxInt64IfInt32:
        return Operand(boxInt32(emitter<ConversionEmitter>().int32OrElse(operand.as<Integer>(), type.integers, doesNotFit)));
    case ElementConversion::Int64ToDouble:
        return Operand(emitter<ConversionEmitter>().toNumber(operand.as<Integer>(), type.integers));
    case ElementConversion::BoxInt64:
        return Operand(emitter<ConversionEmitter>().boxInteger(operand.as<Integer>(), type.integers));
    case ElementConversion::CheckBoxedInt32:
        branchUnless(isInt32(operand.as<Boxed>()), doesNotFit);
        return operand;
    case ElementConversion::Int32ToDouble:
        return Operand(toNumber(operand.as<Word32>()));
    case ElementConversion::UseDouble:
        return numberToStore(Number(operand.value));
    case ElementConversion::UnboxNumber: {
        Boxed boxed = operand.as<Boxed>();
        if (!type.isNumber())
            branchUnless(isNumber(boxed), doesNotFit);
        return numberToStore(numberToDouble(boxed));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ElementAccessEmitter::storeInVector(const ValueContext& store, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, Operand rawElement, LBasicBlock isBeyondVector)
{
    LengthTest test = testLength(index, elements);
    bool isExpectedToAppend = store.opcode() == Opcode::PutArrayElement && store->isExpectedToAppend();
    // The stub behind the store also puts an element behind the last one.
    if (hasOnlyExpectedArmInLine(store) && !isExpectedToAppend) {
        branchUnless(test.isInside, isBeyondVector);
        replaceElement(store, form, elements, test.indexInside, rawElement);
        return;
    }

    LBasicBlock isBeyondLength = newBlock();
    LBasicBlock isWithinLength = newBlock();
    if (isExpectedToAppend)
        m_out.branch(test.isInside.value(), unlikelyEdge(isWithinLength), normalEdge(isBeyondLength));
    else
        m_out.branch(test.isInside.value(), normalEdge(isWithinLength), unlikelyEdge(isBeyondLength));

    m_out.appendTo(isBeyondLength);
    branchUnless(below(index, vectorLength(elements)), isBeyondVector);
    setPublicLength(elements, add(index, integer(1)));
    if (!replacesBoxedInt32(store, form)) {
        m_out.jump(isWithinLength);
        m_out.appendTo(isWithinLength);
        replaceElement(store, form, elements, test.indexInside, rawElement);
        return;
    }

    LBasicBlock continuation = newBlock();
    storeRawElement(form, elements, index, rawElement);
    m_out.jump(continuation);

    m_out.appendTo(isWithinLength);
    replaceElement(store, form, elements, test.indexInside, rawElement);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

// Inside the length of an array of int32 without holes every element is a boxed int32, and all of them have the same high half.
bool ElementAccessEmitter::replacesBoxedInt32(const ValueContext& store, const ArrayStorageForm& form)
{
    return form.element == Representation::Int32 && representationOf(store->valueOperand()) == Representation::Int32 && isDense(store->arrayOperand());
}

void ElementAccessEmitter::replaceElement(const ValueContext& store, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, Operand rawElement)
{
    if (replacesBoxedInt32(store, form)) {
        Word32 stored = in<Word32>(store->valueOperand());
        m_out.store32(stored.value(), elementAt(form, elements, index));
        return;
    }
    storeRawElement(form, elements, index, rawElement);
    if (form.holdsCells && typeOf(store->valueOperand()).hasCells()) {
        Boxed stored = rawElement.as<Boxed>();
        emitter<BarrierEmitter>().elementBarrierAfterStore(store, elements.array, singleElementSpan(stored));
    }
}

// The walk sets isInBounds only where the value's type fits the storage without conversion and a HasArrayStorage[isWritable] test has passed, so nothing is tested here.
void ElementAccessEmitter::storeInsideLength(const ValueContext& value, const ArrayStorageForm& form)
{
    ValueID stored = value->valueOperand();
    Representation representation = representationOf(stored);
    // The operation declares the effects of a single store, so there is no slow path to take. The marker in an optional number would need one.
    bool fitsWithoutTest = canStoreWithoutConversion(types(), form.kind, typeIDOf(stored)) && representation != Representation::OptionalDouble;
    if (!fitsWithoutTest || elementConversionTo(form, representation, typeOf(stored)) == ElementConversion::NeverFits) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements elements = elementsOf(value, array);
    Integer index = indexInsideLength(value->positionOperand());
    // A boxed integer may be boxed as a double, which storage of int32 does not hold.
    bool unboxesInt32 = form.element == Representation::Int32 && representation == Representation::JSValue;
    Operand rawElement = unboxesInt32
        ? Operand(boxInt32(emitter<ConversionEmitter>().unbox(in<Boxed>(stored), typeOf(stored), Representation::Int32).as<Word32>()))
        : *rawElementToStore(form, stored, nullptr);
    replaceElement(value, form, elements, index, rawElement);
}

void ElementAccessEmitter::generatePutArrayElement(const ValueContext& value)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateElementStore(value);
        return;
    }

    const ArrayStorageForm* knownForm = findArrayStorageForm(value->arrayStorage());
    if (knownForm && value->isInBounds()) {
        storeInsideLength(value, *knownForm);
        return;
    }

    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID stored = value->valueOperand();
    bool hasArmInLine = !knownForm || elementConversionTo(*knownForm, representationOf(stored), typeOf(stored)) != ElementConversion::NeverFits;
    if (!hasArmInLine) {
        m_out.noteForm(Form::Closed, StateOfKnowledge::ValueNeverFitsStorage);
        storeOutOfLine(value, array, NumberBoxing::InLine);
        return;
    }

    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    Integer index = indexOrElse(value->positionOperand(), value->keyIsIntegral(), slowPath, NaNIndexMode::ProduceOutOfBoundsIndex);
    if (knownForm) {
        // A storage kind that no passed test stands behind may be only a prediction, and only the run-time operation may replace copy-on-write storage.
        if (!value->storageIsWritable())
            branchUnless(hasWritableShape(indexingMode(array, IndexingShapeAndWritabilityMask), *knownForm), slowPath);
        Operand rawElement = *rawElementToStore(*knownForm, stored, slowPath);
        storeInVector(value, *knownForm, elementsOf(array), index, rawElement, slowPath);
        m_out.jump(continuation);
    } else {
        Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
        ArrayElements elements = elementsOf(array);
        // Only the storage kind that the stored value most likely goes into gets an inline arm. The stubs have a fast path for the others.
        forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, formExpectedToHold(value->arrayOperand(), stored), slowPath, [&](const ArrayStorageForm& form) {
            Operand rawElement = *rawElementToStore(form, stored, slowPath);
            storeInVector(value, form, elements, index, rawElement, slowPath);
            m_out.jump(continuation);
        });
    }

    m_out.appendTo(slowPath);
    storeOutOfLine(value, array, hasOnlyExpectedArmInLine(value) ? NumberBoxing::ByStub : NumberBoxing::InLine);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// Nothing is left to test but the kind of the storage and the length. An element inside the length is replaced in line, a hole too: no list with storage of its own of a plain kind has an accessor, a read-only element or
// a fixed length, on itself or above it (emitStoreInsideVector() of the stub, JSObject::putByIndex). The stub takes every other way: another kind, a value that the kind refuses, a position behind the end.
void ElementAccessEmitter::storeInProvenList(const ValueContext& value, const ArrayStorageFormList& forms)
{
    CellPointer list = in<CellPointer>(value->arrayOperand());
    ValueID stored = value->valueOperand();
    LBasicBlock outOfLine = newBlock();
    LBasicBlock continuation = newBlock();
    Integer index = indexOrElse(value->positionOperand(), true, nullptr);
    ArrayElements elements = elementsOf(list);
    forEachFormByShape(indexingMode(list, IndexingShapeAndWritabilityMask), &ElementAccessEmitter::hasWritableShape, forms, outOfLine, [&](const ArrayStorageForm& form) {
        std::optional<Operand> rawElement = rawElementToStore(form, stored, outOfLine);
        if (!rawElement) {
            m_out.jump(outOfLine);
            return;
        }
        LengthTest test = testLength(index, elements);
        if (!value->isInBounds())
            branchUnless(test.isInside, outOfLine);
        storeRawElement(form, elements, test.indexInside, *rawElement);
        if (form.holdsCells && typeOf(stored).hasCells()) {
            Boxed storedElement = rawElement->as<Boxed>();
            emitter<BarrierEmitter>().elementBarrierAfterStore(value, list, singleElementSpan(storedElement));
        }
        m_out.jump(continuation);
    }, FirstForm::IsLikeTheOthers);

    m_out.appendTo(outOfLine);
    storeOutOfLine(value, list, NumberBoxing::ByStub);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

Boxed ElementAccessEmitter::boxForStub(const OperationContext& store, Operand operand, const Type& type, NumberBoxing boxing)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    if (boxing == NumberBoxing::ByStub && operand.is<Number>() && !operand.value->hasDouble())
        return conversions.boxNumberByStub(operand.as<Number>());
    return conversions.box(store, operand, type);
}

void ElementAccessEmitter::storeOutOfLine(const ValueContext& value, CellPointer array, NumberBoxing boxing)
{
    ValueID position = value->positionOperand();
    ValueID stored = value->valueOperand();
    const Type& storedType = typeOf(stored);
    Boxed boxedValue = boxForStub(value, emitter<ConversionEmitter>().inFormOfType(get(stored), storedType), storedType, boxing);
    if (value->mode() == ECMAMode::Sloppy) {
        emitter<AccessEmitter>().putByVal(value, boxCell(array), boxedPosition(position), boxedValue, AccessEmitter::putModeOf(ECMAMode::Sloppy));
        return;
    }

    bool mayConvertToDouble = storedType.isNumberLike() && !storedType.isInt32();
    Operand key = get(position);
    // The stubs take the position as it is, store wherever the vector has room, and preserve the caller's registers.
    ArrayStoredValue storedValue = mayConvertToDouble ? ArrayStoredValue::Number : ArrayStoredValue::Value;
    if (key.is<Number>()) {
        callStub(value, arrayPutAtNumber(storedValue), { array.value(), boxedValue.value(), key.value });
        return;
    }
    callStub(value, arrayPutAtIndex(storedValue), { array.value(), boxedValue.value(), integerOf(position).value() });
}

// Nothing: the operation names none, or what it names lies in no integer register.
std::optional<Integer> ElementAccessEmitter::integerNamedByAppend(const ValueContext& store, size_t index)
{
    if (store.opcode() != Opcode::AppendElement || store->lengthOperands().size() <= index)
        return std::nullopt;
    Operand named = get(store->lengthOperands()[index]);
    if (named.representation == Representation::Int32)
        return zeroExtended(named.as<Word32>());
    if (named.representation == Representation::Int64)
        return named.as<Integer>();
    return std::nullopt;
}

Integer ElementAccessEmitter::lengthNamedOrRead(const ValueContext& store, const ArrayElements& elements)
{
    std::optional<Integer> named = integerNamedByAppend(store, 0);
    if (!named)
        return publicLength(elements);
    return *named;
}

// The room that the operation names is what the list had in front of the loop, and it has no less now. Where that is used up the list may have grown since.
void ElementAccessEmitter::branchUnlessHasRoomBehindLength(const ValueContext& store, const ArrayElements& elements, Integer length, LBasicBlock isFull)
{
    std::optional<Integer> roomInFrontOfLoop = integerNamedByAppend(store, 1);
    if (!roomInFrontOfLoop) {
        branchUnless(below(length, vectorLength(elements)), isFull);
        return;
    }

    LBasicBlock hasRoom = newBlock();
    LBasicBlock isBeyondNamedRoom = newBlock();
    m_out.branch(below(length, *roomInFrontOfLoop).value(), normalEdge(hasRoom), unlikelyEdge(isBeyondNamedRoom));

    m_out.appendTo(isBeyondNamedRoom);
    m_out.branch(below(length, vectorLength(elements)).value(), normalEdge(hasRoom), rareEdge(isFull));

    m_out.appendTo(hasRoom);
}

void ElementAccessEmitter::appendElement(const ValueContext& store, CellPointer array, ValueID element)
{
    // On x86-64 one arm in line is 113 bytes.
    if (!generator().takesOpenForm(store)) {
        appendOutOfLine(store, array, element, NumberBoxing::ByStub);
        return;
    }

    ArrayStorage expected = ArrayAllocationEmitter(generator()).formAtAllocation(typeOf(store->arrayOperand())).kind;
    ArrayStorage declared = arrayStorageDeclaredBy(m_program, types(), typeIDOf(store->arrayOperand())).predicted();
    // `const list: number[] = []; .. list.push(n)`: the type names the object of the site, which declares nothing. The literal in sight names the kind that it has given the list.
    // A value that the kind may refuse gives the list another kind, and the appends behind that one go on in line.
    if (const Operation& maker = operationAt(definitionOf(store->arrayOperand())); !hasArrayStorageForm(declared) && maker.opcode == Opcode::NewArray && hasArrayStorageForm(maker.arrayStorage())) {
        expected = maker.arrayStorage();
        if (canStoreWithoutConversion(types(), expected, typeIDOf(element)))
            declared = expected;
    }
    ArrayStorageFormList forms = candidateFormsFor(element, expected);
    bool hasOnlyExpectedArm = hasOnlyExpectedArmInLine(store);
    // An array whose storage kind is not the one its element type declares takes the stub, which handles every kind.
    if (forms.size() > 1 && (hasOnlyExpectedArm || (hasArrayStorageForm(declared) && forms.first()->kind == declared)))
        forms.shrink(1);
    else if (!hasArrayStorageForm(declared) && goesByKindOfListOfNumbers(store->arrayOperand(), element))
        forms = formsOfListOfNumbersFor(element, true);
    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
    ArrayElements elements = elementsOf(array);
    forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, forms, slowPath, [&](const ArrayStorageForm& form) {
        Operand rawElement = *rawElementToStore(form, element, slowPath);
        Integer length = lengthNamedOrRead(store, elements);
        branchUnlessHasRoomBehindLength(store, elements, length, slowPath);

        setPublicLength(elements, add(length, integer(1)));
        storeRawElement(form, elements, length, rawElement);
        if (form.holdsCells && typeOf(element).hasCells()) {
            Boxed stored = rawElement.as<Boxed>();
            emitter<BarrierEmitter>().elementBarrierAfterStore(store, elements.array, singleElementSpan(stored));
        }
        m_out.jump(continuation);
    });

    m_out.appendTo(slowPath);
    appendOutOfLine(store, array, element, hasOnlyExpectedArm ? NumberBoxing::ByStub : NumberBoxing::InLine);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

Boxed ElementAccessEmitter::appendOutOfLine(const OperationContext& store, CellPointer array, ValueID element)
{
    return appendOutOfLine(store, array, element, NumberBoxing::InLine);
}

Boxed ElementAccessEmitter::appendOutOfLine(const OperationContext& store, CellPointer array, ValueID element, NumberBoxing boxing)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Operand operand = conversions.inFormOfType(get(element), typeOf(element));
    if (operand.is<Number>() && typeOf(element).isInt32())
        operand = Operand(boxInt32(conversions.toInt32(operand.as<Number>())));
    return Boxed(callStub(store, arrayPush(), { array.value(), boxForStub(store, operand, typeOf(element), boxing).value() }).first);
}

Condition ElementAccessEmitter::tryStoreElement(const ValueContext& store, CellPointer array, Integer index, ValueID stored)
{
    RELEASE_ASSERT(store->valueOperand() == stored);
    LBasicBlock otherwise = newBlock();
    Join<Condition> join(m_out);
    Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
    ArrayElements elements = elementsOf(array);
    forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, candidateFormsFor(stored, storageExpectedToHold(representationOf(stored))), otherwise, [&](const ArrayStorageForm& form) {
        Operand rawElement = *rawElementToStore(form, stored, otherwise);
        storeInVector(store, form, elements, index, rawElement, otherwise);
        join.add(boolean(true));
    });

    m_out.appendTo(otherwise);
    join.add(boolean(false));
    return join.finish();
}

void ElementAccessEmitter::generateReserveArrayCapacity(const ValueContext& value)
{
    RuntimeOperation operation = value->isSureToBeFilled() ? RuntimeOperation::ReserveWholeArrayCapacity : RuntimeOperation::ReserveArrayCapacity;
    callVoidOperation(value, operation, { in<CellPointer>(value->arrayOperand()), integerOf(value->additionalOperand()) });
}

// It stands behind a failed test for Double storage, and the run time looks at the array.
void ElementAccessEmitter::generateConvertToDoubleStorage(const ValueContext& value)
{
    uint32_t place = value->isExpectedToHoldNumbers() ? value.id().index() % ArrayWords::numberOfCounts : ArrayWords::numberOfCounts;
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer copiedFrom = value->copiedFromOperand() ? in<CellPointer>(value->copiedFromOperand()) : array;
    callVoidOperation(value, RuntimeOperation::ConvertToDoubleStorage, { array, copiedFrom, word32(static_cast<int32_t>(place)) }, { .isOnRarePath = true });
}

void ElementAccessEmitter::generateAppendElements(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    callStub(value, appendElements(), { array.value(), source.value() });
}

void ElementAccessEmitter::generateAppendMembers(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    callStub(value, appendMembers(), { array.value(), source.value() });
}

static ArrayElementKind elementKindOfListBornFilled(ArrayStorage named)
{
    const ArrayStorageForm* form = findArrayStorageForm(named);
    return arrayElementKindOf(form ? form->indexingType : ArrayWithContiguous);
}

void ElementAccessEmitter::generateNewArrayOfMembers(const ValueContext& value)
{
    CellPointer source = in<CellPointer>(value->sourceOperand());
    define(value, CellPointer(callStub(value, newArrayOfMembers(elementKindOfListBornFilled(value->arrayStorage())), { source.value() }).first));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
