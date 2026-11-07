/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <compare>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {

// A Decimal value of https://tc39.es/proposal-decimal/: an IEEE 754-2019 Decimal128 value without cohorts, and with one NaN.
//
// A finite non-zero value is coefficient * 10^exponent, and the coefficient never has a trailing zero, so equal values have equal
// representations. Because of that the exponent goes up to 6144 (1 * 10^6144), above the 6111 of a 34-digit coefficient.
class Decimal128 {
public:
    enum class RoundingMode : uint8_t { Ceil, Floor, Trunc, HalfExpand, HalfEven };

    static constexpr int precision = 34;
    static constexpr int minimumQuantumExponent = -6176;
    static constexpr int maximumQuantumExponent = 6111;

    constexpr Decimal128() = default; // +0

    static constexpr Decimal128 nan() { return { Kind::NaN, false }; }
    static constexpr Decimal128 infinity(bool isNegative) { return { Kind::Infinity, isNegative }; }
    static constexpr Decimal128 zero(bool isNegative) { return { Kind::Finite, isNegative }; }

    // StrDecimalLiteral or "NaN". No white space, no numeric separators. nullopt is a syntax error.
    static std::optional<Decimal128> parse(StringView);

    bool isNaN() const { return m_kind == Kind::NaN; }
    bool isInfinity() const { return m_kind == Kind::Infinity; }
    bool isFinite() const { return m_kind == Kind::Finite; }
    bool isZero() const { return isFinite() && !m_low && !m_high; }
    bool isNegative() const { return m_isNegative; } // Also true of -0. False for NaN.
    bool isInteger() const { return isFinite() && (isZero() || m_exponent >= 0); }

    // The exponent in scientific notation: 10^e <= |this| < 10^(e + 1). Finite and not zero.
    int scientificExponent() const;
    // The value s with this = s * 10^scientificExponent(). Finite and not zero.
    Decimal128 significand() const;

    Decimal128 abs() const;
    Decimal128 negate() const;

    static Decimal128 add(const Decimal128&, const Decimal128&);
    static Decimal128 subtract(const Decimal128&, const Decimal128&);
    static Decimal128 multiply(const Decimal128&, const Decimal128&);
    static Decimal128 divide(const Decimal128&, const Decimal128&);
    static Decimal128 remainder(const Decimal128&, const Decimal128&);

    // NaN is unordered. -0 and +0 are equivalent.
    static std::partial_ordering compare(const Decimal128&, const Decimal128&);

    // The count of digits and the power are integers that may be of any magnitude.
    Decimal128 round(double numberOfFractionalDigits, RoundingMode) const;
    Decimal128 scale10(double power) const;

    String toString() const;
    String toExponentialString() const;
    // All digits of an integer, without an exponent.
    String toIntegerString() const;
    // Null if the result is longer than a string can be.
    String toFixed(double numberOfDigits, RoundingMode) const;
    String toPrecision(double numberOfDigits, RoundingMode) const;

private:
    class Wide;
    enum class Kind : uint8_t { Finite, Infinity, NaN };

    constexpr Decimal128(Kind kind, bool isNegative)
        : m_kind(kind)
        , m_isNegative(isNegative)
    {
    }

    Wide coefficient() const;
    // For a value that is only written: the coefficient may end in zeros, or be 10^34.
    void setUnrestrictedCoefficient(const Wide&);
    Vector<Latin1Character, 40> coefficientDigits() const;

    // The value nearest to coefficient * 10^exponent, plus something greater than 0 and less than one unit of the coefficient if isInexact.
    // An inexact coefficient has to have more digits than the precision.
    static Decimal128 roundToDomain(bool isNegative, Wide, int64_t exponent, bool isInexact = false);
    static Wide roundedQuotientByPowerOf10(const Wide&, int64_t power, bool isInexact, RoundingMode);
    // Rounds away everything below 10^exponent. Answers the count of that unit.
    Wide magnitudeInUnitsOf(int64_t exponent, RoundingMode) const;
    static RoundingMode modeForMagnitude(bool isNegative, RoundingMode);

    uint64_t m_low { 0 };
    uint64_t m_high { 0 };
    int16_t m_exponent { 0 };
    Kind m_kind { Kind::Finite };
    bool m_isNegative { false };
};

} // namespace JSC
