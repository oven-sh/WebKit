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
#include "PythonDateTime.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonIO.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonText.h"
#include <wtf/CheckedArithmetic.h>

// datetime.timedelta

namespace JSC { namespace Python {

static constexpr int64_t microsecondsPerMillisecond = 1000;
static constexpr int64_t microsecondsPerSecond = 1000000;
static constexpr int64_t microsecondsPerMinute = 60 * microsecondsPerSecond;
static constexpr int64_t microsecondsPerHour = 60 * microsecondsPerMinute;
static constexpr int64_t microsecondsInDay = 24 * microsecondsPerHour;
static constexpr int64_t microsecondsPerWeek = 7 * microsecondsInDay;
static constexpr int secondsInDay = 24 * 3600;

JSValue newTimeDelta(JSGlobalObject* globalObject, int days, int seconds, int microseconds, bool normalize, PyType* type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (normalize)
        normalizeDelta(days, seconds, microseconds);
    ASSERT(0 <= seconds && seconds < secondsInDay);
    ASSERT(0 <= microseconds && microseconds < 1000000);
    // check_delta_day_range()
    if (days < -maxDeltaDays || days > maxDeltaDays)
        return raise(globalObject, scope, BuiltinType::OverflowError, concatenate("days="_s, days, "; must have magnitude <= "_s, maxDeltaDays));

    auto& state = dateTimeModuleState(globalObject);
    if (!type)
        type = state.deltaType.get();
    // look_up_delta()
    if (!days && !seconds && !microseconds && type == state.deltaType.get() && state.zeroDelta)
        return state.zeroDelta.get();

    auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<TimeDeltaState>());
    auto& self = object->state<TimeDeltaState>();
    self.days = days;
    self.seconds = seconds;
    self.microseconds = microseconds;
    return object;
}

namespace {

// An int and nothing derived from it, so that what is done with it is what int does
bool isPlainInt(JSValue value) { return isInt(value); }

JSValue multiplyValues(JSGlobalObject* globalObject, JSValue left, JSValue right) { return binaryOperation(globalObject, BinaryOperator::Mult, false, left, right); }
JSValue addValues(JSGlobalObject* globalObject, JSValue left, JSValue right) { return binaryOperation(globalObject, BinaryOperator::Add, false, left, right); }

// delta_to_microseconds(): an int
JSValue toMicroseconds(JSGlobalObject* globalObject, const TimeDeltaState& self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    CheckedInt64 total = static_cast<int64_t>(self.days) * secondsInDay + self.seconds;
    total *= microsecondsPerSecond;
    total += self.microseconds;
    if (!total.hasOverflowed()) [[likely]]
        return intFromInt64(globalObject, total.value());
    // There are more in a hundred million days than a word has room for.
    JSValue seconds = intFromInt64(globalObject, static_cast<int64_t>(self.days) * secondsInDay + self.seconds);
    JSValue microseconds = multiplyValues(globalObject, seconds, jsNumber(static_cast<int>(microsecondsPerSecond)));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, addValues(globalObject, microseconds, jsNumber(self.microseconds)));
}

// checked_divmod(): a tuple of two
PyTuple* checkedDivmod(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = divmod(globalObject, left, right);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!isTuple(result)) {
        raiseTypeError(globalObject, scope, concatenate("divmod() returned non-tuple (type "_s, typeName(globalObject, result), ')'));
        return nullptr;
    }
    if (asTuple(result)->length() != 2) {
        raiseTypeError(globalObject, scope, concatenate("divmod() returned a tuple of size "_s, asTuple(result)->length()));
        return nullptr;
    }
    return asTuple(result);
}

// microseconds_to_delta_ex()
JSValue fromMicroseconds(JSGlobalObject* globalObject, JSValue microseconds, PyType* type = nullptr)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isPlainInt(microseconds)) {
        if (auto total = tryInt64(microseconds)) [[likely]] {
            // No more days than there is room for in an int, or than a timedelta can have
            int64_t seconds = *total / microsecondsPerSecond;
            int64_t us = *total % microsecondsPerSecond;
            if (us < 0) {
                us += microsecondsPerSecond;
                --seconds;
            }
            int64_t days = seconds / secondsInDay;
            int64_t s = seconds % secondsInDay;
            if (s < 0) {
                s += secondsInDay;
                --days;
            }
            static_assert(std::numeric_limits<int64_t>::max() / microsecondsInDay < maxDeltaDays);
            RELEASE_AND_RETURN(scope, newTimeDelta(globalObject, static_cast<int>(days), static_cast<int>(s), static_cast<int>(us), false, type));
        }
    }

    auto raiseBadDivmod = [&] { return raiseTypeError(globalObject, scope, "divmod() returned a value out of range"_s); };
    PyTuple* tuple = checkedDivmod(globalObject, microseconds, jsNumber(static_cast<int>(microsecondsPerSecond)));
    RETURN_IF_EXCEPTION(scope, { });
    auto us = toCInt(globalObject, tuple->at(1));
    RETURN_IF_EXCEPTION(scope, { });
    if (!(0 <= *us && *us < 1000000))
        return raiseBadDivmod();

    tuple = checkedDivmod(globalObject, tuple->at(0), jsNumber(secondsInDay));
    RETURN_IF_EXCEPTION(scope, { });
    auto s = toCInt(globalObject, tuple->at(1));
    RETURN_IF_EXCEPTION(scope, { });
    if (!(0 <= *s && *s < secondsInDay))
        return raiseBadDivmod();

    auto days = toCInt(globalObject, tuple->at(0));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, newTimeDelta(globalObject, *days, *s, *us, false, type));
}

// What an int, or an instance of a class derived from int, is as an int
JSValue plainIntOf(const Number& number)
{
    ASSERT(number.isInt());
    return number.kind == Number::Kind::Small ? jsNumber(number.small) : JSValue(number.big);
}

// divide_nearest(), which is the quotient of _PyLong_DivmodNear(): the int nearest to m / n, and the even one of two that are as near. It goes by what ints they are, whatever their classes make of operators.
JSValue divideNearest(JSGlobalObject* globalObject, JSValue m, JSValue n)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Number dividend = classify(m);
    Number divisor = classify(n);
    if (!dividend.isInt() || !divisor.isInt())
        return raiseTypeError(globalObject, scope, "non-integer arguments in division"_s);
    JSValue a = plainIntOf(dividend);
    JSValue b = plainIntOf(divisor);
    JSValue pair = divmod(globalObject, a, b);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue quotient = asTuple(pair)->at(0);
    JSValue remainder = asTuple(pair)->at(1);
    // Up if r / b > 0.5, or if r / b == 0.5 and q is odd. The first is 2 * r > b if b is more than nothing, and 2 * r < b if it is less.
    JSValue twice = multiplyValues(globalObject, remainder, jsNumber(2));
    RETURN_IF_EXCEPTION(scope, { });
    int order = compareInts(twice, b);
    bool divisorIsNegative = compareInts(b, jsNumber(0)) < 0;
    bool isMoreThanHalf = divisorIsNegative ? order < 0 : order > 0;
    if (isMoreThanHalf || (!order && (lowBitsOfInt(quotient) & 1)))
        RELEASE_AND_RETURN(scope, addValues(globalObject, quotient, jsNumber(1)));
    return quotient;
}

// multiply_int_timedelta()
JSValue multiplyByInt(JSGlobalObject* globalObject, JSValue integer, const TimeDeltaState& delta)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue microseconds = toMicroseconds(globalObject, delta);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue product = multiplyValues(globalObject, integer, microseconds);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, fromMicroseconds(globalObject, product));
}

// get_float_as_integer_ratio()
PyTuple* floatAsIntegerRatioOf(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue ratio = callMethodNamed(globalObject, value, globalObject->vm().pythonNames().attribute_as_integer_ratio);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!isTuple(ratio)) {
        raiseTypeError(globalObject, scope, concatenate("unexpected return type from as_integer_ratio(): expected tuple, not '"_s, typeName(globalObject, ratio), '\''));
        return nullptr;
    }
    if (asTuple(ratio)->length() != 2) {
        raiseValueError(globalObject, scope, "as_integer_ratio() must return a 2-tuple"_s);
        return nullptr;
    }
    return asTuple(ratio);
}

// multiply_truedivide_timedelta_float()
JSValue multiplyOrDivideByFloat(JSGlobalObject* globalObject, const TimeDeltaState& delta, JSValue value, bool divides)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue microseconds = toMicroseconds(globalObject, delta);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* ratio = floatAsIntegerRatioOf(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue product = multiplyValues(globalObject, microseconds, ratio->at(divides));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = divideNearest(globalObject, product, ratio->at(!divides));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, fromMicroseconds(globalObject, result));
}

// What two timedeltas come to with an operator of int's between them: divide_timedelta_timedelta(), truedivide_timedelta_timedelta() and part of delta_remainder()
JSValue operateOnMicroseconds(JSGlobalObject* globalObject, BinaryOperator op, const TimeDeltaState& left, const TimeDeltaState& right)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue leftMicroseconds = toMicroseconds(globalObject, left);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue rightMicroseconds = toMicroseconds(globalObject, right);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, binaryOperation(globalObject, op, false, leftMicroseconds, rightMicroseconds));
}

// delta_add() and delta_subtract()
PYTHON_NATIVE(deltaAddOrSubtract)
{
    NATIVE_PROLOGUE();
    auto [leftValue, rightValue] = operandsOfSlot(callFrame);
    auto* left = tryTimeDelta(leftValue);
    auto* right = tryTimeDelta(rightValue);
    if (!left || !right)
        RETURN_NOT_IMPLEMENTED();
    // Neither can be too much for an int, each part being within its bounds.
    int sign = unpack<bool>(callFrame, 1) ? -1 : 1;
    RELEASE_AND_RETURN(scope, JSValue::encode(newTimeDelta(globalObject, left->days + sign * right->days, left->seconds + sign * right->seconds, left->microseconds + sign * right->microseconds, true)));
}

PYTHON_NATIVE(deltaNegative)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(negateTimeDelta(globalObject, stateOf<TimeDeltaState>(args[0]))));
}

PYTHON_NATIVE(deltaPositive)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<TimeDeltaState>(args[0]);
    RELEASE_AND_RETURN(scope, JSValue::encode(newTimeDelta(globalObject, self.days, self.seconds, self.microseconds, false)));
}

PYTHON_NATIVE(deltaAbsolute)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<TimeDeltaState>(args[0]);
    if (self.days < 0)
        RELEASE_AND_RETURN(scope, JSValue::encode(negateTimeDelta(globalObject, self)));
    RELEASE_AND_RETURN(scope, JSValue::encode(newTimeDelta(globalObject, self.days, self.seconds, self.microseconds, false)));
}

PYTHON_NATIVE(deltaBool)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(jsBoolean(!!stateOf<TimeDeltaState>(args[0])));
}

// delta_richcompare()
PYTHON_NATIVE(deltaCompare)
{
    NATIVE_PROLOGUE();
    auto* other = tryTimeDelta(args[1]);
    if (!other)
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(comparisonResult(compareTimeDeltas(stateOf<TimeDeltaState>(args[0]), *other), unpack<ComparisonOperator>(callFrame, 0)));
}

PYTHON_NATIVE(deltaHash)
{
    NATIVE_PROLOGUE();
    int64_t hash = hashOfTimeDelta(globalObject, stateOf<TimeDeltaState>(args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, hash));
}

// delta_multiply()
PYTHON_NATIVE(deltaMultiply)
{
    NATIVE_PROLOGUE();
    auto [left, right] = operandsOfSlot(callFrame);
    // One of them is a timedelta. If both are, it is the one on the left that is multiplied, by what is neither an int nor a float.
    auto* delta = tryTimeDelta(left);
    JSValue factor = delta ? right : left;
    if (!delta)
        delta = tryTimeDelta(right);
    Number number = classify(factor);
    if (number.isInt())
        RELEASE_AND_RETURN(scope, JSValue::encode(multiplyByInt(globalObject, factor, *delta)));
    if (number.kind == Number::Kind::Float)
        RELEASE_AND_RETURN(scope, JSValue::encode(multiplyOrDivideByFloat(globalObject, *delta, factor, false)));
    RETURN_NOT_IMPLEMENTED();
}

// delta_divide()
PYTHON_NATIVE(deltaFloorDivide)
{
    NATIVE_PROLOGUE();
    auto [left, right] = operandsOfSlot(callFrame);
    auto* delta = tryTimeDelta(left);
    if (!delta)
        RETURN_NOT_IMPLEMENTED();
    if (classify(right).isInt()) {
        // divide_timedelta_int()
        JSValue microseconds = toMicroseconds(globalObject, *delta);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue quotient = binaryOperation(globalObject, BinaryOperator::FloorDiv, false, microseconds, right);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(fromMicroseconds(globalObject, quotient)));
    }
    if (auto* divisor = tryTimeDelta(right))
        RELEASE_AND_RETURN(scope, JSValue::encode(floorDivideTimeDeltas(globalObject, *delta, *divisor)));
    RETURN_NOT_IMPLEMENTED();
}

// delta_truedivide()
PYTHON_NATIVE(deltaTrueDivide)
{
    NATIVE_PROLOGUE();
    auto [left, right] = operandsOfSlot(callFrame);
    auto* delta = tryTimeDelta(left);
    if (!delta)
        RETURN_NOT_IMPLEMENTED();
    if (auto* divisor = tryTimeDelta(right))
        RELEASE_AND_RETURN(scope, JSValue::encode(operateOnMicroseconds(globalObject, BinaryOperator::Div, *delta, *divisor)));
    Number number = classify(right);
    if (number.kind == Number::Kind::Float)
        RELEASE_AND_RETURN(scope, JSValue::encode(multiplyOrDivideByFloat(globalObject, *delta, right, true)));
    if (number.isInt()) {
        // truedivide_timedelta_int()
        JSValue microseconds = toMicroseconds(globalObject, *delta);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue quotient = divideNearest(globalObject, microseconds, right);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(fromMicroseconds(globalObject, quotient)));
    }
    RETURN_NOT_IMPLEMENTED();
}

// delta_remainder()
PYTHON_NATIVE(deltaRemainder)
{
    NATIVE_PROLOGUE();
    auto [leftValue, rightValue] = operandsOfSlot(callFrame);
    auto* left = tryTimeDelta(leftValue);
    auto* right = tryTimeDelta(rightValue);
    if (!left || !right)
        RETURN_NOT_IMPLEMENTED();
    JSValue remainder = operateOnMicroseconds(globalObject, BinaryOperator::Mod, *left, *right);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(fromMicroseconds(globalObject, remainder)));
}

// delta_divmod()
PYTHON_NATIVE(deltaDivmod)
{
    NATIVE_PROLOGUE();
    auto [leftValue, rightValue] = operandsOfSlot(callFrame);
    auto* left = tryTimeDelta(leftValue);
    auto* right = tryTimeDelta(rightValue);
    if (!left || !right)
        RETURN_NOT_IMPLEMENTED();
    JSValue leftMicroseconds = toMicroseconds(globalObject, *left);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue rightMicroseconds = toMicroseconds(globalObject, *right);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* pair = checkedDivmod(globalObject, leftMicroseconds, rightMicroseconds);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue remainder = fromMicroseconds(globalObject, pair->at(1));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { pair->at(0), remainder }));
}

// PyLong_FromDouble()
JSValue intFromWholeDouble(JSGlobalObject* globalObject, double value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (std::isinf(value))
        return raise(globalObject, scope, BuiltinType::OverflowError, "cannot convert float infinity to integer"_s);
    if (std::isnan(value))
        return raiseValueError(globalObject, scope, "cannot convert float NaN to integer"_s);
    RELEASE_AND_RETURN(scope, intFromDouble(globalObject, value));
}

// accum(): `soFar` and `number * factor`, as an int. What that leaves out, being less than a microsecond, is added to `leftover`.
JSValue accumulate(JSGlobalObject* globalObject, ASCIILiteral tag, JSValue soFar, JSValue given, int64_t factor, double& leftover)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Number number = classify(given);
    if (number.isInt()) {
        if (number.kind == Number::Kind::Small && (isPlainInt(given) || given.isBoolean())) [[likely]] {
            if (auto sum = tryInt64(soFar)) {
                CheckedInt64 total = factor;
                total *= number.small;
                total += *sum;
                if (!total.hasOverflowed())
                    return intFromInt64(globalObject, total.value());
            }
        }
        JSValue product = multiplyValues(globalObject, given, intFromInt64(globalObject, factor));
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, addValues(globalObject, soFar, product));
    }

    if (number.kind == Number::Kind::Float) {
        // It is taken apart into what is whole and what is not. The whole part times the factor is exact. The rest times the factor is taken apart again, and what is not whole of that is left over.
        double whole;
        double fraction = std::modf(number.real, &whole);
        JSValue x = intFromWholeDouble(globalObject, whole);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue product = multiplyValues(globalObject, x, intFromInt64(globalObject, factor));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue sum = addValues(globalObject, soFar, product);
        RETURN_IF_EXCEPTION(scope, { });
        if (!fraction)
            return sum;
        // Nothing has been lost so far. From here on a little may be.
        fraction = std::modf(static_cast<double>(factor) * fraction, &whole);
        x = intFromWholeDouble(globalObject, whole);
        RETURN_IF_EXCEPTION(scope, { });
        leftover += fraction;
        RELEASE_AND_RETURN(scope, addValues(globalObject, sum, x));
    }

    return raiseTypeError(globalObject, scope, concatenate("unsupported type for timedelta "_s, tag, " component: "_s, typeName(globalObject, given)));
}

// delta_new()
PYTHON_NATIVE(deltaNew)
{
    NATIVE_PROLOGUE();
    // In the order that they are added up in, which is not the order that they are given in
    struct Part {
        unsigned index;
        ASCIILiteral tag;
        int64_t factor;
    };
    static constexpr Part parts[] = {
        { 3, "microseconds"_s, 1 },
        { 4, "milliseconds"_s, microsecondsPerMillisecond },
        { 2, "seconds"_s, microsecondsPerSecond },
        { 5, "minutes"_s, microsecondsPerMinute },
        { 6, "hours"_s, microsecondsPerHour },
        { 1, "days"_s, microsecondsInDay },
        { 7, "weeks"_s, microsecondsPerWeek },
    };
    JSValue sum = jsNumber(0);
    double leftover = 0;
    for (auto& part : parts) {
        if (JSValue given = args.at(part.index)) {
            sum = accumulate(globalObject, part.tag, sum, given, part.factor, leftover);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    if (leftover) {
        // To the nearest microsecond
        double whole = std::round(leftover);
        if (std::abs(whole - leftover) == 0.5) {
            // It is half way between two, and goes to whichever makes the sum even.
            JSValue lowBit = binaryOperation(globalObject, BinaryOperator::BitAnd, false, sum, jsNumber(1));
            RETURN_IF_EXCEPTION(scope, { });
            bool isOdd = isTrue(globalObject, lowBit);
            RETURN_IF_EXCEPTION(scope, { });
            whole = 2.0 * std::round((leftover + isOdd) * 0.5) - isOdd;
        }
        sum = addValues(globalObject, sum, intFromInt64(globalObject, static_cast<long>(whole)));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(fromMicroseconds(globalObject, sum, asType(args[0]))));
}

// delta_repr()
PYTHON_NATIVE(deltaRepr)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<TimeDeltaState>(args[0]);
    TextBuilder builder;
    builder.append(typeName(globalObject, args[0]), '(');
    ASCIILiteral separator = ""_s;
    if (self.days) {
        builder.append("days="_s, self.days);
        separator = ", "_s;
    }
    if (self.seconds) {
        builder.append(separator, "seconds="_s, self.seconds);
        separator = ", "_s;
    }
    if (self.microseconds)
        builder.append(separator, "microseconds="_s, self.microseconds);
    if (!self)
        builder.append('0');
    builder.append(')');
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, builder.tryFinish())));
}

// delta_str()
PYTHON_NATIVE(deltaStr)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<TimeDeltaState>(args[0]);
    int seconds;
    int minutes = floorDivide(self.seconds, 60, seconds);
    int hours = floorDivide(minutes, 60, minutes);
    StringBuilder builder;
    if (self.days)
        builder.append(self.days, " day"_s, self.days == 1 || self.days == -1 ? ""_s : "s"_s, ", "_s);
    builder.append(hours, ':', pad('0', 2, minutes), ':', pad('0', 2, seconds));
    if (self.microseconds)
        builder.append('.', pad('0', 6, self.microseconds));
    return JSValue::encode(jsString(vm, builder.toString()));
}

// delta_getstate()
PyTuple* stateOfDelta(JSGlobalObject* globalObject, const TimeDeltaState& self)
{
    return PyTuple::create(globalObject, { jsNumber(self.days), jsNumber(self.seconds), jsNumber(self.microseconds) });
}

PYTHON_NATIVE(deltaTotalSeconds)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(totalSecondsOfTimeDelta(globalObject, stateOf<TimeDeltaState>(args[0]))));
}

// delta_reduce()
PYTHON_NATIVE(deltaReduce)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, args[0])->object(), stateOfDelta(globalObject, stateOf<TimeDeltaState>(args[0])) }));
}

} // namespace

int compareTimeDeltas(const TimeDeltaState& self, const TimeDeltaState& other)
{
    if (int difference = self.days - other.days)
        return difference;
    if (int difference = self.seconds - other.seconds)
        return difference;
    return self.microseconds - other.microseconds;
}

JSValue negateTimeDelta(JSGlobalObject* globalObject, const TimeDeltaState& self) { return newTimeDelta(globalObject, -self.days, -self.seconds, -self.microseconds, true); }

JSValue subtractTimeDeltas(JSGlobalObject* globalObject, const TimeDeltaState& left, const TimeDeltaState& right)
{
    return newTimeDelta(globalObject, left.days - right.days, left.seconds - right.seconds, left.microseconds - right.microseconds, true);
}

JSValue floorDivideTimeDeltas(JSGlobalObject* globalObject, const TimeDeltaState& left, const TimeDeltaState& right) { return operateOnMicroseconds(globalObject, BinaryOperator::FloorDiv, left, right); }

JSValue totalSecondsOfTimeDelta(JSGlobalObject* globalObject, const TimeDeltaState& self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue microseconds = toMicroseconds(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, binaryOperation(globalObject, BinaryOperator::Div, false, microseconds, jsNumber(static_cast<int>(microsecondsPerSecond))));
}

int64_t hashOfTimeDelta(JSGlobalObject* globalObject, TimeDeltaState& self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (self.hash == -1) {
        int64_t computed = hash(globalObject, stateOfDelta(globalObject, self));
        RETURN_IF_EXCEPTION(scope, -1);
        self.hash = computed;
    }
    return self.hash;
}

void initializeTimeDeltaType(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    PyType* type = createBuiltinType(globalObject, "datetime.timedelta"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    dateTimeModuleState(globalObject).deltaType.set(vm, realm, type);
    addMethods(globalObject, type, {
        { "__new__"_s, deltaNew, Kind::New, 0, "__new__($type, /, days=0, seconds=0, microseconds=0, milliseconds=0, minutes=0, hours=0, weeks=0)"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__repr__"_s, deltaRepr },
        { "__str__"_s, deltaStr },
        { "__hash__"_s, deltaHash },
        { "__add__"_s, deltaAddOrSubtract, Kind::Wrapper, pack(false, false) },
        { "__radd__"_s, deltaAddOrSubtract, Kind::Wrapper, pack(true, false) },
        { "__sub__"_s, deltaAddOrSubtract, Kind::Wrapper, pack(false, true) },
        { "__rsub__"_s, deltaAddOrSubtract, Kind::Wrapper, pack(true, true) },
        { "__mul__"_s, deltaMultiply, Kind::Wrapper, pack(false) },
        { "__rmul__"_s, deltaMultiply, Kind::Wrapper, pack(true) },
        { "__mod__"_s, deltaRemainder, Kind::Wrapper, pack(false) },
        { "__rmod__"_s, deltaRemainder, Kind::Wrapper, pack(true) },
        { "__divmod__"_s, deltaDivmod, Kind::Wrapper, pack(false) },
        { "__rdivmod__"_s, deltaDivmod, Kind::Wrapper, pack(true) },
        { "__floordiv__"_s, deltaFloorDivide, Kind::Wrapper, pack(false) },
        { "__rfloordiv__"_s, deltaFloorDivide, Kind::Wrapper, pack(true) },
        { "__truediv__"_s, deltaTrueDivide, Kind::Wrapper, pack(false) },
        { "__rtruediv__"_s, deltaTrueDivide, Kind::Wrapper, pack(true) },
        { "__neg__"_s, deltaNegative },
        { "__pos__"_s, deltaPositive },
        { "__abs__"_s, deltaAbsolute },
        { "__bool__"_s, deltaBool },
        { "total_seconds"_s, deltaTotalSeconds },
        { "__reduce__"_s, deltaReduce },
    });
    addComparisons(globalObject, type, deltaCompare);
    addMember(globalObject, type, "days"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<TimeDeltaState>(self).days); });
    addMember(globalObject, type, "seconds"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<TimeDeltaState>(self).seconds); });
    addMember(globalObject, type, "microseconds"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<TimeDeltaState>(self).microseconds); });
}

} } // namespace JSC::Python
