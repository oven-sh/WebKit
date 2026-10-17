/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTInlineCaches.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "B3StackmapGenerationParams.h"
#include "BufferAccessorRegistry.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "ObjectPropertyConditionSet.h"

namespace JSC { namespace AOT {

enum class BufferAccessorKind : uint8_t { Signed, Unsigned, Float };

struct BufferAccessor {
    ASCIILiteral name;
    unsigned builtinName;
    bool isWrite;
    uint8_t size;
    BufferAccessorKind kind;
    bool isLittleEndian;
};

static constexpr BufferAccessor bufferAccessors[] = {
#define AOT_BUFFER_ACCESSOR(name, builtinName, isWrite, size, kind, isLittleEndian) { #name ""_s, builtinName, isWrite, size, BufferAccessorKind::kind, isLittleEndian },
    FOR_EACH_AOT_TYPED_BUFFER_ACCESSOR(AOT_BUFFER_ACCESSOR)
#undef AOT_BUFFER_ACCESSOR
};
static_assert(std::size(bufferAccessors) == Instance::numberOfBufferAccessors);

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFindBufferAccessor, void, (Instance* instance, JSCell* receiver, uint32_t row))
{
    VM& vm = *instance->vm;
    auto& accessor = bufferAccessors[row];
    if (instance->bufferAccessorStructureIDs[row])
        return;
    auto find = [&]() -> JSCell* {
        Structure* structure = receiver->structure();
        if (receiver->type() != Uint8ArrayType || structure->isDictionary() || !structure->hasMonoProto() || !structure->propertyAccessesAreCacheable())
            return nullptr;
        JSObject* holder = structure->storedPrototypeObject();
        if (!holder || (instance->bufferStructure && (instance->bufferStructure != structure || instance->bufferAccessorHolder != holder)))
            return nullptr;
        Identifier name = Identifier::fromString(vm, accessor.name);
        if (isValidOffset(structure->get(vm, name)))
            return nullptr;
        if (Structure* forHolder = holder->structure(); forHolder->isDictionary() && !forHolder->isUncacheableDictionary() && !forHolder->hasBeenFlattenedBefore())
            forHolder->flattenDictionaryStructure(vm, holder);
        makePrototypeChainWatchable(vm, receiver);
        if (holder->structure()->typeInfo().overridesGetOwnPropertySlot() || holder->hasNonReifiedStaticProperties())
            return nullptr;
        ObjectPropertyCondition condition = generateConditionForSelfEquivalence(vm, nullptr, holder, name.impl());
        if (!condition)
            return nullptr;
        auto* function = dynamicDowncast<JSFunction>(condition.requiredValue());
        if (!function || !function->isHostFunction() || function->intrinsic() != BufferAccessorIntrinsic)
            return nullptr;
        auto descriptor = bufferAccessorDescriptor(function->nativeFunction());
        if (!descriptor || descriptor->byteLengthFromArgument || descriptor->isWrite != accessor.isWrite || descriptor->data.byteSize != accessor.size
            || descriptor->data.isFloatingPoint != (accessor.kind == BufferAccessorKind::Float) || descriptor->data.isSigned != (accessor.kind == BufferAccessorKind::Signed)
            || descriptor->data.isLittleEndian != triState(accessor.isLittleEndian))
            return nullptr;
        if (!condition.isWatchable(PropertyCondition::EnsureWatchability))
            return nullptr;
        instance->watchBufferAccessor(row, condition);
        instance->bufferStructure = structure;
        instance->bufferAccessorHolder = holder;
        return function;
    };
    JSCell* function = find();
    countOperationNamed(instance, "operationAOTTypedFindBufferAccessor", function ? "found" : "not-found");
    if (function) {
        instance->bufferAccessors[row] = function;
        instance->bufferAccessorStructureIDs[row] = receiver->structureID().bits();
        vm.writeBarrier(instance->globalObject);
    } else if (++instance->bufferAccessorAttempts[row] == Instance::maxBufferAccessorAttempts)
        instance->bufferAccessorStructureIDs[row] = Instance::bufferAccessorLookupDisabled;
}

static constexpr size_t oneDigitBigIntSize = roundUpToMultipleOf<MarkedBlock::atomSize>(JSBigInt::allocationSize(1));

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewBigInt64, JSCell*, (Instance* instance, uint64_t bits, uint32_t isSigned))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (auto& data = instance->inlineAllocations[static_cast<unsigned>(Instance::InlineAllocation::BigInt)]; !data.allocator) {
        Structure* structure = vm.bigIntStructure.get();
        auto* allocator = subspaceFor<JSBigInt>(vm)->allocatorFor(JSBigInt::allocationSize(1), AllocatorForMode::EnsureAllocator).localAllocator();
        RELEASE_ASSERT(allocator->cellSize() == oneDigitBigIntSize);
        data.header = static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
        data.allocator = allocator;
    }
    JSBigInt* result = isSigned ? JSBigInt::createFrom(globalObject, static_cast<int64_t>(bits)) : JSBigInt::createFrom(globalObject, bits);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, result);
}

namespace Typed {

using namespace B3;

#define FOR_EACH_DATA_VIEW_ELEMENT(v) \
    v(Int8) \
    v(Uint8) \
    v(Int16) \
    v(Uint16) \
    v(Int32) \
    v(Uint32) \
    v(Float32) \
    v(Float64) \
    v(BigInt64) \
    v(BigUint64)

struct DataViewAccess {
    bool isWrite;
    TypedArrayType elements;
};

static DataViewAccess dataViewAccessFor(InlineBuiltin which)
{
    switch (which) {
#define AOT_DATA_VIEW_ACCESS(name) \
    case InlineBuiltin::DataViewGet ## name: \
        return { false, Type ## name }; \
    case InlineBuiltin::DataViewSet ## name: \
        return { true, Type ## name };
    FOR_EACH_DATA_VIEW_ELEMENT(AOT_DATA_VIEW_ACCESS)
#undef AOT_DATA_VIEW_ACCESS
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static std::optional<unsigned> numberOfAtomicsOperands(InlineBuiltin which)
{
    switch (which) {
    case InlineBuiltin::AtomicsLoad:
        return 0;
    case InlineBuiltin::AtomicsAdd:
    case InlineBuiltin::AtomicsAnd:
    case InlineBuiltin::AtomicsExchange:
    case InlineBuiltin::AtomicsOr:
    case InlineBuiltin::AtomicsStore:
    case InlineBuiltin::AtomicsSub:
    case InlineBuiltin::AtomicsXor:
        return 1;
    case InlineBuiltin::AtomicsCompareExchange:
        return 2;
    default:
        return std::nullopt;
    }
}

static std::optional<TypedArrayType> elementsForAtomics(const Type& array)
{
    const EngineClass* engineClass = array.withoutPrimitives(Primitive::UndefinedOrNull).singleEngineClass();
    if (!engineClass)
        return std::nullopt;
    switch (engineClass->typedArrayType) {
    case TypeInt8:
    case TypeUint8:
    case TypeInt16:
    case TypeUint16:
    case TypeInt32:
    case TypeUint32:
        return engineClass->typedArrayType;
    default:
        return std::nullopt;
    }
}

std::optional<Writes> writesOfInlineBuffer(InlineBuiltin which, std::span<const Type> arguments)
{
    if (auto numberOfOperands = numberOfAtomicsOperands(which)) {
        if (arguments.size() != 2 + *numberOfOperands || !elementsForAtomics(arguments[0]))
            return std::nullopt;
        for (auto& argument : arguments.subspan(1)) {
            if (!argument.isNumber())
                return std::nullopt;
        }
        return Writes::TypedArrayProperties;
    }
    auto [isWrite, elements] = dataViewAccessFor(which);
    unsigned numberOfOperands = 1 + isWrite;
    if (arguments.size() < numberOfOperands || !arguments[0].isNumber())
        return std::nullopt;
    if (isWrite && !(isBigInt(elements) ? !arguments[1].isNever() && arguments[1].hasOnlyPrimitives(Primitive::BigInt) : arguments[1].isNumber()))
        return std::nullopt;
    if (arguments.size() > numberOfOperands && !arguments[numberOfOperands].hasOnlyPrimitives(Primitive::Boolean | Primitive::UndefinedOrNull))
        return std::nullopt;
    return Writes::RuntimeState;
}

std::optional<Operand> Lowering::lowerAtomics(InlineBuiltin which, ValueID result, const Value& value, Writes inlinePath)
{
    auto& arguments = value.arguments;
    auto& type = typeOf(result);
    auto& arrayType = typeOf(arguments[0]);
    TypedArrayType elements = *elementsForAtomics(arrayType);
    Type elementType = elementTypeOf(elements);
    Width width = widthForBytes(elementSize(elements));
    Rep rep = repOf(type);
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    Operand array = get(arguments[0]);
    if (array.rep != Rep::Cell)
        orElse(isCell(array.as<Boxed>(), arrayType), slowPath);
    CellPointer view = array.rep == Rep::Cell ? array.as<CellPointer>() : asCell(array.as<Boxed>());
    Integer index = indexOrElse(arguments[1], slowPath);
    Integer stored;
    std::array<LValue, 2> operands { };
    if (which == InlineBuiltin::AtomicsStore) {
        stored = indexOrElse(arguments[2], slowPath);
        operands[0] = low32(stored).value();
    } else {
        for (unsigned i = 2; i < arguments.size(); ++i)
            operands[i - 2] = toInt32(arguments[i]).value();
    }
    Pointer vector = findTypedArrayElement(typedArrayReceiver(view), index, slowPath, slowPath);
    LValue address = offsetBy(vector, index, logElementSize(elements)).value();
    TypedPointer pointer(m_heaps.TypedArrayProperties, address);
    FTL::TypedPointer cell(m_heaps.TypedArrayProperties, address);

    LValue access = nullptr;
    switch (which) {
    case InlineBuiltin::AtomicsAdd:
        access = m_out.atomicXchgAdd(operands[0], cell, width);
        break;
    case InlineBuiltin::AtomicsAnd:
        access = m_out.atomicXchgAnd(operands[0], cell, width);
        break;
    case InlineBuiltin::AtomicsCompareExchange:
        access = m_out.atomicStrongCAS(operands[0], operands[1], cell, width);
        break;
    case InlineBuiltin::AtomicsExchange:
        access = m_out.atomicXchg(operands[0], cell, width);
        break;
    case InlineBuiltin::AtomicsLoad:
        access = width == Width8 ? m_out.load8SignExt32(pointer) : width == Width16 ? m_out.load16SignExt32(pointer) : m_out.load32(pointer);
        break;
    case InlineBuiltin::AtomicsOr:
        access = m_out.atomicXchgOr(operands[0], cell, width);
        break;
    case InlineBuiltin::AtomicsStore:
        access = width == Width8 ? m_out.store32As8(operands[0], pointer) : width == Width16 ? m_out.store32As16(operands[0], pointer) : m_out.store32(operands[0], pointer);
        break;
    case InlineBuiltin::AtomicsSub:
        access = m_out.atomicXchgSub(operands[0], cell, width);
        break;
    case InlineBuiltin::AtomicsXor:
        access = m_out.atomicXchgXor(operands[0], cell, width);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    m_heaps.decorateFencedAccess(&m_heaps.TypedArrayProperties, access);
    ensureStillAliveHere(view);
    didEmitWrite(inlinePath);
    if (which == InlineBuiltin::AtomicsStore)
        join.add(convertToDeclaredResult(stored, Type::integer(Range::full()), value, type));
    else {
        LValue bits = access;
        if (elements == TypeUint8)
            bits = m_out.bitAnd(bits, m_out.constInt32(0xff));
        else if (elements == TypeUint16)
            bits = m_out.bitAnd(bits, m_out.constInt32(0xffff));
        Operand element = elements == TypeUint32 ? Operand(zeroExtended(Word32(bits))) : Operand(Word32(bits));
        join.add(convertToDeclaredResult(convert(element, elementType, repOf(elementType)), elementType, value, type));
    }
    m_out.appendTo(slowPath);
    join.add(convertToDeclaredResult(callBuiltinFunction(value, OperationEffects { std::max(inlinePath, builtinCallEffects(value).writes), true }), m_program.at(m_program.types.anything), value, type));
    return join.finish();
}

std::optional<Operand> Lowering::lowerInlineBuffer(InlineBuiltin which, ValueID result, const Value& value, Writes inlinePath)
{
    if (numberOfAtomicsOperands(which))
        return lowerAtomics(which, result, value, inlinePath);
    auto [isWrite, elements] = dataViewAccessFor(which);
    auto& arguments = value.arguments;
    auto& type = typeOf(result);
    unsigned size = elementSize(elements);
    ByteOrder order { false, { } };
    if (unsigned index = 1 + isWrite; size > 1 && arguments.size() > index) {
        uint16_t given = typeOf(arguments[index]).primitives;
        if (given & Primitive::True)
            order = given == Primitive::True ? ByteOrder { true, { } } : ByteOrder { std::nullopt, lowerToBoolean(arguments[index]) };
    }
    CellPointer view = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    Rep rep = repOf(type);
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    Integer offset = indexOrElse(arguments[0], slowPath);
    Operand stored;
    if (isWrite)
        stored = isBigInt(elements) ? Operand(low64Of(in<CellPointer>(arguments[1]))) : isFloat(elements) ? Operand(in<Number>(arguments[1])) : Operand(toInt32(arguments[1]));
    Pointer address = findBytes(view, offset, mayBeNegative(arguments[0]), size, slowPath);
    if (isWrite) {
        storeBytes(address, elements, stored, order);
        ensureStillAliveHere(view);
        join.add(convertToDeclaredResult(undefined(), m_program.at(m_program.types.undefined), value, type));
    } else {
        Operand element = loadBytes(address, elements, order);
        ensureStillAliveHere(view);
        join.add(convertToDeclaredResult(element, elementTypeOf(elements), value, type));
    }
    m_out.appendTo(slowPath);
    join.add(convertToDeclaredResult(callBuiltinFunction(value, OperationEffects { std::max(inlinePath, builtinCallEffects(value).writes), true }), m_program.at(m_program.types.anything), value, type));
    return join.finish();
}

static TypedArrayType elementsMovedBy(const BufferAccessor& accessor)
{
    switch (accessor.kind) {
    case BufferAccessorKind::Signed:
        return accessor.size == 1 ? TypeInt8 : accessor.size == 2 ? TypeInt16 : accessor.size == 4 ? TypeInt32 : TypeBigInt64;
    case BufferAccessorKind::Unsigned:
        return accessor.size == 1 ? TypeUint8 : accessor.size == 2 ? TypeUint16 : accessor.size == 4 ? TypeUint32 : TypeBigUint64;
    case BufferAccessorKind::Float:
        return accessor.size == 4 ? TypeFloat32 : TypeFloat64;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Store elementStore(TypedArrayType elements)
{
    switch (elements) {
    case TypeInt8:
        return Store::Int8;
    case TypeUint8:
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
    case TypeBigInt64:
    case TypeBigUint64:
        return Store::Int64;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool Lowering::mayBeNegative(ValueID id) const
{
    auto& type = typeOf(id);
    return !type.isInteger() || type.integers.minimum < 0;
}

Pointer Lowering::findBytes(CellPointer view, Integer offset, bool mayBeNegative, unsigned size, LBasicBlock otherwise)
{
    if (size > 1 && mayBeNegative)
        orElse(greaterThanOrEqual(offset, integer(0)), otherwise);
    LBasicBlock outside = newColdBlock();
    Pointer vector = findTypedArrayElement(typedArrayReceiver(view), add(offset, integer(size - 1)), otherwise, outside);
    LBasicBlock inside = m_out.m_block;
    m_out.appendTo(outside);
    m_out.jump(otherwise);
    m_out.appendTo(inside);
    return offsetBy(vector, offset, 0);
}

LValue Lowering::byteSwapped(LValue bits, unsigned size)
{
    PatchpointValue* patchpoint = m_out.patchpoint(size == 8 ? Int64 : Int32);
    patchpoint->appendSomeRegister(bits);
    patchpoint->effects = Effects::none();
    patchpoint->setGenerator([size](CCallHelpers& jit, const StackmapGenerationParams& params) {
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

LValue Lowering::inByteOrder(LValue bits, unsigned size, const ByteOrder& order)
{
    if (size == 1 || order.isLittle == true)
        return bits;
    LValue swapped = byteSwapped(bits, size);
    return order.isLittle ? swapped : m_out.select(order.isLittleAtRunTime.value(), bits, swapped);
}

Operand Lowering::loadBytes(Pointer address, TypedArrayType elements, const ByteOrder& order)
{
    TypedPointer pointer(m_heaps.TypedArrayProperties, address.value());
    unsigned size = elementSize(elements);
    Type elementType = elementTypeOf(elements);
    Operand element;
    if (size == 1 || order.isLittle == true)
        element = load(elementStore(elements), pointer, 0);
    else {
        LValue bits = inByteOrder(size == 2 ? m_out.load16ZeroExt32(pointer) : size == 4 ? m_out.load32(pointer) : m_out.load64(pointer), size, order);
        switch (elements) {
        case TypeInt16:
            element = Word32(m_out.aShr(m_out.shl(bits, m_out.constInt32(16)), m_out.constInt32(16)));
            break;
        case TypeUint16:
        case TypeInt32:
            element = Word32(bits);
            break;
        case TypeUint32:
            element = zeroExtended(Word32(bits));
            break;
        case TypeFloat32:
            element = Number(m_out.floatToDouble(m_out.m_block->appendNew<B3::Value>(m_proc, B3::BitwiseCast, m_out.origin(), bits)));
            break;
        case TypeFloat64:
            element = Number(m_out.bitCast(bits, Double));
            break;
        case TypeBigInt64:
        case TypeBigUint64:
            element = Integer(bits);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    if (isBigInt(elements))
        return newBigInt(element.as<Integer>(), elements == TypeBigInt64);
    return convert(element, elementType, repOf(elementType));
}

void Lowering::storeBytes(Pointer address, TypedArrayType elements, Operand stored, const ByteOrder& order)
{
    TypedPointer pointer(m_heaps.TypedArrayProperties, address.value());
    unsigned size = elementSize(elements);
    if (isFloat(elements) && order.isLittle == true) {
        store(elementStore(elements), stored, pointer);
        return;
    }
    LValue bits = stored.value;
    if (elements == TypeFloat32)
        bits = m_out.m_block->appendNew<B3::Value>(m_proc, B3::BitwiseCast, m_out.origin(), m_out.doubleToFloat(bits));
    else if (elements == TypeFloat64)
        bits = m_out.bitCast(bits, Int64);
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

Word32 Lowering::isNegativeBigInt(CellPointer bigInt)
{
    return logicalNot(hasNoBits(loadByte(bigInt, m_heaps.JSCell_typeInfoFlags), TypeInfoPerCellBit));
}

Integer Lowering::low64Of(CellPointer bigInt)
{
    return either<Integer>(logicalNot(isZero(loadField<Word32>(bigInt, m_heaps.JSBigInt_length))), [&] {
        Integer digit = loadField<Integer>(bigInt, m_heaps.JSBigInt_data);
        return select(isNegativeBigInt(bigInt), Integer(m_out.neg(digit.value())), digit);
    }, [&] {
        return integer(0);
    });
}

CellPointer Lowering::newBigInt(Integer bits, bool isSigned)
{
    constexpr auto kind = Instance::InlineAllocation::BigInt;
    static_assert(JSCell::typeInfoFlagsOffset() == 6 && JSBigInt::offsetOfLength() == sizeof(JSCell) && JSBigInt::offsetOfData() == JSBigInt::offsetOfLength() + sizeof(uint64_t));
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue allocator = m_out.loadPtr(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfAllocatorFor(kind)));
    orElse(m_out.notNull(allocator), slowPath);
    CellPointer cell = allocateCell(allocator, oneDigitBigIntSize, slowPath);
    LValue header = m_out.load64(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfHeaderFor(kind)));
    Integer magnitude = bits;
    if (isSigned) {
        Word32 isNegative = lessThan(bits, integer(0));
        magnitude = select(isNegative, Integer(m_out.neg(bits.value())), bits);
        header = m_out.select(isNegative.value(), m_out.bitOr(header, integer(static_cast<int64_t>(TypeInfoPerCellBit) << 48).value()), header);
    }
    m_out.store64(header, cell.value(), m_heaps.JSCell_header);
    m_out.store64(zeroExtended(isNotZero(bits)).value(), cell.value(), m_heaps.JSBigInt_length);
    m_out.store64(magnitude.value(), cell.value(), m_heaps.JSBigInt_data);
    fenceAfterInitialization();
    ValueFromBlock inlinePath = m_out.anchor(cell.value());
    m_out.jump(continuation);
    m_out.appendTo(slowPath);
    ValueFromBlock made = m_out.anchor(AOT_TYPED_OPERATION(NewBigInt64, bits, word32(isSigned)).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(pointerType(), inlinePath, made));
}

std::optional<unsigned> Lowering::bufferAccessorNamed(StringID name) const
{
    BuiltinName builtinName = m_program.builtinName(name);
    if (!builtinName)
        return std::nullopt;
    for (unsigned row = 0; row < std::size(bufferAccessors); ++row) {
        if (bufferAccessors[row].builtinName == builtinName.index())
            return row;
    }
    return std::nullopt;
}

Boxed Lowering::getBufferAccessor(Boxed base, StringID name, unsigned row, bool isCalled)
{
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newUnlikelyBlock();
    LBasicBlock mayProbe = newColdBlock();
    LBasicBlock look = newColdBlock();
    LBasicBlock read = newUnlikelyBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(base).value(), usually(isCellCase), rarely(read));
    m_out.appendTo(isCellCase);
    CellPointer cell = asCell(base);
    Word32 expected(loadRuntimeState32(Instance::offsetOfBufferAccessorStructureID(row)));
    m_out.branch(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), expected).value(), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    ValueFromBlock known = m_out.anchor(m_out.loadPtr(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfBufferAccessor(row))));
    m_out.jump(continuation);
    m_out.appendTo(miss);
    m_out.branch(isZero(expected).value(), rarely(mayProbe), usually(read));
    m_out.appendTo(mayProbe);
    m_out.branch(equal(loadCellType(cell), word32(Uint8ArrayType)).value(), unsure(look), unsure(read));
    m_out.appendTo(look);
    AOT_TYPED_OPERATION(FindBufferAccessor, cell, word32(row));
    m_out.jump(read);
    m_out.appendTo(read);
    ValueFromBlock found = m_out.anchor(emitCachedGetById(base, name, isCalled).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return Boxed(m_out.phi(Int64, known, found));
}

std::optional<Operand> Lowering::lowerBufferAccess(ValueID buffer, StringID name, std::optional<Boxed> method, const Arguments& arguments, const Type& type)
{
    auto row = bufferAccessorNamed(name);
    if (!row || arguments.size() > numberOfArgumentGPRs)
        return std::nullopt;
    auto& accessor = bufferAccessors[*row];
    auto& bufferType = typeOf(buffer);
    const EngineClass* engineClass = bufferType.withoutPrimitives(Primitive::UndefinedOrNull).singleEngineClass();
    bool isByteArray = engineClass && engineClass->type == Uint8ArrayType;
    if (!isByteArray && !bufferType.anyObject)
        return std::nullopt;
    if (auto expected = predictionOf(buffer); expected && expected->singleLayout())
        return std::nullopt;
    unsigned numberOfValues = accessor.isWrite;
    if (arguments.size() < numberOfValues)
        return std::nullopt;
    TypedArrayType elements = elementsMovedBy(accessor);
    Type elementType = elementTypeOf(elements);
    if (accessor.isWrite && !(isFloat(elements) || isBigInt(elements) ? !!(predictedTypeOf(arguments[0]).primitives & elementType.primitives) : mayBeIndex(arguments[0])))
        return std::nullopt;
    bool hasOffset = arguments.size() > numberOfValues;
    if (hasOffset && !mayBeIndex(arguments[numberOfValues]))
        return std::nullopt;
    ByteOrder order { accessor.isLittleEndian, { } };

    return ifPredictionHolds({ }, repOf(type), [&](LBasicBlock otherwise) {
        Operand receiver = get(buffer);
        if (receiver.rep != Rep::Cell)
            orElse(isCell(receiver.as<Boxed>(), bufferType), otherwise);
        CellPointer view = receiver.rep == Rep::Cell ? receiver.as<CellPointer>() : asCell(receiver.as<Boxed>());
        if (method) {
            orElse(equal(*method, Boxed(m_out.loadPtr(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfBufferAccessor(*row))))), otherwise);
            if (!isByteArray)
                orElse(equal(loadCellType(view), word32(Uint8ArrayType)), otherwise);
        } else
            orElse(equal(loadField<Word32>(view, m_heaps.JSCell_structureID), Word32(loadRuntimeState32(Instance::offsetOfBufferAccessorStructureID(*row)))), otherwise);

        Operand stored;
        if (accessor.isWrite && isFloat(elements))
            stored = speculateOrElse(Expectation { arguments[0], &elementType }, otherwise);
        else if (accessor.isWrite && isBigInt(elements)) {
            CellPointer bigInt = speculateOrElse(Expectation { arguments[0], &elementType }, otherwise).as<CellPointer>();
            orElse(belowOrEqual(loadField<Word32>(bigInt, m_heaps.JSBigInt_length), word32(1)), otherwise);
            Integer value = low64Of(bigInt);
            Word32 isNegative = isNegativeBigInt(bigInt);
            orElse(elements == TypeBigUint64 ? logicalNot(isNegative) : logicalOr(isZero(value), equal(lessThan(value, integer(0)), isNegative)), otherwise);
            stored = value;
        } else if (accessor.isWrite) {
            Integer value = indexOrElse(arguments[0], otherwise);
            Range range = elementType.integers;
            if (auto& valueType = typeOf(arguments[0]); !valueType.isInteger() || !valueType.integers.isSubsetOf(range))
                orElse(belowOrEqual(sub(value, integer(range.minimum)), integer(range.maximum - range.minimum)), otherwise);
            stored = low32(value);
        }

        Integer offset = hasOffset ? indexOrElse(arguments[numberOfValues], otherwise) : integer(0);
        Pointer address = findBytes(view, offset, hasOffset && mayBeNegative(arguments[numberOfValues]), accessor.size, otherwise);
        if (accessor.isWrite) {
            storeBytes(address, elements, stored, order);
            ensureStillAliveHere(view);
            return ensure(add(offset, integer(accessor.size)), Type::integer({ accessor.size, Range::full().maximum }), type);
        }
        Operand element = loadBytes(address, elements, order);
        ensureStillAliveHere(view);
        return ensure(element, elementType, type);
    }, [&] {
        Boxed receiver = in<Boxed>(buffer);
        return ensure(method ? callGeneric(*method, receiver, arguments) : callMethodGeneric(receiver, name, arguments), type);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
