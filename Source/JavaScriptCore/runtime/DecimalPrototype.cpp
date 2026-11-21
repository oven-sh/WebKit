/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "DecimalPrototype.h"

#include "AOTRemovableFeature.h"
#include "DecimalObject.h"
#include "DecimalOperation.h"
#include "DecimalStringCache.h"
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
  isNaN                decimalProtoFuncIsNaN                DontEnum|Function 0 DecimalIntrinsic
  isFinite             decimalProtoFuncIsFinite             DontEnum|Function 0 DecimalIntrinsic
  exponent             decimalProtoFuncExponent             DontEnum|Function 0 DecimalIntrinsic
  mantissa             decimalProtoFuncMantissa             DontEnum|Function 0 DecimalIntrinsic
  abs                  decimalProtoFuncAbs                  DontEnum|Function 0 DecimalIntrinsic
  negate               decimalProtoFuncNegate               DontEnum|Function 0 DecimalIntrinsic
  add                  decimalProtoFuncAdd                  DontEnum|Function 1 DecimalIntrinsic
  subtract             decimalProtoFuncSubtract             DontEnum|Function 1 DecimalIntrinsic
  multiply             decimalProtoFuncMultiply             DontEnum|Function 1 DecimalIntrinsic
  divide               decimalProtoFuncDivide               DontEnum|Function 1 DecimalIntrinsic
  remainder            decimalProtoFuncRemainder            DontEnum|Function 1 DecimalIntrinsic
  compare              decimalProtoFuncCompare              DontEnum|Function 1 DecimalIntrinsic
  equals               decimalProtoFuncEquals               DontEnum|Function 1 DecimalIntrinsic
  notEquals            decimalProtoFuncNotEquals            DontEnum|Function 1 DecimalIntrinsic
  lessThan             decimalProtoFuncLessThan             DontEnum|Function 1 DecimalIntrinsic
  lessThanOrEqual      decimalProtoFuncLessThanOrEqual      DontEnum|Function 1 DecimalIntrinsic
  greaterThan          decimalProtoFuncGreaterThan          DontEnum|Function 1 DecimalIntrinsic
  greaterThanOrEqual   decimalProtoFuncGreaterThanOrEqual   DontEnum|Function 1 DecimalIntrinsic
  round                decimalProtoFuncRound                DontEnum|Function 1 DecimalIntrinsic
  scale10              decimalProtoFuncScale10              DontEnum|Function 1 DecimalIntrinsic
  toString             decimalProtoFuncToString             DontEnum|Function 0 DecimalIntrinsic
  toLocaleString       decimalProtoFuncToLocaleString       DontEnum|Function 0
  toExponential        decimalProtoFuncToExponential        DontEnum|Function 0 DecimalIntrinsic
  toFixed              decimalProtoFuncToFixed              DontEnum|Function 0 DecimalIntrinsic
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

static NEVER_INLINE EncodedJSValue throwNotDecimal(JSGlobalObject* globalObject, ASCIILiteral message)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    return throwVMTypeError(globalObject, scope, message);
}

// RequireInternalSlot(value, [[DecimalData]]), which returns from the function if it throws.
#define REQUIRE_DECIMAL(name, value, message) \
    auto* name = dynamicDowncast<DecimalObject>(value); \
    if (!name) [[unlikely]] \
        return throwNotDecimal(globalObject, message)

static constexpr ASCIILiteral receiverMessage = "Decimal.prototype method called on an object that is not a Decimal"_s;
static constexpr ASCIILiteral operandMessage = "The operand of a Decimal operation must be a Decimal"_s;

static EncodedJSValue newDecimal(JSGlobalObject* globalObject, Decimal128 value)
{
    return JSValue::encode(DecimalObject::create(globalObject->vm(), globalObject->decimalStructure(), value));
}

// Answers through `function` of the receiver's value.
template<typename Function>
static ALWAYS_INLINE EncodedJSValue unary(JSGlobalObject* globalObject, CallFrame* callFrame, const Function& function)
{
    REQUIRE_DECIMAL(decimal, callFrame->thisValue(), receiverMessage);
    return function(decimal->value());
}

template<typename Function>
static ALWAYS_INLINE EncodedJSValue binary(JSGlobalObject* globalObject, CallFrame* callFrame, const Function& function)
{
    REQUIRE_DECIMAL(decimal, callFrame->thisValue(), receiverMessage);
    REQUIRE_DECIMAL(operand, callFrame->argument(0), operandMessage);
    return function(decimal->value(), operand->value());
}

static ALWAYS_INLINE EncodedJSValue arithmetic(JSGlobalObject* globalObject, CallFrame* callFrame, Decimal128 (*operation)(Decimal128, Decimal128))
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

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncExponent, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return unary(globalObject, callFrame, [](const Decimal128& value) {
        return JSValue::encode(jsNumber(value.exponent()));
    });
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncMantissa, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return unary(globalObject, callFrame, [&](const Decimal128& value) {
        return newDecimal(globalObject, value.mantissa());
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
    return jsNumber((order > 0) - (order < 0));
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

// Null if it names none, or threw.
static ALWAYS_INLINE std::optional<Decimal128::RoundingMode> roundingModeFromName(JSGlobalObject* globalObject, JSString* string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    using Mode = Decimal128::RoundingMode;
    // The string of a literal is that of the identifier.
    auto* impl = string->tryGetValueImpl();
    if (impl == vm.propertyNames->floor.impl())
        return Mode::Floor;
    if (impl == vm.propertyNames->halfExpand.impl())
        return Mode::HalfExpand;
    if (impl == vm.propertyNames->ceil.impl())
        return Mode::Ceil;
    if (impl == vm.propertyNames->trunc.impl())
        return Mode::Trunc;
    if (impl == vm.propertyNames->halfEven.impl())
        return Mode::HalfEven;
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return Decimal128::roundingModeNamed(view);
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
static NEVER_INLINE EncodedJSValue roundWithAnyArguments(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    REQUIRE_DECIMAL(decimal, callFrame->thisValue(), receiverMessage);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    double digits = digitsOfRound(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    auto mode = toRoundingMode(globalObject, callFrame->argument(1));
    RETURN_IF_EXCEPTION(scope, { });
    return newDecimal(globalObject, decimal->value().round(digits, mode));
}

JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncRound, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    if (callFrame->argument(0).isInt32() && callFrame->argument(1).isUndefined()) {
        return unary(globalObject, callFrame, [&](const Decimal128& value) {
            return newDecimal(globalObject, Decimal128::round(value, callFrame->uncheckedArgument(0).asInt32(), Decimal128::RoundingMode::HalfEven));
        });
    }
    return roundWithAnyArguments(globalObject, callFrame);
}

// https://tc39.es/proposal-decimal/#sec-decimal-method-scale10
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncScale10, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    REQUIRE_DECIMAL(decimal, callFrame->thisValue(), receiverMessage);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue argument = callFrame->argument(0);
    if (!argument.isNumber()) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "The power must be a number"_s);
    double power = argument.asNumber();
    if (std::isfinite(power) && !isInteger(power)) [[unlikely]]
        return throwVMRangeError(globalObject, scope, "The power must be an integer"_s);
    return newDecimal(globalObject, decimal->value().scale10(power));
}

Decimal128 roundOrNoValue(Decimal128 value, JSValue digits, Decimal128::RoundingMode mode)
{
    if (digits.isInt32()) [[likely]]
        return Decimal128::round(value, digits.asInt32(), mode);
    return digits.isNumber() && isInteger(digits.asNumber()) ? value.round(digits.asNumber(), mode) : Decimal128::noValue();
}

Decimal128 scale10OrNoValue(Decimal128 value, JSValue power)
{
    if (!power.isNumber() || (std::isfinite(power.asNumber()) && !isInteger(power.asNumber()))) [[unlikely]]
        return Decimal128::noValue();
    return value.scale10(power.asNumber());
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
    REQUIRE_DECIMAL(decimal, callFrame->thisValue(), receiverMessage);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    toOptions(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(vm.decimalStringCache().toString(vm, decimal->value()));
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.tolocalestring
//
// The steps read [[Decimal128Data]], a slot that nothing has, and do not convert its value. It is [[DecimalData]], as everywhere else.
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncToLocaleString, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSC_REMOVABLE_HOST_FUNCTION_DEFINITION(IntlNumberFormat, decimalProtoFuncToLocaleString);
    REQUIRE_DECIMAL(decimal, callFrame->thisValue(), receiverMessage);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

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
    REQUIRE_DECIMAL(decimal, callFrame->thisValue(), receiverMessage);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
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
    REQUIRE_DECIMAL(decimal, callFrame->thisValue(), receiverMessage);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

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
    JSString* result = tryDecimalToFixed(globalObject, decimal->value(), digitsValue, roundingModeValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (result) [[likely]]
        return JSValue::encode(result);

    toIntegerOption(globalObject, digitsValue, 0, "digits must be a number"_s, "digits must be a non-negative integer"_s);
    RETURN_IF_EXCEPTION(scope, { });
    toRoundingMode(globalObject, roundingModeValue);
    RETURN_IF_EXCEPTION(scope, { });
    throwOutOfMemoryError(globalObject, scope);
    return { };
}

JSString* tryDecimalToFixed(JSGlobalObject* globalObject, Decimal128 value, JSValue digitsValue, JSValue roundingModeValue)
{
    VM& vm = globalObject->vm();
    if (!digitsValue.isInt32() && !(digitsValue.isNumber() && isInteger(digitsValue.asNumber()))) [[unlikely]]
        return nullptr;
    double digits = digitsValue.asNumber();
    if (digits < 0) [[unlikely]]
        return nullptr;
    std::optional mode = Decimal128::RoundingMode::HalfEven;
    if (!roundingModeValue.isUndefined()) {
        mode = roundingModeValue.isString() ? roundingModeFromName(globalObject, asString(roundingModeValue)) : std::nullopt;
        if (!mode) [[unlikely]]
            return nullptr;
    }
    if (digits <= DecimalStringCache::maximumDigits && value.isShort()) [[likely]]
        return vm.decimalStringCache().toFixed(vm, value, static_cast<unsigned>(digits), *mode);
    String text = value.toFixed(digits, *mode);
    return text.isNull() ? nullptr : jsString(vm, WTF::move(text));
}

// https://tc39.es/proposal-decimal/#sec-decimal.prototype.toprecision
//
// The steps read the precision and never use it: it stays 0, which is a RangeError. This uses it. Without options it is 0 and the
// RangeError is what the steps have, although the description of the method says "equivalent to toString". See Decimal128::toPrecision().
JSC_DEFINE_HOST_FUNCTION(decimalProtoFuncToPrecision, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    REQUIRE_DECIMAL(decimal, callFrame->thisValue(), receiverMessage);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

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

// One intrinsic is enough: the function says which method it is.
std::optional<DecimalOperation> decimalOperationOf(TaggedNativeFunction function)
{
#define ANSWER(name, method) \
    if (function == TaggedNativeFunction { decimalProtoFunc##name }) \
        return DecimalOperation::name;
    FOR_EACH_DECIMAL_OPERATION(ANSWER)
#undef ANSWER
    return std::nullopt;
}

#undef REQUIRE_DECIMAL

} // namespace JSC
