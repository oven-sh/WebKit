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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PythonConstantFolding.h"

#include "PythonNumbers.h"
#include "PythonText.h"
#include <wtf/text/StringView.h>

namespace JSC { namespace Python {

using Kind = ConstantValue::Kind;
using Int = __int128;
using UnsignedInt = unsigned __int128;

// As in Python/flowgraph.c.
static constexpr unsigned maxIntSize = 128; // Bits.
static constexpr unsigned maxCollectionSize = 256; // Items.
static constexpr unsigned maxStringSize = 4096; // Characters.
static constexpr unsigned maxTotalItems = 1024; // With what is in what is in it.

// ---- int, of which bool is a kind

static bool isInt(const ConstantValue& value)
{
    return value.kind == Kind::Integer || value.kind == Kind::BigInteger || value.kind == Kind::True || value.kind == Kind::False;
}

static bool isBool(const ConstantValue& value) { return value.kind == Kind::True || value.kind == Kind::False; }

// Nothing if it is not an int, or is one of more than 127 bits.
static std::optional<Int> intOf(const ConstantValue& value)
{
    switch (value.kind) {
    case Kind::True:
        return 1;
    case Kind::False:
        return 0;
    case Kind::Integer:
        return value.isNegative ? -static_cast<Int>(value.bits) : static_cast<Int>(value.bits);
    case Kind::BigInteger: {
        UnsignedInt magnitude = 0;
        constexpr UnsignedInt most = static_cast<UnsignedInt>(std::numeric_limits<Int>::max());
        for (unsigned i = 0; i < value.text.length(); ++i) {
            char16_t character = value.text[i];
            if (!isASCIIAlphanumeric(character))
                return std::nullopt;
            unsigned digit = isASCIIDigit(character) ? character - '0' : toASCIILowerUnchecked(character) - 'a' + 10;
            if (digit >= value.radix || magnitude > (most - digit) / value.radix)
                return std::nullopt;
            magnitude = magnitude * value.radix + digit;
        }
        return value.isNegative ? -static_cast<Int>(magnitude) : static_cast<Int>(magnitude);
    }
    default:
        return std::nullopt;
    }
}

static UnsignedInt magnitudeOf(Int value) { return value < 0 ? -static_cast<UnsignedInt>(value) : static_cast<UnsignedInt>(value); }

static ConstantValue fromInt(Int value)
{
    UnsignedInt magnitude = magnitudeOf(value);
    if (magnitude <= std::numeric_limits<uint64_t>::max())
        return { Kind::Integer, 10, value < 0, static_cast<uint64_t>(magnitude) };
    Vector<Latin1Character, 40> digits;
    for (; magnitude; magnitude /= 10)
        digits.append('0' + static_cast<unsigned>(magnitude % 10));
    digits.reverse();
    return { Kind::BigInteger, 10, value < 0, 0, String(digits.span()) };
}

static ConstantValue fromBool(bool value) { return { value ? Kind::True : Kind::False }; }

static unsigned bitLength(Int value)
{
    UnsignedInt magnitude = magnitudeOf(value);
    uint64_t high = static_cast<uint64_t>(magnitude >> 64);
    return high ? 128 - std::countl_zero(high) : 64 - std::countl_zero(static_cast<uint64_t>(magnitude));
}

static std::optional<ConstantValue> foldInts(BinaryOperator op, Int a, Int b)
{
    Int result;
    switch (op) {
    case BinaryOperator::Add:
        if (__builtin_add_overflow(a, b, &result))
            return std::nullopt;
        return fromInt(result);
    case BinaryOperator::Sub:
        if (__builtin_sub_overflow(a, b, &result))
            return std::nullopt;
        return fromInt(result);
    case BinaryOperator::Mult:
        if (a && b && bitLength(a) + bitLength(b) > maxIntSize)
            return std::nullopt;
        if (__builtin_mul_overflow(a, b, &result))
            return std::nullopt;
        return fromInt(result);
    case BinaryOperator::FloorDiv:
    case BinaryOperator::Mod: {
        // It is rounded down, and what is left over has the sign of what it was divided by. -2**127 // -1 is out of reach, since neither can be written.
        if (!b || (b == -1 && a == std::numeric_limits<Int>::min()))
            return std::nullopt;
        Int quotient = a / b;
        Int remainder = a % b;
        if (remainder && (remainder < 0) != (b < 0)) {
            --quotient;
            remainder += b;
        }
        return fromInt(op == BinaryOperator::Mod ? remainder : quotient);
    }
    case BinaryOperator::Div: {
        // If both are floats exactly there is one rounding, which is the one that is wanted.
        constexpr UnsignedInt exact = static_cast<UnsignedInt>(1) << 53;
        if (!b || magnitudeOf(a) > exact || magnitudeOf(b) > exact)
            return std::nullopt;
        return ConstantValue { Kind::Float, 10, false, std::bit_cast<uint64_t>(static_cast<double>(a) / static_cast<double>(b)) };
    }
    case BinaryOperator::Pow: {
        // To a negative power it is a float, as powerOfInts() of PythonNumbers.cpp has it.
        if (b < 0) {
            if (!a)
                return std::nullopt;
            return ConstantValue { Kind::Float, 10, false, std::bit_cast<uint64_t>(std::pow(static_cast<double>(a), static_cast<double>(b))) };
        }
        if (a && b > 0 && (b > maxIntSize || bitLength(a) > maxIntSize / static_cast<unsigned>(b)))
            return std::nullopt;
        result = 1;
        for (Int i = 0; i < b && a; ++i) {
            if (__builtin_mul_overflow(result, a, &result))
                return std::nullopt;
        }
        if (!a && b)
            result = 0;
        return fromInt(result);
    }
    case BinaryOperator::LShift:
        if (b < 0)
            return std::nullopt;
        if (!a || !b)
            return fromInt(a);
        if (b > maxIntSize || bitLength(a) > maxIntSize - static_cast<unsigned>(b) || bitLength(a) + static_cast<unsigned>(b) > 127)
            return std::nullopt;
        return fromInt(static_cast<Int>(static_cast<UnsignedInt>(a) << static_cast<unsigned>(b)));
    case BinaryOperator::RShift:
        if (b < 0)
            return std::nullopt;
        return fromInt(b >= 127 ? (a < 0 ? -1 : 0) : a >> static_cast<unsigned>(b));
    case BinaryOperator::BitOr:
        return fromInt(a | b);
    case BinaryOperator::BitXor:
        return fromInt(a ^ b);
    case BinaryOperator::BitAnd:
        return fromInt(a & b);
    case BinaryOperator::MatMult:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- float and complex

static ConstantValue fromDouble(double value) { return { Kind::Float, 10, false, std::bit_cast<uint64_t>(value) }; }

// An int or a float, as a float.
static std::optional<double> realOf(const ConstantValue& value)
{
    if (value.kind == Kind::Float)
        return std::bit_cast<double>(value.bits);
    if (auto integer = intOf(value))
        return static_cast<double>(*integer);
    return std::nullopt;
}

static bool isComplex(const ConstantValue& value) { return value.kind == Kind::Imaginary || value.kind == Kind::Complex; }

struct ComplexParts {
    double real;
    double imaginary;
};

static ComplexParts complexOf(const ConstantValue& value)
{
    if (value.kind == Kind::Imaginary)
        return { 0, std::bit_cast<double>(value.bits) };
    return { std::bit_cast<double>(value.bits), std::bit_cast<double>(value.imaginaryBits) };
}

// What has nought for its real part, and not nought with a minus sign, is what could have been written.
static ConstantValue fromComplex(ComplexParts value)
{
    if (!std::bit_cast<uint64_t>(value.real))
        return { Kind::Imaginary, 10, false, std::bit_cast<uint64_t>(value.imaginary) };
    return { Kind::Complex, 10, false, std::bit_cast<uint64_t>(value.real), { }, std::bit_cast<uint64_t>(value.imaginary) };
}

static std::optional<ConstantValue> foldFloats(BinaryOperator op, double a, double b)
{
    switch (op) {
    case BinaryOperator::Add:
        return fromDouble(a + b);
    case BinaryOperator::Sub:
        return fromDouble(a - b);
    case BinaryOperator::Mult:
        return fromDouble(a * b);
    case BinaryOperator::Div:
        if (!b)
            return std::nullopt;
        return fromDouble(a / b);
    case BinaryOperator::FloorDiv:
    case BinaryOperator::Mod: {
        if (!b)
            return std::nullopt;
        double quotient;
        double remainder;
        floatDivmod(a, b, quotient, remainder);
        return fromDouble(op == BinaryOperator::Mod ? remainder : quotient);
    }
    case BinaryOperator::Pow: {
        double result;
        if (powerOfFloats(a, b, result) != FloatPower::IsFloat)
            return std::nullopt;
        return fromDouble(result);
    }
    default:
        return std::nullopt;
    }
}

// As complexBinary() of PythonComplex.cpp has it. A real number is not made complex first: it has no imaginary part, not even a zero with a sign.
static std::optional<ConstantValue> foldComplex(BinaryOperator op, const ConstantValue& left, const ConstantValue& right)
{
    bool leftIsReal = !isComplex(left);
    bool rightIsReal = !isComplex(right);
    ComplexParts a { 0, 0 };
    ComplexParts b { 0, 0 };
    if (leftIsReal) {
        auto real = realOf(left);
        if (!real)
            return std::nullopt;
        a.real = *real;
    } else
        a = complexOf(left);
    if (rightIsReal) {
        auto real = realOf(right);
        if (!real)
            return std::nullopt;
        b.real = *real;
    } else
        b = complexOf(right);
    switch (op) {
    case BinaryOperator::Add:
        return fromComplex(leftIsReal ? ComplexParts { a.real + b.real, b.imaginary } : rightIsReal ? ComplexParts { a.real + b.real, a.imaginary } : ComplexParts { a.real + b.real, a.imaginary + b.imaginary });
    case BinaryOperator::Sub:
        return fromComplex(leftIsReal ? ComplexParts { a.real - b.real, -b.imaginary } : rightIsReal ? ComplexParts { a.real - b.real, a.imaginary } : ComplexParts { a.real - b.real, a.imaginary - b.imaginary });
    default:
        return std::nullopt;
    }
}

// ---- str, bytes and tuple

static bool isSequence(const ConstantValue& value) { return value.kind == Kind::String || value.kind == Kind::Bytes || value.kind == Kind::Tuple; }

// In characters, of which a surrogate pair is one.
static unsigned lengthOf(const ConstantValue& value)
{
    if (value.kind == Kind::Tuple)
        return value.elements.size();
    if (value.kind == Kind::Bytes || value.text.is8Bit())
        return value.text.length();
    unsigned count = 0;
    for ([[maybe_unused]] char32_t character : StringView(value.text).codePoints())
        ++count;
    return count;
}

// const_folding_check_complexity()
static int64_t checkComplexity(const ConstantValue& value, int64_t limit)
{
    if (value.kind != Kind::Tuple)
        return limit;
    limit -= value.elements.size();
    for (unsigned i = 0; limit >= 0 && i < value.elements.size(); ++i)
        limit = checkComplexity(value.elements[i], limit);
    return limit;
}

// const_folding_safe_multiply()
static std::optional<ConstantValue> foldRepeat(const ConstantValue& sequence, Int count)
{
    unsigned size = lengthOf(sequence);
    if (size) {
        unsigned most = (sequence.kind == Kind::Tuple ? maxCollectionSize : maxStringSize) / size;
        if (count < 0 || count > most)
            return std::nullopt;
        if (sequence.kind == Kind::Tuple && count && checkComplexity(sequence, maxTotalItems / static_cast<int64_t>(count)) < 0)
            return std::nullopt;
    } else
        count = 0;
    ConstantValue result { sequence.kind };
    if (sequence.kind == Kind::Tuple) {
        for (Int i = 0; i < count; ++i)
            result.elements.appendVector(sequence.elements);
        return result;
    }
    TextBuilder builder;
    for (Int i = 0; i < count; ++i)
        builder.append(sequence.text);
    result.text = builder.tryFinish();
    if (result.text.isNull())
        return std::nullopt;
    return result;
}

static std::optional<ConstantValue> foldConcatenation(const ConstantValue& left, const ConstantValue& right)
{
    ConstantValue result { left.kind };
    if (left.kind == Kind::Tuple) {
        result.elements.appendVector(left.elements);
        result.elements.appendVector(right.elements);
        return result;
    }
    result.text = concatenate(left.text, right.text);
    if (result.text.isNull())
        return std::nullopt;
    return result;
}

// ---- All of them

std::optional<ConstantValue> foldBinaryOperation(BinaryOperator op, const ConstantValue& left, const ConstantValue& right)
{
    if (isInt(left) && isInt(right)) {
        auto a = intOf(left);
        auto b = intOf(right);
        if (!a || !b)
            return std::nullopt;
        // Of two bools these are a bool.
        if (isBool(left) && isBool(right) && (op == BinaryOperator::BitAnd || op == BinaryOperator::BitOr || op == BinaryOperator::BitXor))
            return fromBool(op == BinaryOperator::BitAnd ? *a & *b : op == BinaryOperator::BitOr ? *a | *b : *a ^ *b);
        return foldInts(op, *a, *b);
    }
    if (isComplex(left) || isComplex(right))
        return foldComplex(op, left, right);
    if (left.kind == Kind::Float || right.kind == Kind::Float) {
        auto a = realOf(left);
        auto b = realOf(right);
        if (!a || !b)
            return std::nullopt;
        return foldFloats(op, *a, *b);
    }
    if (op == BinaryOperator::Add && isSequence(left) && left.kind == right.kind)
        return foldConcatenation(left, right);
    if (op == BinaryOperator::Mult && isSequence(left) && isInt(right)) {
        if (auto count = intOf(right))
            return foldRepeat(left, *count);
    }
    if (op == BinaryOperator::Mult && isInt(left) && isSequence(right)) {
        if (auto count = intOf(left))
            return foldRepeat(right, *count);
    }
    return std::nullopt;
}

static bool isTrue(const ConstantValue& value)
{
    switch (value.kind) {
    case Kind::None:
    case Kind::False:
        return false;
    case Kind::True:
    case Kind::Ellipsis:
    case Kind::Code:
        return true;
    case Kind::Integer:
        return value.bits;
    case Kind::BigInteger:
        return true;
    case Kind::Float:
    case Kind::Imaginary:
        return std::bit_cast<double>(value.bits);
    case Kind::Complex:
        return std::bit_cast<double>(value.bits) || std::bit_cast<double>(value.imaginaryBits);
    case Kind::String:
    case Kind::Bytes:
        return value.text.length();
    case Kind::Tuple:
    case Kind::FrozenSet:
        return value.elements.size();
    case Kind::Slice:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<ConstantValue> foldUnaryOperation(UnaryOperator op, const ConstantValue& operand)
{
    if (op == UnaryOperator::Not)
        return fromBool(!isTrue(operand));
    if (isInt(operand)) {
        // ~True is on its way out, and says so when it is run.
        if (op == UnaryOperator::Invert && isBool(operand))
            return std::nullopt;
        auto value = intOf(operand);
        if (!value) {
            // However large it is, all that there is to do is to say which side of nought it is.
            if (op == UnaryOperator::Invert)
                return std::nullopt;
            ConstantValue result = operand;
            result.isNegative = operand.isNegative != (op == UnaryOperator::USub);
            return result;
        }
        return fromInt(op == UnaryOperator::UAdd ? *value : op == UnaryOperator::USub ? -*value : ~*value);
    }
    if (op == UnaryOperator::Invert)
        return std::nullopt;
    if (operand.kind == Kind::Float)
        return fromDouble(op == UnaryOperator::USub ? -std::bit_cast<double>(operand.bits) : std::bit_cast<double>(operand.bits));
    if (isComplex(operand)) {
        ComplexParts value = complexOf(operand);
        return fromComplex(op == UnaryOperator::USub ? ComplexParts { -value.real, -value.imaginary } : value);
    }
    return std::nullopt;
}

// PySlice_Unpack() and PySlice_AdjustIndices()
static std::optional<ConstantValue> foldSlicing(const ConstantValue& sequence, const ConstantValue& slice)
{
    // What is past either end of anything is as good as the end.
    constexpr Int most = std::numeric_limits<int64_t>::max();
    auto part = [&] (unsigned i) -> std::optional<std::optional<Int>> {
        if (slice.elements[i].kind == Kind::None)
            return std::optional<Int> { };
        if (slice.elements[i].kind == Kind::BigInteger && !intOf(slice.elements[i]))
            return std::optional<Int> { slice.elements[i].isNegative ? -most : most };
        auto value = intOf(slice.elements[i]);
        if (!value)
            return std::nullopt;
        return std::optional<Int> { std::clamp(*value, -most, most) };
    };
    auto givenStart = part(0);
    auto givenStop = part(1);
    auto givenStep = part(2);
    if (!givenStart || !givenStop || !givenStep)
        return std::nullopt;
    Int step = givenStep->value_or(1);
    if (!step)
        return std::nullopt;
    // A str is gone through by the character, and what has half of a pair in it might come out with the halves together.
    if (sequence.kind == Kind::String && !sequence.text.is8Bit()) {
        for (char16_t unit : sequence.text.span16()) {
            if (U16_IS_SURROGATE(unit))
                return std::nullopt;
        }
    }
    Int length = lengthOf(sequence);
    auto adjust = [&] (std::optional<Int> given, Int ifNone) {
        if (!given)
            return ifNone;
        Int value = *given;
        if (value < 0) {
            value += length;
            return value < 0 ? (step < 0 ? static_cast<Int>(-1) : static_cast<Int>(0)) : value;
        }
        return value >= length ? (step < 0 ? length - 1 : length) : value;
    };
    Int start = adjust(*givenStart, step < 0 ? length - 1 : 0);
    Int stop = adjust(*givenStop, step < 0 ? -1 : length);
    ConstantValue result { sequence.kind };
    TextBuilder builder;
    for (Int i = start; step < 0 ? i > stop : i < stop; i += step) {
        if (sequence.kind == Kind::Tuple)
            result.elements.append(sequence.elements[static_cast<unsigned>(i)]);
        else
            builder.append(sequence.text[static_cast<unsigned>(i)]);
    }
    if (sequence.kind != Kind::Tuple) {
        result.text = builder.tryFinish();
        if (result.text.isNull())
            return std::nullopt;
    }
    return result;
}

std::optional<ConstantValue> foldSubscript(const ConstantValue& sequence, const ConstantValue& indexValue)
{
    if (isSequence(sequence) && indexValue.kind == Kind::Slice)
        return foldSlicing(sequence, indexValue);
    if (!isSequence(sequence) || !isInt(indexValue))
        return std::nullopt;
    auto index = intOf(indexValue);
    if (!index)
        return std::nullopt;
    Int length = lengthOf(sequence);
    Int position = *index < 0 ? *index + length : *index;
    if (position < 0 || position >= length)
        return std::nullopt;
    unsigned i = static_cast<unsigned>(position);
    if (sequence.kind == Kind::Tuple)
        return sequence.elements[i];
    if (sequence.kind == Kind::Bytes)
        return ConstantValue { Kind::Integer, 10, false, static_cast<uint64_t>(sequence.text[i]) };
    if (sequence.text.is8Bit())
        return ConstantValue { Kind::String, 10, false, 0, sequence.text.substring(i, 1) };
    unsigned offset = 0;
    for (char32_t character : StringView(sequence.text).codePoints()) {
        unsigned width = U16_LENGTH(character);
        if (!i--)
            return ConstantValue { Kind::String, 10, false, 0, sequence.text.substring(offset, width) };
        offset += width;
    }
    return std::nullopt;
}

unsigned hashOfConstant(const ConstantValue& value)
{
    unsigned hash = computeHash(static_cast<uint8_t>(value.kind), value.radix, value.isNegative, value.bits, value.imaginaryBits, value.text.isNull() ? 0 : value.text.hash());
    for (auto& element : value.elements)
        hash = pairIntHash(hash, hashOfConstant(element));
    return hash;
}

bool isSmallInt(const ConstantValue& value)
{
    return value.kind == Kind::Integer && !value.isNegative && value.bits < 256;
}

bool canBeShared(const ConstantValue& value)
{
    if (value.kind == Kind::Bytes)
        return false;
    for (auto& element : value.elements) {
        if (!canBeShared(element))
            return false;
    }
    return true;
}

bool isObjectOfRealm(const ConstantValue& value)
{
    switch (value.kind) {
    case Kind::Ellipsis:
    case Kind::Imaginary:
    case Kind::Complex:
    case Kind::Bytes:
    case Kind::Tuple:
    case Kind::FrozenSet:
    case Kind::Slice:
    case Kind::Code:
        return true;
    default:
        return false;
    }
}

} } // namespace JSC::Python
