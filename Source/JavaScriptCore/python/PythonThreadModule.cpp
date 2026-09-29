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

#include "PyLock.h"
#include "PythonCodecs.h"
#include "PythonTime.h"
#include <signal.h>

#if USE(PTHREADS)
#include <pthread.h>
#endif
#if OS(LINUX)
#include <sys/syscall.h>
#include <unistd.h>
#endif

// _thread: Modules/_threadmodule.c of CPython.
//
// There is one thread. Everything here is as it is in CPython for a program that has not started another, and starting one fails as it does there when the system will not: RuntimeError, "can't start new thread".
// FIXME: Threads.

namespace JSC { namespace Python {

static PyLock* asLock(JSValue value) { return uncheckedDowncast<PyLock>(value.asCell()); }
static ThreadModuleState& threadStateOf(JSGlobalObject* globalObject) { return globalObject->pyRealm()->threadModule(); }

// PyThread_get_thread_ident_ex()
static uint64_t currentThreadIdentifier()
{
#if USE(PTHREADS)
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#else
    return Thread::currentSingleton().uid();
#endif
}

// ---- Waiting

static constexpr int64_t noTimeout = -nanosecondsPerSecond; // _PyTime_FromSeconds(-1)
static constexpr int64_t longestTimeoutInMicroseconds = std::numeric_limits<int64_t>::max() / 1000; // PY_TIMEOUT_MAX

static Seconds secondsToWait(int64_t timeout)
{
    return timeout < 0 ? Seconds::infinity() : Seconds::fromNanoseconds(static_cast<double>(timeout));
}

// lock_acquire_parse_args(): how long acquire(blocking=True, timeout=-1) is to wait, in nanoseconds. Less than nothing is for ever.
static std::optional<int64_t> parseAcquireArguments(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args)
{
    bool blocking = true;
    if (JSValue value = args.at(1)) {
        blocking = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
    }
    int64_t timeout = noTimeout;
    if (JSValue value = args.at(2)) {
        auto given = timeFromSecondsObject(globalObject, value, TimeRounding::Timeout);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        timeout = *given;
    }
    if (!blocking && timeout != noTimeout) {
        raiseValueError(globalObject, scope, "can't specify a timeout for a non-blocking call"_s);
        return std::nullopt;
    }
    if (timeout < 0 && timeout != noTimeout) {
        raiseValueError(globalObject, scope, "timeout value must be a non-negative number"_s);
        return std::nullopt;
    }
    if (!blocking)
        return 0;
    if (timeout != noTimeout && divideTime(timeout, nanosecondsPerMicrosecond, TimeRounding::Timeout) > longestTimeoutInMicroseconds) {
        raise(globalObject, scope, BuiltinType::OverflowError, "timeout value is too large"_s);
        return std::nullopt;
    }
    return timeout;
}

// ---- lock

PYTHON_NATIVE(lockNew)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "lock"_s))
        return { };
    if (args.size() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("lock expected 0 arguments, got "_s, args.size() - 1)));
    return JSValue::encode(PyLock::create(vm, threadStateOf(globalObject).lockType->instanceStructure()));
}

PYTHON_NATIVE(lockAcquire)
{
    NATIVE_PROLOGUE();
    auto timeout = parseAcquireArguments(globalObject, scope, args);
    if (!timeout)
        return { };
    auto result = asLock(args[0])->lock(vm, secondsToWait(*timeout));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(result == PyLock::Result::Acquired));
}

PYTHON_NATIVE(lockRelease)
{
    NATIVE_PROLOGUE();
    if (!asLock(args[0])->unlock())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "release unlocked lock"_s));
    RETURN_NONE();
}

PYTHON_NATIVE(lockIsLocked)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(jsBoolean(asLock(args[0])->isLocked()));
}

PYTHON_NATIVE(lockReset)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    asLock(args[0])->reset();
    RETURN_NONE();
}

PYTHON_NATIVE(lockRepr)
{
    NATIVE_PROLOGUE();
    PyLock* self = asLock(args[0]);
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', self->isLocked() ? "locked "_s : "unlocked "_s, typeName(globalObject, self), " object at "_s, addressOf(self), '>'))));
}

// ---- RLock

static bool isLockedByCurrentThread(PyLock* lock)
{
    return lock->isLocked() && lock->owner() == currentThreadIdentifier();
}

PYTHON_NATIVE(recursiveLockNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyLock::create(vm, asType(args[0])->instanceStructure()));
}

// _PyRecursiveMutex_LockTimed()
static PyLock::Result lockRecursively(VM& vm, PyLock* lock, Seconds timeout)
{
    if (isLockedByCurrentThread(lock)) {
        lock->setLevel(lock->level() + 1);
        return PyLock::Result::Acquired;
    }
    auto result = lock->lock(vm, timeout);
    if (result == PyLock::Result::Acquired)
        lock->setOwner(currentThreadIdentifier());
    return result;
}

PYTHON_NATIVE(recursiveLockAcquire)
{
    NATIVE_PROLOGUE();
    auto timeout = parseAcquireArguments(globalObject, scope, args);
    if (!timeout)
        return { };
    auto result = lockRecursively(vm, asLock(args[0]), secondsToWait(*timeout));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(result == PyLock::Result::Acquired));
}

PYTHON_NATIVE(recursiveLockRelease)
{
    NATIVE_PROLOGUE();
    PyLock* self = asLock(args[0]);
    // _PyRecursiveMutex_TryUnlock()
    if (!isLockedByCurrentThread(self))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot release un-acquired lock"_s));
    if (self->level())
        self->setLevel(self->level() - 1);
    else {
        self->setOwner(0);
        self->unlock();
    }
    RETURN_NONE();
}

PYTHON_NATIVE(recursiveLockIsOwned)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(jsBoolean(isLockedByCurrentThread(asLock(args[0]))));
}

PYTHON_NATIVE(recursiveLockCount)
{
    NativeArguments args(callFrame);
    PyLock* self = asLock(args[0]);
    return JSValue::encode(isLockedByCurrentThread(self) ? intFromUInt64(globalObject, self->level() + 1) : jsNumber(0));
}

PYTHON_NATIVE(recursiveLockReleaseSave)
{
    NATIVE_PROLOGUE();
    PyLock* self = asLock(args[0]);
    if (!isLockedByCurrentThread(self))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot release un-acquired lock"_s));
    JSValue count = intFromUInt64(globalObject, self->level() + 1);
    JSValue owner = intFromUInt64(globalObject, self->owner());
    self->setLevel(0);
    self->setOwner(0);
    self->unlock();
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { count, owner })));
}

// PyArg_ParseTuple(args, "(nK):_acquire_restore", &count, &owner)
PYTHON_NATIVE(recursiveLockAcquireRestore)
{
    NATIVE_PROLOGUE();
    PyLock* self = asLock(args[0]);
    if (!args.checkNoKeywords(globalObject, scope, "RLock._acquire_restore"_s))
        return { };
    if (args.size() != 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_acquire_restore() takes exactly 1 argument ("_s, args.size() - 1, " given)"_s)));
    JSValue state = args[1];
    if (!isInstance(globalObject, state, realm->typeTuple()) && !isInstance(globalObject, state, realm->typeList()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_acquire_restore() argument 1 must be 2-item tuple, not "_s, isNone(state) ? "None"_s : typeName(globalObject, state))));
    MarkedArgumentBuffer items;
    collect(globalObject, state, items);
    RETURN_IF_EXCEPTION(scope, { });
    if (items.size() != 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_acquire_restore() argument 1 must be tuple of length 2, not "_s, items.size())));
    auto count = toSsize(globalObject, items.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    // "K" takes an int and nothing else, and of that what it has in its low 64 bits, whatever else it has.
    if (!isInstance(globalObject, items.at(1), realm->typeInt()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_acquire_restore() argument 1, item 1 must be int, not "_s, typeNameOfArgument(globalObject, items.at(1)))));
    JSValue ownerValue = toInt(globalObject, items.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    uint64_t owner = lowBitsOfInt(ownerValue);
    lockRecursively(vm, self, Seconds::infinity());
    RETURN_IF_EXCEPTION(scope, { });
    self->setOwner(owner);
    self->setLevel(static_cast<size_t>(*count) - 1);
    RETURN_NONE();
}

PYTHON_NATIVE(recursiveLockRepr)
{
    NATIVE_PROLOGUE();
    PyLock* self = asLock(args[0]);
    bool isLocked = self->isLocked();
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', isLocked ? "locked "_s : "unlocked "_s, typeName(globalObject, self), " object owner="_s, self->owner(), " count="_s, isLocked ? self->level() + 1 : 0, " at "_s, addressOf(self), '>'))));
}

// ---- _local
//
// What a thread has put in one is there for that thread alone. In CPython it has a dict for each thread, and its attributes are got from and set in the one for the thread that is asking. With one thread there is one,
// and it is where the attributes of any instance are: in the instance.

PYTHON_NATIVE(localNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    if (type->lookup(vm, names.dunder_init).asCell() == realm->function(PyRealm::WellKnownFunction::ObjectInit) && (args.size() > 1 || args.keywordCount()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "Initialization arguments are not supported"_s));
    return JSValue::encode(PyInstance::create(vm, type->instanceStructure()));
}

PYTHON_NATIVE(localGetAttribute)
{
    NATIVE_PROLOGUE();
    JSObject* self = asObject(args[0]);
    auto name = attributeName(globalObject, scope, args[1]);
    if (!name)
        return { };
    if (*name == names.dunder_dict)
        RELEASE_AND_RETURN(scope, JSValue::encode(PyDict::backedBy(globalObject, self)));
    // _PyObject_GenericGetAttrWithDict(), with what it has for the dict, whether or not instances of the class have a __dict__ besides. If the class is this one and not one derived from it, what it has comes before
    // anything that the class has to say.
    PyType* type = typeOf(globalObject, self);
    JSValue found = type == threadStateOf(globalObject).localType.get() ? JSValue() : type->lookup(vm, *name);
    if (!found || !isDataDescriptor(globalObject, found)) {
        if (JSValue value = getStoredAttribute(vm, self, *name))
            return JSValue::encode(value);
    }
    JSValue value = genericGetAttribute(globalObject, self, *name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', typeName(globalObject, self), "' object has no attribute '"_s, name->string(), '\'')));
}

PYTHON_NATIVE(localSetAttribute)
{
    NATIVE_PROLOGUE();
    JSObject* self = asObject(args[0]);
    auto name = attributeName(globalObject, scope, args[1]);
    if (!name)
        return { };
    JSValue value = args.size() > 2 ? args[2] : JSValue();
    if (*name == names.dunder_dict) {
        String shown = repr(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', typeName(globalObject, self), "' object attribute "_s, shown, " is read-only"_s)));
    }
    // _PyObject_GenericSetAttrWithDict(), likewise.
    JSValue found = typeOf(globalObject, self)->lookup(vm, *name);
    if (found && isDataDescriptor(globalObject, found)) {
        scope.release();
        genericSetAttribute(globalObject, self, *name, value);
        RETURN_NONE();
    }
    if (value) {
        putStoredAttribute(vm, self, *name, value);
        RETURN_NONE();
    }
    if (!getStoredAttribute(vm, self, *name))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', typeName(globalObject, self), "' object has no attribute '"_s, name->string(), '\'')));
    scope.release();
    deleteStoredAttribute(globalObject, self, *name);
    RETURN_NONE();
}

// ---- _ThreadHandle

enum HandleField : unsigned {
    HandleIdentifier,
    HandleState,
    HandleIsRunning, // A lock, which is held until the thread is done: `thread_is_exiting`.
};

enum class HandleStatus : uint8_t { NotStarted, Starting, Running, Done };

static HandleStatus statusOf(PyNativeObject* handle) { return static_cast<HandleStatus>(handle->field(HandleState).asInt32()); }
static void setStatus(VM& vm, PyNativeObject* handle, HandleStatus status) { handle->setField(vm, HandleState, jsNumber(static_cast<int32_t>(status))); }
static PyLock* runningLockOf(PyNativeObject* handle) { return asLock(handle->field(HandleIsRunning)); }

static PyNativeObject* newHandle(JSGlobalObject* globalObject, JSValue identifier, HandleStatus status)
{
    VM& vm = globalObject->vm();
    auto& state = threadStateOf(globalObject);
    PyLock* isRunning = PyLock::create(vm, state.lockType->instanceStructure());
    isRunning->tryLock();
    auto* handle = PyNativeObject::create(vm, state.handleType->instanceStructure());
    handle->setField(vm, HandleIdentifier, identifier);
    handle->setField(vm, HandleIsRunning, isRunning);
    setStatus(vm, handle, status);
    return handle;
}

// force_done() and set_done()
static void setDone(VM& vm, PyNativeObject* handle)
{
    setStatus(vm, handle, HandleStatus::Done);
    runningLockOf(handle)->unlock();
}

PYTHON_NATIVE(handleNew)
{
    return JSValue::encode(newHandle(globalObject, jsNumber(0), HandleStatus::NotStarted));
}

PYTHON_NATIVE(handleRepr)
{
    NATIVE_PROLOGUE();
    PyNativeObject* self = asNativeObject(args[0]);
    String identifier = repr(globalObject, self->field(HandleIdentifier));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', typeName(globalObject, self), " object: ident="_s, identifier, '>'))));
}

static JSValue getHandleIdentifier(JSGlobalObject*, JSValue self)
{
    return asNativeObject(self)->field(HandleIdentifier);
}

// check_started()
static bool checkIsStarted(JSGlobalObject* globalObject, ThrowScope& scope, PyNativeObject* handle)
{
    if (statusOf(handle) >= HandleStatus::Running)
        return true;
    raise(globalObject, scope, BuiltinType::RuntimeError, "thread not started"_s);
    return false;
}

// ThreadHandle_join()
static void joinHandle(JSGlobalObject* globalObject, PyNativeObject* handle, int64_t timeout)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!checkIsStarted(globalObject, scope, handle))
        return;
    PyLock* isRunning = runningLockOf(handle);
    if (isRunning->isLocked()) {
        auto identifier = tryInt64(handle->field(HandleIdentifier));
        if (identifier && static_cast<uint64_t>(*identifier) == currentThreadIdentifier()) {
            raise(globalObject, scope, BuiltinType::RuntimeError, "Cannot join current thread"_s);
            return;
        }
    }
    auto result = isRunning->waitUntilUnlocked(vm, secondsToWait(timeout));
    RETURN_IF_EXCEPTION(scope, void());
    if (result == PyLock::Result::Acquired)
        setStatus(vm, handle, HandleStatus::Done);
}

PYTHON_NATIVE(handleJoin)
{
    NATIVE_PROLOGUE();
    // PyArg_ParseTuple(args, "|O:join", &timeout_obj)
    if (!args.checkNoKeywords(globalObject, scope, "_ThreadHandle.join"_s))
        return { };
    if (args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("join() takes at most 1 argument ("_s, args.size() - 1, " given)"_s)));
    int64_t timeout = -1;
    if (args.size() > 1 && !isNone(args[1])) {
        auto given = timeFromSecondsObject(globalObject, args[1], TimeRounding::Timeout);
        RETURN_IF_EXCEPTION(scope, { });
        timeout = *given;
    }
    scope.release();
    joinHandle(globalObject, asNativeObject(args[0]), timeout);
    RETURN_NONE();
}

PYTHON_NATIVE(handleIsDone)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(jsBoolean(!runningLockOf(asNativeObject(args[0]))->isLocked()));
}

PYTHON_NATIVE(handleSetDone)
{
    NATIVE_PROLOGUE();
    PyNativeObject* self = asNativeObject(args[0]);
    if (!checkIsStarted(globalObject, scope, self))
        return { };
    setDone(vm, self);
    RETURN_NONE();
}

// ---- The module

// ThreadHandle_start()
static void startThread(JSGlobalObject* globalObject, PyNativeObject* handle)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (statusOf(handle) != HandleStatus::NotStarted) {
        raise(globalObject, scope, BuiltinType::RuntimeError, "thread already started"_s);
        return;
    }
    setDone(vm, handle);
    raise(globalObject, scope, BuiltinType::RuntimeError, "can't start new thread"_s);
}

PYTHON_NATIVE(threadStartNew)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "start_new_thread"_s))
        return { };
    if (args.size() < 2 || args.size() > 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("start_new_thread expected at "_s, args.size() < 2 ? "least 2"_s : "most 3"_s, " arguments, got "_s, args.size())));
    if (!isCallable(globalObject, args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "first arg must be callable"_s));
    if (!isInstance(globalObject, args[1], realm->typeTuple()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "2nd arg must be a tuple"_s));
    if (args.size() > 2 && !isInstance(globalObject, args[2], realm->typeDict()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "optional 3rd arg must be a dictionary"_s));
    if (!audit(globalObject, "_thread.start_new_thread"_s, args[0], args[1], args.size() > 2 ? args[2] : jsUndefined()))
        return { };
    scope.release();
    startThread(globalObject, newHandle(globalObject, jsNumber(0), HandleStatus::NotStarted));
    return { };
}

PYTHON_NATIVE(threadStartJoinable)
{
    NATIVE_PROLOGUE();
    JSValue function = args.at(0);
    JSValue handle = args.at(1);
    bool isDaemon = true;
    if (JSValue value = args.at(2)) {
        isDaemon = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!isCallable(globalObject, function))
        return JSValue::encode(raiseTypeError(globalObject, scope, "thread function must be callable"_s));
    if (!handle)
        handle = jsUndefined();
    else if (!isNone(handle) && typeOf(globalObject, handle) != threadStateOf(globalObject).handleType.get())
        return JSValue::encode(raiseTypeError(globalObject, scope, "'handle' must be a _ThreadHandle"_s));
    if (!audit(globalObject, "_thread.start_joinable_thread"_s, function, jsNumber(isDaemon), handle))
        return { };
    if (isNone(handle))
        handle = newHandle(globalObject, jsNumber(0), HandleStatus::NotStarted);
    scope.release();
    startThread(globalObject, asNativeObject(handle));
    return { };
}

PYTHON_NATIVE(threadExit)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemExit, JSValue()));
}

PYTHON_NATIVE(threadInterruptMain)
{
    NATIVE_PROLOGUE();
    int signal = SIGINT;
    if (JSValue value = args.at(0)) {
        // The "i" of PyArg_ParseTuple()
        auto given = toCLong(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (*given > std::numeric_limits<int>::max() || *given < std::numeric_limits<int>::min())
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, *given > 0 ? "signed integer is greater than maximum"_s : "signed integer is less than minimum"_s));
        signal = static_cast<int>(*given);
    }
    // PyErr_SetInterruptEx()
    if (signal < 1 || signal >= NSIG)
        return JSValue::encode(raiseValueError(globalObject, scope, "signal number out of range"_s));
    // FIXME: What a program has set with signal.signal(), when there is that. Until then only this one has anything set for it, which raises KeyboardInterrupt.
    if (signal != SIGINT)
        RETURN_NONE();
    vm.interruptPython();
    // CPython looks whether there is anything of the kind to be done when a call returns, and so when this one does.
    scope.release();
    doPendingWork(globalObject);
    RETURN_NONE();
}

PYTHON_NATIVE(threadAllocateLock)
{
    return JSValue::encode(PyLock::create(globalObject->vm(), threadStateOf(globalObject).lockType->instanceStructure()));
}

PYTHON_NATIVE(threadGetIdentifier)
{
    return JSValue::encode(intFromUInt64(globalObject, currentThreadIdentifier()));
}

PYTHON_NATIVE(threadGetMainIdentifier)
{
    return JSValue::encode(intFromUInt64(globalObject, threadStateOf(globalObject).mainThread));
}

#if OS(DARWIN) || OS(LINUX)
// PyThread_get_thread_native_id()
PYTHON_NATIVE(threadGetNativeIdentifier)
{
#if OS(DARWIN)
    uint64_t identifier;
    pthread_threadid_np(nullptr, &identifier);
#else
    uint64_t identifier = static_cast<uint64_t>(syscall(SYS_gettid));
#endif
    return JSValue::encode(intFromUInt64(globalObject, identifier));
}
#endif

PYTHON_NATIVE(threadCount)
{
    return JSValue::encode(jsNumber(0));
}

PYTHON_NATIVE(threadTrue)
{
    return JSValue::encode(jsBoolean(true));
}

PYTHON_NATIVE(threadStackSize)
{
    NATIVE_PROLOGUE();
    int64_t newSize = 0;
    if (JSValue value = args.at(0)) {
        auto given = toSsize(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        newSize = *given;
    }
    // _PyOS_MIN_STACK_SIZE and SYSTEM_PAGE_SIZE
    constexpr int64_t smallest = (1 << 11) * sizeof(void*) * 3 + 4 * KB;
    if (newSize && newSize < smallest)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("size must be at least "_s, smallest, " bytes"_s)));
    auto& state = threadStateOf(globalObject);
    size_t oldSize = state.stackSize;
    if (newSize) {
#if USE(PTHREADS)
        // Whether it will do is for the system to say.
        pthread_attr_t attributes;
        bool isValid = false;
        if (!pthread_attr_init(&attributes)) {
            isValid = !pthread_attr_setstacksize(&attributes, static_cast<size_t>(newSize));
            pthread_attr_destroy(&attributes);
        }
        if (!isValid)
            return JSValue::encode(raiseValueError(globalObject, scope, concatenate("size not valid: "_s, newSize, " bytes"_s)));
#endif
    }
    state.stackSize = static_cast<size_t>(newSize);
    return JSValue::encode(intFromUInt64(globalObject, oldSize));
}

static void writeToFile(JSGlobalObject* globalObject, JSValue file, const String& text)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue write = getAttribute(globalObject, file, Identifier::fromString(vm, "write"_s));
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    call(globalObject, write, jsString(vm, text));
}

PYTHON_NATIVE(threadExceptHook)
{
    NATIVE_PROLOGUE();
    if (typeOf(globalObject, args[0]) != threadStateOf(globalObject).exceptHookArgsType.get())
        return JSValue::encode(raiseTypeError(globalObject, scope, "_thread.excepthook argument type must be ExceptHookArgs"_s));
    PyTuple* hookArguments = asTuple(args[0]);
    JSValue type = hookArguments->at(0);
    if (type == JSValue(realm->type(BuiltinType::SystemExit)))
        RETURN_NONE();
    JSValue value = hookArguments->at(1);
    JSValue thread = hookArguments->at(3);

    JSValue file = sysAttribute(globalObject, "stderr"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!file || isNone(file)) {
        if (isNone(thread))
            RETURN_NONE();
        file = getAttribute(globalObject, thread, Identifier::fromString(vm, "_stderr"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (isNone(file))
            RETURN_NONE();
    }

    // thread_excepthook_file()
    writeToFile(globalObject, file, "Exception in thread "_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue name;
    if (!isNone(thread)) {
        name = getAttributeIfPresent(globalObject, thread, Identifier::fromString(vm, "name"_s));
        RETURN_IF_EXCEPTION(scope, { });
    }
    String shown = name ? str(globalObject, name) : String::number(currentThreadIdentifier());
    RETURN_IF_EXCEPTION(scope, { });
    writeToFile(globalObject, file, shown);
    RETURN_IF_EXCEPTION(scope, { });
    writeToFile(globalObject, file, ":\n"_s);
    RETURN_IF_EXCEPTION(scope, { });
    String text = formatException(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    writeToFile(globalObject, file, text);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue flush = getAttribute(globalObject, file, Identifier::fromString(vm, "flush"_s));
    RETURN_IF_EXCEPTION(scope, { });
    call(globalObject, flush);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(threadShutdown)
{
    // There are no others to wait for.
    RETURN_NONE();
}

PYTHON_NATIVE(threadMakeHandle)
{
    NATIVE_PROLOGUE();
    if (!isInstance(globalObject, args[0], realm->typeInt()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "ident must be an integer"_s));
    // PyLong_AsUnsignedLongLong()
    JSValue identifier = toInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (compareInts(identifier, jsNumber(0)) < 0)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "can't convert negative int to unsigned"_s));
    if (identifier.isBigInt() && JSBigInt::compare(identifier, JSBigInt::createFrom(globalObject, std::numeric_limits<uint64_t>::max())) == JSBigInt::ComparisonResult::GreaterThan)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "int too big to convert"_s));
    return JSValue::encode(newHandle(globalObject, identifier, HandleStatus::Running));
}

#if OS(DARWIN) || OS(LINUX)
#if OS(DARWIN)
static constexpr unsigned longestThreadName = 63;
#else
static constexpr unsigned longestThreadName = 15;
#endif

PYTHON_NATIVE(threadSetName)
{
    NATIVE_PROLOGUE();
    JSValue name = args.at(0);
    if (!isInstance(globalObject, name, realm->typeStr()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("set_name() argument 'name' must be str, not "_s, isNone(name) ? "None"_s : typeName(globalObject, name))));
    auto encoded = encodeString(globalObject, name, "utf-8"_s, "replace"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (encoded->hasOverflowed())
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    std::array<char, longestThreadName + 1> buffer { };
    memcpySpan(std::span { buffer }, byteCast<char>(encoded->span().first(std::min<size_t>(encoded->size(), longestThreadName))));
    const char* characters = buffer.data();
#if OS(DARWIN)
    int result = pthread_setname_np(characters);
#else
    int result = pthread_setname_np(pthread_self(), characters);
#endif
    if (result)
        return JSValue::encode(raiseOSError(globalObject, scope, result));
    RETURN_NONE();
}

PYTHON_NATIVE(threadGetName)
{
    NATIVE_PROLOGUE();
    std::array<char, 100> name;
    if (int result = pthread_getname_np(pthread_self(), name.data(), name.size()))
        return JSValue::encode(raiseOSError(globalObject, scope, result));
    JSValue bytes = newBytes(globalObject, byteCast<uint8_t>(unsafeSpan(name.data())));
    RETURN_IF_EXCEPTION(scope, { });
    String decoded = decodeBytes(globalObject, bytes, uncheckedDowncast<JSUint8Array>(bytes.asCell())->span(), "utf-8"_s, "surrogateescape"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, decoded)));
}
#endif

static void initializeThreadTypes(JSGlobalObject* globalObject, ThreadModuleState& state)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    constexpr auto notChecked = PyNativeFunction::Arguments::AreNotChecked;
    auto make = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name, PyType* base, PyType::Layout layout, unsigned flags) {
        PyType* type = createBuiltinType(globalObject, name, base, layout, flags);
        slot.set(vm, realm, type);
        return type;
    };

    PyType* lock = make(state.lockType, "_thread.lock"_s, realm->typeObject(), PyType::Layout::Native, 0);
    lock->setInstanceStructure(vm, PyLock::createStructure(vm, globalObject, lock));
    addMethods(globalObject, lock, {
        { "__new__"_s, lockNew, Kind::New, 0, { }, notChecked },
        { "__repr__"_s, lockRepr },
        { "acquire"_s, lockAcquire },
        { "acquire_lock"_s, lockAcquire },
        { "__enter__"_s, lockAcquire },
        { "release"_s, lockRelease },
        { "release_lock"_s, lockRelease },
        { "__exit__"_s, lockRelease },
        { "locked"_s, lockIsLocked },
        { "locked_lock"_s, lockIsLocked },
        { "_at_fork_reinit"_s, lockReset },
    });

    PyType* recursiveLock = make(state.recursiveLockType, "_thread.RLock"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
    recursiveLock->setInstanceStructure(vm, PyLock::createStructure(vm, globalObject, recursiveLock));
    addMethods(globalObject, recursiveLock, {
        { "__new__"_s, recursiveLockNew, Kind::New, 0, { }, notChecked },
        { "__repr__"_s, recursiveLockRepr },
        { "acquire"_s, recursiveLockAcquire },
        { "__enter__"_s, recursiveLockAcquire },
        { "release"_s, recursiveLockRelease },
        { "__exit__"_s, recursiveLockRelease },
        { "locked"_s, lockIsLocked },
        { "_is_owned"_s, recursiveLockIsOwned },
        { "_recursion_count"_s, recursiveLockCount },
        { "_release_save"_s, recursiveLockReleaseSave },
        { "_acquire_restore"_s, recursiveLockAcquireRestore, Kind::Method, 0, { }, notChecked },
        { "_at_fork_reinit"_s, lockReset },
    });

    PyType* local = make(state.localType, "_thread._local"_s, realm->typeObject(), PyType::Layout::Object, PyType::IsBaseType);
    addMethods(globalObject, local, {
        { "__new__"_s, localNew, Kind::New, 0, { }, notChecked },
        { "__getattribute__"_s, localGetAttribute },
        { "__setattr__"_s, localSetAttribute },
        { "__delattr__"_s, localSetAttribute },
    });

    PyType* handle = make(state.handleType, "_thread._ThreadHandle"_s, realm->typeObject(), PyType::Layout::Native, 0);
    handle->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, handle));
    addMethods(globalObject, handle, {
        { "__new__"_s, handleNew, Kind::New, 0, { }, notChecked },
        { "__repr__"_s, handleRepr },
        { "join"_s, handleJoin, Kind::Method, 0, "($self, /, *args, **kwargs)"_s, notChecked },
        { "is_done"_s, handleIsDone },
        { "_set_done"_s, handleSetDone },
    });
    addGetSet(globalObject, handle, "ident"_s, getHandleIdentifier);

    PyType* exceptHookArgs = make(state.exceptHookArgsType, "_thread._ExceptHookArgs"_s, realm->typeTuple(), PyType::Layout::Tuple, PyType::MatchesSelf | PyType::IsSequence);
    static constexpr std::array fields { "exc_type"_s, "exc_value"_s, "exc_traceback"_s, "thread"_s };
    makeStructSequenceType(globalObject, exceptHookArgs, fields, fields.size());
}

JSObject* createThreadModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = threadStateOf(globalObject);
    if (!state.lockType) {
        initializeThreadTypes(globalObject, state);
        state.mainThread = currentThreadIdentifier();
    }
    JSObject* module = newBuiltinModule(globalObject, "_thread"_s);
    auto put = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    put("_ThreadHandle"_s, state.handleType.get());
    put("lock"_s, state.lockType.get());
    put("LockType"_s, state.lockType.get());
    put("RLock"_s, state.recursiveLockType.get());
    put("_local"_s, state.localType.get());
    put("error"_s, realm->type(BuiltinType::RuntimeError));
    put("_ExceptHookArgs"_s, state.exceptHookArgsType.get());
    put("TIMEOUT_MAX"_s, floatFromDouble(std::floor(std::min(static_cast<double>(longestTimeoutInMicroseconds) * 1e-6, timeAsSeconds(std::numeric_limits<int64_t>::max())))));
    constexpr auto notChecked = PyNativeFunction::Arguments::AreNotChecked;
    addFunction(globalObject, module, "start_new_thread"_s, threadStartNew, 0, { }, notChecked);
    addFunction(globalObject, module, "start_new"_s, threadStartNew, 0, { }, notChecked);
    addFunction(globalObject, module, "start_joinable_thread"_s, threadStartJoinable);
    addFunction(globalObject, module, "daemon_threads_allowed"_s, threadTrue);
    addFunction(globalObject, module, "allocate_lock"_s, threadAllocateLock);
    addFunction(globalObject, module, "allocate"_s, threadAllocateLock);
    addFunction(globalObject, module, "exit_thread"_s, threadExit);
    addFunction(globalObject, module, "exit"_s, threadExit);
    addFunction(globalObject, module, "interrupt_main"_s, threadInterruptMain);
    addFunction(globalObject, module, "get_ident"_s, threadGetIdentifier);
    addFunction(globalObject, module, "_count"_s, threadCount);
    addFunction(globalObject, module, "stack_size"_s, threadStackSize);
    addFunction(globalObject, module, "_excepthook"_s, threadExceptHook);
    addFunction(globalObject, module, "_is_main_interpreter"_s, threadTrue);
    addFunction(globalObject, module, "_shutdown"_s, threadShutdown);
    addFunction(globalObject, module, "_make_thread_handle"_s, threadMakeHandle);
    addFunction(globalObject, module, "_get_main_thread_ident"_s, threadGetMainIdentifier);
#if OS(DARWIN) || OS(LINUX)
    addFunction(globalObject, module, "get_native_id"_s, threadGetNativeIdentifier);
    addFunction(globalObject, module, "set_name"_s, threadSetName);
    addFunction(globalObject, module, "_get_name"_s, threadGetName);
    put("_NAME_MAXLEN"_s, jsNumber(longestThreadName));
#endif
    return module;
}

} } // namespace JSC::Python
