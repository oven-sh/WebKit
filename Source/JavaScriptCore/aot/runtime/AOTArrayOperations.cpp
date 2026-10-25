/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTConvention.h"
#include "AOTFrameArrays.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "AOTVector.h"
#include "ButterflyInlines.h"
#include "DeferGC.h"
#include "DeferTermination.h"
#include "ExceptionHelpers.h"
#include "GCDeferralContextInlines.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSObjectVisitInlines.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static NEVER_INLINE bool appendBoxedDoubles(JSGlobalObject* globalObject, VM& vm, JSArray* target, const double* numbers, unsigned length)
{
    Vector<EncodedJSValue, 16> boxed(length, [&](size_t i) {
        return JSValue::encode(JSValue(JSValue::EncodeAsDouble, numbers[i]));
    });
    return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithContiguous, boxed.span());
}

// NaN stands for a hole in Double storage.
static bool holdsOnlyNumbersForDoubleStorage(std::span<const EncodedJSValue> values)
{
    return std::ranges::all_of(values, [](EncodedJSValue encoded) {
        JSValue value = JSValue::decode(encoded);
        return value.isInt32() || (value.isDouble() && !std::isnan(value.asDouble()));
    });
}

static bool tryAppendArray(JSGlobalObject* globalObject, VM& vm, JSArray* target, JSArray* source)
{
    IndexingType type = source->indexingType();
    unsigned length = source->length();
    if (hasDouble(type)) {
        if (containsHole(source->butterfly()->contiguousDouble().data(), length))
            return false;
        if (hasContiguous(target->indexingType()) && copiesIntoDenseStorage(vm.arrayStoragePolicy(), static_cast<uint64_t>(target->length()) + length))
            return appendBoxedDoubles(globalObject, vm, target, source->butterfly()->contiguousDouble().data(), length);
    } else if (hasInt32(type) || hasContiguous(type)) {
        if (containsHole(source->butterfly()->contiguous().data(), length))
            return false;
        IndexingType targetType = target->indexingType();
        std::span<const EncodedJSValue> values { std::bit_cast<const EncodedJSValue*>(source->butterfly()->contiguous().data()), length };
        if (hasInt32(type) && (hasContiguous(targetType) || hasDouble(targetType)))
            return target->appendMemcpy(globalObject, vm, target->length(), hasDouble(targetType) ? ArrayWithDouble : ArrayWithContiguous, values);
        if (hasContiguous(type) && hasDouble(targetType) && holdsOnlyNumbersForDoubleStorage(values))
            return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithDouble, values);
    } else
        return false;
    return target->appendMemcpy(globalObject, vm, target->length(), source);
}

// A hole reads as undefined when no prototype of the source holds an indexed element.
static NEVER_INLINE bool tryAppendArrayWithHoles(JSGlobalObject* globalObject, VM& vm, JSArray* target, JSArray* source)
{
    IndexingType type = source->indexingType();
    unsigned length = source->length();
    if (!hasInt32(type) && !hasDouble(type) && !hasContiguous(type))
        return false;
    if (source->structure()->holesMustForwardToPrototype(source) || !copiesIntoDenseStorage(vm.arrayStoragePolicy(), static_cast<uint64_t>(target->length()) + length))
        return false;
    // The source keeps these values alive.
    Vector<EncodedJSValue, 16> values(length, [&](size_t i) {
        if (hasDouble(type)) {
            double number = source->butterfly()->contiguousDouble().data()[i];
            return JSValue::encode(std::isnan(number) ? jsUndefined() : JSValue(JSValue::EncodeAsDouble, number));
        }
        JSValue value = source->butterfly()->contiguous().data()[i].get();
        return JSValue::encode(value ? value : jsUndefined());
    });
    if (!target->tryMakeWritableContiguous(vm).data())
        return false;
    return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithContiguous, values.span());
}

static bool tryAppendFrameArray(JSGlobalObject* globalObject, VM& vm, JSArray* target, JSArray* frameArray)
{
    Butterfly* elements = frameArray->butterfly();
    std::span<const EncodedJSValue> values { std::bit_cast<const EncodedJSValue*>(elements->contiguous().data()), elements->publicLength() };
    return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithContiguous, values);
}

JSArray* copyFrameArrayToHeap(Instance* instance, JSCell* frameArray)
{
    Butterfly* elements = uncheckedDowncast<JSArray>(frameArray)->butterfly();
    unsigned length = elements->publicLength();
    JSArray* copy = JSArray::create(*instance->vm, instance->globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), length);
    for (unsigned index = 0; index < length; ++index)
        copy->butterfly()->contiguous().atUnsafe(index).setWithoutWriteBarrier(elements->contiguous().atUnsafe(index).get());
    return copy;
}

JSC_DEFINE_JIT_OPERATION(operationAOTAppendElements, void, (Instance* instance, JSCell* cell, JSCell* sourceCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAppendElements);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    JSArray* target = cellArgument<JSArray>(cell, Entry::operationAOTAppendElements);
    JSArray* source = cellArgument<JSArray>(sourceCell, Entry::operationAOTAppendElements);
    // No dense storage holds more, and element by element the rest would go into a sparse map.
    if (static_cast<uint64_t>(target->length()) + source->length() > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope);
    }
    if (isFrameArray(source) ? tryAppendFrameArray(globalObject, vm, target, source) : tryAppendArray(globalObject, vm, target, source))
        OPERATION_RETURN(scope);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!isFrameArray(source) && tryAppendArrayWithHoles(globalObject, vm, target, source))
        OPERATION_RETURN(scope);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    unsigned length = source->length();
    for (unsigned i = 0; i < length; ++i) {
        JSValue element = source->getIndex(globalObject, i);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        target->push(globalObject, element);
        OPERATION_RETURN_IF_EXCEPTION(scope);
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTAppendHole, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAppendHole);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTAppendHole);
    array->setLength(globalObject, array->length() + 1, true);
    OPERATION_RETURN(scope, array);
}

void reserveArrayCapacity(VM& vm, JSArray* array, uint64_t additional)
{
    IndexingType mode = array->indexingMode();
    if (mode != ArrayWithInt32 && mode != ArrayWithDouble && mode != ArrayWithContiguous)
        return;
    if (additional > runtimeLimitValue(vm, LimitID::MaxReservedArrayCapacity)) {
        countLimitHit(vm, LimitID::MaxReservedArrayCapacity);
        return;
    }
    unsigned publicLength = array->butterfly()->publicLength();
    uint64_t vectorLength = publicLength + additional;
    if (vectorLength > MAX_STORAGE_VECTOR_LENGTH)
        return;

    // ensureLength(length) makes room for nextLength(length) elements.
    unsigned length = static_cast<unsigned>(vectorLength - vectorLength / 3);
    ASSERT(nextLength(length) >= vectorLength);
    if (length <= array->butterfly()->vectorLength() || !array->ensureLength(vm, length))
        return;
    array->butterfly()->setPublicLength(publicLength);
}

JSArray* tryCreateArrayWithStorage(VM& vm, Structure* structure, unsigned publicLength, unsigned vectorLength, unsigned propertyRoom)
{
    IndexingType indexingType = structure->indexingType();
    RELEASE_ASSERT(publicLength <= vectorLength && !structure->outOfLineCapacity() && !structure->hasElementsStartOffset() && structure->classInfoForCells() == JSArray::info());
    RELEASE_ASSERT(hasInt32(indexingType) || hasDouble(indexingType) || hasContiguous(indexingType));
    if (!vectorLength || vectorLength > MarkedSpace::largeCutoff / sizeof(EncodedJSValue))
        return nullptr;
    size_t size = sizeof(JSArray) + propertyRoom + Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue));
    if (size > MarkedSpace::largeCutoff)
        return nullptr;
    char* cell = static_cast<char*>(vm.cellSpace().allocate(vm, size, nullptr, AllocationFailureMode::Assert));
    std::fill_n(std::bit_cast<uint64_t*>(cell + sizeof(JSArray)), propertyRoom / sizeof(uint64_t), 0);
    Butterfly* butterfly = Butterfly::fromBase(cell + sizeof(JSArray) + propertyRoom, 0, 0);
    butterfly->setVectorLength(vectorLength);
    butterfly->setPublicLength(publicLength);
    uint64_t hole = hasDouble(indexingType) ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
    std::fill_n(butterfly->indexingPayload<uint64_t>(), vectorLength, hole);
    *std::bit_cast<Butterfly**>(cell + JSObject::butterflyOffset()) = butterfly;
    *std::bit_cast<uint64_t*>(cell) = cellHeaderFor(structure);
    vm.mutatorFence();
    return std::bit_cast<JSArray*>(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReserveArrayCapacity, void, (Instance* instance, JSCell* cell, uint64_t additional))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTReserveArrayCapacity);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    reserveArrayCapacity(vm, cellArgument<JSArray>(cell, Entry::operationAOTReserveArrayCapacity), additional);
}

// How many of the elements are numbers that Double storage can hold, from the first on. NaN stands for a hole there.
static unsigned countLeadingNumbersAndHoles(const Butterfly* values, bool& holdsNumber)
{
    unsigned publicLength = values->publicLength();
    for (unsigned i = 0; i < publicLength; ++i) {
        JSValue value = values->contiguous().atUnsafe(i).get();
        if (!value)
            continue;
        if (!value.isNumber() || std::isnan(value.asNumber()))
            return i;
        holdsNumber = true;
    }
    return publicLength;
}

// General code makes an array with storage for any value, also where all that it stores are numbers.
static void convertStorageOfNumbers(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSArray* array)
{
    constexpr uint32_t costOfLook = 16;
    constexpr uint32_t worthOfConversion = 64;
    uint32_t& lookedAtInVain = instance->arrays.elementsLookedAtInVain;
    // The place has said which it is.
    if (array->cellState() != CellState::PossiblyBlack && array->butterfly()->publicLength() < ArrayWords::minLengthThatPaysAtOnce)
        ++instance->arrays.shortYoungArraysConverted[instance->arrays.placeThatConverts % ArrayWords::numberOfCounts];

    Structure* structure = array->structure();
    bool isPlainArray = structure == globalObject->originalArrayStructureForIndexingType(ArrayWithContiguous) && !isCopyOnWrite(array->indexingMode()) && !structure->outOfLineCapacity();
    Butterfly* values = array->butterfly();
    unsigned publicLength = values->publicLength();
    bool holdsNumber = false;
    unsigned numberOfLeadingNumbers = isPlainArray ? countLeadingNumbersAndHoles(values, holdsNumber) : 0;
    if (numberOfLeadingNumbers < publicLength || !holdsNumber) {
        lookedAtInVain = std::min<uint64_t>(static_cast<uint64_t>(lookedAtInVain) + costOfLook + numberOfLeadingNumbers, ArrayWords::maxElementsLookedAtInVain);
        return;
    }

    unsigned vectorLength = values->vectorLength();
    Butterfly* numbers = Butterfly::tryCreateUninitialized(vm, array, 0, 0, true, vectorLength * sizeof(double));
    if (!numbers)
        return;
    numbers->setVectorLength(vectorLength);
    numbers->setPublicLength(publicLength);
    for (unsigned i = 0; i < vectorLength; ++i) {
        JSValue value = i < publicLength ? values->contiguous().atUnsafe(i).get() : JSValue();
        numbers->contiguousDouble().atUnsafe(i) = value ? value.asNumber() : PNaN;
    }
    array->nukeStructureAndSetButterfly(vm, structure->id(), numbers);
    array->setStructure(vm, globalObject->originalArrayStructureForIndexingType(ArrayWithDouble));
    lookedAtInVain -= std::min(lookedAtInVain, worthOfConversion);
}

// What a native function or general code has just made: an array of the realm with storage of its own.
static bool tryConvertPlainInt32Array(JSGlobalObject* globalObject, VM& vm, JSArray* array)
{
    Structure* structure = array->structure();
    if (structure != globalObject->originalArrayStructureForIndexingType(ArrayWithInt32) || array->indexingMode() != ArrayWithInt32 || structure->transitionWatchpointSetIsStillValid())
        return false;
    Butterfly* values = array->butterfly();
    for (unsigned i = values->vectorLength(); i--;) {
        JSValue value = values->contiguous().atUnsafe(i).get();
        values->contiguousDouble().atUnsafe(i) = value.isInt32() ? value.asInt32() : PNaN;
    }
    array->setStructure(vm, globalObject->originalArrayStructureForIndexingType(ArrayWithDouble));
    return true;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTConvertToDoubleStorage, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConvertToDoubleStorage);
    DeferTerminationForAWhile deferTermination(vm);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTConvertToDoubleStorage);
    // An array that refuses stays as it was, and goes on failing the test for Double storage.
    if (tryConvertPlainInt32Array(globalObject, vm, array))
        return;
    if (hasInt32(array->indexingType()) || hasUndecided(array->indexingType()))
        array->tryMakeWritableDouble(vm);
    else if (hasContiguous(array->indexingType()))
        convertStorageOfNumbers(instance, globalObject, vm, array);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArray, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArray);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    static_assert(storageOfItsOwnInArrayLiteralCount < 1u << capacityShiftInArrayLiteralCount);
    unsigned emptyCapacity = count >> capacityShiftInArrayLiteralCount;
    bool hasStorageOfItsOwn = count & storageOfItsOwnInArrayLiteralCount;
    count &= storageOfItsOwnInArrayLiteralCount - 1;
    if (emptyCapacity && count) [[unlikely]]
        crashOnWrongOperand(Entry::operationAOTNewArray);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    unsigned vectorLength = emptyCapacity ? emptyCapacity : arrayLiteralVectorLength(count);
    if (hasContiguous(structure->indexingType()) && !structure->outOfLineCapacity() && vectorLength <= maxHoleArrayCapacity) {
        if (JSArray* array = hasStorageOfItsOwn ? nullptr : tryCreateArrayWithStorage(vm, structure, count, vectorLength)) {
            for (uint32_t i = 0; i < count; ++i)
                array->butterfly()->contiguous().atUnsafe(i).setWithoutWriteBarrier(JSValue::decode(values[i]));
            OPERATION_RETURN(scope, array);
        }
        if (void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull)) {
            Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
            butterfly->setVectorLength(vectorLength);
            butterfly->setPublicLength(count);
            for (uint32_t i = 0; i < vectorLength; ++i)
                butterfly->contiguous().atUnsafe(i).setWithoutWriteBarrier(i < count ? JSValue::decode(values[i]) : JSValue());
            OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, structure, butterfly));
        }
    }
    JSArray* array = JSArray::tryCreate(vm, structure, count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    for (uint32_t i = 0; i < count; ++i) {
        array->putDirectIndex(globalObject, i, JSValue::decode(values[i]));
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayOfHoles, JSCell*, (Instance* instance, uint32_t lengthAndCapacity, uint32_t flags))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayOfHoles);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    unsigned length = lengthAndCapacity & ((1u << holeArrayCapacityShift) - 1);
    unsigned capacity = lengthAndCapacity >> holeArrayCapacityShift;
    RELEASE_ASSERT(length <= capacity && (flags == ArrayWithDouble || flags == ArrayWithInt32 || flags == ArrayWithContiguous));
    IndexingType indexingType = static_cast<IndexingType>(flags);
    if (globalObject->isHavingABadTime()) [[unlikely]] {
        JSArray* array = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), length);
        if (!array)
            throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, array);
    }
    Structure* structure = globalObject->originalArrayStructureForIndexingType(indexingType);
    if (JSArray* array = tryCreateArrayWithStorage(vm, structure, length, capacity))
        OPERATION_RETURN(scope, array);
    void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, capacity * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!storage) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
    butterfly->setVectorLength(capacity);
    butterfly->setPublicLength(length);
    for (unsigned i = 0; i < capacity; ++i) {
        if (indexingType == ArrayWithDouble)
            butterfly->contiguousDouble().atUnsafe(i) = PNaN;
        else
            butterfly->contiguous().atUnsafe(i).clear();
    }
    OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, structure, butterfly));
}

// JSObject::ensureLengthSlow() for a full vector with nothing in front of it: the same new length, the same rounding, and a vector with an allocation of its own is reallocated, so a grown array takes the memory it takes in JSArray::push().
static bool growFullVector(VM& vm, JSArray* array, unsigned length)
{
    static_assert(2 * MIN_SPARSE_ARRAY_INDEX <= MAX_STORAGE_VECTOR_LENGTH);
    Structure* structure = array->structure();
    if (length >= MIN_SPARSE_ARRAY_INDEX || structure->outOfLineCapacity() || structure->hasElementsStartOffset()) [[unlikely]]
        return false;

    GCDeferralContext deferralContext(vm);
    AssertNoGC assertNoGC;
    Butterfly* butterfly = array->butterfly();
    unsigned newVectorLength = isStorageInOwnCell(array, butterfly->base(0, 0)) ? length : Butterfly::availableContiguousVectorLength(static_cast<size_t>(0), length);
    bool hasRoomBehindVector = newVectorLength > length;
    if (!hasRoomBehindVector) {
        newVectorLength = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), static_cast<unsigned>(nextLength(length + 1)));
        butterfly = butterfly->reallocArrayRightIfPossible(vm, deferralContext, array, structure, 0, true, length * sizeof(EncodedJSValue), newVectorLength * sizeof(EncodedJSValue));
        if (!butterfly) [[unlikely]]
            return false;
    }

    uint64_t hole = hasDouble(array->indexingType()) ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
    std::fill_n(butterfly->indexingPayload<uint64_t>() + length, newVectorLength - length, hole);
    if (hasRoomBehindVector) {
        WTF::storeStoreFence();
        butterfly->setVectorLength(newVectorLength);
        return true;
    }
    butterfly->setVectorLength(newVectorLength);
    array->setButterfly(vm, butterfly);
    return true;
}

// 0: the array or the value is not of the plain sort, and nothing a program can see was changed.
unsigned pushToFullVector(VM& vm, JSArray* array, JSValue value)
{
    // The mode includes the copy-on-write bit, so shared storage is not taken.
    IndexingType mode = array->indexingMode();
    bool isTaken = mode == ArrayWithContiguous || (mode == ArrayWithInt32 ? value.isInt32() : mode == ArrayWithDouble && value.isNumber() && value.asNumber() == value.asNumber());
    if (!isTaken)
        return 0;
    unsigned length = array->butterfly()->publicLength();
    if (length != array->butterfly()->vectorLength() || !growFullVector(vm, array, length))
        return 0;
    Butterfly* butterfly = array->butterfly();
    if (mode == ArrayWithDouble)
        butterfly->contiguousDouble().at(array, length) = value.asNumber();
    else
        butterfly->contiguous().at(array, length).set(vm, array, value);
    butterfly->setPublicLength(length + 1);
    return length + 1;
}

// An array that takes no more elements, one whose length is fixed, and one below a prototype with elements of its own keep a map beside the vector or store the slow way. A frozen one may keep its elements where they were.
static bool mayRefuseOrRunOtherCode(JSArray* array)
{
    IndexingType type = array->indexingType();
    if (hasAnyArrayStorage(type))
        return hasSlowPutArrayStorage(type) || array->butterfly()->arrayStorage()->m_sparseMap;
    return isCopyOnWrite(array->indexingMode()) && array->hasFrozenElementsInPlace();
}

static EncodedJSValue pushToArray(Instance* instance, JSArray* array, JSValue value)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = globalObject->vm();
    if (unsigned newLength = pushToFullVector(vm, array, value))
        return JSValue::encode(jsNumber(newLength));
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Array.prototype.push() itself refuses, and runs the code of others, in a frame of its own.
    if (mayRefuseOrRunOtherCode(array)) [[unlikely]]
        RELEASE_AND_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypePush, array, value)));
    array->push(globalObject, value);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    return JSValue::encode(jsNumber(array->length()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPush, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayPush);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayPush);
    OPERATION_RETURN(scope, pushToArray(instance, array, JSValue::decode(value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPushNumber, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t numberBits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayPushNumber);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayPushNumber);
    double number = std::bit_cast<double>(numberBits);
    if (number == number)
        array->tryMakeWritableDouble(vm);
    OPERATION_RETURN(scope, pushToArray(instance, array, jsNumber(purifyNaN(number))));
}

static JSArray* tryFastSlice(VM& vm, JSGlobalObject* globalObject, JSArray* array, int64_t from, int64_t to)
{
    if (!array->isPristine() || globalObject->isHavingABadTime())
        return nullptr;
    IndexingType shape = array->indexingType() & IndexingShapeMask;
    if (shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape)
        return nullptr;
    int64_t length = array->butterfly()->publicLength();
    from = from < 0 ? std::max<int64_t>(from + length, 0) : std::min(from, length);
    to = to < 0 ? std::max<int64_t>(to + length, 0) : std::min(to, length);
    int64_t count = std::max<int64_t>(to - from, 0);
    if (count > maxLengthForElementwiseCopy)
        return JSArray::fastSlice(globalObject, array, from, count);
    unsigned vectorLength = static_cast<unsigned>(count) | 1;
    // The stub that makes room for named properties and the one that makes none end up here when their allocator has no cell left.
    auto hasCellsFor = [&](unsigned propertyRoom) {
        Allocator allocator = vm.cellSpace().allocatorFor(sizeOfArrayWithStorage(vectorLength, propertyRoom), AllocatorForMode::AllocatorIfExists);
        return allocator && !std::bit_cast<FreeList*>(std::bit_cast<char*>(allocator.localAllocator()) + LocalAllocator::offsetOfFreeList())->allocationWillFail();
    };
    unsigned propertyRoom = hasCellsFor(0) && !hasCellsFor(propertyRoomOfArray) ? propertyRoomOfArray : 0;
    JSArray* result = tryCreateArrayWithStorage(vm, globalObject->originalArrayStructureForIndexingType(static_cast<IndexingType>(IsArray | shape)), static_cast<unsigned>(count), vectorLength, propertyRoom);
    if (!result)
        return nullptr;
    std::copy_n(array->butterfly()->indexingPayload<uint64_t>() + from, count, result->butterfly()->indexingPayload<uint64_t>());
    ensureStillAliveHere(array);
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayCopy, EncodedJSValue, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayCopy);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayCopy);
    if (JSArray* copy = tryFastSlice(vm, globalObject, array, 0, std::numeric_limits<int64_t>::max()))
        OPERATION_RETURN(scope, JSValue::encode(copy));
    OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypeSlice, array)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySlice, EncodedJSValue, (Instance* instance, JSCell* cell, int64_t from, int64_t to))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySlice);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySlice);
    if (JSArray* part = tryFastSlice(vm, globalObject, array, from, to))
        OPERATION_RETURN(scope, JSValue::encode(part));
    OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypeSlice, array, jsNumber(static_cast<double>(from)), jsNumber(static_cast<double>(to)))));
}

static unsigned capacityInUse(const ArrayAllocationProfile& profile)
{
    return profile.capacity() ? profile.capacity() : arrayLiteralVectorLength(0);
}

static void startWindow(ArrayAllocationProfile& profile)
{
    profile.reviews = 0;
    std::ranges::fill(profile.largestLengths, 0);
}

// Every array of the site is born with the room, and only the ones that outgrow it pay for that.
void reviewArrayAllocationProfile(ArrayAllocationProfile& profile, JSArray* lastArrayOrNull)
{
    if (!lastArrayOrNull)
        return;
    IndexingType shape = lastArrayOrNull->indexingType() & IndexingShapeMask;
    if (shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape)
        return;

    IndexingType learnedShape = std::max(profile.shape(), shape);
    unsigned length = std::min<unsigned>(lastArrayOrNull->butterfly()->publicLength(), BASE_CONTIGUOUS_VECTOR_LEN_MAX);
    unsigned inUse = capacityInUse(profile);
    uint8_t entering = static_cast<uint8_t>(length);
    for (uint8_t& kept : profile.largestLengths) {
        if (entering > kept)
            std::swap(entering, kept);
    }

    unsigned wanted = profile.largestLengths[ArrayAllocationProfile::lengthsKept - 1] | 1;
    if (wanted > inUse) {
        profile.set(std::min<unsigned>(wanted, BASE_CONTIGUOUS_VECTOR_LEN_MAX), learnedShape);
        startWindow(profile);
        return;
    }
    if (++profile.reviews < ArrayAllocationProfile::reviewsInWindow) {
        profile.set(profile.capacity(), learnedShape);
        return;
    }

    profile.set(wanted < inUse ? wanted : profile.capacity(), learnedShape);
    startWindow(profile);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayAtReviewedSite, JSCell*, (Instance* instance, void* profileAddress, uint32_t seedIndexingType))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayAtReviewedSite);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    uint8_t* statics = instance->statics();
    uint8_t* address = static_cast<uint8_t*>(profileAddress);
    uint64_t distanceToLastArray = instance->arrays.distanceFromProfileToLastArray;
    RELEASE_ASSERT(address >= statics && distanceToLastArray >= sizeof(ArrayAllocationProfile));
    RELEASE_ASSERT(address + distanceToLastArray + ArrayAllocationProfile::sizeOfLastArrayWord <= statics + instance->tables().instanceSizes.sizeOfStatics);
    RELEASE_ASSERT(seedIndexingType == ArrayWithInt32 || seedIndexingType == ArrayWithDouble || seedIndexingType == ArrayWithContiguous);
    auto& profile = *reinterpret_cast<ArrayAllocationProfile*>(address);
    auto& lastArray = *reinterpret_cast<JSArray**>(address + distanceToLastArray);
    ASSERT(profile.isDueForReview());

    reviewArrayAllocationProfile(profile, lastArray);

    IndexingType shape = std::max<IndexingType>(profile.shape(), seedIndexingType & IndexingShapeMask);
    JSArray* array = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(IsArray | shape), 0, capacityInUse(profile));
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    // Not a root: every collection zeroes this word, so it neither keeps the array alive nor dangles.
    lastArray = array;
    OPERATION_RETURN(scope, array);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
