/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyModule.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTuple.h"
#include <bit>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <numeric>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The modules math, math.integer and _statistics. (Modules/mathmodule.c, mathintegermodule.c, _statisticsmodule.c in CPython.)
//
// What libm computes is asked of libm. What CPython computes itself (gamma, lgamma, fsum, hypot, dist, sumprod, the logarithm
// of a huge int) is computed here by the same steps in the same order, since every rounding shows in the result.

static constexpr double mathPi = 3.141592653589793238462643383279502884197;
static constexpr double mathE = 2.718281828459045235360287471352662497757;
static constexpr double mathInfinity = std::numeric_limits<double>::infinity();
static constexpr double mathNaN = std::numeric_limits<double>::quiet_NaN();

static ALWAYS_INLINE bool mathIsExactFloat(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isNumber())
        return isFloat(value);
    return PyFloat::is(value) && uncheckedDowncast<PyFloat>(value.asCell())->type() == runtime(globalObject).floatType();
}

static ALWAYS_INLINE unsigned mathBitLength(uint64_t value) { return 64 - std::countl_zero(value); }

// ---- Errors ----

// `expected` is the start of "expected a finite input, got inf"; a null one is for a function with nothing to say.
static JSValue mathDomainError(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral expected, double x)
{
    if (expected.isNull())
        return raiseValueError(globalObject, scope, "math domain error"_s);
    return raiseValueError(globalObject, scope, expected, ", got "_s, floatRepr(x));
}

// After a call that left something in errno. False: it is nothing to complain of.
static bool mathRaiseForErrno(JSGlobalObject* globalObject, ThrowScope& scope, double result)
{
    int error = errno;
    if (error == EDOM) {
        raiseValueError(globalObject, scope, "math domain error"_s);
        return true;
    }
    if (error == ERANGE) {
        // libm says this of an underflow too.
        if (std::abs(result) < 1.5)
            return false;
        raiseOverflowError(globalObject, scope, "math range error"_s);
        return true;
    }
    raiseWithArgs(globalObject, scope, runtime(globalObject).ValueError(), { jsNumber(error), makeStr(globalObject, String::fromUTF8(strerror(error))) });
    return true;
}

// ---- Functions of libm ----

// The error is told from the result, whatever the platform does with errno.
static JSValue mathCall1(JSGlobalObject* globalObject, JSValue argument, double (*function)(double), bool canOverflow, ASCIILiteral expected)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double x = toDouble(globalObject, argument);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    double result = function(x);
    if (std::isnan(result) && !std::isnan(x))
        return mathDomainError(globalObject, scope, expected, x);
    if (std::isinf(result) && std::isfinite(x)) {
        if (canOverflow)
            return raiseOverflowError(globalObject, scope, "math range error"_s);
        return mathDomainError(globalObject, scope, expected, x);
    }
    if (std::isfinite(result) && errno && mathRaiseForErrno(globalObject, scope, result))
        return { };
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, result));
}

// For a function that can be trusted to set errno.
static JSValue mathCall1WithErrno(JSGlobalObject* globalObject, JSValue argument, double (*function)(double), ASCIILiteral expected)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double x = toDouble(globalObject, argument);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    double result = function(x);
    if (errno) {
        if (errno == EDOM && !expected.isNull())
            return mathDomainError(globalObject, scope, expected, x);
        if (mathRaiseForErrno(globalObject, scope, result))
            return { };
    }
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, result));
}

static JSValue mathCall2(JSGlobalObject* globalObject, JSValue first, JSValue second, double (*function)(double, double))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double x = toDouble(globalObject, first);
    RETURN_IF_EXCEPTION(scope, { });
    double y = toDouble(globalObject, second);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    double result = function(x, y);
    if (std::isnan(result))
        errno = !std::isnan(x) && !std::isnan(y) ? EDOM : 0;
    else if (std::isinf(result))
        errno = std::isfinite(x) && std::isfinite(y) ? ERANGE : 0;
    if (errno && mathRaiseForErrno(globalObject, scope, result))
        return { };
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, result));
}

// ---- gamma, lgamma: Lanczos' approximation ----

static constexpr double mathLanczosG = 6.024680040776729583740234375;
static constexpr double mathLanczosGMinusHalf = 5.524680040776729583740234375;
static constexpr unsigned mathLanczosTerms = 13;
static constexpr double mathLanczosNumerator[mathLanczosTerms] = {
    23531376880.410759688572007674451636754734846804940,
    42919803642.649098768957899047001988850926355848959,
    35711959237.355668049440185451547166705960488635843,
    17921034426.037209699919755754458931112671403265390,
    6039542586.3520280050642916443072979210699388420708,
    1439720407.3117216736632230727949123939715485786772,
    248874557.86205415651146038641322942321632125127801,
    31426415.585400194380614231628318205362874684987640,
    2876370.6289353724412254090516208496135991145378768,
    186056.26539522349504029498971604569928220784236328,
    8071.6720023658162106380029022722506138218516325024,
    210.82427775157934587250973392071336271166969580291,
    2.5066282746310002701649081771338373386264310793408
};
static constexpr double mathLanczosDenominator[mathLanczosTerms] = {
    0.0, 39916800.0, 120543840.0, 150917976.0, 105258076.0, 45995730.0, 13339535.0, 2637558.0, 357423.0, 32670.0, 1925.0, 66.0, 1.0
};

static double mathLanczosSum(double x)
{
    double numerator = 0;
    double denominator = 0;
    // Both polynomials overflow for a large x, so there they are divided through by x**12.
    if (x < 5.0) {
        for (unsigned i = mathLanczosTerms; i--;) {
            numerator = numerator * x + mathLanczosNumerator[i];
            denominator = denominator * x + mathLanczosDenominator[i];
        }
    } else {
        for (unsigned i = 0; i < mathLanczosTerms; ++i) {
            numerator = numerator / x + mathLanczosNumerator[i];
            denominator = denominator / x + mathLanczosDenominator[i];
        }
    }
    return numerator / denominator;
}

// sin(pi * x), for a finite x, without the error of the product.
static double mathSinPi(double x)
{
    double y = std::fmod(std::abs(x), 2.0);
    double result;
    switch (static_cast<int>(std::round(2.0 * y))) {
    case 0:
        result = std::sin(mathPi * y);
        break;
    case 1:
        result = std::cos(mathPi * (y - 0.5));
        break;
    case 2:
        result = std::sin(mathPi * (1.0 - y)); // Not -sin(pi * (y - 1)), which is -0.0 for y = 1.
        break;
    case 3:
        result = -std::cos(mathPi * (y - 1.5));
        break;
    default:
        result = std::sin(mathPi * (y - 2.0));
        break;
    }
    return std::copysign(1.0, x) * result;
}

static double mathGammaOf(double x)
{
    if (!std::isfinite(x)) {
        if (std::isnan(x) || x > 0)
            return x;
        errno = EDOM;
        return mathNaN;
    }
    if (!x) {
        errno = EDOM;
        return std::copysign(mathInfinity, x);
    }
    if (x == std::floor(x)) {
        if (x < 0) {
            errno = EDOM;
            return mathNaN;
        }
        // Up to 22!, a factorial is a double.
        if (x <= 23) {
            double factorial = 1;
            for (int i = 2; i < static_cast<int>(x); ++i)
                factorial *= i;
            return factorial;
        }
    }

    double magnitude = std::abs(x);
    if (magnitude < 1e-20) {
        double result = 1.0 / x;
        if (std::isinf(result))
            errno = ERANGE;
        return result;
    }
    if (magnitude > 200.0) {
        if (x < 0)
            return 0.0 / mathSinPi(x);
        errno = ERANGE;
        return mathInfinity;
    }

    double y = magnitude + mathLanczosGMinusHalf;
    // The rounding error of that sum.
    double z;
    if (magnitude > mathLanczosGMinusHalf) {
        double q = y - magnitude;
        z = q - mathLanczosGMinusHalf;
    } else {
        double q = y - mathLanczosGMinusHalf;
        z = q - magnitude;
    }
    z = z * mathLanczosG / y;

    double result;
    if (x < 0) {
        result = -mathPi / mathSinPi(magnitude) / magnitude * std::exp(y) / mathLanczosSum(magnitude);
        result -= z * result;
        if (magnitude < 140.0)
            result /= std::pow(y, magnitude - 0.5);
        else {
            double root = std::pow(y, magnitude / 2.0 - 0.25);
            result /= root;
            result /= root;
        }
    } else {
        result = mathLanczosSum(magnitude) / std::exp(y);
        result += z * result;
        if (magnitude < 140.0)
            result *= std::pow(y, magnitude - 0.5);
        else {
            double root = std::pow(y, magnitude / 2.0 - 0.25);
            result *= root;
            result *= root;
        }
    }
    if (std::isinf(result))
        errno = ERANGE;
    return result;
}

static double mathLogGammaOf(double x)
{
    static constexpr double logPi = 1.144729885849400174143427351353058711647;
    if (!std::isfinite(x))
        return std::isnan(x) ? x : mathInfinity;
    if (x == std::floor(x) && x <= 2.0) {
        if (x <= 0) {
            errno = EDOM;
            return mathInfinity;
        }
        return 0.0;
    }
    double magnitude = std::abs(x);
    if (magnitude < 1e-20)
        return -std::log(magnitude);
    double result = std::log(mathLanczosSum(magnitude)) - mathLanczosG;
    result += (magnitude - 0.5) * (std::log(magnitude + mathLanczosG - 0.5) - 1);
    if (x < 0)
        result = logPi - std::log(std::abs(mathSinPi(magnitude))) - std::log(magnitude) - result;
    if (std::isinf(result))
        errno = ERANGE;
    return result;
}

static double mathLog1pOf(double x)
{
    return !x ? x : std::log1p(x); // Some platforms lose the sign of -0.0.
}

#define MATH_DEFINE_1(name, function, canOverflow, expected) \
    static JSValue name(JSGlobalObject* globalObject, JSValue x) { return mathCall1(globalObject, x, function, canOverflow, expected); }
#define MATH_DEFINE_1_WITH_ERRNO(name, function, expected) \
    static JSValue name(JSGlobalObject* globalObject, JSValue x) { return mathCall1WithErrno(globalObject, x, function, expected); }
#define MATH_DEFINE_2(name, function) \
    static JSValue name(JSGlobalObject* globalObject, JSValue x, JSValue y) { return mathCall2(globalObject, x, y, function); }

MATH_DEFINE_1(mathAcos, ::acos, false, "expected a number in range from -1 up to 1"_s)
MATH_DEFINE_1(mathAcosh, ::acosh, false, "expected argument value not less than 1"_s)
MATH_DEFINE_1(mathAsin, ::asin, false, "expected a number in range from -1 up to 1"_s)
MATH_DEFINE_1(mathAsinh, ::asinh, false, { })
MATH_DEFINE_1(mathAtan, ::atan, false, { })
MATH_DEFINE_2(mathAtan2, ::atan2)
MATH_DEFINE_1(mathAtanh, ::atanh, false, "expected a number between -1 and 1"_s)
MATH_DEFINE_1(mathCbrt, ::cbrt, false, { })
MATH_DEFINE_2(mathCopysign, ::copysign)
MATH_DEFINE_1(mathCos, ::cos, false, "expected a finite input"_s)
MATH_DEFINE_1(mathCosh, ::cosh, true, { })
MATH_DEFINE_1_WITH_ERRNO(mathErf, ::erf, { })
MATH_DEFINE_1_WITH_ERRNO(mathErfc, ::erfc, { })
MATH_DEFINE_1(mathExp, ::exp, true, { })
MATH_DEFINE_1(mathExp2, ::exp2, true, { })
MATH_DEFINE_1(mathExpm1, ::expm1, true, { })
MATH_DEFINE_1(mathFabs, ::fabs, false, { })
MATH_DEFINE_1_WITH_ERRNO(mathGamma, mathGammaOf, "expected a noninteger or positive integer"_s)
MATH_DEFINE_1_WITH_ERRNO(mathLgamma, mathLogGammaOf, "expected a noninteger or positive integer"_s)
MATH_DEFINE_1(mathLog1p, mathLog1pOf, false, "expected argument value > -1"_s)
MATH_DEFINE_2(mathRemainder, ::remainder)
MATH_DEFINE_1(mathSin, ::sin, false, "expected a finite input"_s)
MATH_DEFINE_1(mathSinh, ::sinh, true, { })
MATH_DEFINE_1(mathSqrt, ::sqrt, false, "expected a nonnegative input"_s)
MATH_DEFINE_1(mathTan, ::tan, false, "expected a finite input"_s)
MATH_DEFINE_1(mathTanh, ::tanh, false, { })

#undef MATH_DEFINE_1
#undef MATH_DEFINE_1_WITH_ERRNO
#undef MATH_DEFINE_2

// ---- Rounding to an int ----

static JSValue mathRoundToInt(JSGlobalObject* globalObject, JSValue number, Special special, double (*function)(double))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isExactInt(number))
        return number;
    double x;
    if (mathIsExactFloat(globalObject, number))
        x = floatValue(number);
    else {
        JSValue method = lookupSpecial(globalObject, number, special);
        RETURN_IF_EXCEPTION(scope, { });
        if (method)
            RELEASE_AND_RETURN(scope, call(globalObject, method));
        x = toDouble(globalObject, number);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, intFromDouble(globalObject, function(x)));
}

static JSValue mathCeil(JSGlobalObject* globalObject, JSValue x) { return mathRoundToInt(globalObject, x, Special::Ceil, ::ceil); }
static JSValue mathFloor(JSGlobalObject* globalObject, JSValue x) { return mathRoundToInt(globalObject, x, Special::Floor, ::floor); }

static JSValue mathTrunc(JSGlobalObject* globalObject, JSValue x)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isExactInt(x))
        return x;
    if (mathIsExactFloat(globalObject, x))
        RELEASE_AND_RETURN(scope, intFromDouble(globalObject, std::trunc(floatValue(x))));
    JSValue method = lookupSpecial(globalObject, x, Special::Trunc);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return raiseTypeError(globalObject, scope, "type "_s, typeName(globalObject, x), " doesn't define __trunc__ method"_s);
    RELEASE_AND_RETURN(scope, call(globalObject, method));
}

// ---- Functions of floats ----

#define MATH_GET_DOUBLE(name, value) \
    double name = toDouble(globalObject, value); \
    RETURN_IF_EXCEPTION(scope, { })

static JSValue mathFmax(JSGlobalObject* globalObject, JSValue first, JSValue second)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, first);
    MATH_GET_DOUBLE(y, second);
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, std::fmax(x, y)));
}

static JSValue mathFmin(JSGlobalObject* globalObject, JSValue first, JSValue second)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, first);
    MATH_GET_DOUBLE(y, second);
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, std::fmin(x, y)));
}

static JSValue mathFmod(JSGlobalObject* globalObject, JSValue first, JSValue second)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, first);
    MATH_GET_DOUBLE(y, second);
    if (std::isinf(y) && std::isfinite(x))
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, x));
    errno = 0;
    double result = std::fmod(x, y);
    if (std::isnan(result))
        errno = !std::isnan(x) && !std::isnan(y) ? EDOM : 0;
    if (errno && mathRaiseForErrno(globalObject, scope, result))
        return { };
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, result));
}

static JSValue mathPow(JSGlobalObject* globalObject, JSValue first, JSValue second)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, first);
    MATH_GET_DOUBLE(y, second);
    errno = 0;
    double result = std::pow(x, y);
    // With an infinity or a NaN among the arguments, what C99 says the result is stands.
    if (!std::isfinite(x) || !std::isfinite(y))
        errno = 0;
    else if (std::isnan(result))
        errno = EDOM;
    else if (std::isinf(result))
        errno = !x ? EDOM : ERANGE;
    if (errno && mathRaiseForErrno(globalObject, scope, result))
        return { };
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, result));
}

static JSValue mathFma(JSGlobalObject* globalObject, JSValue first, JSValue second, JSValue third)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, first);
    MATH_GET_DOUBLE(y, second);
    MATH_GET_DOUBLE(z, third);
    double result = std::fma(x, y, z);
    if (std::isnan(result)) {
        if (!std::isnan(x) && !std::isnan(y) && !std::isnan(z))
            return raiseValueError(globalObject, scope, "invalid operation in fma"_s);
    } else if (std::isinf(result) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z))
        return raiseOverflowError(globalObject, scope, "overflow in fma"_s);
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, result));
}

static JSValue mathFrexp(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, argument);
    int exponent = 0;
    if (std::isfinite(x) && x)
        x = std::frexp(x, &exponent);
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { makeFloat(globalObject, x), jsNumber(exponent) }));
}

static JSValue mathLdexp(JSGlobalObject* globalObject, JSValue first, JSValue second)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, first);
    if (!isInt(second))
        return raiseTypeError(globalObject, scope, "Expected an int as second argument to ldexp."_s);
    JSValue exponentValue = intValue(second);
    int64_t exponent;
    if (auto converted = tryInt64(exponentValue))
        exponent = *converted;
    else
        exponent = intSign(exponentValue) < 0 ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();

    double result;
    errno = 0;
    if (!x || !std::isfinite(x))
        result = x;
    else if (exponent > std::numeric_limits<int>::max()) {
        result = std::copysign(mathInfinity, x);
        errno = ERANGE;
    } else if (exponent < std::numeric_limits<int>::min())
        result = std::copysign(0.0, x);
    else {
        result = std::ldexp(x, static_cast<int>(exponent));
        if (std::isinf(result))
            errno = ERANGE;
    }
    if (errno && mathRaiseForErrno(globalObject, scope, result))
        return { };
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, result));
}

static JSValue mathModf(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, argument);
    double fraction;
    double whole;
    if (std::isinf(x)) {
        fraction = std::copysign(0.0, x);
        whole = x;
    } else if (std::isnan(x))
        fraction = whole = x;
    else
        fraction = std::modf(x, &whole);
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { makeFloat(globalObject, fraction), makeFloat(globalObject, whole) }));
}

static JSValue mathDegrees(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, argument);
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, x * (180.0 / mathPi)));
}

static JSValue mathRadians(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, argument);
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, x * (mathPi / 180.0)));
}

template<bool (*predicate)(double)>
static JSValue mathPredicate(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, argument);
    return jsBoolean(predicate(x));
}

static bool mathIsFiniteValue(double x) { return std::isfinite(x); }
static bool mathIsInfValue(double x) { return std::isinf(x); }
static bool mathIsNaNValue(double x) { return std::isnan(x); }
static bool mathIsNormalValue(double x) { return std::isnormal(x); }
static bool mathIsSubnormalValue(double x) { return std::fpclassify(x) == FP_SUBNORMAL; }
static bool mathSignBitValue(double x) { return std::signbit(x); }

static JSValue mathIsClose(JSGlobalObject* globalObject, JSValue first, JSValue second, JSValue relativeTolerance, JSValue absoluteTolerance)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(a, first);
    MATH_GET_DOUBLE(b, second);
    double relative = 1e-09;
    if (relativeTolerance) {
        relative = toDouble(globalObject, relativeTolerance);
        RETURN_IF_EXCEPTION(scope, { });
    }
    double absolute = 0;
    if (absoluteTolerance) {
        absolute = toDouble(globalObject, absoluteTolerance);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (relative < 0 || absolute < 0)
        return raiseValueError(globalObject, scope, "tolerances must be non-negative"_s);
    if (a == b)
        return jsBoolean(true);
    if (std::isinf(a) || std::isinf(b))
        return jsBoolean(false);
    double difference = std::abs(b - a);
    return jsBoolean(difference <= std::abs(relative * b) || difference <= std::abs(relative * a) || difference <= absolute);
}

static JSValue mathNextAfter(JSGlobalObject* globalObject, JSValue first, JSValue second, JSValue stepsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, first);
    MATH_GET_DOUBLE(y, second);
    if (!stepsValue || isNone(stepsValue))
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, std::nextafter(x, y)));

    JSValue index = toIndex(globalObject, stepsValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (intSign(index) < 0)
        return raiseValueError(globalObject, scope, "steps must be a non-negative integer"_s);
    uint64_t steps = tryUInt64(index).value_or(std::numeric_limits<uint64_t>::max());
    if (!steps || std::isnan(x))
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, x));
    if (std::isnan(y))
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, y));

    // The floats in order are the integers in order, once those with the sign bit are taken for negative; both zeros are 0.
    static constexpr uint64_t signBit = 1ULL << 63;
    auto position = [](double value) {
        uint64_t bits = std::bit_cast<uint64_t>(value);
        return bits & signBit ? -static_cast<int64_t>(bits & ~signBit) : static_cast<int64_t>(bits);
    };
    int64_t from = position(x);
    int64_t to = position(y);
    uint64_t distance = from < to ? static_cast<uint64_t>(to) - static_cast<uint64_t>(from) : static_cast<uint64_t>(from) - static_cast<uint64_t>(to);
    if (distance <= steps)
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, y));
    int64_t reached = static_cast<int64_t>(from < to ? static_cast<uint64_t>(from) + steps : static_cast<uint64_t>(from) - steps);
    if (!reached)
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, std::copysign(0.0, x)));
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, std::bit_cast<double>(reached < 0 ? signBit | static_cast<uint64_t>(-reached) : static_cast<uint64_t>(reached))));
}

static JSValue mathUlp(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(x, argument);
    if (std::isnan(x))
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, x));
    x = std::abs(x);
    if (std::isinf(x))
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, x));
    double next = std::nextafter(x, mathInfinity);
    if (std::isinf(next))
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, x - std::nextafter(x, -mathInfinity)));
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, next - x));
}

// ---- Logarithms ----

// value == result * 2**exponent, with 0.5 <= result < 1 rounded half to even, for a positive int too large for a double.
static double mathFrexpOfHugeInt(JSValue value, int64_t& exponent)
{
    JSBigInt* bigInt = value.asHeapBigInt();
    static_assert(sizeof(JSBigInt::Digit) == sizeof(uint64_t));
    unsigned length = bigInt->length();
    ASSERT(length >= 2);
    uint64_t high = bigInt->digit(length - 1);
    uint64_t next = bigInt->digit(length - 2);
    unsigned unused = std::countl_zero(high);
    uint64_t leading = unused ? (high << unused) | (next >> (64 - unused)) : high;
    bool sticky = unused ? next << unused : next;
    for (unsigned i = 0; !sticky && i + 2 < length; ++i)
        sticky = bigInt->digit(i);

    uint64_t mantissa = leading >> 11;
    uint64_t rest = leading & 0x7ff;
    if (rest > 0x400 || (rest == 0x400 && (sticky || (mantissa & 1))))
        ++mantissa;
    exponent = static_cast<int64_t>(length) * 64 - unused;
    double result = std::ldexp(static_cast<double>(mantissa), -53);
    if (result == 1.0) {
        result = 0.5;
        ++exponent;
    }
    return result;
}

static JSValue mathLogOfInt(JSGlobalObject* globalObject, JSValue value, double (*function)(double))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (intSign(value) <= 0)
        return raiseValueError(globalObject, scope, "expected a positive input"_s);
    double x = intToDouble(value);
    if (!std::isinf(x))
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, function(x)));
    int64_t exponent;
    x = mathFrexpOfHugeInt(value, exponent);
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, std::fma(function(2.0), static_cast<double>(exponent), function(x))));
}

static JSValue mathLogOf(JSGlobalObject* globalObject, JSValue argument, double (*function)(double))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isInt(argument))
        RELEASE_AND_RETURN(scope, mathLogOfInt(globalObject, intValue(argument), function));
    JSValue result = mathCall1(globalObject, argument, function, false, "expected a positive input"_s);
    if (!failed(scope, result)) [[likely]]
        return result;
    // An object whose __index__() is too large for a float.
    if (!pendingExceptionMatches(globalObject, runtime(globalObject).OverflowError()) || !hasIndex(globalObject, argument))
        return { };
    takePendingException(globalObject);
    JSValue index = toIndex(globalObject, argument);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, mathLogOfInt(globalObject, index, function));
}

// log(x, [base=math.e]): a signature that cannot be written, so it has none.
static JSValue mathLog(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "math.log"_s, args)) || failed(scope, checkPositional(globalObject, "log"_s, args, 1, 2)))
        return { };
    JSValue numerator = mathLogOf(globalObject, args[0], ::log);
    RETURN_IF_EXCEPTION(scope, { });
    if (args.size() == 1)
        return numerator;
    JSValue denominator = mathLogOf(globalObject, args[1], ::log);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, floatBinaryOperation(globalObject, BinaryOperator::Div, floatValue(numerator), floatValue(denominator)));
}

static JSValue mathLog2(JSGlobalObject* globalObject, JSValue x) { return mathLogOf(globalObject, x, ::log2); }
static JSValue mathLog10(JSGlobalObject* globalObject, JSValue x) { return mathLogOf(globalObject, x, ::log10); }

// ---- Sums ----

namespace {

// hi + lo is the exact result of an operation that hi is the rounded result of.
struct MathDoubleLength {
    double hi;
    double lo;
};

struct MathTripleLength {
    double hi { 0 };
    double lo { 0 };
    double tiny { 0 };
};

}

// For |a| >= |b|.
static ALWAYS_INLINE MathDoubleLength mathFastTwoSum(double a, double b)
{
    double x = a + b;
    return { x, (a - x) + b };
}

static ALWAYS_INLINE MathDoubleLength mathTwoSum(double a, double b)
{
    double x = a + b;
    double z = x - a;
    return { x, (a - (x - z)) + (b - z) };
}

static ALWAYS_INLINE MathDoubleLength mathTwoProduct(double x, double y)
{
    double z = x * y;
    return { z, std::fma(x, y, -z) };
}

static ALWAYS_INLINE MathTripleLength mathAddProduct(double x, double y, MathTripleLength total)
{
    MathDoubleLength product = mathTwoProduct(x, y);
    MathDoubleLength sum = mathTwoSum(total.hi, product.hi);
    MathDoubleLength first = mathTwoSum(total.lo, product.lo);
    MathDoubleLength second = mathTwoSum(first.hi, sum.lo);
    return { sum.hi, second.hi, total.tiny + first.lo + second.lo };
}

static ALWAYS_INLINE double mathRounded(MathTripleLength total)
{
    MathDoubleLength last = mathTwoSum(total.lo, total.hi);
    return total.tiny + last.lo + last.hi;
}

// Shewchuk's algorithm: the sum so far is kept, exactly, as a sum of floats that do not overlap, in increasing magnitude.
static JSValue mathFsum(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<double, 32> partials;
    double specialSum = 0;
    double infinitySum = 0;

    forEach(globalObject, iterable, [&](JSValue item) {
        double x = toDouble(globalObject, item);
        RETURN_IF_EXCEPTION(scope, false);
        double original = x;
        size_t kept = 0;
        for (double y : partials) {
            if (std::abs(x) < std::abs(y))
                std::swap(x, y);
            double hi = x + y;
            double lo = y - (hi - x);
            if (lo)
                partials[kept++] = lo;
            x = hi;
        }
        partials.shrink(kept);
        if (!x)
            return true;
        if (std::isfinite(x)) {
            partials.append(x);
            return true;
        }
        if (std::isfinite(original)) {
            raiseOverflowError(globalObject, scope, "intermediate overflow in fsum"_s);
            return false;
        }
        if (std::isinf(original))
            infinitySum += original;
        specialSum += original;
        partials.shrink(0);
        return true;
    });
    RETURN_IF_EXCEPTION(scope, { });

    if (specialSum) {
        if (std::isnan(infinitySum))
            return raiseValueError(globalObject, scope, "-inf + inf in fsum"_s);
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, specialSum));
    }

    double hi = 0;
    size_t n = partials.size();
    if (n) {
        hi = partials[--n];
        double lo = 0;
        // From the largest down, until a sum is inexact.
        while (n) {
            double x = hi;
            double y = partials[--n];
            hi = x + y;
            lo = y - (hi - x);
            if (lo)
                break;
        }
        // A tie that was rounded to even is no tie if what is left pulls the same way.
        if (n && ((lo < 0 && partials[n - 1] < 0) || (lo > 0 && partials[n - 1] > 0))) {
            double y = lo * 2.0;
            double x = hi + y;
            if (y == x - hi)
                hi = x;
        }
    }
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, hi));
}

// sqrt(sum(x * x for x in values)), where `max` is the largest of the values, none of which is negative.
static double mathVectorNorm(std::span<double> values, double max, bool foundNaN)
{
    if (std::isinf(max))
        return max;
    if (foundNaN)
        return mathNaN;
    if (!max || values.size() <= 1)
        return max;

    int maxExponent;
    std::frexp(max, &maxExponent);
    if (maxExponent < -1023) {
        // The scale below would overflow.
        for (double& value : values)
            value /= DBL_MIN;
        return DBL_MIN * mathVectorNorm(values, max / DBL_MIN, foundNaN);
    }
    double scale = std::ldexp(1.0, -maxExponent);
    double sum = 1.0;
    double fraction1 = 0;
    double fraction2 = 0;
    auto add = [&](MathDoubleLength product) {
        MathDoubleLength total = mathFastTwoSum(sum, product.hi);
        sum = total.hi;
        fraction1 += product.lo;
        fraction2 += total.lo;
    };
    for (double x : values) {
        x *= scale;
        add(mathTwoProduct(x, x));
    }
    double h = std::sqrt(sum - 1.0 + (fraction1 + fraction2));
    // One step of Newton's method with what is left.
    add(mathTwoProduct(-h, h));
    double x = sum - 1.0 + (fraction1 + fraction2);
    h += x / (2.0 * h);
    return h / scale;
}

static JSValue mathHypot(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "math.hypot"_s, args)))
        return { };
    Vector<double, 28> coordinates(args.size());
    double max = 0;
    bool foundNaN = false;
    for (unsigned i = 0; i < args.size(); ++i) {
        MATH_GET_DOUBLE(x, args[i]);
        x = std::abs(x);
        coordinates[i] = x;
        foundNaN |= std::isnan(x);
        if (x > max)
            max = x;
    }
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, mathVectorNorm(coordinates.mutableSpan(), max, foundNaN)));
}

static JSValue mathDist(JSGlobalObject* globalObject, JSValue first, JSValue second)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* p = PyTuple::tryFrom(first);
    if (!p) {
        p = tupleFromIterable(globalObject, first);
        RETURN_IF_EXCEPTION(scope, { });
    }
    PyTuple* q = PyTuple::tryFrom(second);
    if (!q) {
        q = tupleFromIterable(globalObject, second);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (p->length() != q->length())
        return raiseValueError(globalObject, scope, "both points must have the same number of dimensions"_s);

    Vector<double, 28> differences(p->length());
    double max = 0;
    bool foundNaN = false;
    for (unsigned i = 0; i < p->length(); ++i) {
        MATH_GET_DOUBLE(px, p->at(i));
        MATH_GET_DOUBLE(qx, q->at(i));
        double x = std::abs(px - qx);
        differences[i] = x;
        foundNaN |= std::isnan(x);
        if (x > max)
            max = x;
    }
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, mathVectorNorm(differences.mutableSpan(), max, foundNaN)));
}

// Ints are summed exactly and floats in triple length for as long as there is nothing else; which of the three ways a pair
// is added in shows in the last bits of the result, so the rules for falling from one to the next are CPython's.
static JSValue mathSumProd(JSGlobalObject* globalObject, JSValue p, JSValue q)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue pIterator = getIter(globalObject, p);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue qIterator = getIter(globalObject, q);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue total = jsNumber(0);
    bool intPathEnabled = true;
    bool intTotalInUse = false;
    int64_t intTotal = 0;
    bool floatPathEnabled = true;
    bool floatTotalInUse = false;
    MathTripleLength floatTotal;

    auto addInts = [&](JSValue a, JSValue b) {
        if (!isExactInt(a) || !isExactInt(b))
            return false;
        auto x = tryInt64(a);
        auto y = tryInt64(b);
        int64_t product;
        int64_t sum;
        if (!x || !y || __builtin_mul_overflow(*x, *y, &product) || __builtin_add_overflow(intTotal, product, &sum))
            return false;
        intTotal = sum;
        intTotalInUse = true;
        return true;
    };
    auto addFloats = [&](JSValue a, JSValue b) {
        bool aIsFloat = mathIsExactFloat(globalObject, a);
        bool bIsFloat = mathIsExactFloat(globalObject, b);
        auto isIntOrBool = [](JSValue value) { return isExactInt(value) || value.isBoolean(); };
        double x;
        double y;
        if (aIsFloat && bIsFloat) {
            x = floatValue(a);
            y = floatValue(b);
        } else if (aIsFloat && isIntOrBool(b)) {
            x = floatValue(a);
            y = intToDouble(intValue(b));
            if (std::isinf(y))
                return false;
        } else if (bIsFloat && isIntOrBool(a)) {
            y = floatValue(b);
            x = intToDouble(intValue(a));
            if (std::isinf(x))
                return false;
        } else
            return false;
        MathTripleLength sum = mathAddProduct(x, y, floatTotal);
        if (!std::isfinite(sum.hi))
            return false;
        floatTotal = sum;
        floatTotalInUse = true;
        return true;
    };

    while (true) {
        JSValue a = iterNext(globalObject, pIterator);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue b = iterNext(globalObject, qIterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!a != !b)
            return raiseValueError(globalObject, scope, "Inputs are not the same length"_s);
        bool finished = !a;

        if (intPathEnabled) {
            if (!finished && addInts(a, b))
                continue;
            intPathEnabled = false;
            if (intTotalInUse) {
                total = binaryOperation(globalObject, BinaryOperator::Add, total, makeInt(globalObject, intTotal));
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        if (floatPathEnabled) {
            if (!finished && addFloats(a, b))
                continue;
            floatPathEnabled = false;
            if (floatTotalInUse) {
                total = binaryOperation(globalObject, BinaryOperator::Add, total, makeFloat(globalObject, mathRounded(floatTotal)));
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        if (finished)
            return total;

        JSValue term = binaryOperation(globalObject, BinaryOperator::Mult, a, b);
        RETURN_IF_EXCEPTION(scope, { });
        total = binaryOperation(globalObject, BinaryOperator::Add, total, term);
        RETURN_IF_EXCEPTION(scope, { });
    }
}

static JSValue mathProd(JSGlobalObject* globalObject, JSValue iterable, JSValue start)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = getIter(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = start ? start : jsNumber(1);

    // While the product is a machine number, no object is made for it.
    if (auto product = isExactInt(result) ? tryInt64(result) : std::nullopt) {
        while (true) {
            JSValue item = iterNext(globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, { });
            if (!item)
                RELEASE_AND_RETURN(scope, makeInt(globalObject, *product));
            if (auto factor = isExactInt(item) ? tryInt64(item) : std::nullopt) {
                int64_t next;
                if (!__builtin_mul_overflow(*product, *factor, &next)) {
                    product = next;
                    continue;
                }
            }
            result = binaryOperation(globalObject, BinaryOperator::Mult, makeInt(globalObject, *product), item);
            RETURN_IF_EXCEPTION(scope, { });
            break;
        }
    }
    if (mathIsExactFloat(globalObject, result)) {
        double product = floatValue(result);
        while (true) {
            JSValue item = iterNext(globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, { });
            if (!item)
                RELEASE_AND_RETURN(scope, makeFloat(globalObject, product));
            if (mathIsExactFloat(globalObject, item)) {
                product *= floatValue(item);
                continue;
            }
            if (auto factor = isExactInt(item) ? tryInt64(item) : std::nullopt) {
                product *= static_cast<double>(*factor);
                continue;
            }
            result = binaryOperation(globalObject, BinaryOperator::Mult, makeFloat(globalObject, product), item);
            RETURN_IF_EXCEPTION(scope, { });
            break;
        }
    }
    while (true) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            return result;
        result = binaryOperation(globalObject, BinaryOperator::Mult, result, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
}

// ---- math.integer ----

static JSValue mathGcdOfInts(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    a = intAbsolute(globalObject, a);
    RETURN_IF_EXCEPTION(scope, { });
    b = intAbsolute(globalObject, b);
    RETURN_IF_EXCEPTION(scope, { });
    while (true) {
        auto x = tryUInt64(a);
        auto y = tryUInt64(b);
        if (x && y)
            RELEASE_AND_RETURN(scope, makeInt(globalObject, std::gcd(*x, *y)));
        if (!intSign(b))
            return a;
        JSValue remainder = intModulo(globalObject, a, b);
        RETURN_IF_EXCEPTION(scope, { });
        a = b;
        b = remainder;
    }
}

static JSValue mathLcmOfInts(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!intSign(a) || !intSign(b))
        return jsNumber(0);
    JSValue divisor = mathGcdOfInts(globalObject, a, b);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue quotient = intFloorDivide(globalObject, a, divisor);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue product = intMultiply(globalObject, quotient, b);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, intAbsolute(globalObject, product));
}

// gcd and lcm: `absorbing` is the result that no further argument changes, though each must still be an int.
static JSValue mathFoldInts(JSGlobalObject* globalObject, const CallArgs& args, ASCIILiteral name, int identity, int absorbing, JSValue (*combine)(JSGlobalObject*, JSValue, JSValue))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, name, args)))
        return { };
    if (args.isEmpty())
        return jsNumber(identity);
    JSValue result = toIndex(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (args.size() == 1)
        RELEASE_AND_RETURN(scope, intAbsolute(globalObject, result));
    for (unsigned i = 1; i < args.size(); ++i) {
        JSValue x = toIndex(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSmallInt(result) && smallIntValue(result) == absorbing)
            continue;
        result = combine(globalObject, result, x);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue mathGcd(JSGlobalObject* globalObject, const CallArgs& args) { return mathFoldInts(globalObject, args, "math.integer.gcd"_s, 0, 1, mathGcdOfInts); }
static JSValue mathLcm(JSGlobalObject* globalObject, const CallArgs& args) { return mathFoldInts(globalObject, args, "math.integer.lcm"_s, 1, 0, mathLcmOfInts); }

static JSValue mathIsqrt(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue n = toIndex(globalObject, argument);
    RETURN_IF_EXCEPTION(scope, { });
    if (intSign(n) < 0)
        return raiseValueError(globalObject, scope, "isqrt() argument must be nonnegative"_s);

    if (auto small = tryUInt64(n)) {
        // The square root of the nearest double is off by one at most.
        static constexpr uint64_t maxRoot = 0xffffffff;
        uint64_t root = std::min(static_cast<uint64_t>(std::sqrt(static_cast<double>(*small))), maxRoot);
        while (root * root > *small)
            --root;
        while (root < maxRoot && (root + 1) * (root + 1) <= *small)
            ++root;
        RELEASE_AND_RETURN(scope, makeInt(globalObject, root));
    }

    // Newton's method on the leading bits of n, twice as many of them each time: `a` is within one of the root of
    // n >> 2 * (c - d), which has d + 1 bits.
    uint64_t c = (intBitLength(n) - 1) / 2;
    JSValue a = jsNumber(1);
    uint64_t d = 0;
    for (unsigned s = mathBitLength(c); s--;) {
        uint64_t e = d;
        d = c >> s;
        JSValue shifted = intLeftShift(globalObject, a, makeInt(globalObject, d - e - 1));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue leading = intRightShift(globalObject, n, makeInt(globalObject, 2 * c - e - d + 1));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue quotient = intFloorDivide(globalObject, leading, a);
        RETURN_IF_EXCEPTION(scope, { });
        a = intAdd(globalObject, shifted, quotient);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue square = intMultiply(globalObject, a, a);
    RETURN_IF_EXCEPTION(scope, { });
    if (compareInts(square, n) > 0)
        RELEASE_AND_RETURN(scope, intSubtract(globalObject, a, jsNumber(1)));
    return a;
}

// The product of the odd numbers from `start` up to but not including `stop`, both odd, by halves so that the factors of
// each multiplication are of a size.
static JSValue mathProductOfOdds(JSGlobalObject* globalObject, uint64_t start, uint64_t stop)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint64_t count = (stop - start) / 2;
    if (count <= 64 && count * mathBitLength(stop - 2) <= 64) {
        uint64_t product = start;
        for (uint64_t factor = start + 2; factor < stop; factor += 2)
            product *= factor;
        RELEASE_AND_RETURN(scope, makeInt(globalObject, product));
    }
    uint64_t middle = (start + count) | 1;
    JSValue left = mathProductOfOdds(globalObject, start, middle);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue right = mathProductOfOdds(globalObject, middle, stop);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, intMultiply(globalObject, left, right));
}

static JSValue mathFactorial(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, argument);
    RETURN_IF_EXCEPTION(scope, { });
    if (intSign(index) < 0)
        return raiseValueError(globalObject, scope, "factorial() not defined for negative values"_s);
    auto converted = tryInt64(index);
    if (!converted)
        return raiseOverflowError(globalObject, scope, "factorial() argument should not exceed 9223372036854775807"_s);
    uint64_t n = *converted;
    if (n <= 20) {
        uint64_t result = 1;
        for (uint64_t i = 2; i <= n; ++i)
            result *= i;
        RELEASE_AND_RETURN(scope, makeInt(globalObject, result));
    }

    // n! is 2**(n - popcount(n)) times, for each i, the product of the odd numbers up to n >> i. `inner` is that product,
    // which grows from one i to the next lower.
    JSValue inner = jsNumber(1);
    JSValue outer = jsNumber(1);
    uint64_t upper = 3;
    for (unsigned i = mathBitLength(n) - 1; i--;) {
        uint64_t limit = n >> i;
        if (limit <= 2)
            continue;
        uint64_t lower = upper;
        upper = (limit + 1) | 1;
        JSValue partial = mathProductOfOdds(globalObject, lower, upper);
        RETURN_IF_EXCEPTION(scope, { });
        inner = intMultiply(globalObject, inner, partial);
        RETURN_IF_EXCEPTION(scope, { });
        outer = intMultiply(globalObject, outer, inner);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, intLeftShift(globalObject, outer, makeInt(globalObject, n - std::popcount(n))));
}

// n * (n - 1) * ... * (n - k + 1), divided by k! for the combinations. n >= k.
static JSValue mathPermutationsOrCombinations(JSGlobalObject* globalObject, JSValue n, uint64_t k, bool combinations)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!k)
        return jsNumber(1);
    if (k == 1)
        return n;
    if (auto small = tryUInt64(n); small && k <= 64 && k * mathBitLength(*small) <= 64) {
        // No intermediate result is as much as n**k.
        uint64_t result = *small;
        for (uint64_t i = 1; i < k; ++i) {
            result *= *small - i;
            if (combinations)
                result /= i + 1;
        }
        RELEASE_AND_RETURN(scope, makeInt(globalObject, result));
    }

    // P(n, k) = P(n, j) * P(n - j, k - j), and C(n, k) = C(n, j) * C(n - j, k - j) / C(k, j).
    uint64_t j = k / 2;
    JSValue first = mathPermutationsOrCombinations(globalObject, n, j, combinations);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue rest = intSubtract(globalObject, n, makeInt(globalObject, j));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue second = mathPermutationsOrCombinations(globalObject, rest, k - j, combinations);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue product = intMultiply(globalObject, first, second);
    RETURN_IF_EXCEPTION(scope, { });
    if (!combinations)
        return product;
    JSValue divisor = mathPermutationsOrCombinations(globalObject, makeInt(globalObject, k), j, true);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, intFloorDivide(globalObject, product, divisor));
}

static bool mathGetCounts(JSGlobalObject* globalObject, JSValue& n, JSValue& k)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    n = toIndex(globalObject, n);
    RETURN_IF_EXCEPTION(scope, false);
    k = toIndex(globalObject, k);
    RETURN_IF_EXCEPTION(scope, false);
    if (intSign(n) < 0) {
        raiseValueError(globalObject, scope, "n must be a non-negative integer"_s);
        return false;
    }
    if (intSign(k) < 0) {
        raiseValueError(globalObject, scope, "k must be a non-negative integer"_s);
        return false;
    }
    return true;
}

static JSValue mathPerm(JSGlobalObject* globalObject, JSValue n, JSValue k)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!k || isNone(k))
        RELEASE_AND_RETURN(scope, mathFactorial(globalObject, n));
    if (failed(scope, mathGetCounts(globalObject, n, k)))
        return { };
    if (compareInts(n, k) < 0)
        return jsNumber(0);
    auto count = tryInt64(k);
    if (!count)
        return raiseOverflowError(globalObject, scope, "k must not exceed 9223372036854775807"_s);
    RELEASE_AND_RETURN(scope, mathPermutationsOrCombinations(globalObject, n, *count, false));
}

static JSValue mathComb(JSGlobalObject* globalObject, JSValue n, JSValue k)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, mathGetCounts(globalObject, n, k)))
        return { };
    JSValue complement = intSubtract(globalObject, n, k);
    RETURN_IF_EXCEPTION(scope, { });
    if (intSign(complement) < 0)
        return jsNumber(0);
    if (compareInts(complement, k) < 0)
        k = complement;
    auto count = tryInt64(k);
    if (!count)
        return raiseOverflowError(globalObject, scope, "min(n - k, k) must not exceed 9223372036854775807"_s);
    RELEASE_AND_RETURN(scope, mathPermutationsOrCombinations(globalObject, n, *count, true));
}

// ---- The modules ----

static constexpr NativeDef mathIntegerFunctions[] = {
    PY_FUNCTION("comb", mathComb, "(n, k, /)"),
    PY_FUNCTION("factorial", mathFactorial, "(n, /)"),
    PY_FUNCTION("gcd", mathGcd, "(*integers)"),
    PY_FUNCTION("isqrt", mathIsqrt, "(n, /)"),
    PY_FUNCTION("lcm", mathLcm, "(*integers)"),
    PY_FUNCTION("perm", mathPerm, "(n, k=None, /)"),
};

// Imported as _math_integer; `import math` is what makes it math.integer.
void initializeMathIntegerModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    module->dict()->set(vm, rt.names.dName(), makeStr(vm, "math.integer"_s));
    module->dict()->set(vm, rt.names.dDoc(), makeStr(vm, "This module provides access to integer related mathematical functions."_s));
    module->addFunctions(globalObject, mathIntegerFunctions);
}

static constexpr NativeDef mathFunctions[] = {
    PY_FUNCTION("acos", mathAcos, "(x, /)"),
    PY_FUNCTION("acosh", mathAcosh, "(x, /)"),
    PY_FUNCTION("asin", mathAsin, "(x, /)"),
    PY_FUNCTION("asinh", mathAsinh, "(x, /)"),
    PY_FUNCTION("atan", mathAtan, "(x, /)"),
    PY_FUNCTION("atan2", mathAtan2, "(y, x, /)"),
    PY_FUNCTION("atanh", mathAtanh, "(x, /)"),
    PY_FUNCTION("cbrt", mathCbrt, "(x, /)"),
    PY_FUNCTION("ceil", mathCeil, "(x, /)"),
    PY_FUNCTION("copysign", mathCopysign, "(x, y, /)"),
    PY_FUNCTION("cos", mathCos, "(x, /)"),
    PY_FUNCTION("cosh", mathCosh, "(x, /)"),
    PY_FUNCTION("degrees", mathDegrees, "(x, /)"),
    PY_FUNCTION("dist", mathDist, "(p, q, /)"),
    PY_FUNCTION("erf", mathErf, "(x, /)"),
    PY_FUNCTION("erfc", mathErfc, "(x, /)"),
    PY_FUNCTION("exp", mathExp, "(x, /)"),
    PY_FUNCTION("exp2", mathExp2, "(x, /)"),
    PY_FUNCTION("expm1", mathExpm1, "(x, /)"),
    PY_FUNCTION("fabs", mathFabs, "(x, /)"),
    PY_FUNCTION("floor", mathFloor, "(x, /)"),
    PY_FUNCTION("fma", mathFma, "(x, y, z, /)"),
    PY_FUNCTION("fmax", mathFmax, "(x, y, /)"),
    PY_FUNCTION("fmin", mathFmin, "(x, y, /)"),
    PY_FUNCTION("fmod", mathFmod, "(x, y, /)"),
    PY_FUNCTION("frexp", mathFrexp, "(x, /)"),
    PY_FUNCTION("fsum", mathFsum, "(seq, /)"),
    PY_FUNCTION("gamma", mathGamma, "(x, /)"),
    PY_FUNCTION("hypot", mathHypot, "(*coordinates)"),
    PY_FUNCTION("isclose", mathIsClose, "(a, b, *, rel_tol=1e-09, abs_tol=0.0)"),
    PY_FUNCTION("isfinite", mathPredicate<mathIsFiniteValue>, "(x, /)"),
    PY_FUNCTION("isinf", mathPredicate<mathIsInfValue>, "(x, /)"),
    PY_FUNCTION("isnan", mathPredicate<mathIsNaNValue>, "(x, /)"),
    PY_FUNCTION("isnormal", mathPredicate<mathIsNormalValue>, "(x, /)"),
    PY_FUNCTION("issubnormal", mathPredicate<mathIsSubnormalValue>, "(x, /)"),
    PY_FUNCTION("ldexp", mathLdexp, "(x, i, /)"),
    PY_FUNCTION("lgamma", mathLgamma, "(x, /)"),
    PY_FUNCTION("log", mathLog, "(*args)"),
    PY_FUNCTION("log10", mathLog10, "(x, /)"),
    PY_FUNCTION("log1p", mathLog1p, "(x, /)"),
    PY_FUNCTION("log2", mathLog2, "(x, /)"),
    PY_FUNCTION("modf", mathModf, "(x, /)"),
    PY_FUNCTION("nextafter", mathNextAfter, "(x, y, /, *, steps=None)"),
    PY_FUNCTION("pow", mathPow, "(x, y, /)"),
    PY_FUNCTION("prod", mathProd, "(iterable, /, *, start=1)"),
    PY_FUNCTION("radians", mathRadians, "(x, /)"),
    PY_FUNCTION("remainder", mathRemainder, "(x, y, /)"),
    PY_FUNCTION("signbit", mathPredicate<mathSignBitValue>, "(x, /)"),
    PY_FUNCTION("sin", mathSin, "(x, /)"),
    PY_FUNCTION("sinh", mathSinh, "(x, /)"),
    PY_FUNCTION("sqrt", mathSqrt, "(x, /)"),
    PY_FUNCTION("sumprod", mathSumProd, "(p, q, /)"),
    PY_FUNCTION("tan", mathTan, "(x, /)"),
    PY_FUNCTION("tanh", mathTanh, "(x, /)"),
    PY_FUNCTION("trunc", mathTrunc, "(x, /)"),
    PY_FUNCTION("ulp", mathUlp, "(x, /)"),
};

void initializeMathModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    module->dict()->set(vm, rt.names.dDoc(), makeStr(vm, "This module provides access to the mathematical functions\ndefined by the C standard."_s));
    module->addFunctions(globalObject, mathFunctions);
    module->add(globalObject, "pi"_s, makeFloat(globalObject, mathPi));
    module->add(globalObject, "e"_s, makeFloat(globalObject, mathE));
    module->add(globalObject, "tau"_s, makeFloat(globalObject, 2 * mathPi));
    module->add(globalObject, "inf"_s, makeFloat(globalObject, mathInfinity));
    module->add(globalObject, "nan"_s, makeFloat(globalObject, mathNaN));

    JSValue integerModule = importModule(globalObject, "_math_integer"_s);
    RETURN_IF_EXCEPTION(scope, void());
    for (auto& def : mathIntegerFunctions) {
        JSString* name = internString(vm, def.name);
        JSValue function = getAttr(globalObject, integerModule, name);
        RETURN_IF_EXCEPTION(scope, void());
        module->dict()->set(vm, name, function);
    }
    rt.modules()->set(vm, internString(vm, "math.integer"_s), integerModule);
    module->add(globalObject, "integer"_s, integerModule);
}

// ---- _statistics ----

// Wichura's algorithm AS241: the point where the normal distribution reaches the probability p.
static JSValue statisticsNormalDistInvCdf(JSGlobalObject* globalObject, JSValue first, JSValue second, JSValue third)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MATH_GET_DOUBLE(p, first);
    MATH_GET_DOUBLE(mu, second);
    MATH_GET_DOUBLE(sigma, third);

    auto polynomial = [](double r, std::initializer_list<double> coefficients) {
        double result = 0;
        bool isFirst = true;
        for (double coefficient : coefficients) {
            result = isFirst ? coefficient : result * r + coefficient;
            isFirst = false;
        }
        return result;
    };
    auto undefined = [&] {
        return raiseValueError(globalObject, scope, "inv_cdf undefined for these parameters"_s);
    };

    if (p <= 0.0 || p >= 1.0)
        return undefined();
    double q = p - 0.5;
    if (std::abs(q) <= 0.425) {
        double r = 0.180625 - q * q;
        double numerator = polynomial(r, { 2.5090809287301226727e+3, 3.3430575583588128105e+4, 6.7265770927008700853e+4, 4.5921953931549871457e+4,
            1.3731693765509461125e+4, 1.9715909503065514427e+3, 1.3314166789178437745e+2, 3.3871328727963666080e+0 }) * q;
        double denominator = polynomial(r, { 5.2264952788528545610e+3, 2.8729085735721942674e+4, 3.9307895800092710610e+4, 2.1213794301586595867e+4,
            5.3941960214247511077e+3, 6.8718700749205790830e+2, 4.2313330701600911252e+1, 1.0 });
        if (!denominator)
            return undefined();
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, mu + (numerator / denominator * sigma)));
    }

    double r = q <= 0.0 ? p : 1.0 - p;
    if (r <= 0.0 || r >= 1.0)
        return undefined();
    r = std::sqrt(-std::log(r));
    double numerator;
    double denominator;
    if (r <= 5.0) {
        r = r - 1.6;
        numerator = polynomial(r, { 7.74545014278341407640e-4, 2.27238449892691845833e-2, 2.41780725177450611770e-1, 1.27045825245236838258e+0,
            3.64784832476320460504e+0, 5.76949722146069140550e+0, 4.63033784615654529590e+0, 1.42343711074968357734e+0 });
        denominator = polynomial(r, { 1.05075007164441684324e-9, 5.47593808499534494600e-4, 1.51986665636164571966e-2, 1.48103976427480074590e-1,
            6.89767334985100004550e-1, 1.67638483018380384940e+0, 2.05319162663775882187e+0, 1.0 });
    } else {
        r -= 5.0;
        numerator = polynomial(r, { 2.01033439929228813265e-7, 2.71155556874348757815e-5, 1.24266094738807843860e-3, 2.65321895265761230930e-2,
            2.96560571828504891230e-1, 1.78482653991729133580e+0, 5.46378491116411436990e+0, 6.65790464350110377720e+0 });
        denominator = polynomial(r, { 2.04426310338993978564e-15, 1.42151175831644588870e-7, 1.84631831751005468180e-5, 7.86869131145613259100e-4,
            1.48753612908506148525e-2, 1.36929880922735805310e-1, 5.99832206555887937690e-1, 1.0 });
    }
    if (!denominator)
        return undefined();
    double x = numerator / denominator;
    if (q < 0.0)
        x = -x;
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, mu + (x * sigma)));
}

static constexpr NativeDef statisticsFunctions[] = {
    PY_FUNCTION("_normal_dist_inv_cdf", statisticsNormalDistInvCdf, "(p, mu, sigma, /)"),
};

void initializeStatisticsModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->dict()->set(rt.vm(), rt.names.dDoc(), makeStr(rt.vm(), "Accelerators for the statistics module.\n"_s));
    module->addFunctions(globalObject, statisticsFunctions);
}

#undef MATH_GET_DOUBLE

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
