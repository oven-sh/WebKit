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
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonCodecs.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonLocale.h"
#include "PythonOperations.h"
#include "PythonText.h"
#include "PythonTime.h"
#include <sys/resource.h>
#include <wchar.h>

// The module time: Modules/timemodule.c of CPython.

namespace JSC { namespace Python {

namespace {

struct TimeModuleState final : NativeState {
    PYTHON_NATIVE_STATE(TimeModuleState);
    WriteBarrier<PyType> structTime;
};

template<typename Visitor> void TimeModuleState::visit(Visitor& visitor) { visitor.append(structTime); }

TimeModuleState& timeState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<TimeModuleState>(); }

// PyUnicode_DecodeLocale(), with surrogateescape. Empty if it raised.
JSValue decodeLocale(JSGlobalObject* globalObject, const char* text)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String decoded = decodeBytes(globalObject, byteCast<uint8_t>(unsafeSpan(text)), "utf-8"_s, "surrogateescape"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, decoded));
}

enum class Clock : uint8_t { Time, Monotonic, ProcessTime, ThreadTime };

// Of a clock that clock_gettime() has. Nothing if it raised.
std::optional<int64_t> readClock(JSGlobalObject* globalObject, clockid_t clock, ASCIILiteral implementation, ClockInfo* info)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    struct timespec now;
    if (clock_gettime(clock, &now)) {
        raiseOSError(globalObject, scope, errno);
        return std::nullopt;
    }
    if (info) {
        struct timespec resolution;
        if (clock_getres(clock, &resolution)) {
            raiseOSError(globalObject, scope, errno);
            return std::nullopt;
        }
        *info = { implementation, true, false, timespecAsSeconds(resolution) };
    }
    RELEASE_AND_RETURN(scope, timeFromTimespec(globalObject, now));
}

std::optional<int64_t> read(JSGlobalObject* globalObject, Clock clock, ClockInfo* info = nullptr)
{
    switch (clock) {
    case Clock::Time:
        return systemClock(globalObject, info);
    case Clock::Monotonic:
        return monotonicClock(globalObject, info);
    case Clock::ProcessTime:
        // py_process_time()
        return readClock(globalObject, CLOCK_PROCESS_CPUTIME_ID, "clock_gettime(CLOCK_PROCESS_CPUTIME_ID)"_s, info);
    case Clock::ThreadTime:
        // _PyTime_GetThreadTimeWithInfo()
        return readClock(globalObject, CLOCK_THREAD_CPUTIME_ID, "clock_gettime(CLOCK_THREAD_CPUTIME_ID)"_s, info);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// time_clockid_converter(). Nothing if it raised.
std::optional<clockid_t> toClockID(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto id = toCInt(globalObject, value);
    if (scope.exception()) [[unlikely]] {
        // Whatever the matter was
        if (catchException(globalObject, BuiltinType::BaseException))
            raiseTypeError(globalObject, scope, concatenate("clk_id should be integer, not "_s, typeOf(globalObject, value)->nameWithoutModule(globalObject)));
        return std::nullopt;
    }
    return static_cast<clockid_t>(*id);
}

// tmtotuple()
JSValue toStructTime(JSGlobalObject* globalObject, const struct tm& time)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    MarkedArgumentBuffer values;
    values.append(jsNumber(time.tm_year + 1900));
    values.append(jsNumber(time.tm_mon + 1)); // January is 1.
    values.append(jsNumber(time.tm_mday));
    values.append(jsNumber(time.tm_hour));
    values.append(jsNumber(time.tm_min));
    values.append(jsNumber(time.tm_sec));
    values.append(jsNumber((time.tm_wday + 6) % 7)); // Monday is 0.
    values.append(jsNumber(time.tm_yday + 1)); // The first of January is 1.
    values.append(jsNumber(time.tm_isdst));
    values.append(decodeLocale(globalObject, time.tm_zone));
    RETURN_IF_EXCEPTION(scope, { });
    values.append(intFromInt64(globalObject, time.tm_gmtoff));
    RELEASE_AND_RETURN(scope, newStructSequence(globalObject, timeState(globalObject).structTime.get(), values));
}

// _PyTime_localtime() and _PyTime_gmtime(). False if it raised.
bool breakDown(JSGlobalObject* globalObject, time_t when, bool isLocal, struct tm& result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    errno = 0;
    if (isLocal ? localtime_r(&when, &result) : gmtime_r(&when, &result))
        return true;
    raiseOSError(globalObject, scope, errno ? errno : EINVAL);
    return false;
}

// parse_time_t_args(), of what is left of the arguments once there are known to be no more than one. Nothing if it raised.
std::optional<time_t> toWhen(JSGlobalObject* globalObject, JSValue value)
{
    if (!value || isNone(value))
        return ::time(nullptr);
    return objectToTimeT(globalObject, value, TimeRounding::Floor);
}

// What a struct tm points to, for as long as the struct tm is in use.
struct BrokenDownTime {
    struct tm tm { };
    CString zone;
};

// gettmarg(): from a tuple of nine. False if it raised.
bool toBrokenDownTime(JSGlobalObject* globalObject, JSValue given, BrokenDownTime& result, ASCIILiteral function)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isTuple(given)) {
        raiseTypeError(globalObject, scope, "Tuple or struct_time argument required"_s);
        return false;
    }
    PyTuple* tuple = asTuple(given);
    if (tuple->length() != 9) {
        raiseTypeError(globalObject, scope, concatenate(function, "(): illegal time tuple argument"_s));
        return false;
    }
    std::array<int, 9> fields;
    for (unsigned i = 0; i < 9; ++i) {
        auto field = toCIntOfFormat(globalObject, tuple->at(i));
        RETURN_IF_EXCEPTION(scope, false);
        fields[i] = *field;
    }
    if (fields[0] < std::numeric_limits<int>::min() + 1900) {
        raise(globalObject, scope, BuiltinType::OverflowError, "year out of range"_s);
        return false;
    }
    struct tm& tm = result.tm;
    tm.tm_year = fields[0] - 1900;
    tm.tm_mon = static_cast<int>(static_cast<unsigned>(fields[1]) - 1);
    tm.tm_mday = fields[2];
    tm.tm_hour = fields[3];
    tm.tm_min = fields[4];
    tm.tm_sec = fields[5];
    tm.tm_wday = static_cast<int>(static_cast<unsigned>(fields[6]) + 1) % 7;
    tm.tm_yday = static_cast<int>(static_cast<unsigned>(fields[7]) - 1);
    tm.tm_isdst = fields[8];
    if (typeOf(globalObject, given) != timeState(globalObject).structTime.get())
        return true;
    PyTuple* hidden = asTuple(tuple->getDirect(vm, vm.pythonNames().private_hiddenFields));
    if (JSValue zone = hidden->at(0); !isNone(zone)) {
        // PyUnicode_AsUTF8()
        if (!stringIn(zone)) {
            raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
            return false;
        }
        auto bytes = encodeString(globalObject, zone, "utf-8"_s, "strict"_s);
        RETURN_IF_EXCEPTION(scope, false);
        result.zone = CString(byteCast<char>(bytes->span()));
        tm.tm_zone = const_cast<char*>(result.zone.data());
    }
    if (JSValue offset = hidden->at(1); !isNone(offset)) {
        auto seconds = toCLong(globalObject, offset);
        RETURN_IF_EXCEPTION(scope, false);
        tm.tm_gmtoff = *seconds;
    }
    return true;
}

// checktm(): that nothing is asked of strftime() and asctime() that they would look up in a table that has no such place. False if it raised.
bool checkBrokenDownTime(JSGlobalObject* globalObject, struct tm& tm)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto fail = [&] (ASCIILiteral message) {
        raiseValueError(globalObject, scope, message);
        return false;
    };
    // Nought is let pass for the least that there is.
    if (tm.tm_mon == -1)
        tm.tm_mon = 0;
    else if (tm.tm_mon < 0 || tm.tm_mon > 11)
        return fail("month out of range"_s);
    if (!tm.tm_mday)
        tm.tm_mday = 1;
    else if (tm.tm_mday < 0 || tm.tm_mday > 31)
        return fail("day of month out of range"_s);
    if (tm.tm_hour < 0 || tm.tm_hour > 23)
        return fail("hour out of range"_s);
    if (tm.tm_min < 0 || tm.tm_min > 59)
        return fail("minute out of range"_s);
    if (tm.tm_sec < 0 || tm.tm_sec > 61)
        return fail("seconds out of range"_s);
    if (tm.tm_wday < 0)
        return fail("day of week out of range"_s);
    if (tm.tm_yday == -1)
        tm.tm_yday = 0;
    else if (tm.tm_yday < 0 || tm.tm_yday > 365)
        return fail("day of year out of range"_s);
    return true;
}

// _asctime()
JSValue asctime(JSGlobalObject* globalObject, const struct tm& tm)
{
    static constexpr std::array<ASCIILiteral, 7> days { "Sun"_s, "Mon"_s, "Tue"_s, "Wed"_s, "Thu"_s, "Fri"_s, "Sat"_s };
    static constexpr std::array<ASCIILiteral, 12> months { "Jan"_s, "Feb"_s, "Mar"_s, "Apr"_s, "May"_s, "Jun"_s, "Jul"_s, "Aug"_s, "Sep"_s, "Oct"_s, "Nov"_s, "Dec"_s };
    auto twoDigits = [] (int value) { return makeString(value < 10 ? "0"_s : ""_s, value); };
    return jsString(globalObject->vm(), makeString(days[tm.tm_wday], ' ', months[tm.tm_mon], tm.tm_mday < 10 ? "  "_s : " "_s, tm.tm_mday, ' ', twoDigits(tm.tm_hour), ':', twoDigits(tm.tm_min), ':', twoDigits(tm.tm_sec), ' ', static_cast<int>(1900u + static_cast<unsigned>(tm.tm_year))));
}

// The struct tm that is meant by an optional tuple: now, if there is none. False if it raised.
bool toCheckedTime(JSGlobalObject* globalObject, JSValue given, BrokenDownTime& result, ASCIILiteral function)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!given)
        RELEASE_AND_RETURN(scope, breakDown(globalObject, ::time(nullptr), true, result.tm));
    toBrokenDownTime(globalObject, given, result, function);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, checkBrokenDownTime(globalObject, result.tm));
}

// PyArg_ParseTuple() and PyArg_UnpackTuple(), as far as how many there are. False if it raised.
bool checkCount(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, ASCIILiteral function, unsigned least, unsigned most, bool isUnpacked = false)
{
    if (!args.checkNoKeywords(globalObject, scope, function))
        return false;
    unsigned count = args.size();
    if (count >= least && count <= most)
        return true;
    if (isUnpacked) {
        raiseTypeError(globalObject, scope, concatenate(function, " expected at "_s, count < least ? "least "_s : "most "_s, count < least ? least : most, " argument"_s, (count < least ? least : most) == 1 ? ""_s : "s"_s, ", got "_s, count));
        return false;
    }
    unsigned limit = count < least ? least : most;
    raiseTypeError(globalObject, scope, concatenate(function, "() takes "_s, least == most ? "exactly "_s : count < least ? "at least "_s : "at most "_s, limit, " argument"_s, limit == 1 ? ""_s : "s"_s, " ("_s, count, " given)"_s));
    return false;
}

// init_timezone(): timezone, altzone, daylight and tzname, as they are now.
void initializeTimeZone(JSGlobalObject* globalObject, JSObject* module)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto add = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    // Where a struct tm says what zone it is in, CPython goes by what it says in January of this year and in July.
    constexpr time_t year = (365 * 24 + 6) * 3600;
    struct Sample {
        int64_t zone;
        std::array<char, 10> name { };
    };
    auto sample = [&] (time_t when) {
        Sample result;
        struct tm tm;
        localtime_r(&when, &tm);
        strncpy(result.name.data(), tm.tm_zone ? tm.tm_zone : "   ", 9);
        result.zone = -tm.tm_gmtoff;
        return result;
    };
    time_t start = (::time(nullptr) / year) * year;
    Sample january = sample(start);
    Sample july = sample(start + year / 2);
    constexpr int64_t furthest = 48 * 3600;
    if (std::abs(january.zone) > furthest || std::abs(july.zone) > furthest) {
        raise(globalObject, scope, BuiltinType::RuntimeError, "invalid GMT offset"_s);
        return;
    }
    // South of the equator it is the other way about.
    bool isSouthern = january.zone < july.zone;
    const Sample& standard = isSouthern ? july : january;
    const Sample& summer = isSouthern ? january : july;
    add("timezone"_s, intFromInt64(globalObject, standard.zone));
    add("altzone"_s, intFromInt64(globalObject, summer.zone));
    add("daylight"_s, jsNumber(january.zone != july.zone));
    // Py_BuildValue("(zz)"), which takes them for UTF-8
    auto text = [&] (const Sample& sample) { return jsString(vm, String::fromUTF8(sample.name.data())); };
    add("tzname"_s, PyTuple::create(globalObject, { text(standard), text(summer) }));
}

} // anonymous namespace

// time(), monotonic(), perf_counter(), process_time() and thread_time(), and each with _ns
PYTHON_NATIVE(timeRead)
{
    auto clock = unpack<Clock>(callFrame, 0);
    bool isInNanoseconds = unpack<bool>(callFrame, 1);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto now = read(globalObject, clock);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(isInNanoseconds ? intFromInt64(globalObject, *now) : floatFromDouble(timeAsSeconds(*now)));
}

// clock_gettime(clk_id, /) and clock_gettime_ns()
PYTHON_NATIVE(timeClockGetTime)
{
    bool isInNanoseconds = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto clock = toClockID(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    struct timespec now;
    if (clock_gettime(*clock, &now))
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    if (!isInNanoseconds)
        return JSValue::encode(floatFromDouble(timespecAsSeconds(now)));
    auto time = timeFromTimespec(globalObject, now);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, *time));
}

// clock_settime(clk_id, time) and clock_settime_ns()
PYTHON_NATIVE(timeClockSetTime)
{
    bool isInNanoseconds = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    // Both are called this where CPython looks through the arguments.
    if (!args.checkNoKeywords(globalObject, scope, isInNanoseconds ? "clock_settime_ns"_s : "clock_settime"_s) || !checkCount(globalObject, scope, args, "clock_settime"_s, 2, 2))
        return { };
    auto clock = toCIntOfFormat(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto time = isInNanoseconds ? timeFromNanosecondsObject(globalObject, args[1]) : timeFromSecondsObject(globalObject, args[1], TimeRounding::Floor);
    RETURN_IF_EXCEPTION(scope, { });
    struct timespec value;
    timeAsTimespec(globalObject, *time, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (clock_settime(static_cast<clockid_t>(*clock), &value))
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    RETURN_NONE();
}

// clock_getres(clk_id)
PYTHON_NATIVE(timeClockGetRes)
{
    NATIVE_PROLOGUE();
    if (!checkCount(globalObject, scope, args, "clock_getres"_s, 1, 1))
        return { };
    auto clock = toCIntOfFormat(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    struct timespec resolution;
    if (clock_getres(static_cast<clockid_t>(*clock), &resolution))
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    return JSValue::encode(floatFromDouble(timespecAsSeconds(resolution)));
}

// sleep(seconds), and pysleep()
PYTHON_NATIVE(timeSleep)
{
    NATIVE_PROLOGUE();
    if (!audit(globalObject, "time.sleep"_s, args[0]))
        return { };
    auto given = timeFromSecondsObject(globalObject, args[0], TimeRounding::Timeout);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t timeout = *given;
    if (timeout < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "sleep length must be non-negative"_s));
    auto start = monotonicClock(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    CheckedInt64 checkedDeadline = *start;
    checkedDeadline += timeout;
    int64_t deadline = checkedDeadline.hasOverflowed() ? std::numeric_limits<int64_t>::max() : checkedDeadline.value();
    while (true) {
        struct timespec remaining;
        timeAsTimespec(globalObject, timeout, remaining);
        if (!nanosleep(&remaining, nullptr))
            break;
        if (errno != EINTR)
            return JSValue::encode(raiseOSError(globalObject, scope, errno));
        // A signal has come. When it has been seen to, it goes on for what is left.
        checkSignals(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        auto now = monotonicClock(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        timeout = deadline - *now;
        if (timeout < 0)
            break;
    }
    RETURN_NONE();
}

// gmtime([seconds]) and localtime()
PYTHON_NATIVE(timeBreakDown)
{
    bool isLocal = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!checkCount(globalObject, scope, args, isLocal ? "localtime"_s : "gmtime"_s, 0, 1))
        return { };
    auto when = toWhen(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    struct tm tm;
    breakDown(globalObject, *when, isLocal, tm);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(toStructTime(globalObject, tm)));
}

// asctime([tuple])
PYTHON_NATIVE(timeAsctime)
{
    NATIVE_PROLOGUE();
    if (!checkCount(globalObject, scope, args, "asctime"_s, 0, 1, true))
        return { };
    BrokenDownTime time;
    toCheckedTime(globalObject, args.at(0), time, "asctime"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(asctime(globalObject, time.tm));
}

// ctime([seconds])
PYTHON_NATIVE(timeCtime)
{
    NATIVE_PROLOGUE();
    if (!checkCount(globalObject, scope, args, "ctime"_s, 0, 1))
        return { };
    auto when = toWhen(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    struct tm tm;
    breakDown(globalObject, *when, true, tm);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(asctime(globalObject, tm));
}

// mktime(tuple)
PYTHON_NATIVE(timeMktime)
{
    NATIVE_PROLOGUE();
    BrokenDownTime time;
    toBrokenDownTime(globalObject, args[0], time, "mktime"_s);
    RETURN_IF_EXCEPTION(scope, { });
    // -1 may be the answer. If it is, this has been worked out.
    time.tm.tm_wday = -1;
    time_t result = mktime(&time.tm);
    if (result == static_cast<time_t>(-1) && time.tm.tm_wday == -1)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "mktime argument out of range"_s));
    return JSValue::encode(floatFromDouble(static_cast<double>(result)));
}

// strftime(format[, tuple])
PYTHON_NATIVE(timeStrftime)
{
    NATIVE_PROLOGUE();
    if (!checkCount(globalObject, scope, args, "strftime"_s, 1, 2))
        return { };
    JSString* formatString = stringIn(args[0]);
    if (!formatString)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("strftime() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, args[0]))));
    BrokenDownTime time;
    toCheckedTime(globalObject, args.at(1), time, "strftime"_s);
    RETURN_IF_EXCEPTION(scope, { });
    // In case anything takes it to be no more than 1 and no less than -1
    time.tm.tm_isdst = std::clamp(time.tm.tm_isdst, -1, 1);

    Vector<char32_t, 64> format;
    auto view = formatString->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    charactersOf(globalObject, view, format);
    RETURN_IF_EXCEPTION(scope, { });
    TextBuilder result;
    Vector<wchar_t> piece;
    Vector<wchar_t> output;
    size_t outputSize = 1024;
    size_t i = 0;
    // The C library is given what is ASCII, a piece at a time. The rest is put in as it is, as far as the next percent sign.
    while (i < format.size()) {
        piece.shrink(0);
        for (; i < format.size() && format[i] && format[i] <= 127; ++i)
            piece.append(static_cast<wchar_t>(format[i]));
        if (size_t length = piece.size()) {
            piece.append(0);
            // time_strftime1(): there is no telling how much room it wants. If it comes to nothing with 256 times what the format takes, that is likely to be what it comes to.
            while (true) {
                if (!output.tryGrow(outputSize))
                    return JSValue::encode(raiseMemoryError(globalObject, scope));
                size_t written = wcsftime_l(output.mutableSpan().data(), outputSize, piece.span().data(), &time.tm, characterLocale(globalObject));
                if (!written && outputSize < 256 * length) {
                    outputSize += outputSize;
                    continue;
                }
                for (size_t k = 0; k < written; ++k)
                    result.append(static_cast<char32_t>(output[k]));
                break;
            }
        }
        for (; i < format.size() && format[i] != '%'; ++i)
            result.append(format[i]);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, result.tryFinish())));
}

// strptime(string, format), which is written in Python
PYTHON_NATIVE(timeStrptime)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "strptime"_s))
        return { };
    JSValue function = importModuleAttribute(globalObject, "_strptime"_s, "_strptime_time"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, function, args.allFrom(0), nullptr)));
}

PYTHON_NATIVE(timeTzset)
{
    NATIVE_PROLOGUE();
    JSValue module = importModule(globalObject, "time"_s);
    RETURN_IF_EXCEPTION(scope, { });
    tzset();
    initializeTimeZone(globalObject, asObject(module));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// get_clock_info(name)
PYTHON_NATIVE(timeGetClockInfo)
{
    NATIVE_PROLOGUE();
    if (!checkCount(globalObject, scope, args, "get_clock_info"_s, 1, 1))
        return { };
    JSString* nameString = stringIn(args[0]);
    if (!nameString)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("get_clock_info() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, args[0]))));
    String name = nameString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (name.contains('\0'))
        return JSValue::encode(raiseValueError(globalObject, scope, "embedded null character"_s));
    ClockInfo info;
    if (name == "time"_s)
        read(globalObject, Clock::Time, &info);
    else if (name == "monotonic"_s || name == "perf_counter"_s)
        read(globalObject, Clock::Monotonic, &info);
    else if (name == "process_time"_s)
        read(globalObject, Clock::ProcessTime, &info);
    else if (name == "thread_time"_s)
        read(globalObject, Clock::ThreadTime, &info);
    else
        return JSValue::encode(raiseValueError(globalObject, scope, "unknown clock"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* result = newSimpleNamespace(globalObject);
    auto add = [&] (ASCIILiteral field, JSValue value) { result->putDirect(vm, Identifier::fromString(vm, field), value); };
    add("implementation"_s, jsString(vm, String(info.implementation)));
    add("monotonic"_s, jsBoolean(info.isMonotonic));
    add("adjustable"_s, jsBoolean(info.isAdjustable));
    add("resolution"_s, floatFromDouble(info.resolution));
    return JSValue::encode(result);
}

JSObject* createTimeModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = timeState(globalObject);
    using Arguments = PyNativeFunction::Arguments;
    if (!state.structTime) {
        static constexpr std::array<ASCIILiteral, 11> fields { "tm_year"_s, "tm_mon"_s, "tm_mday"_s, "tm_hour"_s, "tm_min"_s, "tm_sec"_s, "tm_wday"_s, "tm_yday"_s, "tm_isdst"_s, "tm_zone"_s, "tm_gmtoff"_s };
        PyType* type = createBuiltinType(globalObject, "time.struct_time"_s, realm->typeTuple(), PyType::Layout::Tuple, PyType::IsSequence | PyType::IsDerivedFromBuiltin);
        state.structTime.set(vm, realm, type);
        makeStructSequenceType(globalObject, type, fields, 9);
    }
    JSObject* module = newBuiltinModule(globalObject, "time"_s);
    // What CPython takes its arguments for as a tuple, and looks through for itself
    constexpr auto any = "($module, /, *args)"_s;
    auto addClock = [&] (ASCIILiteral name, ASCIILiteral nameInNanoseconds, Clock clock) {
        addFunction(globalObject, module, name, timeRead, pack(clock, false));
        addFunction(globalObject, module, nameInNanoseconds, timeRead, pack(clock, true));
    };
    addClock("time"_s, "time_ns"_s, Clock::Time);
    addClock("monotonic"_s, "monotonic_ns"_s, Clock::Monotonic);
    addClock("perf_counter"_s, "perf_counter_ns"_s, Clock::Monotonic);
    addClock("process_time"_s, "process_time_ns"_s, Clock::ProcessTime);
    addClock("thread_time"_s, "thread_time_ns"_s, Clock::ThreadTime);
    addFunction(globalObject, module, "clock_gettime"_s, timeClockGetTime, pack(false));
    addFunction(globalObject, module, "clock_gettime_ns"_s, timeClockGetTime, pack(true));
    addFunction(globalObject, module, "clock_settime"_s, timeClockSetTime, pack(false), any, Arguments::AreNotChecked);
    addFunction(globalObject, module, "clock_settime_ns"_s, timeClockSetTime, pack(true), any, Arguments::AreNotChecked);
    addFunction(globalObject, module, "clock_getres"_s, timeClockGetRes, 0, any, Arguments::AreNotChecked);
    addFunction(globalObject, module, "sleep"_s, timeSleep, 0, "($module, seconds, /)"_s);
    addFunction(globalObject, module, "gmtime"_s, timeBreakDown, pack(false), any, Arguments::AreNotChecked);
    addFunction(globalObject, module, "localtime"_s, timeBreakDown, pack(true), any, Arguments::AreNotChecked);
    addFunction(globalObject, module, "asctime"_s, timeAsctime, 0, any, Arguments::AreNotChecked);
    addFunction(globalObject, module, "ctime"_s, timeCtime, 0, any, Arguments::AreNotChecked);
    addFunction(globalObject, module, "mktime"_s, timeMktime, 0, "($module, tuple, /)"_s);
    addFunction(globalObject, module, "strftime"_s, timeStrftime, 0, any, Arguments::AreNotChecked);
    addFunction(globalObject, module, "strptime"_s, timeStrptime, 0, any, Arguments::AreNotChecked);
    addFunction(globalObject, module, "tzset"_s, timeTzset, 0, "($module, /)"_s);
    addFunction(globalObject, module, "get_clock_info"_s, timeGetClockInfo, 0, any, Arguments::AreNotChecked);
    initializeTimeZone(globalObject, module);

    auto add = [&] (ASCIILiteral name, int value) { module->putDirect(vm, Identifier::fromString(vm, name), jsNumber(value)); };
    add("CLOCK_REALTIME"_s, CLOCK_REALTIME);
    add("CLOCK_MONOTONIC"_s, CLOCK_MONOTONIC);
#ifdef CLOCK_MONOTONIC_RAW
    add("CLOCK_MONOTONIC_RAW"_s, CLOCK_MONOTONIC_RAW);
#endif
#ifdef CLOCK_HIGHRES
    add("CLOCK_HIGHRES"_s, CLOCK_HIGHRES);
#endif
    add("CLOCK_PROCESS_CPUTIME_ID"_s, CLOCK_PROCESS_CPUTIME_ID);
    add("CLOCK_THREAD_CPUTIME_ID"_s, CLOCK_THREAD_CPUTIME_ID);
#ifdef CLOCK_PROF
    add("CLOCK_PROF"_s, CLOCK_PROF);
#endif
#ifdef CLOCK_BOOTTIME
    add("CLOCK_BOOTTIME"_s, CLOCK_BOOTTIME);
#endif
#ifdef CLOCK_TAI
    add("CLOCK_TAI"_s, CLOCK_TAI);
#endif
#ifdef CLOCK_UPTIME
    add("CLOCK_UPTIME"_s, CLOCK_UPTIME);
#endif
#ifdef CLOCK_UPTIME_RAW
    add("CLOCK_UPTIME_RAW"_s, CLOCK_UPTIME_RAW);
#endif
#ifdef CLOCK_MONOTONIC_RAW_APPROX
    add("CLOCK_MONOTONIC_RAW_APPROX"_s, CLOCK_MONOTONIC_RAW_APPROX);
#endif
#ifdef CLOCK_UPTIME_RAW_APPROX
    add("CLOCK_UPTIME_RAW_APPROX"_s, CLOCK_UPTIME_RAW_APPROX);
#endif
    add("_STRUCT_TM_ITEMS"_s, 11);
    module->putDirect(vm, Identifier::fromString(vm, "struct_time"_s), state.structTime.get());
    return module;
}

} } // namespace JSC::Python
