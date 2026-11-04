/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSBigInt.h"
#include "PyBytecode.h"
#include "PyObject.h"
#include <cmath>

namespace JSC {
namespace Python {

// ---- What is an int and what is a float ----
//
// A JS number does not say which it is: how one is tagged depends on how it was computed. So its value decides.
// An integer that JS can hold exactly ("small" below) is an int. A float with such a value is boxed, in a PyFloat.
// -0 is such an integer, 0: it is what JS makes of `-i` and of `0 * -1`. Python's -0.0 is boxed, as its 0.0 is.

static constexpr double maxSmallInt = 9007199254740991.0; // Number.MAX_SAFE_INTEGER
static constexpr int64_t maxSmallInt64 = 9007199254740991;

ALWAYS_INLINE bool isSmallIntValue(double value)
{
    if (!(std::abs(value) <= maxSmallInt))
        return false;
    return static_cast<double>(static_cast<int64_t>(value)) == value;
}

ALWAYS_INLINE bool isSmallInt(JSValue value) { return value.isInt32() || (value.isDouble() && isSmallIntValue(value.asDouble())); }
ALWAYS_INLINE int64_t smallIntValue(JSValue value)
{
    ASSERT(isSmallInt(value));
    return value.isInt32() ? value.asInt32() : static_cast<int64_t>(value.asDouble());
}
// The value of an int or a float that is a number. The int 0 has no sign.
ALWAYS_INLINE double numberValue(JSValue value) { return value.isInt32() ? value.asInt32() : value.asDouble() + 0.0; }
ALWAYS_INLINE bool fitsSmallInt(int64_t value) { return value >= -maxSmallInt64 && value <= maxSmallInt64; }
ALWAYS_INLINE JSValue smallInt(int64_t value)
{
    ASSERT(fitsSmallInt(value));
    if (static_cast<int32_t>(value) == value) [[likely]]
        return jsNumber(static_cast<int32_t>(value));
    return jsDoubleNumber(static_cast<double>(value));
}

// An instance of a class derived from int. (bool is not one of these: True and False are the JS booleans.)
class PyInt final : public PyObject {
public:
    PY_DECLARE_CELL(PyInt, PyObject, PyIntType);
    DECLARE_VISIT_CHILDREN;

    static PyInt* create(VM&, Structure*, PyType*, JSValue value);
    JSValue value() const { return m_value.get(); } // A small int or a JSBigInt.

private:
    PyInt(VM&, Structure*, PyType*, JSValue);

    WriteBarrier<Unknown> m_value;
};

// A float whose value is a small int, or an instance of a class derived from float.
class PyFloat final : public PyObject {
public:
    PY_DECLARE_CELL(PyFloat, PyObject, PyFloatType);

    static PyFloat* create(VM&, Structure*, PyType*, double);
    double value() const { return m_value; }

private:
    PyFloat(VM&, Structure*, PyType*, double);

    double m_value;
};

class PyComplex final : public PyObject {
public:
    PY_DECLARE_CELL(PyComplex, PyObject, PyComplexType);

    static PyComplex* create(VM&, Structure*, PyType*, double real, double imaginary);
    static PyComplex* create(JSGlobalObject*, double real, double imaginary);
    double real() const { return m_real; }
    double imaginary() const { return m_imaginary; }

private:
    PyComplex(VM&, Structure*, PyType*, double real, double imaginary);

    double m_real;
    double m_imaginary;
};

// ---- int ----

// int itself: not bool, not a subclass.
ALWAYS_INLINE bool isExactInt(JSValue value) { return isSmallInt(value) || (value && value.isHeapBigInt()); }
// int, bool, or a subclass.
ALWAYS_INLINE bool isInt(JSValue value) { return isExactInt(value) || value.isBoolean() || PyInt::is(value); }
// The value of any of those, as an exact int.
ALWAYS_INLINE JSValue intValue(JSValue value)
{
    ASSERT(isInt(value));
    if (value.isBoolean())
        return jsNumber(static_cast<int32_t>(value.asBoolean()));
    if (value.isCell() && value.asCell()->type() == PyIntType)
        return uncheckedDowncast<PyInt>(value.asCell())->value();
    return value;
}

JSValue makeBigInt(JSGlobalObject*, int64_t);
JSValue makeBigInt(JSGlobalObject*, uint64_t);
ALWAYS_INLINE JSValue makeInt(JSGlobalObject* globalObject, int64_t value)
{
    if (fitsSmallInt(value)) [[likely]]
        return smallInt(value);
    return makeBigInt(globalObject, value);
}
ALWAYS_INLINE JSValue makeInt(JSGlobalObject* globalObject, uint64_t value)
{
    if (value <= static_cast<uint64_t>(maxSmallInt64)) [[likely]]
        return smallInt(static_cast<int64_t>(value));
    return makeBigInt(globalObject, value);
}
inline JSValue makeInt(JSGlobalObject*, int32_t value) { return jsNumber(value); }
inline JSValue makeInt(JSGlobalObject*, uint32_t value) { return jsNumber(value); }
// A number if it is small enough.
JSValue normalizeInt(JSValue bigIntOrNumber);
JSBigInt* toBigInt(JSGlobalObject*, JSValue exactInt);
// The integral part. Throws for an infinity or a NaN.
JSValue intFromDouble(JSGlobalObject*, double);

// nullopt: out of range.
std::optional<int64_t> tryInt64(JSValue exactInt);
std::optional<uint64_t> tryUInt64(JSValue exactInt);
// Rounded to nearest. An infinity if it is too large.
double intToDouble(JSValue exactInt);
int intSign(JSValue exactInt); // -1, 0 or 1
bool intIsOdd(JSValue exactInt);
uint64_t intBitLength(JSValue exactInt);
int compareInts(JSValue a, JSValue b); // -1, 0 or 1
int64_t hashOfInt(JSValue exactInt);
String intToString(JSGlobalObject*, JSValue exactInt, unsigned radix = 10);
// `text` is digits in the radix, with no sign, prefix or underscores.
JSValue intFromDigits(JSGlobalObject*, StringView text, unsigned radix);

// The operands are exact ints. Empty: an exception.
JSValue intAdd(JSGlobalObject*, JSValue, JSValue);
JSValue intSubtract(JSGlobalObject*, JSValue, JSValue);
JSValue intMultiply(JSGlobalObject*, JSValue, JSValue);
JSValue intFloorDivide(JSGlobalObject*, JSValue, JSValue);
JSValue intModulo(JSGlobalObject*, JSValue, JSValue);
bool intDivMod(JSGlobalObject*, JSValue, JSValue, JSValue& quotient, JSValue& remainder);
JSValue intTrueDivide(JSGlobalObject*, JSValue, JSValue);
JSValue intPower(JSGlobalObject*, JSValue base, JSValue exponent, JSValue modulus = { });
JSValue intLeftShift(JSGlobalObject*, JSValue, JSValue);
JSValue intRightShift(JSGlobalObject*, JSValue, JSValue);
JSValue intAnd(JSGlobalObject*, JSValue, JSValue);
JSValue intOr(JSGlobalObject*, JSValue, JSValue);
JSValue intXor(JSGlobalObject*, JSValue, JSValue);
JSValue intNegate(JSGlobalObject*, JSValue);
JSValue intInvert(JSGlobalObject*, JSValue);
JSValue intAbsolute(JSGlobalObject*, JSValue);

// ---- float ----

// float or a subclass.
ALWAYS_INLINE bool isFloat(JSValue value) { return value.isDouble() ? !isSmallIntValue(value.asDouble()) : PyFloat::is(value); }
ALWAYS_INLINE double floatValue(JSValue value)
{
    ASSERT(isFloat(value));
    return value.isDouble() ? value.asDouble() : uncheckedDowncast<PyFloat>(value.asCell())->value();
}
JSValue makeFloatSlow(JSGlobalObject*, double);
ALWAYS_INLINE JSValue makeFloat(JSGlobalObject* globalObject, double value)
{
    // A whole number is an object, which tells it from the int. So is a NaN: it equals nothing, not even itself, so containers
    // go by which object it is (`x in [x]`, but not `float("nan") in [x]`). As an object it also keeps its sign and payload.
    // (A NaN that comes from JS is a plain number, and they are all one.)
    if (isSmallIntValue(value) || value != value) [[unlikely]]
        return makeFloatSlow(globalObject, value);
    return JSValue(JSValue::EncodeAsDouble, value);
}

int64_t hashOfDouble(double);
String floatRepr(double);
// As format(value, f".{precision}{code}") with the flags of PyOS_double_to_string.
enum FloatFormatFlag : uint8_t {
    FloatAlwaysSign = 1 << 0,
    FloatAddDotZero = 1 << 1,
    FloatAlternate = 1 << 2,
    FloatNoNegativeZero = 1 << 3,
};
String formatDouble(double, char formatCode, int precision, unsigned flags);
// nullopt: not the text of a float. Leading and trailing whitespace and underscores are the caller's business.
std::optional<double> parseDouble(std::span<const Latin1Character>);

// The arithmetic of the built-in numbers, for the interpreter: an empty value without an exception if the operands are not
// both int, bool or float themselves.
JSValue tryNumberBinaryOperation(JSGlobalObject*, BinaryOperator, JSValue, JSValue);

} // namespace Python
} // namespace JSC
