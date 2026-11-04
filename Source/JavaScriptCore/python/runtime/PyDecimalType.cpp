/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyDecimal.h"

#include "JSBigInt.h"
#include "JSCInlines.h"
#include "PyBytes.h"
#include "PyCodecs.h"
#include "PyDict.h"
#include "PyModuleHelpers.h"
#include "PyTuple.h"
#include <cmath>
#include <cwchar>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Decimal {

// ---- Decimal(value='0', context=None) ----

static JSValue decimalNew(JSGlobalObject* globalObject, JSValue cls, JSValue value, JSValue contextValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = contextArgument(globalObject, state, contextValue);
    if (failed(scope, context)) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, decimalFromObjectExactly(globalObject, state, PyType::from(cls), value, context));
}

// An instance of the class, which may be derived from Decimal, with the value of the Decimal.
static JSValue asInstanceOf(JSGlobalObject* globalObject, State& state, JSValue cls, JSValue decimal)
{
    if (!decimal || cls == JSValue(state.decimalType.get()))
        return decimal;
    return call(globalObject, cls, decimal);
}

static JSValue decimalFromFloatMethod(JSGlobalObject* globalObject, JSValue cls, JSValue number)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = currentContext(globalObject, state);
    if (failed(scope, context)) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, asInstanceOf(globalObject, state, cls, decimalFromFloat(globalObject, state, number, context, true)));
}

static JSValue decimalFromNumber(JSGlobalObject* globalObject, JSValue cls, JSValue number)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = currentContext(globalObject, state);
    if (failed(scope, context)) [[unlikely]]
        return { };
    if (!PyDecimal::is(number) && !isInt(number) && !isFloat(number))
        return raiseTypeError(globalObject, scope, "conversion from "_s, typeName(globalObject, number), " to Decimal is not supported"_s);
    RELEASE_AND_RETURN(scope, asInstanceOf(globalObject, state, cls, decimalFromObjectExactly(globalObject, state, state.decimalType.get(), number, context)));
}

// ---- Operators ----

template<UnaryOperation operation>
static JSValue decimalUnaryOperator(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = currentContext(globalObject, state);
    if (failed(scope, context)) [[unlikely]]
        return { };
    Number a;
    PyDecimal::from(self)->lend(a);
    Number result;
    uint32_t status = 0;
    operation(result, a, context->context, status);
    RELEASE_AND_RETURN(scope, finish(globalObject, state, context, result, status));
}

// Either operand may be the Decimal.
template<BinaryOperation operation>
static JSValue decimalBinaryOperator(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = currentContext(globalObject, state);
    if (failed(scope, context)) [[unlikely]]
        return { };
    Number a;
    Number b;
    Conversion conversion = convertOperand(globalObject, state, context, left, a, false);
    if (conversion == Conversion::Done) [[likely]]
        conversion = convertOperand(globalObject, state, context, right, b, false);
    if (conversion != Conversion::Done) [[unlikely]]
        RELEASE_AND_RETURN(scope, conversion == Conversion::NotImplemented ? runtime(globalObject).notImplemented() : JSValue());
    Number result;
    uint32_t status = 0;
    operation(result, a, b, context->context, status);
    RELEASE_AND_RETURN(scope, finish(globalObject, state, context, result, status));
}

template<JSValue (*operation)(JSGlobalObject*, JSValue, JSValue)>
static JSValue decimalReflected(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    return operation(globalObject, other, self);
}

static JSValue decimalDivMod(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = currentContext(globalObject, state);
    if (failed(scope, context)) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, applyDivMod(globalObject, state, context, left, right, false));
}

static JSValue decimalPower(JSGlobalObject* globalObject, JSValue base, JSValue exponent, JSValue modulus)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = currentContext(globalObject, state);
    if (failed(scope, context)) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, applyPower(globalObject, state, context, base, exponent, modulus, false));
}

static JSValue decimalReflectedPower(JSGlobalObject* globalObject, JSValue self, JSValue base, JSValue modulus)
{
    return decimalPower(globalObject, base, self, modulus);
}

static JSValue decimalBool(JSGlobalObject*, JSValue self) { return jsBoolean(!PyDecimal::from(self)->isZero()); }

// ---- Comparisons ----

// The other operand of a comparison that is no Decimal. For a rational number n/d, a is compared with n after it has been multiplied
// by d: `scaled`, which `left` then points to.
static Conversion convertForComparison(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, const Number*& left, Number& scaled, JSValue other, Number& b, bool isEquality)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t status = 0;
    auto report = [&] {
        return status && addStatus(globalObject, state, context, status) ? Conversion::Raised : Conversion::Done;
    };
    auto integerAttribute = [&](ASCIILiteral name, Number& number) {
        JSValue value = getAttr(globalObject, other, internString(vm, name));
        RETURN_IF_EXCEPTION(scope, false);
        if (!isInt(value)) {
            raiseTypeError(globalObject, scope, "argument must be an integer"_s);
            return false;
        }
        numberFromIntExactly(number, intValue(value), status);
        return !failed(scope, report() == Conversion::Done);
    };

    if (isInt(other)) {
        numberFromIntExactly(b, intValue(other), status);
        RELEASE_AND_RETURN(scope, report());
    }
    if (isFloat(other)) {
        if (!isEquality && failed(scope, !addStatus(globalObject, state, context, FloatOperation)))
            return Conversion::Raised;
        context->signals->status |= FloatOperation;
        numberFromDouble(b, floatValue(other), status);
        RELEASE_AND_RETURN(scope, report());
    }
    if (auto* complex = PyComplex::tryFrom(other); complex && isEquality) {
        if (complex->imaginary())
            return Conversion::NotImplemented;
        context->signals->status |= FloatOperation;
        numberFromDouble(b, complex->real(), status);
        RELEASE_AND_RETURN(scope, report());
    }
    bool isRational = isInstance(globalObject, other, state.rational.get());
    RETURN_IF_EXCEPTION(scope, Conversion::Raised);
    if (!isRational)
        return Conversion::NotImplemented;
    if (!integerAttribute("numerator"_s, b))
        return Conversion::Raised;
    if (left->isSpecial())
        return Conversion::Done;
    Number denominator;
    if (!integerAttribute("denominator"_s, denominator))
        return Conversion::Raised;
    // The exponent is set aside, so that nothing overflows: the comparison does not mind a number that is out of bounds.
    Number coefficient(Number::Borrow, left->flags, 0, left->digits, left->length, left->limbs());
    multiply(scaled, coefficient, denominator, Context::maximum(), status);
    if (status) {
        raiseValueError(globalObject, scope, "exact conversion for comparison failed"_s);
        return Conversion::Raised;
    }
    scaled.exponent = left->exponent;
    left = &scaled;
    return Conversion::Done;
}

template<CompareOperator op>
static ALWAYS_INLINE JSValue comparisonResult(int order)
{
    switch (op) {
    case CompareOperator::Eq:
        return jsBoolean(!order);
    case CompareOperator::NotEq:
        return jsBoolean(order);
    case CompareOperator::Lt:
        return jsBoolean(order < 0);
    case CompareOperator::LtE:
        return jsBoolean(order <= 0);
    case CompareOperator::Gt:
        return jsBoolean(order > 0);
    default:
        return jsBoolean(order >= 0);
    }
}

// With something other than a Decimal, or with a NaN: the context has a say.
template<CompareOperator op>
static NEVER_INLINE JSValue decimalCompareSlow(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    constexpr bool isEquality = op == CompareOperator::Eq || op == CompareOperator::NotEq;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = currentContext(globalObject, state);
    if (failed(scope, context)) [[unlikely]]
        return { };
    Number a;
    PyDecimal::from(self)->lend(a);
    const Number* left = &a;
    Number scaled;
    Number b;
    if (auto* decimal = PyDecimal::tryFrom(other))
        decimal->lend(b);
    else {
        Conversion conversion = convertForComparison(globalObject, state, context, left, scaled, other, b, isEquality);
        RETURN_IF_EXCEPTION(scope, { });
        if (conversion == Conversion::NotImplemented)
            return runtime(globalObject).notImplemented();
    }
    uint32_t status = 0;
    int order = compareQuietly(*left, b, status);
    ensureStillAliveHere(self);
    ensureStillAliveHere(other);
    if (order == unordered) {
        // A signaling NaN signals, and so does any NaN where an order is asked for.
        if ((left->isSignalingNaN() || b.isSignalingNaN() || !isEquality) && failed(scope, !addStatus(globalObject, state, context, status)))
            return { };
        return jsBoolean(op == CompareOperator::NotEq);
    }
    return comparisonResult<op>(order);
}

template<CompareOperator op>
static JSValue decimalCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto* first = PyDecimal::from(self);
    auto* second = PyDecimal::tryFrom(other);
    if (!second || ((first->flags() | second->flags()) & Number::anyNaN)) [[unlikely]]
        return decimalCompareSlow<op>(globalObject, self, other);
    Number a;
    Number b;
    first->lend(a);
    second->lend(b);
    return comparisonResult<op>(compareValues(a, b));
}

// ---- hash() ----

// As for every number: the value modulo 2**61 - 1, where 1/10 is the inverse of 10.
static constexpr uint64_t modulusOfHash = (1ull << 61) - 1;

static ALWAYS_INLINE uint64_t reduceForHash(unsigned __int128 value)
{
    // For a value below the square of the modulus.
    uint64_t result = static_cast<uint64_t>(value & modulusOfHash) + static_cast<uint64_t>(value >> 61);
    return result >= modulusOfHash ? result - modulusOfHash : result;
}

static int64_t hashOfFiniteNumber(const Number& number)
{
    constexpr uint64_t inverseOfTen = 2075258708292324556ull;
    static_assert(static_cast<unsigned __int128>(inverseOfTen) * 10 % modulusOfHash == 1);
    constexpr uint64_t radixRemainder = radix % modulusOfHash;

    uint64_t coefficient = 0;
    for (int64_t i = number.length; i--;)
        coefficient = reduceForHash(static_cast<unsigned __int128>(coefficient) * radixRemainder + number.limbs()[i]);

    uint64_t scale = 1;
    uint64_t square = number.exponent >= 0 ? 10 : inverseOfTen;
    for (uint64_t exponent = number.exponent >= 0 ? number.exponent : -static_cast<uint64_t>(number.exponent); exponent; exponent >>= 1) {
        if (exponent & 1)
            scale = reduceForHash(static_cast<unsigned __int128>(scale) * square);
        square = reduceForHash(static_cast<unsigned __int128>(square) * square);
    }
    int64_t hash = static_cast<int64_t>(reduceForHash(static_cast<unsigned __int128>(coefficient) * scale));
    if (number.isNegative())
        hash = -hash;
    return hash == -1 ? -2 : hash;
}

static JSValue decimalHash(JSGlobalObject* globalObject, JSValue self)
{
    auto* decimal = PyDecimal::from(self);
    if (decimal->cachedHash != -1) [[likely]]
        return makeInt(globalObject, decimal->cachedHash);
    if (decimal->isSpecial()) [[unlikely]] {
        if (decimal->flags() & Number::SignalingNaN) {
            VM& vm = getVM(globalObject);
            auto scope = DECLARE_THROW_SCOPE(vm);
            return raiseTypeError(globalObject, scope, "Cannot hash a signaling NaN value"_s);
        }
        if (decimal->isNaN())
            return objectHash(globalObject, self);
        decimal->cachedHash = decimal->flags() & Number::Negative ? -314159 : 314159;
    } else {
        Number number;
        decimal->lend(number);
        decimal->cachedHash = hashOfFiniteNumber(number);
    }
    return makeInt(globalObject, decimal->cachedHash);
}

// ---- To other numbers ----

// An int has no more bits than this (JSBigInt::maxLengthBits). Asked beforehand, as finding out takes long.
static constexpr int64_t maxBitsOfInt = 1 << 30;

// The coefficient as an int, with the sign. Empty: a MemoryError.
static JSValue intFromCoefficient(JSGlobalObject* globalObject, const Number& number)
{
    if (number.length == 1 && number.limbs()[0] <= static_cast<Limb>(std::numeric_limits<int64_t>::max())) [[likely]] {
        int64_t value = static_cast<int64_t>(number.limbs()[0]);
        return makeInt(globalObject, number.isNegative() ? -value : value);
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<uint64_t, 16> words;
    if (binaryWordsFor(number) > maxBitsOfInt / 64 || !words.tryGrow(binaryWordsFor(number)))
        return raiseMemoryError(globalObject, scope, String());
    size_t count = coefficientToBinary(number, words.mutableSpan().data());
    JSBigInt* result = JSBigInt::tryCreateFromWords(vm, words.span().first(count), number.isNegative());
    if (!result)
        return raiseMemoryError(globalObject, scope, String());
    return normalizeInt(result);
}

// The integer that the number is rounded to, whatever the precision of the context.
static JSValue decimalToInt(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, const Number& number, Rounding rounding)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (number.isSpecial()) [[unlikely]] {
        if (number.isNaN())
            return raiseValueError(globalObject, scope, "cannot convert NaN to integer"_s);
        return raiseOverflowError(globalObject, scope, "cannot convert Infinity to integer"_s);
    }
    Context work = context->context;
    work.rounding = rounding;
    Number integer;
    uint32_t status = 0;
    roundToIntegralValue(integer, number, work, status);
    if (failed(scope, !addStatus(globalObject, state, context, status))) [[unlikely]]
        return { };
    if (integer.exponent > 0) {
        if (integer.hasZeroCoefficient())
            return jsNumber(0);
        if (!shiftCoefficientLeft(integer, integer, integer.exponent, status))
            return raiseMemoryError(globalObject, scope, String());
    }
    RELEASE_AND_RETURN(scope, intFromCoefficient(globalObject, integer));
}

template<Rounding rounding>
static JSValue decimalToIntMethod(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = currentContext(globalObject, state);
    if (failed(scope, context)) [[unlikely]]
        return { };
    Number number;
    PyDecimal::from(self)->lend(number);
    RELEASE_AND_RETURN(scope, decimalToInt(globalObject, state, context, number, rounding));
}

static JSValue decimalRound(JSGlobalObject* globalObject, JSValue self, JSValue digits)
{
    if (!digits)
        return decimalToIntMethod<Rounding::HalfEven>(globalObject, self);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = currentContext(globalObject, state);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInt(digits))
        return raiseTypeError(globalObject, scope, "optional arg must be an integer"_s);
    auto count = tryInt64(intValue(digits));
    if (!count)
        return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
    Number number;
    PyDecimal::from(self)->lend(number);
    Number quantum(0, 1, *count == std::numeric_limits<int64_t>::min() ? std::numeric_limits<int64_t>::max() : -*count);
    Number result;
    uint32_t status = 0;
    quantize(result, number, quantum, context->context, status);
    RELEASE_AND_RETURN(scope, finish(globalObject, state, context, result, status));
}

// False: a ValueError.
static bool decimalToDouble(JSGlobalObject* globalObject, const Number& number, double& result)
{
    double sign = number.isNegative() ? -1.0 : 1.0;
    if (number.isSpecial()) [[unlikely]] {
        if (number.isSignalingNaN()) {
            auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
            raiseValueError(globalObject, scope, "cannot convert signaling NaN to float"_s);
            return false;
        }
        result = std::copysign(number.isNaN() ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity(), sign);
        return true;
    }
    // Two numbers that a float holds exactly, and one operation on them, which is rounded correctly.
    static constexpr double exactPowersOfTen[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    if (number.length == 1 && number.limbs()[0] <= (1ull << 53) && number.exponent >= -22 && number.exponent <= 22) [[likely]] {
        double coefficient = static_cast<double>(number.limbs()[0]);
        result = sign * (number.exponent < 0 ? coefficient / exactPowersOfTen[-number.exponent] : coefficient * exactPowersOfTen[number.exponent]);
        return true;
    }
    Text text;
    if (!toText(text, number, TextScientific | TextUpperCase)) {
        auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    result = parseDouble(std::span { byteCast<Latin1Character>(text.data), text.length }).value();
    return true;
}

static JSValue decimalFloat(JSGlobalObject* globalObject, JSValue self)
{
    Number number;
    PyDecimal::from(self)->lend(number);
    double value;
    if (!decimalToDouble(globalObject, number, value))
        return { };
    return makeFloatExactly(globalObject, value);
}

static JSValue decimalComplex(JSGlobalObject* globalObject, JSValue self)
{
    Number number;
    PyDecimal::from(self)->lend(number);
    double value;
    if (!decimalToDouble(globalObject, number, value))
        return { };
    return PyComplex::create(globalObject, value, 0);
}

static JSValue decimalAsIntegerRatio(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Number number;
    PyDecimal::from(self)->lend(number);
    if (number.isSpecial()) {
        if (number.isNaN())
            return raiseValueError(globalObject, scope, "cannot convert NaN to integer ratio"_s);
        return raiseOverflowError(globalObject, scope, "cannot convert Infinity to integer ratio"_s);
    }
    if (number.hasZeroCoefficient())
        return PyTuple::create(globalObject, { jsNumber(0), jsNumber(1) });
    // More digits than an int can have.
    constexpr int64_t tooManyDigits = maxBitsOfInt / 3;
    if (number.exponent >= tooManyDigits || number.exponent <= -tooManyDigits)
        return raiseMemoryError(globalObject, scope, String());

    auto powerOf = [&](int base, int64_t exponent) { return intPower(globalObject, jsNumber(base), makeInt(globalObject, exponent)); };
    if (number.exponent >= 0) {
        JSValue numerator = intFromCoefficient(globalObject, number);
        RETURN_IF_EXCEPTION(scope, { });
        if (number.exponent) {
            JSValue scale = powerOf(10, number.exponent);
            RETURN_IF_EXCEPTION(scope, { });
            numerator = intMultiply(globalObject, numerator, scale);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return PyTuple::create(globalObject, { numerator, jsNumber(1) });
    }

    // The coefficient over 10**places, in lowest terms. First the tens that both have go.
    int64_t places = -number.exponent;
    Number coefficient;
    uint32_t status = 0;
    if (!coefficient.copy(number, status))
        return raiseMemoryError(globalObject, scope, String());
    if (int64_t zeros = std::min(coefficient.trailingZeros(), places)) {
        shiftCoefficientRight(coefficient, zeros);
        places -= zeros;
    }
    JSValue numerator = intFromCoefficient(globalObject, coefficient);
    RETURN_IF_EXCEPTION(scope, { });
    // What is left of the coefficient is not a multiple of both 2 and 5.
    int64_t twos = places;
    int64_t fives = places;
    if (places && !intIsOdd(numerator)) {
        int64_t shared = 0;
        if (isSmallInt(numerator))
            shared = __builtin_ctzll(static_cast<uint64_t>(smallIntValue(numerator)));
        else {
            JSBigInt* bigInt = numerator.asHeapBigInt();
            unsigned word = 0;
            while (!bigInt->digit(word))
                ++word;
            shared = static_cast<int64_t>(word) * 64 + __builtin_ctzll(bigInt->digit(word));
        }
        shared = std::min(shared, places);
        numerator = intRightShift(globalObject, numerator, makeInt(globalObject, shared));
        RETURN_IF_EXCEPTION(scope, { });
        twos -= shared;
    } else {
        while (fives) {
            JSValue quotient;
            JSValue rest;
            bool divided = intDivMod(globalObject, numerator, jsNumber(5), quotient, rest);
            RETURN_IF_EXCEPTION(scope, { });
            ASSERT_UNUSED(divided, divided);
            if (intSign(rest))
                break;
            numerator = quotient;
            --fives;
        }
    }
    JSValue denominator = powerOf(5, fives);
    RETURN_IF_EXCEPTION(scope, { });
    denominator = intLeftShift(globalObject, denominator, makeInt(globalObject, twos));
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { numerator, denominator });
}

static JSValue decimalAsTuple(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    Number number;
    PyDecimal::from(self)->lend(number);
    JSValue exponent;
    PyTuple* digits;
    if (number.isInfinite()) {
        exponent = makeStr(vm, "F"_s);
        digits = PyTuple::create(globalObject, { jsNumber(0) });
    } else {
        if (number.isNaN())
            exponent = makeStr(vm, number.isQuietNaN() ? "n"_s : "N"_s);
        else {
            exponent = makeInt(globalObject, number.exponent);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (number.digits > std::numeric_limits<int32_t>::max())
            return raiseMemoryError(globalObject, scope, String());
        digits = PyTuple::create(globalObject, static_cast<unsigned>(number.digits));
        unsigned position = static_cast<unsigned>(number.digits);
        for (int64_t i = 0; i < number.length; ++i) {
            Limb limb = number.limbs()[i];
            for (int j = 0; j < digitsPerLimb && position; ++j) {
                digits->initializeAt(vm, --position, jsNumber(static_cast<int>(limb % 10)));
                limb /= 10;
            }
        }
        ensureStillAliveHere(self);
    }
    RELEASE_AND_RETURN(scope, call(globalObject, state.decimalTupleType.get(), jsNumber(number.isNegative() ? 1 : 0), digits, exponent));
}

// ---- Text ----

static JSValue decimalStr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDecimalContext* context = currentContext(globalObject, stateOf(globalObject));
    if (failed(scope, context)) [[unlikely]]
        return { };
    Number number;
    PyDecimal::from(self)->lend(number);
    RELEASE_AND_RETURN(scope, decimalToStr(globalObject, number, scientific(context)));
}

static JSValue decimalRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDecimalContext* context = currentContext(globalObject, stateOf(globalObject));
    if (failed(scope, context)) [[unlikely]]
        return { };
    Number number;
    PyDecimal::from(self)->lend(number);
    RELEASE_AND_RETURN(scope, decimalToStr(globalObject, number, scientific(context), "Decimal('"_s, "')"_s));
}

static JSValue decimalToEngString(JSGlobalObject* globalObject, JSValue self, JSValue contextValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDecimalContext* context = contextArgument(globalObject, stateOf(globalObject), contextValue);
    if (failed(scope, context)) [[unlikely]]
        return { };
    Number number;
    PyDecimal::from(self)->lend(number);
    RELEASE_AND_RETURN(scope, decimalToStr(globalObject, number, TextEngineering | (context->capitals ? TextUpperCase : 0)));
}

// What the arithmetic cannot format, _pydecimal can: `z`, `#`, `_` and whatever else is newer than the format of the library.
static JSValue formatWithPythonDecimal(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, const Number& number, JSValue spec)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!state.pythonDecimal) {
        JSValue module = importModule(globalObject, "_pydecimal"_s);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue type = getAttr(globalObject, module, internString(vm, "Decimal"_s));
        RETURN_IF_EXCEPTION(scope, { });
        state.pythonDecimal.set(vm, &state, type);
    }
    JSString* text = decimalToStr(globalObject, number, scientific(context));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue decimal = call(globalObject, state.pythonDecimal.get(), text);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = callMethod(globalObject, decimal, rt.name(Special::Format), spec, context);
    if (failed(scope, result) && clearPendingExceptionIfMatches(globalObject, rt.ValueError()))
        return raiseValueError(globalObject, scope, "invalid format string"_s);
    return result;
}

using Bytes = Vector<uint8_t, 32>;

// The str in UTF-8. False: an exception.
static bool encodeForFormat(JSGlobalObject* globalObject, JSValue value, Bytes& bytes)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = tryStrValue(value);
    if (!string) {
        raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
        return false;
    }
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (text.containsOnlyASCII()) [[likely]] {
        for (unsigned i = 0; i < text.length(); ++i)
            bytes.append(static_cast<uint8_t>(text[i]));
    } else if (auto encoded = tryEncodeUTF8(text))
        bytes.appendVector(*encoded);
    else {
        codecEncodeUTF8(globalObject, string, "strict"_s);
        EXCEPTION_ASSERT(scope.exception());
        return false;
    }
    return true;
}

// A decimal point or a separator of the C locale, in the encoding of that locale. False: a ValueError.
static bool encodeCharacterOfLocale(JSGlobalObject* globalObject, const char*& text, Bytes& bytes)
{
    size_t length = strlen(text);
    if (!length || (length == 1 && isASCII(static_cast<uint8_t>(text[0]))))
        return true;
    wchar_t characters[2];
    if (mbstowcs(characters, text, 2) != 1) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseValueError(globalObject, scope, "invalid decimal point or unsupported combination of LC_CTYPE and LC_NUMERIC"_s);
        return false;
    }
    bytes.appendVector(*tryEncodeUTF8(stringFromCodePoint(static_cast<char32_t>(characters[0])), true));
    bytes.append(0);
    text = byteCast<char>(bytes.span().data());
    return true;
}

static JSValue decimalFormat(JSGlobalObject* globalObject, JSValue self, JSValue specValue, JSValue override)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(specValue))
        return raiseTypeError(globalObject, scope, "__format__() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, specValue));
    State& state = stateOf(globalObject);
    PyDecimalContext* context = currentContext(globalObject, state);
    RETURN_IF_EXCEPTION(scope, { });
    Number number;
    PyDecimal::from(self)->lend(number);

    Bytes specText;
    if (failed(scope, encodeForFormat(globalObject, specValue, specText)))
        return { };
    if (!specText.isEmpty() && specText.last() == 'N') {
        if (failed(scope, warn(globalObject, rt.DeprecationWarning(), "Format specifier 'N' is deprecated and slated for removal in Python 3.18"_s)))
            return { };
    }
    // A NUL as the character to fill with would end the text. Something else stands in for it: first a character that can be parsed,
    // then a byte that is part of no character.
    bool fillsWithNUL = !specText.isEmpty() && !specText[0];
    if (fillsWithNUL)
        specText[0] = '_';
    specText.append(0);

    FormatSpec spec;
    bool isLocaleAware;
    if (!parseFormatSpec(spec, byteCast<char>(specText.span().data()), context->capitals, isLocaleAware))
        RELEASE_AND_RETURN(scope, formatWithPythonDecimal(globalObject, state, context, number, specValue));
    if (fillsWithNUL) {
        spec.fill[0] = '\xff';
        spec.fill[1] = 0;
    }

    Bytes point;
    Bytes separator;
    Bytes grouping;
    if (override) {
        // Undocumented: what the locale would say.
        auto* dict = PyDict::tryFrom(override);
        if (!dict || !dict->type()->has(TypeFlag::DictSubclass))
            return raiseTypeError(globalObject, scope, "optional argument must be a dict"_s);
        auto take = [&](ASCIILiteral key, Bytes& bytes, const char*& field) {
            JSValue value = dict->get(globalObject, makeStr(vm, key));
            RETURN_IF_EXCEPTION(scope, false);
            if (!value)
                return true;
            if (failed(scope, encodeForFormat(globalObject, value, bytes)))
                return false;
            bytes.append(0);
            field = byteCast<char>(bytes.span().data());
            return true;
        };
        if (!take("decimal_point"_s, point, spec.point) || !take("thousands_sep"_s, separator, spec.separator) || !take("grouping"_s, grouping, spec.grouping))
            return { };
        if (!isValidLocaleData(spec))
            return raiseValueError(globalObject, scope, "invalid override dict"_s);
    } else if (failed(scope, encodeCharacterOfLocale(globalObject, spec.point, point)) || failed(scope, encodeCharacterOfLocale(globalObject, spec.separator, separator)))
        return { };

    Text text;
    uint32_t status = 0;
    bool isFormatted = format(text, number, spec, context->context, status);
    ensureStillAliveHere(self);
    if (!isFormatted) {
        if (status & OutOfMemory)
            return raiseMemoryError(globalObject, scope, String());
        return raiseValueError(globalObject, scope, "format specification exceeds internal limits of _decimal"_s);
    }
    if (fillsWithNUL) {
        for (size_t i = 0; i < text.length; ++i) {
            if (text.data[i] == '\xff')
                text.data[i] = 0;
        }
    }
    if (text.length >= String::MaxLength)
        return raiseMemoryError(globalObject, scope, String());
    return makeStr(vm, String::fromUTF8(std::span { byteCast<char8_t>(text.data), text.length }));
}

// ---- Methods that take a context ----

template<UnaryOperation operation>
static JSValue decimalUnaryMethod(JSGlobalObject* globalObject, JSValue self, JSValue contextValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = contextArgument(globalObject, state, contextValue);
    if (failed(scope, context)) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, applyUnary<operation>(globalObject, state, context, self));
}

template<BinaryOperation operation>
static JSValue decimalBinaryMethod(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue contextValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = contextArgument(globalObject, state, contextValue);
    if (failed(scope, context)) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, applyBinary<operation>(globalObject, state, context, self, other));
}

static JSValue decimalFusedMultiplyAdd(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue third, JSValue contextValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = contextArgument(globalObject, state, contextValue);
    if (failed(scope, context)) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, applyFusedMultiplyAdd(globalObject, state, context, self, other, third));
}

// The context with another rounding, if one is given. False: a TypeError.
static ALWAYS_INLINE bool contextWithRounding(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, JSValue rounding, Context& work)
{
    work = context->context;
    if (!rounding || isNone(rounding)) [[likely]]
        return true;
    return roundingArgument(globalObject, state, rounding, work.rounding);
}

template<UnaryOperation operation>
static JSValue decimalToIntegral(JSGlobalObject* globalObject, JSValue self, JSValue rounding, JSValue contextValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = contextArgument(globalObject, state, contextValue);
    if (failed(scope, context)) [[unlikely]]
        return { };
    Context work;
    if (failed(scope, contextWithRounding(globalObject, state, context, rounding, work))) [[unlikely]]
        return { };
    Number a;
    PyDecimal::from(self)->lend(a);
    Number result;
    uint32_t status = 0;
    operation(result, a, work, status);
    RELEASE_AND_RETURN(scope, finish(globalObject, state, context, result, status));
}

static JSValue decimalQuantize(JSGlobalObject* globalObject, JSValue self, JSValue exponent, JSValue rounding, JSValue contextValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = contextArgument(globalObject, state, contextValue);
    if (failed(scope, context)) [[unlikely]]
        return { };
    Context work;
    if (failed(scope, contextWithRounding(globalObject, state, context, rounding, work))) [[unlikely]]
        return { };
    Number a;
    PyDecimal::from(self)->lend(a);
    Number b;
    if (failed(scope, convertOperand(globalObject, state, context, exponent, b, true) == Conversion::Done)) [[unlikely]]
        return { };
    Number result;
    uint32_t status = 0;
    quantize(result, a, b, work, status);
    RELEASE_AND_RETURN(scope, finish(globalObject, state, context, result, status));
}

static JSValue decimalSameQuantum(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue contextValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = contextArgument(globalObject, state, contextValue);
    if (failed(scope, context)) [[unlikely]]
        return { };
    Number a;
    PyDecimal::from(self)->lend(a);
    Number b;
    if (failed(scope, convertOperand(globalObject, state, context, other, b, true) == Conversion::Done)) [[unlikely]]
        return { };
    return jsBoolean(haveSameQuantum(a, b));
}

static JSValue decimalNumberClass(JSGlobalObject* globalObject, JSValue self, JSValue contextValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDecimalContext* context = contextArgument(globalObject, stateOf(globalObject), contextValue);
    if (failed(scope, context)) [[unlikely]]
        return { };
    Number a;
    PyDecimal::from(self)->lend(a);
    return makeStr(vm, String::fromLatin1(classOf(a, context->context)));
}

template<bool isNormal>
static JSValue decimalIsNormalOrSubnormal(JSGlobalObject* globalObject, JSValue self, JSValue contextValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDecimalContext* context = contextArgument(globalObject, stateOf(globalObject), contextValue);
    if (failed(scope, context)) [[unlikely]]
        return { };
    Number a;
    PyDecimal::from(self)->lend(a);
    return jsBoolean(isNormal ? a.isNormal(context->context) : a.isSubnormal(context->context));
}

// ---- Methods that do not ----

template<uint8_t flags, bool expected>
static JSValue decimalHasFlags(JSGlobalObject*, JSValue self) { return jsBoolean(!!(PyDecimal::from(self)->flags() & flags) == expected); }
static JSValue decimalIsZero(JSGlobalObject*, JSValue self) { return jsBoolean(PyDecimal::from(self)->isZero()); }
static JSValue decimalIsCanonical(JSGlobalObject*, JSValue) { return jsBoolean(true); }
static JSValue decimalItself(JSGlobalObject*, JSValue self) { return self; }
static JSValue decimalDeepCopy(JSGlobalObject*, JSValue self, JSValue) { return self; }

static JSValue decimalAdjusted(JSGlobalObject* globalObject, JSValue self)
{
    auto* decimal = PyDecimal::from(self);
    return makeInt(globalObject, decimal->isSpecial() ? static_cast<int64_t>(0) : decimal->exponent() + decimal->digits() - 1);
}

static JSValue decimalRadix(JSGlobalObject* globalObject, JSValue) { return makeDecimal(globalObject, stateOf(globalObject), Number(0, 10, 0)); }
static JSValue decimalImaginary(JSGlobalObject* globalObject, JSValue) { return makeDecimal(globalObject, stateOf(globalObject), Number(0, 0, 0)); }

template<bool isAbsolute>
static JSValue decimalCopyWithSign(JSGlobalObject* globalObject, JSValue self)
{
    // The sign is all that differs, so the limbs are read where they are.
    Number result;
    PyDecimal::from(self)->lend(result);
    if constexpr (isAbsolute)
        result.setSign(0);
    else
        result.negate();
    JSValue copy = makeDecimal(globalObject, stateOf(globalObject), result);
    ensureStillAliveHere(self);
    return copy;
}

static JSValue decimalReduce(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue text = decimalStr(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { PyDecimal::from(self)->type(), PyTuple::create(globalObject, { text }) });
}

static JSValue decimalSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    // What CPython says: an object with room for four limbs, and memory of its own for a longer coefficient.
    uint32_t length = PyDecimal::from(self)->length();
    return makeInt(globalObject, static_cast<int64_t>(104 + (length > 4 ? length * sizeof(Limb) : 0)));
}

#define PY_DECIMAL_OPERATOR(name, reflectedName, function) \
    PY_SLOT(name, function, "($self, value, /)"), \
    PY_SLOT(reflectedName, decimalReflected<function>, "($self, value, /)")
#define PY_DECIMAL_UNARY(name, operation) PY_METHOD(name, decimalUnaryMethod<operation>, "($self, /, context=None)")
#define PY_DECIMAL_BINARY(name, operation) PY_METHOD(name, decimalBinaryMethod<operation>, "($self, /, other, context=None)")

static constexpr NativeDef decimalMethods[] = {
    PY_NEW(decimalNew, "($type, /, value='0', context=None)"),
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_SLOT("__repr__", decimalRepr, "($self, /)"),
    PY_SLOT("__hash__", decimalHash, "($self, /)"),
    PY_SLOT("__str__", decimalStr, "($self, /)"),
    PY_SLOT("__eq__", decimalCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", decimalCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", decimalCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", decimalCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", decimalCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", decimalCompare<CompareOperator::GtE>, "($self, value, /)"),

    PY_DECIMAL_OPERATOR("__add__", "__radd__", decimalBinaryOperator<add>),
    PY_DECIMAL_OPERATOR("__sub__", "__rsub__", decimalBinaryOperator<subtract>),
    PY_DECIMAL_OPERATOR("__mul__", "__rmul__", decimalBinaryOperator<multiply>),
    PY_DECIMAL_OPERATOR("__mod__", "__rmod__", decimalBinaryOperator<remainder>),
    PY_DECIMAL_OPERATOR("__divmod__", "__rdivmod__", decimalDivMod),
    PY_SLOT("__pow__", decimalPower, "($self, value, mod=None, /)"),
    PY_SLOT("__rpow__", decimalReflectedPower, "($self, value, mod=None, /)"),
    PY_SLOT("__neg__", decimalUnaryOperator<minus>, "($self, /)"),
    PY_SLOT("__pos__", decimalUnaryOperator<plus>, "($self, /)"),
    PY_SLOT("__abs__", decimalUnaryOperator<absolute>, "($self, /)"),
    PY_SLOT("__bool__", decimalBool, "($self, /)"),
    PY_SLOT("__int__", decimalToIntMethod<Rounding::Down>, "($self, /)"),
    PY_SLOT("__float__", decimalFloat, "($self, /)"),
    PY_DECIMAL_OPERATOR("__floordiv__", "__rfloordiv__", decimalBinaryOperator<divideInteger>),
    PY_DECIMAL_OPERATOR("__truediv__", "__rtruediv__", decimalBinaryOperator<divide>),

    PY_DECIMAL_UNARY("exp", exponential),
    PY_DECIMAL_UNARY("ln", naturalLogarithm),
    PY_DECIMAL_UNARY("log10", commonLogarithm),
    PY_DECIMAL_UNARY("next_minus", nextMinus),
    PY_DECIMAL_UNARY("next_plus", nextPlus),
    PY_DECIMAL_UNARY("normalize", reduce),
    PY_METHOD("to_integral", decimalToIntegral<roundToIntegralValue>, "($self, /, rounding=None, context=None)"),
    PY_METHOD("to_integral_exact", decimalToIntegral<roundToIntegralExact>, "($self, /, rounding=None, context=None)"),
    PY_METHOD("to_integral_value", decimalToIntegral<roundToIntegralValue>, "($self, /, rounding=None, context=None)"),
    PY_DECIMAL_UNARY("sqrt", squareRoot),

    PY_DECIMAL_BINARY("compare", compare),
    PY_DECIMAL_BINARY("compare_signal", compareSignal),
    PY_DECIMAL_BINARY("max", maximum),
    PY_DECIMAL_BINARY("max_mag", maximumMagnitude),
    PY_DECIMAL_BINARY("min", minimum),
    PY_DECIMAL_BINARY("min_mag", minimumMagnitude),
    PY_DECIMAL_BINARY("next_toward", nextToward),
    PY_METHOD("quantize", decimalQuantize, "($self, /, exp, rounding=None, context=None)"),
    PY_DECIMAL_BINARY("remainder_near", remainderNear),

    PY_METHOD("fma", decimalFusedMultiplyAdd, "($self, /, other, third, context=None)"),

    PY_METHOD("is_canonical", decimalIsCanonical, "($self, /)"),
    PY_METHOD("is_finite", (decimalHasFlags<Number::anySpecial, false>), "($self, /)"),
    PY_METHOD("is_infinite", (decimalHasFlags<Number::Infinite, true>), "($self, /)"),
    PY_METHOD("is_nan", (decimalHasFlags<Number::anyNaN, true>), "($self, /)"),
    PY_METHOD("is_qnan", (decimalHasFlags<Number::QuietNaN, true>), "($self, /)"),
    PY_METHOD("is_snan", (decimalHasFlags<Number::SignalingNaN, true>), "($self, /)"),
    PY_METHOD("is_signed", (decimalHasFlags<Number::Negative, true>), "($self, /)"),
    PY_METHOD("is_zero", decimalIsZero, "($self, /)"),

    PY_METHOD("is_normal", decimalIsNormalOrSubnormal<true>, "($self, /, context=None)"),
    PY_METHOD("is_subnormal", decimalIsNormalOrSubnormal<false>, "($self, /, context=None)"),

    PY_METHOD("adjusted", decimalAdjusted, "($self, /)"),
    PY_METHOD("canonical", decimalItself, "($self, /)"),
    PY_METHOD("conjugate", decimalItself, "($self, /)"),
    PY_METHOD_WITH_FLAGS("radix", decimalRadix, "($self, /)", ParsedByName),

    PY_METHOD_WITH_FLAGS("copy_abs", decimalCopyWithSign<true>, "($self, /)", ParsedByName),
    PY_METHOD_WITH_FLAGS("copy_negate", decimalCopyWithSign<false>, "($self, /)", ParsedByName),

    PY_DECIMAL_UNARY("logb", logB),
    PY_DECIMAL_UNARY("logical_invert", logicalInvert),
    PY_METHOD("number_class", decimalNumberClass, "($self, /, context=None)"),
    PY_METHOD("to_eng_string", decimalToEngString, "($self, /, context=None)"),

    PY_DECIMAL_BINARY("compare_total", compareTotalInContext),
    PY_DECIMAL_BINARY("compare_total_mag", compareTotalMagnitudeInContext),
    PY_DECIMAL_BINARY("copy_sign", copySignInContext),
    PY_METHOD("same_quantum", decimalSameQuantum, "($self, /, other, context=None)"),

    PY_DECIMAL_BINARY("logical_and", logicalAnd),
    PY_DECIMAL_BINARY("logical_or", logicalOr),
    PY_DECIMAL_BINARY("logical_xor", logicalXor),
    PY_DECIMAL_BINARY("rotate", rotate),
    PY_DECIMAL_BINARY("scaleb", scaleB),
    PY_DECIMAL_BINARY("shift", shift),

    PY_CLASSMETHOD_WITH_FLAGS("from_float", decimalFromFloatMethod, "($type, f, /)", ParsedByName),
    PY_CLASSMETHOD_WITH_FLAGS("from_number", decimalFromNumber, "($type, number, /)", ParsedByName),
    PY_METHOD_WITH_FLAGS("as_tuple", decimalAsTuple, "($self, /)", ParsedByName),
    PY_METHOD_WITH_FLAGS("as_integer_ratio", decimalAsIntegerRatio, "($self, /)", ParsedByName),

    PY_METHOD("__copy__", decimalItself, "($self, /)"),
    PY_METHOD("__deepcopy__", decimalDeepCopy, "($self, memo, /)"),
    PY_METHOD_WITH_FLAGS("__format__", decimalFormat, "($self, format_spec, override=<unrepresentable>, /)", ParsedByName),
    PY_METHOD("__reduce__", decimalReduce, "($self, /)"),
    PY_METHOD_WITH_FLAGS("__round__", decimalRound, "($self, ndigits=<unrepresentable>, /)", ParsedByName),
    PY_METHOD_WITH_FLAGS("__ceil__", decimalToIntMethod<Rounding::Ceiling>, "($self, /)", ParsedByName),
    PY_METHOD_WITH_FLAGS("__floor__", decimalToIntMethod<Rounding::Floor>, "($self, /)", ParsedByName),
    PY_METHOD_WITH_FLAGS("__trunc__", decimalToIntMethod<Rounding::Down>, "($self, /)", ParsedByName),
    PY_METHOD("__complex__", decimalComplex, "($self, /)"),
    PY_METHOD("__sizeof__", decimalSizeOf, "($self, /)"),
};

static constexpr NativeGetSet decimalGetSets[] = {
    { "real"_s, decimalItself },
    { "imag"_s, decimalImaginary },
};

constexpr TypeDef decimalTypeDef {
    "decimal.Decimal"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, decimalMethods, decimalGetSets, { }
};

} // namespace Decimal

JSValue decimalPowerWithModulus(JSGlobalObject* globalObject, JSValue base, JSValue exponent, JSValue modulus)
{
    if (!Decimal::PyDecimal::is(modulus)) [[likely]]
        return runtime(globalObject).notImplemented();
    return Decimal::decimalPower(globalObject, base, exponent, modulus);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
