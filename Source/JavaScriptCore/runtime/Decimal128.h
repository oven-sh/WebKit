/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <array>
#include <bit>
#include <compare>
#include <optional>
#include <span>
#include <wtf/HashFunctions.h>
#include <wtf/Int128.h>
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

    static constexpr Decimal128 nan() { return { exponentOfNaN, false }; }
    static constexpr Decimal128 infinity(bool isNegative) { return { exponentOfInfinity, isNegative }; }
    static constexpr Decimal128 zero(bool isNegative) { return { 0, isNegative }; }
    static constexpr Decimal128 one()
    {
        Decimal128 result;
        result.m_low = 1;
        return result;
    }

    // StrDecimalLiteral or "NaN". No white space, no numeric separators. nullopt is a syntax error.
    static std::optional<Decimal128> parse(StringView);

    bool isNaN() const { return m_exponent == exponentOfNaN; }
    bool isInfinity() const { return m_exponent == exponentOfInfinity; }
    bool isFinite() const { return m_exponent < exponentOfInfinity; }
    bool isZero() const { return isFinite() && !m_low && !m_high; }
    bool isNegative() const { return m_isNegative; } // Also true of -0. False for NaN.
    bool isInteger() const { return isFinite() && (isZero() || m_exponent >= 0); }

    // The exponent in scientific notation: 10^e <= |this| < 10^(e + 1). Finite and not zero.
    int scientificExponent() const;
    // The value s with this = s * 10^scientificExponent(). Finite and not zero.
    Decimal128 significand() const;

    // What the methods of these names answer.
    double exponent() const;
    Decimal128 mantissa() const;
    Decimal128 abs() const;
    Decimal128 negate() const;

    // The operands and the results are in registers.
    static Decimal128 add(Decimal128, Decimal128);
    static Decimal128 subtract(Decimal128, Decimal128);
    static Decimal128 multiply(Decimal128, Decimal128);
    static Decimal128 divide(Decimal128, Decimal128);
    static Decimal128 remainder(Decimal128, Decimal128);

    // As Object.is() of Numbers: NaN is itself, and -0 and +0 differ. The representation of a value is unique.
    bool isSameValueAs(const Decimal128& other) const
    {
        // Word by word: to compare the arrays, the compiler stores them first and waits for that.
        auto [low, upper] = words();
        auto [otherLow, otherUpper] = other.words();
        return low == otherLow && upper == otherUpper;
    }

    // For generated code: the coefficient's lower 64 bits, and the rest. See the fields.
    std::array<uint64_t, 2> words() const { return std::bit_cast<std::array<uint64_t, 2>>(*this); }
    static Decimal128 fromWords(uint64_t low, uint64_t upper) { return std::bit_cast<Decimal128>(std::array { low, upper }); }
    static constexpr ptrdiff_t offsetOfLow() { return 0; }
    static constexpr ptrdiff_t offsetOfUpper() { return 8; }
    static constexpr int shiftOfExponent = 49;
    static constexpr int bitsOfExponent = 14;
    // Above every exponent of a finite value.
    static constexpr int exponentOfInfinity = 8190;
    static constexpr int exponentOfNaN = 8191;
    static constexpr uint64_t signBit = 1ull << 63;
    static constexpr uint64_t upperOfNaN = static_cast<uint64_t>(exponentOfNaN) << shiftOfExponent;
    // Whether the compiler has laid out the fields so. A constant expression cannot tell.
    static bool hasLayoutOfGeneratedCode()
    {
        using Words = std::array<uint64_t, 2>;
        return nan().words() == Words { 0, upperOfNaN } && one().words() == Words { 1, 0 } && zero(true).words() == Words { 0, signBit };
    }
    // 10^i for i < 20.
    static const uint64_t* powersOf10();
    // What brings two coefficients to one unit, and the greatest coefficients that are less than 2^62 then. The index is the first exponent
    // less the other, of either sign.
    struct Alignment {
        uint64_t factor;
        uint64_t otherFactor;
        uint64_t limit;
        uint64_t otherLimit;
    };
    static constexpr int reachOfAlignments = 18;
    static const Alignment* alignmentsByDistance();
    // x / 10^i is (x >> i) * reciprocal >> (64 + shift) for x < 2^64 and i > 0.
    struct Divisor {
        uint64_t value;
        uint64_t half;
        uint64_t reciprocal;
        uint64_t shift;
    };
    static constexpr int greatestPowerOfDivisors = 19;
    static const Divisor* divisorsByPower();

    // For caches: each of the factors holds one of the words, so in the rare case that one of them is 0 the other word does not matter.
    unsigned hash(uint64_t seed) const
    {
        auto [low, upper] = words();
        UInt128 product = static_cast<UInt128>(low ^ seed ^ WTF::intHashSecrets[0]) * static_cast<UInt128>(upper ^ WTF::intHashSecrets[1]);
        return static_cast<unsigned>(static_cast<uint64_t>(product) ^ static_cast<uint64_t>(product >> 64));
    }

    // Whether its texts are short, with a number of digits after the point or in all that is.
    bool isShort() const { return !isFinite() || std::abs(static_cast<int>(m_exponent)) <= 64; }

    // Of an integer, and of what Number::toString gives for a double. -0 is +0.
    static Decimal128 fromInteger(int64_t);
    static Decimal128 fromDouble(double);
    // The nearest double.
    double toDouble() const;

    // https://tc39.es/proposal-decimal/#dfn-decimal-rounding-mode
    static std::optional<RoundingMode> roundingModeNamed(StringView);

    // NaN is unordered. -0 and +0 are equivalent.
    static std::partial_ordering compare(Decimal128, Decimal128);

    // The count of digits and the power are integers that may be of any magnitude.
    Decimal128 round(double numberOfFractionalDigits, RoundingMode) const;
    Decimal128 scale10(double power) const;

    String toString() const;
    // Never with an exponent.
    String toDecimalString() const;
    String toExponentialString() const;
    // All digits of an integer, without an exponent.
    String toIntegerString() const;
    // Null if the result is longer than a string can be.
    String toFixed(double numberOfDigits, RoundingMode) const;
    String toPrecision(double numberOfDigits, RoundingMode) const;

private:
    class Wide;

    constexpr Decimal128(int exponent, bool isNegative)
        : m_exponent(exponent)
        , m_isNegative(isNegative)
    {
    }

    Wide coefficient() const;
    struct Digits {
        std::span<const Latin1Character> span() const { return std::span { characters }.subspan(start); }
        size_t size() const { return characters.size() - start; }
        std::array<Latin1Character, 40> characters;
        unsigned start;
    };
    Digits coefficientDigits() const;

    template<typename CharacterType> static std::optional<Decimal128> parse(std::span<const CharacterType>);
    // Apart, so that the common cases need no frame.
    static Decimal128 addSlow(Decimal128, Decimal128);
    static Decimal128 multiplySlow(Decimal128, Decimal128);
    static std::partial_ordering compareSlow(Decimal128, Decimal128);
    static Decimal128 fromExact(bool isNegative, uint64_t high, uint64_t low, int64_t exponent);

    // The value nearest to coefficient * 10^exponent, plus something greater than 0 and less than one unit of the coefficient if isInexact.
    // An inexact coefficient has to have more digits than the precision.
    static Decimal128 roundToDomain(bool isNegative, uint64_t high, uint64_t low, int64_t exponent, bool isInexact);
    static Decimal128 roundToDomainSlow(bool isNegative, uint64_t high, uint64_t low, int64_t exponent, bool isInexact);
    static Decimal128 roundToDomain(bool isNegative, const Wide&, int64_t exponent, bool isInexact);
    static Wide roundedQuotientByPowerOf10(const Wide&, int64_t power, bool isInexact);
    void roundCoefficient(int64_t power, RoundingMode, uint64_t& high, uint64_t& low) const;
    static RoundingMode modeForMagnitude(bool isNegative, RoundingMode);

    uint64_t m_low { 0 };
    // 10^34 is less than 2^113, and an exponent needs 14 bits.
    uint64_t m_high : shiftOfExponent { 0 };
    int64_t m_exponent : bitsOfExponent { 0 };
    uint64_t m_isNegative : 1 { 0 };
};

static_assert(sizeof(Decimal128) == 16);

} // namespace JSC
