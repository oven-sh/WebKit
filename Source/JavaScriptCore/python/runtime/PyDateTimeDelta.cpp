/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyDateTime.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyTuple.h"
#include <cmath>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace DateTime {

// datetime.timedelta

// ---- Microseconds, as ints ----

static JSValue deltaMakeInt(JSGlobalObject* globalObject, Int128 value)
{
    if (value >= std::numeric_limits<int64_t>::min() && value <= std::numeric_limits<int64_t>::max()) [[likely]]
        return makeInt(globalObject, static_cast<int64_t>(value));
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue high = intLeftShift(globalObject, makeInt(globalObject, static_cast<int64_t>(value >> 64)), jsNumber(64));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, intAdd(globalObject, high, makeInt(globalObject, static_cast<uint64_t>(value))));
}

// `total` is an exact int.
static JSValue deltaFromMicroseconds(JSGlobalObject* globalObject, State& state, PyType* type, JSValue total)
{
    if (auto small = tryInt64(total)) [[likely]]
        return newDeltaFromTotal(globalObject, state, type, *small);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue allSeconds;
    JSValue microseconds;
    JSValue days;
    JSValue seconds;
    if (failed(scope, intDivMod(globalObject, total, jsNumber(microsecondsPerSecond), allSeconds, microseconds)))
        return { };
    if (failed(scope, intDivMod(globalObject, allSeconds, jsNumber(secondsPerDay), days, seconds)))
        return { };
    auto fewDays = tryInt64(days);
    if (!fewDays || *fewDays < std::numeric_limits<int>::min() || *fewDays > std::numeric_limits<int>::max())
        return raiseOverflowError(globalObject, scope, "Python int too large to convert to C int"_s);
    RELEASE_AND_RETURN(scope, newDelta(globalObject, state, type, *fewDays, smallIntValue(seconds), smallIntValue(microseconds)));
}

// The quotient that is nearest, the even one of two that are equally near. Both are exact ints.
static JSValue deltaDivideNearest(JSGlobalObject* globalObject, JSValue dividend, JSValue divisor)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue quotient;
    JSValue remainder;
    if (failed(scope, intDivMod(globalObject, dividend, divisor, quotient, remainder)))
        return { };
    JSValue twice = intAdd(globalObject, remainder, remainder);
    RETURN_IF_EXCEPTION(scope, { });
    int order = compareInts(twice, divisor);
    bool isMoreThanHalf = intSign(divisor) > 0 ? order > 0 : order < 0;
    if (isMoreThanHalf || (!order && intIsOdd(quotient)))
        RELEASE_AND_RETURN(scope, intAdd(globalObject, quotient, jsNumber(1)));
    return quotient;
}

// ---- timedelta(...) ----

struct DeltaSum {
    Int128 small { 0 };
    JSValue big; // Instead, once something did not fit.
    double leftover { 0 }; // Fractions of a microsecond.
};

// `value` is an exact int.
static bool deltaAddProduct(JSGlobalObject* globalObject, DeltaSum& sum, JSValue value, int64_t factor)
{
    if (!sum.big) {
        if (auto small = tryInt64(value)) [[likely]] {
            sum.small += static_cast<Int128>(*small) * factor;
            return true;
        }
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!sum.big) {
        sum.big = deltaMakeInt(globalObject, sum.small);
        RETURN_IF_EXCEPTION(scope, false);
    }
    JSValue product = intMultiply(globalObject, value, makeInt(globalObject, factor));
    RETURN_IF_EXCEPTION(scope, false);
    sum.big = intAdd(globalObject, sum.big, product);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

static bool deltaAccumulate(JSGlobalObject* globalObject, DeltaSum& sum, ASCIILiteral tag, JSValue number, int64_t factor)
{
    if (!number)
        return true;
    if (isInt(number))
        return deltaAddProduct(globalObject, sum, intValue(number), factor);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isFloat(number)) {
        raiseTypeError(globalObject, scope, "unsupported type for timedelta "_s, tag, " component: "_s, typeName(globalObject, number));
        return false;
    }
    double value = floatValue(number);
    if (std::isnan(value)) {
        raiseValueError(globalObject, scope, "cannot convert float NaN to integer"_s);
        return false;
    }
    if (std::isinf(value)) {
        raiseOverflowError(globalObject, scope, "cannot convert float infinity to integer"_s);
        return false;
    }
    // The whole part loses nothing. The fraction, in microseconds, has a whole part and a fraction again.
    double whole;
    double fraction = std::modf(value, &whole);
    JSValue wholeInt = intFromDouble(globalObject, whole);
    RETURN_IF_EXCEPTION(scope, false);
    bool isAdded = deltaAddProduct(globalObject, sum, wholeInt, factor);
    RETURN_IF_EXCEPTION(scope, false);
    ASSERT_UNUSED(isAdded, isAdded);
    if (!fraction)
        return true;
    fraction = std::modf(static_cast<double>(factor) * fraction, &whole);
    sum.leftover += fraction;
    RELEASE_AND_RETURN(scope, deltaAddProduct(globalObject, sum, makeInt(globalObject, static_cast<int64_t>(whole)), 1));
}

static JSValue deltaNew(JSGlobalObject* globalObject, JSValue cls, JSValue days, JSValue seconds, JSValue microseconds, JSValue milliseconds, JSValue minutes, JSValue hours, JSValue weeks)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    State& state = stateOf(globalObject, type);

    DeltaSum sum;
    if (failed(scope, deltaAccumulate(globalObject, sum, "microseconds"_s, microseconds, 1))
        || failed(scope, deltaAccumulate(globalObject, sum, "milliseconds"_s, milliseconds, 1000))
        || failed(scope, deltaAccumulate(globalObject, sum, "seconds"_s, seconds, microsecondsPerSecond))
        || failed(scope, deltaAccumulate(globalObject, sum, "minutes"_s, minutes, 60ll * microsecondsPerSecond))
        || failed(scope, deltaAccumulate(globalObject, sum, "hours"_s, hours, 3600ll * microsecondsPerSecond))
        || failed(scope, deltaAccumulate(globalObject, sum, "days"_s, days, microsecondsPerDay))
        || failed(scope, deltaAccumulate(globalObject, sum, "weeks"_s, weeks, 7 * microsecondsPerDay)))
        return { };

    if (sum.leftover) {
        double whole = std::round(sum.leftover);
        if (std::fabs(whole - sum.leftover) == 0.5) {
            // Half way: to whichever makes the sum even.
            int isOdd = sum.big ? intIsOdd(sum.big) : static_cast<int>(sum.small & 1);
            whole = 2.0 * std::round((sum.leftover + isOdd) * 0.5) - isOdd;
        }
        deltaAddProduct(globalObject, sum, makeInt(globalObject, static_cast<int64_t>(whole)), 1);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (sum.big)
        RELEASE_AND_RETURN(scope, deltaFromMicroseconds(globalObject, state, type, sum.big));
    RELEASE_AND_RETURN(scope, newDeltaFromTotal(globalObject, state, type, sum.small));
}

// ---- Arithmetic ----
//
// Each takes the operands in the order in which they were written; at least one is a timedelta. The results are plain timedeltas.

static JSValue deltaAdd(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    auto* a = PyTimeDelta::tryFrom(left);
    auto* b = PyTimeDelta::tryFrom(right);
    if (!a || !b)
        return runtime(globalObject).notImplemented();
    return newDelta(globalObject, stateOf(globalObject, a), static_cast<int64_t>(a->days()) + b->days(), a->seconds() + b->seconds(), a->microseconds() + b->microseconds());
}

static JSValue deltaSubtract(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    auto* a = PyTimeDelta::tryFrom(left);
    auto* b = PyTimeDelta::tryFrom(right);
    if (!a || !b)
        return runtime(globalObject).notImplemented();
    return newDelta(globalObject, stateOf(globalObject, a), static_cast<int64_t>(a->days()) - b->days(), a->seconds() - b->seconds(), a->microseconds() - b->microseconds());
}

// delta * number, or delta / number.
static JSValue deltaScaleByFloat(JSGlobalObject* globalObject, State& state, PyTimeDelta* delta, JSValue number, bool divides)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A class derived from float may have its own.
    JSValue ratio = callMethod(globalObject, number, state.asIntegerRatioName.get());
    RETURN_IF_EXCEPTION(scope, { });
    auto* pair = PyTuple::tryFrom(ratio);
    if (!pair)
        return raiseTypeError(globalObject, scope, "unexpected return type from as_integer_ratio(): expected tuple, not '"_s, typeName(globalObject, ratio), '\'');
    if (pair->length() != 2)
        return raiseValueError(globalObject, scope, "as_integer_ratio() must return a 2-tuple"_s);
    JSValue total = deltaMakeInt(globalObject, delta->total());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue product = binaryOperation(globalObject, BinaryOperator::Mult, total, pair->at(divides));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue divisor = pair->at(!divides);
    if (!isInt(product) || !isInt(divisor))
        return raiseTypeError(globalObject, scope, "non-integer arguments in division"_s);
    JSValue quotient = deltaDivideNearest(globalObject, intValue(product), intValue(divisor));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, deltaFromMicroseconds(globalObject, state, state.deltaType.get(), quotient));
}

static JSValue deltaMultiply(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* delta = PyTimeDelta::tryFrom(left);
    JSValue number = right;
    if (!delta) {
        delta = PyTimeDelta::from(right);
        number = left;
    }
    State& state = stateOf(globalObject, delta);
    if (isFloat(number))
        RELEASE_AND_RETURN(scope, deltaScaleByFloat(globalObject, state, delta, number, false));
    if (!isInt(number))
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    if (!isExactInt(number) && !number.isBoolean()) [[unlikely]] {
        // Its class may have a __rmul__ of its own.
        JSValue total = deltaMakeInt(globalObject, delta->total());
        RETURN_IF_EXCEPTION(scope, { });
        JSValue product = binaryOperation(globalObject, BinaryOperator::Mult, total, number);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isInt(product)) {
            divmod(globalObject, product, jsNumber(microsecondsPerSecond));
            RETURN_IF_EXCEPTION(scope, { });
            return raiseTypeError(globalObject, scope, "an integer is required"_s);
        }
        RELEASE_AND_RETURN(scope, deltaFromMicroseconds(globalObject, state, state.deltaType.get(), intValue(product)));
    }
    JSValue factor = intValue(number);
    auto small = tryInt64(factor);
    if (small && *small > -(1ll << 59) && *small < (1ll << 59)) [[likely]]
        RELEASE_AND_RETURN(scope, newDeltaFromTotal(globalObject, state, state.deltaType.get(), delta->total() * *small));
    JSValue total = deltaMakeInt(globalObject, delta->total());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue product = intMultiply(globalObject, factor, total);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, deltaFromMicroseconds(globalObject, state, state.deltaType.get(), product));
}

static JSValue deltaFloorDivide(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* delta = PyTimeDelta::tryFrom(left);
    if (!delta)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    Int128 remainder;
    if (auto* divisor = PyTimeDelta::tryFrom(right)) {
        if (divisor->isZero())
            RELEASE_AND_RETURN(scope, intFloorDivide(globalObject, jsNumber(1), jsNumber(0)));
        RELEASE_AND_RETURN(scope, deltaMakeInt(globalObject, floorDivide<Int128>(delta->total(), divisor->total(), remainder)));
    }
    if (!isInt(right))
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    State& state = stateOf(globalObject, delta);
    JSValue divisor = intValue(right);
    auto small = tryInt64(divisor);
    if (small && *small) [[likely]]
        RELEASE_AND_RETURN(scope, newDeltaFromTotal(globalObject, state, state.deltaType.get(), floorDivide<Int128>(delta->total(), *small, remainder)));
    JSValue total = deltaMakeInt(globalObject, delta->total());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue quotient = intFloorDivide(globalObject, total, divisor);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, deltaFromMicroseconds(globalObject, state, state.deltaType.get(), quotient));
}

// total / divisor, rounded once.
static JSValue deltaRatio(JSGlobalObject* globalObject, Int128 total, Int128 divisor)
{
    static constexpr Int128 exact = static_cast<Int128>(1) << 53;
    if (divisor && total > -exact && total < exact && divisor > -exact && divisor < exact) [[likely]]
        return makeFloat(globalObject, static_cast<double>(static_cast<int64_t>(total)) / static_cast<double>(static_cast<int64_t>(divisor)));
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue a = deltaMakeInt(globalObject, total);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue b = deltaMakeInt(globalObject, divisor);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, intTrueDivide(globalObject, a, b));
}

static JSValue deltaTrueDivide(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* delta = PyTimeDelta::tryFrom(left);
    if (!delta)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    if (auto* divisor = PyTimeDelta::tryFrom(right))
        RELEASE_AND_RETURN(scope, deltaRatio(globalObject, delta->total(), divisor->total()));
    State& state = stateOf(globalObject, delta);
    if (isFloat(right))
        RELEASE_AND_RETURN(scope, deltaScaleByFloat(globalObject, state, delta, right, true));
    if (!isInt(right))
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    JSValue total = deltaMakeInt(globalObject, delta->total());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue quotient = deltaDivideNearest(globalObject, total, intValue(right));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, deltaFromMicroseconds(globalObject, state, state.deltaType.get(), quotient));
}

static JSValue deltaRemainder(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    auto* a = PyTimeDelta::tryFrom(left);
    auto* b = PyTimeDelta::tryFrom(right);
    if (!a || !b)
        return runtime(globalObject).notImplemented();
    if (b->isZero())
        return intModulo(globalObject, jsNumber(1), jsNumber(0));
    State& state = stateOf(globalObject, a);
    Int128 remainder;
    floorDivide<Int128>(a->total(), b->total(), remainder);
    return newDeltaFromTotal(globalObject, state, state.deltaType.get(), remainder);
}

static JSValue deltaDivMod(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* a = PyTimeDelta::tryFrom(left);
    auto* b = PyTimeDelta::tryFrom(right);
    if (!a || !b)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    if (b->isZero())
        RELEASE_AND_RETURN(scope, divmod(globalObject, jsNumber(1), jsNumber(0)));
    State& state = stateOf(globalObject, a);
    Int128 remainder;
    JSValue quotient = deltaMakeInt(globalObject, floorDivide<Int128>(a->total(), b->total(), remainder));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue rest = newDeltaFromTotal(globalObject, state, state.deltaType.get(), remainder);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { quotient, rest }));
}

template<JSValue (*operation)(JSGlobalObject*, JSValue, JSValue)>
static JSValue deltaReflected(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    return operation(globalObject, other, self);
}

static JSValue deltaNegative(JSGlobalObject* globalObject, JSValue self)
{
    auto* delta = PyTimeDelta::from(self);
    return newDelta(globalObject, stateOf(globalObject, delta), -static_cast<int64_t>(delta->days()), -delta->seconds(), -delta->microseconds());
}

static JSValue deltaPositive(JSGlobalObject* globalObject, JSValue self)
{
    auto* delta = PyTimeDelta::from(self);
    return newDelta(globalObject, stateOf(globalObject, delta), delta->days(), delta->seconds(), delta->microseconds());
}

static JSValue deltaAbsolute(JSGlobalObject* globalObject, JSValue self)
{
    return PyTimeDelta::from(self)->days() < 0 ? deltaNegative(globalObject, self) : deltaPositive(globalObject, self);
}

static JSValue deltaBool(JSGlobalObject*, JSValue self) { return jsBoolean(!PyTimeDelta::from(self)->isZero()); }

template<CompareOperator op>
static JSValue deltaCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto* second = PyTimeDelta::tryFrom(other);
    if (!second)
        return runtime(globalObject).notImplemented();
    return compareResult(PyTimeDelta::from(self)->compare(second), op);
}

static JSValue deltaHash(JSGlobalObject*, JSValue self) { return smallInt(hashOfDelta(PyTimeDelta::from(self))); }

// ---- Text ----

static JSValue deltaRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* delta = PyTimeDelta::from(self);
    Text arguments;
    auto add = [&](ASCIILiteral name, int value) {
        if (!value)
            return;
        if (!arguments.isEmpty())
            arguments.append(", "_s);
        arguments.append(name);
        arguments.append('=');
        arguments.append(value, 1);
    };
    add("days"_s, delta->days());
    add("seconds"_s, delta->seconds());
    add("microseconds"_s, delta->microseconds());
    if (arguments.isEmpty())
        arguments.append('0');
    return makeStr(globalObject, makeString(delta->type()->nameInMessages(), '(', arguments.span(), ')'));
}

static JSValue deltaStr(JSGlobalObject* globalObject, JSValue self)
{
    auto* delta = PyTimeDelta::from(self);
    Text text;
    if (int days = delta->days()) {
        text.append(days, 1);
        text.append(days == 1 || days == -1 ? " day, "_s : " days, "_s);
    }
    text.append(delta->seconds() / 3600, 1);
    text.append(':');
    text.append(delta->seconds() / 60 % 60, 2);
    text.append(':');
    text.append(delta->seconds() % 60, 2);
    if (delta->microseconds()) {
        text.append('.');
        text.append(delta->microseconds(), 6);
    }
    return makeStr(getVM(globalObject), text.toString());
}

// ---- The rest ----

static JSValue deltaTotalSeconds(JSGlobalObject* globalObject, JSValue self)
{
    return deltaRatio(globalObject, PyTimeDelta::from(self)->total(), microsecondsPerSecond);
}

static JSValue deltaReduce(JSGlobalObject* globalObject, JSValue self)
{
    auto* delta = PyTimeDelta::from(self);
    PyTuple* state = PyTuple::create(globalObject, { jsNumber(delta->days()), jsNumber(delta->seconds()), jsNumber(delta->microseconds()) });
    return PyTuple::create(globalObject, { delta->type(), state });
}

static JSValue deltaGetDays(JSGlobalObject*, JSValue self) { return jsNumber(PyTimeDelta::from(self)->days()); }
static JSValue deltaGetSeconds(JSGlobalObject*, JSValue self) { return jsNumber(PyTimeDelta::from(self)->seconds()); }
static JSValue deltaGetMicroseconds(JSGlobalObject*, JSValue self) { return jsNumber(PyTimeDelta::from(self)->microseconds()); }

static constexpr NativeDef deltaMethods[] = {
    PY_NEW(deltaNew, "($type, /, days=0, seconds=0, microseconds=0, milliseconds=0, minutes=0, hours=0, weeks=0)"),
    PY_SLOT("__repr__", deltaRepr, "($self, /)"),
    PY_SLOT("__str__", deltaStr, "($self, /)"),
    PY_SLOT("__hash__", deltaHash, "($self, /)"),
    PY_SLOT("__bool__", deltaBool, "($self, /)"),
    PY_DATETIME_COMPARISONS(deltaCompare),
    PY_SLOT("__add__", deltaAdd, "($self, value, /)"),
    PY_SLOT("__radd__", deltaReflected<deltaAdd>, "($self, value, /)"),
    PY_SLOT("__sub__", deltaSubtract, "($self, value, /)"),
    PY_SLOT("__rsub__", deltaReflected<deltaSubtract>, "($self, value, /)"),
    PY_SLOT("__mul__", deltaMultiply, "($self, value, /)"),
    PY_SLOT("__rmul__", deltaReflected<deltaMultiply>, "($self, value, /)"),
    PY_SLOT("__mod__", deltaRemainder, "($self, value, /)"),
    PY_SLOT("__rmod__", deltaReflected<deltaRemainder>, "($self, value, /)"),
    PY_SLOT("__divmod__", deltaDivMod, "($self, value, /)"),
    PY_SLOT("__rdivmod__", deltaReflected<deltaDivMod>, "($self, value, /)"),
    PY_SLOT("__floordiv__", deltaFloorDivide, "($self, value, /)"),
    PY_SLOT("__rfloordiv__", deltaReflected<deltaFloorDivide>, "($self, value, /)"),
    PY_SLOT("__truediv__", deltaTrueDivide, "($self, value, /)"),
    PY_SLOT("__rtruediv__", deltaReflected<deltaTrueDivide>, "($self, value, /)"),
    PY_SLOT("__neg__", deltaNegative, "($self, /)"),
    PY_SLOT("__pos__", deltaPositive, "($self, /)"),
    PY_SLOT("__abs__", deltaAbsolute, "($self, /)"),
    PY_METHOD("total_seconds", deltaTotalSeconds, "($self, /)"),
    PY_METHOD("__reduce__", deltaReduce, "($self, /)"),
};

static constexpr NativeGetSet deltaGetSets[] = {
    PY_MEMBER("days", deltaGetDays),
    PY_MEMBER("seconds", deltaGetSeconds),
    PY_MEMBER("microseconds", deltaGetMicroseconds),
};

constexpr TypeDef deltaTypeDef {
    "datetime.timedelta"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, deltaMethods, deltaGetSets, { }
};

} // namespace DateTime
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
