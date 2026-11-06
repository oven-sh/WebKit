/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "DeferTermination.h"
#include "JSCInlines.h"
#include "JSWeakMapInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyCodecs.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyStructSeq.h"
#include "PyThreads.h"
#include "StrongInlines.h"
#include "TopExceptionScope.h"
#include "WeakMapImplInlines.h"
#include <wtf/Threading.h>

#if !OS(WINDOWS)
#include <pthread.h>
#include <unistd.h>
#endif
#if OS(LINUX)
#include <sys/syscall.h>
#endif

namespace JSC {
namespace Python {

// The modules _thread and atexit. (Modules/_threadmodule.c and atexitmodule.c in CPython are the reference.)
//
// What threads are is in PyThreads.h. All the state here is read and written with the GIL held, so a lock is a number, and
// to wait for one is Threads::waitUntil().

static constexpr double threadTimeoutMax = 9223372036.0;

uint64_t currentThreadIdent()
{
#if OS(WINDOWS)
    return Thread::currentSingleton().uid();
#else
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(pthread_self())));
#endif
}

bool nanosecondsFromSeconds(JSGlobalObject* globalObject, JSValue value, TimeRounding rounding, int64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr int64_t nanosecondsPerSecond = 1000000000;
    if (hasIndex(globalObject, value)) {
        JSValue index = toIndex(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        auto seconds = tryInt64(index);
        if (!seconds || __builtin_mul_overflow(*seconds, nanosecondsPerSecond, &result)) {
            raiseOverflowError(globalObject, scope, "timestamp out of range for C PyTime_t"_s);
            return false;
        }
        return true;
    }
    double seconds = toDouble(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (std::isnan(seconds)) {
        raiseValueError(globalObject, scope, "Invalid value NaN (not a number)"_s);
        return false;
    }
    double nanoseconds = seconds * static_cast<double>(nanosecondsPerSecond);
    bool roundsUp = rounding == TimeRounding::Ceiling || (rounding == TimeRounding::AwayFromZero && nanoseconds >= 0);
    nanoseconds = roundsUp ? std::ceil(nanoseconds) : std::floor(nanoseconds);
    if (!(nanoseconds >= -0x1p63 && nanoseconds < 0x1p63)) {
        raiseOverflowError(globalObject, scope, "timestamp out of range for C PyTime_t"_s);
        return false;
    }
    result = static_cast<int64_t>(nanoseconds);
    return true;
}

// The arguments of acquire(blocking=True, timeout=-1), as nanoseconds to wait: negative is for ever. False: an exception.
static bool lockAcquireArguments(JSGlobalObject* globalObject, JSValue blockingValue, JSValue timeoutValue, int64_t& timeout)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr int64_t unset = -1000000000;
    bool blocking = !blockingValue || isTrue(globalObject, blockingValue);
    RETURN_IF_EXCEPTION(scope, false);
    timeout = unset;
    if (timeoutValue && failed(scope, nanosecondsFromSeconds(globalObject, timeoutValue, TimeRounding::AwayFromZero, timeout)))
        return false;
    if (!blocking && timeout != unset) {
        raiseValueError(globalObject, scope, "can't specify a timeout for a non-blocking call"_s);
        return false;
    }
    if (timeout < 0 && timeout != unset) {
        raiseValueError(globalObject, scope, "timeout value must be a non-negative number"_s);
        return false;
    }
    if (!blocking)
        timeout = 0;
    return true;
}

static ALWAYS_INLINE Threads& threadsOf(JSGlobalObject* globalObject) { return runtime(globalObject).threads(); }

// One optional positional argument, with the complaints of PyArg_ParseTuple(). False: an exception.
static bool threadOptionalArgument(JSGlobalObject* globalObject, ASCIILiteral qualifiedName, ASCIILiteral name, const CallArgs& args, JSValue& argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.keywordCount()) {
        raiseTypeError(globalObject, scope, qualifiedName, "() takes no keyword arguments"_s);
        return false;
    }
    if (args.size() > 1) {
        raiseTypeError(globalObject, scope, name, "() takes at most 1 argument ("_s, args.size(), " given)"_s);
        return false;
    }
    if (args.size())
        argument = args[0];
    return true;
}

// ---- lock ---- (number: whether it is held)

static bool threadTakesNoArguments(JSGlobalObject* globalObject, ASCIILiteral name, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size()) {
        raiseTypeError(globalObject, scope, name, "() takes no positional arguments"_s);
        return false;
    }
    if (args.keywordCount()) {
        raiseTypeError(globalObject, scope, name, "() takes no keyword arguments"_s);
        return false;
    }
    return true;
}

static JSValue lockNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, threadTakesNoArguments(globalObject, "lock"_s, args)))
        return { };
    RELEASE_AND_RETURN(scope, PyRecord::create(globalObject, PyType::from(cls), 0));
}

static JSValue lockAcquire(JSGlobalObject* globalObject, JSValue self, JSValue blockingValue, JSValue timeoutValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t timeout;
    if (failed(scope, lockAcquireArguments(globalObject, blockingValue, timeoutValue, timeout)))
        return { };
    auto* lock = PyRecord::from(self);
    bool isFree = threadsOf(globalObject).waitUntil(globalObject, lock, timeout, "the lock is held"_s, "acquire lock at interpreter finalization"_s, [&] { return !lock->number; }, true);
    RETURN_IF_EXCEPTION(scope, { });
    if (isFree)
        lock->number = 1;
    return jsBoolean(isFree);
}

static JSValue lockRelease(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* lock = PyRecord::from(self);
    if (!lock->number)
        return raiseRuntimeError(globalObject, scope, "release unlocked lock"_s);
    lock->number = 0;
    threadsOf(globalObject).wakeOne(lock);
    return jsUndefined();
}

static JSValue lockEnter(JSGlobalObject* globalObject, JSValue self) { return lockAcquire(globalObject, self, { }, { }); }
static JSValue lockExit(JSGlobalObject* globalObject, JSValue self, JSValue, JSValue, JSValue) { return lockRelease(globalObject, self); }
static JSValue lockLocked(JSGlobalObject*, JSValue self) { return jsBoolean(!!PyRecord::from(self)->number); }

static JSValue lockReinit(JSGlobalObject*, JSValue self)
{
    PyRecord::from(self)->number = 0;
    return jsUndefined();
}

static JSValue lockRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* lock = PyRecord::from(self);
    return makeStr(globalObject, makeString('<', lock->number ? "locked"_s : "unlocked"_s, ' ', qualifiedTypeName(globalObject, lock->type()), " object at "_s, addressForRepr(globalObject, self), '>'));
}

static constexpr NativeDef lockMethods[] = {
    PY_NEW(lockNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", lockRepr, "($self, /)"),
    PY_WAITING_METHOD("acquire", lockAcquire, "($self, /, blocking=True, timeout=-1)"),
    PY_WAITING_METHOD("acquire_lock", lockAcquire, "($self, /, blocking=True, timeout=-1)"),
    PY_METHOD("release", lockRelease, "($self, /)"),
    PY_METHOD("release_lock", lockRelease, "($self, /)"),
    PY_METHOD("locked", lockLocked, "($self, /)"),
    PY_METHOD("locked_lock", lockLocked, "($self, /)"),
    PY_WAITING_METHOD("__enter__", lockEnter, "($self, /)"),
    PY_METHOD("__exit__", lockExit, "($self, exc_type, exc_value, exc_tb, /)"),
    PY_METHOD("_at_fork_reinit", lockReinit, "($self, /)"),
};

static constexpr TypeDef lockTypeDef { "_thread.lock"_s, { TypeFlag::HasWeakref }, lockMethods, { }, { } };

// ---- RLock ---- (number: how many times the thread that holds it has acquired it; field 0: the ident of that thread)

static const TypeDef& rlockType();

static JSValue rlockNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A class derived from it may have an __init__ that takes some.
    PyType* type = PyType::from(cls);
    PyType* base = runtime(globalObject).typeFor(rlockType());
    if ((type == base || type->lookup(Special::Init) == base->lookup(Special::Init)) && failed(scope, threadTakesNoArguments(globalObject, "RLock"_s, args)))
        return { };
    RELEASE_AND_RETURN(scope, PyRecord::create(globalObject, type, 1));
}

// An ident is an address.
static ALWAYS_INLINE JSValue rlockIdentValue(uint64_t ident) { return jsNumber(static_cast<double>(ident)); }
static ALWAYS_INLINE uint64_t rlockOwner(PyRecord* lock) { return lock->number ? static_cast<uint64_t>(lock->get(0).asNumber()) : 0; }
static ALWAYS_INLINE bool rlockIsMine(PyRecord* lock) { return lock->number && rlockOwner(lock) == currentThreadIdent(); }

static JSValue rlockAcquire(JSGlobalObject* globalObject, JSValue self, JSValue blockingValue, JSValue timeoutValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t timeout;
    if (failed(scope, lockAcquireArguments(globalObject, blockingValue, timeoutValue, timeout)))
        return { };
    auto* lock = PyRecord::from(self);
    if (rlockIsMine(lock)) {
        ++lock->number;
        return jsBoolean(true);
    }
    bool isFree = threadsOf(globalObject).waitUntil(globalObject, lock, timeout, "the lock is held"_s, "acquire lock at interpreter finalization"_s, [&] { return !lock->number; }, true);
    RETURN_IF_EXCEPTION(scope, { });
    if (isFree) {
        lock->number = 1;
        lock->set(vm, 0, rlockIdentValue(currentThreadIdent()));
    }
    return jsBoolean(isFree);
}

static JSValue rlockRelease(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* lock = PyRecord::from(self);
    if (!rlockIsMine(lock))
        return raiseRuntimeError(globalObject, scope, "cannot release un-acquired lock"_s);
    if (!--lock->number)
        threadsOf(globalObject).wakeOne(lock);
    return jsUndefined();
}

static JSValue rlockEnter(JSGlobalObject* globalObject, JSValue self) { return rlockAcquire(globalObject, self, { }, { }); }
static JSValue rlockExit(JSGlobalObject* globalObject, JSValue self, JSValue, JSValue, JSValue) { return rlockRelease(globalObject, self); }
static JSValue rlockLocked(JSGlobalObject*, JSValue self) { return jsBoolean(!!PyRecord::from(self)->number); }
static JSValue rlockIsOwned(JSGlobalObject*, JSValue self) { return jsBoolean(rlockIsMine(PyRecord::from(self))); }
static JSValue rlockRecursionCount(JSGlobalObject*, JSValue self)
{
    auto* lock = PyRecord::from(self);
    return jsNumber(rlockIsMine(lock) ? static_cast<double>(lock->number) : 0);
}

static JSValue rlockReleaseSave(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* lock = PyRecord::from(self);
    if (!rlockIsMine(lock))
        return raiseRuntimeError(globalObject, scope, "cannot release un-acquired lock"_s);
    JSValue state = PyTuple::create(globalObject, { jsNumber(static_cast<double>(lock->number)), makeInt(globalObject, rlockOwner(lock)) });
    lock->number = 0;
    threadsOf(globalObject).wakeOne(lock);
    return state;
}

static JSValue rlockAcquireRestore(JSGlobalObject* globalObject, JSValue self, JSValue state)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* tuple = PyTuple::tryFrom(state);
    if (!tuple)
        return raiseTypeError(globalObject, scope, "_acquire_restore() argument must be 2-item tuple, not "_s, typeName(globalObject, state));
    if (tuple->length() != 2)
        return raiseTypeError(globalObject, scope, "_acquire_restore() argument must be tuple of length 2, not "_s, tuple->length());
    int64_t count = toInt64(globalObject, tuple->at(0));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue owner = toIndex(globalObject, tuple->at(1));
    RETURN_IF_EXCEPTION(scope, { });
    auto* lock = PyRecord::from(self);
    threadsOf(globalObject).waitUntil(globalObject, lock, -1, "the lock is held"_s, "acquire lock at interpreter finalization"_s, [&] { return !lock->number; });
    RETURN_IF_EXCEPTION(scope, { });
    lock->number = count;
    lock->set(vm, 0, rlockIdentValue(tryUInt64(owner).value_or(0)));
    return jsUndefined();
}

static JSValue rlockRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* lock = PyRecord::from(self);
    return makeStr(globalObject, makeString('<', lock->number ? "locked"_s : "unlocked"_s, ' ', qualifiedTypeName(globalObject, lock->type()), " object owner="_s, rlockOwner(lock), " count="_s, lock->number, " at "_s, addressForRepr(globalObject, self), '>'));
}

static constexpr NativeDef rlockMethods[] = {
    PY_NEW(rlockNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", rlockRepr, "($self, /)"),
    PY_WAITING_METHOD("acquire", rlockAcquire, "($self, /, blocking=True, timeout=-1)"),
    PY_METHOD("release", rlockRelease, "($self, /)"),
    PY_METHOD("locked", rlockLocked, "($self, /)"),
    PY_WAITING_METHOD("__enter__", rlockEnter, "($self, /)"),
    PY_METHOD("__exit__", rlockExit, "($self, exc_type, exc_value, exc_tb, /)"),
    PY_METHOD("_is_owned", rlockIsOwned, "($self, /)"),
    PY_METHOD("_recursion_count", rlockRecursionCount, "($self, /)"),
    PY_METHOD("_release_save", rlockReleaseSave, "($self, /)"),
    PY_METHOD("_acquire_restore", rlockAcquireRestore, "($self, state, /)"),
    PY_METHOD("_at_fork_reinit", lockReinit, "($self, /)"),
};

static constexpr TypeDef rlockTypeDef { "_thread.RLock"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasWeakref }, rlockMethods, { }, { } };
static const TypeDef& rlockType() { return rlockTypeDef; }

// ---- _local ----
//
// Each thread has a dict of its own for the attributes of the object. It is in a weak map that belongs to the thread
// (Runtime::threadLocals()), whose keys are the objects: so it goes when either the thread or the object does. It is not the __dict__
// of the object, as it is not in CPython: to object.__getattribute__() and its kin a local has no attributes, and an instance of a
// derived class those of a dict of its own, which nothing here looks at. The methods below search and store "with this dict"
// (genericGetAttrWithDict()). The dict of the thread that came last is at hand in the object.

enum LocalHiddenField : unsigned {
    LocalArguments, // What the object was made with: each thread calls __init__ with the same.
    LocalKeywords,
    LocalAtHand, // A tuple: the serial number of a thread, and its dict. Or 0.
};

static ALWAYS_INLINE JSValue localThisThread(JSGlobalObject* globalObject) { return jsNumber(static_cast<double>(threadsOf(globalObject).current().serial)); }

static void localPutAtHand(JSGlobalObject* globalObject, PyInstance* self, PyDict* dict)
{
    setHiddenField(globalObject, self, LocalAtHand, PyTuple::create(globalObject, { localThisThread(globalObject), dict }));
}

static bool localHasOwnInit(JSGlobalObject* globalObject, PyType* type)
{
    auto* init = PyNativeFunction::tryFrom(type->lookup(Special::Init));
    return !init || init->ownerType() != runtime(globalObject).objectType();
}

static PyDict* localMakeDict(JSGlobalObject* globalObject, PyInstance* self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (!rt.threadLocals() || !rt.threadLocals().isCell())
        rt.setThreadLocals(JSWeakMap::create(vm, globalObject->weakMapStructure()));
    PyDict* dict = PyDict::create(globalObject);
    uncheckedDowncast<JSWeakMap>(rt.threadLocals().asCell())->set(vm, self, dict);
    localPutAtHand(globalObject, self, dict);
    return dict;
}

// The dict of the thread that runs. Null: an exception, from __init__.
static PyDict* localDict(JSGlobalObject* globalObject, JSValue selfValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* self = PyInstance::from(selfValue);
    if (JSValue atHand = hiddenField(globalObject, self, LocalAtHand); atHand && atHand.isCell() && PyTuple::from(atHand)->at(0) == localThisThread(globalObject)) [[likely]]
        return PyDict::from(PyTuple::from(atHand)->at(1));
    if (JSValue map = rt.threadLocals(); map && map.isCell()) {
        if (JSValue dict = uncheckedDowncast<JSWeakMap>(map.asCell())->get(self); dict && !dict.isUndefined()) {
            localPutAtHand(globalObject, self, PyDict::from(dict));
            return PyDict::from(dict);
        }
    }
    PyDict* dict = localMakeDict(globalObject, self);
    if (!localHasOwnInit(globalObject, self->type()))
        return dict;
    JSValue keywords = hiddenField(globalObject, self, LocalKeywords);
    JSValue init = getAttr(globalObject, self, rt.name(Special::Init));
    RETURN_IF_EXCEPTION(scope, nullptr);
    callWithTupleAndDict(globalObject, init, PyTuple::from(hiddenField(globalObject, self, LocalArguments)), keywords ? PyDict::from(keywords) : nullptr);
    if (scope.exception()) [[unlikely]] {
        // The next use starts afresh.
        uncheckedDowncast<JSWeakMap>(rt.threadLocals().asCell())->remove(self);
        setHiddenField(globalObject, self, LocalAtHand, jsNumber(0));
        return nullptr;
    }
    return dict;
}

// The weak map of a thread goes with it. Those of its dicts that are at hand must not stay there.
static void localTakeDictsOutOfPlace(JSGlobalObject* globalObject, JSValue map, const ThreadState& thread)
{
    if (!map || !map.isCell())
        return;
    JSValue serial = jsNumber(static_cast<double>(thread.serial));
    MarkedArgumentBuffer keysAndValues;
    uncheckedDowncast<JSWeakMap>(map.asCell())->takeSnapshot(keysAndValues);
    for (unsigned i = 0; i < keysAndValues.size(); i += 2) {
        auto* self = PyInstance::tryFrom(keysAndValues.at(i));
        if (!self)
            continue;
        if (JSValue atHand = hiddenField(globalObject, self, LocalAtHand); atHand && atHand.isCell() && PyTuple::from(atHand)->at(0) == serial)
            setHiddenField(globalObject, self, LocalAtHand, jsNumber(0));
    }
}

// The thread is about to end.
static void localForgetThisThread(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    localTakeDictsOutOfPlace(globalObject, rt.threadLocals(), threadsOf(globalObject).current());
    rt.setThreadLocals(jsUndefined());
}

void forgetThreadLocalsOfOtherThreads(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.threads().forEachThread([&](ThreadState& thread) {
        if (!thread.saved)
            return;
        for (auto& entry : thread.saved->entries) {
            if (entry.runtime != &rt)
                continue;
            localTakeDictsOutOfPlace(globalObject, entry.state.threadLocals, thread);
            entry.state.threadLocals = jsUndefined();
        }
    });
}

static JSValue localNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    if (args.totalCount() && !localHasOwnInit(globalObject, type))
        return raiseTypeError(globalObject, scope, "Initialization arguments are not supported"_s);
    PyInstance* self = PyInstance::create(vm, type);
    setHiddenField(globalObject, self, LocalArguments, PyTuple::create(globalObject, args.positional()));
    if (args.keywordCount()) {
        PyDict* keywords = PyDict::create(globalObject);
        mergeKeywordsInto(globalObject, keywords, args);
        RETURN_IF_EXCEPTION(scope, { });
        setHiddenField(globalObject, self, LocalKeywords, keywords);
    }
    localMakeDict(globalObject, self);
    return self;
}

// In CPython's order, which a name that is no plain str can tell, and a '__class__' in the dict: first whether the name is '__dict__'; then, unless the class is derived, the dict of
// the thread before anything else; then the search that any object has, with that dict.
static JSValue localGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    PyDict* dict = localDict(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    bool isDict = name.isString() ? string->length() == 8 && string->equal(globalObject, rt.names.dDict()) : richCompareBool(globalObject, name, rt.names.dDict(), CompareOperator::Eq);
    RETURN_IF_EXCEPTION(scope, { });
    if (isDict) [[unlikely]]
        return dict;
    if (!PyInstance::from(self)->type()->isHeapType()) {
        JSValue value = name.isString() && dict->hasOnlyStringKeys() ? dict->get(string) : dict->get(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
    }
    RELEASE_AND_RETURN(scope, genericGetAttrWithDict(globalObject, self, string, name, dict));
}

// An empty value deletes.
static JSValue localSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue name, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    PyDict* dict = localDict(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    bool isDict = name.isString() ? string->equal(globalObject, rt.names.dDict()) : richCompareBool(globalObject, name, rt.names.dDict(), CompareOperator::Eq);
    RETURN_IF_EXCEPTION(scope, { });
    if (isDict)
        return raise(globalObject, scope, rt.AttributeError(), makeString('\'', rt.typeOf(self)->nameInMessages(), "' object attribute '"_s, stringOf(string), "' is read-only"_s));
    genericSetAttrWithDict(globalObject, self, string, name, value, dict);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue localDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue name) { return localSetAttr(globalObject, self, name, { }); }

static constexpr NativeDef localMethods[] = {
    PY_NEW(localNew, "($type, *args, **kwargs)"),
    PY_SLOT("__getattribute__", localGetAttribute, "($self, name, /)"),
    PY_SLOT("__setattr__", localSetAttr, "($self, name, value, /)"),
    PY_SLOT("__delattr__", localDelAttr, "($self, name, /)"),
};

static constexpr TypeDef localTypeDef { "_thread._local"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasWeakref }, localMethods, { }, { } };

// ---- _ThreadHandle ---- (number: the state; field 0: the ident)

enum ThreadHandleState : int64_t {
    ThreadNotStarted,
    ThreadRunning,
    ThreadDone,
};

static PyRecord* threadMakeHandle(JSGlobalObject* globalObject, PyType* type, ThreadHandleState state, JSValue ident)
{
    PyRecord* handle = PyRecord::create(globalObject, type, 1);
    handle->number = state;
    handle->set(getVM(globalObject), 0, ident);
    return handle;
}

static JSValue threadHandleNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&) { return threadMakeHandle(globalObject, PyType::from(cls), ThreadNotStarted, jsNumber(0)); }
static JSValue threadHandleGetIdent(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(0); }
static JSValue threadHandleIsDone(JSGlobalObject*, JSValue self) { return jsBoolean(PyRecord::from(self)->number == ThreadDone); }

static bool threadHandleCheckStarted(JSGlobalObject* globalObject, PyRecord* handle)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (handle->number != ThreadNotStarted)
        return true;
    raiseRuntimeError(globalObject, scope, "thread not started"_s);
    return false;
}

static JSValue threadHandleSetDone(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* handle = PyRecord::from(self);
    if (failed(scope, threadHandleCheckStarted(globalObject, handle)))
        return { };
    handle->number = ThreadDone;
    threadsOf(globalObject).wakeAll(handle);
    return jsUndefined();
}

static JSValue threadHandleJoin(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* handle = PyRecord::from(self);
    JSValue timeoutValue;
    if (failed(scope, threadOptionalArgument(globalObject, "_ThreadHandle.join"_s, "join"_s, args, timeoutValue)))
        return { };
    int64_t timeout = -1;
    if (timeoutValue && !isNone(timeoutValue) && failed(scope, nanosecondsFromSeconds(globalObject, timeoutValue, TimeRounding::AwayFromZero, timeout)))
        return { };
    if (failed(scope, threadHandleCheckStarted(globalObject, handle)))
        return { };
    if (handle->number == ThreadRunning && tryUInt64(handle->get(0)) == currentThreadIdent())
        return raiseRuntimeError(globalObject, scope, "Cannot join current thread"_s);
    // Whatever the timeout.
    if (handle->number != ThreadDone && threadsOf(globalObject).isFinalized())
        return raise(globalObject, scope, runtime(globalObject).PythonFinalizationError(), "cannot join thread at interpreter shutdown"_s);
    threadsOf(globalObject).waitUntil(globalObject, handle, timeout, "the thread to join is not done"_s, "join thread at interpreter shutdown"_s, [&] { return handle->number == ThreadDone; }, true);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue threadHandleRepr(JSGlobalObject* globalObject, JSValue self)
{
    return makeStr(globalObject, makeString("<_thread._ThreadHandle object: ident="_s, intToString(globalObject, PyRecord::from(self)->get(0)), '>'));
}

static constexpr NativeDef threadHandleMethods[] = {
    PY_NEW(threadHandleNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", threadHandleRepr, "($self, /)"),
    PY_METHOD("is_done", threadHandleIsDone, "($self, /)"),
    PY_WAITING_METHOD("join", threadHandleJoin, "($self, *args, **kwargs)"),
    PY_METHOD("_set_done", threadHandleSetDone, "($self, /)"),
};

static constexpr NativeGetSet threadHandleGetSets[] = {
    { "ident"_s, threadHandleGetIdent },
};

static constexpr TypeDef threadHandleTypeDef { "_thread._ThreadHandle"_s, { }, threadHandleMethods, threadHandleGetSets, { } };

// ---- Functions ----

// Never seen from Python: its dict is where the module keeps what it must find again whatever is done to the module.
static constexpr TypeDef threadStateTypeDef { "_thread._state"_s, { TypeFlag::DisallowInstantiation, TypeFlag::Internal }, { }, { }, { } };

static constexpr ASCIILiteral exceptHookArgsFields[] = { "exc_type"_s, "exc_value"_s, "exc_traceback"_s, "thread"_s };
static constexpr StructSeqDef exceptHookArgsDef { "_thread._ExceptHookArgs"_s, exceptHookArgsFields, 4 };

static PyType* exceptHookArgsType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyDict* state = rt.typeFor(threadStateTypeDef)->dict();
    JSString* key = internString(vm, "_ExceptHookArgs"_s);
    if (JSValue type = state->get(key))
        return PyType::from(type);
    PyType* type = makeStructSeqType(globalObject, exceptHookArgsDef);
    state->set(vm, key, type);
    return type;
}

static JSValue threadAllocateLock(JSGlobalObject* globalObject) { return PyRecord::create(globalObject, runtime(globalObject).typeFor(lockTypeDef), 0); }
static JSValue threadGetIdent(JSGlobalObject* globalObject) { return makeInt(globalObject, currentThreadIdent()); }
static JSValue threadGetMainThreadIdent(JSGlobalObject* globalObject) { return makeInt(globalObject, threadsOf(globalObject).mainThreadIdent()); }
static JSValue threadCount(JSGlobalObject* globalObject) { return jsNumber(threadsOf(globalObject).runningCount()); }
static JSValue threadTrue(JSGlobalObject*) { return jsBoolean(true); }

static JSValue threadGetNativeID(JSGlobalObject*)
{
#if OS(LINUX)
    return jsNumber(static_cast<double>(syscall(SYS_gettid)));
#elif OS(DARWIN)
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return jsNumber(static_cast<double>(id));
#else
    return jsNumber(static_cast<double>(Thread::currentSingleton().uid()));
#endif
}

#if OS(LINUX) || OS(DARWIN)
#define PY_HAVE_THREAD_NAMES 1
#if OS(LINUX)
static constexpr size_t threadNameMaxLength = 15;
#else
static constexpr size_t threadNameMaxLength = 63;
#endif

static JSValue threadGetName(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char name[100];
    if (int error = pthread_getname_np(pthread_self(), name, sizeof(name)))
        return raiseOSError(globalObject, scope, error);
    RELEASE_AND_RETURN(scope, codecDecodeUTF8(globalObject, std::span { byteCast<uint8_t>(&name[0]), strlen(name) }, "surrogateescape"_s));
}

static JSValue threadSetName(JSGlobalObject* globalObject, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(name))
        return raiseTypeError(globalObject, scope, "set_name() argument 'name' must be str, not "_s, typeName(globalObject, name));
    JSValue encoded = codecEncodeUTF8(globalObject, strValue(name), "replace"_s);
    RETURN_IF_EXCEPTION(scope, { });
    auto bytes = PyBytes::from(encoded)->span();
    char buffer[threadNameMaxLength + 1] = { };
    memcpySpan(std::span { byteCast<uint8_t>(&buffer[0]), threadNameMaxLength }, bytes.first(std::min(bytes.size(), threadNameMaxLength)));
#if OS(DARWIN)
    int error = pthread_setname_np(buffer);
#else
    int error = pthread_setname_np(pthread_self(), buffer);
#endif
    if (error)
        return raiseOSError(globalObject, scope, error);
    return jsUndefined();
}
#else
#define PY_HAVE_THREAD_NAMES 0
#endif

// The handles of the threads that are not daemons and have not ended: the interpreter waits for them before it exits.
static JSArray* threadShutdownHandles(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyDict* state = rt.typeFor(threadStateTypeDef)->dict();
    JSString* key = internString(vm, "shutdown_handles"_s);
    if (JSValue list = state->get(key))
        return asList(list);
    JSArray* list = newList(globalObject);
    state->set(vm, key, list);
    return list;
}

static void threadRemoveShutdownHandle(JSGlobalObject* globalObject, PyRecord* handle)
{
    JSArray* handles = threadShutdownHandles(globalObject);
    for (unsigned i = 0; i < listLength(handles); ++i) {
        if (listGet(handles, i) == JSValue(handle)) {
            listRemove(globalObject, handles, i, 1);
            return;
        }
    }
}

// A frame of Python code takes a few hundred bytes, but what is written in C++ and calls back into Python takes far more, above
// all in a build that checks every access to memory.
#if ASAN_ENABLED || ASSERT_ENABLED
static constexpr size_t threadStackScale = 8;
#else
static constexpr size_t threadStackScale = 1;
#endif
static constexpr size_t threadDefaultStackSize = threadStackScale * 8 * MB;

// What the new thread does.
static void threadRun(JSGlobalObject* globalObject, JSValue function, PyTuple* args, PyDict* kwargs, PyRecord* handle)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    callWithTupleAndDict(globalObject, function, args, kwargs);
    if (scope.exception()) [[unlikely]] {
        if (!vm.hasPendingTerminationException() && !clearPendingExceptionIfMatches(globalObject, rt.SystemExit()))
            reportUnraisable(globalObject, makeString("Exception ignored in thread started by "_s, reprForMessage(globalObject, function)), { });
        // The host has ended the function, or the hook that was told what it raised.
        if (vm.hasPendingTerminationException())
            scope.clearException();
    }
    localForgetThisThread(globalObject);
    threadRemoveShutdownHandle(globalObject, handle);
    scope.assertNoException();
    handle->number = ThreadDone;
    rt.threads().wakeAll(handle);
}

// False: an exception.
static bool threadStart(JSGlobalObject* globalObject, JSValue function, PyTuple* args, PyDict* kwargs, PyRecord* handle, bool isDaemon)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (rt.threads().isFinalized()) {
        raise(globalObject, scope, rt.PythonFinalizationError(), "can't create new thread at interpreter shutdown"_s);
        return false;
    }
    if (handle->number != ThreadNotStarted) {
        raiseRuntimeError(globalObject, scope, "thread already started"_s);
        return false;
    }
    if (!isDaemon) {
        listAppend(globalObject, threadShutdownHandles(globalObject), handle);
        RETURN_IF_EXCEPTION(scope, false);
    }

    size_t stackSize = threadDefaultStackSize;
    // What threading.stack_size() asks for is what the program gets to use. The end of every stack is the engine's reserve, where
    // nothing of the program's runs: with a stack no larger than that, a thread could not take a step.
    if (JSValue size = rt.typeFor(threadStateTypeDef)->dict()->get(internString(vm, "stack_size"_s)); size && intSign(size) > 0)
        stackSize = static_cast<size_t>(tryUInt64(size).value_or(8 * MB)) * threadStackScale + Options::softReservedZoneSize();

    // The thread is running as far as anybody can tell from here on: it may be over by the time start() returns.
    handle->number = ThreadRunning;
    uint64_t ident = 0;
    int error = rt.threads().start(stackSize, [globalObject, function = Strong<Unknown>(vm, function), args = Strong<PyTuple>(vm, args), kwargs = Strong<PyDict>(vm, kwargs), handle = Strong<PyRecord>(vm, handle)] {
        handle.get()->set(getVM(globalObject), 0, makeInt(globalObject, currentThreadIdent()));
        threadRun(globalObject, function.get(), args.get(), kwargs.get(), handle.get());
    }, ident);
    if (error) {
        handle->number = ThreadNotStarted;
        if (!isDaemon)
            threadRemoveShutdownHandle(globalObject, handle);
        raiseRuntimeError(globalObject, scope, "can't start new thread"_s);
        return false;
    }
    handle->set(vm, 0, makeInt(globalObject, ident));
    return true;
}

static JSValue threadStartNew(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.keywordCount())
        return raiseTypeError(globalObject, scope, "_thread.start_new_thread() takes no keyword arguments"_s);
    if (args.size() < 2)
        return raiseTypeError(globalObject, scope, "start_new_thread expected at least 2 arguments, got "_s, args.size());
    if (args.size() > 3)
        return raiseTypeError(globalObject, scope, "start_new_thread expected at most 3 arguments, got "_s, args.size());
    if (!isCallable(globalObject, args[0]))
        return raiseTypeError(globalObject, scope, "first arg must be callable"_s);
    if (!PyTuple::tryFrom(args[1]))
        return raiseTypeError(globalObject, scope, "2nd arg must be a tuple"_s);
    if (args.size() == 3 && !PyDict::tryFrom(args[2]))
        return raiseTypeError(globalObject, scope, "optional 3rd arg must be a dictionary"_s);
    if (failed(scope, audit(globalObject, "_thread.start_new_thread"_s, args[0], args[1], args.get(2))))
        return { };
    PyRecord* handle = threadMakeHandle(globalObject, rt.typeFor(threadHandleTypeDef), ThreadNotStarted, jsNumber(0));
    if (failed(scope, threadStart(globalObject, args[0], PyTuple::from(args[1]), args.size() == 3 ? PyDict::from(args[2]) : nullptr, handle, true)))
        return { };
    return handle->get(0);
}

static JSValue threadStartJoinable(JSGlobalObject* globalObject, JSValue function, JSValue handleValue, JSValue daemon)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isDaemon = !daemon || isTrue(globalObject, daemon);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isCallable(globalObject, function))
        return raiseTypeError(globalObject, scope, "thread function must be callable"_s);
    PyRecord* handle = nullptr;
    if (handleValue && !isNone(handleValue)) {
        handle = PyRecord::tryFrom(handleValue, rt.typeFor(threadHandleTypeDef));
        if (!handle)
            return raiseTypeError(globalObject, scope, "'handle' must be a _ThreadHandle"_s);
    }
    if (failed(scope, audit(globalObject, "_thread.start_joinable_thread"_s, function, jsNumber(isDaemon), handle)))
        return { };
    if (!handle)
        handle = threadMakeHandle(globalObject, rt.typeFor(threadHandleTypeDef), ThreadNotStarted, jsNumber(0));
    if (failed(scope, threadStart(globalObject, function, rt.emptyTuple(), nullptr, handle, isDaemon)))
        return { };
    return handle;
}

// Waits for the threads that are not daemons.
static JSValue threadShutdown(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* handles = threadShutdownHandles(globalObject);
    while (true) {
        PyRecord* handle = nullptr;
        for (unsigned i = 0; i < listLength(handles) && !handle; ++i) {
            auto* candidate = PyRecord::from(listGet(handles, i));
            if (tryUInt64(candidate->get(0)) != currentThreadIdent())
                handle = candidate;
        }
        if (!handle)
            return jsUndefined();
        threadsOf(globalObject).waitUntil(globalObject, handle, -1, "a thread is still running"_s, "join thread at interpreter shutdown"_s, [&] { return handle->number == ThreadDone; });
        // Interrupted from the keyboard, say.
        RETURN_IF_EXCEPTION(scope, { });
    }
}

static JSValue threadExit(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseWithArgs(globalObject, scope, rt.SystemExit(), { });
}

// As if the signal had arrived: its handler runs at the next safe point.
static JSValue threadInterruptMain(JSGlobalObject* globalObject, JSValue numberValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int number = 2; // SIGINT
    if (numberValue) {
        number = toCInt(globalObject, numberValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!simulateSignal(globalObject, number))
        return raiseValueError(globalObject, scope, "signal number out of range"_s);
    return jsUndefined();
}

// For the threads started from now on.
static JSValue threadStackSize(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr int64_t minimum = 53248;
    JSValue sizeValue;
    if (failed(scope, threadOptionalArgument(globalObject, "_thread.stack_size"_s, "stack_size"_s, args, sizeValue)))
        return { };
    int64_t size = 0;
    if (sizeValue) {
        size = toSsize(globalObject, sizeValue, rt.OverflowError());
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (size && size < minimum)
        return raiseValueError(globalObject, scope, "size must be at least "_s, minimum, " bytes"_s);
    PyDict* state = rt.typeFor(threadStateTypeDef)->dict();
    JSString* key = internString(vm, "stack_size"_s);
    JSValue previous = state->get(key);
    state->set(vm, key, makeInt(globalObject, size));
    return previous ? previous : jsNumber(0);
}

static JSValue threadMakeThreadHandle(JSGlobalObject* globalObject, JSValue ident)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isInt(ident))
        return raiseTypeError(globalObject, scope, "ident must be an integer"_s);
    JSValue value = intValue(ident);
    if (intSign(value) < 0)
        return raiseOverflowError(globalObject, scope, "can't convert negative int to unsigned"_s);
    if (!tryUInt64(value))
        return raiseOverflowError(globalObject, scope, "int too big to convert"_s);
    RELEASE_AND_RETURN(scope, threadMakeHandle(globalObject, runtime(globalObject).typeFor(threadHandleTypeDef), ThreadRunning, value));
}

static JSValue threadExceptHook(JSGlobalObject* globalObject, JSValue args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (rt.typeOf(args) != exceptHookArgsType(globalObject))
        return raiseTypeError(globalObject, scope, "_thread.excepthook argument type must be ExceptHookArgs"_s);
    auto* tuple = PyTuple::from(args);
    if (tuple->at(0) == JSValue(rt.SystemExit()))
        return jsUndefined();
    JSValue thread = tuple->at(3);

    JSValue file = rt.sysDict()->get(rt.names.stderr_());
    if (!file || isNone(file)) {
        if (isNone(thread))
            return jsUndefined();
        // What sys.stderr was when the thread was made.
        file = getAttr(globalObject, thread, internString(vm, "_stderr"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (isNone(file))
            return jsUndefined();
    }

    JSValue name;
    if (!isNone(thread)) {
        name = getAttrIfPresent(globalObject, thread, rt.names.name());
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSString* nameText = name ? str(globalObject, name) : makeStr(globalObject, String::number(currentThreadIdent()));
    RETURN_IF_EXCEPTION(scope, { });
    String text = makeString("Exception in thread "_s, nameText->value(globalObject).data, ":\n"_s);
    RETURN_IF_EXCEPTION(scope, { });
    writeToFile(globalObject, file, makeStr(globalObject, text));
    RETURN_IF_EXCEPTION(scope, { });
    if (auto* exception = PyException::tryFrom(tuple->at(1)); exception && !exception->traceback() && PyTraceback::is(tuple->at(2)))
        exception->setTraceback(vm, tuple->at(2));
    printException(globalObject, tuple->at(1), file, false);
    callMethod(globalObject, file, internString(vm, "flush"_s));
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static constexpr NativeDef threadFunctions[] = {
    PY_FUNCTION("allocate_lock", threadAllocateLock, "()"),
    PY_FUNCTION("allocate", threadAllocateLock, "()"),
    PY_FUNCTION("get_ident", threadGetIdent, "()"),
    PY_FUNCTION("get_native_id", threadGetNativeID, "()"),
    PY_FUNCTION("_get_main_thread_ident", threadGetMainThreadIdent, "()"),
    PY_FUNCTION("_is_main_interpreter", threadTrue, "()"),
    PY_FUNCTION("daemon_threads_allowed", threadTrue, "()"),
    PY_FUNCTION("_count", threadCount, "()"),
    PY_FUNCTION("_shutdown", threadShutdown, "()"),
    PY_FUNCTION("_make_thread_handle", threadMakeThreadHandle, "(ident, /)"),
    PY_FUNCTION("_excepthook", threadExceptHook, "(args, /)"),
#if PY_HAVE_THREAD_NAMES
    PY_FUNCTION("_get_name", threadGetName, "()"),
    PY_FUNCTION("set_name", threadSetName, "(name)"),
#endif
    PY_FUNCTION("stack_size", threadStackSize, "!(size=0, /)"),
    PY_FUNCTION("start_new_thread", threadStartNew, "!(function, args, kwargs={}, /)"),
    PY_FUNCTION("start_new", threadStartNew, "!(function, args, kwargs={}, /)"),
    PY_FUNCTION("start_joinable_thread", threadStartJoinable, "(function, handle=None, daemon=True)"),
    PY_FUNCTION("exit", threadExit, "()"),
    PY_FUNCTION("exit_thread", threadExit, "()"),
    PY_FUNCTION("interrupt_main", threadInterruptMain, "(signum=2, /)"),
};

void initializeThreadModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->addFunctions(globalObject, threadFunctions);
    PyType* lockType = rt.typeFor(lockTypeDef);
    module->add(globalObject, "lock"_s, lockType);
    module->add(globalObject, "LockType"_s, lockType);
    module->add(globalObject, "RLock"_s, rt.typeFor(rlockTypeDef));
    module->add(globalObject, "_local"_s, rt.typeFor(localTypeDef));
    module->add(globalObject, "_ThreadHandle"_s, rt.typeFor(threadHandleTypeDef));
    module->add(globalObject, "_ExceptHookArgs"_s, exceptHookArgsType(globalObject));
    module->add(globalObject, "error"_s, rt.RuntimeError());
    module->add(globalObject, "TIMEOUT_MAX"_s, makeFloat(globalObject, threadTimeoutMax));
#if PY_HAVE_THREAD_NAMES
    module->add(globalObject, "_NAME_MAXLEN"_s, jsNumber(threadNameMaxLength));
#endif
}

// ---- atexit ----

// The callbacks: a list of (function, args, kwargs), the one registered last first.
static JSArray* atexitCallbacks(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (JSValue list = rt.slot(Runtime::Slot::AtexitCallbacks))
        return asList(list);
    JSArray* list = newList(globalObject);
    rt.setSlot(Runtime::Slot::AtexitCallbacks, list);
    return list;
}

static JSValue atexitRegister(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!args.size())
        return raiseTypeError(globalObject, scope, "register() takes at least 1 argument (0 given)"_s);
    if (!isCallable(globalObject, args[0]))
        return raiseTypeError(globalObject, scope, "the first argument must be callable"_s);
    PyDict* keywords = PyDict::create(globalObject);
    mergeKeywordsInto(globalObject, keywords, args);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue entry = PyTuple::create(globalObject, { args[0], PyTuple::create(globalObject, args.positional().subspan(1)), keywords });
    listInsert(globalObject, atexitCallbacks(globalObject), 0, entry);
    RETURN_IF_EXCEPTION(scope, { });
    return args[0];
}

static JSValue atexitUnregister(JSGlobalObject* globalObject, JSValue function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* callbacks = atexitCallbacks(globalObject);
    // In the order they were registered. An __eq__ may itself unregister something, or everything.
    for (int64_t i = static_cast<int64_t>(listLength(callbacks)) - 1; i >= 0; --i) {
        JSValue entry = listGet(callbacks, static_cast<unsigned>(i));
        bool matches = isEqual(globalObject, function, PyTuple::from(entry)->at(0));
        RETURN_IF_EXCEPTION(scope, { });
        if (matches) {
            for (int64_t j = std::min<int64_t>(i, static_cast<int64_t>(listLength(callbacks)) - 1); j >= 0; --j) {
                if (listGet(callbacks, static_cast<unsigned>(j)) != entry)
                    continue;
                listRemove(globalObject, callbacks, static_cast<unsigned>(j));
                RETURN_IF_EXCEPTION(scope, { });
                i = j;
                break;
            }
        }
        i = std::min<int64_t>(i, listLength(callbacks));
    }
    return jsUndefined();
}

// The last registered is the first called. Those there are now: what they unregister is called all the same, and what they
// register is forgotten with the rest.
static JSValue atexitRunExitFuncs(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* callbacks = atexitCallbacks(globalObject);
    MarkedArgumentBuffer entries;
    for (unsigned i = 0; i < listLength(callbacks); ++i)
        entries.append(listGet(callbacks, i));
    for (unsigned i = 0; i < entries.size(); ++i) {
        auto* entry = PyTuple::from(entries.at(i));
        callWithTupleAndDict(globalObject, entry->at(0), PyTuple::from(entry->at(1)), PyDict::from(entry->at(2)));
        if (scope.exception() && !vm.hasPendingTerminationException())
            reportUnraisable(globalObject, makeString("Exception ignored in atexit callback "_s, reprForMessage(globalObject, entry->at(0))), { });
        if (scope.exception()) [[unlikely]] {
            // The host has ended it. If there is another time, that is for those that have not been called.
            ExceptionSetAside suspended(globalObject);
            DeferTerminationForAWhile deferred(vm);
            auto innerScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            listTruncate(globalObject, callbacks, 0);
            while (++i < entries.size()) {
                listAppend(globalObject, callbacks, entries.at(i));
                dropPendingException(globalObject, innerScope);
            }
            return { };
        }
    }
    listTruncate(globalObject, callbacks, 0);
    return jsUndefined();
}

void runAtexitCallbacks(JSGlobalObject* globalObject) { atexitRunExitFuncs(globalObject); }
void waitForThreadsAtExit(JSGlobalObject* globalObject) { threadShutdown(globalObject); }
void stopWaitingForThreadsAtExit(JSGlobalObject* globalObject) { listTruncate(globalObject, threadShutdownHandles(globalObject), 0); }

bool hasThreadsOrAtexitCallbacks(JSGlobalObject* globalObject)
{
    if (listLength(atexitCallbacks(globalObject)))
        return true;
    JSArray* handles = threadShutdownHandles(globalObject);
    for (unsigned i = 0; i < listLength(handles); ++i) {
        if (tryUInt64(PyRecord::from(listGet(handles, i))->get(0)) != currentThreadIdent())
            return true;
    }
    return false;
}

static JSValue atexitClear(JSGlobalObject* globalObject)
{
    listTruncate(globalObject, atexitCallbacks(globalObject), 0);
    return jsUndefined();
}

static JSValue atexitNCallbacks(JSGlobalObject* globalObject) { return jsNumber(listLength(atexitCallbacks(globalObject))); }

static constexpr NativeDef atexitFunctions[] = {
    PY_FUNCTION("register", atexitRegister, "(func, /, *args, **kwargs)"),
    PY_FUNCTION("unregister", atexitUnregister, "(func, /)"),
    PY_FUNCTION("_run_exitfuncs", atexitRunExitFuncs, "()"),
    PY_FUNCTION("_clear", atexitClear, "()"),
    PY_FUNCTION("_ncallbacks", atexitNCallbacks, "()"),
};

void initializeAtexitModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, atexitFunctions);
}

} // namespace Python
} // namespace JSC
