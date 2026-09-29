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

std::optional<int64_t> timeFromSecondsObject(JSGlobalObject* globalObject, JSValue value, TimeRounding rounding)
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
        double nanoseconds = roundTime(*seconds * static_cast<double>(nanosecondsPerSecond), rounding);
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
    nanoseconds *= nanosecondsPerSecond;
    if (!seconds || nanoseconds.hasOverflowed()) {
        raiseTimeOverflow(globalObject, scope);
        return std::nullopt;
    }
    return nanoseconds.value();
}

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

double timeAsSeconds(int64_t time)
{
    // A whole number of seconds is divided as a whole number, 1e-9 not being a double exactly.
    if (!(time % nanosecondsPerSecond))
        return static_cast<double>(time / nanosecondsPerSecond);
    return static_cast<double>(time) / 1e9;
}

} } // namespace JSC::Python
