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
#include "PythonBuiltins.h"

#include "PyTuple.h"
#include "PythonCharacters.h"
#include "PythonSequences.h"

// What the iterators of the built-in classes have besides __next__(): how many more there are, and what copying and pickling go by. Each is as in CPython, where they are
// in the file of what is being gone through.

namespace JSC { namespace Python {

using IteratorKind = PyIterator::Kind;

// An iterator over a str counts in code units, and Python in characters.
static int64_t charactersBefore(JSGlobalObject* globalObject, JSString* string, int64_t units)
{
    if (string->is8Bit())
        return units;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String value = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    return Characters(vm, value).characterAt(units);
}

static int64_t unitsBefore(JSGlobalObject* globalObject, JSString* string, int64_t characters)
{
    if (string->is8Bit())
        return std::min<int64_t>(characters, string->length());
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String value = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    Characters all(vm, value);
    return all.codeUnitOf(std::min<int64_t>(characters, all.count()));
}

static int64_t sizeOf(JSGlobalObject* globalObject, PyIterator* iterator)
{
    switch (iterator->kind()) {
    case IteratorKind::List:
    case IteratorKind::ListReverse:
        return asList(iterator->a())->length();
    case IteratorKind::Tuple:
        return asTuple(iterator->a())->length();
    case IteratorKind::AsciiStr:
    case IteratorKind::Str:
        return charactersBefore(globalObject, asString(iterator->a()), asString(iterator->a())->length());
    case IteratorKind::Bytes:
    case IteratorKind::ByteArray:
        return uncheckedDowncast<JSUint8Array>(iterator->a().asCell())->length();
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// How many more there are, if that can be told.
PYTHON_NATIVE(iteratorLengthHint)
{
    NATIVE_PROLOGUE();
    auto* iterator = uncheckedDowncast<PyIterator>(args[0].asCell());
    if (iterator->kind() == IteratorKind::Range)
        return JSValue::encode(intFromInt64(globalObject, iterator->stop()));
    if (!iterator->a())
        return JSValue::encode(jsNumber(0));
    switch (iterator->kind()) {
    case IteratorKind::LongRange:
        RELEASE_AND_RETURN(scope, JSValue::encode(numberBinaryOperation(globalObject, BinaryOperator::Sub, uncheckedDowncast<PyRange>(iterator->a().asCell())->length(), iterator->b())));
    case IteratorKind::List:
    case IteratorKind::Tuple:
    case IteratorKind::Bytes:
    case IteratorKind::ByteArray:
        return JSValue::encode(intFromInt64(globalObject, iterator->index() < 0 ? 0 : std::max<int64_t>(sizeOf(globalObject, iterator) - iterator->index(), 0)));
    case IteratorKind::AsciiStr:
    case IteratorKind::Str:
        return JSValue::encode(intFromInt64(globalObject, sizeOf(globalObject, iterator) - charactersBefore(globalObject, asString(iterator->a()), iterator->index())));
    case IteratorKind::ListReverse:
        return JSValue::encode(intFromInt64(globalObject, sizeOf(globalObject, iterator) < iterator->index() + 1 ? 0 : iterator->index() + 1));
    case IteratorKind::DictKeys:
    case IteratorKind::DictValues:
    case IteratorKind::DictItems:
    case IteratorKind::DictReverseKeys:
    case IteratorKind::DictReverseValues:
    case IteratorKind::DictReverseItems:
        return JSValue::encode(intFromInt64(globalObject, static_cast<int64_t>(asDict(iterator->a())->size()) == iterator->stop() ? iterator->stop() - iterator->step() : 0));
    case IteratorKind::Set:
        return JSValue::encode(intFromInt64(globalObject, static_cast<int64_t>(uncheckedDowncast<PySet>(iterator->a().asCell())->size()) == iterator->stop() ? iterator->stop() - iterator->step() : 0));
    case IteratorKind::Sequence: {
        if (!typeOf(globalObject, iterator->a())->lookup(vm, names.dunder_len))
            RETURN_NOT_IMPLEMENTED();
        int64_t size = length(globalObject, iterator->a());
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(intFromInt64(globalObject, std::max<int64_t>(size - iterator->index(), 0)));
    }
    case IteratorKind::Reversed: {
        int64_t size = length(globalObject, iterator->a());
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(intFromInt64(globalObject, size < iterator->index() + 1 ? 0 : iterator->index() + 1));
    }
    default:
        RETURN_NOT_IMPLEMENTED();
    }
}

// (what to call, what to call it with[, what to give the __setstate__() of what that returns])
PYTHON_NATIVE(iteratorReduce)
{
    NATIVE_PROLOGUE();
    auto* iterator = uncheckedDowncast<PyIterator>(args[0].asCell());
    JSValue a = iterator->a();
    auto tuple = [&] (std::initializer_list<JSValue> values) -> JSValue { return PyTuple::create(globalObject, values); };
    auto index = [&] { return intFromInt64(globalObject, iterator->index()); };
    JSValue type = typeOf(globalObject, iterator)->object();

    switch (iterator->kind()) {
    case IteratorKind::Enumerate:
        return JSValue::encode(tuple({ type, tuple({ a, iterator->b() ? iterator->b() : index() }) }));
    case IteratorKind::Filter:
        return JSValue::encode(tuple({ type, tuple({ a, iterator->b() }) }));
    case IteratorKind::Zip:
        return JSValue::encode(iterator->index() ? tuple({ type, a, jsBoolean(true) }) : tuple({ type, a }));
    case IteratorKind::Map: {
        MarkedArgumentBuffer arguments;
        arguments.append(a);
        for (auto& each : asTuple(iterator->b())->span())
            arguments.append(each.get());
        JSValue all = PyTuple::createFromArguments(globalObject, arguments);
        return JSValue::encode(iterator->index() ? tuple({ type, all, jsBoolean(true) }) : tuple({ type, all }));
    }
    case IteratorKind::Reversed:
        return JSValue::encode(a && iterator->index() != -1 ? tuple({ type, tuple({ a }), index() }) : tuple({ type, tuple({ tuple({ }) }) }));
    case IteratorKind::JavaScript:
    case IteratorKind::Memory:
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("cannot pickle '"_s, typeName(globalObject, iterator), "' object"_s)));
    default:
        break;
    }

    // Finding the function can run anything, and this may be gone through meanwhile. These have what is left before that, as in CPython, and the rest look afterwards.
    JSValue left;
    switch (iterator->kind()) {
    case IteratorKind::Range: {
        int64_t stop = static_cast<int64_t>(static_cast<uint64_t>(iterator->index()) + static_cast<uint64_t>(iterator->stop()) * static_cast<uint64_t>(iterator->step()));
        left = PyRange::create(globalObject, index(), intFromInt64(globalObject, stop), intFromInt64(globalObject, iterator->step()));
        break;
    }
    case IteratorKind::LongRange: {
        auto* whole = uncheckedDowncast<PyRange>(a.asCell());
        auto operate = [&] (BinaryOperator op, JSValue left, JSValue right) { return numberBinaryOperation(globalObject, op, left, right); };
        JSValue start = operate(BinaryOperator::Add, whole->start(), operate(BinaryOperator::Mult, iterator->b(), whole->step()));
        JSValue stop = operate(BinaryOperator::Add, whole->start(), operate(BinaryOperator::Mult, whole->length(), whole->step()));
        RETURN_IF_EXCEPTION(scope, { });
        left = PyRange::create(globalObject, start, stop, whole->step());
        break;
    }
    case IteratorKind::DictKeys:
    case IteratorKind::DictValues:
    case IteratorKind::DictItems:
    case IteratorKind::DictReverseKeys:
    case IteratorKind::DictReverseValues:
    case IteratorKind::DictReverseItems:
    case IteratorKind::Set:
        // A list, which this is left to go on through.
        left = listFromIterable(globalObject, iterator->copy(globalObject));
        RETURN_IF_EXCEPTION(scope, { });
        break;
    default:
        break;
    }
    JSValue function = getBuiltin(globalObject, iterator->kind() == IteratorKind::ListReverse ? "reversed"_s : "iter"_s);
    RETURN_IF_EXCEPTION(scope, { });
    a = iterator->a();
    switch (iterator->kind()) {
    case IteratorKind::List:
    case IteratorKind::ListReverse:
        return JSValue::encode(a && iterator->index() >= 0 ? tuple({ function, tuple({ a }), index() }) : tuple({ function, tuple({ newList(globalObject) }) }));
    case IteratorKind::Tuple:
    case IteratorKind::Bytes:
    case IteratorKind::Sequence:
        return JSValue::encode(a ? tuple({ function, tuple({ a }), index() }) : tuple({ function, tuple({ tuple({ }) }) }));
    case IteratorKind::ByteArray:
        return JSValue::encode(a && iterator->index() >= 0 ? tuple({ function, tuple({ a }), index() }) : tuple({ function, tuple({ tuple({ }) }) }));
    case IteratorKind::AsciiStr:
    case IteratorKind::Str:
        if (!a)
            return JSValue::encode(tuple({ function, tuple({ jsEmptyString(vm) }) }));
        return JSValue::encode(tuple({ function, tuple({ a }), intFromInt64(globalObject, charactersBefore(globalObject, asString(a), iterator->index())) }));
    case IteratorKind::Callable:
        return JSValue::encode(a ? tuple({ function, tuple({ a, iterator->b() }) }) : tuple({ function, tuple({ tuple({ }) }) }));
    case IteratorKind::Range:
    case IteratorKind::LongRange:
        return JSValue::encode(tuple({ function, tuple({ left }), jsUndefined() }));
    case IteratorKind::DictKeys:
    case IteratorKind::DictValues:
    case IteratorKind::DictItems:
    case IteratorKind::DictReverseKeys:
    case IteratorKind::DictReverseValues:
    case IteratorKind::DictReverseItems:
    case IteratorKind::Set:
        return JSValue::encode(tuple({ function, tuple({ left }) }));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

PYTHON_NATIVE(iteratorSetState)
{
    NATIVE_PROLOGUE();
    auto* iterator = uncheckedDowncast<PyIterator>(args[0].asCell());
    JSValue state = args[1];
    switch (iterator->kind()) {
    case IteratorKind::Zip:
    case IteratorKind::Map: {
        bool isStrict = isTrue(globalObject, state);
        RETURN_IF_EXCEPTION(scope, { });
        iterator->setIndex(isStrict);
        RETURN_NONE();
    }
    case IteratorKind::Range: {
        auto index = toCLong(globalObject, state);
        RETURN_IF_EXCEPTION(scope, { });
        int64_t skipped = std::clamp<int64_t>(*index, 0, iterator->stop());
        iterator->setIndex(static_cast<int64_t>(static_cast<uint64_t>(iterator->index()) + static_cast<uint64_t>(skipped) * static_cast<uint64_t>(iterator->step())));
        iterator->setStop(iterator->stop() - skipped);
        RETURN_NONE();
    }
    case IteratorKind::LongRange: {
        if (!isExactly(globalObject, state, realm->typeInt()))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("state must be an int, not "_s, typeName(globalObject, state))));
        JSValue left = numberBinaryOperation(globalObject, BinaryOperator::Sub, uncheckedDowncast<PyRange>(iterator->a().asCell())->length(), iterator->b());
        RETURN_IF_EXCEPTION(scope, { });
        if (compareInts(state, jsNumber(0)) < 0)
            state = jsNumber(0);
        else if (compareInts(left, state) < 0)
            state = left;
        JSValue position = numberBinaryOperation(globalObject, BinaryOperator::Add, iterator->b(), state);
        RETURN_IF_EXCEPTION(scope, { });
        iterator->setB(vm, position);
        RETURN_NONE();
    }
    default:
        break;
    }

    auto index = toSsizeOfInt(globalObject, state);
    RETURN_IF_EXCEPTION(scope, { });
    // One that has run out stays that way.
    if (!iterator->a())
        RETURN_NONE();
    switch (iterator->kind()) {
    case IteratorKind::List:
    case IteratorKind::ByteArray:
        if (iterator->index() >= 0)
            iterator->setIndex(std::clamp<int64_t>(*index, -1, sizeOf(globalObject, iterator)));
        break;
    case IteratorKind::ListReverse:
        iterator->setIndex(std::clamp<int64_t>(*index, -1, sizeOf(globalObject, iterator) - 1));
        break;
    case IteratorKind::Tuple:
    case IteratorKind::Bytes:
        iterator->setIndex(std::clamp<int64_t>(*index, 0, sizeOf(globalObject, iterator)));
        break;
    case IteratorKind::AsciiStr:
    case IteratorKind::Str:
        iterator->setIndex(unitsBefore(globalObject, asString(iterator->a()), std::max<int64_t>(*index, 0)));
        break;
    case IteratorKind::Sequence:
        iterator->setIndex(std::max<int64_t>(*index, 0));
        break;
    case IteratorKind::Reversed: {
        if (iterator->index() == -1)
            break;
        int64_t size = length(globalObject, iterator->a());
        RETURN_IF_EXCEPTION(scope, { });
        iterator->setIndex(std::clamp<int64_t>(*index, -1, size - 1));
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    RETURN_NONE();
}

void addIteratorProtocol(JSGlobalObject* globalObject, PyType* type)
{
    addMethodsThatCPythonHas(globalObject, type, {
        { "__length_hint__"_s, iteratorLengthHint },
        { "__reduce__"_s, iteratorReduce },
        { "__setstate__"_s, iteratorSetState },
    });
}

} } // namespace JSC::Python
