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

#pragma once

#include "JSBigInt.h"
#include "PythonOperations.h"
#include "TaggedArithmetic.h"

namespace JSC { namespace Python {

// int, float and bool. Inside the runtime: what the rest of it needs to know about numbers.

struct Number {
    enum class Kind : uint8_t {
        None, // Not a number.
        Small, // An int that fits an int32, or a bool.
        Big,
        Float,
    };
    Kind kind { Kind::None };
    int32_t small { 0 };
    JSBigInt* big { nullptr };
    double real { 0 };

    bool isInt() const { return kind == Kind::Small || kind == Kind::Big; }
    explicit operator bool() const { return kind != Kind::None; }
};

// Also of an instance of a class derived from one of them.
Number classify(JSValue);

// Empty if these are not both numbers, and then nothing is raised.
JSValue numberBinaryOperation(JSGlobalObject*, BinaryOperator, JSValue, JSValue);
// What // and % come to, of floats of which the second is not nought, and what ** comes to if that is a float. They are for the compiler as well, which works out what it can beforehand and has no realm to raise anything in.
void floatDivmod(double left, double right, double& quotient, double& remainder);
enum class FloatPower : uint8_t { IsFloat, IsOfZero, IsComplex, IsTooLarge };
FloatPower powerOfFloats(double base, double exponent, double& result);
JSValue numberUnaryOperation(JSGlobalObject*, UnaryOperator, JSValue);
// Less than, equal to or greater than zero. Nothing if they are not both numbers, or one is a NaN (`isUnordered`).
std::optional<int> numberCompare(const Number&, const Number&, bool& isUnordered);

JSValue normalizeBigInt(JSValue);
JSBigInt* toBigInt(JSGlobalObject*, const Number&);
int64_t bitLengthOfInt(const Number&); // int.bit_length()
JSValue greatestCommonDivisor(JSGlobalObject*, JSValue, JSValue); // _PyLong_GCD(), of ints. What it comes to is never less than nothing. Empty if it raised.
// An int, from how large it is written in base 2**64 with the least first, and the other way about. There may be noughts at the top of what is given, and there are none at the top of what is returned, so nought is
// nothing at all. The first is empty if there is no room for it, and then MemoryError has been raised.
JSValue intFromDigits(JSGlobalObject*, std::span<const uint64_t>, bool isNegative = false);
Vector<uint64_t, 4> digitsOfInt(const Number&);
double toDouble(JSGlobalObject*, ThrowScope&, const Number&); // Raises OverflowError if it is an int too large.
// PyComplex_AsCComplex(): of a complex, of what has __complex__(), or of what can be made a float, which is then the real part. Nothing if it raised.
std::optional<std::pair<double, double>> toComplexParts(JSGlobalObject*, JSValue);
// PyFloat_Pack2(), PyFloat_Pack4() and PyFloat_Pack8() of CPython's Objects/floatobject.c: a float as two, four or eight bytes, the least first or the most. The first two are false, having raised OverflowError, if there is
// no room for it. And the other way about, which cannot go wrong.
bool packFloat2(JSGlobalObject*, double, std::span<uint8_t, 2>, bool isLittleEndian);
bool packFloat4(JSGlobalObject*, double, std::span<uint8_t, 4>, bool isLittleEndian);
void packFloat8(double, std::span<uint8_t, 8>, bool isLittleEndian);
double unpackFloat2(std::span<const uint8_t, 2>, bool isLittleEndian);
double unpackFloat4(std::span<const uint8_t, 4>, bool isLittleEndian);
double unpackFloat8(std::span<const uint8_t, 8>, bool isLittleEndian);
int64_t hashOfNumber(JSGlobalObject*, const Number&);
int64_t hashOfDouble(double);
// A negative number to a power that is not whole: a complex number.
JSValue powerOfNegativeFloat(JSGlobalObject*, double base, double exponent);
String reprOfInt(JSGlobalObject*, const Number&, unsigned radix = 10);

} } // namespace JSC::Python
