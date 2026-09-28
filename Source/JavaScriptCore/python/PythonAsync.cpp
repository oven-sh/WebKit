/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PythonBuiltins.h"

#include "GeneratorPrototype.h"
#include "IteratorOperations.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSPromise.h"
#include "PythonGenerators.h"
#include "TopExceptionScope.h"

// Coroutines, asynchronous generators, and what is awaited to drive them. The state machines are CPython's, Objects/genobject.c.

namespace JSC { namespace Python {

static PyNativeObject* asNative(JSValue value) { return uncheckedDowncast<PyNativeObject>(value.asCell()); }

static bool isOfType(JSGlobalObject* globalObject, JSValue value, BuiltinType type)
{
    return value.isCell() && typeOf(globalObject, value) == globalObject->pyRealm()->type(type);
}

GeneratorKind generatorKindOf(JSGlobalObject* globalObject, JSGenerator* generator)
{
    PyRealm* realm = globalObject->pyRealm();
    JSValue prototype = generator->structure()->storedPrototype(generator);
    if (prototype == JSValue(realm->typeCoroutine()))
        return GeneratorKind::Coroutine;
    if (prototype == JSValue(realm->typeAsyncGenerator()))
        return GeneratorKind::AsyncGenerator;
    return GeneratorKind::Generator;
}

JSGenerator* newCoroutine(JSGlobalObject* globalObject, JSValue body, bool isAsyncGenerator)
{
    VM& vm = globalObject->vm();
    JSGenerator* generator = JSGenerator::create(vm, globalObject->pyRealm()->structureFor(isAsyncGenerator ? BuiltinType::AsyncGenerator : BuiltinType::Coroutine));
    generator->internalField(static_cast<unsigned>(JSGenerator::Field::Next)).set(vm, generator, body);
    generator->internalField(static_cast<unsigned>(JSGenerator::Field::This)).set(vm, generator, jsUndefined());
    return generator;
}

JSValue wrapAsyncYield(JSGlobalObject* globalObject, JSValue value)
{
    return PyNativeObject::create(globalObject, BuiltinType::AsyncGeneratorWrappedValue, value);
}

static int32_t stateOf(JSGenerator* generator)
{
    return generator->internalField(static_cast<unsigned>(JSGenerator::Field::State)).get().asInt32();
}

JSValue exceptionToThrow(JSGlobalObject* globalObject, JSValue exception, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isClass(exception) && asType(exception)->isExceptionType()) {
        if (value && typeOf(globalObject, value)->isExceptionType())
            return value;
        RELEASE_AND_RETURN(scope, value && !isNone(value) ? call(globalObject, exception, value) : call(globalObject, exception));
    }
    if (!typeOf(globalObject, exception)->isExceptionType())
        return raiseTypeError(globalObject, scope, makeString("exceptions must be classes or instances deriving from BaseException, not "_s, typeName(globalObject, exception)));
    if (value && !isNone(value))
        return raiseTypeError(globalObject, scope, "instance exception may not have a separate value"_s);
    return exception;
}

// ---- Waiting for what is JavaScript's

bool isThenable(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isObject())
        return false;
    if (value.inherits<JSPromise>())
        return true;
    JSValue then = asObject(value)->get(globalObject, vm.propertyNames->then);
    RETURN_IF_EXCEPTION(scope, false);
    return then.isCallable();
}

// What `await promise` iterates. It yields the promise, once, to whatever is running the coroutine, as a Future of asyncio's yields itself. What is sent
// back is what the promise came to. Fields: the promise, how far it has got, and whether what it comes to is what the next() of an iterator of
// JavaScript's gives.
enum PromiseAwaiterState { AwaiterNotBegun, AwaiterWaiting, AwaiterDone };

static JSValue newPromiseAwaiter(JSGlobalObject* globalObject, JSValue promise, bool isIteratorResult)
{
    return PyNativeObject::create(globalObject, BuiltinType::PromiseAwaiter, promise, jsNumber(AwaiterNotBegun), jsBoolean(isIteratorResult));
}

JSValue awaitableFor(JSGlobalObject* globalObject, JSValue value)
{
    return newPromiseAwaiter(globalObject, value, false);
}

static JSValue finishPromiseAwaiter(JSGlobalObject* globalObject, PyNativeObject* self, JSValue settled)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    self->setField(vm, 1, jsNumber(AwaiterDone));
    if (self->field(2).isTrue()) {
        if (!settled.isObject())
            return raiseTypeError(globalObject, scope, makeString("iterator result "_s, repr(globalObject, settled), " is not an object"_s));
        JSValue done = asObject(settled)->get(globalObject, vm.propertyNames->done);
        RETURN_IF_EXCEPTION(scope, { });
        bool isDone = done.toBoolean(globalObject);
        if (isDone)
            return raise(globalObject, scope, BuiltinType::StopAsyncIteration, JSValue());
        settled = asObject(settled)->get(globalObject, vm.propertyNames->value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return raise(globalObject, scope, BuiltinType::StopIteration, isNone(settled) ? JSValue() : settled);
}

PYTHON_NATIVE(promiseAwaiterSend)
{
    NATIVE_PROLOGUE();
    auto* self = asNative(args[0]);
    JSValue sent = args.size() > 1 ? args[1] : jsUndefined();
    switch (self->field(1).asInt32()) {
    case AwaiterNotBegun: {
        if (!isNone(sent))
            return JSValue::encode(raiseTypeError(globalObject, scope, "can't send non-None value to a just-started promise_awaiter"_s));
        // What there is no waiting for is what it is already.
        bool waits = isThenable(globalObject, self->field(0));
        RETURN_IF_EXCEPTION(scope, { });
        if (!waits)
            RELEASE_AND_RETURN(scope, JSValue::encode(finishPromiseAwaiter(globalObject, self, self->field(0))));
        self->setField(vm, 1, jsNumber(AwaiterWaiting));
        return JSValue::encode(self->field(0));
    }
    case AwaiterWaiting:
        RELEASE_AND_RETURN(scope, JSValue::encode(finishPromiseAwaiter(globalObject, self, sent)));
    default:
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot reuse already awaited promise"_s));
    }
}

PYTHON_NATIVE(promiseAwaiterThrow)
{
    NATIVE_PROLOGUE();
    asNative(args[0])->setField(vm, 1, jsNumber(AwaiterDone));
    // A promise can be rejected with anything at all.
    JSValue exception = args[1];
    if (isClass(exception) || args.size() > 2) {
        exception = exceptionToThrow(globalObject, args[1], args.size() > 2 ? args[2] : JSValue());
        RETURN_IF_EXCEPTION(scope, { });
    }
    throwException(globalObject, scope, exception);
    return { };
}

PYTHON_NATIVE(promiseAwaiterClose)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    asNative(args[0])->setField(vm, 1, jsNumber(AwaiterDone));
    RETURN_NONE();
}

PYTHON_NATIVE(promiseAwait)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newPromiseAwaiter(globalObject, args[0], false));
}

// ---- JavaScript waiting for what is Python's

JSPromise* promiseOfAwaitable(VM& vm, JSCell* iterator)
{
    if (!iterator->isObject())
        return nullptr;
    JSValue promise = asObject(iterator)->getDirect(vm, vm.pythonNames().private_promise);
    return promise ? uncheckedDowncast<JSPromise>(promise.asCell()) : nullptr;
}

// Runs what is being awaited until it has to wait for something, and arranges to go on when that is settled. This is what a Task of asyncio's does, and
// what the engine does for an async function of JavaScript's.
void resumeAwaitable(JSGlobalObject* globalObject, JSObject* iterator, JSValue received, bool wasThrown)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSPromise* promise = promiseOfAwaitable(vm, iterator);
    // Empty: it comes to what it comes to. True: to { value, done }. An object: to that.
    JSValue settlement = iterator->getDirect(vm, names.private_settlement);

    while (true) {
        JSValue returned;
        JSValue yielded = stepIterator(globalObject, iterator, received, wasThrown, returned);
        bool waits = false;
        if (!scope.exception() && yielded) {
            // A bare `yield` gives everything else a turn.
            waits = isNone(yielded) || isThenable(globalObject, yielded);
        }
        if (Exception* exception = scope.exception()) [[unlikely]] {
            if (!scope.clearExceptionExceptTermination())
                return;
            if (settlement && isInstance(globalObject, exception->value(), globalObject->pyRealm()->typeStopAsyncIteration()))
                promise->resolve(globalObject, vm, createIteratorResultObject(globalObject, jsUndefined(), true));
            else
                promise->reject(vm, exception);
            return;
        }
        if (!yielded) {
            if (settlement)
                returned = settlement.isObject() ? settlement : JSValue(createIteratorResultObject(globalObject, returned, false));
            promise->resolve(globalObject, vm, returned);
            return;
        }
        if (waits) {
            JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, yielded, InternalMicrotask::PythonAwaitResume, iterator);
            return;
        }
        String shown = repr(globalObject, yielded);
        if (scope.exception() && !scope.clearExceptionExceptTermination())
            return;
        received = createException(globalObject, globalObject->pyRealm()->typeRuntimeError(), makeString("Task got bad yield: "_s, shown));
        wasThrown = true;
    }
}

JSPromise* toPromise(JSGlobalObject* globalObject, JSValue awaitable, JSValue settlement)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (awaitable.isCell()) {
        if (JSPromise* known = promiseOfAwaitable(vm, awaitable.asCell()))
            return known;
    }
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    JSValue iterator = getAwaitable(globalObject, awaitable, 0);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        if (scope.clearExceptionExceptTermination())
            promise->reject(vm, exception);
        return promise;
    }
    if (JSPromise* known = promiseOfAwaitable(vm, iterator.asCell()))
        return known;
    asObject(iterator)->putDirect(vm, names.private_promise, promise);
    if (settlement)
        asObject(iterator)->putDirect(vm, names.private_settlement, settlement);
    resumeAwaitable(globalObject, asObject(iterator), jsUndefined(), false);
    return promise;
}

// ---- await

bool isIterableCoroutine(JSGlobalObject* globalObject, JSValue value)
{
    if (typeOf(globalObject, value) != globalObject->pyRealm()->typeGenerator())
        return false;
    JSValue body = asGenerator(value)->internalField(static_cast<unsigned>(JSGenerator::Field::Next)).get();
    auto* function = dynamicDowncast<JSFunction>(body);
    return function && function->jsExecutable()->unlinkedExecutable()->pythonInfo()->isIterableCoroutine;
}

JSValue getAwaitable(JSGlobalObject* globalObject, JSValue value, unsigned context)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    PyType* type = typeOf(globalObject, value);
    if (type->hasFlag(PyType::IsJavaScript)) {
        // What JavaScript would wait for, Python waits for.
        bool waits = isThenable(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (waits)
            return newPromiseAwaiter(globalObject, value, false);
    }
    if (type == realm->typeCoroutine()) {
        JSValue awaited = asGenerator(value)->getDirect(vm, vm.pythonNames().private_yieldFrom);
        if (awaited && !isNone(awaited) && stateOf(asGenerator(value)) > 0)
            return raise(globalObject, scope, BuiltinType::RuntimeError, "coroutine is being awaited already"_s);
        return value;
    }
    // A generator that types.coroutine() has been at.
    if (isIterableCoroutine(globalObject, value))
        return value;
    JSValue self;
    JSValue method = lookupSpecial(globalObject, value, vm.pythonNames().dunder_await, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method) {
        if (context)
            return raiseTypeError(globalObject, scope, makeString("'async with' received an object from "_s, context == 1 ? "__aenter__"_s : "__aexit__"_s, " that does not implement __await__: "_s, type->nameString(globalObject)));
        return raiseTypeError(globalObject, scope, makeString('\'', type->nameString(globalObject), "' object can't be awaited"_s));
    }
    JSValue result = callMethod(globalObject, method, self);
    RETURN_IF_EXCEPTION(scope, { });
    PyType* resultType = typeOf(globalObject, result);
    if (resultType == realm->typeCoroutine())
        return raiseTypeError(globalObject, scope, "__await__() returned a coroutine"_s);
    if (!resultType->lookup(vm, vm.pythonNames().dunder_next))
        return raiseTypeError(globalObject, scope, makeString("__await__() returned non-iterator of type '"_s, resultType->nameString(globalObject), '\''));
    return result;
}

// What `for await` would go through, for something of JavaScript's: what its [Symbol.asyncIterator]() gives, or failing that what its [Symbol.iterator]()
// gives, made asynchronous as the engine makes it. Empty if it is not JavaScript's, or has neither.
static JSValue getJavaScriptAsyncIterator(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isObject() || !typeOf(globalObject, value)->hasFlag(PyType::IsJavaScript))
        return { };
    JSValue method = asObject(value)->get(globalObject, vm.propertyNames->asyncIteratorSymbol);
    RETURN_IF_EXCEPTION(scope, { });
    if (method.isCallable())
        RELEASE_AND_RETURN(scope, JSC::call(globalObject, method, JSC::getCallData(method), value, ArgList()));
    method = asObject(value)->get(globalObject, vm.propertyNames->iteratorSymbol);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method.isCallable())
        return { };
    JSValue iterator = JSC::call(globalObject, method, JSC::getCallData(method), value, ArgList());
    RETURN_IF_EXCEPTION(scope, { });
    if (!iterator.isObject())
        return raiseTypeError(globalObject, scope, "Iterator result interface is not an object"_s);
    JSValue next = asObject(iterator)->get(globalObject, vm.propertyNames->next);
    RETURN_IF_EXCEPTION(scope, { });
    return JSAsyncFromSyncIterator::create(vm, globalObject->asyncFromSyncIteratorStructure(), asObject(iterator), next, IterationMode::Generic);
}

// One step of such an iterator, to be awaited. Empty if it is not JavaScript's.
static JSValue getJavaScriptAsyncNext(JSGlobalObject* globalObject, JSValue iterator)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!iterator.isObject() || !typeOf(globalObject, iterator)->hasFlag(PyType::IsJavaScript))
        return { };
    JSValue next = asObject(iterator)->get(globalObject, vm.propertyNames->next);
    RETURN_IF_EXCEPTION(scope, { });
    if (!next.isCallable())
        return { };
    JSValue result = JSC::call(globalObject, next, JSC::getCallData(next), iterator, ArgList());
    RETURN_IF_EXCEPTION(scope, { });
    return newPromiseAwaiter(globalObject, result, true);
}

JSValue getAsyncIterator(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue iterator = getJavaScriptAsyncIterator(globalObject, value); iterator || scope.exception())
        return iterator;
    JSValue self;
    JSValue method = lookupSpecial(globalObject, value, vm.pythonNames().dunder_aiter, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return raiseTypeError(globalObject, scope, makeString("'async for' requires an object with __aiter__ method, got "_s, typeName(globalObject, value)));
    JSValue iterator = callMethod(globalObject, method, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!typeOf(globalObject, iterator)->lookup(vm, vm.pythonNames().dunder_anext) && !typeOf(globalObject, iterator)->hasFlag(PyType::IsJavaScript))
        return raiseTypeError(globalObject, scope, makeString("'async for' received an object from __aiter__ that does not implement __anext__: "_s, typeName(globalObject, iterator)));
    return iterator;
}

JSValue getAsyncNext(JSGlobalObject* globalObject, JSValue iterator)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue awaitable = getJavaScriptAsyncNext(globalObject, iterator); awaitable || scope.exception())
        return awaitable;
    JSValue self;
    JSValue method = lookupSpecial(globalObject, iterator, vm.pythonNames().dunder_anext, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return raiseTypeError(globalObject, scope, makeString("'async for' requires an iterator with __anext__ method, got "_s, typeName(globalObject, iterator)));
    JSValue next = callMethod(globalObject, method, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue awaitable = getAwaitable(globalObject, next, 0);
    if (scope.exception()) {
        JSValue cause = scope.exception()->value();
        if (catchException(globalObject, BuiltinType::TypeError)) {
            JSObject* error = createException(globalObject, globalObject->pyRealm()->typeTypeError(), makeString("'async for' received an invalid object from __anext__: "_s, typeName(globalObject, next)));
            error->putDirect(vm, vm.pythonNames().private_cause, cause);
            error->putDirect(vm, vm.pythonNames().private_context, cause);
            error->putDirect(vm, vm.pythonNames().private_suppressContext, jsBoolean(true));
            throwException(globalObject, scope, error);
        }
        return { };
    }
    return awaitable;
}

// ---- Coroutines

PYTHON_NATIVE(coroutineAwait)
{
    return JSValue::encode(PyNativeObject::create(globalObject, BuiltinType::CoroutineWrapper, callFrame->argument(0)));
}

// send, throw and close, of a generator, a coroutine, or the wrapper that a coroutine's __await__() gives.
static JSGenerator* generatorOfSelf(JSGlobalObject* globalObject, JSValue self)
{
    if (isOfType(globalObject, self, BuiltinType::CoroutineWrapper))
        return asGenerator(asNative(self)->field(0));
    return asGenerator(self);
}

PYTHON_NATIVE(coroutineSend)
{
    NATIVE_PROLOGUE();
    // With no argument, it is __next__.
    RELEASE_AND_RETURN(scope, JSValue::encode(generatorSend(globalObject, generatorOfSelf(globalObject, args.at(0)), args.size() > 1 ? args[1] : jsUndefined())));
}

PYTHON_NATIVE(coroutineThrow)
{
    NATIVE_PROLOGUE();
    JSValue exception = exceptionToThrow(globalObject, args[1], args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(generatorThrow(globalObject, generatorOfSelf(globalObject, args[0]), exception)));
}

PYTHON_NATIVE(coroutineClose)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(generatorClose(globalObject, generatorOfSelf(globalObject, args.at(0)))));
}

// ---- Asynchronous generators

enum AwaitableState : int32_t { Init, Iterating, Closed };

static bool flag(JSGenerator* generator, const Identifier& name)
{
    JSValue value = generator->getDirect(generator->vm(), name);
    return value && value.asBoolean();
}

static void setFlag(JSGenerator* generator, const Identifier& name, bool value)
{
    generator->putDirect(generator->vm(), name, jsBoolean(value));
}

// Resumes it. When it returns, that is StopAsyncIteration.
static JSValue resumeAsyncGenerator(JSGlobalObject* globalObject, JSGenerator* generator, JSValue sent, JSGenerator::ResumeMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue returned;
    JSValue yielded = resumeGenerator(globalObject, generator, sent, mode, returned);
    RETURN_IF_EXCEPTION(scope, { });
    if (yielded)
        return yielded;
    return raise(globalObject, scope, BuiltinType::StopAsyncIteration, JSValue());
}

static bool isWrappedValue(JSGlobalObject* globalObject, JSValue value)
{
    return value && isOfType(globalObject, value, BuiltinType::AsyncGeneratorWrappedValue);
}

static bool hasRaised(JSGlobalObject* globalObject, ThrowScope& scope, BuiltinType type)
{
    Exception* exception = scope.exception();
    return exception && isInstance(globalObject, exception->value(), globalObject->pyRealm()->type(type));
}

// What the generator yielded ends the await, by StopIteration. What an await inside it passed up goes on up.
static JSValue unwrapAsyncValue(JSGlobalObject* globalObject, JSGenerator* generator, JSValue result)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (!result) {
        if (hasRaised(globalObject, scope, BuiltinType::StopAsyncIteration) || hasRaised(globalObject, scope, BuiltinType::GeneratorExit))
            setFlag(generator, names.private_isClosedAsync, true);
        setFlag(generator, names.private_isRunningAsync, false);
        return { };
    }
    if (isWrappedValue(globalObject, result)) {
        setFlag(generator, names.private_isRunningAsync, false);
        JSValue value = asNative(result)->field(0);
        JSObject* stop = createException(globalObject, globalObject->pyRealm()->typeStopIteration(), isNone(value) ? JSValue() : value);
        throwException(globalObject, scope, stop);
        return { };
    }
    return result;
}

// asend: fields are the generator, what to send, and the state.
static JSValue newASend(JSGlobalObject* globalObject, JSValue generator, JSValue value)
{
    return PyNativeObject::create(globalObject, BuiltinType::AsyncGeneratorASend, generator, value, jsNumber(AwaitableState::Init));
}

static int32_t awaitableState(PyNativeObject* object) { return object->field(2).asInt32(); }
static void setAwaitableState(VM& vm, PyNativeObject* object, AwaitableState state) { object->setField(vm, 2, jsNumber(state)); }

PYTHON_NATIVE(asendSend)
{
    NATIVE_PROLOGUE();
    PyNativeObject* self = asNative(args.at(0));
    JSGenerator* generator = asGenerator(self->field(0));
    JSValue argument = args.size() > 1 ? args[1] : jsUndefined();
    if (awaitableState(self) == AwaitableState::Closed)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot reuse already awaited __anext__()/asend()"_s));
    if (awaitableState(self) == AwaitableState::Init) {
        if (flag(generator, names.private_isRunningAsync)) {
            setAwaitableState(vm, self, AwaitableState::Closed);
            return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "anext(): asynchronous generator is already running"_s));
        }
        if (isNone(argument))
            argument = self->field(1);
        setAwaitableState(vm, self, AwaitableState::Iterating);
    }
    setFlag(generator, names.private_isRunningAsync, true);
    JSValue result = resumeAsyncGenerator(globalObject, generator, argument, JSGenerator::ResumeMode::NormalMode);
    result = unwrapAsyncValue(globalObject, generator, result);
    if (!result)
        setAwaitableState(vm, self, AwaitableState::Closed);
    scope.release();
    return JSValue::encode(result);
}

static JSValue asendThrowImpl(JSGlobalObject* globalObject, PyNativeObject* self, JSValue exception)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSGenerator* generator = asGenerator(self->field(0));
    if (awaitableState(self) == AwaitableState::Closed)
        return raise(globalObject, scope, BuiltinType::RuntimeError, "cannot reuse already awaited __anext__()/asend()"_s);
    if (awaitableState(self) == AwaitableState::Init) {
        if (flag(generator, names.private_isRunningAsync)) {
            setAwaitableState(vm, self, AwaitableState::Closed);
            return raise(globalObject, scope, BuiltinType::RuntimeError, "anext(): asynchronous generator is already running"_s);
        }
        setAwaitableState(vm, self, AwaitableState::Iterating);
        setFlag(generator, names.private_isRunningAsync, true);
    }
    JSValue result = resumeAsyncGenerator(globalObject, generator, exception, JSGenerator::ResumeMode::ThrowMode);
    result = unwrapAsyncValue(globalObject, generator, result);
    if (!result) {
        setFlag(generator, names.private_isRunningAsync, false);
        setAwaitableState(vm, self, AwaitableState::Closed);
    }
    scope.release();
    return result;
}

PYTHON_NATIVE(asendThrow)
{
    NATIVE_PROLOGUE();
    JSValue exception = exceptionToThrow(globalObject, args[1], args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(asendThrowImpl(globalObject, asNative(args[0]), exception)));
}

// What close() makes of throwing GeneratorExit in: nothing, if that ended it.
static EncodedJSValue finishClose(JSGlobalObject* globalObject, ThrowScope& scope, JSValue result)
{
    if (!result) {
        if (!catchException(globalObject, BuiltinType::StopIteration) && !catchException(globalObject, BuiltinType::StopAsyncIteration))
            catchException(globalObject, BuiltinType::GeneratorExit);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(jsUndefined());
    }
    return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "coroutine ignored GeneratorExit"_s));
}

PYTHON_NATIVE(asendClose)
{
    NATIVE_PROLOGUE();
    PyNativeObject* self = asNative(args.at(0));
    if (awaitableState(self) == AwaitableState::Closed)
        RETURN_NONE();
    JSValue result = asendThrowImpl(globalObject, self, createException(globalObject, realm->typeGeneratorExit(), JSValue()));
    return finishClose(globalObject, scope, result);
}

// athrow and aclose: fields are the generator, the exception to throw (empty for aclose), and the state.
static JSValue athrowFinish(JSGlobalObject* globalObject, PyNativeObject* self, bool ignoredExit)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSGenerator* generator = asGenerator(self->field(0));
    setFlag(generator, vm.pythonNames().private_isRunningAsync, false);
    setAwaitableState(vm, self, AwaitableState::Closed);
    if (ignoredExit)
        return raise(globalObject, scope, BuiltinType::RuntimeError, "async generator ignored GeneratorExit"_s);
    // aclose() is done when the generator is: neither of these is anyone else's business.
    if (!self->field(1) && (catchException(globalObject, BuiltinType::StopAsyncIteration) || catchException(globalObject, BuiltinType::GeneratorExit)))
        return raise(globalObject, scope, BuiltinType::StopIteration, JSValue());
    return { };
}

static ASCIILiteral alreadyRunningMessage(PyNativeObject* self)
{
    return self->field(1) ? "athrow(): asynchronous generator is already running"_s : "aclose(): asynchronous generator is already running"_s;
}

PYTHON_NATIVE(athrowSend)
{
    NATIVE_PROLOGUE();
    PyNativeObject* self = asNative(args.at(0));
    JSGenerator* generator = asGenerator(self->field(0));
    JSValue argument = args.size() > 1 ? args[1] : jsUndefined();
    bool isClose = !self->field(1);

    if (awaitableState(self) == AwaitableState::Closed)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot reuse already awaited aclose()/athrow()"_s));
    if (stateOf(generator) == static_cast<int32_t>(JSGenerator::State::Completed)) {
        setAwaitableState(vm, self, AwaitableState::Closed);
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    }

    JSValue result;
    if (awaitableState(self) == AwaitableState::Init) {
        if (flag(generator, names.private_isRunningAsync)) {
            setAwaitableState(vm, self, AwaitableState::Closed);
            return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, alreadyRunningMessage(self)));
        }
        if (flag(generator, names.private_isClosedAsync)) {
            setAwaitableState(vm, self, AwaitableState::Closed);
            return JSValue::encode(raise(globalObject, scope, BuiltinType::StopAsyncIteration, JSValue()));
        }
        if (!isNone(argument))
            return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "can't send non-None value to a just-started coroutine"_s));
        setAwaitableState(vm, self, AwaitableState::Iterating);
        setFlag(generator, names.private_isRunningAsync, true);

        if (isClose) {
            setFlag(generator, names.private_isClosedAsync, true);
            result = resumeAsyncGenerator(globalObject, generator, createException(globalObject, realm->typeGeneratorExit(), JSValue()), JSGenerator::ResumeMode::ThrowMode);
            if (isWrappedValue(globalObject, result))
                RELEASE_AND_RETURN(scope, JSValue::encode(athrowFinish(globalObject, self, true)));
        } else {
            result = resumeAsyncGenerator(globalObject, generator, self->field(1), JSGenerator::ResumeMode::ThrowMode);
            result = unwrapAsyncValue(globalObject, generator, result);
        }
        if (!result)
            RELEASE_AND_RETURN(scope, JSValue::encode(athrowFinish(globalObject, self, false)));
        return JSValue::encode(result);
    }

    result = resumeAsyncGenerator(globalObject, generator, argument, JSGenerator::ResumeMode::NormalMode);
    if (!isClose)
        RELEASE_AND_RETURN(scope, JSValue::encode(unwrapAsyncValue(globalObject, generator, result)));
    if (!result)
        RELEASE_AND_RETURN(scope, JSValue::encode(athrowFinish(globalObject, self, false)));
    if (isWrappedValue(globalObject, result))
        RELEASE_AND_RETURN(scope, JSValue::encode(athrowFinish(globalObject, self, true)));
    return JSValue::encode(result);
}

static JSValue athrowThrowImpl(JSGlobalObject* globalObject, PyNativeObject* self, JSValue exception)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSGenerator* generator = asGenerator(self->field(0));
    bool isClose = !self->field(1);
    if (awaitableState(self) == AwaitableState::Closed)
        return raise(globalObject, scope, BuiltinType::RuntimeError, "cannot reuse already awaited aclose()/athrow()"_s);
    if (awaitableState(self) == AwaitableState::Init) {
        if (flag(generator, names.private_isRunningAsync)) {
            setAwaitableState(vm, self, AwaitableState::Closed);
            return raise(globalObject, scope, BuiltinType::RuntimeError, alreadyRunningMessage(self));
        }
        setAwaitableState(vm, self, AwaitableState::Iterating);
        setFlag(generator, names.private_isRunningAsync, true);
    }
    JSValue result = resumeAsyncGenerator(globalObject, generator, exception, JSGenerator::ResumeMode::ThrowMode);
    if (!isClose) {
        result = unwrapAsyncValue(globalObject, generator, result);
        if (!result) {
            setFlag(generator, names.private_isRunningAsync, false);
            setAwaitableState(vm, self, AwaitableState::Closed);
        }
        scope.release();
        return result;
    }
    if (isWrappedValue(globalObject, result))
        RELEASE_AND_RETURN(scope, athrowFinish(globalObject, self, true));
    if (!result)
        RELEASE_AND_RETURN(scope, athrowFinish(globalObject, self, false));
    return result;
}

PYTHON_NATIVE(athrowThrow)
{
    NATIVE_PROLOGUE();
    JSValue exception = exceptionToThrow(globalObject, args[1], args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(athrowThrowImpl(globalObject, asNative(args[0]), exception)));
}

PYTHON_NATIVE(athrowClose)
{
    NATIVE_PROLOGUE();
    PyNativeObject* self = asNative(args.at(0));
    if (awaitableState(self) == AwaitableState::Closed)
        RETURN_NONE();
    JSValue result = athrowThrowImpl(globalObject, self, createException(globalObject, realm->typeGeneratorExit(), JSValue()));
    return finishClose(globalObject, scope, result);
}

// The first time that anything is asked of one, whoever is running things is told of it: sys.set_asyncgen_hooks(). False if it raised.
static bool initializeHooks(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    auto* generator = uncheckedDowncast<JSGenerator>(self.asCell());
    if (flag(generator, names.private_hasHooks))
        return true;
    setFlag(generator, names.private_hasHooks, true);
    // FIXME: It is to be called when the generator is let go of without having finished. See "What is not decided" in README.md.
    if (JSValue finalizer = realm->asyncGeneratorFinalizerHook())
        generator->putDirect(vm, names.private_finalizer, finalizer);
    JSValue firstIteration = realm->asyncGeneratorFirstIterationHook();
    if (!firstIteration)
        return true;
    auto scope = DECLARE_THROW_SCOPE(vm);
    call(globalObject, firstIteration, generator);
    return !scope.exception();
}

PYTHON_NATIVE(asyncGeneratorANext)
{
    NATIVE_PROLOGUE();
    initializeHooks(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(newASend(globalObject, args[0], jsUndefined()));
}

PYTHON_NATIVE(asyncGeneratorASend)
{
    NATIVE_PROLOGUE();
    initializeHooks(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(newASend(globalObject, args[0], args[1]));
}

PYTHON_NATIVE(asyncGeneratorAThrow)
{
    NATIVE_PROLOGUE();
    JSValue exception = exceptionToThrow(globalObject, args[1], args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    initializeHooks(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyNativeObject::create(globalObject, BuiltinType::AsyncGeneratorAThrow, args[0], exception, jsNumber(AwaitableState::Init)));
}

PYTHON_NATIVE(asyncGeneratorAClose)
{
    NATIVE_PROLOGUE();
    initializeHooks(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyNativeObject::create(globalObject, BuiltinType::AsyncGeneratorAThrow, args[0], JSValue(), jsNumber(AwaitableState::Init)));
}

// ---- aiter() and anext()

PYTHON_NATIVE(builtinAIter)
{
    NATIVE_PROLOGUE();
    if (JSValue iterator = getJavaScriptAsyncIterator(globalObject, args[0]); iterator || scope.exception())
        return JSValue::encode(iterator);
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[0], names.dunder_aiter, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, args[0]), "' object is not an async iterable"_s)));
    JSValue iterator = callMethod(globalObject, method, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!typeOf(globalObject, iterator)->lookup(vm, names.dunder_anext))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("aiter() returned not an async iterator of type '"_s, typeName(globalObject, iterator), '\'')));
    return JSValue::encode(iterator);
}

// anext(iterator[, default])
PYTHON_NATIVE(builtinANext)
{
    NATIVE_PROLOGUE();
    JSValue awaitable = getJavaScriptAsyncNext(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!awaitable) {
        JSValue self;
        JSValue method = lookupSpecial(globalObject, args[0], names.dunder_anext, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (!method)
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, args[0]), "' object is not an async iterator"_s)));
        awaitable = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (args.size() == 1)
        return JSValue::encode(awaitable);
    return JSValue::encode(PyNativeObject::create(globalObject, BuiltinType::ANextAwaitable, awaitable, args[1]));
}

// What anext() with a default gives: the awaitable, but that the end of the iteration is the default instead.
static JSValue anextIterator(JSGlobalObject* globalObject, PyNativeObject* self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue awaitable = getAwaitable(globalObject, self->field(0), 0);
    RETURN_IF_EXCEPTION(scope, { });
    if (isOfType(globalObject, awaitable, BuiltinType::Coroutine))
        return PyNativeObject::create(globalObject, BuiltinType::CoroutineWrapper, awaitable);
    return awaitable;
}

// __next__, send, throw and close: data says which.
PYTHON_NATIVE(anextProxy)
{
    static constexpr ASCIILiteral methods[] = { "__next__"_s, "send"_s, "throw"_s, "close"_s };
    ASCIILiteral method = methods[unpack<unsigned>(callFrame, 0)];
    NATIVE_PROLOGUE();
    PyNativeObject* self = asNative(args.at(0));
    JSValue iterator = anextIterator(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue function = getAttribute(globalObject, iterator, Identifier::fromString(vm, method));
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    for (unsigned i = 1; i < args.size(); ++i)
        arguments.append(args[i]);
    JSValue result = call(globalObject, function, arguments);
    if (scope.exception() && catchException(globalObject, BuiltinType::StopAsyncIteration)) {
        JSValue defaultValue = self->field(1);
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, isNone(defaultValue) ? JSValue() : defaultValue));
    }
    scope.release();
    return JSValue::encode(result);
}

// ---- Setting them up

void initializeAsyncTypes(JSGlobalObject* globalObject, JSObject* builtins)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    for (PyType* type : { realm->typeGenerator(), realm->typeCoroutine(), realm->typeAsyncGenerator() })
        type->setInstanceStructure(vm, JSGenerator::createStructure(vm, globalObject, type));
    // What JavaScript can do with a generator it can do with one of Python's.
    realm->typeGenerator()->setPrototypeDirect(vm, globalObject->generatorPrototype());
    for (PyType* type : { realm->typeCoroutineWrapper(), realm->typeAsyncGeneratorASend(), realm->typeAsyncGeneratorAThrow(), realm->typePromiseAwaiter(), realm->typeAsyncGeneratorWrappedValue(), realm->typeANextAwaitable() })
        type->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, type));

    addMethods(globalObject, realm->typeCoroutine(), {
        { "__await__"_s, coroutineAwait },
        { "send"_s, coroutineSend },
        { "throw"_s, coroutineThrow, Kind::Method, 0, "($self, typ, val=None, tb=None, /)"_s },
        { "close"_s, coroutineClose },
    });
    addMethods(globalObject, realm->typeCoroutineWrapper(), {
        { "__iter__"_s, nativeSelf },
        { "__next__"_s, coroutineSend },
        { "send"_s, coroutineSend },
        { "throw"_s, coroutineThrow, Kind::Method, 0, "($self, typ, val=None, tb=None, /)"_s },
        { "close"_s, coroutineClose },
    });
    addMethods(globalObject, realm->typeAsyncGenerator(), {
        { "__aiter__"_s, nativeSelf },
        { "__anext__"_s, asyncGeneratorANext },
        { "asend"_s, asyncGeneratorASend },
        { "athrow"_s, asyncGeneratorAThrow, Kind::Method, 0, "($self, typ, val=None, tb=None, /)"_s },
        { "aclose"_s, asyncGeneratorAClose },
    });
    addMethods(globalObject, realm->typeAsyncGeneratorASend(), {
        { "__await__"_s, nativeSelf },
        { "__iter__"_s, nativeSelf },
        { "__next__"_s, asendSend },
        { "send"_s, asendSend },
        { "throw"_s, asendThrow, Kind::Method, 0, "($self, typ, val=None, tb=None, /)"_s },
        { "close"_s, asendClose },
    });
    addMethods(globalObject, realm->typeAsyncGeneratorAThrow(), {
        { "__await__"_s, nativeSelf },
        { "__iter__"_s, nativeSelf },
        { "__next__"_s, athrowSend },
        { "send"_s, athrowSend },
        { "throw"_s, athrowThrow, Kind::Method, 0, "($self, typ, val=None, tb=None, /)"_s },
        { "close"_s, athrowClose },
    });
    addMethods(globalObject, realm->typeANextAwaitable(), {
        { "__await__"_s, nativeSelf },
        { "__iter__"_s, nativeSelf },
        { "__next__"_s, anextProxy, Kind::Method, 0 },
        { "send"_s, anextProxy, Kind::Method, 1 },
        { "throw"_s, anextProxy, Kind::Method, 2, "($self, typ, val=None, tb=None, /)"_s },
        { "close"_s, anextProxy, Kind::Method, 3 },
    });
    addMethods(globalObject, realm->typePromiseAwaiter(), {
        { "__await__"_s, nativeSelf, Kind::Wrapper, 0, "($self, /)"_s },
        { "__iter__"_s, nativeSelf, Kind::Wrapper, 0, "($self, /)"_s },
        { "__next__"_s, promiseAwaiterSend, Kind::Wrapper, 0, "($self, /)"_s },
        { "send"_s, promiseAwaiterSend, Kind::Method, 0, "($self, value, /)"_s },
        { "throw"_s, promiseAwaiterThrow, Kind::Method, 0, "($self, typ, val=None, tb=None, /)"_s },
        { "close"_s, promiseAwaiterClose, Kind::Method, 0, "($self, /)"_s },
    });
    addMethods(globalObject, realm->typeJSPromise(), {
        { "__await__"_s, promiseAwait, Kind::Wrapper, 0, "($self, /)"_s },
    });
    addFunction(globalObject, builtins, "aiter"_s, builtinAIter);
    addFunction(globalObject, builtins, "anext"_s, builtinANext);
}

} } // namespace JSC::Python
