/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyModule.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyModuleHelpers.h"
#include "PyNamespace.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyStructSeq.h"
#include "PyThreads.h"
#include "PyTuple.h"
#include <cerrno>
#include <cmath>
#include <ctime>
#include <cwchar>
#include <locale.h>
#include <pthread.h>
#if OS(DARWIN)
#include <xlocale.h>
#endif
#include <wtf/Scope.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module time, for POSIX. (Modules/timemodule.c and Python/pytime.c in CPython.)
// FIXME: Windows has none of clock_gettime(), localtime_r(), tm_zone.

static constexpr int64_t timeNanosecondsPerSecond = 1000000000;

// ---- Arguments ----

// For the functions CPython has parse their own arguments, whose complaints are worded differently from those of the rest.
// False: a TypeError.
static bool timeCheckArguments(JSGlobalObject* globalObject, ASCIILiteral function, const CallArgs& args, unsigned min, unsigned max)
{
    if (!rejectKeywords(globalObject, function, args))
        return false;
    unsigned given = args.size();
    if (given >= min && given <= max)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned limit = given < min ? min : max;
    raiseTypeError(globalObject, scope, function, "() takes "_s, min == max ? "exactly"_s : given < min ? "at least"_s : "at most"_s, ' ', limit, " argument"_s, limit == 1 ? ""_s : "s"_s, " ("_s, given, " given)"_s);
    return false;
}

// An argument those functions take for a C int.
static int timeGetCInt(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t result = toInt64(globalObject, value);
    RETURN_IF_EXCEPTION(scope, -1);
    if (result < std::numeric_limits<int>::min() || result > std::numeric_limits<int>::max()) {
        raiseOverflowError(globalObject, scope, result < 0 ? "signed integer is less than minimum"_s : "signed integer is greater than maximum"_s);
        return -1;
    }
    return static_cast<int>(result);
}

static void timeRaiseOutOfRange(JSGlobalObject* globalObject, ThrowScope& scope)
{
    raiseOverflowError(globalObject, scope, "timestamp out of range for C PyTime_t"_s);
}

// A number of seconds, rounded down, as a time_t. False: an exception.
static bool timeGetSeconds(JSGlobalObject* globalObject, JSValue value, time_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static_assert(sizeof(time_t) == sizeof(int64_t));
    if (hasIndex(globalObject, value)) {
        JSValue index = toIndex(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        auto converted = tryInt64(index);
        if (!converted) {
            raiseOverflowError(globalObject, scope, "timestamp out of range for platform time_t"_s);
            return false;
        }
        result = *converted;
        return true;
    }
    double seconds = toDouble(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (std::isnan(seconds)) {
        raiseValueError(globalObject, scope, "Invalid value NaN (not a number)"_s);
        return false;
    }
    seconds = std::floor(seconds);
    if (!(seconds >= -0x1p63 && seconds < 0x1p63)) {
        raiseOverflowError(globalObject, scope, "timestamp out of range for platform time_t"_s);
        return false;
    }
    result = static_cast<time_t>(seconds);
    return true;
}

// A number of seconds, as nanoseconds rounded down or, for a timeout, away from zero. False: an exception.
static bool timeGetNanoseconds(JSGlobalObject* globalObject, JSValue value, bool isTimeout, int64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasIndex(globalObject, value)) {
        JSValue index = toIndex(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        auto converted = tryInt64(index);
        if (!converted || __builtin_mul_overflow(*converted, timeNanosecondsPerSecond, &result)) {
            timeRaiseOutOfRange(globalObject, scope);
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
    double nanoseconds = seconds * static_cast<double>(timeNanosecondsPerSecond);
    nanoseconds = isTimeout && nanoseconds >= 0 ? std::ceil(nanoseconds) : std::floor(nanoseconds);
    if (!(nanoseconds >= -0x1p63 && nanoseconds < 0x1p63)) {
        timeRaiseOutOfRange(globalObject, scope);
        return false;
    }
    result = static_cast<int64_t>(nanoseconds);
    return true;
}

static struct timespec timeToTimespec(int64_t nanoseconds)
{
    int64_t seconds = nanoseconds / timeNanosecondsPerSecond;
    int64_t rest = nanoseconds % timeNanosecondsPerSecond;
    if (rest < 0) {
        rest += timeNanosecondsPerSecond;
        --seconds;
    }
    struct timespec result;
    result.tv_sec = seconds;
    result.tv_nsec = rest;
    return result;
}

// ---- Clocks ----

static ALWAYS_INLINE double timeSecondsOf(const struct timespec& time) { return time.tv_sec + time.tv_nsec * 1e-9; }

static double timeSecondsOf(int64_t nanoseconds)
{
    // 1e-9 is not a double, so whole seconds are divided as ints.
    if (!(nanoseconds % timeNanosecondsPerSecond))
        return static_cast<double>(nanoseconds / timeNanosecondsPerSecond);
    return static_cast<double>(nanoseconds) / 1e9;
}

// False: an exception.
static bool timeReadClock(JSGlobalObject* globalObject, clockid_t clock, int64_t& nanoseconds)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct timespec time;
    if (clock_gettime(clock, &time)) {
        raiseOSError(globalObject, scope, errno);
        return false;
    }
    if (__builtin_mul_overflow(static_cast<int64_t>(time.tv_sec), timeNanosecondsPerSecond, &nanoseconds) || __builtin_add_overflow(nanoseconds, static_cast<int64_t>(time.tv_nsec), &nanoseconds)) {
        timeRaiseOutOfRange(globalObject, scope);
        return false;
    }
    return true;
}

template<clockid_t clock>
static JSValue timeClockSeconds(JSGlobalObject* globalObject)
{
    int64_t nanoseconds;
    if (!timeReadClock(globalObject, clock, nanoseconds))
        return { };
    return makeFloat(globalObject, timeSecondsOf(nanoseconds));
}

template<clockid_t clock>
static JSValue timeClockNanoseconds(JSGlobalObject* globalObject)
{
    int64_t nanoseconds;
    if (!timeReadClock(globalObject, clock, nanoseconds))
        return { };
    return makeInt(globalObject, nanoseconds);
}

// False: a TypeError, whatever was wrong.
static bool timeGetClockID(JSGlobalObject* globalObject, JSValue value, clockid_t& clock)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int id = toCInt(globalObject, value);
    if (scope.exception()) [[unlikely]] {
        takePendingException(globalObject);
        raiseTypeError(globalObject, scope, "clk_id should be integer, not "_s, typeName(globalObject, value));
        return false;
    }
    clock = static_cast<clockid_t>(id);
    return true;
}

static JSValue timeClockGetTime(JSGlobalObject* globalObject, JSValue id)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    clockid_t clock;
    if (!timeGetClockID(globalObject, id, clock))
        return { };
    struct timespec time;
    if (clock_gettime(clock, &time))
        return raiseOSError(globalObject, scope, errno);
    return makeFloat(globalObject, timeSecondsOf(time));
}

static JSValue timeClockGetTimeNs(JSGlobalObject* globalObject, JSValue id)
{
    clockid_t clock;
    if (!timeGetClockID(globalObject, id, clock))
        return { };
    int64_t nanoseconds;
    if (!timeReadClock(globalObject, clock, nanoseconds))
        return { };
    return makeInt(globalObject, nanoseconds);
}

static JSValue timeSetClock(JSGlobalObject* globalObject, const CallArgs& args, bool isNanoseconds)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!timeCheckArguments(globalObject, "clock_settime"_s, args, 2, 2))
        return { };
    int id = timeGetCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t nanoseconds;
    if (isNanoseconds) {
        JSValue index = toIndex(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        auto converted = tryInt64(index);
        if (!converted)
            return raiseOverflowError(globalObject, scope, "Python int too large to convert to C int64_t"_s);
        nanoseconds = *converted;
    } else if (!timeGetNanoseconds(globalObject, args[1], false, nanoseconds))
        return { };
    struct timespec time = timeToTimespec(nanoseconds);
    if (clock_settime(static_cast<clockid_t>(id), &time))
        return raiseOSError(globalObject, scope, errno);
    return none();
}

static JSValue timeClockSetTime(JSGlobalObject* globalObject, const CallArgs& args) { return timeSetClock(globalObject, args, false); }
static JSValue timeClockSetTimeNs(JSGlobalObject* globalObject, const CallArgs& args) { return timeSetClock(globalObject, args, true); }

static JSValue timeClockGetRes(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!timeCheckArguments(globalObject, "clock_getres"_s, args, 1, 1))
        return { };
    int id = timeGetCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    struct timespec resolution;
    if (clock_getres(static_cast<clockid_t>(id), &resolution))
        return raiseOSError(globalObject, scope, errno);
    return makeFloat(globalObject, timeSecondsOf(resolution));
}

#if !OS(DARWIN)
static JSValue timePthreadGetCpuClockID(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!timeCheckArguments(globalObject, "pthread_getcpuclockid"_s, args, 1, 1))
        return { };
    if (!hasIndex(globalObject, args[0]))
        return raiseTypeError(globalObject, scope, "pthread_getcpuclockid() argument 1 must be int, not "_s, typeNameOfArgument(globalObject, args[0]));
    JSValue index = toIndex(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    // The low 64 bits.
    uint64_t thread;
    if (auto converted = tryInt64(index))
        thread = *converted;
    else if (auto unsignedValue = tryUInt64(index))
        thread = *unsignedValue;
    else {
        warn(globalObject, runtime(globalObject).DeprecationWarning(), "integer value out of range"_s);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue masked = intAnd(globalObject, index, makeInt(globalObject, std::numeric_limits<uint64_t>::max()));
        RETURN_IF_EXCEPTION(scope, { });
        thread = tryUInt64(masked).value_or(0);
    }
    if (!runtime(globalObject).threads().hasThread(thread))
        return raiseOSError(globalObject, scope, ESRCH);
    clockid_t clock;
    if (int error = pthread_getcpuclockid(static_cast<pthread_t>(thread), &clock))
        return raiseOSError(globalObject, scope, error);
    return jsNumber(static_cast<int32_t>(clock));
}
#endif

static JSValue timeGetClockInfo(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!timeCheckArguments(globalObject, "get_clock_info"_s, args, 1, 1))
        return { };
    if (!isStr(args[0]))
        return raiseTypeError(globalObject, scope, "get_clock_info() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, args[0]));
    String name = strValue(args[0])->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (name.contains(static_cast<char16_t>(0)))
        return raiseValueError(globalObject, scope, "embedded null character"_s);

    static constexpr struct {
        ASCIILiteral name;
        ASCIILiteral implementation;
        clockid_t clock;
    } clocks[] = {
        { "time"_s, "clock_gettime(CLOCK_REALTIME)"_s, CLOCK_REALTIME },
        { "monotonic"_s, "clock_gettime(CLOCK_MONOTONIC)"_s, CLOCK_MONOTONIC },
        { "perf_counter"_s, "clock_gettime(CLOCK_MONOTONIC)"_s, CLOCK_MONOTONIC },
        { "process_time"_s, "clock_gettime(CLOCK_PROCESS_CPUTIME_ID)"_s, CLOCK_PROCESS_CPUTIME_ID },
        { "thread_time"_s, "clock_gettime(CLOCK_THREAD_CPUTIME_ID)"_s, CLOCK_THREAD_CPUTIME_ID },
    };
    for (auto& entry : clocks) {
        if (name != entry.name)
            continue;
        bool isWallClock = entry.clock == CLOCK_REALTIME;
        struct timespec resolution;
        if (clock_getres(entry.clock, &resolution)) {
            if (!isWallClock)
                return raiseOSError(globalObject, scope, errno);
            resolution = timeToTimespec(1);
        }
        PyDict* info = PyDict::create(globalObject);
        info->set(vm, internString(vm, "implementation"_s), makeStr(vm, entry.implementation));
        info->set(vm, internString(vm, "monotonic"_s), jsBoolean(!isWallClock));
        info->set(vm, internString(vm, "adjustable"_s), jsBoolean(isWallClock));
        info->set(vm, internString(vm, "resolution"_s), makeFloat(globalObject, timeSecondsOf(resolution)));
        RELEASE_AND_RETURN(scope, makeSimpleNamespace(globalObject, info));
    }
    return raiseValueError(globalObject, scope, "unknown clock"_s);
}

static JSValue timeSleep(JSGlobalObject* globalObject, JSValue seconds)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!audit(globalObject, "time.sleep"_s, seconds))
        return { };
    int64_t timeout;
    if (!timeGetNanoseconds(globalObject, seconds, true, timeout))
        return { };
    if (timeout < 0)
        return raiseValueError(globalObject, scope, "sleep length must be non-negative"_s);
    int64_t now;
    if (!timeReadClock(globalObject, CLOCK_MONOTONIC, now))
        return { };
    int64_t deadline;
    if (__builtin_add_overflow(now, timeout, &deadline))
        deadline = std::numeric_limits<int64_t>::max();

    // A signal does not cut the sleep short, unless its handler raises.
    while (true) {
        int error;
        {
            AllowThreads allow(globalObject);
#if OS(LINUX)
            struct timespec until = timeToTimespec(deadline);
            error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr);
#else
            struct timespec remaining = timeToTimespec(timeout);
            error = nanosleep(&remaining, nullptr) ? errno : 0;
#endif
        }
        if (!error)
            return none();
        if (error != EINTR)
            return raiseOSError(globalObject, scope, error);
        if (!checkSignals(globalObject))
            return { };
#if !OS(LINUX)
        if (!timeReadClock(globalObject, CLOCK_MONOTONIC, now))
            return { };
        timeout = deadline - now;
        if (timeout < 0)
            return none();
#endif
    }
}

// ---- struct_time ----

static constexpr ASCIILiteral timeStructFields[] = {
    "tm_year"_s, "tm_mon"_s, "tm_mday"_s, "tm_hour"_s, "tm_min"_s, "tm_sec"_s, "tm_wday"_s, "tm_yday"_s, "tm_isdst"_s, "tm_zone"_s, "tm_gmtoff"_s
};
static constexpr StructSeqDef timeStructDef { "time.struct_time"_s, timeStructFields, 9 };

// Never seen from Python: the runtime keeps a type for each definition, and the dict of this one is where struct_time is kept,
// out of reach of what is done to the module.
static constexpr TypeDef timeStateTypeDef { "time._state"_s, { TypeFlag::DisallowInstantiation }, { }, { }, { } };

static PyType* timeStructType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyDict* state = rt.typeFor(timeStateTypeDef)->dict();
    JSString* key = internString(vm, "struct_time"_s);
    if (JSValue type = state->get(key))
        return PyType::from(type);
    PyType* type = makeStructSeqType(globalObject, timeStructDef);
    state->set(vm, key, type);
    return type;
}

static JSValue timeMakeStruct(JSGlobalObject* globalObject, const struct tm& tm)
{
    JSValue zone = tm.tm_zone ? JSValue(makeStr(globalObject, decodeFilesystem(byteCast<uint8_t>(unsafeSpan(tm.tm_zone))))) : none();
    return makeStructSeq(globalObject, timeStructType(globalObject), {
        makeInt(globalObject, static_cast<int64_t>(tm.tm_year) + 1900), jsNumber(tm.tm_mon + 1), jsNumber(tm.tm_mday),
        jsNumber(tm.tm_hour), jsNumber(tm.tm_min), jsNumber(tm.tm_sec),
        jsNumber((tm.tm_wday + 6) % 7), jsNumber(tm.tm_yday + 1), jsNumber(tm.tm_isdst),
        zone, makeInt(globalObject, static_cast<int64_t>(tm.tm_gmtoff)) });
}

// False: an OSError.
static bool timeToLocal(JSGlobalObject* globalObject, time_t when, struct tm& tm)
{
    errno = 0;
    if (localtime_r(&when, &tm))
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseOSError(globalObject, scope, errno ? errno : EINVAL);
    return false;
}

// The optional argument of gmtime(), localtime() and ctime(): None, like none at all, is now.
static bool timeGetWhen(JSGlobalObject* globalObject, ASCIILiteral function, const CallArgs& args, time_t& when)
{
    if (!timeCheckArguments(globalObject, function, args, 0, 1))
        return false;
    if (args.isEmpty() || isNone(args[0])) {
        when = time(nullptr);
        return true;
    }
    return timeGetSeconds(globalObject, args[0], when);
}

static JSValue timeGmTime(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    time_t when;
    if (!timeGetWhen(globalObject, "gmtime"_s, args, when))
        return { };
    struct tm tm;
    errno = 0;
    if (!gmtime_r(&when, &tm))
        return raiseOSError(globalObject, scope, errno ? errno : EINVAL);
    RELEASE_AND_RETURN(scope, timeMakeStruct(globalObject, tm));
}

static JSValue timeLocalTime(JSGlobalObject* globalObject, const CallArgs& args)
{
    time_t when;
    struct tm tm;
    if (!timeGetWhen(globalObject, "localtime"_s, args, when) || !timeToLocal(globalObject, when, tm))
        return { };
    return timeMakeStruct(globalObject, tm);
}

namespace {

struct TimeFields {
    struct tm tm;
    UTF8CString zone; // What tm.tm_zone points to.
};

}

// From a tuple of nine ints, as struct_time is. False: an exception.
static bool timeGetFields(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, TimeFields& fields)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct tm& tm = fields.tm;
    zeroBytes(tm);
    PyTuple* tuple = PyTuple::tryFrom(value);
    if (!tuple) {
        raiseTypeError(globalObject, scope, "Tuple or struct_time argument required"_s);
        return false;
    }
    if (tuple->length() != 9) {
        raiseTypeError(globalObject, scope, function, "(): illegal time tuple argument"_s);
        return false;
    }
    int year;
    int* const targets[] = { &year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &tm.tm_wday, &tm.tm_yday, &tm.tm_isdst };
    for (unsigned i = 0; i < 9; ++i) {
        *targets[i] = timeGetCInt(globalObject, tuple->at(i));
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (year < std::numeric_limits<int>::min() + 1900) {
        raiseOverflowError(globalObject, scope, "year out of range"_s);
        return false;
    }
    tm.tm_year = year - 1900;
    tm.tm_mon--;
    tm.tm_wday = (tm.tm_wday + 1) % 7;
    tm.tm_yday--;

    if (tuple->type() == timeStructType(globalObject)) {
        JSValue zone = structSeqField(globalObject, tuple, 9);
        if (!isNone(zone)) {
            if (!isStr(zone)) {
                raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
                return false;
            }
            String text = strValue(zone)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            fields.zone = text.utf8();
            tm.tm_zone = const_cast<char*>(fields.zone.legacyCStringPointer()); // Not const everywhere.
        }
        JSValue offset = structSeqField(globalObject, tuple, 10);
        if (!isNone(offset)) {
            tm.tm_gmtoff = toInt64(globalObject, offset);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
    return true;
}

// Before the fields index the tables of names of strftime() and asctime(). A zero where the count starts at one is taken for
// one. False: a ValueError.
static bool timeCheckFields(JSGlobalObject* globalObject, struct tm& tm)
{
    ASCIILiteral wrong;
    if (tm.tm_mon == -1)
        tm.tm_mon = 0;
    else if (tm.tm_mon < 0 || tm.tm_mon > 11)
        wrong = "month"_s;
    if (!tm.tm_mday)
        tm.tm_mday = 1;
    else if (wrong.isNull() && (tm.tm_mday < 0 || tm.tm_mday > 31))
        wrong = "day of month"_s;
    if (wrong.isNull()) {
        if (tm.tm_hour < 0 || tm.tm_hour > 23)
            wrong = "hour"_s;
        else if (tm.tm_min < 0 || tm.tm_min > 59)
            wrong = "minute"_s;
        else if (tm.tm_sec < 0 || tm.tm_sec > 61)
            wrong = "seconds"_s;
        else if (tm.tm_wday < 0)
            wrong = "day of week"_s;
        else if (tm.tm_yday == -1)
            tm.tm_yday = 0;
        else if (tm.tm_yday < 0 || tm.tm_yday > 365)
            wrong = "day of year"_s;
    }
    if (wrong.isNull())
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, wrong, " out of range"_s);
    return false;
}

// 'Sat Jun  6 16:26:11 1998', in English whatever the locale.
static JSValue timeFormatAsC(JSGlobalObject* globalObject, const struct tm& tm)
{
    static constexpr const char* days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static constexpr const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char buffer[64];
    int length = snprintf(buffer, sizeof(buffer), "%s %s%3d %.2d:%.2d:%.2d %d", days[tm.tm_wday], months[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, 1900 + tm.tm_year);
    return makeStr(globalObject, String(byteCast<Latin1Character>(std::span<const char> { buffer, static_cast<size_t>(length) })));
}

static JSValue timeAscTime(JSGlobalObject* globalObject, const CallArgs& args)
{
    if (!rejectKeywords(globalObject, "asctime"_s, args) || !checkPositional(globalObject, "asctime"_s, args, 0, 1))
        return { };
    TimeFields fields;
    if (args.isEmpty()) {
        if (!timeToLocal(globalObject, time(nullptr), fields.tm))
            return { };
    } else if (!timeGetFields(globalObject, args[0], "asctime"_s, fields) || !timeCheckFields(globalObject, fields.tm))
        return { };
    return timeFormatAsC(globalObject, fields.tm);
}

static JSValue timeCTime(JSGlobalObject* globalObject, const CallArgs& args)
{
    time_t when;
    struct tm tm;
    if (!timeGetWhen(globalObject, "ctime"_s, args, when) || !timeToLocal(globalObject, when, tm))
        return { };
    return timeFormatAsC(globalObject, tm);
}

static JSValue timeMkTime(JSGlobalObject* globalObject, JSValue tuple)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    TimeFields fields;
    if (!timeGetFields(globalObject, tuple, "mktime"_s, fields))
        return { };
    // -1 may be a time like any other; but then mktime() has worked out the day of the week.
    fields.tm.tm_wday = -1;
    time_t result = mktime(&fields.tm);
    if (result == static_cast<time_t>(-1) && fields.tm.tm_wday == -1)
        return raiseOverflowError(globalObject, scope, "mktime argument out of range"_s);
    return makeFloat(globalObject, static_cast<double>(result));
}

static JSValue timeStrfTime(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!timeCheckArguments(globalObject, "strftime"_s, args, 1, 2))
        return { };
    if (!isStr(args[0]))
        return raiseTypeError(globalObject, scope, "strftime() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, args[0]));
    TimeFields fields;
    if (args.size() < 2) {
        if (!timeToLocal(globalObject, time(nullptr), fields.tm))
            return { };
    } else if (!timeGetFields(globalObject, args[1], "strftime"_s, fields) || !timeCheckFields(globalObject, fields.tm))
        return { };
    fields.tm.tm_isdst = std::clamp(fields.tm.tm_isdst, -1, 1);

    String format = strValue(args[0])->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned formatLength = format.length();

    // wcsftime(), as in CPython: in glibc it does not treat the flags of %Z quite as strftime() does. It reads tm_zone in the
    // encoding of the locale, so for a name that is not ASCII, which is in UTF-8 here, it is given a locale where that is it.
    locale_t utf8Locale = nullptr;
    if (!charactersAreAllASCII(fields.zone.span())) {
        locale_t base = duplocale(LC_GLOBAL_LOCALE);
#if OS(DARWIN)
        utf8Locale = newlocale(LC_CTYPE_MASK, "UTF-8", base);
#else
        utf8Locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", base);
#endif
        if (!utf8Locale && base)
            freelocale(base);
    }
    auto freeLocale = makeScopeExit([&] {
        if (utf8Locale)
            freelocale(utf8Locale);
    });

    // The C function is given the stretches that are ASCII. What is not, or is a NUL, is copied, with what follows up to the
    // next directive.
    StrBuilder result;
    Vector<wchar_t, 64> stretch;
    Vector<wchar_t, 1024> buffer(1024);
    unsigned i = 0;
    while (i < formatLength) {
        stretch.shrink(0);
        for (; i < formatLength && format[i] && format[i] <= 127; ++i)
            stretch.append(static_cast<wchar_t>(format[i]));
        if (size_t stretchLength = stretch.size()) {
            stretch.append(L'\0');
            size_t length;
            while (true) {
                wchar_t* out = buffer.mutableSpan().data();
                const wchar_t* in = stretch.span().data();
                length = utf8Locale ? wcsftime_l(out, buffer.size(), in, &fields.tm, utf8Locale) : wcsftime(out, buffer.size(), in, &fields.tm);
                // 0 says there was no room; or that the result is empty, which is the likelier with this much room.
                if (length || buffer.size() >= 256 * stretchLength)
                    break;
                buffer.grow(buffer.size() * 2);
            }
            for (wchar_t character : buffer.span().first(length))
                result.append(static_cast<char32_t>(character));
        }
        unsigned start = i;
        while (i < formatLength && format[i] != '%')
            ++i;
        result.append(StringView(format).substring(start, i - start));
    }
    return makeStr(globalObject, result);
}

static JSValue timeStrpTime(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!rejectKeywords(globalObject, "strptime"_s, args))
        return { };
    JSValue module = importModule(globalObject, "_strptime"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue function = getAttr(globalObject, module, internString(vm, "_strptime_time"_s));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, function, args));
}

// ---- Time zones ----

// timezone, altzone, daylight and tzname, from the offsets in force in January and in July of this year.
static void timeInitializeZones(JSGlobalObject* globalObject, PyModule* module)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr time_t year = (365 * 24 + 6) * 3600;
    struct Zone {
        int64_t offset; // West of UTC.
        JSValue name;
    };
    auto zoneAt = [&](time_t when) -> Zone {
        struct tm tm;
        zeroBytes(tm);
        localtime_r(&when, &tm);
        auto name = unsafeSpan(tm.tm_zone ? tm.tm_zone : "   ");
        return { -static_cast<int64_t>(tm.tm_gmtoff), makeStr(vm, String::fromUTF8(name.first(std::min<size_t>(name.size(), 9)))) };
    };
    time_t start = time(nullptr) / year * year;
    Zone january = zoneAt(start);
    Zone july = zoneAt(start + year / 2);
    static constexpr int64_t maxOffset = 48 * 3600;
    if (std::abs(january.offset) > maxOffset || std::abs(july.offset) > maxOffset) {
        raiseRuntimeError(globalObject, scope, "invalid GMT offset"_s);
        return;
    }
    // In the southern hemisphere, it is in January that the clocks are ahead.
    bool isSouthern = january.offset < july.offset;
    Zone& standard = isSouthern ? july : january;
    Zone& summer = isSouthern ? january : july;
    module->add(globalObject, "timezone"_s, jsNumber(static_cast<int32_t>(standard.offset)));
    module->add(globalObject, "altzone"_s, jsNumber(static_cast<int32_t>(summer.offset)));
    module->add(globalObject, "daylight"_s, jsNumber(january.offset != july.offset));
    module->add(globalObject, "tzname"_s, PyTuple::create(globalObject, { standard.name, summer.name }));
}

static JSValue timeTzSet(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, "time"_s);
    RETURN_IF_EXCEPTION(scope, { });
    tzset();
    if (auto* timeModule = PyModule::tryFrom(module)) {
        timeInitializeZones(globalObject, timeModule);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

// ---- The module ----

static constexpr NativeDef timeFunctions[] = {
    PY_FUNCTION("time", timeClockSeconds<CLOCK_REALTIME>, "()"),
    PY_FUNCTION("time_ns", timeClockNanoseconds<CLOCK_REALTIME>, "()"),
    PY_FUNCTION("clock_gettime", timeClockGetTime, "(clk_id, /)"),
    PY_FUNCTION("clock_gettime_ns", timeClockGetTimeNs, "(clk_id, /)"),
    PY_FUNCTION("clock_settime", timeClockSetTime, "(*args)"),
    PY_FUNCTION("clock_settime_ns", timeClockSetTimeNs, "(*args)"),
    PY_FUNCTION("clock_getres", timeClockGetRes, "(*args)"),
#if !OS(DARWIN)
    PY_FUNCTION("pthread_getcpuclockid", timePthreadGetCpuClockID, "(*args)"),
#endif
    PY_FUNCTION("sleep", timeSleep, "(seconds, /)"),
    PY_FUNCTION("gmtime", timeGmTime, "(*args)"),
    PY_FUNCTION("localtime", timeLocalTime, "(*args)"),
    PY_FUNCTION("asctime", timeAscTime, "(*args)"),
    PY_FUNCTION("ctime", timeCTime, "(*args)"),
    PY_FUNCTION("mktime", timeMkTime, "(time_tuple, /)"),
    PY_FUNCTION("strftime", timeStrfTime, "(*args)"),
    PY_FUNCTION("strptime", timeStrpTime, "(*args)"),
    PY_FUNCTION("tzset", timeTzSet, "()"),
    PY_FUNCTION("monotonic", timeClockSeconds<CLOCK_MONOTONIC>, "()"),
    PY_FUNCTION("monotonic_ns", timeClockNanoseconds<CLOCK_MONOTONIC>, "()"),
    PY_FUNCTION("process_time", timeClockSeconds<CLOCK_PROCESS_CPUTIME_ID>, "()"),
    PY_FUNCTION("process_time_ns", timeClockNanoseconds<CLOCK_PROCESS_CPUTIME_ID>, "()"),
    PY_FUNCTION("thread_time", timeClockSeconds<CLOCK_THREAD_CPUTIME_ID>, "()"),
    PY_FUNCTION("thread_time_ns", timeClockNanoseconds<CLOCK_THREAD_CPUTIME_ID>, "()"),
    PY_FUNCTION("perf_counter", timeClockSeconds<CLOCK_MONOTONIC>, "()"),
    PY_FUNCTION("perf_counter_ns", timeClockNanoseconds<CLOCK_MONOTONIC>, "()"),
    PY_FUNCTION("get_clock_info", timeGetClockInfo, "(*args)"),
};

namespace {

struct TimeClockConstant {
    ASCIILiteral name;
    int value;
};

}

static constexpr TimeClockConstant timeClockConstants[] = {
    { "CLOCK_REALTIME"_s, CLOCK_REALTIME },
    { "CLOCK_MONOTONIC"_s, CLOCK_MONOTONIC },
#ifdef CLOCK_MONOTONIC_RAW
    { "CLOCK_MONOTONIC_RAW"_s, CLOCK_MONOTONIC_RAW },
#endif
#ifdef CLOCK_HIGHRES
    { "CLOCK_HIGHRES"_s, CLOCK_HIGHRES },
#endif
    { "CLOCK_PROCESS_CPUTIME_ID"_s, CLOCK_PROCESS_CPUTIME_ID },
    { "CLOCK_THREAD_CPUTIME_ID"_s, CLOCK_THREAD_CPUTIME_ID },
#ifdef CLOCK_PROF
    { "CLOCK_PROF"_s, CLOCK_PROF },
#endif
#ifdef CLOCK_BOOTTIME
    { "CLOCK_BOOTTIME"_s, CLOCK_BOOTTIME },
#endif
#ifdef CLOCK_TAI
    { "CLOCK_TAI"_s, CLOCK_TAI },
#endif
#ifdef CLOCK_UPTIME
    { "CLOCK_UPTIME"_s, CLOCK_UPTIME },
#endif
#ifdef CLOCK_UPTIME_RAW
    { "CLOCK_UPTIME_RAW"_s, CLOCK_UPTIME_RAW },
#endif
#ifdef CLOCK_MONOTONIC_RAW_APPROX
    { "CLOCK_MONOTONIC_RAW_APPROX"_s, CLOCK_MONOTONIC_RAW_APPROX },
#endif
#ifdef CLOCK_UPTIME_RAW_APPROX
    { "CLOCK_UPTIME_RAW_APPROX"_s, CLOCK_UPTIME_RAW_APPROX },
#endif
};

void initializeTimeModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    module->dict()->set(vm, rt.names.dDoc(), makeStr(vm, "This module provides various functions to manipulate time values.\n\n"
        "There are two standard representations of time.  One is the number\n"
        "of seconds since the Epoch, in UTC (a.k.a. GMT).  It may be an integer\n"
        "or a floating-point number (to represent fractions of seconds).\n"
        "The epoch is the point where the time starts, the return value of time.gmtime(0).\n"
        "It is January 1, 1970, 00:00:00 (UTC) on all platforms.\n\n"
        "The other representation is a tuple of 9 integers giving local time.\n"
        "The tuple items are:\n"
        "  year (including century, e.g. 1998)\n"
        "  month (1-12)\n"
        "  day (1-31)\n"
        "  hours (0-23)\n"
        "  minutes (0-59)\n"
        "  seconds (0-59)\n"
        "  weekday (0-6, Monday is 0)\n"
        "  Julian day (day in the year, 1-366)\n"
        "  DST (Daylight Savings Time) flag (-1, 0 or 1)\n"
        "If the DST flag is 0, the time is given in the regular time zone;\n"
        "if it is 1, the time is given in the DST time zone;\n"
        "if it is -1, mktime() should guess based on the date and time.\n"_s));
    module->addFunctions(globalObject, timeFunctions);
    timeInitializeZones(globalObject, module);
    RETURN_IF_EXCEPTION(scope, void());
    for (auto& constant : timeClockConstants)
        module->add(globalObject, constant.name, jsNumber(constant.value));
    module->add(globalObject, "_STRUCT_TM_ITEMS"_s, jsNumber(11));
    module->add(globalObject, "struct_time"_s, timeStructType(globalObject));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
