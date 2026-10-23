/*
 * Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 * Copyright (C) 2006-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2009 Google Inc. All rights reserved.
 * Copyright (C) 2007-2009 Torch Mobile, Inc.
 * Copyright (C) 2010 &yet, LLC. (nate@andyet.net)
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Alternatively, the contents of this file may be used under the terms
 * of either the Mozilla Public License Version 1.1, found at
 * http://www.mozilla.org/MPL/ (the "MPL") or the GNU General Public
 * License Version 2.0, found at http://www.fsf.org/copyleft/gpl.html
 * (the "GPL"), in which case the provisions of the MPL or the GPL are
 * applicable instead of those above.  If you wish to allow use of your
 * version of this file only under the terms of one of those two
 * licenses (the MPL or the GPL) and not to allow others to use your
 * version of this file under the LGPL, indicate your decision by
 * deletingthe provisions above and replace them with the notice and
 * other provisions required by the MPL or the GPL, as the case may be.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under any of the LGPL, the MPL or the GPL.

 * Copyright 2006-2012 the V8 project authors. All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Google Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSDateMath.h"
#include "JSDateMath-v8.h"

#include "ExceptionHelpers.h"
#include "ISO8601.h"
#include "IntlObject.h"
#include "Lexer.h"
#include "VM.h"
#include <limits>
#include <wtf/DateMath.h>
#include <wtf/Language.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/TimeZone.h>
#include <wtf/text/MakeString.h>
#include <wtf/unicode/CharacterNames.h>
#include <wtf/unicode/icu/ICUHelpers.h>

#if OS(UNIX)
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef U_HIDE_DRAFT_API
#undef U_HIDE_DRAFT_API
#endif
#include <unicode/ucal.h>
#define U_HIDE_DRAFT_API 1

namespace JSC {

namespace JSDateMathInternal {
static constexpr bool verbose = false;

static bool canNarrowToInt64Milliseconds(double milliseconds)
{
    return std::isfinite(milliseconds) && std::abs(milliseconds) <= WTF::maxECMAScriptTime + WTF::msPerDay;
}
}

class OpaqueICUTimeZone {
    WTF_MAKE_TZONE_ALLOCATED(OpaqueICUTimeZone);
public:
    std::unique_ptr<UCalendar, ICUDeleter<ucal_close>> m_calendar;
    TimeZone m_canonicalTimeZone;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(OpaqueICUTimeZone);

void OpaqueICUTimeZoneDeleter::operator()(OpaqueICUTimeZone* timeZone)
{
    if (timeZone)
        delete timeZone;
}

// Get the combined UTC + DST offset for the time passed in.
//
// NOTE: The implementation relies on the fact that no time zones have
// more than one daylight savings offset change per month.
// If this function is called with NaN it returns random value.
LocalTimeOffset DateCache::calculateLocalTimeOffset(double millisecondsFromEpoch, TimeType inputTimeType)
{
    int32_t rawOffset = 0;
    int32_t dstOffset = 0;
    UErrorCode status = U_ZERO_ERROR;

    // This function can fail input date is invalid: NaN etc.
    // We can return any values in this case since later we fail when computing non timezone offset part anyway.
    constexpr LocalTimeOffset failed { false, 0 };

    auto& timeZoneCache = *this->timeZoneCache();
    ucal_setMillis(timeZoneCache.m_calendar.get(), millisecondsFromEpoch, &status);
    if (U_FAILURE(status))
        return failed;

    if (inputTimeType != TimeType::LocalTime) {
        rawOffset = ucal_get(timeZoneCache.m_calendar.get(), UCAL_ZONE_OFFSET, &status);
        if (U_FAILURE(status))
            return failed;
        dstOffset = ucal_get(timeZoneCache.m_calendar.get(), UCAL_DST_OFFSET, &status);
        if (U_FAILURE(status))
            return failed;
    } else {
        ucal_getTimeZoneOffsetFromLocal(timeZoneCache.m_calendar.get(), UCAL_TZ_LOCAL_FORMER, UCAL_TZ_LOCAL_FORMER, &rawOffset, &dstOffset, &status);
        if (U_FAILURE(status))
            return failed;
    }

    return { !!dstOffset, rawOffset + dstOffset };
}

LocalTimeOffsetCache* DateCache::DSTCache::leastRecentlyUsed(LocalTimeOffsetCache* exclude)
{
    LocalTimeOffsetCache* result = nullptr;
    for (auto& cache : m_entries) {
        if (&cache == exclude)
            continue;
        if (!result) {
            result = &cache;
            continue;
        }
        if (result->epoch > cache.epoch)
            result = &cache;
    }
    *result = LocalTimeOffsetCache {};
    return result;
}

std::tuple<LocalTimeOffsetCache*, LocalTimeOffsetCache*> DateCache::DSTCache::probe(int64_t millisecondsFromEpoch)
{
    LocalTimeOffsetCache* before = nullptr;
    LocalTimeOffsetCache* after = nullptr;
    for (auto& cache : m_entries) {
        if (cache.start <= millisecondsFromEpoch) {
            if (!before || before->start < cache.start)
                before = &cache;
        } else if (millisecondsFromEpoch < cache.end) {
            if (!after || after->end > cache.end)
                after = &cache;
        }
    }

    if (!before) {
        if (m_before->isEmpty())
            before = m_before;
        else
            before = leastRecentlyUsed(after);
    }
    if (!after) {
        if (m_after->isEmpty() && before != m_after)
            after = m_after;
        else
            after = leastRecentlyUsed(before);
    }

    m_before = before;
    m_after = after;
    return std::tuple { before, after };
}

void DateCache::DSTCache::extendTheAfterCache(int64_t millisecondsFromEpoch, LocalTimeOffset offset)
{
    if (m_after->offset == offset && m_after->start - defaultDSTDeltaInMilliseconds <= millisecondsFromEpoch && millisecondsFromEpoch <= m_after->end) {
        // Extend the m_after cache.
        m_after->start = millisecondsFromEpoch;
        dataLogLnIf(JSDateMathInternal::verbose, "Cache extended1 from ", millisecondsFromEpoch, " to ", m_after->end, " ", offset.offset, " ", offset.isDST);
    } else {
        // The m_after cache is either invalid or starts too late.
        if (!m_after->isEmpty()) {
            // If the m_after cache is valid, replace it with a new cache.
            m_after = leastRecentlyUsed(m_before);
        }
        m_after->start = millisecondsFromEpoch;
        m_after->end = millisecondsFromEpoch;
        m_after->offset = offset;
        m_after->epoch = bumpEpoch();
        dataLogLnIf(JSDateMathInternal::verbose, "Cache miss, recompute2 ", millisecondsFromEpoch, " ", offset.offset, " ", offset.isDST);
    }
}

LocalTimeOffset DateCache::DSTCache::localTimeOffset(DateCache& dateCache, int64_t millisecondsFromEpoch, TimeType inputTimeType)
{
    if (millisecondsFromEpoch >= WTF::Int64Milliseconds::minECMAScriptTime && millisecondsFromEpoch <= WTF::Int64Milliseconds::maxECMAScriptTime) {
        // Do nothing. Use millisecondsFromEpoch directly
    } else {
        // Adjust to equivalent time.
        int64_t newTime = WTF::equivalentTime(millisecondsFromEpoch);
        dataLogLnIf(JSDateMathInternal::verbose, "Equivalent time conversion from ", millisecondsFromEpoch, " to ", newTime);
        millisecondsFromEpoch = newTime;
    }

    if (m_epoch > UINT32_MAX) [[unlikely]] {
        dataLogLnIf(JSDateMathInternal::verbose, "reset DSTCache");
        reset();
    }

    // If the time fits in the cached interval in the last cache hit, return the cached offset.
    if (m_before->start <= millisecondsFromEpoch && millisecondsFromEpoch <= m_before->end) {
        dataLogLnIf(JSDateMathInternal::verbose, "Previous Cache Hit");
        m_before->epoch = bumpEpoch();
        return m_before->offset;
    }

    probe(millisecondsFromEpoch);

    ASSERT(m_before->isEmpty() || m_before->start <= millisecondsFromEpoch);
    ASSERT(m_after->isEmpty() || millisecondsFromEpoch < m_after->start);

    if (m_before->isEmpty()) {
        // Cache miss!
        // Compute the DST offset for the time and shrink the cache interval
        // to only contain the time. This allows fast repeated DST offset
        // computations for the same time.
        LocalTimeOffset offset = dateCache.calculateLocalTimeOffset(millisecondsFromEpoch, inputTimeType);
        m_before->offset = offset;
        m_before->start = millisecondsFromEpoch;
        m_before->end = millisecondsFromEpoch;
        m_before->epoch = bumpEpoch();
        dataLogLnIf(JSDateMathInternal::verbose, "Cache miss, recompute1 ", millisecondsFromEpoch, " ", offset.offset, " ", offset.isDST);
        return offset;
    }

    // Cache hit!
    // If the time fits in the cached interval, return the cached offset.
    if (millisecondsFromEpoch <= m_before->end) {
        dataLogLnIf(JSDateMathInternal::verbose, "Cache hit, simple ", millisecondsFromEpoch, " ", m_before->offset.offset, " ", m_before->offset.isDST);
        m_before->epoch = bumpEpoch();
        return m_before->offset;
    }

    if ((millisecondsFromEpoch - defaultDSTDeltaInMilliseconds) > m_before->end) {
        LocalTimeOffset offset = dateCache.calculateLocalTimeOffset(millisecondsFromEpoch, inputTimeType);
        extendTheAfterCache(millisecondsFromEpoch, offset);
        std::swap(m_before, m_after);
        dataLogLnIf(JSDateMathInternal::verbose, "Cache hit, extend ", millisecondsFromEpoch, " ", offset.offset, " ", offset.isDST);
        return offset;
    }

    m_before->epoch = bumpEpoch();

    // Check if m_after is invalid or starts too late.
    // Note that start of invalid caches is maxECMAScriptTime.
    int64_t newAfterStart = m_before->end < WTF::Int64Milliseconds::maxECMAScriptTime - defaultDSTDeltaInMilliseconds ? m_before->end + defaultDSTDeltaInMilliseconds : WTF::Int64Milliseconds::maxECMAScriptTime;
    if (newAfterStart <= m_after->start) {
        LocalTimeOffset offset = dateCache.calculateLocalTimeOffset(newAfterStart, inputTimeType);
        extendTheAfterCache(newAfterStart, offset);
    } else {
        // Update the usage counter of m_after since it is going to be used.
        ASSERT(!m_after->isEmpty());
        m_after->epoch = bumpEpoch();
    }

    // Now the millisecondsFromEpoch is between m_before->end and m_after->start.
    // Only one daylight savings offset change can occur in this interval.

    if (m_before->offset == m_after->offset) {
        // Merge two caches if they have the same offset.
        m_before->end = m_after->end;
        *m_after = LocalTimeOffsetCache {};
        return m_before->offset;
    }

    // Binary search for daylight savings offset change point,
    // but give up if we don't find it in five iterations.
    for (int i = 4; i >= 0; --i) {
        int64_t delta = m_after->start - m_before->end;
        int64_t middle = !i ? millisecondsFromEpoch : m_before->end + delta / 2;
        LocalTimeOffset offset = dateCache.calculateLocalTimeOffset(middle, inputTimeType);
        if (m_before->offset == offset) {
            m_before->end = middle;
            dataLogLnIf(JSDateMathInternal::verbose, "Cache extended2 from ", m_before->start, " to ", m_before->end, " ", offset.offset, " ", offset.isDST);
            if (millisecondsFromEpoch <= m_before->end)
                return offset;
        } else {
            ASSERT(m_after->offset == offset);
            m_after->start = middle;
            dataLogLnIf(JSDateMathInternal::verbose, "Cache extended3 from ", m_after->start, " to ", m_after->end, " ", offset.offset, " ", offset.isDST);
            if (millisecondsFromEpoch >= m_after->start) {
                // This swap helps the optimistic fast check in subsequent invocations.
                std::swap(m_before, m_after);
                return offset;
            }
        }
    }

    return {};
}

double DateCache::gregorianDateTimeToMS(int32_t year, int32_t month, int32_t monthDay, int32_t hour, int32_t minute, int32_t second, double milliseconds, TimeType inputTimeType)
{
    double day = dateToDaysFrom1970(year, month, monthDay);
    double ms = timeToMS(hour, minute, second, milliseconds);
    double localTimeResult = (day * WTF::msPerDay) + ms;

    if (inputTimeType == TimeType::LocalTime && JSDateMathInternal::canNarrowToInt64Milliseconds(localTimeResult))
        return localTimeResult - localTimeOffset(static_cast<int64_t>(localTimeResult), inputTimeType).offset;
    return localTimeResult;
}

double DateCache::localTimeToMS(double milliseconds, TimeType inputTimeType)
{
    if (inputTimeType == TimeType::LocalTime && JSDateMathInternal::canNarrowToInt64Milliseconds(milliseconds))
        return milliseconds - localTimeOffset(static_cast<int64_t>(milliseconds), inputTimeType).offset;
    return milliseconds;
}

ALWAYS_INLINE std::tuple<int32_t, int32_t, int32_t> DateCache::yearMonthDayFromDaysWithCache(int32_t days)
{
    if (m_yearMonthDayCache) {
        // Check conservatively if the given 'days' has
        // the same year and month as the cached 'days'.
        int32_t cachedDays = m_yearMonthDayCache->m_days;
        int32_t newDay = m_yearMonthDayCache->m_day + (days - cachedDays);
        if (newDay >= 1 && newDay <= 28) {
            int32_t year = m_yearMonthDayCache->m_year;
            int32_t month = m_yearMonthDayCache->m_month;
            m_yearMonthDayCache = { days, year, month, newDay };
            return std::tuple { year, month, newDay };
        }
    }
    auto [year, month, day] = WTF::yearMonthDayFromDays(days);
    m_yearMonthDayCache = { days, year, month, day };
    return std::tuple { year, month, day };
}

// input is UTC
ALWAYS_INLINE PlainGregorianDateTime DateCache::computeGregorianDateTime(double millisecondsFromEpoch, TimeType outputTimeType)
{
    LocalTimeOffset localTime;
    if (outputTimeType == TimeType::LocalTime && JSDateMathInternal::canNarrowToInt64Milliseconds(millisecondsFromEpoch)) {
        localTime = localTimeOffset(static_cast<int64_t>(millisecondsFromEpoch));
        millisecondsFromEpoch += localTime.offset;
    }
    if (!JSDateMathInternal::canNarrowToInt64Milliseconds(millisecondsFromEpoch))
        return { };

    WTF::Int64Milliseconds timeClipped(static_cast<int64_t>(millisecondsFromEpoch));
    int32_t days = WTF::msToDays(timeClipped);
    int32_t timeInDayMS = WTF::timeInDay(timeClipped, days);
    auto [year, month, day] = yearMonthDayFromDaysWithCache(days);
    int32_t hour = timeInDayMS / (60 * 60 * 1000);
    int32_t minute = (timeInDayMS / (60 * 1000)) % 60;
    int32_t second = (timeInDayMS / 1000) % 60;
    return PlainGregorianDateTime(year, month, day, WTF::weekDay(days), hour, minute, second, localTime.offset / WTF::Int64Milliseconds::msPerMinute, localTime.isDST);
}

PlainGregorianDateTime DateCache::msToGregorianDateTime(double millisecondsFromEpoch, TimeType outputTimeType, UseSharedCache useSharedCache)
{
    if (useSharedCache == UseSharedCache::No)
        return computeGregorianDateTime(millisecondsFromEpoch, outputTimeType);

    auto& cache = m_brokenDownDateCaches[static_cast<unsigned>(outputTimeType)];
    if (auto cached = cache.get(millisecondsFromEpoch))
        return cached;

    auto result = computeGregorianDateTime(millisecondsFromEpoch, outputTimeType);
    if (result)
        cache.set(millisecondsFromEpoch, result);
    return result;
}

double DateCache::parseDate(JSGlobalObject* globalObject, VM& vm, const String& date)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (date == m_cachedDateString)
        return m_cachedDateStringValue;

    // V8's date parser (and useful web compat) treats every ECMAScript WhiteSpace code point as a
    // separator: TAB/VT/FF/SP, NBSP, BOM, and every Unicode Zs character including the
    // narrowNoBreakSpace that ICU >= 72 emits from toLocaleString. Both parsers below scan the
    // UTF-8 encoding one byte at a time, so fold all of those into ASCII spaces here. Line
    // terminators (LS/PS) are intentionally left alone so they continue to reject, matching V8.
    String updatedString = date;
    if (!date.containsOnlyASCII()) {
        if (date.is8Bit())
            updatedString = makeStringByReplacingAll(date, noBreakSpace, space);
        else {
            auto characters = date.span16();
            std::span<char16_t> buffer;
            auto result = StringImpl::createUninitialized(characters.size(), buffer);
            for (size_t i = 0; i < characters.size(); ++i) {
                char16_t c = characters[i];
                buffer[i] = isWhiteSpace<char16_t>(c) ? static_cast<char16_t>(space) : c;
            }
            updatedString = WTF::move(result);
        }
    }

    auto expectedString = updatedString.tryGetUTF8();
    if (!expectedString) {
        if (expectedString.error() == UTF8ConversionError::OutOfMemory)
            throwOutOfMemoryError(globalObject, scope);
        // https://tc39.github.io/ecma262/#sec-date-objects section 20.3.3.2 states that:
        // "Unrecognizable Strings or dates containing illegal element values in the
        // format String shall cause Date.parse to return NaN."
        return std::numeric_limits<double>::quiet_NaN();
    }

    auto parseDateImpl = [this] (auto dateString) {
        if (Options::useV8DateParser()) {
            bool local = false;
            double value = v8::ParseDateTimeString(byteCast<uint8_t>(dateString.data()), dateString.size(), local);

            if (local)
                value -= localTimeOffset(static_cast<int64_t>(value), TimeType::LocalTime).offset;

            return v8::TimeClip(value);
        }
        bool isLocalTime;
        double value = WTF::parseES5Date(dateString, isLocalTime);
        if (std::isnan(value))
            value = WTF::parseDate(dateString, isLocalTime);

        if (isLocalTime && JSDateMathInternal::canNarrowToInt64Milliseconds(value))
            value -= localTimeOffset(static_cast<int64_t>(value), TimeType::LocalTime).offset;

        return value;
    };

    // FIXME: expectedString is UTF-8 but parseDateImpl requires Latin1. Which is correct?
    double value = parseDateImpl(byteCast<Latin1Character>(expectedString.value().span()));
    m_cachedDateString = date;
    m_cachedDateStringValue = value;
    return value;
}

// https://tc39.es/ecma402/#sec-defaulttimezone
TimeZone DateCache::defaultTimeZone()
{
    return timeZoneCache()->m_canonicalTimeZone;
}

String DateCache::timeZoneDisplayName(bool isDST)
{
    if (m_timeZoneStandardDisplayNameCache.isNull()) {
        auto& timeZoneCache = *this->timeZoneCache();
        auto language = defaultLanguage().utf8();
        {
            Vector<char16_t, 32> standardDisplayNameBuffer;
            auto status = callBufferProducingFunction(ucal_getTimeZoneDisplayName, timeZoneCache.m_calendar.get(), UCAL_STANDARD, language.legacyCStringPointer(), standardDisplayNameBuffer);
            if (U_SUCCESS(status))
                m_timeZoneStandardDisplayNameCache = String::adopt(WTF::move(standardDisplayNameBuffer));
        }
        {
            Vector<char16_t, 32> dstDisplayNameBuffer;
            auto status = callBufferProducingFunction(ucal_getTimeZoneDisplayName, timeZoneCache.m_calendar.get(), UCAL_DST, language.legacyCStringPointer(), dstDisplayNameBuffer);
            if (U_SUCCESS(status))
                m_timeZoneDSTDisplayNameCache = String::adopt(WTF::move(dstDisplayNameBuffer));
        }
    }
    if (isDST)
        return m_timeZoneDSTDisplayNameCache;
    return m_timeZoneStandardDisplayNameCache;
}

static Lock timeZoneCacheLock;

// To confine icu::TimeZone destructor invocation in this file.
DateCache::DateCache()
{
    WTF::listenForTimeZoneChangeNotifications();
}

struct CachedHostTimeZone {
    TimeZone timeZone;
    uint64_t timeZoneID { 0 };
};

static std::optional<TimeZone> namedTimeZone(StringView id)
{
    if (id.isEmpty())
        return std::nullopt;
    // Resolve through intlResolveTimeZoneID so the zone collapses onto its IANA
    // primary identifier (e.g. "Asia/Calcutta" -> "Asia/Kolkata") and UTC-equivalent
    // names map to the UTC TimeZoneID.
    String primary = toPrimaryIanaTimeZoneIdentifier(id);
    if (auto resolved = intlResolveTimeZoneID(primary))
        return TimeZone::fromID(resolved.value());
    return std::nullopt;
}

// ECMA-262 knows a zone at a fixed offset by a name or as an offset identifier with minute precision. So this is UTC
// for zero, the IANA zone for whole hours ("Etc/GMT-3" is three hours east: its sign counts west, as POSIX does), and
// an offset zone ("+05:30") for the rest.
static TimeZone fixedOffsetTimeZone(int64_t secondsEast)
{
    int64_t minutesEast = secondsEast / 60;
    // ICU has no zone that is a day or more from UTC.
    if (!minutesEast || std::abs(minutesEast) >= 24 * 60)
        return TimeZone();
    int64_t hoursEast = minutesEast / 60;
    if (!(minutesEast % 60) && hoursEast >= -12 && hoursEast <= 14) {
        if (auto id = intlResolveTimeZoneID(makeString("Etc/GMT"_s, hoursEast > 0 ? '-' : '+', static_cast<unsigned>(std::abs(hoursEast)))))
            return TimeZone::fromID(id.value());
    }
    return TimeZone::fromUTCOffset(minutesEast * 60 * 1'000'000'000LL);
}

#if OS(UNIX)
// The middle of January and of July of this year. A zone with daylight saving time has it at one of them and not at
// the other, whichever hemisphere it is in.
using ProbeTimes = std::array<time_t, 2>;

static ProbeTimes probeTimes()
{
    int year = msToYear(WallTime::now().secondsSinceEpoch().milliseconds());
    return { static_cast<time_t>(dateToDaysFrom1970(year, 0, 15) * secondsPerDay), static_cast<time_t>(dateToDaysFrom1970(year, 6, 15) * secondsPerDay) };
}

// Seconds east of UTC.
struct HostOffsets {
    std::array<int64_t, 2> atProbeTimes;
    int64_t standard;
};

// The C library reads every form that TZ and /etc/localtime can have: a POSIX value such as "MSK-3", a tzfile at any
// path. ucal_getHostTimeZone has just run tzset(), so what the C library has is current.
static std::optional<HostOffsets> hostOffsetsFromCLibrary(const ProbeTimes& times)
{
    HostOffsets offsets { };
    bool hasStandard = false;
    for (size_t index = 0; index < times.size(); ++index) {
        struct tm local;
        if (!localtime_r(&times[index], &local))
            return std::nullopt;
        offsets.atProbeTimes[index] = local.tm_gmtoff;
        if (local.tm_isdst <= 0 && !hasStandard) {
            offsets.standard = local.tm_gmtoff;
            hasStandard = true;
        }
    }
    if (!hasStandard)
        offsets.standard = std::min(offsets.atProbeTimes[0], offsets.atProbeTimes[1]);
    return offsets;
}

static std::optional<std::array<int64_t, 2>> offsetsFromICU(TimeZone zone, const ProbeTimes& times)
{
    if (zone == TimeZone())
        return std::array<int64_t, 2> { 0, 0 };
    String icuID = zone.toICUString();
    StringView view { icuID };
    UErrorCode status = U_ZERO_ERROR;
    auto calendar = std::unique_ptr<UCalendar, ICUDeleter<ucal_close>>(ucal_open(view.upconvertedCharacters(), view.length(), "", UCAL_DEFAULT, &status));
    std::array<int64_t, 2> offsets { };
    for (size_t index = 0; index < times.size() && U_SUCCESS(status); ++index) {
        ucal_setMillis(calendar.get(), times[index] * 1000.0, &status);
        offsets[index] = (ucal_get(calendar.get(), UCAL_ZONE_OFFSET, &status) + ucal_get(calendar.get(), UCAL_DST_OFFSET, &status)) / 1000;
    }
    if (U_FAILURE(status))
        return std::nullopt;
    return offsets;
}

// ICU reads a name out of TZ, or out of the path /etc/localtime links to. With neither it guesses one from the zone's
// abbreviation, and with a TZ it cannot read as a name (a POSIX value with a DST part) it names /etc/localtime instead.
// A guess is not what the host is set to: "CST" is America/Chicago to ICU on a host in China, and "CET" has daylight
// saving time on a host in Algeria.
static bool hostIDIsAGuess(StringView hostID)
{
    if (const char* tz = getenv("TZ"); tz && *tz) {
        StringView name = StringView::fromLatin1(tz);
        if (name.startsWith(':'))
            name = name.substring(1);
        if (name.startsWith("posix/"_s) || name.startsWith("right/"_s))
            name = name.substring(6);
        return name != hostID;
    }
    // An abbreviation has three or four characters (TimeZone::detectHostTimeZone has the same test). A longer name
    // is the one in the path of /etc/localtime, or of the zoneinfo file with the same content.
    return hostID.length() >= 3 && hostID.length() <= 4;
}

// A tzfile's path names its zone in what follows "/zoneinfo/". ICU names /etc/localtime the same way.
static String zoneNameInTZFilePath(StringView path)
{
    constexpr auto marker = "/zoneinfo/"_s;
    size_t position = path.reverseFind(marker);
    if (position == notFound)
        return { };
    StringView name = path.substring(position + marker.length());
    // The same zones again, without and with leap seconds.
    if (name.startsWith("posix/"_s) || name.startsWith("right/"_s))
        name = name.substring(6);
    return name.toString();
}

// The names that a link to a tzfile can have: in the path of the file it resolves to, and in the link itself (macOS
// keeps the files in /usr/share/zoneinfo.default/, and only the links to them have a "/zoneinfo/").
static Vector<String, 2> zoneNamesOfTZFileLink(const String& path)
{
    Vector<String, 2> names;
    auto add = [&](StringView target) {
        if (String name = zoneNameInTZFilePath(target); !name.isEmpty())
            names.append(WTF::move(name));
    };
    CString utf8 = path.utf8();
    char buffer[PATH_MAX];
    if (realpath(utf8.legacyCStringPointer(), buffer))
        add(String::fromUTF8(buffer));
    ssize_t length = readlink(utf8.legacyCStringPointer(), buffer, sizeof(buffer));
    if (length > 0)
        add(String::fromUTF8(std::span<const char> { buffer, static_cast<size_t>(length) }));
    return names;
}
#endif

// WTF::setTimeZoneOverride stores what ICU canonicalizes: the ID of a zone, or a custom ID such as "GMT+05:30".
static TimeZone timeZoneFromOverride(std::span<const char16_t> overrideID)
{
    StringView id { overrideID };
    if (auto zone = namedTimeZone(id))
        return zone.value();
    if (id.startsWith("GMT"_s)) {
        if (auto nanosecondsEast = ISO8601::parseUTCOffset(id.substring(3)))
            return fixedOffsetTimeZone(nanosecondsEast.value() / 1'000'000'000);
    }
    // What is left is "Etc/Unknown", ICU's ID for no zone at all.
    return TimeZone();
}

// `hostID` is what ucal_getHostTimeZone returned. The zone object ICU made for it, which has the host's offset even
// when the ID names nothing, is not available: the C library's offset stands in for it.
static TimeZone timeZoneFromHostID(StringView hostID)
{
    auto zone = namedTimeZone(hostID);
#if OS(UNIX)
    if (zone && !hostIDIsAGuess(hostID))
        return zone.value();

    // From here on ICU did not read the zone as a name out of TZ, and what the C library has read is the reference.
    // ICU itself drops a zone it guessed from an abbreviation when its offset is not the C library's, and keeps only
    // that offset. Here the test is the time of day at both probe times.
    auto times = probeTimes();
    auto host = hostOffsetsFromCLibrary(times);
    auto tellsHostTime = [&](TimeZone candidate) {
        return !host || host->atProbeTimes == offsetsFromICU(candidate, times);
    };
    if (zone && tellsHostTime(zone.value()))
        return zone.value();

    // TZ can be the path of a tzfile, and ICU returns the path for an ID. A path into a zoneinfo directory names its
    // zone. A link such as ":/etc/localtime" names the zone it points to, but the file at that path can be another
    // zone: a container with the host's /etc/localtime mounted over the target of the link.
    if (!zone && hostID.startsWith('/')) {
        if (auto literal = namedTimeZone(zoneNameInTZFilePath(hostID)))
            return literal.value();
        for (auto& name : zoneNamesOfTZFileLink(hostID.toString())) {
            if (auto linked = namedTimeZone(name); linked && tellsHostTime(linked.value()))
                return linked.value();
        }
    }

    // A zone the C library knows and ICU has no name for: a POSIX value such as "MSK-3", or a tzfile outside a
    // zoneinfo directory. Its offset is known. Its daylight saving rule, if it has one, is not.
    if (host)
        return fixedOffsetTimeZone(host->standard);
    return TimeZone();
#else
    if (zone)
        return zone.value();
    // ICU takes the host's zone from the Windows time zone and never from TZ, so the zone ICU keeps is the source.
    UErrorCode status = U_ZERO_ERROR;
    auto calendar = std::unique_ptr<UCalendar, ICUDeleter<ucal_close>>(ucal_open(nullptr, 0, "", UCAL_DEFAULT, &status));
    int32_t milliseconds = ucal_get(calendar.get(), UCAL_ZONE_OFFSET, &status);
    if (U_FAILURE(status))
        return TimeZone();
    return fixedOffsetTimeZone(milliseconds / 1000);
#endif
}

static TimeZone retrieveTimeZoneInformation()
{
    Locker locker { timeZoneCacheLock };
    static NeverDestroyed<CachedHostTimeZone> globalCache;

    uint64_t currentID = WTF::lastTimeZoneID();
#if USE(TIME_ZONE_CHANGE_NOTIFICATIONS)
    bool isCacheStale = globalCache->timeZoneID != currentID;
#else
    bool isCacheStale = true;
#endif
    if (isCacheStale) {
        Vector<char16_t, 32> timeZoneID;
        getTimeZoneOverride(timeZoneID);
        TimeZone canonical;
        if (!timeZoneID.isEmpty())
            canonical = timeZoneFromOverride(timeZoneID.span());
        else {
            UErrorCode status = callBufferProducingFunction(ucal_getHostTimeZone, timeZoneID);
            ASSERT(U_SUCCESS(status));
            canonical = timeZoneFromHostID(U_SUCCESS(status) ? StringView(timeZoneID.span()) : StringView());
        }

        globalCache.get() = CachedHostTimeZone { canonical, currentID };
    }
    return globalCache->timeZone;
}

DateCache::~DateCache() = default;

OpaqueICUTimeZone* DateCache::timeZoneCache()
{
    if (!m_timeZoneCache)
        timeZoneCacheSlow();
    return m_timeZoneCache.get();
}

ALWAYS_INLINE LocalTimeOffset DateCache::localTimeOffset(int64_t millisecondsFromEpoch, TimeType inputTimeType)
{
    using Underlying = std::underlying_type_t<TimeType>;
    static_assert(!static_cast<Underlying>(TimeType::UTCTime));
    static_assert(static_cast<Underlying>(TimeType::LocalTime) == 1);
    return m_caches[static_cast<unsigned>(inputTimeType)].localTimeOffset(*this, millisecondsFromEpoch, inputTimeType);
}

void DateCache::timeZoneCacheSlow()
{
    ASSERT(!m_timeZoneCache);
    TimeZone canonical = retrieveTimeZoneInformation();
    String timeZoneForICU = canonical.toICUString();
    StringView timeZoneView(timeZoneForICU);
    auto upconverted = timeZoneView.upconvertedCharacters();
    auto* cache = new OpaqueICUTimeZone;
    cache->m_canonicalTimeZone = canonical;
    UErrorCode status = U_ZERO_ERROR;
    cache->m_calendar = std::unique_ptr<UCalendar, ICUDeleter<ucal_close>>(ucal_open(upconverted, timeZoneView.length(), "", UCAL_DEFAULT, &status));
    ASSERT_UNUSED(status, U_SUCCESS(status));
    ucal_setGregorianChange(cache->m_calendar.get(), minECMAScriptTime, &status); // Ignore "unsupported" error.
    m_timeZoneCache = std::unique_ptr<OpaqueICUTimeZone, OpaqueICUTimeZoneDeleter>(cache);
}

void DateCache::clearForTimeZoneChange()
{
    m_timeZoneCache.reset();
    for (auto& cache : m_caches)
        cache.reset();
    for (auto& cache : m_brokenDownDateCaches)
        cache.reset();
    m_yearMonthDayCache = std::nullopt;
    m_cachedDateString = String();
    m_cachedDateStringValue = std::numeric_limits<double>::quiet_NaN();
    m_timeZoneStandardDisplayNameCache = String();
    m_timeZoneDSTDisplayNameCache = String();
    m_cachedTimeZoneID = WTF::lastTimeZoneID();
}

} // namespace JSC
