/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFormatPatternPlanner.h"

#if ENABLE(AOT)

#include "AOTRemovableFeature.h"
#include "AOTScratchRealm.h"
#include "JSCInlines.h"
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

static String keyOf(const FormatterRequest& request)
{
    StringBuilder builder;
    builder.append(static_cast<unsigned>(request.kind), '\0', static_cast<unsigned>(request.required), '\0', static_cast<unsigned>(request.defaults), '\0', request.locale, '\0', request.hasOptions ? '1' : '0');
    for (auto& option : request.options)
        builder.append('\0', option.name, '\0', static_cast<unsigned>(option.kind), '\0', option.string, '\0', option.number);
    return builder.toString();
}

static std::optional<WTF::Vector<uint8_t>> serializedArgumentsOf(const FormatterRequest& request)
{
    WTF::Vector<uint8_t> bytes;
    bool isValid = request.options.size() <= 0xff;
    auto appendText = [&](const String& text) {
        auto utf8 = text.utf8();
        isValid &= utf8.length() <= 0xffff;
        bytes.append(static_cast<uint8_t>(utf8.length()));
        bytes.append(static_cast<uint8_t>(utf8.length() >> 8));
        bytes.append(utf8.span());
    };
    appendText(request.locale);
    bytes.append(request.hasOptions);
    if (request.hasOptions)
        bytes.append(static_cast<uint8_t>(request.options.size()));
    for (auto& option : request.options) {
        bytes.append(static_cast<uint8_t>(option.kind));
        appendText(option.name);
        switch (option.kind) {
        case FormatterOptionKind::String:
            appendText(option.string);
            break;
        case FormatterOptionKind::Boolean:
            bytes.append(!!option.number);
            break;
        case FormatterOptionKind::Number:
            bytes.append(asByteSpan(option.number));
            break;
        }
    }
    if (!isValid)
        return std::nullopt;
    return bytes;
}

namespace {

class PatternCode {
public:
    void append(unsigned unit)
    {
        m_isValid &= unit <= 0xff;
        m_units.append(static_cast<char16_t>(unit));
    }
    void append(DateTimePatternOp op) { append(static_cast<unsigned>(op)); }
    void appendText(StringView text)
    {
        append(text.length());
        for (unsigned i = 0; i < text.length(); ++i) {
            m_is16Bit |= text[i] > 0xff;
            m_units.append(text[i]);
        }
    }
    void appendLiteral(StringView text)
    {
        if (text.isEmpty())
            return;
        append(DateTimePatternOp::Literal);
        appendText(text);
        m_maxLength += text.length();
    }
    void appendNames(DateTimePatternOp op, std::span<const String> names)
    {
        unsigned longest = 0;
        for (auto& name : names)
            longest = std::max(longest, name.length());
        append(op);
        append(names.size());
        append(longest + 1);
        for (auto& name : names) {
            appendText(name);
            for (unsigned i = name.length(); i < longest; ++i)
                m_units.append(0);
        }
        m_maxLength += longest;
    }
    void didAppendField(unsigned maxLength) { m_maxLength += maxLength; }

    std::optional<WTF::Vector<uint8_t>> finish(const FormatterRequest& request, FormatPatternTimeZone timeZone, int32_t utcOffsetInMinutes) const
    {
        auto constructorArguments = serializedArgumentsOf(request);
        size_t constructorArgumentsOffset = sizeof(FormatPatternHeader) + m_units.size() * (m_is16Bit ? 2 : 1);
        if (!m_isValid || m_maxLength > maxFormatPatternLength || !constructorArguments || constructorArgumentsOffset > 0xffff)
            return std::nullopt;
        FormatPatternHeader header {
            request.kind, m_is16Bit, timeZone, static_cast<uint8_t>(static_cast<unsigned>(request.required) << 4 | static_cast<unsigned>(request.defaults)),
            static_cast<uint16_t>(m_maxLength), static_cast<uint16_t>(constructorArgumentsOffset), static_cast<int16_t>(utcOffsetInMinutes), request.hasOptions, 0,
            { 0, 0 }
        };
        WTF::Vector<uint8_t> bytes;
        bytes.append(asByteSpan(header));
        if (m_is16Bit)
            bytes.append(asBytes(m_units.span()));
        else {
            for (char16_t unit : m_units)
                bytes.append(static_cast<uint8_t>(unit));
        }
        bytes.appendVector(*constructorArguments);
        return bytes;
    }

    bool operator==(const PatternCode&) const = default;

private:
    WTF::Vector<char16_t, 128> m_units;
    unsigned m_maxLength { 0 };
    bool m_is16Bit { false };
    bool m_isValid { true };
};

static const FormatPatternHeader& headerOf(const WTF::Vector<uint8_t>& pattern)
{
    return *reinterpret_cast<const FormatPatternHeader*>(pattern.span().data());
}

class PseudoRandomNumbers {
public:
    double next()
    {
        m_state = m_state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(m_state >> 11) / static_cast<double>(1ull << 53);
    }

private:
    uint64_t m_state { 0x9e3779b97f4a7c15ull };
};

template<auto function, typename... Arguments>
static ALWAYS_INLINE auto ofDateTimeFormat(Arguments... arguments)
{
    return callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, function>(arguments...);
}

template<auto function, typename... Arguments>
static ALWAYS_INLINE auto ofNumberFormat(Arguments... arguments)
{
    return callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, function>(arguments...);
}

// Nothing of Intl or ICU is called by name here: a direct call would make ICU part of what the compiler keeps in a trimmed executable.
class FormatPatternPlanner {
public:
    explicit FormatPatternPlanner(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
    {
    }

    std::optional<WTF::Vector<uint8_t>> plan(const FormatterRequest& request)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        SetForScope scopeOfThisRequest(m_scope, &scope);
        auto result = request.kind == FormatPatternKind::DateTime ? planDateTime(request) : planNumber(request);
        if (scope.exception()) [[unlikely]] {
            scope.clearExceptionExceptTermination();
            return std::nullopt;
        }
        return result;
    }

private:
    bool hasException() { return !!m_scope->exception(); }

    JSValue get(JSValue object, ASCIILiteral name)
    {
        if (hasException() || !object || !object.isObject())
            return { };
        JSValue value = asObject(object)->get(m_globalObject, Identifier::fromString(m_vm, name));
        return hasException() ? JSValue() : value;
    }

    JSValue call(JSValue function, JSValue receiver, std::initializer_list<JSValue> arguments)
    {
        if (hasException() || !function)
            return { };
        CallData callData = getCallData(function);
        if (callData.type == CallData::Type::None)
            return { };
        MarkedArgumentBuffer list;
        for (JSValue argument : arguments)
            list.append(argument);
        JSValue result = JSC::call(m_globalObject, function, callData, receiver, list);
        return hasException() ? JSValue() : result;
    }

    JSValue callMethod(JSValue receiver, ASCIILiteral name, std::initializer_list<JSValue> arguments = { })
    {
        return call(get(receiver, name), receiver, arguments);
    }

    String textOf(JSValue value)
    {
        if (!value || !value.isString())
            return { };
        String text = asString(value)->value(m_globalObject);
        return hasException() ? String() : text;
    }

    std::optional<unsigned> lengthOf(JSValue list)
    {
        JSValue length = get(list, "length"_s);
        if (!length || !length.isUInt32())
            return std::nullopt;
        return length.asUInt32();
    }

    struct Formatter {
        JSObject* object;
        JSValue format;
    };

    std::optional<Formatter> makeFormatter(const FormatterRequest& request)
    {
        auto arguments = PatternCode().finish(request, FormatPatternTimeZone::FixedOffset, 0);
        if (!arguments)
            return std::nullopt;
        JSObject* object = request.kind == FormatPatternKind::Number
            ? ofNumberFormat<newNumberFormatFromArguments>(m_globalObject, &headerOf(*arguments))
            : ofDateTimeFormat<newDateTimeFormatFromArguments>(m_globalObject, &headerOf(*arguments));
        JSValue format = get(object, "format"_s);
        if (!format)
            return std::nullopt;
        return Formatter { object, format };
    }

    // Another locale is formatted in the locale of the machine that the program runs on.
    bool isSupportedLocale(const Formatter& formatter, const String& locale)
    {
        return lengthOf(callMethod(get(formatter.object, "constructor"_s), "supportedLocalesOf"_s, { jsString(m_vm, locale) })) == 1u;
    }

    String formatted(const Formatter& formatter, double value)
    {
        return textOf(call(formatter.format, jsUndefined(), { jsNumber(value) }));
    }

    static bool isSameText(const String& written, const String& throughICU)
    {
        // jsString() answers the shared text for one character, whatever it is handed.
        return !written.isNull() && written == throughICU && (written.length() <= 1 || written.is8Bit() == throughICU.is8Bit());
    }

    struct Part {
        String type;
        String value;
    };
    using Parts = WTF::Vector<Part, 16>;

    std::optional<Parts> partsOf(const Formatter& formatter, double value)
    {
        JSValue list = callMethod(formatter.object, "formatToParts"_s, { jsNumber(value) });
        std::optional<unsigned> length = lengthOf(list);
        if (!length)
            return std::nullopt;
        Parts parts;
        for (unsigned i = 0; i < *length; ++i) {
            JSValue part = asObject(list)->get(m_globalObject, i);
            Part read { textOf(get(part, "type"_s)), textOf(get(part, "value"_s)) };
            if (read.type.isNull() || read.value.isNull())
                return std::nullopt;
            parts.append(WTF::move(read));
        }
        return parts;
    }

    static std::optional<int32_t> fixedUTCOffsetInMinutes(const String& timeZone)
    {
        if (timeZone == "UTC"_s)
            return 0;
        if (timeZone.length() != 6 || (timeZone[0] != '+' && timeZone[0] != '-') || timeZone[3] != ':')
            return std::nullopt;
        for (unsigned index : { 1, 2, 4, 5 }) {
            if (!isASCIIDigit(timeZone[index]))
                return std::nullopt;
        }
        int32_t minutes = ((timeZone[1] - '0') * 10 + (timeZone[2] - '0')) * 60 + (timeZone[4] - '0') * 10 + (timeZone[5] - '0');
        return timeZone[0] == '-' ? -minutes : minutes;
    }

    static constexpr double second = 1000;
    static constexpr double minute = 60 * second;
    static constexpr double hour = 60 * minute;
    static constexpr double day = 24 * hour;

    // Saturday, the 3rd of February 2001, 04:05:06.789 on the formatter's clock.
    static double timeValueWithOneDigitInEachField(int32_t utcOffsetInMinutes)
    {
        constexpr double startOf2001 = 978307200000.0;
        return startOf2001 + 33 * day + 4 * hour + 5 * minute + 6 * second + 789 - utcOffsetInMinutes * minute;
    }

    std::optional<PatternCode> codeFromPartsOf(const Formatter& formatter, int32_t utcOffsetInMinutes)
    {
        double base = timeValueWithOneDigitInEachField(utcOffsetInMinutes);
        std::optional<Parts> parts = partsOf(formatter, base);
        if (!parts)
            return std::nullopt;
        auto samePartAt = [&](size_t index, double timeValue) -> String {
            std::optional<Parts> other = partsOf(formatter, timeValue);
            if (!other || !std::ranges::equal(*other, *parts, { }, &Part::type, &Part::type))
                return { };
            return (*other)[index].value;
        };

        PatternCode code;
        for (size_t index = 0; index < parts->size(); ++index) {
            const String& type = (*parts)[index].type;
            const String& text = (*parts)[index].value;
            auto number = [&](ASCIILiteral oneDigit, ASCIILiteral twoDigits, DateTimePatternOp inOneDigit, DateTimePatternOp inTwoDigits) {
                if (text != oneDigit && text != twoDigits)
                    return false;
                code.append(text == oneDigit ? inOneDigit : inTwoDigits);
                code.didAppendField(2);
                return true;
            };
            auto names = [&](DateTimePatternOp op, unsigned count, const auto& timeValueOf) {
                WTF::Vector<String, 12> list;
                for (unsigned i = 0; i < count; ++i) {
                    String name = samePartAt(index, timeValueOf(i));
                    if (name.isEmpty())
                        return false;
                    list.append(WTF::move(name));
                }
                code.appendNames(op, list.span());
                return true;
            };
            auto hours = [&] {
                bool hasTwoDigits = text == "04"_s;
                if (!hasTwoDigits && text != "4"_s)
                    return false;
                auto withoutLeadingZero = [&](String digits) {
                    return hasTwoDigits && digits.length() == 2 && digits[0] == '0' ? digits.substring(1) : digits;
                };
                String atMidnight = withoutLeadingZero(samePartAt(index, base - 4 * hour));
                String afterNoon = withoutLeadingZero(samePartAt(index, base + 9 * hour));
                struct {
                    ASCIILiteral atMidnight;
                    ASCIILiteral afterNoon;
                    DateTimePatternOp inOneDigit;
                    DateTimePatternOp inTwoDigits;
                } cycles[] = {
                    { "0"_s, "13"_s, DateTimePatternOp::Hour23, DateTimePatternOp::TwoDigitHour23 },
                    { "24"_s, "13"_s, DateTimePatternOp::Hour24, DateTimePatternOp::TwoDigitHour24 },
                    { "12"_s, "1"_s, DateTimePatternOp::Hour12, DateTimePatternOp::TwoDigitHour12 },
                    { "0"_s, "1"_s, DateTimePatternOp::Hour11, DateTimePatternOp::TwoDigitHour11 },
                };
                for (auto& cycle : cycles) {
                    if (atMidnight != cycle.atMidnight || afterNoon != cycle.afterNoon)
                        continue;
                    code.append(hasTwoDigits ? cycle.inTwoDigits : cycle.inOneDigit);
                    code.didAppendField(2);
                    return true;
                }
                return false;
            };
            auto field = [&] {
                if (type == "literal"_s) {
                    code.appendLiteral(text);
                    return true;
                }
                if (type == "year"_s) {
                    bool isWhole = text == "2001"_s;
                    if (!isWhole && text != "01"_s)
                        return false;
                    code.append(isWhole ? DateTimePatternOp::Year : DateTimePatternOp::TwoDigitYear);
                    code.didAppendField(isWhole ? maxDecimalLengthOfYear : 2);
                    return true;
                }
                if (type == "month"_s) {
                    static constexpr int daysBeforeMonth[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
                    return number("2"_s, "02"_s, DateTimePatternOp::Month, DateTimePatternOp::TwoDigitMonth)
                        || names(DateTimePatternOp::MonthName, 12, [&](unsigned month) { return base + (daysBeforeMonth[month] - daysBeforeMonth[1]) * day; });
                }
                if (type == "day"_s)
                    return number("3"_s, "03"_s, DateTimePatternOp::Day, DateTimePatternOp::TwoDigitDay);
                if (type == "weekday"_s)
                    return names(DateTimePatternOp::WeekdayName, 7, [&](unsigned daysAfterSunday) { return base + (1 + daysAfterSunday) * day; });
                if (type == "dayPeriod"_s)
                    return names(DateTimePatternOp::DayPeriodName, 2, [&](unsigned half) { return base + half * 12 * hour; });
                if (type == "hour"_s)
                    return hours();
                if (type == "minute"_s)
                    return number("5"_s, "05"_s, DateTimePatternOp::Minute, DateTimePatternOp::TwoDigitMinute);
                if (type == "second"_s)
                    return number("6"_s, "06"_s, DateTimePatternOp::Second, DateTimePatternOp::TwoDigitSecond);
                if (type == "fractionalSecond"_s) {
                    if (text != "7"_s && text != "78"_s && text != "789"_s)
                        return false;
                    code.append(text.length() == 1 ? DateTimePatternOp::Tenths : text.length() == 2 ? DateTimePatternOp::Hundredths : DateTimePatternOp::Milliseconds);
                    code.didAppendField(text.length());
                    return true;
                }
                return false;
            };
            if (!field())
                return std::nullopt;
        }
        code.append(DateTimePatternOp::End);
        return code;
    }

    std::optional<WTF::Vector<uint8_t>> planDateTime(const FormatterRequest& request)
    {
        bool namesTimeZone = request.options.containsIf([](auto& option) { return option.name == "timeZone"_s; });
        if (namesTimeZone) {
            if (auto planned = planAtFixedOffset(request, { }))
                return planned->first.finish(request, FormatPatternTimeZone::FixedOffset, planned->second);
            if (hasException())
                return std::nullopt;
        }
        auto inUTC = planAtFixedOffset(request, "UTC"_s);
        if (!inUTC)
            return std::nullopt;
        if (!namesTimeZone)
            return inUTC->first.finish(request, FormatPatternTimeZone::Default, 0);
        auto pattern = inUTC->first.finish(request, FormatPatternTimeZone::Named, 0);
        if (!pattern)
            return std::nullopt;
        if (!agreesInNamedTimeZone(request, *pattern))
            return std::nullopt;
        return pattern;
    }

    bool agreesInNamedTimeZone(const FormatterRequest& request, const WTF::Vector<uint8_t>& pattern)
    {
        std::optional<Formatter> throughICU = makeFormatter(request);
        if (!throughICU)
            return false;
        UTCOffsetOfStretch stretch;
        constexpr double startOf1900 = -2208988800000.0;
        constexpr double startOf2024 = 1704067200000.0;
        constexpr double startOf2100 = 4102444800000.0;
        WTF::Vector<double> timeValues;
        for (unsigned i = 0; i < 366 * 4; ++i)
            timeValues.append(startOf2024 + i * 6 * hour + 1799999);
        PseudoRandomNumbers random;
        for (unsigned i = 0; i < 600; ++i)
            timeValues.append(std::floor(i % 3 ? startOf1900 + random.next() * (startOf2100 - startOf1900) : firstTimeValueOfYear1 + 48 * hour + random.next() * (maxTimeValue - 96 * hour - firstTimeValueOfYear1)));
        auto agreesAt = [&](double timeValue) {
            return isSameText(ofDateTimeFormat<formatInNamedTimeZoneWithPattern>(&stretch, &headerOf(pattern), timeValue), formatted(*throughICU, timeValue));
        };
        for (double timeValue : timeValues) {
            if (!agreesAt(timeValue))
                return false;
        }
        for (double transition = startOf2100; std::isfinite(transition) && transition > startOf1900; transition = stretch.start) {
            if (!agreesAt(transition) || !agreesAt(transition - 1))
                return false;
        }
        return true;
    }

    std::optional<std::pair<PatternCode, int32_t>> planAtFixedOffset(const FormatterRequest& request, ASCIILiteral timeZone)
    {
        FormatterRequest inTimeZone = request;
        if (!timeZone.isNull()) {
            inTimeZone.hasOptions = true;
            inTimeZone.options.removeAllMatching([](auto& option) { return option.name == "timeZone"_s; });
            inTimeZone.options.append(FormatterOption { "timeZone"_s, FormatterOptionKind::String, timeZone, 0 });
        }
        std::optional<Formatter> formatter = makeFormatter(inTimeZone);
        if (!formatter || !isSupportedLocale(*formatter, request.locale))
            return std::nullopt;
        JSValue resolved = callMethod(formatter->object, "resolvedOptions"_s);
        if (textOf(get(resolved, "calendar"_s)) != "gregory"_s || textOf(get(resolved, "numberingSystem"_s)) != "latn"_s)
            return std::nullopt;
        auto utcOffsetInMinutes = fixedUTCOffsetInMinutes(textOf(get(resolved, "timeZone"_s)));
        if (!utcOffsetInMinutes)
            return std::nullopt;
        auto code = codeFromPartsOf(*formatter, *utcOffsetInMinutes);
        if (!code)
            return std::nullopt;
        auto pattern = code->finish(request, FormatPatternTimeZone::FixedOffset, *utcOffsetInMinutes);
        if (!pattern)
            return std::nullopt;

        WTF::Vector<double> timeValues;
        double base = timeValueWithOneDigitInEachField(*utcOffsetInMinutes);
        for (unsigned i = 0; i < 48; ++i)
            timeValues.append(base + i * (31 * day + hour + 7 * minute + 11 * second + 101));
        for (unsigned i = 0; i < 366; i += 5)
            timeValues.append(base + i * day);
        for (unsigned i = 0; i < 31; ++i)
            timeValues.append(base - 33 * day + i * day);
        for (unsigned i = 0; i < 24; ++i)
            timeValues.append(base - 4 * hour + i * hour);
        for (unsigned i = 0; i < 60; ++i) {
            timeValues.append(base - 5 * minute + i * minute);
            timeValues.append(base - 6 * second + i * second);
        }
        for (unsigned millisecond : { 0, 1, 5, 9, 10, 50, 99, 100, 500, 999 })
            timeValues.append(base - 789 + millisecond);
        constexpr double daysIn400Years = 146097;
        for (double years : { -2000.0, -1999.0, -1992.0, -1991.0, -1990.0, -1902.0, -1901.0, -1900.0, -1002.0, -1001.0, -1000.0, -419.0, -401.0, -101.0, -32.0, -31.0, -2.0, -1.0, 23.0, 37.0, 7998.0, 7999.0, 10344.0, 97998.0, 97999.0, 273000.0 })
            timeValues.append(base + std::floor(years * daysIn400Years / 400) * day);

        PseudoRandomNumbers random;
        constexpr double startOf1900 = -2208988800000.0;
        constexpr double startOf2100 = 4102444800000.0;
        for (unsigned i = 0; i < 400; ++i) {
            if (i & 1)
                timeValues.append(std::floor(startOf1900 + random.next() * (startOf2100 - startOf1900)));
            else
                timeValues.append(std::floor(firstTimeValueOfYear1 + day + random.next() * (maxTimeValue - 2 * day - firstTimeValueOfYear1)));
        }
        for (double timeValue : timeValues) {
            if (!isSameText(ofDateTimeFormat<formatAtFixedOffsetWithPattern>(&headerOf(*pattern), timeValue), formatted(*formatter, timeValue)))
                return std::nullopt;
        }
        return std::pair { WTF::move(*code), *utcOffsetInMinutes };
    }

    struct NumberParts {
        String prefix;
        String suffix;
        String digits;
        String groupSeparator;
        String decimalSeparator;
        WTF::Vector<unsigned, 8> groupLengths;
        unsigned fractionLength { 0 };

        bool operator==(const NumberParts&) const = default;
    };

    std::optional<NumberParts> numberPartsOf(const Formatter& formatter, double number, bool hasFraction)
    {
        String text = formatted(formatter, number);
        if (text.isNull())
            return std::nullopt;
        size_t first = text.find(isASCIIDigit<char16_t>);
        if (first == notFound)
            return std::nullopt;
        size_t end = text.length();
        while (!isASCIIDigit(text[end - 1]))
            --end;
        NumberParts parts;
        parts.prefix = text.left(first);
        parts.suffix = text.substring(end);
        StringBuilder digits;
        WTF::Vector<String, 8> separators;
        for (size_t i = first; i < end;) {
            size_t start = i;
            while (i < end && isASCIIDigit(text[i]))
                ++i;
            parts.groupLengths.append(i - start);
            digits.append(StringView(text).substring(start, i - start));
            start = i;
            while (i < end && !isASCIIDigit(text[i]))
                ++i;
            if (i > start)
                separators.append(text.substring(start, i - start));
        }
        parts.digits = digits.toString();
        if (hasFraction) {
            if (separators.isEmpty())
                return std::nullopt;
            parts.decimalSeparator = separators.takeLast();
            parts.fractionLength = parts.groupLengths.takeLast();
        }
        for (auto& separator : separators) {
            if (separator != separators[0])
                return std::nullopt;
        }
        if (!separators.isEmpty())
            parts.groupSeparator = separators[0];
        if (numberPartsByType(formatter, number) != parts)
            return std::nullopt;
        return parts;
    }

    std::optional<NumberParts> numberPartsByType(const Formatter& formatter, double number)
    {
        std::optional<Parts> list = partsOf(formatter, number);
        if (!list)
            return std::nullopt;
        enum class Section : uint8_t { Prefix, Integer, Fraction, Suffix };
        Section section = Section::Prefix;
        NumberParts parts;
        StringBuilder prefix;
        StringBuilder suffix;
        StringBuilder digits;
        for (auto& [kind, text] : *list) {
            if (kind == "integer"_s) {
                if (section > Section::Integer)
                    return std::nullopt;
                section = Section::Integer;
                parts.groupLengths.append(text.length());
                digits.append(text);
            } else if (kind == "group"_s) {
                if (section != Section::Integer || (!parts.groupSeparator.isNull() && parts.groupSeparator != text))
                    return std::nullopt;
                parts.groupSeparator = text;
            } else if (kind == "decimal"_s) {
                if (section != Section::Integer)
                    return std::nullopt;
                section = Section::Fraction;
                parts.decimalSeparator = text;
            } else if (kind == "fraction"_s) {
                if (section != Section::Fraction || parts.fractionLength)
                    return std::nullopt;
                parts.fractionLength = text.length();
                digits.append(text);
            } else if (section == Section::Prefix)
                prefix.append(text);
            else {
                section = Section::Suffix;
                suffix.append(text);
            }
        }
        if (section == Section::Prefix)
            return std::nullopt;
        parts.prefix = prefix.toString();
        parts.suffix = suffix.toString();
        parts.digits = digits.toString();
        return parts;
    }

    std::optional<WTF::Vector<uint8_t>> planNumber(const FormatterRequest& request)
    {
        std::optional<Formatter> formatter = makeFormatter(request);
        if (!formatter || !isSupportedLocale(*formatter, request.locale))
            return std::nullopt;

        JSValue resolved = callMethod(formatter->object, "resolvedOptions"_s);
        if (!resolved)
            return std::nullopt;
        auto hasOption = [&](ASCIILiteral name) {
            JSValue value = get(resolved, name);
            return !value || !value.isUndefined();
        };
        auto textOfOption = [&](ASCIILiteral name) {
            return textOf(get(resolved, name));
        };
        auto numberOf = [&](ASCIILiteral name) {
            JSValue value = get(resolved, name);
            return value && value.isNumber() ? value.asNumber() : PNaN;
        };
        String style = textOfOption("style"_s);
        bool isCurrency = style == "currency"_s;
        bool isPercent = style == "percent"_s;
        if (!isCurrency && !isPercent && style != "decimal"_s)
            return std::nullopt;
        unsigned decimalShift = isPercent ? 2 : 0;
        if (isCurrency) {
            String display = textOfOption("currencyDisplay"_s);
            String sign = textOfOption("currencySign"_s);
            if ((display != "symbol"_s && display != "narrowSymbol"_s && display != "code"_s) || (sign != "standard"_s && sign != "accounting"_s))
                return std::nullopt;
        }
        for (auto [name, expected] : { std::pair { "numberingSystem"_s, "latn"_s }, std::pair { "notation"_s, "standard"_s }, std::pair { "signDisplay"_s, "auto"_s }, std::pair { "roundingMode"_s, "halfExpand"_s }, std::pair { "roundingPriority"_s, "auto"_s }, std::pair { "trailingZeroDisplay"_s, "auto"_s } }) {
            if (textOfOption(name) != expected)
                return std::nullopt;
        }
        if (numberOf("roundingIncrement"_s) != 1)
            return std::nullopt;
        double resolvedMinIntegerDigits = numberOf("minimumIntegerDigits"_s);
        if (hasOption("minimumSignificantDigits"_s) || hasOption("maximumSignificantDigits"_s) || !(resolvedMinIntegerDigits >= 1 && resolvedMinIntegerDigits <= 3))
            return std::nullopt;
        unsigned minIntegerDigits = static_cast<unsigned>(resolvedMinIntegerDigits);
        double resolvedMinFractionDigits = numberOf("minimumFractionDigits"_s);
        double resolvedMaxFractionDigits = numberOf("maximumFractionDigits"_s);
        if (!(resolvedMinFractionDigits >= 0 && resolvedMinFractionDigits <= resolvedMaxFractionDigits && resolvedMaxFractionDigits <= maxFractionDigitsInPattern))
            return std::nullopt;
        unsigned minFractionDigits = static_cast<unsigned>(resolvedMinFractionDigits);
        unsigned maxFractionDigits = static_cast<unsigned>(resolvedMaxFractionDigits);
        JSValue useGrouping = get(resolved, "useGrouping"_s);
        if (!useGrouping || !(useGrouping.isFalse() || useGrouping.isString()))
            return std::nullopt;

        double scale = std::pow(10, decimalShift);
        auto positive = numberPartsOf(*formatter, 1, minFractionDigits);
        auto negative = numberPartsOf(*formatter, -1, minFractionDigits);
        auto large = numberPartsOf(*formatter, 1234567890123.0 / scale, minFractionDigits);
        if (!positive || !negative || !large)
            return std::nullopt;
        StringBuilder one;
        for (unsigned digits = 1 + decimalShift; digits < minIntegerDigits; ++digits)
            one.append('0');
        one.append('1');
        for (unsigned i = 0; i < decimalShift + minFractionDigits; ++i)
            one.append('0');
        if (positive->digits != one.toString() || negative->digits != positive->digits || positive->fractionLength != minFractionDigits || !large->digits.startsWith("1234567890123"_s))
            return std::nullopt;
        if (large->prefix != positive->prefix || large->suffix != positive->suffix)
            return std::nullopt;

        unsigned primaryGroupSize = 0;
        unsigned secondaryGroupSize = 0;
        String separator = emptyString();
        if (auto& lengths = large->groupLengths; lengths.size() > 1) {
            primaryGroupSize = lengths.last();
            secondaryGroupSize = lengths[lengths.size() - 2];
            bool isRegular = !useGrouping.isFalse() && lengths.size() > 2 && lengths[0] <= secondaryGroupSize;
            for (size_t i = 1; i + 1 < lengths.size(); ++i)
                isRegular &= lengths[i] == secondaryGroupSize;
            if (!isRegular || primaryGroupSize > 4 || secondaryGroupSize > 4 || primaryGroupSize < 3)
                return std::nullopt;
            separator = large->groupSeparator;
        }

        String decimalSeparator = emptyString();
        if (maxFractionDigits) {
            auto half = numberPartsOf(*formatter, 0.5 / scale, true);
            if (!half || half->decimalSeparator.isNull() || half->prefix != positive->prefix || half->suffix != positive->suffix)
                return std::nullopt;
            decimalSeparator = half->decimalSeparator;
        }

        unsigned minDigitsToGroup = 0;
        if (primaryGroupSize) {
            double number = std::pow(10, primaryGroupSize) / scale;
            for (unsigned digits = primaryGroupSize + 1; digits <= primaryGroupSize + 2 && !minDigitsToGroup; ++digits, number *= 10) {
                auto parts = numberPartsOf(*formatter, number, minFractionDigits);
                if (!parts)
                    return std::nullopt;
                if (parts->groupLengths.size() > 1)
                    minDigitsToGroup = digits;
            }
            if (!minDigitsToGroup)
                return std::nullopt;
        }

        PatternCode code;
        for (unsigned parameter : { primaryGroupSize, secondaryGroupSize, minDigitsToGroup, minFractionDigits, maxFractionDigits, minIntegerDigits, decimalShift })
            code.append(parameter);
        for (auto& text : { positive->prefix, positive->suffix, negative->prefix, negative->suffix, separator, decimalSeparator })
            code.appendText(text);
        code.didAppendField(std::max(positive->prefix.length() + positive->suffix.length(), negative->prefix.length() + negative->suffix.length()) + maxDecimalLengthOfInteger * (1 + separator.length()) + decimalSeparator.length() + maxFractionDigitsInPattern);
        auto pattern = code.finish(request, FormatPatternTimeZone::FixedOffset, 0);
        if (!pattern)
            return std::nullopt;

        WTF::Vector<double> numbers { 0, 5, 9007199254740991.0, 2147483647, 2147483648.0, 4294967295.0, 4294967296.0 };
        for (double power = 1; power < 1e16; power *= 10)
            numbers.appendList({ power - 1, power, power + 1, power * 5, power * 9 });
        for (double scale = 1; scale <= 1e9; scale *= 10) {
            for (double numerator : { 1.0, 4.0, 5.0, 6.0, 9.0, 15.0, 25.0, 35.0, 45.0, 49.0, 50.0, 51.0, 95.0, 99.0, 105.0, 125.0, 375.0, 995.0, 999.0, 1005.0, 1255.0, 2675.0, 8345.0, 9995.0, 99995.0, 123456789.0 })
                numbers.appendList({ numerator / scale, 1 + numerator / scale, 1234567 + numerator / scale });
        }
        numbers.appendList({ 0.1 + 0.2, 1.0 / 3, 2.0 / 3, 1e-6, 1.5e-6, 9.999999e-7, 0.000001234, 4503599627370495.5, 1125899906842623.75, 999999.9999999999 });
        PseudoRandomNumbers random;
        for (unsigned i = 0; i < 200; ++i)
            numbers.append(std::floor(std::pow(10, random.next() * 15.9)));
        for (unsigned i = 0; i < 400; ++i) {
            double scale = std::pow(10, std::floor(random.next() * 8));
            numbers.append(i & 1 ? std::floor(random.next() * 1e9) / scale : std::pow(10, random.next() * 14 - 5));
        }
        for (size_t i = 0; i < numbers.size(); ++i) {
            for (double signedNumber : { numbers[i], -numbers[i] }) {
                String text = ofNumberFormat<formatNumberWithPattern>(&headerOf(*pattern), signedNumber);
                if (!text.isNull() && !isSameText(text, formatted(*formatter, signedNumber)))
                    return std::nullopt;
            }
        }
        return pattern;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    TopExceptionScope* m_scope { nullptr };
};

} // anonymous namespace

const WTF::Vector<uint32_t>* FormatPatternPlans::planFor(ScratchRealms& realms, const FormatterRequest& request)
{
    String key = keyOf(request);
    Locker locker { m_lock };
    if (auto found = m_plans.find(key); found != m_plans.end())
        return found->value.get();
    std::optional<WTF::Vector<uint8_t>> pattern = realms.ofCurrentThread().run([&](JSGlobalObject* realm) {
        return FormatPatternPlanner(realm).plan(request);
    });
    std::unique_ptr<WTF::Vector<uint32_t>> words;
    if (pattern) {
        words = makeUniqueWithoutFastMallocCheck<WTF::Vector<uint32_t>>(FillWith { }, (pattern->size() + sizeof(uint32_t) - 1) / sizeof(uint32_t), 0u);
        memcpySpan(asMutableByteSpan(words->mutableSpan()), pattern->span());
    }
    return m_plans.add(WTF::move(key).isolatedCopy(), WTF::move(words)).iterator->value.get();
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
