/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "Decimal128.h"

#include <wtf/ASCIICType.h>
#include <wtf/FastFloat.h>
#include <wtf/Int128.h>
#include <wtf/dtoa.h>
#include <wtf/text/StringBuilder.h>

namespace JSC {

// Large enough for any exponent that can matter, small enough that sums of a few of them do not overflow.
static constexpr int64_t saturatedExponent = 1'000'000'000'000;

static int64_t saturate(double integer)
{
    return static_cast<int64_t>(std::clamp<double>(integer, -saturatedExponent, saturatedExponent));
}

static ALWAYS_INLINE constexpr UInt128 makeUInt128(uint64_t high, uint64_t low)
{
    return static_cast<UInt128>(high) << 64 | low;
}

static ALWAYS_INLINE constexpr uint64_t lowOf(UInt128 value) { return static_cast<uint64_t>(value); }
static ALWAYS_INLINE constexpr uint64_t highOf(UInt128 value) { return static_cast<uint64_t>(value >> 64); }

// The product of two 64-bit integers. UInt128 multiplies 128 bits by 128 where it is not a type of the compiler.
static ALWAYS_INLINE constexpr UInt128 multiply64(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    return makeUInt128(static_cast<uint64_t>(product >> 64), static_cast<uint64_t>(product));
#else
    return static_cast<UInt128>(a) * static_cast<UInt128>(b);
#endif
}

struct DecimalDivision64 {
    uint64_t quotient;
    uint64_t remainder;
};

// (high * 2^64 + low) / divisor, where high < divisor, so that the quotient fits. A division of UInt128 is several times slower.
static ALWAYS_INLINE DecimalDivision64 divide128By64(uint64_t high, uint64_t low, uint64_t divisor)
{
    ASSERT(high < divisor);
#if CPU(X86_64)
    uint64_t quotient;
    uint64_t remainder;
    asm("divq %[divisor]" : "=a"(quotient), "=d"(remainder) : [divisor] "r"(divisor), "a"(low), "d"(high) : "cc");
    return { quotient, remainder };
#else
    if (!high)
        return { low / divisor, low % divisor };
    // Two steps of Knuth's algorithm D with digits of 32 bits.
    unsigned shift = std::countl_zero(divisor);
    divisor <<= shift;
    if (shift)
        high = high << shift | low >> (64 - shift);
    low <<= shift;
    uint64_t divisorHigh = divisor >> 32;
    uint64_t divisorLow = divisor & 0xffffffff;
    auto step = [&](uint64_t rest, uint64_t next) -> DecimalDivision64 {
        uint64_t digit = rest / divisorHigh;
        uint64_t partial = rest % divisorHigh;
        while (digit >> 32 || digit * divisorLow > (partial << 32 | next)) {
            --digit;
            partial += divisorHigh;
            if (partial >> 32)
                break;
        }
        return { digit, (rest << 32 | next) - digit * divisor };
    };
    auto upper = step(high, low >> 32);
    auto lower = step(upper.remainder, low & 0xffffffff);
    return { upper.quotient << 32 | lower.quotient, lower.remainder >> shift };
#endif
}

static constexpr auto powersOf10In64Bits = [] {
    std::array<uint64_t, 20> result { };
    uint64_t power = 1;
    for (auto& element : result) {
        element = power;
        power *= 10;
    }
    return result;
}();

#if !CPU(X86_64)

// For a division by 10^i without a division: Moller and Granlund, Improved Division by Invariant Integers (2011), algorithm 4.
struct InversePowerOf10 {
    uint64_t divisor; // 10^i, shifted so that its upper bit is set
    uint64_t reciprocal; // (2^128 - 1) / divisor - 2^64
    unsigned shift;
};

static constexpr auto inversePowersOf10 = [] {
    std::array<InversePowerOf10, 20> result { };
    for (unsigned i = 0; i < result.size(); ++i) {
        unsigned shift = std::countl_zero(powersOf10In64Bits[i]);
        uint64_t divisor = powersOf10In64Bits[i] << shift;
        // By bits, for where UInt128 has no constexpr division.
        uint64_t remainder = ~divisor;
        uint64_t quotient = 0;
        for (int bit = 0; bit < 64; ++bit) {
            bool carry = remainder >> 63;
            remainder = remainder << 1 | 1;
            quotient <<= 1;
            if (carry || remainder >= divisor) {
                remainder -= divisor;
                quotient |= 1;
            }
        }
        result[i] = { divisor, quotient, shift };
    }
    return result;
}();

#endif // !CPU(X86_64)

// (high * 2^64 + low) / 10^power, where high is less than 10^power.
static ALWAYS_INLINE DecimalDivision64 divide128ByPowerOf10(uint64_t high, uint64_t low, unsigned power)
{
    ASSERT(high < powersOf10In64Bits[power]);
#if CPU(X86_64)
    return divide128By64(high, low, powersOf10In64Bits[power]);
#else
    auto& inverse = inversePowersOf10[power];
    high = high << inverse.shift | (inverse.shift ? low >> (64 - inverse.shift) : 0);
    low <<= inverse.shift;
    UInt128 estimate = multiply64(inverse.reciprocal, high) + makeUInt128(high, low);
    uint64_t quotient = highOf(estimate) + 1;
    uint64_t remainder = low - quotient * inverse.divisor;
    if (remainder > lowOf(estimate)) {
        --quotient;
        remainder += inverse.divisor;
    }
    if (remainder >= inverse.divisor) [[unlikely]] {
        ++quotient;
        remainder -= inverse.divisor;
    }
    return { quotient, remainder >> inverse.shift };
#endif
}

static constexpr auto powersOf10In128Bits = [] {
    std::array<UInt128, 39> result { };
    UInt128 power = 1;
    for (auto& element : result) {
        element = power;
        power = power * 10;
    }
    return result;
}();

// floor(bits * log10(2)) is the number of digits or one less. Exact up to 256 bits.
static ALWAYS_INLINE int leastNumberOfDigits(unsigned bits)
{
    return static_cast<int>(bits * 1233 >> 12);
}

static ALWAYS_INLINE int numberOfDigits(UInt128 value)
{
    ASSERT(!!value);
    uint64_t high = highOf(value);
    if (!high) {
        int estimate = leastNumberOfDigits(64 - std::countl_zero(lowOf(value)));
        return estimate + (lowOf(value) >= powersOf10In64Bits[estimate]);
    }
    int estimate = leastNumberOfDigits(128 - std::countl_zero(high));
    return estimate + (value >= powersOf10In128Bits[estimate]);
}

struct DecimalDivision128 {
    UInt128 quotient;
    uint64_t remainder;
};

// 10^i is 2^i * 5^i. The reciprocal of 5^i is too large by less than 2^-(64 + shift), which a dividend below 2^63 makes less than 5^-i.
alignas(sizeof(Decimal128::Divisor)) static constexpr auto divisorsOf64Bits = [] {
    std::array<Decimal128::Divisor, Decimal128::greatestPowerOfDivisors + 1> result { };
    for (unsigned i = 1; i < result.size(); ++i) {
        uint64_t powerOf5 = powersOf10In64Bits[i] >> i;
        unsigned shift = 63 - std::countl_zero(powerOf5);
        result[i] = { powersOf10In64Bits[i], powersOf10In64Bits[i] / 2, lowOf((makeUInt128(1, 0) << shift) / powerOf5) + 1, shift };
    }
    return result;
}();

static ALWAYS_INLINE DecimalDivision128 divideByShortPowerOf10(UInt128 value, unsigned power)
{
    ASSERT(power);
    uint64_t divisor = powersOf10In64Bits[power];
    uint64_t high = highOf(value);
    if (!high) {
        uint64_t quotient = highOf(multiply64(lowOf(value) >> power, divisorsOf64Bits[power].reciprocal)) >> divisorsOf64Bits[power].shift;
        return { quotient, lowOf(value) - quotient * divisor };
    }
    uint64_t quotientHigh = 0;
    if (high >= divisor) {
        quotientHigh = high / divisor;
        high %= divisor;
    }
    auto division = divide128ByPowerOf10(high, lowOf(value), power);
    return { makeUInt128(quotientHigh, division.quotient), division.remainder };
}

struct DecimalQuotient {
    UInt128 quotient;
    std::strong_ordering fractionToHalf;
    bool hasFraction;
};

static constexpr int maximumShortPower = 38;

static ALWAYS_INLINE DecimalQuotient divideByPowerOf10(UInt128 value, int power)
{
    ASSERT(power > 0 && power <= maximumShortPower);
    constexpr int step = 19;
    if (power <= step) {
        uint64_t divisor = powersOf10In64Bits[power];
        auto division = divideByShortPowerOf10(value, power);
        return { division.quotient, division.remainder <=> divisor / 2, !!division.remainder };
    }
    auto lower = divideByShortPowerOf10(value, step);
    auto upper = divideByShortPowerOf10(lower.quotient, power - step);
    UInt128 remainder = multiply64(upper.remainder, powersOf10In64Bits[step]) + lower.remainder;
    UInt128 half = powersOf10In128Bits[power] >> 1;
    auto fractionToHalf = remainder < half ? std::strong_ordering::less : remainder == half ? std::strong_ordering::equal : std::strong_ordering::greater;
    return { upper.quotient, fractionToHalf, !!remainder };
}

// The mode is one for a magnitude.
static ALWAYS_INLINE bool roundsUp(Decimal128::RoundingMode mode, bool hasFraction, std::strong_ordering fractionToHalf, bool isOdd)
{
    if (!hasFraction)
        return false;
    switch (mode) {
    case Decimal128::RoundingMode::Floor:
    case Decimal128::RoundingMode::Trunc:
        return false;
    case Decimal128::RoundingMode::Ceil:
        return true;
    case Decimal128::RoundingMode::HalfExpand:
        return fractionToHalf != std::strong_ordering::less;
    case Decimal128::RoundingMode::HalfEven:
        return fractionToHalf == std::strong_ordering::greater || (fractionToHalf == std::strong_ordering::equal && isOdd);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Both remove the zeros at the end of a value that is not 0, and answer how many. A multiple of 10^n is one of 2^n, which costs less to find out.
static ALWAYS_INLINE int stripTrailingZeros(uint64_t& value)
{
    if (value & 1)
        return 0;
    int count = 0;
    auto strip = [&](uint64_t divisor, int power) ALWAYS_INLINE_LAMBDA {
        if (value & ((1ull << power) - 1))
            return;
        uint64_t quotient = value / divisor;
        if (quotient * divisor != value)
            return;
        value = quotient;
        count += power;
    };
    strip(10'000'000'000'000'000, 16);
    strip(100'000'000, 8);
    strip(10'000, 4);
    strip(100, 2);
    strip(10, 1);
    return count;
}

static NEVER_INLINE int stripTrailingZerosOfEven(UInt128& value)
{
    int count = 0;
    // A value of 39 digits has at most 38.
    for (int power : { 16, 16, 8, 4, 2, 1 }) {
        if (!highOf(value) || lowOf(value) & ((1ull << power) - 1))
            continue;
        auto division = divideByShortPowerOf10(value, power);
        if (division.remainder)
            continue;
        value = division.quotient;
        count += power;
    }
    if (highOf(value))
        return count;
    uint64_t low = lowOf(value);
    count += stripTrailingZeros(low);
    value = low;
    return count;
}

// An unsigned integer of 256 bits. No caller makes one of 2^256 or more; that is asserted. 10^77 is the greatest power of 10 that fits.
class Decimal128::Wide {
public:
    static constexpr unsigned numberOfLimbs = 4;
    static constexpr int maximumPowerOf10 = 77;

    constexpr Wide() = default;
    constexpr explicit Wide(uint64_t low, uint64_t high = 0)
        : m_limbs { low, high }
    {
    }

    static ALWAYS_INLINE Wide from(UInt128 value) { return Wide { lowOf(value), highOf(value) }; }

    static const Wide& powerOf10(int power)
    {
        ASSERT(power >= 0 && power <= maximumPowerOf10);
        return s_powersOf10[power];
    }

    bool isZero() const { return !(m_limbs[0] | m_limbs[1] | m_limbs[2] | m_limbs[3]); }
    bool isOdd() const { return m_limbs[0] & 1; }
    bool fitsIn128Bits() const { return !(m_limbs[2] | m_limbs[3]); }

    UInt128 low128() const
    {
        ASSERT(fitsIn128Bits());
        return makeUInt128(m_limbs[1], m_limbs[0]);
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

    constexpr void multiplyAdd(uint64_t factor, uint64_t addend)
    {
        UInt128 carry = addend;
        for (auto& limb : m_limbs) {
            carry = carry + multiply64(limb, factor);
            limb = lowOf(carry);
            carry = carry >> 64;
        }
        ASSERT_UNDER_CONSTEXPR_CONTEXT(!carry);
    }

    // Answers the remainder.
    ALWAYS_INLINE uint64_t divideByPowerOf10(unsigned power)
    {
        uint64_t remainder = 0;
        for (unsigned i = usedLimbs(); i--;) {
            auto division = divide128ByPowerOf10(remainder, m_limbs[i], power);
            m_limbs[i] = division.quotient;
            remainder = division.remainder;
        }
        return remainder;
    }

    Wide half() const
    {
        Wide result;
        for (unsigned i = 0; i < numberOfLimbs; ++i)
            result.m_limbs[i] = m_limbs[i] >> 1 | (i + 1 < numberOfLimbs ? m_limbs[i + 1] << 63 : 0);
        return result;
    }

    friend Wide operator+(const Wide& a, const Wide& b)
    {
        Wide result;
        UInt128 carry = 0;
        for (unsigned i = 0; i < numberOfLimbs; ++i) {
            carry = carry + a.m_limbs[i] + b.m_limbs[i];
            result.m_limbs[i] = lowOf(carry);
            carry = carry >> 64;
        }
        ASSERT(!carry);
        return result;
    }

    friend Wide operator-(const Wide& a, const Wide& b)
    {
        ASSERT(a >= b);
        Wide result;
        uint64_t borrow = 0;
        for (unsigned i = 0; i < numberOfLimbs; ++i) {
            uint64_t difference = a.m_limbs[i] - b.m_limbs[i];
            uint64_t nextBorrow = a.m_limbs[i] < b.m_limbs[i];
            nextBorrow |= difference < borrow;
            result.m_limbs[i] = difference - borrow;
            borrow = nextBorrow;
        }
        return result;
    }

    friend Wide operator*(const Wide& a, const Wide& b)
    {
        Wide result;
        unsigned limbsOfB = b.usedLimbs();
        for (unsigned i = 0; i < numberOfLimbs; ++i) {
            if (!a.m_limbs[i])
                continue;
            ASSERT(i + limbsOfB <= numberOfLimbs);
            uint64_t carry = 0;
            unsigned j = 0;
            for (; j < limbsOfB && i + j < numberOfLimbs; ++j) {
                UInt128 sum = multiply64(a.m_limbs[i], b.m_limbs[j]) + result.m_limbs[i + j] + carry;
                result.m_limbs[i + j] = lowOf(sum);
                carry = highOf(sum);
            }
            if (i + j < numberOfLimbs)
                result.m_limbs[i + j] = carry;
            else
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
        int estimate = leastNumberOfDigits(limbs * 64 - std::countl_zero(m_limbs[limbs - 1]));
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

    std::array<uint64_t, numberOfLimbs> m_limbs { };
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
    unsigned dividendLimbs = dividend.usedLimbs();
    if (divisorLimbs == 1) {
        Division result;
        uint64_t remainder = 0;
        for (unsigned i = dividendLimbs; i--;) {
            auto division = divide128By64(remainder, dividend.m_limbs[i], divisor.m_limbs[0]);
            result.quotient.m_limbs[i] = division.quotient;
            remainder = division.remainder;
        }
        result.remainder = Wide { remainder };
        return result;
    }
    if (dividend < divisor)
        return { { }, dividend };

    unsigned shift = std::countl_zero(divisor.m_limbs[divisorLimbs - 1]);
    auto shiftedLeft = [&](const Wide& value, unsigned i) {
        uint64_t limb = i < numberOfLimbs ? value.m_limbs[i] : 0;
        uint64_t below = i && shift ? value.m_limbs[i - 1] >> (64 - shift) : 0;
        return limb << shift | below;
    };

    std::array<uint64_t, numberOfLimbs + 1> u { };
    std::array<uint64_t, numberOfLimbs> v { };
    for (unsigned i = 0; i <= dividendLimbs; ++i)
        u[i] = shiftedLeft(dividend, i);
    for (unsigned i = 0; i < divisorLimbs; ++i)
        v[i] = shiftedLeft(divisor, i);
    uint64_t vTop = v[divisorLimbs - 1];
    uint64_t vNext = v[divisorLimbs - 2];

    Division result;
    for (unsigned j = dividendLimbs - divisorLimbs + 1; j--;) {
        uint64_t top = u[j + divisorLimbs];
        uint64_t next = u[j + divisorLimbs - 1];
        uint64_t digit;
        uint64_t rest;
        bool restFits = true;
        // The digit is at most one more than 2^64 - 1 here.
        if (top >= vTop) {
            ASSERT(top == vTop);
            digit = std::numeric_limits<uint64_t>::max();
            rest = next + vTop;
            restFits = rest >= vTop;
        } else {
            auto division = divide128By64(top, next, vTop);
            digit = division.quotient;
            rest = division.remainder;
        }
        while (restFits && multiply64(digit, vNext) > makeUInt128(rest, u[j + divisorLimbs - 2])) {
            --digit;
            rest += vTop;
            restFits = rest >= vTop;
        }

        uint64_t carry = 0;
        for (unsigned i = 0; i < divisorLimbs; ++i) {
            UInt128 product = multiply64(digit, v[i]) + carry;
            uint64_t low = lowOf(product);
            carry = highOf(product) + (u[i + j] < low);
            u[i + j] -= low;
        }
        bool isNegative = u[j + divisorLimbs] < carry;
        u[j + divisorLimbs] -= carry;

        if (isNegative) {
            --digit;
            UInt128 addCarry = 0;
            for (unsigned i = 0; i < divisorLimbs; ++i) {
                addCarry = addCarry + u[i + j] + v[i];
                u[i + j] = lowOf(addCarry);
                addCarry = addCarry >> 64;
            }
            u[j + divisorLimbs] += lowOf(addCarry);
        }
        result.quotient.m_limbs[j] = digit;
    }

    for (unsigned i = 0; i < divisorLimbs; ++i)
        result.remainder.m_limbs[i] = shift ? u[i] >> shift | u[i + 1] << (64 - shift) : u[i];
    return result;
}

// The value high:low * 10^exponent, which is in the domain and not 0.
ALWAYS_INLINE Decimal128 Decimal128::fromExact(bool isNegative, uint64_t high, uint64_t low, int64_t exponent)
{
    ASSERT(high | low);
    Decimal128 result;
    result.m_isNegative = isNegative;
    if (!high) {
        exponent += stripTrailingZeros(low);
        result.m_low = low;
    } else {
        UInt128 value = makeUInt128(high, low);
        ASSERT(value < powersOf10In128Bits[precision]);
        if (!(low & 1))
            exponent += stripTrailingZerosOfEven(value);
        result.m_low = lowOf(value);
        result.m_high = highOf(value);
    }
    result.m_exponent = exponent;
    ASSERT(result.m_exponent == exponent && result.isFinite());
    return result;
}

auto Decimal128::coefficient() const -> Wide
{
    ASSERT(isFinite());
    return Wide { m_low, m_high };
}

auto Decimal128::coefficientDigits() const -> Digits
{
    Digits result;
    // From the end, in parts that fit in 64 bits.
    size_t cursor = result.characters.size();
    auto write = [&](uint64_t part, size_t leastDigits) {
        size_t end = cursor;
        while (part >= 100) {
            unsigned pair = static_cast<unsigned>(part % 100);
            part /= 100;
            result.characters[--cursor] = '0' + pair % 10;
            result.characters[--cursor] = '0' + pair / 10;
        }
        if (part >= 10) {
            result.characters[--cursor] = '0' + part % 10;
            part /= 10;
        }
        if (part)
            result.characters[--cursor] = '0' + part;
        while (end - cursor < leastDigits)
            result.characters[--cursor] = '0';
    };
    uint64_t high = m_high;
    uint64_t low = m_low;
    constexpr uint64_t part = 10'000'000'000'000'000'000ull;
    while (high) {
        uint64_t quotientHigh = high / part;
        auto division = divide128ByPowerOf10(high % part, low, 19);
        write(division.remainder, 19);
        high = quotientHigh;
        low = division.quotient;
    }
    write(low, 0);
    result.start = static_cast<unsigned>(cursor);
    return result;
}

int Decimal128::scientificExponent() const
{
    ASSERT(isFinite() && !isZero());
    if (!m_high) [[likely]] {
        int estimate = leastNumberOfDigits(64 - std::countl_zero(m_low));
        return static_cast<int>(m_exponent) + estimate + (m_low >= powersOf10In64Bits[estimate]) - 1;
    }
    return m_exponent + JSC::numberOfDigits(makeUInt128(m_high, m_low)) - 1;
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
// Half even.
auto Decimal128::roundedQuotientByPowerOf10(const Wide& value, int64_t power, bool isInexact) -> Wide
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
        fractionToHalf = division.remainder <=> divisor.half();
        if (isInexact && fractionToHalf == std::strong_ordering::equal)
            fractionToHalf = std::strong_ordering::greater;
    }
    if (roundsUp(RoundingMode::HalfEven, hasFraction, fractionToHalf, quotient.isOdd()))
        quotient = quotient + Wide { 1 };
    return quotient;
}

// https://tc39.es/proposal-decimal/#sec-decimal-roundtodecimaldomain
ALWAYS_INLINE Decimal128 Decimal128::roundToDomain(bool isNegative, uint64_t high, uint64_t low, int64_t exponent, bool isInexact)
{
    // Without rounding.
    if (makeUInt128(high, low) < powersOf10In128Bits[precision] && exponent >= minimumQuantumExponent && exponent <= maximumQuantumExponent) [[likely]] {
        // The spec answers +0 for the mathematical value 0. A caller that wants -0 makes it.
        if (!(high | low)) [[unlikely]]
            return zero(false);
        ASSERT(!isInexact);
        return fromExact(isNegative, high, low, exponent);
    }
    return roundToDomainSlow(isNegative, high, low, exponent, isInexact);
}

Decimal128 Decimal128::roundToDomainSlow(bool isNegative, uint64_t high, uint64_t low, int64_t exponent, bool isInexact)
{
    UInt128 value = makeUInt128(high, low);
    if (!value)
        return zero(false);

    int digits = JSC::numberOfDigits(value);
    ASSERT(!isInexact || digits > precision);
    int64_t quantum = std::max<int64_t>(exponent + digits - precision, minimumQuantumExponent);
    if (quantum > exponent) {
        // 2^128 is less than half of 10^39.
        if (quantum - exponent > maximumShortPower)
            return zero(isNegative);
        auto division = divideByPowerOf10(value, static_cast<int>(quantum - exponent));
        auto fractionToHalf = division.fractionToHalf;
        if (isInexact && fractionToHalf == std::strong_ordering::equal)
            fractionToHalf = std::strong_ordering::greater;
        value = division.quotient;
        if (roundsUp(RoundingMode::HalfEven, division.hasFraction || isInexact, fractionToHalf, lowOf(value) & 1))
            value = value + 1;
        exponent = quantum;
        if (value == powersOf10In128Bits[precision]) {
            value = powersOf10In128Bits[precision - 1];
            ++quantum;
            ++exponent;
        }
    }
    // A shorter coefficient has its quantum below its exponent, and it is the quantum that is limited.
    if (quantum > maximumQuantumExponent)
        return infinity(isNegative);
    if (!value)
        return zero(isNegative);
    return fromExact(isNegative, highOf(value), lowOf(value), exponent);
}

Decimal128 Decimal128::roundToDomain(bool isNegative, const Wide& value, int64_t exponent, bool isInexact)
{
    if (value.fitsIn128Bits())
        return roundToDomain(isNegative, highOf(value.low128()), lowOf(value.low128()), exponent, isInexact);

    int64_t quantum = std::max<int64_t>(exponent + value.numberOfDigits() - precision, minimumQuantumExponent);
    UInt128 quotient;
    if (int64_t power = quantum - exponent; power <= maximumShortPower) {
        // Short divisions by at most 10^19 each.
        Wide rest = value;
        unsigned first = std::min<int64_t>(power, 19);
        unsigned second = static_cast<unsigned>(power) - first;
        uint64_t lowerRemainder = rest.divideByPowerOf10(first);
        uint64_t upperRemainder = second ? rest.divideByPowerOf10(second) : 0;
        UInt128 remainder = multiply64(upperRemainder, powersOf10In64Bits[first]) + lowerRemainder;
        UInt128 half = powersOf10In128Bits[power] >> 1;
        quotient = rest.low128();
        bool isUp = remainder > half || (remainder == half && (isInexact || (lowOf(quotient) & 1)));
        if (isUp)
            quotient = quotient + 1;
    } else
        quotient = roundedQuotientByPowerOf10(value, power, isInexact).low128();
    if (quotient == powersOf10In128Bits[precision]) {
        quotient = powersOf10In128Bits[precision - 1];
        ++quantum;
    }
    if (quantum > maximumQuantumExponent)
        return infinity(isNegative);
    if (!quotient)
        return zero(isNegative);
    return fromExact(isNegative, highOf(quotient), lowOf(quotient), quantum);
}

template<typename CharacterType>
ALWAYS_INLINE std::optional<Decimal128> Decimal128::parse(std::span<const CharacterType> text)
{
    size_t position = 0;
    size_t end = text.size();
    bool isNegative = false;
    if (position != end && (text[position] == '-' || text[position] == '+'))
        isNegative = text[position++] == '-';

    // More digits than any rounding looks at one by one. Of those that follow it only matters whether one is not zero.
    constexpr int partDigits = 19;
    constexpr int keptDigits = 2 * partDigits;
    static_assert(keptDigits > precision + 1);
    // Up to partDigits digits are in the lower one.
    uint64_t upper = 0;
    uint64_t lower = 0;
    int numberOfKept = 0;
    bool isInexact = false;
    int64_t exponent = 0;
    bool hasDigits = false;
    auto parseDigits = [&](bool isFraction) ALWAYS_INLINE_LAMBDA {
        size_t start = position;
        if constexpr (sizeof(CharacterType) == 1 && std::endian::native == std::endian::little) {
            // Eight digits at a time, where they are all of one part.
            while (end - position >= 8) {
                int inPart = numberOfKept < partDigits ? numberOfKept : numberOfKept - partDigits;
                if (!numberOfKept ? text[position] == '0' : (numberOfKept == partDigits || numberOfKept == keptDigits))
                    break;
                if (inPart + 8 > partDigits)
                    break;
                uint64_t chunk;
                memcpySpan(asMutableByteSpan(chunk), asBytes(text.subspan(position, 8)));
                if (((chunk & 0xf0f0f0f0f0f0f0f0) | (((chunk + 0x0606060606060606) & 0xf0f0f0f0f0f0f0f0) >> 4)) != 0x3333333333333333)
                    break;
                chunk = (chunk & 0x0f0f0f0f0f0f0f0f) * 2561 >> 8;
                chunk = (chunk & 0x00ff00ff00ff00ff) * 6553601 >> 16;
                chunk = (chunk & 0x0000ffff0000ffff) * 42949672960001 >> 32;
                lower = lower * 100'000'000 + chunk;
                numberOfKept += 8;
                position += 8;
                if (isFraction)
                    exponent -= 8;
            }
        }
        for (; position != end && isASCIIDigit(text[position]); ++position) {
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
            if (numberOfKept == partDigits) {
                upper = lower;
                lower = 0;
            }
            lower = lower * 10 + digit;
            ++numberOfKept;
        }
        hasDigits |= position != start;
    };

    parseDigits(false);
    if (position != end && text[position] == '.') {
        ++position;
        parseDigits(true);
    }
    if (!hasDigits)
        return std::nullopt;

    if (position != end && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        bool isNegativeExponent = false;
        if (position != end && (text[position] == '-' || text[position] == '+'))
            isNegativeExponent = text[position++] == '-';
        if (position == end)
            return std::nullopt;
        int64_t written = 0;
        for (; position != end && isASCIIDigit(text[position]); ++position)
            written = std::min<int64_t>(written * 10 + (text[position] - '0'), saturatedExponent);
        exponent += isNegativeExponent ? -written : written;
    }
    if (position != end)
        return std::nullopt;

    if (!numberOfKept)
        return zero(isNegative);
    if (numberOfKept <= partDigits)
        return roundToDomain(isNegative, 0, lower, exponent, false);
    UInt128 value = multiply64(upper, powersOf10In64Bits[numberOfKept - partDigits]) + lower;
    return roundToDomain(isNegative, highOf(value), lowOf(value), exponent, isInexact);
}

std::optional<Decimal128> Decimal128::parse(StringView text)
{
    auto result = text.is8Bit() ? parse(text.span8()) : parse(text.span16());
    if (result) [[likely]]
        return result;

    if (text == "NaN"_s)
        return nan();
    if (text == "Infinity"_s || text == "+Infinity"_s)
        return infinity(false);
    if (text == "-Infinity"_s)
        return infinity(true);
    return std::nullopt;
}

auto Decimal128::alignmentsByDistance() -> const Alignment*
{
    alignas(sizeof(Alignment)) static constexpr auto alignments = [] {
        constexpr uint64_t greatest = (1ull << 62) - 1;
        std::array<Alignment, 2 * reachOfAlignments + 1> result { };
        for (int i = -reachOfAlignments; i <= reachOfAlignments; ++i) {
            uint64_t factor = powersOf10In64Bits[std::max(i, 0)];
            uint64_t otherFactor = powersOf10In64Bits[std::max(-i, 0)];
            result[i + reachOfAlignments] = { factor, otherFactor, greatest / factor, greatest / otherFactor };
        }
        return result;
    }();
    return &alignments[reachOfAlignments];
}

auto Decimal128::divisorsByPower() -> const Divisor*
{
    return divisorsOf64Bits.data();
}

const uint64_t* Decimal128::powersOf10()
{
    return powersOf10In64Bits.data();
}

Decimal128 Decimal128::fromInteger(int64_t value)
{
    if (!value)
        return zero(false);
    uint64_t magnitude = value < 0 ? -static_cast<uint64_t>(value) : value;
    return fromExact(value < 0, 0, magnitude, 0);
}

Decimal128 Decimal128::fromDouble(double value)
{
    // Above 2^53 the digits of the double are not those of the integer.
    if (int64_t integer = static_cast<int64_t>(std::abs(value) <= 9007199254740992.0 ? value : 0); integer == value)
        return fromInteger(integer);
    if (!std::isfinite(value)) [[unlikely]]
        return std::isnan(value) ? nan() : infinity(value < 0);
    auto shortest = WTF::numberToShortestDecimal(value);
    return fromExact(value < 0, 0, shortest.significand, shortest.exponent);
}

double Decimal128::toDouble() const
{
    double magnitude;
    if (!m_high && m_low && m_low < powersOf10In64Bits[19]) [[likely]]
        magnitude = WTF::parseJSONDoubleFromDigits(m_low, m_exponent, m_low < powersOf10In64Bits[15] ? 15 : 19);
    else if (isNaN())
        return std::numeric_limits<double>::quiet_NaN();
    else if (isInfinity())
        magnitude = std::numeric_limits<double>::infinity();
    else if (isZero())
        magnitude = 0;
    else {
        size_t parsedLength;
        return parseDouble(toExponentialString(), parsedLength);
    }
    return m_isNegative ? -magnitude : magnitude;
}

auto Decimal128::roundingModeNamed(StringView name) -> std::optional<RoundingMode>
{
    if (name == "floor"_s)
        return RoundingMode::Floor;
    if (name == "halfExpand"_s)
        return RoundingMode::HalfExpand;
    if (name == "ceil"_s)
        return RoundingMode::Ceil;
    if (name == "trunc"_s)
        return RoundingMode::Trunc;
    if (name == "halfEven"_s)
        return RoundingMode::HalfEven;
    return std::nullopt;
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.exponent
double Decimal128::exponent() const
{
    if (isNaN())
        return std::numeric_limits<double>::quiet_NaN();
    if (isInfinity())
        return std::numeric_limits<double>::infinity();
    if (isZero())
        return -std::numeric_limits<double>::infinity();
    return scientificExponent();
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.mantissa
//
// The steps have the Numbers NaN and Infinity in place of the Decimal values, and do not say what e is. It is the exponent.
Decimal128 Decimal128::mantissa() const
{
    if (isInfinity())
        return infinity(false);
    if (isNaN() || isZero())
        return *this;
    return significand();
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
Decimal128 Decimal128::add(Decimal128 x, Decimal128 y)
{
    // Both in 64 bits and not 0, and the sum too. NaN and infinity have a coefficient of 0.
    if (!(x.m_high | y.m_high) && x.m_low && y.m_low) [[likely]] {
        int difference = static_cast<int>(x.m_exponent) - static_cast<int>(y.m_exponent);
        uint64_t a = x.m_low;
        uint64_t b = y.m_low;
        int exponent = std::min<int>(x.m_exponent, y.m_exponent);
        // Above this exponent a sum can be infinite.
        bool overflows = exponent > maximumQuantumExponent;
        if (difference) {
            if (static_cast<unsigned>(difference + 19) > 38)
                overflows = true;
            else {
                overflows |= __builtin_mul_overflow(a, powersOf10In64Bits[std::max(difference, 0)], &a);
                overflows |= __builtin_mul_overflow(b, powersOf10In64Bits[std::max(-difference, 0)], &b);
            }
        }
        bool isNegative = x.m_isNegative;
        uint64_t sum;
        if (x.m_isNegative == y.m_isNegative)
            overflows |= __builtin_add_overflow(a, b, &sum);
        else {
            bool isLess = a < b;
            sum = isLess ? b - a : a - b;
            isNegative ^= isLess;
        }
        if (!overflows) [[likely]] {
            if (!sum) [[unlikely]]
                return zero(false);
            return fromExact(isNegative, 0, sum, exponent);
        }
    }
    return addSlow(x, y);
}

NEVER_INLINE Decimal128 Decimal128::addSlow(Decimal128 x, Decimal128 y)
{
    if (!x.isFinite() || !y.isFinite()) [[unlikely]] {
        if (x.isNaN() || y.isNaN())
            return nan();
        if (x.isInfinity())
            return y.isInfinity() && x.m_isNegative != y.m_isNegative ? nan() : x;
        return y;
    }
    if (x.isZero())
        return y.isZero() ? zero(x.m_isNegative && y.m_isNegative) : y;
    if (y.isZero())
        return x;

    const Decimal128& upper = x.m_exponent >= y.m_exponent ? x : y;
    const Decimal128& lower = x.m_exponent >= y.m_exponent ? y : x;

    // In 128 bits. With 37 digits in units of the lower value the upper one is less than 10^37, and the sum than 2^128.
    constexpr int maximumShortDigits = 37;
    int difference = upper.m_exponent - lower.m_exponent;
    if (!difference || JSC::numberOfDigits(makeUInt128(upper.m_high, upper.m_low)) + difference <= maximumShortDigits) {
        UInt128 upperValue = makeUInt128(upper.m_high, upper.m_low);
        UInt128 lowerValue = makeUInt128(lower.m_high, lower.m_low);
        if (difference)
            upperValue = upper.m_high ? upperValue * powersOf10In128Bits[difference] : difference < static_cast<int>(powersOf10In64Bits.size()) ? multiply64(upper.m_low, powersOf10In64Bits[difference]) : upperValue * powersOf10In128Bits[difference];
        bool isNegative = upper.m_isNegative;
        UInt128 sum;
        if (upper.m_isNegative == lower.m_isNegative)
            sum = upperValue + lowerValue;
        else if (upperValue >= lowerValue)
            sum = upperValue - lowerValue;
        else {
            sum = lowerValue - upperValue;
            isNegative = lower.m_isNegative;
        }
        return roundToDomain(isNegative, highOf(sum), lowOf(sum), lower.m_exponent, false);
    }

    Wide upperValue = upper.coefficient();
    Wide lowerValue = lower.coefficient();

    // 10^76 + 10^34 is less than 2^256, so the sum is exact when the upper value has at most this many digits in units of the lower.
    constexpr int maximumDigits = Wide::maximumPowerOf10 - 1;
    int spare = maximumDigits - upperValue.numberOfDigits();
    if (difference <= spare) {
        upperValue = upperValue * Wide::powerOf10(difference);
        if (upper.m_isNegative == lower.m_isNegative)
            return roundToDomain(upper.m_isNegative, upperValue + lowerValue, lower.m_exponent, false);
        if (upperValue >= lowerValue)
            return roundToDomain(upper.m_isNegative, upperValue - lowerValue, lower.m_exponent, false);
        return roundToDomain(lower.m_isNegative, lowerValue - upperValue, lower.m_exponent, false);
    }

    // The lower value is less than 10^-42 of the upper one, so the upper one is the nearest value.
    return upper;
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.subtract
Decimal128 Decimal128::subtract(Decimal128 x, Decimal128 y)
{
    return add(x, y.negate());
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.multiply
Decimal128 Decimal128::multiply(Decimal128 x, Decimal128 y)
{
    // All in 64 bits and not 0. NaN and infinity have a coefficient of 0.
    uint64_t product;
    int exponent = static_cast<int>(x.m_exponent) + static_cast<int>(y.m_exponent);
    if (!(x.m_high | y.m_high) && !__builtin_mul_overflow(x.m_low, y.m_low, &product) && product && static_cast<unsigned>(exponent - minimumQuantumExponent) <= static_cast<unsigned>(maximumQuantumExponent - minimumQuantumExponent)) [[likely]]
        return fromExact(x.m_isNegative != y.m_isNegative, 0, product, exponent);
    return multiplySlow(x, y);
}

NEVER_INLINE Decimal128 Decimal128::multiplySlow(Decimal128 x, Decimal128 y)
{
    bool isNegative = x.m_isNegative != y.m_isNegative;
    if (!(x.m_low | x.m_high) || !(y.m_low | y.m_high)) [[unlikely]] {
        if (x.isNaN() || y.isNaN())
            return nan();
        if (x.isInfinity() || y.isInfinity())
            return x.isZero() || y.isZero() ? nan() : infinity(isNegative);
        return zero(isNegative);
    }
    if (!(x.m_high | y.m_high)) {
        UInt128 product = multiply64(x.m_low, y.m_low);
        return roundToDomain(isNegative, highOf(product), lowOf(product), x.m_exponent + y.m_exponent, false);
    }
    return roundToDomain(isNegative, x.coefficient() * y.coefficient(), x.m_exponent + y.m_exponent, false);
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.divide
Decimal128 Decimal128::divide(Decimal128 x, Decimal128 y)
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

    // A long division, up to 19 digits at a time.
    if (!(x.m_high | y.m_high)) {
        uint64_t divisor = y.m_low;
        UInt128 first = multiply64(x.m_low, powersOf10In64Bits[19]);
        auto division = divide128By64(highOf(first) % divisor, lowOf(first), divisor);
        UInt128 quotient = makeUInt128(highOf(first) / divisor, division.quotient);
        uint64_t remainder = division.remainder;
        int64_t exponent = static_cast<int64_t>(x.m_exponent) - y.m_exponent - 19;
        while (remainder) {
            int digits = !quotient ? 0 : JSC::numberOfDigits(quotient);
            if (digits > precision)
                break;
            int power = std::min(19, precision + 1 - digits);
            UInt128 dividend = multiply64(remainder, powersOf10In64Bits[power]);
            auto next = divide128By64(highOf(dividend), lowOf(dividend), divisor);
            quotient = quotient * powersOf10In64Bits[power] + next.quotient;
            remainder = next.remainder;
            exponent -= power;
        }
        return roundToDomain(isNegative, highOf(quotient), lowOf(quotient), exponent, !!remainder);
    }

    // The quotient has 35 or 36 digits, which is enough for a correct rounding. The dividend has at most 69.
    UInt128 divisor = makeUInt128(y.m_high, y.m_low);
    int padding = precision + 1 + JSC::numberOfDigits(divisor) - JSC::numberOfDigits(makeUInt128(x.m_high, x.m_low));
    auto division = Wide::divide(x.coefficient() * Wide::powerOf10(padding), Wide::from(divisor));
    return roundToDomain(isNegative, division.quotient, x.m_exponent - padding - y.m_exponent, !division.remainder.isZero());
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.remainder
Decimal128 Decimal128::remainder(Decimal128 x, Decimal128 y)
{
    if (x.isNaN() || y.isNaN() || x.isInfinity() || y.isZero())
        return nan();
    if (y.isInfinity() || x.isZero())
        return x;

    // The result is a multiple of the smaller quantum and less than both operands, so it needs no rounding.
    int distance = x.m_exponent - y.m_exponent;
    auto finish = [&](UInt128 remainder) {
        if (!remainder)
            return zero(x.m_isNegative);
        return fromExact(x.m_isNegative, highOf(remainder), lowOf(remainder), std::min(x.m_exponent, y.m_exponent));
    };
    UInt128 xCoefficient = makeUInt128(x.m_high, x.m_low);
    UInt128 yCoefficient = makeUInt128(y.m_high, y.m_low);
    if (distance <= 0) {
        // Both are less than 10^34 where the multiplied modulus is not greater than the value.
        if (distance && JSC::numberOfDigits(yCoefficient) - distance > JSC::numberOfDigits(xCoefficient))
            return x;
        UInt128 modulus = distance ? yCoefficient * powersOf10In128Bits[-distance] : yCoefficient;
        if (xCoefficient < modulus)
            return x;
        if (!highOf(xCoefficient))
            return finish(lowOf(xCoefficient) % lowOf(modulus));
        return finish(xCoefficient % modulus);
    }
    if (!(x.m_high | y.m_high) && distance <= 19) {
        UInt128 dividend = multiply64(x.m_low, powersOf10In64Bits[distance]);
        return finish(divide128By64(highOf(dividend) % y.m_low, lowOf(dividend), y.m_low).remainder);
    }

    Wide value = x.coefficient();
    Wide modulus = y.coefficient();
    auto modulo = [&](const Wide& dividend) {
        return Wide::divide(dividend, modulus).remainder;
    };
    if (distance <= Wide::maximumPowerOf10 - precision - 1)
        return finish(modulo(value * Wide::powerOf10(distance)).low128());
    // value * 10^distance mod modulus. Each factor is less than 10^34 here.
    value = modulo(value);
    Wide base = modulo(Wide { 10 });
    for (int power = distance; power; power >>= 1) {
        if (power & 1)
            value = modulo(value * base);
        base = modulo(base * base);
    }
    return finish(value.low128());
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.compare
std::partial_ordering Decimal128::compare(Decimal128 x, Decimal128 y)
{
    auto order = [](UInt128 a, UInt128 b, bool isReversed) -> std::partial_ordering {
        int result = (a > b) - (a < b);
        return (isReversed ? -result : result) <=> 0;
    };
    // Of 64 bits, not 0, and of one sign. NaN and infinity have a coefficient of 0.
    int distance = x.m_exponent - y.m_exponent;
    if (!(x.m_high | y.m_high) && x.m_low && y.m_low && x.m_isNegative == y.m_isNegative && static_cast<unsigned>(distance + 19) <= 38) [[likely]] {
        // Without a branch on which one has the larger exponent, which no processor can guess.
        uint64_t xFactor = powersOf10In64Bits[std::max(distance, 0)];
        uint64_t yFactor = powersOf10In64Bits[std::max(-distance, 0)];
        return order(multiply64(x.m_low, xFactor), multiply64(y.m_low, yFactor), x.isNegative());
    }
    return compareSlow(x, y);
}

NEVER_INLINE std::partial_ordering Decimal128::compareSlow(Decimal128 x, Decimal128 y)
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
        UInt128 xCoefficient = makeUInt128(x.m_high, x.m_low);
        UInt128 yCoefficient = makeUInt128(y.m_high, y.m_low);
        if (x.m_exponent == y.m_exponent)
            return xCoefficient < yCoefficient ? std::strong_ordering::less : xCoefficient == yCoefficient ? std::strong_ordering::equal : std::strong_ordering::greater;
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

// The coefficient in units of 10^power. Answers it through the references.
void Decimal128::roundCoefficient(int64_t power, RoundingMode mode, uint64_t& high, uint64_t& low) const
{
    ASSERT(power > 0 && isFinite() && !isZero());
    // Of fewer digits than the power, so less than a tenth.
    DecimalQuotient division { 0, std::strong_ordering::less, true };
    if (power <= maximumShortPower)
        division = divideByPowerOf10(makeUInt128(m_high, m_low), static_cast<int>(power));
    UInt128 count = division.quotient;
    if (roundsUp(modeForMagnitude(m_isNegative, mode), division.hasFraction, division.fractionToHalf, lowOf(count) & 1))
        count = count + 1;
    high = highOf(count);
    low = lowOf(count);
}

// https://tc39.es/proposal-decimal/#sec-decimal-method-round
Decimal128 Decimal128::round(double numberOfFractionalDigits, RoundingMode mode) const
{
    if (!isFinite() || isZero())
        return *this;
    if (numberOfFractionalDigits >= 0 && m_exponent >= 0)
        return *this;
    int64_t unit = saturate(-numberOfFractionalDigits);
    if (unit <= m_exponent)
        return *this;
    // The spec rounds to the domain in the same mode, but nothing is left to round: the count is at most 10^34, and the unit is in the domain or overflows.
    uint64_t high;
    uint64_t low;
    roundCoefficient(unit - m_exponent, mode, high, low);
    return roundToDomain(m_isNegative, high, low, unit, false);
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
    return roundToDomain(m_isNegative, m_high, m_low, m_exponent + saturate(power), false);
}

// What StringBuilder is, for a text of a known length.
class DecimalText {
public:
    static constexpr int64_t capacity = 96;

    void append(char character)
    {
        ASSERT(m_length < m_characters.size());
        m_characters[m_length++] = character;
    }

    void append(std::span<const Latin1Character> characters)
    {
        // Those of zero have no address, which memcpy does not allow.
        if (characters.empty())
            return;
        memcpySpan(std::span { m_characters }.subspan(m_length), characters);
        m_length += characters.size();
    }
    void append(StringView characters) { append(characters.span8()); }
    void append(char character, int number)
    {
        append(character);
        if (number < 0)
            append('-');
        unsigned magnitude = std::abs(number);
        if (magnitude >= 1000)
            append(static_cast<char>('0' + magnitude / 1000));
        if (magnitude >= 100)
            append(static_cast<char>('0' + magnitude / 100 % 10));
        if (magnitude >= 10)
            append(static_cast<char>('0' + magnitude / 10 % 10));
        append(static_cast<char>('0' + magnitude % 10));
    }

    String toString() const { return std::span { m_characters }.first(m_length); }

private:
    std::array<Latin1Character, capacity> m_characters;
    size_t m_length { 0 };
};

template<typename Builder>
static void appendZeros(Builder& builder, int64_t count)
{
    constexpr ASCIILiteral zeros = "0000000000000000000000000000000000000000000000000000000000000000"_s;
    for (; count > 0; count -= zeros.length())
        builder.append(StringView(zeros).left(static_cast<unsigned>(std::min<int64_t>(count, zeros.length()))));
}

// Writes digits * 10^exponent with so many digits behind the point, which are at least as many as it has.
template<typename Builder>
static void appendPlain(Builder& builder, std::span<const Latin1Character> digits, int64_t exponent, int64_t fractionalDigits)
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
template<typename Builder>
static void appendExponential(Builder& builder, std::span<const Latin1Character> digits, int exponent, int64_t numberOfDigits)
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

static String toStringUnlessOverflowed(StringBuilder& builder)
{
    if (builder.hasOverflowed())
        return { };
    return builder.toString();
}

// `greatestLength` is an upper bound.
template<typename Function>
static String writeText(int64_t greatestLength, const Function& write)
{
    if (greatestLength <= DecimalText::capacity) {
        DecimalText builder;
        write(builder);
        return builder.toString();
    }
    StringBuilder builder(OverflowPolicy::RecordOverflow);
    write(builder);
    return toStringUnlessOverflowed(builder);
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
    if (!isFinite() || isZero()) [[unlikely]]
        return toDecimalString();
    auto digits = coefficientDigits();
    int size = static_cast<int>(digits.size());
    int exponent = m_exponent + size - 1;
    if (exponent < -6 || exponent >= precision)
        return toExponentialString();

    DecimalText builder;
    if (m_isNegative)
        builder.append('-');
    appendPlain(builder, digits.span(), m_exponent, std::max<int>(-m_exponent, 0));
    return builder.toString();
}

// https://tc39.es/proposal-decimal/#sec-decimal-decimaltodecimalstring
// With the sign of an integer, and with all digits of the fraction.
String Decimal128::toDecimalString() const
{
    if (!isFinite())
        return nonFiniteString(*this);
    if (isZero())
        return m_isNegative ? "-0"_s : "0"_s;

    auto digits = coefficientDigits();
    return writeText(static_cast<int64_t>(digits.size()) + std::abs(static_cast<int64_t>(m_exponent)) + 3, [&](auto& builder) {
        if (m_isNegative)
            builder.append('-');
        appendPlain(builder, digits.span(), m_exponent, std::max<int>(-m_exponent, 0));
    });
}

// https://tc39.es/proposal-decimal/#sec-decimaltoexponentialstring
// With 1 <= argument * 10^-e < 10 in place of 10^e.
String Decimal128::toExponentialString() const
{
    if (!isFinite())
        return nonFiniteString(*this);
    if (isZero())
        return m_isNegative ? "-0e+0"_s : "0e+0"_s;
    // At most 34 digits, two signs, the point, the e and 4 digits.
    DecimalText builder;
    if (m_isNegative)
        builder.append('-');
    auto digits = coefficientDigits();
    appendExponential(builder, digits.span(), m_exponent + static_cast<int>(digits.size()) - 1, digits.size());
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
        uint64_t high;
        uint64_t low;
        roundCoefficient(static_cast<int64_t>(exponent) - m_exponent, mode, high, low);
        rounded.m_high = high;
        rounded.m_low = low;
    }
    return writeText(static_cast<int64_t>(std::abs(exponent)) + fractionalDigits + precision + 4, [&](auto& builder) {
        // The sign stays when all digits are rounded away, as with Number.prototype.toFixed.
        if (m_isNegative)
            builder.append('-');
        appendPlain(builder, rounded.coefficientDigits().span(), exponent, fractionalDigits);
    });
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

    // The steps have the sign with one digit only, and as many zeros behind the point as there are digits.
    if (isZero() && wanted == 1)
        return m_isNegative ? "-0"_s : "0"_s;

    auto write = [&](auto& builder) {
        if (isZero()) {
            appendPlain(builder, { }, 0, wanted);
            return;
        }

        if (m_isNegative)
            builder.append('-');
        int numberOfCoefficientDigits = JSC::numberOfDigits(makeUInt128(m_high, m_low));
        if (numberOfCoefficientDigits < wanted && !isInteger()) {
            // The steps use decimal notation whatever the exponent is.
            appendPlain(builder, coefficientDigits().span(), m_exponent, static_cast<int64_t>(wanted) - numberOfCoefficientDigits - m_exponent);
            return;
        }

        // The steps for the other values do not give the value (1.25 is "125" and 1200 is "12.0"). They also count the sign as a digit, can
        // write it twice, and round a negative value as if it were positive. These are the digits of Number.prototype.toPrecision, to which
        // the proposal says that this is similar. So is the notation, but a value that has more digits than asked for has an exponent, as in
        // the steps, whose scale and exponent are wrong there.
        int scientific = scientificExponent();
        if (int excess = numberOfCoefficientDigits - wanted; excess > 0) {
            // At most 33 digits go.
            uint64_t high;
            uint64_t low;
            roundCoefficient(excess, mode, high, low);
            UInt128 magnitude = makeUInt128(high, low);
            if (magnitude == powersOf10In128Bits[wanted]) {
                magnitude = powersOf10In128Bits[wanted - 1];
                ++scientific;
            }
            Decimal128 rounded = *this;
            rounded.m_low = lowOf(magnitude);
            rounded.m_high = highOf(magnitude);
            appendExponential(builder, rounded.coefficientDigits().span(), scientific, wanted);
            return;
        }

        auto digits = coefficientDigits();
        if (scientific < -6 || scientific >= wanted)
            appendExponential(builder, digits.span(), scientific, wanted);
        else
            appendPlain(builder, digits.span(), m_exponent, wanted - 1 - scientific);
    };
    return writeText(static_cast<int64_t>(wanted) + std::abs(static_cast<int64_t>(m_exponent)) + precision + 8, write);
}

} // namespace JSC
