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
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include <wtf/SafeStrerror.h>

// The module cmath: Modules/cmathmodule.c of CPython. What takes complex numbers of C's and gives complex numbers of C's is PythonCMathKernels.h, which is made from it. This is what has to do with objects.

// As in PythonMathModule.cpp, and for the same reason: see "One rounding or two" in the README.
#pragma STDC FP_CONTRACT ON

#include "PythonCMathKernels.h"

namespace JSC { namespace Python {

using namespace CMathKernels;

namespace {

// math_error(): what errno says went wrong
JSValue raiseCMathError(JSGlobalObject* globalObject, ThrowScope& scope)
{
    if (errno == EDOM)
        return raiseValueError(globalObject, scope, "math domain error"_s);
    if (errno == ERANGE)
        return raise(globalObject, scope, BuiltinType::OverflowError, "math range error"_s);
    return raiseValueError(globalObject, scope, String::fromUTF8(safeStrerror(errno).span()));
}

// Nothing if it raised.
std::optional<Py_complex> complexArgument(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto parts = toComplexParts(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return Py_complex { parts->first, parts->second };
}

using Kernel = Py_complex (*)(Py_complex);
constexpr std::pair<ASCIILiteral, Kernel> kernels[] = {
    { "acos"_s, cmath_acos_impl }, { "acosh"_s, cmath_acosh_impl }, { "asin"_s, cmath_asin_impl }, { "asinh"_s, cmath_asinh_impl }, { "atan"_s, cmath_atan_impl }, { "atanh"_s, cmath_atanh_impl },
    { "cos"_s, cmath_cos_impl }, { "cosh"_s, cmath_cosh_impl }, { "exp"_s, cmath_exp_impl }, { "log10"_s, cmath_log10_impl }, { "sin"_s, cmath_sin_impl }, { "sinh"_s, cmath_sinh_impl },
    { "sqrt"_s, cmath_sqrt_impl }, { "tan"_s, cmath_tan_impl }, { "tanh"_s, cmath_tanh_impl },
};

enum class Predicate : uint8_t { IsFinite, IsNaN, IsInfinite };

} // anonymous namespace

// All that take a complex number and give one
PYTHON_NATIVE(cmathKernel)
{
    Kernel kernel = kernels[unpack<unsigned>(callFrame, 0)].second;
    NATIVE_PROLOGUE();
    auto z = complexArgument(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    Py_complex result = kernel(*z);
    if (errno == EDOM || errno == ERANGE)
        return JSValue::encode(raiseCMathError(globalObject, scope));
    return JSValue::encode(PyComplex::create(globalObject, result.real, result.imag));
}

// log(z[, base])
PYTHON_NATIVE(cmathLog)
{
    NATIVE_PROLOGUE();
    auto z = complexArgument(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    Py_complex result = c_log(*z);
    if (args.size() > 1) {
        // What is done to make a number of it can set errno, and what it was before that is not to be lost.
        int saved = errno;
        auto base = complexArgument(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        errno = saved;
        result = _Py_c_quot(result, c_log(*base));
    }
    if (errno)
        return JSValue::encode(raiseCMathError(globalObject, scope));
    return JSValue::encode(PyComplex::create(globalObject, result.real, result.imag));
}

PYTHON_NATIVE(cmathPhase)
{
    NATIVE_PROLOGUE();
    auto z = complexArgument(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    double phi = atan2(z->imag, z->real);
    if (errno)
        return JSValue::encode(raiseCMathError(globalObject, scope));
    return JSValue::encode(floatFromDouble(phi));
}

PYTHON_NATIVE(cmathPolar)
{
    NATIVE_PROLOGUE();
    auto z = complexArgument(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    double phi = atan2(z->imag, z->real);
    double r = _Py_c_abs(*z);
    if (errno)
        return JSValue::encode(raiseCMathError(globalObject, scope));
    return JSValue::encode(PyTuple::create(globalObject, { floatFromDouble(r), floatFromDouble(phi) }));
}

PYTHON_NATIVE(cmathRect)
{
    NATIVE_PROLOGUE();
    auto r = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto phi = toDouble(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    Py_complex result = cmath_rect_impl(*r, *phi);
    if (errno)
        return JSValue::encode(raiseCMathError(globalObject, scope));
    return JSValue::encode(PyComplex::create(globalObject, result.real, result.imag));
}

// isfinite(z), isnan(z) and isinf(z)
PYTHON_NATIVE(cmathPredicate)
{
    auto predicate = unpack<Predicate>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto z = complexArgument(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    switch (predicate) {
    case Predicate::IsFinite:
        return JSValue::encode(jsBoolean(isfinite(z->real) && isfinite(z->imag)));
    case Predicate::IsNaN:
        return JSValue::encode(jsBoolean(isnan(z->real) || isnan(z->imag)));
    case Predicate::IsInfinite:
        return JSValue::encode(jsBoolean(isinf(z->real) || isinf(z->imag)));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// isclose(a, b, *, rel_tol=1e-09, abs_tol=0.0)
PYTHON_NATIVE(cmathIsClose)
{
    NATIVE_PROLOGUE();
    auto a = complexArgument(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    auto b = complexArgument(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    double relativeTolerance = 1e-09;
    double absoluteTolerance = 0;
    if (JSValue given = args.at(2)) {
        auto value = toDouble(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        relativeTolerance = *value;
    }
    if (JSValue given = args.at(3)) {
        auto value = toDouble(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        absoluteTolerance = *value;
    }
    if (relativeTolerance < 0 || absoluteTolerance < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "tolerances must be non-negative"_s));
    // Two infinities of the same sign are close.
    if (a->real == b->real && a->imag == b->imag)
        return JSValue::encode(jsBoolean(true));
    // And an infinity is close to nothing else, which what follows would not find.
    if (isinf(a->real) || isinf(a->imag) || isinf(b->real) || isinf(b->imag))
        return JSValue::encode(jsBoolean(false));
    double difference = _Py_c_abs(_Py_c_diff(*a, *b));
    return JSValue::encode(jsBoolean(difference <= relativeTolerance * _Py_c_abs(*b) || difference <= relativeTolerance * _Py_c_abs(*a) || difference <= absoluteTolerance));
}

JSObject* createCMathModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "cmath"_s);
    for (unsigned i = 0; i < std::size(kernels); ++i)
        addFunction(globalObject, module, kernels[i].first, cmathKernel, pack(i));
    addFunction(globalObject, module, "isclose"_s, cmathIsClose);
    addFunction(globalObject, module, "isfinite"_s, cmathPredicate, pack(Predicate::IsFinite));
    addFunction(globalObject, module, "isinf"_s, cmathPredicate, pack(Predicate::IsInfinite));
    addFunction(globalObject, module, "isnan"_s, cmathPredicate, pack(Predicate::IsNaN));
    addFunction(globalObject, module, "log"_s, cmathLog);
    addFunction(globalObject, module, "phase"_s, cmathPhase);
    addFunction(globalObject, module, "polar"_s, cmathPolar);
    addFunction(globalObject, module, "rect"_s, cmathRect);
    auto add = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    constexpr double infinity = std::numeric_limits<double>::infinity();
    double nan = fabs(std::numeric_limits<double>::quiet_NaN());
    add("pi"_s, floatFromDouble(3.14159265358979323846));
    add("e"_s, floatFromDouble(2.7182818284590452354));
    add("tau"_s, floatFromDouble(6.2831853071795864769252867665590057683943));
    add("inf"_s, floatFromDouble(infinity));
    add("infj"_s, PyComplex::create(globalObject, 0, infinity));
    add("nan"_s, floatFromDouble(nan));
    add("nanj"_s, PyComplex::create(globalObject, 0, nan));
    return module;
}

} } // namespace JSC::Python

#pragma STDC FP_CONTRACT OFF
