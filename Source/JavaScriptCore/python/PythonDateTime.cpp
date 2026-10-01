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
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSignatures.h"
#include "PythonText.h"
#include <time.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace Python {

DateTimeModuleState& dateTimeModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<DateTimeModuleState>(); }

bool isTZInfo(JSGlobalObject* globalObject, JSValue value) { return isInstance(globalObject, value, dateTimeModuleState(globalObject).tzinfoType.get()); }

std::pair<JSValue, JSValue> operandsOfSlot(CallFrame* callFrame)
{
    bool isReflected = unpack<bool>(callFrame, 0);
    return { callFrame->uncheckedArgument(isReflected), callFrame->uncheckedArgument(!isReflected) };
}

// ---- Math utilities

int floorDivide(int x, int y, int& remainder)
{
    ASSERT(y > 0);
    int quotient = x / y;
    remainder = x - quotient * y;
    if (remainder < 0) {
        --quotient;
        remainder += y;
    }
    ASSERT(0 <= remainder && remainder < y);
    return quotient;
}

// ---- General calendrical helper functions

// For each month, counted from 1, how many days there are in it and how many before it in the year, in a year that is not a leap year.
static constexpr int daysInMonthTable[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static constexpr int daysBeforeMonthTable[] = { 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

bool isLeapYear(int year)
{
    unsigned unsignedYear = static_cast<unsigned>(year);
    return !(unsignedYear % 4) && (unsignedYear % 100 || !(unsignedYear % 400));
}

int daysInMonth(int year, int month)
{
    ASSERT(month >= 1 && month <= 12);
    return month == 2 && isLeapYear(year) ? 29 : daysInMonthTable[month];
}

int daysBeforeMonth(int year, int month)
{
    ASSERT(month >= 1 && month <= 12);
    return daysBeforeMonthTable[month] + (month > 2 && isLeapYear(year));
}

int daysBeforeYear(int year)
{
    ASSERT(year >= 1);
    int y = year - 1;
    return y * 365 + y / 4 - y / 100 + y / 400;
}

static constexpr int daysIn4Years = 1461; // daysBeforeYear(5)
static constexpr int daysIn100Years = 36524; // daysBeforeYear(101)
static constexpr int daysIn400Years = 146097; // daysBeforeYear(401)
static_assert(daysIn4Years == 4 * 365 + 1);
static_assert(daysIn400Years == 4 * daysIn100Years + 1);
static_assert(daysIn100Years == 25 * daysIn4Years - 1);

void ordinalToDate(int ordinal, int& year, int& month, int& day)
{
    // The pattern of leap years repeats every 400 years. With 1 taken from it, the ordinal is divisible by that many days at each such boundary.
    ASSERT(ordinal >= 1);
    --ordinal;
    int n400 = ordinal / daysIn400Years;
    int n = ordinal % daysIn400Years;
    year = n400 * 400 + 1;

    // n is how many days it is after 1 January of that year. n100 can be 4, and then it is 31 December at the end of a cycle of 400 years.
    int n100 = n / daysIn100Years;
    n %= daysIn100Years;
    int n4 = n / daysIn4Years;
    n %= daysIn4Years;
    // Likewise n1, at the end of a cycle of 4
    int n1 = n / 365;
    n %= 365;

    year += n100 * 100 + n4 * 4 + n1;
    if (n1 == 4 || n100 == 4) {
        ASSERT(!n);
        year -= 1;
        month = 12;
        day = 31;
        return;
    }

    // The year is right, and n is how many days after 1 January. The month is found by an estimate that is right or one too many.
    bool isLeap = n1 == 3 && (n4 != 24 || n100 == 3);
    ASSERT(isLeap == isLeapYear(year));
    month = (n + 50) >> 5;
    int preceding = daysBeforeMonthTable[month] + (month > 2 && isLeap);
    if (preceding > n) {
        month -= 1;
        preceding -= daysInMonth(year, month);
    }
    n -= preceding;
    ASSERT(0 <= n && n < daysInMonth(year, month));
    day = n + 1;
}

int dateToOrdinal(int year, int month, int day) { return daysBeforeYear(year) + daysBeforeMonth(year, month) + day; }

int weekdayOf(int year, int month, int day) { return (dateToOrdinal(year, month, day) + 6) % 7; }

int isoWeek1Monday(int year)
{
    int firstDay = dateToOrdinal(year, 1, 1);
    int firstWeekday = (firstDay + 6) % 7; // 0 if 1 January is a Monday
    int week1Monday = firstDay - firstWeekday; // The Monday at or before it
    if (firstWeekday > 3) // It is a Friday, a Saturday or a Sunday.
        week1Monday += 7;
    return week1Monday;
}

int isoToDate(int isoYear, int isoWeek, int isoDay, int& year, int& month, int& day)
{
    if (isoYear < minYear || isoYear > maxYear)
        return -4;
    if (isoWeek <= 0 || isoWeek >= 53) {
        bool isOutOfRange = true;
        if (isoWeek == 53) {
            // A year has 53 weeks if it starts on a Thursday, or is a leap year and starts on a Wednesday.
            int firstWeekday = weekdayOf(isoYear, 1, 1);
            if (firstWeekday == 3 || (firstWeekday == 2 && isLeapYear(isoYear)))
                isOutOfRange = false;
        }
        if (isOutOfRange)
            return -2;
    }
    if (isoDay <= 0 || isoDay >= 8)
        return -3;
    int day1 = isoWeek1Monday(isoYear);
    int dayOffset = (isoWeek - 1) * 7 + isoDay - 1;
    ordinalToDate(day1 + dayOffset, year, month, day);
    return 0;
}

// ---- Range checkers

void checkDateArguments(JSGlobalObject* globalObject, int year, int month, int day)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (year < minYear || year > maxYear) {
        raiseValueError(globalObject, scope, concatenate("year must be in "_s, minYear, ".."_s, maxYear, ", not "_s, year));
        return;
    }
    if (month < 1 || month > 12) {
        raiseValueError(globalObject, scope, concatenate("month must be in 1..12, not "_s, month));
        return;
    }
    int inMonth = daysInMonth(year, month);
    if (day < 1 || day > inMonth)
        raiseValueError(globalObject, scope, concatenate("day "_s, day, " must be in range 1.."_s, inMonth, " for month "_s, month, " in year "_s, year));
}

void checkTimeArguments(JSGlobalObject* globalObject, int hour, int minute, int second, int microsecond, int fold)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (hour < 0 || hour > 23)
        raiseValueError(globalObject, scope, concatenate("hour must be in 0..23, not "_s, hour));
    else if (minute < 0 || minute > 59)
        raiseValueError(globalObject, scope, concatenate("minute must be in 0..59, not "_s, minute));
    else if (second < 0 || second > 59)
        raiseValueError(globalObject, scope, concatenate("second must be in 0..59, not "_s, second));
    else if (microsecond < 0 || microsecond > 999999)
        raiseValueError(globalObject, scope, concatenate("microsecond must be in 0..999999, not "_s, microsecond));
    else if (fold && fold != 1)
        raiseValueError(globalObject, scope, concatenate("fold must be either 0 or 1, not "_s, fold));
}

// ---- Normalization utilities

// normalize_pair(): what is too much or too little in `low` is carried to `high`.
static void normalizePair(int& high, int& low, int factor)
{
    ASSERT(factor > 0);
    if (low < 0 || low >= factor)
        high += floorDivide(low, factor, low);
    ASSERT(0 <= low && low < factor);
}

void normalizeDelta(int& days, int& seconds, int& microseconds)
{
    if (microseconds < 0 || microseconds >= 1000000)
        normalizePair(seconds, microseconds, 1000000);
    if (seconds < 0 || seconds >= 24 * 3600)
        normalizePair(days, seconds, 24 * 3600);
}

void normalizeDate(JSGlobalObject* globalObject, int& year, int& month, int& day)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto raiseOutOfRange = [&] { raise(globalObject, scope, BuiltinType::OverflowError, "date value out of range"_s); };
    // The month is out of a date, and only the day can be out of bounds. So can the year of a datetime, which is seen to at the end.
    ASSERT(1 <= month && month <= 12);
    int inMonth = daysInMonth(year, month);
    if (day < 1 || day > inMonth) {
        // It is cheap if it is only a day out, and a time zone cannot make it more.
        if (!day) {
            --month;
            if (month > 0)
                day = daysInMonth(year, month);
            else {
                --year;
                month = 12;
                day = 31;
            }
        } else if (day == inMonth + 1) {
            ++month;
            day = 1;
            if (month > 12) {
                month = 1;
                ++year;
            }
        } else {
            int ordinal = dateToOrdinal(year, month, 1) + day - 1;
            if (ordinal < 1 || ordinal > maxOrdinal)
                return raiseOutOfRange();
            ordinalToDate(ordinal, year, month, day);
            return;
        }
    }
    ASSERT(month > 0 && day > 0);
    if (year < minYear || year > maxYear)
        raiseOutOfRange();
}

void normalizeDateTime(JSGlobalObject* globalObject, int& year, int& month, int& day, int& hour, int& minute, int& second, int& microsecond)
{
    normalizePair(second, microsecond, 1000000);
    normalizePair(minute, second, 60);
    normalizePair(hour, minute, 60);
    normalizePair(day, hour, 24);
    normalizeDate(globalObject, year, month, day);
}

// ---- String parsing utilities and helper functions

static bool isDigit(char c) { return static_cast<unsigned>(c - '0') < 10; }

// parse_digits(): null if they are not all digits.
static const char* parseDigits(const char* pointer, int& value, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        unsigned digit = static_cast<unsigned>(*pointer++ - '0');
        if (digit > 9)
            return nullptr;
        value *= 10;
        value += static_cast<int>(digit);
    }
    return pointer;
}

int parseISODate(const char* text, size_t length, int& year, int& month, int& day)
{
    // 0 if it will do. -1 if a part is no number, -2 if there is a separator in one place and not in the other, -3 and -4 if the week or the day of the week is no number, and from -5 to -7 what isoToDate() made of them.
    const char* p = parseDigits(text, year, 4);
    if (!p)
        return -1;

    bool usesSeparator = *p == '-';
    if (usesSeparator)
        ++p;

    if (*p == 'W') {
        // As isocalendar() has it
        ++p;
        int isoWeek = 0;
        int isoDay = 0;
        p = parseDigits(p, isoWeek, 2);
        if (!p)
            return -3;
        ASSERT(p > text);
        if (static_cast<size_t>(p - text) < length) {
            if (usesSeparator && *p++ != '-')
                return -2;
            p = parseDigits(p, isoDay, 1);
            if (!p)
                return -4;
        } else
            isoDay = 1;
        int result = isoToDate(year, isoWeek, isoDay, year, month, day);
        return result ? -3 + result : 0;
    }

    p = parseDigits(p, month, 2);
    if (!p)
        return -1;
    if (usesSeparator && *p++ != '-')
        return -2;
    p = parseDigits(p, day, 2);
    return p ? 0 : -1;
}

// parse_hh_mm_ss_ff()
static int parseHoursToFraction(const char* text, const char* end, int& hour, int& minute, int& second, int& microsecond)
{
    hour = minute = second = microsecond = 0;
    const char* p = text;
    int* values[3] = { &hour, &minute, &second };
    bool hasSeparator = true;

    // [HH[:?MM[:?SS]]]
    for (size_t i = 0; i < 3; ++i) {
        p = parseDigits(p, *values[i], 2);
        if (!p)
            return -3;
        char c = *p++;
        if (!i)
            hasSeparator = c == ':';
        if (c == '.' || c == ',') {
            if (i < 2)
                return -3; // After the hour or the minute
            if (p >= end)
                return -3; // With no digit after it
            break;
        }
        if (p >= end)
            return c != '\0';
        if (hasSeparator && c == ':') {
            if (i == 2)
                return -4; // Where the fraction is to begin
            continue;
        }
        if (!hasSeparator)
            --p;
        else
            return -4;
    }

    // The fraction
    size_t remaining = end - p;
    size_t toParse = std::min<size_t>(remaining, 6);
    p = parseDigits(p, microsecond, toParse);
    if (!p)
        return -3;
    static constexpr int correction[] = { 100000, 10000, 1000, 100, 10 };
    if (toParse < 6)
        microsecond *= correction[toParse - 1];
    // Any more digits are let go.
    while (isDigit(*p))
        ++p;
    return *p != '\0';
}

int parseISOTime(const char* text, size_t length, int& hour, int& minute, int& second, int& microsecond, int& offset, int& offsetMicrosecond)
{
    // 0 if it will do and says nothing of a zone, and 1 if it does. -3 if a part is no number, -4 if a separator is amiss, -5 if the zone is.
    const char* end = text + length;
    const char* zone = text;
    do {
        if (*zone == 'Z' || *zone == '+' || *zone == '-')
            break;
    } while (++zone < end);

    int result = parseHoursToFraction(text, zone, hour, minute, second, microsecond);
    if (result < 0)
        return result;
    if (zone == end) {
        // There is no zone, so there is to be nothing more.
        return result == 1 ? -5 : 0;
    }

    if (*zone == 'Z') {
        offset = 0;
        offsetMicrosecond = 0;
        return zone[1] != '\0' ? -5 : 1;
    }

    int sign = *zone == '-' ? -1 : 1;
    ++zone;
    int zoneHour = 0;
    int zoneMinute = 0;
    int zoneSecond = 0;
    result = parseHoursToFraction(zone, end, zoneHour, zoneMinute, zoneSecond, offsetMicrosecond);
    offset = sign * (zoneHour * 3600 + zoneMinute * 60 + zoneSecond);
    offsetMicrosecond *= sign;
    return result ? -5 : 1;
}

std::optional<CString> utf8ForParsing(const String& text)
{
    auto converted = text.tryGetUTF8(StrictConversion);
    if (!converted)
        return std::nullopt;
    // There is a null at the end of it even if there is nothing before that.
    if (converted->isNull())
        return CString(""_span);
    return CString(WTF::move(converted.value()));
}

JSValue raiseInvalidISOFormat(JSGlobalObject* globalObject, ThrowScope& scope, JSValue given)
{
    String shown = repr(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseValueError(globalObject, scope, concatenate("Invalid isoformat string: "_s, shown));
}

JSValue tzinfoFromISOFormat(JSGlobalObject* globalObject, int result, int offset, int offsetMicrosecond)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (result != 1)
        return jsUndefined();
    if (!offset && !offsetMicrosecond)
        return dateTimeModuleState(globalObject).utc.get();
    JSValue delta = newTimeDelta(globalObject, 0, offset, offsetMicrosecond, true);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, newTimeZone(globalObject, uncheckedDowncast<PyStateObject>(delta.asCell()), JSValue()));
}

// ---- tzinfo helpers

void checkTZInfo(JSGlobalObject* globalObject, JSValue value)
{
    if (isNone(value) || isTZInfo(globalObject, value))
        return;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    raiseTypeError(globalObject, scope, concatenate("tzinfo argument must be None or of a tzinfo subclass, not type '"_s, typeName(globalObject, value), '\''));
}

// Whether a timedelta is strictly between -timedelta(hours=24) and timedelta(hours=24)
static bool isWithinADay(const TimeDeltaState& offset)
{
    return !((offset.days == -1 && !offset.seconds && offset.microseconds < 1) || offset.days < -1 || offset.days >= 1);
}

static JSValue raiseOffsetOutOfRange(JSGlobalObject* globalObject, ThrowScope& scope, JSValue offset)
{
    String shown = repr(globalObject, offset);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseValueError(globalObject, scope, concatenate("offset must be a timedelta strictly between -timedelta(hours=24) and timedelta(hours=24), not "_s, shown));
}

// PyObject_CallMethod(), which has its own way of saying that what goes by the name cannot be called
template<typename... Arguments>
static JSValue callMethodOrComplain(JSGlobalObject* globalObject, JSValue object, const Identifier& name, Arguments... arguments)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue callable = getAttribute(globalObject, object, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isCallable(globalObject, callable))
        return raiseTypeError(globalObject, scope, concatenate("attribute of type '"_s, typeName(globalObject, callable), "' is not callable"_s));
    RELEASE_AND_RETURN(scope, call(globalObject, callable, arguments...));
}

// call_tzinfo_method()
static JSValue callTZInfoMethod(JSGlobalObject* globalObject, JSValue tzinfo, const Identifier& name, JSValue argument)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isNone(tzinfo))
        return jsUndefined();
    JSValue offset = callMethodOrComplain(globalObject, tzinfo, name, argument);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(offset))
        return jsUndefined();
    auto* delta = tryTimeDelta(offset);
    if (!delta)
        return raiseTypeError(globalObject, scope, concatenate("tzinfo."_s, name.string(), "() must return None or timedelta, not '"_s, typeName(globalObject, offset), '\''));
    if (!isWithinADay(*delta))
        RELEASE_AND_RETURN(scope, raiseOffsetOutOfRange(globalObject, scope, offset));
    return offset;
}

JSValue callUTCOffset(JSGlobalObject* globalObject, JSValue tzinfo, JSValue argument) { return callTZInfoMethod(globalObject, tzinfo, globalObject->vm().pythonNames().attribute_utcoffset, argument); }
JSValue callDST(JSGlobalObject* globalObject, JSValue tzinfo, JSValue argument) { return callTZInfoMethod(globalObject, tzinfo, globalObject->vm().pythonNames().attribute_dst, argument); }

JSValue callTZName(JSGlobalObject* globalObject, JSValue tzinfo, JSValue argument)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isNone(tzinfo))
        return jsUndefined();
    JSValue result = callMethodNamed(globalObject, tzinfo, globalObject->vm().pythonNames().attribute_tzname, argument);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(result))
        return jsUndefined();
    if (!stringIn(result))
        return raiseTypeError(globalObject, scope, concatenate("tzinfo.tzname() must return None or a string, not '"_s, typeName(globalObject, result), '\''));
    return result;
}

JSValue createTimeZone(JSGlobalObject* globalObject, PyStateObject* offset, JSValue name)
{
    VM& vm = globalObject->vm();
    auto& state = dateTimeModuleState(globalObject);
    // look_up_timezone()
    if (offset == state.zeroDelta.get() && !name && state.utc)
        return state.utc.get();
    auto* object = PyStateObject::create(vm, state.timeZoneType->instanceStructure(), makeUnique<TimeZoneState>());
    auto& self = object->state<TimeZoneState>();
    self.offset.set(vm, object, offset);
    if (name)
        self.name.set(vm, object, name);
    return object;
}

JSValue newTimeZone(JSGlobalObject* globalObject, PyStateObject* offset, JSValue name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& delta = offset->state<TimeDeltaState>();
    if (!name && !delta)
        return dateTimeModuleState(globalObject).utc.get();
    if (!isWithinADay(delta))
        RELEASE_AND_RETURN(scope, raiseOffsetOutOfRange(globalObject, scope, offset));
    return createTimeZone(globalObject, offset, name);
}

// ---- repr() helpers

String withTZInfoKeyword(JSGlobalObject* globalObject, const String& text, JSValue tzinfo)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isNone(tzinfo))
        return text;
    ASSERT(text.endsWith(')'));
    String shown = repr(globalObject, tzinfo);
    RETURN_IF_EXCEPTION(scope, { });
    return concatenate(StringView(text).left(text.length() - 1), ", tzinfo="_s, shown, ')');
}

String withFoldKeyword(const String& text, int fold)
{
    if (!fold)
        return text;
    ASSERT(text.endsWith(')'));
    return concatenate(StringView(text).left(text.length() - 1), ", fold="_s, fold, ')');
}

// ---- String format helpers

String formatCTime(const DateState& date, int hours, int minutes, int seconds)
{
    static constexpr ASCIILiteral dayNames[] = { "Mon"_s, "Tue"_s, "Wed"_s, "Thu"_s, "Fri"_s, "Sat"_s, "Sun"_s };
    static constexpr ASCIILiteral monthNames[] = { "Jan"_s, "Feb"_s, "Mar"_s, "Apr"_s, "May"_s, "Jun"_s, "Jul"_s, "Aug"_s, "Sep"_s, "Oct"_s, "Nov"_s, "Dec"_s };
    return makeString(dayNames[weekdayOf(date.year(), date.month(), date.day())], ' ', monthNames[date.month() - 1], ' ', pad(' ', 2, date.day()), ' ', pad('0', 2, hours), ':', pad('0', 2, minutes), ':', pad('0', 2, seconds), ' ',
        pad('0', 4, date.year()));
}

String formatUTCOffset(JSGlobalObject* globalObject, ASCIILiteral separator, JSValue tzinfo, JSValue argument)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue offset = callUTCOffset(globalObject, tzinfo, argument);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(offset))
        return emptyString();
    // It is normalized, so it is less than nothing if its days are.
    char sign = '+';
    if (tryTimeDelta(offset)->days < 0) {
        sign = '-';
        offset = negateTimeDelta(globalObject, *tryTimeDelta(offset));
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto& delta = *tryTimeDelta(offset);
    int seconds;
    int minutes = floorDivide(delta.seconds, 60, seconds);
    int hours = floorDivide(minutes, 60, minutes);
    if (delta.microseconds)
        return makeString(sign, pad('0', 2, hours), separator, pad('0', 2, minutes), separator, pad('0', 2, seconds), '.', pad('0', 6, delta.microseconds));
    if (seconds)
        return makeString(sign, pad('0', 2, hours), separator, pad('0', 2, minutes), separator, pad('0', 2, seconds));
    return makeString(sign, pad('0', 2, hours), separator, pad('0', 2, minutes));
}

// get_tzinfo_member(): empty if it has none.
static JSValue tzinfoMemberOf(JSValue object)
{
    if (auto* dateTime = tryDateTime(object))
        return dateTime->tzinfo.get();
    if (auto* time = tryTimeOfDay(object))
        return time->tzinfo.get();
    return { };
}

// normalize_century(): whether the system's strftime() leaves a year before 1000 shorter than four digits
static bool normalizesCentury()
{
    static const bool result = [] {
        char year[5];
        struct tm date { };
        date.tm_year = -1801;
        date.tm_mon = 0;
        date.tm_mday = 1;
        return strftime(year, sizeof(year), "%Y", &date) && strcmp(year, "0099");
    }();
    return result;
}

// make_somezreplacement(). Null if it raised.
static String makeOffsetReplacement(JSGlobalObject* globalObject, JSValue object, ASCIILiteral separator, JSValue tzinfoArgument)
{
    JSValue tzinfo = tzinfoMemberOf(object);
    if (!tzinfo || isNone(tzinfo))
        return emptyString();
    return formatUTCOffset(globalObject, separator, tzinfo, tzinfoArgument);
}

// make_Zreplacement(). Null if it raised.
static String makeNameReplacement(JSGlobalObject* globalObject, JSValue object, JSValue tzinfoArgument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue tzinfo = tzinfoMemberOf(object);
    if (!tzinfo || isNone(tzinfo))
        return emptyString();
    JSValue name = callTZName(globalObject, tzinfo, tzinfoArgument);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(name))
        return emptyString();
    // It goes into the format, so that any % in it is doubled for strftime() not to make something of it. It may be of a class derived from str, which is asked.
    JSValue replaced = callMethodOrComplain(globalObject, name, vm.pythonNames().attribute_replace, jsString(vm, String("%"_s)), jsNontrivialString(vm, "%%"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSString* string = stringIn(replaced);
    if (!string) {
        raiseTypeError(globalObject, scope, "tzname.replace() did not return a string"_s);
        return { };
    }
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return text.isNull() ? emptyString() : text;
}

// make_freplacement()
static String makeFractionReplacement(JSValue object)
{
    int microsecond = 0;
    if (auto* time = tryTimeOfDay(object))
        microsecond = time->microsecond();
    else if (auto* dateTime = tryDateTime(object))
        microsecond = dateTime->microsecond();
    return makeString(pad('0', 6, microsecond));
}

JSValue wrapStrftime(JSGlobalObject* globalObject, JSValue object, JSString* format, JSValue timeTuple, JSValue tzinfoArgument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue strftime = importModuleAttribute(globalObject, "time"_s, "strftime"_s);
    RETURN_IF_EXCEPTION(scope, { });

    // What takes the place of %z, %:z, %Z and %f. Each takes some working out, so none is until it is wanted.
    String offsetReplacement;
    String colonOffsetReplacement;
    String nameReplacement;
    String fractionReplacement;

    String text = format->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    TextBuilder builder;
    size_t length = text.length();
    size_t i = 0;
    size_t start = 0;
    while (i != length) {
        i = text.find('%', i);
        if (i == notFound)
            break;
        size_t end = i;
        ++i;
        if (i == length)
            break;
        char16_t character = text[i];
        ++i;
        const String* replacement = nullptr;
        if (character == 'z') {
            if (offsetReplacement.isNull()) {
                offsetReplacement = makeOffsetReplacement(globalObject, object, ""_s, tzinfoArgument);
                RETURN_IF_EXCEPTION(scope, { });
            }
            replacement = &offsetReplacement;
        } else if (character == ':' && i < length && text[i] == 'z') {
            ++i;
            if (colonOffsetReplacement.isNull()) {
                colonOffsetReplacement = makeOffsetReplacement(globalObject, object, ":"_s, tzinfoArgument);
                RETURN_IF_EXCEPTION(scope, { });
            }
            replacement = &colonOffsetReplacement;
        } else if (character == 'Z') {
            if (nameReplacement.isNull()) {
                nameReplacement = makeNameReplacement(globalObject, object, tzinfoArgument);
                RETURN_IF_EXCEPTION(scope, { });
            }
            replacement = &nameReplacement;
        } else if (character == 'f') {
            if (fractionReplacement.isNull())
                fractionReplacement = makeFractionReplacement(object);
            replacement = &fractionReplacement;
        } else if (normalizesCentury() && (character == 'Y' || character == 'G' || character == 'F' || character == 'C')) {
            // The year is filled out with noughts.
            JSValue item = sequenceItem(globalObject, timeTuple, 0);
            RETURN_IF_EXCEPTION(scope, { });
            auto year = toCLong(globalObject, item);
            RETURN_IF_EXCEPTION(scope, { });
            // datetime(1000, 1, 1).strftime('%G') == '1000'
            if (*year >= 1000)
                continue;
            if (character == 'G') {
                JSValue yearString = call(globalObject, strftime, jsNontrivialString(vm, "%G"_s), timeTuple);
                RETURN_IF_EXCEPTION(scope, { });
                JSValue yearNumber = numberLong(globalObject, yearString);
                RETURN_IF_EXCEPTION(scope, { });
                year = toCLong(globalObject, yearNumber);
                RETURN_IF_EXCEPTION(scope, { });
            }
            // "%04ld"
            String digits = *year < 0 ? makeString('-', pad('0', 3, static_cast<uint64_t>(-*year))) : makeString(pad('0', 4, static_cast<uint64_t>(*year)));
            builder.append(StringView(text).substring(start, end - start));
            start = i;
            if (character == 'F')
                builder.append(digits, "-%m-%d"_s);
            else if (character == 'C')
                builder.append(StringView(digits).left(digits.length() - 2));
            else
                builder.append(digits);
            continue;
        } else
            continue;
        builder.append(StringView(text).substring(start, end - start));
        start = i;
        builder.append(*replacement);
    }

    JSValue newFormat = format;
    if (start) {
        builder.append(StringView(text).substring(start));
        newFormat = strOrMemoryError(globalObject, builder.tryFinish());
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, call(globalObject, strftime, newFormat, timeTuple));
}

JSValue buildStructTime(JSGlobalObject* globalObject, int year, int month, int day, int hour, int minute, int second, int dstFlag)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue structTime = importModuleAttribute(globalObject, "time"_s, "struct_time"_s);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* fields = PyTuple::create(globalObject, { jsNumber(year), jsNumber(month), jsNumber(day), jsNumber(hour), jsNumber(minute), jsNumber(second), jsNumber(weekdayOf(year, month, day)),
        jsNumber(daysBeforeMonth(year, month) + day), jsNumber(dstFlag) });
    RELEASE_AND_RETURN(scope, call(globalObject, structTime, fields));
}

JSValue callStrptime(JSGlobalObject* globalObject, ASCIILiteral function, JSValue cls, JSValue string, JSValue format)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, "_strptime"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue callee = getAttribute(globalObject, module, Identifier::fromString(vm, function));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, callee, cls, string, format));
}

// ---- Miscellaneous helpers

void parseIntArguments(JSGlobalObject* globalObject, const NativeArguments& args, unsigned first, std::span<int> fields, unsigned required)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    for (unsigned i = 0; i < fields.size(); ++i) {
        JSValue given = args.at(first + i);
        if (!given) {
            if (i < required) {
                // It says which is missing.
                scope.release();
                checkArgumentsSlow(globalObject, args.callFrame());
                return;
            }
            continue;
        }
        auto converted = toCIntOfFormat(globalObject, given);
        RETURN_IF_EXCEPTION(scope, void());
        fields[i] = *converted;
    }
}

JSValue comparisonResult(int difference, ComparisonOperator op)
{
    switch (op) {
    case ComparisonOperator::Eq:
        return jsBoolean(!difference);
    case ComparisonOperator::NotEq:
        return jsBoolean(difference);
    case ComparisonOperator::Lt:
        return jsBoolean(difference < 0);
    case ComparisonOperator::LtE:
        return jsBoolean(difference <= 0);
    case ComparisonOperator::Gt:
        return jsBoolean(difference > 0);
    case ComparisonOperator::GtE:
        return jsBoolean(difference >= 0);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

int compareBytes(std::span<const uint8_t> left, std::span<const uint8_t> right)
{
    ASSERT(left.size() == right.size());
    return memcmp(left.data(), right.data(), left.size());
}

std::optional<Vector<uint8_t, DateState::dateTimeSize>> pickledState(JSGlobalObject* globalObject, JSValue value, size_t length, unsigned indexToLookAt, bool (*isSane)(unsigned), ASCIILiteral what)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Vector<uint8_t, DateState::dateTimeSize> state;
    if (isBytes(value)) {
        auto bytes = *builtinBufferOf(value);
        if (bytes.size() != length || !isSane(bytes[indexToLookAt]))
            return std::nullopt;
        state.append(bytes);
        return state;
    }
    JSString* string = stringIn(value);
    if (!string)
        return std::nullopt;
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    // As many characters, which is as many units unless some are not Latin-1. What is looked at is a character, and so are those before it.
    Vector<char32_t, DateState::dateTimeSize> characters;
    for (char32_t character : StringView(text).codePoints()) {
        if (characters.size() == length)
            return std::nullopt;
        characters.append(character);
    }
    if (characters.size() != length || !isSane(characters[indexToLookAt]))
        return std::nullopt;
    for (char32_t character : characters) {
        if (character > 0xFF) {
            raiseValueError(globalObject, scope, concatenate("Failed to encode latin1 string when unpickling a "_s, what, " object. pickle.load(data, encoding='latin1') is assumed."_s));
            return std::nullopt;
        }
        state.append(static_cast<uint8_t>(character));
    }
    return state;
}

// ---- The module

JSObject* createDateTimeModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = dateTimeModuleState(globalObject);
    if (!state.deltaType) {
        // _PyDateTime_InitTypes(). A class comes before what is derived from it, and before what is made of it.
        initializeTimeDeltaType(globalObject);
        initializeTimeZoneTypes(globalObject);
        initializeDateTypes(globalObject);
        initializeDateTimeType(globalObject);
        initializeTimeOfDayType(globalObject);

        auto set = [&] (WriteBarrier<PyType>& type, ASCIILiteral name, JSValue value) {
            ASSERT(value);
            type->putDirect(vm, Identifier::fromString(vm, name), value);
        };
        auto asObject = [] (JSValue value) { return uncheckedDowncast<PyStateObject>(value.asCell()); };
        state.zeroDelta.set(vm, realm, asObject(newTimeDelta(globalObject, 0, 0, 0, false)));
        state.utc.set(vm, realm, asObject(createTimeZone(globalObject, state.zeroDelta.get(), JSValue())));

        set(state.deltaType, "resolution"_s, newTimeDelta(globalObject, 0, 0, 1, false));
        set(state.deltaType, "min"_s, newTimeDelta(globalObject, -maxDeltaDays, 0, 0, false));
        set(state.deltaType, "max"_s, newTimeDelta(globalObject, maxDeltaDays, 24 * 3600 - 1, 1000000 - 1, false));

        set(state.dateType, "min"_s, newDate(globalObject, 1, 1, 1));
        set(state.dateType, "max"_s, newDate(globalObject, maxYear, 12, 31));
        set(state.dateType, "resolution"_s, newTimeDelta(globalObject, 1, 0, 0, false));

        set(state.timeType, "min"_s, newTimeOfDay(globalObject, 0, 0, 0, 0, jsUndefined(), 0));
        set(state.timeType, "max"_s, newTimeOfDay(globalObject, 23, 59, 59, 999999, jsUndefined(), 0));
        set(state.timeType, "resolution"_s, newTimeDelta(globalObject, 0, 0, 1, false));

        set(state.dateTimeType, "min"_s, newDateTime(globalObject, 1, 1, 1, 0, 0, 0, 0, jsUndefined(), 0));
        set(state.dateTimeType, "max"_s, newDateTime(globalObject, maxYear, 12, 31, 23, 59, 59, 999999, jsUndefined(), 0));
        set(state.dateTimeType, "resolution"_s, newTimeDelta(globalObject, 0, 0, 1, false));

        set(state.timeZoneType, "utc"_s, state.utc.get());
        // To the minute, as they have always been, though more will do for the class: -23:59 and +23:59
        set(state.timeZoneType, "min"_s, createTimeZone(globalObject, asObject(newTimeDelta(globalObject, -1, 60, 0, true)), JSValue()));
        set(state.timeZoneType, "max"_s, createTimeZone(globalObject, asObject(newTimeDelta(globalObject, 0, (23 * 60 + 59) * 60, 0, false)), JSValue()));

        state.epoch.set(vm, realm, asObject(newDateTime(globalObject, 1970, 1, 1, 0, 0, 0, 0, state.utc.get(), 0)));
        scope.assertNoException();
    }

    JSObject* module = newBuiltinModule(globalObject, "_datetime"_s);
    auto add = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    add("date"_s, state.dateType->object());
    add("datetime"_s, state.dateTimeType->object());
    add("time"_s, state.timeType->object());
    add("timedelta"_s, state.deltaType->object());
    add("tzinfo"_s, state.tzinfoType->object());
    add("timezone"_s, state.timeZoneType->object());
    add("MINYEAR"_s, jsNumber(minYear));
    add("MAXYEAR"_s, jsNumber(maxYear));
    add("UTC"_s, state.utc.get());
    add("datetime_CAPI"_s, newCapsule(globalObject, "datetime.datetime_CAPI"_s));
    return module;
}

} } // namespace JSC::Python
