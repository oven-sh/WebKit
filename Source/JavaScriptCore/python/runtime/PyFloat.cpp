/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyNumbers.h"

#include "JSBigIntInlines.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyStr.h"
#include "PyTypes.h"
#include <wtf/dtoa.h>
#include <wtf/dtoa/double-conversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// float. (Objects/floatobject.c and Python/pystrtod.c in CPython are the reference.)

PY_DEFINE_CELL(PyFloat);

PyFloat::PyFloat(VM& vm, Structure* structure, PyType* type, double value)
    : Base(vm, structure, type)
    , m_value(value)
{
}

PyFloat* PyFloat::create(VM& vm, Structure* structure, PyType* type, double value)
{
    auto* object = new (NotNull, allocateCell<PyFloat>(vm)) PyFloat(vm, structure, type, value);
    object->finishCreation(vm);
    return object;
}

JSValue makeFloatSlow(JSGlobalObject* globalObject, double value)
{
    Runtime& rt = runtime(globalObject);
    return PyFloat::create(rt.vm(), rt.structure<PyFloat>(), rt.floatType(), value);
}

// ---- Hashing ----

int64_t hashOfDouble(double value)
{
    static constexpr unsigned hashBits = 61;
    static constexpr uint64_t modulus = (static_cast<uint64_t>(1) << hashBits) - 1;
    if (!std::isfinite(value)) {
        if (std::isinf(value))
            return value > 0 ? 314159 : -314159;
        return 0;
    }
    int exponent;
    double mantissa = std::frexp(value, &exponent);
    int sign = 1;
    if (mantissa < 0) {
        sign = -1;
        mantissa = -mantissa;
    }
    // The value modulo 2**61 - 1, 28 bits at a time: as for an int, so that equal numbers hash alike.
    uint64_t x = 0;
    while (mantissa) {
        x = ((x << 28) & modulus) | x >> (hashBits - 28);
        mantissa *= 268435456.0;
        exponent -= 28;
        uint64_t y = static_cast<uint64_t>(mantissa);
        mantissa -= static_cast<double>(y);
        x += y;
        if (x >= modulus)
            x -= modulus;
    }
    exponent = exponent >= 0 ? exponent % hashBits : hashBits - 1 - ((-1 - exponent) % hashBits);
    x = ((x << exponent) & modulus) | x >> (hashBits - exponent);
    int64_t result = static_cast<int64_t>(x) * sign;
    return result == -1 ? -2 : result;
}

// ---- To text ----

namespace {

// The digits of a number, without leading or trailing zeros, and where the decimal point goes: 0.d1d2d3... * 10**point.
struct Digits {
    Vector<char, 32> digits;
    int point { 0 };
};

}

// Every double has a finite decimal expansion.
static void exactDigits(double value, Digits& result)
{
    ASSERT(std::isfinite(value) && value > 0);
    uint64_t bits = std::bit_cast<uint64_t>(value);
    uint64_t mantissa = bits & ((static_cast<uint64_t>(1) << 52) - 1);
    int biasedExponent = static_cast<int>((bits >> 52) & 0x7ff);
    int exponent;
    if (biasedExponent) {
        mantissa |= static_cast<uint64_t>(1) << 52;
        exponent = biasedExponent - 1075;
    } else
        exponent = -1074;
    int trailingZeros = std::countr_zero(mantissa);
    mantissa >>= trailingZeros;
    exponent += trailingZeros;

    // mantissa * 2**exponent is an integer, or (mantissa * 5**-exponent) / 10**-exponent.
    Vector<uint32_t, 96> big;
    big.append(static_cast<uint32_t>(mantissa));
    if (mantissa >> 32)
        big.append(static_cast<uint32_t>(mantissa >> 32));
    auto multiply = [&](uint32_t factor) {
        uint64_t carry = 0;
        for (auto& word : big) {
            uint64_t product = static_cast<uint64_t>(word) * factor + carry;
            word = static_cast<uint32_t>(product);
            carry = product >> 32;
        }
        if (carry)
            big.append(static_cast<uint32_t>(carry));
    };
    if (exponent >= 0) {
        for (int remaining = exponent; remaining > 0; remaining -= 31)
            multiply(static_cast<uint32_t>(1) << std::min(remaining, 31));
    } else {
        int remaining = -exponent;
        for (; remaining >= 13; remaining -= 13)
            multiply(1220703125); // 5**13
        uint32_t factor = 1;
        for (; remaining; --remaining)
            factor *= 5;
        multiply(factor);
    }

    Vector<uint32_t, 96> chunks; // Nine digits each, the least significant first.
    while (!big.isEmpty()) {
        uint64_t remainder = 0;
        for (size_t i = big.size(); i--;) {
            uint64_t current = (remainder << 32) | big[i];
            big[i] = static_cast<uint32_t>(current / 1000000000);
            remainder = current % 1000000000;
        }
        while (!big.isEmpty() && !big.last())
            big.removeLast();
        chunks.append(static_cast<uint32_t>(remainder));
    }
    result.digits.shrink(0);
    for (size_t i = chunks.size(); i--;) {
        char buffer[9];
        uint32_t chunk = chunks[i];
        for (int j = 8; j >= 0; --j) {
            buffer[j] = static_cast<char>('0' + chunk % 10);
            chunk /= 10;
        }
        int start = 0;
        if (i == chunks.size() - 1) {
            while (buffer[start] == '0')
                ++start;
        }
        result.digits.append(std::span<const char> { buffer + start, static_cast<size_t>(9 - start) });
    }
    result.point = static_cast<int>(result.digits.size()) + std::min(exponent, 0);
    while (result.digits.last() == '0')
        result.digits.removeLast();
}

enum class DigitsMode : uint8_t {
    Shortest, // The fewest that read back as the same double.
    Significant, // `count` digits in all.
    Fixed, // `count` digits after the decimal point.
};

// Correctly rounded, to even on a tie. `value` is finite and not negative.
static void digitsOf(double value, DigitsMode mode, int count, Digits& result)
{
    if (!value) {
        result.digits.append('0');
        result.point = 1;
        return;
    }
    if (mode == DigitsMode::Shortest) {
        char buffer[WTF::double_conversion::DoubleToStringConverter::kBase10MaximalLength + 1];
        bool sign;
        int length;
        WTF::double_conversion::DoubleToStringConverter::DoubleToAscii(value, WTF::double_conversion::DoubleToStringConverter::SHORTEST, 0, std::span<char> { buffer, sizeof(buffer) }, sign, length, result.point);
        result.digits.append(std::span<const char> { buffer, static_cast<size_t>(length) });
        return;
    }

    exactDigits(value, result);
    int64_t keep = mode == DigitsMode::Significant ? std::max(count, 1) : static_cast<int64_t>(result.point) + count;
    if (keep >= static_cast<int64_t>(result.digits.size()))
        return;
    auto becomeZero = [&] {
        result.digits.shrink(0);
        result.point = -count;
    };
    if (keep < 0)
        return becomeZero();

    auto& digits = result.digits;
    char next = digits[keep];
    bool roundUp = next > '5';
    if (next == '5') {
        if (static_cast<size_t>(keep) + 1 < digits.size())
            roundUp = true; // There are no trailing zeros, so something follows.
        else
            roundUp = keep && ((digits[keep - 1] - '0') & 1);
    }
    digits.shrink(keep);
    if (roundUp) {
        size_t i = digits.size();
        while (i && digits[i - 1] == '9')
            --i;
        digits.shrink(i);
        if (i)
            ++digits[i - 1];
        else {
            digits.append('1');
            ++result.point;
        }
    }
    while (!digits.isEmpty() && digits.last() == '0')
        digits.removeLast();
    if (digits.isEmpty())
        becomeZero();
}

String formatDouble(double value, char formatCode, int precision, unsigned flags)
{
    bool upperCase = false;
    DigitsMode mode;
    switch (formatCode) {
    case 'E':
        upperCase = true;
        formatCode = 'e';
        [[fallthrough]];
    case 'e':
        mode = DigitsMode::Significant;
        ++precision;
        break;
    case 'F':
        upperCase = true;
        formatCode = 'f';
        [[fallthrough]];
    case 'f':
        mode = DigitsMode::Fixed;
        break;
    case 'G':
        upperCase = true;
        formatCode = 'g';
        [[fallthrough]];
    case 'g':
        mode = DigitsMode::Significant;
        if (!precision)
            precision = 1;
        break;
    case 'r':
        mode = DigitsMode::Shortest;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool negative = std::signbit(value);
    StrBuilder builder;
    if (!std::isfinite(value)) {
        if (std::isnan(value))
            negative = false;
        if (negative)
            builder.append('-');
        else if (flags & FloatAlwaysSign)
            builder.append('+');
        builder.append(std::isnan(value) ? (upperCase ? "NAN"_s : "nan"_s) : (upperCase ? "INF"_s : "inf"_s));
        return builder.toString();
    }

    Digits result;
    digitsOf(std::abs(value), mode, precision, result);
    auto& digits = result.digits;
    int64_t digitCount = digits.size();
    int64_t point = result.point;
    if ((flags & FloatNoNegativeZero) && negative && (!digitCount || (digitCount == 1 && digits[0] == '0')))
        negative = false;

    // The output is a slice [start, end) of the digits with endless zeros on both sides of them.
    bool addDotZero = flags & FloatAddDotZero;
    bool alternate = flags & FloatAlternate;
    bool useExponent = false;
    int64_t end = digitCount;
    switch (formatCode) {
    case 'e':
        useExponent = true;
        end = precision;
        break;
    case 'f':
        end = point + precision;
        break;
    case 'g':
        if (point <= -4 || point > (addDotZero ? precision - 1 : precision))
            useExponent = true;
        if (alternate)
            end = precision;
        break;
    case 'r':
        if (point <= -4 || point > 16)
            useExponent = true;
        break;
    }
    int64_t exponent = 0;
    if (useExponent) {
        exponent = point - 1;
        point = 1;
    }
    int64_t start = point <= 0 ? point - 1 : 0;
    if (!useExponent && addDotZero)
        end = std::max(end, point + 1);
    else
        end = std::max(end, point);

    if (negative)
        builder.append('-');
    else if (flags & FloatAlwaysSign)
        builder.append('+');
    auto zeros = [&](int64_t count) {
        appendRepeated(builder, '0', count);
    };
    auto appendDigits = [&](int64_t from, int64_t to) {
        builder.append(std::span<const char> { digits.span().data() + from, static_cast<size_t>(to - from) });
    };
    if (point <= 0) {
        zeros(point - start);
        builder.append('.');
        zeros(-point);
    } else
        zeros(-start);
    if (0 < point && point <= digitCount) {
        appendDigits(0, point);
        builder.append('.');
        appendDigits(point, digitCount);
    } else
        appendDigits(0, digitCount);
    if (digitCount < point) {
        zeros(point - digitCount);
        builder.append('.');
        zeros(end - point);
    } else
        zeros(end - digitCount);
    if (!alternate && builder[builder.length() - 1] == '.')
        builder.shrink(builder.length() - 1);

    if (useExponent) {
        builder.append(upperCase ? 'E' : 'e', exponent < 0 ? '-' : '+');
        int64_t magnitude = std::abs(exponent);
        if (magnitude < 10)
            builder.append('0');
        builder.append(magnitude);
    }
    return builder.toString();
}

String floatRepr(double value)
{
    return formatDouble(value, 'r', 0, FloatAddDotZero);
}

// ---- From text ----

// How many characters at the start are the text of a float. 0: none.
size_t scanDouble(std::span<const Latin1Character> text, double& value)
{
    size_t size = text.size();
    size_t i = 0;
    bool negative = false;
    if (i < size && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    auto matches = [&](ASCIILiteral word) {
        if (size - i < word.length())
            return false;
        for (size_t j = 0; j < word.length(); ++j) {
            if (toASCIILower(text[i + j]) != word[j])
                return false;
        }
        return true;
    };
    if (matches("inf"_s)) {
        i += matches("infinity"_s) ? 8 : 3;
        value = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return i;
    }
    if (matches("nan"_s)) {
        value = negative ? -std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::quiet_NaN();
        return i + 3;
    }

    size_t start = i;
    size_t digitCount = 0;
    while (i < size && isASCIIDigit(text[i])) {
        ++i;
        ++digitCount;
    }
    if (i < size && text[i] == '.') {
        ++i;
        while (i < size && isASCIIDigit(text[i])) {
            ++i;
            ++digitCount;
        }
    }
    if (!digitCount)
        return 0;
    if (i < size && (text[i] == 'e' || text[i] == 'E')) {
        size_t j = i + 1;
        if (j < size && (text[j] == '+' || text[j] == '-'))
            ++j;
        if (j < size && isASCIIDigit(text[j])) {
            while (j < size && isASCIIDigit(text[j]))
                ++j;
            i = j;
        }
    }
    size_t parsed;
    double magnitude = WTF::parseDouble(text.subspan(start, i - start), parsed);
    // "1." and ".5e3" may be more than it cares to read.
    if (parsed != i - start) {
        Vector<Latin1Character, 64> normalized;
        if (text[start] == '.')
            normalized.append('0');
        for (size_t j = start; j < i; ++j) {
            normalized.append(text[j]);
            if (text[j] == '.' && (j + 1 == i || !isASCIIDigit(text[j + 1])))
                normalized.append('0');
        }
        magnitude = WTF::parseDouble(normalized.span(), parsed);
        ASSERT(parsed == normalized.size());
    }
    value = negative ? -magnitude : magnitude;
    return i;
}

std::optional<double> parseDouble(std::span<const Latin1Character> text)
{
    double value;
    size_t length = scanDouble(text, value);
    if (!length || length != text.size())
        return std::nullopt;
    return value;
}

// ASCII, with the whitespace at the ends gone, any decimal digit as an ASCII one, and the underscores between digits gone.
// False: there is something no number has.
bool normalizeNumberText(StringView text, Vector<Latin1Character, 64>& result)
{
    unsigned start = 0;
    unsigned end = text.length();
    while (start < end && isPythonWhitespace(text[start]))
        ++start;
    while (end > start && isPythonWhitespace(text[end - 1]))
        --end;
    bool previousIsDigit = false;
    for (unsigned i = start; i < end; ++i) {
        char32_t character = text[i];
        if (U16_IS_LEAD(character) && i + 1 < end && U16_IS_TRAIL(text[i + 1])) {
            character = U16_GET_SUPPLEMENTARY(character, text[i + 1]);
            ++i;
        }
        if (character == '_') {
            // Only between two digits.
            if (!previousIsDigit || i + 1 >= end || decimalDigitValue(text[i + 1]) < 0)
                return false;
            previousIsDigit = false;
            continue;
        }
        if (character >= 0x80) {
            int digit = decimalDigitValue(character);
            if (digit >= 0)
                character = '0' + digit;
            else if (isPythonWhitespace(character))
                character = ' '; // complex("( 1+1j )")
            else
                return false;
        }
        previousIsDigit = isASCIIDigit(character);
        result.append(static_cast<Latin1Character>(character));
    }
    return true;
}

std::optional<double> floatFromText(StringView text)
{
    Vector<Latin1Character, 64> normalized;
    if (!normalizeNumberText(text, normalized))
        return std::nullopt;
    return parseDouble(normalized.span());
}

int decimalDigitValue(char32_t character)
{
    if (character < 0x80)
        return isASCIIDigit(character) ? static_cast<int>(character - '0') : -1;
    return Unicode::decimalValue(character);
}

// ---- Arithmetic ----

static ALWAYS_INLINE bool isOddInteger(double value) { return std::fmod(std::abs(value), 2.0) == 1.0; }

static void floatDivMod(double a, double b, double& quotient, double& remainder)
{
    double mod = std::fmod(a, b);
    double div = (a - mod) / b;
    if (mod) {
        // The remainder has the sign of the divisor.
        if ((b < 0) != (mod < 0)) {
            mod += b;
            div -= 1.0;
        }
    } else
        mod = std::copysign(0.0, b);
    if (div) {
        quotient = std::floor(div);
        if (div - quotient > 0.5)
            quotient += 1.0;
    } else
        quotient = std::copysign(0.0, a / b);
    remainder = mod;
}

static JSValue floatPower(JSGlobalObject* globalObject, double base, double exponent)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto result = [&](double value) { return makeFloat(globalObject, value); };

    if (!exponent)
        return result(1.0);
    if (std::isnan(base))
        return result(base);
    if (std::isnan(exponent))
        return result(base == 1.0 ? 1.0 : exponent);
    if (std::isinf(exponent)) {
        base = std::abs(base);
        if (base == 1.0)
            return result(1.0);
        if ((exponent > 0.0) == (base > 1.0))
            return result(std::abs(exponent));
        return result(0.0);
    }
    if (std::isinf(base)) {
        bool odd = isOddInteger(exponent);
        if (exponent > 0.0)
            return result(odd ? base : std::abs(base));
        return result(odd ? std::copysign(0.0, base) : 0.0);
    }
    if (!base) {
        if (exponent < 0.0)
            return raiseZeroDivisionError(globalObject, scope, "zero to a negative power"_s);
        return result(isOddInteger(exponent) ? base : 0.0);
    }
    bool negate = false;
    if (base < 0.0) {
        // A root of a negative number is complex.
        if (exponent != std::floor(exponent))
            RELEASE_AND_RETURN(scope, complexPower(globalObject, base, 0.0, exponent, 0.0));
        base = -base;
        negate = isOddInteger(exponent);
    }
    if (base == 1.0)
        return result(negate ? -1.0 : 1.0);
    double value = std::pow(base, exponent);
    if (std::isinf(value))
        return raiseWithArgs(globalObject, scope, runtime(globalObject).OverflowError(), { jsNumber(34), makeStr(vm, "Numerical result out of range"_s) });
    return result(negate ? -value : value);
}

JSValue floatBinaryOperation(JSGlobalObject* globalObject, BinaryOperator op, double a, double b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    switch (op) {
    case BinaryOperator::Add:
        return makeFloat(globalObject, a + b);
    case BinaryOperator::Sub:
        return makeFloat(globalObject, a - b);
    case BinaryOperator::Mult:
        return makeFloat(globalObject, a * b);
    case BinaryOperator::Div:
        if (!b)
            return raiseZeroDivisionError(globalObject, scope, "division by zero"_s);
        return makeFloat(globalObject, a / b);
    case BinaryOperator::Mod:
    case BinaryOperator::FloorDiv: {
        if (!b)
            return raiseZeroDivisionError(globalObject, scope, "division by zero"_s);
        double quotient, remainder;
        floatDivMod(a, b, quotient, remainder);
        return makeFloat(globalObject, op == BinaryOperator::Mod ? remainder : quotient);
    }
    case BinaryOperator::Pow:
        RELEASE_AND_RETURN(scope, floatPower(globalObject, a, b));
    default:
        return { };
    }
}

JSValue tryNumberBinaryOperation(JSGlobalObject* globalObject, BinaryOperator op, JSValue a, JSValue b)
{
    if (a.isInt32() && b.isInt32()) {
        int64_t x = a.asInt32();
        int64_t y = b.asInt32();
        switch (op) {
        case BinaryOperator::Add:
            return jsNumber(static_cast<double>(x + y));
        case BinaryOperator::Sub:
            return jsNumber(static_cast<double>(x - y));
        case BinaryOperator::Mult:
            return makeInt(globalObject, x * y);
        case BinaryOperator::BitAnd:
            return jsNumber(static_cast<int32_t>(x & y));
        case BinaryOperator::BitOr:
            return jsNumber(static_cast<int32_t>(x | y));
        case BinaryOperator::BitXor:
            return jsNumber(static_cast<int32_t>(x ^ y));
        default:
            break;
        }
    }

    bool aIsInt = isExactInt(a);
    bool bIsInt = isExactInt(b);
    if (aIsInt && bIsInt) {
        switch (op) {
        case BinaryOperator::Add:
            return intAdd(globalObject, a, b);
        case BinaryOperator::Sub:
            return intSubtract(globalObject, a, b);
        case BinaryOperator::Mult:
            return intMultiply(globalObject, a, b);
        case BinaryOperator::Div:
            return intTrueDivide(globalObject, a, b);
        case BinaryOperator::Mod:
            return intModulo(globalObject, a, b);
        case BinaryOperator::Pow:
            return intPower(globalObject, a, b);
        case BinaryOperator::LShift:
            return intLeftShift(globalObject, a, b);
        case BinaryOperator::RShift:
            return intRightShift(globalObject, a, b);
        case BinaryOperator::BitOr:
            return intOr(globalObject, a, b);
        case BinaryOperator::BitXor:
            return intXor(globalObject, a, b);
        case BinaryOperator::BitAnd:
            return intAnd(globalObject, a, b);
        case BinaryOperator::FloorDiv:
            return intFloorDivide(globalObject, a, b);
        default:
            return { };
        }
    }

    // A float, or a float and an int that a double holds exactly.
    auto asDouble = [&](JSValue value, bool isInteger, double& result) {
        if (value.isNumber()) {
            result = value.asNumber();
            return true;
        }
        if (isInteger || !PyFloat::is(value))
            return false;
        auto* object = uncheckedDowncast<PyFloat>(value.asCell());
        if (object->type() != runtime(globalObject).floatType())
            return false;
        result = object->value();
        return true;
    };
    double x, y;
    if (!asDouble(a, aIsInt, x) || !asDouble(b, bIsInt, y))
        return { };
    return floatBinaryOperation(globalObject, op, x, y);
}

// ---- The methods of float ----

// The operand as a double. False: it is neither a float nor an int (`result` is then NotImplemented), or an exception.
static bool convertOperand(JSGlobalObject* globalObject, JSValue value, double& number, JSValue& result)
{
    if (value.isNumber()) {
        number = value.asNumber();
        return true;
    }
    if (isFloat(value)) {
        number = floatValue(value);
        return true;
    }
    if (isInt(value)) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        number = toDouble(globalObject, value);
        if (scope.exception()) {
            result = JSValue();
            return false;
        }
        return true;
    }
    result = runtime(globalObject).notImplemented();
    return false;
}

template<BinaryOperator op>
static JSValue floatBinaryMethod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    double b;
    JSValue result;
    if (!convertOperand(globalObject, other, b, result))
        return result;
    return floatBinaryOperation(globalObject, op, floatValue(self), b);
}

template<BinaryOperator op>
static JSValue floatReflectedMethod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    double a;
    JSValue result;
    if (!convertOperand(globalObject, other, a, result))
        return result;
    return floatBinaryOperation(globalObject, op, a, floatValue(self));
}

static JSValue floatDivModOf(JSGlobalObject* globalObject, double a, double b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!b)
        return raiseZeroDivisionError(globalObject, scope, "division by zero"_s);
    double quotient, remainder;
    floatDivMod(a, b, quotient, remainder);
    return PyTuple::create(globalObject, { makeFloat(globalObject, quotient), makeFloat(globalObject, remainder) });
}

static JSValue floatDivModMethod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    double b;
    JSValue result;
    if (!convertOperand(globalObject, other, b, result))
        return result;
    return floatDivModOf(globalObject, floatValue(self), b);
}

static JSValue floatRDivModMethod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    double a;
    JSValue result;
    if (!convertOperand(globalObject, other, a, result))
        return result;
    return floatDivModOf(globalObject, a, floatValue(self));
}

static JSValue floatPowMethod(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue modulus)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (modulus && !isNone(modulus))
        return raiseTypeError(globalObject, scope, "pow() 3rd argument not allowed unless all arguments are integers"_s);
    RELEASE_AND_RETURN(scope, floatBinaryMethod<BinaryOperator::Pow>(globalObject, self, other));
}

static JSValue floatRPowMethod(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue modulus)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (modulus && !isNone(modulus))
        return raiseTypeError(globalObject, scope, "pow() 3rd argument not allowed unless all arguments are integers"_s);
    RELEASE_AND_RETURN(scope, floatReflectedMethod<BinaryOperator::Pow>(globalObject, self, other));
}

template<CompareOperator op>
static JSValue floatCompareMethod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    double a = floatValue(self);
    auto decide = [](auto x, auto y) {
        switch (op) {
        case CompareOperator::Eq:
            return x == y;
        case CompareOperator::NotEq:
            return x != y;
        case CompareOperator::Lt:
            return x < y;
        case CompareOperator::LtE:
            return x <= y;
        case CompareOperator::Gt:
            return x > y;
        case CompareOperator::GtE:
            return x >= y;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    };
    if (other.isNumber())
        return jsBoolean(decide(a, other.asNumber()));
    if (isFloat(other))
        return jsBoolean(decide(a, floatValue(other)));
    if (!isInt(other))
        return runtime(globalObject).notImplemented();
    JSValue integer = intValue(other);
    if (integer.isNumber())
        return jsBoolean(decide(a, integer.asNumber()));
    // Too large for a double to hold exactly.
    if (std::isnan(a))
        return jsBoolean(op == CompareOperator::NotEq);
    switch (JSBigInt::compareToDouble(integer.asHeapBigInt(), a)) {
    case JSBigInt::ComparisonResult::Equal:
        return jsBoolean(decide(0, 0));
    case JSBigInt::ComparisonResult::LessThan:
        return jsBoolean(decide(1, 0)); // The int is the lesser.
    default:
        return jsBoolean(decide(0, 1));
    }
}

static JSValue floatReprMethod(JSGlobalObject* globalObject, JSValue self) { return makeStr(globalObject, floatRepr(floatValue(self))); }
static JSValue floatHashMethod(JSGlobalObject* globalObject, JSValue self)
{
    double value = floatValue(self);
    // Two of them are never equal, so they had better not collide.
    if (std::isnan(value) && self.isCell())
        return objectHash(globalObject, self);
    return makeInt(globalObject, hashOfDouble(value));
}
static JSValue floatBoolMethod(JSGlobalObject*, JSValue self) { return jsBoolean(!!floatValue(self)); }
static JSValue floatNegMethod(JSGlobalObject* globalObject, JSValue self) { return makeFloat(globalObject, -floatValue(self)); }
static JSValue floatAbsMethod(JSGlobalObject* globalObject, JSValue self) { return makeFloat(globalObject, std::abs(floatValue(self))); }
static JSValue floatIdentity(JSGlobalObject* globalObject, JSValue self)
{
    if (self.isDouble() || typeOf(globalObject, self) == runtime(globalObject).floatType())
        return self;
    return makeFloat(globalObject, floatValue(self));
}
static JSValue floatIntMethod(JSGlobalObject* globalObject, JSValue self) { return intFromDouble(globalObject, floatValue(self)); }
static JSValue floatFloorMethod(JSGlobalObject* globalObject, JSValue self) { return intFromDouble(globalObject, std::floor(floatValue(self))); }
static JSValue floatCeilMethod(JSGlobalObject* globalObject, JSValue self) { return intFromDouble(globalObject, std::ceil(floatValue(self))); }
static JSValue floatZero(JSGlobalObject* globalObject, JSValue) { return makeFloat(globalObject, 0.0); }
static JSValue floatGetNewArgs(JSGlobalObject* globalObject, JSValue self) { return PyTuple::create(globalObject, { makeFloat(globalObject, floatValue(self)) }); }

static JSValue floatIsInteger(JSGlobalObject*, JSValue self)
{
    double value = floatValue(self);
    return jsBoolean(std::isfinite(value) && std::floor(value) == value);
}

static JSValue floatRoundMethod(JSGlobalObject* globalObject, JSValue self, JSValue digitsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double value = floatValue(self);
    if (!digitsValue || isNone(digitsValue)) {
        double rounded = std::round(value);
        if (std::abs(value - rounded) == 0.5)
            rounded = 2.0 * std::round(value / 2.0); // To even.
        RELEASE_AND_RETURN(scope, intFromDouble(globalObject, rounded));
    }
    int64_t count = toSsize(globalObject, digitsValue, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    if (!std::isfinite(value))
        return makeFloat(globalObject, value);
    // Beyond these, there is nothing to round off, or nothing left.
    if (count > 323)
        return makeFloat(globalObject, value);
    if (count < -308)
        return makeFloat(globalObject, 0.0 * value);

    Digits digits;
    digitsOf(std::abs(value), DigitsMode::Fixed, static_cast<int>(count), digits);
    Vector<Latin1Character, 64> text;
    text.append('0');
    for (char digit : digits.digits)
        text.append(digit);
    text.append('e');
    String exponent = String::number(digits.point - static_cast<int>(digits.digits.size()));
    text.append(exponent.span8());
    double rounded = *parseDouble(text.span());
    if (std::isinf(rounded))
        return raiseOverflowError(globalObject, scope, "rounded value too large to represent"_s);
    return makeFloat(globalObject, std::signbit(value) ? -rounded : rounded);
}

static JSValue floatAsIntegerRatio(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double value = floatValue(self);
    if (std::isinf(value))
        return raiseOverflowError(globalObject, scope, "cannot convert Infinity to integer ratio"_s);
    if (std::isnan(value))
        return raiseValueError(globalObject, scope, "cannot convert NaN to integer ratio"_s);
    int exponent;
    double fraction = std::frexp(value, &exponent);
    for (int i = 0; i < 300 && fraction != std::floor(fraction); ++i) {
        fraction *= 2.0;
        --exponent;
    }
    JSValue numerator = intFromDouble(globalObject, fraction);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue denominator = jsNumber(1);
    if (exponent > 0)
        numerator = intLeftShift(globalObject, numerator, jsNumber(exponent));
    else
        denominator = intLeftShift(globalObject, denominator, jsNumber(-exponent));
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { numerator, denominator });
}

static JSValue floatHex(JSGlobalObject* globalObject, JSValue self)
{
    double value = floatValue(self);
    if (!std::isfinite(value))
        return makeStr(globalObject, floatRepr(value));
    if (!value)
        return makeStr(getVM(globalObject), std::signbit(value) ? "-0x0.0p+0"_s : "0x0.0p+0"_s);
    int exponent;
    double mantissa = std::frexp(std::abs(value), &exponent);
    int shift = 1 - std::max(std::numeric_limits<double>::min_exponent - exponent, 0);
    mantissa = std::ldexp(mantissa, shift);
    exponent -= shift;
    StrBuilder builder;
    if (value < 0)
        builder.append('-');
    builder.append("0x"_s);
    auto digit = [&] {
        int integer = static_cast<int>(mantissa);
        builder.append("0123456789abcdef"[integer]);
        mantissa -= integer;
    };
    digit();
    builder.append('.');
    for (int i = 0; i < 13; ++i) {
        mantissa *= 16.0;
        digit();
    }
    builder.append('p', exponent < 0 ? '-' : '+', std::abs(exponent));
    return makeStr(globalObject, builder);
}

static JSValue floatFromHex(JSGlobalObject* globalObject, JSValue cls, JSValue string)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(string))
        return raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
    String text = strValue(string)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    auto invalid = [&] { return raiseValueError(globalObject, scope, "invalid hexadecimal floating-point string"_s); };
    auto finish = [&](double value) -> JSValue {
        JSValue result = makeFloat(globalObject, value);
        if (cls.asCell() == rt.floatType())
            return result;
        RELEASE_AND_RETURN(scope, call(globalObject, cls, result));
    };

    // Unlike float(), this knows the white space of C only.
    auto isSpace = [](char16_t character) { return character == ' ' || (character >= '\t' && character <= '\r'); };
    unsigned i = 0;
    unsigned end = text.length();
    while (i < end && isSpace(text[i]))
        ++i;
    while (end > i && isSpace(text[end - 1]))
        --end;
    for (unsigned j = i; j < end; ++j) {
        if (text[j] >= 0x80)
            return invalid();
    }
    {
        // "inf", "nan" and the like.
        Vector<Latin1Character, 16> ascii;
        for (unsigned j = i; j < end; ++j)
            ascii.append(static_cast<Latin1Character>(text[j]));
        unsigned first = !ascii.isEmpty() && (ascii[0] == '+' || ascii[0] == '-');
        if (first < ascii.size() && (toASCIILower(ascii[first]) == 'i' || toASCIILower(ascii[first]) == 'n')) {
            auto special = parseDouble(ascii.span());
            if (!special)
                return invalid();
            return finish(*special);
        }
    }
    bool negative = false;
    if (i < end && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    if (i + 1 < end && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X'))
        i += 2;

    // The first 64 significant bits, whether anything is lost after them, and the power of two to scale by.
    uint64_t mantissa = 0;
    bool sticky = false;
    int64_t exponent = 0;
    unsigned digitCount = 0;
    bool afterPoint = false;
    for (; i < end; ++i) {
        char16_t character = text[i];
        if (character == '.') {
            if (afterPoint)
                return invalid();
            afterPoint = true;
            continue;
        }
        if (!isASCIIHexDigit(character))
            break;
        ++digitCount;
        unsigned digit = toASCIIHexValue(character);
        if (mantissa >> 60) {
            sticky |= !!digit;
            if (!afterPoint)
                exponent += 4;
        } else {
            mantissa = (mantissa << 4) | digit;
            if (afterPoint)
                exponent -= 4;
        }
    }
    if (!digitCount)
        return invalid();
    if (i < end && (text[i] == 'p' || text[i] == 'P')) {
        ++i;
        bool negativeExponent = false;
        if (i < end && (text[i] == '+' || text[i] == '-')) {
            negativeExponent = text[i] == '-';
            ++i;
        }
        if (i >= end || !isASCIIDigit(text[i]))
            return invalid();
        int64_t given = 0;
        for (; i < end && isASCIIDigit(text[i]); ++i)
            given = std::min<int64_t>(given * 10 + (text[i] - '0'), 1000000000);
        exponent += negativeExponent ? -given : given;
    }
    if (i != end)
        return invalid();
    if (!mantissa)
        return finish(negative ? -0.0 : 0.0);

    int bitLength = 64 - std::countl_zero(mantissa);
    int64_t top = exponent + bitLength; // The value is below 2**top.
    if (top > std::numeric_limits<double>::max_exponent)
        return raiseOverflowError(globalObject, scope, "hexadecimal value too large to represent as a float"_s);
    if (top < std::numeric_limits<double>::min_exponent - std::numeric_limits<double>::digits)
        return finish(negative ? -0.0 : 0.0);
    int keep = std::numeric_limits<double>::digits;
    if (top < std::numeric_limits<double>::min_exponent)
        keep -= static_cast<int>(std::numeric_limits<double>::min_exponent - top);
    int drop = bitLength - keep;
    if (drop > 0) {
        uint64_t mask = drop >= 64 ? ~static_cast<uint64_t>(0) : (static_cast<uint64_t>(1) << drop) - 1;
        uint64_t low = mantissa & mask;
        uint64_t half = static_cast<uint64_t>(1) << (drop - 1);
        mantissa = drop >= 64 ? 0 : mantissa >> drop;
        if (low > half || (low == half && (sticky || (mantissa & 1))))
            ++mantissa;
        exponent += drop;
    }
    double value = std::ldexp(static_cast<double>(mantissa), static_cast<int>(exponent));
    if (std::isinf(value))
        return raiseOverflowError(globalObject, scope, "hexadecimal value too large to represent as a float"_s);
    return finish(negative ? -value : value);
}

static JSValue floatFormatMethod(JSGlobalObject* globalObject, JSValue self, JSValue spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(spec))
        return raiseTypeError(globalObject, scope, "__format__() argument must be str, not "_s, typeNameOfArgument(globalObject, spec));
    RELEASE_AND_RETURN(scope, formatFloat(globalObject, self, strValue(spec)));
}

static JSValue floatGetFormat(JSGlobalObject* globalObject, JSValue, JSValue kind)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(kind))
        return raiseTypeError(globalObject, scope, "__getformat__() argument must be str, not "_s, typeNameOfArgument(globalObject, kind));
    String text = strValue(kind)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (text != "double"_s && text != "float"_s)
        return raiseValueError(globalObject, scope, "__getformat__() argument 1 must be 'double' or 'float'"_s);
    return makeStr(vm, "IEEE, little-endian"_s);
}

// float(value), for anything but text.
static bool floatFromNumber(JSGlobalObject* globalObject, JSValue source, double& value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, source);
    if (!isFloat(source) && !isInt(source) && !type->lookup(Special::Float) && !type->lookup(Special::Index)) {
        raiseTypeError(globalObject, scope, "float() argument must be a string or a real number, not '"_s, type->nameInMessages(), '\'');
        return false;
    }
    value = toDoubleAsFloatDoes(globalObject, source);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

static JSValue floatNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    if (!rejectKeywordsInNew(globalObject, "float"_s, type, rt.floatType(), args))
        return { };
    if (!checkPositional(globalObject, "float"_s, args, 0, 1))
        return { };

    double value = 0.0;
    if (args.size()) {
        JSValue source = args[0];
        if (type == rt.floatType() && rt.typeOf(source) == type)
            return source;
        // An instance of a class derived from str or bytes may have a __float__.
        bool isText = isStr(source) || isBytesLike(source);
        if (isText && !source.isString()) {
            PyType* sourceType = rt.typeOf(source);
            isText = !sourceType->lookup(Special::Float) && !sourceType->lookup(Special::Index);
        }
        if (isText) {
            String text;
            if (isStr(source)) {
                text = strValue(source)->value(globalObject);
                RETURN_IF_EXCEPTION(scope, { });
            } else {
                std::span<const uint8_t> bytes;
                getBytesLike(globalObject, source, bytes);
                RETURN_IF_EXCEPTION(scope, { });
                // Not Latin-1, in which 0xA0 would be a space.
                if (std::ranges::all_of(bytes, isASCII<uint8_t>))
                    text = String(byteCast<Latin1Character>(bytes));
            }
            auto parsed = text.isNull() ? std::nullopt : floatFromText(text);
            if (!parsed)
                return raiseValueError(globalObject, scope, "could not convert string to float: "_s, reprForMessage(globalObject, source));
            value = *parsed;
        } else {
            floatFromNumber(globalObject, source, value);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    if (type == rt.floatType())
        return makeFloat(globalObject, value);
    return PyFloat::create(vm, rt.structure<PyFloat>(), type, value);
}

static JSValue floatFromNumberMethod(JSGlobalObject* globalObject, JSValue cls, JSValue number)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (cls.asCell() == rt.floatType() && rt.typeOf(number) == rt.floatType())
        return number;
    double value = toDouble(globalObject, number);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = makeFloat(globalObject, value);
    if (cls.asCell() == rt.floatType())
        return result;
    RELEASE_AND_RETURN(scope, call(globalObject, cls, result));
}

static constexpr NativeDef floatMethods[] = {
    PY_NEW(floatNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", floatReprMethod, "($self, /)"),
    PY_SLOT("__hash__", floatHashMethod, "($self, /)"),
    PY_SLOT("__bool__", floatBoolMethod, "($self, /)"),
    PY_SLOT("__eq__", floatCompareMethod<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", floatCompareMethod<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", floatCompareMethod<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", floatCompareMethod<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", floatCompareMethod<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", floatCompareMethod<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__add__", floatBinaryMethod<BinaryOperator::Add>, "($self, value, /)"),
    PY_SLOT("__radd__", floatReflectedMethod<BinaryOperator::Add>, "($self, value, /)"),
    PY_SLOT("__sub__", floatBinaryMethod<BinaryOperator::Sub>, "($self, value, /)"),
    PY_SLOT("__rsub__", floatReflectedMethod<BinaryOperator::Sub>, "($self, value, /)"),
    PY_SLOT("__mul__", floatBinaryMethod<BinaryOperator::Mult>, "($self, value, /)"),
    PY_SLOT("__rmul__", floatReflectedMethod<BinaryOperator::Mult>, "($self, value, /)"),
    PY_SLOT("__mod__", floatBinaryMethod<BinaryOperator::Mod>, "($self, value, /)"),
    PY_SLOT("__rmod__", floatReflectedMethod<BinaryOperator::Mod>, "($self, value, /)"),
    PY_SLOT("__divmod__", floatDivModMethod, "($self, value, /)"),
    PY_SLOT("__rdivmod__", floatRDivModMethod, "($self, value, /)"),
    PY_SLOT("__pow__", floatPowMethod, "($self, value, mod=None, /)"),
    PY_SLOT("__rpow__", floatRPowMethod, "($self, value, mod=None, /)"),
    PY_SLOT("__neg__", floatNegMethod, "($self, /)"),
    PY_SLOT("__pos__", floatIdentity, "($self, /)"),
    PY_SLOT("__abs__", floatAbsMethod, "($self, /)"),
    PY_SLOT("__int__", floatIntMethod, "($self, /)"),
    PY_SLOT("__float__", floatIdentity, "($self, /)"),
    PY_SLOT("__floordiv__", floatBinaryMethod<BinaryOperator::FloorDiv>, "($self, value, /)"),
    PY_SLOT("__rfloordiv__", floatReflectedMethod<BinaryOperator::FloorDiv>, "($self, value, /)"),
    PY_SLOT("__truediv__", floatBinaryMethod<BinaryOperator::Div>, "($self, value, /)"),
    PY_SLOT("__rtruediv__", floatReflectedMethod<BinaryOperator::Div>, "($self, value, /)"),
    PY_CLASSMETHOD("from_number", floatFromNumberMethod, "($type, number, /)"),
    PY_METHOD("conjugate", floatIdentity, "($self, /)"),
    PY_METHOD("__trunc__", floatIntMethod, "($self, /)"),
    PY_METHOD("__floor__", floatFloorMethod, "($self, /)"),
    PY_METHOD("__ceil__", floatCeilMethod, "($self, /)"),
    PY_METHOD("__round__", floatRoundMethod, "($self, ndigits=None, /)"),
    PY_METHOD("as_integer_ratio", floatAsIntegerRatio, "($self, /)"),
    PY_CLASSMETHOD("fromhex", floatFromHex, "($type, string, /)"),
    PY_METHOD("hex", floatHex, "($self, /)"),
    PY_METHOD("is_integer", floatIsInteger, "($self, /)"),
    PY_METHOD("__getnewargs__", floatGetNewArgs, "($self, /)"),
    PY_CLASSMETHOD("__getformat__", floatGetFormat, "($type, typestr, /)"),
    PY_METHOD("__format__", floatFormatMethod, "($self, format_spec, /)"),
};

static constexpr NativeGetSet floatGetSets[] = {
    { "real"_s, floatIdentity },
    { "imag"_s, floatZero },
};

void initializeFloatTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.floatType()->initializeBuiltin(globalObject, { "float"_s, { TypeFlag::Base, TypeFlag::FloatSubclass, TypeFlag::MatchSelf, TypeFlag::OwnLayout }, floatMethods, floatGetSets, "Convert a string or number to a floating-point number, if possible."_s }, rt.typeType(), rt.objectType());
    initializeComplexType(globalObject);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
