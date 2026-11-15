/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinIDs.h"
#include "AOTErrorMessages.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTSavedFrame.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "IteratorOperations.h"
#include "JSArrayIterator.h"
#include "JSArrayIteratorInlines.h"
#include "JSCInlines.h"
#include "JSMapIterator.h"
#include "JSPropertyNameEnumerator.h"
#include "JSSetIterator.h"
#include "JSStringIteratorInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

enum class WordOfStraightIteration : uint32_t { Iterator, Next, IsDone, Stepper };
static constexpr size_t numberOfWordsOfStraightIteration = 4;
enum class StepperOfStraightIteration : uint8_t { Protocol, List, Map, Set, Text };
enum class WordOfStraightEnumeration : uint32_t { Enumerator, Position };
static constexpr size_t numberOfWordsOfStraightEnumeration = 2;

enum class ResultOfReturnInStraightIteration : uint8_t { MustBeObject, IsIgnored };

template<typename Word>
static uint64_t& wordOfStraightState(SavedFrame* state, Word word)
{
    return reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(state) + SavedFrame::offsetOfWords())[static_cast<uint32_t>(word)];
}

template<typename Word>
static JSValue readWordOfStraightState(SavedFrame* state, Word word)
{
    return JSValue::decode(static_cast<EncodedJSValue>(wordOfStraightState(state, word)));
}

template<typename Word>
static void storeWordOfStraightState(SavedFrame* state, Word word, JSValue value)
{
    wordOfStraightState(state, word) = static_cast<uint64_t>(JSValue::encode(value));
}

static UGPRPair resultOfStraightStep(bool isDone, JSValue value)
{
    return makeUGPRPair(JSValue::encode(jsBoolean(isDone)), JSValue::encode(value));
}

static JSValue callMethodOfStraightIteration(JSGlobalObject* globalObject, JSValue method, JSValue receiver)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto callData = JSC::getCallData(method);
    if (callData.type == CallData::Type::None) [[unlikely]] {
        throwNotAFunctionError(globalObject, scope, method);
        return { };
    }
    RELEASE_AND_RETURN(scope, call(globalObject, method, callData, receiver, ArgList()));
}

static void closeStraightIterationUnlessDone(JSGlobalObject* globalObject, SavedFrame* state, ResultOfReturnInStraightIteration expectation)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool wasDone = readWordOfStraightState(state, WordOfStraightIteration::IsDone).isTrue();
    storeWordOfStraightState(state, WordOfStraightIteration::IsDone, jsBoolean(true));
    if (wasDone)
        return;
    JSValue iterator = readWordOfStraightState(state, WordOfStraightIteration::Iterator);
    JSValue method = iterator.get(globalObject, vm.propertyNames->returnKeyword);
    RETURN_IF_EXCEPTION(scope, void());
    if (method.isUndefinedOrNull())
        return;
    JSValue result = callMethodOfStraightIteration(globalObject, method, iterator);
    RETURN_IF_EXCEPTION(scope, void());
    if (expectation == ResultOfReturnInStraightIteration::MustBeObject && !result.isObject()) [[unlikely]]
        throwException(globalObject, scope, createRuntimeError(globalObject, Typed::RuntimeErrorKind::IteratorResultNotObject, nullptr));
}

static StepperOfStraightIteration stepperOfStraightIteration(Instance& instance, JSObject* iterator, JSValue next)
{
    auto steppedByItsOwn = [&](BuiltinID method, StepperOfStraightIteration stepper) {
        return next.isCell() && next.asCell() == instance.state->builtinCells->findObject(method) ? stepper : StepperOfStraightIteration::Protocol;
    };
    switch (iterator->type()) {
    case JSArrayIteratorType:
        return isJSArray(uncheckedDowncast<JSArrayIterator>(iterator)->iteratedObject()) ? steppedByItsOwn(Builtins::ArrayIteratorPrototypeNext, StepperOfStraightIteration::List) : StepperOfStraightIteration::Protocol;
    case JSMapIteratorType:
        return steppedByItsOwn(Builtins::MapIteratorPrototypeNext, StepperOfStraightIteration::Map);
    case JSSetIteratorType:
        return steppedByItsOwn(Builtins::SetIteratorPrototypeNext, StepperOfStraightIteration::Set);
    case JSStringIteratorType:
        return steppedByItsOwn(Builtins::StringIteratorPrototypeNext, StepperOfStraightIteration::Text);
    default:
        return StepperOfStraightIteration::Protocol;
    }
}

static bool stepIteratorOfEngine(JSGlobalObject* globalObject, StepperOfStraightIteration stepper, JSCell* iterator, JSValue& value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    switch (stepper) {
    case StepperOfStraightIteration::List:
        RELEASE_AND_RETURN(scope, uncheckedDowncast<JSArrayIterator>(iterator)->next(globalObject, value));
    case StepperOfStraightIteration::Map:
        RELEASE_AND_RETURN(scope, uncheckedDowncast<JSMapIterator>(iterator)->next(globalObject, value));
    case StepperOfStraightIteration::Set:
        RELEASE_AND_RETURN(scope, uncheckedDowncast<JSSetIterator>(iterator)->next(globalObject, value));
    case StepperOfStraightIteration::Text: {
        JSString* piece = uncheckedDowncast<JSStringIterator>(iterator)->nextWithAdvance(globalObject, vm);
        RETURN_IF_EXCEPTION(scope, false);
        value = piece;
        return piece;
    }
    case StepperOfStraightIteration::Protocol:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTOpenIteration, JSCell*, (Instance* instance, EncodedJSValue encodedIterable))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTOpenIteration);
    JSValue iterable = JSValue::decode(encodedIterable);
    if (iterable.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createIterationOfNullishError(globalObject, iterable));
        OPERATION_RETURN(scope, nullptr);
    }
    JSValue method = iterable.get(globalObject, vm.propertyNames->iteratorSymbol);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (!method.isCallable()) [[unlikely]] {
        throwTypeError(globalObject, scope, getIteratorErrorMessage(validateIterable(vm, iterable, jsUndefined()), iterable));
        OPERATION_RETURN(scope, nullptr);
    }
    JSValue iterator = callMethodOfStraightIteration(globalObject, method, iterable);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (!iterator.isObject()) [[unlikely]] {
        throwException(globalObject, scope, createRuntimeError(globalObject, Typed::RuntimeErrorKind::IteratorNotObject, nullptr));
        OPERATION_RETURN(scope, nullptr);
    }
    JSValue next = asObject(iterator)->get(globalObject, vm.propertyNames->next);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    StepperOfStraightIteration stepper = stepperOfStraightIteration(*instance, asObject(iterator), next);
    SavedFrame* state = SavedFrame::createHeapFrame(vm, *instance, numberOfWordsOfStraightIteration, nullptr);
    storeWordOfStraightState(state, WordOfStraightIteration::Iterator, iterator);
    storeWordOfStraightState(state, WordOfStraightIteration::Next, next);
    storeWordOfStraightState(state, WordOfStraightIteration::Stepper, jsNumber(static_cast<int32_t>(stepper)));
    storeWordOfStraightState(state, WordOfStraightIteration::IsDone, jsBoolean(false));
    vm.mutatorFence();
    OPERATION_RETURN(scope, state);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStepIteration, UGPRPair, (Instance* instance, JSCell* cell))
{
    constexpr Entry entry = Entry::operationAOTStepIteration;
    AOT_OPERATION_BEGIN(instance, entry);
    SavedFrame* state = cellArgument<SavedFrame>(cell, entry);
    if (readWordOfStraightState(state, WordOfStraightIteration::IsDone).isTrue())
        OPERATION_RETURN(scope, resultOfStraightStep(true, jsUndefined()));
    storeWordOfStraightState(state, WordOfStraightIteration::IsDone, jsBoolean(true));
    JSValue iterator = readWordOfStraightState(state, WordOfStraightIteration::Iterator);
    auto stepper = static_cast<StepperOfStraightIteration>(readWordOfStraightState(state, WordOfStraightIteration::Stepper).asInt32());
    JSValue value;
    if (stepper != StepperOfStraightIteration::Protocol) {
        bool hasValue = stepIteratorOfEngine(globalObject, stepper, iterator.asCell(), value);
        OPERATION_RETURN_IF_EXCEPTION(scope, makeUGPRPair(0, 0));
        if (!hasValue)
            OPERATION_RETURN(scope, resultOfStraightStep(true, jsUndefined()));
    } else {
        JSValue result = callMethodOfStraightIteration(globalObject, readWordOfStraightState(state, WordOfStraightIteration::Next), iterator);
        OPERATION_RETURN_IF_EXCEPTION(scope, makeUGPRPair(0, 0));
        if (!result.isObject()) [[unlikely]] {
            throwException(globalObject, scope, createRuntimeError(globalObject, Typed::RuntimeErrorKind::IteratorResultNotObject, nullptr));
            OPERATION_RETURN(scope, makeUGPRPair(0, 0));
        }
        JSValue done = asObject(result)->get(globalObject, vm.propertyNames->done);
        OPERATION_RETURN_IF_EXCEPTION(scope, makeUGPRPair(0, 0));
        if (done.toBoolean(globalObject))
            OPERATION_RETURN(scope, resultOfStraightStep(true, jsUndefined()));
        value = asObject(result)->get(globalObject, vm.propertyNames->value);
    }
    OPERATION_RETURN_IF_EXCEPTION(scope, makeUGPRPair(0, 0));
    storeWordOfStraightState(state, WordOfStraightIteration::IsDone, jsBoolean(false));
    OPERATION_RETURN(scope, resultOfStraightStep(false, value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCloseIteration, void, (Instance* instance, JSCell* cell))
{
    constexpr Entry entry = Entry::operationAOTCloseIteration;
    AOT_OPERATION_BEGIN(instance, entry);
    closeStraightIterationUnlessDone(globalObject, cellArgument<SavedFrame>(cell, entry), ResultOfReturnInStraightIteration::MustBeObject);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCloseIterationAfterThrow, void, (Instance* instance, JSCell* cell))
{
    constexpr Entry entry = Entry::operationAOTCloseIterationAfterThrow;
    AOT_OPERATION_BEGIN(instance, entry);
    closeStraightIterationUnlessDone(globalObject, cellArgument<SavedFrame>(cell, entry), ResultOfReturnInStraightIteration::IsIgnored);
    TRY_CLEAR_EXCEPTION(scope, makeOperationResult(scope));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTOpenEnumeration, JSCell*, (Instance* instance, EncodedJSValue object))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTOpenEnumeration);
    JSPropertyNameEnumerator* enumerator = enumeratorFor(globalObject, JSValue::decode(object));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    SavedFrame* state = SavedFrame::createHeapFrame(vm, *instance, numberOfWordsOfStraightEnumeration, nullptr);
    storeWordOfStraightState(state, WordOfStraightEnumeration::Enumerator, enumerator);
    storeWordOfStraightState(state, WordOfStraightEnumeration::Position, jsNumber(0));
    vm.mutatorFence();
    OPERATION_RETURN(scope, state);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStepEnumeration, UGPRPair, (Instance* instance, JSCell* cell, EncodedJSValue object))
{
    constexpr Entry entry = Entry::operationAOTStepEnumeration;
    AOT_OPERATION_BEGIN(instance, entry);
    SavedFrame* state = cellArgument<SavedFrame>(cell, entry);
    JSValue enumerator = readWordOfStraightState(state, WordOfStraightEnumeration::Enumerator);
    uint64_t position = static_cast<uint64_t>(readWordOfStraightState(state, WordOfStraightEnumeration::Position).asNumber());
    EnumeratedKey found = findEnumeratedKey(globalObject, cellArgument<JSPropertyNameEnumerator>(enumerator.isCell() ? enumerator.asCell() : nullptr, entry), JSValue::decode(object), position);
    OPERATION_RETURN_IF_EXCEPTION(scope, makeUGPRPair(0, 0));
    storeWordOfStraightState(state, WordOfStraightEnumeration::Position, jsNumber(static_cast<double>(found.position + 1)));
    OPERATION_RETURN(scope, resultOfStraightStep(!found.key, found.key ? found.key : jsEmptyString(vm)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
