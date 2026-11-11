/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElementAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTArrayAllocationProfile.h"
#include "AOTBarrierEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTGuessedAccessEmitter.h"
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
    if (stored == Representation::Int64)
        return form.element == Representation::Int32 ? ElementConversion::BoxInt64IfInt32 : form.element == Representation::Double ? ElementConversion::Int64ToDouble : ElementConversion::BoxInt64;
    if (form.element == Representation::Int32) {
        if (stored == Representation::Int32)
            return ElementConversion::BoxInt32;
        if (stored == Representation::JSValue && type.primitives & Primitive::Integer)
            return ElementConversion::CheckBoxedInt32;
        if (isUnboxedDouble && type.isInt32())
            return ElementConversion::BoxDoubleAsInt32;
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

ArrayStorageFormList ElementAccessEmitter::formsOfListOfNumbersFor(ValueID stored, bool hasArmForEveryKind)
{
    bool isWhole = representationOf(stored) == Representation::Int32 || representationOf(stored) == Representation::Int64 || typeOf(stored).hasOnlyPrimitives(Primitive::NumberWithoutFraction | Primitive::UndefinedOrNull);
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
    const Type& type = typeOf(stored);
    return type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull) && isDeclaredToHoldNumbersAlone(m_program, types(), typeIDOf(list));
}

const ArrayStorageForm& ElementAccessEmitter::expectedFormOf(ValueID array)
{
    ArrayStorage declared = arrayStorageDeclaredBy(m_program, types(), typeIDOf(array)).predicted();
    if (hasArrayStorageForm(declared))
        return arrayStorageFormOf(declared);
    return arrayStorageFormOf(isDeclaredToHoldNumbersAlone(m_program, types(), typeIDOf(array)) ? ArrayStorage::Int32 : ArrayStorage::Contiguous);
}

ArrayStorageFormList ElementAccessEmitter::formsThatMayHold(ValueID list, ValueID stored)
{
    return goesByKindOfListOfNumbers(list, stored) ? formsOfListOfNumbersFor(stored, true) : candidateFormsFor(stored, ArrayStorage::Other);
}

ArrayStorageFormList ElementAccessEmitter::formExpectedToHold(ValueID list, ValueID stored)
{
    bool isTestedForNumber = representationOf(stored) == Representation::JSValue && typeOf(stored).primitives & Primitive::Number && isDeclaredToHoldNumbersAlone(m_program, types(), typeIDOf(list));
    if (isTestedForNumber || goesByKindOfListOfNumbers(list, stored))
        return formsOfListOfNumbersFor(stored, false);
    ArrayStorageFormList forms = candidateFormsFor(stored, storageExpectedToHold(representationOf(stored)));
    if (forms.size() > 1)
        forms.shrink(1);
    return forms;
}

ValueID ElementAccessEmitter::int32ThatArmOfValuesStoresInEitherKind(ValueID stored, const ArrayStorageFormList& forms)
{
    if (forms.size() != 1 || forms[0]->kind != ArrayStorage::Contiguous || !typeOf(stored).isInt32())
        return { };
    if (representationOf(stored) != Representation::JSValue && representationOf(stored) != Representation::Int32)
        return { };
    build().statistics().count(StatisticID::Int32StoresSharingBoxedPath);
    return stored;
}

bool ElementAccessEmitter::hasOnlyExpectedArmInLine(const ValueContext& store)
{
    if (store.opcode() != Opcode::PutArrayElement)
        return false;
    return !generator().takesFormOfLoops(store);
}

void ElementAccessEmitter::forEachFormByShape(Word32 mode, ShapeTest hasShapeOf, const ArrayStorageFormList& forms, LBasicBlock hasNoForm, const ScopedLambda<void(const ArrayStorageForm&)>& emitArm, FirstForm firstForm, ValueID int32StoredInEitherKind)
{
    for (const ArrayStorageForm* form : forms) {
        LBasicBlock hasForm = newBlock();
        LBasicBlock hasOtherForm = newBlock();
        bool othersAreRare = form == forms.last() || (firstForm == FirstForm::IsExpected && form == forms.first());
        if (int32StoredInEitherKind && form->kind == ArrayStorage::Contiguous) {
            LBasicBlock holdsNoValues = newBlock();
            m_out.branch((this->*hasShapeOf)(mode, *form).value(), normalEdge(hasForm), normalEdge(holdsNoValues));
            m_out.appendTo(holdsNoValues);
            bool tagIsAsked = representationOf(int32StoredInEitherKind) == Representation::JSValue;
            LBasicBlock holdsInt32 = tagIsAsked ? newBlock() : hasForm;
            m_out.branch((this->*hasShapeOf)(mode, arrayStorageFormOf(ArrayStorage::Int32)).value(), normalEdge(holdsInt32), othersAreRare ? rareEdge(hasOtherForm) : normalEdge(hasOtherForm));
            if (tagIsAsked) {
                m_out.appendTo(holdsInt32);
                m_out.branch(isInt32(in<Boxed>(int32StoredInEitherKind)).value(), normalEdge(hasForm), othersAreRare ? rareEdge(hasOtherForm) : normalEdge(hasOtherForm));
            }
        } else
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
        if (!canStoreWithoutConversion(types(), form.kind, typeIDOf(stored)) && !AOT::hasFlag(build().options().testing, Wire::TestingSwitch::AssumeNoNaNStores))
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
    if (hasOnlyExpectedArmInLine(store)) {
        branchUnless(test.isInside, isBeyondVector);
        replaceElement(store, form, elements, test.indexInside, rawElement);
        return;
    }

    LBasicBlock isBeyondLength = newBlock();
    LBasicBlock isWithinLength = newBlock();
    m_out.branch(test.isInside.value(), normalEdge(isWithinLength), unlikelyEdge(isBeyondLength));

    m_out.appendTo(isBeyondLength);
    branchUnless(below(index, vectorLength(elements)), isBeyondVector);
    setPublicLength(elements, add(index, integer(1)));
    m_out.jump(isWithinLength);
    m_out.appendTo(isWithinLength);
    replaceElement(store, form, elements, test.indexInside, rawElement);
}

void ElementAccessEmitter::replaceElement(const ValueContext& store, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, Operand rawElement)
{
    stopWhereHoleWouldBeStored(store, form, rawElement);
    storeRawElement(form, elements, index, rawElement);
    if (form.holdsCells && typeOf(store->valueOperand()).hasCells()) {
        Boxed stored = rawElement.as<Boxed>();
        emitter<BarrierEmitter>().elementBarrierAfterStore(store, elements.array, singleElementSpan(stored));
    }
}

void ElementAccessEmitter::storeInsideLength(const ValueContext& value, const ArrayStorageForm& form)
{
    ValueID stored = value->valueOperand();
    Representation representation = representationOf(stored);
    bool fitsWithoutTest = canStoreWithoutConversion(types(), form.kind, typeIDOf(stored)) && representation != Representation::OptionalDouble;
    if (!fitsWithoutTest || elementConversionTo(form, representation, typeOf(stored)) == ElementConversion::NeverFits) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements elements = elementsOf(value, array);
    Integer index = indexInsideLength(value->positionOperand());
    bool unboxesInt32 = form.element == Representation::Int32 && representation == Representation::JSValue;
    Operand rawElement = unboxesInt32
        ? Operand(boxInt32(emitter<ConversionEmitter>().unbox(in<Boxed>(stored), typeOf(stored), Representation::Int32).as<Word32>()))
        : *rawElementToStore(form, stored, nullptr);
    replaceElement(value, form, elements, index, rawElement);
}

void ElementAccessEmitter::generatePutArrayElement(const ValueContext& value)
{
    if (value->isInBounds())
        holdPositionInsideList(value);
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateElementStore(value);
        return;
    }

    const ArrayStorageForm* knownForm = findArrayStorageForm(value->arrayStorage());
    if (value->fillsPlaceOfListMadeHere()) {
        ArrayStorageFormList forms;
        if (knownForm)
            forms.append(knownForm);
        else
            forms = hasOnlyExpectedArmInLine(value) ? formExpectedToHold(value->arrayOperand(), value->valueOperand()) : formsThatMayHold(value->arrayOperand(), value->valueOperand());
        storeInProvenList(value, forms);
        return;
    }
    if (knownForm && value->isInBounds()) {
        holdKindOfList(value, StorageIsWritten::Yes);
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
    Integer index = indexOfAccessOrElse(value, slowPath, NaNIndexMode::ProduceOutOfBoundsIndex);
    if (knownForm) {
        if (!value->storageIsWritable())
            branchUnless(hasWritableShape(indexingMode(array, IndexingShapeAndWritabilityMask), *knownForm), slowPath);
        else
            holdKindOfList(value, StorageIsWritten::Yes);
        Operand rawElement = *rawElementToStore(*knownForm, stored, slowPath);
        storeInVector(value, *knownForm, elementsOf(array), index, rawElement, slowPath);
        m_out.jump(continuation);
    } else {
        Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
        ArrayElements elements = elementsOf(array);
        ArrayStorageFormList forms = formExpectedToHold(value->arrayOperand(), stored);
        forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, forms, slowPath, [&](const ArrayStorageForm& form) {
            Operand rawElement = *rawElementToStore(form, stored, slowPath);
            storeInVector(value, form, elements, index, rawElement, slowPath);
            m_out.jump(continuation);
        }, FirstForm::IsExpected, int32ThatArmOfValuesStoresInEitherKind(stored, forms));
    }

    m_out.appendTo(slowPath);
    storeOutOfLine(value, array, hasOnlyExpectedArmInLine(value) ? NumberBoxing::ByStub : NumberBoxing::InLine);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

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
        if (!value->isInBounds() && generator().takesFormOfLoops(value)) {
            LBasicBlock inBounds = newBlock();
            LBasicBlock pastEnd = newBlock();
            m_out.branch(test.isInside.value(), normalEdge(inBounds), normalEdge(pastEnd));
            m_out.appendTo(pastEnd);
            branchUnless(equal(test.indexInside, publicLength(elements)), outOfLine);
            branchUnless(below(test.indexInside, vectorLength(elements)), outOfLine);
            setPublicLength(elements, add(test.indexInside, integer(1)));
            m_out.jump(inBounds);
            m_out.appendTo(inBounds);
        } else if (!value->isInBounds())
            branchUnless(test.isInside, outOfLine);
        stopWhereHoleWouldBeStored(value, form, *rawElement);
        storeRawElement(form, elements, test.indexInside, *rawElement);
        if (form.holdsCells && typeOf(stored).hasCells()) {
            Boxed storedElement = rawElement->as<Boxed>();
            emitter<BarrierEmitter>().elementBarrierAfterStore(value, list, singleElementSpan(storedElement));
        }
        m_out.jump(continuation);
    }, FirstForm::IsLikeTheOthers, int32ThatArmOfValuesStoresInEitherKind(stored, forms));

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
    Operand key = get(position);
    if (value->mode() == ECMAMode::Sloppy) {
        if (key.is<Number>())
            emitter<AccessEmitter>().putByVal(value, boxCell(array), boxedPosition(position), boxedValue, AccessEmitter::putModeOf(ECMAMode::Sloppy));
        else
            callStub(value, arrayPutAtIndex(ArrayStoredValue::ValueInSloppyCode), { array.value(), boxedValue.value(), integerOf(position).value() });
        return;
    }

    bool isWholeNumberOfAnySize = storedType.isNumberLike() && !storedType.isInt32() && !(storedType.primitives & Primitive::FractionalNumber);
    if (isWholeNumberOfAnySize)
        build().statistics().count(StatisticID::IntegralStoresWithoutDoubleConversion);
    bool mayConvertToDouble = storedType.isNumberLike() && !storedType.isInt32() && !isWholeNumberOfAnySize;
    ArrayStoredValue storedValue = mayConvertToDouble ? ArrayStoredValue::Number : ArrayStoredValue::Value;
    if (key.is<Number>()) {
        callStub(value, arrayPutAtNumber(storedValue), { array.value(), boxedValue.value(), key.value });
        return;
    }
    callStub(value, arrayPutAtIndex(storedValue), { array.value(), boxedValue.value(), integerOf(position).value() });
}

void ElementAccessEmitter::appendElement(const ValueContext& store, CellPointer array, ValueID element)
{
    if (!generator().takesOpenForm(store)) {
        appendOutOfLine(store, array, element, NumberBoxing::ByStub);
        return;
    }

    ArrayStorage expected = ArrayAllocationEmitter(generator()).formAtAllocation(typeOf(store->arrayOperand())).kind;
    ArrayStorage declared = arrayStorageDeclaredBy(m_program, types(), typeIDOf(store->arrayOperand())).predicted();
    if (const Operation& maker = operationAt(definitionOf(store->arrayOperand())); !hasArrayStorageForm(declared) && maker.opcode == Opcode::NewArray && hasArrayStorageForm(maker.arrayStorage())) {
        expected = maker.arrayStorage();
        if (canStoreWithoutConversion(types(), expected, typeIDOf(element)))
            declared = expected;
    }
    ArrayStorageFormList forms = candidateFormsFor(element, expected);
    bool hasOnlyExpectedArm = hasOnlyExpectedArmInLine(store);
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
        Integer length = publicLength(elements);
        branchUnless(below(length, vectorLength(elements)), slowPath);

        setPublicLength(elements, add(length, integer(1)));
        stopWhereHoleWouldBeStored(store, form, rawElement);
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

void ElementAccessEmitter::widenStorageOfIntegers(CellPointer array, const ArrayStorageForm& wider, LBasicBlock isLeftToOthers)
{
    Integer headerForInt32(m_out.load64(mutableInstanceField(Instance::offsetOfNewArrayHeader(ArrayWithInt32))));
    Integer headerOfWider(m_out.load64(mutableInstanceField(Instance::offsetOfNewArrayHeader(wider.indexingType))));
    branchUnless(logicalAnd(equal(loadField<Word32>(array, m_heaps.JSCell_structureID), low32(headerForInt32)), isNotZero(headerOfWider)), isLeftToOthers);
    storeField(array, m_heaps.JSCell_structureID, low32(headerOfWider));
    m_out.store32As8(m_out.constInt32(wider.indexingType), array.value(), m_heaps.JSCell_indexingTypeAndMisc);
}

void ElementAccessEmitter::turnIntegersIntoFractions(CellPointer array, const ArrayElements& elements, LBasicBlock isLeftToOthers)
{
    const ArrayStorageForm& forFractions = arrayStorageFormOf(ArrayStorage::Double);
    widenStorageOfIntegers(array, forFractions, isLeftToOthers);
    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock done = newBlock();
    Integer count = vectorLength(elements);
    Number hole = rawHole(forFractions).as<Number>();
    ValueFromBlock start = m_out.anchor(integer(0).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer position(m_out.phi(Machine::Int64, start));
    branch(below(position, count), { normalEdge(element), normalEdge(done) });

    m_out.appendTo(element);
    Boxed word(m_out.load64(elementOfUnknownForm(elements, position)));
    storeRawElement(forFractions, elements, position, select(isZero(Integer(word.value())), hole, toNumber(unboxInt32(word))));
    m_out.addIncomingToPhi(position.value(), m_out.anchor(add(position, integer(1)).value()));
    m_out.jump(loop);

    m_out.appendTo(done);
}

Condition ElementAccessEmitter::tryStoreElement(const ValueContext& store, CellPointer array, Integer index, ValueID stored, bool leavesStorageAsItIs, std::optional<Pointer> profileOfSite)
{
    RELEASE_ASSERT(store->valueOperand() == stored);
    LBasicBlock otherwise = newBlock();
    Join<Condition> join(m_out);
    Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
    ArrayElements elements = elementsOf(array);
    forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, candidateFormsFor(stored, typeOf(stored).isInt32() ? ArrayStorage::Int32 : storageExpectedToHold(representationOf(stored))), otherwise, [&](const ArrayStorageForm& form) {
        Operand rawElement = *rawElementToStore(form, stored, otherwise);
        if (leavesStorageAsItIs) {
            LengthTest test = testLength(index, elements);
            branchUnless(test.isInside, otherwise);
            replaceElement(store, form, elements, test.indexInside, rawElement);
        } else
            storeInVector(store, form, elements, index, rawElement, otherwise);
        join.add(boolean(true));
    });

    m_out.appendTo(otherwise);
    if (leavesStorageAsItIs) {
        static_assert(ArrayAllocationProfile::countMask == 0xffffu << ArrayAllocationProfile::allocationCountShift && !(ArrayAllocationProfile::allocationCountShift % 8));
        if (profileOfSite)
            m_out.store32As16(m_out.int32Zero, m_out.address(m_heaps.AOTInstance_arrayProfiles, profileOfSite->value(), OBJECT_OFFSETOF(ArrayAllocationProfile, word) + ArrayAllocationProfile::allocationCountShift / 8));
        join.add(boolean(false));
        return join.finish();
    }
    LBasicBlock isLeftToOthers = newBlock();
    const ArrayStorageForm& forInt32 = arrayStorageFormOf(ArrayStorage::Int32);
    const ArrayStorageForm& forAnyValue = arrayStorageFormOf(ArrayStorage::Contiguous);
    Operand given = get(stored);
    bool mayBeNoNumber = !typeOf(stored).isNumber();
    if (mayBeNoNumber && (given.representation == Representation::JSValue || given.representation == Representation::Cell)) {
        branchUnless(hasWritableShape(mode, forInt32), isLeftToOthers);
        if (given.representation == Representation::JSValue) {
            Boxed boxed = given.as<Boxed>();
            LBasicBlock isNumberBlock = newBlock();
            LBasicBlock isAnyValue = newBlock();
            branch(isNumber(boxed), { normalEdge(isNumberBlock), normalEdge(isAnyValue) });

            m_out.appendTo(isNumberBlock);
            branchUnless(logicalNot(isInt32(boxed)), isLeftToOthers);
            Number fraction = unboxDouble(boxed);
            branchUnless(equal(fraction, fraction), isAnyValue);
            turnIntegersIntoFractions(array, elements, isLeftToOthers);
            storeInVector(store, arrayStorageFormOf(ArrayStorage::Double), elements, index, fraction, isLeftToOthers);
            join.add(boolean(true));

            m_out.appendTo(isAnyValue);
        }
        widenStorageOfIntegers(array, forAnyValue, isLeftToOthers);
        Operand rawElement = *rawElementToStore(forAnyValue, stored, isLeftToOthers);
        storeInVector(store, forAnyValue, elements, index, rawElement, isLeftToOthers);
        join.add(boolean(true));
    } else if (std::optional<Operand> fraction = mayBeNoNumber || typeOf(stored).isInt32() ? std::nullopt : rawElementToStore(arrayStorageFormOf(ArrayStorage::Double), stored, isLeftToOthers)) {
        branchUnless(hasWritableShape(mode, forInt32), isLeftToOthers);
        branchUnless(below(index, hasOnlyExpectedArmInLine(store) ? publicLength(elements) : vectorLength(elements)), isLeftToOthers);
        turnIntegersIntoFractions(array, elements, isLeftToOthers);
        storeInVector(store, arrayStorageFormOf(ArrayStorage::Double), elements, index, *fraction, isLeftToOthers);
        join.add(boolean(true));
    } else
        m_out.jump(isLeftToOthers);

    m_out.appendTo(isLeftToOthers);
    join.add(boolean(false));
    return join.finish();
}

void ElementAccessEmitter::generateAppendElements(const ValueContext& value)
{
    if (generator().m_literalLeftToSpreadRightBehind == value->arrayOperand()) [[unlikely]] {
        ArrayAllocationEmitter(generator()).bearLiteralHoldingList(value, in<CellPointer>(value->sourceOperand()).value(), appendElements());
        return;
    }
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

void ElementAccessEmitter::generateAppendValuesOfMap(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    callStub(value, appendValuesOfMap(), { array.value(), source.value() });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
