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
#include <cmath>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace DateTime {

// datetime.datetime

static constexpr ASCIILiteral dateTimeParameterNames[] = { "year"_s, "month"_s, "day"_s, "hour"_s, "minute"_s, "second"_s, "microsecond"_s, "tzinfo"_s, "fold"_s };
static constexpr Parameters dateTimeParameters { "datetime"_s, dateTimeParameterNames, 3, 8 };
static constexpr Parameters dateTimeReplaceParameters { "replace"_s, dateTimeParameterNames, 0, 8 };

// What was given replaces what `fields` and `tzinfo` have. False: an exception.
static bool dateTimeTakeArguments(JSGlobalObject* globalObject, const JSValue* values, Fields& fields, JSValue& tzinfo)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int* numbers[] = { &fields.year, &fields.month, &fields.day, &fields.hour, &fields.minute, &fields.second, &fields.microsecond };
    for (unsigned i = 0; i < 7; ++i) {
        if (values[i] && failed(scope, intArgument(globalObject, values[i], *numbers[i])))
            return false;
    }
    if (values[7])
        tzinfo = values[7];
    RELEASE_AND_RETURN(scope, !values[8] || intArgument(globalObject, values[8], fields.fold));
}

static JSValue dateTimeNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    State& state = stateOf(globalObject, type);
    if (args.size() >= 1 && args.size() <= 2) [[unlikely]] {
        uint8_t bytes[10];
        PickledState pickled = pickledStateArgument(globalObject, args[0], "datetime"_s, 10, 2, 0x7f, 1, 13, bytes);
        RETURN_IF_EXCEPTION(scope, { });
        if (pickled == PickledState::Yes) {
            JSValue tzinfo = args.size() == 2 ? args[1] : none();
            if (!isNone(tzinfo) && !isTzInfo(globalObject, state, tzinfo))
                return raiseTypeError(globalObject, scope, "bad tzinfo state arg"_s);
            uint64_t packed = Packed::date(bytes[0] << 8 | bytes[1], bytes[2] & 0x7f, bytes[3])
                | Packed::time(bytes[4], bytes[5], bytes[6], bytes[7] << 16 | bytes[8] << 8 | bytes[9], bytes[2] >> 7);
            return PyDateTime::create(vm, state.dateTimeStructure.get(), type, packed, tzinfo);
        }
    }
    JSValue values[9];
    Fields fields;
    JSValue tzinfo = none();
    if (failed(scope, bindArguments(globalObject, dateTimeParameters, args, values)) || failed(scope, dateTimeTakeArguments(globalObject, values, fields, tzinfo)))
        return { };
    RELEASE_AND_RETURN(scope, newDateTime(globalObject, state, type, fields, tzinfo));
}

// ---- Local time ----

static constexpr int64_t maxFoldSeconds = 24 * 3600; // No zone has ever moved its clocks by more.
static constexpr int64_t epochSeconds = epochOrdinal * secondsPerDay;

// Seconds are counted from the day before 0001-01-01, as ordinals are.
static ALWAYS_INLINE int64_t dateTimeSeconds(const Fields& fields)
{
    return ((static_cast<int64_t>(toOrdinal(fields.year, fields.month, fields.day)) * 24 + fields.hour) * 60 + fields.minute) * 60 + fields.second;
}

// What the local clock shows at the moment `utc`. False: an exception.
static bool dateTimeLocalSeconds(JSGlobalObject* globalObject, int64_t utc, int64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct tm tm;
    if (failed(scope, toLocalTime(globalObject, utc - epochSeconds, tm)))
        return false;
    int64_t year = static_cast<int64_t>(tm.tm_year) + 1900;
    if (year < minYear || year > maxYear) {
        raiseValueError(globalObject, scope, "year must be in "_s, minYear, ".."_s, maxYear, ", not "_s, year);
        return false;
    }
    result = dateTimeSeconds({ static_cast<int>(year), tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec });
    return true;
}

// The moment at which the local clock shows that, the first or the second of two according to the fold (PEP 495).
static bool dateTimeLocalToUtc(JSGlobalObject* globalObject, const Fields& fields, int fold, int64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t wanted = dateTimeSeconds(fields);
    int64_t shown;
    if (failed(scope, dateTimeLocalSeconds(globalObject, wanted, shown)))
        return false;
    int64_t firstOffset = shown - wanted;
    int64_t first = wanted - firstOffset;
    int64_t shownAtFirst;
    if (failed(scope, dateTimeLocalSeconds(globalObject, first, shownAtFirst)))
        return false;
    int64_t secondOffset;
    if (shownAtFirst == wanted) {
        // A solution. There may be another, earlier or later, and it may be the one asked for.
        int64_t probe = first + (fold ? maxFoldSeconds : -maxFoldSeconds);
        if (failed(scope, dateTimeLocalSeconds(globalObject, probe, shown)))
            return false;
        secondOffset = shown - probe;
        if (secondOffset == firstOffset) {
            result = first;
            return true;
        }
    } else
        secondOffset = shownAtFirst - first;
    int64_t second = wanted - secondOffset;
    int64_t shownAtSecond;
    if (failed(scope, dateTimeLocalSeconds(globalObject, second, shownAtSecond)))
        return false;
    if (shownAtSecond == wanted)
        result = second;
    else if (shownAtFirst == wanted)
        result = first;
    else // The clock never shows it: it is in a gap.
        result = fold ? std::min(first, second) : std::max(first, second);
    return true;
}

static JSValue dateTimeFromSeconds(JSGlobalObject* globalObject, State& state, PyType* cls, bool isLocal, time_t when, int microsecond, JSValue tzinfo)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct tm tm;
    if (failed(scope, isLocal ? toLocalTime(globalObject, when, tm) : toUniversalTime(globalObject, when, tm)))
        return { };
    int64_t year = static_cast<int64_t>(tm.tm_year) + 1900;
    if (year < minYear || year > maxYear)
        return raiseValueError(globalObject, scope, "year must be in "_s, minYear, ".."_s, maxYear, ", not "_s, year);
    // A leap second is not told from the second before it.
    Fields fields { static_cast<int>(year), tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, std::min(59, tm.tm_sec), microsecond, 0 };
    if (isLocal && isNone(tzinfo)) {
        // Has the clock shown this before?
        int64_t shown = dateTimeSeconds(fields);
        int64_t probe;
        if (failed(scope, dateTimeLocalSeconds(globalObject, epochSeconds + when - maxFoldSeconds, probe)))
            return { };
        int64_t transition = shown - probe - maxFoldSeconds;
        if (transition < 0) {
            if (failed(scope, dateTimeLocalSeconds(globalObject, epochSeconds + when + transition, probe)))
                return { };
            fields.fold = probe == shown;
        }
    }
    RELEASE_AND_RETURN(scope, makeDateTime(globalObject, state, cls, fields, tzinfo));
}

// A timestamp, its microseconds rounded half to even. False: an exception.
static bool dateTimeTimestampArgument(JSGlobalObject* globalObject, JSValue value, time_t& seconds, int& microseconds)
{
    microseconds = 0;
    if (hasIndex(globalObject, value))
        return secondsArgument(globalObject, value, seconds);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double number = toDouble(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (std::isnan(number)) {
        raiseValueError(globalObject, scope, "Invalid value NaN (not a number)"_s);
        return false;
    }
    double whole;
    double fraction = std::modf(number, &whole) * microsecondsPerSecond;
    double rounded = std::round(fraction);
    if (std::fabs(fraction - rounded) == 0.5)
        rounded = 2.0 * std::round(fraction / 2.0);
    if (rounded >= microsecondsPerSecond) {
        rounded -= microsecondsPerSecond;
        whole += 1.0;
    } else if (rounded < 0) {
        rounded += microsecondsPerSecond;
        whole -= 1.0;
    }
    if (!(whole >= -0x1p63 && whole < 0x1p63)) {
        raiseOverflowError(globalObject, scope, "timestamp out of range for platform time_t"_s);
        return false;
    }
    seconds = static_cast<time_t>(whole);
    microseconds = static_cast<int>(rounded);
    return true;
}

static JSValue dateTimeCurrent(JSGlobalObject* globalObject, State& state, PyType* cls, bool isLocal, JSValue tzinfo)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    time_t seconds = now.tv_sec;
    int microseconds = static_cast<int>(now.tv_nsec / 1000);
    int rest = static_cast<int>(now.tv_nsec % 1000);
    if (rest > 500 || (rest == 500 && (microseconds & 1)))
        ++microseconds;
    if (microseconds == microsecondsPerSecond) {
        microseconds = 0;
        ++seconds;
    }
    return dateTimeFromSeconds(globalObject, state, cls, isLocal, seconds, microseconds, tzinfo);
}

// ---- Class methods ----

static JSValue dateTimeNow(JSGlobalObject* globalObject, JSValue cls, JSValue tz)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    State& state = stateOf(globalObject, type);
    if (!tz)
        tz = none();
    if (failed(scope, checkTzInfo(globalObject, state, tz)))
        return { };
    JSValue result = dateTimeCurrent(globalObject, state, type, isNone(tz), tz);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(tz))
        return result;
    RELEASE_AND_RETURN(scope, callMethod(globalObject, tz, state.fromutcName.get(), result));
}

static JSValue dateTimeUtcNow(JSGlobalObject* globalObject, JSValue cls)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    if (failed(scope, warn(globalObject, runtime(globalObject).DeprecationWarning(), "datetime.datetime.utcnow() is deprecated and scheduled for removal in a future version. Use timezone-aware objects to represent datetimes in UTC: datetime.datetime.now(datetime.UTC)."_s)))
        return { };
    RELEASE_AND_RETURN(scope, dateTimeCurrent(globalObject, stateOf(globalObject, type), type, false, none()));
}

static JSValue dateTimeFromTimestamp(JSGlobalObject* globalObject, JSValue cls, JSValue timestamp, JSValue tz)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    State& state = stateOf(globalObject, type);
    if (!tz)
        tz = none();
    time_t seconds;
    int microseconds;
    if (failed(scope, checkTzInfo(globalObject, state, tz)) || failed(scope, dateTimeTimestampArgument(globalObject, timestamp, seconds, microseconds)))
        return { };
    JSValue result = dateTimeFromSeconds(globalObject, state, type, isNone(tz), seconds, microseconds, tz);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(tz))
        return result;
    RELEASE_AND_RETURN(scope, callMethod(globalObject, tz, state.fromutcName.get(), result));
}

static JSValue dateTimeUtcFromTimestamp(JSGlobalObject* globalObject, JSValue cls, JSValue timestamp)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    if (failed(scope, warn(globalObject, runtime(globalObject).DeprecationWarning(), "datetime.datetime.utcfromtimestamp() is deprecated and scheduled for removal in a future version. Use timezone-aware objects to represent datetimes in UTC: datetime.datetime.fromtimestamp(timestamp, datetime.UTC)."_s)))
        return { };
    time_t seconds;
    int microseconds;
    if (failed(scope, dateTimeTimestampArgument(globalObject, timestamp, seconds, microseconds)))
        return { };
    RELEASE_AND_RETURN(scope, dateTimeFromSeconds(globalObject, stateOf(globalObject, type), type, false, seconds, microseconds, none()));
}

static JSValue dateTimeStrptime(JSGlobalObject* globalObject, JSValue cls, JSValue string, JSValue format)
{
    return callStrptime(globalObject, stateOf(globalObject, PyType::from(cls)).strptimeDateTimeName.get(), cls, string, format);
}

static JSValue dateTimeCombine(JSGlobalObject* globalObject, JSValue cls, JSValue dateValue, JSValue timeValue, JSValue tzinfo)
{
    PyType* type = PyType::from(cls);
    auto* date = PyDate::tryFrom(dateValue);
    if (!date)
        return raiseBadArgument(globalObject, "combine"_s, "argument 'date'"_s, "datetime.date"_s, dateValue);
    auto* time = PyTimeOfDay::tryFrom(timeValue);
    if (!time)
        return raiseBadArgument(globalObject, "combine"_s, "argument 'time'"_s, "datetime.time"_s, timeValue);
    Fields fields { date->year(), date->month(), date->day(), time->hour(), time->minute(), time->second(), time->microsecond(), time->fold() };
    return makeDateTime(globalObject, stateOf(globalObject, type), type, fields, tzinfo ? tzinfo : time->tzinfo());
}

// Where the date ends, which the form of the date tells. Any one character may follow it. Negative: nowhere.
static int dateTimeFindIsoSeparator(const IsoText& text)
{
    unsigned length = text.length();
    if (length == 7)
        return 7;
    if (text.at(4) == '-') {
        if (text.at(5) != 'W')
            return 10; // YYYY-MM-DD
        if (length > 8 && text.at(8) == '-') {
            if (length == 9)
                return -1;
            // YYYY-Www-D, or YYYY-Www with a hyphen for a separator: the likelier if a digit is where the separator would be.
            return length > 10 && isASCIIDigit(text.at(10)) ? 8 : 10;
        }
        return 8; // YYYY-Www
    }
    if (text.at(4) != 'W')
        return 8; // YYYYMMDD
    // YYYYWww or YYYYWwwD. The fields of a time have two digits each.
    unsigned index = 7;
    while (index < length && isASCIIDigit(text.at(index)))
        ++index;
    if (index < 9)
        return static_cast<int>(index);
    return index % 2 ? 8 : 7;
}

static JSValue dateTimeFromIsoFormat(JSGlobalObject* globalObject, JSValue cls, JSValue string)
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
    int separator = text.length() < 7 ? -1 : dateTimeFindIsoSeparator(text);
    if (separator < 0 || !parseIsoDate(text, separator, fields.year, fields.month, fields.day))
        RELEASE_AND_RETURN(scope, raiseInvalidIsoFormat(globalObject, string));
    IsoTimeResult result = IsoTimeResult::Naive;
    int offsetSeconds = 0;
    int offsetMicroseconds = 0;
    if (text.length() > static_cast<unsigned>(separator)) {
        result = parseIsoTime(globalObject, text, separator + 1, fields, offsetSeconds, offsetMicroseconds);
        RETURN_IF_EXCEPTION(scope, { });
        if (result == IsoTimeResult::Malformed)
            RELEASE_AND_RETURN(scope, raiseInvalidIsoFormat(globalObject, string));
    }
    JSValue tzinfo = tzinfoFromIsoOffset(globalObject, state, result, offsetSeconds, offsetMicroseconds);
    RETURN_IF_EXCEPTION(scope, { });

    // 24:00 is midnight of the next day.
    if (fields.hour == 24 && fields.month >= 1 && fields.month <= 12 && fields.day <= daysInMonth(fields.year, fields.month)) {
        if (fields.minute || fields.second || fields.microsecond)
            return raiseValueError(globalObject, scope, "minute, second, and microsecond must be 0 when hour is 24"_s);
        fields.hour = 0;
        if (++fields.day > daysInMonth(fields.year, fields.month)) {
            fields.day = 1;
            if (++fields.month > 12) {
                fields.month = 1;
                ++fields.year;
            }
        }
    }
    RELEASE_AND_RETURN(scope, makeDateTime(globalObject, state, type, fields, tzinfo));
}

// ---- Arithmetic ----

JSValue addDelta(JSGlobalObject* globalObject, State& state, PyDateTime* dateTime, PyTimeDelta* delta, int factor)
{
    Fields fields = dateTime->fields();
    int64_t microseconds = fields.microsecond + static_cast<int64_t>(factor) * delta->microseconds();
    int64_t seconds = fields.hour * 3600 + fields.minute * 60 + fields.second + static_cast<int64_t>(factor) * delta->seconds();
    int64_t days = static_cast<int64_t>(factor) * delta->days();
    seconds += floorDivide<int64_t>(microseconds, microsecondsPerSecond, microseconds);
    days += floorDivide<int64_t>(seconds, secondsPerDay, seconds);
    if (days) {
        int64_t ordinal = toOrdinal(dateTime->packed()) + days;
        if (ordinal < 1 || ordinal > maxOrdinal) {
            VM& vm = getVM(globalObject);
            auto scope = DECLARE_THROW_SCOPE(vm);
            return raiseOverflowError(globalObject, scope, "date value out of range"_s);
        }
        fromOrdinal(static_cast<int>(ordinal), fields.year, fields.month, fields.day);
    }
    fields.hour = static_cast<int>(seconds / 3600);
    fields.minute = static_cast<int>(seconds / 60 % 60);
    fields.second = static_cast<int>(seconds % 60);
    fields.microsecond = static_cast<int>(microseconds);
    fields.fold = 0;
    return makeDateTime(globalObject, state, dateTime->type(), fields, dateTime->tzinfo());
}

JSValue subtractDateTimes(JSGlobalObject* globalObject, State& state, PyDateTime* left, PyDateTime* right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t days = toOrdinal(left->packed()) - toOrdinal(right->packed());
    int64_t seconds = (left->hour() - right->hour()) * 3600 + (left->minute() - right->minute()) * 60 + (left->second() - right->second());
    int64_t microseconds = left->microsecond() - right->microsecond();
    if (left->tzinfo() != right->tzinfo()) {
        JSValue leftOffset = callUtcOffset(globalObject, state, left->tzinfo(), left);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue rightOffset = callUtcOffset(globalObject, state, right->tzinfo(), right);
        RETURN_IF_EXCEPTION(scope, { });
        if (isNone(leftOffset) != isNone(rightOffset))
            return raiseTypeError(globalObject, scope, "can't subtract offset-naive and offset-aware datetimes"_s);
        if (!isNone(leftOffset)) {
            auto* a = PyTimeDelta::from(leftOffset);
            auto* b = PyTimeDelta::from(rightOffset);
            days -= a->days() - b->days();
            seconds -= a->seconds() - b->seconds();
            microseconds -= a->microseconds() - b->microseconds();
        }
    }
    RELEASE_AND_RETURN(scope, newDelta(globalObject, state, days, seconds, microseconds));
}

// The operands are in the order in which they were written.
static JSValue dateTimeAdd(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    if (auto* dateTime = PyDateTime::tryFrom(left)) {
        if (auto* delta = PyTimeDelta::tryFrom(right))
            return addDelta(globalObject, stateOf(globalObject, dateTime), dateTime, delta, 1);
    } else if (auto* delta = PyTimeDelta::tryFrom(left)) {
        auto* dateTime = PyDateTime::from(right);
        return addDelta(globalObject, stateOf(globalObject, dateTime), dateTime, delta, 1);
    }
    return runtime(globalObject).notImplemented();
}

static JSValue dateTimeSubtract(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    if (auto* dateTime = PyDateTime::tryFrom(left)) {
        if (auto* other = PyDateTime::tryFrom(right))
            return subtractDateTimes(globalObject, stateOf(globalObject, dateTime), dateTime, other);
        if (auto* delta = PyTimeDelta::tryFrom(right))
            return addDelta(globalObject, stateOf(globalObject, dateTime), dateTime, delta, -1);
    }
    return runtime(globalObject).notImplemented();
}

template<JSValue (*operation)(JSGlobalObject*, JSValue, JSValue)>
static JSValue dateTimeReflected(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    return operation(globalObject, other, self);
}

// ---- Comparison ----

static bool dateTimeOffsetsDiffer(JSValue a, JSValue b)
{
    if (isNone(a) || isNone(b))
        return isNone(a) != isNone(b);
    return PyTimeDelta::from(a)->compare(PyTimeDelta::from(b));
}

// PEP 495: a datetime whose offset depends on its fold, one in a fold or a gap, is equal to none in another zone.
static bool dateTimeOffsetDependsOnFold(JSGlobalObject* globalObject, State& state, PyDateTime* dateTime, JSValue offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* flipped = PyDateTime::create(vm, state.dateTimeStructure.get(), dateTime->type(), dateTime->packed() ^ 1, dateTime->tzinfo());
    JSValue flippedOffset = callUtcOffset(globalObject, state, flipped->tzinfo(), flipped);
    RETURN_IF_EXCEPTION(scope, false);
    return dateTimeOffsetsDiffer(flippedOffset, offset);
}

template<CompareOperator op>
static JSValue dateTimeCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto* second = PyDateTime::tryFrom(other);
    if (!second)
        return runtime(globalObject).notImplemented();
    auto* first = PyDateTime::from(self);
    if (first->tzinfo() == second->tzinfo()) [[likely]]
        return compareResult(Packed::compare(first->packed(), second->packed()), op);

    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject, first);
    JSValue firstOffset = callUtcOffset(globalObject, state, first->tzinfo(), first);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue secondOffset = callUtcOffset(globalObject, state, second->tzinfo(), second);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(firstOffset) != isNone(secondOffset)) {
        if (op == CompareOperator::Eq)
            return jsBoolean(false);
        if (op == CompareOperator::NotEq)
            return jsBoolean(true);
        return raiseTypeError(globalObject, scope, "can't compare offset-naive and offset-aware datetimes"_s);
    }
    int difference;
    if (!dateTimeOffsetsDiffer(firstOffset, secondOffset))
        difference = Packed::compare(first->packed(), second->packed());
    else {
        JSValue delta = subtractDateTimes(globalObject, state, first, second);
        RETURN_IF_EXCEPTION(scope, { });
        auto* between = PyTimeDelta::from(delta);
        difference = between->days() ? between->days() : between->seconds() | between->microseconds();
    }
    if ((op == CompareOperator::Eq || op == CompareOperator::NotEq) && !difference) {
        bool isException = dateTimeOffsetDependsOnFold(globalObject, state, first, firstOffset);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isException) {
            isException = dateTimeOffsetDependsOnFold(globalObject, state, second, secondOffset);
            RETURN_IF_EXCEPTION(scope, { });
        }
        difference = isException;
    }
    return compareResult(difference, op);
}

static int64_t dateTimeHash(JSGlobalObject* globalObject, JSValue self)
{
    auto* dateTime = PyDateTime::from(self);
    if (dateTime->cachedHash == -1) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue offset = none();
        if (!dateTime->isNaive()) {
            State& state = stateOf(globalObject, dateTime);
            // The fold does not count, so neither does what the offset would be with it.
            PyDateTime* unfolded = dateTime->fold() ? PyDateTime::create(vm, state.dateTimeStructure.get(), dateTime->type(), dateTime->packed() & ~1ull, dateTime->tzinfo()) : dateTime;
            offset = callUtcOffset(globalObject, state, dateTime->tzinfo(), unfolded);
            RETURN_IF_EXCEPTION(scope, -1);
        }
        if (isNone(offset)) {
            uint8_t bytes[10];
            writeDateBytes(dateTime->packed(), bytes);
            writeTimeBytes(dateTime->packed(), bytes + 4);
            dateTime->cachedHash = hashOfBytes(bytes);
        } else {
            auto* delta = PyTimeDelta::from(offset);
            int64_t microseconds = dateTime->microsecond() - delta->microseconds();
            int64_t seconds = dateTime->hour() * 3600 + dateTime->minute() * 60 + dateTime->second() - delta->seconds();
            int64_t days = toOrdinal(dateTime->packed()) - delta->days();
            seconds += floorDivide<int64_t>(microseconds, microsecondsPerSecond, microseconds);
            days += floorDivide<int64_t>(seconds, secondsPerDay, seconds);
            dateTime->cachedHash = hashOfDelta(days, static_cast<int>(seconds), static_cast<int>(microseconds));
        }
    }
    return dateTime->cachedHash;
}

// ---- Text ----

static JSValue dateTimeRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* dateTime = PyDateTime::from(self);
    StrBuilder builder;
    builder.append(dateTime->type()->nameInMessages(), '(', dateTime->year(), ", "_s, dateTime->month(), ", "_s, dateTime->day(), ", "_s, dateTime->hour(), ", "_s, dateTime->minute());
    if (dateTime->second() || dateTime->microsecond())
        builder.append(", "_s, dateTime->second());
    if (dateTime->microsecond())
        builder.append(", "_s, dateTime->microsecond());
    if (dateTime->fold())
        builder.append(", fold=1"_s);
    if (!dateTime->isNaive()) {
        JSString* tzinfo = repr(globalObject, dateTime->tzinfo());
        RETURN_IF_EXCEPTION(scope, { });
        String tzinfoText = tzinfo->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(", tzinfo="_s, tzinfoText);
    }
    builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue dateTimeStr(JSGlobalObject* globalObject, JSValue self)
{
    return callMethod(globalObject, self, stateOf(globalObject, PyDateTime::from(self)).isoformatName.get(), makeStr(getVM(globalObject), " "_s));
}

static JSValue dateTimeIsoFormat(JSGlobalObject* globalObject, JSValue self, JSValue sep, JSValue timespec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* dateTime = PyDateTime::from(self);
    char32_t separator = 'T';
    if (sep) {
        if (!isStr(sep))
            RELEASE_AND_RETURN(scope, raiseBadArgument(globalObject, "isoformat"_s, "argument 'sep'"_s, "a unicode character"_s, sep));
        StrView view(globalObject, strValue(sep));
        RETURN_IF_EXCEPTION(scope, { });
        if (view.length() != 1)
            return raiseTypeError(globalObject, scope, "isoformat(): argument 'sep' must be a unicode character, not a string of length "_s, view.length());
        separator = view.at(0);
    }
    TimeSpec spec;
    if (failed(scope, timeSpecArgument(globalObject, timespec, dateTime->microsecond(), spec)))
        return { };

    Text date;
    appendIsoDate(date, dateTime->packed());
    Text time;
    Text& text = separator < 0x80 ? date : time;
    if (separator < 0x80)
        date.append(static_cast<char>(separator));
    appendIsoTime(text, dateTime->packed(), spec);
    if (!dateTime->isNaive()) {
        bool isAppended = appendUtcOffset(globalObject, stateOf(globalObject, dateTime), text, ":"_s, dateTime->tzinfo(), dateTime);
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT_UNUSED(isAppended, isAppended);
    }
    if (separator < 0x80) [[likely]]
        return makeStr(vm, date.toString());
    RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString(date.span(), stringFromCodePoint(separator), time.span())));
}

static JSValue dateTimeCTime(JSGlobalObject* globalObject, JSValue self) { return formatCTime(globalObject, PyDateTime::from(self)->packed()); }

// ---- Time zones ----

static JSValue dateTimeUtcOffset(JSGlobalObject* globalObject, JSValue self)
{
    auto* dateTime = PyDateTime::from(self);
    return callUtcOffset(globalObject, stateOf(globalObject, dateTime), dateTime->tzinfo(), self);
}

static JSValue dateTimeDst(JSGlobalObject* globalObject, JSValue self)
{
    auto* dateTime = PyDateTime::from(self);
    return callDst(globalObject, stateOf(globalObject, dateTime), dateTime->tzinfo(), self);
}

static JSValue dateTimeTzName(JSGlobalObject* globalObject, JSValue self)
{
    auto* dateTime = PyDateTime::from(self);
    return callTzName(globalObject, stateOf(globalObject, dateTime), dateTime->tzinfo(), self);
}

// The zone of the system as it is at that moment: a timezone with its offset and its name.
static JSValue dateTimeLocalZoneAt(JSGlobalObject* globalObject, State& state, time_t when)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct tm tm;
    if (failed(scope, toLocalTime(globalObject, when, tm)))
        return { };
    JSValue offset = newDelta(globalObject, state, 0, tm.tm_gmtoff, 0);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue name = tm.tm_zone ? JSValue(makeStr(globalObject, decodeFilesystem(byteCast<uint8_t>(unsafeSpan(tm.tm_zone))))) : JSValue();
    RELEASE_AND_RETURN(scope, newTimeZone(globalObject, state, PyTimeDelta::from(offset), name));
}

static JSValue dateTimeLocalZoneOf(JSGlobalObject* globalObject, State& state, PyDateTime* local)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Fields fields = local->fields();
    int64_t seconds;
    int64_t otherSeconds;
    if (failed(scope, dateTimeLocalToUtc(globalObject, fields, fields.fold, seconds)) || failed(scope, dateTimeLocalToUtc(globalObject, fields, !fields.fold, otherSeconds)))
        return { };
    // In a gap, the offset is the one from the other side of it.
    if (otherSeconds != seconds && (otherSeconds > seconds) == fields.fold)
        seconds = otherSeconds;
    RELEASE_AND_RETURN(scope, dateTimeLocalZoneAt(globalObject, state, seconds - epochSeconds));
}

static JSValue dateTimeAsTimeZone(JSGlobalObject* globalObject, JSValue self, JSValue tz)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* dateTime = PyDateTime::from(self);
    State& state = stateOf(globalObject, dateTime);
    if (!tz)
        tz = none();
    if (failed(scope, checkTzInfo(globalObject, state, tz)))
        return { };

    // One that is naive, or whose zone does not know, is in the zone of the system.
    JSValue offset = none();
    if (!dateTime->isNaive()) {
        if (dateTime->tzinfo() == tz)
            return self;
        offset = callUtcOffset(globalObject, state, dateTime->tzinfo(), self);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (isNone(offset)) {
        JSValue localZone = dateTimeLocalZoneOf(globalObject, state, dateTime);
        RETURN_IF_EXCEPTION(scope, { });
        if (localZone == tz)
            return self;
        offset = PyTimeZone::from(localZone)->offset();
    }

    JSValue shifted = addDelta(globalObject, state, dateTime, PyTimeDelta::from(offset), -1);
    RETURN_IF_EXCEPTION(scope, { });
    // A class derived from datetime was called for it, and may have returned anything.
    auto* utc = PyDateTime::tryFrom(shifted);
    if (!utc)
        return raiseTypeError(globalObject, scope, "fromutc: argument must be a datetime"_s);
    if (isNone(tz)) {
        tz = dateTimeLocalZoneAt(globalObject, state, dateTimeSeconds(utc->fields()) - epochSeconds);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto* inZone = PyDateTime::create(vm, state.dateTimeStructure.get(), utc->type(), utc->packed(), tz);
    RELEASE_AND_RETURN(scope, callMethod(globalObject, tz, state.fromutcName.get(), inZone));
}

// ---- The rest ----

static JSValue dateTimeTimeTuple(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* dateTime = PyDateTime::from(self);
    State& state = stateOf(globalObject, dateTime);
    int dstFlag = -1;
    JSValue dst = callDst(globalObject, state, dateTime->tzinfo(), self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isNone(dst))
        dstFlag = !PyTimeDelta::from(dst)->isZero();
    RELEASE_AND_RETURN(scope, makeStructTime(globalObject, state, dateTime->year(), dateTime->month(), dateTime->day(), dateTime->hour(), dateTime->minute(), dateTime->second(), dstFlag));
}

static JSValue dateTimeUtcTimeTuple(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* dateTime = PyDateTime::from(self);
    State& state = stateOf(globalObject, dateTime);
    JSValue offset = callUtcOffset(globalObject, state, dateTime->tzinfo(), self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isNone(offset)) {
        JSValue shifted = addDelta(globalObject, state, dateTime, PyTimeDelta::from(offset), -1);
        RETURN_IF_EXCEPTION(scope, { });
        dateTime = PyDateTime::tryFrom(shifted);
        if (!dateTime)
            return raiseTypeError(globalObject, scope, "fromutc: argument must be a datetime"_s);
    }
    RELEASE_AND_RETURN(scope, makeStructTime(globalObject, state, dateTime->year(), dateTime->month(), dateTime->day(), dateTime->hour(), dateTime->minute(), dateTime->second(), 0));
}

static JSValue dateTimeTimestamp(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* dateTime = PyDateTime::from(self);
    if (dateTime->isNaive()) {
        int64_t seconds;
        if (failed(scope, dateTimeLocalToUtc(globalObject, dateTime->fields(), dateTime->fold(), seconds)))
            return { };
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, static_cast<double>(seconds - epochSeconds) + dateTime->microsecond() / 1e6));
    }
    State& state = stateOf(globalObject, dateTime);
    JSValue sinceEpoch = subtractDateTimes(globalObject, state, dateTime, state.epoch.get());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, intTrueDivide(globalObject, makeInt(globalObject, static_cast<int64_t>(PyTimeDelta::from(sinceEpoch)->total())), jsNumber(microsecondsPerSecond)));
}

static JSValue dateTimeGetDate(JSGlobalObject* globalObject, JSValue self)
{
    auto* dateTime = PyDateTime::from(self);
    State& state = stateOf(globalObject, dateTime);
    return PyDate::create(getVM(globalObject), state.dateStructure.get(), state.dateType.get(), dateTime->packed() & ~Packed::timeMask);
}

static JSValue dateTimeGetTime(JSGlobalObject* globalObject, JSValue self)
{
    auto* dateTime = PyDateTime::from(self);
    State& state = stateOf(globalObject, dateTime);
    return PyTimeOfDay::create(getVM(globalObject), state.timeStructure.get(), state.timeType.get(), dateTime->packed() & Packed::timeMask, none());
}

static JSValue dateTimeGetTimeTz(JSGlobalObject* globalObject, JSValue self)
{
    auto* dateTime = PyDateTime::from(self);
    State& state = stateOf(globalObject, dateTime);
    return PyTimeOfDay::create(getVM(globalObject), state.timeStructure.get(), state.timeType.get(), dateTime->packed() & Packed::timeMask, dateTime->tzinfo());
}

static JSValue dateTimeReplace(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* dateTime = PyDateTime::from(self);
    JSValue values[9];
    Fields fields = dateTime->fields();
    JSValue tzinfo = dateTime->tzinfo();
    if (failed(scope, bindArguments(globalObject, dateTimeReplaceParameters, args, values)) || failed(scope, dateTimeTakeArguments(globalObject, values, fields, tzinfo)))
        return { };
    RELEASE_AND_RETURN(scope, makeDateTime(globalObject, stateOf(globalObject, dateTime), dateTime->type(), fields, tzinfo));
}

static JSValue dateTimeReduceFor(JSGlobalObject* globalObject, PyDateTime* dateTime, int protocol)
{
    uint8_t bytes[] = {
        static_cast<uint8_t>(dateTime->year() >> 8), static_cast<uint8_t>(dateTime->year()),
        static_cast<uint8_t>(dateTime->month() | (protocol > 3 && dateTime->fold() ? 0x80 : 0)), static_cast<uint8_t>(dateTime->day()),
        static_cast<uint8_t>(dateTime->hour()), static_cast<uint8_t>(dateTime->minute()), static_cast<uint8_t>(dateTime->second()),
        static_cast<uint8_t>(dateTime->microsecond() >> 16), static_cast<uint8_t>(dateTime->microsecond() >> 8), static_cast<uint8_t>(dateTime->microsecond())
    };
    PyBytes* base = PyBytes::create(globalObject, std::span<const uint8_t> { bytes });
    PyTuple* state = dateTime->isNaive() ? PyTuple::create(globalObject, { base }) : PyTuple::create(globalObject, { base, dateTime->tzinfo() });
    return PyTuple::create(globalObject, { dateTime->type(), state });
}

static JSValue dateTimeReduceEx(JSGlobalObject* globalObject, JSValue self, JSValue protocolValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int protocol;
    if (failed(scope, intArgument(globalObject, protocolValue, protocol)))
        return { };
    RELEASE_AND_RETURN(scope, dateTimeReduceFor(globalObject, PyDateTime::from(self), protocol));
}

static JSValue dateTimeReduce(JSGlobalObject* globalObject, JSValue self) { return dateTimeReduceFor(globalObject, PyDateTime::from(self), 2); }

static JSValue dateTimeGetHour(JSGlobalObject*, JSValue self) { return jsNumber(PyDateTime::from(self)->hour()); }
static JSValue dateTimeGetMinute(JSGlobalObject*, JSValue self) { return jsNumber(PyDateTime::from(self)->minute()); }
static JSValue dateTimeGetSecond(JSGlobalObject*, JSValue self) { return jsNumber(PyDateTime::from(self)->second()); }
static JSValue dateTimeGetMicrosecond(JSGlobalObject*, JSValue self) { return jsNumber(PyDateTime::from(self)->microsecond()); }
static JSValue dateTimeGetTzInfo(JSGlobalObject*, JSValue self) { return PyDateTime::from(self)->tzinfo(); }
static JSValue dateTimeGetFold(JSGlobalObject*, JSValue self) { return jsNumber(PyDateTime::from(self)->fold()); }

static constexpr NativeDef dateTimeMethods[] = {
    PY_NEW(dateTimeNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", dateTimeRepr, "($self, /)"),
    PY_SLOT("__str__", dateTimeStr, "($self, /)"),
    PY_HASH(dateTimeHash),
    PY_DATETIME_COMPARISONS(dateTimeCompare),
    PY_SLOT("__add__", dateTimeAdd, "($self, value, /)"),
    PY_SLOT("__radd__", dateTimeReflected<dateTimeAdd>, "($self, value, /)"),
    PY_SLOT("__sub__", dateTimeSubtract, "($self, value, /)"),
    PY_SLOT("__rsub__", dateTimeReflected<dateTimeSubtract>, "($self, value, /)"),
    PY_CLASSMETHOD("now", dateTimeNow, "($type, /, tz=None)"),
    PY_CLASSMETHOD("utcnow", dateTimeUtcNow, "($type, /)"),
    PY_CLASSMETHOD("fromtimestamp", dateTimeFromTimestamp, "($type, /, timestamp, tz=None)"),
    PY_CLASSMETHOD("utcfromtimestamp", dateTimeUtcFromTimestamp, "($type, timestamp, /)"),
    PY_CLASSMETHOD("strptime", dateTimeStrptime, "($type, string, format, /)"),
    PY_CLASSMETHOD("combine", dateTimeCombine, "($type, /, date, time, tzinfo=<unrepresentable>)"),
    PY_CLASSMETHOD("fromisoformat", dateTimeFromIsoFormat, "($type, string, /)"),
    PY_METHOD("date", dateTimeGetDate, "($self, /)"),
    PY_METHOD("time", dateTimeGetTime, "($self, /)"),
    PY_METHOD("timetz", dateTimeGetTimeTz, "($self, /)"),
    PY_METHOD("ctime", dateTimeCTime, "($self, /)"),
    PY_METHOD("timetuple", dateTimeTimeTuple, "($self, /)"),
    PY_METHOD("timestamp", dateTimeTimestamp, "($self, /)"),
    PY_METHOD("utctimetuple", dateTimeUtcTimeTuple, "($self, /)"),
    PY_METHOD("isoformat", dateTimeIsoFormat, "($self, /, sep='T', timespec='auto')"),
    PY_METHOD("utcoffset", dateTimeUtcOffset, "($self, /)"),
    PY_METHOD("tzname", dateTimeTzName, "($self, /)"),
    PY_METHOD("dst", dateTimeDst, "($self, /)"),
    PY_METHOD("replace", dateTimeReplace, "!($self, /, year=unchanged, month=unchanged, day=unchanged,\n        hour=unchanged, minute=unchanged, second=unchanged,\n        microsecond=unchanged, tzinfo=unchanged, *, fold=unchanged)"),
    PY_METHOD("__replace__", dateTimeReplace, "!($self, /, **changes)"),
    PY_METHOD("astimezone", dateTimeAsTimeZone, "($self, /, tz=None)"),
    PY_METHOD("__reduce_ex__", dateTimeReduceEx, "($self, proto, /)"),
    PY_METHOD("__reduce__", dateTimeReduce, "($self, /)"),
};

static constexpr NativeGetSet dateTimeGetSets[] = {
    { "hour"_s, dateTimeGetHour },
    { "minute"_s, dateTimeGetMinute },
    { "second"_s, dateTimeGetSecond },
    { "microsecond"_s, dateTimeGetMicrosecond },
    { "tzinfo"_s, dateTimeGetTzInfo },
    { "fold"_s, dateTimeGetFold },
};

constexpr TypeDef dateTimeTypeDef {
    "datetime.datetime"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, dateTimeMethods, dateTimeGetSets, { }
};

} // namespace DateTime
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
