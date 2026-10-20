/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/DateTimeWriter.h>
#include <memory>
#include <optional>
#include <unicode/ucal.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/dragonbox/dragonbox.h>
#include <wtf/text/Latin1Character.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class IntlDateTimeFormat;
class IntlNumberFormat;
class JSGlobalObject;
class VM;

enum class FormatPatternKind : uint8_t {
    DateTime,
    Number,
};

enum class FormatPatternTimeZone : uint8_t {
    FixedOffset,
    Named,
};

struct FormatPatternHeader {
    FormatPatternKind kind;
    bool is16Bit;
    FormatPatternTimeZone timeZone;
    uint16_t maxLength;
    int32_t utcOffsetInMinutes;
};
static_assert(sizeof(FormatPatternHeader) == 12);

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

inline std::optional<UTCDateTime> fieldsAtUTCOffset(double timeValue, double utcOffsetInMilliseconds)
{
    if (!(std::abs(timeValue) <= maxTimeValue))
        return std::nullopt;
    double shifted = std::trunc(timeValue) + utcOffsetInMilliseconds;
    if (shifted < firstTimeValueOfYear1 || shifted > maxTimeValue)
        return std::nullopt;
    return utcDateTimeFromTimeValue(shifted);
}

inline std::optional<UTCDateTime> fieldsForDateTimePattern(const FormatPatternHeader& header, double timeValue)
{
    return fieldsAtUTCOffset(timeValue, header.utcOffsetInMinutes * 60000.0);
}

enum class NumberPatternParameter : uint8_t {
    PrimaryGroupSize,
    SecondaryGroupSize,
    MinDigitsToGroup,
    MinFractionDigits,
    MaxFractionDigits,
    MinIntegerDigits,
    DecimalShift,
};
static constexpr unsigned numberOfNumberPatternParameters = 7;

enum class NumberPatternText : uint8_t {
    PositivePrefix,
    PositiveSuffix,
    NegativePrefix,
    NegativeSuffix,
    GroupSeparator,
    DecimalSeparator,
};
static constexpr unsigned numberOfNumberPatternTexts = 6;

struct DecimalNumber {
    bool isNegative;
    uint8_t fractionDigits;
    uint64_t digits;
};

static constexpr unsigned maxFractionDigitsInPattern = 15;
static constexpr double maxMagnitudeOfFormattedNumber = 9007199254740991.0;
static constexpr double minMagnitudeOfFormattedFraction = 1e-6;

ALWAYS_INLINE uint64_t powerOfTen(unsigned exponent)
{
    static constexpr uint64_t powers[] = {
        1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull, 100000000ull, 1000000000ull,
        10000000000ull, 100000000000ull, 1000000000000ull, 10000000000000ull, 100000000000000ull, 1000000000000000ull,
        10000000000000000ull, 100000000000000000ull, 1000000000000000000ull, 10000000000000000000ull,
    };
    ASSERT(exponent < std::size(powers));
    return powers[exponent];
}

inline std::optional<DecimalNumber> decimalNumberForPattern(double number, unsigned maxFractionDigits, unsigned decimalShift)
{
    double magnitude = std::abs(number);
    if (!(magnitude <= maxMagnitudeOfFormattedNumber))
        return std::nullopt;
    bool isNegative = std::signbit(number);
    uint64_t digits = static_cast<uint64_t>(magnitude);
    unsigned fractionDigits = 0;
    if (static_cast<double>(digits) != magnitude) {
        if (magnitude < minMagnitudeOfFormattedFraction)
            return std::nullopt;
        auto shortest = WTF::dragonbox::to_decimal(magnitude);
        ASSERT(shortest.exponent < 0);
        digits = shortest.significand;
        fractionDigits = static_cast<unsigned>(-shortest.exponent);
        if (fractionDigits >= decimalShift)
            fractionDigits -= decimalShift;
        else {
            digits *= powerOfTen(decimalShift - fractionDigits);
            fractionDigits = 0;
        }
        if (fractionDigits > maxFractionDigits) {
            unsigned dropped = fractionDigits - maxFractionDigits;
            if (dropped > 19)
                digits = 0;
            else {
                uint64_t divisor = powerOfTen(dropped);
                uint64_t remainder = digits % divisor;
                digits = digits / divisor + (remainder >= divisor / 2);
            }
            fractionDigits = maxFractionDigits;
        }
        for (; fractionDigits && !(digits % 10); --fractionDigits)
            digits /= 10;
    } else
        digits *= powerOfTen(decimalShift);
    if (!digits && isNegative)
        return std::nullopt;
    return DecimalNumber { isNegative, static_cast<uint8_t>(fractionDigits), digits };
}

template<typename CharacterType>
inline CharacterType* writeNumber(CharacterType* cursor, const CharacterType* code, double number)
{
    auto parameter = [&](NumberPatternParameter which) { return patternUnit(code[static_cast<unsigned>(which)]); };
    auto decimal = decimalNumberForPattern(number, parameter(NumberPatternParameter::MaxFractionDigits), parameter(NumberPatternParameter::DecimalShift));
    if (!decimal) [[unlikely]]
        return nullptr;
    unsigned primaryGroupSize = parameter(NumberPatternParameter::PrimaryGroupSize);
    unsigned secondaryGroupSize = parameter(NumberPatternParameter::SecondaryGroupSize);
    unsigned minDigitsToGroup = parameter(NumberPatternParameter::MinDigitsToGroup);
    unsigned minFractionDigits = parameter(NumberPatternParameter::MinFractionDigits);
    const CharacterType* texts[numberOfNumberPatternTexts];
    const CharacterType* next = code + numberOfNumberPatternParameters;
    for (auto& text : texts) {
        text = next;
        next += 1 + patternUnit(next[0]);
    }
    auto writeText = [&](NumberPatternText which) ALWAYS_INLINE_LAMBDA {
        const CharacterType* text = texts[static_cast<unsigned>(which)];
        cursor = writeCharacters(cursor, text + 1, patternUnit(text[0]));
    };
    writeText(decimal->isNegative ? NumberPatternText::NegativePrefix : NumberPatternText::PositivePrefix);

    uint64_t scale = powerOfTen(decimal->fractionDigits);
    uint64_t integerPart = decimal->digits / scale;
    uint64_t fraction = decimal->digits % scale;
    CharacterType digits[maxDecimalLengthOfInteger];
    unsigned length = 0;
    do {
        digits[length++] = static_cast<CharacterType>('0' + integerPart % 10);
        integerPart /= 10;
    } while (integerPart);
    while (length < parameter(NumberPatternParameter::MinIntegerDigits))
        digits[length++] = '0';
    if (!primaryGroupSize || length < minDigitsToGroup) {
        while (length)
            *cursor++ = digits[--length];
    } else {
        while (length) {
            *cursor++ = digits[--length];
            if (length >= primaryGroupSize && !((length - primaryGroupSize) % secondaryGroupSize))
                writeText(NumberPatternText::GroupSeparator);
        }
    }
    if (decimal->fractionDigits || minFractionDigits) {
        writeText(NumberPatternText::DecimalSeparator);
        for (unsigned place = decimal->fractionDigits; place--;)
            *cursor++ = static_cast<CharacterType>('0' + fraction / powerOfTen(place) % 10);
        for (unsigned place = decimal->fractionDigits; place < minFractionDigits; ++place)
            *cursor++ = '0';
    }
    writeText(decimal->isNegative ? NumberPatternText::NegativeSuffix : NumberPatternText::PositiveSuffix);
    return cursor;
}

class IntlFormatPattern {
    WTF_MAKE_TZONE_ALLOCATED(IntlFormatPattern);
    WTF_MAKE_NONCOPYABLE(IntlFormatPattern);
public:
    static constexpr uint8_t numberOfFormatsBeforeCompiling = 16;

    IntlFormatPattern(Vector<uint8_t>&& bytes, String&& timeZoneID);
    ~IntlFormatPattern();

    const FormatPatternHeader& header() const { return *reinterpret_cast<const FormatPatternHeader*>(m_bytes.span().data()); }
    String tryFormatDateTime(double timeValue);
    String tryFormatNumber(double) const;

private:
    struct UTCOffsetRange {
        double start { 1 };
        double end { 0 };
        double offsetInMilliseconds { 0 };
    };

    friend class IntlFormatPatternCompiler;

    bool findUTCOffset(double timeValue);

    const Vector<uint8_t> m_bytes;
    const String m_timeZoneID;
    UTCOffsetRange m_utcOffsetRange;
    UCalendar* m_calendar { nullptr };
};

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
