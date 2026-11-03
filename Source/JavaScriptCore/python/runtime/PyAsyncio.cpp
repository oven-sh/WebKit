/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyAsyncio.h"

#include "CodeBlock.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyContextVars.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyGenerator.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyWeakRef.h"
#include "TopExceptionScope.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _asyncio: Future, Task, and what keeps track of the running loop and of the tasks, which Lib/asyncio takes in
// place of its own if they are there. (Modules/_asynciomodule.c in CPython is the reference.)
//
// A future, a task, and the small objects that go with them are PyRecords.

// ---- The state of the module ----
//
// A record that programs never see. Every future has it in a field; the types Future and Task have it in a hidden field.

#define FOR_EACH_ASYNCIO_NAME(v) \
    v(AddDoneCallback, "add_done_callback") \
    v(Add, "add") \
    v(Blocking, "_asyncio_future_blocking") \
    v(CallExceptionHandler, "call_exception_handler") \
    v(CallSoon, "call_soon") \
    v(Cancel, "cancel") \
    v(Cancelled, "CANCELLED") \
    v(Discard, "discard") \
    v(Done, "done") \
    v(Exception, "exception") \
    v(Finished, "FINISHED") \
    v(Future, "future") \
    v(GetDebug, "get_debug") \
    v(GetEventLoop, "get_event_loop") \
    v(GetLoop, "get_loop") \
    v(IsRunning, "is_running") \
    v(Loop, "_loop") \
    v(Message, "message") \
    v(Pending, "PENDING") \
    v(Result, "result") \
    v(SourceTraceback, "source_traceback") \
    v(Task, "task")

enum StateField : unsigned {
    StateFutureType,
    StateTaskType,
    StateFutureIterType,
    StateStepWrapperType,
    StateWakeupMethod, // What a task gives the future it waits for is this, bound to the task.
    StateContextKeyword, // The names of the keywords of f(..., context=c).

    // From Lib/asyncio, traceback and weakref.
    StateGetEventLoopPolicy,
    StateFutureRepr,
    StateInvalidStateError,
    StateCancelledError,
    StateTaskRepr,
    StateTaskGetStack,
    StateTaskPrintStack,
    StateIsCoroutine,
    StateExtractStack,

    StateForeignTasks, // A WeakSet: the scheduled tasks that are not instances of Task.
    StateForeignEagerTasks, // A set: likewise, those that are running eagerly.
    StateCoroutineTypes, // A set: types that asyncio.iscoroutine() has accepted instances of.
    StateTaskNameCounter,

    // What belongs to a thread: of the thread whose ThreadState::serial is `number`,
    StateRunningLoop, // Empty: none.
    StateCurrentTask,
    StateOtherThreads, // and of the others that have any: a dict, serial -> (loop, task). Empty: no thread has.

#define ASYNCIO_DECLARE_NAME(id, text) StateName##id,
    FOR_EACH_ASYNCIO_NAME(ASYNCIO_DECLARE_NAME)
#undef ASYNCIO_DECLARE_NAME
    StateFieldCount
};

static constexpr TypeDef asyncioStateTypeDef { "_asyncio.state"_s, { TypeFlag::DisallowInstantiation, TypeFlag::Internal }, { }, { }, { } };

enum FutureField : unsigned {
    FutureModuleState,
    FutureLoop, // Empty: __init__ has not run.
    FutureCallback0, // The first callback and its context,
    FutureContext0,
    FutureCallbacks, // and a list of (callback, context) for the others. Empty: there are none.
    FutureException,
    FutureExceptionTraceback,
    FutureResult,
    FutureSourceTraceback,
    FutureCancelMessage,
    FutureCancelledError,
    FutureAwaitedBy, // The one future that awaits this one, or a set of them (FlagAwaitedByIsSet).
    FutureFieldCount,

    TaskWaiter = FutureFieldCount, // _fut_waiter
    TaskCoroutine,
    TaskName, // An int until somebody asks: the number in "Task-1".
    TaskContext,
    TaskIndex, // Where it is in Runtime::scheduledTasks. Empty: it is not there.
    TaskWakeup, // The callables the task hands to the loop and to futures, which are made once.
    TaskStep,
    TaskFieldCount
};

// PyRecord::number of a future. The upper half is how many cancellations of the task have been requested.
enum FutureFlag : int64_t {
    FlagStateMask = 3,
    FlagLogTraceback = 1 << 2,
    FlagBlocking = 1 << 3,
    FlagAwaitedByIsSet = 1 << 4,
    FlagMustCancel = 1 << 5,
    FlagLogDestroyPending = 1 << 6,
};
enum FutureState : int64_t { StatePending, StateCancelled, StateFinished };

static ALWAYS_INLINE PyRecord* recordOf(JSValue value) { return uncheckedDowncast<PyRecord>(value.asCell()); }
static ALWAYS_INLINE PyRecord* stateOf(PyRecord* future) { return recordOf(future->get(FutureModuleState)); }
static ALWAYS_INLINE PyType* typeIn(PyRecord* state, StateField field) { return uncheckedDowncast<PyType>(state->get(field).asCell()); }
static ALWAYS_INLINE JSString* nameIn(PyRecord* state, StateField field) { return asString(state->get(field)); }

static ALWAYS_INLINE FutureState futureState(PyRecord* future) { return static_cast<FutureState>(future->number & FlagStateMask); }
static ALWAYS_INLINE void setFutureState(PyRecord* future, FutureState state) { future->number = (future->number & ~static_cast<int64_t>(FlagStateMask)) | state; }
static ALWAYS_INLINE bool hasFlag(PyRecord* future, FutureFlag flag) { return future->number & flag; }
static ALWAYS_INLINE void setFlag(PyRecord* future, FutureFlag flag, bool value)
{
    if (value)
        future->number |= flag;
    else
        future->number &= ~static_cast<int64_t>(flag);
}
static ALWAYS_INLINE int32_t cancelRequests(PyRecord* task) { return static_cast<int32_t>(task->number >> 32); }
static ALWAYS_INLINE void setCancelRequests(PyRecord* task, int32_t count) { task->number = (task->number & 0xffffffff) | (static_cast<int64_t>(count) << 32); }

static ALWAYS_INLINE bool isAlive(PyRecord* future) { return !!future->get(FutureLoop); }
static ALWAYS_INLINE bool isTaskRecord(PyRecord* future) { return future->fieldCount() == TaskFieldCount; }
static ALWAYS_INLINE JSValue orNone(JSValue value) { return value ? value : jsUndefined(); }

static ALWAYS_INLINE PyType* typeOfObject(JSValue value)
{
    if (!value.isCell() || value.asCell()->type() != PyObjectType)
        return nullptr;
    return uncheckedDowncast<PyObject>(value.asCell())->type();
}

// An instance of Future or Task itself.
static ALWAYS_INLINE PyRecord* asExactFuture(PyRecord* state, JSValue value)
{
    PyType* type = typeOfObject(value);
    return type && (type == typeIn(state, StateTaskType) || type == typeIn(state, StateFutureType)) ? recordOf(value) : nullptr;
}

// Or of a class derived from them.
static ALWAYS_INLINE PyRecord* asFuture(PyRecord* state, JSValue value)
{
    PyType* type = typeOfObject(value);
    if (!type)
        return nullptr;
    if (type == typeIn(state, StateTaskType) || type == typeIn(state, StateFutureType)) [[likely]]
        return recordOf(value);
    return type->isSubtypeOf(typeIn(state, StateFutureType)) ? recordOf(value) : nullptr;
}

static ALWAYS_INLINE PyRecord* asTask(PyRecord* state, JSValue value)
{
    PyRecord* future = asFuture(state, value);
    return future && isTaskRecord(future) ? future : nullptr;
}

static const TypeDef& futureTypeDefinition();

static PyRecord* moduleState(JSGlobalObject* globalObject)
{
    return recordOf(hiddenField(globalObject, runtime(globalObject).typeFor(futureTypeDefinition()), 0));
}

// Whether what the module takes from Lib is there. Before that, and if that has failed, its types and functions can be had all the
// same, because asyncio, which this imports, imports this. False: an exception.
static bool ensureIsInitialized(JSGlobalObject* globalObject, PyRecord* state)
{
    // The last that initializeAsyncioModule() sets.
    if (state->get(StateForeignTasks)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseRuntimeError(globalObject, scope, "the module _asyncio has not been initialized"_s);
    return false;
}

static JSValue raiseInvalidState(JSGlobalObject* globalObject, ThrowScope& scope, PyRecord* state, const String& message)
{
    return raise(globalObject, scope, typeIn(state, StateInvalidStateError), message);
}

// ---- What belongs to a thread ----

static NEVER_INLINE void switchToThread(JSGlobalObject* globalObject, PyRecord* state, uint64_t serial)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto keyOf = [](uint64_t serial) {
        return jsNumber(static_cast<double>(serial));
    };
    JSValue loop = state->get(StateRunningLoop);
    JSValue task = state->get(StateCurrentTask);
    PyDict* others = state->get(StateOtherThreads) ? PyDict::from(state->get(StateOtherThreads)) : nullptr;
    if (loop || task) {
        if (!others) {
            others = PyDict::create(globalObject);
            state->set(vm, StateOtherThreads, others);
        }
        others->set(globalObject, keyOf(state->number), PyTuple::create(globalObject, { orNone(loop), orNone(task) }));
    }
    JSValue mine = others ? others->take(globalObject, keyOf(serial)) : JSValue();
    auto valueAt = [&](unsigned index) {
        JSValue value = mine ? PyTuple::from(mine)->at(index) : JSValue();
        return value && !isNone(value) ? value : JSValue();
    };
    state->set(vm, StateRunningLoop, valueAt(0));
    state->set(vm, StateCurrentTask, valueAt(1));
    state->number = serial;

    // A thread that ended while a loop of its was running left that behind.
    if (others && others->size() > 4) {
        Vector<uint64_t, 16> living;
        rt.threads().forEachThread([&](ThreadState& thread) {
            living.append(thread.serial);
        });
        MarkedArgumentBuffer dead;
        JSValue key;
        JSValue value;
        for (unsigned position = 0; others->next(position, key, value);) {
            if (!living.contains(static_cast<uint64_t>(key.asNumber())))
                dead.append(key);
        }
        for (unsigned i = 0; i < dead.size(); ++i)
            others->remove(globalObject, dead.at(i));
    }
}

// After this, StateRunningLoop and StateCurrentTask are those of the thread that is running.
static ALWAYS_INLINE void useThisThread(JSGlobalObject* globalObject, PyRecord* state)
{
    uint64_t serial = runtime(globalObject).threads().current().serial;
    if (static_cast<uint64_t>(state->number) != serial) [[unlikely]]
        switchToThread(globalObject, state, serial);
}

// ---- The scheduled tasks ----
//
// all_tasks() finds them, and they are not kept alive by that: a task that nothing else refers to is garbage ("Task was destroyed
// but it is pending!"). It leaves the table when it is done, or else when the collector frees it, as it leaves CPython's list.

static void registerTask(JSGlobalObject* globalObject, PyRecord* task)
{
    if (task->get(TaskIndex))
        return;
    Runtime& rt = runtime(globalObject);
    task->set(rt.vm(), TaskIndex, jsNumber(rt.scheduledTasks.add(task)));
}

static void unregisterTask(JSGlobalObject* globalObject, PyRecord* task)
{
    JSValue index = task->get(TaskIndex);
    if (!index)
        return;
    Runtime& rt = runtime(globalObject);
    rt.scheduledTasks.remove(index.asUInt32(), task);
    task->set(rt.vm(), TaskIndex, { });
}

// ---- Finalization ----

static JSValue futureDel(JSGlobalObject*, JSValue self);
static JSValue taskDel(JSGlobalObject*, JSValue self);

// Whether the __del__ of the object is one of the two above, which have nothing to do for most futures most of the time: then the
// runtime is told of the object only while they have. (It is told of every object that has a __del__ when the object is made.)
static bool hasFinalizerOfModule(PyRecord* state, PyRecord* future)
{
    PyType* type = future->type();
    if (type == typeIn(state, StateTaskType) || type == typeIn(state, StateFutureType)) [[likely]]
        return true;
    auto* native = PyNativeFunction::tryFrom(type->lookup(Special::Del));
    return native && (native->def().pointer.f1 == futureDel || native->def().pointer.f1 == taskDel);
}

static ALWAYS_INLINE void finalizerHasNothingToDo(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future)
{
    if (hasFinalizerOfModule(state, future))
        runtime(globalObject).unregisterFinalizable(future);
}

static ALWAYS_INLINE void finalizerHasSomethingToDo(JSGlobalObject* globalObject, PyRecord* future)
{
    runtime(globalObject).registerFinalizable(future);
}

// loop.call_exception_handler(context). Never raises.
static void callExceptionHandler(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future, PyDict* context)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (JSValue source = future->get(FutureSourceTraceback))
        context->set(vm, nameIn(state, StateNameSourceTraceback), source);
    JSValue function = getAttr(globalObject, future->get(FutureLoop), nameIn(state, StateNameCallExceptionHandler));
    if (!function) {
        takePendingException(globalObject);
        return;
    }
    call(globalObject, function, context);
    if (scope.exception() && !vm.hasPendingTerminationException())
        reportUnraisable(globalObject, makeString("Exception ignored while calling asyncio function "_s, reprForMessage(globalObject, function)), { });
}

static JSValue futureDel(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* future = recordOf(self);
    if (!hasFlag(future, FlagLogTraceback))
        return jsUndefined();
    setFlag(future, FlagLogTraceback, false);
    VM& vm = getVM(globalObject);
    PyRecord* state = stateOf(future);
    PyDict* context = PyDict::create(globalObject);
    context->set(vm, nameIn(state, StateNameMessage), makeStr(vm, makeString(future->type()->name(), " exception was never retrieved"_s)));
    context->set(vm, nameIn(state, StateNameException), future->get(FutureException));
    context->set(vm, nameIn(state, StateNameFuture), future);
    callExceptionHandler(globalObject, state, future, context);
    return jsUndefined();
}

static JSValue taskDel(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* task = recordOf(self);
    VM& vm = getVM(globalObject);
    PyRecord* state = stateOf(task);
    // What the handler is given may keep the task, which is then still scheduled.
    if (futureState(task) == StatePending && hasFlag(task, FlagLogDestroyPending) && isAlive(task)) {
        PyDict* context = PyDict::create(globalObject);
        context->set(vm, nameIn(state, StateNameMessage), makeStr(vm, "Task was destroyed but it is pending!"_s));
        context->set(vm, nameIn(state, StateNameTask), task);
        callExceptionHandler(globalObject, state, task, context);
    }
    return futureDel(globalObject, self);
}

// ---- Futures ----

// asyncio.futures._get_loop()
static JSValue loopOfFuture(JSGlobalObject* globalObject, PyRecord* state, JSValue future)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyRecord* exact = asExactFuture(state, future))
        return orNone(exact->get(FutureLoop));
    JSValue getLoop = getAttrIfPresent(globalObject, future, nameIn(state, StateNameGetLoop));
    RETURN_IF_EXCEPTION(scope, { });
    if (getLoop)
        RELEASE_AND_RETURN(scope, call(globalObject, getLoop));
    RELEASE_AND_RETURN(scope, getAttr(globalObject, future, nameIn(state, StateNameLoop)));
}

// No loop of asyncio's is running, and yet the code may be running in an event loop: the host's, which runs the code of JS. For
// what JS calls, however indirectly, the loop by which the host's event loop module runs coroutines for JS is the running one:
// asyncio.gather(), create_task() and Future() are as good there as in a coroutine. (Not to asyncio.run(), which asks
// _get_running_loop(): it may run a loop of its own, as it may in a program that is all Python.) Empty: there is none.
static JSValue loopOfHost(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    bool isCalledFromJS = false;
    EntryFrame* entryFrame = vm.topEntryFrame;
    for (CallFrame* callFrame = vm.topCallFrame; callFrame && !isCalledFromJS; callFrame = callFrame->callerFrame(entryFrame)) {
        if (callFrame->isNativeCalleeFrame())
            continue;
        CodeBlock* codeBlock = callFrame->codeBlock();
        isCalledFromJS = codeBlock && !codeBlock->unlinkedCodeBlock()->isPython();
    }
    if (!isCalledFromJS)
        return { };
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue loop = eventLoopFunction(globalObject, "loop"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!loop)
        return { };
    RELEASE_AND_RETURN(scope, call(globalObject, loop));
}

static JSValue getEventLoop(JSGlobalObject* globalObject, PyRecord* state)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    useThisThread(globalObject, state);
    if (JSValue loop = state->get(StateRunningLoop))
        return loop;
    JSValue hostLoop = loopOfHost(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (hostLoop)
        return hostLoop;
    if (failed(scope, ensureIsInitialized(globalObject, state)))
        return { };
    JSValue policy = call(globalObject, state->get(StateGetEventLoopPolicy));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callMethod(globalObject, policy, nameIn(state, StateNameGetEventLoop)));
}

// loop.call_soon(function[, argument], context=context). False: an exception.
static bool callSoon(JSGlobalObject* globalObject, PyRecord* state, JSValue loop, JSValue function, JSValue argument, JSValue context)
{
    JSValue values[4];
    JSValue method = getMethod(globalObject, loop, nameIn(state, StateNameCallSoon), values[0]);
    if (!method)
        return false;
    unsigned count = 1;
    values[count++] = function;
    if (argument)
        values[count++] = argument;
    values[count] = context;
    auto* keywords = uncheckedDowncast<PyTuple>(state->get(StateContextKeyword).asCell());
    if (values[0])
        return !!call(globalObject, method, CallArgs(values, count, keywords));
    return !!call(globalObject, method, CallArgs(values + 1, count - 1, keywords));
}

// The future is done. False: an exception.
static bool scheduleCallbacks(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future)
{
    VM& vm = getVM(globalObject);
    ASSERT(futureState(future) != StatePending);
    if (isTaskRecord(future))
        unregisterTask(globalObject, future);

    // They are taken out of the future first: what call_soon() runs may be anything.
    if (JSValue callback = future->get(FutureCallback0)) {
        JSValue context = future->get(FutureContext0);
        future->set(vm, FutureCallback0, { });
        future->set(vm, FutureContext0, { });
        if (!callSoon(globalObject, state, future->get(FutureLoop), callback, future, context)) {
            future->set(vm, FutureCallbacks, { });
            return false;
        }
    }
    JSValue callbacks = future->get(FutureCallbacks);
    if (!callbacks)
        return true;
    future->set(vm, FutureCallbacks, { });
    JSArray* list = asList(callbacks);
    for (unsigned i = 0; i < listLength(list); ++i) {
        auto* pair = PyTuple::from(listGet(list, i));
        if (!callSoon(globalObject, state, future->get(FutureLoop), pair->at(0), future, pair->at(1)))
            return false;
    }
    return true;
}

// Future.__init__(). False: an exception.
static bool initializeFuture(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future, JSValue loop)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isAlive(future)) {
        raiseRuntimeError(globalObject, scope, typeNameForMessage(globalObject, future->type()), " object is already initialized"_s);
        return false;
    }
    future->number = StatePending;
    if (!loop || isNone(loop)) {
        loop = getEventLoop(globalObject, state);
        RETURN_IF_EXCEPTION(scope, false);
    }
    future->set(vm, FutureLoop, loop);

    JSValue debug = callMethod(globalObject, loop, nameIn(state, StateNameGetDebug));
    RETURN_IF_EXCEPTION(scope, false);
    bool isDebug = debug.isBoolean() ? debug.asBoolean() : isTrue(globalObject, debug);
    RETURN_IF_EXCEPTION(scope, false);
    if (isDebug && !runtime(globalObject).threads().isFinalized()) [[unlikely]] {
        JSValue stack = call(globalObject, state->get(StateExtractStack));
        RETURN_IF_EXCEPTION(scope, false);
        future->set(vm, FutureSourceTraceback, stack);
    }
    return true;
}

static bool addToAwaitedBy(JSGlobalObject* globalObject, PyRecord* future, JSValue waiter)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue awaitedBy = future->get(FutureAwaitedBy);
    // Most are awaited by one thing only.
    if (!awaitedBy) {
        future->set(vm, FutureAwaitedBy, waiter);
        return true;
    }
    if (hasFlag(future, FlagAwaitedByIsSet)) {
        PySet::from(awaitedBy)->add(globalObject, waiter);
        return !scope.exception();
    }
    PySet* set = PySet::create(globalObject);
    set->add(globalObject, waiter);
    RETURN_IF_EXCEPTION(scope, false);
    set->add(globalObject, awaitedBy);
    RETURN_IF_EXCEPTION(scope, false);
    future->set(vm, FutureAwaitedBy, set);
    setFlag(future, FlagAwaitedByIsSet, true);
    return true;
}

static bool discardFromAwaitedBy(JSGlobalObject* globalObject, PyRecord* future, JSValue waiter)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue awaitedBy = future->get(FutureAwaitedBy);
    if (!awaitedBy)
        return true;
    if (awaitedBy == waiter) {
        future->set(vm, FutureAwaitedBy, { });
        return true;
    }
    if (hasFlag(future, FlagAwaitedByIsSet))
        PySet::from(awaitedBy)->remove(globalObject, waiter);
    return !scope.exception();
}

static bool setFutureResult(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future, JSValue result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isAlive(future)) {
        raiseRuntimeError(globalObject, scope, "Future object is not initialized."_s);
        return false;
    }
    if (futureState(future) != StatePending) {
        raiseInvalidState(globalObject, scope, state, "invalid state"_s);
        return false;
    }
    future->set(vm, FutureResult, result);
    setFutureState(future, StateFinished);
    if (isTaskRecord(future)) {
        unregisterTask(globalObject, future);
        finalizerHasNothingToDo(globalObject, state, future);
    }
    RELEASE_AND_RETURN(scope, scheduleCallbacks(globalObject, state, future));
}

static bool setFutureException(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future, JSValue given)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (futureState(future) != StatePending) {
        raiseInvalidState(globalObject, scope, state, "invalid state"_s);
        return false;
    }
    JSValue value = given;
    if (auto* type = PyType::tryFrom(given); type && type->has(TypeFlag::ExceptionSubclass)) {
        value = call(globalObject, type);
        RETURN_IF_EXCEPTION(scope, false);
        if (futureState(future) != StatePending) {
            raiseInvalidState(globalObject, scope, state, "invalid state"_s);
            return false;
        }
    }
    auto* exception = PyException::tryFrom(value);
    if (!exception) {
        raiseTypeError(globalObject, scope, "invalid exception object"_s);
        return false;
    }
    if (exception->type()->isSubtypeOf(rt.StopIteration())) {
        PyException* error = newException(globalObject, rt.RuntimeError(), "StopIteration interacts badly with generators and cannot be raised into a Future"_s);
        RETURN_IF_EXCEPTION(scope, false);
        error->setCause(vm, exception);
        error->setSuppressContext(true);
        error->setContext(vm, exception);
        exception = error;
    }
    future->set(vm, FutureException, exception);
    future->set(vm, FutureExceptionTraceback, exception->traceback());
    setFutureState(future, StateFinished);
    bool isScheduled = scheduleCallbacks(globalObject, state, future);
    RETURN_IF_EXCEPTION(scope, false);
    ASSERT_UNUSED(isScheduled, isScheduled);
    setFlag(future, FlagLogTraceback, true);
    finalizerHasSomethingToDo(globalObject, future);
    return true;
}

// The CancelledError to raise for a future that was cancelled. Empty: an exception.
static JSValue makeCancelledError(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future)
{
    if (JSValue exception = future->get(FutureCancelledError)) {
        future->set(getVM(globalObject), FutureCancelledError, { });
        return exception;
    }
    JSValue message = future->get(FutureCancelMessage);
    if (!message || isNone(message))
        return call(globalObject, state->get(StateCancelledError));
    return call(globalObject, state->get(StateCancelledError), message);
}

static void raiseCancelledError(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue exception = makeCancelledError(globalObject, state, future);
    RETURN_IF_EXCEPTION(scope, void());
    raiseException(globalObject, scope, exception);
}

enum class Outcome : uint8_t { Result, Exception, Raised };
// What future.result() comes to: the result; an exception for the caller to raise; or one that is raised already.
static Outcome getFutureResult(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future, JSValue& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (futureState(future) == StateCancelled) {
        scope.release();
        raiseCancelledError(globalObject, state, future);
        return Outcome::Raised;
    }
    if (futureState(future) != StateFinished) {
        raiseInvalidState(globalObject, scope, state, "Result is not set."_s);
        return Outcome::Raised;
    }
    setFlag(future, FlagLogTraceback, false);
    if (JSValue exception = future->get(FutureException)) {
        PyException::from(exception)->setTraceback(vm, future->get(FutureExceptionTraceback));
        future->set(vm, FutureExceptionTraceback, { });
        result = exception;
        return Outcome::Exception;
    }
    result = future->get(FutureResult);
    return Outcome::Result;
}

static bool addDoneCallback(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future, JSValue callback, JSValue context)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isAlive(future)) {
        raiseRuntimeError(globalObject, scope, "uninitialized Future object"_s);
        return false;
    }
    if (futureState(future) != StatePending)
        RELEASE_AND_RETURN(scope, callSoon(globalObject, state, future->get(FutureLoop), callback, future, context));
    if (!future->get(FutureCallbacks) && !future->get(FutureCallback0)) {
        future->set(vm, FutureCallback0, callback);
        future->set(vm, FutureContext0, context);
        return true;
    }
    PyTuple* pair = PyTuple::create(globalObject, { callback, context });
    JSValue callbacks = future->get(FutureCallbacks);
    if (!callbacks) {
        callbacks = newList(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        future->set(vm, FutureCallbacks, callbacks);
    }
    listAppend(globalObject, asList(callbacks), pair);
    return !scope.exception();
}

// Future.cancel(). Empty: an exception.
static JSValue cancelFuture(JSGlobalObject* globalObject, PyRecord* state, PyRecord* future, JSValue message)
{
    setFlag(future, FlagLogTraceback, false);
    if (futureState(future) != StatePending)
        return jsBoolean(false);
    setFutureState(future, StateCancelled);
    future->set(getVM(globalObject), FutureCancelMessage, message);
    if (isTaskRecord(future)) {
        unregisterTask(globalObject, future);
        finalizerHasNothingToDo(globalObject, state, future);
    }
    if (!scheduleCallbacks(globalObject, state, future))
        return { };
    return jsBoolean(true);
}

#define ENSURE_FUTURE_ALIVE(future) \
    do { \
        if (!isAlive(future)) [[unlikely]] \
            return raiseRuntimeError(globalObject, scope, "Future object is not initialized."_s); \
    } while (false)

static JSValue futureNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    PyType* type = PyType::from(cls);
    JSValue stateValue = hiddenField(globalObject, type, 0);
    PyRecord* state = stateValue ? recordOf(stateValue) : moduleState(globalObject);
    if (!ensureIsInitialized(globalObject, state))
        return { };
    PyType* taskType = typeIn(state, StateTaskType);
    bool isTask = type == taskType || type->isSubtypeOf(taskType);
    PyRecord* future = PyRecord::create(globalObject, type, isTask ? TaskFieldCount : FutureFieldCount);
    future->set(getVM(globalObject), FutureModuleState, state);
    return future;
}

static JSValue futureInit(JSGlobalObject* globalObject, JSValue self, JSValue loop)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    PyRecord* state = stateOf(future);
    if (failed(scope, initializeFuture(globalObject, state, future, loop)))
        return { };
    finalizerHasNothingToDo(globalObject, state, future);
    return jsUndefined();
}

static JSValue futureResult(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    PyRecord* state = stateOf(future);
    if (!isAlive(future))
        return raiseInvalidState(globalObject, scope, state, "Future object is not initialized."_s);
    JSValue result;
    switch (getFutureResult(globalObject, state, future, result)) {
    case Outcome::Result:
        return result;
    case Outcome::Exception:
        return raiseException(globalObject, scope, result);
    case Outcome::Raised:
        break;
    }
    scope.release();
    return { };
}

static JSValue futureException(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    PyRecord* state = stateOf(future);
    if (!isAlive(future))
        return raiseInvalidState(globalObject, scope, state, "Future object is not initialized."_s);
    if (futureState(future) == StateCancelled) {
        scope.release();
        raiseCancelledError(globalObject, state, future);
        return { };
    }
    if (futureState(future) != StateFinished)
        return raiseInvalidState(globalObject, scope, state, "Exception is not set."_s);
    JSValue exception = future->get(FutureException);
    if (!exception)
        return jsUndefined();
    setFlag(future, FlagLogTraceback, false);
    return exception;
}

static JSValue futureSetResult(JSGlobalObject* globalObject, JSValue self, JSValue result)
{
    PyRecord* future = recordOf(self);
    return setFutureResult(globalObject, stateOf(future), future, result) ? jsUndefined() : JSValue();
}

static JSValue futureSetException(JSGlobalObject* globalObject, JSValue self, JSValue exception)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);
    scope.release();
    RELEASE_AND_RETURN(scope, setFutureException(globalObject, stateOf(future), future, exception) ? jsUndefined() : JSValue());
}

static JSValue futureAddDoneCallback(JSGlobalObject* globalObject, JSValue self, JSValue callback, JSValue context)
{
    PyRecord* future = recordOf(self);
    if (!context)
        context = copyCurrentContext(globalObject);
    return addDoneCallback(globalObject, stateOf(future), future, callback, context) ? jsUndefined() : JSValue();
}

static JSValue futureRemoveDoneCallback(JSGlobalObject* globalObject, JSValue self, JSValue callback)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);

    // A comparison may run anything, which may change the callbacks.
    unsigned removed = 0;
    if (JSValue callback0 = future->get(FutureCallback0)) {
        bool isSame = isEqual(globalObject, callback0, callback);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame) {
            future->set(vm, FutureCallback0, { });
            future->set(vm, FutureContext0, { });
            removed = 1;
        }
    }
    if (!future->get(FutureCallbacks))
        return jsNumber(removed);
    unsigned length = listLength(asList(future->get(FutureCallbacks)));
    JSArray* kept = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; future->get(FutureCallbacks) && i < listLength(asList(future->get(FutureCallbacks))); ++i) {
        JSValue pair = listGet(asList(future->get(FutureCallbacks)), i);
        bool isSame = isEqual(globalObject, PyTuple::from(pair)->at(0), callback);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame)
            continue;
        listAppend(globalObject, kept, pair);
        RETURN_IF_EXCEPTION(scope, { });
    }
    unsigned keptCount = listLength(kept);
    if (!keptCount || !future->get(FutureCallbacks)) {
        future->set(vm, FutureCallbacks, { });
        return jsNumber(length + removed);
    }
    length = listLength(asList(future->get(FutureCallbacks)));
    future->set(vm, FutureCallbacks, kept);
    return jsNumber(static_cast<int64_t>(length) - keptCount + removed);
}

static JSValue futureCancel(JSGlobalObject* globalObject, JSValue self, JSValue message)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);
    RELEASE_AND_RETURN(scope, cancelFuture(globalObject, stateOf(future), future, orNone(message)));
}

static JSValue futureCancelled(JSGlobalObject*, JSValue self)
{
    PyRecord* future = recordOf(self);
    return jsBoolean(isAlive(future) && futureState(future) == StateCancelled);
}

static JSValue futureDone(JSGlobalObject*, JSValue self)
{
    PyRecord* future = recordOf(self);
    return jsBoolean(isAlive(future) && futureState(future) != StatePending);
}

static JSValue futureGetLoop(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);
    return future->get(FutureLoop);
}

static JSValue futureMakeCancelledError(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* future = recordOf(self);
    return makeCancelledError(globalObject, stateOf(future), future);
}

static JSValue futureRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);
    RELEASE_AND_RETURN(scope, call(globalObject, stateOf(future)->get(StateFutureRepr), self));
}

// ---- The attributes of a future ----

static void raiseCannotDelete(JSGlobalObject* globalObject, JSValue self, ASCIILiteral name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raise(globalObject, scope, runtime(globalObject).AttributeError(), makeString("attribute '"_s, name, "' of '"_s, typeName(globalObject, self), "' objects cannot be deleted"_s));
}

static JSValue futureGetAwaitedBy(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    JSValue awaitedBy = future->get(FutureAwaitedBy);
    if (!awaitedBy)
        return jsUndefined();
    if (hasFlag(future, FlagAwaitedByIsSet))
        RELEASE_AND_RETURN(scope, PySet::from(awaitedBy)->copy(globalObject, runtime(globalObject).frozensetType()));
    PySet* set = PySet::createFrozen(globalObject);
    set->add(globalObject, awaitedBy);
    RETURN_IF_EXCEPTION(scope, { });
    return set;
}

static JSValue futureGetBlocking(JSGlobalObject*, JSValue self)
{
    PyRecord* future = recordOf(self);
    return jsBoolean(isAlive(future) && hasFlag(future, FlagBlocking));
}

static void futureSetBlocking(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value)
        RELEASE_AND_RETURN(scope, raiseCannotDelete(globalObject, self, "_asyncio_future_blocking"_s));
    PyRecord* future = recordOf(self);
    if (!isAlive(future)) {
        raiseRuntimeError(globalObject, scope, "Future object is not initialized."_s);
        return;
    }
    bool isBlocking = isTrue(globalObject, value);
    RETURN_IF_EXCEPTION(scope, void());
    setFlag(future, FlagBlocking, isBlocking);
}

static JSValue futureGetLogTraceback(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);
    return jsBoolean(hasFlag(future, FlagLogTraceback));
}

static void futureSetLogTraceback(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value)
        RELEASE_AND_RETURN(scope, raiseCannotDelete(globalObject, self, "_log_traceback"_s));
    bool logs = isTrue(globalObject, value);
    RETURN_IF_EXCEPTION(scope, void());
    if (logs) {
        raiseValueError(globalObject, scope, "_log_traceback can only be set to False"_s);
        return;
    }
    setFlag(recordOf(self), FlagLogTraceback, false);
}

static JSValue futureGetLoopAttribute(JSGlobalObject*, JSValue self) { return orNone(recordOf(self)->get(FutureLoop)); }

static JSValue futureGetCallbacks(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);
    JSValue callback0 = future->get(FutureCallback0);
    JSValue callbacks = future->get(FutureCallbacks);
    if (!callback0 && (!callbacks || !listLength(asList(callbacks))))
        return jsUndefined();
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (callback0) {
        listAppend(globalObject, result, PyTuple::create(globalObject, { callback0, future->get(FutureContext0) }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (callbacks) {
        listExtend(globalObject, result, callbacks);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue futureGetResultAttribute(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);
    return orNone(future->get(FutureResult));
}

static JSValue futureGetExceptionAttribute(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);
    return orNone(future->get(FutureException));
}

static JSValue futureGetSourceTraceback(JSGlobalObject*, JSValue self) { return orNone(recordOf(self)->get(FutureSourceTraceback)); }
static JSValue futureGetCancelMessage(JSGlobalObject*, JSValue self) { return orNone(recordOf(self)->get(FutureCancelMessage)); }

static void futureSetCancelMessage(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    if (!value)
        return raiseCannotDelete(globalObject, self, "_cancel_message"_s);
    recordOf(self)->set(getVM(globalObject), FutureCancelMessage, value);
}

static JSValue futureGetState(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);
    PyRecord* state = stateOf(future);
    switch (futureState(future)) {
    case StatePending:
        return state->get(StateNamePending);
    case StateCancelled:
        return state->get(StateNameCancelled);
    case StateFinished:
        return state->get(StateNameFinished);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- What `await future` runs ----

enum FutureIterField : unsigned { FutureIterFuture, FutureIterFieldCount }; // Empty: it is over.

static JSValue futureIter(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* future = recordOf(self);
    ENSURE_FUTURE_ALIVE(future);
    PyRecord* iterator = PyRecord::create(globalObject, typeIn(stateOf(future), StateFutureIterType), FutureIterFieldCount);
    iterator->set(vm, FutureIterFuture, future);
    return iterator;
}

static JSValue futureIterSelf(JSGlobalObject*, JSValue self) { return self; }

JSValue stepFutureIter(JSGlobalObject* globalObject, JSValue iterator, JSValue& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue futureValue = recordOf(iterator)->get(FutureIterFuture);
    if (!futureValue)
        return raiseStopIteration(globalObject, scope);
    PyRecord* future = recordOf(futureValue);
    if (futureState(future) == StatePending) {
        if (!hasFlag(future, FlagBlocking)) {
            setFlag(future, FlagBlocking, true);
            return future;
        }
        return raiseRuntimeError(globalObject, scope, "await wasn't used with future"_s);
    }
    JSValue value;
    switch (getFutureResult(globalObject, stateOf(future), future, value)) {
    case Outcome::Result:
        result = value;
        return { };
    case Outcome::Exception:
        return raiseException(globalObject, scope, value);
    case Outcome::Raised:
        break;
    }
    scope.release();
    return { };
}

static JSValue futureIterNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result;
    JSValue yielded = stepFutureIter(globalObject, self, result);
    if (!result)
        RELEASE_AND_RETURN(scope, yielded);
    // StopIteration(result), also for None.
    Runtime& rt = runtime(globalObject);
    PyException* stop = PyException::create(vm, rt.structure<PyException>(), rt.StopIteration());
    stop->setArgs(vm, PyTuple::create(globalObject, { result }));
    stop->setField(vm, ExceptionField::stopIterationValue, result);
    return raiseException(globalObject, scope, stop);
}

bool isNextOfFutureIter(JSValue next)
{
    auto* native = PyNativeFunction::tryFrom(next);
    return native && native->def().pointer.f1 == futureIterNext;
}

// What is sent does not matter: the value of `await future` is future.result().
static JSValue futureIterSend(JSGlobalObject* globalObject, JSValue self, JSValue) { return futureIterNext(globalObject, self); }

static JSValue futureIterThrow(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "FutureIter.throw"_s, args)) || failed(scope, checkPositional(globalObject, "throw"_s, args, 1, 3)))
        return { };
    if (args.size() > 1) {
        bool goOn = warn(globalObject, rt.DeprecationWarning(), "the (type, exc, tb) signature of throw() is deprecated, use the single-arg signature instead."_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (!goOn)
            return { };
    }
    JSValue type = args[0];
    JSValue value = args.get(1);
    JSValue traceback = args.get(2);
    if (value && isNone(value))
        value = { };
    if (traceback && isNone(traceback))
        traceback = { };
    if (traceback && !PyTraceback::is(traceback))
        return raiseTypeError(globalObject, scope, "throw() third argument must be a traceback"_s);

    JSValue exception;
    if (auto* cls = PyType::tryFrom(type); cls && cls->has(TypeFlag::ExceptionSubclass)) {
        if (value && PyException::is(value) && PyException::from(value)->type()->isSubtypeOf(cls))
            exception = value;
        else if (!value)
            exception = call(globalObject, cls);
        else if (auto* tuple = PyTuple::tryFrom(value))
            exception = call(globalObject, cls, CallArgs(tuple->span()));
        else
            exception = call(globalObject, cls, value);
        if (!scope.exception() && !PyException::is(exception))
            raiseTypeError(globalObject, scope, "calling "_s, reprForMessage(globalObject, cls), " should have returned an instance of BaseException, not "_s, typeName(globalObject, exception));
        // If it cannot be made, what says so is raised in its place.
        if (scope.exception()) [[unlikely]] {
            exception = takePendingException(globalObject);
            if (!exception)
                return { };
        }
    } else if (PyException::is(type)) {
        if (value)
            return raiseTypeError(globalObject, scope, "instance exception may not have a separate value"_s);
        exception = type;
    } else
        return raiseTypeError(globalObject, scope, "exceptions must be classes deriving BaseException or instances of such a class"_s);
    if (traceback)
        PyException::from(exception)->setTraceback(vm, traceback);

    recordOf(self)->set(vm, FutureIterFuture, { });
    throwPythonException(globalObject, scope, exception);
    return { };
}

static JSValue futureIterClose(JSGlobalObject* globalObject, JSValue self)
{
    recordOf(self)->set(getVM(globalObject), FutureIterFuture, { });
    return jsUndefined();
}

static constexpr NativeDef futureIterMethods[] = {
    PY_SLOT("__iter__", futureIterSelf, "($self, /)"),
    PY_SLOT("__next__", futureIterNext, "($self, /)"),
    PY_METHOD("send", futureIterSend, "($self, value, /)"),
    PY_METHOD("throw", futureIterThrow, "($self, *args)"),
    PY_METHOD("close", futureIterClose, "($self, /)"),
};
static constexpr TypeDef futureIterTypeDef { "_asyncio.FutureIter"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, futureIterMethods, { }, { } };

static constexpr NativeDef futureMethods[] = {
    PY_NEW(futureNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", futureInit, "($self, /, *, loop=None)"),
    PY_SLOT("__del__", futureDel, "($self, /)"),
    PY_SLOT("__repr__", futureRepr, "($self, /)"),
    PY_SLOT("__iter__", futureIter, "($self, /)"),
    PY_SLOT("__await__", futureIter, "($self, /)"),
    PY_METHOD("result", futureResult, "($self, /)"),
    PY_METHOD("exception", futureException, "($self, /)"),
    PY_METHOD("set_result", futureSetResult, "($self, result, /)"),
    PY_METHOD("set_exception", futureSetException, "($self, exception, /)"),
    PY_METHOD("add_done_callback", futureAddDoneCallback, "($self, fn, /, *, context=<unrepresentable>)"),
    PY_METHOD("remove_done_callback", futureRemoveDoneCallback, "($self, fn, /)"),
    PY_METHOD("cancel", futureCancel, "($self, /, msg=None)"),
    PY_METHOD("cancelled", futureCancelled, "($self, /)"),
    PY_METHOD("done", futureDone, "($self, /)"),
    PY_METHOD("get_loop", futureGetLoop, "($self, /)"),
    PY_METHOD("_make_cancelled_error", futureMakeCancelledError, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};
static constexpr NativeGetSet futureGetSets[] = {
    { "_state"_s, futureGetState },
    { "_asyncio_future_blocking"_s, futureGetBlocking, futureSetBlocking },
    { "_loop"_s, futureGetLoopAttribute },
    { "_callbacks"_s, futureGetCallbacks },
    { "_result"_s, futureGetResultAttribute },
    { "_exception"_s, futureGetExceptionAttribute },
    { "_log_traceback"_s, futureGetLogTraceback, futureSetLogTraceback },
    { "_source_traceback"_s, futureGetSourceTraceback },
    { "_cancel_message"_s, futureGetCancelMessage, futureSetCancelMessage },
    { "_asyncio_awaited_by"_s, futureGetAwaitedBy },
};
static constexpr TypeDef futureTypeDef { "_asyncio.Future"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasDict, TypeFlag::HasWeakref }, futureMethods, futureGetSets, { } };
static const TypeDef& futureTypeDefinition() { return futureTypeDef; }

// ---- Tasks ----

static JSValue stepTask(JSGlobalObject*, PyRecord* state, PyRecord* task, JSValue exception);

// What the loop is given to call for the next step of a task. Fields: the task; the exception to throw into the coroutine, or empty.
enum StepWrapperField : unsigned { StepWrapperTask, StepWrapperException, StepWrapperFieldCount };

static JSValue stepWrapperCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.keywordCount())
        return raiseTypeError(globalObject, scope, "function takes no keyword arguments"_s);
    if (args.size())
        return raiseTypeError(globalObject, scope, "function takes no positional arguments"_s);
    PyRecord* wrapper = recordOf(self);
    PyRecord* task = recordOf(wrapper->get(StepWrapperTask));
    RELEASE_AND_RETURN(scope, stepTask(globalObject, stateOf(task), task, wrapper->get(StepWrapperException)));
}

static JSValue stepWrapperGetSelf(JSGlobalObject*, JSValue self) { return recordOf(self)->get(StepWrapperTask); }

static constexpr NativeDef stepWrapperMethods[] = {
    PY_SLOT("__call__", stepWrapperCall, "($self, /, *args, **kwargs)"),
};
static constexpr NativeGetSet stepWrapperGetSets[] = {
    { "__self__"_s, stepWrapperGetSelf },
};
static constexpr TypeDef stepWrapperTypeDef { "_asyncio.TaskStepMethWrapper"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, stepWrapperMethods, stepWrapperGetSets, { } };

// loop.call_soon(task.__step[, exception], context=task's). False: an exception.
static bool callStepSoon(JSGlobalObject* globalObject, PyRecord* state, PyRecord* task, JSValue exception)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // The one made last time will do, if it is still there. The task does not hold on to it: the two would be a cycle, which a
    // task is not in CPython, and what is in a cycle is finalized in another order and counted by gc.collect().
    JSValue wrapper;
    PyWeakRef* last = nullptr;
    if (!exception) {
        if (JSValue field = task->get(TaskStep)) {
            last = PyWeakRef::from(field);
            if (JSCell* cell = last->referent())
                wrapper = cell;
        }
    }
    if (!wrapper) {
        PyRecord* record = PyRecord::create(globalObject, typeIn(state, StateStepWrapperType), StepWrapperFieldCount);
        record->set(vm, StepWrapperTask, task);
        record->set(vm, StepWrapperException, exception);
        if (!exception) {
            if (!last) {
                last = PyWeakRef::create(vm, rt.weakRefStructure(), rt.weakRefType(), nullptr, { });
                last->setIsInternal();
                task->set(vm, TaskStep, last);
            }
            last->setReferent(record);
        }
        wrapper = record;
    }
    return callSoon(globalObject, state, task->get(FutureLoop), wrapper, { }, task->get(TaskContext));
}

// The next step raises RuntimeError(message) in the coroutine. Empty: an exception.
static JSValue setErrorSoon(JSGlobalObject* globalObject, PyRecord* state, PyRecord* task, const String& message)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* error = newException(globalObject, runtime(globalObject).RuntimeError(), message);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    RELEASE_AND_RETURN(scope, callStepSoon(globalObject, state, task, error) ? jsUndefined() : JSValue());
}

static bool enterTask(JSGlobalObject* globalObject, PyRecord* state, JSValue loop, JSValue task)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    useThisThread(globalObject, state);
    if (state->get(StateRunningLoop) != loop) [[unlikely]] {
        raiseRuntimeError(globalObject, scope, "loop "_s, reprForMessage(globalObject, loop), " is not the running loop"_s);
        return false;
    }
    if (JSValue current = state->get(StateCurrentTask)) [[unlikely]] {
        raiseRuntimeError(globalObject, scope, "Cannot enter into task "_s, reprForMessage(globalObject, task), " while another task "_s, reprForMessage(globalObject, current), " is being executed."_s);
        return false;
    }
    state->set(vm, StateCurrentTask, task);
    return true;
}

static bool leaveTask(JSGlobalObject* globalObject, PyRecord* state, JSValue loop, JSValue task)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    useThisThread(globalObject, state);
    if (state->get(StateRunningLoop) != loop) [[unlikely]] {
        raiseRuntimeError(globalObject, scope, "loop "_s, reprForMessage(globalObject, loop), " is not the running loop"_s);
        return false;
    }
    if (state->get(StateCurrentTask) != task) [[unlikely]] {
        raiseRuntimeError(globalObject, scope, "Invalid attempt to leave task "_s, reprForMessage(globalObject, task), " while task "_s, reprForMessage(globalObject, orNone(state->get(StateCurrentTask))), " is entered."_s);
        return false;
    }
    state->set(vm, StateCurrentTask, { });
    return true;
}

// The task that was current, or None. Empty: an exception.
static JSValue swapCurrentTask(JSGlobalObject* globalObject, PyRecord* state, JSValue loop, JSValue task)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    useThisThread(globalObject, state);
    if (state->get(StateRunningLoop) != loop)
        return raiseRuntimeError(globalObject, scope, "loop "_s, reprForMessage(globalObject, loop), " is not the running loop"_s);
    JSValue previous = state->get(StateCurrentTask);
    state->set(vm, StateCurrentTask, isNone(task) ? JSValue() : task);
    return orNone(previous);
}

// 1, 0, or -1 for an exception.
static int isCoroutine(JSGlobalObject* globalObject, PyRecord* state, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* generator = PyGenerator::tryFrom(object); generator && generator->kind() == PyGenerator::Kind::Coroutine) [[likely]]
        return 1;
    // What else asyncio.iscoroutine() accepts, which is remembered by type.
    PySet* types = PySet::from(state->get(StateCoroutineTypes));
    PyType* type = typeOf(globalObject, object);
    bool isKnown = types->contains(globalObject, type);
    RETURN_IF_EXCEPTION(scope, -1);
    if (isKnown)
        return 1;
    JSValue answer = call(globalObject, state->get(StateIsCoroutine), object);
    RETURN_IF_EXCEPTION(scope, -1);
    bool isOne = isTrue(globalObject, answer);
    RETURN_IF_EXCEPTION(scope, -1);
    if (!isOne)
        return 0;
    if (types->size() < 100) {
        types->add(globalObject, type);
        RETURN_IF_EXCEPTION(scope, -1);
    }
    return 1;
}

// If a StopIteration is propagating, it stops, and this is its value. Empty: something else is.
static JSValue takeStopIterationValue(JSGlobalObject* globalObject)
{
    if (!pendingExceptionMatches(globalObject, runtime(globalObject).StopIteration()))
        return { };
    return orNone(PyException::from(takePendingException(globalObject))->field(ExceptionField::stopIterationValue));
}

// coroutine.send(None), or coroutine.throw(exception). What it yields; or empty, with what it returned in `returned`; or, with
// that empty too, an exception.
static JSValue resumeCoroutine(JSGlobalObject* globalObject, JSValue coroutine, JSValue exception, JSValue& returned)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* generator = PyGenerator::tryFrom(coroutine); generator && generator->kind() != PyGenerator::Kind::AsyncGenerator) [[likely]] {
        if (exception)
            RELEASE_AND_RETURN(scope, generatorThrow(globalObject, generator, exception, returned));
        RELEASE_AND_RETURN(scope, generatorSend(globalObject, generator, jsUndefined(), returned));
    }
    JSValue yielded;
    if (exception)
        yielded = callMethod(globalObject, coroutine, rt.names.throw_(), exception);
    else if (JSValue next = rt.typeOf(coroutine)->lookup(Special::Next); next && !isNone(next))
        yielded = callSpecial(globalObject, next, coroutine);
    else
        yielded = callMethod(globalObject, coroutine, rt.names.send(), jsUndefined());
    EXCEPTION_ASSERT(!scope.exception() || !yielded);
    if (yielded)
        return yielded;
    scope.release();
    returned = takeStopIterationValue(globalObject);
    return { };
}

static JSValue handleYielded(JSGlobalObject*, PyRecord* state, PyRecord* task, JSValue yielded);

// Task.__step(), but for entering and leaving the task. None. Empty: an exception.
static JSValue stepTaskEntered(JSGlobalObject* globalObject, PyRecord* state, PyRecord* task, JSValue exception)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (futureState(task) != StatePending)
        return raiseInvalidState(globalObject, scope, state, makeString("__step(): already done: "_s, reprForMessage(globalObject, task), ' ', reprForMessage(globalObject, orNone(exception))));

    if (hasFlag(task, FlagMustCancel)) {
        bool isCancellation = false;
        if (exception) {
            isCancellation = exceptionMatches(globalObject, exception, state->get(StateCancelledError));
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (!isCancellation) {
            exception = makeCancelledError(globalObject, state, task);
            RETURN_IF_EXCEPTION(scope, { });
        }
        setFlag(task, FlagMustCancel, false);
    }
    task->set(vm, TaskWaiter, { });

    JSValue coroutine = task->get(TaskCoroutine);
    if (!coroutine)
        return raiseRuntimeError(globalObject, scope, "uninitialized Task object"_s);

    JSValue returned;
    JSValue yielded = resumeCoroutine(globalObject, coroutine, exception, returned);
    EXCEPTION_ASSERT(!scope.exception() || !yielded);
    if (yielded)
        RELEASE_AND_RETURN(scope, handleYielded(globalObject, state, task, yielded));

    if (returned) {
        ASSERT(!scope.exception());
        scope.release();
        if (hasFlag(task, FlagMustCancel)) {
            // It was cancelled just before the coroutine came to its end.
            setFlag(task, FlagMustCancel, false);
            RELEASE_AND_RETURN(scope, cancelFuture(globalObject, state, task, task->get(FutureCancelMessage)) ? jsUndefined() : JSValue());
        }
        RELEASE_AND_RETURN(scope, setFutureResult(globalObject, state, task, returned) ? jsUndefined() : JSValue());
    }

    JSValue raised = takePendingException(globalObject);
    if (!raised) // The host is ending what runs here.
        return { };
    if (typeOf(globalObject, raised)->isSubtypeOf(typeIn(state, StateCancelledError))) {
        task->set(vm, FutureCancelledError, raised);
        scope.release();
        RELEASE_AND_RETURN(scope, cancelFuture(globalObject, state, task, { }) ? jsUndefined() : JSValue());
    }
    setFutureException(globalObject, state, task, raised);
    RETURN_IF_EXCEPTION(scope, { });
    PyType* raisedType = typeOf(globalObject, raised);
    if (raisedType->isSubtypeOf(rt.KeyboardInterrupt()) || raisedType->isSubtypeOf(rt.SystemExit())) {
        throwPythonException(globalObject, scope, raised);
        return { };
    }
    return jsUndefined();
}

// If the task is to be cancelled, so is the future it has just begun to wait for. False: an exception.
static bool cancelWaiterIfMustCancel(JSGlobalObject* globalObject, PyRecord* state, PyRecord* task, JSValue waiter)
{
    if (!hasFlag(task, FlagMustCancel)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue answer = callMethod(globalObject, waiter, nameIn(state, StateNameCancel), orNone(task->get(FutureCancelMessage)));
    RETURN_IF_EXCEPTION(scope, false);
    bool isCancelled = isTrue(globalObject, answer);
    RETURN_IF_EXCEPTION(scope, false);
    if (isCancelled)
        setFlag(task, FlagMustCancel, false);
    return true;
}

static JSValue wakeupOf(JSGlobalObject* globalObject, PyRecord* state, PyRecord* task)
{
    if (JSValue wakeup = task->get(TaskWakeup)) [[likely]]
        return wakeup;
    JSValue wakeup = PyNativeFunction::createBound(globalObject, uncheckedDowncast<PyNativeFunction>(state->get(StateWakeupMethod).asCell()), task);
    task->set(getVM(globalObject), TaskWakeup, wakeup);
    return wakeup;
}

// What the coroutine of the task has yielded says what the task waits for.
static JSValue handleYielded(JSGlobalObject* globalObject, PyRecord* state, PyRecord* task, JSValue yielded)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto describe = [&](JSValue value) {
        return reprForMessage(globalObject, value);
    };
    auto differentLoop = [&] {
        return setErrorSoon(globalObject, state, task, makeString("Task "_s, describe(task), " got Future "_s, describe(yielded), " attached to a different loop"_s));
    };
    auto yieldInsteadOfYieldFrom = [&] {
        return setErrorSoon(globalObject, state, task, makeString("yield was used instead of yield from in task "_s, describe(task), " with "_s, describe(yielded)));
    };

    if (yielded == JSValue(task))
        RELEASE_AND_RETURN(scope, setErrorSoon(globalObject, state, task, makeString("Task cannot await on itself: "_s, describe(task))));

    if (PyRecord* future = asExactFuture(state, yielded)) [[likely]] {
        if (future->get(FutureLoop) != task->get(FutureLoop))
            RELEASE_AND_RETURN(scope, differentLoop());
        if (!hasFlag(future, FlagBlocking))
            RELEASE_AND_RETURN(scope, yieldInsteadOfYieldFrom());
        addToAwaitedBy(globalObject, future, task);
        RETURN_IF_EXCEPTION(scope, { });
        setFlag(future, FlagBlocking, false);
        addDoneCallback(globalObject, state, future, wakeupOf(globalObject, state, task), task->get(TaskContext));
        RETURN_IF_EXCEPTION(scope, { });
        task->set(vm, TaskWaiter, future);
        scope.release();
        RELEASE_AND_RETURN(scope, cancelWaiterIfMustCancel(globalObject, state, task, future) ? jsUndefined() : JSValue());
    }

    // A bare yield gives up control for one turn of the loop.
    if (isNone(yielded)) {
        scope.release();
        RELEASE_AND_RETURN(scope, callStepSoon(globalObject, state, task, { }) ? jsUndefined() : JSValue());
    }

    // Anything that says it is like a future.
    JSValue blockingValue = getAttrIfPresent(globalObject, yielded, nameIn(state, StateNameBlocking));
    RETURN_IF_EXCEPTION(scope, { });
    if (blockingValue && !isNone(blockingValue)) {
        bool isBlocking = isTrue(globalObject, blockingValue);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue loop = loopOfFuture(globalObject, state, yielded);
        RETURN_IF_EXCEPTION(scope, { });
        if (loop != task->get(FutureLoop))
            RELEASE_AND_RETURN(scope, differentLoop());
        if (!isBlocking)
            RELEASE_AND_RETURN(scope, yieldInsteadOfYieldFrom());
        if (PyRecord* future = asFuture(state, yielded)) {
            addToAwaitedBy(globalObject, future, task);
            RETURN_IF_EXCEPTION(scope, { });
        }
        setAttr(globalObject, yielded, nameIn(state, StateNameBlocking), jsBoolean(false));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue addCallback = getAttr(globalObject, yielded, nameIn(state, StateNameAddDoneCallback));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue arguments[] = { wakeupOf(globalObject, state, task), task->get(TaskContext) };
        call(globalObject, addCallback, CallArgs(arguments, 1, uncheckedDowncast<PyTuple>(state->get(StateContextKeyword).asCell())));
        RETURN_IF_EXCEPTION(scope, { });
        task->set(vm, TaskWaiter, yielded);
        scope.release();
        RELEASE_AND_RETURN(scope, cancelWaiterIfMustCancel(globalObject, state, task, yielded) ? jsUndefined() : JSValue());
    }

    bool isGenerator = isInstance(globalObject, yielded, rt.generatorType());
    RETURN_IF_EXCEPTION(scope, { });
    if (isGenerator)
        RELEASE_AND_RETURN(scope, setErrorSoon(globalObject, state, task, makeString("yield was used instead of yield from for generator in task "_s, describe(task), " with "_s, describe(yielded))));
    RELEASE_AND_RETURN(scope, setErrorSoon(globalObject, state, task, makeString("Task got bad yield: "_s, describe(yielded))));
}

// `first` was raised, then put aside for something that may have raised too: that goes on, with `first` as its context, or else `first` does.
static void raiseChained(JSGlobalObject* globalObject, JSValue first)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (scope.exception()) {
        JSValue second = takePendingException(globalObject);
        if (!second)
            return;
        if (auto* exception = PyException::tryFrom(second); exception && second != first)
            exception->setContext(vm, first);
        throwPythonException(globalObject, scope, second);
        return;
    }
    throwPythonException(globalObject, scope, first);
}

static JSValue stepTask(JSGlobalObject* globalObject, PyRecord* state, PyRecord* task, JSValue exception)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue loop = task->get(FutureLoop);
    if (failed(scope, enterTask(globalObject, state, loop, task)))
        return { };
    JSValue result = stepTaskEntered(globalObject, state, task, exception);
    if (failed(scope, result)) [[unlikely]] {
        JSValue raised = takePendingException(globalObject);
        if (!raised)
            return { };
        leaveTask(globalObject, state, loop, task);
        scope.release();
        raiseChained(globalObject, raised);
        return { };
    }
    scope.release();
    RELEASE_AND_RETURN(scope, leaveTask(globalObject, state, loop, task) ? result : JSValue());
}

// The first step of the task, at once, within whatever is running. False: an exception.
static bool startEagerly(JSGlobalObject* globalObject, PyRecord* state, PyRecord* task)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue loop = task->get(FutureLoop);
    JSValue previous = swapCurrentTask(globalObject, state, loop, task);
    RETURN_IF_EXCEPTION(scope, false);
    // If it comes to its end without suspending, that takes it out again; otherwise it goes on as any task.
    registerTask(globalObject, task);
    if (failed(scope, enterContext(globalObject, task->get(TaskContext)))) {
        JSValue raised = takePendingException(globalObject);
        setFlag(task, FlagLogDestroyPending, false);
        swapCurrentTask(globalObject, state, loop, previous);
        if (scope.exception())
            takePendingException(globalObject);
        unregisterTask(globalObject, task);
        throwPythonException(globalObject, scope, raised);
        return false;
    }

    bool succeeded = true;
    JSValue raised;
    auto noteFailure = [&] {
        succeeded = false;
        JSValue exception = takePendingException(globalObject);
        if (!raised)
            raised = exception;
    };
    if (!stepTaskEntered(globalObject, state, task, { }))
        noteFailure();
    if (!swapCurrentTask(globalObject, state, loop, previous))
        noteFailure();
    if (failed(scope, exitContext(globalObject, task->get(TaskContext))))
        noteFailure();
    if (futureState(task) != StatePending)
        task->set(vm, TaskCoroutine, { });
    if (!succeeded && raised)
        throwPythonException(globalObject, scope, raised);
    return succeeded;
}

static JSValue taskInit(JSGlobalObject* globalObject, JSValue self, JSValue coroutine, JSValue loop, JSValue name, JSValue context, JSValue eagerStartValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* task = recordOf(self);
    PyRecord* state = stateOf(task);
    bool eagerStart = false;
    if (eagerStartValue) {
        eagerStart = isTrue(globalObject, eagerStartValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (failed(scope, initializeFuture(globalObject, state, task, loop)))
        return { };

    // JS never holds a coroutine: what it gets from an `async def`, as from an async function of its own, is a promise.
    if (typeOf(globalObject, coroutine) == runtime(globalObject).jsPromiseType()) {
        JSValue coroutineFor = eventLoopFunction(globalObject, "coroutine_for"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (coroutineFor) {
            coroutine = call(globalObject, coroutineFor, coroutine);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    int isOne = isCoroutine(globalObject, state, coroutine);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isOne)
        return raiseTypeError(globalObject, scope, "a coroutine was expected, got "_s, reprForMessage(globalObject, coroutine));

    if (!context || isNone(context))
        context = copyCurrentContext(globalObject);
    else if (!isContext(globalObject, context))
        return raiseTypeError(globalObject, scope, "a contextvars.Context was expected, got "_s, typeNameForMessage(globalObject, typeOf(globalObject, context)));
    task->set(vm, TaskContext, context);
    task->set(vm, TaskWaiter, { });
    setFlag(task, FlagMustCancel, false);
    setFlag(task, FlagLogDestroyPending, true);
    setCancelRequests(task, 0);
    task->set(vm, TaskCoroutine, coroutine);

    if (!name || isNone(name)) {
        // "Task-1" is put together when somebody asks.
        name = jsNumber(state->get(StateTaskNameCounter).asNumber() + 1);
        state->set(vm, StateTaskNameCounter, name);
    } else if (!name.isString()) {
        name = str(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
    }
    task->set(vm, TaskName, name);

    if (eagerStart) {
        JSValue answer = callMethod(globalObject, task->get(FutureLoop), nameIn(state, StateNameIsRunning));
        RETURN_IF_EXCEPTION(scope, { });
        if (answer.isTrue()) {
            scope.release();
            RELEASE_AND_RETURN(scope, startEagerly(globalObject, state, task) ? jsUndefined() : JSValue());
        }
    }
    if (failed(scope, callStepSoon(globalObject, state, task, { })))
        return { };
    registerTask(globalObject, task);
    return jsUndefined();
}

static JSValue taskGetLogDestroyPending(JSGlobalObject*, JSValue self) { return jsBoolean(hasFlag(recordOf(self), FlagLogDestroyPending)); }

static void taskSetLogDestroyPending(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value)
        RELEASE_AND_RETURN(scope, raiseCannotDelete(globalObject, self, "_log_destroy_pending"_s));
    bool logs = isTrue(globalObject, value);
    RETURN_IF_EXCEPTION(scope, void());
    setFlag(recordOf(self), FlagLogDestroyPending, logs);
}

static JSValue taskGetMustCancel(JSGlobalObject*, JSValue self) { return jsBoolean(hasFlag(recordOf(self), FlagMustCancel)); }
static JSValue taskGetCoroutine(JSGlobalObject*, JSValue self) { return orNone(recordOf(self)->get(TaskCoroutine)); }
static JSValue taskGetWaiter(JSGlobalObject*, JSValue self) { return orNone(recordOf(self)->get(TaskWaiter)); }
static JSValue taskGetContext(JSGlobalObject*, JSValue self) { return orNone(recordOf(self)->get(TaskContext)); }

static JSValue taskRepr(JSGlobalObject* globalObject, JSValue self) { return call(globalObject, stateOf(recordOf(self))->get(StateTaskRepr), self); }

static JSValue taskCancel(JSGlobalObject* globalObject, JSValue self, JSValue message)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* task = recordOf(self);
    message = orNone(message);
    setFlag(task, FlagLogTraceback, false);
    if (futureState(task) != StatePending)
        return jsBoolean(false);
    setCancelRequests(task, cancelRequests(task) + 1);
    if (JSValue waiter = task->get(TaskWaiter)) {
        JSValue answer = callMethod(globalObject, waiter, nameIn(stateOf(task), StateNameCancel), message);
        RETURN_IF_EXCEPTION(scope, { });
        bool isCancelled = isTrue(globalObject, answer);
        RETURN_IF_EXCEPTION(scope, { });
        if (isCancelled)
            return jsBoolean(true);
    }
    setFlag(task, FlagMustCancel, true);
    task->set(vm, FutureCancelMessage, message);
    return jsBoolean(true);
}

static JSValue taskCancelling(JSGlobalObject*, JSValue self) { return jsNumber(cancelRequests(recordOf(self))); }

static JSValue taskUncancel(JSGlobalObject*, JSValue self)
{
    PyRecord* task = recordOf(self);
    if (int32_t count = cancelRequests(task); count > 0) {
        setCancelRequests(task, count - 1);
        if (count == 1)
            setFlag(task, FlagMustCancel, false);
    }
    return jsNumber(cancelRequests(task));
}

static JSValue taskGetStack(JSGlobalObject* globalObject, JSValue self, JSValue limit)
{
    return call(globalObject, stateOf(recordOf(self))->get(StateTaskGetStack), self, orNone(limit));
}

static JSValue taskPrintStack(JSGlobalObject* globalObject, JSValue self, JSValue limit, JSValue file)
{
    return call(globalObject, stateOf(recordOf(self))->get(StateTaskPrintStack), self, orNone(limit), orNone(file));
}

static JSValue taskSetResult(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseRuntimeError(globalObject, scope, "Task does not support set_result operation"_s);
}

static JSValue taskSetException(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseRuntimeError(globalObject, scope, "Task does not support set_exception operation"_s);
}

static JSValue taskGetName(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* task = recordOf(self);
    JSValue name = task->get(TaskName);
    if (!name)
        return jsUndefined();
    if (name.isNumber()) {
        VM& vm = getVM(globalObject);
        name = makeStr(vm, makeString("Task-"_s, static_cast<uint64_t>(name.asNumber())));
        task->set(vm, TaskName, name);
    }
    return name;
}

static JSValue taskSetName(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isString()) {
        value = str(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    recordOf(self)->set(vm, TaskName, value);
    return jsUndefined();
}

// The future the task waited for is done.
static JSValue taskWakeup(JSGlobalObject* globalObject, JSValue self, JSValue futureValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* task = recordOf(self);
    PyRecord* state = stateOf(task);
    if (PyRecord* future = asFuture(state, futureValue)) {
        discardFromAwaitedBy(globalObject, future, task);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (PyRecord* future = asExactFuture(state, futureValue)) [[likely]] {
        JSValue result;
        switch (getFutureResult(globalObject, state, future, result)) {
        case Outcome::Result:
            RELEASE_AND_RETURN(scope, stepTask(globalObject, state, task, { }));
        case Outcome::Exception:
            RELEASE_AND_RETURN(scope, stepTask(globalObject, state, task, result));
        case Outcome::Raised:
            break;
        }
    } else {
        JSValue result = callMethod(globalObject, futureValue, nameIn(state, StateNameResult));
        if (!failed(scope, result))
            RELEASE_AND_RETURN(scope, stepTask(globalObject, state, task, { }));
    }
    JSValue raised = takePendingException(globalObject);
    if (!raised)
        return { };
    RELEASE_AND_RETURN(scope, stepTask(globalObject, state, task, raised));
}

static constexpr NativeDef taskWakeupDef = PY_METHOD("task_wakeup", taskWakeup, "($self, future, /)");

static constexpr NativeDef taskMethods[] = {
    PY_SLOT("__init__", taskInit, "($self, /, coro, *, loop=None, name=None, context=None, eager_start=False)"),
    PY_SLOT("__del__", taskDel, "($self, /)"),
    PY_SLOT("__repr__", taskRepr, "($self, /)"),
    PY_METHOD("set_result", taskSetResult, "($self, result, /)"),
    PY_METHOD("set_exception", taskSetException, "($self, exception, /)"),
    PY_METHOD("cancel", taskCancel, "($self, /, msg=None)"),
    PY_METHOD("cancelling", taskCancelling, "($self, /)"),
    PY_METHOD("uncancel", taskUncancel, "($self, /)"),
    PY_METHOD("get_stack", taskGetStack, "($self, /, *, limit=None)"),
    PY_METHOD("print_stack", taskPrintStack, "($self, /, *, limit=None, file=None)"),
    PY_METHOD("_make_cancelled_error", futureMakeCancelledError, "($self, /)"),
    PY_METHOD("get_name", taskGetName, "($self, /)"),
    PY_METHOD("set_name", taskSetName, "($self, value, /)"),
    PY_METHOD("get_coro", taskGetCoroutine, "($self, /)"),
    PY_METHOD("get_context", taskGetContext, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};
static constexpr NativeGetSet taskGetSets[] = {
    { "_log_destroy_pending"_s, taskGetLogDestroyPending, taskSetLogDestroyPending },
    { "_must_cancel"_s, taskGetMustCancel },
    { "_coro"_s, taskGetCoroutine },
    { "_fut_waiter"_s, taskGetWaiter },
};
static constexpr TypeDef taskTypeDef { "_asyncio.Task"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasDict, TypeFlag::HasWeakref }, taskMethods, taskGetSets, { } };

// ---- The functions of the module ----

static JSValue asyncioGetRunningLoopOrNone(JSGlobalObject* globalObject)
{
    PyRecord* state = moduleState(globalObject);
    useThisThread(globalObject, state);
    return orNone(state->get(StateRunningLoop));
}

static JSValue asyncioSetRunningLoop(JSGlobalObject* globalObject, JSValue loop)
{
    PyRecord* state = moduleState(globalObject);
    useThisThread(globalObject, state);
    state->set(getVM(globalObject), StateRunningLoop, isNone(loop) ? JSValue() : loop);
    return jsUndefined();
}

static JSValue asyncioGetEventLoop(JSGlobalObject* globalObject) { return getEventLoop(globalObject, moduleState(globalObject)); }

static JSValue runningLoop(JSGlobalObject* globalObject, PyRecord* state)
{
    useThisThread(globalObject, state);
    if (JSValue loop = state->get(StateRunningLoop)) [[likely]]
        return loop;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue hostLoop = loopOfHost(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (hostLoop)
        return hostLoop;
    return raiseRuntimeError(globalObject, scope, "no running event loop"_s);
}

static JSValue asyncioGetRunningLoop(JSGlobalObject* globalObject) { return runningLoop(globalObject, moduleState(globalObject)); }

static JSValue asyncioRegisterTask(JSGlobalObject* globalObject, JSValue taskValue)
{
    PyRecord* state = moduleState(globalObject);
    if (PyRecord* task = asTask(state, taskValue)) {
        registerTask(globalObject, task);
        return jsUndefined();
    }
    if (!ensureIsInitialized(globalObject, state))
        return { };
    return callMethod(globalObject, state->get(StateForeignTasks), nameIn(state, StateNameAdd), taskValue) ? jsUndefined() : JSValue();
}

static JSValue asyncioRegisterEagerTask(JSGlobalObject* globalObject, JSValue taskValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* state = moduleState(globalObject);
    if (PyRecord* task = asTask(state, taskValue)) {
        registerTask(globalObject, task);
        return jsUndefined();
    }
    PySet::from(state->get(StateForeignEagerTasks))->add(globalObject, taskValue);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue asyncioUnregisterTask(JSGlobalObject* globalObject, JSValue taskValue)
{
    PyRecord* state = moduleState(globalObject);
    if (PyRecord* task = asTask(state, taskValue)) {
        unregisterTask(globalObject, task);
        return jsUndefined();
    }
    if (!ensureIsInitialized(globalObject, state))
        return { };
    return callMethod(globalObject, state->get(StateForeignTasks), nameIn(state, StateNameDiscard), taskValue) ? jsUndefined() : JSValue();
}

static JSValue asyncioUnregisterEagerTask(JSGlobalObject* globalObject, JSValue taskValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* state = moduleState(globalObject);
    if (PyRecord* task = asTask(state, taskValue)) {
        unregisterTask(globalObject, task);
        return jsUndefined();
    }
    PySet::from(state->get(StateForeignEagerTasks))->remove(globalObject, taskValue);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue asyncioEnterTask(JSGlobalObject* globalObject, JSValue loop, JSValue task)
{
    return enterTask(globalObject, moduleState(globalObject), loop, task) ? jsUndefined() : JSValue();
}

static JSValue asyncioLeaveTask(JSGlobalObject* globalObject, JSValue loop, JSValue task)
{
    return leaveTask(globalObject, moduleState(globalObject), loop, task) ? jsUndefined() : JSValue();
}

static JSValue asyncioSwapCurrentTask(JSGlobalObject* globalObject, JSValue loop, JSValue task)
{
    return swapCurrentTask(globalObject, moduleState(globalObject), loop, task);
}

static JSValue asyncioCurrentTask(JSGlobalObject* globalObject, JSValue loop)
{
    PyRecord* state = moduleState(globalObject);
    if (!loop || isNone(loop)) {
        loop = runningLoop(globalObject, state);
        if (!loop)
            return { };
    }
    useThisThread(globalObject, state);
    if (state->get(StateRunningLoop) == loop) [[likely]]
        return orNone(state->get(StateCurrentTask));
    // It may be running in another thread.
    if (JSValue others = state->get(StateOtherThreads)) {
        JSValue key;
        JSValue value;
        for (unsigned position = 0; PyDict::from(others)->next(position, key, value);) {
            if (PyTuple::from(value)->at(0) == loop)
                return PyTuple::from(value)->at(1);
        }
    }
    return jsUndefined();
}

static JSValue asyncioAllTasks(JSGlobalObject* globalObject, JSValue loop)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* state = moduleState(globalObject);
    if (failed(scope, ensureIsInitialized(globalObject, state)))
        return { };
    if (!loop || isNone(loop)) {
        loop = runningLoop(globalObject, state);
        RETURN_IF_EXCEPTION(scope, { });
    }
    // The eager ones first, so that one that stops being eager meanwhile is not missed.
    JSArray* tasks = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    listExtend(globalObject, tasks, state->get(StateForeignEagerTasks));
    RETURN_IF_EXCEPTION(scope, { });
    listExtend(globalObject, tasks, state->get(StateForeignTasks));
    RETURN_IF_EXCEPTION(scope, { });

    PySet* result = PySet::create(globalObject);
    // Nothing runs while these are copied. A collection may come in between, which takes out what is dead.
    auto& scheduled = runtime(globalObject).scheduledTasks;
    for (unsigned i = 0; i < scheduled.size(); ++i) {
        if (JSCell* task = scheduled.at(i)) {
            listAppend(globalObject, tasks, task);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }

    for (unsigned i = 0; i < listLength(tasks); ++i) {
        JSValue task = listGet(tasks, i);
        if (typeOfObject(task) == typeIn(state, StateTaskType)) [[likely]] {
            PyRecord* record = recordOf(task);
            if (futureState(record) == StatePending && record->get(FutureLoop) == loop) {
                result->add(globalObject, task);
                RETURN_IF_EXCEPTION(scope, { });
            }
            continue;
        }
        JSValue done = callMethod(globalObject, task, nameIn(state, StateNameDone));
        RETURN_IF_EXCEPTION(scope, { });
        if (done.isTrue())
            continue;
        JSValue taskLoop = loopOfFuture(globalObject, state, task);
        RETURN_IF_EXCEPTION(scope, { });
        if (taskLoop == loop) {
            result->add(globalObject, task);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return result;
}

static JSValue asyncioFutureAddToAwaitedBy(JSGlobalObject* globalObject, JSValue futureValue, JSValue waiter)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* state = moduleState(globalObject);
    PyRecord* future = asFuture(state, futureValue);
    if (future && asFuture(state, waiter) && failed(scope, addToAwaitedBy(globalObject, future, waiter)))
        return { };
    return jsUndefined();
}

static JSValue asyncioFutureDiscardFromAwaitedBy(JSGlobalObject* globalObject, JSValue futureValue, JSValue waiter)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* state = moduleState(globalObject);
    PyRecord* future = asFuture(state, futureValue);
    if (future && asFuture(state, waiter) && failed(scope, discardFromAwaitedBy(globalObject, future, waiter)))
        return { };
    return jsUndefined();
}

static constexpr NativeDef asyncioFunctions[] = {
    PY_FUNCTION("current_task", asyncioCurrentTask, "(loop=None)"),
    PY_FUNCTION("get_event_loop", asyncioGetEventLoop, "()"),
    PY_FUNCTION("get_running_loop", asyncioGetRunningLoop, "()"),
    PY_FUNCTION("_get_running_loop", asyncioGetRunningLoopOrNone, "()"),
    PY_FUNCTION("_set_running_loop", asyncioSetRunningLoop, "(loop, /)"),
    PY_FUNCTION("_register_task", asyncioRegisterTask, "(task)"),
    PY_FUNCTION("_register_eager_task", asyncioRegisterEagerTask, "(task)"),
    PY_FUNCTION("_unregister_task", asyncioUnregisterTask, "(task)"),
    PY_FUNCTION("_unregister_eager_task", asyncioUnregisterEagerTask, "(task)"),
    PY_FUNCTION("_enter_task", asyncioEnterTask, "(loop, task)"),
    PY_FUNCTION("_leave_task", asyncioLeaveTask, "(loop, task)"),
    PY_FUNCTION("_swap_current_task", asyncioSwapCurrentTask, "(loop, task)"),
    PY_FUNCTION("all_tasks", asyncioAllTasks, "(loop=None)"),
    PY_FUNCTION("future_add_to_awaited_by", asyncioFutureAddToAwaitedBy, "(fut, waiter, /)"),
    PY_FUNCTION("future_discard_from_awaited_by", asyncioFutureDiscardFromAwaitedBy, "(fut, waiter, /)"),
};

void initializeAsyncioModule(JSGlobalObject*, PyModule*);
void initializeAsyncioModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyType* futureType = rt.typeFor(futureTypeDef);
    PyType* taskType = rt.typeFor(taskTypeDef, futureType);
    JSValue existing = hiddenField(globalObject, futureType, 0);
    PyRecord* state = existing ? recordOf(existing) : PyRecord::create(globalObject, rt.typeFor(asyncioStateTypeDef), StateFieldCount);
    if (!existing) {
        state->number = rt.threads().current().serial;
        state->set(vm, StateFutureType, futureType);
        state->set(vm, StateTaskType, taskType);
        state->set(vm, StateFutureIterType, rt.typeFor(futureIterTypeDef));
        state->set(vm, StateStepWrapperType, rt.typeFor(stepWrapperTypeDef));
        RETURN_IF_EXCEPTION(scope, void());
        state->set(vm, StateWakeupMethod, PyNativeFunction::create(globalObject, taskWakeupDef, NativeKind::Method, { }, taskType));
        PyTuple* keyword = PyTuple::createUninitialized(vm, rt.keywordNamesStructure(), rt.tupleType(), 1);
        keyword->initializeAt(vm, 0, internString(vm, "context"_s));
        state->set(vm, StateContextKeyword, keyword);
        state->set(vm, StateForeignEagerTasks, PySet::create(globalObject));
        state->set(vm, StateCoroutineTypes, PySet::create(globalObject));
        state->set(vm, StateTaskNameCounter, jsNumber(0));
#define ASYNCIO_INTERN_NAME(id, text) state->set(vm, StateName##id, internString(vm, text ""_s));
        FOR_EACH_ASYNCIO_NAME(ASYNCIO_INTERN_NAME)
#undef ASYNCIO_INTERN_NAME
        setHiddenField(globalObject, futureType, 0, state);
        setHiddenField(globalObject, taskType, 0, state);
    }

    module->add(globalObject, "Future"_s, futureType);
    module->add(globalObject, "Task"_s, taskType);
    module->addFunctions(globalObject, asyncioFunctions);

    // After the types are there: asyncio, which is being imported, imports this.
    importModule(globalObject, "asyncio"_s);
    RETURN_IF_EXCEPTION(scope, void());
    struct Import {
        ASCIILiteral module;
        ASCIILiteral name;
        StateField field;
    };
    static constexpr Import imports[] = {
        { "asyncio.events"_s, "_get_event_loop_policy"_s, StateGetEventLoopPolicy },
        { "asyncio.base_futures"_s, "_future_repr"_s, StateFutureRepr },
        { "asyncio.exceptions"_s, "InvalidStateError"_s, StateInvalidStateError },
        { "asyncio.exceptions"_s, "CancelledError"_s, StateCancelledError },
        { "asyncio.base_tasks"_s, "_task_repr"_s, StateTaskRepr },
        { "asyncio.base_tasks"_s, "_task_get_stack"_s, StateTaskGetStack },
        { "asyncio.base_tasks"_s, "_task_print_stack"_s, StateTaskPrintStack },
        { "asyncio.coroutines"_s, "iscoroutine"_s, StateIsCoroutine },
        { "traceback"_s, "extract_stack"_s, StateExtractStack },
        { "weakref"_s, "WeakSet"_s, StateForeignTasks },
    };
    for (auto& import : imports) {
        JSValue from = importModule(globalObject, import.module);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue value = getAttr(globalObject, from, internString(vm, import.name));
        RETURN_IF_EXCEPTION(scope, void());
        if (import.field == StateForeignTasks) {
            value = call(globalObject, value);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (import.field == StateInvalidStateError || import.field == StateCancelledError) {
            if (auto* type = PyType::tryFrom(value); !type || !type->has(TypeFlag::ExceptionSubclass)) {
                raiseTypeError(globalObject, scope, import.module, '.', import.name, " must be an exception class, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)));
                return;
            }
        }
        state->set(vm, import.field, value);
    }
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
