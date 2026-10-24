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
#include "AOTCellFormat.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTRuntimeValidationEmitter.h"
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
    CheckBoxedInt32,
    Int32ToDouble,
    UseDouble,
    UnboxNumber,
};

} // anonymous namespace

static ElementConversion conversionTo(const ArrayStorageForm& form, Representation stored, const Type& type)
{
    bool isUnboxedDouble = stored == Representation::Double || stored == Representation::OptionalDouble;
    if (form.element == Representation::Int32) {
        if (stored == Representation::Int32)
            return ElementConversion::BoxInt32;
        if (stored == Representation::JSValue && type.primitives & Primitive::Integer)
            return ElementConversion::CheckBoxedInt32;
        // The representation follows the uses of a value, so an int32 that is mostly wanted as a number arrives as a double.
        if (isUnboxedDouble && type.isInt32())
            return ElementConversion::BoxDoubleAsInt32;
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
        if (conversionTo(form, representation, type) != ElementConversion::NeverFits)
            forms.appendIfNotContains(&form);
    };
    if (const ArrayStorageForm* form = findArrayStorageForm(expected))
        appendIfPossible(*form);
    for (const ArrayStorageForm& form : arrayStorageForms | std::views::reverse)
        appendIfPossible(form);
    return forms;
}

void ElementAccessEmitter::forEachFormByShape(Word32 mode, ShapeTest hasShapeOf, const ArrayStorageFormList& forms, LBasicBlock hasNoForm, const ScopedLambda<void(const ArrayStorageForm&)>& emitArm)
{
    for (const ArrayStorageForm* form : forms) {
        LBasicBlock hasForm = newBlock();
        LBasicBlock hasOtherForm = newBlock();
        m_out.branch((this->*hasShapeOf)(mode, *form).value(), normalEdge(hasForm), form == forms.last() ? rareEdge(hasOtherForm) : normalEdge(hasOtherForm));
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
        if (!canStoreWithoutConversion(form.kind, type))
            branchUnless(equal(number, number), doesNotFit);
        return number;
    };

    switch (conversionTo(form, operand.representation, type)) {
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
        return Operand(emitter<ConversionEmitter>().boxNumber(number));
    }
    case ElementConversion::BoxDoubleAsInt32:
        return Operand(boxInt32(emitter<ConversionEmitter>().toInt32(Number(operand.value))));
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
    // Outside fast code the stub behind a store also puts an element behind the last one.
    if (store.opcode() == Opcode::PutArrayElement && store.shape() != CodeShape::Fast) {
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
        m_out.store32(in<Word32>(store->valueOperand()).value(), elementAt(form, elements, index));
        return;
    }
    storeRawElement(form, elements, index, rawElement);
    if (form.holdsCells && typeOf(store->valueOperand()).hasCells()) {
        Boxed stored = rawElement.as<Boxed>();
        emitter<BarrierEmitter>().elementBarrierAfterStore(store, elements.array, singleElementSpan(stored));
    }
}

// The type of the value says that the storage takes it as it is, and the guard that the storage is the array's own.
void ElementAccessEmitter::storeInsideLength(const ValueContext& value, const ArrayStorageForm& form)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements elements = elementsOf(array);
    Integer index = indexInsideLength(value->positionOperand());
    if (generator().validatesOrChecksProofs()) {
        RuntimeValidationEmitter& validation = emitter<RuntimeValidationEmitter>();
        validation.checkPrecondition(value, hasWritableShape(indexingMode(array, IndexingShapeAndWritabilityMask), form));
        validation.checkPrecondition(value, below(index, publicLength(elements)));
    }
    ValueID stored = value->valueOperand();
    // A boxed integer may be boxed as a double, which storage of int32 does not hold.
    bool unboxesInt32 = form.element == Representation::Int32 && representationOf(stored) == Representation::JSValue;
    Operand rawElement = unboxesInt32
        ? Operand(boxInt32(emitter<ConversionEmitter>().unbox(in<Boxed>(stored), typeOf(stored), Representation::Int32).as<Word32>()))
        : *rawElementToStore(form, stored, nullptr);
    replaceElement(value, form, elements, index, rawElement);
}

void ElementAccessEmitter::generatePutArrayElement(const ValueContext& value)
{
    const ArrayStorageForm* knownForm = findArrayStorageForm(value->arrayStorage());
    if (knownForm && value->isInBounds()) {
        storeInsideLength(value, *knownForm);
        return;
    }

    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID stored = value->valueOperand();
    bool hasArmInLine = knownForm ? conversionTo(*knownForm, representationOf(stored), typeOf(stored)) != ElementConversion::NeverFits : value.shape() == CodeShape::Fast;
    if (!hasArmInLine) {
        storeOutOfLine(value, array);
        return;
    }

    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    Integer index = indexOrElse(value->positionOperand(), value->keyIsIntegral(), slowPath, NaNIndexMode::ProduceOutOfBoundsIndex);
    Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
    if (knownForm) {
        // The guard accepts copy-on-write storage, which only the operation may replace.
        branchUnless(hasWritableShape(mode, *knownForm), slowPath);
        Operand rawElement = *rawElementToStore(*knownForm, stored, slowPath);
        storeInVector(value, *knownForm, elementsOf(array), index, rawElement, slowPath);
        m_out.jump(continuation);
    } else {
        ArrayElements elements = elementsOf(array);
        // Only the storage that the stored value is expected to go into has an arm in line. The operations have a short way in front of them for the others.
        ArrayStorageFormList forms = candidateFormsFor(stored, storageExpectedToHold(representationOf(stored)));
        if (forms.size() > 1)
            forms.shrink(1);
        forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, forms, slowPath, [&](const ArrayStorageForm& form) {
            Operand rawElement = *rawElementToStore(form, stored, slowPath);
            storeInVector(value, form, elements, index, rawElement, slowPath);
            m_out.jump(continuation);
        });
    }

    m_out.appendTo(slowPath);
    storeOutOfLine(value, array);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void ElementAccessEmitter::storeOutOfLine(const ValueContext& value, CellPointer array)
{
    ValueID position = value->positionOperand();
    ValueID stored = value->valueOperand();
    const Type& storedType = typeOf(stored);
    bool isFast = value.shape() == CodeShape::Fast;
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Boxed boxedValue = isFast ? conversions.box(value, get(stored), storedType) : conversions.boxInArm(value, get(stored), storedType);
    if (value->mode() == ECMAMode::Sloppy) {
        emitter<AccessEmitter>().putByVal(value, boxCell(array), boxedPosition(position), boxedValue, AccessEmitter::putModeOf(ECMAMode::Sloppy));
        return;
    }

    bool mayConvertToDouble = storedType.isNumberLike() && !storedType.isInt32();
    Operand key = get(position);
    // The stubs take the position as it is, store wherever the vector has room, and keep the registers of the code around the arm.
    ArrayStoredValue storedValue = mayConvertToDouble ? ArrayStoredValue::Number : ArrayStoredValue::Value;
    if (key.is<Number>()) {
        callStub(value, arrayPutAtNumber(storedValue), { array.value(), boxedValue.value(), key.value });
        return;
    }
    callStub(value, arrayPutAtIndex(storedValue), { array.value(), boxedValue.value(), integerOf(position).value() });
}

void ElementAccessEmitter::appendElement(const ValueContext& store, CellPointer array, ValueID element)
{
    if (store.shape() != CodeShape::Fast) {
        appendOutOfLine(store, array, element);
        return;
    }

    ArrayStorage expected = ArrayAllocationEmitter(generator()).formAtAllocation(typeOf(store->arrayOperand())).kind;
    ArrayStorage declared = arrayStorageDeclaredBy(m_program, typeOf(store->arrayOperand()));
    ArrayStorageFormList forms = candidateFormsFor(element, expected);
    // An array of any other kind than its element type declares takes the stub, which knows them all.
    if (hasArrayStorageForm(declared) && forms.size() > 1 && forms.first()->kind == declared)
        forms.shrink(1);
    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
    ArrayElements elements = elementsOf(array);
    forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, forms, slowPath, [&](const ArrayStorageForm& form) {
        Operand rawElement = *rawElementToStore(form, element, slowPath);
        Integer length = publicLength(elements);
        branchUnless(below(length, vectorLength(elements)), slowPath);

        setPublicLength(elements, add(length, integer(1)));
        storeRawElement(form, elements, length, rawElement);
        if (form.holdsCells && typeOf(element).hasCells()) {
            Boxed stored = rawElement.as<Boxed>();
            emitter<BarrierEmitter>().elementBarrierAfterStore(store, elements.array, singleElementSpan(stored));
        }
        m_out.jump(continuation);
    });

    m_out.appendTo(slowPath);
    appendOutOfLine(store, array, element);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

Boxed ElementAccessEmitter::appendOutOfLine(const OperationContext& store, CellPointer array, ValueID element)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Operand operand = get(element);
    if (operand.is<Number>() && typeOf(element).isInt32())
        operand = Operand(boxInt32(conversions.toInt32(operand.as<Number>())));
    if (operand.is<Number>())
        return Boxed(callStub(store, arrayPushDouble(), { array.value(), conversions.bitsOf(operand).value() }).first);
    return Boxed(callStub(store, arrayPush(), { array.value(), conversions.box(store, operand, typeOf(element)).value() }).first);
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

void ElementAccessEmitter::generateHasSpareArrayCapacity(const ValueContext& value)
{
    ArrayElements elements = elementsOf(in<CellPointer>(value->arrayOperand()));
    defineTest(value, below(publicLength(elements), vectorLength(elements)));
}

void ElementAccessEmitter::generateReserveArrayCapacity(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ReserveArrayCapacity, { in<CellPointer>(value->arrayOperand()), integerOf(value->additionalOperand()) });
}

// An array of integers, or one that has held nothing yet, is converted: any other array that comes by pays one compare and no call.
void ElementAccessEmitter::generateConvertToDoubleStorage(const ValueContext& value)
{
    static_assert(UndecidedShape + 2 == Int32Shape && !(IndexingShapeMask & 1));
    CellPointer array = in<CellPointer>(value->arrayOperand());
    LBasicBlock converts = newBlock();
    LBasicBlock continuation = newBlock();
    Word32 shape = indexingMode(array, IndexingShapeMask);
    const ClassPart* declared = typeOf(value->arrayOperand()).singleBuiltinClass();
    // What a hole reads as is among the elements of an array that may have holes.
    if (declared && m_program.at(declared->contentsAt(m_program.builtins(), ContentsSlot::Element)).withoutPrimitives(Primitive::Undefined).isNumber()) {
        LBasicBlock isOfOtherKind = newBlock();
        LBasicBlock stillConverts = newBlock();
        LBasicBlock holdsAnyValue = newBlock();
        LBasicBlock liesInFrame = newBlock();
        LBasicBlock isOnHeap = newBlock();
        LBasicBlock mayHoldNumbers = newBlock();
        LBasicBlock isYoung = newBlock();
        LBasicBlock isShort = newBlock();
        branch(belowOrEqual(sub(shape, word32(UndecidedShape)), word32(Int32Shape - UndecidedShape)), { rareEdge(converts), normalEdge(isOfOtherKind) });

        m_out.appendTo(isOfOtherKind);
        uint32_t numberOfPlace = (m_function.position + value.id().index()) % ArrayWords::numberOfCounts;
        TypedPointer count = mutableInstanceField(AOT_INSTANCE_OFFSET(arrays.shortYoungArraysConverted) + numberOfPlace);
        branch(below(Word32(m_out.load8ZeroExt32(count)), word32(ArrayWords::maxShortYoungArraysConverted)), { normalEdge(stillConverts), normalEdge(continuation) });

        m_out.appendTo(stillConverts);
        branch(equal(shape, word32(ContiguousShape)), { normalEdge(holdsAnyValue), normalEdge(continuation) });

        m_out.appendTo(holdsAnyValue);
        Word32 state(m_out.load8ZeroExt32(m_out.address(m_heaps.collectorState, array.value(), JSCell::cellStateOffset())));
        branch(equal(state, word32(frameCellState)), { normalEdge(liesInFrame), normalEdge(isOnHeap) });

        // No array is shorter lived.
        m_out.appendTo(liesInFrame);
        m_out.store32As8(word32(ArrayWords::maxShortYoungArraysConverted).value(), count);
        m_out.jump(continuation);

        m_out.appendTo(isOnHeap);
        branch(below(loadMutable32(AOT_INSTANCE_OFFSET(arrays.elementsLookedAtInVain)), word32(ArrayWords::maxElementsLookedAtInVain)), { normalEdge(mayHoldNumbers), normalEdge(continuation) });

        m_out.appendTo(mayHoldNumbers);
        branch(equal(state, word32(static_cast<int32_t>(CellState::PossiblyBlack))), { rareEdge(converts), normalEdge(isYoung) });

        m_out.appendTo(isYoung);
        branch(below(publicLength(elementsOf(array)), integer(ArrayWords::minLengthThatPaysAtOnce)), { normalEdge(isShort), rareEdge(converts) });

        m_out.appendTo(isShort);
        m_out.store32(word32(static_cast<int32_t>(numberOfPlace)).value(), mutableInstanceField(AOT_INSTANCE_OFFSET(arrays.placeThatConverts)));
        m_out.jump(converts);
    } else
        branch(belowOrEqual(sub(shape, word32(UndecidedShape)), word32(Int32Shape - UndecidedShape)), { rareEdge(converts), normalEdge(continuation) });

    m_out.appendTo(converts);
    callVoidOperation(value, RuntimeOperation::ConvertToDoubleStorage, { array }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void ElementAccessEmitter::generateAppendElements(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    callStub(value, appendElements(), { array.value(), source.value() });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
