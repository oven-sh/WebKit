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
#include "PythonPosix.h"

#if OS(LINUX)

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonOperations.h"
#include "PythonPosixModule.h"
#include "PythonSequences.h"
#include "PythonSignals.h"
#include "PythonTime.h"
#include <errno.h>
#include <linux/limits.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/xattr.h>
#include <unistd.h>

// What os has on Linux and not on macOS: the rest of Modules/posixmodule.c of CPython.

namespace JSC { namespace Python {

#define CONVERT_PATH(variable, value, function, argument, options) \
    PathArgument variable(function, argument, options); \
    if (!variable.convert(globalObject, value)) \
        return { };
#define CONVERT(variable, expression) \
    auto variable##Converted = expression; \
    RETURN_IF_EXCEPTION(scope, { }); \
    auto variable = *variable##Converted;
#define CONVERT_BOOL(variable, value, defaultValue) \
    bool variable = defaultValue; \
    if (JSValue given = value) { \
        variable = isTrue(globalObject, given); \
        RETURN_IF_EXCEPTION(scope, { }); \
    }
#define CONVERT_INT_OR(variable, value, defaultValue) \
    int variable = defaultValue; \
    if (JSValue given = value) { \
        auto converted = toCInt(globalObject, given); \
        RETURN_IF_EXCEPTION(scope, { }); \
        variable = *converted; \
    }
// `unsigned_int`: _PyLong_UnsignedInt_Converter()
#define CONVERT_UNSIGNED_OR(variable, value, defaultValue) \
    unsigned variable = defaultValue; \
    if (JSValue given = value) { \
        auto converted = toUnsigned<unsigned>(globalObject, given, "unsigned int"_s); \
        RETURN_IF_EXCEPTION(scope, { }); \
        variable = *converted; \
    }

// ---- Descriptors that are made out of nothing

// eventfd(initval, flags=EFD_CLOEXEC)
PYTHON_NATIVE(posixEventfd)
{
    NATIVE_PROLOGUE();
    CONVERT_UNSIGNED_OR(initial, args.at(0), 0);
    CONVERT_INT_OR(flags, args.at(1), EFD_CLOEXEC);
    int descriptor = ::eventfd(initial, flags);
    if (descriptor == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsNumber(descriptor));
}

PYTHON_NATIVE(posixEventfdRead)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toFileDescriptorOrFile(globalObject, args.at(0)));
    eventfd_t value;
    if (::eventfd_read(descriptor, &value) == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(intFromUInt64(globalObject, value));
}

PYTHON_NATIVE(posixEventfdWrite)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toFileDescriptorOrFile(globalObject, args.at(0)));
    CONVERT(value, toUnsigned<unsigned long long>(globalObject, args.at(1), "unsigned long long"_s));
    if (::eventfd_write(descriptor, value) == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// memfd_create(name, flags=MFD_CLOEXEC)
PYTHON_NATIVE(posixMemfdCreate)
{
    NATIVE_PROLOGUE();
    CONVERT(name, toFileSystemEncoded(globalObject, args.at(0)));
    unsigned flags = MFD_CLOEXEC;
    if (JSValue given = args.at(1)) {
        // PyLong_AsUnsignedLongMask()
        JSValue integer = toInt(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        flags = static_cast<unsigned>(lowBitsOfInt(integer));
    }
    int descriptor = ::memfd_create(name.data(), flags);
    if (descriptor == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsNumber(descriptor));
}

// pidfd_open(pid, flags=0)
PYTHON_NATIVE(posixPidfdOpen)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    CONVERT_UNSIGNED_OR(flags, args.at(1), 0);
    long descriptor = ::syscall(SYS_pidfd_open, process, flags);
    if (descriptor < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsNumber(static_cast<int>(descriptor)));
}

PYTHON_NATIVE(posixPipe2)
{
    NATIVE_PROLOGUE();
    CONVERT(flags, toCInt(globalObject, args.at(0)));
    int descriptors[2];
    if (::pipe2(descriptors, flags))
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(PyTuple::create(globalObject, { jsNumber(descriptors[0]), jsNumber(descriptors[1]) }));
}

// ---- Timers that are descriptors

// build_itimerspec() and build_itimerspec_ns()
static JSValue timerSettings(JSGlobalObject* globalObject, const struct itimerspec& settings, bool inNanoseconds)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!inNanoseconds)
        return PyTuple::create(globalObject, { floatFromDouble(timespecAsSeconds(settings.it_value)), floatFromDouble(timespecAsSeconds(settings.it_interval)) });
    auto value = timeFromTimespec(globalObject, settings.it_value);
    RETURN_IF_EXCEPTION(scope, { });
    auto interval = timeFromTimespec(globalObject, settings.it_interval);
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { intFromInt64(globalObject, *value), intFromInt64(globalObject, *interval) });
}

// timerfd_create(clockid, /, *, flags=0)
PYTHON_NATIVE(posixTimerfdCreate)
{
    NATIVE_PROLOGUE();
    CONVERT(clock, toCInt(globalObject, args.at(0)));
    CONVERT_INT_OR(flags, args.at(1), 0);
    // It is not passed on to what the process runs, whatever is asked for.
    int descriptor = ::timerfd_create(clock, flags | TFD_CLOEXEC);
    if (descriptor == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsNumber(descriptor));
}

// timerfd_settime(fd, /, *, flags=0, initial=0.0, interval=0.0) and timerfd_settime_ns(fd, /, *, flags=0, initial=0, interval=0)
PYTHON_NATIVE(posixTimerfdSettime)
{
    NATIVE_PROLOGUE();
    bool inNanoseconds = unpack<bool>(callFrame, 0);
    CONVERT(descriptor, toFileDescriptorOrFile(globalObject, args.at(0)));
    CONVERT_INT_OR(flags, args.at(1), 0);
    int64_t times[2] = { 0, 0 };
    if (inNanoseconds) {
        for (unsigned i = 0; i < 2; ++i) {
            if (JSValue given = args.at(2 + i)) {
                CONVERT(nanoseconds, toCLongLong(globalObject, given));
                times[i] = nanoseconds;
            }
        }
    } else {
        // Both are made numbers of C's first, by what CPython generates.
        JSValue seconds[2];
        for (unsigned i = 0; i < 2; ++i) {
            if (JSValue given = args.at(2 + i)) {
                CONVERT(number, toDouble(globalObject, given));
                // _PyTime_FromSecondsDouble(), which does not look for what is not a number, and finds that it is in no range
                if (std::isnan(number))
                    return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "timestamp out of range for C PyTime_t"_s));
                seconds[i] = floatFromDouble(number);
            }
        }
        for (unsigned i = 0; i < 2; ++i) {
            if (seconds[i]) {
                CONVERT(time, timeFromSecondsObject(globalObject, seconds[i], TimeRounding::Floor));
                times[i] = time;
            }
        }
    }
    struct itimerspec settings;
    struct itimerspec previous;
    if (!timeAsTimespec(globalObject, times[0], settings.it_value)) {
        if (catchException(globalObject, BuiltinType::BaseException))
            raiseValueError(globalObject, scope, "invalid initial value"_s);
        return { };
    }
    if (!timeAsTimespec(globalObject, times[1], settings.it_interval)) {
        if (catchException(globalObject, BuiltinType::BaseException))
            raiseValueError(globalObject, scope, "invalid interval value"_s);
        return { };
    }
    if (::timerfd_settime(descriptor, flags, &settings, &previous) == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(timerSettings(globalObject, previous, inNanoseconds)));
}

// timerfd_gettime(fd, /) and timerfd_gettime_ns(fd, /)
PYTHON_NATIVE(posixTimerfdGettime)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toFileDescriptorOrFile(globalObject, args.at(0)));
    struct itimerspec settings;
    if (::timerfd_gettime(descriptor, &settings) == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(timerSettings(globalObject, settings, unpack<bool>(callFrame, 0))));
}

// ---- What is in files

PYTHON_NATIVE(posixFdatasync)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toFileDescriptorOrFile(globalObject, args.at(0)));
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return -std::abs(::fdatasync(descriptor)); }))
        return { };
    if (result)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// posix_fadvise(fd, offset, length, advice, /) and posix_fallocate(fd, offset, length, /), which return what errno would be
PYTHON_NATIVE(posixAdviseOrAllocate)
{
    NATIVE_PROLOGUE();
    bool isAdvice = unpack<bool>(callFrame, 0);
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(offset, toFileOffset(globalObject, args.at(1)));
    CONVERT(length, toFileOffset(globalObject, args.at(2)));
    CONVERT_INT_OR(advice, isAdvice ? args.at(3) : JSValue(), 0);
    int result;
    do {
        result = isAdvice ? ::posix_fadvise(descriptor, offset, length, advice) : ::posix_fallocate(descriptor, offset, length);
        if (result != EINTR)
            break;
        if (!checkSignals(globalObject))
            return { };
    } while (true);
    if (!result)
        RETURN_NONE();
    return JSValue::encode(raiseOSError(globalObject, scope, result));
}

// copy_file_range(src, dst, count, offset_src=None, offset_dst=None) and splice(src, dst, count, offset_src=None, offset_dst=None, flags=0)
PYTHON_NATIVE(posixCopyBetweenDescriptors)
{
    NATIVE_PROLOGUE();
    bool isSplice = unpack<bool>(callFrame, 0);
    CONVERT(source, toCInt(globalObject, args.at(0)));
    CONVERT(destination, toCInt(globalObject, args.at(1)));
    CONVERT(count, toSsize(globalObject, args.at(2)));
    CONVERT_UNSIGNED_OR(flags, isSplice ? args.at(5) : JSValue(), 0);
    if (count < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "negative value for 'count' not allowed"_s));
    off_t offsets[2];
    off_t* pointers[2] = { nullptr, nullptr };
    for (unsigned i = 0; i < 2; ++i) {
        if (JSValue given = args.at(3 + i); given && !isNone(given)) {
            CONVERT(offset, toFileOffset(globalObject, given));
            offsets[i] = offset;
            pointers[i] = &offsets[i];
        }
    }
    ssize_t result;
    bool isDone = retryIfInterrupted(globalObject, result, [&] () -> ssize_t {
        return isSplice ? ::splice(source, pointers[0], destination, pointers[1], count, flags) : ::copy_file_range(source, pointers[0], destination, pointers[1], count, 0);
    });
    if (!isDone)
        return { };
    if (result < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(intFromInt64(globalObject, result));
}

// getrandom(size, flags=0)
PYTHON_NATIVE(posixGetrandom)
{
    NATIVE_PROLOGUE();
    CONVERT(size, toSsize(globalObject, args.at(0)));
    CONVERT_INT_OR(flags, args.at(1), 0);
    if (size < 0)
        return JSValue::encode(raiseOSError(globalObject, scope, EINVAL));
    Vector<uint8_t> bytes;
    if (!bytes.tryGrow(static_cast<size_t>(size)))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    long count;
    bool isDone = retryIfInterrupted(globalObject, count, [&] () -> long { return ::syscall(SYS_getrandom, bytes.mutableSpan().data(), bytes.size(), flags); });
    if (!isDone)
        return { };
    if (count < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    bytes.shrink(static_cast<size_t>(count));
    return JSValue::encode(newBytes(globalObject, bytes.span()));
}

// ---- What is kept with a file besides what is in it

static bool descriptorAndFollowingAreInvalid(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, int descriptor, bool followsSymlinks)
{
    if (descriptor < 0 || followsSymlinks)
        return false;
    raiseValueError(globalObject, scope, concatenate(function, ": cannot use fd and follow_symlinks together"_s));
    return true;
}

// getxattr(path, attribute, *, follow_symlinks=True)
PYTHON_NATIVE(posixGetxattr)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "getxattr"_s, "path"_s, PathArgument::AllowsDescriptor);
    CONVERT_PATH(attribute, args.at(1), "getxattr"_s, "attribute"_s, 0);
    CONVERT_BOOL(followsSymlinks, args.at(2), true);
    if (descriptorAndFollowingAreInvalid(globalObject, scope, "getxattr"_s, path.descriptor, followsSymlinks))
        return { };
    if (!audit(globalObject, "os.getxattr"_s, path.object, attribute.object))
        return { };
    for (size_t size : { static_cast<size_t>(128), static_cast<size_t>(XATTR_SIZE_MAX) }) {
        Vector<uint8_t> bytes(size);
        ssize_t result = path.descriptor >= 0 ? ::fgetxattr(path.descriptor, attribute.narrow(), bytes.mutableSpan().data(), size)
            : followsSymlinks ? ::getxattr(path.narrow(), attribute.narrow(), bytes.mutableSpan().data(), size) : ::lgetxattr(path.narrow(), attribute.narrow(), bytes.mutableSpan().data(), size);
        if (result >= 0) {
            bytes.shrink(static_cast<size_t>(result));
            return JSValue::encode(newBytes(globalObject, bytes.span()));
        }
        if (errno != ERANGE)
            break;
    }
    return JSValue::encode(raisePathError(globalObject, scope, path));
}

// setxattr(path, attribute, value, flags=0, *, follow_symlinks=True)
PYTHON_NATIVE(posixSetxattr)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "setxattr"_s, "path"_s, PathArgument::AllowsDescriptor);
    CONVERT_PATH(attribute, args.at(1), "setxattr"_s, "attribute"_s, 0);
    Buffer value = bufferOf(globalObject, args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    CONVERT_INT_OR(flags, args.at(3), 0);
    CONVERT_BOOL(followsSymlinks, args.at(4), true);
    if (descriptorAndFollowingAreInvalid(globalObject, scope, "setxattr"_s, path.descriptor, followsSymlinks))
        return { };
    if (!audit(globalObject, "os.setxattr"_s, path.object, attribute.object, newBytes(globalObject, value.span()), jsNumber(flags)))
        return { };
    int result = path.descriptor > -1 ? ::fsetxattr(path.descriptor, attribute.narrow(), value.data(), value.size(), flags)
        : followsSymlinks ? ::setxattr(path.narrow(), attribute.narrow(), value.data(), value.size(), flags) : ::lsetxattr(path.narrow(), attribute.narrow(), value.data(), value.size(), flags);
    if (result)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

// removexattr(path, attribute, *, follow_symlinks=True)
PYTHON_NATIVE(posixRemovexattr)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "removexattr"_s, "path"_s, PathArgument::AllowsDescriptor);
    CONVERT_PATH(attribute, args.at(1), "removexattr"_s, "attribute"_s, 0);
    CONVERT_BOOL(followsSymlinks, args.at(2), true);
    if (descriptorAndFollowingAreInvalid(globalObject, scope, "removexattr"_s, path.descriptor, followsSymlinks))
        return { };
    if (!audit(globalObject, "os.removexattr"_s, path.object, attribute.object))
        return { };
    int result = path.descriptor > -1 ? ::fremovexattr(path.descriptor, attribute.narrow()) : followsSymlinks ? ::removexattr(path.narrow(), attribute.narrow()) : ::lremovexattr(path.narrow(), attribute.narrow());
    if (result)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

// listxattr(path=None, *, follow_symlinks=True)
PYTHON_NATIVE(posixListxattr)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0) ? args.at(0) : jsUndefined(), "listxattr"_s, "path"_s, PathArgument::AllowsDescriptor | PathArgument::Nullable);
    CONVERT_BOOL(followsSymlinks, args.at(1), true);
    if (descriptorAndFollowingAreInvalid(globalObject, scope, "listxattr"_s, path.descriptor, followsSymlinks))
        return { };
    if (!audit(globalObject, "os.listxattr"_s, path.object ? path.object : jsUndefined()))
        return { };
    const char* name = path.narrow() ? path.narrow() : ".";
    for (size_t size : { static_cast<size_t>(256), static_cast<size_t>(XATTR_LIST_MAX) }) {
        Vector<char> names(size);
        ssize_t length = path.descriptor > -1 ? ::flistxattr(path.descriptor, names.mutableSpan().data(), size) : followsSymlinks ? ::listxattr(name, names.mutableSpan().data(), size) : ::llistxattr(name, names.mutableSpan().data(), size);
        if (length < 0) {
            if (errno == ERANGE)
                continue;
            break;
        }
        // One after another, each ended by a zero.
        MarkedArgumentBuffer result;
        auto rest = names.span().first(static_cast<size_t>(length));
        while (!rest.empty()) {
            size_t end = find(rest, '\0');
            if (end == notFound)
                break;
            result.append(decodeFileSystemBytes(globalObject, rest.first(end)));
            RETURN_IF_EXCEPTION(scope, { });
            rest = rest.subspan(end + 1);
        }
        return JSValue::encode(newList(globalObject, result));
    }
    return JSValue::encode(raisePathError(globalObject, scope, path));
}

// ---- Who the process is

// getresuid() and getresgid()
PYTHON_NATIVE(posixGetAllThreeIDs)
{
    NATIVE_PROLOGUE();
    static_assert(std::is_same_v<uid_t, gid_t>);
    uid_t real, effective, saved;
    if ((unpack<bool>(callFrame, 0) ? ::getresuid(&real, &effective, &saved) : ::getresgid(&real, &effective, &saved)) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(PyTuple::create(globalObject, { intFromUserID(globalObject, real), intFromUserID(globalObject, effective), intFromUserID(globalObject, saved) }));
}

PYTHON_NATIVE(posixSetresuid)
{
    NATIVE_PROLOGUE();
    CONVERT(real, toUserID(globalObject, args.at(0)));
    CONVERT(effective, toUserID(globalObject, args.at(1)));
    CONVERT(saved, toUserID(globalObject, args.at(2)));
    if (::setresuid(real, effective, saved) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixSetresgid)
{
    NATIVE_PROLOGUE();
    CONVERT(real, toGroupID(globalObject, args.at(0)));
    CONVERT(effective, toGroupID(globalObject, args.at(1)));
    CONVERT(saved, toGroupID(globalObject, args.at(2)));
    if (::setresgid(real, effective, saved) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// setns(fd, nstype=0)
PYTHON_NATIVE(posixSetns)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toFileDescriptorOrFile(globalObject, args.at(0)));
    CONVERT_INT_OR(kind, args.at(1), 0);
    if (::setns(descriptor, kind))
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixUnshare)
{
    NATIVE_PROLOGUE();
    CONVERT(flags, toCInt(globalObject, args.at(0)));
    // The kernel takes them for a long, and what glibc has for this hands on the register as it is, the upper half of which need have nothing to do with an int that is in it.
    if (::syscall(SYS_unshare, static_cast<unsigned long>(static_cast<unsigned>(flags))))
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// ---- When a process is given its turn, and where

// sched_param(sched_priority)
PYTHON_NATIVE(schedulerParameterNew)
{
    NATIVE_PROLOGUE();
    MarkedArgumentBuffer values;
    values.append(args.at(1));
    RELEASE_AND_RETURN(scope, JSValue::encode(newStructSequence(globalObject, asType(args[0]), values)));
}

PYTHON_NATIVE(schedulerParameterReduce)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, args[0])->object(), PyTuple::create(globalObject, { asTuple(args[0])->at(0) }) }));
}

bool convertSchedulerParameter(JSGlobalObject* globalObject, JSValue value, struct sched_param& result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (typeOf(globalObject, value) != posixState(globalObject).schedulerParameter.get()) {
        raiseTypeError(globalObject, scope, "must have a sched_param object"_s);
        return false;
    }
    auto priority = toCLong(globalObject, asTuple(value)->at(0));
    RETURN_IF_EXCEPTION(scope, false);
    if (*priority > INT_MAX || *priority < INT_MIN) {
        raise(globalObject, scope, BuiltinType::OverflowError, "sched_priority out of range"_s);
        return false;
    }
    result.sched_priority = static_cast<int>(*priority);
    return true;
}

PYTHON_NATIVE(posixSchedGetscheduler)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    int policy = ::sched_getscheduler(process);
    if (policy < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsNumber(policy));
}

// sched_setscheduler(pid, policy, param, /)
PYTHON_NATIVE(posixSchedSetscheduler)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    CONVERT(policy, toCInt(globalObject, args.at(1)));
    struct sched_param parameter;
    if (!convertSchedulerParameter(globalObject, args.at(2), parameter))
        return { };
    if (::sched_setscheduler(process, policy, &parameter) == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixSchedGetparam)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    struct sched_param parameter;
    if (::sched_getparam(process, &parameter))
        return JSValue::encode(raisePosixError(globalObject, scope));
    MarkedArgumentBuffer values;
    values.append(jsNumber(parameter.sched_priority));
    RELEASE_AND_RETURN(scope, JSValue::encode(newStructSequence(globalObject, posixState(globalObject).schedulerParameter.get(), values)));
}

PYTHON_NATIVE(posixSchedSetparam)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    struct sched_param parameter;
    if (!convertSchedulerParameter(globalObject, args.at(1), parameter))
        return { };
    if (::sched_setparam(process, &parameter))
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixSchedRRGetInterval)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    struct timespec interval;
    if (::sched_rr_get_interval(process, &interval))
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(floatFromDouble(static_cast<double>(interval.tv_sec) + 1e-9 * interval.tv_nsec));
}

// A set of processors, of whatever size it has to be
class ProcessorSet {
    WTF_MAKE_NONCOPYABLE(ProcessorSet);
public:
    // NCPUS_START
    static constexpr int initialCount = sizeof(unsigned long) * CHAR_BIT;

    ProcessorSet() = default;
    ~ProcessorSet() { free(); }

    // False if there is no room.
    bool allocate(int count)
    {
        cpu_set_t* set = CPU_ALLOC(count);
        if (!set)
            return false;
        size_t size = CPU_ALLOC_SIZE(count);
        CPU_ZERO_S(size, set);
        if (m_set)
            memcpy(set, m_set, m_size);
        free();
        m_set = set;
        m_size = size;
        m_count = count;
        return true;
    }

    cpu_set_t* set() { return m_set; }
    size_t size() const { return m_size; }
    int count() const { return m_count; }

private:
    void free()
    {
        if (m_set)
            CPU_FREE(m_set);
        m_set = nullptr;
    }

    cpu_set_t* m_set { nullptr };
    size_t m_size { 0 };
    int m_count { 0 };
};

PYTHON_NATIVE(posixSchedGetaffinity)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    ProcessorSet processors;
    for (int count = ProcessorSet::initialCount; ; count *= 2) {
        if (!processors.allocate(count))
            return JSValue::encode(raiseMemoryError(globalObject, scope));
        if (!::sched_getaffinity(process, processors.size(), processors.set()))
            break;
        if (errno != EINVAL)
            return JSValue::encode(raisePosixError(globalObject, scope));
        if (count > INT_MAX / 2)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "could not allocate a large enough CPU set"_s));
    }
    PySet* result = PySet::create(globalObject);
    int remaining = CPU_COUNT_S(processors.size(), processors.set());
    for (int processor = 0; remaining; ++processor) {
        if (!CPU_ISSET_S(processor, processors.size(), processors.set()))
            continue;
        --remaining;
        result->add(globalObject, jsNumber(processor));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

// sched_setaffinity(pid, mask, /)
PYTHON_NATIVE(posixSchedSetaffinity)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    JSValue iterator = getIterator(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    ProcessorSet processors;
    if (!processors.allocate(ProcessorSet::initialCount))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    while (true) {
        JSValue item = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            break;
        if (!typeOf(globalObject, item)->lookup(vm, names.dunder_index)) {
            String shown = repr(globalObject, typeOf(globalObject, item)->object());
            RETURN_IF_EXCEPTION(scope, { });
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("expected an iterator of ints, but iterator yielded "_s, shown)));
        }
        CONVERT(processor, toCLong(globalObject, item));
        if (processor < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, "negative CPU number"_s));
        if (processor > INT_MAX - 1)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "CPU number too large"_s));
        if (processor >= processors.count()) {
            int count = processors.count();
            while (count <= processor)
                count = count > INT_MAX / 2 ? static_cast<int>(processor) + 1 : count * 2;
            if (!processors.allocate(count))
                return JSValue::encode(raiseMemoryError(globalObject, scope));
        }
        CPU_SET_S(static_cast<int>(processor), processors.size(), processors.set());
    }
    if (::sched_setaffinity(process, processors.size(), processors.set()))
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

void initializePosixLinuxTypes(JSGlobalObject* globalObject, PosixModuleState& state)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    static constexpr ASCIILiteral fields[] = { "sched_priority"_s };
    PyType* type = createBuiltinType(globalObject, "posix.sched_param"_s, realm->typeTuple(), PyType::Layout::Tuple, PyType::IsSequence | PyType::IsDerivedFromBuiltin);
    state.schedulerParameter.set(vm, realm, type);
    makeStructSequenceType(globalObject, type, std::span(fields), 1);
    // It is made of what it has in it, and not of a sequence of that.
    addMethods(globalObject, type, {
        { "__new__"_s, schedulerParameterNew, PyNativeFunction::Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__reduce__"_s, schedulerParameterReduce },
    });
}

void addPosixLinuxFunctions(JSGlobalObject* globalObject, JSObject* module)
{
    auto add = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, module, name, function, data); };
    add("eventfd"_s, posixEventfd);
    add("eventfd_read"_s, posixEventfdRead);
    add("eventfd_write"_s, posixEventfdWrite);
    add("memfd_create"_s, posixMemfdCreate);
    add("pidfd_open"_s, posixPidfdOpen);
    add("pipe2"_s, posixPipe2);
    add("timerfd_create"_s, posixTimerfdCreate);
    add("timerfd_settime"_s, posixTimerfdSettime, pack(false));
    add("timerfd_settime_ns"_s, posixTimerfdSettime, pack(true));
    add("timerfd_gettime"_s, posixTimerfdGettime, pack(false));
    add("timerfd_gettime_ns"_s, posixTimerfdGettime, pack(true));
    add("fdatasync"_s, posixFdatasync);
    add("posix_fadvise"_s, posixAdviseOrAllocate, pack(true));
    add("posix_fallocate"_s, posixAdviseOrAllocate, pack(false));
    add("copy_file_range"_s, posixCopyBetweenDescriptors, pack(false));
    add("splice"_s, posixCopyBetweenDescriptors, pack(true));
    add("getrandom"_s, posixGetrandom);
    add("getxattr"_s, posixGetxattr);
    add("setxattr"_s, posixSetxattr);
    add("removexattr"_s, posixRemovexattr);
    add("listxattr"_s, posixListxattr);
    add("getresuid"_s, posixGetAllThreeIDs, pack(true));
    add("getresgid"_s, posixGetAllThreeIDs, pack(false));
    add("setresuid"_s, posixSetresuid);
    add("setresgid"_s, posixSetresgid);
    add("setns"_s, posixSetns);
    add("unshare"_s, posixUnshare);
    add("sched_getscheduler"_s, posixSchedGetscheduler);
    add("sched_setscheduler"_s, posixSchedSetscheduler);
    add("sched_getparam"_s, posixSchedGetparam);
    add("sched_setparam"_s, posixSchedSetparam);
    add("sched_rr_get_interval"_s, posixSchedRRGetInterval);
    add("sched_getaffinity"_s, posixSchedGetaffinity);
    add("sched_setaffinity"_s, posixSchedSetaffinity);
}

} } // namespace JSC::Python

#endif // OS(LINUX)
