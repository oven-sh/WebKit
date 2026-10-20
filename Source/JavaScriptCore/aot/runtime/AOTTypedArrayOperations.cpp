/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "DeferGC.h"
#include "DeferTermination.h"
#include "Error.h"
#include "JSArrayBufferViewInlines.h"
#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSTypedArrays.h"

namespace JSC { namespace AOT {

template<typename View>
static JSCell* newTypedArrayView(Instance* instance, JSGlobalObject* globalObject, size_t length)
{
    static_assert(View::TypedArrayStorageType < AllocationWords::numberOfTypedArrayAllocations && sizeof(View) == sizeof(JSArrayBufferView));
    VM& vm = *instance->vm;
    Structure* structure = globalObject->typedArrayStructure(View::TypedArrayStorageType, false);
    auto& allocation = instance->allocation.typedArrayAllocations[View::TypedArrayStorageType];
    structure->tryMarkPristine(vm);
    allocation.header = cellHeaderFor(structure);
    if (!allocation.allocator) {
        allocation.elementSize = View::elementSize;
        allocation.allocator = subspaceFor<View>(vm)->allocatorFor(sizeof(View), AllocatorForMode::EnsureAllocator).localAllocator();
    }
    return View::create(globalObject, structure, length);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewTypedArray, JSCell*, (Instance* instance, uint32_t type, double length))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewTypedArray);
    double truncated = std::isnan(length) ? 0 : std::trunc(length);
    if (truncated < 0 || truncated > MAX_ARRAY_BUFFER_SIZE) {
        throwRangeError(globalObject, scope, "Length out of range of buffer"_s);
        OPERATION_RETURN(scope, nullptr);
    }
    size_t elementCount = static_cast<size_t>(truncated);
    switch (static_cast<TypedArrayType>(type)) {
#define AOT_NEW_TYPED_ARRAY_VIEW(name) \
    case Type ## name: \
        OPERATION_RETURN(scope, newTypedArrayView<JS ## name ## Array>(instance, globalObject, elementCount));
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_NEW_TYPED_ARRAY_VIEW)
#undef AOT_NEW_TYPED_ARRAY_VIEW
    case NotTypedArray:
    case TypeDataView:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayPut, void, (Instance* instance, JSCell* cell, uint64_t index, double number))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTypedArrayPut);
    DeferTerminationForAWhile deferTermination(vm);
    auto* view = cellArgument<JSArrayBufferView>(cell, Entry::operationAOTTypedArrayPut);
    JSType type = view->type();
    RELEASE_ASSERT(type != BigInt64ArrayType && type != BigUint64ArrayType);
    JSValue value(JSValue::EncodeAsDouble, purifyNaN(number));
    switch (type) {
#define AOT_PUT_ELEMENT(name) \
    case name ## ArrayType: \
        uncheckedDowncast<JS ## name ## Array>(view)->setIndex(globalObject, index, value); \
        return;
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_PUT_ELEMENT)
#undef AOT_PUT_ELEMENT
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowIfTypedArrayIsDetached, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowIfTypedArrayIsDetached);
    DeferGCForAWhile deferGC(vm);
    auto* view = cellArgument<JSArrayBufferView>(cell, Entry::operationAOTThrowIfTypedArrayIsDetached);
    if (view->isDetached() || view->isOutOfBounds())
        throwTypeError(globalObject, scope, typedArrayBufferHasBeenDetachedErrorMessage);
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
