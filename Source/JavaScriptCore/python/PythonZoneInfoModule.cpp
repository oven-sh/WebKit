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
#include "PyDict.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonOperations.h"
#include "PythonText.h"

// _zoneinfo: Modules/_zoneinfo.c of CPython, function for function. What is in a file of the time zone database is read by zoneinfo._common, which is written in Python, here as there.

namespace JSC { namespace Python {

namespace {

constexpr int epochOrdinal = 719163;
enum class Source : uint8_t { NoCache, Cache, File };
constexpr size_t strongCacheMaxSize = 8;

// CalendarRule and DayRule: when in a year the clocks are changed
struct TransitionRule {
    bool isCalendarRule { false };
    // Mm.w.d: the d'th day of week w of month m
    uint8_t month { 0 }; // 1 - 12
    uint8_t week { 0 }; // 1 - 5
    // Of the week, 0 - 6, or of the year, 0 - 365
    uint16_t day { 0 };
    bool isJulian { false };
    int16_t hour { 0 }; // -167 - 167, RFC 8536 3.3.1
    int8_t minute { 0 };
    int8_t second { 0 };

    int64_t yearToTimestamp(int year) const;
};

// calendarrule_year_to_timestamp() and dayrule_year_to_timestamp(): the local time of it in a year
int64_t TransitionRule::yearToTimestamp(int year) const
{
    int64_t sinceMidnight = static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(minute) * 60 + second;
    if (isCalendarRule) {
        // Week 1 is the first in which there is such a day, where 0 is Sunday, and week 5 is the last. So it takes what day the month begins on and how many days it has.
        int firstDay = (dateToOrdinal(year, month, 1) + 6) % 7;
        int inMonth = daysInMonth(year, month);
        // The calendar has 0 for Monday and POSIX has it for Sunday.
        int monthDay = (static_cast<int>(day) - (firstDay + 1)) % 7;
        if (monthDay < 0)
            monthDay += 7;
        monthDay += 1;
        monthDay += (static_cast<int>(week) - 1) * 7;
        // Only if it was week 5
        if (monthDay > inMonth)
            monthDay -= 7;
        int64_t ordinal = dateToOrdinal(year, month, monthDay) - epochOrdinal;
        return ordinal * 86400 + sinceMidnight;
    }
    // Whole days before it, so one fewer for 1 January
    int64_t daysBefore = dateToOrdinal(year, 1, 1) - epochOrdinal - 1;
    // A Julian day leaves out 29 February: J60 is 1 March in every year.
    unsigned dayOfYear = day;
    if (isJulian && dayOfYear >= 59 && isLeapYear(year))
        dayOfYear += 1;
    return (daysBefore + dayOfYear) * 86400 + sinceMidnight;
}

// PyZoneInfo_ZoneInfo. It is filled in once, before anything can get at it.
struct ZoneInfoState final : NativeState {
    PYTHON_NATIVE_STATE(ZoneInfoState);
    WriteBarrier<Unknown> key;
    WriteBarrier<Unknown> fileRepr;
    Vector<int64_t> transitionsUTC; // trans_list_utc
    Vector<int64_t> transitionsWall[2]; // trans_list_wall
    // A _ttinfo goes by a number. Those of the file come first, and after them the two of tzrule_after, std and dst.
    Vector<uint32_t> transitionTTInfos; // trans_ttinfos
    std::optional<uint32_t> ttinfoBefore;
    size_t ttinfoCount { 0 }; // num_ttinfos
    WriteBarrier<PyTuple> ttinfoValues; // utcoff, dstoff and tzname of each, one after another
    Vector<long> utcoffSeconds;
    // _tzrule
    int dstDifference { 0 };
    TransitionRule start;
    TransitionRule end;
    bool isStandardOnly { false };

    bool isFixedOffset { false };
    Source source { Source::NoCache };

    uint32_t standardAfter() const { return ttinfoCount; }
    uint32_t daylightAfter() const { return ttinfoCount + 1; }
    JSValue utcoff(uint32_t ttinfo) const { return ttinfoValues->at(ttinfo * 3); }
    JSValue dstoff(uint32_t ttinfo) const { return ttinfoValues->at(ttinfo * 3 + 1); }
    JSValue tzname(uint32_t ttinfo) const { return ttinfoValues->at(ttinfo * 3 + 2); }
};

template<typename Visitor>
void ZoneInfoState::visit(Visitor& visitor)
{
    visitor.append(key);
    visitor.append(fileRepr);
    visitor.append(ttinfoValues);
}

// zoneinfo_state
struct ZoneInfoModuleState final : NativeState {
    PYTHON_NATIVE_STATE(ZoneInfoModuleState);
    WriteBarrier<PyType> type;
    WriteBarrier<Unknown> ioOpen;
    WriteBarrier<Unknown> findTZFile; // zoneinfo._tzpath.find_tzfile
    WriteBarrier<Unknown> commonModule; // zoneinfo._common
    WriteBarrier<PyDict> timeDeltaCache;
    WriteBarrier<Unknown> weakCache;
    // ZONEINFO_STRONG_CACHE: the last few that were asked for, the latest first
    WriteBarrier<Unknown> strongKeys[strongCacheMaxSize];
    WriteBarrier<Unknown> strongZones[strongCacheMaxSize];
    size_t strongCount { 0 };
};

template<typename Visitor>
void ZoneInfoModuleState::visit(Visitor& visitor)
{
    visitor.append(type);
    visitor.append(ioOpen);
    visitor.append(findTZFile);
    visitor.append(commonModule);
    visitor.append(timeDeltaCache);
    visitor.append(weakCache);
    for (auto& key : strongKeys)
        visitor.append(key);
    for (auto& zone : strongZones)
        visitor.append(zone);
}

ZoneInfoModuleState& zoneInfoModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<ZoneInfoModuleState>(); }

// PyObject_CallMethod(object, name, "O", argument), which for the sake of old times takes an argument that is a tuple for all of the arguments. So ZoneInfo(("UTC",)) finds what ZoneInfo("UTC") left in the cache.
JSValue callMethodWithObject(JSGlobalObject* globalObject, JSValue object, ASCIILiteral name, JSValue argument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isTuple(argument)) [[likely]]
        RELEASE_AND_RETURN(scope, callMethodNamed(globalObject, object, Identifier::fromString(vm, name), argument));
    JSValue method = getAttribute(globalObject, object, Identifier::fromString(vm, name));
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    for (auto& item : asTuple(argument)->span())
        arguments.append(item.get());
    RELEASE_AND_RETURN(scope, call(globalObject, method, arguments));
}

// ---- The strong cache, which is only for the class itself and not for what is derived from it

// find_in_strong_cache(): where a key is. Nothing if it is not there, or if it raised.
std::optional<size_t> findInStrongCache(JSGlobalObject* globalObject, JSValue key)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& state = zoneInfoModuleState(globalObject);
    for (size_t i = 0; i < state.strongCount; ++i) {
        bool isSame = isEqual(globalObject, key, state.strongKeys[i].get());
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (isSame)
            return i;
    }
    return std::nullopt;
}

// remove_from_strong_cache()
void removeFromStrongCache(JSGlobalObject* globalObject, size_t index)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = zoneInfoModuleState(globalObject);
    // Comparing keys may have run something that took it out already.
    if (index >= state.strongCount)
        return;
    for (size_t i = index; i + 1 < state.strongCount; ++i) {
        state.strongKeys[i].set(vm, realm, state.strongKeys[i + 1].get());
        state.strongZones[i].set(vm, realm, state.strongZones[i + 1].get());
    }
    --state.strongCount;
    state.strongKeys[state.strongCount].clear();
    state.strongZones[state.strongCount].clear();
}

// What is put at the front pushes the rest back, and the last of them out if there is no room for it.
void putAtFrontOfStrongCache(JSGlobalObject* globalObject, JSValue key, JSValue zone)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = zoneInfoModuleState(globalObject);
    state.strongCount = std::min(state.strongCount + 1, strongCacheMaxSize);
    for (size_t i = state.strongCount; i-- > 1;) {
        state.strongKeys[i].set(vm, realm, state.strongKeys[i - 1].get());
        state.strongZones[i].set(vm, realm, state.strongZones[i - 1].get());
    }
    state.strongKeys[0].set(vm, realm, key);
    state.strongZones[0].set(vm, realm, zone);
}

// eject_from_strong_cache(). It may raise.
void ejectFromStrongCache(JSGlobalObject* globalObject, PyType* type, JSValue key)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (type != zoneInfoModuleState(globalObject).type.get())
        return;
    auto index = findInStrongCache(globalObject, key);
    RETURN_IF_EXCEPTION(scope, void());
    if (index)
        removeFromStrongCache(globalObject, *index);
}

// zone_from_strong_cache(). Empty if it is not there, or if it raised.
JSValue zoneFromStrongCache(JSGlobalObject* globalObject, PyType* type, JSValue key)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& state = zoneInfoModuleState(globalObject);
    if (type != state.type.get())
        return { };
    auto index = findInStrongCache(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!index || *index >= state.strongCount)
        return { };
    // move_strong_cache_node_to_front()
    JSValue foundKey = state.strongKeys[*index].get();
    JSValue zone = state.strongZones[*index].get();
    removeFromStrongCache(globalObject, *index);
    putAtFrontOfStrongCache(globalObject, foundKey, zone);
    return zone;
}

// update_strong_cache()
void updateStrongCache(JSGlobalObject* globalObject, PyType* type, JSValue key, JSValue zone)
{
    if (type == zoneInfoModuleState(globalObject).type.get())
        putAtFrontOfStrongCache(globalObject, key, zone);
}

// clear_strong_cache()
void clearStrongCache(JSGlobalObject* globalObject, PyType* type)
{
    auto& state = zoneInfoModuleState(globalObject);
    if (type != state.type.get())
        return;
    for (size_t i = 0; i < state.strongCount; ++i) {
        state.strongKeys[i].clear();
        state.strongZones[i].clear();
    }
    state.strongCount = 0;
}

// new_weak_cache()
JSValue newWeakCache(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue weakValueDictionary = importModuleAttribute(globalObject, "weakref"_s, "WeakValueDictionary"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, weakValueDictionary));
}

// get_weak_cache()
JSValue weakCacheOf(JSGlobalObject* globalObject, PyType* type)
{
    auto& state = zoneInfoModuleState(globalObject);
    if (type == state.type.get())
        return state.weakCache.get();
    return getAttribute(globalObject, type->object(), Identifier::fromString(globalObject->vm(), "_weak_cache"_s));
}

// ---- What is in a file

// load_timedelta(): there are few of them, so each is made once.
JSValue loadTimeDelta(JSGlobalObject* globalObject, long seconds)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyDict* cache = zoneInfoModuleState(globalObject).timeDeltaCache.get();
    JSValue offset = intFromInt64(globalObject, seconds);
    JSValue cached = cache->get(globalObject, offset);
    RETURN_IF_EXCEPTION(scope, { });
    if (cached)
        return cached;
    // Delta_FromDelta() takes an int.
    JSValue delta = newTimeDelta(globalObject, 0, static_cast<int>(seconds), 0, true);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, cache->getOrAdd(globalObject, offset, delta));
}

// build_ttinfo(). It may raise.
void buildTTInfo(JSGlobalObject* globalObject, ZoneInfoState& self, uint32_t ttinfo, long utcOffset, long dstOffset, JSValue name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    self.utcoffSeconds[ttinfo] = utcOffset;
    JSValue utcoff = loadTimeDelta(globalObject, utcOffset);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue dstoff = loadTimeDelta(globalObject, dstOffset);
    RETURN_IF_EXCEPTION(scope, void());
    self.ttinfoValues->initializeAt(vm, ttinfo * 3, utcoff);
    self.ttinfoValues->initializeAt(vm, ttinfo * 3 + 1, dstoff);
    self.ttinfoValues->initializeAt(vm, ttinfo * 3 + 2, name);
}

// build_tzrule(). `daylightName` is empty if there is no such time. It may raise.
void buildTZRule(JSGlobalObject* globalObject, ZoneInfoState& self, JSValue standardName, JSValue daylightName, long standardOffset, long daylightOffset)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    buildTTInfo(globalObject, self, self.standardAfter(), standardOffset, 0, standardName);
    RETURN_IF_EXCEPTION(scope, void());
    if (!daylightName) {
        self.isStandardOnly = true;
        return;
    }
    self.dstDifference = static_cast<int>(daylightOffset - standardOffset);
    RELEASE_AND_RETURN(scope, buildTTInfo(globalObject, self, self.daylightAfter(), daylightOffset, self.dstDifference, daylightName));
}

// ---- A TZ string, as POSIX has it: std offset[dst[offset],start[/time],end[/time]]. Each of these moves on past what it has read, and is false if that will not do.

bool parseDigitsOfRule(const char*& p, int minimum, int maximum, int& value)
{
    value = 0;
    for (int i = 0; i < maximum; ++i, ++p) {
        if (!isASCIIDigit(*p))
            return i >= minimum;
        value *= 10;
        value += *p - '0';
    }
    return true;
}

// parse_abbr(): letters, or between < and > letters, digits and signs
bool parseAbbreviation(VM& vm, const char*& p, JSValue& abbreviation)
{
    const char* pointer = p;
    const char* start;
    const char* end;
    if (*pointer == '<') {
        ++pointer;
        start = pointer;
        while (*pointer != '>') {
            if (!isASCIIAlphanumeric(*pointer) && *pointer != '+' && *pointer != '-')
                return false;
            ++pointer;
        }
        end = pointer;
        if (end == start)
            return false;
        ++pointer;
    } else {
        start = pointer;
        while (isASCIIAlpha(*pointer))
            ++pointer;
        end = pointer;
        if (end == start)
            return false;
    }
    abbreviation = jsString(vm, String(byteCast<Latin1Character>(std::span(start, end))));
    p = pointer;
    return true;
}

// parse_transition_time(): [+|-]hh[h][:mm[:ss]]
bool parseTransitionTime(const char*& p, int& hour, int& minute, int& second)
{
    const char* pointer = p;
    int sign = 1;
    if (*pointer == '-' || *pointer == '+') {
        if (*pointer == '-')
            sign = -1;
        ++pointer;
    }
    if (!parseDigitsOfRule(pointer, 1, 3, hour))
        return false;
    hour *= sign;
    if (*pointer == ':') {
        ++pointer;
        if (!parseDigitsOfRule(pointer, 2, 2, minute))
            return false;
        minute *= sign;
        if (*pointer == ':') {
            ++pointer;
            if (!parseDigitsOfRule(pointer, 2, 2, second))
                return false;
            second *= sign;
        }
    }
    p = pointer;
    return true;
}

// parse_tz_delta()
bool parseOffset(const char*& p, long& totalSeconds)
{
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    if (!parseTransitionTime(p, hours, minutes, seconds))
        return false;
    if (hours > 24 || hours < -24)
        return false;
    // What is west of Greenwich is written as more than nothing.
    totalSeconds = -(hours * 3600L + minutes * 60 + seconds);
    return true;
}

// parse_transition_rule(), with calendarrule_new() and dayrule_new(). What those raise is replaced by whoever calls this, so it is only said here that it will not do.
bool parseTransitionRule(const char*& p, TransitionRule& rule)
{
    const char* pointer = p;
    int hour = 2;
    int minute = 0;
    int second = 0;
    auto parseTime = [&] {
        if (*pointer != '/')
            return true;
        ++pointer;
        return parseTransitionTime(pointer, hour, minute, second);
    };
    if (*pointer == 'M') {
        int month;
        int week;
        int day;
        ++pointer;
        if (!parseDigitsOfRule(pointer, 1, 2, month) || *pointer++ != '.')
            return false;
        if (!parseDigitsOfRule(pointer, 1, 1, week) || *pointer++ != '.')
            return false;
        if (!parseDigitsOfRule(pointer, 1, 1, day) || !parseTime())
            return false;
        if (month < 1 || month > 12 || week < 1 || week > 5 || day < 0 || day > 6 || hour < -167 || hour > 167)
            return false;
        rule.isCalendarRule = true;
        rule.month = month;
        rule.week = week;
        rule.day = day;
    } else {
        int day = 0;
        bool isJulian = *pointer == 'J';
        if (isJulian)
            ++pointer;
        if (!parseDigitsOfRule(pointer, 1, 3, day) || !parseTime())
            return false;
        if (day < isJulian || day > 365 || hour < -167 || hour > 167)
            return false;
        rule.isCalendarRule = false;
        rule.isJulian = isJulian;
        rule.day = day;
    }
    rule.hour = hour;
    rule.minute = minute;
    rule.second = second;
    p = pointer;
    return true;
}

// parse_tz_str(). It may raise.
void parseTZString(JSGlobalObject* globalObject, ZoneInfoState& self, JSValue given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // PyBytes_AsString()
    if (!isBytes(given)) {
        raiseTypeError(globalObject, scope, concatenate("expected bytes, "_s, typeName(globalObject, given), " found"_s));
        return;
    }
    Vector<char, 64> text;
    text.append(byteCast<char>(*builtinBufferOf(given)));
    text.append('\0');
    auto complain = [&] (ASCIILiteral what) {
        String shown = repr(globalObject, given);
        RETURN_IF_EXCEPTION(scope, void());
        raiseValueError(globalObject, scope, concatenate(what, shown));
    };

    const char* p = text.span().data();
    JSValue standardName;
    JSValue daylightName;
    long standardOffset = 1 << 20;
    long daylightOffset = 1 << 20;
    if (!parseAbbreviation(vm, p, standardName))
        return complain("Invalid STD format in "_s);
    if (!parseOffset(p, standardOffset))
        return complain("Invalid STD offset in "_s);
    // If it ends here there is no other time.
    if (*p != '\0') {
        if (!parseAbbreviation(vm, p, daylightName))
            return complain("Invalid DST format in "_s);
        // With no offset it is an hour ahead.
        if (*p == ',')
            daylightOffset = standardOffset + 3600;
        else if (!parseOffset(p, daylightOffset))
            return complain("Invalid DST offset in "_s);
        for (TransitionRule* rule : { &self.start, &self.end }) {
            if (*p != ',')
                return complain("Missing transition rules in TZ string: "_s);
            ++p;
            if (!parseTransitionRule(p, *rule))
                return complain("Malformed transition rule in TZ string: "_s);
        }
        if (*p != '\0')
            return complain("Extraneous characters at end of TZ string: "_s);
    }
    RELEASE_AND_RETURN(scope, buildTZRule(globalObject, self, standardName, daylightName, standardOffset, daylightOffset));
}

// utcoff_to_dstoff(): how much of each offset is for daylight saving, which the file does not say. It is taken to be how far the offset is from that of the standard time on one side of it or the other.
void deriveDSTOffsets(const Vector<size_t>& transitionIndices, const Vector<long>& utcoffs, Vector<long>& dstoffs, const Vector<bool>& isDST)
{
    size_t ttinfoCount = utcoffs.size();
    size_t transitionCount = transitionIndices.size();
    size_t dstCount = ttinfoCount;
    size_t dstFound = 0;
    for (size_t i = 1; i < transitionCount; ++i) {
        if (dstCount == dstFound)
            break;
        size_t index = transitionIndices[i];
        size_t compared = transitionIndices[i - 1];
        // One that is not for daylight saving, or has been seen to
        if (!isDST[index] || dstoffs[index])
            continue;
        long dstoff = 0;
        long utcoff = utcoffs[index];
        if (!isDST[compared])
            dstoff = utcoff - utcoffs[compared];
        if (!dstoff && index < ttinfoCount - 1 && i + 1 < transitionCount) {
            compared = transitionIndices[i + 1];
            // If the one after is for daylight saving too, there may be a better one to go by further on.
            if (isDST[compared])
                continue;
            dstoff = utcoff - utcoffs[compared];
        }
        if (dstoff) {
            ++dstFound;
            dstoffs[index] = dstoff;
        }
    }
    if (dstFound < dstCount) {
        // What is left is taken to be an hour.
        for (size_t index = 0; index < ttinfoCount; ++index) {
            if (isDST[index] && !dstoffs[index])
                dstoffs[index] = 3600;
        }
    }
}

// ts_to_local(): when each change is by the clock, with no fold and with one
void deriveWallTransitions(ZoneInfoState& self, const Vector<size_t>& transitionIndices, const Vector<long>& utcoffs)
{
    size_t count = self.transitionsUTC.size();
    if (!count)
        return;
    self.transitionsWall[0] = self.transitionsUTC;
    self.transitionsWall[1] = self.transitionsUTC;
    int64_t offset0 = utcoffs[0];
    int64_t offset1 = utcoffs[0];
    if (utcoffs.size() > 1) {
        offset1 = utcoffs[transitionIndices[0]];
        if (offset1 > offset0)
            std::swap(offset0, offset1);
    }
    self.transitionsWall[0][0] += offset0;
    self.transitionsWall[1][0] += offset1;
    for (size_t i = 1; i < count; ++i) {
        offset0 = utcoffs[transitionIndices[i - 1]];
        offset1 = utcoffs[transitionIndices[i]];
        if (offset1 > offset0)
            std::swap(offset1, offset0);
        self.transitionsWall[0][i] += offset0;
        self.transitionsWall[1][i] += offset1;
    }
}

// PyTuple_GetItem(). Empty if it raised.
JSValue tupleItem(JSGlobalObject* globalObject, JSValue tuple, size_t index)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!isTuple(tuple))
        return raise(globalObject, scope, BuiltinType::SystemError, "bad argument to internal function"_s);
    if (index >= asTuple(tuple)->length())
        return raise(globalObject, scope, BuiltinType::IndexError, "tuple index out of range"_s);
    return asTuple(tuple)->at(index);
}

// load_data(): fills one in from a file. It may raise.
void loadData(JSGlobalObject* globalObject, PyStateObject* object, JSValue file)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& self = object->state<ZoneInfoState>();
    JSValue data = callMethodWithObject(globalObject, zoneInfoModuleState(globalObject).commonModule.get(), "load_data"_s, file);
    RETURN_IF_EXCEPTION(scope, void());
    if (!isExactly(globalObject, data, BuiltinType::Tuple)) {
        String shown = repr(globalObject, data);
        RETURN_IF_EXCEPTION(scope, void());
        raiseTypeError(globalObject, scope, concatenate("Invalid data result type: "_s, shown));
        return;
    }
    JSValue parts[6];
    for (unsigned i = 0; i < 6; ++i) {
        parts[i] = tupleItem(globalObject, data, i);
        RETURN_IF_EXCEPTION(scope, void());
    }
    auto [transitionIndexList, transitionsUTC, utcoffList, isDSTList, abbreviations, tzString] = parts;

    // PyTuple_Size()
    if (!isTuple(transitionsUTC) || !isTuple(utcoffList)) {
        raise(globalObject, scope, BuiltinType::SystemError, "bad argument to internal function"_s);
        return;
    }
    size_t transitionCount = asTuple(transitionsUTC)->length();
    size_t ttinfoCount = asTuple(utcoffList)->length();
    self.ttinfoCount = ttinfoCount;

    Vector<size_t> transitionIndices;
    for (size_t i = 0; i < transitionCount; ++i) {
        auto when = toCLongLong(globalObject, asTuple(transitionsUTC)->at(i));
        RETURN_IF_EXCEPTION(scope, void());
        self.transitionsUTC.append(*when);
        JSValue number = tupleItem(globalObject, transitionIndexList, i);
        RETURN_IF_EXCEPTION(scope, void());
        auto index = toSsizeOfInt(globalObject, number);
        RETURN_IF_EXCEPTION(scope, void());
        if (static_cast<size_t>(*index) >= ttinfoCount) {
            raiseValueError(globalObject, scope, concatenate("Invalid transition index found while reading TZif: "_s, *index));
            return;
        }
        transitionIndices.append(static_cast<size_t>(*index));
    }

    Vector<long> utcoffs;
    Vector<bool> isDST;
    for (size_t i = 0; i < ttinfoCount; ++i) {
        auto offset = toCLong(globalObject, asTuple(utcoffList)->at(i));
        RETURN_IF_EXCEPTION(scope, void());
        utcoffs.append(*offset);
        JSValue flag = tupleItem(globalObject, isDSTList, i);
        RETURN_IF_EXCEPTION(scope, void());
        bool isSo = isTrue(globalObject, flag);
        RETURN_IF_EXCEPTION(scope, void());
        isDST.append(isSo);
    }

    Vector<long> dstoffs;
    dstoffs.fill(0, ttinfoCount);
    deriveDSTOffsets(transitionIndices, utcoffs, dstoffs, isDST);
    deriveWallTransitions(self, transitionIndices, utcoffs);

    // With room for the two of the rule that comes after
    PyTuple* values = PyTuple::tryCreate(globalObject, static_cast<unsigned>((ttinfoCount + 2) * 3));
    RETURN_IF_EXCEPTION(scope, void());
    for (auto& place : values->span())
        place.clear();
    self.ttinfoValues.set(vm, object, values);
    self.utcoffSeconds.fill(0, ttinfoCount + 2);
    for (size_t i = 0; i < ttinfoCount; ++i) {
        JSValue name = tupleItem(globalObject, abbreviations, i);
        RETURN_IF_EXCEPTION(scope, void());
        buildTTInfo(globalObject, self, i, utcoffs[i], dstoffs[i], name);
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (size_t index : transitionIndices)
        self.transitionTTInfos.append(static_cast<uint32_t>(index));

    // What holds before the first change is the first that is not for daylight saving, or the first of all if they all are.
    for (size_t i = 0; i < ttinfoCount; ++i) {
        if (!isDST[i]) {
            self.ttinfoBefore = i;
            break;
        }
    }
    if (!self.ttinfoBefore && ttinfoCount)
        self.ttinfoBefore = 0;

    bool hasTZString = !isNone(tzString);
    if (hasTZString) {
        hasTZString = isTrue(globalObject, tzString);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (hasTZString) {
        parseTZString(globalObject, self, tzString);
        RETURN_IF_EXCEPTION(scope, void());
    } else {
        if (!ttinfoCount) {
            raiseValueError(globalObject, scope, "No time zone information found."_s);
            return;
        }
        uint32_t ttinfo = transitionCount ? transitionIndices.last() : ttinfoCount - 1;
        buildTZRule(globalObject, self, self.tzname(ttinfo), JSValue(), self.utcoffSeconds[ttinfo], 0);
        RETURN_IF_EXCEPTION(scope, void());
        // That makes a rule with standard time only, out of what may be for daylight saving.
        if (*tryTimeDelta(self.dstoff(ttinfo)))
            values->initializeAt(vm, self.standardAfter() * 3 + 1, self.dstoff(ttinfo));
    }

    // Whether what utcoffset(), dst() and tzname() give is the same whenever it is. It is taken that a rule with two times has changes, that no two of the file's are the same, and that none of them goes unused. It only matters to
    // a datetime.time.
    if (ttinfoCount > 1 || !self.isStandardOnly)
        self.isFixedOffset = false;
    else if (!ttinfoCount)
        self.isFixedOffset = true;
    else {
        // ttinfo_eq()
        uint32_t after = self.standardAfter();
        bool isSame = true;
        for (unsigned i = 0; i < 3 && isSame; ++i) {
            isSame = isEqual(globalObject, values->at(i), values->at(after * 3 + i));
            RETURN_IF_EXCEPTION(scope, void());
        }
        self.isFixedOffset = isSame;
    }
}

// ---- Which holds at a time

// _bisect(): how many of them are no later
size_t bisect(int64_t value, const Vector<int64_t>& array)
{
    size_t low = 0;
    size_t high = array.size();
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (array[middle] > value)
            high = middle;
        else
            low = middle + 1;
    }
    return high;
}

// find_tzrule_ttinfo(): by the clock
uint32_t findInRule(const ZoneInfoState& self, int64_t timestamp, bool fold, int year)
{
    if (self.isStandardOnly)
        return self.standardAfter();
    int64_t start = self.start.yearToTimestamp(year);
    int64_t end = self.end.yearToTimestamp(year);
    // With no fold, the time with the smaller offset runs from the end of what the clocks skip to the end of what they repeat, and with one from the start of the one to the start of the other. So where daylight saving begins and
    // ends goes by the fold and by whether it puts the clocks forward, as it nearly always does.
    if (fold == (self.dstDifference >= 0))
        end -= self.dstDifference;
    else
        start += self.dstDifference;
    bool isDST = start < end ? timestamp >= start && timestamp < end : timestamp < end || timestamp >= start;
    return isDST ? self.daylightAfter() : self.standardAfter();
}

// find_tzrule_ttinfo_fromutc(): by UTC
uint32_t findInRuleFromUTC(const ZoneInfoState& self, int64_t timestamp, int year, bool& fold)
{
    if (self.isStandardOnly) {
        fold = false;
        return self.standardAfter();
    }
    int64_t start = self.start.yearToTimestamp(year) - self.utcoffSeconds[self.standardAfter()];
    int64_t end = self.end.yearToTimestamp(year) - self.utcoffSeconds[self.daylightAfter()];
    bool isDST = start < end ? timestamp >= start && timestamp < end : timestamp < end || timestamp >= start;
    // What the clocks repeat comes after the end of daylight saving if that puts them forward, and before the start of it if it puts them back.
    int64_t ambiguousStart = self.dstDifference > 0 ? end : start;
    int64_t ambiguousEnd = self.dstDifference > 0 ? end + self.dstDifference : start - self.dstDifference;
    fold = timestamp >= ambiguousStart && timestamp < ambiguousEnd;
    return isDST ? self.daylightAfter() : self.standardAfter();
}

// get_local_timestamp(): seconds since 1970 by what a datetime says, whatever zone it is in. It may raise.
int64_t localTimestampOf(JSGlobalObject* globalObject, JSValue dateTime)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    long ordinal;
    long fields[3];
    if (isExactly(globalObject, dateTime, dateTimeModuleState(globalObject).dateTimeType.get())) {
        auto& state = *tryDateTime(dateTime);
        ordinal = dateToOrdinal(state.year(), state.month(), state.day());
        fields[0] = state.hour();
        fields[1] = state.minute();
        fields[2] = state.second();
    } else {
        JSValue number = callMethodNamed(globalObject, dateTime, Identifier::fromString(vm, "toordinal"_s));
        RETURN_IF_EXCEPTION(scope, 0);
        auto converted = toCLong(globalObject, number);
        RETURN_IF_EXCEPTION(scope, 0);
        ordinal = static_cast<int>(*converted);
        unsigned i = 0;
        for (ASCIILiteral name : { "hour"_s, "minute"_s, "second"_s }) {
            number = getAttribute(globalObject, dateTime, Identifier::fromString(vm, name));
            RETURN_IF_EXCEPTION(scope, 0);
            converted = toCLong(globalObject, number);
            RETURN_IF_EXCEPTION(scope, 0);
            fields[i++] = static_cast<int>(*converted);
        }
    }
    return static_cast<int64_t>(ordinal - epochOrdinal) * 86400 + static_cast<int64_t>(fields[0] * 3600L + fields[1] * 60 + fields[2]);
}

// find_ttinfo(). Nothing if it is NO_TTINFO, of which everything is None, or if it raised.
std::optional<uint32_t> findTTInfo(JSGlobalObject* globalObject, const ZoneInfoState& self, JSValue dateTime)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isNone(dateTime)) {
        if (self.isFixedOffset)
            return self.standardAfter();
        return std::nullopt;
    }
    int64_t timestamp = localTimestampOf(globalObject, dateTime);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    // CPython reads these out of whatever it was given as if that were a datetime. What has got this far and is none has what it takes to be taken for one, but for these.
    auto* state = tryDateTime(dateTime);
    bool fold = state && state->fold;
    int year = state ? state->year() : 1;
    auto& transitions = self.transitionsWall[fold];
    if (!transitions.isEmpty() && timestamp < transitions[0])
        return self.ttinfoBefore;
    if (transitions.isEmpty() || timestamp > transitions.last())
        return findInRule(self, timestamp, fold, year);
    return self.transitionTTInfos[bisect(timestamp, transitions) - 1];
}

// ---- Making one

PyStateObject* allocateZoneInfo(VM& vm, PyType* type) { return PyStateObject::create(vm, type->instanceStructure(), makeUnique<ZoneInfoState>()); }

// zoneinfo_new_instance()
JSValue newInstance(JSGlobalObject* globalObject, PyType* type, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = zoneInfoModuleState(globalObject);
    Identifier close = vm.pythonNames().attribute_close;
    JSValue path = call(globalObject, state.findTZFile.get(), key);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue file;
    if (isNone(path))
        file = callMethodWithObject(globalObject, state.commonModule.get(), "load_tzdata"_s, key);
    else
        file = call(globalObject, state.ioOpen.get(), path, jsNontrivialString(vm, "rb"_s));
    RETURN_IF_EXCEPTION(scope, { });

    PyStateObject* self = allocateZoneInfo(vm, type);
    loadData(globalObject, self, file);
    if (scope.exception()) [[unlikely]] {
        // It is closed all the same, and what comes of that comes of this.
        Exception* raised = takeRaisedException(vm);
        if (!raised)
            return { };
        callMethodNamed(globalObject, file, close);
        scope.release();
        chainRaisedExceptions(globalObject, raised);
        return { };
    }
    callMethodNamed(globalObject, file, close);
    RETURN_IF_EXCEPTION(scope, { });
    self->state<ZoneInfoState>().key.set(vm, self, key);
    return self;
}

// zoneinfo_ZoneInfo_impl()
JSValue zoneInfoFor(JSGlobalObject* globalObject, PyType* type, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue instance = zoneFromStrongCache(globalObject, type, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (instance)
        return instance;

    JSValue weakCache = weakCacheOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    instance = callMethodWithObject(globalObject, weakCache, "get"_s, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(instance)) {
        JSValue made = newInstance(globalObject, type, key);
        RETURN_IF_EXCEPTION(scope, { });
        stateOf<ZoneInfoState>(made).source = Source::Cache;
        instance = callMethodNamed(globalObject, weakCache, Identifier::fromString(vm, "setdefault"_s), key, made);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!isInstance(globalObject, instance, type)) {
        String found = fullyQualifiedTypeName(globalObject, instance);
        RETURN_IF_EXCEPTION(scope, { });
        String shown = repr(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        String name = type->nameWithoutModule(globalObject);
        return raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("Unexpected instance of "_s, found, " in "_s, name.substring(name.reverseFind('.') + 1), " weak cache for key "_s, shown));
    }
    updateStrongCache(globalObject, type, key, instance);
    return instance;
}

PYTHON_NATIVE(zoneInfoNew)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(zoneInfoFor(globalObject, asType(args[0]), args[1])));
}

// zoneinfo_ZoneInfo_from_file_impl()
PYTHON_NATIVE(zoneInfoFromFile)
{
    NATIVE_PROLOGUE();
    JSValue file = args[1];
    PyStateObject* object = allocateZoneInfo(vm, asType(args[0]));
    String shown = repr(globalObject, file);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue fileRepr = strOrMemoryError(globalObject, shown);
    RETURN_IF_EXCEPTION(scope, { });
    loadData(globalObject, object, file);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = object->state<ZoneInfoState>();
    self.source = Source::File;
    self.fileRepr.set(vm, object, fileRepr);
    self.key.set(vm, object, args.at(2) ? args.at(2) : jsUndefined());
    return JSValue::encode(object);
}

// zoneinfo_ZoneInfo_no_cache_impl()
PYTHON_NATIVE(zoneInfoNoCache)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(newInstance(globalObject, asType(args[0]), args[1])));
}

// zoneinfo_ZoneInfo_clear_cache_impl()
PYTHON_NATIVE(zoneInfoClearCache)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    JSValue onlyKeys = args.at(1);
    JSValue weakCache = weakCacheOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (!onlyKeys || isNone(onlyKeys)) {
        callMethodNamed(globalObject, weakCache, Identifier::fromString(vm, "clear"_s));
        // The other is cleared whatever came of that.
        clearStrongCache(globalObject, type);
        RETURN_IF_EXCEPTION(scope, { });
        RETURN_NONE();
    }
    JSValue iterator = getIterator(globalObject, onlyKeys);
    RETURN_IF_EXCEPTION(scope, { });
    Identifier pop = Identifier::fromString(vm, "pop"_s);
    while (true) {
        JSValue item = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            break;
        ejectFromStrongCache(globalObject, type, item);
        RETURN_IF_EXCEPTION(scope, { });
        callMethodNamed(globalObject, weakCache, pop, item, jsUndefined());
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// zoneinfo_ZoneInfo_utcoffset_impl(), zoneinfo_ZoneInfo_dst_impl() and zoneinfo_ZoneInfo_tzname_impl()
PYTHON_NATIVE(zoneInfoLookUp)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<ZoneInfoState>(args[0]);
    auto ttinfo = findTTInfo(globalObject, self, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!ttinfo)
        RETURN_NONE();
    return JSValue::encode(self.ttinfoValues->at(*ttinfo * 3 + unpack<unsigned>(callFrame, 0)));
}

// zoneinfo_fromutc()
PYTHON_NATIVE(zoneInfoFromUTC)
{
    NATIVE_PROLOGUE();
    JSValue dateTime = args[1];
    auto* given = tryDateTime(dateTime);
    if (!given)
        return JSValue::encode(raiseTypeError(globalObject, scope, "fromutc: argument must be a datetime"_s));
    if (given->tzinfoOrNone() != args[0])
        return JSValue::encode(raiseValueError(globalObject, scope, "fromutc: dt.tzinfo is not self"_s));
    auto& self = stateOf<ZoneInfoState>(args[0]);
    int64_t timestamp = localTimestampOf(globalObject, dateTime);
    RETURN_IF_EXCEPTION(scope, { });

    auto& transitions = self.transitionsUTC;
    size_t count = transitions.size();
    uint32_t ttinfo;
    bool fold = false;
    if (count >= 1 && timestamp < transitions[0])
        ttinfo = *self.ttinfoBefore;
    else if (!count || timestamp > transitions.last()) {
        ttinfo = findInRuleFromUTC(self, timestamp, given->year(), fold);
        // Just after the last change that is listed, what the clocks repeat or skip lies between what held before that change and what holds now, and not between the two times of the rule.
        if (count) {
            uint32_t previous = count == 1 ? *self.ttinfoBefore : self.transitionTTInfos[count - 2];
            int64_t difference = self.utcoffSeconds[previous] - self.utcoffSeconds[ttinfo];
            if (difference > 0 && timestamp < transitions.last() + difference)
                fold = true;
        }
    } else {
        size_t index = bisect(timestamp, transitions);
        uint32_t previous = index >= 2 ? self.transitionTTInfos[index - 2] : *self.ttinfoBefore;
        ttinfo = index >= 2 ? self.transitionTTInfos[index - 1] : self.transitionTTInfos[0];
        int64_t shift = self.utcoffSeconds[previous] - self.utcoffSeconds[ttinfo];
        if (shift > timestamp - transitions[index - 1])
            fold = true;
    }

    JSValue result = binaryOperation(globalObject, BinaryOperator::Add, false, dateTime, self.utcoff(ttinfo));
    RETURN_IF_EXCEPTION(scope, { });
    if (!fold)
        return JSValue::encode(result);
    // It has only just been made, and nothing else has it yet.
    if (isExactly(globalObject, result, dateTimeModuleState(globalObject).dateTimeType.get())) {
        tryDateTime(result)->fold = 1;
        return JSValue::encode(result);
    }
    JSValue replace = getAttribute(globalObject, result, names.attribute_replace);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    RELEASE_AND_RETURN(scope, JSValue::encode(callClassWithFold(globalObject, replace, arguments, 1)));
}

// zoneinfo_repr()
JSValue reprOfZoneInfo(JSGlobalObject* globalObject, JSValue zone)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& self = stateOf<ZoneInfoState>(zone);
    String type = fullyQualifiedTypeName(globalObject, zone);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isNone(self.key.get())) {
        String key = repr(globalObject, self.key.get());
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, concatenate(type, "(key="_s, key, ')')));
    }
    String file = asString(self.fileRepr.get())->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, concatenate(type, ".from_file("_s, file, ')')));
}

PYTHON_NATIVE(zoneInfoRepr)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(reprOfZoneInfo(globalObject, args[0])));
}

// zoneinfo_str()
PYTHON_NATIVE(zoneInfoStr)
{
    NATIVE_PROLOGUE();
    JSValue key = stateOf<ZoneInfoState>(args[0]).key.get();
    if (!isNone(key))
        return JSValue::encode(key);
    RELEASE_AND_RETURN(scope, JSValue::encode(reprOfZoneInfo(globalObject, args[0])));
}

// zoneinfo_reduce(): it is pickled as the key that it goes by and whether it was out of the cache. So what comes back is whatever the files say where that is. One that was made from a file cannot be.
PYTHON_NATIVE(zoneInfoReduce)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<ZoneInfoState>(args[0]);
    if (self.source == Source::File) {
        JSValue picklingError = importModuleAttribute(globalObject, "pickle"_s, "PicklingError"_s);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue exception = call(globalObject, picklingError, jsNontrivialString(vm, "Cannot pickle a ZoneInfo file from a file stream."_s));
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseObject(globalObject, scope, exception));
    }
    JSValue constructor = getAttribute(globalObject, args[0], Identifier::fromString(vm, "_unpickle"_s));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { constructor, PyTuple::create(globalObject, { self.key.get(), jsNumber(self.source == Source::Cache) }) }));
}

// zoneinfo_ZoneInfo__unpickle_impl()
PYTHON_NATIVE(zoneInfoUnpickle)
{
    NATIVE_PROLOGUE();
    auto fromCache = toUnsignedIntMask(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    if (static_cast<unsigned char>(*fromCache))
        RELEASE_AND_RETURN(scope, JSValue::encode(zoneInfoFor(globalObject, asType(args[0]), args[1])));
    RELEASE_AND_RETURN(scope, JSValue::encode(newInstance(globalObject, asType(args[0]), args[1])));
}

// zoneinfo_init_subclass(): each class derived from it has a cache of its own.
PYTHON_NATIVE(zoneInfoInitSubclass)
{
    NATIVE_PROLOGUE();
    JSValue weakCache = newWeakCache(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    setAttribute(globalObject, args[0], Identifier::fromString(vm, "_weak_cache"_s), weakCache);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

} // namespace

JSObject* createZoneInfoModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto& state = zoneInfoModuleState(globalObject);

    // PyDateTime_IMPORT
    importModule(globalObject, "_datetime"_s);
    RETURN_IF_EXCEPTION(scope, nullptr);

    if (!state.type) {
        PyType* type = createBuiltinType(globalObject, "zoneinfo.ZoneInfo"_s, dateTimeModuleState(globalObject).tzinfoType.get(), PyType::Layout::Native, PyType::IsBaseType);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        state.type.set(vm, realm, type);
        addGenericGetAttribute(globalObject, type);
        addMethods(globalObject, type, {
            { "__new__"_s, zoneInfoNew, Kind::New, 0, "ZoneInfo($type, /, key)"_s, Arguments::AreThoseOfTheClass },
            { "__repr__"_s, zoneInfoRepr },
            { "__str__"_s, zoneInfoStr },
            { "clear_cache"_s, zoneInfoClearCache, Kind::ClassMethod, 0, { }, Arguments::AreCheckedAsWithDefiningClass },
            { "no_cache"_s, zoneInfoNoCache, Kind::ClassMethod, 0, { }, Arguments::AreCheckedAsWithDefiningClass },
            { "from_file"_s, zoneInfoFromFile, Kind::ClassMethod, 0, { }, Arguments::AreCheckedAsWithDefiningClass },
            { "utcoffset"_s, zoneInfoLookUp, Kind::Method, pack(0u), { }, Arguments::AreCheckedAsWithDefiningClass },
            { "dst"_s, zoneInfoLookUp, Kind::Method, pack(1u), { }, Arguments::AreCheckedAsWithDefiningClass },
            { "tzname"_s, zoneInfoLookUp, Kind::Method, pack(2u), { }, Arguments::AreCheckedAsWithDefiningClass },
            { "fromutc"_s, zoneInfoFromUTC },
            { "__reduce__"_s, zoneInfoReduce },
            { "_unpickle"_s, zoneInfoUnpickle, Kind::ClassMethod, 0, { }, Arguments::AreCheckedAsWithDefiningClass },
            { "__init_subclass__"_s, zoneInfoInitSubclass, Kind::ClassMethod, 0, "($type, /, *args, **kwargs)"_s, Arguments::AreNotChecked },
        });
        // Py_T_OBJECT_EX: there is no such attribute while there is nothing there.
        addMember(globalObject, type, "key"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
            if (JSValue key = stateOf<ZoneInfoState>(self).key.get())
                return key;
            auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
            return raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', typeName(globalObject, self), "' object has no attribute 'key'"_s));
        });
    }

    JSObject* module = newBuiltinModule(globalObject, "_zoneinfo"_s);
    module->putDirect(vm, Identifier::fromString(vm, "ZoneInfo"_s), state.type->object());

    JSValue findTZFile = importModuleAttribute(globalObject, "zoneinfo._tzpath"_s, "find_tzfile"_s);
    RETURN_IF_EXCEPTION(scope, nullptr);
    state.findTZFile.set(vm, realm, findTZFile);
    JSValue ioOpen = importModuleAttribute(globalObject, "io"_s, "open"_s);
    RETURN_IF_EXCEPTION(scope, nullptr);
    state.ioOpen.set(vm, realm, ioOpen);
    JSValue commonModule = importModule(globalObject, "zoneinfo._common"_s);
    RETURN_IF_EXCEPTION(scope, nullptr);
    state.commonModule.set(vm, realm, commonModule);

    // initialize_caches(). In CPython a module that is made again has another class, with nothing in its caches.
    clearStrongCache(globalObject, state.type.get());
    state.timeDeltaCache.set(vm, realm, PyDict::create(globalObject));
    JSValue weakCache = newWeakCache(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    state.weakCache.set(vm, realm, weakCache);
    return module;
}

} } // namespace JSC::Python
