/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

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
    case Rep::Double: {
        Number number = operand.as<Number>();
        Integer index = truncateToInteger(number);
        orElse(equal(convertToNumber(index), number), otherwise);
        return index;
    }
    case Rep::JSValue: {
        Boxed boxed = operand.as<Boxed>();
        orElse(isInt32(boxed), otherwise);
        return signExtended(low32(boxed));
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
    return emitGetByVal(base, convertTo<Boxed>(operand, typeOf(key)));
}

void Lowering::putByValGenerally(ValueID object, ValueID key, ValueID stored)
{
    Boxed base = in<Boxed>(object);
    Operand operand = get(key);
    if (operand.rep != Rep::Int64) {
        Boxed boxedKey = convertTo<Boxed>(operand, typeOf(key));
        emitPutByVal(base, boxedKey, in<Boxed>(stored));
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
    if (rep != Rep::Int64 && rep != Rep::Double && rep != Rep::JSValue)
        return false;
    return expectedTypeOf(key).primitives & Primitive::Int;
}

Operand Lowering::getArrayElement(CellPointer array, ValueID key, const Type& type, LBasicBlock otherwise, bool looksForNumbersLast)
{
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
    // Goes on where the elements are kept another way.
    auto whenKeptAs = [&](IndexingType kept, const auto& read) {
        LBasicBlock isKeptSo = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        m_out.branch(equal(shape, word32(kept)).value(), unsure(isKeptSo), unsure(isNot));
        m_out.appendTo(isKeptSo);
        read();
        m_out.appendTo(isNot);
    };
    auto readValue = [&] {
        Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
        orElse(isNotEmpty(element), absent);
        join.add(ensure(element, type));
    };
    if (looksForNumbersLast)
        whenKeptAs(ContiguousShape, readValue);
    // Kept as numbers, they are only looked at where a number will do.
    if (type.primitives & Primitive::Number) {
        whenKeptAs(DoubleShape, [&] {
            Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
            // Among doubles a hole is what is not a number, and nothing else is.
            orElse(equal(asDouble, asDouble), absent);
            join.add(ensure(asDouble, m_program.at(m_program.types.number), type));
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
    barrier(array);
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
    bool isNumber = type.isNumber();
    // Among doubles it would read as a hole. It is rare enough not to look first how the elements are kept.
    if (Operand operand = get(stored); operand.rep == Rep::Double)
        orElse(equal(operand.as<Number>(), operand.as<Number>()), otherwise);

    Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
    Word32 isDouble = equal(mode, word32(DoubleShape));
    Word32 isTaken = equal(mode, word32(ContiguousShape));
    if (isNumber)
        isTaken = logicalOr(isTaken, isDouble);
    if (type.isInteger() && type.integers.isWithin(Range::int32()))
        isTaken = logicalOr(isTaken, equal(mode, word32(Int32Shape)));
    orElse(isTaken, otherwise);

    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    Integer place = index.value_or(length);
    // What is between the length and the capacity is kept clear, so that it reads as holes once the array is that long.
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

    LBasicBlock values = m_out.newBlock();
    if (isNumber) {
        LBasicBlock doubles = m_out.newBlock();
        m_out.branch(isDouble.value(), unsure(doubles), unsure(values));
        m_out.appendTo(doubles);
        m_out.storeDouble(in<Number>(stored).value(), m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), place.value()));
        afterwards(place);
    } else
        m_out.jump(values);
    m_out.appendTo(values);
    m_out.store64(in<Boxed>(stored).value(), m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), place.value()));
    if (type.hasCells())
        barrier(array);
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
        AOT_TYPED_OPERATION(ArrayPut, array, in<Boxed>(key), in<Boxed>(stored));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
}

// Yields the new length, or what a function that has taken the place of the engine's yields.
Operand Lowering::pushArrayElement(CellPointer array, ValueID pushed, const Type& type)
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
    join.add(ensure(AOT_TYPED_OPERATION(ArrayPush, array, in<Boxed>(pushed)), type));
    return join.finish();
}

// The vector of a typed array and the characters of a string are freed with their owner, and a pointer to them keeps nothing alive. The back
// end holds on to one across calls that collect, so after each use of one the collector still has to find the owner. No code comes of this.
void Lowering::ensureStillAliveHere(CellPointer owner)
{
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
        return getArrayElement(nonNil(object, { }), key, type);
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
        bool looksForNumbersLast = expected.elements && !(m_program.at(expected.elements).primitives & Primitive::Number);
        return inPlace([&](CellPointer array, LBasicBlock otherwise) {
            return getArrayElement(array, key, type, otherwise, looksForNumbersLast);
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

Operand Lowering::lowerIteratorOpen(ValueID result, const Value& value)
{
    ValueID iterable = value.child(0);
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array) {
        CellPointer array = nonNil(iterable, { });
        LBasicBlock hasChanged = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isPlainArray(array).value(), usually(continuation), rarely(hasChanged));
        m_out.appendTo(hasChanged);
        AOT_TYPED_OPERATION(OpenChangedArray, array);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
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
    return either<Word32>(isArrayStandingForItsIterator(iterator), isInsideArray, [&] {
        return AOT_TYPED_OPERATION(IterHas, in<Boxed>(iterator));
    });
}

Operand Lowering::lowerIteratorValue(ValueID result, const Value& value)
{
    ValueID iterator = value.child(0);
    auto& type = typeOf(result);
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array)
        return getArrayElement(asCell(in<Boxed>(iterator)), value.child(1), type);
    // The iterator of arrays asks the array for the element, whatever has become of the array since.
    return eitherOperand(isArrayStandingForItsIterator(iterator), repOf(type), [&] {
        return ifAsExpected({ }, repOf(type), [&](LBasicBlock otherwise) {
            return getArrayElement(asCell(in<Boxed>(iterator)), value.child(1), type, otherwise);
        }, [&] {
            return ensure(getByValGenerally(iterator, value.child(1)), type);
        });
    }, [&] {
        return ensure(AOT_TYPED_OPERATION(IterGet, in<Boxed>(iterator)), type);
    });
}

void Lowering::lowerIteratorClose(const Value& value)
{
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array)
        return;
    ifElse(isArrayStandingForItsIterator(value.child(0)), [] { }, [&] {
        if (value.immediates[0])
            AOT_TYPED_OPERATION(IterCloseForThrow, in<Boxed>(value.child(0)));
        else
            AOT_TYPED_OPERATION(IterClose, in<Boxed>(value.child(0)));
    });
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
        AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effectsOfStoringElements, box(array), word32(m_program.stringOf(m_builtins.at(value.builtin()).nameName).index()), in<Boxed>(arguments[0]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case InlineBuiltin::ArrayPush:
        return pushArrayElement(receiver(), arguments[0], type);
    case InlineBuiltin::ArrayWithLength: {
        if (typeOf(arguments[0]).isInteger())
            return yieldsNew(AOT_TYPED_OPERATION(NewArrayWithLength, get(arguments[0]).as<Integer>()));
        // What is no integer is no length, as little as what is negative.
        Number length = in<Number>(arguments[0]);
        return yieldsNew(AOT_TYPED_OPERATION(NewArrayWithLength, select(isIntegral(length), truncateToInteger(length), integer(-1))));
    }
    case InlineBuiltin::ArrayOf: {
        auto [count, list] = argumentBuffer(arguments);
        return yieldsNew(AOT_TYPED_OPERATION(NewArray, count, list));
    }
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
        join.add(unboxedBoolean(callFunctionOfEngine(value, OperationEffects { inPlace, true })));
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
            return convertTo<Integer>(callFunctionOfEngine(value, OperationEffects { inPlace, true }), lengthType);
        }), lengthType, type);
    }

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
