/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "Decimal128.h"

#include <wtf/ASCIICType.h>
#include <wtf/FastFloat.h>
#include <wtf/UnalignedAccess.h>
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

// For divisions by two words: Moller and Granlund, Improved Division by Invariant Integers (2011), algorithms 5 and 6.
struct LongDivisor {
    UInt128 value; // shifted so that its upper bit is set
    uint64_t reciprocal; // (2^192 - 1) / value - 2^64
    unsigned shift;
};

static ALWAYS_INLINE LongDivisor makeLongDivisor(UInt128 divisor)
{
    ASSERT(highOf(divisor));
    unsigned shift = std::countl_zero(highOf(divisor));
    uint64_t high = highOf(divisor) << shift | lowOf(divisor) >> 1 >> (63 - shift);
    uint64_t low = lowOf(divisor) << shift;

    uint64_t reciprocal = divide128By64(~high, ~0ull, high).quotient;
    uint64_t rest = high * reciprocal + low;
    uint64_t carries = -static_cast<uint64_t>(rest < low);
    uint64_t carriesTwice = carries & -static_cast<uint64_t>(rest >= high);
    reciprocal += carries + carriesTwice;
    rest -= (carries & high) + (carriesTwice & high);

    UInt128 product = multiply64(reciprocal, low);
    rest += highOf(product);
    carries = -static_cast<uint64_t>(rest < highOf(product));
    carriesTwice = carries & -static_cast<uint64_t>(makeUInt128(rest, lowOf(product)) >= makeUInt128(high, low));
    return { makeUInt128(high, low), reciprocal + carries + carriesTwice, shift };
}

static constexpr int leastLongPower = 20;

static constexpr auto longPowersOf10 = [] {
    std::array<LongDivisor, powersOf10In128Bits.size() - leastLongPower> result { };
    for (unsigned i = 0; i < result.size(); ++i) {
        unsigned shift = std::countl_zero(highOf(powersOf10In128Bits[leastLongPower + i]));
        UInt128 divisor = powersOf10In128Bits[leastLongPower + i] << shift;
        // By bits.
        UInt128 remainder = makeUInt128(~highOf(divisor), ~lowOf(divisor));
        uint64_t quotient = 0;
        for (int bit = 0; bit < 64; ++bit) {
            bool carry = highOf(remainder) >> 63;
            remainder = remainder << 1 | 1;
            quotient <<= 1;
            if (carry || remainder >= divisor) {
                remainder = remainder - divisor;
                quotient |= 1;
            }
        }
        result[i] = { divisor, quotient, shift };
    }
    return result;
}();

struct DecimalDivision192 {
    uint64_t quotient;
    UInt128 remainder;
};

// (upper * 2^64 + low) / divisor, where upper is less than the divisor. All are shifted.
static ALWAYS_INLINE DecimalDivision192 divide192By128(UInt128 upper, uint64_t low, const LongDivisor& divisor)
{
    ASSERT(upper < divisor.value);
    UInt128 estimate = multiply64(divisor.reciprocal, highOf(upper)) + upper;
    uint64_t quotient = highOf(estimate);
    UInt128 remainder = makeUInt128(lowOf(upper) - quotient * highOf(divisor.value), low) - multiply64(lowOf(divisor.value), quotient) - divisor.value;
    uint64_t isTooLarge = -static_cast<uint64_t>(highOf(remainder) >= lowOf(estimate));
    quotient += 1 + isTooLarge;
    remainder = remainder + makeUInt128(highOf(divisor.value) & isTooLarge, lowOf(divisor.value) & isTooLarge);
    if (remainder >= divisor.value) [[unlikely]] {
        ++quotient;
        remainder = remainder - divisor.value;
    }
    return { quotient, remainder };
}

// floor(bits * log10(2)) is the number of digits or one less. Exact from -256 to 256 bits.
static ALWAYS_INLINE int leastNumberOfDigits(int bits)
{
    return bits * 1233 >> 12;
}

static ALWAYS_INLINE int numberOfBits(UInt128 value)
{
    return highOf(value) ? 128 - std::countl_zero(highOf(value)) : 64 - std::countl_zero(lowOf(value));
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

alignas(64) static constexpr auto decimalTables = [] {
    Decimal128::Tables result { };

    constexpr uint64_t greatest = (1ull << 62) - 1;
    for (int i = -Decimal128::reachOfAlignments; i <= Decimal128::reachOfAlignments; ++i) {
        uint64_t factor = powersOf10In64Bits[std::max(i, 0)];
        uint64_t otherFactor = powersOf10In64Bits[std::max(-i, 0)];
        constexpr uint64_t greatestUnit = static_cast<uint64_t>(Decimal128::maximumQuantumExponent) << Decimal128::shiftOfUnit;
        result.alignments[i + Decimal128::reachOfAlignments] = { factor, otherFactor, greatest / factor, greatest / otherFactor, greatestUnit, Decimal128::inverseOf5, Decimal128::greatestTenth, 0 };
    }

    // 10^i is 2^i * 5^i. The reciprocal of 5^i is too large by less than 2^-(64 + shift), which a dividend below 2^63 makes less than 5^-i.
    for (unsigned i = 1; i < result.divisors.size(); ++i) {
        uint64_t powerOf5 = powersOf10In64Bits[i] >> i;
        unsigned shift = 63 - std::countl_zero(powerOf5);
        // 2^(64 + shift) / 5^i. The division of UInt128 is not constexpr everywhere.
        uint64_t quotient = 0;
        uint64_t remainder = 1ull << shift;
        for (unsigned bit = 0; bit < 64; ++bit) {
            remainder *= 2;
            quotient = quotient * 2 + (remainder >= powerOf5);
            remainder %= powerOf5;
        }
        result.divisors[i] = { powersOf10In64Bits[i], powersOf10In64Bits[i] / 2, quotient + 1, shift };
    }

    // From the next power of 10 on the sum carries into the count.
    for (unsigned i = 0; i < result.digitSteps.size(); ++i) {
        uint64_t least = 1ull << (Decimal128::shiftOfDigitSteps - 1 - i);
        uint64_t digits = 1;
        while (powersOf10In64Bits[digits] <= least)
            ++digits;
        result.digitSteps[i] = (digits << Decimal128::shiftOfDigitSteps) - std::min<uint64_t>(powersOf10In64Bits[digits], 1ull << Decimal128::shiftOfDigitSteps);
    }
    for (unsigned i = 0; i < result.digitStepsOfLongValues.size(); ++i) {
        UInt128 least = static_cast<UInt128>(1) << (64 + Decimal128::shiftOfExponent - 1 - i);
        uint64_t digits = 1;
        while (powersOf10In128Bits[digits] <= least)
            ++digits;
        UInt128 step = makeUInt128(digits << Decimal128::shiftOfExponent, 0) - powersOf10In128Bits[digits];
        result.digitStepsOfLongValues[i] = { lowOf(step), highOf(step) };
    }
    return result;
}();

static ALWAYS_INLINE DecimalDivision64 divide64ByPowerOf10(uint64_t value, unsigned power)
{
    ASSERT(power);
    auto& divisor = decimalTables.divisors[power];
    uint64_t quotient = highOf(multiply64(value >> power, divisor.reciprocal)) >> divisor.shift;
    return { quotient, value - quotient * divisor.value };
}

static ALWAYS_INLINE DecimalDivision128 divideByShortPowerOf10(UInt128 value, unsigned power)
{
    if (!highOf(value)) {
        auto division = divide64ByPowerOf10(lowOf(value), power);
        return { division.quotient, division.remainder };
    }
    auto upper = divide64ByPowerOf10(highOf(value), power);
    auto lower = divide128ByPowerOf10(upper.remainder, lowOf(value), power);
    return { makeUInt128(upper.quotient, lower.quotient), lower.remainder };
}

// The rest is what the remainder lacks of the divisor.
struct DecimalQuotient {
    UInt128 quotient;
    UInt128 remainder;
    UInt128 rest;
};

static constexpr int maximumShortPower = 38;

static ALWAYS_INLINE DecimalQuotient divideByPowerOf10(UInt128 value, int power)
{
    ASSERT(power > 0 && power <= maximumShortPower);
    if (power < leastLongPower) {
        auto division = divideByShortPowerOf10(value, power);
        return { division.quotient, division.remainder, powersOf10In64Bits[power] - division.remainder };
    }
    auto& divisor = longPowersOf10[power - leastLongPower];
    uint64_t top = highOf(value) >> 1 >> (63 - divisor.shift);
    uint64_t middle = highOf(value) << divisor.shift | lowOf(value) >> 1 >> (63 - divisor.shift);
    auto division = divide192By128(makeUInt128(top, middle), lowOf(value) << divisor.shift, divisor);
    return { division.quotient, division.remainder, divisor.value - division.remainder };
}

// Both without a branch on the digits or on the sign, which no processor can guess.
template<typename Integer>
static ALWAYS_INLINE bool isNearerToNext(Integer remainder, Integer rest, bool tieGoesToNext)
{
    return remainder > rest - tieGoesToNext;
}

template<typename Integer>
static ALWAYS_INLINE bool magnitudeRoundsUp(Decimal128::RoundingMode mode, bool isNegative, Integer remainder, Integer rest, bool isOdd)
{
    using enum Decimal128::RoundingMode;
    if (mode == HalfEven || mode == HalfExpand)
        return isNearerToNext(remainder, rest, isOdd | (mode == HalfExpand));
    return (mode == (isNegative ? Floor : Ceil)) & !!remainder;
}

// Answers the value if it is no multiple. See Decimal128::inverseOf5.
template<int power>
static ALWAYS_INLINE uint64_t divideIfMultipleOfPowerOf10(uint64_t value)
{
    constexpr uint64_t inverse = [] {
        uint64_t result = 1;
        for (int i = 0; i < power; ++i)
            result *= Decimal128::inverseOf5;
        return result;
    }();
    uint64_t quotient = std::rotr(value * inverse, power);
    return quotient <= std::numeric_limits<uint64_t>::max() / powersOf10In64Bits[power] ? quotient : value;
}

struct StrippedCoefficient {
    uint64_t value;
    int numberOfZeros;
};

static NEVER_INLINE StrippedCoefficient withoutManyTrailingZeros(uint64_t value)
{
    uint64_t result = divideIfMultipleOfPowerOf10<16>(value);
    result = divideIfMultipleOfPowerOf10<8>(result);
    result = divideIfMultipleOfPowerOf10<4>(result);
    result = divideIfMultipleOfPowerOf10<2>(result);
    result = divideIfMultipleOfPowerOf10<1>(result);
    return { result, std::countr_zero(value) - std::countr_zero(result) };
}

// These remove the zeros at the end of a value that is not 0, and answer how many.
static ALWAYS_INLINE int stripTrailingZeros(uint64_t& value)
{
    uint64_t tenth = std::rotr(value * Decimal128::inverseOf5, 1);
    if (tenth > Decimal128::greatestTenth) [[likely]]
        return 0;
    if (std::rotr(tenth * Decimal128::inverseOf5, 1) > Decimal128::greatestTenth) [[likely]] {
        value = tenth;
        return 1;
    }
    auto stripped = withoutManyTrailingZeros(value);
    value = stripped.value;
    return stripped.numberOfZeros;
}

// What leaves the same remainder by 5: 2^64 is 1 more than a multiple of it.
static ALWAYS_INLINE uint64_t sumOfWords(UInt128 value)
{
    uint64_t sum = highOf(value) + lowOf(value);
    return sum + (sum < lowOf(value));
}

static ALWAYS_INLINE bool isMultipleOf10(UInt128 value)
{
    return (sumOfWords(value) * Decimal128::inverseOf5 | lowOf(value) << 63) <= std::numeric_limits<uint64_t>::max() / 5;
}

// With the inverse of 5 modulo 2^128.
static ALWAYS_INLINE UInt128 divideMultipleOf10By10(UInt128 value)
{
    constexpr uint64_t inverseHigh = 0xccccccccccccccccull;
    constexpr uint64_t inverseLow = 0xcccccccccccccccdull;
    value = value >> 1;
    UInt128 product = multiply64(lowOf(value), inverseLow);
    return makeUInt128(highOf(product) + lowOf(value) * inverseHigh + highOf(value) * inverseLow, lowOf(product));
}

// Of a multiple of 10.
static ALWAYS_INLINE int stripTrailingZerosOfLong(UInt128& value)
{
    value = divideMultipleOf10By10(value);
    int count = 1;
    if (!isMultipleOf10(value)) [[likely]]
        return count;
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

    static ALWAYS_INLINE Wide product(UInt128 a, UInt128 b)
    {
        UInt128 lowest = multiply64(lowOf(a), lowOf(b));
        UInt128 middle = multiply64(lowOf(a), highOf(b)) + highOf(lowest);
        UInt128 otherMiddle = multiply64(highOf(a), lowOf(b)) + lowOf(middle);
        UInt128 highest = multiply64(highOf(a), highOf(b)) + highOf(middle) + highOf(otherMiddle);
        Wide result;
        result.m_limbs = { lowOf(lowest), lowOf(otherMiddle), lowOf(highest), highOf(highest) };
        return result;
    }

    // Both for a quotient below 2^127. The functor divides two words by the divisor.
    ALWAYS_INLINE DecimalQuotient dividedBy(uint64_t divisor, NOESCAPE const Invocable<DecimalDivision64(uint64_t, uint64_t)> auto& divide) const
    {
        ASSERT(!m_limbs[3]);
        auto upper = divide(m_limbs[2], m_limbs[1]);
        auto lower = divide(upper.remainder, m_limbs[0]);
        return { makeUInt128(upper.quotient, lower.quotient), lower.remainder, divisor - lower.remainder };
    }

    ALWAYS_INLINE DecimalQuotient dividedBy(const LongDivisor& divisor) const
    {
        auto shifted = [&](unsigned i) {
            return m_limbs[i] << divisor.shift | m_limbs[i - 1] >> 1 >> (63 - divisor.shift);
        };
        ASSERT(!(m_limbs[3] >> 1 >> (63 - divisor.shift)));
        auto upper = divide192By128(makeUInt128(shifted(3), shifted(2)), shifted(1), divisor);
        auto lower = divide192By128(upper.remainder, m_limbs[0] << divisor.shift, divisor);
        return { makeUInt128(upper.quotient, lower.quotient), lower.remainder, divisor.value - lower.remainder };
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

    // Of more than 128 bits.
    ALWAYS_INLINE int numberOfDigits() const
    {
        ASSERT(!fitsIn128Bits());
        UInt128 upper = makeUInt128(m_limbs[3], m_limbs[2]);
        int estimate = leastNumberOfDigits(128 + numberOfBits(upper));
        const Wide& power = powerOf10(estimate);
        bool borrow = makeUInt128(m_limbs[1], m_limbs[0]) < makeUInt128(power.m_limbs[1], power.m_limbs[0]);
        return estimate + (upper >= makeUInt128(power.m_limbs[3], power.m_limbs[2]) + borrow);
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
    ASSERT(makeUInt128(high, low) < powersOf10In128Bits[precision]);
    if (!high)
        exponent += stripTrailingZeros(low);
    else if (isMultipleOf10(makeUInt128(high, low))) [[unlikely]]
        return fromLongMultipleOf10(isNegative, high, low, exponent);
    return fromFields(isNegative, high, low, exponent);
}

ALWAYS_INLINE Decimal128 Decimal128::fromFields(bool isNegative, uint64_t high, uint64_t low, int64_t exponent)
{
    Decimal128 result { static_cast<int>(exponent), isNegative };
    result.m_low = low;
    result.m_high = high;
    ASSERT(result.m_exponent == exponent && result.isFinite());
    return result;
}

NEVER_INLINE Decimal128 Decimal128::fromLongMultipleOf10(bool isNegative, uint64_t high, uint64_t low, int64_t exponent)
{
    UInt128 value = makeUInt128(high, low);
    exponent += stripTrailingZerosOfLong(value);
    return fromFields(isNegative, highOf(value), lowOf(value), exponent);
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

// https://tc39.es/proposal-decimal/#sec-decimal-applyroundingmodetopositive
// Half even.
NEVER_INLINE auto Decimal128::roundedQuotientByPowerOf10(const Wide& value, int64_t power) -> Wide
{
    ASSERT(power > 0);
    // A value of fewer digits than the power is less than a tenth.
    if (power > value.numberOfDigits())
        return { };
    const Wide& divisor = Wide::powerOf10(static_cast<int>(power));
    auto division = Wide::divide(value, divisor);
    // The divisor is even, so half of it is a whole number.
    auto order = division.remainder <=> divisor.half();
    return order > 0 || (order == 0 && division.quotient.isOdd()) ? division.quotient + Wide { 1 } : division.quotient;
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

    // One digit too many, as many a sum has, and no more once it is rounded.
    if (value >= powersOf10In128Bits[precision] && value < powersOf10In128Bits[precision + 1] - 5 && exponent >= minimumQuantumExponent && exponent < maximumQuantumExponent) {
        unsigned digit = sumOfWords(value) % 5;
        digit += (digit ^ low) & 1 ? 5 : 0;
        value = divideMultipleOf10By10(value - digit);
        value = value + isNearerToNext(digit, 10 - digit, isInexact | (lowOf(value) & 1));
        return fromExact(isNegative, highOf(value), lowOf(value), exponent + 1);
    }

    int64_t quantum = std::max<int64_t>(exponent + JSC::numberOfDigits(value) - precision, minimumQuantumExponent);
    ASSERT(!isInexact || quantum > exponent || quantum > maximumQuantumExponent);
    if (quantum > exponent) {
        // 2^128 is less than half of 10^39.
        if (quantum - exponent > maximumShortPower)
            return zero(isNegative);
        auto division = divideByPowerOf10(value, static_cast<int>(quantum - exponent));
        value = division.quotient + isNearerToNext(division.remainder, division.rest, isInexact | (lowOf(division.quotient) & 1));
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

ALWAYS_INLINE Decimal128 Decimal128::roundToDomain(bool isNegative, const Wide& value, int64_t exponent)
{
    if (value.fitsIn128Bits())
        return roundToDomain(isNegative, highOf(value.low128()), lowOf(value.low128()), exponent, false);

    int64_t quantum = std::max<int64_t>(exponent + value.numberOfDigits() - precision, minimumQuantumExponent);
    UInt128 quotient;
    if (int64_t power = quantum - exponent; power <= maximumShortPower) [[likely]] {
        auto division = power >= leastLongPower ? value.dividedBy(longPowersOf10[power - leastLongPower]) : value.dividedBy(powersOf10In64Bits[power], [&](uint64_t high, uint64_t low) {
            return divide128ByPowerOf10(high, low, power);
        });
        quotient = division.quotient + isNearerToNext(division.remainder, division.rest, lowOf(division.quotient) & 1);
    } else
        quotient = roundedQuotientByPowerOf10(value, power).low128();
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

// Of eight bytes from 0 to 9, the first of which is the lowest.
static ALWAYS_INLINE uint64_t valueOfEightDigits(uint64_t digits)
{
    digits = digits * 2561 >> 8;
    digits = (digits & 0x00ff00ff00ff00ff) * 6553601 >> 16;
    return (digits & 0x0000ffff0000ffff) * 42949672960001 >> 32;
}

// What is behind the digits, from the position on. Null if it is not an exponent or nothing.
template<typename CharacterType>
static ALWAYS_INLINE std::optional<int64_t> parseExponent(std::span<const CharacterType> text, size_t position)
{
    size_t end = text.size();
    if (position == end)
        return 0;
    if (!isASCIIAlphaCaselessEqual(text[position++], 'e'))
        return std::nullopt;
    bool isNegative = false;
    if (position != end && (text[position] == '-' || text[position] == '+'))
        isNegative = text[position++] == '-';
    if (position == end)
        return std::nullopt;
    int64_t written = 0;
    for (; position != end && isASCIIDigit(text[position]); ++position)
        written = std::min<int64_t>(written * 10 + (text[position] - '0'), saturatedExponent);
    if (position != end)
        return std::nullopt;
    return isNegative ? -written : written;
}

template<typename CharacterType>
std::optional<Decimal128> Decimal128::parseByDigits(std::span<const CharacterType> text)
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

    auto written = parseExponent(text, position);
    if (!written)
        return std::nullopt;
    exponent += *written;

    if (!numberOfKept)
        return zero(isNegative);
    if (numberOfKept <= partDigits)
        return roundToDomain(isNegative, 0, lower, exponent, false);
    UInt128 value = multiply64(upper, powersOf10In64Bits[numberOfKept - partDigits]) + lower;
    return roundToDomain(isNegative, highOf(value), lowOf(value), exponent, isInexact);
}

static constexpr uint64_t eachByte = 0x0101010101010101;

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

template<typename Word>
static ALWAYS_INLINE uint64_t loadCharacters(std::span<const Latin1Character> text, size_t offset)
{
    ASSERT(offset + sizeof(Word) <= text.size());
    return WTF::unalignedLoad<Word>(text.data() + offset);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

// One to eight, with zeros above them.
static ALWAYS_INLINE uint64_t loadFewCharacters(std::span<const Latin1Character> text)
{
    size_t size = text.size();
    if (size >= 4)
        return loadCharacters<uint32_t>(text, 0) | loadCharacters<uint32_t>(text, size - 4) << (size - 4) * 8;
    return loadCharacters<uint8_t>(text, 0) | loadCharacters<uint8_t>(text, size / 2) << size / 2 * 8 | loadCharacters<uint8_t>(text, size - 1) << (size - 1) * 8;
}

// The lowest bit that is set is the upper one of the first byte above 9.
static ALWAYS_INLINE uint64_t bytesAbove9(uint64_t bytes)
{
    return ((bytes + eachByte * 0x76) | bytes) & eachByte * 0x80;
}

Decimal128 Decimal128::parseOrNoValue(StringView view)
{
    // Up to eight digits and a point, with no branch on where the point is or on how many zeros are at the end.
    if (!view.is8Bit() || view.isEmpty() || std::endian::native != std::endian::little) [[unlikely]]
        return parseSlow(view);
    bool isNegative = view.span8()[0] == '-';
    auto text = view.span8().subspan(isNegative);
    if (text.size() - 1 >= 8)
        return parseByWords(view);

    // The first byte that is no digit goes. If it is neither the point nor behind the text, a digit is missing at the end.
    uint64_t bytes = loadFewCharacters(text) ^ eachByte * '0';
    uint64_t others = bytesAbove9(bytes);
    if (!others) [[unlikely]]
        return parseByWords(view);
    unsigned shiftOfOther = std::countr_zero(others) & ~7;
    uint64_t beforeOther = (others ^ (others - 1)) >> 8;
    unsigned numberOfDigits = text.size() - (static_cast<uint8_t>(bytes >> shiftOfOther) == ('.' ^ '0'));
    uint64_t digits = ((bytes & beforeOther) | ~(~bytes >> 8 | beforeOther)) << (-numberOfDigits * 8 & 63);
    if (bytesAbove9(digits) || !digits) [[unlikely]]
        return parseByWords(view);

    unsigned shiftOfZeros = std::countl_zero(digits) & ~7;
    Decimal128 result { static_cast<int>((shiftOfZeros + shiftOfOther) / 8 - numberOfDigits), isNegative };
    result.m_low = valueOfEightDigits(digits << shiftOfZeros);
    return result;
}

// Eight characters at a time, if the digits fit in 128 bits.
NEVER_INLINE Decimal128 Decimal128::parseByWords(StringView view)
{
    auto text = view.span8();
    size_t end = text.size();
    // More than the digits, a sign, a point and an exponent of the domain take.
    if (end > maximumShortPower + 8) [[unlikely]]
        return parseSlow(view);
    bool isNegative = text[0] == '-';
    size_t position = isNegative || text[0] == '+';
    bool isShort = end < 8;
    uint64_t lastWord = isShort ? loadFewCharacters(text) << (8 - end) * 8 : loadCharacters<uint64_t>(text, end - 8);

    uint64_t high = 0;
    uint64_t low = 0;
    auto append = [&](uint64_t digits, unsigned count) ALWAYS_INLINE_LAMBDA {
        UInt128 lower = multiply64(low, powersOf10In64Bits[count]) + valueOfEightDigits(digits);
        high = high * powersOf10In64Bits[count] + highOf(lower);
        low = lowOf(lower);
        position += count;
    };
    auto parseDigits = [&] ALWAYS_INLINE_LAMBDA {
        size_t start = position;
        uint64_t bytes;
        uint64_t others;
        while (true) {
            size_t left = end - position;
            bytes = (left >= 8 ? loadCharacters<uint64_t>(text, position) : lastWord >> 1 >> (63 - left * 8)) ^ eachByte * '0';
            others = bytesAbove9(bytes);
            if (others)
                break;
            append(bytes, 8);
        }
        unsigned count = std::countr_zero(others) / 8;
        append(bytes << 1 << (63 - count * 8), count);
        return position - start;
    };
    size_t wholeDigits = parseDigits();
    size_t fractionalDigits = 0;
    if (position != end && text[position] == '.') {
        ++position;
        fractionalDigits = parseDigits();
    }
    auto exponent = parseExponent(text, position);
    if (wholeDigits + fractionalDigits - 1 >= maximumShortPower || !exponent) [[unlikely]]
        return parseSlow(view);
    if (!(high | low))
        return zero(isNegative);
    return roundToDomain(isNegative, high, low, *exponent - static_cast<int64_t>(fractionalDigits), false);
}

NEVER_INLINE Decimal128 Decimal128::parseSlow(StringView text)
{
    if (auto result = text.is8Bit() ? parseByDigits(text.span8()) : parseByDigits(text.span16()))
        return *result;
    if (text == "NaN"_s)
        return nan();
    if (text == "Infinity"_s || text == "+Infinity"_s)
        return infinity(false);
    if (text == "-Infinity"_s)
        return infinity(true);
    return noValue();
}

auto Decimal128::tables() -> const Tables&
{
    return decimalTables;
}

Decimal128 Decimal128::withoutTrailingZerosSlow(Decimal128 value)
{
    return fromExact(value.m_isNegative, 0, value.m_low, value.m_exponent);
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
    if (x.m_high | y.m_high)
        return addSlow(x, y);
    // As DecimalFastPaths does it. NaN and infinity are far from any finite value.
    if (unsigned index = x.m_exponent - y.m_exponent + reachOfAlignments; index <= 2 * reachOfAlignments && x.m_exponent <= maximumQuantumExponent) [[likely]] {
        auto& alignment = decimalTables.alignments[index];
        if (x.m_low <= alignment.limit && y.m_low <= alignment.otherLimit) [[likely]] {
            uint64_t signsDiffer = -static_cast<uint64_t>(x.m_isNegative ^ y.m_isNegative);
            uint64_t sum = x.m_low * alignment.factor + ((y.m_low * alignment.otherFactor ^ signsDiffer) - signsDiffer);
            uint64_t isReversed = static_cast<int64_t>(sum) >> 63;
            if (sum) [[likely]]
                return fromExact(x.m_isNegative ^ (isReversed & 1), 0, (sum ^ isReversed) - isReversed, std::min(x.m_exponent, y.m_exponent));
        }
    }
    return addSlow(x, y);
}

static NEVER_INLINE Decimal128 addWithoutCoefficient(Decimal128 x, Decimal128 y)
{
    if (x.isNaN() || y.isNaN())
        return Decimal128::nan();
    if (x.isInfinity())
        return y.isInfinity() && x.isNegative() != y.isNegative() ? Decimal128::nan() : x;
    if (y.isInfinity())
        return y;
    if (x.isZero())
        return y.isZero() ? Decimal128::zero(x.isNegative() && y.isNegative()) : y.withoutTrailingZeros();
    return x.withoutTrailingZeros();
}

NEVER_INLINE Decimal128 Decimal128::addSlow(Decimal128 upper, Decimal128 lower)
{
    if (!(upper.m_low | upper.m_high) || !(lower.m_low | lower.m_high)) [[unlikely]]
        return addWithoutCoefficient(upper, lower);

    UInt128 upperValue = makeUInt128(upper.m_high, upper.m_low);
    if (int difference = upper.m_exponent - lower.m_exponent) {
        auto x = upper.words();
        auto y = lower.words();
        bool isReversed = difference < 0;
        upper = fromWords(isReversed ? y[0] : x[0], isReversed ? y[1] : x[1]);
        lower = fromWords(isReversed ? x[0] : y[0], isReversed ? x[1] : y[1]);
        upperValue = makeUInt128(upper.m_high, upper.m_low);
        difference = std::abs(difference);
        // With 37 digits in units of the lower value the upper one is less than 10^37, and the sum than 2^128.
        if (JSC::numberOfDigits(upperValue) + difference > 37)
            return addFarApart(upper, lower);
        upperValue = upperValue * powersOf10In128Bits[difference];
    }
    UInt128 lowerValue = makeUInt128(lower.m_high, lower.m_low);
    if (upper.m_isNegative == lower.m_isNegative) {
        UInt128 sum = upperValue + lowerValue;
        return roundToDomain(upper.m_isNegative, highOf(sum), lowOf(sum), lower.m_exponent, false);
    }
    uint64_t isLess = -static_cast<uint64_t>(upperValue < lowerValue);
    UInt128 rest = ((upperValue - lowerValue) ^ makeUInt128(isLess, isLess)) + (isLess & 1);
    return roundToDomain(upper.m_isNegative ^ (isLess & 1), highOf(rest), lowOf(rest), lower.m_exponent, false);
}

// In units of the lower value the upper one has more digits than the precision.
NEVER_INLINE Decimal128 Decimal128::addFarApart(Decimal128 upper, Decimal128 lower)
{
    UInt128 upperValue = makeUInt128(upper.m_high, upper.m_low);
    int difference = upper.m_exponent - lower.m_exponent;
    // 10^76 + 10^34 is less than 2^256. With more digits the lower value is less than 10^-42 of the upper one, which is the nearest then.
    if (JSC::numberOfDigits(upperValue) + difference >= Wide::maximumPowerOf10)
        return upper.withoutTrailingZeros();
    if (difference > maximumShortPower) {
        upperValue = upperValue * powersOf10In128Bits[difference - maximumShortPower];
        difference = maximumShortPower;
    }
    Wide scaled = Wide::product(upperValue, powersOf10In128Bits[difference]);
    return roundToDomain(upper.m_isNegative, upper.m_isNegative == lower.m_isNegative ? scaled + lower.coefficient() : scaled - lower.coefficient(), lower.m_exponent);
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.subtract
Decimal128 Decimal128::subtract(Decimal128 x, Decimal128 y)
{
    // The sum does not look at the sign of NaN.
    y.m_isNegative ^= 1;
    return add(x, y);
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
    return roundToDomain(isNegative, Wide::product(makeUInt128(x.m_high, x.m_low), makeUInt128(y.m_high, y.m_low)), x.m_exponent + y.m_exponent);
}

// The quotient has as many digits as the precision.
ALWAYS_INLINE Decimal128 Decimal128::roundQuotient(bool isNegative, const DecimalQuotient& division, int64_t exponent)
{
    UInt128 value = division.quotient;
    ASSERT(JSC::numberOfDigits(value) == precision);
    if (exponent >= minimumQuantumExponent && exponent <= maximumQuantumExponent) [[likely]] {
        // No quotient is nearer to the next power of 10 than one unit.
        value = value + isNearerToNext(division.remainder, division.rest, lowOf(value) & 1);
        return fromExact(isNegative, highOf(value), lowOf(value), exponent);
    }
    return roundToDomainSlow(isNegative, highOf(value), lowOf(value), exponent, !!division.remainder);
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.divide
Decimal128 Decimal128::divide(Decimal128 x, Decimal128 y)
{
    // Both in 64 bits and not 0. NaN and infinity have a coefficient of 0.
    if ((x.m_high | y.m_high) || !x.m_low || !y.m_low) [[unlikely]]
        return divideSlow(x, y);

    bool isNegative = x.m_isNegative != y.m_isNegative;
    int64_t exponent = static_cast<int64_t>(x.m_exponent) - y.m_exponent;
    uint64_t divisor = y.m_low;
    if (divisor == 1)
        return roundToDomain(isNegative, 0, x.m_low, exponent, false);

    // A long division. The first step gives 18 or 19 digits, and the second one the rest.
    constexpr int step = 19;
    int power = step - 1 - leastNumberOfDigits(std::countl_zero(divisor) - std::countl_zero(x.m_low) + 1);
    UInt128 factor = powersOf10In128Bits[power];
    UInt128 dividend = multiply64(x.m_low, lowOf(factor));
    auto upper = divide128By64(highOf(dividend) + x.m_low * highOf(factor), lowOf(dividend), divisor);
    exponent -= power;
    if (!upper.remainder)
        return roundToDomain(isNegative, 0, upper.quotient, exponent, false);

    power = precision - step + (upper.quotient < powersOf10In64Bits[step - 1]);
    dividend = multiply64(upper.remainder, powersOf10In64Bits[power]);
    auto lower = divide128By64(highOf(dividend), lowOf(dividend), divisor);
    UInt128 quotient = multiply64(upper.quotient, powersOf10In64Bits[power]) + lower.quotient;
    return roundQuotient(isNegative, { quotient, lower.remainder, divisor - lower.remainder }, exponent - power);
}

NEVER_INLINE Decimal128 Decimal128::divideSlow(Decimal128 x, Decimal128 y)
{
    bool isNegative = x.m_isNegative != y.m_isNegative;
    if (!(x.m_low | x.m_high) || !(y.m_low | y.m_high)) [[unlikely]] {
        if (x.isNaN() || y.isNaN())
            return nan();
        if (x.isInfinity())
            return y.isInfinity() ? nan() : infinity(isNegative);
        if (y.isInfinity())
            return zero(isNegative);
        if (y.isZero())
            return x.isZero() ? nan() : infinity(isNegative);
        return zero(isNegative);
    }

    UInt128 value = makeUInt128(x.m_high, x.m_low);
    UInt128 divisor = makeUInt128(y.m_high, y.m_low);
    // The exponent of the quotient of the coefficients in scientific notation is this or one less.
    int magnitude = leastNumberOfDigits(numberOfBits(value) - numberOfBits(divisor) + 1);
    magnitude -= magnitude >= 0 ? value < divisor * powersOf10In128Bits[magnitude] : value * powersOf10In128Bits[-magnitude] < divisor;
    int power = precision - 1 - magnitude;
    int64_t exponent = static_cast<int64_t>(x.m_exponent) - y.m_exponent - power;
    // The dividend has at most 68 digits.
    if (power > maximumShortPower) {
        value = value * powersOf10In128Bits[power - maximumShortPower];
        power = maximumShortPower;
    }
    Wide dividend = Wide::product(value, powersOf10In128Bits[power]);
    if (y.m_high)
        return roundQuotient(isNegative, dividend.dividedBy(makeLongDivisor(divisor)), exponent);
    return roundQuotient(isNegative, dividend.dividedBy(y.m_low, [&](uint64_t high, uint64_t low) { return divide128By64(high, low, y.m_low); }), exponent);
}

Decimal128 Decimal128::divideAndRound(Decimal128 x, Decimal128 y, int32_t numberOfFractionalDigits, RoundingMode mode)
{
    // What is left of a quotient by less than 2^64 is 0, or a half, or not within 10^-20 of these and of 1. To round it to 20 digits or more
    // first changes nothing.
    constexpr uint64_t limit = powersOf10In64Bits[precision - 20];
    int64_t unit = -static_cast<int64_t>(numberOfFractionalDigits);
    int64_t power = static_cast<int64_t>(x.m_exponent) - y.m_exponent - unit;
    constexpr int reach = 19;
    if (!(x.m_high | y.m_high) && x.m_low && y.m_low && static_cast<uint64_t>(power + reach) <= 2 * reach && unit >= minimumQuantumExponent + 20 && unit <= maximumQuantumExponent) [[likely]] {
        UInt128 dividend = multiply64(x.m_low, powersOf10In64Bits[std::max<int64_t>(power, 0)]);
        UInt128 divisor = multiply64(y.m_low, powersOf10In64Bits[std::max<int64_t>(-power, 0)]);
        if (!highOf(divisor) && highOf(dividend) < lowOf(divisor)) [[likely]] {
            auto [count, remainder] = divide128By64(highOf(dividend), lowOf(dividend), lowOf(divisor));
            if (count < limit) [[likely]] {
                bool isNegative = x.m_isNegative != y.m_isNegative;
                count += magnitudeRoundsUp(mode, isNegative, remainder, lowOf(divisor) - remainder, count & 1);
                return roundToDomain(isNegative, 0, count, unit, false);
            }
        }
    }
    return round(divide(x, y), numberOfFractionalDigits, mode);
}

std::optional<Decimal128> Decimal128::exactReciprocal() const
{
    if (m_high || !m_low)
        return std::nullopt;
    Decimal128 result = divide(one(), *this);
    int power = -(m_exponent + result.m_exponent);
    if (result.m_high || power < 0 || power > maximumShortPower || multiply64(m_low, result.m_low) != powersOf10In128Bits[power])
        return std::nullopt;
    return result;
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.remainder
Decimal128 Decimal128::remainder(Decimal128 x, Decimal128 y)
{
    // Both in 64 bits and not 0, and so in one unit. NaN and infinity have a coefficient of 0.
    if (unsigned index = x.m_exponent - y.m_exponent + reachOfAlignments; !(x.m_high | y.m_high) && x.m_low && y.m_low && index <= 2 * reachOfAlignments) [[likely]] {
        auto& alignment = decimalTables.alignments[index];
        if (x.m_low <= alignment.limit && y.m_low <= alignment.otherLimit) [[likely]] {
            uint64_t rest = x.m_low * alignment.factor % (y.m_low * alignment.otherFactor);
            return rest ? fromExact(x.m_isNegative, 0, rest, std::min(x.m_exponent, y.m_exponent)) : zero(x.m_isNegative);
        }
    }
    return remainderSlow(x, y);
}

// Warren, Hacker's Delight, 9-5: the estimate less one is the quotient or one less.
static ALWAYS_INLINE UInt128 remainderOf(UInt128 dividend, UInt128 divisor)
{
    if (!highOf(divisor))
        return divide128By64(highOf(dividend) % lowOf(divisor), lowOf(dividend), lowOf(divisor)).remainder;
    unsigned shift = std::countl_zero(highOf(divisor));
    uint64_t top = highOf(divisor) << shift | lowOf(divisor) >> 1 >> (63 - shift);
    uint64_t quotient = divide128By64(highOf(dividend) >> 1, lowOf(dividend >> 1), top).quotient >> (63 - shift);
    quotient -= !!quotient;
    UInt128 product = multiply64(quotient, lowOf(divisor));
    UInt128 rest = dividend - makeUInt128(highOf(product) + quotient * highOf(divisor), lowOf(product));
    uint64_t isTooLarge = -static_cast<uint64_t>(rest >= divisor);
    return rest - makeUInt128(highOf(divisor) & isTooLarge, lowOf(divisor) & isTooLarge);
}

NEVER_INLINE Decimal128 Decimal128::remainderSlow(Decimal128 x, Decimal128 y)
{
    if (x.isNaN() || y.isNaN() || x.isInfinity() || y.isZero())
        return nan();
    if (y.isInfinity() || x.isZero())
        return x.withoutTrailingZeros();

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
            return x.withoutTrailingZeros();
        UInt128 modulus = distance ? yCoefficient * powersOf10In128Bits[-distance] : yCoefficient;
        return xCoefficient < modulus ? x.withoutTrailingZeros() : finish(remainderOf(xCoefficient, modulus));
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
    if (!distance && x.m_isNegative == y.m_isNegative && !x.isNaN())
        return order(makeUInt128(x.m_high, x.m_low), makeUInt128(y.m_high, y.m_low), x.isNegative());
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
        if (auto order = x.scientificExponent() <=> y.scientificExponent(); order != 0)
            return order;
        // The same scientific exponent: in the lower unit both have as many digits.
        int distance = x.m_exponent - y.m_exponent;
        UInt128 xValue = makeUInt128(x.m_high, x.m_low) * powersOf10In128Bits[std::max(distance, 0)];
        UInt128 yValue = makeUInt128(y.m_high, y.m_low) * powersOf10In128Bits[std::max(-distance, 0)];
        return (xValue > yValue) - (xValue < yValue) <=> 0;
    };
    return sign > 0 ? compareMagnitudes(x, y) : compareMagnitudes(y, x);
}

// The coefficient in units of 10^power. Answers it through the references.
ALWAYS_INLINE void Decimal128::roundCoefficient(int64_t power, RoundingMode mode, uint64_t& high, uint64_t& low) const
{
    ASSERT(power > 0 && isFinite() && !isZero());
    // Of fewer digits than the power, so less than a tenth.
    DecimalQuotient division { 0, 1, 9 };
    if (power <= maximumShortPower) [[likely]]
        division = divideByPowerOf10(makeUInt128(m_high, m_low), static_cast<int>(power));
    UInt128 count = division.quotient + magnitudeRoundsUp(mode, m_isNegative, division.remainder, division.rest, lowOf(division.quotient) & 1);
    high = highOf(count);
    low = lowOf(count);
}

// https://tc39.es/proposal-decimal/#sec-decimal-method-round
Decimal128 Decimal128::round(Decimal128 x, int32_t numberOfFractionalDigits, RoundingMode mode)
{
    int64_t unit = -static_cast<int64_t>(numberOfFractionalDigits);
    // NaN and infinity have a coefficient of 0.
    if (unit <= x.m_exponent || !(x.m_low | x.m_high))
        return x.withoutTrailingZeros();
    // The spec rounds to the domain in the same mode, but nothing is left to round: the count is at most 10^34, and the unit is in the domain or overflows.
    uint64_t high;
    uint64_t low;
    x.roundCoefficient(unit - x.m_exponent, mode, high, low);
    return roundToDomain(x.m_isNegative, high, low, unit, false);
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
