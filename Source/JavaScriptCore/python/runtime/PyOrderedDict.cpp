/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyList.h"
#include "PyModuleHelpers.h"
#include "PyNumbers.h"
#include "PyRuntime.h"
#include "PyStr.h"
#include "PyTuple.h"
#include "PyType.h"

namespace JSC {
namespace Python {

// collections.OrderedDict. (Objects/odictobject.c in CPython is the reference for what it does.)
//
// A dict keeps its entries in the order they were inserted, can give up an entry at either end and can take one at either end
// (PyDict::takeFirst(), moveToEnd()), so the order of an OrderedDict is that of the dict it is, and there is no list on the side
// as in CPython. What shows: dict.__delitem__(od, key) and the like, which go behind the back of CPython's and leave it
// unusable, do here what od.__delitem__(key) does.
//
// Where CPython's looks a key up again although it has just come from the OrderedDict itself (iterators, copy(), repr()), so that
// keys whose hash changes are a KeyError, this one does the same for keys of classes that could do that.

extern const TypeDef orderedDictTypeDef;

PyType* orderedDictType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    PyType* type = rt.typeFor(orderedDictTypeDef, rt.dictType());
    if (!type->dict()->get(rt.name(Special::Hash))) [[unlikely]] {
        type->dict()->set(rt.vm(), rt.name(Special::Hash), jsUndefined());
        type->didChange(rt.vm());
    }
    return type;
}

static bool isOrderedDict(JSGlobalObject* globalObject, JSValue value)
{
    auto* dict = PyDict::tryFrom(value);
    return dict && dict->type()->isSubtypeOf(orderedDictType(globalObject));
}

static bool isExactOrderedDict(JSGlobalObject* globalObject, PyDict* dict) { return dict->type() == orderedDictType(globalObject); }

// Whether the key is sure to be found where it was put.
static ALWAYS_INLINE bool hashCannotChange(JSValue key) { return key.isString() || key.isNumber() || key.isUndefinedOrNull() || key.isBoolean(); }

static JSValue raiseMutated(JSGlobalObject* globalObject, ThrowScope& scope)
{
    return raiseRuntimeError(globalObject, scope, "OrderedDict mutated during iteration"_s);
}

// The value of a key that iteration has come to.
static JSValue valueOfOwnKey(JSGlobalObject* globalObject, PyDict* dict, JSValue key, JSValue valueInEntry)
{
    if (hashCannotChange(key)) [[likely]]
        return valueInEntry;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = dict->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return raiseKeyError(globalObject, scope, key);
    return value;
}

// ---- Iterators ----

enum ODictIteratorField : unsigned {
    ODictIteratorDict, // Empty: exhausted.
    ODictIteratorState, // A tuple of numbers would do; these are three fields for three numbers.
    ODictIteratorSize,
    ODictIteratorKind,
    ODictIteratorFieldCount,
};
// PyRecord::number is the position.

enum ODictIteratorKindBits : int { ODictReversed = 1, ODictKeys = 2, ODictValues = 4, ODictItems = ODictKeys | ODictValues };

extern const TypeDef odictIteratorTypeDef;

static JSValue makeODictIterator(JSGlobalObject* globalObject, PyDict* dict, int kind)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyRecord* iterator = PyRecord::create(globalObject, rt.typeFor(odictIteratorTypeDef), ODictIteratorFieldCount);
    // One over an empty OrderedDict is exhausted from the start, whatever is added later.
    if (dict->size())
        iterator->set(vm, ODictIteratorDict, dict);
    iterator->set(vm, ODictIteratorState, jsNumber(dict->keysVersion()));
    iterator->set(vm, ODictIteratorSize, jsNumber(dict->size()));
    iterator->set(vm, ODictIteratorKind, jsNumber(kind));
    iterator->number = kind & ODictReversed ? dict->endPosition() : 0;
    return iterator;
}

// Empty, without an exception: exhausted.
static JSValue odictIteratorStep(JSGlobalObject* globalObject, PyRecord* iterator)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue dictValue = iterator->get(ODictIteratorDict);
    if (!dictValue)
        return { };
    PyDict* dict = PyDict::from(dictValue);
    int kind = iterator->get(ODictIteratorKind).asInt32();

    if (iterator->number < 0) {
        // It came to the end last time.
        iterator->set(vm, ODictIteratorDict, { });
        return { };
    }
    if (dict->keysVersion() != iterator->get(ODictIteratorState).asNumber()) {
        iterator->set(vm, ODictIteratorDict, { });
        return raiseMutated(globalObject, scope);
    }
    if (dict->size() != iterator->get(ODictIteratorSize).asNumber()) {
        iterator->set(vm, ODictIteratorSize, jsNumber(-1)); // For good.
        return raiseRuntimeError(globalObject, scope, "OrderedDict changed size during iteration"_s);
    }

    unsigned position = static_cast<unsigned>(iterator->number);
    JSValue key, value;
    bool found = kind & ODictReversed ? dict->previous(position, key, value) : dict->next(position, key, value);
    if (!found) {
        iterator->set(vm, ODictIteratorDict, { });
        return { };
    }
    iterator->number = position;
    // CPython's lets go of the OrderedDict when it is asked for more than there is, but knows that it is at the end one step before.
    {
        unsigned ahead = position;
        JSValue ignoredKey, ignoredValue;
        if (!(kind & ODictReversed ? dict->previous(ahead, ignoredKey, ignoredValue) : dict->next(ahead, ignoredKey, ignoredValue)))
            iterator->number = -1;
    }

    if (!hashCannotChange(key)) [[unlikely]] {
        value = dict->get(globalObject, key);
        if (!value && !scope.exception())
            raiseKeyError(globalObject, scope, key);
        if (scope.exception()) {
            if (kind & ODictValues)
                iterator->set(vm, ODictIteratorDict, { });
            iterator->number = -1;
            return { };
        }
    }
    if (!(kind & ODictValues))
        return key;
    if (!(kind & ODictKeys))
        return value;
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { key, value }));
}

static JSValue odictIteratorIter(JSGlobalObject*, JSValue self) { return self; }

static JSValue odictIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue item = odictIteratorStep(globalObject, PyRecord::from(self));
    RETURN_IF_EXCEPTION(scope, { });
    return item ? item : raiseStopIteration(globalObject, scope);
}

// (iter, (what is left,)), found with a copy: this one stays where it is.
static JSValue odictIteratorReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* iterator = PyRecord::from(self);
    PyRecord* copy = PyRecord::create(globalObject, iterator->type(), ODictIteratorFieldCount);
    for (unsigned i = 0; i < ODictIteratorFieldCount; ++i)
        copy->set(vm, i, iterator->get(i));
    copy->number = iterator->number;
    JSArray* rest = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    while (true) {
        JSValue item = odictIteratorStep(globalObject, copy);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            break;
        listAppend(globalObject, rest, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue iter = currentBuiltin(globalObject, internString(vm, "iter"_s));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { iter, PyTuple::create(globalObject, { rest }) }));
}

static constexpr NativeDef odictIteratorMethods[] = {
    PY_SLOT("__iter__", odictIteratorIter, "($self, /)"),
    PY_SLOT("__next__", odictIteratorNext, "($self, /)"),
    PY_METHOD("__reduce__", odictIteratorReduce, "($self, /)"),
};
constexpr TypeDef odictIteratorTypeDef { "odict_iterator"_s, { TypeFlag::DisallowInstantiation }, odictIteratorMethods, { }, { } };

// ---- Views ----

template<int kind>
static JSValue odictViewIter(JSGlobalObject* globalObject, JSValue self) { return makeODictIterator(globalObject, PyDictView::from(self)->dict(), kind); }

static constexpr NativeDef odictKeysMethods[] = {
    PY_SLOT("__iter__", odictViewIter<ODictKeys>, "($self, /)"),
    PY_METHOD("__reversed__", odictViewIter<ODictKeys | ODictReversed>, "($self, /)"),
};
static constexpr NativeDef odictValuesMethods[] = {
    PY_SLOT("__iter__", odictViewIter<ODictValues>, "($self, /)"),
    PY_METHOD("__reversed__", odictViewIter<ODictValues | ODictReversed>, "($self, /)"),
};
static constexpr NativeDef odictItemsMethods[] = {
    PY_SLOT("__iter__", odictViewIter<ODictItems>, "($self, /)"),
    PY_METHOD("__reversed__", odictViewIter<ODictItems | ODictReversed>, "($self, /)"),
};
static constexpr TypeDef odictKeysTypeDef { "odict_keys"_s, { }, odictKeysMethods, { }, { } };
static constexpr TypeDef odictValuesTypeDef { "odict_values"_s, { }, odictValuesMethods, { }, { } };
static constexpr TypeDef odictItemsTypeDef { "odict_items"_s, { }, odictItemsMethods, { }, { } };

static JSValue makeODictView(JSGlobalObject* globalObject, JSValue self, PyDictView::Kind kind)
{
    Runtime& rt = runtime(globalObject);
    PyDictView* view = PyDictView::create(globalObject, kind, PyDict::from(self));
    PyType* type = kind == PyDictView::Kind::Keys ? rt.typeFor(odictKeysTypeDef, rt.dictKeysType())
        : kind == PyDictView::Kind::Values ? rt.typeFor(odictValuesTypeDef, rt.dictValuesType()) : rt.typeFor(odictItemsTypeDef, rt.dictItemsType());
    view->setType(rt.vm(), type);
    return view;
}

static JSValue odictKeys(JSGlobalObject* globalObject, JSValue self) { return makeODictView(globalObject, self, PyDictView::Kind::Keys); }
static JSValue odictValues(JSGlobalObject* globalObject, JSValue self) { return makeODictView(globalObject, self, PyDictView::Kind::Values); }
static JSValue odictItems(JSGlobalObject* globalObject, JSValue self) { return makeODictView(globalObject, self, PyDictView::Kind::Items); }
static JSValue odictIter(JSGlobalObject* globalObject, JSValue self) { return makeODictIterator(globalObject, PyDict::from(self), ODictKeys); }
static JSValue odictReversed(JSGlobalObject* globalObject, JSValue self) { return makeODictIterator(globalObject, PyDict::from(self), ODictKeys | ODictReversed); }

// ---- update(), as collections.abc.MutableMapping has it: through __setitem__ ----

// A pair is whatever can be gone through and gives two.
static bool addPair(JSGlobalObject* globalObject, JSValue self, JSValue pair)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = getIter(globalObject, pair);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue key = iterNext(globalObject, iterator);
    RETURN_IF_EXCEPTION(scope, false);
    if (!key) {
        raiseValueError(globalObject, scope, "need more than 0 values to unpack"_s);
        return false;
    }
    JSValue value = iterNext(globalObject, iterator);
    RETURN_IF_EXCEPTION(scope, false);
    if (!value) {
        raiseValueError(globalObject, scope, "need more than 1 value to unpack"_s);
        return false;
    }
    JSValue unexpected = iterNext(globalObject, iterator);
    RETURN_IF_EXCEPTION(scope, false);
    if (unexpected) {
        raiseValueError(globalObject, scope, "too many values to unpack (expected 2)"_s);
        return false;
    }
    setItem(globalObject, self, key, value);
    return !scope.exception();
}

static bool addPairs(JSGlobalObject* globalObject, JSValue self, JSValue pairs)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    forEach(globalObject, pairs, [&](JSValue pair) {
        addPair(globalObject, self, pair);
        return !scope.exception();
    });
    return !scope.exception();
}

static bool updateFrom(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* dict = PyDict::tryFrom(other); dict && (dict->type() == rt.dictType() || dict->type() == rt.frozendictType())) {
        // Into an OrderedDict itself nothing of the program's runs but the methods of the keys.
        auto* target = PyDict::tryFrom(self);
        if (target && !isExactOrderedDict(globalObject, target))
            target = nullptr;
        if (target && dict->hasOnlyStringKeys())
            RELEASE_AND_RETURN(scope, (dictUpdate(globalObject, target, dict), !scope.exception()));
        // The items as they are now are stored one by one, so each key is asked for its hash again.
        JSValue iterator = getIter(globalObject, PyDictView::create(globalObject, PyDictView::Kind::Items, dict));
        RETURN_IF_EXCEPTION(scope, false);
        JSArray* items = listFromIterable(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!target)
            RELEASE_AND_RETURN(scope, addPairs(globalObject, self, items));
        for (unsigned i = 0; i < listLength(items); ++i) {
            // The list is an object like any other, which gc.get_objects() hands out to the __hash__ of a key. What lies there in the place of a tuple of two is taken apart as any pair is.
            JSValue item = listGet(items, i);
            auto* pair = PyTuple::tryFrom(item);
            if (!pair || pair->type() != rt.tupleType() || pair->length() != 2) [[unlikely]] {
                addPair(globalObject, self, item);
                RETURN_IF_EXCEPTION(scope, false);
                continue;
            }
            target->set(globalObject, pair->at(0), pair->at(1));
            RETURN_IF_EXCEPTION(scope, false);
        }
        return true;
    }
    JSValue keysMethod = getAttrIfPresent(globalObject, other, rt.names.keys());
    RETURN_IF_EXCEPTION(scope, false);
    if (keysMethod) {
        JSValue keys = call(globalObject, keysMethod);
        RETURN_IF_EXCEPTION(scope, false);
        forEach(globalObject, keys, [&](JSValue key) {
            JSValue value = getItem(globalObject, other, key);
            RETURN_IF_EXCEPTION(scope, false);
            setItem(globalObject, self, key, value);
            return !scope.exception();
        });
        return !scope.exception();
    }
    JSValue itemsMethod = getAttrIfPresent(globalObject, other, rt.names.items());
    RETURN_IF_EXCEPTION(scope, false);
    if (itemsMethod) {
        JSValue items = call(globalObject, itemsMethod);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, addPairs(globalObject, self, items));
    }
    RELEASE_AND_RETURN(scope, addPairs(globalObject, self, other));
}

static JSValue odictUpdate(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() > 1)
        return raiseTypeError(globalObject, scope, "update() takes at most 1 positional argument ("_s, args.size(), " given)"_s);
    if (args.size()) {
        bool ok = updateFrom(globalObject, self, args[0]);
        EXCEPTION_ASSERT(!scope.exception() == ok);
        if (!ok)
            return { };
    }
    PyTuple* keys = args.keywordCount() ? keywordKeys(globalObject, args.keywordNames()) : nullptr;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        setItem(globalObject, self, keys ? keys->at(i) : JSValue(args.keywordName(i)), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue odictInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() > 1)
        return raiseTypeError(globalObject, scope, "expected at most 1 argument, got "_s, args.size());
    RELEASE_AND_RETURN(scope, odictUpdate(globalObject, self, args));
}

// ---- Items ----

static JSValue odictSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t keyHash = PyDict::hashIfNeeded(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    PyDict::from(self)->set(globalObject, key, keyHash, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue odictDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t keyHash = PyDict::hashIfNeeded(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    bool removed = !!PyDict::from(self)->take(globalObject, key, keyHash);
    RETURN_IF_EXCEPTION(scope, { });
    if (!removed)
        return raiseKeyError(globalObject, scope, key);
    return jsUndefined();
}

static JSValue odictSetDefault(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = PyDict::from(self);
    if (!defaultValue)
        defaultValue = jsUndefined();
    if (isExactOrderedDict(globalObject, dict))
        RELEASE_AND_RETURN(scope, dict->setDefault(globalObject, key, defaultValue));
    bool exists = contains(globalObject, self, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (exists)
        RELEASE_AND_RETURN(scope, getItem(globalObject, self, key));
    setItem(globalObject, self, key, defaultValue);
    RETURN_IF_EXCEPTION(scope, { });
    return defaultValue;
}

// CPython's OrderedDict hands the key over as the value of the exception, not as its argument: a tuple is taken for all the
// arguments, and None for none.
static JSValue odictRaiseKeyError(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const JSValue> arguments { &key, 1 };
    if (isNone(key))
        arguments = { };
    else if (auto* tuple = PyTuple::tryFrom(key))
        arguments = tuple->span();
    PyException* exception = newException(globalObject, runtime(globalObject).KeyError(), arguments);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseException(globalObject, scope, exception);
}

static JSValue odictPop(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t keyHash = PyDict::hashIfNeeded(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = PyDict::from(self)->take(globalObject, key, keyHash);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    if (defaultValue)
        return defaultValue;
    RELEASE_AND_RETURN(scope, odictRaiseKeyError(globalObject, key));
}

static JSValue odictPopItem(JSGlobalObject* globalObject, JSValue self, JSValue lastValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool last = true;
    if (lastValue) {
        last = isTrue(globalObject, lastValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    PyDict* dict = PyDict::from(self);
    JSValue key, value;
    if (!(last ? dict->takeLast(vm, key, value) : dict->takeFirst(vm, key, value)))
        return raiseKeyError(globalObject, scope, makeStr(vm, "dictionary is empty"_s));
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { key, value }));
}

static JSValue odictMoveToEnd(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue lastValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool last = true;
    if (lastValue) {
        last = isTrue(globalObject, lastValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool found = false;
    // Nothing is looked for in an empty one, so the key need not even be hashable.
    if (PyDict::from(self)->size()) {
        int64_t keyHash = PyDict::hashIfNeeded(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        found = PyDict::from(self)->moveToEnd(globalObject, key, keyHash, last);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (found)
        return jsUndefined();
    RELEASE_AND_RETURN(scope, odictRaiseKeyError(globalObject, key));
}

static JSValue odictClear(JSGlobalObject* globalObject, JSValue self)
{
    PyDict::from(self)->clear(getVM(globalObject));
    return jsUndefined();
}

static JSValue odictCopy(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = PyDict::from(self);
    bool isExact = isExactOrderedDict(globalObject, dict);
    JSValue result = isExact ? JSValue(PyDict::create(vm, rt.structure<PyDict>(), dict->type())) : call(globalObject, dict->type());
    RETURN_IF_EXCEPTION(scope, { });

    uint32_t state = dict->keysVersion();
    JSValue key, value;
    int64_t hash;
    for (unsigned position = 0; dict->next(position, key, value, hash);) {
        if (isExact) {
            value = valueOfOwnKey(globalObject, dict, key, value);
            RETURN_IF_EXCEPTION(scope, { });
            PyDict::from(result)->set(globalObject, key, hash, value);
        } else {
            value = getItem(globalObject, self, key);
            RETURN_IF_EXCEPTION(scope, { });
            setItem(globalObject, result, key, value);
        }
        RETURN_IF_EXCEPTION(scope, { });
        if (dict->keysVersion() != state)
            return raiseMutated(globalObject, scope);
    }
    return result;
}

static JSValue odictFromKeys(JSGlobalObject* globalObject, JSValue cls, JSValue iterable, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value)
        value = jsUndefined();
    JSValue result = call(globalObject, cls);
    RETURN_IF_EXCEPTION(scope, { });
    forEach(globalObject, iterable, [&](JSValue key) {
        setItem(globalObject, result, key, value);
        return !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

// ---- The rest ----

static JSValue odictRepr(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = PyDict::from(self);
    if (dict->isEmpty())
        RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString(dict->type()->name(), "()"_s)));
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return makeStr(vm, "..."_s);
    // dict(self), which asks for the keys and then for the item of each.
    PyDict* copy = PyDict::create(globalObject);
    dictUpdate(globalObject, copy, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSString* text = repr(globalObject, copy);
    RETURN_IF_EXCEPTION(scope, { });
    String items = text->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString(dict->type()->name(), '(', items, ')')));
}

// 1, 0, or -1 for an exception.
static int keysAreEqualInOrder(JSGlobalObject* globalObject, PyDict* a, PyDict* b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t stateA = a->keysVersion();
    uint32_t stateB = b->keysVersion();
    unsigned positionA = 0;
    unsigned positionB = 0;
    while (true) {
        JSValue keyA, keyB, ignored;
        bool hasA = a->next(positionA, keyA, ignored);
        bool hasB = b->next(positionB, keyB, ignored);
        if (!hasA && !hasB)
            return 1;
        if (!hasA || !hasB)
            return 0;
        bool equal = richCompareBool(globalObject, keyA, keyB, CompareOperator::Eq);
        RETURN_IF_EXCEPTION(scope, -1);
        if (a->keysVersion() != stateA || b->keysVersion() != stateB) {
            raiseMutated(globalObject, scope);
            return -1;
        }
        if (!equal)
            return 0;
    }
}

template<bool wantEqual>
static JSValue odictEquality(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyDict::is(other))
        return rt.notImplemented();
    JSValue asDicts = callSpecial(globalObject, rt.dictType()->lookup(wantEqual ? Special::Eq : Special::Ne), self, other);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isOrderedDict(globalObject, other) || asDicts == jsBoolean(!wantEqual))
        return asDicts;
    int equal = keysAreEqualInOrder(globalObject, PyDict::from(self), PyDict::from(other));
    if (failed(scope, equal >= 0))
        return { };
    return jsBoolean(!!equal == wantEqual);
}

static JSValue odictNotOrdered(JSGlobalObject* globalObject, JSValue, JSValue) { return runtime(globalObject).notImplemented(); }

static JSValue odictUnion(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool leftIsOrdered = isOrderedDict(globalObject, left);
    JSValue other = leftIsOrdered ? right : left;
    if (!PyDict::is(other))
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    JSValue result = call(globalObject, PyDict::from(leftIsOrdered ? left : right)->type(), left);
    RETURN_IF_EXCEPTION(scope, { });
    updateFrom(globalObject, result, right);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue odictOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return odictUnion(globalObject, self, other); }
static JSValue odictROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return odictUnion(globalObject, other, self); }

static JSValue odictIOr(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    updateFrom(globalObject, self, other);
    RETURN_IF_EXCEPTION(scope, { });
    return self;
}

static JSValue odictReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue state = getObjectState(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue items = callMethod(globalObject, self, rt.names.items());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue iterator = getIter(globalObject, items);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { JSValue(PyDict::from(self)->type()), rt.emptyTuple(), state, jsUndefined(), iterator }));
}

// More than the dict it is, as in CPython, where it has a list besides.
static JSValue odictSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue base = call(globalObject, rt.dictType()->lookup(internString(vm, "__sizeof__"_s)), self);
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(base.asNumber() + 2 * sizeof(void*));
}

static constexpr NativeDef orderedDictMethods[] = {
    PY_SLOT("__init__", odictInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__setitem__", odictSetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", odictDelItem, "($self, key, /)"),
    PY_SLOT("__iter__", odictIter, "($self, /)"),
    PY_SLOT("__repr__", odictRepr, "($self, /)"),
    PY_SLOT("__eq__", odictEquality<true>, "($self, value, /)"),
    PY_SLOT("__ne__", odictEquality<false>, "($self, value, /)"),
    PY_SLOT("__lt__", odictNotOrdered, "($self, value, /)"),
    PY_SLOT("__le__", odictNotOrdered, "($self, value, /)"),
    PY_SLOT("__gt__", odictNotOrdered, "($self, value, /)"),
    PY_SLOT("__ge__", odictNotOrdered, "($self, value, /)"),
    PY_SLOT("__or__", odictOr, "($self, value, /)"),
    PY_SLOT("__ror__", odictROr, "($self, value, /)"),
    PY_SLOT("__ior__", odictIOr, "($self, value, /)"),
    PY_METHOD("__reversed__", odictReversed, "($self, /)"),
    PY_METHOD("__reduce__", odictReduce, "($self, /)"),
    PY_METHOD("__sizeof__", odictSizeOf, "($self, /)"),
    PY_CLASSMETHOD("fromkeys", odictFromKeys, "($type, /, iterable, value=None)"),
    PY_METHOD("setdefault", odictSetDefault, "($self, /, key, default=None)"),
    PY_METHOD("pop", odictPop, "($self, /, key, default=<unrepresentable>)"),
    PY_METHOD("popitem", odictPopItem, "($self, /, last=True)"),
    PY_METHOD("keys", odictKeys, "($self, /)"),
    PY_METHOD("values", odictValues, "($self, /)"),
    PY_METHOD("items", odictItems, "($self, /)"),
    PY_METHOD("update", odictUpdate, "($self, *args, **kwargs)"),
    PY_METHOD("clear", odictClear, "($self, /)"),
    PY_METHOD("copy", odictCopy, "($self, /)"),
    PY_METHOD("move_to_end", odictMoveToEnd, "($self, /, key, last=True)"),
};

constexpr TypeDef orderedDictTypeDef { "collections.OrderedDict"_s, { TypeFlag::Base, TypeFlag::HasDict, TypeFlag::HasWeakref, TypeFlag::OwnLayout }, orderedDictMethods, { },
    "Dictionary that remembers insertion order"_s };

} // namespace Python
} // namespace JSC
