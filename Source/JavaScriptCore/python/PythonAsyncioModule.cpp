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

#include "JSCInlines.h"
#include "JSPromise.h"
#include "PyDict.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonAsyncio.h"
#include "PythonContextVars.h"
#include "PythonGenerators.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSequences.h"

// The module _asyncio: Modules/_asynciomodule.c of CPython, function for function. It is Future and Task, and which event loop and which task are running.

namespace JSC { namespace Python {

namespace {

using Kind = PyNativeFunction::Kind;

// asyncio_state, and what CPython keeps in the state of the thread.
struct AsyncioState final : NativeState {
    PYTHON_NATIVE_STATE(AsyncioState);
    WriteBarrier<PyType> futureIteratorType;
    WriteBarrier<PyType> stepWrapperType;
    WriteBarrier<PyType> futureType;
    WriteBarrier<PyType> taskType;
    WriteBarrier<Unknown> wakeup; // TaskWakeupDef
    // What a promise that a task is waiting for is given to call, and what the loop is then given to call: see waitForPromise().
    WriteBarrier<Unknown> promiseFulfilled;
    WriteBarrier<Unknown> promiseRejected;
    WriteBarrier<Unknown> finishStandIn;
    WriteBarrier<KeywordNames> contextKeyword;
    // CPython has the tasks that are its own on a list that does not keep them. There is no such list here, so they are where those that are not its own are: in a WeakSet.
    WriteBarrier<Unknown> scheduledTasks;
    WriteBarrier<Unknown> otherTasks;
    WriteBarrier<PySet> otherEagerTasks;
    WriteBarrier<PySet> coroutineTypes;
    WriteBarrier<Unknown> getEventLoopPolicy;
    WriteBarrier<Unknown> futureRepr;
    WriteBarrier<Unknown> cancelledError;
    WriteBarrier<Unknown> invalidStateError;
    WriteBarrier<Unknown> taskGetStack;
    WriteBarrier<Unknown> taskPrintStack;
    WriteBarrier<Unknown> taskRepr;
    WriteBarrier<Unknown> isCoroutineFunction;
    WriteBarrier<Unknown> extractStack;
    WriteBarrier<Unknown> runningLoop;
    WriteBarrier<Unknown> runningTask;
    uint64_t taskNameCounter { 0 };

    // The loop that the host turns: see hostedLoop().
    WriteBarrier<Unknown> hostedLoop;
    bool isMakingHostedLoop { false };
    bool isBeingTurned { false };
    // What it said when it came to where it would wait, this time round.
    bool hasNotedWait { false };
    bool hadEvents { false };
    int descriptor { -1 };
    int watched { 0 };
    std::optional<Seconds> wouldHaveWaited;
    // How much it watches for with nothing asked of it, which is its own being woken.
    int watchedForItself { 0 };
};

template<typename Visitor>
void AsyncioState::visit(Visitor& visitor)
{
    visitor.append(futureIteratorType);
    visitor.append(stepWrapperType);
    visitor.append(futureType);
    visitor.append(taskType);
    visitor.append(wakeup);
    visitor.append(promiseFulfilled);
    visitor.append(promiseRejected);
    visitor.append(finishStandIn);
    visitor.append(contextKeyword);
    visitor.append(scheduledTasks);
    visitor.append(otherTasks);
    visitor.append(otherEagerTasks);
    visitor.append(coroutineTypes);
    visitor.append(getEventLoopPolicy);
    visitor.append(futureRepr);
    visitor.append(cancelledError);
    visitor.append(invalidStateError);
    visitor.append(taskGetStack);
    visitor.append(taskPrintStack);
    visitor.append(taskRepr);
    visitor.append(isCoroutineFunction);
    visitor.append(extractStack);
    visitor.append(runningLoop);
    visitor.append(runningTask);
    visitor.append(hostedLoop);
}

AsyncioState& asyncioState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<AsyncioState>(); }

enum class Status : uint8_t { Pending, Cancelled, Finished };

// FutureObj, and TaskObj, which begins with one. A task is told by its class.
struct Future final : NativeState {
    PYTHON_NATIVE_STATE(Future);
    WriteBarrier<Unknown> loop;
    WriteBarrier<Unknown> callback0;
    WriteBarrier<Unknown> context0;
    WriteBarrier<Unknown> callbacks;
    WriteBarrier<Unknown> exception;
    WriteBarrier<Unknown> exceptionTraceback;
    WriteBarrier<Unknown> result;
    WriteBarrier<Unknown> sourceTraceback;
    WriteBarrier<Unknown> cancelMessage;
    WriteBarrier<Unknown> cancelledException;
    WriteBarrier<Unknown> awaitedBy;
    Status status { Status::Pending };
    bool awaitedByIsSet { false };
    bool logsTraceback { false };
    bool isBlocking { false };
    // It stands for a promise that a task is waiting for, and this is what that came to.
    bool standsForPromise { false };
    bool promiseWasRejected { false };
    WriteBarrier<Unknown> promiseOutcome;
    // The promise that JavaScript has for it, if it has been given one.
    WriteBarrier<Unknown> promise;
    // It is a task that has taken over what JavaScript was running, and has that one's promise to settle.
    bool settlesAwaitable { false };

    bool mustCancel { false };
    bool logsDestroyPending { false };
    int cancelsRequested { 0 };
    WriteBarrier<Unknown> waiter;
    WriteBarrier<Unknown> coroutine;
    WriteBarrier<Unknown> name;
    WriteBarrier<Unknown> context;

    bool isAlive() const { return !!loop; }
};

template<typename Visitor>
void Future::visit(Visitor& visitor)
{
    visitor.append(loop);
    visitor.append(callback0);
    visitor.append(context0);
    visitor.append(callbacks);
    visitor.append(exception);
    visitor.append(exceptionTraceback);
    visitor.append(result);
    visitor.append(sourceTraceback);
    visitor.append(cancelMessage);
    visitor.append(cancelledException);
    visitor.append(awaitedBy);
    visitor.append(promiseOutcome);
    visitor.append(promise);
    visitor.append(waiter);
    visitor.append(coroutine);
    visitor.append(name);
    visitor.append(context);
}

PyStateObject* asFutureCell(JSValue value) { return uncheckedDowncast<PyStateObject>(value.asCell()); }
Future& futureIn(PyStateObject* cell) { return cell->state<Future>(); }

bool isExactFutureOrTask(JSGlobalObject* globalObject, JSValue value)
{
    auto& state = asyncioState(globalObject);
    PyType* type = typeOf(globalObject, value);
    return type == state.futureType.get() || type == state.taskType.get();
}
bool isFutureOrTask(JSGlobalObject* globalObject, JSValue value) { return isInstance(globalObject, value, asyncioState(globalObject).futureType.get()); }
bool isTask(JSGlobalObject* globalObject, JSValue value) { return isInstance(globalObject, value, asyncioState(globalObject).taskType.get()); }

Identifier named(VM& vm, ASCIILiteral name) { return Identifier::fromString(vm, name); }
void setOrClear(VM& vm, JSCell* owner, WriteBarrier<Unknown>& slot, JSValue value)
{
    if (value)
        slot.set(vm, owner, value);
    else
        slot.clear();
}
JSValue orNone(JSValue value) { return value ? value : jsUndefined(); }

// PyErr_SetString(), of a class that is written in Python.
JSValue raiseOf(JSGlobalObject* globalObject, ThrowScope& scope, JSValue exceptionClass, const String& message)
{
    JSValue exception = call(globalObject, exceptionClass, jsString(globalObject->vm(), message));
    RETURN_IF_EXCEPTION(scope, { });
    setContext(globalObject, asObject(exception));
    throwException(globalObject, scope, exception);
    return { };
}

JSValue raiseInvalidState(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral message)
{
    return raiseOf(globalObject, scope, asyncioState(globalObject).invalidStateError.get(), message);
}

// PyErr_SetObject(), of an exception that there is already. Or of what JavaScript threw that is no exception, which a task can have ended with: see stepTaskEntered().
JSValue raiseException(JSGlobalObject* globalObject, ThrowScope& scope, JSValue exception)
{
    if (isInstance(globalObject, exception, globalObject->pyRealm()->typeBaseException()))
        setContext(globalObject, asObject(exception));
    throwException(globalObject, scope, exception);
    return { };
}

// PyErr_GivenExceptionMatches(), of an exception and a class.
bool matches(JSGlobalObject* globalObject, JSValue exception, JSValue exceptionClass)
{
    return isClass(exceptionClass) && isInstance(globalObject, exception, asType(exceptionClass));
}

// ENSURE_FUTURE_ALIVE. False if it raised.
bool ensureAlive(JSGlobalObject* globalObject, ThrowScope& scope, Future& future)
{
    if (future.isAlive())
        return true;
    raise(globalObject, scope, BuiltinType::RuntimeError, "Future object is not initialized."_s);
    return false;
}

// get_future_loop(): asyncio.futures._get_loop()
JSValue loopOfFuture(JSGlobalObject* globalObject, JSValue future)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isExactFutureOrTask(globalObject, future))
        return futureIn(asFutureCell(future)).loop.get();
    JSValue getLoop = getAttributeIfPresent(globalObject, future, named(vm, "get_loop"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (getLoop)
        RELEASE_AND_RETURN(scope, call(globalObject, getLoop));
    RELEASE_AND_RETURN(scope, getAttribute(globalObject, future, named(vm, "_loop"_s)));
}

// ---- The loop that the host turns

// Once round: what is ready is run, and what has happened to what it is watching is seen to. run_forever() goes round until it is stopped, and it is stopped where it would wait: noteWaitOfEventLoop().
void turnHostedLoop(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = asyncioState(globalObject);
    // Whatever else is going on is as it might be in another thread.
    JSValue outerLoop = state.runningLoop.get();
    JSValue outerTask = state.runningTask.get();
    JSObject* outerAwaitable = realm->awaitableBeingRun();
    state.runningLoop.clear();
    state.runningTask.clear();
    realm->setAwaitableBeingRun(nullptr);
    state.isBeingTurned = true;
    state.hasNotedWait = false;
    callMethodNamed(globalObject, state.hostedLoop.get(), named(vm, "run_forever"_s));
    state.isBeingTurned = false;
    realm->setAwaitableBeingRun(outerAwaitable);
    setOrClear(vm, realm, state.runningLoop, outerLoop);
    setOrClear(vm, realm, state.runningTask, outerTask);
}

// asyncio's own, as asyncio.run() would make. Nothing in Python runs it. Empty if it raised.
JSValue hostedLoop(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& state = asyncioState(globalObject);
    if (JSValue loop = state.hostedLoop.get()) {
        JSValue closed = callMethodNamed(globalObject, loop, named(vm, "is_closed"_s));
        RETURN_IF_EXCEPTION(scope, { });
        bool isClosed = isTrue(globalObject, closed);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isClosed)
            return loop;
        state.hostedLoop.clear();
    }
    SetForScope isMaking(state.isMakingHostedLoop, true);
    JSValue make = importModuleAttribute(globalObject, "asyncio.events"_s, "new_event_loop"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue loop = call(globalObject, make);
    RETURN_IF_EXCEPTION(scope, { });
    state.hostedLoop.set(vm, realm, loop);
    // It has nothing to do yet. This is to find out what the host is to watch.
    turnHostedLoop(globalObject);
    if (scope.exception() || !state.hasNotedWait) [[unlikely]] {
        state.hostedLoop.clear();
        if (!scope.exception())
            raise(globalObject, scope, BuiltinType::RuntimeError, "the event loop has no selector that can be watched"_s);
        return { };
    }
    state.watchedForItself = state.watched;
    return loop;
}

PyStateObject* adopt(JSGlobalObject*, JSObject* iterator, JSValue loop);
JSValue runningLoopOrRaise(JSGlobalObject*);

// The loop that is running. Empty if there is none, or if it raised.
//
// What JavaScript is waiting for and is running is in a loop, though nothing in Python may be running one: then it is the one that the host turns. And what is running in a loop is a task. So it is one from when it
// first asks, and is there in all_tasks() and for current_task().
JSValue runningLoop(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& state = asyncioState(globalObject);
    JSValue loop = state.runningLoop.get();
    JSObject* awaitable = realm->awaitableBeingRun();
    if (!awaitable || state.isMakingHostedLoop)
        return loop;
    if (!loop) {
        if (!realm->configuration().watchEventLoop)
            return { };
        loop = hostedLoop(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        // Until JavaScriptStep is over
        state.runningLoop.set(vm, realm, loop);
    }
    if (!state.runningTask) {
        adopt(globalObject, awaitable, loop);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return loop;
}

JSValue getEventLoop(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = asyncioState(globalObject);
    JSValue running = runningLoop(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (running)
        return running;
    JSValue policy = call(globalObject, state.getEventLoopPolicy.get());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callMethodNamed(globalObject, policy, named(vm, "get_event_loop"_s)));
}

// loop.call_soon(function, argument, context=context). Either of the last two may be empty.
void callSoon(JSGlobalObject* globalObject, JSValue loop, JSValue function, JSValue argument, JSValue context)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = getAttribute(globalObject, loop, named(vm, "call_soon"_s));
    RETURN_IF_EXCEPTION(scope, void());
    MarkedArgumentBuffer arguments;
    arguments.append(function);
    if (argument)
        arguments.append(argument);
    scope.release();
    if (!context) {
        call(globalObject, method, arguments);
        return;
    }
    arguments.append(context);
    callWithKeywords(globalObject, method, arguments, asyncioState(globalObject).contextKeyword.get());
}

void registerTask(JSGlobalObject* globalObject, JSValue task)
{
    callMethodNamed(globalObject, asyncioState(globalObject).scheduledTasks.get(), named(globalObject->vm(), "add"_s), task);
}

void unregisterTask(JSGlobalObject* globalObject, JSValue task)
{
    callMethodNamed(globalObject, asyncioState(globalObject).scheduledTasks.get(), named(globalObject->vm(), "discard"_s), task);
}

// ---- Future

JSValue createCancelledError(JSGlobalObject*, PyStateObject*);

// It is done, and JavaScript is told at once, not when the loop next comes round.
void settleForJavaScript(JSGlobalObject* globalObject, PyStateObject* cell)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Future& future = futureIn(cell);
    if (!future.promise && !future.settlesAwaitable) [[likely]]
        return;
    JSValue thrown = future.exception.get();
    if (future.status == Status::Cancelled) {
        thrown = createCancelledError(globalObject, cell);
        RETURN_IF_EXCEPTION(scope, void());
    }
    JSValue result = thrown ? JSValue() : future.result.get();
    if (std::exchange(future.settlesAwaitable, false))
        settleAwaitable(globalObject, asObject(future.coroutine.get()), result, thrown);
    if (JSValue promise = future.promise.get()) {
        if (thrown)
            uncheckedDowncast<JSPromise>(promise.asCell())->reject(vm, thrown);
        else
            uncheckedDowncast<JSPromise>(promise.asCell())->resolve(globalObject, vm, result);
    }
}

void scheduleCallbacks(JSGlobalObject* globalObject, PyStateObject* cell)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Future& future = futureIn(cell);
    ASSERT(future.status != Status::Pending);
    settleForJavaScript(globalObject, cell);
    RETURN_IF_EXCEPTION(scope, void());
    // It is done, so it is no longer among those that are to be.
    if (isTask(globalObject, cell)) {
        unregisterTask(globalObject, cell);
        RETURN_IF_EXCEPTION(scope, void());
    }
    // A call_soon() that is up to no good could change any of these. They are taken away first, so that it cannot.
    if (JSValue callback = future.callback0.get()) {
        JSValue context = future.context0.get();
        future.callback0.clear();
        future.context0.clear();
        callSoon(globalObject, future.loop.get(), callback, cell, context);
        if (scope.exception()) [[unlikely]] {
            future.callbacks.clear();
            return;
        }
    }
    JSValue callbacks = future.callbacks.get();
    if (!callbacks)
        return;
    future.callbacks.clear();
    JSArray* list = asList(callbacks);
    for (unsigned i = 0, count = list->length(); i < count; ++i) {
        auto* pair = uncheckedDowncast<PyTuple>(list->getIndexQuickly(i).asCell());
        callSoon(globalObject, future.loop.get(), pair->at(0), cell, pair->at(1));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void initializeFuture(JSGlobalObject* globalObject, PyStateObject* cell, JSValue loop)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Future& future = futureIn(cell);
    future.loop.clear();
    future.callback0.clear();
    future.context0.clear();
    future.callbacks.clear();
    future.result.clear();
    future.exception.clear();
    future.exceptionTraceback.clear();
    future.sourceTraceback.clear();
    future.cancelMessage.clear();
    future.cancelledException.clear();
    future.awaitedBy.clear();
    future.status = Status::Pending;
    future.logsTraceback = false;
    future.isBlocking = false;
    future.awaitedByIsSet = false;

    if (isNone(loop)) {
        loop = getEventLoop(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
    }
    future.loop.set(vm, cell, loop);

    JSValue debugs = callMethodNamed(globalObject, loop, named(vm, "get_debug"_s));
    RETURN_IF_EXCEPTION(scope, void());
    bool isDebugging = isTrue(globalObject, debugs);
    RETURN_IF_EXCEPTION(scope, void());
    if (isDebugging) {
        JSValue stack = call(globalObject, asyncioState(globalObject).extractStack.get());
        RETURN_IF_EXCEPTION(scope, void());
        future.sourceTraceback.set(vm, cell, stack);
    }
}

void addToAwaitedBy(JSGlobalObject* globalObject, PyStateObject* cell, JSValue thing)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Future& future = futureIn(cell);
    // Most are awaited by one thing, and there is no set until there is a second.
    if (!future.awaitedBy) {
        future.awaitedBy.set(vm, cell, thing);
        return;
    }
    if (future.awaitedByIsSet) {
        scope.release();
        uncheckedDowncast<PySet>(future.awaitedBy.get().asCell())->add(globalObject, thing);
        return;
    }
    PySet* set = PySet::create(globalObject);
    set->add(globalObject, thing);
    RETURN_IF_EXCEPTION(scope, void());
    set->add(globalObject, future.awaitedBy.get());
    RETURN_IF_EXCEPTION(scope, void());
    future.awaitedBy.set(vm, cell, set);
    future.awaitedByIsSet = true;
}

void discardFromAwaitedBy(JSGlobalObject* globalObject, PyStateObject* cell, JSValue thing)
{
    Future& future = futureIn(cell);
    if (!future.awaitedBy)
        return;
    if (isIdentical(future.awaitedBy.get(), thing)) {
        future.awaitedBy.clear();
        return;
    }
    if (future.awaitedByIsSet)
        uncheckedDowncast<PySet>(future.awaitedBy.get().asCell())->remove(globalObject, thing);
}

void setFutureResult(JSGlobalObject* globalObject, PyStateObject* cell, JSValue result)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Future& future = futureIn(cell);
    if (!ensureAlive(globalObject, scope, future))
        return;
    if (future.status != Status::Pending) {
        raiseInvalidState(globalObject, scope, "invalid state"_s);
        return;
    }
    future.result.set(vm, cell, result);
    future.status = Status::Finished;
    RELEASE_AND_RETURN(scope, scheduleCallbacks(globalObject, cell));
}

void setFutureException(JSGlobalObject* globalObject, PyStateObject* cell, JSValue given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& names = vm.pythonNames();
    Future& future = futureIn(cell);
    if (future.status != Status::Pending) {
        raiseInvalidState(globalObject, scope, "invalid state"_s);
        return;
    }
    JSValue exception = given;
    if (isClass(given) && asType(given)->isSubtypeOf(realm->typeBaseException())) {
        exception = call(globalObject, given);
        RETURN_IF_EXCEPTION(scope, void());
        if (future.status != Status::Pending) {
            raiseInvalidState(globalObject, scope, "invalid state"_s);
            return;
        }
    }
    if (!isInstance(globalObject, exception, realm->typeBaseException())) {
        raiseTypeError(globalObject, scope, "invalid exception object"_s);
        return;
    }
    if (isInstance(globalObject, exception, realm->typeStopIteration())) {
        JSObject* error = createException(globalObject, realm->typeRuntimeError(), jsNontrivialString(vm, "StopIteration interacts badly with generators and cannot be raised into a Future"_s));
        RETURN_IF_EXCEPTION(scope, void());
        // PyException_SetCause() and PyException_SetContext()
        error->putDirect(vm, names.private_cause, exception);
        error->putDirect(vm, names.private_suppressContext, jsBoolean(true));
        error->putDirect(vm, names.private_context, exception);
        exception = error;
    }
    future.exception.set(vm, cell, exception);
    if (JSValue traceback = asObject(exception)->getDirect(vm, names.private_traceback); traceback && !isNone(traceback))
        future.exceptionTraceback.set(vm, cell, traceback);
    future.status = Status::Finished;
    // JavaScript says for itself when nothing has dealt with a rejection.
    bool isForJavaScript = future.promise || future.settlesAwaitable;
    scheduleCallbacks(globalObject, cell);
    RETURN_IF_EXCEPTION(scope, void());
    future.logsTraceback = !isForJavaScript;
}

JSValue createCancelledError(JSGlobalObject* globalObject, PyStateObject* cell)
{
    Future& future = futureIn(cell);
    if (JSValue exception = future.cancelledException.get()) {
        future.cancelledException.clear();
        return exception;
    }
    JSValue exceptionClass = asyncioState(globalObject).cancelledError.get();
    JSValue message = future.cancelMessage.get();
    return !message || isNone(message) ? call(globalObject, exceptionClass) : call(globalObject, exceptionClass, message);
}

void raiseCancelledError(JSGlobalObject* globalObject, PyStateObject* cell)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue exception = createCancelledError(globalObject, cell);
    RETURN_IF_EXCEPTION(scope, void());
    raiseException(globalObject, scope, exception);
}

// future_get_result(). Nothing if it raised. Otherwise whether what is given is an exception, that is to be raised.
std::optional<bool> getFutureResult(JSGlobalObject* globalObject, PyStateObject* cell, JSValue& result)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Future& future = futureIn(cell);
    if (future.status == Status::Cancelled) {
        scope.release();
        raiseCancelledError(globalObject, cell);
        return std::nullopt;
    }
    if (future.status != Status::Finished) {
        raiseInvalidState(globalObject, scope, "Result is not set."_s);
        return std::nullopt;
    }
    future.logsTraceback = false;
    if (JSValue exception = future.exception.get()) {
        if (isInstance(globalObject, exception, globalObject->pyRealm()->typeBaseException()))
            asObject(exception)->putDirect(vm, vm.pythonNames().private_traceback, orNone(future.exceptionTraceback.get()));
        future.exceptionTraceback.clear();
        result = exception;
        return true;
    }
    result = future.result.get();
    return false;
}

// Future.result()
JSValue futureResult(JSGlobalObject* globalObject, PyStateObject* cell)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!futureIn(cell).isAlive())
        return raiseInvalidState(globalObject, scope, "Future object is not initialized."_s);
    JSValue result;
    auto isException = getFutureResult(globalObject, cell, result);
    RETURN_IF_EXCEPTION(scope, { });
    if (!*isException)
        return result;
    return raiseException(globalObject, scope, result);
}

void addDoneCallback(JSGlobalObject* globalObject, PyStateObject* cell, JSValue callback, JSValue context)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Future& future = futureIn(cell);
    if (!future.isAlive()) {
        raise(globalObject, scope, BuiltinType::RuntimeError, "uninitialized Future object"_s);
        return;
    }
    if (future.status != Status::Pending)
        RELEASE_AND_RETURN(scope, callSoon(globalObject, future.loop.get(), callback, cell, context));
    // The first is kept by itself, and there is no list until there is a second.
    if (!future.callbacks && !future.callback0) {
        future.callback0.set(vm, cell, callback);
        future.context0.set(vm, cell, context);
        return;
    }
    JSValue pair = PyTuple::create(globalObject, { callback, context });
    if (JSValue callbacks = future.callbacks.get()) {
        scope.release();
        listAppend(globalObject, asList(callbacks), pair);
        return;
    }
    MarkedArgumentBuffer items;
    items.append(pair);
    future.callbacks.set(vm, cell, newList(globalObject, items));
}

// Whether it was.
bool cancelFuture(JSGlobalObject* globalObject, PyStateObject* cell, JSValue message)
{
    VM& vm = globalObject->vm();
    Future& future = futureIn(cell);
    future.logsTraceback = false;
    if (future.status != Status::Pending)
        return false;
    future.status = Status::Cancelled;
    if (message)
        future.cancelMessage.set(vm, cell, message);
    else
        future.cancelMessage.clear();
    scheduleCallbacks(globalObject, cell);
    return true;
}

} // anonymous namespace

// PyType_GenericNew()
PYTHON_NATIVE(futureNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<Future>()));
}

PYTHON_NATIVE(futureInit)
{
    NATIVE_PROLOGUE();
    initializeFuture(globalObject, asFutureCell(args[0]), orNone(args.at(1)));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(futureResultMethod)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(futureResult(globalObject, asFutureCell(args[0]))));
}

PYTHON_NATIVE(futureException)
{
    NATIVE_PROLOGUE();
    PyStateObject* cell = asFutureCell(args[0]);
    Future& future = futureIn(cell);
    if (!future.isAlive())
        return JSValue::encode(raiseInvalidState(globalObject, scope, "Future object is not initialized."_s));
    if (future.status == Status::Cancelled) {
        scope.release();
        raiseCancelledError(globalObject, cell);
        return { };
    }
    if (future.status != Status::Finished)
        return JSValue::encode(raiseInvalidState(globalObject, scope, "Exception is not set."_s));
    if (JSValue exception = future.exception.get()) {
        future.logsTraceback = false;
        return JSValue::encode(exception);
    }
    RETURN_NONE();
}

PYTHON_NATIVE(futureSetResult)
{
    NATIVE_PROLOGUE();
    setFutureResult(globalObject, asFutureCell(args[0]), args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(futureSetException)
{
    NATIVE_PROLOGUE();
    PyStateObject* cell = asFutureCell(args[0]);
    if (!ensureAlive(globalObject, scope, futureIn(cell)))
        return { };
    setFutureException(globalObject, cell, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(futureAddDoneCallback)
{
    NATIVE_PROLOGUE();
    JSValue context = args.at(2);
    if (!context)
        context = copyCurrentContext(globalObject);
    addDoneCallback(globalObject, asFutureCell(args[0]), args[1], context);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(futureRemoveDoneCallback)
{
    NATIVE_PROLOGUE();
    PyStateObject* cell = asFutureCell(args[0]);
    Future& future = futureIn(cell);
    JSValue function = args[1];
    if (!ensureAlive(globalObject, scope, future))
        return { };
    // What is asked whether it is equal can do anything to the future meanwhile.
    unsigned clearedFirst = 0;
    if (JSValue callback = future.callback0.get()) {
        bool isIt = isEqual(globalObject, callback, function);
        RETURN_IF_EXCEPTION(scope, { });
        if (isIt) {
            future.callback0.clear();
            future.context0.clear();
            clearedFirst = 1;
        }
    }
    if (!future.callbacks)
        return JSValue::encode(jsNumber(static_cast<int32_t>(clearedFirst)));
    unsigned length = asList(future.callbacks.get())->length();
    if (!length) {
        future.callbacks.clear();
        return JSValue::encode(jsNumber(static_cast<int32_t>(clearedFirst)));
    }
    MarkedArgumentBuffer kept;
    for (unsigned i = 0; future.callbacks && i < asList(future.callbacks.get())->length(); ++i) {
        JSValue item = asList(future.callbacks.get())->getIndexQuickly(i);
        bool isIt = isEqual(globalObject, uncheckedDowncast<PyTuple>(item.asCell())->at(0), function);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isIt)
            kept.append(item);
    }
    if (kept.isEmpty() || !future.callbacks) {
        future.callbacks.clear();
        return JSValue::encode(jsNumber(static_cast<int32_t>(length + clearedFirst)));
    }
    length = asList(future.callbacks.get())->length();
    if (kept.size() != length)
        future.callbacks.set(vm, cell, newList(globalObject, kept));
    return JSValue::encode(jsNumber(static_cast<int32_t>(length - kept.size() + clearedFirst)));
}

PYTHON_NATIVE(futureCancel)
{
    NATIVE_PROLOGUE();
    PyStateObject* cell = asFutureCell(args[0]);
    if (!ensureAlive(globalObject, scope, futureIn(cell)))
        return { };
    bool wasCancelled = cancelFuture(globalObject, cell, orNone(args.at(1)));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(wasCancelled));
}

PYTHON_NATIVE(futureCancelled)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    Future& future = futureIn(asFutureCell(args[0]));
    return JSValue::encode(jsBoolean(future.isAlive() && future.status == Status::Cancelled));
}

PYTHON_NATIVE(futureDone)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    Future& future = futureIn(asFutureCell(args[0]));
    return JSValue::encode(jsBoolean(future.isAlive() && future.status != Status::Pending));
}

PYTHON_NATIVE(futureGetLoop)
{
    NATIVE_PROLOGUE();
    Future& future = futureIn(asFutureCell(args[0]));
    if (!ensureAlive(globalObject, scope, future))
        return { };
    return JSValue::encode(future.loop.get());
}

PYTHON_NATIVE(futureMakeCancelledError)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(createCancelledError(globalObject, asFutureCell(args[0]))));
}

PYTHON_NATIVE(futureReprMethod)
{
    NATIVE_PROLOGUE();
    if (!ensureAlive(globalObject, scope, futureIn(asFutureCell(args[0]))))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, asyncioState(globalObject).futureRepr.get(), args[0])));
}

namespace {

// loop.call_exception_handler(context), whatever comes of it, with what has been raised already left as it is.
void callExceptionHandler(JSGlobalObject* globalObject, JSValue loop, PyDict* context)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue function = getAttribute(globalObject, loop, named(vm, "call_exception_handler"_s));
    if (scope.exception()) {
        (void)scope.tryClearException();
        return;
    }
    call(globalObject, function, context);
    if (scope.exception())
        reportUnraisableShowing(globalObject, "Exception ignored while calling asyncio function"_s, function);
}

// FutureObj_finalize()
void finalizeFuture(JSGlobalObject* globalObject, PyStateObject* cell)
{
    VM& vm = globalObject->vm();
    Future& future = futureIn(cell);
    if (!future.logsTraceback)
        return;
    future.logsTraceback = false;
    Exception* raised = takeRaisedException(vm);
    PyDict* context = PyDict::create(globalObject);
    context->setString(globalObject, "message"_s, jsString(vm, concatenate(typeOf(globalObject, cell)->nameWithoutModule(globalObject), " exception was never retrieved"_s)));
    context->setString(globalObject, "exception"_s, future.exception.get());
    context->setString(globalObject, "future"_s, cell);
    if (JSValue source = future.sourceTraceback.get())
        context->setString(globalObject, "source_traceback"_s, source);
    callExceptionHandler(globalObject, future.loop.get(), context);
    if (raised)
        restoreRaisedException(globalObject, raised);
}

JSValue getAwaitedBy(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Future& future = futureIn(asFutureCell(self));
    if (!future.awaitedBy)
        return jsUndefined();
    JSValue frozenSet = globalObject->pyRealm()->typeFrozenSet();
    if (future.awaitedByIsSet)
        RELEASE_AND_RETURN(scope, call(globalObject, frozenSet, future.awaitedBy.get()));
    RELEASE_AND_RETURN(scope, call(globalObject, frozenSet, PyTuple::create(globalObject, { future.awaitedBy.get() })));
}

JSValue getBlocking(JSGlobalObject*, JSValue self)
{
    Future& future = futureIn(asFutureCell(self));
    return jsBoolean(future.isAlive() && future.isBlocking);
}

// What a setter that takes true or false is given. Nothing if it raised.
std::optional<bool> flagToSet(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    if (!value) {
        raise(globalObject, scope, BuiltinType::AttributeError, "cannot delete attribute"_s);
        return std::nullopt;
    }
    bool flag = isTrue(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return flag;
}

void setBlocking(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Future& future = futureIn(asFutureCell(self));
    if (!ensureAlive(globalObject, scope, future))
        return;
    if (auto flag = flagToSet(globalObject, scope, value))
        future.isBlocking = *flag;
}

JSValue getLogTraceback(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Future& future = futureIn(asFutureCell(self));
    if (!ensureAlive(globalObject, scope, future))
        return { };
    return jsBoolean(future.logsTraceback);
}

void setLogTraceback(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto flag = flagToSet(globalObject, scope, value);
    if (!flag)
        return;
    if (*flag) {
        raiseValueError(globalObject, scope, "_log_traceback can only be set to False"_s);
        return;
    }
    futureIn(asFutureCell(self)).logsTraceback = false;
}

JSValue getLoopOrNone(JSGlobalObject*, JSValue self) { return orNone(futureIn(asFutureCell(self)).loop.get()); }

JSValue getCallbacks(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Future& future = futureIn(asFutureCell(self));
    if (!ensureAlive(globalObject, scope, future))
        return { };
    MarkedArgumentBuffer all;
    if (JSValue callback = future.callback0.get())
        all.append(PyTuple::create(globalObject, { callback, future.context0.get() }));
    if (JSValue callbacks = future.callbacks.get()) {
        JSArray* list = asList(callbacks);
        for (unsigned i = 0; i < list->length(); ++i)
            all.append(list->getIndexQuickly(i));
    }
    if (all.isEmpty())
        return jsUndefined();
    return newList(globalObject, all);
}

template<WriteBarrier<Unknown> Future::* field>
JSValue getIfAlive(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Future& future = futureIn(asFutureCell(self));
    if (!ensureAlive(globalObject, scope, future))
        return { };
    return orNone((future.*field).get());
}

template<WriteBarrier<Unknown> Future::* field>
JSValue getOrNone(JSGlobalObject*, JSValue self) { return orNone((futureIn(asFutureCell(self)).*field).get()); }

void setCancelMessage(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raise(globalObject, scope, BuiltinType::AttributeError, "cannot delete attribute"_s);
        return;
    }
    futureIn(asFutureCell(self)).cancelMessage.set(vm, asFutureCell(self), value);
}

JSValue getState(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Future& future = futureIn(asFutureCell(self));
    if (!ensureAlive(globalObject, scope, future))
        return { };
    switch (future.status) {
    case Status::Pending:
        return jsNontrivialString(vm, "PENDING"_s);
    case Status::Cancelled:
        return jsNontrivialString(vm, "CANCELLED"_s);
    case Status::Finished:
        return jsNontrivialString(vm, "FINISHED"_s);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- What `await future` goes through

struct FutureIterator final : NativeState {
    PYTHON_NATIVE_STATE(FutureIterator);
    WriteBarrier<Unknown> future;
};

template<typename Visitor> void FutureIterator::visit(Visitor& visitor) { visitor.append(future); }

} // anonymous namespace

PYTHON_NATIVE(futureDelete)
{
    NATIVE_PROLOGUE();
    finalizeFuture(globalObject, asFutureCell(args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// future_new_iter()
PYTHON_NATIVE(futureIter)
{
    NATIVE_PROLOGUE();
    if (!ensureAlive(globalObject, scope, futureIn(asFutureCell(args[0]))))
        return { };
    auto* iterator = PyStateObject::create(vm, asyncioState(globalObject).futureIteratorType->instanceStructure(), makeUnique<FutureIterator>());
    iterator->state<FutureIterator>().future.set(vm, iterator, args[0]);
    return JSValue::encode(iterator);
}

// FutureIter_iternext(), and send(), which makes nothing of what it is sent.
PYTHON_NATIVE(futureIteratorNext)
{
    NATIVE_PROLOGUE();
    JSValue value = stateOf<FutureIterator>(args[0]).future.get();
    // It has been closed, or thrown into.
    if (!value)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    PyStateObject* cell = asFutureCell(value);
    Future& future = futureIn(cell);
    if (future.status == Status::Pending) {
        if (!future.isBlocking) {
            future.isBlocking = true;
            return JSValue::encode(cell);
        }
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "await wasn't used with future"_s));
    }
    JSValue result = futureResult(globalObject, cell);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, result));
}

PYTHON_NATIVE(futureIteratorThrow)
{
    NATIVE_PROLOGUE();
    PyRealm* theRealm = realm;
    unsigned count = args.size() - 1;
    if (count < 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, "throw expected at least 1 argument, got 0"_s));
    if (count > 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("throw expected at most 3 arguments, got "_s, count)));
    if (count > 1 && !warn(globalObject, BuiltinType::DeprecationWarning, "the (type, exc, tb) signature of throw() is deprecated, use the single-arg signature instead."_s))
        return { };
    JSValue type = args[1];
    JSValue value = count > 1 && !isNone(args[2]) ? args[2] : JSValue();
    JSValue traceback = count > 2 && !isNone(args[3]) ? args[3] : JSValue();
    if (traceback && typeOf(globalObject, traceback) != theRealm->typeTraceback())
        return JSValue::encode(raiseTypeError(globalObject, scope, "throw() third argument must be a traceback"_s));
    bool isExceptionClass = isClass(type) && asType(type)->isSubtypeOf(theRealm->typeBaseException());
    if (!isExceptionClass) {
        if (!isInstance(globalObject, type, theRealm->typeBaseException()))
            return JSValue::encode(raiseTypeError(globalObject, scope, "exceptions must be classes deriving BaseException or instances of such a class"_s));
        if (value)
            return JSValue::encode(raiseTypeError(globalObject, scope, "instance exception may not have a separate value"_s));
    }
    JSValue exception = exceptionToThrow(globalObject, type, value, traceback);
    RETURN_IF_EXCEPTION(scope, { });
    stateOf<FutureIterator>(args[0]).future.clear();
    throwException(globalObject, scope, exception);
    return { };
}

PYTHON_NATIVE(futureIteratorClose)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    stateOf<FutureIterator>(args[0]).future.clear();
    RETURN_NONE();
}

PYTHON_NATIVE(returnSelf)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(args[0]);
}

// ---- Task

namespace {

// TaskStepMethWrapper: what is given to call_soon() to take a task a step further.
struct StepWrapper final : NativeState {
    PYTHON_NATIVE_STATE(StepWrapper);
    WriteBarrier<Unknown> task;
    WriteBarrier<Unknown> argument;
};

template<typename Visitor>
void StepWrapper::visit(Visitor& visitor)
{
    visitor.append(task);
    visitor.append(argument);
}

void raiseNotRunningLoop(JSGlobalObject* globalObject, ThrowScope& scope, JSValue loop)
{
    String shown = repr(globalObject, loop);
    RETURN_IF_EXCEPTION(scope, void());
    raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("loop "_s, shown, " is not the running loop"_s));
}

void enterTask(JSGlobalObject* globalObject, JSValue loop, JSValue task)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = asyncioState(globalObject);
    if (!isIdentical(orNone(state.runningLoop.get()), loop))
        RELEASE_AND_RETURN(scope, raiseNotRunningLoop(globalObject, scope, loop));
    if (JSValue running = state.runningTask.get()) {
        String entering = repr(globalObject, task);
        RETURN_IF_EXCEPTION(scope, void());
        String other = repr(globalObject, running);
        RETURN_IF_EXCEPTION(scope, void());
        raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("Cannot enter into task "_s, entering, " while another task "_s, other, " is being executed."_s));
        return;
    }
    state.runningTask.set(vm, globalObject->pyRealm(), task);
}

void leaveTask(JSGlobalObject* globalObject, JSValue loop, JSValue task)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& state = asyncioState(globalObject);
    if (!isIdentical(orNone(state.runningLoop.get()), loop))
        RELEASE_AND_RETURN(scope, raiseNotRunningLoop(globalObject, scope, loop));
    if (!isIdentical(orNone(state.runningTask.get()), task) || !state.runningTask) {
        String leaving = repr(globalObject, task);
        RETURN_IF_EXCEPTION(scope, void());
        String other = repr(globalObject, orNone(state.runningTask.get()));
        RETURN_IF_EXCEPTION(scope, void());
        raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("Invalid attempt to leave task "_s, leaving, " while task "_s, other, " is entered."_s));
        return;
    }
    state.runningTask.clear();
}

// The one that was, or None.
JSValue swapCurrentTask(JSGlobalObject* globalObject, JSValue loop, JSValue task)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = asyncioState(globalObject);
    if (!isIdentical(orNone(state.runningLoop.get()), loop)) {
        scope.release();
        raiseNotRunningLoop(globalObject, scope, loop);
        return { };
    }
    JSValue previous = orNone(state.runningTask.get());
    if (isNone(task))
        state.runningTask.clear();
    else
        state.runningTask.set(vm, globalObject->pyRealm(), task);
    return previous;
}

// is_coroutine(). Nothing if it raised.
std::optional<bool> isCoroutine(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& state = asyncioState(globalObject);
    PyType* type = typeOf(globalObject, value);
    if (type == globalObject->pyRealm()->typeCoroutine())
        return true;
    // asyncio.iscoroutine() remembers too, but it is a function that is written in Python and takes calling.
    bool isKnown = state.coroutineTypes->find(globalObject, type->object()) >= 0;
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (isKnown)
        return true;
    JSValue answer = call(globalObject, state.isCoroutineFunction.get(), value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    bool isOne = isTrue(globalObject, answer);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (isOne && state.coroutineTypes->size() < 100) {
        state.coroutineTypes->add(globalObject, type->object());
        RETURN_IF_EXCEPTION(scope, std::nullopt);
    }
    return isOne;
}

void callStepSoon(JSGlobalObject* globalObject, PyStateObject* task, JSValue argument)
{
    VM& vm = globalObject->vm();
    auto* wrapper = PyStateObject::create(vm, asyncioState(globalObject).stepWrapperType->instanceStructure(), makeUnique<StepWrapper>());
    wrapper->state<StepWrapper>().task.set(vm, wrapper, task);
    if (argument)
        wrapper->state<StepWrapper>().argument.set(vm, wrapper, argument);
    callSoon(globalObject, futureIn(task).loop.get(), wrapper, JSValue(), futureIn(task).context.get());
}

// task_set_error_soon(): the next step is to have RuntimeError thrown into it.
void setErrorSoon(JSGlobalObject* globalObject, PyStateObject* task, const String& message)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSObject* error = createException(globalObject, globalObject->pyRealm()->typeRuntimeError(), jsString(globalObject->vm(), message));
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, callStepSoon(globalObject, task, error));
}

// waiter.cancel(msg), if the task is to be cancelled, which it then need not be.
void cancelWaiterIfMustCancel(JSGlobalObject* globalObject, PyStateObject* task, JSValue waiter)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Future& state = futureIn(task);
    if (!state.mustCancel)
        return;
    JSValue answer = callMethodNamed(globalObject, waiter, named(vm, "cancel"_s), orNone(state.cancelMessage.get()));
    RETURN_IF_EXCEPTION(scope, void());
    bool wasCancelled = isTrue(globalObject, answer);
    RETURN_IF_EXCEPTION(scope, void());
    if (wasCancelled)
        state.mustCancel = false;
}

// CPython has nothing of the kind. `await promise` yields the promise to whatever is running the coroutine, which is to send back what it comes to, or throw in what it is rejected with.
//
// The task waits for a future that stands for the promise, as it would for any. So it can be cancelled while it waits, though a promise cannot: the future is, and what the promise comes to is then nobody's business.
// The promise is settled whenever JavaScript settles it, which is likely to be while the loop is waiting for something to do. So the loop is told as it would be from another thread, which wakes it.
void waitForPromise(JSGlobalObject* globalObject, PyStateObject* task, JSValue thenable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& module = asyncioState(globalObject);
    Future& state = futureIn(task);
    auto* standIn = PyStateObject::create(vm, module.futureType->instanceStructure(), makeUnique<Future>());
    initializeFuture(globalObject, standIn, state.loop.get());
    RETURN_IF_EXCEPTION(scope, void());
    futureIn(standIn).standsForPromise = true;
    addToAwaitedBy(globalObject, standIn, task);
    RETURN_IF_EXCEPTION(scope, void());
    addDoneCallback(globalObject, standIn, PyBoundMethod::create(globalObject, module.wakeup.get(), task), state.context.get());
    RETURN_IF_EXCEPTION(scope, void());
    state.waiter.set(vm, task, standIn);

    JSPromise* promise = JSPromise::resolvedPromise(globalObject, thenable);
    RETURN_IF_EXCEPTION(scope, void());
    promise->performPromiseThenExported(vm, globalObject, PyBoundMethod::create(globalObject, module.promiseFulfilled.get(), standIn), PyBoundMethod::create(globalObject, module.promiseRejected.get(), standIn), jsUndefined());
    RELEASE_AND_RETURN(scope, cancelWaiterIfMustCancel(globalObject, task, standIn));
}

// task_step_handle_result_impl(): the coroutine has yielded something, which is what it is waiting for.
void handleYielded(JSGlobalObject* globalObject, PyStateObject* task, JSValue yielded)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& module = asyncioState(globalObject);
    Future& state = futureIn(task);
    auto complain = [&] (ASCIILiteral before, bool showsTask, ASCIILiteral between, bool showsYielded, ASCIILiteral after = ""_s) {
        String taskShown = showsTask ? repr(globalObject, task) : emptyString();
        RETURN_IF_EXCEPTION(scope, void());
        String yieldedShown = showsYielded ? repr(globalObject, yielded) : emptyString();
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, setErrorSoon(globalObject, task, concatenate(before, taskShown, between, yieldedShown, after)));
    };
    auto complainOfDifferentLoop = [&] { complain("Task "_s, true, " got Future "_s, true, " attached to a different loop"_s); };
    auto complainOfYield = [&] { complain("yield was used instead of yield from in task "_s, true, " with "_s, true); };

    if (yielded == JSValue(task))
        return complain("Task cannot await on itself: "_s, true, ""_s, false);

    if (isExactFutureOrTask(globalObject, yielded)) {
        PyStateObject* cell = asFutureCell(yielded);
        Future& future = futureIn(cell);
        if (!isIdentical(future.loop.get(), state.loop.get()))
            return complainOfDifferentLoop();
        if (!future.isBlocking)
            return complainOfYield();
        addToAwaitedBy(globalObject, cell, task);
        RETURN_IF_EXCEPTION(scope, void());
        future.isBlocking = false;
        addDoneCallback(globalObject, cell, PyBoundMethod::create(globalObject, module.wakeup.get(), task), state.context.get());
        RETURN_IF_EXCEPTION(scope, void());
        state.waiter.set(vm, task, yielded);
        RELEASE_AND_RETURN(scope, cancelWaiterIfMustCancel(globalObject, task, yielded));
    }

    // A bare yield gives up its turn.
    if (isNone(yielded))
        RELEASE_AND_RETURN(scope, callStepSoon(globalObject, task, JSValue()));

    JSValue blocking = getAttributeIfPresent(globalObject, yielded, named(vm, "_asyncio_future_blocking"_s));
    RETURN_IF_EXCEPTION(scope, void());
    if (blocking && !isNone(blocking)) {
        // It is something that will do for a future.
        bool isBlocking = isTrue(globalObject, blocking);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue loop = loopOfFuture(globalObject, yielded);
        RETURN_IF_EXCEPTION(scope, void());
        if (!isIdentical(loop, state.loop.get()))
            return complainOfDifferentLoop();
        if (!isBlocking)
            return complainOfYield();
        if (isFutureOrTask(globalObject, yielded)) {
            addToAwaitedBy(globalObject, asFutureCell(yielded), task);
            RETURN_IF_EXCEPTION(scope, void());
        }
        setAttribute(globalObject, yielded, named(vm, "_asyncio_future_blocking"_s), jsBoolean(false));
        RETURN_IF_EXCEPTION(scope, void());
        JSValue add = getAttribute(globalObject, yielded, named(vm, "add_done_callback"_s));
        RETURN_IF_EXCEPTION(scope, void());
        MarkedArgumentBuffer arguments;
        arguments.append(PyBoundMethod::create(globalObject, module.wakeup.get(), task));
        arguments.append(state.context.get());
        callWithKeywords(globalObject, add, arguments, module.contextKeyword.get());
        RETURN_IF_EXCEPTION(scope, void());
        state.waiter.set(vm, task, yielded);
        RELEASE_AND_RETURN(scope, cancelWaiterIfMustCancel(globalObject, task, yielded));
    }

    if (typeOf(globalObject, yielded)->hasFlag(PyType::IsJavaScript)) {
        bool isWaitedFor = isThenable(globalObject, yielded);
        RETURN_IF_EXCEPTION(scope, void());
        if (isWaitedFor)
            RELEASE_AND_RETURN(scope, waitForPromise(globalObject, task, yielded));
    }

    if (isInstance(globalObject, yielded, globalObject->pyRealm()->typeGenerator()))
        return complain("yield was used instead of yield from for generator in task "_s, true, " with "_s, true);
    complain("Task got bad yield: "_s, false, ""_s, true);
}

void concludeStep(JSGlobalObject*, PyStateObject* task, JSValue yielded, JSValue returned);

// What a promise came to, for the coroutine that was waiting for it.
struct Outcome {
    JSValue value { jsUndefined() };
    bool wasRejected { false };
};

// task_step_impl(). `exception` is empty if there is nothing to throw into it.
void stepTaskEntered(JSGlobalObject* globalObject, PyStateObject* task, JSValue exception, Outcome outcome = { })
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& module = asyncioState(globalObject);
    Future& state = futureIn(task);

    if (state.status != Status::Pending) {
        String taskShown = repr(globalObject, task);
        RETURN_IF_EXCEPTION(scope, void());
        String exceptionShown = repr(globalObject, orNone(exception));
        RETURN_IF_EXCEPTION(scope, void());
        raiseOf(globalObject, scope, module.invalidStateError.get(), concatenate("__step(): already done: "_s, taskShown, ' ', exceptionShown));
        return;
    }
    if (state.mustCancel) {
        if (!exception || !matches(globalObject, exception, module.cancelledError.get())) {
            exception = createCancelledError(globalObject, task);
            RETURN_IF_EXCEPTION(scope, void());
        }
        state.mustCancel = false;
    }
    state.waiter.clear();
    JSValue coroutine = state.coroutine.get();
    if (!coroutine) {
        raise(globalObject, scope, BuiltinType::RuntimeError, "uninitialized Task object"_s);
        return;
    }

    // What it yielded, or else what it returned, or else it raised.
    JSValue yielded;
    JSValue returned;
    // A promise can be rejected with anything. What is an exception is thrown in as any is.
    if (!exception && outcome.wasRejected && isInstance(globalObject, outcome.value, realm->typeBaseException()))
        exception = outcome.value;
    if (!exception) {
        // PyIter_Send(coroutine, None)
        if (outcome.wasRejected || coroutine.asCell()->type() == JSGeneratorType || (isNone(outcome.value) && typeOf(globalObject, coroutine)->lookup(vm, vm.pythonNames().dunder_next)))
            yielded = stepIterator(globalObject, coroutine, outcome.value, outcome.wasRejected, returned);
        else {
            yielded = callMethodNamed(globalObject, coroutine, named(vm, "send"_s), outcome.value);
            if (scope.exception())
                returned = catchStopIteration(globalObject);
        }
    } else {
        yielded = callMethodNamed(globalObject, coroutine, named(vm, "throw"_s), exception);
        if (scope.exception())
            returned = catchStopIteration(globalObject);
    }

    RELEASE_AND_RETURN(scope, concludeStep(globalObject, task, yielded, returned));
}

// The rest of task_step_impl(): the coroutine has yielded, or returned, or else it has raised.
void concludeStep(JSGlobalObject* globalObject, PyStateObject* task, JSValue yielded, JSValue returned)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& module = asyncioState(globalObject);
    Future& state = futureIn(task);

    if (returned) {
        if (state.mustCancel) {
            // It was cancelled just as it was coming to its end.
            state.mustCancel = false;
            scope.release();
            cancelFuture(globalObject, task, state.cancelMessage.get());
            return;
        }
        RELEASE_AND_RETURN(scope, setFutureResult(globalObject, task, returned));
    }
    if (scope.exception()) {
        Exception* taken = takeRaisedException(vm);
        if (!taken)
            return;
        JSValue raised = exceptionValue(globalObject, taken->value());
        if (matches(globalObject, raised, module.cancelledError.get())) {
            state.cancelledException.set(vm, task, raised);
            scope.release();
            cancelFuture(globalObject, task, JSValue());
            return;
        }
        // JavaScript can throw what is no exception: a promise can be rejected with anything. `except` does not catch it, and it has come all the way out. It is what the task ended with, as it is, and is thrown
        // at whatever asks for the result. If the task were left as it was, whatever is waiting for it would wait for ever.
        if (!isInstance(globalObject, raised, realm->typeBaseException())) {
            state.exception.set(vm, task, raised);
            state.status = Status::Finished;
            RELEASE_AND_RETURN(scope, scheduleCallbacks(globalObject, task));
        }
        setFutureException(globalObject, task, raised);
        RETURN_IF_EXCEPTION(scope, void());
        if (isInstance(globalObject, raised, realm->typeKeyboardInterrupt()) || isInstance(globalObject, raised, realm->typeSystemExit()))
            restoreRaisedException(globalObject, taken);
        return;
    }
    RELEASE_AND_RETURN(scope, handleYielded(globalObject, task, yielded));
}

// task_step()
void stepTask(JSGlobalObject* globalObject, PyStateObject* task, JSValue exception, Outcome outcome = { })
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue loop = futureIn(task).loop.get();
    enterTask(globalObject, loop, task);
    RETURN_IF_EXCEPTION(scope, void());
    stepTaskEntered(globalObject, task, exception, outcome);
    if (scope.exception()) {
        Exception* taken = takeRaisedException(vm);
        if (!taken)
            return;
        leaveTask(globalObject, loop, task);
        scope.release();
        chainRaisedExceptions(globalObject, taken);
        return;
    }
    RELEASE_AND_RETURN(scope, leaveTask(globalObject, loop, task));
}

// task_eager_start()
void startTaskEagerly(JSGlobalObject* globalObject, PyStateObject* task)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Future& state = futureIn(task);
    JSValue previous = swapCurrentTask(globalObject, state.loop.get(), task);
    RETURN_IF_EXCEPTION(scope, void());
    // If it comes to its end without waiting for anything it takes itself off again.
    registerTask(globalObject, task);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue context = state.context.get();
    if (typeOf(globalObject, context) != globalObject->pyRealm()->typeContext()) {
        raiseTypeError(globalObject, scope, "an instance of Context was expected"_s);
        return;
    }
    auto* contextObject = uncheckedDowncast<PyNativeObject>(context.asCell());
    if (!enterContext(globalObject, contextObject))
        return;

    stepTaskEntered(globalObject, task, JSValue());
    Exception* raised = scope.exception() ? takeRaisedException(vm) : nullptr;
    if (scope.exception())
        return;
    swapCurrentTask(globalObject, state.loop.get(), previous);
    if (scope.exception() && !raised)
        raised = takeRaisedException(vm);
    exitContext(globalObject, contextObject);
    if (state.status != Status::Pending)
        state.coroutine.clear();
    if (raised)
        chainRaisedExceptions(globalObject, raised);
}

// What JavaScript is waiting for, and has been running, is a task from now on. It has got as far as it has got, so there is no first step to arrange for. Null if it raised.
PyStateObject* adopt(JSGlobalObject* globalObject, JSObject* iterator, JSValue loop)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& module = asyncioState(globalObject);
    auto* task = PyStateObject::create(vm, module.taskType->instanceStructure(), makeUnique<Future>());
    initializeFuture(globalObject, task, loop);
    RETURN_IF_EXCEPTION(scope, nullptr);
    Future& state = futureIn(task);
    // It goes on in the context that it has been running in.
    JSValue context = iterator->getDirect(vm, vm.pythonNames().private_taskContext);
    state.context.set(vm, task, context ? context : JSValue(copyCurrentContext(globalObject)));
    state.logsDestroyPending = true;
    state.coroutine.set(vm, task, iterator);
    state.name.set(vm, task, intFromInt64(globalObject, static_cast<int64_t>(++module.taskNameCounter)));
    state.settlesAwaitable = true;
    registerTask(globalObject, task);
    RETURN_IF_EXCEPTION(scope, nullptr);
    iterator->putDirect(vm, vm.pythonNames().private_task, task);
    module.runningTask.set(vm, realm, task);
    return task;
}

} // anonymous namespace

JavaScriptStep::JavaScriptStep(JSGlobalObject* globalObject, JSObject* iterator)
    : m_globalObject(globalObject)
    , m_iterator(iterator)
{
    PyRealm* realm = globalObject->pyRealm();
    m_outerIterator = realm->awaitableBeingRun();
    realm->setAwaitableBeingRun(iterator);
    if (!realm->hasAsyncio()) [[likely]]
        return;
    // If this is inside a step of some task, it is no part of that task.
    auto& state = asyncioState(globalObject);
    m_outerTask = state.runningTask.get();
    state.runningTask.clear();
    m_hadRunningLoop = !!state.runningLoop;
}

JavaScriptStep::~JavaScriptStep()
{
    VM& vm = m_globalObject->vm();
    PyRealm* realm = m_globalObject->pyRealm();
    realm->setAwaitableBeingRun(m_outerIterator);
    if (!realm->hasAsyncio()) [[likely]]
        return;
    auto& state = asyncioState(m_globalObject);
    setOrClear(vm, realm, state.runningTask, m_outerTask);
    if (m_hadRunningLoop || !state.runningLoop)
        return;
    // It has been at the loop that the host turns, and may have given it something to do. Or it may have closed it.
    state.runningLoop.clear();
    if (state.hostedLoop)
        realm->configuration().watchEventLoop(m_globalObject, state.descriptor, 0_s, false);
}

bool JavaScriptStep::isTask() const
{
    VM& vm = m_globalObject->vm();
    return m_globalObject->pyRealm()->hasAsyncio() && m_iterator->getDirect(vm, vm.pythonNames().private_task);
}

bool JavaScriptStep::becomeTask(JSValue yielded)
{
    VM& vm = m_globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!m_globalObject->pyRealm()->hasAsyncio())
        return false;
    // asyncio.isfuture()
    JSValue blocking = getAttributeIfPresent(m_globalObject, yielded, named(vm, "_asyncio_future_blocking"_s));
    RETURN_IF_EXCEPTION(scope, false);
    if (!blocking || isNone(blocking))
        return false;
    runningLoopOrRaise(m_globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

void JavaScriptStep::conclude(JSValue yielded, JSValue returned)
{
    VM& vm = m_globalObject->vm();
    concludeStep(m_globalObject, asFutureCell(m_iterator->getDirect(vm, vm.pythonNames().private_task)), yielded, returned);
}

JSPromise* promiseOfFuture(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    if (!globalObject->pyRealm()->hasAsyncio() || !isFutureOrTask(globalObject, value))
        return nullptr;
    PyStateObject* cell = asFutureCell(value);
    Future& future = futureIn(cell);
    if (!future.isAlive())
        return nullptr;
    if (JSValue known = future.promise.get())
        return uncheckedDowncast<JSPromise>(known.asCell());
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    future.promise.set(vm, cell, promise);
    future.logsTraceback = false;
    if (future.status != Status::Pending) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        settleForJavaScript(globalObject, cell);
        if (Exception* exception = scope.exception(); exception && scope.clearExceptionExceptTermination())
            promise->reject(vm, exception);
    }
    return promise;
}

bool isEventLoopBeingTurned(JSGlobalObject* globalObject)
{
    if (!globalObject->pyRealm()->hasAsyncio())
        return false;
    // It looks before it runs anything, so the first to look is the loop.
    auto& state = asyncioState(globalObject);
    return state.isBeingTurned && !state.hasNotedWait;
}

void noteWaitOfEventLoop(JSGlobalObject* globalObject, int descriptor, int watched, std::optional<Seconds> timeout, bool hasEvents)
{
    auto& state = asyncioState(globalObject);
    state.hasNotedWait = true;
    state.descriptor = descriptor;
    state.watched = watched;
    state.wouldHaveWaited = timeout;
    state.hadEvents = hasEvents;
    callMethodNamed(globalObject, state.hostedLoop.get(), named(globalObject->vm(), "stop"_s));
}

void noteClosingOfDescriptor(JSGlobalObject* globalObject, int descriptor)
{
    PyRealm* realm = globalObject->pyRealm();
    if (!realm->hasAsyncio())
        return;
    auto& state = asyncioState(globalObject);
    if (state.descriptor != descriptor)
        return;
    state.descriptor = -1;
    state.hostedLoop.clear();
    realm->configuration().watchEventLoop(globalObject, -1, std::nullopt, false);
}

void turnEventLoop(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& state = asyncioState(globalObject);
    // If it is being turned already, and something that it ran is waiting for the host, it will ask again when it has been round.
    if (!state.hostedLoop || state.isBeingTurned)
        return;
    // A program can get hold of it and run it for itself, with run_until_complete(). It is the program's for as long as that lasts: see asyncioSetRunningLoop().
    if (state.runningLoop && isIdentical(state.runningLoop.get(), state.hostedLoop.get()))
        return;
    JSValue closed = callMethodNamed(globalObject, state.hostedLoop.get(), named(vm, "is_closed"_s));
    RETURN_IF_EXCEPTION(scope, void());
    bool isClosed = isTrue(globalObject, closed);
    RETURN_IF_EXCEPTION(scope, void());
    if (isClosed) {
        state.hostedLoop.clear();
        return;
    }
    turnHostedLoop(globalObject);
    // Whatever got out of it, it goes on, unless something that it ran closed it.
    if (!state.hasNotedWait || !state.hostedLoop)
        return;
    // How long it would have waited was worked out before it ran anything. If it ran something, that may have given it more to do at once. It ran something only if there was something ready, and then it would not
    // have waited at all, or if something had happened to what it is watching.
    bool hasMoreToDo = state.hadEvents || (state.wouldHaveWaited && *state.wouldHaveWaited <= 0_s);
    realm->configuration().watchEventLoop(globalObject, state.descriptor, hasMoreToDo ? std::optional { 0_s } : state.wouldHaveWaited, state.watched > state.watchedForItself);
}

// task_wakeup(): what the task was waiting for is done.
PYTHON_NATIVE(taskWakeup)
{
    NATIVE_PROLOGUE();
    PyStateObject* task = asFutureCell(args[0]);
    JSValue waitedFor = args[1];
    if (isFutureOrTask(globalObject, waitedFor)) {
        discardFromAwaitedBy(globalObject, asFutureCell(waitedFor), task);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue toThrow;
    Outcome outcome;
    if (isExactFutureOrTask(globalObject, waitedFor)) {
        JSValue result;
        auto isException = getFutureResult(globalObject, asFutureCell(waitedFor), result);
        if (isException && *isException) {
            if (isInstance(globalObject, result, realm->typeBaseException()))
                toThrow = result;
            else
                outcome = { result, true };
        } else if (Future& future = futureIn(asFutureCell(waitedFor)); isException && future.standsForPromise)
            outcome = { future.promiseOutcome.get(), future.promiseWasRejected };
    } else
        callMethodNamed(globalObject, waitedFor, named(vm, "result"_s));
    if (scope.exception()) {
        Exception* taken = takeRaisedException(vm);
        if (!taken)
            return { };
        toThrow = exceptionValue(globalObject, taken->value());
    }
    stepTask(globalObject, task, toThrow, outcome);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// The promise that a future stands for has been settled. JavaScript calls this, from wherever it has got to.
PYTHON_NATIVE(standInPromiseSettled)
{
    NATIVE_PROLOGUE();
    PyStateObject* standIn = asFutureCell(args[0]);
    Future& future = futureIn(standIn);
    // The task has been cancelled meanwhile, and is waiting for it no longer.
    if (future.status != Status::Pending)
        RETURN_NONE();
    future.promiseOutcome.set(vm, standIn, args.size() > 1 ? args[1] : jsUndefined());
    future.promiseWasRejected = unpack<bool>(callFrame, 0);
    JSValue loop = future.loop.get();
    JSValue closed = callMethodNamed(globalObject, loop, named(vm, "is_closed"_s));
    RETURN_IF_EXCEPTION(scope, { });
    bool isClosed = isTrue(globalObject, closed);
    RETURN_IF_EXCEPTION(scope, { });
    if (isClosed)
        RETURN_NONE();
    callMethodNamed(globalObject, loop, named(vm, "call_soon_threadsafe"_s), PyBoundMethod::create(globalObject, asyncioState(globalObject).finishStandIn.get(), standIn));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// And the loop calls this, in its turn.
PYTHON_NATIVE(standInFinish)
{
    NATIVE_PROLOGUE();
    PyStateObject* standIn = asFutureCell(args[0]);
    if (futureIn(standIn).status == Status::Pending) {
        setFutureResult(globalObject, standIn, jsUndefined());
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

PYTHON_NATIVE(stepWrapperCall)
{
    NATIVE_PROLOGUE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, "function takes no keyword arguments"_s));
    if (args.size() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, "function takes no positional arguments"_s));
    auto& wrapper = stateOf<StepWrapper>(args[0]);
    // A program can make one with nothing in it, which CPython crashes on.
    if (!wrapper.task)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "uninitialized Task object"_s));
    stepTask(globalObject, asFutureCell(wrapper.task.get()), wrapper.argument.get());
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// Task(coro, *, loop=None, name=None, context=None, eager_start=False)
PYTHON_NATIVE(taskInit)
{
    NATIVE_PROLOGUE();
    PyStateObject* task = asFutureCell(args[0]);
    Future& state = futureIn(task);
    JSValue coroutine = args.at(1);
    JSValue name = orNone(args.at(3));
    JSValue context = orNone(args.at(4));
    bool startsEagerly = args.at(5) && isTrue(globalObject, args.at(5));
    RETURN_IF_EXCEPTION(scope, { });

    initializeFuture(globalObject, task, orNone(args.at(2)));
    RETURN_IF_EXCEPTION(scope, { });
    auto isOne = isCoroutine(globalObject, coroutine);
    RETURN_IF_EXCEPTION(scope, { });
    if (!*isOne) {
        state.logsDestroyPending = false;
        String shown = repr(globalObject, coroutine);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("a coroutine was expected, got "_s, shown)));
    }
    state.context.set(vm, task, isNone(context) ? JSValue(copyCurrentContext(globalObject)) : context);
    state.waiter.clear();
    state.mustCancel = false;
    state.logsDestroyPending = true;
    state.cancelsRequested = 0;
    state.coroutine.set(vm, task, coroutine);

    // The name is not made until it is asked for. Until then it is the number that is to be in it.
    if (isNone(name))
        name = intFromInt64(globalObject, static_cast<int64_t>(++asyncioState(globalObject).taskNameCounter));
    else if (!name.isString()) {
        name = strObject(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
    }
    state.name.set(vm, task, name);

    if (startsEagerly) {
        JSValue answer = callMethodNamed(globalObject, state.loop.get(), named(vm, "is_running"_s));
        RETURN_IF_EXCEPTION(scope, { });
        // Py_IsTrue()
        if (answer.isTrue()) {
            startTaskEagerly(globalObject, task);
            RETURN_IF_EXCEPTION(scope, { });
            RETURN_NONE();
        }
    }
    callStepSoon(globalObject, task, JSValue());
    RETURN_IF_EXCEPTION(scope, { });
    registerTask(globalObject, task);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(taskReprMethod)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, asyncioState(globalObject).taskRepr.get(), args[0])));
}

PYTHON_NATIVE(taskCancel)
{
    NATIVE_PROLOGUE();
    PyStateObject* task = asFutureCell(args[0]);
    Future& state = futureIn(task);
    JSValue message = orNone(args.at(1));
    state.logsTraceback = false;
    if (state.status != Status::Pending)
        return JSValue::encode(jsBoolean(false));
    ++state.cancelsRequested;
    if (JSValue waiter = state.waiter.get()) {
        JSValue answer = callMethodNamed(globalObject, waiter, named(vm, "cancel"_s), message);
        RETURN_IF_EXCEPTION(scope, { });
        bool wasCancelled = isTrue(globalObject, answer);
        RETURN_IF_EXCEPTION(scope, { });
        if (wasCancelled)
            return JSValue::encode(jsBoolean(true));
    }
    state.mustCancel = true;
    state.cancelMessage.set(vm, task, message);
    return JSValue::encode(jsBoolean(true));
}

PYTHON_NATIVE(taskCancelling)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(jsNumber(futureIn(asFutureCell(args[0])).cancelsRequested));
}

PYTHON_NATIVE(taskUncancel)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    Future& state = futureIn(asFutureCell(args[0]));
    if (state.cancelsRequested > 0 && !--state.cancelsRequested)
        state.mustCancel = false;
    return JSValue::encode(jsNumber(state.cancelsRequested));
}

PYTHON_NATIVE(taskGetStack)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, asyncioState(globalObject).taskGetStack.get(), args[0], orNone(args.at(1)))));
}

PYTHON_NATIVE(taskPrintStack)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, asyncioState(globalObject).taskPrintStack.get(), args[0], orNone(args.at(1)), orNone(args.at(2)))));
}

PYTHON_NATIVE(taskSetResult)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "Task does not support set_result operation"_s));
}

PYTHON_NATIVE(taskSetException)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "Task does not support set_exception operation"_s));
}

PYTHON_NATIVE(taskGetCoroutine)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(orNone(futureIn(asFutureCell(args[0])).coroutine.get()));
}

PYTHON_NATIVE(taskGetContext)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(orNone(futureIn(asFutureCell(args[0])).context.get()));
}

PYTHON_NATIVE(taskGetName)
{
    NATIVE_PROLOGUE();
    PyStateObject* task = asFutureCell(args[0]);
    Future& state = futureIn(task);
    JSValue name = state.name.get();
    if (!name)
        RETURN_NONE();
    if (isExactly(globalObject, name, BuiltinType::Int)) {
        String number = str(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        name = jsString(vm, concatenate("Task-"_s, number));
        state.name.set(vm, task, name);
    }
    return JSValue::encode(name);
}

PYTHON_NATIVE(taskSetName)
{
    NATIVE_PROLOGUE();
    JSValue name = args[1];
    if (!name.isString()) {
        name = strObject(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
    }
    futureIn(asFutureCell(args[0])).name.set(vm, asFutureCell(args[0]), name);
    RETURN_NONE();
}

// TaskObj_finalize()
PYTHON_NATIVE(taskDelete)
{
    NATIVE_PROLOGUE();
    PyStateObject* task = asFutureCell(args[0]);
    Future& state = futureIn(task);
    if (state.status == Status::Pending && state.logsDestroyPending) {
        Exception* raised = takeRaisedException(vm);
        PyDict* context = PyDict::create(globalObject);
        context->setString(globalObject, "message"_s, jsNontrivialString(vm, "Task was destroyed but it is pending!"_s));
        context->setString(globalObject, "task"_s, task);
        if (JSValue source = state.sourceTraceback.get())
            context->setString(globalObject, "source_traceback"_s, source);
        callExceptionHandler(globalObject, state.loop.get(), context);
        if (raised)
            restoreRaisedException(globalObject, raised);
    }
    finalizeFuture(globalObject, task);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// ---- The functions of the module

PYTHON_NATIVE(asyncioGetRunningLoopOrNone)
{
    UNUSED_PARAM(callFrame);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue loop = runningLoop(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(orNone(loop));
}

PYTHON_NATIVE(asyncioSetRunningLoop)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& state = asyncioState(globalObject);
    if (!isNone(args[0])) {
        state.runningLoop.set(vm, realm, args[0]);
        RETURN_NONE();
    }
    // If it is the loop that the host turns, and it is the program that has been running it, it is the host's again, with whatever it has been left to do.
    bool isHostsAgain = !state.isBeingTurned && state.hostedLoop && state.runningLoop && isIdentical(state.runningLoop.get(), state.hostedLoop.get());
    state.runningLoop.clear();
    if (isHostsAgain)
        realm->configuration().watchEventLoop(globalObject, state.descriptor, 0_s, false);
    RETURN_NONE();
}

PYTHON_NATIVE(asyncioGetEventLoop)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(getEventLoop(globalObject));
}

namespace {

JSValue runningLoopOrRaise(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue loop = runningLoop(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (loop)
        return loop;
    return raise(globalObject, scope, BuiltinType::RuntimeError, "no running event loop"_s);
}

} // anonymous namespace

PYTHON_NATIVE(asyncioGetRunningLoop)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(runningLoopOrRaise(globalObject));
}

enum class Registry : uint8_t { Scheduled, Eager };

// _register_task() and _register_eager_task(), and the two that undo them.
PYTHON_NATIVE(asyncioRegisterTask)
{
    NATIVE_PROLOGUE();
    auto& state = asyncioState(globalObject);
    bool isAdding = unpack<bool>(callFrame, 1);
    JSValue task = args.at(0);
    if (isTask(globalObject, task)) {
        if (isAdding)
            registerTask(globalObject, task);
        else
            unregisterTask(globalObject, task);
    } else if (unpack<Registry>(callFrame, 0) == Registry::Scheduled)
        callMethodNamed(globalObject, state.otherTasks.get(), named(vm, isAdding ? "add"_s : "discard"_s), task);
    else if (isAdding)
        state.otherEagerTasks->add(globalObject, task);
    else
        state.otherEagerTasks->remove(globalObject, task);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(asyncioEnterTask)
{
    NATIVE_PROLOGUE();
    enterTask(globalObject, args.at(0), args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(asyncioLeaveTask)
{
    NATIVE_PROLOGUE();
    leaveTask(globalObject, args.at(0), args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(asyncioSwapCurrentTask)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(swapCurrentTask(globalObject, args.at(0), args.at(1))));
}

PYTHON_NATIVE(asyncioCurrentTask)
{
    NATIVE_PROLOGUE();
    auto& state = asyncioState(globalObject);
    JSValue loop = orNone(args.at(0));
    if (isNone(loop)) {
        loop = runningLoopOrRaise(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    runningLoop(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    // There is the one thread.
    if (!isIdentical(orNone(state.runningLoop.get()), loop))
        RETURN_NONE();
    return JSValue::encode(orNone(state.runningTask.get()));
}

PYTHON_NATIVE(asyncioAllTasks)
{
    NATIVE_PROLOGUE();
    auto& state = asyncioState(globalObject);
    JSValue loop = orNone(args.at(0));
    if (isNone(loop)) {
        loop = runningLoopOrRaise(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    // Those that are eager first, so that one that ceases to be meanwhile is not missed.
    MarkedArgumentBuffer tasks;
    for (JSValue some : { JSValue(state.otherEagerTasks.get()), state.otherTasks.get(), state.scheduledTasks.get() }) {
        if (!collect(globalObject, some, tasks))
            return { };
    }
    PySet* result = PySet::create(globalObject);
    for (unsigned i = 0; i < tasks.size(); ++i) {
        JSValue task = tasks.at(i);
        // add_one_task()
        if (typeOf(globalObject, task) == state.taskType.get()) {
            Future& future = futureIn(asFutureCell(task));
            if (future.status != Status::Pending || !isIdentical(future.loop.get(), loop))
                continue;
        } else {
            JSValue done = callMethodNamed(globalObject, task, named(vm, "done"_s));
            RETURN_IF_EXCEPTION(scope, { });
            if (done.isTrue())
                continue;
            JSValue itsLoop = loopOfFuture(globalObject, task);
            RETURN_IF_EXCEPTION(scope, { });
            if (!isIdentical(itsLoop, loop))
                continue;
        }
        result->add(globalObject, task);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

// future_add_to_awaited_by() and future_discard_from_awaited_by()
PYTHON_NATIVE(asyncioChangeAwaitedBy)
{
    NATIVE_PROLOGUE();
    if (isFutureOrTask(globalObject, args[0]) && isFutureOrTask(globalObject, args[1])) {
        if (unpack<bool>(callFrame, 0))
            addToAwaitedBy(globalObject, asFutureCell(args[0]), args[1]);
        else
            discardFromAwaitedBy(globalObject, asFutureCell(args[0]), args[1]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

JSObject* createAsyncioModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = asyncioState(globalObject);

    if (!state.futureType) {
        auto make = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name, PyType* base, unsigned flags) {
            PyType* type = createBuiltinType(globalObject, name, base, PyType::Layout::Native, flags);
            type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
            slot.set(vm, realm, type);
            return type;
        };
        constexpr auto withClass = PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass;

        PyType* wrapper = make(state.stepWrapperType, "_asyncio.TaskStepMethWrapper"_s, realm->typeObject(), 0);
        addMethods(globalObject, wrapper, { { "__call__"_s, stepWrapperCall, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreNotChecked } });
        addGenericGetAttribute(globalObject, wrapper);
        wrapper->setAllocator([] (VM& vm, Structure* structure) -> JSObject* { return PyStateObject::create(vm, structure, makeUnique<StepWrapper>()); });
        addGetSet(globalObject, wrapper, "__self__"_s, [] (JSGlobalObject*, JSValue self) { return orNone(stateOf<StepWrapper>(self).task.get()); });

        PyType* iterator = make(state.futureIteratorType, "_asyncio.FutureIter"_s, realm->typeObject(), 0);
        addMethods(globalObject, iterator, {
            { "__iter__"_s, returnSelf, Kind::Wrapper },
            { "__next__"_s, futureIteratorNext, Kind::Wrapper },
            { "send"_s, futureIteratorNext },
            { "throw"_s, futureIteratorThrow, Kind::Method, 0, "($self, /, *args)"_s, PyNativeFunction::Arguments::AreNotChecked },
            { "close"_s, futureIteratorClose },
        });
        addGenericGetAttribute(globalObject, iterator);
        iterator->setAllocator([] (VM& vm, Structure* structure) -> JSObject* { return PyStateObject::create(vm, structure, makeUnique<FutureIterator>()); });

        constexpr unsigned flags = PyType::IsBaseType | PyType::HasInstanceDict | PyType::HasWeakReferences;
        PyType* future = make(state.futureType, "_asyncio.Future"_s, realm->typeObject(), flags);
        PyType* task = make(state.taskType, "_asyncio.Task"_s, future, flags);
        for (PyType* type : { future, task }) {
            addMethods(globalObject, type, {
                { "__new__"_s, futureNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
                { "__iter__"_s, futureIter, Kind::Wrapper },
                { "__await__"_s, futureIter, Kind::Wrapper },
                { "result"_s, futureResultMethod },
                { "exception"_s, futureException, Kind::Method, 0, { }, withClass },
                { "add_done_callback"_s, futureAddDoneCallback, Kind::Method, 0, { }, withClass },
                { "remove_done_callback"_s, futureRemoveDoneCallback, Kind::Method, 0, { }, withClass },
                { "cancelled"_s, futureCancelled },
                { "done"_s, futureDone },
                { "_make_cancelled_error"_s, futureMakeCancelledError },
            });
            addClassGetItemIfGeneric(globalObject, type);
        }
        addMethods(globalObject, future, {
            { "__init__"_s, futureInit, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
            { "__repr__"_s, futureReprMethod, Kind::Wrapper },
            { "__del__"_s, futureDelete, Kind::Wrapper },
            { "set_result"_s, futureSetResult, Kind::Method, 0, { }, withClass },
            { "set_exception"_s, futureSetException, Kind::Method, 0, { }, withClass },
            { "cancel"_s, futureCancel, Kind::Method, 0, { }, withClass },
            { "get_loop"_s, futureGetLoop, Kind::Method, 0, { }, withClass },
        });
        addGetSet(globalObject, future, "_state"_s, getState);
        addGetSet(globalObject, future, "_asyncio_future_blocking"_s, getBlocking, setBlocking);
        addGetSet(globalObject, future, "_loop"_s, getLoopOrNone);
        addGetSet(globalObject, future, "_callbacks"_s, getCallbacks);
        addGetSet(globalObject, future, "_result"_s, getIfAlive<&Future::result>);
        addGetSet(globalObject, future, "_exception"_s, getIfAlive<&Future::exception>);
        addGetSet(globalObject, future, "_log_traceback"_s, getLogTraceback, setLogTraceback);
        addGetSet(globalObject, future, "_source_traceback"_s, getOrNone<&Future::sourceTraceback>);
        addGetSet(globalObject, future, "_cancel_message"_s, getOrNone<&Future::cancelMessage>, setCancelMessage);
        addGetSet(globalObject, future, "_asyncio_awaited_by"_s, getAwaitedBy);

        addMethods(globalObject, task, {
            { "__init__"_s, taskInit, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
            { "__repr__"_s, taskReprMethod, Kind::Wrapper },
            { "__del__"_s, taskDelete, Kind::Wrapper },
            { "set_result"_s, taskSetResult },
            { "set_exception"_s, taskSetException },
            { "cancel"_s, taskCancel },
            { "cancelling"_s, taskCancelling },
            { "uncancel"_s, taskUncancel },
            { "get_stack"_s, taskGetStack, Kind::Method, 0, { }, withClass },
            { "print_stack"_s, taskPrintStack, Kind::Method, 0, { }, withClass },
            { "get_name"_s, taskGetName },
            { "set_name"_s, taskSetName },
            { "get_coro"_s, taskGetCoroutine },
            { "get_context"_s, taskGetContext },
        });
        addGetSet(globalObject, task, "_log_destroy_pending"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(futureIn(asFutureCell(self)).logsDestroyPending); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
            auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
            if (auto flag = flagToSet(globalObject, scope, value))
                futureIn(asFutureCell(self)).logsDestroyPending = *flag;
        });
        addGetSet(globalObject, task, "_must_cancel"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(futureIn(asFutureCell(self)).mustCancel); });
        addGetSet(globalObject, task, "_coro"_s, getOrNone<&Future::coroutine>);
        addGetSet(globalObject, task, "_fut_waiter"_s, getOrNone<&Future::waiter>);

        state.wakeup.set(vm, realm, PyNativeFunction::create(vm, globalObject, 1, "task_wakeup"_s, taskWakeup, Kind::Method, task->object(), 0, ImplementationVisibility::Public, "($self, object, /)"_s));
        auto hidden = [&] (WriteBarrier<Unknown>& slot, ASCIILiteral name, NativeFunction function, unsigned data, ASCIILiteral signature) {
            slot.set(vm, realm, PyNativeFunction::create(vm, globalObject, 1, name, function, Kind::Method, future->object(), data, ImplementationVisibility::Public, signature));
        };
        hidden(state.promiseFulfilled, "promise_fulfilled"_s, standInPromiseSettled, pack(false), "($self, value=None, /)"_s);
        hidden(state.promiseRejected, "promise_rejected"_s, standInPromiseSettled, pack(true), "($self, reason=None, /)"_s);
        hidden(state.finishStandIn, "promise_settled"_s, standInFinish, 0, "($self, /)"_s);
        KeywordNames* keyword = KeywordNames::create(vm, CopyOnWriteArrayWithContiguous, 1);
        keyword->setIndex(vm, 0, jsNontrivialString(vm, "context"_s));
        state.contextKeyword.set(vm, realm, keyword);
    }

    JSObject* module = newBuiltinModule(globalObject, "_asyncio"_s);
    addFunction(globalObject, module, "current_task"_s, asyncioCurrentTask);
    addFunction(globalObject, module, "get_event_loop"_s, asyncioGetEventLoop);
    addFunction(globalObject, module, "get_running_loop"_s, asyncioGetRunningLoop);
    addFunction(globalObject, module, "_get_running_loop"_s, asyncioGetRunningLoopOrNone);
    addFunction(globalObject, module, "_set_running_loop"_s, asyncioSetRunningLoop);
    addFunction(globalObject, module, "_register_task"_s, asyncioRegisterTask, pack(Registry::Scheduled, true));
    addFunction(globalObject, module, "_register_eager_task"_s, asyncioRegisterTask, pack(Registry::Eager, true));
    addFunction(globalObject, module, "_unregister_task"_s, asyncioRegisterTask, pack(Registry::Scheduled, false));
    addFunction(globalObject, module, "_unregister_eager_task"_s, asyncioRegisterTask, pack(Registry::Eager, false));
    addFunction(globalObject, module, "_enter_task"_s, asyncioEnterTask);
    addFunction(globalObject, module, "_leave_task"_s, asyncioLeaveTask);
    addFunction(globalObject, module, "_swap_current_task"_s, asyncioSwapCurrentTask);
    addFunction(globalObject, module, "all_tasks"_s, asyncioAllTasks);
    addFunction(globalObject, module, "future_add_to_awaited_by"_s, asyncioChangeAwaitedBy, pack(true));
    addFunction(globalObject, module, "future_discard_from_awaited_by"_s, asyncioChangeAwaitedBy, pack(false));
    module->putDirect(vm, named(vm, "Future"_s), state.futureType->object());
    module->putDirect(vm, named(vm, "Task"_s), state.taskType->object());
    return module;
}

// module_init(). asyncio imports this, and this imports asyncio, so it is in sys.modules by now.
void executeAsyncioModule(JSGlobalObject* globalObject, JSObject*)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& state = asyncioState(globalObject);
    importModule(globalObject, "asyncio"_s);
    RETURN_IF_EXCEPTION(scope, void());
    state.coroutineTypes.set(vm, realm, PySet::create(globalObject));
    state.otherEagerTasks.set(vm, realm, PySet::create(globalObject));
    struct Import {
        WriteBarrier<Unknown>& slot;
        ASCIILiteral module;
        ASCIILiteral name;
    };
    for (auto& [slot, from, name] : std::initializer_list<Import> {
        { state.getEventLoopPolicy, "asyncio.events"_s, "_get_event_loop_policy"_s },
        { state.futureRepr, "asyncio.base_futures"_s, "_future_repr"_s },
        { state.invalidStateError, "asyncio.exceptions"_s, "InvalidStateError"_s },
        { state.cancelledError, "asyncio.exceptions"_s, "CancelledError"_s },
        { state.taskRepr, "asyncio.base_tasks"_s, "_task_repr"_s },
        { state.taskGetStack, "asyncio.base_tasks"_s, "_task_get_stack"_s },
        { state.taskPrintStack, "asyncio.base_tasks"_s, "_task_print_stack"_s },
        { state.isCoroutineFunction, "asyncio.coroutines"_s, "iscoroutine"_s },
        { state.extractStack, "traceback"_s, "extract_stack"_s },
    }) {
        JSValue value = importModuleAttribute(globalObject, String(from), name);
        RETURN_IF_EXCEPTION(scope, void());
        slot.set(vm, realm, value);
    }
    JSValue weakSet = importModuleAttribute(globalObject, "weakref"_s, "WeakSet"_s);
    RETURN_IF_EXCEPTION(scope, void());
    for (auto* slot : { &state.otherTasks, &state.scheduledTasks }) {
        JSValue set = call(globalObject, weakSet);
        RETURN_IF_EXCEPTION(scope, void());
        slot->set(vm, realm, set);
    }
    realm->setHasAsyncio();
}

} } // namespace JSC::Python
