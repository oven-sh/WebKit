/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesAnswers.h"
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
#include <span>

namespace JSC { namespace AOT {

static_assert(sizeof(UCPURegister) == sizeof(uint64_t), "operationAOTEnumeratorNext returns a position in one register.");

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
    // No enumerator is kept for a dictionary.
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenForListing(vm, object);
    RELEASE_AND_RETURN(scope, JSC::propertyNameEnumerator(globalObject, object));
}

EnumeratedKey findEnumeratedKey(JSGlobalObject* globalObject, JSPropertyNameEnumerator* enumerator, JSValue value, uint64_t position)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint64_t indexedLength = enumerator->indexedLength();
    uint64_t end = indexedLength + enumerator->sizeOfPropertyNames();
    if (position >= end)
        return { nullptr, end };
    JSObject* base = value.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    uint32_t index = 0;
    JSPropertyNameEnumerator::Flag mode = JSPropertyNameEnumerator::InitMode;
    if (position > indexedLength) {
        index = static_cast<uint32_t>(position - indexedLength - 1);
        mode = JSPropertyNameEnumerator::OwnStructureMode;
    } else if (position) {
        index = static_cast<uint32_t>(position - 1);
        mode = JSPropertyNameEnumerator::IndexedMode;
    }
    JSString* key = enumerator->computeNext(globalObject, base, index, mode);
    RETURN_IF_EXCEPTION(scope, { });
    if (!key)
        return { nullptr, end };
    RELEASE_ASSERT(mode == JSPropertyNameEnumerator::IndexedMode || mode == JSPropertyNameEnumerator::OwnStructureMode || mode == JSPropertyNameEnumerator::GenericMode);
    return { key, mode == JSPropertyNameEnumerator::IndexedMode ? index : indexedLength + index };
}

static void destructureIterable(JSGlobalObject* globalObject, JSValue iterable, std::span<EncodedJSValue> elements)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (EncodedJSValue& element : elements)
        element = JSValue::encode(jsUndefined());
    JSValue method = iterable.get(globalObject, vm.propertyNames->iteratorSymbol);
    RETURN_IF_EXCEPTION(scope, void());
    if (auto validation = validateIterable(vm, iterable, method); validation != IterableValidationResult::Valid) [[unlikely]] {
        throwTypeError(globalObject, scope, getIteratorErrorMessage(validation, iterable));
        return;
    }
    JSValue iterator = call(globalObject, method, JSC::getCallData(method), iterable, ArgList());
    RETURN_IF_EXCEPTION(scope, void());
    if (!iterator.isObject()) [[unlikely]] {
        throwException(globalObject, scope, createIteratorResultNotObjectError(globalObject));
        return;
    }
    JSValue next = asObject(iterator)->get(globalObject, vm.propertyNames->next);
    RETURN_IF_EXCEPTION(scope, void());
    for (EncodedJSValue& element : elements) {
        if (!next.isCallable()) [[unlikely]] {
            throwNotAFunctionError(globalObject, scope, next);
            return;
        }
        JSValue result = iteratorStep(globalObject, { iterator, next });
        RETURN_IF_EXCEPTION(scope, void());
        if (result.isFalse())
            return;
        JSValue value = iteratorValue(globalObject, result);
        RETURN_IF_EXCEPTION(scope, void());
        element = JSValue::encode(value);
    }
    JSValue close = iterator.get(globalObject, vm.propertyNames->returnKeyword);
    RETURN_IF_EXCEPTION(scope, void());
    if (close.isUndefinedOrNull())
        return;
    if (!close.isCallable()) [[unlikely]] {
        throwNotAFunctionError(globalObject, scope, close);
        return;
    }
    JSValue result = call(globalObject, close, JSC::getCallData(close), iterator, ArgList());
    RETURN_IF_EXCEPTION(scope, void());
    if (!result.isObject()) [[unlikely]]
        throwException(globalObject, scope, createIteratorResultNotObjectError(globalObject));
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
    destructureIterable(globalObject, JSValue::decode(iterable), unsafeMakeSpan(results, count));
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

// What the engine's spread lists in one piece: a text, or a view whose iteration nothing has replaced. A text of 16-bit units makes one element of a surrogate pair.
JSC_DEFINE_JIT_OPERATION(operationAOTThrowIfTooLongForList, void, (Instance* instance, JSCell* source))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowIfTooLongForList);
    size_t count = 0;
    if (auto* view = dynamicDowncast<JSArrayBufferView>(source)) {
        if (!view->isOutOfBounds() && view->isIteratorProtocolFastAndNonObservable())
            count = view->length();
    } else if (globalObject->isStringPrototypeIteratorProtocolFastAndNonObservable()) {
        JSString* string = asString(source);
        count = string->length();
        if (count > IndexingHeader::maximumLength && !string->is8Bit()) {
            auto text = string->view(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope);
            std::span<const char16_t> units = text->span16();
            for (size_t i = 0; i + 1 < units.size(); ++i) {
                if (U16_IS_LEAD(units[i]) && U16_IS_TRAIL(units[i + 1])) {
                    --count;
                    ++i;
                }
            }
        }
    }
    if (count > IndexingHeader::maximumLength)
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
