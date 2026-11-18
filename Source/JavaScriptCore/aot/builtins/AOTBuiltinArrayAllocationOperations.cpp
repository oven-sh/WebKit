/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayAllocationProfile.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlotRepresentation.h"
#include "AOTUnwinding.h"
#include "ArrayConstructor.h"
#include "IndexingTypeInlines.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include <algorithm>
#include <bit>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static JSArray* createArrayFromValues(JSGlobalObject* globalObject, ThrowScope& scope, uint32_t count, IndexingType indexingType, const Invocable<JSValue(uint32_t)> auto& valueAt)
{
    VM& vm = globalObject->vm();
    for (uint32_t i = 0; i < count && !hasContiguous(indexingType); ++i)
        indexingType = leastUpperBoundOfIndexingTypeAndValue(indexingType, valueAt(i));
    ObjectInitializationScope initializationScope(vm);
    JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, nullptr, globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    IndexingType arrayIndexingType = array->indexingType();
    if (hasContiguous(arrayIndexingType)) {
        auto elements = array->butterfly()->contiguous();
        for (uint32_t i = 0; i < count; ++i)
            elements.atUnsafe(i).setWithoutWriteBarrier(valueAt(i));
        vm.writeBarrier(array);
        return array;
    }
    for (uint32_t i = 0; i < count; ++i)
        array->initializeIndex(initializationScope, i, valueAt(i), arrayIndexingType);
    return array;
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayWithIndexingType, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* values, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayWithIndexingType);
    bool areRaw = indexingType & valuesAreRawNumbers;
    indexingType &= ~valuesAreRawNumbers;
    auto valueAt = [&](uint32_t i) {
        if (!areRaw)
            return JSValue::decode(values[i]);
        uint64_t bits = static_cast<uint64_t>(values[i]);
        if (bits == nullDouble)
            return jsNull();
        if (bits == undefinedDouble || bits == absentDouble)
            return jsUndefined();
        return jsNumber(withoutPayload(std::bit_cast<double>(bits)));
    };
    OPERATION_RETURN(scope, createArrayFromValues(globalObject, scope, count, indexingType, valueAt));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayFromDoubles, JSCell*, (Instance* instance, uint32_t countAndNumberMask, double first, double second, double third, double fourth))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayFromDoubles);
    double elements[] = { first, second, third, fourth };
    unsigned count = countAndNumberMask & ((1u << arrayLiteralWithDoubleIsNumberMaskShift) - 1);
    RELEASE_ASSERT(count <= std::size(elements));
    bool fit = true;
    for (unsigned i = 0; i < count; ++i)
        fit &= elements[i] == elements[i];
    if (fit && !globalObject->isHavingABadTime()) {
        unsigned capacity = arrayLiteralVectorLength(count);
        JSArray* array = tryCreateArrayWithStorage(vm, globalObject->originalArrayStructureForIndexingType(ArrayWithDouble), count, capacity);
        if (!array) {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, nullptr);
        }
        for (unsigned i = 0; i < count; ++i)
            array->butterfly()->contiguousDouble().atUnsafe(i) = elements[i];
        instance->findMissingShortArrayAllocator(capacity);
        OPERATION_RETURN(scope, array);
    }
    auto valueAt = [&](uint32_t i) {
        uint64_t bits = std::bit_cast<uint64_t>(elements[i]);
        bool isNumber = countAndNumberMask >> (arrayLiteralWithDoubleIsNumberMaskShift + i) & 1;
        return !isNumber && bits == nullDouble ? jsNull() : !isNumber && (bits == undefinedDouble || bits == absentDouble) ? jsUndefined() : jsNumber(withoutPayload(elements[i]));
    };
    OPERATION_RETURN(scope, createArrayFromValues(globalObject, scope, count, ArrayWithDouble, valueAt));
}

template<typename Scope>
static JSArray* bearArrayWithHoles(JSGlobalObject* globalObject, VM& vm, Scope& scope, JSCell* source, uint64_t length, uint32_t indexingType, uint32_t flags)
{
    if (length > UINT32_MAX) {
        throwException(globalObject, scope, createRangeError(globalObject, ArrayInvalidLengthError));
        return nullptr;
    }
    bool mustHaveIndexingType = flags & NewArrayWithHolesFlag::mustHaveIndexingType;
    IndexingType sourceIndexingType = source ? source->indexingType() : NonArray;
    bool mustUseIndexingType = mustHaveIndexingType || hasInt32(sourceIndexingType) || hasDouble(sourceIndexingType) || hasContiguous(sourceIndexingType);
    if ((mustUseIndexingType || length < MIN_ARRAY_STORAGE_CONSTRUCTION_LENGTH) && (mustHaveIndexingType || !globalObject->isHavingABadTime())) {
        Structure* structure = globalObject->originalArrayStructureForIndexingType(indexingType);
        bool holdsPlainVector = hasInt32(structure->indexingType()) || hasDouble(structure->indexingType()) || hasContiguous(structure->indexingType());
        if (JSArray* array = holdsPlainVector ? tryCreateArrayWithStorage(vm, structure, length, static_cast<unsigned>(length) | 1) : nullptr)
            return array;
        if (JSArray* array = JSArray::tryCreate(vm, structure, length))
            return array;
        if (mustUseIndexingType) {
            throwOutOfMemoryError(globalObject, scope);
            return nullptr;
        }
    }
    return constructEmptyArray(globalObject, nullptr, static_cast<uint32_t>(length));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSortKeyArray, JSCell*, (Instance* instance, uint64_t length))
{
    static constexpr uint64_t maxLengthOfKeptArray = 32;
    auto* kept = static_cast<JSArray*>(instance->arrays.sortKeys);
    if (kept && length <= maxLengthOfKeptArray && kept->indexingMode() == ArrayWithContiguous) [[likely]]
        return { kept, nullptr };
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSortKeyArray);
    JSArray* made = bearArrayWithHoles(globalObject, vm, scope, nullptr, std::max(length, maxLengthOfKeptArray), ArrayWithContiguous, NewArrayWithHolesFlag::mustHaveIndexingType);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (length <= maxLengthOfKeptArray)
        instance->arrays.sortKeys = made;
    OPERATION_RETURN(scope, made);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayWithHoles, JSCell*, (Instance* instance, JSCell* source, uint64_t length, uint32_t indexingType, uint32_t flags))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayWithHoles);
    OPERATION_RETURN(scope, bearArrayWithHoles(globalObject, vm, scope, source, length, indexingType, flags));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayWithHolesAtSite, JSCell*, (Instance* instance, void* profileAddress, JSCell* source, uint64_t length, uint32_t seedIndexingType))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayWithHolesAtSite);
    uint8_t* statics = instance->statics();
    uint8_t* address = static_cast<uint8_t*>(profileAddress);
    uint64_t distanceToLastArray = instance->arrays.distanceFromProfileToLastArray;
    RELEASE_ASSERT(address >= statics && distanceToLastArray >= sizeof(ArrayAllocationProfile));
    RELEASE_ASSERT(address + distanceToLastArray + ArrayAllocationProfile::sizeOfLastArrayWord <= statics + instance->tables().instanceSizes.sizeOfStatics);
    RELEASE_ASSERT(seedIndexingType == ArrayWithUndecided || seedIndexingType == ArrayWithInt32 || seedIndexingType == ArrayWithDouble);
    auto& profile = *reinterpret_cast<ArrayAllocationProfile*>(address);
    auto& lastArray = *reinterpret_cast<JSArray**>(address + distanceToLastArray);

    bool isReviewed = !(profile.word & ArrayAllocationProfile::countMask);
    profile.word -= ArrayAllocationProfile::oneAllocation;
    if (isReviewed) {
        reviewArrayAllocationProfile(profile, lastArray);
        profile.startInterval();
        lastArray = nullptr;
    }
    IndexingType shape = std::max<IndexingType>(profile.shape(), seedIndexingType & IndexingShapeMask);
    if (shape == UndecidedShape)
        shape = Int32Shape;
    JSArray* array = bearArrayWithHoles(globalObject, vm, scope, source, length, IsArray | shape, 0);
    if (isReviewed && array)
        lastArray = array;
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReviewArraySite, void, (Instance* instance, void* profileAddress, JSCell* array))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTReviewArraySite);
    uint8_t* statics = instance->statics();
    uint8_t* address = static_cast<uint8_t*>(profileAddress);
    uint64_t distanceToLastArray = instance->arrays.distanceFromProfileToLastArray;
    RELEASE_ASSERT(address >= statics && distanceToLastArray >= sizeof(ArrayAllocationProfile));
    RELEASE_ASSERT(address + distanceToLastArray + ArrayAllocationProfile::sizeOfLastArrayWord <= statics + instance->tables().instanceSizes.sizeOfStatics);
    auto& profile = *reinterpret_cast<ArrayAllocationProfile*>(address);
    auto& lastArray = *reinterpret_cast<JSArray**>(address + distanceToLastArray);
    ASSERT(profile.isDueForReview());
    reviewArrayAllocationProfile(profile, lastArray);
    profile.startInterval();
    lastArray = cellArgument<JSArray>(array, Entry::operationAOTReviewArraySite);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayOfGiven, JSCell*, (Instance* instance, EncodedJSValue encodedGiven))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayOfGiven);
    JSValue given = JSValue::decode(encodedGiven);
    if (std::optional<uint32_t> length = given.tryGetAsUint32Index(); length && *length < MIN_ARRAY_STORAGE_CONSTRUCTION_LENGTH && !globalObject->isHavingABadTime()) {
        Structure* structure = globalObject->originalArrayStructureForIndexingType(ArrayWithInt32);
        if (JSArray* array = tryCreateArrayWithStorage(vm, structure, *length, *length | 1))
            OPERATION_RETURN(scope, array);
        if (JSArray* array = JSArray::tryCreate(vm, structure, *length))
            OPERATION_RETURN(scope, array);
    }
    OPERATION_RETURN(scope, constructArrayWithSizeQuirk(globalObject, nullptr, given));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTruncateNewArray, JSCell*, (Instance* instance, JSCell* cell, uint32_t length))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTruncateNewArray);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTTruncateNewArray);
    if (globalObject->isHavingABadTime() && !hasSlowPutArrayStorage(array->indexingType()))
        array->switchToSlowPutArrayStorage(vm);
    if (length < array->length()) {
        if (JSArray* copy = JSArray::fastSlice(globalObject, array, 0, length))
            OPERATION_RETURN(scope, copy);
    }
    array->setLength(globalObject, length, true);
    OPERATION_RETURN(scope, array);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
