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
JSValue numberUnaryOperation(JSGlobalObject*, UnaryOperator, JSValue);
// Less than, equal to or greater than zero. Nothing if they are not both numbers, or one is a NaN (`isUnordered`).
std::optional<int> numberCompare(const Number&, const Number&, bool& isUnordered);

JSValue normalizeBigInt(JSValue);
JSBigInt* toBigInt(JSGlobalObject*, const Number&);
double toDouble(JSGlobalObject*, ThrowScope&, const Number&); // Raises OverflowError if it is an int too large.
int64_t hashOfNumber(JSGlobalObject*, const Number&);
int64_t hashOfDouble(double);
// A negative number to a power that is not whole: a complex number.
JSValue powerOfNegativeFloat(JSGlobalObject*, double base, double exponent);
String reprOfInt(JSGlobalObject*, const Number&, unsigned radix = 10);

} } // namespace JSC::Python
