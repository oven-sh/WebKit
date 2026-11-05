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
#include "AOTDiagnostics.h"
#include "AOTElementAccessEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "ArrayConventions.h"
#include "IndexingHeader.h"
#include "JSArray.h"
#include "MarkedSpace.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned maxInlineArrayLiteralLength = 16;
static constexpr int64_t maxCapacityInStubOperand = (1 << (32 - holeArrayCapacityShift)) - 1;

static_assert(maxInlineArrayLiteralLength < (1u << holeArrayCapacityShift));
static_assert(maxCapacityInStubOperand & 1);

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

static ArrayElementKind elementKindOfForm(const ArrayStorageForm& form)
{
    return arrayElementKindOf(form.indexingType);
}

static std::optional<EmptyArray> emptyArrayStubWithRoom(const ArrayStorageForm& form, uint64_t room)
{
    if (room != 1 && room != 3)
        return std::nullopt;
    switch (elementKindOfForm(form)) {
    case ArrayElementKind::Contiguous:
        return room == 1 ? EmptyArray::ContiguousCapacity1 : EmptyArray::ContiguousCapacity3;
    case ArrayElementKind::Double:
        return room == 1 ? EmptyArray::DoubleCapacity1 : EmptyArray::DoubleCapacity3;
    case ArrayElementKind::Int32:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static EmptyArray emptyNumberArray(const ArrayStorageForm& form)
{
    switch (elementKindOfForm(form)) {
    case ArrayElementKind::Int32:
        return EmptyArray::Int32DefaultCapacity;
    case ArrayElementKind::Double:
        return EmptyArray::DoubleDefaultCapacity;
    case ArrayElementKind::Contiguous:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<uint32_t> kindOfEmptyArrayLiteralAtSite(const Operation& literal, const ArrayStorageForm& form)
{
    bool kindIsDecided = false;
    switch (literal.literalStorage()) {
    case ArrayLiteralStorage::Fresh:
        return std::nullopt;
    case ArrayLiteralStorage::Profiled:
        break;
    case ArrayLiteralStorage::ProfiledForRoom:
        kindIsDecided = true;
        break;
    }
    if (!literal.elementsOperands().empty() || !literal.holes().empty() || literal.capacityOperand())
        return std::nullopt;
    return static_cast<uint32_t>(elementKindOfForm(form)) << 1 | static_cast<uint32_t>(kindIsDecided);
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

bool ArrayAllocationEmitter::isNumberOrMarker(ValueID element)
{
    if (representationOf(element) == Representation::OptionalDouble)
        return true;
    const Type& type = typeOf(element);
    return representationOf(element) == Representation::JSValue && !type.isNumber() && type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
}

static const ArrayStorageForm& formAtAllocationOfStorageDeclared(ArrayStorage declared)
{
    return arrayStorageFormOf(hasArrayStorageForm(declared) ? declared : ArrayStorage::Contiguous);
}

const ArrayStorageForm& ArrayAllocationEmitter::formAtAllocation(const Type& array) const
{
    TypeArena& arena = m_generator.inputs().types;
    return formAtAllocationOfStorageDeclared(arrayStorageDeclaredBy(m_program, arena, arena.intern(array)).predicted());
}

const ArrayStorageForm& ArrayAllocationEmitter::formOfLiteral(const OperationContext& context, std::span<const ValueID> elements, const Type& array)
{
    return formOfLiteral(context, elements, array, ArrayStorage::Other);
}

const ArrayStorageForm& ArrayAllocationEmitter::formOfLiteral(const OperationContext& context, std::span<const ValueID> elements, const Type& array, ArrayStorage named, const ArrayStorage* declaredByArray)
{
    if (const ArrayStorageForm* form = findFormOfLiteral(elements, array, named, declaredByArray))
        return *form;
    generator().fail(context, InternalErrorKind::OpcodeWithoutCode, "an element is not proven to fit the storage that the array literal names"_s);
    return arrayStorageFormOf(ArrayStorage::Contiguous);
}

const ArrayStorageForm* ArrayAllocationEmitter::findFormOfLiteral(std::span<const ValueID> elements, const Type& array, ArrayStorage named, const ArrayStorage* declaredByArray)
{
    bool isNeverMade = std::ranges::any_of(elements, [&](ValueID element) {
        return isNeverType(typeIDOf(element));
    });
    if (isNeverMade)
        return &arrayStorageFormOf(ArrayStorage::Contiguous);

    if (const ArrayStorageForm* namedForm = findArrayStorageForm(named)) {
        bool fitsNamedStorage = std::ranges::all_of(elements, [&](ValueID element) {
            return fitsStorageOfLiteral(types(), named, typeIDOf(element));
        });
        return fitsNamedStorage ? namedForm : nullptr;
    }

    const ArrayStorageForm& form = declaredByArray ? formAtAllocationOfStorageDeclared(*declaredByArray) : formAtAllocation(array);
    bool canHoldElements = std::ranges::all_of(elements, [&](ValueID element) {
        return (form.element == Representation::Double && isNumberOrMarker(element)) || fitsStorageOfLiteral(types(), form.kind, typeIDOf(element));
    });
    return canHoldElements ? &form : &arrayStorageFormOf(ArrayStorage::Contiguous);
}

bool newArrayFencesBeforeHandingCellOn(const Operation& newArray)
{
    size_t length = newArray.elementsOperands().size();
    return newArray.storage() == AllocationStorage::Heap && newArray.holes().empty() && !newArray.capacityOperand() && newArray.arrayStorage() == ArrayStorage::Contiguous
        && length > maxArrayElementsInRegisters && length <= std::min<size_t>(maxInlineArrayLiteralLength, maxArrayLengthInStubKey);
}

CellPointer ArrayAllocationEmitter::newLiteral(const ValueContext& value)
{
    if (value->storage() == AllocationStorage::Frame) {
        generator().fail(value, InternalErrorKind::OpcodeWithoutCode, "the frame has no form for an array literal"_s);
        return CellPointer(m_out.int64Zero);
    }

    if (!value->holes().empty())
        return newLiteralWithHoles(value);
    std::span<const ValueID> elements = value->elementsOperands();
    TypeArena& arena = m_generator.inputs().types;
    const ArrayStorage named = value->arrayStorage();
    const ArrayStorage declared = hasArrayStorageForm(named) ? ArrayStorage::Other : arrayStorageDeclaredBy(m_program, arena, typeIDOf(value.id())).predicted();
    const ArrayStorageForm& form = formOfLiteral(value, elements, typeOf(value.id()), named, &declared);
    unsigned length = elements.size();
    bool storageIsDeclared = hasArrayStorageForm(named) || hasArrayStorageForm(declared);
    const ArrayStorageForm& formAtSite = storageIsDeclared ? form : arrayStorageFormOf(ArrayStorage::Int32);
    std::optional<uint32_t> kindAtSite = kindOfEmptyArrayLiteralAtSite(value.value(), formAtSite);
    if (std::optional<Vector<uint32_t, 12>> site = kindAtSite ? modulesAndPositionsOf(value) : std::nullopt) {
        site->append(*kindAtSite);
        return newLiteralAtSite(value, formAtSite, site->span());
    }
    if (!value->capacityOperand() || length > maxInlineArrayLiteralLength)
        return newArrayInForm(value, elements, length ? form : formAtSite);

    Integer capacity = integerOf(value->capacityOperand());
    if (!length && capacity.value()->hasInt64()) {
        uint64_t room = static_cast<uint64_t>(capacity.value()->asInt64()) | 1;
        if (room > static_cast<uint64_t>(maxCapacityInStubOperand) || room == arrayLiteralVectorLength(0))
            return newArrayInForm(value, elements, form);
        if (std::optional<EmptyArray> emptyArray = emptyArrayStubWithRoom(form, room))
            return callNewEmptyArray(value, *emptyArray);
    }
    if (!length && form.kind == ArrayStorage::Contiguous && !value->isFilledToCapacity())
        return CellPointer(callStub(value, newEmptyArrayWithCapacity(), { capacity.value() }).first);

    LBasicBlock withoutRoom = newBlock();
    Join<CellPointer> join(m_out);
    RawElements rawElements = form.element == Representation::JSValue ? boxElements<Operand>(generator(), value, elements) : rawElementsOf(elements, form, withoutRoom);
    if (rawElements.size() != length) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return CellPointer(m_out.int64Zero);
    }
    branchUnless(belowOrEqual(capacity, integer(maxCapacityInStubOperand)), withoutRoom);
    Integer room = length ? select(below(capacity, integer(length)), integer(length), capacity) : capacity;
    Integer lengthAndCapacity(m_out.shl(bitOr(room, integer(1)).value(), m_out.constInt32(holeArrayCapacityShift)));
    if (length)
        lengthAndCapacity = bitOr(lengthAndCapacity, integer(length));
    CellPointer newArray = callNewArrayWithLength(value, form, lengthAndCapacity);
    branchUnless(Condition(m_out.notNull(newArray.value())), withoutRoom);
    storeElements(value, newArray, form, rawElements.span(), indexesWithoutHoles(length).span());
    join.add(newArray);

    m_out.appendTo(withoutRoom);
    CellPointer madeWithoutRoom = newArrayOutOfLine(value, elements, form);
    if (value->isFilledToCapacity()) {
        LBasicBlock reserves = newBlock();
        LBasicBlock isMade = newBlock();
        m_out.branch(below(integer(length), capacity).value(), normalEdge(reserves), normalEdge(isMade));

        m_out.appendTo(reserves);
        {
            Machine::DeclaredAccessesSuspension writesToNewArray(m_out);
            callVoidOperation(value, RuntimeOperation::ReserveWholeArrayCapacity, { madeWithoutRoom, length ? sub(capacity, integer(length)) : capacity }, { .isOnRarePath = true });
        }
        m_out.jump(isMade);

        m_out.appendTo(isMade);
    }
    join.add(madeWithoutRoom);
    return join.finish();
}

CellPointer ArrayAllocationEmitter::newLiteralAtSite(const ValueContext& value, const ArrayStorageForm& form, std::span<const uint32_t> site)
{
    LValue profile = m_out.valueOf(instanceWord(m_heaps.entire(AbstractHeap::RuntimeState), m_mentions.add(NumberSpace::ArrayProfile, site)));
    return CellPointer(callStub(value, newEmptyArrayAtSite(elementKindOfForm(form)), { profile }).first);
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

void ArrayAllocationEmitter::setLengthOfNewArray(CellPointer array, Integer length)
{
    CodeGenerator::InitializationScope scope(generator());
    m_out.store32(low32(length).value(), m_out.loadPtr(array.value(), m_heaps.JSArray_butterfly), m_heaps.Butterfly_publicLength);
}

CellPointer ArrayAllocationEmitter::allocateArrayOfHoles(const OperationContext& context, Integer length, const ArrayStorageForm& form, LBasicBlock slowPath, ElementsBelowLength elementsBelowLength, LengthIsSetBy lengthIsSetBy)
{
    RELEASE_ASSERT(elementsBelowLength == ElementsBelowLength::Cleared || lengthIsSetBy == LengthIsSetBy::Allocation);
    auto setLength = [&](CellPointer array) {
        if (lengthIsSetBy == LengthIsSetBy::Allocation)
            setLengthOfNewArray(array, length);
        return array;
    };
    if (length.value()->hasInt64() && length.value()->asInt64() >= 1 && length.value()->asInt64() <= 3 && elementKindOfForm(form) != ArrayElementKind::Int32) {
        bool holdsOne = length.value()->asInt64() == 1;
        if (elementKindOfForm(form) == ArrayElementKind::Double)
            return setLength(callNewEmptyArray(context, holdsOne ? EmptyArray::DoubleCapacity1 : EmptyArray::DoubleCapacity3));
        return setLength(callNewEmptyArray(context, holdsOne ? EmptyArray::ContiguousCapacity1 : EmptyArray::ContiguousCapacity3));
    }
    branchUnless(belowOrEqual(length, integer(maxHoleArrayCapacity)), slowPath);
    bool isEmpty = length.value()->hasInt64() && !length.value()->asInt64();
    Integer capacity = isEmpty ? integer(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY | 1) : bitOr(length, integer(1));
    Integer lengthAndCapacity(m_out.shl(capacity.value(), m_out.constInt32(holeArrayCapacityShift)));
    static_assert(maxHoleArrayCapacity < 1 << holeArrayCapacityShift);
    if (elementsBelowLength == ElementsBelowLength::NotCleared)
        lengthAndCapacity = bitOr(lengthAndCapacity, length);
    CellPointer array = callNewArrayWithLength(context, form, lengthAndCapacity);
    branchUnless(Condition(m_out.notNull(array.value())), slowPath);
    return elementsBelowLength == ElementsBelowLength::NotCleared ? array : setLength(array);
}

CellPointer ArrayAllocationEmitter::allocateEmptyArrayWithRoom(const OperationContext& context, Integer room, const ArrayStorageForm& form, LBasicBlock slowPath)
{
    static_assert((BASE_CONTIGUOUS_VECTOR_LEN_EMPTY | 1) <= maxHoleArrayCapacity);
    Integer capacity = bitOr(room, integer(1));
    CellPointer array = callNewArrayWithLength(context, form, Integer(m_out.shl(capacity.value(), m_out.constInt32(holeArrayCapacityShift))));
    branchUnless(Condition(m_out.notNull(array.value())), slowPath);
    return array;
}

void ArrayAllocationEmitter::allocateListLike(const OperationContext& context, CellPointer source, Integer length, std::optional<Integer> roomOfEmptyList, LBasicBlock slowPath, const ScopedLambda<void(CellPointer)>& take)
{
    if (roomOfEmptyList) {
        Integer most = integer(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY);
        roomOfEmptyList = select(below(*roomOfEmptyList, most), *roomOfEmptyList, most);
    }
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    ArrayStorageFormList kinds { &arrayStorageFormOf(ArrayStorage::Contiguous), &arrayStorageFormOf(ArrayStorage::Int32), &arrayStorageFormOf(ArrayStorage::Double) };
    elements.forEachFormByShape(elements.indexingMode(source, IndexingShapeMask), &ElementAccessEmitter::hasShape, kinds, slowPath, scopedLambda<void(const ArrayStorageForm&)>([&](const ArrayStorageForm& form) {
        take(roomOfEmptyList ? allocateEmptyArrayWithRoom(context, *roomOfEmptyList, form, slowPath) : allocateArrayOfHoles(context, length, form, slowPath));
    }));
}

CellPointer ArrayAllocationEmitter::newArrayOfHoles(const OperationContext& context, Integer length, const ArrayStorageForm& form, BuiltinID calledBuiltin)
{
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    join.add(allocateArrayOfHoles(context, length, form, slowPath, ElementsBelowLength::Cleared, LengthIsSetBy::Caller));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::NewArrayWithHoles, { CellPointer(m_out.int64Zero), length, word32(form.indexingType), word32(0) }, { .calledBuiltin = calledBuiltin, .isOnRarePath = true }));
    CellPointer array = join.finish();
    setLengthOfNewArray(array, length);
    return array;
}

CellPointer ArrayAllocationEmitter::callNewEmptyArray(const OperationContext& context, EmptyArray emptyArray)
{
    return CellPointer(callStub(context, newEmptyArray(emptyArray), { }).first);
}

CellPointer ArrayAllocationEmitter::callNewArrayWithLength(const OperationContext& context, const ArrayStorageForm& form, Integer lengthAndCapacity)
{
    return CellPointer(callStub(context, newArrayWithLength(elementKindOfForm(form)), { lengthAndCapacity.value() }).first);
}

Number ArrayAllocationEmitter::numberOrMarker(ValueID element)
{
    Operand operand = get(element);
    switch (operand.representation) {
    case Representation::Double:
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
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CellPointer ArrayAllocationEmitter::newArrayOutOfLine(const OperationContext& context, std::span<const ValueID> elements, const ArrayStorageForm& form)
{
    unsigned length = elements.size();
    Word32 count = word32(length);
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    if (form.element != Representation::Double || !length) {
        Vector<Boxed, 16> boxedElements(length, [&](size_t index) {
            const Type& type = typeOf(elements[index]);
            return conversions.box(context, conversions.inFormOfType(get(elements[index]), type), type);
        });
        Pointer values = argumentBuffer(boxedElements.span());
        return callOperation<CellPointer>(context, RuntimeOperation::NewArrayWithIndexingType, { count, values, word32(form.indexingType) }, { .isOnRarePath = true });
    }

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

void ArrayAllocationEmitter::storeElements(const OperationContext& context, CellPointer newArray, const ArrayStorageForm& form, std::span<const Operand> rawElements, std::span<const uint32_t> indexes)
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
        emitter<BarrierEmitter>().fenceAfterInitialization(context);
}

CellPointer ArrayAllocationEmitter::newArrayOf(const OperationContext& context, std::span<const ValueID> elements, const Type& array)
{
    return newArrayInForm(context, elements, formOfLiteral(context, elements, array));
}

CellPointer ArrayAllocationEmitter::newArrayInForm(const OperationContext& context, std::span<const ValueID> elements, const ArrayStorageForm& form)
{
    unsigned length = elements.size();
    bool exceedsInlineLength = length > maxInlineArrayLiteralLength;
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
        storeElements(context, newArray, form, rawElements.span(), indexesWithoutHoles(length).span());
        cells.finishArrayOfLength(context, newArray, length);
        join.add(newArray);
    } else {
        CellPointer newArray = length
            ? callNewArrayWithLength(context, form, integer(length | arrayLiteralVectorLength(length) << holeArrayCapacityShift))
            : callNewEmptyArray(context, emptyNumberArray(form));
        branchUnless(Condition(m_out.notNull(newArray.value())), slowPath);
        storeElements(context, newArray, form, rawElements.span(), indexesWithoutHoles(length).span());
        join.add(newArray);
    }

    m_out.appendTo(slowPath);
    join.add(newArrayOutOfLine(context, elements, form));
    return join.finish();
}

CellPointer ArrayAllocationEmitter::newLiteralWithHoles(const ValueContext& value)
{
    std::span<const ValueID> elements = value->elementsOperands();
    const ArrayStorageForm& form = formOfLiteral(value, elements, typeOf(value.id()), value->arrayStorage());
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
    storeElements(value, newArray, form, rawElements.span(), indexes.span());
    join.add(newArray);

    m_out.appendTo(slowPath);
    bool mustHaveForm = hasArrayStorageForm(value->arrayStorage());
    uint32_t flags = mustHaveForm ? NewArrayWithHolesFlag::mustHaveIndexingType : 0;
    CellPointer arrayOfHoles = callOperation<CellPointer>(value, RuntimeOperation::NewArrayWithHoles, { CellPointer(m_out.int64Zero), integer(length), word32(form.indexingType), word32(flags) }, { .isOnRarePath = true });
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    RawElements boxedElements(elements.size(), [&](size_t index) -> Operand {
        const Type& type = typeOf(elements[index]);
        return conversions.box(value, conversions.inFormOfType(get(elements[index]), type), type);
    });
    for (size_t i = 0; i < boxedElements.size(); ++i)
        callVoidOperation(value, RuntimeOperation::ArrayPutAtIndex, { arrayOfHoles, boxedElements[i], integer(indexes[i]) }, { .isOnRarePath = true });
    join.add(arrayOfHoles);
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
