/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "ICUTimeZone.h"

#include <cmath>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/unicode/icu/ICUHelpers.h>

#if USE(BUN_JSC_ADDITIONS) && !OS(DARWIN)
// The ICU that Bun bundles is the one these headers describe, so its C++ API can be called. Platform.h hides that API
// from the rest of WebKit, and this file is not part of a unified source, so it shows only here.
#define USE_ICU_TIME_ZONE_OBJECT 1
#undef U_SHOW_CPLUSPLUS_API
#define U_SHOW_CPLUSPLUS_API 1
#include <unicode/basictz.h>
#include <unicode/locid.h>
#include <unicode/timezone.h>
#include <unicode/tztrans.h>
#include <unicode/unistr.h>
#undef U_SHOW_CPLUSPLUS_API
#define U_SHOW_CPLUSPLUS_API 0
#else
#define USE_ICU_TIME_ZONE_OBJECT 0
#ifdef U_HIDE_DRAFT_API
#undef U_HIDE_DRAFT_API
#endif
#include <unicode/ucal.h>
#define U_HIDE_DRAFT_API 1
#endif

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ICUTimeZone);

#if USE(ICU_TIME_ZONE_OBJECT)

// The calls below are the ones that ucal.cpp makes on the zone of a UCalendar, so both ways give the same answers.
//
// On Windows JavaScriptCore is compiled without RTTI and ICU with it. A constructor of an ICU class that is inline in
// ICU's headers would put a copy of that class's vftable without RTTI data into this file. So every ICU object that
// this file constructs uses a constructor that ICU's library defines.

static const icu::TimeZone& timeZone(void* zone)
{
    return *static_cast<const icu::TimeZone*>(zone);
}

// Every zone that TimeZone::createTimeZone returns is a BasicTimeZone: an OlsonTimeZone or a SimpleTimeZone. Without
// RTTI this cannot be the dynamic_cast that ucal.cpp has.
static const icu::BasicTimeZone& basicTimeZone(void* zone)
{
    return static_cast<const icu::BasicTimeZone&>(timeZone(zone));
}

// ucal_setMillis refuses NaN. It also pins a time to the range of a Calendar, about 5.8 million years either way. No
// caller has a time near that: each one checks its time against the range of ECMAScript time values first.
static bool isTimeForICU(double milliseconds)
{
    return !std::isnan(milliseconds);
}

std::unique_ptr<ICUTimeZone> ICUTimeZone::create(StringView icuIdentifier)
{
    auto characters = icuIdentifier.upconvertedCharacters();
    // This aliases the characters, as ucal_open does. createTimeZone copies what it keeps.
    icu::UnicodeString identifier(false, characters.get(), static_cast<int32_t>(icuIdentifier.length()));
    icu::TimeZone* zone = icu::TimeZone::createTimeZone(identifier);
    if (!zone)
        return nullptr;
    return std::unique_ptr<ICUTimeZone>(new ICUTimeZone(zone));
}

ICUTimeZone::~ICUTimeZone()
{
    delete static_cast<icu::TimeZone*>(m_zone);
}

auto ICUTimeZone::offsetAt(double epochMilliseconds) -> std::optional<Offsets>
{
    if (!isTimeForICU(epochMilliseconds))
        return std::nullopt;
    Offsets offsets;
    UErrorCode status = U_ZERO_ERROR;
    timeZone(m_zone).getOffset(epochMilliseconds, false, offsets.raw, offsets.dst, status);
    if (U_FAILURE(status))
        return std::nullopt;
    return offsets;
}

auto ICUTimeZone::offsetFromLocal(double localMilliseconds, LocalTime localTime) -> std::optional<Offsets>
{
    if (!isTimeForICU(localMilliseconds))
        return std::nullopt;
    Offsets offsets;
    UErrorCode status = U_ZERO_ERROR;
    auto option = localTime == LocalTime::Former ? UCAL_TZ_LOCAL_FORMER : UCAL_TZ_LOCAL_LATTER;
    basicTimeZone(m_zone).getOffsetFromLocal(localMilliseconds, option, option, offsets.raw, offsets.dst, status);
    if (U_FAILURE(status))
        return std::nullopt;
    return offsets;
}

std::optional<std::optional<double>> ICUTimeZone::transition(double epochMilliseconds, Transition transition)
{
    if (!isTimeForICU(epochMilliseconds))
        return std::nullopt;
    icu::TimeZoneTransition found;
    bool hasTransition = transition == Transition::Next
        ? basicTimeZone(m_zone).getNextTransition(epochMilliseconds, false, found)
        : basicTimeZone(m_zone).getPreviousTransition(epochMilliseconds, transition == Transition::PreviousInclusive, found);
    if (!hasTransition)
        return std::optional<double> { };
    return std::optional<double> { found.getTime() };
}

String ICUTimeZone::displayName(DisplayName displayName, const char* locale)
{
    // ICU writes into this buffer, and replaces it with one of its own if the name is longer.
    std::array<char16_t, 64> buffer;
    icu::UnicodeString name(buffer.data(), 0, static_cast<int32_t>(buffer.size()));
    timeZone(m_zone).getDisplayName(displayName == DisplayName::Daylight, icu::TimeZone::LONG, icu::Locale(locale), name);
    if (name.isBogus())
        return { };
    return String(std::span<const char16_t> { name.getBuffer(), static_cast<size_t>(name.length()) });
}

#else // !USE(ICU_TIME_ZONE_OBJECT)

static UCalendar* calendar(void* zone)
{
    return static_cast<UCalendar*>(zone);
}

std::unique_ptr<ICUTimeZone> ICUTimeZone::create(StringView icuIdentifier)
{
    auto characters = icuIdentifier.upconvertedCharacters();
    UErrorCode status = U_ZERO_ERROR;
    // Only the zone of this calendar is read, so its locale and its kind do not matter.
    UCalendar* zone = ucal_open(characters, icuIdentifier.length(), "", UCAL_DEFAULT, &status);
    if (U_FAILURE(status) || !zone)
        return nullptr;
    return std::unique_ptr<ICUTimeZone>(new ICUTimeZone(zone));
}

ICUTimeZone::~ICUTimeZone()
{
    ucal_close(calendar(m_zone));
}

auto ICUTimeZone::offsetAt(double epochMilliseconds) -> std::optional<Offsets>
{
    UErrorCode status = U_ZERO_ERROR;
    ucal_setMillis(calendar(m_zone), epochMilliseconds, &status);
    if (U_FAILURE(status))
        return std::nullopt;
    Offsets offsets;
    offsets.raw = ucal_get(calendar(m_zone), UCAL_ZONE_OFFSET, &status);
    if (U_FAILURE(status))
        return std::nullopt;
    offsets.dst = ucal_get(calendar(m_zone), UCAL_DST_OFFSET, &status);
    if (U_FAILURE(status))
        return std::nullopt;
    return offsets;
}

auto ICUTimeZone::offsetFromLocal(double localMilliseconds, LocalTime localTime) -> std::optional<Offsets>
{
    UErrorCode status = U_ZERO_ERROR;
    // ucal_getTimeZoneOffsetFromLocal reads the time of the calendar as a wall-clock time.
    ucal_setMillis(calendar(m_zone), localMilliseconds, &status);
    if (U_FAILURE(status))
        return std::nullopt;
    Offsets offsets;
    auto option = localTime == LocalTime::Former ? UCAL_TZ_LOCAL_FORMER : UCAL_TZ_LOCAL_LATTER;
    ucal_getTimeZoneOffsetFromLocal(calendar(m_zone), option, option, &offsets.raw, &offsets.dst, &status);
    if (U_FAILURE(status))
        return std::nullopt;
    return offsets;
}

std::optional<std::optional<double>> ICUTimeZone::transition(double epochMilliseconds, Transition transition)
{
    UErrorCode status = U_ZERO_ERROR;
    ucal_setMillis(calendar(m_zone), epochMilliseconds, &status);
    if (U_FAILURE(status))
        return std::nullopt;
    UTimeZoneTransitionType type = UCAL_TZ_TRANSITION_NEXT;
    if (transition == Transition::Previous)
        type = UCAL_TZ_TRANSITION_PREVIOUS;
    else if (transition == Transition::PreviousInclusive)
        type = UCAL_TZ_TRANSITION_PREVIOUS_INCLUSIVE;
    UDate found = 0;
    bool hasTransition = ucal_getTimeZoneTransitionDate(calendar(m_zone), type, &found, &status);
    if (U_FAILURE(status))
        return std::nullopt;
    if (!hasTransition)
        return std::optional<double> { };
    return std::optional<double> { found };
}

String ICUTimeZone::displayName(DisplayName displayName, const char* locale)
{
    Vector<char16_t, 32> buffer;
    auto status = callBufferProducingFunction(ucal_getTimeZoneDisplayName, calendar(m_zone), displayName == DisplayName::Daylight ? UCAL_DST : UCAL_STANDARD, locale, buffer);
    if (U_FAILURE(status))
        return { };
    return String::adopt(WTF::move(buffer));
}

#endif // USE(ICU_TIME_ZONE_OBJECT)

} // namespace JSC
