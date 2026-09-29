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
#include "PythonText.h"
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

JSValue intFromUInt64(JSGlobalObject* globalObject, uint64_t value)
{
    if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return intFromInt64(globalObject, static_cast<int64_t>(value));
    return JSBigInt::createFrom(globalObject, value);
}

// PyNumber_Check()
bool isNumber(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    PyType* type = typeOf(globalObject, value);
    return type->lookup(vm, names.dunder_index) || type->lookup(vm, names.dunder_int) || type->lookup(vm, names.dunder_float) || type->isSubtypeOf(globalObject->pyRealm()->typeComplex());
}

std::optional<int64_t> toSsizeOfInt(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!isInstance(globalObject, value, globalObject->pyRealm()->typeInt())) {
        raiseTypeError(globalObject, scope, "an integer is required"_s);
        return std::nullopt;
    }
    RELEASE_AND_RETURN(scope, toSsize(globalObject, value));
}

std::optional<int> toCIntOfFormat(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto given = toCLong(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (*given > std::numeric_limits<int>::max() || *given < std::numeric_limits<int>::min()) {
        raise(globalObject, scope, BuiltinType::OverflowError, *given > 0 ? "signed integer is greater than maximum"_s : "signed integer is less than minimum"_s);
        return std::nullopt;
    }
    return static_cast<int>(*given);
}

uint64_t lowBitsOfInt(JSValue value)
{
    Number number = classify(value);
    ASSERT(number.isInt());
    return number.kind == Number::Kind::Small ? static_cast<uint64_t>(static_cast<int64_t>(number.small)) : JSBigInt::toBigUInt64(number.big);
}

JSValue floatFromDouble(double value)
{
    // It may have been read out of bytes that a program supplied, and then it can be any NaN at all.
    return jsTaggedFloat(purifyNaNKeepingPayload(value));
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

JSValue intFromDigits(JSGlobalObject* globalObject, std::span<const uint64_t> digits, bool isNegative)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    static_assert(JSBigInt::digitBits == 64);
    size_t length = digits.size();
    while (length && !digits[length - 1])
        --length;
    if (!length)
        return jsNumber(0);
    // It says for itself if that is more than it can have.
    JSBigInt* result = length <= std::numeric_limits<unsigned>::max() ? JSBigInt::tryCreateWithLength(vm, static_cast<unsigned>(length)) : nullptr;
    if (!result)
        return raiseMemoryError(globalObject, scope);
    for (unsigned i = 0; i < length; ++i)
        result->setDigit(i, digits[i]);
    result->setSign(isNegative);
    return normalizeBigInt(result);
}

Vector<uint64_t, 4> digitsOfInt(const Number& number)
{
    ASSERT(number.isInt());
    Vector<uint64_t, 4> digits;
    if (number.kind == Number::Kind::Small) {
        if (number.small)
            digits.append(static_cast<uint64_t>(std::abs(static_cast<int64_t>(number.small))));
        return digits;
    }
    for (unsigned i = 0; i < number.big->length(); ++i)
        digits.append(number.big->digit(i));
    return digits;
}

// ---- Floats as bytes

// The bytes of something, the least first or the most.
template<typename T, size_t size>
static void storeBytes(T value, std::span<uint8_t, size> bytes, bool isLittleEndian)
{
    static_assert(sizeof(T) == size);
    static_assert(std::endian::native == std::endian::little);
    memcpySpan(bytes, asByteSpan(value));
    if (!isLittleEndian)
        std::ranges::reverse(bytes);
}

template<typename T, size_t size>
static T loadBytes(std::span<const uint8_t, size> bytes, bool isLittleEndian)
{
    static_assert(sizeof(T) == size);
    std::array<uint8_t, size> ordered;
    memcpySpan(std::span(ordered), bytes);
    if (!isLittleEndian)
        std::ranges::reverse(ordered);
    return std::bit_cast<T>(ordered);
}

bool packFloat2(JSGlobalObject* globalObject, double x, std::span<uint8_t, 2> bytes, bool isLittleEndian)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto overflow = [&] {
        raise(globalObject, scope, BuiltinType::OverflowError, "float too large to pack with e format"_s);
        return false;
    };
    unsigned sign;
    int e;
    unsigned short bits;
    if (x == 0.0) {
        sign = std::signbit(x);
        e = 0;
        bits = 0;
    } else if (std::isinf(x)) {
        sign = x < 0.0;
        e = 0x1f;
        bits = 0;
    } else if (std::isnan(x)) {
        sign = std::signbit(x);
        e = 0x1f;
        // What kind of NaN it is, and what else it has in it
        bits = static_cast<unsigned short>((std::bit_cast<uint64_t>(x) & 0xffc0000000000ULL) >> 42);
        if (!bits)
            bits |= 1 << 9;
    } else {
        sign = x < 0.0;
        if (sign)
            x = -x;
        double f = std::frexp(x, &e);
        // So that f is at least 1 and less than 2
        f *= 2.0;
        e--;
        if (e >= 16)
            return overflow();
        if (e < -25) {
            // It comes to nothing.
            f = 0.0;
            e = 0;
        } else if (e < -14) {
            // It is on its way to nothing.
            f = std::ldexp(f, 14 + e);
            e = 0;
        } else {
            e += 15;
            f -= 1.0; // The 1 that it begins with goes without saying.
        }
        f *= 1024.0;
        // To the nearest, and to the even one of two that are as near
        bits = static_cast<unsigned short>(f);
        if ((f - bits > 0.5) || ((f - bits == 0.5) && (bits % 2 == 1))) {
            ++bits;
            if (bits == 1024) {
                bits = 0;
                ++e;
                if (e == 31)
                    return overflow();
            }
        }
    }
    bits |= (e << 10) | (sign << 15);
    storeBytes(bits, bytes, isLittleEndian);
    return true;
}

bool packFloat4(JSGlobalObject* globalObject, double x, std::span<uint8_t, 4> bytes, bool isLittleEndian)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    float y = static_cast<float>(x);
    if (std::isinf(y) && !std::isinf(x)) {
        raise(globalObject, scope, BuiltinType::OverflowError, "float too large to pack with f format"_s);
        return false;
    }
    // A signalling NaN has been made a quiet one on the way, and is made a signalling one again if there is anything else in it to tell it from an infinity by.
    if (std::isnan(x) && !(std::bit_cast<uint64_t>(x) & (1ULL << 51))) {
        uint32_t bits = std::bit_cast<uint32_t>(y);
        if (bits & 0x3fffff)
            bits &= ~(1u << 22);
        y = std::bit_cast<float>(bits);
    }
    storeBytes(y, bytes, isLittleEndian);
    return true;
}

void packFloat8(double x, std::span<uint8_t, 8> bytes, bool isLittleEndian)
{
    storeBytes(x, bytes, isLittleEndian);
}

double unpackFloat2(std::span<const uint8_t, 2> bytes, bool isLittleEndian)
{
    unsigned short bits = loadBytes<unsigned short>(bytes, isLittleEndian);
    bool sign = bits >> 15;
    int e = (bits >> 10) & 0x1f;
    unsigned f = bits & 0x3ff;
    if (e == 0x1f) {
        if (!f)
            return sign ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return std::bit_cast<double>((sign ? 0xfff0000000000000ULL : 0x7ff0000000000000ULL) + (static_cast<uint64_t>(f) << 42));
    }
    double x = static_cast<double>(f) / 1024.0;
    if (!e)
        e = -14;
    else {
        x += 1.0;
        e -= 15;
    }
    x = std::ldexp(x, e);
    return sign ? -x : x;
}

double unpackFloat4(std::span<const uint8_t, 4> bytes, bool isLittleEndian)
{
    float x = loadBytes<float>(bytes, isLittleEndian);
    // A signalling NaN stays one.
    if (std::isnan(x) && !(std::bit_cast<uint32_t>(x) & (1u << 22)))
        return std::bit_cast<double>(std::bit_cast<uint64_t>(static_cast<double>(x)) & ~(1ULL << 51));
    return x;
}

double unpackFloat8(std::span<const uint8_t, 8> bytes, bool isLittleEndian)
{
    return loadBytes<double>(bytes, isLittleEndian);
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

// JavaScript's BigInts have a largest size, and say RangeError past it. That is where there is no more room for an int.
static JSValue finishBigInt(JSGlobalObject* globalObject, ThrowScope& scope, JSValue result)
{
    if (scope.exception()) [[unlikely]] {
        if (!scope.tryClearException())
            return { };
        return raiseMemoryError(globalObject, scope);
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

static JSValue intBinaryOperation(JSGlobalObject*, ThrowScope&, BinaryOperator, const Number&, const Number&);

static bool isNegativeInt(const Number& number)
{
    return number.kind == Number::Kind::Small ? number.small < 0 : number.big->sign();
}

int64_t bitLengthOfInt(const Number& number)
{
    if (number.kind == Number::Kind::Small) {
        uint32_t magnitude = number.small < 0 ? 0u - static_cast<uint32_t>(number.small) : number.small;
        return 32 - std::countl_zero(magnitude);
    }
    unsigned length = number.big->length();
    return static_cast<int64_t>(length - 1) * 64 + (64 - std::countl_zero(static_cast<uint64_t>(number.big->digit(length - 1))));
}

// The float that is nearest to the quotient, which is not what dividing the nearest floats gives: that rounds three times, and overflows where the
// quotient would not. This is long_true_divide() of CPython's Objects/longobject.c, where why it is right is gone into at length.
static JSValue trueDivideInts(JSGlobalObject* globalObject, ThrowScope& scope, const Number& left, const Number& right)
{
    if (right.kind == Number::Kind::Small && !right.small)
        return raiseZeroDivision(globalObject, scope);
    // Both are floats exactly, so there is one rounding.
    if (left.kind == Number::Kind::Small && right.kind == Number::Kind::Small)
        return floatFromDouble(static_cast<double>(left.small) / static_cast<double>(right.small));

    bool isNegative = isNegativeInt(left) != isNegativeInt(right);
    auto signedResult = [&] (double magnitude) { return floatFromDouble(isNegative ? -magnitude : magnitude); };
    auto overflow = [&] { return raise(globalObject, scope, BuiltinType::OverflowError, "integer division result too large for a float"_s); };
    if (left.kind == Number::Kind::Small && !left.small)
        return signedResult(0);

    Number zero = classify(jsNumber(0));
    auto magnitudeOf = [&] (const Number& number) -> Number {
        return isNegativeInt(number) ? classify(intBinaryOperation(globalObject, scope, BinaryOperator::Sub, zero, number)) : number;
    };
    Number dividend = magnitudeOf(left);
    RETURN_IF_EXCEPTION(scope, { });
    Number divisor = magnitudeOf(right);
    RETURN_IF_EXCEPTION(scope, { });

    int64_t difference = bitLengthOfInt(dividend) - bitLengthOfInt(divisor);
    if (difference > std::numeric_limits<double>::max_exponent)
        return overflow();
    if (difference < std::numeric_limits<double>::min_exponent - std::numeric_limits<double>::digits - 1)
        return signedResult(0);

    // The dividend is scaled by a power of two so that the whole part of the quotient has two or three bits more than a float holds. Whether anything
    // is lost on the way is kept track of, since that decides which way a tie goes.
    int64_t shift = std::max<int64_t>(difference, std::numeric_limits<double>::min_exponent) - std::numeric_limits<double>::digits - 2;
    bool isInexact = false;
    Number scaled;
    if (shift <= 0)
        scaled = classify(intBinaryOperation(globalObject, scope, BinaryOperator::LShift, dividend, classify(jsNumber(static_cast<int32_t>(-shift)))));
    else {
        Number amount = classify(jsNumber(static_cast<int32_t>(shift)));
        scaled = classify(intBinaryOperation(globalObject, scope, BinaryOperator::RShift, dividend, amount));
        RETURN_IF_EXCEPTION(scope, { });
        Number restored = classify(intBinaryOperation(globalObject, scope, BinaryOperator::LShift, scaled, amount));
        RETURN_IF_EXCEPTION(scope, { });
        bool isUnordered = false;
        isInexact = *numberCompare(restored, dividend, isUnordered);
    }
    RETURN_IF_EXCEPTION(scope, { });
    Number quotient = classify(intBinaryOperation(globalObject, scope, BinaryOperator::FloorDiv, scaled, divisor));
    RETURN_IF_EXCEPTION(scope, { });
    Number remainder = classify(intBinaryOperation(globalObject, scope, BinaryOperator::Mod, scaled, divisor));
    RETURN_IF_EXCEPTION(scope, { });
    if (remainder.kind != Number::Kind::Small || remainder.small)
        isInexact = true;

    int64_t quotientBits = bitLengthOfInt(quotient);
    ASSERT(quotientBits <= 64);
    uint64_t bits = quotient.kind == Number::Kind::Small ? static_cast<uint64_t>(quotient.small) : static_cast<uint64_t>(quotient.big->digit(0));

    // To the nearest, and to an even one from half way.
    int64_t extraBits = std::max<int64_t>(quotientBits, std::numeric_limits<double>::min_exponent - shift) - std::numeric_limits<double>::digits;
    ASSERT(extraBits == 2 || extraBits == 3);
    uint64_t mask = 1ull << (extraBits - 1);
    bits |= isInexact;
    if ((bits & mask) && (bits & (3 * mask - 1)))
        bits += mask;
    bits &= ~(2 * mask - 1);
    double rounded = static_cast<double>(bits);

    if (shift + quotientBits >= std::numeric_limits<double>::max_exponent && (shift + quotientBits > std::numeric_limits<double>::max_exponent || rounded == std::ldexp(1.0, static_cast<int>(quotientBits))))
        return overflow();
    return signedResult(std::ldexp(rounded, static_cast<int>(shift)));
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
        // By more than there could be room for anywhere: CPython keeps count of the bits of an int in an int64_t.
        if (!a->isZero() && !b->sign() && (b->length() > 1 || b->digit(0) >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - bitLengthOfInt(classify(JSValue(a)))))
            return raise(globalObject, scope, BuiltinType::OverflowError, "too many digits in integer"_s);
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
void floatDivmod(double left, double right, double& quotient, double& remainder)
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

FloatPower powerOfFloats(double base, double exponent, double& result)
{
    result = 1.0;
    if (!exponent)
        return FloatPower::IsFloat;
    result = base;
    if (std::isnan(base))
        return FloatPower::IsFloat;
    result = base == 1.0 ? 1.0 : exponent;
    if (std::isnan(exponent))
        return FloatPower::IsFloat;
    if (!base && exponent < 0 && std::isfinite(exponent))
        return FloatPower::IsOfZero;
    if (base < 0 && std::isfinite(base) && std::isfinite(exponent) && exponent != std::floor(exponent))
        return FloatPower::IsComplex;
    result = std::pow(base, exponent);
    if (std::isinf(result) && std::isfinite(base) && std::isfinite(exponent))
        return FloatPower::IsTooLarge;
    return FloatPower::IsFloat;
}

static JSValue powerOfFloats(JSGlobalObject* globalObject, ThrowScope& scope, double base, double exponent)
{
    double result;
    switch (powerOfFloats(base, exponent, result)) {
    case FloatPower::IsFloat:
        return floatFromDouble(result);
    case FloatPower::IsOfZero:
        return raise(globalObject, scope, BuiltinType::ZeroDivisionError, "zero to a negative power"_s);
    case FloatPower::IsComplex:
        return powerOfNegativeFloat(globalObject, base, exponent);
    case FloatPower::IsTooLarge:
        return raise(globalObject, scope, BuiltinType::OverflowError, PyTuple::create(globalObject, { jsNumber(34), jsNontrivialString(globalObject->vm(), "Result too large"_s) }));
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    if (op == UnaryOperator::Invert && value.isBoolean()) [[unlikely]] {
        bool succeeded = warn(globalObject, BuiltinType::DeprecationWarning, "Bitwise inversion '~' on bool is deprecated and will be removed in Python 3.16. This returns the bitwise inversion of the underlying int object and is "
            "usually not what you expect from negating a bool. Use the 'not' operator for boolean negation or ~int(x) if you really want the bitwise inversion of the underlying int."_s);
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT_UNUSED(succeeded, succeeded);
    }
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
        // CPython hashes a NaN by which object it is, since no two are equal and a great many in one set would otherwise all collide. Here a float is a
        // value and not an object, so one NaN is another, `is` says so, and a set has room for one.
        return 0;
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
    // Writing a long int in decimal takes time that goes up as the square of its length, and a program can be given one by someone who means it harm. So there is a limit,
    // sys.set_int_max_str_digits(), and it is looked at before the work is done wherever the answer is plain from how many bits there are.
    int limit = globalObject->pyRealm()->maximumDigitsOfIntAsString;
    if (radix != 10 || !limit)
        return number.big->toString(globalObject, radix);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto raiseTooLong = [&] {
        raiseValueError(globalObject, scope, concatenate("Exceeds the limit ("_s, limit, " digits) for integer string conversion; use sys.set_int_max_str_digits() to increase the limit"_s));
        return String();
    };
    constexpr double digitsPerBit = 0.30102999566398114; // log10(2), rounded down
    if (static_cast<double>(bitLengthOfInt(number) - 1) * digitsPerBit >= limit)
        return raiseTooLong();
    String digits = number.big->toString(globalObject, 10);
    RETURN_IF_EXCEPTION(scope, { });
    if (static_cast<int>(digits.length() - number.big->sign()) > limit)
        return raiseTooLong();
    return digits;
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

    TextBuilder builder;
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
        return builder.tryFinish();
    }
    if (point <= 0) {
        builder.append("0."_s);
        for (int i = point; i < 0; ++i)
            builder.append('0');
        for (int i = 0; i < length; ++i)
            builder.append(digits[i]);
        return builder.tryFinish();
    }
    for (int i = 0; i < point; ++i)
        builder.append(digit(i));
    builder.append('.');
    if (length <= point)
        builder.append('0');
    for (int i = point; i < length; ++i)
        builder.append(digits[i]);
    return builder.tryFinish();
}

} } // namespace JSC::Python
