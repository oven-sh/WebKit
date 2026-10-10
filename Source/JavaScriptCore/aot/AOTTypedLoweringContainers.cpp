/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTTypedCompactTable.h"
#include "B3BasicBlockInlines.h"
#include "B3StackmapGenerationParams.h"
#include "JSCInlines.h"
#include "PageCount.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// Arrays, typed arrays and iteration. For strings, maps and sets see AOTTypedLoweringStrings.cpp.
//
// That a receiver is an array or a typed array is what its type says, and is not tested. What is found out when
// the code runs is what no type says: whether it is `undefined` or `null`, how an array keeps its elements, the bounds, whether there is
// a hole. Where there is no element, `undefined` is read: the prototypes of the engine have no elements and cannot be given any.
//
// What a container holds nobody vouches for: it is held to the type of the value that reads it.
//
// An array was as the engine makes them when it was taken for one. Code that is not typed may have given it another prototype, a property
// of its own in the place of a method, or an accessor for an element since. Then it is of another JSType and keeps its elements in a
// way that nothing here does in place (JSObject::becomeDerivedArray()), so that it comes to the operations by itself wherever the way
// the elements are kept is looked at. Everywhere else isPlainArray() is asked. See AOTTypedOperations.h for what is done then.
//
// No path that reads runs code of the program, see keyedAccessOf(). So the back end keeps what it has loaded, and moves out of a loop what
// stays the same there: where the elements are, how many there are and how they are kept.
//
// A receiver that is only expected to be one of these is tested for it, see AOTTypedLoweringGeneric.cpp. Then only what the language says
// is done in place: an element that is there is read, an element is stored where the engine itself would just store it. The functions
// below take a block for that, `otherwise`: with it, whatever else there is to an access goes there, which is the general way.

// Changes elements and lengths, and runs no code of the program.
static constexpr OperationEffects effectsOfStoringElements { Writes::Heap, true, false };

// Of the typed arrays for which keyedAccessOf() says TypedArray.
static Store storeOfElements(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
        return Store::Int8;
    case TypeUint8:
    case TypeUint8Clamped:
        return Store::Uint8;
    case TypeInt16:
        return Store::Int16;
    case TypeUint16:
        return Store::Uint16;
    case TypeInt32:
        return Store::Int32;
    case TypeUint32:
        return Store::Uint32;
    case TypeFloat32:
        return Store::Float;
    case TypeFloat64:
        return Store::Double;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// As keyedAccessOf() has it.
static bool hasElementsReadInPlace(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
    case TypeUint8:
    case TypeUint8Clamped:
    case TypeInt16:
    case TypeUint16:
    case TypeInt32:
    case TypeUint32:
    case TypeFloat32:
    case TypeFloat64:
        return true;
    default:
        return false;
    }
}

// Operands.

// A key that is a number, or the count of the steps of an iteration, as an integer. It may still be negative or beyond every length.
Integer Lowering::indexOrElse(ValueID key, LBasicBlock otherwise)
{
    Operand operand = get(key);
    switch (operand.rep) {
    case Rep::Int64:
        return operand.as<Integer>();
    case Rep::OptionalInt64:
        orElse(logicalNot(isMarker(operand)), otherwise);
        return assumedInteger(operand.as<OptionalInteger>());
    case Rep::Double:
    // What stands for `undefined` or `null` is not a number, and equal to nothing.
    case Rep::OptionalDouble: {
        Number number = operand.rep == Rep::Double ? operand.as<Number>() : assumedNumber(operand.as<OptionalNumber>());
        Integer index = truncateToInteger(number);
        orElse(equal(convertToNumber(index), number), otherwise);
        return index;
    }
    case Rep::JSValue: {
        Boxed boxed = operand.as<Boxed>();
        LBasicBlock isSmall = m_out.newBlock();
        LBasicBlock isNotSmall = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isInt32(boxed).value(), usually(isSmall), rarely(isNotSmall));
        m_out.appendTo(isSmall);
        ValueFromBlock small = m_out.anchor(signExtended(low32(boxed)).value());
        m_out.jump(continuation);
        // The engine may keep an integral number as a double.
        m_out.appendTo(isNotSmall);
        orElse(isNumber(boxed), otherwise);
        Number number = unboxedDouble(boxed);
        Integer index = truncateToInteger(number);
        orElse(equal(convertToNumber(index), number), otherwise);
        ValueFromBlock large = m_out.anchor(index.value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return Integer(m_out.phi(Int64, small, large));
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// What is read where there is nothing. Nothing: `undefined` is not of the type, and the block has been ended with a TypeError.
std::optional<Operand> Lowering::absentElement(const Type& type)
{
    auto& undefinedType = m_program.at(m_program.types.undefined);
    if (type.primitives & Primitive::Undefined)
        return ensure(undefined(), undefinedType, type);
    throwTypeMismatch(undefined(), undefinedType, type);
    return std::nullopt;
}

Boxed Lowering::getByValGenerally(ValueID object, ValueID key)
{
    Boxed base = in<Boxed>(object);
    Operand operand = get(key);
    if (operand.rep == Rep::Int64)
        return Boxed(callICStub(Stub::GetByValAtIndex, Int64, { { base.value(), firstStubOperandGPR }, { operand.value, GPRInfo::argumentGPR1 } }));
    return emitGetByVal(base, convertTo<Boxed>(operand, typeOf(key)), !expectedTypeOf(key).isNumber());
}

void Lowering::putByValGenerally(ValueID object, ValueID key, ValueID stored)
{
    Boxed base = in<Boxed>(object);
    Operand operand = get(key);
    if (operand.rep != Rep::Int64) {
        Boxed boxedKey = convertTo<Boxed>(operand, typeOf(key));
        emitPutByVal(base, boxedKey, in<Boxed>(stored), !expectedTypeOf(key).isNumber());
        return;
    }
    constexpr uint32_t isStrict = 1;
    callICStub(Stub::PutByValAtIndex, Void, { { base.value(), firstStubOperandGPR }, { operand.value, GPRInfo::argumentGPR1 }, { in<Boxed>(stored).value(), GPRInfo::argumentGPR2 } }, std::pair { GPRInfo::argumentGPR3, isStrict });
}

// Arrays.

Pointer Lowering::butterflyOf(CellPointer object)
{
    return loadField<Pointer>(object, m_heaps.JSObject_butterfly, safeIfReaches(object, sizeof(JSObject)));
}

// Of the butterfly of an object that has indexed properties: no other has the lengths.
Integer Lowering::publicLength(Pointer butterfly)
{
    return zeroExtended(loadField<Word32>(butterfly, m_heaps.Butterfly_publicLength));
}

Integer Lowering::vectorLength(Pointer butterfly)
{
    return zeroExtended(loadField<Word32>(butterfly, m_heaps.Butterfly_vectorLength));
}

// Every array has a butterfly, with the length in the same place however the elements are kept.
Integer Lowering::arrayLength(CellPointer array)
{
    return publicLength(butterflyOf(array));
}

// Of what is an array for a fact, see EngineClass::type. Unlike that of any other cell, its JSType may change, once: typeOfCell() tells
// the back end that it never does, which will do wherever the two types of arrays are not told apart.
Word32 Lowering::isPlainArray(CellPointer array)
{
    return equal(loadByte(array, m_heaps.JSCell_typeInfoType, safeIfReaches(array, sizeof(JSCell))), word32(ArrayType));
}

// Of any cell: whether it is what an array that was taken for one becomes.
Word32 Lowering::isChangedArray(CellPointer cell)
{
    static_assert(ArrayStorageShape < SlowPutArrayStorageShape && (SlowPutArrayStorageShape | 2) == IndexingShapeMask);
    return logicalAnd(equal(loadByte(cell, m_heaps.JSCell_typeInfoType, safeIfReaches(cell, sizeof(JSCell))), word32(DerivedArrayType)), aboveOrEqual(indexingModeOf(cell, IndexingShapeMask), word32(ArrayStorageShape)));
}

Word32 Lowering::indexingModeOf(CellPointer array, unsigned mask)
{
    return bitAnd(loadByte(array, m_heaps.JSCell_indexingTypeAndMisc, safeIfReaches(array, sizeof(JSCell))), word32(mask));
}

// Whether the key of an access, going by its type and by what is expected of it, may be what indexOrElse() takes.
bool Lowering::mayBeIndex(ValueID key) const
{
    Rep rep = repOf(typeOf(key));
    if (rep != Rep::Int64 && rep != Rep::Double && rep != Rep::OptionalInt64 && rep != Rep::OptionalDouble && rep != Rep::JSValue)
        return false;
    return expectedTypeOf(key).primitives & Primitive::Int;
}

// Whether the element that is being read is expected to be a number: not where all that is done with it is a cast, right behind the read, to a type that has none. That decides which ways
// of keeping elements there is code in place for, and nothing else.
bool Lowering::isExpectedToBeNumber(ValueID element)
{
    if (!(expectedTypeOf(element).primitives & Primitive::Number))
        return false;
    auto& values = m_currentBlock->values;
    size_t next = m_currentIndexInBlock + 1;
    if (next >= values.size() || numberOfUsesOf(element) != 1)
        return true;
    auto& cast = m_function.at(values[next]);
    return cast.opcode != Opcode::Cast || cast.child(0) != element || typeOf(values[next]).primitives & Primitive::Number;
}

Operand Lowering::getArrayElement(CellPointer array, ValueID key, const Type& type, LBasicBlock otherwise, bool expectsNumbers)
{
    bool looksForNumbers = expectsNumbers && type.primitives & Primitive::Number;
    bool looksForNumbersLast = !type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
    Rep rep = repOf(type);
    LBasicBlock inside = m_out.newBlock();
    LBasicBlock outside = otherwise ? otherwise : m_out.newBlock();
    LBasicBlock absent = otherwise ? otherwise : m_out.newBlock();
    LBasicBlock slowPath = otherwise ? otherwise : newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    Integer index = indexOrElse(key, slowPath);
    Pointer butterfly = butterflyOf(array);
    m_out.branch(below(index, publicLength(butterfly)).value(), usually(inside), rarely(outside));

    // An array that shares its elements until it is stored to is read like any other.
    m_out.appendTo(inside);
    Word32 shape = indexingModeOf(array, IndexingShapeMask);
    // Goes on where the elements are kept another way. What is asked first is what the array was born as if it was made as its type says; the back end would
    // make one switch of the questions, in the order of the numbers, if it saw that they are about one value.
    bool isFirst = true;
    auto whenKeptAs = [&](IndexingType kept, const auto& read) {
        LBasicBlock isKeptSo = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        // Where more than a number will do, the first question is for values, and most arrays keep values.
        if (looksForNumbers && (!isFirst || !looksForNumbersLast))
            m_out.branch(equal(shape, word32(kept)).value(), unsure(isKeptSo), unsure(isNot));
        else
            m_out.branch(equal(shape, word32(kept)).value(), usually(isKeptSo), rarely(isNot));
        m_out.appendTo(isKeptSo);
        read();
        m_out.appendTo(isNot);
        if (std::exchange(isFirst, false))
            shape = Word32(m_out.opaque(shape.value()));
    };
    auto readValue = [&] {
        Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
        addIfNumber(join, element, type);
        orElse(isNotEmpty(element), absent);
        join.add(ensureElement(array, index, element, type));
    };
    if (looksForNumbersLast)
        whenKeptAs(ContiguousShape, readValue);
    // Kept as numbers, they are only looked at where a number will do and one is expected. All else is left to the operation.
    // Where values are what is expected, and outside loops, numbers are left to the stub that reads any array at an index: the arms are five times the code of the read.
    bool leavesNumbersToStub = looksForNumbers && looksForNumbersLast && !otherwise && !m_isInLoop;
    if (looksForNumbers && !leavesNumbersToStub) {
        whenKeptAs(DoubleShape, [&] {
            Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
            // Among doubles a hole is what is not a number, and nothing else is.
            orElse(equal(asDouble, asDouble), absent);
            join.add(elementOfDoubles(asDouble, type));
        });
        whenKeptAs(Int32Shape, [&] {
            Boxed asInt32(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
            orElse(isNotEmpty(asInt32), absent);
            auto& int32Type = m_program.at(m_program.types.int32);
            if (rep == Rep::JSValue)
                join.add(ensure(asInt32, int32Type, type));
            else
                join.add(ensure(signExtended(low32(asInt32)), int32Type, type));
        });
    }
    if (!looksForNumbersLast)
        whenKeptAs(ContiguousShape, readValue);
    if (leavesNumbersToStub)
        join.add(ensure(Boxed(callICStub(Stub::GetByValAtIndex, Int64, { { array.value(), firstStubOperandGPR }, { index.value(), GPRInfo::argumentGPR1 } }, std::nullopt, effectsOfOperation<operationAOTTypedArrayGet>())), type));
    else
        m_out.jump(slowPath);
    if (otherwise)
        return join.finish();

    // From its length on an array has no elements, and inherits none as long as it is as it was made. What is no index may be the name of a property.
    m_out.appendTo(outside);
    m_out.branch(logicalAnd(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), isPlainArray(array)).value(), usually(absent), rarely(slowPath));
    m_out.appendTo(absent);
    if (auto nothing = absentElement(type))
        join.add(*nothing);

    m_out.appendTo(slowPath);
    // To box a double takes thirteen instructions, and the operation would undo it.
    if (get(key).rep == Rep::Double)
        join.add(ensure(AOT_TYPED_OPERATION(ArrayGetAtNumber, array, in<Number>(key)), type));
    else
        join.add(ensure(AOT_TYPED_OPERATION(ArrayGet, array, in<Boxed>(key)), type));
    return join.finish();
}

// Stores an element of which it is not known whether it is a number: that is looked at where the array keeps numbers. See storeArrayElement().
template<typename Functor>
void Lowering::storeBoxedArrayElement(CellPointer array, std::optional<Integer> index, Boxed stored, LBasicBlock otherwise, const Functor& afterwards)
{
    Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    Integer place = index.value_or(length);
    // Once nothing can get in the way of the store. What is between the length and the capacity is kept clear, so that it reads as holes
    // once the array is that long.
    auto makeRoom = [&] {
        LBasicBlock isWithinLength = m_out.newBlock();
        if (index) {
            LBasicBlock isBeyondLength = m_out.newBlock();
            m_out.branch(below(place, length).value(), unsure(isWithinLength), unsure(isBeyondLength));
            m_out.appendTo(isBeyondLength);
        }
        orElse(below(place, vectorLength(butterfly)), otherwise);
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(add(place, integer(1))));
        m_out.jump(isWithinLength);
        m_out.appendTo(isWithinLength);
    };
    // Array.push: an array that is handed anything is expected to keep values. The stub has the arrays that keep numbers: generatePushToFullVector().
    if (!index) {
        orElse(equal(mode, word32(ContiguousShape)), otherwise);
        orElse(isBeyondBarrierThreshold(array), otherwise);
        makeRoom();
        m_out.store64(stored.value(), m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), place.value()));
        afterwards(place);
        return;
    }
    LBasicBlock values = m_out.newBlock();
    LBasicBlock notValues = m_out.newBlock();
    LBasicBlock int32s = m_out.newBlock();
    LBasicBlock notInt32s = m_out.newBlock();
    m_out.branch(equal(mode, word32(ContiguousShape)).value(), unsure(values), unsure(notValues));
    m_out.appendTo(notValues);
    m_out.branch(equal(mode, word32(Int32Shape)).value(), unsure(int32s), unsure(notInt32s));
    m_out.appendTo(int32s);
    orElse(isInt32(stored), otherwise);
    m_out.jump(values);

    m_out.appendTo(notInt32s);
    orElse(equal(mode, word32(DoubleShape)), otherwise);
    orElse(isNumber(stored), otherwise);
    Number number = select(isInt32(stored), toNumber(low32(stored)), unboxedDouble(stored));
    // Among doubles it would read as a hole.
    orElse(equal(number, number), otherwise);
    makeRoom();
    m_out.storeDouble(number.value(), m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), place.value()));
    afterwards(place);

    m_out.appendTo(values);
    makeRoom();
    m_out.store64(stored.value(), m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), place.value()));
    barrier(array, { { stored } });
    afterwards(place);
}

// Stores an element where that takes no more than a store, and making the array longer within the room it has. Without an index: after
// the last element. `afterwards` is handed the index and ends the block. To `otherwise`, with the array as it was, for all else.
template<typename Functor>
void Lowering::storeArrayElement(CellPointer array, std::optional<Integer> index, ValueID stored, LBasicBlock otherwise, const Functor& afterwards)
{
    auto& type = typeOf(stored);
    if (Operand operand = get(stored); operand.rep == Rep::JSValue && type.primitives & Primitive::Number) {
        storeBoxedArrayElement(array, index, operand.as<Boxed>(), otherwise, afterwards);
        return;
    }
    Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    Integer place = index.value_or(length);
    // Once nothing can get in the way of the store. What is between the length and the capacity is kept clear, so that it reads as holes
    // once the array is that long.
    auto makeRoom = [&] {
        LBasicBlock isWithinLength = m_out.newBlock();
        if (index) {
            LBasicBlock isBeyondLength = m_out.newBlock();
            m_out.branch(below(place, length).value(), unsure(isWithinLength), unsure(isBeyondLength));
            m_out.appendTo(isBeyondLength);
        }
        orElse(below(place, vectorLength(butterfly)), otherwise);
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(add(place, integer(1))));
        m_out.jump(isWithinLength);
        m_out.appendTo(isWithinLength);
    };
    // An array of numbers is born keeping doubles.
    if (type.isNumber()) {
        LBasicBlock doubles = m_out.newBlock();
        LBasicBlock notDoubles = m_out.newBlock();
        m_out.branch(equal(mode, word32(DoubleShape)).value(), unsure(doubles), unsure(notDoubles));
        m_out.appendTo(doubles);
        Number number = in<Number>(stored);
        // Among doubles it would read as a hole.
        if (!type.isInteger())
            orElse(equal(number, number), otherwise);
        makeRoom();
        m_out.storeDouble(number.value(), m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), place.value()));
        afterwards(place);
        m_out.appendTo(notDoubles);
    }
    Word32 isTaken = equal(mode, word32(ContiguousShape));
    if (type.isInteger() && type.integers.isWithin(Range::int32()))
        isTaken = logicalOr(isTaken, equal(mode, word32(Int32Shape)));
    orElse(isTaken, otherwise);
    // Array.push: an array that has to be told of the store is left to the stub with all else, so that there is one way out.
    if (!index && type.hasCells())
        orElse(isBeyondBarrierThreshold(array), otherwise);
    makeRoom();
    m_out.store64(in<Boxed>(stored).value(), m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), place.value()));
    if (index && type.hasCells())
        barrier(array, { { in<Boxed>(stored) } });
    afterwards(place);
}

void Lowering::putArrayElement(CellPointer array, ValueID key, ValueID stored, LBasicBlock otherwise)
{
    LBasicBlock slowPath = otherwise ? otherwise : newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Integer index = indexOrElse(key, slowPath);
    storeArrayElement(array, index, stored, slowPath, [&](Integer) {
        m_out.jump(continuation);
    });
    if (!otherwise) {
        m_out.appendTo(slowPath);
        if (get(key).rep == Rep::Double)
            AOT_TYPED_OPERATION(ArrayPutAtNumber, array, in<Boxed>(stored), in<Number>(key));
        else
            AOT_TYPED_OPERATION(ArrayPut, array, in<Boxed>(key), in<Boxed>(stored));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
}

// Whether it was stored. If not, the array is as it was.
Word32 Lowering::tryStoreArrayElement(CellPointer array, Integer index, ValueID stored)
{
    LBasicBlock otherwise = newUnlikelyBlock();
    Join join(m_out, Rep::Int32, Int32);
    storeArrayElement(array, index, stored, otherwise, [&](Integer) {
        join.add(boolean(true));
    });
    m_out.appendTo(otherwise);
    join.add(boolean(false));
    return join.finish().as<Word32>();
}

// Yields the new length, or what a function that has taken the place of the engine's yields.
Operand Lowering::pushArrayElement(CellPointer array, ValueID receiver, ValueID pushed, const Type& type)
{
    Rep rep = repOf(type);
    Type lengthType = Type::integer(Range::uint32());
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    didEmitWrite(Writes::Heap);
    storeArrayElement(array, std::nullopt, pushed, slowPath, [&](Integer place) {
        join.add(ensure(add(place, integer(1)), lengthType, type));
    });
    m_out.appendTo(slowPath);
    updateVectorLengthHint(array, receiver);
    static_assert(static_cast<unsigned>(Stub::PushToFullVector) + 1 == static_cast<unsigned>(Stub::LeafPushToFullVector));
    Boxed newLength(callAllocationStub(Stub::PushToFullVector, { { array.value(), GPRInfo::toArgumentRegister(1) }, { in<Boxed>(pushed).value(), GPRInfo::toArgumentRegister(2) }, { m_out.zeroExtPtr(turnsLeftInLoop().value()), GPRInfo::toArgumentRegister(3) } }, effectsOfOperation<operationAOTTypedArrayPush>(), true));
    join.add(ensure(newLength, type));
    return join.finish();
}

// `array.push(a, b, ..)`. All of them are stored, or none and the call is made by name.
Operand Lowering::pushArrayElements(CellPointer array, ValueID receiver, const Arguments& pushed, StringID name, const Type& type)
{
    Rep rep = repOf(type);
    Type lengthType = Type::integer(Range::uint32());
    bool areNumbers = true;
    bool areInt32 = true;
    bool haveCells = false;
    for (ValueID id : pushed) {
        auto& element = typeOf(id);
        areNumbers &= element.isNumber();
        areInt32 &= element.isInteger() && element.integers.isWithin(Range::int32());
        haveCells |= element.hasCells();
    }
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    didEmitWrite(Writes::Heap);
    Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    Integer newLength = add(length, integer(pushed.size()));
    // Where there is none the vector grows and all goes on here. Yields where the elements are then.
    auto makeRoom = [&] {
        LBasicBlock hasRoom = m_out.newBlock();
        LBasicBlock isFull = newColdBlock();
        ValueFromBlock asItIs = m_out.anchor(butterfly.value());
        m_out.branch(belowOrEqual(newLength, vectorLength(butterfly)).value(), usually(hasRoom), rarely(isFull));
        m_out.appendTo(isFull);
        updateVectorLengthHint(array, receiver);
        orElse(logicalNot(isZero(AOT_TYPED_OPERATION(ArrayGrow, array, low32(newLength)))), slowPath);
        ValueFromBlock grown = m_out.anchor(butterflyOf(array).value());
        m_out.jump(hasRoom);
        m_out.appendTo(hasRoom);
        Pointer elements(m_out.phi(pointerType(), asItIs, grown));
        storeField(elements, m_heaps.Butterfly_publicLength, low32(newLength));
        return elements;
    };
    auto placeOf = [&](unsigned i) {
        return i ? add(length, integer(i)) : length;
    };
    if (areNumbers) {
        LBasicBlock doubles = m_out.newBlock();
        LBasicBlock notDoubles = m_out.newBlock();
        m_out.branch(equal(mode, word32(DoubleShape)).value(), unsure(doubles), unsure(notDoubles));
        m_out.appendTo(doubles);
        Vector<Number, 8> numbers;
        for (ValueID id : pushed) {
            numbers.append(in<Number>(id));
            if (!typeOf(id).isInteger())
                orElse(equal(numbers.last(), numbers.last()), slowPath);
        }
        Pointer elements = makeRoom();
        for (unsigned i = 0; i < pushed.size(); ++i)
            m_out.storeDouble(numbers[i].value(), m_out.baseIndex(m_heaps.indexedDoubleProperties, elements.value(), placeOf(i).value()));
        join.add(ensure(newLength, lengthType, type));
        m_out.appendTo(notDoubles);
    }
    Word32 isTaken = equal(mode, word32(ContiguousShape));
    if (areInt32)
        isTaken = logicalOr(isTaken, equal(mode, word32(Int32Shape)));
    orElse(isTaken, slowPath);
    Vector<Boxed, 8> values;
    for (ValueID id : pushed)
        values.append(in<Boxed>(id));
    Pointer elements = makeRoom();
    for (unsigned i = 0; i < pushed.size(); ++i)
        m_out.store64(values[i].value(), m_out.baseIndex(m_heaps.indexedContiguousProperties, elements.value(), placeOf(i).value()));
    if (haveCells)
        barrier(array, values.span());
    join.add(ensure(newLength, lengthType, type));

    m_out.appendTo(slowPath);
    Boxed result = callMethodGenerally(box(array), name, pushed);
    join.add(ensure(result, type));
    return join.finish();
}

// The vector of a typed array and the characters of a string are freed with their owner, and a pointer to them keeps nothing alive. The back
// end holds on to one across calls that collect, so after each use of one the collector still has to find the owner. No code comes of this.
bool Lowering::isDroppedForTesting(const char* file, unsigned line)
{
    if (!Options::aotGuardToDropForTesting()) [[likely]]
        return false;
    StringView path = StringView::fromLatin1(file);
    size_t slash = path.reverseFind('/');
    return makeString(slash == notFound ? path : path.substring(slash + 1), ':', line) == StringView::fromLatin1(byteCast<char>(Options::aotGuardToDropForTesting()));
}

void Lowering::ensureStillAliveHere(CellPointer owner, const char* file, unsigned line)
{
    if (isDroppedForTesting(file, line)) [[unlikely]]
        return;
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    patchpoint->effects = Effects::none();
    patchpoint->effects.writesLocalState = true;
    patchpoint->effects.reads = HeapRange::top();
    patchpoint->append(owner.value(), ValueRep::ColdAny);
    patchpoint->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
}

// Typed arrays.

// The receiver of an access by a type that says it is a typed array, `undefined` or `null`. See findTypedArrayElement() for the last two.
Lowering::TypedArrayReceiver Lowering::typedArrayReceiver(ValueID object)
{
    Operand operand = get(object);
    if (operand.rep == Rep::Cell)
        return typedArrayReceiver(operand.as<CellPointer>());
    // In the place of what is no view, something is loaded from that is always there. What is found there is not looked at.
    static_assert(sizeof(Instance) >= sizeof(JSArrayBufferView));
    Boxed value = operand.as<Boxed>();
    Word32 isView = isCell(value);
    return { CellPointer(m_out.select(isView.value(), value.value(), m_instance)), IsSafe, isView, value };
}

Lowering::TypedArrayReceiver Lowering::typedArrayReceiver(CellPointer view)
{
    return { view, safeIfReaches(view, sizeof(JSArrayBufferView)), { }, { } };
}

void Lowering::turnAwayNil(const TypedArrayReceiver& receiver)
{
    if (!receiver.isView)
        return;
    LBasicBlock isNil = newColdBlock();
    LBasicBlock continuation = newColdBlock();
    m_out.branch(receiver.isView.value(), usually(continuation), rarely(isNil));
    m_out.appendTo(isNil);
    throwNullish(receiver.value, { });
    m_out.appendTo(continuation);
}

// Otherwise the length depends on that of the buffer. Of the mode only these two bits are looked at, which stay as the view was made with them.
Word32 Lowering::hasLengthOfItsOwn(const TypedArrayReceiver& receiver)
{
    return lacksBits(loadByte(receiver.view, m_heaps.JSArrayBufferView_mode, IsFixed | receiver.flags), isResizableOrGrowableSharedMode);
}

// Goes on where the index is inside, and yields where the elements are. Of a typed array that has been detached the length is zero.
Pointer Lowering::findTypedArrayElement(const TypedArrayReceiver& receiver, Integer index, LBasicBlock otherwise, LBasicBlock outside)
{
    Word32 hasOwnLength = hasLengthOfItsOwn(receiver);
    // One test in the usual case: `undefined`, `null` and a view that goes by its buffer count as empty. None of this changes around a loop that only reads and stores elements.
    Word32 isAsLongAsItSays = receiver.isView ? logicalAnd(receiver.isView, hasOwnLength) : hasOwnLength;
    Integer length = select(isAsLongAsItSays, loadField<Integer>(receiver.view, m_heaps.JSArrayBufferView_length, receiver.flags), integer(0));
    LBasicBlock inside = m_out.newBlock();
    LBasicBlock notInside = newColdBlock();
    m_out.branch(below(index, length).value(), usually(inside), rarely(notInside));
    m_out.appendTo(notInside);
    turnAwayNil(receiver);
    m_out.branch(hasOwnLength.value(), usually(outside), rarely(otherwise));
    m_out.appendTo(inside);
    return loadField<Pointer>(receiver.view, m_heaps.JSArrayBufferView_vector, receiver.flags);
}

Operand Lowering::getTypedArrayElement(const TypedArrayReceiver& receiver, ValueID key, TypedArrayType arrayType, const Type& type, LBasicBlock otherwise)
{
    Rep rep = repOf(type);
    Type elementType = elementTypeOf(arrayType);
    LBasicBlock slowPath = otherwise ? otherwise : newColdBlock();
    LBasicBlock outside = newColdBlock();
    LBasicBlock notIndex = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    // No number is the name of a property of a typed array that is looked for anywhere else. A key that is only expected to be one may be any name.
    Integer index = indexOrElse(key, otherwise ? otherwise : notIndex);
    Pointer vector = findTypedArrayElement(receiver, index, slowPath, outside);
    Operand element = load(storeOfElements(arrayType), TypedPointer(m_heaps.TypedArrayProperties, offsetBy(vector, index, logElementSize(arrayType)).value()), 0);
    element = convert(element, elementType, repOf(elementType));
    ensureStillAliveHere(receiver.view);
    join.add(ensure(element, elementType, type));

    m_out.appendTo(notIndex);
    turnAwayNil(receiver);
    m_out.jump(outside);
    m_out.appendTo(outside);
    if (auto nothing = absentElement(type))
        join.add(*nothing);
    if (otherwise)
        return join.finish();
    m_out.appendTo(slowPath);
    join.add(ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(GetByKey, (OperationEffects { Writes::Nothing, false }), box(receiver.view), in<Boxed>(key)), type));
    return join.finish();
}

void Lowering::putTypedArrayElement(const TypedArrayReceiver& receiver, ValueID key, TypedArrayType arrayType, ValueID stored, LBasicBlock otherwise)
{
    Store store = storeOfElements(arrayType);
    LBasicBlock slowPath = otherwise ? otherwise : newColdBlock();
    LBasicBlock notIndex = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    // A store outside does nothing.
    Integer index = indexOrElse(key, otherwise ? otherwise : notIndex);
    // What is stored is made a number of first, which only what is one already leaves to be done in place.
    Operand element;
    if (store == Store::Double || store == Store::Float)
        element = otherwise ? expectOrElse(Expectation { stored, &m_program.at(m_program.types.number) }, otherwise) : Operand(in<Number>(stored));
    else {
        Word32 bits = otherwise ? toInt32OrElse(stored, otherwise) : toInt32(stored);
        element = store == Store::Uint32 ? Operand(zeroExtended(bits)) : Operand(bits);
    }
    Pointer vector = findTypedArrayElement(receiver, index, slowPath, continuation);
    this->store(store, element, TypedPointer(m_heaps.TypedArrayProperties, offsetBy(vector, index, logElementSize(arrayType)).value()));
    ensureStillAliveHere(receiver.view);
    m_out.jump(continuation);
    m_out.appendTo(notIndex);
    turnAwayNil(receiver);
    m_out.jump(continuation);
    if (!otherwise) {
        m_out.appendTo(slowPath);
        AOT_TYPED_OPERATION_WITH_EFFECTS(TypedArrayPut, (OperationEffects { Writes::TypedArrayProperties, false, false, false, true }), receiver.view, index, in<Number>(stored));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
}

// Access by value.

Operand Lowering::lowerGetByVal(ValueID result, const Value& value)
{
    ValueID object = value.child(0);
    ValueID key = value.child(1);
    auto& type = typeOf(result);
    switch (keyedAccessOf(m_program, m_function, value)) {
    case KeyedAccess::Array:
        return getArrayElement(nonNil(object, { }), key, type, nullptr, isExpectedToBeNumber(result));
    case KeyedAccess::TypedArray:
        return getTypedArrayElement(typedArrayReceiver(object), key, typeOf(object).onlyEngineClass()->typedArrayType, type);
    case KeyedAccess::String:
        return getStringElement(stringReceiver(object), key, type);
    case KeyedAccess::Generic:
        break;
    }
    auto generally = [&] {
        return ensure(getByValGenerally(object, key), type);
    };
    // The receiver, or the key, is only expected to be what an access in place takes.
    if (!mayBeIndex(key))
        return generally();
    Type expected = expectedTypeOf(object).withoutPrimitives(Primitive::UndefinedOrNull);
    auto inPlace = [&](const auto& read) {
        return ifAsExpected({ { object, &expected } }, repOf(type), [&](LBasicBlock otherwise) {
            return read(get(object).as<CellPointer>(), otherwise);
        }, generally);
    };
    if (expected.isString()) {
        return inPlace([&](CellPointer string, LBasicBlock otherwise) {
            return getStringElement(stringReceiver(string), key, type, otherwise);
        });
    }
    auto* engineClass = expected.onlyEngineClass();
    if (!engineClass)
        return generally();
    if (engineClass->type == ArrayType) {
        bool expectsNumbers = (!expected.elements || m_program.at(expected.elements).primitives & Primitive::Number) && isExpectedToBeNumber(result);
        return inPlace([&](CellPointer array, LBasicBlock otherwise) {
            return getArrayElement(array, key, type, otherwise, expectsNumbers);
        });
    }
    if (hasElementsReadInPlace(engineClass->typedArrayType)) {
        return inPlace([&](CellPointer view, LBasicBlock otherwise) {
            return getTypedArrayElement(typedArrayReceiver(view), key, engineClass->typedArrayType, type, otherwise);
        });
    }
    return generally();
}

void Lowering::lowerPutByVal(const Value& value)
{
    if (auto* run = propertyRunAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == run->stores.last().index)
            lowerPropertyRun(*run);
        return;
    }
    ValueID object = value.child(0);
    ValueID key = value.child(1);
    ValueID stored = value.child(2);
    KeyedAccess access = keyedAccessOf(m_program, m_function, value);
    didEmitWrite(access == KeyedAccess::TypedArray ? Writes::TypedArrayProperties : Writes::Heap);
    switch (access) {
    case KeyedAccess::Array:
        putArrayElement(nonNil(object, { }), key, stored);
        return;
    case KeyedAccess::TypedArray:
        putTypedArrayElement(typedArrayReceiver(object), key, typeOf(object).onlyEngineClass()->typedArrayType, stored);
        return;
    case KeyedAccess::String:
    case KeyedAccess::Generic:
        break;
    }
    auto generally = [&]() -> Operand {
        putByValGenerally(object, key, stored);
        return { };
    };
    Type expected = expectedTypeOf(object).withoutPrimitives(Primitive::UndefinedOrNull);
    auto* engineClass = expected.onlyEngineClass();
    if (!engineClass || !mayBeIndex(key)) {
        generally();
        return;
    }
    auto inPlace = [&](const auto& write) {
        ifAsExpected({ { object, &expected } }, Rep::Void, [&](LBasicBlock otherwise) -> Operand {
            write(get(object).as<CellPointer>(), otherwise);
            return { };
        }, generally);
    };
    if (engineClass->type == ArrayType) {
        inPlace([&](CellPointer array, LBasicBlock otherwise) {
            putArrayElement(array, key, stored, otherwise);
        });
        return;
    }
    // Rounding to a clamped byte is left to the engine.
    bool mayStoreNumber = typeOf(stored).isNumber() || (repOf(typeOf(stored)) == Rep::JSValue && expectedTypeOf(stored).primitives & Primitive::Number);
    if (hasElementsReadInPlace(engineClass->typedArrayType) && engineClass->typedArrayType != TypeUint8Clamped && mayStoreNumber) {
        inPlace([&](CellPointer view, LBasicBlock otherwise) {
            putTypedArrayElement(typedArrayReceiver(view), key, engineClass->typedArrayType, stored, otherwise);
        });
        return;
    }
    generally();
}

// Iteration.
//
// An array whose iteration nothing can observe stands for its own iterator, and compiled code steps through it by index. Every value
// that is typed as an array is one. Of any other value operationAOTTypedIterOpen finds out: what it returns is such an array, or a
// record of the iterator, which is a cell of another type. Where it does not show how the iterator was made, that is looked at.
// Of a value that is expected to be an array it is found out in place first. An array that was not typed as one is asked for its
// elements the way the language has it: one that is there is read, for anything else the array is asked the general way.

// It was of ArrayType when the iteration was opened.
Word32 Lowering::isArrayStandingForItsIterator(ValueID iterator)
{
    static_assert(DerivedArrayType == ArrayType + 1);
    return below(binary<Word32>(&Output::sub, typeOfCell(asCell(in<Boxed>(iterator))), word32(ArrayType)), word32(2));
}

// A string stands for its own iterator too: its iteration cannot be replaced either, and has nothing to close. It goes by code points, so that where it is is not what the steps count: that is kept in a variable of the
// back end's, for an iterator that nothing uses but its own IteratorHasNext, IteratorValue and IteratorClose.
void Lowering::findIterationsOfStrings()
{
    Vector<std::pair<ValueID, bool>, 4> found;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::IteratorOpen)
                continue;
            auto& type = m_program.at(m_function.at(value.child(0)).type);
            bool isCertain = type.primitives & Primitive::String && type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
            auto expected = predictionOf(value.child(0));
            if (isCertain || (expected && expected->isString()))
                found.append({ id, isCertain });
        }
    }
    if (found.isEmpty())
        return;
    BitVector hasOtherUse;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& user = m_function.at(id);
            bool stepsThrough = user.opcode == Opcode::IteratorHasNext || user.opcode == Opcode::IteratorValue || user.opcode == Opcode::IteratorClose;
            unsigned index = 0;
            user.forEachChild([&](ValueID child) {
                if (!stepsThrough || index++)
                    hasOtherUse.set(child.index());
            });
        }
        if (block.terminator.value)
            hasOtherUse.set(block.terminator.value.index());
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                hasOtherUse.set(argument.index());
        }
    }
    for (auto [id, isCertain] : found) {
        if (!hasOtherUse.get(id.index()))
            m_iterationsOfStrings.add(id.index(), IterationOfString { m_proc.addVariable(Int64), m_proc.addVariable(Int64), m_proc.addVariable(Int64), m_proc.addVariable(Int32), isCertain, m_catchEntrypoints.isEmpty() ? nullptr : m_out.lockedStackSlot(4 * sizeof(Register)) });
    }
}

// The position is kept in a variable of the back end's, and so is the rest. What is defined ahead of the entry of a handler is not there behind it: see define() and lowerCatchEntrypoint().
void Lowering::setAndKeep(const IterationOfString& iteration, B3::Variable* variable, LValue value)
{
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), variable, value);
    if (!iteration.kept)
        return;
    unsigned word = variable == iteration.position ? 0 : variable == iteration.data ? 1 : variable == iteration.length ? 2 : 3;
    TypedPointer pointer = m_out.address(m_heaps.AOTTyped_catchSpillSlots[m_catchSpillSlotCount + word], iteration.kept, word * sizeof(Register));
    if (variable == iteration.flags)
        m_out.store32(value, pointer);
    else
        m_out.store64(value, pointer);
}

void Lowering::restoreIterationsOfStrings()
{
    Vector<uint32_t, 4> iterators;
    for (uint32_t iterator : m_iterationsOfStrings.keys())
        iterators.append(iterator);
    std::ranges::sort(iterators);
    for (uint32_t iterator : iterators) {
        auto& iteration = m_iterationsOfStrings.find(iterator)->value;
        unsigned word = 0;
        for (B3::Variable* variable : { iteration.position, iteration.data, iteration.length, iteration.flags }) {
            TypedPointer pointer = m_out.address(m_heaps.AOTTyped_catchSpillSlots[m_catchSpillSlotCount + word], iteration.kept, word * sizeof(Register));
            ++word;
            m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), variable, variable == iteration.flags ? m_out.load32(pointer) : m_out.load64(pointer));
        }
    }
}

const Lowering::IterationOfString* Lowering::iterationOfString(ValueID iterator) const
{
    if (m_iterationsOfStrings.isEmpty())
        return nullptr;
    auto found = m_iterationsOfStrings.find(iterator.index());
    return found == m_iterationsOfStrings.end() ? nullptr : &found->value;
}

// What operationAOTTypedIterOpen yields is an array or a record, which is a cell of another type.
Word32 Lowering::isStringStandingForItsIterator(ValueID iterator)
{
    return equal(typeOfCell(asCell(in<Boxed>(iterator))), word32(StringType));
}

Integer Lowering::positionIn(const IterationOfString& iteration)
{
    return Integer(m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), iteration.position));
}

void Lowering::setPositionIn(const IterationOfString& iteration, Integer position)
{
    setAndKeep(iteration, iteration.position, position.value());
}

Operand Lowering::openIterationOfString(const IterationOfString& iteration, ValueID result, ValueID iterable)
{
    setPositionIn(iteration, integer(0));
    auto& type = typeOf(iterable);
    auto inPlace = [&](CellPointer string) {
        // Every rope is made flat here, once: a substring that is looked at in its base lets go of the base when something in the loop makes it flat.
        Pointer fiber = fiberOf(stringReceiver(string));
        Pointer impl = eitherPointer(lacksBits(fiber, JSString::isRopeInPointer), [&] {
            return fiber;
        }, [&] {
            return AOT_TYPED_OPERATION(Flatten, string);
        });
        auto set = [&](B3::Variable* variable, LValue value) {
            setAndKeep(iteration, variable, value);
        };
        set(iteration.data, loadField<Pointer>(impl, m_heaps.StringImpl_data).value());
        set(iteration.length, zeroExtended(loadField<Word32>(impl, m_heaps.StringImpl_length)).value());
        set(iteration.flags, loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags, IsFixed).value());
        ensureStillAliveHere(string);
        // The word that was read above may have said "rope": whoever reads it from here on does so through a pointer of its own.
        return box(CellPointer(m_out.opaque(string.value())));
    };
    if (type.isString())
        return ensure(inPlace(in<CellPointer>(iterable)), typeOf(result));
    Boxed boxed = in<Boxed>(iterable);
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock isStringCase = iteration.isCertain ? isCellCase : m_out.newBlock();
    LBasicBlock otherwise = newColdBlock();
    Join join(m_out, Rep::JSValue, Int64);
    m_out.branch((iteration.isCertain ? isCell(boxed, type) : isCell(boxed)).value(), usually(isCellCase), rarely(otherwise));
    m_out.appendTo(isCellCase);
    if (!iteration.isCertain) {
        m_out.branch(equal(typeOfCell(asCell(boxed)), word32(StringType)).value(), usually(isStringCase), rarely(otherwise));
        m_out.appendTo(isStringCase);
    }
    join.add(inPlace(asCell(boxed)));
    m_out.appendTo(otherwise);
    join.add(AOT_TYPED_OPERATION(IterOpen, boxed));
    return ensure(join.finish().as<Boxed>(), typeOf(result));
}

Lowering::Characters Lowering::charactersOfIteratedString(const IterationOfString& iteration, CellPointer string)
{
    auto get = [&](B3::Variable* variable) {
        return m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), variable);
    };
    return { Pointer(get(iteration.data)), Word32(get(iteration.flags)), Integer(get(iteration.length)), string };
}

Word32 Lowering::iteratedStringHasNext(const IterationOfString& iteration, ValueID iterator)
{
    return below(positionIn(iteration), charactersOfIteratedString(iteration, asCell(in<Boxed>(iterator))).length);
}

// What the front end holds the value of an iteration to is the type of the loop's variable. The iteration of a string yields strings.
std::optional<Operand> Lowering::castOfIteratedCharacter(const Value& value)
{
    if (m_iterationsOfStrings.isEmpty() || value.opcode != Opcode::Cast || value.castReason() == CastReason::Coerce)
        return std::nullopt;
    auto& from = m_function.at(value.child(0));
    if (from.opcode != Opcode::IteratorValue || !(m_program.at(value.type).primitives & Primitive::String))
        return std::nullopt;
    auto* iteration = iterationOfString(from.child(0));
    if (!iteration || !iteration->isCertain)
        return std::nullopt;
    // There every value is looked at by itself.
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return std::nullopt;
    return convert(asCell(in<Boxed>(value.child(0))), m_program.at(m_program.types.string), value.rep);
}

// The position is inside.
CellPointer Lowering::nextOfIteratedString(const IterationOfString& iteration, ValueID iterator, Word32& codePoint)
{
    StringReceiver receiver = stringReceiver(asCell(in<Boxed>(iterator)));
    Integer position = positionIn(iteration);
    Characters characters = charactersOfIteratedString(iteration, receiver.string);
    LBasicBlock narrow = m_out.newBlock();
    LBasicBlock wide = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(hasBits(characters.flags, StringImpl::flagIs8Bit()).value(), unsure(narrow), unsure(wide));

    static_assert(maxSingleCharacterString == std::numeric_limits<Latin1Character>::max());
    m_out.appendTo(narrow);
    LValue unit = marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), offsetBy(characters.data, position, 0).value())), IsFixed);
    ensureStillAliveHere(receiver.string);
    setPositionIn(iteration, add(position, integer(1)));
    ValueFromBlock narrowUnit = m_out.anchor(unit);
    ValueFromBlock narrowString = m_out.anchor(singleCharacterString(unit));
    m_out.jump(continuation);

    m_out.appendTo(wide);
    Word32 wideCodePoint = codePointAt(characters, position);
    Integer next = add(position, select(below(word32(0xffff), wideCodePoint), integer(2), integer(1)));
    setPositionIn(iteration, next);
    CellPointer wideString = either<CellPointer>(belowOrEqual(wideCodePoint, word32(maxSingleCharacterString)), [&] {
        return CellPointer(singleCharacterString(wideCodePoint.value()));
    }, [&] {
        return partOfString(receiver, characters.length, position, next);
    });
    ValueFromBlock wideUnit = m_out.anchor(wideCodePoint.value());
    ValueFromBlock wideResult = m_out.anchor(wideString.value());
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    codePoint = Word32(m_out.phi(Int32, narrowUnit, wideUnit));
    return CellPointer(m_out.phi(Int64, narrowString, wideResult));
}

// `for (const [i, x] of array.entries())`. The call of `entries`, on an array that has its original structure, goes to one place: the IteratorOpen of a loop, straight or through parameters of blocks
// and upcasts that nothing else uses. Nothing uses that but its IteratorHasNext, IteratorValue and IteratorClose, and what each IteratorValue yields is only taken apart: by an IteratorOpen that
// nothing uses but IteratorHasNext and IteratorValue with constant indices and IteratorClose, or by casts that nothing uses but GetByVal with 0 or 1. Then nobody can tell whether there is an
// iterator, or a pair: the call makes a record (CollectionIteration::isOfEntriesOfArrayByIndex), and each of the others asks the iterator of the loop whether it is one: see partOfEntriesOfArray().
void Lowering::findEntriesOfArrays()
{
    Vector<ValueID, 2> calls;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::CallBuiltin)
                continue;
            auto& implementation = m_builtins.at(value.builtin()).implementation;
            if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ArrayEntries && value.arguments.isEmpty())
                calls.append(id);
        }
    }
    if (calls.isEmpty())
        return;

    // The value that has it for its first operand, or the parameter it is handed to, if that is its only use.
    auto whereItGoes = [&](ValueID id) -> ValueID {
        if (numberOfUsesOf(id) != 1)
            return { };
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                if (m_function.at(user).child(0) == id)
                    return user;
            }
            for (auto& target : block.terminator.targets) {
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (target.arguments[i] == id)
                        return m_function.at(target.block).parameters[i];
                }
            }
        }
        return { };
    };
    // Whether every use of the value is as the first operand of a value the functor accepts.
    auto isOnlyUsedBy = [&](ValueID id, const auto& accepts) {
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                unsigned index = 0;
                bool isAccepted = true;
                m_function.at(user).forEachChild([&](ValueID child) {
                    if (child == id && (index || !accepts(user, m_function.at(user))))
                        isAccepted = false;
                    ++index;
                });
                if (!isAccepted)
                    return false;
            }
            if (block.terminator.value == id)
                return false;
            for (auto& target : block.terminator.targets) {
                if (target.arguments.contains(id))
                    return false;
            }
        }
        return true;
    };
    auto isConstantIndex = [&](ValueID id, int64_t limit) {
        auto& index = m_function.at(id);
        return index.opcode == Opcode::Integer && index.integer() >= 0 && index.integer() < limit;
    };

    for (ValueID call : calls) {
        ValueID iterator = whereItGoes(call);
        for (unsigned steps = 0; steps < 4 && iterator; ++steps) {
            auto& value = m_function.at(iterator);
            if (value.opcode != Opcode::Parameter && (value.opcode != Opcode::Cast || value.castReason() != CastReason::Upcast))
                break;
            iterator = whereItGoes(iterator);
        }
        if (!iterator || m_function.at(iterator).opcode != Opcode::IteratorOpen || iterationOfString(iterator))
            continue;
        if (m_iterationsOfEntries.get(iterator.index())) {
            m_entriesMadeAsRecords.set(call.index());
            continue;
        }

        PairsOfEntries pairs;
        bool isTakenApart = isOnlyUsedBy(iterator, [&](ValueID user, const Value& step) {
            if (step.opcode == Opcode::IteratorValue)
                pairs.add(user.index(), PairOfEntries { iterator, user });
            return step.opcode == Opcode::IteratorHasNext || step.opcode == Opcode::IteratorValue || step.opcode == Opcode::IteratorClose;
        });
        Vector<ValueID, 2> values;
        for (auto& pair : pairs.values())
            values.append(pair.pair);
        for (ValueID pair : values) {
            if (!isTakenApart)
                break;
            isTakenApart = isOnlyUsedBy(pair, [&](ValueID user, const Value& use) {
                if (m_function.at(user).rep != Rep::JSValue)
                    return false;
                pairs.add(user.index(), PairOfEntries { iterator, pair });
                if (use.opcode == Opcode::IteratorOpen) {
                    return numberOfUsesOf(pair) == 1 && !iterationOfString(user) && isOnlyUsedBy(user, [&](ValueID, const Value& step) {
                        if (step.opcode == Opcode::IteratorHasNext || step.opcode == Opcode::IteratorValue)
                            return isConstantIndex(step.child(1), INT32_MAX);
                        return step.opcode == Opcode::IteratorClose;
                    });
                }
                if (use.opcode != Opcode::Cast || use.castReason() == CastReason::Coerce || use.castReason() == CastReason::As)
                    return false;
                return isOnlyUsedBy(user, [&](ValueID, const Value& read) {
                    return read.opcode == Opcode::GetByVal && isConstantIndex(read.child(1), 2);
                });
            });
        }
        if (!isTakenApart)
            continue;
        m_entriesMadeAsRecords.set(call.index());
        m_iterationsOfEntries.set(iterator.index());
        for (auto& [id, pair] : pairs)
            m_pairsOfEntries.add(id, pair);
    }
}

TypedPointer Lowering::wordOfIterationRecord(CellPointer record, unsigned word)
{
    return m_out.address(m_heaps.AOTTyped_runtimeState, record.value(), JSCellButterfly::offsetOfData() + word * sizeof(EncodedJSValue));
}

// Of what an IteratorOpen has yielded, which is a cell: an array or a string that stands for its own iterator, or a record. The first two words of a record stay what they are.
Word32 Lowering::iteratesEntriesOfArray(ValueID iterator)
{
    CellPointer cell = asCell(in<Boxed>(iterator));
    return either<Word32>(equal(typeOfCell(cell), word32(JSCellButterflyType)), [&] {
        return Word32(m_out.equal(marked(m_out.load64(wordOfIterationRecord(cell, 1)), IsFixed), m_out.constInt64(JSValue::encode(jsNumber(CollectionIteration::isOfEntriesOfArrayByIndex)))));
    }, [&] {
        return boolean(false);
    });
}

CellPointer Lowering::arrayOfEntries(ValueID iterator)
{
    return CellPointer(marked(m_out.load64(wordOfIterationRecord(asCell(in<Boxed>(iterator)), 0)), IsFixed));
}

// Nothing: the value has no part in such a loop.
std::optional<Operand> Lowering::partOfEntriesOfArray(ValueID id, const Value& value)
{
    if (m_iterationsOfEntries.isEmpty())
        return std::nullopt;
    auto& type = typeOf(id);
    Rep rep = repOf(type);
    auto pairOf = [&](ValueID operand) -> const PairOfEntries* {
        auto found = m_pairsOfEntries.find(operand.index());
        return found == m_pairsOfEntries.end() ? nullptr : &found->value;
    };
    // The counter of the loop, which is the index of a fresh iterator, and the element.
    auto part = [&](const PairOfEntries& pair, int64_t index) -> Operand {
        if (!index) {
            ValueID counter = m_function.at(pair.pair).child(1);
            return ensure(in<Integer>(counter), typeOf(counter), type);
        }
        if (index == 1)
            return ensure(in<Boxed>(pair.pair), type);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    };

    switch (value.opcode) {
    case Opcode::IteratorOpen: {
        Boxed operand = in<Boxed>(value.child(0));
        if (m_iterationsOfEntries.get(id.index())) {
            // No value of the program is such a cell.
            Word32 isRecord = either<Word32>(isCell(operand), [&] {
                return equal(typeOfCell(asCell(operand)), word32(JSCellButterflyType));
            }, [&] {
                return boolean(false);
            });
            return eitherOperand(isRecord, rep, [&] {
                return convert(operand, type, rep);
            }, [&] {
                return lowerIteratorOpen(id, value);
            });
        }
        auto* pair = pairOf(id);
        if (!pair)
            return std::nullopt;
        return eitherOperand(iteratesEntriesOfArray(pair->iterator), rep, [&] {
            return convert(operand, type, rep);
        }, [&] {
            return lowerIteratorOpen(id, value);
        });
    }
    case Opcode::IteratorHasNext: {
        if (m_iterationsOfEntries.get(value.child(0).index())) {
            return Operand(either<Word32>(iteratesEntriesOfArray(value.child(0)), [&] {
                return below(in<Integer>(value.child(1)), arrayLength(arrayOfEntries(value.child(0))));
            }, [&] {
                return lowerIteratorHasNext(value);
            }));
        }
        auto* pair = pairOf(value.child(0));
        if (!pair)
            return std::nullopt;
        return Operand(either<Word32>(iteratesEntriesOfArray(pair->iterator), [&] {
            return boolean(m_function.at(value.child(1)).integer() < 2);
        }, [&] {
            return lowerIteratorHasNext(value);
        }));
    }
    case Opcode::IteratorValue: {
        if (m_iterationsOfEntries.get(value.child(0).index())) {
            return eitherOperand(iteratesEntriesOfArray(value.child(0)), rep, [&] {
                return getArrayElement(arrayOfEntries(value.child(0)), value.child(1), type, nullptr, false);
            }, [&] {
                return lowerIteratorValue(id, value);
            });
        }
        auto* pair = pairOf(value.child(0));
        if (!pair)
            return std::nullopt;
        return eitherOperand(iteratesEntriesOfArray(pair->iterator), rep, [&] {
            return part(*pair, m_function.at(value.child(1)).integer());
        }, [&] {
            return lowerIteratorValue(id, value);
        });
    }
    case Opcode::IteratorClose: {
        auto* pair = pairOf(value.child(0));
        if (!pair)
            return std::nullopt;
        ifElse(iteratesEntriesOfArray(pair->iterator), [] { }, [&] {
            lowerIteratorClose(value);
        });
        return Operand(undefined());
    }
    case Opcode::Cast: {
        auto* pair = pairOf(id);
        if (!pair)
            return std::nullopt;
        return eitherOperand(iteratesEntriesOfArray(pair->iterator), rep, [&] {
            return convert(undefined(), m_program.at(m_program.types.undefined), rep);
        }, [&] {
            return lowerCast(id, value);
        });
    }
    case Opcode::GetByVal: {
        auto* pair = pairOf(value.child(0));
        if (!pair)
            return std::nullopt;
        return eitherOperand(iteratesEntriesOfArray(pair->iterator), rep, [&] {
            return part(*pair, m_function.at(value.child(1)).integer());
        }, [&] {
            return lowerGetByVal(id, value);
        });
    }
    default:
        return std::nullopt;
    }
}

Operand Lowering::lowerIteratorOpen(ValueID result, const Value& value)
{
    ValueID iterable = value.child(0);
    if (auto* iteration = iterationOfString(result))
        return openIterationOfString(*iteration, result, iterable);
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array) {
        CellPointer array = nonNil(iterable, { });
        LBasicBlock hasChanged = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isPlainArray(array).value(), usually(continuation), rarely(hasChanged));
        m_out.appendTo(hasChanged);
        AOT_TYPED_OPERATION(OpenChangedArray, array);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        // Read here, so that a loop that changes no array does not read them again for every element.
        Pointer butterfly = butterflyOf(array);
        ensureStillAliveHere(CellPointer(publicLength(butterfly).value()));
        ensureStillAliveHere(CellPointer(m_out.zeroExtPtr(indexingModeOf(array, IndexingShapeMask).value())));
        return ensure(array, typeOf(iterable), typeOf(result));
    }
    auto generally = [&] {
        return AOT_TYPED_OPERATION(IterOpen, in<Boxed>(iterable));
    };
    // An array as the engine makes them finds the iteration of arrays, which cannot be replaced.
    auto expected = predictionOf(iterable);
    if (!expected || !expected->onlyEngineClass() || expected->onlyEngineClass()->type != ArrayType)
        return ensure(generally(), typeOf(result));
    return ensure(ifAsExpected({ { iterable, &*expected, true } }, Rep::JSValue, [&](LBasicBlock) {
        return in<Boxed>(iterable);
    }, generally).as<Boxed>(), typeOf(result));
}

Word32 Lowering::lowerIteratorHasNext(const Value& value)
{
    ValueID iterator = value.child(0);
    auto isInsideArray = [&] {
        return below(in<Integer>(value.child(1)), arrayLength(asCell(in<Boxed>(iterator))));
    };
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array)
        return isInsideArray();
    auto generally = [&] {
        return either<Word32>(isArrayStandingForItsIterator(iterator), isInsideArray, [&] {
            return AOT_TYPED_OPERATION(IterHas, in<Boxed>(iterator));
        });
    };
    auto* iteration = iterationOfString(iterator);
    if (!iteration)
        return generally();
    if (iteration->isCertain)
        return iteratedStringHasNext(*iteration, iterator);
    return either<Word32>(isStringStandingForItsIterator(iterator), [&] {
        return iteratedStringHasNext(*iteration, iterator);
    }, generally);
}

Operand Lowering::lowerIteratorValue(ValueID result, const Value& value)
{
    ValueID iterator = value.child(0);
    auto& type = typeOf(result);
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array)
        return getArrayElement(asCell(in<Boxed>(iterator)), value.child(1), type, nullptr, isExpectedToBeNumber(result));
    auto* iteration = iterationOfString(iterator);
    auto& stringType = m_program.at(m_program.types.string);
    if (iteration && iteration->isCertain) {
        Word32 codePoint;
        Operand string = ensure(nextOfIteratedString(*iteration, iterator, codePoint), stringType, type);
        // No code unit is above 0xffff: a pair is equal to no string of one.
        if (keepsCodeUnitsOfStrings())
            m_codeUnitsOfStrings.set(result.index(), CodeUnitOfString { string.value, zeroExtended(codePoint).value() });
        return string;
    }
    if (iteration) {
        return eitherOperand(isStringStandingForItsIterator(iterator), repOf(type), [&] {
            Word32 codePoint;
            return ensure(nextOfIteratedString(*iteration, iterator, codePoint), stringType, type);
        }, [&] {
            return valueOfArrayOrRecord(result, value);
        });
    }
    return valueOfArrayOrRecord(result, value);
}

Operand Lowering::valueOfArrayOrRecord(ValueID result, const Value& value)
{
    ValueID iterator = value.child(0);
    auto& type = typeOf(result);
    // The iterator of arrays asks the array for the element, whatever has become of the array since.
    return eitherOperand(isArrayStandingForItsIterator(iterator), repOf(type), [&] {
        return ifAsExpected({ }, repOf(type), [&](LBasicBlock otherwise) {
            return getArrayElement(asCell(in<Boxed>(iterator)), value.child(1), type, otherwise, isExpectedToBeNumber(result));
        }, [&] {
            return ensure(getByValGenerally(iterator, value.child(1)), type);
        });
    }, [&] {
        // Every record has the value of the step there: see operationAOTTypedIterHas.
        return ensure(Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, asCell(in<Boxed>(iterator)).value(), JSCellButterfly::offsetOfData() + 2 * sizeof(EncodedJSValue)))), type);
    });
}

void Lowering::lowerIteratorClose(const Value& value)
{
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array)
        return;
    auto generally = [&] {
        ifElse(isArrayStandingForItsIterator(value.child(0)), [] { }, [&] {
            if (value.immediates[0])
                AOT_TYPED_OPERATION(IterCloseForThrow, in<Boxed>(value.child(0)));
            else
                AOT_TYPED_OPERATION(IterClose, in<Boxed>(value.child(0)));
        });
    };
    auto* iteration = iterationOfString(value.child(0));
    if (!iteration)
        return generally();
    if (!iteration->isCertain)
        ifElse(isStringStandingForItsIterator(value.child(0)), [] { }, generally);
}

// The table.

// The entries that have to do with the above, for arguments writesOfInlineBuiltin() says there is code for. `inPlace`: what it says.
Operand Lowering::lowerContainerBuiltin(InlineBuiltin which, ValueID result, const Value& value, Writes inPlace)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto& booleanType = m_program.at(m_program.types.boolean);
    // Of what takes one.
    auto receiver = [&] {
        return nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    };
    auto yieldsNew = [&](CellPointer cell) {
        return ensure(cell, Type::engineClass(m_builtins.classes()[value.engineClass()]), type);
    };
    auto yieldsLength = [&](Integer length, int64_t most) {
        return ensure(length, Type::integer({ 0, most }), type);
    };

    switch (which) {
    case InlineBuiltin::ArrayLength:
        return yieldsLength(arrayLength(receiver()), UINT32_MAX);
    // An own property of every array that cannot be redefined as anything else, so that nothing gets in the way of its name.
    case InlineBuiltin::ArraySetLength: {
        CellPointer array = receiver();
        // Of an array with its elements in place, which it may write to, the length can be written. What is between the length and the capacity is kept clear.
        constexpr unsigned maxLengthToClearOneByOne = 64;
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock shorter = m_out.newBlock();
        LBasicBlock notShorter = m_out.newBlock();
        LBasicBlock clearLoop = m_out.newBlock();
        LBasicBlock clear = m_out.newBlock();
        LBasicBlock setLength = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        didEmitWrite(Writes::Heap);
        Integer newLength = indexOrElse(arguments[0], slowPath);
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
        orElse(belowOrEqual(binary<Word32>(&Output::sub, mode, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
        Pointer butterfly = butterflyOf(array);
        Integer length = publicLength(butterfly);
        m_out.branch(below(newLength, length).value(), unsure(shorter), unsure(notShorter));
        m_out.appendTo(notShorter);
        orElse(belowOrEqual(newLength, vectorLength(butterfly)), slowPath);
        m_out.jump(setLength);
        m_out.appendTo(shorter);
        orElse(belowOrEqual(sub(length, newLength), integer(maxLengthToClearOneByOne)), slowPath);
        LValue hole = m_out.select(equal(mode, word32(DoubleShape)).value(), m_out.constInt64(std::bit_cast<int64_t>(PNaN)), m_out.int64Zero);
        ValueFromBlock first = m_out.anchor(newLength.value());
        m_out.jump(clearLoop);
        m_out.appendTo(clearLoop);
        LValue place = m_out.phi(Int64, first);
        m_out.branch(m_out.below(place, length.value()), unsure(clear), unsure(setLength));
        m_out.appendTo(clear);
        // Whichever way the elements are kept.
        m_out.store64(hole, m_out.baseIndex(m_heaps.root, butterfly.value(), place, FTL::ScaleEight));
        m_out.addIncomingToPhi(place, m_out.anchor(m_out.add(place, m_out.constInt64(1))));
        m_out.jump(clearLoop);
        m_out.appendTo(setLength);
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(newLength));
        m_out.jump(continuation);
        m_out.appendTo(slowPath);
        AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effectsOfStoringElements, box(array), word32(m_program.stringOf(m_builtins.at(value.builtin()).nameName).index()), in<Boxed>(arguments[0]));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case InlineBuiltin::ArrayPush:
        if (arguments.size() == 1)
            return pushArrayElement(receiver(), value.child(0), arguments[0], type);
        return pushArrayElements(receiver(), value.child(0), arguments, m_program.stringOf(m_builtins.at(value.builtin()).nameName), type);
    case InlineBuiltin::ArrayWithLength: {
        Integer length;
        if (typeOf(arguments[0]).isInteger())
            length = get(arguments[0]).as<Integer>();
        else {
            // What is no integer is no length, as little as what is negative.
            Number number = in<Number>(arguments[0]);
            length = select(isIntegral(number), truncateToInteger(number), integer(-1));
        }
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fast = m_out.anchor(allocateArrayWithHoles(length, indexingTypeAtBirth(arrayTypeAtBirth(result)), slowPath).value());
        m_out.jump(continuation);
        m_out.appendTo(slowPath);
        ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArrayWithHoles, CellPointer(m_out.int64Zero), length, word32(indexingTypeAtBirth(arrayTypeAtBirth(result))), word32(false)).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return yieldsNew(CellPointer(m_out.phi(Int64, fast, slow)));
    }
    case InlineBuiltin::ArrayOf:
        return yieldsNew(newArrayOfType(arguments, expectedTypeOf(result)));
    case InlineBuiltin::ArrayIsArray: {
        static_assert(DerivedArrayType == ArrayType + 1);
        ValueID argument = arguments[0];
        if (!typeOf(argument).hasObjects())
            return ensure(boolean(false), booleanType, type);
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock isArray = m_out.newBlock();
        LBasicBlock isOtherCell = m_out.newBlock();
        LBasicBlock isNotArray = m_out.newBlock();
        LBasicBlock isProxy = newColdBlock();
        Join join(m_out, Rep::Int32, Int32);
        Boxed boxed = in<Boxed>(argument);
        m_out.branch(isCell(boxed).value(), unsure(isCellCase), unsure(isNotArray));
        m_out.appendTo(isCellCase);
        Word32 cellType = typeOfCell(asCell(boxed));
        m_out.branch(below(binary<Word32>(&Output::sub, cellType, word32(ArrayType)), word32(2)).value(), unsure(isArray), unsure(isOtherCell));
        m_out.appendTo(isArray);
        join.add(boolean(true));
        m_out.appendTo(isOtherCell);
        m_out.branch(equal(cellType, word32(ProxyObjectType)).value(), rarely(isProxy), usually(isNotArray));
        m_out.appendTo(isNotArray);
        join.add(boolean(false));
        // It is what its target is. That runs no code of the program, but there is no target once the proxy has been revoked: that throws.
        m_out.appendTo(isProxy);
        join.add(unboxedBoolean(callBuiltinFunction(value, OperationEffects { inPlace, true })));
        return ensure(join.finish(), booleanType, type);
    }

    case InlineBuiltin::TypedArrayWithLength:
        return yieldsNew(AOT_TYPED_OPERATION(NewTypedArray, word32(m_builtins.classes()[value.engineClass()].typedArrayType), in<Number>(arguments[0])));
    case InlineBuiltin::TypedArrayLength: {
        TypedArrayReceiver view = typedArrayReceiver(receiver());
        Type lengthType = Type::integer({ 0, static_cast<int64_t>(MAX_ARRAY_BUFFER_SIZE) });
        return ensure(either<Integer>(hasLengthOfItsOwn(view), [&] {
            return loadField<Integer>(view.view, m_heaps.JSArrayBufferView_length, view.flags);
        }, [&] {
            return convertTo<Integer>(callBuiltinFunction(value, OperationEffects { inPlace, true }), lengthType);
        }), lengthType, type);
    }

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
