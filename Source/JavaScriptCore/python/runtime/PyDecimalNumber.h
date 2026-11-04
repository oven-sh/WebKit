/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace JSC {
namespace Python {
namespace Decimal {

// The arithmetic of the module _decimal: the General Decimal Arithmetic Specification, with the limits, the conditions and the
// corner cases of the library that CPython's module is built on (libmpdec, which is the reference for what every function here
// does). Nothing in this file and in PyDecimalNumber.cpp knows of the engine or of Python: numbers in, numbers and conditions out.
//
// A number is a sign, a coefficient and an exponent, or a special value. The coefficient is an integer in base 10**19, the least
// significant limb first.

using Limb = uint64_t;

static constexpr Limb radix = 10000000000000000000ull;
static constexpr int digitsPerLimb = 19;

static constexpr int64_t maxPrecision = 999999999999999999ll;
static constexpr int64_t maxEmax = 999999999999999999ll;
static constexpr int64_t minEmin = -999999999999999999ll;
static constexpr int64_t minEtiny = minEmin - (maxPrecision - 1);
// Exponents that are sure to overflow, and to underflow, when the number is finalized.
static constexpr int64_t overflowingExponent = 2000000000000000001ll;
static constexpr int64_t underflowingExponent = -4000000000000000001ll;
static constexpr int maxExponentDigits = 19;
static constexpr int maxBitsOfIEEEContext = 512;

// In the order in which the names are listed wherever they are listed.
enum class Rounding : uint8_t {
    Up,
    Down,
    Ceiling,
    Floor,
    HalfUp,
    HalfDown,
    HalfEven,
    ZeroFiveUp,
    Truncate, // As Down, but what overflows is an infinity. For use in here only.
};
static constexpr unsigned numberOfRoundings = 8;

// What an operation reports. In the order in which repr(context) lists them.
enum Condition : uint32_t {
    Clamped = 1 << 0,
    ConversionSyntax = 1 << 1,
    DivisionByZero = 1 << 2,
    DivisionImpossible = 1 << 3,
    DivisionUndefined = 1 << 4,
    Inexact = 1 << 6,
    InvalidContext = 1 << 7,
    InvalidOperation = 1 << 8,
    OutOfMemory = 1 << 9,
    FloatOperation = 1 << 10,
    Overflow = 1 << 11,
    Rounded = 1 << 12,
    Subnormal = 1 << 13,
    Underflow = 1 << 14,
};
static constexpr unsigned numberOfConditionBits = 15;
// The conditions that are one signal, InvalidOperation.
static constexpr uint32_t invalidOperationSignal = ConversionSyntax | DivisionImpossible | DivisionUndefined | InvalidContext | InvalidOperation | OutOfMemory;
// After which the result is a NaN.
static constexpr uint32_t errorConditions = invalidOperationSignal | DivisionByZero;

struct Context {
    int64_t precision { 28 };
    int64_t emax { 999999 };
    int64_t emin { -999999 };
    uint32_t traps { invalidOperationSignal | DivisionByZero | Overflow };
    uint32_t status { 0 };
    Rounding rounding { Rounding::HalfEven };
    bool clamp { false };

    int64_t etiny() const { return emin - (precision - 1); }
    int64_t etop() const { return emax - (precision - 1); }

    // In which nothing that can be represented at all is rounded.
    static Context maximum()
    {
        Context context;
        context.precision = maxPrecision;
        context.emax = maxEmax;
        context.emin = minEmin;
        return context;
    }
    // False: there is no such format.
    static bool forIEEE(int64_t bits, Context&);
};

extern const Limb powersOfTen[20];

// 1 for 0.
inline int digitCount(Limb value)
{
    value |= 1;
    int bits = 64 - __builtin_clzll(value);
    int estimate = (bits * 1233) >> 12;
    return estimate + (value >= powersOfTen[estimate]);
}

class Number {
public:
    enum Flag : uint8_t {
        Negative = 1 << 0,
        Infinite = 1 << 1,
        QuietNaN = 1 << 2,
        SignalingNaN = 1 << 3,
    };
    static constexpr uint8_t anyNaN = QuietNaN | SignalingNaN;
    static constexpr uint8_t anySpecial = Infinite | anyNaN;
    static constexpr int64_t inlineCapacity = 8;

    // A NaN without payload, until something is made of it.
    Number()
        : m_limbs(m_inline)
    {
    }
    // (-1)**sign * value * 10**exponent
    Number(uint8_t sign, Limb value, int64_t exponent)
        : m_limbs(m_inline)
    {
        set(sign, value, exponent);
    }
    // One that reads limbs that are somebody else's, for as long as they stay. It is never a result.
    enum BorrowTag { Borrow };
    Number(BorrowTag, uint8_t flags, int64_t exponent, int64_t digits, int64_t length, const Limb* limbs)
        : flags(flags)
        , exponent(exponent)
        , digits(digits)
        , length(length)
        , m_limbs(const_cast<Limb*>(limbs))
        , m_capacity(0)
    {
    }
    Number(BorrowTag, const Number& other)
        : Number(Borrow, other.flags, other.exponent, other.digits, other.length, other.m_limbs)
    {
    }
    Number(const Number&) = delete;
    Number& operator=(const Number&) = delete;
    // The same for one that exists and has not been given a long coefficient.
    void borrow(uint8_t newFlags, int64_t newExponent, int64_t newDigits, int64_t newLength, const Limb* limbs)
    {
        flags = newFlags;
        exponent = newExponent;
        digits = newDigits;
        length = newLength;
        m_limbs = const_cast<Limb*>(limbs);
        m_capacity = 0;
    }
    ~Number()
    {
        if (m_capacity > inlineCapacity)
            release();
    }

    uint8_t flags { QuietNaN };
    int64_t exponent { 0 };
    int64_t digits { 0 }; // Of the coefficient. 0: a special value without payload.
    int64_t length { 0 }; // In limbs.

    const Limb* limbs() const { return m_limbs; }
    Limb* limbs() { return m_limbs; }
    Limb top() const { return m_limbs[length - 1]; }

    bool isSpecial() const { return flags & anySpecial; }
    bool isFinite() const { return !isSpecial(); }
    bool isInfinite() const { return flags & Infinite; }
    bool isNaN() const { return flags & anyNaN; }
    bool isQuietNaN() const { return flags & QuietNaN; }
    bool isSignalingNaN() const { return flags & SignalingNaN; }
    bool isNegative() const { return flags & Negative; }
    uint8_t sign() const { return flags & Negative; }
    int arithmeticSign() const { return isNegative() ? -1 : 1; }
    // For what is known to be finite.
    bool hasZeroCoefficient() const { return !top(); }
    bool isZero() const { return isFinite() && hasZeroCoefficient(); }
    bool hasOddCoefficient() const { return m_limbs[0] & 1; }
    int64_t adjustedExponent() const { return exponent + digits - 1; }
    bool isNormal(const Context& context) const { return isFinite() && !hasZeroCoefficient() && adjustedExponent() >= context.emin; }
    bool isSubnormal(const Context& context) const { return isFinite() && !hasZeroCoefficient() && adjustedExponent() < context.emin; }
    int64_t trailingZeros() const;
    // These two are for finite numbers.
    bool isIntegerValued() const { return hasZeroCoefficient() || exponent + trailingZeros() >= 0; }
    bool isOddInteger() const;
    bool isInteger() const { return isFinite() && isIntegerValued(); }
    bool coefficientIsPowerOfTen() const;
    bool coefficientIsAllNines() const;

    void setSign(uint8_t sign) { flags = (flags & ~Negative) | sign; }
    void negate() { flags ^= Negative; }
    void set(uint8_t sign, Limb value, int64_t newExponent)
    {
        flags = sign;
        exponent = newExponent;
        if (value < radix) [[likely]] {
            m_limbs[0] = value;
            length = 1;
            digits = digitCount(value);
        } else {
            m_limbs[0] = value - radix;
            m_limbs[1] = 1;
            length = 2;
            digits = digitsPerLimb + 1;
        }
    }
    void setInt64(int64_t value) { set(value < 0 ? Negative : 0, value < 0 ? -static_cast<uint64_t>(value) : static_cast<uint64_t>(value), 0); }
    void setSpecial(uint8_t sign, uint8_t kind)
    {
        flags = sign | kind;
        exponent = digits = length = 0;
    }
    // A NaN, and the condition that explains it.
    void setError(uint32_t condition, uint32_t& status)
    {
        setSpecial(0, QuietNaN);
        status |= condition;
    }
    void setZeroCoefficient()
    {
        m_limbs[0] = 0;
        length = 1;
        digits = 1;
    }
    // From the length and the limbs.
    void updateDigits() { digits = digitCount(top()) + (length - 1) * digitsPerLimb; }
    // Limbs that are zero at the top do not count.
    void trim(int64_t candidateLength)
    {
        while (candidateLength > 1 && !m_limbs[candidateLength - 1])
            --candidateLength;
        length = candidateLength;
        updateDigits();
    }

    // Room for that many limbs. What is there stays, or not. False: there is no such room, and the number is as it was.
    bool reserve(int64_t limbCount)
    {
        if (limbCount <= m_capacity) [[likely]]
            return true;
        return grow(limbCount, true);
    }
    bool reserveDiscarding(int64_t limbCount)
    {
        if (limbCount <= m_capacity) [[likely]]
            return true;
        return grow(limbCount, false);
    }
    // The same, reporting: the number becomes a NaN.
    bool reserve(int64_t limbCount, uint32_t& status)
    {
        if (reserve(limbCount)) [[likely]]
            return true;
        setError(OutOfMemory, status);
        return false;
    }
    bool reserveDiscarding(int64_t limbCount, uint32_t& status)
    {
        if (reserveDiscarding(limbCount)) [[likely]]
            return true;
        setError(OutOfMemory, status);
        return false;
    }

    // False: out of memory, reported, and this is a NaN.
    bool copy(const Number&, uint32_t& status);
    void swap(Number&);
    // Takes over limbs that were allocated with allocateLimbs().
    void adopt(Limb*, int64_t capacity);

private:
    bool grow(int64_t limbCount, bool keep);
    void release();

    Limb* m_limbs;
    int64_t m_capacity { inlineCapacity }; // 0: the limbs are borrowed.
    Limb m_inline[inlineCapacity];
};

// Null: there is no such room.
Limb* allocateLimbs(int64_t count);
void freeLimbs(Limb*);

static inline int64_t limbsForDigits(int64_t digits) { return (digits + digitsPerLimb - 1) / digitsPerLimb; }

// ---- The operations ----
//
// Each sets `result` and adds the conditions it met to `status`; the status and the traps of the context are not looked at. The result
// may be one of the operands.

void finalize(Number&, const Context&, uint32_t& status);

// -1, 0 or 1. Neither is a NaN.
int compareValues(const Number&, const Number&);
int compareMagnitudes(const Number&, const Number&);
static constexpr int unordered = 2;
// `unordered`, and InvalidOperation, if either is a NaN.
int compareQuietly(const Number&, const Number&, uint32_t& status);
int compareTotalOrder(const Number&, const Number&);
int compareTotalOrderOfMagnitudes(const Number&, const Number&);

void compare(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void compareSignal(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void compareTotal(Number& result, const Number&, const Number&);
void compareTotalMagnitude(Number& result, const Number&, const Number&);

void absolute(Number& result, const Number&, const Context&, uint32_t& status);
void plus(Number& result, const Number&, const Context&, uint32_t& status);
void minus(Number& result, const Number&, const Context&, uint32_t& status);
void add(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void subtract(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void multiply(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void divide(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void divideInteger(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void remainder(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void remainderNear(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void divideWithRemainder(Number& quotient, Number& remainder, const Number&, const Number&, const Context&, uint32_t& status);
void fusedMultiplyAdd(Number& result, const Number&, const Number&, const Number&, const Context&, uint32_t& status);
void squareRoot(Number& result, const Number&, const Context&, uint32_t& status);
void exponential(Number& result, const Number&, const Context&, uint32_t& status);
void naturalLogarithm(Number& result, const Number&, const Context&, uint32_t& status);
void commonLogarithm(Number& result, const Number&, const Context&, uint32_t& status);
void power(Number& result, const Number& base, const Number& exponent, const Context&, uint32_t& status);
void powerModulo(Number& result, const Number& base, const Number& exponent, const Number& modulus, const Context&, uint32_t& status);

void maximum(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void maximumMagnitude(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void minimum(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void minimumMagnitude(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void nextMinus(Number& result, const Number&, const Context&, uint32_t& status);
void nextPlus(Number& result, const Number&, const Context&, uint32_t& status);
void nextToward(Number& result, const Number&, const Number&, const Context&, uint32_t& status);

void quantize(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void reduce(Number& result, const Number&, const Context&, uint32_t& status);
// To that exponent, whatever the precision and the exponent limits of the context. `forFormatting`: it may be smaller than that of any number.
void rescale(Number& result, const Number&, int64_t exponent, const Context&, uint32_t& status, bool forFormatting = false);
void roundToIntegralValue(Number& result, const Number&, const Context&, uint32_t& status);
void roundToIntegralExact(Number& result, const Number&, const Context&, uint32_t& status);

void logicalAnd(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void logicalOr(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void logicalXor(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void logicalInvert(Number& result, const Number&, const Context&, uint32_t& status);
void logB(Number& result, const Number&, const Context&, uint32_t& status);
void scaleB(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void shift(Number& result, const Number&, const Number&, const Context&, uint32_t& status);
void rotate(Number& result, const Number&, const Number&, const Context&, uint32_t& status);

void copyAbsolute(Number& result, const Number&, uint32_t& status);
void copyNegated(Number& result, const Number&, uint32_t& status);
void copySign(Number& result, const Number&, const Number& signSource, uint32_t& status);
bool haveSameQuantum(const Number&, const Number&);
const char* classOf(const Number&, const Context&);

// The coefficient times 10**count, or divided by it: the exponent stays. Not for special values.
bool shiftCoefficientLeft(Number& result, const Number&, int64_t count, uint32_t& status);
// What is returned says what was lost: 0 nothing, 5 exactly half a unit, 1 to 4 less than that, 6 to 9 more.
unsigned shiftCoefficientRight(Number&, int64_t count);

// ---- Conversions ----

// The value, if the number is an integer that fits. Otherwise InvalidOperation.
int64_t toInt64(const Number&, uint32_t& status);
uint64_t magnitudeToUInt64(const Number&, uint32_t& status);

// `text` is ASCII and ends with a NUL that is not part of `length`.
void fromString(Number& result, const char* text, size_t length, const Context&, uint32_t& status);

// |value|, an integer in base 2**64 with the least significant digit first, becomes the coefficient; then the number is finalized.
void fromBinaryMagnitude(Number& result, uint8_t sign, const uint64_t* words, size_t count, const Context&, uint32_t& status);
// The coefficient in base 2**64. `words` has room for binaryWordsFor() of them; the number that count is returned.
size_t binaryWordsFor(const Number&);
size_t coefficientToBinary(const Number&, uint64_t* words);

enum TextFlag : unsigned {
    TextUpperCase = 1 << 0,
    TextScientific = 1 << 1, // to-scientific-string
    TextEngineering = 1 << 2, // to-engineering-string
    TextExponent = 1 << 3, // Always with an exponent,
    TextFixed = 1 << 4, // or never.
    TextPercent = 1 << 5,
    TextSpaceForPlus = 1 << 6,
    TextPlus = 1 << 7,
};
// A buffer of characters that is freed with it. What is short needs no memory but its own.
class Text {
public:
    Text() = default;
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;
    ~Text()
    {
        if (data != m_inline)
            release();
    }

    // What was there is gone. False: out of memory.
    bool allocate(size_t capacity)
    {
        length = 0;
        if (capacity <= sizeof(m_inline) && data == m_inline) [[likely]]
            return true;
        return allocateSlow(capacity);
    }
    void swap(Text&);

    char* data { m_inline };
    size_t length { 0 };

private:
    bool allocateSlow(size_t capacity);
    void release();

    char m_inline[96];
};
// `pointPlace`: how many digits come before the point where the flags do not decide it. False: out of memory.
bool toText(Text&, const Number&, unsigned flags, int64_t pointPlace = 1);

// What format(number, spec) is told.
struct FormatSpec {
    int64_t minWidth { 0 };
    int64_t precision { -1 }; // -1: none.
    char type { 'g' }; // One of e E f F g G %
    char align { '>' }; // One of < > = ^, or z for padding with zeros.
    char sign { '-' };
    char fill[5] { ' ', 0, 0, 0, 0 }; // One character in UTF-8.
    // In UTF-8, of 4 bytes at most. No point: the number is printed as it is.
    const char* point { "" };
    const char* separator { "" };
    const char* grouping { "" }; // As in struct lconv.
};
// False: the text is not a spec that this knows of. For `n` and `N`, point, separator and grouping are those of the C locale: isLocaleAware.
bool parseFormatSpec(FormatSpec&, const char* text, bool capitals, bool& isLocaleAware);
bool isValidLocaleData(const FormatSpec&);
// The result is UTF-8. False: `status` says why (OutOfMemory, or InvalidOperation for what exceeds the limits).
bool format(Text&, const Number&, const FormatSpec&, const Context&, uint32_t& status);

} // namespace Decimal
} // namespace Python
} // namespace JSC
