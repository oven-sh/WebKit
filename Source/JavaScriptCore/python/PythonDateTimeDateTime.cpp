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
#include "PythonDateTime.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonIO.h"
#include "PythonOperations.h"
#include "PythonSignatures.h"
#include "PythonText.h"
#include "PythonTime.h"

// datetime.datetime

namespace JSC { namespace Python {

JSValue newDateTime(JSGlobalObject* globalObject, int year, int month, int day, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold, PyType* type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    checkDateArguments(globalObject, year, month, day);
    RETURN_IF_EXCEPTION(scope, { });
    checkTimeArguments(globalObject, hour, minute, second, microsecond, fold);
    RETURN_IF_EXCEPTION(scope, { });
    checkTZInfo(globalObject, tzinfo);
    RETURN_IF_EXCEPTION(scope, { });
    if (!type)
        type = dateTimeModuleState(globalObject).dateTimeType.get();
    auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<DateState>());
    auto& self = object->state<DateState>();
    self.isDateTime = true;
    self.setDate(year, month, day);
    self.setTime(hour, minute, second, microsecond);
    self.fold = fold;
    if (!isNone(tzinfo))
        self.tzinfo.set(vm, object, tzinfo);
    return object;
}

JSValue newDateTimeOfClass(JSGlobalObject* globalObject, int year, int month, int day, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold, JSValue cls)
{
    if (cls == dateTimeModuleState(globalObject).dateTimeType->object())
        return newDateTime(globalObject, year, month, day, hour, minute, second, microsecond, tzinfo, fold);
    MarkedArgumentBuffer arguments;
    for (int field : { year, month, day, hour, minute, second, microsecond })
        arguments.append(jsNumber(field));
    arguments.append(tzinfo);
    return callClassWithFold(globalObject, cls, arguments, fold);
}

JSValue addTimeDeltaToDateTime(JSGlobalObject* globalObject, JSValue dateTime, const TimeDeltaState& delta, int factor)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    ASSERT(factor == 1 || factor == -1);
    // It may be what a class derived from datetime made when it was called, which CPython takes for a datetime whatever it is.
    if (!tryDateTime(dateTime))
        return raiseTypeError(globalObject, scope, concatenate("expected a datetime, not "_s, typeName(globalObject, dateTime)));
    auto& self = stateOf<DateState>(dateTime);
    // None of these can be too much for an int, each part being within its bounds.
    int year = self.year();
    int month = self.month();
    int day = self.day() + delta.days * factor;
    int hour = self.hour();
    int minute = self.minute();
    int second = self.second() + delta.seconds * factor;
    int microsecond = self.microsecond() + delta.microseconds * factor;
    normalizeDateTime(globalObject, year, month, day, hour, minute, second, microsecond);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, newDateTimeOfClass(globalObject, year, month, day, hour, minute, second, microsecond, self.tzinfoOrNone(), 0, typeOf(globalObject, dateTime)->object()));
}

namespace {

bool isSaneMonthWithFold(unsigned month) { return (month & 0x7F) - 1 < 12; }

// A copy that differs in its tzinfo, its fold or both, of the same class and without asking the class
JSValue copyOfDateTime(JSGlobalObject* globalObject, JSValue dateTime, JSValue tzinfo, int fold)
{
    auto& self = stateOf<DateState>(dateTime);
    return newDateTime(globalObject, self.year(), self.month(), self.day(), self.hour(), self.minute(), self.second(), self.microsecond(), tzinfo, fold, typeOf(globalObject, dateTime));
}

// datetime_new()
PYTHON_NATIVE(dateTimeNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    // What pickle calls it with is what __reduce__() gave.
    if (unsigned given = args.size() - 1; given >= 1 && given <= 2) {
        auto state = pickledState(globalObject, args.at(1), DateState::dateTimeSize, 2, isSaneMonthWithFold, "datetime"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (state) {
            // datetime_from_pickle()
            JSValue tzinfo = given == 2 ? args.at(2) : jsUndefined();
            if (!isNone(tzinfo) && !isTZInfo(globalObject, tzinfo))
                return JSValue::encode(raiseTypeError(globalObject, scope, "bad tzinfo state arg"_s));
            auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<DateState>());
            auto& self = object->state<DateState>();
            self.isDateTime = true;
            memcpySpan(std::span(self.data), state->span());
            if (!isNone(tzinfo))
                self.tzinfo.set(vm, object, tzinfo);
            if (self.data[2] & 1 << 7) {
                self.data[2] -= 128;
                self.fold = 1;
            }
            return JSValue::encode(object);
        }
    }

    // PyArg_ParseTupleAndKeywords(), of "iii|iiiiO$i": whether there are too many is seen first. Then each is made what it can be as it is come to, so that what is wrong with the first is said before that there is no second. Whether
    // too many were given by position is seen at the $.
    if (args.size() - 1 + args.keywordCount() > 9) {
        checkArgumentsSlow(globalObject, callFrame);
        return { };
    }
    int fields[7] = { 0, 0, 0, 0, 0, 0, 0 };
    parseIntArguments(globalObject, args, 1, fields, 3);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue tzinfo = args.at(8) ? args.at(8) : jsUndefined();
    if (args.size() - 1 > 8) {
        checkArgumentsSlow(globalObject, callFrame);
        return { };
    }
    int fold = 0;
    parseIntArguments(globalObject, args, 9, { &fold, 1 }, 0);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkArgumentsSlow(globalObject, callFrame))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(newDateTime(globalObject, fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], tzinfo, fold, type)));
}

// ---- Seconds since 1 January of the year 1, which is what local time is worked out in

constexpr long long maxFoldSeconds = 24 * 3600;
constexpr long long epochSeconds = 719163LL * 24 * 60 * 60; // When 1970 began

// utc_to_seconds(). It may raise.
long long utcToSeconds(JSGlobalObject* globalObject, int year, int month, int day, int hour, int minute, int second)
{
    // There is no ordinal for a year before the first.
    if (year < minYear || year > maxYear) {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        raiseValueError(globalObject, scope, concatenate("year must be in "_s, minYear, ".."_s, maxYear, ", not "_s, year));
        return -1;
    }
    long long ordinal = dateToOrdinal(year, month, day);
    return ((ordinal * 24 + hour) * 60 + minute) * 60 + second;
}

// local(): what the clocks here say at a time that is given in UTC. It may raise.
long long localFromUTC(JSGlobalObject* globalObject, long long u)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    static_assert(sizeof(time_t) == sizeof(long long));
    struct tm tm;
    breakDownTime(globalObject, static_cast<time_t>(u - epochSeconds), BrokenDownAs::Local, tm);
    RETURN_IF_EXCEPTION(scope, -1);
    RELEASE_AND_RETURN(scope, utcToSeconds(globalObject, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec));
}

// local_to_seconds(): the other way about, which is to solve t = local(u) for u. It may raise.
long long localToSeconds(JSGlobalObject* globalObject, int year, int month, int day, int hour, int minute, int second, int fold)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    long long t = utcToSeconds(globalObject, year, month, day, hour, minute, second);
    RETURN_IF_EXCEPTION(scope, -1);
    long long lt = localFromUTC(globalObject, t);
    RETURN_IF_EXCEPTION(scope, -1);
    long long a = lt - t;
    long long u1 = t - a;
    long long t1 = localFromUTC(globalObject, u1);
    RETURN_IF_EXCEPTION(scope, -1);
    long long b;
    if (t1 == t) {
        // That is one solution, and may not be the one that is wanted. An earlier one is looked for if there is no fold, and a later one if there is.
        long long u2 = fold ? u1 + maxFoldSeconds : u1 - maxFoldSeconds;
        lt = localFromUTC(globalObject, u2);
        RETURN_IF_EXCEPTION(scope, -1);
        b = lt - u2;
        if (a == b)
            return u1;
    } else {
        b = t1 - u1;
        ASSERT(a != b);
    }
    long long u2 = t - b;
    long long t2 = localFromUTC(globalObject, u2);
    RETURN_IF_EXCEPTION(scope, -1);
    if (t2 == t)
        return u2;
    if (t1 == t)
        return u1;
    // Both offsets have been found, and neither gives a solution: it is a time that the clocks skipped.
    return fold ? std::min(u1, u2) : std::max(u1, u2);
}

long long localToSeconds(JSGlobalObject* globalObject, const DateState& self, int fold)
{
    return localToSeconds(globalObject, self.year(), self.month(), self.day(), self.hour(), self.minute(), self.second(), fold);
}

// datetime_from_timet_and_us()
JSValue dateTimeFromTimeAndMicroseconds(JSGlobalObject* globalObject, JSValue cls, BrokenDownAs how, time_t when, int microsecond, JSValue tzinfo)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    struct tm tm;
    breakDownTime(globalObject, when, how, tm);
    RETURN_IF_EXCEPTION(scope, { });
    int year = tm.tm_year + 1900;
    int month = tm.tm_mon + 1;
    int day = tm.tm_mday;
    int hour = tm.tm_hour;
    int minute = tm.tm_min;
    // The system may put in leap seconds, which are of no interest, and would be refused for a reason that made no sense to whoever asked.
    int second = std::min(59, tm.tm_sec);
    int fold = 0;

    // In local time there may be a fold.
    if (isNone(tzinfo) && how == BrokenDownAs::Local) {
        long long resultSeconds = utcToSeconds(globalObject, year, month, day, hour, minute, second);
        RETURN_IF_EXCEPTION(scope, { });
        long long probeSeconds = localFromUTC(globalObject, epochSeconds + when - maxFoldSeconds);
        RETURN_IF_EXCEPTION(scope, { });
        long long transition = resultSeconds - probeSeconds - maxFoldSeconds;
        if (transition < 0) {
            probeSeconds = localFromUTC(globalObject, epochSeconds + when + transition);
            RETURN_IF_EXCEPTION(scope, { });
            if (probeSeconds == resultSeconds)
                fold = 1;
        }
    }
    RELEASE_AND_RETURN(scope, newDateTimeOfClass(globalObject, year, month, day, hour, minute, second, microsecond, tzinfo, fold, cls));
}

// datetime_from_timestamp()
JSValue dateTimeFromTimestampObject(JSGlobalObject* globalObject, JSValue cls, BrokenDownAs how, JSValue timestamp, JSValue tzinfo)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    time_t when;
    long microseconds;
    objectToTimeval(globalObject, timestamp, when, microseconds, TimeRounding::HalfEven);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, dateTimeFromTimeAndMicroseconds(globalObject, cls, how, when, static_cast<int>(microseconds), tzinfo));
}

// datetime_best_possible(): now, which is not held to what a timestamp can say
JSValue dateTimeOfNow(JSGlobalObject* globalObject, JSValue cls, BrokenDownAs how, JSValue tzinfo)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto now = systemClock(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    // _PyTime_AsTimevalTime_t(), rounding down
    int64_t microseconds = divideTime(*now, nanosecondsPerMicrosecond, TimeRounding::Floor);
    int64_t seconds = microseconds / 1000000;
    int64_t rest = microseconds % 1000000;
    if (rest < 0) {
        rest += 1000000;
        --seconds;
    }
    RELEASE_AND_RETURN(scope, dateTimeFromTimeAndMicroseconds(globalObject, cls, how, static_cast<time_t>(seconds), static_cast<int>(rest), tzinfo));
}

// What is in UTC and has a tzinfo, as that tzinfo has it: tz.fromutc(self)
JSValue fromUTC(JSGlobalObject* globalObject, JSValue tzinfo, JSValue dateTime) { return callMethodNamed(globalObject, tzinfo, globalObject->vm().pythonNames().attribute_fromutc, dateTime); }

// datetime_datetime_now_impl()
PYTHON_NATIVE(dateTimeNow)
{
    NATIVE_PROLOGUE();
    JSValue tzinfo = args.at(1) ? args.at(1) : jsUndefined();
    checkTZInfo(globalObject, tzinfo);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue self = dateTimeOfNow(globalObject, args[0], isNone(tzinfo) ? BrokenDownAs::Local : BrokenDownAs::UTC, tzinfo);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(tzinfo))
        return JSValue::encode(self);
    RELEASE_AND_RETURN(scope, JSValue::encode(fromUTC(globalObject, tzinfo, self)));
}

PYTHON_NATIVE(dateTimeUTCNow)
{
    NATIVE_PROLOGUE();
    warn(globalObject, BuiltinType::DeprecationWarning, "datetime.datetime.utcnow() is deprecated and scheduled for removal in a future version. Use timezone-aware objects to represent datetimes in UTC: datetime.datetime.now(datetime.UTC)."_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(dateTimeOfNow(globalObject, args[0], BrokenDownAs::UTC, jsUndefined())));
}

// datetime_fromtimestamp()
PYTHON_NATIVE(dateTimeFromTimestamp)
{
    NATIVE_PROLOGUE();
    JSValue tzinfo = args.at(2) ? args.at(2) : jsUndefined();
    checkTZInfo(globalObject, tzinfo);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue self = dateTimeFromTimestampObject(globalObject, args[0], isNone(tzinfo) ? BrokenDownAs::Local : BrokenDownAs::UTC, args[1], tzinfo);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(tzinfo))
        return JSValue::encode(self);
    RELEASE_AND_RETURN(scope, JSValue::encode(fromUTC(globalObject, tzinfo, self)));
}

// datetime_utcfromtimestamp()
PYTHON_NATIVE(dateTimeUTCFromTimestamp)
{
    NATIVE_PROLOGUE();
    warn(globalObject, BuiltinType::DeprecationWarning, "datetime.datetime.utcfromtimestamp() is deprecated and scheduled for removal in a future version. Use timezone-aware objects to represent datetimes in UTC: datetime.datetime.fromtimestamp(timestamp, datetime.UTC)."_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(dateTimeFromTimestampObject(globalObject, args[0], BrokenDownAs::UTC, args[1], jsUndefined())));
}

PYTHON_NATIVE(dateTimeStrptime)
{
    NATIVE_PROLOGUE();
    checkIsStrArgument(globalObject, "strptime"_s, 1, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    checkIsStrArgument(globalObject, "strptime"_s, 2, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(callStrptime(globalObject, "_strptime_datetime_datetime"_s, args[0], args[1], args[2])));
}

// datetime_combine()
PYTHON_NATIVE(dateTimeCombine)
{
    NATIVE_PROLOGUE();
    // PyArg_ParseTupleAndKeywords(), of "O!O!|O": what is wrong with the first is said before that there is no second.
    if (args.size() - 1 + args.keywordCount() > 3 || !args.at(1)) {
        checkArgumentsSlow(globalObject, callFrame);
        return { };
    }
    auto* date = tryDate(args.at(1));
    if (!date)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("combine() argument 1 must be datetime.date, not "_s, typeNameOfArgument(globalObject, args.at(1)))));
    if (!args.at(2)) {
        checkArgumentsSlow(globalObject, callFrame);
        return { };
    }
    auto* time = tryTimeOfDay(args.at(2));
    if (!time)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("combine() argument 2 must be datetime.time, not "_s, typeNameOfArgument(globalObject, args.at(2)))));
    if (!checkArgumentsSlow(globalObject, callFrame))
        return { };
    JSValue tzinfo = args.at(3) ? args.at(3) : time->tzinfoOrNone();
    RELEASE_AND_RETURN(scope, JSValue::encode(newDateTimeOfClass(globalObject, date->year(), date->month(), date->day(), time->hour(), time->minute(), time->second(), time->microsecond(), tzinfo, time->fold, args[0])));
}

// _find_isoformat_datetime_separator()
int64_t findISOFormatSeparator(const char* text, int64_t length)
{
    // Which way the date is written can be told from the fifth and sixth characters, and that says where to look for what separates it from the time.
    //
    //     YYYY-MM-DD   10      YYYYMMDD   8
    //     YYYY-Www      8      YYYYWww    7
    //     YYYY-Www-d   10      YYYYWwwd   8
    //
    // Any character at all may separate them, so where there is a W it is not always plain. 2020-W01-0000 could be YYYY-Www-D0HH or YYYY-Www-HHMM. YYYYWww can be told from YYYYWwwd by going on to the end or to what is no digit,
    // since the parts of a time come in pairs.
    if (length == 7)
        return 7;
    auto isDigit = [] (char c) { return static_cast<unsigned>(c - '0') < 10; };
    if (text[4] == '-') {
        if (text[5] != 'W')
            return 10;
        if (length < 8)
            return -1;
        if (length > 8 && text[8] == '-') {
            // YYYY-Www-D (10) or YYYY-Www-HH (8)
            if (length == 9)
                return -1;
            // It is taken for a hyphen at 8, that being likelier to separate them than a digit at 10.
            if (length > 10 && isDigit(text[10]))
                return 8;
            return 10;
        }
        return 8;
    }
    if (text[4] != 'W')
        return 8;
    // YYYYWww (7) or YYYYWwwd (8)
    int64_t index = 7;
    for (; index < length; ++index) {
        if (!isDigit(text[index]))
            break;
    }
    if (index < 9)
        return index;
    return index % 2 ? 8 : 7;
}

// datetime_fromisoformat()
PYTHON_NATIVE(dateTimeFromISOFormat)
{
    NATIVE_PROLOGUE();
    JSString* string = stringIn(args[1]);
    if (!string)
        return JSValue::encode(raiseTypeError(globalObject, scope, "fromisoformat: argument must be str"_s));
    auto raiseInvalid = [&] { return JSValue::encode(raiseInvalidISOFormat(globalObject, scope, args[1])); };

    // _sanitize_isoformat_str(): half of a surrogate pair will do in one place, which is between the date and the time, and it is made a T so that the rest can be read as UTF-8. That place is at 7, 8 or 10. Everything before it
    // is to be ASCII, so where it is in the string is where it is among the characters.
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (text.length() < 7)
        RELEASE_AND_RETURN(scope, raiseInvalid());
    for (unsigned position : { 7u, 8u, 10u }) {
        if (position >= text.length())
            break;
        char16_t unit = text[position];
        if (!U16_IS_SURROGATE(unit))
            continue;
        bool isHalfOfAPair = U16_IS_LEAD(unit) ? position + 1 < text.length() && U16_IS_TRAIL(text[position + 1]) : U16_IS_LEAD(text[position - 1]);
        if (!isHalfOfAPair)
            text = makeString(StringView(text).left(position), 'T', StringView(text).substring(position + 1));
        break;
    }
    auto converted = utf8ForParsing(text);
    if (!converted)
        RELEASE_AND_RETURN(scope, raiseInvalid());
    const char* start = converted->data();
    int64_t length = converted->length();

    int64_t separator = findISOFormatSeparator(start, length);
    const char* p = start;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int microsecond = 0;
    int offset = 0;
    int offsetMicrosecond = 0;
    int result = parseISODate(p, separator, year, month, day);
    if (!result && length > separator) {
        // How many bytes there are to a character is in the first of them.
        p += separator;
        if (!(p[0] & 0x80))
            p += 1;
        else {
            switch (p[0] & 0xF0) {
            case 0xE0:
                p += 3;
                break;
            case 0xF0:
                p += 4;
                break;
            default:
                p += 2;
                break;
            }
        }
        length -= p - start;
        result = parseISOTime(p, length, hour, minute, second, microsecond, offset, offsetMicrosecond);
    }
    if (result < 0)
        RELEASE_AND_RETURN(scope, raiseInvalid());

    JSValue tzinfo = tzinfoFromISOFormat(globalObject, result, offset, offsetMicrosecond);
    RETURN_IF_EXCEPTION(scope, { });

    if (hour == 24 && month >= 1 && month <= 12) {
        int inMonth = daysInMonth(year, month);
        if (day <= inMonth) {
            if (minute || second || microsecond)
                return JSValue::encode(raiseValueError(globalObject, scope, "minute, second, and microsecond must be 0 when hour is 24"_s));
            // Midnight of the day after
            hour = 0;
            day += 1;
            if (day > inMonth) {
                day = 1;
                month += 1;
                if (month > 12) {
                    month = 1;
                    year += 1;
                }
            }
        }
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newDateTimeOfClass(globalObject, year, month, day, hour, minute, second, microsecond, tzinfo, 0, args[0])));
}

// ---- What is asked of the tzinfo

JSValue utcOffsetOf(JSGlobalObject* globalObject, JSValue dateTime) { return callUTCOffset(globalObject, stateOf<DateState>(dateTime).tzinfoOrNone(), dateTime); } // datetime_utcoffset()

PYTHON_NATIVE(dateTimeUTCOffset)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(utcOffsetOf(globalObject, args[0])));
}

PYTHON_NATIVE(dateTimeDST)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callDST(globalObject, stateOf<DateState>(args[0]).tzinfoOrNone(), args[0])));
}

PYTHON_NATIVE(dateTimeTZName)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callTZName(globalObject, stateOf<DateState>(args[0]).tzinfoOrNone(), args[0])));
}

// ---- Arithmetic

// datetime_add()
PYTHON_NATIVE(dateTimeAdd)
{
    NATIVE_PROLOGUE();
    auto [left, right] = operandsOfSlot(callFrame);
    if (tryDateTime(left)) {
        if (auto* delta = tryTimeDelta(right))
            RELEASE_AND_RETURN(scope, JSValue::encode(addTimeDeltaToDateTime(globalObject, left, *delta, 1)));
    } else if (auto* delta = tryTimeDelta(left))
        RELEASE_AND_RETURN(scope, JSValue::encode(addTimeDeltaToDateTime(globalObject, right, *delta, 1)));
    RETURN_NOT_IMPLEMENTED();
}

// The part of datetime_subtract() that is for two of them
JSValue subtractDateTimes(JSGlobalObject* globalObject, JSValue leftValue, JSValue rightValue)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& left = stateOf<DateState>(leftValue);
    auto& right = stateOf<DateState>(rightValue);
    JSValue offsetDifference;
    if (left.tzinfoOrNone() != right.tzinfoOrNone()) {
        JSValue offset1 = utcOffsetOf(globalObject, leftValue);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue offset2 = utcOffsetOf(globalObject, rightValue);
        RETURN_IF_EXCEPTION(scope, { });
        if (isNone(offset1) != isNone(offset2))
            return raiseTypeError(globalObject, scope, "can't subtract offset-naive and offset-aware datetimes"_s);
        if (offset1 != offset2 && compareTimeDeltas(*tryTimeDelta(offset1), *tryTimeDelta(offset2))) {
            offsetDifference = subtractTimeDeltas(globalObject, *tryTimeDelta(offset1), *tryTimeDelta(offset2));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    int days = dateToOrdinal(left.year(), left.month(), left.day()) - dateToOrdinal(right.year(), right.month(), right.day());
    // No more than there are seconds in a day
    int seconds = (left.hour() - right.hour()) * 3600 + (left.minute() - right.minute()) * 60 + (left.second() - right.second());
    int microseconds = left.microsecond() - right.microsecond();
    JSValue result = newTimeDelta(globalObject, days, seconds, microseconds, true);
    RETURN_IF_EXCEPTION(scope, { });
    if (offsetDifference)
        RELEASE_AND_RETURN(scope, subtractTimeDeltas(globalObject, *tryTimeDelta(result), *tryTimeDelta(offsetDifference)));
    return result;
}

// datetime_subtract()
PYTHON_NATIVE(dateTimeSubtract)
{
    NATIVE_PROLOGUE();
    auto [left, right] = operandsOfSlot(callFrame);
    if (tryDateTime(left)) {
        if (tryDateTime(right))
            RELEASE_AND_RETURN(scope, JSValue::encode(subtractDateTimes(globalObject, left, right)));
        if (auto* delta = tryTimeDelta(right))
            RELEASE_AND_RETURN(scope, JSValue::encode(addTimeDeltaToDateTime(globalObject, left, *delta, -1)));
    }
    RETURN_NOT_IMPLEMENTED();
}

// ---- Various ways to turn a datetime into a string

// datetime_repr()
PYTHON_NATIVE(dateTimeRepr)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    TextBuilder builder;
    builder.append(typeName(globalObject, args[0]), '(', self.year(), ", "_s, self.month(), ", "_s, self.day(), ", "_s, self.hour(), ", "_s, self.minute());
    if (self.microsecond())
        builder.append(", "_s, self.second(), ", "_s, self.microsecond());
    else if (self.second())
        builder.append(", "_s, self.second());
    builder.append(')');
    String result = withFoldKeyword(builder.tryFinish(), self.fold);
    if (self.tzinfo) {
        result = withTZInfoKeyword(globalObject, result, self.tzinfo.get());
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, result)));
}

// datetime_str()
PYTHON_NATIVE(dateTimeStr)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, args[0], names.attribute_isoformat, jsString(vm, String(" "_s)))));
}

// datetime_isoformat()
PYTHON_NATIVE(dateTimeISOFormat)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    // The "C" of PyArg_ParseTuple(): a str of one character
    char32_t separator = 'T';
    if (JSValue given = args.at(1)) {
        JSString* string = stringIn(given);
        if (!string)
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("isoformat() argument 1 must be a unicode character, not "_s, typeNameOfArgument(globalObject, given))));
        String text = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        size_t count = 0;
        for (char32_t character : StringView(text).codePoints()) {
            separator = character;
            ++count;
        }
        if (count != 1)
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("isoformat() argument 1 must be a unicode character, not a string of length "_s, count)));
    }
    auto specification = timeSpecificationFrom(globalObject, args.at(2), 2, self.microsecond());
    RETURN_IF_EXCEPTION(scope, { });

    StringBuilder builder;
    builder.append(pad('0', 4, self.year()), '-', pad('0', 2, self.month()), '-', pad('0', 2, self.day()));
    builder.append(separator);
    appendTimeAsISO(builder, *specification, self.hour(), self.minute(), self.second(), self.microsecond());
    if (self.tzinfo) {
        String offset = formatUTCOffset(globalObject, ":"_s, self.tzinfo.get(), args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(offset);
    }
    return JSValue::encode(jsString(vm, builder.toString()));
}

PYTHON_NATIVE(dateTimeCTime)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    return JSValue::encode(jsString(vm, formatCTime(self, self.hour(), self.minute(), self.second())));
}

// ---- Miscellaneous methods

// get_flip_fold_offset(): how far it would be from UTC with the other fold
JSValue offsetWithOtherFold(JSGlobalObject* globalObject, JSValue dateTime)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& self = stateOf<DateState>(dateTime);
    // flip_fold()
    JSValue flipped = copyOfDateTime(globalObject, dateTime, self.tzinfoOrNone(), !self.fold);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, utcOffsetOf(globalObject, flipped));
}

// pep495_eq_exception(): whether either is at a time that comes twice or not at all where it is, and so is equal to nothing in another zone. It may raise.
bool isProblematicForEquality(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue offsetOfSelf, JSValue offsetOfOther)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    // What is None is not told from a timedelta here in CPython, which reads one as the other. Neither is None by the time this is asked unless both are.
    auto differs = [] (JSValue flipped, JSValue offset) {
        if (flipped == offset)
            return false;
        auto* left = tryTimeDelta(flipped);
        auto* right = tryTimeDelta(offset);
        return !left || !right || compareTimeDeltas(*left, *right);
    };
    JSValue flipped = offsetWithOtherFold(globalObject, self);
    RETURN_IF_EXCEPTION(scope, false);
    if (differs(flipped, offsetOfSelf))
        return true;
    flipped = offsetWithOtherFold(globalObject, other);
    RETURN_IF_EXCEPTION(scope, false);
    return differs(flipped, offsetOfOther);
}

// datetime_richcompare()
PYTHON_NATIVE(dateTimeCompare)
{
    NATIVE_PROLOGUE();
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    auto* other = tryDateTime(args[1]);
    if (!other)
        RETURN_NOT_IMPLEMENTED();
    auto& self = stateOf<DateState>(args[0]);
    if (self.tzinfoOrNone() == other->tzinfoOrNone())
        return JSValue::encode(comparisonResult(compareBytes(self.data, other->data), op));

    JSValue offset1 = utcOffsetOf(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue offset2 = utcOffsetOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    auto* delta1 = tryTimeDelta(offset1);
    auto* delta2 = tryTimeDelta(offset2);
    int difference;
    // It is cheap if neither says how far it is from UTC, or both say the same.
    if (offset1 == offset2 || (delta1 && delta2 && !compareTimeDeltas(*delta1, *delta2)))
        difference = compareBytes(self.data, other->data);
    else if (delta1 && delta2) {
        JSValue delta = subtractDateTimes(globalObject, args[0], args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        auto& between = *tryTimeDelta(delta);
        difference = between.days;
        if (!difference)
            difference = between.seconds | between.microseconds;
    } else if (op == ComparisonOperator::Eq)
        return JSValue::encode(jsBoolean(false));
    else if (op == ComparisonOperator::NotEq)
        return JSValue::encode(jsBoolean(true));
    else
        return JSValue::encode(raiseTypeError(globalObject, scope, "can't compare offset-naive and offset-aware datetimes"_s));

    if (isEquality(op) && !difference) {
        bool isProblematic = isProblematicForEquality(globalObject, args[0], args[1], offset1, offset2);
        RETURN_IF_EXCEPTION(scope, { });
        if (isProblematic)
            difference = 1;
    }
    return JSValue::encode(comparisonResult(difference, op));
}

// datetime_hash()
PYTHON_NATIVE(dateTimeHash)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    if (self.hash == -1) {
        JSValue withoutFold = args[0];
        if (self.fold) {
            withoutFold = copyOfDateTime(globalObject, args[0], self.tzinfoOrNone(), 0);
            RETURN_IF_EXCEPTION(scope, { });
        }
        JSValue offset = utcOffsetOf(globalObject, withoutFold);
        RETURN_IF_EXCEPTION(scope, { });
        // It comes down to the hash of something else.
        if (isNone(offset))
            self.hash = hashOfBytes(self.data);
        else {
            JSValue sinceTheStart = newTimeDelta(globalObject, dateToOrdinal(self.year(), self.month(), self.day()), self.hour() * 3600 + self.minute() * 60 + self.second(), self.microsecond(), true);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue inUTC = subtractTimeDeltas(globalObject, *tryTimeDelta(sinceTheStart), *tryTimeDelta(offset));
            RETURN_IF_EXCEPTION(scope, { });
            int64_t computed = hashOfTimeDelta(globalObject, *tryTimeDelta(inUTC));
            RETURN_IF_EXCEPTION(scope, { });
            self.hash = computed;
        }
    }
    return JSValue::encode(intFromInt64(globalObject, self.hash));
}

// datetime_datetime_replace_impl()
PYTHON_NATIVE(dateTimeReplace)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    int fields[7] = { self.year(), self.month(), self.day(), self.hour(), self.minute(), self.second(), self.microsecond() };
    for (unsigned i = 0; i < 7; ++i) {
        if (JSValue given = args.at(i + 1)) {
            auto converted = toCInt(globalObject, given);
            RETURN_IF_EXCEPTION(scope, { });
            fields[i] = *converted;
        }
    }
    JSValue tzinfo = args.at(8) ? args.at(8) : self.tzinfoOrNone();
    int fold = self.fold;
    if (JSValue given = args.at(9)) {
        auto converted = toCInt(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        fold = *converted;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newDateTimeOfClass(globalObject, fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], tzinfo, fold, typeOf(globalObject, args[0])->object())));
}

// local_timezone_from_timestamp(): a timezone that is how far the clocks here are from UTC at a time, and what they call it
JSValue localTimeZoneAt(JSGlobalObject* globalObject, time_t timestamp)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    struct tm tm;
    breakDownTime(globalObject, timestamp, BrokenDownAs::Local, tm);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue delta = newTimeDelta(globalObject, 0, static_cast<int>(tm.tm_gmtoff), 0, true);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue name;
    if (tm.tm_zone) {
        // PyUnicode_DecodeLocale()
        String decoded = decodeBytes(globalObject, byteCast<uint8_t>(unsafeSpan(tm.tm_zone)), "utf-8"_s, "surrogateescape"_s);
        RETURN_IF_EXCEPTION(scope, { });
        name = strOrMemoryError(globalObject, decoded);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, newTimeZone(globalObject, uncheckedDowncast<PyStateObject>(delta.asCell()), name));
}

// local_timezone(), of what is in UTC
JSValue localTimeZoneForUTC(JSGlobalObject* globalObject, JSValue utcTime)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue delta = subtractDateTimes(globalObject, utcTime, dateTimeModuleState(globalObject).epoch.get());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue oneSecond = newTimeDelta(globalObject, 0, 1, 0, false);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue seconds = floorDivideTimeDeltas(globalObject, *tryTimeDelta(delta), *tryTimeDelta(oneSecond));
    RETURN_IF_EXCEPTION(scope, { });
    // _PyLong_AsTime_t(). There are not so many seconds in ten thousand years.
    RELEASE_AND_RETURN(scope, localTimeZoneAt(globalObject, static_cast<time_t>(*tryInt64(seconds))));
}

// local_timezone_from_local(), of what is taken to be local time
JSValue localTimeZoneForLocal(JSGlobalObject* globalObject, const DateState& local)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int fold = local.fold;
    long long seconds = localToSeconds(globalObject, local, fold);
    RETURN_IF_EXCEPTION(scope, { });
    long long seconds2 = localToSeconds(globalObject, local, !fold);
    RETURN_IF_EXCEPTION(scope, { });
    // It is a time that the clocks skipped.
    if (seconds2 != seconds && (seconds2 > seconds) == fold)
        seconds = seconds2;
    RELEASE_AND_RETURN(scope, localTimeZoneAt(globalObject, static_cast<time_t>(seconds - epochSeconds)));
}

// datetime_astimezone()
PYTHON_NATIVE(dateTimeAsTimeZone)
{
    NATIVE_PROLOGUE();
    JSValue tzinfo = args.at(1) ? args.at(1) : jsUndefined();
    checkTZInfo(globalObject, tzinfo);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = stateOf<DateState>(args[0]);

    JSValue offset;
    for (JSValue tzinfoOfSelf = self.tzinfo.get(); ; tzinfoOfSelf = JSValue()) {
        // It is taken to be local time if it does not say, or has a tzinfo that does not.
        if (!tzinfoOfSelf) {
            tzinfoOfSelf = localTimeZoneForLocal(globalObject, self);
            RETURN_IF_EXCEPTION(scope, { });
        }
        // There is nothing to do if it is in that zone already.
        if (tzinfoOfSelf == tzinfo)
            return JSValue::encode(args[0]);
        offset = callUTCOffset(globalObject, tzinfoOfSelf, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(offset))
            break;
    }

    // To UTC: self - offset
    JSValue result = addTimeDeltaToDateTime(globalObject, args[0], *tryTimeDelta(offset), -1);
    RETURN_IF_EXCEPTION(scope, { });
    // A class derived from this one has been called to make it, and need not have made one of these.
    auto* inUTC = tryDateTime(result);
    if (!inUTC)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("expected a datetime, not "_s, typeName(globalObject, result))));
    JSValue utc = dateTimeModuleState(globalObject).utc.get();
    if (!inUTC->tzinfo) {
        result = copyOfDateTime(globalObject, result, utc, inUTC->fold);
        RETURN_IF_EXCEPTION(scope, { });
        inUTC = tryDateTime(result);
    } else
        inUTC->tzinfo.set(vm, result.asCell(), utc);

    // It is given the new tzinfo, whose fromutc() does the rest.
    if (isNone(tzinfo)) {
        tzinfo = localTimeZoneForUTC(globalObject, result);
        RETURN_IF_EXCEPTION(scope, { });
    }
    inUTC->tzinfo.set(vm, result.asCell(), tzinfo);
    RELEASE_AND_RETURN(scope, JSValue::encode(fromUTC(globalObject, tzinfo, result)));
}

// datetime_timetuple()
PYTHON_NATIVE(dateTimeTimeTuple)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    int dstFlag = -1;
    if (self.tzinfo) {
        JSValue dst = callDST(globalObject, self.tzinfo.get(), args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(dst))
            dstFlag = !!*tryTimeDelta(dst);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(buildStructTime(globalObject, self.year(), self.month(), self.day(), self.hour(), self.minute(), self.second(), dstFlag)));
}

// datetime_timestamp()
PYTHON_NATIVE(dateTimeTimestamp)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    if (self.tzinfo) {
        JSValue delta = subtractDateTimes(globalObject, args[0], dateTimeModuleState(globalObject).epoch.get());
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(totalSecondsOfTimeDelta(globalObject, *tryTimeDelta(delta))));
    }
    long long seconds = localToSeconds(globalObject, self, self.fold);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(floatFromDouble(seconds - epochSeconds + self.microsecond() / 1e6));
}

PYTHON_NATIVE(dateTimeGetDate)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    RELEASE_AND_RETURN(scope, JSValue::encode(newDate(globalObject, self.year(), self.month(), self.day())));
}

// datetime_gettime() and datetime_gettimetz()
PYTHON_NATIVE(dateTimeGetTime)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    bool keepsTZInfo = unpack<bool>(callFrame, 0);
    RELEASE_AND_RETURN(scope, JSValue::encode(newTimeOfDay(globalObject, self.hour(), self.minute(), self.second(), self.microsecond(), keepsTZInfo ? self.tzinfoOrNone() : jsUndefined(), self.fold)));
}

// datetime_utctimetuple()
PYTHON_NATIVE(dateTimeUTCTimeTuple)
{
    NATIVE_PROLOGUE();
    JSValue inUTC = args[0];
    if (JSValue tzinfo = stateOf<DateState>(args[0]).tzinfo.get()) {
        JSValue offset = callUTCOffset(globalObject, tzinfo, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(offset)) {
            inUTC = addTimeDeltaToDateTime(globalObject, args[0], *tryTimeDelta(offset), -1);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    auto* self = tryDateTime(inUTC);
    if (!self)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("expected a datetime, not "_s, typeName(globalObject, inUTC))));
    RELEASE_AND_RETURN(scope, JSValue::encode(buildStructTime(globalObject, self->year(), self->month(), self->day(), self->hour(), self->minute(), self->second(), 0)));
}

// datetime_reduce_ex() and datetime_reduce()
PYTHON_NATIVE(dateTimeReduce)
{
    NATIVE_PROLOGUE();
    int protocol = 2;
    if (unpack<bool>(callFrame, 0)) {
        auto converted = toCIntOfFormat(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        protocol = *converted;
    }
    // datetime_getstate()
    auto& self = stateOf<DateState>(args[0]);
    auto data = self.data;
    if (protocol > 3 && self.fold)
        data[2] |= 1 << 7;
    JSValue base = newBytes(globalObject, std::span<const uint8_t>(data));
    PyTuple* state = self.tzinfo ? PyTuple::create(globalObject, { base, self.tzinfo.get() }) : PyTuple::create(globalObject, { base });
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, args[0])->object(), state }));
}

} // namespace

void initializeDateTimeType(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto& state = dateTimeModuleState(globalObject);
    PyType* type = createBuiltinType(globalObject, "datetime.datetime"_s, state.dateType.get(), PyType::Layout::Native, PyType::IsBaseType);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    state.dateTimeType.set(vm, realm, type);
    addMethods(globalObject, type, {
        { "__new__"_s, dateTimeNew, Kind::New, 0, "?($type, /, year, month, day, hour=0, minute=0, second=0, microsecond=0, tzinfo=None, *, fold=0)"_s, Arguments::AreThoseOfTheClassButNotChecked },
        { "now"_s, dateTimeNow, Kind::ClassMethod },
        { "utcnow"_s, dateTimeUTCNow, Kind::ClassMethod },
        { "fromtimestamp"_s, dateTimeFromTimestamp, Kind::ClassMethod, 0, "($type, /, timestamp, tz=None)"_s },
        { "utcfromtimestamp"_s, dateTimeUTCFromTimestamp, Kind::ClassMethod, 0, "($type, timestamp, /)"_s, Arguments::AreCheckedAsByParseTuple },
        { "strptime"_s, dateTimeStrptime, Kind::ClassMethod, 0, "($type, string, format, /)"_s, Arguments::AreCheckedAsByParseTuple },
        { "combine"_s, dateTimeCombine, Kind::ClassMethod, 0, "($type, /, date, time, tzinfo=<unrepresentable>)"_s, Arguments::AreNotChecked },
        { "fromisoformat"_s, dateTimeFromISOFormat, Kind::ClassMethod },
        { "date"_s, dateTimeGetDate },
        { "time"_s, dateTimeGetTime, Kind::Method, pack(false) },
        { "timetz"_s, dateTimeGetTime, Kind::Method, pack(true) },
        { "ctime"_s, dateTimeCTime },
        { "timetuple"_s, dateTimeTimeTuple },
        { "timestamp"_s, dateTimeTimestamp },
        { "utctimetuple"_s, dateTimeUTCTimeTuple },
        { "isoformat"_s, dateTimeISOFormat, Kind::Method, 0, "($self, /, sep='T', timespec='auto')"_s },
        { "utcoffset"_s, dateTimeUTCOffset },
        { "tzname"_s, dateTimeTZName },
        { "dst"_s, dateTimeDST },
        { "replace"_s, dateTimeReplace },
        { "__replace__"_s, dateTimeReplace, Kind::Method, 0,
            "replace($self, /, year=unchanged, month=unchanged, day=unchanged, hour=unchanged, minute=unchanged, second=unchanged, microsecond=unchanged, tzinfo=unchanged, *, fold=unchanged)"_s },
        { "astimezone"_s, dateTimeAsTimeZone, Kind::Method, 0, "($self, /, tz=None)"_s },
        { "__reduce_ex__"_s, dateTimeReduce, Kind::Method, pack(true), "($self, protocol, /)"_s, Arguments::AreCheckedAsByParseTuple },
        { "__reduce__"_s, dateTimeReduce, Kind::Method, pack(false) },
        { "__repr__"_s, dateTimeRepr },
        { "__str__"_s, dateTimeStr },
        { "__hash__"_s, dateTimeHash },
        { "__add__"_s, dateTimeAdd, Kind::Wrapper, pack(false) },
        { "__radd__"_s, dateTimeAdd, Kind::Wrapper, pack(true) },
        { "__sub__"_s, dateTimeSubtract, Kind::Wrapper, pack(false) },
        { "__rsub__"_s, dateTimeSubtract, Kind::Wrapper, pack(true) },
    });
    addComparisons(globalObject, type, dateTimeCompare);
    addGetSet(globalObject, type, "hour"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<DateState>(self).hour()); });
    addGetSet(globalObject, type, "minute"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<DateState>(self).minute()); });
    addGetSet(globalObject, type, "second"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<DateState>(self).second()); });
    addGetSet(globalObject, type, "microsecond"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<DateState>(self).microsecond()); });
    addGetSet(globalObject, type, "tzinfo"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<DateState>(self).tzinfoOrNone(); });
    addGetSet(globalObject, type, "fold"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<DateState>(self).fold); });
}

} } // namespace JSC::Python
