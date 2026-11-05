/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _functools, which functools.py takes partial, reduce, cmp_to_key and the cache behind lru_cache from.
// (Modules/_functoolsmodule.c in CPython is the reference.)
//
// The instances are PyRecords. What their fields hold is said by the enum in the namespace named after the type.

static ALWAYS_INLINE PyRecord* functoolsRecord(JSValue self) { return uncheckedDowncast<PyRecord>(self.asCell()); }

// The __get__ of what is bound to an instance as a function is.
static JSValue functoolsBindAsMethod(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue owner)
{
    if (!isNone(instance))
        return PyMethod::create(globalObject, self, instance);
    if (owner && !isNone(owner))
        return self;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "__get__(None, None) is invalid"_s);
}

// ---- Placeholder ----

static PyType* functoolsPlaceholderType(JSGlobalObject*);

// The only instance is kept with the type.
static JSValue functoolsPlaceholder(JSGlobalObject* globalObject)
{
    PyType* type = functoolsPlaceholderType(globalObject);
    if (JSValue placeholder = hiddenField(globalObject, type, 0))
        return placeholder;
    PyRecord* placeholder = PyRecord::create(globalObject, type, 0);
    setHiddenField(globalObject, type, 0, placeholder);
    return placeholder;
}

static JSValue functoolsPlaceholderNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.totalCount())
        return raiseTypeError(globalObject, scope, "PlaceholderType takes no arguments"_s);
    RELEASE_AND_RETURN(scope, functoolsPlaceholder(globalObject));
}

static JSValue functoolsPlaceholderName(JSGlobalObject* globalObject, JSValue) { return makeStr(getVM(globalObject), "Placeholder"_s); }

static constexpr NativeDef functoolsPlaceholderMethods[] = {
    PY_NEW(functoolsPlaceholderNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", functoolsPlaceholderName, "($self, /)"),
    PY_METHOD("__reduce__", functoolsPlaceholderName, "($self, /)"),
};
static constexpr TypeDef functoolsPlaceholderTypeDef { "functools._PlaceholderType"_s, { }, functoolsPlaceholderMethods, { }, { } };
static PyType* functoolsPlaceholderType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(functoolsPlaceholderTypeDef); }

// ---- partial ----

namespace FunctoolsPartial {
enum : unsigned { Function, Arguments, Keywords, Placeholder, FieldCount }; // number: how many of the arguments are placeholders.
}

static PyType* functoolsPartialType(JSGlobalObject*);

// How many, but for the last, which must not be one. -1: it is, a TypeError.
static int64_t functoolsCountPlaceholders(JSGlobalObject* globalObject, std::span<const JSValue> arguments, JSValue placeholder)
{
    if (arguments.empty())
        return 0;
    if (arguments.back() == placeholder) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseTypeError(globalObject, scope, "trailing Placeholders are not allowed"_s);
        return -1;
    }
    int64_t count = 0;
    for (JSValue argument : arguments)
        count += argument == placeholder;
    return count;
}

static JSValue functoolsPartialNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!args.size())
        return raiseTypeError(globalObject, scope, "type 'partial' takes at least one argument"_s);
    JSValue function = args[0];
    if (!isCallable(globalObject, function))
        return raiseTypeError(globalObject, scope, "the first argument must be callable"_s);
    JSValue placeholder = functoolsPlaceholder(globalObject);
    auto newArguments = args.positional().subspan(1);
    int64_t placeholderCount = functoolsCountPlaceholders(globalObject, newArguments, placeholder);
    if (failed(scope, placeholderCount >= 0))
        return { };
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        if (args.keywordValue(i) == placeholder)
            return raiseTypeError(globalObject, scope, "Placeholder cannot be passed as a keyword argument"_s);
    }

    // A partial of a partial that has no attributes of its own calls the function directly.
    PyTuple* innerArguments = nullptr;
    PyDict* innerKeywords = nullptr;
    int64_t innerPlaceholderCount = 0;
    if (PyRecord* inner = PyRecord::tryFrom(function, functoolsPartialType(globalObject)); inner && !instanceDict(globalObject, inner, false)) {
        function = inner->get(FunctoolsPartial::Function);
        innerArguments = PyTuple::from(inner->get(FunctoolsPartial::Arguments));
        innerKeywords = PyDict::from(inner->get(FunctoolsPartial::Keywords));
        innerPlaceholderCount = inner->number;
    }

    PyTuple* arguments;
    if (!innerArguments)
        arguments = PyTuple::create(globalObject, newArguments);
    else {
        // The new arguments take the place of the placeholders, from the left; what is left of them goes at the end.
        MarkedArgumentBuffer merged;
        size_t used = 0;
        for (JSValue item : innerArguments->span()) {
            if (item == placeholder && used < newArguments.size()) {
                item = newArguments[used++];
                --innerPlaceholderCount;
            }
            merged.append(item);
        }
        for (; used < newArguments.size(); ++used)
            merged.append(newArguments[used]);
        arguments = PyTuple::create(globalObject, spanOf(merged));
        placeholderCount += innerPlaceholderCount;
    }

    PyDict* keywords = innerKeywords ? innerKeywords->copy(globalObject) : PyDict::create(globalObject);
    mergeKeywordsInto(globalObject, keywords, args);
    RETURN_IF_EXCEPTION(scope, { });

    PyRecord* record = PyRecord::create(globalObject, PyType::from(cls), FunctoolsPartial::FieldCount);
    record->set(vm, FunctoolsPartial::Function, function);
    record->set(vm, FunctoolsPartial::Arguments, arguments);
    record->set(vm, FunctoolsPartial::Keywords, keywords);
    record->set(vm, FunctoolsPartial::Placeholder, placeholder);
    record->number = placeholderCount;
    return record;
}

// Both have keywords, and not all of them are plain strs, so that they can tell what they are asked. As measured on CPython: each of the caller's is looked for among the partial's; if
// it is there, it is stored in a copy of them, which looks for it again; if not, it follows them as it is, and no dict hears of it.
static NEVER_INLINE JSValue functoolsPartialCallWithKeys(JSGlobalObject* globalObject, JSValue function, MarkedArgumentBuffer& values, PyDict* keywords, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned positionalCount = values.size();
    PyTuple* keys = keywordKeys(globalObject, args.keywordNames());
    PyDict* merged = keywords;
    Vector<unsigned, 8> added;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        JSValue key = keys ? keys->at(i) : JSValue(args.keywordName(i));
        bool isThere = keywords->contains(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isThere) {
            added.append(i);
            continue;
        }
        if (merged == keywords)
            merged = keywords->copy(globalObject);
        merged->set(globalObject, key, args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    PyTuple* names = keywordNamesOf(globalObject, merged, args, added.span());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue key, value;
    for (unsigned position = 0; merged->next(position, key, value);)
        values.append(value);
    for (unsigned i : added)
        values.append(args.keywordValue(i));
    RELEASE_AND_RETURN(scope, call(globalObject, function, CallArgs(valuesOf(values), positionalCount, names)));
}

static JSValue functoolsPartialCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = functoolsRecord(self);
    JSValue function = record->get(FunctoolsPartial::Function);
    PyTuple* stored = PyTuple::from(record->get(FunctoolsPartial::Arguments));
    PyDict* keywords = PyDict::from(record->get(FunctoolsPartial::Keywords));
    if (args.size() < record->number)
        return raiseTypeError(globalObject, scope, "missing positional arguments in 'partial' call; expected at least "_s, record->number, ", got "_s, args.size());

    MarkedArgumentBuffer values;
    values.ensureCapacity(stored->length() + args.totalCount());
    if (values.hasOverflowed()) [[unlikely]]
        return raiseWithArgs(globalObject, scope, runtime(globalObject).MemoryError(), { });
    unsigned used = 0;
    if (record->number) {
        JSValue placeholder = record->get(FunctoolsPartial::Placeholder);
        for (JSValue item : stored->span())
            values.append(item == placeholder ? args[used++] : item);
    } else {
        for (JSValue item : stored->span())
            values.append(item);
    }
    for (; used < args.size(); ++used)
        values.append(args[used]);
    unsigned positionalCount = values.size();

    if (keywords->isEmpty()) {
        for (unsigned i = 0; i < args.keywordCount(); ++i)
            values.append(args.keywordValue(i));
        RELEASE_AND_RETURN(scope, call(globalObject, function, CallArgs(valuesOf(values), positionalCount, args.keywordNames())));
    }
    // The caller's keywords win, where the partial has them.
    if (args.hasKeywords()) {
        if (!keywords->hasOnlyStringKeys() || keywordKeys(globalObject, args.keywordNames())) [[unlikely]]
            RELEASE_AND_RETURN(scope, functoolsPartialCallWithKeys(globalObject, function, values, keywords, args));
        keywords = keywords->copy(globalObject);
        addKeywordsTo(globalObject, keywords, args);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, callWithTupleAndDict(globalObject, function, PyTuple::create(globalObject, spanOf(values)), keywords));
}

static JSValue functoolsPartialRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return makeStr(vm, "..."_s);
    PyRecord* record = functoolsRecord(self);
    // A __repr__ may change them.
    JSValue function = record->get(FunctoolsPartial::Function);
    PyTuple* arguments = PyTuple::from(record->get(FunctoolsPartial::Arguments));
    PyDict* keywords = PyDict::from(record->get(FunctoolsPartial::Keywords));

    StrBuilder list;
    for (unsigned i = 0; i < arguments->length(); ++i) {
        JSString* text = repr(globalObject, arguments->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        list.append(", "_s, text->value(globalObject).data);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue key;
    JSValue value;
    for (unsigned position = 0; keywords->next(position, key, value);) {
        JSString* keyText = str(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        JSString* valueText = repr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        list.append(", "_s, keyText->value(globalObject).data, '=', valueText->value(globalObject).data);
        RETURN_IF_EXCEPTION(scope, { });
    }
    PyType* type = record->type();
    JSValue module = getAttr(globalObject, type, runtime(globalObject).names.dModule());
    RETURN_IF_EXCEPTION(scope, { });
    JSString* moduleText = str(globalObject, module);
    RETURN_IF_EXCEPTION(scope, { });
    JSString* functionText = repr(globalObject, function);
    RETURN_IF_EXCEPTION(scope, { });
    String result = makeString(moduleText->value(globalObject).data, '.', type->qualname()->value(globalObject).data, '(', functionText->value(globalObject).data, list.toString(), ')');
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, result);
}

static JSValue functoolsPartialReduce(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* record = functoolsRecord(self);
    JSValue function = record->get(FunctoolsPartial::Function);
    PyDict* dict = instanceDict(globalObject, self, false);
    PyTuple* state = PyTuple::create(globalObject, { function, record->get(FunctoolsPartial::Arguments), record->get(FunctoolsPartial::Keywords), dict ? JSValue(dict) : jsUndefined() });
    return PyTuple::create(globalObject, { record->type(), PyTuple::create(globalObject, { function }), state });
}

static JSValue functoolsPartialSetState(JSGlobalObject* globalObject, JSValue self, JSValue stateValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = functoolsRecord(self);
    PyTuple* state = PyTuple::tryFrom(stateValue);
    bool isValid = state && state->length() == 4;
    if (isValid) {
        isValid = isCallable(globalObject, state->at(0)) && PyTuple::is(state->at(1))
            && (isNone(state->at(2)) || isDict(state->at(2))) && (isNone(state->at(3)) || isDict(state->at(3)));
    }
    if (!isValid)
        return raiseTypeError(globalObject, scope, "invalid partial state"_s);

    PyTuple* arguments = PyTuple::from(state->at(1));
    int64_t placeholderCount = functoolsCountPlaceholders(globalObject, arguments->span(), record->get(FunctoolsPartial::Placeholder));
    if (failed(scope, placeholderCount >= 0))
        return { };
    if (arguments->type() != rt.tupleType())
        arguments = PyTuple::create(globalObject, arguments->span());
    PyDict* keywords;
    if (isNone(state->at(2)))
        keywords = PyDict::create(globalObject);
    else {
        keywords = PyDict::from(state->at(2));
        if (keywords->type() != rt.dictType())
            keywords = keywords->copy(globalObject);
    }

    record->set(vm, FunctoolsPartial::Function, state->at(0));
    record->set(vm, FunctoolsPartial::Arguments, arguments);
    record->set(vm, FunctoolsPartial::Keywords, keywords);
    record->number = placeholderCount;
    if (!isNone(state->at(3)))
        setInstanceDict(globalObject, self, PyDict::from(state->at(3)));
    else if (instanceDict(globalObject, self, false)) {
        DeletePropertySlot slot;
        JSObject::deleteProperty(record, globalObject, rt.dictKey(), slot);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue functoolsPartialGetFunction(JSGlobalObject*, JSValue self) { return functoolsRecord(self)->get(FunctoolsPartial::Function); }
static JSValue functoolsPartialGetArguments(JSGlobalObject*, JSValue self) { return functoolsRecord(self)->get(FunctoolsPartial::Arguments); }
static JSValue functoolsPartialGetKeywords(JSGlobalObject*, JSValue self) { return functoolsRecord(self)->get(FunctoolsPartial::Keywords); }

static constexpr NativeDef functoolsPartialMethods[] = {
    PY_NEW(functoolsPartialNew, "($type, func, /, *args, **keywords)"),
    PY_SLOT("__call__", functoolsPartialCall, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", functoolsPartialRepr, "($self, /)"),
    PY_SLOT("__get__", functoolsBindAsMethod, "($self, instance, owner=None, /)"),
    PY_METHOD("__reduce__", functoolsPartialReduce, "($self, /)"),
    PY_METHOD("__setstate__", functoolsPartialSetState, "($self, object, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};
static constexpr NativeGetSet functoolsPartialGetSets[] = {
    PY_MEMBER("func", functoolsPartialGetFunction),
    PY_MEMBER("args", functoolsPartialGetArguments),
    PY_MEMBER("keywords", functoolsPartialGetKeywords),
};
static constexpr TypeDef functoolsPartialTypeDef {
    "functools.partial"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasDict, TypeFlag::HasWeakref }, functoolsPartialMethods, functoolsPartialGetSets,
    "Create a new function with partial application of the given arguments\nand keywords."_s
};
static PyType* functoolsPartialType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(functoolsPartialTypeDef); }

// ---- cmp_to_key ----

namespace FunctoolsKey {
enum : unsigned { Compare, Object, FieldCount }; // What cmp_to_key returns has no object. Called with one, it makes a key that has.
}

static PyType* functoolsKeyType(JSGlobalObject*);

static JSValue functoolsMakeKey(JSGlobalObject* globalObject, PyType* type, JSValue compare, JSValue object)
{
    VM& vm = getVM(globalObject);
    PyRecord* record = PyRecord::create(globalObject, type, FunctoolsKey::FieldCount);
    record->set(vm, FunctoolsKey::Compare, compare);
    record->set(vm, FunctoolsKey::Object, object);
    return record;
}

static JSValue functoolsCmpToKey(JSGlobalObject* globalObject, JSValue compare) { return functoolsMakeKey(globalObject, functoolsKeyType(globalObject), compare, { }); }

static JSValue functoolsKeyCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "obj"_s };
    JSValue object;
    if (failed(scope, unpackKeywordArguments(globalObject, "K"_s, keywords, 1, 1, false, args, &object)))
        return { };
    PyRecord* record = functoolsRecord(self);
    RELEASE_AND_RETURN(scope, functoolsMakeKey(globalObject, record->type(), record->get(FunctoolsKey::Compare), object));
}

template<CompareOperator op>
static JSValue functoolsKeyCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = functoolsRecord(self);
    if (rt.typeOf(other) != record->type())
        return raiseTypeError(globalObject, scope, "other argument must be K instance"_s);
    JSValue left = record->get(FunctoolsKey::Object);
    JSValue right = functoolsRecord(other)->get(FunctoolsKey::Object);
    if (!left || !right)
        return raise(globalObject, scope, rt.AttributeError(), "object"_s);
    JSValue order = call(globalObject, record->get(FunctoolsKey::Compare), left, right);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, richCompare(globalObject, order, jsNumber(0), op));
}

static JSValue functoolsKeyGetObject(JSGlobalObject*, JSValue self)
{
    JSValue object = functoolsRecord(self)->get(FunctoolsKey::Object);
    return object ? object : jsUndefined();
}

static void functoolsKeySetObject(JSGlobalObject* globalObject, JSValue self, JSValue value) { functoolsRecord(self)->set(getVM(globalObject), FunctoolsKey::Object, value); }
static JSValue functoolsKeyTextSignature(JSGlobalObject* globalObject, JSValue) { return makeStr(getVM(globalObject), "(obj)"_s); }

static constexpr NativeDef functoolsKeyMethods[] = {
    PY_SLOT("__call__", functoolsKeyCall, "($self, /, *args, **kwargs)"),
    PY_SLOT("__lt__", functoolsKeyCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", functoolsKeyCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__eq__", functoolsKeyCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", functoolsKeyCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__gt__", functoolsKeyCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", functoolsKeyCompare<CompareOperator::GtE>, "($self, value, /)"),
};
static constexpr NativeGetSet functoolsKeyGetSets[] = {
    { "obj"_s, functoolsKeyGetObject, functoolsKeySetObject },
    { "__text_signature__"_s, functoolsKeyTextSignature, nullptr },
};
static constexpr TypeDef functoolsKeyTypeDef { "functools.KeyWrapper"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, functoolsKeyMethods, functoolsKeyGetSets, { } };
static PyType* functoolsKeyType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(functoolsKeyTypeDef); }

// ---- reduce ----

static JSValue functoolsReduce(JSGlobalObject* globalObject, JSValue function, JSValue iterable, JSValue result)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    GeneratorArgumentScope argument(globalObject, iterable);
    JSValue iterator = getIter(globalObject, iterable);
    if (failed(scope, iterator)) {
        if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            return raiseTypeError(globalObject, scope, "reduce() arg 2 must support iteration"_s);
        return { };
    }
    while (true) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            break;
        if (!result) {
            result = item;
            continue;
        }
        result = call(globalObject, function, result, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!result)
        return raiseTypeError(globalObject, scope, "reduce() of empty iterable with no initial value"_s);
    return result;
}

// ---- The cache behind lru_cache ----
//
// With a bound, what the dict maps a key to is a link in a ring that starts at the cache itself and goes from what was used
// longest ago to what was used last.

namespace FunctoolsLink {
enum : unsigned { Previous, Next, Key, Result, FieldCount }; // number: the hash of the key.
}
namespace FunctoolsCache {
// number: the bound, 0 to cache nothing, or -1 for no bound.
enum : unsigned { Previous = FunctoolsLink::Previous, Next = FunctoolsLink::Next, Function, Cache, CacheInfoType, KeywordMark, LinkType, IsTyped, Hits, Misses, FieldCount };
}

static constexpr TypeDef functoolsLinkTypeDef { "functools._lru_list_elem"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, { }, { }, { } };

static PyType* functoolsCacheType(JSGlobalObject*);

static JSValue functoolsCacheNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "user_function"_s, "maxsize"_s, "typed"_s, "cache_info_type"_s };
    JSValue values[4];
    if (failed(scope, unpackKeywordArguments(globalObject, "lru_cache"_s, keywords, 4, 4, false, args, values)))
        return { };
    bool isTyped = isTrue(globalObject, values[2]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isCallable(globalObject, values[0]))
        return raiseTypeError(globalObject, scope, "the first argument must be callable"_s);
    int64_t maxSize = -1;
    if (!isNone(values[1])) {
        if (!hasIndex(globalObject, values[1]))
            return raiseTypeError(globalObject, scope, "maxsize should be integer or None"_s);
        maxSize = toSsize(globalObject, values[1], rt.OverflowError());
        RETURN_IF_EXCEPTION(scope, { });
        maxSize = std::max<int64_t>(maxSize, 0);
    }

    // What sets the keywords apart from the positional arguments in a key. There is one for all the caches, kept with the type.
    PyType* type = functoolsCacheType(globalObject);
    JSValue keywordMark = hiddenField(globalObject, type, 0);
    if (!keywordMark) {
        keywordMark = allocateInstance(globalObject, rt.objectType());
        setHiddenField(globalObject, type, 0, keywordMark);
    }

    PyRecord* record = PyRecord::create(globalObject, PyType::from(cls), FunctoolsCache::FieldCount);
    record->set(vm, FunctoolsCache::Previous, record);
    record->set(vm, FunctoolsCache::Next, record);
    record->set(vm, FunctoolsCache::Function, values[0]);
    record->set(vm, FunctoolsCache::Cache, PyDict::create(globalObject));
    record->set(vm, FunctoolsCache::CacheInfoType, values[3]);
    record->set(vm, FunctoolsCache::KeywordMark, keywordMark);
    record->set(vm, FunctoolsCache::LinkType, rt.typeFor(functoolsLinkTypeDef));
    RETURN_IF_EXCEPTION(scope, { });
    record->set(vm, FunctoolsCache::IsTyped, jsBoolean(isTyped));
    record->set(vm, FunctoolsCache::Hits, jsNumber(0));
    record->set(vm, FunctoolsCache::Misses, jsNumber(0));
    record->number = maxSize;
    return record;
}

static JSValue functoolsMakeCacheKey(JSGlobalObject* globalObject, PyRecord* cache, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    bool isTyped = cache->get(FunctoolsCache::IsTyped).asBoolean();
    unsigned count = args.size();
    unsigned keywordCount = args.keywordCount();
    if (!isTyped && !keywordCount) {
        // A str or an int stands for itself.
        if (count == 1 && (isExactStr(args[0]) || isExactInt(args[0])))
            return args[0];
        return PyTuple::create(globalObject, args.positional());
    }
    unsigned size = count;
    if (keywordCount)
        size += keywordCount * 2 + 1;
    if (isTyped)
        size += count + keywordCount;
    PyTuple* key = PyTuple::create(globalObject, size);
    unsigned position = 0;
    for (unsigned i = 0; i < count; ++i)
        key->initializeAt(vm, position++, args[i]);
    if (keywordCount) {
        key->initializeAt(vm, position++, cache->get(FunctoolsCache::KeywordMark));
        PyTuple* keys = keywordKeys(globalObject, args.keywordNames());
        for (unsigned i = 0; i < keywordCount; ++i) {
            key->initializeAt(vm, position++, keys ? keys->at(i) : JSValue(args.keywordName(i)));
            key->initializeAt(vm, position++, args.keywordValue(i));
        }
    }
    if (isTyped) {
        for (unsigned i = 0; i < count + keywordCount; ++i)
            key->initializeAt(vm, position++, rt.typeOf(args.values()[i]));
    }
    ASSERT(position == size);
    return key;
}

static ALWAYS_INLINE void functoolsCount(VM& vm, PyRecord* cache, unsigned field) { cache->set(vm, field, jsNumber(cache->get(field).asNumber() + 1)); }

static void functoolsExtractLink(VM& vm, PyRecord* link)
{
    PyRecord* previous = functoolsRecord(link->get(FunctoolsLink::Previous));
    PyRecord* next = functoolsRecord(link->get(FunctoolsLink::Next));
    previous->set(vm, FunctoolsLink::Next, next);
    next->set(vm, FunctoolsLink::Previous, previous);
}

static void functoolsInsertLink(VM& vm, PyRecord* link, PyRecord* previous, PyRecord* next)
{
    previous->set(vm, FunctoolsLink::Next, link);
    next->set(vm, FunctoolsLink::Previous, link);
    link->set(vm, FunctoolsLink::Previous, previous);
    link->set(vm, FunctoolsLink::Next, next);
}

// As the last used.
static void functoolsAppendLink(VM& vm, PyRecord* cache, PyRecord* link) { functoolsInsertLink(vm, link, functoolsRecord(cache->get(FunctoolsCache::Previous)), cache); }

static JSValue functoolsCacheCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = functoolsRecord(self);
    JSValue function = record->get(FunctoolsCache::Function);
    int64_t maxSize = record->number;
    if (!maxSize) {
        functoolsCount(vm, record, FunctoolsCache::Misses);
        RELEASE_AND_RETURN(scope, call(globalObject, function, args));
    }

    PyDict* cache = PyDict::from(record->get(FunctoolsCache::Cache));
    JSValue key = functoolsMakeCacheKey(globalObject, record, args);
    int64_t keyHash = PyDict::hashIfNeeded(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue found = cache->get(globalObject, key, keyHash);
    RETURN_IF_EXCEPTION(scope, { });

    if (maxSize < 0) {
        if (found) {
            functoolsCount(vm, record, FunctoolsCache::Hits);
            return found;
        }
        functoolsCount(vm, record, FunctoolsCache::Misses);
        JSValue result = call(globalObject, function, args);
        RETURN_IF_EXCEPTION(scope, { });
        cache->set(globalObject, key, keyHash, result);
        RETURN_IF_EXCEPTION(scope, { });
        return result;
    }

    if (found) {
        PyRecord* link = functoolsRecord(found);
        functoolsExtractLink(vm, link);
        functoolsAppendLink(vm, record, link);
        functoolsCount(vm, record, FunctoolsCache::Hits);
        return link->get(FunctoolsLink::Result);
    }
    functoolsCount(vm, record, FunctoolsCache::Misses);
    JSValue result = call(globalObject, function, args);
    RETURN_IF_EXCEPTION(scope, { });

    // Whatever looks the key up may run an __eq__, which may call the cache. So the ring and the dict must agree before each.
    found = cache->get(globalObject, key, keyHash);
    RETURN_IF_EXCEPTION(scope, { });
    // The function has called itself with the same arguments.
    if (found)
        return result;

    PyRecord* oldest = functoolsRecord(record->get(FunctoolsCache::Next));
    if (cache->size() < maxSize || oldest == record) {
        PyRecord* link = PyRecord::create(globalObject, PyType::from(record->get(FunctoolsCache::LinkType)), FunctoolsLink::FieldCount);
        link->number = keyHash;
        link->set(vm, FunctoolsLink::Key, key);
        link->set(vm, FunctoolsLink::Result, result);
        cache->set(globalObject, key, keyHash, link);
        RETURN_IF_EXCEPTION(scope, { });
        functoolsAppendLink(vm, record, link);
        return result;
    }

    // It is full. The link of what was used longest ago is taken for the new key.
    functoolsExtractLink(vm, oldest);
    JSValue removed = cache->take(globalObject, oldest->get(FunctoolsLink::Key), oldest->number);
    if (scope.exception()) [[unlikely]] {
        // Back where it was.
        functoolsInsertLink(vm, oldest, record, functoolsRecord(record->get(FunctoolsCache::Next)));
        return { };
    }
    // The function has cleared the cache: the link belongs to nothing.
    if (!removed)
        return result;
    oldest->number = keyHash;
    oldest->set(vm, FunctoolsLink::Key, key);
    oldest->set(vm, FunctoolsLink::Result, result);
    cache->set(globalObject, key, keyHash, oldest);
    RETURN_IF_EXCEPTION(scope, { });
    functoolsAppendLink(vm, record, oldest);
    return result;
}

static JSValue functoolsCacheInfo(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* record = functoolsRecord(self);
    JSValue maxSize = record->number < 0 ? jsUndefined() : makeInt(globalObject, record->number);
    return call(globalObject, record->get(FunctoolsCache::CacheInfoType), record->get(FunctoolsCache::Hits), record->get(FunctoolsCache::Misses), maxSize, jsNumber(PyDict::from(record->get(FunctoolsCache::Cache))->size()));
}

static JSValue functoolsCacheClear(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyRecord* record = functoolsRecord(self);
    record->set(vm, FunctoolsCache::Previous, record);
    record->set(vm, FunctoolsCache::Next, record);
    record->set(vm, FunctoolsCache::Hits, jsNumber(0));
    record->set(vm, FunctoolsCache::Misses, jsNumber(0));
    PyDict::from(record->get(FunctoolsCache::Cache))->clear(vm);
    return jsUndefined();
}

static JSValue functoolsCacheReduce(JSGlobalObject* globalObject, JSValue self) { return getAttr(globalObject, self, runtime(globalObject).names.dQualname()); }
static JSValue functoolsCacheCopy(JSGlobalObject*, JSValue self, const CallArgs&) { return self; }

static constexpr NativeDef functoolsCacheMethods[] = {
    PY_NEW(functoolsCacheNew, "($type, *args, **kwargs)"),
    PY_SLOT("__call__", functoolsCacheCall, "($self, /, *args, **kwargs)"),
    PY_SLOT("__get__", functoolsBindAsMethod, "($self, instance, owner=None, /)"),
    PY_METHOD("cache_info", functoolsCacheInfo, "($self, /)"),
    PY_METHOD("cache_clear", functoolsCacheClear, "($self, /)"),
    PY_METHOD("__reduce__", functoolsCacheReduce, "($self, /)"),
    PY_METHOD("__copy__", functoolsCacheCopy, "($self, *args)"),
    PY_METHOD("__deepcopy__", functoolsCacheCopy, "($self, *args)"),
};
static constexpr TypeDef functoolsCacheTypeDef {
    "functools._lru_cache_wrapper"_s, { TypeFlag::OwnLayout, TypeFlag::HasDict, TypeFlag::HasWeakref }, functoolsCacheMethods, { },
    "Create a cached callable that wraps another function.\n\n"
    "user_function:      the function being cached\n\n"
    "maxsize:  0         for no caching\n"
    "          None      for unlimited cache size\n"
    "          n         for a bounded cache\n\n"
    "typed:    False     cache f(3) and f(3.0) as identical calls\n"
    "          True      cache f(3) and f(3.0) as distinct calls\n\n"
    "cache_info_type:    namedtuple class with the fields:\n"
    "                        hits misses currsize maxsize\n"_s
};
static PyType* functoolsCacheType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(functoolsCacheTypeDef); }

// ---- The module ----

static constexpr NativeDef functoolsFunctions[] = {
    PY_FUNCTION_WITH_FLAGS("reduce", functoolsReduce, "(function, iterable, /, initial=functools._initial_missing)", ClosesGeneratorArgument),
    PY_FUNCTION("cmp_to_key", functoolsCmpToKey, "(mycmp)"),
};

void initializeFunctoolsModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    module->addFunctions(globalObject, functoolsFunctions);

    // What has __eq__ and no __hash__ cannot be hashed.
    PyType* keyType = functoolsKeyType(globalObject);
    keyType->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
    keyType->didChange(vm);

    module->add(globalObject, "_PlaceholderType"_s, functoolsPlaceholderType(globalObject));
    module->add(globalObject, "Placeholder"_s, functoolsPlaceholder(globalObject));
    module->add(globalObject, "partial"_s, functoolsPartialType(globalObject));
    module->add(globalObject, "_lru_cache_wrapper"_s, functoolsCacheType(globalObject));
    module->add(globalObject, "__doc__"_s, makeStr(vm, "Tools that operate on functions."_s));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
