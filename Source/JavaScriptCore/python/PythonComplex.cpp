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

#include <wtf/dtoa.h>
#include <wtf/text/StringBuilder.h>

// complex. The arithmetic is CPython's, Objects/complexobject.c, case for case.

namespace JSC {

const ClassInfo PyComplex::s_info = { "complex"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyComplex) };

template<typename Visitor>
void PyComplex::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
}

DEFINE_VISIT_CHILDREN(PyComplex);

Structure* PyComplex::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags | pythonCellFlags), info());
}

PyComplex* PyComplex::create(VM& vm, Structure* structure, double real, double imaginary)
{
    auto* complex = new (NotNull, allocateCell<PyComplex>(vm)) PyComplex(vm, structure, real, imaginary);
    complex->finishCreation(vm);
    return complex;
}

PyComplex* PyComplex::create(JSGlobalObject* globalObject, double real, double imaginary)
{
    return create(globalObject->vm(), globalObject->pyRealm()->structureFor(BuiltinType::Complex), real, imaginary);
}

namespace Python {

struct Complex {
    double real { 0 };
    double imag { 0 };
};

enum class Failure : uint8_t { None, Domain, Range };

static Complex product(Complex z, Complex w)
{
    double a = z.real;
    double b = z.imag;
    double c = w.real;
    double d = w.imag;
    double ac = a * c;
    double bd = b * d;
    double ad = a * d;
    double bc = b * c;
    Complex r { ac - bd, ad + bc };

    // Recover infinities that computed as nan+nanj. See C11, Annex G.5.1, _Cmultd().
    if (std::isnan(r.real) && std::isnan(r.imag)) {
        auto zeroIfNaN = [] (double& x) {
            if (std::isnan(x))
                x = std::copysign(0.0, x);
        };
        bool recalculate = false;
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
            constexpr double infinity = std::numeric_limits<double>::infinity();
            r.real = infinity * (a * c - b * d);
            r.imag = infinity * (a * d + b * c);
        }
    }
    return r;
}

// Robert L. Smith's algorithm: divide top and bottom by whichever part of b is the larger.
static Complex quotient(Complex a, Complex b, Failure& failure)
{
    constexpr double infinity = std::numeric_limits<double>::infinity();
    Complex r;
    double absReal = std::abs(b.real);
    double absImag = std::abs(b.imag);
    if (absReal >= absImag) {
        if (!absReal) {
            failure = Failure::Domain;
            return { };
        }
        double ratio = b.imag / b.real;
        double denominator = multiplyAdd(b.imag, ratio, b.real);
        r.real = multiplyAdd(a.imag, ratio, a.real) / denominator;
        r.imag = multiplyAdd(-a.real, ratio, a.imag) / denominator;
    } else if (absImag >= absReal) {
        double ratio = b.real / b.imag;
        double denominator = multiplyAdd(b.real, ratio, b.imag);
        r.real = multiplyAdd(a.real, ratio, a.imag) / denominator;
        r.imag = multiplyAdd(a.imag, ratio, -a.real) / denominator;
    } else
        r.real = r.imag = std::numeric_limits<double>::quiet_NaN();

    if (std::isnan(r.real) && std::isnan(r.imag)) {
        if ((std::isinf(a.real) || std::isinf(a.imag)) && std::isfinite(b.real) && std::isfinite(b.imag)) {
            double x = std::copysign(std::isinf(a.real) ? 1.0 : 0.0, a.real);
            double y = std::copysign(std::isinf(a.imag) ? 1.0 : 0.0, a.imag);
            r.real = infinity * (x * b.real + y * b.imag);
            r.imag = infinity * (y * b.real - x * b.imag);
        } else if ((std::isinf(absReal) || std::isinf(absImag)) && std::isfinite(a.real) && std::isfinite(a.imag)) {
            double x = std::copysign(std::isinf(b.real) ? 1.0 : 0.0, b.real);
            double y = std::copysign(std::isinf(b.imag) ? 1.0 : 0.0, b.imag);
            r.real = 0.0 * (a.real * x + a.imag * y);
            r.imag = 0.0 * (a.imag * x - a.real * y);
        }
    }
    return r;
}

// The same, of a real number by a complex one.
static Complex quotient(double a, Complex b, Failure& failure)
{
    Complex r;
    double absReal = std::abs(b.real);
    double absImag = std::abs(b.imag);
    if (absReal >= absImag) {
        if (!absReal) {
            failure = Failure::Domain;
            return { };
        }
        double ratio = b.imag / b.real;
        double denominator = multiplyAdd(b.imag, ratio, b.real);
        r.real = a / denominator;
        r.imag = (-a * ratio) / denominator;
    } else if (absImag >= absReal) {
        double ratio = b.real / b.imag;
        double denominator = multiplyAdd(b.real, ratio, b.imag);
        r.real = (a * ratio) / denominator;
        r.imag = -a / denominator;
    } else
        r.real = r.imag = std::numeric_limits<double>::quiet_NaN();

    if (std::isnan(r.real) && std::isnan(r.imag) && std::isfinite(a) && (std::isinf(absReal) || std::isinf(absImag))) {
        double x = std::copysign(std::isinf(b.real) ? 1.0 : 0.0, b.real);
        double y = std::copysign(std::isinf(b.imag) ? 1.0 : 0.0, b.imag);
        r.real = 0.0 * (a * x);
        r.imag = 0.0 * (-a * y);
    }
    return r;
}

static void noteOverflow(Complex r, Failure& failure)
{
    if (std::isinf(r.real) || std::isinf(r.imag))
        failure = Failure::Range;
}

static Complex powerOfComplex(Complex a, Complex b, Failure& failure)
{
    // A small whole exponent is done by multiplying, which is more accurate.
    if (!b.imag && b.real == std::floor(b.real) && std::abs(b.real) <= 100.0) {
        long n = static_cast<long>(b.real);
        unsigned long count = n < 0 ? -n : n;
        Complex r { 1, 0 };
        Complex p = a;
        for (unsigned long mask = 1; mask && count >= mask; mask <<= 1) {
            if (count & mask)
                r = product(r, p);
            p = product(p, p);
        }
        if (n <= 0)
            r = quotient(Complex { 1, 0 }, r, failure);
        if (failure == Failure::None)
            noteOverflow(r, failure);
        return r;
    }
    if (!a.real && !a.imag) {
        if (b.imag || b.real < 0)
            failure = Failure::Domain;
        return { };
    }
    double magnitude = std::hypot(a.real, a.imag);
    double length = std::pow(magnitude, b.real);
    double angle = std::atan2(a.imag, a.real);
    double phase = angle * b.real;
    if (b.imag) {
        length *= std::exp(-angle * b.imag);
        phase = multiplyAdd(b.imag, std::log(magnitude), phase);
    }
    Complex r { length * std::cos(phase), length * std::sin(phase) };
    noteOverflow(r, failure);
    return r;
}

static PyComplex* tryComplex(JSValue value)
{
    return dynamicDowncast<PyComplex>(value);
}

static Complex valueOf(PyComplex* complex) { return { complex->real(), complex->imaginary() }; }

static JSValue toJS(JSGlobalObject* globalObject, Complex value)
{
    return PyComplex::create(globalObject, value.real, value.imag);
}

JSValue powerOfNegativeFloat(JSGlobalObject* globalObject, double base, double exponent)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Failure failure = Failure::None;
    Complex result = powerOfComplex({ base, 0 }, { exponent, 0 }, failure);
    if (failure == Failure::Range)
        return raise(globalObject, scope, BuiltinType::OverflowError, "complex exponentiation"_s);
    return toJS(globalObject, result);
}

// ---- Operators

// One operand of an operator: a complex number, or a real one, or neither.
struct Operand {
    enum class Kind : uint8_t { None, Real, Complex };
    Kind kind { Kind::None };
    Complex value;
};

static Operand operandOf(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    if (auto* complex = tryComplex(value))
        return { Operand::Kind::Complex, valueOf(complex) };
    if (Number number = classify(value)) {
        double real = toDouble(globalObject, scope, number);
        RETURN_IF_EXCEPTION(scope, { });
        return { Operand::Kind::Real, { real, 0 } };
    }
    return { };
}

PYTHON_NATIVE(complexBinary)
{
    auto op = unpack<BinaryOperator>(callFrame, 0);
    bool reflected = unpack<bool>(callFrame, 1);
    NATIVE_PROLOGUE();
    if (args.size() < 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "expected 1 argument, got 0"_s));
    Operand left = operandOf(globalObject, scope, args[reflected ? 1 : 0]);
    RETURN_IF_EXCEPTION(scope, { });
    Operand right = operandOf(globalObject, scope, args[reflected ? 0 : 1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (left.kind == Operand::Kind::None || right.kind == Operand::Kind::None)
        RETURN_NOT_IMPLEMENTED();

    // A real number is not made complex first: it has no imaginary part, not even a zero with a sign.
    bool leftIsReal = left.kind == Operand::Kind::Real;
    bool rightIsReal = right.kind == Operand::Kind::Real;
    Complex a = left.value;
    Complex b = right.value;
    Complex r;
    Failure failure = Failure::None;
    switch (op) {
    case BinaryOperator::Add:
        r = leftIsReal ? Complex { a.real + b.real, b.imag } : rightIsReal ? Complex { a.real + b.real, a.imag } : Complex { a.real + b.real, a.imag + b.imag };
        break;
    case BinaryOperator::Sub:
        r = leftIsReal ? Complex { a.real - b.real, -b.imag } : rightIsReal ? Complex { a.real - b.real, a.imag } : Complex { a.real - b.real, a.imag - b.imag };
        break;
    case BinaryOperator::Mult:
        r = leftIsReal ? Complex { b.real * a.real, b.imag * a.real } : rightIsReal ? Complex { a.real * b.real, a.imag * b.real } : product(a, b);
        break;
    case BinaryOperator::Div:
        if (leftIsReal)
            r = quotient(a.real, b, failure);
        else if (rightIsReal) {
            if (b.real)
                r = { a.real / b.real, a.imag / b.real };
            else
                failure = Failure::Domain;
        } else
            r = quotient(a, b, failure);
        if (failure == Failure::Domain)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::ZeroDivisionError, "division by zero"_s));
        break;
    case BinaryOperator::Pow:
        if (args.size() > 2 && !isNone(args[2]))
            return JSValue::encode(raiseValueError(globalObject, scope, "complex modulo"_s));
        r = powerOfComplex(a, b, failure);
        if (failure == Failure::Domain)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::ZeroDivisionError, "zero to a negative or complex power"_s));
        if (failure == Failure::Range)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "complex exponentiation"_s));
        break;
    default:
        RETURN_NOT_IMPLEMENTED();
    }
    return JSValue::encode(toJS(globalObject, r));
}

PYTHON_NATIVE(complexEquality)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    bool wantsEqual = op == ComparisonOperator::Eq;
    NATIVE_PROLOGUE();
    if (!Python::isEquality(op))
        RETURN_NOT_IMPLEMENTED();
    Complex self = valueOf(tryComplex(args.at(0)));
    JSValue other = args.at(1);
    bool isEqual;
    if (auto* complex = tryComplex(other))
        isEqual = self.real == complex->real() && self.imag == complex->imaginary();
    else if (Number number = classify(other)) {
        // Exactly, and not by making a float of an int that may be too long for one.
        bool isUnordered;
        Number real;
        real.kind = Number::Kind::Float;
        real.real = self.real;
        isEqual = !self.imag && !*numberCompare(real, number, isUnordered) && !isUnordered;
    } else
        RETURN_NOT_IMPLEMENTED();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsBoolean(isEqual == wantsEqual));
}

PYTHON_NATIVE(complexNegative)
{
    Complex self = valueOf(tryComplex(callFrame->argument(0)));
    return JSValue::encode(toJS(globalObject, { -self.real, -self.imag }));
}

// +z, and z.__complex__()
PYTHON_NATIVE(complexPositive)
{
    JSValue self = callFrame->argument(0);
    if (typeOf(globalObject, self) == globalObject->pyRealm()->typeComplex())
        return JSValue::encode(self);
    return JSValue::encode(toJS(globalObject, valueOf(tryComplex(self))));
}

PYTHON_NATIVE(complexConjugate)
{
    Complex self = valueOf(tryComplex(callFrame->argument(0)));
    return JSValue::encode(toJS(globalObject, { self.real, -self.imag }));
}

PYTHON_NATIVE(complexAbs)
{
    NATIVE_PROLOGUE();
    Complex self = valueOf(tryComplex(args.at(0)));
    if (std::isinf(self.real) || std::isinf(self.imag))
        return JSValue::encode(floatFromDouble(std::numeric_limits<double>::infinity()));
    if (std::isnan(self.real) || std::isnan(self.imag))
        return JSValue::encode(floatFromDouble(std::numeric_limits<double>::quiet_NaN()));
    double result = std::hypot(self.real, self.imag);
    if (!std::isfinite(result))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "absolute value too large"_s));
    return JSValue::encode(floatFromDouble(result));
}

PYTHON_NATIVE(complexBool)
{
    UNUSED_PARAM(globalObject);
    Complex self = valueOf(tryComplex(callFrame->argument(0)));
    return JSValue::encode(jsBoolean(self.real || self.imag));
}

PYTHON_NATIVE(complexHash)
{
    Complex self = valueOf(tryComplex(callFrame->argument(0)));
    uint64_t combined = static_cast<uint64_t>(hashOfDouble(self.real)) + 1000003ULL * static_cast<uint64_t>(hashOfDouble(self.imag));
    if (combined == static_cast<uint64_t>(-1))
        combined = static_cast<uint64_t>(-2);
    return JSValue::encode(intFromInt64(globalObject, static_cast<int64_t>(combined)));
}

// ---- To text

// As repr() has a float, but a whole number has nothing after it.
static String reprOfPart(double value, bool alwaysSigned)
{
    String text = reprOfDouble(value);
    if (text.endsWith(".0"_s))
        text = text.left(text.length() - 2);
    if (alwaysSigned && !text.startsWith('-'))
        return concatenate('+', text);
    return text;
}

static String reprOfComplex(Complex value)
{
    if (!value.real && !std::signbit(value.real))
        return concatenate(reprOfPart(value.imag, false), 'j');
    return concatenate('(', reprOfPart(value.real, false), reprOfPart(value.imag, true), "j)"_s);
}

String reprOfComplex(double real, double imaginary)
{
    return reprOfComplex(Complex { real, imaginary });
}

PYTHON_NATIVE(complexRepr)
{
    return JSValue::encode(jsString(globalObject->vm(), reprOfComplex(valueOf(tryComplex(callFrame->argument(0))))));
}

PYTHON_NATIVE(complexFormat)
{
    NATIVE_PROLOGUE();
    JSString* given = stringIn(args[1]);
    if (!given)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("__format__() argument must be str, not "_s, typeNameOfArgument(globalObject, args[1]))));
    Complex self = valueOf(tryComplex(args[0]));
    String text = given->value(globalObject);
    if (text.isEmpty())
        return JSValue::encode(jsString(vm, reprOfComplex(self)));
    auto parsed = parseFormatSpecification(globalObject, text, typeName(globalObject, args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    FormatSpecification specification = *parsed;
    switch (specification.type) {
    case 0:
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'n':
        break;
    default:
        raiseUnknownFormatCode(globalObject, scope, specification.type, specification.typeName);
        return { };
    }
    if (specification.fill == '0')
        return JSValue::encode(raiseValueError(globalObject, scope, "Zero padding is not allowed in complex format specifier"_s));
    if (specification.align == '=')
        return JSValue::encode(raiseValueError(globalObject, scope, "'=' alignment flag is not allowed in complex format specifier"_s));

    bool skipReal = false;
    bool addParentheses = false;
    FormatSpecification part = specification;
    part.hasWidth = false;
    part.width = 0;
    part.align = '<';
    part.fill = ' ';
    bool asRepr = false;
    if (!part.type) {
        skipReal = !self.real && !std::signbit(self.real);
        addParentheses = !skipReal;
        if (part.precision < 0)
            asRepr = true;
        else
            part.type = 'g';
    }
    auto formatPart = [&] (double value, char sign) -> String {
        if (asRepr) {
            String digits = reprOfPart(value, sign == '+');
            if (sign == ' ' && !digits.startsWith('-'))
                return concatenate(' ', digits);
            return digits;
        }
        FormatSpecification one = part;
        one.sign = sign;
        return formatFloat(globalObject, value, one);
    };
    TextBuilder body;
    if (addParentheses)
        body.append('(');
    if (!skipReal) {
        String real = formatPart(self.real, specification.sign);
        RETURN_IF_EXCEPTION(scope, { });
        body.append(real);
    }
    String imaginary = formatPart(self.imag, skipReal ? specification.sign : '+');
    RETURN_IF_EXCEPTION(scope, { });
    body.append(imaginary, 'j');
    if (addParentheses)
        body.append(')');

    FormatSpecification padding;
    padding.fill = specification.fill;
    padding.align = specification.align ? specification.align : '>';
    padding.hasWidth = specification.hasWidth;
    padding.width = specification.width;
    String unpadded = body.finish(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String result = formatString(globalObject, unpadded, padding);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, result));
}

// ---- From text and from numbers

// The float at the front of the text, and how much of the text it takes up: nothing, if there is none.
static double parseFloatPrefix(std::span<const Latin1Character> text, size_t& consumed)
{
    consumed = 0;
    size_t i = 0;
    double sign = 1;
    if (i < text.size() && (text[i] == '+' || text[i] == '-'))
        sign = text[i++] == '-' ? -1 : 1;
    auto startsWith = [&] (ASCIILiteral word) {
        if (text.size() - i < word.length())
            return false;
        for (size_t k = 0; k < word.length(); ++k) {
            if (toASCIILower(text[i + k]) != word[k])
                return false;
        }
        return true;
    };
    if (startsWith("infinity"_s)) {
        consumed = i + 8;
        return sign * std::numeric_limits<double>::infinity();
    }
    if (startsWith("inf"_s)) {
        consumed = i + 3;
        return sign * std::numeric_limits<double>::infinity();
    }
    if (startsWith("nan"_s)) {
        consumed = i + 3;
        return std::copysign(std::numeric_limits<double>::quiet_NaN(), sign);
    }
    size_t start = i;
    size_t digits = 0;
    while (i < text.size() && isASCIIDigit(text[i])) {
        ++i;
        ++digits;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && isASCIIDigit(text[i])) {
            ++i;
            ++digits;
        }
    }
    if (!digits)
        return 0;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        size_t k = i + 1;
        if (k < text.size() && (text[k] == '+' || text[k] == '-'))
            ++k;
        if (k < text.size() && isASCIIDigit(text[k])) {
            while (k < text.size() && isASCIIDigit(text[k]))
                ++k;
            i = k;
        }
    }
    size_t parsed;
    double value = parseDouble(text.subspan(start, i - start), parsed);
    consumed = i;
    return sign * value;
}

static std::optional<Complex> parseComplex(StringView view)
{
    Vector<Latin1Character, 32> characters;
    if (!characters.tryReserveCapacity(view.length()))
        return std::nullopt;
    for (unsigned i = 0; i < view.length(); ++i) {
        char16_t c = view[i];
        if (c == '_') {
            if (!i || i + 1 == view.length() || !isASCIIDigit(view[i - 1]) || !isASCIIDigit(view[i + 1]))
                return std::nullopt;
            continue;
        }
        if (c >= 0x80)
            return std::nullopt;
        characters.append(static_cast<Latin1Character>(c));
    }
    auto text = characters.span();
    size_t i = 0;
    auto skipSpaces = [&] {
        while (i < text.size() && isUnicodeCompatibleASCIIWhitespace(text[i]))
            ++i;
    };
    auto at = [&] (size_t index) -> Latin1Character { return index < text.size() ? text[index] : 0; };
    auto isJ = [&] (size_t index) { return at(index) == 'j' || at(index) == 'J'; };

    skipSpaces();
    bool hasBracket = at(i) == '(';
    if (hasBracket) {
        ++i;
        skipSpaces();
    }
    double x = 0;
    double y = 0;
    size_t consumed;
    double z = parseFloatPrefix(text.subspan(i), consumed);
    if (consumed) {
        i += consumed;
        if (at(i) == '+' || at(i) == '-') {
            // <float><signed-float>j, or <float><sign>j
            x = z;
            y = parseFloatPrefix(text.subspan(i), consumed);
            if (consumed)
                i += consumed;
            else
                y = text[i++] == '+' ? 1.0 : -1.0;
            if (!isJ(i))
                return std::nullopt;
            ++i;
        } else if (isJ(i)) {
            ++i;
            y = z;
        } else
            x = z;
    } else {
        // <sign>j, or j
        if (at(i) == '+' || at(i) == '-')
            y = text[i++] == '+' ? 1.0 : -1.0;
        else
            y = 1.0;
        if (!isJ(i))
            return std::nullopt;
        ++i;
    }
    skipSpaces();
    if (hasBracket) {
        if (at(i) != ')')
            return std::nullopt;
        ++i;
        skipSpaces();
    }
    if (i != text.size())
        return std::nullopt;
    return Complex { x, y };
}

// What __complex__() gives. Null if there is no such method, or it raised.
static PyComplex* callComplexMethod(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isObject())
        return nullptr;
    JSValue self;
    JSValue method = lookupSpecial(globalObject, value, vm.pythonNames().dunder_complex, self);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!method)
        return nullptr;
    JSValue result = callMethod(globalObject, method, self);
    RETURN_IF_EXCEPTION(scope, nullptr);
    auto* complex = tryComplex(result);
    if (!complex) {
        raiseTypeError(globalObject, scope, concatenate("__complex__ returned non-complex (type "_s, typeName(globalObject, result), ')'));
        return nullptr;
    }
    if (!warnIfOfStrictSubclass(globalObject, result, BuiltinType::Complex, "__complex__ returned non-complex"_s, "complex"_s))
        return nullptr;
    return complex;
}

static bool isRealNumber(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    if (classify(value))
        return true;
    PyType* type = typeOf(globalObject, value);
    return type->lookup(vm, vm.pythonNames().dunder_float) || type->lookup(vm, vm.pythonNames().dunder_index);
}

// complex(), complex(string), complex(number), complex(real=0, imag=0)
PYTHON_NATIVE(complexNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args.at(0));
    auto make = [&] (Complex value) {
        return JSValue::encode(PyComplex::create(vm, type->instanceStructure(), value.real, value.imag));
    };
    if (args.size() > 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("complex() takes at most 2 arguments ("_s, args.size() - 1, " given)"_s)));

    if (args.size() <= 2 && !args.keywordCount()) {
        if (args.size() == 1)
            return make({ });
        JSValue argument = args[1];
        if (type == realm->typeComplex() && typeOf(globalObject, argument) == type)
            return JSValue::encode(argument);
        JSValue plain = argument;
        if (auto* boxed = tryBoxedValue(plain))
            plain = boxed->value();
        if (plain.isString()) {
            auto view = asString(plain)->view(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            auto parsed = parseComplex(view);
            if (!parsed)
                return JSValue::encode(raiseValueError(globalObject, scope, "complex() arg is a malformed string"_s));
            return make(*parsed);
        }
        PyComplex* converted = callComplexMethod(globalObject, argument);
        RETURN_IF_EXCEPTION(scope, { });
        if (converted)
            return make(valueOf(converted));
        if (!isRealNumber(globalObject, argument))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("complex() argument must be a string or a number, not "_s, typeName(globalObject, argument))));
        auto real = toDouble(globalObject, argument);
        RETURN_IF_EXCEPTION(scope, { });
        return make({ *real, 0 });
    }

    JSValue realValue = args.at(1);
    JSValue imaginaryValue = args.at(2);
    if (!realValue)
        realValue = jsNumber(0);

    // Either part may itself be complex. That is deprecated, and adds up as real + imag * 1j does.
    JSValue givenRealValue = realValue;
    if (PyComplex* converted = callComplexMethod(globalObject, realValue))
        realValue = converted;
    RETURN_IF_EXCEPTION(scope, { });
    if (tryComplex(realValue) && !isRealNumber(globalObject, givenRealValue)) {
        if (!warn(globalObject, BuiltinType::DeprecationWarning, concatenate("complex() argument 'real' must be a real number, not "_s, typeName(globalObject, givenRealValue))))
            return { };
    }
    if (!tryComplex(realValue) && !isRealNumber(globalObject, realValue))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("complex() argument 'real' must be a real number, not "_s, typeName(globalObject, realValue))));
    if (imaginaryValue && !tryComplex(imaginaryValue) && !isRealNumber(globalObject, imaginaryValue))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("complex() argument 'imag' must be a real number, not "_s, typeName(globalObject, imaginaryValue))));

    Complex real;
    Complex imaginary;
    bool realIsComplex = false;
    bool imaginaryIsComplex = false;
    if (auto* complex = tryComplex(realValue)) {
        real = valueOf(complex);
        realIsComplex = true;
    } else {
        auto converted = toDouble(globalObject, realValue);
        RETURN_IF_EXCEPTION(scope, { });
        real.real = *converted;
    }
    if (!imaginaryValue)
        imaginary.real = real.imag;
    else if (auto* complex = tryComplex(imaginaryValue)) {
        if (!warn(globalObject, BuiltinType::DeprecationWarning, concatenate("complex() argument 'imag' must be a real number, not "_s, typeName(globalObject, imaginaryValue))))
            return { };
        imaginary = valueOf(complex);
        imaginaryIsComplex = true;
    } else {
        auto converted = toDouble(globalObject, imaginaryValue);
        RETURN_IF_EXCEPTION(scope, { });
        imaginary.real = *converted;
    }
    if (imaginaryIsComplex)
        real.real -= imaginary.imag;
    if (realIsComplex && imaginaryValue)
        imaginary.real += real.imag;
    return make({ real.real, imaginary.real });
}

// complex.from_number(number)
PYTHON_NATIVE(complexFromNumber)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    JSValue number = args[1];
    if (type == realm->typeComplex() && typeOf(globalObject, number) == type)
        return JSValue::encode(number);
    Complex value;
    if (auto* complex = tryComplex(number))
        value = valueOf(complex);
    else {
        PyComplex* converted = callComplexMethod(globalObject, number);
        RETURN_IF_EXCEPTION(scope, { });
        if (converted)
            value = valueOf(converted);
        else {
            auto real = toDouble(globalObject, number);
            RETURN_IF_EXCEPTION(scope, { });
            value.real = *real;
        }
    }
    JSValue result = PyComplex::create(globalObject, value.real, value.imag);
    if (type == realm->typeComplex())
        return JSValue::encode(result);
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, type->object(), result)));
}

void initializeComplexType(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    PyType* type = realm->typeComplex();
    using Kind = PyNativeFunction::Kind;
    type->setInstanceStructure(vm, PyComplex::createStructure(vm, globalObject, type));
    for (BinaryOperator op : { BinaryOperator::Add, BinaryOperator::Sub, BinaryOperator::Mult, BinaryOperator::Div, BinaryOperator::Pow }) {
        type->putDirect(vm, names.method(op), PyNativeFunction::create(vm, globalObject, 1, names.method(op).string(), complexBinary, Kind::Method, type, pack(op, false)));
        type->putDirect(vm, names.reflectedMethod(op), PyNativeFunction::create(vm, globalObject, 1, names.reflectedMethod(op).string(), complexBinary, Kind::Method, type, pack(op, true)));
    }
    addMethods(globalObject, type, {
        { "__new__"_s, complexNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__neg__"_s, complexNegative },
        { "__pos__"_s, complexPositive },
        { "__complex__"_s, complexPositive },
        { "__abs__"_s, complexAbs },
        { "__bool__"_s, complexBool },
        { "__hash__"_s, complexHash },
        { "__repr__"_s, complexRepr },
        { "__format__"_s, complexFormat },
        { "conjugate"_s, complexConjugate },
        { "from_number"_s, complexFromNumber, Kind::ClassMethod },
    });
    addComparisons(globalObject, type, complexEquality);
    addMember(globalObject, type, "real"_s, [] (JSGlobalObject*, JSValue self) { return floatFromDouble(tryComplex(self)->real()); });
    addMember(globalObject, type, "imag"_s, [] (JSGlobalObject*, JSValue self) { return floatFromDouble(tryComplex(self)->imaginary()); });
}

} } // namespace JSC::Python
