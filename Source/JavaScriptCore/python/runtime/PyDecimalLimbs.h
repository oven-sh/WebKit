/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyDecimalNumber.h"

#ifdef PY_DECIMAL_STANDALONE
#define PY_DECIMAL_INLINE inline __attribute__((always_inline))
#define PY_DECIMAL_NEVER_INLINE __attribute__((noinline))
#else
#include <wtf/Compiler.h>
#define PY_DECIMAL_INLINE ALWAYS_INLINE
#define PY_DECIMAL_NEVER_INLINE NEVER_INLINE
#endif

namespace JSC {
namespace Python {
namespace Decimal {

// What the files of the arithmetic share: integers in base 10**19 as arrays of limbs, and the operations without their last step.

PY_DECIMAL_INLINE void multiplyWide(Limb a, Limb b, Limb& high, Limb& low)
{
    unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    high = static_cast<Limb>(product >> 64);
    low = static_cast<Limb>(product);
}

// (high * 2**64 + low) divided by 10**19, for high < 10**19. By multiplication with floor((2**128 - 1) / 10**19) - 2**64:
// Moller and Granlund, "Improved division by invariant integers", algorithm 4.
PY_DECIMAL_INLINE void divideByRadix(Limb high, Limb low, Limb& quotient, Limb& remainder)
{
    constexpr Limb reciprocal = 0xd83c94fb6d2ac34aull;
    unsigned __int128 estimate = static_cast<unsigned __int128>(reciprocal) * high;
    estimate += (static_cast<unsigned __int128>(high) << 64) | low;
    Limb q = static_cast<Limb>(estimate >> 64) + 1;
    Limb r = low - q * radix;
    if (r > static_cast<Limb>(estimate)) {
        --q;
        r += radix;
    }
    if (r >= radix) [[unlikely]] {
        ++q;
        r -= radix;
    }
    quotient = q;
    remainder = r;
}

// a * b + c + d as two limbs. All are limbs, so it fits.
PY_DECIMAL_INLINE void multiplyAdd(Limb a, Limb b, Limb c, Limb d, Limb& high, Limb& low)
{
    unsigned __int128 sum = static_cast<unsigned __int128>(a) * b + c + d;
    divideByRadix(static_cast<Limb>(sum >> 64), static_cast<Limb>(sum), high, low);
}

// (high * 2**64 + low) / divisor, for high < divisor.
PY_DECIMAL_INLINE Limb divideWide(Limb high, Limb low, Limb divisor, Limb& remainder)
{
#if defined(__x86_64__) && defined(__GNUC__) && !defined(PY_DECIMAL_WITHOUT_ASSEMBLY)
    Limb quotient;
    __asm__("divq %4" : "=a"(quotient), "=d"(remainder) : "0"(low), "1"(high), "rm"(divisor) : "cc");
    return quotient;
#else
    // Two digits of 32 bits, each estimated from the top half of the divisor and corrected. (Knuth, 4.3.1, algorithm D.)
    constexpr Limb base = 1ull << 32;
    int shift = __builtin_clzll(divisor);
    divisor <<= shift;
    Limb divisorHigh = divisor >> 32;
    Limb divisorLow = divisor & 0xffffffff;
    Limb upper = shift ? (high << shift) | (low >> (64 - shift)) : high;
    Limb lower = low << shift;
    Limb lowerHigh = lower >> 32;
    Limb lowerLow = lower & 0xffffffff;
    Limb q1 = upper / divisorHigh;
    Limb rest = upper - q1 * divisorHigh;
    while (q1 >= base || q1 * divisorLow > base * rest + lowerHigh) {
        --q1;
        rest += divisorHigh;
        if (rest >= base)
            break;
    }
    Limb middle = upper * base + lowerHigh - q1 * divisor;
    Limb q0 = middle / divisorHigh;
    rest = middle - q0 * divisorHigh;
    while (q0 >= base || q0 * divisorLow > base * rest + lowerLow) {
        --q0;
        rest += divisorHigh;
        if (rest >= base)
            break;
    }
    remainder = (middle * base + lowerLow - q0 * divisor) >> shift;
    return q1 * base + q0;
#endif
}

// high = value / 10**count, low = value % 10**count, for count <= 19. With the divisors as constants, so that none is a division.
PY_DECIMAL_INLINE void splitLimb(Limb value, unsigned count, Limb& high, Limb& low)
{
#define PY_DECIMAL_SPLIT(n, power) \
    case n: \
        high = value / power; \
        low = value - high * power; \
        return;
    switch (count) {
    case 0:
        high = value;
        low = 0;
        return;
        PY_DECIMAL_SPLIT(1, 10ull)
        PY_DECIMAL_SPLIT(2, 100ull)
        PY_DECIMAL_SPLIT(3, 1000ull)
        PY_DECIMAL_SPLIT(4, 10000ull)
        PY_DECIMAL_SPLIT(5, 100000ull)
        PY_DECIMAL_SPLIT(6, 1000000ull)
        PY_DECIMAL_SPLIT(7, 10000000ull)
        PY_DECIMAL_SPLIT(8, 100000000ull)
        PY_DECIMAL_SPLIT(9, 1000000000ull)
        PY_DECIMAL_SPLIT(10, 10000000000ull)
        PY_DECIMAL_SPLIT(11, 100000000000ull)
        PY_DECIMAL_SPLIT(12, 1000000000000ull)
        PY_DECIMAL_SPLIT(13, 10000000000000ull)
        PY_DECIMAL_SPLIT(14, 100000000000000ull)
        PY_DECIMAL_SPLIT(15, 1000000000000000ull)
        PY_DECIMAL_SPLIT(16, 10000000000000000ull)
        PY_DECIMAL_SPLIT(17, 100000000000000000ull)
        PY_DECIMAL_SPLIT(18, 1000000000000000000ull)
    default:
        high = 0;
        low = value;
        return;
    }
#undef PY_DECIMAL_SPLIT
}

PY_DECIMAL_INLINE bool limbsAreZero(const Limb* limbs, int64_t length)
{
    for (int64_t i = 0; i < length; ++i) {
        if (limbs[i])
            return false;
    }
    return true;
}

// ---- Arrays of limbs (PyDecimalNumber.cpp) ----

// sum = big + small, where big has at least as many limbs. They may be the same memory. Returns the carry.
Limb addLimbs(Limb* sum, const Limb* big, const Limb* small, int64_t bigLength, int64_t smallLength);
// difference = big - small, which is not negative.
void subtractLimbs(Limb* difference, const Limb* big, const Limb* small, int64_t bigLength, int64_t smallLength);
// Returns the carry.
Limb incrementLimbs(Limb*, int64_t length);
// -1, 0 or 1, for arrays of the same length.
int compareLimbs(const Limb*, const Limb*, int64_t length);
// product, of length bigLength + smallLength, is none of the operands. False: out of memory.
bool multiplyLimbs(Limb* product, const Limb* big, int64_t bigLength, const Limb* small, int64_t smallLength);
// The same for very long operands (PyDecimalTransform.cpp). False: there is no memory for this way of doing it, which takes more than others.
bool transformIsFaster(int64_t bigLength, int64_t smallLength);
bool multiplyByTransform(Limb* product, const Limb* big, int64_t bigLength, const Limb* small, int64_t smallLength);
// quotient has dividendLength limbs. Returns the remainder.
Limb divideLimbsByLimb(Limb* quotient, const Limb* dividend, int64_t dividendLength, Limb divisor);
// For a divisor of two limbs or more whose top limb is not zero, and a dividend that is no shorter. quotient has
// dividendLength - divisorLength + 1 limbs, remainder divisorLength or is null. Neither is an operand.
// -1: out of memory. Otherwise whether there is a remainder.
int divideLimbs(Limb* quotient, Limb* remainder, const Limb* dividend, int64_t dividendLength, const Limb* divisor, int64_t divisorLength);

// ---- Operations that leave the result as it comes out, however long (PyDecimalNumber.cpp) ----

// For finite numbers.
void addUnrounded(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void subtractUnrounded(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
// For any.
void multiplyUnrounded(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
// These give a NaN, and InvalidOperation, if the context makes them round or clamp.
void addExactly(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void subtractExactly(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void multiplyExactly(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
// `keepsAllDigits`: an exact quotient is not brought towards the ideal exponent.
void divideCore(Number& result, const Number&, const Number&, const Context&, uint32_t& status, bool keepsAllDigits);
// The integer quotient and the remainder of finite numbers, the divisor not zero. Neither is finalized.
void divideWithRemainderCore(Number& quotient, Number& remainder, const Number&, const Number&, const Context&, uint32_t& status);

// If an operand is a NaN, so is the result, and true.
bool propagateNaN(Number& result, const Number&, const Context&, uint32_t& status);
bool propagateNaN(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
bool propagateNaN(Number& result, const Number&, const Number&, const Number&, const Context&, uint32_t& status);

// As shiftCoefficientRight(), into another number. 10: out of memory.
static constexpr unsigned shiftFailed = 10;
unsigned shiftCoefficientRight(Number& result, const Number&, int64_t count, uint32_t& status);
// One more unit in the last place if the rounding mode and what was lost say so. The coefficient may get longer than the precision.
void applyRoundingFreely(Number&, unsigned lost, const Context&, uint32_t& status);

} // namespace Decimal
} // namespace Python
} // namespace JSC
