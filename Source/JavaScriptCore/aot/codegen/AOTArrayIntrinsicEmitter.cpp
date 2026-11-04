/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBarrierEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "JSType.h"
#include <bit>
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

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
    // The three shapes are two apart and in the order of their kinds. The shape just below them gets 0, which is Other.
    static_assert(int32s.shape + 2 == doubles.shape && doubles.shape + 2 == contiguous.shape && int32s.shape >= 2);
    static_assert(!static_cast<int32_t>(ArrayStorage::Other) && static_cast<int32_t>(ArrayStorage::Int32) == 1 && static_cast<int32_t>(ArrayStorage::Double) == 2 && static_cast<int32_t>(ArrayStorage::Contiguous) == 3);
    Word32 shape = elements().indexingMode(in<CellPointer>(value->arrayOperand()), arrayStorageTestMask);
    Word32 number = shiftRightUnsigned(sub(shape, int32s.shape - 2), 1);
    define(value, select(belowOrEqual(number, static_cast<int32_t>(ArrayStorage::Contiguous)), number, word32(static_cast<int32_t>(ArrayStorage::Other))));
}

void ArrayIntrinsicEmitter::generateGetDoubleIndex(const ValueContext& value)
{
    static_assert(doubles.element == Representation::Double);
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    LBasicBlock otherwise = newBlock();
    Join<Number> join(m_out);
    branchUnless(elements().hasShape(elements().indexingMode(array, arrayStorageTestMask), doubles), otherwise);
    ArrayElements storage = elements().elementsOf(value, array);
    branchUnless(below(index, elements().publicLength(storage)), otherwise);
    join.add(elements().loadRawElement(doubles, storage, index).as<Number>());

    m_out.appendTo(otherwise);
    join.add(elements().rawHole(doubles).as<Number>());
    define(value, join.finish());
}

Boxed ArrayIntrinsicEmitter::contiguousElementOrEmpty(const ValueContext& access, CellPointer array, Integer index)
{
    static_assert(int32s.hole == contiguous.hole && int32s.element != Representation::Double && contiguous.element == Representation::JSValue);
    LBasicBlock otherwise = newBlock();
    Join<Boxed> join(m_out);
    Word32 shape = elements().indexingMode(array, arrayStorageTestMask);
    // One test covers both shapes: the distance from the lower one is zero or the single bit by which they differ.
    constexpr IndexingType distance = contiguous.shape - int32s.shape;
    static_assert(contiguous.shape > int32s.shape && hasOneBitSet(distance));
    branchUnless(Condition(m_out.testIsZero32(m_out.sub(shape.value(), int32s.shape), ~static_cast<int32_t>(distance))), otherwise);
    ArrayElements storage = elements().elementsOf(access, array);
    branchUnless(below(index, elements().publicLength(storage)), otherwise);
    Boxed element(m_out.load64(elements().elementOfUnknownForm(storage, index)));
    ensureStillAliveHere(array);
    join.add(element);

    m_out.appendTo(otherwise);
    join.add(elements().rawHole(contiguous).as<Boxed>());
    return join.finish();
}

LValue ArrayIntrinsicEmitter::elementFoundByTestInFront(const ValueContext& read)
{
    if (!generator().testThatFoundElement)
        return nullptr;
    auto behindNames = [&](ValueID id) {
        while (operationAt(id).opcode == Opcode::Narrowed)
            id = operationAt(id).valueOperand();
        return id;
    };
    const Operation& test = operationAt(generator().testThatFoundElement);
    if (behindNames(test.arrayOperand()) != behindNames(read->arrayOperand()) || behindNames(test.positionOperand()) != behindNames(read->positionOperand()))
        return nullptr;

    // Up the only way into the block, as far as the arm of the Branch that asked. Nothing stands on it but names, so nothing has written since the test.
    BlockID asked = m_code.blockOf(generator().testThatFoundElement);
    for (BlockID block = read.block();;) {
        for (ValueID id : m_code.valuesOf(block)) {
            if (id == read.id())
                break;
            if (operationAt(id).opcode != Opcode::Narrowed)
                return nullptr;
        }
        BlockID above = generator().singlePredecessorOf(block);
        if (!above)
            return nullptr;
        const Terminator& end = m_code.at(above).terminator;
        if (above == asked)
            return end.isConditionalBranch() && end.conditionOperand() == generator().testThatFoundElement && m_code.at(end.taken()).to == block ? generator().elementFound : nullptr;
        if (end.kind != TerminatorKind::Jump)
            return nullptr;
        block = above;
    }
}

void ArrayIntrinsicEmitter::generateHasContiguousIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    // Each test leads to the arm that finds nothing by itself. So the load stands on the only way into the other arm, and the read there takes what it gave: elementFoundByTestInFront().
    if (value.isBranchCondition()) {
        const BranchTargets& targets = value.branchTargets();
        Integer index = integerOperand(value->positionOperand());
        auto goOnIf = [&](Condition condition) {
            LBasicBlock next = newBlock();
            branch(condition, { normalEdge(next), targets.notTaken });
            m_out.appendToContinuation(next);
        };
        constexpr IndexingType distance = contiguous.shape - int32s.shape;
        goOnIf(Condition(m_out.testIsZero32(m_out.sub(elements().indexingMode(array, arrayStorageTestMask).value(), int32s.shape), ~static_cast<int32_t>(distance))));
        ArrayElements storage = elements().elementsOf(value, array);
        goOnIf(below(index, elements().publicLength(storage)));
        Boxed element(m_out.load64(elements().elementOfUnknownForm(storage, index)));
        ensureStillAliveHere(array);
        generator().testThatFoundElement = value.id();
        generator().elementFound = element.value();
        branch(isNotEmpty(element), targets);
        return;
    }
    Boxed element = contiguousElementOrEmpty(value, array, integerOperand(value->positionOperand()));
    define(value, isNotEmpty(element));
}

void ArrayIntrinsicEmitter::generateGetContiguousIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Boxed element = contiguousElementOrEmpty(value, array, integerOperand(value->positionOperand()));
    Condition isFilled = isNotEmpty(element);
    defineAs(value, select(isFilled, element, undefined()), typeOf(value.id()));
}

// The GetOwn and PutOwn operations check nothing: the library's code guarantees the storage kind and that the index is below the length.
void ArrayIntrinsicEmitter::generateGetOwnContiguousIndex(const ValueContext& value)
{
    if (LValue found = elementFoundByTestInFront(value)) {
        defineAs(value, Boxed(found), typeOf(value.id()));
        return;
    }
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(value, array);
    Boxed element(m_out.load64(elements().elementOfUnknownForm(storage, integerOperand(value->positionOperand()))));
    ensureStillAliveHere(array);
    defineAs(value, element, typeOf(value.id()));
}

void ArrayIntrinsicEmitter::generateGetOwnDoubleIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(value, array);
    define(value, elements().loadRawElement(doubles, storage, integerOperand(value->positionOperand())).as<Number>());
}

void ArrayIntrinsicEmitter::generateGetOwnInt32Index(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(value, array);
    define(value, Word32(m_out.load32(elements().elementOfUnknownForm(storage, integerOperand(value->positionOperand())))));
}

void ArrayIntrinsicEmitter::generatePutOwnContiguousIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID stored = value->valueOperand();
    Boxed rawElement = in<Boxed>(stored);
    // The storage may be Int32 storage: see loadBoxedElement().
    m_out.store64(rawElement.value(), elements().elementOfUnknownForm(elements().elementsOf(value, array), integerOperand(value->positionOperand())));
    barrierAfterStore(value, array, rawElement, typeOf(stored));
}

void ArrayIntrinsicEmitter::generatePutOwnDoubleIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    elements().storeRawElement(doubles, elements().elementsOf(value, array), integerOperand(value->positionOperand()), in<Number>(value->valueOperand()));
}

// The element already holds a boxed int32, so only the low 32 bits are written: the tag in the upper half is the same for every int32.
void ArrayIntrinsicEmitter::generatePutOwnInt32Index(const ValueContext& value)
{
    static_assert(std::endian::native == std::endian::little);
    CellPointer array = in<CellPointer>(value->arrayOperand());
    m_out.store32(low32(integerOperand(value->valueOperand())).value(), elements().elementOfUnknownForm(elements().elementsOf(value, array), integerOperand(value->positionOperand())));
}

void ArrayIntrinsicEmitter::putInBounds(const ValueContext& value, const ArrayStorageForm& form)
{
    RELEASE_ASSERT(form.kind != ArrayStorage::Int32);
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    ValueID stored = value->valueOperand();
    Operand rawElement = generator().getIn(stored, form.element);
    ArrayElements storage = elements().elementsOf(value, array);
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

void ArrayIntrinsicEmitter::newArrayOfHoles(const ValueContext& value, Integer length, ValueID source, const ArrayStorageForm& form, uint32_t flags)
{
    using ElementsBelowLength = ArrayAllocationEmitter::ElementsBelowLength;
    using LengthIsSetBy = ArrayAllocationEmitter::LengthIsSetBy;
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    ArrayAllocationEmitter allocation(generator());
    // The runtime operation sets this length too, unless `length` is a capacity.
    bool setsLengthBehindJoin = !(flags & NewArrayWithHolesFlag::lengthIsCapacity);
    join.add(allocation.allocateArrayOfHoles(value, length, form, slowPath, ElementsBelowLength::Cleared, setsLengthBehindJoin ? LengthIsSetBy::Caller : LengthIsSetBy::Allocation));

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
    static constexpr int64_t maxFusedArrayCapacity = 268435456;
    Integer capacity = integerOperand(value->lengthOperand());
    Integer maxCapacity = integer(maxFusedArrayCapacity);
    Integer length = select(below(capacity, maxCapacity), capacity, maxCapacity);
    newArrayOfHoles(value, length, value->sourceOperand(), formToConstruct(value), NewArrayWithHolesFlag::lengthIsCapacity);
}

Integer ArrayIntrinsicEmitter::lengthOfArrayStoredInto(const ValueContext& value, const ArrayElements& storage)
{
    if (!value->lengthOperand())
        return elements().publicLength(storage);
    return integerOperand(value->lengthOperand());
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

// The loop has no poll and no barrier: it calls nothing, and the caller runs any barrier after it.
template<typename Functor>
void ArrayIntrinsicEmitter::forEachPositionBelow(Integer count, const Functor& functor)
{
    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock done = newBlock();
    ValueFromBlock start = m_out.anchor(integer(0).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer position(m_out.phi(Machine::Int64, start));
    branch(below(position, count), { normalEdge(element), normalEdge(done) });

    m_out.appendTo(element);
    functor(position);
    m_out.addIncomingToPhi(position.value(), m_out.anchor(add(position, integer(1)).value()));
    m_out.jump(loop);

    m_out.appendTo(done);
}

// One test covers both forms that hold boxed values: the distance from the lower one is zero or the single bit by which they differ.
Condition ArrayIntrinsicEmitter::isOneOfBoxedForms(Word32 mode, IndexingType ofIntegers, IndexingType ofValues)
{
    IndexingType distance = ofValues - ofIntegers;
    RELEASE_ASSERT(ofValues > ofIntegers && hasOneBitSet(distance));
    return Condition(m_out.testIsZero32(m_out.sub(mode.value(), ofIntegers), ~static_cast<int32_t>(distance)));
}

// The library has just allocated `array` with contiguous storage for at least `count` elements, and nothing else can reach it yet. `source` belongs to the program.
// False if `source` has no Int32, Double or Contiguous storage, is shorter than `count`, or holds a hole or undefined below `count`. `array` then has length 0.
// No barrier: nothing has run since `array` was allocated, so the collector has not marked it.
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
            // One test catches a hole, which is 0, and undefined, which is two bits. It also catches null, which is one of those bits: mayBeGap lets it through.
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

// `source` holds boxed values that the library read from `array` earlier. `array` belongs to the program, whose code may have run since.
// The storage kind of an array only widens: Int32, Double, Contiguous. So if `array` is still Int32, every value is an int32 and none is tested.
// If `array` is Double now, it may have been Int32 then, so each value is tested and converted. False at the first value that is not a number: the library then stores all of them the ordinary way.
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
            branchUnless(isNumber(word), otherwise);
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
    static constexpr int64_t smallUnusedCapacity = 8;
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer length = integerOperand(value->lengthOperand());
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    static_assert(ArrayWithInt32 + 2 == ArrayWithDouble && ArrayWithDouble + 2 == ArrayWithContiguous, "an array has no mode between these");
    branchUnless(belowOrEqual(sub(elements().indexingMode(array, IndexingShapeAndWritabilityMask), ArrayWithInt32), ArrayWithContiguous - ArrayWithInt32), slowPath);
    ArrayElements storage = elements().elementsOf(array);
    // If `length` exceeds the current length, the subtraction wraps to more than any length and both tests fail.
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
