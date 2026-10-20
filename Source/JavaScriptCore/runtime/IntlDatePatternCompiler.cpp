/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "IntlFormatPatternCompiler.h"

#include "JSCInlines.h"
#include <cmath>
#include <unicode/udat.h>
#include <wtf/ASCIICType.h>
#include <wtf/unicode/icu/ICUHelpers.h>

#include <unicode/uformattedvalue.h>
#ifdef U_HIDE_DRAFT_API
#undef U_HIDE_DRAFT_API
#endif
#include <unicode/udateintervalformat.h>
#define U_HIDE_DRAFT_API 1

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

bool IntlFormatPatternCompiler::appendSymbols(PatternCode& code, DateTimePatternOp op, const UDateFormat* dateFormat, UDateFormatSymbolType type, int32_t first, unsigned count)
{
    Vector<String, 12> names;
    for (unsigned i = 0; i < count; ++i) {
        Vector<char16_t, 32> name;
        if (U_FAILURE(callBufferProducingFunction(udat_getSymbols, dateFormat, type, first + static_cast<int32_t>(i), name)) || name.isEmpty())
            return false;
        names.append(withNormalSpaces(name.span()));
    }
    code.appendNames(op, names.span());
    return true;
}

bool IntlFormatPatternCompiler::appendField(PatternCode& code, const UDateFormat* dateFormat, char16_t letter, unsigned count)
{
    auto number = [&](DateTimePatternOp oneDigit, DateTimePatternOp twoDigits) {
        if (count > 2)
            return false;
        code.append(count == 1 ? oneDigit : twoDigits);
        code.didAppendField(2);
        return true;
    };
    auto months = [&](UDateFormatSymbolType abbreviated, UDateFormatSymbolType wide, UDateFormatSymbolType narrow) {
        if (count <= 2)
            return number(DateTimePatternOp::Month, DateTimePatternOp::TwoDigitMonth);
        return count <= 5 && appendSymbols(code, DateTimePatternOp::MonthName, dateFormat, count == 3 ? abbreviated : count == 4 ? wide : narrow, 0, 12);
    };
    auto weekdays = [&](UDateFormatSymbolType abbreviated, UDateFormatSymbolType wide, UDateFormatSymbolType narrow, UDateFormatSymbolType shorter) {
        return count <= 6 && appendSymbols(code, DateTimePatternOp::WeekdayName, dateFormat, count <= 3 ? abbreviated : count == 4 ? wide : count == 5 ? narrow : shorter, 1, 7);
    };
    switch (letter) {
    case 'y':
        if (count > 2)
            return false;
        code.append(count == 1 ? DateTimePatternOp::Year : DateTimePatternOp::TwoDigitYear);
        code.didAppendField(count == 1 ? maxDecimalLengthOfYear : 2);
        return true;
    case 'M':
        return months(UDAT_SHORT_MONTHS, UDAT_MONTHS, UDAT_NARROW_MONTHS);
    case 'L':
        return months(UDAT_STANDALONE_SHORT_MONTHS, UDAT_STANDALONE_MONTHS, UDAT_STANDALONE_NARROW_MONTHS);
    case 'd':
        return number(DateTimePatternOp::Day, DateTimePatternOp::TwoDigitDay);
    case 'E':
        return weekdays(UDAT_SHORT_WEEKDAYS, UDAT_WEEKDAYS, UDAT_NARROW_WEEKDAYS, UDAT_SHORTER_WEEKDAYS);
    case 'c':
        return count >= 3 && weekdays(UDAT_STANDALONE_SHORT_WEEKDAYS, UDAT_STANDALONE_WEEKDAYS, UDAT_STANDALONE_NARROW_WEEKDAYS, UDAT_STANDALONE_SHORTER_WEEKDAYS);
    case 'a':
        return count <= 3 && appendSymbols(code, DateTimePatternOp::DayPeriodName, dateFormat, UDAT_AM_PMS, 0, 2);
    case 'H':
        return number(DateTimePatternOp::Hour23, DateTimePatternOp::TwoDigitHour23);
    case 'k':
        return number(DateTimePatternOp::Hour24, DateTimePatternOp::TwoDigitHour24);
    case 'h':
        return number(DateTimePatternOp::Hour12, DateTimePatternOp::TwoDigitHour12);
    case 'K':
        return number(DateTimePatternOp::Hour11, DateTimePatternOp::TwoDigitHour11);
    case 'm':
        return number(DateTimePatternOp::Minute, DateTimePatternOp::TwoDigitMinute);
    case 's':
        return number(DateTimePatternOp::Second, DateTimePatternOp::TwoDigitSecond);
    case 'S':
        if (count > 3)
            return false;
        code.append(count == 1 ? DateTimePatternOp::Tenths : count == 2 ? DateTimePatternOp::Hundredths : DateTimePatternOp::Milliseconds);
        code.didAppendField(count);
        return true;
    default:
        return false;
    }
}

std::optional<PatternCode> IntlFormatPatternCompiler::codeFromPatternOf(const UDateFormat* dateFormat)
{
    Vector<char16_t, 64> pattern;
    if (U_FAILURE(callBufferProducingFunction(udat_toPattern, dateFormat, false, pattern))) {
        decline(PatternDecline::PatternNotReadable);
        return std::nullopt;
    }
    PatternCode code;
    Vector<char16_t, 32> literal;
    auto flushLiteral = [&] {
        code.appendLiteral(withNormalSpaces(literal.span()));
        literal.shrink(0);
    };
    for (size_t i = 0; i < pattern.size();) {
        char16_t character = pattern[i];
        if (character == '\'') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '\'') {
                literal.append('\'');
                i += 2;
                continue;
            }
            for (++i;; ++i) {
                if (i == pattern.size()) {
                    decline(PatternDecline::PatternNotReadable);
                    return std::nullopt;
                }
                if (pattern[i] != '\'') {
                    literal.append(pattern[i]);
                    continue;
                }
                if (i + 1 == pattern.size() || pattern[i + 1] != '\'')
                    break;
                literal.append('\'');
                ++i;
            }
            ++i;
            continue;
        }
        if (!isASCIIAlpha(character)) {
            literal.append(character);
            ++i;
            continue;
        }
        size_t end = i;
        while (end < pattern.size() && pattern[end] == character)
            ++end;
        flushLiteral();
        if (!appendField(code, dateFormat, character, end - i)) {
            decline(PatternDecline::PatternLetterNotSupported);
            return std::nullopt;
        }
        i = end;
    }
    flushLiteral();
    code.append(DateTimePatternOp::End);
    return code;
}

Vector<double> IntlFormatPatternCompiler::sampleTimeValues(int32_t utcOffsetInMinutes)
{
    Vector<double> timeValues;
    constexpr double second = 1000;
    constexpr double minute = 60 * second;
    constexpr double hour = 60 * minute;
    constexpr double day = 24 * hour;
    constexpr double startOf2001 = 978307200000.0;
    double base = startOf2001 + 33 * day + 4 * hour + 5 * minute + 6 * second + 789 - utcOffsetInMinutes * minute;
    for (unsigned i = 0; i < 48; ++i)
        timeValues.append(base + i * (31 * day + hour + 7 * minute + 11 * second + 101));
    constexpr double daysIn400Years = 146097;
    for (double years : { -2000.0, -1999.0, -1992.0, -1991.0, -1990.0, -1902.0, -1901.0, -1900.0, -1002.0, -1001.0, -1000.0, -419.0, -401.0, -101.0, -32.0, -31.0, -2.0, -1.0, 23.0, 37.0, 7998.0, 7999.0, 10344.0, 97998.0, 97999.0, 273000.0 })
        timeValues.append(base + std::floor(years * daysIn400Years / 400) * day);

    PseudoRandomNumbers random;
    constexpr double startOf1900 = -2208988800000.0;
    constexpr double startOf2100 = 4102444800000.0;
    for (unsigned i = 0; i < 8; ++i) {
        if (i & 1)
            timeValues.append(std::floor(startOf1900 + random.next() * (startOf2100 - startOf1900)));
        else
            timeValues.append(std::floor(firstTimeValueOfYear1 + day + random.next() * (maxTimeValue - 2 * day - firstTimeValueOfYear1)));
    }
    return timeValues;
}

bool IntlFormatPatternCompiler::agreesOnSamples(const IntlDateTimeFormat& formatter, IntlFormatPattern& pattern)
{
    for (double timeValue : sampleTimeValues(pattern.header().utcOffsetInMinutes)) {
        String text = pattern.tryFormatDateTime(timeValue);
        if (text.isNull() || text != formatter.tryFormatWithICU(timeValue))
            return false;
    }
    return true;
}

bool IntlFormatPatternCompiler::agreesAtTimeZoneTransitions(const IntlDateTimeFormat& formatter, IntlFormatPattern& pattern)
{
    constexpr double hour = 3600000;
    constexpr double startOf1900 = -2208988800000.0;
    constexpr double startOf2024 = 1704067200000.0;
    constexpr double startOf2100 = 4102444800000.0;
    constexpr double year = 365 * 24 * hour;
    Vector<double> timeValues;
    for (unsigned i = 0; i < 366 * 4; i += 61)
        timeValues.append(startOf2024 + i * 6 * hour + 1799999);
    PseudoRandomNumbers random;
    for (unsigned i = 0; i < 12; ++i)
        timeValues.append(std::floor(i % 3 ? startOf1900 + random.next() * (startOf2100 - startOf1900) : firstTimeValueOfYear1 + 48 * hour + random.next() * (maxTimeValue - 96 * hour - firstTimeValueOfYear1)));
    auto agrees = [&](double timeValue) {
        String text = pattern.tryFormatDateTime(timeValue);
        return !text.isNull() && text == formatter.tryFormatWithICU(timeValue);
    };
    for (double timeValue : timeValues) {
        if (!agrees(timeValue))
            return false;
    }
    for (double transition = startOf2024 + 2 * year; std::isfinite(transition) && transition > startOf2024 - year; transition = pattern.m_utcOffsetRange.start) {
        if (!agrees(transition) || !agrees(transition - 1))
            return false;
    }
    return true;
}

static std::optional<int32_t> fixedOffsetInMinutes(const TimeZone& timeZone)
{
    if (timeZone.isUTCOffset())
        return static_cast<int32_t>(timeZone.utcOffsetNanoseconds() / static_cast<int64_t>(ISO8601::ExactTime::nsPerMinute));
    if (intlPrimaryTimeZoneID(timeZone.id()) == utcTimeZoneID())
        return 0;
    return std::nullopt;
}

std::unique_ptr<IntlFormatPattern> IntlFormatPatternCompiler::compile(const IntlDateTimeFormat& formatter)
{
    auto& impl = formatter.impl();
    if (formatter.ensureCalendar() != "gregory"_s || formatter.ensureNumberingSystem() != "latn"_s)
        return decline(PatternDecline::CalendarOrNumberingSystemNotSupported);
    if (impl.m_era != IntlDateTimeFormat::Era::None)
        return decline(PatternDecline::OptionNotSupported);
    auto code = codeFromPatternOf(impl.m_dateFormat.get());
    if (!code)
        return nullptr;
    auto utcOffsetInMinutes = fixedOffsetInMinutes(impl.m_timeZone);
    auto timeZone = utcOffsetInMinutes ? FormatPatternTimeZone::FixedOffset : FormatPatternTimeZone::Named;
    auto bytes = code->finish(FormatPatternKind::DateTime, timeZone, utcOffsetInMinutes.value_or(0));
    if (!bytes)
        return decline(PatternDecline::PatternTooLong);
    auto pattern = makeUnique<IntlFormatPattern>(WTF::move(*bytes), utcOffsetInMinutes ? String() : impl.m_timeZone.toICUString());
    if (!agreesOnSamples(formatter, *pattern))
        return decline(PatternDecline::DiffersFromICU);
    if (timeZone == FormatPatternTimeZone::Named && !agreesAtTimeZoneTransitions(formatter, *pattern))
        return decline(PatternDecline::DiffersFromICU);
    return pattern;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
