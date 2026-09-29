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
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonOperators.h"
#include "PythonSequences.h"
#include <wtf/SafeStrerror.h>

// The module math: Modules/mathmodule.c of CPython.
//
// From here to the end of the file, `a * b + c` that is written as one expression is rounded once where the processor can do that, which is how what CPython is built with treats it and is not how the rest of the engine
// is built. So a sum here is written as it is written there. See "One rounding or two" in the README.
#if COMPILER(CLANG)
#pragma STDC FP_CONTRACT ON
#endif

#include "PythonMathKernels.h"

namespace JSC { namespace Python {

using namespace MathKernels;

namespace {

JSValue multiply(JSGlobalObject* globalObject, JSValue a, JSValue b) { return binaryOperation(globalObject, BinaryOperator::Mult, false, a, b); }
JSValue add(JSGlobalObject* globalObject, JSValue a, JSValue b) { return binaryOperation(globalObject, BinaryOperator::Add, false, a, b); }
JSValue subtract(JSGlobalObject* globalObject, JSValue a, JSValue b) { return binaryOperation(globalObject, BinaryOperator::Sub, false, a, b); }
JSValue floorDivide(JSGlobalObject* globalObject, JSValue a, JSValue b) { return binaryOperation(globalObject, BinaryOperator::FloorDiv, false, a, b); }
// _PyLong_Lshift() and _PyLong_Rshift()
JSValue shiftLeft(JSGlobalObject* globalObject, JSValue a, int64_t count) { return binaryOperation(globalObject, BinaryOperator::LShift, false, a, intFromInt64(globalObject, count)); }
JSValue shiftRight(JSGlobalObject* globalObject, JSValue a, int64_t count) { return binaryOperation(globalObject, BinaryOperator::RShift, false, a, intFromInt64(globalObject, count)); }

// Of ints
bool isNegative(JSValue integer) { return compareInts(integer, jsNumber(0)) < 0; }
bool isZero(JSValue integer) { return !compareInts(integer, jsNumber(0)); }
JSValue absoluteOfInt(JSGlobalObject* globalObject, JSValue integer) { return isNegative(integer) ? numberUnaryOperation(globalObject, UnaryOperator::USub, integer) : integer; }

// PyLong_Check()
bool isIntOrDerived(JSGlobalObject* globalObject, JSValue value) { return isInstance(globalObject, value, globalObject->pyRealm()->typeInt()); }

// PyLong_AsLongAndOverflow(), of an int: nothing if there is no room for it, and then which way.
std::optional<long> toLong(JSValue integer, int& overflow)
{
    static_assert(sizeof(long) == sizeof(int64_t));
    overflow = 0;
    if (auto value = tryInt64(integer))
        return static_cast<long>(*value);
    overflow = isNegative(integer) ? -1 : 1;
    return std::nullopt;
}

// _Py_bit_length()
int bitLength(unsigned long value) { return value ? static_cast<int>(8 * sizeof(unsigned long)) - __builtin_clzl(value) : 0; }

// PyLong_FromDouble()
JSValue longFromDouble(JSGlobalObject* globalObject, double value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (std::isnan(value))
        return raiseValueError(globalObject, scope, "cannot convert float NaN to integer"_s);
    if (std::isinf(value))
        return raise(globalObject, scope, BuiltinType::OverflowError, "cannot convert float infinity to integer"_s);
    return intFromDouble(globalObject, value);
}

// _PyLong_GCD(), of ints. What it comes to is never less than nothing.
JSValue greatestCommonDivisor(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    while (!isZero(b)) {
        JSValue remainder = binaryOperation(globalObject, BinaryOperator::Mod, false, a, b);
        RETURN_IF_EXCEPTION(scope, { });
        a = b;
        b = remainder;
    }
    RELEASE_AND_RETURN(scope, absoluteOfInt(globalObject, a));
}

// _PyLong_Frexp(), of an int that is more than nothing: it is x * 2 ** exponent, as nearly as a double can say, with x at least a half and less than one.
double frexpOfInt(JSGlobalObject* globalObject, JSValue integer, int64_t& exponent)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    exponent = bitLengthOfInt(classify(integer));
    // Two bits more than a double has, the last of them saying as well whether anything has been left off.
    int64_t shift = exponent - (DBL_MANT_DIG + 2);
    uint64_t digits;
    if (shift <= 0)
        digits = lowBitsOfInt(integer) << -shift;
    else {
        JSValue top = shiftRight(globalObject, integer, shift);
        RETURN_IF_EXCEPTION(scope, 0);
        digits = lowBitsOfInt(top);
        JSValue back = shiftLeft(globalObject, top, shift);
        RETURN_IF_EXCEPTION(scope, 0);
        if (compareInts(back, integer))
            digits |= 1;
    }
    // To the nearest, and to the even one of two that are as near.
    static constexpr int halfEvenCorrection[8] = { 0, -1, -2, 1, 0, -1, 2, 1 };
    digits += halfEvenCorrection[digits & 7];
    double x = static_cast<double>(digits);
    x /= 4.0 * 9007199254740992.0;
    if (x == 1.0) {
        x = 0.5;
        ++exponent;
    }
    return x;
}

// is_error(): what is to be made of errno, given what the function returned. False if it is nothing after all, and then nothing is raised.
bool isError(JSGlobalObject* globalObject, double x, bool raisesDomainError)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (errno == EDOM) {
        if (raisesDomainError)
            raiseValueError(globalObject, scope, "math domain error"_s);
        return true;
    }
    if (errno == ERANGE) {
        // It may be said of what has come to nothing as well as of what is too large, and only the second is an error.
        if (fabs(x) < 1.5)
            return false;
        raise(globalObject, scope, BuiltinType::OverflowError, "math range error"_s);
        return true;
    }
    // PyErr_SetFromErrno(PyExc_ValueError)
    raise(globalObject, scope, BuiltinType::ValueError, PyTuple::create(globalObject, { jsNumber(errno), jsString(globalObject->vm(), String::fromUTF8(safeStrerror(errno).span())) }));
    return true;
}

// FUNC1, FUNC1D, FUNC1A and FUNC1AD
struct FunctionOfOne {
    ASCIILiteral name;
    double (*function)(double);
    bool setsErrno; // math_1a() and not math_1()
    bool canOverflow;
    ASCIILiteral expected; // What is said of an argument that will not do, before the argument. Null for "math domain error".
};

constexpr FunctionOfOne functionsOfOne[] = {
    { "acos"_s, acos, false, false, "expected a number in range from -1 up to 1, got "_s },
    { "acosh"_s, acosh, false, false, "expected argument value not less than 1, got "_s },
    { "asin"_s, asin, false, false, "expected a number in range from -1 up to 1, got "_s },
    { "asinh"_s, asinh, false, false, { } },
    { "atan"_s, atan, false, false, { } },
    { "atanh"_s, atanh, false, false, "expected a number between -1 and 1, got "_s },
    { "cbrt"_s, cbrt, false, false, { } },
    { "cos"_s, cos, false, false, "expected a finite input, got "_s },
    { "cosh"_s, cosh, false, true, { } },
    { "erf"_s, erf, true, false, { } },
    { "erfc"_s, erfc, true, false, { } },
    { "exp"_s, exp, false, true, { } },
    { "exp2"_s, exp2, false, true, { } },
    { "expm1"_s, expm1, false, true, { } },
    { "fabs"_s, fabs, false, false, { } },
    { "gamma"_s, m_tgamma, true, false, "expected a noninteger or positive integer, got "_s },
    { "lgamma"_s, m_lgamma, true, false, "expected a noninteger or positive integer, got "_s },
    { "log1p"_s, _Py_log1p, false, false, "expected argument value > -1, got "_s },
    { "sin"_s, sin, false, false, "expected a finite input, got "_s },
    { "sinh"_s, sinh, false, true, { } },
    { "sqrt"_s, sqrt, false, false, "expected a nonnegative input, got "_s },
    { "tan"_s, tan, false, false, "expected a finite input, got "_s },
    { "tanh"_s, tanh, false, false, { } },
};

// math_1()
JSValue callFunctionOfOne(JSGlobalObject* globalObject, JSValue argument, double (*function)(double), bool canOverflow, ASCIILiteral expected)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto given = toDouble(globalObject, argument);
    RETURN_IF_EXCEPTION(scope, { });
    double x = *given;
    auto raiseDomainError = [&] {
        if (expected.isNull())
            return raiseValueError(globalObject, scope, "math domain error"_s);
        return raiseValueError(globalObject, scope, concatenate(expected, reprOfDouble(x)));
    };
    errno = 0;
    double r = function(x);
    if (isnan(r) && !isnan(x))
        return raiseDomainError();
    if (isinf(r) && isfinite(x)) {
        if (canOverflow)
            return raise(globalObject, scope, BuiltinType::OverflowError, "math range error"_s);
        return raiseDomainError();
    }
    if (isfinite(r) && errno) {
        bool failed = isError(globalObject, r, true);
        RETURN_IF_EXCEPTION(scope, { });
        if (failed)
            return { };
    }
    return floatFromDouble(r);
}

// FUNC2
struct FunctionOfTwo {
    ASCIILiteral name;
    double (*function)(double, double);
};

constexpr FunctionOfTwo functionsOfTwo[] = {
    { "atan2"_s, atan2 },
    { "copysign"_s, copysign },
    { "remainder"_s, m_remainder },
};

// What is left to do when errno has been set to say how things went
JSValue floatUnlessError(JSGlobalObject* globalObject, double r)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (errno) {
        isError(globalObject, r, true);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return floatFromDouble(r);
}

// loghelper()
JSValue logarithm(JSGlobalObject* globalObject, JSValue argument, double (*func)(double))
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    // The logarithm of an int can be had though the int is too large to be a float.
    if (!isIntOrDerived(globalObject, argument))
        RELEASE_AND_RETURN(scope, callFunctionOfOne(globalObject, argument, func, false, "expected a positive input, got "_s));
    JSValue integer = toInt(globalObject, argument);
    RETURN_IF_EXCEPTION(scope, { });
    if (compareInts(integer, jsNumber(0)) <= 0)
        return raiseValueError(globalObject, scope, "expected a positive input"_s);
    double result;
    // As many bits as this and it is too large, and with fewer it is not, unless rounding it makes it so.
    auto x = bitLengthOfInt(classify(integer)) <= DBL_MAX_EXP ? toDouble(globalObject, integer) : std::nullopt;
    if (scope.exception()) [[unlikely]] {
        if (!catchException(globalObject, BuiltinType::OverflowError))
            return { };
        x = std::nullopt;
    }
    if (x)
        result = func(*x);
    else {
        int64_t e;
        double mantissa = frexpOfInt(globalObject, integer, e);
        RETURN_IF_EXCEPTION(scope, { });
        // Value is ~= x * 2**e, so the log ~= log(x) + log(2) * e.
        result = func(mantissa) + func(2.0) * e;
    }
    return floatFromDouble(result);
}

// factorial_partial_product(): the product of range(start, stop, 2), both of which are odd.
JSValue factorialPartialProduct(JSGlobalObject* globalObject, unsigned long start, unsigned long stop, unsigned long maxBits)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    unsigned long operandCount = (stop - start) / 2;
    // If there is room for it in an unsigned long
    if (operandCount <= 8 * sizeof(long) && operandCount * maxBits <= 8 * sizeof(long)) {
        unsigned long total = start;
        for (unsigned long j = start + 2; j < stop; j += 2)
            total *= j;
        return intFromUInt64(globalObject, total);
    }
    unsigned long midpoint = (start + operandCount) | 1;
    JSValue left = factorialPartialProduct(globalObject, start, midpoint, bitLength(midpoint - 2));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue right = factorialPartialProduct(globalObject, midpoint, stop, maxBits);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, multiply(globalObject, left, right));
}

// factorial_odd_part()
JSValue factorialOddPart(JSGlobalObject* globalObject, unsigned long n)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue inner = jsNumber(1);
    JSValue outer = inner;
    unsigned long upper = 3;
    for (long i = bitLength(n) - 2; i >= 0; --i) {
        unsigned long v = n >> i;
        if (v <= 2)
            continue;
        unsigned long lower = upper;
        // The least odd number that is more than n / 2**i
        upper = (v + 1) | 1;
        JSValue partial = factorialPartialProduct(globalObject, lower, upper, bitLength(upper - 2));
        RETURN_IF_EXCEPTION(scope, { });
        inner = multiply(globalObject, inner, partial);
        RETURN_IF_EXCEPTION(scope, { });
        outer = multiply(globalObject, outer, inner);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return outer;
}

JSValue factorial(JSGlobalObject* globalObject, JSValue argument)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue integer = toInt(globalObject, argument);
    RETURN_IF_EXCEPTION(scope, { });
    int overflow;
    auto given = toLong(integer, overflow);
    if (overflow == 1)
        return raise(globalObject, scope, BuiltinType::OverflowError, concatenate("factorial() argument should not exceed "_s, static_cast<int64_t>(LONG_MAX)));
    if (overflow == -1 || *given < 0)
        return raiseValueError(globalObject, scope, "factorial() not defined for negative values"_s);
    long x = *given;
    if (x < static_cast<long>(std::size(SmallFactorials)))
        return intFromUInt64(globalObject, SmallFactorials[x]);
    // The odd part of it, and then as many twos as there are in it
    JSValue oddPart = factorialOddPart(globalObject, x);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, shiftLeft(globalObject, oddPart, x - count_set_bits(x)));
}

// perm_comb_small(): P(n, k) or C(n, k), for an n that has fewer than 64 bits.
JSValue permutationsOrCombinationsOfSmall(JSGlobalObject* globalObject, unsigned long long n, unsigned long long k, bool isCombinations)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isCombinations) {
        if (k < std::size(fast_comb_limits1) && n <= fast_comb_limits1[k]) {
            // It fits in 64 bits: the odd part of it, worked out modulo 2**64, and then the twos.
            uint64_t oddPart = reduced_factorial_odd_part[n] * inverted_factorial_odd_part[k] * inverted_factorial_odd_part[n - k];
            int shift = factorial_trailing_zeros[n] - factorial_trailing_zeros[k] - factorial_trailing_zeros[n - k];
            return intFromUInt64(globalObject, oddPart << shift);
        }
        if (k < std::size(fast_comb_limits2) && n <= fast_comb_limits2[k]) {
            // C(n, k) = C(n, k-1) * (n-k+1) / k
            unsigned long long result = n;
            for (unsigned long long i = 1; i < k;) {
                result *= --n;
                result /= ++i;
            }
            return intFromUInt64(globalObject, result);
        }
    } else if (k < std::size(fast_perm_limits) && n <= fast_perm_limits[k]) {
        if (n <= 127) {
            uint64_t oddPart = reduced_factorial_odd_part[n] * inverted_factorial_odd_part[n - k];
            int shift = factorial_trailing_zeros[n] - factorial_trailing_zeros[n - k];
            return intFromUInt64(globalObject, oddPart << shift);
        }
        // P(n, k) = P(n, k-1) * (n-k+1)
        unsigned long long result = n;
        for (unsigned long long i = 1; i < k;) {
            result *= --n;
            ++i;
        }
        return intFromUInt64(globalObject, result);
    }
    // P(n, k) = P(n, j) * P(n-j, k-j), and C(n, k) = C(n, j) * C(n-j, k-j) // C(k, j)
    unsigned long long j = k / 2;
    JSValue a = permutationsOrCombinationsOfSmall(globalObject, n, j, isCombinations);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue b = permutationsOrCombinationsOfSmall(globalObject, n - j, k - j, isCombinations);
    RETURN_IF_EXCEPTION(scope, { });
    a = multiply(globalObject, a, b);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isCombinations)
        return a;
    b = permutationsOrCombinationsOfSmall(globalObject, k, j, true);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, floorDivide(globalObject, a, b));
}

// perm_comb()
JSValue permutationsOrCombinations(JSGlobalObject* globalObject, JSValue n, unsigned long long k, bool isCombinations)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!k)
        return jsNumber(1);
    if (k == 1)
        return n;
    unsigned long long j = k / 2;
    JSValue a = permutationsOrCombinations(globalObject, n, j, isCombinations);
    RETURN_IF_EXCEPTION(scope, { });
    n = subtract(globalObject, n, intFromUInt64(globalObject, j));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue b = permutationsOrCombinations(globalObject, n, k - j, isCombinations);
    RETURN_IF_EXCEPTION(scope, { });
    a = multiply(globalObject, a, b);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isCombinations)
        return a;
    b = permutationsOrCombinationsOfSmall(globalObject, k, j, true);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, floorDivide(globalObject, a, b));
}

// The two arguments of perm() and comb(), as ints that are not less than nothing. False if it raised.
bool toCounts(JSGlobalObject* globalObject, JSValue& n, JSValue& k)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    n = toInt(globalObject, n);
    RETURN_IF_EXCEPTION(scope, false);
    k = toInt(globalObject, k);
    RETURN_IF_EXCEPTION(scope, false);
    if (isNegative(n)) {
        raiseValueError(globalObject, scope, "n must be a non-negative integer"_s);
        return false;
    }
    if (isNegative(k)) {
        raiseValueError(globalObject, scope, "k must be a non-negative integer"_s);
        return false;
    }
    return true;
}

// The coordinates that hypot() and dist() work from
struct Coordinates {
    Vector<double, 16> values;
    double max { 0.0 };
    bool foundNaN { false };

    void append(double x)
    {
        values.append(x);
        foundNaN |= isnan(x);
        if (x > max)
            max = x;
    }
    double norm() { return vector_norm(static_cast<ptrdiff_t>(values.size()), values.mutableSpan().data(), max, foundNaN); }
};

} // anonymous namespace

PYTHON_NATIVE(mathFunctionOfOne)
{
    const FunctionOfOne& entry = functionsOfOne[unpack<unsigned>(callFrame, 0)];
    NATIVE_PROLOGUE();
    if (!entry.setsErrno)
        RELEASE_AND_RETURN(scope, JSValue::encode(callFunctionOfOne(globalObject, args[0], entry.function, entry.canOverflow, entry.expected)));
    // math_1a()
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    double r = entry.function(*x);
    if (errno) {
        bool failed = isError(globalObject, r, entry.expected.isNull());
        RETURN_IF_EXCEPTION(scope, { });
        if (failed)
            return JSValue::encode(raiseValueError(globalObject, scope, concatenate(entry.expected, reprOfDouble(*x))));
    }
    return JSValue::encode(floatFromDouble(r));
}

// math_2()
PYTHON_NATIVE(mathFunctionOfTwo)
{
    const FunctionOfTwo& entry = functionsOfTwo[unpack<unsigned>(callFrame, 0)];
    NATIVE_PROLOGUE();
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto y = toDouble(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    double r = entry.function(*x, *y);
    if (isnan(r))
        errno = !isnan(*x) && !isnan(*y) ? EDOM : 0;
    else if (isinf(r))
        errno = isfinite(*x) && isfinite(*y) ? ERANGE : 0;
    RELEASE_AND_RETURN(scope, JSValue::encode(floatUnlessError(globalObject, r)));
}

// gcd(*integers)
PYTHON_NATIVE(mathGcd)
{
    NATIVE_PROLOGUE();
    if (!args.size())
        return JSValue::encode(jsNumber(0));
    JSValue result = toInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (args.size() == 1)
        RELEASE_AND_RETURN(scope, JSValue::encode(absoluteOfInt(globalObject, result)));
    for (unsigned i = 1; i < args.size(); ++i) {
        JSValue x = toInt(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        // The rest are only looked at to see that they will do.
        if (result == jsNumber(1))
            continue;
        result = greatestCommonDivisor(globalObject, result, x);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

// lcm(*integers)
PYTHON_NATIVE(mathLcm)
{
    NATIVE_PROLOGUE();
    if (!args.size())
        return JSValue::encode(jsNumber(1));
    JSValue result = toInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (args.size() == 1)
        RELEASE_AND_RETURN(scope, JSValue::encode(absoluteOfInt(globalObject, result)));
    for (unsigned i = 1; i < args.size(); ++i) {
        JSValue x = toInt(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        if (result == jsNumber(0))
            continue;
        // long_lcm()
        if (isZero(x)) {
            result = jsNumber(0);
            continue;
        }
        JSValue divisor = greatestCommonDivisor(globalObject, result, x);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue quotient = floorDivide(globalObject, result, divisor);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue product = multiply(globalObject, quotient, x);
        RETURN_IF_EXCEPTION(scope, { });
        result = absoluteOfInt(globalObject, product);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

// ceil(x, /) and floor(x, /)
PYTHON_NATIVE(mathCeilOrFloor)
{
    bool isCeil = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    double x;
    if (isFloat(args[0]))
        x = classify(args[0]).real;
    else {
        JSValue self;
        JSValue method = lookupSpecial(globalObject, args[0], isCeil ? names.dunder_ceil : names.dunder_floor, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (method)
            RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
        auto given = toDouble(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        x = *given;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(longFromDouble(globalObject, isCeil ? ceil(x) : floor(x))));
}

// fsum(seq, /)
PYTHON_NATIVE(mathFsum)
{
    NATIVE_PROLOGUE();
    // Partial sums that do not overlap, each larger than the last, which together are exactly what has been added up so far
    Vector<double, 32> p;
    double specialSum = 0.0;
    double infinitySum = 0.0;
    forEach(globalObject, args[0], [&] (JSValue item) {
        auto given = toDouble(globalObject, item);
        RETURN_IF_EXCEPTION(scope, false);
        double x = *given;
        double xsave = x;
        size_t i = 0;
        for (size_t j = 0; j < p.size(); ++j) {
            double y = p[j];
            if (fabs(x) < fabs(y))
                std::swap(x, y);
            double hi = x + y;
            double yr = hi - x;
            double lo = y - yr;
            if (lo != 0.0)
                p[i++] = lo;
            x = hi;
        }
        p.shrink(i);
        if (x != 0.0) {
            if (!isfinite(x)) {
                // Either it has got too large on the way, or there was a nan or an inf among what is being added up.
                if (isfinite(xsave)) {
                    raise(globalObject, scope, BuiltinType::OverflowError, "intermediate overflow in fsum"_s);
                    return false;
                }
                if (isinf(xsave))
                    infinitySum += xsave;
                specialSum += xsave;
                p.shrink(0);
            } else
                p.append(x);
        }
        return true;
    });
    RETURN_IF_EXCEPTION(scope, { });
    if (specialSum != 0.0) {
        if (isnan(infinitySum))
            return JSValue::encode(raiseValueError(globalObject, scope, "-inf + inf in fsum"_s));
        return JSValue::encode(floatFromDouble(specialSum));
    }
    double hi = 0.0;
    size_t n = p.size();
    if (n > 0) {
        double lo = 0.0;
        hi = p[--n];
        // From the top, until what they come to is no longer exact
        while (n > 0) {
            double x = hi;
            double y = p[--n];
            hi = x + y;
            double yr = hi - x;
            lo = y - yr;
            if (lo != 0.0)
                break;
        }
        // So that rounding to the even one of two takes account of what comes after
        if (n > 0 && ((lo < 0.0 && p[n - 1] < 0.0) || (lo > 0.0 && p[n - 1] > 0.0))) {
            double y = lo * 2.0;
            double x = hi + y;
            double yr = x - hi;
            if (y == yr)
                hi = x;
        }
    }
    return JSValue::encode(floatFromDouble(hi));
}

// isqrt(n, /)
PYTHON_NATIVE(mathIsqrt)
{
    NATIVE_PROLOGUE();
    JSValue n = toInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNegative(n))
        return JSValue::encode(raiseValueError(globalObject, scope, "isqrt() argument must be nonnegative"_s));
    if (isZero(n))
        return JSValue::encode(jsNumber(0));
    // c = (n.bit_length() - 1) // 2
    int64_t c = (bitLengthOfInt(classify(n)) - 1) / 2;
    if (c <= 31) {
        // n < 2**64
        int shift = 31 - static_cast<int>(c);
        uint64_t m = lowBitsOfInt(n);
        uint32_t u = _approximate_isqrt(m << 2 * shift) >> shift;
        u -= static_cast<uint64_t>(u) * u > m;
        return JSValue::encode(intFromUInt64(globalObject, u));
    }
    // The first five times round are done with numbers of C's.
    int bitLengthOfC = 6;
    while ((c >> bitLengthOfC) > 0)
        ++bitLengthOfC;
    int64_t d = c >> (bitLengthOfC - 5);
    JSValue b = shiftRight(globalObject, n, 2 * c - 62);
    RETURN_IF_EXCEPTION(scope, { });
    uint32_t u = _approximate_isqrt(lowBitsOfInt(b)) >> (31U - d);
    JSValue a = intFromUInt64(globalObject, u);
    for (int s = bitLengthOfC - 6; s >= 0; --s) {
        int64_t e = d;
        d = c >> s;
        // q = (n >> 2*c - e - d + 1) // a
        JSValue q = shiftRight(globalObject, n, 2 * c - d - e + 1);
        RETURN_IF_EXCEPTION(scope, { });
        q = floorDivide(globalObject, q, a);
        RETURN_IF_EXCEPTION(scope, { });
        // a = (a << d - 1 - e) + q
        a = shiftLeft(globalObject, a, d - 1 - e);
        RETURN_IF_EXCEPTION(scope, { });
        a = add(globalObject, a, q);
        RETURN_IF_EXCEPTION(scope, { });
    }
    // It is that or one less.
    b = multiply(globalObject, a, a);
    RETURN_IF_EXCEPTION(scope, { });
    if (compareInts(n, b) < 0)
        RELEASE_AND_RETURN(scope, JSValue::encode(subtract(globalObject, a, jsNumber(1))));
    return JSValue::encode(a);
}

// factorial(n, /)
PYTHON_NATIVE(mathFactorial)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(factorial(globalObject, args[0])));
}

// trunc(x, /)
PYTHON_NATIVE(mathTrunc)
{
    NATIVE_PROLOGUE();
    if (isFloat(args[0]))
        RELEASE_AND_RETURN(scope, JSValue::encode(longFromDouble(globalObject, classify(args[0]).real)));
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[0], names.dunder_trunc, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("type "_s, typeName(globalObject, args[0]), " doesn't define __trunc__ method"_s)));
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
}

// frexp(x, /)
PYTHON_NATIVE(mathFrexp)
{
    NATIVE_PROLOGUE();
    auto given = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    double x = *given;
    int i = 0;
    if (!isnan(x) && !isinf(x) && x)
        x = frexp(x, &i);
    return JSValue::encode(PyTuple::create(globalObject, { floatFromDouble(x), jsNumber(i) }));
}

// ldexp(x, i, /)
PYTHON_NATIVE(mathLdexp)
{
    NATIVE_PROLOGUE();
    auto given = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    double x = *given;
    if (!isIntOrDerived(globalObject, args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "Expected an int as second argument to ldexp."_s));
    JSValue integer = toInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int overflow;
    long exponent = toLong(integer, overflow).value_or(0);
    if (overflow)
        exponent = overflow < 0 ? LONG_MIN : LONG_MAX;
    double r;
    errno = 0;
    if (x == 0. || !isfinite(x))
        r = x;
    else if (exponent > INT_MAX) {
        r = copysign(std::numeric_limits<double>::infinity(), x);
        errno = ERANGE;
    } else if (exponent < INT_MIN)
        r = copysign(0., x);
    else {
        r = ldexp(x, static_cast<int>(exponent));
        if (isinf(r))
            errno = ERANGE;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(floatUnlessError(globalObject, r)));
}

// modf(x, /)
PYTHON_NATIVE(mathModf)
{
    NATIVE_PROLOGUE();
    auto given = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    double x = *given;
    if (isinf(x))
        return JSValue::encode(PyTuple::create(globalObject, { floatFromDouble(copysign(0., x)), floatFromDouble(x) }));
    if (isnan(x))
        return JSValue::encode(PyTuple::create(globalObject, { floatFromDouble(x), floatFromDouble(x) }));
    double y;
    x = modf(x, &y);
    return JSValue::encode(PyTuple::create(globalObject, { floatFromDouble(x), floatFromDouble(y) }));
}

// log(x, [base=math.e])
PYTHON_NATIVE(mathLog)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "math.log"_s))
        return { };
    if (args.size() < 1 || args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("log expected at "_s, args.size() < 1 ? "least 1 argument"_s : "most 2 arguments"_s, ", got "_s, args.size())));
    JSValue numerator = logarithm(globalObject, args[0], m_log);
    RETURN_IF_EXCEPTION(scope, { });
    if (args.size() == 1)
        return JSValue::encode(numerator);
    JSValue denominator = logarithm(globalObject, args[1], m_log);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(binaryOperation(globalObject, BinaryOperator::Div, false, numerator, denominator)));
}

// log2(x, /) and log10(x, /)
PYTHON_NATIVE(mathLogToBase)
{
    bool isBaseTwo = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(logarithm(globalObject, args[0], isBaseTwo ? m_log2 : m_log10)));
}

// fma(x, y, z, /)
PYTHON_NATIVE(mathFma)
{
    NATIVE_PROLOGUE();
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto y = toDouble(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    auto z = toDouble(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    double r = fma(*x, *y, *z);
    if (isfinite(r))
        return JSValue::encode(floatFromDouble(r));
    if (isnan(r)) {
        if (!isnan(*x) && !isnan(*y) && !isnan(*z))
            return JSValue::encode(raiseValueError(globalObject, scope, "invalid operation in fma"_s));
    } else if (isfinite(*x) && isfinite(*y) && isfinite(*z))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "overflow in fma"_s));
    return JSValue::encode(floatFromDouble(r));
}

// fmod(x, y, /)
PYTHON_NATIVE(mathFmod)
{
    NATIVE_PROLOGUE();
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto y = toDouble(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (isinf(*y) && isfinite(*x))
        return JSValue::encode(floatFromDouble(*x));
    errno = 0;
    double r = fmod(*x, *y);
    if (isnan(r))
        errno = !isnan(*x) && !isnan(*y) ? EDOM : 0;
    RELEASE_AND_RETURN(scope, JSValue::encode(floatUnlessError(globalObject, r)));
}

// dist(p, q, /)
PYTHON_NATIVE(mathDist)
{
    NATIVE_PROLOGUE();
    PyTuple* p = isTuple(args[0]) ? asTuple(args[0]) : tupleFromIterable(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* q = isTuple(args[1]) ? asTuple(args[1]) : tupleFromIterable(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (p->length() != q->length())
        return JSValue::encode(raiseValueError(globalObject, scope, "both points must have the same number of dimensions"_s));
    Coordinates differences;
    for (unsigned i = 0; i < p->length(); ++i) {
        auto px = toDouble(globalObject, p->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        auto qx = toDouble(globalObject, q->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        differences.append(fabs(*px - *qx));
    }
    return JSValue::encode(floatFromDouble(differences.norm()));
}

// hypot(*coordinates)
PYTHON_NATIVE(mathHypot)
{
    NATIVE_PROLOGUE();
    Coordinates coordinates;
    for (unsigned i = 0; i < args.size(); ++i) {
        auto x = toDouble(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        coordinates.append(fabs(*x));
    }
    return JSValue::encode(floatFromDouble(coordinates.norm()));
}

// sumprod(p, q, /)
PYTHON_NATIVE(mathSumprod)
{
    NATIVE_PROLOGUE();
    JSValue pIterator = getIterator(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue qIterator = getIterator(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue total = jsNumber(0);
    // While they are all ints that there is room for, and then while they are all floats or ints and floats, the total is kept as a number of C's.
    bool isIntPathEnabled = true;
    bool isIntTotalInUse = false;
    bool isFloatPathEnabled = true;
    bool isFloatTotalInUse = false;
    long intTotal = 0;
    TripleLength floatTotal = tl_zero;
    while (true) {
        JSValue p = iteratorNext(globalObject, pIterator);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue q = iteratorNext(globalObject, qIterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!p != !q)
            return JSValue::encode(raiseValueError(globalObject, scope, "Inputs are not the same length"_s));
        bool isFinished = !p;

        if (isIntPathEnabled) {
            if (!isFinished && isInt(p) && isInt(q)) {
                auto intP = tryInt64(p);
                auto intQ = tryInt64(q);
                if (intP && intQ && !_check_long_mult_overflow(*intP, *intQ)) {
                    long product = *intP * *intQ;
                    if (!long_add_would_overflow(intTotal, product)) {
                        intTotal += product;
                        isIntTotalInUse = true;
                        continue;
                    }
                }
            }
            // It is over, or there is no room, or one of them is not an int.
            isIntPathEnabled = false;
            if (isIntTotalInUse) {
                total = add(globalObject, total, intFromInt64(globalObject, intTotal));
                RETURN_IF_EXCEPTION(scope, { });
                intTotal = 0;
                isIntTotalInUse = false;
            }
        }

        if (isFloatPathEnabled) {
            if (!isFinished) {
                auto isIntOrBool = [] (JSValue value) { return isInt(value) || value.isBoolean(); };
                // An int that is too large to be a float is not one for this.
                auto asDouble = [&] (JSValue value) -> std::optional<double> {
                    if (isFloat(value))
                        return classify(value).real;
                    auto result = toDouble(globalObject, value);
                    if (scope.exception()) [[unlikely]] {
                        scope.tryClearException();
                        return std::nullopt;
                    }
                    return result;
                };
                bool isPFloat = isFloat(p);
                bool isQFloat = isFloat(q);
                if ((isPFloat && isQFloat) || (isPFloat && isIntOrBool(q)) || (isQFloat && isIntOrBool(p))) {
                    // The float of the two first, as far as anything can tell
                    auto floatP = asDouble(p);
                    auto floatQ = floatP ? asDouble(q) : std::nullopt;
                    if (floatP && floatQ) {
                        TripleLength newTotal = tl_fma(*floatP, *floatQ, floatTotal);
                        if (isfinite(newTotal.hi)) {
                            floatTotal = newTotal;
                            isFloatTotalInUse = true;
                            continue;
                        }
                    }
                }
            }
            // It is over, or has got too large, or one of them is neither, or is not finite.
            isFloatPathEnabled = false;
            if (isFloatTotalInUse) {
                total = add(globalObject, total, floatFromDouble(tl_to_d(floatTotal)));
                RETURN_IF_EXCEPTION(scope, { });
                floatTotal = tl_zero;
                isFloatTotalInUse = false;
            }
        }

        if (isFinished)
            return JSValue::encode(total);
        JSValue term = multiply(globalObject, p, q);
        RETURN_IF_EXCEPTION(scope, { });
        total = add(globalObject, total, term);
        RETURN_IF_EXCEPTION(scope, { });
    }
}

// pow(x, y, /)
PYTHON_NATIVE(mathPow)
{
    NATIVE_PROLOGUE();
    auto givenX = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto givenY = toDouble(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    double x = *givenX;
    double y = *givenY;
    double r;
    errno = 0;
    if (!isfinite(x) || !isfinite(y)) {
        if (isnan(x))
            r = y == 0. ? 1. : x; // NaN**0 = 1
        else if (isnan(y))
            r = x == 1. ? 1. : y; // 1**NaN = 1
        else if (isinf(x)) {
            bool isYOdd = isfinite(y) && fmod(fabs(y), 2.0) == 1.0;
            if (y > 0.)
                r = isYOdd ? x : fabs(x);
            else if (y == 0.)
                r = 1.;
            else
                r = isYOdd ? copysign(0., x) : 0.;
        } else if (fabs(x) == 1.0)
            r = 1.;
        else if (y > 0. && fabs(x) > 1.0)
            r = y;
        else if (y < 0. && fabs(x) < 1.0)
            r = -y; // +inf
        else
            r = 0.;
    } else {
        r = pow(x, y);
        // A NaN comes only of something negative to a power that is not whole. An infinity comes of nought to a negative power, or of there being no room.
        if (isnan(r))
            errno = EDOM;
        else if (isinf(r))
            errno = x == 0. ? EDOM : ERANGE;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(floatUnlessError(globalObject, r)));
}

// degrees(x, /) and radians(x, /)
PYTHON_NATIVE(mathConvertAngle)
{
    bool isToDegrees = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(floatFromDouble(*x * (isToDegrees ? radToDeg : degToRad)));
}

enum class Classification : uint8_t { IsFinite, IsInfinite, IsNaN };

// isfinite(x, /), isinf(x, /) and isnan(x, /)
PYTHON_NATIVE(mathClassify)
{
    auto test = unpack<Classification>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(test == Classification::IsFinite ? isfinite(*x) : test == Classification::IsInfinite ? isinf(*x) : isnan(*x)));
}

// isclose(a, b, *, rel_tol=1e-09, abs_tol=0.0)
PYTHON_NATIVE(mathIsClose)
{
    NATIVE_PROLOGUE();
    auto a = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto b = toDouble(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    double relativeTolerance = 1e-09;
    if (JSValue given = args.at(2)) {
        auto value = toDouble(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        relativeTolerance = *value;
    }
    double absoluteTolerance = 0.0;
    if (JSValue given = args.at(3)) {
        auto value = toDouble(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        absoluteTolerance = *value;
    }
    if (relativeTolerance < 0.0 || absoluteTolerance < 0.0)
        return JSValue::encode(raiseValueError(globalObject, scope, "tolerances must be non-negative"_s));
    // Two infinities of the same sign among them
    if (*a == *b)
        return JSValue::encode(jsBoolean(true));
    if (isinf(*a) || isinf(*b))
        return JSValue::encode(jsBoolean(false));
    double difference = fabs(*b - *a);
    return JSValue::encode(jsBoolean(((difference <= fabs(relativeTolerance * *b)) || (difference <= fabs(relativeTolerance * *a))) || (difference <= absoluteTolerance)));
}

// prod(iterable, /, *, start=1)
PYTHON_NATIVE(mathProd)
{
    NATIVE_PROLOGUE();
    JSValue iterator = getIterator(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = args.at(1) ? args.at(1) : JSValue(jsNumber(1));

    // While they are all ints that there is room for the product of, it is kept as a number of C's.
    if (auto start = isInt(result) ? tryInt64(result) : std::nullopt) {
        long product = *start;
        result = { };
        while (!result) {
            JSValue item = iteratorNext(globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, { });
            if (!item)
                return JSValue::encode(intFromInt64(globalObject, product));
            if (auto b = isInt(item) ? tryInt64(item) : std::nullopt; b && !_check_long_mult_overflow(product, *b)) {
                product = product * *b;
                continue;
            }
            result = multiply(globalObject, intFromInt64(globalObject, product), item);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }

    // And likewise while they are floats, or ints that there is room for
    if (isFloat(result)) {
        double product = classify(result).real;
        result = { };
        while (!result) {
            JSValue item = iteratorNext(globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, { });
            if (!item)
                return JSValue::encode(floatFromDouble(product));
            if (isFloat(item)) {
                product *= classify(item).real;
                continue;
            }
            if (auto value = isInt(item) ? tryInt64(item) : std::nullopt) {
                product *= static_cast<double>(*value);
                continue;
            }
            result = multiply(globalObject, floatFromDouble(product), item);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }

    while (true) {
        JSValue item = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            return JSValue::encode(result);
        result = multiply(globalObject, result, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
}

// perm(n, k=None, /)
PYTHON_NATIVE(mathPerm)
{
    NATIVE_PROLOGUE();
    JSValue n = args[0];
    JSValue k = args.at(1);
    if (!k || isNone(k))
        RELEASE_AND_RETURN(scope, JSValue::encode(factorial(globalObject, n)));
    toCounts(globalObject, n, k);
    RETURN_IF_EXCEPTION(scope, { });
    if (compareInts(n, k) < 0)
        return JSValue::encode(jsNumber(0));
    auto smallK = tryInt64(k);
    if (!smallK)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, concatenate("k must not exceed "_s, static_cast<int64_t>(LLONG_MAX))));
    if (auto smallN = tryInt64(n); smallN && *smallK > 1)
        RELEASE_AND_RETURN(scope, JSValue::encode(permutationsOrCombinationsOfSmall(globalObject, *smallN, *smallK, false)));
    RELEASE_AND_RETURN(scope, JSValue::encode(permutationsOrCombinations(globalObject, n, *smallK, false)));
}

// comb(n, k, /)
PYTHON_NATIVE(mathComb)
{
    NATIVE_PROLOGUE();
    JSValue n = args[0];
    JSValue k = args[1];
    toCounts(globalObject, n, k);
    RETURN_IF_EXCEPTION(scope, { });
    long long count;
    if (auto smallN = tryInt64(n)) {
        auto smallK = tryInt64(k);
        if (!smallK || *smallK > *smallN)
            return JSValue::encode(jsNumber(0));
        count = std::min<long long>(*smallK, *smallN - *smallK);
        if (count > 1)
            RELEASE_AND_RETURN(scope, JSValue::encode(permutationsOrCombinationsOfSmall(globalObject, *smallN, count, true)));
        // If it is 1, what is returned is n as it is.
    } else {
        // k = min(k, n - k)
        JSValue rest = subtract(globalObject, n, k);
        RETURN_IF_EXCEPTION(scope, { });
        if (isNegative(rest))
            return JSValue::encode(jsNumber(0));
        if (compareInts(rest, k) < 0)
            k = rest;
        auto smallK = tryInt64(k);
        if (!smallK)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, concatenate("min(n - k, k) must not exceed "_s, static_cast<int64_t>(LLONG_MAX))));
        count = *smallK;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(permutationsOrCombinations(globalObject, n, count, true)));
}

// nextafter(x, y, /, *, steps=None)
PYTHON_NATIVE(mathNextAfter)
{
    NATIVE_PROLOGUE();
    auto givenX = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto givenY = toDouble(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    double x = *givenX;
    double y = *givenY;
    JSValue steps = args.at(2);
    if (!steps || isNone(steps))
        return JSValue::encode(floatFromDouble(nextafter(x, y)));
    steps = toInt(globalObject, steps);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNegative(steps))
        return JSValue::encode(raiseValueError(globalObject, scope, "steps must be a non-negative integer"_s));
    // There are fewer floats than this, so more will do no more.
    uint64_t count = lowBitsOfInt(steps);
    if (compareInts(steps, intFromUInt64(globalObject, count)))
        count = UINT64_MAX;
    if (!count || isnan(x))
        return JSValue::encode(floatFromDouble(x));
    if (isnan(y))
        return JSValue::encode(floatFromDouble(y));
    uint64_t ux = std::bit_cast<uint64_t>(x);
    uint64_t uy = std::bit_cast<uint64_t>(y);
    if (ux == uy)
        return JSValue::encode(floatFromDouble(x));
    constexpr uint64_t signBit = 1ULL << 63;
    uint64_t ax = ux & ~signBit;
    uint64_t ay = uy & ~signBit;
    auto result = [] (uint64_t bits) { return JSValue::encode(floatFromDouble(std::bit_cast<double>(bits))); };
    if ((ux ^ uy) & signBit) {
        // Of opposite signs. Neither has its top bit set, so there is room for the two together.
        if (ax + ay <= count)
            return result(uy);
        // Not <=, which would take +0.0 for -0.0
        if (ax < count)
            return result((uy & signBit) | (count - ax));
        return result(ux - count);
    }
    if (ax > ay)
        return result(ax - ay >= count ? ux - count : uy);
    return result(ay - ax >= count ? ux + count : uy);
}

// ulp(x, /)
PYTHON_NATIVE(mathUlp)
{
    NATIVE_PROLOGUE();
    auto given = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    double x = *given;
    if (isnan(x))
        return JSValue::encode(floatFromDouble(x));
    x = fabs(x);
    if (isinf(x))
        return JSValue::encode(floatFromDouble(x));
    constexpr double infinity = std::numeric_limits<double>::infinity();
    double x2 = nextafter(x, infinity);
    if (isinf(x2)) {
        // It is the largest float that there is.
        x2 = nextafter(x, -infinity);
        return JSValue::encode(floatFromDouble(x - x2));
    }
    return JSValue::encode(floatFromDouble(x2 - x));
}

JSObject* createMathModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "math"_s);
    auto addFunctionNamed = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, module, name, function, data); };
    for (unsigned i = 0; i < std::size(functionsOfOne); ++i)
        addFunctionNamed(functionsOfOne[i].name, mathFunctionOfOne, pack(i));
    for (unsigned i = 0; i < std::size(functionsOfTwo); ++i)
        addFunctionNamed(functionsOfTwo[i].name, mathFunctionOfTwo, pack(i));
    addFunctionNamed("ceil"_s, mathCeilOrFloor, pack(true));
    addFunctionNamed("comb"_s, mathComb);
    addFunctionNamed("degrees"_s, mathConvertAngle, pack(true));
    addFunctionNamed("dist"_s, mathDist);
    addFunctionNamed("factorial"_s, mathFactorial);
    addFunctionNamed("floor"_s, mathCeilOrFloor, pack(false));
    addFunctionNamed("fma"_s, mathFma);
    addFunctionNamed("fmod"_s, mathFmod);
    addFunctionNamed("frexp"_s, mathFrexp);
    addFunctionNamed("fsum"_s, mathFsum);
    addFunctionNamed("gcd"_s, mathGcd);
    addFunctionNamed("hypot"_s, mathHypot);
    addFunctionNamed("isclose"_s, mathIsClose);
    addFunctionNamed("isfinite"_s, mathClassify, pack(Classification::IsFinite));
    addFunctionNamed("isinf"_s, mathClassify, pack(Classification::IsInfinite));
    addFunctionNamed("isnan"_s, mathClassify, pack(Classification::IsNaN));
    addFunctionNamed("isqrt"_s, mathIsqrt);
    addFunctionNamed("lcm"_s, mathLcm);
    addFunctionNamed("ldexp"_s, mathLdexp);
    // It says nothing of what it takes, there being two ways to call it.
    addFunction(globalObject, module, "log"_s, mathLog, 0, "($module, /, *args)"_s, PyNativeFunction::Arguments::AreNotChecked);
    addFunctionNamed("log10"_s, mathLogToBase, pack(false));
    addFunctionNamed("log2"_s, mathLogToBase, pack(true));
    addFunctionNamed("modf"_s, mathModf);
    addFunctionNamed("nextafter"_s, mathNextAfter);
    addFunctionNamed("perm"_s, mathPerm);
    addFunctionNamed("pow"_s, mathPow);
    addFunctionNamed("prod"_s, mathProd);
    addFunctionNamed("radians"_s, mathConvertAngle, pack(false));
    addFunctionNamed("sumprod"_s, mathSumprod);
    addFunctionNamed("trunc"_s, mathTrunc);
    addFunctionNamed("ulp"_s, mathUlp);

    auto constant = [&] (ASCIILiteral name, double value) { module->putDirect(vm, Identifier::fromString(vm, name), floatFromDouble(value)); };
    constant("pi"_s, 3.14159265358979323846);
    constant("e"_s, 2.7182818284590452354);
    constant("tau"_s, 6.2831853071795864769252867665590057683943);
    constant("inf"_s, std::numeric_limits<double>::infinity());
    constant("nan"_s, fabs(std::numeric_limits<double>::quiet_NaN()));
    return module;
}

} } // namespace JSC::Python

// What comes after this in the same translation unit is compiled as the rest of the engine is.
#if COMPILER(CLANG)
#pragma STDC FP_CONTRACT OFF
#endif
