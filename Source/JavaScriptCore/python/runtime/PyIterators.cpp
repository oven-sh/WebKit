/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyIterators.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModuleHelpers.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyTypes.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

PY_DEFINE_CELL(PyIterator);
PY_DEFINE_CELL(PyMultiIterator);

// ---- The iterators of the built-in containers ----

PyIterator::PyIterator(VM& vm, Structure* structure, PyType* type, Kind kind)
    : Base(vm, structure, type)
    , m_kind(kind)
{
}

PyType* strAsciiIteratorType(JSGlobalObject*);

static PyType* typeForIterator(Runtime& rt, PyIterator::Kind kind)
{
    switch (kind) {
    case PyIterator::Kind::List:
        return rt.listIteratorType();
    case PyIterator::Kind::ListReverse:
        return rt.listReverseIteratorType();
    case PyIterator::Kind::Tuple:
        return rt.tupleIteratorType();
    case PyIterator::Kind::Str:
        return rt.strIteratorType();
    case PyIterator::Kind::Bytes:
        return rt.bytesIteratorType();
    case PyIterator::Kind::ByteArray:
        return rt.bytearrayIteratorType();
    case PyIterator::Kind::Range:
        return rt.rangeIteratorType();
    case PyIterator::Kind::LongRange:
        return rt.longRangeIteratorType();
    case PyIterator::Kind::DictKeys:
        return rt.dictKeyIteratorType();
    case PyIterator::Kind::DictValues:
        return rt.dictValueIteratorType();
    case PyIterator::Kind::DictItems:
        return rt.dictItemIteratorType();
    case PyIterator::Kind::DictReverseKeys:
        return rt.dictReverseKeyIteratorType();
    case PyIterator::Kind::DictReverseValues:
        return rt.dictReverseValueIteratorType();
    case PyIterator::Kind::DictReverseItems:
        return rt.dictReverseItemIteratorType();
    case PyIterator::Kind::Set:
        return rt.setIteratorType();
    case PyIterator::Kind::Sequence:
        return rt.sequenceIteratorType();
    case PyIterator::Kind::Callable:
        return rt.callableIteratorType();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PyIterator* PyIterator::create(JSGlobalObject* globalObject, Kind kind, JSValue target)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyType* type = typeForIterator(rt, kind);
    if (kind == Kind::Str) {
        if (auto* impl = asString(target)->tryGetValueImpl(); impl && impl->containsOnlyASCII())
            type = strAsciiIteratorType(globalObject);
    }
    auto* iterator = new (NotNull, allocateCell<PyIterator>(vm)) PyIterator(vm, rt.structure<PyIterator>(), type, kind);
    iterator->finishCreation(vm);
    iterator->m_target.set(vm, iterator, target);
    if (kind >= Kind::DictKeys && kind <= Kind::DictReverseItems)
        iterator->step = PyDict::from(target)->size();
    return iterator;
}

template<typename Visitor>
void PyIterator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyIterator>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_target);
    visitor.append(thisObject->m_extra);
}

DEFINE_VISIT_CHILDREN(PyIterator);

JSValue PyIterator::next(JSGlobalObject* globalObject)
{
    JSValue target = m_target.get();
    if (!target)
        return { };
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto exhaust = [&]() -> JSValue {
        m_target.clear();
        m_extra.clear();
        return { };
    };

    switch (m_kind) {
    case Kind::List: {
        JSArray* list = asList(target);
        if (position < list->length())
            return listGet(list, position++);
        return exhaust();
    }
    case Kind::ListReverse: {
        // Having gone through the list it still has it, and __setstate__() can send it back. It lets go of a list that has shrunk under it.
        if (position < 0)
            return { };
        JSArray* list = asList(target);
        if (position < list->length())
            return listGet(list, position--);
        position = -1;
        return exhaust();
    }
    case Kind::Tuple: {
        auto* tuple = PyTuple::from(target);
        if (position < tuple->length())
            return tuple->at(position++);
        return exhaust();
    }
    case Kind::Str: {
        // The position is in code units.
        JSString* string = asString(target);
        if (position >= string->length())
            return exhaust();
        auto view = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        char16_t unit = view[position++];
        if (U16_IS_LEAD(unit) && position < view->length() && U16_IS_TRAIL(view[position]))
            return makeStr(vm, stringFromCodePoint(U16_GET_SUPPLEMENTARY(unit, view[position++])));
        return jsSingleCharacterString(vm, unit);
    }
    case Kind::Bytes: {
        auto* bytes = PyBytes::from(target);
        if (static_cast<size_t>(position) < bytes->length())
            return jsNumber(bytes->data()[position++]);
        return exhaust();
    }
    case Kind::ByteArray: {
        auto* bytes = PyByteArray::from(target);
        if (static_cast<size_t>(position) < bytes->length())
            return jsNumber(bytes->span()[position++]);
        return exhaust();
    }
    case Kind::Range: {
        if (remaining <= 0)
            return { };
        int64_t value = position;
        position += step;
        --remaining;
        RELEASE_AND_RETURN(scope, makeInt(globalObject, value));
    }
    case Kind::LongRange: {
        auto* state = PyTuple::from(m_extra.get());
        JSValue stride = state->at(0);
        JSValue count = state->at(1);
        if (intSign(count) <= 0)
            return { };
        JSValue following = intAdd(globalObject, target, stride);
        RETURN_IF_EXCEPTION(scope, { });
        count = intSubtract(globalObject, count, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, { });
        m_target.set(vm, this, following);
        m_extra.set(vm, this, PyTuple::create(globalObject, { stride, count }));
        return target;
    }
    case Kind::DictKeys:
    case Kind::DictValues:
    case Kind::DictItems:
    case Kind::DictReverseKeys:
    case Kind::DictReverseValues:
    case Kind::DictReverseItems: {
        auto* dict = PyDict::from(target);
        if (remaining != dict->size()) {
            remaining = -1; // From now on, it fails every time.
            return raiseRuntimeError(globalObject, scope, "dictionary changed size during iteration"_s);
        }
        JSValue key, value;
        unsigned index = static_cast<unsigned>(position);
        bool isReverse = m_kind >= Kind::DictReverseKeys;
        // The entries it was to go back over are gone.
        if (isReverse && index > dict->endPosition())
            return exhaust();
        if (!(isReverse ? dict->previous(index, key, value) : dict->next(index, key, value)))
            return exhaust();
        // As in CPython, keys that come and go are noticed only once there is one more than there was to begin with.
        if (!step) {
            exhaust();
            return raiseRuntimeError(globalObject, scope, "dictionary keys changed during iteration"_s);
        }
        --step;
        position = index;
        switch (m_kind) {
        case Kind::DictKeys:
        case Kind::DictReverseKeys:
            return key;
        case Kind::DictValues:
        case Kind::DictReverseValues:
            return value;
        default:
            RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { key, value }));
        }
    }
    case Kind::Set: {
        auto* set = PySet::from(target);
        if (static_cast<size_t>(remaining) != set->size()) {
            remaining = -1;
            return raiseRuntimeError(globalObject, scope, "Set changed size during iteration"_s);
        }
        size_t index = static_cast<size_t>(position);
        JSValue key;
        if (!set->next(index, key))
            return exhaust();
        position = index;
        return key;
    }
    case Kind::Sequence: {
        if (position == std::numeric_limits<int64_t>::max()) [[unlikely]]
            return raiseOverflowError(globalObject, scope, "iter index too large"_s);
        JSValue item = getItem(globalObject, target, makeInt(globalObject, position));
        if (!failed(scope, item)) {
            ++position;
            return item;
        }
        if (clearPendingExceptionIfMatches(globalObject, rt.IndexError()) || clearPendingExceptionIfMatches(globalObject, rt.StopIteration()))
            return exhaust();
        return { };
    }
    case Kind::Callable: {
        JSValue sentinel = m_extra.get();
        JSValue result = call(globalObject, target);
        if (failed(scope, result)) {
            if (clearPendingExceptionIfMatches(globalObject, rt.StopIteration()))
                return exhaust();
            return { };
        }
        // The call went through the iterator itself, to its end.
        if (!m_target)
            return { };
        bool isSentinel = isEqual(globalObject, result, sentinel);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSentinel)
            return exhaust();
        return result;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue iteratorSelf(JSGlobalObject*, JSValue self) { return self; }

// __next__, from next().
template<typename Iterator>
static JSValue nextOrStopIteration(JSGlobalObject* globalObject, Iterator* iterator)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = iterator->next(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;
    return raiseStopIteration(globalObject, scope);
}

static JSValue iteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    return nextOrStopIteration(globalObject, PyIterator::from(self));
}

static JSValue iteratorLengthHint(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* iterator = PyIterator::from(self);
    JSValue target = iterator->target();
    if (!target)
        return jsNumber(0);
    int64_t result = 0;
    switch (iterator->kind()) {
    case PyIterator::Kind::List:
        result = static_cast<int64_t>(asList(target)->length()) - iterator->position;
        break;
    case PyIterator::Kind::ListReverse:
        result = iterator->position + 1;
        if (result > asList(target)->length())
            result = 0;
        break;
    case PyIterator::Kind::Tuple:
        result = static_cast<int64_t>(PyTuple::from(target)->length()) - iterator->position;
        break;
    case PyIterator::Kind::Str: {
        String string = asString(target)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        result = StrView(string.substring(iterator->position)).length();
        break;
    }
    case PyIterator::Kind::Bytes:
        result = static_cast<int64_t>(PyBytes::from(target)->length()) - iterator->position;
        break;
    case PyIterator::Kind::ByteArray:
        result = static_cast<int64_t>(PyByteArray::from(target)->length()) - iterator->position;
        break;
    case PyIterator::Kind::Range:
        result = iterator->remaining;
        break;
    case PyIterator::Kind::LongRange:
        return PyTuple::from(iterator->extra())->at(1);
    case PyIterator::Kind::DictKeys:
    case PyIterator::Kind::DictValues:
    case PyIterator::Kind::DictItems:
    case PyIterator::Kind::DictReverseKeys:
    case PyIterator::Kind::DictReverseValues:
    case PyIterator::Kind::DictReverseItems: {
        if (iterator->remaining == PyDict::from(target)->size())
            result = iterator->step;
        break;
    }
    case PyIterator::Kind::Set: {
        auto* set = PySet::from(target);
        if (static_cast<size_t>(iterator->remaining) != set->size())
            break;
        size_t index = static_cast<size_t>(iterator->position);
        JSValue key;
        while (set->next(index, key))
            ++result;
        break;
    }
    case PyIterator::Kind::Sequence: {
        if (!typeOf(globalObject, target)->lookup(Special::Len))
            RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
        int64_t size = length(globalObject, target);
        RETURN_IF_EXCEPTION(scope, { });
        result = size - iterator->position;
        break;
    }
    case PyIterator::Kind::Callable:
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    }
    RELEASE_AND_RETURN(scope, makeInt(globalObject, std::max<int64_t>(result, 0)));
}

// ---- Pickling: __reduce__ and __setstate__ ----

static JSValue builtinFunctionNamed(JSGlobalObject* globalObject, ASCIILiteral name)
{
    return currentBuiltin(globalObject, internString(getVM(globalObject), name));
}

// (function, (argument,)) or (function, (argument,), state)
static JSValue reduceTo(JSGlobalObject* globalObject, JSValue function, JSValue argument, JSValue state = { })
{
    if (!function)
        return { };
    JSValue arguments = PyTuple::create(globalObject, { argument });
    if (!state)
        return PyTuple::create(globalObject, { function, arguments });
    return PyTuple::create(globalObject, { function, arguments, state });
}

static JSValue iteratorReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* iterator = PyIterator::from(self);
    // Looking that up may run code, which may use the iterator: its state is read afterwards.
    JSValue iter = builtinFunctionNamed(globalObject, iterator->kind() == PyIterator::Kind::ListReverse ? "reversed"_s : "iter"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue target = iterator->target();

    switch (iterator->kind()) {
    case PyIterator::Kind::List:
        if (target)
            RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, target, jsNumber(static_cast<double>(iterator->position))));
        RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, newList(globalObject)));
    case PyIterator::Kind::ListReverse:
        if (target && iterator->position >= 0)
            RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, target, jsNumber(static_cast<double>(iterator->position))));
        RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, newList(globalObject)));
    case PyIterator::Kind::Str: {
        if (!target)
            RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, rt.names.empty()));
        StrView view(globalObject, asString(target));
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, target, jsNumber(view.indexOfUnitOffset(iterator->position))));
    }
    case PyIterator::Kind::Tuple:
    case PyIterator::Kind::Bytes:
    case PyIterator::Kind::ByteArray:
    case PyIterator::Kind::Sequence:
        if (target)
            RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, target, makeInt(globalObject, iterator->position)));
        RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, rt.emptyTuple()));
    case PyIterator::Kind::Range: {
        // What is left of the range.
        JSValue start = makeInt(globalObject, iterator->position);
        JSValue stop = makeInt(globalObject, iterator->position + iterator->remaining * iterator->step);
        JSValue range = PyRange::create(globalObject, start, stop, makeInt(globalObject, iterator->step), makeInt(globalObject, iterator->remaining));
        RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, range, jsUndefined()));
    }
    case PyIterator::Kind::LongRange: {
        auto* state = PyTuple::from(iterator->extra());
        JSValue span = intMultiply(globalObject, state->at(1), state->at(0));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue stop = intAdd(globalObject, target, span);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, PyRange::create(globalObject, target, stop, state->at(0), state->at(1)), jsUndefined()));
    }
    case PyIterator::Kind::Callable:
        if (target)
            RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { iter, PyTuple::create(globalObject, { target, iterator->extra() }) }));
        RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, rt.emptyTuple()));
    case PyIterator::Kind::DictKeys:
    case PyIterator::Kind::DictValues:
    case PyIterator::Kind::DictItems:
    case PyIterator::Kind::DictReverseKeys:
    case PyIterator::Kind::DictReverseValues:
    case PyIterator::Kind::DictReverseItems:
    case PyIterator::Kind::Set: {
        // A list of what is to come, found with a copy: this one stays where it is.
        JSArray* rest = newList(globalObject);
        if (target) {
            PyIterator* copy = PyIterator::create(globalObject, iterator->kind(), target);
            copy->position = iterator->position;
            copy->step = iterator->step;
            copy->remaining = iterator->remaining;
            while (true) {
                JSValue item = copy->next(globalObject);
                RETURN_IF_EXCEPTION(scope, { });
                if (!item)
                    break;
                listAppend(globalObject, rest, item);
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        RELEASE_AND_RETURN(scope, reduceTo(globalObject, iter, rest));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// As PyLong_AsSsize_t(): __index__ is not asked.
static bool stateAsIndex(JSGlobalObject* globalObject, JSValue state, int64_t& index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isInt(state)) {
        raiseTypeError(globalObject, scope, "an integer is required"_s);
        return false;
    }
    RELEASE_AND_RETURN(scope, ssizeArgument(globalObject, state, index));
}

static JSValue iteratorSetState(JSGlobalObject* globalObject, JSValue self, JSValue state)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* iterator = PyIterator::from(self);
    JSValue target = iterator->target();

    if (iterator->kind() == PyIterator::Kind::LongRange) {
        if (!isExactInt(state))
            return raiseTypeError(globalObject, scope, "state must be an int, not "_s, typeNameForMessage(globalObject, rt.typeOf(state)));
        auto* extra = PyTuple::from(iterator->extra());
        JSValue step = extra->at(0);
        JSValue count = extra->at(1);
        if (intSign(state) < 0)
            state = jsNumber(0);
        else if (compareInts(count, state) < 0)
            state = count;
        JSValue skipped = intMultiply(globalObject, state, step);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue start = intAdd(globalObject, target, skipped);
        RETURN_IF_EXCEPTION(scope, { });
        count = intSubtract(globalObject, count, state);
        RETURN_IF_EXCEPTION(scope, { });
        iterator->setTarget(vm, start);
        iterator->setExtra(vm, PyTuple::create(globalObject, { step, count }));
        return jsUndefined();
    }

    int64_t index;
    if (iterator->kind() == PyIterator::Kind::Range) {
        index = toInt64(globalObject, state);
        RETURN_IF_EXCEPTION(scope, { });
        index = std::clamp<int64_t>(index, 0, iterator->remaining);
        iterator->position += index * iterator->step;
        iterator->remaining -= index;
        return jsUndefined();
    }
    if (failed(scope, stateAsIndex(globalObject, state, index)))
        return { };
    if (!target)
        return jsUndefined();

    switch (iterator->kind()) {
    case PyIterator::Kind::List:
        // Below zero, it is exhausted.
        if (index < 0)
            iterator->setTarget(vm, { });
        else
            iterator->position = std::min<int64_t>(index, asList(target)->length());
        break;
    case PyIterator::Kind::ListReverse:
        iterator->position = std::clamp<int64_t>(index, -1, static_cast<int64_t>(asList(target)->length()) - 1);
        break;
    case PyIterator::Kind::Tuple:
        iterator->position = std::clamp<int64_t>(index, 0, PyTuple::from(target)->length());
        break;
    case PyIterator::Kind::Str: {
        StrView view(globalObject, asString(target));
        RETURN_IF_EXCEPTION(scope, { });
        iterator->position = view.unitOffset(std::clamp<int64_t>(index, 0, view.length()));
        break;
    }
    case PyIterator::Kind::Bytes:
        iterator->position = std::clamp<int64_t>(index, 0, PyBytes::from(target)->length());
        break;
    case PyIterator::Kind::ByteArray:
        // Below zero, it is exhausted.
        if (index < 0)
            iterator->setTarget(vm, { });
        else
            iterator->position = std::min<int64_t>(index, PyByteArray::from(target)->length());
        break;
    case PyIterator::Kind::Sequence:
        iterator->position = std::max<int64_t>(index, 0);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return jsUndefined();
}

#define PY_ITERATOR_METHODS \
    PY_SLOT("__iter__", iteratorSelf, "($self, /)"), \
    PY_SLOT("__next__", iteratorNext, "($self, /)"), \
    PY_METHOD("__reduce__", iteratorReduce, "($self, /)")

// Those that know where they are by a number.
static constexpr NativeDef iteratorMethods[] = {
    PY_ITERATOR_METHODS,
    PY_METHOD("__length_hint__", iteratorLengthHint, "($self, /)"),
    PY_METHOD("__setstate__", iteratorSetState, "($self, object, /)"),
};

static constexpr NativeDef rangeIteratorMethods[] = {
    PY_ITERATOR_METHODS,
    PY_METHOD("__length_hint__", iteratorLengthHint, "($self, /)"),
    PY_METHOD("__setstate__", iteratorSetState, "($self, state, /)"),
};

static constexpr NativeDef hashIteratorMethods[] = {
    PY_ITERATOR_METHODS,
    PY_METHOD("__length_hint__", iteratorLengthHint, "($self, /)"),
};

static constexpr NativeDef callableIteratorMethods[] = {
    PY_ITERATOR_METHODS,
};

// CPython has a second, faster iterator for strs that are all ASCII, and it shows in the name of the type.
static constexpr TypeDef strAsciiIteratorTypeDef { "str_ascii_iterator"_s, { }, iteratorMethods, { }, { } };

PyType* strAsciiIteratorType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(strAsciiIteratorTypeDef); }

// ---- enumerate, zip, map, filter, reversed ----

PyMultiIterator::PyMultiIterator(VM& vm, Structure* structure, PyType* type, Kind kind)
    : Base(vm, structure, type)
    , m_kind(kind)
{
}

PyMultiIterator* PyMultiIterator::create(VM& vm, Structure* structure, PyType* type, Kind kind)
{
    auto* iterator = new (NotNull, allocateCell<PyMultiIterator>(vm)) PyMultiIterator(vm, structure, type, kind);
    iterator->finishCreation(vm);
    return iterator;
}

template<typename Visitor>
void PyMultiIterator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyMultiIterator>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->first);
    visitor.append(thisObject->second);
}

DEFINE_VISIT_CHILDREN(PyMultiIterator);

// With strict=True, one iterator having run out at `index`, the others must have too.
static JSValue checkStrict(JSGlobalObject* globalObject, ASCIILiteral function, PyTuple* iterators, unsigned index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto plural = [](unsigned count) { return count == 1 ? " "_s : "s 1-"_s; };
    if (index)
        return raiseValueError(globalObject, scope, function, "() argument "_s, index + 1, " is shorter than argument"_s, plural(index), index);
    for (unsigned i = 1; i < iterators->length(); ++i) {
        JSValue item = iterNext(globalObject, iterators->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        if (item)
            return raiseValueError(globalObject, scope, function, "() argument "_s, i + 1, " is longer than argument"_s, plural(i), i);
    }
    return { };
}

JSValue PyMultiIterator::next(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // map(f, map(f, map(f, ...)))
    if (!vm.isSafeToRecurseSoft()) [[unlikely]]
        return raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded"_s);
    switch (m_kind) {
    case Kind::Enumerate: {
        JSValue item = iterNext(globalObject, first.get());
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            return { };
        JSValue count = second.get();
        JSValue following = intAdd(globalObject, count, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, { });
        second.set(vm, this, following);
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { count, item }));
    }
    case Kind::Zip: {
        auto* iterators = PyTuple::from(first.get());
        unsigned count = iterators->length();
        if (!count)
            return { };
        PyTuple* result = PyTuple::create(globalObject, count);
        for (unsigned i = 0; i < count; ++i) {
            JSValue item = iterNext(globalObject, iterators->at(i));
            RETURN_IF_EXCEPTION(scope, { });
            if (!item) {
                if (flag)
                    RELEASE_AND_RETURN(scope, checkStrict(globalObject, "zip"_s, iterators, i));
                return { };
            }
            result->initializeAt(vm, i, item);
        }
        return result;
    }
    case Kind::Map: {
        auto* iterators = PyTuple::from(first.get());
        unsigned count = iterators->length();
        MarkedArgumentBuffer arguments;
        for (unsigned i = 0; i < count; ++i) {
            JSValue item = iterNext(globalObject, iterators->at(i));
            RETURN_IF_EXCEPTION(scope, { });
            if (!item) {
                if (flag)
                    RELEASE_AND_RETURN(scope, checkStrict(globalObject, "map"_s, iterators, i));
                return { };
            }
            arguments.append(item);
        }
        RELEASE_AND_RETURN(scope, call(globalObject, second.get(), CallArgs(valuesOf(arguments), count)));
    }
    case Kind::Filter: {
        JSValue function = second.get();
        bool usesTruth = isNone(function) || function == JSValue(runtime(globalObject).boolType());
        while (true) {
            JSValue item = iterNext(globalObject, first.get());
            RETURN_IF_EXCEPTION(scope, { });
            if (!item)
                return { };
            JSValue verdict = item;
            if (!usesTruth) {
                verdict = call(globalObject, function, item);
                RETURN_IF_EXCEPTION(scope, { });
            }
            bool keep = isTrue(globalObject, verdict);
            RETURN_IF_EXCEPTION(scope, { });
            if (keep)
                return item;
        }
    }
    case Kind::Reversed: {
        if (position < 0 || !first)
            return { };
        JSValue item = getItem(globalObject, first.get(), makeInt(globalObject, position));
        if (!failed(scope, item)) {
            --position;
            return item;
        }
        Runtime& rt = runtime(globalObject);
        if (clearPendingExceptionIfMatches(globalObject, rt.IndexError()) || clearPendingExceptionIfMatches(globalObject, rt.StopIteration())) {
            position = -1;
            first.clear();
        }
        return { };
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue multiIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    return nextOrStopIteration(globalObject, PyMultiIterator::from(self));
}

static PyMultiIterator* newMultiIterator(JSGlobalObject* globalObject, JSValue cls, PyMultiIterator::Kind kind)
{
    Runtime& rt = runtime(globalObject);
    return PyMultiIterator::create(rt.vm(), rt.structure<PyMultiIterator>(), PyType::from(cls), kind);
}

static JSValue enumerateNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() > 2)
        return raiseTypeError(globalObject, scope, "enumerate() takes at most 2 arguments ("_s, args.totalCount(), " given)"_s);
    JSValue iterable = args.get(0);
    JSValue start = args.get(1);
    // enumerate itself goes by how many arguments there are, and then expects the names that go with that.
    if (cls == JSValue(runtime(globalObject).enumerateType()) && args.hasKeywords() && args.totalCount() <= 2) {
        auto isNamed = [&](unsigned index, ASCIILiteral name) { return stringOf(args.keywordName(index)) == name; };
        auto invalid = [&](unsigned index) {
            return raiseTypeError(globalObject, scope, '\'', stringOf(args.keywordName(index)), "' is an invalid keyword argument for enumerate()"_s);
        };
        if (args.keywordCount() == 1) {
            if (!isNamed(0, args.size() ? "start"_s : "iterable"_s))
                return invalid(0);
        } else if (isNamed(0, "start"_s)) {
            if (!isNamed(1, "iterable"_s))
                return invalid(1);
        } else if (!isNamed(0, "iterable"_s))
            return invalid(0);
        else if (!isNamed(1, "start"_s))
            return invalid(1);
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        String keyword = stringOf(args.keywordName(i));
        JSValue& target = keyword == "iterable"_s ? iterable : start;
        if (keyword != "iterable"_s && keyword != "start"_s)
            return raiseTypeError(globalObject, scope, "enumerate() got an unexpected keyword argument '"_s, keyword, '\'');
        if (target)
            return raiseTypeError(globalObject, scope, "argument for enumerate() given by name ('"_s, keyword, "') and position ("_s, keyword == "iterable"_s ? 1 : 2, ')');
        target = args.keywordValue(i);
    }
    if (!iterable)
        return raiseTypeError(globalObject, scope, "enumerate() missing required argument 'iterable'"_s);
    JSValue count = jsNumber(0);
    if (start) {
        count = toIndex(globalObject, start);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue iterator = getIter(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, { });
    auto* result = newMultiIterator(globalObject, cls, PyMultiIterator::Kind::Enumerate);
    result->first.set(vm, result, iterator);
    result->second.set(vm, result, count);
    return result;
}

// The one keyword zip() and map() take.
static bool parseStrict(JSGlobalObject* globalObject, ASCIILiteral function, const CallArgs& args, bool& strict)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    strict = false;
    if (args.keywordCount() > 1) {
        raiseTypeError(globalObject, scope, function, "() takes at most 1 keyword argument ("_s, args.keywordCount(), " given)"_s);
        return false;
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        String keyword = stringOf(args.keywordName(i));
        if (keyword != "strict"_s) {
            scope.release();
            raiseUnexpectedKeyword(globalObject, function, keyword, { "strict"_s });
            return false;
        }
        strict = isTrue(globalObject, args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

static PyTuple* iteratorsOf(JSGlobalObject* globalObject, std::span<const JSValue> iterables)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* result = PyTuple::create(globalObject, iterables.size());
    for (size_t i = 0; i < iterables.size(); ++i) {
        JSValue iterator = getIter(globalObject, iterables[i]);
        RETURN_IF_EXCEPTION(scope, nullptr);
        result->initializeAt(vm, i, iterator);
    }
    return result;
}

static JSValue zipNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool strict;
    if (failed(scope, parseStrict(globalObject, "zip"_s, args, strict)))
        return { };
    PyTuple* iterators = iteratorsOf(globalObject, args.positional());
    RETURN_IF_EXCEPTION(scope, { });
    auto* result = newMultiIterator(globalObject, cls, PyMultiIterator::Kind::Zip);
    result->first.set(vm, result, iterators);
    result->flag = strict;
    return result;
}

static JSValue mapNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool strict;
    if (failed(scope, parseStrict(globalObject, "map"_s, args, strict)))
        return { };
    if (args.size() < 2)
        RELEASE_AND_RETURN(scope, raiseTypeError(globalObject, scope, "map() must have at least two arguments."_s));
    PyTuple* iterators = iteratorsOf(globalObject, args.positional().subspan(1));
    RETURN_IF_EXCEPTION(scope, { });
    auto* result = newMultiIterator(globalObject, cls, PyMultiIterator::Kind::Map);
    result->first.set(vm, result, iterators);
    result->second.set(vm, result, args[0]);
    result->flag = strict;
    return result;
}

static JSValue filterNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywordsInNew(globalObject, "filter"_s, PyType::from(cls), rt.filterType(), args)))
        return { };
    if (failed(scope, checkPositional(globalObject, "filter"_s, args, 2, 2)))
        return { };
    JSValue iterator = getIter(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    auto* result = newMultiIterator(globalObject, cls, PyMultiIterator::Kind::Filter);
    result->first.set(vm, result, iterator);
    result->second.set(vm, result, args[0]);
    return result;
}

static JSValue reversedNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywordsInNew(globalObject, "reversed"_s, PyType::from(cls), rt.reversedType(), args)))
        return { };
    if (failed(scope, checkPositional(globalObject, "reversed"_s, args, 1, 1)))
        return { };
    JSValue sequence = args[0];
    PyType* type = rt.typeOf(sequence);
    JSValue method = type->lookup(Special::Reversed);
    auto notReversible = [&] { return raiseTypeError(globalObject, scope, '\'', type->nameInMessages(), "' object is not reversible"_s); };
    if (method) {
        if (isNone(method))
            return notReversible();
        RELEASE_AND_RETURN(scope, callSpecial(globalObject, method, sequence));
    }
    if (!type->lookup(Special::GetItem) || type->has(TypeFlag::DictSubclass))
        return notReversible();
    int64_t size = length(globalObject, sequence);
    RETURN_IF_EXCEPTION(scope, { });
    auto* result = newMultiIterator(globalObject, cls, PyMultiIterator::Kind::Reversed);
    result->first.set(vm, result, sequence);
    result->position = size - 1;
    return result;
}

static JSValue reversedLengthHint(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* iterator = PyMultiIterator::from(self);
    if (!iterator->first)
        return jsNumber(0);
    int64_t size = length(globalObject, iterator->first.get());
    RETURN_IF_EXCEPTION(scope, { });
    int64_t position = iterator->position + 1;
    RELEASE_AND_RETURN(scope, makeInt(globalObject, size < position ? static_cast<int64_t>(0) : position));
}

static JSValue multiIteratorReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* iterator = PyMultiIterator::from(self);
    JSValue type = iterator->type();
    switch (iterator->kind()) {
    case PyMultiIterator::Kind::Enumerate:
        return PyTuple::create(globalObject, { type, PyTuple::create(globalObject, { iterator->first.get(), iterator->second.get() }) });
    case PyMultiIterator::Kind::Filter:
        return PyTuple::create(globalObject, { type, PyTuple::create(globalObject, { iterator->second.get(), iterator->first.get() }) });
    case PyMultiIterator::Kind::Zip:
    case PyMultiIterator::Kind::Map: {
        auto* arguments = PyTuple::from(iterator->first.get());
        if (iterator->kind() == PyMultiIterator::Kind::Map) {
            auto* iterators = arguments;
            arguments = PyTuple::create(globalObject, iterators->length() + 1);
            arguments->initializeAt(vm, 0, iterator->second.get());
            for (unsigned i = 0; i < iterators->length(); ++i)
                arguments->initializeAt(vm, i + 1, iterators->at(i));
        }
        if (iterator->flag)
            return PyTuple::create(globalObject, { type, arguments, jsBoolean(true) });
        return PyTuple::create(globalObject, { type, arguments });
    }
    case PyMultiIterator::Kind::Reversed:
        if (iterator->first && iterator->position != -1)
            return reduceTo(globalObject, type, iterator->first.get(), jsNumber(static_cast<double>(iterator->position)));
        return reduceTo(globalObject, type, rt.emptyTuple());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// zip, map: whether it is strict.
static JSValue multiIteratorSetStrict(JSGlobalObject* globalObject, JSValue self, JSValue state)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool strict = isTrue(globalObject, state);
    RETURN_IF_EXCEPTION(scope, { });
    PyMultiIterator::from(self)->flag = strict;
    return jsUndefined();
}

static JSValue reversedSetState(JSGlobalObject* globalObject, JSValue self, JSValue state)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* iterator = PyMultiIterator::from(self);
    int64_t index;
    if (failed(scope, stateAsIndex(globalObject, state, index)))
        return { };
    if (!iterator->first || iterator->position == -1)
        return jsUndefined();
    int64_t size = length(globalObject, iterator->first.get());
    RETURN_IF_EXCEPTION(scope, { });
    iterator->position = std::clamp<int64_t>(index, -1, size - 1);
    return jsUndefined();
}

#define PY_MULTI_ITERATOR_METHODS(newFunction) \
    PY_NEW(newFunction, "($type, *args, **kwargs)"), \
    PY_SLOT("__iter__", iteratorSelf, "($self, /)"), \
    PY_SLOT("__next__", multiIteratorNext, "($self, /)"), \
    PY_METHOD("__reduce__", multiIteratorReduce, "($self, /)")

static constexpr NativeDef enumerateMethods[] = {
    PY_MULTI_ITERATOR_METHODS(enumerateNew),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};
static constexpr NativeDef zipMethods[] = {
    PY_MULTI_ITERATOR_METHODS(zipNew),
    PY_METHOD("__setstate__", multiIteratorSetStrict, "($self, object, /)"),
};
static constexpr NativeDef mapMethods[] = {
    PY_MULTI_ITERATOR_METHODS(mapNew),
    PY_METHOD("__setstate__", multiIteratorSetStrict, "($self, object, /)"),
};
static constexpr NativeDef filterMethods[] = { PY_MULTI_ITERATOR_METHODS(filterNew) };
static constexpr NativeDef reversedMethods[] = {
    PY_MULTI_ITERATOR_METHODS(reversedNew),
    PY_METHOD("__length_hint__", reversedLengthHint, "($self, /)"),
    PY_METHOD("__setstate__", reversedSetState, "($self, object, /)"),
};

void initializeIteratorTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    auto define = [&](PyType* type, ASCIILiteral name, std::span<const NativeDef> methods, OptionSet<TypeFlag> flags = { }) {
        type->initializeBuiltin(globalObject, { name, flags, methods, { }, { } }, rt.typeType(), rt.objectType());
    };
    define(rt.listIteratorType(), "list_iterator"_s, iteratorMethods);
    define(rt.listReverseIteratorType(), "list_reverseiterator"_s, iteratorMethods);
    define(rt.tupleIteratorType(), "tuple_iterator"_s, iteratorMethods);
    define(rt.strIteratorType(), "str_iterator"_s, iteratorMethods);
    define(rt.bytesIteratorType(), "bytes_iterator"_s, iteratorMethods);
    define(rt.bytearrayIteratorType(), "bytearray_iterator"_s, iteratorMethods);
    define(rt.rangeIteratorType(), "range_iterator"_s, rangeIteratorMethods);
    define(rt.longRangeIteratorType(), "longrange_iterator"_s, rangeIteratorMethods);
    define(rt.dictKeyIteratorType(), "dict_keyiterator"_s, hashIteratorMethods);
    define(rt.dictValueIteratorType(), "dict_valueiterator"_s, hashIteratorMethods);
    define(rt.dictItemIteratorType(), "dict_itemiterator"_s, hashIteratorMethods);
    define(rt.dictReverseKeyIteratorType(), "dict_reversekeyiterator"_s, hashIteratorMethods);
    define(rt.dictReverseValueIteratorType(), "dict_reversevalueiterator"_s, hashIteratorMethods);
    define(rt.dictReverseItemIteratorType(), "dict_reverseitemiterator"_s, hashIteratorMethods);
    define(rt.setIteratorType(), "set_iterator"_s, hashIteratorMethods);
    define(rt.sequenceIteratorType(), "iterator"_s, iteratorMethods);
    define(rt.callableIteratorType(), "callable_iterator"_s, callableIteratorMethods);

    define(rt.enumerateType(), "enumerate"_s, enumerateMethods, { TypeFlag::Base, TypeFlag::OwnLayout });
    define(rt.zipType(), "zip"_s, zipMethods, { TypeFlag::Base, TypeFlag::OwnLayout });
    define(rt.mapType(), "map"_s, mapMethods, { TypeFlag::Base, TypeFlag::OwnLayout });
    define(rt.filterType(), "filter"_s, filterMethods, { TypeFlag::Base, TypeFlag::OwnLayout });
    define(rt.reversedType(), "reversed"_s, reversedMethods, { TypeFlag::Base, TypeFlag::OwnLayout });
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
