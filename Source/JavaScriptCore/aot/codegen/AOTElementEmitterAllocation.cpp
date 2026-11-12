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

static_assert(maxInlineArrayLiteralLength < (1u << holeArrayCapacityShift));

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
    if (literal.literalStorage() != ArrayLiteralStorage::Profiled || !literal.elementsOperands().empty() || !literal.holes().empty())
        return std::nullopt;
    return static_cast<uint32_t>(elementKindOfForm(form));
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
    ArrayStorage declared = declaredByArray ? *declaredByArray : hasArrayStorageForm(named) ? ArrayStorage::Other : formAtAllocation(array).kind;
    return Typed::findFormOfLiteral(types(), m_code, elements, named, declared);
}

bool newArrayFencesBeforeHandingCellOn(const Operation& newArray)
{
    size_t length = newArray.elementsOperands().size();
    return newArray.storage() == AllocationStorage::Heap && newArray.holes().empty() && newArray.arrayStorage() == ArrayStorage::Contiguous
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
    if (std::optional<Integer> room = length && length <= maxInlineArrayLiteralLength ? takeRoomAskedRightBehind(value) : std::nullopt)
        return newLiteralWithRoom(value, elements, form, *room);
    bool hasPropertyRoom = length && length <= maxArrayElementsInRegisters && generator().isLiteralStoredIntoByName(value.block(), value.id());
    if (value->literalStorage() == ArrayLiteralStorage::ByElements && !hasPropertyRoom) {
        if (length <= maxArrayElementsInRegisters && !generator().runsRepeatedly(value.block()))
            return emitter<CellAllocationEmitter>().newArrayOfNumbers(value, boxElements<Boxed>(generator(), value, elements).span());
        return newNumberLiteral(value, elements);
    }
    return newArrayInForm(value, elements, length ? form : formAtSite, hasPropertyRoom);
}

static constexpr OpcodeSet opcodesThatAppendManyToLists { Opcode::AppendElements, Opcode::AppendIterable, Opcode::AppendMembers, Opcode::AppendValuesOfMap };

bool ArrayAllocationEmitter::isLeftToSpreadRightBehind(BlockID block, ValueID literal, const Operation& operation, std::span<const ValueID> behind)
{
    if (!generator().paysForCodeInLine(block) || operation.storage() != AllocationStorage::Heap)
        return false;
    if (!operation.elementsOperands().empty() || !operation.holes().empty())
        return false;
    const Operation& spread = operationAt(behind.front());
    if (spread.opcode != Opcode::AppendElements && (spread.opcode != Opcode::AppendIterable || !spread.readsListsByIndex()))
        return false;
    if (spread.arrayOperand() != literal)
        return false;
    ArrayStorage kind = storageThatLiteralNames(literal, operation);
    if (hasArrayStorageForm(kind) && kind != ArrayStorage::Contiguous)
        return false;
    std::optional<AppendedBehindSpread> appended = findWhatIsAppendedBehindSpread(block, literal, behind.front());
    return appended && appended->room < maxHoleArrayCapacity;
}

auto ArrayAllocationEmitter::findWhatIsAppendedBehindSpread(BlockID block, ValueID literal, ValueID spread) -> std::optional<AppendedBehindSpread>
{
    AppendedBehindSpread appended;
    uint32_t usesSeen = 0;
    for (auto [used, user] : generator().usersOfLiteralInBlock(block, literal)) {
        ++usesSeen;
        if (user == spread)
            continue;
        const Operation& operation = operationAt(user);
        if (operation.opcode != Opcode::AppendElement || operation.arrayOperand() != literal)
            return opcodesThatAppendManyToLists.contains(operation.opcode) ? std::nullopt : std::optional(appended);
        ++appended.room;
        appended.holdsWhatIsNoNumber |= !(typeOf(operation.valueOperand()).primitives & Primitive::Number);
    }
    if (uint32_t usesInLaterBlocks = m_code.useCountOf(literal) - usesSeen; usesInLaterBlocks > 1)
        appended.room += usesInLaterBlocks - 1;
    return appended;
}

ArrayStorage ArrayAllocationEmitter::storageThatLiteralNames(ValueID literal, const Operation& operation)
{
    if (hasArrayStorageForm(operation.arrayStorage()))
        return operation.arrayStorage();
    return arrayStorageDeclaredBy(m_program, m_generator.inputs().types, typeIDOf(literal)).predicted();
}

void ArrayAllocationEmitter::bearLiteralHoldingList(const ValueContext& spread, LValue iterable, StubKey appendsAsWritten)
{
    ValueID literal = std::exchange(generator().m_literalLeftToSpreadRightBehind, ValueID());
    AppendedBehindSpread appended = findWhatIsAppendedBehindSpread(spread.block(), literal, spread.id()).value();
    bool isContiguous = appended.holdsWhatIsNoNumber || hasArrayStorageForm(storageThatLiteralNames(literal, operationAt(literal)));
    build().statistics().count(StatisticID::ArrayLiteralsAllocatedFromSpread);
    LBasicBlock isNotServed = newBlock();
    Join<CellPointer> join(m_out);
    CellPointer born(callStub(spread, newArrayOfList(isContiguous), { iterable, m_out.constInt64(appended.room) }).first);
    branchUnless(Condition(m_out.notNull(born.value())), isNotServed);
    join.add(born);

    m_out.appendTo(isNotServed);
    CellPointer asWritten(generator().bearLiteralAsWritten(spread.block(), literal));
    callStub(spread, appendsAsWritten, { asWritten.value(), iterable });
    join.add(asWritten);
    generator().nameAgain(literal, join.finish().value());
}

std::optional<Integer> ArrayAllocationEmitter::takeRoomAskedRightBehind(const ValueContext& value)
{
    std::span<const ValueID> behind = value.operationsBehind();
    if (behind.size() > 1 && operationAt(behind[0]).opcode == Opcode::IntegerToNumber)
        behind = behind.subspan(1);
    if (behind.empty())
        return std::nullopt;
    const Operation& reservation = operationAt(behind[0]);
    if (reservation.opcode != Opcode::ReserveArrayCapacity || reservation.arrayOperand() != value.id() || !reservation.heldOperands().empty())
        return std::nullopt;
    ValueID room = reservation.capacityOperand();
    if (operationAt(room).opcode == Opcode::IntegerToNumber)
        room = operationAt(room).operand(0);
    Representation kept = representationOf(room);
    if (!typeOf(room).isInteger() || (kept != Representation::Int32 && kept != Representation::Int64 && kept != Representation::Double))
        return std::nullopt;
    generator().m_reservationThatBirthHasServed = behind[0];
    build().statistics().count(StatisticID::ArrayLiteralsAllocatedWithCapacity);
    return kept == Representation::Double ? emitter<ConversionEmitter>().toInteger(in<Number>(room)) : integerOf(room);
}

CellPointer ArrayAllocationEmitter::newLiteralWithRoom(const OperationContext& context, std::span<const ValueID> elements, const ArrayStorageForm& form, Integer room)
{
    unsigned length = elements.size();
    LBasicBlock isBorneByStub = newBlock();
    LBasicBlock isTooLong = newBlock();
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    static_assert(maxInlineArrayLiteralLength < maxHoleArrayCapacity && maxHoleArrayCapacity & 1);
    branch(greaterThan(room, integer(maxHoleArrayCapacity)), { rareEdge(isTooLong), normalEdge(isBorneByStub) });

    m_out.appendTo(isBorneByStub);
    RawElements rawElements = rawElementsOf(elements, form, slowPath);
    if (rawElements.size() != length) {
        fail(context, InternalErrorKind::OpcodeWithoutCode);
        return CellPointer(m_out.int64Zero);
    }
    Integer written = integer(arrayLiteralVectorLength(length));
    Integer capacity = bitOr(select(greaterThan(room, written), room, written), integer(1));
    Integer lengthAndCapacity = bitOr(Integer(m_out.shl(capacity.value(), m_out.constInt32(holeArrayCapacityShift))), integer(length));
    CellPointer newArray = callNewArrayWithLength(context, form, lengthAndCapacity);
    branchUnless(Condition(m_out.notNull(newArray.value())), slowPath);
    storeElements(context, newArray, form, rawElements.span(), indexesWithoutHoles(length).span());
    join.add(newArray);

    m_out.appendTo(isTooLong);
    CellPointer asWritten = newArrayInForm(context, elements, form);
    callStub(context, reserveArrayCapacity(), { asWritten.value(), toNumber(room).value() });
    join.add(asWritten);

    m_out.appendTo(slowPath);
    join.add(newArrayOutOfLine(context, elements, form));
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
    CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap::No);
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

std::optional<Pointer> ArrayAllocationEmitter::profileOfListsOfLengthAtSite(Origin origin, const ArrayStorageForm& seed)
{
    static constexpr uint32_t markOfListsOfLength = 8;
    std::optional<Vector<uint32_t, 12>> site = modulesAndPositionsOf(origin);
    if (!site || seed.holdsCells)
        return std::nullopt;
    site->append(markOfListsOfLength | static_cast<uint32_t>(elementKindOfForm(seed)));
    return Pointer(m_out.valueOf(instanceWord(m_heaps.entire(AbstractHeap::RuntimeState), m_mentions.add(NumberSpace::ArrayProfile, site->span()))));
}

CellPointer ArrayAllocationEmitter::allocateArrayOfHolesAtSite(const ValueContext& value, Integer length, const ArrayStorageForm& seed, Pointer profile, LBasicBlock slowPath)
{
    branchUnless(belowOrEqual(length, integer(maxHoleArrayCapacity)), slowPath);
    bool isEmpty = length.value()->hasInt64() && !length.value()->asInt64();
    Integer capacity = isEmpty ? integer(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY | 1) : bitOr(length, integer(1));
    Integer lengthAndCapacity(m_out.shl(capacity.value(), m_out.constInt32(holeArrayCapacityShift)));
    CellPointer array(callStub(value, newArrayWithLengthAtSite(elementKindOfForm(seed)), { lengthAndCapacity.value(), profile.value() }).first);
    branchUnless(Condition(m_out.notNull(array.value())), slowPath);
    return array;
}

CellPointer ArrayAllocationEmitter::bearArrayOfLength(const OperationContext& context, const ArrayStorageForm& form, Integer length)
{
    if (length.value()->hasInt64() && length.value()->asInt64() >= 1 && length.value()->asInt64() <= 3 && elementKindOfForm(form) != ArrayElementKind::Int32) {
        bool holdsOne = length.value()->asInt64() == 1;
        bool holdsFractions = elementKindOfForm(form) == ArrayElementKind::Double;
        CellPointer array = callNewEmptyArray(context, holdsFractions ? (holdsOne ? EmptyArray::DoubleCapacity1 : EmptyArray::DoubleCapacity3) : (holdsOne ? EmptyArray::ContiguousCapacity1 : EmptyArray::ContiguousCapacity3));
        setLengthOfNewArray(array, length);
        return array;
    }
    return CellPointer(callStub(context, newArrayOfLength(elementKindOfForm(form)), { length.value() }).first);
}

CellPointer ArrayAllocationEmitter::allocateEmptyArrayWithRoom(const OperationContext& context, Integer room, const ArrayStorageForm& form, LBasicBlock slowPath)
{
    static_assert((BASE_CONTIGUOUS_VECTOR_LEN_EMPTY | 1) <= maxHoleArrayCapacity);
    Integer capacity = bitOr(room, integer(1));
    CellPointer array = callNewArrayWithLength(context, form, Integer(m_out.shl(capacity.value(), m_out.constInt32(holeArrayCapacityShift))));
    branchUnless(Condition(m_out.notNull(array.value())), slowPath);
    return array;
}

static constexpr int64_t mostRoomOfEmptyListLikeSource = 9;

void ArrayAllocationEmitter::allocateListLike(const OperationContext& context, CellPointer source, Integer length, std::optional<Integer> roomOfEmptyList, LBasicBlock slowPath, const ScopedLambda<void(CellPointer)>& take, const ArrayStorageForm* inPlaceOfAnyValue)
{
    if (roomOfEmptyList) {
        Integer most = integer(mostRoomOfEmptyListLikeSource);
        roomOfEmptyList = select(below(*roomOfEmptyList, most), *roomOfEmptyList, most);
    }
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    ArrayStorageFormList kinds { &arrayStorageFormOf(ArrayStorage::Contiguous), &arrayStorageFormOf(ArrayStorage::Int32), &arrayStorageFormOf(ArrayStorage::Double) };
    auto takeListOfForm = [&](const ArrayStorageForm& form) {
        take(roomOfEmptyList ? allocateEmptyArrayWithRoom(context, *roomOfEmptyList, form, slowPath) : allocateArrayOfHoles(context, length, form, slowPath));
    };
    LBasicBlock hasNoneOfTheKinds = newBlock();
    Word32 shape = elements.indexingMode(source, IndexingShapeMask);
    elements.forEachFormByShape(shape, &ElementAccessEmitter::hasShape, kinds, hasNoneOfTheKinds, scopedLambda<void(const ArrayStorageForm&)>([&](const ArrayStorageForm& formOfSource) {
        takeListOfForm(formOfSource.holdsCells && inPlaceOfAnyValue ? *inPlaceOfAnyValue : formOfSource);
    }));

    m_out.appendTo(hasNoneOfTheKinds);
    branchUnless(equal(shape, word32(UndecidedShape)), slowPath);
    takeListOfForm(arrayStorageFormOf(ArrayStorage::Int32));
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
        CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap(form.holdsCells));
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

std::optional<Pointer> ArrayAllocationEmitter::numberLiteralProfile(const OperationContext& context)
{
    static constexpr uint32_t numberLiteralMark = 16;
    std::optional<Vector<uint32_t, 12>> site = modulesAndPositionsOf(context);
    if (!site)
        return std::nullopt;
    site->append(numberLiteralMark);
    return Pointer(m_out.valueOf(instanceWord(m_heaps.entire(AbstractHeap::RuntimeState), m_mentions.add(NumberSpace::ArrayProfile, site->span()))));
}

Word32 ArrayAllocationEmitter::shapeAtSite(Pointer profile)
{
    static_assert(ArrayAllocationProfile::shapeShift == 8 && ArrayAllocationProfile::shapeMask == 0xff00);
    return Word32(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTInstance_arrayProfiles, profile.value(), OBJECT_OFFSETOF(ArrayAllocationProfile, word) + 1)));
}

CellPointer ArrayAllocationEmitter::newLiteralOfRawElements(const OperationContext& context, const ArrayStorageForm& form, std::span<const Operand> rawElements)
{
    unsigned length = rawElements.size();
    if (length > maxArrayElementsInRegisters) {
        CellAllocationEmitter& cells = emitter<CellAllocationEmitter>();
        CellPointer newArray = cells.allocateArrayOfLength(context, form.indexingType, length);
        storeElements(context, newArray, form, rawElements, indexesWithoutHoles(length).span());
        cells.finishArrayOfLength(context, newArray, length);
        return newArray;
    }
    if (form.kind == ArrayStorage::Double) {
        Vector<Number, maxArrayElementsInRegisters> numbers(length, [&](size_t index) {
            return rawElements[index].as<Number>();
        });
        return newDoubleLiteral(context, numbers.span(), (1u << length) - 1);
    }
    Vector<LValue, maxArrayElementsInRegisters> operands;
    for (Operand element : rawElements)
        operands.append(zeroExtended(unboxInt32(element.as<Boxed>())).value());
    return CellPointer(callStub(context, arrayLiteralWithInt32(length), operands.span()).first);
}

ArrayAllocationEmitter::RawElements ArrayAllocationEmitter::numbersOf(std::span<const Operand> int32s)
{
    RawElements numbers;
    for (Operand element : int32s)
        numbers.append(Operand(toNumber(unboxInt32(element.as<Boxed>()))));
    return numbers;
}

CellPointer ArrayAllocationEmitter::newProfiledInt32Literal(const OperationContext& context, std::span<const ValueID> elements, std::span<const Operand> int32s, Pointer profile)
{
    unsigned length = int32s.size();
    const ArrayStorageForm& int32Form = arrayStorageFormOf(ArrayStorage::Int32);
    const ArrayStorageForm& doubleForm = arrayStorageFormOf(ArrayStorage::Double);
    LBasicBlock isDueForReview = newBlock();
    LBasicBlock isNotDue = newBlock();
    LBasicBlock isNotServed = newBlock();
    LBasicBlock holdsDoubles = newBlock();
    LBasicBlock holdsValues = newBlock();
    Join<CellPointer> join(m_out);
    auto store = [&](const ArrayStorageForm& form, CellPointer array, std::span<const Operand> rawElements) {
        storeElements(context, array, form, rawElements, indexesWithoutHoles(length).span());
        return array;
    };

    auto wordOfProfile = m_out.address(m_heaps.AOTInstance_arrayProfiles, profile.value(), OBJECT_OFFSETOF(ArrayAllocationProfile, word));
    Word32 word(m_out.load32(wordOfProfile));
    m_out.store32(add(word, word32(-static_cast<int32_t>(ArrayAllocationProfile::oneAllocation))).value(), wordOfProfile);
    m_out.branch(aboveOrEqual(word, word32(ArrayAllocationProfile::oneAllocation)).value(), normalEdge(isNotDue), rareEdge(isDueForReview));

    m_out.appendTo(isNotDue);
    join.add(newLiteralOfRawElements(context, int32Form, int32s));

    m_out.appendTo(isDueForReview);
    uint32_t seedAndLengthAndCapacity = static_cast<uint32_t>(ArrayWithInt32) << seedShiftBesideLengthAndCapacity | arrayLiteralVectorLength(length) << holeArrayCapacityShift | length;
    CellPointer reviewed = callOperation<CellPointer>(context, RuntimeOperation::NewArrayWithLengthAtReviewedSite, { profile, word32(static_cast<int32_t>(seedAndLengthAndCapacity)) }, { .isOnRarePath = true });
    branchUnless(Condition(m_out.notNull(reviewed.value())), isNotServed);
    m_out.branch(emitter<ElementAccessEmitter>().hasArrayStorage(reviewed, ArrayStorage::Double, false).value(), normalEdge(holdsDoubles), normalEdge(holdsValues));

    m_out.appendTo(holdsDoubles);
    join.add(store(doubleForm, reviewed, numbersOf(int32s).span()));

    m_out.appendTo(holdsValues);
    join.add(store(int32Form, reviewed, int32s));

    m_out.appendTo(isNotServed);
    join.add(newArrayOutOfLine(context, elements, int32Form));
    return join.finish();
}

CellPointer ArrayAllocationEmitter::newNumberLiteral(const OperationContext& context, std::span<const ValueID> elements)
{
    unsigned length = elements.size();
    bool areProvenNumbers = std::ranges::all_of(elements, [&](ValueID element) {
        return typeOf(element).isNumber();
    });
    const ArrayStorageForm& int32Form = arrayStorageFormOf(ArrayStorage::Int32);
    const ArrayStorageForm& doubleForm = arrayStorageFormOf(ArrayStorage::Double);
    std::optional<Pointer> profile = numberLiteralProfile(context);
    LBasicBlock holdsDoubles = newBlock();
    LBasicBlock holdsValues = profile || !areProvenNumbers ? newBlock() : nullptr;
    Join<CellPointer> join(m_out);
    if (profile) {
        LBasicBlock mayHoldInt32s = newBlock();
        LBasicBlock holdsConverted = newBlock();
        m_out.branch(belowOrEqual(shapeAtSite(*profile), word32(Int32Shape)).value(), normalEdge(mayHoldInt32s), normalEdge(holdsConverted));
        m_out.appendTo(holdsConverted);
        branchUnless(belowOrEqual(shapeAtSite(*profile), word32(DoubleShape)), holdsValues);
        join.add(newLiteralOfRawElements(context, doubleForm, numbersOf(rawElementsOf(elements, int32Form, holdsDoubles).span()).span()));
        m_out.appendTo(mayHoldInt32s);
    }
    RawElements int32s = rawElementsOf(elements, int32Form, holdsDoubles);
    if (int32s.size() != length) {
        fail(context, InternalErrorKind::OpcodeWithoutCode);
        return CellPointer(m_out.int64Zero);
    }
    join.add(profile ? newProfiledInt32Literal(context, elements, int32s.span(), *profile) : newLiteralOfRawElements(context, int32Form, int32s.span()));

    m_out.appendTo(holdsDoubles);
    if (areProvenNumbers)
        join.add(newArrayInForm(context, elements, doubleForm));
    else {
        RawElements numbers = rawElementsOf(elements, doubleForm, holdsValues);
        if (numbers.size() != length) {
            fail(context, InternalErrorKind::OpcodeWithoutCode);
            return CellPointer(m_out.int64Zero);
        }
        join.add(newLiteralOfRawElements(context, doubleForm, numbers.span()));
    }

    if (holdsValues) {
        m_out.appendTo(holdsValues);
        join.add(newArrayInForm(context, elements, arrayStorageFormOf(ArrayStorage::Contiguous)));
    }
    return join.finish();
}

CellPointer ArrayAllocationEmitter::newArrayInForm(const OperationContext& context, std::span<const ValueID> elements, const ArrayStorageForm& form, bool hasPropertyRoom)
{
    unsigned length = elements.size();
    bool exceedsInlineLength = length > maxInlineArrayLiteralLength;
    if (form.kind == ArrayStorage::Contiguous && !exceedsInlineLength)
        return emitter<CellAllocationEmitter>().newArrayOf(context, boxElements<Boxed>(generator(), context, elements).span(), hasPropertyRoom);
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
        return representationOf(element) == Representation::Int32 || (representationOf(element) == Representation::JSValue && typeOf(element).isInt32());
    });
    if (form.kind == ArrayStorage::Int32 && length && length <= maxArrayElementsInRegisters && areInt32) {
        bool someAreBoxed = std::ranges::any_of(elements, [&](ValueID element) {
            return representationOf(element) == Representation::JSValue;
        });
        LBasicBlock liesInBoxOfDouble = someAreBoxed ? newBlock() : nullptr;
        Vector<LValue, maxArrayElementsInRegisters> operands;
        for (ValueID element : elements) {
            if (representationOf(element) == Representation::Int32) {
                operands.append(zeroExtended(get(element).as<Word32>()).value());
                continue;
            }
            Boxed boxed = get(element).as<Boxed>();
            branchUnless(isInt32(boxed), liesInBoxOfDouble);
            operands.append(zeroExtended(unboxInt32(boxed)).value());
        }
        CellPointer born(callStub(context, hasPropertyRoom ? arrayLiteralWithRoom(length, true) : arrayLiteralWithInt32(length), operands.span()).first);
        if (!someAreBoxed)
            return born;
        Join<CellPointer> join(m_out);
        join.add(born);
        m_out.appendTo(liesInBoxOfDouble);
        join.add(newArrayOutOfLine(context, elements, form));
        return join.finish();
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
