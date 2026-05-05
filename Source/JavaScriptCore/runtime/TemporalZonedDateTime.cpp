/*
 * Copyright (C) 2026 Oven Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "TemporalZonedDateTime.h"

#include "IntlObjectInlines.h"
#include "JSCInlines.h"
#include "JSDateMath.h"
#include "LazyPropertyInlines.h"
#include "TemporalInstant.h"
#include "TemporalPlainTime.h"
#include "VMTrapsInlines.h"
#include <cmath>
#include <unicode/ucal.h>
#include <wtf/DateMath.h>
#include <wtf/GregorianDateTime.h>
#include <wtf/text/MakeString.h>
#include <wtf/unicode/icu/ICUHelpers.h>

namespace JSC {

const ClassInfo TemporalZonedDateTime::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(TemporalZonedDateTime) };

Structure* TemporalZonedDateTime::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

TemporalZonedDateTime::TemporalZonedDateTime(VM& vm, Structure* structure, ISO8601::ExactTime exactTime, TimeZone timeZone)
    : Base(vm, structure)
    , m_exactTime(exactTime)
    , m_timeZone(timeZone)
{
}

TemporalZonedDateTime* TemporalZonedDateTime::create(VM& vm, Structure* structure, ISO8601::ExactTime exactTime, TimeZone timeZone)
{
    ASSERT(exactTime.isValid());
    auto* object = new (NotNull, allocateCell<TemporalZonedDateTime>(vm)) TemporalZonedDateTime(vm, structure, exactTime, timeZone);
    object->finishCreation(vm);
    return object;
}

void TemporalZonedDateTime::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    m_calendar.initLater(
        [] (const auto& init) {
            VM& vm = init.vm;
            auto* globalObject = init.owner->realm();
            auto* calendar = TemporalCalendar::create(vm, globalObject->calendarStructure(), iso8601CalendarID());
            init.set(calendar);
        });
}

template<typename Visitor>
void TemporalZonedDateTime::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    auto* thisObject = uncheckedDowncast<TemporalZonedDateTime>(cell);
    thisObject->m_calendar.visit(visitor);
}

DEFINE_VISIT_CHILDREN(TemporalZonedDateTime);

// https://tc39.es/proposal-temporal/#sec-temporal-createtemporalzoneddatetime
TemporalZonedDateTime* TemporalZonedDateTime::tryCreateIfValid(JSGlobalObject* globalObject, ISO8601::ExactTime exactTime, TimeZone timeZone, Structure* structure)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!exactTime.isValid()) {
        throwRangeError(globalObject, scope, "epoch nanoseconds is outside of supported range for Temporal.ZonedDateTime"_s);
        return { };
    }

    return create(vm, structure ? structure : globalObject->zonedDateTimeStructure(), exactTime, timeZone);
}

// https://tc39.es/proposal-temporal/#sec-temporal-getnamedtimezoneoffsetnanoseconds
// Returns the raw+DST offset in nanoseconds for a named IANA timezone at a
// specific instant. Uses ICU's UCalendar, matching the approach used by
// JSDateMath::calculateLocalTimeOffset and by temporal_rs (via icu_timezone).
int64_t getNamedTimeZoneOffsetNanoseconds(TimeZoneID id, ISO8601::ExactTime exactTime)
{
    static constexpr int64_t nsPerMs = 1'000'000;

    StringView name { intlTimeZoneIDToString(id) };
    auto nameUpconverted = name.upconvertedCharacters();

    UErrorCode status = U_ZERO_ERROR;
    auto calendar = std::unique_ptr<UCalendar, ICUDeleter<ucal_close>>(ucal_open(nameUpconverted, name.length(), "", UCAL_DEFAULT, &status));
    if (U_FAILURE(status))
        return 0;
    // Use proleptic Gregorian, matching ISO 8601 calendar semantics.
    ucal_setGregorianChange(calendar.get(), minECMAScriptTime, &status);
    status = U_ZERO_ERROR; // ignore unsupported-error

    ucal_setMillis(calendar.get(), static_cast<UDate>(exactTime.floorEpochMilliseconds()), &status);
    if (U_FAILURE(status))
        return 0;

    int32_t rawOffset = ucal_get(calendar.get(), UCAL_ZONE_OFFSET, &status);
    if (U_FAILURE(status))
        return 0;
    int32_t dstOffset = ucal_get(calendar.get(), UCAL_DST_OFFSET, &status);
    if (U_FAILURE(status))
        return 0;

    return (static_cast<int64_t>(rawOffset) + static_cast<int64_t>(dstOffset)) * nsPerMs;
}

// https://tc39.es/proposal-temporal/#sec-temporal-getoffsetnanosecondsfor
int64_t TemporalZonedDateTime::getTimeZoneOffsetNanoseconds(const TimeZone& timeZone, ISO8601::ExactTime exactTime)
{
    if (timeZone.isUTCOffset())
        return timeZone.utcOffsetNanoseconds();
    return getNamedTimeZoneOffsetNanoseconds(timeZone.id(), exactTime);
}

int64_t TemporalZonedDateTime::offsetNanoseconds() const
{
    return getTimeZoneOffsetNanoseconds(m_timeZone, m_exactTime);
}

static int64_t floorDiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b) && ((a < 0) != (b < 0)))
        --q;
    return q;
}

static int64_t floorMod(int64_t a, int64_t b)
{
    int64_t r = a % b;
    if (r && ((a < 0) != (b < 0)))
        r += b;
    return r;
}

// https://tc39.es/proposal-temporal/#sec-temporal-getisopartsfromepoch
// Breaks epoch nanoseconds into ISO 8601 date+time components assuming a
// zero UTC offset. Algorithm is ported from temporal_rs (iso.rs,
// IsoDateTime::from_epoch_nanoseconds) and matches BalanceISODateTime in
// the spec: first split into whole milliseconds (flooring toward -inf) plus
// a sub-millisecond remainder, then derive calendar fields from the
// millisecond count.
std::pair<ISO8601::PlainDate, ISO8601::PlainTime> getISOPartsFromEpoch(ISO8601::ExactTime exactTime)
{
    static constexpr int64_t nsPerMs = 1'000'000;
    static constexpr int64_t nsPerMicro = 1'000;

    int64_t epochMs = exactTime.floorEpochMilliseconds();
    Int128 remainderNs128 = exactTime.epochNanoseconds() - Int128 { epochMs } * Int128 { nsPerMs };
    // remainder is in [0, 1e6) because floorEpochMilliseconds floors toward -inf.
    int64_t remainderNs = static_cast<int64_t>(remainderNs128);
    ASSERT(remainderNs >= 0 && remainderNs < nsPerMs);

    static constexpr int64_t msPerSecond = 1000;
    static constexpr int64_t msPerMinute = msPerSecond * 60;
    static constexpr int64_t msPerHour = msPerMinute * 60;
    static constexpr int64_t msPerDay = msPerHour * 24;

    int64_t epochDays = floorDiv(epochMs, msPerDay);
    int64_t msIntoDay = floorMod(epochMs, msPerDay);

    unsigned hour = static_cast<unsigned>(msIntoDay / msPerHour);
    unsigned minute = static_cast<unsigned>((msIntoDay / msPerMinute) % 60);
    unsigned second = static_cast<unsigned>((msIntoDay / msPerSecond) % 60);
    unsigned millisecond = static_cast<unsigned>(msIntoDay % msPerSecond);
    unsigned microsecond = static_cast<unsigned>(remainderNs / nsPerMicro);
    unsigned nanosecond = static_cast<unsigned>(remainderNs % nsPerMicro);

    // Convert day count since 1970-01-01 back into year/month/day.
    GregorianDateTime g { static_cast<double>(epochDays) * WTF::msPerDay, LocalTimeOffset { } };
    ISO8601::PlainDate date { g.year(), static_cast<unsigned>(g.month() + 1), static_cast<unsigned>(g.monthDay()) };
    ISO8601::PlainTime time { hour, minute, second, millisecond, microsecond, nanosecond };
    return { date, time };
}

// https://tc39.es/proposal-temporal/#sec-temporal-getisodatetimefor
std::pair<ISO8601::PlainDate, ISO8601::PlainTime> TemporalZonedDateTime::getISODateTimeFor(const TimeZone& timeZone, ISO8601::ExactTime exactTime)
{
    int64_t offsetNs = getTimeZoneOffsetNanoseconds(timeZone, exactTime);
    // Shift the instant by the offset and break down as if at UTC.
    // (epochNs + offsetNs) is within [minValue - nsPerDay, maxValue + nsPerDay],
    // which still fits comfortably in Int128.
    ISO8601::ExactTime local { exactTime.epochNanoseconds() + Int128 { offsetNs } };
    return getISOPartsFromEpoch(local);
}

// https://tc39.es/proposal-temporal/#sec-temporal-getpossibleepochnanoseconds
// Returns the list of exact-time candidates whose wall-clock reading in
// the given time zone equals the supplied ISO date-time. For offset zones
// there is exactly one. For named zones there can be zero (DST spring-
// forward gap), one (normal), or two (DST fall-back overlap). Algorithm is
// ported from temporal_rs time_zone.rs `get_named_tz_epoch_nanoseconds`:
// probe the offsets one day before and after the wall time interpreted as
// UTC, and keep each candidate whose round-trip offset matches.
Vector<ISO8601::ExactTime, 2> TemporalZonedDateTime::getPossibleEpochNanoseconds(const TimeZone& timeZone, ISO8601::PlainDate date, ISO8601::PlainTime time)
{
    Vector<ISO8601::ExactTime, 2> result;

    // GetUTCEpochNanoseconds(isoDateTime)
    ISO8601::ExactTime localAsUTC = ISO8601::ExactTime::fromISOPartsAndOffset(date.year(), date.month(), date.day(), time.hour(), time.minute(), time.second(), time.millisecond(), time.microsecond(), time.nanosecond(), 0);

    if (timeZone.isUTCOffset()) {
        ISO8601::ExactTime only { localAsUTC.epochNanoseconds() - Int128 { timeZone.utcOffsetNanoseconds() } };
        if (only.isValid())
            result.append(only);
        return result;
    }

    // Named zone: probe offsets around the local time. Offsets never exceed
    // a day, so +/- 1 day on either side is enough to bracket any transition.
    int64_t offsetBefore = getNamedTimeZoneOffsetNanoseconds(timeZone.id(), ISO8601::ExactTime { localAsUTC.epochNanoseconds() - ISO8601::ExactTime::nsPerDay });
    int64_t offsetAfter = getNamedTimeZoneOffsetNanoseconds(timeZone.id(), ISO8601::ExactTime { localAsUTC.epochNanoseconds() + ISO8601::ExactTime::nsPerDay });

    auto tryOffset = [&](int64_t offsetNs) {
        ISO8601::ExactTime candidate { localAsUTC.epochNanoseconds() - Int128 { offsetNs } };
        if (!candidate.isValid())
            return;
        if (getNamedTimeZoneOffsetNanoseconds(timeZone.id(), candidate) == offsetNs)
            result.append(candidate);
    };

    tryOffset(offsetBefore);
    if (offsetAfter != offsetBefore)
        tryOffset(offsetAfter);

    // Ensure ascending order (earlier first).
    if (result.size() == 2 && result[0].epochNanoseconds() > result[1].epochNanoseconds())
        std::swap(result[0], result[1]);

    return result;
}

// https://tc39.es/proposal-temporal/#sec-temporal-disambiguatepossibleepochns
std::optional<ISO8601::ExactTime> TemporalZonedDateTime::disambiguatePossibleEpochNanoseconds(JSGlobalObject* globalObject, Vector<ISO8601::ExactTime, 2>&& possible, const TimeZone& timeZone, ISO8601::PlainDate date, ISO8601::PlainTime time, TemporalDisambiguation disambiguation)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (possible.size() == 1)
        return possible[0];

    if (possible.size() > 1) {
        switch (disambiguation) {
        case TemporalDisambiguation::Compatible:
        case TemporalDisambiguation::Earlier:
            return possible.first();
        case TemporalDisambiguation::Later:
            return possible.last();
        case TemporalDisambiguation::Reject:
            throwRangeError(globalObject, scope, "ambiguous wall-clock time in time zone"_s);
            return std::nullopt;
        }
    }

    // Zero candidates: spring-forward gap.
    if (disambiguation == TemporalDisambiguation::Reject) {
        throwRangeError(globalObject, scope, "wall-clock time does not exist in time zone"_s);
        return std::nullopt;
    }

    // The gap's width equals offsetAfter - offsetBefore. Shift the local
    // time by that amount in the appropriate direction and resolve again.
    ISO8601::ExactTime localAsUTC = ISO8601::ExactTime::fromISOPartsAndOffset(date.year(), date.month(), date.day(), time.hour(), time.minute(), time.second(), time.millisecond(), time.microsecond(), time.nanosecond(), 0);
    int64_t offsetBefore = getTimeZoneOffsetNanoseconds(timeZone, ISO8601::ExactTime { localAsUTC.epochNanoseconds() - ISO8601::ExactTime::nsPerDay });
    int64_t offsetAfter = getTimeZoneOffsetNanoseconds(timeZone, ISO8601::ExactTime { localAsUTC.epochNanoseconds() + ISO8601::ExactTime::nsPerDay });
    int64_t shiftNs = offsetAfter - offsetBefore;

    if (disambiguation == TemporalDisambiguation::Earlier) {
        auto [d2, t2] = getISOPartsFromEpoch(ISO8601::ExactTime { localAsUTC.epochNanoseconds() - Int128 { shiftNs } });
        auto shifted = getPossibleEpochNanoseconds(timeZone, d2, t2);
        if (shifted.isEmpty()) {
            throwRangeError(globalObject, scope, "wall-clock time does not exist in time zone"_s);
            return std::nullopt;
        }
        return shifted.last();
    }

    // Later or Compatible.
    auto [d2, t2] = getISOPartsFromEpoch(ISO8601::ExactTime { localAsUTC.epochNanoseconds() + Int128 { shiftNs } });
    auto shifted = getPossibleEpochNanoseconds(timeZone, d2, t2);
    if (shifted.isEmpty()) {
        throwRangeError(globalObject, scope, "wall-clock time does not exist in time zone"_s);
        return std::nullopt;
    }
    return shifted.first();
}

// https://tc39.es/proposal-temporal/#sec-temporal-getepochnanosecondsfor
std::optional<ISO8601::ExactTime> TemporalZonedDateTime::getEpochNanosecondsFor(JSGlobalObject* globalObject, const TimeZone& timeZone, ISO8601::PlainDate date, ISO8601::PlainTime time, TemporalDisambiguation disambiguation)
{
    auto possible = getPossibleEpochNanoseconds(timeZone, date, time);
    return disambiguatePossibleEpochNanoseconds(globalObject, WTF::move(possible), timeZone, date, time, disambiguation);
}

std::pair<ISO8601::PlainDate, ISO8601::PlainTime> TemporalZonedDateTime::plainDateTime() const
{
    return getISODateTimeFor(m_timeZone, m_exactTime);
}

ISO8601::PlainDate TemporalZonedDateTime::plainDate() const
{
    return plainDateTime().first;
}

ISO8601::PlainTime TemporalZonedDateTime::plainTime() const
{
    return plainDateTime().second;
}

static std::optional<TimeZone> tryParseTimeZoneIdentifier(StringView string)
{
    if (auto id = ISO8601::parseTimeZoneName(string))
        return TimeZone::fromID(id.value());
    // Offset identifiers are minute-precision per spec.
    if (auto offsetMinutes = ISO8601::parseUTCOffsetInMinutes(string))
        return TimeZone::fromUTCOffset(static_cast<int64_t>(offsetMinutes.value()) * static_cast<int64_t>(ISO8601::ExactTime::nsPerMinute));
    return std::nullopt;
}

enum class TemporalOffsetOption : uint8_t { Prefer, Use, Ignore, Reject };

static TemporalDisambiguation toTemporalDisambiguation(JSGlobalObject* globalObject, JSObject* options)
{
    auto value = intlStringOption(globalObject, options, Identifier::fromString(globalObject->vm(), "disambiguation"_s), { "compatible"_s, "earlier"_s, "later"_s, "reject"_s }, "disambiguation must be \"compatible\", \"earlier\", \"later\", or \"reject\""_s, "compatible"_s);
    if (value == "earlier"_s)
        return TemporalDisambiguation::Earlier;
    if (value == "later"_s)
        return TemporalDisambiguation::Later;
    if (value == "reject"_s)
        return TemporalDisambiguation::Reject;
    return TemporalDisambiguation::Compatible;
}

static TemporalOffsetOption toTemporalOffsetOption(JSGlobalObject* globalObject, JSObject* options, TemporalOffsetOption fallback)
{
    ASCIILiteral fallbackStr = fallback == TemporalOffsetOption::Reject ? "reject"_s : "prefer"_s;
    auto value = intlStringOption(globalObject, options, Identifier::fromString(globalObject->vm(), "offset"_s), { "prefer"_s, "use"_s, "ignore"_s, "reject"_s }, "offset must be \"prefer\", \"use\", \"ignore\", or \"reject\""_s, fallbackStr);
    if (value == "use"_s)
        return TemporalOffsetOption::Use;
    if (value == "ignore"_s)
        return TemporalOffsetOption::Ignore;
    if (value == "reject"_s)
        return TemporalOffsetOption::Reject;
    return TemporalOffsetOption::Prefer;
}

// https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimezoneidentifier
// Accepts an IANA name, a "+HH:MM" offset, a ZonedDateTime (extracting its
// [[TimeZone]]), or a full ISO date-time string from which the time zone
// is extracted: bracket annotation if present, else "Z" -> UTC, else the
// numeric offset at minute precision.
std::optional<TimeZone> TemporalZonedDateTime::toTimeZoneIdentifier(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isObject()) {
        if (auto* zdt = dynamicDowncast<TemporalZonedDateTime>(value))
            return zdt->timeZone();
        throwTypeError(globalObject, scope, "time zone must be a string or a Temporal.ZonedDateTime"_s);
        return std::nullopt;
    }

    if (!value.isString()) {
        throwTypeError(globalObject, scope, "time zone must be a string"_s);
        return std::nullopt;
    }

    auto string = value.toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);

    // 1. Direct identifier.
    if (auto direct = tryParseTimeZoneIdentifier(string))
        return direct;

    // 2. Try as a date-time string and extract its time zone component.
    //    A bracketed annotation, if present, wins over any inline offset.
    StringView full { string };
    StringView prefix = full;
    if (size_t b = string.find('['); b != notFound) {
        prefix = full.left(b);
        size_t e = string.find(']', b + 1);
        if (e != notFound) {
            StringView annotation = full.substring(b + 1, e - b - 1);
            if (annotation.length() && annotation[0] == '!')
                annotation = annotation.substring(1);
            // Validate that the prefix is a syntactically valid date-time.
            if (ISO8601::parseCalendarDateTime(prefix.toString(), TemporalDateFormat::Date)) {
                if (auto tz = tryParseTimeZoneIdentifier(annotation))
                    return tz;
            }
        }
    } else if (auto dt = ISO8601::parseCalendarDateTime(string, TemporalDateFormat::Date)) {
        auto& [date, timeOpt, tzRecord, cal] = dt.value();
        if (tzRecord) {
            if (tzRecord->m_z)
                return TimeZone::fromID(utcTimeZoneID());
            if (tzRecord->m_offset) {
                int64_t offsetNs = tzRecord->m_offset.value();
                // Identifier offsets must be minute-precision.
                if (offsetNs % static_cast<int64_t>(ISO8601::ExactTime::nsPerMinute)) {
                    throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, string), "' has a sub-minute UTC offset which is not a valid time zone identifier"_s));
                    return std::nullopt;
                }
                return TimeZone::fromUTCOffset(offsetNs);
            }
        }
    }

    throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, string), "' is not a valid time zone identifier"_s));
    return std::nullopt;
}

TimeZone TemporalZonedDateTime::systemTimeZone(VM& vm)
{
    return vm.dateCache.defaultTimeZone();
}

enum class OffsetBehaviour : uint8_t { Option, Exact, Wall };
enum class MatchBehaviour : bool { Exactly, Minutes };

// https://tc39.es/proposal-temporal/#sec-temporal-interpretisodatetimeoffset
static std::optional<ISO8601::ExactTime> interpretISODateTimeOffset(JSGlobalObject* globalObject, ISO8601::PlainDate date, ISO8601::PlainTime time, OffsetBehaviour offsetBehaviour, int64_t offsetNs, const TimeZone& timeZone, TemporalDisambiguation disambiguation, TemporalOffsetOption offsetOption, MatchBehaviour matchBehaviour)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // 1. If offsetBehaviour is "wall", or offsetOption is "ignore":
    //    Return GetEpochNanosecondsFor(timeZone, isoDateTime, disambiguation).
    if (offsetBehaviour == OffsetBehaviour::Wall || offsetOption == TemporalOffsetOption::Ignore)
        RELEASE_AND_RETURN(scope, TemporalZonedDateTime::getEpochNanosecondsFor(globalObject, timeZone, date, time, disambiguation));

    // 2. If offsetBehaviour is "exact", or offsetOption is "use":
    //    Return GetUTCEpochNanoseconds(isoDateTime) - offsetNs.
    if (offsetBehaviour == OffsetBehaviour::Exact || offsetOption == TemporalOffsetOption::Use) {
        auto exact = ISO8601::ExactTime::fromISOPartsAndOffset(date.year(), date.month(), date.day(), time.hour(), time.minute(), time.second(), time.millisecond(), time.microsecond(), time.nanosecond(), offsetNs);
        if (!exact.isValid()) {
            throwRangeError(globalObject, scope, "date-time is outside the representable range"_s);
            return std::nullopt;
        }
        return exact;
    }

    // 3. offsetBehaviour == "option" and offsetOption is "prefer"/"reject".
    auto possible = TemporalZonedDateTime::getPossibleEpochNanoseconds(timeZone, date, time);
    static constexpr int64_t nsPerMinute = static_cast<int64_t>(ISO8601::ExactTime::nsPerMinute);
    for (auto& candidate : possible) {
        int64_t candidateOffset = TemporalZonedDateTime::getTimeZoneOffsetNanoseconds(timeZone, candidate);
        if (candidateOffset == offsetNs)
            return candidate;
        if (matchBehaviour == MatchBehaviour::Minutes) {
            // RoundNumberToIncrement(candidateOffset, 60e9, halfExpand)
            int64_t absCand = candidateOffset < 0 ? -candidateOffset : candidateOffset;
            int64_t roundedAbs = ((absCand + nsPerMinute / 2) / nsPerMinute) * nsPerMinute;
            int64_t rounded = candidateOffset < 0 ? -roundedAbs : roundedAbs;
            if (rounded == offsetNs)
                return candidate;
        }
    }

    if (offsetOption == TemporalOffsetOption::Reject) {
        throwRangeError(globalObject, scope, "provided UTC offset does not match the time zone"_s);
        return std::nullopt;
    }

    // "prefer": fall through to disambiguation.
    RELEASE_AND_RETURN(scope, TemporalZonedDateTime::disambiguatePossibleEpochNanoseconds(globalObject, WTF::move(possible), timeZone, date, time, disambiguation));
}

// https://tc39.es/proposal-temporal/#sec-temporal-totemporalzoneddatetime
TemporalZonedDateTime* TemporalZonedDateTime::toZonedDateTime(JSGlobalObject* globalObject, JSValue itemValue, JSValue optionsValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* options = intlGetOptionsObject(globalObject, optionsValue);
    RETURN_IF_EXCEPTION(scope, { });

    auto readResolvedOptions = [&](TemporalOffsetOption offsetFallback, TemporalDisambiguation& disambiguation, TemporalOffsetOption& offsetOption) -> bool {
        if (!options) {
            disambiguation = TemporalDisambiguation::Compatible;
            offsetOption = offsetFallback;
            return true;
        }
        disambiguation = toTemporalDisambiguation(globalObject, options);
        RETURN_IF_EXCEPTION(scope, false);
        offsetOption = toTemporalOffsetOption(globalObject, options, offsetFallback);
        RETURN_IF_EXCEPTION(scope, false);
        toTemporalOverflow(globalObject, options);
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    };

    if (itemValue.isObject()) {
        if (auto* zdt = dynamicDowncast<TemporalZonedDateTime>(itemValue)) {
            TemporalDisambiguation disambiguation; TemporalOffsetOption offsetOption;
            if (!readResolvedOptions(TemporalOffsetOption::Reject, disambiguation, offsetOption))
                return { };
            return create(vm, globalObject->zonedDateTimeStructure(), zdt->exactTime(), zdt->timeZone());
        }

        JSObject* item = asObject(itemValue);

        // --- Property bag: calendar -------------------------------------
        JSValue calendarValue = item->get(globalObject, vm.propertyNames->calendar);
        RETURN_IF_EXCEPTION(scope, { });
        if (!calendarValue.isUndefined()) {
            if (!calendarValue.isString()) {
                throwTypeError(globalObject, scope, "calendar must be a string"_s);
                return { };
            }
            auto calendarString = calendarValue.toWTFString(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            auto cid = TemporalCalendar::isBuiltinCalendar(calendarString);
            if (!cid) {
                throwRangeError(globalObject, scope, "invalid calendar"_s);
                return { };
            }
            if (cid.value() != iso8601CalendarID()) {
                throwRangeError(globalObject, scope, "only the iso8601 calendar is currently supported"_s);
                return { };
            }
        }

        // --- Property bag: date fields (year, month, monthCode, day) ----
        TemporalOverflow overflow = TemporalOverflow::Constrain;
        auto plainDate = TemporalCalendar::isoDateFromFields(globalObject, item, TemporalDateFormat::Date, overflow, overflow);
        RETURN_IF_EXCEPTION(scope, { });

        // --- Property bag: time fields (optional) ------------------------
        constexpr bool skipRelevantPropertyCheck = true;
        auto timeDuration = TemporalPlainTime::toTemporalTimeRecord(globalObject, item, skipRelevantPropertyCheck);
        RETURN_IF_EXCEPTION(scope, { });
        auto plainTime = TemporalPlainTime::regulateTime(globalObject, WTF::move(timeDuration), TemporalOverflow::Constrain);
        RETURN_IF_EXCEPTION(scope, { });

        // --- Property bag: timeZone (required) ---------------------------
        JSValue tzValue = item->get(globalObject, vm.propertyNames->timeZone);
        RETURN_IF_EXCEPTION(scope, { });
        if (tzValue.isUndefined()) {
            throwTypeError(globalObject, scope, "timeZone is required in property bag"_s);
            return { };
        }
        auto timeZone = toTimeZoneIdentifier(globalObject, tzValue);
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT(timeZone);

        // --- Property bag: offset (optional) -----------------------------
        OffsetBehaviour offsetBehaviour = OffsetBehaviour::Option;
        int64_t offsetNs = 0;
        JSValue offsetValue = item->get(globalObject, Identifier::fromString(vm, "offset"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (offsetValue.isUndefined())
            offsetBehaviour = OffsetBehaviour::Wall;
        else {
            if (!offsetValue.isString()) {
                throwTypeError(globalObject, scope, "offset must be a string"_s);
                return { };
            }
            auto offsetString = offsetValue.toWTFString(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            auto parsed = ISO8601::parseUTCOffset(offsetString);
            if (!parsed) {
                throwRangeError(globalObject, scope, "invalid UTC offset string"_s);
                return { };
            }
            offsetNs = parsed.value();
        }

        TemporalDisambiguation disambiguation; TemporalOffsetOption offsetOption;
        if (!readResolvedOptions(TemporalOffsetOption::Reject, disambiguation, offsetOption))
            return { };

        auto exact = interpretISODateTimeOffset(globalObject, plainDate, plainTime, offsetBehaviour, offsetNs, timeZone.value(), disambiguation, offsetOption, MatchBehaviour::Exactly);
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT(exact);
        RELEASE_AND_RETURN(scope, tryCreateIfValid(globalObject, exact.value(), timeZone.value()));
    }

    if (!itemValue.isString()) {
        throwTypeError(globalObject, scope, "can only convert to ZonedDateTime from object or string values"_s);
        return { };
    }

    auto string = itemValue.toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    // Find the bracketed time zone annotation (required).
    size_t bracketStart = string.find('[');
    if (bracketStart == notFound) {
        throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, string), "' is not a valid Temporal.ZonedDateTime string: time zone annotation is required"_s));
        return { };
    }

    StringView full { string };
    StringView prefix = full.left(bracketStart);
    size_t bracketEnd = string.find(']', bracketStart + 1);
    if (bracketEnd == notFound) {
        throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, string), "' is not a valid Temporal.ZonedDateTime string: unterminated time zone annotation"_s));
        return { };
    }
    StringView annotation = full.substring(bracketStart + 1, bracketEnd - bracketStart - 1);
    if (annotation.length() && annotation[0] == '!')
        annotation = annotation.substring(1);

    auto timeZone = tryParseTimeZoneIdentifier(annotation);
    if (!timeZone) {
        throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, annotation.toString()), "' is not a valid time zone in ZonedDateTime string"_s));
        return { };
    }

    auto dateTime = ISO8601::parseCalendarDateTime(prefix.toString(), TemporalDateFormat::Date);
    if (!dateTime) {
        throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, string), "' is not a valid Temporal.ZonedDateTime string"_s));
        return { };
    }
    auto& [plainDate, plainTimeOptional, tzRecord, calendarOptional] = dateTime.value();
    ISO8601::PlainTime plainTime = plainTimeOptional.value_or(ISO8601::PlainTime());

    OffsetBehaviour offsetBehaviour = OffsetBehaviour::Wall;
    int64_t offsetNs = 0;
    if (tzRecord) {
        if (tzRecord->m_z)
            offsetBehaviour = OffsetBehaviour::Exact; // offsetNs = 0
        else if (tzRecord->m_offset) {
            offsetBehaviour = OffsetBehaviour::Option;
            offsetNs = tzRecord->m_offset.value();
        }
    }

    TemporalDisambiguation disambiguation; TemporalOffsetOption offsetOption;
    if (!readResolvedOptions(TemporalOffsetOption::Reject, disambiguation, offsetOption))
        return { };

    auto exact = interpretISODateTimeOffset(globalObject, plainDate, plainTime, offsetBehaviour, offsetNs, timeZone.value(), disambiguation, offsetOption, MatchBehaviour::Minutes);
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT(exact);
    RELEASE_AND_RETURN(scope, tryCreateIfValid(globalObject, exact.value(), timeZone.value()));
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.from
TemporalZonedDateTime* TemporalZonedDateTime::from(JSGlobalObject* globalObject, JSValue itemValue, JSValue optionsValue)
{
    return toZonedDateTime(globalObject, itemValue, optionsValue);
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.compare
JSValue TemporalZonedDateTime::compare(JSGlobalObject* globalObject, JSValue oneValue, JSValue twoValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* one = toZonedDateTime(globalObject, oneValue, jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    auto* two = toZonedDateTime(globalObject, twoValue, jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });

    if (one->exactTime() > two->exactTime())
        return jsNumber(1);
    if (one->exactTime() < two->exactTime())
        return jsNumber(-1);
    return jsNumber(0);
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.equals
bool TemporalZonedDateTime::equals(JSGlobalObject* globalObject, TemporalZonedDateTime* other)
{
    if (m_exactTime != other->exactTime())
        return false;
    if (!(m_timeZone == other->timeZone()))
        return false;
    return calendar()->equals(globalObject, other->calendar());
}

// FormatOffsetTimeZoneIdentifier: "+HH:MM" (minute precision, per spec for
// the offset portion that appears before the bracket).
static void appendRoundedOffset(StringBuilder& builder, int64_t offsetNs)
{
    static constexpr int64_t nsPerMinute = static_cast<int64_t>(ISO8601::ExactTime::nsPerMinute);
    // RoundNumberToIncrement(offsetNs, 60e9, halfExpand)
    int64_t absOffset = offsetNs < 0 ? -offsetNs : offsetNs;
    int64_t minutes = (absOffset + nsPerMinute / 2) / nsPerMinute;
    int64_t hours = minutes / 60;
    minutes = minutes % 60;
    builder.append(offsetNs < 0 ? '-' : '+');
    builder.append(pad('0', 2, hours));
    builder.append(':');
    builder.append(pad('0', 2, minutes));
}

// https://tc39.es/proposal-temporal/#sec-temporal-temporalzoneddatetimetostring
String TemporalZonedDateTime::toString(std::tuple<Precision, unsigned> precision, bool showOffset, bool showTimeZone, bool showCalendar) const
{
    auto [date, time] = plainDateTime();
    StringBuilder builder;
    builder.append(ISO8601::temporalDateTimeToString(date, time, precision));
    if (showOffset)
        appendRoundedOffset(builder, offsetNanoseconds());
    if (showTimeZone) {
        builder.append('[');
        builder.append(m_timeZone.toString());
        builder.append(']');
    }
    if (showCalendar)
        builder.append("[u-ca=iso8601]"_s);
    return builder.toString();
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tostring
String TemporalZonedDateTime::toString(JSGlobalObject* globalObject, JSValue optionsValue) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* options = intlGetOptionsObject(globalObject, optionsValue);
    RETURN_IF_EXCEPTION(scope, { });

    if (!options)
        return toString();

    // Read options in alphabetical order per GetTemporalShowCalendarNameOption
    // et al.: calendarName, fractionalSecondDigits, offset, roundingMode,
    // smallestUnit, timeZoneName.
    String showCalendar = intlStringOption(globalObject, options, vm.propertyNames->calendarName, { "auto"_s, "always"_s, "never"_s, "critical"_s }, "calendarName must be \"auto\", \"always\", \"never\", or \"critical\""_s, "auto"_s);
    RETURN_IF_EXCEPTION(scope, { });

    auto fractionalDigits = temporalFractionalSecondDigits(globalObject, options);
    RETURN_IF_EXCEPTION(scope, { });

    String showOffset = intlStringOption(globalObject, options, Identifier::fromString(vm, "offset"_s), { "auto"_s, "never"_s }, "offset must be \"auto\" or \"never\""_s, "auto"_s);
    RETURN_IF_EXCEPTION(scope, { });

    auto roundingMode = temporalRoundingMode(globalObject, options, RoundingMode::Trunc);
    RETURN_IF_EXCEPTION(scope, { });

    auto smallestUnit = getTemporalUnitValuedOption(globalObject, options, vm.propertyNames->smallestUnit);
    RETURN_IF_EXCEPTION(scope, { });

    String showTimeZone = intlStringOption(globalObject, options, vm.propertyNames->timeZoneName, { "auto"_s, "never"_s, "critical"_s }, "timeZoneName must be \"auto\", \"never\", or \"critical\""_s, "auto"_s);
    RETURN_IF_EXCEPTION(scope, { });

    // Resolve precision from smallestUnit + fractionalSecondDigits (what
    // secondsStringPrecision() does, but it reads options again and we need
    // strict ordering; so reconstruct here).
    PrecisionData data { { Precision::Auto, 0 }, TemporalUnit::Nanosecond, 1 };
    if (auto* unitp = std::get_if<std::optional<TemporalUnit>>(&smallestUnit); unitp && unitp->has_value()) {
        TemporalUnit unit = unitp->value();
        if (unit < TemporalUnit::Minute) {
            throwRangeError(globalObject, scope, "smallestUnit must be a time unit"_s);
            return { };
        }
        switch (unit) {
        case TemporalUnit::Minute: data = { { Precision::Minute, 0 }, TemporalUnit::Minute, 1 }; break;
        case TemporalUnit::Second: data = { { Precision::Fixed, 0 }, TemporalUnit::Second, 1 }; break;
        case TemporalUnit::Millisecond: data = { { Precision::Fixed, 3 }, TemporalUnit::Millisecond, 1 }; break;
        case TemporalUnit::Microsecond: data = { { Precision::Fixed, 6 }, TemporalUnit::Microsecond, 1 }; break;
        case TemporalUnit::Nanosecond: data = { { Precision::Fixed, 9 }, TemporalUnit::Nanosecond, 1 }; break;
        default: break;
        }
    } else if (fractionalDigits) {
        unsigned d = fractionalDigits.value();
        if (!d) data = { { Precision::Fixed, 0 }, TemporalUnit::Second, 1 };
        else if (d <= 3) data = { { Precision::Fixed, d }, TemporalUnit::Millisecond, static_cast<unsigned>(std::pow(10, 3 - d)) };
        else if (d <= 6) data = { { Precision::Fixed, d }, TemporalUnit::Microsecond, static_cast<unsigned>(std::pow(10, 6 - d)) };
        else data = { { Precision::Fixed, d }, TemporalUnit::Nanosecond, static_cast<unsigned>(std::pow(10, 9 - d)) };
    }

    ISO8601::ExactTime rounded = m_exactTime;
    if (!(std::get<0>(data.precision) == Precision::Auto && roundingMode == RoundingMode::Trunc)) {
        rounded = m_exactTime.round(globalObject, data.increment, data.unit, roundingMode);
        RETURN_IF_EXCEPTION(scope, { });
    }

    auto [date, time] = getISODateTimeFor(m_timeZone, rounded);
    StringBuilder builder;
    builder.append(ISO8601::temporalDateTimeToString(date, time, data.precision));
    if (showOffset != "never"_s)
        appendRoundedOffset(builder, getTimeZoneOffsetNanoseconds(m_timeZone, rounded));
    if (showTimeZone != "never"_s) {
        builder.append('[');
        if (showTimeZone == "critical"_s)
            builder.append('!');
        builder.append(m_timeZone.toString());
        builder.append(']');
    }
    if (showCalendar == "always"_s || showCalendar == "critical"_s) {
        builder.append('[');
        if (showCalendar == "critical"_s)
            builder.append('!');
        builder.append("u-ca=iso8601]"_s);
    }
    return builder.toString();
}

} // namespace JSC
