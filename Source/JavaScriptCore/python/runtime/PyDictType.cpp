/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyDict.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyTypes.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The types dict and frozendict, and the views of them. (Objects/dictobject.c in CPython is the reference.)
// How a dict is stored is in PyDict.cpp.

// ---- update ----

static bool iteratesAsDict(JSGlobalObject* globalObject, PyDict* dict);

// for key in other.keys(): dict[key] = other[key]. With `duplicate`, a key that is there already ends it: that is where it is put.
static void mergeMapping(JSGlobalObject* globalObject, PyDict* dict, JSValue other, JSValue* duplicate = nullptr)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto hashOf = [&](JSValue key) {
        return hashForContainer(globalObject, key, dict->type()->has(TypeFlag::DictSubclass) ? "a dict key"_s : "a frozendict key"_s);
    };
    // False: it is, or an exception.
    auto isNew = [&](JSValue key, int64_t hash) {
        if (!duplicate)
            return true;
        bool present = !!dict->get(globalObject, key, hash);
        RETURN_IF_EXCEPTION(scope, false);
        if (present)
            *duplicate = key;
        return !present;
    };
    auto insert = [&](JSValue key, JSValue value, int64_t hash) {
        if (!isNew(key, hash))
            return false;
        dict->set(globalObject, key, hash, value);
        return !scope.exception();
    };

    if (auto* otherDict = PyDict::tryFrom(other); otherDict && iteratesAsDict(globalObject, otherDict)) {
        if (otherDict == dict || otherDict->isEmpty())
            return;
        if (dict->isEmpty()) {
            // Nothing can be there twice, so nothing is looked for before it is stored: the keys that a search comes by can tell.
            duplicate = nullptr;
            if (otherDict->hasTableThatFits())
                return dict->takeTableOf(vm, otherDict);
            // Otherwise the keys go in one by one, each compared with those of the same hash that are in already. Among strs nobody can tell. (Into
            // the table that the dict has, if it has one: what is left in that shows later.)
            if (otherDict->hasOnlyStringKeys() && !dict->storageIdentity())
                return dict->takeItemsOf(vm, otherDict);
        }
        dict->reserve(vm, otherDict->size(), otherDict->hasOnlyStringKeys());
        unsigned size = otherDict->size();
        JSValue key, value;
        int64_t hash;
        for (unsigned position = 0; otherDict->next(position, key, value, hash);) {
            if (!insert(key, value, hash))
                return;
            // The __eq__ of a key may have done anything. What is noticed is what CPython notices.
            if (otherDict->size() != size) [[unlikely]] {
                raiseRuntimeError(globalObject, scope, "dict mutated during update"_s);
                return;
            }
        }
        return;
    }

    // All the keys are taken before the first value is.
    JSValue keys = callMethod(globalObject, other, rt.names.keys());
    RETURN_IF_EXCEPTION(scope, void());
    JSArray* list;
    if (isExactList(keys))
        list = asList(keys);
    else {
        JSValue iterator = getIter(globalObject, keys);
        if (scope.exception()) [[unlikely]] {
            if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, rt.typeOf(other)), ".keys() must return an iterable, not "_s, typeNameForMessage(globalObject, rt.typeOf(keys)));
            return;
        }
        list = listFromIterable(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (unsigned i = 0; i < listLength(list); ++i) {
        JSValue key = listGet(list, i);
        if (duplicate) {
            int64_t hash = hashOf(key);
            RETURN_IF_EXCEPTION(scope, void());
            if (!isNew(key, hash))
                return;
        }
        // (The other is asked first, and then the key for its hash: a program can tell.)
        JSValue value = getItem(globalObject, other, key);
        RETURN_IF_EXCEPTION(scope, void());
        int64_t hash = hashOf(key);
        RETURN_IF_EXCEPTION(scope, void());
        dict->set(globalObject, key, hash, value);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// for key, value in pairs: dict[key] = value
static void mergePairs(JSGlobalObject* globalObject, PyDict* dict, JSValue pairs)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint64_t index = 0;
    forEach(globalObject, pairs, [&](JSValue item) {
        PyType* type = typeOf(globalObject, item);
        PyTuple* pair = nullptr;
        if (!type->lookup(Special::Iter) && !type->lookup(Special::GetItem))
            raiseTypeError(globalObject, scope, "object is not iterable"_s);
        else
            pair = tupleFromIterable(globalObject, item);
        if (scope.exception()) [[unlikely]] {
            if (pendingExceptionMatches(globalObject, runtime(globalObject).TypeError()))
                addNoteToPendingException(globalObject, makeString("Cannot convert dictionary update sequence element #"_s, index, " to a sequence"_s));
            return false;
        }
        if (pair->length() != 2) {
            raiseValueError(globalObject, scope, "dictionary update sequence element #"_s, index, " has length "_s, pair->length(), "; 2 is required"_s);
            return false;
        }
        dict->set(globalObject, pair->at(0), pair->at(1));
        RETURN_IF_EXCEPTION(scope, false);
        ++index;
        return true;
    });
    scope.release();
}

void dictUpdate(JSGlobalObject* globalObject, PyDict* dict, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyDict::is(other))
        RELEASE_AND_RETURN(scope, mergeMapping(globalObject, dict, other));
    bool hasKeys = hasAttr(globalObject, other, rt.names.keys());
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    if (hasKeys)
        mergeMapping(globalObject, dict, other);
    else
        mergePairs(globalObject, dict, other);
}

void dictUpdateFromMapping(JSGlobalObject* globalObject, PyDict* dict, JSValue mapping)
{
    mergeMapping(globalObject, dict, mapping);
}

// "module.qualname()", for a message about a call.
static String functionNameForMessage(JSGlobalObject* globalObject, JSValue function)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto textOf = [&](JSValue value) -> String {
        JSString* string = str(globalObject, value);
        if (scope.exception()) {
            scope.clearException();
            return "?"_s;
        }
        String text = string->value(globalObject);
        if (scope.exception()) {
            scope.clearException();
            return "?"_s;
        }
        return text;
    };
    JSValue qualname = getAttrIfPresent(globalObject, function, rt.names.dQualname());
    if (scope.exception())
        scope.clearException();
    if (!qualname)
        return textOf(function);
    JSValue module = getAttrIfPresent(globalObject, function, rt.names.dModule());
    if (scope.exception())
        scope.clearException();
    if (module && !isNone(module)) {
        String moduleName = textOf(module);
        if (moduleName != "builtins"_s)
            return makeString(moduleName, '.', textOf(qualname), "()"_s);
    }
    return makeString(textOf(qualname), "()"_s);
}

void dictMergeForCall(JSGlobalObject* globalObject, PyDict* dict, JSValue mapping, JSValue function)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue duplicate;
    mergeMapping(globalObject, dict, mapping, &duplicate);
    if (duplicate) [[unlikely]] {
        scope.assertNoException();
        String functionName = functionNameForMessage(globalObject, function);
        JSString* name = str(globalObject, duplicate);
        RETURN_IF_EXCEPTION(scope, void());
        raiseTypeError(globalObject, scope, functionName, " got multiple values for keyword argument '"_s, name->value(globalObject).data, '\'');
        return;
    }
    if (!scope.exception()) [[likely]]
        return;
    // What has no keys() is no mapping.
    if (!pendingExceptionMatches(globalObject, rt.AttributeError()))
        return;
    JSValue original = takePendingException(globalObject);
    bool hasKeys = hasAttr(globalObject, mapping, rt.names.keys());
    RETURN_IF_EXCEPTION(scope, void());
    if (hasKeys)
        throwPythonException(globalObject, scope, original);
    else
        raiseTypeError(globalObject, scope, "Value after ** must be a mapping, not "_s, typeNameForMessage(globalObject, rt.typeOf(mapping)));
}

// dict(*args, **kwargs), dict.update(*args, **kwargs)
// `mergesKeywords`: CPython has the keywords in a dict and merges that, so room is made as for a dict. Only dict() itself stores them one by one.
static bool updateFromArguments(JSGlobalObject* globalObject, PyDict* dict, const CallArgs& args, ASCIILiteral function, bool mergesKeywords)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, function, args, 0, 1)))
        return false;
    if (args.size()) {
        dictUpdate(globalObject, dict, args[0]);
        RETURN_IF_EXCEPTION(scope, false);
    }
    PyTuple* keys = args.keywordCount() ? keywordKeys(globalObject, args.keywordNames()) : nullptr;
    if (mergesKeywords && args.keywordCount()) {
        // (An empty dict takes the table of the other, whatever is left in its own.)
        if (dict->isEmpty())
            dict->clear(vm);
        dict->reserve(vm, args.keywordCount(), !keys);
    }
    if (mergesKeywords && keys) [[unlikely]] {
        mergeKeywordsInto(globalObject, dict, args);
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        // (A key of another kind that is there may say that it is equal to the name.)
        dict->set(globalObject, keys ? keys->at(i) : JSValue(args.keywordName(i)), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

// ---- dict ----

#define PY_DICT_PROLOGUE() \
    VM& vm = getVM(globalObject); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    PyDict* dict = PyDict::from(self); \
    UNUSED_VARIABLE(dict)

static JSValue dictNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    return PyDict::create(rt.vm(), rt.structure<PyDict>(), PyType::from(cls));
}

static JSValue dictInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = PyDict::from(self);
    if (failed(scope, updateFromArguments(globalObject, dict, args, "dict"_s, dict->type() != runtime(globalObject).dictType())))
        return { };
    return jsUndefined();
}

static bool appendDictRepr(JSGlobalObject* globalObject, PyDict* dict, StringBuilder& builder)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ReprGuard guard(globalObject, dict);
    if (guard.isRecursive()) {
        builder.append("{...}"_s);
        return true;
    }
    builder.append('{');
    bool first = true;
    JSValue key, value;
    for (unsigned position = 0; dict->next(position, key, value);) {
        if (!first)
            builder.append(", "_s);
        first = false;
        for (unsigned part = 0; part < 2; ++part) {
            JSString* text = repr(globalObject, part ? value : key);
            RETURN_IF_EXCEPTION(scope, false);
            auto view = text->view(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            builder.append(view.data);
            if (!part)
                builder.append(": "_s);
        }
    }
    builder.append('}');
    return true;
}

static JSValue dictRepr(JSGlobalObject* globalObject, JSValue self)
{
    PY_DICT_PROLOGUE();
    if (dict->isEmpty())
        return makeStr(vm, "{}"_s);
    StrBuilder builder;
    if (failed(scope, appendDictRepr(globalObject, dict, builder)))
        return { };
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue dictLen(JSGlobalObject*, JSValue self) { return jsNumber(PyDict::from(self)->size()); }

JSValue dictSubscript(JSGlobalObject* globalObject, PyDict* dict, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = dict->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    if (dict->type() != rt.dictType()) {
        if (JSValue missing = dict->type()->lookup(Special::Missing))
            RELEASE_AND_RETURN(scope, callSpecial(globalObject, missing, dict, key));
    }
    return raiseKeyError(globalObject, scope, key);
}

static JSValue dictGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key) { return dictSubscript(globalObject, PyDict::from(self), key); }

static JSValue dictSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    PY_DICT_PROLOGUE();
    dict->set(globalObject, key, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue dictDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    PY_DICT_PROLOGUE();
    bool removed = dict->remove(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!removed)
        return raiseKeyError(globalObject, scope, key);
    return jsUndefined();
}

static JSValue dictContains(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    PY_DICT_PROLOGUE();
    bool found = dict->contains(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

static PyIterator* makeDictIterator(JSGlobalObject* globalObject, PyIterator::Kind kind, PyDict* dict)
{
    PyIterator* iterator = PyIterator::create(globalObject, kind, dict);
    iterator->remaining = dict->size();
    if (kind >= PyIterator::Kind::DictReverseKeys)
        iterator->position = dict->endPosition();
    return iterator;
}

static JSValue dictIter(JSGlobalObject* globalObject, JSValue self) { return makeDictIterator(globalObject, PyIterator::Kind::DictKeys, PyDict::from(self)); }
static JSValue dictReversed(JSGlobalObject* globalObject, JSValue self) { return makeDictIterator(globalObject, PyIterator::Kind::DictReverseKeys, PyDict::from(self)); }

static bool iteratesAsDict(JSGlobalObject* globalObject, PyDict* dict)
{
    Runtime& rt = runtime(globalObject);
    if (dict->type() == rt.dictType() || dict->type() == rt.frozendictType())
        return true;
    auto* native = PyNativeFunction::tryFrom(dict->type()->lookup(Special::Iter));
    return native && native->def().pointer.f1 == dictIter;
}

static bool dictsAreEqual(JSGlobalObject* globalObject, PyDict* a, PyDict* b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (a->size() != b->size())
        return false;
    JSValue key, value;
    int64_t hash;
    for (unsigned position = 0; a->next(position, key, value, hash);) {
        JSValue other = b->get(globalObject, key, hash);
        RETURN_IF_EXCEPTION(scope, false);
        if (!other)
            return false;
        bool equal = isEqual(globalObject, value, other);
        RETURN_IF_EXCEPTION(scope, false);
        if (!equal)
            return false;
    }
    return true;
}

template<bool wantEqual>
static JSValue dictEquality(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    PY_DICT_PROLOGUE();
    auto* otherDict = PyDict::tryFrom(other);
    if (!otherDict)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    bool equal = dictsAreEqual(globalObject, dict, otherDict);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(equal == wantEqual);
}

// A copy of the same kind: a dict of a dict, a frozendict of a frozendict.
static PyDict* copyAsBaseType(JSGlobalObject* globalObject, PyDict* dict)
{
    Runtime& rt = runtime(globalObject);
    PyDict* result;
    if (iteratesAsDict(globalObject, dict) && (dict->hasFewDeletedEntries() || dict->hasOnlyStringKeys()))
        result = dict->copy(globalObject);
    else {
        // A class with an __iter__ of its own is asked for its keys and items. (And the keys of a dict that has lost many go in one by one.)
        auto scope = DECLARE_THROW_SCOPE(rt.vm());
        result = PyDict::create(globalObject);
        mergeMapping(globalObject, result, dict);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (!dict->type()->has(TypeFlag::DictSubclass))
        result->setType(rt.vm(), rt.frozendictType());
    return result;
}

static JSValue dictOrOf(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyDict::is(left) || !PyDict::is(right))
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    PyDict* result = copyAsBaseType(globalObject, PyDict::from(left));
    RETURN_IF_EXCEPTION(scope, { });
    mergeMapping(globalObject, result, right);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue dictOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return dictOrOf(globalObject, self, other); }
static JSValue dictROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return dictOrOf(globalObject, other, self); }

static JSValue dictIOr(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    PY_DICT_PROLOGUE();
    dictUpdate(globalObject, dict, other);
    RETURN_IF_EXCEPTION(scope, { });
    return self;
}

static JSValue dictGet(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    PY_DICT_PROLOGUE();
    JSValue value = dict->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    return defaultValue ? defaultValue : jsUndefined();
}

static JSValue dictSetDefault(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    return PyDict::from(self)->setDefault(globalObject, key, defaultValue ? defaultValue : jsUndefined());
}

static JSValue dictPop(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    PY_DICT_PROLOGUE();
    // Nothing is in an empty dict, not even what cannot be hashed.
    if (!dict->isEmpty()) {
        JSValue value = dict->take(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
    }
    if (defaultValue)
        return defaultValue;
    return raiseKeyError(globalObject, scope, key);
}

static JSValue dictPopItem(JSGlobalObject* globalObject, JSValue self)
{
    PY_DICT_PROLOGUE();
    JSValue key, value;
    if (!dict->takeLast(vm, key, value))
        return raiseKeyError(globalObject, scope, makeStr(vm, "popitem(): dictionary is empty"_s));
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { key, value }));
}

static JSValue dictKeys(JSGlobalObject* globalObject, JSValue self) { return PyDictView::create(globalObject, PyDictView::Kind::Keys, PyDict::from(self)); }
static JSValue dictValues(JSGlobalObject* globalObject, JSValue self) { return PyDictView::create(globalObject, PyDictView::Kind::Values, PyDict::from(self)); }
static JSValue dictItems(JSGlobalObject* globalObject, JSValue self) { return PyDictView::create(globalObject, PyDictView::Kind::Items, PyDict::from(self)); }

static JSValue dictUpdateMethod(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, updateFromArguments(globalObject, PyDict::from(self), args, "update"_s, true)))
        return { };
    return jsUndefined();
}

static JSValue dictClear(JSGlobalObject* globalObject, JSValue self)
{
    PyDict::from(self)->clear(getVM(globalObject));
    return jsUndefined();
}

static JSValue dictCopy(JSGlobalObject* globalObject, JSValue self) { return copyAsBaseType(globalObject, PyDict::from(self)); }

static JSValue dictFromKeys(JSGlobalObject* globalObject, JSValue cls, JSValue iterable, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value)
        value = jsUndefined();
    PyType* type = PyType::from(cls);
    if (type == rt.dictType() || type == rt.frozendictType()) {
        PyDict* result = PyDict::create(vm, rt.structure<PyDict>(), type);
        // The keys of a dict and the elements of a set come with their hashes, and differ from one another.
        if (auto* source = PyDict::tryFrom(iterable); source && (source->type() == rt.dictType() || source->type() == rt.frozendictType())) {
            result->reserve(vm, source->size(), source->hasOnlyStringKeys());
            JSValue key, ignored;
            int64_t hash;
            for (unsigned position = 0; source->next(position, key, ignored, hash);) {
                result->set(globalObject, key, hash, value);
                RETURN_IF_EXCEPTION(scope, { });
            }
            return result;
        }
        if (auto* source = PySet::tryFrom(iterable); source && (source->type() == rt.setType() || source->type() == rt.frozensetType())) {
            // (A table for keys of any kind, whatever the elements are: so CPython.)
            result->reserve(vm, source->size(), false);
            JSValue key;
            int64_t hash;
            for (size_t position = 0; source->next(position, key, hash);) {
                result->set(globalObject, key, hash, value);
                RETURN_IF_EXCEPTION(scope, { });
            }
            return result;
        }
        forEach(globalObject, iterable, [&](JSValue key) {
            result->set(globalObject, key, value);
            return true;
        });
        RETURN_IF_EXCEPTION(scope, { });
        return result;
    }
    JSValue result = call(globalObject, cls);
    RETURN_IF_EXCEPTION(scope, { });
    if (isFrozenDict(result)) {
        // It cannot be filled, so it is made again from what should be in it.
        PyDict* contents = PyDict::from(result)->copy(globalObject);
        forEach(globalObject, iterable, [&](JSValue key) {
            contents->set(globalObject, key, value);
            return true;
        });
        RETURN_IF_EXCEPTION(scope, { });
        contents->setType(vm, rt.frozendictType());
        RELEASE_AND_RETURN(scope, call(globalObject, cls, contents));
    }
    forEach(globalObject, iterable, [&](JSValue key) {
        setItem(globalObject, result, key, value);
        return true;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue dictSizeOf(JSGlobalObject*, JSValue self)
{
    return jsNumber(static_cast<double>(64 + 36 * static_cast<uint64_t>(PyDict::from(self)->size())));
}

#define PY_DICT_SHARED_METHODS \
    PY_SLOT("__len__", dictLen, "($self, /)"), \
    PY_METHOD("__getitem__", dictGetItem, "($self, key, /)"), \
    PY_METHOD("__contains__", dictContains, "($self, key, /)"), \
    PY_SLOT("__iter__", dictIter, "($self, /)"), \
    PY_SLOT("__eq__", dictEquality<true>, "($self, value, /)"), \
    PY_SLOT("__ne__", dictEquality<false>, "($self, value, /)"), \
    PY_SLOT("__ror__", dictROr, "($self, value, /)"), \
    PY_METHOD("__reversed__", dictReversed, "($self, /)"), \
    PY_METHOD("__sizeof__", dictSizeOf, "($self, /)"), \
    PY_METHOD("get", dictGet, "($self, key, default=None, /)"), \
    PY_METHOD("keys", dictKeys, "($self, /)"), \
    PY_METHOD("items", dictItems, "($self, /)"), \
    PY_METHOD("values", dictValues, "($self, /)"), \
    PY_CLASSMETHOD("fromkeys", dictFromKeys, "($type, iterable, value=None, /)"), \
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)")

static constexpr NativeDef dictMethods[] = {
    PY_NEW(dictNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", dictInit, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", dictRepr, "($self, /)"),
    PY_DICT_SHARED_METHODS,
    PY_SLOT("__setitem__", dictSetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", dictDelItem, "($self, key, /)"),
    PY_SLOT("__or__", dictOr, "($self, value, /)"),
    PY_SLOT("__ior__", dictIOr, "($self, value, /)"),
    PY_METHOD("setdefault", dictSetDefault, "($self, key, default=None, /)"),
    PY_METHOD("pop", dictPop, "($self, key, default=<unrepresentable>, /)"),
    PY_METHOD("popitem", dictPopItem, "($self, /)"),
    PY_METHOD("update", dictUpdateMethod, "($self, *args, **kwargs)"),
    PY_METHOD("clear", dictClear, "($self, /)"),
    PY_METHOD("copy", dictCopy, "($self, /)"),
};

// ---- frozendict ----

static JSValue frozendictNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Runtime& rt = runtime(globalObject);
    PyType* type = PyType::from(cls);
    if (type == rt.frozendictType() && args.size() == 1 && !args.hasKeywords()) {
        if (auto* existing = PyDict::tryFrom(args[0]); existing && existing->type() == type)
            return existing;
    }
    PyDict* result = PyDict::create(rt.vm(), rt.structure<PyDict>(), type);
    if (failed(scope, updateFromArguments(globalObject, result, args, "frozendict"_s, false)))
        return { };
    return result;
}

static JSValue frozendictRepr(JSGlobalObject* globalObject, JSValue self)
{
    PY_DICT_PROLOGUE();
    StrBuilder builder;
    builder.append(dict->type()->name(), '(');
    if (!dict->isEmpty() && failed(scope, appendDictRepr(globalObject, dict, builder)))
        return { };
    builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static int64_t frozendictHash(JSGlobalObject* globalObject, JSValue self)
{
    PY_DICT_PROLOGUE();
    if (dict->hasCachedHash())
        return dict->cachedHash();
    if (!isSafeToRecurseWithMargin(vm, 96 * KB)) {
        raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded while hashing a frozendict"_s);
        return -1;
    }
    // As for a frozenset of the (key, value) pairs. The keys are not asked again: the table has kept their hashes (but not those of strs).
    uint64_t result = 0;
    JSValue key, value;
    int64_t keyHash;
    for (unsigned position = 0; dict->next(position, key, value, keyHash);) {
        if (key.isString())
            keyHash = hashOfString(asString(key)->tryGetValueImpl());
        int64_t valueHash = hash(globalObject, value);
        RETURN_IF_EXCEPTION(scope, -1);
        TupleHasher pair;
        pair.add(keyHash);
        pair.add(valueHash);
        uint64_t h = static_cast<uint64_t>(pair.finish());
        result ^= ((h ^ 89869747UL) ^ (h << 16)) * 3644798167UL;
    }
    result ^= (static_cast<uint64_t>(dict->size()) + 1) * 1927868237UL;
    result ^= (result >> 11) ^ (result >> 25);
    result = result * 69069U + 907133923UL;
    if (result == static_cast<uint64_t>(-1))
        result = 590923713UL;
    dict->setCachedHash(static_cast<int64_t>(result));
    return static_cast<int64_t>(result);
}

static JSValue frozendictOr(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    PyDict* dict = PyDict::from(self);
    if (dict->type() == rt.frozendictType()) {
        if (auto* otherDict = PyDict::tryFrom(other)) {
            if (dict->isEmpty() && otherDict->type() == rt.frozendictType())
                return other;
            if (otherDict->isEmpty() && (otherDict->type() == rt.frozendictType() || otherDict->type() == rt.dictType()))
                return self;
        }
    }
    return dictOrOf(globalObject, self, other);
}

static JSValue frozendictCopy(JSGlobalObject* globalObject, JSValue self)
{
    PyDict* dict = PyDict::from(self);
    if (dict->type() == runtime(globalObject).frozendictType())
        return self;
    return copyAsBaseType(globalObject, dict);
}

static JSValue frozendictGetNewArgs(JSGlobalObject* globalObject, JSValue self)
{
    return PyTuple::create(globalObject, { JSValue(PyDict::from(self)->copy(globalObject)) });
}

static constexpr NativeDef frozendictMethods[] = {
    PY_NEW(frozendictNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", frozendictRepr, "($self, /)"),
    PY_HASH(frozendictHash),
    PY_DICT_SHARED_METHODS,
    PY_SLOT("__or__", frozendictOr, "($self, value, /)"),
    PY_METHOD("copy", frozendictCopy, "($self, /)"),
    PY_METHOD("__getnewargs__", frozendictGetNewArgs, "($self, /)"),
};

// ---- Views ----

PY_DEFINE_CELL(PyDictView);

PyDictView::PyDictView(VM& vm, Structure* structure, PyType* type, Kind kind)
    : Base(vm, structure, type)
    , m_kind(kind)
{
}

PyDictView* PyDictView::create(JSGlobalObject* globalObject, Kind kind, PyDict* dict)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyType* type = kind == Kind::Keys ? rt.dictKeysType() : kind == Kind::Values ? rt.dictValuesType() : rt.dictItemsType();
    auto* view = new (NotNull, allocateCell<PyDictView>(vm)) PyDictView(vm, rt.structure<PyDictView>(), type, kind);
    view->finishCreation(vm);
    view->m_dict.set(vm, view, dict);
    return view;
}

template<typename Visitor>
void PyDictView::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyDictView>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_dict);
}

DEFINE_VISIT_CHILDREN(PyDictView);

static JSValue viewLen(JSGlobalObject*, JSValue self) { return jsNumber(PyDictView::from(self)->dict()->size()); }

static JSValue viewIter(JSGlobalObject* globalObject, JSValue self)
{
    auto* view = PyDictView::from(self);
    static constexpr PyIterator::Kind kinds[] = { PyIterator::Kind::DictKeys, PyIterator::Kind::DictValues, PyIterator::Kind::DictItems };
    return makeDictIterator(globalObject, kinds[static_cast<unsigned>(view->kind())], view->dict());
}

static JSValue viewReversed(JSGlobalObject* globalObject, JSValue self)
{
    auto* view = PyDictView::from(self);
    static constexpr PyIterator::Kind kinds[] = { PyIterator::Kind::DictReverseKeys, PyIterator::Kind::DictReverseValues, PyIterator::Kind::DictReverseItems };
    return makeDictIterator(globalObject, kinds[static_cast<unsigned>(view->kind())], view->dict());
}

static JSValue viewRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyDictView::from(self);
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return makeStr(vm, "..."_s);
    JSArray* items = listFromIterable(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSString* text = repr(globalObject, items);
    RETURN_IF_EXCEPTION(scope, { });
    String string = text->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(view->type()->name(), '(', string, ')'));
}

static JSValue viewMapping(JSGlobalObject* globalObject, JSValue self) { return makeMappingProxy(globalObject, PyDictView::from(self)->dict()); }

static bool viewContainsItem(JSGlobalObject* globalObject, PyDictView* view, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (view->kind() == PyDictView::Kind::Keys)
        RELEASE_AND_RETURN(scope, view->dict()->contains(globalObject, item));
    auto* pair = PyTuple::tryFrom(item);
    if (!pair || pair->length() != 2)
        return false;
    JSValue value = view->dict()->get(globalObject, pair->at(0));
    RETURN_IF_EXCEPTION(scope, false);
    if (!value)
        return false;
    RELEASE_AND_RETURN(scope, isEqual(globalObject, value, pair->at(1)));
}

static JSValue viewContains(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool found = viewContainsItem(globalObject, PyDictView::from(self), item);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

// keys() and items() behave as sets.
static PyDictView* asSetLikeView(JSValue value)
{
    auto* view = PyDictView::tryFrom(value);
    return view && view->kind() != PyDictView::Kind::Values ? view : nullptr;
}

static bool allContainedIn(JSGlobalObject* globalObject, JSValue items, JSValue container)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = true;
    forEach(globalObject, items, [&](JSValue item) {
        result = contains(globalObject, container, item);
        return result;
    });
    RETURN_IF_EXCEPTION(scope, false);
    return result;
}

template<CompareOperator op>
static JSValue viewCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PySet::is(other) && !asSetLikeView(other))
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    int64_t lengthSelf = PyDictView::from(self)->dict()->size();
    int64_t lengthOther = length(globalObject, other);
    RETURN_IF_EXCEPTION(scope, { });
    bool result = false;
    switch (op) {
    case CompareOperator::Eq:
    case CompareOperator::NotEq:
        if (lengthSelf == lengthOther)
            result = allContainedIn(globalObject, self, other);
        RETURN_IF_EXCEPTION(scope, { });
        return jsBoolean(result == (op == CompareOperator::Eq));
    case CompareOperator::Lt:
        if (lengthSelf < lengthOther)
            result = allContainedIn(globalObject, self, other);
        break;
    case CompareOperator::LtE:
        if (lengthSelf <= lengthOther)
            result = allContainedIn(globalObject, self, other);
        break;
    case CompareOperator::Gt:
        if (lengthSelf > lengthOther)
            result = allContainedIn(globalObject, other, self);
        break;
    default:
        if (lengthSelf >= lengthOther)
            result = allContainedIn(globalObject, other, self);
        break;
    }
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static PySet* setFrom(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PySet* result = PySet::create(globalObject);
    // The keys of a dict as set(d) takes them: with room for all from the start, and with the hashes that the dict has kept.
    if (PyDictView* view = asSetLikeView(iterable); view && view->kind() == PyDictView::Kind::Keys && view->dict()->type() == runtime(globalObject).dictType())
        iterable = view->dict();
    setUpdate(globalObject, result, iterable);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return result;
}

static JSValue viewAnd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // For the reflected operation, the view is on the right.
    if (!asSetLikeView(self))
        std::swap(self, other);
    PyDictView* view = asSetLikeView(self);
    // A set that is not smaller does it: it goes through the view, which shows if an item cannot be hashed.
    if (auto* set = PySet::tryFrom(other); set && set->type() == rt.setType() && view->dict()->size() <= set->size())
        RELEASE_AND_RETURN(scope, callMethod(globalObject, other, internString(vm, "intersection"_s), self));
    // Go through the smaller of two views.
    if (PyDictView* otherView = asSetLikeView(other); otherView && otherView->dict()->size() > view->dict()->size()) {
        std::swap(self, other);
        view = otherView;
    }
    PySet* result = PySet::create(globalObject);
    forEach(globalObject, other, [&](JSValue item) {
        bool found = viewContainsItem(globalObject, view, item);
        RETURN_IF_EXCEPTION(scope, false);
        if (found)
            result->add(globalObject, item);
        return true;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue viewOrOf(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PySet* result = setFrom(globalObject, left);
    RETURN_IF_EXCEPTION(scope, { });
    setUpdate(globalObject, result, right);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue viewSubOf(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PySet* result = setFrom(globalObject, left);
    RETURN_IF_EXCEPTION(scope, { });
    setDifferenceUpdate(globalObject, result, right);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

// d.items() ^ e.items() goes by the keys, with the hashes that the dicts have kept, and makes pairs only of what is in the result. (Which keys and
// values are asked for that, and the order of the result, are CPython's.)
static JSValue itemsXor(JSGlobalObject* globalObject, PyDict* left, PyDict* right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* rest = copyAsBaseType(globalObject, left);
    RETURN_IF_EXCEPTION(scope, { });
    PySet* result = PySet::create(globalObject);
    JSValue key, value;
    int64_t hash;
    for (unsigned position = 0; right->next(position, key, value, hash);) {
        JSValue other = rest->get(globalObject, key, hash);
        RETURN_IF_EXCEPTION(scope, { });
        bool isInBoth = other && isEqual(globalObject, other, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (isInBoth) {
            // (A second search, which an __eq__ that has changed its mind may send elsewhere.)
            bool removed = !!rest->take(globalObject, key, hash);
            RETURN_IF_EXCEPTION(scope, { });
            if (!removed)
                return raiseKeyError(globalObject, scope, key);
        } else
            result->add(globalObject, PyTuple::create(globalObject, { key, value }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    setUpdate(globalObject, result, PyDictView::create(globalObject, PyDictView::Kind::Items, rest));
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue viewXorOf(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyDictView* leftView = asSetLikeView(left), *rightView = asSetLikeView(right); leftView && rightView && leftView->kind() == PyDictView::Kind::Items && rightView->kind() == PyDictView::Kind::Items)
        RELEASE_AND_RETURN(scope, itemsXor(globalObject, leftView->dict(), rightView->dict()));
    PySet* result = setFrom(globalObject, left);
    RETURN_IF_EXCEPTION(scope, { });
    setSymmetricDifferenceUpdate(globalObject, result, right);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue viewOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return viewOrOf(globalObject, self, other); }
static JSValue viewROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return viewOrOf(globalObject, other, self); }
static JSValue viewSub(JSGlobalObject* globalObject, JSValue self, JSValue other) { return viewSubOf(globalObject, self, other); }
static JSValue viewRSub(JSGlobalObject* globalObject, JSValue self, JSValue other) { return viewSubOf(globalObject, other, self); }
static JSValue viewXor(JSGlobalObject* globalObject, JSValue self, JSValue other) { return viewXorOf(globalObject, self, other); }
static JSValue viewRXor(JSGlobalObject* globalObject, JSValue self, JSValue other) { return viewXorOf(globalObject, other, self); }

static JSValue viewIsDisjoint(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyDictView::from(self);
    if (self == other)
        return jsBoolean(view->dict()->isEmpty());
    // Through the shorter of the two, if looking into the other is cheap.
    Runtime& rt = runtime(globalObject);
    auto* otherSet = PySet::tryFrom(other);
    if (otherSet && otherSet->type() != rt.setType() && otherSet->type() != rt.frozensetType())
        otherSet = nullptr;
    PyDictView* otherView = asSetLikeView(other);
    if ((otherSet && otherSet->size() > view->dict()->size()) || (otherView && otherView->dict()->size() > view->dict()->size())) {
        bool disjoint = true;
        forEach(globalObject, self, [&](JSValue item) {
            disjoint = !(otherSet ? setContains(globalObject, otherSet, item) : viewContainsItem(globalObject, otherView, item));
            return disjoint;
        });
        RETURN_IF_EXCEPTION(scope, { });
        return jsBoolean(disjoint);
    }
    bool disjoint = true;
    forEach(globalObject, other, [&](JSValue item) {
        disjoint = !viewContainsItem(globalObject, view, item);
        return disjoint;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(disjoint);
}

#define PY_VIEW_SHARED_METHODS \
    PY_SLOT("__len__", viewLen, "($self, /)"), \
    PY_SLOT("__iter__", viewIter, "($self, /)"), \
    PY_SLOT("__repr__", viewRepr, "($self, /)"), \
    PY_METHOD("__reversed__", viewReversed, "($self, /)")

static constexpr NativeDef setLikeViewMethods[] = {
    PY_VIEW_SHARED_METHODS,
    PY_SLOT("__contains__", viewContains, "($self, key, /)"),
    PY_SLOT("__eq__", viewCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", viewCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", viewCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", viewCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", viewCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", viewCompare<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__and__", viewAnd, "($self, value, /)"),
    PY_SLOT("__rand__", viewAnd, "($self, value, /)"),
    PY_SLOT("__or__", viewOr, "($self, value, /)"),
    PY_SLOT("__ror__", viewROr, "($self, value, /)"),
    PY_SLOT("__sub__", viewSub, "($self, value, /)"),
    PY_SLOT("__rsub__", viewRSub, "($self, value, /)"),
    PY_SLOT("__xor__", viewXor, "($self, value, /)"),
    PY_SLOT("__rxor__", viewRXor, "($self, value, /)"),
    PY_METHOD("isdisjoint", viewIsDisjoint, "($self, object, /)"),
};

static constexpr NativeDef valuesViewMethods[] = {
    PY_VIEW_SHARED_METHODS,
};

static constexpr NativeGetSet viewGetSets[] = {
    { "mapping"_s, viewMapping },
};

void initializeDictTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    rt.dictType()->initializeBuiltin(globalObject, { "dict"_s, { TypeFlag::Base, TypeFlag::DictSubclass, TypeFlag::Mapping, TypeFlag::MatchSelf, TypeFlag::OwnLayout, TypeFlag::GoesThroughArgument }, dictMethods, { }, "dict() -> new empty dictionary\ndict(mapping) -> new dictionary initialized from a mapping object's\n    (key, value) pairs\ndict(iterable) -> new dictionary initialized as if via:\n    d = {}\n    for k, v in iterable:\n        d[k] = v\ndict(**kwargs) -> new dictionary initialized with the name=value pairs\n    in the keyword argument list.  For example:  dict(one=1, two=2)"_s }, rt.typeType(), rt.objectType());
    rt.dictType()->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
    rt.dictType()->didChange(vm);
    rt.frozendictType()->initializeBuiltin(globalObject, { "frozendict"_s, { TypeFlag::Base, TypeFlag::Mapping, TypeFlag::MatchSelf, TypeFlag::OwnLayout }, frozendictMethods, { }, { } }, rt.typeType(), rt.objectType());

    rt.dictKeysType()->initializeBuiltin(globalObject, { "dict_keys"_s, { }, setLikeViewMethods, viewGetSets, { } }, rt.typeType(), rt.objectType());
    rt.dictItemsType()->initializeBuiltin(globalObject, { "dict_items"_s, { }, setLikeViewMethods, viewGetSets, { } }, rt.typeType(), rt.objectType());
    rt.dictValuesType()->initializeBuiltin(globalObject, { "dict_values"_s, { }, valuesViewMethods, viewGetSets, { } }, rt.typeType(), rt.objectType());
    for (PyType* type : { rt.dictKeysType(), rt.dictItemsType() }) {
        type->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
        type->didChange(vm);
    }
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
