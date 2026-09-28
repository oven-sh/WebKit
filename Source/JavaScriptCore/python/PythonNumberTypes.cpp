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

#include "JSBigIntInlines.h"
#include "ParseInt.h"
#include <wtf/dtoa.h>

// int, float and bool.

namespace JSC { namespace Python {

// ---- Arithmetic

enum class NumberType : uint8_t { Int, Float };

// int.__add__(1, 2.0) is NotImplemented: it is float.__radd__ that knows.
static bool accepts(NumberType type, const Number& self, const Number& other)
{
    if (type == NumberType::Int)
        return self.isInt() && other.isInt();
    return self.kind == Number::Kind::Float && !!other;
}

PYTHON_NATIVE(numberBinary)
{
    auto type = unpack<NumberType>(callFrame, 0);
    auto op = unpack<BinaryOperator>(callFrame, 1);
    auto reflected = unpack<bool>(callFrame, 2);
    NATIVE_PROLOGUE();
    if (args.size() < 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "expected 1 argument, got 0"_s));
    if (!accepts(type, classify(args[0]), classify(args[1])))
        RETURN_NOT_IMPLEMENTED();
    if (op == BinaryOperator::Pow && args.size() > 2 && !isNone(args[2]))
        RELEASE_AND_RETURN(scope, JSValue::encode(power(globalObject, args[0], args[1], args[2])));
    JSValue result = reflected ? numberBinaryOperation(globalObject, op, args[1], args[0]) : numberBinaryOperation(globalObject, op, args[0], args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!result)
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(result);
}

static void addOperator(JSGlobalObject* globalObject, PyType* target, NumberType type, BinaryOperator op)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    target->putDirect(vm, names.method(op), PyNativeFunction::create(vm, globalObject, 1, names.method(op).string(), numberBinary, PyNativeFunction::Kind::Method, target, pack(type, op, false)));
    target->putDirect(vm, names.reflectedMethod(op), PyNativeFunction::create(vm, globalObject, 1, names.reflectedMethod(op).string(), numberBinary, PyNativeFunction::Kind::Method, target, pack(type, op, true)));
}

PYTHON_NATIVE(numberUnary)
{
    auto op = unpack<UnaryOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(numberUnaryOperation(globalObject, op, args.at(0))));
}

PYTHON_NATIVE(numberDivmod)
{
    auto reflected = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!classify(args.at(0)) || !classify(args.at(1)))
        RETURN_NOT_IMPLEMENTED();
    RELEASE_AND_RETURN(scope, JSValue::encode(reflected ? divmod(globalObject, args[1], args[0]) : divmod(globalObject, args[0], args[1])));
}

static JSValue unbox(JSValue value)
{
    if (auto* boxed = tryBoxedValue(value))
        return boxed->value();
    return value;
}

PYTHON_NATIVE(numberBool)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(isTrue(globalObject, unbox(args.at(0))))));
}

PYTHON_NATIVE(numberAbs)
{
    NATIVE_PROLOGUE();
    Number number = classify(args.at(0));
    if (number.kind == Number::Kind::Float)
        return JSValue::encode(floatFromDouble(std::abs(number.real)));
    bool isNegative = number.kind == Number::Kind::Small ? number.small < 0 : number.big->sign();
    RELEASE_AND_RETURN(scope, JSValue::encode(numberUnaryOperation(globalObject, isNegative ? UnaryOperator::USub : UnaryOperator::UAdd, args[0])));
}

// The int that a number is, or is cut down to.
static JSValue toInt(JSGlobalObject* globalObject, ThrowScope& scope, const Number& number)
{
    switch (number.kind) {
    case Number::Kind::Small:
        return jsNumber(number.small);
    case Number::Kind::Big:
        return number.big;
    case Number::Kind::Float:
        if (std::isnan(number.real))
            return raiseValueError(globalObject, scope, "cannot convert float NaN to integer"_s);
        if (std::isinf(number.real))
            return raise(globalObject, scope, BuiltinType::OverflowError, "cannot convert float infinity to integer"_s);
        return intFromDouble(globalObject, number.real);
    case Number::Kind::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PYTHON_NATIVE(numberInt)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(toInt(globalObject, scope, classify(args.at(0)))));
}

PYTHON_NATIVE(numberFloat)
{
    NATIVE_PROLOGUE();
    double value = toDouble(globalObject, scope, classify(args.at(0)));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(floatFromDouble(value));
}

PYTHON_NATIVE(numberFloorOrCeil)
{
    bool isFloor = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    Number number = classify(args.at(0));
    if (number.kind == Number::Kind::Float && std::isfinite(number.real))
        number.real = isFloor ? std::floor(number.real) : std::ceil(number.real);
    RELEASE_AND_RETURN(scope, JSValue::encode(toInt(globalObject, scope, number)));
}

// round(number, ndigits=None)
PYTHON_NATIVE(numberRound)
{
    NATIVE_PROLOGUE();
    Number number = classify(args.at(0));
    JSValue digitsValue = args.at(1);
    bool hasDigits = digitsValue && !isNone(digitsValue);
    int64_t digits = 0;
    if (hasDigits) {
        auto index = toIndex(globalObject, digitsValue, true);
        RETURN_IF_EXCEPTION(scope, { });
        digits = *index;
    }

    if (number.kind == Number::Kind::Float) {
        if (hasDigits)
            return JSValue::encode(floatFromDouble(roundToDigits(number.real, std::clamp<int64_t>(digits, -400, 400))));
        // To the nearest int, and to the even one of two that are as near.
        if (std::isfinite(number.real)) {
            double rounded = std::round(number.real);
            if (std::abs(number.real - rounded) == 0.5)
                rounded = 2.0 * std::round(number.real / 2.0);
            number.real = rounded;
        }
        RELEASE_AND_RETURN(scope, JSValue::encode(toInt(globalObject, scope, number)));
    }

    JSValue self = toInt(globalObject, scope, number);
    if (digits >= 0)
        return JSValue::encode(self);
    // To tens, hundreds and so on: self - self % 10**n, up or down to the nearest, and to the even one of two.
    JSValue unit = power(globalObject, jsNumber(10), intFromInt64(globalObject, -digits), jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue remainder = binaryOperation(globalObject, BinaryOperator::Mod, false, self, unit);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue lower = binaryOperation(globalObject, BinaryOperator::Sub, false, self, remainder);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue twice = binaryOperation(globalObject, BinaryOperator::Mult, false, remainder, jsNumber(2));
    RETURN_IF_EXCEPTION(scope, { });
    bool isUnordered;
    int order = *numberCompare(classify(twice), classify(unit), isUnordered);
    bool roundUp = order > 0;
    if (!order) {
        JSValue quotient = binaryOperation(globalObject, BinaryOperator::FloorDiv, false, lower, unit);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue parity = binaryOperation(globalObject, BinaryOperator::BitAnd, false, quotient, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, { });
        roundUp = parity.asInt32();
    }
    if (!roundUp)
        return JSValue::encode(lower);
    RELEASE_AND_RETURN(scope, JSValue::encode(binaryOperation(globalObject, BinaryOperator::Add, false, lower, unit)));
}

PYTHON_NATIVE(numberFormat)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__format__"_s, 2, 2))
        return { };
    if (!args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("__format__() argument must be str, not "_s, typeName(globalObject, args[1]))));
    RELEASE_AND_RETURN(scope, JSValue::encode(builtinFormat(globalObject, args[0], asString(args[1])->value(globalObject))));
}

// ---- int

// The int that a string spells, in the base. Empty if it spells none.
static JSValue parseInt(JSGlobalObject* globalObject, StringView text, unsigned base)
{
    unsigned start = 0;
    unsigned end = text.length();
    while (start < end && isUnicodeCompatibleASCIIWhitespace(text[start]))
        ++start;
    while (end > start && isUnicodeCompatibleASCIIWhitespace(text[end - 1]))
        --end;
    bool isNegative = false;
    if (start < end && (text[start] == '+' || text[start] == '-'))
        isNegative = text[start++] == '-';

    auto hasPrefix = [&] (char lower) {
        return end - start >= 2 && text[start] == '0' && toASCIILower(text[start + 1]) == lower;
    };
    bool hadPrefix = false;
    if (!base) {
        base = hasPrefix('x') ? 16 : hasPrefix('o') ? 8 : hasPrefix('b') ? 2 : 10;
        if (base != 10)
            hadPrefix = true;
        else if (end - start > 1 && text[start] == '0') {
            // Only zero may begin with a zero.
            for (unsigned i = start; i < end; ++i) {
                if (text[i] != '0' && text[i] != '_')
                    return { };
            }
        }
    } else
        hadPrefix = (base == 16 && hasPrefix('x')) || (base == 8 && hasPrefix('o')) || (base == 2 && hasPrefix('b'));
    if (hadPrefix) {
        start += 2;
        if (start < end && text[start] == '_')
            ++start;
    }
    if (start >= end)
        return { };

    Vector<Latin1Character, 32> digits;
    bool previousWasUnderscore = true; // Not at the front.
    for (unsigned i = start; i < end; ++i) {
        char16_t c = text[i];
        if (c == '_') {
            if (previousWasUnderscore)
                return { };
            previousWasUnderscore = true;
            continue;
        }
        previousWasUnderscore = false;
        if (!isASCIIAlphanumeric(c) || static_cast<unsigned>(isASCIIDigit(c) ? c - '0' : toASCIILower(c) - 'a' + 10) >= base)
            return { };
        digits.append(static_cast<Latin1Character>(c));
    }
    if (previousWasUnderscore)
        return { };

    JSValue magnitude = JSBigInt::parseInt(globalObject, globalObject->vm(), StringView(digits.span()), base, JSBigInt::ErrorParseMode::IgnoreExceptions, JSBigInt::ParseIntSign::Unsigned);
    if (!magnitude)
        return { };
    if (isNegative)
        magnitude = JSBigInt::unaryMinus(globalObject, magnitude.asHeapBigInt());
    return normalizeBigInt(magnitude);
}

// int(x=0), int(x, base=10)
PYTHON_NATIVE(intNew)
{
    NATIVE_PROLOGUE();
    auto* type = uncheckedDowncast<PyType>(args.at(0).asCell());
    JSValue value = args.at(1);
    JSValue baseValue = args.at(2);
    if (!baseValue)
        baseValue = args.keyword(globalObject, "base"_s);
    if (args.size() > 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("int() takes at most 2 arguments ("_s, args.size() - 1, " given)"_s)));

    JSValue result;
    if (!value) {
        if (baseValue)
            return JSValue::encode(raiseTypeError(globalObject, scope, "int() missing string argument"_s));
        result = jsNumber(0);
    } else if (baseValue || unbox(value).isString()) {
        JSValue string = unbox(value);
        if (!string.isString())
            return JSValue::encode(raiseTypeError(globalObject, scope, "int() can't convert non-string with explicit base"_s));
        int64_t base = 10;
        if (baseValue) {
            auto index = toIndex(globalObject, baseValue, true);
            RETURN_IF_EXCEPTION(scope, { });
            base = *index;
            if (base && (base < 2 || base > 36))
                return JSValue::encode(raiseValueError(globalObject, scope, "int() base must be >= 2 and <= 36, or 0"_s));
        }
        auto view = asString(string)->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        result = parseInt(globalObject, view, base);
        RETURN_IF_EXCEPTION(scope, { });
        if (!result)
            return JSValue::encode(raiseValueError(globalObject, scope, makeString("invalid literal for int() with base "_s, base, ": "_s, reprOfString(view))));
    } else if (Number number = classify(value)) {
        result = toInt(globalObject, scope, number);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        JSValue self;
        JSValue method;
        for (const Identifier* name : { &names.dunder_int, &names.dunder_index, &names.dunder_trunc }) {
            method = lookupSpecial(globalObject, value, *name, self);
            RETURN_IF_EXCEPTION(scope, { });
            if (method)
                break;
        }
        if (!method)
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("int() argument must be a string, a bytes-like object or a real number, not '"_s, typeName(globalObject, value), '\'')));
        result = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, { });
        Number converted = classify(result);
        if (!converted.isInt())
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("__int__ returned non-int (type "_s, typeName(globalObject, result), ')')));
        result = toInt(globalObject, scope, converted);
    }
    return JSValue::encode(boxIfDerived(globalObject, type, realm->typeInt(), result));
}

PYTHON_NATIVE(intBitLength)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    Number number = classify(args.at(0));
    if (number.kind == Number::Kind::Small) {
        uint32_t magnitude = number.small < 0 ? 0u - static_cast<uint32_t>(number.small) : number.small;
        return JSValue::encode(jsNumber(32 - std::countl_zero(magnitude)));
    }
    unsigned length = number.big->length();
    return JSValue::encode(jsNumber(static_cast<int32_t>((length - 1) * 64 + (64 - std::countl_zero(static_cast<uint64_t>(number.big->digit(length - 1)))))));
}

PYTHON_NATIVE(intBitCount)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    Number number = classify(args.at(0));
    if (number.kind == Number::Kind::Small) {
        uint32_t magnitude = number.small < 0 ? 0u - static_cast<uint32_t>(number.small) : number.small;
        return JSValue::encode(jsNumber(std::popcount(magnitude)));
    }
    int32_t count = 0;
    for (unsigned i = 0; i < number.big->length(); ++i)
        count += std::popcount(static_cast<uint64_t>(number.big->digit(i)));
    return JSValue::encode(jsNumber(count));
}

PYTHON_NATIVE(intAsIntegerRatio)
{
    NATIVE_PROLOGUE();
    JSValue self = toInt(globalObject, scope, classify(args.at(0)));
    return JSValue::encode(PyTuple::create(globalObject, { self, jsNumber(1) }));
}

PYTHON_NATIVE(returnTrue)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsBoolean(true));
}

// ---- float

// The float that a string spells. Nothing if it spells none.
static std::optional<double> parseFloat(StringView text)
{
    Vector<Latin1Character, 32> characters;
    unsigned start = 0;
    unsigned end = text.length();
    while (start < end && isUnicodeCompatibleASCIIWhitespace(text[start]))
        ++start;
    while (end > start && isUnicodeCompatibleASCIIWhitespace(text[end - 1]))
        --end;
    for (unsigned i = start; i < end; ++i) {
        char16_t c = text[i];
        if (c == '_') {
            // Only between digits.
            if (i == start || i + 1 == end || !isASCIIDigit(text[i - 1]) || !isASCIIDigit(text[i + 1]))
                return std::nullopt;
            continue;
        }
        if (c >= 0x80)
            return std::nullopt;
        characters.append(toASCIILower(static_cast<Latin1Character>(c)));
    }
    if (characters.isEmpty())
        return std::nullopt;
    StringView body { characters.span() };
    double sign = 1;
    if (body[0] == '+' || body[0] == '-') {
        sign = body[0] == '-' ? -1 : 1;
        body = body.substring(1);
    }
    if (body == "inf"_s || body == "infinity"_s)
        return sign * std::numeric_limits<double>::infinity();
    if (body == "nan"_s)
        return std::copysign(std::numeric_limits<double>::quiet_NaN(), sign);
    if (body.isEmpty() || !(isASCIIDigit(body[0]) || body[0] == '.'))
        return std::nullopt;
    if (body == "."_s || body.startsWith(".e"_s))
        return std::nullopt;
    size_t parsed;
    double value = parseDouble(body, parsed);
    if (parsed != body.length())
        return std::nullopt;
    return sign * value;
}

PYTHON_NATIVE(floatNew)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "float"_s, 1, 2))
        return { };
    auto* type = uncheckedDowncast<PyType>(args[0].asCell());
    JSValue value = args.at(1);
    double result = 0;
    if (value) {
        JSValue plain = unbox(value);
        if (plain.isString()) {
            auto view = asString(plain)->view(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            auto parsed = parseFloat(view);
            if (!parsed)
                return JSValue::encode(raiseValueError(globalObject, scope, makeString("could not convert string to float: "_s, reprOfString(view))));
            result = *parsed;
        } else {
            if (!classify(value) && !typeOf(globalObject, value)->lookup(vm, names.dunder_float) && !typeOf(globalObject, value)->lookup(vm, names.dunder_index))
                return JSValue::encode(raiseTypeError(globalObject, scope, makeString("float() argument must be a string or a real number, not '"_s, typeName(globalObject, value), '\'')));
            auto converted = toDouble(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            result = *converted;
        }
    }
    return JSValue::encode(boxIfDerived(globalObject, type, realm->typeFloat(), floatFromDouble(result)));
}

PYTHON_NATIVE(floatIsInteger)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    double value = classify(args.at(0)).real;
    return JSValue::encode(jsBoolean(std::isfinite(value) && std::floor(value) == value));
}

PYTHON_NATIVE(floatAsIntegerRatio)
{
    NATIVE_PROLOGUE();
    double value = classify(args.at(0)).real;
    if (std::isinf(value))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "cannot convert Infinity to integer ratio"_s));
    if (std::isnan(value))
        return JSValue::encode(raiseValueError(globalObject, scope, "cannot convert NaN to integer ratio"_s));
    int exponent;
    double mantissa = std::frexp(value, &exponent);
    for (int i = 0; i < 300 && mantissa != std::floor(mantissa); ++i) {
        mantissa *= 2.0;
        --exponent;
    }
    JSValue numerator = intFromDouble(globalObject, mantissa);
    JSValue denominator = jsNumber(1);
    JSValue shift = jsNumber(std::abs(exponent));
    if (exponent > 0)
        numerator = binaryOperation(globalObject, BinaryOperator::LShift, false, numerator, shift);
    else
        denominator = binaryOperation(globalObject, BinaryOperator::LShift, false, denominator, shift);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { numerator, denominator }));
}

// ---- bool

PYTHON_NATIVE(boolNew)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "bool"_s, 1, 2))
        return { };
    if (args.size() == 1)
        return JSValue::encode(jsBoolean(false));
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(isTrue(globalObject, args[1]))));
}

static void addArithmetic(JSGlobalObject* globalObject, PyType* target, NumberType type)
{
    addOperator(globalObject, target, type, BinaryOperator::Add);
    addOperator(globalObject, target, type, BinaryOperator::Sub);
    addOperator(globalObject, target, type, BinaryOperator::Mult);
    addOperator(globalObject, target, type, BinaryOperator::Div);
    addOperator(globalObject, target, type, BinaryOperator::FloorDiv);
    addOperator(globalObject, target, type, BinaryOperator::Mod);
    addOperator(globalObject, target, type, BinaryOperator::Pow);
    addMethods(globalObject, target, {
        { "__divmod__"_s, numberDivmod, PyNativeFunction::Kind::Method, pack(false) },
        { "__rdivmod__"_s, numberDivmod, PyNativeFunction::Kind::Method, pack(true) },
        { "__neg__"_s, numberUnary, PyNativeFunction::Kind::Method, pack(UnaryOperator::USub) },
        { "__pos__"_s, numberUnary, PyNativeFunction::Kind::Method, pack(UnaryOperator::UAdd) },
        { "__abs__"_s, numberAbs },
        { "__bool__"_s, numberBool },
        { "__int__"_s, numberInt },
        { "__trunc__"_s, numberInt },
        { "__float__"_s, numberFloat },
        { "__floor__"_s, numberFloorOrCeil, PyNativeFunction::Kind::Method, pack(true) },
        { "__ceil__"_s, numberFloorOrCeil, PyNativeFunction::Kind::Method, pack(false) },
        { "__round__"_s, numberRound },
        { "__format__"_s, numberFormat },
        { "__repr__"_s, nativeRepr },
        { "__hash__"_s, nativeHash },
        { "conjugate"_s, numberUnary, PyNativeFunction::Kind::Method, pack(UnaryOperator::UAdd) },
    });
    addComparisons(globalObject, target, true);
    addGetSet(globalObject, target, "real"_s, [] (JSGlobalObject* globalObject, JSValue self) { return numberUnaryOperation(globalObject, UnaryOperator::UAdd, self); });
    addGetSet(globalObject, target, "imag"_s, [] (JSGlobalObject*, JSValue self) { return classify(self).kind == Number::Kind::Float ? floatFromDouble(0) : jsNumber(0); });
}

void initializeNumberTypes(JSGlobalObject* globalObject)
{
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;

    PyType* intType = realm->typeInt();
    addArithmetic(globalObject, intType, NumberType::Int);
    addOperator(globalObject, intType, NumberType::Int, BinaryOperator::LShift);
    addOperator(globalObject, intType, NumberType::Int, BinaryOperator::RShift);
    addOperator(globalObject, intType, NumberType::Int, BinaryOperator::BitAnd);
    addOperator(globalObject, intType, NumberType::Int, BinaryOperator::BitOr);
    addOperator(globalObject, intType, NumberType::Int, BinaryOperator::BitXor);
    addMethods(globalObject, intType, {
        { "__new__"_s, intNew, Kind::Function },
        { "__index__"_s, numberInt },
        { "__invert__"_s, numberUnary, PyNativeFunction::Kind::Method, pack(UnaryOperator::Invert) },
        { "bit_length"_s, intBitLength },
        { "bit_count"_s, intBitCount },
        { "as_integer_ratio"_s, intAsIntegerRatio },
        { "is_integer"_s, returnTrue },
    });
    addGetSet(globalObject, intType, "numerator"_s, [] (JSGlobalObject* globalObject, JSValue self) { return numberUnaryOperation(globalObject, UnaryOperator::UAdd, self); });
    addGetSet(globalObject, intType, "denominator"_s, [] (JSGlobalObject*, JSValue) -> JSValue { return jsNumber(1); });

    PyType* floatType = realm->typeFloat();
    addArithmetic(globalObject, floatType, NumberType::Float);
    addMethods(globalObject, floatType, {
        { "__new__"_s, floatNew, Kind::Function },
        { "is_integer"_s, floatIsInteger },
        { "as_integer_ratio"_s, floatAsIntegerRatio },
    });

    addMethods(globalObject, realm->typeBool(), {
        { "__new__"_s, boolNew, Kind::Function },
        { "__repr__"_s, nativeRepr },
    });
}

} } // namespace JSC::Python
