/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIteration.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinIDs.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "IteratorOperations.h"
#include "JSArrayIterator.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSCInlines.h"
#include "JSPropertyNameEnumeratorInlines.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

static_assert(sizeof(UCPURegister) == sizeof(uint64_t), "operationAOTEnumeratorNext returns a position in one register.");

static uint64_t endPositionOf(const JSPropertyNameEnumerator& enumerator)
{
    return static_cast<uint64_t>(enumerator.indexedLength()) + enumerator.sizeOfPropertyNames();
}

EnumeratorCursor cursorBefore(const JSPropertyNameEnumerator& enumerator, uint64_t position)
{
    ASSERT(position <= endPositionOf(enumerator));
    uint64_t indexedLength = enumerator.indexedLength();
    if (!position)
        return { 0, JSPropertyNameEnumerator::InitMode };
    if (position <= indexedLength)
        return { static_cast<uint32_t>(position - 1), JSPropertyNameEnumerator::IndexedMode };
    return { static_cast<uint32_t>(position - indexedLength - 1), JSPropertyNameEnumerator::OwnStructureMode };
}

uint64_t positionOf(const JSPropertyNameEnumerator& enumerator, EnumeratorCursor cursor)
{
    switch (cursor.mode) {
    case JSPropertyNameEnumerator::IndexedMode:
        return cursor.index;
    case JSPropertyNameEnumerator::OwnStructureMode:
    case JSPropertyNameEnumerator::GenericMode:
        return static_cast<uint64_t>(enumerator.indexedLength()) + cursor.index;
    case JSPropertyNameEnumerator::InitMode:
    case JSPropertyNameEnumerator::HasSeenOwnStructureModeStructureMismatch:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSPropertyNameEnumerator* enumeratorFor(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value.isUndefinedOrNull())
        return vm.emptyPropertyNameEnumerator();
    JSObject* object = value.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (Object* compiled = compiledObjectOrNull(object))
        RELEASE_AND_RETURN(scope, compiled->propertyNameEnumerator(globalObject));
    // The enumerator lists the own names from the structure that the object has when it is asked for.
    PropertyWalk::materializeLazyProperties(vm, object);
    RELEASE_AND_RETURN(scope, JSC::propertyNameEnumerator(globalObject, object));
}

EnumeratedKey findEnumeratedKey(JSGlobalObject* globalObject, JSPropertyNameEnumerator* enumerator, JSValue value, uint64_t position)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint64_t end = endPositionOf(*enumerator);
    if (position >= end)
        return { nullptr, end };
    JSObject* base = value.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    EnumeratorCursor cursor = cursorBefore(*enumerator, position);
    JSString* key = enumerator->computeNext(globalObject, base, cursor.index, cursor.mode);
    RETURN_IF_EXCEPTION(scope, { });
    if (!key)
        return { nullptr, end };
    return { key, positionOf(*enumerator, cursor) };
}

static IterationRecord getIterator(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (iterable.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createNotAnObjectError(globalObject, iterable));
        return { };
    }
    JSValue method = iterable.get(globalObject, vm.propertyNames->iteratorSymbol);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto validation = validateIterable(vm, iterable, method); validation != IterableValidationResult::Valid) [[unlikely]] {
        throwTypeError(globalObject, scope, getIteratorErrorMessage(validation, iterable));
        return { };
    }
    JSValue iterator = call(globalObject, method, JSC::getCallData(method), iterable, ArgList());
    RETURN_IF_EXCEPTION(scope, { });
    if (!iterator.isObject()) [[unlikely]] {
        throwException(globalObject, scope, createIteratorResultNotObjectError(globalObject));
        return { };
    }
    JSValue next = asObject(iterator)->get(globalObject, vm.propertyNames->next);
    RETURN_IF_EXCEPTION(scope, { });
    return { iterator, next };
}

static std::optional<JSValue> iteratorStepValue(JSGlobalObject* globalObject, IterationRecord record)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!record.nextMethod.isCallable()) [[unlikely]] {
        throwNotAFunctionError(globalObject, scope, record.nextMethod);
        return std::nullopt;
    }
    JSValue result = iteratorNextExported(globalObject, record);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (JSObject* object = asObject(result); object->structure() == globalObject->iteratorResultObjectStructure()) {
        if (object->getDirect(iteratorResultObjectDonePropertyOffset).toBoolean(globalObject))
            return std::nullopt;
        return object->getDirect(iteratorResultObjectValuePropertyOffset);
    }
    bool isDone = iteratorCompleteExported(globalObject, result);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (isDone)
        return std::nullopt;
    JSValue value = iteratorValue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return value;
}

static void closeIterator(JSGlobalObject* globalObject, JSValue iterator)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = iterator.get(globalObject, vm.propertyNames->returnKeyword);
    RETURN_IF_EXCEPTION(scope, void());
    if (method.isUndefinedOrNull())
        return;
    if (!method.isCallable()) [[unlikely]] {
        throwNotAFunctionError(globalObject, scope, method);
        return;
    }
    JSValue result = call(globalObject, method, JSC::getCallData(method), iterator, ArgList());
    RETURN_IF_EXCEPTION(scope, void());
    if (!result.isObject()) [[unlikely]]
        throwException(globalObject, scope, createIteratorResultNotObjectError(globalObject));
}

void destructureIterable(JSGlobalObject* globalObject, JSValue iterable, uint32_t count, EncodedJSValue* results)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<EncodedJSValue> elements = unsafeMakeSpan(results, count);
    for (EncodedJSValue& element : elements)
        element = JSValue::encode(jsUndefined());
    IterationRecord record = getIterator(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, void());
    for (EncodedJSValue& element : elements) {
        std::optional<JSValue> value = iteratorStepValue(globalObject, record);
        RETURN_IF_EXCEPTION(scope, void());
        if (!value)
            return;
        element = JSValue::encode(*value);
    }
    scope.release();
    closeIterator(globalObject, record.iterator);
}

void initializeIterationWords(Instance& instance)
{
    instance.iteration.emptyEnumerator = instance.vm->emptyPropertyNameEnumerator();
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetPropertyEnumerator, JSCell*, (Instance* instance, EncodedJSValue object))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetPropertyEnumerator);
    OPERATION_RETURN(scope, enumeratorFor(globalObject, JSValue::decode(object)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorNext, UGPRPair, (Instance* instance, JSCell* enumerator, EncodedJSValue object, uint64_t position))
{
    constexpr Entry entry = Entry::operationAOTEnumeratorNext;
    AOT_OPERATION_BEGIN(instance, entry);
    EnumeratedKey found = findEnumeratedKey(globalObject, cellArgument<JSPropertyNameEnumerator>(enumerator, entry), JSValue::decode(object), position);
    OPERATION_RETURN(scope, makeUGPRPair(std::bit_cast<UCPURegister>(found.key), found.position));
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorGetByVal, EncodedJSValue, (Instance* instance, EncodedJSValue object, EncodedJSValue property, JSCell*))
{
    constexpr Entry entry = Entry::operationAOTEnumeratorGetByVal;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferGCForAWhile deferGC(vm);
    JSValue key = JSValue::decode(property);
    Identifier identifier = cellArgument<JSString>(key.isCell() ? key.asCell() : nullptr, entry)->toIdentifier(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(objectArgument(object, entry)->get(globalObject, identifier)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDestructureIterable, void, (Instance* instance, EncodedJSValue iterable, uint32_t count, EncodedJSValue* results))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDestructureIterable);
    destructureIterable(globalObject, JSValue::decode(iterable), count, results);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCreateAsyncFromSyncIterator, JSCell*, (Instance* instance, JSCell* iterator, EncodedJSValue next))
{
    constexpr Entry entry = Entry::operationAOTCreateAsyncFromSyncIterator;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferTerminationForAWhile deferTermination(vm);
    JSObject* syncIterator = cellArgument<JSObject>(iterator, entry);
    JSValue nextMethod = JSValue::decode(next);
    IterationMode mode = IterationMode::Generic;
    if (auto* arrayIterator = dynamicDowncast<JSArrayIterator>(syncIterator); arrayIterator && nextMethod.isCell() && nextMethod.asCell() == instance->state->builtinCells->findObject(Builtins::ArrayIteratorPrototypeNext)) {
        JSValue iterated = arrayIterator->internalField(JSArrayIterator::Field::IteratedObject).get();
        if (iterated.isCell() && isJSArray(iterated.asCell())) {
            switch (arrayIterator->kind()) {
            case IterationKind::Keys:
                mode = IterationMode::FastArrayKeys;
                break;
            case IterationKind::Values:
                mode = IterationMode::FastArrayValues;
                break;
            case IterationKind::Entries:
                mode = IterationMode::FastArrayEntries;
                break;
            }
        }
    }
    RELEASE_AND_RETURN(scope, JSAsyncFromSyncIterator::create(vm, globalObject->asyncFromSyncIteratorStructure(), syncIterator, nextMethod, mode));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowNotIterable, void, (Instance* instance, EncodedJSValue encodedIterable, uint32_t isSpread))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowNotIterable);
    JSValue iterable = JSValue::decode(encodedIterable);
    if (isSpread)
        throwException(globalObject, scope, createSpreadIteratorNotFunctionError(globalObject));
    else
        throwTypeError(globalObject, scope, getIteratorErrorMessage(validateIterable(vm, iterable, jsUndefined()), iterable));
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
