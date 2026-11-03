/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyAbstract.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PyStr.h"

namespace JSC {
namespace Python {

// ---- Binary operators ----

static constexpr ASCIILiteral binarySymbols[] = { "+"_s, "-"_s, "*"_s, "@"_s, "/"_s, "%"_s, "** or pow()"_s, "<<"_s, ">>"_s, "|"_s, "^"_s, "&"_s, "//"_s };
static constexpr ASCIILiteral inPlaceSymbols[] = { "+="_s, "-="_s, "*="_s, "@="_s, "/="_s, "%="_s, "**="_s, "<<="_s, ">>="_s, "|="_s, "^="_s, "&="_s, "//="_s };

static constexpr unsigned numberOfBinaryOperators = static_cast<unsigned>(BinaryOperator::FirstInPlace);

static ALWAYS_INLINE Special methodFor(unsigned op) { return static_cast<Special>(static_cast<unsigned>(Special::Add) + op); }
static ALWAYS_INLINE Special reflectedMethodFor(unsigned op) { return static_cast<Special>(static_cast<unsigned>(Special::RAdd) + op); }
static ALWAYS_INLINE Special inPlaceMethodFor(unsigned op) { return static_cast<Special>(static_cast<unsigned>(Special::IAdd) + op); }

static_assert(static_cast<unsigned>(Special::FloorDiv) - static_cast<unsigned>(Special::Add) == static_cast<unsigned>(BinaryOperator::FloorDiv));
static_assert(static_cast<unsigned>(Special::RFloorDiv) - static_cast<unsigned>(Special::RAdd) == static_cast<unsigned>(BinaryOperator::FloorDiv));
static_assert(static_cast<unsigned>(Special::IFloorDiv) - static_cast<unsigned>(Special::IAdd) == static_cast<unsigned>(BinaryOperator::FloorDiv));

// list.__add__ and the like are not tried until the methods of numbers have been, on both operands.
static ALWAYS_INLINE bool isSequenceOperation(JSValue method)
{
    auto* native = PyNativeFunction::tryFrom(method);
    return native && (native->def().flags & SequenceOperation);
}

// For an operator in JS code, only the classes of objects of Python have a say: "%s" % x is not a format there.
enum class OperatorOf : bool { Python, JS };

// The result, or NotImplemented.
static JSValue numberOperation(JSGlobalObject* globalObject, Special method, Special reflected, JSValue a, JSValue b, OperatorOf language = OperatorOf::Python)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue notImplemented = rt.notImplemented();

    PyType* typeA = rt.typeOf(a);
    PyType* typeB = rt.typeOf(b);
    JSValue left = typeA->lookup(method);
    if (left && (isSequenceOperation(left) || (language == OperatorOf::JS && !isPyObject(a))))
        left = JSValue();
    JSValue right;
    if (typeB != typeA) {
        right = typeB->lookup(reflected);
        if (right && (isSequenceOperation(right) || (language == OperatorOf::JS && !isPyObject(b))))
            right = JSValue();
    }

    // A subclass that has its own idea of the reflected operation goes first.
    if (right && typeB->isSubtypeOf(typeA) && right != typeA->lookup(reflected)) {
        JSValue result = callOperatorMethod(globalObject, right, b, a);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
        right = JSValue();
    }
    if (left) {
        JSValue result = callOperatorMethod(globalObject, left, a, b);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
    }
    if (right) {
        JSValue result = callOperatorMethod(globalObject, right, b, a);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
    }
    return notImplemented;
}

static NEVER_INLINE JSValue raiseUnsupportedOperands(JSGlobalObject* globalObject, ASCIILiteral symbol, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "unsupported operand type(s) for "_s, symbol, ": '"_s, typeName(globalObject, a), "' and '"_s, typeName(globalObject, b), '\'');
}

// The result, or NotImplemented.
static JSValue binaryOperationOrNotImplemented(JSGlobalObject* globalObject, unsigned op, JSValue a, JSValue b, bool inPlace, OperatorOf language)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue result = numberOperation(globalObject, methodFor(op), reflectedMethodFor(op), a, b, language);
    RETURN_IF_EXCEPTION(scope, { });
    if (result != rt.notImplemented())
        return result;
    bool asksA = language == OperatorOf::Python || isPyObject(a);
    bool asksB = language == OperatorOf::Python || isPyObject(b);

    // Concatenation and repetition of sequences.
    auto sequenceMethod = [&](Special method, Special inPlaceMethod) {
        PyType* type = rt.typeOf(a);
        if (inPlace) {
            if (JSValue found = type->lookup(inPlaceMethod); found && isSequenceOperation(found))
                return found;
        }
        return type->lookup(method);
    };
    if (op == static_cast<unsigned>(BinaryOperator::Add)) {
        JSValue method = asksA ? sequenceMethod(Special::Add, Special::IAdd) : JSValue();
        if (method && isSequenceOperation(method))
            RELEASE_AND_RETURN(scope, callSpecial(globalObject, method, a, b));
    } else if (op == static_cast<unsigned>(BinaryOperator::Mult)) {
        JSValue method = asksA ? sequenceMethod(Special::Mul, Special::IMul) : JSValue();
        if (method && isSequenceOperation(method)) {
            if (!hasIndex(globalObject, b))
                return raiseTypeError(globalObject, scope, "can't multiply sequence by non-int of type '"_s, typeName(globalObject, b), '\'');
            RELEASE_AND_RETURN(scope, callSpecial(globalObject, method, a, b));
        }
        // PyNumber_InPlaceMultiply() goes on to the right operand only if the type of the left one has no table of sequence methods.
        if (inPlace && hasTableOfSequenceMethods(rt.typeOf(a)))
            return rt.notImplemented();
        method = asksB ? rt.typeOf(b)->lookup(Special::Mul) : JSValue();
        if (method && isSequenceOperation(method)) {
            if (!hasIndex(globalObject, a))
                return raiseTypeError(globalObject, scope, "can't multiply sequence by non-int of type '"_s, typeName(globalObject, a), '\'');
            RELEASE_AND_RETURN(scope, callSpecial(globalObject, method, b, a));
        }
    }
    return rt.notImplemented();
}

static JSValue binaryOperationSlow(JSGlobalObject* globalObject, unsigned op, JSValue a, JSValue b, ASCIILiteral symbol, bool inPlace)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = binaryOperationOrNotImplemented(globalObject, op, a, b, inPlace, OperatorOf::Python);
    RETURN_IF_EXCEPTION(scope, { });
    if (result != runtime(globalObject).notImplemented())
        return result;
    RELEASE_AND_RETURN(scope, raiseUnsupportedOperands(globalObject, symbol, a, b));
}

// ---- The operators of JS (PyHooks.h) ----

JSValue binaryOperatorForJS(JSGlobalObject* globalObject, JSOperator op, JSValue a, JSValue b)
{
    static constexpr BinaryOperator operators[] = {
        BinaryOperator::Add, BinaryOperator::Sub, BinaryOperator::Mult, BinaryOperator::Div, BinaryOperator::Mod, BinaryOperator::Pow,
        BinaryOperator::LShift, BinaryOperator::RShift, BinaryOperator::BitOr, BinaryOperator::BitXor, BinaryOperator::BitAnd,
    };
    ASSERT(static_cast<unsigned>(op) < std::size(operators));
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = binaryOperationOrNotImplemented(globalObject, static_cast<unsigned>(operators[static_cast<unsigned>(op)]), a, b, false, OperatorOf::JS);
    RETURN_IF_EXCEPTION(scope, { });
    if (result == runtime(globalObject).notImplemented())
        return { };
    return valueForJS(result);
}

JSValue unaryOperatorForJS(JSGlobalObject* globalObject, JSOperator op, JSValue operand)
{
    ASSERT(op == JSOperator::Negate || op == JSOperator::BitNot);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = typeOf(globalObject, operand)->lookup(op == JSOperator::Negate ? Special::Neg : Special::Invert);
    if (!method)
        return { };
    JSValue result = callSpecial(globalObject, method, operand);
    RETURN_IF_EXCEPTION(scope, { });
    return valueForJS(result);
}

JSValue binaryOperation(JSGlobalObject* globalObject, BinaryOperator binaryOperator, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    unsigned op = static_cast<unsigned>(binaryOperator);
    bool inPlace = op >= numberOfBinaryOperators;
    if (inPlace)
        op -= numberOfBinaryOperators;

    JSValue fast = tryNumberBinaryOperation(globalObject, static_cast<BinaryOperator>(op), a, b);
    RETURN_IF_EXCEPTION(scope, { });
    if (fast)
        return fast;
    if (op == static_cast<unsigned>(BinaryOperator::Add) && a.isString() && b.isString())
        RELEASE_AND_RETURN(scope, jsString(globalObject, asString(a), asString(b)));

    if (inPlace) {
        Runtime& rt = runtime(globalObject);
        if (JSValue method = rt.typeOf(a)->lookup(inPlaceMethodFor(op)); method && !isSequenceOperation(method)) {
            JSValue result = callSpecial(globalObject, method, a, b);
            RETURN_IF_EXCEPTION(scope, { });
            if (result != rt.notImplemented())
                return result;
        }
    }
    RELEASE_AND_RETURN(scope, binaryOperationSlow(globalObject, op, a, b, inPlace ? inPlaceSymbols[op] : binarySymbols[op], inPlace));
}

JSValue power(JSGlobalObject* globalObject, JSValue base, JSValue exponent, JSValue modulus)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!modulus || isNone(modulus))
        RELEASE_AND_RETURN(scope, binaryOperation(globalObject, BinaryOperator::Pow, base, exponent));

    // pow(a, b, c): only the first operand's type, then the second's, get a say.
    JSValue notImplemented = rt.notImplemented();
    PyType* typeA = rt.typeOf(base);
    PyType* typeB = rt.typeOf(exponent);
    JSValue reflected = typeB != typeA ? typeB->lookup(Special::RPow) : JSValue();
    // As for the binary operators: a subclass that has its own idea of the reflected operation goes first.
    if (reflected && typeB->isSubtypeOf(typeA) && reflected != typeA->lookup(Special::RPow)) {
        JSValue result = callOperatorMethod(globalObject, reflected, exponent, base, modulus);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
        reflected = JSValue();
    }
    if (JSValue method = typeA->lookup(Special::Pow)) {
        JSValue result = callOperatorMethod(globalObject, method, base, exponent, modulus);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
    }
    if (reflected) {
        JSValue result = callOperatorMethod(globalObject, reflected, exponent, base, modulus);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
    }
    // The type of the third is asked last, and those that are numbers only have this to say.
    PyType* typeC = rt.typeOf(modulus);
    if (typeC->has(TypeFlag::FloatSubclass))
        return raiseTypeError(globalObject, scope, "pow() 3rd argument not allowed unless all arguments are integers"_s);
    if (typeC->isSubtypeOf(rt.complexType()))
        return raiseValueError(globalObject, scope, "complex modulo"_s);
    return raiseTypeError(globalObject, scope, "unsupported operand type(s) for ** or pow(): '"_s, typeA->nameInMessages(), "', '"_s, typeB->nameInMessages(), "', '"_s, typeC->nameInMessages(), '\'');
}

JSValue divmod(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = numberOperation(globalObject, Special::DivMod, Special::RDivMod, a, b);
    RETURN_IF_EXCEPTION(scope, { });
    if (result != runtime(globalObject).notImplemented())
        return result;
    RELEASE_AND_RETURN(scope, raiseUnsupportedOperands(globalObject, "divmod()"_s, a, b));
}

// ---- Unary operators ----

JSValue unaryOperation(JSGlobalObject* globalObject, UnaryOperator op, JSValue operand)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    Special method;
    ASCIILiteral symbol;
    switch (op) {
    case UnaryOperator::Not: {
        bool truth = isTrue(globalObject, operand);
        RETURN_IF_EXCEPTION(scope, { });
        return jsBoolean(!truth);
    }
    case UnaryOperator::Negative:
        if (operand.isInt32() && operand.asInt32() && operand.asInt32() != std::numeric_limits<int32_t>::min())
            return jsNumber(-operand.asInt32());
        method = Special::Neg;
        symbol = "-"_s;
        break;
    case UnaryOperator::Positive:
        if (operand.isNumber())
            return operand;
        method = Special::Pos;
        symbol = "+"_s;
        break;
    case UnaryOperator::Invert:
        if (operand.isInt32())
            return jsNumber(~operand.asInt32());
        method = Special::Invert;
        symbol = "~"_s;
        break;
    }
    PyType* type = typeOf(globalObject, operand);
    JSValue function = type->lookup(method);
    if (!function)
        return raiseTypeError(globalObject, scope, "bad operand type for unary "_s, symbol, ": '"_s, type->nameInMessages(), '\'');
    RELEASE_AND_RETURN(scope, callSpecial(globalObject, function, operand));
}

JSValue absolute(JSGlobalObject* globalObject, JSValue operand)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, operand);
    JSValue function = type->lookup(Special::Abs);
    if (!function)
        return raiseTypeError(globalObject, scope, "bad operand type for abs(): '"_s, type->nameInMessages(), '\'');
    RELEASE_AND_RETURN(scope, callSpecial(globalObject, function, operand));
}

// ---- Conversions ----

bool hasIndex(JSGlobalObject* globalObject, JSValue value)
{
    return isInt(value) || typeOf(globalObject, value)->lookup(Special::Index);
}

static NEVER_INLINE void warnOfIntSubclass(JSGlobalObject* globalObject, PyType* type, ASCIILiteral method, JSValue result)
{
    warn(globalObject, runtime(globalObject).DeprecationWarning(), makeString(typeNameForMessage(globalObject, type), '.', method, "() must return an int, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, result)), ".  The ability to return an instance of a strict subclass of int is deprecated, and may be removed in a future version of Python."_s));
}

JSValue toIndex(JSGlobalObject* globalObject, JSValue value)
{
    if (isExactInt(value)) [[likely]]
        return value;
    if (isInt(value))
        return intValue(value);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, value);
    JSValue method = type->lookup(Special::Index);
    if (!method)
        return raiseTypeError(globalObject, scope, '\'', type->name(), "' object cannot be interpreted as an integer"_s);
    JSValue result = callSpecial(globalObject, method, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInt(result))
        return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__index__() must return an int, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, result)));
    if (!isExactInt(result)) [[unlikely]] {
        warnOfIntSubclass(globalObject, type, "__index__"_s, result);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return intValue(result);
}

int64_t toSsize(JSGlobalObject* globalObject, JSValue value, PyType* overflowError)
{
    if (value.isInt32()) [[likely]]
        return value.asInt32();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, -1);
    if (auto result = tryInt64(index))
        return *result;
    if (!overflowError)
        return intSign(index) < 0 ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
    raise(globalObject, scope, overflowError, makeString("cannot fit '"_s, typeName(globalObject, value), "' into an index-sized integer"_s));
    return -1;
}

int64_t toInt64(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isInt32()) [[likely]]
        return value.asInt32();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, -1);
    if (auto result = tryInt64(index))
        return *result;
    raiseOverflowError(globalObject, scope, "Python int too large to convert to C long"_s);
    return -1;
}

int toCInt(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isInt32()) [[likely]]
        return value.asInt32();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t result = toInt64(globalObject, value);
    if (scope.exception()) [[unlikely]] {
        if (!clearPendingExceptionIfMatches(globalObject, runtime(globalObject).OverflowError()))
            return -1;
        result = std::numeric_limits<int64_t>::max();
    }
    if (result < std::numeric_limits<int>::min() || result > std::numeric_limits<int>::max()) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C int"_s);
        return -1;
    }
    return static_cast<int>(result);
}

double toDouble(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isNumber()) [[likely]]
        return value.asNumber();
    if (isFloat(value))
        return floatValue(value);
    return toDoubleAsFloatDoes(globalObject, value);
}

double toDoubleAsFloatDoes(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isNumber()) [[likely]]
        return value.asNumber();
    if (isFloat(value) && typeOf(globalObject, value) == runtime(globalObject).floatType())
        return floatValue(value);

    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto fromInt = [&](JSValue integer) -> double {
        double result = intToDouble(integer);
        if (std::isinf(result)) {
            raiseOverflowError(globalObject, scope, "int too large to convert to float"_s);
            return -1;
        }
        return result;
    };
    // A class derived from int may have a __float__ of its own.
    if (isExactInt(value))
        return fromInt(value);

    PyType* type = typeOf(globalObject, value);
    if (JSValue method = type->lookup(Special::Float)) {
        JSValue result = callSpecial(globalObject, method, value);
        RETURN_IF_EXCEPTION(scope, -1);
        PyType* resultType = typeOf(globalObject, result);
        if (!isFloat(result)) {
            raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__float__() must return a float, not "_s, typeNameForMessage(globalObject, resultType));
            return -1;
        }
        if (resultType != runtime(globalObject).floatType()) {
            warn(globalObject, runtime(globalObject).DeprecationWarning(), makeString(typeNameForMessage(globalObject, type), ".__float__() must return a float, not "_s, typeNameForMessage(globalObject, resultType), ".  The ability to return an instance of a strict subclass of float is deprecated, and may be removed in a future version of Python."_s));
            RETURN_IF_EXCEPTION(scope, -1);
        }
        return floatValue(result);
    }
    if (type->lookup(Special::Index)) {
        JSValue index = toIndex(globalObject, value);
        RETURN_IF_EXCEPTION(scope, -1);
        return fromInt(index);
    }
    raiseTypeError(globalObject, scope, "must be real number, not "_s, type->name());
    return -1;
}

JSValue toInt(JSGlobalObject* globalObject, JSValue value)
{
    if (isExactInt(value))
        return value;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, value);
    if (JSValue method = type->lookup(Special::Int)) {
        JSValue result = callSpecial(globalObject, method, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isInt(result))
            return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__int__() must return an int, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, result)));
        if (!isExactInt(result)) [[unlikely]] {
            warnOfIntSubclass(globalObject, type, "__int__"_s, result);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return intValue(result);
    }
    if (type->lookup(Special::Index))
        RELEASE_AND_RETURN(scope, toIndex(globalObject, value));
    return raiseTypeError(globalObject, scope, "int() argument must be a string, a bytes-like object or a real number, not '"_s, type->nameInMessages(), '\'');
}

} // namespace Python
} // namespace JSC
