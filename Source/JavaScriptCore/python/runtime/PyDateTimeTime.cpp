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

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace DateTime {

// datetime.tzinfo, datetime.timezone and datetime.time.

// ---- tzinfo ----

// Its instances, and those of classes derived from it in Python, are laid out as those of object are.
static JSValue tzinfoNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    return allocateInstance(globalObject, PyType::from(cls));
}

static JSValue tzinfoRaiseAbstract(JSGlobalObject* globalObject, ASCIILiteral method)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseNotImplementedError(globalObject, scope, "a tzinfo subclass must implement "_s, method, "()"_s);
}

static JSValue tzinfoTzName(JSGlobalObject* globalObject, JSValue, JSValue) { return tzinfoRaiseAbstract(globalObject, "tzname"_s); }
static JSValue tzinfoUtcOffset(JSGlobalObject* globalObject, JSValue, JSValue) { return tzinfoRaiseAbstract(globalObject, "utcoffset"_s); }
static JSValue tzinfoDst(JSGlobalObject* globalObject, JSValue, JSValue) { return tzinfoRaiseAbstract(globalObject, "dst"_s); }

// The argument of fromutc(). Null: an exception.
static PyDateTime* zoneFromUtcArgument(JSGlobalObject* globalObject, JSValue self, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* dateTime = PyDateTime::tryFrom(argument);
    if (!dateTime) {
        raiseTypeError(globalObject, scope, "fromutc: argument must be a datetime"_s);
        return nullptr;
    }
    if (dateTime->tzinfo() != self) {
        raiseValueError(globalObject, scope, "fromutc: dt.tzinfo is not self"_s);
        return nullptr;
    }
    return dateTime;
}

static JSValue tzinfoFromUtc(JSGlobalObject* globalObject, JSValue self, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDateTime* dateTime = zoneFromUtcArgument(globalObject, self, argument);
    RETURN_IF_EXCEPTION(scope, { });
    State& state = stateOf(globalObject, dateTime);

    JSValue offset = callUtcOffset(globalObject, state, self, dateTime);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(offset))
        return raiseValueError(globalObject, scope, "fromutc: non-None utcoffset() result required"_s);
    JSValue dst = callDst(globalObject, state, self, dateTime);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(dst))
        return raiseValueError(globalObject, scope, "fromutc: non-None dst() result required"_s);

    // First to the zone's standard time, then on by what dst() says of that.
    auto* offsetDelta = PyTimeDelta::from(offset);
    auto* dstDelta = PyTimeDelta::from(dst);
    JSValue standardOffset = newDelta(globalObject, state, offsetDelta->days() - dstDelta->days(), offsetDelta->seconds() - dstDelta->seconds(), offsetDelta->microseconds() - dstDelta->microseconds());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = addDelta(globalObject, state, dateTime, PyTimeDelta::from(standardOffset), 1);
    RETURN_IF_EXCEPTION(scope, { });
    dst = callDst(globalObject, state, self, result);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(dst))
        return raiseValueError(globalObject, scope, "fromutc: tz.dst() gave inconsistent results; cannot convert"_s);
    dstDelta = PyTimeDelta::from(dst);
    if (dstDelta->isZero())
        return result;
    // A class derived from datetime was called for it, and may have returned anything.
    auto* standard = PyDateTime::tryFrom(result);
    if (!standard)
        return raiseTypeError(globalObject, scope, "fromutc: argument must be a datetime"_s);
    RELEASE_AND_RETURN(scope, addDelta(globalObject, state, standard, dstDelta, 1));
}

static JSValue tzinfoReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue getInitArgs = getAttrIfPresent(globalObject, self, stateOf(globalObject).getInitArgsName.get());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue arguments = rt.emptyTuple();
    if (getInitArgs) {
        arguments = call(globalObject, getInitArgs);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue state = getObjectState(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { typeOf(globalObject, self), arguments, state }));
}

static constexpr NativeDef tzinfoMethods[] = {
    PY_NEW(tzinfoNew, "($type, *args, **kwargs)"),
    PY_METHOD("tzname", tzinfoTzName, "($self, object, /)"),
    PY_METHOD("utcoffset", tzinfoUtcOffset, "($self, object, /)"),
    PY_METHOD("dst", tzinfoDst, "($self, object, /)"),
    PY_METHOD("fromutc", tzinfoFromUtc, "($self, object, /)"),
    PY_METHOD("__reduce__", tzinfoReduce, "($self, /)"),
};

constexpr TypeDef tzinfoTypeDef { "datetime.tzinfo"_s, { TypeFlag::Base, TypeFlag::NothingOfItsOwn }, tzinfoMethods, { }, { } };

// ---- timezone ----

static JSValue zoneNew(JSGlobalObject* globalObject, JSValue cls, JSValue offset, JSValue name)
{
    auto* delta = PyTimeDelta::tryFrom(offset);
    if (!delta)
        return raiseBadArgument(globalObject, "timezone"_s, "argument 'offset'"_s, "datetime.timedelta"_s, offset);
    if (name && !isStr(name))
        return raiseBadArgument(globalObject, "timezone"_s, "argument 'name'"_s, "str"_s, name);
    return newTimeZone(globalObject, stateOf(globalObject, PyType::from(cls)), delta, name);
}

template<CompareOperator op>
static JSValue zoneCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto* second = PyTimeZone::tryFrom(other);
    if (!second || (op != CompareOperator::Eq && op != CompareOperator::NotEq))
        return runtime(globalObject).notImplemented();
    return compareResult(PyTimeZone::from(self)->offset()->compare(second->offset()), op);
}

static JSValue zoneHash(JSGlobalObject*, JSValue self) { return smallInt(hashOfDelta(PyTimeZone::from(self)->offset())); }

static JSValue zoneRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* zone = PyTimeZone::from(self);
    String typeName = zone->type()->nameInMessages();
    if (zone == stateOf(globalObject, zone).utc.get())
        RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString(typeName, ".utc"_s)));
    JSString* offset = repr(globalObject, zone->offset());
    RETURN_IF_EXCEPTION(scope, { });
    String offsetText = offset->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (!zone->name())
        RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString(typeName, '(', offsetText, ')')));
    JSString* name = repr(globalObject, zone->name());
    RETURN_IF_EXCEPTION(scope, { });
    String nameText = name->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString(typeName, '(', offsetText, ", "_s, nameText, ')')));
}

static JSValue zoneStr(JSGlobalObject* globalObject, JSValue self)
{
    auto* zone = PyTimeZone::from(self);
    if (JSValue name = zone->name())
        return name;
    return timeZoneName(globalObject, stateOf(globalObject, zone), zone);
}

// False: a TypeError.
static bool zoneCheckArgument(JSGlobalObject* globalObject, ASCIILiteral method, JSValue argument)
{
    if (isNone(argument) || PyDateTime::is(argument))
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, method, "(dt) argument must be a datetime instance or None, not "_s, typeName(globalObject, argument));
    return false;
}

static JSValue zoneTzName(JSGlobalObject* globalObject, JSValue self, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, zoneCheckArgument(globalObject, "tzname"_s, argument)))
        return { };
    RELEASE_AND_RETURN(scope, zoneStr(globalObject, self));
}

static JSValue zoneUtcOffset(JSGlobalObject* globalObject, JSValue self, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, zoneCheckArgument(globalObject, "utcoffset"_s, argument)))
        return { };
    return PyTimeZone::from(self)->offset();
}

static JSValue zoneDst(JSGlobalObject* globalObject, JSValue, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, zoneCheckArgument(globalObject, "dst"_s, argument)))
        return { };
    return none();
}

static JSValue zoneFromUtc(JSGlobalObject* globalObject, JSValue self, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDateTime* dateTime = zoneFromUtcArgument(globalObject, self, argument);
    if (failed(scope, dateTime))
        return { };
    RELEASE_AND_RETURN(scope, addDelta(globalObject, stateOf(globalObject, dateTime), dateTime, PyTimeZone::from(self)->offset(), 1));
}

static JSValue zoneGetInitArgs(JSGlobalObject* globalObject, JSValue self)
{
    auto* zone = PyTimeZone::from(self);
    if (JSValue name = zone->name())
        return PyTuple::create(globalObject, { zone->offset(), name });
    return PyTuple::create(globalObject, { zone->offset() });
}

static constexpr NativeDef zoneMethods[] = {
    PY_NEW(zoneNew, "($type, /, offset, name=<unrepresentable>)"),
    PY_SLOT("__repr__", zoneRepr, "($self, /)"),
    PY_SLOT("__str__", zoneStr, "($self, /)"),
    PY_SLOT("__hash__", zoneHash, "($self, /)"),
    PY_DATETIME_COMPARISONS(zoneCompare),
    PY_METHOD("tzname", zoneTzName, "($self, object, /)"),
    PY_METHOD("utcoffset", zoneUtcOffset, "($self, object, /)"),
    PY_METHOD("dst", zoneDst, "($self, object, /)"),
    PY_METHOD("fromutc", zoneFromUtc, "($self, object, /)"),
    PY_METHOD("__getinitargs__", zoneGetInitArgs, "($self, /)"),
};

constexpr TypeDef timeZoneTypeDef { "datetime.timezone"_s, { TypeFlag::OwnLayout }, zoneMethods, { }, { } };

// ---- time ----

static constexpr ASCIILiteral timeParameterNames[] = { "hour"_s, "minute"_s, "second"_s, "microsecond"_s, "tzinfo"_s, "fold"_s };
static constexpr Parameters timeParameters { "time"_s, timeParameterNames, 0, 5 };

static JSValue timeOfDayNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    State& state = stateOf(globalObject, type);
    if (args.size() >= 1 && args.size() <= 2 && !args[0].isInt32()) [[unlikely]] {
        uint8_t bytes[6];
        PickledState pickled = pickledStateArgument(globalObject, args[0], "time"_s, 6, 0, 0x7f, 0, 24, bytes);
        RETURN_IF_EXCEPTION(scope, { });
        if (pickled == PickledState::Yes) {
            JSValue tzinfo = args.size() == 2 ? args[1] : none();
            if (!isNone(tzinfo) && !isTzInfo(globalObject, state, tzinfo))
                return raiseTypeError(globalObject, scope, "bad tzinfo state arg"_s);
            return PyTimeOfDay::create(vm, state.timeStructure.get(), type, Packed::time(bytes[0] & 0x7f, bytes[1], bytes[2], bytes[3] << 16 | bytes[4] << 8 | bytes[5], bytes[0] >> 7), tzinfo);
        }
    }
    JSValue values[6];
    if (failed(scope, bindArguments(globalObject, timeParameters, args, values)))
        return { };
    int numbers[4] = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < 4; ++i) {
        if (values[i] && failed(scope, intArgument(globalObject, values[i], numbers[i])))
            return { };
    }
    int fold = 0;
    if (values[5] && failed(scope, intArgument(globalObject, values[5], fold)))
        return { };
    RELEASE_AND_RETURN(scope, newTime(globalObject, state, type, numbers[0], numbers[1], numbers[2], numbers[3], values[4] ? values[4] : none(), fold));
}

static JSValue timeOfDayStrptime(JSGlobalObject* globalObject, JSValue cls, JSValue string, JSValue format)
{
    return callStrptime(globalObject, stateOf(globalObject, PyType::from(cls)).strptimeTimeName.get(), cls, string, format);
}

static JSValue timeOfDayFromIsoFormat(JSGlobalObject* globalObject, JSValue cls, JSValue string)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    State& state = stateOf(globalObject, type);
    JSString* given = strArgument(globalObject, "fromisoformat"_s, "argument"_s, string);
    RETURN_IF_EXCEPTION(scope, { });
    IsoText text(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });

    Fields fields;
    int offsetSeconds = 0;
    int offsetMicroseconds = 0;
    IsoTimeResult result = parseIsoTime(globalObject, text, text.at(0) == 'T', fields, offsetSeconds, offsetMicroseconds);
    RETURN_IF_EXCEPTION(scope, { });
    if (result == IsoTimeResult::Malformed)
        RELEASE_AND_RETURN(scope, raiseInvalidIsoFormat(globalObject, string));
    if (fields.hour == 24) {
        if (fields.minute || fields.second || fields.microsecond)
            return raiseValueError(globalObject, scope, "minute, second, and microsecond must be 0 when hour is 24"_s);
        fields.hour = 0;
    }
    JSValue tzinfo = tzinfoFromIsoOffset(globalObject, state, result, offsetSeconds, offsetMicroseconds);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeTime(globalObject, state, type, fields.hour, fields.minute, fields.second, fields.microsecond, tzinfo, 0));
}

static JSValue timeOfDayUtcOffset(JSGlobalObject* globalObject, JSValue self)
{
    auto* time = PyTimeOfDay::from(self);
    return callUtcOffset(globalObject, stateOf(globalObject, time), time->tzinfo(), none());
}

static JSValue timeOfDayDst(JSGlobalObject* globalObject, JSValue self)
{
    auto* time = PyTimeOfDay::from(self);
    return callDst(globalObject, stateOf(globalObject, time), time->tzinfo(), none());
}

static JSValue timeOfDayTzName(JSGlobalObject* globalObject, JSValue self)
{
    auto* time = PyTimeOfDay::from(self);
    return callTzName(globalObject, stateOf(globalObject, time), time->tzinfo(), none());
}

static JSValue timeOfDayRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* time = PyTimeOfDay::from(self);
    StrBuilder builder;
    builder.append(time->type()->nameInMessages(), '(', time->hour(), ", "_s, time->minute());
    if (time->second() || time->microsecond())
        builder.append(", "_s, time->second());
    if (time->microsecond())
        builder.append(", "_s, time->microsecond());
    if (!isNone(time->tzinfo())) {
        JSString* tzinfo = repr(globalObject, time->tzinfo());
        RETURN_IF_EXCEPTION(scope, { });
        String tzinfoText = tzinfo->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(", tzinfo="_s, tzinfoText);
    }
    if (time->fold())
        builder.append(", fold=1"_s);
    builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue timeOfDayStr(JSGlobalObject* globalObject, JSValue self)
{
    return callMethod(globalObject, self, stateOf(globalObject, PyTimeOfDay::from(self)).isoformatName.get());
}

static JSValue timeOfDayIsoFormat(JSGlobalObject* globalObject, JSValue self, JSValue timespec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* time = PyTimeOfDay::from(self);
    TimeSpec spec;
    if (failed(scope, timeSpecArgument(globalObject, timespec, time->microsecond(), spec)))
        return { };
    Text text;
    appendIsoTime(text, time->packed(), spec);
    if (!isNone(time->tzinfo()) && failed(scope, appendUtcOffset(globalObject, stateOf(globalObject, time), text, ":"_s, time->tzinfo(), none())))
        return { };
    RELEASE_AND_RETURN(scope, makeStr(getVM(globalObject), text.toString()));
}

static JSValue timeOfDayStrftime(JSGlobalObject* globalObject, JSValue self, JSValue format)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* time = PyTimeOfDay::from(self);
    JSString* formatString = strArgument(globalObject, "strftime"_s, "argument 'format'"_s, format);
    RETURN_IF_EXCEPTION(scope, { });
    // The date is one that strftime() takes everywhere.
    PyTuple* timeTuple = PyTuple::create(globalObject, {
        jsNumber(1900), jsNumber(1), jsNumber(1), jsNumber(time->hour()), jsNumber(time->minute()), jsNumber(time->second()),
        jsNumber(0), jsNumber(1), jsNumber(-1) });
    RELEASE_AND_RETURN(scope, wrapStrftime(globalObject, stateOf(globalObject, time), self, formatString, timeTuple, none()));
}

static JSValue timeOfDayFormat(JSGlobalObject* globalObject, JSValue self, JSValue format)
{
    return formatWithStrftime(globalObject, stateOf(globalObject, PyTimeOfDay::from(self)), self, format);
}

static ALWAYS_INLINE int timeOfDaySeconds(PyTimeOfDay* time) { return time->hour() * 3600 + time->minute() * 60 + time->second(); }

template<CompareOperator op>
static JSValue timeOfDayCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto* second = PyTimeOfDay::tryFrom(other);
    if (!second)
        return runtime(globalObject).notImplemented();
    auto* first = PyTimeOfDay::from(self);
    if (first->tzinfo() == second->tzinfo()) [[likely]]
        return compareResult(Packed::compare(first->packed(), second->packed()), op);

    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject, first);
    JSValue firstOffset = callUtcOffset(globalObject, state, first->tzinfo(), none());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue secondOffset = callUtcOffset(globalObject, state, second->tzinfo(), none());
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(firstOffset) && isNone(secondOffset))
        return compareResult(Packed::compare(first->packed(), second->packed()), op);
    if (!isNone(firstOffset) && !isNone(secondOffset)) {
        auto* a = PyTimeDelta::from(firstOffset);
        auto* b = PyTimeDelta::from(secondOffset);
        if (!a->compare(b))
            return compareResult(Packed::compare(first->packed(), second->packed()), op);
        // The microseconds of the offsets do not count, as in CPython.
        int difference = (timeOfDaySeconds(first) - a->days() * secondsPerDay - a->seconds()) - (timeOfDaySeconds(second) - b->days() * secondsPerDay - b->seconds());
        if (!difference)
            difference = first->microsecond() - second->microsecond();
        return compareResult(difference, op);
    }
    if (op == CompareOperator::Eq)
        return jsBoolean(false);
    if (op == CompareOperator::NotEq)
        return jsBoolean(true);
    return raiseTypeError(globalObject, scope, "can't compare offset-naive and offset-aware times"_s);
}

static JSValue timeOfDayHash(JSGlobalObject* globalObject, JSValue self)
{
    auto* time = PyTimeOfDay::from(self);
    if (time->cachedHash == -1) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue offset = callUtcOffset(globalObject, stateOf(globalObject, time), time->tzinfo(), none());
        RETURN_IF_EXCEPTION(scope, { });
        if (isNone(offset))
            time->cachedHash = mixHash(time->packed() >> 1);
        else {
            int64_t total = static_cast<int64_t>(timeOfDaySeconds(time)) * microsecondsPerSecond + time->microsecond() - static_cast<int64_t>(PyTimeDelta::from(offset)->total());
            int64_t microseconds;
            int64_t seconds;
            int64_t days = floorDivide<int64_t>(floorDivide<int64_t>(total, microsecondsPerSecond, microseconds), secondsPerDay, seconds);
            time->cachedHash = hashOfDelta(days, static_cast<int>(seconds), static_cast<int>(microseconds));
        }
    }
    return smallInt(time->cachedHash);
}

static constexpr Parameters timeReplaceParameters { "replace"_s, timeParameterNames, 0, 5 };

// Also __replace__(), which complains under the same name.
static JSValue timeOfDayReplace(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* time = PyTimeOfDay::from(self);
    int numbers[] = { time->hour(), time->minute(), time->second(), time->microsecond() };
    JSValue values[6];
    if (failed(scope, bindArguments(globalObject, timeReplaceParameters, args, values)))
        return { };
    for (unsigned i = 0; i < 4; ++i) {
        if (values[i] && failed(scope, intArgument(globalObject, values[i], numbers[i])))
            return { };
    }
    int fold = time->fold();
    if (values[5] && failed(scope, intArgument(globalObject, values[5], fold)))
        return { };
    RELEASE_AND_RETURN(scope, makeTime(globalObject, stateOf(globalObject, time), time->type(), numbers[0], numbers[1], numbers[2], numbers[3], values[4] ? values[4] : time->tzinfo(), fold));
}

static JSValue timeOfDayReduceFor(JSGlobalObject* globalObject, PyTimeOfDay* time, int protocol)
{
    uint8_t bytes[] = {
        static_cast<uint8_t>(time->hour() | (protocol > 3 && time->fold() ? 0x80 : 0)), static_cast<uint8_t>(time->minute()), static_cast<uint8_t>(time->second()),
        static_cast<uint8_t>(time->microsecond() >> 16), static_cast<uint8_t>(time->microsecond() >> 8), static_cast<uint8_t>(time->microsecond())
    };
    PyBytes* base = PyBytes::create(globalObject, std::span<const uint8_t> { bytes });
    PyTuple* state = isNone(time->tzinfo()) ? PyTuple::create(globalObject, { base }) : PyTuple::create(globalObject, { base, time->tzinfo() });
    return PyTuple::create(globalObject, { time->type(), state });
}

static JSValue timeOfDayReduceEx(JSGlobalObject* globalObject, JSValue self, JSValue protocolValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int protocol;
    if (failed(scope, intArgument(globalObject, protocolValue, protocol)))
        return { };
    RELEASE_AND_RETURN(scope, timeOfDayReduceFor(globalObject, PyTimeOfDay::from(self), protocol));
}

static JSValue timeOfDayReduce(JSGlobalObject* globalObject, JSValue self) { return timeOfDayReduceFor(globalObject, PyTimeOfDay::from(self), 2); }

static JSValue timeOfDayGetHour(JSGlobalObject*, JSValue self) { return jsNumber(PyTimeOfDay::from(self)->hour()); }
static JSValue timeOfDayGetMinute(JSGlobalObject*, JSValue self) { return jsNumber(PyTimeOfDay::from(self)->minute()); }
static JSValue timeOfDayGetSecond(JSGlobalObject*, JSValue self) { return jsNumber(PyTimeOfDay::from(self)->second()); }
static JSValue timeOfDayGetMicrosecond(JSGlobalObject*, JSValue self) { return jsNumber(PyTimeOfDay::from(self)->microsecond()); }
static JSValue timeOfDayGetTzInfo(JSGlobalObject*, JSValue self) { return PyTimeOfDay::from(self)->tzinfo(); }
static JSValue timeOfDayGetFold(JSGlobalObject*, JSValue self) { return jsNumber(PyTimeOfDay::from(self)->fold()); }

static constexpr NativeDef timeOfDayMethods[] = {
    PY_NEW(timeOfDayNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", timeOfDayRepr, "($self, /)"),
    PY_SLOT("__str__", timeOfDayStr, "($self, /)"),
    PY_SLOT("__hash__", timeOfDayHash, "($self, /)"),
    PY_DATETIME_COMPARISONS(timeOfDayCompare),
    PY_CLASSMETHOD("fromisoformat", timeOfDayFromIsoFormat, "($type, string, /)"),
    PY_CLASSMETHOD("strptime", timeOfDayStrptime, "($type, string, format, /)"),
    PY_METHOD("isoformat", timeOfDayIsoFormat, "($self, /, timespec='auto')"),
    PY_METHOD("strftime", timeOfDayStrftime, "($self, /, format)"),
    PY_METHOD("__format__", timeOfDayFormat, "($self, format, /)"),
    PY_METHOD("utcoffset", timeOfDayUtcOffset, "($self, /)"),
    PY_METHOD("tzname", timeOfDayTzName, "($self, /)"),
    PY_METHOD("dst", timeOfDayDst, "($self, /)"),
    PY_METHOD("replace", timeOfDayReplace, "!($self, /, hour=unchanged, minute=unchanged, second=unchanged,\n        microsecond=unchanged, tzinfo=unchanged, *, fold=unchanged)"),
    PY_METHOD("__replace__", timeOfDayReplace, "!($self, /, **changes)"),
    PY_METHOD("__reduce_ex__", timeOfDayReduceEx, "($self, proto, /)"),
    PY_METHOD("__reduce__", timeOfDayReduce, "($self, /)"),
};

static constexpr NativeGetSet timeOfDayGetSets[] = {
    { "hour"_s, timeOfDayGetHour },
    { "minute"_s, timeOfDayGetMinute },
    { "second"_s, timeOfDayGetSecond },
    { "microsecond"_s, timeOfDayGetMicrosecond },
    { "tzinfo"_s, timeOfDayGetTzInfo },
    { "fold"_s, timeOfDayGetFold },
};

constexpr TypeDef timeTypeDef {
    "datetime.time"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, timeOfDayMethods, timeOfDayGetSets, { }
};

} // namespace DateTime
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
