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
#include "VMTrapsInlines.h"
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

// https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimezoneidentifier
// Simplified: accepts a string that is either an IANA name or a UTC offset,
// or an object that is a ZonedDateTime (use its [[TimeZone]]).
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

    if (auto id = ISO8601::parseTimeZoneName(string))
        return TimeZone::fromID(id.value());

    // Offset time zones must be at minute precision per the current spec
    // (ParseTimeZoneIdentifier). Accept "+HH:MM" / "-HH:MM" only.
    if (auto offsetMinutes = ISO8601::parseUTCOffsetInMinutes(string))
        return TimeZone::fromUTCOffset(static_cast<int64_t>(offsetMinutes.value()) * static_cast<int64_t>(ISO8601::ExactTime::nsPerMinute));

    throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, string), "' is not a valid time zone identifier"_s));
    return std::nullopt;
}

TimeZone TemporalZonedDateTime::systemTimeZone(VM& vm)
{
    return vm.dateCache.defaultTimeZone();
}

// https://tc39.es/proposal-temporal/#sec-temporal-totemporalzoneddatetime
// Initial version: supports an existing ZonedDateTime, or a string in the
// form "YYYY-MM-DDTHH:MM:SS[.fraction](Z|±HH:MM)[TimeZone]".
TemporalZonedDateTime* TemporalZonedDateTime::toZonedDateTime(JSGlobalObject* globalObject, JSValue itemValue, JSValue optionsValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (itemValue.isObject()) {
        if (auto* zdt = dynamicDowncast<TemporalZonedDateTime>(itemValue)) {
            JSObject* options = intlGetOptionsObject(globalObject, optionsValue);
            RETURN_IF_EXCEPTION(scope, { });
            if (options) {
                toTemporalOverflow(globalObject, options);
                RETURN_IF_EXCEPTION(scope, { });
            }
            return create(vm, globalObject->zonedDateTimeStructure(), zdt->exactTime(), zdt->timeZone());
        }

        // Full property-bag conversion (with disambiguation + offset options)
        // depends on GetPossibleEpochNanoseconds; implemented in a follow-up.
        throwRangeError(globalObject, scope, "Temporal.ZonedDateTime.from does not yet support property bags; pass a string or a ZonedDateTime"_s);
        return { };
    }

    if (!itemValue.isString()) {
        throwTypeError(globalObject, scope, "can only convert to ZonedDateTime from object or string values"_s);
        return { };
    }

    auto string = itemValue.toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    JSObject* options = intlGetOptionsObject(globalObject, optionsValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (options) {
        toTemporalOverflow(globalObject, options);
        RETURN_IF_EXCEPTION(scope, { });
    }

    // Find the bracketed time zone annotation. ParseTemporalZonedDateTimeString
    // requires a time zone annotation to be present.
    size_t bracketStart = string.find('[');
    if (bracketStart == notFound) {
        throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, string), "' is not a valid Temporal.ZonedDateTime string: time zone annotation is required"_s));
        return { };
    }

    // Parse the date-time + offset portion before the annotation using the
    // existing Instant parser so we reuse its validation and offset handling.
    StringView full { string };
    StringView prefix = full.left(bracketStart);

    // Extract the time zone annotation (first bracket pair). Calendar
    // annotations like "[u-ca=...]" are tolerated but currently only ISO is
    // supported by the rest of the implementation.
    size_t bracketEnd = string.find(']', bracketStart + 1);
    if (bracketEnd == notFound) {
        throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, string), "' is not a valid Temporal.ZonedDateTime string: unterminated time zone annotation"_s));
        return { };
    }
    StringView annotation = full.substring(bracketStart + 1, bracketEnd - bracketStart - 1);
    bool critical = annotation.length() && annotation[0] == '!';
    if (critical)
        annotation = annotation.substring(1);

    std::optional<TimeZone> timeZone;
    if (auto id = ISO8601::parseTimeZoneName(annotation))
        timeZone = TimeZone::fromID(id.value());
    else if (auto offsetMinutes = ISO8601::parseUTCOffsetInMinutes(annotation))
        timeZone = TimeZone::fromUTCOffset(static_cast<int64_t>(offsetMinutes.value()) * static_cast<int64_t>(ISO8601::ExactTime::nsPerMinute));

    if (!timeZone) {
        throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, annotation.toString()), "' is not a valid time zone in ZonedDateTime string"_s));
        return { };
    }

    // If there is an explicit UTC designator or offset in the prefix, the
    // Instant parser will compute exact epoch nanoseconds directly.
    if (auto parsedExact = ISO8601::parseInstant(prefix.toString())) {
        RELEASE_AND_RETURN(scope, tryCreateIfValid(globalObject, parsedExact.value(), timeZone.value()));
    }

    // Otherwise the prefix is a bare date-time; interpret it in the given
    // time zone. For offset time zones this is exact. For named zones we
    // use the "compatible" disambiguation (UCAL_TZ_LOCAL_FORMER) like
    // legacy Date, which matches the spec default.
    auto dateTime = ISO8601::parseCalendarDateTime(prefix.toString(), TemporalDateFormat::Date);
    if (!dateTime) {
        throwRangeError(globalObject, scope, makeString("'"_s, ellipsizeAt(100, string), "' is not a valid Temporal.ZonedDateTime string"_s));
        return { };
    }
    auto& [plainDate, plainTimeOptional, timeZoneOptional, calendarOptional] = dateTime.value();
    if (timeZoneOptional && timeZoneOptional->m_z) {
        // "Z" with a bracket annotation means UTC instant in that zone.
        auto exact = ISO8601::ExactTime::fromISOPartsAndOffset(plainDate.year(), plainDate.month(), plainDate.day(), 0, 0, 0, 0, 0, 0, 0);
        RELEASE_AND_RETURN(scope, tryCreateIfValid(globalObject, exact, timeZone.value()));
    }

    ISO8601::PlainTime plainTime = plainTimeOptional.value_or(ISO8601::PlainTime());
    int64_t offsetNs;
    if (timeZone->isUTCOffset())
        offsetNs = timeZone->utcOffsetNanoseconds();
    else {
        // Evaluate offset at the approximate UTC position of this wall-clock
        // time, which is the "compatible" behavior ICU provides.
        auto approx = ISO8601::ExactTime::fromISOPartsAndOffset(plainDate.year(), plainDate.month(), plainDate.day(), plainTime.hour(), plainTime.minute(), plainTime.second(), plainTime.millisecond(), plainTime.microsecond(), plainTime.nanosecond(), 0);
        offsetNs = getNamedTimeZoneOffsetNanoseconds(timeZone->id(), approx);
    }

    auto exact = ISO8601::ExactTime::fromISOPartsAndOffset(plainDate.year(), plainDate.month(), plainDate.day(), plainTime.hour(), plainTime.minute(), plainTime.second(), plainTime.millisecond(), plainTime.microsecond(), plainTime.nanosecond(), offsetNs);
    RELEASE_AND_RETURN(scope, tryCreateIfValid(globalObject, exact, timeZone.value()));
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

    PrecisionData data = secondsStringPrecision(globalObject, options);
    RETURN_IF_EXCEPTION(scope, { });

    auto roundingMode = temporalRoundingMode(globalObject, options, RoundingMode::Trunc);
    RETURN_IF_EXCEPTION(scope, { });

    String showCalendar = intlStringOption(globalObject, options, vm.propertyNames->calendarName, { "auto"_s, "always"_s, "never"_s, "critical"_s }, "calendarName must be \"auto\", \"always\", \"never\", or \"critical\""_s, "auto"_s);
    RETURN_IF_EXCEPTION(scope, { });

    String showTimeZone = intlStringOption(globalObject, options, vm.propertyNames->timeZoneName, { "auto"_s, "never"_s, "critical"_s }, "timeZoneName must be \"auto\", \"never\", or \"critical\""_s, "auto"_s);
    RETURN_IF_EXCEPTION(scope, { });

    String showOffset = intlStringOption(globalObject, options, Identifier::fromString(vm, "offset"_s), { "auto"_s, "never"_s }, "offset must be \"auto\" or \"never\""_s, "auto"_s);
    RETURN_IF_EXCEPTION(scope, { });

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
