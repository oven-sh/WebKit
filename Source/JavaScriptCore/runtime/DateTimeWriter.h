/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <cmath>
#include <wtf/Assertions.h>
#include <wtf/Compiler.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

struct UTCDateTime {
    int32_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekDay;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint16_t millisecond;
};

ALWAYS_INLINE UTCDateTime utcDateTimeFromTimeValue(double timeValue)
{
    ASSERT(std::isfinite(timeValue) && std::abs(timeValue) <= 8.64e15 && timeValue == std::trunc(timeValue));
    constexpr uint32_t daysPerEra = 146097;
    constexpr uint32_t yearsPerEra = 400;
    constexpr uint32_t erasBeforeOrigin = 700;
    constexpr uint32_t daysFromOriginToEpoch = 719468 + erasBeforeOrigin * daysPerEra;
    constexpr uint64_t millisecondsPerDay = 86400000;
    constexpr uint32_t weekDayOfOrigin = (4 + 7 - daysFromOriginToEpoch % 7) % 7;

    uint64_t milliseconds = static_cast<uint64_t>(static_cast<int64_t>(timeValue) + static_cast<int64_t>(daysFromOriginToEpoch * millisecondsPerDay));
    uint32_t days = static_cast<uint32_t>(milliseconds / millisecondsPerDay);
    uint32_t millisecondsInDay = static_cast<uint32_t>(milliseconds - days * millisecondsPerDay);

    uint32_t era = days / daysPerEra;
    uint32_t dayOfEra = days - era * daysPerEra;
    uint32_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    uint32_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    uint32_t monthFromMarch = (5 * dayOfYear + 2) / 153;
    uint32_t month = monthFromMarch < 10 ? monthFromMarch + 3 : monthFromMarch - 9;

    uint32_t seconds = millisecondsInDay / 1000;
    uint32_t minutes = seconds / 60;

    UTCDateTime result;
    result.year = static_cast<int32_t>(yearOfEra + era * yearsPerEra) - static_cast<int32_t>(erasBeforeOrigin * yearsPerEra) + (month <= 2);
    result.month = static_cast<uint8_t>(month);
    result.day = static_cast<uint8_t>(dayOfYear - (153 * monthFromMarch + 2) / 5 + 1);
    result.weekDay = static_cast<uint8_t>((days + weekDayOfOrigin) % 7);
    result.hour = static_cast<uint8_t>(minutes / 60);
    result.minute = static_cast<uint8_t>(minutes % 60);
    result.second = static_cast<uint8_t>(seconds % 60);
    result.millisecond = static_cast<uint16_t>(millisecondsInDay % 1000);
    return result;
}

template<typename CharacterType>
ALWAYS_INLINE CharacterType* writeTwoDigits(CharacterType* cursor, uint32_t value)
{
    ASSERT(value < 100);
    cursor[0] = static_cast<CharacterType>('0' + value / 10);
    cursor[1] = static_cast<CharacterType>('0' + value % 10);
    return cursor + 2;
}

template<typename CharacterType>
ALWAYS_INLINE CharacterType* writeOneOrTwoDigits(CharacterType* cursor, uint32_t value)
{
    ASSERT(value < 100);
    if (value < 10) {
        *cursor = static_cast<CharacterType>('0' + value);
        return cursor + 1;
    }
    return writeTwoDigits(cursor, value);
}

static constexpr unsigned maxISODateTimeLength = 27;

template<typename CharacterType>
ALWAYS_INLINE CharacterType* writeISODateTime(CharacterType* cursor, double timeValue)
{
    UTCDateTime fields = utcDateTimeFromTimeValue(timeValue);
    uint32_t year = static_cast<uint32_t>(fields.year);
    if (fields.year < 0 || fields.year > 9999) [[unlikely]] {
        *cursor++ = fields.year < 0 ? '-' : '+';
        year = static_cast<uint32_t>(std::abs(fields.year));
        cursor = writeTwoDigits(cursor, year / 10000);
        year %= 10000;
    }
    cursor = writeTwoDigits(cursor, year / 100);
    cursor = writeTwoDigits(cursor, year % 100);
    *cursor++ = '-';
    cursor = writeTwoDigits(cursor, fields.month);
    *cursor++ = '-';
    cursor = writeTwoDigits(cursor, fields.day);
    *cursor++ = 'T';
    cursor = writeTwoDigits(cursor, fields.hour);
    *cursor++ = ':';
    cursor = writeTwoDigits(cursor, fields.minute);
    *cursor++ = ':';
    cursor = writeTwoDigits(cursor, fields.second);
    *cursor++ = '.';
    *cursor++ = static_cast<CharacterType>('0' + fields.millisecond / 100);
    cursor = writeTwoDigits(cursor, fields.millisecond % 100);
    *cursor++ = 'Z';
    return cursor;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
