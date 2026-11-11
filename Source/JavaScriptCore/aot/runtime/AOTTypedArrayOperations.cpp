/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTBuiltinTable.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRealmStartup.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "DeferGC.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "JSArrayBufferViewInlines.h"
#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSGenericTypedArrayViewPrototypeFunctions.h"
#include "JSTypedArrays.h"
#include "ObjectConstructor.h"
#include "StructureCache.h"
#include <wtf/SIMDUTF.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringCommon.h>

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
    size_t elementCount = jsNumber(purifyNaN(length)).toIndex(globalObject, "length"_s);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
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

static BufferStructureProvider bufferStructureProvider;

void setBufferStructureProvider(BufferStructureProvider provider)
{
    bufferStructureProvider = provider;
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyByteLiteral, JSCell*, (Instance* instance, const uint8_t* bytes, uint32_t size, uint32_t isBuffer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyByteLiteral);
    RELEASE_ASSERT_WITH_MESSAGE(!isBuffer || bufferStructureProvider, "The host encodes literals for Buffer and has not called setBufferStructureProvider().");
    Structure* structure = isBuffer ? bufferStructureProvider(globalObject) : globalObject->typedArrayStructure(TypeUint8, false);
    JSUint8Array* array = JSUint8Array::createUninitialized(globalObject, structure, size);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (size)
        memcpy(array->typedVector(), bytes, size);
    OPERATION_RETURN(scope, array);
}

static ALWAYS_INLINE bool isFixed(const JSArrayBufferView* view)
{
    return !isResizableOrGrowableShared(view->mode()) && view->vector();
}

static ALWAYS_INLINE JSUint8Array* asFixedBytes(JSValue value)
{
    if (!value.isCell() || value.asCell()->type() != Uint8ArrayType)
        return nullptr;
    auto* view = uncheckedDowncast<JSUint8Array>(value.asCell());
    return isFixed(view) ? view : nullptr;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySetOfAnyValue, EncodedJSValue, (Instance*, EncodedJSValue encodedReceiver, EncodedJSValue encodedSource, EncodedJSValue encodedOffset))
{
    JSValue receiver = JSValue::decode(encodedReceiver);
    JSValue given = JSValue::decode(encodedSource);
    JSValue offset = JSValue::decode(encodedOffset);
    bool isPlace = offset.isUndefined() || (offset.isInt32() ? offset.asInt32() >= 0 : offset.isDouble() && offset.asDouble() > -1 && offset.asDouble() <= static_cast<double>(MAX_ARRAY_BUFFER_SIZE));
    if (receiver.isCell() && given.isCell() && isTypedArrayType(receiver.asCell()->type()) && receiver.asCell()->type() == given.asCell()->type() && isPlace) [[likely]] {
        auto* target = uncheckedDowncast<JSArrayBufferView>(receiver.asCell());
        auto* source = uncheckedDowncast<JSArrayBufferView>(given.asCell());
        size_t position = offset.isUndefined() ? 0 : offset.isInt32() ? offset.asInt32() : static_cast<size_t>(offset.asDouble());
        size_t count = source->lengthRaw();
        size_t room = target->lengthRaw();
        if (isFixed(target) && isFixed(source) && position <= room && count <= room - position) [[likely]] {
            unsigned shift = logElementSize(target->type());
            memmove(static_cast<uint8_t*>(target->vector()) + (position << shift), source->vector(), count << shift);
            return { JSValue::encode(jsUndefined()), nullptr };
        }
    }
    return { encodedJSValue(), nullptr };
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySubarrayOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedReceiver, EncodedJSValue encodedBegin, EncodedJSValue encodedEnd))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTypedArraySubarrayOfAnyValue);
    JSValue begin = JSValue::decode(encodedBegin);
    JSValue end = JSValue::decode(encodedEnd);
    JSUint8Array* view = asFixedBytes(JSValue::decode(encodedReceiver));
    bool makesPlainView = view && (begin.isInt32() || begin.isUndefined()) && (end.isInt32() || end.isUndefined()) && view->realm() == globalObject && speciesWatchpointIsValid(globalObject, view);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (makesPlainView) [[likely]] {
        int64_t length = view->lengthRaw();
        auto placeOf = [&](JSValue position, int64_t otherwise) {
            if (position.isUndefined())
                return otherwise;
            int64_t place = position.asInt32();
            return place < 0 ? std::max<int64_t>(length + place, 0) : std::min(place, length);
        };
        int64_t first = placeOf(begin, 0);
        int64_t last = std::max(first, placeOf(end, length));
        if (RefPtr<ArrayBuffer> buffer = view->possiblySharedBuffer()) [[likely]] {
            Structure* structure = globalObject->typedArrayStructure(TypeUint8, false);
            OPERATION_RETURN(scope, JSValue::encode(JSUint8Array::create(globalObject, structure, WTF::move(buffer), view->byteOffsetRaw() + first, static_cast<size_t>(last - first))));
        }
    }
    OPERATION_RETURN(scope, encodedJSValue());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMoveBytes, void, (uint8_t* target, const uint8_t* source, uint64_t count))
{
    memmove(target, source, count);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayLength, uint64_t, (JSCell* view))
{
    return cellArgument<JSArrayBufferView>(view, Entry::operationAOTTypedArrayLength)->length();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayPut, void, (Instance* instance, JSCell* cell, uint64_t index, double number))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTTypedArrayPut);
    DeferTerminationForAWhile deferTermination(vm);
    auto* view = cellArgument<JSArrayBufferView>(cell, Entry::operationAOTTypedArrayPut);
    JSType type = view->type();
    RELEASE_ASSERT(type != BigInt64ArrayType && type != BigUint64ArrayType);
    switch (type) {
#define AOT_PUT_ELEMENT(name) \
    case name ## ArrayType: \
        if (auto* elements = uncheckedDowncast<JS ## name ## Array>(view); !elements->isDetached() && elements->inBounds(index)) \
            elements->setIndexQuicklyToNativeValue(index, name ## Adaptor::toNativeFromDouble(number)); \
        return;
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_PUT_ELEMENT)
#undef AOT_PUT_ELEMENT
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowIfTypedArrayIsDetached, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTThrowIfTypedArrayIsDetached);
    auto* view = cellArgument<JSArrayBufferView>(cell, Entry::operationAOTThrowIfTypedArrayIsDetached);
    if (view->isDetached() || view->isOutOfBounds())
        throwTypeError(globalObject, scope, typedArrayBufferHasBeenDetachedErrorMessage);
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
