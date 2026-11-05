/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyDecimal.h"

#include "JSCInlines.h"
#include "PyDateTime.h"
#include "PyDict.h"
#include "PyList.h"
#include "PyTuple.h"
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Decimal {

// ---- Signals as Python has them ----

static constexpr ASCIILiteral invalidSignalsMessage =
    "valid values for signals are:\n"
    "  [InvalidOperation, FloatOperation, DivisionByZero,\n"
    "   Overflow, Underflow, Subnormal, Inexact, Rounded,\n"
    "   Clamped]"_s;

static constexpr ASCIILiteral invalidRoundingMessage =
    "valid values for rounding are:\n"
    "  [ROUND_CEILING, ROUND_FLOOR, ROUND_UP, ROUND_DOWN,\n"
    "   ROUND_HALF_UP, ROUND_HALF_DOWN, ROUND_HALF_EVEN,\n"
    "   ROUND_05UP]"_s;

static JSValue raiseKeyErrorWithMessage(JSGlobalObject* globalObject, ASCIILiteral message)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseKeyError(globalObject, scope, makeStr(vm, message));
}

// The conditions of the signal that the class is. False: a KeyError.
static bool conditionsOfClass(JSGlobalObject* globalObject, State& state, JSValue key, uint32_t& conditions)
{
    for (unsigned i = 0; i < numberOfSignals; ++i) {
        if (key == JSValue(state.signals[i].get())) {
            conditions = conditionsOfSignal[i];
            return true;
        }
    }
    raiseKeyErrorWithMessage(globalObject, invalidSignalsMessage);
    return false;
}

static JSArray* signalsAsList(JSGlobalObject* globalObject, State& state, uint32_t conditions)
{
    JSValue classes[numberOfSignals];
    unsigned count = 0;
    for (unsigned i = 0; i < numberOfSignals; ++i) {
        if (conditions & conditionsOfSignal[i])
            classes[count++] = state.signals[i].get();
    }
    return newList(globalObject, std::span<const JSValue> { classes, count });
}

static bool conditionsOfList(JSGlobalObject* globalObject, State& state, JSArray* list, uint32_t& conditions)
{
    conditions = 0;
    for (unsigned i = 0; i < listLength(list); ++i) {
        uint32_t some;
        if (!conditionsOfClass(globalObject, state, listGet(list, i), some))
            return false;
        conditions |= some;
    }
    return true;
}

enum class DictOfSignals : uint8_t { Valid, Invalid, Raised };
// A dict with the nine signals as its keys. Invalid: it is not that, which has been raised.
static DictOfSignals conditionsOfDict(JSGlobalObject* globalObject, State& state, JSValue value, uint32_t& conditions)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* dict = PyDict::tryFrom(value);
    if (!dict) {
        raiseTypeError(globalObject, scope, "argument must be a signal dict"_s);
        return DictOfSignals::Invalid;
    }
    if (dict->size() != numberOfSignals) {
        scope.release();
        raiseKeyErrorWithMessage(globalObject, "invalid signal dict"_s);
        return DictOfSignals::Invalid;
    }
    conditions = 0;
    for (unsigned i = 0; i < numberOfSignals; ++i) {
        JSValue item = dict->get(globalObject, state.signals[i].get());
        RETURN_IF_EXCEPTION(scope, DictOfSignals::Raised);
        if (!item) {
            scope.release();
            raiseKeyErrorWithMessage(globalObject, "invalid signal dict"_s);
            return DictOfSignals::Invalid;
        }
        bool isSet = isTrue(globalObject, item);
        RETURN_IF_EXCEPTION(scope, DictOfSignals::Raised);
        if (isSet)
            conditions |= conditionsOfSignal[i];
    }
    return DictOfSignals::Valid;
}

bool roundingArgument(JSGlobalObject* globalObject, State& state, JSValue value, Rounding& rounding)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSString* string = tryStrValue(value)) {
        for (unsigned i = 0; i < numberOfRoundings; ++i) {
            if (string == state.roundings[i].get()) {
                rounding = static_cast<Rounding>(i);
                return true;
            }
        }
        String text = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        for (unsigned i = 0; i < numberOfRoundings; ++i) {
            if (text == namesOfRoundings[i]) {
                rounding = static_cast<Rounding>(i);
                return true;
            }
        }
    }
    raiseTypeError(globalObject, scope, invalidRoundingMessage);
    return false;
}

// ---- context.traps and context.flags ----
//
// A mapping from the signals to bools that reads and writes the signals of a context. Field 0 is the cell that has them; it is empty or None in
// a mapping that a program made by calling the type, or parted from its context by calling __init__(). `number` says which of the two it is.

enum SignalDictKind : int64_t { SignalDictOfTraps, SignalDictOfFlags };

static PyRecord* newSignalDict(JSGlobalObject* globalObject, PyType* type, PyDecimalSignals* signals, SignalDictKind kind)
{
    PyRecord* mapping = PyRecord::create(globalObject, type, 1);
    if (signals)
        mapping->set(getVM(globalObject), 0, signals);
    mapping->number = kind;
    return mapping;
}

static ALWAYS_INLINE uint32_t* tryConditionsOfSignalDict(PyRecord* mapping)
{
    JSValue signals = mapping->get(0);
    if (!signals || !signals.isCell()) [[unlikely]]
        return nullptr;
    return mapping->number == SignalDictOfTraps ? &PyDecimalSignals::from(signals)->traps : &PyDecimalSignals::from(signals)->status;
}

// Null: a ValueError.
static uint32_t* conditionsOfSignalDict(JSGlobalObject* globalObject, JSValue self)
{
    uint32_t* conditions = tryConditionsOfSignalDict(PyRecord::from(self));
    if (!conditions) [[unlikely]] {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseValueError(globalObject, scope, "invalid signal dict"_s);
    }
    return conditions;
}

static JSValue signalDictNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    return newSignalDict(globalObject, PyType::from(cls), nullptr, SignalDictOfFlags);
}

static JSValue signalDictInit(JSGlobalObject* globalObject, JSValue self, const CallArgs&)
{
    PyRecord::from(self)->set(getVM(globalObject), 0, jsUndefined());
    return jsUndefined();
}

static JSValue signalDictLen(JSGlobalObject* globalObject, JSValue self)
{
    if (!conditionsOfSignalDict(globalObject, self))
        return { };
    return jsNumber(numberOfSignals);
}

static JSValue signalDictIter(JSGlobalObject* globalObject, JSValue self)
{
    if (!conditionsOfSignalDict(globalObject, self))
        return { };
    return getIter(globalObject, stateOf(globalObject).signalTuple.get());
}

static JSValue signalDictGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    uint32_t* conditions = conditionsOfSignalDict(globalObject, self);
    uint32_t asked;
    if (!conditions || !conditionsOfClass(globalObject, stateOf(globalObject), key, asked))
        return { };
    return jsBoolean(*conditions & asked);
}

static JSValue signalDictSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t changed;
    if (failed(scope, conditionsOfSignalDict(globalObject, self)) || failed(scope, conditionsOfClass(globalObject, stateOf(globalObject), key, changed)))
        return { };
    bool isSet = isTrue(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    // Whatever __bool__ did, the mapping may have been parted from its context.
    uint32_t* conditions = conditionsOfSignalDict(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (isSet)
        *conditions |= changed;
    else
        *conditions &= ~changed;
    return jsUndefined();
}

static JSValue signalDictDelItem(JSGlobalObject* globalObject, JSValue self, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, conditionsOfSignalDict(globalObject, self)))
        return { };
    return raiseValueError(globalObject, scope, "signal keys cannot be deleted"_s);
}

static JSValue signalDictRepr(JSGlobalObject* globalObject, JSValue self)
{
    uint32_t* conditions = conditionsOfSignalDict(globalObject, self);
    if (!conditions)
        return { };
    StringBuilder builder;
    builder.append('{');
    for (unsigned i = 0; i < numberOfSignals; ++i)
        builder.append(i ? ", "_s : ""_s, "<class 'decimal."_s, namesOfSignals[i], "'>:"_s, *conditions & conditionsOfSignal[i] ? "True"_s : "False"_s);
    builder.append('}');
    return makeStr(globalObject, builder.toString());
}

template<CompareOperator op>
static JSValue signalDictCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    uint32_t* conditions = conditionsOfSignalDict(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* otherMapping = PyRecord::tryFrom(other, state.signalDictMixinType.get());
    uint32_t otherConditions = 0;
    if (otherMapping) {
        uint32_t* those = conditionsOfSignalDict(globalObject, otherMapping);
        RETURN_IF_EXCEPTION(scope, { });
        otherConditions = *those;
    }
    if constexpr (op != CompareOperator::Eq && op != CompareOperator::NotEq)
        return rt.notImplemented();
    if (!otherMapping) {
        if (!PyDict::is(other))
            return rt.notImplemented();
        uint32_t own = *conditions;
        DictOfSignals kind = conditionsOfDict(globalObject, state, other, otherConditions);
        if (failed(scope, kind == DictOfSignals::Valid)) {
            if (kind == DictOfSignals::Raised)
                return { };
            takePendingException(globalObject);
            return rt.notImplemented();
        }
        return jsBoolean((own == otherConditions) == (op == CompareOperator::Eq));
    }
    return jsBoolean((*conditions == otherConditions) == (op == CompareOperator::Eq));
}

static JSValue signalDictCopy(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t* conditions = conditionsOfSignalDict(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    uint32_t snapshot = *conditions;
    State& state = stateOf(globalObject);
    PyDict* dict = PyDict::create(globalObject);
    for (unsigned i = 0; i < numberOfSignals; ++i) {
        dict->set(globalObject, state.signals[i].get(), jsBoolean(snapshot & conditionsOfSignal[i]));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return dict;
}

static constexpr NativeDef signalDictMethods[] = {
    PY_NEW(signalDictNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", signalDictInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_SLOT("__repr__", signalDictRepr, "($self, /)"),
    PY_SLOT("__eq__", signalDictCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", signalDictCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", signalDictCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", signalDictCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", signalDictCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", signalDictCompare<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__iter__", signalDictIter, "($self, /)"),
    PY_SLOT("__len__", signalDictLen, "($self, /)"),
    PY_SLOT("__getitem__", signalDictGetItem, "($self, key, /)"),
    PY_SLOT("__setitem__", signalDictSetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", signalDictDelItem, "($self, key, /)"),
    PY_METHOD("copy", signalDictCopy, "($self, /)"),
};

constexpr TypeDef signalDictMixinTypeDef {
    "decimal.SignalDictMixin"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, signalDictMethods, { }, { }
};

// ---- Making contexts ----

PyDecimalContext* newContext(JSGlobalObject* globalObject, State& state, PyType* type)
{
    VM& vm = getVM(globalObject);
    auto* context = PyDecimalContext::create(vm, state.contextStructure.get(), type);
    auto* signals = PyDecimalSignals::create(vm, state.signalsStructure.get(), state.signalsType.get());
    context->signals.set(vm, context, signals);
    if (auto* defaults = state.defaultContext.get()) {
        context->context = defaults->context;
        signals->traps = defaults->signals->traps;
        signals->status = defaults->signals->status;
    }
    context->traps.set(vm, context, newSignalDict(globalObject, state.signalDictType.get(), signals, SignalDictOfTraps));
    context->flags.set(vm, context, newSignalDict(globalObject, state.signalDictType.get(), signals, SignalDictOfFlags));
    return context;
}

PyDecimalContext* copyContext(JSGlobalObject* globalObject, State& state, PyDecimalContext* original)
{
    PyDecimalContext* copy = newContext(globalObject, state, state.contextType.get());
    copy->context = original->context;
    copy->capitals = original->capitals;
    copy->signals->traps = original->signals->traps;
    copy->signals->status = original->signals->status;
    return copy;
}

// ---- The attributes ----

// As PyLong_AsSsize_t(). False: an exception.
static bool ssizeOf(JSGlobalObject* globalObject, JSValue value, int64_t& result)
{
    if (isInt(value)) [[likely]] {
        if (auto converted = tryInt64(intValue(value))) [[likely]] {
            result = *converted;
            return true;
        }
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isInt(value))
        raiseTypeError(globalObject, scope, "an integer is required"_s);
    else
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
    return false;
}

static void raiseValueErrorWithMessage(JSGlobalObject* globalObject, ASCIILiteral message)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, message);
}

static ALWAYS_INLINE Context& limitsOf(JSValue self) { return PyDecimalContext::from(self)->context; }

static JSValue contextGetPrecision(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, limitsOf(self).precision); }
static JSValue contextGetEmax(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, limitsOf(self).emax); }
static JSValue contextGetEmin(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, limitsOf(self).emin); }
static JSValue contextGetClamp(JSGlobalObject*, JSValue self) { return jsNumber(limitsOf(self).clamp ? 1 : 0); }
static JSValue contextGetCapitals(JSGlobalObject*, JSValue self) { return jsNumber(PyDecimalContext::from(self)->capitals); }
static JSValue contextGetRounding(JSGlobalObject* globalObject, JSValue self) { return stateOf(globalObject).roundings[static_cast<unsigned>(limitsOf(self).rounding)].get(); }

static void contextSetPrecision(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    int64_t number;
    if (!ssizeOf(globalObject, value, number))
        return;
    if (number <= 0 || number > maxPrecision)
        return raiseValueErrorWithMessage(globalObject, "valid range for prec is [1, MAX_PREC]"_s);
    limitsOf(self).precision = number;
}

static void contextSetEmin(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    int64_t number;
    if (!ssizeOf(globalObject, value, number))
        return;
    if (number > 0 || number < minEmin)
        return raiseValueErrorWithMessage(globalObject, "valid range for Emin is [MIN_EMIN, 0]"_s);
    limitsOf(self).emin = number;
}

static void contextSetEmax(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    int64_t number;
    if (!ssizeOf(globalObject, value, number))
        return;
    if (number < 0 || number > maxEmax)
        return raiseValueErrorWithMessage(globalObject, "valid range for Emax is [0, MAX_EMAX]"_s);
    limitsOf(self).emax = number;
}

static void contextSetRounding(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Rounding rounding;
    if (roundingArgument(globalObject, stateOf(globalObject), value, rounding))
        limitsOf(self).rounding = rounding;
}

static void contextSetCapitals(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    int64_t number;
    if (!ssizeOf(globalObject, value, number))
        return;
    if (number != 0 && number != 1)
        return raiseValueErrorWithMessage(globalObject, "valid values for capitals are 0 or 1"_s);
    PyDecimalContext::from(self)->capitals = static_cast<int>(number);
}

static void contextSetClamp(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    int64_t number;
    if (!ssizeOf(globalObject, value, number))
        return;
    if (number != 0 && number != 1)
        return raiseValueErrorWithMessage(globalObject, "valid values for clamp are 0 or 1"_s);
    limitsOf(self).clamp = number;
}

// context.traps = value, context.flags = value: another such mapping, or a dict. `allowsList`: or a list of signals.
static bool setSignals(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, SignalDictKind kind, JSValue value, bool allowsList)
{
    uint32_t conditions;
    JSArray* list = allowsList ? tryAsList(value) : nullptr;
    if (list) {
        if (!conditionsOfList(globalObject, state, list, conditions))
            return false;
    } else if (PyRecord* mapping = PyRecord::tryFrom(value, state.signalDictMixinType.get())) {
        uint32_t* those = conditionsOfSignalDict(globalObject, mapping);
        if (!those)
            return false;
        conditions = *those;
    } else if (conditionsOfDict(globalObject, state, value, conditions) != DictOfSignals::Valid)
        return false;
    (kind == SignalDictOfTraps ? context->signals->traps : context->signals->status) = conditions;
    return true;
}

bool applyContextSettings(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, const ContextSettings& settings)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto isGiven = [](JSValue value) { return value && !isNone(value); };
    auto set = [&](JSValue value, NativeGetSet::Setter setter) {
        if (isGiven(value))
            setter(globalObject, context, value);
        return !scope.exception();
    };
    if (!set(settings.precision, contextSetPrecision) || !set(settings.rounding, contextSetRounding) || !set(settings.emin, contextSetEmin)
        || !set(settings.emax, contextSetEmax) || !set(settings.capitals, contextSetCapitals) || !set(settings.clamp, contextSetClamp))
        return false;
    if (isGiven(settings.traps) && failed(scope, setSignals(globalObject, state, context, SignalDictOfTraps, settings.traps, true)))
        return false;
    if (isGiven(settings.flags) && failed(scope, setSignals(globalObject, state, context, SignalDictOfFlags, settings.flags, true)))
        return false;
    return true;
}

static JSValue contextNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    return newContext(globalObject, stateOf(globalObject), PyType::from(cls));
}

static JSValue contextInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    static constexpr ASCIILiteral names[] = { "prec"_s, "rounding"_s, "Emin"_s, "Emax"_s, "capitals"_s, "clamp"_s, "flags"_s, "traps"_s };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue values[std::size(names)];
    if (failed(scope, DateTime::bindArguments(globalObject, { "Context"_s, names, 0, std::size(names) }, args, values)))
        return { };
    if (failed(scope, applyContextSettings(globalObject, stateOf(globalObject), PyDecimalContext::from(self), { values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7] })))
        return { };
    return jsUndefined();
}

// `name` is not a rope.
static ALWAYS_INLINE bool namesSignalDict(JSString* name, SignalDictKind& kind)
{
    if (name->length() != 5) [[likely]]
        return false;
    StringImpl* text = name->tryGetValueImpl();
    if (WTF::equal(text, "traps"_s)) {
        kind = SignalDictOfTraps;
        return true;
    }
    if (WTF::equal(text, "flags"_s)) {
        kind = SignalDictOfFlags;
        return true;
    }
    return false;
}

static JSValue contextGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    SignalDictKind kind;
    if (namesSignalDict(string, kind)) {
        auto* context = PyDecimalContext::from(self);
        return kind == SignalDictOfTraps ? context->traps.get() : context->flags.get();
    }
    RELEASE_AND_RETURN(scope, genericGetAttrAsGiven(globalObject, self, string, name));
}

static JSValue contextSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue name, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    SignalDictKind kind;
    if (namesSignalDict(string, kind)) {
        if (failed(scope, setSignals(globalObject, stateOf(globalObject), PyDecimalContext::from(self), kind, value, false)))
            return { };
        return jsUndefined();
    }
    genericSetAttrAsGiven(globalObject, self, string, name, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue contextDelAttr(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), "context attributes cannot be deleted"_s);
}

static void appendSignals(StringBuilder& builder, uint32_t conditions)
{
    // In the order of the bits. All that is InvalidOperation is named once, where the first of it is.
    static constexpr ASCIILiteral names[numberOfConditionBits] = {
        "Clamped"_s, "InvalidOperation"_s, "DivisionByZero"_s, "InvalidOperation"_s, "InvalidOperation"_s, "InvalidOperation"_s, "Inexact"_s,
        "InvalidOperation"_s, "InvalidOperation"_s, "InvalidOperation"_s, "FloatOperation"_s, "Overflow"_s, "Rounded"_s, "Subnormal"_s, "Underflow"_s
    };
    builder.append('[');
    bool isFirst = true;
    bool hasInvalidOperation = false;
    for (unsigned bit = 0; bit < numberOfConditionBits; ++bit) {
        uint32_t condition = conditions & (1u << bit);
        if (!condition)
            continue;
        if (condition & invalidOperationSignal) {
            if (hasInvalidOperation)
                continue;
            hasInvalidOperation = true;
        }
        builder.append(isFirst ? ""_s : ", "_s, names[bit]);
        isFirst = false;
    }
    builder.append(']');
}

static JSValue contextRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* context = PyDecimalContext::from(self);
    const Context& limits = context->context;
    StringBuilder builder;
    builder.append("Context(prec="_s, limits.precision, ", rounding="_s, namesOfRoundings[static_cast<unsigned>(limits.rounding)], ", Emin="_s, limits.emin, ", Emax="_s, limits.emax,
        ", capitals="_s, context->capitals, ", clamp="_s, limits.clamp ? 1 : 0, ", flags="_s);
    appendSignals(builder, context->signals->status);
    builder.append(", traps="_s);
    appendSignals(builder, context->signals->traps);
    builder.append(')');
    return makeStr(globalObject, builder.toString());
}

static JSValue contextCopy(JSGlobalObject* globalObject, JSValue self)
{
    return copyContext(globalObject, stateOf(globalObject), PyDecimalContext::from(self));
}

static JSValue contextReduce(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    auto* context = PyDecimalContext::from(self);
    const Context& limits = context->context;
    JSValue flags = signalsAsList(globalObject, state, context->signals->status);
    JSValue traps = signalsAsList(globalObject, state, context->signals->traps);
    JSValue precision = makeInt(globalObject, limits.precision);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue emin = makeInt(globalObject, limits.emin);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue emax = makeInt(globalObject, limits.emax);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* arguments = PyTuple::create(globalObject, { precision, makeStr(vm, namesOfRoundings[static_cast<unsigned>(limits.rounding)]), emin, emax,
        jsNumber(context->capitals), jsNumber(limits.clamp ? 1 : 0), flags, traps });
    return PyTuple::create(globalObject, { context->type(), arguments });
}

static JSValue contextEtiny(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, limitsOf(self).etiny()); }
static JSValue contextEtop(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, limitsOf(self).etop()); }

static JSValue contextClearFlags(JSGlobalObject*, JSValue self)
{
    PyDecimalContext::from(self)->signals->status = 0;
    return jsUndefined();
}

static JSValue contextClearTraps(JSGlobalObject*, JSValue self)
{
    PyDecimalContext::from(self)->signals->traps = 0;
    return jsUndefined();
}

// ---- The operations ----

static JSValue contextCreateDecimal(JSGlobalObject* globalObject, JSValue self, JSValue number)
{
    return decimalFromObject(globalObject, stateOf(globalObject), number, PyDecimalContext::from(self));
}

static JSValue contextCreateDecimalFromFloat(JSGlobalObject* globalObject, JSValue self, JSValue number)
{
    return decimalFromFloat(globalObject, stateOf(globalObject), number, PyDecimalContext::from(self), false);
}

template<UnaryOperation operation>
static JSValue contextUnary(JSGlobalObject* globalObject, JSValue self, JSValue x)
{
    return applyUnary<operation>(globalObject, stateOf(globalObject), PyDecimalContext::from(self), x);
}

template<BinaryOperation operation>
static JSValue contextBinary(JSGlobalObject* globalObject, JSValue self, JSValue x, JSValue y)
{
    return applyBinary<operation>(globalObject, stateOf(globalObject), PyDecimalContext::from(self), x, y);
}

// Operations that take no notice of the context, made to look as if they did.
static void applyContext(Number& result, const Number& a, const Context& context, uint32_t& status)
{
    if (result.copy(a, status))
        finalize(result, context, status);
}
static void copyAbsoluteInContext(Number& result, const Number& a, const Context&, uint32_t& status) { copyAbsolute(result, a, status); }
static void copyNegatedInContext(Number& result, const Number& a, const Context&, uint32_t& status) { copyNegated(result, a, status); }

template<bool (*predicate)(const Number&, const Context&)>
static JSValue contextPredicate(JSGlobalObject* globalObject, JSValue self, JSValue x)
{
    auto* context = PyDecimalContext::from(self);
    Number a;
    if (convertOperand(globalObject, stateOf(globalObject), context, x, a, true) != Conversion::Done)
        return { };
    return jsBoolean(predicate(a, context->context));
}

static bool isFinite(const Number& a, const Context&) { return a.isFinite(); }
static bool isInfinite(const Number& a, const Context&) { return a.isInfinite(); }
static bool isNaN(const Number& a, const Context&) { return a.isNaN(); }
static bool isQuietNaN(const Number& a, const Context&) { return a.isQuietNaN(); }
static bool isSignalingNaN(const Number& a, const Context&) { return a.isSignalingNaN(); }
static bool isSigned(const Number& a, const Context&) { return a.isNegative(); }
static bool isZero(const Number& a, const Context&) { return a.isZero(); }
static bool isNormal(const Number& a, const Context& context) { return a.isNormal(context); }
static bool isSubnormal(const Number& a, const Context& context) { return a.isSubnormal(context); }

static JSValue raiseNotDecimal(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "argument must be a Decimal"_s);
}

static JSValue contextIsCanonical(JSGlobalObject* globalObject, JSValue, JSValue x)
{
    if (!PyDecimal::is(x))
        return raiseNotDecimal(globalObject);
    return jsBoolean(true);
}

static JSValue contextCanonical(JSGlobalObject* globalObject, JSValue, JSValue x)
{
    if (!PyDecimal::is(x))
        return raiseNotDecimal(globalObject);
    return x;
}

static JSValue contextCopyDecimal(JSGlobalObject* globalObject, JSValue self, JSValue x)
{
    if (PyDecimal::is(x))
        return x;
    State& state = stateOf(globalObject);
    Number a;
    if (convertOperand(globalObject, state, PyDecimalContext::from(self), x, a, true) != Conversion::Done)
        return { };
    return makeDecimal(globalObject, state, a);
}

static JSValue contextRadix(JSGlobalObject* globalObject, JSValue)
{
    return makeDecimal(globalObject, stateOf(globalObject), Number(0, 10, 0));
}

static JSValue contextNumberClass(JSGlobalObject* globalObject, JSValue self, JSValue x)
{
    auto* context = PyDecimalContext::from(self);
    Number a;
    if (convertOperand(globalObject, stateOf(globalObject), context, x, a, true) != Conversion::Done)
        return { };
    return makeStr(getVM(globalObject), String::fromLatin1(classOf(a, context->context)));
}

template<unsigned notation>
static JSValue contextToString(JSGlobalObject* globalObject, JSValue self, JSValue x)
{
    auto* context = PyDecimalContext::from(self);
    Number a;
    if (convertOperand(globalObject, stateOf(globalObject), context, x, a, true) != Conversion::Done)
        return { };
    return decimalToStr(globalObject, a, notation | (context->capitals ? TextUpperCase : 0));
}

static JSValue contextSameQuantum(JSGlobalObject* globalObject, JSValue self, JSValue x, JSValue y)
{
    State& state = stateOf(globalObject);
    auto* context = PyDecimalContext::from(self);
    Number a;
    Number b;
    if (convertOperand(globalObject, state, context, x, a, true) != Conversion::Done || convertOperand(globalObject, state, context, y, b, true) != Conversion::Done)
        return { };
    return jsBoolean(haveSameQuantum(a, b));
}

static JSValue contextDivMod(JSGlobalObject* globalObject, JSValue self, JSValue x, JSValue y)
{
    return applyDivMod(globalObject, stateOf(globalObject), PyDecimalContext::from(self), x, y, true);
}

static JSValue contextPower(JSGlobalObject* globalObject, JSValue self, JSValue a, JSValue b, JSValue modulo)
{
    return applyPower(globalObject, stateOf(globalObject), PyDecimalContext::from(self), a, b, modulo, true);
}

static JSValue contextFusedMultiplyAdd(JSGlobalObject* globalObject, JSValue self, JSValue x, JSValue y, JSValue z)
{
    return applyFusedMultiplyAdd(globalObject, stateOf(globalObject), PyDecimalContext::from(self), x, y, z);
}

#define PY_CONTEXT_UNARY(name, operation) PY_METHOD_WITH_FLAGS(name, contextUnary<operation>, "($self, x, /)", ParsedByName)
#define PY_CONTEXT_BINARY(name, operation) PY_METHOD_WITH_FLAGS(name, contextBinary<operation>, "($self, x, y, /)", ParsedByName)
#define PY_CONTEXT_PREDICATE(name, predicate) PY_METHOD_WITH_FLAGS(name, contextPredicate<predicate>, "($self, x, /)", ParsedByName)

static constexpr NativeDef contextMethods[] = {
    PY_NEW(contextNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", contextInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__getattribute__", contextGetAttribute, "($self, name, /)"),
    PY_SLOT("__setattr__", contextSetAttr, "($self, name, value, /)"),
    PY_SLOT("__delattr__", contextDelAttr, "($self, name, /)"),
    PY_SLOT("__repr__", contextRepr, "($self, /)"),

    PY_CONTEXT_UNARY("abs", absolute),
    PY_CONTEXT_UNARY("exp", exponential),
    PY_CONTEXT_UNARY("ln", naturalLogarithm),
    PY_CONTEXT_UNARY("log10", commonLogarithm),
    PY_CONTEXT_UNARY("minus", minus),
    PY_CONTEXT_UNARY("next_minus", nextMinus),
    PY_CONTEXT_UNARY("next_plus", nextPlus),
    PY_CONTEXT_UNARY("normalize", reduce),
    PY_CONTEXT_UNARY("plus", plus),
    PY_CONTEXT_UNARY("to_integral", roundToIntegralValue),
    PY_CONTEXT_UNARY("to_integral_exact", roundToIntegralExact),
    PY_CONTEXT_UNARY("to_integral_value", roundToIntegralValue),
    PY_CONTEXT_UNARY("sqrt", squareRoot),

    PY_CONTEXT_BINARY("add", add),
    PY_CONTEXT_BINARY("compare", compare),
    PY_CONTEXT_BINARY("compare_signal", compareSignal),
    PY_CONTEXT_BINARY("divide", divide),
    PY_CONTEXT_BINARY("divide_int", divideInteger),
    PY_METHOD("divmod", contextDivMod, "($self, x, y, /)"),
    PY_CONTEXT_BINARY("max", maximum),
    PY_CONTEXT_BINARY("max_mag", maximumMagnitude),
    PY_CONTEXT_BINARY("min", minimum),
    PY_CONTEXT_BINARY("min_mag", minimumMagnitude),
    PY_CONTEXT_BINARY("multiply", multiply),
    PY_CONTEXT_BINARY("next_toward", nextToward),
    PY_CONTEXT_BINARY("quantize", quantize),
    PY_CONTEXT_BINARY("remainder", remainder),
    PY_CONTEXT_BINARY("remainder_near", remainderNear),
    PY_CONTEXT_BINARY("subtract", subtract),

    PY_METHOD("power", contextPower, "($self, /, a, b, modulo=None)"),
    PY_METHOD_WITH_FLAGS("fma", contextFusedMultiplyAdd, "($self, x, y, z, /)", ParsedByName),

    PY_METHOD("Etiny", contextEtiny, "($self, /)"),
    PY_METHOD("Etop", contextEtop, "($self, /)"),
    PY_METHOD_WITH_FLAGS("radix", contextRadix, "($self, /)", ParsedByName),

    PY_METHOD_WITH_FLAGS("is_canonical", contextIsCanonical, "($self, x, /)", ParsedByName),
    PY_CONTEXT_PREDICATE("is_finite", isFinite),
    PY_CONTEXT_PREDICATE("is_infinite", isInfinite),
    PY_CONTEXT_PREDICATE("is_nan", isNaN),
    PY_CONTEXT_PREDICATE("is_normal", isNormal),
    PY_CONTEXT_PREDICATE("is_qnan", isQuietNaN),
    PY_CONTEXT_PREDICATE("is_signed", isSigned),
    PY_CONTEXT_PREDICATE("is_snan", isSignalingNaN),
    PY_CONTEXT_PREDICATE("is_subnormal", isSubnormal),
    PY_CONTEXT_PREDICATE("is_zero", isZero),

    PY_CONTEXT_UNARY("_apply", applyContext),
    PY_METHOD_WITH_FLAGS("canonical", contextCanonical, "($self, x, /)", ParsedByName),
    PY_CONTEXT_UNARY("copy_abs", copyAbsoluteInContext),
    PY_METHOD_WITH_FLAGS("copy_decimal", contextCopyDecimal, "($self, x, /)", ParsedByName),
    PY_CONTEXT_UNARY("copy_negate", copyNegatedInContext),
    PY_CONTEXT_UNARY("logb", logB),
    PY_CONTEXT_UNARY("logical_invert", logicalInvert),
    PY_METHOD_WITH_FLAGS("number_class", contextNumberClass, "($self, x, /)", ParsedByName),
    PY_METHOD_WITH_FLAGS("to_sci_string", contextToString<TextScientific>, "($self, x, /)", ParsedByName),
    PY_METHOD_WITH_FLAGS("to_eng_string", contextToString<TextEngineering>, "($self, x, /)", ParsedByName),

    PY_CONTEXT_BINARY("compare_total", compareTotalInContext),
    PY_CONTEXT_BINARY("compare_total_mag", compareTotalMagnitudeInContext),
    PY_CONTEXT_BINARY("copy_sign", copySignInContext),
    PY_CONTEXT_BINARY("logical_and", logicalAnd),
    PY_CONTEXT_BINARY("logical_or", logicalOr),
    PY_CONTEXT_BINARY("logical_xor", logicalXor),
    PY_CONTEXT_BINARY("rotate", rotate),
    PY_METHOD_WITH_FLAGS("same_quantum", contextSameQuantum, "($self, x, y, /)", ParsedByName),
    PY_CONTEXT_BINARY("scaleb", scaleB),
    PY_CONTEXT_BINARY("shift", shift),

    PY_METHOD("clear_flags", contextClearFlags, "($self, /)"),
    PY_METHOD("clear_traps", contextClearTraps, "($self, /)"),

    PY_METHOD_WITH_FLAGS("__copy__", contextCopy, "($self, /)", ParsedByName),
    PY_METHOD_WITH_FLAGS("__reduce__", contextReduce, "($self, /)", ParsedByName),
    PY_METHOD_WITH_FLAGS("copy", contextCopy, "($self, /)", ParsedByName),

    PY_METHOD("create_decimal", contextCreateDecimal, "($self, num='0', /)"),
    PY_METHOD_WITH_FLAGS("create_decimal_from_float", contextCreateDecimalFromFloat, "($self, f, /)", ParsedByName),
};

static constexpr NativeGetSet contextGetSets[] = {
    { "prec"_s, contextGetPrecision, contextSetPrecision },
    { "Emax"_s, contextGetEmax, contextSetEmax },
    { "Emin"_s, contextGetEmin, contextSetEmin },
    { "rounding"_s, contextGetRounding, contextSetRounding },
    { "capitals"_s, contextGetCapitals, contextSetCapitals },
    { "clamp"_s, contextGetClamp, contextSetClamp },
};

constexpr TypeDef contextTypeDef {
    "decimal.Context"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, contextMethods, contextGetSets, { }
};

} // namespace Decimal
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
