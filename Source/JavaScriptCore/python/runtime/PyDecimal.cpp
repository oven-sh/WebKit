/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyDecimal.h"

#include "JSBigInt.h"
#include "JSCInlines.h"
#include "PyContextVars.h"
#include "PyDateTime.h"
#include "PyDict.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyTuple.h"
#include "PyUnicode.h"
#include <cmath>
#include <wtf/RAMSize.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Decimal {

// ---- The cells ----

PY_DEFINE_CELL(PyDecimal);
PY_DEFINE_CELL(PyDecimalContext);
PY_DEFINE_CELL(PyDecimalSignals);
const ClassInfo State::s_info = { "PyDecimalState"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(State) };

PyDecimal::PyDecimal(VM& vm, Structure* structure, PyType* type, const Number& number)
    : Base(vm, structure, type)
    , m_exponent(number.exponent)
    , m_length(static_cast<uint32_t>(number.length))
    , m_flags(number.flags)
{
    const Limb* source = number.limbs();
    Limb* target = limbs();
    for (int64_t i = 0; i < number.length; ++i)
        target[i] = source[i];
}

PyDecimal* PyDecimal::tryCreate(VM& vm, Structure* structure, PyType* type, const Number& number)
{
    if (number.length > 2) [[unlikely]] {
        if (static_cast<uint64_t>(number.length) > std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), WTF::ramSize() / sizeof(Limb)))
            return nullptr;
    }
    void* cell = tryAllocateCell<PyDecimal>(vm, allocationSize(number.length));
    if (!cell) [[unlikely]]
        return nullptr;
    auto* decimal = new (NotNull, cell) PyDecimal(vm, structure, type, number);
    decimal->finishCreation(vm);
    return decimal;
}

PyDecimalSignals::PyDecimalSignals(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyDecimalSignals* PyDecimalSignals::create(VM& vm, Structure* structure, PyType* type)
{
    auto* signals = new (NotNull, allocateCell<PyDecimalSignals>(vm)) PyDecimalSignals(vm, structure, type);
    signals->finishCreation(vm);
    return signals;
}

PyDecimalContext::PyDecimalContext(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyDecimalContext* PyDecimalContext::create(VM& vm, Structure* structure, PyType* type)
{
    auto* context = new (NotNull, allocateCell<PyDecimalContext>(vm)) PyDecimalContext(vm, structure, type);
    context->finishCreation(vm);
    return context;
}

template<typename Visitor>
void PyDecimalContext::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyDecimalContext>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->signals);
    visitor.append(thisObject->traps);
    visitor.append(thisObject->flags);
}

DEFINE_VISIT_CHILDREN(PyDecimalContext);

State::State(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

State* State::create(VM& vm, Structure* structure, PyType* type)
{
    auto* state = new (NotNull, allocateCell<State>(vm)) State(vm, structure, type);
    state->finishCreation(vm);
    return state;
}

template<typename Visitor>
void State::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<State>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->decimalType);
    visitor.append(thisObject->contextType);
    visitor.append(thisObject->contextManagerType);
    visitor.append(thisObject->signalsType);
    visitor.append(thisObject->signalDictMixinType);
    visitor.append(thisObject->signalDictType);
    visitor.append(thisObject->decimalTupleType);
    visitor.append(thisObject->decimalException);
    for (auto& signal : thisObject->signals)
        visitor.append(signal);
    for (auto& condition : thisObject->invalidConditions)
        visitor.append(condition);
    visitor.append(thisObject->signalTuple);
    for (auto& rounding : thisObject->roundings)
        visitor.append(rounding);
    visitor.append(thisObject->decimalStructure);
    visitor.append(thisObject->contextStructure);
    visitor.append(thisObject->signalsStructure);
    visitor.append(thisObject->defaultContext);
    visitor.append(thisObject->basicContext);
    visitor.append(thisObject->extendedContext);
    visitor.append(thisObject->contextVariable);
    visitor.append(thisObject->rational);
    visitor.append(thisObject->pythonDecimal);
    visitor.append(thisObject->cachedContext);
}

DEFINE_VISIT_CHILDREN(State);

// ---- Signals ----

const uint32_t conditionsOfSignal[numberOfSignals] = {
    invalidOperationSignal, FloatOperation, DivisionByZero, Overflow, Underflow, Subnormal, Inexact, Rounded, Clamped
};
const ASCIILiteral namesOfSignals[numberOfSignals] = {
    "InvalidOperation"_s, "FloatOperation"_s, "DivisionByZero"_s, "Overflow"_s, "Underflow"_s, "Subnormal"_s, "Inexact"_s, "Rounded"_s, "Clamped"_s
};
static constexpr uint32_t conditionsOfInvalidConditions[numberOfInvalidConditions] = {
    ConversionSyntax, DivisionImpossible, DivisionUndefined, InvalidContext
};
static constexpr ASCIILiteral namesOfInvalidConditions[numberOfInvalidConditions] = {
    "ConversionSyntax"_s, "DivisionImpossible"_s, "DivisionUndefined"_s, "InvalidContext"_s
};
const ASCIILiteral namesOfRoundings[numberOfRoundings] = {
    "ROUND_UP"_s, "ROUND_DOWN"_s, "ROUND_CEILING"_s, "ROUND_FLOOR"_s, "ROUND_HALF_UP"_s, "ROUND_HALF_DOWN"_s, "ROUND_HALF_EVEN"_s, "ROUND_05UP"_s
};

bool addStatusSlow(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, uint32_t status)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (status & OutOfMemory) {
        raiseMemoryError(globalObject, scope, String());
        return true;
    }
    uint32_t trapped = context->signals->traps & status;
    unsigned first = 0;
    while (!(trapped & conditionsOfSignal[first]))
        ++first;
    // The one argument of the exception: the classes of all that is trapped, the conditions that InvalidOperation stands for first.
    JSValue classes[static_cast<unsigned>(numberOfSignals) + static_cast<unsigned>(numberOfInvalidConditions)];
    unsigned count = 0;
    if (trapped & InvalidOperation)
        classes[count++] = state.signals[SignalInvalidOperation].get();
    for (unsigned i = 0; i < numberOfInvalidConditions; ++i) {
        if (trapped & conditionsOfInvalidConditions[i])
            classes[count++] = state.invalidConditions[i].get();
    }
    for (unsigned i = 1; i < numberOfSignals; ++i) {
        if (trapped & conditionsOfSignal[i])
            classes[count++] = state.signals[i].get();
    }
    raiseWithArgs(globalObject, scope, state.signals[first].get(), { newList(globalObject, std::span<const JSValue> { classes, count }) });
    return true;
}

// ---- The context of the thread ----

PyDecimalContext* currentContextSlow(JSGlobalObject* globalObject, State& state)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = contextVariableValue(globalObject, state.contextVariable.get());
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyDecimalContext* context;
    if (value) {
        context = PyDecimalContext::tryFrom(value);
        // The variable is to be found among those of contextvars.copy_context(), and can be set to anything.
        if (!context) [[unlikely]] {
            raiseTypeError(globalObject, scope, "argument must be a context"_s);
            return nullptr;
        }
    } else {
        context = copyContext(globalObject, state, state.defaultContext.get());
        RETURN_IF_EXCEPTION(scope, nullptr);
        context->signals->status = 0;
        if (failed(scope, setContextVariable(globalObject, state.contextVariable.get(), context)))
            return nullptr;
    }
    state.cachedContext.set(vm, &state, context);
    state.cachedContextVersion = rt.contextVariablesVersion();
    return context;
}

bool setCurrentContext(JSGlobalObject* globalObject, State& state, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* context = PyDecimalContext::tryFrom(value);
    if (!context) {
        raiseTypeError(globalObject, scope, "argument must be a context"_s);
        return false;
    }
    // The three that the module offers stay as they are.
    if (context == state.defaultContext.get() || context == state.basicContext.get() || context == state.extendedContext.get()) {
        context = copyContext(globalObject, state, context);
        RETURN_IF_EXCEPTION(scope, false);
        context->signals->status = 0;
    }
    RELEASE_AND_RETURN(scope, setContextVariable(globalObject, state.contextVariable.get(), context));
}

PyDecimalContext* contextArgument(JSGlobalObject* globalObject, State& state, JSValue value)
{
    if (!value || isNone(value)) [[likely]]
        return currentContext(globalObject, state);
    if (auto* context = PyDecimalContext::tryFrom(value)) [[likely]]
        return context;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "optional argument must be a context"_s);
    return nullptr;
}

// ---- From numbers of Python ----

void numberFromInt(Number& number, JSValue value, const Context& context, uint32_t& status)
{
    if (isSmallInt(value)) [[likely]] {
        number.setInt64(smallIntValue(value));
        finalize(number, context, status);
        return;
    }
    JSBigInt* bigInt = value.asHeapBigInt();
    unsigned length = bigInt->length();
    Vector<uint64_t, 16> words;
    if (!words.tryGrow(length)) {
        number.setError(OutOfMemory, status);
        return;
    }
    for (unsigned i = 0; i < length; ++i)
        words[i] = bigInt->digit(i);
    fromBinaryMagnitude(number, bigInt->sign() ? Number::Negative : 0, words.span().data(), length, context, status);
}

void numberFromIntExactly(Number& number, JSValue value, uint32_t& status)
{
    if (isSmallInt(value)) [[likely]] {
        number.setInt64(smallIntValue(value));
        return;
    }
    uint32_t ownStatus = 0;
    numberFromInt(number, value, Context::maximum(), ownStatus);
    if (ownStatus & (Inexact | Rounded | Clamped))
        number.setError(InvalidOperation, ownStatus);
    status |= ownStatus & errorConditions;
}

void numberFromDouble(Number& number, double value, uint32_t& status)
{
    if (std::isnan(value)) {
        number.setSpecial(0, Number::QuietNaN);
        return;
    }
    uint8_t sign = std::signbit(value) ? Number::Negative : 0;
    if (std::isinf(value)) {
        number.setSpecial(sign, Number::Infinite);
        return;
    }
    if (!value) {
        number.set(sign, 0, 0);
        return;
    }
    // |value| is mantissa * 2**exponent, with an odd mantissa.
    int exponent;
    uint64_t mantissa = static_cast<uint64_t>(std::ldexp(std::frexp(std::fabs(value), &exponent), 53));
    exponent -= 53;
    int zeros = __builtin_ctzll(mantissa);
    mantissa >>= zeros;
    exponent += zeros;

    Context unlimited = Context::maximum();
    if (exponent >= 0) {
        uint64_t words[18] = { };
        unsigned word = exponent / 64;
        unsigned bit = exponent % 64;
        words[word] = mantissa << bit;
        if (bit)
            words[word + 1] = mantissa >> (64 - bit);
        fromBinaryMagnitude(number, sign, words, words[word + 1] ? word + 2 : word + 1, unlimited, status);
        return;
    }
    // mantissa / 2**k is mantissa * 5**k / 10**k.
    Number five(0, 5, 0);
    Number k(0, static_cast<Limb>(-exponent), 0);
    Number scale;
    power(scale, five, k, unlimited, status);
    Number coefficient(0, mantissa, 0);
    multiply(number, coefficient, scale, unlimited, status);
    if (number.isFinite()) {
        number.setSign(sign);
        number.exponent = exponent;
    }
}

Conversion convertOperandSlow(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, JSValue value, Number& number, bool raises)
{
    if (isInt(value)) {
        uint32_t status = 0;
        numberFromIntExactly(number, intValue(value), status);
        if (status && addStatus(globalObject, state, context, status)) [[unlikely]]
            return Conversion::Raised;
        return Conversion::Done;
    }
    if (!raises)
        return Conversion::NotImplemented;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "conversion from "_s, typeName(globalObject, value), " to Decimal is not supported"_s);
    return Conversion::Raised;
}

// ---- Making a Decimal ----

JSValue makeDecimal(JSGlobalObject* globalObject, State& state, PyType* type, const Number& number)
{
    VM& vm = getVM(globalObject);
    PyDecimal* decimal = PyDecimal::tryCreate(vm, state.decimalStructure.get(), type, number);
    if (!decimal) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseMemoryError(globalObject, scope, String());
    }
    return decimal;
}

using Characters = Vector<char, 80>;

static JSValue decimalFromText(JSGlobalObject* globalObject, State& state, PyType* type, const Characters& text, PyDecimalContext* context, bool isExact)
{
    // The text ends with a NUL.
    Number number;
    uint32_t status = 0;
    if (isExact) {
        fromString(number, text.span().data(), text.size() - 1, Context::maximum(), status);
        if (status & (Inexact | Rounded | Clamped))
            number.setError(InvalidOperation, status);
        status &= errorConditions;
    } else
        fromString(number, text.span().data(), text.size() - 1, context->context, status);
    if (addStatus(globalObject, state, context, status)) [[unlikely]]
        return { };
    return makeDecimal(globalObject, state, type, number);
}

// The characters of a numeric string in ASCII: any digit of Unicode is one, any space is a space. A character that cannot be part
// of a number makes the text empty, which is no number either. For the constructor, space around the number and underscores do not count.
static bool collectNumericText(JSGlobalObject* globalObject, JSString* string, bool forConstructor, Characters& text)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    StrView view(globalObject, string);
    RETURN_IF_EXCEPTION(scope, false);
    unsigned start = 0;
    unsigned end = view.length();
    if (forConstructor) {
        while (end && isWhitespace(view.at(end - 1)))
            --end;
        while (start < end && isWhitespace(view.at(start)))
            ++start;
    }
    if (!text.tryReserveCapacity(end - start + 1)) {
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    for (unsigned i = start; i < end; ++i) {
        char32_t character = view.at(i);
        if (character == '_' && forConstructor)
            continue;
        if (character && character <= 127) [[likely]] {
            text.append(static_cast<char>(character));
            continue;
        }
        if (isWhitespace(character)) {
            text.append(' ');
            continue;
        }
        int digit = Unicode::decimalValue(character);
        if (digit < 0) {
            text.shrink(0);
            break;
        }
        text.append(static_cast<char>('0' + digit));
    }
    text.append('\0');
    return true;
}

static JSValue decimalFromStr(JSGlobalObject* globalObject, State& state, PyType* type, JSString* string, PyDecimalContext* context, bool isExact)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Characters text;
    if (failed(scope, collectNumericText(globalObject, string, isExact, text))) [[unlikely]]
        return { };
    RELEASE_AND_RETURN(scope, decimalFromText(globalObject, state, type, text, context, isExact));
}

// A tuple or a list, as a span that holds for as long as nothing of the program runs.
static std::optional<std::span<const JSValue>> itemsOfSequence(JSValue value, Vector<JSValue, 32>& storage)
{
    if (auto* tuple = PyTuple::tryFrom(value))
        return tuple->span();
    JSArray* list = tryAsList(value);
    if (!list)
        return std::nullopt;
    unsigned length = listLength(list);
    storage.reserveInitialCapacity(length);
    for (unsigned i = 0; i < length; ++i)
        storage.append(listGet(list, i));
    return storage.span();
}

// (sign, digits, exponent), as as_tuple() returns it, becomes the text of the number.
static bool collectTextOfTuple(JSGlobalObject* globalObject, JSValue sequence, Characters& text)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<JSValue, 32> storage;
    auto items = itemsOfSequence(sequence, storage);
    ASSERT(items);
    if (items->size() != 3) {
        raiseValueError(globalObject, scope, "argument must be a sequence of length 3"_s);
        return false;
    }
    auto toLong = [&](JSValue value, int64_t& result, ASCIILiteral type) {
        auto converted = tryInt64(intValue(value));
        if (!converted) {
            raiseOverflowError(globalObject, scope, "Python int too large to convert to C "_s, type);
            return false;
        }
        result = *converted;
        return true;
    };

    JSValue signValue = (*items)[0];
    int64_t sign = -1;
    if (isInt(signValue) && !toLong(signValue, sign, "long"_s))
        return false;
    if (sign != 0 && sign != 1) {
        raiseValueError(globalObject, scope, "sign must be an integer with the value 0 or 1"_s);
        return false;
    }
    text.append(sign ? '-' : '+');

    JSValue exponentValue = (*items)[2];
    int64_t exponent = 0;
    bool isSpecial = false;
    bool isInfinite = false;
    if (JSString* kind = tryStrValue(exponentValue)) {
        String word = kind->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        isSpecial = true;
        if (word == "F"_s) {
            text.append(std::span { "Inf", 3 });
            isInfinite = true;
        } else if (word == "n"_s)
            text.append(std::span { "NaN", 3 });
        else if (word == "N"_s)
            text.append(std::span { "sNaN", 4 });
        else {
            raiseValueError(globalObject, scope, "string argument in the third position must be 'F', 'n' or 'N'"_s);
            return false;
        }
    } else {
        if (!isInt(exponentValue)) {
            raiseValueError(globalObject, scope, "exponent must be an integer"_s);
            return false;
        }
        if (!toLong(exponentValue, exponent, "ssize_t"_s))
            return false;
    }

    Vector<JSValue, 32> digitStorage;
    auto digits = itemsOfSequence((*items)[1], digitStorage);
    if (!digits) {
        raiseValueError(globalObject, scope, "coefficient must be a tuple of digits"_s);
        return false;
    }
    if (!text.tryReserveCapacity(digits->size() + 32)) {
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    if (digits->empty() && !isSpecial)
        text.append('0');
    for (JSValue item : *digits) {
        int64_t digit = -1;
        if (isInt(item) && !toLong(item, digit, "long"_s))
            return false;
        if (digit < 0 || digit > 9) {
            raiseValueError(globalObject, scope, "coefficient must be a tuple of digits"_s);
            return false;
        }
        // An infinity may come with digits, which mean nothing.
        if (!isInfinite)
            text.append(static_cast<char>('0' + digit));
    }
    if (!isSpecial) {
        text.append('E');
        char buffer[24];
        int length = snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(exponent));
        text.append(std::span { buffer, static_cast<size_t>(length) });
    }
    text.append('\0');
    return true;
}

static JSValue decimalFromSequence(JSGlobalObject* globalObject, State& state, PyType* type, JSValue sequence, PyDecimalContext* context, bool isExact)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Characters text;
    if (failed(scope, collectTextOfTuple(globalObject, sequence, text)))
        return { };
    RELEASE_AND_RETURN(scope, decimalFromText(globalObject, state, type, text, context, isExact));
}

static JSValue decimalFromInt(JSGlobalObject* globalObject, State& state, PyType* type, JSValue exactInt, PyDecimalContext* context, bool isExact)
{
    Number number;
    uint32_t status = 0;
    if (isExact)
        numberFromIntExactly(number, exactInt, status);
    else
        numberFromInt(number, exactInt, context->context, status);
    if (addStatus(globalObject, state, context, status)) [[unlikely]]
        return { };
    return makeDecimal(globalObject, state, type, number);
}

static JSValue decimalFromFloat(JSGlobalObject* globalObject, State& state, PyType* type, JSValue value, PyDecimalContext* context, bool isExact)
{
    Number number;
    uint32_t status = 0;
    if (isInt(value))
        numberFromIntExactly(number, intValue(value), status);
    else if (isFloat(value))
        numberFromDouble(number, floatValue(value), status);
    else {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseTypeError(globalObject, scope, "argument must be int or float"_s);
    }
    if (addStatus(globalObject, state, context, status)) [[unlikely]]
        return { };
    if (!isExact) {
        status = 0;
        finalize(number, context->context, status);
        if (addStatus(globalObject, state, context, status)) [[unlikely]]
            return { };
    }
    return makeDecimal(globalObject, state, type, number);
}

JSValue decimalFromFloat(JSGlobalObject* globalObject, State& state, JSValue value, PyDecimalContext* context, bool isExact)
{
    return decimalFromFloat(globalObject, state, state.decimalType.get(), value, context, isExact);
}

static JSValue raiseNotConvertible(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "conversion from "_s, typeName(globalObject, value), " to Decimal is not supported"_s);
}

JSValue decimalFromObjectExactly(JSGlobalObject* globalObject, State& state, PyType* type, JSValue value, PyDecimalContext* context)
{
    if (!value)
        return makeDecimal(globalObject, state, type, Number(0, 0, 0));
    if (auto* decimal = PyDecimal::tryFrom(value)) {
        if (type == state.decimalType.get() && decimal->type() == type)
            return decimal;
        Number number;
        decimal->lend(number);
        JSValue copy = makeDecimal(globalObject, state, type, number);
        ensureStillAliveHere(decimal);
        return copy;
    }
    if (JSString* string = tryStrValue(value))
        return decimalFromStr(globalObject, state, type, string, context, true);
    if (isInt(value))
        return decimalFromInt(globalObject, state, type, intValue(value), context, true);
    if (PyTuple::is(value) || isList(value))
        return decimalFromSequence(globalObject, state, type, value, context, true);
    if (isFloat(value)) {
        if (addStatus(globalObject, state, context, FloatOperation))
            return { };
        return decimalFromFloat(globalObject, state, type, value, context, true);
    }
    return raiseNotConvertible(globalObject, value);
}

JSValue decimalFromObject(JSGlobalObject* globalObject, State& state, JSValue value, PyDecimalContext* context)
{
    PyType* type = state.decimalType.get();
    if (!value)
        return decimalFromInt(globalObject, state, type, jsNumber(0), context, false);
    if (auto* decimal = PyDecimal::tryFrom(value)) {
        const Context& limits = context->context;
        if (decimal->isNaN() && decimal->digits() > limits.precision - limits.clamp) {
            // A diagnostic that is too long for the context.
            if (addStatus(globalObject, state, context, ConversionSyntax))
                return { };
            return makeDecimal(globalObject, state, Number());
        }
        Number number;
        decimal->lend(number);
        Number result;
        uint32_t status = 0;
        if (result.copy(number, status))
            finalize(result, limits, status);
        return finish(globalObject, state, context, result, status);
    }
    if (JSString* string = tryStrValue(value))
        return decimalFromStr(globalObject, state, type, string, context, false);
    if (isInt(value))
        return decimalFromInt(globalObject, state, type, intValue(value), context, false);
    if (PyTuple::is(value) || isList(value))
        return decimalFromSequence(globalObject, state, type, value, context, false);
    if (isFloat(value)) {
        if (addStatus(globalObject, state, context, FloatOperation))
            return { };
        return decimalFromFloat(globalObject, state, type, value, context, false);
    }
    return raiseNotConvertible(globalObject, value);
}

// ---- Text ----

JSString* decimalToStr(JSGlobalObject* globalObject, const Number& number, unsigned flags, ASCIILiteral before, ASCIILiteral after)
{
    VM& vm = getVM(globalObject);
    Text text;
    RefPtr<StringImpl> impl;
    if (toText(text, number, flags) && text.length < String::MaxLength - 16) [[likely]] {
        std::span<Latin1Character> characters;
        impl = StringImpl::tryCreateUninitialized(before.length() + text.length + after.length(), characters);
        if (impl) [[likely]] {
            memcpy(characters.data(), before.characters(), before.length());
            memcpy(characters.data() + before.length(), text.data, text.length);
            memcpy(characters.data() + before.length() + text.length, after.characters(), after.length());
            return makeStr(vm, String(impl.releaseNonNull()));
        }
    }
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseMemoryError(globalObject, scope, String());
    return nullptr;
}

// ---- What Decimal and Context both have ----

JSValue applyFusedMultiplyAdd(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, JSValue x, JSValue y, JSValue z)
{
    Number a;
    Number b;
    Number c;
    if (convertOperand(globalObject, state, context, x, a, true) != Conversion::Done
        || convertOperand(globalObject, state, context, y, b, true) != Conversion::Done
        || convertOperand(globalObject, state, context, z, c, true) != Conversion::Done)
        return { };
    Number result;
    uint32_t status = 0;
    fusedMultiplyAdd(result, a, b, c, context->context, status);
    return finish(globalObject, state, context, result, status);
}

// False: `result` is what to return, NotImplemented or empty.
static ALWAYS_INLINE bool convertOrGiveUp(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, JSValue value, Number& number, bool raises, JSValue& result)
{
    switch (convertOperand(globalObject, state, context, value, number, raises)) {
    case Conversion::Done:
        return true;
    case Conversion::NotImplemented:
        result = runtime(globalObject).notImplemented();
        return false;
    case Conversion::Raised:
        break;
    }
    result = { };
    return false;
}

JSValue applyDivMod(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, JSValue x, JSValue y, bool raises)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Number a;
    Number b;
    JSValue early;
    if (!convertOrGiveUp(globalObject, state, context, x, a, raises, early) || !convertOrGiveUp(globalObject, state, context, y, b, raises, early))
        RELEASE_AND_RETURN(scope, early);
    Number quotient;
    Number rest;
    uint32_t status = 0;
    divideWithRemainder(quotient, rest, a, b, context->context, status);
    if (failed(scope, !addStatus(globalObject, state, context, status)))
        return { };
    JSValue first = makeDecimal(globalObject, state, quotient);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue second = makeDecimal(globalObject, state, rest);
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { first, second });
}

JSValue applyPower(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, JSValue base, JSValue exponent, JSValue modulus, bool raises)
{
    Number a;
    Number b;
    Number c;
    JSValue early;
    if (!convertOrGiveUp(globalObject, state, context, base, a, raises, early) || !convertOrGiveUp(globalObject, state, context, exponent, b, raises, early))
        return early;
    bool hasModulus = modulus && !isNone(modulus);
    if (hasModulus && !convertOrGiveUp(globalObject, state, context, modulus, c, raises, early))
        return early;
    Number result;
    uint32_t status = 0;
    if (hasModulus)
        powerModulo(result, a, b, c, context->context, status);
    else
        power(result, a, b, context->context, status);
    return finish(globalObject, state, context, result, status);
}

// ---- The state ----

// Never seen from Python.
static constexpr TypeDef stateTypeDef { "decimal._state"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation, TypeFlag::Internal }, { }, { }, { } };
static constexpr TypeDef signalsTypeDef { "decimal._signals"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation, TypeFlag::Internal }, { }, { }, { } };

State* createState(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State* state = State::create(vm, State::createStructure(vm, globalObject), rt.typeFor(stateTypeDef));

    state->decimalType.set(vm, state, rt.typeFor(decimalTypeDef));
    state->contextType.set(vm, state, rt.typeFor(contextTypeDef));
    state->contextManagerType.set(vm, state, rt.typeFor(contextManagerTypeDef));
    state->signalsType.set(vm, state, rt.typeFor(signalsTypeDef));
    PyType* mixin = rt.typeFor(signalDictMixinTypeDef);
    state->signalDictMixinType.set(vm, state, mixin);
    if (!mixin->dict()->get(rt.name(Special::Hash))) {
        mixin->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
        mixin->didChange(vm);
    }
    state->decimalStructure.set(vm, state, PyDecimal::createStructure(vm, globalObject));
    state->contextStructure.set(vm, state, PyDecimalContext::createStructure(vm, globalObject));
    state->signalsStructure.set(vm, state, PyDecimalSignals::createStructure(vm, globalObject));
    JSString* moduleName = internString(vm, "decimal"_s);

    // numbers.Number.register(Decimal)
    JSValue numbers = importModule(globalObject, "numbers"_s);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSValue number = getAttr(globalObject, numbers, internString(vm, "Number"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    callMethod(globalObject, number, internString(vm, "register"_s), state->decimalType.get());
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSValue rational = getAttr(globalObject, numbers, internString(vm, "Rational"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    state->rational.set(vm, state, rational);

    // DecimalTuple = collections.namedtuple('DecimalTuple', 'sign digits exponent')
    JSValue collections = importModule(globalObject, "collections"_s);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSValue decimalTuple = callMethod(globalObject, collections, internString(vm, "namedtuple"_s), makeStr(vm, "DecimalTuple"_s), makeStr(vm, "sign digits exponent"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!PyType::is(decimalTuple)) {
        raiseTypeError(globalObject, scope, "type is expected from namedtuple call"_s);
        return nullptr;
    }
    state->decimalTupleType.set(vm, state, PyType::from(decimalTuple));
    setAttr(globalObject, decimalTuple, rt.names.dModule(), moduleName);
    RETURN_IF_EXCEPTION(scope, nullptr);

    // class SignalDict(SignalDictMixin, collections.abc.MutableMapping)
    JSValue abstractCollections = importModule(globalObject, "collections.abc"_s);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSValue mutableMapping = getAttr(globalObject, abstractCollections, internString(vm, "MutableMapping"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSValue signalDict = call(globalObject, rt.typeType(), makeStr(vm, "SignalDict"_s), PyTuple::create(globalObject, { mixin, mutableMapping }), PyDict::create(globalObject));
    RETURN_IF_EXCEPTION(scope, nullptr);
    // abc.ABCMeta has made it, a class written in Python, or whatever the program has put in the place of MutableMapping. Its instances are made here.
    if (!PyType::is(signalDict) || PyType::from(signalDict)->layoutBase() != mixin) {
        raiseTypeError(globalObject, scope, "type is expected for SignalDict"_s);
        return nullptr;
    }
    state->signalDictType.set(vm, state, PyType::from(signalDict));

    auto newException = [&](ASCIILiteral name, std::initializer_list<JSValue> bases) -> PyType* {
        PyDict* members = PyDict::create(globalObject);
        members->set(vm, rt.names.dModule(), moduleName);
        JSValue type = call(globalObject, rt.typeType(), makeStr(vm, name), PyTuple::create(globalObject, bases), members);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return PyType::from(type);
    };
    PyType* decimalException = newException("DecimalException"_s, { rt.ArithmeticError() });
    RETURN_IF_EXCEPTION(scope, nullptr);
    state->decimalException.set(vm, state, decimalException);
    // From the last, as those that others derive from come later.
    for (unsigned i = numberOfSignals; i--;) {
        PyType* type;
        switch (i) {
        case SignalFloatOperation:
            type = newException(namesOfSignals[i], { decimalException, rt.TypeError() });
            break;
        case SignalDivisionByZero:
            type = newException(namesOfSignals[i], { decimalException, rt.ZeroDivisionError() });
            break;
        case SignalOverflow:
            type = newException(namesOfSignals[i], { state->signals[SignalInexact].get(), state->signals[SignalRounded].get() });
            break;
        case SignalUnderflow:
            type = newException(namesOfSignals[i], { state->signals[SignalInexact].get(), state->signals[SignalRounded].get(), state->signals[SignalSubnormal].get() });
            break;
        default:
            type = newException(namesOfSignals[i], { decimalException });
            break;
        }
        RETURN_IF_EXCEPTION(scope, nullptr);
        state->signals[i].set(vm, state, type);
    }
    PyType* invalidOperation = state->signals[SignalInvalidOperation].get();
    for (unsigned i = 0; i < numberOfInvalidConditions; ++i) {
        PyType* type = i == InvalidDivisionUndefined ? newException(namesOfInvalidConditions[i], { invalidOperation, rt.ZeroDivisionError() }) : newException(namesOfInvalidConditions[i], { invalidOperation });
        RETURN_IF_EXCEPTION(scope, nullptr);
        state->invalidConditions[i].set(vm, state, type);
    }
    JSValue signals[numberOfSignals];
    for (unsigned i = 0; i < numberOfSignals; ++i)
        signals[i] = state->signals[i].get();
    state->signalTuple.set(vm, state, PyTuple::create(globalObject, std::span<const JSValue> { signals, numberOfSignals }));

    for (unsigned i = 0; i < numberOfRoundings; ++i)
        state->roundings[i].set(vm, state, internString(vm, namesOfRoundings[i]));

    PyDecimalContext* defaultContext = newContext(globalObject, *state, state->contextType.get());
    RETURN_IF_EXCEPTION(scope, nullptr);
    state->defaultContext.set(vm, state, defaultContext);
    state->contextVariable.set(vm, state, newContextVariable(globalObject, "decimal_context"_s));

    PyDecimalContext* basicContext = newContext(globalObject, *state, state->contextType.get());
    RETURN_IF_EXCEPTION(scope, nullptr);
    basicContext->context.precision = 9;
    basicContext->context.rounding = Rounding::HalfUp;
    basicContext->signals->traps |= Underflow | Clamped;
    state->basicContext.set(vm, state, basicContext);

    PyDecimalContext* extendedContext = newContext(globalObject, *state, state->contextType.get());
    RETURN_IF_EXCEPTION(scope, nullptr);
    extendedContext->context.precision = 9;
    extendedContext->signals->traps = 0;
    state->extendedContext.set(vm, state, extendedContext);

    rt.setSlot(Runtime::Slot::Decimal, state);
    return state;
}

// ---- The functions of the module ----

static JSValue moduleGetContext(JSGlobalObject* globalObject)
{
    return currentContext(globalObject, stateOf(globalObject));
}

static JSValue moduleSetContext(JSGlobalObject* globalObject, JSValue context)
{
    if (!setCurrentContext(globalObject, stateOf(globalObject), context))
        return { };
    return jsUndefined();
}

enum ContextManagerField : unsigned { ManagerLocal, ManagerGlobal, ManagerFieldCount };

static JSValue moduleLocalContext(JSGlobalObject* globalObject, const CallArgs& args)
{
    static constexpr ASCIILiteral names[] = { "ctx"_s, "prec"_s, "rounding"_s, "Emin"_s, "Emax"_s, "capitals"_s, "clamp"_s, "flags"_s, "traps"_s };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue values[std::size(names)];
    if (failed(scope, DateTime::bindArguments(globalObject, { "localcontext"_s, names, 0, 1 }, args, values)))
        return { };
    State& state = stateOf(globalObject);
    PyDecimalContext* global = currentContext(globalObject, state);
    RETURN_IF_EXCEPTION(scope, { });
    PyDecimalContext* local = global;
    if (values[0] && !isNone(values[0])) {
        local = PyDecimalContext::tryFrom(values[0]);
        if (!local)
            return raiseTypeError(globalObject, scope, "optional argument must be a context"_s);
    }
    PyDecimalContext* copy = copyContext(globalObject, state, local);
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, applyContextSettings(globalObject, state, copy, { values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8] })))
        return { };
    PyRecord* manager = PyRecord::create(globalObject, state.contextManagerType.get(), ManagerFieldCount);
    manager->set(vm, ManagerLocal, copy);
    manager->set(vm, ManagerGlobal, global);
    return manager;
}

static JSValue moduleIEEEContext(JSGlobalObject* globalObject, JSValue bitsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, bitsValue);
    RETURN_IF_EXCEPTION(scope, { });
    auto bits = tryInt64(index);
    if (!bits)
        return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
    Context limits;
    if (!Context::forIEEE(*bits, limits))
        return raiseValueError(globalObject, scope, "argument must be a multiple of 32, with a maximum of "_s, maxBitsOfIEEEContext);
    State& state = stateOf(globalObject);
    PyDecimalContext* context = newContext(globalObject, state, state.contextType.get());
    RETURN_IF_EXCEPTION(scope, { });
    context->context = limits;
    context->signals->traps = 0;
    context->signals->status = 0;
    return context;
}

static JSValue moduleGetAttr(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.hasKeywords())
        return raiseTypeError(globalObject, scope, "_decimal.__getattr__() takes no keyword arguments"_s);
    if (args.size() != 1)
        return raiseTypeError(globalObject, scope, "__getattr__ expected 1 argument, got "_s, args.size());
    JSValue name = args[0];
    if (JSString* string = tryStrValue(name)) {
        String text = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (text == "__version__"_s) {
            if (failed(scope, warn(globalObject, rt.DeprecationWarning(), "'__version__' is deprecated and slated for removal in Python 3.20"_s)))
                return { };
            return makeStr(vm, "1.70"_s);
        }
    }
    String shown = reprForMessage(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    return raise(globalObject, scope, rt.AttributeError(), makeString("module 'decimal' has no attribute "_s, shown));
}

static constexpr NativeDef moduleFunctions[] = {
    PY_FUNCTION("getcontext", moduleGetContext, "()"),
    PY_FUNCTION("setcontext", moduleSetContext, "(context, /)"),
    PY_FUNCTION("localcontext", moduleLocalContext, "!(ctx=None, **kwargs)"),
    PY_FUNCTION("IEEEContext", moduleIEEEContext, "(bits, /)"),
    PY_FUNCTION("__getattr__", moduleGetAttr, "(*args)"),
};

// ---- What localcontext() returns ----

static JSValue contextManagerEnter(JSGlobalObject* globalObject, JSValue self)
{
    JSValue local = PyRecord::from(self)->get(ManagerLocal);
    if (!setCurrentContext(globalObject, stateOf(globalObject), local))
        return { };
    return local;
}

static JSValue contextManagerExit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    if (args.hasKeywords()) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseTypeError(globalObject, scope, "ContextManager.__exit__() takes no keyword arguments"_s);
    }
    if (!setCurrentContext(globalObject, stateOf(globalObject), PyRecord::from(self)->get(ManagerGlobal)))
        return { };
    return jsUndefined();
}

static constexpr NativeDef contextManagerMethods[] = {
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_METHOD("__enter__", contextManagerEnter, "($self, /)"),
    PY_METHOD("__exit__", contextManagerExit, "($self, *args)"),
};

constexpr TypeDef contextManagerTypeDef {
    "decimal.ContextManager"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, contextManagerMethods, { }, { }
};

} // namespace Decimal

void initializeDecimalModule(JSGlobalObject* globalObject, PyModule* module)
{
    using namespace Decimal;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // One for the runtime: a module that is imported afresh has the types and the contexts of the first.
    State* state = rt.slot(Runtime::Slot::Decimal) ? &stateOf(globalObject) : createState(globalObject);
    RETURN_IF_EXCEPTION(scope, void());

    module->dict()->set(vm, rt.names.dDoc(), makeStr(vm, "C decimal arithmetic module"_s));
    module->addFunctions(globalObject, moduleFunctions);
    module->add(globalObject, "Decimal"_s, state->decimalType.get());
    module->add(globalObject, "Context"_s, state->contextType.get());
    module->add(globalObject, "DecimalTuple"_s, state->decimalTupleType.get());
    module->add(globalObject, "DecimalException"_s, state->decimalException.get());
    for (unsigned i = numberOfSignals; i--;)
        module->add(globalObject, namesOfSignals[i], state->signals[i].get());
    for (unsigned i = 0; i < numberOfInvalidConditions; ++i)
        module->add(globalObject, namesOfInvalidConditions[i], state->invalidConditions[i].get());
    module->add(globalObject, "DefaultContext"_s, state->defaultContext.get());
    module->add(globalObject, "HAVE_CONTEXTVAR"_s, jsBoolean(true));
    module->add(globalObject, "HAVE_THREADS"_s, jsBoolean(true));
    module->add(globalObject, "BasicContext"_s, state->basicContext.get());
    module->add(globalObject, "ExtendedContext"_s, state->extendedContext.get());
    for (auto [name, limit] : { std::pair { "MAX_PREC"_s, maxPrecision }, std::pair { "MAX_EMAX"_s, maxEmax }, std::pair { "MIN_EMIN"_s, minEmin }, std::pair { "MIN_ETINY"_s, minEtiny } }) {
        JSValue value = makeInt(globalObject, limit);
        RETURN_IF_EXCEPTION(scope, void());
        module->add(globalObject, name, value);
    }
    module->add(globalObject, "IEEE_CONTEXT_MAX_BITS"_s, jsNumber(maxBitsOfIEEEContext));
    for (unsigned i = 0; i < numberOfRoundings; ++i)
        module->add(globalObject, namesOfRoundings[i], state->roundings[i].get());
    module->add(globalObject, "SPEC_VERSION"_s, makeStr(vm, "1.70"_s));
    // The version of the library whose results these are.
    module->add(globalObject, "__libmpdec_version__"_s, makeStr(vm, "2.5.1"_s));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
