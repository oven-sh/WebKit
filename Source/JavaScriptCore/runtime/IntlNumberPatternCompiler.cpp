/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "IntlFormatPatternCompiler.h"

#include "IntlObject.h"
#include "JSCInlines.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <unicode/ucurr.h>
#include <unicode/ufieldpositer.h>
#include <unicode/unum.h>
#include <wtf/Range.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/unicode/icu/ICUHelpers.h>

#ifdef U_HIDE_DRAFT_API
#undef U_HIDE_DRAFT_API
#endif
#include <unicode/unumberformatter.h>
#define U_HIDE_DRAFT_API 1

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

enum class CurrencyText : uint8_t { Symbol, NarrowSymbol, Code };

struct IntlFormatPatternCompiler::NumberParts {
    String prefix;
    String suffix;
    String digits;
    String groupSeparator;
    String decimalSeparator;
    Vector<unsigned, 8> groupLengths;
    unsigned fractionLength { 0 };

    bool operator==(const NumberParts&) const = default;
};

struct IntlFormatPatternCompiler::NumberOptions {
    static std::optional<CurrencyText> currencyTextOf(IntlNumberFormat::CurrencyDisplay);

    String locale;
    String currency;
    bool isCurrency { false };
    bool isPercent { false };
    bool isAccounting { false };
    CurrencyText currencyText { CurrencyText::Symbol };
    bool usesGrouping { false };
    unsigned minIntegerDigits { 1 };
    unsigned minFractionDigits { 0 };
    unsigned maxFractionDigits { 0 };
    unsigned decimalShift { 0 };
};

struct IntlFormatPatternCompiler::NumberRules {
    String positivePrefix;
    String positiveSuffix;
    String negativePrefix;
    String negativeSuffix;
    String groupSeparator;
    String decimalSeparator;
    unsigned primaryGroupSize { 0 };
    unsigned secondaryGroupSize { 0 };
};

std::optional<CurrencyText> IntlFormatPatternCompiler::NumberOptions::currencyTextOf(IntlNumberFormat::CurrencyDisplay display)
{
    switch (display) {
    case IntlNumberFormat::CurrencyDisplay::Symbol:
        return CurrencyText::Symbol;
    case IntlNumberFormat::CurrencyDisplay::NarrowSymbol:
        return CurrencyText::NarrowSymbol;
    case IntlNumberFormat::CurrencyDisplay::Code:
        return CurrencyText::Code;
    case IntlNumberFormat::CurrencyDisplay::FormalSymbol:
    case IntlNumberFormat::CurrencyDisplay::Name:
    case IntlNumberFormat::CurrencyDisplay::Never:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Getter, typename Selector>
static String readICUText(const Getter& getter, const UNumberFormat* format, Selector selector, UErrorCode& status)
{
    if (U_FAILURE(status))
        return { };
    Vector<char16_t, 32> buffer;
    status = callBufferProducingFunction(getter, format, selector, buffer);
    if (U_FAILURE(status))
        return { };
    return String { buffer.span() };
}

struct Affixes {
    String prefix;
    String suffix;
};

static Affixes readAffixes(const UNumberFormat* format, double number, UErrorCode& status)
{
    if (U_FAILURE(status))
        return { };
    std::unique_ptr<UFieldPositionIterator, ICUDeleter<ufieldpositer_close>> fields(ufieldpositer_open(&status));
    if (U_FAILURE(status))
        return { };
    Vector<char16_t, 32> text;
    status = callBufferProducingFunction(unum_formatDoubleForFields, format, number, text, fields.get());
    if (U_FAILURE(status))
        return { };

    int32_t digitsStart = text.size();
    int32_t digitsEnd = 0;
    int32_t fieldStart = 0;
    int32_t fieldEnd = 0;
    for (int32_t field = ufieldpositer_next(fields.get(), &fieldStart, &fieldEnd); field >= 0; field = ufieldpositer_next(fields.get(), &fieldStart, &fieldEnd)) {
        if (field != UNUM_INTEGER_FIELD && field != UNUM_FRACTION_FIELD)
            continue;
        digitsStart = std::min(digitsStart, fieldStart);
        digitsEnd = std::max(digitsEnd, fieldEnd);
    }
    if (digitsStart >= digitsEnd) {
        status = U_INVALID_FORMAT_ERROR;
        return { };
    }
    return { String { text.span().first(digitsStart) }, String { text.span().subspan(digitsEnd) } };
}

static void setCurrency(UNumberFormat* format, const String& currency, CurrencyText text, const char* localeID, UErrorCode& status)
{
    std::array<char16_t, 4> code { currency[0], currency[1], currency[2], 0 };
    unum_setTextAttribute(format, UNUM_CURRENCY_CODE, code.data(), 3, &status);
    switch (text) {
    case CurrencyText::Symbol:
        return;
    case CurrencyText::NarrowSymbol: {
        int32_t length = 0;
        const char16_t* name = ucurr_getName(code.data(), localeID, UCURR_NARROW_SYMBOL_NAME, nullptr, &length, &status);
        unum_setSymbol(format, UNUM_CURRENCY_SYMBOL, name, length, &status);
        return;
    }
    case CurrencyText::Code:
        unum_setSymbol(format, UNUM_CURRENCY_SYMBOL, code.data(), 3, &status);
        return;
    }
}

static bool hasRegularGroups(std::span<const unsigned> lengths, unsigned primaryGroupSize, unsigned secondaryGroupSize, unsigned digitCount)
{
    if (!primaryGroupSize)
        return lengths.size() == 1 && lengths[0] == digitCount;
    if (lengths.size() < 2 || lengths.back() != primaryGroupSize)
        return false;
    unsigned total = lengths[0] + primaryGroupSize;
    for (size_t i = 1; i + 1 < lengths.size(); ++i) {
        if (lengths[i] != secondaryGroupSize)
            return false;
        total += secondaryGroupSize;
    }
    return lengths[0] >= 1 && lengths[0] <= secondaryGroupSize && total == digitCount;
}

std::optional<IntlFormatPatternCompiler::NumberParts> IntlFormatPatternCompiler::partsOf(const IntlNumberFormat& formatter, double number, bool hasFraction)
{
    String text = formatter.tryFormatWithICU(number);
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
    Vector<String, 8> separators;
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
    if (partsByFieldPositions(formatter, number) != parts)
        return std::nullopt;
    return parts;
}

std::optional<IntlFormatPatternCompiler::NumberParts> IntlFormatPatternCompiler::partsByFieldPositions(const IntlNumberFormat& formatter, double number)
{
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<UFieldPositionIterator, ICUDeleter<ufieldpositer_close>> fields(ufieldpositer_open(&status));
    std::unique_ptr<UFormattedNumber, ICUDeleter<unumf_closeResult>> formattedNumber(unumf_openResult(&status));
    if (U_FAILURE(status))
        return std::nullopt;
    unumf_formatDouble(formatter.m_numberFormatter.get(), number, formattedNumber.get(), &status);
    unumf_resultGetAllFieldPositions(formattedNumber.get(), fields.get(), &status);
    if (U_FAILURE(status))
        return std::nullopt;
    Vector<char16_t, 32> characters;
    if (U_FAILURE(callBufferProducingFunction(unumf_resultToString, formattedNumber.get(), characters)))
        return std::nullopt;
    String text { characters.span() };

    using FieldRange = WTF::Range<int32_t>;
    FieldRange integer;
    FieldRange decimalSeparator;
    FieldRange fraction;
    Vector<FieldRange, 8> groupSeparators;
    bool hasRepeatedField = false;
    int32_t fieldStart = 0;
    int32_t fieldEnd = 0;
    for (int32_t field = ufieldpositer_next(fields.get(), &fieldStart, &fieldEnd); field >= 0; field = ufieldpositer_next(fields.get(), &fieldStart, &fieldEnd)) {
        FieldRange range { fieldStart, fieldEnd };
        if (field == UNUM_GROUPING_SEPARATOR_FIELD) {
            groupSeparators.append(range);
            continue;
        }
        FieldRange* found = field == UNUM_INTEGER_FIELD ? &integer : field == UNUM_DECIMAL_SEPARATOR_FIELD ? &decimalSeparator : field == UNUM_FRACTION_FIELD ? &fraction : nullptr;
        if (!found)
            continue;
        hasRepeatedField |= !!*found;
        *found = range;
    }
    if (hasRepeatedField || !integer)
        return std::nullopt;
    std::ranges::sort(groupSeparators, { }, &FieldRange::begin);

    NumberParts parts;
    StringBuilder digits;
    int32_t groupStart = integer.begin();
    auto appendGroup = [&](int32_t groupEnd) {
        parts.groupLengths.append(groupEnd - groupStart);
        digits.append(StringView(text).substring(groupStart, groupEnd - groupStart));
    };
    for (auto& separator : groupSeparators) {
        String separatorText = text.substring(separator.begin(), separator.distance());
        if (separator.begin() <= groupStart || separator.end() >= integer.end() || (!parts.groupSeparator.isNull() && parts.groupSeparator != separatorText))
            return std::nullopt;
        parts.groupSeparator = WTF::move(separatorText);
        appendGroup(separator.begin());
        groupStart = separator.end();
    }
    appendGroup(integer.end());

    int32_t numberEnd = integer.end();
    if (decimalSeparator) {
        if (decimalSeparator.begin() != numberEnd)
            return std::nullopt;
        parts.decimalSeparator = text.substring(decimalSeparator.begin(), decimalSeparator.distance());
        numberEnd = decimalSeparator.end();
    }
    if (fraction) {
        if (!decimalSeparator || fraction.begin() != numberEnd)
            return std::nullopt;
        parts.fractionLength = fraction.distance();
        digits.append(StringView(text).substring(fraction.begin(), fraction.distance()));
        numberEnd = fraction.end();
    }
    parts.prefix = text.left(integer.begin());
    parts.suffix = text.substring(numberEnd);
    parts.digits = digits.toString();
    return parts;
}

std::optional<IntlFormatPatternCompiler::NumberOptions> IntlFormatPatternCompiler::readNumberOptions(const IntlNumberFormat& formatter)
{
    NumberOptions options;
    options.locale = formatter.m_dataLocale;
    bool isSupported = true;
    switch (formatter.m_style) {
    case IntlNumberFormat::Style::Decimal:
        break;
    case IntlNumberFormat::Style::Percent:
        options.isPercent = true;
        options.decimalShift = 2;
        break;
    case IntlNumberFormat::Style::Currency: {
        auto currencyText = NumberOptions::currencyTextOf(formatter.m_currencyDisplay);
        isSupported = currencyText && formatter.m_currency.length() == 3;
        options.isCurrency = true;
        options.currency = formatter.m_currency;
        options.currencyText = currencyText.value_or(CurrencyText::Symbol);
        options.isAccounting = formatter.m_currencySign == IntlNumberFormat::CurrencySign::Accounting;
        break;
    }
    case IntlNumberFormat::Style::Unit:
        isSupported = false;
        break;
    }
    isSupported = isSupported
        && formatter.m_notation == IntlNotation::Standard
        && formatter.m_signDisplay == IntlNumberFormat::SignDisplay::Auto
        && formatter.m_roundingMode == RoundingMode::HalfExpand
        && formatter.m_roundingType == IntlRoundingType::FractionDigits
        && formatter.m_trailingZeroDisplay == IntlTrailingZeroDisplay::Auto
        && formatter.m_roundingIncrement == 1
        && formatter.m_minimumIntegerDigits >= 1
        && formatter.m_minimumIntegerDigits <= 3
        && formatter.m_minimumFractionDigits <= formatter.m_maximumFractionDigits
        && formatter.m_maximumFractionDigits <= maxFractionDigitsInPattern
        && (formatter.m_numberingSystem.isNull() ? defaultNumberingSystemForLocale(formatter.m_dataLocale) : formatter.m_numberingSystem) == "latn"_s;
    if (!isSupported) {
        decline(PatternDecline::OptionNotSupported);
        return std::nullopt;
    }
    options.minIntegerDigits = formatter.m_minimumIntegerDigits;
    options.minFractionDigits = formatter.m_minimumFractionDigits;
    options.maxFractionDigits = formatter.m_maximumFractionDigits;
    options.usesGrouping = formatter.m_useGrouping != IntlNumberFormat::UseGrouping::False;
    return options;
}

std::optional<IntlFormatPatternCompiler::NumberRules> IntlFormatPatternCompiler::readNumberRules(const NumberOptions& options)
{
    ASCIICString localeID = makeString(options.locale, "-u-nu-latn"_s).ascii();
    UNumberFormatStyle style = options.isPercent ? UNUM_PERCENT : !options.isCurrency ? UNUM_DECIMAL : options.isAccounting ? UNUM_CURRENCY_ACCOUNTING : UNUM_CURRENCY;
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<UNumberFormat, ICUDeleter<unum_close>> format(unum_open(style, nullptr, 0, localeID.data(), nullptr, &status));
    if (U_FAILURE(status)) {
        decline(PatternDecline::SymbolsNotReadable);
        return std::nullopt;
    }
    if (options.isCurrency)
        setCurrency(format.get(), options.currency, options.currencyText, localeID.data(), status);

    NumberRules rules;
    Affixes positive = readAffixes(format.get(), 1, status);
    Affixes negative = readAffixes(format.get(), -1, status);
    rules.positivePrefix = WTF::move(positive.prefix);
    rules.positiveSuffix = WTF::move(positive.suffix);
    rules.negativePrefix = WTF::move(negative.prefix);
    rules.negativeSuffix = WTF::move(negative.suffix);
    String groupSeparator = readICUText(unum_getSymbol, format.get(), options.isCurrency ? UNUM_MONETARY_GROUPING_SEPARATOR_SYMBOL : UNUM_GROUPING_SEPARATOR_SYMBOL, status);
    String decimalSeparator = readICUText(unum_getSymbol, format.get(), options.isCurrency ? UNUM_MONETARY_SEPARATOR_SYMBOL : UNUM_DECIMAL_SEPARATOR_SYMBOL, status);
    String zeroDigit = readICUText(unum_getSymbol, format.get(), UNUM_ZERO_DIGIT_SYMBOL, status);
    if (U_FAILURE(status) || zeroDigit != "0"_s) {
        decline(PatternDecline::SymbolsNotReadable);
        return std::nullopt;
    }
    rules.decimalSeparator = options.maxFractionDigits ? decimalSeparator : emptyString();
    rules.groupSeparator = emptyString();

    if (!options.usesGrouping || !unum_getAttribute(format.get(), UNUM_GROUPING_USED))
        return rules;
    int32_t primaryGroupSize = unum_getAttribute(format.get(), UNUM_GROUPING_SIZE);
    int32_t secondaryGroupSize = unum_getAttribute(format.get(), UNUM_SECONDARY_GROUPING_SIZE);
    if (secondaryGroupSize <= 0)
        secondaryGroupSize = primaryGroupSize;
    if (primaryGroupSize <= 0)
        return rules;
    if (primaryGroupSize < 3 || primaryGroupSize > 4 || secondaryGroupSize > 4) {
        decline(PatternDecline::GroupingNotSupported);
        return std::nullopt;
    }
    rules.primaryGroupSize = primaryGroupSize;
    rules.secondaryGroupSize = secondaryGroupSize;
    rules.groupSeparator = groupSeparator;
    return rules;
}

bool IntlFormatPatternCompiler::agreesOnParts(const IntlNumberFormat& formatter, const NumberOptions& options, const NumberRules& rules)
{
    unsigned decimalShift = options.decimalShift;
    unsigned minFractionDigits = options.minFractionDigits;
    double scale = std::pow(10, decimalShift);
    auto positive = partsOf(formatter, 1, minFractionDigits);
    auto negative = partsOf(formatter, -1, minFractionDigits);
    auto large = partsOf(formatter, 1234567890123.0 / scale, minFractionDigits);
    if (!positive || !negative || !large)
        return false;
    StringBuilder one;
    for (unsigned digits = 1 + decimalShift; digits < options.minIntegerDigits; ++digits)
        one.append('0');
    one.append('1');
    for (unsigned i = 0; i < decimalShift + minFractionDigits; ++i)
        one.append('0');
    if (positive->digits != one.toString() || negative->digits != positive->digits || positive->fractionLength != minFractionDigits || !large->digits.startsWith("1234567890123"_s))
        return false;
    if (positive->prefix != rules.positivePrefix || positive->suffix != rules.positiveSuffix || large->prefix != rules.positivePrefix || large->suffix != rules.positiveSuffix)
        return false;
    if (negative->prefix != rules.negativePrefix || negative->suffix != rules.negativeSuffix)
        return false;

    if (!hasRegularGroups(large->groupLengths.span(), rules.primaryGroupSize, rules.secondaryGroupSize, 13))
        return false;
    if (rules.primaryGroupSize ? large->groupSeparator != rules.groupSeparator : !large->groupSeparator.isNull())
        return false;

    if (!options.maxFractionDigits)
        return true;
    auto half = partsOf(formatter, 0.5 / scale, true);
    return half && half->decimalSeparator == rules.decimalSeparator && half->prefix == rules.positivePrefix && half->suffix == rules.positiveSuffix;
}

std::optional<unsigned> IntlFormatPatternCompiler::findMinDigitsToGroup(const IntlNumberFormat& formatter, const NumberOptions& options, const NumberRules& rules)
{
    if (!rules.primaryGroupSize)
        return 0;
    double number = std::pow(10, rules.primaryGroupSize) / std::pow(10, options.decimalShift);
    for (unsigned digits = rules.primaryGroupSize + 1; digits <= rules.primaryGroupSize + 2; ++digits, number *= 10) {
        auto parts = partsOf(formatter, number, options.minFractionDigits);
        if (!parts) {
            decline(PatternDecline::DiffersFromICU);
            return std::nullopt;
        }
        if (parts->groupLengths.size() > 1)
            return digits;
    }
    decline(PatternDecline::GroupingNotSupported);
    return std::nullopt;
}

bool IntlFormatPatternCompiler::agreesOnSamples(const IntlNumberFormat& formatter, const IntlFormatPattern& pattern)
{
    Vector<double> numbers { 0, 5, 9007199254740991.0, 2147483647, 2147483648.0, 4294967295.0, 4294967296.0 };
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
    constexpr size_t stride = 29;
    for (size_t i = 0; i < numbers.size(); i += stride) {
        for (double signedNumber : { numbers[i], -numbers[i] }) {
            String text = pattern.tryFormatNumber(signedNumber);
            if (!text.isNull() && text != formatter.tryFormatWithICU(signedNumber))
                return false;
        }
    }
    return true;
}

std::unique_ptr<IntlFormatPattern> IntlFormatPatternCompiler::compile(const IntlNumberFormat& formatter)
{
    auto options = readNumberOptions(formatter);
    if (!options)
        return nullptr;
    auto rules = readNumberRules(*options);
    if (!rules)
        return nullptr;
    if (!agreesOnParts(formatter, *options, *rules))
        return decline(PatternDecline::DiffersFromICU);
    auto minDigitsToGroup = findMinDigitsToGroup(formatter, *options, *rules);
    if (!minDigitsToGroup)
        return nullptr;

    PatternCode code;
    for (unsigned parameter : { rules->primaryGroupSize, rules->secondaryGroupSize, *minDigitsToGroup, options->minFractionDigits, options->maxFractionDigits, options->minIntegerDigits, options->decimalShift })
        code.append(parameter);
    for (auto& text : { rules->positivePrefix, rules->positiveSuffix, rules->negativePrefix, rules->negativeSuffix, rules->groupSeparator, rules->decimalSeparator })
        code.appendText(text);
    code.didAppendField(std::max(rules->positivePrefix.length() + rules->positiveSuffix.length(), rules->negativePrefix.length() + rules->negativeSuffix.length()) + maxDecimalLengthOfInteger * (1 + rules->groupSeparator.length()) + rules->decimalSeparator.length() + maxFractionDigitsInPattern);
    auto bytes = code.finish(FormatPatternKind::Number, FormatPatternTimeZone::FixedOffset, 0);
    if (!bytes)
        return decline(PatternDecline::PatternTooLong);
    auto pattern = makeUnique<IntlFormatPattern>(WTF::move(*bytes), String());
    if (!agreesOnSamples(formatter, *pattern))
        return decline(PatternDecline::DiffersFromICU);
    return pattern;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
