/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include <wtf/HexNumber.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// format(value, spec) for the built-in types, str.format(), and the % operator of str.
// (Objects/unicode_formatter.c, Objects/stringlib/unicode_format.h and Objects/unicode_format.c in CPython are the reference.)

namespace {

// A string indexed by code point in constant time.
class CodePoints {
public:
    explicit CodePoints(const String& string)
        : m_string(string)
        , m_length(string.length())
    {
        if (string.is8Bit())
            return;
        auto result = m_points.set(string.span16());
        if (result == WideText::Result::NoPairs)
            return;
        m_hasRoom = result == WideText::Result::Done;
        m_length = m_points.size();
    }

    // If not, it is empty.
    bool hasRoom() const { return m_hasRoom; }
    unsigned length() const { return m_length; }
    char32_t operator[](unsigned index) const { return m_points.size() ? m_points[index] : m_string[index]; }

    void appendTo(StringBuilder& builder, unsigned start, unsigned end) const
    {
        if (!m_points.size()) {
            builder.append(StringView(m_string).substring(start, end - start));
            return;
        }
        for (unsigned i = start; i < end; ++i)
            appendCodePoint(builder, m_points[i]);
    }

    String substring(unsigned start, unsigned end) const
    {
        if (!m_points.size())
            return m_string.substring(start, end - start);
        StrBuilder builder;
        appendTo(builder, start, end);
        return builder.toString();
    }

private:
    String m_string;
    WideText m_points;
    unsigned m_length;
    bool m_hasRoom { true };
};

struct FormatSpec {
    char32_t fill { ' ' };
    char align { '<' };
    bool alternate { false };
    bool noNegativeZero { false };
    char sign { 0 };
    int64_t width { -1 };
    char thousands { 0 }; // ',' or '_', or '4' for an underscore every four digits.
    int64_t precision { -1 };
    char fractionThousands { 0 };
    char32_t type { 0 };
};

}

static constexpr int64_t maxSsize = std::numeric_limits<int64_t>::max();

// How many digits were read, or -1 with an exception.
static int readInteger(JSGlobalObject* globalObject, const CodePoints& text, unsigned& position, unsigned end, int64_t& result)
{
    int64_t accumulator = 0;
    int count = 0;
    for (; position < end; ++position, ++count) {
        int digit = decimalDigitValue(text[position]);
        if (digit < 0)
            break;
        if (accumulator > (maxSsize - digit) / 10) {
            VM& vm = getVM(globalObject);
            auto scope = DECLARE_THROW_SCOPE(vm);
            raiseValueError(globalObject, scope, "Too many decimal digits in format string"_s);
            return -1;
        }
        accumulator = accumulator * 10 + digit;
    }
    result = accumulator;
    return count;
}

static String describeCharacter(char32_t character)
{
    if (character > 32 && character < 128)
        return makeString(static_cast<char>(character));
    return makeString("\\x"_s, hex(static_cast<unsigned>(character), Lowercase));
}

static bool parseFormatSpec(JSGlobalObject* globalObject, const CodePoints& text, JSValue object, char defaultType, char defaultAlign, FormatSpec& spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!text.hasRoom()) [[unlikely]] {
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    unsigned position = 0;
    unsigned end = text.length();
    auto isAlignment = [](char32_t c) { return c == '<' || c == '>' || c == '=' || c == '^'; };
    auto at = [&](char c) { return position < end && text[position] == static_cast<char32_t>(c); };
    auto bothSeparators = [&] {
        raiseValueError(globalObject, scope, "Cannot specify both ',' and '_'."_s);
        return false;
    };

    spec.align = defaultAlign;
    spec.type = defaultType;
    bool alignSpecified = false;
    bool fillSpecified = false;
    if (end - position >= 2 && isAlignment(text[position + 1])) {
        spec.align = static_cast<char>(text[position + 1]);
        spec.fill = text[position];
        fillSpecified = true;
        alignSpecified = true;
        position += 2;
    } else if (end - position >= 1 && isAlignment(text[position])) {
        spec.align = static_cast<char>(text[position]);
        alignSpecified = true;
        ++position;
    }
    if (at(' ') || at('+') || at('-'))
        spec.sign = static_cast<char>(text[position++]);
    if (at('z')) {
        spec.noNegativeZero = true;
        ++position;
    }
    if (at('#')) {
        spec.alternate = true;
        ++position;
    }
    if (!fillSpecified && at('0')) {
        spec.fill = '0';
        if (!alignSpecified && defaultAlign == '>')
            spec.align = '=';
        ++position;
    }
    int consumed = readInteger(globalObject, text, position, end, spec.width);
    EXCEPTION_ASSERT(!!scope.exception() == (consumed < 0));
    if (consumed < 0)
        return false;
    if (!consumed)
        spec.width = -1;
    if (at(',')) {
        spec.thousands = ',';
        ++position;
    }
    if (at('_')) {
        if (spec.thousands)
            return bothSeparators();
        spec.thousands = '_';
        ++position;
    }
    if (at(',') && spec.thousands == '_')
        return bothSeparators();

    if (at('.')) {
        ++position;
        consumed = readInteger(globalObject, text, position, end, spec.precision);
        EXCEPTION_ASSERT(!!scope.exception() == (consumed < 0));
        if (consumed < 0)
            return false;
        if (at(',')) {
            if (!consumed)
                spec.precision = -1;
            spec.fractionThousands = ',';
            ++position;
            ++consumed;
        }
        if (at('_')) {
            if (spec.fractionThousands)
                return bothSeparators();
            if (!consumed)
                spec.precision = -1;
            spec.fractionThousands = '_';
            ++position;
            ++consumed;
        }
        if (at(',') && spec.fractionThousands == '_')
            return bothSeparators();
        if (!consumed) {
            raiseValueError(globalObject, scope, "Format specifier missing precision"_s);
            return false;
        }
    }

    if (end - position > 1) {
        raiseValueError(globalObject, scope, "Invalid format specifier '"_s, text.substring(0, end), "' for object of type '"_s, typeName(globalObject, object), '\'');
        return false;
    }
    if (end - position == 1)
        spec.type = text[position++];

    if (spec.thousands) {
        switch (spec.type) {
        case 'd':
        case 'e':
        case 'f':
        case 'g':
        case 'E':
        case 'G':
        case '%':
        case 'F':
        case 0:
            break;
        case 'b':
        case 'o':
        case 'x':
        case 'X':
            if (spec.thousands == '_') {
                spec.thousands = '4';
                break;
            }
            [[fallthrough]];
        default:
            raiseValueError(globalObject, scope, "Cannot specify '"_s, spec.thousands, "' with '"_s, describeCharacter(spec.type), "'."_s);
            return false;
        }
    }
    if (spec.type == 'n' && spec.fractionThousands) {
        raiseValueError(globalObject, scope, "Cannot specify '"_s, spec.fractionThousands, "' with 'n'."_s);
        return false;
    }
    // The result would be as long as that at least, and no str is.
    if (spec.width > String::MaxLength) {
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    return true;
}

static JSString* raiseUnknownFormatCode(JSGlobalObject* globalObject, char32_t type, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "Unknown format code '"_s, describeCharacter(type), "' for object of type '"_s, typeName(globalObject, object), '\'');
    return nullptr;
}

static void appendFill(StringBuilder& builder, char32_t fill, int64_t count) { appendRepeated(builder, fill, count); }

// The groups of so many digits, with zeros on the left up to the width, from the right: function(whether a separator comes
// first, how many digits, how many zeros).
template<typename Function>
static void forEachGroupOfDigits(int64_t remaining, int64_t minWidth, unsigned groupSize, bool hasSeparator, const Function& function)
{
    minWidth = std::max<int64_t>(minWidth, 0);
    bool useSeparator = false;
    auto emit = [&](int64_t length) {
        int64_t zeros = std::max<int64_t>(0, length - remaining);
        int64_t count = std::max<int64_t>(0, std::min(remaining, length));
        function(useSeparator && hasSeparator, count, zeros);
        remaining -= count;
        useSeparator = true;
    };
    if (!groupSize) {
        emit(std::max<int64_t>(std::max(remaining, minWidth), 1));
        return;
    }
    while (true) {
        int64_t length = std::min<int64_t>(groupSize, std::max<int64_t>(std::max(remaining, minWidth), 1));
        emit(length);
        minWidth -= length;
        if (remaining <= 0 && minWidth <= 0)
            break;
        minWidth -= hasSeparator;
    }
}

// False: there is no room for so many.
static bool groupDigits(Vector<char, 64>& result, std::span<const Latin1Character> digits, int64_t minWidth, unsigned groupSize, char separator)
{
    size_t length = 0;
    forEachGroupOfDigits(digits.size(), minWidth, groupSize, separator, [&](bool hasSeparator, int64_t count, int64_t zeros) {
        length += hasSeparator + count + zeros;
    });
    if (!result.tryReserveInitialCapacity(length))
        return false;
    size_t remaining = digits.size();
    forEachGroupOfDigits(digits.size(), minWidth, groupSize, separator, [&](bool hasSeparator, int64_t count, int64_t zeros) {
        if (hasSeparator)
            result.append(separator);
        for (int64_t i = 0; i < count; ++i)
            result.append(static_cast<char>(digits[--remaining]));
        for (int64_t i = 0; i < zeros; ++i)
            result.append('0');
    });
    result.reverse();
    return true;
}

namespace {

// The text of a number, taken apart.
struct NumberParts {
    char sign { 0 }; // '-' or none.
    std::span<const Latin1Character> prefix; // "0x"
    std::span<const Latin1Character> digits; // Before the decimal point.
    bool hasDecimalPoint { false };
    std::span<const Latin1Character> fraction;
    String remainder; // The exponent, a percent sign, or for 'c' the character.
};

}

// <left padding> <sign> <prefix> <padding after the sign> <grouped digits> <point> <fraction> <remainder> <right padding>
static void appendNumber(StringBuilder& builder, const FormatSpec& spec, const NumberParts& parts, bool upperCase)
{
    char sign = 0;
    switch (spec.sign) {
    case '+':
        sign = parts.sign == '-' ? '-' : '+';
        break;
    case ' ':
        sign = parts.sign == '-' ? '-' : ' ';
        break;
    default:
        sign = parts.sign == '-' ? '-' : 0;
        break;
    }

    Vector<char, 64> fraction;
    for (size_t i = 0; i < parts.fraction.size(); ++i) {
        if (i && !(i % 3) && spec.fractionThousands)
            fraction.append(spec.fractionThousands);
        fraction.append(static_cast<char>(parts.fraction[i]));
    }

    int64_t remainderLength = StrView(parts.remainder).length();
    int64_t other = (sign ? 1 : 0) + parts.prefix.size() + parts.hasDecimalPoint + fraction.size() + remainderLength;
    int64_t minWidth = spec.fill == '0' && spec.align == '=' ? spec.width - other : 0;
    Vector<char, 64> digits;
    if (!parts.digits.empty()) {
        unsigned groupSize = spec.thousands == '4' ? 4 : spec.thousands || spec.fractionThousands ? 3 : 0;
        if (!groupDigits(digits, parts.digits, minWidth, groupSize, spec.thousands == '4' ? '_' : spec.thousands)) {
            builder.didOverflow();
            return;
        }
    }

    int64_t padding = std::max<int64_t>(spec.width - (other + static_cast<int64_t>(digits.size())), 0);
    int64_t left = 0, afterSign = 0, right = 0;
    switch (spec.align) {
    case '<':
        right = padding;
        break;
    case '^':
        left = padding / 2;
        right = padding - left;
        break;
    case '=':
        afterSign = padding;
        break;
    default:
        left = padding;
        break;
    }

    appendFill(builder, spec.fill, left);
    if (sign)
        builder.append(sign);
    for (auto character : parts.prefix)
        builder.append(static_cast<char>(upperCase ? toASCIIUpper(character) : character));
    appendFill(builder, spec.fill, afterSign);
    for (char character : digits)
        builder.append(upperCase ? toASCIIUpper(character) : character);
    if (parts.hasDecimalPoint)
        builder.append('.');
    builder.append(fraction.span());
    builder.append(parts.remainder);
    appendFill(builder, spec.fill, right);
}

static void paddingFor(int64_t length, int64_t width, char align, int64_t& left, int64_t& right)
{
    int64_t total = std::max(length, width);
    if (align == '>')
        left = total - length;
    else if (align == '^')
        left = (total - length) / 2;
    else
        left = 0;
    right = total - length - left;
}

// ---- str ----

JSString* formatStr(JSGlobalObject* globalObject, JSValue self, JSString* specString)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!specString->length())
        RELEASE_AND_RETURN(scope, str(globalObject, self));
    String specText = specString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    FormatSpec spec;
    if (failed(scope, parseFormatSpec(globalObject, CodePoints(specText), self, 's', '<', spec)))
        return nullptr;
    if (spec.type != 's')
        RELEASE_AND_RETURN(scope, raiseUnknownFormatCode(globalObject, spec.type, self));

    auto fail = [&](ASCIILiteral message) -> JSString* {
        raiseValueError(globalObject, scope, message);
        return nullptr;
    };
    if (spec.sign)
        return fail(spec.sign == ' ' ? "Space not allowed in string format specifier"_s : "Sign not allowed in string format specifier"_s);
    if (spec.noNegativeZero)
        return fail("Negative zero coercion (z) not allowed in string format specifier"_s);
    if (spec.alternate)
        return fail("Alternate form (#) not allowed in string format specifier"_s);
    if (spec.align == '=')
        return fail("'=' alignment not allowed in string format specifier"_s);

    JSString* string = strValue(self);
    StrView view(globalObject, string);
    RETURN_IF_EXCEPTION(scope, nullptr);
    int64_t length = view.length();
    if ((spec.width == -1 || spec.width <= length) && (spec.precision == -1 || spec.precision >= length))
        return string;
    if (spec.precision >= 0 && length >= spec.precision)
        length = spec.precision;
    int64_t left, right;
    paddingFor(length, spec.width, spec.align, left, right);
    StrBuilder builder;
    appendFill(builder, spec.fill, left);
    builder.append(view.substring(0, length));
    appendFill(builder, spec.fill, right);
    if (builder.hasOverflowed()) {
        raiseMemoryError(globalObject, scope, String());
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

// ---- float ----

// The sign, the digits, the fraction and what follows, of the text formatDouble() makes.
static NumberParts splitNumber(const String& text)
{
    NumberParts parts;
    auto characters = text.span8();
    size_t i = 0;
    if (!characters.empty() && characters[0] == '-') {
        parts.sign = '-';
        ++i;
    }
    size_t start = i;
    while (i < characters.size() && isASCIIDigit(characters[i]))
        ++i;
    parts.digits = characters.subspan(start, i - start);
    if (i < characters.size() && characters[i] == '.') {
        parts.hasDecimalPoint = true;
        ++i;
    }
    start = i;
    while (i < characters.size() && isASCIIDigit(characters[i]))
        ++i;
    parts.fraction = characters.subspan(start, i - start);
    parts.remainder = text.substring(i);
    return parts;
}

static bool checkPrecision(JSGlobalObject* globalObject, int64_t precision)
{
    if (precision <= std::numeric_limits<int>::max() - 1024)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "precision too big"_s);
    return false;
}

static JSString* formatFloatWithSpec(JSGlobalObject* globalObject, double value, const FormatSpec& spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPrecision(globalObject, spec.precision)))
        return nullptr;
    int precision = static_cast<int>(spec.precision);
    int defaultPrecision = 6;
    char32_t type = spec.type;
    unsigned flags = 0;
    if (spec.alternate)
        flags |= FloatAlternate;
    if (spec.noNegativeZero)
        flags |= FloatNoNegativeZero;
    if (!type) {
        // As repr(), or with a precision as 'g', but always with a digit after the point.
        flags |= FloatAddDotZero;
        type = 'r';
        defaultPrecision = 0;
    }
    if (type == 'n')
        type = 'g';
    bool addPercent = false;
    if (type == '%') {
        type = 'f';
        value *= 100;
        addPercent = true;
    }
    if (precision < 0)
        precision = defaultPrecision;
    else if (type == 'r')
        type = 'g';

    String text = formatDouble(value, static_cast<char>(type), precision, flags);
    if (!text) [[unlikely]] {
        raiseMemoryError(globalObject, scope, String());
        return nullptr;
    }
    if (addPercent)
        text = makeString(text, '%');
    if (spec.sign != '+' && spec.sign != ' ' && spec.width == -1 && !spec.thousands && !spec.fractionThousands)
        return makeStr(vm, text);
    StrBuilder builder;
    appendNumber(builder, spec, splitNumber(text), false);
    if (builder.hasOverflowed()) {
        raiseMemoryError(globalObject, scope, String());
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

JSString* formatFloat(JSGlobalObject* globalObject, JSValue self, JSString* specString)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!specString->length())
        RELEASE_AND_RETURN(scope, str(globalObject, self));
    String specText = specString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    FormatSpec spec;
    if (failed(scope, parseFormatSpec(globalObject, CodePoints(specText), self, 0, '>', spec)))
        return nullptr;
    switch (spec.type) {
    case 0:
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'n':
    case '%':
        RELEASE_AND_RETURN(scope, formatFloatWithSpec(globalObject, floatValue(self), spec));
    default:
        RELEASE_AND_RETURN(scope, raiseUnknownFormatCode(globalObject, spec.type, self));
    }
}

// ---- int ----

JSString* formatInt(JSGlobalObject* globalObject, JSValue self, JSString* specString)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!specString->length())
        RELEASE_AND_RETURN(scope, str(globalObject, self));
    String specText = specString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    FormatSpec spec;
    if (failed(scope, parseFormatSpec(globalObject, CodePoints(specText), self, 'd', '>', spec)))
        return nullptr;
    JSValue value = intValue(self);

    auto fail = [&](ASCIILiteral message) -> JSString* {
        raiseValueError(globalObject, scope, message);
        return nullptr;
    };
    unsigned base = 10;
    switch (spec.type) {
    case 'b':
        base = 2;
        break;
    case 'o':
        base = 8;
        break;
    case 'x':
    case 'X':
        base = 16;
        break;
    case 'c':
    case 'd':
    case 'n':
        break;
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case '%': {
        double number = toDouble(globalObject, value);
        RETURN_IF_EXCEPTION(scope, nullptr);
        RELEASE_AND_RETURN(scope, formatFloatWithSpec(globalObject, number, spec));
    }
    default:
        RELEASE_AND_RETURN(scope, raiseUnknownFormatCode(globalObject, spec.type, self));
    }

    if (spec.precision != -1)
        return fail("Precision not allowed in integer format specifier"_s);
    if (spec.noNegativeZero)
        return fail("Negative zero coercion (z) not allowed in integer format specifier"_s);

    NumberParts parts;
    String text;
    static constexpr Latin1Character prefixes[] = { '0', 'b', '0', 'o', '0', 'x' };
    if (spec.type == 'c') {
        if (spec.sign)
            return fail("Sign not allowed with integer format specifier 'c'"_s);
        if (spec.alternate)
            return fail("Alternate form (#) not allowed with integer format specifier 'c'"_s);
        auto character = tryInt64(value);
        if (!character) {
            raiseOverflowError(globalObject, scope, "Python int too large to convert to C long"_s);
            return nullptr;
        }
        if (*character < 0 || *character > 0x10ffff) {
            raiseOverflowError(globalObject, scope, "%c arg not in range(0x110000)"_s);
            return nullptr;
        }
        parts.remainder = stringFromCodePoint(static_cast<char32_t>(*character));
    } else {
        text = intToString(globalObject, value, base);
        RETURN_IF_EXCEPTION(scope, nullptr);
        auto characters = text.span8();
        if (characters[0] == '-') {
            parts.sign = '-';
            characters = characters.subspan(1);
        }
        parts.digits = characters;
        if (spec.alternate && base != 10)
            parts.prefix = std::span<const Latin1Character> { prefixes + (base == 2 ? 0 : base == 8 ? 2 : 4), 2 };
    }
    StrBuilder builder;
    appendNumber(builder, spec, parts, spec.type == 'X');
    if (builder.hasOverflowed()) {
        raiseMemoryError(globalObject, scope, String());
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

// ---- complex ----

JSString* formatComplex(JSGlobalObject* globalObject, JSValue self, JSString* specString)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!specString->length())
        RELEASE_AND_RETURN(scope, str(globalObject, self));
    String specText = specString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    FormatSpec spec;
    if (failed(scope, parseFormatSpec(globalObject, CodePoints(specText), self, 0, '>', spec)))
        return nullptr;
    switch (spec.type) {
    case 0:
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'n':
        break;
    default:
        RELEASE_AND_RETURN(scope, raiseUnknownFormatCode(globalObject, spec.type, self));
    }
    if (failed(scope, checkPrecision(globalObject, spec.precision)))
        return nullptr;
    if (spec.fill == '0') {
        raiseValueError(globalObject, scope, "Zero padding is not allowed in complex format specifier"_s);
        return nullptr;
    }
    if (spec.align == '=') {
        raiseValueError(globalObject, scope, "'=' alignment flag is not allowed in complex format specifier"_s);
        return nullptr;
    }

    double real = PyComplex::from(self)->real();
    double imaginary = PyComplex::from(self)->imaginary();
    int precision = static_cast<int>(spec.precision);
    int defaultPrecision = 6;
    char32_t type = spec.type;
    unsigned flags = 0;
    if (spec.alternate)
        flags |= FloatAlternate;
    if (spec.noNegativeZero)
        flags |= FloatNoNegativeZero;
    bool skipReal = false;
    bool addParentheses = false;
    if (!type) {
        // As str().
        type = 'r';
        defaultPrecision = 0;
        if (!real && !std::signbit(real))
            skipReal = true;
        else
            addParentheses = true;
    }
    if (type == 'n')
        type = 'g';
    if (precision < 0)
        precision = defaultPrecision;
    else if (type == 'r')
        type = 'g';

    String realText = formatDouble(real, static_cast<char>(type), precision, flags);
    String imaginaryText = formatDouble(imaginary, static_cast<char>(type), precision, flags);
    if (!realText || !imaginaryText) [[unlikely]] {
        raiseMemoryError(globalObject, scope, String());
        return nullptr;
    }
    // The two numbers without padding, which goes around the whole.
    FormatSpec inner = spec;
    inner.align = '<';
    inner.width = -1;
    StrBuilder number;
    if (addParentheses)
        number.append('(');
    if (!skipReal) {
        appendNumber(number, inner, splitNumber(realText), false);
        inner.sign = '+';
    }
    appendNumber(number, inner, splitNumber(imaginaryText), false);
    number.append('j');
    if (addParentheses)
        number.append(')');

    int64_t left, right;
    paddingFor(number.length(), spec.width, spec.align, left, right);
    StrBuilder builder;
    appendFill(builder, spec.fill, left);
    builder.append(static_cast<StringBuilder&>(number));
    appendFill(builder, spec.fill, right);
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

// ---- str.format() ----

namespace {

// One stretch of a format string: literal text, then maybe a replacement field.
struct Markup {
    unsigned literalStart { 0 };
    unsigned literalEnd { 0 };
    bool hasField { false };
    unsigned nameStart { 0 };
    unsigned nameEnd { 0 };
    bool hasSpec { false };
    unsigned specStart { 0 };
    unsigned specEnd { 0 };
    bool specNeedsExpanding { false };
    char32_t conversion { 0 };
};

enum class AutoNumbering : uint8_t { Undecided, Automatic, Manual };

struct FormatArguments {
    const CallArgs* args { nullptr }; // Null for format_map().
    JSValue mapping; // For format_map().
    AutoNumbering numbering { AutoNumbering::Undecided };
    int64_t nextIndex { 0 };
    // CPython does not copy what is written last into a result that is still empty: the result is then that object, of whatever
    // class derived from str.
    bool isAtLastField { false };
    JSValue whole;
};

}

// After the '{' of a field. False: an exception.
static bool parseField(JSGlobalObject* globalObject, const CodePoints& text, unsigned& position, unsigned end, Markup& markup)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto fail = [&](ASCIILiteral message) {
        raiseValueError(globalObject, scope, message);
        return false;
    };
    char32_t c = 0;
    markup.nameStart = position;
    while (position < end) {
        c = text[position++];
        if (c == '{')
            return fail("unexpected '{' in field name"_s);
        if (c == '[') {
            while (position < end && text[position] != ']')
                ++position;
            continue;
        }
        if (c == '}' || c == ':' || c == '!')
            break;
    }
    markup.nameEnd = position - 1;
    if (c == '!' || c == ':') {
        if (c == '!') {
            if (position >= end)
                return fail("end of string while looking for conversion specifier"_s);
            markup.conversion = text[position++];
            if (position < end) {
                c = text[position++];
                if (c == '}')
                    return true;
                if (c != ':')
                    return fail("expected ':' after conversion specifier"_s);
            }
        }
        markup.hasSpec = true;
        markup.specStart = position;
        unsigned depth = 1;
        while (position < end) {
            c = text[position++];
            if (c == '{') {
                markup.specNeedsExpanding = true;
                ++depth;
            } else if (c == '}' && !--depth) {
                markup.specEnd = position - 1;
                return true;
            }
        }
        return fail("unmatched '{' in format spec"_s);
    }
    if (c != '}')
        return fail("expected '}' before end of string"_s);
    return true;
}

// 1: there is no more; 2: `markup` is the next stretch; 0: an exception.
static int nextMarkup(JSGlobalObject* globalObject, const CodePoints& text, unsigned& position, unsigned end, Markup& markup)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    markup = Markup();
    if (position >= end)
        return 1;
    unsigned start = position;
    char32_t c = 0;
    bool markupFollows = false;
    while (position < end) {
        c = text[position++];
        if (c == '{' || c == '}') {
            markupFollows = true;
            break;
        }
    }
    bool atEnd = position >= end;
    unsigned length = position - start;
    if (c == '}' && (atEnd || text[position] != c)) {
        raiseValueError(globalObject, scope, "Single '}' encountered in format string"_s);
        return 0;
    }
    if (atEnd && c == '{') {
        raiseValueError(globalObject, scope, "Single '{' encountered in format string"_s);
        return 0;
    }
    if (!atEnd) {
        if (c == text[position]) {
            // A doubled brace: the literal takes one of the two.
            ++position;
            markupFollows = false;
        } else
            --length;
    }
    markup.literalStart = start;
    markup.literalEnd = start + length;
    if (!markupFollows)
        return 2;
    markup.hasField = true;
    if (failed(scope, parseField(globalObject, text, position, end, markup)))
        return 0;
    return 2;
}

// The text as an index, -1 if it is not one, or -2 with an exception.
static int64_t parseIndex(JSGlobalObject* globalObject, const CodePoints& text, unsigned start, unsigned end)
{
    if (start >= end)
        return -1;
    int64_t accumulator = 0;
    for (unsigned i = start; i < end; ++i) {
        int digit = decimalDigitValue(text[i]);
        if (digit < 0)
            return -1;
        if (accumulator > (maxSsize - digit) / 10) {
            VM& vm = getVM(globalObject);
            auto scope = DECLARE_THROW_SCOPE(vm);
            raiseValueError(globalObject, scope, "Too many decimal digits in format string"_s);
            return -2;
        }
        accumulator = accumulator * 10 + digit;
    }
    return accumulator;
}

namespace {

// The ".name" and "[key]" parts of a field name.
struct FieldNamePart {
    bool isAttribute;
    int64_t index; // -1: the key is the text.
    unsigned start;
    unsigned end;
};

}

// 1: no more; 2: `part` is the next; 0: an exception.
static int nextFieldNamePart(JSGlobalObject* globalObject, const CodePoints& text, unsigned& position, unsigned end, FieldNamePart& part)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (position >= end)
        return 1;
    char32_t c = text[position++];
    part.index = -1;
    if (c == '.') {
        part.isAttribute = true;
        part.start = position;
        while (position < end && text[position] != '[' && text[position] != '.')
            ++position;
        part.end = position;
    } else if (c == '[') {
        part.isAttribute = false;
        part.start = position;
        bool sawBracket = false;
        while (position < end) {
            if (text[position++] == ']') {
                sawBracket = true;
                break;
            }
        }
        if (!sawBracket) {
            raiseValueError(globalObject, scope, "Missing ']' in format string"_s);
            return 0;
        }
        part.end = position - 1;
        part.index = parseIndex(globalObject, text, part.start, part.end);
        EXCEPTION_ASSERT(!!scope.exception() == (part.index == -2));
        if (part.index == -2)
            return 0;
    } else {
        raiseValueError(globalObject, scope, "Only '.' or '[' may follow ']' in format field specifier"_s);
        return 0;
    }
    if (part.start == part.end) {
        raiseValueError(globalObject, scope, "Empty attribute in format string"_s);
        return 0;
    }
    return 2;
}

static JSValue fieldObject(JSGlobalObject* globalObject, const CodePoints& text, unsigned start, unsigned end, FormatArguments& arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    unsigned firstEnd = start;
    while (firstEnd < end && text[firstEnd] != '[' && text[firstEnd] != '.')
        ++firstEnd;
    int64_t index = parseIndex(globalObject, text, start, firstEnd);
    EXCEPTION_ASSERT(!!scope.exception() == (index == -2));
    if (index == -2)
        return { };
    bool isEmpty = start >= firstEnd;
    if (isEmpty || index != -1) {
        if (arguments.numbering == AutoNumbering::Undecided)
            arguments.numbering = isEmpty ? AutoNumbering::Automatic : AutoNumbering::Manual;
        if (arguments.numbering == AutoNumbering::Manual && isEmpty)
            return raiseValueError(globalObject, scope, "cannot switch from manual field specification to automatic field numbering"_s);
        if (arguments.numbering == AutoNumbering::Automatic && !isEmpty)
            return raiseValueError(globalObject, scope, "cannot switch from automatic field numbering to manual field specification"_s);
        if (isEmpty)
            index = arguments.nextIndex++;
    }

    JSValue object;
    if (index == -1) {
        JSString* key = makeStr(vm, text.substring(start, firstEnd));
        if (arguments.args) {
            object = arguments.args->keyword(key);
            if (!object)
                return raiseKeyError(globalObject, scope, key);
        } else {
            object = getItem(globalObject, arguments.mapping, key);
            RETURN_IF_EXCEPTION(scope, { });
        }
    } else {
        if (!arguments.args)
            return raiseValueError(globalObject, scope, "Format string contains positional fields"_s);
        if (index >= arguments.args->size())
            return raiseIndexError(globalObject, scope, "Replacement index "_s, index, " out of range for positional args tuple"_s);
        object = (*arguments.args)[static_cast<unsigned>(index)];
    }

    unsigned position = firstEnd;
    FieldNamePart part;
    while (true) {
        int status = nextFieldNamePart(globalObject, text, position, end, part);
        RETURN_IF_EXCEPTION(scope, { });
        if (status != 2)
            return object;
        if (part.isAttribute)
            object = getAttr(globalObject, object, internString(vm, text.substring(part.start, part.end)));
        else if (part.index == -1)
            object = getItem(globalObject, object, makeStr(vm, text.substring(part.start, part.end)));
        else
            object = getItem(globalObject, object, makeInt(globalObject, part.index));
        RETURN_IF_EXCEPTION(scope, { });
    }
}

static bool expandFormat(JSGlobalObject*, const CodePoints&, unsigned start, unsigned end, FormatArguments&, int depth, StringBuilder&);

static bool appendField(JSGlobalObject* globalObject, const CodePoints& text, const Markup& markup, FormatArguments& arguments, int depth, StringBuilder& builder)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue object = fieldObject(globalObject, text, markup.nameStart, markup.nameEnd, arguments);
    RETURN_IF_EXCEPTION(scope, false);

    switch (markup.conversion) {
    case 0:
        break;
    case 'r':
        object = repr(globalObject, object);
        break;
    case 's':
        object = str(globalObject, object);
        break;
    case 'a':
        object = ascii(globalObject, object);
        break;
    default:
        raiseValueError(globalObject, scope, "Unknown conversion specifier "_s, markup.conversion > 32 && markup.conversion < 127 ? makeString(static_cast<char>(markup.conversion)) : makeString("\\x"_s, hex(static_cast<unsigned>(markup.conversion), Lowercase)));
        return false;
    }
    RETURN_IF_EXCEPTION(scope, false);

    bool mayBeWhole = arguments.isAtLastField && builder.isEmpty();
    String spec;
    if (markup.specNeedsExpanding) {
        StrBuilder expanded;
        if (failed(scope, expandFormat(globalObject, text, markup.specStart, markup.specEnd, arguments, depth - 1, expanded)))
            return false;
        if (expanded.hasOverflowed()) {
            raiseMemoryError(globalObject, scope, String());
            return false;
        }
        spec = expanded.toString();
    } else if (markup.hasSpec)
        spec = text.substring(markup.specStart, markup.specEnd);

    JSValue formatted = formatObject(globalObject, object, spec.isEmpty() ? nullptr : makeStr(vm, spec));
    RETURN_IF_EXCEPTION(scope, false);
    JSString* result = strValue(formatted);
    auto view = result->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    builder.append(view.data);
    if (mayBeWhole && !formatted.isString() && result->length()) [[unlikely]]
        arguments.whole = formatted;
    return true;
}

static bool expandFormat(JSGlobalObject* globalObject, const CodePoints& text, unsigned start, unsigned end, FormatArguments& arguments, int depth, StringBuilder& builder)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (depth <= 0) {
        raiseValueError(globalObject, scope, "Max string recursion exceeded"_s);
        return false;
    }
    unsigned position = start;
    Markup markup;
    while (true) {
        int status = nextMarkup(globalObject, text, position, end, markup);
        EXCEPTION_ASSERT(!!scope.exception() == !status);
        if (status != 2)
            return status == 1;
        text.appendTo(builder, markup.literalStart, markup.literalEnd);
        if (markup.hasField) {
            arguments.isAtLastField = depth == 2 && position == end;
            if (failed(scope, appendField(globalObject, text, markup, arguments, depth, builder)))
                return false;
        }
    }
}

static JSValue formatWith(JSGlobalObject* globalObject, JSValue self, FormatArguments& arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String string = strValue(self)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    CodePoints text(string);
    if (!text.hasRoom()) [[unlikely]]
        return raiseMemoryError(globalObject, scope, String());
    StrBuilder builder;
    // A field may be nested in the format spec of another, and no deeper.
    if (failed(scope, expandFormat(globalObject, text, 0, text.length(), arguments, 2, builder)))
        return { };
    if (builder.hasOverflowed())
        return raiseMemoryError(globalObject, scope, String());
    if (arguments.whole) [[unlikely]]
        return arguments.whole;
    // Text with nothing to replace in it is the result as it is, be it an instance of a class derived from str.
    if (!self.isString() && !string.isEmpty() && string.find('{') == notFound && string.find('}') == notFound)
        return self;
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

JSValue strFormatMethod(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    FormatArguments arguments;
    arguments.args = &args;
    return formatWith(globalObject, self, arguments);
}

JSValue strFormatMap(JSGlobalObject* globalObject, JSValue self, JSValue mapping)
{
    FormatArguments arguments;
    arguments.mapping = mapping;
    return formatWith(globalObject, self, arguments);
}

// _string.formatter_parser(): (literal, field_name, format_spec, conversion) for each stretch.
JSValue formatterParser(JSGlobalObject* globalObject, JSValue formatString)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(formatString))
        return raiseTypeError(globalObject, scope, "expected str, got "_s, typeName(globalObject, formatString));
    String string = strValue(formatString)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    CodePoints text(string);
    if (!text.hasRoom()) [[unlikely]]
        return raiseMemoryError(globalObject, scope, String());
    JSArray* result = newList(globalObject);
    unsigned position = 0;
    Markup markup;
    int status;
    while ((status = nextMarkup(globalObject, text, position, text.length(), markup)) == 2) {
        JSValue literal = makeStr(vm, text.substring(markup.literalStart, markup.literalEnd));
        JSValue name = markup.hasField ? JSValue(makeStr(vm, text.substring(markup.nameStart, markup.nameEnd))) : jsUndefined();
        JSValue spec = markup.hasField ? JSValue(makeStr(vm, markup.hasSpec ? text.substring(markup.specStart, markup.specEnd) : String())) : jsUndefined();
        JSValue conversion = markup.conversion ? JSValue(makeStr(vm, stringFromCodePoint(markup.conversion))) : jsUndefined();
        listAppend(globalObject, result, PyTuple::create(globalObject, { literal, name, spec, conversion }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    EXCEPTION_ASSERT(!!scope.exception() == !status);
    if (!status)
        return { };
    RELEASE_AND_RETURN(scope, getIter(globalObject, result));
}

// _string.formatter_field_name_split(): (first, iterator of (is_attribute, key)).
JSValue formatterFieldNameSplit(JSGlobalObject* globalObject, JSValue fieldName)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(fieldName))
        return raiseTypeError(globalObject, scope, "expected str, got "_s, typeName(globalObject, fieldName));
    String string = strValue(fieldName)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    CodePoints text(string);
    if (!text.hasRoom()) [[unlikely]]
        return raiseMemoryError(globalObject, scope, String());
    unsigned end = text.length();
    unsigned firstEnd = 0;
    while (firstEnd < end && text[firstEnd] != '[' && text[firstEnd] != '.')
        ++firstEnd;
    int64_t index = parseIndex(globalObject, text, 0, firstEnd);
    if (index == -2)
        return { };
    JSValue first = index == -1 ? JSValue(makeStr(vm, text.substring(0, firstEnd))) : makeInt(globalObject, index);

    JSArray* rest = newList(globalObject);
    unsigned position = firstEnd;
    FieldNamePart part;
    int status;
    while ((status = nextFieldNamePart(globalObject, text, position, end, part)) == 2) {
        JSValue key = part.index == -1 ? JSValue(makeStr(vm, text.substring(part.start, part.end))) : makeInt(globalObject, part.index);
        listAppend(globalObject, rest, PyTuple::create(globalObject, { jsBoolean(part.isAttribute), key }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    EXCEPTION_ASSERT(!!scope.exception() == !status);
    if (!status)
        return { };
    JSValue iterator = getIter(globalObject, rest);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { first, iterator }));
}

// ---- format % arguments ----

namespace {

enum PercentFlag : uint8_t {
    LeftJustify = 1 << 0,
    AlwaysSign = 1 << 1,
    BlankSign = 1 << 2,
    Alternate = 1 << 3,
    ZeroPad = 1 << 4,
};

struct PercentArgument {
    char32_t code { 0 };
    unsigned flags { 0 };
    int64_t width { -1 };
    int precision { -1 };
    bool isNumber { false };
    unsigned start { 0 }; // Where the % is.
    JSValue key;
    JSValue object; // For %s, %r and %a: what was converted to, if that is of a class derived from str.
};

struct PercentFormatter {
    JSGlobalObject* globalObject;
    const CodePoints& text;
    unsigned position { 0 };
    JSValue arguments;
    // As CPython counts: the length is -1 for a single value that is not a tuple, and below that for one found by key.
    int64_t argumentCount;
    int64_t argumentIndex;
    JSValue mapping;
    ShortStrBuilder builder;

    bool atEnd() const { return position >= text.length(); }

    String errorPrefix(const PercentArgument& argument)
    {
        if (argument.key)
            return makeString("format argument "_s, reprForMessage(globalObject, argument.key), ": "_s);
        if (argumentIndex >= 0)
            return makeString("format argument "_s, argumentIndex, ": "_s);
        return "format argument: "_s;
    }

    JSValue nextArgument(bool allowSingle)
    {
        if (argumentIndex < argumentCount && (allowSingle || argumentCount >= 0)) {
            int64_t index = argumentIndex++;
            if (argumentCount >= 0)
                return PyTuple::from(arguments)->at(static_cast<unsigned>(index));
            if (allowSingle)
                return arguments;
        }
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseTypeError(globalObject, scope, "not enough arguments for format string (got "_s, argumentCount < 0 ? 1 : argumentCount, ')');
    }

    // What nextArgument(true) is going to return, where arguments are taken one after the other. Empty: it has something to say.
    JSValue argumentToCome() const
    {
        if (argumentIndex >= argumentCount || argumentCount < -1)
            return { };
        return argumentCount >= 0 ? PyTuple::from(arguments)->at(static_cast<unsigned>(argumentIndex)) : arguments;
    }
};

}

// "-"? ("0x" | "0X")? digit+, with at least `precision` digits.
static String formatIntForPercent(JSGlobalObject* globalObject, JSValue value, bool alternate, int precision, char32_t code)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned base = code == 'o' ? 8 : code == 'x' || code == 'X' ? 16 : 10;
    String digits = intToString(globalObject, value, base);
    RETURN_IF_EXCEPTION(scope, { });
    bool negative = digits[0] == '-';
    StringView magnitude = StringView(digits).substring(negative);
    StrBuilder builder;
    if (negative)
        builder.append('-');
    if (alternate && base != 10)
        builder.append('0', static_cast<char>(code));
    appendRepeated(builder, '0', precision - static_cast<int64_t>(magnitude.length()));
    if (code == 'X')
        builder.append(magnitude.convertToASCIIUppercase());
    else
        builder.append(magnitude);
    if (builder.hasOverflowed()) {
        raiseMemoryError(globalObject, scope, String());
        return { };
    }
    return builder.toString();
}

static bool isNumberLike(JSGlobalObject* globalObject, JSValue value)
{
    if (isInt(value) || isFloat(value) || PyComplex::is(value))
        return true;
    PyType* type = typeOf(globalObject, value);
    return type->lookup(Special::Index) || type->lookup(Special::Int) || type->lookup(Special::Float);
}

// Reads what is between the % and the conversion character, which is left in `argument.code`.
static bool parsePercentArgument(PercentFormatter& f, PercentArgument& argument)
{
    JSGlobalObject* globalObject = f.globalObject;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const CodePoints& text = f.text;
    unsigned end = text.length();
    // True if there was a character to read.
    bool exhausted = false;
    auto advance = [&] {
        if (f.position < end) {
            argument.code = text[f.position++];
            return true;
        }
        exhausted = true;
        return false;
    };

    if (argument.code == '(') {
        if (!f.mapping) {
            raiseTypeError(globalObject, scope, "format requires a mapping, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, f.arguments)));
            return false;
        }
        ++f.position;
        unsigned keyStart = f.position;
        unsigned depth = 1;
        while (depth && f.position < end) {
            char32_t c = text[f.position++];
            if (c == ')')
                --depth;
            else if (c == '(')
                ++depth;
        }
        if (depth) {
            raiseValueError(globalObject, scope, "stray % or incomplete format key at position "_s, argument.start);
            return false;
        }
        argument.key = makeStr(vm, text.substring(keyStart, f.position - 1));
        f.arguments = getItem(globalObject, f.mapping, argument.key);
        RETURN_IF_EXCEPTION(scope, false);
        f.argumentCount = -3;
        f.argumentIndex = -4;
    } else if (f.argumentCount < -1) {
        raiseValueError(globalObject, scope, "format requires a parenthesised mapping key at position "_s, argument.start);
        return false;
    }

    while (advance()) {
        switch (argument.code) {
        case '-':
            argument.flags |= LeftJustify;
            continue;
        case '+':
            argument.flags |= AlwaysSign;
            continue;
        case ' ':
            argument.flags |= BlankSign;
            continue;
        case '#':
            argument.flags |= Alternate;
            continue;
        case '0':
            argument.flags |= ZeroPad;
            continue;
        }
        break;
    }

    auto starArgument = [&]() -> JSValue {
        if (f.argumentCount < -1)
            return raiseValueError(globalObject, scope, "* cannot be used with a parenthesised mapping key at position "_s, argument.start);
        JSValue value = f.nextArgument(false);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isInt(value))
            return raiseTypeError(globalObject, scope, f.errorPrefix(argument), "* requires int, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)));
        return intValue(value);
    };

    if (!exhausted && argument.code == '*') {
        JSValue value = starArgument();
        RETURN_IF_EXCEPTION(scope, false);
        auto width = tryInt64(value);
        if (!width || *width == std::numeric_limits<int64_t>::min()) {
            raiseOverflowError(globalObject, scope, f.errorPrefix(argument), "too big for width"_s);
            return false;
        }
        argument.width = *width;
        if (argument.width < 0) {
            argument.flags |= LeftJustify;
            argument.width = -argument.width;
        }
        advance();
    } else if (!exhausted && isASCIIDigit(argument.code)) {
        argument.width = argument.code - '0';
        while (advance()) {
            if (!isASCIIDigit(argument.code))
                break;
            int digit = argument.code - '0';
            if (argument.width > (maxSsize - digit) / 10) {
                raiseValueError(globalObject, scope, "width too big at position "_s, argument.start);
                return false;
            }
            argument.width = argument.width * 10 + digit;
        }
    }

    if (!exhausted && argument.code == '.') {
        argument.precision = 0;
        advance();
        if (!exhausted && argument.code == '*') {
            JSValue value = starArgument();
            RETURN_IF_EXCEPTION(scope, false);
            auto precision = tryInt64(value);
            if (!precision || *precision > std::numeric_limits<int>::max() || *precision < std::numeric_limits<int>::min()) {
                raiseOverflowError(globalObject, scope, f.errorPrefix(argument), "too big for precision"_s);
                return false;
            }
            argument.precision = std::max<int>(static_cast<int>(*precision), 0);
            advance();
        } else if (!exhausted && isASCIIDigit(argument.code)) {
            argument.precision = argument.code - '0';
            while (advance()) {
                if (!isASCIIDigit(argument.code))
                    break;
                int digit = argument.code - '0';
                if (argument.precision > (std::numeric_limits<int>::max() - digit) / 10) {
                    raiseValueError(globalObject, scope, "precision too big at position "_s, argument.start);
                    return false;
                }
                argument.precision = argument.precision * 10 + digit;
            }
        }
    }

    // As C has them, and they mean nothing.
    if (!exhausted && (argument.code == 'h' || argument.code == 'l' || argument.code == 'L'))
        advance();
    if (exhausted) {
        raiseValueError(globalObject, scope, "stray % at position "_s, argument.start);
        return false;
    }
    return true;
}

// The text of the argument, before it is padded.
static bool convertPercentArgument(PercentFormatter& f, PercentArgument& argument, String& result)
{
    JSGlobalObject* globalObject = f.globalObject;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue value = f.nextArgument(true);
    RETURN_IF_EXCEPTION(scope, false);
    char32_t code = argument.code;
    switch (code) {
    case 's':
    case 'r':
    case 'a': {
        JSValue object = code == 's' ? strObject(globalObject, value) : reprObject(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        result = strValue(object)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (code == 'a' && !result.containsOnlyASCII())
            result = asciiEscape(result);
        else if (!object.isString()) [[unlikely]]
            argument.object = object;
        return true;
    }
    case 'i':
    case 'd':
    case 'u':
    case 'o':
    case 'x':
    case 'X': {
        bool needsInteger = code == 'o' || code == 'x' || code == 'X';
        auto wrongType = [&] {
            raiseTypeError(globalObject, scope, f.errorPrefix(argument), '%', static_cast<char>(code), needsInteger ? " requires an integer, not "_s : " requires a real number, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)));
            return false;
        };
        if (!isNumberLike(globalObject, value))
            return wrongType();
        JSValue integer;
        if (isInt(value))
            integer = intValue(value);
        else {
            integer = needsInteger ? toIndex(globalObject, value) : toInt(globalObject, value);
            if (failed(scope, integer)) {
                if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                    return wrongType();
                return false;
            }
        }
        if (argument.precision > std::numeric_limits<int>::max() - 3) {
            raiseOverflowError(globalObject, scope, "precision too large"_s);
            return false;
        }
        result = formatIntForPercent(globalObject, integer, argument.flags & Alternate, argument.precision, code);
        RETURN_IF_EXCEPTION(scope, false);
        argument.isNumber = true;
        return true;
    }
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G': {
        double number = toDouble(globalObject, value);
        if (scope.exception()) {
            if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                raiseTypeError(globalObject, scope, f.errorPrefix(argument), '%', static_cast<char>(code), " requires a real number, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)));
            return false;
        }
        if (failed(scope, checkPrecision(globalObject, argument.precision)))
            return false;
        result = formatDouble(number, static_cast<char>(code), argument.precision < 0 ? 6 : argument.precision, argument.flags & Alternate ? FloatAlternate : 0);
        if (!result) [[unlikely]] {
            raiseMemoryError(globalObject, scope, String());
            return false;
        }
        argument.isNumber = true;
        return true;
    }
    case 'c': {
        if (isStr(value)) {
            StrView view(globalObject, strValue(value));
            RETURN_IF_EXCEPTION(scope, false);
            if (view.length() != 1) {
                raiseTypeError(globalObject, scope, f.errorPrefix(argument), "%c requires an integer or a unicode character, not a string of length "_s, view.length());
                return false;
            }
            result = view.string();
            return true;
        }
        if (!hasIndex(globalObject, value)) {
            raiseTypeError(globalObject, scope, f.errorPrefix(argument), "%c requires an integer or a unicode character, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)));
            return false;
        }
        JSValue index = toIndex(globalObject, value);
        if (failed(scope, index)) {
            // Whatever TypeError __index__() ends in.
            if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                raiseTypeError(globalObject, scope, f.errorPrefix(argument), "%c requires an integer or a unicode character, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)));
            return false;
        }
        auto character = tryInt64(index);
        if (!character || *character < 0 || *character > 0x10ffff) {
            raiseOverflowError(globalObject, scope, f.errorPrefix(argument), "%c argument not in range(0x110000)"_s);
            return false;
        }
        result = stringFromCodePoint(static_cast<char32_t>(*character));
        return true;
    }
    default:
        break;
    }

    unsigned at = f.position - 1;
    if (code < 128 && isASCIIAlpha(code))
        raiseValueError(globalObject, scope, "unsupported format %"_s, static_cast<char>(code), " at position "_s, argument.start);
    else if (code == '\'')
        raiseValueError(globalObject, scope, "stray % at position "_s, argument.start, " or unexpected format character \"'\" at position "_s, at);
    else if (code >= 32 && code < 127)
        raiseValueError(globalObject, scope, "stray % at position "_s, argument.start, " or unexpected format character '"_s, static_cast<char>(code), "' at position "_s, at);
    else if (isPrintable(code))
        raiseValueError(globalObject, scope, "stray % at position "_s, argument.start, " or unexpected format character '"_s, stringFromCodePoint(code), "' (U+"_s, hex(static_cast<unsigned>(code), 4), ") at position "_s, at);
    else
        raiseValueError(globalObject, scope, "stray % at position "_s, argument.start, " or unexpected format character U+"_s, hex(static_cast<unsigned>(code), 4), " at position "_s, at);
    return false;
}

static void appendPercentArgument(PercentFormatter& f, PercentArgument& argument, const String& string)
{
    bool hasSignFlag = argument.flags & (AlwaysSign | BlankSign);
    if (argument.width == -1 && (argument.precision == -1 || argument.isNumber) && !hasSignFlag) {
        f.builder.append(StringView(string));
        return;
    }
    CodePoints text(string);
    if (!text.hasRoom()) [[unlikely]]
        return f.builder.builder().didOverflow();
    int64_t length = text.length();
    char fill = argument.isNumber && (argument.flags & ZeroPad) ? '0' : ' ';
    if ((argument.width == -1 || argument.width <= length) && (argument.precision == -1 || argument.precision >= length) && !hasSignFlag) {
        f.builder.append(StringView(string));
        return;
    }
    StringBuilder& builder = f.builder.builder();
    if ((argument.code == 's' || argument.code == 'r' || argument.code == 'a') && argument.precision >= 0 && length > argument.precision)
        length = argument.precision;

    unsigned index = 0;
    char sign = 0;
    bool hasSign = argument.isNumber;
    if (hasSign) {
        char32_t first = text[0];
        if (first == '-' || first == '+') {
            sign = static_cast<char>(first);
            --length;
            ++index;
        } else if (argument.flags & AlwaysSign)
            sign = '+';
        else if (argument.flags & BlankSign)
            sign = ' ';
        else
            hasSign = false;
    }
    int64_t width = std::max(argument.width, length);
    if (hasSign) {
        if (fill != ' ')
            builder.append(sign);
        if (width > length)
            --width;
    }
    bool hasPrefix = (argument.flags & Alternate) && (argument.code == 'x' || argument.code == 'X' || argument.code == 'o');
    if (hasPrefix) {
        if (fill != ' ') {
            builder.append('0', static_cast<char>(argument.code));
            index += 2;
        }
        width = std::max<int64_t>(width - 2, 0);
        length -= 2;
    }
    if (width > length && !(argument.flags & LeftJustify)) {
        appendRepeated(builder, fill, width - length);
        width = length;
    }
    if (fill == ' ') {
        if (hasSign)
            builder.append(sign);
        if (hasPrefix) {
            builder.append('0', static_cast<char>(argument.code));
            index += 2;
        }
    }
    text.appendTo(builder, index, index + static_cast<unsigned>(length));
    appendRepeated(builder, ' ', width - length);
}

JSValue strFormatPercent(JSGlobalObject* globalObject, JSString* formatString, JSValue arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String string = formatString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    CodePoints text(string);
    if (!text.hasRoom()) [[unlikely]]
        return raiseMemoryError(globalObject, scope, String());

    PercentFormatter f { globalObject, text, 0, arguments, -1, -2, { }, { } };
    if (auto* tuple = PyTuple::tryFrom(arguments)) {
        f.argumentCount = tuple->length();
        f.argumentIndex = 0;
    } else if (!isStr(arguments) && typeOf(globalObject, arguments)->lookup(Special::GetItem))
        f.mapping = arguments;

    unsigned end = text.length();
    JSValue whole;
    while (f.position < end) {
        if (text[f.position] != '%') {
            unsigned start = f.position;
            if (string.is8Bit()) {
                f.position = std::min<size_t>(find(string.span8(), static_cast<Latin1Character>('%'), start), end);
                f.builder.append(string.span8().subspan(start, f.position - start));
                continue;
            }
            while (f.position < end && text[f.position] != '%')
                ++f.position;
            text.appendTo(f.builder.builder(), start, f.position);
            continue;
        }
        PercentArgument argument;
        argument.start = f.position++;
        if (f.atEnd())
            return raiseValueError(globalObject, scope, "stray % at position "_s, argument.start);
        argument.code = text[f.position];
        if (argument.code == '%') {
            ++f.position;
            f.builder.append('%');
            continue;
        }
        // What most are.
        if (JSValue value = argument.code == 's' || argument.code == 'r' || argument.code == 'd' ? f.argumentToCome() : JSValue()) {
            if (isSmallInt(value)) {
                f.builder.append(smallIntValue(value));
                ++f.argumentIndex;
                ++f.position;
                continue;
            }
            if (argument.code != 'd') {
                JSValue object = argument.code == 'r' ? reprObject(globalObject, value) : value.isString() ? value : strObject(globalObject, value);
                RETURN_IF_EXCEPTION(scope, { });
                ++f.argumentIndex;
                ++f.position;
                JSString* converted = strValue(object);
                // See FormatArguments::whole.
                if (f.position == end && f.builder.isEmpty() && (object.isString() || converted->length())) {
                    whole = object;
                    continue;
                }
                auto view = converted->view(globalObject);
                RETURN_IF_EXCEPTION(scope, { });
                f.builder.append(view);
                continue;
            }
        }
        if (failed(scope, parsePercentArgument(f, argument)))
            return { };
        String converted;
        if (failed(scope, convertPercentArgument(f, argument, converted)))
            return { };
        // See FormatArguments::whole.
        if (argument.object && f.position >= end && f.builder.isEmpty() && !converted.isEmpty() && !(argument.flags & (AlwaysSign | BlankSign))) [[unlikely]] {
            int64_t length = CodePoints(converted).length();
            if (argument.width <= length && (argument.precision < 0 || argument.precision >= length))
                whole = argument.object;
        }
        appendPercentArgument(f, argument, converted);
    }
    if (f.argumentIndex < f.argumentCount && !f.mapping)
        return raiseTypeError(globalObject, scope, "not all arguments converted during string formatting (required "_s, f.argumentCount < 0 ? 0 : f.argumentIndex, ", got "_s, f.argumentCount < 0 ? 1 : f.argumentCount, ')');
    if (whole)
        return whole;
    String result = f.builder.toStringOrNull();
    if (!result) [[unlikely]]
        return raiseMemoryError(globalObject, scope, String());
    return makeStr(vm, WTF::move(result));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
