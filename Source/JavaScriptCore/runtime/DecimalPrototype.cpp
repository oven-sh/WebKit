/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "DecimalPrototype.h"

#include "DecimalObject.h"
#include "DecimalOperator.h"
#include "IntlNumberFormatInlines.h"
#include "JSCInlines.h"

namespace JSC {

static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncIsNaN);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncIsFinite);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncExponent);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncMantissa);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncAbs);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncNegate);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncAdd);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncSubtract);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncMultiply);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncDivide);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncRemainder);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncCompare);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncEquals);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncNotEquals);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncLessThan);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncLessThanOrEqual);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncGreaterThan);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncGreaterThanOrEqual);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncRound);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncScale10);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncToString);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncToLocaleString);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncToExponential);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncToFixed);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncToPrecision);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncToJSON);
static JSC_DECLARE_HOST_FUNCTION(decimalProtoFuncValueOf);

}

#include "DecimalPrototype.lut.h"

namespace JSC {

const ClassInfo DecimalPrototype::s_info = { "Decimal"_s, &Base::s_info, &decimalPrototypeTable, nullptr, CREATE_METHOD_TABLE(DecimalPrototype) };

/* Source for DecimalPrototype.lut.h
@begin decimalPrototypeTable
  isNaN                decimalProtoFuncIsNaN                DontEnum|Function 0
  isFinite             decimalProtoFuncIsFinite             DontEnum|Function 0
  exponent             decimalProtoFuncExponent             DontEnum|Function 0
  mantissa             decimalProtoFuncMantissa             DontEnum|Function 0
  abs                  decimalProtoFuncAbs                  DontEnum|Function 0
  negate               decimalProtoFuncNegate               DontEnum|Function 0
  add                  decimalProtoFuncAdd                  DontEnum|Function 1
  subtract             decimalProtoFuncSubtract             DontEnum|Function 1
  multiply             decimalProtoFuncMultiply             DontEnum|Function 1
  divide               decimalProtoFuncDivide               DontEnum|Function 1
  remainder            decimalProtoFuncRemainder            DontEnum|Function 1
  compare              decimalProtoFuncCompare              DontEnum|Function 1
  equals               decimalProtoFuncEquals               DontEnum|Function 1
  notEquals            decimalProtoFuncNotEquals            DontEnum|Function 1
  lessThan             decimalProtoFuncLessThan             DontEnum|Function 1
  lessThanOrEqual      decimalProtoFuncLessThanOrEqual      DontEnum|Function 1
  greaterThan          decimalProtoFuncGreaterThan          DontEnum|Function 1
  greaterThanOrEqual   decimalProtoFuncGreaterThanOrEqual   DontEnum|Function 1
  round                decimalProtoFuncRound                DontEnum|Function 1
  scale10              decimalProtoFuncScale10              DontEnum|Function 1
  toString             decimalProtoFuncToString             DontEnum|Function 0
  toLocaleString       decimalProtoFuncToLocaleString       DontEnum|Function 0
  toExponential        decimalProtoFuncToExponential        DontEnum|Function 0
  toFixed              decimalProtoFuncToFixed              DontEnum|Function 0
  toPrecision          decimalProtoFuncToPrecision          DontEnum|Function 0
  toJSON               decimalProtoFuncToJSON               DontEnum|Function 0
  valueOf              decimalProtoFuncValueOf              DontEnum|Function 1
@end
*/

DecimalPrototype::DecimalPrototype(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

DecimalPrototype* DecimalPrototype::create(VM& vm, Structure* structure)
{
    auto* prototype = new (NotNull, allocateCell<DecimalPrototype>(vm)) DecimalPrototype(vm, structure);
    prototype->finishCreation(vm);
    return prototype;
}

Structure* DecimalPrototype::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

void DecimalPrototype::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
}

// RequireInternalSlot(value, [[DecimalData]]). Null if it threw.
static DecimalObject* requireDecimal(JSGlobalObject* globalObject, JSValue value, ASCIILiteral message)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* decimal = dynamicDowncast<DecimalObject>(value);
    if (!decimal) [[unlikely]]
        throwTypeError(globalObject, scope, message);
    return decimal;
}

static constexpr ASCIILiteral receiverMessage = "Decimal.prototype method called on an object that is not a Decimal"_s;
static constexpr ASCIILiteral operandMessage = "The operand of a Decimal operation must be a Decimal"_s;

static EncodedJSValue newDecimal(JSGlobalObject* globalObject, const Decimal128& value)
{
    return JSValue::encode(DecimalObject::create(globalObject->vm(), globalObject->decimalStructure(), value));
}

// Answers through `function` of the receiver's value.
template<typename Function>
static ALWAYS_INLINE EncodedJSValue unary(JSGlobalObject* globalObject, CallFrame* callFrame, const Function& function)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, function(decimal->value()));
}

template<typename Function>
static ALWAYS_INLINE EncodedJSValue binary(JSGlobalObject* globalObject, CallFrame* callFrame, const Function& function)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
    RETURN_IF_EXCEPTION(scope, { });
    auto* operand = requireDecimal(globalObject, callFrame->argument(0), operandMessage);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, function(decimal->value(), operand->value()));
}

static ALWAYS_INLINE EncodedJSValue arithmetic(JSGlobalObject* globalObject, CallFrame* callFrame, Decimal128 (*operation)(const Decimal128&, const Decimal128&))
{
    return binary(globalObject, callFrame, [&](const Decimal128& x, const Decimal128& y) {
        return newDecimal(globalObject, operation(x, y));
    });
}

template<typename Predicate>
static ALWAYS_INLINE EncodedJSValue comparison(JSGlobalObject* globalObject, CallFrame* callFrame, const Predicate& predicate)
{
    return binary(globalObject, callFrame, [&](const Decimal128& x, const Decimal128& y) {
        return JSValue::encode(jsBoolean(predicate(Decimal128::compare(x, y))));
    });
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.isnan
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncIsNaN, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return unary(globalObject, callFrame, [](const Decimal128& value) {
        return JSValue::encode(jsBoolean(value.isNaN()));
    });
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.isfinite
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncIsFinite, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return unary(globalObject, callFrame, [](const Decimal128& value) {
        return JSValue::encode(jsBoolean(value.isFinite()));
    });
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.exponent
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncExponent, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return unary(globalObject, callFrame, [](const Decimal128& value) {
        if (value.isNaN())
            return JSValue::encode(jsNaN());
        if (value.isInfinity())
            return JSValue::encode(jsNumber(std::numeric_limits<double>::infinity()));
        if (value.isZero())
            return JSValue::encode(jsNumber(-std::numeric_limits<double>::infinity()));
        return JSValue::encode(jsNumber(value.scientificExponent()));
    });
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.mantissa
//
// The steps have the Numbers NaN and Infinity in place of the Decimal values, and do not say what e is. It is the exponent.
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncMantissa, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return unary(globalObject, callFrame, [&](const Decimal128& value) {
        if (value.isInfinity())
            return newDecimal(globalObject, Decimal128::infinity(false));
        if (value.isNaN() || value.isZero())
            return newDecimal(globalObject, value);
        return newDecimal(globalObject, value.significand());
    });
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.abs
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncAbs, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return unary(globalObject, callFrame, [&](const Decimal128& value) {
        return newDecimal(globalObject, value.abs());
    });
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.negate
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncNegate, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return unary(globalObject, callFrame, [&](const Decimal128& value) {
        return newDecimal(globalObject, value.negate());
    });
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncAdd, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return arithmetic(globalObject, callFrame, Decimal128::add);
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncSubtract, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return arithmetic(globalObject, callFrame, Decimal128::subtract);
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncMultiply, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return arithmetic(globalObject, callFrame, Decimal128::multiply);
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncDivide, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return arithmetic(globalObject, callFrame, Decimal128::divide);
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncRemainder, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return arithmetic(globalObject, callFrame, Decimal128::remainder);
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.compare
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncCompare, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return binary(globalObject, callFrame, [](const Decimal128& x, const Decimal128& y) {
        auto order = Decimal128::compare(x, y);
        if (order == std::partial_ordering::unordered)
            return JSValue::encode(jsNaN());
        return JSValue::encode(jsNumber(order < 0 ? -1 : order > 0 ? 1 : 0));
    });
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncEquals, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return comparison(globalObject, callFrame, [](std::partial_ordering order) { return order == 0; });
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.notequals
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncNotEquals, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    // Unlike != of Numbers this is false of a NaN.
    return comparison(globalObject, callFrame, [](std::partial_ordering order) { return order < 0 || order > 0; });
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncLessThan, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return comparison(globalObject, callFrame, [](std::partial_ordering order) { return order < 0; });
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncLessThanOrEqual, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return comparison(globalObject, callFrame, [](std::partial_ordering order) { return order <= 0; });
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncGreaterThan, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return comparison(globalObject, callFrame, [](std::partial_ordering order) { return order > 0; });
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncGreaterThanOrEqual, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return comparison(globalObject, callFrame, [](std::partial_ordering order) { return order >= 0; });
}

// https://tc39.es/proposal-decimal/#dfn-decimal-rounding-mode
static std::optional<Decimal128::RoundingMode> roundingModeFromName(const String& name)
{
    if (name == "ceil"_s)
        return Decimal128::RoundingMode::Ceil;
    if (name == "floor"_s)
        return Decimal128::RoundingMode::Floor;
    if (name == "trunc"_s)
        return Decimal128::RoundingMode::Trunc;
    if (name == "halfExpand"_s)
        return Decimal128::RoundingMode::HalfExpand;
    if (name == "halfEven"_s)
        return Decimal128::RoundingMode::HalfEven;
    return std::nullopt;
}

static Decimal128::RoundingMode toRoundingMode(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isUndefined())
        return Decimal128::RoundingMode::HalfEven;
    if (!value.isString()) [[unlikely]] {
        throwTypeError(globalObject, scope, "roundingMode must be a string"_s);
        return { };
    }
    String name = asString(value)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto mode = roundingModeFromName(name)) [[likely]]
        return *mode;
    throwRangeError(globalObject, scope, "roundingMode must be either \"ceil\", \"floor\", \"trunc\", \"halfExpand\", or \"halfEven\""_s);
    return { };
}

// Null with no exception if it is undefined.
static JSObject* toOptions(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value.isUndefined())
        return nullptr;
    if (!value.isObject()) [[unlikely]] {
        throwTypeError(globalObject, scope, "options must be an object or undefined"_s);
        return nullptr;
    }
    return asObject(value);
}

static double toIntegerOption(JSGlobalObject* globalObject, JSValue value, double minimum, ASCIILiteral typeMessage, ASCIILiteral rangeMessage)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isNumber()) [[unlikely]] {
        throwTypeError(globalObject, scope, typeMessage);
        return 0;
    }
    double number = value.asNumber();
    if (!isInteger(number) || number < minimum) [[unlikely]] {
        throwRangeError(globalObject, scope, rangeMessage);
        return 0;
    }
    return number;
}

static EncodedJSValue stringOrOutOfMemory(JSGlobalObject* globalObject, String&& string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (string.isNull()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    return JSValue::encode(jsString(vm, WTF::move(string)));
}

// https://tc39.es/proposal-decimal/#sec-decimal-method-round
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncRound, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
    RETURN_IF_EXCEPTION(scope, { });
    // A negative count rounds to a power of ten above 1.
    double digits = toIntegerOption(globalObject, callFrame->argument(0), -std::numeric_limits<double>::infinity(), "numFractionalDigits must be a number"_s, "numFractionalDigits must be an integer"_s);
    RETURN_IF_EXCEPTION(scope, { });
    auto mode = toRoundingMode(globalObject, callFrame->argument(1));
    RETURN_IF_EXCEPTION(scope, { });
    return newDecimal(globalObject, decimal->value().round(digits, mode));
}

// https://tc39.es/proposal-decimal/#sec-decimal-method-scale10
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncScale10, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue argument = callFrame->argument(0);
    if (!argument.isNumber()) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "The power must be a number"_s);
    double power = argument.asNumber();
    if (std::isfinite(power) && !isInteger(power)) [[unlikely]]
        return throwVMRangeError(globalObject, scope, "The power must be an integer"_s);
    return newDecimal(globalObject, decimal->value().scale10(power));
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.tostring
//
// Three errors of the steps of the operations below it are not followed. The sign of an integer is kept. A fraction is written whole:
// the steps cut it to the number of digits of the integer part. The exponential notation is that of the significand and the exponent:
// the steps have the exponent with the wrong sign and scale the mantissa the wrong way (12300e-2 for 123), even out of the domain.
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncToString, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
    RETURN_IF_EXCEPTION(scope, { });
    toOptions(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, decimal->value().toString()));
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.tolocalestring
//
// The steps read [[Decimal128Data]], a slot that nothing has, and do not convert its value. It is [[DecimalData]], as everywhere else.
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncToLocaleString, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue locales = callFrame->argument(0);
    JSValue options = callFrame->argument(1);
    IntlNumberFormat* numberFormat = nullptr;
    if (locales.isUndefined() && options.isUndefined())
        numberFormat = globalObject->defaultNumberFormat();
    else if (locales.isString() && options.isUndefined())
        numberFormat = globalObject->cachedToLocaleStringNumberFormat(asString(locales));
    else {
        numberFormat = IntlNumberFormat::create(vm, globalObject->numberFormatStructure());
        numberFormat->initializeNumberFormat(globalObject, locales, options);
    }
    RETURN_IF_EXCEPTION(scope, { });

    auto value = toIntlMathematicalValue(globalObject, decimal);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto number = value.tryGetDouble())
        RELEASE_AND_RETURN(scope, JSValue::encode(numberFormat->format(globalObject, *number)));
    RELEASE_AND_RETURN(scope, JSValue::encode(numberFormat->format(globalObject, WTF::move(value))));
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.toexponential
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncToExponential, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
    RETURN_IF_EXCEPTION(scope, { });
    toOptions(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, decimal->value().toExponentialString()));
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.tofixed
//
// The last steps cannot be followed. They put the point behind the scaled digits, append what the rounding dropped, and strip the
// trailing zeros that make up the "specified number of digits after the decimal point". This writes the rounded value with that
// number of digits. It also rounds a negative value in the direction that the rounding modes are defined to have.
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncToFixed, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
    RETURN_IF_EXCEPTION(scope, { });

    auto* options = toOptions(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue digitsValue = jsNumber(0);
    JSValue roundingModeValue = jsUndefined();
    if (options) {
        digitsValue = options->get(globalObject, Identifier::fromString(vm, "digits"_s));
        RETURN_IF_EXCEPTION(scope, { });
        roundingModeValue = options->get(globalObject, vm.propertyNames->roundingMode);
        RETURN_IF_EXCEPTION(scope, { });
    }
    double digits = toIntegerOption(globalObject, digitsValue, 0, "digits must be a number"_s, "digits must be a non-negative integer"_s);
    RETURN_IF_EXCEPTION(scope, { });
    auto mode = toRoundingMode(globalObject, roundingModeValue);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, stringOrOutOfMemory(globalObject, decimal->value().toFixed(digits, mode)));
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.toprecision
//
// The steps read the precision and never use it: it stays 0, which is a RangeError. This uses it. Without options it is 0 and the
// RangeError is what the steps have, although the description of the method says "equivalent to toString". See Decimal128::toPrecision().
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncToPrecision, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
    RETURN_IF_EXCEPTION(scope, { });

    auto* options = toOptions(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    if (!options)
        return throwVMRangeError(globalObject, scope, "precision must be a positive integer"_s);
    JSValue precisionValue = options->get(globalObject, Identifier::fromString(vm, "precision"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (precisionValue.isUndefined())
        return JSValue::encode(jsString(vm, decimal->value().toDecimalString()));

    if (!precisionValue.isNumber()) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "precision must be a number"_s);
    // -Infinity is not in this step. It throws below, behind the second Get.
    if (double number = precisionValue.asNumber(); std::isnan(number) || number == std::numeric_limits<double>::infinity()) [[unlikely]]
        return throwVMRangeError(globalObject, scope, "precision must be a positive integer"_s);
    JSValue roundingModeValue = options->get(globalObject, vm.propertyNames->roundingMode);
    RETURN_IF_EXCEPTION(scope, { });
    double precision = toIntegerOption(globalObject, precisionValue, 1, "precision must be a number"_s, "precision must be a positive integer"_s);
    RETURN_IF_EXCEPTION(scope, { });
    // The steps do not check the rounding mode, and round by anything that is not one as by undefined.
    auto mode = Decimal128::RoundingMode::HalfEven;
    if (roundingModeValue.isString()) {
        String name = asString(roundingModeValue)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        mode = roundingModeFromName(name).value_or(mode);
    }
    RELEASE_AND_RETURN(scope, stringOrOutOfMemory(globalObject, decimal->value().toPrecision(precision, mode)));
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.tojson
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncToJSON, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "Decimal cannot be serialized as JSON"_s);
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.valueof
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncValueOf, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "Decimal.prototype.valueOf must not be called. To compare Decimals, use compare(), equals() or lessThan()"_s);
}

// Not in the proposal, where operators throw. See --useDecimalOperators and BytecodeGenerator::emitDecimalOperator().
JSC_DEFINE_HOST_FUNCTION(decimalOperator, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* left = DecimalObject::tryCast(callFrame->argument(1));
    auto* right = DecimalObject::tryCast(callFrame->argument(2));
    if (!left || !right)
        return JSValue::encode(jsUndefined());

    const Decimal128& x = left->value();
    const Decimal128& y = right->value();
    auto boolean = [](bool value) { return JSValue::encode(jsBoolean(value)); };
    switch (static_cast<DecimalOperator>(callFrame->argument(0).asInt32())) {
    case DecimalOperator::Add:
        return newDecimal(globalObject, Decimal128::add(x, y));
    case DecimalOperator::Subtract:
        return newDecimal(globalObject, Decimal128::subtract(x, y));
    case DecimalOperator::Multiply:
        return newDecimal(globalObject, Decimal128::multiply(x, y));
    case DecimalOperator::Divide:
        return newDecimal(globalObject, Decimal128::divide(x, y));
    case DecimalOperator::Remainder:
        return newDecimal(globalObject, Decimal128::remainder(x, y));
    case DecimalOperator::Equal:
        return boolean(Decimal128::compare(x, y) == 0);
    case DecimalOperator::NotEqual:
        // Unlike notEquals() this is the opposite of ==, as for every other type.
        return boolean(Decimal128::compare(x, y) != 0);
    case DecimalOperator::Less:
        return boolean(Decimal128::compare(x, y) < 0);
    case DecimalOperator::LessOrEqual:
        return boolean(Decimal128::compare(x, y) <= 0);
    case DecimalOperator::Greater:
        return boolean(Decimal128::compare(x, y) > 0);
    case DecimalOperator::GreaterOrEqual:
        return boolean(Decimal128::compare(x, y) >= 0);
    case DecimalOperator::Negate:
        return newDecimal(globalObject, x.negate());
    case DecimalOperator::Increment:
        return newDecimal(globalObject, Decimal128::add(x, Decimal128::one()));
    case DecimalOperator::Decrement:
        return newDecimal(globalObject, Decimal128::subtract(x, Decimal128::one()));
    case DecimalOperator::ToNumeric:
        return JSValue::encode(left);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace JSC
