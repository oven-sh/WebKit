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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "Rounding.h"

#include "FractionToDouble.h"
#include "TemporalObject.h"

namespace JSC {
namespace TemporalCore {

// NegateRoundingMode — temporal_rs: RoundingMode::negate (src/options.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-negateroundingmode
RoundingMode negateTemporalRoundingMode(RoundingMode roundingMode)
{
    switch (roundingMode) {
    case RoundingMode::Ceil:
        return RoundingMode::Floor;
    case RoundingMode::Floor:
        return RoundingMode::Ceil;
    case RoundingMode::HalfCeil:
        return RoundingMode::HalfFloor;
    case RoundingMode::HalfFloor:
        return RoundingMode::HalfCeil;
    default:
        return roundingMode;
    }
}

// ApplyUnsignedRoundingMode — temporal_rs: apply_unsigned_rounding_mode
// https://tc39.es/proposal-temporal/#sec-applyunsignedroundingmode
Int128 applyUnsignedRoundingMode(Int128 xNumerator, Int128 xDenominator, Int128 r1, Int128 r2, UnsignedRoundingMode unsignedRoundingMode)
{
    ASSERT(xDenominator > 0);
    Int128 scaledR1 = r1 * xDenominator;
    // 1. If x = r1, return r1.
    if (xNumerator == scaledR1)
        return r1;
    // 2. Assert: r1 < x < r2. 3. Assert: unsignedRoundingMode is not undefined.
    ASSERT(scaledR1 < xNumerator && xNumerator < r2 * xDenominator);
    // 4. If unsignedRoundingMode is ~zero~, return r1.
    if (unsignedRoundingMode == UnsignedRoundingMode::Zero)
        return r1;
    // 5. If unsignedRoundingMode is ~infinity~, return r2.
    if (unsignedRoundingMode == UnsignedRoundingMode::Infinity)
        return r2;
    // 6-9. d1 = x - r1, d2 = r2 - x; d1 < d2 iff 2x < r1 + r2.
    Int128 doubledX = xNumerator * 2;
    Int128 sumOfBounds = (r1 + r2) * xDenominator;
    if (doubledX < sumOfBounds)
        return r1;
    if (doubledX > sumOfBounds)
        return r2;
    // 10. Assert: d1 is equal to d2.
    // 11. If unsignedRoundingMode is ~half-zero~, return r1.
    if (unsignedRoundingMode == UnsignedRoundingMode::HalfZero)
        return r1;
    // 12. If unsignedRoundingMode is ~half-infinity~, return r2.
    if (unsignedRoundingMode == UnsignedRoundingMode::HalfInfinity)
        return r2;
    // 13. Assert: unsignedRoundingMode is ~half-even~.
    ASSERT(unsignedRoundingMode == UnsignedRoundingMode::HalfEven);
    // 14-16. cardinality = (r1 / (r2 - r1)) modulo 2.
    return !((r1 / (r2 - r1)) % 2) ? r1 : r2;
}

// MaximumTemporalDurationRoundingIncrement — temporal_rs: Unit::to_maximum_rounding_increment (src/options.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-maximumtemporaldurationroundingincrement
std::optional<unsigned> maximumRoundingIncrement(TemporalUnit unit)
{
    // 1. Return the value from the "Maximum duration rounding increment" column of #table-temporal-units.
    // Year/Month/Week/Day have no maximum (return ~unset~); time units return fixed values.
    if (unit <= TemporalUnit::Day)
        return std::nullopt;
    if (unit == TemporalUnit::Hour)
        return 24;
    if (unit <= TemporalUnit::Second)
        return 60;
    return 1000;
}

// RoundNumberToIncrement (double path) — temporal_rs: IncrementRounder<f64>::round (src/rounding.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrement
// NOTE: Stays signed throughout rather than abs+negate like temporal_rs/spec.
// truncatedQuotient = -r1 (negative) or r1 (positive); expandedQuotient = -r2 or r2.
// Produces identical results to IncrementRounder<f64>::round.
double roundNumberToIncrementDouble(double x, double increment, RoundingMode mode)
{
    // quotient = x / increment (spec step 1).
    auto quotient = x / increment;
    auto truncatedQuotient = std::trunc(quotient);
    if (truncatedQuotient == quotient)
        return truncatedQuotient * increment;

    auto isNegative = quotient < 0;
    // expandedQuotient = r2 (positive) or r1 (negative) in spec terms.
    auto expandedQuotient = isNegative ? truncatedQuotient - 1 : truncatedQuotient + 1;

    if (mode >= RoundingMode::HalfCeil) {
        auto unsignedFractionalPart = std::abs(quotient - truncatedQuotient);
        if (unsignedFractionalPart < 0.5)
            return truncatedQuotient * increment;
        if (unsignedFractionalPart > 0.5)
            return expandedQuotient * increment;
    }

    switch (mode) {
    case RoundingMode::Ceil:
    case RoundingMode::HalfCeil:
        return (isNegative ? truncatedQuotient : expandedQuotient) * increment;
    case RoundingMode::Floor:
    case RoundingMode::HalfFloor:
        return (isNegative ? expandedQuotient : truncatedQuotient) * increment;
    case RoundingMode::Expand:
    case RoundingMode::HalfExpand:
        return expandedQuotient * increment;
    case RoundingMode::Trunc:
    case RoundingMode::HalfTrunc:
        return truncatedQuotient * increment;
    case RoundingMode::HalfEven:
        return (!std::fmod(truncatedQuotient, 2) ? truncatedQuotient : expandedQuotient) * increment;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// RoundNumberToIncrementAsIfPositive — temporal_rs: IncrementRounder::round_as_if_positive (src/rounding.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrementasifpositive
Int128 roundNumberToIncrementAsIfPositive(Int128 x, Int128 increment, RoundingMode roundingMode)
{
    // 1. Let quotient be x / increment.
    Int128 quotient;
    Int128 remainder;
    // Most epoch nanoseconds (within int64, about ±292 years around 1970) and increments fit in 64-bit division.
    if (x >= std::numeric_limits<int64_t>::min() && x <= std::numeric_limits<int64_t>::max() && increment > 0 && increment <= std::numeric_limits<int64_t>::max()) [[likely]] {
        quotient = static_cast<int64_t>(x) / static_cast<int64_t>(increment);
        remainder = static_cast<int64_t>(x) % static_cast<int64_t>(increment);
    } else {
        quotient = x / increment;
        remainder = x % increment;
    }
    // Exact case: x is divisible by increment — return x directly before computing r1/r2.
    // (spec step 1 of ApplyUnsignedRoundingMode: "if x = r1, return r1")
    if (!remainder)
        return x;
    // 2. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, ~positive~).
    auto unsignedRoundingMode = getUnsignedRoundingMode(roundingMode, false);
    // 3. Let r1 be the largest integer such that r1 ≤ quotient.
    // 4. Let r2 be the smallest integer such that r2 > quotient.
    auto r1 = quotient;
    auto r2 = quotient + 1;
    if (x < 0) {
        r1 = quotient - 1;
        r2 = quotient;
    }
    // 5. Let rounded be ApplyUnsignedRoundingMode(quotient, r1, r2, unsignedRoundingMode).
    // 6. Return rounded × increment.
    return applyUnsignedRoundingMode(x, increment, r1, r2, unsignedRoundingMode) * increment;
}

template<typename Integer>
static Integer roundNumberToIncrementImpl(Integer x, Integer increment, RoundingMode mode)
{
    // 1. Let quotient be x / increment.
    Integer quotient = x / increment;
    Integer remainder = x % increment;
    if (!remainder)
        return x;
    // 2-3. Determine isNegative from x; work with abs(quotient) as unsigned quotient.
    bool isNegative = x < 0;
    // 4. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, isNegative).
    auto unsignedRoundingMode = getUnsignedRoundingMode(mode, isNegative);
    // 5. Let r1 be the largest integer such that r1 ≤ abs(quotient).
    Integer r1 = quotient < 0 ? -quotient : quotient;
    // 6. Let r2 be the smallest integer such that r2 > abs(quotient).
    Integer r2 = r1 + 1;
    // 7. Let rounded be ApplyUnsignedRoundingMode(abs(quotient), r1, r2, unsignedRoundingMode).
    Integer rounded = static_cast<Integer>(applyUnsignedRoundingMode(isNegative ? -x : x, increment, r1, r2, unsignedRoundingMode));
    // 8. If isNegative is ~negative~, set rounded to -rounded.
    if (isNegative)
        rounded = -rounded;
    // 9. Return rounded × increment.
    return rounded * increment;
}

// RoundNumberToIncrement (Int128 path) — temporal_rs: IncrementRounder<i128>::round (src/rounding.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrement
// NOTE: With |x| and increment below 2^61 every intermediate fits in int64, so the divisions run in int64.
Int128 roundNumberToIncrementInt128(Int128 x, Int128 increment, RoundingMode mode)
{
    constexpr int64_t int64Limit = static_cast<int64_t>(1) << 61;
    if (x > -int64Limit && x < int64Limit && increment > 0 && increment < int64Limit) [[likely]]
        return roundNumberToIncrementImpl<int64_t>(static_cast<int64_t>(x), static_cast<int64_t>(increment), mode);
    return roundNumberToIncrementImpl<Int128>(x, increment, mode);
}

// ValidateTemporalRoundingIncrement — temporal_rs: RoundingIncrement::validate (src/options/increment.rs)
// https://tc39.es/proposal-temporal/#sec-validatetemporalroundingincrement
// NOTE: dividend is optional here; callers pass nullopt when largestUnit ≤ Day (no maximum applies).
// In that case we use nsPerSecond as a safe upper bound (largest valid nanosecond increment).
TemporalResult<void> validateTemporalRoundingIncrement(double increment, std::optional<double> dividend, Inclusivity isInclusive)
{
    // 1. If inclusive is true, then a. Let maximum be dividend.
    // 2. Else, b. Let maximum be dividend - 1.
    double maximum;
    if (!dividend)
        maximum = static_cast<double>(lengthInNanoseconds(TemporalUnit::Second));
    else if (isInclusive == Inclusivity::Inclusive)
        maximum = dividend.value();
    else if (dividend.value() > 1)
        maximum = dividend.value() - 1;
    else
        maximum = 1;

    increment = std::trunc(increment);
    // 3. If increment > maximum, throw a RangeError exception.
    if (increment < 1 || increment > maximum)
        return makeUnexpected(TemporalError { TemporalErrorKind::RangeError, "rounding increment is out of range"_s });
    // 4. If dividend modulo increment ≠ 0, throw a RangeError exception.
    //    In int64 when the dividend is an integer in (0, 2^62); a NaN increment takes fmod, whose NaN is a remainder.
    if (dividend) {
        constexpr double int64DividendLimit = 0x1p62;
        double dividendValue = dividend.value();
        bool isIntegralInt64 = std::trunc(dividendValue) == dividendValue && dividendValue > 0 && dividendValue < int64DividendLimit && increment >= 1;
        bool hasRemainder;
        if (isIntegralInt64)
            hasRemainder = static_cast<int64_t>(dividendValue) % static_cast<int64_t>(increment);
        else
            hasRemainder = std::fmod(dividendValue, increment);
        if (hasRemainder)
            return makeUnexpected(TemporalError { TemporalErrorKind::RangeError, "roundingIncrement does not divide evenly"_s });
    }
    // 5. Return ~unused~.
    return { };
}

} // namespace TemporalCore
} // namespace JSC
