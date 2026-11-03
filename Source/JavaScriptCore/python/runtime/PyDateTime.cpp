/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyDateTime.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyModule.h"
#include "PyTuple.h"
#include <ctime>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace DateTime {

// ---- The cells ----

PY_DEFINE_CELL(PyTimeDelta);
PY_DEFINE_CELL(PyDate);
PY_DEFINE_CELL(PyDateTime);
PY_DEFINE_CELL(PyTimeOfDay);
PY_DEFINE_CELL(PyTimeZone);
const ClassInfo State::s_info = { "PyDateTimeState"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(State) };

PyTimeDelta::PyTimeDelta(VM& vm, Structure* structure, PyType* type, int days, int seconds, int microseconds)
    : Base(vm, structure, type)
    , m_days(days)
    , m_seconds(seconds)
    , m_microseconds(microseconds)
{
}

PyTimeDelta* PyTimeDelta::create(VM& vm, Structure* structure, PyType* type, int days, int seconds, int microseconds)
{
    auto* delta = new (NotNull, allocateCell<PyTimeDelta>(vm)) PyTimeDelta(vm, structure, type, days, seconds, microseconds);
    delta->finishCreation(vm);
    return delta;
}

PyDate::PyDate(VM& vm, Structure* structure, PyType* type, uint64_t packed)
    : Base(vm, structure, type)
    , m_packed(packed)
{
}

PyDate* PyDate::create(VM& vm, Structure* structure, PyType* type, uint64_t packed)
{
    auto* date = new (NotNull, allocateCell<PyDate>(vm)) PyDate(vm, structure, type, packed);
    date->finishCreation(vm);
    return date;
}

PyDateTime::PyDateTime(VM& vm, Structure* structure, PyType* type, uint64_t packed)
    : Base(vm, structure, type, packed)
{
}

PyDateTime* PyDateTime::create(VM& vm, Structure* structure, PyType* type, uint64_t packed, JSValue tzinfo)
{
    auto* dateTime = new (NotNull, allocateCell<PyDateTime>(vm)) PyDateTime(vm, structure, type, packed);
    dateTime->finishCreation(vm);
    dateTime->m_tzinfo.set(vm, dateTime, isNone(tzinfo) ? none() : tzinfo);
    return dateTime;
}

template<typename Visitor>
void PyDateTime::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyDateTime>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_tzinfo);
}

DEFINE_VISIT_CHILDREN(PyDateTime);

PyTimeOfDay::PyTimeOfDay(VM& vm, Structure* structure, PyType* type, uint64_t packed)
    : Base(vm, structure, type)
    , m_packed(packed)
{
}

PyTimeOfDay* PyTimeOfDay::create(VM& vm, Structure* structure, PyType* type, uint64_t packed, JSValue tzinfo)
{
    auto* time = new (NotNull, allocateCell<PyTimeOfDay>(vm)) PyTimeOfDay(vm, structure, type, packed);
    time->finishCreation(vm);
    time->m_tzinfo.set(vm, time, isNone(tzinfo) ? none() : tzinfo);
    return time;
}

template<typename Visitor>
void PyTimeOfDay::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyTimeOfDay>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_tzinfo);
}

DEFINE_VISIT_CHILDREN(PyTimeOfDay);

PyTimeZone::PyTimeZone(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyTimeZone* PyTimeZone::create(VM& vm, Structure* structure, PyType* type, PyTimeDelta* offset, JSValue name)
{
    auto* zone = new (NotNull, allocateCell<PyTimeZone>(vm)) PyTimeZone(vm, structure, type);
    zone->finishCreation(vm);
    zone->m_offset.set(vm, zone, offset);
    if (name)
        zone->m_name.set(vm, zone, name);
    return zone;
}

template<typename Visitor>
void PyTimeZone::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyTimeZone>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_offset);
    visitor.append(thisObject->m_name);
}

DEFINE_VISIT_CHILDREN(PyTimeZone);

State::State(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

State* State::create(VM& vm, Structure* structure, PyType* type)
{
    auto* state = new (NotNull, allocateCell<State>(vm)) State(vm, structure, type);
    state->finishCreation(vm);
    return state;
}

template<typename Visitor>
void State::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<State>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->deltaType);
    visitor.append(thisObject->dateType);
    visitor.append(thisObject->dateTimeType);
    visitor.append(thisObject->timeType);
    visitor.append(thisObject->tzinfoType);
    visitor.append(thisObject->timeZoneType);
    visitor.append(thisObject->isoCalendarDateType);
    visitor.append(thisObject->deltaStructure);
    visitor.append(thisObject->dateStructure);
    visitor.append(thisObject->dateTimeStructure);
    visitor.append(thisObject->timeStructure);
    visitor.append(thisObject->timeZoneStructure);
    visitor.append(thisObject->zeroDelta);
    visitor.append(thisObject->utc);
    visitor.append(thisObject->epoch);
#define VISIT_NAME(id, text) visitor.append(thisObject->id##Name);
    FOR_EACH_DATETIME_NAME(VISIT_NAME)
#undef VISIT_NAME
}

DEFINE_VISIT_CHILDREN(State);

// ---- The state ----

// Never seen from Python.
static constexpr TypeDef stateTypeDef { "datetime._state"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation, TypeFlag::Internal }, { }, { }, { } };

static State* createState(JSGlobalObject* globalObject, PyType* holder)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    State* state = State::create(vm, State::createStructure(vm, globalObject), holder);
    setHiddenField(globalObject, holder, 0, state);

    auto addType = [&](WriteBarrier<PyType>& field, const TypeDef& def, PyType* base) {
        PyType* type = rt.typeFor(def, base);
        field.set(vm, state, type);
        setHiddenField(globalObject, type, 0, state);
    };
    addType(state->deltaType, deltaTypeDef, nullptr);
    addType(state->dateType, dateTypeDef, nullptr);
    addType(state->dateTimeType, dateTimeTypeDef, state->dateType.get());
    addType(state->timeType, timeTypeDef, nullptr);
    addType(state->tzinfoType, tzinfoTypeDef, nullptr);
    addType(state->timeZoneType, timeZoneTypeDef, state->tzinfoType.get());
    addType(state->isoCalendarDateType, isoCalendarDateTypeDef, rt.tupleType());

    state->deltaStructure.set(vm, state, PyTimeDelta::createStructure(vm, globalObject));
    state->dateStructure.set(vm, state, PyDate::createStructure(vm, globalObject));
    state->dateTimeStructure.set(vm, state, PyDateTime::createStructure(vm, globalObject));
    state->timeStructure.set(vm, state, PyTimeOfDay::createStructure(vm, globalObject));
    state->timeZoneStructure.set(vm, state, PyTimeZone::createStructure(vm, globalObject));

#define INTERN_NAME(id, text) state->id##Name.set(vm, state, internString(vm, text ""_s));
    FOR_EACH_DATETIME_NAME(INTERN_NAME)
#undef INTERN_NAME

    auto delta = [&](int days, int seconds, int microseconds) {
        return PyTimeDelta::create(vm, state->deltaStructure.get(), state->deltaType.get(), days, seconds, microseconds);
    };
    auto zone = [&](PyTimeDelta* offset) {
        return PyTimeZone::create(vm, state->timeZoneStructure.get(), state->timeZoneType.get(), offset, { });
    };
    auto dateTime = [&](const Fields& fields, JSValue tzinfo) {
        return PyDateTime::create(vm, state->dateTimeStructure.get(), state->dateTimeType.get(), Packed::all(fields), tzinfo);
    };
    auto time = [&](int hour, int minute, int second, int microsecond) {
        return PyTimeOfDay::create(vm, state->timeStructure.get(), state->timeType.get(), Packed::time(hour, minute, second, microsecond, 0), none());
    };
    auto date = [&](int year, int month, int day) {
        return PyDate::create(vm, state->dateStructure.get(), state->dateType.get(), Packed::date(year, month, day));
    };
    state->zeroDelta.set(vm, state, delta(0, 0, 0));
    state->utc.set(vm, state, zone(state->zeroDelta.get()));
    state->epoch.set(vm, state, dateTime({ 1970, 1, 1 }, state->utc.get()));

    auto setAttributes = [&](PyType* type, std::initializer_list<std::pair<ASCIILiteral, JSValue>> attributes) {
        for (auto& [name, value] : attributes)
            type->dict()->set(vm, internString(vm, name), value);
        type->didChange(vm);
    };
    setAttributes(state->deltaType.get(), {
        { "resolution"_s, delta(0, 0, 1) },
        { "min"_s, delta(-maxDeltaDays, 0, 0) },
        { "max"_s, delta(maxDeltaDays, secondsPerDay - 1, microsecondsPerSecond - 1) } });
    setAttributes(state->dateType.get(), {
        { "min"_s, date(minYear, 1, 1) },
        { "max"_s, date(maxYear, 12, 31) },
        { "resolution"_s, delta(1, 0, 0) } });
    setAttributes(state->timeType.get(), {
        { "min"_s, time(0, 0, 0, 0) },
        { "max"_s, time(23, 59, 59, 999999) },
        { "resolution"_s, delta(0, 0, 1) } });
    setAttributes(state->dateTimeType.get(), {
        { "min"_s, dateTime({ minYear, 1, 1 }, none()) },
        { "max"_s, dateTime({ maxYear, 12, 31, 23, 59, 59, 999999 }, none()) },
        { "resolution"_s, delta(0, 0, 1) } });
    setAttributes(state->timeZoneType.get(), {
        { "utc"_s, state->utc.get() },
        { "min"_s, zone(delta(-1, 60, 0)) },
        { "max"_s, zone(delta(0, secondsPerDay - 60, 0)) } });
    setAttributes(state->isoCalendarDateType.get(), { { "__module__"_s, internString(vm, "datetime"_s) } });
    return state;
}

State& stateOf(JSGlobalObject* globalObject)
{
    PyType* holder = runtime(globalObject).typeFor(stateTypeDef);
    if (JSValue state = hiddenField(globalObject, holder, 0)) [[likely]]
        return *State::from(state);
    return *createState(globalObject, holder);
}

// ---- The calendar ----

IsoCalendarError fromIsoCalendar(int isoYear, int isoWeek, int isoDay, int& year, int& month, int& day)
{
    if (isoYear < minYear || isoYear > maxYear)
        return IsoCalendarError::Year;
    if (isoWeek < 1 || isoWeek > 52) {
        // A year has a week 53 if it starts on a Thursday, or on a Wednesday and is a leap year.
        if (isoWeek != 53)
            return IsoCalendarError::Week;
        int firstWeekday = weekdayOfOrdinal(toOrdinal(isoYear, 1, 1));
        if (firstWeekday != 3 && !(firstWeekday == 2 && isLeapYear(isoYear)))
            return IsoCalendarError::Week;
    }
    if (isoDay < 1 || isoDay > 7)
        return IsoCalendarError::Day;
    fromOrdinal(isoWeek1Monday(isoYear) + (isoWeek - 1) * 7 + isoDay - 1, year, month, day);
    return IsoCalendarError::None;
}

// ---- Arguments ----

bool intArgumentSlow(JSGlobalObject* globalObject, JSValue value, int& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    result = toCInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

JSValue raiseBadArgument(JSGlobalObject* globalObject, ASCIILiteral function, ASCIILiteral which, const String& expected, JSValue given)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, function, "() "_s, which, " must be "_s, expected, ", not "_s, typeNameOfArgument(globalObject, given));
}

JSString* strArgument(JSGlobalObject* globalObject, ASCIILiteral function, ASCIILiteral which, JSValue value)
{
    if (isStr(value)) [[likely]]
        return strValue(value);
    raiseBadArgument(globalObject, function, which, "str"_s, value);
    return nullptr;
}

bool bindArguments(JSGlobalObject* globalObject, const Parameters& parameters, const CallArgs& args, JSValue* values)
{
    unsigned count = args.size();
    if (!args.hasKeywords() && count >= parameters.required && count <= parameters.maxPositional) [[likely]] {
        for (unsigned i = 0; i < count; ++i)
            values[i] = args[i];
        return true;
    }

    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASCIILiteral name = parameters.function;
    unsigned total = parameters.names.size();
    unsigned max = parameters.maxPositional;
    unsigned keywordCount = args.keywordCount();
    auto plural = [](unsigned number) { return number == 1 ? ""_s : "s"_s; };

    if (count + keywordCount > total) {
        raiseTypeError(globalObject, scope, name, "() takes at most "_s, total, ' ', !count ? "keyword "_s : ""_s, "argument"_s, plural(total), " ("_s, count + keywordCount, " given)"_s);
        return false;
    }
    if (count > max) {
        raiseTypeError(globalObject, scope, name, "() takes "_s, parameters.required == max ? "exactly "_s : "at most "_s, max, " positional argument"_s, plural(max), " ("_s, count, " given)"_s);
        return false;
    }

    for (unsigned i = 0; i < count; ++i)
        values[i] = args[i];
    auto parameterOf = [&](unsigned keyword) {
        StringImpl* given = args.keywordName(keyword)->tryGetValueImpl();
        unsigned index = 0;
        while (index < total && !WTF::equal(given, parameters.names[index].span8()))
            ++index;
        return index;
    };
    bool allKeywordsFit = true;
    for (unsigned k = 0; k < keywordCount; ++k) {
        unsigned index = parameterOf(k);
        if (index < total && !values[index])
            values[index] = args.keywordValue(k);
        else
            allKeywordsFit = false;
    }

    // A parameter left out is complained of first, then one given twice, then a keyword that is no parameter.
    for (unsigned i = 0; i < parameters.required; ++i) {
        if (!values[i]) {
            raiseTypeError(globalObject, scope, name, "() missing required argument '"_s, parameters.names[i], "' (pos "_s, i + 1, ')');
            return false;
        }
    }
    if (allKeywordsFit)
        return true;
    unsigned firstGivenTwice = total;
    for (unsigned k = 0; k < keywordCount; ++k)
        firstGivenTwice = std::min(firstGivenTwice, parameterOf(k) < count ? parameterOf(k) : total);
    if (firstGivenTwice < total) {
        raiseTypeError(globalObject, scope, "argument for "_s, name, "() given by name ('"_s, parameters.names[firstGivenTwice], "') and position ("_s, firstGivenTwice + 1, ')');
        return false;
    }
    unsigned unexpected = 0;
    while (parameterOf(unexpected) < total)
        ++unexpected;
    String keyword = stringOf(args.keywordName(unexpected));
    Vector<String> candidates;
    for (ASCIILiteral candidate : parameters.names)
        candidates.append(String(candidate));
    String suggestion = calculateSuggestion(candidates, keyword);
    if (suggestion.isNull())
        raiseTypeError(globalObject, scope, name, "() got an unexpected keyword argument '"_s, keyword, '\'');
    else
        raiseTypeError(globalObject, scope, name, "() got an unexpected keyword argument '"_s, keyword, "'. Did you mean '"_s, suggestion, "'?"_s);
    return false;
}

// ---- Making the objects ----

bool checkDateFields(JSGlobalObject* globalObject, int year, int month, int day)
{
    if (year >= minYear && year <= maxYear && month >= 1 && month <= 12 && day >= 1 && day <= daysInMonth(year, month)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (year < minYear || year > maxYear)
        raiseValueError(globalObject, scope, "year must be in "_s, minYear, ".."_s, maxYear, ", not "_s, year);
    else if (month < 1 || month > 12)
        raiseValueError(globalObject, scope, "month must be in 1..12, not "_s, month);
    else
        raiseValueError(globalObject, scope, "day "_s, day, " must be in range 1.."_s, daysInMonth(year, month), " for month "_s, month, " in year "_s, year);
    return false;
}

bool checkTimeFields(JSGlobalObject* globalObject, int hour, int minute, int second, int microsecond, int fold)
{
    auto isWithin = [](int value, int limit) { return static_cast<unsigned>(value) < static_cast<unsigned>(limit); };
    if (isWithin(hour, 24) && isWithin(minute, 60) && isWithin(second, 60) && isWithin(microsecond, microsecondsPerSecond) && isWithin(fold, 2)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isWithin(hour, 24))
        raiseValueError(globalObject, scope, "hour must be in 0..23, not "_s, hour);
    else if (!isWithin(minute, 60))
        raiseValueError(globalObject, scope, "minute must be in 0..59, not "_s, minute);
    else if (!isWithin(second, 60))
        raiseValueError(globalObject, scope, "second must be in 0..59, not "_s, second);
    else if (!isWithin(microsecond, microsecondsPerSecond))
        raiseValueError(globalObject, scope, "microsecond must be in 0..999999, not "_s, microsecond);
    else
        raiseValueError(globalObject, scope, "fold must be either 0 or 1, not "_s, fold);
    return false;
}

bool checkTzInfo(JSGlobalObject* globalObject, State& state, JSValue value)
{
    if (isNone(value) || isTzInfo(globalObject, state, value)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "tzinfo argument must be None or of a tzinfo subclass, not type '"_s, typeName(globalObject, value), '\'');
    return false;
}

JSValue newDelta(JSGlobalObject* globalObject, State& state, PyType* type, int64_t days, int64_t seconds, int64_t microseconds)
{
    VM& vm = getVM(globalObject);
    if (static_cast<uint64_t>(microseconds) >= static_cast<uint64_t>(microsecondsPerSecond))
        seconds += floorDivide<int64_t>(microseconds, microsecondsPerSecond, microseconds);
    if (static_cast<uint64_t>(seconds) >= static_cast<uint64_t>(secondsPerDay))
        days += floorDivide<int64_t>(seconds, secondsPerDay, seconds);
    if (days < -maxDeltaDays || days > maxDeltaDays) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseOverflowError(globalObject, scope, "days="_s, days, "; must have magnitude <= "_s, maxDeltaDays);
    }
    if (!(days | seconds | microseconds) && type == state.deltaType.get())
        return state.zeroDelta.get();
    return PyTimeDelta::create(vm, state.deltaStructure.get(), type, static_cast<int>(days), static_cast<int>(seconds), static_cast<int>(microseconds));
}

JSValue newDeltaFromTotal(JSGlobalObject* globalObject, State& state, PyType* type, Int128 total)
{
    Int128 microseconds;
    Int128 seconds;
    Int128 days = floorDivide<Int128>(floorDivide<Int128>(total, microsecondsPerSecond, microseconds), secondsPerDay, seconds);
    if (days < std::numeric_limits<int>::min() || days > std::numeric_limits<int>::max()) [[unlikely]] {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseOverflowError(globalObject, scope, "Python int too large to convert to C int"_s);
    }
    return newDelta(globalObject, state, type, static_cast<int64_t>(days), static_cast<int64_t>(seconds), static_cast<int64_t>(microseconds));
}

JSValue newDate(JSGlobalObject* globalObject, State& state, PyType* type, int year, int month, int day)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkDateFields(globalObject, year, month, day))) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, PyDate::create(getVM(globalObject), state.dateStructure.get(), type, Packed::date(year, month, day)));
}

JSValue newTime(JSGlobalObject* globalObject, State& state, PyType* type, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkTimeFields(globalObject, hour, minute, second, microsecond, fold)) || failed(scope, checkTzInfo(globalObject, state, tzinfo))) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, PyTimeOfDay::create(getVM(globalObject), state.timeStructure.get(), type, Packed::time(hour, minute, second, microsecond, fold), tzinfo));
}

JSValue newDateTime(JSGlobalObject* globalObject, State& state, PyType* type, const Fields& fields, JSValue tzinfo)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkDateFields(globalObject, fields.year, fields.month, fields.day))
        || failed(scope, checkTimeFields(globalObject, fields.hour, fields.minute, fields.second, fields.microsecond, fields.fold))
        || failed(scope, checkTzInfo(globalObject, state, tzinfo))) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, PyDateTime::create(getVM(globalObject), state.dateTimeStructure.get(), type, Packed::all(fields), tzinfo));
}

static bool isOffsetInRange(PyTimeDelta* offset)
{
    // Strictly between -24 and 24 hours.
    return !offset->days() || (offset->days() == -1 && (offset->seconds() || offset->microseconds()));
}

static JSValue raiseOffsetOutOfRange(JSGlobalObject* globalObject, PyTimeDelta* offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String shown = reprForMessage(globalObject, offset);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseValueError(globalObject, scope, "offset must be a timedelta strictly between -timedelta(hours=24) and timedelta(hours=24), not "_s, shown);
}

JSValue newTimeZone(JSGlobalObject* globalObject, State& state, PyTimeDelta* offset, JSValue name)
{
    if (!name && offset->isZero())
        return state.utc.get();
    if (!isOffsetInRange(offset))
        return raiseOffsetOutOfRange(globalObject, offset);
    return PyTimeZone::create(getVM(globalObject), state.timeZoneStructure.get(), state.timeZoneType.get(), offset, name);
}

JSValue makeDate(JSGlobalObject* globalObject, State& state, PyType* cls, int year, int month, int day)
{
    if (cls == state.dateType.get()) [[likely]]
        return newDate(globalObject, state, cls, year, month, day);
    if (cls == state.dateTimeType.get())
        return newDateTime(globalObject, state, cls, { year, month, day }, none());
    return call(globalObject, cls, jsNumber(year), jsNumber(month), jsNumber(day));
}

// cls(*arguments), with fold=1 if that is what it is.
static JSValue callWithFold(JSGlobalObject* globalObject, State& state, PyType* cls, std::span<const JSValue> arguments, int fold)
{
    if (!fold)
        return call(globalObject, cls, CallArgs(arguments));
    VM& vm = getVM(globalObject);
    PyDict* keywords = PyDict::create(globalObject);
    keywords->set(vm, state.foldName.get(), jsNumber(fold));
    return callWithTupleAndDict(globalObject, cls, PyTuple::create(globalObject, arguments), keywords);
}

JSValue makeTime(JSGlobalObject* globalObject, State& state, PyType* cls, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold)
{
    if (cls == state.timeType.get()) [[likely]]
        return newTime(globalObject, state, cls, hour, minute, second, microsecond, tzinfo, fold);
    JSValue arguments[] = { jsNumber(hour), jsNumber(minute), jsNumber(second), jsNumber(microsecond), tzinfo };
    return callWithFold(globalObject, state, cls, arguments, fold);
}

JSValue makeDateTime(JSGlobalObject* globalObject, State& state, PyType* cls, const Fields& fields, JSValue tzinfo)
{
    if (cls == state.dateTimeType.get()) [[likely]]
        return newDateTime(globalObject, state, cls, fields, tzinfo);
    JSValue arguments[] = {
        jsNumber(fields.year), jsNumber(fields.month), jsNumber(fields.day), jsNumber(fields.hour), jsNumber(fields.minute),
        jsNumber(fields.second), jsNumber(fields.microsecond), tzinfo
    };
    return callWithFold(globalObject, state, cls, arguments, fields.fold);
}

// ---- Time zones ----

static JSValue callOffsetMethod(JSGlobalObject* globalObject, JSValue tzinfo, JSString* name, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue offset = callMethod(globalObject, tzinfo, name, argument);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(offset))
        return none();
    auto* delta = PyTimeDelta::tryFrom(offset);
    if (!delta)
        return raiseTypeError(globalObject, scope, "tzinfo."_s, stringOf(name), "() must return None or timedelta, not '"_s, typeName(globalObject, offset), '\'');
    if (!isOffsetInRange(delta))
        RELEASE_AND_RETURN(scope, raiseOffsetOutOfRange(globalObject, delta));
    return delta;
}

JSValue callUtcOffset(JSGlobalObject* globalObject, State& state, JSValue tzinfo, JSValue argument)
{
    if (isNone(tzinfo))
        return none();
    // Nothing can be derived from timezone, nor its methods replaced.
    if (auto* zone = PyTimeZone::tryFrom(tzinfo))
        return zone->offset();
    return callOffsetMethod(globalObject, tzinfo, state.utcoffsetName.get(), argument);
}

JSValue callDst(JSGlobalObject* globalObject, State& state, JSValue tzinfo, JSValue argument)
{
    if (isNone(tzinfo) || PyTimeZone::is(tzinfo))
        return none();
    return callOffsetMethod(globalObject, tzinfo, state.dstName.get(), argument);
}

JSValue callTzName(JSGlobalObject* globalObject, State& state, JSValue tzinfo, JSValue argument)
{
    if (isNone(tzinfo))
        return none();
    if (auto* zone = PyTimeZone::tryFrom(tzinfo)) {
        if (JSValue name = zone->name())
            return name;
        return timeZoneName(globalObject, state, zone);
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue name = callMethod(globalObject, tzinfo, state.tznameName.get(), argument);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(name))
        return none();
    if (!isStr(name))
        return raiseTypeError(globalObject, scope, "tzinfo.tzname() must return None or a string, not '"_s, typeName(globalObject, name), '\'');
    return name;
}

// ---- Text ----

void Text::append(int64_t number, unsigned minWidth, char padding)
{
    char digits[24];
    unsigned count = 0;
    uint64_t magnitude = number < 0 ? -static_cast<uint64_t>(number) : static_cast<uint64_t>(number);
    do {
        digits[count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    unsigned width = count + (number < 0);
    if (number < 0 && padding == '0')
        append('-');
    for (; width < minWidth; ++width)
        append(padding);
    if (number < 0 && padding != '0')
        append('-');
    while (count)
        append(digits[--count]);
}

void appendIsoDate(Text& text, uint64_t packed)
{
    text.append(Packed::year(packed), 4);
    text.append('-');
    text.append(Packed::month(packed), 2);
    text.append('-');
    text.append(Packed::day(packed), 2);
}

bool timeSpecArgument(JSGlobalObject* globalObject, JSValue value, int microsecond, TimeSpec& result)
{
    result = microsecond ? TimeSpec::Microseconds : TimeSpec::Seconds;
    if (!value)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = strArgument(globalObject, "isoformat"_s, "argument 'timespec'"_s, value);
    RETURN_IF_EXCEPTION(scope, false);
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (!text.is8Bit() && !tryEncodeUTF8(text)) {
        // Which raises.
        strEncode(globalObject, string, makeStr(vm, "utf-8"_s), makeStr(vm, "strict"_s));
        return false;
    }
    if (text.contains('\0')) {
        raiseValueError(globalObject, scope, "embedded null character"_s);
        return false;
    }
    if (text == "auto"_s)
        return true;
    static constexpr ASCIILiteral names[] = { "hours"_s, "minutes"_s, "seconds"_s, "milliseconds"_s, "microseconds"_s };
    for (unsigned i = 0; i < std::size(names); ++i) {
        if (text == names[i]) {
            result = static_cast<TimeSpec>(i);
            return true;
        }
    }
    raiseValueError(globalObject, scope, "Unknown timespec value"_s);
    return false;
}

void appendIsoTime(Text& text, uint64_t packed, TimeSpec spec)
{
    text.append(Packed::hour(packed), 2);
    if (spec == TimeSpec::Hours)
        return;
    text.append(':');
    text.append(Packed::minute(packed), 2);
    if (spec == TimeSpec::Minutes)
        return;
    text.append(':');
    text.append(Packed::second(packed), 2);
    if (spec == TimeSpec::Seconds)
        return;
    text.append('.');
    if (spec == TimeSpec::Milliseconds)
        text.append(Packed::microsecond(packed) / 1000, 3);
    else
        text.append(Packed::microsecond(packed), 6);
}

void appendOffset(Text& text, ASCIILiteral separator, PyTimeDelta* offset)
{
    int64_t total = static_cast<int64_t>(offset->total());
    text.append(total < 0 ? '-' : '+');
    total = std::abs(total);
    int microseconds = static_cast<int>(total % microsecondsPerSecond);
    int seconds = static_cast<int>(total / microsecondsPerSecond);
    text.append(seconds / 3600, 2);
    text.append(separator);
    text.append(seconds / 60 % 60, 2);
    if (!(seconds % 60) && !microseconds)
        return;
    text.append(separator);
    text.append(seconds % 60, 2);
    if (microseconds) {
        text.append('.');
        text.append(microseconds, 6);
    }
}

bool appendUtcOffset(JSGlobalObject* globalObject, State& state, Text& text, ASCIILiteral separator, JSValue tzinfo, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue offset = callUtcOffset(globalObject, state, tzinfo, argument);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isNone(offset))
        appendOffset(text, separator, PyTimeDelta::from(offset));
    return true;
}

JSString* timeZoneName(JSGlobalObject* globalObject, State&, PyTimeZone* zone)
{
    VM& vm = getVM(globalObject);
    if (zone->offset()->isZero())
        return makeStr(vm, "UTC"_s);
    Text text;
    text.append("UTC"_s);
    appendOffset(text, ":"_s, zone->offset());
    return makeStr(vm, text.toString());
}

JSValue formatCTime(JSGlobalObject* globalObject, uint64_t packed)
{
    static constexpr ASCIILiteral dayNames[] = { "Mon"_s, "Tue"_s, "Wed"_s, "Thu"_s, "Fri"_s, "Sat"_s, "Sun"_s };
    static constexpr ASCIILiteral monthNames[] = { "Jan"_s, "Feb"_s, "Mar"_s, "Apr"_s, "May"_s, "Jun"_s, "Jul"_s, "Aug"_s, "Sep"_s, "Oct"_s, "Nov"_s, "Dec"_s };
    Text text;
    text.append(dayNames[weekdayOfOrdinal(toOrdinal(packed))]);
    text.append(' ');
    text.append(monthNames[(Packed::month(packed) + 11) % 12]);
    text.append(' ');
    text.append(Packed::day(packed), 2, ' ');
    text.append(' ');
    appendIsoTime(text, packed, TimeSpec::Seconds);
    text.append(' ');
    text.append(Packed::year(packed), 4);
    return makeStr(getVM(globalObject), text.toString());
}

static JSValue attributeOfTimeModule(JSGlobalObject* globalObject, JSString* name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, "time"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getAttr(globalObject, module, name));
}

JSValue makeStructTime(JSGlobalObject* globalObject, State& state, int year, int month, int day, int hour, int minute, int second, int dstFlag)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue structTime = attributeOfTimeModule(globalObject, state.structTimeName.get());
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* fields = PyTuple::create(globalObject, {
        jsNumber(year), jsNumber(month), jsNumber(day), jsNumber(hour), jsNumber(minute), jsNumber(second),
        jsNumber(weekdayOfOrdinal(toOrdinal(year, month, day))), jsNumber(daysBeforeMonth(year, month) + day), jsNumber(dstFlag) });
    RELEASE_AND_RETURN(scope, call(globalObject, structTime, fields));
}

// Whether the C library writes the year 99 as "99" where Python says "0099".
static bool libraryLeavesYearsUnpadded()
{
    static const bool result = [] {
        struct tm tm { };
        tm.tm_year = -1801;
        tm.tm_mday = 1;
        char year[5];
        return strftime(year, sizeof(year), "%Y", &tm) && strcmp(year, "0099");
    }();
    return result;
}

JSValue wrapStrftime(JSGlobalObject* globalObject, State& state, JSValue object, JSString* formatString, JSValue timeTuple, JSValue tzinfoArgument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue strftimeFunction = attributeOfTimeModule(globalObject, state.strftimeName.get());
    RETURN_IF_EXCEPTION(scope, { });
    String format = formatString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue tzinfo = none();
    int microsecond = 0;
    if (auto* dateTime = PyDateTime::tryFrom(object)) {
        tzinfo = dateTime->tzinfo();
        microsecond = dateTime->microsecond();
    } else if (auto* time = PyTimeOfDay::tryFrom(object)) {
        tzinfo = time->tzinfo();
        microsecond = time->microsecond();
    }

    // Each is made when first asked for. Null: not yet.
    String offsetText;
    String offsetWithColonsText;
    String zoneName;
    auto makeOffsetText = [&](String& text, ASCIILiteral separator) {
        Text buffer;
        if (failed(scope, appendUtcOffset(globalObject, state, buffer, separator, tzinfo, tzinfoArgument)))
            return false;
        text = buffer.isEmpty() ? emptyString() : buffer.toString();
        return true;
    };

    StrBuilder result;
    unsigned length = format.length();
    unsigned copied = 0;
    unsigned i = 0;
    while (i < length) {
        size_t percent = format.find('%', i);
        if (percent == notFound || percent + 1 == length)
            break;
        char16_t directive = format[percent + 1];
        i = percent + 2;
        auto replaceWith = [&](auto&& replacement) {
            result.append(StringView(format).substring(copied, percent - copied));
            result.append(replacement);
            copied = i;
        };
        if (directive == 'z') {
            if (offsetText.isNull()) {
                bool isMade = makeOffsetText(offsetText, ""_s);
                RETURN_IF_EXCEPTION(scope, { });
                ASSERT_UNUSED(isMade, isMade);
            }
            replaceWith(offsetText);
        } else if (directive == ':' && i < length && format[i] == 'z') {
            ++i;
            if (offsetWithColonsText.isNull()) {
                bool isMade = makeOffsetText(offsetWithColonsText, ":"_s);
                RETURN_IF_EXCEPTION(scope, { });
                ASSERT_UNUSED(isMade, isMade);
            }
            replaceWith(offsetWithColonsText);
        } else if (directive == 'Z') {
            if (zoneName.isNull()) {
                zoneName = emptyString();
                JSValue name = callTzName(globalObject, state, tzinfo, tzinfoArgument);
                RETURN_IF_EXCEPTION(scope, { });
                if (!isNone(name)) {
                    // It goes through strftime() too.
                    JSValue escaped = callMethod(globalObject, name, state.replaceName.get(), makeStr(vm, "%"_s), makeStr(vm, "%%"_s));
                    RETURN_IF_EXCEPTION(scope, { });
                    if (!isStr(escaped))
                        return raiseTypeError(globalObject, scope, "tzname.replace() did not return a string"_s);
                    zoneName = strValue(escaped)->value(globalObject);
                    RETURN_IF_EXCEPTION(scope, { });
                }
            }
            replaceWith(zoneName);
        } else if (directive == 'f') {
            Text digits;
            digits.append(microsecond, 6);
            replaceWith(digits.span());
        } else if ((directive == 'Y' || directive == 'G' || directive == 'F' || directive == 'C') && libraryLeavesYearsUnpadded()) {
            if (!PyTuple::is(timeTuple) && !typeOf(globalObject, timeTuple)->lookup(Special::GetItem))
                return raiseTypeError(globalObject, scope, '\'', typeName(globalObject, timeTuple), "' object does not support indexing"_s);
            JSValue item = getItem(globalObject, timeTuple, jsNumber(0));
            RETURN_IF_EXCEPTION(scope, { });
            int64_t year = toInt64(globalObject, item);
            RETURN_IF_EXCEPTION(scope, { });
            if (year >= 1000)
                continue;
            if (directive == 'G') {
                JSValue isoYearText = call(globalObject, strftimeFunction, makeStr(vm, "%G"_s), timeTuple);
                RETURN_IF_EXCEPTION(scope, { });
                JSValue isoYear = call(globalObject, runtime(globalObject).intType(), isoYearText);
                RETURN_IF_EXCEPTION(scope, { });
                year = toInt64(globalObject, isoYear);
                RETURN_IF_EXCEPTION(scope, { });
            }
            Text digits;
            digits.append(year, 4);
            if (directive == 'F')
                digits.append("-%m-%d"_s);
            auto span = digits.span();
            replaceWith(directive == 'C' ? span.first(span.size() - 2) : span);
        }
    }

    JSValue newFormat = formatString;
    if (copied) {
        result.append(StringView(format).substring(copied));
        newFormat = makeStr(globalObject, result);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, call(globalObject, strftimeFunction, newFormat, timeTuple));
}

JSValue formatWithStrftime(JSGlobalObject* globalObject, State& state, JSValue self, JSValue format)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = strArgument(globalObject, "__format__"_s, "argument"_s, format);
    RETURN_IF_EXCEPTION(scope, { });
    if (!string->length())
        RELEASE_AND_RETURN(scope, strObject(globalObject, self));
    RELEASE_AND_RETURN(scope, callMethod(globalObject, self, state.strftimeName.get(), format));
}

JSValue callStrptime(JSGlobalObject* globalObject, JSString* name, JSValue cls, JSValue string, JSValue format)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    strArgument(globalObject, "strptime"_s, "argument 1"_s, string);
    RETURN_IF_EXCEPTION(scope, { });
    strArgument(globalObject, "strptime"_s, "argument 2"_s, format);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue module = importModule(globalObject, "_strptime"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callMethod(globalObject, module, name, cls, string, format));
}

// ---- ISO 8601 ----

IsoText::IsoText(JSGlobalObject* globalObject, JSString* string)
    : m_string(string->value(globalObject))
{
    if (m_string.is8Bit()) {
        auto characters = m_string.span8();
        m_characters = byteCast<uint8_t>(characters.data());
        m_length = characters.size();
        return;
    }
    auto units = m_string.span16();
    for (size_t i = 0; i < units.size(); ++i) {
        char16_t unit = units[i];
        if (U16_IS_LEAD(unit) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1]))
            ++i;
        m_storage.append(unit < 0x80 ? static_cast<uint8_t>(unit) : 0xff);
    }
    m_characters = m_storage.span().data();
    m_length = m_storage.size();
}

// That many digits at `position`, which moves past them.
static bool parseIsoDigits(const IsoText& text, unsigned& position, unsigned count, int& value)
{
    int result = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned digit = static_cast<unsigned>(text.at(position + i)) - '0';
        if (digit > 9)
            return false;
        result = result * 10 + static_cast<int>(digit);
    }
    position += count;
    value = result;
    return true;
}

// YYYY-MM-DD, YYYYMMDD, YYYY-Www, YYYYWww, YYYY-Www-D or YYYYWwwD. What follows a date with a month is not looked at.
bool parseIsoDate(const IsoText& text, unsigned length, int& year, int& month, int& day)
{
    unsigned position = 0;
    if (!parseIsoDigits(text, position, 4, year))
        return false;
    bool hasHyphens = text.at(position) == '-';
    if (hasHyphens)
        ++position;
    if (text.at(position) != 'W') {
        if (!parseIsoDigits(text, position, 2, month))
            return false;
        if (hasHyphens && text.at(position++) != '-')
            return false;
        return parseIsoDigits(text, position, 2, day);
    }
    ++position;
    int week;
    int weekday = 1;
    if (!parseIsoDigits(text, position, 2, week))
        return false;
    if (position < length) {
        if (hasHyphens && text.at(position++) != '-')
            return false;
        if (!parseIsoDigits(text, position, 1, weekday))
            return false;
    }
    return fromIsoCalendar(year, week, weekday, year, month, day) == IsoCalendarError::None;
}

enum class IsoClock : uint8_t { Malformed, Ended, TextFollows };

// HH[[:]MM[[:]SS[(.|,)f+]]] from `start` up to `end`, where the text ends or an offset begins. The fields read before something
// was found wrong stay.
static IsoClock parseIsoClock(const IsoText& text, unsigned start, unsigned end, int (&fields)[4])
{
    unsigned position = start;
    bool hasColons = false;
    for (unsigned i = 0; i < 3; ++i) {
        if (!parseIsoDigits(text, position, 2, fields[i]))
            return IsoClock::Malformed;
        uint8_t next = text.at(position++);
        if (!i)
            hasColons = next == ':';
        if (next == '.' || next == ',') {
            // Only seconds have a fraction, and it has a digit.
            if (i < 2 || position >= end)
                return IsoClock::Malformed;
            break;
        }
        if (position >= end)
            return next ? IsoClock::TextFollows : IsoClock::Ended;
        if (!hasColons) {
            --position;
            continue;
        }
        if (next != ':' || i == 2)
            return IsoClock::Malformed;
    }

    // Six digits count. (Without colons, digits right after the seconds are taken for a fraction as well.)
    unsigned digits = std::min(end - position, 6u);
    if (!parseIsoDigits(text, position, digits, fields[3]))
        return IsoClock::Malformed;
    for (; digits < 6; ++digits)
        fields[3] *= 10;
    while (isASCIIDigit(text.at(position)))
        ++position;
    return text.at(position) ? IsoClock::TextFollows : IsoClock::Ended;
}

IsoTimeResult parseIsoTime(JSGlobalObject* globalObject, const IsoText& text, unsigned start, Fields& result, int& offsetSeconds, int& offsetMicroseconds)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned end = text.length();
    unsigned zone = start;
    do {
        uint8_t character = text.at(zone);
        if (character == 'Z' || character == '+' || character == '-')
            break;
    } while (++zone < end);

    int fields[4] = { 0, 0, 0, 0 };
    IsoClock clock = parseIsoClock(text, start, zone, fields);
    if (clock == IsoClock::Malformed)
        return IsoTimeResult::Malformed;
    result.hour = fields[0];
    result.minute = fields[1];
    result.second = fields[2];
    result.microsecond = fields[3];
    if (zone == end)
        return clock == IsoClock::Ended ? IsoTimeResult::Naive : IsoTimeResult::Malformed;

    offsetSeconds = 0;
    offsetMicroseconds = 0;
    if (text.at(zone) == 'Z')
        return text.at(zone + 1) ? IsoTimeResult::Malformed : IsoTimeResult::Aware;

    int sign = text.at(zone) == '-' ? -1 : 1;
    int offset[4] = { 0, 0, 0, 0 };
    clock = parseIsoClock(text, zone + 1, end, offset);
    // An hour 25 is complained of as such even if what follows it makes no sense.
    if (failed(scope, checkTimeFields(globalObject, offset[0], offset[1], offset[2], offset[3], 0)))
        return IsoTimeResult::Raised;
    if (clock != IsoClock::Ended)
        return IsoTimeResult::Malformed;
    offsetSeconds = sign * (offset[0] * 3600 + offset[1] * 60 + offset[2]);
    offsetMicroseconds = sign * offset[3];
    return IsoTimeResult::Aware;
}

JSValue tzinfoFromIsoOffset(JSGlobalObject* globalObject, State& state, IsoTimeResult result, int offsetSeconds, int offsetMicroseconds)
{
    if (result != IsoTimeResult::Aware)
        return none();
    if (!offsetSeconds && !offsetMicroseconds)
        return state.utc.get();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue offset = newDelta(globalObject, state, 0, offsetSeconds, offsetMicroseconds);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, newTimeZone(globalObject, state, PyTimeDelta::from(offset), { }));
}

JSValue raiseInvalidIsoFormat(JSGlobalObject* globalObject, JSValue string)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String shown = reprForMessage(globalObject, string);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseValueError(globalObject, scope, "Invalid isoformat string: "_s, shown);
}

} // namespace DateTime

// ---- The module ----

void initializeDateTimeModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    DateTime::State& state = DateTime::stateOf(globalObject);
    module->dict()->set(rt.vm(), rt.names.dDoc(), makeStr(rt.vm(), "Fast implementation of the datetime module."_s));
    module->add(globalObject, "MINYEAR"_s, jsNumber(DateTime::minYear));
    module->add(globalObject, "MAXYEAR"_s, jsNumber(DateTime::maxYear));
    module->add(globalObject, "date"_s, state.dateType.get());
    module->add(globalObject, "datetime"_s, state.dateTimeType.get());
    module->add(globalObject, "time"_s, state.timeType.get());
    module->add(globalObject, "timedelta"_s, state.deltaType.get());
    module->add(globalObject, "tzinfo"_s, state.tzinfoType.get());
    module->add(globalObject, "timezone"_s, state.timeZoneType.get());
    module->add(globalObject, "UTC"_s, state.utc.get());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
