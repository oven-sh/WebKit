/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTuple.h"
#include <cmath>

namespace JSC {
namespace Python {

// What _collections, _heapq, _bisect, array, _struct and binascii share.

// ---- Arguments, worded as the functions of CPython that Argument Clinic generates ----

// An argument declared Py_ssize_t: anything with __index__ that fits. False: an exception.
inline bool ssizeArgument(JSGlobalObject* globalObject, JSValue value, int64_t& result)
{
    if (value.isInt32()) [[likely]] {
        result = value.asInt32();
        return true;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(index);
    if (!converted) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        return false;
    }
    result = *converted;
    return true;
}

// An argument declared slice_index(accept={int}), such as the bounds of index(): what does not fit is clipped.
inline bool sliceIndexArgument(JSGlobalObject* globalObject, JSValue value, int64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasIndex(globalObject, value)) {
        raiseTypeError(globalObject, scope, "slice indices must be integers or have an __index__ method"_s);
        return false;
    }
    result = toSsize(globalObject, value, nullptr);
    return !scope.exception();
}

// builtins.name
inline JSValue builtinNamed(JSGlobalObject* globalObject, ASCIILiteral name)
{
    return currentBuiltin(globalObject, internString(getVM(globalObject), name));
}

// A bytes of that length, to be filled before anything else sees it. Null: a MemoryError.
inline PyBytes* newBytes(JSGlobalObject* globalObject, size_t length)
{
    Runtime& rt = runtime(globalObject);
    if (!length)
        return rt.emptyBytes();
    PyBytes* bytes = PyBytes::tryCreateUninitialized(rt.vm(), rt.structure<PyBytes>(), rt.bytesType(), length);
    if (!bytes) {
        auto scope = DECLARE_THROW_SCOPE(rt.vm());
        raiseMemoryError(globalObject, scope, String());
    }
    return bytes;
}

// ---- Floats as bytes ----

// A float with exactly these bits. What makeFloat() makes of a NaN has neither its sign nor its payload, which a value in a
// JSValue cannot have; one in a cell can.
inline JSValue makeFloatExactly(JSGlobalObject* globalObject, double value)
{
    if (value != value && std::bit_cast<uint64_t>(value) != std::bit_cast<uint64_t>(PNaN)) [[unlikely]]
        return makeFloatSlow(globalObject, value);
    return makeFloat(globalObject, value);
}

// IEEE 754 binary16, rounding to nearest, ties to even. nullopt: too large for it.
inline std::optional<uint16_t> packFloat16(double value)
{
    uint64_t bits = std::bit_cast<uint64_t>(value);
    uint16_t sign = static_cast<uint16_t>((bits >> 63) << 15);
    int exponent = static_cast<int>((bits >> 52) & 0x7ff);
    uint64_t fraction = bits & ((1ull << 52) - 1);
    if (exponent == 0x7ff) {
        if (!fraction)
            return sign | 0x7c00;
        // The top of the payload, or a quiet NaN if that leaves none.
        uint16_t payload = static_cast<uint16_t>(fraction >> 42);
        return sign | 0x7c00 | (payload ? payload : 0x200);
    }
    exponent -= 1023;
    if (exponent >= 16)
        return std::nullopt;
    if (exponent < -25)
        return sign;
    // 1.fraction * 2**exponent, of which 10 bits of fraction are kept, or fewer below 2**-14.
    uint64_t significand = fraction | (1ull << 52);
    unsigned shift = exponent >= -14 ? 42 : 28 - exponent;
    uint64_t kept = significand >> shift;
    uint64_t dropped = significand & ((1ull << shift) - 1);
    uint64_t half = 1ull << (shift - 1);
    if (dropped > half || (dropped == half && (kept & 1)))
        ++kept;
    // A carry out of the fraction goes into the exponent, as it should.
    uint64_t result = exponent >= -14 ? (static_cast<uint64_t>(exponent + 15) << 10) + (kept - 1024) : kept;
    if (result >= 0x7c00)
        return std::nullopt;
    return sign | static_cast<uint16_t>(result);
}

inline double unpackFloat16(uint16_t bits)
{
    uint64_t sign = static_cast<uint64_t>(bits >> 15) << 63;
    int exponent = (bits >> 10) & 0x1f;
    unsigned fraction = bits & 0x3ff;
    if (exponent == 0x1f)
        return std::bit_cast<double>(sign | 0x7ff0000000000000ull | (static_cast<uint64_t>(fraction) << 42));
    double magnitude = exponent ? std::ldexp(static_cast<double>(fraction + 1024), exponent - 25) : std::ldexp(static_cast<double>(fraction), -24);
    return sign ? -magnitude : magnitude;
}

// IEEE 754 binary32. A signaling NaN stays one. nullopt: too large for it.
inline std::optional<uint32_t> packFloat32(double value)
{
    if (value != value) {
        uint64_t bits = std::bit_cast<uint64_t>(value);
        uint32_t result = static_cast<uint32_t>((bits >> 63) << 31) | 0x7f800000u | static_cast<uint32_t>((bits >> 29) & 0x7fffff);
        return result & 0x7fffff ? result : result | 0x400000;
    }
    float narrowed = static_cast<float>(value);
    if (std::isinf(narrowed) && !std::isinf(value))
        return std::nullopt;
    return std::bit_cast<uint32_t>(narrowed);
}

inline double unpackFloat32(uint32_t bits)
{
    if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x7fffff))
        return std::bit_cast<double>((static_cast<uint64_t>(bits >> 31) << 63) | 0x7ff0000000000000ull | (static_cast<uint64_t>(bits & 0x7fffff) << 29));
    return std::bit_cast<float>(bits);
}

// An argument that must be a complex number, or a real one. False: an exception.
inline bool complexArgument(JSGlobalObject* globalObject, JSValue value, double& real, double& imaginary)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyComplex::is(value) && rt.typeOf(value)->lookup(Special::Complex)) {
        value = call(globalObject, rt.complexType(), value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (PyComplex::is(value)) {
        auto* complex = PyComplex::from(value);
        real = complex->real();
        imaginary = complex->imaginary();
        return true;
    }
    real = toDouble(globalObject, value);
    imaginary = 0;
    return !scope.exception();
}

} // namespace Python
} // namespace JSC
