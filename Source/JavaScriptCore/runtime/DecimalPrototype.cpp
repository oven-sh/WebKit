/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "DecimalPrototype.h"

#include "DecimalObject.h"
#include "DecimalOperator.h"
#include "DecimalStringCache.h"
#include "IntlNumberFormatInlines.h"
#include "JSCInlines.h"

#if ENABLE(JIT)
#include "CCallHelpers.h"
#include "DOMJITCallDOMGetterSnippet.h"
#include "DOMJITGetterSetter.h"
#include "DOMJITSignature.h"
#include "FrameTracers.h"
#include "JITOperations.h"
#include "SnippetParams.h"
#endif

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
  isNaN                decimalProtoFuncIsNaN                DontEnum|Function 0 DecimalPrototypeIntrinsic
  isFinite             decimalProtoFuncIsFinite             DontEnum|Function 0 DecimalPrototypeIntrinsic
  exponent             decimalProtoFuncExponent             DontEnum|Function 0 DecimalPrototypeIntrinsic
  mantissa             decimalProtoFuncMantissa             DontEnum|Function 0 DecimalPrototypeIntrinsic
  abs                  decimalProtoFuncAbs                  DontEnum|Function 0 DecimalPrototypeIntrinsic
  negate               decimalProtoFuncNegate               DontEnum|Function 0 DecimalPrototypeIntrinsic
  add                  decimalProtoFuncAdd                  DontEnum|Function 1 DecimalPrototypeIntrinsic
  subtract             decimalProtoFuncSubtract             DontEnum|Function 1 DecimalPrototypeIntrinsic
  multiply             decimalProtoFuncMultiply             DontEnum|Function 1 DecimalPrototypeIntrinsic
  divide               decimalProtoFuncDivide               DontEnum|Function 1 DecimalPrototypeIntrinsic
  remainder            decimalProtoFuncRemainder            DontEnum|Function 1 DecimalPrototypeIntrinsic
  compare              decimalProtoFuncCompare              DontEnum|Function 1 DecimalPrototypeIntrinsic
  equals               decimalProtoFuncEquals               DontEnum|Function 1 DecimalPrototypeIntrinsic
  notEquals            decimalProtoFuncNotEquals            DontEnum|Function 1 DecimalPrototypeIntrinsic
  lessThan             decimalProtoFuncLessThan             DontEnum|Function 1 DecimalPrototypeIntrinsic
  lessThanOrEqual      decimalProtoFuncLessThanOrEqual      DontEnum|Function 1 DecimalPrototypeIntrinsic
  greaterThan          decimalProtoFuncGreaterThan          DontEnum|Function 1 DecimalPrototypeIntrinsic
  greaterThanOrEqual   decimalProtoFuncGreaterThanOrEqual   DontEnum|Function 1 DecimalPrototypeIntrinsic
  round                decimalProtoFuncRound                DontEnum|Function 1 DecimalPrototypeIntrinsic
  scale10              decimalProtoFuncScale10              DontEnum|Function 1
  toString             decimalProtoFuncToString             DontEnum|Function 0 DecimalPrototypeIntrinsic
  toLocaleString       decimalProtoFuncToLocaleString       DontEnum|Function 0
  toExponential        decimalProtoFuncToExponential        DontEnum|Function 0 DecimalPrototypeIntrinsic
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

static JSValue exponentOf(const Decimal128& value)
{
    if (value.isNaN())
        return jsNaN();
    if (value.isInfinity())
        return jsNumber(std::numeric_limits<double>::infinity());
    if (value.isZero())
        return jsNumber(-std::numeric_limits<double>::infinity());
    return jsNumber(value.scientificExponent());
}

static Decimal128 mantissaOf(const Decimal128& value)
{
    if (value.isInfinity())
        return Decimal128::infinity(false);
    if (value.isNaN() || value.isZero())
        return value;
    return value.significand();
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
        return JSValue::encode(exponentOf(value));
    });
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.mantissa
//
// The steps have the Numbers NaN and Infinity in place of the Decimal values, and do not say what e is. It is the exponent.
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncMantissa, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return unary(globalObject, callFrame, [&](const Decimal128& value) {
        return newDecimal(globalObject, mantissaOf(value));
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

// Unlike != of Numbers this is false of a NaN.
static bool isLessOrGreater(std::partial_ordering order)
{
    return order < 0 || order > 0;
}

static JSValue numberOfOrder(std::partial_ordering order)
{
    if (order == std::partial_ordering::unordered)
        return jsNaN();
    return jsNumber(order < 0 ? -1 : order > 0 ? 1 : 0);
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.compare
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncCompare, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return binary(globalObject, callFrame, [](const Decimal128& x, const Decimal128& y) {
        return JSValue::encode(numberOfOrder(Decimal128::compare(x, y)));
    });
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncEquals, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return comparison(globalObject, callFrame, [](std::partial_ordering order) { return order == 0; });
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.notequals
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncNotEquals, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return comparison(globalObject, callFrame, isLessOrGreater);
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
// Null if it names none, or threw.
static ALWAYS_INLINE std::optional<Decimal128::RoundingMode> roundingModeFromName(JSGlobalObject* globalObject, JSString* string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    using Mode = Decimal128::RoundingMode;
    auto find = [&](const auto& isNamed) -> std::optional<Mode> {
        if (isNamed(vm.propertyNames->floor))
            return Mode::Floor;
        if (isNamed(vm.propertyNames->halfExpand))
            return Mode::HalfExpand;
        if (isNamed(vm.propertyNames->ceil))
            return Mode::Ceil;
        if (isNamed(vm.propertyNames->trunc))
            return Mode::Trunc;
        if (isNamed(vm.propertyNames->halfEven))
            return Mode::HalfEven;
        return std::nullopt;
    };
    // The string of a literal is that of the identifier.
    auto* impl = string->tryGetValueImpl();
    if (auto mode = find([&](const Identifier& name) ALWAYS_INLINE_LAMBDA { return impl == name.impl(); })) [[likely]]
        return mode;
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return find([&](const Identifier& name) { return view == name.string(); });
}

static ALWAYS_INLINE Decimal128::RoundingMode toRoundingMode(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isUndefined())
        return Decimal128::RoundingMode::HalfEven;
    if (!value.isString()) [[unlikely]] {
        throwTypeError(globalObject, scope, "roundingMode must be a string"_s);
        return { };
    }
    auto mode = roundingModeFromName(globalObject, asString(value));
    RETURN_IF_EXCEPTION(scope, { });
    if (mode) [[likely]]
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

// Get(options, name), without the general lookup where every object on the way is ordinary and the property is a value.
static ALWAYS_INLINE JSValue getOption(JSGlobalObject* globalObject, JSObject* options, const Identifier& name)
{
    VM& vm = globalObject->vm();
    JSObject* object = options;
    for (unsigned i = 0; i < 4; ++i) {
        Structure* structure = object->structure();
        if (structure->typeInfo().overridesGetOwnPropertySlot() || structure->typeInfo().overridesGetPrototype() || structure->typeInfo().hasStaticPropertyTable())
            break;
        unsigned attributes;
        PropertyOffset offset = structure->get(vm, name, attributes);
        if (isValidOffset(offset)) {
            if (attributes & (PropertyAttribute::AccessorOrCustomAccessorOrValue))
                break;
            return object->getDirect(offset);
        }
        JSValue prototype = structure->storedPrototype(object);
        if (prototype.isNull())
            return jsUndefined();
        object = asObject(prototype);
    }
    return options->get(globalObject, name);
}

static double toIntegerOption(JSGlobalObject* globalObject, JSValue value, double minimum, ASCIILiteral typeMessage, ASCIILiteral rangeMessage)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value.isInt32() && value.asInt32() >= minimum) [[likely]]
        return value.asInt32();
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

// A negative count rounds to a power of ten above 1.
static ALWAYS_INLINE double digitsOfRound(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isInt32()) [[likely]]
        return value.asInt32();
    return toIntegerOption(globalObject, value, -std::numeric_limits<double>::infinity(), "numFractionalDigits must be a number"_s, "numFractionalDigits must be an integer"_s);
}

// https://tc39.es/proposal-decimal/#sec-decimal-method-round
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncRound, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
    RETURN_IF_EXCEPTION(scope, { });
    double digits = digitsOfRound(globalObject, callFrame->argument(0));
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

JSString* DecimalStringCache::add(VM& vm, Set& set, const Decimal128& value, uint32_t format)
{
    auto mode = static_cast<Decimal128::RoundingMode>(format >> 2 & 7);
    unsigned digits = format >> 5;
    String text;
    switch (static_cast<Notation>(format & 3)) {
    case Notation::Standard:
        text = value.toString();
        break;
    case Notation::Exponential:
        text = value.toExponentialString();
        break;
    case Notation::Fixed:
        text = value.toFixed(digits, mode);
        break;
    case Notation::Precision:
        text = value.toPrecision(digits, mode);
        break;
    }
    ASSERT(!text.isNull());
    JSString* string = jsString(vm, WTF::move(text));
    set[1] = set[0];
    set[0] = { value, string, format };
    return string;
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
    return JSValue::encode(vm.decimalStringCache().toString(vm, decimal->value()));
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
    return JSValue::encode(vm.decimalStringCache().toExponential(vm, decimal->value()));
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
        digitsValue = getOption(globalObject, options, vm.propertyNames->digits);
        RETURN_IF_EXCEPTION(scope, { });
        roundingModeValue = getOption(globalObject, options, vm.propertyNames->roundingMode);
        RETURN_IF_EXCEPTION(scope, { });
    }
    double digits = toIntegerOption(globalObject, digitsValue, 0, "digits must be a number"_s, "digits must be a non-negative integer"_s);
    RETURN_IF_EXCEPTION(scope, { });
    auto mode = toRoundingMode(globalObject, roundingModeValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (digits <= DecimalStringCache::maximumDigits && decimal->value().isShort()) [[likely]]
        return JSValue::encode(vm.decimalStringCache().toFixed(vm, decimal->value(), static_cast<unsigned>(digits), mode));
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
    JSValue precisionValue = getOption(globalObject, options, vm.propertyNames->precision);
    RETURN_IF_EXCEPTION(scope, { });
    if (precisionValue.isUndefined())
        return JSValue::encode(jsString(vm, decimal->value().toDecimalString()));

    if (!precisionValue.isNumber()) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "precision must be a number"_s);
    // -Infinity is not in this step. It throws below, behind the second Get.
    if (double number = precisionValue.asNumber(); std::isnan(number) || number == std::numeric_limits<double>::infinity()) [[unlikely]]
        return throwVMRangeError(globalObject, scope, "precision must be a positive integer"_s);
    JSValue roundingModeValue = getOption(globalObject, options, vm.propertyNames->roundingMode);
    RETURN_IF_EXCEPTION(scope, { });
    double precision = toIntegerOption(globalObject, precisionValue, 1, "precision must be a number"_s, "precision must be a positive integer"_s);
    RETURN_IF_EXCEPTION(scope, { });
    // The steps do not check the rounding mode, and round by anything that is not one as by undefined.
    auto mode = Decimal128::RoundingMode::HalfEven;
    if (roundingModeValue.isString()) {
        auto named = roundingModeFromName(globalObject, asString(roundingModeValue));
        RETURN_IF_EXCEPTION(scope, { });
        mode = named.value_or(mode);
    }
    if (precision <= DecimalStringCache::maximumDigits && decimal->value().isShort()) [[likely]]
        return JSValue::encode(vm.decimalStringCache().toPrecision(vm, decimal->value(), static_cast<unsigned>(precision), mode));
    RELEASE_AND_RETURN(scope, stringOrOutOfMemory(globalObject, decimal->value().toPrecision(precision, mode)));
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.tojson
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncToJSON, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (Options::useDecimalToJSON()) {
        // Not in the proposal. A String and not a number, whose digits JSON.parse() and most other parsers would round to those of a double.
        auto* decimal = requireDecimal(globalObject, callFrame->thisValue(), receiverMessage);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(vm.decimalStringCache().toString(vm, decimal->value()));
    }
    return throwVMTypeError(globalObject, scope, "Decimal cannot be serialized as JSON"_s);
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.valueof
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncValueOf, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "Decimal.prototype.valueOf must not be called. To compare Decimals, use compare(), equals() or lessThan()"_s);
}

#if ENABLE(JIT)

// The methods without arguments that optimized code calls directly, with a check of the receiver that it can hoist.
#define FOR_EACH_DIRECT_METHOD(macro) \
    macro(Mantissa, DecimalObject::create(vm, globalObject->decimalStructure(), mantissaOf(value)), SpecObjectOther) \
    macro(Abs, DecimalObject::create(vm, globalObject->decimalStructure(), value.abs()), SpecObjectOther) \
    macro(Negate, DecimalObject::create(vm, globalObject->decimalStructure(), value.negate()), SpecObjectOther) \
    macro(ToString, vm.decimalStringCache().toString(vm, value), SpecString) \
    macro(ToExponential, vm.decimalStringCache().toExponential(vm, value), SpecString)

#define DEFINE_DIRECT_METHOD(name, result, type) \
JSC_DECLARE_JIT_OPERATION(operationDecimal##name, EncodedJSValue, (JSGlobalObject*, DecimalObject*)); \
JSC_DEFINE_JIT_OPERATION(operationDecimal##name, EncodedJSValue, (JSGlobalObject* globalObject, DecimalObject* decimal)) \
{ \
    VM& vm = globalObject->vm(); \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    const Decimal128& value = decimal->value(); \
    OPERATION_RETURN(scope, JSValue::encode(result)); \
}

FOR_EACH_DIRECT_METHOD(DEFINE_DIRECT_METHOD)

// Makes an object, and reads and writes nothing else. Not pure: two calls make two objects.
static constexpr DOMJIT::Effect effectOfAllocation { DOMJIT::HeapRange::none(), DOMJIT::HeapRange::none(), DOMJIT::HeapRange::top(), { DFG::HeapObjectCount, DFG::InvalidAbstractHeap, DFG::InvalidAbstractHeap, DFG::InvalidAbstractHeap }, { DFG::HeapObjectCount, DFG::InvalidAbstractHeap, DFG::InvalidAbstractHeap, DFG::InvalidAbstractHeap } };

#define DEFINE_METHOD_SIGNATURE(name, result, type) \
    static const DOMJIT::Signature signatureOfDecimal##name(operationDecimal##name, DecimalObject::info(), effectOfAllocation, type);
FOR_EACH_DIRECT_METHOD(DEFINE_METHOD_SIGNATURE)

// Answers the exponent of the representation in `result`.
static void loadExponent(CCallHelpers& jit, GPRReg base, GPRReg result)
{
    constexpr int bitsAbove = 64 - Decimal128::shiftOfExponent - Decimal128::bitsOfExponent;
    jit.load64(CCallHelpers::Address(base, DecimalObject::offsetOfValue() + Decimal128::offsetOfUpper()), result);
    jit.lshift64(CCallHelpers::TrustedImm32(bitsAbove), result);
    jit.rshift64(CCallHelpers::TrustedImm32(64 - Decimal128::bitsOfExponent), result);
}

// Of the 16 bits that hold the exponent, whether those in the mask are the expected ones.
static Ref<DOMJIT::CallDOMGetterSnippet> testOfExponent(int32_t mask, CCallHelpers::RelationalCondition condition, int32_t expected)
{
    Ref<DOMJIT::CallDOMGetterSnippet> snippet = DOMJIT::CallDOMGetterSnippet::create();
    snippet->requireGlobalObject = false;
    snippet->effect = DOMJIT::Effect::forPure();
    snippet->setGenerator([=](CCallHelpers& jit, SnippetParams& params) {
        GPRReg result = params[0].gpr();
        jit.load16(CCallHelpers::Address(params[1].gpr(), DecimalObject::offsetOfValue() + Decimal128::offsetOfUpper() + 6), result);
        jit.and32(CCallHelpers::TrustedImm32(mask), result);
        jit.compare32(condition, result, CCallHelpers::TrustedImm32(expected), result);
        jit.or32(CCallHelpers::TrustedImm32(JSValue::ValueFalse), result);
        return CCallHelpers::JumpList();
    });
    return snippet;
}

static constexpr int shiftOfExponentIn16Bits = Decimal128::shiftOfExponent - 48;
static constexpr int32_t maskOfExponent = ((1 << Decimal128::bitsOfExponent) - 1) << shiftOfExponentIn16Bits;

static Ref<DOMJIT::CallDOMGetterSnippet> isNaNSnippet()
{
    return testOfExponent(maskOfExponent, CCallHelpers::Equal, Decimal128::exponentOfNaN << shiftOfExponentIn16Bits);
}

static Ref<DOMJIT::CallDOMGetterSnippet> isFiniteSnippet()
{
    // NaN and infinity differ in the last bit only.
    static_assert((Decimal128::exponentOfNaN ^ Decimal128::exponentOfInfinity) == 1);
    return testOfExponent(maskOfExponent & ~(1 << shiftOfExponentIn16Bits), CCallHelpers::NotEqual, (Decimal128::exponentOfInfinity & ~1) << shiftOfExponentIn16Bits);
}

// Looks at no realm, so its caller passes none: the first operand is anything.
JSC_DECLARE_JIT_OPERATION(operationDecimalExponent, EncodedJSValue, (JSGlobalObject*, void*));
JSC_DEFINE_JIT_OPERATION(operationDecimalExponent, EncodedJSValue, (JSGlobalObject*, void* decimal))
{
    return { JSValue::encode(exponentOf(static_cast<DecimalObject*>(decimal)->value())), nullptr };
}

static Ref<DOMJIT::CallDOMGetterSnippet> exponentSnippet()
{
    Ref<DOMJIT::CallDOMGetterSnippet> snippet = DOMJIT::CallDOMGetterSnippet::create();
    snippet->requireGlobalObject = false;
    snippet->numGPScratchRegisters = 3;
    snippet->effect = DOMJIT::Effect::forPure();
    snippet->setGenerator([=](CCallHelpers& jit, SnippetParams& params) {
        GPRReg result = params[0].gpr();
        GPRReg base = params[1].gpr();
        GPRReg low = params.gpScratch(0);
        GPRReg digits = params.gpScratch(1);
        GPRReg scratch = params.gpScratch(2);

        // A coefficient of more than 64 bits, and the values that have none.
        CCallHelpers::JumpList slow;
        jit.load64(CCallHelpers::Address(base, DecimalObject::offsetOfValue() + Decimal128::offsetOfUpper()), scratch);
        jit.lshift64(CCallHelpers::TrustedImm32(64 - Decimal128::shiftOfExponent), scratch);
        slow.append(jit.branchTest64(CCallHelpers::NonZero, scratch));
        jit.load64(CCallHelpers::Address(base, DecimalObject::offsetOfValue() + Decimal128::offsetOfLow()), low);
        slow.append(jit.branchTest64(CCallHelpers::Zero, low));

        // As numberOfDigits().
        jit.countLeadingZeros64(low, scratch);
        jit.move(CCallHelpers::TrustedImm32(64), digits);
        jit.sub32(scratch, digits);
        jit.mul32(CCallHelpers::TrustedImm32(1233), digits, digits);
        jit.urshift32(CCallHelpers::TrustedImm32(12), digits);
        jit.move(CCallHelpers::TrustedImmPtr(Decimal128::powersOf10()), scratch);
        jit.load64(CCallHelpers::BaseIndex(scratch, digits, CCallHelpers::TimesEight), scratch);
        jit.compare64(CCallHelpers::AboveOrEqual, low, scratch, scratch);
        jit.add32(scratch, digits);

        loadExponent(jit, base, scratch);
        jit.add32(scratch, digits);
        jit.sub32(CCallHelpers::TrustedImm32(1), digits);
        jit.boxInt32(digits, result);
        params.addSlowPathCall(slow, jit, operationDecimalExponent, result, base, base);
        return CCallHelpers::JumpList();
    });
    return snippet;
}

// One intrinsic is enough: the function says which method it is.
const DOMJIT::GetterSetter* DecimalObject::inlinedMethod(TaggedNativeFunction function)
{
    RELEASE_ASSERT(Decimal128::hasLayoutOfGeneratedCode());
    static const DOMJIT::GetterSetter isNaN { nullptr, isNaNSnippet, SpecBoolean };
    static const DOMJIT::GetterSetter isFinite { nullptr, isFiniteSnippet, SpecBoolean };
    static const DOMJIT::GetterSetter exponent { nullptr, exponentSnippet, SpecBytecodeNumber };
    if (function == TaggedNativeFunction { decimalProtoFuncIsNaN })
        return &isNaN;
    if (function == TaggedNativeFunction { decimalProtoFuncIsFinite })
        return &isFinite;
    if (function == TaggedNativeFunction { decimalProtoFuncExponent })
        return &exponent;
    return nullptr;
}

// The methods with a Decimal as their one argument, and round() with an integer and no mode.
#define FOR_EACH_DIRECT_BINARY_METHOD(macro) \
    macro(Add, add) \
    macro(Subtract, subtract) \
    macro(Multiply, multiply) \
    macro(Divide, divide) \
    macro(Remainder, remainder)

#define FOR_EACH_DIRECT_COMPARISON(macro) \
    macro(Compare, numberOfOrder(Decimal128::compare(x, y)), SpecBytecodeNumber) \
    macro(Equals, jsBoolean(Decimal128::compare(x, y) == 0), SpecBoolean) \
    macro(NotEquals, jsBoolean(isLessOrGreater(Decimal128::compare(x, y))), SpecBoolean) \
    macro(LessThan, jsBoolean(Decimal128::compare(x, y) < 0), SpecBoolean) \
    macro(LessThanOrEqual, jsBoolean(Decimal128::compare(x, y) <= 0), SpecBoolean) \
    macro(GreaterThan, jsBoolean(Decimal128::compare(x, y) > 0), SpecBoolean) \
    macro(GreaterThanOrEqual, jsBoolean(Decimal128::compare(x, y) >= 0), SpecBoolean)

#define DEFINE_DIRECT_BINARY_METHOD(name, function) \
JSC_DECLARE_JIT_OPERATION(operationDecimal##name, EncodedJSValue, (JSGlobalObject*, DecimalObject*, DecimalObject*)); \
JSC_DEFINE_JIT_OPERATION(operationDecimal##name, EncodedJSValue, (JSGlobalObject* globalObject, DecimalObject* left, DecimalObject* right)) \
{ \
    VM& vm = globalObject->vm(); \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    OPERATION_RETURN(scope, JSValue::encode(DecimalObject::create(vm, globalObject->decimalStructure(), Decimal128::function(left->value(), right->value())))); \
} \
static const DOMJIT::Signature signatureOfDecimal##name(operationDecimal##name, DecimalObject::info(), effectOfAllocation, SpecObjectOther, SpecObjectOther);

// Neither allocates nor throws.
#define DEFINE_DIRECT_COMPARISON(name, result, type) \
JSC_DECLARE_JIT_OPERATION(operationDecimal##name, EncodedJSValue, (JSGlobalObject*, DecimalObject*, DecimalObject*)); \
JSC_DEFINE_JIT_OPERATION(operationDecimal##name, EncodedJSValue, (JSGlobalObject*, DecimalObject* left, DecimalObject* right)) \
{ \
    const Decimal128& x = left->value(); \
    const Decimal128& y = right->value(); \
    return { JSValue::encode(result), nullptr }; \
} \
static const DOMJIT::Signature signatureOfDecimal##name(operationDecimal##name, DecimalObject::info(), effectOfComparison, type, SpecObjectOther);

static constexpr DOMJIT::Effect effectOfComparison { DOMJIT::HeapRange::none(), DOMJIT::HeapRange::none(), DOMJIT::HeapRange::top() };

FOR_EACH_DIRECT_BINARY_METHOD(DEFINE_DIRECT_BINARY_METHOD)
FOR_EACH_DIRECT_COMPARISON(DEFINE_DIRECT_COMPARISON)

// For a count that is an integer in the source.
JSC_DECLARE_JIT_OPERATION(operationDecimalRound, EncodedJSValue, (JSGlobalObject*, DecimalObject*, int32_t));
JSC_DEFINE_JIT_OPERATION(operationDecimalRound, EncodedJSValue, (JSGlobalObject* globalObject, DecimalObject* decimal, int32_t digits))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    OPERATION_RETURN(scope, JSValue::encode(DecimalObject::create(vm, globalObject->decimalStructure(), decimal->value().round(digits, Decimal128::RoundingMode::HalfEven))));
}
static const DOMJIT::Signature signatureOfDecimalRound(operationDecimalRound, DecimalObject::info(), effectOfAllocation, SpecObjectOther, SpecInt32Only);

// Null if it is none of them.
const DOMJIT::Signature* DecimalObject::directMethod(TaggedNativeFunction function)
{
#define ANSWER_SIGNATURE(name, ...) \
    if (function == TaggedNativeFunction { decimalProtoFunc##name }) \
        return &signatureOfDecimal##name;
    FOR_EACH_DIRECT_BINARY_METHOD(ANSWER_SIGNATURE)
    FOR_EACH_DIRECT_COMPARISON(ANSWER_SIGNATURE)
    FOR_EACH_DIRECT_METHOD(ANSWER_SIGNATURE)
    if (function == TaggedNativeFunction { decimalProtoFuncRound })
        return &signatureOfDecimalRound;
#undef ANSWER_SIGNATURE
    return nullptr;
}

#endif // ENABLE(JIT)

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
