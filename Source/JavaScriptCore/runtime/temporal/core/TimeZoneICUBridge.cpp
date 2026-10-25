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
#include "TimeZoneICUBridge.h"

#include "CalendarArithmetic.h"
#include "CalendarICUBridge.h"
#include "DateConstructor.h"
#include "DurationArithmetic.h"
#include "IntlObject.h"
#include "JSCTimeZone.h"
#include <unicode/ucal.h>
#include <wtf/DateMath.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/TinyLRUCache.h>
#include <wtf/unicode/icu/ICUHelpers.h>

namespace JSC {
namespace TemporalCore {

// [startMs, endMs) between two ICU transitions, with offset offsetMs (±Infinity at the ends).
struct OffsetPeriod {
    double startMs;
    double endMs;
    int32_t offsetMs;
};

class TimeZoneCacheEntry final : public ThreadSafeRefCounted<TimeZoneCacheEntry> {
    WTF_MAKE_TZONE_ALLOCATED(TimeZoneCacheEntry);
public:
    // Lookups a new entry serves from ICU before it learns periods.
    static constexpr unsigned lookupsBeforeLearning = 16;
    // About 128 years of a zone with two transitions a year.
    static constexpr size_t maxCachedPeriods = 256;
    // Lookups without learning after an eviction once full.
    static constexpr unsigned lookupsWithoutLearningWhenFull = 64 * maxCachedPeriods;

    Lock useLock;
    std::unique_ptr<UCalendar, ICUDeleter<ucal_close>> cal WTF_GUARDED_BY_LOCK(useLock);
    // Disjoint periods learned from cal, sorted by startMs.
    Vector<OffsetPeriod> periods WTF_GUARDED_BY_LOCK(useLock);
    unsigned lookupsUntilLearning WTF_GUARDED_BY_LOCK(useLock) { lookupsBeforeLearning };
    unsigned nextEviction WTF_GUARDED_BY_LOCK(useLock) { 0 };
};
WTF_MAKE_TZONE_ALLOCATED_IMPL(TimeZoneCacheEntry);

struct TimeZoneLRUCachePolicy {
    static bool isKeyNull(const TimeZone&) { return false; }
    static RefPtr<TimeZoneCacheEntry> createValueForNullKey() { return nullptr; }
    static RefPtr<TimeZoneCacheEntry> createValueForKey(const TimeZone&) { return adoptRef(*new TimeZoneCacheEntry); }
    static TimeZone createKeyForStorage(const TimeZone& tz) { return tz; }
};

static RefPtr<TimeZoneCacheEntry> timeZoneCacheEntry(const TimeZone& timeZone)
{
    static Lock cacheLock;
    static LazyNeverDestroyed<TinyLRUCache<TimeZone, RefPtr<TimeZoneCacheEntry>, 16, TimeZoneLRUCachePolicy>> cache;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        cache.construct();
    });
    Locker locker { cacheLock };
    return cache.get().get(timeZone);
}

template<typename F>
static auto withTimeZone(const TimeZone& timeZone, NOESCAPE const F& fn) -> decltype(fn(std::declval<TimeZoneCacheEntry&>()))
{
    ASSERT(timeZone.isID());
    auto entry = timeZoneCacheEntry(timeZone);
    ASSERT(entry);
    Locker locker { entry->useLock };
    if (!entry->cal) {
        String tzStr = timeZone.toICUString();
        StringView view(tzStr);
        auto upconverted = view.upconvertedCharacters();
        UErrorCode status = U_ZERO_ERROR;
        entry->cal = std::unique_ptr<UCalendar, ICUDeleter<ucal_close>>(ucal_open(upconverted, view.length(), "", UCAL_DEFAULT, &status));
    }
    // No ucal_clear: every use of the calendar starts with ucal_setMillis, which resets the time and all fields.
    return fn(*entry);
}

// isUTCEquivalent — internal: a named zone whose primary identifier is UTC (offset 0, no transitions).
static bool isUTCEquivalent(const TimeZone& timeZone)
{
    return timeZone.isID() && intlPrimaryTimeZoneID(timeZone.id()) == utcTimeZoneID();
}

// getOffsetMsAtEpoch — internal: queries ICU for UTC+DST offset in ms at a given epoch
static std::optional<int32_t> getOffsetMsAtEpoch(UCalendar* cal, double epochMs)
{
    UErrorCode status = U_ZERO_ERROR;
    ucal_setMillis(cal, epochMs, &status);
    if (U_FAILURE(status)) [[unlikely]]
        return std::nullopt;
    int32_t rawOffset = ucal_get(cal, UCAL_ZONE_OFFSET, &status);
    if (U_FAILURE(status)) [[unlikely]]
        return std::nullopt;
    int32_t dstOffset = ucal_get(cal, UCAL_DST_OFFSET, &status);
    if (U_FAILURE(status)) [[unlikely]]
        return std::nullopt;
    return rawOffset + dstOffset;
}

// offsetPeriodAt — internal: the OffsetPeriod containing epochMs, cached or from ICU (empty if only the offset is known).
// The caller has checked entry.cal.
static std::optional<OffsetPeriod> offsetPeriodAt(TimeZoneCacheEntry& entry, double epochMs) WTF_REQUIRES_LOCK(entry.useLock)
{
    auto& periods = entry.periods;
    auto next = std::ranges::upper_bound(periods, epochMs, std::less { }, &OffsetPeriod::startMs);
    if (next != periods.begin() && epochMs < std::prev(next)->endMs)
        return *std::prev(next);

    UCalendar* cal = entry.cal.get();
    auto offsetMs = getOffsetMsAtEpoch(cal, epochMs);
    if (!offsetMs) [[unlikely]]
        return std::nullopt;
    if (entry.lookupsUntilLearning) {
        --entry.lookupsUntilLearning;
        return OffsetPeriod { epochMs, epochMs, *offsetMs };
    }
    // ICU reports every change of the raw or DST offset as a transition, so the offset is constant
    // from the last transition at or before epochMs up to the first one after it.
    UErrorCode status = U_ZERO_ERROR;
    UDate previousMs = 0;
    UDate nextMs = 0;
    bool hasPrevious = ucal_getTimeZoneTransitionDate(cal, UCAL_TZ_TRANSITION_PREVIOUS_INCLUSIVE, &previousMs, &status);
    bool hasNext = ucal_getTimeZoneTransitionDate(cal, UCAL_TZ_TRANSITION_NEXT, &nextMs, &status);
    if (U_FAILURE(status)) [[unlikely]]
        return std::nullopt;
    OffsetPeriod period {
        hasPrevious ? previousMs : -std::numeric_limits<double>::infinity(),
        hasNext ? nextMs : std::numeric_limits<double>::infinity(),
        *offsetMs
    };
    if (!(period.startMs <= epochMs && epochMs < period.endMs)) [[unlikely]]
        return OffsetPeriod { epochMs, epochMs, *offsetMs };

    size_t index = next - periods.begin();
    if (periods.size() >= TimeZoneCacheEntry::maxCachedPeriods) {
        size_t victim = entry.nextEviction++ % periods.size();
        periods.removeAt(victim);
        if (victim < index)
            --index;
        entry.lookupsUntilLearning = TimeZoneCacheEntry::lookupsWithoutLearningWhenFull;
    }
    periods.insert(index, period);
    return period;
}

// isoDateTimeToLocalMs — internal: converts ISO date+time to epoch ms treating local time as UTC (no offset applied)
static double isoDateTimeToLocalMs(const ISO8601::PlainDate& date, const ISO8601::PlainTime& time)
{
    // makeDay takes month as 0-indexed; PlainDate month is 1-indexed.
    double days = makeDay(date.year(), date.month() - 1, date.day());
    double timeMs = makeTime(time.hour(), time.minute(), time.second(), time.millisecond());
    return makeDate(days, timeMs);
}

static constexpr std::pair<int64_t, int64_t> floorDivMod(int64_t num, int64_t denom)
{
    int64_t q = num / denom;
    int64_t r = num % denom;
    if (r < 0) {
        r += denom;
        q -= 1;
    }
    return { q, r };
}

// temporal_rs: IsoTime::balance (src/iso.rs:664). Cascades ns→μs→ms→s→m→h; returns overflow days.
static std::pair<int64_t, ISO8601::PlainTime> balanceIsoTime(int64_t hour, int64_t minute, int64_t second, int64_t millisecond, int64_t microsecond, int64_t nanosecond)
{
    auto [nq, ns] = floorDivMod(nanosecond, 1000);
    microsecond += nq;
    auto [uq, us] = floorDivMod(microsecond, 1000);
    millisecond += uq;
    auto [mq, ms] = floorDivMod(millisecond, 1000);
    second += mq;
    auto [sq, s] = floorDivMod(second, 60);
    minute += sq;
    auto [minq, min] = floorDivMod(minute, 60);
    hour += minq;
    auto [days, h] = floorDivMod(hour, 24);
    return { days, ISO8601::PlainTime(static_cast<int>(h), static_cast<int>(min), static_cast<int>(s), static_cast<int>(ms), static_cast<int>(us), static_cast<int>(ns)) };
}

// temporal_rs: IsoDate::balance (src/iso.rs:343). Normalizes (y, m, d) via epoch-days round-trip.
static ISO8601::PlainDate balanceIsoDate(int32_t year, int32_t month, int64_t day)
{
    double epochDays = makeDay(year, month - 1, static_cast<int>(day));
    auto [y, m0, d] = WTF::yearMonthDayFromDays(static_cast<int32_t>(epochDays));
    return ISO8601::PlainDate(y, static_cast<uint8_t>(m0 + 1), static_cast<uint8_t>(d));
}

// temporal_rs: IsoDateTime::from_epoch_nanos (src/iso.rs:87).
// Steps 2-3 of GetISODateTimeFor with caller-supplied offset: GetISOPartsFromEpoch(epochNs + offsetNs).
ISO8601::PlainDateTime exactTimeToLocalDateAndTime(ISO8601::ExactTime exactTime, int64_t offsetNs)
{
    auto [days, nsInDay] = splitTimeDuration(exactTime.epochNanoseconds() + offsetNs);
    auto [year, month0, day] = WTF::yearMonthDayFromDays(static_cast<int32_t>(days));
    return ISO8601::PlainDateTime(ISO8601::PlainDate(year, static_cast<uint8_t>(month0 + 1), static_cast<uint8_t>(day)), plainTimeFromSubdayNs(nsInDay));
}

// getOffsetNanosecondsFor — temporal_rs: TimeZone::get_offset_nanos_for (src/builtins/core/time_zone.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-getoffsetnanosecondsfor
TemporalResult<int64_t> getOffsetNanosecondsFor(const TimeZone& timeZone, ISO8601::ExactTime exactTime)
{
    // 1. Let parseResult be ! ParseTimeZoneIdentifier(timeZone).
    // 2. If parseResult.[[OffsetMinutes]] is not ~empty~, return parseResult.[[OffsetMinutes]] × (60 × 10^9).
    if (timeZone.isUTCOffset())
        return timeZone.utcOffsetNanoseconds();
    if (isUTCEquivalent(timeZone))
        return 0;

    // 3. Return GetNamedTimeZoneOffsetNanoseconds(parseResult.[[Name]], epochNs).
    // NOTE: From the zone's cached offset periods, filled from ICU4C (UCAL_ZONE_OFFSET + UCAL_DST_OFFSET).
    return withTimeZone(timeZone, [&](TimeZoneCacheEntry& entry) WTF_REQUIRES_LOCK(entry.useLock) -> TemporalResult<int64_t> {
        if (!entry.cal) [[unlikely]]
            return makeUnexpected(rangeError(icuOpenTimeZoneFailed));
        double epochMs = static_cast<double>(exactTime.floorEpochMilliseconds());
        auto period = offsetPeriodAt(entry, epochMs);
        if (!period)
            return makeUnexpected(rangeError(icuTimeZoneOffsetFailed));
        constexpr int64_t nsPerMs = 1'000'000;
        return static_cast<int64_t>(period->offsetMs) * nsPerMs;
    });
}

// https://tc39.es/proposal-temporal/#sec-temporal-getisodatetimefor
TemporalResult<ISO8601::PlainDateTime> getISODateTimeFor(const TimeZone& timeZone, ISO8601::ExactTime epochNs)
{
    // Step 1: Let offsetNs be GetOffsetNanosecondsFor(tz, epochNs).
    auto offsetResult = getOffsetNanosecondsFor(timeZone, epochNs);
    if (!offsetResult) [[unlikely]]
        return makeUnexpected(offsetResult.error());
    // Steps 2-3: GetISOPartsFromEpoch(ℝ(epochNs) + offsetNs) + CombineISODateAndTimeRecord.
    return exactTimeToLocalDateAndTime(epochNs, *offsetResult);
}

// getNamedTimeZoneEpochNanoseconds — ICU4C implementation of the implementation-defined AO
// https://tc39.es/proposal-temporal/#sec-getnamedtimezoneepochnanoseconds
// NOTE: Solved from the zone's cached offset periods (see below). When those are unavailable, falls
// back to ucal_getTimeZoneOffsetFromLocal (ICU 69+): querying the FORMER (pre-transition) and LATTER
// (post-transition) interpretations of the local time yields the two bracket offsets, from which
// normal/gap/fold follow arithmetically.
static TemporalResult<PossibleEpochNanoseconds> getNamedTimeZoneEpochNanoseconds(const TimeZone& timeZone, const ISO8601::PlainDate& date, const ISO8601::PlainTime& time)
{
    // NOTE: Sub-ms fields are added back after ms-level computation; offset changes occur at ≥second granularity.
    Int128 subMs = static_cast<Int128>(time.microsecond()) * 1000 + static_cast<Int128>(time.nanosecond());

    // Compute the "naive" local epoch — the local datetime treated as UTC, used for gap/fold detection.
    double localMs = isoDateTimeToLocalMs(date, time);

    return withTimeZone(timeZone, [&](TimeZoneCacheEntry& entry) WTF_REQUIRES_LOCK(entry.useLock) -> TemporalResult<PossibleEpochNanoseconds> {
        UCalendar* cal = entry.cal.get();
        if (!cal) [[unlikely]]
            return makeUnexpected(rangeError(icuOpenTimeZoneFailed));

        auto makeExactTime = [&](double epochMs) -> ISO8601::ExactTime {
            auto base = ISO8601::ExactTime::fromEpochMilliseconds(static_cast<int64_t>(epochMs));
            return ISO8601::ExactTime(base.epochNanoseconds() + subMs);
        };
        constexpr int64_t nsPerMs = static_cast<int64_t>(ISO8601::ExactTime::nsPerMillisecond);

        // Candidates are u = localMs - offset within a day of localMs that fall in their own period; with at most one
        // transition in the window this matches the ICU path below, otherwise defer to it.
        // While the entry is not learning, lookups skip the walk, even where the cached periods would cover it.
        static constexpr unsigned maxPeriodsWalked = 2;
        std::array<double, maxPeriodsWalked> candidates;
        size_t candidateCount = 0;
        bool walked = false;
        std::optional<OffsetPeriod> period;
        if (entry.lookupsUntilLearning)
            --entry.lookupsUntilLearning;
        else
            period = offsetPeriodAt(entry, localMs - msPerDay);
        for (unsigned steps = 0; period && steps < maxPeriodsWalked; ++steps) {
            if (period->startMs == period->endMs)
                break; // Offset only: fall back to ICU below.
            double candidate = localMs - period->offsetMs;
            if (period->startMs <= candidate && candidate < period->endMs)
                candidates[candidateCount++] = candidate;
            if (period->endMs > localMs + msPerDay) {
                walked = true;
                break;
            }
            period = offsetPeriodAt(entry, period->endMs);
        }
        if (walked) {
            if (candidateCount == 1)
                return PossibleEpochNanoseconds { makeExactTime(candidates[0]) };
            if (candidateCount == 2)
                return PossibleEpochNanoseconds { std::array<ISO8601::ExactTime, 2> { makeExactTime(candidates[0]), makeExactTime(candidates[1]) } };
            // Gap: the offsets one day either side, as DisambiguatePossibleEpochNanoseconds steps 6-13 use
            // (without their IsValidEpochNanoseconds checks, like the ICU path below).
            auto before = offsetPeriodAt(entry, localMs - msPerDay);
            auto after = offsetPeriodAt(entry, localMs + msPerDay);
            if (before && after)
                return PossibleEpochNanoseconds { GapOffsets { before->offsetMs * nsPerMs, after->offsetMs * nsPerMs } };
        }

        // Store the naive local epoch as the calendar's time value. ucal_getTimeZoneOffsetFromLocal
        // reinterprets that stored value as wall-clock (local) time. IMPORTANT: do NOT use
        // ucal_setDateTime here — it would apply ICU's own disambiguation and store a resolved UTC
        // instant, which is not what getTimeZoneOffsetFromLocal expects to read.
        UErrorCode status = U_ZERO_ERROR;
        ucal_setMillis(cal, localMs, &status);
        if (U_FAILURE(status)) [[unlikely]]
            return makeUnexpected(rangeError(icuSetCalendarFailed));

        // Query both interpretations of the local time. Away from any transition they are equal.
        //   FORMER = offset in effect before a nearby transition, LATTER = offset after it.
        auto offsetMsFor = [&](UTimeZoneLocalOption opt) -> std::optional<int64_t> {
            UErrorCode s = U_ZERO_ERROR;
            int32_t rawOffset = 0;
            int32_t dstOffset = 0;
            ucal_getTimeZoneOffsetFromLocal(cal, opt, opt, &rawOffset, &dstOffset, &s);
            if (U_FAILURE(s)) [[unlikely]]
                return std::nullopt;
            return static_cast<int64_t>(rawOffset) + static_cast<int64_t>(dstOffset);
        };

        auto before = offsetMsFor(UCAL_TZ_LOCAL_FORMER);
        auto after = offsetMsFor(UCAL_TZ_LOCAL_LATTER);
        if (!before || !after) [[unlikely]]
            return makeUnexpected(rangeError(icuTimeZoneOffsetFailed));

        // Normal: a single offset is in force → exactly one instant.
        if (*before == *after)
            return PossibleEpochNanoseconds { makeExactTime(localMs - static_cast<double>(*before)) };

        // Offset jumped up (spring-forward) → the local time is skipped (gap): no instant maps here.
        // The probes above are also Disambiguate steps 6-13's offsetBefore/offsetAfter, which ICU
        // gives directly; those steps cannot throw, so skipping them loses nothing observable.
        if (*after > *before)
            return PossibleEpochNanoseconds { GapOffsets { *before * nsPerMs, *after * nsPerMs } };

        // Offset jumped down (fall-back) → the local time occurs twice (fold).
        // earlier uses the larger (pre-transition) offset; later uses the smaller (post-transition).
        ISO8601::ExactTime earlier = makeExactTime(localMs - static_cast<double>(*before));
        ISO8601::ExactTime later = makeExactTime(localMs - static_cast<double>(*after));
        ASSERT(earlier.epochNanoseconds() <= later.epochNanoseconds());
        return PossibleEpochNanoseconds { std::array<ISO8601::ExactTime, 2> { earlier, later } };
    }); // withTimeZone
}

// getPossibleEpochNanosecondsFor — temporal_rs: TimeZone::get_possible_epoch_ns_for (src/builtins/core/time_zone.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-getpossibleepochnanoseconds
TemporalResult<PossibleEpochNanoseconds> getPossibleEpochNanosecondsFor(const TimeZone& timeZone, const ISO8601::PlainDate& date, const ISO8601::PlainTime& time)
{
    // 1. Let parseResult be ! ParseTimeZoneIdentifier(timeZone).
    // 2. If parseResult.[[OffsetMinutes]] is not ~empty~, then
    //    a-d. Balance, check range, compute UTC epoch ns, return « epochNs ».
    // NOTE: UTC-offset timezones always yield exactly 1 candidate; no ICU needed.
    // NOTE: A UTC-equivalent named zone behaves exactly like the offset zone +00:00 here.
    bool utcEquivalent = isUTCEquivalent(timeZone);
    if (timeZone.isUTCOffset() || utcEquivalent) {
        Int128 subMs = static_cast<Int128>(time.microsecond()) * 1000 + static_cast<Int128>(time.nanosecond());
        int64_t offsetMs = utcEquivalent ? 0 : timeZone.utcOffsetNanoseconds() / static_cast<int64_t>(ISO8601::ExactTime::nsPerMillisecond);
        double localMs = isoDateTimeToLocalMs(date, time);
        double epochMs = localMs - static_cast<double>(offsetMs);
        auto base = ISO8601::ExactTime::fromEpochMilliseconds(static_cast<int64_t>(epochMs));
        ISO8601::ExactTime exactTime(base.epochNanoseconds() + subMs);
        if (!exactTime.isValid())
            return makeUnexpected(rangeError(epochNanosecondsOutOfRange));
        return PossibleEpochNanoseconds { exactTime };
    }

    // 3. Else, let possibleEpochNanoseconds be GetNamedTimeZoneEpochNanoseconds(parseResult.[[Name]], isoDateTime).
    auto possible = getNamedTimeZoneEpochNanoseconds(timeZone, date, time);
    if (!possible)
        return makeUnexpected(possible.error());

    // 4. For each value epochNanoseconds in possibleEpochNanoseconds:
    //    a. If IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError.
    for (auto& candidate : epochCandidates(*possible)) {
        if (!candidate.isValid())
            return makeUnexpected(rangeError(epochNanosecondsOutOfRange));
    }

    // 5. Return possibleEpochNanoseconds.
    return *possible;
}

// getTimeZoneTransition — ICU4C implementation of GetNamedTimeZoneNextTransition / GetNamedTimeZonePreviousTransition
// https://tc39.es/proposal-temporal/#sec-temporal-getnamedtimezonenexttransition (Next)
// https://tc39.es/proposal-temporal/#sec-temporal-getnamedtimezoneprevioustransition (Previous)
TemporalResult<std::optional<ISO8601::ExactTime>> getTimeZoneTransition(const TimeZone& timeZone, ISO8601::ExactTime exactTime, TransitionDirection direction)
{
    // 1. If timeZone is a UTC-offset timezone, return null (no transitions). Same for a UTC-equivalent name.
    if (timeZone.isUTCOffset() || isUTCEquivalent(timeZone))
        return std::optional<ISO8601::ExactTime> { std::nullopt };

    // 2. Compute epochMs.
    double epochMs;
    if (direction == TransitionDirection::Previous) {
        Int128 epochNs = exactTime.epochNanoseconds();
        int64_t ms = static_cast<int64_t>(epochNs / static_cast<int64_t>(ISO8601::ExactTime::nsPerMillisecond));
        if (epochNs % static_cast<int64_t>(ISO8601::ExactTime::nsPerMillisecond) > 0)
            ms++;
        epochMs = static_cast<double>(ms);
    } else
        epochMs = static_cast<double>(exactTime.floorEpochMilliseconds());

    return withTimeZone(timeZone, [&](TimeZoneCacheEntry& entry) WTF_REQUIRES_LOCK(entry.useLock) -> TemporalResult<std::optional<ISO8601::ExactTime>> {
        UCalendar* cal = entry.cal.get();
        if (!cal) [[unlikely]]
            return makeUnexpected(rangeError(icuOpenTimeZoneFailed));

        UErrorCode status = U_ZERO_ERROR;
        ucal_setMillis(cal, epochMs, &status);
        if (U_FAILURE(status)) [[unlikely]]
            return makeUnexpected(rangeError(icuSetCalendarFailed));

        UTimeZoneTransitionType transType = (direction == TransitionDirection::Next) ? UCAL_TZ_TRANSITION_NEXT : UCAL_TZ_TRANSITION_PREVIOUS;

        // 3. Repeat (up to 20 times to skip rule-only changes).
        for (int safetyLimit = 0; safetyLimit < 20; ++safetyLimit) {
            UDate transitionMs = 0;
            bool hasTransition = ucal_getTimeZoneTransitionDate(cal, transType, &transitionMs, &status);
            if (U_FAILURE(status)) [[unlikely]]
                return makeUnexpected(rangeError(icuTransitionFailed));

            if (!hasTransition)
                return std::optional<ISO8601::ExactTime> { std::nullopt };

            auto transition = ISO8601::ExactTime::fromEpochMilliseconds(static_cast<int64_t>(transitionMs));
            if (!transition.isValid())
                return std::optional<ISO8601::ExactTime> { std::nullopt };

            // Check if offset actually changed at this transition by querying before/after on cal directly.
            double beforeMs = transitionMs - 1.0;
            auto offsetBefore = getOffsetMsAtEpoch(cal, beforeMs);
            if (!offsetBefore) [[unlikely]]
                return makeUnexpected(rangeError(icuTransitionFailed));
            auto offsetAfter = getOffsetMsAtEpoch(cal, transitionMs);
            if (!offsetAfter) [[unlikely]]
                return makeUnexpected(rangeError(icuTransitionFailed));
            if (*offsetBefore != *offsetAfter)
                return std::optional<ISO8601::ExactTime> { transition };

            status = U_ZERO_ERROR;
            ucal_setMillis(cal, direction == TransitionDirection::Previous ? beforeMs : transitionMs + 1.0, &status);
            if (U_FAILURE(status)) [[unlikely]]
                return makeUnexpected(rangeError(icuTransitionFailed));
        }
        return std::optional<ISO8601::ExactTime> { std::nullopt };
    }); // withTimeZone
}

// disambiguatePossibleEpochNanoseconds — temporal_rs: TimeZone::disambiguate_possible_epoch_nanos (src/builtins/core/time_zone.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-disambiguatepossibleepochnanoseconds
// NOTE: steps 6-13 are done during gap detection in getNamedTimeZoneEpochNanoseconds and arrive as
// GapOffsets, so the gap branch here starts at step 14.
static TemporalResult<ISO8601::ExactTime> disambiguatePossibleEpochNanoseconds(const PossibleEpochNanoseconds& possible, const TimeZone& timeZone, const ISO8601::PlainDate& date, const ISO8601::PlainTime& time, TemporalDisambiguation disambiguation)
{
    return WTF::switchOn(possible,
        // 1. n = elements in possibleEpochNs (encoded in Variant type).
        // 2. If n = 1, return the sole element.
        [](const ISO8601::ExactTime& t) -> TemporalResult<ISO8601::ExactTime> {
            return t;
        },
        // 3. If n ≠ 0 (DST fold: 2 candidates [earlier, later]):
        //    a. If disambiguation is ~earlier~ or ~compatible~, return possibleEpochNs[0].
        //    b. If disambiguation is ~later~, return possibleEpochNs[n-1].
        //    c. Assert: disambiguation is ~reject~. Throw a RangeError.
        [&](const std::array<ISO8601::ExactTime, 2>& fold) -> TemporalResult<ISO8601::ExactTime> {
            if (disambiguation == TemporalDisambiguation::Reject)
                return makeUnexpected(rangeError("ambiguous instant: use a 'disambiguation' option to resolve"_s));
            if (disambiguation == TemporalDisambiguation::Earlier || disambiguation == TemporalDisambiguation::Compatible)
                return fold[0];
            ASSERT(disambiguation == TemporalDisambiguation::Later);
            return fold[1];
        },
        // 4. Assert: n = 0 (DST gap — local time does not exist).
        [&](const GapOffsets& gap) -> TemporalResult<ISO8601::ExactTime> {
            // 5. If disambiguation is ~reject~, throw a RangeError.
            if (disambiguation == TemporalDisambiguation::Reject)
                return makeUnexpected(rangeError("nonexistent instant: local time does not exist in this time zone (DST gap)"_s));

            // 14. nanoseconds = offsetAfter - offsetBefore.
            int64_t nanoseconds = gap.afterNs - gap.beforeNs;
            // 15. Assert: abs(nanoseconds) ≤ nsPerDay.
            ASSERT(Int128(nanoseconds < 0 ? -nanoseconds : nanoseconds) <= ISO8601::ExactTime::nsPerDay);

            bool isEarlier = disambiguation == TemporalDisambiguation::Earlier;
            // 16.a-16.d / 18-21. Shift the local time by ∓nanoseconds: AddTime, AddDaysToISODate, CombineISODateAndTimeRecord.
            auto [dayShift, shiftedTime] = balanceIsoTime(time.hour(), time.minute(), time.second(),
                time.millisecond(), time.microsecond(),
                static_cast<int64_t>(time.nanosecond()) + (isEarlier ? -nanoseconds : nanoseconds));
            auto shiftedDate = balanceIsoDate(date.year(), date.month(), static_cast<int64_t>(date.day()) + dayShift);

            // 16.e / 22. Re-entering, rather than computing naiveNs - offset, is what carries GetPossibleEpochNanoseconds step 5's IsValidEpochNanoseconds throw.
            auto shiftedPossible = getPossibleEpochNanosecondsFor(timeZone, shiftedDate, shiftedTime);
            if (!shiftedPossible)
                return makeUnexpected(shiftedPossible.error());
            auto candidates = epochCandidates(*shiftedPossible);

            // 16.f / 23-24. Assert: n ≠ 0 — the shifted time is past the transition, so it exists.
            ASSERT(!candidates.empty());
            if (candidates.empty()) [[unlikely]]
                return makeUnexpected(rangeError("nonexistent instant: local time does not exist in this time zone (DST gap)"_s));

            // 16.g / 25. Return possibleEpochNs[0] / possibleEpochNs[n - 1].
            return isEarlier ? candidates.front() : candidates.back();
        });
}

// getEpochNanosecondsFor — temporal_rs: TimeZone::get_epoch_nanoseconds_for (src/builtins/core/time_zone.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-getepochnanosecondsfor
TemporalResult<ISO8601::ExactTime> getEpochNanosecondsFor(const TimeZone& timeZone, const ISO8601::PlainDate& date, const ISO8601::PlainTime& time, TemporalDisambiguation disambiguation)
{
    // 1. Let possibleEpochNs be ? GetPossibleEpochNanosecondsFor(timeZone, date, time).
    auto possible = getPossibleEpochNanosecondsFor(timeZone, date, time);
    if (!possible)
        return makeUnexpected(possible.error());
    // 2. Return ? DisambiguatePossibleEpochNanoseconds(possibleEpochNs, timeZone, isoDateTime, disambiguation).
    return disambiguatePossibleEpochNanoseconds(*possible, timeZone, date, time, disambiguation);
}

// addZonedDateTime — temporal_rs: ZonedDateTime::add_zoned_date_time (src/builtins/core/zoned_date_time.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-addzoneddatetime
TemporalResult<ISO8601::ExactTime> addZonedDateTime(ISO8601::ExactTime startEpochNs, const TimeZone& timeZone, const ISO8601::Duration& duration, TemporalOverflow overflow, CalendarID calendarId)
{
    // NOTE: Pre-compute the time duration (norm) as nanoseconds; used by AddInstant in steps 1 and 7.
    CheckedInt128 m = checkedCastDoubleToInt128(duration.minutes()) + checkedCastDoubleToInt128(duration.hours()) * Int128(60);
    CheckedInt128 s = checkedCastDoubleToInt128(duration.seconds()) + m * Int128(60);
    CheckedInt128 ms = checkedCastDoubleToInt128(duration.milliseconds()) + s * Int128(1000);
    CheckedInt128 us = CheckedInt128(duration.microseconds()) + ms * Int128(1000);
    CheckedInt128 ns = CheckedInt128(duration.nanoseconds()) + us * Int128(1000);
    ASSERT(!ns.hasOverflowed());
    Int128 norm = ns;

    // 1. If DateDurationSign(duration.[[Date]]) = 0, return ? AddInstant(epochNanoseconds, duration.[[Time]]).
    bool noDateComponents = !duration.years() && !duration.months()
        && !duration.weeks() && !duration.days();
    if (noDateComponents) {
        if (!norm)
            return startEpochNs;
        ISO8601::ExactTime result(startEpochNs.epochNanoseconds() + norm);
        if (!result.isValid())
            return makeUnexpected(rangeError("Duration addition results in an out-of-range ZonedDateTime"_s));
        return result;
    }

    // 2. Let isoDateTime be GetISODateTimeFor(timeZone, epochNanoseconds).
    auto isoDateTimeResult = getISODateTimeFor(timeZone, startEpochNs);
    if (!isoDateTimeResult) [[unlikely]]
        return makeUnexpected(isoDateTimeResult.error());
    auto [date, time] = *isoDateTimeResult;

    // 3. Let addedDate be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], duration.[[Date]], overflow).
    ISO8601::Duration dateDuration(duration.years(), duration.months(), duration.weeks(), duration.days(), 0, 0, 0, 0, 0, 0);
    TemporalResult<ISO8601::PlainDate> addedDateResult;
    if (calendarIsISO(calendarId))
        addedDateResult = calendarDateAdd(date, dateDuration, overflow);
    else
        addedDateResult = TemporalCore::calendarDateAdd(calendarId, date, dateDuration, overflow);
    if (!addedDateResult)
        return makeUnexpected(addedDateResult.error());

    // 4. Let intermediateDateTime be CombineISODateAndTimeRecord(addedDate, isoDateTime.[[Time]]).
    // 5. If ISODateTimeWithinLimits(intermediateDateTime) is false, throw a RangeError exception.
    if (!ISO8601::isDateTimeWithinLimits(addedDateResult->year(), addedDateResult->month(), addedDateResult->day(), time.hour(), time.minute(), time.second(), time.millisecond(), time.microsecond(), time.nanosecond()))
        return makeUnexpected(rangeError("intermediate datetime out of range"_s));

    // 6. Let intermediateNs be ! GetEpochNanosecondsFor(timeZone, intermediateDateTime, ~compatible~).
    auto intermediateResult = getEpochNanosecondsFor(timeZone, *addedDateResult, time, TemporalDisambiguation::Compatible);
    if (!intermediateResult)
        return makeUnexpected(intermediateResult.error());

    // 7. Return ? AddInstant(intermediateNs, duration.[[Time]]).
    ISO8601::ExactTime result(intermediateResult->epochNanoseconds() + norm);
    if (!result.isValid())
        return makeUnexpected(rangeError("Duration addition results in an out-of-range ZonedDateTime"_s));
    return result;
}

// timeZoneEquals — temporal_rs: TimeZone::time_zone_equals_with_provider (src/builtins/core/time_zone.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-timezoneequals
bool timeZoneEquals(const TimeZone& a, const TimeZone& b)
{
    // 1. If one is two, return true.
    if (a == b)
        return true;

    // 2. If neither one nor two is an offset time zone identifier, then
    if (a.isUTCOffset() || b.isUTCOffset())
        return false; // 4. Return false.

    // 2.a-2.d. recordOne/recordTwo = GetAvailableNamedTimeZoneIdentifier(...); both are non-empty.
    // 2.e. If recordOne.[[PrimaryIdentifier]] is recordTwo.[[PrimaryIdentifier]], return true.
    // 4. Return false.
    return intlPrimaryTimeZoneID(a.id()) == intlPrimaryTimeZoneID(b.id());
}

} // namespace TemporalCore
} // namespace JSC
