/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElementEmitterAllocation.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTConvention.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTTypeTable.h"
#include "AOTValueIdentity.h"
#include "ArrayConventions.h"
#include "IndexingHeader.h"
#include "JSArray.h"
#include "MarkedSpace.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t largestDefaultOf(LimitID limit)
{
    return std::max(defaultLimitValue(limit, Architecture::ARM64), defaultLimitValue(limit, Architecture::X86_64));
}

static_assert(largestDefaultOf(LimitID::MaxInlineArrayLiteralLength) < (1u << holeArrayCapacityShift));
static_assert(((largestDefaultOf(LimitID::MaxPlannedArrayCapacity) | 1) << holeArrayCapacityShift) <= UINT32_MAX);

static Vector<uint32_t, 16> indexesWithoutHoles(unsigned length, std::span<const uint32_t> holes = { })
{
    Vector<uint32_t, 16> indexes;
    size_t nextHole = 0;
    for (uint32_t index = 0; index < length; ++index) {
        if (nextHole < holes.size() && holes[nextHole] == index)
            ++nextHole;
        else
            indexes.append(index);
    }
    return indexes;
}

static ArrayElementKind kindOf(const ArrayStorageForm& form)
{
    return arrayElementKindOf(form.indexingType);
}

static std::optional<EmptyArray> emptyArrayWithCapacity(const ArrayStorageForm& form, uint64_t capacity)
{
    if (capacity != 1 && capacity != 3)
        return std::nullopt;
    switch (kindOf(form)) {
    case ArrayElementKind::Contiguous:
        return capacity == 1 ? EmptyArray::ContiguousCapacity1OwnStorage : EmptyArray::ContiguousCapacity3OwnStorage;
    case ArrayElementKind::Double:
        return capacity == 1 ? EmptyArray::DoubleCapacity1 : EmptyArray::DoubleCapacity3;
    case ArrayElementKind::Int32:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static EmptyArray emptyNumberArray(const ArrayStorageForm& form)
{
    switch (kindOf(form)) {
    case ArrayElementKind::Int32:
        return EmptyArray::Int32DefaultCapacity;
    case ArrayElementKind::Double:
        return EmptyArray::DoubleDefaultCapacity;
    case ArrayElementKind::Contiguous:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}


template<typename T>
static Vector<T, 16> boxElements(CodeGenerator& generator, const OperationContext& context, std::span<const ValueID> elements)
{
    ConversionEmitter& conversions = generator.emitter<ConversionEmitter>();
    return Vector<T, 16>(elements.size(), [&](size_t index) -> T {
        ValueID element = elements[index];
        return conversions.box(context, generator.get(element), generator.typeOf(element));
    });
}

static Integer plannedCapacityOf(CodeGenerator& generator, const ValueContext& literal)
{
    ValueID capacity = literal->capacityOperand();
    Operand operand = generator.get(capacity);
    if (operand.value->hasInt())
        return Integer(generator.out().constInt64(operand.value->asInt()));
    if (operand.is<Word32>())
        return generator.emitter<ConversionEmitter>().widen(operand.as<Word32>(), generator.typeOf(capacity).integers);
    return operand.as<Integer>();
}

bool ArrayAllocationEmitter::isNumberOrMarker(ValueID element)
{
    if (representationOf(element) == Representation::OptionalDouble)
        return true;
    const Type& type = typeOf(element);
    return representationOf(element) == Representation::JSValue && !type.isNumber() && type.isSubsetOf(Type::fromPrimitives(Primitive::Number | Primitive::Undefined), m_program);
}

const ArrayStorageForm& ArrayAllocationEmitter::formAtAllocation(const Type& array) const
{
    return arrayStorageFormOf(arrayStorageAtAllocation(m_program, array));
}

const ArrayStorageForm& ArrayAllocationEmitter::formOfLiteral(const OperationContext& context, std::span<const ValueID> elements, const Type& array, bool hasHoles)
{
    // arrayStorageOfLiteral() returns nothing for an array created with no elements and no holes, unless it is declared to use storage for any value.
    if (elements.empty() && !hasHoles)
        return formOfEmptyLiteral(context.valueID(), array);
    if (const ArrayStorageForm* form = findFormOfLiteral(elements, array))
        return *form;
    fail(context, InternalErrorKind::OpcodeWithoutCode);
    return arrayStorageFormOf(ArrayStorage::Contiguous);
}

// Null where the declared storage cannot hold the elements.
const ArrayStorageForm* ArrayAllocationEmitter::findFormOfLiteral(std::span<const ValueID> elements, const Type& array)
{
    const ArrayStorageForm& form = formAtAllocation(array);
    // A literal with an element that never arrives is never made.
    bool isNeverMade = std::ranges::any_of(elements, [&](ValueID element) {
        return typeOf(element).isNever();
    });
    bool canHoldElements = !isNeverMade && std::ranges::all_of(elements, [&](ValueID element) {
        return (form.element == Representation::Double && isNumberOrMarker(element)) || fitsStorageOfLiteral(form.kind, typeOf(element));
    });
    if (canHoldElements)
        return &form;
    const ClassPart* part = array.singleBuiltinClass();
    if (!isNeverMade && part && holdsOnlyNumbers(part->arrayStorage))
        return nullptr;
    return &arrayStorageFormOf(ArrayStorage::Contiguous);
}

CellPointer ArrayAllocationEmitter::newLiteral(const ValueContext& value)
{
    if (value->storage() != AllocationStorage::Heap)
        return emitter<CellAllocationEmitter>().newArrayOf(value, boxElements<Boxed>(generator(), value, value->elementsOperands()).span(), value->storage());

    switch (value->literalStorage()) {
    case ArrayLiteralStorage::Fresh:
        break;
    case ArrayLiteralStorage::Profiled:
        if (std::optional<StaticsOffset> offset = value->staticsOffset(); offset && value->elementsOperands().empty() && value->holes().empty())
            return newLiteralAtSite(value, *offset);
        break;
    }
    if (!value->holes().empty())
        return newLiteralWithHoles(value);
    if (value->capacityOperand())
        return newLiteralWithCapacity(value);
    return newArrayOf(value, value->elementsOperands(), typeOf(value.id()));
}

Integer ArrayAllocationEmitter::headerOfNewArray(const ArrayStorageForm& form)
{
    return loadMutable64(Instance::offsetOfNewArrayHeader(form.indexingType));
}

// An empty array with room for `capacity` elements, which nobody has written yet. Whoever gives up on it leaves an array that the collector can visit.
CellPointer ArrayAllocationEmitter::allocateEmptyArrayWithStorage(const OperationContext& context, Integer header, Integer capacity, LBasicBlock slowPath)
{
    CellAllocationEmitter& cells = emitter<CellAllocationEmitter>();
    LBasicBlock isExhausted = newBlock();
    Join<Pointer> join(m_out);
    Integer sizeInBytes = add(mul(capacity, sizeof(EncodedJSValue)), sizeOfArrayWithStorage(0));
    Pointer allocator = cells.allocatorForSize(loadImmutablePointer(AOT_INSTANCE_OFFSET(allocation.cellSpace)), sizeInBytes, slowPath, isExhausted);
    join.add(asPointer(cells.allocateHeapCell(allocator, std::nullopt, isExhausted)));

    m_out.appendTo(isExhausted);
    Pointer allocated = callOperation<Pointer>(context, RuntimeOperation::AllocateStorage, { sizeInBytes }, { .isOnRarePath = true });
    branchUnless(Condition(m_out.notNull(allocated.value())), slowPath);
    join.add(allocated);

    CellPointer array(join.finish().value());
    CodeGenerator::InitializationScope scope(generator());
    Pointer butterfly = offsetBy(asPointer(array), sizeOfArrayWithStorage(0));
    cells.storeHeader(array, header);
    m_out.storePtr(butterfly.value(), array.value(), m_heaps.JSArray_butterfly);
    m_out.store32(m_out.int32Zero, butterfly.value(), m_heaps.Butterfly_publicLength);
    m_out.store32(low32(capacity).value(), butterfly.value(), m_heaps.Butterfly_vectorLength);
    return array;
}

CellPointer ArrayAllocationEmitter::allocateArrayCell(Integer header, Pointer butterfly, LBasicBlock slowPath)
{
    static_assert(sizeof(JSArray) <= MarkedSpace::preciseCutoff && !(sizeof(JSArray) % MarkedSpace::sizeStep));
    CellAllocationEmitter& cells = emitter<CellAllocationEmitter>();
    CellPointer array = cells.allocateHeapCell(loadImmutablePointer(AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator)), sizeof(JSArray), slowPath);
    {
        CodeGenerator::InitializationScope scope(generator());
        cells.storeHeader(array, header);
        m_out.storePtr(butterfly.value(), array.value(), m_heaps.JSArray_butterfly);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    return array;
}

CellPointer ArrayAllocationEmitter::allocateArrayOfHoles(const OperationContext& context, Integer length, const ArrayStorageForm& form, LBasicBlock slowPath, ElementsBelowLength elementsBelowLength)
{
    auto setLength = [&](CellPointer array) {
        CodeGenerator::InitializationScope scope(generator());
        m_out.store32(low32(length).value(), m_out.loadPtr(array.value(), m_heaps.JSArray_butterfly), m_heaps.Butterfly_publicLength);
        return array;
    };
    // `new Array(3)`: the stub for an empty array with that room knows its size and its allocator.
    if (length.value()->hasInt64() && length.value()->asInt64() >= 1 && length.value()->asInt64() <= 3 && kindOf(form) != ArrayElementKind::Int32) {
        bool holdsOne = length.value()->asInt64() == 1;
        if (kindOf(form) == ArrayElementKind::Double)
            return setLength(callNewEmptyArray(context, holdsOne ? EmptyArray::DoubleCapacity1 : EmptyArray::DoubleCapacity3));
        return setLength(callNewEmptyArray(context, holdsOne ? EmptyArray::ContiguousCapacity1 : EmptyArray::ContiguousCapacity3));
    }
    branchUnless(belowOrEqual(length, integer(maxHoleArrayCapacity)), slowPath);
    // Nobody makes an array of no elements to leave it so.
    bool isEmpty = length.value()->hasInt64() && !length.value()->asInt64();
    Integer capacity = isEmpty ? integer(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY | 1) : bitOr(length, integer(1));
    Integer lengthAndCapacity(m_out.shl(capacity.value(), m_out.constInt32(holeArrayCapacityShift)));
    // The stub sets the length that it is handed and puts holes behind it.
    static_assert(maxHoleArrayCapacity < 1 << holeArrayCapacityShift);
    if (elementsBelowLength == ElementsBelowLength::NotCleared)
        lengthAndCapacity = bitOr(lengthAndCapacity, length);
    CellPointer array = callNewArrayWithLength(context, form, lengthAndCapacity);
    branchUnless(Condition(m_out.notNull(array.value())), slowPath);
    return elementsBelowLength == ElementsBelowLength::NotCleared ? array : setLength(array);
}

CellPointer ArrayAllocationEmitter::newArrayOfHoles(const OperationContext& context, Integer length, const ArrayStorageForm& form, BuiltinID calledBuiltin)
{
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    join.add(allocateArrayOfHoles(context, length, form, slowPath));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::NewArrayWithHoles, { CellPointer(m_out.int64Zero), length, word32(form.indexingType), word32(0) }, { .calledBuiltin = calledBuiltin, .isOnRarePath = true }));
    return join.finish();
}

CellPointer ArrayAllocationEmitter::callNewEmptyArray(const OperationContext& context, EmptyArray emptyArray)
{
    return CellPointer(callStub(context, newEmptyArray(emptyArray), { }).first);
}

CellPointer ArrayAllocationEmitter::callNewArrayWithLength(const OperationContext& context, const ArrayStorageForm& form, Integer lengthAndCapacity)
{
    return CellPointer(callStub(context, newArrayWithLength(kindOf(form)), { lengthAndCapacity.value() }).first);
}

Number ArrayAllocationEmitter::numberOrMarker(ValueID element)
{
    Operand operand = get(element);
    switch (operand.representation) {
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalDouble:
        return Number(operand.value);
    case Representation::Int32:
        return toNumber(operand.as<Word32>());
    case Representation::JSValue:
        return Number(emitter<ConversionEmitter>().unbox(operand.as<Boxed>(), typeOf(element), isNumberOrMarker(element) ? Representation::OptionalDouble : Representation::Double).value);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CellPointer ArrayAllocationEmitter::newArrayOutOfLine(const OperationContext& context, std::span<const ValueID> elements, const ArrayStorageForm& form)
{
    unsigned length = elements.size();
    Word32 count = word32(length);
    if (form.element != Representation::Double || !length) {
        Pointer values = argumentBuffer(boxElements<Boxed>(generator(), context, elements).span());
        return callOperation<CellPointer>(context, RuntimeOperation::NewArrayWithIndexingType, { count, values, word32(form.indexingType) }, { .isOnRarePath = true });
    }

    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Vector<Number, 16> rawNumbers(length, [&](size_t index) {
        ValueID element = elements[index];
        Number rawNumber = numberOrMarker(element);
        bool mayLookLikeMarker = !isNumberOrMarker(element) && typeOf(element).mayBeNaN();
        return mayLookLikeMarker ? conversions.withoutPayload(rawNumber) : rawNumber;
    });
    Pointer values = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(length);
    for (unsigned i = 0; i < length; ++i)
        m_out.storeDouble(rawNumbers[i].value(), m_out.address(m_heaps.argumentBuffer, values.value(), i * sizeof(EncodedJSValue)));
    return callOperation<CellPointer>(context, RuntimeOperation::NewArrayWithIndexingType, { count, values, word32(form.indexingType | valuesAreRawNumbers) }, { .isOnRarePath = true });
}

CellPointer ArrayAllocationEmitter::newDoubleLiteral(const OperationContext& context, std::span<const Number> numbersOrMarkers, uint32_t isNumberMask)
{
    unsigned count = numbersOrMarkers.size();
    RELEASE_ASSERT(count && count <= maxArrayElementsInRegisters);
    Vector<LValue, 1 + maxArrayElementsInRegisters> operands;
    operands.append(m_out.constInt64(count | isNumberMask << arrayLiteralWithDoubleIsNumberMaskShift));
    for (Number element : numbersOrMarkers)
        operands.append(element.value());
    return CellPointer(callStub(context, arrayLiteralWithDouble(count), operands.span()).first);
}

CellPointer ArrayAllocationEmitter::newArrayOfNumbers(const OperationContext& context, std::span<const Number> numbers)
{
    RELEASE_ASSERT(numbers.size() <= maxArrayElementsInRegisters);
    if (numbers.empty())
        return callNewEmptyArray(context, EmptyArray::DoubleDefaultCapacity);
    return newDoubleLiteral(context, numbers, (1u << numbers.size()) - 1);
}

// An element that never fits is left out, so a result shorter than the list means the form cannot hold the list.
ArrayAllocationEmitter::RawElements ArrayAllocationEmitter::rawElementsOf(std::span<const ValueID> elements, const ArrayStorageForm& form, LBasicBlock doesNotFit)
{
    ElementAccessEmitter& access = emitter<ElementAccessEmitter>();
    RawElements rawElements;
    for (ValueID element : elements) {
        if (form.element == Representation::Double && isNumberOrMarker(element)) {
            Number number = numberOrMarker(element);
            branchUnless(equal(number, number), doesNotFit);
            rawElements.append(number);
            continue;
        }
        if (std::optional<Operand> rawElement = access.rawElementToStore(form, element, doesNotFit))
            rawElements.append(*rawElement);
    }
    return rawElements;
}

void ArrayAllocationEmitter::storeElements(CellPointer newArray, const ArrayStorageForm& form, std::span<const Operand> rawElements, std::span<const uint32_t> indexes)
{
    RELEASE_ASSERT(rawElements.size() == indexes.size());
    if (rawElements.empty())
        return;
    ElementAccessEmitter& access = emitter<ElementAccessEmitter>();
    {
        CodeGenerator::InitializationScope scope(generator());
        ArrayElements elements = access.elementsOf(newArray);
        for (size_t i = 0; i < rawElements.size(); ++i)
            access.storeRawElement(form, elements, integer(indexes[i]), rawElements[i]);
    }
    if (form.holdsCells)
        emitter<BarrierEmitter>().fenceAfterInitialization();
}

CellPointer ArrayAllocationEmitter::newArrayOf(const OperationContext& context, std::span<const ValueID> elements, const Type& array)
{
    unsigned length = elements.size();
    const ArrayStorageForm& form = formOfLiteral(context, elements, array);
    bool exceedsInlineLength = build().exceeds(LimitID::MaxInlineArrayLiteralLength, length, analyses().subject(context.valueID()));
    if (form.kind == ArrayStorage::Contiguous && !exceedsInlineLength)
        return emitter<CellAllocationEmitter>().newArrayOf(context, boxElements<Boxed>(generator(), context, elements).span());
    if (exceedsInlineLength)
        return newArrayOutOfLine(context, elements, form);

    bool holdsDoubles = form.element == Representation::Double;
    if (holdsDoubles && !length)
        return callNewEmptyArray(context, EmptyArray::DoubleDefaultCapacity);
    if (holdsDoubles && length <= maxArrayElementsInRegisters) {
        Vector<Number, maxArrayElementsInRegisters> numbersOrMarkers;
        uint32_t isNumberMask = 0;
        for (unsigned i = 0; i < length; ++i) {
            numbersOrMarkers.append(numberOrMarker(elements[i]));
            isNumberMask |= static_cast<uint32_t>(!isNumberOrMarker(elements[i])) << i;
        }
        return newDoubleLiteral(context, numbersOrMarkers.span(), isNumberMask);
    }
    bool areInt32 = std::ranges::all_of(elements, [&](ValueID element) {
        return representationOf(element) == Representation::Int32;
    });
    if (form.kind == ArrayStorage::Int32 && length && length <= maxArrayElementsInRegisters && areInt32) {
        Vector<LValue, maxArrayElementsInRegisters> operands;
        for (ValueID element : elements)
            operands.append(zeroExtended(get(element).as<Word32>()).value());
        return CellPointer(callStub(context, arrayLiteralWithInt32(length), operands.span()).first);
    }

    if (storesElementsBehindAllocation(elements, form))
        return newArrayStoredBehindAllocation(context, elements, form);

    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    RawElements rawElements = rawElementsOf(elements, form, slowPath);
    if (rawElements.size() != length) {
        fail(context, InternalErrorKind::OpcodeWithoutCode);
        return CellPointer(m_out.int64Zero);
    }
    if (length && length <= maxArrayLengthInStubKey) {
        CellAllocationEmitter& cells = emitter<CellAllocationEmitter>();
        CellPointer newArray = cells.allocateArrayOfLength(context, form.indexingType, length);
        storeElements(newArray, form, rawElements.span(), indexesWithoutHoles(length).span());
        cells.finishArrayOfLength(context, newArray, length);
        join.add(newArray);
    } else {
        CellPointer newArray = length
            ? callNewArrayWithLength(context, form, integer(length | arrayLiteralVectorLength(length) << holeArrayCapacityShift))
            : callNewEmptyArray(context, emptyNumberArray(form));
        branchUnless(Condition(m_out.notNull(newArray.value())), slowPath);
        storeElements(newArray, form, rawElements.span(), indexesWithoutHoles(length).span());
        join.add(newArray);
    }

    m_out.appendTo(slowPath);
    join.add(newArrayOutOfLine(context, elements, form));
    return join.finish();
}

// No element needs a test, so nothing sends the literal elsewhere once the array is allocated.
bool ArrayAllocationEmitter::storesElementsBehindAllocation(std::span<const ValueID> elements, const ArrayStorageForm& form)
{
    if (elements.size() <= maxArrayElementsInRegisters || elements.size() > maxArrayLengthInStubKey || !holdsOnlyNumbers(form.kind))
        return false;
    return std::ranges::all_of(elements, [&](ValueID element) {
        switch (m_function.at(element).representation) {
        case Representation::Int32:
            return true;
        case Representation::Double:
            return form.kind == ArrayStorage::Double && canStoreWithoutConversion(form.kind, typeOf(element));
        default:
            return false;
        }
    });
}

CellPointer ArrayAllocationEmitter::newArrayStoredBehindAllocation(const OperationContext& context, std::span<const ValueID> elements, const ArrayStorageForm& form)
{
    ElementAccessEmitter& access = emitter<ElementAccessEmitter>();
    CellAllocationEmitter& cells = emitter<CellAllocationEmitter>();
    unsigned length = elements.size();
    CellPointer newArray = cells.allocateArrayOfLength(context, form.indexingType, length);
    ArrayElements storage = access.elementsOf(newArray);
    for (unsigned i = 0; i < length; ++i) {
        generator().generateValueLeftToUser(context, elements[i]);
        Operand rawElement = *access.rawElementToStore(form, elements[i], nullptr);
        CodeGenerator::InitializationScope scope(generator());
        access.storeRawElement(form, storage, integer(i), rawElement);
    }
    cells.finishArrayOfLength(context, newArray, length);
    return newArray;
}

static bool isArithmeticOnInt32(const Value& value)
{
    if (value.representation != Representation::Int32)
        return false;
    switch (value.opcode) {
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
        return value.form() == OperatorForm::Integers || value.form() == OperatorForm::Numbers;
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
        return value.form() == OperatorForm::Integers;
    default:
        return false;
    }
}

bool ArrayAllocationEmitter::isComputedBehindAllocation(ValueID id)
{
    AOT_THRESHOLD(unsigned, maxOperationsOfElement, 4);
    AOT_THRESHOLD(uint32_t, maxValuesBetweenElementAndLiteral, 64);
    // A checking build looks at the operands of the literal in front of it.
    if (generator().validatesOrChecksProofs() || generator().checksKeptRegisters())
        return false;
    const UseLists& useLists = analyses().useLists();
    ValueLocation location = useLists.locationOf(id);
    for (unsigned depth = 0; depth < maxOperationsOfElement; ++depth) {
        const Value& value = m_function.at(id);
        if (!isArithmeticOnInt32(value))
            return false;
        // Computed late, a value of two variables would hold both across the call in place of itself.
        unsigned numberOfVariables = 0;
        for (uint32_t i = 0; i < value.numberOfOperands(); ++i) {
            const Value& operand = m_function.at(value.operand(i));
            if (operand.representation != Representation::Int32)
                return false;
            numberOfVariables += !isRematerializable(operand);
        }
        std::optional<Use> use = useLists.singleUse(id);
        if (numberOfVariables > 1 || !use || use->kind != Use::Kind::Operand)
            return false;
        id = use->user;
        const Value& user = m_function.at(id);
        if (user.opcode != Opcode::NewArray)
            continue;

        ValueLocation locationOfLiteral = useLists.locationOf(id);
        if (locationOfLiteral.block != location.block || locationOfLiteral.index - location.index > maxValuesBetweenElementAndLiteral)
            return false;
        // What is alive across a suspension is saved with the frame: the value, not what it is computed from.
        std::span<const ValueID> between = m_function.at(location.block).values.span().subspan(location.index, locationOfLiteral.index - location.index);
        if (std::ranges::any_of(between, [&](ValueID other) { return maySuspend(m_function.at(other).opcode); }))
            return false;
        std::span<const ValueID> elements = user.elementsOperands();
        if (user.storage() != AllocationStorage::Heap || !user.holes().empty() || user.capacityOperand() || elements.size() > build().limit(LimitID::MaxInlineArrayLiteralLength))
            return false;
        const ArrayStorageForm* form = findFormOfLiteral(elements, typeOf(id));
        return form && storesElementsBehindAllocation(elements, *form);
    }
    return false;
}

CellPointer ArrayAllocationEmitter::newLiteralWithHoles(const ValueContext& value)
{
    std::span<const ValueID> elements = value->elementsOperands();
    const ArrayStorageForm& form = formOfLiteral(value, elements, typeOf(value.id()), true);
    unsigned length = elements.size() + value->holes().size();
    Vector<uint32_t, 16> indexes = indexesWithoutHoles(length, value->holes());
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    RawElements rawElements = form.element == Representation::JSValue ? boxElements<Operand>(generator(), value, elements) : rawElementsOf(elements, form, slowPath);
    if (rawElements.size() != elements.size()) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return CellPointer(m_out.int64Zero);
    }
    CellPointer newArray = allocateArrayOfHoles(value, integer(length), form, slowPath);
    storeElements(newArray, form, rawElements.span(), indexes.span());
    join.add(newArray);

    m_out.appendTo(slowPath);
    uint32_t flags = value->keepsLength() ? NewArrayWithHolesFlag::mustHaveIndexingType : 0;
    CellPointer arrayOfHoles = callOperation<CellPointer>(value, RuntimeOperation::NewArrayWithHoles, { CellPointer(m_out.int64Zero), integer(length), word32(form.indexingType), word32(flags) }, { .isOnRarePath = true });
    RawElements boxedElements = boxElements<Operand>(generator(), value, elements);
    for (size_t i = 0; i < boxedElements.size(); ++i)
        callVoidOperation(value, RuntimeOperation::ArrayPutAtIndex, { arrayOfHoles, boxedElements[i], integer(indexes[i]) }, { .isOnRarePath = true });
    join.add(arrayOfHoles);
    return join.finish();
}

CellPointer ArrayAllocationEmitter::newLiteralWithCapacity(const ValueContext& value)
{
    std::span<const ValueID> elements = value->elementsOperands();
    const Type& array = typeOf(value.id());
    unsigned length = elements.size();
    if (length > build().limit(LimitID::MaxInlineArrayLiteralLength))
        return newArrayOf(value, elements, array);
    uint64_t maxCapacity = build().limit(LimitID::MaxPlannedArrayCapacity);
    Integer capacity = plannedCapacityOf(generator(), value);
    const ArrayStorageForm& form = formOfLiteral(value, elements, array);
    if (!length && capacity.value()->hasInt64()) {
        uint64_t constantCapacity = capacity.value()->asInt64();
        if (constantCapacity > maxCapacity || (constantCapacity | 1) == arrayLiteralVectorLength(0))
            return newArrayOf(value, elements, array);
        if (std::optional<EmptyArray> emptyArray = emptyArrayWithCapacity(form, constantCapacity | 1))
            return callNewEmptyArray(value, *emptyArray);
    }

    LBasicBlock fallback = newBlock();
    Join<CellPointer> join(m_out);
    RawElements rawElements = form.element == Representation::JSValue ? boxElements<Operand>(generator(), value, elements) : rawElementsOf(elements, form, fallback);
    if (rawElements.size() != length) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return CellPointer(m_out.int64Zero);
    }
    branchUnless(belowOrEqual(capacity, integer(maxCapacity)), fallback);
    Integer total = length ? select(lessThan(capacity, length), length, capacity) : capacity;
    Integer lengthAndCapacity(m_out.shl(bitOr(total, 1).value(), holeArrayCapacityShift));
    if (length)
        lengthAndCapacity = bitOr(lengthAndCapacity, integer(length));
    CellPointer newArray = callNewArrayWithLength(value, form, lengthAndCapacity);
    branchUnless(Condition(m_out.notNull(newArray.value())), fallback);
    storeElements(newArray, form, rawElements.span(), indexesWithoutHoles(length).span());
    join.add(newArray);

    m_out.appendTo(fallback);
    join.add(newArrayOf(value, elements, array));
    return join.finish();
}

const ArrayStorageForm& ArrayAllocationEmitter::formOfEmptyLiteral(ValueID literal, const Type& array)
{
    static_assert(ArrayStorage::Int32 < ArrayStorage::Double && ArrayStorage::Double < ArrayStorage::Contiguous);
    if (arrayStorageDeclaredBy(m_program, array).predicted() != ArrayStorage::Other)
        return formAtAllocation(array);

    std::optional<ArrayStorage> pushed;
    for (const Use& use : analyses().useLists().usesOf(literal)) {
        if (use.kind != Use::Kind::Operand)
            continue;
        const Value& user = m_function.at(use.user);
        if (user.opcode != Opcode::CallInline || user.inlineBuiltin() != InlineBuiltin::ArrayPush || user.receiverOperand() != literal)
            continue;
        for (ValueID argument : user.argumentsOperands())
            pushed = std::max(pushed.value_or(ArrayStorage::Int32), arrayStorageAtAllocation(typeOf(argument)));
    }
    return pushed ? arrayStorageFormOf(*pushed) : formAtAllocation(array);
}

CellPointer ArrayAllocationEmitter::newLiteralAtSite(const ValueContext& value, StaticsOffset offset)
{
    LValue profile = m_out.valueOf(emitter<StaticsEmitter>().inStatics(m_heaps.entire(AbstractHeap::RuntimeState), offset));
    return CellPointer(callStub(value, newEmptyArrayAtSite(kindOf(formOfEmptyLiteral(value.id(), typeOf(value.id())))), { profile }).first);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
