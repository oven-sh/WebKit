/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyDateTime.h"

#include "JSCInlines.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyTuple.h"
#include <cerrno>
#include <cmath>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace DateTime {

// datetime.date and the result of its isocalendar().

// ---- What datetime needs as well ----

PickledState pickledStateArgument(JSGlobalObject* globalObject, JSValue value, ASCIILiteral what, unsigned length, unsigned checkedIndex, unsigned mask, unsigned least, unsigned limit, uint8_t* bytes)
{
    auto isSane = [&](unsigned checked) {
        checked &= mask;
        return checked >= least && checked < limit;
    };
    if (auto* given = PyBytes::tryFrom(value)) {
        if (given->length() != length || !isSane(given->data()[checkedIndex]))
            return PickledState::No;
        memcpy(bytes, given->data(), length);
        return PickledState::Yes;
    }
    if (!isStr(value))
        return PickledState::No;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    StrView text(globalObject, strValue(value));
    RETURN_IF_EXCEPTION(scope, PickledState::Raised);
    if (text.length() != length || !isSane(text.at(checkedIndex)))
        return PickledState::No;
    for (unsigned i = 0; i < length; ++i) {
        char32_t character = text.at(i);
        if (character > 0xff) {
            raiseValueError(globalObject, scope, "Failed to encode latin1 string when unpickling a "_s, what, " object. pickle.load(data, encoding='latin1') is assumed."_s);
            return PickledState::Raised;
        }
        bytes[i] = static_cast<uint8_t>(character);
    }
    return PickledState::Yes;
}

bool secondsArgument(JSGlobalObject* globalObject, JSValue value, time_t& result)
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

bool toLocalTime(JSGlobalObject* globalObject, time_t when, struct tm& tm)
{
    errno = 0;
    if (localtime_r(&when, &tm)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseOSError(globalObject, scope, errno ? errno : EINVAL);
    return false;
}

bool toUniversalTime(JSGlobalObject* globalObject, time_t when, struct tm& tm)
{
    errno = 0;
    if (gmtime_r(&when, &tm)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseOSError(globalObject, scope, errno ? errno : EINVAL);
    return false;
}

// ---- Constructors ----

static constexpr ASCIILiteral dateParameterNames[] = { "year"_s, "month"_s, "day"_s };
static constexpr Parameters dateParameters { "date"_s, dateParameterNames, 3, 3 };

static JSValue dateNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    State& state = stateOf(globalObject, type);
    if (args.size() == 1) [[unlikely]] {
        uint8_t bytes[4];
        PickledState pickled = pickledStateArgument(globalObject, args[0], "date"_s, 4, 2, 0xffffffff, 1, 13, bytes);
        RETURN_IF_EXCEPTION(scope, { });
        if (pickled == PickledState::Yes)
            return PyDate::create(vm, state.dateStructure.get(), type, Packed::date(bytes[0] << 8 | bytes[1], bytes[2], bytes[3]));
    }
    JSValue values[3];
    int year;
    int month;
    int day;
    if (failed(scope, bindArguments(globalObject, dateParameters, args, values)) || failed(scope, intArgument(globalObject, values[0], year))
        || failed(scope, intArgument(globalObject, values[1], month)) || failed(scope, intArgument(globalObject, values[2], day)))
        return { };
    RELEASE_AND_RETURN(scope, newDate(globalObject, state, type, year, month, day));
}

static JSValue dateFromTimestamp(JSGlobalObject* globalObject, JSValue cls, JSValue timestamp)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    time_t when;
    struct tm tm;
    if (failed(scope, secondsArgument(globalObject, timestamp, when)) || failed(scope, toLocalTime(globalObject, when, tm)))
        return { };
    RELEASE_AND_RETURN(scope, makeDate(globalObject, stateOf(globalObject, type), type, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday));
}

static JSValue dateToday(JSGlobalObject* globalObject, JSValue cls)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    State& state = stateOf(globalObject, type);
    if (type == state.dateType.get()) {
        struct tm tm;
        if (failed(scope, toLocalTime(globalObject, ::time(nullptr), tm)))
            return { };
        RELEASE_AND_RETURN(scope, newDate(globalObject, state, type, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday));
    }
    // A class may have its own fromtimestamp().
    JSValue module = importModule(globalObject, "time"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue now = callMethod(globalObject, module, state.timeName.get());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callMethod(globalObject, cls, state.fromtimestampName.get(), now));
}

static JSValue dateFromOrdinal(JSGlobalObject* globalObject, JSValue cls, JSValue ordinalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    int ordinal;
    if (failed(scope, intArgument(globalObject, ordinalValue, ordinal)))
        return { };
    if (ordinal < 1)
        return raiseValueError(globalObject, scope, "ordinal must be >= 1"_s);
    int year;
    int month;
    int day;
    fromOrdinal(ordinal, year, month, day);
    RELEASE_AND_RETURN(scope, makeDate(globalObject, stateOf(globalObject, type), type, year, month, day));
}

static JSValue dateFromIsoFormat(JSGlobalObject* globalObject, JSValue cls, JSValue string)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    JSString* given = strArgument(globalObject, "fromisoformat"_s, "argument"_s, string);
    RETURN_IF_EXCEPTION(scope, { });
    IsoText text(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned length = text.length();
    int year = 0;
    int month = 0;
    int day = 0;
    if ((length != 7 && length != 8 && length != 10) || !parseIsoDate(text, length, year, month, day))
        RELEASE_AND_RETURN(scope, raiseInvalidIsoFormat(globalObject, string));
    RELEASE_AND_RETURN(scope, makeDate(globalObject, stateOf(globalObject, type), type, year, month, day));
}

static JSValue dateFromIsoCalendar(JSGlobalObject* globalObject, JSValue cls, JSValue yearValue, JSValue weekValue, JSValue dayValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    int isoYear;
    int isoWeek;
    int isoDay;
    if (failed(scope, intArgument(globalObject, yearValue, isoYear)) || failed(scope, intArgument(globalObject, weekValue, isoWeek)) || failed(scope, intArgument(globalObject, dayValue, isoDay)))
        return { };
    int year;
    int month;
    int day;
    switch (fromIsoCalendar(isoYear, isoWeek, isoDay, year, month, day)) {
    case IsoCalendarError::Year:
        return raiseValueError(globalObject, scope, "year must be in "_s, minYear, ".."_s, maxYear, ", not "_s, isoYear);
    case IsoCalendarError::Week:
        return raiseValueError(globalObject, scope, "Invalid week: "_s, isoWeek);
    case IsoCalendarError::Day:
        return raiseValueError(globalObject, scope, "Invalid weekday: "_s, isoDay, " (range is [1, 7])"_s);
    case IsoCalendarError::None:
        break;
    }
    RELEASE_AND_RETURN(scope, makeDate(globalObject, stateOf(globalObject, type), type, year, month, day));
}

static JSValue dateStrptime(JSGlobalObject* globalObject, JSValue cls, JSValue string, JSValue format)
{
    return callStrptime(globalObject, stateOf(globalObject, PyType::from(cls)).strptimeDateName.get(), cls, string, format);
}

// ---- Arithmetic ----

static JSValue dateAddDays(JSGlobalObject* globalObject, PyDate* date, PyTimeDelta* delta, int factor)
{
    int64_t ordinal = toOrdinal(date->packed()) + static_cast<int64_t>(factor) * delta->days();
    if (ordinal < 1 || ordinal > maxOrdinal) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseOverflowError(globalObject, scope, "date value out of range"_s);
    }
    int year;
    int month;
    int day;
    fromOrdinal(static_cast<int>(ordinal), year, month, day);
    return makeDate(globalObject, stateOf(globalObject, date), date->type(), year, month, day);
}

// The operands are in the order in which they were written.
static JSValue dateAdd(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    if (!PyDateTime::is(left) && !PyDateTime::is(right)) {
        if (auto* date = PyDate::tryFrom(left)) {
            if (auto* delta = PyTimeDelta::tryFrom(right))
                return dateAddDays(globalObject, date, delta, 1);
        } else if (auto* delta = PyTimeDelta::tryFrom(left))
            return dateAddDays(globalObject, PyDate::from(right), delta, 1);
    }
    return runtime(globalObject).notImplemented();
}

static JSValue dateSubtract(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    if (!PyDateTime::is(left) && !PyDateTime::is(right)) {
        if (auto* date = PyDate::tryFrom(left)) {
            if (auto* other = PyDate::tryFrom(right))
                return newDelta(globalObject, stateOf(globalObject, date), toOrdinal(date->packed()) - toOrdinal(other->packed()), 0, 0);
            if (auto* delta = PyTimeDelta::tryFrom(right))
                return dateAddDays(globalObject, date, delta, -1);
        }
    }
    return runtime(globalObject).notImplemented();
}

template<JSValue (*operation)(JSGlobalObject*, JSValue, JSValue)>
static JSValue dateReflected(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    return operation(globalObject, other, self);
}

template<CompareOperator op>
static JSValue dateCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto* second = PyDate::tryFrom(other);
    if (!second || PyDateTime::is(other))
        return runtime(globalObject).notImplemented();
    // `self` may be a datetime, if the method was reached for by name: only the date counts.
    return compareResult(Packed::compare(PyDate::from(self)->packed() & ~Packed::timeMask, second->packed()), op);
}

static JSValue dateHash(JSGlobalObject*, JSValue self) { return smallInt(mixHash(PyDate::from(self)->packed() & ~Packed::timeMask)); }

// ---- Text ----

static JSValue dateRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* date = PyDate::from(self);
    return makeStr(globalObject, makeString(date->type()->nameInMessages(), '(', date->year(), ", "_s, date->month(), ", "_s, date->day(), ')'));
}

static JSValue dateIsoFormat(JSGlobalObject* globalObject, JSValue self)
{
    Text text;
    appendIsoDate(text, PyDate::from(self)->packed());
    return makeStr(getVM(globalObject), text.toString());
}

static JSValue dateStr(JSGlobalObject* globalObject, JSValue self)
{
    return callMethod(globalObject, self, stateOf(globalObject, PyDate::from(self)).isoformatName.get());
}

static JSValue dateCTime(JSGlobalObject* globalObject, JSValue self)
{
    return formatCTime(globalObject, PyDate::from(self)->packed() & ~Packed::timeMask);
}

static JSValue dateStrftime(JSGlobalObject* globalObject, JSValue self, JSValue format)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject, PyDate::from(self));
    JSString* formatString = strArgument(globalObject, "strftime"_s, "argument 'format'"_s, format);
    RETURN_IF_EXCEPTION(scope, { });
    // datetime inherits this, and has a timetuple() of its own.
    JSValue timeTuple = callMethod(globalObject, self, state.timetupleName.get());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, wrapStrftime(globalObject, state, self, formatString, timeTuple, self));
}

static JSValue dateFormat(JSGlobalObject* globalObject, JSValue self, JSValue format)
{
    return formatWithStrftime(globalObject, stateOf(globalObject, PyDate::from(self)), self, format);
}

// ---- The rest ----

static JSValue dateTimeTuple(JSGlobalObject* globalObject, JSValue self)
{
    auto* date = PyDate::from(self);
    return makeStructTime(globalObject, stateOf(globalObject, date), date->year(), date->month(), date->day(), 0, 0, 0, -1);
}

static JSValue makeIsoCalendarDate(JSGlobalObject* globalObject, PyType* type, int year, int week, int weekday)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyTuple* result = PyTuple::createUninitialized(vm, rt.structure<PyTuple>(), type, 3);
    result->initializeAt(vm, 0, jsNumber(year));
    result->initializeAt(vm, 1, jsNumber(week));
    result->initializeAt(vm, 2, jsNumber(weekday));
    return result;
}

static JSValue dateIsoCalendar(JSGlobalObject* globalObject, JSValue self)
{
    auto* date = PyDate::from(self);
    int year = date->year();
    int ordinal = toOrdinal(date->packed());
    int monday = isoWeek1Monday(year);
    if (ordinal < monday)
        monday = isoWeek1Monday(--year);
    else if (ordinal - monday >= 52 * 7 && ordinal >= isoWeek1Monday(year + 1))
        monday = isoWeek1Monday(++year);
    return makeIsoCalendarDate(globalObject, stateOf(globalObject, date).isoCalendarDateType.get(), year, (ordinal - monday) / 7 + 1, (ordinal - monday) % 7 + 1);
}

static JSValue dateIsoWeekday(JSGlobalObject*, JSValue self) { return jsNumber(weekdayOfOrdinal(toOrdinal(PyDate::from(self)->packed())) + 1); }
static JSValue dateWeekday(JSGlobalObject*, JSValue self) { return jsNumber(weekdayOfOrdinal(toOrdinal(PyDate::from(self)->packed()))); }
static JSValue dateToOrdinal(JSGlobalObject*, JSValue self) { return jsNumber(toOrdinal(PyDate::from(self)->packed())); }

static constexpr Parameters dateReplaceParameters { "replace"_s, dateParameterNames, 0, 3 };

// Also __replace__(), which complains under the same name.
static JSValue dateReplace(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* date = PyDate::from(self);
    int year = date->year();
    int month = date->month();
    int day = date->day();
    JSValue values[3];
    if (failed(scope, bindArguments(globalObject, dateReplaceParameters, args, values)) || (values[0] && failed(scope, intArgument(globalObject, values[0], year)))
        || (values[1] && failed(scope, intArgument(globalObject, values[1], month))) || (values[2] && failed(scope, intArgument(globalObject, values[2], day))))
        return { };
    RELEASE_AND_RETURN(scope, makeDate(globalObject, stateOf(globalObject, date), date->type(), year, month, day));
}

static JSValue dateReduce(JSGlobalObject* globalObject, JSValue self)
{
    auto* date = PyDate::from(self);
    uint8_t bytes[] = { static_cast<uint8_t>(date->year() >> 8), static_cast<uint8_t>(date->year()), static_cast<uint8_t>(date->month()), static_cast<uint8_t>(date->day()) };
    PyTuple* state = PyTuple::create(globalObject, { PyBytes::create(globalObject, std::span<const uint8_t> { bytes }) });
    return PyTuple::create(globalObject, { date->type(), state });
}

static JSValue dateGetYear(JSGlobalObject*, JSValue self) { return jsNumber(PyDate::from(self)->year()); }
static JSValue dateGetMonth(JSGlobalObject*, JSValue self) { return jsNumber(PyDate::from(self)->month()); }
static JSValue dateGetDay(JSGlobalObject*, JSValue self) { return jsNumber(PyDate::from(self)->day()); }

static constexpr NativeDef dateMethods[] = {
    PY_NEW(dateNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", dateRepr, "($self, /)"),
    PY_SLOT("__str__", dateStr, "($self, /)"),
    PY_SLOT("__hash__", dateHash, "($self, /)"),
    PY_DATETIME_COMPARISONS(dateCompare),
    PY_SLOT("__add__", dateAdd, "($self, value, /)"),
    PY_SLOT("__radd__", dateReflected<dateAdd>, "($self, value, /)"),
    PY_SLOT("__sub__", dateSubtract, "($self, value, /)"),
    PY_SLOT("__rsub__", dateReflected<dateSubtract>, "($self, value, /)"),
    PY_CLASSMETHOD("fromtimestamp", dateFromTimestamp, "($type, timestamp, /)"),
    PY_CLASSMETHOD("fromordinal", dateFromOrdinal, "($type, ordinal, /)"),
    PY_CLASSMETHOD("fromisoformat", dateFromIsoFormat, "($type, string, /)"),
    PY_CLASSMETHOD("fromisocalendar", dateFromIsoCalendar, "($type, /, year, week, day)"),
    PY_CLASSMETHOD("strptime", dateStrptime, "($type, string, format, /)"),
    PY_CLASSMETHOD("today", dateToday, "($type, /)"),
    PY_METHOD("ctime", dateCTime, "($self, /)"),
    PY_METHOD("strftime", dateStrftime, "($self, /, format)"),
    PY_METHOD("__format__", dateFormat, "($self, format, /)"),
    PY_METHOD("timetuple", dateTimeTuple, "($self, /)"),
    PY_METHOD("isocalendar", dateIsoCalendar, "($self, /)"),
    PY_METHOD("isoformat", dateIsoFormat, "($self, /)"),
    PY_METHOD("isoweekday", dateIsoWeekday, "($self, /)"),
    PY_METHOD("toordinal", dateToOrdinal, "($self, /)"),
    PY_METHOD("weekday", dateWeekday, "($self, /)"),
    PY_METHOD("replace", dateReplace, "!($self, /, year=unchanged, month=unchanged, day=unchanged)"),
    PY_METHOD("__replace__", dateReplace, "!($self, /, **changes)"),
    PY_METHOD("__reduce__", dateReduce, "($self, /)"),
};

static constexpr NativeGetSet dateGetSets[] = {
    { "year"_s, dateGetYear },
    { "month"_s, dateGetMonth },
    { "day"_s, dateGetDay },
};

constexpr TypeDef dateTypeDef { "datetime.date"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, dateMethods, dateGetSets, { } };

// ---- IsoCalendarDate ----

static JSValue isoCalendarDateNew(JSGlobalObject* globalObject, JSValue cls, JSValue yearValue, JSValue weekValue, JSValue weekdayValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int year;
    int week;
    int weekday;
    if (failed(scope, intArgument(globalObject, yearValue, year)) || failed(scope, intArgument(globalObject, weekValue, week)) || failed(scope, intArgument(globalObject, weekdayValue, weekday)))
        return { };
    RELEASE_AND_RETURN(scope, makeIsoCalendarDate(globalObject, PyType::from(cls), year, week, weekday));
}

static JSValue isoCalendarDateRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* tuple = PyTuple::from(self);
    return makeStr(globalObject, makeString(tuple->type()->nameInMessages(), "(year="_s, tuple->at(0).asInt32(), ", week="_s, tuple->at(1).asInt32(), ", weekday="_s, tuple->at(2).asInt32(), ')'));
}

// It is pickled as the plain tuple.
static JSValue isoCalendarDateReduce(JSGlobalObject* globalObject, JSValue self)
{
    auto* tuple = PyTuple::from(self);
    PyTuple* plain = PyTuple::create(globalObject, { tuple->at(0), tuple->at(1), tuple->at(2) });
    return PyTuple::create(globalObject, { runtime(globalObject).tupleType(), PyTuple::create(globalObject, { plain }) });
}

template<unsigned index>
static JSValue isoCalendarDateGet(JSGlobalObject*, JSValue self) { return PyTuple::from(self)->at(index); }

static constexpr NativeDef isoCalendarDateMethods[] = {
    PY_NEW(isoCalendarDateNew, "($type, /, year, week, weekday)"),
    PY_SLOT("__repr__", isoCalendarDateRepr, "($self, /)"),
    PY_METHOD("__reduce__", isoCalendarDateReduce, "($self, /)"),
};

static constexpr NativeGetSet isoCalendarDateGetSets[] = {
    { "year"_s, isoCalendarDateGet<0> },
    { "week"_s, isoCalendarDateGet<1> },
    { "weekday"_s, isoCalendarDateGet<2> },
};

constexpr TypeDef isoCalendarDateTypeDef {
    "datetime.IsoCalendarDate"_s, { TypeFlag::Immutable }, isoCalendarDateMethods, isoCalendarDateGetSets, { }
};

} // namespace DateTime
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
