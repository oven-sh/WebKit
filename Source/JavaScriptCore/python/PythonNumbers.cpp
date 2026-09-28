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
#include "PythonNumbers.h"

#include "JSBigIntInlines.h"
#include "JSCInlines.h"
#include "PyObjects.h"
#include <wtf/dtoa/double-conversion.h>

namespace JSC { namespace Python {

Number classify(JSValue value)
{
    Number number;
    if (value.isNumber()) {
        if (auto integer = taggedInteger(value)) {
            number.kind = Number::Kind::Small;
            number.small = *integer;
            return number;
        }
        number.kind = Number::Kind::Float;
        number.real = value.asNumber();
        return number;
    }
    if (value.isBoolean()) {
        number.kind = Number::Kind::Small;
        number.small = value.asBoolean();
        return number;
    }
    if (value.isHeapBigInt()) {
        number.kind = Number::Kind::Big;
        number.big = value.asHeapBigInt();
        return number;
    }
    if (auto* boxed = tryBoxedValue(value))
        return classify(boxed->value());
    return number;
}

bool isInt(JSValue value)
{
    return value.isHeapBigInt() || (value.isNumber() && taggedInteger(value));
}

bool isFloat(JSValue value)
{
    return value.isNumber() && !taggedInteger(value);
}

JSValue floatFromDouble(double value)
{
    return jsTaggedFloat(value);
}

JSValue intFromInt64(JSGlobalObject* globalObject, int64_t value)
{
    if (value == static_cast<int32_t>(value)) [[likely]]
        return jsNumber(static_cast<int32_t>(value));
    return JSBigInt::createFrom(globalObject, value);
}

JSValue intFromDouble(JSGlobalObject* globalObject, double value)
{
    value = std::trunc(value);
    if (value >= -2147483648.0 && value <= 2147483647.0)
        return jsNumber(static_cast<int32_t>(value));
    return JSBigInt::createFrom(globalObject, value);
}

// An int that fits an int32 is never a BigInt.
JSValue normalizeBigInt(JSValue value)
{
    if (!value || !value.isHeapBigInt())
        return value;
    JSBigInt* big = value.asHeapBigInt();
    if (big->length() > 1)
        return value;
    if (big->isZero())
        return jsNumber(0);
    uint64_t magnitude = big->digit(0);
    if (!big->sign() && magnitude <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
        return jsNumber(static_cast<int32_t>(magnitude));
    if (big->sign() && magnitude <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1)
        return jsNumber(static_cast<int32_t>(-static_cast<int64_t>(magnitude)));
    return value;
}

JSBigInt* toBigInt(JSGlobalObject* globalObject, const Number& number)
{
    ASSERT(number.isInt());
    if (number.kind == Number::Kind::Big)
        return number.big;
    return JSBigInt::createFrom(globalObject, number.small);
}

double toDouble(JSGlobalObject* globalObject, ThrowScope& scope, const Number& number)
{
    switch (number.kind) {
    case Number::Kind::Small:
        return number.small;
    case Number::Kind::Float:
        return number.real;
    case Number::Kind::Big: {
        double result = JSBigInt::toNumberHeap(number.big).asNumber();
        if (std::isinf(result))
            raise(globalObject, scope, BuiltinType::OverflowError, "int too large to convert to float"_s);
        return result;
    }
    case Number::Kind::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue raiseZeroDivision(JSGlobalObject* globalObject, ThrowScope& scope)
{
    return raise(globalObject, scope, BuiltinType::ZeroDivisionError, "division by zero"_s);
}

// JavaScript's BigInts have a largest size, and say RangeError past it.
static JSValue finishBigInt(JSGlobalObject* globalObject, ThrowScope& scope, JSValue result)
{
    if (scope.exception()) [[unlikely]] {
        if (!scope.tryClearException())
            return { };
        return raise(globalObject, scope, BuiltinType::OverflowError, "int too large"_s);
    }
    return normalizeBigInt(result);
}

// ---- int

static JSValue floorDivideBig(JSGlobalObject* globalObject, ThrowScope& scope, JSBigInt* left, JSBigInt* right, bool wantRemainder)
{
    if (right->isZero())
        return raiseZeroDivision(globalObject, scope);
    // JavaScript rounds towards zero, and the remainder has the sign of the dividend. Python rounds down, and it has the divisor's.
    JSValue remainder = JSBigInt::remainder(globalObject, left, right);
    RETURN_IF_EXCEPTION(scope, { });
    JSBigInt* remainderBig = remainder.asHeapBigInt();
    bool adjust = !remainderBig->isZero() && remainderBig->sign() != right->sign();
    if (wantRemainder) {
        if (adjust)
            remainder = JSBigInt::add(globalObject, remainderBig, right);
        return finishBigInt(globalObject, scope, remainder);
    }
    JSValue quotient = JSBigInt::divide(globalObject, left, right);
    RETURN_IF_EXCEPTION(scope, { });
    if (adjust)
        quotient = JSBigInt::dec(globalObject, quotient.asHeapBigInt());
    return finishBigInt(globalObject, scope, quotient);
}

static JSValue powerOfInts(JSGlobalObject* globalObject, ThrowScope& scope, const Number& base, const Number& exponent)
{
    bool exponentIsNegative = exponent.kind == Number::Kind::Small ? exponent.small < 0 : exponent.big->sign();
    if (exponentIsNegative) {
        double baseValue = toDouble(globalObject, scope, base);
        RETURN_IF_EXCEPTION(scope, { });
        double exponentValue = toDouble(globalObject, scope, exponent);
        RETURN_IF_EXCEPTION(scope, { });
        if (!baseValue)
            return raise(globalObject, scope, BuiltinType::ZeroDivisionError, "zero to a negative power"_s);
        return floatFromDouble(std::pow(baseValue, exponentValue));
    }
    if (base.kind == Number::Kind::Small && exponent.kind == Number::Kind::Small) {
        // By squaring, for as long as it fits.
        int64_t result = 1;
        int64_t factor = base.small;
        uint32_t remaining = exponent.small;
        bool overflowed = false;
        while (remaining) {
            if (remaining & 1) {
                if (!WTF::safeMultiply(result, factor, result)) {
                    overflowed = true;
                    break;
                }
            }
            remaining >>= 1;
            if (remaining && !WTF::safeMultiply(factor, factor, factor)) {
                overflowed = true;
                break;
            }
        }
        if (!overflowed)
            return intFromInt64(globalObject, result);
    }
    JSBigInt* baseBig = toBigInt(globalObject, base);
    RETURN_IF_EXCEPTION(scope, { });
    JSBigInt* exponentBig = toBigInt(globalObject, exponent);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = JSBigInt::exponentiate(globalObject, baseBig, exponentBig);
    return finishBigInt(globalObject, scope, result);
}

static JSValue trueDivideInts(JSGlobalObject* globalObject, ThrowScope& scope, const Number& left, const Number& right)
{
    if (right.kind == Number::Kind::Small && !right.small)
        return raiseZeroDivision(globalObject, scope);
    if (left.kind == Number::Kind::Small && right.kind == Number::Kind::Small)
        return floatFromDouble(static_cast<double>(left.small) / static_cast<double>(right.small));
    // FIXME: This rounds twice, and overflows where the quotient would not.
    double leftValue = toDouble(globalObject, scope, left);
    RETURN_IF_EXCEPTION(scope, { });
    double rightValue = toDouble(globalObject, scope, right);
    RETURN_IF_EXCEPTION(scope, { });
    return floatFromDouble(leftValue / rightValue);
}

static JSValue intBinaryOperation(JSGlobalObject* globalObject, ThrowScope& scope, BinaryOperator op, const Number& left, const Number& right)
{
    if (op == BinaryOperator::Div)
        return trueDivideInts(globalObject, scope, left, right);
    if (op == BinaryOperator::Pow)
        return powerOfInts(globalObject, scope, left, right);

    if (left.kind == Number::Kind::Small && right.kind == Number::Kind::Small) {
        int64_t a = left.small;
        int64_t b = right.small;
        switch (op) {
        case BinaryOperator::Add:
            return intFromInt64(globalObject, a + b);
        case BinaryOperator::Sub:
            return intFromInt64(globalObject, a - b);
        case BinaryOperator::Mult:
            return intFromInt64(globalObject, a * b);
        case BinaryOperator::FloorDiv: {
            if (!b)
                return raiseZeroDivision(globalObject, scope);
            int64_t quotient = a / b;
            if (a % b && (a < 0) != (b < 0))
                --quotient;
            return intFromInt64(globalObject, quotient);
        }
        case BinaryOperator::Mod: {
            if (!b)
                return raiseZeroDivision(globalObject, scope);
            int64_t remainder = a % b;
            if (remainder && (remainder < 0) != (b < 0))
                remainder += b;
            return intFromInt64(globalObject, remainder);
        }
        case BinaryOperator::BitAnd:
            return jsNumber(left.small & right.small);
        case BinaryOperator::BitOr:
            return jsNumber(left.small | right.small);
        case BinaryOperator::BitXor:
            return jsNumber(left.small ^ right.small);
        case BinaryOperator::LShift:
            if (b < 0)
                return raiseValueError(globalObject, scope, "negative shift count"_s);
            if (!a)
                return jsNumber(0);
            if (b < 32)
                return intFromInt64(globalObject, a * (static_cast<int64_t>(1) << b));
            break;
        case BinaryOperator::RShift:
            if (b < 0)
                return raiseValueError(globalObject, scope, "negative shift count"_s);
            return jsNumber(b >= 31 ? (left.small < 0 ? -1 : 0) : left.small >> b);
        case BinaryOperator::MatMult:
            return { };
        case BinaryOperator::Div:
        case BinaryOperator::Pow:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    if (op == BinaryOperator::MatMult)
        return { };
    if ((op == BinaryOperator::LShift || op == BinaryOperator::RShift) && (right.kind == Number::Kind::Small ? right.small < 0 : right.big->sign()))
        return raiseValueError(globalObject, scope, "negative shift count"_s);

    JSBigInt* a = toBigInt(globalObject, left);
    RETURN_IF_EXCEPTION(scope, { });
    JSBigInt* b = toBigInt(globalObject, right);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result;
    switch (op) {
    case BinaryOperator::Add:
        result = JSBigInt::add(globalObject, a, b);
        break;
    case BinaryOperator::Sub:
        result = JSBigInt::sub(globalObject, a, b);
        break;
    case BinaryOperator::Mult:
        result = JSBigInt::multiply(globalObject, a, b);
        break;
    case BinaryOperator::FloorDiv:
        return floorDivideBig(globalObject, scope, a, b, false);
    case BinaryOperator::Mod:
        return floorDivideBig(globalObject, scope, a, b, true);
    case BinaryOperator::BitAnd:
        result = JSBigInt::bitwiseAnd(globalObject, a, b);
        break;
    case BinaryOperator::BitOr:
        result = JSBigInt::bitwiseOr(globalObject, a, b);
        break;
    case BinaryOperator::BitXor:
        result = JSBigInt::bitwiseXor(globalObject, a, b);
        break;
    case BinaryOperator::LShift:
        result = JSBigInt::leftShift(globalObject, a, b);
        break;
    case BinaryOperator::RShift:
        result = JSBigInt::signedRightShift(globalObject, a, b);
        break;
    case BinaryOperator::MatMult:
    case BinaryOperator::Div:
    case BinaryOperator::Pow:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return finishBigInt(globalObject, scope, result);
}

// ---- float

// float_divmod() of CPython's Objects/floatobject.c.
static void floatDivmod(double left, double right, double& quotient, double& remainder)
{
    remainder = std::fmod(left, right);
    double division = (left - remainder) / right;
    if (remainder) {
        if ((right < 0) != (remainder < 0)) {
            remainder += right;
            division -= 1.0;
        }
    } else
        remainder = std::copysign(0.0, right);
    if (division) {
        quotient = std::floor(division);
        if (division - quotient > 0.5)
            quotient += 1.0;
    } else
        quotient = std::copysign(0.0, left / right);
}

static JSValue powerOfFloats(JSGlobalObject* globalObject, ThrowScope& scope, double base, double exponent)
{
    if (!exponent)
        return floatFromDouble(1.0);
    if (std::isnan(base))
        return floatFromDouble(base);
    if (std::isnan(exponent))
        return floatFromDouble(base == 1.0 ? 1.0 : exponent);
    if (!base && exponent < 0)
        return raise(globalObject, scope, BuiltinType::ZeroDivisionError, "zero to a negative power"_s);
    if (base < 0 && std::isfinite(base) && std::isfinite(exponent) && exponent != std::floor(exponent)) {
        // FIXME: This is a complex number.
        return raiseValueError(globalObject, scope, "negative number cannot be raised to a fractional power"_s);
    }
    double result = std::pow(base, exponent);
    if (std::isinf(result) && std::isfinite(base) && std::isfinite(exponent))
        return raise(globalObject, scope, BuiltinType::OverflowError, PyTuple::create(globalObject, { jsNumber(34), jsNontrivialString(globalObject->vm(), "Result too large"_s) }));
    return floatFromDouble(result);
}

static JSValue floatBinaryOperation(JSGlobalObject* globalObject, ThrowScope& scope, BinaryOperator op, double left, double right)
{
    switch (op) {
    case BinaryOperator::Add:
        return floatFromDouble(left + right);
    case BinaryOperator::Sub:
        return floatFromDouble(left - right);
    case BinaryOperator::Mult:
        return floatFromDouble(left * right);
    case BinaryOperator::Div:
        if (!right)
            return raiseZeroDivision(globalObject, scope);
        return floatFromDouble(left / right);
    case BinaryOperator::FloorDiv:
    case BinaryOperator::Mod: {
        if (!right)
            return raiseZeroDivision(globalObject, scope);
        double quotient;
        double remainder;
        floatDivmod(left, right, quotient, remainder);
        return floatFromDouble(op == BinaryOperator::Mod ? remainder : quotient);
    }
    case BinaryOperator::Pow:
        return powerOfFloats(globalObject, scope, left, right);
    default:
        return { }; // Not for floats.
    }
}

JSValue numberBinaryOperation(JSGlobalObject* globalObject, BinaryOperator op, JSValue leftValue, JSValue rightValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Number left = classify(leftValue);
    if (!left)
        return { };
    Number right = classify(rightValue);
    if (!right)
        return { };
    if (left.isInt() && right.isInt()) {
        // bool & bool is a bool.
        if (leftValue.isBoolean() && rightValue.isBoolean()) {
            switch (op) {
            case BinaryOperator::BitAnd:
                return jsBoolean(left.small & right.small);
            case BinaryOperator::BitOr:
                return jsBoolean(left.small | right.small);
            case BinaryOperator::BitXor:
                return jsBoolean(left.small ^ right.small);
            default:
                break;
            }
        }
        RELEASE_AND_RETURN(scope, intBinaryOperation(globalObject, scope, op, left, right));
    }
    double a = toDouble(globalObject, scope, left);
    RETURN_IF_EXCEPTION(scope, { });
    double b = toDouble(globalObject, scope, right);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, floatBinaryOperation(globalObject, scope, op, a, b));
}

JSValue numberUnaryOperation(JSGlobalObject* globalObject, UnaryOperator op, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Number number = classify(value);
    switch (number.kind) {
    case Number::Kind::None:
        return { };
    case Number::Kind::Small:
        switch (op) {
        case UnaryOperator::USub:
            return intFromInt64(globalObject, -static_cast<int64_t>(number.small));
        case UnaryOperator::UAdd:
            return jsNumber(number.small);
        case UnaryOperator::Invert:
            return jsNumber(~number.small);
        case UnaryOperator::Not:
            return jsBoolean(!number.small);
        }
        break;
    case Number::Kind::Big:
        switch (op) {
        case UnaryOperator::USub:
            RELEASE_AND_RETURN(scope, finishBigInt(globalObject, scope, JSBigInt::unaryMinus(globalObject, number.big)));
        case UnaryOperator::UAdd:
            return number.big;
        case UnaryOperator::Invert:
            RELEASE_AND_RETURN(scope, finishBigInt(globalObject, scope, JSBigInt::bitwiseNot(globalObject, number.big)));
        case UnaryOperator::Not:
            return jsBoolean(false);
        }
        break;
    case Number::Kind::Float:
        switch (op) {
        case UnaryOperator::USub:
            return floatFromDouble(-number.real);
        case UnaryOperator::UAdd:
            return floatFromDouble(number.real);
        case UnaryOperator::Invert:
            return { };
        case UnaryOperator::Not:
            return jsBoolean(!number.real);
        }
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static int fromComparisonResult(JSBigInt::ComparisonResult result, bool& isUnordered)
{
    switch (result) {
    case JSBigInt::ComparisonResult::Equal:
        return 0;
    case JSBigInt::ComparisonResult::LessThan:
        return -1;
    case JSBigInt::ComparisonResult::GreaterThan:
        return 1;
    case JSBigInt::ComparisonResult::Undefined:
        isUnordered = true;
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<int> numberCompare(const Number& left, const Number& right, bool& isUnordered)
{
    isUnordered = false;
    if (!left || !right)
        return std::nullopt;
    auto ofDoubles = [&] (double a, double b) -> int {
        if (a < b)
            return -1;
        if (a > b)
            return 1;
        if (a == b)
            return 0;
        isUnordered = true;
        return 0;
    };
    using Kind = Number::Kind;
    switch (left.kind) {
    case Kind::Small:
        switch (right.kind) {
        case Kind::Small:
            return left.small < right.small ? -1 : left.small > right.small;
        case Kind::Big:
            return right.big->sign() ? 1 : -1; // It does not fit an int32.
        case Kind::Float:
            return ofDoubles(left.small, right.real);
        case Kind::None:
            break;
        }
        break;
    case Kind::Big:
        switch (right.kind) {
        case Kind::Small:
            return left.big->sign() ? -1 : 1;
        case Kind::Big:
            return fromComparisonResult(JSBigInt::compare(JSValue(left.big), JSValue(right.big)), isUnordered);
        case Kind::Float:
            return fromComparisonResult(JSBigInt::compareToDouble(left.big, right.real), isUnordered);
        case Kind::None:
            break;
        }
        break;
    case Kind::Float:
        switch (right.kind) {
        case Kind::Small:
            return ofDoubles(left.real, right.small);
        case Kind::Big:
            return fromComparisonResult(JSBigInt::compareToDouble(left.real, right.big), isUnordered);
        case Kind::Float:
            return ofDoubles(left.real, right.real);
        case Kind::None:
            break;
        }
        break;
    case Kind::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- hash

// Numbers that are equal have the same hash, whatever their types. It is the number modulo 2**61 - 1. See Python/pyhash.c of CPython.
static constexpr unsigned hashBits = 61;
static constexpr uint64_t hashModulus = (static_cast<uint64_t>(1) << hashBits) - 1;

int64_t hashOfDouble(double value)
{
    if (!std::isfinite(value)) {
        if (std::isinf(value))
            return value > 0 ? 314159 : -314159;
        return 0; // FIXME: CPython hashes a NaN by its identity.
    }
    int exponent;
    double mantissa = std::frexp(value, &exponent);
    int sign = 1;
    if (mantissa < 0) {
        sign = -1;
        mantissa = -mantissa;
    }
    uint64_t x = 0;
    while (mantissa) {
        x = ((x << 28) & hashModulus) | x >> (hashBits - 28);
        mantissa *= 268435456.0; // 2**28
        exponent -= 28;
        uint64_t y = static_cast<uint64_t>(mantissa);
        mantissa -= y;
        x += y;
        if (x >= hashModulus)
            x -= hashModulus;
    }
    exponent = exponent >= 0 ? exponent % static_cast<int>(hashBits) : hashBits - 1 - ((-1 - exponent) % static_cast<int>(hashBits));
    x = ((x << exponent) & hashModulus) | x >> (hashBits - exponent);
    int64_t result = static_cast<int64_t>(x) * sign;
    return result == -1 ? -2 : result;
}

int64_t hashOfNumber(JSGlobalObject*, const Number& number)
{
    switch (number.kind) {
    case Number::Kind::Small:
        return number.small == -1 ? -2 : number.small;
    case Number::Kind::Float:
        return hashOfDouble(number.real);
    case Number::Kind::Big: {
        // Digit by digit from the top: x = (x * 2**64 + digit) mod (2**61 - 1), and 2**64 is 8 there.
        uint64_t x = 0;
        for (unsigned i = number.big->length(); i--;) {
            UInt128 wide = static_cast<UInt128>(x) * 8 + (number.big->digit(i) % hashModulus);
            // 2**64 = 8 (mod 2**61 - 1), so multiplying by it is multiplying by 8.
            x = static_cast<uint64_t>(wide % hashModulus);
        }
        int64_t result = number.big->sign() ? -static_cast<int64_t>(x) : static_cast<int64_t>(x);
        return result == -1 ? -2 : result;
    }
    case Number::Kind::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- repr

String reprOfInt(JSGlobalObject* globalObject, const Number& number, unsigned radix)
{
    if (number.kind == Number::Kind::Small) {
        if (radix == 10)
            return String::number(number.small);
        return JSBigInt::createFrom(globalObject, number.small)->toString(globalObject, radix);
    }
    return number.big->toString(globalObject, radix);
}

// The shortest digits that give the same float back, as for JavaScript, but laid out as Python does: 1e+16 and 1e-05, and always
// something to show that it is a float.
String reprOfDouble(double value)
{
    if (std::isnan(value))
        return "nan"_s;
    if (std::isinf(value))
        return value < 0 ? "-inf"_s : "inf"_s;

    std::array<char, WTF::double_conversion::DoubleToStringConverter::kBase10MaximalLength + 1> digits;
    bool isNegative;
    int length;
    int point;
    WTF::double_conversion::DoubleToStringConverter::DoubleToAscii(value, WTF::double_conversion::DoubleToStringConverter::SHORTEST, 0, std::span<char> { digits }, isNegative, length, point);

    StringBuilder builder;
    if (isNegative)
        builder.append('-');
    auto digit = [&] (int i) -> char { return i < length ? digits[i] : '0'; };
    if (point > 16 || point < -3) {
        builder.append(digit(0));
        if (length > 1) {
            builder.append('.');
            for (int i = 1; i < length; ++i)
                builder.append(digits[i]);
        }
        int exponent = point - 1;
        builder.append('e', exponent < 0 ? '-' : '+');
        exponent = std::abs(exponent);
        if (exponent < 10)
            builder.append('0');
        builder.append(exponent);
        return builder.toString();
    }
    if (point <= 0) {
        builder.append("0."_s);
        for (int i = point; i < 0; ++i)
            builder.append('0');
        for (int i = 0; i < length; ++i)
            builder.append(digits[i]);
        return builder.toString();
    }
    for (int i = 0; i < point; ++i)
        builder.append(digit(i));
    builder.append('.');
    if (length <= point)
        builder.append('0');
    for (int i = point; i < length; ++i)
        builder.append(digits[i]);
    return builder.toString();
}

} } // namespace JSC::Python
