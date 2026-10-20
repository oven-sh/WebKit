/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayAllocationProfile.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "ArrayPrototypeInlines.h"
#include "ButterflyInlines.h"
#include "JSArrayInlines.h"
#include "JSArrayIterator.h"
#include "JSCInlines.h"
#include "JSStringJoiner.h"
#include <algorithm>
#include <bit>
#include <span>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTArraySplice, EncodedJSValue, (Instance* instance, JSCell* cell, int32_t start, int32_t deleteCount, uint32_t count, const EncodedJSValue* items))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySplice);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySplice);
    if (!arraySpeciesWatchpointIsValid(vm, array)) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    int64_t length = array->length();
    int64_t from = start < 0 ? std::max<int64_t>(length + start, 0) : std::min<int64_t>(start, length);
    int64_t removed = std::clamp<int64_t>(deleteCount, 0, length - from);
    JSArray* result = JSArray::fastSlice(globalObject, array, from, removed);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!result)
        OPERATION_RETURN(scope, encodedJSValue());
    bool isDone = array->fastSplice(globalObject, length, from, removed, std::span { items, count });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, isDone ? JSValue::encode(result) : encodedJSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayFromTypedArray, JSCell*, (Instance* instance, JSCell* cell, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayFromTypedArray);
    auto* view = cellArgument<JSArrayBufferView>(cell, Entry::operationAOTArrayFromTypedArray);
    markPristineIfOriginal(*instance, view);
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAllocateStorage, void*, (Instance* instance, uint64_t size))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAllocateStorage);
    return vm.auxiliarySpace().allocate(vm, size, nullptr, AllocationFailureMode::ReturnNull);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTArrayGrow, size_t, (Instance* instance, JSCell* cell, uint32_t length))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayGrow);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayGrow);
    IndexingType mode = array->indexingMode();
    if (mode != ArrayWithInt32 && mode != ArrayWithDouble && mode != ArrayWithContiguous) [[unlikely]]
        return false;
    if (length > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
        return false;
    unsigned publicLength = array->butterfly()->publicLength();
    if (!array->ensureLength(vm, length)) [[unlikely]]
        return false;
    array->butterfly()->setPublicLength(publicLength);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayJoin, JSCell*, (Instance* instance, JSCell* cell, JSCell* separator))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayJoin);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayJoin);
    // The elements are read through a pointer into the butterfly while ropes are allocated, and the caller may hold the array no longer.
    EnsureStillAliveScope arrayScope(array);
    JSString* between = separator ? cellArgument<JSString>(separator, Entry::operationAOTArrayJoin) : vm.smallStrings.singleCharacterString(',');
    auto view = between->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSOnlyStringsAndInt32sJoiner joiner(view);
    Butterfly& butterfly = *array->butterfly();
    unsigned length = butterfly.publicLength();
    switch (array->indexingType()) {
    case ALL_INT32_INDEXING_TYPES:
        OPERATION_RETURN(scope, joiner.tryJoin<Int32Shape>(globalObject, butterfly.contiguous().data(), length));
    case ALL_DOUBLE_INDEXING_TYPES:
        OPERATION_RETURN(scope, joiner.tryJoin(globalObject, butterfly.contiguousDouble().data(), length));
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        auto* elements = butterfly.contiguous().data();
        uint64_t units = 0;
        bool areAllStrings = length >= 2;
        for (unsigned i = 0; i < length && areAllStrings; ++i) {
            JSValue element = elements[i].get();
            areAllStrings = element && element.isString();
            if (areAllStrings)
                units += asString(element)->length();
        }
        if (areAllStrings && units >= runtimeLimitValue(vm, LimitID::MinRopeJoinLength) && units / length >= runtimeLimitValue(vm, LimitID::MinRopeJoinUnitsPerElement)) {
            JSRopeString::RopeBuilder<RecordOverflow> builder(vm);
            for (unsigned i = 0; i < length; ++i) {
                if (i)
                    builder.append(between);
                builder.append(asString(elements[i].get()));
            }
            if (!builder.hasOverflowed())
                OPERATION_RETURN(scope, builder.release());
        }
        OPERATION_RETURN(scope, joiner.tryJoin<ContiguousShape>(globalObject, elements, length));
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTArrayIndexOfDouble, size_t, (Instance*, JSCell* array, double searched))
{
    Butterfly& butterfly = *cellArgument<JSArray>(array, Entry::operationAOTArrayIndexOfDouble)->butterfly();
    const double* data = butterfly.contiguousDouble().data();
    const double* found = WTF::findDouble(data, searched, butterfly.publicLength());
    return found ? found - data : static_cast<size_t>(-1);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTArrayIndexOfInt32, size_t, (Instance*, JSCell* array, EncodedJSValue searched))
{
    Butterfly& butterfly = *cellArgument<JSArray>(array, Entry::operationAOTArrayIndexOfInt32)->butterfly();
    auto* data = std::bit_cast<const uint64_t*>(butterfly.contiguous().data());
    const uint64_t* found = WTF::find64(data, searched, butterfly.publicLength());
    return found ? found - data : static_cast<size_t>(-1);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayUnshift, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue item))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayUnshift);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayUnshift);
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewArrayIterator, JSCell*, (Instance* instance, JSCell* array, uint32_t kind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayIterator);
    RELEASE_ASSERT(kind <= static_cast<uint32_t>(IterationKind::Entries));
    return JSArrayIterator::create(vm, globalObject->arrayIteratorStructure(), cellArgument<JSObject>(array, Entry::operationAOTNewArrayIterator), static_cast<IterationKind>(kind));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
