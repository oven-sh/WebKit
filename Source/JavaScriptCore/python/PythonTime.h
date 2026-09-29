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
// _PyTime_AsMicroseconds() and the like.
int64_t divideTime(int64_t time, int64_t divisor, TimeRounding);
// PyTime_AsSecondsDouble()
double timeAsSeconds(int64_t);

} } // namespace JSC::Python
