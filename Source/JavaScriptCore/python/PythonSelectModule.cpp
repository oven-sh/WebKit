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
#include "PythonPosixModule.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonAsyncio.h"
#include "PythonBuiltins.h"
#include "PythonIO.h"
#include "PythonOperations.h"
#include "PythonOperators.h"
#include "PythonPosix.h"
#include "PythonSequences.h"
#include "PythonStructMember.h"
#include "PythonTime.h"
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/select.h>
#include <unistd.h>

#if OS(DARWIN) || OS(FREEBSD)
#define PYTHON_HAVE_KQUEUE 1
#include <sys/event.h>
#endif

// The module select: Modules/selectmodule.c of CPython.
//
// FIXME: epoll, which is Linux's, is not written. `selectors` makes do with poll().

namespace JSC { namespace Python {

namespace {

struct SelectModuleState final : NativeState {
    PYTHON_NATIVE_STATE(SelectModuleState);
    WriteBarrier<PyType> poll;
    WriteBarrier<PyType> kevent;
    WriteBarrier<PyType> kqueue;
};

template<typename Visitor>
void SelectModuleState::visit(Visitor& visitor)
{
    visitor.append(poll);
    visitor.append(kevent);
    visitor.append(kqueue);
}

SelectModuleState& selectState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<SelectModuleState>(); }

// PySequence_Fast(): a list or a tuple as it is, or a list of what is in anything else that can be gone through. So a list that is given is the one that is looked in, whatever becomes of it meanwhile.
// Empty if it raised.
JSValue toFastSequence(JSGlobalObject* globalObject, JSValue given, ASCIILiteral ifNotIterable)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyRealm* realm = globalObject->pyRealm();
    if (isTuple(given) || isInstance(globalObject, given, realm->typeList()))
        return given;
    JSValue iterator = getIterator(globalObject, given);
    if (scope.exception()) [[unlikely]] {
        if (catchException(globalObject, BuiltinType::TypeError))
            raiseTypeError(globalObject, scope, ifNotIterable);
        return { };
    }
    RELEASE_AND_RETURN(scope, call(globalObject, realm->typeList(), iterator));
}

unsigned fastSequenceLength(JSValue sequence) { return isTuple(sequence) ? asTuple(sequence)->length() : asList(sequence)->length(); }
JSValue fastSequenceAt(JSValue sequence, unsigned i) { return isTuple(sequence) ? asTuple(sequence)->at(i) : asList(sequence)->getIndexQuickly(i); }

// ---- select()

// What is waited for, each with its descriptor
struct Waited {
    fd_set set;
    MarkedArgumentBuffer objects;
    Vector<int, 16> descriptors;
    int limit { 0 }; // One more than the largest

    // seq2set(). False if it raised.
    bool fill(JSGlobalObject* globalObject, JSValue sequence)
    {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        FD_ZERO(&set);
        JSValue items = toFastSequence(globalObject, sequence, "arguments 1-3 must be sequences"_s);
        RETURN_IF_EXCEPTION(scope, false);
        for (unsigned i = 0; i < fastSequenceLength(items); ++i) {
            JSValue item = fastSequenceAt(items, i);
            auto descriptor = toFileDescriptorOrFile(globalObject, item);
            RETURN_IF_EXCEPTION(scope, false);
            // _PyIsSelectable_fd()
            if (*descriptor < 0 || *descriptor >= FD_SETSIZE) {
                raiseValueError(globalObject, scope, "filedescriptor out of range in select()"_s);
                return false;
            }
            limit = std::max(limit, *descriptor + 1);
            FD_SET(*descriptor, &set);
            if (descriptors.size() >= FD_SETSIZE) {
                raiseValueError(globalObject, scope, "too many file descriptors in select()"_s);
                return false;
            }
            objects.append(item);
            descriptors.append(*descriptor);
        }
        return true;
    }

    // set2list()
    JSValue ready(JSGlobalObject* globalObject)
    {
        MarkedArgumentBuffer result;
        for (unsigned i = 0; i < descriptors.size(); ++i) {
            if (FD_ISSET(descriptors[i], &set))
                result.append(objects.at(i));
        }
        return newList(globalObject, result);
    }
};

// ---- poll

struct PollState final : NativeState {
    PYTHON_NATIVE_STATE(PollState);
    WriteBarrier<PyDict> registered; // What to look for, by descriptor
    Vector<struct pollfd> descriptors;
    bool isUpToDate { false };
    bool isRunning { false };
};

template<typename Visitor> void PollState::visit(Visitor& visitor) { visitor.append(registered); }

// _PyLong_UnsignedShort_Converter(). Nothing if it raised.
std::optional<unsigned short> toUnsignedShort(JSGlobalObject* globalObject, JSValue value) { return toUnsigned<unsigned short>(globalObject, value, "unsigned short"_s); }

#if PYTHON_HAVE_KQUEUE

// ---- kevent and kqueue

struct KeventState final : NativeState {
    PYTHON_NATIVE_STATE(KeventState);
    struct kevent event { };
};

template<typename Visitor> void KeventState::visit(Visitor&) { }

struct KqueueState final : NativeState {
    PYTHON_NATIVE_STATE(KqueueState);
    int descriptor { -1 };
};

template<typename Visitor> void KqueueState::visit(Visitor&) { }

bool isKevent(JSGlobalObject* globalObject, JSValue value) { return isInstance(globalObject, value, selectState(globalObject).kevent.get()); }

JSValue raiseClosedKqueue(JSGlobalObject* globalObject, ThrowScope& scope) { return raiseValueError(globalObject, scope, "I/O operation on closed kqueue object"_s); }

// newKqueue_Object(). There is no fork() to see to those that are open after.
JSValue newKqueue(JSGlobalObject* globalObject, PyType* type, int given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int descriptor = given == -1 ? kqueue() : given;
    if (descriptor < 0)
        return raiseOSError(globalObject, scope, errno);
    if (given == -1 && fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
        int error = errno;
        close(descriptor);
        return raiseOSError(globalObject, scope, error);
    }
    auto state = makeUnique<KqueueState>();
    state->descriptor = descriptor;
    return PyStateObject::create(vm, type->instanceStructure(), WTF::move(state));
}

#endif // PYTHON_HAVE_KQUEUE

} // anonymous namespace

// select(rlist, wlist, xlist, timeout=None, /)
PYTHON_NATIVE(selectSelect)
{
    NATIVE_PROLOGUE();
    struct timeval time;
    struct timeval* timePointer = nullptr;
    int64_t timeout = 0;
    if (JSValue given = args.at(3); given && !isNone(given)) {
        auto converted = timeFromSecondsObject(globalObject, given, TimeRounding::Timeout);
        if (scope.exception()) [[unlikely]] {
            if (catchException(globalObject, BuiltinType::TypeError))
                raiseTypeError(globalObject, scope, "timeout must be a float or None"_s);
            return { };
        }
        timeout = *converted;
        timeAsTimeval(globalObject, timeout, time, TimeRounding::Timeout);
        RETURN_IF_EXCEPTION(scope, { });
        if (time.tv_sec < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, "timeout must be non-negative"_s));
        timePointer = &time;
    }
    Waited toRead;
    Waited toWrite;
    Waited exceptional;
    toRead.fill(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    toWrite.fill(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    exceptional.fill(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    int limit = std::max({ toRead.limit, toWrite.limit, exceptional.limit });
    int64_t deadline = timePointer ? deadlineAfter(timeout) : 0;
    int count;
    while (true) {
        errno = 0;
        count = ::select(limit, toRead.limit ? &toRead.set : nullptr, toWrite.limit ? &toWrite.set : nullptr, exceptional.limit ? &exceptional.set : nullptr, timePointer);
        if (errno != EINTR)
            break;
        checkSignals(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!timePointer)
            continue;
        timeout = timeUntil(deadline);
        if (timeout < 0) {
            // They are as they were given, which is not to say that anything is ready.
            FD_ZERO(&toRead.set);
            FD_ZERO(&toWrite.set);
            FD_ZERO(&exceptional.set);
            count = 0;
            break;
        }
        timeAsTimeval(globalObject, timeout, time, TimeRounding::Ceiling);
    }
    if (count < 0)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    JSValue readable = toRead.ready(globalObject);
    JSValue writable = toWrite.ready(globalObject);
    return JSValue::encode(PyTuple::create(globalObject, { readable, writable, exceptional.ready(globalObject) }));
}

PYTHON_NATIVE(selectPoll)
{
    UNUSED_PARAM(callFrame);
    VM& vm = globalObject->vm();
    auto* object = PyStateObject::create(vm, selectState(globalObject).poll->instanceStructure(), makeUnique<PollState>());
    object->state<PollState>().registered.set(vm, object, PyDict::create(globalObject));
    return JSValue::encode(object);
}

// register(fd, eventmask=POLLIN | POLLPRI | POLLOUT, /) and modify(fd, eventmask, /)
PYTHON_NATIVE(pollRegister)
{
    bool isModify = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto& self = stateOf<PollState>(args[0]);
    auto descriptor = toFileDescriptorOrFile(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned short events = POLLIN | POLLPRI | POLLOUT;
    if (JSValue given = args.at(2)) {
        auto converted = toUnsignedShort(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        events = *converted;
    }
    JSValue key = jsNumber(*descriptor);
    if (isModify) {
        bool isRegistered = !!self.registered->get(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isRegistered)
            return JSValue::encode(raiseOSError(globalObject, scope, ENOENT));
    }
    self.registered->set(globalObject, key, jsNumber(events));
    RETURN_IF_EXCEPTION(scope, { });
    self.isUpToDate = false;
    RETURN_NONE();
}

// unregister(fd, /)
PYTHON_NATIVE(pollUnregister)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<PollState>(args[0]);
    auto descriptor = toFileDescriptorOrFile(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue key = jsNumber(*descriptor);
    bool wasRegistered = !!self.registered->remove(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!wasRegistered)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, key));
    self.isUpToDate = false;
    RETURN_NONE();
}

// poll(timeout=None, /)
PYTHON_NATIVE(pollPoll)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<PollState>(args[0]);
    int64_t timeout = -1;
    int64_t milliseconds = -1;
    int64_t deadline = 0;
    if (JSValue given = args.at(1); given && !isNone(given)) {
        auto converted = timeFromMillisecondsObject(globalObject, given, TimeRounding::Timeout);
        if (scope.exception()) [[unlikely]] {
            if (catchException(globalObject, BuiltinType::TypeError))
                raiseTypeError(globalObject, scope, "timeout must be an integer or None"_s);
            return { };
        }
        timeout = *converted;
        milliseconds = divideTime(timeout, nanosecondsPerMillisecond, TimeRounding::Timeout);
        if (milliseconds < std::numeric_limits<int>::min() || milliseconds > std::numeric_limits<int>::max())
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "timeout is too large"_s));
        if (timeout >= 0)
            deadline = deadlineAfter(timeout);
    }
    // Some systems will have less than nothing be just this.
    if (milliseconds < 0)
        milliseconds = -1;
    if (self.isRunning)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "concurrent poll() invocation"_s));

    // update_ufd_array()
    if (!self.isUpToDate) {
        self.descriptors.shrink(0);
        self.registered->forEach(globalObject, [&] (JSValue key, JSValue value) {
            self.descriptors.append({ key.asInt32(), static_cast<short>(static_cast<unsigned short>(value.asInt32())), 0 });
            return true;
        });
        self.isUpToDate = true;
    }

    self.isRunning = true;
    int count;
    while (true) {
        errno = 0;
        count = ::poll(self.descriptors.mutableSpan().data(), static_cast<nfds_t>(self.descriptors.size()), static_cast<int>(milliseconds));
        if (errno != EINTR)
            break;
        checkSignals(globalObject);
        if (scope.exception()) [[unlikely]] {
            self.isRunning = false;
            return { };
        }
        if (timeout < 0)
            continue;
        timeout = timeUntil(deadline);
        if (timeout < 0) {
            count = 0;
            break;
        }
        milliseconds = divideTime(timeout, nanosecondsPerMillisecond, TimeRounding::Ceiling);
    }
    self.isRunning = false;
    if (count < 0)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    MarkedArgumentBuffer result;
    for (auto& entry : self.descriptors) {
        if (entry.revents)
            result.append(PyTuple::create(globalObject, { jsNumber(entry.fd), jsNumber(entry.revents & 0xffff) }));
    }
    return JSValue::encode(newList(globalObject, result));
}

#if PYTHON_HAVE_KQUEUE

// PyType_GenericNew(): whatever it is given is for __init__().
PYTHON_NATIVE(keventNew)
{
    return JSValue::encode(PyStateObject::create(globalObject->vm(), asType(callFrame->uncheckedArgument(0))->instanceStructure(), makeUnique<KeventState>()));
}

// kevent(ident, filter=KQ_FILTER_READ, flags=KQ_EV_ADD, fflags=0, data=0, udata=0)
PYTHON_NATIVE(keventInit)
{
    NATIVE_PROLOGUE();
    struct kevent& event = stateOf<KeventState>(args[0]).event;
    EV_SET(&event, 0, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    // What has been got through by the time that something is wrong stays.
    if (JSValue filter = args.at(2)) {
        // "h"
        auto value = toCLong(globalObject, filter);
        RETURN_IF_EXCEPTION(scope, { });
        if (*value < SHRT_MIN)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "signed short integer is less than minimum"_s));
        if (*value > SHRT_MAX)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "signed short integer is greater than maximum"_s));
        event.filter = static_cast<short>(*value);
    }
    // "H" and "I": PyLong_AsUnsignedLongMask()
    if (JSValue flags = args.at(3)) {
        JSValue integer = toInt(globalObject, flags);
        RETURN_IF_EXCEPTION(scope, { });
        event.flags = static_cast<unsigned short>(lowBitsOfInt(integer));
    }
    if (JSValue moreFlags = args.at(4)) {
        JSValue integer = toInt(globalObject, moreFlags);
        RETURN_IF_EXCEPTION(scope, { });
        event.fflags = static_cast<unsigned>(lowBitsOfInt(integer));
    }
    // "L"
    if (JSValue data = args.at(5)) {
        auto value = toCLongLong(globalObject, data);
        RETURN_IF_EXCEPTION(scope, { });
        event.data = static_cast<intptr_t>(*value);
    }
    auto hasIndex = [&] (JSValue value) { return !!typeOf(globalObject, value)->lookup(vm, names.dunder_index); };
    // "K"
    if (JSValue userData = args.at(6)) {
        if (!hasIndex(userData))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("kevent() argument 6 must be int, not "_s, typeNameOfArgument(globalObject, userData))));
        JSValue integer = toInt(globalObject, userData);
        RETURN_IF_EXCEPTION(scope, { });
        event.udata = std::bit_cast<void*>(static_cast<uintptr_t>(lowBitsOfInt(integer)));
    }
    JSValue identifier = args[1];
    if (hasIndex(identifier)) {
        // PyLong_AsNativeBytes(), of what is not to be less than nothing
        auto value = toUnsigned<uintptr_t>(globalObject, identifier, "kqueue event identifier"_s);
        RETURN_IF_EXCEPTION(scope, { });
        event.ident = *value;
    } else {
        // It is stored before it is looked at, and what says that it went wrong is -1.
        event.ident = static_cast<uintptr_t>(-1);
        auto descriptor = toFileDescriptorOrFile(globalObject, identifier);
        RETURN_IF_EXCEPTION(scope, { });
        event.ident = static_cast<uintptr_t>(*descriptor);
    }
    RETURN_NONE();
}

PYTHON_NATIVE(keventRepr)
{
    NATIVE_PROLOGUE();
    const struct kevent& event = stateOf<KeventState>(args[0]).event;
    // %p, as PyUnicode_FromFormat() has it: always with 0x before it
    return JSValue::encode(jsString(vm, makeString("<select.kevent ident="_s, static_cast<uint64_t>(event.ident), " filter="_s, static_cast<int>(event.filter), " flags=0x"_s, hex(static_cast<unsigned>(event.flags), Lowercase), " fflags=0x"_s, hex(static_cast<unsigned>(event.fflags), Lowercase), " data=0x"_s, hex(static_cast<uint64_t>(event.data), Lowercase), " udata=0x"_s, hex(static_cast<uint64_t>(std::bit_cast<uintptr_t>(event.udata)), Lowercase), '>')));
}

PYTHON_NATIVE(keventCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!isKevent(globalObject, args[1]))
        RETURN_NOT_IMPLEMENTED();
    const struct kevent& left = stateOf<KeventState>(args[0]).event;
    const struct kevent& right = stateOf<KeventState>(args[1]).event;
    auto compare = [] (auto a, auto b) { return a != b ? (a < b ? -1 : 1) : 0; };
    int result = compare(left.ident, right.ident);
    if (!result)
        result = compare(left.filter, right.filter);
    if (!result)
        result = compare(left.flags, right.flags);
    if (!result)
        result = compare(left.fflags, right.fflags);
    if (!result)
        result = compare(left.data, right.data);
    if (!result)
        result = compare(std::bit_cast<intptr_t>(left.udata), std::bit_cast<intptr_t>(right.udata));
    switch (op) {
    case ComparisonOperator::Eq:
        return JSValue::encode(jsBoolean(!result));
    case ComparisonOperator::NotEq:
        return JSValue::encode(jsBoolean(!!result));
    case ComparisonOperator::Lt:
        return JSValue::encode(jsBoolean(result < 0));
    case ComparisonOperator::LtE:
        return JSValue::encode(jsBoolean(result <= 0));
    case ComparisonOperator::Gt:
        return JSValue::encode(jsBoolean(result > 0));
    case ComparisonOperator::GtE:
        return JSValue::encode(jsBoolean(result >= 0));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

PYTHON_NATIVE(kqueueNew)
{
    NATIVE_PROLOGUE();
    if (args.size() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, "kqueue() takes no positional arguments"_s));
    if (!args.checkNoKeywords(globalObject, scope, "kqueue"_s))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(newKqueue(globalObject, asType(args[0]), -1)));
}

// fromfd(fd, /)
PYTHON_NATIVE(kqueueFromDescriptor)
{
    NATIVE_PROLOGUE();
    auto descriptor = toCInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newKqueue(globalObject, asType(args[0]), *descriptor)));
}

PYTHON_NATIVE(kqueueClose)
{
    NATIVE_PROLOGUE();
    // What goes wrong with closing it is not said: an errno is never less than nothing, which is what CPython looks for.
    if (int descriptor = std::exchange(stateOf<KqueueState>(args[0]).descriptor, -1); descriptor >= 0) {
        noteClosingOfDescriptor(globalObject, descriptor);
        close(descriptor);
    }
    RETURN_NONE();
}

PYTHON_NATIVE(kqueueFileno)
{
    NATIVE_PROLOGUE();
    int descriptor = stateOf<KqueueState>(args[0]).descriptor;
    if (descriptor < 0)
        return JSValue::encode(raiseClosedKqueue(globalObject, scope));
    return JSValue::encode(jsNumber(descriptor));
}

// control(changelist, maxevents, timeout=None, /)
PYTHON_NATIVE(kqueueControl)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<KqueueState>(args[0]);
    auto maximum = toCInt(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    if (self.descriptor < 0)
        return JSValue::encode(raiseClosedKqueue(globalObject, scope));
    if (*maximum < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("Length of eventlist must be 0 or positive, got "_s, *maximum)));
    struct timespec time;
    struct timespec* timePointer = nullptr;
    int64_t timeout = 0;
    if (JSValue given = args.at(3); given && !isNone(given)) {
        auto converted = timeFromSecondsObject(globalObject, given, TimeRounding::Timeout);
        if (scope.exception()) [[unlikely]] {
            // Whatever the matter was
            if (catchException(globalObject, BuiltinType::BaseException))
                raiseTypeError(globalObject, scope, concatenate("timeout argument must be a number or None, got "_s, typeOf(globalObject, given)->nameWithoutModule(globalObject)));
            return { };
        }
        timeout = *converted;
        timeAsTimespec(globalObject, timeout, time);
        if (time.tv_sec < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, "timeout must be positive or None"_s));
        timePointer = &time;
    }
    Vector<struct kevent, 8> changes;
    if (!isNone(args[1])) {
        JSValue items = toFastSequence(globalObject, args[1], "changelist is not iterable"_s);
        RETURN_IF_EXCEPTION(scope, { });
        for (unsigned i = 0, count = fastSequenceLength(items); i < count; ++i) {
            JSValue item = fastSequenceAt(items, i);
            if (!isKevent(globalObject, item))
                return JSValue::encode(raiseTypeError(globalObject, scope, "changelist must be an iterable of select.kevent objects"_s));
            changes.append(stateOf<KeventState>(item).event);
        }
    }
    Vector<struct kevent, 8> events;
    if (!events.tryGrow(static_cast<size_t>(*maximum)))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    // An event loop that the host turns does not wait. It says how long it would have, and it is the host that waits, with nothing of Python's on the stack: turnEventLoop().
    bool isBeingTurned = *maximum && isEventLoopBeingTurned(globalObject);
    std::optional<Seconds> wouldHaveWaited = timePointer ? std::optional { Seconds::fromNanoseconds(static_cast<double>(timeout)) } : std::nullopt;
    if (isBeingTurned) {
        timeout = 0;
        time = { 0, 0 };
        timePointer = &time;
    }
    int64_t deadline = timePointer ? deadlineAfter(timeout) : 0;
    int count;
    // With a host that has things of its own to do, it is the host that waits: Configuration::waitForDescriptor. This looks, without waiting, before and after.
    // There is no waiting if there is nothing to be told of, or no time to wait for.
    auto wait = realm->configuration().waitForDescriptor;
    bool hostWaits = wait && *maximum && (!timePointer || timeout > 0);
    while (hostWaits) {
        // What the host ran meanwhile may have closed it.
        if (self.descriptor < 0)
            return JSValue::encode(raiseClosedKqueue(globalObject, scope));
        struct timespec noTime { 0, 0 };
        errno = 0;
        count = kevent(self.descriptor, changes.span().data(), static_cast<int>(changes.size()), events.mutableSpan().data(), *maximum, &noTime);
        // They have been made, even if it was interrupted.
        changes.clear();
        if (count && errno != EINTR)
            break;
        checkSignals(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (timePointer) {
            timeout = timeUntil(deadline);
            if (timeout <= 0) {
                count = 0;
                break;
            }
        }
        wait(globalObject, self.descriptor, timePointer ? std::optional { Seconds::fromNanoseconds(static_cast<double>(timeout)) } : std::nullopt);
        RETURN_IF_EXCEPTION(scope, { });
    }
    while (!hostWaits) {
        errno = 0;
        count = kevent(self.descriptor, changes.span().data(), static_cast<int>(changes.size()), events.mutableSpan().data(), *maximum, timePointer);
        if (errno != EINTR)
            break;
        checkSignals(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!timePointer)
            continue;
        timeout = timeUntil(deadline);
        if (timeout < 0) {
            count = 0;
            break;
        }
        timeAsTimespec(globalObject, timeout, time);
    }
    if (count == -1)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    if (isBeingTurned) {
        // A selector asks to be told of as many things as it is watching for.
        noteWaitOfEventLoop(globalObject, self.descriptor, *maximum, wouldHaveWaited, count);
        RETURN_IF_EXCEPTION(scope, { });
    }
    MarkedArgumentBuffer result;
    Structure* structure = selectState(globalObject).kevent->instanceStructure();
    for (int i = 0; i < count; ++i) {
        auto state = makeUnique<KeventState>();
        state->event = events[i];
        result.append(PyStateObject::create(vm, structure, WTF::move(state)));
    }
    return JSValue::encode(newList(globalObject, result));
}

#endif // PYTHON_HAVE_KQUEUE

JSObject* createSelectModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = selectState(globalObject);
    using Kind = PyNativeFunction::Kind;
    auto createType = [&] (WriteBarrier<PyType>& member, ASCIILiteral name) {
        PyType* type = createBuiltinType(globalObject, name, realm->typeObject(), PyType::Layout::Native, 0);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        member.set(vm, realm, type);
        return type;
    };
    if (!state.poll) {
        PyType* poll = createType(state.poll, "select.poll"_s);
        addMethods(globalObject, poll, {
            { "register"_s, pollRegister, Kind::Method, pack(false) },
            { "modify"_s, pollRegister, Kind::Method, pack(true) },
            { "unregister"_s, pollUnregister },
            { "poll"_s, pollPoll },
        });
#if PYTHON_HAVE_KQUEUE
        PyType* kevent = createType(state.kevent, "select.kevent"_s);
        addMethods(globalObject, kevent, {
            { "__new__"_s, keventNew, Kind::New, 0, "($type, /, *args, **kwargs)"_s, PyNativeFunction::Arguments::AreNotChecked },
            { "__init__"_s, keventInit, Kind::Wrapper, 0, "(ident, filter=<unrepresentable>, flags=<unrepresentable>, fflags=0, data=0, udata=0)"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
            { "__repr__"_s, keventRepr },
        });
        addComparisons(globalObject, kevent, keventCompare);
        // It says when two are equal and nothing of what they hash to.
        kevent->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
#define EVENT(self) stateOf<KeventState>(self).event
        addMember(globalObject, kevent, "ident"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return intFromUInt64(globalObject, EVENT(self).ident); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
            unsigned long long field = EVENT(self).ident;
            setMember(globalObject, value, field);
            EVENT(self).ident = static_cast<uintptr_t>(field);
        });
        addMember(globalObject, kevent, "filter"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(EVENT(self).filter); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) { setMember(globalObject, value, EVENT(self).filter); });
        addMember(globalObject, kevent, "flags"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(EVENT(self).flags); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) { setMember(globalObject, value, EVENT(self).flags); });
        addMember(globalObject, kevent, "fflags"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return intFromInt64(globalObject, EVENT(self).fflags); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) { setMember(globalObject, value, EVENT(self).fflags); });
        addMember(globalObject, kevent, "data"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return intFromInt64(globalObject, EVENT(self).data); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
            long long field = EVENT(self).data;
            setMember(globalObject, value, field);
            EVENT(self).data = static_cast<intptr_t>(field);
        });
        addMember(globalObject, kevent, "udata"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return intFromUInt64(globalObject, std::bit_cast<uintptr_t>(EVENT(self).udata)); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
            unsigned long long field = std::bit_cast<uintptr_t>(EVENT(self).udata);
            setMember(globalObject, value, field);
            EVENT(self).udata = std::bit_cast<void*>(static_cast<uintptr_t>(field));
        });
#undef EVENT
        PyType* kqueue = createType(state.kqueue, "select.kqueue"_s);
        addMethods(globalObject, kqueue, {
            { "__new__"_s, kqueueNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
            { "__del__"_s, kqueueClose, Kind::Wrapper },
            { "fromfd"_s, kqueueFromDescriptor, Kind::ClassMethod },
            { "close"_s, kqueueClose },
            { "fileno"_s, kqueueFileno },
            { "control"_s, kqueueControl },
        });
        addGetSet(globalObject, kqueue, "closed"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOf<KqueueState>(self).descriptor < 0); });
#endif
    }

    JSObject* module = newBuiltinModule(globalObject, "select"_s);
    addFunction(globalObject, module, "select"_s, selectSelect);
    addFunction(globalObject, module, "poll"_s, selectPoll);
    auto add = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    add("error"_s, realm->typeOSError());
#define ADD(name) add(#name ""_s, jsNumber(name))
    ADD(PIPE_BUF);
    ADD(POLLIN);
    ADD(POLLPRI);
    ADD(POLLOUT);
    ADD(POLLERR);
    ADD(POLLHUP);
    ADD(POLLNVAL);
#ifdef POLLRDNORM
    ADD(POLLRDNORM);
#endif
#ifdef POLLRDBAND
    ADD(POLLRDBAND);
#endif
#ifdef POLLWRNORM
    ADD(POLLWRNORM);
#endif
#ifdef POLLWRBAND
    ADD(POLLWRBAND);
#endif
#ifdef POLLMSG
    ADD(POLLMSG);
#endif
#ifdef POLLRDHUP
    ADD(POLLRDHUP);
#endif
#undef ADD
#if PYTHON_HAVE_KQUEUE
    add("kevent"_s, state.kevent.get());
    add("kqueue"_s, state.kqueue.get());
#define ADD(name, value) add("KQ_" #name ""_s, intFromInt64(globalObject, static_cast<int64_t>(value)))
    ADD(FILTER_READ, EVFILT_READ);
    ADD(FILTER_WRITE, EVFILT_WRITE);
#ifdef EVFILT_AIO
    ADD(FILTER_AIO, EVFILT_AIO);
#endif
#ifdef EVFILT_VNODE
    ADD(FILTER_VNODE, EVFILT_VNODE);
#endif
#ifdef EVFILT_PROC
    ADD(FILTER_PROC, EVFILT_PROC);
#endif
#ifdef EVFILT_NETDEV
    ADD(FILTER_NETDEV, EVFILT_NETDEV);
#endif
#ifdef EVFILT_SIGNAL
    ADD(FILTER_SIGNAL, EVFILT_SIGNAL);
#endif
    ADD(FILTER_TIMER, EVFILT_TIMER);
    ADD(EV_ADD, EV_ADD);
    ADD(EV_DELETE, EV_DELETE);
    ADD(EV_ENABLE, EV_ENABLE);
    ADD(EV_DISABLE, EV_DISABLE);
    ADD(EV_ONESHOT, EV_ONESHOT);
    ADD(EV_CLEAR, EV_CLEAR);
#ifdef EV_SYSFLAGS
    ADD(EV_SYSFLAGS, EV_SYSFLAGS);
#endif
#ifdef EV_FLAG1
    ADD(EV_FLAG1, EV_FLAG1);
#endif
    ADD(EV_EOF, EV_EOF);
    ADD(EV_ERROR, EV_ERROR);
#ifdef NOTE_LOWAT
    ADD(NOTE_LOWAT, NOTE_LOWAT);
#endif
#ifdef EVFILT_VNODE
    ADD(NOTE_DELETE, NOTE_DELETE);
    ADD(NOTE_WRITE, NOTE_WRITE);
    ADD(NOTE_EXTEND, NOTE_EXTEND);
    ADD(NOTE_ATTRIB, NOTE_ATTRIB);
    ADD(NOTE_LINK, NOTE_LINK);
    ADD(NOTE_RENAME, NOTE_RENAME);
    ADD(NOTE_REVOKE, NOTE_REVOKE);
#endif
#ifdef EVFILT_PROC
    ADD(NOTE_EXIT, NOTE_EXIT);
    ADD(NOTE_FORK, NOTE_FORK);
    ADD(NOTE_EXEC, NOTE_EXEC);
    ADD(NOTE_PCTRLMASK, NOTE_PCTRLMASK);
    ADD(NOTE_PDATAMASK, NOTE_PDATAMASK);
    ADD(NOTE_TRACK, NOTE_TRACK);
    ADD(NOTE_CHILD, NOTE_CHILD);
    ADD(NOTE_TRACKERR, NOTE_TRACKERR);
#endif
#ifdef EVFILT_NETDEV
    ADD(NOTE_LINKUP, NOTE_LINKUP);
    ADD(NOTE_LINKDOWN, NOTE_LINKDOWN);
    ADD(NOTE_LINKINV, NOTE_LINKINV);
#endif
#undef ADD
#endif
    return module;
}

} } // namespace JSC::Python
