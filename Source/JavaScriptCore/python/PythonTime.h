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


#pragma once

#include "JSCJSValue.h"
#include <optional>
#include <sys/time.h>
#include <time.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class JSGlobalObject;

namespace Python {

// Times, as a number of nanoseconds: PyTime_t, and Python/pytime.c of CPython.

enum class TimeRounding : uint8_t {
    Floor,
    Ceiling,
    HalfEven,
    Up, // Away from zero.
    Timeout = Up, // So as not to wait for less long than was asked.
};

static constexpr int64_t nanosecondsPerSecond = 1000 * 1000 * 1000;
static constexpr int64_t nanosecondsPerMillisecond = 1000 * 1000;
static constexpr int64_t nanosecondsPerMicrosecond = 1000;

// _PyTime_FromSecondsObject(): from an int or a float that is a number of seconds. Nothing if it raised.
std::optional<int64_t> timeFromSecondsObject(JSGlobalObject*, JSValue, TimeRounding);
// _PyTime_FromMillisecondsObject()
std::optional<int64_t> timeFromMillisecondsObject(JSGlobalObject*, JSValue, TimeRounding);
// _PyTime_AsMicroseconds() and the like.
int64_t divideTime(int64_t time, int64_t divisor, TimeRounding);
// PyTime_AsSecondsDouble()
double timeAsSeconds(int64_t);
// _PyTime_FromLong(): from an int that is a number of nanoseconds. Nothing if it raised.
std::optional<int64_t> timeFromNanosecondsObject(JSGlobalObject*, JSValue);
// _PyTime_ObjectToTime_t(): a whole number of seconds, from an int or a float. Nothing if it raised.
std::optional<time_t> objectToTimeT(JSGlobalObject*, JSValue, TimeRounding);
// _PyTime_FromTimespec() and _PyTime_AsTimespec(). Nothing, or false, if it raised.
std::optional<int64_t> timeFromTimespec(JSGlobalObject*, const struct timespec&);
bool timeAsTimespec(JSGlobalObject*, int64_t, struct timespec&);
// _PyTime_AsTimeval(). False if it raised.
bool timeAsTimeval(JSGlobalObject*, int64_t, struct timeval&, TimeRounding);
// tv_sec + tv_nsec * 1e-9
double timespecAsSeconds(const struct timespec&);

// _PyDeadline_Init() and _PyDeadline_Get(): when a time from now is, by the clock that only goes forward, and how long it is until then. Neither raises.
int64_t deadlineAfter(int64_t timeout);
int64_t timeUntil(int64_t deadline);

// _Py_clock_info_t
struct ClockInfo {
    ASCIILiteral implementation { ""_s };
    bool isMonotonic { false };
    bool isAdjustable { false };
    double resolution { 1.0 };
};
// _PyTime_TimeWithInfo() and _PyTime_MonotonicWithInfo(), which is _PyTime_PerfCounterWithInfo() too. Nothing if it raised.
std::optional<int64_t> systemClock(JSGlobalObject*, ClockInfo* = nullptr);
std::optional<int64_t> monotonicClock(JSGlobalObject*, ClockInfo* = nullptr);
// PyTime_MonotonicRaw(), which is PyTime_PerfCounterRaw() too. It does not raise: it is 0 if there is no reading the clock.
int64_t monotonicClockRaw();

} } // namespace JSC::Python
