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

// The product of two very long coefficients.
//
// The limbs of a product, before carries, are the convolution of the limbs of the factors. A convolution is a pointwise product between
// Fourier transforms, which take n log n steps. Here the transforms are over the integers modulo a prime p with 2**40 dividing p - 1, where
// there are roots of unity of every order 2**k that is needed, and nothing is rounded. A sum of n products of limbs is below n * 10**38,
// which is more than one such prime can tell apart: the convolution is computed modulo three of them, and the Chinese remainder theorem
// gives the sums.

struct TransformPrime {
    uint64_t modulus; // Between 10**19 / 2 and 2**63.
    uint64_t generator; // Of the multiplicative group.
};
static constexpr TransformPrime transformPrimes[3] = {
    { 0x7ffffe0000000001ull, 7 },
    { 0x7fffef0000000001ull, 5 },
    { 0x7fffe90000000001ull, 7 },
};
static constexpr int maxLog2OfTransform = 40;

// Arithmetic modulo a prime. Where it says so, a number x is kept as x * 2**64 ("scaled"): the product of a scaled number and a plain one is plain.
class Modular {
public:
    explicit Modular(uint64_t modulus)
        : p(modulus)
    {
        // Newton's iteration doubles the number of bits that are right. 3 * p ^ 2 has five.
        uint64_t inverse = (3 * p) ^ 2;
        for (int i = 0; i < 4; ++i)
            inverse *= 2 - p * inverse;
        m_negatedInverse = -inverse;
        scaledOne = -p % p;
        Limb remainder;
        unsigned __int128 square = static_cast<unsigned __int128>(scaledOne) * scaledOne;
        divideWide(static_cast<uint64_t>(square >> 64), static_cast<uint64_t>(square), p, remainder);
        m_scaledTwice = remainder;
    }

    // For a difference between -p and p. Without a branch: which way it would go is as good as random.
    PY_DECIMAL_INLINE uint64_t addModulusIfNegative(uint64_t difference) const { return difference + (p & static_cast<uint64_t>(static_cast<int64_t>(difference) >> 63)); }
    PY_DECIMAL_INLINE uint64_t reduce(uint64_t belowTwiceTheModulus) const { return addModulusIfNegative(belowTwiceTheModulus - p); }
    PY_DECIMAL_INLINE uint64_t add(uint64_t a, uint64_t b) const { return reduce(a + b); }
    PY_DECIMAL_INLINE uint64_t subtract(uint64_t a, uint64_t b) const { return addModulusIfNegative(a - b); }
    // a * b / 2**64 (Montgomery)
    PY_DECIMAL_INLINE uint64_t multiply(uint64_t a, uint64_t b) const
    {
        unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
        uint64_t factor = static_cast<uint64_t>(product) * m_negatedInverse;
        return reduce(static_cast<uint64_t>((product + static_cast<unsigned __int128>(factor) * p) >> 64));
    }
    uint64_t scale(uint64_t plain) const { return multiply(plain, m_scaledTwice); }
    // Both the base and the result are scaled.
    uint64_t power(uint64_t base, uint64_t exponent) const
    {
        uint64_t result = scaledOne;
        for (; exponent; exponent >>= 1) {
            if (exponent & 1)
                result = multiply(result, base);
            base = multiply(base, base);
        }
        return result;
    }
    uint64_t inverse(uint64_t scaled) const { return power(scaled, p - 2); }

    uint64_t p;
    uint64_t scaledOne;

private:
    uint64_t m_negatedInverse;
    uint64_t m_scaledTwice;
};

// The powers of the roots of unity that a transform of length n multiplies by, scaled: for each half length h = n/2, n/4, ... 1, one after the
// other, the first h powers of a root of order 2h. That is n - 1 numbers.
static void fillTwiddles(const Modular& field, uint64_t generator, int log2Length, uint64_t* twiddles)
{
    uint64_t length = static_cast<uint64_t>(1) << log2Length;
    uint64_t root = field.power(field.scale(generator), (field.p - 1) >> log2Length);
    uint64_t half = length / 2;
    uint64_t current = field.scaledOne;
    for (uint64_t j = 0; j < half; ++j) {
        twiddles[j] = current;
        current = field.multiply(current, root);
    }
    // Every other one of a level is the next level.
    const uint64_t* previous = twiddles;
    for (uint64_t h = half / 2; h; h /= 2) {
        uint64_t* level = const_cast<uint64_t*>(previous) + 2 * h;
        for (uint64_t j = 0; j < h; ++j)
            level[j] = previous[2 * j];
        previous = level;
    }
}

// The result is in bit-reversed order, which is the order that the way back starts from.
static void transformForward(const Modular& field, uint64_t* data, uint64_t length, const uint64_t* twiddles)
{
    for (uint64_t half = length / 2; half; twiddles += half, half /= 2) {
        for (uint64_t start = 0; start < length; start += 2 * half) {
            uint64_t* low = data + start;
            uint64_t* high = low + half;
            for (uint64_t j = 0; j < half; ++j) {
                uint64_t u = low[j];
                uint64_t v = high[j];
                low[j] = field.add(u, v);
                high[j] = field.multiply(field.subtract(u, v), twiddles[j]);
            }
        }
    }
}

// Without the division by the length.
static void transformBackward(const Modular& field, uint64_t* data, uint64_t length, const uint64_t* twiddles)
{
    const uint64_t* level = twiddles + length - 1;
    for (uint64_t half = 1; half < length; half *= 2) {
        level -= half;
        for (uint64_t start = 0; start < length; start += 2 * half) {
            uint64_t* low = data + start;
            uint64_t* high = low + half;
            // The inverse of the power j of a root of order 2 * half is minus its power half - j.
            uint64_t u = low[0];
            uint64_t v = high[0];
            low[0] = field.add(u, v);
            high[0] = field.subtract(u, v);
            for (uint64_t j = 1; j < half; ++j) {
                u = low[j];
                v = field.multiply(high[j], level[half - j]);
                low[j] = field.subtract(u, v);
                high[j] = field.add(u, v);
            }
        }
    }
}

static void loadForTransform(const Modular& field, uint64_t* data, uint64_t length, const Limb* limbs, int64_t count)
{
    for (int64_t i = 0; i < count; ++i)
        data[i] = field.reduce(limbs[i]);
    std::fill(data + count, data + length, 0);
}

// The longer factor is taken in pieces: each piece times the shorter factor fits one transform. Lengths are powers of two, so one piece is
// not always the best, even for factors of the same length.
struct TransformPlan {
    int log2Length { 0 };
    int64_t pieceLength { 0 };
    int64_t pieceCount { 0 };
    double nanoseconds { std::numeric_limits<double>::infinity() }; // Roughly, as measured.
};

static TransformPlan planTransform(int64_t bigLength, int64_t smallLength)
{
    TransformPlan best;
    int log2Length = 1;
    while ((static_cast<int64_t>(1) << log2Length) <= smallLength)
        ++log2Length;
    for (; log2Length <= maxLog2OfTransform; ++log2Length) {
        int64_t length = static_cast<int64_t>(1) << log2Length;
        int64_t pieceLength = std::min(bigLength, length - smallLength);
        int64_t pieceCount = (bigLength + pieceLength - 1) / pieceLength;
        double points = static_cast<double>(length);
        double pieces = static_cast<double>(pieceCount);
        double nanoseconds = (1 + 2 * pieces) * 2.85 * points * log2Length + (9 * pieces + 6) * points + 15 * pieces * static_cast<double>(pieceLength + smallLength);
        if (nanoseconds < best.nanoseconds)
            best = { log2Length, pieceLength, pieceCount, nanoseconds };
        if (pieceCount == 1)
            break;
    }
    return best;
}

bool transformIsFaster(int64_t bigLength, int64_t smallLength)
{
    double karatsuba = 14.1 * std::pow(static_cast<double>(smallLength), 0.585) * static_cast<double>(bigLength);
    return planTransform(bigLength, smallLength).nanoseconds < karatsuba;
}

bool multiplyByTransform(Limb* product, const Limb* big, int64_t bigLength, const Limb* small, int64_t smallLength)
{
    TransformPlan plan = planTransform(bigLength, smallLength);
    if (!plan.pieceCount)
        return false;
    int log2Length = plan.log2Length;
    int64_t length = static_cast<int64_t>(1) << log2Length;
    int64_t pieceLength = plan.pieceLength;
    int64_t pieceCount = plan.pieceCount;
    bool isSquare = big == small && bigLength == smallLength && pieceCount == 1;

    // The twiddles, the transform of the shorter factor, and for each prime the convolutions of all pieces.
    Limb* memory = allocateLimbs(2 * length + 3 * pieceCount * length + (pieceCount > 1 ? pieceLength + smallLength : 0));
    if (!memory)
        return false;
    uint64_t* twiddles = memory;
    uint64_t* other = twiddles + length;
    uint64_t* residues[3];
    for (int i = 0; i < 3; ++i)
        residues[i] = other + length + i * pieceCount * length;
    Limb* partial = pieceCount > 1 ? residues[2] + pieceCount * length : product;

    for (int i = 0; i < 3; ++i) {
        Modular field(transformPrimes[i].modulus);
        fillTwiddles(field, transformPrimes[i].generator, log2Length, twiddles);
        // What undoes the scaling of one product and divides by the length.
        uint64_t correction = field.multiply(field.scale(field.scaledOne), field.inverse(field.scale(static_cast<uint64_t>(length))));
        if (!isSquare) {
            loadForTransform(field, other, length, small, smallLength);
            transformForward(field, other, length, twiddles);
            for (int64_t j = 0; j < length; ++j)
                other[j] = field.multiply(other[j], correction);
        }
        for (int64_t piece = 0; piece < pieceCount; ++piece) {
            uint64_t* data = residues[i] + piece * length;
            loadForTransform(field, data, length, big + piece * pieceLength, std::min(pieceLength, bigLength - piece * pieceLength));
            transformForward(field, data, length, twiddles);
            if (isSquare) {
                for (int64_t j = 0; j < length; ++j)
                    data[j] = field.multiply(data[j], field.multiply(data[j], correction));
            } else {
                for (int64_t j = 0; j < length; ++j)
                    data[j] = field.multiply(data[j], other[j]);
            }
            transformBackward(field, data, length, twiddles);
        }
    }

    // A number below the product of the primes, from its remainders r0, r1 and r2, is x0 + p0 * (x1 + p1 * x2) with each x below its prime.
    Modular second(transformPrimes[1].modulus);
    Modular third(transformPrimes[2].modulus);
    uint64_t p0 = transformPrimes[0].modulus;
    uint64_t p1 = transformPrimes[1].modulus;
    uint64_t inverseOfFirstInSecond = second.inverse(second.scale(second.reduce(p0)));
    uint64_t inverseOfFirstInThird = third.inverse(third.scale(third.reduce(p0)));
    uint64_t inverseOfSecondInThird = third.inverse(third.scale(third.reduce(p1)));

    if (pieceCount > 1)
        std::fill(product, product + bigLength + smallLength, 0);
    for (int64_t piece = 0; piece < pieceCount; ++piece) {
        int64_t offset = piece * pieceLength;
        int64_t resultLength = std::min(pieceLength, bigLength - offset) + smallLength;
        const uint64_t* r0 = residues[0] + piece * length;
        const uint64_t* r1 = residues[1] + piece * length;
        const uint64_t* r2 = residues[2] + piece * length;
        unsigned __int128 carry = 0;
        for (int64_t j = 0; j < resultLength; ++j) {
            // The last limb is all carry: the convolution is one shorter, and the transform may be no longer than that.
            uint64_t x0 = 0;
            unsigned __int128 rest = 0;
            if (j < resultLength - 1) {
                x0 = r0[j];
                uint64_t x1 = second.multiply(second.subtract(r1[j], second.reduce(x0)), inverseOfFirstInSecond);
                uint64_t x2 = third.multiply(third.subtract(third.multiply(third.subtract(r2[j], third.reduce(x0)), inverseOfFirstInThird), third.reduce(x1)), inverseOfSecondInThird);
                rest = static_cast<unsigned __int128>(p1) * x2 + x1;
            }
            // x0 + p0 * rest + carry, in three words.
            unsigned __int128 lowProduct = static_cast<unsigned __int128>(p0) * static_cast<uint64_t>(rest);
            unsigned __int128 highProduct = static_cast<unsigned __int128>(p0) * static_cast<uint64_t>(rest >> 64);
            unsigned __int128 low = static_cast<unsigned __int128>(static_cast<uint64_t>(lowProduct)) + x0 + static_cast<uint64_t>(carry);
            unsigned __int128 high = highProduct + static_cast<uint64_t>(lowProduct >> 64) + static_cast<uint64_t>(carry >> 64) + static_cast<uint64_t>(low >> 64);
            Limb upperQuotient;
            Limb lowerQuotient;
            Limb remainder;
            divideByRadix(static_cast<uint64_t>(high >> 64), static_cast<uint64_t>(high), upperQuotient, remainder);
            divideByRadix(remainder, static_cast<uint64_t>(low), lowerQuotient, partial[j]);
            carry = (static_cast<unsigned __int128>(upperQuotient) << 64) | lowerQuotient;
        }
        if (pieceCount > 1)
            addLimbs(product + offset, product + offset, partial, bigLength + smallLength - offset, resultLength);
    }
    freeLimbs(memory);
    return true;
}

} // namespace Decimal
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
