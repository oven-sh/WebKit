/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "IntlFormatPattern.h"

#include <cmath>
#include <limits>
#include <span>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringView.h>
#include <wtf/unicode/icu/ICUHelpers.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(IntlFormatPattern);

IntlFormatPattern::IntlFormatPattern(Vector<uint8_t>&& bytes, String&& timeZoneID)
    : m_bytes(WTF::move(bytes))
    , m_timeZoneID(WTF::move(timeZoneID))
{
}

IntlFormatPattern::~IntlFormatPattern()
{
    if (m_calendar)
        ucal_close(m_calendar);
}

template<typename CharacterType, typename Write>
static ALWAYS_INLINE String tryFormatWithPattern(const FormatPatternHeader& header, const Write& write)
{
    CharacterType buffer[maxFormatPatternLength];
    CharacterType* end = write(buffer, reinterpret_cast<const CharacterType*>(&header + 1));
    if (!end) [[unlikely]]
        return { };
    return String(std::span<const CharacterType> { buffer, end });
}

static String tryFormatFields(const FormatPatternHeader& header, const std::optional<UTCDateTime>& fields)
{
    if (!fields) [[unlikely]]
        return { };
    auto write = [&](auto* cursor, auto* code) {
        return writeDateTime(cursor, code, *fields);
    };
    return header.is16Bit ? tryFormatWithPattern<char16_t>(header, write) : tryFormatWithPattern<Latin1Character>(header, write);
}

String IntlFormatPattern::tryFormatDateTime(double timeValue)
{
    ASSERT(header().kind == FormatPatternKind::DateTime);
    switch (header().timeZone) {
    case FormatPatternTimeZone::FixedOffset:
        return tryFormatFields(header(), fieldsForDateTimePattern(header(), timeValue));
    case FormatPatternTimeZone::Named:
        timeValue = std::trunc(timeValue);
        if (!(timeValue >= m_utcOffsetRange.start && timeValue < m_utcOffsetRange.end)) [[unlikely]] {
            if (!findUTCOffset(timeValue))
                return { };
        }
        return tryFormatFields(header(), fieldsAtUTCOffset(timeValue, m_utcOffsetRange.offsetInMilliseconds));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

NEVER_INLINE bool IntlFormatPattern::findUTCOffset(double timeValue)
{
    if (!(std::abs(timeValue) <= maxTimeValue))
        return false;
    UErrorCode status = U_ZERO_ERROR;
    if (!m_calendar) {
        auto characters = StringView(m_timeZoneID).upconvertedCharacters();
        auto calendar = std::unique_ptr<UCalendar, ICUDeleter<ucal_close>>(ucal_open(characters.get(), m_timeZoneID.length(), "", UCAL_GREGORIAN, &status));
        if (U_FAILURE(status))
            return false;
        m_calendar = calendar.release();
    }
    ucal_setMillis(m_calendar, timeValue, &status);
    double offset = ucal_get(m_calendar, UCAL_ZONE_OFFSET, &status);
    offset += ucal_get(m_calendar, UCAL_DST_OFFSET, &status);
    UDate previous = 0;
    UDate next = 0;
    bool hasPrevious = ucal_getTimeZoneTransitionDate(m_calendar, UCAL_TZ_TRANSITION_PREVIOUS_INCLUSIVE, &previous, &status);
    bool hasNext = ucal_getTimeZoneTransitionDate(m_calendar, UCAL_TZ_TRANSITION_NEXT, &next, &status);
    if (U_FAILURE(status))
        return false;
    constexpr double infinity = std::numeric_limits<double>::infinity();
    m_utcOffsetRange = { hasPrevious ? previous : -infinity, hasNext ? next : infinity, offset };
    return true;
}

String IntlFormatPattern::tryFormatNumber(double number) const
{
    ASSERT(header().kind == FormatPatternKind::Number);
    auto write = [&](auto* cursor, auto* code) {
        return writeNumber(cursor, code, number);
    };
    return header().is16Bit ? tryFormatWithPattern<char16_t>(header(), write) : tryFormatWithPattern<Latin1Character>(header(), write);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
