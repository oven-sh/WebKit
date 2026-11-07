/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "Decimal128.h"

#include <wtf/ASCIICType.h>
#include <wtf/text/StringBuilder.h>

namespace JSC {

// Large enough for any exponent that can matter, small enough that sums of a few of them do not overflow.
static constexpr int64_t saturatedExponent = 1'000'000'000'000;

static int64_t saturate(double integer)
{
    return static_cast<int64_t>(std::clamp<double>(integer, -saturatedExponent, saturatedExponent));
}

// An unsigned integer of 256 bits, in limbs of 32 so that no product needs more than 64 bits on any platform.
// No caller makes one of 2^256 or more; that is asserted. 10^77 is the greatest power of 10 that fits.
class Decimal128::Wide {
public:
    static constexpr unsigned numberOfLimbs = 8;
    static constexpr int maximumPowerOf10 = 77;

    constexpr Wide() = default;
    constexpr explicit Wide(uint64_t low, uint64_t high = 0)
        : m_limbs { static_cast<uint32_t>(low), static_cast<uint32_t>(low >> 32), static_cast<uint32_t>(high), static_cast<uint32_t>(high >> 32) }
    {
    }

    static const Wide& powerOf10(int power)
    {
        ASSERT(power >= 0 && power <= maximumPowerOf10);
        return s_powersOf10[power];
    }

    bool isZero() const
    {
        return std::ranges::all_of(m_limbs, [](uint32_t limb) { return !limb; });
    }

    bool isOdd() const { return m_limbs[0] & 1; }

    uint64_t low64() const { return m_limbs[0] | static_cast<uint64_t>(m_limbs[1]) << 32; }
    uint64_t high64() const
    {
        ASSERT(!m_limbs[4] && !m_limbs[5] && !m_limbs[6] && !m_limbs[7]);
        return m_limbs[2] | static_cast<uint64_t>(m_limbs[3]) << 32;
    }

    friend bool operator==(const Wide&, const Wide&) = default;
    friend std::strong_ordering operator<=>(const Wide& a, const Wide& b)
    {
        for (unsigned i = numberOfLimbs; i--;) {
            if (a.m_limbs[i] != b.m_limbs[i])
                return a.m_limbs[i] <=> b.m_limbs[i];
        }
        return std::strong_ordering::equal;
    }

    constexpr void multiplyAdd(uint32_t factor, uint32_t addend)
    {
        uint64_t carry = addend;
        for (auto& limb : m_limbs) {
            carry += static_cast<uint64_t>(limb) * factor;
            limb = static_cast<uint32_t>(carry);
            carry >>= 32;
        }
        ASSERT_UNDER_CONSTEXPR_CONTEXT(!carry);
    }

    // Answers the remainder.
    uint32_t divideBySmall(uint32_t divisor)
    {
        uint64_t remainder = 0;
        for (unsigned i = numberOfLimbs; i--;) {
            uint64_t dividend = remainder << 32 | m_limbs[i];
            m_limbs[i] = static_cast<uint32_t>(dividend / divisor);
            remainder = dividend % divisor;
        }
        return static_cast<uint32_t>(remainder);
    }

    friend Wide operator+(const Wide& a, const Wide& b)
    {
        Wide result;
        uint64_t carry = 0;
        for (unsigned i = 0; i < numberOfLimbs; ++i) {
            carry += static_cast<uint64_t>(a.m_limbs[i]) + b.m_limbs[i];
            result.m_limbs[i] = static_cast<uint32_t>(carry);
            carry >>= 32;
        }
        ASSERT(!carry);
        return result;
    }

    friend Wide operator-(const Wide& a, const Wide& b)
    {
        ASSERT(a >= b);
        Wide result;
        int64_t borrow = 0;
        for (unsigned i = 0; i < numberOfLimbs; ++i) {
            int64_t difference = static_cast<int64_t>(a.m_limbs[i]) - b.m_limbs[i] + borrow;
            result.m_limbs[i] = static_cast<uint32_t>(difference);
            borrow = difference < 0 ? -1 : 0;
        }
        return result;
    }

    friend Wide operator*(const Wide& a, const Wide& b)
    {
        Wide result;
        for (unsigned i = 0; i < numberOfLimbs; ++i) {
            if (!a.m_limbs[i])
                continue;
            uint64_t carry = 0;
            for (unsigned j = 0; i + j < numberOfLimbs; ++j) {
                carry += static_cast<uint64_t>(a.m_limbs[i]) * b.m_limbs[j] + result.m_limbs[i + j];
                result.m_limbs[i + j] = static_cast<uint32_t>(carry);
                carry >>= 32;
            }
            ASSERT(!carry);
        }
        return result;
    }

    struct Division;
    static Division divide(const Wide& dividend, const Wide& divisor);

    int numberOfDigits() const
    {
        unsigned limbs = usedLimbs();
        if (!limbs)
            return 0;
        // floor(bits * log10(2)) is the number of digits or one less.
        unsigned bits = limbs * 32 - std::countl_zero(m_limbs[limbs - 1]);
        int estimate = static_cast<int>(bits * 1233 >> 12);
        return estimate > maximumPowerOf10 || *this < powerOf10(estimate) ? estimate : estimate + 1;
    }

private:
    unsigned usedLimbs() const
    {
        unsigned result = numberOfLimbs;
        while (result && !m_limbs[result - 1])
            --result;
        return result;
    }

    static constexpr auto makePowersOf10();
    static const std::array<Wide, maximumPowerOf10 + 1> s_powersOf10;

    std::array<uint32_t, numberOfLimbs> m_limbs { };
};

struct Decimal128::Wide::Division {
    Wide quotient;
    Wide remainder;
};

constexpr auto Decimal128::Wide::makePowersOf10()
{
    std::array<Wide, maximumPowerOf10 + 1> result;
    result[0] = Wide { 1 };
    for (int i = 1; i <= maximumPowerOf10; ++i) {
        result[i] = result[i - 1];
        result[i].multiplyAdd(10, 0);
    }
    return result;
}

constexpr std::array<Decimal128::Wide, Decimal128::Wide::maximumPowerOf10 + 1> Decimal128::Wide::s_powersOf10 = Decimal128::Wide::makePowersOf10();

// Knuth, The Art of Computer Programming, volume 2, 4.3.1, algorithm D.
auto Decimal128::Wide::divide(const Wide& dividend, const Wide& divisor) -> Division
{
    unsigned divisorLimbs = divisor.usedLimbs();
    ASSERT(divisorLimbs);
    if (dividend < divisor)
        return { { }, dividend };
    if (divisorLimbs == 1) {
        Division result { dividend, { } };
        result.remainder = Wide { result.quotient.divideBySmall(divisor.m_limbs[0]) };
        return result;
    }

    unsigned dividendLimbs = dividend.usedLimbs();
    unsigned shift = std::countl_zero(divisor.m_limbs[divisorLimbs - 1]);
    auto shiftedLeft = [&](const Wide& value, unsigned i) {
        uint64_t limb = i < numberOfLimbs ? value.m_limbs[i] : 0;
        uint64_t below = i ? value.m_limbs[i - 1] : 0;
        return static_cast<uint32_t>((limb << 32 | below) << shift >> 32);
    };

    std::array<uint32_t, numberOfLimbs + 1> u { };
    std::array<uint32_t, numberOfLimbs> v { };
    for (unsigned i = 0; i <= dividendLimbs; ++i)
        u[i] = shiftedLeft(dividend, i);
    for (unsigned i = 0; i < divisorLimbs; ++i)
        v[i] = shiftedLeft(divisor, i);

    constexpr uint64_t base = 1ull << 32;
    Division result;
    for (unsigned j = dividendLimbs - divisorLimbs + 1; j--;) {
        uint64_t top = static_cast<uint64_t>(u[j + divisorLimbs]) << 32 | u[j + divisorLimbs - 1];
        uint64_t digit = top / v[divisorLimbs - 1];
        uint64_t rest = top % v[divisorLimbs - 1];
        while (digit >= base || digit * v[divisorLimbs - 2] > (rest << 32 | u[j + divisorLimbs - 2])) {
            --digit;
            rest += v[divisorLimbs - 1];
            if (rest >= base)
                break;
        }

        uint64_t carry = 0;
        int64_t borrow = 0;
        for (unsigned i = 0; i < divisorLimbs; ++i) {
            carry += digit * v[i];
            int64_t difference = static_cast<int64_t>(u[i + j]) - static_cast<int64_t>(carry & 0xffffffff) + borrow;
            u[i + j] = static_cast<uint32_t>(difference);
            borrow = difference < 0 ? -1 : 0;
            carry >>= 32;
        }
        int64_t difference = static_cast<int64_t>(u[j + divisorLimbs]) - static_cast<int64_t>(carry) + borrow;
        u[j + divisorLimbs] = static_cast<uint32_t>(difference);

        if (difference < 0) {
            --digit;
            uint64_t addCarry = 0;
            for (unsigned i = 0; i < divisorLimbs; ++i) {
                addCarry += static_cast<uint64_t>(u[i + j]) + v[i];
                u[i + j] = static_cast<uint32_t>(addCarry);
                addCarry >>= 32;
            }
            u[j + divisorLimbs] += static_cast<uint32_t>(addCarry);
        }
        result.quotient.m_limbs[j] = static_cast<uint32_t>(digit);
    }

    for (unsigned i = 0; i < divisorLimbs; ++i)
        result.remainder.m_limbs[i] = static_cast<uint32_t>((static_cast<uint64_t>(u[i + 1]) << 32 | u[i]) >> shift);
    return result;
}

auto Decimal128::coefficient() const -> Wide
{
    ASSERT(isFinite());
    return Wide { m_low, m_high };
}

void Decimal128::setUnrestrictedCoefficient(const Wide& value)
{
    m_low = value.low64();
    m_high = value.high64();
}

Vector<Latin1Character, 40> Decimal128::coefficientDigits() const
{
    constexpr unsigned chunkDigits = 9;
    constexpr uint32_t chunk = 1'000'000'000;
    Vector<Latin1Character, 40> digits;
    auto rest = coefficient();
    while (!rest.isZero()) {
        uint32_t low = rest.divideBySmall(chunk);
        // The chunk at the front has no zeros in front of it.
        for (unsigned i = 0; i < chunkDigits && (low || !rest.isZero()); ++i) {
            digits.append('0' + low % 10);
            low /= 10;
        }
    }
    digits.reverse();
    return digits;
}

int Decimal128::scientificExponent() const
{
    ASSERT(isFinite() && !isZero());
    return m_exponent + coefficient().numberOfDigits() - 1;
}

Decimal128 Decimal128::significand() const
{
    ASSERT(isFinite() && !isZero());
    Decimal128 result = *this;
    result.m_exponent -= scientificExponent();
    return result;
}

auto Decimal128::modeForMagnitude(bool isNegative, RoundingMode mode) -> RoundingMode
{
    if (isNegative && mode == RoundingMode::Floor)
        return RoundingMode::Ceil;
    if (isNegative && mode == RoundingMode::Ceil)
        return RoundingMode::Floor;
    return mode;
}

// https://tc39.es/proposal-decimal/#sec-decimal-applyroundingmodetopositive
// The mode is one for a magnitude: see modeForMagnitude().
auto Decimal128::roundedQuotientByPowerOf10(const Wide& value, int64_t power, bool isInexact, RoundingMode mode) -> Wide
{
    ASSERT(power > 0);
    Wide quotient;
    auto fractionToHalf = std::strong_ordering::less;
    bool hasFraction = isInexact || !value.isZero();
    // A value of fewer digits than the power is less than a tenth.
    if (power <= value.numberOfDigits()) {
        const Wide& divisor = Wide::powerOf10(static_cast<int>(power));
        auto division = Wide::divide(value, divisor);
        quotient = division.quotient;
        hasFraction = isInexact || !division.remainder.isZero();
        // The divisor is even, so half of it is a whole number.
        Wide half = divisor;
        half.divideBySmall(2);
        fractionToHalf = division.remainder <=> half;
        if (isInexact && fractionToHalf == std::strong_ordering::equal)
            fractionToHalf = std::strong_ordering::greater;
    }
    if (!hasFraction)
        return quotient;

    bool isUp = false;
    switch (mode) {
    case RoundingMode::Floor:
    case RoundingMode::Trunc:
        break;
    case RoundingMode::Ceil:
        isUp = true;
        break;
    case RoundingMode::HalfExpand:
        isUp = fractionToHalf != std::strong_ordering::less;
        break;
    case RoundingMode::HalfEven:
        isUp = fractionToHalf == std::strong_ordering::greater || (fractionToHalf == std::strong_ordering::equal && quotient.isOdd());
        break;
    }
    if (isUp)
        quotient = quotient + Wide { 1 };
    return quotient;
}

// https://tc39.es/proposal-decimal/#sec-decimal-roundtodecimaldomain
Decimal128 Decimal128::roundToDomain(bool isNegative, Wide value, int64_t exponent, bool isInexact)
{
    // The spec answers +0 for the mathematical value 0. A caller that wants -0 makes it.
    if (value.isZero() && !isInexact)
        return zero(false);

    int digits = value.numberOfDigits();
    ASSERT(!isInexact || digits > precision);
    int64_t quantum = std::max<int64_t>(exponent + digits - precision, minimumQuantumExponent);
    if (quantum > exponent) {
        value = roundedQuotientByPowerOf10(value, quantum - exponent, isInexact, RoundingMode::HalfEven);
        exponent = quantum;
        if (value == Wide::powerOf10(precision)) {
            value = Wide::powerOf10(precision - 1);
            ++quantum;
            ++exponent;
        }
    }
    // A shorter coefficient has its quantum below its exponent, and it is the quantum that is limited.
    if (quantum > maximumQuantumExponent)
        return infinity(isNegative);
    if (value.isZero())
        return zero(isNegative);

    for (auto [divisor, power] : { std::pair<uint32_t, int> { 100'000'000, 8 }, { 10'000, 4 }, { 100, 2 }, { 10, 1 } }) {
        while (true) {
            Wide quotient = value;
            if (quotient.divideBySmall(divisor))
                break;
            value = quotient;
            exponent += power;
        }
    }

    Decimal128 result { Kind::Finite, isNegative };
    result.m_low = value.low64();
    result.m_high = value.high64();
    result.m_exponent = static_cast<int16_t>(exponent);
    ASSERT(result.m_exponent == exponent);
    return result;
}

std::optional<Decimal128> Decimal128::parse(StringView text)
{
    if (text == "NaN"_s)
        return nan();

    unsigned length = text.length();
    unsigned position = 0;
    bool isNegative = false;
    if (position < length && (text[position] == '-' || text[position] == '+'))
        isNegative = text[position++] == '-';

    if (text.substring(position) == "Infinity"_s)
        return infinity(isNegative);

    // More digits than any rounding looks at one by one. Of those that follow it only matters whether one is not zero.
    constexpr int keptDigits = 2 * precision;
    Wide value;
    int numberOfKept = 0;
    bool isInexact = false;
    int64_t exponent = 0;
    bool hasDigits = false;
    auto parseDigits = [&](bool isFraction) {
        for (; position < length && isASCIIDigit(text[position]); ++position) {
            hasDigits = true;
            uint32_t digit = text[position] - '0';
            if (numberOfKept == keptDigits) {
                isInexact |= !!digit;
                if (!isFraction)
                    ++exponent;
                continue;
            }
            if (isFraction)
                --exponent;
            if (!numberOfKept && !digit)
                continue;
            value.multiplyAdd(10, digit);
            ++numberOfKept;
        }
    };

    parseDigits(false);
    if (position < length && text[position] == '.') {
        ++position;
        parseDigits(true);
    }
    if (!hasDigits)
        return std::nullopt;

    if (position < length && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        bool isNegativeExponent = false;
        if (position < length && (text[position] == '-' || text[position] == '+'))
            isNegativeExponent = text[position++] == '-';
        if (position == length)
            return std::nullopt;
        int64_t written = 0;
        for (; position < length && isASCIIDigit(text[position]); ++position)
            written = std::min<int64_t>(written * 10 + (text[position] - '0'), saturatedExponent);
        exponent += isNegativeExponent ? -written : written;
    }
    if (position != length)
        return std::nullopt;

    if (value.isZero())
        return zero(isNegative);
    return roundToDomain(isNegative, value, exponent, isInexact);
}

// https://tc39.es/proposal-decimal/#sec-decimal-abs
Decimal128 Decimal128::abs() const
{
    Decimal128 result = *this;
    result.m_isNegative = false;
    return result;
}

// https://tc39.es/proposal-decimal/#sec-decimal-negate
Decimal128 Decimal128::negate() const
{
    Decimal128 result = *this;
    result.m_isNegative = !isNaN() && !m_isNegative;
    return result;
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.add
Decimal128 Decimal128::add(const Decimal128& x, const Decimal128& y)
{
    if (x.isNaN() || y.isNaN())
        return nan();
    if (x.isInfinity())
        return y.isInfinity() && x.m_isNegative != y.m_isNegative ? nan() : x;
    if (y.isInfinity())
        return y;
    if (x.isZero())
        return y.isZero() ? zero(x.m_isNegative && y.m_isNegative) : y;
    if (y.isZero())
        return x;

    const Decimal128& upper = x.m_exponent >= y.m_exponent ? x : y;
    const Decimal128& lower = x.m_exponent >= y.m_exponent ? y : x;
    Wide upperValue = upper.coefficient();
    Wide lowerValue = lower.coefficient();

    // 10^76 + 10^34 is less than 2^256, so the sum is exact when the upper value has at most this many digits in units of the lower.
    constexpr int maximumDigits = Wide::maximumPowerOf10 - 1;
    int difference = upper.m_exponent - lower.m_exponent;
    int spare = maximumDigits - upperValue.numberOfDigits();
    if (difference <= spare) {
        upperValue = upperValue * Wide::powerOf10(difference);
        if (upper.m_isNegative == lower.m_isNegative)
            return roundToDomain(upper.m_isNegative, upperValue + lowerValue, lower.m_exponent);
        if (upperValue >= lowerValue)
            return roundToDomain(upper.m_isNegative, upperValue - lowerValue, lower.m_exponent);
        return roundToDomain(lower.m_isNegative, lowerValue - upperValue, lower.m_exponent);
    }

    // The lower value is less than 10^-42 of the upper one, so the upper one is the nearest value.
    return upper;
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.subtract
Decimal128 Decimal128::subtract(const Decimal128& x, const Decimal128& y)
{
    return add(x, y.negate());
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.multiply
Decimal128 Decimal128::multiply(const Decimal128& x, const Decimal128& y)
{
    if (x.isNaN() || y.isNaN())
        return nan();
    bool isNegative = x.m_isNegative != y.m_isNegative;
    if (x.isInfinity() || y.isInfinity())
        return x.isZero() || y.isZero() ? nan() : infinity(isNegative);
    if (x.isZero() || y.isZero())
        return zero(isNegative);
    return roundToDomain(isNegative, x.coefficient() * y.coefficient(), x.m_exponent + y.m_exponent);
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.divide
Decimal128 Decimal128::divide(const Decimal128& x, const Decimal128& y)
{
    if (x.isNaN() || y.isNaN())
        return nan();
    bool isNegative = x.m_isNegative != y.m_isNegative;
    if (x.isInfinity())
        return y.isInfinity() ? nan() : infinity(isNegative);
    if (y.isInfinity())
        return zero(isNegative);
    if (y.isZero())
        return x.isZero() ? nan() : infinity(isNegative);
    if (x.isZero())
        return zero(isNegative);

    // With both coefficients 34 digits long the quotient of 35 more has 35 or 36 digits, which is enough for a correct rounding.
    Wide dividend = x.coefficient();
    Wide divisor = y.coefficient();
    int dividendPadding = precision + 1 + precision - dividend.numberOfDigits();
    int divisorPadding = precision - divisor.numberOfDigits();
    auto division = Wide::divide(dividend * Wide::powerOf10(dividendPadding), divisor * Wide::powerOf10(divisorPadding));
    return roundToDomain(isNegative, division.quotient, x.m_exponent - dividendPadding - y.m_exponent + divisorPadding, !division.remainder.isZero());
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.remainder
Decimal128 Decimal128::remainder(const Decimal128& x, const Decimal128& y)
{
    if (x.isNaN() || y.isNaN() || x.isInfinity() || y.isZero())
        return nan();
    if (y.isInfinity() || x.isZero())
        return x;

    // The result is a multiple of the smaller quantum and less than both operands, so it needs no rounding.
    Wide value = x.coefficient();
    Wide modulus = y.coefficient();
    int exponent = y.m_exponent;
    if (int power = x.m_exponent - y.m_exponent; power >= 0) {
        // value * 10^power mod modulus. Each factor is less than 10^34 here.
        auto modulo = [&](const Wide& dividend) {
            return Wide::divide(dividend, modulus).remainder;
        };
        value = modulo(value);
        Wide base = modulo(Wide { 10 });
        for (; power; power >>= 1) {
            if (power & 1)
                value = modulo(value * base);
            base = modulo(base * base);
        }
    } else {
        exponent = x.m_exponent;
        if (-power >= precision)
            return x;
        value = Wide::divide(value, modulus * Wide::powerOf10(-power)).remainder;
    }
    if (value.isZero())
        return zero(x.m_isNegative);
    return roundToDomain(x.m_isNegative, value, exponent);
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.compare
std::partial_ordering Decimal128::compare(const Decimal128& x, const Decimal128& y)
{
    if (x.isNaN() || y.isNaN())
        return std::partial_ordering::unordered;
    auto signum = [](const Decimal128& value) {
        return value.isZero() ? 0 : value.m_isNegative ? -1 : 1;
    };
    int sign = signum(x);
    if (int other = signum(y); sign != other || !sign)
        return sign <=> other;

    auto compareMagnitudes = [](const Decimal128& x, const Decimal128& y) -> std::strong_ordering {
        if (x.isInfinity() || y.isInfinity())
            return x.isInfinity() <=> y.isInfinity();
        if (x.m_exponent == y.m_exponent)
            return x.coefficient() <=> y.coefficient();
        if (auto order = x.scientificExponent() <=> y.scientificExponent(); order != 0)
            return order;
        // The same scientific exponent: the quanta are at most 33 apart.
        Wide xValue = x.coefficient();
        Wide yValue = y.coefficient();
        if (x.m_exponent > y.m_exponent)
            xValue = xValue * Wide::powerOf10(x.m_exponent - y.m_exponent);
        else
            yValue = yValue * Wide::powerOf10(y.m_exponent - x.m_exponent);
        return xValue <=> yValue;
    };
    return sign > 0 ? compareMagnitudes(x, y) : compareMagnitudes(y, x);
}

auto Decimal128::magnitudeInUnitsOf(int64_t exponent, RoundingMode mode) const -> Wide
{
    ASSERT(m_exponent < exponent);
    return roundedQuotientByPowerOf10(coefficient(), exponent - m_exponent, false, modeForMagnitude(m_isNegative, mode));
}

// https://tc39.es/proposal-decimal/#sec-decimal-method-round
Decimal128 Decimal128::round(double numberOfFractionalDigits, RoundingMode mode) const
{
    if (!isFinite() || isZero())
        return *this;
    int64_t unit = saturate(-numberOfFractionalDigits);
    if (unit <= m_exponent)
        return *this;
    // The spec rounds to the domain in the same mode, but nothing is left to round: the count is at most 10^34, and the unit is in the domain or overflows.
    return roundToDomain(m_isNegative, magnitudeInUnitsOf(unit, mode), unit);
}

// https://tc39.es/proposal-decimal/#sec-decimal-method-scale10
Decimal128 Decimal128::scale10(double power) const
{
    if (!isFinite() || isZero())
        return *this;
    if (std::isnan(power))
        return nan();
    if (std::isinf(power))
        return power > 0 ? infinity(m_isNegative) : zero(m_isNegative);
    return roundToDomain(m_isNegative, coefficient(), m_exponent + saturate(power));
}

static void appendZeros(StringBuilder& builder, int64_t count)
{
    constexpr ASCIILiteral zeros = "0000000000000000000000000000000000000000000000000000000000000000"_s;
    for (; count > 0; count -= zeros.length())
        builder.append(StringView(zeros).left(static_cast<unsigned>(std::min<int64_t>(count, zeros.length()))));
}

// Writes digits * 10^exponent with so many digits behind the point, which are at least as many as it has.
static void appendPlain(StringBuilder& builder, std::span<const Latin1Character> digits, int64_t exponent, int64_t fractionalDigits)
{
    ASSERT(fractionalDigits >= 0 && (digits.empty() || fractionalDigits >= -exponent));
    int64_t size = static_cast<int64_t>(digits.size());
    int64_t beforePoint = std::clamp<int64_t>(size + exponent, 0, size);
    if (!beforePoint)
        builder.append('0');
    builder.append(digits.first(static_cast<size_t>(beforePoint)));
    if (beforePoint)
        appendZeros(builder, exponent);
    if (!fractionalDigits)
        return;
    builder.append('.');
    int64_t zerosInFront = size && exponent < -size ? -exponent - size : 0;
    appendZeros(builder, zerosInFront);
    builder.append(digits.subspan(static_cast<size_t>(beforePoint)));
    appendZeros(builder, fractionalDigits - zerosInFront - (size - beforePoint));
}

// Writes at least so many digits.
static void appendExponential(StringBuilder& builder, std::span<const Latin1Character> digits, int exponent, int64_t numberOfDigits)
{
    builder.append(digits.first(1));
    if (numberOfDigits > 1) {
        builder.append('.');
        builder.append(digits.subspan(1));
        appendZeros(builder, numberOfDigits - static_cast<int64_t>(digits.size()));
    }
    // Only zero, 0e+0, is written with a plus sign.
    builder.append('e', exponent);
}

static String nonFiniteString(const Decimal128& value)
{
    if (value.isNaN())
        return "NaN"_s;
    return value.isNegative() ? "-Infinity"_s : "Infinity"_s;
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.tostring
String Decimal128::toString() const
{
    if (!isFinite())
        return nonFiniteString(*this);
    if (isZero())
        return m_isNegative ? "-0"_s : "0"_s;

    int exponent = scientificExponent();
    if (exponent < -6 || exponent >= precision)
        return toExponentialString();

    StringBuilder builder;
    if (m_isNegative)
        builder.append('-');
    appendPlain(builder, coefficientDigits().span(), m_exponent, std::max<int>(-m_exponent, 0));
    return builder.toString();
}

// https://tc39.es/proposal-decimal/#sec-decimaltoexponentialstring
String Decimal128::toExponentialString() const
{
    if (!isFinite())
        return nonFiniteString(*this);
    if (isZero())
        return m_isNegative ? "-0e+0"_s : "0e+0"_s;
    StringBuilder builder;
    if (m_isNegative)
        builder.append('-');
    auto digits = coefficientDigits();
    appendExponential(builder, digits.span(), scientificExponent(), digits.size());
    return builder.toString();
}

String Decimal128::toIntegerString() const
{
    ASSERT(isInteger());
    if (isZero())
        return "0"_s;
    StringBuilder builder;
    if (m_isNegative)
        builder.append('-');
    appendPlain(builder, coefficientDigits().span(), m_exponent, 0);
    return builder.toString();
}

// Beside the digits asked for there are at most 6145 in front of the point, the point, the sign and the exponent.
static constexpr double maximumDigitsAskedFor = String::MaxLength - 6200;

static String toStringUnlessOverflowed(StringBuilder& builder)
{
    if (builder.hasOverflowed())
        return { };
    return builder.toString();
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.tofixed
String Decimal128::toFixed(double numberOfDigits, RoundingMode mode) const
{
    ASSERT(numberOfDigits >= 0);
    if (!isFinite())
        return nonFiniteString(*this);
    if (numberOfDigits > maximumDigitsAskedFor)
        return { };
    int fractionalDigits = static_cast<int>(numberOfDigits);

    Decimal128 rounded = *this;
    int exponent = m_exponent;
    if (!isZero() && m_exponent < -fractionalDigits) {
        exponent = -fractionalDigits;
        rounded.setUnrestrictedCoefficient(magnitudeInUnitsOf(exponent, mode));
    }

    StringBuilder builder(OverflowPolicy::RecordOverflow);
    // The sign stays when all digits are rounded away, as with Number.prototype.toFixed.
    if (m_isNegative)
        builder.append('-');
    appendPlain(builder, rounded.coefficientDigits().span(), exponent, fractionalDigits);
    return toStringUnlessOverflowed(builder);
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.toprecision
String Decimal128::toPrecision(double numberOfDigits, RoundingMode mode) const
{
    ASSERT(numberOfDigits >= 1);
    if (!isFinite())
        return nonFiniteString(*this);
    if (numberOfDigits > maximumDigitsAskedFor)
        return { };
    int wanted = static_cast<int>(numberOfDigits);

    StringBuilder builder(OverflowPolicy::RecordOverflow);
    if (isZero()) {
        // The steps have the sign with one digit only, and as many zeros behind the point as there are digits.
        if (wanted == 1)
            return m_isNegative ? "-0"_s : "0"_s;
        appendPlain(builder, { }, 0, wanted);
        return toStringUnlessOverflowed(builder);
    }

    if (m_isNegative)
        builder.append('-');
    int numberOfCoefficientDigits = coefficient().numberOfDigits();
    if (numberOfCoefficientDigits < wanted && !isInteger()) {
        // The steps use decimal notation whatever the exponent is.
        appendPlain(builder, coefficientDigits().span(), m_exponent, static_cast<int64_t>(wanted) - numberOfCoefficientDigits - m_exponent);
        return toStringUnlessOverflowed(builder);
    }

    // The steps for the other values do not give the value (1.25 is "125" and 1200 is "12.0"). These are the digits and the choice of
    // notation of Number.prototype.toPrecision, to which the proposal says that this is similar.
    int scientific = scientificExponent();
    int exponent = m_exponent;
    Decimal128 rounded = *this;
    if (int excess = numberOfCoefficientDigits - wanted; excess > 0) {
        exponent += excess;
        Wide magnitude = magnitudeInUnitsOf(exponent, mode);
        if (magnitude == Wide::powerOf10(wanted)) {
            magnitude = Wide::powerOf10(wanted - 1);
            ++exponent;
            ++scientific;
        }
        rounded.setUnrestrictedCoefficient(magnitude);
    }
    auto digits = rounded.coefficientDigits();
    if (scientific < -6 || scientific >= wanted)
        appendExponential(builder, digits.span(), scientific, wanted);
    else
        appendPlain(builder, digits.span(), exponent, wanted - 1 - scientific);
    return toStringUnlessOverflowed(builder);
}

} // namespace JSC
