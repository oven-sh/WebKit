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
    for (uint32_t i = 0; i < count; ++i)
        indexingType = leastUpperBoundOfIndexingTypeAndValue(indexingType, valueAt(i));
    ObjectInitializationScope initializationScope(vm);
    JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, nullptr, globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    IndexingType arrayIndexingType = array->indexingType();
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
        return jsNumber(purifyNaN(std::bit_cast<double>(bits)));
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
        void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, capacity * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
        if (!storage) {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, nullptr);
        }
        Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
        butterfly->setVectorLength(capacity);
        butterfly->setPublicLength(count);
        for (unsigned i = 0; i < capacity; ++i)
            butterfly->contiguousDouble().atUnsafe(i) = i < count ? elements[i] : PNaN;
        OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(ArrayWithDouble), butterfly));
    }
    auto valueAt = [&](uint32_t i) {
        uint64_t bits = std::bit_cast<uint64_t>(elements[i]);
        bool isNumber = countAndNumberMask >> (arrayLiteralWithDoubleIsNumberMaskShift + i) & 1;
        return !isNumber && bits == nullDouble ? jsNull() : !isNumber && (bits == undefinedDouble || bits == absentDouble) ? jsUndefined() : jsNumber(purifyNaN(elements[i]));
    };
    OPERATION_RETURN(scope, createArrayFromValues(globalObject, scope, count, ArrayWithDouble, valueAt));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayWithHoles, JSCell*, (Instance* instance, JSCell* source, uint64_t length, uint32_t indexingType, uint32_t flags))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayWithHoles);
    if (length > UINT32_MAX) {
        throwException(globalObject, scope, createRangeError(globalObject, ArrayInvalidLengthError));
        OPERATION_RETURN(scope, nullptr);
    }
    bool mustHaveIndexingType = flags & NewArrayWithHolesFlag::mustHaveIndexingType;
    IndexingType sourceIndexingType = source ? source->indexingType() : NonArray;
    bool mustUseIndexingType = mustHaveIndexingType || hasInt32(sourceIndexingType) || hasDouble(sourceIndexingType) || hasContiguous(sourceIndexingType);
    if ((mustUseIndexingType || length < MIN_ARRAY_STORAGE_CONSTRUCTION_LENGTH) && (mustHaveIndexingType || !globalObject->isHavingABadTime())) {
        if (JSArray* array = JSArray::tryCreate(vm, globalObject->originalArrayStructureForIndexingType(indexingType), length))
            OPERATION_RETURN(scope, array);
        if (mustUseIndexingType) {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, nullptr);
        }
    }
    OPERATION_RETURN(scope, constructEmptyArray(globalObject, nullptr, flags & NewArrayWithHolesFlag::lengthIsCapacity ? 0 : static_cast<uint32_t>(length)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGrowNewArray, void, (Instance* instance, JSCell* array, uint64_t length))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGrowNewArray);
    if (length <= MAX_STORAGE_VECTOR_LENGTH / 2)
        cellArgument<JSArray>(array, Entry::operationAOTGrowNewArray)->setLength(globalObject, std::max<uint32_t>(length * 2, 8), true);
    OPERATION_RETURN(scope);
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
