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
#include "AOTRuntimeCallEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsArrays.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestEmitter.h"
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

static constexpr std::array shapesOfArrays { UndecidedShape, Int32Shape, DoubleShape, ContiguousShape, ArrayStorageShape, SlowPutArrayStorageShape };

static_assert(std::ranges::all_of(shapesOfArrays, [](IndexingType shape) {
    return !(shape & UndecidedShape) == (shape == Int32Shape || shape == ContiguousShape);
}));
static_assert(!arrayStorageFormOf(ArrayStorage::Int32).hole && !arrayStorageFormOf(ArrayStorage::Contiguous).hole);

Condition ElementAccessEmitter::holdsBoxedElements(Word32 maskedMode)
{
    return hasNoBits(maskedMode, UndecidedShape);
}

Condition ElementAccessEmitter::holdsBoxedElements(CellPointer array)
{
    return hasNoBits(loadByte(array, m_heaps.JSArray_indexingTypeAndMisc), UndecidedShape);
}

static constexpr uint32_t bitsOfShapeOutsideBoxedShapes = IndexingShapeMask & ~static_cast<uint32_t>(ContiguousShape - Int32Shape);

static_assert([] {
    for (uint32_t byte = 0; byte <= 0xff; ++byte) {
        uint32_t shape = byte & IndexingShapeMask;
        bool passesTest = !((byte - Int32Shape) & bitsOfShapeOutsideBoxedShapes);
        if (passesTest != (shape == Int32Shape || shape == ContiguousShape))
            return false;
    }
    return true;
}());

static constexpr uint32_t bitsOfByteOutsideWritableBoxedShapes = 0xff & ~(IsArray | Int32Shape | ContiguousShape);

static_assert(std::ranges::all_of(shapesOfArrays, [](IndexingType shape) {
    for (uint32_t otherBits = 0; otherBits <= 0xff; ++otherBits) {
        uint32_t byte = IsArray | shape | (otherBits & ~IndexingTypeMask);
        if (!(byte & bitsOfByteOutsideWritableBoxedShapes) != (byte == (IsArray | Int32Shape) || byte == (IsArray | ContiguousShape)))
            return false;
    }
    return true;
}));

Condition ElementAccessEmitter::holdsWritableBoxedElements(CellPointer array)
{
    return hasNoBits(indexingMode(array, IndexingShapeAndWritabilityMask), bitsOfByteOutsideWritableBoxedShapes);
}

static constexpr uint32_t bitsOutsideDoubleAndContiguousShapes = ~static_cast<uint32_t>(ContiguousShape - DoubleShape);
static constexpr uint32_t bitsOutsidePlainShapes = ~static_cast<uint32_t>((DoubleShape - Int32Shape) | (ContiguousShape - Int32Shape));

Condition ElementAccessEmitter::takesNumbersByKindOfStorage(CellPointer array, bool admitsInt32Storage)
{
    Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
    if (!admitsInt32Storage)
        return hasNoBits(sub(mode, word32(IsArray | DoubleShape)), bitsOutsideDoubleAndContiguousShapes);
    return logicalAnd(hasNoBits(sub(mode, word32(IsArray | Int32Shape)), bitsOutsidePlainShapes), notEqual(mode, word32(IsArray | ArrayStorageShape)));
}

static_assert([] {
    for (uint32_t byte = 0; byte <= 0xff; ++byte) {
        bool holdsDoublesOrValues = byte == (IsArray | DoubleShape) || byte == (IsArray | ContiguousShape);
        if (!((byte - (IsArray | DoubleShape)) & bitsOutsideDoubleAndContiguousShapes) != holdsDoublesOrValues)
            return false;
        bool passesWiderTest = !((byte - (IsArray | Int32Shape)) & bitsOutsidePlainShapes) && byte != (IsArray | ArrayStorageShape);
        if (passesWiderTest != (holdsDoublesOrValues || byte == (IsArray | Int32Shape)))
            return false;
    }
    return true;
}());

static constexpr uint32_t bitsOutsideNumericShapes = ~static_cast<uint32_t>(DoubleShape - Int32Shape);

Condition ElementAccessEmitter::hasWritableNumericStorage(CellPointer array)
{
    return hasNoBits(sub(indexingMode(array, IndexingShapeAndWritabilityMask), word32(IsArray | Int32Shape)), bitsOutsideNumericShapes);
}

static_assert([] {
    for (uint32_t byte = 0; byte <= 0xff; ++byte) {
        bool passesTest = !((byte - (IsArray | Int32Shape)) & bitsOutsideNumericShapes);
        if (passesTest != (byte == (IsArray | Int32Shape) || byte == (IsArray | DoubleShape)))
            return false;
    }
    return true;
}());

Condition ElementAccessEmitter::objectHoldsBoxedElements(CellPointer object)
{
    return hasNoBits(sub(loadByte(object, m_heaps.JSArray_indexingTypeAndMisc), word32(Int32Shape)), bitsOfShapeOutsideBoxedShapes);
}

Boxed ElementAccessEmitter::loadArrayStorageElement(CellPointer object, Integer index, LBasicBlock notFound)
{
    static_assert(ArrayStorageShape > DoubleShape && ArrayStorageShape > ContiguousShape && SlowPutArrayStorageShape > ArrayStorageShape);
    branchUnless(belowOrEqual(word32(ArrayStorageShape), indexingMode(object, IndexingShapeMask)), notFound);
    Pointer storage = elementsOf(object).butterfly;
    branchUnless(below(index, zeroExtended(loadField<Word32>(storage, m_heaps.ArrayStorage_vectorLength))), notFound);
    Boxed element(m_out.load64(m_out.baseIndex(m_heaps.ArrayStorage_vector, storage.value(), index.value())));
    branchUnless(isNotEmpty(element), notFound);
    return element;
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
    if (form.element == Representation::Double) {
        stopUnlessDoubleIsOf(context, rawElement, result);
        if ((holdsInteger(representation) || representation == Representation::Int32) && types().at(types().intersectionOf(result, TypeTable::number)).isInteger()) {
            Integer integer = emitter<ConversionEmitter>().truncateUnchecked(rawElement.as<Number>());
            if (representation == Representation::Int32)
                return low32(integer);
            return Operand(representation, integer.value());
        }
    }
    return convertElementThatHasBeenAsked(context, form, rawElement, representation, result);
}

Operand ElementAccessEmitter::convertElementThatHasBeenAsked(const OperationContext& context, const ArrayStorageForm& form, Operand rawElement, Representation representation, TypeID result)
{
    if (form.element == Representation::Double && holdsDouble(representation))
        return Operand(representation, rawElement.value);
    if (form.element == Representation::Int32 && holdsUnboxedNumber(representation))
        return convertInt32Element(unboxInt32Element(rawElement.as<Boxed>()), representation);
    Boxed element = boxRawElement(form, rawElement);
    if (form.element == Representation::JSValue && holdsUnboxedNumber(representation)) {
        if (std::optional<Operand> number = tryUnboxNumberOrCrash(context, element, representation, result))
            return *number;
        stopUnlessElementIsOf(context, element, result);
    }
    return emitter<ConversionEmitter>().unbox(element, types().at(result), representation);
}

std::optional<Operand> ElementAccessEmitter::tryUnboxNumberOrCrash(const OperationContext& context, Boxed element, Representation representation, TypeID result)
{
    bool mayBeUndefined = representation == Representation::OptionalDouble && types().at(result).primitives & Primitive::Undefined;
    TypeID numbers = mayBeUndefined ? types().withoutPrimitives(result, Primitive::Undefined) : result;
    if (!holdsDouble(representation) || numbers != TypeID(TypeTable::number))
        return std::nullopt;
    LBasicBlock isInt32Element = newBlock();
    LBasicBlock isOther = newBlock();
    OperandJoin join(m_out, representation);
    m_out.branch(isInt32(element).value(), normalEdge(isInt32Element), normalEdge(isOther));
    m_out.appendTo(isInt32Element);
    join.add(Operand(representation, toNumber(unboxInt32(element)).value()));
    m_out.appendTo(isOther);
    if (mayBeUndefined) {
        LBasicBlock isDouble = newBlock();
        LBasicBlock isNoNumber = newBlock();
        m_out.branch(isNumber(element).value(), normalEdge(isDouble), unlikelyEdge(isNoNumber));
        m_out.appendTo(isNoNumber);
        emitter<ThrowEmitter>().requireOrCrash(context, equal(element, undefined()), element, ProofSource::Contents);
        join.add(emitter<ConversionEmitter>().unbox(element, types().at(TypeTable::undefined), representation));
        m_out.appendTo(isDouble);
    } else
        emitter<ThrowEmitter>().requireOrCrash(context, isNumber(element), element, ProofSource::Contents);
    join.add(Operand(representation, unboxDouble(element).value()));
    return join.finish();
}

bool ElementAccessEmitter::isTruncatedAlone(ValueID number) const
{
    static constexpr OpcodeSet truncations { Opcode::ToInt32, Opcode::ToUint32 };
    static constexpr OpcodeSet bitOperators { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::URShift };
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    static constexpr unsigned mostSums = 6;
    for (unsigned sum = 0; sum <= mostSums; ++sum) {
        ValueID user = m_code.useCountOf(number) == 1 ? m_code.onlyUserOf(number) : ValueID();
        if (!user)
            return false;
        const Operation& operation = m_code.at(user);
        if (truncations.contains(operation.opcode))
            return true;
        bool takesNumbers = (bitOperators.contains(operation.opcode) || sums.contains(operation.opcode)) && operation.form() == OperatorForm::Numbers;
        if (!takesNumbers || bitOperators.contains(operation.opcode))
            return takesNumbers;
        if (operation.representation != Representation::Double)
            return false;
        number = user;
    }
    return false;
}

void ElementAccessEmitter::stopUnlessDoubleIsOf(const OperationContext& context, Operand element, TypeID proven)
{
    TypeID numbers = types().intersectionOf(proven, TypeTable::number);
    if (numbers == TypeID(TypeTable::number))
        return;
    if (context.valueID() && !emittersCheckProofs() && m_code.at(context.valueID()).representation == Representation::Double && isTruncatedAlone(context.valueID()))
        return;
    RELEASE_ASSERT(!context.valueID() || !static_cast<const ValueContext&>(context).value().traits().isRemovableWhenUnused());
    if (Type claimed = types().at(numbers); claimed.isInteger() && claimed.integers != Range::int32()) {
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Number number(element.value);
        Integer truncated = conversions.truncateUnchecked(number);
        emitter<ThrowEmitter>().checkOrCrash(context, equal(conversions.bitsOf(toNumber(truncated)), conversions.bitsOf(number)), ProofSource::Contents);
        emitter<ThrowEmitter>().checkOrCrash(context, emitter<TypeTestEmitter>().isInRange(truncated, claimed.integers), ProofSource::Contents);
        return;
    }
    auto isOfPartClaimed = [&] {
        return isNeverType(numbers) ? boolean(false) : hasType(Operand(Representation::Double, element.value), typeTestPlanFor(TypeTable::number, Representation::Double, numbers));
    };
    Type claimed = types().at(numbers);
    if (!claimed.isNumberWithoutFraction() || !(claimed.primitives & Primitive::Integer) || !claimed.integers.isSubsetOf(Range::exact()) || claimed.integers == Range::exact()) {
        emitter<ThrowEmitter>().checkOrCrash(context, isOfPartClaimed(), ProofSource::Contents);
        return;
    }
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Number number(element.value);
    LBasicBlock isInteger = newBlock();
    LBasicBlock isOther = newBlock();
    LBasicBlock isOfPart = newBlock();
    Integer truncated = conversions.truncateUnchecked(number);
    Condition isExact = equal(conversions.bitsOf(toNumber(truncated)), conversions.bitsOf(number));
    ScopeFork fork = m_out.forkScope();
    branch(isExact, { normalEdge(isInteger), rareEdge(isOther) });
    m_out.appendToArm(isInteger, fork);
    branch(emitter<TypeTestEmitter>().isInRange(truncated, claimed.integers), { normalEdge(isOfPart), rareEdge(isOther) });
    m_out.appendToJoin(isOther, fork);
    emitter<ThrowEmitter>().checkOrCrash(context, isOfPartClaimed(), ProofSource::Contents);
    m_out.jump(isOfPart);
    m_out.appendToJoin(isOfPart, fork);
}

bool ElementAccessEmitter::boxIsMadeInFormOfType(ValueID id) const
{
    while (opcodeOf(id) == Opcode::Narrowed)
        id = operationAt(id).valueOperand();
    return opcodeOf(id) == Opcode::Box || opcodeOf(id) == Opcode::Integer;
}

void ElementAccessEmitter::storeWordOfOperand(const OperationContext& context, ValueID stored, Boxed word, const ArrayElements& elements, Integer index)
{
    if (typeOf(stored).isInt32() && !boxIsMadeInFormOfType(stored) && get(stored).value == word.value()) [[unlikely]]
        generator().fail(context, InternalErrorKind::Int32StoredWithoutReboxing, makeString('v', stored.index(), " ("_s, nameOf(opcodeOf(stored)), ')'));
    m_out.store64(word.value(), elementOfUnknownForm(elements, index));
}

void ElementAccessEmitter::stopUnlessElementIsOf(const OperationContext& context, Boxed element, TypeID proven)
{
    RELEASE_ASSERT(!context.valueID() || !static_cast<const ValueContext&>(context).value().traits().isRemovableWhenUnused());
    emitter<ThrowEmitter>().requireOrCrash(context, hasType(element, typeTestPlanFor(TypeTable::anything, Representation::JSValue, proven)), element, ProofSource::Contents);
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

    Integer truncated = emitter<ConversionEmitter>().truncateUnchecked(Number(whole));
    if (isNotAnIndex && !isX86_64())
        branchUnless(equal(toNumber(truncated), Number(whole)), isNotAnIndex);
    int64_t step = static_cast<int64_t>(addend);
    return add(truncated, integer(sum.opcode == Opcode::Add ? step : -step));
}

std::optional<Boxed> ElementAccessEmitter::wordOfPosition(ValueID position)
{
    if (std::optional<Boxed> word = generator().findBoxed(position))
        return word;
    const Operation& unboxed = operationAt(definitionOf(position));
    if (unboxed.opcode != Opcode::Unbox || unboxed.representation != Representation::Double || unboxed.block() != generator().blockBeingGenerated() || generator().keepsValuesInHeapFrame())
        return std::nullopt;
    if (representationOf(unboxed.valueOperand()) != Representation::JSValue || !(typeOf(position).primitives & Primitive::Integer) || generator().isCarriedBoxed(unboxed.valueOperand()))
        return std::nullopt;
    return in<Boxed>(unboxed.valueOperand());
}

Integer ElementAccessEmitter::wordToIndexOrElse(Boxed word, bool keyIsIntegral, LBasicBlock isNotAnIndex, NaNIndexMode nanMode)
{
    return either<Integer>(isInt32(word), [&] {
        return signExtended(unboxInt32(word));
    }, [&] {
        Number number = unboxDouble(word);
        Integer index = emitter<ConversionEmitter>().truncateUnchecked(number);
        if (isNotAnIndex && (!keyIsIntegral || !isX86_64()))
            branchUnless(isX86_64() && nanMode == NaNIndexMode::ProduceOutOfBoundsIndex ? equalOrUnordered(toNumber(index), number) : equal(toNumber(index), number), isNotAnIndex);
        return index;
    });
}

Integer ElementAccessEmitter::indexOrElse(ValueID position, bool keyIsIntegral, LBasicBlock isNotAnIndex, NaNIndexMode nanMode)
{
    if (std::optional<Boxed> word = wordOfPosition(position))
        return wordToIndexOrElse(*word, keyIsIntegral, isNotAnIndex, nanMode);
    if (LValue word = representationOf(position) == Representation::Double ? generator().wordOf(position) : nullptr; word && emitter<ConversionEmitter>().isHeldAsIndex(word)) {
        Integer index = *emitter<ConversionEmitter>().indexBehind(Number(word));
        if (isNotAnIndex && nanMode == NaNIndexMode::TakeSlowPath)
            branchUnless(greaterThanOrEqual(index, integer(0)), isNotAnIndex);
        return index;
    }
    Operand operand = get(position);
    switch (operand.representation) {
    case Representation::Int32:
        return signExtended(operand.as<Word32>());
    case Representation::Int64:
        return operand.as<Integer>();
    case Representation::Double:
    case Representation::OptionalDouble: {
        Number number(operand.value);
        if (std::optional<Integer> exact = emitter<ConversionEmitter>().exactIntegerBehind(number))
            return *exact;
        if (std::optional<Integer> index = emitter<ConversionEmitter>().indexBehind(number)) {
            if (isNotAnIndex && nanMode == NaNIndexMode::TakeSlowPath)
                branchUnless(greaterThanOrEqual(*index, integer(0)), isNotAnIndex);
            return *index;
        }
        if (std::optional<Integer> beside = keyIsIntegral ? indexBesideWholeNumber(position, number, isNotAnIndex) : std::nullopt)
            return *beside;
        Integer index = emitter<ConversionEmitter>().truncateUnchecked(number);
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
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Integer ElementAccessEmitter::indexOfAccessOrElse(const ValueContext& access, LBasicBlock isNotAnIndex, NaNIndexMode nanMode)
{
    ValueID position = access->positionOperand();
    if (access->keyIsIntegral() && checksBounds() && representationOf(position) == Representation::Double) {
        std::optional<Boxed> word = generator().findBoxed(position);
        Number number = word ? emitter<ConversionEmitter>().unbox(*word, typeOf(position), Representation::Double).as<Number>() : in<Number>(position);
        emitter<ThrowEmitter>().requireOrCrash(access, equalOrUnordered(trunc(number), number), std::nullopt, ProofSource::OtherOperation);
    }
    return indexOrElse(position, access->keyIsIntegral(), isNotAnIndex, nanMode);
}

Boxed ElementAccessEmitter::boxedPosition(ValueID position)
{
    if (std::optional<Boxed> word = wordOfPosition(position))
        return *word;
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

void ElementAccessEmitter::generateGetArrayCapacity(const ValueContext& value)
{
    define(value, vectorLength(elementsOf(in<CellPointer>(value->arrayOperand()))));
}

void ElementAccessEmitter::generateExtendArrayLength(const ValueContext& value)
{
    ArrayElements elements = elementsOf(value, in<CellPointer>(value->arrayOperand()));
    Integer position = indexInsideLength(value->positionOperand());
    if (checksBounds()) {
        stopUnlessInside(value, equal(position, publicLength(elements)));
        stopUnlessInside(value, below(position, vectorLength(elements)));
    }
    setPublicLength(elements, add(position, integer(1)));
}

void ElementAccessEmitter::generateRestoreArrayStore(const ValueContext& value)
{
    const ArrayStorageForm& form = arrayStorageFormOf(value->arrayStorage());
    ArrayElements elements = elementsOf(in<CellPointer>(value->arrayOperand()));
    Integer position = indexInsideLength(value->positionOperand());
    Integer length = integerOf(value->lengthOperand());
    Number held = in<Number>(value->valueOperand());
    LBasicBlock isAppended = newBlock();
    LBasicBlock done = newBlock();
    branchUnless(below(position, length), isAppended);
    if (form.kind == ArrayStorage::Double)
        storeRawElement(form, elements, position, held);
    else
        storeRawElement(form, elements, position, boxInt32(low32(emitter<ConversionEmitter>().truncateUnchecked(held))));
    m_out.jump(done);

    m_out.appendTo(isAppended);
    storeRawElement(form, elements, position, rawHole(form));
    setPublicLength(elements, position);
    emitter<RuntimeCallEmitter>().takeBackCountOfExtension();
    m_out.jump(done);

    m_out.appendTo(done);
}

void ElementAccessEmitter::generateSetArrayLength(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer length = integerOf(value->lengthOperand());
    StubKey stub = setArrayLength(value->mode() == ECMAMode::Sloppy);
    build().statistics().count(StatisticID::LengthsStoredIntoProvenLists);
    if (!generator().takesOpenForm(value, StateOfKnowledge::Always)) {
        callStub(value, stub, { array.value(), length.value() });
        return;
    }
    using Asked = ListWhoseLengthIsStoredWithoutRunTime;
    LBasicBlock otherwise = newBlock();
    LBasicBlock done = newBlock();
    branchUnless(belowOrEqual(sub(indexingMode(array, IndexingShapeAndWritabilityMask), Asked::firstKind), Asked::lastKind - Asked::firstKind), otherwise);
    branchUnless(isZero(bitAnd(loadField<Word32>(loadStructure(array), m_heaps.Structure_bitField), word32(Asked::bitsThatItsStructureMayNotHave))), otherwise);
    branchUnless(equal(publicLength(elementsOf(array)), length), otherwise);
    m_out.jump(done);

    m_out.appendTo(otherwise);
    callStub(value, stub, { array.value(), length.value() }, { .isOnRarePath = true });
    m_out.jump(done);

    m_out.appendTo(done);
}

bool ElementAccessEmitter::checksBounds()
{
    return checksBoundsUnder(build().options().testing) || AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckBoundsInEmittersOnly);
}

std::optional<ArrayStorage> ElementAccessEmitter::kindOfListFoundInFrontOfTurns(const ValueContext& access, ValueID list, bool isStoredInto)
{
    std::optional<ArrayStorage> found = generator().kindOfListFoundInFrontOfTurnsLaidOut(list);
    if (found && checksBounds()) {
        CellPointer asked = askedAnew(in<CellPointer>(list));
        if (!isStoredInto)
            stopUnlessInside(access, hasArrayStorage(asked, *found));
        else if (*found == ArrayStorage::Double)
            stopUnlessInside(access, hasWritableShape(indexingMode(asked, IndexingShapeAndWritabilityMask), arrayStorageFormOf(ArrayStorage::Double)));
        else
            stopUnlessInside(access, holdsWritableBoxedElements(asked));
        build().statistics().count(StatisticID::UnrolledLoopTypeProofsChecked);
    }
    return found;
}

std::optional<TypedArrayType> ElementAccessEmitter::kindOfViewFoundInFrontOfTurns(const ValueContext& access, ValueID view)
{
    std::optional<TypedArrayType> found = generator().kindOfViewFoundInFrontOfTurnsLaidOut(view);
    if (found && checksBounds()) {
        stopUnlessInside(access, equal(loadByte(askedAnew(in<CellPointer>(view)), m_heaps.JSCell_typeInfoType), static_cast<int32_t>(typeForTypedArrayType(*found))));
        build().statistics().count(StatisticID::UnrolledLoopTypeProofsChecked);
    }
    return found;
}

void ElementAccessEmitter::stopUnlessInside(const ValueContext& access, Condition isInside)
{
    emitter<ThrowEmitter>().requireOrCrash(access, isInside, std::nullopt, ProofSource::RememberedFact);
}

void ElementAccessEmitter::checkElementPresenceProof(const ValueContext& read, Condition isThere, StatisticID counted)
{
    if (!checksBounds())
        return;
    stopUnlessInside(read, isThere);
    build().statistics().count(counted);
}

void ElementAccessEmitter::stopWhereHoleWouldBeStored(const OperationContext& store, const ArrayStorageForm& form, Operand rawElement)
{
    if (emittersCheckProofs())
        emitter<ThrowEmitter>().requireOrCrash(store, logicalNot(isHole(form, rawElement)), std::nullopt, ProofSource::OtherOperation);
}

void ElementAccessEmitter::stopUnlessListIsAsMadeWhereNothingIsFound(const ValueContext& access, CellPointer list)
{
    if (!Operation::opcodesWithAsksCellWhereNothingIsFound.contains(access.opcode()) || !access->asksCellWhereNothingIsFound())
        return;
    if (access.opcode() == Opcode::GetArrayElement && access->mayHaveNumberNames())
        return;
    emitter<ThrowEmitter>().requireOrCrash(access, isPristineArray(list, { PristineAspects::Storage, PristineAspects::Names }), std::nullopt, ProofSource::OtherOperation);
}

void ElementAccessEmitter::holdKindOfList(const ValueContext& access, StorageIsWritten isWritten)
{
    const ArrayStorageForm* form = findArrayStorageForm(access->arrayStorage());
    if (!form || !checksBounds())
        return;
    CellPointer list = askedAnew(in<CellPointer>(access->arrayOperand()));
    bool admitsInt32Storage = Operation::opcodesWithAdmitsInt32Storage.contains(access.opcode()) && access->admitsInt32Storage();
    Condition holds = admitsInt32Storage ? form->kind == ArrayStorage::Contiguous ? holdsBoxedElements(list) : hasDoubleOrInt32Storage(list) : hasArrayStorage(list, form->kind, isWritten == StorageIsWritten::Yes);
    emitter<ThrowEmitter>().requireOrCrash(access, holds, std::nullopt, ProofSource::RememberedFact);
}

void ElementAccessEmitter::holdPositionInsideList(const ValueContext& access)
{
    if (!checksBounds()) {
        build().statistics().count(StatisticID::RangeChecksRemoved);
        return;
    }
    if (Operand position = get(access->positionOperand()); position.is<Number>())
        stopUnlessInside(access, equal(position.as<Number>(), position.as<Number>()));
    ArrayElements elements = elementsOf(askedAnew(in<CellPointer>(access->arrayOperand())));
    stopUnlessInside(access, testLength(indexInsideLength(access->positionOperand()), elements).isInside);
}

ArrayElements ElementAccessEmitter::elementsOf(const ValueContext& access, CellPointer array)
{
    std::optional<Pointer> storage = inOptional<Pointer>(access->storageOperand());
    if (!storage)
        return elementsOf(array);
    if (AOT::hasFlag(build().options().injectedFaults, Wire::FaultInjection::MisplaceHoistedArrayStorage))
        storage = offsetBy(*storage, sizeof(EncodedJSValue));
    if (checksBounds())
        stopUnlessInside(access, equal(*storage, elementsOf(askedAnew(array)).butterfly));
    return { array, *storage };
}

std::optional<ElementAccessEmitter::IndexBesideConstant> ElementAccessEmitter::indexBesideConstant(ValueID position, LValue sum)
{
    static constexpr int32_t greatestConstant = std::numeric_limits<int16_t>::max();
    if (sum->opcode() != Machine::Add || !sum->child(1)->hasInt32())
        return std::nullopt;
    LValue index = sum->child(0);
    int32_t constant = sum->child(1)->asInt32();
    if (index->hasInt32() || !Machine::clearsUpperHalf(index) || constant < -greatestConstant || constant > greatestConstant)
        return std::nullopt;
    for (ValueID named = position; index->opcode() != Machine::Phi; named = operationAt(named).valueOperand()) {
        if (generator().code().useCountOf(named) != 1)
            return std::nullopt;
        if (operationAt(named).opcode != Opcode::Narrowed)
            break;
    }
    if (constant < 0)
        return IndexBesideConstant::IsNotNegative;
    const Operation& definition = operationAt(definitionOf(position));
    for (uint32_t operandIndex = 0; operandIndex < definition.numberOfOperands(); ++operandIndex) {
        ValueID operand = definition.operand(operandIndex);
        if (operand && representationOf(operand) == Representation::Int32 && generator().isGenerated(operand) && get(operand).value == index)
            return typeOf(operand).integers.minimum >= 0 ? IndexBesideConstant::IsNotNegative : IndexBesideConstant::IsSigned;
    }
    return std::nullopt;
}

Integer ElementAccessEmitter::indexInsideLength(ValueID position)
{
    Operand operand = get(position);
    if (operand.representation != Representation::Int32)
        return indexOrElse(position, true, nullptr);
    LValue word = operand.as<Word32>().value();
    std::optional<IndexBesideConstant> index = indexBesideConstant(position, word);
    if (!index)
        return Integer(m_out.extendWordThatIsNotNegative(word));
    build().statistics().count(StatisticID::ConstantsFoldedIntoElementAddresses);
    LValue extended = *index == IndexBesideConstant::IsSigned ? signExtended(Word32(word->child(0))).value() : m_out.extendWordThatIsNotNegative(word->child(0));
    return Integer(m_out.add(extended, m_out.constInt64(static_cast<int64_t>(word->child(1)->asInt32()))));
}

static_assert(MAX_STORAGE_VECTOR_LENGTH <= static_cast<unsigned>(std::numeric_limits<int32_t>::max()));

LengthTest ElementAccessEmitter::testLength(Integer index, const ArrayElements& elements)
{
    if (index.value()->opcode() != Machine::SExt32)
        return { below(index, publicLength(elements)), index };
    Word32 word(index.value()->child(0));
    return { below(word, loadField<Word32>(elements.butterfly, m_heaps.Butterfly_publicLength)), zeroExtended(word) };
}

static bool liesInsideLength(const CodeGenerator& generator, const ValueContext& access)
{
    if (access.opcode() == Opcode::IsElementPresent)
        return access->isInBounds() || generator.liesInsideByQuestionInFrontOfTurnsLaidOut(access.id());
    return access.opcode() == Opcode::GetArrayElement && (access->isInBounds() || access->isPresent() || generator.liesInsideByQuestionInFrontOfTurnsLaidOut(access.id()));
}

LengthTest ElementAccessEmitter::testLength(const ValueContext& value, Integer index, const ArrayElements& elements)
{
    if (liesInsideLength(generator(), value))
        return { boolean(true), index.value()->opcode() == Machine::SExt32 ? zeroExtended(Word32(index.value()->child(0))) : index };
    if (!VariadicOperandPosition::length(value->opcode) || value->lengthOperands().empty())
        return testLength(index, elements);
    auto lengths = value->lengthOperands();
    Operand length = get(lengths[0]);
    bool isWord = length.representation == Representation::Int32;
    if (!isWord && length.representation != Representation::Int64)
        return testLength(index, elements);
    if (checksBounds())
        stopUnlessInside(value, equal(isWord ? zeroExtended(length.as<Word32>()) : length.as<Integer>(), arrayLength(askedAnew(elements.array))));
    if (index.value()->opcode() != Machine::SExt32)
        return { below(index, isWord ? zeroExtended(length.as<Word32>()) : length.as<Integer>()), index };
    Word32 word(index.value()->child(0));
    return { below(word, isWord ? length.as<Word32>() : low32(length.as<Integer>())), zeroExtended(word) };
}

bool ElementAccessEmitter::readsElementBoxed(const ValueContext& value, const ArrayStorageForm& form)
{
    return value.opcode() == Opcode::GetArrayElement && form.element == Representation::Int32 && holdsUnboxedNumber(value.representation()) && !value->readsHoleAsNaN() && generator().isOnlyBoxed(value.id());
}

void ElementAccessEmitter::defineElement(const ValueContext& value, Operand element, bool isBoxed)
{
    if (!isBoxed) {
        define(value, element);
        return;
    }
    build().statistics().count(StatisticID::NumberElementsReadBoxed);
    generator().defineBoxed(value, element.as<Boxed>());
}

Operand ElementAccessEmitter::readElement(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, LBasicBlock isAbsent, std::optional<Representation> representation)
{
    LengthTest test = testLength(value, index, elements);
    if (!liesInsideLength(generator(), value))
        branchUnless(test.isInside, isAbsent);
    return readNumberInsideLength(value, form, elements, test.indexInside, isAbsent, representation);
}

static bool readsHoleAsNaN(const ValueContext& read)
{
    return read.opcode() == Opcode::GetArrayElement && read->readsHoleAsNaN();
}

bool ElementAccessEmitter::findsElement(const ValueContext& read)
{
    if (readsHoleAsNaN(read))
        return true;
    return read.opcode() == Opcode::GetArrayElement && (read->isPresent() || read->listHasNoHoles());
}

Operand ElementAccessEmitter::whatReadGivesWhereNothingIsFound(const ValueContext& read, Representation representation)
{
    if (!readsHoleAsNaN(read))
        return absentElement(read, representation);
    return convertElementThatHasBeenAsked(read, arrayStorageFormOf(ArrayStorage::Double), number(PNaN), representation, TypeTable::number);
}

Operand ElementAccessEmitter::readElementInsideLength(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, LBasicBlock isHole, std::optional<Representation> representation)
{
    RELEASE_ASSERT(!readsHoleAsNaN(value) || form.element == Representation::Double);
    Operand rawElement = loadRawElement(form, elements, index);
    if (!findsElement(value)) {
        LBasicBlock holdsValue = newBlock();
        m_out.branch(this->isHole(form, rawElement).value(), rareEdge(isHole), normalEdge(holdsValue));
        m_out.appendTo(holdsValue);
    } else if (!readsHoleAsNaN(value))
        checkElementPresenceProof(value, logicalNot(this->isHole(form, rawElement)));
    return convertRawElement(value, form, rawElement, representation.value_or(value.representation()), typeIDOf(value.id()));
}

const PlacedSlot* ElementAccessEmitter::extraPropertiesWordOfReadByPosition(const ValueContext& read)
{
    Type receiver = typeOf(read->objectOperand());
    Type key = typeOf(read->propertyOperand());
    if (!receiver.hasObjects() || !receiver.layouts || !receiver.hasOnlyLayouts() || receiver.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) || !key.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull) || (key.isInteger() && key.integers.maximum < 0))
        return nullptr;
    const PlacedSlot* found = nullptr;
    for (LayoutID layout : membersOf(types(), receiver)) {
        LayoutKind kind = m_program.at(layout).kind;
        if ((kind != LayoutKind::Container && kind != LayoutKind::Record && kind != LayoutKind::Instance) || !m_plan.slots().holdsCompiledObjects(layout))
            return nullptr;
        const PlacedSlot& word = m_plan.slots().extraPropertiesWord(layout);
        if (!word.hasStorage() || (found && (found->offset != word.offset || found->storage != word.storage || found->representation != word.representation)))
            return nullptr;
        found = &word;
    }
    return found;
}

Boxed ElementAccessEmitter::readElementOfExtraProperties(CellPointer object, const PlacedSlot& extraPropertiesWord, Integer index, LBasicBlock notFound, bool readsArrayStorage)
{
    CellPointer holder = emitter<SlotEmitter>().extraPropertiesOf(object, extraPropertiesWord, notFound);
    LBasicBlock hasOtherStorage = readsArrayStorage ? newBlock() : notFound;
    Join<Boxed> join(m_out);
    branchUnless(objectHoldsBoxedElements(holder), hasOtherStorage);
    ArrayElements elements = elementsOf(holder);
    LengthTest test = testLength(index, elements);
    branchUnless(test.isInside, notFound);
    Boxed rawElement = loadBoxedElement(elements, test.indexInside);
    branchUnless(isNotEmpty(rawElement), notFound);
    if (!readsArrayStorage)
        return rawElement;
    join.add(rawElement);

    m_out.appendTo(hasOtherStorage);
    join.add(loadArrayStorageElement(holder, index, notFound));
    return join.finish();
}

void ElementAccessEmitter::overwriteElementOfExtraProperties(const ValueContext& store, CellPointer object, const PlacedSlot& extraPropertiesWord, Integer index, Boxed stored, LBasicBlock notFound)
{
    CellPointer holder = emitter<SlotEmitter>().extraPropertiesOf(object, extraPropertiesWord, notFound);
    Word32 mode = indexingMode(holder, IndexingShapeMask | CopyOnWrite);
    Type typeOfStored = typeOf(store->valueOperand());
    branchUnless(logicalOr(equal(mode, word32(ContiguousShape)), logicalAnd(equal(mode, word32(Int32Shape)), isInt32(stored))), notFound);
    ArrayElements elements = elementsOf(holder);
    LengthTest test = testLength(index, elements);
    branchUnless(test.isInside, notFound);
    branchUnless(isNotEmpty(loadBoxedElement(elements, test.indexInside)), notFound);
    m_out.store64(stored.value(), elementOfUnknownForm(elements, test.indexInside));
    if (!typeOfStored.hasOnlyPrimitives(Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull))
        emitter<BarrierEmitter>().barrier(store, holder);
}

Boxed ElementAccessEmitter::readBoxedElement(const ValueContext& value, const ArrayElements& elements, Integer index, LBasicBlock isAbsent)
{
    LengthTest test = testLength(value, index, elements);
    if (!liesInsideLength(generator(), value))
        branchUnless(test.isInside, isAbsent);
    Boxed rawElement = loadBoxedElement(elements, test.indexInside);
    if (findsElement(value))
        checkElementPresenceProof(value, isNotEmpty(rawElement));
    else
        branchUnless(isNotEmpty(rawElement), isAbsent);
    return rawElement;
}

bool ElementAccessEmitter::asksTruthOfRawElement(ValueID read)
{
    const Operation& operation = generator().code().at(read);
    if (operation.opcode != Opcode::GetArrayElement || findArrayStorageForm(operation.arrayStorage()) || operation.level() != KnowledgeLevel::Proven || !operation.isInBounds() || operation.asksCellWhereNothingIsFound() || operation.readsHoleAsNaN())
        return false;
    std::optional<ArrayStorage> found = generator().kindOfListFoundInFrontOfTurnsLaidOut(operation.arrayOperand());
    return found && isNumericStorage(*found);
}

void ElementAccessEmitter::generateGetArrayElement(const ValueContext& value)
{
    if (!accessToElementsHasCode(Opcode::GetArrayElement, value->arrayStorage(), value->admitsInt32Storage(), false)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    generator().forgetBoxed(value.id());
    if (liesInsideLength(generator(), value))
        holdPositionInsideList(value);
    if (value.isBranchCondition()) {
        branchOnArrayNumberTruth(value);
        return;
    }
    if (LValue number = generator().takeNumberReadByPresenceTest(value.id())) {
        define(value, Number(number));
        return;
    }
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateElementRead(value);
        return;
    }

    if (emitter<GuessedAccessEmitter>().tryReadNumberOfProvenList(value))
        return;
    const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage());
    RELEASE_ASSERT(!form || !value->asksCellWhereNothingIsFound());
    holdKindOfList(value);
    if (std::optional<ArrayStorage> found = !form && value->level() == KnowledgeLevel::Proven && value->isInBounds() && !value->asksCellWhereNothingIsFound() ? kindOfListFoundInFrontOfTurns(value, value->arrayOperand()) : std::nullopt)
        form = findArrayStorageForm(*found);
    if (!form)
        readWithAnyStorage(value);
    else if (value->admitsInt32Storage() && form->kind == ArrayStorage::Double)
        readFromDoubleOrInt32Storage(value);
    else if (liesInsideLength(generator(), value))
        readInsideLength(value, *form);
    else
        readWithKnownStorage(value, *form);
}

void ElementAccessEmitter::readInsideLength(const ValueContext& value, const ArrayStorageForm& form)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements elements = elementsOf(value, array);
    Integer index = indexInsideLength(value->positionOperand());
    const bool isBoxed = readsElementBoxed(value, form);
    const Representation representation = isBoxed ? Representation::JSValue : value.representation();
    if (findsElement(value)) {
        defineElement(value, readNumberInsideLength(value, form, elements, index, nullptr, isBoxed ? std::optional { representation } : std::nullopt), isBoxed);
        return;
    }

    if (form.element == Representation::JSValue && value.representation() == Representation::JSValue && generator().leavesHoleToTestBehind(value)) {
        define(value, convertRawElement(value, form, loadRawElement(form, elements, index), Representation::JSValue, typeIDOf(value.id())));
        return;
    }

    LBasicBlock isHole = newBlock();
    OperandJoin join(m_out, representation);
    join.add(readElementInsideLength(value, form, elements, index, isHole, representation));
    m_out.appendTo(isHole);
    join.add(absentElement(value, representation));
    defineElement(value, join.finish(), isBoxed);
}

static bool isAlwaysArrayIndex(const Type& position)
{
    return position.isInteger() && position.integers.isSubsetOf({ 0, MAX_ARRAY_INDEX });
}

bool ElementAccessEmitter::testsArrayNumberTruth(ValueID read)
{
    const Operation& operation = generator().code().at(read);
    if (operation.opcode != Opcode::GetArrayElement || !isNumericStorage(operation.arrayStorage()) || operation.level() != KnowledgeLevel::Proven || operation.asksCellWhereNothingIsFound() || operation.readsHoleAsNaN())
        return false;
    return (operation.isInBounds() || isAlwaysArrayIndex(typeOf(operation.positionOperand()))) && typeOf(read).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined | Primitive::Absent);
}

void ElementAccessEmitter::branchOnArrayNumberTruth(const ValueContext& value)
{
    static_assert(!static_cast<uint32_t>(JSValue::ValueEmpty));
    BranchTargets targets = value.branchTargets();
    CellPointer array = in<CellPointer>(value->arrayOperand());
    std::optional<ArrayStorage> found = kindOfListFoundInFrontOfTurns(value, value->arrayOperand());
    if (found)
        build().statistics().count(StatisticID::UnrolledLoopRawTruthinessTests);
    else
        holdKindOfList(value);
    ArrayElements elements = elementsOf(value, array);
    bool isInsideLength = liesInsideLength(generator(), value);
    std::optional<Integer> indexMadeInFront;
    if (!isInsideLength) {
        LengthTest test = testLength(value, indexOfAccessOrElse(value, nullptr, NaNIndexMode::ProduceOutOfBoundsIndex), elements);
        LBasicBlock isInBounds = newBlock();
        branch(test.isInside, { normalEdge(isInBounds), targets.notTaken });
        m_out.appendTo(isInBounds);
        indexMadeInFront = test.indexInside.value()->opcode() == Machine::ZExt32 ? Integer(m_out.extendWordThatIsNotNegative(test.indexInside.value()->child(0))) : test.indexInside;
    }
    auto branchOnTruth = [&](const ArrayStorageForm& form) {
        Operand raw = loadRawElement(form, elements, indexMadeInFront ? *indexMadeInFront : indexInsideLength(value->positionOperand()));
        branch(form.kind == ArrayStorage::Double ? isNeitherZeroNorNaN(raw.as<Number>()) : isNotZero(unboxInt32(raw.as<Boxed>())), targets);
    };
    if (found || !value->admitsInt32Storage()) {
        branchOnTruth(arrayStorageFormOf(found.value_or(value->arrayStorage())));
        return;
    }
    if (value->holdsDoublesOperand() && generator().isGenerated(value->holdsDoublesOperand())) {
        Condition holdsDoubles = in<Condition>(value->holdsDoublesOperand());
        if (checksBounds())
            emitter<ThrowEmitter>().requireOrCrash(value, Condition(m_out.equal(holdsDoubles.value(), hasArrayStorage(askedAnew(array), ArrayStorage::Double).value())), std::nullopt, ProofSource::RememberedFact);
        LBasicBlock isOfDoubles = newBlock();
        LBasicBlock isOfInt32s = newBlock();
        m_out.branch(holdsDoubles.value(), normalEdge(isOfDoubles), normalEdge(isOfInt32s));
        m_out.appendTo(isOfDoubles);
        branchOnTruth(arrayStorageFormOf(ArrayStorage::Double));
        m_out.appendTo(isOfInt32s);
        branchOnTruth(arrayStorageFormOf(ArrayStorage::Int32));
        return;
    }
    forDoubleAndInt32Storage(*this, m_out, array, branchOnTruth);
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
    bool isListOfNumbers = result.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent) || isDeclaredToHoldNumbersAlone(m_program, types(), typeIDOf(value->arrayOperand()));
    LBasicBlock holdsUnboxed = !form && representation == Representation::JSValue && isListOfNumbers && generator().runsRepeatedly(value.block()) ? newBlock() : nullptr;
    if (holdsUnboxed)
        generator().build().statistics().count(StatisticID::BoxedReadsWithDoublePath);
    if (!knownForm)
        branchUnless(form ? hasArrayStorage(array, form->kind) : holdsBoxedElements(array), holdsUnboxed ? holdsUnboxed : outOfLine);
    Integer index = indexOfAccessOrElse(value, outOfLine, NaNIndexMode::ProduceOutOfBoundsIndex);
    LBasicBlock holeInside = knownForm && !findsElement(value) && generator().runsRepeatedly(value.block()) && m_code.at(value.block()).frequency != BlockFrequency::Rare ? newBlock() : nullptr;
    if (holeInside) {
        LengthTest test = testLength(value, index, elements);
        if (!liesInsideLength(generator(), value))
            branchUnless(test.isInside, outOfLine);
        join.add(readNumberInsideLength(value, *form, elements, test.indexInside, holeInside));
        m_out.appendToJoin(holeInside, fork);
        join.add(absentElement(value, representation));
    } else if (form)
        join.add(readElement(value, *form, elements, index, outOfLine));
    else
        join.add(conversions.unbox(readBoxedElement(value, elements, index, outOfLine), result, representation));

    if (holdsUnboxed) {
        const ArrayStorageForm& doubles = arrayStorageFormOf(ArrayStorage::Double);
        m_out.appendToArm(holdsUnboxed, fork);
        branchUnless(hasArrayStorage(array, ArrayStorage::Double), outOfLine);
        LengthTest test = testLength(value, indexOfAccessOrElse(value, outOfLine, NaNIndexMode::ProduceOutOfBoundsIndex), elements);
        if (!liesInsideLength(generator(), value))
            branchUnless(test.isInside, outOfLine);
        Operand element = loadRawElement(doubles, elements, test.indexInside);
        if (!findsElement(value))
            branchUnless(logicalNot(isHole(doubles, element)), outOfLine);
        else if (!readsHoleAsNaN(value))
            checkElementPresenceProof(value, logicalNot(isHole(doubles, element)), StatisticID::DoubleElementPresenceProofsChecked);
        join.add(convertElementThatHasBeenAsked(value, doubles, element, representation, typeIDOf(value.id())));
    }

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
    const bool isBoxed = readsElementBoxed(value, form);
    const Representation representation = isBoxed ? Representation::JSValue : value.representation();
    if (isAlwaysIndex && readsHoleAsNaN(value) && form.element == Representation::Int32) {
        LBasicBlock isOutside = newBlock();
        ArrayElements elements = elementsOf(value, array);
        LengthTest test = testLength(value, indexOfAccessOrElse(value, nullptr, NaNIndexMode::ProduceOutOfBoundsIndex), elements);
        branchUnless(test.isInside, isOutside);
        Join<Boxed> join(m_out);
        join.add(loadRawElement(form, elements, test.indexInside).as<Boxed>());
        m_out.appendTo(isOutside);
        join.add(Boxed(m_out.constInt64(0)));
        define(value, numberOfInt32Element(value, join.finish()));
        return;
    }

    LBasicBlock isNotAnIndex = isAlwaysIndex ? nullptr : newBlock();
    LBasicBlock isAbsent = newBlock();
    OperandJoin join(m_out, representation);

    Integer index = indexOfAccessOrElse(value, isNotAnIndex, NaNIndexMode::ProduceOutOfBoundsIndex);
    join.add(readElement(value, form, elementsOf(value, array), index, isAbsent, isBoxed ? std::optional { representation } : std::nullopt));

    m_out.appendTo(isAbsent);
    if (isNotAnIndex)
        branchUnless(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), isNotAnIndex);
    join.add(whatReadGivesWhereNothingIsFound(value, representation));

    if (isNotAnIndex) {
        m_out.appendTo(isNotAnIndex);
        join.add(emitter<ConversionEmitter>().unbox(readOutOfLine(value, array), typeOf(value.id()), representation));
    }
    defineElement(value, join.finish(), isBoxed);
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

    Integer index = indexOfAccessOrElse(value, outOfLine, NaNIndexMode::ProduceOutOfBoundsIndex);
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

static bool endsInCutToInt32(const Code& code, ValueID read)
{
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Negate };
    static constexpr OpcodeSet cuts { Opcode::ToInt32, Opcode::ToUint32, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::URShift };
    for (ValueID user = code.onlyUserOf(read); user; user = code.onlyUserOf(user)) {
        const Operation& operation = code.at(user);
        if (cuts.contains(operation.opcode))
            return true;
        if (!sums.contains(operation.opcode) || operation.representation != Representation::Double)
            return false;
    }
    return false;
}

static bool isTruncatedByOnlyUser(const Code& code, ValueID read)
{
    ValueID user = code.onlyUserOf(read);
    return user && code.at(user).opcode == Opcode::ToInt32;
}

void ElementAccessEmitter::readTruncatedNumber(const ValueContext& value, const ArrayElements& elements, std::optional<Integer> index)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    LBasicBlock isOutside = index ? nullptr : newBlock();
    if (!index) {
        LengthTest test = testLength(value, indexOfAccessOrElse(value, nullptr, NaNIndexMode::ProduceOutOfBoundsIndex), elements);
        branchUnless(test.isInside, isOutside);
        index = test.indexInside;
    }
    static_assert(!static_cast<uint32_t>(JSValue::ValueEmpty));
    Boxed rawElement(m_out.load64(elementOfUnknownForm(elements, *index)));
    LBasicBlock holdsInt32 = newBlock();
    LBasicBlock holdsDouble = newBlock();
    Join<Word32> join(m_out);
    Type position = typeOf(value->positionOperand());
    bool isTable = position.isInteger() && position.integers.minimum >= 0 && position.integers.maximum <= std::numeric_limits<uint16_t>::max();
    m_out.branch(isInt32(rawElement).value(), normalEdge(holdsInt32), isTable ? unlikelyEdge(holdsDouble) : normalEdge(holdsDouble));
    m_out.appendTo(holdsInt32);
    join.add(unboxInt32(rawElement));
    m_out.appendTo(holdsDouble);
    join.add(conversions.toInt32(Number(m_out.bitCast(rawElement.value(), Machine::Double))));
    if (isOutside) {
        m_out.appendTo(isOutside);
        join.add(word32(0));
    }
    build().statistics().count(StatisticID::TruncatedElementsReadAsWords);
    define(value, toNumber(join.finish()));
}

Operand ElementAccessEmitter::numberOfInt32Element(const ValueContext& value, Boxed element)
{
    if (endsInCutToInt32(m_code, value.id())) {
        Number chosen = select(isEmpty(element), this->number(PNaN), toNumber(unboxInt32(element)));
        emitter<ConversionEmitter>().noteInt32ElementBehindNumberOrNaN(chosen, element);
        return convertElementThatHasBeenAsked(value, arrayStorageFormOf(ArrayStorage::Double), chosen, value.representation(), typeIDOf(value.id()));
    }
    LBasicBlock isHole = newBlock();
    LBasicBlock holdsValue = newBlock();
    Join<Number> join(m_out);
    m_out.branch(isEmpty(element).value(), rareEdge(isHole), normalEdge(holdsValue));
    m_out.appendTo(holdsValue);
    join.add(toNumber(unboxInt32Element(element)));
    m_out.appendTo(isHole);
    join.add(this->number(PNaN));
    Number number = join.finish();
    emitter<ConversionEmitter>().noteInt32ElementBehindNumberOrNaN(number, element);
    return convertElementThatHasBeenAsked(value, arrayStorageFormOf(ArrayStorage::Double), number, value.representation(), typeIDOf(value.id()));
}

bool ElementAccessEmitter::leavesIterationAtHole(const ValueContext& read, std::optional<Representation> representation)
{
    return !readsHoleAsNaN(read) && !findsElement(read) && !representation && holdsDouble(read.representation());
}

Operand ElementAccessEmitter::readNumberInsideLength(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, LBasicBlock isHole, std::optional<Representation> representation)
{
    RELEASE_ASSERT(!representation || !readsHoleAsNaN(value));
    if (form.element == Representation::Double && generator().isLayingOutTurns()) {
        LBasicBlock holeExit = leavesIterationAtHole(value, representation) ? generator().optionalNumberHoleExit() : nullptr;
        Operand element = readElementInsideLength(value, form, elements, index, holeExit ? holeExit : isHole, representation);
        if (element.value && element.value->type() == Machine::Double)
            emitter<ConversionEmitter>().noteDoubleElement(element.value);
        return element;
    }
    if (LBasicBlock holeExit = form.element == Representation::Int32 && leavesIterationAtHole(value, representation) ? generator().optionalNumberHoleExit() : nullptr) {
        Boxed rawElement = loadRawElement(form, elements, index).as<Boxed>();
        LBasicBlock holdsValue = newBlock();
        m_out.branch(isEmpty(rawElement).value(), rareEdge(holeExit), normalEdge(holdsValue));
        m_out.appendTo(holdsValue);
        Word32 word = unboxInt32Element(rawElement);
        generator().noteWordOfElementInTurnLaidOut(value.id(), word.value());
        Number number = toNumber(word);
        emitter<ConversionEmitter>().noteNumberOfInt32Element(number);
        return convertElementThatHasBeenAsked(value, arrayStorageFormOf(ArrayStorage::Double), number, value.representation(), typeIDOf(value.id()));
    }
    if (!readsHoleAsNaN(value) || form.element == Representation::Double)
        return readElementInsideLength(value, form, elements, index, isHole, representation);

    RELEASE_ASSERT(form.element == Representation::Int32);
    Boxed rawElement = loadRawElement(form, elements, index).as<Boxed>();
    if (generator().asksBehindTurnsWhetherElementIsThere(value.id())) {
        if (!value->isPresent() && !value->listHasNoHoles())
            generator().noteBoxedWordOfElementInTurns(rawElement.value());
        else
            checkElementPresenceProof(value, isNotEmpty(rawElement), StatisticID::DeferredElementPresenceProofsChecked);
        Word32 word = unboxInt32Element(rawElement);
        generator().noteWordOfElementInTurnLaidOut(value.id(), word.value());
        Number number = toNumber(word);
        emitter<ConversionEmitter>().noteNumberOfInt32Element(number);
        return convertElementThatHasBeenAsked(value, arrayStorageFormOf(ArrayStorage::Double), number, value.representation(), typeIDOf(value.id()));
    }
    if (value->listHasNoHoles()) {
        checkElementPresenceProof(value, isNotEmpty(rawElement));
        Word32 word = unboxInt32Element(rawElement);
        generator().noteWordOfElementInTurnLaidOut(value.id(), word.value());
        Number number = toNumber(word);
        emitter<ConversionEmitter>().noteNumberOfInt32Element(number);
        return convertElementThatHasBeenAsked(value, arrayStorageFormOf(ArrayStorage::Double), number, value.representation(), typeIDOf(value.id()));
    }
    if (LBasicBlock leaving = generator().blockThatHoleLeavesTurnLaidOutBy()) {
        LBasicBlock holdsValue = newBlock();
        m_out.branch(isEmpty(rawElement).value(), rareEdge(leaving), normalEdge(holdsValue));
        m_out.appendTo(holdsValue);
        Word32 word = unboxInt32Element(rawElement);
        generator().noteWordOfElementInTurnLaidOut(value.id(), word.value());
        Number number = toNumber(word);
        emitter<ConversionEmitter>().noteNumberOfInt32Element(number);
        return convertElementThatHasBeenAsked(value, arrayStorageFormOf(ArrayStorage::Double), number, value.representation(), typeIDOf(value.id()));
    }
    if (isHole) {
        LBasicBlock holdsValue = newBlock();
        m_out.branch(isEmpty(rawElement).value(), rareEdge(isHole), normalEdge(holdsValue));
        m_out.appendTo(holdsValue);
        return convertElementThatHasBeenAsked(value, form, rawElement, value.representation(), typeIDOf(value.id()));
    }
    return numberOfInt32Element(value, rawElement);
}

bool ElementAccessEmitter::isReckonedWithFraction(ValueID read) const
{
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    ValueID user = m_code.onlyUserOf(read);
    if (!user || !(typeOf(read).primitives & Primitive::FractionalNumber) || !arithmetic.contains(m_code.at(user).opcode))
        return false;
    const Operation& operation = m_code.at(user);
    return !!(typeOf(operation.operand(operation.operand(0) == read ? 1 : 0)).primitives & Primitive::FractionalNumber);
}

void ElementAccessEmitter::readFromDoubleOrInt32Storage(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID position = value->positionOperand();
    RELEASE_ASSERT(value->arrayStorage() == ArrayStorage::Double);
    bool isOnlyBoxed = !readsHoleAsNaN(value) && generator().isOnlyBoxed(value.id());
    Representation representation = isOnlyBoxed ? Representation::JSValue : value.representation();
    std::optional<Representation> representationOfElement = isOnlyBoxed ? std::optional { representation } : std::nullopt;
    auto defineElement = [&](Operand element) {
        if (isOnlyBoxed) {
            build().statistics().count(StatisticID::NumberElementsReadBoxed);
            generator().defineBoxed(value, element.as<Boxed>());
        } else
            define(value, element);
    };

    bool isInsideLength = liesInsideLength(generator(), value);
    std::optional<ArrayStorage> found = isInsideLength ? kindOfListFoundInFrontOfTurns(value, value->arrayOperand()) : std::nullopt;
    if (readsHoleAsNaN(value) && representation == Representation::Double && (isInsideLength || isAlwaysArrayIndex(typeOf(position))) && isTruncatedByOnlyUser(m_code, value.id()) && !generator().isLayingOutTurns() && !generator().asksBehindTurnsWhetherElementIsThere(value.id())) {
        readTruncatedNumber(value, elementsOf(value, array), isInsideLength ? std::optional(indexInsideLength(position)) : std::nullopt);
        return;
    }
    if (found && (findsElement(value) || (leavesIterationAtHole(value, representationOfElement) && generator().optionalNumberHoleExit()))) {
        defineElement(readNumberInsideLength(value, arrayStorageFormOf(*found), elementsOf(value, array), indexInsideLength(position), nullptr, representationOfElement));
        return;
    }

    LBasicBlock isNotAnIndex = isInsideLength || isAlwaysArrayIndex(typeOf(position)) ? nullptr : newBlock();
    LBasicBlock isAbsent = isInsideLength && findsElement(value) ? nullptr : newBlock();
    OperandJoin join(m_out, representation);

    ArrayElements elements = elementsOf(value, array);
    bool isExtendedAtLoad = isInsideLength && representationOf(position) == Representation::Int32;
    std::optional<Integer> indexMadeInFront;
    if (!isExtendedAtLoad)
        indexMadeInFront = isInsideLength ? indexInsideLength(position) : indexOfAccessOrElse(value, isNotAnIndex, NaNIndexMode::ProduceOutOfBoundsIndex);
    std::optional<Integer> indexInside = indexMadeInFront;
    auto indexInArm = [&] {
        return indexInside ? *indexInside : indexInsideLength(position);
    };
    if (!isInsideLength) {
        LengthTest test = testLength(value, *indexMadeInFront, elements);
        branchUnless(test.isInside, isAbsent);
        indexInside = test.indexInside;
    }
    if (std::optional<ArrayStorage> found = kindOfListFoundInFrontOfTurns(value, value->arrayOperand()))
        join.add(readNumberInsideLength(value, arrayStorageFormOf(*found), elements, indexInArm(), isAbsent, representationOfElement));
    else if (LValue holdsDoubles = value->holdsDoublesOperand() && generator().isGenerated(value->holdsDoublesOperand()) ? in<Condition>(value->holdsDoublesOperand()).value() : generator().holdsDoublesAsReadInFrontOfTurnsLaidOut(value->arrayOperand())) {
        if (checksBounds()) {
            emitter<ThrowEmitter>().requireOrCrash(value, Condition(m_out.equal(holdsDoubles, hasArrayStorage(askedAnew(array), ArrayStorage::Double).value())), std::nullopt, ProofSource::RememberedFact);
            build().statistics().count(StatisticID::IndexingTypeProofsChecked);
        }
        LBasicBlock isOfDoubles = newBlock();
        LBasicBlock isOfInt32s = newBlock();
        m_out.branch(holdsDoubles, normalEdge(isOfDoubles), isReckonedWithFraction(value.id()) ? unlikelyEdge(isOfInt32s) : normalEdge(isOfInt32s));
        m_out.appendTo(isOfDoubles);
        join.add(readNumberInsideLength(value, arrayStorageFormOf(ArrayStorage::Double), elements, indexInArm(), isAbsent, representationOfElement));
        m_out.appendTo(isOfInt32s);
        join.add(readNumberInsideLength(value, arrayStorageFormOf(ArrayStorage::Int32), elements, indexInArm(), isAbsent, representationOfElement));
    } else {
        forDoubleAndInt32Storage(*this, m_out, array, [&](const ArrayStorageForm& form) {
            join.add(readNumberInsideLength(value, form, elements, indexInArm(), isAbsent, representationOfElement));
        });
    }

    if (isAbsent) {
        m_out.appendTo(isAbsent);
        if (isNotAnIndex)
            branchUnless(belowOrEqual(*indexMadeInFront, integer(MAX_ARRAY_INDEX)), isNotAnIndex);
        join.add(whatReadGivesWhereNothingIsFound(value, representation));
    }
    if (isNotAnIndex) {
        m_out.appendTo(isNotAnIndex);
        join.add(emitter<ConversionEmitter>().unbox(readOutOfLine(value, array), typeOf(value.id()), representation));
    }
    defineElement(join.finish());
}

ValueID ElementAccessEmitter::findReadAfterPresenceTest(const ValueContext& test)
{
    const Terminator& terminator = m_code.at(test.block()).terminator;
    if (!test.isBranchCondition() || checksBounds() || terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != test.id())
        return { };
    const Edge& passing = m_code.at(terminator.taken());
    const Block& block = m_code.at(passing.to);
    std::span<const ValueID> values = m_code.valuesOf(passing.to);
    if (values.empty() || !passing.arguments().empty() || block.isLoopHead || generator().singlePredecessorOf(passing.to) != test.block())
        return { };
    const Operation& read = m_code.at(values.front());
    if (read.opcode != Opcode::GetArrayElement || read.arrayStorage() != ArrayStorage::Double || !read.admitsInt32Storage() || !read.isPresent() || read.level() != KnowledgeLevel::Proven || read.readsHoleAsNaN())
        return { };
    if (read.arrayOperand() != test->arrayOperand() || read.positionOperand() != test->positionOperand() || read.representation != Representation::Double)
        return { };
    return generator().isOnlyBoxed(values.front()) || endsInCutToInt32(m_code, values.front()) ? ValueID() : values.front();
}

void ElementAccessEmitter::branchOnPresentElementInDoubleOrInt32Storage(const ValueContext& test, BranchTargets targets)
{
    RELEASE_ASSERT(test->arrayStorage() == ArrayStorage::Double);
    CellPointer array = in<CellPointer>(test->arrayOperand());
    Integer index = indexOfPresenceTest(test, targets.notTaken.block());
    ArrayElements elements = elementsOf(test, array);
    if (std::optional<ArrayStorage> found = kindOfListFoundInFrontOfTurns(test, test->arrayOperand())) {
        branchOnElementPresence(test, &arrayStorageFormOf(*found), elements, index, targets);
        return;
    }
    ValueID read = findReadAfterPresenceTest(test);
    std::optional<OperandJoin> join;
    if (read)
        join.emplace(m_out, Representation::Double);
    auto branchInArm = [&](const ArrayStorageForm& form) {
        if (!join) {
            branchOnElementPresence(test, &form, elements, index, targets);
            return;
        }
        LBasicBlock isPresent = newBlock();
        TestedElement tested = branchOnElementPresence(test, &form, elements, index, { normalEdge(isPresent), targets.notTaken });
        m_out.appendTo(isPresent);
        Operand raw = tested.raw ? *tested.raw : loadRawElement(form, elements, tested.index);
        join->add(form.element == Representation::Double ? raw : toNumber(unboxInt32Element(raw.as<Boxed>())));
    };
    auto finish = [&] {
        if (!join)
            return;
        generator().noteNumberReadByPresenceTest(read, join->finish().as<Number>().value());
        m_out.jump(targets.taken);
    };
    if (test->holdsDoublesOperand() && generator().isGenerated(test->holdsDoublesOperand())) {
        Condition holdsDoubles = in<Condition>(test->holdsDoublesOperand());
        if (checksBounds())
            emitter<ThrowEmitter>().requireOrCrash(test, Condition(m_out.equal(holdsDoubles.value(), hasArrayStorage(askedAnew(array), ArrayStorage::Double).value())), std::nullopt, ProofSource::RememberedFact);
        LBasicBlock isOfDoubles = newBlock();
        LBasicBlock isOfInt32s = newBlock();
        m_out.branch(holdsDoubles.value(), normalEdge(isOfDoubles), normalEdge(isOfInt32s));
        m_out.appendTo(isOfDoubles);
        branchInArm(arrayStorageFormOf(ArrayStorage::Double));
        m_out.appendTo(isOfInt32s);
        branchInArm(arrayStorageFormOf(ArrayStorage::Int32));
    } else
        forDoubleAndInt32Storage(*this, m_out, array, branchInArm);
    finish();
}

Boxed ElementAccessEmitter::readOutOfLine(const ValueContext& value, CellPointer array)
{
    ValueID position = value->positionOperand();
    if (value.opcode() != Opcode::GetArrayElement)
        return emitter<AccessEmitter>().getByVal(value, boxCell(array), boxedPosition(position));

    stopUnlessListIsAsMadeWhereNothingIsFound(value, array);
    ArrayRead read = value->mayHaveNumberNames() ? ArrayRead::OfPlainList : ArrayRead::OfListAsMade;
    if (Operand key = get(position); key.is<Number>())
        return Boxed(callStub(value, arrayGetAtNumber(read), { array.value(), key.value }).first);
    return Boxed(callStub(value, arrayGetAtIndex(read), { array.value(), integerOf(position).value() }).first);
}

Integer ElementAccessEmitter::indexOfPresenceTest(const ValueContext& test, LBasicBlock isNotAnIndex)
{
    ValueID position = test->positionOperand();
    if (!liesInsideLength(generator(), test))
        return indexOfAccessOrElse(test, isNotAnIndex, NaNIndexMode::ProduceOutOfBoundsIndex);
    holdPositionInsideList(test);
    return indexInsideLength(position);
}

ElementAccessEmitter::TestedElement ElementAccessEmitter::branchOnElementPresence(const ValueContext& value, const ArrayStorageForm* form, const ArrayElements& elements, Integer index, BranchTargets targets)
{
    LengthTest test = testLength(value, index, elements);
    Integer inside = test.indexInside;
    if (!liesInsideLength(generator(), value)) {
        LBasicBlock isInBounds = newBlock();
        branch(test.isInside, { normalEdge(isInBounds), targets.notTaken });
        m_out.appendTo(isInBounds);
        if (inside.value()->opcode() == Machine::ZExt32)
            inside = Integer(m_out.extendWordThatIsNotNegative(inside.value()->child(0)));
    }
    if (value.opcode() == Opcode::IsElementPresent && value->listHasNoHoles()) {
        if (checksBounds())
            stopUnlessInside(value, logicalNot(form ? isHole(*form, loadRawElement(*form, elements, inside)) : isEmpty(loadBoxedElement(elements, inside))));
        branch(boolean(true), targets);
    } else if (AOT::hasFlag(build().options().injectedFaults, Wire::FaultInjection::AssumeInBoundsElementsPresent))
        branch(boolean(true), targets);
    else if (form) {
        Operand raw = loadRawElement(*form, elements, inside);
        branch(isHole(*form, raw), targets.swapped());
        return { inside, raw };
    } else
        branch(isEmpty(loadBoxedElement(elements, inside)), targets.swapped());
    return { inside, std::nullopt };
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

        Integer index = indexOfAccessOrElse(value, outOfLine);
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
        Integer position = indexOrElse(value->positionOperand(), false, fails, NaNIndexMode::ProduceOutOfBoundsIndex);
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

void ElementAccessEmitter::generateGetElementInStorageOrAbsent(const ValueContext& value)
{
    LBasicBlock isAbsent = newBlock();
    LBasicBlock holdsDoubles = newBlock();
    LBasicBlock holdsValues = newBlock();
    Join<Boxed> join(m_out);
    Integer position = indexOrElse(value->positionOperand(), false, isAbsent, NaNIndexMode::ProduceOutOfBoundsIndex);
    Operand operand = get(value->objectOperand());
    if (!operand.is<CellPointer>()) {
        branchUnless(isCell(operand.as<Boxed>()), isAbsent);
        if (typeOf(value->objectOperand()).primitives & Primitive::Absent)
            branchUnless(isNotEmpty(operand.as<Boxed>()), isAbsent);
    }
    CellPointer object = operand.is<CellPointer>() ? operand.as<CellPointer>() : unboxCell(operand.as<Boxed>());
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape && !(IndexingShapeMask & 1));
    Word32 shape = indexingMode(object, IndexingShapeMask);
    branchUnless(belowOrEqual(sub(shape, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), isAbsent);
    ArrayElements elements = elementsOf(object);
    LengthTest test = testLength(position, elements);
    branchUnless(test.isInside, isAbsent);
    m_out.branch(equal(shape, word32(DoubleShape)).value(), normalEdge(holdsDoubles), normalEdge(holdsValues));

    m_out.appendTo(holdsValues);
    join.add(loadBoxedElement(elements, test.indexInside));

    m_out.appendTo(holdsDoubles);
    const ArrayStorageForm& doubles = arrayStorageFormOf(ArrayStorage::Double);
    Operand number = loadRawElement(doubles, elements, test.indexInside);
    branchUnless(logicalNot(isHole(doubles, number)), isAbsent);
    join.add(boxRawElement(doubles, number));

    m_out.appendTo(isAbsent);
    join.add(boxedConstant(JSValue()));
    define(value, join.finish());
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
    CharacterView view = strings.viewForReadOfCodeUnit(value, value->stringOperand());
    Word32 position = strings.word32Of(value->positionOperand());
    if (checksBounds() && view.possibleRope)
        view.length = strings.lengthOf(view.owner);
    if (checksBounds())
        stopUnlessInside(value, below(position, view.length));
    ValueID wholeNumber = value->positionOperand();
    while (valuePreservingOpcodes.contains(opcodeOf(wholeNumber)))
        wholeNumber = operationAt(wholeNumber).operand(0);
    bool isAddressedByWholeNumber = generator().isLayingOutTurns() && !m_code.isConstant(wholeNumber) && representationOf(wholeNumber) == Representation::Int64
        && representationOf(value->positionOperand()) == Representation::Int32 && typeOf(value->positionOperand()).integers.minimum >= 0
        && position.value()->opcode() == Machine::Trunc && position.value()->child(0) == in<Integer>(wholeNumber).value();
    if (isAddressedByWholeNumber && checksBounds())
        stopUnlessInside(value, below(in<Integer>(wholeNumber), zeroExtended(view.length)));
    if (isAddressedByWholeNumber)
        build().statistics().count(StatisticID::CodeUnitIndexExtensionsRemoved);
    define(value, strings.codeUnitAt(view, position, isAddressedByWholeNumber ? std::optional(in<Integer>(wholeNumber)) : std::nullopt));
}

void ElementAccessEmitter::generateConvertToDoubleStorage(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    LBasicBlock isOtherStorage = newBlock();
    LBasicBlock isNumeric = newBlock();
    LBasicBlock done = newBlock();
    branchUnless(hasArrayStorage(array, ArrayStorage::Double, true), isOtherStorage);
    m_out.jump(done);

    m_out.appendTo(isOtherStorage);
    m_out.branch(hasNoBits(loadByte(array, m_heaps.JSArray_indexingTypeAndMisc), ContiguousShape).value(), normalEdge(isNumeric), normalEdge(done));

    m_out.appendTo(isNumeric);
    callVoidOperation(value, RuntimeOperation::ConvertToDoubleStorage, { array }, { .isOnRarePath = true });
    m_out.jump(done);

    m_out.appendTo(done);
}

void ElementAccessEmitter::generateReserveArrayCapacity(const ValueContext& value)
{
    if (std::exchange(generator().m_reservationThatBirthHasServed, ValueID()) == value.id())
        return;
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Number room = in<Number>(value->capacityOperand());
    if (!generator().paysForCodeInLine(value.block()) && value->heldOperands().empty()) {
        callStub(value, reserveArrayCapacity(), { array.value(), room.value() });
        return;
    }
    LBasicBlock done = newBlock();
    std::optional<Integer> lengthJoined;
    std::span<const ValueID> allHeld = value->heldOperands();
    for (size_t place = 0; place < allHeld.size(); ++place) {
        ValueID held = allHeld[place];
        const Type& type = typeOf(held);
        if (type.isInteger()) {
            lengthJoined = add(lengthJoined.value_or(integer(0)), in<Integer>(held));
            continue;
        }
        Operand operand = get(held);
        if (type.primitives & Primitive::Absent)
            branchUnless(logicalNot(emitter<ConversionEmitter>().isAbsent(operand)), done);
        if (!type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Absent))
            continue;
        CellPointer text = operand.is<CellPointer>() ? operand.as<CellPointer>() : unboxCell(operand.as<Boxed>());
        Integer length = zeroExtended(emitter<StringEmitter>().lengthOf(text));
        if (place + 1 < allHeld.size() && typeOf(allHeld[place + 1]).isInteger())
            length = mul(length, in<Integer>(allHeld[++place]));
        lengthJoined = add(lengthJoined.value_or(integer(0)), length);
    }
    if (lengthJoined)
        branchUnless(belowOrEqual(*lengthJoined, integer(JSString::MaxLength)), done);
    if (generator().paysForCodeInLine(value.block()))
        branchUnless(greaterThan(room, toNumber(vectorLength(elementsOf(array)))), done);
    callStub(value, reserveArrayCapacity(), { array.value(), room.value() });
    m_out.jump(done);

    m_out.appendTo(done);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
