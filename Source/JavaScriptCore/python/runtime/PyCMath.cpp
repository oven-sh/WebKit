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
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTuple.h"
#include <cerrno>
#include <cfloat>
#include <cmath>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module cmath. (Modules/cmathmodule.c in CPython.)
//
// The results are those Annex G of C99 recommends. The formulas, and the order of their operations, are CPython's, so that
// the results are the same to the last bit.

namespace {

struct CMathValue {
    double real;
    double imaginary;
};

// Domain: C99 would raise divide-by-zero or invalid. Range: overflow.
enum class CMathError : uint8_t { None, Domain, Range };

using CMathFunction = CMathValue (*)(CMathValue, CMathError&);

}

static constexpr double cmathPi = 3.14159265358979323846;
static constexpr double cmathE = 2.7182818284590452354;
static constexpr double cmathLn2 = 0.693147180559945309417232121458176568;
static constexpr double cmathLn10 = 2.30258509299404568401799145468436421;
static constexpr double cmathInfinity = std::numeric_limits<double>::infinity();
static constexpr double cmathNaN = std::numeric_limits<double>::quiet_NaN();

// Beyond these, the usual formulas overflow though the result does not.
static constexpr double cmathLarge = DBL_MAX / 4.0;
static constexpr double cmathSqrtLarge = 6.703903964971298e+153;
static constexpr double cmathLogLarge = 708.3964185322641;
static constexpr double cmathSqrtMin = 1.4916681462400413e-154; // sqrt(DBL_MIN)

// ---- Infinities and NaNs ----

// The result for an argument with a part that is not finite, by the kind of the real part and then of the imaginary part:
// -inf, negative, -0, +0, positive, +inf, nan. U is where the table is not consulted.
namespace CMathTable {

using Table = CMathValue[7][7];

static constexpr double P = cmathPi;
static constexpr double P14 = 0.25 * cmathPi;
static constexpr double P12 = 0.5 * cmathPi;
static constexpr double P34 = 0.75 * cmathPi;
static constexpr double INF = cmathInfinity;
static constexpr double N = cmathNaN;
static constexpr double U = -9.5426319407711027e33;

static constexpr Table acos = {
    { { P34, INF }, { P, INF }, { P, INF }, { P, -INF }, { P, -INF }, { P34, -INF }, { N, INF } },
    { { P12, INF }, { U, U }, { U, U }, { U, U }, { U, U }, { P12, -INF }, { N, N } },
    { { P12, INF }, { U, U }, { P12, 0. }, { P12, -0. }, { U, U }, { P12, -INF }, { P12, N } },
    { { P12, INF }, { U, U }, { P12, 0. }, { P12, -0. }, { U, U }, { P12, -INF }, { P12, N } },
    { { P12, INF }, { U, U }, { U, U }, { U, U }, { U, U }, { P12, -INF }, { N, N } },
    { { P14, INF }, { 0., INF }, { 0., INF }, { 0., -INF }, { 0., -INF }, { P14, -INF }, { N, INF } },
    { { N, INF }, { N, N }, { N, N }, { N, N }, { N, N }, { N, -INF }, { N, N } },
};

static constexpr Table acosh = {
    { { INF, -P34 }, { INF, -P }, { INF, -P }, { INF, P }, { INF, P }, { INF, P34 }, { INF, N } },
    { { INF, -P12 }, { U, U }, { U, U }, { U, U }, { U, U }, { INF, P12 }, { N, N } },
    { { INF, -P12 }, { U, U }, { 0., -P12 }, { 0., P12 }, { U, U }, { INF, P12 }, { N, P12 } },
    { { INF, -P12 }, { U, U }, { 0., -P12 }, { 0., P12 }, { U, U }, { INF, P12 }, { N, P12 } },
    { { INF, -P12 }, { U, U }, { U, U }, { U, U }, { U, U }, { INF, P12 }, { N, N } },
    { { INF, -P14 }, { INF, -0. }, { INF, -0. }, { INF, 0. }, { INF, 0. }, { INF, P14 }, { INF, N } },
    { { INF, N }, { N, N }, { N, N }, { N, N }, { N, N }, { INF, N }, { N, N } },
};

static constexpr Table asinh = {
    { { -INF, -P14 }, { -INF, -0. }, { -INF, -0. }, { -INF, 0. }, { -INF, 0. }, { -INF, P14 }, { -INF, N } },
    { { -INF, -P12 }, { U, U }, { U, U }, { U, U }, { U, U }, { -INF, P12 }, { N, N } },
    { { -INF, -P12 }, { U, U }, { -0., -0. }, { -0., 0. }, { U, U }, { -INF, P12 }, { N, N } },
    { { INF, -P12 }, { U, U }, { 0., -0. }, { 0., 0. }, { U, U }, { INF, P12 }, { N, N } },
    { { INF, -P12 }, { U, U }, { U, U }, { U, U }, { U, U }, { INF, P12 }, { N, N } },
    { { INF, -P14 }, { INF, -0. }, { INF, -0. }, { INF, 0. }, { INF, 0. }, { INF, P14 }, { INF, N } },
    { { INF, N }, { N, N }, { N, -0. }, { N, 0. }, { N, N }, { INF, N }, { N, N } },
};

static constexpr Table atanh = {
    { { -0., -P12 }, { -0., -P12 }, { -0., -P12 }, { -0., P12 }, { -0., P12 }, { -0., P12 }, { -0., N } },
    { { -0., -P12 }, { U, U }, { U, U }, { U, U }, { U, U }, { -0., P12 }, { N, N } },
    { { -0., -P12 }, { U, U }, { -0., -0. }, { -0., 0. }, { U, U }, { -0., P12 }, { -0., N } },
    { { 0., -P12 }, { U, U }, { 0., -0. }, { 0., 0. }, { U, U }, { 0., P12 }, { 0., N } },
    { { 0., -P12 }, { U, U }, { U, U }, { U, U }, { U, U }, { 0., P12 }, { N, N } },
    { { 0., -P12 }, { 0., -P12 }, { 0., -P12 }, { 0., P12 }, { 0., P12 }, { 0., P12 }, { 0., N } },
    { { 0., -P12 }, { N, N }, { N, N }, { N, N }, { N, N }, { 0., P12 }, { N, N } },
};

static constexpr Table cosh = {
    { { INF, N }, { U, U }, { INF, 0. }, { INF, -0. }, { U, U }, { INF, N }, { INF, N } },
    { { N, N }, { U, U }, { U, U }, { U, U }, { U, U }, { N, N }, { N, N } },
    { { N, 0. }, { U, U }, { 1., 0. }, { 1., -0. }, { U, U }, { N, 0. }, { N, 0. } },
    { { N, 0. }, { U, U }, { 1., -0. }, { 1., 0. }, { U, U }, { N, 0. }, { N, 0. } },
    { { N, N }, { U, U }, { U, U }, { U, U }, { U, U }, { N, N }, { N, N } },
    { { INF, N }, { U, U }, { INF, -0. }, { INF, 0. }, { U, U }, { INF, N }, { INF, N } },
    { { N, N }, { N, N }, { N, 0. }, { N, 0. }, { N, N }, { N, N }, { N, N } },
};

static constexpr Table exp = {
    { { 0., 0. }, { U, U }, { 0., -0. }, { 0., 0. }, { U, U }, { 0., 0. }, { 0., 0. } },
    { { N, N }, { U, U }, { U, U }, { U, U }, { U, U }, { N, N }, { N, N } },
    { { N, N }, { U, U }, { 1., -0. }, { 1., 0. }, { U, U }, { N, N }, { N, N } },
    { { N, N }, { U, U }, { 1., -0. }, { 1., 0. }, { U, U }, { N, N }, { N, N } },
    { { N, N }, { U, U }, { U, U }, { U, U }, { U, U }, { N, N }, { N, N } },
    { { INF, N }, { U, U }, { INF, -0. }, { INF, 0. }, { U, U }, { INF, N }, { INF, N } },
    { { N, N }, { N, N }, { N, -0. }, { N, 0. }, { N, N }, { N, N }, { N, N } },
};

static constexpr Table log = {
    { { INF, -P34 }, { INF, -P }, { INF, -P }, { INF, P }, { INF, P }, { INF, P34 }, { INF, N } },
    { { INF, -P12 }, { U, U }, { U, U }, { U, U }, { U, U }, { INF, P12 }, { N, N } },
    { { INF, -P12 }, { U, U }, { -INF, -P }, { -INF, P }, { U, U }, { INF, P12 }, { N, N } },
    { { INF, -P12 }, { U, U }, { -INF, -0. }, { -INF, 0. }, { U, U }, { INF, P12 }, { N, N } },
    { { INF, -P12 }, { U, U }, { U, U }, { U, U }, { U, U }, { INF, P12 }, { N, N } },
    { { INF, -P14 }, { INF, -0. }, { INF, -0. }, { INF, 0. }, { INF, 0. }, { INF, P14 }, { INF, N } },
    { { INF, N }, { N, N }, { N, N }, { N, N }, { N, N }, { INF, N }, { N, N } },
};

static constexpr Table sinh = {
    { { INF, N }, { U, U }, { -INF, -0. }, { -INF, 0. }, { U, U }, { INF, N }, { INF, N } },
    { { N, N }, { U, U }, { U, U }, { U, U }, { U, U }, { N, N }, { N, N } },
    { { 0., N }, { U, U }, { -0., -0. }, { -0., 0. }, { U, U }, { 0., N }, { 0., N } },
    { { 0., N }, { U, U }, { 0., -0. }, { 0., 0. }, { U, U }, { 0., N }, { 0., N } },
    { { N, N }, { U, U }, { U, U }, { U, U }, { U, U }, { N, N }, { N, N } },
    { { INF, N }, { U, U }, { INF, -0. }, { INF, 0. }, { U, U }, { INF, N }, { INF, N } },
    { { N, N }, { N, N }, { N, -0. }, { N, 0. }, { N, N }, { N, N }, { N, N } },
};

static constexpr Table sqrt = {
    { { INF, -INF }, { 0., -INF }, { 0., -INF }, { 0., INF }, { 0., INF }, { INF, INF }, { N, INF } },
    { { INF, -INF }, { U, U }, { U, U }, { U, U }, { U, U }, { INF, INF }, { N, N } },
    { { INF, -INF }, { U, U }, { 0., -0. }, { 0., 0. }, { U, U }, { INF, INF }, { N, N } },
    { { INF, -INF }, { U, U }, { 0., -0. }, { 0., 0. }, { U, U }, { INF, INF }, { N, N } },
    { { INF, -INF }, { U, U }, { U, U }, { U, U }, { U, U }, { INF, INF }, { N, N } },
    { { INF, -INF }, { INF, -0. }, { INF, -0. }, { INF, 0. }, { INF, 0. }, { INF, INF }, { INF, N } },
    { { INF, -INF }, { N, N }, { N, N }, { N, N }, { N, N }, { INF, INF }, { N, N } },
};

static constexpr Table tanh = {
    { { -1., 0. }, { U, U }, { -1., -0. }, { -1., 0. }, { U, U }, { -1., 0. }, { -1., 0. } },
    { { N, N }, { U, U }, { U, U }, { U, U }, { U, U }, { N, N }, { N, N } },
    { { -0., N }, { U, U }, { -0., -0. }, { -0., 0. }, { U, U }, { -0., N }, { -0., N } },
    { { 0., N }, { U, U }, { 0., -0. }, { 0., 0. }, { U, U }, { 0., N }, { 0., N } },
    { { N, N }, { U, U }, { U, U }, { U, U }, { U, U }, { N, N }, { N, N } },
    { { 1., 0. }, { U, U }, { 1., -0. }, { 1., 0. }, { U, U }, { 1., 0. }, { 1., 0. } },
    { { N, N }, { N, N }, { N, -0. }, { N, 0. }, { N, N }, { N, N }, { N, N } },
};

// By the kind of r and then of phi.
static constexpr Table rect = {
    { { INF, N }, { U, U }, { -INF, 0. }, { -INF, -0. }, { U, U }, { INF, N }, { INF, N } },
    { { N, N }, { U, U }, { U, U }, { U, U }, { U, U }, { N, N }, { N, N } },
    { { 0., 0. }, { U, U }, { -0., 0. }, { -0., -0. }, { U, U }, { 0., 0. }, { 0., 0. } },
    { { 0., 0. }, { U, U }, { 0., -0. }, { 0., 0. }, { U, U }, { 0., 0. }, { 0., 0. } },
    { { N, N }, { U, U }, { U, U }, { U, U }, { U, U }, { N, N }, { N, N } },
    { { INF, N }, { U, U }, { INF, -0. }, { INF, 0. }, { U, U }, { INF, N }, { INF, N } },
    { { N, N }, { N, N }, { N, 0. }, { N, 0. }, { N, N }, { N, N }, { N, N } },
};

} // namespace CMathTable

static unsigned cmathKindOf(double value)
{
    if (std::isnan(value))
        return 6;
    bool isNegative = std::signbit(value);
    if (std::isinf(value))
        return isNegative ? 0 : 5;
    if (value)
        return isNegative ? 1 : 4;
    return isNegative ? 2 : 3;
}

static ALWAYS_INLINE bool cmathIsFinite(CMathValue z) { return std::isfinite(z.real) && std::isfinite(z.imaginary); }
static ALWAYS_INLINE CMathValue cmathLookUp(const CMathTable::Table& table, CMathValue z) { return table[cmathKindOf(z.real)][cmathKindOf(z.imaginary)]; }

// An infinite real part with an imaginary part that is finite and not zero: the signs come from the cosine and the sine.
static ALWAYS_INLINE bool cmathIsInfiniteWithAngle(CMathValue z) { return std::isinf(z.real) && std::isfinite(z.imaginary) && z.imaginary; }

static ALWAYS_INLINE double cmathLog1p(double x) { return !x ? x : std::log1p(x); }

// i * z, and -i * z.
static ALWAYS_INLINE CMathValue cmathTimesI(CMathValue z) { return { -z.imaginary, z.real }; }
static ALWAYS_INLINE CMathValue cmathTimesMinusI(CMathValue z) { return { z.imaginary, -z.real }; }

// ---- The functions ----

static CMathValue cmathSqrtOf(CMathValue z, CMathError&)
{
    if (!cmathIsFinite(z))
        return cmathLookUp(CMathTable::sqrt, z);
    if (!z.real && !z.imaginary)
        return { 0.0, z.imaginary };

    // For x, y >= 0 the real part is s = sqrt((x + hypot(x, y)) / 2) and the imaginary part (y / 2) / s. The arguments are
    // scaled so that the sum does not overflow, or, if both are subnormal, so that hypot() has all its bits.
    double ax = std::abs(z.real);
    double ay = std::abs(z.imaginary);
    double s;
    if (ax < DBL_MIN && ay < DBL_MIN) {
        ax = std::ldexp(ax, 53);
        s = std::ldexp(std::sqrt(ax + std::hypot(ax, std::ldexp(ay, 53))), -27);
    } else {
        ax /= 8.0;
        s = 2.0 * std::sqrt(ax + std::hypot(ax, ay / 8.0));
    }
    double d = ay / (2.0 * s);
    if (z.real >= 0)
        return { s, std::copysign(d, z.imaginary) };
    return { d, std::copysign(s, z.imaginary) };
}

static ALWAYS_INLINE CMathValue cmathSqrtOf(double real, double imaginary)
{
    CMathError ignored = CMathError::None;
    return cmathSqrtOf({ real, imaginary }, ignored);
}

static ALWAYS_INLINE bool cmathIsLarge(CMathValue z) { return std::abs(z.real) > cmathLarge || std::abs(z.imaginary) > cmathLarge; }

// log(abs(z)) + log(2), for a z whose absolute value may overflow.
static ALWAYS_INLINE double cmathLogOfTwiceLarge(CMathValue z) { return std::log(std::hypot(z.real / 2.0, z.imaginary / 2.0)) + cmathLn2 * 2.0; }

static CMathValue cmathAcosOf(CMathValue z, CMathError&)
{
    if (!cmathIsFinite(z))
        return cmathLookUp(CMathTable::acos, z);
    if (cmathIsLarge(z))
        return { std::atan2(std::abs(z.imaginary), z.real), -std::copysign(cmathLogOfTwiceLarge(z), z.imaginary) };
    CMathValue s1 = cmathSqrtOf(1.0 - z.real, -z.imaginary);
    CMathValue s2 = cmathSqrtOf(1.0 + z.real, z.imaginary);
    return { 2.0 * std::atan2(s1.real, s2.real), std::asinh(s2.real * s1.imaginary - s2.imaginary * s1.real) };
}

static CMathValue cmathAcoshOf(CMathValue z, CMathError&)
{
    if (!cmathIsFinite(z))
        return cmathLookUp(CMathTable::acosh, z);
    if (cmathIsLarge(z))
        return { cmathLogOfTwiceLarge(z), std::atan2(z.imaginary, z.real) };
    CMathValue s1 = cmathSqrtOf(z.real - 1.0, z.imaginary);
    CMathValue s2 = cmathSqrtOf(z.real + 1.0, z.imaginary);
    return { std::asinh(s1.real * s2.real + s1.imaginary * s2.imaginary), 2.0 * std::atan2(s1.imaginary, s2.real) };
}

static CMathValue cmathAsinhOf(CMathValue z, CMathError&)
{
    if (!cmathIsFinite(z))
        return cmathLookUp(CMathTable::asinh, z);
    if (cmathIsLarge(z)) {
        double real = z.imaginary >= 0 ? std::copysign(cmathLogOfTwiceLarge(z), z.real) : -std::copysign(cmathLogOfTwiceLarge(z), -z.real);
        return { real, std::atan2(z.imaginary, std::abs(z.real)) };
    }
    CMathValue s1 = cmathSqrtOf(1.0 + z.imaginary, -z.real);
    CMathValue s2 = cmathSqrtOf(1.0 - z.imaginary, z.real);
    return { std::asinh(s1.real * s2.imaginary - s2.real * s1.imaginary), std::atan2(z.imaginary, s1.real * s2.real - s1.imaginary * s2.imaginary) };
}

static CMathValue cmathAsinOf(CMathValue z, CMathError& error)
{
    return cmathTimesMinusI(cmathAsinhOf(cmathTimesI(z), error));
}

static CMathValue cmathAtanhOf(CMathValue z, CMathError& error)
{
    if (!cmathIsFinite(z))
        return cmathLookUp(CMathTable::atanh, z);
    if (z.real < 0) {
        CMathValue result = cmathAtanhOf({ -z.real, -z.imaginary }, error);
        return { -result.real, -result.imaginary };
    }

    double ay = std::abs(z.imaginary);
    if (z.real > cmathSqrtLarge || ay > cmathSqrtLarge) {
        // About 1 / z +- i * pi / 2.
        double h = std::hypot(z.real / 2.0, z.imaginary / 2.0);
        return { z.real / 4.0 / h / h, std::copysign(cmathPi / 2.0, z.imaginary) };
    }
    if (z.real == 1.0 && ay < cmathSqrtMin) {
        if (!ay) {
            error = CMathError::Domain;
            return { cmathInfinity, z.imaginary };
        }
        return { -std::log(std::sqrt(ay) / std::sqrt(std::hypot(ay, 2.0))), std::copysign(std::atan2(2.0, -ay) / 2, z.imaginary) };
    }
    return {
        cmathLog1p(4.0 * z.real / ((1 - z.real) * (1 - z.real) + ay * ay)) / 4.0,
        -std::atan2(-2.0 * z.imaginary, (1 - z.real) * (1 + z.real) - ay * ay) / 2.0
    };
}

static CMathValue cmathAtanOf(CMathValue z, CMathError& error)
{
    return cmathTimesMinusI(cmathAtanhOf(cmathTimesI(z), error));
}

static ALWAYS_INLINE CMathValue cmathCheckOverflow(CMathValue result, CMathError& error)
{
    if (std::isinf(result.real) || std::isinf(result.imaginary))
        error = CMathError::Range;
    return result;
}

static CMathValue cmathCoshOf(CMathValue z, CMathError& error)
{
    if (!cmathIsFinite(z)) {
        if (std::isinf(z.imaginary) && !std::isnan(z.real))
            error = CMathError::Domain;
        if (cmathIsInfiniteWithAngle(z)) {
            double imaginary = std::copysign(cmathInfinity, std::sin(z.imaginary));
            return { std::copysign(cmathInfinity, std::cos(z.imaginary)), z.real > 0 ? imaginary : -imaginary };
        }
        return cmathLookUp(CMathTable::cosh, z);
    }
    if (std::abs(z.real) > cmathLogLarge) {
        // cosh(z.real) overflows, but the result need not.
        double lessOne = z.real - std::copysign(1.0, z.real);
        return cmathCheckOverflow({ std::cos(z.imaginary) * std::cosh(lessOne) * cmathE, std::sin(z.imaginary) * std::sinh(lessOne) * cmathE }, error);
    }
    return cmathCheckOverflow({ std::cos(z.imaginary) * std::cosh(z.real), std::sin(z.imaginary) * std::sinh(z.real) }, error);
}

static CMathValue cmathCosOf(CMathValue z, CMathError& error)
{
    return cmathCoshOf(cmathTimesI(z), error);
}

static CMathValue cmathSinhOf(CMathValue z, CMathError& error)
{
    if (!cmathIsFinite(z)) {
        if (std::isinf(z.imaginary) && !std::isnan(z.real))
            error = CMathError::Domain;
        if (cmathIsInfiniteWithAngle(z)) {
            double real = std::copysign(cmathInfinity, std::cos(z.imaginary));
            return { z.real > 0 ? real : -real, std::copysign(cmathInfinity, std::sin(z.imaginary)) };
        }
        return cmathLookUp(CMathTable::sinh, z);
    }
    if (std::abs(z.real) > cmathLogLarge) {
        double lessOne = z.real - std::copysign(1.0, z.real);
        return cmathCheckOverflow({ std::cos(z.imaginary) * std::sinh(lessOne) * cmathE, std::sin(z.imaginary) * std::cosh(lessOne) * cmathE }, error);
    }
    return cmathCheckOverflow({ std::cos(z.imaginary) * std::sinh(z.real), std::sin(z.imaginary) * std::cosh(z.real) }, error);
}

static CMathValue cmathSinOf(CMathValue z, CMathError& error)
{
    return cmathTimesMinusI(cmathSinhOf(cmathTimesI(z), error));
}

static CMathValue cmathTanhOf(CMathValue z, CMathError& error)
{
    if (!cmathIsFinite(z)) {
        if (std::isinf(z.imaginary) && std::isfinite(z.real))
            error = CMathError::Domain;
        if (cmathIsInfiniteWithAngle(z))
            return { z.real > 0 ? 1.0 : -1.0, std::copysign(0.0, 2.0 * std::sin(z.imaginary) * std::cos(z.imaginary)) };
        return cmathLookUp(CMathTable::tanh, z);
    }
    if (std::abs(z.real) > cmathLogLarge)
        return { std::copysign(1.0, z.real), 4.0 * std::sin(z.imaginary) * std::cos(z.imaginary) * std::exp(-2.0 * std::abs(z.real)) };
    // (tanh(x) * (1 + tan(y)**2) + i * tan(y) * (1 - tanh(x)**2)) / (1 + tan(y)**2 * tanh(x)**2), with 1 / cosh(x)**2 for
    // 1 - tanh(x)**2, which loses fewer bits.
    double tx = std::tanh(z.real);
    double ty = std::tan(z.imaginary);
    double cx = 1.0 / std::cosh(z.real);
    double txty = tx * ty;
    double denominator = 1.0 + txty * txty;
    return { tx * (1.0 + ty * ty) / denominator, ((ty / denominator) * cx) * cx };
}

static CMathValue cmathTanOf(CMathValue z, CMathError& error)
{
    return cmathTimesMinusI(cmathTanhOf(cmathTimesI(z), error));
}

static CMathValue cmathExpOf(CMathValue z, CMathError& error)
{
    if (!cmathIsFinite(z)) {
        if (std::isinf(z.imaginary) && (std::isfinite(z.real) || (std::isinf(z.real) && z.real > 0)))
            error = CMathError::Domain;
        if (cmathIsInfiniteWithAngle(z)) {
            double magnitude = z.real > 0 ? cmathInfinity : 0.0;
            return { std::copysign(magnitude, std::cos(z.imaginary)), std::copysign(magnitude, std::sin(z.imaginary)) };
        }
        return cmathLookUp(CMathTable::exp, z);
    }
    if (z.real > cmathLogLarge) {
        double l = std::exp(z.real - 1.0);
        return cmathCheckOverflow({ l * std::cos(z.imaginary) * cmathE, l * std::sin(z.imaginary) * cmathE }, error);
    }
    double l = std::exp(z.real);
    return cmathCheckOverflow({ l * std::cos(z.imaginary), l * std::sin(z.imaginary) }, error);
}

// Sets the error, to None too.
static CMathValue cmathLogOf(CMathValue z, CMathError& error)
{
    error = CMathError::None;
    if (!cmathIsFinite(z))
        return cmathLookUp(CMathTable::log, z);

    // The real part is log(hypot(x, y)), but for where hypot() overflows or is subnormal, and near 1, where the logarithm
    // would lose what hypot() rounded off.
    double ax = std::abs(z.real);
    double ay = std::abs(z.imaginary);
    double real;
    if (ax > cmathLarge || ay > cmathLarge)
        real = std::log(std::hypot(ax / 2.0, ay / 2.0)) + cmathLn2;
    else if (ax < DBL_MIN && ay < DBL_MIN) {
        if (!(ax > 0 || ay > 0)) {
            error = CMathError::Domain;
            return { -cmathInfinity, std::atan2(z.imaginary, z.real) };
        }
        real = std::log(std::hypot(std::ldexp(ax, DBL_MANT_DIG), std::ldexp(ay, DBL_MANT_DIG))) - DBL_MANT_DIG * cmathLn2;
    } else {
        double h = std::hypot(ax, ay);
        if (0.71 <= h && h <= 1.73) {
            double larger = ax > ay ? ax : ay;
            double smaller = ax > ay ? ay : ax;
            real = cmathLog1p((larger - 1) * (larger + 1) + smaller * smaller) / 2.0;
        } else
            real = std::log(h);
    }
    return { real, std::atan2(z.imaginary, z.real) };
}

static CMathValue cmathLog10Of(CMathValue z, CMathError& error)
{
    CMathValue result = cmathLogOf(z, error);
    return { result.real / cmathLn10, result.imaginary / cmathLn10 };
}

static double cmathAbsOf(CMathValue z, CMathError& error)
{
    if (std::isinf(z.real) || std::isinf(z.imaginary))
        return cmathInfinity;
    if (std::isnan(z.real) || std::isnan(z.imaginary))
        return cmathNaN;
    double result = std::hypot(z.real, z.imaginary);
    if (std::isinf(result))
        error = CMathError::Range;
    return result;
}

// ---- From and to Python ----

static JSValue cmathRaise(JSGlobalObject* globalObject, ThrowScope& scope, CMathError error)
{
    if (error == CMathError::Domain)
        return raiseValueError(globalObject, scope, "math domain error"_s);
    return raiseOverflowError(globalObject, scope, "math range error"_s);
}

// A complex, what __complex__() returns, or a real number. False: an exception.
static bool cmathGetArgument(JSGlobalObject* globalObject, JSValue value, CMathValue& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyComplex* complex = PyComplex::tryFrom(value);
    if (!complex && !value.isNumber()) {
        Runtime& rt = runtime(globalObject);
        PyType* type = rt.typeOf(value);
        if (JSValue method = type->lookup(Special::Complex)) {
            JSValue converted = callSpecial(globalObject, method, value);
            RETURN_IF_EXCEPTION(scope, false);
            complex = PyComplex::tryFrom(converted);
            if (!complex) {
                raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__complex__() must return a complex, not "_s, typeNameForMessage(globalObject, rt.typeOf(converted)));
                return false;
            }
            if (complex->type() != rt.complexType()) {
                warn(globalObject, rt.DeprecationWarning(), makeString(typeNameForMessage(globalObject, type), ".__complex__() must return a complex, not "_s, typeNameForMessage(globalObject, complex->type()), ".  The ability to return an instance of a strict subclass of complex is deprecated, and may be removed in a future version of Python."_s));
                RETURN_IF_EXCEPTION(scope, false);
            }
        }
    }
    if (complex) {
        result = { complex->real(), complex->imaginary() };
        return true;
    }
    result = { toDouble(globalObject, value), 0.0 };
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

template<CMathFunction function>
static JSValue cmathCall(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CMathValue z;
    if (failed(scope, cmathGetArgument(globalObject, argument, z)))
        return { };
    CMathError error = CMathError::None;
    CMathValue result = function(z, error);
    if (error != CMathError::None)
        return cmathRaise(globalObject, scope, error);
    RELEASE_AND_RETURN(scope, PyComplex::create(globalObject, result.real, result.imaginary));
}

static JSValue cmathLog(JSGlobalObject* globalObject, JSValue argument, JSValue base)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CMathValue x;
    if (failed(scope, cmathGetArgument(globalObject, argument, x)))
        return { };
    CMathError error;
    x = cmathLogOf(x, error);
    if (!base) {
        if (error != CMathError::None)
            return cmathRaise(globalObject, scope, error);
        RELEASE_AND_RETURN(scope, PyComplex::create(globalObject, x.real, x.imaginary));
    }

    // As in CPython, what was wrong with the first logarithm is forgotten over the second.
    CMathValue y;
    if (failed(scope, cmathGetArgument(globalObject, base, y)))
        return { };
    y = cmathLogOf(y, error);
    if (!y.real && !y.imaginary)
        error = CMathError::Domain;
    if (error != CMathError::None)
        return cmathRaise(globalObject, scope, error);
    RELEASE_AND_RETURN(scope, binaryOperation(globalObject, BinaryOperator::Div, PyComplex::create(globalObject, x.real, x.imaginary), PyComplex::create(globalObject, y.real, y.imaginary)));
}

static JSValue cmathPhase(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CMathValue z;
    if (failed(scope, cmathGetArgument(globalObject, argument, z)))
        return { };
    // CPython goes by errno here, which glibc sets when the result underflows.
    errno = 0;
    double phi = std::atan2(z.imaginary, z.real);
    if (errno)
        return cmathRaise(globalObject, scope, errno == EDOM ? CMathError::Domain : CMathError::Range);
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, phi));
}

static JSValue cmathPolar(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CMathValue z;
    if (failed(scope, cmathGetArgument(globalObject, argument, z)))
        return { };
    CMathError error = CMathError::None;
    double r = cmathAbsOf(z, error);
    if (error != CMathError::None)
        return cmathRaise(globalObject, scope, error);
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { makeFloat(globalObject, r), makeFloat(globalObject, std::atan2(z.imaginary, z.real)) }));
}

static JSValue cmathRect(JSGlobalObject* globalObject, JSValue first, JSValue second)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double r = toDouble(globalObject, first);
    RETURN_IF_EXCEPTION(scope, { });
    double phi = toDouble(globalObject, second);
    RETURN_IF_EXCEPTION(scope, { });

    CMathValue z;
    if (!std::isfinite(r) || !std::isfinite(phi)) {
        if (r && !std::isnan(r) && std::isinf(phi))
            return cmathRaise(globalObject, scope, CMathError::Domain);
        if (cmathIsInfiniteWithAngle({ r, phi })) {
            z = { std::copysign(cmathInfinity, std::cos(phi)), std::copysign(cmathInfinity, std::sin(phi)) };
            if (r < 0)
                z = { -z.real, -z.imaginary };
        } else
            z = cmathLookUp(CMathTable::rect, { r, phi });
    } else if (!phi)
        z = { r, r * phi };
    else
        z = { r * std::cos(phi), r * std::sin(phi) };
    RELEASE_AND_RETURN(scope, PyComplex::create(globalObject, z.real, z.imaginary));
}

template<bool (*predicate)(CMathValue)>
static JSValue cmathPredicate(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CMathValue z;
    if (failed(scope, cmathGetArgument(globalObject, argument, z)))
        return { };
    return jsBoolean(predicate(z));
}

static bool cmathIsFiniteValue(CMathValue z) { return cmathIsFinite(z); }
static bool cmathIsInfValue(CMathValue z) { return std::isinf(z.real) || std::isinf(z.imaginary); }
static bool cmathIsNaNValue(CMathValue z) { return std::isnan(z.real) || std::isnan(z.imaginary); }

static JSValue cmathIsClose(JSGlobalObject* globalObject, JSValue first, JSValue second, JSValue relativeTolerance, JSValue absoluteTolerance)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CMathValue a;
    if (failed(scope, cmathGetArgument(globalObject, first, a)))
        return { };
    CMathValue b;
    if (failed(scope, cmathGetArgument(globalObject, second, b)))
        return { };
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
    if (a.real == b.real && a.imaginary == b.imaginary)
        return jsBoolean(true);
    if (cmathIsInfValue(a) || cmathIsInfValue(b))
        return jsBoolean(false);
    CMathError ignored = CMathError::None;
    double difference = cmathAbsOf({ a.real - b.real, a.imaginary - b.imaginary }, ignored);
    return jsBoolean(difference <= relative * cmathAbsOf(b, ignored) || difference <= relative * cmathAbsOf(a, ignored) || difference <= absolute);
}

static constexpr NativeDef cmathFunctions[] = {
    PY_FUNCTION("acos", cmathCall<cmathAcosOf>, "(z, /)"),
    PY_FUNCTION("acosh", cmathCall<cmathAcoshOf>, "(z, /)"),
    PY_FUNCTION("asin", cmathCall<cmathAsinOf>, "(z, /)"),
    PY_FUNCTION("asinh", cmathCall<cmathAsinhOf>, "(z, /)"),
    PY_FUNCTION("atan", cmathCall<cmathAtanOf>, "(z, /)"),
    PY_FUNCTION("atanh", cmathCall<cmathAtanhOf>, "(z, /)"),
    PY_FUNCTION("cos", cmathCall<cmathCosOf>, "(z, /)"),
    PY_FUNCTION("cosh", cmathCall<cmathCoshOf>, "(z, /)"),
    PY_FUNCTION("exp", cmathCall<cmathExpOf>, "(z, /)"),
    PY_FUNCTION("isclose", cmathIsClose, "(a, b, *, rel_tol=1e-09, abs_tol=0.0)"),
    PY_FUNCTION("isfinite", cmathPredicate<cmathIsFiniteValue>, "(z, /)"),
    PY_FUNCTION("isinf", cmathPredicate<cmathIsInfValue>, "(z, /)"),
    PY_FUNCTION("isnan", cmathPredicate<cmathIsNaNValue>, "(z, /)"),
    PY_FUNCTION("log", cmathLog, "(z, base=<unrepresentable>, /)"),
    PY_FUNCTION("log10", cmathCall<cmathLog10Of>, "(z, /)"),
    PY_FUNCTION("phase", cmathPhase, "(z, /)"),
    PY_FUNCTION("polar", cmathPolar, "(z, /)"),
    PY_FUNCTION("rect", cmathRect, "(r, phi, /)"),
    PY_FUNCTION("sin", cmathCall<cmathSinOf>, "(z, /)"),
    PY_FUNCTION("sinh", cmathCall<cmathSinhOf>, "(z, /)"),
    PY_FUNCTION("sqrt", cmathCall<static_cast<CMathFunction>(cmathSqrtOf)>, "(z, /)"),
    PY_FUNCTION("tan", cmathCall<cmathTanOf>, "(z, /)"),
    PY_FUNCTION("tanh", cmathCall<cmathTanhOf>, "(z, /)"),
};

void initializeCMathModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    module->dict()->set(vm, rt.names.dDoc(), makeStr(vm, "This module provides access to mathematical functions for complex\nnumbers."_s));
    module->addFunctions(globalObject, cmathFunctions);
    module->add(globalObject, "pi"_s, makeFloat(globalObject, cmathPi));
    module->add(globalObject, "e"_s, makeFloat(globalObject, cmathE));
    module->add(globalObject, "tau"_s, makeFloat(globalObject, 2 * cmathPi));
    module->add(globalObject, "inf"_s, makeFloat(globalObject, cmathInfinity));
    module->add(globalObject, "infj"_s, PyComplex::create(globalObject, 0.0, cmathInfinity));
    module->add(globalObject, "nan"_s, makeFloat(globalObject, cmathNaN));
    module->add(globalObject, "nanj"_s, PyComplex::create(globalObject, 0.0, cmathNaN));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
