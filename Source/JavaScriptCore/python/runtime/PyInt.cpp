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
#include "PyModuleHelpers.h"
#include "PyStr.h"
#include "PyTypes.h"
#include <wtf/FlipBytes.h>
#include <wtf/text/StringToIntegerConversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// int and bool. (Objects/longobject.c in CPython is the reference.) The arithmetic beyond what a number holds is JSBigInt's.

PY_DEFINE_CELL(PyInt);

PyInt::PyInt(VM& vm, Structure* structure, PyType* type, JSValue value)
    : Base(vm, structure, type)
    , m_value(value, WriteBarrierEarlyInit)
{
}

PyInt* PyInt::create(VM& vm, Structure* structure, PyType* type, JSValue value)
{
    ASSERT(isExactInt(value));
    auto* object = new (NotNull, allocateCell<PyInt>(vm)) PyInt(vm, structure, type, value);
    object->finishCreation(vm);
    return object;
}

template<typename Visitor>
void PyInt::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyInt>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_value);
}

DEFINE_VISIT_CHILDREN(PyInt);

// ---- Representation ----

// JSBigInt::createFrom() asks whether an exception is pending. That question is also where a termination that has been asked for is thrown,
// and then it returns nothing. Who wants an int gets one all the same, and comes upon the termination in time.
static NEVER_INLINE JSBigInt* makeBigIntWhileTerminating(VM& vm, uint64_t magnitude, bool isNegative)
{
    RELEASE_ASSERT(vm.hasPendingTerminationException());
    JSBigInt* result = JSBigInt::tryCreateWithLength(vm, 1);
    RELEASE_ASSERT(result);
    result->setDigit(0, magnitude);
    result->setSign(isNegative);
    return result;
}

// Of one digit. Like any small cell, it is there or the process is out of memory.
template<typename Integer>
static ALWAYS_INLINE JSValue makeBigIntFrom(JSGlobalObject* globalObject, Integer value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSBigInt* result = JSBigInt::createFrom(globalObject, value);
    EXCEPTION_ASSERT_UNUSED(scope, !scope.exception() || vm.hasPendingTerminationException());
    if (!result) [[unlikely]]
        return makeBigIntWhileTerminating(vm, value < 0 ? -static_cast<uint64_t>(value) : static_cast<uint64_t>(value), value < 0);
    return result;
}

JSValue makeBigInt(JSGlobalObject* globalObject, int64_t value) { return makeBigIntFrom(globalObject, value); }
JSValue makeBigInt(JSGlobalObject* globalObject, uint64_t value) { return makeBigIntFrom(globalObject, value); }

JSValue normalizeInt(JSValue value)
{
    if (!value || !value.isHeapBigInt())
        return value;
    JSBigInt* bigInt = value.asHeapBigInt();
    if (bigInt->length() > 1)
        return value;
    if (!bigInt->length())
        return jsNumber(0);
    uint64_t digit = bigInt->digit(0);
    if (digit > static_cast<uint64_t>(maxSmallInt64))
        return value;
    int64_t result = static_cast<int64_t>(digit);
    return smallInt(bigInt->sign() ? -result : result);
}

JSBigInt* toBigInt(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isHeapBigInt())
        return value.asHeapBigInt();
    return JSBigInt::createFrom(globalObject, smallIntValue(value));
}

JSValue intFromDouble(JSGlobalObject* globalObject, double value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (std::isnan(value))
        return raiseValueError(globalObject, scope, "cannot convert float NaN to integer"_s);
    if (std::isinf(value))
        return raiseOverflowError(globalObject, scope, "cannot convert float infinity to integer"_s);
    value = std::trunc(value);
    if (std::abs(value) <= maxSmallInt)
        return jsNumber(value + 0.0);
    RELEASE_AND_RETURN(scope, JSBigInt::createFrom(globalObject, value));
}

std::optional<int64_t> tryInt64(JSValue value)
{
    if (isSmallInt(value))
        return smallIntValue(value);
    return JSBigInt::tryToBigInt64(value);
}

std::optional<uint64_t> tryUInt64(JSValue value)
{
    if (isSmallInt(value)) {
        int64_t result = smallIntValue(value);
        if (result < 0)
            return std::nullopt;
        return static_cast<uint64_t>(result);
    }
    return JSBigInt::tryToBigUInt64(value);
}

double intToDouble(JSValue value)
{
    if (value.isNumber())
        return numberValue(value);
    return JSBigInt::toNumberHeap(value.asHeapBigInt()).asNumber();
}

int intSign(JSValue value)
{
    if (value.isNumber()) {
        double number = value.asNumber();
        return number < 0 ? -1 : number > 0;
    }
    JSBigInt* bigInt = value.asHeapBigInt();
    return bigInt->isZero() ? 0 : bigInt->sign() ? -1 : 1;
}

bool intIsOdd(JSValue value)
{
    if (isSmallInt(value))
        return smallIntValue(value) & 1;
    JSBigInt* bigInt = value.asHeapBigInt();
    return bigInt->length() && (bigInt->digit(0) & 1);
}

uint64_t intBitLength(JSValue value)
{
    if (isSmallInt(value)) {
        int64_t number = smallIntValue(value);
        uint64_t magnitude = number < 0 ? -static_cast<uint64_t>(number) : number;
        return magnitude ? 64 - std::countl_zero(magnitude) : 0;
    }
    // (A BigInt of JS's may be 0n.)
    return value.asHeapBigInt()->length() ? value.asHeapBigInt()->bitLength() : 0;
}

int compareInts(JSValue a, JSValue b)
{
    if (a.isNumber() && b.isNumber()) {
        double x = a.asNumber();
        double y = b.asNumber();
        return x < y ? -1 : x > y;
    }
    JSBigInt::ComparisonResult result;
    if (a.isHeapBigInt() && b.isHeapBigInt())
        result = JSBigInt::compare(a.asHeapBigInt(), b.asHeapBigInt());
    else if (a.isHeapBigInt())
        result = JSBigInt::compare(a.asHeapBigInt(), smallIntValue(b));
    else {
        result = JSBigInt::compare(b.asHeapBigInt(), smallIntValue(a));
        if (result == JSBigInt::ComparisonResult::LessThan)
            return 1;
        if (result == JSBigInt::ComparisonResult::GreaterThan)
            return -1;
        return 0;
    }
    return result == JSBigInt::ComparisonResult::LessThan ? -1 : result == JSBigInt::ComparisonResult::GreaterThan ? 1 : 0;
}

static constexpr uint64_t hashModulus = (static_cast<uint64_t>(1) << 61) - 1;

// The value modulo 2**61 - 1, with its sign: equal numbers of any type hash alike.
int64_t hashOfInt(JSValue value)
{
    if (isSmallInt(value)) {
        int64_t number = smallIntValue(value);
        return number == -1 ? -2 : number;
    }
    JSBigInt* bigInt = value.asHeapBigInt();
    uint64_t result = 0;
    for (unsigned i = bigInt->length(); i--;) {
        // 2**64 is 8 modulo 2**61 - 1.
        UInt128 next = static_cast<UInt128>(result) * 8 + bigInt->digit(i) % hashModulus;
        result = static_cast<uint64_t>(next % hashModulus);
    }
    int64_t hash = static_cast<int64_t>(result);
    if (bigInt->sign())
        hash = -hash;
    return hash == -1 ? -2 : hash;
}

String intToString(JSGlobalObject* globalObject, JSValue value, unsigned radix)
{
    if (isSmallInt(value)) {
        int64_t number = smallIntValue(value);
        if (radix == 10)
            return String::number(number);
        uint64_t magnitude = number < 0 ? -static_cast<uint64_t>(number) : number;
        char buffer[66];
        unsigned position = sizeof(buffer);
        do {
            buffer[--position] = "0123456789abcdefghijklmnopqrstuvwxyz"[magnitude % radix];
            magnitude /= radix;
        } while (magnitude);
        if (number < 0)
            buffer[--position] = '-';
        return String(byteCast<Latin1Character>(std::span<const char> { buffer + position, sizeof(buffer) - position }));
    }

    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSBigInt* bigInt = value.asHeapBigInt();
    int limit = runtime(globalObject).maxStrDigits;
    bool isLimited = limit > 0 && (radix & (radix - 1));
    // The limit is there so that no time is spent on a number that is too long: its length in bits says how many digits it has
    // at the least.
    if (isLimited && std::floor((bigInt->bitLength() - 1) * (std::numbers::ln2 / std::log(radix)) - 1e-5) + 1 > limit) {
        raiseValueError(globalObject, scope, "Exceeds the limit ("_s, limit, " digits) for integer string conversion; use sys.set_int_max_str_digits() to increase the limit"_s);
        return { };
    }
    String result = bigInt->toString(globalObject, radix);
    RETURN_IF_EXCEPTION(scope, { });
    if (isLimited && result.length() - bigInt->sign() > static_cast<unsigned>(limit)) {
        raiseValueError(globalObject, scope, "Exceeds the limit ("_s, limit, " digits) for integer string conversion; use sys.set_int_max_str_digits() to increase the limit"_s);
        return { };
    }
    return result;
}

JSValue intFromDigits(JSGlobalObject* globalObject, StringView text, unsigned radix)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What surely fits in 53 bits.
    if (text.length() <= 10) {
        int64_t result = 0;
        for (unsigned i = 0; i < text.length(); ++i)
            result = result * radix + alphanumericDigitValue(text[i]);
        if (fitsSmallInt(result))
            return smallInt(result);
    }
    JSValue result = JSBigInt::parseInt(globalObject, vm, text, radix, JSBigInt::ErrorParseMode::ThrowExceptions, JSBigInt::ParseIntSign::Unsigned);
    RETURN_IF_EXCEPTION(scope, { });
    return normalizeInt(result);
}

// ---- Arithmetic ----

#define PY_BIGINT_OPERATION(function) \
    do { \
        VM& vm = getVM(globalObject); \
        auto scope = DECLARE_THROW_SCOPE(vm); \
        JSBigInt* x = toBigInt(globalObject, a); \
        RETURN_IF_EXCEPTION(scope, { }); \
        JSBigInt* y = toBigInt(globalObject, b); \
        RETURN_IF_EXCEPTION(scope, { }); \
        JSValue result = JSBigInt::function(globalObject, x, y); \
        RETURN_IF_EXCEPTION(scope, { }); \
        return normalizeInt(result); \
    } while (false)

JSValue intAdd(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    if (isSmallInt(a) && isSmallInt(b)) [[likely]]
        return makeInt(globalObject, smallIntValue(a) + smallIntValue(b));
    PY_BIGINT_OPERATION(add);
}

JSValue intSubtract(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    if (isSmallInt(a) && isSmallInt(b)) [[likely]]
        return makeInt(globalObject, smallIntValue(a) - smallIntValue(b));
    PY_BIGINT_OPERATION(sub);
}

JSValue intMultiply(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    if (isSmallInt(a) && isSmallInt(b)) [[likely]] {
        int64_t result;
        if (!__builtin_mul_overflow(smallIntValue(a), smallIntValue(b), &result))
            return makeInt(globalObject, result);
    }
    PY_BIGINT_OPERATION(multiply);
}

bool intDivMod(JSGlobalObject* globalObject, JSValue a, JSValue b, JSValue& quotient, JSValue& remainder)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!intSign(b)) {
        raiseZeroDivisionError(globalObject, scope, "division by zero"_s);
        return false;
    }
    if (isSmallInt(a) && isSmallInt(b)) [[likely]] {
        int64_t x = smallIntValue(a);
        int64_t y = smallIntValue(b);
        int64_t q = x / y;
        int64_t r = x % y;
        // The quotient is rounded down, so that the remainder has the sign of the divisor.
        if (r && ((r < 0) != (y < 0))) {
            --q;
            r += y;
        }
        quotient = smallInt(q);
        remainder = smallInt(r);
        return true;
    }
    JSBigInt* x = toBigInt(globalObject, a);
    RETURN_IF_EXCEPTION(scope, false);
    JSBigInt* y = toBigInt(globalObject, b);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue q = JSBigInt::divide(globalObject, x, y);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue r = JSBigInt::remainder(globalObject, x, y);
    RETURN_IF_EXCEPTION(scope, false);
    q = normalizeInt(q);
    r = normalizeInt(r);
    int remainderSign = intSign(r);
    if (remainderSign && (remainderSign < 0) != (intSign(b) < 0)) {
        q = intSubtract(globalObject, q, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, false);
        r = intAdd(globalObject, r, b);
        RETURN_IF_EXCEPTION(scope, false);
    }
    quotient = q;
    remainder = r;
    return true;
}

JSValue intFloorDivide(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue quotient, remainder;
    if (failed(scope, intDivMod(globalObject, a, b, quotient, remainder)))
        return { };
    return quotient;
}

JSValue intModulo(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue quotient, remainder;
    if (failed(scope, intDivMod(globalObject, a, b, quotient, remainder)))
        return { };
    return remainder;
}

JSValue intTrueDivide(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!intSign(b))
        return raiseZeroDivisionError(globalObject, scope, "division by zero"_s);
    // Both are exact as doubles, so the quotient is correctly rounded.
    if (a.isNumber() && b.isNumber()) [[likely]]
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, numberValue(a) / numberValue(b)));
    if (!intSign(a))
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, intSign(b) < 0 ? -0.0 : 0.0));

    // Scale so that the integer quotient has a couple of bits more than a double holds, then round once.
    bool negative = (intSign(a) < 0) != (intSign(b) < 0);
    JSValue x = intAbsolute(globalObject, a);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue y = intAbsolute(globalObject, b);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t difference = static_cast<int64_t>(intBitLength(x)) - static_cast<int64_t>(intBitLength(y));
    if (difference > std::numeric_limits<double>::max_exponent)
        return raiseOverflowError(globalObject, scope, "integer division result too large for a float"_s);
    if (difference < std::numeric_limits<double>::min_exponent - std::numeric_limits<double>::digits - 1)
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, negative ? -0.0 : 0.0));
    int64_t shift = std::max<int64_t>(difference, std::numeric_limits<double>::min_exponent) - std::numeric_limits<double>::digits - 2;
    bool inexact = false;
    if (shift <= 0) {
        x = intLeftShift(globalObject, x, jsNumber(static_cast<double>(-shift)));
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        JSValue shifted = intRightShift(globalObject, x, jsNumber(static_cast<double>(shift)));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue restored = intLeftShift(globalObject, shifted, jsNumber(static_cast<double>(shift)));
        RETURN_IF_EXCEPTION(scope, { });
        inexact = compareInts(restored, x);
        x = shifted;
    }
    JSValue quotient, remainder;
    intDivMod(globalObject, x, y, quotient, remainder);
    RETURN_IF_EXCEPTION(scope, { });
    if (intSign(remainder))
        inexact = true;
    uint64_t bits = *tryUInt64(quotient);
    // Round to 53 bits, to even on a tie; what was lost above breaks a tie upwards.
    int extra = static_cast<int>(64 - std::countl_zero(bits)) - std::numeric_limits<double>::digits;
    // Results below the normal range have fewer bits.
    int64_t resultExponent = shift + static_cast<int64_t>(64 - std::countl_zero(bits));
    if (resultExponent < std::numeric_limits<double>::min_exponent)
        extra += static_cast<int>(std::numeric_limits<double>::min_exponent - resultExponent);
    ASSERT(extra > 0);
    uint64_t mask = (static_cast<uint64_t>(1) << extra) - 1;
    uint64_t low = bits & mask;
    uint64_t half = static_cast<uint64_t>(1) << (extra - 1);
    bits &= ~mask;
    if (low > half || (low == half && (inexact || (bits & (mask + 1)))))
        bits += mask + 1;
    double result = std::ldexp(static_cast<double>(bits), static_cast<int>(shift));
    if (std::isinf(result))
        return raiseOverflowError(globalObject, scope, "integer division result too large for a float"_s);
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, negative ? -result : result));
}

// The operand is the result. JS, which may have encoded a 0 as -0.0, can tell the two apart.
static ALWAYS_INLINE JSValue sameInt(JSValue value) { return value.isDouble() ? jsNumber(numberValue(value)) : value; }

JSValue intNegate(JSGlobalObject* globalObject, JSValue a)
{
    if (isSmallInt(a)) [[likely]]
        return smallInt(-smallIntValue(a));
    return normalizeInt(JSBigInt::unaryMinus(globalObject, a.asHeapBigInt()));
}

JSValue intAbsolute(JSGlobalObject* globalObject, JSValue a)
{
    return intSign(a) < 0 ? intNegate(globalObject, a) : sameInt(a);
}

JSValue intInvert(JSGlobalObject* globalObject, JSValue a)
{
    if (isSmallInt(a)) [[likely]]
        return makeInt(globalObject, ~smallIntValue(a));
    return normalizeInt(JSBigInt::bitwiseNot(globalObject, a.asHeapBigInt()));
}

JSValue intAnd(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    if (isSmallInt(a) && isSmallInt(b)) [[likely]]
        return makeInt(globalObject, smallIntValue(a) & smallIntValue(b));
    PY_BIGINT_OPERATION(bitwiseAnd);
}

JSValue intOr(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    if (isSmallInt(a) && isSmallInt(b)) [[likely]]
        return makeInt(globalObject, smallIntValue(a) | smallIntValue(b));
    PY_BIGINT_OPERATION(bitwiseOr);
}

JSValue intXor(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    if (isSmallInt(a) && isSmallInt(b)) [[likely]]
        return makeInt(globalObject, smallIntValue(a) ^ smallIntValue(b));
    PY_BIGINT_OPERATION(bitwiseXor);
}

// As many as a JSBigInt holds.
static constexpr int64_t maxIntBits = 1 << 30;

JSValue intLeftShift(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A BigInt is taken for more than a small int holds below. One that JS has made may be small.
    if (!isSmallInt(b)) [[unlikely]]
        b = normalizeInt(b);
    if (intSign(b) < 0)
        return raiseValueError(globalObject, scope, "negative shift count"_s);
    if (!intSign(a))
        return jsNumber(0);
    if (isSmallInt(a) && isSmallInt(b)) {
        int64_t shift = smallIntValue(b);
        if (shift + static_cast<int64_t>(intBitLength(a)) <= 62)
            RELEASE_AND_RETURN(scope, makeInt(globalObject, static_cast<int64_t>(static_cast<uint64_t>(smallIntValue(a)) << shift)));
    }
    if (!isSmallInt(b) || smallIntValue(b) > maxIntBits)
        return raiseOverflowError(globalObject, scope, "too many digits in integer"_s);
    scope.release();
    PY_BIGINT_OPERATION(leftShift);
}

JSValue intRightShift(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // As above.
    if (!isSmallInt(b)) [[unlikely]]
        b = normalizeInt(b);
    if (intSign(b) < 0)
        return raiseValueError(globalObject, scope, "negative shift count"_s);
    if (isSmallInt(a)) {
        int64_t value = smallIntValue(a);
        if (!isSmallInt(b) || smallIntValue(b) >= 63)
            return jsNumber(value < 0 ? -1 : 0);
        return smallInt(value >> smallIntValue(b));
    }
    if (!isSmallInt(b))
        return jsNumber(intSign(a) < 0 ? -1 : 0);
    scope.release();
    PY_BIGINT_OPERATION(signedRightShift);
}

// a**-1 modulo n, or an empty value with no exception if there is none.
static JSValue modularInverse(JSGlobalObject* globalObject, JSValue a, JSValue n)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The extended Euclidean algorithm.
    JSValue b = jsNumber(1);
    JSValue c = jsNumber(0);
    while (intSign(n)) {
        JSValue q, r;
        intDivMod(globalObject, a, n, q, r);
        RETURN_IF_EXCEPTION(scope, { });
        a = n;
        n = r;
        JSValue t = intMultiply(globalObject, q, c);
        RETURN_IF_EXCEPTION(scope, { });
        t = intSubtract(globalObject, b, t);
        RETURN_IF_EXCEPTION(scope, { });
        b = c;
        c = t;
    }
    if (compareInts(a, jsNumber(1)))
        return { };
    return b;
}

JSValue intPower(JSGlobalObject* globalObject, JSValue base, JSValue exponent, JSValue modulus)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    // As for the shifts.
    if (!isSmallInt(base) || !isSmallInt(exponent)) [[unlikely]] {
        base = normalizeInt(base);
        exponent = normalizeInt(exponent);
    }

    if (modulus) {
        if (!intSign(modulus))
            return raiseValueError(globalObject, scope, "pow() 3rd argument cannot be 0"_s);
        bool negativeModulus = intSign(modulus) < 0;
        JSValue m = intAbsolute(globalObject, modulus);
        RETURN_IF_EXCEPTION(scope, { });
        if (!compareInts(m, jsNumber(1)))
            return jsNumber(0);
        if (intSign(exponent) < 0) {
            exponent = intNegate(globalObject, exponent);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue reduced = intModulo(globalObject, base, m);
            RETURN_IF_EXCEPTION(scope, { });
            base = modularInverse(globalObject, reduced, m);
            RETURN_IF_EXCEPTION(scope, { });
            if (!base)
                return raiseValueError(globalObject, scope, "base is not invertible for the given modulus"_s);
        }
        base = intModulo(globalObject, base, m);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue result = jsNumber(1);
        uint64_t bits = intBitLength(exponent);
        for (uint64_t i = bits; i--;) {
            result = intMultiply(globalObject, result, result);
            RETURN_IF_EXCEPTION(scope, { });
            result = intModulo(globalObject, result, m);
            RETURN_IF_EXCEPTION(scope, { });
            bool bit;
            if (isSmallInt(exponent))
                bit = (smallIntValue(exponent) >> i) & 1;
            else
                bit = (exponent.asHeapBigInt()->digit(i / 64) >> (i % 64)) & 1;
            if (bit) {
                result = intMultiply(globalObject, result, base);
                RETURN_IF_EXCEPTION(scope, { });
                result = intModulo(globalObject, result, m);
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        if (negativeModulus && intSign(result))
            RELEASE_AND_RETURN(scope, intSubtract(globalObject, result, m));
        return result;
    }

    if (intSign(exponent) < 0) {
        // A fraction: the result is a float.
        if (!intSign(base))
            return raiseZeroDivisionError(globalObject, scope, "zero to a negative power"_s);
        double x = toDouble(globalObject, base);
        RETURN_IF_EXCEPTION(scope, { });
        double y = toDouble(globalObject, exponent);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, std::pow(x, y)));
    }

    if (isSmallInt(base) && isSmallInt(exponent)) {
        int64_t x = smallIntValue(base);
        int64_t n = smallIntValue(exponent);
        if (x == 0 || x == 1)
            return n ? base : jsNumber(1);
        if (x == -1)
            return jsNumber(n & 1 ? -1 : 1);
        int64_t result = 1;
        bool overflowed = false;
        while (n) {
            if ((n & 1) && __builtin_mul_overflow(result, x, &result)) {
                overflowed = true;
                break;
            }
            n >>= 1;
            if (n && __builtin_mul_overflow(x, x, &x)) {
                overflowed = true;
                break;
            }
        }
        if (!overflowed)
            RELEASE_AND_RETURN(scope, makeInt(globalObject, result));
    }
    if (!isSmallInt(exponent)) {
        if (isSmallInt(base) && std::abs(smallIntValue(base)) <= 1)
            return !smallIntValue(base) ? jsNumber(0) : smallIntValue(base) == 1 || !intIsOdd(exponent) ? jsNumber(1) : jsNumber(-1);
        return raiseOverflowError(globalObject, scope, "too many digits in integer"_s);
    }
    JSValue a = base;
    JSValue b = exponent;
    scope.release();
    PY_BIGINT_OPERATION(exponentiate);
}

// ---- The methods of int ----

// The operand as an exact int, or empty if it is not an int at all.
static ALWAYS_INLINE JSValue operand(JSValue value)
{
    if (isExactInt(value)) [[likely]]
        return value;
    return isInt(value) ? intValue(value) : JSValue();
}

#define PY_INT_BINARY(name, function) \
    static JSValue name(JSGlobalObject* globalObject, JSValue self, JSValue other) \
    { \
        JSValue b = operand(other); \
        if (!b) \
            return runtime(globalObject).notImplemented(); \
        return function(globalObject, intValue(self), b); \
    } \
    static JSValue name##Reflected(JSGlobalObject* globalObject, JSValue self, JSValue other) \
    { \
        JSValue a = operand(other); \
        if (!a) \
            return runtime(globalObject).notImplemented(); \
        return function(globalObject, a, intValue(self)); \
    }

PY_INT_BINARY(intAddMethod, intAdd)
PY_INT_BINARY(intSubMethod, intSubtract)
PY_INT_BINARY(intMulMethod, intMultiply)
PY_INT_BINARY(intFloorDivMethod, intFloorDivide)
PY_INT_BINARY(intModMethod, intModulo)
PY_INT_BINARY(intTrueDivMethod, intTrueDivide)
PY_INT_BINARY(intLShiftMethod, intLeftShift)
PY_INT_BINARY(intRShiftMethod, intRightShift)
PY_INT_BINARY(intAndMethod, intAnd)
PY_INT_BINARY(intOrMethod, intOr)
PY_INT_BINARY(intXorMethod, intXor)

static JSValue intDivModTuple(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue quotient, remainder;
    if (failed(scope, intDivMod(globalObject, a, b, quotient, remainder)))
        return { };
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { quotient, remainder }));
}
PY_INT_BINARY(intDivModMethod, intDivModTuple)

static JSValue intPowMethod(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue modulus)
{
    JSValue b = operand(other);
    if (!b)
        return runtime(globalObject).notImplemented();
    JSValue m;
    if (modulus && !isNone(modulus)) {
        m = operand(modulus);
        if (!m)
            return runtime(globalObject).notImplemented();
    }
    return intPower(globalObject, intValue(self), b, m);
}

static JSValue intPowMethodReflected(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue modulus)
{
    JSValue a = operand(other);
    if (!a)
        return runtime(globalObject).notImplemented();
    JSValue m;
    if (modulus && !isNone(modulus)) {
        m = operand(modulus);
        if (!m)
            return runtime(globalObject).notImplemented();
    }
    return intPower(globalObject, a, intValue(self), m);
}

template<CompareOperator op>
static JSValue intCompareMethod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    JSValue b = operand(other);
    if (!b)
        return runtime(globalObject).notImplemented();
    int order = compareInts(intValue(self), b);
    switch (op) {
    case CompareOperator::Eq:
        return jsBoolean(!order);
    case CompareOperator::NotEq:
        return jsBoolean(!!order);
    case CompareOperator::Lt:
        return jsBoolean(order < 0);
    case CompareOperator::LtE:
        return jsBoolean(order <= 0);
    case CompareOperator::Gt:
        return jsBoolean(order > 0);
    case CompareOperator::GtE:
        return jsBoolean(order >= 0);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static JSValue intNegMethod(JSGlobalObject* globalObject, JSValue self) { return intNegate(globalObject, intValue(self)); }
static JSValue intAbsMethod(JSGlobalObject* globalObject, JSValue self) { return intAbsolute(globalObject, intValue(self)); }
static JSValue intInvertMethod(JSGlobalObject* globalObject, JSValue self) { return intInvert(globalObject, intValue(self)); }
static JSValue intIdentity(JSGlobalObject*, JSValue self) { return sameInt(intValue(self)); }
static JSValue intBoolMethod(JSGlobalObject*, JSValue self) { return jsBoolean(!!intSign(intValue(self))); }
static int64_t intHashMethod(JSGlobalObject*, JSValue self) { return hashOfInt(intValue(self)); }

static JSValue intFloatMethod(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double value = toDouble(globalObject, intValue(self));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, value));
}

static JSValue intReprMethod(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = intToString(globalObject, intValue(self));
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, text);
}

static JSValue intFormatMethod(JSGlobalObject* globalObject, JSValue self, JSValue spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(spec))
        return raiseTypeError(globalObject, scope, "__format__() argument must be str, not "_s, typeNameOfArgument(globalObject, spec));
    RELEASE_AND_RETURN(scope, formatInt(globalObject, self, strValue(spec)));
}

// round(n, -2) rounds to hundreds, to even on a tie.
static JSValue intRoundMethod(JSGlobalObject* globalObject, JSValue self, JSValue digits)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = sameInt(intValue(self));
    if (!digits || isNone(digits))
        return value;
    JSValue count = toIndex(globalObject, digits);
    RETURN_IF_EXCEPTION(scope, { });
    if (intSign(count) >= 0)
        return value;
    count = intNegate(globalObject, count);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue unit = intPower(globalObject, jsNumber(10), count);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue quotient, remainder;
    intDivMod(globalObject, value, unit, quotient, remainder);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue twice = intMultiply(globalObject, remainder, jsNumber(2));
    RETURN_IF_EXCEPTION(scope, { });
    int order = compareInts(twice, unit);
    JSValue result = intSubtract(globalObject, value, remainder);
    RETURN_IF_EXCEPTION(scope, { });
    if (order > 0 || (!order && intIsOdd(quotient)))
        RELEASE_AND_RETURN(scope, intAdd(globalObject, result, unit));
    return result;
}

static JSValue intBitLengthMethod(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, intBitLength(intValue(self))); }

// What it takes in CPython: __basicsize__, and __itemsize__ for each digit of sys.int_info.bits_per_digit bits.
static JSValue intSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    uint64_t digits = std::max<uint64_t>((intBitLength(intValue(self)) + 29) / 30, 1);
    return makeInt(globalObject, 24 + 4 * digits);
}

static JSValue intBitCountMethod(JSGlobalObject* globalObject, JSValue self)
{
    JSValue value = intValue(self);
    if (isSmallInt(value)) {
        int64_t number = smallIntValue(value);
        return jsNumber(std::popcount(number < 0 ? -static_cast<uint64_t>(number) : static_cast<uint64_t>(number)));
    }
    uint64_t count = 0;
    for (auto digit : static_cast<const JSBigInt*>(value.asHeapBigInt())->digits())
        count += std::popcount(digit);
    return makeInt(globalObject, count);
}

static JSValue intAsIntegerRatio(JSGlobalObject* globalObject, JSValue self) { return PyTuple::create(globalObject, { intValue(self), jsNumber(1) }); }
static JSValue intIsInteger(JSGlobalObject*, JSValue) { return jsBoolean(true); }
static JSValue intGetNewArgs(JSGlobalObject* globalObject, JSValue self) { return PyTuple::create(globalObject, { intValue(self) }); }
static JSValue intZero(JSGlobalObject*, JSValue) { return jsNumber(0); }
static JSValue intOne(JSGlobalObject*, JSValue) { return jsNumber(1); }

// The arguments are looked at in two rounds, as in CPython: are they of the right kind, all of them, and then what do they say.
static bool checkByteOrder(JSGlobalObject* globalObject, ASCIILiteral function, JSValue byteOrder)
{
    if (!byteOrder || isStr(byteOrder))
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, function, "() argument 'byteorder' must be str, not "_s, typeNameOfArgument(globalObject, byteOrder));
    return false;
}

static bool parseByteOrder(JSGlobalObject* globalObject, JSValue byteOrder, bool& littleEndian)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    littleEndian = false;
    if (!byteOrder)
        return true;
    String text = strValue(byteOrder)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (text == "little"_s)
        littleEndian = true;
    else if (text != "big"_s) {
        raiseValueError(globalObject, scope, "byteorder must be either 'little' or 'big'"_s);
        return false;
    }
    return true;
}

static JSValue intToBytes(JSGlobalObject* globalObject, JSValue self, JSValue lengthValue, JSValue byteOrder, JSValue isSignedValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t length = 1;
    if (lengthValue) {
        if (failed(scope, ssizeArgument(globalObject, lengthValue, length)))
            return { };
        if (length < 0)
            return raiseValueError(globalObject, scope, "length cannot be negative"_s);
    }
    if (failed(scope, checkByteOrder(globalObject, "to_bytes"_s, byteOrder)))
        return { };
    bool isSigned = false;
    if (isSignedValue) {
        isSigned = isTrue(globalObject, isSignedValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool littleEndian;
    if (failed(scope, parseByteOrder(globalObject, byteOrder, littleEndian)))
        return { };
    // The bytes are made before the int is looked at.
    Runtime& rt = runtime(globalObject);
    if (static_cast<uint64_t>(length) > maxLengthOfBytes)
        return raiseOverflowError(globalObject, scope, "byte string is too large"_s);
    PyBytes* result = length ? PyBytes::tryCreateUninitialized(vm, rt.structure<PyBytes>(), rt.bytesType(), length) : rt.emptyBytes();
    if (!result)
        return raiseMemoryError(globalObject, scope, String());

    JSValue value = intValue(self);
    bool negative = intSign(value) < 0;
    if (negative && !isSigned)
        return raiseOverflowError(globalObject, scope, "can't convert negative int to unsigned"_s);

    // In two's complement, the least significant byte first.
    uint64_t bits = intBitLength(value);
    JSBigInt* magnitude = toBigInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    size_t magnitudeBytes = (bits + 7) / 8;
    if (magnitudeBytes > static_cast<size_t>(length))
        return raiseOverflowError(globalObject, scope, "int too big to convert"_s);
    uint8_t* data = result->mutableData();
    unsigned carry = 1;
    bool fits = true;
    for (int64_t i = 0; fits && i < length; ++i) {
        uint8_t byte = static_cast<size_t>(i) < magnitudeBytes ? static_cast<uint8_t>(magnitude->digit(i / 8) >> ((i % 8) * 8)) : 0;
        if (negative) {
            unsigned inverted = static_cast<uint8_t>(~byte) + carry;
            byte = static_cast<uint8_t>(inverted);
            carry = inverted >> 8;
        }
        data[littleEndian ? i : length - 1 - i] = byte;
    }
    if (fits && isSigned && length) {
        // The top bit is the sign.
        bool top = data[littleEndian ? length - 1 : 0] & 0x80;
        if (top != negative)
            fits = false;
    } else if (fits && isSigned && !length && intSign(value))
        fits = false;
    if (!fits)
        return raiseOverflowError(globalObject, scope, "int too big to convert"_s);
    return result;
}

JSValue intFromByteSpan(JSGlobalObject* globalObject, std::span<const uint8_t> allBytes, bool littleEndian, bool isSigned)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (allBytes.empty())
        return jsNumber(0);
    auto fromTheTop = [=](size_t i) { return allBytes[littleEndian ? allBytes.size() - 1 - i : i]; };
    bool negative = isSigned && (fromTheTop(0) & 0x80);
    // All but one of the bytes at the upper end that say nothing are left out.
    size_t idle = 0;
    for (uint8_t padding = negative ? 0xff : 0; idle + 1 < allBytes.size() && fromTheTop(idle) == padding && fromTheTop(idle + 1) == padding;)
        ++idle;
    std::span<const uint8_t> bytes = littleEndian ? allBytes.first(allBytes.size() - idle) : allBytes.subspan(idle);
    if (bytes.size() > maxIntBits / 8 + 1) [[unlikely]]
        return raiseOverflowError(globalObject, scope, "too many digits in integer"_s);

    // A word at a time, from the lower end.
    unsigned bitsOfLastWord = bytes.size() % 8 * 8;
    Vector<uint64_t, 8> words;
    words.reserveInitialCapacity((bytes.size() + 7) / 8);
    constexpr bool hostIsLittleEndian = std::endian::native == std::endian::little;
    while (bytes.size() >= sizeof(uint64_t)) {
        uint64_t word;
        memcpySpan(asMutableByteSpan(word), littleEndian ? bytes.first<sizeof(uint64_t)>() : bytes.last<sizeof(uint64_t)>());
        words.append(littleEndian == hostIsLittleEndian ? word : flipBytes(word));
        if (littleEndian)
            skip(bytes, sizeof(uint64_t));
        else
            dropLast(bytes, sizeof(uint64_t));
    }
    if (!bytes.empty()) {
        uint64_t word = 0;
        if (littleEndian) {
            for (uint8_t byte : bytes | std::views::reverse)
                word = word << 8 | byte;
        } else {
            for (uint8_t byte : bytes)
                word = word << 8 | byte;
        }
        words.append(word);
    }
    if (negative) {
        // What is above the sign is like the sign. Then the magnitude is the complement and one more.
        if (bitsOfLastWord)
            words.last() |= ~0ull << bitsOfLastWord;
        bool carry = true;
        for (uint64_t& word : words) {
            word = ~word + carry;
            carry = carry && !word;
        }
    }
    while (!words.isEmpty() && !words.last())
        words.removeLast();
    if (words.isEmpty())
        return jsNumber(0);
    JSBigInt* result = JSBigInt::createFromWords(globalObject, words.span(), negative);
    RETURN_IF_EXCEPTION(scope, { });
    return normalizeInt(result);
}

static JSValue intFromBytes(JSGlobalObject* globalObject, JSValue cls, JSValue bytesValue, JSValue byteOrder, JSValue isSignedValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkByteOrder(globalObject, "from_bytes"_s, byteOrder)))
        return { };
    bool isSigned = false;
    if (isSignedValue) {
        isSigned = isTrue(globalObject, isSignedValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool littleEndian;
    if (failed(scope, parseByteOrder(globalObject, byteOrder, littleEndian)))
        return { };
    JSValue bytesObject;
    std::span<const uint8_t> bytes;
    if (isBytesLike(bytesValue)) {
        if (failed(scope, getBytesLike(globalObject, bytesValue, bytes)))
            return { };
    } else {
        bytesObject = bytesFromObject(globalObject, bytesValue);
        RETURN_IF_EXCEPTION(scope, { });
        bytes = PyBytes::from(bytesObject)->span();
    }
    JSValue result = intFromByteSpan(globalObject, bytes, littleEndian, isSigned);
    ensureStillAliveHere(bytesObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (cls.asCell() == runtime(globalObject).intType())
        return result;
    RELEASE_AND_RETURN(scope, call(globalObject, cls, result));
}

// ---- int() ----

// The text of an int in the given base (0: as a literal says). Empty with no exception: it is not one.
JSValue intFromText(JSGlobalObject* globalObject, StringView text, int base, NumberTextOf textOf)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Whatever is a decimal digit anywhere counts as the ASCII digit, also where a prefix or a leading zero is looked for.
    if (!text.is8Bit()) [[unlikely]] {
        Vector<Latin1Character, 64> ascii;
        if (text.length() > ascii.capacity() && !ascii.tryReserveInitialCapacity(text.length())) [[unlikely]]
            return raiseMemoryError(globalObject, scope, String());
        for (char32_t character : text.codePoints()) {
            if (character >= 0x80) {
                int digit = decimalDigitValue(character);
                character = digit >= 0 ? '0' + digit : isPythonWhitespace(character) ? ' ' : '?';
            }
            ascii.append(static_cast<Latin1Character>(character));
        }
        RELEASE_AND_RETURN(scope, intFromText(globalObject, StringView(ascii.span()), base));
    }

    auto isSpace = [&](Latin1Character character) { return isWhitespaceAroundNumber(character) && (character < 0x80 || textOf == NumberTextOf::Str); };
    unsigned start = 0;
    unsigned end = text.length();
    while (start < end && isSpace(text[start]))
        ++start;
    while (end > start && isSpace(text[end - 1]))
        --end;
    bool negative = false;
    if (start < end && (text[start] == '+' || text[start] == '-')) {
        negative = text[start] == '-';
        ++start;
    }

    auto prefixBase = [&]() -> int {
        if (end - start < 2 || text[start] != '0')
            return 0;
        switch (text[start + 1]) {
        case 'x':
        case 'X':
            return 16;
        case 'o':
        case 'O':
            return 8;
        case 'b':
        case 'B':
            return 2;
        default:
            return 0;
        }
    };
    bool hadPrefix = false;
    bool mustBeZero = false;
    if (!base) {
        base = prefixBase();
        if (base)
            hadPrefix = true;
        else {
            base = 10;
            // "0" and "00" are fine, "01" is not.
            mustBeZero = start < end && text[start] == '0';
        }
    } else if (prefixBase() == base)
        hadPrefix = true;
    if (hadPrefix) {
        start += 2;
        // One underscore may follow the prefix.
        if (start < end && text[start] == '_')
            ++start;
    }
    if (start >= end)
        return { };

    // Goes through the digits and hands each to `take`. False: this is no number.
    size_t count = 0;
    bool isZero = true;
    bool endsAtNUL = false;
    auto scan = [&](auto&& take) {
        count = 0;
        bool lastWasUnderscore = true; // None at the start.
        for (unsigned i = start; i < end; ++i) {
            Latin1Character ch = text[i];
            if (ch == '_') {
                if (lastWasUnderscore)
                    return false;
                lastWasUnderscore = true;
                continue;
            }
            int digit = isASCIIDigit(ch) ? ch - '0' : isASCIIAlpha(ch) ? toASCIILowerUnchecked(ch) - 'a' + 10 : -1;
            if (digit < 0 || digit >= base) {
                while (i < end && isSpace(text[i]))
                    ++i;
                endsAtNUL = i < end && !text[i] && !lastWasUnderscore;
                return false;
            }
            lastWasUnderscore = false;
            isZero &= !digit;
            ++count;
            take("0123456789abcdefghijklmnopqrstuvwxyz"[digit]);
        }
        return !lastWasUnderscore;
    };

    // No memory is spent on a text that is too long. (CPython counts the digits of a C string, and finds out afterwards that the text
    // went on behind a NUL.)
    int limit = runtime(globalObject).maxStrDigits;
    if (limit > 0 && (base & (base - 1)) && end - start > static_cast<unsigned>(limit)) {
        bool isNumber = scan([](Latin1Character) { });
        if ((isNumber || endsAtNUL) && count > static_cast<size_t>(limit))
            return raiseValueError(globalObject, scope, "Exceeds the limit ("_s, limit, " digits) for integer string conversion: value has "_s, count, " digits; use sys.set_int_max_str_digits() to increase the limit"_s);
        if (!isNumber)
            return { };
    }

    Vector<Latin1Character, 64> digits;
    // All at once: a Vector that grows step by step ends the process long before it is full.
    if (end - start > digits.capacity() && !digits.tryReserveInitialCapacity(end - start)) [[unlikely]]
        return raiseMemoryError(globalObject, scope, String());
    if (!scan([&](Latin1Character digit) { digits.append(digit); }))
        return { };
    if (mustBeZero && !isZero)
        return { };

    JSValue result = intFromDigits(globalObject, StringView(digits.span()), base);
    RETURN_IF_EXCEPTION(scope, { });
    if (negative)
        RELEASE_AND_RETURN(scope, intNegate(globalObject, result));
    return result;
}

JSValue raiseInvalidIntLiteral(JSGlobalObject* globalObject, JSValue source, int base)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String shown = shortRepr(globalObject, source, 200);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseValueError(globalObject, scope, "invalid literal for int() with base "_s, base, ": "_s, shown);
}

static JSValue intFromTextObject(JSGlobalObject* globalObject, JSValue source, int base)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text;
    if (isStr(source)) {
        text = strValue(source)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        std::span<const uint8_t> bytes;
        bool isText = getBytesOfNumber(globalObject, source, bytes);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isText)
            RELEASE_AND_RETURN(scope, toInt(globalObject, source));
        if (bytes.size() > String::MaxLength) [[unlikely]]
            return raiseMemoryError(globalObject, scope, String());
        text = String(byteCast<Latin1Character>(bytes));
    }
    JSValue result = intFromText(globalObject, StringView(text), base, isStr(source) ? NumberTextOf::Str : NumberTextOf::Bytes);
    RETURN_IF_EXCEPTION(scope, { });
    if (!result) {
        // What is not a str is shown as bytes, whatever it is.
        if (!isStr(source))
            source = PyBytes::create(globalObject, byteCast<uint8_t>(text.span8().first(std::min<size_t>(text.length(), 200))));
        RELEASE_AND_RETURN(scope, raiseInvalidIntLiteral(globalObject, source, base));
    }
    return result;
}

static JSValue intNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue source = args.get(0);
    JSValue baseValue = args.get(1);
    if (args.size() > 2 && !args.hasKeywords() && cls.asCell() == rt.intType())
        return raiseTypeError(globalObject, scope, "int expected at most 2 arguments, got "_s, args.size());
    if (args.totalCount() > 2)
        return raiseTypeError(globalObject, scope, "int() takes at most 2 "_s, args.size() ? ""_s : "keyword "_s, "arguments ("_s, args.totalCount(), " given)"_s);
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        if (!WTF::equal(args.keywordName(i)->tryGetValueImpl(), "base"_s.span8()) || baseValue)
            RELEASE_AND_RETURN(scope, raiseUnexpectedKeyword(globalObject, "int"_s, stringOf(args.keywordName(i)), { "base"_s }));
        baseValue = args.keywordValue(i);
    }

    JSValue result;
    if (!source) {
        if (baseValue)
            return raiseTypeError(globalObject, scope, "int() missing string argument"_s);
        result = jsNumber(0);
    } else if (!baseValue) {
        // What a class derived from str or bytes says of itself comes before the text.
        PyType* sourceType = rt.typeOf(source);
        if ((isStr(source) || isBytesLike(source)) && !sourceType->lookup(Special::Int) && !sourceType->lookup(Special::Index))
            result = intFromTextObject(globalObject, source, 10);
        else if (isFloat(source) && !rt.typeOf(source)->isHeapType())
            result = intFromDouble(globalObject, floatValue(source));
        else
            result = toInt(globalObject, source);
    } else {
        int64_t base = toSsize(globalObject, baseValue, nullptr);
        RETURN_IF_EXCEPTION(scope, { });
        if (base && (base < 2 || base > 36))
            return raiseValueError(globalObject, scope, "int() base must be >= 2 and <= 36, or 0"_s);
        if (!isStr(source) && !PyBytes::is(source) && !PyByteArray::is(source))
            return raiseTypeError(globalObject, scope, "int() can't convert non-string with explicit base"_s);
        result = intFromTextObject(globalObject, source, static_cast<int>(base));
    }
    RETURN_IF_EXCEPTION(scope, { });

    PyType* type = PyType::from(cls);
    if (type == rt.intType())
        return sameInt(result);
    return PyInt::create(vm, rt.structure<PyInt>(), type, result);
}

static constexpr NativeDef intMethods[] = {
    PY_NEW(intNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", intReprMethod, "($self, /)"),
    PY_HASH(intHashMethod),
    PY_SLOT("__bool__", intBoolMethod, "($self, /)"),
    PY_SLOT("__eq__", intCompareMethod<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", intCompareMethod<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", intCompareMethod<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", intCompareMethod<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", intCompareMethod<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", intCompareMethod<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__add__", intAddMethod, "($self, value, /)"),
    PY_SLOT("__radd__", intAddMethodReflected, "($self, value, /)"),
    PY_SLOT("__sub__", intSubMethod, "($self, value, /)"),
    PY_SLOT("__rsub__", intSubMethodReflected, "($self, value, /)"),
    PY_SLOT("__mul__", intMulMethod, "($self, value, /)"),
    PY_SLOT("__rmul__", intMulMethodReflected, "($self, value, /)"),
    PY_SLOT("__mod__", intModMethod, "($self, value, /)"),
    PY_SLOT("__rmod__", intModMethodReflected, "($self, value, /)"),
    PY_SLOT("__divmod__", intDivModMethod, "($self, value, /)"),
    PY_SLOT("__rdivmod__", intDivModMethodReflected, "($self, value, /)"),
    PY_SLOT("__pow__", intPowMethod, "($self, value, mod=None, /)"),
    PY_SLOT("__rpow__", intPowMethodReflected, "($self, value, mod=None, /)"),
    PY_SLOT("__neg__", intNegMethod, "($self, /)"),
    PY_SLOT("__pos__", intIdentity, "($self, /)"),
    PY_SLOT("__abs__", intAbsMethod, "($self, /)"),
    PY_SLOT("__invert__", intInvertMethod, "($self, /)"),
    PY_SLOT("__lshift__", intLShiftMethod, "($self, value, /)"),
    PY_SLOT("__rlshift__", intLShiftMethodReflected, "($self, value, /)"),
    PY_SLOT("__rshift__", intRShiftMethod, "($self, value, /)"),
    PY_SLOT("__rrshift__", intRShiftMethodReflected, "($self, value, /)"),
    PY_SLOT("__and__", intAndMethod, "($self, value, /)"),
    PY_SLOT("__rand__", intAndMethodReflected, "($self, value, /)"),
    PY_SLOT("__xor__", intXorMethod, "($self, value, /)"),
    PY_SLOT("__rxor__", intXorMethodReflected, "($self, value, /)"),
    PY_SLOT("__or__", intOrMethod, "($self, value, /)"),
    PY_SLOT("__ror__", intOrMethodReflected, "($self, value, /)"),
    PY_SLOT("__int__", intIdentity, "($self, /)"),
    PY_SLOT("__float__", intFloatMethod, "($self, /)"),
    PY_SLOT("__floordiv__", intFloorDivMethod, "($self, value, /)"),
    PY_SLOT("__rfloordiv__", intFloorDivMethodReflected, "($self, value, /)"),
    PY_SLOT("__truediv__", intTrueDivMethod, "($self, value, /)"),
    PY_SLOT("__rtruediv__", intTrueDivMethodReflected, "($self, value, /)"),
    PY_SLOT("__index__", intIdentity, "($self, /)"),
    PY_METHOD("conjugate", intIdentity, "($self, /)"),
    PY_METHOD("bit_length", intBitLengthMethod, "($self, /)"),
    PY_METHOD("__sizeof__", intSizeOf, "($self, /)"),
    PY_METHOD("bit_count", intBitCountMethod, "($self, /)"),
    PY_METHOD("to_bytes", intToBytes, "($self, /, length=1, byteorder='big', *, signed=False)"),
    PY_CLASSMETHOD("from_bytes", intFromBytes, "($type, /, bytes, byteorder='big', *, signed=False)"),
    PY_METHOD("as_integer_ratio", intAsIntegerRatio, "($self, /)"),
    PY_METHOD("is_integer", intIsInteger, "($self, /)"),
    PY_METHOD("__trunc__", intIdentity, "($self, /)"),
    PY_METHOD("__floor__", intIdentity, "($self, /)"),
    PY_METHOD("__ceil__", intIdentity, "($self, /)"),
    PY_METHOD("__round__", intRoundMethod, "($self, ndigits=None, /)"),
    PY_METHOD("__getnewargs__", intGetNewArgs, "($self, /)"),
    PY_METHOD("__format__", intFormatMethod, "($self, format_spec, /)"),
};

static constexpr NativeGetSet intGetSets[] = {
    { "real"_s, intIdentity },
    { "imag"_s, intZero },
    { "numerator"_s, intIdentity },
    { "denominator"_s, intOne },
};

// ---- bool ----

static JSValue boolNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.hasKeywords())
        return raiseTypeError(globalObject, scope, "bool() takes no keyword arguments"_s);
    if (args.size() > 1)
        return raiseTypeError(globalObject, scope, "bool expected at most 1 argument, got "_s, args.size());
    if (!args.size())
        return jsBoolean(false);
    bool result = isTrue(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue boolInvert(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    warn(globalObject, runtime(globalObject).DeprecationWarning(), "Bitwise inversion '~' on bool is deprecated and will be removed in Python 3.16. This returns the bitwise inversion of the underlying int object and is usually not what you expect from negating a bool. Use the 'not' operator for boolean negation or ~int(x) if you really want the bitwise inversion of the underlying int."_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, intInvert(globalObject, intValue(self)));
}

static JSValue boolRepr(JSGlobalObject* globalObject, JSValue self)
{
    return makeStr(getVM(globalObject), self.asBoolean() ? "True"_s : "False"_s);
}

#define PY_BOOL_BINARY(name, op, fallback) \
    static JSValue name(JSGlobalObject* globalObject, JSValue self, JSValue other) \
    { \
        if (self.isBoolean() && other.isBoolean()) \
            return jsBoolean(self.asBoolean() op other.asBoolean()); \
        return fallback(globalObject, self, other); \
    }

PY_BOOL_BINARY(boolAnd, &, intAndMethod)
PY_BOOL_BINARY(boolOr, |, intOrMethod)
PY_BOOL_BINARY(boolXor, ^, intXorMethod)

static constexpr NativeDef boolMethods[] = {
    PY_NEW(boolNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", boolRepr, "($self, /)"),
    PY_SLOT("__and__", boolAnd, "($self, value, /)"),
    PY_SLOT("__rand__", boolAnd, "($self, value, /)"),
    PY_SLOT("__or__", boolOr, "($self, value, /)"),
    PY_SLOT("__ror__", boolOr, "($self, value, /)"),
    PY_SLOT("__xor__", boolXor, "($self, value, /)"),
    PY_SLOT("__rxor__", boolXor, "($self, value, /)"),
    PY_SLOT("__invert__", boolInvert, "($self, /)"),
};

void initializeIntTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.intType()->initializeBuiltin(globalObject, { "int"_s, { TypeFlag::Base, TypeFlag::IntSubclass, TypeFlag::MatchSelf, TypeFlag::OwnLayout }, intMethods, intGetSets, { } }, rt.typeType(), rt.objectType());
    rt.boolType()->initializeBuiltin(globalObject, { "bool"_s, { }, boolMethods, { }, { } }, rt.typeType(), rt.intType());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
