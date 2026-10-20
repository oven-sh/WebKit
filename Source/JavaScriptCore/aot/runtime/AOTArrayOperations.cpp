/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "ButterflyInlines.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include <bit>
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
    } else
        return false;
    return target->appendMemcpy(globalObject, vm, target->length(), source);
}

JSC_DEFINE_JIT_OPERATION(operationAOTAppendElements, void, (Instance* instance, JSCell* cell, JSCell* sourceCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAppendElements);
    JSArray* target = cellArgument<JSArray>(cell, Entry::operationAOTAppendElements);
    JSArray* source = cellArgument<JSArray>(sourceCell, Entry::operationAOTAppendElements);
    if (tryAppendArray(globalObject, vm, target, source))
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReserveArrayCapacity, void, (Instance* instance, JSCell* cell, uint64_t additional))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTReserveArrayCapacity);
    reserveArrayCapacity(vm, cellArgument<JSArray>(cell, Entry::operationAOTReserveArrayCapacity), additional);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArray, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArray);
    unsigned emptyCapacity = count >> capacityShiftInArrayLiteralCount;
    count &= (1u << capacityShiftInArrayLiteralCount) - 1;
    if (emptyCapacity && count) [[unlikely]]
        crashOnWrongOperand(Entry::operationAOTNewArray);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    unsigned vectorLength = emptyCapacity ? emptyCapacity : arrayLiteralVectorLength(count);
    if (hasContiguous(structure->indexingType()) && !structure->outOfLineCapacity() && vectorLength <= maxHoleArrayCapacity) {
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
    OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(indexingType), butterfly));
}

static EncodedJSValue pushToArray(JSGlobalObject* globalObject, JSArray* array, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    array->push(globalObject, value);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    return JSValue::encode(jsNumber(array->length()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPush, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayPush);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayPush);
    OPERATION_RETURN(scope, pushToArray(globalObject, array, JSValue::decode(value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPushNumber, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t numberBits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayPushNumber);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayPushNumber);
    double number = std::bit_cast<double>(numberBits);
    if (number == number)
        array->tryMakeWritableDouble(vm);
    OPERATION_RETURN(scope, pushToArray(globalObject, array, jsNumber(purifyNaN(number))));
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
    void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!storage)
        return nullptr;
    Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
    butterfly->setVectorLength(vectorLength);
    butterfly->setPublicLength(static_cast<unsigned>(count));
    const uint64_t* source = array->butterfly()->indexingPayload<uint64_t>() + from;
    uint64_t* target = butterfly->indexingPayload<uint64_t>();
    for (unsigned i = 0; i < vectorLength; ++i)
        target[i] = i < count ? source[i] : shape == DoubleShape ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
    JSArray* result = JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(static_cast<IndexingType>(IsArray | shape)), butterfly);
    ensureStillAliveHere(array);
    return result;
}

static JSValue callBuiltinSlice(Instance* instance, JSArray* array, const ArgList& arguments)
{
    JSCell* function = instance->state->builtinCells->findObject(Builtins::ArrayPrototypeSlice);
    RELEASE_ASSERT(function);
    auto callData = JSC::getCallData(function);
    RELEASE_ASSERT(callData.type != CallData::Type::None);
    return call(instance->globalObject, function, callData, array, arguments);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayCopy, EncodedJSValue, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayCopy);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayCopy);
    if (JSArray* copy = tryFastSlice(vm, globalObject, array, 0, std::numeric_limits<int64_t>::max()))
        OPERATION_RETURN(scope, JSValue::encode(copy));
    MarkedArgumentBuffer arguments;
    OPERATION_RETURN(scope, JSValue::encode(callBuiltinSlice(instance, array, arguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySlice, EncodedJSValue, (Instance* instance, JSCell* cell, int64_t from, int64_t to))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySlice);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySlice);
    if (JSArray* part = tryFastSlice(vm, globalObject, array, from, to))
        OPERATION_RETURN(scope, JSValue::encode(part));
    MarkedArgumentBuffer arguments;
    arguments.append(jsNumber(static_cast<double>(from)));
    arguments.append(jsNumber(static_cast<double>(to)));
    OPERATION_RETURN(scope, JSValue::encode(callBuiltinSlice(instance, array, arguments)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
