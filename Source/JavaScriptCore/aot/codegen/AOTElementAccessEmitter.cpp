/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElementAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinTypes.h"
#include "AOTCompileUnit.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTGuessedAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsArrays.h"
#include "AOTThrowEmitter.h"
#include "ArrayConventions.h"
#include "Butterfly.h"
#include "JSArray.h"
#include "JSCJSValueInlines.h"
#include "JSTypeInfo.h"
#include "Structure.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::all_of(arrayStorageForms, [](const ArrayStorageForm& form) {
    return form.element == Representation::Int32 || form.element == Representation::Double || form.element == Representation::JSValue;
}));

static JSValue constantIndexOf(Integer index)
{
    LValue value = index.value();
    if (!value->hasInt64() || value->asInt64() < 0 || value->asInt64() > INT32_MAX)
        return { };
    return jsNumber(static_cast<int32_t>(value->asInt64()));
}

template<typename Functor>
static void forBoxedAndDoubleShapes(ElementAccessEmitter& elements, Output& out, Word32 shape, LBasicBlock hasNoForm, const Functor& emitArm)
{
    const ArrayStorageForm& doubleForm = arrayStorageFormOf(ArrayStorage::Double);
    LBasicBlock holdsBoxed = out.newBlock();
    LBasicBlock holdsUnboxed = out.newBlock();
    LBasicBlock holdsDoubles = out.newBlock();
    out.branch(elements.holdsBoxedElements(shape).value(), normalEdge(holdsBoxed), normalEdge(holdsUnboxed));

    out.appendTo(holdsBoxed);
    emitArm(nullptr);

    out.appendTo(holdsUnboxed);
    out.branch(elements.hasShape(shape, doubleForm).value(), normalEdge(holdsDoubles), rareEdge(hasNoForm));

    out.appendTo(holdsDoubles);
    emitArm(&doubleForm);
}

template<typename Functor>
static void forDoubleAndInt32Storage(ElementAccessEmitter& elements, Output& out, CellPointer array, const Functor& emitArm)
{
    const ArrayStorageForm& doubleForm = arrayStorageFormOf(ArrayStorage::Double);
    LBasicBlock holdsDoubles = out.newBlock();
    LBasicBlock holdsInt32 = out.newBlock();
    out.branch(elements.hasShape(elements.indexingMode(array, arrayStorageTestMask), doubleForm).value(), normalEdge(holdsDoubles), normalEdge(holdsInt32));

    out.appendTo(holdsDoubles);
    emitArm(doubleForm);

    out.appendTo(holdsInt32);
    emitArm(arrayStorageFormOf(ArrayStorage::Int32));
}

Condition ElementAccessEmitter::isPristineArray(CellPointer array, OptionSet<PristineAspects> aspects)
{
    if (!aspects.containsAny({ PristineAspects::Names, PristineAspects::Storage }))
        return boolean(true);
    return hasBits(loadByte(array, m_heaps.JSCell_typeInfoFlags), JSC::IsPristine);
}

Condition ElementAccessEmitter::isPristineInstance(CellPointer cell, OptionSet<PristineAspects> aspects)
{
    Condition result;
    if (aspects.contains(PristineAspects::Names))
        result = hasBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), JSC::IsPristine);
    if (aspects.contains(PristineAspects::Storage)) {
        Word32 bitField = loadField<Word32>(loadStructure(cell), m_heaps.Structure_bitField);
        Condition hasPristineElements = hasBits(bitField, Structure::s_hasPristineElementsBits);
        result = result ? logicalAnd(result, hasPristineElements) : hasPristineElements;
    }
    return result ? result : boolean(true);
}

static constexpr IndexingType bitsThatOtherShapesHave(ArrayStorage kind)
{
    switch (kind) {
    case ArrayStorage::Contiguous:
        return UndecidedShape | Int32Shape;
    case ArrayStorage::Int32:
        return UndecidedShape | ContiguousShape;
    case ArrayStorage::Double:
    case ArrayStorage::Other:
        return 0;
    }
    return 0;
}

static_assert([] {
    for (ArrayStorage kind : { ArrayStorage::Contiguous, ArrayStorage::Int32 }) {
        for (IndexingType shape : { UndecidedShape, Int32Shape, DoubleShape, ContiguousShape, ArrayStorageShape, SlowPutArrayStorageShape }) {
            if (!(shape & bitsThatOtherShapesHave(kind)) != (shape == arrayStorageFormOf(kind).shape))
                return false;
        }
        if (bitsThatOtherShapesHave(kind) & ~IndexingShapeMask)
            return false;
    }
    return true;
}());

Condition ElementAccessEmitter::hasArrayStorage(CellPointer array, ArrayStorage kind, bool isWritable)
{
    if (isWritable)
        return hasWritableShape(indexingMode(array, IndexingShapeAndWritabilityMask), arrayStorageFormOf(kind));
    if (IndexingType otherShapes = bitsThatOtherShapesHave(kind))
        return hasNoBits(loadByte(array, m_heaps.JSArray_indexingTypeAndMisc), otherShapes);
    return hasShape(indexingMode(array, arrayStorageTestMask), arrayStorageFormOf(kind));
}

// The byte of an array with storage of its own of a plain kind has the bits of the kind and no other, so hasWritableShape() compares all of it. A lock bit that another thread holds for a moment sends the access the slow way.
Word32 ElementAccessEmitter::indexingMode(CellPointer array, IndexingType mask)
{
    Word32 byte = loadByte(array, m_heaps.JSArray_indexingTypeAndMisc);
    return mask == IndexingShapeAndWritabilityMask ? byte : bitAnd(byte, word32(mask));
}

static constexpr uint32_t bitsOutsideBoxedShapes = ~static_cast<uint32_t>(ContiguousShape - Int32Shape);

static_assert([] {
    for (uint32_t shape = 0; shape <= arrayStorageTestMask; ++shape) {
        bool passesTest = !((shape - Int32Shape) & bitsOutsideBoxedShapes);
        if (passesTest != (shape == Int32Shape || shape == ContiguousShape))
            return false;
    }
    return true;
}());
static_assert(!arrayStorageFormOf(ArrayStorage::Int32).hole && !arrayStorageFormOf(ArrayStorage::Contiguous).hole);

Condition ElementAccessEmitter::holdsBoxedElements(Word32 maskedMode)
{
    return hasNoBits(sub(maskedMode, word32(Int32Shape)), bitsOutsideBoxedShapes);
}

static constexpr uint32_t bitsOfShapeOutsideBoxedShapes = IndexingShapeMask & bitsOutsideBoxedShapes;

static_assert([] {
    for (uint32_t byte = 0; byte <= 0xff; ++byte) {
        uint32_t shape = byte & IndexingShapeMask;
        bool passesTest = !((byte - Int32Shape) & bitsOfShapeOutsideBoxedShapes);
        if (passesTest != (shape == Int32Shape || shape == ContiguousShape))
            return false;
    }
    return true;
}());

Condition ElementAccessEmitter::holdsBoxedElements(CellPointer array)
{
    return hasNoBits(sub(loadByte(array, m_heaps.JSArray_indexingTypeAndMisc), word32(Int32Shape)), bitsOfShapeOutsideBoxedShapes);
}

TypedPointer ElementAccessEmitter::elementAt(const ArrayStorageForm& form, const ArrayElements& elements, Integer index)
{
    const Machine::IndexedAbstractHeap* heap = m_heaps.forIndexingType(form.indexingType);
    RELEASE_ASSERT(heap);
    return m_out.baseIndex(*heap, elements.butterfly.value(), index.value(), constantIndexOf(index));
}

Operand ElementAccessEmitter::loadRawElement(const ArrayStorageForm& form, const ArrayElements& elements, Integer index)
{
    TypedPointer element = elementAt(form, elements, index);
    if (form.element == Representation::Double)
        return Number(m_out.loadDouble(element));
    Boxed rawElement(m_out.load64(element));
    if (form.holdsCells)
        ensureStillAliveHere(elements.array);
    return rawElement;
}

Boxed ElementAccessEmitter::loadBoxedElement(const ArrayElements& elements, Integer index)
{
    Boxed rawElement(m_out.load64(elementOfUnknownForm(elements, index)));
    ensureStillAliveHere(elements.array);
    return rawElement;
}

void ElementAccessEmitter::storeRawElement(const ArrayStorageForm& form, const ArrayElements& elements, Integer index, Operand rawElement)
{
    TypedPointer element = elementAt(form, elements, index);
    if (form.element == Representation::Double) {
        m_out.storeDouble(rawElement.as<Number>().value(), element);
        return;
    }
    m_out.store64(rawElement.as<Boxed>().value(), element);
}

Condition ElementAccessEmitter::isHole(const ArrayStorageForm& form, Operand rawElement)
{
    if (form.element == Representation::Double) {
        Number number = rawElement.as<Number>();
        return notEqualOrUnordered(number, number);
    }
    return isEmpty(rawElement.as<Boxed>());
}

Operand ElementAccessEmitter::rawHole(const ArrayStorageForm& form)
{
    if (form.element == Representation::Double)
        return number(std::bit_cast<double>(form.hole));
    return Boxed(m_out.constInt64(static_cast<int64_t>(form.hole)));
}

Boxed ElementAccessEmitter::boxRawElement(const ArrayStorageForm& form, Operand rawElement)
{
    if (form.element == Representation::Double)
        return emitter<ConversionEmitter>().boxNumber(rawElement.as<Number>());
    return rawElement.as<Boxed>();
}

static bool holdsInteger(Representation representation)
{
    return representation == Representation::Int64 || representation == Representation::OptionalInt64;
}

static bool holdsDouble(Representation representation)
{
    return representation == Representation::Double || representation == Representation::OptionalDouble;
}

static bool holdsUnboxedNumber(Representation representation)
{
    return representation == Representation::Int32 || holdsInteger(representation) || holdsDouble(representation);
}

Operand ElementAccessEmitter::convertInt32Element(Word32 element, Representation representation)
{
    if (holdsInteger(representation))
        return Operand(representation, signExtended(element).value());
    if (holdsDouble(representation))
        return Operand(representation, toNumber(element).value());
    RELEASE_ASSERT(representation == Representation::Int32);
    return element;
}

Operand ElementAccessEmitter::convertRawElement(const OperationContext& context, const ArrayStorageForm& form, Operand rawElement, Representation representation, TypeID result)
{
    if (form.element == Representation::Double)
        stopUnlessDoubleIsOf(context, rawElement, result);
    return convertElementThatHasBeenAsked(context, form, rawElement, representation, result);
}

Operand ElementAccessEmitter::convertElementThatHasBeenAsked(const OperationContext& context, const ArrayStorageForm& form, Operand rawElement, Representation representation, TypeID result)
{
    if (form.element == Representation::Double && holdsDouble(representation))
        return Operand(representation, rawElement.value);
    if (form.element == Representation::Int32 && holdsUnboxedNumber(representation))
        return convertInt32Element(unboxInt32Element(rawElement.as<Boxed>()), representation);
    Boxed element = boxRawElement(form, rawElement);
    if (form.element == Representation::JSValue && holdsUnboxedNumber(representation))
        stopUnlessElementIsOf(context, element, result);
    return emitter<ConversionEmitter>().unbox(element, types().at(result), representation);
}

void ElementAccessEmitter::stopUnlessDoubleIsOf(const OperationContext& context, Operand element, TypeID proven)
{
    TypeID numbers = types().intersectionOf(proven, TypeTable::number);
    if (numbers == TypeID(TypeTable::number))
        return;
    Condition isOfPartClaimed = isNeverType(numbers) ? boolean(false) : hasType(Operand(Representation::Double, element.value), typeTestPlanFor(TypeTable::number, Representation::Double, numbers));
    emitter<ThrowEmitter>().checkOrStopAtFalseProof(context, isOfPartClaimed, ProofSource::Contents);
}

void ElementAccessEmitter::stopUnlessElementIsOf(const OperationContext& context, Boxed element, TypeID proven)
{
    emitter<ThrowEmitter>().requireOrStopAtFalseProof(context, hasType(element, typeTestPlanFor(TypeTable::anything, Representation::JSValue, proven)), element, ProofSource::Contents);
}

static bool holdsUndefined(Representation representation)
{
    switch (representation) {
    case Representation::JSValue:
    case Representation::Void:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
        return true;
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::RawWord:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand ElementAccessEmitter::absentElement(const OperationContext& context, Representation representation)
{
    if (holdsUndefined(representation))
        return emitter<ConstantEmitter>().constantOf(jsUndefined(), representation);
    fail(context, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(representation);
}

std::optional<Integer> ElementAccessEmitter::indexBesideWholeNumber(ValueID position, Number number, LBasicBlock isNotAnIndex)
{
    const Operation& sum = operationAt(definitionOf(position));
    if (sum.opcode != Opcode::Add && sum.opcode != Opcode::Sub)
        return std::nullopt;
    if (!typeOf(sum.leftOperand()).isNumberWithoutFraction() || !typeOf(sum.rightOperand()).isNumberWithoutFraction())
        return std::nullopt;
    LValue value = number.value();
    if (value->opcode() != (sum.opcode == Opcode::Add ? Machine::Add : Machine::Sub) || value->type() != Machine::Double)
        return std::nullopt;
    LValue whole = value->child(0);
    LValue written = value->child(1);
    if (sum.opcode == Opcode::Add && whole->hasDouble())
        std::swap(whole, written);
    if (!written->hasDouble() || whole->hasDouble())
        return std::nullopt;
    double addend = written->asDouble();
    if (!(std::abs(addend) <= INT32_MAX) || addend != std::trunc(addend))
        return std::nullopt;

    Integer truncated = emitter<ConversionEmitter>().toInteger(Number(whole));
    if (isNotAnIndex && !isX86_64())
        branchUnless(equal(toNumber(truncated), Number(whole)), isNotAnIndex);
    int64_t step = static_cast<int64_t>(addend);
    return add(truncated, integer(sum.opcode == Opcode::Add ? step : -step));
}

Integer ElementAccessEmitter::indexOrElse(ValueID position, bool keyIsIntegral, LBasicBlock isNotAnIndex, NaNIndexMode nanMode)
{
    Operand operand = get(position);
    switch (operand.representation) {
    case Representation::Int32:
        return signExtended(operand.as<Word32>());
    case Representation::Int64:
        return operand.as<Integer>();
    case Representation::Double: {
        Number number = operand.as<Number>();
        if (std::optional<Integer> exact = emitter<ConversionEmitter>().exactIntegerBehind(number))
            return *exact;
        if (std::optional<Integer> beside = keyIsIntegral ? indexBesideWholeNumber(position, number, isNotAnIndex) : std::nullopt)
            return *beside;
        Integer index = emitter<ConversionEmitter>().toInteger(number);
        if (isNotAnIndex && (!keyIsIntegral || !isX86_64())) {
            bool truncatesNaNOutOfBounds = isX86_64() && nanMode == NaNIndexMode::ProduceOutOfBoundsIndex;
            branchUnless(truncatesNaNOutOfBounds ? equalOrUnordered(toNumber(index), number) : equal(toNumber(index), number), isNotAnIndex);
        }
        return index;
    }
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed ElementAccessEmitter::boxedPosition(ValueID position)
{
    Operand operand = get(position);
    switch (operand.representation) {
    case Representation::Int32:
        return boxInt32(operand.as<Word32>());
    case Representation::Int64:
        return emitter<ConversionEmitter>().boxInteger(operand.as<Integer>(), typeOf(position).integers);
    case Representation::Double:
        return emitter<ConversionEmitter>().boxNumber(operand);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ElementAccessEmitter::generateGetArrayLength(const ValueContext& value)
{
    define(value, arrayLength(in<CellPointer>(value->arrayOperand())));
}

void ElementAccessEmitter::generateGetArrayStorage(const ValueContext& value)
{
    define(value, elementsOf(in<CellPointer>(value->arrayOperand())).butterfly);
}

bool ElementAccessEmitter::testsEveryPosition()
{
    return readsStorageWhereItIsUsedUnder(build().options().testing);
}

void ElementAccessEmitter::stopUnlessInside(const ValueContext& access, Condition isInside)
{
    emitter<ThrowEmitter>().requireOrStopAtFalseProof(access, isInside, std::nullopt, ProofSource::WhatIsRemembered);
}

void ElementAccessEmitter::stopUnlessListIsAsMadeWhereNothingIsFound(const ValueContext& access, CellPointer list)
{
    if (!Operation::opcodesWithAsksCellWhereNothingIsFound.contains(access.opcode()) || !access->asksCellWhereNothingIsFound())
        return;
    emitter<ThrowEmitter>().requireOrStopAtFalseProof(access, isPristineArray(list, { PristineAspects::Storage, PristineAspects::Names }), std::nullopt, ProofSource::OtherOperation);
}

void ElementAccessEmitter::holdKindOfList(const ValueContext& access, StorageIsWritten isWritten)
{
    const ArrayStorageForm* form = findArrayStorageForm(access->arrayStorage());
    if (!form || !testsEveryPosition())
        return;
    CellPointer list = in<CellPointer>(access->arrayOperand());
    bool admitsInt32Storage = Operation::opcodesWithAdmitsInt32Storage.contains(access.opcode()) && access->admitsInt32Storage();
    Condition holds = admitsInt32Storage ? hasDoubleOrInt32Storage(list) : hasArrayStorage(list, form->kind, isWritten == StorageIsWritten::Yes);
    emitter<ThrowEmitter>().requireOrStopAtFalseProof(access, holds, std::nullopt, ProofSource::WhatIsRemembered);
}

void ElementAccessEmitter::holdPositionInsideList(const ValueContext& access)
{
    if (!testsEveryPosition()) {
        build().statistics().count(StatisticID::RangeChecksRemoved);
        return;
    }
    ArrayElements elements = elementsOf(in<CellPointer>(access->arrayOperand()));
    stopUnlessInside(access, testLength(indexInsideLength(access->positionOperand()), elements).isInside);
}

ArrayElements ElementAccessEmitter::elementsOf(const ValueContext& access, CellPointer array)
{
    std::optional<Pointer> storage = inOptional<Pointer>(access->storageOperand());
    if (!storage || testsEveryPosition())
        return elementsOf(array);
    return { array, *storage };
}

Integer ElementAccessEmitter::indexInsideLength(ValueID position)
{
    Operand operand = get(position);
    if (operand.representation == Representation::Int32)
        return zeroExtended(operand.as<Word32>());
    return indexOrElse(position, true, nullptr);
}

static_assert(MAX_STORAGE_VECTOR_LENGTH <= static_cast<unsigned>(std::numeric_limits<int32_t>::max()));

LengthTest ElementAccessEmitter::testLength(Integer index, const ArrayElements& elements)
{
    if (index.value()->opcode() != Machine::SExt32)
        return { below(index, publicLength(elements)), index };
    Word32 word(index.value()->child(0));
    return { below(word, loadField<Word32>(elements.butterfly, m_heaps.Butterfly_publicLength)), zeroExtended(word) };
}

static bool liesInsideLength(const ValueContext& access)
{
    if (access.opcode() == Opcode::IsElementPresent)
        return access->isInBounds();
    return access.opcode() == Opcode::GetArrayElement && (access->isInBounds() || access->isPresent());
}

LengthTest ElementAccessEmitter::testLength(const ValueContext& value, Integer index, const ArrayElements& elements)
{
    if (liesInsideLength(value))
        return { boolean(true), index.value()->opcode() == Machine::SExt32 ? zeroExtended(Word32(index.value()->child(0))) : index };
    if (!VariadicOperandPosition::length(value->opcode) || value->lengthOperands().empty() || testsEveryPosition())
        return testLength(index, elements);
    auto lengths = value->lengthOperands();
    Operand length = get(lengths[0]);
    bool isWord = length.representation == Representation::Int32;
    if (!isWord && length.representation != Representation::Int64)
        return testLength(index, elements);
    if (index.value()->opcode() != Machine::SExt32)
        return { below(index, isWord ? zeroExtended(length.as<Word32>()) : length.as<Integer>()), index };
    Word32 word(index.value()->child(0));
    return { below(word, isWord ? length.as<Word32>() : low32(length.as<Integer>())), zeroExtended(word) };
}

Operand ElementAccessEmitter::readElement(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, LBasicBlock isAbsent)
{
    LengthTest test = testLength(value, index, elements);
    if (!liesInsideLength(value))
        branchUnless(test.isInside, isAbsent);
    return readElementInsideLength(value, form, elements, test.indexInside, isAbsent);
}

static bool readsHoleAsNaN(const ValueContext& read)
{
    return read.opcode() == Opcode::GetArrayElement && read->readsHoleAsNaN();
}

bool ElementAccessEmitter::findsElement(const ValueContext& read)
{
    if (readsHoleAsNaN(read))
        return true;
    return read.opcode() == Opcode::GetArrayElement && read->isPresent();
}

Operand ElementAccessEmitter::readElementInsideLength(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, LBasicBlock isHole)
{
    RELEASE_ASSERT(!readsHoleAsNaN(value) || form.element == Representation::Double);
    Operand rawElement = loadRawElement(form, elements, index);
    if (!findsElement(value)) {
        LBasicBlock holdsValue = newBlock();
        m_out.branch(this->isHole(form, rawElement).value(), rareEdge(isHole), normalEdge(holdsValue));
        m_out.appendTo(holdsValue);
    }
    return convertRawElement(value, form, rawElement, value.representation(), typeIDOf(value.id()));
}

const PlacedSlot* ElementAccessEmitter::extraPropertiesWordOfReadByPosition(const ValueContext& read)
{
    Type receiver = typeOf(read->objectOperand());
    if (!receiver.isObject() || !receiver.layouts || !receiver.hasOnlyLayouts() || !typeOf(read->propertyOperand()).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull))
        return nullptr;
    std::span<const LayoutID> layouts = membersOf(types(), receiver);
    LayoutKind kind = layouts.size() == 1 ? m_program.at(layouts[0]).kind : LayoutKind::Environment;
    if ((kind != LayoutKind::Container && kind != LayoutKind::Record) || !m_plan.slots().holdsCompiledObjects(layouts[0]))
        return nullptr;
    const PlacedSlot& word = m_plan.slots().extraPropertiesWord(layouts[0]);
    return word.hasStorage() ? &word : nullptr;
}

Boxed ElementAccessEmitter::readElementOfExtraProperties(CellPointer object, const PlacedSlot& extraPropertiesWord, Integer index, LBasicBlock notFound)
{
    CellPointer holder = emitter<SlotEmitter>().extraPropertiesOf(object, extraPropertiesWord, notFound);
    branchUnless(holdsBoxedElements(indexingMode(holder, arrayStorageTestMask)), notFound);
    ArrayElements elements = elementsOf(holder);
    LengthTest test = testLength(index, elements);
    branchUnless(test.isInside, notFound);
    Boxed rawElement = loadBoxedElement(elements, test.indexInside);
    branchUnless(isNotEmpty(rawElement), notFound);
    return rawElement;
}

Boxed ElementAccessEmitter::readBoxedElement(const ValueContext& value, const ArrayElements& elements, Integer index, LBasicBlock isAbsent)
{
    LengthTest test = testLength(value, index, elements);
    if (!liesInsideLength(value))
        branchUnless(test.isInside, isAbsent);
    Boxed rawElement = loadBoxedElement(elements, test.indexInside);
    branchUnless(isNotEmpty(rawElement), isAbsent);
    return rawElement;
}

void ElementAccessEmitter::generateGetArrayElement(const ValueContext& value)
{
    if (value->isInBounds() || value->isPresent())
        holdPositionInsideList(value);
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateElementRead(value);
        return;
    }

    if (emitter<GuessedAccessEmitter>().tryReadNumberOfProvenList(value))
        return;
    const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage());
    RELEASE_ASSERT(!form || !value->asksCellWhereNothingIsFound());
    holdKindOfList(value);
    if (!form)
        readWithAnyStorage(value);
    else if (value->admitsInt32Storage())
        readFromDoubleOrInt32Storage(value);
    else if (value->isInBounds() || value->isPresent())
        readInsideLength(value, *form);
    else
        readWithKnownStorage(value, *form);
}

void ElementAccessEmitter::readInsideLength(const ValueContext& value, const ArrayStorageForm& form)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements elements = elementsOf(value, array);
    Integer index = indexInsideLength(value->positionOperand());
    if (findsElement(value)) {
        define(value, readNumberInsideLength(value, form, elements, index, nullptr));
        return;
    }

    LBasicBlock isHole = newBlock();
    OperandJoin join(m_out, value.representation());
    join.add(readElementInsideLength(value, form, elements, index, isHole));
    m_out.appendTo(isHole);
    join.add(absentElement(value, value.representation()));
    define(value, join.finish());
}

static bool isAlwaysArrayIndex(const Type& position)
{
    return position.isInteger() && position.integers.isSubsetOf({ 0, MAX_ARRAY_INDEX });
}

bool ElementAccessEmitter::hasOneArmInLine(const ValueContext& read)
{
    return holdsUndefined(read.representation());
}

void ElementAccessEmitter::readWithOneArmInLine(const ValueContext& value, const ArrayStorageForm* knownForm)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Representation representation = value.representation();
    const Type& result = typeOf(value.id());
    bool mayReadNumber = !!(result.primitives & Primitive::Number);
    const ArrayStorageForm* form = knownForm;
    if (!form && !mayReadNumber)
        form = &arrayStorageFormOf(ArrayStorage::Contiguous);
    LBasicBlock outOfLine = newBlock();
    OperandJoin join(m_out, representation);

    ArrayElements elements = knownForm ? elementsOf(value, array) : elementsOf(array);
    ScopeFork fork = m_out.forkScope();
    if (!knownForm)
        branchUnless(form ? hasArrayStorage(array, form->kind) : holdsBoxedElements(array), outOfLine);
    Integer index = indexOrElse(value->positionOperand(), value->keyIsIntegral(), outOfLine, NaNIndexMode::ProduceOutOfBoundsIndex);
    if (form)
        join.add(readElement(value, *form, elements, index, outOfLine));
    else
        join.add(conversions.unbox(readBoxedElement(value, elements, index, outOfLine), result, representation));

    m_out.appendToJoin(outOfLine, fork);
    Boxed answer = readOutOfLine(value, array);
    join.add(conversions.unbox(answer, result, representation));
    define(value, join.finishBehind(fork));
}

void ElementAccessEmitter::readWithKnownStorage(const ValueContext& value, const ArrayStorageForm& form)
{
    ValueID position = value->positionOperand();
    bool isAlwaysIndex = isAlwaysArrayIndex(typeOf(position));
    if (!isAlwaysIndex && hasOneArmInLine(value)) {
        readWithOneArmInLine(value, &form);
        return;
    }

    CellPointer array = in<CellPointer>(value->arrayOperand());
    Representation representation = value.representation();
    LBasicBlock isNotAnIndex = isAlwaysIndex ? nullptr : newBlock();
    LBasicBlock isAbsent = newBlock();
    OperandJoin join(m_out, representation);

    Integer index = indexOrElse(position, value->keyIsIntegral(), isNotAnIndex, NaNIndexMode::ProduceOutOfBoundsIndex);
    join.add(readElement(value, form, elementsOf(value, array), index, isAbsent));

    m_out.appendTo(isAbsent);
    if (isNotAnIndex)
        branchUnless(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), isNotAnIndex);
    join.add(absentElement(value, representation));

    if (isNotAnIndex) {
        m_out.appendTo(isNotAnIndex);
        join.add(emitter<ConversionEmitter>().unbox(readOutOfLine(value, array), typeOf(value.id()), representation));
    }
    define(value, join.finish());
}

void ElementAccessEmitter::readWithAnyStorage(const ValueContext& value)
{
    if (hasOneArmInLine(value)) {
        readWithOneArmInLine(value, nullptr);
        return;
    }

    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Representation representation = value.representation();
    const Type& result = typeOf(value.id());
    ValueID position = value->positionOperand();
    LBasicBlock isAbsent = newBlock();
    LBasicBlock outOfLine = newBlock();
    OperandJoin join(m_out, representation);

    Integer index = indexOrElse(position, value->keyIsIntegral(), outOfLine, NaNIndexMode::ProduceOutOfBoundsIndex);
    Word32 shape = indexingMode(array, arrayStorageTestMask);
    ArrayElements elements = elementsOf(array);
    bool mayReadNumber = !!(result.primitives & Primitive::Number);
    auto readWithForm = [&](const ArrayStorageForm* form) {
        if (!form)
            join.add(readBoxedElement(value, elements, index, isAbsent));
        else if (!mayReadNumber && form->kind != ArrayStorage::Contiguous)
            m_out.jump(isAbsent);
        else
            join.add(readElement(value, *form, elements, index, isAbsent));
    };
    if (representation == Representation::JSValue && mayReadNumber)
        forBoxedAndDoubleShapes(*this, m_out, shape, outOfLine, readWithForm);
    else if (representation == Representation::JSValue) {
        LBasicBlock holdsBoxed = newBlock();
        m_out.branch(holdsBoxedElements(shape).value(), normalEdge(holdsBoxed), rareEdge(outOfLine));
        m_out.appendTo(holdsBoxed);
        readWithForm(nullptr);
    } else {
        ArrayStorageFormList everyForm;
        for (const ArrayStorageForm& form : arrayStorageForms | std::views::reverse)
            everyForm.append(&form);
        forEachFormByShape(shape, &ElementAccessEmitter::hasShape, everyForm, outOfLine, [&](const ArrayStorageForm& form) {
            readWithForm(&form);
        }, FirstForm::IsLikeTheOthers);
    }

    m_out.appendTo(isAbsent);
    if (!isAlwaysArrayIndex(typeOf(position)))
        branchUnless(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), outOfLine);
    join.add(absentElement(value, representation));

    m_out.appendTo(outOfLine);
    join.add(conversions.unbox(readOutOfLine(value, array), result, representation));
    define(value, join.finish());
}

static_assert(Int32Shape + 2 == DoubleShape && !(arrayStorageTestMask & 1));

Condition ElementAccessEmitter::hasDoubleOrInt32Storage(CellPointer array)
{
    return below(sub(indexingMode(array, arrayStorageTestMask), word32(Int32Shape)), word32(DoubleShape - Int32Shape + 1));
}

Operand ElementAccessEmitter::readNumberInsideLength(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, LBasicBlock isHole)
{
    if (!readsHoleAsNaN(value) || form.element == Representation::Double)
        return readElementInsideLength(value, form, elements, index, isHole);

    RELEASE_ASSERT(form.element == Representation::Int32);
    Boxed rawElement = loadRawElement(form, elements, index).as<Boxed>();
    Number element = select(isEmpty(rawElement), number(PNaN), toNumber(unboxInt32Element(rawElement)));
    return convertElementThatHasBeenAsked(value, arrayStorageFormOf(ArrayStorage::Double), element, value.representation(), typeIDOf(value.id()));
}

void ElementAccessEmitter::readFromDoubleOrInt32Storage(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID position = value->positionOperand();
    Representation representation = value.representation();
    if (value->arrayStorage() != ArrayStorage::Double) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(representation));
        return;
    }

    bool isInsideLength = value->isInBounds() || value->isPresent();
    LBasicBlock isNotAnIndex = isInsideLength || isAlwaysArrayIndex(typeOf(position)) ? nullptr : newBlock();
    LBasicBlock isAbsent = isInsideLength && findsElement(value) ? nullptr : newBlock();
    OperandJoin join(m_out, representation);

    ArrayElements elements = elementsOf(value, array);
    Integer index = isInsideLength ? indexInsideLength(position) : indexOrElse(position, value->keyIsIntegral(), isNotAnIndex, NaNIndexMode::ProduceOutOfBoundsIndex);
    Integer indexInside = index;
    if (!isInsideLength) {
        LengthTest test = testLength(value, index, elements);
        branchUnless(test.isInside, isAbsent);
        indexInside = test.indexInside;
    }
    forDoubleAndInt32Storage(*this, m_out, array, [&](const ArrayStorageForm& form) {
        join.add(readNumberInsideLength(value, form, elements, indexInside, isAbsent));
    });

    if (isAbsent) {
        m_out.appendTo(isAbsent);
        if (isNotAnIndex)
            branchUnless(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), isNotAnIndex);
        join.add(absentElement(value, representation));
    }
    if (isNotAnIndex) {
        m_out.appendTo(isNotAnIndex);
        join.add(emitter<ConversionEmitter>().unbox(readOutOfLine(value, array), typeOf(value.id()), representation));
    }
    define(value, join.finish());
}

void ElementAccessEmitter::branchOnPresentElementInDoubleOrInt32Storage(const ValueContext& test, BranchTargets targets)
{
    CellPointer array = in<CellPointer>(test->arrayOperand());
    Integer index = indexOfPresenceTest(test, targets.notTaken.block());
    ArrayElements elements = elementsOf(test, array);
    forDoubleAndInt32Storage(*this, m_out, array, [&](const ArrayStorageForm& form) {
        branchOnElementPresence(test, &form, elements, index, targets);
    });
}

Boxed ElementAccessEmitter::readOutOfLine(const ValueContext& value, CellPointer array)
{
    ValueID position = value->positionOperand();
    if (value.opcode() != Opcode::GetArrayElement)
        return emitter<AccessEmitter>().getByVal(value, boxCell(array), boxedPosition(position));

    stopUnlessListIsAsMadeWhereNothingIsFound(value, array);
    if (Operand key = get(position); key.is<Number>())
        return Boxed(callStub(value, arrayGetAtNumber(), { array.value(), key.value }).first);
    return Boxed(callStub(value, arrayGetAtIndex(), { array.value(), integerOf(position).value() }).first);
}

Integer ElementAccessEmitter::indexOfPresenceTest(const ValueContext& test, LBasicBlock isNotAnIndex)
{
    ValueID position = test->positionOperand();
    if (!liesInsideLength(test))
        return indexOrElse(position, test->keyIsIntegral(), isNotAnIndex, NaNIndexMode::ProduceOutOfBoundsIndex);
    holdPositionInsideList(test);
    return indexInsideLength(position);
}

void ElementAccessEmitter::branchOnElementPresence(const ValueContext& value, const ArrayStorageForm* form, const ArrayElements& elements, Integer index, BranchTargets targets)
{
    LengthTest test = testLength(value, index, elements);
    if (!liesInsideLength(value)) {
        LBasicBlock isInBounds = newBlock();
        branch(test.isInside, { normalEdge(isInBounds), targets.notTaken });
        m_out.appendTo(isInBounds);
    }
    branch(form ? isHole(*form, loadRawElement(*form, elements, test.indexInside)) : isEmpty(loadBoxedElement(elements, test.indexInside)), targets.swapped());
}

void ElementAccessEmitter::branchOnPresentElement(const ValueContext& test, const ArrayStorageForm& form, BranchTargets targets)
{
    CellPointer array = in<CellPointer>(test->arrayOperand());
    Integer index = indexOfPresenceTest(test, targets.notTaken.block());
    branchOnElementPresence(test, &form, elementsOf(test, array), index, targets);
}

void ElementAccessEmitter::branchOnPresentElementInAnyStorage(const ValueContext& test, BranchTargets targets)
{
    CellPointer array = in<CellPointer>(test->arrayOperand());
    Integer index = indexOfPresenceTest(test, targets.notTaken.block());
    ArrayElements elements = elementsOf(array);
    forBoxedAndDoubleShapes(*this, m_out, indexingMode(array, arrayStorageTestMask), targets.notTaken.block(), [&](const ArrayStorageForm* form) {
        branchOnElementPresence(test, form, elements, index, targets);
    });
}

void ElementAccessEmitter::generateHasArrayElement(const ValueContext& value)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateElementTest(value);
        return;
    }

    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID position = value->positionOperand();
    const ArrayStorageForm* knownForm = findArrayStorageForm(value->arrayStorage());
    holdKindOfList(value);
    auto hasOutOfLine = [&] {
        return callOperation<Condition>(value, RuntimeOperation::InByValAndCache, { boxCell(array), boxedPosition(position) }, { .isOnRarePath = true });
    };

    defineTestByBranching(value, [&](BranchTargets targets) {
        bool isAlwaysIndex = isAlwaysArrayIndex(typeOf(position));
        LBasicBlock outOfLine = knownForm && isAlwaysIndex ? nullptr : newBlock();
        LBasicBlock isAbsent = isAlwaysIndex ? nullptr : newBlock();
        LBasicBlock asksCell = value->asksCellWhereNothingIsFound() ? newBlock() : nullptr;
        BlockEdge isFoundNowhere = asksCell ? BlockEdge(asksCell, targets.notTaken.edgeClass()) : targets.notTaken;
        BranchTargets presenceTargets { targets.taken, !isAbsent ? isFoundNowhere : normalEdge(isAbsent) };

        Integer index = indexOrElse(position, value->keyIsIntegral(), outOfLine);
        ArrayElements elements = elementsOf(array);
        auto branchOnPresence = [&](const ArrayStorageForm* form) {
            branchOnElementPresence(value, form, elements, index, presenceTargets);
        };
        if (knownForm)
            branchOnPresence(knownForm);
        else
            forBoxedAndDoubleShapes(*this, m_out, indexingMode(array, arrayStorageTestMask), outOfLine, branchOnPresence);

        if (isAbsent) {
            m_out.appendTo(isAbsent);
            branch(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), { isFoundNowhere, rareEdge(outOfLine) });
        }
        if (asksCell) {
            m_out.appendTo(asksCell);
            stopUnlessListIsAsMadeWhereNothingIsFound(value, array);
            m_out.jump(targets.notTaken);
        }
        if (outOfLine) {
            m_out.appendTo(outOfLine);
            branch(hasOutOfLine(), targets);
        }
    });
}

void ElementAccessEmitter::generateGetEnumeratorIndexedLength(const ValueContext& value)
{
    CellPointer enumerator = in<CellPointer>(value->enumeratorOperand());
    define(value, zeroExtended(loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_indexLength)));
}

void ElementAccessEmitter::generateIsElementInStorage(const ValueContext& value)
{
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock fails = targets.notTaken.block();
        Integer position = indexOrElse(value->positionOperand(), false, fails);
        Operand operand = get(value->objectOperand());
        if (!operand.is<CellPointer>())
            branchUnless(isCell(operand.as<Boxed>()), fails);
        CellPointer object = operand.is<CellPointer>() ? operand.as<CellPointer>() : unboxCell(operand.as<Boxed>());
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape && !(IndexingShapeMask & 1));
        Word32 shape = indexingMode(object, IndexingShapeMask);
        branchUnless(belowOrEqual(sub(shape, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), fails);
        ArrayElements elements = elementsOf(object);
        LengthTest test = testLength(position, elements);
        branchUnless(test.isInside, fails);
        Integer hole = select(equal(shape, word32(DoubleShape)), integer(arrayStorageFormOf(ArrayStorage::Double).hole), integer(arrayStorageFormOf(ArrayStorage::Contiguous).hole));
        branch(notEqual(Integer(loadBoxedElement(elements, test.indexInside).value()), hole), targets);
    });
}

void ElementAccessEmitter::generateGetStringLength(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    ValueID string = value->stringOperand();
    std::optional<CellPointer> constant = m_code.isConstant(string) ? std::optional<CellPointer>(in<CellPointer>(string)) : std::nullopt;
    define(value, constant ? strings.lengthOf(*constant) : strings.lengthOf(string));
}

void ElementAccessEmitter::generateGetStringCodeUnit(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    CharacterView view = strings.viewOf(value, value->stringOperand());
    Word32 position = strings.word32Of(value->positionOperand());
    if (testsEveryPosition())
        stopUnlessInside(value, below(position, view.length));
    define(value, strings.codeUnitAt(view, position));
}

void ElementAccessEmitter::generateReserveArrayRoom(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Number room = in<Number>(value->roomOperand());
    if (!generator().paysForCodeInLine(value.block()) && value->heldOperands().empty()) {
        callStub(value, reserveArrayRoom(), { array.value(), room.value() });
        return;
    }
    LBasicBlock done = newBlock();
    for (ValueID held : value->heldOperands())
        branchUnless(logicalNot(emitter<ConversionEmitter>().isAbsent(get(held))), done);
    if (generator().paysForCodeInLine(value.block()))
        branchUnless(greaterThan(room, toNumber(vectorLength(elementsOf(array)))), done);
    callStub(value, reserveArrayRoom(), { array.value(), room.value() });
    m_out.jump(done);

    m_out.appendTo(done);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
