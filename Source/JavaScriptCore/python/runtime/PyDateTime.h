/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyAbstract.h"
#include "PyInternals.h"
#include "PyNumbers.h"
#include "PyObject.h"
#include "PyStr.h"
#include "PyType.h"
#include <wtf/Int128.h>

namespace JSC {
namespace Python {

class PyModule;

// The module _datetime, which Lib/datetime.py prefers to Lib/_pydatetime.py. (Modules/_datetimemodule.c in CPython is the
// reference for what it does.) It is in PyDateTime.cpp (what the types share), PyDateTimeDelta.cpp (timedelta),
// PyDateTimeDate.cpp (date, IsoCalendarDate), PyDateTimeTime.cpp (tzinfo, timezone, time) and PyDateTimeDateTime.cpp (datetime).
namespace DateTime {

static constexpr int minYear = 1;
static constexpr int maxYear = 9999;
static constexpr int maxOrdinal = 3652059; // date(9999, 12, 31).toordinal()
static constexpr int maxDeltaDays = 999999999;
static constexpr int secondsPerDay = 24 * 3600;
static constexpr int microsecondsPerSecond = 1000000;
static constexpr int64_t microsecondsPerDay = static_cast<int64_t>(secondsPerDay) * microsecondsPerSecond;
static constexpr int64_t epochOrdinal = 719163; // date(1970, 1, 1).toordinal()

// ---- The objects ----
//
// None of them changes once made, but for the hash of those for which it takes a call to find out.

class PyTimeDelta final : public PyObject {
public:
    PY_DECLARE_CELL(PyTimeDelta, PyObject, PyObjectType);

    // 0 <= seconds < 24 * 3600, 0 <= microseconds < 1000000.
    static PyTimeDelta* create(VM&, Structure*, PyType*, int days, int seconds, int microseconds);

    int days() const { return m_days; }
    int seconds() const { return m_seconds; }
    int microseconds() const { return m_microseconds; }
    bool isZero() const { return !(m_days | m_seconds | m_microseconds); }
    // 67 bits at most.
    Int128 total() const { return (static_cast<Int128>(m_days) * secondsPerDay + m_seconds) * microsecondsPerSecond + m_microseconds; }
    // Negative, zero or positive.
    int compare(const PyTimeDelta* other) const
    {
        if (m_days != other->m_days)
            return m_days < other->m_days ? -1 : 1;
        if (m_seconds != other->m_seconds)
            return m_seconds < other->m_seconds ? -1 : 1;
        return m_microseconds - other->m_microseconds;
    }

private:
    PyTimeDelta(VM&, Structure*, PyType*, int days, int seconds, int microseconds);

    int32_t m_days;
    int32_t m_seconds;
    int32_t m_microseconds;
};

// What a date, a time or a datetime says.
struct Fields {
    int year { 0 };
    int month { 0 };
    int day { 0 };
    int hour { 0 };
    int minute { 0 };
    int second { 0 };
    int microsecond { 0 };
    int fold { 0 };
};

// The same in one word, the year first and the fold last, so that words compare as moments do.
//
// A pickle is taken at its word but for the month (or the hour of a time), as in CPython, so a forged one makes an object with a
// day 53. Here each field keeps the bits it has room for, which is all of them for anything a constructor lets through.
namespace Packed {
static constexpr unsigned foldShift = 0;
static constexpr unsigned microsecondShift = 1;
static constexpr unsigned secondShift = 21;
static constexpr unsigned minuteShift = 27;
static constexpr unsigned hourShift = 33;
static constexpr unsigned dayShift = 38;
static constexpr unsigned monthShift = 43;
static constexpr unsigned yearShift = 47;

ALWAYS_INLINE uint64_t date(int year, int month, int day)
{
    return (static_cast<uint64_t>(year & 0xffff) << yearShift) | (static_cast<uint64_t>(month & 15) << monthShift) | (static_cast<uint64_t>(day & 31) << dayShift);
}
ALWAYS_INLINE uint64_t time(int hour, int minute, int second, int microsecond, int fold)
{
    return (static_cast<uint64_t>(hour & 31) << hourShift) | (static_cast<uint64_t>(minute & 63) << minuteShift) | (static_cast<uint64_t>(second & 63) << secondShift)
        | (static_cast<uint64_t>(microsecond & 0xfffff) << microsecondShift) | static_cast<uint64_t>(fold & 1);
}
ALWAYS_INLINE uint64_t all(const Fields& fields)
{
    return date(fields.year, fields.month, fields.day) | time(fields.hour, fields.minute, fields.second, fields.microsecond, fields.fold);
}
ALWAYS_INLINE int year(uint64_t packed) { return static_cast<int>(packed >> yearShift); }
ALWAYS_INLINE int month(uint64_t packed) { return static_cast<int>(packed >> monthShift) & 15; }
ALWAYS_INLINE int day(uint64_t packed) { return static_cast<int>(packed >> dayShift) & 31; }
ALWAYS_INLINE int hour(uint64_t packed) { return static_cast<int>(packed >> hourShift) & 31; }
ALWAYS_INLINE int minute(uint64_t packed) { return static_cast<int>(packed >> minuteShift) & 63; }
ALWAYS_INLINE int second(uint64_t packed) { return static_cast<int>(packed >> secondShift) & 63; }
ALWAYS_INLINE int microsecond(uint64_t packed) { return static_cast<int>(packed >> microsecondShift) & 0xfffff; }
ALWAYS_INLINE int fold(uint64_t packed) { return static_cast<int>(packed & 1); }
static constexpr uint64_t timeMask = (1ull << dayShift) - 1;
ALWAYS_INLINE Fields unpack(uint64_t packed)
{
    return { year(packed), month(packed), day(packed), hour(packed), minute(packed), second(packed), microsecond(packed), fold(packed) };
}
// Negative, zero or positive. The fold does not count.
ALWAYS_INLINE int compare(uint64_t a, uint64_t b)
{
    a >>= 1;
    b >>= 1;
    return a == b ? 0 : a < b ? -1 : 1;
}
} // namespace Packed

class PyDate : public PyObject {
public:
    PY_DECLARE_CELL(PyDate, PyObject, PyObjectType);

    static PyDate* create(VM&, Structure*, PyType*, uint64_t packed);

    uint64_t packed() const { return m_packed; }
    int year() const { return Packed::year(m_packed); }
    int month() const { return Packed::month(m_packed); }
    int day() const { return Packed::day(m_packed); }

protected:
    PyDate(VM&, Structure*, PyType*, uint64_t packed);

    uint64_t m_packed;
};

class PyDateTime final : public PyDate {
public:
    PY_DECLARE_CELL(PyDateTime, PyDate, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyDateTime* create(VM&, Structure*, PyType*, uint64_t packed, JSValue tzinfo);

    int hour() const { return Packed::hour(m_packed); }
    int minute() const { return Packed::minute(m_packed); }
    int second() const { return Packed::second(m_packed); }
    int microsecond() const { return Packed::microsecond(m_packed); }
    int fold() const { return Packed::fold(m_packed); }
    Fields fields() const { return Packed::unpack(m_packed); }
    JSValue tzinfo() const { return m_tzinfo.get(); } // None, or an instance of tzinfo.
    bool isNaive() const { return isNone(m_tzinfo.get()); }

    int64_t cachedHash { -1 }; // -1: not known.

private:
    PyDateTime(VM&, Structure*, PyType*, uint64_t packed);

    WriteBarrier<Unknown> m_tzinfo;
};

// datetime.time
class PyTimeOfDay final : public PyObject {
public:
    PY_DECLARE_CELL(PyTimeOfDay, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyTimeOfDay* create(VM&, Structure*, PyType*, uint64_t packed, JSValue tzinfo);

    uint64_t packed() const { return m_packed; }
    int hour() const { return Packed::hour(m_packed); }
    int minute() const { return Packed::minute(m_packed); }
    int second() const { return Packed::second(m_packed); }
    int microsecond() const { return Packed::microsecond(m_packed); }
    int fold() const { return Packed::fold(m_packed); }
    JSValue tzinfo() const { return m_tzinfo.get(); }

    int64_t cachedHash { -1 };

private:
    PyTimeOfDay(VM&, Structure*, PyType*, uint64_t packed);

    uint64_t m_packed;
    WriteBarrier<Unknown> m_tzinfo;
};

class PyTimeZone final : public PyObject {
public:
    PY_DECLARE_CELL(PyTimeZone, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyTimeZone* create(VM&, Structure*, PyType*, PyTimeDelta* offset, JSValue name);

    PyTimeDelta* offset() const { return m_offset.get(); }
    JSValue name() const { return m_name.get(); } // Empty: it was given none.

private:
    PyTimeZone(VM&, Structure*, PyType*);

    WriteBarrier<PyTimeDelta> m_offset;
    WriteBarrier<Unknown> m_name;
};

// ---- What there is one of for each runtime ----

#define FOR_EACH_DATETIME_NAME(v) \
    v(utcoffset, "utcoffset") \
    v(dst, "dst") \
    v(tzname, "tzname") \
    v(fromutc, "fromutc") \
    v(isoformat, "isoformat") \
    v(strftime, "strftime") \
    v(timetuple, "timetuple") \
    v(fromtimestamp, "fromtimestamp") \
    v(replace, "replace") \
    v(asIntegerRatio, "as_integer_ratio") \
    v(time, "time") \
    v(structTime, "struct_time") \
    v(fold, "fold") \
    v(getInitArgs, "__getinitargs__") \
    v(strptimeDate, "_strptime_datetime_date") \
    v(strptimeTime, "_strptime_datetime_time") \
    v(strptimeDateTime, "_strptime_datetime_datetime")

// It is the hidden field 0 of each of the types.
class State final : public PyObject {
public:
    PY_DECLARE_CELL(State, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static State* create(VM&, Structure*, PyType*);

    WriteBarrier<PyType> deltaType;
    WriteBarrier<PyType> dateType;
    WriteBarrier<PyType> dateTimeType;
    WriteBarrier<PyType> timeType;
    WriteBarrier<PyType> tzinfoType;
    WriteBarrier<PyType> timeZoneType;
    WriteBarrier<PyType> isoCalendarDateType;

    WriteBarrier<Structure> deltaStructure;
    WriteBarrier<Structure> dateStructure;
    WriteBarrier<Structure> dateTimeStructure;
    WriteBarrier<Structure> timeStructure;
    WriteBarrier<Structure> timeZoneStructure;

    WriteBarrier<PyTimeDelta> zeroDelta; // Every timedelta(0) is this one.
    WriteBarrier<PyTimeZone> utc;
    WriteBarrier<PyDateTime> epoch; // 1970-01-01T00:00:00+00:00

#define DECLARE_NAME(id, text) WriteBarrier<JSString> id##Name;
    FOR_EACH_DATETIME_NAME(DECLARE_NAME)
#undef DECLARE_NAME

private:
    State(VM&, Structure*, PyType*);
};

State& stateOf(JSGlobalObject*);
// For one of the types of the module, or a class derived from one.
ALWAYS_INLINE State& stateOf(JSGlobalObject* globalObject, PyType* type)
{
    while (type->isHeapType())
        type = type->base();
    if (State* state = State::tryFrom(hiddenField(globalObject, type, 0))) [[likely]]
        return *state;
    return stateOf(globalObject);
}
ALWAYS_INLINE State& stateOf(JSGlobalObject* globalObject, PyObject* object) { return stateOf(globalObject, object->type()); }

extern const TypeDef deltaTypeDef;
extern const TypeDef dateTypeDef;
extern const TypeDef isoCalendarDateTypeDef;
extern const TypeDef dateTimeTypeDef;
extern const TypeDef timeTypeDef;
extern const TypeDef tzinfoTypeDef;
extern const TypeDef timeZoneTypeDef;

// ---- The calendar ----

ALWAYS_INLINE bool isLeapYear(int year) { return !(year % 4) && (year % 100 || !(year % 400)); }

ALWAYS_INLINE int daysInMonth(int year, int month)
{
    static constexpr uint8_t days[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0, 0, 0 };
    return month == 2 && isLeapYear(year) ? 29 : days[month & 15];
}

// In that year, before the first of that month.
ALWAYS_INLINE int daysBeforeMonth(int year, int month)
{
    static constexpr uint16_t days[] = { 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 0, 0, 0 };
    return days[month & 15] + (month > 2 && isLeapYear(year));
}

ALWAYS_INLINE int daysBeforeYear(int year)
{
    int before = year - 1;
    return before * 365 + before / 4 - before / 100 + before / 400;
}

// 0001-01-01 is day 1.
ALWAYS_INLINE int toOrdinal(int year, int month, int day) { return daysBeforeYear(year) + daysBeforeMonth(year, month) + day; }
ALWAYS_INLINE int toOrdinal(uint64_t packed) { return toOrdinal(Packed::year(packed), Packed::month(packed), Packed::day(packed)); }

// For an ordinal that is 1 at least.
inline void fromOrdinal(int ordinal, int& year, int& month, int& day)
{
    // Years that begin in March have the leap day last, and their months have 153 days for every five.
    unsigned days = static_cast<unsigned>(ordinal) + 305; // Since 0000-03-01.
    unsigned era = days / 146097;
    unsigned dayOfEra = days % 146097;
    unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    unsigned monthFromMarch = (5 * dayOfYear + 2) / 153;
    day = static_cast<int>(dayOfYear - (153 * monthFromMarch + 2) / 5 + 1);
    month = static_cast<int>(monthFromMarch < 10 ? monthFromMarch + 3 : monthFromMarch - 9);
    year = static_cast<int>(yearOfEra + era * 400 + (month <= 2));
}

// Monday is 0.
ALWAYS_INLINE int weekdayOfOrdinal(int ordinal) { return (ordinal + 6) % 7; }

// The ordinal of the Monday that week 1 of the ISO year starts with: the week that has the year's first Thursday.
inline int isoWeek1Monday(int year)
{
    int firstDay = toOrdinal(year, 1, 1);
    int firstWeekday = weekdayOfOrdinal(firstDay);
    return firstDay - firstWeekday + (firstWeekday > 3 ? 7 : 0);
}

enum class IsoCalendarError : uint8_t { None, Year, Week, Day };
// The date may be in year 10000.
IsoCalendarError fromIsoCalendar(int isoYear, int isoWeek, int isoDay, int& year, int& month, int& day);

template<typename Integer>
ALWAYS_INLINE Integer floorDivide(Integer dividend, Integer divisor, Integer& remainder)
{
    Integer quotient = dividend / divisor;
    remainder = dividend % divisor;
    if (remainder && ((remainder < 0) != (divisor < 0))) {
        remainder += divisor;
        --quotient;
    }
    return quotient;
}

// ---- Arguments ----

bool intArgumentSlow(JSGlobalObject*, JSValue, int& result);
// For a parameter that is a C int in CPython. False: an exception.
ALWAYS_INLINE bool intArgument(JSGlobalObject* globalObject, JSValue value, int& result)
{
    if (value.isInt32()) [[likely]] {
        result = value.asInt32();
        return true;
    }
    return intArgumentSlow(globalObject, value, result);
}

// "function() which must be expected, not int"
JSValue raiseBadArgument(JSGlobalObject*, ASCIILiteral function, ASCIILiteral which, const String& expected, JSValue given);
// Null: it is no str, and that has been said.
JSString* strArgument(JSGlobalObject*, ASCIILiteral function, ASCIILiteral which, JSValue);

// For the functions with more parameters than a table of natives can say, and the constructors, which look at what they are given
// before that is matched with their parameters. All parameters can be given by name; those after `maxPositional` only so.
struct Parameters {
    ASCIILiteral function;
    std::span<const ASCIILiteral> names;
    unsigned required;
    unsigned maxPositional;
};
// `values` has room for one of each, and they are empty. False: a TypeError, worded as for any native.
bool bindArguments(JSGlobalObject*, const Parameters&, const CallArgs&, JSValue* values);

// ---- Making the objects ----

// False: a ValueError.
bool checkDateFields(JSGlobalObject*, int year, int month, int day);
bool checkTimeFields(JSGlobalObject*, int hour, int minute, int second, int microsecond, int fold);
// None, or an instance of tzinfo. False: a TypeError.
bool checkTzInfo(JSGlobalObject*, State&, JSValue);
ALWAYS_INLINE bool isTzInfo(JSGlobalObject* globalObject, State& state, JSValue value)
{
    return PyTimeZone::is(value) || isInstanceOf(globalObject, value, state.tzinfoType.get());
}

// These make an instance of `type` without asking it. Whatever the numbers are, they are brought into range or complained of.
JSValue newDelta(JSGlobalObject*, State&, PyType*, int64_t days, int64_t seconds, int64_t microseconds);
inline JSValue newDelta(JSGlobalObject* globalObject, State& state, int64_t days, int64_t seconds, int64_t microseconds) { return newDelta(globalObject, state, state.deltaType.get(), days, seconds, microseconds); }
JSValue newDeltaFromTotal(JSGlobalObject*, State&, PyType*, Int128 microseconds);
JSValue newDate(JSGlobalObject*, State&, PyType*, int year, int month, int day);
JSValue newTime(JSGlobalObject*, State&, PyType*, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold);
JSValue newDateTime(JSGlobalObject*, State&, PyType*, const Fields&, JSValue tzinfo);
// The name is a str, or empty.
JSValue newTimeZone(JSGlobalObject*, State&, PyTimeDelta* offset, JSValue name);

// These call the class, unless it is one of the module's own.
JSValue makeDate(JSGlobalObject*, State&, PyType* cls, int year, int month, int day);
JSValue makeTime(JSGlobalObject*, State&, PyType* cls, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold);
JSValue makeDateTime(JSGlobalObject*, State&, PyType* cls, const Fields&, JSValue tzinfo);

// ---- Time zones ----

// tzinfo.utcoffset(argument) and tzinfo.dst(argument), checked: None or a PyTimeDelta. None for a tzinfo that is None.
JSValue callUtcOffset(JSGlobalObject*, State&, JSValue tzinfo, JSValue argument);
JSValue callDst(JSGlobalObject*, State&, JSValue tzinfo, JSValue argument);
// None or a str.
JSValue callTzName(JSGlobalObject*, State&, JSValue tzinfo, JSValue argument);

JSString* timeZoneName(JSGlobalObject*, State&, PyTimeZone*);
// dateTime + factor * delta, of the type of the former.
JSValue addDelta(JSGlobalObject*, State&, PyDateTime*, PyTimeDelta*, int factor);
// left - right, as datetime.__sub__ has it.
JSValue subtractDateTimes(JSGlobalObject*, State&, PyDateTime* left, PyDateTime* right);

// ---- Text ----

// Short text in ASCII.
class Text {
public:
    void append(char character)
    {
        if (m_length < capacity)
            m_characters[m_length++] = static_cast<Latin1Character>(character);
    }
    void append(ASCIILiteral literal)
    {
        for (char character : literal.span())
            append(character);
    }
    // As %0*d does.
    void append(int64_t number, unsigned minWidth = 1, char padding = '0');
    std::span<const Latin1Character> span() const { return { m_characters, m_length }; }
    String toString() const { return String(span()); }
    bool isEmpty() const { return !m_length; }

private:
    static constexpr unsigned capacity = 96;
    Latin1Character m_characters[capacity];
    unsigned m_length { 0 };
};

void appendIsoDate(Text&, uint64_t packed);
enum class TimeSpec : uint8_t { Hours, Minutes, Seconds, Milliseconds, Microseconds };
// What isoformat() was given, if anything. False: an exception.
bool timeSpecArgument(JSGlobalObject*, JSValue, int microsecond, TimeSpec&);
void appendIsoTime(Text&, uint64_t packed, TimeSpec);
// "+05:30", with the separator that is given; nothing if the offset is None. False: an exception.
bool appendUtcOffset(JSGlobalObject*, State&, Text&, ASCIILiteral separator, JSValue tzinfo, JSValue argument);
void appendOffset(Text&, ASCIILiteral separator, PyTimeDelta*);

JSValue formatCTime(JSGlobalObject*, uint64_t packed);
// time.struct_time((...))
JSValue makeStructTime(JSGlobalObject*, State&, int year, int month, int day, int hour, int minute, int second, int dstFlag);
// time.strftime(format, timeTuple), after %z, %:z, %Z and %f have been seen to, and the years the C library does not pad.
JSValue wrapStrftime(JSGlobalObject*, State&, JSValue object, JSString* format, JSValue timeTuple, JSValue tzinfoArgument);
// object.__format__(format) of date and time.
JSValue formatWithStrftime(JSGlobalObject*, State&, JSValue self, JSValue format);
// _strptime.name(cls, string, format)
JSValue callStrptime(JSGlobalObject*, JSString* name, JSValue cls, JSValue string, JSValue format);

// ---- ISO 8601 ----

// The characters of a str, one for each code point. What is not ASCII matters only in that it is not.
class IsoText {
public:
    // Check for an exception afterwards.
    IsoText(JSGlobalObject*, JSString*);

    unsigned length() const { return m_length; }
    // Past the end there is a NUL, as for the C code whose every whim programs may have come to count on.
    uint8_t at(unsigned index) const { return index < m_length ? m_characters[index] : 0; }

private:
    String m_string;
    Vector<uint8_t, 48> m_storage;
    const uint8_t* m_characters { nullptr };
    unsigned m_length { 0 };
};

// The date is in the first `length` characters. False: there is none.
bool parseIsoDate(const IsoText&, unsigned length, int& year, int& month, int& day);
enum class IsoTimeResult : uint8_t { Malformed, Raised, Naive, Aware };
// From `start` to the end of the text. Raised: the fields of the offset are out of range, and a ValueError says so.
IsoTimeResult parseIsoTime(JSGlobalObject*, const IsoText&, unsigned start, Fields&, int& offsetSeconds, int& offsetMicroseconds);
JSValue tzinfoFromIsoOffset(JSGlobalObject*, State&, IsoTimeResult, int offsetSeconds, int offsetMicroseconds);
JSValue raiseInvalidIsoFormat(JSGlobalObject*, JSValue string);

// ---- Hashes ----
//
// They fit a small int, so that __hash__ allocates nothing.

ALWAYS_INLINE int64_t mixHash(uint64_t value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return static_cast<int64_t>(value >> 12);
}
ALWAYS_INLINE int64_t hashOfDelta(int64_t days, int seconds, int microseconds)
{
    return mixHash(static_cast<uint64_t>(days) * 0x9e3779b97f4a7c15ull + ((static_cast<uint64_t>(seconds) << 20) | static_cast<uint64_t>(microseconds)));
}
ALWAYS_INLINE int64_t hashOfDelta(const PyTimeDelta* delta) { return hashOfDelta(delta->days(), delta->seconds(), delta->microseconds()); }

// ---- Comparisons ----

ALWAYS_INLINE JSValue compareResult(int difference, CompareOperator op)
{
    switch (op) {
    case CompareOperator::Eq:
        return jsBoolean(!difference);
    case CompareOperator::NotEq:
        return jsBoolean(difference);
    case CompareOperator::Lt:
        return jsBoolean(difference < 0);
    case CompareOperator::LtE:
        return jsBoolean(difference <= 0);
    case CompareOperator::Gt:
        return jsBoolean(difference > 0);
    case CompareOperator::GtE:
        return jsBoolean(difference >= 0);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

#define PY_DATETIME_COMPARISONS(function) \
    PY_SLOT("__eq__", function<CompareOperator::Eq>, "($self, value, /)"), \
    PY_SLOT("__ne__", function<CompareOperator::NotEq>, "($self, value, /)"), \
    PY_SLOT("__lt__", function<CompareOperator::Lt>, "($self, value, /)"), \
    PY_SLOT("__le__", function<CompareOperator::LtE>, "($self, value, /)"), \
    PY_SLOT("__gt__", function<CompareOperator::Gt>, "($self, value, /)"), \
    PY_SLOT("__ge__", function<CompareOperator::GtE>, "($self, value, /)")

} // namespace DateTime
} // namespace Python
} // namespace JSC
