/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTRuntimeValidation.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "JSType.h"
#include <algorithm>
#include <bit>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr const ArrayStorageForm& int32s = arrayStorageFormOf(ArrayStorage::Int32);
static constexpr const ArrayStorageForm& doubles = arrayStorageFormOf(ArrayStorage::Double);
static constexpr const ArrayStorageForm& contiguous = arrayStorageFormOf(ArrayStorage::Contiguous);

static_assert(int32s.hole == contiguous.hole && int32s.element != Representation::Double && contiguous.element == Representation::JSValue);
static_assert(doubles.element == Representation::Double);
static_assert(!remembersOwnerOf(Opcode::TryPutByValDirect), "the owner barrier runs only on the path that stores, so it cannot cover an earlier store");
static_assert(!remembersOwnerOf(Opcode::TryPutIndex), "tryStoreElement remembers the stored element, not the array");

void ArrayIntrinsicEmitter::generateIsPristineArray(const ValueContext& value)
{
    static_assert(DerivedArrayType == ArrayType + 1);
    LBasicBlock isNotObject = newBlock();
    Join<Condition> join(m_out);
    CellPointer object = objectOrElse(value->valueOperand(), isNotObject);
    Condition isArray = hasCellTypeIn(object, { ArrayType, DerivedArrayType });
    join.add(logicalAnd(isArray, elements().isPristineArray(object, { PristineAspects::Storage, PristineAspects::Names })));

    m_out.appendTo(isNotObject);
    join.add(boolean(false));
    defineTest(value, join.finish());
}

void ArrayIntrinsicEmitter::generateArrayStorageKind(const ValueContext& value)
{
    // The three shapes lie two apart in the order of their kinds, and the shape below them is of no kind.
    static_assert(int32s.shape + 2 == doubles.shape && doubles.shape + 2 == contiguous.shape && int32s.shape >= 2);
    static_assert(!static_cast<int32_t>(ArrayStorage::Other) && static_cast<int32_t>(ArrayStorage::Int32) == 1 && static_cast<int32_t>(ArrayStorage::Double) == 2 && static_cast<int32_t>(ArrayStorage::Contiguous) == 3);
    Word32 shape = elements().indexingMode(in<CellPointer>(value->arrayOperand()), arrayStorageTestMask);
    Word32 number = shiftRightUnsigned(sub(shape, int32s.shape - 2), 1);
    define(value, select(belowOrEqual(number, static_cast<int32_t>(ArrayStorage::Contiguous)), number, word32(static_cast<int32_t>(ArrayStorage::Other))));
}

void ArrayIntrinsicEmitter::generateGetDoubleIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    LBasicBlock otherwise = newBlock();
    Join<Number> join(m_out);
    branchUnless(elements().hasShape(elements().indexingMode(array, arrayStorageTestMask), doubles), otherwise);
    ArrayElements storage = elements().elementsOf(array);
    branchUnless(below(index, elements().publicLength(storage)), otherwise);
    join.add(elements().loadRawElement(doubles, storage, index).as<Number>());

    m_out.appendTo(otherwise);
    join.add(elements().rawHole(doubles).as<Number>());
    define(value, join.finish());
}

Boxed ArrayIntrinsicEmitter::contiguousElementOrEmpty(CellPointer array, Integer index)
{
    LBasicBlock otherwise = newBlock();
    Join<Boxed> join(m_out);
    Word32 shape = elements().indexingMode(array, arrayStorageTestMask);
    // One comparison for the two shapes: the distance from the lower one is nothing or the one bit between them.
    constexpr IndexingType distance = contiguous.shape - int32s.shape;
    static_assert(contiguous.shape > int32s.shape && hasOneBitSet(distance));
    branchUnless(Condition(m_out.testIsZero32(m_out.sub(shape.value(), int32s.shape), ~static_cast<int32_t>(distance))), otherwise);
    ArrayElements storage = elements().elementsOf(array);
    branchUnless(below(index, elements().publicLength(storage)), otherwise);
    Boxed element(m_out.load64(elements().elementOfUnknownForm(storage, index)));
    ensureStillAliveHere(array);
    join.add(element);

    m_out.appendTo(otherwise);
    join.add(elements().rawHole(contiguous).as<Boxed>());
    return join.finish();
}

void ArrayIntrinsicEmitter::generateHasContiguousIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Boxed element = contiguousElementOrEmpty(array, integerOperand(value->positionOperand()));
    define(value, isNotEmpty(element));
}

void ArrayIntrinsicEmitter::generateGetContiguousIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Boxed element = contiguousElementOrEmpty(array, integerOperand(value->positionOperand()));
    Condition isFilled = isNotEmpty(element);
    defineAs(value, select(isFilled, element, undefined()), typeOf(value.id()));
}

// For an array that the library made, filled and handed to nothing else: its form holds, and it holds the index.
void ArrayIntrinsicEmitter::generateGetOwnContiguousIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(array);
    Boxed element(m_out.load64(elements().elementOfUnknownForm(storage, integerOperand(value->positionOperand()))));
    ensureStillAliveHere(array);
    defineAs(value, element, typeOf(value.id()));
}

void ArrayIntrinsicEmitter::generateGetOwnDoubleIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(array);
    define(value, elements().loadRawElement(doubles, storage, integerOperand(value->positionOperand())).as<Number>());
}

void ArrayIntrinsicEmitter::generateGetOwnInt32Index(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(array);
    define(value, Word32(m_out.load32(elements().elementOfUnknownForm(storage, integerOperand(value->positionOperand())))));
}

void ArrayIntrinsicEmitter::generatePutOwnContiguousIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID stored = value->valueOperand();
    Operand rawElement = get(stored);
    elements().storeRawElement(contiguous, elements().elementsOf(array), integerOperand(value->positionOperand()), rawElement);
    barrierAfterStore(value, array, rawElement, typeOf(stored));
}

void ArrayIntrinsicEmitter::generatePutOwnDoubleIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    elements().storeRawElement(doubles, elements().elementsOf(array), integerOperand(value->positionOperand()), get(value->valueOperand()));
}

// Over an integer: the upper half of the word is the tag of every int32, and stays.
void ArrayIntrinsicEmitter::generatePutOwnInt32Index(const ValueContext& value)
{
    static_assert(std::endian::native == std::endian::little);
    CellPointer array = in<CellPointer>(value->arrayOperand());
    m_out.store32(low32(integerOperand(value->valueOperand())).value(), elements().elementOfUnknownForm(elements().elementsOf(array), integerOperand(value->positionOperand())));
}

void ArrayIntrinsicEmitter::putInBounds(const ValueContext& value, const ArrayStorageForm& form)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    ValueID stored = value->valueOperand();
    Operand rawElement = get(stored);
    LBasicBlock isInvalid = newBlock();
    LBasicBlock isValid = newBlock();
    branchUnless(elements().hasWritableShape(elements().indexingMode(array, IndexingShapeAndWritabilityMask), form), isInvalid);
    ArrayElements storage = elements().elementsOf(array);
    branch(below(index, elements().publicLength(storage)), { normalEdge(isValid), rareEdge(isInvalid) });

    m_out.appendTo(isInvalid);
    callVoidOperation(value, RuntimeOperation::ReportValidationFailure, { boxCell(array), word32(static_cast<int32_t>(ValidationFailureKind::ValueType)), word32(m_id.index()), word32(value.id().index()) }, { .isOnRarePath = true });
    m_out.unreachable();

    m_out.appendTo(isValid);
    elements().storeRawElement(form, storage, index, rawElement);
    if (form.holdsCells)
        barrierAfterStore(value, array, rawElement, typeOf(stored));
}

const ArrayStorageForm& ArrayIntrinsicEmitter::formToConstruct(const ValueContext& value) const
{
    const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage());
    return form ? *form : contiguous;
}

CellPointer ArrayIntrinsicEmitter::sourceOrNull(ValueID id)
{
    CellPointer null(m_out.int64Zero);
    if (!id || typeOf(id).isUndefinedOrNull())
        return null;
    Operand operand = get(id);
    switch (operand.representation) {
    case Representation::Cell:
        return operand.as<CellPointer>();
    case Representation::Cell32: {
        NarrowReference reference = operand.as<NarrowReference>();
        Condition isCell = isCellReference(reference);
        return select(isCell, decode(reference), null);
    }
    case Representation::JSValue: {
        Boxed boxed = operand.as<Boxed>();
        return select(isCell(boxed), unboxCell(boxed), null);
    }
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::LiteralString:
        return null;
    case Representation::Unchosen:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The next operation copies as many elements into the array as it has. Only casts stand between the two, so nothing runs and nobody else has the array.
bool ArrayIntrinsicEmitter::isFilledByCopyBehind(const ValueContext& allocation) const
{
    if (allocation.opcode() != Opcode::NewContiguousArray)
        return false;
    std::span<const ValueID> values = m_function.at(allocation.block()).values.span();
    auto next = std::ranges::find(values, allocation.id());
    RELEASE_ASSERT(next != values.end());
    for (++next; next != values.end(); ++next) {
        const Value& value = m_function.at(*next);
        if (value.opcode == Opcode::AssumeType)
            continue;
        return value.opcode == Opcode::TryCopyFilledElements && value.countOperand() == allocation->lengthOperand()
            && sameObjectThroughCasts(m_function, value.arrayOperand()).valueInFrontOfFacts() == allocation.id();
    }
    return false;
}

void ArrayIntrinsicEmitter::newArrayOfHoles(const ValueContext& value, Integer length, ValueID source, const ArrayStorageForm& form, uint32_t flags)
{
    using ElementsBelowLength = ArrayAllocationEmitter::ElementsBelowLength;
    using LengthIsSetBy = ArrayAllocationEmitter::LengthIsSetBy;
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    ArrayAllocationEmitter allocation(generator());
    bool isFilledByCopy = isFilledByCopyBehind(value);
    // The operation gives the array this length too, unless the number is its capacity.
    bool setsLengthBehindJoin = !isFilledByCopy && !(flags & NewArrayWithHolesFlag::lengthIsCapacity);
    join.add(allocation.allocateArrayOfHoles(value, length, form, slowPath, isFilledByCopy ? ElementsBelowLength::NotCleared : ElementsBelowLength::Cleared, setsLengthBehindJoin ? LengthIsSetBy::Caller : LengthIsSetBy::Allocation));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::NewArrayWithHoles, { sourceOrNull(source), length, word32(form.indexingType), word32(flags) }, { .isOnRarePath = true }));
    CellPointer array = join.finish();
    if (setsLengthBehindJoin)
        allocation.setLengthOfNewArray(array, length);
    define(value, array);
}

void ArrayIntrinsicEmitter::newArrayOfForm(const ValueContext& value, const ArrayStorageForm& form)
{
    newArrayOfHoles(value, integerOperand(value->lengthOperand()), ValueID(), form, NewArrayWithHolesFlag::mustHaveIndexingType);
}

void ArrayIntrinsicEmitter::generateNewArrayLike(const ValueContext& value)
{
    const Type& length = typeOf(value->lengthOperand());
    bool isEmpty = length.isInteger() && length.integers == Range::one(0);
    newArrayOfHoles(value, isEmpty ? integer(0) : integerOperand(value->lengthOperand()), value->sourceOperand(), formToConstruct(value), 0);
}

void ArrayIntrinsicEmitter::generateNewArrayWithCapacity(const ValueContext& value)
{
    Integer capacity = integerOperand(value->lengthOperand());
    Integer maxCapacity = integer(static_cast<int64_t>(build().limit(LimitID::MaxFusedArrayCapacity)));
    Integer length = select(below(capacity, maxCapacity), capacity, maxCapacity);
    newArrayOfHoles(value, length, value->sourceOperand(), formToConstruct(value), NewArrayWithHolesFlag::lengthIsCapacity);
}

Integer ArrayIntrinsicEmitter::lengthOfArrayStoredInto(const ValueContext& value, const ArrayElements& storage)
{
    if (!value->lengthOperand())
        return elements().publicLength(storage);
    Integer length = integerOperand(value->lengthOperand());
    if (!generator().validatesOrChecksProofs())
        return length;
    LBasicBlock isInvalid = newBlock();
    LBasicBlock isValid = newBlock();
    branch(equal(length, elements().publicLength(storage)), { normalEdge(isValid), rareEdge(isInvalid) });

    m_out.appendTo(isInvalid);
    callVoidOperation(value, RuntimeOperation::ReportValidationFailure, { boxCell(storage.array), word32(static_cast<int32_t>(ValidationFailureKind::ValueType)), word32(m_id.index()), word32(value.id().index()) }, { .isOnRarePath = true });
    m_out.unreachable();

    m_out.appendTo(isValid);
    return length;
}

void ArrayIntrinsicEmitter::generateTryPutByValDirect(const ValueContext& value)
{
    const ArrayStorageForm& form = formToConstruct(value);
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    ValueID stored = value->valueOperand();
    LBasicBlock otherwise = newBlock();
    std::optional<Operand> rawElement = elements().rawElementToStore(form, stored, otherwise);
    if (!rawElement) {
        m_out.jump(otherwise);
        m_out.appendTo(otherwise);
        define(value, boolean(false));
        return;
    }

    Join<Condition> join(m_out);
    branchUnless(elements().hasWritableShape(elements().indexingMode(array, IndexingShapeAndWritabilityMask), form), otherwise);
    ArrayElements storage = elements().elementsOf(array);
    branchUnless(below(index, lengthOfArrayStoredInto(value, storage)), otherwise);
    elements().storeRawElement(form, storage, index, *rawElement);
    if (form.holdsCells)
        barrierAfterStore(value, array, *rawElement, typeOf(stored));
    join.add(boolean(true));

    m_out.appendTo(otherwise);
    join.add(boolean(false));
    define(value, join.finish());
}

void ArrayIntrinsicEmitter::generateTryPutIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    define(value, elements().tryStoreElement(value, array, index, value->valueOperand()));
}

// Asks what putInBounds() asks and answers false to a no, so it may store into an array that the program holds. A NaN that it stores reads as a hole.
void ArrayIntrinsicEmitter::generateTryPutDoubleIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    Operand rawElement = get(value->valueOperand());
    LBasicBlock otherwise = newBlock();
    Join<Condition> join(m_out);
    branchUnless(elements().hasWritableShape(elements().indexingMode(array, IndexingShapeAndWritabilityMask), doubles), otherwise);
    ArrayElements storage = elements().elementsOf(array);
    branchUnless(below(index, elements().publicLength(storage)), otherwise);
    elements().storeRawElement(doubles, storage, index, rawElement);
    join.add(boolean(true));

    m_out.appendTo(otherwise);
    join.add(boolean(false));
    define(value, join.finish());
}

// No poll and no barrier stands in the loop: it calls nothing, and whoever makes it runs the barrier behind it.
template<typename Functor>
void ArrayIntrinsicEmitter::forEachPositionBelow(Integer count, const Functor& functor)
{
    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock done = newBlock();
    ValueFromBlock start = m_out.anchor(integer(0).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer position(m_out.phi(B3::Int64, start));
    branch(below(position, count), { normalEdge(element), normalEdge(done) });

    m_out.appendTo(element);
    functor(position);
    m_out.addIncomingToPhi(position.value(), m_out.anchor(add(position, integer(1)).value()));
    m_out.jump(loop);

    m_out.appendTo(done);
}

// One comparison for the two forms that hold boxed values: the distance from the lower one is nothing or the one bit between them.
Condition ArrayIntrinsicEmitter::isOneOfBoxedForms(Word32 mode, IndexingType ofIntegers, IndexingType ofValues)
{
    IndexingType distance = ofValues - ofIntegers;
    RELEASE_ASSERT(ofValues > ofIntegers && hasOneBitSet(distance));
    return Condition(m_out.testIsZero32(m_out.sub(mode.value(), ofIntegers), ~static_cast<int32_t>(distance)));
}

// Into an array that the library has just made with room for boxed values and handed to nobody, out of one that the program holds. False where that one has storage of another kind,
// is shorter, or has a hole or undefined among them: the new array is then left empty, for its elements may never have been cleared. A number out of storage for doubles is boxed.
// Nothing has run since the array was made, so the collector has not marked it: no barrier.
void ArrayIntrinsicEmitter::generateTryCopyFilledElements(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    Integer count = integerOperand(value->countOperand());
    LBasicBlock holdsBoxedValues = newBlock();
    LBasicBlock holdsNoBoxedValues = newBlock();
    LBasicBlock copied = newBlock();
    LBasicBlock otherwise = newBlock();
    Join<Condition> join(m_out);
    Word32 shape = elements().indexingMode(source, arrayStorageTestMask);
    branch(isOneOfBoxedForms(shape, int32s.shape, contiguous.shape), { normalEdge(holdsBoxedValues), normalEdge(holdsNoBoxedValues) });

    m_out.appendTo(holdsBoxedValues);
    {
        ArrayElements from = elements().elementsOf(source);
        ArrayElements to = elements().elementsOf(array);
        branchUnless(belowOrEqual(count, elements().publicLength(from)), otherwise);
        forEachPositionBelow(count, [&](Integer position) {
            LBasicBlock mayBeGap = newBlock();
            LBasicBlock isValue = newBlock();
            Boxed word(m_out.load64(elements().elementOfUnknownForm(from, position)));
            // One test for the two: the hole is no bit at all, and undefined has two bits, one of which is null.
            static_assert(!contiguous.hole && JSValue::ValueUndefined == (JSValue::ValueNull | JSValue::UndefinedTag));
            m_out.branch(m_out.testIsZero64(word.value(), ~static_cast<int64_t>(JSValue::ValueUndefined)), rareEdge(mayBeGap), normalEdge(isValue));

            m_out.appendTo(mayBeGap);
            m_out.branch(m_out.equal(word.value(), m_out.constInt64(JSValue::ValueNull)), normalEdge(isValue), normalEdge(otherwise));

            m_out.appendTo(isValue);
            m_out.store64(word.value(), elements().elementOfUnknownForm(to, position));
        });
        m_out.jump(copied);
    }

    m_out.appendTo(holdsNoBoxedValues);
    {
        branchUnless(elements().hasShape(shape, doubles), otherwise);
        ArrayElements from = elements().elementsOf(source);
        ArrayElements to = elements().elementsOf(array);
        branchUnless(belowOrEqual(count, elements().publicLength(from)), otherwise);
        forEachPositionBelow(count, [&](Integer position) {
            Number number = elements().loadRawElement(doubles, from, position).as<Number>();
            // In this storage NaN means a hole.
            branchUnless(equal(number, number), otherwise);
            m_out.store64(elements().boxRawElement(doubles, number).value(), elements().elementOfUnknownForm(to, position));
        });
        m_out.jump(copied);
    }

    m_out.appendTo(copied);
    ensureStillAliveHere(source);
    join.add(boolean(true));

    m_out.appendTo(otherwise);
    elements().setPublicLength(elements().elementsOf(array), integer(0));
    join.add(boolean(false));
    define(value, join.finish());
}

// Out of an array of boxed values that the library made and filled, into one that the program holds. False, with nothing stored, where that one is shorter or has no writable storage
// of the three kinds. Storage for integers or doubles takes them as they are: whoever asks knows that they are such numbers then.
void ArrayIntrinsicEmitter::generateTryPutElements(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    Integer count = integerOperand(value->countOperand());
    LBasicBlock holdsBoxedValues = newBlock();
    LBasicBlock holdsNoBoxedValues = newBlock();
    LBasicBlock otherwise = newBlock();
    Join<Condition> join(m_out);
    Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    branch(isOneOfBoxedForms(mode, int32s.indexingType, contiguous.indexingType), { normalEdge(holdsBoxedValues), normalEdge(holdsNoBoxedValues) });

    m_out.appendTo(holdsBoxedValues);
    {
        ArrayElements from = elements().elementsOf(source);
        ArrayElements to = elements().elementsOf(array);
        branchUnless(belowOrEqual(count, elements().publicLength(to)), otherwise);
        forEachPositionBelow(count, [&](Integer position) {
            m_out.store64(m_out.load64(elements().elementOfUnknownForm(from, position)), elements().elementOfUnknownForm(to, position));
        });
        ensureStillAliveHere(source);
        emitter<BarrierEmitter>().barrier(value, array);
        join.add(boolean(true));
    }

    m_out.appendTo(holdsNoBoxedValues);
    {
        branchUnless(elements().hasWritableShape(mode, doubles), otherwise);
        ArrayElements from = elements().elementsOf(source);
        ArrayElements to = elements().elementsOf(array);
        branchUnless(belowOrEqual(count, elements().publicLength(to)), otherwise);
        forEachPositionBelow(count, [&](Integer position) {
            Boxed word(m_out.load64(elements().elementOfUnknownForm(from, position)));
            Number number = select(isInt32(word), toNumber(unboxInt32(word)), unboxDouble(word));
            elements().storeRawElement(doubles, to, position, number);
        });
        ensureStillAliveHere(source);
        join.add(boolean(true));
    }

    m_out.appendTo(otherwise);
    join.add(boolean(false));
    define(value, join.finish());
}

void ArrayIntrinsicEmitter::generateGrowNewArray(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::GrowNewArray, { in<CellPointer>(value->arrayOperand()), integerOperand(value->lengthOperand()) });
}

void ArrayIntrinsicEmitter::generateTruncateNewArray(const ValueContext& value)
{
    AOT_THRESHOLD(int64_t, smallUnusedCapacity, 8);
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer length = integerOperand(value->lengthOperand());
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    static_assert(ArrayWithInt32 + 2 == ArrayWithDouble && ArrayWithDouble + 2 == ArrayWithContiguous, "an array has no mode between these");
    branchUnless(belowOrEqual(sub(elements().indexingMode(array, IndexingShapeAndWritabilityMask), ArrayWithInt32), ArrayWithContiguous - ArrayWithInt32), slowPath);
    ArrayElements storage = elements().elementsOf(array);
    // Below a length that is above the capacity this wraps to more than any length.
    Integer unusedCapacity = sub(elements().publicLength(storage), length);
    Condition leavesLittleUnused = belowOrEqual(unusedCapacity, smallUnusedCapacity);
    branchUnless(logicalOr(leavesLittleUnused, belowOrEqual(unusedCapacity, length)), slowPath);
    elements().setPublicLength(storage, length);
    join.add(array);

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::TruncateNewArray, { array, low32(length) }, { .isOnRarePath = true }));
    define(value, join.finish());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
