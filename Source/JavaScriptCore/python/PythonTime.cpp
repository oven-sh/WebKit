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
#include "PythonTime.h"

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PythonOperations.h"
#include <numeric>

#if OS(DARWIN)
#include <mach/mach_time.h>
#endif

namespace JSC { namespace Python {

static void raiseTimeOverflow(JSGlobalObject* globalObject, ThrowScope& scope)
{
    raise(globalObject, scope, BuiltinType::OverflowError, "timestamp out of range for C PyTime_t"_s);
}

// pytime_round()
static double roundTime(double value, TimeRounding rounding)
{
    switch (rounding) {
    case TimeRounding::HalfEven: {
        double rounded = std::round(value);
        // Halfway between two, it is the even one.
        if (std::fabs(value - rounded) == 0.5)
            rounded = 2.0 * std::round(value / 2.0);
        return rounded;
    }
    case TimeRounding::Ceiling:
        return std::ceil(value);
    case TimeRounding::Floor:
        return std::floor(value);
    case TimeRounding::Up:
        return value >= 0.0 ? std::ceil(value) : std::floor(value);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// pytime_from_object()
static std::optional<int64_t> timeFromObject(JSGlobalObject* globalObject, JSValue value, TimeRounding rounding, int64_t nanosecondsPerUnit)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isInstance(globalObject, value, globalObject->pyRealm()->typeFloat())) {
        auto seconds = toDouble(globalObject, value);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (std::isnan(*seconds)) {
            raiseValueError(globalObject, scope, "Invalid value NaN (not a number)"_s);
            return std::nullopt;
        }
        double nanoseconds = roundTime(*seconds * static_cast<double>(nanosecondsPerUnit), rounding);
        // The least there can be is a double exactly, and the most is not.
        constexpr double least = static_cast<double>(std::numeric_limits<int64_t>::min());
        if (!(least <= nanoseconds && nanoseconds < -least)) {
            raiseTimeOverflow(globalObject, scope);
            return std::nullopt;
        }
        return static_cast<int64_t>(nanoseconds);
    }
    JSValue integer = toInt(globalObject, value);
    if (scope.exception()) [[unlikely]] {
        if (catchException(globalObject, BuiltinType::TypeError)) {
            String type = fullyQualifiedTypeName(globalObject, value);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            raiseTypeError(globalObject, scope, concatenate('\'', type, "' object cannot be interpreted as an integer or float"_s));
        }
        return std::nullopt;
    }
    auto seconds = tryInt64(integer);
    CheckedInt64 nanoseconds = seconds.value_or(0);
    nanoseconds *= nanosecondsPerUnit;
    if (!seconds || nanoseconds.hasOverflowed()) {
        raiseTimeOverflow(globalObject, scope);
        return std::nullopt;
    }
    return nanoseconds.value();
}

std::optional<int64_t> timeFromSecondsObject(JSGlobalObject* globalObject, JSValue value, TimeRounding rounding) { return timeFromObject(globalObject, value, rounding, nanosecondsPerSecond); }
std::optional<int64_t> timeFromMillisecondsObject(JSGlobalObject* globalObject, JSValue value, TimeRounding rounding) { return timeFromObject(globalObject, value, rounding, nanosecondsPerMillisecond); }

// pytime_divide_round_up()
static int64_t divideAwayFromZero(int64_t time, int64_t divisor)
{
    int64_t quotient = time / divisor;
    if (time % divisor)
        quotient += time >= 0 ? 1 : -1;
    return quotient;
}

int64_t divideTime(int64_t time, int64_t divisor, TimeRounding rounding)
{
    ASSERT(divisor > 1);
    switch (rounding) {
    case TimeRounding::HalfEven: {
        int64_t quotient = time / divisor;
        int64_t remainder = std::abs(time % divisor);
        if (remainder > divisor / 2 || (remainder == divisor / 2 && (std::abs(quotient) & 1)))
            quotient += time >= 0 ? 1 : -1;
        return quotient;
    }
    case TimeRounding::Ceiling:
        return time >= 0 ? divideAwayFromZero(time, divisor) : time / divisor;
    case TimeRounding::Floor:
        return time >= 0 ? time / divisor : divideAwayFromZero(time, divisor);
    case TimeRounding::Up:
        return divideAwayFromZero(time, divisor);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<int64_t> timeFromNanosecondsObject(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!isInstance(globalObject, value, globalObject->pyRealm()->typeInt())) {
        raiseTypeError(globalObject, scope, concatenate("expect int, got "_s, typeName(globalObject, value)));
        return std::nullopt;
    }
    JSValue integer = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    auto nanoseconds = tryInt64(integer);
    if (!nanoseconds)
        raiseTimeOverflow(globalObject, scope);
    return nanoseconds;
}

static void raiseTimeTOverflow(JSGlobalObject* globalObject, ThrowScope& scope)
{
    raise(globalObject, scope, BuiltinType::OverflowError, "timestamp out of range for platform time_t"_s);
}

std::optional<time_t> objectToTimeT(JSGlobalObject* globalObject, JSValue value, TimeRounding rounding)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    static_assert(sizeof(time_t) == sizeof(int64_t));
    if (isInstance(globalObject, value, globalObject->pyRealm()->typeFloat())) {
        auto seconds = toDouble(globalObject, value);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (std::isnan(*seconds)) {
            raiseValueError(globalObject, scope, "Invalid value NaN (not a number)"_s);
            return std::nullopt;
        }
        double whole = std::trunc(roundTime(*seconds, rounding));
        constexpr double least = static_cast<double>(std::numeric_limits<time_t>::min());
        if (!(least <= whole && whole < -least)) {
            raiseTimeTOverflow(globalObject, scope);
            return std::nullopt;
        }
        return static_cast<time_t>(whole);
    }
    // _PyLong_AsTime_t()
    JSValue integer = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    auto seconds = tryInt64(integer);
    if (!seconds) {
        raiseTimeTOverflow(globalObject, scope);
        return std::nullopt;
    }
    return static_cast<time_t>(*seconds);
}

std::optional<int64_t> timeFromTimespec(JSGlobalObject* globalObject, const struct timespec& given)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    CheckedInt64 time = static_cast<int64_t>(given.tv_sec);
    time *= nanosecondsPerSecond;
    time += static_cast<int64_t>(given.tv_nsec);
    if (time.hasOverflowed()) {
        raiseTimeOverflow(globalObject, scope);
        return std::nullopt;
    }
    return time.value();
}

bool timeAsTimespec(JSGlobalObject*, int64_t time, struct timespec& result)
{
    // pytime_divmod(): what is left over is never less than nothing. A time_t has room for as many seconds as there can be.
    int64_t seconds = time / nanosecondsPerSecond;
    int64_t nanoseconds = time % nanosecondsPerSecond;
    if (nanoseconds < 0) {
        nanoseconds += nanosecondsPerSecond;
        --seconds;
    }
    result.tv_sec = static_cast<time_t>(seconds);
    result.tv_nsec = static_cast<long>(nanoseconds);
    return true;
}

bool timeAsTimeval(JSGlobalObject*, int64_t time, struct timeval& result, TimeRounding rounding)
{
    // pytime_divmod(), as above. There are too few microseconds in an int64 of nanoseconds for any of it to go wrong.
    constexpr int64_t microsecondsPerSecond = 1000 * 1000;
    int64_t microseconds = divideTime(time, nanosecondsPerMicrosecond, rounding);
    int64_t seconds = microseconds / microsecondsPerSecond;
    int64_t rest = microseconds % microsecondsPerSecond;
    if (rest < 0) {
        rest += microsecondsPerSecond;
        --seconds;
    }
    result.tv_sec = static_cast<time_t>(seconds);
    result.tv_usec = static_cast<suseconds_t>(rest);
    return true;
}

double timespecAsSeconds(const struct timespec& time) { return multiplyAdd(static_cast<double>(time.tv_nsec), 1e-9, static_cast<double>(time.tv_sec)); }

// py_get_system_clock()
std::optional<int64_t> systemClock(JSGlobalObject* globalObject, ClockInfo* info)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now)) {
        raiseOSError(globalObject, scope, errno);
        return std::nullopt;
    }
    if (info) {
        struct timespec resolution;
        *info = { "clock_gettime(CLOCK_REALTIME)"_s, false, true, clock_getres(CLOCK_REALTIME, &resolution) ? 1e-9 : timespecAsSeconds(resolution) };
    }
    RELEASE_AND_RETURN(scope, timeFromTimespec(globalObject, now));
}

// py_get_monotonic_clock(), which does not itself raise. What went wrong, if anything did: an errno, or -1 for there being no room for the answer.
static int readMonotonicClock(int64_t& time, ClockInfo* info)
{
    time = 0;
#if OS(DARWIN)
    // py_mach_timebase_info(), _PyTimeFraction_Set() and _PyTimeFraction_Mul()
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    int64_t divisor = std::gcd<int64_t, int64_t>(timebase.numer, timebase.denom);
    int64_t numerator = timebase.numer / divisor;
    int64_t denominator = timebase.denom / divisor;
    if (info)
        *info = { "mach_absolute_time()"_s, true, false, static_cast<double>(numerator) / static_cast<double>(denominator) / 1e9 };
    uint64_t ticks = mach_absolute_time();
    if (ticks > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return -1;
    auto multiply = [] (int64_t a, int64_t b) {
        CheckedInt64 product = a;
        product *= b;
        return product.hasOverflowed() ? std::numeric_limits<int64_t>::max() : product.value();
    };
    int64_t whole = static_cast<int64_t>(ticks) / denominator;
    int64_t rest = multiply(static_cast<int64_t>(ticks) % denominator, numerator) / denominator;
    CheckedInt64 result = multiply(whole, numerator);
    result += rest;
    time = result.hasOverflowed() ? std::numeric_limits<int64_t>::max() : result.value();
    return 0;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return errno;
    if (info) {
        struct timespec resolution;
        if (clock_getres(CLOCK_MONOTONIC, &resolution))
            return errno;
        *info = { "clock_gettime(CLOCK_MONOTONIC)"_s, true, false, timespecAsSeconds(resolution) };
    }
    CheckedInt64 result = static_cast<int64_t>(now.tv_sec);
    result *= nanosecondsPerSecond;
    result += static_cast<int64_t>(now.tv_nsec);
    if (result.hasOverflowed())
        return -1;
    time = result.value();
    return 0;
#endif
}

std::optional<int64_t> monotonicClock(JSGlobalObject* globalObject, ClockInfo* info)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int64_t time;
    int error = readMonotonicClock(time, info);
    if (!error)
        return time;
    if (error < 0)
        raiseTimeOverflow(globalObject, scope);
    else
        raiseOSError(globalObject, scope, error);
    return std::nullopt;
}

int64_t deadlineAfter(int64_t timeout)
{
    int64_t now;
    readMonotonicClock(now, nullptr);
    // _PyTime_Add(), which stops at the most and the least that there can be.
    CheckedInt64 deadline = now;
    deadline += timeout;
    if (deadline.hasOverflowed())
        return timeout > 0 ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min();
    return deadline.value();
}

int64_t timeUntil(int64_t deadline)
{
    int64_t now;
    readMonotonicClock(now, nullptr);
    return deadline - now;
}

double timeAsSeconds(int64_t time)
{
    // A whole number of seconds is divided as a whole number, 1e-9 not being a double exactly.
    if (!(time % nanosecondsPerSecond))
        return static_cast<double>(time / nanosecondsPerSecond);
    return static_cast<double>(time) / 1e9;
}

} } // namespace JSC::Python
