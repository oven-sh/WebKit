/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedObject.h"
#include "ArrayPrototypeInlines.h"
#include "CommonSlowPaths.h"
#include "JSCInlines.h"
#include "JSStringJoiner.h"

namespace JSC { namespace AOT {

// These do what takes no code of the program, on an array as the engine made it. None, or no value: nothing was done, and the engine's function has to be called.

// Where nobody looks at what was removed no array is made of it, and some array is yielded.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySplice, EncodedJSValue, (Instance* instance, JSCell* cell, int32_t start, int32_t deleteCount, uint32_t count, const EncodedJSValue* items, uint32_t isUsed))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    if (!arraySpeciesWatchpointIsValid(vm, array)) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    int64_t length = array->length();
    int64_t from = start < 0 ? std::max<int64_t>(length + start, 0) : std::min<int64_t>(start, length);
    int64_t removed = std::clamp<int64_t>(deleteCount, 0, length - from);
    JSArray* result = isUsed ? JSArray::fastSlice(globalObject, array, from, removed) : array;
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!result)
        OPERATION_RETURN(scope, encodedJSValue());
    bool isDone = array->fastSplice(globalObject, length, from, removed, std::span { items, count });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, isDone ? JSValue::encode(result) : encodedJSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayFromTypedArray, JSCell*, (Instance* instance, JSCell* cell, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* view = uncheckedDowncast<JSArrayBufferView>(cell);
    size_t count = view->length();
    JSArray* array = count <= MAX_STORAGE_VECTOR_LENGTH ? JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), 0, count) : nullptr;
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    for (size_t i = 0; i < count; ++i) {
        array->push(globalObject, view->getIndexQuicklyForTypedArray(i));
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, array);
}

// A vector with room for so many elements, as JSObject::ensureLengthSlow() makes it, for a short array that keeps its elements in place, may write to them, and has no named property
// out of line. The length stays. `expected`: how many elements it is expected to come to, or zero. False: nothing was done.
static bool growVector(VM& vm, JSArray* array, unsigned length, unsigned expected = 0)
{
    static constexpr unsigned maxLength = 1024;
    static_assert(maxLength < MIN_SPARSE_ARRAY_INDEX && maxLength < MAX_STORAGE_VECTOR_LENGTH);
    Butterfly* butterfly = array->butterfly();
    unsigned oldVectorLength = butterfly->vectorLength();
    if (length > maxLength || length <= oldVectorLength || array->structure()->outOfLineCapacity()) [[unlikely]]
        return false;
    unsigned newVectorLength = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), expected >= length && expected <= maxLength ? expected : static_cast<unsigned>(nextLength(length)));
    void* base = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, newVectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!base) [[unlikely]]
        return false;
    // Nobody has it yet.
    memcpy(base, butterfly->base(0, 0), Butterfly::totalSize(0, 0, true, oldVectorLength * sizeof(EncodedJSValue)));
    Butterfly* grown = Butterfly::fromBase(base, 0, 0);
    uint64_t hole = hasDouble(array->indexingType()) ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
    for (unsigned i = oldVectorLength; i < newVectorLength; ++i)
        grown->indexingPayload<uint64_t>()[i] = hole;
    grown->setVectorLength(newVectorLength);
    array->setButterfly(vm, grown);
    return true;
}

// See Lowering::allocateStorage(). Null: there is no memory.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedAllocateStorage, void*, (Instance* instance, uint64_t size))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    return vm.auxiliarySpace().allocate(vm, size, nullptr, AllocationFailureMode::ReturnNull);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayGrow, size_t, (Instance* instance, JSCell* cell, uint32_t length))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    return growVector(vm, uncheckedDowncast<JSArray>(cell), length);
}

// `array.push(value)` where the vector is full, which is what brings most pushes out of compiled code. `expected`: how many more are expected to be pushed, this one among them, or zero.
// The new length, or zero: nothing was done.
unsigned pushToFullVector(VM& vm, JSArray* array, JSValue value, unsigned expected)
{
    IndexingType mode = array->indexingMode();
    bool isTaken = mode == ArrayWithContiguous || (mode == ArrayWithInt32 ? value.isInt32() : mode == ArrayWithDouble && value.isNumber() && value.asNumber() == value.asNumber());
    if (!isTaken)
        return 0;
    unsigned length = array->butterfly()->publicLength();
    if (length != array->butterfly()->vectorLength() || !growVector(vm, array, length + 1, expected ? length + expected : 0))
        return 0;
    Butterfly* butterfly = array->butterfly();
    if (mode == ArrayWithDouble)
        butterfly->contiguousDouble().at(array, length) = value.asNumber();
    else
        butterfly->contiguous().at(array, length).set(vm, array, value);
    butterfly->setPublicLength(length + 1);
    return length + 1;
}

// See Typed::CollectionIteration::isOfEntriesOfArrayByIndex.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewEntriesOfArray, JSCell*, (Instance* instance, JSCell* array))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSCellButterfly* record = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, 2);
    record->setIndex(vm, 0, array);
    record->setIndex(vm, 1, jsNumber(Typed::CollectionIteration::isOfEntriesOfArrayByIndex));
    OPERATION_RETURN(scope, record);
}

// The array has its elements in place. No separator: a comma. Done where every element is a string or an int32, however that is kept.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayJoin, JSCell*, (Instance* instance, JSCell* cell, JSCell* separator))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    auto view = (separator ? uncheckedDowncast<JSString>(separator) : vm.smallStrings.singleCharacterString(','))->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSOnlyStringsAndInt32sJoiner joiner(view);
    Butterfly& butterfly = *array->butterfly();
    unsigned length = butterfly.publicLength();
    switch (array->indexingType()) {
    case ALL_INT32_INDEXING_TYPES:
        OPERATION_RETURN(scope, joiner.tryJoin<Int32Shape>(globalObject, butterfly.contiguous().data(), length));
    case ALL_DOUBLE_INDEXING_TYPES:
        OPERATION_RETURN(scope, joiner.tryJoin(globalObject, butterfly.contiguousDouble().data(), length));
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        OPERATION_RETURN(scope, joiner.tryJoin<ContiguousShape>(globalObject, butterfly.contiguous().data(), length));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static JSValue constantOfArrayLiteral(VM& vm, Typed::State& state, uint64_t encoded)
{
    JSValue value = JSValue::decode(encoded);
    if (!value.isCell())
        return value;
    if (encoded & Typed::Data::encodedStringIsSmall)
        return encoded >> 3 ? vm.smallStrings.singleCharacterString(static_cast<unsigned char>(encoded >> 3)) : vm.smallStrings.emptyString();
    uint32_t string = Typed::Data::stringOfEncodedConstant(encoded);
    return encoded & Typed::Data::encodedStringIsMadeOnce ? state.makeStringConstant(vm, string) : state.string(vm, string);
}

// See Placement::placeArraySites(). The first time, or where the array cannot be allocated in place.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayBufferFromData, JSCell*, (Instance* instance, uint32_t offset, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto& shared = *reinterpret_cast<JSCellButterfly**>(instance->typedStatics + offset);
    IndexingType mode = indexingType | CopyOnWrite;
    if (!shared) {
        auto& data = instance->typed->data;
        auto [valuesOffset, count] = data.at<std::array<uint32_t, 2>>(data.arrayLiteralsOffset)[(offset - data.copyOnWriteButterfliesOffset) / sizeof(JSCell*)];
        const uint64_t* values = data.at<uint64_t>(valuesOffset);
        JSCellButterfly* elements = JSCellButterfly::tryCreate(vm, mode, count);
        if (!elements) {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, nullptr);
        }
        for (uint32_t i = 0; i < count; ++i)
            elements->setIndex(vm, i, constantOfArrayLiteral(vm, *instance->typed, values[i]));
        // The collector may read the statics while this runs.
        vm.mutatorFence();
        shared = elements;
        instance->didStoreRootInStatics(offset);
    }
    OPERATION_RETURN(scope, CommonSlowPaths::allocateNewArrayBuffer(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(mode), shared));
}

// What an empty array literal keeps in the statics: see Placement::placeArraySites(). The arrays made there are born with room for `capacity()` elements, which follows what they come to.
// More: once over half of those born with it have outgrown it. Less: that is tried from time to time, for as many arrays as lie between two reviews, and kept unless more than one in eight
// of them would have had room with what the site had before.
struct ArraySite {
    static constexpr unsigned bornShift = arraySiteBornShift;
    static constexpr uint32_t capacityMask = (1 << bornShift) - 1;
    // So many arrays are made between two reviews.
    static constexpr uint32_t interval = arraySiteInterval;
    // So many reviews pass after less has been tried in vain.
    static constexpr uint8_t patience = 16;
    static_assert(BASE_CONTIGUOUS_VECTOR_LEN_MAX <= capacityMask && interval <= UINT8_MAX);

    unsigned capacity() const { return capacityAndBorn & capacityMask ? capacityAndBorn & capacityMask : BASE_CONTIGUOUS_VECTOR_LEN_EMPTY; }
    unsigned bornSinceReview() const { return capacityAndBorn >> bornShift & (interval - 1); }
    void change(unsigned capacity)
    {
        capacityAndBorn = capacity;
        outgrown = 0;
    }
    void goBack()
    {
        change(capacityBefore);
        capacityBefore = 0;
        reviewsToWait = patience;
    }

    // In the low byte the capacity, zero for that of any empty literal. Above it how many arrays have been made, which compiled code counts.
    uint32_t capacityAndBorn;
    // Since the last review.
    uint8_t outgrown;
    // While less is being tried: what the site had before, and how many had outgrown that between two reviews. Else zero.
    uint8_t capacityBefore;
    uint8_t outgrownBefore;
    uint8_t reviewsToWait;
};
static_assert(sizeof(ArraySite) == 2 * sizeof(uint32_t) && !OBJECT_OFFSETOF(ArraySite, capacityAndBorn));

// Before a push that may have found no room.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedUpdateVectorLengthHint, void, (Instance* instance, JSCell* cell, uint32_t offset))
{
    IndexingType type = cell->indexingType();
    if (!hasInt32(type) && !hasDouble(type) && !hasContiguous(type))
        return;
    auto& site = *reinterpret_cast<ArraySite*>(instance->typedStatics + offset);
    Butterfly* butterfly = uncheckedDowncast<JSArray>(cell)->butterfly();
    unsigned capacity = butterfly->vectorLength();
    // Full for the first time.
    if (butterfly->publicLength() != capacity || capacity != site.capacity() || capacity >= BASE_CONTIGUOUS_VECTOR_LEN_MAX || site.outgrown == UINT8_MAX)
        return;
    if (++site.outgrown < 2 || site.outgrown * 2u <= site.bornSinceReview())
        return;
    if (site.capacityBefore)
        site.goBack();
    else
        site.change(capacity <= 9 ? capacity + 2 : std::min<unsigned>((capacity * 5 / 4) | 1, BASE_CONTIGUOUS_VECTOR_LEN_MAX));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedReviewVectorLengthHint, void, (Instance* instance, uint32_t offset))
{
    auto& site = *reinterpret_cast<ArraySite*>(instance->typedStatics + offset);
    unsigned capacity = site.capacity();
    unsigned outgrown = std::exchange(site.outgrown, 0);
    if (site.capacityBefore) {
        if (outgrown > site.outgrownBefore + ArraySite::interval / 8) {
            site.goBack();
            return;
        }
        site.capacityBefore = 0;
    }
    if (site.reviewsToWait) {
        --site.reviewsToWait;
        return;
    }
    if (capacity == 1)
        return;
    site.capacityBefore = capacity;
    site.outgrownBefore = outgrown;
    site.change(capacity <= 9 ? capacity - 2 : (capacity * 3 / 4) | 1);
}

// Of a long array, several elements at a time. -1: there is none.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayIndexOfDouble, size_t, (Instance*, JSCell* array, double searched))
{
    Butterfly& butterfly = *uncheckedDowncast<JSArray>(array)->butterfly();
    const double* data = butterfly.contiguousDouble().data();
    const double* found = WTF::findDouble(data, searched, butterfly.publicLength());
    return found ? found - data : static_cast<size_t>(-1);
}

// Among int32s, where only the same bits are the same number.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayIndexOfInt32, size_t, (Instance*, JSCell* array, EncodedJSValue searched))
{
    Butterfly& butterfly = *uncheckedDowncast<JSArray>(array)->butterfly();
    auto* data = std::bit_cast<const uint64_t*>(butterfly.contiguous().data());
    const uint64_t* found = WTF::find64(data, searched, butterfly.publicLength());
    return found ? found - data : static_cast<size_t>(-1);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayUnshift, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue item))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    // Neither the array nor what it inherits from answers an index with code of the program.
    if (shouldUseSlowPut(array->indexingType()) || array->structure()->holesMustForwardToPrototype(array)) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    uint64_t length = array->length();
    unshift(globalObject, array, 0, 0, 1, length);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    array->putByIndexInline(globalObject, static_cast<unsigned>(0), JSValue::decode(item), true);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    setLength(globalObject, vm, array, length + 1);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(length + 1)));
}

// The values are boxed. One that the indexing type has no room for makes the array one that keeps values.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayWithIndexingType, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* values, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    for (uint32_t i = 0; i < count; ++i) {
        JSValue value = JSValue::decode(values[i]);
        bool fits = indexingType == ArrayWithContiguous || (indexingType == ArrayWithInt32 ? value.isInt32() : value.isNumber() && value.asNumber() == value.asNumber());
        if (!fits)
            indexingType = ArrayWithContiguous;
    }
    ObjectInitializationScope initializationScope(vm);
    JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, nullptr, globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    IndexingType made = array->indexingType();
    for (uint32_t i = 0; i < count; ++i)
        array->initializeIndex(initializationScope, i, JSValue::decode(values[i]), made);
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayOfConstants, JSCell*, (Instance* instance, uint32_t literal, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto& data = instance->typed->data;
    auto [valuesOffset, count] = data.at<std::array<uint32_t, 2>>(data.arrayLiteralsOffset)[literal];
    const uint64_t* values = data.at<uint64_t>(valuesOffset);
    auto valueAt = [&](uint32_t i) {
        return constantOfArrayLiteral(vm, *instance->typed, values[i]);
    };
    for (uint32_t i = 0; i < count; ++i) {
        JSValue value = valueAt(i);
        bool fits = indexingType == ArrayWithContiguous || (indexingType == ArrayWithInt32 ? value.isInt32() : value.isNumber() && value.asNumber() == value.asNumber());
        if (!fits)
            indexingType = ArrayWithContiguous;
    }
    ObjectInitializationScope initializationScope(vm);
    JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, nullptr, globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    IndexingType made = array->indexingType();
    for (uint32_t i = 0; i < count; ++i)
        array->initializeIndex(initializationScope, i, valueAt(i), made);
    OPERATION_RETURN(scope, array);
}

namespace Typed {

using namespace B3;

// The family aot/builtins/array.def.

static constexpr unsigned minLengthToSearchOutOfLine = 32;
// The engine copies many at a time, which pays for the call from here on.
static constexpr unsigned maxLengthToCopyOneByOne = 64;

void sinkEmptyArrayLiterals(Function& function)
{
    for (auto& block : function.blocks) {
        size_t end = block.values.size() - (block.terminator.kind == TerminatorKind::Invoke && !block.values.isEmpty());
        for (size_t i = end; i--;) {
            ValueID id = block.values[i];
            auto& value = function.at(id);
            if (value.opcode != Opcode::NewArray || !value.arguments.isEmpty())
                continue;
            size_t firstUse = i + 1;
            bool isUsed = false;
            for (; firstUse < end; ++firstUse) {
                auto& user = function.at(block.values[firstUse]);
                if ((user.opcode == Opcode::PutById && user.isInitialStore()) || user.opcode == Opcode::IteratorOpen || user.opcode == Opcode::NewObjectWithShape)
                    break;
                user.forEachChild([&](ValueID child) {
                    isUsed |= child == id;
                });
                if (isUsed)
                    break;
            }
            if (!isUsed || firstUse == i + 1)
                continue;
            block.values.removeAt(i);
            block.values.insert(firstUse - 1, id);
        }
    }
}

static bool isInt32Type(const Type& type)
{
    return type.isInteger() && type.integers.isWithin(Range::int32());
}

// How an array that is made as the type keeps its elements. That says what is expected of its future: whatever is stored later that does not fit converts it.
// What the array that is being made is expected to hold. Nothing vouches for the elements of an array, so every array type is within every other, whatever their elements, and
// predictionOf() has nothing to say of one. It decides how the array keeps its elements from its birth, and nothing else.
Type Lowering::arrayTypeAtBirth(ValueID id) const
{
    if (TypeID prediction = m_function.at(id).prediction; prediction && m_program.at(prediction).elements)
        return m_program.at(prediction).withoutPrimitives(Primitive::UndefinedOrNull);
    return typeOf(id);
}

IndexingType Lowering::indexingTypeAtBirth(const Type& array) const
{
    if (!array.elements)
        return ArrayWithContiguous;
    auto& elements = m_program.at(array.elements);
    if (isInt32Type(elements))
        return ArrayWithInt32;
    return elements.isNumber() ? ArrayWithDouble : ArrayWithContiguous;
}

int64_t Lowering::holeIn(IndexingType indexingType)
{
    return indexingType == ArrayWithDouble ? std::bit_cast<int64_t>(PNaN) : JSValue::encode(JSValue());
}

const AbstractHeap& Lowering::elementsOf(IndexingType indexingType)
{
    return indexingType == ArrayWithDouble ? m_heaps.indexedDoubleProperties.atAnyIndex() : m_heaps.indexedContiguousProperties.atAnyIndex();
}

// So many bytes for the elements of an array, with nothing in them. Where the free list of the size has run out the run time finds the next one, which may collect, and all goes on here:
// whoever has read a pointer out of an object before keeps the object alive. To the block for a size that is not allocated in place, or where there is no memory.
LValue Lowering::allocateStorage(LValue size, LBasicBlock slowPath)
{
    LBasicBlock hasRunOut = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    // A length of -1, which stands for one that is none, comes to no bytes at all.
    orElse(m_out.notZero64(size), slowPath);
    LValue storage = allocateHeapCell(allocatorForSize(fixedPointer(Instance::offsetOfAuxiliarySpace()), size, slowPath), hasRunOut);
    applyEffects(storage->as<PatchpointValue>(), { Writes::RuntimeState, false });
    ValueFromBlock inPlace = m_out.anchor(storage);
    m_out.jump(continuation);
    m_out.appendTo(hasRunOut);
    Pointer found = AOT_TYPED_OPERATION(AllocateStorage, Integer(size));
    orElse(m_out.notNull(found.value()), slowPath);
    ValueFromBlock outOfLine = m_out.anchor(found.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(pointerType(), inPlace, outOfLine);
}

// Nothing but holes, up to the capacity. To the block for a length that is none, or too large to be allocated in place.
CellPointer Lowering::allocateArrayWithHoles(Integer length, IndexingType indexingType, LBasicBlock slowPath, bool isInTheMaking)
{
    LBasicBlock fill = m_out.newBlock();
    LBasicBlock filled = m_out.newBlock();
    // Zero once arrays are no longer allocated with their original structures.
    LValue header = changing64(isInTheMaking ? Instance::offsetOfArrayInTheMakingHeaders() + Instance::indexOfArrayInTheMaking(indexingType) * sizeof(uint64_t) : Instance::offsetOfNewArrayHeader(indexingType));
    orElse(m_out.notZero64(header), slowPath);
    // With the header the butterfly is a multiple of 16 bytes.
    LValue vectorLength = m_out.bitOr(length.value(), m_out.constInt64(1));
    LValue size = m_out.add(m_out.shl(vectorLength, m_out.constInt32(3)), m_out.constIntPtr(sizeof(IndexingHeader)));
    LValue storage = allocateStorage(size, slowPath);
    LValue end = m_out.add(storage, size);
    LValue hole = m_out.constInt64(holeIn(indexingType));
    ValueFromBlock start = m_out.anchor(storage);
    m_out.jump(fill);

    m_out.appendTo(fill);
    LValue pair = m_out.phi(pointerType(), start);
    m_out.store64(hole, TypedPointer(elementsOf(indexingType), pair));
    m_out.store64(hole, TypedPointer(elementsOf(indexingType), m_out.add(pair, m_out.intPtrEight)));
    LValue next = m_out.add(pair, m_out.constIntPtr(2 * sizeof(EncodedJSValue)));
    m_out.addIncomingToPhi(pair, m_out.anchor(next));
    m_out.branch(m_out.below(next, end), unsure(fill), unsure(filled));

    m_out.appendTo(filled);
    LValue butterfly = m_out.add(storage, m_out.constIntPtr(sizeof(IndexingHeader)));
    m_out.store32(m_out.castToInt32(length.value()), butterfly, m_heaps.Butterfly_publicLength);
    m_out.store32(m_out.castToInt32(vectorLength), butterfly, m_heaps.Butterfly_vectorLength);
    LValue array = allocateHeapCell(fixedPointer(Instance::offsetOfArrayAllocator()), sizeof(JSArray), slowPath);
    applyEffects(array->as<PatchpointValue>(), { Writes::RuntimeState, false });
    storeHeader(array, header);
    m_out.storePtr(butterfly, array, m_heaps.JSObject_butterfly);
    fenceAfterInitialization();
    return CellPointer(array);
}

IndexingType Lowering::indexingTypeOfLiteral(const Arguments& elements, const Type& type) const
{
    IndexingType indexingType = indexingTypeAtBirth(type);
    for (ValueID element : elements) {
        if (indexingType == ArrayWithInt32 ? !isInt32Type(typeOf(element)) : indexingType == ArrayWithDouble && !typeOf(element).isNumber())
            return ArrayWithContiguous;
    }
    return indexingType;
}

// See Stub::ArrayOfDoubles. Nothing: the empty literal.
LValue Lowering::allocateArrayOfNumbers(IndexingType indexingType, LValue lengthAndCapacity, LBasicBlock slowPath)
{
    static_assert(static_cast<unsigned>(Stub::ArrayOfDoubles) + 1 == static_cast<unsigned>(Stub::LeafArrayOfDoubles) && static_cast<unsigned>(Stub::ArrayOfInt32s) + 1 == static_cast<unsigned>(Stub::LeafArrayOfInt32s));
    static_assert(static_cast<unsigned>(Stub::EmptyArrayOfDoubles) + 1 == static_cast<unsigned>(Stub::LeafEmptyArrayOfDoubles) && static_cast<unsigned>(Stub::EmptyArrayOfInt32s) + 1 == static_cast<unsigned>(Stub::LeafEmptyArrayOfInt32s));
    RELEASE_ASSERT(indexingType == ArrayWithDouble || indexingType == ArrayWithInt32);
    bool areDoubles = indexingType == ArrayWithDouble;
    OperationEffects effects = effectsOfOperation<operationAOTTypedNewArrayOfNumbers>();
    LValue array = lengthAndCapacity
        ? callAllocationStub(areDoubles ? Stub::ArrayOfDoubles : Stub::ArrayOfInt32s, { { lengthAndCapacity, stubTemporaryGPRs[0] } }, effects, true)
        : callAllocationStub(areDoubles ? Stub::EmptyArrayOfDoubles : Stub::EmptyArrayOfInt32s, { }, effects, true);
    orElse(m_out.notZero64(array), slowPath);
    return array;
}

// An array literal, made as the type says.
CellPointer Lowering::newArrayOfType(const Arguments& elements, const Type& type)
{
    constexpr unsigned maxInlineLength = Program::maxLengthOfArrayLiteralMadeInline;
    unsigned length = elements.size();
    IndexingType indexingType = indexingTypeOfLiteral(elements, type);
    if (indexingType == ArrayWithContiguous && length <= maxInlineLength)
        return newArray(elements);
    auto outOfLine = [&] {
        auto [count, list] = argumentBuffer(elements);
        return AOT_TYPED_OPERATION(NewArrayWithIndexingType, count, list, word32(indexingType));
    };
    if (length > maxInlineLength)
        return outOfLine();

    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<LValue, 16> stored;
    for (ValueID element : elements) {
        if (indexingType == ArrayWithInt32) {
            stored.append(boxInt32(low32(in<Integer>(element))).value());
            continue;
        }
        Number number = in<Number>(element);
        // Among doubles it would read as a hole.
        if (!typeOf(element).isInteger())
            orElse(equal(number, number), slowPath);
        stored.append(number.value());
    }
    LValue array = allocateArrayOfNumbers(indexingType, length ? m_out.constInt64(length | vectorLengthOfArrayLiteral(length) << capacityShiftInArrayOfNumbers) : nullptr, slowPath);
    if (length) {
        LValue butterfly = m_out.loadPtr(array, m_heaps.JSObject_butterfly);
        for (unsigned i = 0; i < length; ++i) {
            if (indexingType == ArrayWithDouble)
                m_out.storeDouble(stored[i], butterfly, m_heaps.indexedDoubleProperties[i]);
            else
                m_out.store64(stored[i], butterfly, m_heaps.indexedContiguousProperties[i]);
        }
    }
    ValueFromBlock fast = m_out.anchor(array);
    m_out.jump(continuation);
    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(outOfLine().value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

bool Lowering::isAvailableAtCurrentValue(ValueID id) const
{
    BlockID block = m_definingBlock[id.index()];
    if (!block)
        return false;
    if (block != m_currentBlockID)
        return dominates(block, m_currentBlockID);
    if (m_currentBlock->parameters.contains(id))
        return true;
    for (size_t i = 0; i < m_currentIndexInBlock; ++i) {
        if (m_currentBlock->values[i] == id)
            return true;
    }
    return false;
}

std::optional<Lowering::CountedLoop> Lowering::countedLoopContaining(BlockID block) const
{
    unsigned index = m_innermostLoop[block.index()];
    if (index == noLoop)
        return std::nullopt;
    auto& loop = m_loops[index];
    auto& header = m_function.at(loop.header);
    if (header.terminator.kind != TerminatorKind::Branch || m_definingBlock[header.terminator.value.index()] != loop.header)
        return std::nullopt;
    auto& test = m_function.at(header.terminator.value);
    if (!loop.blocks.get(header.terminator.targets[0].block.index()) || loop.blocks.get(header.terminator.targets[1].block.index()))
        return std::nullopt;
    CountedLoop result;
    result.index = index;
    if (test.opcode == Opcode::Less) {
        result.counter = test.child(0);
        result.bound = test.child(1);
        if (!typeOf(result.bound).isInteger())
            return std::nullopt;
    } else if (test.opcode == Opcode::IteratorHasNext && keyedAccessOf(m_program, m_function, test) == KeyedAccess::Array) {
        result.counter = test.child(1);
        result.iteratedArray = m_function.at(test.child(0)).child(0);
    } else
        return std::nullopt;
    result.position = header.parameters.find(result.counter);
    if (result.position == notFound || !typeOf(result.counter).isInteger())
        return std::nullopt;
    for (size_t blockIndex : loop.blocks) {
        auto& terminator = m_function.blocks[blockIndex].terminator;
        if (terminator.kind == TerminatorKind::Return)
            return std::nullopt;
        for (auto& target : successorsOf(terminator)) {
            if (terminator.kind == TerminatorKind::Invoke && &target == &terminator.targets.last())
                continue;
            if (target.block != loop.header) {
                if (!loop.blocks.get(target.block.index()) && BlockID(blockIndex) != loop.header)
                    return std::nullopt;
                continue;
            }
            auto& next = m_function.at(originOf(target.arguments[result.position]));
            ValueID stepped = next.opcode == Opcode::Add ? originOf(next.child(0)) : ValueID();
            if (stepped && m_function.at(stepped).opcode == Opcode::ToNumeric)
                stepped = originOf(m_function.at(stepped).child(0));
            if (stepped != result.counter || m_function.at(next.child(1)).opcode != Opcode::Integer || m_function.at(next.child(1)).integer() != 1)
                return std::nullopt;
            if (!dominates(block, BlockID(blockIndex)))
                return std::nullopt;
        }
    }
    return result;
}

Integer Lowering::arrayLengthOrBail(ValueID array, LBasicBlock otherwise)
{
    Operand operand = get(array);
    if (operand.rep == Rep::Cell && !typeOf(array).mayBeUndefinedOrNull())
        return arrayLength(operand.as<CellPointer>());
    Boxed boxed = convertTo<Boxed>(operand, typeOf(array));
    orElse(isCell(boxed, typeOf(array)), otherwise);
    return arrayLength(asCell(boxed));
}

std::optional<Lowering::InitialCapacity> Lowering::inferInitialCapacity(ValueID literal)
{
    auto isAvailableArray = [&](ValueID id) {
        auto* engineClass = typeOf(id).onlyEngineClass();
        return engineClass && engineClass->type == ArrayType && isAvailableAtCurrentValue(id) && get(id).value;
    };
    auto isAppendSpread = [&](const Value& value) {
        auto isOperation = [&](const Value& call) {
            if (call.opcode != Opcode::CallBuiltin)
                return false;
            auto& implementation = m_builtins.at(call.builtin()).implementation;
            return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::AppendSpread;
        };
        if (isOperation(value))
            return true;
        if (value.opcode != Opcode::CallDirect || value.arguments.size() != 2)
            return false;
        auto& body = m_program.at(value.function());
        return body.blocks.size() == 1 && body.blocks[0].parameters.size() == 2 && body.blocks[0].values.size() == 1 && isOperation(body.at(body.blocks[0].values[0]));
    };
    InitialCapacity room;
    ValueID array = literal;
    for (size_t i = m_currentIndexInBlock + 1; i < m_currentBlock->values.size(); ++i) {
        ValueID id = m_currentBlock->values[i];
        auto& value = m_function.at(id);
        if (!isAppendSpread(value) || originOf(value.arguments[0]) != array)
            continue;
        if (!isAvailableArray(value.arguments[1]))
            return std::nullopt;
        room.arrays.append(value.arguments[1]);
        room.constant += numberOfElementsPushedTo(id);
        array = id;
    }
    if (array != literal)
        return room;

    Vector<BlockID, 4> pushBlocks;
    unsigned pushedCount = 0;
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        for (ValueID id : m_function.blocks[b].values) {
            if (!isInlineBuiltin(id, InlineBuiltin::ArrayPush) || originOf(m_function.at(id).child(0)) != literal)
                continue;
            pushBlocks.append(BlockID(b));
            pushedCount += m_function.at(id).arguments.size();
        }
    }
    if (pushBlocks.isEmpty())
        return std::nullopt;
    bool isPushedInLoop = pushBlocks.containsIf([&](BlockID block) {
        return m_innermostLoop[block.index()] != m_innermostLoop[m_currentBlockID.index()];
    });
    if (!isPushedInLoop) {
        bool isUnconditional = !pushBlocks.containsIf([&](BlockID block) {
            return block != m_currentBlockID;
        });
        if (isUnconditional ? pushedCount > maxCapacityOfArrayOfNumbers : pushedCount > 1)
            return std::nullopt;
        room.constant = pushedCount;
        return room;
    }
    if (pushBlocks.size() != 1 || pushedCount != 1)
        return std::nullopt;
    auto counted = countedLoopContaining(pushBlocks[0]);
    if (!counted)
        return std::nullopt;
    auto& loop = m_loops[counted->index];
    if (loop.blocks.get(m_currentBlockID.index()) || !dominates(m_currentBlockID, loop.header))
        return std::nullopt;
    for (auto& other : m_loops) {
        if (&other != &loop && other.blocks.get(loop.header.index()) && !other.blocks.get(m_currentBlockID.index()))
            return std::nullopt;
    }
    std::optional<int64_t> start;
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        if (loop.blocks.get(b))
            continue;
        for (auto& target : successorsOf(m_function.blocks[b].terminator)) {
            if (target.block != loop.header)
                continue;
            ValueID first = m_program.constantOf(m_function, target.arguments[counted->position]);
            if (!first || m_function.at(first).opcode != Opcode::Integer || (start && *start != m_function.at(first).integer()))
                return std::nullopt;
            start = m_function.at(first).integer();
        }
    }
    if (!start || *start < 0)
        return std::nullopt;
    room.constant = -*start;
    ValueID iteratedArray = counted->iteratedArray;
    if (!iteratedArray) {
        ValueID bound = originOf(counted->bound);
        if (isInlineBuiltin(bound, InlineBuiltin::ArrayLength))
            iteratedArray = m_function.at(bound).child(0);
        else if (isAvailableAtCurrentValue(bound) && get(bound).value && get(bound).rep == Rep::Int64) {
            room.count = bound;
            return room;
        } else
            return std::nullopt;
    }
    if (!isAvailableArray(iteratedArray))
        return std::nullopt;
    room.arrays.append(iteratedArray);
    return room;
}

// A NewArray. See Placement::placeArraySites().
CellPointer Lowering::newArrayLiteral(ValueID id)
{
    std::optional<InitialCapacity> inferred;
    if (m_function.at(id).arguments.isEmpty() && m_function.at(id).copyOnWriteButterflyOffset() == Format::none)
        inferred = inferInitialCapacity(id);
    if (!inferred)
        return newArrayLiteralWithAllocationSite(id);
    auto isAppendSpread = [&](const Value& value) {
        if (value.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = m_builtins.at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::AppendSpread;
    };
    unsigned numberOfSpreads = 0;
    for (auto& value : m_function.values) {
        if (!isAppendSpread(value))
            continue;
        ValueID target = originOf(value.arguments[0]);
        while (isAppendSpread(m_function.at(target)))
            target = originOf(m_function.at(target).arguments[0]);
        numberOfSpreads += target == id;
    }
    if (numberOfSpreads == inferred->arrays.size())
        m_literalsWithRoomForAllSpreads.set(id.index());
    static_assert(static_cast<unsigned>(Stub::EmptyArrayWithRoom) + 1 == static_cast<unsigned>(Stub::LeafEmptyArrayWithRoom));
    static_assert(static_cast<unsigned>(Stub::EmptyArrayWithRoomFor1) + 1 == static_cast<unsigned>(Stub::LeafEmptyArrayWithRoomFor1));
    static_assert(static_cast<unsigned>(Stub::EmptyArrayWithRoomFor3) + 1 == static_cast<unsigned>(Stub::LeafEmptyArrayWithRoomFor3));
    IndexingType indexingType = indexingTypeAtBirth(arrayTypeAtBirth(id));
    unsigned constantCapacity = !inferred->count && inferred->arrays.isEmpty() ? inferred->constant | 1 : 0;
    if (constantCapacity == vectorLengthOfArrayLiteral(0))
        return newArrayOfType(m_function.at(id).arguments, arrayTypeAtBirth(id));
    if (indexingType == ArrayWithContiguous && (constantCapacity == 1 || constantCapacity == 3))
        return CellPointer(callAllocationStub(constantCapacity == 1 ? Stub::EmptyArrayWithRoomFor1 : Stub::EmptyArrayWithRoomFor3, { }, effectsOfOperation<operationAOTTypedNewArray>(), true));
    LBasicBlock fallback = newUnlikelyBlock();
    LBasicBlock continuation = m_out.newBlock();
    if (indexingType == ArrayWithDouble && (constantCapacity == 1 || constantCapacity == 3)) {
        LValue made = callAllocationStub(constantCapacity == 1 ? Stub::EmptyArrayOfDoublesWithRoomFor1 : Stub::EmptyArrayOfDoublesWithRoomFor3, { }, effectsOfOperation<operationAOTTypedNewArrayOfNumbers>(), true);
        orElse(m_out.notZero64(made), fallback);
        ValueFromBlock sized = m_out.anchor(made);
        m_out.jump(continuation);
        m_out.appendTo(fallback);
        ValueFromBlock usual = m_out.anchor(newArrayOfType(m_function.at(id).arguments, arrayTypeAtBirth(id)).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return CellPointer(m_out.phi(Int64, sized, usual));
    }
    unsigned numberOfSpreadsAtBirth = 0;
    if ((indexingType == ArrayWithContiguous || indexingType == ArrayWithDouble) && !inferred->count) {
        ValueID target = id;
        for (size_t next = m_currentIndexInBlock + 1; next < m_currentBlock->values.size() && numberOfSpreadsAtBirth < inferred->arrays.size(); ++next) {
            auto& spread = m_function.at(m_currentBlock->values[next]);
            if (!isAppendSpread(spread) || originOf(spread.arguments[0]) != target || spread.arguments[1] != inferred->arrays[numberOfSpreadsAtBirth])
                break;
            target = m_currentBlock->values[next];
            ++numberOfSpreadsAtBirth;
            if (indexingType == ArrayWithDouble)
                break;
        }
    }
    if (numberOfSpreadsAtBirth) {
        static_assert(static_cast<unsigned>(Stub::ArrayOfValues) + 1 == static_cast<unsigned>(Stub::LeafArrayOfValues));
        Vector<Integer, 4> counts;
        Integer length = integer(0);
        Integer room = integer(inferred->constant);
        for (size_t i = 0; i < inferred->arrays.size(); ++i) {
            Integer count = arrayLengthOrBail(inferred->arrays[i], fallback);
            room = add(room, count);
            if (i >= numberOfSpreadsAtBirth)
                continue;
            Operand operand = get(inferred->arrays[i]);
            Word32 shape = indexingModeOf(operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>()), IndexingShapeMask);
            if (indexingType == ArrayWithDouble)
                orElse(equal(shape, word32(DoubleShape)), fallback);
            else
                orElse(logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))), fallback);
            orElse(belowOrEqual(count, integer(maxLengthToCopyOneByOne)), fallback);
            counts.append(count);
            length = i ? add(length, count) : count;
        }
        orElse(belowOrEqual(room, integer(maxCapacityOfArrayOfNumbers)), fallback);
        LValue lengthAndCapacity = m_out.bitOr(m_out.shl(m_out.bitOr(room.value(), m_out.constInt64(1)), m_out.constInt32(capacityShiftInArrayOfNumbers)), length.value());
        LValue made;
        if (indexingType == ArrayWithDouble)
            made = allocateArrayOfNumbers(indexingType, lengthAndCapacity, fallback);
        else {
            LBasicBlock isShort = m_out.newBlock();
            LBasicBlock isLong = m_out.newBlock();
            LBasicBlock isMade = m_out.newBlock();
            m_out.branch(belowOrEqual(room, integer(vectorLengthOfArrayLiteral(0))).value(), unsure(isShort), unsure(isLong));
            m_out.appendTo(isShort);
            LValue madeShort = callAllocationStub(Stub::EmptyArrayWithRoom, { { room.value(), stubTemporaryGPRs[0] } }, effectsOfOperation<operationAOTTypedNewArray>(), true);
            m_out.store32(m_out.castToInt32(length.value()), butterflyOf(CellPointer(madeShort)).value(), m_heaps.Butterfly_publicLength);
            ValueFromBlock shortOne = m_out.anchor(madeShort);
            m_out.jump(isMade);
            m_out.appendTo(isLong);
            LValue madeLong = callAllocationStub(Stub::ArrayOfValues, { { lengthAndCapacity, stubTemporaryGPRs[0] } }, effectsOfOperation<operationAOTTypedNewArrayOfNumbers>(), true);
            orElse(m_out.notZero64(madeLong), fallback);
            ValueFromBlock longOne = m_out.anchor(madeLong);
            m_out.jump(isMade);
            m_out.appendTo(isMade);
            made = m_out.phi(Int64, shortOne, longOne);
        }
        m_isFillingArrayAtBirth = true;
        auto& elements = indexingType == ArrayWithDouble ? m_heaps.indexedDoubleProperties : m_heaps.indexedContiguousProperties;
        LValue to = butterflyOf(CellPointer(made)).value();
        for (unsigned i = 0; i < numberOfSpreadsAtBirth; ++i) {
            Operand operand = get(inferred->arrays[i]);
            CellPointer source = operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>());
            Pointer from = butterflyOf(source);
            LBasicBlock copyLoop = m_out.newBlock();
            LBasicBlock copy = m_out.newBlock();
            LBasicBlock copied = m_out.newBlock();
            ValueFromBlock firstCopied = m_out.anchor(m_out.intPtrZero);
            m_out.jump(copyLoop);
            m_out.appendTo(copyLoop);
            LValue index = m_out.phi(pointerType(), firstCopied);
            m_out.branch(m_out.below(index, counts[i].value()), unsure(copy), unsure(copied));
            m_out.appendTo(copy);
            LValue element = m_out.load64(m_out.baseIndex(elements, from.value(), index));
            if (indexingType == ArrayWithDouble)
                orElse(Word32(m_out.notEqual(element, m_out.constInt64(holeIn(ArrayWithDouble)))), fallback);
            else
                element = m_out.select(m_out.isZero64(element), undefined().value(), element);
            m_out.store64(element, m_out.baseIndex(elements, to, index));
            m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.intPtrOne)));
            m_out.jump(copyLoop);
            m_out.appendTo(copied);
            ensureStillAliveHere(source);
            if (i + 1 < numberOfSpreadsAtBirth)
                to = m_out.add(to, m_out.shl(counts[i].value(), m_out.constInt32(3)));
        }
        if (indexingType != ArrayWithDouble)
            fenceAfterInitialization();
        m_isFillingArrayAtBirth = false;
        ValueFromBlock sized = m_out.anchor(made);
        ValueFromBlock isSized = m_out.anchor(m_out.int32One);
        m_out.jump(continuation);
        m_out.appendTo(fallback);
        ValueFromBlock usual = m_out.anchor(newArrayOfType(m_function.at(id).arguments, arrayTypeAtBirth(id)).value());
        ValueFromBlock isUsual = m_out.anchor(m_out.int32Zero);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        LValue isBornForThem = m_out.phi(Int32, isSized, isUsual);
        for (unsigned i = 0; i < numberOfSpreadsAtBirth; ++i)
            m_spreadsCarriedOutAtBirth.add(m_currentBlock->values[m_currentIndexInBlock + 1 + i].index(), isBornForThem);
        return CellPointer(m_out.phi(Int64, sized, usual));
    }
    Integer room = inferred->count ? add(in<Integer>(inferred->count), integer(inferred->constant)) : integer(inferred->constant);
    for (ValueID array : inferred->arrays)
        room = add(room, arrayLengthOrBail(array, fallback));
    LValue made;
    if (indexingType == ArrayWithContiguous)
        made = callAllocationStub(Stub::EmptyArrayWithRoom, { { room.value(), stubTemporaryGPRs[0] } }, effectsOfOperation<operationAOTTypedNewArray>(), true);
    else {
        orElse(belowOrEqual(room, integer(maxCapacityOfArrayOfNumbers)), fallback);
        made = allocateArrayOfNumbers(indexingType, m_out.shl(m_out.bitOr(room.value(), m_out.constInt64(1)), m_out.constInt32(capacityShiftInArrayOfNumbers)), fallback);
    }
    ValueFromBlock sized = m_out.anchor(made);
    m_out.jump(continuation);
    m_out.appendTo(fallback);
    ValueFromBlock usual = m_out.anchor(newArrayOfType(m_function.at(id).arguments, arrayTypeAtBirth(id)).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, sized, usual));
}

CellPointer Lowering::newArrayLiteralWithAllocationSite(ValueID id)
{
    auto& value = m_function.at(id);
    Type type = arrayTypeAtBirth(id);
    LValue statics = startOfStatics();
    if (value.copyOnWriteButterflyOffset() != Format::none) {
        IndexingType indexingType = indexingTypeOfLiteral(value.arguments, type);
        // Among doubles it would read as a hole.
        for (ValueID element : value.arguments) {
            if (indexingType == ArrayWithDouble && m_function.at(element).opcode == Opcode::Double && std::isnan(std::bit_cast<double>(m_function.at(element).bits())))
                indexingType = ArrayWithContiguous;
        }
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue shared = m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, statics, value.copyOnWriteButterflyOffset()));
        orElse(m_out.notZero64(shared), slowPath);
        // Zero once arrays are no longer allocated with their original structures.
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        LValue header = changing64(Instance::offsetOfNewCopyOnWriteArrayHeaders() + ((indexingType & IndexingShapeMask) - Int32Shape) / 2 * sizeof(uint64_t));
        orElse(m_out.notZero64(header), slowPath);
        LValue array = allocateHeapCell(fixedPointer(Instance::offsetOfArrayAllocator()), sizeof(JSArray), slowPath);
        applyEffects(array->as<PatchpointValue>(), { Writes::RuntimeState, false });
        storeHeader(array, header);
        m_out.storePtr(m_out.add(shared, m_out.constIntPtr(JSCellButterfly::offsetOfData())), array, m_heaps.JSObject_butterfly);
        fenceAfterInitialization();
        ValueFromBlock fast = m_out.anchor(array);
        m_out.jump(continuation);
        m_out.appendTo(slowPath);
        ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArrayBufferFromData, word32(value.copyOnWriteButterflyOffset()), word32(indexingType)).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return CellPointer(m_out.phi(Int64, fast, slow));
    }
    if (value.unsharedLiteralOfConstants() != Format::none)
        return AOT_TYPED_OPERATION(NewArrayOfConstants, word32(value.unsharedLiteralOfConstants()), word32(indexingTypeOfLiteral(value.arguments, type)));
    if (value.vectorLengthHintOffset() == Format::none)
        return newArrayOfType(value.arguments, type);
    static_assert(static_cast<unsigned>(Stub::EmptyArrayAtSite) + 1 == static_cast<unsigned>(Stub::LeafEmptyArrayAtSite));
    bool hasStubs = indexingTypeAtBirth(type) == ArrayWithContiguous;
    if (hasStubs) {
        ++m_context.numberOfEmptyArrayLiteralsMadeByStub;
        return CellPointer(callAllocationStub(Stub::EmptyArrayAtSite, { { m_out.constInt64(value.vectorLengthHintOffset()), stubTemporaryGPRs[0] } }, effectsOfOperation<operationAOTTypedNewArray>(), true));
    }
    m_context.numberOfEmptyArrayLiteralsMadeInPlace += hasStubs;

    LBasicBlock isDueForReview = newColdBlock();
    LBasicBlock isCounted = m_out.newBlock();
    LBasicBlock hasCapacityOfItsOwn = m_out.newBlock();
    LBasicBlock asUsual = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    TypedPointer site = m_out.address(m_heaps.AOTTyped_collectorState, statics, value.vectorLengthHintOffset() + OBJECT_OFFSETOF(ArraySite, capacityAndBorn));
    LValue counted = m_out.add(m_out.load32(site), m_out.constInt32(1 << ArraySite::bornShift));
    m_out.store32(counted, site);
    ValueFromBlock asItWas = m_out.anchor(counted);
    m_out.branch(m_out.testIsZero32(counted, m_out.constInt32((ArraySite::interval - 1) << ArraySite::bornShift)), rarely(isDueForReview), usually(isCounted));
    m_out.appendTo(isDueForReview);
    AOT_TYPED_OPERATION(ReviewVectorLengthHint, word32(value.vectorLengthHintOffset()));
    ValueFromBlock reviewed = m_out.anchor(m_out.load32(site));
    m_out.jump(isCounted);
    m_out.appendTo(isCounted);
    LValue capacity = m_out.bitAnd(m_out.phi(Int32, asItWas, reviewed), m_out.constInt32(ArraySite::capacityMask));
    if (auto entries = mostPushedInLoopOverCollection(id)) {
        LValue room = m_out.select(m_out.isZero32(capacity), m_out.constInt32(vectorLengthOfArrayLiteral(0)), capacity);
        LValue bound = m_out.bitOr(entries->value(), m_out.int32One);
        capacity = m_out.select(m_out.below(bound, room), bound, room);
    }
    Vector<ValueFromBlock, 2> small;
    if (hasStubs) {
        for (auto [room, stub] : { std::pair { 1, Stub::EmptyArrayWithRoomFor1 }, std::pair { 3, Stub::EmptyArrayWithRoomFor3 } }) {
            LBasicBlock isThat = m_out.newBlock();
            LBasicBlock isNot = m_out.newBlock();
            m_out.branch(m_out.equal(capacity, m_out.constInt32(room)), unsure(isThat), unsure(isNot));
            m_out.appendTo(isThat);
            small.append(m_out.anchor(callAllocationStub(stub, { }, effectsOfOperation<operationAOTTypedNewArray>(), true)));
            m_out.jump(continuation);
            m_out.appendTo(isNot);
        }
    } else if (indexingTypeAtBirth(type) == ArrayWithDouble) {
        static_assert(static_cast<unsigned>(Stub::EmptyArrayOfDoublesWithRoomFor1) + 1 == static_cast<unsigned>(Stub::LeafEmptyArrayOfDoublesWithRoomFor1));
        static_assert(static_cast<unsigned>(Stub::EmptyArrayOfDoublesWithRoomFor3) + 1 == static_cast<unsigned>(Stub::LeafEmptyArrayOfDoublesWithRoomFor3));
        for (auto [room, stub] : { std::pair { 1, Stub::EmptyArrayOfDoublesWithRoomFor1 }, std::pair { 3, Stub::EmptyArrayOfDoublesWithRoomFor3 } }) {
            LBasicBlock isThat = m_out.newBlock();
            LBasicBlock isNot = m_out.newBlock();
            m_out.branch(m_out.equal(capacity, m_out.constInt32(room)), unsure(isThat), unsure(isNot));
            m_out.appendTo(isThat);
            LValue made = callAllocationStub(stub, { }, effectsOfOperation<operationAOTTypedNewArrayOfNumbers>(), true);
            orElse(m_out.notZero64(made), asUsual);
            small.append(m_out.anchor(made));
            m_out.jump(continuation);
            m_out.appendTo(isNot);
        }
    }
    static_assert(vectorLengthOfArrayLiteral(0) == 5);
    m_out.branch(hasStubs ? m_out.above(capacity, m_out.constInt32(vectorLengthOfArrayLiteral(0))) : m_out.notZero32(capacity), unsure(hasCapacityOfItsOwn), unsure(asUsual));
    m_out.appendTo(hasCapacityOfItsOwn);
    ValueFromBlock hinted = [&] {
        if (!hasStubs)
            return m_out.anchor(allocateArrayOfNumbers(indexingTypeAtBirth(type), m_out.shl(m_out.zeroExtPtr(m_out.bitOr(capacity, m_out.int32One)), m_out.constInt32(capacityShiftInArrayOfNumbers)), asUsual));
        CellPointer roomy = allocateArrayWithHoles(Integer(m_out.zeroExtPtr(capacity)), ArrayWithContiguous, asUsual);
        storeField(butterflyOf(roomy), m_heaps.Butterfly_publicLength, word32(0));
        return m_out.anchor(roomy.value());
    }();
    m_out.jump(continuation);
    m_out.appendTo(asUsual);
    ValueFromBlock usual = m_out.anchor(newArrayOfType(value.arguments, type).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    LValue made = m_out.phi(Int64, hinted, usual);
    for (ValueFromBlock& one : small)
        m_out.addIncomingToPhi(made, one);
    return CellPointer(made);
}

// Before a push that may have found no room is carried out.
// The literal that the receiver of a push is, or may be where ways meet: `c ? [] : a.slice(0, i)`. None where more than one may come along.
ValueID Lowering::emptyLiteralBehind(ValueID receiver)
{
    ValueID origin = originOf(receiver);
    if (m_function.at(origin).opcode == Opcode::NewArray)
        return origin;
    BlockID meeting = m_definingBlock[origin.index()];
    size_t position = m_function.at(meeting).parameters.find(origin);
    if (position == notFound)
        return ValueID();
    ValueID found;
    for (auto& block : m_function.blocks) {
        for (auto& target : block.terminator.targets) {
            if (target.block != meeting || position >= target.arguments.size())
                continue;
            ValueID incoming = originOf(target.arguments[position]);
            if (m_function.at(incoming).opcode != Opcode::NewArray || incoming == found)
                continue;
            if (found)
                return ValueID();
            found = incoming;
        }
    }
    return found;
}

void Lowering::updateVectorLengthHint(CellPointer array, ValueID receiver)
{
    ValueID literal = emptyLiteralBehind(receiver);
    if (!literal || m_function.at(literal).vectorLengthHintOffset() == Format::none)
        return;
    // The arm is also come to by what is not stored in place for another reason. The room is only at fault where there is none left.
    LBasicBlock isFull = newColdBlock();
    LBasicBlock continuation = newColdBlock();
    Pointer butterfly = butterflyOf(array);
    m_out.branch(aboveOrEqual(publicLength(butterfly), vectorLength(butterfly)).value(), unsure(isFull), unsure(continuation));
    m_out.appendTo(isFull);
    AOT_TYPED_OPERATION(UpdateVectorLengthHint, array, word32(m_function.at(literal).vectorLengthHintOffset()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

// How many turns of the innermost loop are left, this one among them, where what is being lowered is run once a turn and the loop counts up by one to a bound: `for (; i < n; i++)`, left
// nowhere else. Zero: that is not known. It decides how much room is made for what the loop pushes, and nothing else.
Word32 Lowering::turnsLeftInLoop()
{
    static constexpr int64_t maxTurns = 1 << 20;
    auto counted = countedLoopContaining(m_currentBlockID);
    if (!counted)
        return word32(0);
    ValueID sourceOfBound = counted->iteratedArray ? m_function.at(m_function.at(m_loops[counted->index].header).terminator.value).child(0) : counted->bound;
    if (!isAvailableAtCurrentValue(sourceOfBound) || !get(sourceOfBound).value)
        return word32(0);
    Integer bound = counted->iteratedArray ? arrayLength(asCell(in<Boxed>(m_function.at(m_function.at(m_loops[counted->index].header).terminator.value).child(0)))) : in<Integer>(counted->bound);
    Integer turns = sub(bound, in<Integer>(counted->counter));
    return low32(select(below(turns, integer(maxTurns)), turns, integer(0)));
}

std::optional<Word32> Lowering::mostPushedInLoopOverCollection(ValueID literal)
{
    unsigned index = m_innermostLoop[m_currentBlockID.index()];
    if (index == noLoop)
        return std::nullopt;
    ValueID collection;
    bool isMap = false;
    for (size_t block : m_loops[index].blocks) {
        if (m_innermostLoop[block] != index || collection)
            continue;
        for (ValueID id : m_function.blocks[block].values) {
            isMap = isInlineBuiltin(id, InlineBuiltin::MapNext);
            if (!isMap && !isInlineBuiltin(id, InlineBuiltin::SetNext))
                continue;
            collection = m_function.at(id).arguments[0];
            break;
        }
    }
    if (!collection || !isAvailableAtCurrentValue(collection) || !get(collection).value)
        return std::nullopt;
    unsigned pushedInTurn = 0;
    for (unsigned block = 0; block < m_function.blocks.size(); ++block) {
        for (ValueID id : m_function.blocks[block].values) {
            if (!isInlineBuiltin(id, InlineBuiltin::ArrayPush) || emptyLiteralBehind(m_function.at(id).child(0)) != literal)
                continue;
            if (m_innermostLoop[block] != index)
                return std::nullopt;
            pushedInTurn += m_function.at(id).arguments.size();
        }
    }
    if (!pushedInTurn)
        return std::nullopt;
    auto unknown = [&] {
        return word32(std::numeric_limits<int32_t>::max());
    };
    auto ofCell = [&](CellPointer cell) {
        return either<Word32>(equal(typeOfCell(cell), word32(isMap ? JSMapType : JSSetType)), [&] {
            return low32(mul(sizeOfMapOrSet(cell, isMap, false), integer(pushedInTurn)));
        }, unknown);
    };
    Operand operand = get(collection);
    if (operand.rep == Rep::Cell && !typeOf(collection).mayBeUndefinedOrNull())
        return ofCell(operand.as<CellPointer>());
    Boxed boxed = convertTo<Boxed>(operand, typeOf(collection));
    return either<Word32>(isCell(boxed, typeOf(collection)), [&] {
        return ofCell(asCell(boxed));
    }, unknown);
}

// By the text of the function: `[...a, x, y]`: two to what the spread yields.
unsigned Lowering::numberOfElementsPushedTo(ValueID array)
{
    unsigned count = 0;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::CallBuiltin)
                continue;
            auto& builtin = m_builtins.at(value.builtin());
            if (builtin.takesReceiver() && value.child(0) && builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::ArrayPush && originOf(value.child(0)) == array)
                count += value.arguments.size();
        }
    }
    return count;
}

// `[.., ...iterable]`: AppendSpread target, iterable. The target is an array literal that nobody has seen. Done here where the other is an array with its elements in place,
// kept the way the literal keeps its own: nobody can tell that it was not iterated. The iteration yields `undefined` for a hole, which only values can hold.
// Null: there is no code for this one. Else the block that all else goes to, with the target as it was.
LBasicBlock Lowering::appendSpread(const Value& value)
{
    if (auto* engineClass = typeOf(value.arguments[1]).onlyEngineClass(); engineClass && engineClass->typedArrayType != NotTypedArray)
        return appendSpreadOfTypedArray(value);
    ValueID literal = originOf(value.arguments[0]);
    while (m_function.at(literal).opcode == Opcode::CallBuiltin && m_function.at(literal).builtin() == value.builtin())
        literal = originOf(m_function.at(literal).arguments[0]);
    Operand iterable = get(value.arguments[1]);
    if (!holdsCells(iterable.rep) || !typeOf(value.arguments[1]).hasObjects())
        return nullptr;
    LBasicBlock otherwise = newUnlikelyBlock();
    if (m_function.at(literal).opcode != Opcode::NewArray || (!m_isInLoop && !m_program.isHotByText(m_id) && !m_spreadsCarriedOutAtBirth.contains(m_currentBlock->values[m_currentIndexInBlock].index()))) {
        if (iterable.rep != Rep::Cell)
            orElse(isCell(iterable.as<Boxed>(), typeOf(value.arguments[1])), otherwise);
        CellPointer array = iterable.rep == Rep::Cell ? iterable.as<CellPointer>() : asCell(iterable.as<Boxed>());
        orElse(equal(typeOfCell(array), word32(ArrayType)), otherwise);
        orElse(isZero(publicLength(butterflyOf(array))), otherwise);
        didEmitWrite(effectsOfBuiltinCall(value).writes);
        return otherwise;
    }
    if (auto isBornForIt = m_spreadsCarriedOutAtBirth.getOptional(m_currentBlock->values[m_currentIndexInBlock].index())) {
        orElse(Word32(*isBornForIt), otherwise);
        didEmitWrite(effectsOfBuiltinCall(value).writes);
        return otherwise;
    }
    IndexingType indexingType = indexingTypeOfLiteral(m_function.at(literal).arguments, arrayTypeAtBirth(literal));
    auto& elements = elementsOf(indexingType);
    bool holdsValues = indexingType == ArrayWithContiguous;

    CellPointer target = asCell(in<Boxed>(value.arguments[0]));
    CellPointer source;
    if (iterable.rep == Rep::Cell)
        source = iterable.as<CellPointer>();
    else {
        orElse(isCell(iterable.as<Boxed>(), typeOf(value.arguments[1])), otherwise);
        source = asCell(iterable.as<Boxed>());
    }
    if (typeOf(value.arguments[1]).onlyEngineClass() != &m_builtins.arrayClass())
        orElse(equal(typeOfCell(source), word32(ArrayType)), otherwise);
    orElse(equal(indexingModeOf(target, IndexingShapeAndWritabilityMask), word32(indexingType & IndexingShapeMask)), otherwise);
    Word32 shape = indexingModeOf(source, IndexingShapeMask);
    Word32 fits = equal(shape, word32(indexingType & IndexingShapeMask));
    if (holdsValues)
        fits = logicalOr(fits, equal(shape, word32(Int32Shape)));
    orElse(fits, otherwise);

    Pointer from = butterflyOf(source);
    Integer count = publicLength(from);
    orElse(belowOrEqual(count, integer(maxLengthToCopyOneByOne)), otherwise);
    Pointer old = butterflyOf(target);
    Integer length = publicLength(old);
    Integer newLength = add(length, count);
    // With room for what the text still has for it.
    unsigned toCome = numberOfElementsPushedTo(m_currentBlock->values[m_currentIndexInBlock]);
    Integer needed = add(newLength, integer(toCome));
    LBasicBlock grow = m_out.newBlock();
    LBasicBlock moveLoop = m_out.newBlock();
    LBasicBlock move = m_out.newBlock();
    LBasicBlock hasRoom = m_out.newBlock();
    LBasicBlock copyLoop = m_out.newBlock();
    LBasicBlock copy = m_out.newBlock();
    LBasicBlock copied = m_out.newBlock();
    LBasicBlock publish = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    didEmitWrite(effectsOfBuiltinCall(value).writes);
    ValueFromBlock inPlace = m_out.anchor(old.value());
    Word32 hasRoomInPlace = belowOrEqual(needed, vectorLength(old));
    if (m_literalsWithRoomForAllSpreads.get(literal.index()))
        orElse(hasRoomInPlace, otherwise);
    m_out.branch(hasRoomInPlace.value(), unsure(hasRoom), unsure(grow));

    // With the header the butterfly is a multiple of 16 bytes. The last slot is beyond the length if that is even.
    m_out.appendTo(grow);
    LValue capacity = m_out.bitOr(needed.value(), m_out.constInt64(1));
    LValue size = m_out.add(m_out.shl(capacity, m_out.constInt32(3)), m_out.constIntPtr(sizeof(IndexingHeader)));
    LValue storage = allocateStorage(size, otherwise);
    LValue grown = m_out.add(storage, m_out.constIntPtr(sizeof(IndexingHeader)));
    m_out.store32(m_out.castToInt32(length.value()), grown, m_heaps.Butterfly_publicLength);
    m_out.store32(m_out.castToInt32(capacity), grown, m_heaps.Butterfly_vectorLength);
    // What is between the length and the capacity is kept clear.
    if (!toCome)
        m_out.store64(m_out.constInt64(holeIn(indexingType)), TypedPointer(elements, m_out.add(storage, m_out.shl(capacity, m_out.constInt32(3)))));
    else {
        LBasicBlock clearLoop = m_out.newBlock();
        LBasicBlock clear = m_out.newBlock();
        LBasicBlock cleared = m_out.newBlock();
        ValueFromBlock firstCleared = m_out.anchor(newLength.value());
        m_out.jump(clearLoop);
        m_out.appendTo(clearLoop);
        LValue slot = m_out.phi(pointerType(), firstCleared);
        m_out.branch(m_out.below(slot, capacity), unsure(clear), unsure(cleared));
        m_out.appendTo(clear);
        m_out.store64(m_out.constInt64(holeIn(indexingType)), m_out.baseIndex(elements, grown, slot, FTL::ScaleEight));
        m_out.addIncomingToPhi(slot, m_out.anchor(m_out.add(slot, m_out.intPtrOne)));
        m_out.jump(clearLoop);
        m_out.appendTo(cleared);
    }
    ValueFromBlock firstMoved = m_out.anchor(m_out.intPtrZero);
    m_out.jump(moveLoop);
    m_out.appendTo(moveLoop);
    LValue moved = m_out.phi(pointerType(), firstMoved);
    ValueFromBlock outOfPlace = m_out.anchor(grown);
    m_out.branch(m_out.below(moved, length.value()), unsure(move), unsure(hasRoom));
    m_out.appendTo(move);
    m_out.store64(m_out.load64(m_out.baseIndex(elements, old.value(), moved, FTL::ScaleEight)), m_out.baseIndex(elements, grown, moved, FTL::ScaleEight));
    m_out.addIncomingToPhi(moved, m_out.anchor(m_out.add(moved, m_out.intPtrOne)));
    m_out.jump(moveLoop);

    m_out.appendTo(hasRoom);
    LValue butterfly = m_out.phi(pointerType(), inPlace, outOfPlace);
    LValue to = m_out.add(butterfly, m_out.shl(length.value(), m_out.constInt32(3)));
    ValueFromBlock firstCopied = m_out.anchor(m_out.intPtrZero);
    m_out.jump(copyLoop);
    m_out.appendTo(copyLoop);
    LValue index = m_out.phi(pointerType(), firstCopied);
    m_out.branch(m_out.below(index, count.value()), unsure(copy), unsure(copied));
    m_out.appendTo(copy);
    LValue element = m_out.load64(m_out.baseIndex(elements, from.value(), index, FTL::ScaleEight));
    LValue isHole = m_out.equal(element, m_out.constInt64(holeIn(indexingType)));
    if (holdsValues)
        element = m_out.select(isHole, undefined().value(), element);
    else {
        // What has been stored beyond the length is cleared again: there everything reads as a hole.
        LBasicBlock clearLoop = newColdBlock();
        LBasicBlock clear = newColdBlock();
        LBasicBlock isElement = m_out.newBlock();
        ValueFromBlock reached = m_out.anchor(index);
        m_out.branch(isHole, rarely(clearLoop), usually(isElement));
        m_out.appendTo(clearLoop);
        LValue left = m_out.phi(pointerType(), reached);
        m_out.branch(m_out.isZero64(left), unsure(otherwise), unsure(clear));
        m_out.appendTo(clear);
        LValue previous = m_out.sub(left, m_out.intPtrOne);
        m_out.store64(m_out.constInt64(holeIn(indexingType)), m_out.baseIndex(elements, to, previous, FTL::ScaleEight));
        m_out.addIncomingToPhi(left, m_out.anchor(previous));
        m_out.jump(clearLoop);
        m_out.appendTo(isElement);
    }
    m_out.store64(element, m_out.baseIndex(elements, to, index, FTL::ScaleEight));
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.intPtrOne)));
    m_out.jump(copyLoop);

    m_out.appendTo(copied);
    ensureStillAliveHere(source);
    m_out.store32(m_out.castToInt32(newLength.value()), butterfly, m_heaps.Butterfly_publicLength);
    m_out.branch(m_out.equal(butterfly, old.value()), unsure(done), unsure(publish));
    m_out.appendTo(publish);
    fenceAfterInitialization();
    m_out.storePtr(butterfly, target.value(), m_heaps.JSObject_butterfly);
    m_out.jump(done);
    m_out.appendTo(done);
    barrier(target);
    return otherwise;
}

// An element of an array of values that is read as a type that is kept as a double. One test for the usual case: where it is a number, that is what is read. Goes on where it
// is none, which a hole is not either. As the engine keeps it, a number is none of what stands for `undefined` or `null`.
void Lowering::addIfNumber(Join& join, Boxed element, const Type& type)
{
    Rep rep = repOf(type);
    if ((rep != Rep::Double && rep != Rep::OptionalDouble) || Options::auditAOTTypedAssertions())
        return;
    LBasicBlock isNumberCase = m_out.newBlock();
    LBasicBlock isOther = newUnlikelyBlock();
    m_out.branch(isNumber(element).value(), usually(isNumberCase), rarely(isOther));
    m_out.appendTo(isNumberCase);
    join.add(Operand(rep, select(isInt32(element), toNumber(low32(element)), unboxedDouble(element)).value()));
    m_out.appendTo(isOther);
}

// It was compared with itself: it is no hole, so it is a number.
Operand Lowering::elementOfDoubles(Number element, const Type& type)
{
    Rep rep = repOf(type);
    if (rep == Rep::Double || rep == Rep::OptionalDouble)
        return Operand(rep, element.value());
    return ensure(element, m_program.at(m_program.types.number), type);
}

// With its elements in place the array is as it was made, and finds the engine's function under the name. Any other is asked by name.
Boxed Lowering::callArrayMethodOnSlowPath(const Value& value, CellPointer array)
{
    if (m_expectedMethodThatWasRead == value.builtin())
        return callBuiltinFunction(value, { }, box(array));
    return callMethodGenerally(box(array), m_program.stringOf(m_builtins.at(value.builtin()).nameName), value.arguments);
}

Operand Lowering::lowerArrayAt(const Value& value, const Type& type)
{
    StringID name = m_program.stringOf(m_builtins.at(value.builtin()).nameName);
    CellPointer array = nonNil(value.child(0), name);
    Rep rep = repOf(type);
    LBasicBlock doubles = m_out.newBlock();
    LBasicBlock notDoubles = m_out.newBlock();
    LBasicBlock absent = m_out.newBlock();
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    Integer given = in<Integer>(value.arguments[0]);
    Word32 shape = indexingModeOf(array, IndexingShapeMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    Integer index = select(lessThan(given, integer(0)), add(given, length), given);
    m_out.branch(equal(shape, word32(DoubleShape)).value(), unsure(doubles), unsure(notDoubles));

    m_out.appendTo(doubles);
    orElse(below(index, length), absent);
    Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
    orElse(equal(asDouble, asDouble), absent);
    join.add(elementOfDoubles(asDouble, type));

    m_out.appendTo(notDoubles);
    orElse(logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))), slowPath);
    orElse(below(index, length), absent);
    Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
    addIfNumber(join, element, type);
    orElse(isNotEmpty(element), absent);
    join.add(ensure(element, type));

    m_out.appendTo(absent);
    join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));

    m_out.appendTo(slowPath);
    join.add(ensure(callArrayMethodOnSlowPath(value, array), type));
    return join.finish();
}

// Likewise. toString() finds the engine's join().
Operand Lowering::lowerArrayJoin(const Value& value, const Type& type, ValueID separator)
{
    StringID name = m_program.stringOf(m_builtins.at(value.builtin()).nameName);
    CellPointer array = nonNil(value.child(0), name);
    LBasicBlock slowPath = newUnlikelyBlock();
    Join join(m_out, repOf(type), b3TypeOf(repOf(type)));
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    orElse(belowOrEqual(binary<Word32>(&Output::sub, indexingModeOf(array, IndexingShapeMask), word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
    CellPointer joined = AOT_TYPED_OPERATION(ArrayJoin, array, separator ? in<CellPointer>(separator) : CellPointer(m_out.int64Zero));
    orElse(Word32(m_out.notZero64(joined.value())), slowPath);
    join.add(ensure(joined, m_program.at(m_program.types.string), type));

    m_out.appendTo(slowPath);
    join.add(ensure(callArrayMethodOnSlowPath(value, array), type));
    return join.finish();
}

// Of arrays that have their original structures and keep their elements the same way: none has a property of its own but its elements, so each is spread, and the result is an
// array of this realm that keeps them that way too. Holes are copied as holes. What is no object is an element.
Operand Lowering::lowerArrayConcat(const Value& value, const Type& type)
{
    StringID name = m_program.stringOf(m_builtins.at(value.builtin()).nameName);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    // Null: the argument itself is the element.
    Vector<CellPointer, 4> arrays { nonNil(value.child(0), name) };
    bool areElementsNumbers = true;
    bool areElementsInt32 = true;
    for (ValueID argument : value.arguments) {
        if (!typeOf(argument).hasObjects()) {
            areElementsNumbers &= typeOf(argument).isNumber();
            areElementsInt32 &= isInt32Type(typeOf(argument));
            arrays.append(CellPointer());
            continue;
        }
        Operand operand = get(argument);
        if (operand.rep != Rep::Cell)
            orElse(isCell(operand.as<Boxed>(), typeOf(argument)), slowPath);
        arrays.append(operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>()));
    }
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    Word32 shape = indexingModeOf(arrays[0], IndexingShapeMask);
    orElse(belowOrEqual(binary<Word32>(&Output::sub, shape, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
    static_assert(Instance::arrayKindShift == 1);
    auto originalStructureID = [&](Word32 mode) {
        return Word32(m_out.load32(TypedPointer(m_heaps.AOTInstance_fixedFields, m_out.add(m_instance, m_out.add(m_out.shl(m_out.zeroExtPtr(mode.value()), m_out.constInt32(1)), m_out.constIntPtr(Instance::offsetOfOriginalArrayStructureIDs()))))));
    };
    Vector<Pointer, 4> butterflies;
    Vector<Integer, 4> lengths;
    Integer length = integer(0);
    for (CellPointer array : arrays) {
        if (!array) {
            butterflies.append(Pointer());
            lengths.append(integer(1));
            length = add(length, integer(1));
            continue;
        }
        Word32 mode = indexingModeOf(array, IndexingShapeMask | CopyOnWrite);
        orElse(equal(binary<Word32>(&Output::bitAnd, mode, word32(IndexingShapeMask)), shape), slowPath);
        orElse(equal(loadField<Word32>(array, m_heaps.JSCell_structureID), originalStructureID(mode)), slowPath);
        butterflies.append(butterflyOf(array));
        lengths.append(publicLength(butterflies.last()));
        length = add(length, lengths.last());
    }
    orElse(belowOrEqual(length, integer(maxLengthToCopyOneByOne)), slowPath);

    orElse(m_out.notZero32(changing32(Instance::offsetOfNewArrayWithContiguousStructureID())), slowPath);
    LValue capacity = m_out.bitOr(length.value(), m_out.constInt64(1));
    LValue size = m_out.add(m_out.shl(capacity, m_out.constInt32(3)), m_out.constIntPtr(sizeof(IndexingHeader)));
    LValue storage = allocateStorage(size, slowPath);
    LValue made = m_out.add(storage, m_out.constIntPtr(sizeof(IndexingHeader)));
    m_out.store32(m_out.castToInt32(length.value()), made, m_heaps.Butterfly_publicLength);
    m_out.store32(m_out.castToInt32(capacity), made, m_heaps.Butterfly_vectorLength);
    LBasicBlock copied = m_out.newBlock();
    // The last slot is beyond the length if that is even.
    auto copyElements = [&](IndexingType indexingType) {
        auto& elements = elementsOf(indexingType);
        m_out.store64(m_out.constInt64(holeIn(indexingType)), TypedPointer(elements, m_out.add(storage, m_out.shl(capacity, m_out.constInt32(3)))));
        LValue to = made;
        for (unsigned i = 0; i < arrays.size(); ++i) {
            if (!arrays[i]) {
                ValueID element = value.arguments[i - 1];
                if (indexingType != ArrayWithDouble)
                    m_out.store64(in<Boxed>(element).value(), TypedPointer(elements, to));
                else {
                    Number number = in<Number>(element);
                    // Among doubles it would read as a hole.
                    if (!typeOf(element).isInteger())
                        orElse(equal(number, number), slowPath);
                    m_out.storeDouble(number.value(), TypedPointer(elements, to));
                }
                to = m_out.add(to, m_out.intPtrEight);
                continue;
            }
            LBasicBlock loop = m_out.newBlock();
            LBasicBlock element = m_out.newBlock();
            LBasicBlock next = m_out.newBlock();
            ValueFromBlock start = m_out.anchor(m_out.intPtrZero);
            m_out.jump(loop);
            m_out.appendTo(loop);
            LValue index = m_out.phi(pointerType(), start);
            m_out.branch(m_out.below(index, lengths[i].value()), unsure(element), unsure(next));
            m_out.appendTo(element);
            m_out.store64(m_out.load64(m_out.baseIndex(elements, butterflies[i].value(), index, FTL::ScaleEight)), m_out.baseIndex(elements, to, index, FTL::ScaleEight));
            m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.intPtrOne)));
            m_out.jump(loop);
            m_out.appendTo(next);
            to = m_out.add(to, m_out.shl(lengths[i].value(), m_out.constInt32(3)));
        }
        m_out.jump(copied);
    };
    LBasicBlock doubles = m_out.newBlock();
    LBasicBlock values = m_out.newBlock();
    m_out.branch(equal(shape, word32(DoubleShape)).value(), unsure(doubles), unsure(values));
    m_out.appendTo(doubles);
    if (areElementsNumbers)
        copyElements(ArrayWithDouble);
    else
        m_out.jump(slowPath);
    m_out.appendTo(values);
    if (!areElementsInt32)
        orElse(equal(shape, word32(ContiguousShape)), slowPath);
    copyElements(ArrayWithContiguous);

    m_out.appendTo(copied);
    for (CellPointer array : arrays) {
        if (array)
            ensureStillAliveHere(array);
    }
    LValue result = allocateHeapCell(fixedPointer(Instance::offsetOfArrayAllocator()), sizeof(JSArray), slowPath);
    applyEffects(result->as<PatchpointValue>(), { Writes::RuntimeState, false });
    m_out.store32(originalStructureID(shape).value(), result, m_heaps.JSCell_structureID);
    m_out.store32(m_out.bitOr(shape.value(), m_out.constInt32(arrayTypeInfoBlob(ArrayClass))), result, m_heaps.JSCell_usefulBytes);
    m_out.storePtr(made, result, m_heaps.JSObject_butterfly);
    fenceAfterInitialization();
    ValueFromBlock fast = m_out.anchor(result);
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(callArrayMethodOnSlowPath(value, arrays[0]).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return ensure(Boxed(m_out.phi(Int64, fast, slow)), type);
}

// Of a short array with its elements in place, which it may write to. The others move down one by one, holes as holes. The engine has better ways for a long one.
Operand Lowering::lowerArrayShift(const Value& value, const Type& type)
{
    constexpr unsigned maxLengthToMoveOneByOne = 32;
    StringID name = m_program.stringOf(m_builtins.at(value.builtin()).nameName);
    CellPointer array = nonNil(value.child(0), name);
    Rep rep = repOf(type);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock absent = m_out.newBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    didEmitWrite(Writes::Heap);
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
    orElse(belowOrEqual(binary<Word32>(&Output::sub, mode, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    orElse(belowOrEqual(sub(length, integer(1)), integer(maxLengthToMoveOneByOne - 1)), slowPath);
    Integer last = sub(length, integer(1));
    auto shift = [&](IndexingType indexingType) {
        auto& elements = elementsOf(indexingType);
        LBasicBlock loop = m_out.newBlock();
        LBasicBlock element = m_out.newBlock();
        LBasicBlock moved = m_out.newBlock();
        LValue first = m_out.load64(TypedPointer(elements, butterfly.value()));
        ValueFromBlock start = m_out.anchor(m_out.intPtrZero);
        m_out.jump(loop);
        m_out.appendTo(loop);
        LValue index = m_out.phi(pointerType(), start);
        m_out.branch(m_out.below(index, last.value()), unsure(element), unsure(moved));
        m_out.appendTo(element);
        LValue next = m_out.add(index, m_out.intPtrOne);
        m_out.store64(m_out.load64(m_out.baseIndex(elements, butterfly.value(), next, FTL::ScaleEight)), m_out.baseIndex(elements, butterfly.value(), index, FTL::ScaleEight));
        m_out.addIncomingToPhi(index, m_out.anchor(next));
        m_out.jump(loop);
        m_out.appendTo(moved);
        m_out.store64(m_out.constInt64(holeIn(indexingType)), m_out.baseIndex(elements, butterfly.value(), last.value(), FTL::ScaleEight));
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(last));
        // What has moved may have passed the collector by.
        if (indexingType != ArrayWithDouble)
            barrier(array);
        orElse(m_out.notEqual(first, m_out.constInt64(holeIn(indexingType))), absent);
        return first;
    };
    LBasicBlock doubles = m_out.newBlock();
    LBasicBlock values = m_out.newBlock();
    m_out.branch(equal(mode, word32(DoubleShape)).value(), unsure(doubles), unsure(values));
    m_out.appendTo(doubles);
    join.add(elementOfDoubles(Number(m_out.bitCast(shift(ArrayWithDouble), Double)), type));
    m_out.appendTo(values);
    Boxed first(shift(ArrayWithContiguous));
    addIfNumber(join, first, type);
    join.add(ensure(first, type));

    m_out.appendTo(absent);
    join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));

    m_out.appendTo(slowPath);
    join.add(ensure(callArrayMethodOnSlowPath(value, array), type));
    return join.finish();
}

// The receiver has its original structure, so the result is an array of this realm that keeps its elements the same way. Holes are copied as holes.
Operand Lowering::lowerArraySlice(const Value& value, const Type& type)
{
    auto& arguments = value.arguments;
    CellPointer array = nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    LBasicBlock slowPath = newBlockForGeneralCall(value);
    LBasicBlock continuation = m_out.newBlock();
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    m_context.emitsCalls = true;
    ValueFromBlock byStub;
    if (arguments.isEmpty())
        byStub = m_out.anchor(callAllocationStub(Stub::ArrayCopy, { { array.value(), GPRInfo::toArgumentRegister(1) } }, effectsOfOperation<operationAOTTypedArrayCopy>(), true));
    else {
        Integer first = indexOrElse(arguments[0], slowPath);
        Integer behindLast = arguments.size() > 1 ? indexOrElse(arguments[1], slowPath) : integer(static_cast<int64_t>(1) << 32);
        byStub = m_out.anchor(callAllocationStub(Stub::ArraySlice, { { array.value(), GPRInfo::toArgumentRegister(1) }, { first.value(), GPRInfo::toArgumentRegister(2) }, { behindLast.value(), GPRInfo::toArgumentRegister(3) } }, effectsOfOperation<operationAOTTypedArraySlice>(), true));
    }
    Vector<ValueFromBlock, 2> made { byStub };
    m_out.jump(continuation);
    if (!isGeneralArmOnOffer(slowPath)) {
        m_out.appendTo(slowPath);
        made.append(m_out.anchor(callBuiltinFunction(value, OperationEffects { Writes::Heap, true }).value()));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
    return ensure(Boxed(m_out.phi(Int64, made)), Type::engineClass(m_builtins.classes()[value.engineClass()]), type);
}

// `array.splice(start, removed, items..)`. The receiver has its original structure. In place where `removed` is a small constant, nobody looks at what was removed, and the array keeps its
// elements in place and may write to them: what comes after moves one by one, holes as holes. The engine has better ways for a long tail.
Operand Lowering::lowerArraySplice(ValueID result, const Value& value, const Type& type)
{
    constexpr unsigned maxLengthToMoveOneByOne = 32;
    constexpr unsigned maxItems = 2;
    constexpr int64_t maxRemoved = 2;
    auto& arguments = value.arguments;
    CellPointer array = nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    Type newArrayType = Type::engineClass(m_builtins.classes()[value.engineClass()]);
    bool isUsed = numberOfUsesOf(result);
    Arguments items;
    if (arguments.size() > 2)
        items.append(arguments.span().subspan(2));
    Rep rep = repOf(type);
    Join join(m_out, rep, b3TypeOf(rep));
    // A number that the type does not say is an int32 mostly is one. All else is for the engine's function.
    LBasicBlock isNot = newBlockForGeneralCall(value);
    Integer start = indexOrElse(arguments[0], isNot);
    if (!isInt32Type(typeOf(arguments[0])))
        orElse(equal(signExtended(low32(start)), start), isNot);

    Range given = arguments.size() > 1 ? typeOf(arguments[1]).integers : Range::full();
    if (given.least == given.most && given.least >= 0 && given.least <= maxRemoved && items.size() <= maxItems && (!isUsed || !given.least)) {
        int64_t removed = given.least;
        int64_t added = items.size();
        bool areNumbers = true;
        bool areInt32 = true;
        for (ValueID item : items) {
            areNumbers &= typeOf(item).isNumber();
            areInt32 &= isInt32Type(typeOf(item));
        }
        LBasicBlock slowPath = newColdBlock();
        didEmitWrite(Writes::Heap);
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        // Not what is shared until it is written to.
        Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
        orElse(belowOrEqual(binary<Word32>(&Output::sub, mode, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
        Pointer butterfly = butterflyOf(array);
        Integer length = publicLength(butterfly);
        Integer fromEnd = add(start, length);
        Integer from = select(lessThan(start, integer(0)), select(lessThan(fromEnd, integer(0)), integer(0), fromEnd), select(lessThan(start, length), start, length));
        // Negative: there is less to remove.
        Integer tail = sub(length, add(from, integer(removed)));
        orElse(belowOrEqual(tail, integer(maxLengthToMoveOneByOne)), slowPath);
        Integer newLength = add(length, integer(added - removed));
        if (added > removed) {
            LBasicBlock hasRoom = m_out.newBlock();
            LBasicBlock isFull = newColdBlock();
            ValueFromBlock asItIs = m_out.anchor(butterfly.value());
            m_out.branch(belowOrEqual(newLength, vectorLength(butterfly)).value(), usually(hasRoom), rarely(isFull));
            m_out.appendTo(isFull);
            orElse(logicalNot(isZero(AOT_TYPED_OPERATION(ArrayGrow, array, low32(newLength)))), slowPath);
            ValueFromBlock grown = m_out.anchor(butterflyOf(array).value());
            m_out.jump(hasRoom);
            m_out.appendTo(hasRoom);
            butterfly = Pointer(m_out.phi(pointerType(), asItIs, grown));
        }
        auto splice = [&](IndexingType indexingType) {
            auto& elements = elementsOf(indexingType);
            Vector<LValue, maxItems> stored;
            for (ValueID item : items) {
                if (indexingType != ArrayWithDouble) {
                    stored.append(in<Boxed>(item).value());
                    continue;
                }
                Number number = in<Number>(item);
                // Among doubles it would read as a hole.
                if (!typeOf(item).isInteger())
                    orElse(equal(number, number), slowPath);
                stored.append(m_out.bitCast(number.value(), Int64));
            }
            auto addressOf = [&](Integer index) {
                return m_out.add(butterfly.value(), m_out.shl(index.value(), m_out.constInt32(3)));
            };
            if (added != removed) {
                bool movesUp = added > removed;
                LValue source = addressOf(add(from, integer(removed)));
                LValue target = addressOf(add(from, integer(added)));
                LBasicBlock loop = m_out.newBlock();
                LBasicBlock element = m_out.newBlock();
                LBasicBlock moved = m_out.newBlock();
                // From the far end, so that nothing is overwritten before it has moved.
                ValueFromBlock first = m_out.anchor(movesUp ? tail.value() : m_out.int64Zero);
                m_out.jump(loop);
                m_out.appendTo(loop);
                LValue index = m_out.phi(Int64, first);
                m_out.branch(movesUp ? m_out.notZero64(index) : m_out.below(index, tail.value()), unsure(element), unsure(moved));
                m_out.appendTo(element);
                LValue place = movesUp ? m_out.sub(index, m_out.constInt64(1)) : index;
                m_out.store64(m_out.load64(m_out.baseIndex(elements, source, place, FTL::ScaleEight)), m_out.baseIndex(elements, target, place, FTL::ScaleEight));
                m_out.addIncomingToPhi(index, m_out.anchor(movesUp ? place : m_out.add(index, m_out.constInt64(1))));
                m_out.jump(loop);
                m_out.appendTo(moved);
                // What is between the length and the capacity is kept clear.
                for (int64_t i = 0; i < removed - added; ++i)
                    m_out.store64(m_out.constInt64(holeIn(indexingType)), TypedPointer(elements, addressOf(add(newLength, integer(i)))));
            }
            for (unsigned i = 0; i < stored.size(); ++i)
                m_out.store64(stored[i], TypedPointer(elements, addressOf(add(from, integer(i)))));
            if (added != removed)
                storeField(butterfly, m_heaps.Butterfly_publicLength, low32(newLength));
            // What has moved may have passed the collector by.
            if (indexingType != ArrayWithDouble)
                barrier(array);
            join.add(ensure(isUsed ? newArray(Arguments { }) : array, newArrayType, type));
        };
        LBasicBlock doubles = m_out.newBlock();
        LBasicBlock values = m_out.newBlock();
        m_out.branch(equal(mode, word32(DoubleShape)).value(), unsure(doubles), unsure(values));
        m_out.appendTo(doubles);
        if (areNumbers)
            splice(ArrayWithDouble);
        else
            m_out.jump(slowPath);
        m_out.appendTo(values);
        if (!areInt32)
            orElse(equal(mode, word32(ContiguousShape)), slowPath);
        splice(ArrayWithContiguous);
        m_out.appendTo(slowPath);
    }

    auto [count, buffer] = argumentBuffer(items);
    Word32 deleteCount = arguments.size() > 1 ? low32(in<Integer>(arguments[1])) : word32(INT32_MAX);
    LValue done = AOT_TYPED_OPERATION(ArraySplice, array, low32(start), deleteCount, count, buffer, word32(isUsed)).value();
    LBasicBlock isDone = m_out.newBlock();
    m_out.branch(m_out.notZero64(done), usually(isDone), rarely(isNot));
    m_out.appendTo(isDone);
    join.add(ensure(Boxed(done), newArrayType, type));
    if (!isGeneralArmOnOffer(isNot)) {
        m_out.appendTo(isNot);
        join.add(ensure(callBuiltinFunction(value, OperationEffects { Writes::Heap, true }), newArrayType, type));
    }
    return join.finish();
}

std::optional<Operand> Lowering::lowerArraySearchAsOneCall(ValueID result, const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    if (builtin.implementation.kind != BuiltinImplementation::Inline || (builtin.implementation.inlined != InlineBuiltin::ArrayIndexOf && builtin.implementation.inlined != InlineBuiltin::ArrayIncludes))
        return std::nullopt;
    if (value.arguments.size() != 1 || typeOf(value.arguments[0]).isNumber() || typeOf(value.child(0)).onlyEngineClass() != &m_builtins.arrayClass())
        return std::nullopt;
    bool isIndexOf = builtin.implementation.inlined == InlineBuiltin::ArrayIndexOf;
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    CellPointer array = nonNil(value.child(0), m_program.stringOf(builtin.nameName));
    LBasicBlock isValueOfCall = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    Vector<std::pair<LValue, GPRReg>, 4> operands { { array.value(), GPRInfo::argumentGPR1 }, { mapKey(value.arguments[0]).value(), GPRInfo::argumentGPR2 } };
    if (value.methodRead())
        operands.append({ in<Boxed>(value.methodRead()).value(), GPRInfo::argumentGPR3 });
    Stub stub = value.methodRead() ? (isIndexOf ? Stub::ArrayIndexOfOrCallMethodRead : Stub::ArrayIncludesOrCallMethodRead) : (isIndexOf ? Stub::ArrayIndexOfOrCall : Stub::ArrayIncludesOrCall);
    LValue answer = callICStub(stub, Int64, operands, std::nullopt, OperationEffects { Writes::Heap, true });
    ensureStillAliveHere(array);
    orElse(Word32(isIndexOf ? m_out.isZero64(m_out.lShr(answer, m_out.constInt32(32))) : m_out.belowOrEqual(answer, m_out.constInt64(1))), isValueOfCall);
    Word32 found(m_out.castToInt32(answer));
    if (isIndexOf)
        join.add(ensure(signExtended(found), Type::integer({ -1, INT32_MAX }), type));
    else
        join.add(yieldBoolean(found, type));
    m_out.appendTo(isValueOfCall);
    LValue yielded = answer;
    if (isIndexOf) {
        static_assert(!(arraySearchIsHandedBack & JSValue::NumberTag) && arraySearchIsHandedBack >> 32);
        LValue handedBack = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfVM()), VM::offsetOfEncodedHostCallReturnValue()));
        yielded = m_out.select(m_out.equal(answer, m_out.constInt64(arraySearchIsHandedBack)), handedBack, answer);
    }
    join.add(ensure(Boxed(yielded), type));
    return join.finish();
}

// Among doubles. -1: there is none. What is not a number is equal to none, and none of them is that.
Integer Lowering::indexOfNumber(CellPointer array, Number searched)
{
    LBasicBlock isLong = newUnlikelyBlock();
    LBasicBlock loop = m_out.newBlock();
    LBasicBlock element = m_out.newBlock();
    LBasicBlock next = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    ValueFromBlock start = m_out.anchor(m_out.int64Zero);
    m_out.branch(below(length, integer(minLengthToSearchOutOfLine)).value(), usually(loop), rarely(isLong));
    m_out.appendTo(isLong);
    ValueFromBlock outOfLine = m_out.anchor(signExtended(AOT_TYPED_OPERATION(ArrayIndexOfDouble, array, searched)).value());
    m_out.jump(continuation);
    m_out.appendTo(loop);
    LValue index = m_out.phi(Int64, start);
    ValueFromBlock none = m_out.anchor(m_out.constInt64(-1));
    m_out.branch(m_out.below(index, length.value()), unsure(element), unsure(continuation));
    m_out.appendTo(element);
    ValueFromBlock found = m_out.anchor(index);
    m_out.branch(m_out.doubleEqual(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index)), searched.value()), unsure(continuation), unsure(next));
    m_out.appendTo(next);
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.constInt64(1))));
    m_out.jump(loop);
    m_out.appendTo(continuation);
    ensureStillAliveHere(array);
    return Integer(m_out.phi(Int64, none, found, outOfLine));
}

static bool isTypedArrayWithElementsReadInPlace(const Type& type)
{
    auto* engineClass = type.onlyEngineClass();
    if (!engineClass)
        return false;
    switch (engineClass->typedArrayType) {
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

std::optional<Writes> writesOfInlineArray(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::ArrayFrom:
        return writesIf(isTypedArrayWithElementsReadInPlace(arguments[0]), Writes::RuntimeState);
    case InlineBuiltin::ArrayLength:
        return Writes::Nothing;
    case InlineBuiltin::ArrayEntries:
        return writesIf(arguments.empty(), Writes::RuntimeState);
    case InlineBuiltin::ArrayPush:
        return writesIf(!arguments.empty() && arguments.size() <= 8, Writes::Heap);
    case InlineBuiltin::ArraySetLength:
    case InlineBuiltin::ArrayPop:
        return Writes::Heap;
    case InlineBuiltin::ArrayAt:
        return writesIf(arguments[0].isInteger(), Writes::Heap);
    case InlineBuiltin::ArrayJoin:
        return writesIf(arguments.empty() || arguments[0].isString(), Writes::Heap);
    case InlineBuiltin::ArrayToString:
    case InlineBuiltin::ArrayShift:
        return Writes::Heap;
    case InlineBuiltin::ArrayConcat:
        return writesIf(!arguments.empty() && arguments.size() <= 3 && std::ranges::all_of(arguments, [](auto& argument) {
            return !argument.hasObjects() || argument.onlyEngineClass() == &Builtins::singleton().arrayClass();
        }), Writes::Heap);
    case InlineBuiltin::ArrayUnshift:
        return writesIf(arguments.size() == 1, Writes::Heap);
    // To compare it with another may flatten a rope.
    case InlineBuiltin::ArrayIndexOf:
    case InlineBuiltin::ArrayIncludes:
        return writesIf(arguments.size() == 1, Writes::RuntimeState);
    case InlineBuiltin::ArraySlice:
        return writesIf(std::ranges::all_of(arguments, [](auto& argument) {
            return argument.isNumber();
        }), Writes::RuntimeState);
    case InlineBuiltin::ArraySplice:
        return writesIf(!arguments.empty() && arguments[0].isNumber() && (arguments.size() == 1 || isInt32Type(arguments[1])), Writes::Heap);
    case InlineBuiltin::ArrayWithLength:
    case InlineBuiltin::ArrayOf:
        return Writes::RuntimeState;
    // What a proxy stands for is asked of the engine, which runs no code of the program for it.
    case InlineBuiltin::ArrayIsArray:
        return arguments[0].hasObjects() ? Writes::RuntimeState : Writes::Pure;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<Operand> Lowering::lowerInlineArray(InlineBuiltin which, ValueID result, const Value& value, Writes inPlace)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto receiver = [&] {
        return nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    };
    auto generally = [&] {
        return callBuiltinFunction(value, OperationEffects { Writes::Heap, true });
    };
    // What the operation yields, or what the engine's function yields where it has done nothing.
    auto orGenerally = [&](LValue done, const Type& guarantee) {
        LBasicBlock isDone = m_out.newBlock();
        LBasicBlock isNot = newBlockForGeneralCall(value);
        Join join(m_out, Rep::JSValue, Int64);
        m_out.branch(m_out.notZero64(done), usually(isDone), rarely(isNot));
        m_out.appendTo(isDone);
        join.add(Boxed(done));
        if (!isGeneralArmOnOffer(isNot)) {
            m_out.appendTo(isNot);
            join.add(generally());
        }
        return ensure(join.finish(), guarantee, type);
    };

    switch (which) {
    // What only a loop that takes the pairs apart gets hold of is no iterator: see findEntriesOfArrays(). It is no object either, and nothing asks.
    case InlineBuiltin::ArrayEntries:
        if (!m_entriesMadeAsRecords.get(result.index()))
            return ensure(generally(), type);
        return convert(AOT_TYPED_OPERATION(NewEntriesOfArray, receiver()), Type::engineClass(m_builtins.arrayClass()), repOf(type));
    case InlineBuiltin::ArrayFrom:
        return lowerArrayFromTypedArray(result, value, type);
    case InlineBuiltin::ArrayAt:
        return lowerArrayAt(value, type);
    case InlineBuiltin::ArrayJoin:
        return lowerArrayJoin(value, type, arguments.isEmpty() ? ValueID() : arguments[0]);
    case InlineBuiltin::ArrayToString:
        return lowerArrayJoin(value, type, { });
    case InlineBuiltin::ArrayConcat:
        return lowerArrayConcat(value, type);
    case InlineBuiltin::ArrayShift:
        return lowerArrayShift(value, type);
    case InlineBuiltin::ArrayPop: {
        CellPointer array = receiver();
        Rep rep = repOf(type);
        LBasicBlock hasElements = m_out.newBlock();
        LBasicBlock hasNone = m_out.newBlock();
        LBasicBlock values = m_out.newBlock();
        LBasicBlock doubles = m_out.newBlock();
        LBasicBlock slowPath = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        didEmitWrite(Writes::Heap);
        // Not what is shared until it is written to.
        Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
        Word32 isDouble = equal(mode, word32(DoubleShape));
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        orElse(belowOrEqual(binary<Word32>(&Output::sub, mode, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
        Pointer butterfly = butterflyOf(array);
        Integer length = publicLength(butterfly);
        m_out.branch(isZero(length).value(), rarely(hasNone), usually(hasElements));

        m_out.appendTo(hasElements);
        Integer last = add(length, integer(-1));
        m_out.branch(isDouble.value(), unsure(doubles), unsure(values));

        // For a hole the array is asked the general way. What is between the length and the capacity is kept clear.
        m_out.appendTo(values);
        TypedPointer slot = m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), last.value());
        Boxed element(m_out.load64(slot));
        orElse(isNotEmpty(element), slowPath);
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(last));
        m_out.store64(m_out.constInt64(JSValue::encode(JSValue())), slot);
        addIfNumber(join, element, type);
        join.add(ensure(element, type));

        m_out.appendTo(doubles);
        TypedPointer slotOfDouble = m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), last.value());
        Number asDouble(m_out.loadDouble(slotOfDouble));
        orElse(equal(asDouble, asDouble), slowPath);
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(last));
        m_out.storeDouble(m_out.constDouble(PNaN), slotOfDouble);
        join.add(elementOfDoubles(asDouble, type));

        m_out.appendTo(hasNone);
        join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));

        m_out.appendTo(slowPath);
        join.add(ensure(generally(), type));
        return join.finish();
    }
    case InlineBuiltin::ArraySlice:
        return lowerArraySlice(value, type);
    case InlineBuiltin::ArraySplice:
        return lowerArraySplice(result, value, type);
    // Stub::ArrayIndexOf and Stub::ArrayIncludes go through boxed elements: by identity, or as strings; what takes more they leave to the engine's
    // operation. A hole is no element to the one, and `undefined` to the other, which is left to the operation too.
    case InlineBuiltin::ArrayIndexOf:
    case InlineBuiltin::ArrayIncludes: {
        bool isIndexOf = which == InlineBuiltin::ArrayIndexOf;
        CellPointer array = receiver();
        Rep rep = repOf(type);
        LBasicBlock slowPath = newBlockForGeneralCall(value);
        Join join(m_out, rep, b3TypeOf(rep));
        Word32 shape = indexingModeOf(array, IndexingShapeMask);
        if (typeOf(arguments[0]).isNumber()) {
            LBasicBlock doubles = m_out.newBlock();
            LBasicBlock notDoubles = m_out.newBlock();
            m_out.branch(equal(shape, word32(DoubleShape)).value(), unsure(doubles), unsure(notDoubles));
            m_out.appendTo(doubles);
            auto yieldIndex = [&](Integer index) {
                if (isIndexOf)
                    join.add(ensure(index, Type::integer({ -1, INT32_MAX }), type));
                else
                    join.add(yieldBoolean(greaterThanOrEqual(index, integer(0)), type));
            };
            yieldIndex(indexOfNumber(array, in<Number>(arguments[0])));
            m_out.appendTo(notDoubles);
            LBasicBlock longInt32s = newUnlikelyBlock();
            LBasicBlock others = m_out.newBlock();
            Word32 isLong = aboveOrEqual(publicLength(butterflyOf(array)), integer(minLengthToSearchOutOfLine));
            m_out.branch(logicalAnd(equal(shape, word32(Int32Shape)), isLong).value(), rarely(longInt32s), usually(others));
            m_out.appendTo(longInt32s);
            yieldIndex(signExtended(AOT_TYPED_OPERATION(ArrayIndexOfInt32, array, mapKey(arguments[0]))));
            m_out.appendTo(others);
        }
        orElse(logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))), slowPath);
        Vector<std::pair<LValue, GPRReg>, 4> operands { { butterflyOf(array).value(), GPRInfo::argumentGPR1 }, { mapKey(arguments[0]).value(), GPRInfo::argumentGPR2 } };
        Word32 found(m_out.castToInt32(callICStub(isIndexOf ? Stub::ArrayIndexOf : Stub::ArrayIncludes, Int64, operands, std::nullopt, OperationEffects { Writes::RuntimeState, true })));
        ensureStillAliveHere(array);
        if (isIndexOf)
            join.add(ensure(signExtended(found), Type::integer({ -1, INT32_MAX }), type));
        else
            join.add(yieldBoolean(logicalNot(isZero(found)), type));
        if (!isGeneralArmOnOffer(slowPath)) {
            m_out.appendTo(slowPath);
            join.add(ensure(callBuiltinFunction(value, OperationEffects { Writes::RuntimeState, true }), type));
        }
        return join.finish();
    }
    case InlineBuiltin::ArrayUnshift:
        return orGenerally(AOT_TYPED_OPERATION(ArrayUnshift, receiver(), in<Boxed>(arguments[0])).value(), m_program.at(m_program.types.anything));
    default:
        return lowerContainerBuiltin(which, result, value, inPlace);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
