/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef PY_DECIMAL_STANDALONE
#include "config.h"
#endif
#include "PyDecimalLimbs.h"

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef PY_DECIMAL_STANDALONE
#define WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#define WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Decimal {

// Square root, exponential, logarithms and powers.
//
// The square root, e**x, ln and log10 are correctly rounded, so what they return does not depend on how it is found. x**y is not: it is
// defined by the steps it is made of, each of which is rounded in a given way to a given precision. Those steps are libmpdec's,
// so that the results are too. So are the working precisions and the numbers of terms of the series, whose error bounds its authors
// have proved.

static int exponentDigits(int64_t exponent)
{
    return digitCount(exponent < 0 ? -static_cast<uint64_t>(exponent) : static_cast<uint64_t>(exponent));
}

// With zeros at the end if it has fewer digits than the precision.
static void padWithZeros(Number& number, const Context& context, uint32_t& status)
{
    if (number.isSpecial() || number.hasZeroCoefficient() || number.digits >= context.precision)
        return;
    int64_t count = context.precision - number.digits;
    if (shiftCoefficientLeft(number, number, count, status))
        number.exponent -= count;
}

// What is computed here has only the digits that rounding needs, so that one that is about to become subnormal may happen to lose nothing but
// zeros. It is inexact all the same.
static void noteUnderflow(const Number& number, const Context& context, uint32_t& status)
{
    if (number.adjustedExponent() < context.emin && !number.isZero() && number.exponent < context.etiny())
        status |= Underflow;
}

// ---- Square root ----

// root = floor(sqrt(c)), for an integer c > 0 (its exponent is 0). False: an error, which `status` has.
static bool integerSquareRoot(Number& root, const Number& c, uint32_t& status)
{
    Context unlimited = Context::maximum();
    // Newton's iteration from above, which doubles the number of digits that are right.
    if (c.digits > 100) {
        // It starts from the root of the first half of the digits, plus one.
        int64_t dropped = c.digits / 4 * 2;
        Number head;
        uint32_t ownStatus = 0;
        if (shiftCoefficientRight(head, c, dropped, status) == shiftFailed || !integerSquareRoot(root, head, status))
            return false;
        addUnrounded(root, root, Number(0, 1, 0), unlimited, ownStatus);
        if (root.isSpecial() || !shiftCoefficientLeft(root, root, dropped / 2, status)) {
            status |= ownStatus & errorConditions;
            return false;
        }
    } else {
        // It starts from the root of the first 17 or 18 digits in floating point, rounded up.
        int64_t leading = std::min<int64_t>(c.digits, 17 + ((c.digits ^ 17) & 1));
        int64_t half = (c.digits - leading) / 2;
        Number head;
        if (shiftCoefficientRight(head, c, c.digits - leading, status) == shiftFailed)
            return false;
        int64_t extra = std::min<int64_t>(half, 7);
        double estimate = std::sqrt(static_cast<double>(head.limbs()[0] + 1)) * static_cast<double>(powersOfTen[extra]);
        Limb start = static_cast<Limb>(estimate * (1 + 1e-14)) + 2;
        if (!shiftCoefficientLeft(root, Number(0, start, 0), half - extra, status))
            return false;
    }
    Number quotient;
    Number rest;
    while (true) {
        uint32_t ownStatus = 0;
        divideWithRemainderCore(quotient, rest, c, root, unlimited, ownStatus);
        if (quotient.isSpecial()) {
            status |= ownStatus & errorConditions;
            return false;
        }
        if (compareValues(root, quotient) <= 0)
            return true;
        addUnrounded(root, root, quotient, unlimited, ownStatus);
        if (root.isSpecial()) {
            status |= ownStatus & errorConditions;
            return false;
        }
        divideLimbsByLimb(root.limbs(), root.limbs(), root.length, 2);
        root.trim(root.length);
    }
}

static void squareRootCore(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (a.isSpecial()) {
        if (propagateNaN(result, a, context, status))
            return;
        if (a.isNegative())
            result.setError(InvalidOperation, status);
        else
            result.setSpecial(0, Number::Infinite);
        return;
    }
    int64_t idealExponent = (a.exponent - (a.exponent & 1)) / 2;
    if (a.hasZeroCoefficient()) {
        result.set(a.sign(), 0, idealExponent);
        finalize(result, context, status);
        return;
    }
    if (a.isNegative()) {
        result.setError(InvalidOperation, status);
        return;
    }

    // The coefficient, with an even exponent, is given as many digits as it takes for its integer root to have one digit more than the
    // precision.
    int64_t precision = context.precision + 1;
    Number c;
    Number root;
    bool isFine = c.copy(a, status);
    int64_t rootDigits;
    if (a.exponent & 1) {
        isFine = isFine && shiftCoefficientLeft(c, c, 1, status);
        rootDigits = (a.digits >> 1) + 1;
    } else
        rootDigits = (a.digits + 1) >> 1;
    int64_t count = precision - rootDigits;
    bool isExact = true;
    if (isFine) {
        c.exponent = 0;
        if (count >= 0)
            isFine = shiftCoefficientLeft(c, c, 2 * count, status);
        else
            isExact = !shiftCoefficientRight(c, -2 * count);
    }
    idealExponent -= count;
    isFine = isFine && integerSquareRoot(root, c, status);
    if (isFine && isExact) {
        Context unlimited = Context::maximum();
        uint32_t ownStatus = 0;
        Number square;
        multiplyExactly(square, root, root, unlimited, ownStatus);
        if (square.isSpecial()) {
            status |= ownStatus & errorConditions;
            isFine = false;
        } else
            isExact = !compareValues(square, c);
    }
    if (isFine) {
        if (isExact) {
            if (count >= 0)
                shiftCoefficientRight(root, count);
            else
                isFine = shiftCoefficientLeft(root, root, -count, status);
            idealExponent += count;
        } else {
            Limb last = root.limbs()[0] % 10;
            if (!last || last == 5)
                ++root.limbs()[0];
        }
    }
    if (!isFine) {
        if (!(status & errorConditions))
            status |= OutOfMemory;
        result.setSpecial(0, Number::QuietNaN);
        return;
    }
    root.exponent = idealExponent;
    result.swap(root);
    Context halfEven = context;
    halfEven.rounding = Rounding::HalfEven;
    finalize(result, halfEven, status);
}

void squareRoot(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    uint32_t firstStatus = 0;
    Number root;
    squareRootCore(root, a, context, firstStatus);
    if (!(firstStatus & (OutOfMemory | DivisionImpossible))) [[likely]] {
        status |= firstStatus;
        result.swap(root);
        return;
    }
    // There may be no memory for all the digits of the precision. A root that ends has no more digits than the operand.
    Context smaller = context;
    smaller.precision = a.digits;
    if (smaller.precision >= context.precision) {
        status |= firstStatus | errorConditions;
        result.swap(root);
        return;
    }
    uint32_t secondStatus = 0;
    squareRootCore(root, a, smaller, secondStatus);
    if (secondStatus)
        root.setError((firstStatus | secondStatus) & errorConditions, status);
    result.swap(root);
}

// ---- Powers with integer exponents ----

// base**exponent for exponent > 0, by squaring and multiplying from the first bit of the exponent down, every product rounded as
// the context says.
static void powerOfUInt64(Number& result, const Number& base, uint64_t exponent, uint8_t sign, const Context& context, uint32_t& status)
{
    uint32_t ownStatus = 0;
    Number power;
    if (!power.copy(base, status)) {
        result.swap(power);
        return;
    }
    uint64_t bit = 1ull << (63 - __builtin_clzll(exponent));
    while (bit >>= 1) {
        multiply(power, power, power, context, ownStatus);
        if (exponent & bit)
            multiply(power, power, base, context, ownStatus);
        if (power.isSpecial() || (power.hasZeroCoefficient() && (ownStatus & Clamped)))
            break;
    }
    status |= ownStatus;
    power.setSign(sign);
    result.swap(power);
}

// The same for an exponent that is too large for that, from its last bit up. Both operands are used up.
static void powerOfLargeInteger(Number& result, Number& base, Number& exponent, uint8_t sign, const Context& context, uint32_t& status)
{
    uint32_t ownStatus = 0;
    Context unlimited = Context::maximum();
    Number two(0, 2, 0);
    result.set(0, 1, 0);
    while (!exponent.isZero()) {
        if (exponent.isOddInteger()) {
            multiply(result, result, base, context, ownStatus);
            status |= ownStatus;
            if (result.isSpecial() || (result.hasZeroCoefficient() && (ownStatus & Clamped)))
                break;
        }
        multiply(base, base, base, context, ownStatus);
        divideInteger(exponent, exponent, two, unlimited, ownStatus);
        if (base.isNaN() || exponent.isNaN()) {
            result.setError(ownStatus & errorConditions, status);
            return;
        }
    }
    result.setSign(sign);
}

// Whether one of the powers of the base on the way to base**exponent (or any greater exponent) is sure to be longer than a number can be. Each of
// them has about twice the digits of the one before, so it would be hours until a product finds out.
static bool powerIsSureToBeTooLong(const Number& base, uint64_t exponent, const Context& context)
{
    double mostDigits = static_cast<double>(mostLimbs()) * digitsPerLimb;
    if (static_cast<double>(context.precision) <= mostDigits)
        return false;
    // base**k has at least k * growth digits, as long as nothing is rounded.
    Limb first = base.top() / powersOfTen[digitCount(base.top()) - 1];
    double growth = static_cast<double>(base.digits - 1) + std::log10(static_cast<double>(first)) * 0.999;
    if (growth <= 0)
        return false;
    // Up to which k it does not overflow, and has no digits below the least exponent.
    double reached = static_cast<double>(exponent);
    if (base.adjustedExponent() >= 0)
        reached = std::min(reached, static_cast<double>(context.emax) / (static_cast<double>(base.adjustedExponent()) + 1));
    if (base.exponent < 0)
        reached = std::min(reached, static_cast<double>(context.etiny()) / static_cast<double>(base.exponent));
    // Of the exponents on the way, each is twice the one before or one more than that, so one of them is between k and 2 * k + 1, for any k.
    return 2.5 * (mostDigits / growth) + 2 < reached;
}

static void powerWithIntegerExponent(Number& result, const Number& base, const Number& exponent, uint8_t sign, const Context& context, uint32_t& status)
{
    Context working = context;
    working.precision += exponent.digits + exponent.exponent + 2;
    working.rounding = Rounding::HalfEven;
    working.clamp = false;
    Number workingBase;
    bool isFine = true;
    if (exponent.isNegative()) {
        uint32_t ownStatus = 0;
        ++working.precision;
        divide(workingBase, Number(0, 1, 0), base, working, ownStatus);
        status |= ownStatus;
        isFine = !(ownStatus & errorConditions);
    } else
        isFine = workingBase.copy(base, status);
    if (!isFine)
        result.setSpecial(0, Number::QuietNaN);
    else {
        uint32_t ownStatus = 0;
        uint64_t count = magnitudeToUInt64(exponent, ownStatus);
        if (powerIsSureToBeTooLong(workingBase, ownStatus & InvalidOperation ? std::numeric_limits<uint64_t>::max() : count, working))
            result.setError(OutOfMemory, status);
        else if (ownStatus & InvalidOperation) {
            Number workingExponent;
            if (!workingExponent.copy(exponent, status))
                result.setSpecial(0, Number::QuietNaN);
            else
                powerOfLargeInteger(result, workingBase, workingExponent, sign, working, status);
        } else
            powerOfUInt64(result, workingBase, count, sign, working, status);
        // So that the rounding mode of the caller decides what becomes of it.
        if (result.isInfinite())
            result.set(sign, 1, overflowingExponent);
    }
    finalize(result, context, status);
}

// ---- e**x ----

// How many terms of the series it takes for precision p, given the size of the reduced argument. -1: more than can be told.
static int64_t termsOfExponentialSeries(const Number& reduced, int64_t p)
{
    if (p > (static_cast<int64_t>(1) << 52))
        return -1;
    int64_t magnitude = (digitCount(static_cast<Limb>(p)) - 1) - (reduced.adjustedExponent() + 1);
    int64_t terms = static_cast<int64_t>(std::ceil((1.43503 * static_cast<double>(p) - 1.182) / static_cast<double>(magnitude)));
    return std::max<int64_t>(terms, 3);
}

// e**a for a finite a, with a relative error of less than half a unit in digit `precision` (an absolute one of a whole unit where the
// result is 1). Hull and Abrham, "Variable precision exponential function": a = r * 10**t with |r| < 1; e**r from the series, by Horner's rule; then the power.
static void exponentialApproximation(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (a.hasZeroCoefficient()) {
        result.set(0, 1, 0);
        return;
    }
    int64_t t = std::max<int64_t>(a.digits + a.exponent, 0);
    if (t > maxExponentDigits) {
        if (!a.isNegative()) {
            result.setSpecial(0, Number::Infinite);
            status |= Overflow | Inexact | Rounded;
        } else {
            result.set(0, 0, context.etiny());
            status |= Inexact | Rounded | Subnormal | Underflow | Clamped;
        }
        return;
    }
    {
        // So close to zero that the result is 1.
        Number limit(0, 9, -(context.precision + 1));
        if (compareMagnitudes(a, limit) <= 0) {
            result.set(0, 1, 0);
            status |= Rounded | Inexact;
            return;
        }
    }
    Context working = Context::maximum();
    working.precision = std::max<int64_t>(context.precision + t + 2, 10);
    uint32_t workingStatus = 0;

    Number reduced;
    if (!reduced.copy(a, status)) {
        result.swap(reduced);
        return;
    }
    reduced.exponent -= t;
    int64_t terms = termsOfExponentialSeries(reduced, working.precision);
    if (terms < 0) {
        result.setError(InvalidOperation, status);
        return;
    }
    Number sum(0, 1, 0);
    Number one(0, 1, 0);
    Number term;
    Number index;
    for (int64_t j = terms - 1; j >= 1; --j) {
        index.set(0, static_cast<Limb>(j), 0);
        divide(term, reduced, index, working, workingStatus);
        fusedMultiplyAdd(sum, sum, term, one, working, workingStatus);
        // Out of memory, and it stays a NaN. There may be billions of terms to go.
        if (sum.isSpecial())
            break;
    }
    powerOfUInt64(result, sum, powersOfTen[t], 0, working, status);
    status |= workingStatus & errorConditions;
    status |= Inexact | Rounded;
}

// Whether an approximation decides how the exact value is rounded: it does if one unit of its last place more and one less round alike.
// (Ziv's strategy: if not, the caller tries again with more digits.)
static bool roundingIsDecided(const Number& approximation, int64_t unitExponent, const Context& context, uint32_t& status)
{
    if (approximation.isSpecial() || approximation.hasZeroCoefficient())
        return true;
    Number unit(0, 1, unitExponent);
    Number above;
    Number below;
    uint32_t ignored = 0;
    add(above, approximation, unit, context, ignored);
    subtract(below, approximation, unit, context, ignored);
    return !compareQuietly(above, below, status);
}

void exponential(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (a.isSpecial()) {
        if (propagateNaN(result, a, context, status))
            return;
        if (a.isNegative())
            result.set(0, 0, 0);
        else
            result.setSpecial(0, Number::Infinite);
        return;
    }
    if (a.hasZeroCoefficient()) {
        result.set(0, 1, 0);
        return;
    }
    Context working = context;
    working.rounding = Rounding::HalfEven;
    working.clamp = false;
    Number approximation;
    for (int64_t precision = context.precision + 3;; precision += digitsPerLimb) {
        working.precision = precision;
        uint32_t ownStatus = 0;
        exponentialApproximation(approximation, a, working, ownStatus);
        status |= ownStatus;
        // After an underflow it has only the digits that are left.
        int64_t unitExponent = ownStatus & Underflow ? approximation.exponent : approximation.exponent + approximation.digits - precision;
        working.precision = context.precision;
        if (roundingIsDecided(approximation, unitExponent, working, status))
            break;
    }
    working.clamp = context.clamp;
    padWithZeros(approximation, working, status);
    noteUnderflow(approximation, working, status);
    finalize(approximation, working, status);
    result.swap(approximation);
}

// ---- Logarithms ----

static constexpr int64_t limbsOfLn10 = 64;
static constexpr int64_t digitsOfLn10 = limbsOfLn10 * digitsPerLimb;
// The first 1216 digits of ln(10) = 2.302585...
static const Limb ln10Limbs[limbsOfLn10] = {
    6983716328982174407ull, 9089704281976336583ull, 1515961135648465461ull, 4416816335727555703ull,
    2900988039194170265ull, 2307925037472986509ull, 107598438319191292ull, 3466624107184669231ull,
    4450099781311469159ull, 9807828059751193854ull, 7713456862091670584ull, 1492198849978748873ull,
    6528728696511086257ull, 2385392051446341972ull, 8692180205189339507ull, 6518769751037497088ull,
    2375253577097505395ull, 9095610299291824318ull, 982748238504564801ull, 5438635917781170543ull,
    7547331541421808427ull, 752371033310119785ull, 3171643095059950878ull, 9785265383207606726ull,
    2932258279850258550ull, 5497347726624257094ull, 2976979522110718264ull, 9221477656763693866ull,
    1979650047149510504ull, 6674183485704422507ull, 9702766860595249671ull, 9278096762712757753ull,
    9314848524948644871ull, 6826928280848118428ull, 754403708474699401ull, 230105703089634572ull,
    1929203337658714166ull, 7589402567763113569ull, 4208241314695689016ull, 2922455440575892572ull,
    9356734206705811364ull, 2684916746550586856ull, 644507064800027750ull, 9476834636167921018ull,
    5659121373450747856ull, 2835522011480466371ull, 6470806855677432162ull, 7141748003688084012ull,
    9619404400222105101ull, 5504893431493939147ull, 6674744042432743651ull, 2287698219886746543ull,
    7773262884616336622ull, 1985283935053089653ull, 4680843799894826233ull, 8168948290720832555ull,
    8067566662873690987ull, 6248633409525465082ull, 9829834196778404228ull, 3524802359972050895ull,
    3327900967572609677ull, 110148862877297603ull, 179914546843642076ull, 2302585092994045684ull,
};

// The precisions for a Newton iteration that doubles the number of correct digits, the last first: each is half the one before, and
// the one that is reached first is no more than what the initial approximation has. Returns the index of that one, or -1: no step is needed.
static int schedulePrecisions(int64_t (&list)[64], int64_t target, int64_t initial)
{
    if (target <= initial)
        return -1;
    int count = 0;
    int64_t k = target;
    do {
        k = (k + 2) / 2;
        list[count++] = k;
    } while (k > initial);
    return count - 1;
}

// One step of Newton's iteration for z = ln(v): z += v * e**-z - 1.
static void improveLogarithm(Number& z, const Number& v, int64_t k, uint32_t& status)
{
    Context unlimited = Context::maximum();
    Context truncating = Context::maximum();
    truncating.rounding = Rounding::Truncate;
    truncating.precision = 2 * k + 3;
    Number term;
    Number one(0, 1, 0);
    z.negate();
    exponentialApproximation(term, z, truncating, status);
    z.negate();
    if (v.digits > truncating.precision) {
        Number shortened;
        int64_t count = v.digits - truncating.precision;
        shiftCoefficientRight(shortened, v, count, status);
        shortened.exponent += count;
        multiply(term, shortened, term, truncating, status);
    } else
        multiply(term, v, term, truncating, status);
    subtract(term, term, one, unlimited, status);
    add(z, z, term, unlimited, status);
}

// ln(10), less than a unit in the last place off.
static void ln10(Number& result, int64_t precision, uint32_t& status)
{
    Number table(Number::Borrow, 0, -(digitsOfLn10 - 1), digitsOfLn10, limbsOfLn10, ln10Limbs);
    unsigned lost = shiftCoefficientRight(result, table, std::max<int64_t>(digitsOfLn10 - precision, 0), status);
    if (lost == shiftFailed) {
        result.setError(OutOfMemory, status);
        return;
    }
    result.exponent = -(result.digits - 1);
    Context unlimited = Context::maximum();
    if (precision < digitsOfLn10) {
        applyRoundingFreely(result, lost, unlimited, status);
        status |= Inexact | Rounded;
        return;
    }
    int64_t list[64];
    Number ten(0, 10, 0);
    for (int i = schedulePrecisions(list, precision + 2, -result.exponent); i >= 0; --i) {
        improveLogarithm(result, ten, list[i], status);
        if (result.isSpecial())
            break;
    }
    unlimited.precision = precision;
    finalize(result, unlimited, status);
}

// ln(a) for a finite a > 0 that is not 1, with a relative error of less than a tenth of a unit in digit `precision`.
static void logarithmApproximation(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    // a = v * 10**t, with 0.5 < v <= 5.
    Number v;
    if (!v.copy(a, status)) {
        result.setError(OutOfMemory, status);
        status |= Inexact | Rounded;
        return;
    }
    // The first 17 digits, as 1.xxx
    Number head;
    double leading;
    if (a.digits > 17) {
        shiftCoefficientRight(head, a, a.digits - 17, status);
        leading = static_cast<double>(head.limbs()[0]) / 1e16;
    } else
        leading = static_cast<double>(a.limbs()[0]) / static_cast<double>(powersOfTen[a.digits - 1]);
    int64_t t;
    bool isNearOne;
    if (leading < 5.01) {
        v.exponent = -(a.digits - 1);
        t = a.exponent + a.digits - 1;
        isNearOne = leading < 1.16;
    } else {
        v.exponent = -a.digits;
        t = a.exponent + a.digits;
        leading /= 10;
        isNearOne = leading >= 0.9;
    }
    // The initial approximation is off by less than 10**-14.
    constexpr int64_t initialPrecision = 14;
    Number z;
    {
        double approximation = std::log(leading);
        z.set(approximation < 0 ? Number::Negative : 0, static_cast<Limb>(std::llround(std::fabs(approximation) * 1e16)), -16);
    }

    Context unlimited = Context::maximum();
    int64_t target = context.precision + 2;
    bool underflows = false;
    if (!t && isNearOne) {
        // The logarithm is about v - 1. If that is too small for the context, there is nothing to compute. Otherwise, the closer it is to
        // zero, the more digits behind the point it takes for as many significant ones.
        int order = compareValues(v, Number(0, 1, 0));
        uint32_t ownStatus = 0;
        Number bound;
        subtractUnrounded(bound, v, Number(0, 1, 0), unlimited, ownStatus);
        if (ownStatus & errorConditions) {
            result.setError(OutOfMemory, status);
            status |= Inexact | Rounded;
            return;
        }
        // An upper bound: |v - 1| above 1, ten times that below.
        if (order < 0)
            ++bound.exponent;
        if (bound.adjustedExponent() < context.etiny()) {
            result.set(order < 0 ? Number::Negative : 0, 1, context.etiny() - 1);
            underflows = true;
        } else {
            // A lower bound: a tenth of the upper one.
            --bound.exponent;
            if (bound.adjustedExponent() < 0)
                target -= bound.adjustedExponent();
        }
    }
    if (!underflows) {
        // The result has that many digits. The steps begin with few, and it would be hours until one of them finds out.
        if (limbsForDigits(target) > mostLimbs()) {
            result.setError(OutOfMemory, status);
            status |= Inexact | Rounded;
            return;
        }
        int64_t list[64];
        for (int i = schedulePrecisions(list, target, initialPrecision); i >= 0; --i) {
            improveLogarithm(z, v, list[i], status);
            if (z.isSpecial())
                break;
        }
        Number constant;
        Number multiple;
        Number count;
        count.setInt64(t);
        ln10(constant, target + 1, status);
        multiply(multiple, constant, count, unlimited, status);
        add(result, multiple, z, unlimited, status);
    }
    status |= Inexact | Rounded;
}

// What ln and log10 do about special values, zero and negative numbers. True: that was all.
static bool logarithmOfSpecialValue(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (a.isSpecial()) {
        if (propagateNaN(result, a, context, status))
            return true;
        if (a.isNegative())
            result.setError(InvalidOperation, status);
        else
            result.setSpecial(0, Number::Infinite);
        return true;
    }
    if (a.hasZeroCoefficient()) {
        result.setSpecial(Number::Negative, Number::Infinite);
        return true;
    }
    if (a.isNegative()) {
        result.setError(InvalidOperation, status);
        return true;
    }
    return false;
}

void naturalLogarithm(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (logarithmOfSpecialValue(result, a, context, status))
        return;
    if (!compareValues(a, Number(0, 1, 0))) {
        result.set(0, 0, 0);
        return;
    }
    // |ln(a)| is more than twice |log10(a)|, of which the adjusted exponent is a bound: the result may be known to overflow.
    int64_t adjusted = a.adjustedExponent();
    int64_t bound = 2 * (adjusted < 0 ? -adjusted - 1 : adjusted);
    if (exponentDigits(bound) - 1 > context.emax) {
        status |= Overflow | Inexact | Rounded;
        result.setSpecial(adjusted < 0 ? Number::Negative : 0, Number::Infinite);
        return;
    }
    Context working = context;
    working.rounding = Rounding::HalfEven;
    working.clamp = false;
    Number approximation;
    for (int64_t precision = context.precision + 3;; precision += digitsPerLimb) {
        working.precision = precision;
        logarithmApproximation(approximation, a, working, status);
        int64_t unitExponent = approximation.exponent + approximation.digits - precision;
        working.precision = context.precision;
        if (roundingIsDecided(approximation, unitExponent, working, status))
            break;
    }
    working.clamp = context.clamp;
    noteUnderflow(approximation, working, status);
    finalize(approximation, working, status);
    result.swap(approximation);
}

void commonLogarithm(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (logarithmOfSpecialValue(result, a, context, status))
        return;
    Context working = context;
    working.rounding = Rounding::HalfEven;
    int64_t adjusted = a.adjustedExponent();
    if (a.coefficientIsPowerOfTen()) {
        result.setInt64(adjusted);
        finalize(result, working, status);
        return;
    }
    int64_t bound = adjusted < 0 ? -adjusted - 1 : adjusted;
    if (exponentDigits(bound) - 1 > context.emax) {
        status |= Overflow | Inexact | Rounded;
        result.setSpecial(adjusted < 0 ? Number::Negative : 0, Number::Infinite);
        return;
    }
    working.clamp = false;
    Number approximation;
    for (int64_t precision = context.precision + 3;; precision += digitsPerLimb) {
        // ln(a) / ln(10), each with three digits more.
        Context wider = Context::maximum();
        wider.precision = precision + 3;
        Number constant;
        logarithmApproximation(approximation, a, wider, status);
        if (approximation.isSpecial())
            break;
        ln10(constant, wider.precision, status);
        divideCore(approximation, approximation, constant, wider, status, true);
        int64_t unitExponent = approximation.exponent + approximation.digits - precision;
        working.precision = context.precision;
        if (roundingIsDecided(approximation, unitExponent, working, status))
            break;
    }
    working.clamp = context.clamp;
    noteUnderflow(approximation, working, status);
    finalize(approximation, working, status);
    result.swap(approximation);
}

// ---- x**y ----

static void setOneWithAllDigits(Number& result, uint8_t sign, int64_t zeros, uint32_t& status)
{
    if (!shiftCoefficientLeft(result, Number(0, 1, 0), zeros, status))
        return;
    result.exponent = -zeros;
    result.flags = sign;
}

// If |base| is 1, the result is set. Returns how |base| compares with 1.
static int powerOfOne(Number& result, const Number& base, const Number& exponent, uint8_t sign, const Context& context, uint32_t& status)
{
    int order = compareMagnitudes(base, Number(0, 1, 0));
    if (order)
        return order;
    int64_t zeros;
    if (exponent.isIntegerValued()) {
        if (exponent.isNegative()) {
            result.set(sign, 1, 0);
            return 0;
        }
        // 1.000**3 is 1.000000000, as far as the precision goes.
        uint32_t ownStatus = 0;
        Number count;
        count.setInt64(-base.exponent);
        multiply(result, exponent, count, context, ownStatus);
        if (ownStatus & errorConditions) {
            status |= ownStatus & errorConditions;
            return 0;
        }
        zeros = toInt64(result, ownStatus);
        if (zeros > context.precision - 1) {
            zeros = context.precision - 1;
            status |= Rounded;
        }
    } else {
        zeros = context.precision - 1;
        status |= Inexact | Rounded;
    }
    setOneWithAllDigits(result, sign, zeros, status);
    return 0;
}

// A lower bound for log10(|log10(x)|), for x > 0 that is not 1. INT64_MAX: out of memory.
static int64_t magnitudeOfLogarithm(const Number& x, uint32_t& status)
{
    int64_t t = x.adjustedExponent();
    if (t > 0)
        return exponentDigits(t) - 1;
    if (t < -1)
        return exponentDigits(t + 1) - 1;
    Context unlimited = Context::maximum();
    Number difference;
    subtract(difference, x, Number(0, 1, 0), unlimited, status);
    if (difference.isSpecial())
        return std::numeric_limits<int64_t>::max();
    int64_t u = difference.adjustedExponent();
    return !t ? u - 2 : u - 1;
}

// Whether x**y is sure to overflow or to underflow, going by the sizes of log10(x) and y. If so, the result is set.
static bool powerIsOutOfBounds(Number& result, const Number& x, const Number& y, uint8_t sign, const Context& context, uint32_t& status)
{
    Number magnitude(Number::Borrow, x.flags & ~Number::Negative, x.exponent, x.digits, x.length, x.limbs());
    int64_t sizeOfY = y.adjustedExponent();
    int64_t sizeOfLog = magnitudeOfLogarithm(magnitude, status);
    if (sizeOfLog == std::numeric_limits<int64_t>::max()) {
        result.setError(OutOfMemory, status);
        return true;
    }
    bool shrinks = (magnitude.adjustedExponent() < 0) != y.isNegative();
    if (!shrinks) {
        if (exponentDigits(context.emax) < sizeOfLog + sizeOfY) {
            result.set(sign, 1, overflowingExponent);
            finalize(result, context, status);
            return true;
        }
    } else if (exponentDigits(context.etiny()) < sizeOfLog + sizeOfY) {
        result.set(sign, 1, context.etiny() - 1);
        finalize(result, context, status);
        return true;
    }
    return false;
}

// e**(y * ln(x)), each of the three steps correctly rounded to 23 digits more than are wanted.
static void powerWithRealExponent(Number& result, const Number& base, const Number& exponent, const Context& context, uint32_t& status)
{
    Context working = Context::maximum();
    working.precision = std::max(base.digits, context.precision) + 4 + maxExponentDigits;
    uint32_t workingStatus = 0;
    Number value;
    naturalLogarithm(value, base, working, workingStatus);
    multiply(value, value, exponent, working, workingStatus);
    exponential(value, value, working, status);
    result.swap(value);
    status |= workingStatus & errorConditions;
    status |= Inexact | Rounded;
}

void power(Number& result, const Number& base, const Number& exponent, const Context& context, uint32_t& status)
{
    if (propagateNaN(result, base, exponent, context, status))
        return;
    bool exponentIsInteger = exponent.isInteger();
    uint8_t sign = exponentIsInteger && base.isNegative() && exponent.isOddInteger() ? Number::Negative : 0;

    if (base.isZero()) {
        if (exponent.isZero())
            result.setError(InvalidOperation, status);
        else if (exponent.isNegative())
            result.setSpecial(sign, Number::Infinite);
        else
            result.set(sign, 0, 0);
        return;
    }
    if (base.isNegative() && (!exponentIsInteger || exponent.isInfinite())) {
        result.setError(InvalidOperation, status);
        return;
    }
    if (exponent.isInfinite()) {
        int order = compareValues(base, Number(0, 1, 0));
        if (!order) {
            setOneWithAllDigits(result, sign, context.precision - 1, status);
            status |= Inexact | Rounded;
        } else if (order * exponent.arithmeticSign() < 0)
            result.set(sign, 0, 0);
        else
            result.setSpecial(sign, Number::Infinite);
        return;
    }
    if (base.isInfinite()) {
        if (exponent.isZero())
            result.set(sign, 1, 0);
        else if (exponent.isNegative())
            result.set(sign, 0, 0);
        else
            result.setSpecial(sign, Number::Infinite);
        return;
    }
    if (exponent.isZero()) {
        result.set(sign, 1, 0);
        return;
    }
    Number value;
    if (!powerOfOne(value, base, exponent, sign, context, status) || powerIsOutOfBounds(value, base, exponent, sign, context, status)) {
        result.swap(value);
        return;
    }
    if (exponentIsInteger) {
        powerWithIntegerExponent(value, base, exponent, sign, context, status);
        result.swap(value);
        return;
    }
    powerWithRealExponent(value, base, exponent, context, status);
    if (!value.isSpecial() && !compareValues(value, Number(0, 1, 0)))
        setOneWithAllDigits(value, 0, context.precision - 1, status);
    if (value.isInfinite())
        value.set(0, 1, overflowingExponent);
    finalize(value, context, status);
    result.swap(value);
}

// ---- (x**y) % m ----

static void multiplyModulo(Number& result, const Number& a, const Number& b, const Number& modulus, const Context& unlimited, uint32_t& status)
{
    multiplyExactly(result, a, b, unlimited, status);
    remainder(result, result, modulus, unlimited, status);
}

// base**exponent % modulus. The base is used up.
static void powerModuloOfUInt64(Number& result, Number& base, uint64_t exponent, const Number& modulus, const Context& unlimited, uint32_t& status)
{
    result.set(0, 1, 0);
    while (exponent) {
        if (exponent & 1)
            multiplyModulo(result, result, base, modulus, unlimited, status);
        multiplyModulo(base, base, base, modulus, unlimited, status);
        exponent >>= 1;
    }
}

void powerModulo(Number& result, const Number& base, const Number& exponent, const Number& modulus, const Context& context, uint32_t& status)
{
    if ((base.flags | exponent.flags | modulus.flags) & Number::anySpecial) {
        if (!propagateNaN(result, base, exponent, modulus, context, status))
            result.setError(InvalidOperation, status);
        return;
    }
    if (!base.isIntegerValued() || !exponent.isIntegerValued() || !modulus.isIntegerValued() || modulus.hasZeroCoefficient()
        || modulus.digits + modulus.exponent > context.precision) {
        result.setError(InvalidOperation, status);
        return;
    }
    uint8_t sign = base.isNegative() && exponent.isOddInteger() ? Number::Negative : 0;
    if (exponent.hasZeroCoefficient()) {
        if (base.hasZeroCoefficient())
            result.setError(InvalidOperation, status);
        else
            result.set(sign, compareMagnitudes(modulus, Number(0, 1, 0)) ? 1 : 0, 0);
        return;
    }
    if (exponent.isNegative()) {
        result.setError(InvalidOperation, status);
        return;
    }
    if (base.hasZeroCoefficient()) {
        result.set(sign, 0, 0);
        return;
    }

    Context unlimited = Context::maximum();
    Number m;
    {
        uint32_t ownStatus = 0;
        rescale(m, modulus, 0, unlimited, ownStatus);
        if (ownStatus & errorConditions) {
            result.setError(ownStatus & errorConditions, status);
            return;
        }
        m.setSign(0);
    }
    // Both as integer coefficients and powers of ten.
    Number b;
    Number e;
    Number value;
    Number scratch;
    roundToIntegralValue(b, base, unlimited, status);
    b.setSign(0);
    int64_t exponentOfBase = b.exponent;
    b.exponent = 0;
    roundToIntegralValue(e, exponent, unlimited, status);
    int64_t exponentOfExponent = e.exponent;
    e.exponent = 0;

    remainder(b, b, m, unlimited, status);
    {
        Number ten(0, 10, 0);
        powerModuloOfUInt64(value, ten, static_cast<uint64_t>(exponentOfBase), m, unlimited, status);
        remainder(value, value, m, unlimited, status);
    }
    multiplyModulo(b, b, value, m, unlimited, status);
    bool isFine = !b.isSpecial() && !e.isSpecial() && !m.isSpecial();
    for (int64_t i = 0; isFine && i < exponentOfExponent; ++i) {
        powerModuloOfUInt64(scratch, b, 10, m, unlimited, status);
        b.swap(scratch);
        isFine = !b.isSpecial();
    }
    if (isFine) {
        Number two(0, 2, 0);
        value.set(0, 1, 0);
        // 1 % 1 is 0.
        remainder(value, value, m, unlimited, status);
        while (e.isFinite() && !e.hasZeroCoefficient()) {
            if (e.isOddInteger())
                multiplyModulo(value, value, b, m, unlimited, status);
            multiplyModulo(b, b, b, m, unlimited, status);
            divideInteger(e, e, two, unlimited, status);
        }
        isFine = !e.isSpecial() && !b.isSpecial() && !value.isSpecial();
    }
    if (!isFine) {
        result.setSpecial(0, Number::QuietNaN);
        return;
    }
    value.setSign(sign);
    result.swap(value);
}

} // namespace Decimal
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
