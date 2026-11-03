/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _multiprocessing: semaphores that processes share. (Modules/_multiprocessing/ in CPython is the reference.)

#if OS(DARWIN)
// There, sem_getvalue() always fails, and there is no sem_timedwait().
#define PY_BROKEN_SEM_GETVALUE 1
#define PY_HAVE_SEM_TIMEDWAIT 0
#else
#define PY_BROKEN_SEM_GETVALUE 0
#define PY_HAVE_SEM_TIMEDWAIT 1
#endif

enum SemLockKind : int { SemLockRecursiveMutex, SemLockSemaphore };

// The number of the record is the sem_t*.
enum SemLockField : unsigned {
    SemLockKindField,
    SemLockMaxValue,
    SemLockName, // None if it has been unlinked.
    SemLockCount, // Times acquired less times released, by this process.
    SemLockLastThread,
    SemLockGuard,
    SemLockFieldCount
};

static sem_t* semLockHandle(PyRecord* lock) { return reinterpret_cast<sem_t*>(static_cast<intptr_t>(lock->number)); }
static int semLockCount(PyRecord* lock) { return lock->get(SemLockCount).asInt32(); }
static void semLockSetCount(JSGlobalObject* globalObject, PyRecord* lock, int count) { lock->set(getVM(globalObject), SemLockCount, jsNumber(count)); }
static JSValue semLockThisThread(JSGlobalObject* globalObject) { return posixMakeInt(globalObject, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()))); }

static bool semLockIsMine(JSGlobalObject* globalObject, PyRecord* lock)
{
    return semLockCount(lock) > 0 && tryUInt64(lock->get(SemLockLastThread)) == tryUInt64(semLockThisThread(globalObject));
}

// The type has no __del__: an object nobody sees closes the handle when the lock cannot be reached.
static JSValue semLockGuardFinalize(JSGlobalObject*, JSValue self)
{
    auto* guard = PyRecord::from(self);
    if (auto* handle = reinterpret_cast<sem_t*>(static_cast<intptr_t>(std::exchange(guard->number, 0))))
        sem_close(handle);
    return jsUndefined();
}

static constexpr NativeDef semLockGuardMethods[] = {
    PY_SLOT("__del__", semLockGuardFinalize, "($self, /)"),
};

static constexpr TypeDef semLockGuardTypeDef { "_multiprocessing._handle_guard"_s, { TypeFlag::DisallowInstantiation, TypeFlag::Internal }, semLockGuardMethods, { }, { } };

static JSValue semLockCreate(JSGlobalObject* globalObject, JSValue type, sem_t* handle, int kind, int maxValue, JSValue name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyRecord* guard = PyRecord::create(globalObject, rt.typeFor(semLockGuardTypeDef), 0);
    guard->number = static_cast<int64_t>(reinterpret_cast<intptr_t>(handle));
    rt.registerFinalizable(guard);
    PyRecord* lock = PyRecord::create(globalObject, PyType::from(type), SemLockFieldCount);
    lock->number = guard->number;
    lock->set(vm, SemLockKindField, jsNumber(kind));
    lock->set(vm, SemLockMaxValue, jsNumber(maxValue));
    lock->set(vm, SemLockName, name);
    lock->set(vm, SemLockCount, jsNumber(0));
    lock->set(vm, SemLockLastThread, jsNumber(0));
    lock->set(vm, SemLockGuard, guard);
    return lock;
}

static bool semLockToName(JSGlobalObject* globalObject, JSValue value, ASCIILiteral complaint, PosixBytes& name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, complaint, posixTypeNameOfArgument(globalObject, value));
        return false;
    }
    String text = strValue(value)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    auto encoded = tryEncodeUTF8(text);
    if (!encoded) {
        // Which raises.
        strEncode(globalObject, strValue(value), makeStr(vm, "utf-8"_s), makeStr(vm, "strict"_s));
        return false;
    }
    name.assign(byteCast<char>(encoded->span()));
    if (name.contains('\0')) {
        raiseValueError(globalObject, scope, "embedded null character"_s);
        return false;
    }
    return true;
}

static JSValue semLockNew(JSGlobalObject* globalObject, JSValue type, JSValue kindValue, JSValue valueValue, JSValue maxValueValue, JSValue nameValue, JSValue unlinkValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int kind;
    int value;
    int maxValue;
    PosixBytes name;
    if (!posixToInt(globalObject, kindValue, kind) || !posixToInt(globalObject, valueValue, value) || !posixToInt(globalObject, maxValueValue, maxValue)
        || !semLockToName(globalObject, nameValue, "SemLock() argument 'name' must be str, not "_s, name))
        return { };
    bool unlinks = isTrue(globalObject, unlinkValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (kind != SemLockRecursiveMutex && kind != SemLockSemaphore)
        return raiseValueError(globalObject, scope, "unrecognized kind"_s);
    sem_t* handle = sem_open(name.characters(), O_CREAT | O_EXCL, 0600, value);
    if (handle == SEM_FAILED)
        return posixError(globalObject);
    if (unlinks && sem_unlink(name.characters()) < 0) {
        int error = errno;
        sem_close(handle);
        return posixError(globalObject, error);
    }
    return semLockCreate(globalObject, type, handle, kind, maxValue, unlinks ? jsUndefined() : nameValue);
}

static JSValue semLockRebuild(JSGlobalObject* globalObject, JSValue type, JSValue handleValue, JSValue kindValue, JSValue maxValueValue, JSValue nameValue)
{
    uint64_t address;
    int kind;
    int maxValue;
    PosixBytes name;
    if (!posixToUnsignedLongLong(globalObject, handleValue, address) || !posixToInt(globalObject, kindValue, kind) || !posixToInt(globalObject, maxValueValue, maxValue)
        || (!isNone(nameValue) && !semLockToName(globalObject, nameValue, "_rebuild() argument 4 must be str or None, not "_s, name)))
        return { };
    auto* handle = reinterpret_cast<sem_t*>(static_cast<uintptr_t>(address));
    // The handle is that of the process that pickled the lock. The name is what another can open it by.
    if (!isNone(nameValue)) {
        handle = sem_open(name.characters(), 0);
        if (handle == SEM_FAILED)
            return posixError(globalObject);
    }
    return semLockCreate(globalObject, type, handle, kind, maxValue, nameValue);
}

#if !PY_HAVE_SEM_TIMEDWAIT
// Tries again and again, at growing intervals of up to 20 ms.
static int semLockTimedWait(JSGlobalObject* globalObject, sem_t* handle, const struct timespec& deadline)
{
    for (unsigned long delay = 0;; delay += 1000) {
        if (!sem_trywait(handle))
            return 0;
        if (errno != EAGAIN)
            return -1;
        struct timeval now;
        if (gettimeofday(&now, nullptr) < 0)
            return -1;
        int64_t left = (static_cast<int64_t>(deadline.tv_sec) - now.tv_sec) * 1000000 + (deadline.tv_nsec / 1000 - now.tv_usec);
        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct timeval pause { 0, static_cast<suseconds_t>(std::min<int64_t>(std::min<unsigned long>(delay, 20000), left)) };
        int result;
        {
            AllowThreads allow(globalObject);
            result = select(0, nullptr, nullptr, nullptr, &pause);
        }
        if (result < 0)
            return -1;
        if (!checkSignals(globalObject)) {
            errno = EINTR;
            return -1;
        }
    }
}
#endif

static JSValue semLockAcquire(JSGlobalObject* globalObject, JSValue self, JSValue blockValue, JSValue timeoutValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* lock = PyRecord::from(self);
    bool blocks;
    if (!posixToBool(globalObject, blockValue, true, blocks))
        return { };
    if (lock->get(SemLockKindField).asInt32() == SemLockRecursiveMutex && semLockIsMine(globalObject, lock)) {
        semLockSetCount(globalObject, lock, semLockCount(lock) + 1);
        return jsBoolean(true);
    }
    bool hasDeadline = timeoutValue && !isNone(timeoutValue);
    struct timespec deadline { };
    if (hasDeadline) {
        double timeout = toDouble(globalObject, timeoutValue);
        RETURN_IF_EXCEPTION(scope, { });
        timeout = std::max(timeout, 0.0);
        struct timeval now;
        if (gettimeofday(&now, nullptr) < 0)
            return posixError(globalObject);
        long seconds = static_cast<long>(std::min(timeout, 1e15));
        long nanoseconds = static_cast<long>(1e9 * (timeout - seconds) + 0.5);
        deadline.tv_sec = now.tv_sec + seconds;
        deadline.tv_nsec = now.tv_usec * 1000 + nanoseconds;
        deadline.tv_sec += deadline.tv_nsec / 1000000000;
        deadline.tv_nsec %= 1000000000;
    }
    sem_t* handle = semLockHandle(lock);
    int result;
    do {
        result = sem_trywait(handle);
    } while (result < 0 && errno == EINTR && checkSignals(globalObject));
    RETURN_IF_EXCEPTION(scope, { });
    if (result < 0 && errno == EAGAIN && blocks) {
        do {
            if (!hasDeadline) {
                AllowThreads allow(globalObject);
                result = sem_wait(handle);
            } else {
#if PY_HAVE_SEM_TIMEDWAIT
                AllowThreads allow(globalObject);
                result = sem_timedwait(handle, &deadline);
#else
                result = semLockTimedWait(globalObject, handle, deadline);
                if (scope.exception())
                    return { };
#endif
            }
        } while (result < 0 && errno == EINTR && checkSignals(globalObject));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (result < 0) {
        if (errno == EAGAIN || errno == ETIMEDOUT)
            return jsBoolean(false);
        return posixError(globalObject);
    }
    semLockSetCount(globalObject, lock, semLockCount(lock) + 1);
    lock->set(vm, SemLockLastThread, semLockThisThread(globalObject));
    return jsBoolean(true);
}

static JSValue semLockRelease(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* lock = PyRecord::from(self);
    sem_t* handle = semLockHandle(lock);
    if (lock->get(SemLockKindField).asInt32() == SemLockRecursiveMutex) {
        if (!semLockIsMine(globalObject, lock))
            return raise(globalObject, scope, rt.AssertionError(), "attempt to release recursive lock not owned by thread"_s);
        if (semLockCount(lock) > 1) {
            semLockSetCount(globalObject, lock, semLockCount(lock) - 1);
            return jsUndefined();
        }
    } else {
#if PY_BROKEN_SEM_GETVALUE
        // Only a lock can be checked: it must be held.
        if (lock->get(SemLockMaxValue).asInt32() == 1) {
            if (!sem_trywait(handle)) {
                if (sem_post(handle) < 0)
                    return posixError(globalObject);
                return raiseValueError(globalObject, scope, "semaphore or lock released too many times"_s);
            }
            if (errno != EAGAIN)
                return posixError(globalObject);
        }
#else
        // Another process may release it between this and sem_post(): it is a check, not a guarantee.
        int value;
        if (sem_getvalue(handle, &value) < 0)
            return posixError(globalObject);
        if (value >= lock->get(SemLockMaxValue).asInt32())
            return raiseValueError(globalObject, scope, "semaphore or lock released too many times"_s);
#endif
    }
    if (sem_post(handle) < 0)
        return posixError(globalObject);
    semLockSetCount(globalObject, lock, semLockCount(lock) - 1);
    return jsUndefined();
}

static JSValue semLockGetCount(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(SemLockCount); }
static JSValue semLockIsMineMethod(JSGlobalObject* globalObject, JSValue self) { return jsBoolean(semLockIsMine(globalObject, PyRecord::from(self))); }

static JSValue semLockGetValue(JSGlobalObject* globalObject, JSValue self)
{
#if PY_BROKEN_SEM_GETVALUE
    UNUSED_PARAM(self);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).NotImplementedError(), String());
#else
    int value;
    if (sem_getvalue(semLockHandle(PyRecord::from(self)), &value) < 0)
        return posixError(globalObject);
    // Some systems count those who wait, below zero.
    return jsNumber(std::max(value, 0));
#endif
}

static JSValue semLockIsZero(JSGlobalObject* globalObject, JSValue self)
{
    sem_t* handle = semLockHandle(PyRecord::from(self));
#if PY_BROKEN_SEM_GETVALUE
    if (sem_trywait(handle) < 0) {
        if (errno == EAGAIN)
            return jsBoolean(true);
        return posixError(globalObject);
    }
    if (sem_post(handle) < 0)
        return posixError(globalObject);
    return jsBoolean(false);
#else
    int value;
    if (sem_getvalue(handle, &value) < 0)
        return posixError(globalObject);
    return jsBoolean(!value);
#endif
}

static JSValue semLockAfterFork(JSGlobalObject* globalObject, JSValue self)
{
    semLockSetCount(globalObject, PyRecord::from(self), 0);
    return jsUndefined();
}

static JSValue semLockEnter(JSGlobalObject* globalObject, JSValue self) { return semLockAcquire(globalObject, self, { }, { }); }
static JSValue semLockExit(JSGlobalObject* globalObject, JSValue self, JSValue, JSValue, JSValue) { return semLockRelease(globalObject, self); }

static JSValue semLockGetHandle(JSGlobalObject* globalObject, JSValue self) { return posixMakeInt(globalObject, static_cast<uint64_t>(PyRecord::from(self)->number)); }
static JSValue semLockGetKind(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(SemLockKindField); }
static JSValue semLockGetMaxValue(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(SemLockMaxValue); }
static JSValue semLockGetName(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(SemLockName); }

static constexpr NativeDef semLockMethods[] = {
    PY_NEW(semLockNew, "($type, /, kind, value, maxvalue, name, unlink)"),
    PY_CLASSMETHOD("_rebuild", semLockRebuild, "($type, handle, kind, maxvalue, name, /)"),
    PY_METHOD("acquire", semLockAcquire, "($self, /, block=True, timeout=None)"),
    PY_METHOD("release", semLockRelease, "($self, /)"),
    PY_METHOD("__enter__", semLockEnter, "($self, /)"),
    PY_METHOD("__exit__", semLockExit, "($self, exc_type=None, exc_value=None, exc_tb=None, /)"),
    PY_METHOD("_count", semLockGetCount, "($self, /)"),
    PY_METHOD("_is_mine", semLockIsMineMethod, "($self, /)"),
    PY_METHOD("_get_value", semLockGetValue, "($self, /)"),
    PY_METHOD("_is_zero", semLockIsZero, "($self, /)"),
    PY_METHOD("_after_fork", semLockAfterFork, "($self, /)"),
};

static constexpr NativeGetSet semLockGetSets[] = {
    PY_MEMBER("handle", semLockGetHandle),
    PY_MEMBER("kind", semLockGetKind),
    PY_MEMBER("maxvalue", semLockGetMaxValue),
    PY_MEMBER("name", semLockGetName),
};

static constexpr TypeDef semLockTypeDef { "_multiprocessing.SemLock"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable }, semLockMethods, semLockGetSets, { } };

static JSValue multiprocessingSemUnlink(JSGlobalObject* globalObject, JSValue nameValue)
{
    PosixBytes name;
    if (!semLockToName(globalObject, nameValue, "sem_unlink() argument must be str, not "_s, name))
        return { };
    if (sem_unlink(name.characters()) < 0)
        return posixError(globalObject);
    return jsUndefined();
}

static constexpr NativeDef multiprocessingFunctions[] = {
    PY_FUNCTION("sem_unlink", multiprocessingSemUnlink, "(name, /)"),
};

void initializeMultiprocessingModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    module->addFunctions(globalObject, multiprocessingFunctions);
    PyType* type = rt.typeFor(semLockTypeDef);
    JSString* limitName = internString(vm, "SEM_VALUE_MAX"_s);
    if (!type->dict()->get(limitName)) {
        int64_t limit = INT_MAX;
#ifdef SEM_VALUE_MAX
        if (SEM_VALUE_MAX >= 0)
            limit = std::min<int64_t>(limit, SEM_VALUE_MAX);
#endif
        type->dict()->set(vm, limitName, jsNumber(static_cast<int32_t>(limit)));
        type->didChange(vm);
    }
    module->add(globalObject, "SemLock"_s, type);
    PyDict* flags = PyDict::create(globalObject);
    flags->set(vm, internString(vm, "HAVE_SEM_OPEN"_s), jsNumber(1));
#if PY_HAVE_SEM_TIMEDWAIT
    flags->set(vm, internString(vm, "HAVE_SEM_TIMEDWAIT"_s), jsNumber(1));
#endif
#if PY_BROKEN_SEM_GETVALUE
    flags->set(vm, internString(vm, "HAVE_BROKEN_SEM_GETVALUE"_s), jsNumber(1));
#endif
    module->add(globalObject, "flags"_s, flags);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
