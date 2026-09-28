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
#include "PythonOperations.h"

#include "IteratorOperations.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSGenerator.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyNativeFunction.h"
#include "PyObjects.h"
#include "PythonGenerators.h"
#include "PythonNumbers.h"
#include "PythonSequences.h"
#include "PythonStrings.h"
#include "TopExceptionScope.h"

// Operators, subscripts, iteration, and what every object can be asked.

namespace JSC { namespace Python {

std::optional<bool> builtinContains(JSGlobalObject*, JSValue container, JSValue);
int64_t builtinLength(JSGlobalObject*, JSValue);

static ASCIILiteral symbolOf(BinaryOperator op)
{
    static constexpr ASCIILiteral symbols[] = { "+"_s, "-"_s, "*"_s, "@"_s, "/"_s, "%"_s, "** or pow()"_s, "<<"_s, ">>"_s, "|"_s, "^"_s, "&"_s, "//"_s };
    return symbols[static_cast<unsigned>(op)];
}

static ASCIILiteral inPlaceSymbolOf(BinaryOperator op)
{
    static constexpr ASCIILiteral symbols[] = { "+="_s, "-="_s, "*="_s, "@="_s, "/="_s, "%="_s, "**="_s, "<<="_s, ">>="_s, "|="_s, "^="_s, "&="_s, "//="_s };
    return symbols[static_cast<unsigned>(op)];
}

static bool isNotImplemented(JSGlobalObject* globalObject, JSValue value)
{
    return value.isCell() && value.asCell() == globalObject->pyRealm()->notImplemented();
}

// Whether it is what a literal or a built-in function makes, and not an instance of some class derived from that.
static bool isExact(JSGlobalObject* globalObject, JSValue value)
{
    return !value.isObject() || !typeOf(globalObject, value)->hasFlag(PyType::IsHeapType);
}

// ---- Binary operators

std::optional<int64_t> toIndexOrOverflow(JSGlobalObject*, JSValue);

static std::optional<int64_t> repeatCount(JSGlobalObject* globalObject, JSValue value)
{
    Number number = classify(value);
    if (number.kind == Number::Kind::Small)
        return number.small;
    if (number.kind == Number::Kind::Big)
        return toIndexOrOverflow(globalObject, value);
    return std::nullopt;
}

static PyTuple* tupleConcatenate(JSGlobalObject* globalObject, PyTuple* left, PyTuple* right)
{
    VM& vm = globalObject->vm();
    PyTuple* result = PyTuple::create(globalObject, left->length() + right->length());
    for (unsigned i = 0; i < left->length(); ++i)
        result->initializeAt(vm, i, left->at(i));
    for (unsigned i = 0; i < right->length(); ++i)
        result->initializeAt(vm, left->length() + i, right->at(i));
    return result;
}

static JSValue tupleRepeat(JSGlobalObject* globalObject, PyTuple* tuple, int64_t count)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    count = std::max<int64_t>(count, 0);
    uint64_t length = static_cast<uint64_t>(tuple->length()) * count;
    if (length > std::numeric_limits<int32_t>::max())
        return raise(globalObject, scope, BuiltinType::MemoryError, JSValue());
    PyTuple* result = PyTuple::create(globalObject, length);
    for (unsigned i = 0; i < length; ++i)
        result->initializeAt(vm, i, tuple->at(i % tuple->length()));
    return result;
}

// What the built-in types other than the numbers do. Empty, with nothing raised, if it is not for them. This is what their __add__
// and the like are, so it looks at what kind of cell it has and not at the class.
// What str does with an instance of a class derived from it, it does with the string that is in that.
static JSValue stringIfHasOne(JSValue value)
{
    JSString* string = stringIn(value);
    return string ? JSValue(string) : value;
}

JSValue builtinBinaryOperation(JSGlobalObject* globalObject, BinaryOperator op, bool inPlace, JSValue left, JSValue right)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue givenLeft = left;
    JSValue givenRight = right;
    left = stringIfHasOne(left);
    right = stringIfHasOne(right);

    switch (op) {
    case BinaryOperator::Add:
        if (left.isString() && right.isString())
            RELEASE_AND_RETURN(scope, jsString(globalObject, asString(left), asString(right)));
        if (JSArray* list = tryList(left)) {
            if (inPlace) {
                listExtend(globalObject, list, right);
                RETURN_IF_EXCEPTION(scope, { });
                return list;
            }
            JSArray* other = tryList(right);
            if (!other)
                return { };
            unsigned leftLength = list->length();
            unsigned rightLength = other->length();
            JSArray* result = newList(globalObject, leftLength + rightLength);
            RETURN_IF_EXCEPTION(scope, { });
            for (unsigned i = 0; i < leftLength + rightLength; ++i) {
                JSValue item = i < leftLength ? listGet(globalObject, list, i) : listGet(globalObject, other, i - leftLength);
                RETURN_IF_EXCEPTION(scope, { });
                listInitializeAt(globalObject, result, i, item);
            }
            return result;
        }
        if (isTuple(left) && isTuple(right))
            return tupleConcatenate(globalObject, uncheckedDowncast<PyTuple>(left.asCell()), uncheckedDowncast<PyTuple>(right.asCell()));
        return { };

    case BinaryOperator::Mult: {
        // A sequence times an int, either way round.
        JSValue sequence = left;
        JSValue times = right;
        if (classify(left).isInt()) {
            if (inPlace)
                return { };
            std::swap(sequence, times);
        }
        if (!sequence.isString() && !isList(sequence) && !isTuple(sequence))
            return { };
        auto count = repeatCount(globalObject, times);
        RETURN_IF_EXCEPTION(scope, { });
        if (!count)
            return { };
        if (sequence.isString())
            RELEASE_AND_RETURN(scope, stringRepeat(globalObject, asString(sequence), *count));
        if (isTuple(sequence))
            RELEASE_AND_RETURN(scope, tupleRepeat(globalObject, uncheckedDowncast<PyTuple>(sequence.asCell()), *count));
        JSArray* result = listRepeat(globalObject, asList(sequence), *count);
        RETURN_IF_EXCEPTION(scope, { });
        if (!inPlace)
            return result;
        MarkedArgumentBuffer values;
        for (unsigned i = 0; i < result->length(); ++i)
            values.append(listGet(globalObject, result, i));
        listReplaceRange(globalObject, asList(sequence), 0, asList(sequence)->length(), values);
        RETURN_IF_EXCEPTION(scope, { });
        return sequence;
    }

    case BinaryOperator::Mod:
        if (left.isString())
            RELEASE_AND_RETURN(scope, stringPercentFormat(globalObject, givenLeft, givenRight));
        return { };

    case BinaryOperator::BitOr:
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitXor:
    case BinaryOperator::Sub:
        if (isSet(left) && isSet(right)) {
            // A frozenset is not changed, whatever the operator: `a -= b` is `a = a - b`.
            if (typeOf(globalObject, left)->isSubtypeOf(globalObject->pyRealm()->typeFrozenSet()))
                inPlace = false;
            RELEASE_AND_RETURN(scope, setOperation(globalObject, op, inPlace, uncheckedDowncast<PySet>(left.asCell()), uncheckedDowncast<PySet>(right.asCell())));
        }
        // d |= x takes whatever d.update(x) does.
        if (op == BinaryOperator::BitOr && isDict(left) && (inPlace || isDict(right))) {
            auto* result = uncheckedDowncast<PyDict>(left.asCell());
            if (!inPlace) {
                result = PyDict::create(globalObject);
                result->copyFrom(globalObject, *asDict(left));
            }
            updateDictFrom(globalObject, result, right);
            RETURN_IF_EXCEPTION(scope, { });
            return result;
        }
        return { };

    default:
        return { };
    }
}

// Whether it is what a built-in sequence has for + or *. In CPython those are not among what a type can do as a number (nb_add, nb_multiply) but among what it can do as a sequence (sq_concat,
// sq_repeat), and __add__ and __mul__ are how a program comes by them.
bool isSequenceSlot(JSGlobalObject* globalObject, JSValue method)
{
    auto* native = dynamicDowncast<PyNativeFunction>(method);
    if (!native || !native->owner())
        return false;
    PyRealm* realm = globalObject->pyRealm();
    JSObject* owner = native->owner();
    return owner == realm->typeStr() || owner == realm->typeList() || owner == realm->typeTuple() || owner == realm->typeBytes() || owner == realm->typeByteArray() || owner == realm->typeTemplate();
}

// A class that is derived from a built-in sequence still has what that has, whatever it has by the same name for itself.
static JSValue sequenceSlot(JSGlobalObject* globalObject, PyType* type, const Identifier& name)
{
    VM& vm = globalObject->vm();
    for (auto& entry : type->mro()->span()) {
        PyType* base = asType(entry.get());
        if (base->hasFlag(PyType::IsHeapType))
            continue;
        if (JSValue method = base->getDirect(vm, name))
            return isSequenceSlot(globalObject, method) ? method : JSValue();
    }
    return { };
}

// sequence_repeat()
static JSValue sequenceRepeat(JSGlobalObject* globalObject, JSValue slot, JSValue sequence, JSValue count)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!classify(count).isInt() && !typeOf(globalObject, count)->lookup(vm, vm.pythonNames().dunder_index))
        return raiseTypeError(globalObject, scope, makeString("can't multiply sequence by non-int of type '"_s, typeName(globalObject, count), '\''));
    RELEASE_AND_RETURN(scope, call(globalObject, slot, sequence, count));
}

static JSValue raiseUnsupportedOperands(JSGlobalObject* globalObject, ThrowScope& scope, BinaryOperator op, bool inPlace, JSValue left, JSValue right)
{
    return raiseTypeError(globalObject, scope, makeString("unsupported operand type(s) for "_s, inPlace ? inPlaceSymbolOf(op) : symbolOf(op), ": '"_s, typeName(globalObject, left), "' and '"_s, typeName(globalObject, right), '\''));
}

JSValue binaryOperation(JSGlobalObject* globalObject, BinaryOperator op, bool inPlace, JSValue left, JSValue right)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();

    if (!left.isObject() && !right.isObject()) {
        JSValue result = numberBinaryOperation(globalObject, op, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
    }
    if (isExact(globalObject, left) && isExact(globalObject, right)) {
        JSValue result = builtinBinaryOperation(globalObject, op, inPlace, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
    }

    PyType* leftType = typeOf(globalObject, left);
    PyType* rightType = typeOf(globalObject, right);

    // First as numbers: binary_op1() and binary_iop1() of CPython's Objects/abstract.c. What a built-in sequence has for + and * is not for this. It has its turn when this has come to nothing,
    // so that what it is added to or multiplied by is asked first.
    bool mayBeForSequences = op == BinaryOperator::Add || op == BinaryOperator::Mult;
    auto lookupForNumbers = [&] (PyType* type, const Identifier& name) -> JSValue {
        JSValue method = type->lookup(vm, name);
        return method && mayBeForSequences && isSequenceSlot(globalObject, method) ? JSValue() : method;
    };

    if (inPlace) {
        // A class that a program derives from list or bytearray has their += as a number has it, too. When CPython fills in what such a class can do, it takes what it finds as __iadd__ for
        // nb_inplace_add if that is wrapped as one of those would be, and it is. Nothing else of a sequence's is.
        bool isOfDerivedClass = op == BinaryOperator::Add && leftType->hasFlag(PyType::IsHeapType);
        if (JSValue method = isOfDerivedClass ? leftType->lookup(vm, names.inPlaceMethod(op)) : lookupForNumbers(leftType, names.inPlaceMethod(op))) {
            JSValue result = call(globalObject, method, left, right);
            RETURN_IF_EXCEPTION(scope, { });
            if (!isNotImplemented(globalObject, result))
                return result;
        }
    }

    JSValue leftMethod = lookupForNumbers(leftType, names.method(op));
    JSValue rightMethod = rightType != leftType ? lookupForNumbers(rightType, names.reflectedMethod(op)) : JSValue();
    // A class derived from the left operand's, that has its own idea of the operation, goes first.
    if (rightMethod && rightType->isSubtypeOf(leftType) && rightMethod != leftType->lookup(vm, names.reflectedMethod(op))) {
        JSValue result = call(globalObject, rightMethod, right, left);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNotImplemented(globalObject, result))
            return result;
        rightMethod = { };
    }
    if (leftMethod) {
        JSValue result = call(globalObject, leftMethod, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNotImplemented(globalObject, result))
            return result;
    }
    if (rightMethod) {
        JSValue result = call(globalObject, rightMethod, right, left);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNotImplemented(globalObject, result))
            return result;
    }

    // Then as sequences: the rest of PyNumber_Add(), PyNumber_Multiply() and their like.
    if (mayBeForSequences) {
        auto slotOf = [&] (PyType* type, bool wantsInPlace) -> JSValue {
            JSValue slot = wantsInPlace ? sequenceSlot(globalObject, type, names.inPlaceMethod(op)) : JSValue();
            return slot ? slot : sequenceSlot(globalObject, type, names.method(op));
        };
        if (JSValue slot = slotOf(leftType, inPlace)) {
            if (op == BinaryOperator::Add)
                RELEASE_AND_RETURN(scope, call(globalObject, slot, left, right));
            RELEASE_AND_RETURN(scope, sequenceRepeat(globalObject, slot, left, right));
        }
        // What is on the right is not to be changed. And with *= it is not come to at all if what is on the left can do anything that a sequence can, though it cannot do this: that is how
        // PyNumber_InPlaceMultiply() is written. Whatever is of a class that a program made can.
        auto isSomethingOfASequence = [&] {
            return leftType->hasFlag(PyType::IsHeapType) || leftType->lookup(vm, names.dunder_len) || leftType->lookup(vm, names.dunder_contains) || leftType->lookup(vm, names.dunder_getitem);
        };
        if (JSValue slot = op == BinaryOperator::Mult && !(inPlace && isSomethingOfASequence()) ? slotOf(rightType, false) : JSValue())
            RELEASE_AND_RETURN(scope, sequenceRepeat(globalObject, slot, right, left));
    }
    return raiseUnsupportedOperands(globalObject, scope, op, inPlace, left, right);
}

// What times `value` is one more than a multiple of `modulus`, which is positive. By Euclid's algorithm, as long_invmod() of CPython's
// Objects/longobject.c does it.
static JSValue inverseModulo(JSGlobalObject* globalObject, JSValue value, JSValue modulus)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue b = jsNumber(1);
    JSValue c = jsNumber(0);
    while (true) {
        bool isDone = !isTrue(globalObject, modulus);
        RETURN_IF_EXCEPTION(scope, { });
        if (isDone)
            break;
        JSValue quotient = binaryOperation(globalObject, BinaryOperator::FloorDiv, false, value, modulus);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue remainder = binaryOperation(globalObject, BinaryOperator::Mod, false, value, modulus);
        RETURN_IF_EXCEPTION(scope, { });
        value = modulus;
        modulus = remainder;
        JSValue product = binaryOperation(globalObject, BinaryOperator::Mult, false, quotient, c);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue next = binaryOperation(globalObject, BinaryOperator::Sub, false, b, product);
        RETURN_IF_EXCEPTION(scope, { });
        b = c;
        c = next;
    }
    if (!value.isInt32() || value.asInt32() != 1)
        return raiseValueError(globalObject, scope, "base is not invertible for the given modulus"_s);
    return b;
}

// What a class has for pow() with three arguments: nb_power. A class derived from one of the built-in ones has what that has, unless it says something of its own.
enum class PowerSlot : uint8_t { None, Int, Float, Complex, Methods };

static PowerSlot powerSlotOf(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    JSValue method = type->lookup(vm, names.method(BinaryOperator::Pow));
    JSValue reflected = type->lookup(vm, names.reflectedMethod(BinaryOperator::Pow));
    if (!method && !reflected)
        return PowerSlot::None;
    auto ownerOf = [] (JSValue function) -> JSObject* {
        auto* native = dynamicDowncast<PyNativeFunction>(function);
        return native ? native->owner() : nullptr;
    };
    JSObject* owner = ownerOf(method);
    if (!owner || owner != ownerOf(reflected))
        return PowerSlot::Methods;
    if (owner == realm->typeInt())
        return PowerSlot::Int;
    if (owner == realm->typeFloat())
        return PowerSlot::Float;
    if (owner == realm->typeComplex())
        return PowerSlot::Complex;
    return PowerSlot::Methods;
}

// slot_nb_power(), with a modulus
static JSValue powerByMethods(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue modulus)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSValue notImplemented = globalObject->pyRealm()->notImplemented();
    PyType* selfType = typeOf(globalObject, self);
    PyType* otherType = typeOf(globalObject, other);
    const Identifier& methodName = names.method(BinaryOperator::Pow);
    const Identifier& reflectedName = names.reflectedMethod(BinaryOperator::Pow);
    // vectorcall_maybe()
    auto callIfPresent = [&] (PyType* type, const Identifier& name, JSValue receiver, JSValue argument) -> JSValue {
        JSValue method = type->lookup(vm, name);
        if (!method)
            return notImplemented;
        return call(globalObject, method, receiver, argument, modulus);
    };
    bool triesOther = selfType != otherType && powerSlotOf(globalObject, otherType) == PowerSlot::Methods;
    if (powerSlotOf(globalObject, selfType) == PowerSlot::Methods) {
        if (triesOther && otherType->isSubtypeOf(selfType)) {
            JSValue reflected = otherType->lookup(vm, reflectedName);
            if (reflected && reflected != selfType->lookup(vm, reflectedName)) {
                JSValue result = call(globalObject, reflected, other, self, modulus);
                RETURN_IF_EXCEPTION(scope, { });
                if (result != notImplemented)
                    return result;
                triesOther = false;
            }
        }
        JSValue result = callIfPresent(selfType, methodName, self, other);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented || otherType == selfType)
            return result;
    }
    if (triesOther)
        RELEASE_AND_RETURN(scope, callIfPresent(otherType, reflectedName, other, self));
    return notImplemented;
}

// ternary_op()
JSValue power(JSGlobalObject* globalObject, JSValue base, JSValue exponent, JSValue modulus)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(modulus))
        RELEASE_AND_RETURN(scope, binaryOperation(globalObject, BinaryOperator::Pow, false, base, exponent));

    JSValue notImplemented = globalObject->pyRealm()->notImplemented();
    auto isNumber = [&] (JSValue value) { return classify(value) || isInstance(globalObject, value, globalObject->pyRealm()->typeComplex()); };
    auto apply = [&] (PowerSlot slot) -> JSValue {
        switch (slot) {
        case PowerSlot::None:
            break;
        case PowerSlot::Int:
            // long_pow()
            if (!classify(base).isInt() || !classify(exponent).isInt() || !classify(modulus).isInt())
                return notImplemented;
            return powerOfInts(globalObject, base, exponent, modulus);
        case PowerSlot::Float:
            // float_pow()
            return raiseTypeError(globalObject, scope, "pow() 3rd argument not allowed unless all arguments are integers"_s);
        case PowerSlot::Complex:
            // complex_pow()
            if (!isNumber(base) || !isNumber(exponent))
                return notImplemented;
            return raiseValueError(globalObject, scope, "complex modulo"_s);
        case PowerSlot::Methods:
            return powerByMethods(globalObject, base, exponent, modulus);
        }
        return notImplemented;
    };
    PyType* baseType = typeOf(globalObject, base);
    PyType* exponentType = typeOf(globalObject, exponent);
    PowerSlot baseSlot = powerSlotOf(globalObject, baseType);
    PowerSlot exponentSlot = exponentType != baseType ? powerSlotOf(globalObject, exponentType) : PowerSlot::None;
    if (exponentSlot == baseSlot)
        exponentSlot = PowerSlot::None;
    if (baseSlot != PowerSlot::None) {
        if (exponentSlot != PowerSlot::None && exponentType->isSubtypeOf(baseType)) {
            JSValue result = apply(exponentSlot);
            RETURN_IF_EXCEPTION(scope, { });
            if (result != notImplemented)
                return result;
            exponentSlot = PowerSlot::None;
        }
        JSValue result = apply(baseSlot);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
    }
    if (exponentSlot != PowerSlot::None) {
        JSValue result = apply(exponentSlot);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
    }
    PowerSlot modulusSlot = powerSlotOf(globalObject, typeOf(globalObject, modulus));
    if (modulusSlot != PowerSlot::None && modulusSlot != baseSlot && modulusSlot != exponentSlot) {
        JSValue result = apply(modulusSlot);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
    }
    return raiseTypeError(globalObject, scope, makeString("unsupported operand type(s) for ** or pow(): '"_s, typeName(globalObject, base), "', '"_s, typeName(globalObject, exponent), "', '"_s, typeName(globalObject, modulus), '\''));
}

JSValue powerOfInts(JSGlobalObject* globalObject, JSValue base, JSValue exponent, JSValue modulus)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Whatever classes derived from int they may be of, and whatever those have to say about the operators, it is as ints that they are taken.
    for (JSValue* value : { &base, &exponent, &modulus }) {
        if (auto* boxed = tryBoxedValue(*value))
            *value = boxed->value();
    }
    Number e = classify(exponent);
    Number m = classify(modulus);
    if (m.kind == Number::Kind::Small && !m.small)
        return raiseValueError(globalObject, scope, "pow() 3rd argument cannot be 0"_s);
    JSValue remaining = exponent.isBoolean() ? jsNumber(exponent.asBoolean()) : exponent;
    if (e.kind == Number::Kind::Small ? e.small < 0 : e.big->sign()) {
        // The inverse of the base, to the power without its sign.
        JSValue magnitude = modulus;
        if (m.kind == Number::Kind::Small ? m.small < 0 : m.big->sign()) {
            magnitude = unaryOperation(globalObject, UnaryOperator::USub, modulus);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (magnitude.isInt32() && magnitude.asInt32() == 1)
            return jsNumber(0);
        base = inverseModulo(globalObject, base.isBoolean() ? jsNumber(base.asBoolean()) : base, magnitude);
        RETURN_IF_EXCEPTION(scope, { });
        remaining = unaryOperation(globalObject, UnaryOperator::USub, remaining);
        RETURN_IF_EXCEPTION(scope, { });
    }
    // By squaring, reducing as it goes.
    JSValue result = jsNumber(1);
    JSValue factor = binaryOperation(globalObject, BinaryOperator::Mod, false, base, modulus);
    RETURN_IF_EXCEPTION(scope, { });
    while (true) {
        bool isZero = !isTrue(globalObject, remaining);
        RETURN_IF_EXCEPTION(scope, { });
        if (isZero)
            break;
        JSValue bit = binaryOperation(globalObject, BinaryOperator::BitAnd, false, remaining, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, { });
        if (bit.asInt32()) {
            result = binaryOperation(globalObject, BinaryOperator::Mult, false, result, factor);
            RETURN_IF_EXCEPTION(scope, { });
            result = binaryOperation(globalObject, BinaryOperator::Mod, false, result, modulus);
            RETURN_IF_EXCEPTION(scope, { });
        }
        factor = binaryOperation(globalObject, BinaryOperator::Mult, false, factor, factor);
        RETURN_IF_EXCEPTION(scope, { });
        factor = binaryOperation(globalObject, BinaryOperator::Mod, false, factor, modulus);
        RETURN_IF_EXCEPTION(scope, { });
        remaining = binaryOperation(globalObject, BinaryOperator::RShift, false, remaining, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, binaryOperation(globalObject, BinaryOperator::Mod, false, result, modulus));
}

JSValue divmod(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (classify(left) && classify(right)) {
        JSValue quotient = binaryOperation(globalObject, BinaryOperator::FloorDiv, false, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue remainder = binaryOperation(globalObject, BinaryOperator::Mod, false, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        return PyTuple::create(globalObject, { quotient, remainder });
    }
    auto& names = vm.pythonNames();
    if (JSValue method = typeOf(globalObject, left)->lookup(vm, names.dunder_divmod)) {
        JSValue result = call(globalObject, method, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNotImplemented(globalObject, result))
            return result;
    }
    if (JSValue method = typeOf(globalObject, right)->lookup(vm, names.dunder_rdivmod)) {
        JSValue result = call(globalObject, method, right, left);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNotImplemented(globalObject, result))
            return result;
    }
    return raiseTypeError(globalObject, scope, makeString("unsupported operand type(s) for divmod(): '"_s, typeName(globalObject, left), "' and '"_s, typeName(globalObject, right), '\''));
}

// ---- Unary operators

JSValue unaryOperation(JSGlobalObject* globalObject, UnaryOperator op, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (op == UnaryOperator::Not) {
        bool truth = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        return jsBoolean(!truth);
    }
    if (!value.isObject()) {
        JSValue result = numberUnaryOperation(globalObject, op, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
    }
    auto& names = vm.pythonNames();
    const Identifier& name = op == UnaryOperator::USub ? names.dunder_neg : op == UnaryOperator::UAdd ? names.dunder_pos : names.dunder_invert;
    JSValue self;
    JSValue method = lookupSpecial(globalObject, value, name, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (method)
        RELEASE_AND_RETURN(scope, callMethod(globalObject, method, self));
    ASCIILiteral symbol = op == UnaryOperator::USub ? "-"_s : op == UnaryOperator::UAdd ? "+"_s : "~"_s;
    return raiseTypeError(globalObject, scope, makeString("bad operand type for unary "_s, symbol, ": '"_s, typeName(globalObject, value), '\''));
}

// ---- Truth

bool isTrue(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isBoolean())
        return value.asBoolean();
    if (value.isInt32())
        return value.asInt32();
    if (value.isUndefinedOrNull())
        return false;
    if (value.isDouble())
        return value.asDouble() != 0; // A NaN is true.

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSCell* cell = value.asCell();
    switch (cell->type()) {
    case StringType:
        return uncheckedDowncast<JSString>(cell)->length();
    case HeapBigIntType:
        return true; // Zero is not a BigInt.
    case JSFunctionType:
    case PyTypeType:
    case PyBoundMethodType:
        return true;
    default:
        break;
    }
    if (isExact(globalObject, value)) {
        switch (cell->type()) {
        case PyTupleType:
            return uncheckedDowncast<PyTuple>(cell)->length();
        case PyDictType:
            return uncheckedDowncast<PyDict>(cell)->size();
        case PySetType:
            return uncheckedDowncast<PySet>(cell)->size();
        case PyRangeType:
            return !uncheckedDowncast<PyRange>(cell)->isEmpty();
        default:
            if (isListCell(cell))
                return uncheckedDowncast<JSArray>(cell)->length();
            break;
        }
    }

    auto& names = vm.pythonNames();
    JSValue self;
    JSValue method = lookupSpecial(globalObject, value, names.dunder_bool, self);
    RETURN_IF_EXCEPTION(scope, false);
    if (method) {
        JSValue result = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, false);
        if (!result.isBoolean()) {
            raiseTypeError(globalObject, scope, makeString("__bool__ should return bool, returned "_s, typeName(globalObject, result)));
            return false;
        }
        return result.asBoolean();
    }
    if (typeOf(globalObject, value)->lookup(vm, names.dunder_len)) {
        int64_t size = length(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        return size;
    }
    return true;
}

// ---- Comparison

bool isIdentical(JSValue left, JSValue right)
{
    if (left == right)
        return true;
    // Numbers are not objects here, so two of them are the same one if nothing can tell them apart.
    if (left.isNumber() && right.isNumber()) {
        auto leftInteger = taggedInteger(left);
        auto rightInteger = taggedInteger(right);
        if (leftInteger || rightInteger)
            return leftInteger == rightInteger;
        return std::bit_cast<uint64_t>(left.asNumber()) == std::bit_cast<uint64_t>(right.asNumber());
    }
    return left.isUndefinedOrNull() && right.isUndefinedOrNull();
}

static ASCIILiteral symbolOf(ComparisonOperator op)
{
    static constexpr ASCIILiteral symbols[] = { "=="_s, "!="_s, "<"_s, "<="_s, ">"_s, ">="_s };
    return symbols[static_cast<unsigned>(op)];
}

static bool resultOf(ComparisonOperator op, int order)
{
    switch (op) {
    case ComparisonOperator::Eq:
        return !order;
    case ComparisonOperator::NotEq:
        return order;
    case ComparisonOperator::Lt:
        return order < 0;
    case ComparisonOperator::LtE:
        return order <= 0;
    case ComparisonOperator::Gt:
        return order > 0;
    case ComparisonOperator::GtE:
        return order >= 0;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// Of lists and of tuples: by the first place where they differ.
template<typename Get>
static JSValue compareSequences(JSGlobalObject* globalObject, ComparisonOperator op, unsigned leftLength, unsigned rightLength, const Get& get)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (leftLength != rightLength && (op == ComparisonOperator::Eq || op == ComparisonOperator::NotEq))
        return jsBoolean(op == ComparisonOperator::NotEq);
    unsigned common = std::min(leftLength, rightLength);
    for (unsigned i = 0; i < common; ++i) {
        auto [a, b] = get(i);
        RETURN_IF_EXCEPTION(scope, { });
        bool same = isEqual(globalObject, a, b);
        RETURN_IF_EXCEPTION(scope, { });
        if (same)
            continue;
        if (op == ComparisonOperator::Eq)
            return jsBoolean(false);
        if (op == ComparisonOperator::NotEq)
            return jsBoolean(true);
        RELEASE_AND_RETURN(scope, compare(globalObject, op, a, b));
    }
    return jsBoolean(resultOf(op, leftLength < rightLength ? -1 : leftLength > rightLength));
}

// What the built-in types do. Empty if it is not for them.
JSValue builtinCompare(JSGlobalObject* globalObject, ComparisonOperator op, JSValue left, JSValue right)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isEquality = op == ComparisonOperator::Eq || op == ComparisonOperator::NotEq;
    left = stringIfHasOne(left);
    right = stringIfHasOne(right);

    if (Number a = classify(left)) {
        Number b = classify(right);
        if (!b)
            return { };
        bool isUnordered;
        int order = *numberCompare(a, b, isUnordered);
        if (isUnordered)
            return jsBoolean(op == ComparisonOperator::NotEq);
        return jsBoolean(resultOf(op, order));
    }
    if (left.isString()) {
        if (!right.isString())
            return { };
        if (isEquality) {
            bool same = asString(left)->equal(globalObject, asString(right));
            RETURN_IF_EXCEPTION(scope, { });
            return jsBoolean(same == (op == ComparisonOperator::Eq));
        }
        auto a = asString(left)->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        auto b = asString(right)->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        return jsBoolean(resultOf(op, compareStrings(a, b)));
    }
    if (!left.isCell() || !right.isCell())
        return { };

    if (isTuple(left) && isTuple(right)) {
        auto* a = uncheckedDowncast<PyTuple>(left.asCell());
        auto* b = uncheckedDowncast<PyTuple>(right.asCell());
        RELEASE_AND_RETURN(scope, compareSequences(globalObject, op, a->length(), b->length(), [&] (unsigned i) { return std::pair { a->at(i), b->at(i) }; }));
    }
    if (isList(left) && isList(right)) {
        JSArray* a = asList(left);
        JSArray* b = asList(right);
        RELEASE_AND_RETURN(scope, compareSequences(globalObject, op, a->length(), b->length(), [&] (unsigned i) { return std::pair { listGet(globalObject, a, i), listGet(globalObject, b, i) }; }));
    }
    if (isDict(left) && isDict(right)) {
        if (!isEquality)
            return { };
        auto* a = uncheckedDowncast<PyDict>(left.asCell());
        auto* b = uncheckedDowncast<PyDict>(right.asCell());
        bool same = a->size() == b->size();
        if (same) {
            a->forEach(globalObject, [&] (JSValue key, JSValue value) {
                JSValue other = b->get(globalObject, key);
                same = other && isEqual(globalObject, value, other);
                return same;
            });
            RETURN_IF_EXCEPTION(scope, { });
        }
        return jsBoolean(same == (op == ComparisonOperator::Eq));
    }
    if (isSet(left) && isSet(right))
        RELEASE_AND_RETURN(scope, setCompare(globalObject, op, uncheckedDowncast<PySet>(left.asCell()), uncheckedDowncast<PySet>(right.asCell())));
    if (auto* a = trySlice(left)) {
        auto* b = trySlice(right);
        if (!b)
            return { };
        // As (start, stop, step) and (start, stop, step) would.
        if (a == b)
            return jsBoolean(op == ComparisonOperator::Eq || op == ComparisonOperator::LtE || op == ComparisonOperator::GtE);
        JSValue ofA[] = { a->start(), a->stop(), a->step() };
        JSValue ofB[] = { b->start(), b->stop(), b->step() };
        RELEASE_AND_RETURN(scope, compareSequences(globalObject, op, 3, 3, [&] (unsigned i) { return std::pair { ofA[i], ofB[i] }; }));
    }
    if (auto* a = tryRange(left)) {
        auto* b = tryRange(right);
        if (!b || !isEquality)
            return { };
        return jsBoolean(rangesAreEqual(a, b) == (op == ComparisonOperator::Eq));
    }
    return { };
}

static const Identifier& methodFor(CommonNames& names, ComparisonOperator op)
{
    switch (op) {
    case ComparisonOperator::Eq:
        return names.dunder_eq;
    case ComparisonOperator::NotEq:
        return names.dunder_ne;
    case ComparisonOperator::Lt:
        return names.dunder_lt;
    case ComparisonOperator::LtE:
        return names.dunder_le;
    case ComparisonOperator::Gt:
        return names.dunder_gt;
    case ComparisonOperator::GtE:
        return names.dunder_ge;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static ComparisonOperator swapped(ComparisonOperator op)
{
    switch (op) {
    case ComparisonOperator::Lt:
        return ComparisonOperator::Gt;
    case ComparisonOperator::LtE:
        return ComparisonOperator::GtE;
    case ComparisonOperator::Gt:
        return ComparisonOperator::Lt;
    case ComparisonOperator::GtE:
        return ComparisonOperator::LtE;
    default:
        return op;
    }
}

static bool exceptionMatches(JSGlobalObject* globalObject, JSValue exception, JSValue pattern)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto isExceptionClass = [] (JSValue value) { return isClass(value) && asType(value)->isExceptionType(); };
    auto complain = [&] {
        raiseTypeError(globalObject, scope, "catching classes that do not inherit from BaseException is not allowed"_s);
        return false;
    };
    if (isTuple(pattern)) {
        // All of them are looked at before any is tried, and a tuple in the tuple will not do.
        auto entries = uncheckedDowncast<PyTuple>(pattern.asCell())->span();
        for (auto& entry : entries) {
            if (!isExceptionClass(entry.get()))
                return complain();
        }
        for (auto& entry : entries) {
            if (isInstance(globalObject, exception, asType(entry.get())))
                return true;
        }
        return false;
    }
    if (!isExceptionClass(pattern))
        return complain();
    return isInstance(globalObject, exception, asType(pattern));
}

JSValue compare(JSGlobalObject* globalObject, ComparisonOperator op, JSValue left, JSValue right)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    switch (op) {
    case ComparisonOperator::Is:
        return jsBoolean(isIdentical(left, right));
    case ComparisonOperator::IsNot:
        return jsBoolean(!isIdentical(left, right));
    case ComparisonOperator::In:
    case ComparisonOperator::NotIn: {
        bool found = contains(globalObject, right, left);
        RETURN_IF_EXCEPTION(scope, { });
        return jsBoolean(found == (op == ComparisonOperator::In));
    }
    case ComparisonOperator::ExceptionMatch:
        RELEASE_AND_RETURN(scope, jsBoolean(exceptionMatches(globalObject, left, right)));
    default:
        break;
    }

    if (isExact(globalObject, left) && isExact(globalObject, right)) {
        JSValue result = builtinCompare(globalObject, op, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
    }

    auto& names = vm.pythonNames();
    PyType* leftType = typeOf(globalObject, left);
    PyType* rightType = typeOf(globalObject, right);
    bool triedReflected = false;
    auto tryReflected = [&] () -> JSValue {
        triedReflected = true;
        JSValue method = rightType->lookup(vm, methodFor(names, swapped(op)));
        if (!method)
            return { };
        JSValue result = call(globalObject, method, right, left);
        RETURN_IF_EXCEPTION(scope, { });
        return isNotImplemented(globalObject, result) ? JSValue() : result;
    };

    if (rightType != leftType && rightType->isSubtypeOf(leftType)) {
        JSValue result = tryReflected();
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
    }
    if (JSValue method = leftType->lookup(vm, methodFor(names, op))) {
        JSValue result = call(globalObject, method, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNotImplemented(globalObject, result))
            return result;
    }
    if (!triedReflected) {
        JSValue result = tryReflected();
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
    }
    if (op == ComparisonOperator::Eq)
        return jsBoolean(isIdentical(left, right));
    if (op == ComparisonOperator::NotEq)
        return jsBoolean(!isIdentical(left, right));
    return raiseTypeError(globalObject, scope, makeString('\'', symbolOf(op), "' not supported between instances of '"_s, leftType->nameString(globalObject), "' and '"_s, rightType->nameString(globalObject), '\''));
}

bool isEqual(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    if (isIdentical(left, right))
        return true;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = compare(globalObject, ComparisonOperator::Eq, left, right);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
}

// What the built-in containers do. Nothing if it is none of them.
std::optional<bool> builtinContains(JSGlobalObject* globalObject, JSValue container, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* boxed = tryBoxedValue(container))
        container = boxed->value();
    if (!container.isCell())
        return std::nullopt;
    {
        JSCell* cell = container.asCell();
        switch (cell->type()) {
        case StringType: {
            value = stringIfHasOne(value);
            if (!value.isString()) {
                raiseTypeError(globalObject, scope, makeString("'in <string>' requires string as left operand, not "_s, typeName(globalObject, value)));
                return false;
            }
            auto haystack = asString(container)->view(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            auto needle = asString(value)->view(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            return haystack->find(needle) != notFound;
        }
        case PyDictType:
            RELEASE_AND_RETURN(scope, uncheckedDowncast<PyDict>(cell)->contains(globalObject, value));
        case PySetType: {
            value = keyToLookForInSet(globalObject, value);
            RETURN_IF_EXCEPTION(scope, false);
            int entry = uncheckedDowncast<PySet>(cell)->find(globalObject, value);
            RETURN_IF_EXCEPTION(scope, false);
            return entry >= 0;
        }
        case PyTupleType: {
            auto* tuple = uncheckedDowncast<PyTuple>(cell);
            for (unsigned i = 0; i < tuple->length(); ++i) {
                bool same = isEqual(globalObject, tuple->at(i), value);
                RETURN_IF_EXCEPTION(scope, false);
                if (same)
                    return true;
            }
            return false;
        }
        case PyRangeType: {
            RELEASE_AND_RETURN(scope, rangeContains(globalObject, uncheckedDowncast<PyRange>(cell), value));
        }
        default:
            if (isListCell(cell)) {
                auto* list = uncheckedDowncast<JSArray>(cell);
                for (unsigned i = 0; i < list->length(); ++i) {
                    JSValue item = listGet(globalObject, list, i);
                    RETURN_IF_EXCEPTION(scope, false);
                    bool same = isEqual(globalObject, item, value);
                    RETURN_IF_EXCEPTION(scope, false);
                    if (same)
                        return true;
                }
                return false;
            }
            break;
        }
    }
    return std::nullopt;
}

bool contains(JSGlobalObject* globalObject, JSValue container, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (isExact(globalObject, container)) {
        auto result = builtinContains(globalObject, container, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (result)
            return *result;
    }

    JSValue self;
    JSValue method = lookupSpecial(globalObject, container, vm.pythonNames().dunder_contains, self);
    RETURN_IF_EXCEPTION(scope, false);
    if (method && !isNone(method)) {
        JSValue result = callMethod(globalObject, method, self, value);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
    }

    // Otherwise, by going through it.
    JSValue iterator = getIterator(globalObject, container);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::TypeError))
            raiseTypeError(globalObject, scope, makeString("argument of type '"_s, typeName(globalObject, container), "' is not a container or iterable"_s));
        return false;
    }
    while (true) {
        JSValue item = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!item)
            return false;
        bool same = isEqual(globalObject, item, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (same)
            return true;
    }
}

// ---- hash

int64_t hashOfString(const String& string)
{
    // Which is nothing like CPython's, but that one's is different every time it is run.
    return string.isEmpty() ? 0 : static_cast<int64_t>(string.impl()->hash()) + 1;
}

int64_t hashOfPointer(const void* pointer)
{
    // Cells are aligned to 16 bytes, so the low bits say nothing.
    uint64_t bits = std::bit_cast<uintptr_t>(pointer);
    int64_t result = static_cast<int64_t>((bits >> 4) | (bits << 60));
    return result == -1 ? -2 : result;
}

// tuplehash() of CPython's Objects/tupleobject.c, which is a simplified xxHash. slicehash() of Objects/sliceobject.c is the same but for the length.
template<typename At>
static int64_t hashOfValues(JSGlobalObject* globalObject, unsigned count, bool mixesInLength, const At& at)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr uint64_t prime1 = 11400714785074694791ULL;
    constexpr uint64_t prime2 = 14029467366897019727ULL;
    constexpr uint64_t prime5 = 2870177450012600261ULL;
    uint64_t accumulator = prime5;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t lane = static_cast<uint64_t>(hash(globalObject, at(i)));
        RETURN_IF_EXCEPTION(scope, -1);
        accumulator += lane * prime2;
        accumulator = (accumulator << 31) | (accumulator >> 33);
        accumulator *= prime1;
    }
    if (mixesInLength)
        accumulator += count ^ (prime5 ^ 3527539ULL);
    if (accumulator == static_cast<uint64_t>(-1))
        return 1546275796;
    return static_cast<int64_t>(accumulator);
}

static int64_t hashOfTuple(JSGlobalObject* globalObject, PyTuple* tuple)
{
    return hashOfValues(globalObject, tuple->length(), true, [&] (unsigned i) { return tuple->at(i); });
}

static int64_t hashOfSlice(JSGlobalObject* globalObject, PySlice* slice)
{
    JSValue parts[] = { slice->start(), slice->stop(), slice->step() };
    return hashOfValues(globalObject, 3, false, [&] (unsigned i) { return parts[i]; });
}

// frozenset_hash() of CPython's Objects/setobject.c. It does not depend on the order.
static int64_t hashOfFrozenSet(JSGlobalObject* globalObject, PySet* set)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto shuffle = [] (uint64_t h) { return ((h ^ 89869747ULL) ^ (h << 16)) * 3644798167ULL; };
    uint64_t result = 0;
    for (unsigned entry = 0; entry < set->entryCount(); ++entry) {
        JSValue key = set->keyAt(entry);
        if (!key)
            continue;
        uint64_t h = static_cast<uint64_t>(hash(globalObject, key));
        RETURN_IF_EXCEPTION(scope, -1);
        result ^= shuffle(h);
    }
    result ^= (static_cast<uint64_t>(set->size()) + 1) * 1927868237ULL;
    result ^= (result >> 11) ^ (result >> 25);
    result = result * 69069U + 907133923ULL;
    if (result == static_cast<uint64_t>(-1))
        result = 590923713ULL;
    return static_cast<int64_t>(result);
}

int64_t hash(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!value.isCell()) {
        if (value.isUndefinedOrNull())
            return 0xfca86420;
        return hashOfNumber(globalObject, classify(value));
    }
    JSCell* cell = value.asCell();
    switch (cell->type()) {
    case StringType: {
        String string = uncheckedDowncast<JSString>(cell)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, -1);
        return hashOfString(string);
    }
    case HeapBigIntType:
        return hashOfNumber(globalObject, classify(value));
    case JSFunctionType:
        // One that is bound to an object is equal to another that is: see JSFunction.__eq__.
        if (cell->inherits<JSBoundFunction>())
            break;
        return hashOfPointer(cell);
    case SymbolType:
        return hashOfPointer(cell);
    default:
        break;
    }

    PyRealm* realm = globalObject->pyRealm();
    PyType* type = typeOf(globalObject, value);
    if (type == realm->typeTuple())
        RELEASE_AND_RETURN(scope, hashOfTuple(globalObject, uncheckedDowncast<PyTuple>(cell)));

    JSValue method = type->lookup(vm, vm.pythonNames().dunder_hash);
    if (!method || isNone(method)) {
        raiseTypeError(globalObject, scope, makeString("unhashable type: '"_s, type->nameString(globalObject), '\''));
        return -1;
    }
    if (method.asCell() == realm->function(PyRealm::WellKnownFunction::ObjectHash))
        return hashOfPointer(cell);
    JSValue result = call(globalObject, method, value);
    RETURN_IF_EXCEPTION(scope, -1);
    Number number = classify(result);
    if (!number.isInt()) {
        raiseTypeError(globalObject, scope, "__hash__ method should return an integer"_s);
        return -1;
    }
    // It is the hash, if it fits. Otherwise its own hash is.
    if (auto small = tryInt64(result))
        return *small == -1 ? -2 : *small;
    return hashOfNumber(globalObject, number);
}

int64_t builtinHash(JSGlobalObject* globalObject, JSValue value)
{
    if (isTuple(value))
        return hashOfTuple(globalObject, uncheckedDowncast<PyTuple>(value.asCell()));
    if (isSet(value))
        return hashOfFrozenSet(globalObject, uncheckedDowncast<PySet>(value.asCell()));
    if (auto* slice = trySlice(value))
        return hashOfSlice(globalObject, slice);
    if (auto* boxed = tryBoxedValue(value))
        return hash(globalObject, boxed->value());
    if (auto* range = tryRange(value)) {
        // Of what decides whether two are equal.
        if (range->isEmpty())
            return hashOfTuple(globalObject, PyTuple::create(globalObject, { range->length(), jsUndefined(), jsUndefined() }));
        if (range->length().isInt32() && range->length().asInt32() == 1)
            return hashOfTuple(globalObject, PyTuple::create(globalObject, { range->length(), range->start(), jsUndefined() }));
        return hashOfTuple(globalObject, PyTuple::create(globalObject, { range->length(), range->start(), range->step() }));
    }
    if (value.isCell() && value.isObject())
        return hashOfPointer(value.asCell());
    return hash(globalObject, value);
}

// ---- Numbers, from anything

JSValue toInt(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Number number = classify(value);
    if (!number.isInt()) {
        JSValue self;
        JSValue method = number ? JSValue() : lookupSpecial(globalObject, value, vm.pythonNames().dunder_index, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (!method)
            return raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, value), "' object cannot be interpreted as an integer"_s));
        JSValue result = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, { });
        number = classify(result);
        if (!number.isInt())
            return raiseTypeError(globalObject, scope, makeString("__index__ returned non-int (type "_s, typeName(globalObject, result), ')'));
        if (!warnIfOfStrictSubclass(globalObject, result, BuiltinType::Int, "__index__ returned non-int"_s, "int"_s))
            return { };
    }
    return number.kind == Number::Kind::Small ? jsNumber(number.small) : JSValue(number.big);
}

std::optional<int64_t> tryInt64(JSValue value)
{
    Number number = classify(value);
    ASSERT(number.isInt());
    if (number.kind == Number::Kind::Small)
        return number.small;
    if (number.big->length() > 1)
        return std::nullopt;
    uint64_t magnitude = number.big->digit(0);
    if (magnitude <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return number.big->sign() ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
    if (number.big->sign() && magnitude == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1)
        return std::numeric_limits<int64_t>::min();
    return std::nullopt;
}

// What is done about an int that is too large.
enum class IfTooLarge : uint8_t { Clamp, IndexError, OverflowError, TooLargeForSsize, TooLargeForLong, TooLargeForInt };

static std::optional<int64_t> toInt64(JSGlobalObject* globalObject, JSValue value, IfTooLarge ifTooLarge)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue integer = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    Number number = classify(integer);
    if (number.kind == Number::Kind::Small)
        return number.small;
    if (number.big->length() <= 1) {
        uint64_t magnitude = number.big->digit(0);
        if (magnitude <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return number.big->sign() ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
        if (number.big->sign() && magnitude == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1)
            return std::numeric_limits<int64_t>::min();
    }
    switch (ifTooLarge) {
    case IfTooLarge::Clamp:
        return number.big->sign() ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
    case IfTooLarge::IndexError:
    case IfTooLarge::OverflowError:
        raise(globalObject, scope, ifTooLarge == IfTooLarge::IndexError ? BuiltinType::IndexError : BuiltinType::OverflowError, makeString("cannot fit '"_s, typeName(globalObject, value), "' into an index-sized integer"_s));
        break;
    case IfTooLarge::TooLargeForSsize:
        raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C ssize_t"_s);
        break;
    case IfTooLarge::TooLargeForLong:
        raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C long"_s);
        break;
    case IfTooLarge::TooLargeForInt:
        raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C int"_s);
        break;
    }
    return std::nullopt;
}

std::optional<int64_t> toIndex(JSGlobalObject* globalObject, JSValue value, bool clamp)
{
    return toInt64(globalObject, value, clamp ? IfTooLarge::Clamp : IfTooLarge::IndexError);
}

std::optional<int64_t> toIndexOrOverflow(JSGlobalObject* globalObject, JSValue value)
{
    return toInt64(globalObject, value, IfTooLarge::OverflowError);
}

std::optional<int64_t> toSsize(JSGlobalObject* globalObject, JSValue value)
{
    return toInt64(globalObject, value, IfTooLarge::TooLargeForSsize);
}

std::optional<int64_t> toCLong(JSGlobalObject* globalObject, JSValue value)
{
    return toInt64(globalObject, value, IfTooLarge::TooLargeForLong);
}

std::optional<int> toCInt(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto result = toInt64(globalObject, value, IfTooLarge::TooLargeForInt);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (*result > std::numeric_limits<int>::max() || *result < std::numeric_limits<int>::min()) {
        raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C int"_s);
        return std::nullopt;
    }
    return static_cast<int>(*result);
}

std::optional<int64_t> toSliceIndex(JSGlobalObject* globalObject, JSValue value, bool mayBeNone)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!classify(value).isInt() && !typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_index)) {
        raiseTypeError(globalObject, scope, mayBeNone ? "slice indices must be integers or None or have an __index__ method"_s : "slice indices must be integers or have an __index__ method"_s);
        return std::nullopt;
    }
    RELEASE_AND_RETURN(scope, toInt64(globalObject, value, IfTooLarge::Clamp));
}

std::optional<double> toDouble(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (Number number = classify(value)) {
        double result = toDouble(globalObject, scope, number);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        return result;
    }
    auto& names = vm.pythonNames();
    JSValue self;
    JSValue method = lookupSpecial(globalObject, value, names.dunder_float, self);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (method) {
        JSValue result = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        Number number = classify(result);
        if (number.kind != Number::Kind::Float) {
            raiseTypeError(globalObject, scope, makeString(typeName(globalObject, value), ".__float__ returned non-float (type "_s, typeName(globalObject, result), ')'));
            return std::nullopt;
        }
        if (!warnIfOfStrictSubclass(globalObject, result, BuiltinType::Float, makeString(typeName(globalObject, value), ".__float__ returned non-float"_s), "float"_s))
            return std::nullopt;
        return number.real;
    }
    method = lookupSpecial(globalObject, value, names.dunder_index, self);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (method) {
        JSValue result = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        Number number = classify(result);
        if (number.isInt()) {
            if (!warnIfOfStrictSubclass(globalObject, result, BuiltinType::Int, "__index__ returned non-int"_s, "int"_s))
                return std::nullopt;
            double converted = toDouble(globalObject, scope, number);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            return converted;
        }
    }
    raiseTypeError(globalObject, scope, makeString("must be real number, not "_s, typeName(globalObject, value)));
    return std::nullopt;
}

// ---- len()

// Of the built-in containers. -1 if it is none of them.
int64_t builtinLength(JSGlobalObject* globalObject, JSValue value)
{
    if (auto* boxed = tryBoxedValue(value))
        value = boxed->value();
    if (!value.isCell())
        return -1;
    JSCell* cell = value.asCell();
    switch (cell->type()) {
    case StringType:
        return stringLength(globalObject, uncheckedDowncast<JSString>(cell));
    case PyTupleType:
        return uncheckedDowncast<PyTuple>(cell)->length();
    case PyDictType:
        return uncheckedDowncast<PyDict>(cell)->size();
    case PySetType:
        return uncheckedDowncast<PySet>(cell)->size();
    case PyRangeType:
        return rangeLength(globalObject, uncheckedDowncast<PyRange>(cell));
    default:
        return isListCell(cell) ? static_cast<int64_t>(uncheckedDowncast<JSArray>(cell)->length()) : -1;
    }
}

int64_t length(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isExact(globalObject, value)) {
        int64_t result = builtinLength(globalObject, value);
        if (result >= 0)
            return result;
    }
    JSValue self;
    JSValue method = lookupSpecial(globalObject, value, vm.pythonNames().dunder_len, self);
    RETURN_IF_EXCEPTION(scope, -1);
    if (!method) {
        raiseTypeError(globalObject, scope, makeString("object of type '"_s, typeName(globalObject, value), "' has no len()"_s));
        return -1;
    }
    JSValue result = callMethod(globalObject, method, self);
    RETURN_IF_EXCEPTION(scope, -1);
    auto size = toIndex(globalObject, result);
    RETURN_IF_EXCEPTION(scope, -1);
    if (*size < 0) {
        raiseValueError(globalObject, scope, "__len__() should return >= 0"_s);
        return -1;
    }
    return *size;
}

// ---- Subscripts

// The place in a sequence of the length that an index means. Nothing if it is outside, and then IndexError has been raised.
static std::optional<unsigned> normalizeIndex(JSGlobalObject* globalObject, ThrowScope& scope, JSValue key, int64_t length, ASCIILiteral what)
{
    auto index = toIndex(globalObject, key);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    int64_t i = *index;
    if (i < 0)
        i += length;
    if (i < 0 || i >= length) {
        raise(globalObject, scope, BuiltinType::IndexError, makeString(what, " out of range"_s));
        return std::nullopt;
    }
    return static_cast<unsigned>(i);
}

static bool isIndexLike(JSGlobalObject* globalObject, JSValue key)
{
    if (classify(key).isInt())
        return true;
    return key.isObject() && typeOf(globalObject, key)->lookup(globalObject->vm(), globalObject->vm().pythonNames().dunder_index);
}

// What the built-in sequences and dict do. Empty, with nothing raised, if `base` is none of them.
JSValue builtinGetItem(JSGlobalObject* globalObject, JSValue base, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!base.isCell())
        return { };
    JSCell* cell = base.asCell();

    if (isListCell(cell)) {
        auto* list = uncheckedDowncast<JSArray>(cell);
        if (auto* slice = trySlice(key)) {
            auto indices = slice->indices(globalObject, [&] { return list->length(); });
            RETURN_IF_EXCEPTION(scope, { });
            JSArray* result = newList(globalObject, indices->length);
            RETURN_IF_EXCEPTION(scope, { });
            int64_t from = indices->start;
            for (int64_t i = 0; i < indices->length; ++i, from += indices->step) {
                JSValue item = listGet(globalObject, list, from);
                RETURN_IF_EXCEPTION(scope, { });
                listInitializeAt(globalObject, result, i, item);
            }
            return result;
        }
        if (!isIndexLike(globalObject, key))
            return raiseTypeError(globalObject, scope, makeString("list indices must be integers or slices, not "_s, typeName(globalObject, key)));
        auto index = normalizeIndex(globalObject, scope, key, list->length(), "list index"_s);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, listGet(globalObject, list, *index));
    }

    switch (cell->type()) {
    case PyTupleType: {
        auto* tuple = uncheckedDowncast<PyTuple>(cell);
        if (auto* slice = trySlice(key)) {
            auto indices = slice->indices(globalObject, tuple->length());
            RETURN_IF_EXCEPTION(scope, { });
            PyTuple* result = PyTuple::create(globalObject, indices->length);
            int64_t from = indices->start;
            for (int64_t i = 0; i < indices->length; ++i, from += indices->step)
                result->initializeAt(vm, i, tuple->at(from));
            return result;
        }
        if (!isIndexLike(globalObject, key))
            return raiseTypeError(globalObject, scope, makeString("tuple indices must be integers or slices, not "_s, typeName(globalObject, key)));
        auto index = normalizeIndex(globalObject, scope, key, tuple->length(), "tuple index"_s);
        RETURN_IF_EXCEPTION(scope, { });
        return tuple->at(*index);
    }
    case StringType:
        RELEASE_AND_RETURN(scope, stringGetItem(globalObject, uncheckedDowncast<JSString>(cell), key));
    case PyDictType: {
        auto* dict = uncheckedDowncast<PyDict>(cell);
        JSValue value = dict->get(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
        // A class derived from dict can say what a missing key has.
        JSValue self;
        JSValue missing = lookupSpecial(globalObject, base, vm.pythonNames().dunder_missing, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (missing)
            RELEASE_AND_RETURN(scope, callMethod(globalObject, missing, self, key));
        return raise(globalObject, scope, BuiltinType::KeyError, key);
    }
    case PyRangeType: {
        RELEASE_AND_RETURN(scope, rangeGetItem(globalObject, uncheckedDowncast<PyRange>(cell), key));
    }
    default:
        // An instance of a class derived from str, which has the string in it.
        if (JSString* string = stringIn(base))
            RELEASE_AND_RETURN(scope, stringGetItem(globalObject, string, key));
        return { };
    }
}

JSValue getItem(JSGlobalObject* globalObject, JSValue base, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();

    if (isExact(globalObject, base)) {
        JSValue result = builtinGetItem(globalObject, base, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
    }

    JSValue self;
    JSValue method = lookupSpecial(globalObject, base, names.dunder_getitem, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (method)
        RELEASE_AND_RETURN(scope, callMethod(globalObject, method, self, key));

    if (isClass(base)) {
        // type[int], which is not to make every class that has type for its class generic.
        if (base == globalObject->pyRealm()->typeType())
            return newGenericAlias(globalObject, base, key);
        // list[int]
        JSValue classGetItem = getAttributeIfPresent(globalObject, base, names.dunder_class_getitem);
        RETURN_IF_EXCEPTION(scope, { });
        if (classGetItem)
            RELEASE_AND_RETURN(scope, call(globalObject, classGetItem, key));
        return raiseTypeError(globalObject, scope, makeString("type '"_s, asType(base)->nameString(globalObject), "' is not subscriptable"_s));
    }
    return raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, base), "' object is not subscriptable"_s));
}

// True if `base` is a list or a dict, and then it has been done or something has been raised. `value` is empty to delete.
bool builtinSetItem(JSGlobalObject* globalObject, JSValue base, JSValue key, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!base.isCell())
        return false;
    JSCell* cell = base.asCell();

    if (cell->type() == PyDictType) {
        auto* dict = uncheckedDowncast<PyDict>(cell);
        if (value) {
            scope.release();
            dict->set(globalObject, key, value);
            return true;
        }
        JSValue removed = dict->remove(globalObject, key);
        RETURN_IF_EXCEPTION(scope, true);
        if (!removed)
            raise(globalObject, scope, BuiltinType::KeyError, key);
        return true;
    }
    if (!isListCell(cell))
        return false;

    auto* list = uncheckedDowncast<JSArray>(cell);
    if (auto* slice = trySlice(key)) {
        MarkedArgumentBuffer values;
        if (value) {
            collect(globalObject, value, values);
            if (scope.exception()) {
                if (catchException(globalObject, BuiltinType::TypeError))
                    raiseTypeError(globalObject, scope, "must assign iterable to extended slice"_s);
                return true;
            }
        }
        auto indices = slice->indices(globalObject, [&] { return list->length(); });
        RETURN_IF_EXCEPTION(scope, true);
        if (indices->step == 1) {
            scope.release();
            listReplaceRange(globalObject, list, indices->start, std::max<int64_t>(indices->stop - indices->start, 0), values);
            return true;
        }
        if (value) {
            if (static_cast<int64_t>(values.size()) != indices->length) {
                raiseValueError(globalObject, scope, makeString("attempt to assign sequence of size "_s, values.size(), " to extended slice of size "_s, indices->length));
                return true;
            }
            int64_t at = indices->start;
            for (int64_t i = 0; i < indices->length; ++i, at += indices->step) {
                listSet(globalObject, list, at, values.at(i));
                RETURN_IF_EXCEPTION(scope, true);
            }
            return true;
        }
        // From the top down, so that what has yet to go stays where it is.
        int64_t first = indices->step > 0 ? indices->start + (indices->length - 1) * indices->step : indices->start;
        int64_t step = std::abs(indices->step);
        for (int64_t i = 0; i < indices->length; ++i, first -= step) {
            listRemoveRange(globalObject, list, first, 1);
            RETURN_IF_EXCEPTION(scope, true);
        }
        return true;
    }
    if (!isIndexLike(globalObject, key)) {
        raiseTypeError(globalObject, scope, makeString("list indices must be integers or slices, not "_s, typeName(globalObject, key)));
        return true;
    }
    auto index = normalizeIndex(globalObject, scope, key, list->length(), value ? "list assignment index"_s : "list assignment index"_s);
    RETURN_IF_EXCEPTION(scope, true);
    scope.release();
    if (value)
        listSet(globalObject, list, *index, value);
    else
        listRemoveRange(globalObject, list, *index, 1);
    return true;
}

static void setOrDeleteItem(JSGlobalObject* globalObject, JSValue base, JSValue key, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();

    if (isExact(globalObject, base)) {
        bool done = builtinSetItem(globalObject, base, key, value);
        RETURN_IF_EXCEPTION(scope, void());
        if (done)
            return;
    }
    JSValue self;
    JSValue method = lookupSpecial(globalObject, base, value ? names.dunder_setitem : names.dunder_delitem, self);
    RETURN_IF_EXCEPTION(scope, void());
    if (method) {
        scope.release();
        if (value)
            callMethod(globalObject, method, self, key, value);
        else
            callMethod(globalObject, method, self, key);
        return;
    }
    // PyObject_SetItem() and PyObject_DelItem(): what could be a sequence is tried as one if it is a place in a sequence that is asked for, and what that comes to is put a little differently.
    PyType* type = typeOf(globalObject, base);
    bool couldBeSequence = type->hasFlag(PyType::IsHeapType) || type->lookup(vm, names.dunder_len) || type->lookup(vm, names.dunder_contains) || type->isSubtypeOf(globalObject->pyRealm()->typeTemplate());
    bool isTriedAsSequence = couldBeSequence && (classify(key).isInt() || typeOf(globalObject, key)->lookup(vm, names.dunder_index));
    if (isTriedAsSequence) {
        toIndex(globalObject, key);
        RETURN_IF_EXCEPTION(scope, void());
    }
    raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, base), "' object "_s, value ? "does not support item assignment"_s : isTriedAsSequence ? "doesn't support item deletion"_s : "does not support item deletion"_s));
}

void setItem(JSGlobalObject* globalObject, JSValue base, JSValue key, JSValue value)
{
    setOrDeleteItem(globalObject, base, key, value);
}

void deleteItem(JSGlobalObject* globalObject, JSValue base, JSValue key)
{
    setOrDeleteItem(globalObject, base, key, JSValue());
}

// ---- Iteration

// What the built-in types give. Empty if it is none of them.
JSValue builtinGetIterator(JSGlobalObject* globalObject, JSValue value)
{
    if (!value.isCell())
        return { };
    JSCell* cell = value.asCell();
    using Kind = PyIterator::Kind;
    switch (cell->type()) {
    case StringType: {
        auto view = asString(value)->view(globalObject);
        return PyIterator::create(globalObject, view->containsOnlyASCII() ? Kind::AsciiStr : Kind::Str, value);
    }
    case PyTupleType:
        return PyIterator::create(globalObject, Kind::Tuple, value);
    case PyDictType:
        return PyIterator::create(globalObject, Kind::DictKeys, uncheckedDowncast<PyDict>(cell));
    case PySetType:
        return PyIterator::create(globalObject, Kind::Set, value, JSValue(), 0, uncheckedDowncast<PySet>(cell)->size());
    case PyRangeType: {
        return rangeIterator(globalObject, uncheckedDowncast<PyRange>(cell));
    }
    case PyIteratorType:
        return value;
    case JSGeneratorType:
        // Not a coroutine, nor an asynchronous generator.
        return generatorKindOf(globalObject, uncheckedDowncast<JSGenerator>(cell)) == GeneratorKind::Generator ? value : JSValue();
    default:
        if (isListCell(cell))
            return PyIterator::create(globalObject, Kind::List, value);
        // An instance of a class derived from str, which has the string in it.
        // CPython has a quicker iterator for a str that is all ASCII, but not for an instance of a class derived from str.
        if (JSString* string = stringIn(value))
            return PyIterator::create(globalObject, Kind::Str, string);
        return { };
    }
}

JSValue getIterator(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();

    if (isExact(globalObject, value)) {
        if (JSValue iterator = builtinGetIterator(globalObject, value))
            return iterator;
    }

    PyType* type = typeOf(globalObject, value);
    JSValue self;
    JSValue method = lookupSpecial(globalObject, value, names.dunder_iter, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (method && !isNone(method)) {
        JSValue iterator = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (!tryIterator(iterator) && !typeOf(globalObject, iterator)->lookup(vm, names.dunder_next))
            return raiseTypeError(globalObject, scope, makeString("iter() returned non-iterator of type '"_s, typeName(globalObject, iterator), '\''));
        return iterator;
    }
    if (!method && type->lookup(vm, names.dunder_getitem))
        return PyIterator::create(globalObject, PyIterator::Kind::Sequence, value);

    return raiseTypeError(globalObject, scope, makeString('\'', type->nameString(globalObject), "' object is not iterable"_s));
}

JSValue iteratorNext(JSGlobalObject* globalObject, JSValue iterator, JSValue* returnedByGenerator)
{
    if (auto* native = tryIterator(iterator); native && !native->isOfDerivedClass())
        return native->next(globalObject);

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (iterator.isCell() && iterator.asCell()->type() == JSGeneratorType && generatorKindOf(globalObject, uncheckedDowncast<JSGenerator>(iterator.asCell())) == GeneratorKind::Generator) {
        JSValue returned;
        JSValue yielded = resumeGenerator(globalObject, uncheckedDowncast<JSGenerator>(iterator.asCell()), jsUndefined(), JSGenerator::ResumeMode::NormalMode, returned);
        RETURN_IF_EXCEPTION(scope, { });
        if (!yielded && returnedByGenerator)
            *returnedByGenerator = returned ? returned : jsUndefined();
        return yielded;
    }

    JSValue self;
    JSValue method = lookupSpecial(globalObject, iterator, vm.pythonNames().dunder_next, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, iterator), "' object is not an iterator"_s));
    JSValue value = callMethod(globalObject, method, self);
    if (scope.exception()) [[unlikely]] {
        JSValue raised = scope.exception()->value();
        if (catchException(globalObject, BuiltinType::StopIteration) && vm.isPythonWatched())
            noteCaughtStopIteration(globalObject, raised);
        return { };
    }
    return value;
}

bool forEach(JSGlobalObject* globalObject, JSValue iterable, const ScopedLambda<bool(JSValue)>& function)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (isExact(globalObject, iterable) && iterable.isCell()) {
        if (JSArray* list = tryList(iterable)) {
            for (unsigned i = 0; i < list->length(); ++i) {
                JSValue item = listGet(globalObject, list, i);
                RETURN_IF_EXCEPTION(scope, false);
                bool more = function(item);
                RETURN_IF_EXCEPTION(scope, false);
                if (!more)
                    break;
            }
            return true;
        }
        if (isTuple(iterable)) {
            auto* tuple = uncheckedDowncast<PyTuple>(iterable.asCell());
            for (unsigned i = 0; i < tuple->length(); ++i) {
                bool more = function(tuple->at(i));
                RETURN_IF_EXCEPTION(scope, false);
                if (!more)
                    break;
            }
            return true;
        }
    }

    JSValue iterator = getIterator(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, false);
    while (true) {
        JSValue value = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!value)
            return true;
        bool more = function(value);
        RETURN_IF_EXCEPTION(scope, false);
        if (!more)
            return true;
    }
}

bool collect(JSGlobalObject* globalObject, JSValue iterable, MarkedArgumentBuffer& values)
{
    return forEach(globalObject, iterable, [&] (JSValue value) {
        values.append(value);
        return true;
    });
}

void unpackSequence(JSGlobalObject* globalObject, JSValue iterable, unsigned count, int starIndex, Register* first)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto target = [&] (unsigned i) -> Register& { return first[-static_cast<int>(i)]; };

    if (starIndex < 0 && isExact(globalObject, iterable)) {
        if (isTuple(iterable)) {
            auto* tuple = uncheckedDowncast<PyTuple>(iterable.asCell());
            if (tuple->length() == count) {
                for (unsigned i = 0; i < count; ++i)
                    target(i) = tuple->at(i);
                return;
            }
        } else if (JSArray* list = tryList(iterable)) {
            if (list->length() == count) {
                for (unsigned i = 0; i < count; ++i) {
                    target(i) = listGet(globalObject, list, i);
                    RETURN_IF_EXCEPTION(scope, void());
                }
                return;
            }
        }
    }

    JSValue iterator = getIterator(globalObject, iterable);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::TypeError))
            raiseTypeError(globalObject, scope, makeString("cannot unpack non-iterable "_s, typeName(globalObject, iterable), " object"_s));
        return;
    }

    unsigned before = starIndex < 0 ? count : starIndex;
    MarkedArgumentBuffer values;
    for (unsigned i = 0; i < before; ++i) {
        JSValue value = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, void());
        if (!value) {
            if (starIndex < 0)
                raiseValueError(globalObject, scope, makeString("not enough values to unpack (expected "_s, count, ", got "_s, i, ')'));
            else
                raiseValueError(globalObject, scope, makeString("not enough values to unpack (expected at least "_s, count - 1, ", got "_s, i, ')'));
            return;
        }
        values.append(value);
    }

    if (starIndex < 0) {
        JSValue extra = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, void());
        if (extra) {
            // If it is easy to say how many there were, it is said.
            if (isExact(globalObject, iterable) && (isList(iterable) || isTuple(iterable) || isDict(iterable))) {
                int64_t size = length(globalObject, iterable);
                RETURN_IF_EXCEPTION(scope, void());
                raiseValueError(globalObject, scope, makeString("too many values to unpack (expected "_s, count, ", got "_s, size, ')'));
                return;
            }
            raiseValueError(globalObject, scope, makeString("too many values to unpack (expected "_s, count, ')'));
            return;
        }
        for (unsigned i = 0; i < count; ++i)
            target(i) = values.at(i);
        return;
    }

    MarkedArgumentBuffer rest;
    while (true) {
        JSValue value = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, void());
        if (!value)
            break;
        rest.append(value);
    }
    unsigned after = count - before - 1;
    if (rest.size() < after) {
        raiseValueError(globalObject, scope, makeString("not enough values to unpack (expected at least "_s, count - 1, ", got "_s, before + rest.size(), ')'));
        return;
    }
    unsigned starred = rest.size() - after;
    JSArray* list = newList(globalObject, starred);
    RETURN_IF_EXCEPTION(scope, void());
    for (unsigned i = 0; i < starred; ++i)
        listInitializeAt(globalObject, list, i, rest.at(i));
    for (unsigned i = 0; i < before; ++i)
        target(i) = values.at(i);
    target(before) = list;
    for (unsigned i = 0; i < after; ++i)
        target(before + 1 + i) = rest.at(starred + i);
}

JSValue newTuple(JSGlobalObject* globalObject, Register* first, unsigned count)
{
    VM& vm = globalObject->vm();
    PyTuple* tuple = PyTuple::create(globalObject, count);
    for (unsigned i = 0; i < count; ++i)
        tuple->initializeAt(vm, i, first[-static_cast<int>(i)].jsValue());
    return tuple;
}

} } // namespace JSC::Python
