/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef PY_DECIMAL_STANDALONE
#include "config.h"
#endif
#include "PyDecimalLimbs.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#ifdef PY_DECIMAL_STANDALONE
#include <unistd.h>
#endif

#ifdef PY_DECIMAL_STANDALONE
#define WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#define WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#else
#include <wtf/FastMalloc.h>
#include <wtf/RAMSize.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Decimal {

const Limb powersOfTen[20] = {
    1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull, 100000000ull, 1000000000ull, 10000000000ull,
    100000000000ull, 1000000000000ull, 10000000000000ull, 100000000000000ull, 1000000000000000ull, 10000000000000000ull,
    100000000000000000ull, 1000000000000000000ull, 10000000000000000000ull
};

bool Context::forIEEE(int64_t bits, Context& context)
{
    if (bits <= 0 || bits > maxBitsOfIEEEContext || bits % 32)
        return false;
    context.precision = 9 * (bits / 32) - 2;
    context.emax = 3 * (static_cast<int64_t>(1) << (bits / 16 + 3));
    context.emin = 1 - context.emax;
    context.rounding = Rounding::HalfEven;
    context.traps = 0;
    context.status = 0;
    context.clamp = true;
    return true;
}

// ---- Memory ----

static thread_local int64_t limbLimit = std::numeric_limits<int64_t>::max();

LimbLimit::LimbLimit(int64_t limbs)
    : m_previous(limbLimit)
{
    limbLimit = std::min(limbLimit, limbs);
}

LimbLimit::~LimbLimit()
{
    limbLimit = m_previous;
}

int64_t mostLimbs()
{
    // The allocator would hand out addresses for more than the machine has, and the process would end on filling them.
#ifdef PY_DECIMAL_STANDALONE
    size_t memory = static_cast<size_t>(sysconf(_SC_PHYS_PAGES)) * static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
    size_t memory = WTF::ramSize();
#endif
    return std::min(static_cast<int64_t>(memory / sizeof(Limb)), limbLimit);
}

Limb* allocateLimbs(int64_t count)
{
    if (count <= 0 || count > mostLimbs())
        return nullptr;
#ifdef PY_DECIMAL_STANDALONE
    return static_cast<Limb*>(std::malloc(static_cast<size_t>(count) * sizeof(Limb)));
#else
    void* memory = nullptr;
    if (!tryFastMalloc(static_cast<size_t>(count) * sizeof(Limb)).getValue(memory))
        return nullptr;
    return static_cast<Limb*>(memory);
#endif
}

void freeLimbs(Limb* limbs)
{
#ifdef PY_DECIMAL_STANDALONE
    std::free(limbs);
#else
    fastFree(limbs);
#endif
}

// Limbs for the duration of a function: on the stack if they are few.
template<int64_t inlineCount>
class Scratch {
public:
    Scratch() = default;
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    ~Scratch()
    {
        if (m_allocated)
            freeLimbs(m_allocated);
    }
    // Null: out of memory.
    Limb* get(int64_t count)
    {
        if (count <= inlineCount)
            return m_inline;
        m_allocated = allocateLimbs(count);
        return m_allocated;
    }

private:
    Limb* m_allocated { nullptr };
    Limb m_inline[inlineCount];
};

bool Number::grow(int64_t limbCount, bool keep)
{
    // Somewhat more than is asked for, as what grows once tends to grow again.
    int64_t capacity = limbCount + (limbCount >> 3);
    Limb* limbs = allocateLimbs(capacity);
    if (!limbs) {
        capacity = limbCount;
        limbs = allocateLimbs(capacity);
        if (!limbs)
            return false;
    }
    if (keep && length)
        std::memcpy(limbs, m_limbs, static_cast<size_t>(length) * sizeof(Limb));
    if (m_capacity > inlineCapacity)
        freeLimbs(m_limbs);
    m_limbs = limbs;
    m_capacity = capacity;
    return true;
}

void Number::release()
{
    freeLimbs(m_limbs);
}

void Number::adopt(Limb* limbs, int64_t capacity)
{
    if (m_capacity > inlineCapacity)
        freeLimbs(m_limbs);
    m_limbs = limbs;
    m_capacity = capacity;
}

bool Number::copy(const Number& other, uint32_t& status)
{
    if (this == &other)
        return true;
    if (!reserveDiscarding(other.length, status))
        return false;
    flags = other.flags;
    exponent = other.exponent;
    digits = other.digits;
    length = other.length;
    if (length)
        std::memcpy(m_limbs, other.m_limbs, static_cast<size_t>(length) * sizeof(Limb));
    return true;
}

void Number::swap(Number& other)
{
    bool isInline = m_capacity <= inlineCapacity;
    bool otherIsInline = other.m_capacity <= inlineCapacity;
    Limb* heapLimbs = m_limbs;
    Limb saved[inlineCapacity];
    if (isInline)
        std::memcpy(saved, m_inline, sizeof(saved));
    if (otherIsInline) {
        std::memcpy(m_inline, other.m_inline, sizeof(m_inline));
        m_limbs = m_inline;
    } else
        m_limbs = other.m_limbs;
    if (isInline) {
        std::memcpy(other.m_inline, saved, sizeof(saved));
        other.m_limbs = other.m_inline;
    } else
        other.m_limbs = heapLimbs;
    std::swap(m_capacity, other.m_capacity);
    std::swap(flags, other.flags);
    std::swap(exponent, other.exponent);
    std::swap(digits, other.digits);
    std::swap(length, other.length);
}

// ---- What a number is like ----

int64_t Number::trailingZeros() const
{
    for (int64_t i = 0; i < length; ++i) {
        Limb limb = m_limbs[i];
        if (!limb)
            continue;
        int64_t count = i * digitsPerLimb;
        while (!(limb % 10)) {
            limb /= 10;
            ++count;
        }
        return count;
    }
    return 0;
}

bool Number::isOddInteger() const
{
    if (hasZeroCoefficient())
        return false;
    if (exponent < 0) {
        int64_t index = -exponent / digitsPerLimb;
        return (m_limbs[index] / powersOfTen[-exponent % digitsPerLimb]) & 1;
    }
    return !exponent && (m_limbs[0] & 1);
}

bool Number::coefficientIsPowerOfTen() const
{
    return top() == powersOfTen[digitCount(top()) - 1] && limbsAreZero(m_limbs, length - 1);
}

bool Number::coefficientIsAllNines() const
{
    if (top() != powersOfTen[digitCount(top())] - 1)
        return false;
    for (int64_t i = 0; i < length - 1; ++i) {
        if (m_limbs[i] != radix - 1)
            return false;
    }
    return true;
}

// ---- Arrays of limbs ----

Limb addLimbs(Limb* sum, const Limb* big, const Limb* small, int64_t bigLength, int64_t smallLength)
{
    Limb carry = 0;
    int64_t i = 0;
    for (; i < smallLength; ++i) {
        // Twice 10**19 is more than 2**64: what wraps around comes out right all the same once 10**19 is taken off.
        Limb s = big[i] + (small[i] + carry);
        carry = (s < big[i]) | (s >= radix);
        sum[i] = carry ? s - radix : s;
    }
    for (; carry && i < bigLength; ++i) {
        Limb s = big[i] + 1;
        carry = s == radix;
        sum[i] = carry ? 0 : s;
    }
    if (sum != big) {
        for (; i < bigLength; ++i)
            sum[i] = big[i];
    }
    return carry;
}

void subtractLimbs(Limb* difference, const Limb* big, const Limb* small, int64_t bigLength, int64_t smallLength)
{
    Limb borrow = 0;
    int64_t i = 0;
    for (; i < smallLength; ++i) {
        Limb d = big[i] - (small[i] + borrow);
        borrow = big[i] < d;
        difference[i] = borrow ? d + radix : d;
    }
    for (; borrow && i < bigLength; ++i) {
        Limb d = big[i] - 1;
        borrow = !big[i];
        difference[i] = borrow ? radix - 1 : d;
    }
    if (difference != big) {
        for (; i < bigLength; ++i)
            difference[i] = big[i];
    }
}

Limb incrementLimbs(Limb* limbs, int64_t length)
{
    for (int64_t i = 0; i < length; ++i) {
        if (++limbs[i] != radix)
            return 0;
        limbs[i] = 0;
    }
    return 1;
}

int compareLimbs(const Limb* a, const Limb* b, int64_t length)
{
    for (int64_t i = length - 1; i >= 0; --i) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

// target += addend, where the sum fits the target.
static void addToLimbs(Limb* target, int64_t targetLength, const Limb* addend, int64_t addendLength)
{
    while (addendLength && !addend[addendLength - 1])
        --addendLength;
    addLimbs(target, target, addend, targetLength, addendLength);
}

static void subtractFromLimbs(Limb* target, int64_t targetLength, const Limb* subtrahend, int64_t subtrahendLength)
{
    while (subtrahendLength && !subtrahend[subtrahendLength - 1])
        --subtrahendLength;
    subtractLimbs(target, target, subtrahend, targetLength, subtrahendLength);
}

static void multiplyLimbsByLimb(Limb* product, const Limb* limbs, int64_t length, Limb factor)
{
    Limb carry = 0;
    for (int64_t i = 0; i < length; ++i)
        multiplyAdd(limbs[i], factor, carry, 0, carry, product[i]);
    product[length] = carry;
}

static void multiplySchoolbook(Limb* product, const Limb* big, int64_t bigLength, const Limb* small, int64_t smallLength)
{
    multiplyLimbsByLimb(product, big, bigLength, small[0]);
    for (int64_t j = 1; j < smallLength; ++j) {
        Limb factor = small[j];
        Limb carry = 0;
        Limb* row = product + j;
        for (int64_t i = 0; i < bigLength; ++i)
            multiplyAdd(big[i], factor, row[i], carry, carry, row[i]);
        row[bigLength] = carry;
    }
}

static constexpr int64_t karatsubaThreshold = 28;
static constexpr int64_t transformThreshold = 96; // Below this there is no need to ask.

static constexpr int64_t karatsubaScratchSize(int64_t length) { return 6 * length + 512; }

// Both operands have `length` limbs. (Karatsuba: three products of half the length in place of four.)
static void multiplyKaratsuba(Limb* product, const Limb* a, const Limb* b, int64_t length, Limb* scratch)
{
    if (length < karatsubaThreshold) {
        multiplySchoolbook(product, a, length, b, length);
        return;
    }
    int64_t low = length / 2;
    int64_t high = length - low;
    multiplyKaratsuba(product, a, b, low, scratch);
    multiplyKaratsuba(product + 2 * low, a + low, b + low, high, scratch);

    int64_t sumLength = high + 1;
    Limb* sumA = scratch;
    Limb* sumB = sumA + sumLength;
    Limb* middle = sumB + sumLength;
    Limb* rest = middle + 2 * sumLength;
    sumA[high] = addLimbs(sumA, a + low, a, high, low);
    sumB[high] = addLimbs(sumB, b + low, b, high, low);
    multiplyKaratsuba(middle, sumA, sumB, sumLength, rest);
    subtractFromLimbs(middle, 2 * sumLength, product, 2 * low);
    subtractFromLimbs(middle, 2 * sumLength, product + 2 * low, 2 * high);
    addToLimbs(product + low, 2 * length - low, middle, 2 * sumLength);
}

bool multiplyLimbs(Limb* product, const Limb* big, int64_t bigLength, const Limb* small, int64_t smallLength)
{
    if (smallLength < karatsubaThreshold) {
        multiplySchoolbook(product, big, bigLength, small, smallLength);
        return true;
    }
    if (smallLength >= transformThreshold && transformIsFaster(bigLength, smallLength) && multiplyByTransform(product, big, bigLength, small, smallLength))
        return true;
    Scratch<1> memory;
    Limb* scratch = memory.get(karatsubaScratchSize(smallLength) + 2 * smallLength);
    if (!scratch)
        return false;
    if (bigLength == smallLength) {
        multiplyKaratsuba(product, big, small, smallLength, scratch);
        return true;
    }
    // The longer one in pieces as long as the shorter one.
    Limb* piece = scratch;
    scratch += 2 * smallLength;
    std::memset(product, 0, static_cast<size_t>(bigLength + smallLength) * sizeof(Limb));
    int64_t offset = 0;
    for (; offset + smallLength <= bigLength; offset += smallLength) {
        multiplyKaratsuba(piece, big + offset, small, smallLength, scratch);
        addToLimbs(product + offset, bigLength + smallLength - offset, piece, 2 * smallLength);
    }
    if (int64_t left = bigLength - offset) {
        if (!multiplyLimbs(piece, small, smallLength, big + offset, left))
            return false;
        addToLimbs(product + offset, bigLength + smallLength - offset, piece, smallLength + left);
    }
    return true;
}

Limb divideLimbsByLimb(Limb* quotient, const Limb* dividend, int64_t dividendLength, Limb divisor)
{
    Limb remainder = 0;
    for (int64_t i = dividendLength - 1; i >= 0; --i) {
        unsigned __int128 partial = static_cast<unsigned __int128>(remainder) * radix + dividend[i];
        quotient[i] = divideWide(static_cast<Limb>(partial >> 64), static_cast<Limb>(partial), divisor, remainder);
    }
    return remainder;
}

// Knuth, The Art of Computer Programming, 4.3.1, algorithm D, in base 10**19.
static int divideSchoolbook(Limb* quotient, Limb* remainder, const Limb* dividend, int64_t dividendLength, const Limb* divisor, int64_t divisorLength)
{
    int64_t n = divisorLength;
    int64_t m = dividendLength - divisorLength;
    Scratch<40> memory;
    Limb* u = memory.get(dividendLength + 1 + n + 1);
    if (!u)
        return -1;
    Limb* v = u + dividendLength + 1;

    // So that the top limb of the divisor is at least half the base.
    Limb scale = radix / (divisor[n - 1] + 1);
    multiplyLimbsByLimb(u, dividend, dividendLength, scale);
    multiplyLimbsByLimb(v, divisor, n, scale);

    Limb top = v[n - 1];
    Limb second = v[n - 2];
    for (int64_t j = m; j >= 0; --j) {
        unsigned __int128 leading = static_cast<unsigned __int128>(u[j + n]) * radix + u[j + n - 1];
        Limb rest;
        Limb estimate = divideWide(static_cast<Limb>(leading >> 64), static_cast<Limb>(leading), top, rest);
        while (estimate >= radix || static_cast<unsigned __int128>(estimate) * second > static_cast<unsigned __int128>(rest) * radix + u[j + n - 2]) {
            --estimate;
            if (rest >= radix - top)
                break;
            rest += top;
        }
        // u[j .. j + n] -= estimate * v
        Limb carry = 0;
        Limb borrow = 0;
        for (int64_t i = 0; i < n; ++i) {
            Limb low;
            multiplyAdd(v[i], estimate, carry, 0, carry, low);
            Limb d = u[i + j] - (low + borrow);
            borrow = u[i + j] < d;
            u[i + j] = borrow ? d + radix : d;
        }
        Limb d = u[j + n] - (carry + borrow);
        borrow = u[j + n] < d;
        u[j + n] = borrow ? d + radix : d;
        if (borrow) [[unlikely]] {
            --estimate;
            u[j + n] += addLimbs(u + j, u + j, v, n, n);
            if (u[j + n] >= radix)
                u[j + n] -= radix;
        }
        quotient[j] = estimate;
    }
    if (remainder) {
        divideLimbsByLimb(remainder, u, n, scale);
        return !limbsAreZero(remainder, n);
    }
    return !limbsAreZero(u, n);
}

// ---- Division of long numbers ----
//
// Burnikel and Ziegler, "Fast Recursive Division": a division of 2n limbs by n is two of 3n/2 by n, each of which is one of n by n/2
// and a multiplication. So division costs what multiplication costs, but for a logarithm.

static constexpr int64_t recursiveDivisionThreshold = 64;

static bool divideTwoByOne(Limb* quotient, Limb* dividendAndRemainder, const Limb* divisor, int64_t n, Limb* scratch);

// a has 3 * half limbs and is less than b * base**half; b has 2 * half, with a top limb of at least half the base. The quotient has `half`
// limbs; the remainder is left in the lower 2 * half limbs of a.
static bool divideThreeByTwo(Limb* quotient, Limb* a, const Limb* b, int64_t half, Limb* scratch)
{
    const Limb* bHigh = b + half;
    Limb* product = scratch;
    Limb* rest = scratch + 2 * half;
    if (compareLimbs(a + 2 * half, bHigh, half) < 0) {
        if (!divideTwoByOne(quotient, a + half, bHigh, half, rest))
            return false;
    } else {
        // The quotient would not fit: it is taken to be base**half - 1, and the upper part is what that leaves.
        for (int64_t i = 0; i < half; ++i)
            quotient[i] = radix - 1;
        subtractLimbs(a + 2 * half, a + 2 * half, bHigh, half, half);
        Limb carry = addLimbs(a + half, a + half, bHigh, 2 * half, half);
        (void)carry;
    }
    // a is now (remainder of the upper part, lowest third). Take off quotient * lower half of b; too much by at most twice b.
    if (!multiplyLimbs(product, quotient, half, b, half))
        return false;
    // As a + 2 * half .. holds what is beyond the remainder, it is zero unless the branch above left a carry there.
    int64_t length = 3 * half;
    bool isNegative = false;
    {
        int64_t productLength = 2 * half;
        while (productLength && !product[productLength - 1])
            --productLength;
        int64_t aLength = length;
        while (aLength && !a[aLength - 1])
            --aLength;
        if (aLength < productLength || (aLength == productLength && compareLimbs(a, product, aLength) < 0))
            isNegative = true;
    }
    if (!isNegative) {
        subtractFromLimbs(a, length, product, 2 * half);
        return true;
    }
    // product - a is how far below zero it is.
    subtractFromLimbs(product, 2 * half, a, 2 * half);
    std::memset(a, 0, static_cast<size_t>(length) * sizeof(Limb));
    while (true) {
        // quotient -= 1; deficit -= b
        for (int64_t i = 0; i < half; ++i) {
            if (quotient[i]) {
                --quotient[i];
                break;
            }
            quotient[i] = radix - 1;
        }
        if (compareLimbs(product, b, 2 * half) <= 0) {
            subtractLimbs(a, b, product, 2 * half, 2 * half);
            return true;
        }
        subtractLimbs(product, product, b, 2 * half, 2 * half);
    }
}

// The dividend has 2 * n limbs and is less than divisor * base**n. The quotient has n limbs; the remainder is left in the lower n limbs.
static bool divideTwoByOne(Limb* quotient, Limb* a, const Limb* divisor, int64_t n, Limb* scratch)
{
    if ((n & 1) || n < recursiveDivisionThreshold) {
        Limb* wideQuotient = scratch;
        Limb* remainder = scratch + n + 1;
        if (divideSchoolbook(wideQuotient, remainder, a, 2 * n, divisor, n) < 0)
            return false;
        std::memcpy(quotient, wideQuotient, static_cast<size_t>(n) * sizeof(Limb));
        std::memcpy(a, remainder, static_cast<size_t>(n) * sizeof(Limb));
        std::memset(a + n, 0, static_cast<size_t>(n) * sizeof(Limb));
        return true;
    }
    int64_t half = n / 2;
    if (!divideThreeByTwo(quotient + half, a + half, divisor, half, scratch))
        return false;
    return divideThreeByTwo(quotient, a, divisor, half, scratch);
}

static int divideRecursively(Limb* quotient, Limb* remainder, const Limb* dividend, int64_t dividendLength, const Limb* divisor, int64_t divisorLength)
{
    // The divisor is padded at the low end to a length that halves down to the threshold, and scaled so that its top limb is large. The
    // dividend goes along, and is then divided block by block.
    int64_t blocks = 1;
    while ((divisorLength + blocks - 1) / blocks >= recursiveDivisionThreshold)
        blocks *= 2;
    int64_t n = (divisorLength + blocks - 1) / blocks * blocks;
    int64_t padding = n - divisorLength;
    Limb scale = radix / (divisor[divisorLength - 1] + 1);

    int64_t scaledLength = dividendLength + padding + 1;
    int64_t count = std::max<int64_t>(2, (scaledLength + n - 1) / n + 1);
    Scratch<1> memory;
    int64_t total = n + 1 + count * n + count * n + 2 * n + 8 * n + 64;
    Limb* b = memory.get(total);
    if (!b)
        return -1;
    Limb* a = b + n + 1;
    Limb* q = a + count * n;
    Limb* window = q + count * n;
    Limb* scratch = window + 2 * n;

    std::memset(b, 0, static_cast<size_t>(padding) * sizeof(Limb));
    multiplyLimbsByLimb(b + padding, divisor, divisorLength, scale);
    std::memset(a, 0, static_cast<size_t>(count * n) * sizeof(Limb));
    multiplyLimbsByLimb(a + padding, dividend, dividendLength, scale);

    // window = (what is left of the blocks above, the next block)
    std::memcpy(window + n, a + (count - 1) * n, static_cast<size_t>(n) * sizeof(Limb));
    for (int64_t block = count - 2; block >= 0; --block) {
        std::memcpy(window, a + block * n, static_cast<size_t>(n) * sizeof(Limb));
        if (!divideTwoByOne(q + block * n, window, b, n, scratch))
            return -1;
        std::memcpy(window + n, window, static_cast<size_t>(n) * sizeof(Limb));
    }
    std::memcpy(quotient, q, static_cast<size_t>(dividendLength - divisorLength + 1) * sizeof(Limb));
    // The remainder is scaled and padded as the divisor was.
    const Limb* scaledRemainder = window + n + padding;
    if (remainder) {
        divideLimbsByLimb(remainder, scaledRemainder, divisorLength, scale);
        return !limbsAreZero(remainder, divisorLength);
    }
    return !limbsAreZero(window + n, n);
}

int divideLimbs(Limb* quotient, Limb* remainder, const Limb* dividend, int64_t dividendLength, const Limb* divisor, int64_t divisorLength)
{
    if (divisorLength < 2 * recursiveDivisionThreshold || dividendLength - divisorLength < recursiveDivisionThreshold)
        return divideSchoolbook(quotient, remainder, dividend, dividendLength, divisor, divisorLength);
    return divideRecursively(quotient, remainder, dividend, dividendLength, divisor, divisorLength);
}

// ---- Shifting coefficients ----

// destination = source * 10**count. The destination, which may be the source, has just the limbs that takes.
static void shiftLimbsLeft(Limb* destination, const Limb* source, int64_t destinationLength, int64_t sourceLength, int64_t count)
{
    int64_t whole = count / digitsPerLimb;
    unsigned part = static_cast<unsigned>(count % digitsPerLimb);
    if (!part)
        std::memmove(destination + whole, source, static_cast<size_t>(sourceLength) * sizeof(Limb));
    else {
        Limb scale = powersOfTen[part];
        Limb high;
        Limb low;
        splitLimb(source[sourceLength - 1], digitsPerLimb - part, high, low);
        if (destinationLength > sourceLength + whole)
            destination[sourceLength + whole] = high;
        for (int64_t i = sourceLength - 1; i > 0; --i) {
            Limb previousLow = low;
            splitLimb(source[i - 1], digitsPerLimb - part, high, low);
            destination[i + whole] = previousLow * scale + high;
        }
        destination[whole] = low * scale;
    }
    for (int64_t i = 0; i < whole; ++i)
        destination[i] = 0;
}

static PY_DECIMAL_INLINE unsigned lossIndicator(Limb firstLostDigit, bool hasMore)
{
    return static_cast<unsigned>(firstLostDigit) + ((!firstLostDigit || firstLostDigit == 5) && hasMore);
}

// destination = source / 10**count, for a count less than the number of digits.
static unsigned shiftLimbsRight(Limb* destination, const Limb* source, int64_t sourceLength, int64_t count)
{
    int64_t whole = count / digitsPerLimb;
    unsigned part = static_cast<unsigned>(count % digitsPerLimb);
    Limb firstLost;
    Limb rest;
    if (part) {
        Limb high;
        Limb low;
        splitLimb(source[whole], part, high, low);
        splitLimb(low, part - 1, firstLost, rest);
        bool hasMore = rest || !limbsAreZero(source, whole);
        Limb scale = powersOfTen[digitsPerLimb - part];
        int64_t j = 0;
        for (int64_t i = whole + 1; i < sourceLength; ++i, ++j) {
            Limb nextHigh;
            splitLimb(source[i], part, nextHigh, low);
            destination[j] = low * scale + high;
            high = nextHigh;
        }
        if (high)
            destination[j] = high;
        return lossIndicator(firstLost, hasMore);
    }
    splitLimb(source[whole - 1], digitsPerLimb - 1, firstLost, rest);
    bool hasMore = rest || !limbsAreZero(source, whole - 1);
    std::memmove(destination, source + whole, static_cast<size_t>(sourceLength - whole) * sizeof(Limb));
    return lossIndicator(firstLost, hasMore);
}

// What is lost when all digits go. `count` is the number of digits or more.
static unsigned lossOfWholeCoefficient(const Number& number, bool countIsDigits)
{
    if (!countIsDigits)
        return !limbsAreZero(number.limbs(), number.length);
    Limb first;
    Limb rest;
    splitLimb(number.top(), static_cast<unsigned>(digitCount(number.top()) - 1), first, rest);
    return lossIndicator(first, rest || !limbsAreZero(number.limbs(), number.length - 1));
}

bool shiftCoefficientLeft(Number& result, const Number& a, int64_t count, uint32_t& status)
{
    if (a.hasZeroCoefficient() || !count)
        return result.copy(a, status);
    int64_t size = limbsForDigits(a.digits + count);
    if (&result == &a ? !result.reserve(size, status) : !result.reserveDiscarding(size, status))
        return false;
    shiftLimbsLeft(result.limbs(), a.limbs(), size, a.length, count);
    result.flags = a.flags;
    result.exponent = a.exponent;
    result.digits = a.digits + count;
    result.length = size;
    return true;
}

unsigned shiftCoefficientRight(Number& number, int64_t count)
{
    if (number.hasZeroCoefficient() || !count)
        return 0;
    if (count >= number.digits) {
        unsigned lost = lossOfWholeCoefficient(number, count == number.digits);
        number.setZeroCoefficient();
        return lost;
    }
    unsigned lost = shiftLimbsRight(number.limbs(), number.limbs(), number.length, count);
    number.digits -= count;
    number.length = limbsForDigits(number.digits);
    return lost;
}

unsigned shiftCoefficientRight(Number& result, const Number& a, int64_t count, uint32_t& status)
{
    if (&result == &a)
        return shiftCoefficientRight(result, count);
    if (a.hasZeroCoefficient() || !count)
        return result.copy(a, status) ? 0 : shiftFailed;
    unsigned lost;
    if (count >= a.digits) {
        lost = lossOfWholeCoefficient(a, count == a.digits);
        result.setZeroCoefficient();
    } else {
        int64_t digits = a.digits - count;
        int64_t size = limbsForDigits(digits);
        if (!result.reserveDiscarding(size, status))
            return shiftFailed;
        lost = shiftLimbsRight(result.limbs(), a.limbs(), a.length, count);
        result.digits = digits;
        result.length = size;
    }
    result.flags = a.flags;
    result.exponent = a.exponent;
    return lost;
}

// ---- Rounding, and bringing a result within the limits of the context ----

static PY_DECIMAL_INLINE bool roundsAwayFromZero(const Number& number, unsigned lost, Rounding rounding)
{
    switch (rounding) {
    case Rounding::Down:
    case Rounding::Truncate:
        return false;
    case Rounding::HalfUp:
        return lost >= 5;
    case Rounding::HalfEven:
        return lost > 5 || (lost == 5 && number.hasOddCoefficient());
    case Rounding::Ceiling:
        return lost && !number.isNegative();
    case Rounding::Floor:
        return lost && number.isNegative();
    case Rounding::HalfDown:
        return lost > 5;
    case Rounding::Up:
        return lost;
    case Rounding::ZeroFiveUp: {
        Limb last = number.limbs()[0] % 10;
        return lost && (!last || last == 5);
    }
    }
    return false;
}

// As many nines as the precision allows.
static void setLargestCoefficient(Number& number, const Context& context, uint32_t& status)
{
    int64_t size = limbsForDigits(context.precision);
    if (!number.reserveDiscarding(size, status))
        return;
    number.length = size;
    number.digits = context.precision;
    Limb* limbs = number.limbs();
    int part = static_cast<int>(context.precision % digitsPerLimb);
    for (int64_t i = 0; i < size; ++i)
        limbs[i] = radix - 1;
    if (part)
        limbs[size - 1] = powersOfTen[part] - 1;
}

static void checkExponent(Number&, const Context&, uint32_t& status);

// For a coefficient that has just been cut down to the precision.
static void applyRounding(Number& number, unsigned lost, const Context& context, uint32_t& status)
{
    if (!roundsAwayFromZero(number, lost, context.rounding))
        return;
    // All nines become a one and zeros, one digit too many: the last goes, and the exponent makes up for it.
    if (incrementLimbs(number.limbs(), number.length)) {
        number.limbs()[number.length - 1] = powersOfTen[digitsPerLimb - 1];
        ++number.exponent;
        checkExponent(number, context, status);
        return;
    }
    number.updateDigits();
    if (number.digits > context.precision) {
        shiftCoefficientRight(number, 1);
        ++number.exponent;
        number.digits = context.precision;
        checkExponent(number, context, status);
    }
}

void applyRoundingFreely(Number& number, unsigned lost, const Context& context, uint32_t& status)
{
    if (!roundsAwayFromZero(number, lost, context.rounding))
        return;
    if (incrementLimbs(number.limbs(), number.length)) {
        if (!number.reserve(number.length + 1, status))
            return;
        number.limbs()[number.length++] = 1;
    }
    number.updateDigits();
}

// False: the coefficient would get longer than the precision. That is an InvalidOperation.
static bool applyRoundingIfItFits(Number& number, unsigned lost, const Context& context, uint32_t& status)
{
    if (!roundsAwayFromZero(number, lost, context.rounding))
        return true;
    if (incrementLimbs(number.limbs(), number.length)) {
        if (!number.reserve(number.length + 1, status))
            return false;
        number.limbs()[number.length++] = 1;
    }
    number.updateDigits();
    if (number.digits > context.precision) {
        number.setError(InvalidOperation, status);
        return false;
    }
    return true;
}

// Overflow, underflow, clamping. For a finite number; what it becomes may be an infinity.
static void checkExponent(Number& number, const Context& context, uint32_t& status)
{
    int64_t adjusted = number.adjustedExponent();
    if (adjusted > context.emax) {
        if (number.hasZeroCoefficient()) {
            number.exponent = context.clamp ? context.etop() : context.emax;
            number.setZeroCoefficient();
            status |= Clamped;
            return;
        }
        bool becomesInfinite = true;
        switch (context.rounding) {
        case Rounding::Down:
        case Rounding::ZeroFiveUp:
            becomesInfinite = false;
            break;
        case Rounding::Ceiling:
            becomesInfinite = !number.isNegative();
            break;
        case Rounding::Floor:
            becomesInfinite = number.isNegative();
            break;
        default:
            break;
        }
        if (becomesInfinite)
            number.setSpecial(number.sign(), Number::Infinite);
        else {
            setLargestCoefficient(number, context, status);
            number.exponent = context.etop();
        }
        status |= Overflow | Inexact | Rounded;
        return;
    }
    if (context.clamp && number.exponent > context.etop()) {
        // The exponent is brought down, with zeros at the end of the coefficient. They fit, as the adjusted exponent is within the limit.
        int64_t count = number.exponent - context.etop();
        if (!shiftCoefficientLeft(number, number, count, status))
            return;
        number.exponent -= count;
        status |= Clamped;
        if (!number.hasZeroCoefficient() && adjusted < context.emin)
            status |= Subnormal;
        return;
    }
    if (adjusted < context.emin) {
        int64_t etiny = context.etiny();
        if (number.hasZeroCoefficient()) {
            if (number.exponent < etiny) {
                number.exponent = etiny;
                number.setZeroCoefficient();
                status |= Clamped;
            }
            return;
        }
        status |= Subnormal;
        if (number.exponent < etiny) {
            unsigned lost = shiftCoefficientRight(number, etiny - number.exponent);
            number.exponent = etiny;
            applyRoundingFreely(number, lost, context, status);
            status |= Rounded;
            if (lost) {
                status |= Inexact | Underflow;
                if (number.hasZeroCoefficient()) {
                    number.setZeroCoefficient();
                    status |= Clamped;
                }
            }
        }
    }
}

// The payload of a NaN loses the digits at the front that the context has no room for.
static void fitPayload(Number& number, const Context& context)
{
    int64_t room = context.precision - context.clamp;
    if (!number.length || number.digits <= room)
        return;
    if (!room) {
        number.length = number.digits = 0;
        return;
    }
    int64_t size = limbsForDigits(room);
    if (int part = static_cast<int>(room % digitsPerLimb))
        number.limbs()[size - 1] %= powersOfTen[part];
    number.trim(size);
    if (number.hasZeroCoefficient())
        number.length = number.digits = 0;
}

// The same for the coefficient of a finite number.
static void dropLeadingDigits(Number& number, const Context& context)
{
    if (number.length && number.digits > context.precision) {
        int64_t size = limbsForDigits(context.precision);
        if (int part = static_cast<int>(context.precision % digitsPerLimb))
            number.limbs()[size - 1] %= powersOfTen[part];
        number.trim(size);
    }
    if (number.isZero())
        number.setZeroCoefficient();
}

void finalize(Number& number, const Context& context, uint32_t& status)
{
    if (number.isSpecial()) [[unlikely]] {
        if (number.isNaN())
            fitPayload(number, context);
        return;
    }
    checkExponent(number, context, status);
    if (number.isSpecial())
        return;
    if (number.digits > context.precision) {
        int64_t count = number.digits - context.precision;
        unsigned lost = shiftCoefficientRight(number, count);
        number.exponent += count;
        applyRounding(number, lost, context, status);
        status |= Rounded;
        if (lost)
            status |= Inexact;
    }
}

static void becomeQuietNaN(Number& result, const Number& source, const Context& context, uint32_t& status)
{
    if (!result.copy(source, status))
        return;
    result.flags = (result.flags & ~Number::anySpecial) | Number::QuietNaN;
    fitPayload(result, context);
}

bool propagateNaN(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (!a.isNaN())
        return false;
    if (a.isSignalingNaN())
        status |= InvalidOperation;
    becomeQuietNaN(result, a, context, status);
    return true;
}

// A signaling one goes before a quiet one, and the first operand before the second.
bool propagateNaN(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if (!((a.flags | b.flags) & Number::anyNaN))
        return false;
    const Number* choice = &b;
    if (a.isSignalingNaN()) {
        choice = &a;
        status |= InvalidOperation;
    } else if (b.isSignalingNaN())
        status |= InvalidOperation;
    else if (a.isQuietNaN())
        choice = &a;
    becomeQuietNaN(result, *choice, context, status);
    return true;
}

bool propagateNaN(Number& result, const Number& a, const Number& b, const Number& c, const Context& context, uint32_t& status)
{
    if (!((a.flags | b.flags | c.flags) & Number::anyNaN))
        return false;
    const Number* choice = &c;
    if (a.isSignalingNaN()) {
        choice = &a;
        status |= InvalidOperation;
    } else if (b.isSignalingNaN()) {
        choice = &b;
        status |= InvalidOperation;
    } else if (c.isSignalingNaN())
        status |= InvalidOperation;
    else if (a.isQuietNaN())
        choice = &a;
    else if (b.isQuietNaN())
        choice = &b;
    becomeQuietNaN(result, *choice, context, status);
    return true;
}

// ---- Comparisons ----

// The digits of the coefficient in groups of 19, counted from the front: group 0 has the first 19. Zeros follow the last digit.
static PY_DECIMAL_INLINE Limb leadingGroup(const Number& number, unsigned missing, int64_t index)
{
    int64_t i = number.length - 1 - index;
    if (i < 0)
        return 0;
    if (!missing)
        return number.limbs()[i];
    Limb high;
    Limb low;
    splitLimb(number.limbs()[i], digitsPerLimb - missing, high, low);
    Limb group = low * powersOfTen[missing];
    if (i > 0)
        group += number.limbs()[i - 1] / powersOfTen[digitsPerLimb - missing];
    return group;
}

static PY_DECIMAL_INLINE unsigned digitsMissingInTopLimb(const Number& number)
{
    return static_cast<unsigned>(number.length * digitsPerLimb - number.digits);
}

// Of the coefficients of numbers whose first digits have the same weight.
static int compareAlignedAtFront(const Number& a, const Number& b)
{
    if (a.exponent == b.exponent)
        return compareLimbs(a.limbs(), b.limbs(), a.length);
    unsigned missingA = digitsMissingInTopLimb(a);
    unsigned missingB = digitsMissingInTopLimb(b);
    int64_t groups = std::max(a.length, b.length);
    for (int64_t i = 0; i < groups; ++i) {
        Limb groupA = leadingGroup(a, missingA, i);
        Limb groupB = leadingGroup(b, missingB, i);
        if (groupA != groupB)
            return groupA < groupB ? -1 : 1;
    }
    return 0;
}

int compareMagnitudes(const Number& a, const Number& b)
{
    if (&a == &b)
        return 0;
    if (a.isInfinite())
        return !b.isInfinite();
    if (b.isInfinite())
        return -1;
    if (a.hasZeroCoefficient())
        return b.hasZeroCoefficient() ? 0 : -1;
    if (b.hasZeroCoefficient())
        return 1;
    int64_t adjustedA = a.adjustedExponent();
    int64_t adjustedB = b.adjustedExponent();
    if (adjustedA != adjustedB)
        return adjustedA < adjustedB ? -1 : 1;
    return compareAlignedAtFront(a, b);
}

int compareValues(const Number& a, const Number& b)
{
    if (&a == &b)
        return 0;
    if (a.isInfinite()) {
        if (b.isInfinite())
            return static_cast<int>(b.isNegative()) - static_cast<int>(a.isNegative());
        return a.arithmeticSign();
    }
    if (b.isInfinite())
        return -b.arithmeticSign();
    if (a.hasZeroCoefficient())
        return b.hasZeroCoefficient() ? 0 : -b.arithmeticSign();
    if (b.hasZeroCoefficient())
        return a.arithmeticSign();
    if (a.sign() != b.sign())
        return static_cast<int>(b.sign()) - static_cast<int>(a.sign());
    return compareMagnitudes(a, b) * a.arithmeticSign();
}

int compareQuietly(const Number& a, const Number& b, uint32_t& status)
{
    if ((a.flags | b.flags) & Number::anyNaN) {
        status |= InvalidOperation;
        return unordered;
    }
    return compareValues(a, b);
}

static void setComparisonResult(Number& result, int order)
{
    result.set(order < 0 ? Number::Negative : 0, order ? 1 : 0, 0);
}

void compare(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if (propagateNaN(result, a, b, context, status))
        return;
    setComparisonResult(result, compareValues(a, b));
}

void compareSignal(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if (propagateNaN(result, a, b, context, status)) {
        status |= InvalidOperation;
        return;
    }
    setComparisonResult(result, compareValues(a, b));
}

int compareTotalOrder(const Number& a, const Number& b)
{
    if (a.sign() != b.sign())
        return static_cast<int>(b.sign()) - static_cast<int>(a.sign());
    int order;
    if (a.isNaN()) {
        order = 1;
        if (b.isNaN()) {
            if (a.isQuietNaN() == b.isQuietNaN()) {
                if (a.length && b.length) {
                    Number payloadA(Number::Borrow, 0, 0, a.digits, a.length, a.limbs());
                    Number payloadB(Number::Borrow, 0, 0, b.digits, b.length, b.limbs());
                    order = compareMagnitudes(payloadA, payloadB);
                } else
                    order = static_cast<int>(a.length > 0) - static_cast<int>(b.length > 0);
            } else
                order = static_cast<int>(a.isQuietNaN()) - static_cast<int>(b.isQuietNaN());
        }
    } else if (b.isNaN())
        order = -1;
    else {
        order = compareMagnitudes(a, b);
        if (!order && a.exponent != b.exponent)
            order = a.exponent < b.exponent ? -1 : 1;
    }
    return order * a.arithmeticSign();
}

int compareTotalOrderOfMagnitudes(const Number& a, const Number& b)
{
    Number magnitudeA(Number::Borrow, a.flags & ~Number::Negative, a.exponent, a.digits, a.length, a.limbs());
    Number magnitudeB(Number::Borrow, b.flags & ~Number::Negative, b.exponent, b.digits, b.length, b.limbs());
    return compareTotalOrder(magnitudeA, magnitudeB);
}

void compareTotal(Number& result, const Number& a, const Number& b)
{
    setComparisonResult(result, compareTotalOrder(a, b));
}

void compareTotalMagnitude(Number& result, const Number& a, const Number& b)
{
    setComparisonResult(result, compareTotalOrderOfMagnitudes(a, b));
}

// An order for numbers that are equal in value: by sign, then by exponent.
static int orderOfEqualValues(const Number& a, const Number& b)
{
    if (a.sign() != b.sign())
        return static_cast<int>(b.sign()) - static_cast<int>(a.sign());
    return (a.exponent < b.exponent ? -1 : 1) * a.arithmeticSign();
}

template<bool wantsLarger, bool byMagnitude>
static void select(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    // A number wins over a quiet NaN.
    if (a.isQuietNaN() && !b.isNaN())
        result.copy(b, status);
    else if (b.isQuietNaN() && !a.isNaN())
        result.copy(a, status);
    else if (propagateNaN(result, a, b, context, status))
        return;
    else {
        int order = byMagnitude ? compareMagnitudes(a, b) : compareValues(a, b);
        if (!order)
            order = orderOfEqualValues(a, b);
        result.copy((order < 0) == wantsLarger ? b : a, status);
    }
    finalize(result, context, status);
}

void maximum(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status) { select<true, false>(result, a, b, context, status); }
void maximumMagnitude(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status) { select<true, true>(result, a, b, context, status); }
void minimum(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status) { select<false, false>(result, a, b, context, status); }
void minimumMagnitude(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status) { select<false, true>(result, a, b, context, status); }

// ---- Copies ----

void copyAbsolute(Number& result, const Number& a, uint32_t& status)
{
    if (result.copy(a, status))
        result.setSign(0);
}

void copyNegated(Number& result, const Number& a, uint32_t& status)
{
    if (result.copy(a, status))
        result.negate();
}

void copySign(Number& result, const Number& a, const Number& signSource, uint32_t& status)
{
    uint8_t sign = signSource.sign();
    if (result.copy(a, status))
        result.setSign(sign);
}

bool haveSameQuantum(const Number& a, const Number& b)
{
    if (a.isSpecial() || b.isSpecial())
        return (a.isNaN() && b.isNaN()) || (a.isInfinite() && b.isInfinite());
    return a.exponent == b.exponent;
}

const char* classOf(const Number& a, const Context& context)
{
    if (a.isNaN())
        return a.isQuietNaN() ? "NaN" : "sNaN";
    bool isNegative = a.isNegative();
    if (a.isInfinite())
        return isNegative ? "-Infinity" : "+Infinity";
    if (a.isZero())
        return isNegative ? "-Zero" : "+Zero";
    if (a.isNormal(context))
        return isNegative ? "-Normal" : "+Normal";
    return isNegative ? "-Subnormal" : "+Subnormal";
}

// ---- To machine integers ----

uint64_t magnitudeToUInt64(const Number& a, uint32_t& status)
{
    constexpr uint64_t failure = std::numeric_limits<uint64_t>::max();
    if (a.isSpecial()) {
        status |= InvalidOperation;
        return failure;
    }
    if (a.hasZeroCoefficient())
        return 0;
    int64_t integerDigits = a.digits + a.exponent;
    if (integerDigits > digitsPerLimb + 1 || (a.exponent < 0 && !a.isIntegerValued())) {
        status |= InvalidOperation;
        return failure;
    }
    // The digits of the coefficient that count, then zeros if the exponent is positive.
    int64_t taken = std::min(a.digits, integerDigits);
    unsigned missing = digitsMissingInTopLimb(a);
    Limb first = leadingGroup(a, missing, 0);
    unsigned __int128 value;
    if (taken <= digitsPerLimb)
        value = first / powersOfTen[digitsPerLimb - taken];
    else
        value = static_cast<unsigned __int128>(first) * 10 + leadingGroup(a, missing, 1) / powersOfTen[digitsPerLimb - 1];
    value *= powersOfTen[integerDigits - taken];
    if (value > failure) {
        status |= InvalidOperation;
        return failure;
    }
    return static_cast<uint64_t>(value);
}

int64_t toInt64(const Number& a, uint32_t& status)
{
    constexpr int64_t failure = std::numeric_limits<int64_t>::max();
    uint32_t ownStatus = 0;
    uint64_t magnitude = magnitudeToUInt64(a, ownStatus);
    if (ownStatus & InvalidOperation) {
        status |= ownStatus;
        return failure;
    }
    if (magnitude <= static_cast<uint64_t>(failure))
        return a.isNegative() ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
    if (a.isNegative() && magnitude == static_cast<uint64_t>(failure) + 1)
        return std::numeric_limits<int64_t>::min();
    status |= InvalidOperation;
    return failure;
}

// ---- Addition ----

static void addInfinities(Number& result, const Number& a, const Number& b, uint8_t signOfB, uint32_t& status)
{
    if (a.isInfinite()) {
        if (a.sign() != signOfB && b.isInfinite())
            result.setError(InvalidOperation, status);
        else
            result.setSpecial(a.sign(), Number::Infinite);
        return;
    }
    result.setSpecial(signOfB, Number::Infinite);
}

// a + b, with the sign of b as given. Both are finite.
static void addWithSign(Number& result, const Number& a, const Number& b, uint8_t signOfB, const Context& context, uint32_t& status)
{
    const Number* big = &a;
    const Number* small = &b;
    uint8_t signOfA = a.sign();
    bool isSwapped = false;
    Number aligned;
    Number tiny;

    if (big->exponent != small->exponent) {
        if (small->exponent > big->exponent) {
            std::swap(big, small);
            isSwapped = !isSwapped;
        }
        if (!big->hasZeroCoefficient()) {
            // An operand that lies wholly below what rounding can see only matters in that it is there, on one side or the other. It
            // is replaced by a single 1 just below the digits that count, so that the other need not be shifted by more than the precision.
            int64_t floor = big->exponent - 1;
            if (big->digits <= context.precision)
                floor += big->digits - context.precision - 1;
            if (small->adjustedExponent() < floor) {
                tiny.set(small->sign(), small->hasZeroCoefficient() ? 0 : 1, floor);
                small = &tiny;
            }
            if (!shiftCoefficientLeft(aligned, *big, big->exponent - small->exponent, status)) {
                result.setError(OutOfMemory, status);
                return;
            }
            big = &aligned;
        }
    }
    int64_t exponent = small->exponent;

    if (big->length < small->length) {
        std::swap(big, small);
        isSwapped = !isSwapped;
    }
    int64_t size = big->length;
    if (!result.reserve(size + 1, status))
        return;

    if (signOfA == signOfB) {
        Limb carry = addLimbs(result.limbs(), big->limbs(), small->limbs(), size, small->length);
        if (carry)
            result.limbs()[size++] = carry;
        result.length = size;
        result.flags = signOfB;
        result.updateDigits();
    } else {
        if (size == small->length && compareLimbs(big->limbs(), small->limbs(), size) < 0) {
            std::swap(big, small);
            isSwapped = !isSwapped;
        }
        subtractLimbs(result.limbs(), big->limbs(), small->limbs(), size, small->length);
        result.flags = isSwapped ? signOfB : signOfA;
        result.trim(size);
        if (result.hasZeroCoefficient())
            result.flags = context.rounding == Rounding::Floor ? Number::Negative : 0;
    }
    result.exponent = exponent;
}

void addUnrounded(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    addWithSign(result, a, b, b.sign(), context, status);
}

void subtractUnrounded(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    addWithSign(result, a, b, b.sign() ^ Number::Negative, context, status);
}

void add(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if ((a.flags | b.flags) & Number::anySpecial) [[unlikely]] {
        if (!propagateNaN(result, a, b, context, status))
            addInfinities(result, a, b, b.sign(), status);
        return;
    }
    addWithSign(result, a, b, b.sign(), context, status);
    finalize(result, context, status);
}

void subtract(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if ((a.flags | b.flags) & Number::anySpecial) [[unlikely]] {
        if (!propagateNaN(result, a, b, context, status))
            addInfinities(result, a, b, b.sign() ^ Number::Negative, status);
        return;
    }
    addWithSign(result, a, b, b.sign() ^ Number::Negative, context, status);
    finalize(result, context, status);
}

template<void (*operation)(Number&, const Number&, const Number&, const Context&, uint32_t&)>
static void exactly(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    uint32_t ownStatus = 0;
    operation(result, a, b, context, ownStatus);
    status |= ownStatus;
    if (ownStatus & (Inexact | Rounded | Clamped))
        result.setError(InvalidOperation, status);
}

void addExactly(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status) { exactly<add>(result, a, b, context, status); }
void subtractExactly(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status) { exactly<subtract>(result, a, b, context, status); }
void multiplyExactly(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status) { exactly<multiply>(result, a, b, context, status); }

void plus(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (propagateNaN(result, a, context, status))
        return;
    // There is no -0 but when rounding towards minus infinity.
    if (a.isZero() && context.rounding != Rounding::Floor)
        copyAbsolute(result, a, status);
    else
        result.copy(a, status);
    finalize(result, context, status);
}

void minus(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (propagateNaN(result, a, context, status))
        return;
    if (a.isZero() && context.rounding != Rounding::Floor)
        copyAbsolute(result, a, status);
    else
        copyNegated(result, a, status);
    finalize(result, context, status);
}

void absolute(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (a.isNegative())
        minus(result, a, context, status);
    else
        plus(result, a, context, status);
}

// ---- Multiplication ----

void multiplyUnrounded(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if ((a.flags | b.flags) & Number::anySpecial) [[unlikely]] {
        if (propagateNaN(result, a, b, context, status))
            return;
        // Infinity times zero is undefined.
        if ((a.isInfinite() && b.isZero()) || (b.isInfinite() && a.isZero()))
            result.setError(InvalidOperation, status);
        else
            result.setSpecial(a.sign() ^ b.sign(), Number::Infinite);
        return;
    }
    const Number* big = &a;
    const Number* small = &b;
    if (small->length > big->length)
        std::swap(big, small);
    uint8_t sign = a.sign() ^ b.sign();
    int64_t exponent = a.exponent + b.exponent;
    int64_t size = big->length + small->length;

    if (big->length == 1) {
        Limb high;
        Limb low;
        multiplyAdd(big->limbs()[0], small->limbs()[0], 0, 0, high, low);
        result.limbs()[0] = low;
        result.limbs()[1] = high;
    } else if (size <= 32) {
        Limb product[32];
        multiplySchoolbook(product, big->limbs(), big->length, small->limbs(), small->length);
        if (!result.reserveDiscarding(size, status))
            return;
        std::memcpy(result.limbs(), product, static_cast<size_t>(size) * sizeof(Limb));
    } else {
        Limb* product = allocateLimbs(size);
        if (!product || !multiplyLimbs(product, big->limbs(), big->length, small->limbs(), small->length)) {
            if (product)
                freeLimbs(product);
            result.setError(OutOfMemory, status);
            return;
        }
        result.adopt(product, size);
    }
    result.flags = sign;
    result.exponent = exponent;
    result.trim(size);
}

void multiply(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    multiplyUnrounded(result, a, b, context, status);
    finalize(result, context, status);
}

void fusedMultiplyAdd(Number& result, const Number& a, const Number& b, const Number& c, const Context& context, uint32_t& status)
{
    uint32_t ownStatus = 0;
    Number product;
    multiplyUnrounded(product, a, b, context, ownStatus);
    if (ownStatus & InvalidOperation)
        result.swap(product);
    else
        add(result, product, c, context, ownStatus);
    status |= ownStatus;
}

// ---- Division ----

void divideCore(Number& result, const Number& dividend, const Number& divisor, const Context& context, uint32_t& status, bool keepsAllDigits)
{
    const Number* a = &dividend;
    const Number* b = &divisor;
    uint8_t sign = a->sign() ^ b->sign();

    if ((a->flags | b->flags) & Number::anySpecial) [[unlikely]] {
        if (propagateNaN(result, *a, *b, context, status))
            return;
        if (a->isInfinite()) {
            if (b->isInfinite())
                result.setError(InvalidOperation, status);
            else
                result.setSpecial(sign, Number::Infinite);
            return;
        }
        result.set(sign, 0, context.etiny());
        status |= Clamped;
        return;
    }
    if (b->hasZeroCoefficient()) {
        if (a->hasZeroCoefficient())
            result.setError(DivisionUndefined, status);
        else {
            result.setSpecial(sign, Number::Infinite);
            status |= DivisionByZero;
        }
        return;
    }
    if (a->hasZeroCoefficient()) {
        result.set(sign, 0, a->exponent - b->exponent);
        finalize(result, context, status);
        return;
    }

    // One of the coefficients gets zeros at the end, so that the integer quotient has one digit more than the precision, or two.
    Number aligned;
    Number quotient;
    int64_t count = b->digits - a->digits + context.precision + 1;
    int64_t idealExponent = a->exponent - b->exponent;
    int64_t exponent = idealExponent - count;
    bool hasRemainder = false;
    bool failed = false;
    if (count > 0) {
        failed = !shiftCoefficientLeft(aligned, *a, count, status);
        a = &aligned;
    } else if (count < 0) {
        failed = !shiftCoefficientLeft(aligned, *b, -count, status);
        b = &aligned;
    }
    int64_t size = 0;
    if (!failed) {
        size = a->length - b->length + 1;
        failed = !quotient.reserveDiscarding(size, status);
    }
    if (!failed) {
        if (b->length == 1)
            hasRemainder = divideLimbsByLimb(quotient.limbs(), a->limbs(), a->length, b->limbs()[0]);
        else {
            int outcome = divideLimbs(quotient.limbs(), nullptr, a->limbs(), a->length, b->limbs(), b->length);
            failed = outcome < 0;
            hasRemainder = outcome > 0;
        }
    }
    if (failed) {
        result.setError(OutOfMemory, status);
        return;
    }
    quotient.flags = sign;
    quotient.trim(size);
    if (hasRemainder) {
        // What is beyond the last digit is only seen by the rounding if that digit is 0 or 5.
        Limb last = quotient.limbs()[0] % 10;
        if (!last || last == 5)
            ++quotient.limbs()[0];
    } else if (!keepsAllDigits && idealExponent > exponent) {
        int64_t excess = std::min(quotient.trailingZeros(), idealExponent - exponent);
        shiftCoefficientRight(quotient, excess);
        exponent += excess;
    }
    quotient.exponent = exponent;
    result.swap(quotient);
    finalize(result, context, status);
}

void divide(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    uint32_t firstStatus = 0;
    Number quotient;
    divideCore(quotient, a, b, context, firstStatus, false);
    if (!(firstStatus & (OutOfMemory | DivisionImpossible))) [[likely]] {
        status |= firstStatus;
        result.swap(quotient);
        return;
    }
    // A quotient that does not end fills the whole precision, which may be more than there is memory for. One that ends has at
    // most as many digits as the dividend, and four for each digit of the divisor.
    Context smaller = context;
    smaller.precision = a.digits + b.digits * 4;
    if (smaller.precision >= context.precision) {
        status |= firstStatus & errorConditions;
        result.swap(quotient);
        return;
    }
    uint32_t secondStatus = 0;
    divideCore(quotient, a, b, smaller, secondStatus, false);
    if (secondStatus)
        quotient.setError((secondStatus | firstStatus) & errorConditions, status);
    result.swap(quotient);
}

void divideWithRemainderCore(Number& quotient, Number& remainder, const Number& dividend, const Number& divisor, const Context& context, uint32_t& status)
{
    const Number* a = &dividend;
    const Number* b = &divisor;
    uint8_t signOfA = a->sign();
    uint8_t sign = a->sign() ^ b->sign();
    int64_t idealExponent = std::min(a->exponent, b->exponent);

    auto fail = [&] {
        quotient.setSpecial(0, Number::QuietNaN);
        remainder.setSpecial(0, Number::QuietNaN);
    };

    if (a->hasZeroCoefficient()) {
        if (!remainder.copy(*a, status))
            return fail();
        remainder.exponent = idealExponent;
        quotient.set(sign, 0, 0);
        return;
    }
    int64_t difference = a->adjustedExponent() - b->adjustedExponent();
    if (difference < 0) {
        // The dividend is the smaller: it is the remainder.
        if (a->exponent > b->exponent) {
            if (!shiftCoefficientLeft(remainder, *a, a->exponent - b->exponent, status))
                return fail();
            remainder.exponent = idealExponent;
        } else if (!remainder.copy(*a, status))
            return fail();
        quotient.set(sign, 0, 0);
        return;
    }
    if (difference > context.precision) {
        status |= DivisionImpossible;
        return fail();
    }

    Number aligned;
    if (a->exponent != b->exponent) {
        int64_t count = a->exponent - b->exponent;
        if (count > 0) {
            if (!shiftCoefficientLeft(aligned, *a, count, status))
                return fail();
            a = &aligned;
        } else {
            if (!shiftCoefficientLeft(aligned, *b, -count, status))
                return fail();
            b = &aligned;
        }
    }
    Number q;
    Number r;
    int64_t quotientSize = a->length - b->length + 1;
    int64_t remainderSize = b->length;
    if (!q.reserveDiscarding(quotientSize, status) || !r.reserveDiscarding(remainderSize, status))
        return fail();
    if (b->length == 1)
        r.limbs()[0] = divideLimbsByLimb(q.limbs(), a->limbs(), a->length, b->limbs()[0]);
    else if (divideLimbs(q.limbs(), r.limbs(), a->limbs(), a->length, b->limbs(), b->length) < 0) {
        status |= OutOfMemory;
        return fail();
    }
    q.flags = sign;
    q.exponent = 0;
    q.trim(quotientSize);
    if (q.digits > context.precision) {
        status |= DivisionImpossible;
        return fail();
    }
    r.flags = signOfA;
    r.exponent = idealExponent;
    r.trim(remainderSize);
    quotient.swap(q);
    remainder.swap(r);
}

void divideWithRemainder(Number& quotient, Number& rest, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    uint8_t sign = a.sign() ^ b.sign();
    if ((a.flags | b.flags) & Number::anySpecial) [[unlikely]] {
        if (propagateNaN(quotient, a, b, context, status)) {
            rest.copy(quotient, status);
            return;
        }
        if (a.isInfinite()) {
            if (b.isInfinite())
                quotient.setSpecial(0, Number::QuietNaN);
            else
                quotient.setSpecial(sign, Number::Infinite);
            rest.setSpecial(0, Number::QuietNaN);
            status |= InvalidOperation;
            return;
        }
        if (!rest.copy(a, status)) {
            quotient.setError(OutOfMemory, status);
            return;
        }
        finalize(rest, context, status);
        quotient.set(sign, 0, 0);
        return;
    }
    if (b.hasZeroCoefficient()) {
        if (a.hasZeroCoefficient()) {
            quotient.setSpecial(0, Number::QuietNaN);
            status |= DivisionUndefined;
        } else {
            quotient.setSpecial(sign, Number::Infinite);
            status |= DivisionByZero | InvalidOperation;
        }
        rest.setSpecial(0, Number::QuietNaN);
        return;
    }
    divideWithRemainderCore(quotient, rest, a, b, context, status);
    finalize(quotient, context, status);
    finalize(rest, context, status);
}

void divideInteger(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    uint8_t sign = a.sign() ^ b.sign();
    if ((a.flags | b.flags) & Number::anySpecial) [[unlikely]] {
        if (propagateNaN(result, a, b, context, status))
            return;
        if (a.isInfinite() && b.isInfinite())
            result.setError(InvalidOperation, status);
        else if (a.isInfinite())
            result.setSpecial(sign, Number::Infinite);
        else
            result.set(sign, 0, 0);
        return;
    }
    if (b.hasZeroCoefficient()) {
        if (a.hasZeroCoefficient())
            result.setError(DivisionUndefined, status);
        else {
            result.setSpecial(sign, Number::Infinite);
            status |= DivisionByZero;
        }
        return;
    }
    Number rest;
    divideWithRemainderCore(result, rest, a, b, context, status);
    finalize(result, context, status);
}

// What remainder() and remainderNear() do before they divide. True: that was all.
static bool remainderOfSpecialValues(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if ((a.flags | b.flags) & Number::anySpecial) [[unlikely]] {
        if (propagateNaN(result, a, b, context, status))
            return true;
        if (a.isInfinite()) {
            result.setError(InvalidOperation, status);
            return true;
        }
        result.copy(a, status);
        finalize(result, context, status);
        return true;
    }
    if (b.hasZeroCoefficient()) {
        result.setError(a.hasZeroCoefficient() ? DivisionUndefined : InvalidOperation, status);
        return true;
    }
    return false;
}

void remainder(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if (remainderOfSpecialValues(result, a, b, context, status))
        return;
    Number quotient;
    divideWithRemainderCore(quotient, result, a, b, context, status);
    finalize(result, context, status);
}

void remainderNear(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if (remainderOfSpecialValues(result, a, b, context, status))
        return;
    bool haveSameSign = a.sign() == b.sign();
    Number quotient;
    Number rest;
    divideWithRemainderCore(quotient, rest, a, b, context, status);
    if (!quotient.isNaN() && !rest.isNaN() && !rest.hasZeroCoefficient()) {
        int64_t difference = b.adjustedExponent() - rest.adjustedExponent();
        if (difference >= -1 && difference <= 1) {
            // The remainder for the next quotient, away from zero. The smaller of the two is wanted; of two that are equally
            // small, the one whose quotient is even.
            Context unlimited = Context::maximum();
            uint32_t ownStatus = 0;
            Number other;
            if (haveSameSign)
                subtractUnrounded(other, rest, b, unlimited, ownStatus);
            else
                addUnrounded(other, rest, b, unlimited, ownStatus);
            if (ownStatus & errorConditions)
                rest.setError(ownStatus & errorConditions, status);
            else {
                int order = compareMagnitudes(other, rest);
                if (order < 0 || (!order && quotient.isOddInteger())) {
                    if (quotient.coefficientIsAllNines() && quotient.digits == context.precision)
                        rest.setError(DivisionImpossible, status);
                    else
                        rest.swap(other);
                }
            }
        }
    }
    result.swap(rest);
    finalize(result, context, status);
}

// ---- Neighbours ----

template<bool upwards>
static void nextInDirection(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (a.isSpecial()) {
        if (propagateNaN(result, a, context, status))
            return;
        if (a.isNegative() != upwards) {
            result.copy(a, status);
            return;
        }
        // From the infinity at the other end: the largest number there is.
        result.flags = 0;
        setLargestCoefficient(result, context, status);
        if (result.isNaN())
            return;
        result.flags = upwards ? Number::Negative : 0;
        result.exponent = context.etop();
        return;
    }
    Context directed = context;
    directed.rounding = upwards ? Rounding::Ceiling : Rounding::Floor;
    uint32_t ownStatus = 0;
    Number rounded;
    if (!rounded.copy(a, status)) {
        result.swap(rounded);
        return;
    }
    // If the operand itself has to be rounded, that takes it to its neighbour.
    finalize(rounded, directed, ownStatus);
    if (ownStatus & (Inexact | errorConditions)) {
        status |= ownStatus & errorConditions;
        result.swap(rounded);
        return;
    }
    // Otherwise something smaller than anything the context can tell apart does.
    Number tiny(0, 1, context.etiny() - 1);
    ownStatus = 0;
    if (upwards)
        add(rounded, a, tiny, directed, ownStatus);
    else
        subtract(rounded, a, tiny, directed, ownStatus);
    status |= ownStatus & errorConditions;
    result.swap(rounded);
}

void nextMinus(Number& result, const Number& a, const Context& context, uint32_t& status) { nextInDirection<false>(result, a, context, status); }
void nextPlus(Number& result, const Number& a, const Context& context, uint32_t& status) { nextInDirection<true>(result, a, context, status); }

void nextToward(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if (propagateNaN(result, a, b, context, status))
        return;
    int order = compareValues(a, b);
    if (!order) {
        copySign(result, a, b, status);
        return;
    }
    if (order < 0)
        nextPlus(result, a, context, status);
    else
        nextMinus(result, a, context, status);
    if (result.isInfinite())
        status |= Overflow | Rounded | Inexact;
    else if (result.adjustedExponent() < context.emin) {
        status |= Underflow | Subnormal | Rounded | Inexact;
        if (result.isZero())
            status |= Clamped;
    }
}

// ---- Exponents ----

void quantize(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if ((a.flags | b.flags) & Number::anySpecial) [[unlikely]] {
        if (propagateNaN(result, a, b, context, status))
            return;
        if (a.isInfinite() && b.isInfinite())
            result.copy(a, status);
        else
            result.setError(InvalidOperation, status);
        return;
    }
    int64_t exponent = b.exponent;
    if (exponent > context.emax || exponent < context.etiny()) {
        result.setError(InvalidOperation, status);
        return;
    }
    if (a.hasZeroCoefficient()) {
        result.set(a.sign(), 0, exponent);
        finalize(result, context, status);
        return;
    }
    int64_t difference = a.exponent - exponent;
    if (a.digits + difference > context.precision) {
        result.setError(InvalidOperation, status);
        return;
    }
    uint32_t ownStatus = 0;
    if (difference >= 0) {
        if (!shiftCoefficientLeft(result, a, difference, status))
            return;
        result.exponent = exponent;
    } else {
        unsigned lost = shiftCoefficientRight(result, a, -difference, status);
        if (lost == shiftFailed)
            return;
        result.exponent = exponent;
        if (!applyRoundingIfItFits(result, lost, context, status))
            return;
        ownStatus |= Rounded;
        if (lost)
            ownStatus |= Inexact;
    }
    if (result.adjustedExponent() > context.emax || result.adjustedExponent() < context.etiny()) {
        result.setError(InvalidOperation, status);
        return;
    }
    status |= ownStatus;
    finalize(result, context, status);
}

void reduce(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (a.isSpecial()) {
        if (!propagateNaN(result, a, context, status))
            result.copy(a, status);
        return;
    }
    uint8_t sign = a.sign();
    if (!result.copy(a, status))
        return;
    finalize(result, context, status);
    if (result.isSpecial())
        return;
    if (result.hasZeroCoefficient()) {
        result.set(sign, 0, 0);
        return;
    }
    int64_t room = (context.clamp ? context.etop() : context.emax) - result.exponent;
    int64_t count = std::min(result.trailingZeros(), room);
    shiftCoefficientRight(result, count);
    result.exponent += count;
}

void rescale(Number& result, const Number& a, int64_t exponent, const Context& context, uint32_t& status, bool forFormatting)
{
    if (exponent > maxEmax + 1 || exponent < (forFormatting ? minEtiny - maxPrecision : minEtiny)) {
        result.setError(InvalidOperation, status);
        return;
    }
    if (a.isSpecial()) {
        result.copy(a, status);
        return;
    }
    if (a.hasZeroCoefficient()) {
        result.set(a.sign(), 0, exponent);
        return;
    }
    int64_t difference = a.exponent - exponent;
    if (difference >= 0) {
        if (a.digits + difference > maxPrecision + 1) {
            result.setError(InvalidOperation, status);
            return;
        }
        if (!shiftCoefficientLeft(result, a, difference, status))
            return;
        result.exponent = exponent;
    } else {
        unsigned lost = shiftCoefficientRight(result, a, -difference, status);
        if (lost == shiftFailed)
            return;
        result.exponent = exponent;
        applyRoundingFreely(result, lost, context, status);
        status |= Rounded;
        if (lost)
            status |= Inexact;
    }
    if (result.isSubnormal(context))
        status |= Subnormal;
}

static void roundToIntegral(Number& result, const Number& a, const Context& context, uint32_t& status, bool reports)
{
    if (a.isSpecial()) {
        if (!propagateNaN(result, a, context, status))
            result.copy(a, status);
        return;
    }
    if (a.exponent >= 0) {
        result.copy(a, status);
        return;
    }
    if (a.hasZeroCoefficient()) {
        result.set(a.sign(), 0, 0);
        return;
    }
    unsigned lost = shiftCoefficientRight(result, a, -a.exponent, status);
    if (lost == shiftFailed)
        return;
    result.exponent = 0;
    applyRoundingFreely(result, lost, context, status);
    if (reports) {
        status |= Rounded;
        if (lost)
            status |= Inexact;
    }
}

void roundToIntegralValue(Number& result, const Number& a, const Context& context, uint32_t& status) { roundToIntegral(result, a, context, status, false); }
void roundToIntegralExact(Number& result, const Number& a, const Context& context, uint32_t& status) { roundToIntegral(result, a, context, status, true); }

void logB(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (a.isSpecial()) {
        if (!propagateNaN(result, a, context, status))
            result.setSpecial(0, Number::Infinite);
        return;
    }
    if (a.hasZeroCoefficient()) {
        result.setSpecial(Number::Negative, Number::Infinite);
        status |= DivisionByZero;
        return;
    }
    result.setInt64(a.adjustedExponent());
    finalize(result, context, status);
}

void scaleB(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if (propagateNaN(result, a, b, context, status))
        return;
    if (b.exponent || b.isInfinite()) {
        result.setError(InvalidOperation, status);
        return;
    }
    uint32_t ownStatus = 0;
    uint64_t count = magnitudeToUInt64(b, ownStatus);
    uint64_t limit = 2 * static_cast<uint64_t>(context.emax + context.precision);
    if (count > limit || (ownStatus & InvalidOperation)) {
        result.setError(InvalidOperation, status);
        return;
    }
    if (a.isInfinite()) {
        result.copy(a, status);
        return;
    }
    int64_t exponent = a.exponent + static_cast<int64_t>(count) * b.arithmeticSign();
    exponent = std::clamp(exponent, underflowingExponent, overflowingExponent);
    if (!result.copy(a, status))
        return;
    result.exponent = exponent;
    finalize(result, context, status);
}

// How far shift() and rotate() are to go. False: that is not for them to do, and the result says so, or is there already.
static bool shiftCount(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status, int64_t& count)
{
    if (propagateNaN(result, a, b, context, status))
        return false;
    if (b.exponent || b.isInfinite()) {
        result.setError(InvalidOperation, status);
        return false;
    }
    uint32_t ownStatus = 0;
    count = toInt64(b, ownStatus);
    if ((ownStatus & InvalidOperation) || count > context.precision || count < -context.precision) {
        result.setError(InvalidOperation, status);
        return false;
    }
    if (a.isInfinite()) {
        result.copy(a, status);
        return false;
    }
    return true;
}

void shift(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    int64_t count;
    if (!shiftCount(result, a, b, context, status, count))
        return;
    if (count >= 0) {
        if (shiftCoefficientLeft(result, a, count, status))
            dropLeadingDigits(result, context);
        return;
    }
    if (!result.copy(a, status))
        return;
    dropLeadingDigits(result, context);
    shiftCoefficientRight(result, -count);
}

void rotate(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    int64_t count;
    if (!shiftCount(result, a, b, context, status, count))
        return;
    int64_t left = count >= 0 ? count : context.precision + count;
    int64_t right = context.precision - left;
    Number shortened;
    Number upper;
    Number lower;
    const Number* source = &a;
    if (a.digits > context.precision) {
        if (!shortened.copy(a, status)) {
            result.setError(OutOfMemory, status);
            return;
        }
        dropLeadingDigits(shortened, context);
        source = &shortened;
    }
    if (!shiftCoefficientLeft(upper, *source, left, status)) {
        result.setError(OutOfMemory, status);
        return;
    }
    dropLeadingDigits(upper, context);
    if (shiftCoefficientRight(lower, *source, right, status) == shiftFailed) {
        result.setError(OutOfMemory, status);
        return;
    }
    addUnrounded(result, upper, lower, context, status);
}

// ---- Digits as truth values ----

enum class LogicalOperation : uint8_t { And, Or, Xor };

// The digits of a limb as bits. False: one of them is neither 0 nor 1.
static PY_DECIMAL_INLINE bool limbToBits(Limb limb, uint32_t& bits)
{
    bits = 0;
    for (unsigned i = 0; limb; ++i) {
        Limb digit = limb % 10;
        limb /= 10;
        if (digit > 1)
            return false;
        bits |= static_cast<uint32_t>(digit) << i;
    }
    return true;
}

static PY_DECIMAL_INLINE Limb bitsToLimb(uint32_t bits)
{
    Limb limb = 0;
    for (unsigned i = 0; bits; ++i, bits >>= 1) {
        if (bits & 1)
            limb += powersOfTen[i];
    }
    return limb;
}

static bool isLogicalOperand(const Number& a) { return !a.isSpecial() && !a.isNegative() && !a.exponent; }

template<LogicalOperation operation>
static void logical(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status)
{
    if (!isLogicalOperand(a) || !isLogicalOperand(b)) {
        result.setError(InvalidOperation, status);
        return;
    }
    const Number* big = &a;
    const Number* small = &b;
    if (small->length > big->length)
        std::swap(big, small);
    int64_t size = big->length;
    if (!result.reserve(size, status))
        return;
    for (int64_t i = 0; i < size; ++i) {
        uint32_t x;
        uint32_t y = 0;
        if (!limbToBits(big->limbs()[i], x) || (i < small->length && !limbToBits(small->limbs()[i], y))) {
            result.setError(InvalidOperation, status);
            return;
        }
        uint32_t z = operation == LogicalOperation::And ? x & y : operation == LogicalOperation::Or ? x | y : x ^ y;
        result.limbs()[i] = bitsToLimb(z);
    }
    result.flags = 0;
    result.exponent = 0;
    result.trim(size);
    dropLeadingDigits(result, context);
}

void logicalAnd(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status) { logical<LogicalOperation::And>(result, a, b, context, status); }
void logicalOr(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status) { logical<LogicalOperation::Or>(result, a, b, context, status); }
void logicalXor(Number& result, const Number& a, const Number& b, const Context& context, uint32_t& status) { logical<LogicalOperation::Xor>(result, a, b, context, status); }

void logicalInvert(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (!isLogicalOperand(a)) {
        result.setError(InvalidOperation, status);
        return;
    }
    // As many digits as the precision, at least: those the operand does not have are zeros, and become ones.
    int64_t size = limbsForDigits(std::max(a.digits, context.precision));
    int64_t oldLength = a.length;
    if (!result.reserve(size, status))
        return;
    constexpr uint32_t allDigits = (1u << digitsPerLimb) - 1;
    for (int64_t i = 0; i < size; ++i) {
        uint32_t bits = 0;
        if (i < oldLength && !limbToBits(a.limbs()[i], bits)) {
            result.setError(InvalidOperation, status);
            return;
        }
        result.limbs()[i] = bitsToLimb(~bits & allDigits);
    }
    result.flags = 0;
    result.exponent = 0;
    result.trim(size);
    dropLeadingDigits(result, context);
}

} // namespace Decimal
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
