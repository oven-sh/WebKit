/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTFormatPatterns.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedThreads.h"
#include "DateInstance.h"
#include "IntlDateTimeFormat.h"
#include "IntlNumberFormat.h"
#include "IntlObjectInlines.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <unicode/udat.h>
#include <unicode/ufieldpositer.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static ALWAYS_INLINE const FormatPatternHeader& formatPatternAt(Instance* instance, uint32_t index)
{
    auto* data = reinterpret_cast<const Typed::Data*>(instance->typedData);
    return *data->at<FormatPatternHeader>(data->at<uint32_t>(data->formatPatternsOffset)[index]);
}

template<typename CharacterType, typename Write>
static ALWAYS_INLINE String formatWithPattern(const FormatPatternHeader& header, const Write& write)
{
    CharacterType buffer[maxFormatPatternLength];
    CharacterType* end = write(buffer, reinterpret_cast<const CharacterType*>(&header + 1));
    return String(std::span<const CharacterType> { buffer, end });
}

static String formatDateTimeWithPattern(const FormatPatternHeader& header, double timeValue)
{
    auto fields = fieldsForDateTimePattern(header, timeValue);
    if (!fields) [[unlikely]]
        return { };
    auto write = [&](auto* cursor, auto* code) {
        return writeDateTime(cursor, code, *fields);
    };
    return header.is16Bit ? formatWithPattern<char16_t>(header, write) : formatWithPattern<Latin1Character>(header, write);
}

static String formatNumberWithPattern(const FormatPatternHeader& header, double number)
{
    auto integer = integerForPattern(number);
    if (!integer) [[unlikely]]
        return { };
    auto write = [&](auto* cursor, auto* code) {
        return writeInteger(cursor, code, *integer);
    };
    return header.is16Bit ? formatWithPattern<char16_t>(header, write) : formatWithPattern<Latin1Character>(header, write);
}

static ALWAYS_INLINE bool hasStructure(EncodedJSValue encodedValue, Structure* structure)
{
    JSValue value = JSValue::decode(encodedValue);
    return value.isCell() && value.asCell()->structureID() == structure->id();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFormatDateTime, JSCell*, (Instance* instance, EncodedJSValue formatter, uint32_t pattern, double timeValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (!hasStructure(formatter, globalObject->dateTimeFormatStructure())) [[unlikely]]
        return nullptr;
    String text = formatDateTimeWithPattern(formatPatternAt(instance, pattern), timeValue);
    return text.isNull() ? nullptr : jsString(vm, WTF::move(text));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFormatNumber, JSCell*, (Instance* instance, EncodedJSValue formatter, uint32_t pattern, double number))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (!hasStructure(formatter, globalObject->numberFormatStructure())) [[unlikely]]
        return nullptr;
    String text = formatNumberWithPattern(formatPatternAt(instance, pattern), number);
    return text.isNull() ? nullptr : jsString(vm, WTF::move(text));
}

namespace Typed {

using namespace B3;

namespace {

struct ConstantOption {
    enum class Kind : uint8_t { String, Boolean, Number };
    String name;
    Kind kind;
    String string;
    double number { 0 };
};

struct FormatterRequest {
    FormatPatternKind kind;
    String locale;
    bool hasOptions { false };
    Vector<ConstantOption, 8> options;
    bool isUsed { false };
    std::optional<uint32_t> pattern;

    String key() const
    {
        StringBuilder builder;
        builder.append(static_cast<unsigned>(kind), '\0', locale, '\0', hasOptions ? '1' : '0');
        for (auto& option : options)
            builder.append('\0', option.name, '\0', static_cast<unsigned>(option.kind), '\0', option.string, '\0', option.number);
        return builder.toString();
    }
};

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

    std::optional<Vector<uint8_t>> finish(FormatPatternKind kind, int32_t utcOffsetInMinutes) const
    {
        if (!m_isValid || m_maxLength > maxFormatPatternLength)
            return std::nullopt;
        FormatPatternHeader header { kind, m_is16Bit, static_cast<uint16_t>(m_maxLength), utcOffsetInMinutes };
        Vector<uint8_t> bytes;
        bytes.append(asByteSpan(header));
        if (m_is16Bit)
            bytes.append(asBytes(m_units.span()));
        else {
            for (char16_t unit : m_units)
                bytes.append(static_cast<uint8_t>(unit));
        }
        return bytes;
    }

private:
    Vector<char16_t, 128> m_units;
    unsigned m_maxLength { 0 };
    bool m_is16Bit { false };
    bool m_isValid { true };
};

static const FormatPatternHeader& headerOf(const Vector<uint8_t>& pattern)
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

class PatternCompiler {
public:
    explicit PatternCompiler(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
    {
    }

    std::optional<Vector<uint8_t>> compile(const FormatterRequest& request)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        auto result = request.kind == FormatPatternKind::DateTime ? compileDateTime(request) : compileInteger(request);
        if (scope.exception()) [[unlikely]] {
            scope.clearException();
            return std::nullopt;
        }
        return result;
    }

private:
    struct Part {
        bool isLiteral;
        String text;
    };
    using Parts = Vector<Part, 16>;
    struct Sample {
        UTCDateTime fields;
        Parts parts;
    };

    bool hasException()
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        return !!scope.exception();
    }

    JSValue optionsOf(const FormatterRequest& request)
    {
        if (!request.hasOptions)
            return jsUndefined();
        JSObject* options = constructEmptyObject(m_globalObject);
        for (auto& option : request.options) {
            JSValue value;
            switch (option.kind) {
            case ConstantOption::Kind::String:
                value = jsString(m_vm, option.string);
                break;
            case ConstantOption::Kind::Boolean:
                value = jsBoolean(option.number);
                break;
            case ConstantOption::Kind::Number:
                value = jsNumber(option.number);
                break;
            }
            options->putDirect(m_vm, Identifier::fromString(m_vm, option.name), value);
        }
        return options;
    }

    bool isSupportedLocale(const String& locale)
    {
        Vector<String> requested = canonicalizeLocaleList(m_globalObject, jsString(m_vm, locale));
        return !hasException() && requested.size() == 1 && !bestAvailableLocale(intlAvailableLocales(), requested[0]).isNull();
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

    template<typename Formatter>
    String formatted(const Formatter& formatter, double value)
    {
        JSValue text = formatter.format(m_globalObject, value);
        if (hasException() || !text.isString())
            return { };
        return asString(text)->value(m_globalObject);
    }

    std::optional<Parts> partsOf(const IntlDateTimeFormat& formatter, double timeValue)
    {
        String text = formatted(formatter, timeValue);
        if (text.isNull())
            return std::nullopt;
        UErrorCode status = U_ZERO_ERROR;
        auto positions = std::unique_ptr<UFieldPositionIterator, ICUDeleter<ufieldpositer_close>>(ufieldpositer_open(&status));
        if (U_FAILURE(status))
            return std::nullopt;
        Vector<char16_t, 32> unprocessed;
        status = callBufferProducingFunction(udat_formatForFields, formatter.impl().m_dateFormat.get(), timeValue, unprocessed, positions.get());
        if (U_FAILURE(status) || unprocessed.size() != text.length())
            return std::nullopt;
        Parts parts;
        int32_t length = text.length();
        int32_t previousEnd = 0;
        for (;;) {
            int32_t begin = 0;
            int32_t end = 0;
            bool isField = ufieldpositer_next(positions.get(), &begin, &end) >= 0;
            if (!isField)
                begin = end = length;
            if (begin < previousEnd || end < begin || end > length)
                return std::nullopt;
            if (previousEnd < begin)
                parts.append({ true, text.substring(previousEnd, begin - previousEnd) });
            if (!isField)
                return parts;
            parts.append({ false, text.substring(begin, end - begin) });
            previousEnd = end;
        }
    }

    static String writtenBy(DateTimePatternOp op, const UTCDateTime& fields)
    {
        char16_t code[] = { static_cast<char16_t>(op), static_cast<char16_t>(DateTimePatternOp::End) };
        char16_t buffer[maxDecimalLengthOfInteger];
        return String(std::span<const char16_t> { buffer, writeDateTime(buffer, code, fields) });
    }

    template<typename IndexOf>
    static std::optional<Vector<String>> namesByIndex(const Vector<Sample>& samples, unsigned part, unsigned count, const IndexOf& indexOf)
    {
        Vector<String> names(count);
        for (auto& sample : samples) {
            String& name = names[indexOf(sample.fields)];
            auto& text = sample.parts[part].text;
            if (name.isNull())
                name = text;
            else if (name != text)
                return std::nullopt;
        }
        if (names.containsIf([](auto& name) { return name.isNull(); }))
            return std::nullopt;
        return names;
    }

    std::optional<Vector<uint8_t>> compileDateTime(const FormatterRequest& request)
    {
        if (!isSupportedLocale(request.locale))
            return std::nullopt;
        if (!request.options.containsIf([](auto& option) { return option.name == "timeZone"_s; }))
            return std::nullopt;
        auto* formatter = IntlDateTimeFormat::create(m_vm, m_globalObject->dateTimeFormatStructure());
        formatter->initializeDateTimeFormat(m_globalObject, jsString(m_vm, request.locale), optionsOf(request), IntlDateTimeFormat::RequiredComponent::Any, IntlDateTimeFormat::Defaults::Date);
        if (hasException())
            return std::nullopt;
        if (formatter->ensureCalendar() != "gregory"_s || formatter->ensureNumberingSystem() != "latn"_s)
            return std::nullopt;
        auto utcOffsetInMinutes = fixedUTCOffsetInMinutes(formatter->impl().m_timeZoneForResolvedOptions);
        if (!utcOffsetInMinutes)
            return std::nullopt;
        FormatPatternHeader zone { FormatPatternKind::DateTime, false, 0, *utcOffsetInMinutes };

        Vector<double> timeValues;
        constexpr double second = 1000;
        constexpr double minute = 60 * second;
        constexpr double hour = 60 * minute;
        constexpr double day = 24 * hour;
        constexpr double startOf2001 = 978307200000.0;
        double base = startOf2001 + 33 * day + 4 * hour + 5 * minute + 6 * second + 789 - *utcOffsetInMinutes * minute;
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

        Vector<Sample> samples;
        for (double timeValue : timeValues) {
            auto fields = fieldsForDateTimePattern(zone, timeValue);
            auto parts = partsOf(*formatter, timeValue);
            if (!fields || !parts)
                return std::nullopt;
            if (!samples.isEmpty()) {
                auto& first = samples[0].parts;
                if (first.size() != parts->size())
                    return std::nullopt;
                for (unsigned i = 0; i < first.size(); ++i) {
                    if (first[i].isLiteral != parts->at(i).isLiteral)
                        return std::nullopt;
                }
            }
            samples.append({ *fields, WTF::move(*parts) });
        }

        static constexpr std::pair<DateTimePatternOp, unsigned> numericOps[] = {
            { DateTimePatternOp::Year, maxDecimalLengthOfYear }, { DateTimePatternOp::TwoDigitYear, 2 },
            { DateTimePatternOp::Month, 2 }, { DateTimePatternOp::TwoDigitMonth, 2 },
            { DateTimePatternOp::Day, 2 }, { DateTimePatternOp::TwoDigitDay, 2 },
            { DateTimePatternOp::Hour23, 2 }, { DateTimePatternOp::TwoDigitHour23, 2 },
            { DateTimePatternOp::Hour24, 2 }, { DateTimePatternOp::TwoDigitHour24, 2 },
            { DateTimePatternOp::Hour12, 2 }, { DateTimePatternOp::TwoDigitHour12, 2 },
            { DateTimePatternOp::Hour11, 2 }, { DateTimePatternOp::TwoDigitHour11, 2 },
            { DateTimePatternOp::Minute, 2 }, { DateTimePatternOp::TwoDigitMinute, 2 },
            { DateTimePatternOp::Second, 2 }, { DateTimePatternOp::TwoDigitSecond, 2 },
            { DateTimePatternOp::Tenths, 1 }, { DateTimePatternOp::Hundredths, 2 }, { DateTimePatternOp::Milliseconds, 3 },
        };
        PatternCode code;
        StringBuilder pendingLiteral;
        auto flushLiteral = [&] {
            code.appendLiteral(pendingLiteral.toString());
            pendingLiteral.clear();
        };
        for (unsigned part = 0; part < samples[0].parts.size(); ++part) {
            auto& text = samples[0].parts[part].text;
            bool isConstant = std::ranges::all_of(samples, [&](auto& sample) { return sample.parts[part].text == text; });
            if (isConstant) {
                pendingLiteral.append(text);
                continue;
            }
            if (samples[0].parts[part].isLiteral)
                return std::nullopt;
            flushLiteral();
            auto* numeric = std::ranges::find_if(numericOps, [&](auto& candidate) {
                return std::ranges::all_of(samples, [&](auto& sample) { return sample.parts[part].text == writtenBy(candidate.first, sample.fields); });
            });
            if (numeric != std::end(numericOps)) {
                code.append(numeric->first);
                code.didAppendField(numeric->second);
                continue;
            }
            if (auto months = namesByIndex(samples, part, 12, [](auto& fields) { return fields.month - 1; }))
                code.appendNames(DateTimePatternOp::MonthName, months->span());
            else if (auto weekdays = namesByIndex(samples, part, 7, [](auto& fields) { return fields.weekDay; }))
                code.appendNames(DateTimePatternOp::WeekdayName, weekdays->span());
            else if (auto dayPeriods = namesByIndex(samples, part, 2, [](auto& fields) { return fields.hour >= 12; }))
                code.appendNames(DateTimePatternOp::DayPeriodName, dayPeriods->span());
            else
                return std::nullopt;
        }
        flushLiteral();
        code.append(DateTimePatternOp::End);
        auto pattern = code.finish(FormatPatternKind::DateTime, *utcOffsetInMinutes);
        if (!pattern)
            return std::nullopt;

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
            if (formatDateTimeWithPattern(headerOf(*pattern), timeValue) != formatted(*formatter, timeValue))
                return std::nullopt;
        }
        return pattern;
    }

    static std::optional<std::pair<String, String>> affixesAround(const String& one, const String& two)
    {
        if (one.isNull() || one.length() != two.length())
            return std::nullopt;
        std::optional<unsigned> place;
        for (unsigned i = 0; i < one.length(); ++i) {
            if (one[i] == two[i])
                continue;
            if (place || one[i] != '1' || two[i] != '2')
                return std::nullopt;
            place = i;
        }
        if (!place)
            return std::nullopt;
        return std::pair { one.left(*place), one.substring(*place + 1) };
    }

    std::optional<Vector<uint8_t>> compileInteger(const FormatterRequest& request)
    {
        if (!isSupportedLocale(request.locale))
            return std::nullopt;
        auto* formatter = IntlNumberFormat::create(m_vm, m_globalObject->numberFormatStructure());
        formatter->initializeNumberFormat(m_globalObject, jsString(m_vm, request.locale), optionsOf(request));
        if (hasException())
            return std::nullopt;
        auto positive = affixesAround(formatted(*formatter, 1), formatted(*formatter, 2));
        auto negative = affixesAround(formatted(*formatter, -1), formatted(*formatter, -2));
        if (!positive || !negative)
            return std::nullopt;

        constexpr double longNumber = 1234567891234567.0;
        String grouped = formatted(*formatter, longNumber);
        unsigned lengthOfAffixes = positive->first.length() + positive->second.length();
        if (grouped.isNull() || !grouped.startsWith(positive->first) || !grouped.endsWith(positive->second) || grouped.length() < lengthOfAffixes)
            return std::nullopt;
        grouped = grouped.substring(positive->first.length(), grouped.length() - lengthOfAffixes);
        Vector<unsigned, 8> groupSizes;
        String separator;
        for (unsigned i = 0; i < grouped.length();) {
            unsigned start = i;
            while (i < grouped.length() && isASCIIDigit(grouped[i]))
                ++i;
            if (i == start)
                return std::nullopt;
            groupSizes.append(i - start);
            start = i;
            while (i < grouped.length() && !isASCIIDigit(grouped[i]))
                ++i;
            if (i == start)
                break;
            String found = grouped.substring(start, i - start);
            if (i == grouped.length() || (!separator.isNull() && separator != found))
                return std::nullopt;
            separator = found;
        }
        unsigned primaryGroupSize = groupSizes.size() > 1 ? groupSizes.last() : 0;
        unsigned secondaryGroupSize = groupSizes.size() > 2 ? groupSizes[groupSizes.size() - 2] : primaryGroupSize;
        unsigned minDigitsToGroup = 0;
        if (primaryGroupSize) {
            double number = std::pow(10, primaryGroupSize);
            for (unsigned digits = primaryGroupSize + 1; digits <= primaryGroupSize + 4 && !minDigitsToGroup; ++digits, number *= 10) {
                String text = formatted(*formatter, number);
                if (text.isNull())
                    return std::nullopt;
                if (text.length() > lengthOfAffixes + digits)
                    minDigitsToGroup = digits;
            }
            if (!minDigitsToGroup)
                return std::nullopt;
        } else
            separator = emptyString();

        PatternCode code;
        code.append(primaryGroupSize);
        code.append(secondaryGroupSize);
        code.append(minDigitsToGroup);
        for (auto& text : { positive->first, positive->second, negative->first, negative->second, separator })
            code.appendText(text);
        code.didAppendField(std::max(lengthOfAffixes, negative->first.length() + negative->second.length()) + maxDecimalLengthOfInteger * (1 + separator.length()));
        auto pattern = code.finish(FormatPatternKind::Integer, 0);
        if (!pattern)
            return std::nullopt;

        Vector<double> numbers { 0, 5, 9007199254740991.0, 2147483647, 2147483648.0, 4294967295.0, 4294967296.0 };
        for (double power = 1; power < 1e16; power *= 10)
            numbers.appendList({ power - 1, power, power + 1, power * 5, power * 9 });
        PseudoRandomNumbers random;
        for (unsigned i = 0; i < 200; ++i)
            numbers.append(std::floor(std::pow(10, random.next() * 15.9)));
        for (double number : numbers) {
            for (double signedNumber : { number, -number }) {
                if (!integerForPattern(signedNumber))
                    continue;
                if (formatNumberWithPattern(headerOf(*pattern), signedNumber) != formatted(*formatter, signedNumber))
                    return std::nullopt;
            }
        }
        return pattern;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
};

class ConstantFormatterSpecialization {
public:
    explicit ConstantFormatterSpecialization(Program& program)
        : m_program(program)
    {
    }

    void run()
    {
        findConstructions();
        if (m_requests.isEmpty())
            return;
        findVariables();
        findCalls();
        if (m_calls.isEmpty())
            return;
        compilePatterns();
        rewriteCalls();
    }

private:
    struct Call {
        unsigned function;
        ValueID value;
        unsigned request;
        ValueID methodGetById;
    };

    bool isNamed(StringID id, ASCIILiteral name) const { return id && equalSpans(m_program.string(id), asBytes(name.span8())); }
    String textOf(StringID id) const { return String::fromUTF8(m_program.string(id)); }
    static uint64_t keyOf(unsigned function, ValueID value) { return static_cast<uint64_t>(function) << 32 | value.index(); }

    std::optional<FormatPatternKind> kindConstructedBy(const Function& function, const Value& construction) const
    {
        auto& constructor = function.at(m_program.originOf(function, construction.child(0)));
        if (constructor.opcode != Opcode::GetById)
            return std::nullopt;
        auto& holder = function.at(m_program.originOf(function, constructor.child(0)));
        if (holder.opcode != Opcode::GetGlobal || !isNamed(holder.string(), "Intl"_s))
            return std::nullopt;
        if (isNamed(constructor.string(), "DateTimeFormat"_s))
            return FormatPatternKind::DateTime;
        if (isNamed(constructor.string(), "NumberFormat"_s))
            return FormatPatternKind::Integer;
        return std::nullopt;
    }

    std::optional<ConstantOption> constantOption(const Function& function, StringID name, ValueID id) const
    {
        auto& value = function.at(m_program.originOf(function, id));
        ConstantOption option { textOf(name), ConstantOption::Kind::Number, { }, 0 };
        switch (value.opcode) {
        case Opcode::String:
            option.kind = ConstantOption::Kind::String;
            option.string = textOf(value.string());
            return option;
        case Opcode::Boolean:
            option.kind = ConstantOption::Kind::Boolean;
            option.number = value.boolean();
            return option;
        case Opcode::Integer:
            option.number = static_cast<double>(value.integer());
            return option;
        case Opcode::Double:
            option.number = std::bit_cast<double>(value.bits());
            return option;
        default:
            return std::nullopt;
        }
    }

    bool collectOptions(const Function& function, ValueID construction, ValueID argument, FormatterRequest& request) const
    {
        ValueID object = m_program.originOf(function, argument);
        auto& allocation = function.at(object);
        if (allocation.opcode == Opcode::Undefined)
            return true;
        if (allocation.opcode != Opcode::NewObject && allocation.opcode != Opcode::NewObjectWithShape)
            return false;
        request.hasOptions = true;
        auto& builtins = Builtins::singleton();
        auto isObject = [&](ValueID id) {
            return id && m_program.originOf(function, id) == object;
        };
        auto uses = [&](const Value& value) {
            bool result = false;
            value.forEachChild([&](ValueID child) {
                result |= isObject(child);
            });
            return result;
        };
        bool isConstructed = false;
        for (auto& block : function.blocks) {
            bool isHome = block.values.contains(object);
            if (isHome && !block.values.contains(construction))
                return false;
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (!uses(value))
                    continue;
                if (!isHome || isConstructed)
                    return false;
                if (id == construction) {
                    isConstructed = true;
                    continue;
                }
                if (value.opcode == Opcode::Cast && isObject(id))
                    continue;
                std::optional<ConstantOption> option;
                if (value.opcode == Opcode::PutField && isObject(value.child(0)))
                    option = constantOption(function, m_program.at(value.layout()).fields[value.field()].name, value.child(1));
                else if (value.opcode == Opcode::PutById && isObject(value.child(0)))
                    option = constantOption(function, value.string(), value.child(1));
                else if (value.opcode == Opcode::CallBuiltin && value.arguments.size() == 3 && isObject(value.arguments[0])) {
                    auto& implementation = builtins.at(value.builtin()).implementation;
                    auto& name = function.at(value.arguments[1]);
                    if (implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::DefineField && name.opcode == Opcode::String)
                        option = constantOption(function, name.string(), value.arguments[2]);
                }
                if (!option || request.options.containsIf([&](auto& earlier) { return earlier.name == option->name; }))
                    return false;
                request.options.append(WTF::move(*option));
            }
            bool isUsedByTerminator = false;
            const_cast<Terminator&>(block.terminator).forEachChild([&](ValueID& child) {
                isUsedByTerminator |= isObject(child);
            });
            if (isUsedByTerminator)
                return false;
        }
        return isConstructed;
    }

    std::optional<FormatterRequest> requestOf(const Function& function, ValueID id) const
    {
        auto& construction = function.at(id);
        auto kind = kindConstructedBy(function, construction);
        if (!kind || construction.arguments.isEmpty() || construction.arguments.size() > 2)
            return std::nullopt;
        auto& locale = function.at(m_program.originOf(function, construction.arguments[0]));
        if (locale.opcode != Opcode::String)
            return std::nullopt;
        FormatterRequest request { *kind, textOf(locale.string()) };
        if (construction.arguments.size() == 2 && !collectOptions(function, id, construction.arguments[1], request))
            return std::nullopt;
        return request;
    }

    template<typename Functor>
    void forEachValue(const OpcodeSet& opcodes, const Functor& functor) const
    {
        auto& functions = std::as_const(m_program).functions;
        for (unsigned i = 0; i < functions.size(); ++i) {
            if (functions[i].blocks.isEmpty() || !functions.mayContain(i, opcodes))
                continue;
            functions.forEachIndexedValue(i, opcodes, [&](ValueID id) {
                functor(i, functions[i], id);
            });
        }
    }

    void findConstructions()
    {
        static constexpr OpcodeSet constructions { Opcode::Construct };
        UncheckedKeyHashMap<String, unsigned> requestsByKey;
        forEachValue(constructions, [&](unsigned index, const Function& function, ValueID id) {
            auto request = requestOf(function, id);
            if (!request)
                return;
            unsigned number = requestsByKey.ensure(request->key(), [&] {
                m_requests.append(WTF::move(*request));
                return m_requests.size() - 1;
            }).iterator->value;
            m_constructions.add(keyOf(index, id), number);
        });
    }

    void findVariables()
    {
        static constexpr OpcodeSet stores { Opcode::PutModuleVariable };
        static constexpr unsigned hasOtherStores = UINT_MAX;
        forEachValue(stores, [&](unsigned index, const Function& function, ValueID id) {
            auto& store = function.at(id);
            auto construction = m_constructions.find(keyOf(index, m_program.originOf(function, store.child(0))));
            auto result = m_variables.add(store.variable().index(), construction == m_constructions.end() ? hasOtherStores : construction->value);
            if (!result.isNewEntry)
                result.iterator->value = hasOtherStores;
        });
        m_variables.removeIf([](auto& entry) { return entry.value == hasOtherStores; });
    }

    std::optional<unsigned> requestOfFormatter(unsigned index, const Function& function, ValueID id) const
    {
        ValueID origin = m_program.originOf(function, id);
        auto& value = function.at(origin);
        if (value.opcode == Opcode::GetModuleVariable) {
            if (auto found = m_variables.find(value.variable().index()); found != m_variables.end())
                return found->value;
            return std::nullopt;
        }
        if (auto found = m_constructions.find(keyOf(index, origin)); found != m_constructions.end())
            return found->value;
        return std::nullopt;
    }

    void findCalls()
    {
        static constexpr OpcodeSet calls { Opcode::CallMethod };
        forEachValue(calls, [&](unsigned index, const Function& function, ValueID id) {
            auto& call = function.at(id);
            if (call.arguments.size() != 1 || call.child(1) || !isNamed(call.string(), "format"_s))
                return;
            auto request = requestOfFormatter(index, function, call.child(0));
            if (!request)
                return;
            m_requests[*request].isUsed = true;
            m_nameOfFormatMethod = call.string();
            m_calls.append({ index, id, *request, { } });
        });
        static constexpr OpcodeSet callsOfValues { Opcode::Call };
        forEachValue(callsOfValues, [&](unsigned index, const Function& function, ValueID id) {
            auto& call = function.at(id);
            ValueID readID = call.child(0);
            auto& read = function.at(readID);
            if (call.arguments.size() != 1 || read.opcode != Opcode::GetById || !isNamed(read.string(), "format"_s))
                return;
            if (m_program.originOf(function, read.child(0)) != m_program.originOf(function, call.child(1)))
                return;
            auto request = requestOfFormatter(index, function, read.child(0));
            if (!request || !canReadMethodAtCall(function, readID, id))
                return;
            m_requests[*request].isUsed = true;
            m_nameOfFormatMethod = read.string();
            m_calls.append({ index, id, *request, readID });
        });
    }

    bool canReadMethodAtCall(const Function& function, ValueID read, ValueID call) const
    {
        unsigned numberOfUses = 0;
        bool isInOneBlock = false;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                function.at(id).forEachChild([&](ValueID child) {
                    numberOfUses += child == read;
                });
            }
            const_cast<Terminator&>(block.terminator).forEachChild([&](ValueID& child) {
                numberOfUses += child == read;
            });
            size_t first = block.values.find(read);
            size_t last = block.values.find(call);
            if (first == notFound || last == notFound || last < first)
                continue;
            isInOneBlock = true;
            for (size_t index = first + 1; index < last; ++index) {
                switch (function.at(block.values[index]).opcode) {
                case Opcode::Call:
                case Opcode::CallDirect:
                case Opcode::CallMethod:
                case Opcode::Construct:
                    return false;
                default:
                    break;
                }
                if (writesOf(m_program, function, block, index) >= Writes::Heap)
                    return false;
            }
        }
        return isInOneBlock && numberOfUses == 1;
    }

    void compilePatterns()
    {
        constexpr size_t stackSize = 4 * MB;
        Vector<std::optional<Vector<uint8_t>>> patterns(m_requests.size());
        Thread::create("AOT format patterns"_s, [&] {
            RefPtr<VM> vm = VM::create(HeapType::Small);
            JSLockHolder locker(*vm);
            {
                Strong<JSGlobalObject> globalObject(*vm, JSGlobalObject::create(*vm, JSGlobalObject::createStructure(*vm, jsNull())));
                PatternCompiler compiler(globalObject.get());
                for (unsigned i = 0; i < m_requests.size(); ++i) {
                    if (m_requests[i].isUsed)
                        patterns[i] = compiler.compile(m_requests[i]);
                }
            }
            vm = nullptr;
        }, ThreadType::Unknown, Thread::defaultQOS, Thread::defaultSchedulingPolicy, StackAllocationSpecification::RequestSize(stackSize))->waitForCompletion();
        unsigned numberCompiled = 0;
        for (unsigned i = 0; i < m_requests.size(); ++i) {
            if (!patterns[i])
                continue;
            m_requests[i].pattern = m_program.formatPatterns.size();
            m_program.formatPatterns.append(WTF::move(*patterns[i]));
            ++numberCompiled;
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", m_requests.size(), " Intl formatters have a constant locale and constant options; ", numberCompiled, " have a pattern in the image");
    }

    void rewriteCalls()
    {
        m_program.nameOfFormatMethod = m_nameOfFormatMethod;
        for (auto& call : m_calls) {
            auto& request = m_requests[call.request];
            if (!request.pattern)
                continue;
            auto& function = m_program.functions[call.function];
            Value constant;
            constant.opcode = Opcode::Integer;
            constant.position = function.at(call.value).position;
            constant.inlineFrame = function.at(call.value).inlineFrame;
            constant.type = m_program.types.integer;
            constant.rep = repOf(m_program.at(constant.type));
            constant.setBits(*request.pattern);
            ValueID constantID(function.values.size());
            function.values.append(WTF::move(constant));
            for (auto& block : function.blocks) {
                if (size_t index = block.values.find(call.value); index != notFound)
                    block.values.insert(index, constantID);
                if (call.methodGetById)
                    block.values.removeFirst(call.methodGetById);
            }
            auto& value = function.at(call.value);
            ValueID formatter = value.child(call.methodGetById ? 1 : 0);
            ValueID argument = value.arguments[0];
            value.opcode = Opcode::CallBuiltin;
            value.children = { };
            value.immediates = { (request.kind == FormatPatternKind::DateTime ? Operations::FormatDateTime : Operations::FormatNumber).index(), Format::none };
            value.setCallSignature(Format::none);
            value.isInTailPosition = false;
            value.arguments = { formatter, argument, constantID };
        }
    }

    Program& m_program;
    Vector<FormatterRequest> m_requests;
    UncheckedKeyHashMap<uint64_t, unsigned, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_constructions;
    UncheckedKeyHashMap<uint32_t, unsigned, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_variables;
    Vector<Call> m_calls;
    StringID m_nameOfFormatMethod;
};

} // namespace

void specializeConstantFormatters(Program& program)
{
    ConstantFormatterSpecialization(program).run();
}

std::optional<Writes> writesOfInlineIntlTemporal(InlineBuiltin, std::span<const Type>)
{
    return Writes::Heap;
}

std::optional<Operand> Lowering::lowerInlineIntlTemporal(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    ValueID formatter = value.arguments[0];
    ValueID argument = value.arguments[1];
    Word32 pattern = word32(static_cast<int32_t>(m_function.at(value.arguments[2]).integer()));
    auto genericPath = [&] {
        return ensure(callMethodGeneric(in<Boxed>(formatter), m_program.nameOfFormatMethod, Arguments { argument }), type);
    };
    auto stringResult = [&](CellPointer text, LBasicBlock slowPath) {
        orElse(Word32(m_out.notNull(text.value())), slowPath);
        return ensure(text, m_program.at(m_program.types.string), type);
    };
    if (which == InlineBuiltin::FormatNumber || typeOf(argument).isNumber()) {
        auto& number = m_program.at(m_program.types.number);
        return ifPredictionHolds({ Expectation { argument, &number } }, rep, [&](LBasicBlock slowPath) {
            if (which == InlineBuiltin::FormatNumber)
                return stringResult(AOT_TYPED_OPERATION(FormatNumber, in<Boxed>(formatter), pattern, in<Number>(argument)), slowPath);
            return stringResult(AOT_TYPED_OPERATION(FormatDateTime, in<Boxed>(formatter), pattern, in<Number>(argument)), slowPath);
        }, genericPath);
    }
    RELEASE_ASSERT(which == InlineBuiltin::FormatDateTime);
    auto classes = m_builtins.classes();
    Type date = Type::engineClass(*std::ranges::find_if(classes, [](auto& engineClass) { return engineClass.type == JSDateType; }));
    return ifPredictionHolds({ Expectation { argument, &date, true } }, rep, [&](LBasicBlock slowPath) {
        Number timeValue(m_out.loadDouble(m_out.address(m_heaps.AOTTyped_runtimeState, in<CellPointer>(argument).value(), DateInstance::offsetOfInternalNumber())));
        return stringResult(AOT_TYPED_OPERATION(FormatDateTime, in<Boxed>(formatter), pattern, timeValue), slowPath);
    }, genericPath);
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
