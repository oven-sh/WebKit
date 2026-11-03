/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyNumbers.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyStr.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// complex. (Objects/complexobject.c in CPython is the reference.)

PY_DEFINE_CELL(PyComplex);

PyComplex::PyComplex(VM& vm, Structure* structure, PyType* type, double real, double imaginary)
    : Base(vm, structure, type)
    , m_real(real)
    , m_imaginary(imaginary)
{
}

PyComplex* PyComplex::create(VM& vm, Structure* structure, PyType* type, double real, double imaginary)
{
    auto* object = new (NotNull, allocateCell<PyComplex>(vm)) PyComplex(vm, structure, type, real, imaginary);
    object->finishCreation(vm);
    return object;
}

PyComplex* PyComplex::create(JSGlobalObject* globalObject, double real, double imaginary)
{
    Runtime& rt = runtime(globalObject);
    return create(rt.vm(), rt.structure<PyComplex>(), rt.complexType(), real, imaginary);
}

namespace {

struct Complex {
    double real { 0 };
    double imaginary { 0 };
};

enum class ComplexError : uint8_t { None, Domain, Range };

}

static constexpr double infinity = std::numeric_limits<double>::infinity();

static Complex product(Complex z, Complex w)
{
    double a = z.real, b = z.imaginary, c = w.real, d = w.imaginary;
    double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    Complex r { ac - bd, ad + bc };
    // An infinity that came out as nan+nanj is recovered as C11's Annex G says.
    if (std::isnan(r.real) && std::isnan(r.imaginary)) {
        bool recalculate = false;
        auto zeroIfNaN = [](double& value) {
            if (std::isnan(value))
                value = std::copysign(0.0, value);
        };
        if (std::isinf(a) || std::isinf(b)) {
            a = std::copysign(std::isinf(a) ? 1.0 : 0.0, a);
            b = std::copysign(std::isinf(b) ? 1.0 : 0.0, b);
            zeroIfNaN(c);
            zeroIfNaN(d);
            recalculate = true;
        }
        if (std::isinf(c) || std::isinf(d)) {
            c = std::copysign(std::isinf(c) ? 1.0 : 0.0, c);
            d = std::copysign(std::isinf(d) ? 1.0 : 0.0, d);
            zeroIfNaN(a);
            zeroIfNaN(b);
            recalculate = true;
        }
        if (!recalculate && (std::isinf(ac) || std::isinf(bd) || std::isinf(ad) || std::isinf(bc))) {
            zeroIfNaN(a);
            zeroIfNaN(b);
            zeroIfNaN(c);
            zeroIfNaN(d);
            recalculate = true;
        }
        if (recalculate) {
            r.real = infinity * (a * c - b * d);
            r.imaginary = infinity * (a * d + b * c);
        }
    }
    return r;
}

static Complex quotient(Complex a, Complex b, ComplexError& error)
{
    // Divide both by whichever part of b is the larger, so as not to overflow for nothing.
    Complex r;
    double absReal = std::abs(b.real);
    double absImaginary = std::abs(b.imaginary);
    if (absReal >= absImaginary) {
        if (!absReal)
            error = ComplexError::Domain;
        else {
            double ratio = b.imaginary / b.real;
            double denominator = b.real + b.imaginary * ratio;
            r.real = (a.real + a.imaginary * ratio) / denominator;
            r.imaginary = (a.imaginary - a.real * ratio) / denominator;
        }
    } else if (absImaginary >= absReal) {
        double ratio = b.real / b.imaginary;
        double denominator = b.real * ratio + b.imaginary;
        r.real = (a.real * ratio + a.imaginary) / denominator;
        r.imaginary = (a.imaginary * ratio - a.real) / denominator;
    } else
        r.real = r.imaginary = std::numeric_limits<double>::quiet_NaN();

    if (std::isnan(r.real) && std::isnan(r.imaginary)) {
        if ((std::isinf(a.real) || std::isinf(a.imaginary)) && std::isfinite(b.real) && std::isfinite(b.imaginary)) {
            double x = std::copysign(std::isinf(a.real) ? 1.0 : 0.0, a.real);
            double y = std::copysign(std::isinf(a.imaginary) ? 1.0 : 0.0, a.imaginary);
            r.real = infinity * (x * b.real + y * b.imaginary);
            r.imaginary = infinity * (y * b.real - x * b.imaginary);
        } else if ((std::isinf(absReal) || std::isinf(absImaginary)) && std::isfinite(a.real) && std::isfinite(a.imaginary)) {
            double x = std::copysign(std::isinf(b.real) ? 1.0 : 0.0, b.real);
            double y = std::copysign(std::isinf(b.imaginary) ? 1.0 : 0.0, b.imaginary);
            r.real = 0.0 * (a.real * x + a.imaginary * y);
            r.imaginary = 0.0 * (a.imaginary * x - a.real * y);
        }
    }
    return r;
}

// A real number divided by a complex one.
static Complex quotient(double a, Complex b, ComplexError& error)
{
    Complex r;
    double absReal = std::abs(b.real);
    double absImaginary = std::abs(b.imaginary);
    if (absReal >= absImaginary) {
        if (!absReal)
            error = ComplexError::Domain;
        else {
            double ratio = b.imaginary / b.real;
            double denominator = b.real + b.imaginary * ratio;
            r.real = a / denominator;
            r.imaginary = (-a * ratio) / denominator;
        }
    } else if (absImaginary >= absReal) {
        double ratio = b.real / b.imaginary;
        double denominator = b.real * ratio + b.imaginary;
        r.real = (a * ratio) / denominator;
        r.imaginary = (-a) / denominator;
    } else
        r.real = r.imaginary = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(r.real) && std::isnan(r.imaginary) && std::isfinite(a) && (std::isinf(absReal) || std::isinf(absImaginary))) {
        double x = std::copysign(std::isinf(b.real) ? 1.0 : 0.0, b.real);
        double y = std::copysign(std::isinf(b.imaginary) ? 1.0 : 0.0, b.imaginary);
        r.real = 0.0 * (a * x);
        r.imaginary = 0.0 * (-a * y);
    }
    return r;
}

static Complex powerOfUnsigned(Complex x, long n)
{
    Complex r { 1.0, 0.0 };
    Complex p = x;
    for (long mask = 1; mask > 0 && n >= mask; mask <<= 1) {
        if (n & mask)
            r = product(r, p);
        p = product(p, p);
    }
    return r;
}

JSValue complexPower(JSGlobalObject* globalObject, double baseReal, double baseImaginary, double exponentReal, double exponentImaginary)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Complex a { baseReal, baseImaginary };
    Complex b { exponentReal, exponentImaginary };
    Complex r;
    ComplexError error = ComplexError::None;

    if (!b.imaginary && b.real == std::floor(b.real) && std::abs(b.real) <= 100.0) {
        // A small integer: repeated multiplication is faster and more accurate.
        long n = static_cast<long>(b.real);
        r = n > 0 ? powerOfUnsigned(a, n) : quotient(Complex { 1.0, 0.0 }, powerOfUnsigned(a, -n), error);
        if (error == ComplexError::None && (std::isinf(r.real) || std::isinf(r.imaginary)))
            error = ComplexError::Range;
    } else if (!a.real && !a.imaginary) {
        if (b.imaginary || b.real < 0.0)
            error = ComplexError::Domain;
    } else {
        double magnitude = std::hypot(a.real, a.imaginary);
        double length = std::pow(magnitude, b.real);
        double angle = std::atan2(a.imaginary, a.real);
        double phase = angle * b.real;
        if (b.imaginary) {
            length *= std::exp(-angle * b.imaginary);
            phase += b.imaginary * std::log(magnitude);
        }
        r.real = length * std::cos(phase);
        r.imaginary = length * std::sin(phase);
        if (std::isinf(r.real) || std::isinf(r.imaginary))
            error = ComplexError::Range;
    }
    if (error == ComplexError::Domain)
        return raiseZeroDivisionError(globalObject, scope, "zero to a negative or complex power"_s);
    if (error == ComplexError::Range)
        return raiseOverflowError(globalObject, scope, "complex exponentiation"_s);
    return PyComplex::create(globalObject, r.real, r.imaginary);
}

// ---- Methods ----

static ALWAYS_INLINE Complex valueOf(JSValue value)
{
    auto* object = PyComplex::from(value);
    return { object->real(), object->imaginary() };
}

// A float or an int as a double. False: it is neither (`result` is NotImplemented), or an exception (it is empty).
static bool realOperand(JSGlobalObject* globalObject, JSValue value, double& number, JSValue& result)
{
    if (!isFloat(value) && !isInt(value)) {
        result = runtime(globalObject).notImplemented();
        return false;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    number = toDouble(globalObject, value);
    if (scope.exception()) {
        result = JSValue();
        return false;
    }
    return true;
}

// A real operand is not made complex first: float(a) + complex(b, c) is complex(a + b, c), whatever the sign of a zero c.
static JSValue complexArithmetic(JSGlobalObject* globalObject, BinaryOperator op, JSValue v, JSValue w)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Complex r;
    ComplexError error = ComplexError::None;
    JSValue failure;
    if (PyComplex::is(w)) {
        Complex b = valueOf(w);
        if (PyComplex::is(v)) {
            Complex a = valueOf(v);
            switch (op) {
            case BinaryOperator::Add:
                r = { a.real + b.real, a.imaginary + b.imaginary };
                break;
            case BinaryOperator::Sub:
                r = { a.real - b.real, a.imaginary - b.imaginary };
                break;
            case BinaryOperator::Mult:
                r = product(a, b);
                break;
            default:
                r = quotient(a, b, error);
                break;
            }
        } else {
            double a;
            if (!realOperand(globalObject, v, a, failure))
                RELEASE_AND_RETURN(scope, failure);
            switch (op) {
            case BinaryOperator::Add:
                r = { b.real + a, b.imaginary };
                break;
            case BinaryOperator::Sub:
                r = { a - b.real, -b.imaginary };
                break;
            case BinaryOperator::Mult:
                r = { b.real * a, b.imaginary * a };
                break;
            default:
                r = quotient(a, b, error);
                break;
            }
        }
    } else {
        Complex a = valueOf(v);
        double b;
        if (!realOperand(globalObject, w, b, failure))
            RELEASE_AND_RETURN(scope, failure);
        switch (op) {
        case BinaryOperator::Add:
            r = { a.real + b, a.imaginary };
            break;
        case BinaryOperator::Sub:
            r = { a.real - b, a.imaginary };
            break;
        case BinaryOperator::Mult:
            r = { a.real * b, a.imaginary * b };
            break;
        default:
            if (b)
                r = { a.real / b, a.imaginary / b };
            else
                error = ComplexError::Domain;
            break;
        }
    }
    if (error == ComplexError::Domain)
        return raiseZeroDivisionError(globalObject, scope, "division by zero"_s);
    return PyComplex::create(globalObject, r.real, r.imaginary);
}

template<BinaryOperator op>
static JSValue complexBinaryMethod(JSGlobalObject* globalObject, JSValue self, JSValue other) { return complexArithmetic(globalObject, op, self, other); }
template<BinaryOperator op>
static JSValue complexReflectedMethod(JSGlobalObject* globalObject, JSValue self, JSValue other) { return complexArithmetic(globalObject, op, other, self); }

static bool complexOperand(JSGlobalObject* globalObject, JSValue value, Complex& number, JSValue& result)
{
    if (PyComplex::is(value)) {
        number = valueOf(value);
        return true;
    }
    number.imaginary = 0.0;
    return realOperand(globalObject, value, number.real, result);
}

static JSValue complexPowOf(JSGlobalObject* globalObject, JSValue v, JSValue w, JSValue modulus)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Complex a, b;
    JSValue failure;
    if (!complexOperand(globalObject, v, a, failure) || !complexOperand(globalObject, w, b, failure))
        RELEASE_AND_RETURN(scope, failure);
    if (modulus && !isNone(modulus))
        return raiseValueError(globalObject, scope, "complex modulo"_s);
    RELEASE_AND_RETURN(scope, complexPower(globalObject, a.real, a.imaginary, b.real, b.imaginary));
}

static JSValue complexPowMethod(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue modulus) { return complexPowOf(globalObject, self, other, modulus); }
static JSValue complexRPowMethod(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue modulus) { return complexPowOf(globalObject, other, self, modulus); }

static JSValue complexNeg(JSGlobalObject* globalObject, JSValue self)
{
    Complex value = valueOf(self);
    return PyComplex::create(globalObject, -value.real, -value.imaginary);
}

static JSValue complexPos(JSGlobalObject* globalObject, JSValue self)
{
    if (PyComplex::from(self)->type() == runtime(globalObject).complexType())
        return self;
    Complex value = valueOf(self);
    return PyComplex::create(globalObject, value.real, value.imaginary);
}

static JSValue complexAbs(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Complex value = valueOf(self);
    if (std::isinf(value.real) || std::isinf(value.imaginary))
        return makeFloat(globalObject, infinity);
    if (std::isnan(value.real) || std::isnan(value.imaginary))
        return makeFloat(globalObject, std::numeric_limits<double>::quiet_NaN());
    double result = std::hypot(value.real, value.imaginary);
    if (std::isinf(result))
        return raiseOverflowError(globalObject, scope, "absolute value too large"_s);
    return makeFloat(globalObject, result);
}

static JSValue complexBool(JSGlobalObject*, JSValue self)
{
    Complex value = valueOf(self);
    return jsBoolean(value.real || value.imaginary);
}

template<bool wantEqual>
static JSValue complexEquality(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Complex value = valueOf(self);
    bool equal;
    if (isInt(other)) {
        if (value.imaginary)
            equal = false;
        else
            return richCompare(globalObject, makeFloat(globalObject, value.real), other, wantEqual ? CompareOperator::Eq : CompareOperator::NotEq);
    } else if (isFloat(other))
        equal = value.real == floatValue(other) && !value.imaginary;
    else if (PyComplex::is(other)) {
        Complex second = valueOf(other);
        equal = value.real == second.real && value.imaginary == second.imaginary;
    } else
        return runtime(globalObject).notImplemented();
    return jsBoolean(equal == wantEqual);
}

static JSValue complexHash(JSGlobalObject* globalObject, JSValue self)
{
    Complex value = valueOf(self);
    uint64_t combined = static_cast<uint64_t>(hashOfDouble(value.real)) + 1000003 * static_cast<uint64_t>(hashOfDouble(value.imaginary));
    int64_t hash = static_cast<int64_t>(combined);
    return makeInt(globalObject, hash == -1 ? static_cast<int64_t>(-2) : hash);
}

static JSValue complexRepr(JSGlobalObject* globalObject, JSValue self)
{
    Complex value = valueOf(self);
    // With a real part of +0, only the imaginary part is shown.
    if (!value.real && !std::signbit(value.real))
        return makeStr(globalObject, makeString(formatDouble(value.imaginary, 'r', 0, 0), 'j'));
    return makeStr(globalObject, makeString('(', formatDouble(value.real, 'r', 0, 0), formatDouble(value.imaginary, 'r', 0, FloatAlwaysSign), "j)"_s));
}

static JSValue complexConjugate(JSGlobalObject* globalObject, JSValue self)
{
    Complex value = valueOf(self);
    return PyComplex::create(globalObject, value.real, -value.imaginary);
}

static JSValue complexGetNewArgs(JSGlobalObject* globalObject, JSValue self)
{
    Complex value = valueOf(self);
    return PyTuple::create(globalObject, { makeFloat(globalObject, value.real), makeFloat(globalObject, value.imaginary) });
}

static JSValue complexFormatMethod(JSGlobalObject* globalObject, JSValue self, JSValue spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(spec))
        return raiseTypeError(globalObject, scope, "__format__() argument must be str, not "_s, typeNameOfArgument(globalObject, spec));
    RELEASE_AND_RETURN(scope, formatComplex(globalObject, self, strValue(spec)));
}

static JSValue complexGetReal(JSGlobalObject* globalObject, JSValue self) { return makeFloat(globalObject, valueOf(self).real); }
static JSValue complexGetImag(JSGlobalObject* globalObject, JSValue self) { return makeFloat(globalObject, valueOf(self).imaginary); }

// ---- complex() ----

static JSValue makeComplex(JSGlobalObject* globalObject, PyType* type, double real, double imaginary)
{
    Runtime& rt = runtime(globalObject);
    return PyComplex::create(rt.vm(), rt.structure<PyComplex>(), type, real, imaginary);
}

static bool parseComplex(std::span<const Latin1Character> text, Complex& result)
{
    size_t i = 0;
    size_t size = text.size();
    auto skipSpaces = [&] {
        while (i < size && isPythonWhitespace(text[i]))
            ++i;
    };
    auto at = [&](size_t index) -> Latin1Character { return index < size ? text[index] : Latin1Character(0); };
    auto isJ = [&](size_t index) { return at(index) == 'j' || at(index) == 'J'; };

    skipSpaces();
    bool hasBracket = false;
    if (at(i) == '(') {
        hasBracket = true;
        ++i;
        skipSpaces();
    }
    // <float>, <float>j, <float><signed-float>j, <float><sign>j, <sign>j or j.
    double x = 0.0, y = 0.0, z;
    if (size_t length = scanDouble(text.subspan(i), z)) {
        i += length;
        if (at(i) == '+' || at(i) == '-') {
            x = z;
            if (size_t second = scanDouble(text.subspan(i), y))
                i += second;
            else {
                y = at(i) == '+' ? 1.0 : -1.0;
                ++i;
            }
            if (!isJ(i))
                return false;
            ++i;
        } else if (isJ(i)) {
            ++i;
            y = z;
        } else
            x = z;
    } else {
        if (at(i) == '+' || at(i) == '-') {
            y = at(i) == '+' ? 1.0 : -1.0;
            ++i;
        } else
            y = 1.0;
        if (!isJ(i))
            return false;
        ++i;
    }
    skipSpaces();
    if (hasBracket) {
        if (at(i) != ')')
            return false;
        ++i;
        skipSpaces();
    }
    if (i != size)
        return false;
    result = { x, y };
    return true;
}

// value.__complex__(). Empty with no exception: it has none.
static JSValue tryComplexMethod(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, value);
    JSValue method = type->lookup(Special::Complex);
    if (!method)
        return { };
    JSValue result = callSpecial(globalObject, method, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (!PyComplex::is(result))
        return raiseTypeError(globalObject, scope, type->name(), ".__complex__() must return a complex, not "_s, typeName(globalObject, result));
    return result;
}

static bool isRealNumber(JSGlobalObject* globalObject, JSValue value)
{
    if (isFloat(value) || isInt(value))
        return true;
    PyType* type = typeOf(globalObject, value);
    return type->lookup(Special::Float) || type->lookup(Special::Index);
}

// Whether an underscore is anywhere but between two digits. (With an underscore in the text, a NUL counts as well.)
static bool hasMisplacedUnderscore(StringView text)
{
    if (text.find('_') == notFound)
        return false;
    bool previousIsDigit = false;
    bool previousIsUnderscore = false;
    for (char32_t character : text.codePoints()) {
        if (!character)
            return true;
        bool isDigit = decimalDigitValue(character) >= 0;
        if (character == '_' ? !previousIsDigit : previousIsUnderscore && !isDigit)
            return true;
        previousIsDigit = isDigit;
        previousIsUnderscore = character == '_';
    }
    return previousIsUnderscore;
}

static JSValue complexNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);

    if (args.size() <= 1 && !args.hasKeywords()) {
        if (!args.size())
            return makeComplex(globalObject, type, 0.0, 0.0);
        JSValue argument = args[0];
        if (type == rt.complexType() && PyComplex::is(argument) && PyComplex::from(argument)->type() == type)
            return argument;
        if (isStr(argument)) {
            String text = strValue(argument)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            Vector<Latin1Character, 64> normalized;
            Complex value;
            if (hasMisplacedUnderscore(text))
                return raiseValueError(globalObject, scope, "could not convert string to complex: "_s, reprForMessage(globalObject, argument));
            if (!normalizeNumberText(text, normalized) || !parseComplex(normalized.span(), value))
                return raiseValueError(globalObject, scope, "complex() arg is a malformed string"_s);
            return makeComplex(globalObject, type, value.real, value.imaginary);
        }
        JSValue converted = tryComplexMethod(globalObject, argument);
        RETURN_IF_EXCEPTION(scope, { });
        if (!converted && PyComplex::is(argument))
            converted = argument;
        if (converted) {
            Complex value = valueOf(converted);
            return makeComplex(globalObject, type, value.real, value.imaginary);
        }
        if (!isRealNumber(globalObject, argument))
            return raiseTypeError(globalObject, scope, "complex() argument must be a string or a number, not "_s, typeName(globalObject, argument));
        double real = toDouble(globalObject, argument);
        RETURN_IF_EXCEPTION(scope, { });
        return makeComplex(globalObject, type, real, 0.0);
    }

    if (args.size() > 2)
        return raiseTypeError(globalObject, scope, "complex() takes at most 2 arguments ("_s, args.totalCount(), " given)"_s);
    JSValue r = args.get(0);
    JSValue i = args.get(1);
    for (unsigned k = 0; k < args.keywordCount(); ++k) {
        String keyword = stringOf(args.keywordName(k));
        JSValue& target = keyword == "real"_s ? r : i;
        if (keyword != "real"_s && keyword != "imag"_s)
            return raiseTypeError(globalObject, scope, "complex() got an unexpected keyword argument '"_s, keyword, '\'');
        if (target)
            return raiseTypeError(globalObject, scope, "argument for complex() given by name ('"_s, keyword, "') and position ("_s, keyword == "real"_s ? 1 : 2, ')');
        target = args.keywordValue(k);
    }
    if (!r)
        r = jsNumber(0);

    JSValue converted = tryComplexMethod(globalObject, r);
    RETURN_IF_EXCEPTION(scope, { });
    if (converted)
        r = converted;
    if (!PyComplex::is(r) && !isRealNumber(globalObject, r))
        return raiseTypeError(globalObject, scope, "complex() argument 'real' must be a real number, not "_s, typeName(globalObject, r));
    if (i && !PyComplex::is(i) && !isRealNumber(globalObject, i))
        return raiseTypeError(globalObject, scope, "complex() argument 'imag' must be a real number, not "_s, typeName(globalObject, i));

    // real + imag*1j, where either may itself be complex.
    Complex cr, ci;
    bool realIsComplex = PyComplex::is(r);
    bool imaginaryIsComplex = false;
    if (realIsComplex)
        cr = valueOf(r);
    else {
        cr.real = toDouble(globalObject, r);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!i)
        ci.real = cr.imaginary;
    else if (PyComplex::is(i)) {
        ci = valueOf(i);
        imaginaryIsComplex = true;
    } else {
        ci.real = toDouble(globalObject, i);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (imaginaryIsComplex)
        cr.real -= ci.imaginary;
    if (realIsComplex && i)
        ci.real += cr.imaginary;
    return makeComplex(globalObject, type, cr.real, ci.real);
}

static JSValue complexFromNumber(JSGlobalObject* globalObject, JSValue cls, JSValue number)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isExact = cls.asCell() == rt.complexType();
    if (isExact && PyComplex::is(number) && PyComplex::from(number)->type() == rt.complexType())
        return number;
    Complex value;
    JSValue converted = PyComplex::is(number) ? number : tryComplexMethod(globalObject, number);
    RETURN_IF_EXCEPTION(scope, { });
    if (converted)
        value = valueOf(converted);
    else {
        value.real = toDouble(globalObject, number);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue result = PyComplex::create(globalObject, value.real, value.imaginary);
    if (isExact)
        return result;
    RELEASE_AND_RETURN(scope, call(globalObject, cls, result));
}

static constexpr NativeDef complexMethods[] = {
    PY_NEW(complexNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", complexRepr, "($self, /)"),
    PY_SLOT("__hash__", complexHash, "($self, /)"),
    PY_SLOT("__bool__", complexBool, "($self, /)"),
    PY_SLOT("__eq__", complexEquality<true>, "($self, value, /)"),
    PY_SLOT("__ne__", complexEquality<false>, "($self, value, /)"),
    PY_SLOT("__add__", complexBinaryMethod<BinaryOperator::Add>, "($self, value, /)"),
    PY_SLOT("__radd__", complexReflectedMethod<BinaryOperator::Add>, "($self, value, /)"),
    PY_SLOT("__sub__", complexBinaryMethod<BinaryOperator::Sub>, "($self, value, /)"),
    PY_SLOT("__rsub__", complexReflectedMethod<BinaryOperator::Sub>, "($self, value, /)"),
    PY_SLOT("__mul__", complexBinaryMethod<BinaryOperator::Mult>, "($self, value, /)"),
    PY_SLOT("__rmul__", complexReflectedMethod<BinaryOperator::Mult>, "($self, value, /)"),
    PY_SLOT("__truediv__", complexBinaryMethod<BinaryOperator::Div>, "($self, value, /)"),
    PY_SLOT("__rtruediv__", complexReflectedMethod<BinaryOperator::Div>, "($self, value, /)"),
    PY_SLOT("__pow__", complexPowMethod, "($self, value, mod=None, /)"),
    PY_SLOT("__rpow__", complexRPowMethod, "($self, value, mod=None, /)"),
    PY_SLOT("__neg__", complexNeg, "($self, /)"),
    PY_SLOT("__pos__", complexPos, "($self, /)"),
    PY_SLOT("__abs__", complexAbs, "($self, /)"),
    PY_CLASSMETHOD("from_number", complexFromNumber, "($type, number, /)"),
    PY_METHOD("conjugate", complexConjugate, "($self, /)"),
    PY_METHOD("__complex__", complexPos, "($self, /)"),
    PY_METHOD("__getnewargs__", complexGetNewArgs, "($self, /)"),
    PY_METHOD("__format__", complexFormatMethod, "($self, format_spec, /)"),
};

static constexpr NativeGetSet complexGetSets[] = {
    PY_MEMBER("real", complexGetReal),
    PY_MEMBER("imag", complexGetImag),
};

void initializeComplexType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.complexType()->initializeBuiltin(globalObject, { "complex"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, complexMethods, complexGetSets, "Create a complex number from a string or numbers."_s }, rt.typeType(), rt.objectType());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
