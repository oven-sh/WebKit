/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PythonStrings.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyObjects.h"
#include "PythonBytes.h"
#include "PythonNumbers.h"
#include "PythonSequences.h"
#include <unicode/uchar.h>
#include <wtf/dtoa.h>
#include <wtf/dtoa/double-conversion.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace Python {

// ---- Characters and code units

bool stringHasSurrogatePairs(StringView view)
{
    if (view.is8Bit())
        return false;
    for (char16_t c : view.span16()) {
        if (U16_IS_LEAD(c))
            return true; // A lone one is a character by itself, but then counting finds that out.
    }
    return false;
}

static unsigned countCharacters(StringView view)
{
    unsigned count = 0;
    auto span = view.span16();
    for (size_t i = 0; i < span.size(); ++i, ++count) {
        if (U16_IS_LEAD(span[i]) && i + 1 < span.size() && U16_IS_TRAIL(span[i + 1]))
            ++i;
    }
    return count;
}

unsigned stringLength(JSGlobalObject* globalObject, JSString* string)
{
    if (string->is8Bit())
        return string->length();
    auto view = string->view(globalObject);
    if (!stringHasSurrogatePairs(view))
        return string->length();
    return countCharacters(view);
}

unsigned stringOffsetOfCharacter(StringView view, unsigned character)
{
    auto span = view.span16();
    size_t i = 0;
    for (; character && i < span.size(); ++i, --character) {
        if (U16_IS_LEAD(span[i]) && i + 1 < span.size() && U16_IS_TRAIL(span[i + 1]))
            ++i;
    }
    return i;
}

JSValue stringGetItem(JSGlobalObject* globalObject, JSString* string, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    bool hasPairs = stringHasSurrogatePairs(view);
    int64_t length = hasPairs ? countCharacters(view) : view->length();

    if (auto* slice = trySlice(key)) {
        auto indices = slice->indices(globalObject, length);
        RETURN_IF_EXCEPTION(scope, { });
        if (!indices->length)
            return jsEmptyString(vm);
        if (indices->step == 1) {
            unsigned start = hasPairs ? stringOffsetOfCharacter(view, indices->start) : indices->start;
            unsigned end = hasPairs ? stringOffsetOfCharacter(view, indices->stop) : indices->stop;
            RELEASE_AND_RETURN(scope, jsSubstring(globalObject, string, start, end - start));
        }
        StringBuilder builder;
        int64_t at = indices->start;
        for (int64_t i = 0; i < indices->length; ++i, at += indices->step) {
            if (!hasPairs) {
                builder.append(view[at]);
                continue;
            }
            unsigned offset = stringOffsetOfCharacter(view, at);
            builder.append(view[offset]);
            if (U16_IS_LEAD(view[offset]) && offset + 1 < view->length() && U16_IS_TRAIL(view[offset + 1]))
                builder.append(view[offset + 1]);
        }
        return jsString(vm, builder.toString());
    }

    if (!classify(key).isInt() && !(key.isObject() && typeOf(globalObject, key)->lookup(vm, vm.pythonNames().dunder_index)))
        return raiseTypeError(globalObject, scope, makeString("string indices must be integers, not '"_s, typeName(globalObject, key), '\''));
    auto index = toIndex(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t i = *index < 0 ? *index + length : *index;
    if (i < 0 || i >= length)
        return raise(globalObject, scope, BuiltinType::IndexError, "string index out of range"_s);
    if (!hasPairs)
        return jsSingleCharacterString(vm, view[i]);
    unsigned offset = stringOffsetOfCharacter(view, i);
    unsigned size = U16_IS_LEAD(view[offset]) && offset + 1 < view->length() && U16_IS_TRAIL(view[offset + 1]) ? 2 : 1;
    RELEASE_AND_RETURN(scope, jsSubstring(globalObject, string, offset, size));
}

JSValue stringRepeat(JSGlobalObject* globalObject, JSString* string, int64_t count)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (count <= 0 || !string->length())
        return jsEmptyString(vm);
    if (count == 1)
        return string;
    // Too long to be counted, and too long for there to be room for it, are not the same to CPython.
    int64_t characters = stringLength(globalObject, string);
    if (count > std::numeric_limits<int64_t>::max() / characters)
        return raise(globalObject, scope, BuiltinType::OverflowError, "repeated string is too long"_s);
    if (count > static_cast<int64_t>(String::MaxLength / string->length()))
        return raiseMemoryError(globalObject, scope);
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    StringBuilder builder;
    builder.reserveCapacity(view->length() * count);
    for (int64_t i = 0; i < count; ++i)
        builder.append(view.data);
    return jsString(vm, builder.toString());
}

// ---- repr

static bool isPrintable(char32_t c)
{
    if (c == ' ')
        return true;
    switch (u_charType(c)) {
    case U_CONTROL_CHAR:
    case U_FORMAT_CHAR:
    case U_SURROGATE:
    case U_PRIVATE_USE_CHAR:
    case U_UNASSIGNED:
    case U_LINE_SEPARATOR:
    case U_PARAGRAPH_SEPARATOR:
    case U_SPACE_SEPARATOR:
        return false;
    default:
        return true;
    }
}

int compareStrings(StringView a, StringView b)
{
    unsigned common = std::min(a.length(), b.length());
    for (unsigned i = 0; i < common; ++i) {
        char16_t x = a[i];
        char16_t y = b[i];
        if (x == y)
            continue;
        // The halves of a pair are numbered below U+E000, and stand for what is above U+FFFF.
        auto rank = [] (char16_t c) -> unsigned { return U16_IS_SURROGATE(c) ? c + 0x10000 : c; };
        return rank(x) < rank(y) ? -1 : 1;
    }
    return a.length() == b.length() ? 0 : a.length() < b.length() ? -1 : 1;
}

String reprOfString(StringView view)
{
    // In single quotes, unless that would take escaping and double quotes would not.
    bool hasSingle = view.contains('\'');
    bool hasDouble = view.contains('"');
    char quote = hasSingle && !hasDouble ? '"' : '\'';
    StringBuilder builder;
    builder.append(quote);
    for (char32_t c : view.codePoints()) {
        switch (c) {
        case '\\':
            builder.append("\\\\"_s);
            continue;
        case '\n':
            builder.append("\\n"_s);
            continue;
        case '\r':
            builder.append("\\r"_s);
            continue;
        case '\t':
            builder.append("\\t"_s);
            continue;
        default:
            break;
        }
        if (c == static_cast<char32_t>(quote)) {
            builder.append('\\', quote);
            continue;
        }
        if (c >= ' ' && c < 0x7F) {
            builder.append(static_cast<Latin1Character>(c));
            continue;
        }
        if (c >= 0x7F && isPrintable(c)) {
            builder.append(c);
            continue;
        }
        if (c <= 0xFF)
            builder.append("\\x"_s, hex(static_cast<unsigned>(c), 2, Lowercase));
        else if (c <= 0xFFFF)
            builder.append("\\u"_s, hex(static_cast<unsigned>(c), 4, Lowercase));
        else
            builder.append("\\U"_s, hex(static_cast<unsigned>(c), 8, Lowercase));
    }
    builder.append(quote);
    return builder.toString();
}

// ---- Digits of floats

using Converter = WTF::double_conversion::DoubleToStringConverter;

struct Digits {
    Vector<char, 64> digits; // Without a point, and with no zeros at the end.
    int point { 0 }; // How many of them are before the point. It can be negative, or more than there are.
    bool isNegative { false };
};

// How many digits the exact decimal expansion of a float has after the point. It has that many because it is an odd number over a
// power of two, and then the last of them is a 5.
static int exactFractionDigits(double value)
{
    if (!value)
        return 0;
    int exponent;
    double mantissa = std::frexp(std::abs(value), &exponent);
    uint64_t bits = static_cast<uint64_t>(std::ldexp(mantissa, 53));
    exponent -= 53;
    int trailing = std::countr_zero(bits);
    exponent += trailing;
    return exponent < 0 ? -exponent : 0;
}

// double-conversion rounds a tie away from zero. Python rounds it to the even digit. `last` is the index of the digit that was rounded to.
static void roundTieToEven(Digits& result, int last)
{
    // It was rounded up from an exact ...5, so what is there is one more than what was before it, and there was no carry into it if it is odd.
    if (last < 0 || last >= static_cast<int>(result.digits.size()))
        return; // It is a zero, from a carry, which is even.
    if ((result.digits[last] - '0') & 1) {
        --result.digits[last];
        while (!result.digits.isEmpty() && result.digits.last() == '0')
            result.digits.removeLast();
    }
}

static Digits toDigits(double value, Converter::DtoaMode mode, int requested)
{
    Digits result;
    Vector<char, 512> buffer(std::max(requested, 0) + 400);
    int length;
    Converter::DoubleToAscii(value, mode, requested, buffer.mutableSpan(), result.isNegative, length, result.point);
    result.digits.append(buffer.span().first(length));
    while (!result.digits.isEmpty() && result.digits.last() == '0')
        result.digits.removeLast();
    return result;
}

// Rounded to `precision` places after the point.
static Digits fixedPointDigits(double value, int precision)
{
    Digits result = toDigits(value, Converter::FIXED, precision);
    if (exactFractionDigits(value) == precision + 1)
        roundTieToEven(result, result.point + precision - 1);
    return result;
}

// Rounded to `count` significant digits.
static Digits significantDigits(double value, int count)
{
    Digits result = toDigits(value, Converter::PRECISION, count);
    if (!value)
        return result;
    // A tie if the exact expansion has just one digit more.
    Digits shortest = toDigits(value, Converter::SHORTEST, 0);
    int fraction = exactFractionDigits(value);
    int exactCount = fraction ? shortest.point + fraction : static_cast<int>(shortest.digits.size());
    // If rounding up carried all the way, as from 9.5 to 10, the digit that was rounded to is a zero, and even.
    bool didCarry = result.point != shortest.point;
    if (!didCarry && exactCount == count + 1 && (fraction || (shortest.digits.last() == '5' && std::abs(value) < 9007199254740992.0)))
        roundTieToEven(result, count - 1);
    return result;
}

static void appendFixed(StringBuilder& builder, const Digits& digits, int precision, bool alwaysPoint)
{
    int size = digits.digits.size();
    if (digits.point <= 0)
        builder.append('0');
    for (int i = 0; i < digits.point; ++i)
        builder.append(i < size ? digits.digits[i] : '0');
    if (precision > 0 || alwaysPoint)
        builder.append('.');
    for (int i = 0; i < precision; ++i) {
        int index = digits.point + i;
        builder.append(index >= 0 && index < size ? digits.digits[index] : '0');
    }
}

static void appendExponential(StringBuilder& builder, const Digits& digits, int precision, bool alwaysPoint, char exponentCharacter, bool isZero)
{
    int size = digits.digits.size();
    builder.append(size ? digits.digits[0] : '0');
    if (precision > 0 || alwaysPoint)
        builder.append('.');
    for (int i = 1; i <= precision; ++i)
        builder.append(i < size ? digits.digits[i] : '0');
    int exponent = isZero ? 0 : digits.point - 1;
    builder.append(exponentCharacter, exponent < 0 ? '-' : '+');
    exponent = std::abs(exponent);
    if (exponent < 10)
        builder.append('0');
    builder.append(exponent);
}

String fixedDigits(double value, int precision)
{
    StringBuilder builder;
    Digits digits = fixedPointDigits(value, precision);
    if (digits.isNegative)
        builder.append('-');
    appendFixed(builder, digits, precision, false);
    return builder.toString();
}

double roundToDigits(double value, int digits)
{
    if (!std::isfinite(value) || !value)
        return value;
    // Beyond these it changes nothing, or leaves nothing.
    if (digits > 323)
        return value;
    if (digits < -308)
        return std::copysign(0.0, value);
    if (digits >= 0) {
        String text = fixedDigits(value, digits);
        size_t parsed;
        return parseDouble(text, parsed);
    }
    // To tens, hundreds and so on.
    Digits shortest = toDigits(value, Converter::SHORTEST, 0);
    int keep = shortest.point + digits;
    if (keep < 0)
        return std::copysign(0.0, value);
    StringBuilder builder;
    if (value < 0)
        builder.append('-');
    if (!keep) {
        // Whether it is more than half way to the first place.
        double unit = std::pow(10.0, -digits);
        return std::abs(value) > unit / 2 ? std::copysign(unit, value) : std::copysign(0.0, value);
    }
    Digits rounded = significantDigits(value, keep);
    for (int i = 0; i < rounded.point; ++i)
        builder.append(i < static_cast<int>(rounded.digits.size()) ? rounded.digits[i] : '0');
    size_t parsed;
    return parseDouble(builder.toString(), parsed);
}

// ---- The format specification

// One more digit on the end of a number that is being read. False if it would then be more than `limit`.
static bool appendDigit(int64_t& number, char32_t digit, int64_t limit = std::numeric_limits<int64_t>::max())
{
    int64_t value = digit - '0';
    if (number > (limit - value) / 10)
        return false;
    number = number * 10 + value;
    return true;
}

std::optional<FormatSpecification> parseFormatSpecification(JSGlobalObject* globalObject, StringView text, const String& typeName, bool isForString)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    FormatSpecification result;
    result.typeName = typeName;
    Vector<char32_t, 32> characters;
    for (char32_t c : text.codePoints())
        characters.append(c);
    size_t i = 0;
    size_t end = characters.size();
    auto isAlign = [] (char32_t c) { return c == '<' || c == '>' || c == '=' || c == '^'; };
    auto invalid = [&] () -> std::optional<FormatSpecification> {
        raiseValueError(globalObject, scope, makeString("Invalid format specifier '"_s, text, "' for object of type '"_s, typeName, '\''));
        return std::nullopt;
    };

    bool hasFill = false;
    if (end >= 2 && isAlign(characters[1])) {
        result.fill = characters[0];
        result.align = characters[1];
        hasFill = true;
        i = 2;
    } else if (end >= 1 && isAlign(characters[0])) {
        result.align = characters[0];
        i = 1;
    }
    if (i < end && (characters[i] == '+' || characters[i] == '-' || characters[i] == ' ')) {
        result.sign = characters[i++];
        result.hasSign = true;
    }
    if (i < end && characters[i] == 'z') {
        result.noNegativeZero = true;
        ++i;
    }
    if (i < end && characters[i] == '#') {
        result.alternate = true;
        ++i;
    }
    if (i < end && characters[i] == '0' && !hasFill) {
        result.fill = '0';
        if (!result.align && !isForString)
            result.align = '=';
        ++i;
    }
    while (i < end && isASCIIDigit(characters[i])) {
        result.hasWidth = true;
        if (!appendDigit(result.width, characters[i++])) {
            raiseValueError(globalObject, scope, "Too many decimal digits in format string"_s);
            return std::nullopt;
        }
    }
    if (i < end && (characters[i] == ',' || characters[i] == '_')) {
        result.grouping = characters[i++];
        if (i < end && (characters[i] == ',' || characters[i] == '_') && characters[i] != static_cast<char32_t>(result.grouping)) {
            raiseValueError(globalObject, scope, "Cannot specify both ',' and '_'."_s);
            return std::nullopt;
        }
    }
    if (i < end && characters[i] == '.') {
        ++i;
        bool hasSomething = false;
        if (i < end && isASCIIDigit(characters[i])) {
            hasSomething = true;
            result.precision = 0;
        }
        while (i < end && isASCIIDigit(characters[i])) {
            if (!appendDigit(result.precision, characters[i++])) {
                raiseValueError(globalObject, scope, "Too many decimal digits in format string"_s);
                return std::nullopt;
            }
        }
        if (i < end && (characters[i] == ',' || characters[i] == '_')) {
            hasSomething = true;
            result.fractionGrouping = characters[i++];
            if (i < end && (characters[i] == ',' || characters[i] == '_') && characters[i] != static_cast<char32_t>(result.fractionGrouping)) {
                raiseValueError(globalObject, scope, "Cannot specify both ',' and '_'."_s);
                return std::nullopt;
            }
        }
        if (!hasSomething) {
            raiseValueError(globalObject, scope, "Format specifier missing precision"_s);
            return std::nullopt;
        }
    }
    if (end - i > 1)
        return invalid();
    if (i < end)
        result.type = characters[i];

    // What can be told to be wrong without knowing what is being formatted.
    auto cannotSpecify = [&] (char separator) -> std::optional<FormatSpecification> {
        StringBuilder message;
        message.append("Cannot specify '"_s, separator, "' with '"_s);
        if (result.type > 32 && result.type < 128)
            message.append(static_cast<char>(result.type));
        else
            message.append("\\x"_s, hex(static_cast<unsigned>(result.type), Lowercase));
        message.append("'."_s);
        raiseValueError(globalObject, scope, message.toString());
        return std::nullopt;
    };
    if (result.grouping) {
        switch (result.type) {
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
            if (result.grouping == '_')
                break;
            [[fallthrough]];
        default:
            return cannotSpecify(result.grouping);
        }
    }
    if (result.type == 'n' && result.fractionGrouping)
        return cannotSpecify(result.fractionGrouping);
    return result;
}

String raiseUnknownFormatCode(JSGlobalObject* globalObject, ThrowScope& scope, char32_t code, const String& typeName)
{
    StringBuilder builder;
    builder.append("Unknown format code '"_s);
    if (code > 32 && code < 128)
        builder.append(static_cast<char>(code));
    else
        builder.append("\\x"_s, hex(static_cast<unsigned>(code), Lowercase));
    builder.append("' for object of type '"_s, typeName, '\'');
    raiseValueError(globalObject, scope, builder.toString());
    return { };
}

static unsigned lengthInCharacters(const String& string)
{
    return stringHasSurrogatePairs(string) ? countCharacters(string) : string.length();
}

// `prefixLength` is how much at the front is a sign and 0x and the like, which '=' puts the padding after.
// Null, with MemoryError raised, if there is no room for it.
static String pad(JSGlobalObject* globalObject, const String& text, unsigned prefixLength, const FormatSpecification& specification, char defaultAlign)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    unsigned length = lengthInCharacters(text);
    if (length >= specification.width)
        return text;
    if (specification.width > static_cast<int64_t>(String::MaxLength)) {
        raiseMemoryError(globalObject, scope);
        return { };
    }
    unsigned padding = specification.width - length;
    char align = specification.align ? specification.align : defaultAlign;
    StringBuilder builder(OverflowPolicy::RecordOverflow);
    auto fill = [&] (unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            builder.append(specification.fill);
    };
    switch (align) {
    case '<':
        builder.append(text);
        fill(padding);
        break;
    case '>':
        fill(padding);
        builder.append(text);
        break;
    case '^':
        fill(padding / 2);
        builder.append(text);
        fill(padding - padding / 2);
        break;
    case '=':
        builder.append(StringView(text).left(prefixLength));
        fill(padding);
        builder.append(StringView(text).substring(prefixLength));
        break;
    }
    if (builder.hasOverflowed()) {
        raiseMemoryError(globalObject, scope);
        return { };
    }
    return builder.toString();
}

// 1234567 to 1,234,567. With zeros for padding, they are grouped too, so that is done here: `minimumWidth` is what the digits and
// separators should come to.
static String groupDigits(StringView digits, char separator, unsigned groupSize, unsigned minimumWidth)
{
    Vector<char16_t, 64> reversed;
    unsigned count = 0;
    auto push = [&] (char16_t digit) {
        if (count && !(count % groupSize))
            reversed.append(separator);
        reversed.append(digit);
        ++count;
    };
    for (unsigned i = digits.length(); i--;)
        push(digits[i]);
    while (reversed.size() < minimumWidth) {
        // Not a separator at the front.
        if (!(count % groupSize) && reversed.size() + 1 == minimumWidth) {
            push('0');
            break;
        }
        push('0');
    }
    reversed.reverse();
    return String(reversed.span());
}

String formatString(JSGlobalObject* globalObject, const String& value, const FormatSpecification& specification)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (specification.type && specification.type != 's')
        return raiseUnknownFormatCode(globalObject, scope, specification.type, specification.typeName);
    if (specification.hasSign) {
        raiseValueError(globalObject, scope, specification.sign == ' ' ? "Space not allowed in string format specifier"_s : "Sign not allowed in string format specifier"_s);
        return { };
    }
    if (specification.noNegativeZero) {
        raiseValueError(globalObject, scope, "Negative zero coercion (z) not allowed in string format specifier"_s);
        return { };
    }
    if (specification.alternate) {
        raiseValueError(globalObject, scope, "Alternate form (#) not allowed in string format specifier"_s);
        return { };
    }
    if (specification.align == '=') {
        raiseValueError(globalObject, scope, "'=' alignment not allowed in string format specifier"_s);
        return { };
    }
    if (specification.grouping) {
        raiseValueError(globalObject, scope, makeString("Cannot specify '"_s, specification.grouping, "' with 's'."_s));
        return { };
    }
    String text = value;
    if (specification.precision >= 0 && specification.precision < static_cast<int64_t>(lengthInCharacters(text)))
        text = StringView(text).left(stringHasSurrogatePairs(text) ? stringOffsetOfCharacter(text, specification.precision) : specification.precision).toString();
    RELEASE_AND_RETURN(scope, pad(globalObject, text, 0, specification, '<'));
}

static void appendSign(StringBuilder& builder, bool isNegative, char sign)
{
    if (isNegative)
        builder.append('-');
    else if (sign != '-')
        builder.append(sign);
}

String formatInt(JSGlobalObject* globalObject, JSValue value, const FormatSpecification& specification)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Number number = classify(value);
    ASSERT(number.isInt());
    unsigned radix = 10;
    ASCIILiteral prefix = ""_s;
    switch (specification.type) {
    case 0:
    case 'd':
    case 'n':
        break;
    case 'b':
        radix = 2;
        prefix = "0b"_s;
        break;
    case 'o':
        radix = 8;
        prefix = "0o"_s;
        break;
    case 'x':
        radix = 16;
        prefix = "0x"_s;
        break;
    case 'X':
        radix = 16;
        prefix = "0X"_s;
        break;
    case 'c': {
        if (specification.precision >= 0) {
            raiseValueError(globalObject, scope, "Precision not allowed in integer format specifier"_s);
            return { };
        }
        if (specification.noNegativeZero) {
            raiseValueError(globalObject, scope, "Negative zero coercion (z) not allowed in integer format specifier"_s);
            return { };
        }
        if (specification.hasSign) {
            raiseValueError(globalObject, scope, "Sign not allowed with integer format specifier 'c'"_s);
            return { };
        }
        if (specification.alternate) {
            raiseValueError(globalObject, scope, "Alternate form (#) not allowed with integer format specifier 'c'"_s);
            return { };
        }
        if (number.kind == Number::Kind::Big && number.big->length() > 1) {
            raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C long"_s);
            return { };
        }
        if (number.kind != Number::Kind::Small || number.small < 0 || number.small > 0x10FFFF) {
            raise(globalObject, scope, BuiltinType::OverflowError, "%c arg not in range(0x110000)"_s);
            return { };
        }
        StringBuilder builder;
        builder.append(static_cast<char32_t>(number.small));
        RELEASE_AND_RETURN(scope, pad(globalObject, builder.toString(), 0, specification, '>'));
    }
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case '%': {
        double real = toDouble(globalObject, scope, number);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, formatFloat(globalObject, real, specification));
    }
    default:
        return raiseUnknownFormatCode(globalObject, scope, specification.type, specification.typeName);
    }
    if (specification.precision >= 0) {
        raiseValueError(globalObject, scope, "Precision not allowed in integer format specifier"_s);
        return { };
    }
    if (specification.noNegativeZero) {
        raiseValueError(globalObject, scope, "Negative zero coercion (z) not allowed in integer format specifier"_s);
        return { };
    }
    if (specification.grouping == ',' && radix != 10) {
        StringBuilder message;
        message.append("Cannot specify ',' with '"_s);
        message.append(specification.type);
        message.append("'."_s);
        raiseValueError(globalObject, scope, message.toString());
        return { };
    }

    String digits = reprOfInt(globalObject, number, radix);
    RETURN_IF_EXCEPTION(scope, { });
    bool isNegative = digits.startsWith('-');
    if (isNegative)
        digits = digits.substring(1);
    if (specification.type == 'X')
        digits = digits.convertToASCIIUppercase();

    StringBuilder builder;
    appendSign(builder, isNegative, specification.sign);
    if (specification.alternate)
        builder.append(prefix);
    unsigned prefixLength = builder.length();
    if (specification.grouping) {
        bool padsWithZeros = specification.fill == '0' && specification.align == '=';
        if (padsWithZeros && specification.width > static_cast<int64_t>(String::MaxLength)) {
            raiseMemoryError(globalObject, scope);
            return { };
        }
        unsigned minimum = padsWithZeros && specification.width > prefixLength ? specification.width - prefixLength : 0;
        digits = groupDigits(digits, specification.grouping, radix == 10 ? 3 : 4, minimum);
    }
    builder.append(digits);
    RELEASE_AND_RETURN(scope, pad(globalObject, builder.toString(), prefixLength, specification, '>'));
}

String formatFloat(JSGlobalObject* globalObject, double value, const FormatSpecification& specification)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t type = specification.type;
    if (specification.precision > std::numeric_limits<int>::max()) {
        raiseValueError(globalObject, scope, "precision too big"_s);
        return { };
    }
    int precision = specification.precision;

    switch (type) {
    case 0:
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'n':
    case '%':
        break;
    default:
        return raiseUnknownFormatCode(globalObject, scope, type, specification.typeName);
    }

    bool isUpper = type == 'E' || type == 'F' || type == 'G';
    bool isPercent = type == '%';
    if (isPercent) {
        value *= 100;
        type = 'f';
    }
    bool isNegative = std::signbit(value);

    StringBuilder body;
    if (!std::isfinite(value)) {
        if (std::isnan(value)) {
            body.append(isUpper ? "NAN"_s : "nan"_s);
            isNegative = false;
        } else
            body.append(isUpper ? "INF"_s : "inf"_s);
    } else if (!type && precision < 0) {
        // As repr() has it.
        String text = reprOfDouble(std::abs(value));
        // '#' is for there to be a point, come what may.
        if (size_t exponentAt = text.find('e'); specification.alternate && exponentAt != notFound && !text.contains('.'))
            text = makeString(StringView(text).left(exponentAt), '.', StringView(text).substring(exponentAt));
        body.append(text);
    } else {
        bool addPointZero = false;
        if (!type) {
            // Like 'g', but with something after the point, and it takes to exponents one digit later.
            type = 'g';
            addPointZero = true;
        }
        if (precision < 0)
            precision = 6;
        double magnitude = std::abs(value);
        char exponentCharacter = isUpper ? 'E' : 'e';
        switch (type) {
        case 'f':
        case 'F':
            appendFixed(body, fixedPointDigits(magnitude, precision), precision, specification.alternate);
            break;
        case 'e':
        case 'E':
            appendExponential(body, significantDigits(magnitude, precision + 1), precision, specification.alternate, exponentCharacter, !magnitude);
            break;
        default: {
            int significant = precision ? precision : 1;
            Digits digits = significantDigits(magnitude, significant);
            int exponent = magnitude ? digits.point - 1 : 0;
            bool usesExponent = exponent < -4 || exponent >= (addPointZero ? significant - 1 : significant);
            // Zeros at the end are dropped, unless '#' says otherwise.
            int kept = specification.alternate ? significant : std::max<int>(digits.digits.size(), 1);
            if (usesExponent)
                appendExponential(body, digits, kept - 1, specification.alternate, exponentCharacter, !magnitude);
            else {
                int places = std::max(kept - (exponent + 1), 0);
                appendFixed(body, digits, places, specification.alternate);
                if (addPointZero && !places)
                    body.append(specification.alternate ? "0"_s : ".0"_s);
            }
            break;
        }
        }
    }

    String text = body.toString();
    if (specification.noNegativeZero && isNegative) {
        bool isAllZeros = true;
        for (unsigned i = 0; i < text.length() && text[i] != 'e' && text[i] != 'E'; ++i)
            isAllZeros &= text[i] == '0' || text[i] == '.';
        if (isAllZeros)
            isNegative = false;
    }

    if (specification.fractionGrouping && std::isfinite(value)) {
        // In threes from the point.
        if (size_t point = text.find('.'); point != notFound) {
            StringBuilder grouped;
            grouped.append(StringView(text).left(point + 1));
            unsigned i = point + 1;
            for (unsigned count = 0; i < text.length() && isASCIIDigit(text[i]); ++i, ++count) {
                if (count && !(count % 3))
                    grouped.append(specification.fractionGrouping);
                grouped.append(text[i]);
            }
            grouped.append(StringView(text).substring(i));
            text = grouped.toString();
        }
    }

    StringBuilder builder;
    appendSign(builder, isNegative, specification.sign);
    unsigned prefixLength = builder.length();
    if (specification.grouping && std::isfinite(value)) {
        // Only what is before the point.
        unsigned whole = 0;
        while (whole < text.length() && isASCIIDigit(text[whole]))
            ++whole;
        unsigned restLength = text.length() - whole + isPercent;
        bool padsWithZeros = specification.fill == '0' && specification.align == '=';
        if (padsWithZeros && specification.width > static_cast<int64_t>(String::MaxLength)) {
            raiseMemoryError(globalObject, scope);
            return { };
        }
        unsigned minimum = padsWithZeros && specification.width > prefixLength + restLength ? specification.width - prefixLength - restLength : 0;
        builder.append(groupDigits(StringView(text).left(whole), specification.grouping, 3, minimum), StringView(text).substring(whole));
    } else
        builder.append(text);
    if (isPercent)
        builder.append('%');
    RELEASE_AND_RETURN(scope, pad(globalObject, builder.toString(), prefixLength, specification, '>'));
}

// ---- format % values

String escapeNonASCII(const String& text)
{
    if (text.containsOnlyASCII())
        return text;
    StringBuilder builder;
    for (char32_t c : StringView(text).codePoints()) {
        if (c < 0x80)
            builder.append(static_cast<Latin1Character>(c));
        else if (c <= 0xFF)
            builder.append("\\x"_s, hex(static_cast<unsigned>(c), 2, Lowercase));
        else if (c <= 0xFFFF)
            builder.append("\\u"_s, hex(static_cast<unsigned>(c), 4, Lowercase));
        else
            builder.append("\\U"_s, hex(static_cast<unsigned>(c), 8, Lowercase));
    }
    return builder.toString();
}

// For bytes, the format and the result have a character for each byte.
//
// CPython writes what it makes piece by piece, and if the first piece that has anything in it is also the last, and is a str that it had already, it gives that str and not another like it. It shows if the
// str is of a class derived from str. `only` is given that str.
static String percentFormat(JSGlobalObject* globalObject, const String& format, JSValue values, bool isForBytes, JSValue* only = nullptr)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto raiseTypeError = [] (JSGlobalObject* globalObject, ThrowScope& scope, const String& message) { Python::raiseTypeError(globalObject, scope, message); return String(); };
    auto raiseValueError = [] (JSGlobalObject* globalObject, ThrowScope& scope, const String& message) { Python::raiseValueError(globalObject, scope, message); return String(); };

    // A tuple is the arguments. Anything else is the one argument, and a mapping is where %(name)s looks.
    PyTuple* tuple = isTuple(values) ? uncheckedDowncast<PyTuple>(values.asCell()) : nullptr;
    unsigned argumentCount = tuple ? tuple->length() : 1;
    unsigned nextArgument = 0;
    // Whatever can be subscripted, but for what is plainly the one argument. Of that it is not asked whether it was used.
    bool isMapping = !tuple && !stringIn(values) && !(isForBytes && bytesKindOf(values) != BytesKind::None) && typeOf(globalObject, values)->lookup(vm, vm.pythonNames().dunder_getitem);
    auto takeArgument = [&] () -> JSValue {
        if (nextArgument >= argumentCount) {
            raiseTypeError(globalObject, scope, "not enough arguments for format string"_s);
            return { };
        }
        JSValue result = tuple ? tuple->at(nextArgument) : values;
        ++nextArgument;
        return result;
    };

    StringBuilder result;
    unsigned length = format.length();
    for (unsigned i = 0; i < length;) {
        char16_t c = format[i++];
        if (c != '%') {
            result.append(c);
            continue;
        }
        if (i >= length)
            return raiseValueError(globalObject, scope, "incomplete format"_s);

        JSValue argument;
        if (format[i] == '(') {
            unsigned depth = 1;
            unsigned start = ++i;
            while (i < length && depth) {
                if (format[i] == '(')
                    ++depth;
                else if (format[i] == ')')
                    --depth;
                ++i;
            }
            if (depth)
                return raiseValueError(globalObject, scope, "incomplete format key"_s);
            if (!isMapping)
                return raiseTypeError(globalObject, scope, "format requires a mapping"_s);
            String key = format.substring(start, i - 1 - start);
            argument = getItem(globalObject, values, isForBytes ? JSValue(newBytes(globalObject, key.span8())) : JSValue(jsString(vm, key)));
            RETURN_IF_EXCEPTION(scope, { });
        }

        FormatSpecification specification;
        bool leftAlign = false;
        bool zeroPad = false;
        for (; i < length; ++i) {
            char16_t flag = format[i];
            if (flag == '-')
                leftAlign = true;
            else if (flag == '+')
                specification.sign = '+';
            else if (flag == ' ') {
                if (specification.sign != '+')
                    specification.sign = ' ';
            } else if (flag == '#')
                specification.alternate = true;
            else if (flag == '0')
                zeroPad = true;
            else
                break;
        }
        // How wide is a Py_ssize_t in CPython, and how much of it an int.
        auto readNumber = [&] (int64_t& target, bool isWidth) -> bool {
            if (i < length && format[i] == '*') {
                ++i;
                JSValue star = takeArgument();
                RETURN_IF_EXCEPTION(scope, false);
                if (!classify(star).isInt()) {
                    raiseTypeError(globalObject, scope, "* wants int"_s);
                    return false;
                }
                if (isWidth) {
                    auto given = toSsize(globalObject, star);
                    RETURN_IF_EXCEPTION(scope, false);
                    target = *given;
                } else {
                    auto given = toCInt(globalObject, star);
                    RETURN_IF_EXCEPTION(scope, false);
                    target = std::max(*given, 0);
                }
                return true;
            }
            if (i < length && isASCIIDigit(format[i])) {
                target = 0;
                while (i < length && isASCIIDigit(format[i])) {
                    if (!appendDigit(target, format[i++], isWidth ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int>::max())) {
                        raiseValueError(globalObject, scope, isWidth ? "width too big"_s : "precision too big"_s);
                        return false;
                    }
                }
            }
            return true;
        };
        int64_t width = -1;
        readNumber(width, true);
        RETURN_IF_EXCEPTION(scope, { });
        if (width < -1) {
            leftAlign = true;
            // The least of all has nothing to be the opposite of. There is no room for either.
            width = width == std::numeric_limits<int64_t>::min() ? std::numeric_limits<int64_t>::max() : -width;
        }
        if (i < length && format[i] == '.') {
            ++i;
            specification.precision = 0;
            readNumber(specification.precision, false);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (i < length && (format[i] == 'l' || format[i] == 'h' || format[i] == 'L'))
            ++i;
        if (i >= length)
            return raiseValueError(globalObject, scope, "incomplete format"_s);
        char16_t conversion = format[i++];
        if (conversion == '%') {
            result.append('%');
            continue;
        }
        if (!argument) {
            argument = takeArgument();
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (width > 0) {
            specification.hasWidth = true;
            specification.width = width;
        }
        specification.align = leftAlign ? '<' : '>';

        String piece;
        switch (conversion) {
        case 'b':
            if (!isForBytes)
                return raiseValueError(globalObject, scope, makeString("unsupported format character 'b' (0x62) at index "_s, i - 1));
            [[fallthrough]];
        case 's':
        case 'r':
        case 'a': {
            String text;
            JSValue object;
            if (conversion == 'r' || conversion == 'a') {
                object = reprObject(globalObject, argument);
                RETURN_IF_EXCEPTION(scope, { });
                text = stringIn(object)->value(globalObject);
                // PyObject_ASCII(): what is ASCII already is left as it is.
                if ((conversion == 'a' || isForBytes) && !text.containsOnlyASCII()) {
                    text = escapeNonASCII(text);
                    object = { };
                }
            } else if (!isForBytes) {
                object = strObject(globalObject, argument);
                RETURN_IF_EXCEPTION(scope, { });
                text = stringIn(object)->value(globalObject);
            } else {
                // format_obj(): a bytes or a bytearray, then what has __bytes__(), and then whatever else has bytes to show.
                Buffer buffer;
                if (bytesKindOf(argument) != BytesKind::None)
                    buffer = tryBufferOf(globalObject, argument, FullReadOnlyBuffer);
                else {
                    JSValue self;
                    JSValue method = lookupSpecial(globalObject, argument, vm.pythonNames().dunder_bytes, self);
                    RETURN_IF_EXCEPTION(scope, { });
                    if (method) {
                        JSValue converted = callMethod(globalObject, method, self);
                        RETURN_IF_EXCEPTION(scope, { });
                        if (bytesKindOf(converted) != BytesKind::Bytes)
                            return raiseTypeError(globalObject, scope, makeString("__bytes__ returned non-bytes (type "_s, typeName(globalObject, converted), ')'));
                        buffer = tryBufferOf(globalObject, converted);
                    } else
                        buffer = tryBufferOf(globalObject, argument, FullReadOnlyBuffer);
                }
                RETURN_IF_EXCEPTION(scope, { });
                if (!buffer)
                    return raiseTypeError(globalObject, scope, makeString("%b requires a bytes-like object, or an object that implements __bytes__, not '"_s, typeName(globalObject, argument), '\''));
                ByteVector all;
                buffer.appendTo(all);
                text = String(byteCast<Latin1Character>(all.span()));
            }
            RETURN_IF_EXCEPTION(scope, { });
            // With a '+' or a ' ', which mean nothing here, CPython does not go the quick way.
            bool hadSignFlag = specification.sign == '+' || specification.sign == ' ';
            specification.sign = '-';
            specification.alternate = false;
            piece = formatString(globalObject, text, specification);
            if (only && object && result.isEmpty() && i == length && piece == text && !text.isEmpty() && !hadSignFlag)
                *only = object;
            break;
        }
        case 'c': {
            String text;
            if (isForBytes) {
                if (auto buffer = bytesKindOf(argument) == BytesKind::None ? std::nullopt : builtinBufferOf(argument)) {
                    if (buffer->size() != 1)
                        return raiseTypeError(globalObject, scope, makeString("%c requires an integer in range(256) or a single byte, not a "_s, typeName(globalObject, argument), " object of length "_s, buffer->size()));
                    text = String(byteCast<Latin1Character>(*buffer));
                } else {
                    Number number = classify(argument);
                    if (!number && typeOf(globalObject, argument)->lookup(vm, vm.pythonNames().dunder_index)) {
                        auto index = toIndex(globalObject, argument, true);
                        RETURN_IF_EXCEPTION(scope, { });
                        argument = intFromInt64(globalObject, *index);
                        number = classify(argument);
                    }
                    if (!number.isInt())
                        return raiseTypeError(globalObject, scope, makeString("%c requires an integer in range(256) or a single byte, not "_s, typeName(globalObject, argument)));
                    if (number.kind != Number::Kind::Small || number.small < 0 || number.small > 255) {
                        raise(globalObject, scope, BuiltinType::OverflowError, "%c arg not in range(256)"_s);
                        return { };
                    }
                    Latin1Character byte = number.small;
                    text = String(std::span<const Latin1Character>(&byte, 1));
                }
            } else if (argument.isString()) {
                text = asString(argument)->value(globalObject);
                if (lengthInCharacters(text) != 1)
                    return raiseTypeError(globalObject, scope, makeString("%c requires an int or a unicode character, not a string of length "_s, lengthInCharacters(text)));
            } else {
                Number number = classify(argument);
                JSValue given = argument;
                if (!number && typeOf(globalObject, argument)->lookup(vm, vm.pythonNames().dunder_index)) {
                    // formatchar(): a TypeError from __index__() is put as one from here.
                    auto index = toIndex(globalObject, argument, true);
                    if (scope.exception()) {
                        if (!catchException(globalObject, BuiltinType::TypeError))
                            return { };
                    } else {
                        argument = intFromInt64(globalObject, *index);
                        number = classify(argument);
                    }
                }
                if (!number.isInt())
                    return raiseTypeError(globalObject, scope, makeString("%c requires an int or a unicode character, not "_s, typeName(globalObject, given)));
                if (number.kind != Number::Kind::Small || number.small < 0 || number.small > 0x10FFFF) {
                    raise(globalObject, scope, BuiltinType::OverflowError, "%c arg not in range(0x110000)"_s);
                    return { };
                }
                StringBuilder builder;
                builder.append(static_cast<char32_t>(number.small));
                text = builder.toString();
            }
            specification.sign = '-';
            specification.precision = -1;
            piece = formatString(globalObject, text, specification);
            break;
        }
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            Number number = classify(argument);
            if (number.kind == Number::Kind::Float && (conversion == 'd' || conversion == 'i' || conversion == 'u')) {
                if (std::isnan(number.real))
                    return raiseValueError(globalObject, scope, "cannot convert float NaN to integer"_s);
                if (std::isinf(number.real)) {
                    raise(globalObject, scope, BuiltinType::OverflowError, "cannot convert float infinity to integer"_s);
                    return { };
                }
                argument = intFromDouble(globalObject, number.real);
                number = classify(argument);
            }
            JSValue given = argument;
            if (!number && argument.isObject()) {
                // mainformatlong(): as int() would for a decimal, and only by __index__() otherwise. If that raises TypeError, what is said is that it is of the wrong type.
                PyType* type = typeOf(globalObject, argument);
                auto& names = vm.pythonNames();
                // PyNumber_Check()
                if (type->lookup(vm, names.dunder_index) || type->lookup(vm, names.dunder_int) || type->lookup(vm, names.dunder_float) || type->isSubtypeOf(globalObject->pyRealm()->typeComplex())) {
                    bool isDecimal = conversion == 'd' || conversion == 'i' || conversion == 'u';
                    argument = isDecimal ? numberLong(globalObject, argument) : toInt(globalObject, argument);
                    if (scope.exception()) {
                        if (!catchException(globalObject, BuiltinType::TypeError))
                            return { };
                        argument = given;
                    }
                    number = classify(argument);
                }
            }
            if (!number.isInt()) {
                if (conversion == 'd' || conversion == 'i' || conversion == 'u')
                    return raiseTypeError(globalObject, scope, makeString('%', conversion, " format: a real number is required, not "_s, typeName(globalObject, given)));
                return raiseTypeError(globalObject, scope, makeString('%', conversion, " format: an integer is required, not "_s, typeName(globalObject, given)));
            }
            specification.type = conversion == 'i' || conversion == 'u' ? 'd' : conversion;
            // The precision is the least number of digits.
            int64_t minimumDigits = std::exchange(specification.precision, -1);
            FormatSpecification bare = specification;
            bare.width = 0;
            bare.hasWidth = false;
            String text = formatInt(globalObject, argument, bare);
            RETURN_IF_EXCEPTION(scope, { });
            unsigned prefixLength = 0;
            while (prefixLength < text.length() && !isASCIIAlphanumeric(text[prefixLength]))
                ++prefixLength;
            if (specification.alternate && conversion != 'd' && conversion != 'i' && conversion != 'u')
                prefixLength += 2;
            if (minimumDigits > 0 && text.length() - prefixLength < static_cast<unsigned>(minimumDigits)) {
                StringBuilder builder(OverflowPolicy::RecordOverflow);
                builder.append(StringView(text).left(prefixLength));
                for (unsigned k = text.length() - prefixLength; k < static_cast<unsigned>(minimumDigits); ++k)
                    builder.append('0');
                builder.append(StringView(text).substring(prefixLength));
                if (builder.hasOverflowed()) {
                    raiseMemoryError(globalObject, scope);
                    return { };
                }
                text = builder.toString();
            }
            if (zeroPad && !leftAlign) {
                specification.fill = '0';
                specification.align = '=';
            }
            piece = pad(globalObject, text, prefixLength, specification, '>');
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G': {
            if (!classify(argument) && !typeOf(globalObject, argument)->lookup(vm, vm.pythonNames().dunder_float) && !typeOf(globalObject, argument)->lookup(vm, vm.pythonNames().dunder_index))
                return raiseTypeError(globalObject, scope, isForBytes ? makeString("float argument required, not "_s, typeName(globalObject, argument)) : makeString("must be real number, not "_s, typeName(globalObject, argument)));
            auto real = toDouble(globalObject, argument);
            RETURN_IF_EXCEPTION(scope, { });
            specification.type = conversion;
            if (zeroPad && !leftAlign) {
                specification.fill = '0';
                specification.align = '=';
            }
            piece = formatFloat(globalObject, *real, specification);
            break;
        }
        default:
            return raiseValueError(globalObject, scope, makeString("unsupported format character '"_s, conversion, "' (0x"_s, hex(static_cast<unsigned>(conversion), Lowercase), ") at index "_s, i - 1));
        }
        RETURN_IF_EXCEPTION(scope, { });
        result.append(piece);
    }

    if (nextArgument < argumentCount && !isMapping)
        return raiseTypeError(globalObject, scope, isForBytes ? "not all arguments converted during bytes formatting"_s : "not all arguments converted during string formatting"_s);
    String text = result.toString();
    return text.isNull() ? emptyString() : text;
}

JSValue stringPercentFormat(JSGlobalObject* globalObject, JSValue format, JSValue values)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = stringIn(format)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue only;
    String result = percentFormat(globalObject, text, values, false, &only);
    RETURN_IF_EXCEPTION(scope, { });
    if (only)
        return only;
    // And so it is with the format itself, if there is nothing in it to fill in.
    if (!text.contains('%'))
        return format;
    return jsString(vm, result);
}

String bytesPercentFormat(JSGlobalObject* globalObject, std::span<const uint8_t> format, JSValue values)
{
    return percentFormat(globalObject, String(byteCast<Latin1Character>(format)), values, true);
}

} } // namespace JSC::Python
