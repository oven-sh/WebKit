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

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonOperators.h"
#include "PythonSequences.h"

// The module _functools: Modules/_functoolsmodule.c of CPython.

namespace JSC { namespace Python {

namespace {

struct FunctoolsModuleState final : NativeState {
    PYTHON_NATIVE_STATE(FunctoolsModuleState);
    // What comes between what was given by position and what was given by name, in what a cache goes by.
    WriteBarrier<Unknown> keywordMark;
    WriteBarrier<PyType> placeholderType;
    WriteBarrier<Unknown> placeholder;
    WriteBarrier<PyType> partialType;
    WriteBarrier<PyType> cacheType;
    WriteBarrier<PyType> keyType;
    WriteBarrier<PyType> linkType;
};

template<typename Visitor>
void FunctoolsModuleState::visit(Visitor& visitor)
{
    visitor.append(keywordMark);
    visitor.append(placeholderType);
    visitor.append(placeholder);
    visitor.append(partialType);
    visitor.append(cacheType);
    visitor.append(keyType);
    visitor.append(linkType);
}

FunctoolsModuleState& functoolsModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<FunctoolsModuleState>(); }

// What was given by name, as a dict. Null if there was nothing, or if it raised.
PyDict* keywordsOf(JSGlobalObject* globalObject, const NativeArguments& args)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!args.keywordCount())
        return nullptr;
    PyDict* keywords = PyDict::create(globalObject);
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        keywords->set(globalObject, args.keywordNameAsGiven(i), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return keywords;
}

// Whether `dict` in the struct of CPython's is not NULL: there is none until something is put in it or it is asked for.
bool hasInstanceDictYet(JSValue object) { return asObject(object)->structure()->maxOffset() != invalidOffset; }

// tp_descr_get of a partial and of what lru_cache makes: like a function, it is a method of what it is got from.
EncodedJSValue bindLikeFunction(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args)
{
    if (args.size() < 2 || args.size() > 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, args.size() < 2 ? concatenate("__get__ expected at least 1 argument, got "_s, args.size() - 1) : concatenate("__get__ expected at most 2 arguments, got "_s, args.size() - 1)));
    bool hasInstance = !isNone(args[1]);
    if (!hasInstance && (args.size() < 3 || isNone(args[2])))
        return JSValue::encode(raiseTypeError(globalObject, scope, "__get__(None, None) is invalid"_s));
    if (!hasInstance)
        return JSValue::encode(args[0]);
    return JSValue::encode(PyBoundMethod::createMethod(globalObject, args[0], args[1]));
}

} // anonymous namespace

// ---- Placeholder

namespace {

// There is nothing to it but that it is what it is.
struct Placeholder final : NativeState {
    PYTHON_NATIVE_STATE(Placeholder);
};

template<typename Visitor> void Placeholder::visit(Visitor&) { }

} // anonymous namespace

PYTHON_NATIVE(placeholderNew)
{
    NATIVE_PROLOGUE();
    if (args.size() > 1 || args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, "PlaceholderType takes no arguments"_s));
    auto& state = functoolsModuleState(globalObject);
    if (!state.placeholder)
        state.placeholder.set(vm, realm, PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<Placeholder>()));
    return JSValue::encode(state.placeholder.get());
}

// __repr__() and __reduce__()
PYTHON_NATIVE(placeholderName)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNontrivialString(globalObject->vm(), "Placeholder"_s));
}

// ---- partial

namespace {

struct Partial final : NativeState {
    PYTHON_NATIVE_STATE(Partial);
    WriteBarrier<Unknown> function;
    WriteBarrier<PyTuple> arguments;
    WriteBarrier<PyDict> keywords;
    unsigned placeholderCount { 0 };
};

template<typename Visitor>
void Partial::visit(Visitor& visitor)
{
    visitor.append(function);
    visitor.append(arguments);
    visitor.append(keywords);
}

// How many there are but for the last, which is not to be one.
unsigned countPlaceholders(PyTuple* arguments, JSValue placeholder)
{
    unsigned count = 0;
    for (unsigned i = 0; i + 1 < arguments->length(); ++i)
        count += arguments->at(i) == placeholder;
    return count;
}

} // anonymous namespace

PYTHON_NATIVE(partialNew)
{
    NATIVE_PROLOGUE();
    if (args.size() < 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "type 'partial' takes at least one argument"_s));
    JSValue function = args[1];
    if (!isCallable(globalObject, function))
        return JSValue::encode(raiseTypeError(globalObject, scope, "the first argument must be callable"_s));
    auto& module = functoolsModuleState(globalObject);
    JSValue placeholder = module.placeholder.get();
    unsigned newCount = args.size() - 2;
    if (newCount && args[args.size() - 1] == placeholder)
        return JSValue::encode(raiseTypeError(globalObject, scope, "trailing Placeholders are not allowed"_s));
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        if (args.keywordValue(i) == placeholder)
            return JSValue::encode(raiseTypeError(globalObject, scope, "Placeholder cannot be passed as a keyword argument"_s));
    }

    // Of a partial, it is of what that is of, with what that has and then this. But not if that has been given anything else, which would be lost.
    PyTuple* innerArguments = nullptr;
    PyDict* innerKeywords = nullptr;
    unsigned innerPlaceholderCount = 0;
    if (auto* inner = tryStateOf<Partial>(function); inner && !hasInstanceDictYet(function)) {
        innerArguments = inner->arguments.get();
        innerKeywords = inner->keywords.get();
        innerPlaceholderCount = inner->placeholderCount;
        function = inner->function.get();
    }

    PyTuple* newArguments = PyTuple::create(globalObject, newCount);
    for (unsigned i = 0; i < newCount; ++i)
        newArguments->initializeAt(vm, i, args[i + 2]);
    unsigned placeholderCount = countPlaceholders(newArguments, placeholder);

    PyTuple* arguments;
    if (innerPlaceholderCount && newCount) {
        // What is new goes where there is a place kept for it, and the rest at the end.
        unsigned innerCount = innerArguments->length();
        unsigned total = innerCount + (newCount > innerPlaceholderCount ? newCount - innerPlaceholderCount : 0);
        arguments = PyTuple::create(globalObject, total);
        for (unsigned i = 0, j = 0; i < total; ++i) {
            JSValue item;
            if (i < innerCount) {
                item = innerArguments->at(i);
                if (j < newCount && item == placeholder) {
                    item = newArguments->at(j++);
                    --innerPlaceholderCount;
                }
            } else
                item = newArguments->at(j++);
            arguments->initializeAt(vm, i, item);
        }
        placeholderCount += innerPlaceholderCount;
    } else if (!innerArguments)
        arguments = newArguments;
    else {
        arguments = PyTuple::create(globalObject, innerArguments->length() + newCount);
        for (unsigned i = 0; i < innerArguments->length(); ++i)
            arguments->initializeAt(vm, i, innerArguments->at(i));
        for (unsigned i = 0; i < newCount; ++i)
            arguments->initializeAt(vm, innerArguments->length() + i, newArguments->at(i));
        placeholderCount += innerPlaceholderCount;
    }

    PyDict* keywords = PyDict::create(globalObject);
    if (innerKeywords)
        keywords->copyFrom(globalObject, *innerKeywords);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        keywords->set(globalObject, args.keywordNameAsGiven(i), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }

    auto* object = PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<Partial>());
    auto& state = object->state<Partial>();
    state.function.set(vm, object, function);
    state.arguments.set(vm, object, arguments);
    state.keywords.set(vm, object, keywords);
    state.placeholderCount = placeholderCount;
    return JSValue::encode(object);
}

PYTHON_NATIVE(partialCall)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<Partial>(args[0]);
    unsigned given = args.size() - 1;
    if (given < state.placeholderCount)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("missing positional arguments in 'partial' call; expected at least "_s, state.placeholderCount, ", got "_s, given)));
    JSValue placeholder = functoolsModuleState(globalObject).placeholder.get();
    JSValue function = state.function.get();
    PyTuple* own = state.arguments.get();
    MarkedArgumentBuffer arguments;
    unsigned next = 1;
    for (auto& item : own->span())
        arguments.append(state.placeholderCount && item.get() == placeholder ? args[next++] : item.get());
    for (; next < args.size(); ++next)
        arguments.append(args[next]);
    if (!state.keywords->size()) {
        for (unsigned i = 0; i < args.keywordCount(); ++i)
            arguments.append(args.keywordValue(i));
        RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, function, arguments, args.keywordNames())));
    }
    // A copy, since what is called can change what it is given.
    PyDict* keywords = PyDict::create(globalObject);
    keywords->copyFrom(globalObject, *state.keywords.get());
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        keywords->set(globalObject, args.keywordNameAsGiven(i), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywordDict(globalObject, function, arguments, keywords)));
}

PYTHON_NATIVE(partialGet)
{
    NATIVE_PROLOGUE();
    return bindLikeFunction(globalObject, scope, args);
}

PYTHON_NATIVE(partialRepr)
{
    NATIVE_PROLOGUE();
    ReprGuard guard(globalObject, args[0].asCell());
    if (guard.isRecursive())
        return JSValue::encode(jsNontrivialString(vm, "..."_s));
    auto& state = stateOf<Partial>(args[0]);
    // What there is now, in case showing some of it changes it.
    JSValue function = state.function.get();
    PyTuple* arguments = state.arguments.get();
    PyDict* keywords = state.keywords.get();
    // In the order in which CPython shows them, which is not that in which they are written: the function is shown last.
    Vector<String> list;
    for (auto& argument : arguments->span()) {
        String shown = repr(globalObject, argument.get());
        RETURN_IF_EXCEPTION(scope, { });
        list.append(concatenate(", "_s, shown));
    }
    MarkedArgumentBuffer pairs;
    keywords->forEach(globalObject, [&] (JSValue key, JSValue value) {
        pairs.append(key);
        pairs.append(value);
        return true;
    });
    for (size_t i = 0; i < pairs.size(); i += 2) {
        String key = str(globalObject, pairs.at(i));
        RETURN_IF_EXCEPTION(scope, { });
        String shown = repr(globalObject, pairs.at(i + 1));
        RETURN_IF_EXCEPTION(scope, { });
        list.append(concatenate(", "_s, key, '=', shown));
    }
    JSValue type = typeOf(globalObject, args[0])->object();
    JSValue moduleName = getAttribute(globalObject, type, names.dunder_module);
    RETURN_IF_EXCEPTION(scope, { });
    String module = str(globalObject, moduleName);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue qualifiedName = getAttribute(globalObject, type, names.dunder_qualname);
    RETURN_IF_EXCEPTION(scope, { });
    String name = str(globalObject, qualifiedName);
    RETURN_IF_EXCEPTION(scope, { });
    String shownFunction = repr(globalObject, function);
    RETURN_IF_EXCEPTION(scope, { });
    TextBuilder builder;
    builder.append(module, '.', name, '(', shownFunction);
    for (auto& item : list)
        builder.append(item);
    builder.append(')');
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, builder.tryFinish())));
}

// There is no saying in what __reduce__() gives what keywords a class is to be called with, so it is made of the function alone and then told everything by __setstate__().
PYTHON_NATIVE(partialReduce)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<Partial>(args[0]);
    JSValue dict = jsUndefined();
    if (hasInstanceDictYet(args[0])) {
        dict = getInstanceDict(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue function = state.function.get();
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, args[0])->object(), PyTuple::create(globalObject, { function }), PyTuple::create(globalObject, { function, state.arguments.get(), state.keywords.get(), dict }) }));
}

PYTHON_NATIVE(partialSetState)
{
    NATIVE_PROLOGUE();
    auto invalid = [&] { return JSValue::encode(raiseTypeError(globalObject, scope, "invalid partial state"_s)); };
    if (!isTuple(args[1]) || asTuple(args[1])->length() != 4)
        return invalid();
    PyTuple* given = asTuple(args[1]);
    JSValue function = given->at(0);
    JSValue argumentsValue = given->at(1);
    JSValue keywordsValue = given->at(2);
    JSValue dict = given->at(3);
    if (!isCallable(globalObject, function) || !isTuple(argumentsValue) || (!isNone(keywordsValue) && !isDict(keywordsValue)) || (!isNone(dict) && !isDict(dict)))
        return invalid();
    JSValue placeholder = functoolsModuleState(globalObject).placeholder.get();
    PyTuple* arguments = asTuple(argumentsValue);
    if (arguments->length() && arguments->at(arguments->length() - 1) == placeholder)
        return JSValue::encode(raiseTypeError(globalObject, scope, "trailing Placeholders are not allowed"_s));
    unsigned placeholderCount = countPlaceholders(arguments, placeholder);
    // What is of a class derived from tuple or from dict is not kept, but a tuple or a dict of what is in it.
    if (!isExactly(globalObject, arguments, BuiltinType::Tuple)) {
        PyTuple* copy = PyTuple::create(globalObject, arguments->length());
        for (unsigned i = 0; i < arguments->length(); ++i)
            copy->initializeAt(vm, i, arguments->at(i));
        arguments = copy;
    }
    PyDict* keywords;
    if (isNone(keywordsValue))
        keywords = PyDict::create(globalObject);
    else if (!isExactly(globalObject, keywordsValue, BuiltinType::Dict)) {
        keywords = PyDict::create(globalObject);
        keywords->copyFrom(globalObject, *asDict(keywordsValue));
        RETURN_IF_EXCEPTION(scope, { });
    } else
        keywords = asDict(keywordsValue);
    setInstanceDict(globalObject, args[0], isNone(dict) ? JSValue(PyDict::create(globalObject)) : dict);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = uncheckedDowncast<PyStateObject>(args[0].asCell());
    auto& state = object->state<Partial>();
    state.function.set(vm, object, function);
    state.arguments.set(vm, object, arguments);
    state.keywords.set(vm, object, keywords);
    state.placeholderCount = placeholderCount;
    RETURN_NONE();
}

// ---- cmp_to_key

namespace {

struct KeyObject final : NativeState {
    PYTHON_NATIVE_STATE(KeyObject);
    WriteBarrier<Unknown> compare;
    // Empty in the one that cmp_to_key() gives, which is only for making the others.
    WriteBarrier<Unknown> object;
};

template<typename Visitor>
void KeyObject::visit(Visitor& visitor)
{
    visitor.append(compare);
    visitor.append(object);
}

PyStateObject* newKeyObject(JSGlobalObject* globalObject, PyType* type, JSValue compare, JSValue object)
{
    VM& vm = globalObject->vm();
    auto* result = PyStateObject::create(vm, type->instanceStructure(), makeUnique<KeyObject>());
    result->state<KeyObject>().compare.set(vm, result, compare);
    if (object)
        result->state<KeyObject>().object.set(vm, result, object);
    return result;
}

} // anonymous namespace

// K(obj)
PYTHON_NATIVE(keyObjectCall)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newKeyObject(globalObject, typeOf(globalObject, args[0]), stateOf<KeyObject>(args[0]).compare.get(), args.at(1)));
}

PYTHON_NATIVE(keyObjectCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (typeOf(globalObject, args[1]) != typeOf(globalObject, args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "other argument must be K instance"_s));
    auto& left = stateOf<KeyObject>(args[0]);
    auto& right = stateOf<KeyObject>(args[1]);
    if (!left.object || !right.object)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, "object"_s));
    // What the function makes of them is less than, equal to or more than nought.
    JSValue result = call(globalObject, left.compare.get(), left.object.get(), right.object.get());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(compare(globalObject, op, result, jsNumber(0))));
}

// cmp_to_key(mycmp)
PYTHON_NATIVE(functoolsCmpToKey)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newKeyObject(globalObject, functoolsModuleState(globalObject).keyType.get(), args.at(0), JSValue()));
}

// ---- reduce(function, iterable, /, initial=<none>)

PYTHON_NATIVE(functoolsReduce)
{
    NATIVE_PROLOGUE();
    JSValue function = args[0];
    JSValue result = args.at(2);
    JSValue iterator = getIterator(globalObject, args[1]);
    if (scope.exception()) [[unlikely]] {
        if (catchException(globalObject, BuiltinType::TypeError))
            return JSValue::encode(raiseTypeError(globalObject, scope, "reduce() arg 2 must support iteration"_s));
        return { };
    }
    for (;;) {
        JSValue next = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!next)
            break;
        if (!result) {
            result = next;
            continue;
        }
        result = call(globalObject, function, result, next);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!result)
        return JSValue::encode(raiseTypeError(globalObject, scope, "reduce() of empty iterable with no initial value"_s));
    return JSValue::encode(result);
}

// ---- What lru_cache makes

namespace {

// One thing that is kept. They are in a ring, from the one that was wanted longest ago to the one that was wanted last, with the cache itself between those two. In CPython what is in the ring is there by a pointer that does
// not count, and each is counted once besides for being in it. Here to be in it is to be kept.
struct CacheLink final : NativeState {
    PYTHON_NATIVE_STATE(CacheLink);
    // Null for the cache itself.
    WriteBarrier<PyStateObject> previous;
    WriteBarrier<PyStateObject> next;
    uint32_t hash { 0 };
    WriteBarrier<Unknown> key;
    WriteBarrier<Unknown> result;
};

template<typename Visitor>
void CacheLink::visit(Visitor& visitor)
{
    visitor.append(previous);
    visitor.append(next);
    visitor.append(key);
    visitor.append(result);
}

struct Cache final : NativeState {
    PYTHON_NATIVE_STATE(Cache);
    enum class Bound : uint8_t { Uncached, Infinite, Bounded };
    Bound bound { Bound::Uncached };
    bool isTyped { false };
    // The ends of the ring. Null if there is nothing in it.
    WriteBarrier<PyStateObject> oldest;
    WriteBarrier<PyStateObject> newest;
    // No program ever sees this.
    WriteBarrier<PyDict> cache;
    WriteBarrier<Unknown> function;
    WriteBarrier<Unknown> infoType;
    int64_t hits { 0 };
    int64_t misses { 0 };
    int64_t maximumSize { 0 };
};

template<typename Visitor>
void Cache::visit(Visitor& visitor)
{
    visitor.append(oldest);
    visitor.append(newest);
    visitor.append(cache);
    visitor.append(function);
    visitor.append(infoType);
}

CacheLink& linkOf(PyStateObject* object) { return object->state<CacheLink>(); }

// lru_cache_extract_link()
void extractLink(VM& vm, PyStateObject* owner, Cache& cache, PyStateObject* link)
{
    PyStateObject* previous = linkOf(link).previous.get();
    PyStateObject* next = linkOf(link).next.get();
    if (previous)
        linkOf(previous).next.setMayBeNull(vm, previous, next);
    else
        cache.oldest.setMayBeNull(vm, owner, next);
    if (next)
        linkOf(next).previous.setMayBeNull(vm, next, previous);
    else
        cache.newest.setMayBeNull(vm, owner, previous);
    linkOf(link).previous.clear();
    linkOf(link).next.clear();
}

// lru_cache_append_link(): as the one that was wanted last
void appendLink(VM& vm, PyStateObject* owner, Cache& cache, PyStateObject* link)
{
    PyStateObject* last = cache.newest.get();
    linkOf(link).previous.setMayBeNull(vm, link, last);
    linkOf(link).next.clear();
    if (last)
        linkOf(last).next.set(vm, last, link);
    else
        cache.oldest.set(vm, owner, link);
    cache.newest.set(vm, owner, link);
}

// lru_cache_prepend_link(): as the one that was wanted longest ago
void prependLink(VM& vm, PyStateObject* owner, Cache& cache, PyStateObject* link)
{
    PyStateObject* first = cache.oldest.get();
    linkOf(link).next.setMayBeNull(vm, link, first);
    linkOf(link).previous.clear();
    if (first)
        linkOf(first).previous.set(vm, first, link);
    else
        cache.newest.set(vm, owner, link);
    cache.oldest.set(vm, owner, link);
}

// lru_cache_make_key(): what stands in the cache for what it was called with
JSValue makeKey(JSGlobalObject* globalObject, const NativeArguments& args, bool isTyped)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    unsigned count = args.size() - 1;
    unsigned keywordCount = args.keywordCount();
    if (!isTyped && !keywordCount && count == 1) {
        // A str or an int stands for itself, to save room.
        PyType* type = typeOf(globalObject, args[1]);
        if (type == realm->typeStr() || type == realm->typeInt())
            return args[1];
    }
    unsigned size = count + (keywordCount ? keywordCount * 2 + 1 : 0) + (isTyped ? count + keywordCount : 0);
    PyTuple* key = PyTuple::create(globalObject, size);
    unsigned at = 0;
    for (unsigned i = 0; i < count; ++i)
        key->initializeAt(vm, at++, args[i + 1]);
    if (keywordCount) {
        key->initializeAt(vm, at++, functoolsModuleState(globalObject).keywordMark.get());
        for (unsigned i = 0; i < keywordCount; ++i) {
            key->initializeAt(vm, at++, args.keywordNameAsGiven(i));
            key->initializeAt(vm, at++, args.keywordValue(i));
        }
    }
    if (isTyped) {
        for (unsigned i = 0; i < count; ++i)
            key->initializeAt(vm, at++, typeOf(globalObject, args[i + 1])->object());
        for (unsigned i = 0; i < keywordCount; ++i)
            key->initializeAt(vm, at++, typeOf(globalObject, args.keywordValue(i))->object());
    }
    return key;
}

JSValue callCached(JSGlobalObject* globalObject, Cache& cache, const NativeArguments& args)
{
    return callWithKeywords(globalObject, cache.function.get(), args.allFrom(1), args.keywordNames());
}

// What the cache has for the key, whose hash is known so that it is not asked for it again. Empty if it has nothing, or it raised.
JSValue lookUp(JSGlobalObject* globalObject, Cache& cache, JSValue key, uint32_t hash)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyHashTable& table = cache.cache->ownTable();
    int entry = table.find(globalObject, key, hash);
    RETURN_IF_EXCEPTION(scope, { });
    return entry == PyHashTable::notFound ? JSValue() : table.valueAt(entry);
}

// bounded_lru_cache_update_lock_held(), when what was called has given something
JSValue updateBounded(JSGlobalObject* globalObject, PyStateObject* owner, Cache& cache, JSValue result, JSValue key, uint32_t hash)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyHashTable& table = cache.cache->ownTable();
    JSValue found = lookUp(globalObject, cache, key, hash);
    RETURN_IF_EXCEPTION(scope, { });
    // It was put there while the function was being called, and there is no more to do.
    if (found)
        return result;

    if (static_cast<int64_t>(table.size()) < cache.maximumSize || !cache.oldest) {
        auto* link = PyStateObject::create(vm, functoolsModuleState(globalObject).linkType->instanceStructure(), makeUnique<CacheLink>());
        linkOf(link).hash = hash;
        linkOf(link).key.set(vm, link, key);
        linkOf(link).result.set(vm, link, result);
        table.addWithHash(globalObject, key, hash, link);
        RETURN_IF_EXCEPTION(scope, { });
        appendLink(vm, owner, cache, link);
        return result;
    }

    // It is full. The one that was wanted longest ago is taken out, and used again for this.
    PyStateObject* link = cache.oldest.get();
    extractLink(vm, owner, cache, link);
    int entry = table.find(globalObject, linkOf(link).key.get(), linkOf(link).hash);
    if (scope.exception()) [[unlikely]] {
        prependLink(vm, owner, cache, link);
        return { };
    }
    // What was called has taken it out already. It is left out of the ring.
    if (entry == PyHashTable::notFound)
        return result;
    table.removeEntry(vm, entry);
    linkOf(link).hash = hash;
    linkOf(link).key.set(vm, link, key);
    linkOf(link).result.set(vm, link, result);
    // It is not in the ring until it is in the dict, since putting it there can run something that goes round the ring.
    table.addWithHash(globalObject, key, hash, link);
    RETURN_IF_EXCEPTION(scope, { });
    appendLink(vm, owner, cache, link);
    return result;
}

} // anonymous namespace

// _lru_cache_wrapper(user_function, maxsize, typed, cache_info_type)
PYTHON_NATIVE(cacheNew)
{
    NATIVE_PROLOGUE();
    JSValue function = args.at(1);
    JSValue maximumSizeValue = args.at(2);
    bool isTyped = isTrue(globalObject, args.at(3));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isCallable(globalObject, function))
        return JSValue::encode(raiseTypeError(globalObject, scope, "the first argument must be callable"_s));
    Cache::Bound bound;
    int64_t maximumSize;
    if (isNone(maximumSizeValue)) {
        bound = Cache::Bound::Infinite;
        maximumSize = -1;
    } else if (classify(maximumSizeValue).isInt() || typeOf(globalObject, maximumSizeValue)->lookup(vm, names.dunder_index)) {
        auto size = toIndexOrOverflow(globalObject, maximumSizeValue);
        RETURN_IF_EXCEPTION(scope, { });
        maximumSize = std::max<int64_t>(*size, 0);
        bound = maximumSize ? Cache::Bound::Bounded : Cache::Bound::Uncached;
    } else
        return JSValue::encode(raiseTypeError(globalObject, scope, "maxsize should be integer or None"_s));
    auto* object = PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<Cache>());
    auto& state = object->state<Cache>();
    state.bound = bound;
    state.isTyped = isTyped;
    state.maximumSize = maximumSize;
    state.cache.set(vm, object, PyDict::create(globalObject));
    state.function.set(vm, object, function);
    state.infoType.set(vm, object, args.at(4));
    return JSValue::encode(object);
}

PYTHON_NATIVE(cacheCall)
{
    NATIVE_PROLOGUE();
    auto* owner = uncheckedDowncast<PyStateObject>(args[0].asCell());
    auto& cache = owner->state<Cache>();
    if (cache.bound == Cache::Bound::Uncached) {
        ++cache.misses;
        RELEASE_AND_RETURN(scope, JSValue::encode(callCached(globalObject, cache, args)));
    }
    JSValue key = makeKey(globalObject, args, cache.isTyped);
    int64_t fullHash = hash(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    uint32_t folded = PyHashTable::foldHash(fullHash);
    JSValue found = lookUp(globalObject, cache, key, folded);
    RETURN_IF_EXCEPTION(scope, { });
    if (cache.bound == Cache::Bound::Infinite) {
        if (found) {
            ++cache.hits;
            return JSValue::encode(found);
        }
        ++cache.misses;
        JSValue result = callCached(globalObject, cache, args);
        RETURN_IF_EXCEPTION(scope, { });
        cache.cache->ownTable().addWithHash(globalObject, key, folded, result);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(result);
    }
    if (found) {
        auto* link = uncheckedDowncast<PyStateObject>(found.asCell());
        extractLink(vm, owner, cache, link);
        appendLink(vm, owner, cache, link);
        ++cache.hits;
        return JSValue::encode(linkOf(link).result.get());
    }
    ++cache.misses;
    JSValue result = callCached(globalObject, cache, args);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(updateBounded(globalObject, owner, cache, result, key, folded)));
}

PYTHON_NATIVE(cacheGet)
{
    NATIVE_PROLOGUE();
    return bindLikeFunction(globalObject, scope, args);
}

PYTHON_NATIVE(cacheInfo)
{
    NATIVE_PROLOGUE();
    auto& cache = stateOf<Cache>(args[0]);
    MarkedArgumentBuffer arguments;
    arguments.append(intFromInt64(globalObject, cache.hits));
    arguments.append(intFromInt64(globalObject, cache.misses));
    arguments.append(cache.maximumSize == -1 ? jsUndefined() : intFromInt64(globalObject, cache.maximumSize));
    arguments.append(intFromUInt64(globalObject, cache.cache->size()));
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, cache.infoType.get(), arguments)));
}

PYTHON_NATIVE(cacheClear)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& cache = stateOf<Cache>(args[0]);
    cache.oldest.clear();
    cache.newest.clear();
    cache.hits = 0;
    cache.misses = 0;
    cache.cache->clear(globalObject);
    RETURN_NONE();
}

// It is pickled by what it is called, as a function is.
PYTHON_NATIVE(cacheReduce)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(getAttribute(globalObject, args[0], names.dunder_qualname)));
}

// __copy__() and __deepcopy__(): a copy of it is itself, as it is of a function.
PYTHON_NATIVE(cacheCopy)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, unpack<bool>(callFrame, 0) ? "__deepcopy__"_s : "__copy__"_s))
        return { };
    return JSValue::encode(args[0]);
}

// ---- The module

JSObject* createFunctoolsModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto& state = functoolsModuleState(globalObject);
    if (!state.partialType) {
        auto make = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name, unsigned flags = 0) {
            PyType* type = createBuiltinType(globalObject, name, realm->typeObject(), PyType::Layout::Native, flags);
            type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
            slot.set(vm, realm, type);
            return type;
        };
        state.keywordMark.set(vm, realm, PyInstance::create(vm, realm->structureFor(BuiltinType::Object)));

        PyType* placeholderType = make(state.placeholderType, "functools._PlaceholderType"_s);
        addMethods(globalObject, placeholderType, {
            { "__new__"_s, placeholderNew, Kind::New, 0, "($type, /, *args, **kwargs)"_s, Arguments::AreNotChecked },
            { "__repr__"_s, placeholderName, Kind::Wrapper },
            { "__reduce__"_s, placeholderName },
        });
        state.placeholder.set(vm, realm, PyStateObject::create(vm, placeholderType->instanceStructure(), makeUnique<Placeholder>()));

        PyType* partial = make(state.partialType, "functools.partial"_s, PyType::IsBaseType);
        addMethods(globalObject, partial, {
            { "__new__"_s, partialNew, Kind::New, 0, "($type, /, *args, **kwargs)"_s, Arguments::AreNotChecked },
            { "__call__"_s, partialCall, Kind::Wrapper, 0, "($self, /, *args, **kwargs)"_s, Arguments::AreNotChecked },
            { "__get__"_s, partialGet, Kind::Wrapper, 0, { }, Arguments::AreNotChecked },
            { "__repr__"_s, partialRepr, Kind::Wrapper },
            { "__reduce__"_s, partialReduce },
            { "__setstate__"_s, partialSetState },
        });
        addGenericGetAttribute(globalObject, partial);
        addGenericSetAttribute(globalObject, partial);
        addClassGetItemIfGeneric(globalObject, partial);
        addMember(globalObject, partial, "func"_s, [] (JSGlobalObject*, JSValue self) { return stateOf<Partial>(self).function.get(); });
        addMember(globalObject, partial, "args"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Partial>(self).arguments.get(); });
        addMember(globalObject, partial, "keywords"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Partial>(self).keywords.get(); });
        // See what is said of this in PythonOperatorModule.cpp.
        addMember(globalObject, partial, "__vectorcalloffset__"_s, [] (JSGlobalObject*, JSValue) -> JSValue { return jsNumber(0); });
        addGetSet(globalObject, partial, "__dict__"_s, getInstanceDict, setInstanceDictOfBuiltin);

        PyType* cache = make(state.cacheType, "functools._lru_cache_wrapper"_s);
        addMethods(globalObject, cache, {
            { "__new__"_s, cacheNew, Kind::New, 0, "lru_cache(user_function, maxsize, typed, cache_info_type)"_s, Arguments::AreThoseOfTheClass },
            { "__call__"_s, cacheCall, Kind::Wrapper, 0, "($self, /, *args, **kwargs)"_s, Arguments::AreNotChecked },
            { "__get__"_s, cacheGet, Kind::Wrapper, 0, { }, Arguments::AreNotChecked },
            { "cache_info"_s, cacheInfo },
            { "cache_clear"_s, cacheClear },
            { "__reduce__"_s, cacheReduce },
            { "__copy__"_s, cacheCopy, Kind::Method, pack(false), "($self, /, *args)"_s, Arguments::AreNotChecked },
            { "__deepcopy__"_s, cacheCopy, Kind::Method, pack(true), "($self, /, *args)"_s, Arguments::AreNotChecked },
        });
        addGetSet(globalObject, cache, "__dict__"_s, getInstanceDict, setInstanceDictOfBuiltin);

        PyType* key = make(state.keyType, "functools.KeyWrapper"_s);
        addMethods(globalObject, key, { { "__call__"_s, keyObjectCall, Kind::Wrapper, 0, "K(obj)"_s, Arguments::AreThoseOfTheClass } });
        addComparisons(globalObject, key, keyObjectCompare);
        addGenericGetAttribute(globalObject, key);
        key->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
        addMember(globalObject, key, "obj"_s, [] (JSGlobalObject*, JSValue self) -> JSValue {
            JSValue object = stateOf<KeyObject>(self).object.get();
            return object ? object : jsUndefined();
        }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
            auto* object = uncheckedDowncast<PyStateObject>(self.asCell());
            if (value)
                object->state<KeyObject>().object.set(globalObject->vm(), object, value);
            else
                object->state<KeyObject>().object.clear();
        });
        addGetSet(globalObject, key, "__text_signature__"_s, [] (JSGlobalObject* globalObject, JSValue) -> JSValue { return jsNontrivialString(globalObject->vm(), "(obj)"_s); });

        make(state.linkType, "functools._lru_list_elem"_s);
    }

    JSObject* module = newBuiltinModule(globalObject, "_functools"_s);
    addFunction(globalObject, module, "reduce"_s, functoolsReduce);
    addFunction(globalObject, module, "cmp_to_key"_s, functoolsCmpToKey);
    module->putDirect(vm, Identifier::fromString(vm, "_PlaceholderType"_s), state.placeholderType->object());
    module->putDirect(vm, Identifier::fromString(vm, "Placeholder"_s), state.placeholder.get());
    module->putDirect(vm, Identifier::fromString(vm, "partial"_s), state.partialType->object());
    module->putDirect(vm, Identifier::fromString(vm, "_lru_cache_wrapper"_s), state.cacheType->object());
    return module;
}

} } // namespace JSC::Python
