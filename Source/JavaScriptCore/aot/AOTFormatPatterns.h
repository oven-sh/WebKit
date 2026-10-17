/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDateTimeWriter.h"
#include <optional>
#include <wtf/text/Latin1Character.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

enum class FormatPatternKind : uint8_t {
    DateTime,
    Integer,
};

struct FormatPatternHeader {
    FormatPatternKind kind;
    uint8_t is16Bit;
    uint16_t maxLength;
    int32_t utcOffsetInMinutes;
};
static_assert(sizeof(FormatPatternHeader) == 8);

enum class DateTimePatternOp : uint8_t {
    End,
    Literal,
    Year,
    TwoDigitYear,
    Month,
    TwoDigitMonth,
    MonthName,
    Day,
    TwoDigitDay,
    WeekdayName,
    Hour23,
    TwoDigitHour23,
    Hour24,
    TwoDigitHour24,
    Hour12,
    TwoDigitHour12,
    Hour11,
    TwoDigitHour11,
    Minute,
    TwoDigitMinute,
    Second,
    TwoDigitSecond,
    Tenths,
    Hundredths,
    Milliseconds,
    DayPeriodName,
};

static constexpr unsigned maxFormatPatternLength = 256;
static constexpr unsigned maxDecimalLengthOfYear = 6;
static constexpr unsigned maxDecimalLengthOfInteger = 20;

ALWAYS_INLINE unsigned patternUnit(Latin1Character unit) { return unit.value; }
ALWAYS_INLINE unsigned patternUnit(char16_t unit) { return unit; }

template<typename CharacterType>
ALWAYS_INLINE CharacterType* writeCharacters(CharacterType* cursor, const CharacterType* characters, unsigned length)
{
    for (unsigned i = 0; i < length; ++i)
        cursor[i] = characters[i];
    return cursor + length;
}

template<typename CharacterType>
ALWAYS_INLINE CharacterType* writeDecimal(CharacterType* cursor, uint64_t value)
{
    CharacterType digits[maxDecimalLengthOfInteger];
    unsigned length = 0;
    do {
        digits[length++] = static_cast<CharacterType>('0' + value % 10);
        value /= 10;
    } while (value);
    while (length)
        *cursor++ = digits[--length];
    return cursor;
}

template<typename CharacterType>
inline CharacterType* writeDateTime(CharacterType* cursor, const CharacterType* code, const UTCDateTime& fields)
{
    auto writeName = [&](unsigned index) ALWAYS_INLINE_LAMBDA {
        unsigned count = patternUnit(code[0]);
        unsigned stride = patternUnit(code[1]);
        const CharacterType* name = code + 2 + index * stride;
        cursor = writeCharacters(cursor, name + 1, patternUnit(name[0]));
        code += 2 + count * stride;
    };
    for (;;) {
        switch (static_cast<DateTimePatternOp>(patternUnit(*code++))) {
        case DateTimePatternOp::End:
            return cursor;
        case DateTimePatternOp::Literal: {
            unsigned length = patternUnit(*code++);
            cursor = writeCharacters(cursor, code, length);
            code += length;
            break;
        }
        case DateTimePatternOp::Year:
            cursor = writeDecimal(cursor, static_cast<uint32_t>(fields.year));
            break;
        case DateTimePatternOp::TwoDigitYear:
            cursor = writeTwoDigits(cursor, static_cast<uint32_t>(fields.year) % 100);
            break;
        case DateTimePatternOp::Month:
            cursor = writeOneOrTwoDigits(cursor, fields.month);
            break;
        case DateTimePatternOp::TwoDigitMonth:
            cursor = writeTwoDigits(cursor, fields.month);
            break;
        case DateTimePatternOp::MonthName:
            writeName(fields.month - 1);
            break;
        case DateTimePatternOp::Day:
            cursor = writeOneOrTwoDigits(cursor, fields.day);
            break;
        case DateTimePatternOp::TwoDigitDay:
            cursor = writeTwoDigits(cursor, fields.day);
            break;
        case DateTimePatternOp::WeekdayName:
            writeName(fields.weekDay);
            break;
        case DateTimePatternOp::Hour23:
            cursor = writeOneOrTwoDigits(cursor, fields.hour);
            break;
        case DateTimePatternOp::TwoDigitHour23:
            cursor = writeTwoDigits(cursor, fields.hour);
            break;
        case DateTimePatternOp::Hour24:
            cursor = writeOneOrTwoDigits(cursor, fields.hour ? fields.hour : 24);
            break;
        case DateTimePatternOp::TwoDigitHour24:
            cursor = writeTwoDigits(cursor, fields.hour ? fields.hour : 24);
            break;
        case DateTimePatternOp::Hour12:
            cursor = writeOneOrTwoDigits(cursor, fields.hour % 12 ? fields.hour % 12 : 12);
            break;
        case DateTimePatternOp::TwoDigitHour12:
            cursor = writeTwoDigits(cursor, fields.hour % 12 ? fields.hour % 12 : 12);
            break;
        case DateTimePatternOp::Hour11:
            cursor = writeOneOrTwoDigits(cursor, fields.hour % 12);
            break;
        case DateTimePatternOp::TwoDigitHour11:
            cursor = writeTwoDigits(cursor, fields.hour % 12);
            break;
        case DateTimePatternOp::Minute:
            cursor = writeOneOrTwoDigits(cursor, fields.minute);
            break;
        case DateTimePatternOp::TwoDigitMinute:
            cursor = writeTwoDigits(cursor, fields.minute);
            break;
        case DateTimePatternOp::Second:
            cursor = writeOneOrTwoDigits(cursor, fields.second);
            break;
        case DateTimePatternOp::TwoDigitSecond:
            cursor = writeTwoDigits(cursor, fields.second);
            break;
        case DateTimePatternOp::Tenths:
            *cursor++ = static_cast<CharacterType>('0' + fields.millisecond / 100);
            break;
        case DateTimePatternOp::Hundredths:
            cursor = writeTwoDigits(cursor, fields.millisecond / 10);
            break;
        case DateTimePatternOp::Milliseconds:
            *cursor++ = static_cast<CharacterType>('0' + fields.millisecond / 100);
            cursor = writeTwoDigits(cursor, fields.millisecond % 100);
            break;
        case DateTimePatternOp::DayPeriodName:
            writeName(fields.hour >= 12);
            break;
        }
    }
}

static constexpr double firstTimeValueOfYear1 = -62135596800000.0;
static constexpr double maxTimeValue = 8.64e15;

inline std::optional<UTCDateTime> fieldsForDateTimePattern(const FormatPatternHeader& header, double timeValue)
{
    if (!(std::abs(timeValue) <= maxTimeValue))
        return std::nullopt;
    double shifted = std::trunc(timeValue) + header.utcOffsetInMinutes * 60000.0;
    if (shifted < firstTimeValueOfYear1 || shifted > maxTimeValue)
        return std::nullopt;
    return utcDateTimeFromTimeValue(shifted);
}

enum class IntegerPatternText : uint8_t {
    PositivePrefix,
    PositiveSuffix,
    NegativePrefix,
    NegativeSuffix,
    GroupSeparator,
};
static constexpr unsigned numberOfIntegerPatternTexts = 5;

template<typename CharacterType>
inline CharacterType* writeInteger(CharacterType* cursor, const CharacterType* code, int64_t integer)
{
    unsigned primaryGroupSize = patternUnit(code[0]);
    unsigned secondaryGroupSize = patternUnit(code[1]);
    unsigned minDigitsToGroup = patternUnit(code[2]);
    code += 3;
    const CharacterType* texts[numberOfIntegerPatternTexts];
    for (auto& text : texts) {
        text = code;
        code += 1 + patternUnit(code[0]);
    }
    auto textOf = [&](IntegerPatternText which) { return texts[static_cast<unsigned>(which)]; };
    bool isNegative = integer < 0;
    uint64_t magnitude = isNegative ? -static_cast<uint64_t>(integer) : static_cast<uint64_t>(integer);
    const CharacterType* prefix = textOf(isNegative ? IntegerPatternText::NegativePrefix : IntegerPatternText::PositivePrefix);
    const CharacterType* suffix = textOf(isNegative ? IntegerPatternText::NegativeSuffix : IntegerPatternText::PositiveSuffix);
    cursor = writeCharacters(cursor, prefix + 1, patternUnit(prefix[0]));

    CharacterType digits[maxDecimalLengthOfInteger];
    unsigned length = 0;
    do {
        digits[length++] = static_cast<CharacterType>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (!primaryGroupSize || length < minDigitsToGroup) {
        while (length)
            *cursor++ = digits[--length];
    } else {
        const CharacterType* separator = textOf(IntegerPatternText::GroupSeparator);
        while (length) {
            *cursor++ = digits[--length];
            if (length >= primaryGroupSize && !((length - primaryGroupSize) % secondaryGroupSize))
                cursor = writeCharacters(cursor, separator + 1, patternUnit(separator[0]));
        }
    }
    return writeCharacters(cursor, suffix + 1, patternUnit(suffix[0]));
}

static constexpr int64_t maxMagnitudeOfFormattedInteger = 9007199254740991;

inline std::optional<int64_t> integerForPattern(double number)
{
    if (!(std::abs(number) <= static_cast<double>(maxMagnitudeOfFormattedInteger)))
        return std::nullopt;
    int64_t integer = static_cast<int64_t>(number);
    if (static_cast<double>(integer) != number || (!integer && std::signbit(number)))
        return std::nullopt;
    return integer;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
