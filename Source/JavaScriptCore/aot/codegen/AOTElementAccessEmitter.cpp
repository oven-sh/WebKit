/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElementAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinTypes.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTOperatorEmitter.h"
#include "AOTRuntimeValidation.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsArrays.h"
#include "ArrayConventions.h"
#include "Butterfly.h"
#include "JSArray.h"
#include "JSCJSValueInlines.h"
#include "JSTypeInfo.h"
#include "Structure.h"
#include <algorithm>
#include <bit>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::all_of(arrayStorageForms, [](const ArrayStorageForm& form) {
    return form.element == Representation::Int32 || form.element == Representation::Double || form.element == Representation::JSValue;
}));

void ElementAccessEmitter::validateStorage(const OperationContext& operation, CellPointer array, ArrayStorage kind)
{
    if (!generator().validatesOrChecksProofs())
        return;
    LBasicBlock isInvalid = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(hasArrayStorage(array, kind).value(), normalEdge(continuation), rareEdge(isInvalid));

    m_out.appendTo(isInvalid);
    callVoidOperation(operation, RuntimeOperation::ReportValidationFailure, { boxCell(array), word32(static_cast<int32_t>(ValidationFailureKind::ValueType)), word32(m_id.index()), word32(operation.valueID().index()) }, { .isOnRarePath = true });
    m_out.unreachable();

    m_out.appendTo(continuation);
}

static JSValue constantIndexOf(Integer index)
{
    LValue value = index.value();
    if (!value->hasInt64() || value->asInt64() < 0 || value->asInt64() > INT32_MAX)
        return { };
    return jsNumber(static_cast<int32_t>(value->asInt64()));
}

// The functor ends the block it is called in. It gets no form for storage that holdsBoxedElements().
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

// Structure::setPristineBits(): the bit of the cell says both of an array.
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

// An array has storage for elements from its creation: Array.prototype, which has none, passes no test for an array (AOTTypeTestPlan.cpp).
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
        return hasNoBits(loadByte(array, m_heaps.JSCell_indexingTypeAndMisc), otherShapes);
    return hasShape(indexingMode(array, arrayStorageTestMask), arrayStorageFormOf(kind));
}

// The byte of an array with storage of its own of a plain kind has the bits of the kind and no other, so hasWritableShape() compares all of it. A lock bit that another thread holds for a moment sends the access the slow way.
// So only the yes of that test is a fact. A checking build, which has no slow way, asks hasWritableShapeIgnoringLock().
Word32 ElementAccessEmitter::indexingMode(CellPointer array, IndexingType mask)
{
    Word32 byte = loadByte(array, m_heaps.JSCell_indexingTypeAndMisc);
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

TypedPointer ElementAccessEmitter::elementAt(const ArrayStorageForm& form, const ArrayElements& elements, Integer index)
{
    B3::IndexedAbstractHeap* heap = m_heaps.forIndexingType(form.indexingType);
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

// Of storage that holdsBoxedElements(). The load names the elements of every form: one that named contiguous storage could move across a store into storage of int32.
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

bool ElementAccessEmitter::isDense(ValueID array)
{
    return pristineAspectsProvenBy(typeOf(array), BuiltinClasses::Array).contains(PristineAspects::Dense);
}

// Of an element below the length.
Condition ElementAccessEmitter::isHoleIn(ValueID array, const ArrayStorageForm& form, Operand rawElement)
{
    if (isDense(array) && !generator().validatesOrChecksProofs())
        return boolean(false);
    return isHole(form, rawElement);
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

Operand ElementAccessEmitter::convertRawElement(const ArrayStorageForm& form, Operand rawElement, Representation representation, const Type& result)
{
    if (form.element == Representation::Double && holdsDouble(representation))
        return Operand(representation, rawElement.value);
    if (form.element == Representation::Int32 && holdsUnboxedNumber(representation))
        return convertInt32Element(unboxInt32(rawElement.as<Boxed>()), representation);
    return emitter<ConversionEmitter>().unbox(boxRawElement(form, rawElement), result, representation);
}

Operand ElementAccessEmitter::absentElement(const OperationContext& context, Representation representation)
{
    switch (representation) {
    case Representation::JSValue:
    case Representation::Void:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::LiteralString:
        return emitter<ConstantEmitter>().constantOf(jsUndefined(), representation);
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::Cell:
    case Representation::RawWord:
        break;
    }
    fail(context, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(representation);
}

Integer ElementAccessEmitter::indexOrElse(ValueID position, bool keyIsIntegral, LBasicBlock isNotAnIndex, NaNIndexMode nanMode)
{
    Operand operand = get(position);
    switch (operand.representation) {
    case Representation::Int32:
        return signExtended(operand.as<Word32>());
    case Representation::Int64:
        return operand.as<Integer>();
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::Double: {
        Number number = operand.as<Number>();
        if (std::optional<Integer> whole = emitter<OperatorEmitter>().wholeRemainderAsIndex(position, isNotAnIndex))
            return *whole;
        if (std::optional<Integer> exact = emitter<ConversionEmitter>().exactIntegerBehind(number))
            return *exact;
        Integer index = emitter<ConversionEmitter>().toInteger(number);
        // x86-64 truncates NaN and every number outside int64 to INT64_MIN, which is above every length when compared unsigned.
        if (!keyIsIntegral || !isX86_64()) {
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
    case Representation::LiteralString:
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
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
        return emitter<ConversionEmitter>().boxNumber(getInFormOfItsNaN(position));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ElementAccessEmitter::generateGetArrayLength(const ValueContext& value)
{
    define(value, arrayLength(in<CellPointer>(value->arrayOperand())));
}

// The guard has found the position to be an index.
Integer ElementAccessEmitter::indexInsideLength(ValueID position)
{
    Operand operand = get(position);
    if (operand.representation == Representation::Int32)
        return zeroExtended(operand.as<Word32>());
    if (operand.representation != Representation::Double)
        return indexOrElse(position, true, nullptr);
    if (std::optional<Integer> whole = emitter<OperatorEmitter>().wholeRemainderAsIndex(position, nullptr))
        return *whole;
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    if (std::optional<Integer> exact = conversions.exactIntegerBehind(operand.as<Number>()))
        return *exact;
    return conversions.toInteger(operand.as<Number>());
}

static_assert(MAX_STORAGE_VECTOR_LENGTH <= static_cast<unsigned>(std::numeric_limits<int32_t>::max()));

// No such storage has 2^31 elements, so a negative int32 is above the length as a word too.
LengthTest ElementAccessEmitter::testLength(Integer index, const ArrayElements& elements)
{
    if (index.value()->opcode() != B3::SExt32)
        return { below(index, publicLength(elements)), index };
    Word32 word(index.value()->child(0));
    return { below(word, loadField<Word32>(elements.butterfly, m_heaps.Butterfly_publicLength)), zeroExtended(word) };
}

Operand ElementAccessEmitter::readElement(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, LBasicBlock isAbsent)
{
    LengthTest test = testLength(index, elements);
    branchUnless(test.isInside, isAbsent);
    return readElementInsideLength(value, form, elements, test.indexInside, isAbsent);
}

static bool readsHoleAsNaN(const ValueContext& read)
{
    return read.opcode() == Opcode::GetArrayElement && read->readsHoleAsNaN();
}

// Of a position inside the length. The hole of storage of doubles is a NaN, so a read that takes a hole for NaN has its result either way.
bool ElementAccessEmitter::findsElement(const ValueContext& read)
{
    if (isDense(read->arrayOperand()) || readsHoleAsNaN(read))
        return true;
    return read.opcode() == Opcode::GetArrayElement && read->isPresent();
}

Operand ElementAccessEmitter::readElementInsideLength(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, LBasicBlock isHole)
{
    RELEASE_ASSERT(!readsHoleAsNaN(value) || form.element == Representation::Double);
    // The low half of a boxed int32 is the integer. A presence test has loaded the whole element, and B3 shares only a load of the same width.
    if (isDense(value->arrayOperand()) && form.element == Representation::Int32 && holdsUnboxedNumber(value.representation()) && !generator().validatesOrChecksProofs())
        return convertInt32Element(Word32(m_out.load32(elementAt(form, elements, index))), value.representation());

    Operand rawElement = loadRawElement(form, elements, index);
    if (!findsElement(value)) {
        LBasicBlock holdsValue = newBlock();
        m_out.branch(this->isHole(form, rawElement).value(), rareEdge(isHole), normalEdge(holdsValue));
        m_out.appendTo(holdsValue);
    } else if (generator().validatesOrChecksProofs() && !readsHoleAsNaN(value))
        emitter<RuntimeValidationEmitter>().checkPrecondition(value, logicalNot(this->isHole(form, rawElement)));
    return convertRawElement(form, rawElement, value.representation(), typeOf(value.id()));
}

Boxed ElementAccessEmitter::readBoxedElement(const ValueContext& value, const ArrayElements& elements, Integer index, LBasicBlock isAbsent)
{
    LengthTest test = testLength(index, elements);
    branchUnless(test.isInside, isAbsent);
    Boxed rawElement = loadBoxedElement(elements, test.indexInside);
    if (!isDense(value->arrayOperand()))
        branchUnless(isNotEmpty(rawElement), isAbsent);
    else if (generator().validatesOrChecksProofs())
        emitter<RuntimeValidationEmitter>().checkPrecondition(value, isNotEmpty(rawElement));
    return rawElement;
}

void ElementAccessEmitter::generateGetArrayElement(const ValueContext& value)
{
    const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage());
    if (!form)
        readWithAnyStorage(value);
    else if (value->isInBounds() || value->isPresent())
        readInsideLength(value, *form);
    else
        readWithKnownStorage(value, *form);
}

void ElementAccessEmitter::readInsideLength(const ValueContext& value, const ArrayStorageForm& form)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    validateStorage(value, array, form.kind);
    ArrayElements elements = elementsOf(array);
    Integer index = indexInsideLength(value->positionOperand());
    if (generator().validatesOrChecksProofs()) {
        RuntimeValidationEmitter& validation = emitter<RuntimeValidationEmitter>();
        if (Operand position = get(value->positionOperand()); position.is<Number>())
            validation.checkPrecondition(value, equal(toNumber(index), position.as<Number>()));
        validation.checkPrecondition(value, below(index, publicLength(elements)));
    }
    if (findsElement(value)) {
        define(value, readElementInsideLength(value, form, elements, index, nullptr));
        return;
    }

    LBasicBlock isHole = newBlock();
    OperandJoin join(m_out, value.representation());
    join.add(readElementInsideLength(value, form, elements, index, isHole));
    m_out.appendTo(isHole);
    join.add(absentElement(value, value.representation()));
    define(value, join.finish());
}

void ElementAccessEmitter::readWithKnownStorage(const ValueContext& value, const ArrayStorageForm& form)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID position = value->positionOperand();
    Representation representation = value.representation();
    LBasicBlock isNotAnIndex = isArrayIndex(typeOf(position)) ? nullptr : newBlock();
    // The arm for whatever the code in line does not know is there anyway. Outside fast code it is the only one: the stub answers undefined in a few instructions.
    LBasicBlock isAbsent = isNotAnIndex && value.shape() != CodeShape::Fast ? isNotAnIndex : newBlock();
    OperandJoin join(m_out, representation);

    validateStorage(value, array, form.kind);
    Integer index = indexOrElse(position, value->keyIsIntegral(), isNotAnIndex, NaNIndexMode::ProduceOutOfBoundsIndex);
    join.add(readElement(value, form, elementsOf(array), index, isAbsent));

    if (isAbsent != isNotAnIndex) {
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

void ElementAccessEmitter::readWithAnyStorage(const ValueContext& value)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Representation representation = value.representation();
    const Type& result = typeOf(value.id());
    // The test of the shape, the two tests of the element and the arm take 70 bytes and more. Outside fast code they are the stub's.
    if (value.shape() != CodeShape::Fast) {
        define(value, conversions.unbox(readOutOfLine(value, array), result, representation));
        return;
    }

    ValueID position = value->positionOperand();
    LBasicBlock isAbsent = newBlock();
    LBasicBlock outOfLine = newBlock();
    OperandJoin join(m_out, representation);

    Integer index = indexOrElse(position, value->keyIsIntegral(), outOfLine, NaNIndexMode::ProduceOutOfBoundsIndex);
    Word32 shape = indexingMode(array, arrayStorageTestMask);
    ArrayElements elements = elementsOf(array);
    // Storage of numbers holds nothing that a result without numbers admits: there every position is a hole or behind the end.
    bool mayReadNumber = !!(result.primitives & Primitive::Number);
    auto readWithForm = [&](const ArrayStorageForm* form) {
        if (!form)
            join.add(readBoxedElement(value, elements, index, isAbsent));
        else if (!mayReadNumber && form->kind != ArrayStorage::Contiguous)
            m_out.jump(isAbsent);
        else
            join.add(readElement(value, *form, elements, index, isAbsent));
    };
    // The arm for doubles takes 30 bytes more. The library's loops are there once for all their callers and walk arrays of every kind; a program's own read of storage that nobody knows seldom meets doubles.
    if (representation == Representation::JSValue && mayReadNumber && isLibraryCode(m_program, m_function))
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
        });
    }

    m_out.appendTo(isAbsent);
    if (!isArrayIndex(typeOf(position)))
        branchUnless(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), outOfLine);
    join.add(absentElement(value, representation));

    m_out.appendTo(outOfLine);
    join.add(conversions.unbox(readOutOfLine(value, array), result, representation));
    define(value, join.finish());
}

// The stubs for arrays take the position as it is, so nothing is boxed here, and keep the registers of the code around the arm.
Boxed ElementAccessEmitter::readOutOfLine(const ValueContext& value, CellPointer array)
{
    ValueID position = value->positionOperand();
    if (value.opcode() != Opcode::GetArrayElement)
        return emitter<AccessEmitter>().getByVal(value, boxCell(array), boxedPosition(position));

    emitter<AccessEmitter>().recordArrival(value, boxCell(array));
    if (Operand key = get(position); key.is<Number>())
        return Boxed(callStub(value, arrayGetAtNumber(), { array.value(), key.value }).first);
    return Boxed(callStub(value, arrayGetAtIndex(), { array.value(), integerOf(position).value() }).first);
}

void ElementAccessEmitter::branchOnElementPresence(const ValueContext& value, const ArrayStorageForm* form, const ArrayElements& elements, Integer index, BranchTargets targets)
{
    LengthTest test = testLength(index, elements);
    if (isDense(value->arrayOperand()) && !generator().validatesOrChecksProofs()) {
        branch(test.isInside, targets);
        return;
    }

    LBasicBlock isInBounds = newBlock();
    branch(test.isInside, { normalEdge(isInBounds), targets.notTaken });

    m_out.appendTo(isInBounds);
    branch(form ? isHole(*form, loadRawElement(*form, elements, test.indexInside)) : isEmpty(loadBoxedElement(elements, test.indexInside)), targets.swapped());
}

void ElementAccessEmitter::branchOnPresentElement(const ValueContext& test, const ArrayStorageForm& form, BranchTargets targets)
{
    CellPointer array = in<CellPointer>(test->arrayOperand());
    validateStorage(test, array, form.kind);
    Integer index = indexOrElse(test->positionOperand(), test->keyIsIntegral(), targets.notTaken.block(), NaNIndexMode::ProduceOutOfBoundsIndex);
    branchOnElementPresence(test, &form, elementsOf(array), index, targets);
}

void ElementAccessEmitter::generateHasArrayElement(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID position = value->positionOperand();
    const ArrayStorageForm* knownForm = findArrayStorageForm(value->arrayStorage());
    bool isFast = value.shape() == CodeShape::Fast;
    auto hasOutOfLine = [&] {
        emitter<AccessEmitter>().recordArrival(value, boxCell(array));
        if (Operand key = get(position); key.is<Number>())
            return Condition(m_out.castToInt32(callStub(value, arrayHasAtNumber(), { array.value(), key.value }).first));
        return Condition(m_out.castToInt32(callStub(value, arrayHasAtIndex(), { array.value(), integerOf(position).value() }).first));
    };
    if (!knownForm && !isFast) {
        defineTest(value, hasOutOfLine());
        return;
    }

    if (knownForm)
        validateStorage(value, array, knownForm->kind);

    // A read of the element on the taken edge repeats these two branches, and B3 folds a branch that an equal one dominates.
    defineTestByBranching(value, [&](BranchTargets targets) {
        bool isAlwaysIndex = isArrayIndex(typeOf(position));
        LBasicBlock outOfLine = knownForm && isAlwaysIndex ? nullptr : newBlock();
        // Outside fast code the one arm also says whether a position where nothing was found is an index.
        LBasicBlock isAbsent = isAlwaysIndex ? nullptr : isFast ? newBlock() : outOfLine;
        BranchTargets presenceTargets { targets.taken, !isAbsent ? targets.notTaken : isAbsent == outOfLine ? rareEdge(isAbsent) : normalEdge(isAbsent) };

        Integer index = indexOrElse(position, value->keyIsIntegral(), outOfLine);
        ArrayElements elements = elementsOf(array);
        auto branchOnPresence = [&](const ArrayStorageForm* form) {
            branchOnElementPresence(value, form, elements, index, presenceTargets);
        };
        if (knownForm)
            branchOnPresence(knownForm);
        else
            forBoxedAndDoubleShapes(*this, m_out, indexingMode(array, arrayStorageTestMask), outOfLine, branchOnPresence);

        if (isAbsent && isAbsent != outOfLine) {
            m_out.appendTo(isAbsent);
            branch(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), { targets.notTaken, rareEdge(outOfLine) });
        }
        if (outOfLine) {
            m_out.appendTo(outOfLine);
            branch(hasOutOfLine(), targets);
        }
    });
}

void ElementAccessEmitter::generateGetEnumeratorIndexedLength(const ValueContext& value)
{
    define(value, zeroExtended(loadField<Word32>(in<CellPointer>(value->enumeratorOperand()), m_heaps.JSPropertyNameEnumerator_indexLength)));
}

// An own element in storage of one of the three forms. Whatever else may answer for the position fails the test.
void ElementAccessEmitter::generateIsElementInStorage(const ValueContext& value)
{
    defineTestByBranching(value, [&](BranchTargets targets) {
        Integer position = indexOrElse(value->positionOperand(), false, targets.notTaken.block());
        Operand operand = get(value->objectOperand());
        CellPointer object = operand.is<CellPointer>() ? operand.as<CellPointer>() : unboxCell(operand.as<Boxed>());
        if (!operand.is<CellPointer>())
            branchUnless(isCell(operand.as<Boxed>()), targets.notTaken.block());
        // One arm for the three forms: an element is eight bytes in each, and only what marks a hole differs.
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape && !(IndexingShapeMask & 1));
        Word32 shape = indexingMode(object, IndexingShapeMask);
        branchUnless(belowOrEqual(sub(shape, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), targets.notTaken.block());
        ArrayElements elements = elementsOf(object);
        LengthTest test = testLength(position, elements);
        branchUnless(test.isInside, targets.notTaken.block());
        Integer hole = select(equal(shape, word32(DoubleShape)), integer(arrayStorageFormOf(ArrayStorage::Double).hole), integer(arrayStorageFormOf(ArrayStorage::Contiguous).hole));
        branch(notEqual(Integer(loadBoxedElement(elements, test.indexInside).value()), hole), targets);
    });
}

// The position is an index of the enumerator, whatever the type says of it in a copy of the loop that has no proofs.
void ElementAccessEmitter::generateToEnumeratorPosition(const ValueContext& value)
{
    Operand position = get(value->positionOperand());
    Integer whole = position.is<Word32>() ? signExtended(position.as<Word32>()) : position.is<Integer>() ? position.as<Integer>() : emitter<ConversionEmitter>().toInteger(position.as<Number>());
    define(value, zeroExtended(low32(whole)));
}

void ElementAccessEmitter::generateGetStringLength(const ValueContext& value)
{
    define(value, emitter<StringEmitter>().lengthOf(value->stringOperand()));
}

void ElementAccessEmitter::generateGetStringCodeUnit(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    CharacterView view = strings.viewOf(value, value->stringOperand());
    Word32 position = strings.word32Of(value->positionOperand());
    if (generator().validatesOrChecksProofs())
        emitter<RuntimeValidationEmitter>().checkPrecondition(value, below(position, view.length));
    define(value, strings.codeUnitAt(view, position));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
