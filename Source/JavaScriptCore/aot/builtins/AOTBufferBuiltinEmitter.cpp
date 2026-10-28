/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBufferBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayBufferViewBuiltinEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTResultRules.h"
#include "AOTSlotEmitter.h"
#include "AOTTruthinessEmitter.h"
#include "AOTTypedArrayKinds.h"
#include "B3StackmapGenerationParams.h"
#include "CCallHelpers.h"

namespace JSC { namespace AOT { namespace Typed {

static LValue bitwiseCast(Output& out, LValue value)
{
    return out.m_block->appendNew<B3::Value>(out.m_proc, B3::BitwiseCast, out.origin(), value);
}

BufferBuiltinEmitter::BufferBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void BufferBuiltinEmitter::generateDataViewByteLength(const InlineCall& call)
{
    ArrayBufferViewBuiltinEmitter& views = emitter<ArrayBufferViewBuiltinEmitter>();
    CellPointer view = cellOperand(call.receiver());
    missUnless(call, views.hasFixedLength(view));
    // A view of a detached buffer has no bytes, and the getter throws.
    Integer length = views.lengthOf(view);
    missUnless(call, isNotZero(length));
    finish(call, length, Range { 1, static_cast<int64_t>(MAX_ARRAY_BUFFER_SIZE) });
}

void BufferBuiltinEmitter::generateArrayBufferByteLength(const InlineCall& call)
{
    CellPointer cell = cellOperand(call.receiver());
    Pointer buffer = loadField<Pointer>(cell, m_heaps.JSArrayBuffer_impl);
    Integer length = loadField<Integer>(buffer, m_heaps.ArrayBuffer_sizeInBytes);
    ensureStillAliveHere(cell);
    finish(call, length, Range { 0, static_cast<int64_t>(MAX_ARRAY_BUFFER_SIZE) });
}

void BufferBuiltinEmitter::generateDataViewGet(const InlineCall& call)
{
    TypedArrayType elements = call.parameter<TypedArrayType>(0);
    unsigned size = elementSize(elements);
    LBasicBlock miss = missBlockOf(call);
    ByteOrder order = byteOrderOf(call, 1, size);
    CellPointer view = cellOperand(call.receiver());
    Integer offset = offsetOrElse(call.argument(0), miss);
    Pointer address = findBytes(view, offset, mayBeNegative(call.argument(0)), size, miss);
    Operand loaded = loadBytes(call.context(), address, elements, order);
    ensureStillAliveHere(view);
    finish(call, loaded, elementTypeOf(elements));
}

void BufferBuiltinEmitter::generateDataViewSet(const InlineCall& call)
{
    TypedArrayType elements = call.parameter<TypedArrayType>(0);
    unsigned size = elementSize(elements);
    ValueID value = call.argument(1);
    LBasicBlock miss = missBlockOf(call);
    ByteOrder order = byteOrderOf(call, 2, size);
    CellPointer view = cellOperand(call.receiver());
    Integer offset = offsetOrElse(call.argument(0), miss);
    Operand stored;
    if (isBigInt(elements))
        stored = low64Of(cellOperand(value));
    else if (isFloat(elements))
        stored = numberOperand(value);
    else if (typeOf(value).isInt32())
        stored = int32Operand(value);
    else
        stored = emitter<ConversionEmitter>().toInt32(numberOperand(value));
    Pointer address = findBytes(view, offset, mayBeNegative(call.argument(0)), size, miss);
    storeBytes(address, elements, stored, order);
    ensureStillAliveHere(view);
    finishWithUndefined(call);
}

// `source.copy(target, targetStart, sourceStart, sourceEnd)`: as many bytes as there are and as there is room for. A position that is negative, has a fraction or lies behind the source leaves for the function.
void BufferBuiltinEmitter::generateBufferCopy(const InlineCall& call)
{
    ArrayBufferViewBuiltinEmitter& views = emitter<ArrayBufferViewBuiltinEmitter>();
    LBasicBlock miss = missBlockOf(call);
    CellPointer source = cellOperand(call.receiver());
    CellPointer target = cellOperand(call.argument(0));
    auto positionOr = [&](unsigned argumentIndex, Integer otherwise) {
        ValueID id = call.argumentOrNull(argumentIndex);
        if (!id)
            return otherwise;
        Integer position = offsetOrElse(id, miss);
        if (mayBeNegative(id))
            branchUnless(greaterThanOrEqual(position, 0), miss);
        return position;
    };
    auto byteLengthOf = [&](CellPointer view, ValueID id) {
        return Integer(m_out.shl(views.lengthOf(view).value(), logElementSize(typedArrayTypeOf(m_builtins, typeOf(id)))));
    };
    Integer targetStart = positionOr(1, integer(0));
    Integer sourceStart = positionOr(2, integer(0));
    missUnless(call, views.haveFixedLength(source, target));
    Integer sourceLength = byteLengthOf(source, call.receiver());
    Integer sourceEnd = positionOr(3, sourceLength);
    if (call.numberOfArguments() > 2)
        branchUnless(lessThanOrEqual(sourceStart, sourceLength), miss);
    if (call.numberOfArguments() > 3)
        sourceEnd = select(lessThan(sourceEnd, sourceLength), sourceEnd, sourceLength);
    Integer available = sub(sourceEnd, sourceStart);
    Integer room = sub(byteLengthOf(target, call.argument(0)), targetStart);
    Integer least = select(lessThan(available, room), available, room);
    Integer count = select(greaterThan(least, 0), least, 0);
    LBasicBlock move = newBlock();
    LBasicBlock done = newBlock();
    m_out.branch(isNotZero(count).value(), normalEdge(move), unlikelyEdge(done));

    m_out.appendTo(move);
    views.moveBytes(call.context(), target, targetStart, source, sourceStart, count);
    m_out.jump(done);

    m_out.appendTo(done);
    finish(call, count, Range { 0, static_cast<int64_t>(MAX_ARRAY_BUFFER_SIZE) });
}

BufferBuiltinEmitter::ByteOrder BufferBuiltinEmitter::byteOrderOf(const InlineCall& call, unsigned argumentIndex, unsigned size)
{
    ValueID argument = call.argumentOrNull(argumentIndex);
    if (size == 1 || !argument)
        return { false, { } };
    const Type& type = typeOf(argument);
    if (type.hasOnlyPrimitives(Primitive::UndefinedOrNull | Primitive::False))
        return { false, { } };
    if (type.hasOnlyPrimitives(Primitive::True))
        return { true, { } };
    return { std::nullopt, emitter<TruthinessEmitter>().isTruthy(call.context(), get(argument), type) };
}

bool BufferBuiltinEmitter::mayBeNegative(ValueID id) const
{
    const Type& type = typeOf(id);
    return !type.isInteger() || type.integers.minimum < 0;
}

Integer BufferBuiltinEmitter::offsetOrElse(ValueID id, LBasicBlock otherwise)
{
    if (typeOf(id).isInteger())
        return integerOperand(id);
    Number number = numberOperand(id);
    Integer offset = emitter<ConversionEmitter>().toInteger(number);
    branchUnless(equal(toNumber(offset), number), otherwise);
    return offset;
}

Pointer BufferBuiltinEmitter::findBytes(CellPointer view, Integer offset, bool mayBeNegative, unsigned size, LBasicBlock otherwise)
{
    if (size > 1 && mayBeNegative)
        branchUnless(greaterThanOrEqual(offset, integer(0)), otherwise);
    Pointer vector = emitter<ArrayBufferViewBuiltinEmitter>().findTypedArrayElement(view, add(offset, integer(size - 1)), otherwise, otherwise);
    return offsetBy(vector, offset, 0);
}

LValue BufferBuiltinEmitter::byteSwapped(LValue bits, unsigned size)
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(size == 8 ? B3::Int64 : B3::Int32);
    patchpoint->appendSomeRegister(bits);
    patchpoint->effects = B3::Effects::none();
    patchpoint->setRunsWithoutFrame(true);
    patchpoint->setGenerator([size](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        GPRReg result = params[0].gpr();
        jit.move(params[1].gpr(), result);
        switch (size) {
        case 2:
            jit.byteSwap16(result);
            return;
        case 4:
            jit.byteSwap32(result);
            return;
        case 8:
            jit.byteSwap64(result);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    return patchpoint;
}

LValue BufferBuiltinEmitter::inByteOrder(LValue bits, unsigned size, const ByteOrder& order)
{
    if (size == 1 || order.isLittle == true)
        return bits;
    LValue swapped = byteSwapped(bits, size);
    return order.isLittle ? swapped : m_out.select(order.isLittleAtRunTime.value(), bits, swapped);
}

Operand BufferBuiltinEmitter::loadBytes(const OperationContext& context, Pointer address, TypedArrayType elements, const ByteOrder& order)
{
    TypedPointer pointer(m_heaps.TypedArrayProperties, address.value());
    unsigned size = elementSize(elements);
    Operand element;
    if (size == 1 || order.isLittle == true)
        element = emitter<SlotEmitter>().load(pointer, elementStore(elements));
    else {
        LValue bits = inByteOrder(size == 2 ? m_out.load16ZeroExt32(pointer) : size == 4 ? m_out.load32(pointer) : m_out.load64(pointer), size, order);
        switch (elements) {
        case TypeInt16:
            element = shiftRight(shiftLeft(Word32(bits), 16), 16);
            break;
        case TypeUint16:
        case TypeInt32:
            element = Word32(bits);
            break;
        case TypeUint32:
            element = zeroExtended(Word32(bits));
            break;
        case TypeFloat32:
            element = Number(m_out.floatToDouble(bitwiseCast(m_out, bits)));
            break;
        case TypeFloat64:
            element = Number(m_out.bitCast(bits, B3::Double));
            break;
        case TypeBigInt64:
        case TypeBigUint64:
            element = Integer(bits);
            break;
        case NotTypedArray:
        case TypeInt8:
        case TypeUint8:
        case TypeUint8Clamped:
        case TypeFloat16:
        case TypeDataView:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    if (isBigInt(elements))
        return newBigInt(context, element.as<Integer>(), elements == TypeBigInt64);
    if (isFloat(elements))
        return ImpureNumber(element.value);
    return element;
}

void BufferBuiltinEmitter::storeBytes(Pointer address, TypedArrayType elements, Operand stored, const ByteOrder& order)
{
    TypedPointer pointer(m_heaps.TypedArrayProperties, address.value());
    unsigned size = elementSize(elements);
    if (isFloat(elements) && order.isLittle == true) {
        emitter<SlotEmitter>().store(pointer, elementStore(elements), stored);
        return;
    }
    LValue bits = stored.value;
    if (elements == TypeFloat32)
        bits = bitwiseCast(m_out, m_out.doubleToFloat(bits));
    else if (elements == TypeFloat64)
        bits = m_out.bitCast(bits, B3::Int64);
    bits = inByteOrder(bits, size, order);
    switch (size) {
    case 1:
        m_out.store32As8(bits, pointer);
        return;
    case 2:
        m_out.store32As16(bits, pointer);
        return;
    case 4:
        m_out.store32(bits, pointer);
        return;
    case 8:
        m_out.store64(bits, pointer);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition BufferBuiltinEmitter::isNegativeBigInt(CellPointer bigInt)
{
    return hasBits(loadByte(bigInt, m_heaps.JSCell_typeInfoFlags), TypeInfoPerCellBit);
}

Integer BufferBuiltinEmitter::low64Of(CellPointer bigInt)
{
    return either<Integer>(isNotZero(loadField<Word32>(bigInt, m_heaps.JSBigInt_length)), [&] {
        Integer digit = loadField<Integer>(bigInt, m_heaps.JSBigInt_data);
        Condition isNegative = isNegativeBigInt(bigInt);
        return select(isNegative, neg(digit), digit);
    }, [&] {
        return integer(0);
    });
}

CellPointer BufferBuiltinEmitter::newBigInt(const OperationContext& context, Integer bits, bool isSigned)
{
    constexpr auto kind = static_cast<unsigned>(InlineAllocation::BigInt);
    static_assert(JSCell::typeInfoFlagsOffset() == 6 && JSBigInt::offsetOfLength() == sizeof(JSCell) && JSBigInt::offsetOfData() == JSBigInt::offsetOfLength() + sizeof(uint64_t));
    static_assert(JSBigInt::offsetOfData() + sizeof(uint64_t) <= oneDigitBigIntSize);
    CellAllocationEmitter& cells = emitter<CellAllocationEmitter>();
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    CellPointer cell;
    {
        CodeGenerator::InitializationScope scope(generator());
        cell = cells.allocateObject(InlineAllocation::BigInt, slowPath);
        Integer magnitude = bits;
        if (isSigned) {
            Condition isNegative = lessThan(bits, integer(0));
            magnitude = select(isNegative, neg(bits), bits);
            Integer header(m_out.load64(instanceField(m_heaps.entire(AbstractHeap::AllocatorState), AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header))));
            cells.storeHeader(cell, select(isNegative, bitOr(header, integer(static_cast<int64_t>(TypeInfoPerCellBit) << 48)), header));
        }
        storeField(cell, m_heaps.JSBigInt_length, zeroExtended(isNotZero(bits)));
        storeField(cell, m_heaps.JSBigInt_data, magnitude);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    join.add(cell);

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::NewBigInt64, { bits, word32(isSigned) }, { .isOnRarePath = true }));
    return join.finish();
}

Operand BufferBuiltinEmitter::valueToStoreOrElse(ValueID id, TypedArrayType elements, LBasicBlock otherwise)
{
    if (isFloat(elements))
        return numberOperand(id);
    if (isBigInt(elements)) {
        CellPointer bigInt = cellOperand(id);
        branchUnless(belowOrEqual(loadField<Word32>(bigInt, m_heaps.JSBigInt_length), 1), otherwise);
        Integer value = low64Of(bigInt);
        Condition isNegative = isNegativeBigInt(bigInt);
        if (elements == TypeBigUint64)
            branchUnless(logicalNot(isNegative), otherwise);
        else {
            Condition valueIsZero = isZero(value);
            branchUnless(logicalOr(valueIsZero, equal(lessThan(value, 0), isNegative)), otherwise);
        }
        return value;
    }
    Integer value = offsetOrElse(id, otherwise);
    Range range = elementTypeOf(elements).integers;
    if (const Type& valueType = typeOf(id); !valueType.isInteger() || !valueType.integers.isSubsetOf(range))
        branchUnless(belowOrEqual(sub(value, range.minimum), range.maximum - range.minimum), otherwise);
    return low32(value);
}

void BufferBuiltinEmitter::generateBufferAccess(const InlineCall& call)
{
    bool isWrite = call.parameter<AccessDirection>(0) == AccessDirection::Write;
    unsigned size = call.parameterWord(1);
    ByteOrder order { call.parameter<Endianness>(3) == Endianness::Little, { } };
    std::optional<TypedArrayType> elements = elementsMovedBy(size, call.parameter<NumberKind>(2));
    RELEASE_ASSERT(elements);
    unsigned numberOfValues = isWrite;
    ValueID offsetArgument = call.argumentOrNull(numberOfValues);
    LBasicBlock miss = missBlockOf(call);
    CellPointer view = cellOperand(call.receiver());

    Operand stored;
    if (isWrite)
        stored = valueToStoreOrElse(call.argument(0), *elements, miss);
    Integer offset = offsetArgument ? offsetOrElse(offsetArgument, miss) : integer(0);
    Pointer address = findBytes(view, offset, offsetArgument && mayBeNegative(offsetArgument), size, miss);
    if (isWrite) {
        storeBytes(address, *elements, stored, order);
        ensureStillAliveHere(view);
        finish(call, add(offset, integer(size)), Range { size, Range::full().maximum });
        return;
    }
    Operand loaded = loadBytes(call.context(), address, *elements, order);
    ensureStillAliveHere(view);
    finish(call, loaded, elementTypeOf(*elements));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
