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

#include "JSCJSValueInlines.h"
#include <wtf/CheckedArithmetic.h>

namespace JSC {

class JSGlobalObject;
class JSObject;
class VM;

// Arithmetic for a language that has integers and floats, on JavaScript's numbers. JavaScript encodes a whole number as an
// int32 or as a double as it sees fit, and differently from one tier to the next, so which of the two a number is cannot
// depend on that:
//
//     a number that could be encoded as an int32    an integer, however it is encoded
//     any other number                              a float: a fraction, -0.0, an infinity, NaN, 2**31 and up
//     a whole float (JSValue::WholeFloatMark)       a float, with a value that could be encoded as an int32
//
// Only this arithmetic makes whole floats. What comes of an operation is decided by what went in:
//
//     integer op integer    an integer, or undefined if it does not fit an int32
//     anything else         a float
//     integer / integer     a float
//
// Undefined is also the answer for an operand that is not a number. It says "this is not for me".
// This is the definition. Every tier does what these do.

ALWAYS_INLINE std::optional<int32_t> tryConvertToTaggedInteger(double value)
{
    if (!(value >= -2147483648.0 && value <= 2147483647.0))
        return std::nullopt;
    int32_t integer = static_cast<int32_t>(value);
    // By their bits, so that -0.0 is not 0.
    if (std::bit_cast<uint64_t>(static_cast<double>(integer)) != std::bit_cast<uint64_t>(value))
        return std::nullopt;
    return integer;
}

// The value of a number that is an integer.
ALWAYS_INLINE std::optional<int32_t> taggedInteger(JSValue value)
{
    if (value.isInt32()) {
        if (value.isWholeFloat())
            return std::nullopt;
        return value.asInt32();
    }
    if (value.isDouble())
        return tryConvertToTaggedInteger(value.asDouble());
    return std::nullopt;
}

ALWAYS_INLINE JSValue jsTaggedFloat(double value)
{
    if (auto integer = tryConvertToTaggedInteger(value))
        return jsWholeFloat(*integer);
    return jsDoubleNumber(value);
}

ALWAYS_INLINE JSValue taggedAdd(JSValue left, JSValue right)
{
    if (!left.isNumber() || !right.isNumber())
        return jsUndefined();
    auto leftInteger = taggedInteger(left);
    auto rightInteger = taggedInteger(right);
    if (leftInteger && rightInteger) {
        int32_t result;
        if (!WTF::safeAdd(*leftInteger, *rightInteger, result))
            return jsUndefined();
        return jsNumber(result);
    }
    return jsTaggedFloat(left.asNumber() + right.asNumber());
}

ALWAYS_INLINE JSValue taggedSub(JSValue left, JSValue right)
{
    if (!left.isNumber() || !right.isNumber())
        return jsUndefined();
    auto leftInteger = taggedInteger(left);
    auto rightInteger = taggedInteger(right);
    if (leftInteger && rightInteger) {
        int32_t result;
        if (!WTF::safeSub(*leftInteger, *rightInteger, result))
            return jsUndefined();
        return jsNumber(result);
    }
    return jsTaggedFloat(left.asNumber() - right.asNumber());
}

ALWAYS_INLINE JSValue taggedMul(JSValue left, JSValue right)
{
    if (!left.isNumber() || !right.isNumber())
        return jsUndefined();
    auto leftInteger = taggedInteger(left);
    auto rightInteger = taggedInteger(right);
    if (leftInteger && rightInteger) {
        // There is no negative zero among the integers.
        int32_t result;
        if (!WTF::safeMultiply(*leftInteger, *rightInteger, result))
            return jsUndefined();
        return jsNumber(result);
    }
    return jsTaggedFloat(left.asNumber() * right.asNumber());
}

ALWAYS_INLINE JSValue taggedDiv(JSValue left, JSValue right)
{
    if (!left.isNumber() || !right.isNumber())
        return jsUndefined();
    return jsTaggedFloat(left.asNumber() / right.asNumber());
}

// The float with the value of a number. It is taggedAdd(value, -0.0), which is how compiled code should ask for it.
ALWAYS_INLINE JSValue toTaggedFloat(JSValue value)
{
    if (value.isNumber())
        return jsTaggedFloat(value.asNumber());
    return jsUndefined();
}

// An object with these as functions that the optimizing compilers know: add, sub, mul, div, toFloat and isInt. And newArray,
// which makes an array of its arguments that is contiguous from the start, and does not have to become so when it is first
// given a whole float.
JS_EXPORT_PRIVATE JSObject* createTaggedArithmeticObject(VM&, JSGlobalObject*);

} // namespace JSC
