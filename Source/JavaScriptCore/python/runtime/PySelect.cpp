/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyEventWait.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include "PyHooks.h"
#include "PyPosix.h"
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <wtf/HexNumber.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// Waiting for file descriptors (PyEventWait.h), and the module select. (Modules/selectmodule.c in CPython.)

// ---- Waiting ----

static constexpr int64_t waitNanosecondsPerSecond = 1000000000;

static WaitFunction s_hostWaitFunction;

void setWaitFunction(WaitFunction function)
{
    s_hostWaitFunction = function;
}

// The host's event loop belongs to the main thread.
bool waitsInHost(JSGlobalObject* globalObject)
{
    return s_hostWaitFunction && runtime(globalObject).threads().isMainThread();
}

int64_t monotonicNanoseconds()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * waitNanosecondsPerSecond + now.tv_nsec;
}

static int waitWithTimeout(WaitRequest& request, int64_t timeout)
{
    switch (request.kind) {
    case WaitRequest::Kind::Poll: {
#if OS(LINUX)
        struct timespec time { static_cast<time_t>(timeout / waitNanosecondsPerSecond), static_cast<long>(timeout % waitNanosecondsPerSecond) };
        return ppoll(request.descriptors, request.descriptorCount, timeout < 0 ? nullptr : &time, nullptr);
#else
        int64_t milliseconds = timeout < 0 ? -1 : std::min<int64_t>(divideRoundingUp(timeout, 1000000), INT_MAX);
        return poll(request.descriptors, request.descriptorCount, static_cast<int>(milliseconds));
#endif
    }
    case WaitRequest::Kind::Select: {
        int64_t microseconds = divideRoundingUp(timeout, 1000);
        struct timeval time { static_cast<time_t>(microseconds / 1000000), static_cast<suseconds_t>(microseconds % 1000000) };
        return select(request.selectLimit, request.readable, request.writable, request.exceptional, timeout < 0 ? nullptr : &time);
    }
    case WaitRequest::Kind::Epoll: {
#if OS(LINUX)
        int64_t milliseconds = timeout < 0 ? -1 : std::min<int64_t>(divideRoundingUp(timeout, 1000000), INT_MAX);
        return epoll_wait(request.queue, request.epollEvents, request.maxEvents, static_cast<int>(milliseconds));
#else
        break;
#endif
    }
    case WaitRequest::Kind::Kqueue: {
#if PY_HAVE_KQUEUE
        struct timespec time { static_cast<time_t>(timeout / waitNanosecondsPerSecond), static_cast<long>(timeout % waitNanosecondsPerSecond) };
        int result = kevent(request.queue, request.kqueueChanges, request.kqueueChangeCount, request.kqueueEvents, request.maxEvents, timeout < 0 ? nullptr : &time);
        // Unless the call was interrupted before it did anything, the changes are made, and are not to be made again.
        if (result >= 0 || errno != EINTR) {
            request.kqueueChanges = nullptr;
            request.kqueueChangeCount = 0;
        }
        return result;
#else
        break;
#endif
    }
    }
    errno = ENOSYS;
    return -1;
}

int blockingWait(JSGlobalObject* globalObject, WaitRequest& request)
{
    AllowThreads allow(globalObject, AllowThreads::LosesNothing);
    return waitWithTimeout(request, request.timeout);
}

int waitWithoutBlocking(WaitRequest& request)
{
    if (request.kind != WaitRequest::Kind::Select)
        return waitWithTimeout(request, 0);
    // select() replaces the sets by what is ready. If nothing is, they are put back, so that the same can be asked again.
    fd_set* sets[3] = { request.readable, request.writable, request.exceptional };
    fd_set saved[3];
    for (unsigned i = 0; i < 3; ++i) {
        if (sets[i])
            saved[i] = *sets[i];
    }
    int result = waitWithTimeout(request, 0);
    if (result <= 0) {
        int error = errno;
        for (unsigned i = 0; i < 3; ++i) {
            if (sets[i])
                *sets[i] = saved[i];
        }
        errno = error;
    }
    return result;
}

static void waitClearSets(WaitRequest& request)
{
    if (request.kind != WaitRequest::Kind::Select)
        return;
    for (fd_set* set : { request.readable, request.writable, request.exceptional }) {
        if (set)
            FD_ZERO(set);
    }
}

int waitForEvents(JSGlobalObject* globalObject, WaitRequest& request)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t deadline = 0;
    if (request.timeout > 0 && __builtin_add_overflow(monotonicNanoseconds(), request.timeout, &deadline))
        deadline = std::numeric_limits<int64_t>::max();
    WaitFunction function = waitsInHost(globalObject) ? s_hostWaitFunction : blockingWait;
    IdleWait idle(globalObject, deadline, request.isWaitOfCall);
    if (idle.continuesWait()) {
        request.deadlineTakenUp = deadline;
        if (request.timeout > 0)
            request.timeout = std::max<int64_t>(deadline - monotonicNanoseconds(), 0);
    } else if (request.timeout && idle.mayBeginCallAgain()) {
        int result = waitWithoutBlocking(request);
        if (result > 0 || (result < 0 && errno != EINTR))
            return result;
        if (failed(scope, idle.willBlock()))
            return waitRaised;
    }
    while (true) {
        // A long wait is made piece by piece.
        int64_t timeout = request.timeout;
        int64_t piece = timeout ? idle.nextPiece(timeout) : 0;
        fd_set* sets[3] = { request.readable, request.writable, request.exceptional };
        fd_set saved[3];
        if (piece != timeout && request.kind == WaitRequest::Kind::Select) {
            for (unsigned i = 0; i < 3; ++i) {
                if (sets[i])
                    saved[i] = *sets[i];
            }
        }
        request.timeout = piece;
        int result;
        if (function == blockingWait)
            result = blockingWait(globalObject, request);
        else {
            Runtime::EventLoopPositionScope position(runtime(globalObject), true);
            result = function(globalObject, request);
        }
        request.timeout = timeout;
        // What the host's loop ran has raised.
        RETURN_IF_EXCEPTION(scope, waitRaised);
        if (!result && piece != timeout) {
            if (request.kind == WaitRequest::Kind::Select) {
                for (unsigned i = 0; i < 3; ++i) {
                    if (sets[i])
                        *sets[i] = saved[i];
                }
            }
            if (failed(scope, idle.didWait(timeout > 0 ? deadline - monotonicNanoseconds() : -1)))
                return waitRaised;
            if (timeout > 0)
                request.timeout = std::max<int64_t>(deadline - monotonicNanoseconds(), 0);
            continue;
        }
        if (!result)
            waitClearSets(request);
        if (result >= 0 || errno != EINTR)
            return result;
        {
            makeCallsOwed(vm);
            RETURN_IF_EXCEPTION(scope, waitRaised);
            // What that has put off would end the next wait as it ended this one.
            if (Runtime& rt = runtime(globalObject); rt.hasPendingCalls()) {
                rt.runPendingCalls();
                RETURN_IF_EXCEPTION(scope, waitRaised);
            }
        }
        if (request.timeout < 0)
            continue;
        request.timeout = request.timeout ? deadline - monotonicNanoseconds() : -1;
        if (request.timeout < 0) {
            // Time is up, and what the call leaves in the sets when it is interrupted is anybody's guess.
            waitClearSets(request);
            return 0;
        }
    }
}

bool waitUntilReady(JSGlobalObject* globalObject, int fd, bool isWaitOfCall)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct pollfd descriptor { fd, POLLIN, 0 };
    WaitRequest request { WaitRequest::Kind::Poll };
    request.timeout = -1;
    request.isWaitOfCall = isWaitOfCall;
    request.descriptors = &descriptor;
    request.descriptorCount = 1;
    RELEASE_AND_RETURN(scope, waitForEvents(globalObject, request) != waitRaised);
}

// Nothing can be read from it now. Whether read() is sure to wait until something can.
static bool readWaitsAsPollDoes(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_NONBLOCK) || (flags & O_ACCMODE) == O_WRONLY)
        return false;
    struct stat status;
    if (fstat(fd, &status) < 0)
        return false;
    if (S_ISFIFO(status.st_mode))
        return true;
    if (S_ISSOCK(status.st_mode)) {
        // read() fails at once on a socket that listens, and gives up after a time that poll() knows nothing of.
        int isListening = 0;
        socklen_t length = sizeof(isListening);
        if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &isListening, &length) < 0 || isListening)
            return false;
        struct timeval timeout { };
        length = sizeof(timeout);
        return !getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, &length) && !timeout.tv_sec && !timeout.tv_usec;
    }
    if (S_ISCHR(status.st_mode)) {
        // A terminal may be set to return nothing rather than wait, or to wait for a time only.
        struct termios settings;
        if (tcgetattr(fd, &settings) < 0)
            return false;
        return (settings.c_lflag & ICANON) || (settings.c_cc[VMIN] && !settings.c_cc[VTIME]);
    }
    return false;
}

bool waitUntilReadable(JSGlobalObject* globalObject, int fd)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct pollfd descriptor { fd, POLLIN, 0 };
    int result = poll(&descriptor, 1, 0);
    // A signal interrupts even this. Its handler is not to wait behind a read.
    if (result < 0 && errno == EINTR) {
        if (failed(scope, checkSignals(globalObject)))
            return false;
        result = 0;
    }
    // Most of the time there is something to read. What is wrong with the descriptor, the read will say.
    if (result || !readWaitsAsPollDoes(fd))
        return true;
    RELEASE_AND_RETURN(scope, waitUntilReady(globalObject, fd, true));
}

bool timeoutFromObject(JSGlobalObject* globalObject, JSValue value, int64_t unit, int64_t& nanoseconds, bool namesTimeout)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto outOfRange = [&] {
        raiseOverflowError(globalObject, scope, "timestamp out of range for C PyTime_t"_s);
        return false;
    };
    if (hasIndex(globalObject, value)) {
        JSValue index = toIndex(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        auto converted = tryInt64(index);
        if (!converted || __builtin_mul_overflow(*converted, unit, &nanoseconds))
            return outOfRange();
        return true;
    }
    double number = toDouble(globalObject, value);
    if (scope.exception()) [[unlikely]] {
        if (namesTimeout && clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            raiseTypeError(globalObject, scope, "timeout must be a real number or None, not "_s, typeNameForMessage(globalObject, rt.typeOf(value)));
        return false;
    }
    if (std::isnan(number)) {
        raiseValueError(globalObject, scope, "Invalid value NaN (not a number)"_s);
        return false;
    }
    number *= static_cast<double>(unit);
    number = number >= 0 ? std::ceil(number) : std::floor(number);
    if (!(number >= -0x1p63 && number < 0x1p63))
        return outOfRange();
    nanoseconds = static_cast<int64_t>(number);
    return true;
}

// ---- Descriptors that are closed when nobody can reach their owner ----

static JSValue descriptorGuardFinalize(JSGlobalObject*, JSValue self)
{
    auto* guard = PyRecord::from(self);
    int fd = static_cast<int>(std::exchange(guard->number, -1));
    if (fd >= 0)
        close(fd);
    return jsUndefined();
}

static constexpr NativeDef descriptorGuardMethods[] = {
    PY_SLOT("__del__", descriptorGuardFinalize, "($self, /)"),
};

static constexpr TypeDef descriptorGuardTypeDef { "select._descriptor_guard"_s, { TypeFlag::DisallowInstantiation, TypeFlag::Internal }, descriptorGuardMethods, { }, { } };

JSValue makeDescriptorGuard(JSGlobalObject* globalObject, int fd)
{
    Runtime& rt = runtime(globalObject);
    PyRecord* guard = PyRecord::create(globalObject, rt.typeFor(descriptorGuardTypeDef), 0);
    guard->number = fd;
    rt.registerFinalizable(guard);
    return guard;
}

void disarmDescriptorGuard(JSGlobalObject* globalObject, JSValue guard)
{
    if (!guard)
        return;
    PyRecord::from(guard)->number = -1;
    runtime(globalObject).unregisterFinalizable(PyRecord::from(guard));
}

// ---- Arguments ----

// None, or absent: for ever, which is -1.
static bool selectOptionalTimeout(JSGlobalObject* globalObject, JSValue value, int64_t unit, int64_t& nanoseconds)
{
    nanoseconds = -1;
    if (!value || isNone(value))
        return true;
    return timeoutFromObject(globalObject, value, unit, nanoseconds);
}

static bool selectToUnsignedShort(JSGlobalObject* globalObject, JSValue value, unsigned short& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (intSign(index) < 0) {
        raiseValueError(globalObject, scope, "Cannot convert negative int"_s);
        return false;
    }
    auto converted = tryUInt64(index);
    if (!converted || *converted > USHRT_MAX) {
        raiseOverflowError(globalObject, scope, "Python int too large for C unsigned short"_s);
        return false;
    }
    result = static_cast<unsigned short>(*converted);
    return true;
}

// The items of a list or a tuple, or of whatever else can be iterated over. (PySequence_Fast)
static bool selectItems(JSGlobalObject* globalObject, JSValue iterable, ASCIILiteral message, MarkedArgumentBuffer& items)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* tuple = PyTuple::tryFrom(iterable)) {
        for (unsigned i = 0; i < tuple->length(); ++i)
            items.append(tuple->at(i));
        return true;
    }
    JSArray* list = tryAsList(iterable);
    if (!list) {
        JSValue iterator = getIter(globalObject, iterable);
        if (scope.exception()) [[unlikely]] {
            if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                raiseTypeError(globalObject, scope, message);
            return false;
        }
        list = listFromIterable(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
    }
    for (unsigned i = 0; i < listLength(list); ++i)
        items.append(listGet(list, i));
    return true;
}

// ---- select() ----

struct SelectSet {
    fd_set set;
    Vector<int, 16> descriptors;
    MarkedArgumentBuffer objects;
    int limit { 0 };

    bool fill(JSGlobalObject* globalObject, JSValue iterable)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        FD_ZERO(&set);
        // A list is looked at as it is when each item's turn comes: a fileno() may change it.
        JSArray* list = tryAsList(iterable);
        if (!list && failed(scope, selectItems(globalObject, iterable, "arguments 1-3 must be sequences"_s, objects)))
            return false;
        for (unsigned i = 0; i < (list ? listLength(list) : objects.size()); ++i) {
            if (list)
                objects.append(listGet(list, i));
            int fd;
            if (failed(scope, posixToFileDescriptor(globalObject, objects.at(i), fd)))
                return false;
            if (fd < 0 || fd >= FD_SETSIZE) {
                raiseValueError(globalObject, scope, "filedescriptor out of range in select()"_s);
                return false;
            }
            if (i >= FD_SETSIZE) {
                raiseValueError(globalObject, scope, "too many file descriptors in select()"_s);
                return false;
            }
            limit = std::max(limit, fd + 1);
            FD_SET(fd, &set);
            descriptors.append(fd);
        }
        return true;
    }

    JSValue ready(JSGlobalObject* globalObject)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSArray* list = newList(globalObject);
        for (unsigned i = 0; i < descriptors.size(); ++i) {
            if (FD_ISSET(descriptors[i], &set)) {
                listAppend(globalObject, list, objects.at(i));
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        return list;
    }
};

static JSValue selectSelect(JSGlobalObject* globalObject, JSValue readList, JSValue writeList, JSValue exceptList, JSValue timeoutValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t timeout;
    if (failed(scope, selectOptionalTimeout(globalObject, timeoutValue, waitNanosecondsPerSecond, timeout)))
        return { };
    if (timeoutValue && !isNone(timeoutValue) && timeout < 0)
        return raiseValueError(globalObject, scope, "timeout must be non-negative"_s);

    SelectSet readable;
    SelectSet writable;
    SelectSet exceptional;
    if (failed(scope, readable.fill(globalObject, readList)) || failed(scope, writable.fill(globalObject, writeList)) || failed(scope, exceptional.fill(globalObject, exceptList)))
        return { };

    WaitRequest request { WaitRequest::Kind::Select };
    request.timeout = timeout;
    request.selectLimit = std::max({ readable.limit, writable.limit, exceptional.limit });
    request.readable = readable.limit ? &readable.set : nullptr;
    request.writable = writable.limit ? &writable.set : nullptr;
    request.exceptional = exceptional.limit ? &exceptional.set : nullptr;
    int result = waitForEvents(globalObject, request);
    if (failed(scope, result != waitRaised))
        return { };
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));

    JSValue first = readable.ready(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue second = writable.ready(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue third = exceptional.ready(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { first, second, third }));
}

// ---- poll ---- (field 0: {fd: events}; number: whether poll() is running)

static JSValue pollRegister(JSGlobalObject* globalObject, JSValue self, JSValue fdValue, JSValue maskValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    unsigned short mask = POLLIN | POLLPRI | POLLOUT;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)) || (maskValue && failed(scope, selectToUnsignedShort(globalObject, maskValue, mask))))
        return { };
    scope.release();
    PyDict::from(PyRecord::from(self)->get(0))->set(globalObject, jsNumber(fd), jsNumber(mask));
    return jsUndefined();
}

static JSValue pollModify(JSGlobalObject* globalObject, JSValue self, JSValue fdValue, JSValue maskValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    unsigned short mask;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)) || failed(scope, selectToUnsignedShort(globalObject, maskValue, mask)))
        return { };
    PyDict* dict = PyDict::from(PyRecord::from(self)->get(0));
    bool isRegistered = dict->contains(globalObject, jsNumber(fd));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isRegistered)
        RELEASE_AND_RETURN(scope, posixError(globalObject, ENOENT));
    scope.release();
    dict->set(globalObject, jsNumber(fd), jsNumber(mask));
    return jsUndefined();
}

static JSValue pollUnregister(JSGlobalObject* globalObject, JSValue self, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    bool removed = PyDict::from(PyRecord::from(self)->get(0))->remove(globalObject, jsNumber(fd));
    RETURN_IF_EXCEPTION(scope, { });
    if (!removed)
        return raiseKeyError(globalObject, scope, jsNumber(fd));
    return jsUndefined();
}

static JSValue pollPoll(JSGlobalObject* globalObject, JSValue self, JSValue timeoutValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PyRecord::from(self);
    int64_t timeout;
    if (failed(scope, selectOptionalTimeout(globalObject, timeoutValue, 1000000, timeout)))
        return { };
    if (object->number)
        return raiseRuntimeError(globalObject, scope, "concurrent poll() invocation"_s);

    Vector<struct pollfd, 16> descriptors;
    {
        PyDict* dict = PyDict::from(object->get(0));
        JSValue key;
        JSValue value;
        for (unsigned position = 0; dict->next(position, key, value);)
            descriptors.append({ key.asInt32(), static_cast<short>(static_cast<unsigned short>(value.asInt32())), 0 });
    }

    WaitRequest request { WaitRequest::Kind::Poll };
    request.timeout = std::max<int64_t>(timeout, -1);
    request.isWaitOfCall = true;
    request.descriptors = descriptors.mutableSpan().data();
    request.descriptorCount = descriptors.size();
    object->number = 1;
    int result = waitForEvents(globalObject, request);
    object->number = 0;
    if (failed(scope, result != waitRaised))
        return { };
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));

    JSArray* list = newList(globalObject);
    for (auto& descriptor : descriptors) {
        if (!descriptor.revents)
            continue;
        listAppend(globalObject, list, PyTuple::create(globalObject, { jsNumber(descriptor.fd), jsNumber(descriptor.revents & 0xffff) }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return list;
}

static constexpr NativeDef pollMethods[] = {
    PY_METHOD("register", pollRegister, "($self, fd, eventmask=select.POLLIN | select.POLLPRI | select.POLLOUT, /)"),
    PY_METHOD("modify", pollModify, "($self, fd, eventmask, /)"),
    PY_METHOD("unregister", pollUnregister, "($self, fd, /)"),
    PY_WAITING_METHOD("poll", pollPoll, "($self, timeout=None, /)"),
};

static constexpr TypeDef pollTypeDef { "select.poll"_s, { TypeFlag::DisallowInstantiation }, pollMethods, { }, { } };

static JSValue selectPoll(JSGlobalObject* globalObject)
{
    PyRecord* object = PyRecord::create(globalObject, runtime(globalObject).typeFor(pollTypeDef), 1);
    object->set(getVM(globalObject), 0, PyDict::create(globalObject));
    return object;
}

// ---- epoll and kqueue: what they share ---- (number: the descriptor, or -1; field 0: its guard)

static JSValue queueCreate(JSGlobalObject* globalObject, JSValue type, int fd)
{
    if (fd < 0)
        return posixError(globalObject);
    PyRecord* object = PyRecord::create(globalObject, PyType::from(type), 1);
    object->number = fd;
    object->set(getVM(globalObject), 0, makeDescriptorGuard(globalObject, fd));
    return object;
}

static JSValue queueClose(JSGlobalObject* globalObject, JSValue self)
{
    auto* object = PyRecord::from(self);
    int fd = static_cast<int>(std::exchange(object->number, -1));
    if (fd < 0)
        return jsUndefined();
    disarmDescriptorGuard(globalObject, object->get(0));
    if (close(fd) < 0)
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue queueGetClosed(JSGlobalObject*, JSValue self) { return jsBoolean(PyRecord::from(self)->number < 0); }

static JSValue queueExit(JSGlobalObject* globalObject, JSValue self, JSValue, JSValue, JSValue)
{
    return callMethod(globalObject, self, internString(getVM(globalObject), "close"_s));
}

static constexpr NativeGetSet queueGetSets[] = {
    { "closed"_s, queueGetClosed },
};

#if OS(LINUX)

// ---- epoll ----

static JSValue epollClosedError(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseValueError(globalObject, scope, "I/O operation on closed epoll object"_s);
}

static JSValue epollNew(JSGlobalObject* globalObject, JSValue type, JSValue sizeHintValue, JSValue flagsValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int sizeHint = -1;
    int flags = 0;
    if ((sizeHintValue && failed(scope, posixToInt(globalObject, sizeHintValue, sizeHint))) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags))))
        return { };
    if (sizeHint != -1 && sizeHint <= 0)
        return raiseValueError(globalObject, scope, "negative sizehint"_s);
    if (flags && flags != EPOLL_CLOEXEC)
        return raise(globalObject, scope, rt.OSError(), "invalid flags"_s);
    RELEASE_AND_RETURN(scope, queueCreate(globalObject, type, epoll_create1(EPOLL_CLOEXEC)));
}

static JSValue epollFromFd(JSGlobalObject* globalObject, JSValue type, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    if (fd == -1)
        fd = epoll_create1(EPOLL_CLOEXEC);
    RELEASE_AND_RETURN(scope, queueCreate(globalObject, type, fd));
}

static JSValue epollFileno(JSGlobalObject* globalObject, JSValue self)
{
    int64_t fd = PyRecord::from(self)->number;
    if (fd < 0)
        return epollClosedError(globalObject);
    return jsNumber(static_cast<int>(fd));
}

static JSValue epollControl(JSGlobalObject* globalObject, JSValue self, int operation, JSValue fdValue, JSValue maskValue, unsigned mask)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)) || (maskValue && failed(scope, posixToUnsignedMask(globalObject, maskValue, mask))))
        return { };
    int64_t queue = PyRecord::from(self)->number;
    if (queue < 0)
        RELEASE_AND_RETURN(scope, epollClosedError(globalObject));
    struct epoll_event event { };
    event.events = mask;
    event.data.fd = fd;
    if (epoll_ctl(static_cast<int>(queue), operation, fd, &event) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue epollRegister(JSGlobalObject* globalObject, JSValue self, JSValue fd, JSValue mask) { return epollControl(globalObject, self, EPOLL_CTL_ADD, fd, mask, EPOLLIN | EPOLLPRI | EPOLLOUT); }
static JSValue epollModify(JSGlobalObject* globalObject, JSValue self, JSValue fd, JSValue mask) { return epollControl(globalObject, self, EPOLL_CTL_MOD, fd, mask, 0); }
static JSValue epollUnregister(JSGlobalObject* globalObject, JSValue self, JSValue fd) { return epollControl(globalObject, self, EPOLL_CTL_DEL, fd, { }, 0); }

static JSValue epollPoll(JSGlobalObject* globalObject, JSValue self, JSValue timeoutValue, JSValue maxEventsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int maxEvents = -1;
    if (maxEventsValue && failed(scope, posixToInt(globalObject, maxEventsValue, maxEvents)))
        return { };
    auto* object = PyRecord::from(self);
    if (object->number < 0)
        RELEASE_AND_RETURN(scope, epollClosedError(globalObject));
    int64_t timeout;
    if (failed(scope, selectOptionalTimeout(globalObject, timeoutValue, waitNanosecondsPerSecond, timeout)))
        return { };
    int64_t milliseconds = divideRoundingUp(timeout, 1000000);
    if (milliseconds < INT_MIN || milliseconds > INT_MAX)
        return raiseOverflowError(globalObject, scope, "timeout is too large"_s);
    if (maxEvents == -1)
        maxEvents = FD_SETSIZE - 1;
    else if (maxEvents < 1)
        return raiseValueError(globalObject, scope, "maxevents must be greater than 0, got "_s, maxEvents);

    Vector<struct epoll_event> events;
    if (!events.tryGrow(maxEvents))
        return raise(globalObject, scope, runtime(globalObject).MemoryError(), String());

    WaitRequest request { WaitRequest::Kind::Epoll };
    request.timeout = std::max<int64_t>(timeout, -1);
    request.isWaitOfCall = true;
    request.queue = static_cast<int>(object->number);
    request.epollEvents = events.mutableSpan().data();
    request.maxEvents = maxEvents;
    int result = waitForEvents(globalObject, request);
    if (failed(scope, result != waitRaised))
        return { };
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));

    JSArray* list = newList(globalObject);
    for (int i = 0; i < result; ++i) {
        listAppend(globalObject, list, PyTuple::create(globalObject, { jsNumber(events[i].data.fd), jsNumber(events[i].events) }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return list;
}

static JSValue epollEnter(JSGlobalObject* globalObject, JSValue self)
{
    if (PyRecord::from(self)->number < 0)
        return epollClosedError(globalObject);
    return self;
}

static constexpr NativeDef epollMethods[] = {
    PY_NEW(epollNew, "($type, /, sizehint=-1, flags=0)"),
    PY_CLASSMETHOD("fromfd", epollFromFd, "($type, fd, /)"),
    PY_METHOD("close", queueClose, "($self, /)"),
    PY_METHOD("fileno", epollFileno, "($self, /)"),
    PY_METHOD("register", epollRegister, "($self, /, fd, eventmask=select.EPOLLIN | select.EPOLLPRI | select.EPOLLOUT)"),
    PY_METHOD("modify", epollModify, "($self, /, fd, eventmask)"),
    PY_METHOD("unregister", epollUnregister, "($self, /, fd)"),
    PY_WAITING_METHOD("poll", epollPoll, "($self, /, timeout=None, maxevents=-1)"),
    PY_METHOD("__enter__", epollEnter, "($self, /)"),
    PY_METHOD("__exit__", queueExit, "($self, exc_type=None, exc_value=None, exc_tb=None, /)"),
};

static constexpr TypeDef epollTypeDef { "select.epoll"_s, { }, epollMethods, queueGetSets, { } };

#endif // OS(LINUX)

#if PY_HAVE_KQUEUE

// ---- kevent ---- (the fields of the struct, as ints)

enum KeventField : unsigned { KeventIdent, KeventFilter, KeventFlags, KeventFFlags, KeventData, KeventUData, KeventFieldCount };

static PyType* keventType(JSGlobalObject*);

static JSValue keventFromStruct(JSGlobalObject* globalObject, const struct kevent& event)
{
    VM& vm = getVM(globalObject);
    PyRecord* object = PyRecord::create(globalObject, keventType(globalObject), KeventFieldCount);
    object->set(vm, KeventIdent, makeInt(globalObject, static_cast<uint64_t>(event.ident)));
    object->set(vm, KeventFilter, makeInt(globalObject, static_cast<int64_t>(event.filter)));
    object->set(vm, KeventFlags, makeInt(globalObject, static_cast<uint64_t>(event.flags)));
    object->set(vm, KeventFFlags, makeInt(globalObject, static_cast<uint64_t>(event.fflags)));
    object->set(vm, KeventData, makeInt(globalObject, static_cast<int64_t>(event.data)));
    object->set(vm, KeventUData, makeInt(globalObject, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(event.udata))));
    return object;
}

static struct kevent keventToStruct(PyRecord* object)
{
    struct kevent event { };
    event.ident = static_cast<decltype(event.ident)>(tryUInt64(object->get(KeventIdent)).value_or(0));
    event.filter = static_cast<decltype(event.filter)>(tryInt64(object->get(KeventFilter)).value_or(0));
    event.flags = static_cast<decltype(event.flags)>(tryUInt64(object->get(KeventFlags)).value_or(0));
    event.fflags = static_cast<decltype(event.fflags)>(tryUInt64(object->get(KeventFFlags)).value_or(0));
    event.data = static_cast<decltype(event.data)>(tryInt64(object->get(KeventData)).value_or(0));
    event.udata = reinterpret_cast<decltype(event.udata)>(static_cast<uintptr_t>(tryUInt64(object->get(KeventUData)).value_or(0)));
    return event;
}

static JSValue keventNew(JSGlobalObject* globalObject, JSValue type, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    PyRecord* object = PyRecord::create(globalObject, PyType::from(type), KeventFieldCount);
    for (unsigned i = 0; i < KeventFieldCount; ++i)
        object->set(vm, i, jsNumber(0));
    return object;
}

// An int that fits the field. What does not fit is an OverflowError.
template<typename Integer>
static bool keventToField(JSGlobalObject* globalObject, JSValue value, JSValue& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    bool fits;
    if constexpr (std::is_signed_v<Integer>) {
        auto converted = tryInt64(index);
        fits = converted && *converted >= std::numeric_limits<Integer>::min() && *converted <= std::numeric_limits<Integer>::max();
    } else {
        auto converted = tryUInt64(index);
        fits = converted && *converted <= std::numeric_limits<Integer>::max();
    }
    if (!fits) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to a field of kevent"_s);
        return false;
    }
    result = index;
    return true;
}

static JSValue keventInit(JSGlobalObject* globalObject, JSValue self, JSValue ident, JSValue filter, JSValue flags, JSValue fflags, JSValue data, JSValue udata)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct kevent layout;
    JSValue values[KeventFieldCount] = { jsNumber(0), jsNumber(EVFILT_READ), jsNumber(EV_ADD), jsNumber(0), jsNumber(0), jsNumber(0) };
    if ((filter && !keventToField<decltype(layout.filter)>(globalObject, filter, values[KeventFilter]))
        || (flags && !keventToField<decltype(layout.flags)>(globalObject, flags, values[KeventFlags]))
        || (fflags && !keventToField<decltype(layout.fflags)>(globalObject, fflags, values[KeventFFlags]))
        || (data && !keventToField<decltype(layout.data)>(globalObject, data, values[KeventData]))
        || (udata && failed(scope, keventToField<uintptr_t>(globalObject, udata, values[KeventUData]))))
        return { };
    if (hasIndex(globalObject, ident)) {
        JSValue index = toIndex(globalObject, ident);
        RETURN_IF_EXCEPTION(scope, { });
        if (intSign(index) < 0)
            return raiseValueError(globalObject, scope, "Cannot convert negative int"_s);
        if (!tryUInt64(index))
            return raiseOverflowError(globalObject, scope, "Python int too large for C kqueue event identifier"_s);
        values[KeventIdent] = index;
    } else {
        int fd;
        if (failed(scope, posixToFileDescriptor(globalObject, ident, fd)))
            return { };
        values[KeventIdent] = jsNumber(fd);
    }
    auto* object = PyRecord::from(self);
    for (unsigned i = 0; i < KeventFieldCount; ++i)
        object->set(vm, i, values[i]);
    return jsUndefined();
}

static JSValue keventRepr(JSGlobalObject* globalObject, JSValue self)
{
    struct kevent event = keventToStruct(PyRecord::from(self));
    return makeStr(globalObject, makeString("<select.kevent ident="_s, static_cast<uint64_t>(event.ident), " filter="_s, static_cast<int>(event.filter),
        " flags=0x"_s, hex(static_cast<unsigned>(event.flags), Lowercase), " fflags=0x"_s, hex(static_cast<unsigned>(event.fflags), Lowercase),
        " data=0x"_s, hex(static_cast<uint64_t>(event.data), Lowercase), " udata=0x"_s, hex(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(event.udata)), Lowercase), '>'));
}

template<CompareOperator op>
static JSValue keventCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto* right = PyRecord::tryFrom(other, keventType(globalObject));
    if (!right)
        return runtime(globalObject).notImplemented();
    struct kevent a = keventToStruct(PyRecord::from(self));
    struct kevent b = keventToStruct(right);
    auto compare = [](auto x, auto y) { return x < y ? -1 : x > y ? 1 : 0; };
    int order = compare(a.ident, b.ident);
    if (!order)
        order = compare(a.filter, b.filter);
    if (!order)
        order = compare(a.flags, b.flags);
    if (!order)
        order = compare(a.fflags, b.fflags);
    if (!order)
        order = compare(a.data, b.data);
    if (!order)
        order = compare(reinterpret_cast<intptr_t>(a.udata), reinterpret_cast<intptr_t>(b.udata));
    switch (op) {
    case CompareOperator::Eq:
        return jsBoolean(!order);
    case CompareOperator::NotEq:
        return jsBoolean(!!order);
    case CompareOperator::Lt:
        return jsBoolean(order < 0);
    case CompareOperator::LtE:
        return jsBoolean(order <= 0);
    case CompareOperator::Gt:
        return jsBoolean(order > 0);
    default:
        return jsBoolean(order >= 0);
    }
}

template<unsigned field>
static JSValue keventGet(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(field); }

template<unsigned field, typename Integer>
static void keventSet(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete numeric/char attribute"_s);
        return;
    }
    JSValue converted;
    if (keventToField<Integer>(globalObject, value, converted))
        PyRecord::from(self)->set(vm, field, converted);
}

static constexpr NativeDef keventMethods[] = {
    PY_NEW(keventNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", keventInit, "($self, /, ident, filter=KQ_FILTER_READ, flags=KQ_EV_ADD, fflags=0, data=0, udata=0)"),
    PY_SLOT("__repr__", keventRepr, "($self, /)"),
    PY_SLOT("__eq__", keventCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", keventCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", keventCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", keventCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", keventCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", keventCompare<CompareOperator::GtE>, "($self, value, /)"),
};

using KeventLayout = struct kevent;
static constexpr NativeGetSet keventGetSets[] = {
    PY_WRITABLE_MEMBER("ident", keventGet<KeventIdent>, (keventSet<KeventIdent, uintptr_t>)),
    PY_WRITABLE_MEMBER("filter", keventGet<KeventFilter>, (keventSet<KeventFilter, decltype(KeventLayout::filter)>)),
    PY_WRITABLE_MEMBER("flags", keventGet<KeventFlags>, (keventSet<KeventFlags, decltype(KeventLayout::flags)>)),
    PY_WRITABLE_MEMBER("fflags", keventGet<KeventFFlags>, (keventSet<KeventFFlags, decltype(KeventLayout::fflags)>)),
    PY_WRITABLE_MEMBER("data", keventGet<KeventData>, (keventSet<KeventData, decltype(KeventLayout::data)>)),
    PY_WRITABLE_MEMBER("udata", keventGet<KeventUData>, (keventSet<KeventUData, uintptr_t>)),
};

static constexpr TypeDef keventTypeDef { "select.kevent"_s, { }, keventMethods, keventGetSets, { } };

static PyType* keventType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyType* type = rt.typeFor(keventTypeDef);
    // It defines __eq__, and so is not hashable.
    if (!type->dict()->get(rt.name(Special::Hash))) {
        type->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
        type->didChange(vm);
    }
    return type;
}

// ---- kqueue ----

static JSValue kqueueClosedError(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseValueError(globalObject, scope, "I/O operation on closed kqueue object"_s);
}

static JSValue kqueueCreate(JSGlobalObject* globalObject, JSValue type)
{
    int fd = kqueue();
    if (fd >= 0 && !posixSetInheritable(fd, false)) {
        int saved = errno;
        close(fd);
        return posixError(globalObject, saved);
    }
    return queueCreate(globalObject, type, fd);
}

static JSValue kqueueNew(JSGlobalObject* globalObject, JSValue type) { return kqueueCreate(globalObject, type); }

static JSValue kqueueFromFd(JSGlobalObject* globalObject, JSValue type, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    if (fd == -1)
        RELEASE_AND_RETURN(scope, kqueueCreate(globalObject, type));
    RELEASE_AND_RETURN(scope, queueCreate(globalObject, type, fd));
}

static JSValue kqueueFileno(JSGlobalObject* globalObject, JSValue self)
{
    int64_t fd = PyRecord::from(self)->number;
    if (fd < 0)
        return kqueueClosedError(globalObject);
    return jsNumber(static_cast<int>(fd));
}

static JSValue kqueueControl(JSGlobalObject* globalObject, JSValue self, JSValue changeList, JSValue maxEventsValue, JSValue timeoutValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int maxEvents;
    if (failed(scope, posixToInt(globalObject, maxEventsValue, maxEvents)))
        return { };
    auto* object = PyRecord::from(self);
    if (object->number < 0)
        RELEASE_AND_RETURN(scope, kqueueClosedError(globalObject));
    if (maxEvents < 0)
        return raiseValueError(globalObject, scope, "Length of eventlist must be 0 or positive, got "_s, maxEvents);
    int64_t timeout;
    if (failed(scope, selectOptionalTimeout(globalObject, timeoutValue, waitNanosecondsPerSecond, timeout)))
        return { };
    if (timeoutValue && !isNone(timeoutValue) && timeout < 0)
        return raiseValueError(globalObject, scope, "timeout must be positive or None"_s);

    Vector<struct kevent, 8> changes;
    if (!isNone(changeList)) {
        MarkedArgumentBuffer items;
        if (failed(scope, selectItems(globalObject, changeList, "changelist is not iterable"_s, items)))
            return { };
        if (items.size() > INT_MAX)
            return raiseOverflowError(globalObject, scope, "changelist is too long"_s);
        PyType* type = keventType(globalObject);
        for (unsigned i = 0; i < items.size(); ++i) {
            auto* event = PyRecord::tryFrom(items.at(i), type);
            if (!event)
                return raiseTypeError(globalObject, scope, "changelist must be an iterable of select.kevent objects"_s);
            changes.append(keventToStruct(event));
        }
    }
    Vector<struct kevent, 8> events;
    if (!events.tryGrow(maxEvents))
        return raise(globalObject, scope, runtime(globalObject).MemoryError(), String());

    WaitRequest request { WaitRequest::Kind::Kqueue };
    request.timeout = timeout;
    request.queue = static_cast<int>(object->number);
    request.kqueueChanges = changes.span().data();
    request.kqueueChangeCount = static_cast<int>(changes.size());
    request.kqueueEvents = events.mutableSpan().data();
    request.maxEvents = maxEvents;
    int result = waitForEvents(globalObject, request);
    if (failed(scope, result != waitRaised))
        return { };
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));

    JSArray* list = newList(globalObject);
    for (int i = 0; i < result; ++i) {
        listAppend(globalObject, list, keventFromStruct(globalObject, events[i]));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return list;
}

static constexpr NativeDef kqueueMethods[] = {
    PY_NEW(kqueueNew, "($type, /)"),
    PY_CLASSMETHOD("fromfd", kqueueFromFd, "($type, fd, /)"),
    PY_METHOD("close", queueClose, "($self, /)"),
    PY_METHOD("fileno", kqueueFileno, "($self, /)"),
    PY_METHOD("control", kqueueControl, "($self, changelist, maxevents, timeout=None, /)"),
};

static constexpr TypeDef kqueueTypeDef { "select.kqueue"_s, { }, kqueueMethods, queueGetSets, { } };

#endif // PY_HAVE_KQUEUE

// ---- The module ----

static constexpr NativeDef selectFunctions[] = {
    NativeDef(NativeKind::Function, "select"_s, selectSelect, "(rlist, wlist, xlist, timeout=None, /)"_s, MayWait),
    PY_FUNCTION("poll", selectPoll, "()"),
};

#define SELECT_CONSTANT(name) { #name ""_s, static_cast<int64_t>(static_cast<uint32_t>(name)) }
#define SELECT_KQ_CONSTANT(name, value) { "KQ_" #name ""_s, static_cast<int64_t>(value) }

static constexpr PosixConstant selectConstants[] = {
#ifdef PIPE_BUF
    SELECT_CONSTANT(PIPE_BUF),
#endif
    SELECT_CONSTANT(POLLIN),
    SELECT_CONSTANT(POLLPRI),
    SELECT_CONSTANT(POLLOUT),
    SELECT_CONSTANT(POLLERR),
    SELECT_CONSTANT(POLLHUP),
    SELECT_CONSTANT(POLLNVAL),
#ifdef POLLRDNORM
    SELECT_CONSTANT(POLLRDNORM),
#endif
#ifdef POLLRDBAND
    SELECT_CONSTANT(POLLRDBAND),
#endif
#ifdef POLLWRNORM
    SELECT_CONSTANT(POLLWRNORM),
#endif
#ifdef POLLWRBAND
    SELECT_CONSTANT(POLLWRBAND),
#endif
#ifdef POLLMSG
    SELECT_CONSTANT(POLLMSG),
#endif
#ifdef POLLRDHUP
    SELECT_CONSTANT(POLLRDHUP),
#endif
#if OS(LINUX)
    SELECT_CONSTANT(EPOLLIN),
    SELECT_CONSTANT(EPOLLOUT),
    SELECT_CONSTANT(EPOLLPRI),
    SELECT_CONSTANT(EPOLLERR),
    SELECT_CONSTANT(EPOLLHUP),
    SELECT_CONSTANT(EPOLLRDHUP),
    SELECT_CONSTANT(EPOLLET),
    SELECT_CONSTANT(EPOLLONESHOT),
    SELECT_CONSTANT(EPOLLEXCLUSIVE),
    SELECT_CONSTANT(EPOLLWAKEUP),
    SELECT_CONSTANT(EPOLLRDNORM),
    SELECT_CONSTANT(EPOLLRDBAND),
    SELECT_CONSTANT(EPOLLWRNORM),
    SELECT_CONSTANT(EPOLLWRBAND),
    SELECT_CONSTANT(EPOLLMSG),
    SELECT_CONSTANT(EPOLL_CLOEXEC),
#endif
#if PY_HAVE_KQUEUE
    SELECT_KQ_CONSTANT(FILTER_READ, EVFILT_READ),
    SELECT_KQ_CONSTANT(FILTER_WRITE, EVFILT_WRITE),
#ifdef EVFILT_AIO
    SELECT_KQ_CONSTANT(FILTER_AIO, EVFILT_AIO),
#endif
#ifdef EVFILT_VNODE
    SELECT_KQ_CONSTANT(FILTER_VNODE, EVFILT_VNODE),
#endif
#ifdef EVFILT_PROC
    SELECT_KQ_CONSTANT(FILTER_PROC, EVFILT_PROC),
#endif
#ifdef EVFILT_NETDEV
    SELECT_KQ_CONSTANT(FILTER_NETDEV, EVFILT_NETDEV),
#endif
#ifdef EVFILT_SIGNAL
    SELECT_KQ_CONSTANT(FILTER_SIGNAL, EVFILT_SIGNAL),
#endif
    SELECT_KQ_CONSTANT(FILTER_TIMER, EVFILT_TIMER),
    SELECT_KQ_CONSTANT(EV_ADD, EV_ADD),
    SELECT_KQ_CONSTANT(EV_DELETE, EV_DELETE),
    SELECT_KQ_CONSTANT(EV_ENABLE, EV_ENABLE),
    SELECT_KQ_CONSTANT(EV_DISABLE, EV_DISABLE),
    SELECT_KQ_CONSTANT(EV_ONESHOT, EV_ONESHOT),
    SELECT_KQ_CONSTANT(EV_CLEAR, EV_CLEAR),
#ifdef EV_SYSFLAGS
    SELECT_KQ_CONSTANT(EV_SYSFLAGS, EV_SYSFLAGS),
#endif
#ifdef EV_FLAG1
    SELECT_KQ_CONSTANT(EV_FLAG1, EV_FLAG1),
#endif
    SELECT_KQ_CONSTANT(EV_EOF, EV_EOF),
    SELECT_KQ_CONSTANT(EV_ERROR, EV_ERROR),
#ifdef NOTE_LOWAT
    SELECT_KQ_CONSTANT(NOTE_LOWAT, NOTE_LOWAT),
#endif
#ifdef EVFILT_VNODE
    SELECT_KQ_CONSTANT(NOTE_DELETE, NOTE_DELETE),
    SELECT_KQ_CONSTANT(NOTE_WRITE, NOTE_WRITE),
    SELECT_KQ_CONSTANT(NOTE_EXTEND, NOTE_EXTEND),
    SELECT_KQ_CONSTANT(NOTE_ATTRIB, NOTE_ATTRIB),
    SELECT_KQ_CONSTANT(NOTE_LINK, NOTE_LINK),
    SELECT_KQ_CONSTANT(NOTE_RENAME, NOTE_RENAME),
    SELECT_KQ_CONSTANT(NOTE_REVOKE, NOTE_REVOKE),
#endif
#ifdef EVFILT_PROC
    SELECT_KQ_CONSTANT(NOTE_EXIT, static_cast<uint32_t>(NOTE_EXIT)),
    SELECT_KQ_CONSTANT(NOTE_FORK, NOTE_FORK),
    SELECT_KQ_CONSTANT(NOTE_EXEC, NOTE_EXEC),
    SELECT_KQ_CONSTANT(NOTE_PCTRLMASK, static_cast<uint32_t>(NOTE_PCTRLMASK)),
    SELECT_KQ_CONSTANT(NOTE_PDATAMASK, NOTE_PDATAMASK),
    SELECT_KQ_CONSTANT(NOTE_TRACK, NOTE_TRACK),
    SELECT_KQ_CONSTANT(NOTE_CHILD, NOTE_CHILD),
    SELECT_KQ_CONSTANT(NOTE_TRACKERR, NOTE_TRACKERR),
#endif
#ifdef EVFILT_NETDEV
    SELECT_KQ_CONSTANT(NOTE_LINKUP, NOTE_LINKUP),
    SELECT_KQ_CONSTANT(NOTE_LINKDOWN, NOTE_LINKDOWN),
    SELECT_KQ_CONSTANT(NOTE_LINKINV, NOTE_LINKINV),
#endif
#endif // PY_HAVE_KQUEUE
};

void initializeSelectModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->addFunctions(globalObject, selectFunctions);
    module->add(globalObject, "error"_s, rt.OSError());
    addPosixConstants(globalObject, module, selectConstants);
#if OS(LINUX)
    module->add(globalObject, "epoll"_s, rt.typeFor(epollTypeDef));
#endif
#if PY_HAVE_KQUEUE
    module->add(globalObject, "kevent"_s, keventType(globalObject));
    module->add(globalObject, "kqueue"_s, rt.typeFor(kqueueTypeDef));
#endif
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
