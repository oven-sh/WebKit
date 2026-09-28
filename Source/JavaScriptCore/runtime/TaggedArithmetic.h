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

// Arithmetic for a language that has integers and floats, on JavaScript's numbers. How a number is encoded says which
// it is: one encoded as an int32 is an integer, and one encoded as a double is a float, whatever its value. So 2 and 2.0
// are different, as they are not to JavaScript, and what comes of an operation is decided by what went in:
//
//     integer op integer    an integer, or undefined if it does not fit an int32
//     anything else         a float
//     integer / integer     a float
//
// Undefined is also the answer for an operand that is not a number. It says "this is not for me".
// This is the definition. Every tier does what these do.

ALWAYS_INLINE JSValue taggedAdd(JSValue left, JSValue right)
{
    if (left.isInt32() && right.isInt32()) {
        int32_t result;
        if (!WTF::safeAdd(left.asInt32(), right.asInt32(), result))
            return jsUndefined();
        return jsNumber(result);
    }
    if (left.isNumber() && right.isNumber())
        return jsDoubleNumber(left.asNumber() + right.asNumber());
    return jsUndefined();
}

ALWAYS_INLINE JSValue taggedSub(JSValue left, JSValue right)
{
    if (left.isInt32() && right.isInt32()) {
        int32_t result;
        if (!WTF::safeSub(left.asInt32(), right.asInt32(), result))
            return jsUndefined();
        return jsNumber(result);
    }
    if (left.isNumber() && right.isNumber())
        return jsDoubleNumber(left.asNumber() - right.asNumber());
    return jsUndefined();
}

ALWAYS_INLINE JSValue taggedMul(JSValue left, JSValue right)
{
    if (left.isInt32() && right.isInt32()) {
        // There is no negative zero among the integers.
        int32_t result;
        if (!WTF::safeMultiply(left.asInt32(), right.asInt32(), result))
            return jsUndefined();
        return jsNumber(result);
    }
    if (left.isNumber() && right.isNumber())
        return jsDoubleNumber(left.asNumber() * right.asNumber());
    return jsUndefined();
}

ALWAYS_INLINE JSValue taggedDiv(JSValue left, JSValue right)
{
    if (left.isNumber() && right.isNumber())
        return jsDoubleNumber(left.asNumber() / right.asNumber());
    return jsUndefined();
}

// The float with the value of a number. It is taggedAdd(value, -0.0), which is how compiled code should ask for it.
ALWAYS_INLINE JSValue toTaggedDouble(JSValue value)
{
    if (value.isNumber())
        return jsDoubleNumber(value.asNumber());
    return jsUndefined();
}

// An object with these as functions that the optimizing compilers know: add, sub, mul, div, toDouble and isInt32. And
// newArray, which makes an array of its arguments that keeps each as it is: not one that holds only int32s, or only
// doubles, and turns what is in it from the one into the other when it is given something else.
JS_EXPORT_PRIVATE JSObject* createTaggedArithmeticObject(VM&, JSGlobalObject*);

} // namespace JSC
