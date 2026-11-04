/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef PY_DECIMAL_STANDALONE
#include "config.h"
#endif
#include "PyDecimalLimbs.h"

#include <algorithm>
#include <climits>
#include <clocale>
#include <cstdlib>
#include <limits>

#ifdef PY_DECIMAL_STANDALONE
#define WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#define WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#else
#include <wtf/FastMalloc.h>
#include <wtf/RAMSize.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Decimal {

// Numbers as text, and as integers in base 2**64.

static PY_DECIMAL_INLINE bool isDigit(char c) { return c >= '0' && c <= '9'; }

void Text::release()
{
#ifdef PY_DECIMAL_STANDALONE
    std::free(data);
#else
    fastFree(data);
#endif
}

bool Text::allocateSlow(size_t capacity)
{
    char* memory = m_inline;
    if (capacity > sizeof(m_inline)) {
#ifdef PY_DECIMAL_STANDALONE
        if (capacity > std::numeric_limits<size_t>::max() / 4)
            return false;
        memory = static_cast<char*>(std::malloc(capacity));
        if (!memory)
            return false;
#else
        if (capacity > WTF::ramSize())
            return false;
        void* allocated = nullptr;
        if (!tryFastMalloc(capacity).getValue(allocated))
            return false;
        memory = static_cast<char*>(allocated);
#endif
    }
    if (data != m_inline)
        release();
    data = memory;
    return true;
}

void Text::swap(Text& other)
{
    bool isInline = data == m_inline;
    bool otherIsInline = other.data == other.m_inline;
    char* mine = data;
    data = otherIsInline ? m_inline : other.data;
    other.data = isInline ? other.m_inline : mine;
    if (isInline || otherIsInline)
        std::swap(m_inline, other.m_inline);
    std::swap(length, other.length);
}

// ---- From text ----

// Whether the text starts with the word, in whatever case. The word is in lower case.
static bool startsWithWord(const char* text, const char* word)
{
    for (; *word; ++text, ++word) {
        if ((*text | 0x20) != *word || !((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z')))
            return false;
    }
    return true;
}

// The coefficient from `digits` digit characters that end before `end`. A point among them is skipped.
static void setCoefficientFromText(Number& number, const char* end, int64_t digits)
{
    int64_t size = limbsForDigits(digits);
    Limb* limbs = number.limbs();
    const char* p = end;
    int64_t left = digits;
    for (int64_t i = 0; i < size; ++i) {
        int count = static_cast<int>(std::min<int64_t>(left, digitsPerLimb));
        Limb limb = 0;
        for (int k = 0; k < count; ++k) {
            --p;
            if (*p == '.')
                --p;
            limb += static_cast<Limb>(*p - '0') * powersOfTen[k];
        }
        limbs[i] = limb;
        left -= count;
    }
    number.length = size;
    number.updateDigits();
}

void fromString(Number& result, const char* text, size_t length, const Context& context, uint32_t& status)
{
    const char* s = text;
    const char* end = text + length;
    uint8_t sign = 0;
    if (*s == '+')
        ++s;
    else if (*s == '-') {
        sign = Number::Negative;
        ++s;
    }

    uint8_t nanKind = 0;
    if (startsWithWord(s, "nan")) {
        nanKind = Number::QuietNaN;
        s += 3;
    } else if (startsWithWord(s, "snan")) {
        nanKind = Number::SignalingNaN;
        s += 4;
    }
    if (nanKind) {
        // What follows is the payload: digits, of which zeros at the front do not count.
        while (*s == '0')
            ++s;
        const char* payload = s;
        while (isDigit(*s))
            ++s;
        if (s != end)
            return result.setError(ConversionSyntax, status);
        result.setSpecial(sign, nanKind);
        int64_t digits = s - payload;
        if (!digits)
            return;
        if (digits > context.precision - context.clamp)
            return result.setError(ConversionSyntax, status);
        if (!result.reserveDiscarding(limbsForDigits(digits), status))
            return;
        setCoefficientFromText(result, s, digits);
        return;
    }
    if (startsWithWord(s, "inf")) {
        s += 3;
        if (s != end && !(startsWithWord(s, "inity") && s + 5 == end))
            return result.setError(ConversionSyntax, status);
        result.setSpecial(sign, Number::Infinite);
        return;
    }

    // digits [. digits] [e [sign] digits], with a digit somewhere before the e.
    const char* first = s;
    while (isDigit(*s))
        ++s;
    const char* point = nullptr;
    if (*s == '.') {
        point = s++;
        while (isDigit(*s))
            ++s;
    }
    const char* coefficientEnd = s;
    int64_t fractionDigits = point ? coefficientEnd - point - 1 : 0;
    int64_t digits = (coefficientEnd - first) - (point ? 1 : 0);
    if (!digits)
        return result.setError(ConversionSyntax, status);

    int64_t exponent = 0;
    if (*s == 'e' || *s == 'E') {
        ++s;
        bool isNegative = *s == '-';
        if (*s == '+' || *s == '-')
            ++s;
        if (!isDigit(*s))
            return result.setError(ConversionSyntax, status);
        // What does not fit is as large as can be.
        constexpr int64_t limit = std::numeric_limits<int64_t>::max();
        bool overflows = false;
        for (; isDigit(*s); ++s) {
            int digit = *s - '0';
            if (exponent > (limit - digit) / 10)
                overflows = true;
            else
                exponent = exponent * 10 + digit;
        }
        if (overflows)
            exponent = isNegative ? std::numeric_limits<int64_t>::min() : limit;
        else if (isNegative)
            exponent = -exponent;
    }
    if (s != end)
        return result.setError(ConversionSyntax, status);

    // Zeros at the front do not count, but for the last digit.
    while (digits > 1 && (*first == '0' || *first == '.')) {
        if (*first == '0')
            --digits;
        ++first;
    }
    if (fractionDigits > maxPrecision || digits > maxPrecision)
        return result.setError(ConversionSyntax, status);
    if (exponent < std::numeric_limits<int64_t>::min() + fractionDigits)
        exponent = std::numeric_limits<int64_t>::min();
    else
        exponent -= fractionDigits;
    exponent = std::min(exponent, overflowingExponent);
    if (exponent == std::numeric_limits<int64_t>::min())
        ++exponent;

    if (!result.reserveDiscarding(limbsForDigits(digits), status))
        return;
    result.flags = sign;
    result.exponent = exponent;
    setCoefficientFromText(result, coefficientEnd, digits);
    finalize(result, context, status);
}

// ---- To text ----

// `count` digits, with zeros at the front if the value has fewer.
static PY_DECIMAL_INLINE char* writeDigits(char* p, Limb value, int count)
{
    for (int i = count - 1; i >= 0; --i) {
        p[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    return p + count;
}

static char* writeCoefficient(char* p, const Number& number)
{
    p = writeDigits(p, number.top(), digitCount(number.top()));
    for (int64_t i = number.length - 2; i >= 0; --i)
        p = writeDigits(p, number.limbs()[i], digitsPerLimb);
    return p;
}

static PY_DECIMAL_INLINE char* writeLiteral(char* p, const char* literal)
{
    while (*literal)
        *p++ = *literal++;
    return p;
}

static PY_DECIMAL_INLINE int64_t floorModulo(int64_t value, int64_t modulus)
{
    int64_t rest = value % modulus;
    return rest < 0 ? rest + modulus : rest;
}

bool toText(Text& text, const Number& number, unsigned flags, int64_t pointPlace)
{
    char* p;
    auto writeSign = [&] {
        if (number.isNegative())
            *p++ = '-';
        else if (flags & TextSpaceForPlus)
            *p++ = ' ';
        else if (flags & TextPlus)
            *p++ = '+';
    };

    if (number.isSpecial()) {
        if (!text.allocate(static_cast<size_t>(16 + number.digits)))
            return false;
        p = text.data;
        writeSign();
        if (number.isNaN()) {
            p = writeLiteral(p, number.isQuietNaN() ? "NaN" : "sNaN");
            if (number.length)
                p = writeCoefficient(p, number);
        } else
            p = writeLiteral(p, "Infinity");
    } else {
        // How many digits there are before the point when there is no exponent. pointPlace is where the point goes, counted from the
        // first digit of the coefficient; the exponent that is printed makes up for the difference.
        int64_t integerDigits = number.digits + number.exponent;
        if (flags & TextExponent) {
        } else if ((flags & TextFixed) || (number.exponent <= 0 && integerDigits > -6))
            pointPlace = integerDigits;
        else if (flags & TextEngineering) {
            // The exponent is to be a multiple of three.
            if (number.hasZeroCoefficient())
                pointPlace = -1 + floorModulo(number.exponent + 2, 3);
            else
                pointPlace += floorModulo(integerDigits - 1, 3);
        }

        int64_t size;
        if (pointPlace <= 0)
            size = -pointPlace + number.digits + 2;
        else
            size = std::max(pointPlace, number.digits);
        if (!text.allocate(static_cast<size_t>(size) + maxExponentDigits + 8))
            return false;
        p = text.data;
        writeSign();
        if (pointPlace <= 0) {
            *p++ = '0';
            *p++ = '.';
            std::memset(p, '0', static_cast<size_t>(-pointPlace));
            p = writeCoefficient(p - pointPlace, number);
        } else if (pointPlace >= number.digits) {
            p = writeCoefficient(p, number);
            std::memset(p, '0', static_cast<size_t>(pointPlace - number.digits));
            p += pointPlace - number.digits;
        } else {
            char* start = p;
            p = writeCoefficient(p, number);
            std::memmove(start + pointPlace + 1, start + pointPlace, static_cast<size_t>(number.digits - pointPlace));
            start[pointPlace] = '.';
            ++p;
        }
        if (integerDigits != pointPlace || (flags & TextExponent)) {
            *p++ = flags & TextUpperCase ? 'E' : 'e';
            int64_t exponent = integerDigits - pointPlace;
            *p++ = exponent < 0 ? '-' : '+';
            Limb magnitude = exponent < 0 ? -static_cast<uint64_t>(exponent) : static_cast<uint64_t>(exponent);
            p = writeDigits(p, magnitude, digitCount(magnitude));
        }
    }
    if (flags & TextPercent)
        *p++ = '%';
    *p = 0;
    text.length = static_cast<size_t>(p - text.data);
    return true;
}

// ---- format() ----

// The length of the character in UTF-8 that the text starts with. 0: the text is empty. -1: it is not well formed.
static int lengthOfFirstCharacter(const char* text)
{
    auto* p = reinterpret_cast<const unsigned char*>(text);
    unsigned char lead = p[0];
    if (!lead)
        return 0;
    if (lead < 0x80)
        return 1;
    int count;
    unsigned char low = 0x80;
    unsigned char high = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf)
        count = 2;
    else if (lead >= 0xe0 && lead <= 0xef) {
        count = 3;
        if (lead == 0xe0)
            low = 0xa0;
        else if (lead == 0xed)
            high = 0x9f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        count = 4;
        if (lead == 0xf0)
            low = 0x90;
        else if (lead == 0xf4)
            high = 0x8f;
    } else
        return -1;
    if (p[1] < low || p[1] > high)
        return -1;
    for (int i = 2; i < count; ++i) {
        if (p[i] < 0x80 || p[i] > 0xbf)
            return -1;
    }
    return count;
}

static PY_DECIMAL_INLINE bool isAlignment(char c) { return c == '<' || c == '>' || c == '=' || c == '^'; }

// Digits that are there. False: the number does not fit.
static bool parseCount(const char*& p, int64_t& value)
{
    constexpr int64_t limit = std::numeric_limits<int64_t>::max();
    value = 0;
    for (; isDigit(*p); ++p) {
        int digit = *p - '0';
        if (value > (limit - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    return true;
}

bool isValidLocaleData(const FormatSpec& spec)
{
    for (const char* p = spec.grouping; *p; ++p) {
        if (*p < 0)
            return false;
    }
    size_t pointLength = std::strlen(spec.point);
    return pointLength && pointLength <= 4 && std::strlen(spec.separator) <= 4;
}

// [[fill]align][sign][0][width][,][.precision][type]
bool parseFormatSpec(FormatSpec& spec, const char* text, bool capitals, bool& isLocaleAware)
{
    const char* p = text;
    bool hasAlignment = false;
    isLocaleAware = false;
    spec = FormatSpec();
    spec.type = capitals ? 'G' : 'g';

    int fillLength = lengthOfFirstCharacter(p);
    if (fillLength < 0)
        return false;
    if (*p && isAlignment(p[fillLength])) {
        std::memcpy(spec.fill, p, static_cast<size_t>(fillLength));
        spec.fill[fillLength] = 0;
        p += fillLength;
        spec.align = *p++;
        hasAlignment = true;
    } else if (isAlignment(*p)) {
        spec.align = *p++;
        hasAlignment = true;
    }
    if (*p == '+' || *p == '-' || *p == ' ')
        spec.sign = *p++;
    if (*p == '0') {
        // Padding with zeros is an alignment too.
        if (hasAlignment)
            return false;
        spec.align = 'z';
        spec.fill[0] = *p++;
        spec.fill[1] = 0;
    }
    if (isDigit(*p)) {
        if (*p == '0' || !parseCount(p, spec.minWidth))
            return false;
    }
    if (*p == ',') {
        spec.point = ".";
        spec.separator = ",";
        spec.grouping = "\003\003";
        ++p;
    }
    if (*p == '.') {
        ++p;
        if (!isDigit(*p) || !parseCount(p, spec.precision))
            return false;
    }
    switch (*p) {
    case 'E':
    case 'e':
    case 'F':
    case 'f':
    case 'G':
    case 'g':
    case '%':
        spec.type = *p++;
        break;
    case 'N':
    case 'n': {
        if (*spec.separator)
            return false;
        spec.type = *p++ == 'N' ? 'G' : 'g';
        struct lconv* locale = localeconv();
        spec.point = locale->decimal_point;
        spec.separator = locale->thousands_sep;
        spec.grouping = locale->grouping;
        isLocaleAware = true;
        if (!isValidLocaleData(spec))
            return false;
        break;
    }
    default:
        break;
    }
    return !*p;
}

// To that many significant digits. If rounding up makes it one more, once again.
static void roundToDigits(Number& result, const Number& a, int64_t digits, const Context& context, uint32_t& status)
{
    int64_t exponent = a.exponent + a.digits - digits;
    rescale(result, a, exponent, context, status, true);
    if (result.digits > digits)
        rescale(result, result, exponent + 1, context, status, true);
}

// Writes from the end of a buffer towards the start, or only counts.
struct BackwardWriter {
    char* cursor { nullptr }; // Null: counting.
    int64_t bytes { 0 };
    int64_t characters { 0 };

    void ascii(const char* source, int64_t count)
    {
        bytes += count;
        characters += count;
        if (cursor) {
            cursor -= count;
            std::memcpy(cursor, source, static_cast<size_t>(count));
        }
    }
    void character(const char* source)
    {
        int64_t count = static_cast<int64_t>(std::strlen(source));
        bytes += count;
        characters += count ? 1 : 0;
        if (cursor) {
            cursor -= count;
            std::memcpy(cursor, source, static_cast<size_t>(count));
        }
    }
    void zeros(int64_t count)
    {
        bytes += count;
        characters += count;
        if (cursor) {
            cursor -= count;
            std::memset(cursor, '0', static_cast<size_t>(count));
        }
    }
};

// The number with the point and the separators of the spec: what follows the integer part, then the integer part group by group, from the
// right. With padding by zeros, zeros and separators go on to the left until the width is reached.
static void writeGrouped(BackwardWriter& writer, const char* sign, const char* integer, int64_t integerLength, const char* point, const char* rest, int64_t restLength, const FormatSpec& spec)
{
    int64_t signLength = sign ? 1 : 0;
    bool hasSeparator = *spec.separator;
    writer.ascii(rest, restLength);
    if (point)
        writer.character(point);

    const char* group = spec.grouping;
    int64_t take = *group;
    int64_t left = integerLength;
    bool isPadding = false;
    while (true) {
        // A group size of 0 or CHAR_MAX: all that is left is one group.
        if (!*group || *group == CHAR_MAX || take > left)
            take = left;
        left -= take;
        if (isPadding)
            writer.zeros(take);
        else
            writer.ascii(integer + left, take);
        if (!left) {
            if (spec.align == 'z' && writer.characters + signLength < spec.minWidth) {
                // From here on the digits are zeros, first those that fill up the group that was begun.
                left = spec.minWidth - (writer.characters + signLength);
                take = *group - take;
                isPadding = true;
                continue;
            }
            break;
        }
        if (hasSeparator) {
            // A separator takes the place of a zero, unless it would come first.
            if (isPadding && left > 1)
                --left;
            writer.character(spec.separator);
        }
        if (*group && group[1])
            ++group;
        take = *group;
    }
    if (sign)
        writer.ascii(sign, 1);
}

// `text` is [sign] digits [. [digits]] [exponent]. `characters` is set to its length in characters.
static bool applyLocale(Text& text, int64_t& characters, const FormatSpec& spec, uint32_t& status)
{
    const char* p = text.data;
    const char* sign = nullptr;
    if (*p == '+' || *p == '-' || *p == ' ')
        sign = p++;
    const char* integer = p;
    while (isDigit(*p))
        ++p;
    int64_t integerLength = p - integer;
    const char* point = nullptr;
    if (*p == '.') {
        ++p;
        point = spec.point;
    }
    const char* rest = p;
    int64_t restLength = static_cast<int64_t>(text.length) - (p - text.data);
    if (!point && (!*spec.separator || !*spec.grouping))
        return true;

    BackwardWriter counter;
    writeGrouped(counter, sign, integer, integerLength, point, rest, restLength, spec);
    Text grouped;
    if (!grouped.allocate(static_cast<size_t>(counter.bytes) + 1)) {
        status |= OutOfMemory;
        return false;
    }
    BackwardWriter writer;
    writer.cursor = grouped.data + counter.bytes;
    *writer.cursor = 0;
    writeGrouped(writer, sign, integer, integerLength, point, rest, restLength, spec);
    grouped.length = static_cast<size_t>(counter.bytes);
    characters = counter.characters;
    text.swap(grouped);
    return true;
}

static bool pad(Text& text, int64_t characters, const FormatSpec& spec, uint32_t& status)
{
    if (characters >= spec.minWidth)
        return true;
    size_t fillLength = std::strlen(spec.fill);
    size_t count = static_cast<size_t>(spec.minWidth - characters);
    Text padded;
    if (!padded.allocate(text.length + count * fillLength + 1)) {
        status |= OutOfMemory;
        return false;
    }
    char align = spec.align == 'z' ? '=' : spec.align;
    size_t before = 0;
    if (align == '>' || align == '=')
        before = count;
    else if (align == '^')
        before = count / 2;
    size_t after = count - before;

    const char* source = text.data;
    size_t sourceLength = text.length;
    char* p = padded.data;
    if (align == '=' && (*source == '-' || *source == '+' || *source == ' ')) {
        *p++ = *source++;
        --sourceLength;
    }
    for (size_t i = 0; i < before; ++i, p += fillLength)
        std::memcpy(p, spec.fill, fillLength);
    std::memcpy(p, source, sourceLength);
    p += sourceLength;
    for (size_t i = 0; i < after; ++i, p += fillLength)
        std::memcpy(p, spec.fill, fillLength);
    *p = 0;
    padded.length = static_cast<size_t>(p - padded.data);
    text.swap(padded);
    return true;
}

bool format(Text& text, const Number& number, const FormatSpec& givenSpec, const Context& context, uint32_t& status)
{
    FormatSpec spec = givenSpec;
    if (spec.minWidth > maxPrecision) {
        status |= InvalidOperation;
        return false;
    }
    unsigned flags = 0;
    char type = spec.type;
    if (type >= 'A' && type <= 'Z') {
        type = static_cast<char>(type | 0x20);
        flags |= TextUpperCase;
    }
    if (spec.sign == ' ')
        flags |= TextSpaceForPlus;
    else if (spec.sign == '+')
        flags |= TextPlus;

    Number scaled;
    const Number* shown = &number;
    int64_t pointPlace = 1;
    if (number.isSpecial()) {
        // No zeros before an infinity.
        if (spec.align == 'z') {
            spec.fill[0] = ' ';
            spec.fill[1] = 0;
            spec.align = '>';
        }
        if (type == '%')
            flags |= TextPercent;
    } else {
        uint32_t ownStatus = 0;
        if (type == '%') {
            flags |= TextPercent;
            if (!scaled.copy(number, status))
                return false;
            scaled.exponent += 2;
            shown = &scaled;
            type = 'f';
        }
        flags |= type == 'g' ? TextScientific : type == 'e' ? TextExponent : TextFixed;
        if (spec.precision >= 0) {
            if (spec.precision > maxPrecision) {
                status |= InvalidOperation;
                return false;
            }
            switch (type) {
            case 'g': {
                int64_t digits = spec.precision ? spec.precision : 1;
                if (shown->digits > digits) {
                    roundToDigits(scaled, *shown, digits, context, ownStatus);
                    shown = &scaled;
                }
                break;
            }
            case 'e':
                if (shown->hasZeroCoefficient())
                    pointPlace = 1 - spec.precision;
                else {
                    roundToDigits(scaled, *shown, spec.precision + 1, context, ownStatus);
                    shown = &scaled;
                }
                break;
            default:
                rescale(scaled, *shown, -spec.precision, context, ownStatus);
                shown = &scaled;
                break;
            }
        }
        if (type == 'f' && shown->isZero() && shown->exponent > 0) {
            rescale(scaled, *shown, 0, context, ownStatus);
            shown = &scaled;
        }
        if (ownStatus & errorConditions) {
            status |= ownStatus & errorConditions;
            return false;
        }
    }

    if (!toText(text, *shown, flags, pointPlace)) {
        status |= OutOfMemory;
        return false;
    }
    int64_t characters = static_cast<int64_t>(text.length);
    if (*spec.point && !shown->isSpecial()) {
        if (characters > maxPrecision + 36) {
            status |= InvalidOperation;
            return false;
        }
        if (!applyLocale(text, characters, spec, status))
            return false;
    }
    if (spec.minWidth)
        return pad(text, characters, spec, status);
    return true;
}

// ---- Base 2**64 ----

void fromBinaryMagnitude(Number& result, uint8_t sign, const uint64_t* words, size_t count, const Context& context, uint32_t& status)
{
    while (count && !words[count - 1])
        --count;
    if (!count) {
        result.set(sign, 0, 0);
        finalize(result, context, status);
        return;
    }
    if (count > static_cast<size_t>(2 * maxPrecision / digitsPerLimb)) {
        result.setError(InvalidOperation, status);
        return;
    }
    // A word is a little more than a limb: 64 bits against 63.1.
    int64_t capacity = static_cast<int64_t>(count) + static_cast<int64_t>(count) / 64 + 2;
    if (!result.reserveDiscarding(capacity, status))
        return;
    Limb* limbs = result.limbs();
    int64_t length = 0;
    // From the top: what there is times 2**64, plus the next word.
    for (size_t i = count; i--;) {
        Limb carry = words[i];
        for (int64_t j = 0; j < length; ++j)
            divideByRadix(limbs[j], carry, carry, limbs[j]);
        while (carry) {
            limbs[length++] = carry % radix;
            carry /= radix;
        }
    }
    result.flags = sign;
    result.exponent = 0;
    result.length = length;
    result.updateDigits();
    finalize(result, context, status);
}

size_t binaryWordsFor(const Number& number) { return static_cast<size_t>(number.length) + 1; }

size_t coefficientToBinary(const Number& number, uint64_t* words)
{
    size_t count = 0;
    for (int64_t i = number.length - 1; i >= 0; --i) {
        uint64_t carry = number.limbs()[i];
        for (size_t j = 0; j < count; ++j) {
            unsigned __int128 product = static_cast<unsigned __int128>(words[j]) * radix + carry;
            words[j] = static_cast<uint64_t>(product);
            carry = static_cast<uint64_t>(product >> 64);
        }
        if (carry)
            words[count++] = carry;
    }
    return count;
}

} // namespace Decimal
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
