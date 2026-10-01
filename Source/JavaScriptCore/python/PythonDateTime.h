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

#include "PyStateObject.h"
#include "PyType.h"
#include "PythonOperators.h"
#include "WriteBarrier.h"
#include <array>
#include <wtf/text/CString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// _datetime: Modules/_datetimemodule.c of CPython, function for function.
//
//     PythonDateTime.cpp          the calendar, what reads ISO 8601, what is asked of a tzinfo, strftime(), and the module
//     PythonDateTimeDelta.cpp     timedelta
//     PythonDateTimeDate.cpp      date, and what isocalendar() gives
//     PythonDateTimeTime.cpp      time
//     PythonDateTimeDateTime.cpp  datetime
//     PythonDateTimeZone.cpp      tzinfo and timezone

namespace JSC { namespace Python {

class NativeArguments;

static constexpr int minYear = 1;
static constexpr int maxYear = 9999;
static constexpr int maxOrdinal = 3652059; // date(9999, 12, 31).toordinal()
static constexpr int maxDeltaDays = 999999999;

// PyDateTime_Delta
struct TimeDeltaState final : NativeState {
    PYTHON_NATIVE_STATE(TimeDeltaState);
    int days { 0 };
    int seconds { 0 }; // 0 <= seconds < 24 * 3600
    int microseconds { 0 }; // 0 <= microseconds < 1000000
    int64_t hash { -1 };

    explicit operator bool() const { return days || seconds || microseconds; } // delta_bool()
};
template<typename Visitor> void TimeDeltaState::visit(Visitor&) { }

// PyDateTime_Date and PyDateTime_DateTime. A datetime is a date, so what is a method of date finds in either what it looks for.
struct DateState final : NativeState {
    PYTHON_NATIVE_STATE(DateState);
    // As CPython has them, which is what is compared, hashed and pickled: two bytes of year, the month and the day, and for a datetime the hour, the minute, the second and three bytes of microseconds.
    static constexpr size_t dateSize = 4;
    static constexpr size_t dateTimeSize = 10;
    std::array<uint8_t, dateTimeSize> data { };
    bool isDateTime { false };
    uint8_t fold { 0 };
    int64_t hash { -1 };
    WriteBarrier<Unknown> tzinfo; // Empty if it has none: HASTZINFO()

    int year() const { return data[0] << 8 | data[1]; }
    int month() const { return data[2]; }
    int day() const { return data[3]; }
    int hour() const { return data[4]; }
    int minute() const { return data[5]; }
    int second() const { return data[6]; }
    int microsecond() const { return data[7] << 16 | data[8] << 8 | data[9]; }
    JSValue tzinfoOrNone() const { return tzinfo ? tzinfo.get() : jsUndefined(); } // GET_DT_TZINFO()
    std::span<const uint8_t> bytes() const { return std::span(data).first(isDateTime ? dateTimeSize : dateSize); }
    void setDate(int year, int month, int day)
    {
        data[0] = year >> 8;
        data[1] = year & 0xFF;
        data[2] = month;
        data[3] = day;
    }
    void setTime(int hour, int minute, int second, int microsecond)
    {
        data[4] = hour;
        data[5] = minute;
        data[6] = second;
        data[7] = microsecond >> 16;
        data[8] = microsecond >> 8 & 0xFF;
        data[9] = microsecond & 0xFF;
    }
};
template<typename Visitor> void DateState::visit(Visitor& visitor) { visitor.append(tzinfo); }

// PyDateTime_Time
struct TimeOfDayState final : NativeState {
    PYTHON_NATIVE_STATE(TimeOfDayState);
    static constexpr size_t size = 6;
    std::array<uint8_t, size> data { };
    uint8_t fold { 0 };
    int64_t hash { -1 };
    WriteBarrier<Unknown> tzinfo; // Empty if it has none

    int hour() const { return data[0]; }
    int minute() const { return data[1]; }
    int second() const { return data[2]; }
    int microsecond() const { return data[3] << 16 | data[4] << 8 | data[5]; }
    JSValue tzinfoOrNone() const { return tzinfo ? tzinfo.get() : jsUndefined(); } // GET_TIME_TZINFO()
};
template<typename Visitor> void TimeOfDayState::visit(Visitor& visitor) { visitor.append(tzinfo); }

// PyDateTime_TZInfo, which has nothing in it. An instance of a class that a program derives from tzinfo has one.
struct TZInfoState final : NativeState {
    PYTHON_NATIVE_STATE(TZInfoState);
};
template<typename Visitor> void TZInfoState::visit(Visitor&) { }

// PyDateTime_TimeZone
struct TimeZoneState final : NativeState {
    PYTHON_NATIVE_STATE(TimeZoneState);
    WriteBarrier<PyStateObject> offset; // A timedelta
    WriteBarrier<Unknown> name; // A str, or empty
};
template<typename Visitor>
void TimeZoneState::visit(Visitor& visitor)
{
    visitor.append(offset);
    visitor.append(name);
}

// datetime_state, and what in CPython is static
struct DateTimeModuleState final : NativeState {
    PYTHON_NATIVE_STATE(DateTimeModuleState);
    WriteBarrier<PyType> deltaType;
    WriteBarrier<PyType> dateType;
    WriteBarrier<PyType> dateTimeType;
    WriteBarrier<PyType> timeType;
    WriteBarrier<PyType> tzinfoType;
    WriteBarrier<PyType> timeZoneType;
    WriteBarrier<PyType> isoCalendarDateType;
    WriteBarrier<PyStateObject> zeroDelta;
    WriteBarrier<PyStateObject> utc;
    WriteBarrier<PyStateObject> epoch; // datetime(1970, 1, 1, tzinfo=timezone.utc)
};
template<typename Visitor>
void DateTimeModuleState::visit(Visitor& visitor)
{
    visitor.append(deltaType);
    visitor.append(dateType);
    visitor.append(dateTimeType);
    visitor.append(timeType);
    visitor.append(tzinfoType);
    visitor.append(timeZoneType);
    visitor.append(isoCalendarDateType);
    visitor.append(zeroDelta);
    visitor.append(utc);
    visitor.append(epoch);
}
DateTimeModuleState& dateTimeModuleState(JSGlobalObject*);

// PyDelta_Check() and the like. An instance of a class, or of one derived from it, has what the __new__() of the class gave it, so that is what tells. Null if it is no such thing.
inline TimeDeltaState* tryTimeDelta(JSValue value) { return tryStateOf<TimeDeltaState>(value); }
inline DateState* tryDate(JSValue value) { return tryStateOf<DateState>(value); } // A datetime is one.
inline DateState* tryDateTime(JSValue value)
{
    DateState* state = tryDate(value);
    return state && state->isDateTime ? state : nullptr;
}
inline TimeOfDayState* tryTimeOfDay(JSValue value) { return tryStateOf<TimeOfDayState>(value); }
bool isTZInfo(JSGlobalObject*, JSValue); // PyTZInfo_Check()

// ---- The calendar. None of this raises.

int floorDivide(int x, int y, int& remainder); // divmod(), where y > 0
bool isLeapYear(int year);
int daysInMonth(int year, int month);
int daysBeforeMonth(int year, int month);
int daysBeforeYear(int year);
void ordinalToDate(int ordinal, int& year, int& month, int& day); // ord_to_ymd()
int dateToOrdinal(int year, int month, int day); // ymd_to_ord()
int weekdayOf(int year, int month, int day); // 0 is Monday.
int isoWeek1Monday(int year);
// iso_to_ymd(): 0, or -4, -2 or -3 if it is the year, the week or the day that there is no such thing as.
int isoToDate(int isoYear, int isoWeek, int isoDay, int& year, int& month, int& day);
void normalizeDelta(int& days, int& seconds, int& microseconds); // normalize_d_s_us()

// ---- What reads ISO 8601. What is read ends with a null, as what a CString has does. They give what CPython's give: less than 0 if it will not do.

int parseISODate(const char*, size_t length, int& year, int& month, int& day); // parse_isoformat_date()
// parse_isoformat_time(): 1 if it says how far it is from UTC.
int parseISOTime(const char*, size_t length, int& hour, int& minute, int& second, int& microsecond, int& offset, int& offsetMicrosecond);
// PyUnicode_AsUTF8AndSize(). Nothing if there is half of a surrogate pair in it.
std::optional<CString> utf8ForParsing(const String&);
JSValue raiseInvalidISOFormat(JSGlobalObject*, ThrowScope&, JSValue given);
JSValue tzinfoFromISOFormat(JSGlobalObject*, int result, int offset, int offsetMicrosecond); // tzinfo_from_isoformat_results()

// ---- These may raise, and what they return is empty if they did.

void normalizeDate(JSGlobalObject*, int& year, int& month, int& day); // normalize_y_m_d()
void normalizeDateTime(JSGlobalObject*, int& year, int& month, int& day, int& hour, int& minute, int& second, int& microsecond);
void checkDateArguments(JSGlobalObject*, int year, int month, int day);
void checkTimeArguments(JSGlobalObject*, int hour, int minute, int second, int microsecond, int fold);
void checkTZInfo(JSGlobalObject*, JSValue); // check_tzinfo_subclass(): None, or an instance of tzinfo

// Each makes one of a class, or of the built-in one if it is given none. new_delta_ex(), new_date_ex(), new_datetime_ex2() and new_time_ex2()
JSValue newTimeDelta(JSGlobalObject*, int days, int seconds, int microseconds, bool normalize, PyType* = nullptr);
JSValue newDate(JSGlobalObject*, int year, int month, int day, PyType* = nullptr);
JSValue newDateTime(JSGlobalObject*, int year, int month, int day, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold, PyType* = nullptr);
JSValue newTimeOfDay(JSGlobalObject*, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold, PyType* = nullptr);
// The same by calling the class, if it is not the built-in one. new_date_subclass_ex(), new_datetime_subclass_fold_ex() and new_time_subclass_fold_ex()
JSValue newDateOfClass(JSGlobalObject*, int year, int month, int day, JSValue cls);
JSValue newDateTimeOfClass(JSGlobalObject*, int year, int month, int day, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold, JSValue cls);
JSValue newTimeOfDayOfClass(JSGlobalObject*, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold, JSValue cls);
// call_subclass_fold(): cls(*arguments), and fold=fold if that is not 0. It adds to the arguments.
JSValue callClassWithFold(JSGlobalObject*, JSValue cls, MarkedArgumentBuffer& arguments, int fold);
JSValue createTimeZone(JSGlobalObject*, PyStateObject* offset, JSValue name); // create_timezone(). The name may be empty.
JSValue newTimeZone(JSGlobalObject*, PyStateObject* offset, JSValue name); // new_timezone()

// call_utcoffset() and call_dst(): None or a timedelta. call_tzname(): None or a str.
JSValue callUTCOffset(JSGlobalObject*, JSValue tzinfo, JSValue argument);
JSValue callDST(JSGlobalObject*, JSValue tzinfo, JSValue argument);
JSValue callTZName(JSGlobalObject*, JSValue tzinfo, JSValue argument);
// format_utcoffset(): "+05:30", or nothing at all if there is none. Null if it raised.
String formatUTCOffset(JSGlobalObject*, ASCIILiteral separator, JSValue tzinfo, JSValue argument);
String withTZInfoKeyword(JSGlobalObject*, const String& repr, JSValue tzinfo); // append_keyword_tzinfo()
String withFoldKeyword(const String& repr, int fold); // append_keyword_fold()
String formatCTime(const DateState&, int hours, int minutes, int seconds); // format_ctime()
JSValue wrapStrftime(JSGlobalObject*, JSValue object, JSString* format, JSValue timeTuple, JSValue tzinfoArgument);
JSValue buildStructTime(JSGlobalObject*, int year, int month, int day, int hour, int minute, int second, int dstFlag);
JSValue callStrptime(JSGlobalObject*, ASCIILiteral function, JSValue cls, JSValue string, JSValue format); // _strptime.function(cls, string, format)
void checkIsStrArgument(JSGlobalObject*, ASCIILiteral function, unsigned position, JSValue); // The "U" of PyArg_ParseTuple()
// The part of PyArg_ParseTupleAndKeywords() that is for a run of "i", of which the first so many are required. It makes what it can of each as it comes to it, so that what is wrong with the first is said before that there is
// no second. `first` is where they begin in the signature. What was not given is left as it was. It is for a function whose arguments have not been checked, and may raise.
void parseIntArguments(JSGlobalObject*, const NativeArguments&, unsigned first, std::span<int> fields, unsigned required);
JSValue comparisonResult(int difference, ComparisonOperator); // diff_to_bool()
int compareBytes(std::span<const uint8_t>, std::span<const uint8_t>); // memcmp()
// The state that pickle calls a class with, as bytes: it is bytes, or a str out of Python 2 that is as long. Nothing if it is neither, or is not of that length. `what` is for what is said of a str that is not Latin-1, which
// is raised.
std::optional<Vector<uint8_t, DateState::dateTimeSize>> pickledState(JSGlobalObject*, JSValue, size_t length, unsigned indexToLookAt, bool (*isSane)(unsigned), ASCIILiteral what);

// ---- timedelta, for the rest

int compareTimeDeltas(const TimeDeltaState&, const TimeDeltaState&); // delta_cmp()
JSValue negateTimeDelta(JSGlobalObject*, const TimeDeltaState&); // delta_negative()
JSValue subtractTimeDeltas(JSGlobalObject*, const TimeDeltaState&, const TimeDeltaState&); // delta_subtract(), of two that are
JSValue floorDivideTimeDeltas(JSGlobalObject*, const TimeDeltaState&, const TimeDeltaState&); // divide_timedelta_timedelta()
JSValue totalSecondsOfTimeDelta(JSGlobalObject*, const TimeDeltaState&); // delta_total_seconds()
int64_t hashOfTimeDelta(JSGlobalObject*, TimeDeltaState&); // delta_hash()

// ---- datetime, for the rest

JSValue addTimeDeltaToDateTime(JSGlobalObject*, JSValue dateTime, const TimeDeltaState&, int factor); // add_datetime_timedelta()

// ---- isoformat(), of a time and of a datetime

// How much of the time is written
enum class TimeSpecification : uint8_t { Hours, Minutes, Seconds, Milliseconds, Microseconds };
// What `timespec` says, which may not have been given. `position` is where it is among the arguments. Nothing if it raised.
std::optional<TimeSpecification> timeSpecificationFrom(JSGlobalObject*, JSValue given, unsigned position, int microsecond);
void appendTimeAsISO(StringBuilder&, TimeSpecification, int hour, int minute, int second, int microsecond);

JSC_DECLARE_HOST_FUNCTION(dateFormat); // date_format(), which is time's as well

// A slot of PyNumberMethods is given what is on either side of the operator in the order that it is written. __radd__() is __add__() with them the other way about, and says so in what it has packed.
std::pair<JSValue, JSValue> operandsOfSlot(CallFrame*);

void initializeTimeDeltaType(JSGlobalObject*);
void initializeDateTypes(JSGlobalObject*);
void initializeTimeOfDayType(JSGlobalObject*);
void initializeDateTimeType(JSGlobalObject*);
void initializeTimeZoneTypes(JSGlobalObject*);

} } // namespace JSC::Python
