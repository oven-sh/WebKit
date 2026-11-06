/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyTypes.h"

namespace JSC {
namespace Python {

// types.GenericAlias, as list[int], and typing.Union (which is types.UnionType), as int | str.
// (Objects/genericaliasobject.c and Objects/unionobject.c in CPython are the reference.)
//
// Both are records.

namespace {

// `number` says whether the alias is starred: *tuple[int].
enum AliasField : unsigned {
    AliasOrigin,
    AliasArgs, // A tuple.
    AliasParameters, // A tuple. Empty until it is asked for.
    AliasFieldCount,
};

enum UnionField : unsigned {
    UnionArgs, // A tuple, of all of them.
    UnionHashableArgs, // A frozenset.
    UnionUnhashableArgs, // A tuple, or empty.
    UnionParameters, // A tuple. Empty until it is asked for.
    UnionFieldCount,
};

}

static ALWAYS_INLINE PyRecord* aliasRecord(JSValue value)
{
    ASSERT(PyRecord::is(value));
    return uncheckedDowncast<PyRecord>(value.asCell());
}

static ALWAYS_INLINE PyRecord* tryAlias(JSGlobalObject* globalObject, JSValue value) { return PyRecord::tryFrom(value, runtime(globalObject).genericAliasType()); }

static ALWAYS_INLINE PyRecord* tryUnion(JSGlobalObject* globalObject, JSValue value)
{
    if (!PyRecord::is(value))
        return nullptr;
    PyRecord* record = aliasRecord(value);
    return record->type() == runtime(globalObject).unionType() ? record : nullptr;
}

static ALWAYS_INLINE PyTuple* aliasArgs(PyRecord* alias) { return PyTuple::from(alias->get(AliasArgs)); }
static ALWAYS_INLINE bool aliasIsStarred(PyRecord* alias) { return alias->number; }
static ALWAYS_INLINE PyTuple* unionArgs(PyRecord* record) { return PyTuple::from(record->get(UnionArgs)); }

// builtins.name
static JSValue aliasGetBuiltin(JSGlobalObject* globalObject, ASCIILiteral name)
{
    return currentBuiltin(globalObject, internString(getVM(globalObject), name));
}

// "module.qualname" of the type of the value, without the module if that is builtins or __main__.
static String aliasFullTypeName(JSGlobalObject* globalObject, JSValue value)
{
    String name = qualifiedTypeName(globalObject, typeOf(globalObject, value));
    return name.startsWith("__main__."_s) ? name.substring(9) : name;
}

// repr(value). Null: an exception.
static String aliasReprText(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = repr(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return text.isNull() ? emptyString() : text;
}

static String aliasStrText(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = str(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return text.isNull() ? emptyString() : text;
}

// How a type is shown inside an alias or a union: a class by its name, most other things by their repr().
// False: an exception.
static bool aliasAppendTypeRepr(JSGlobalObject* globalObject, StringBuilder& out, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value == rt.ellipsis()) {
        out.append("..."_s);
        return true;
    }
    if (value == JSValue(rt.noneType())) {
        out.append("None"_s);
        return true;
    }

    auto appendRepr = [&] {
        String text = aliasReprText(globalObject, value);
        if (text.isNull())
            return false;
        out.append(text);
        return true;
    };

    // What looks like a generic alias knows how to show itself.
    bool looksLikeAlias = hasAttr(globalObject, value, internString(vm, "__origin__"_s));
    RETURN_IF_EXCEPTION(scope, false);
    if (looksLikeAlias) {
        looksLikeAlias = hasAttr(globalObject, value, internString(vm, "__args__"_s));
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (looksLikeAlias)
        RELEASE_AND_RETURN(scope, appendRepr());

    JSValue qualname = getAttrIfPresent(globalObject, value, rt.names.dQualname());
    RETURN_IF_EXCEPTION(scope, false);
    if (!qualname)
        RELEASE_AND_RETURN(scope, appendRepr());
    JSValue module = getAttrIfPresent(globalObject, value, rt.names.dModule());
    RETURN_IF_EXCEPTION(scope, false);
    if (!module || isNone(module))
        RELEASE_AND_RETURN(scope, appendRepr());

    String qualnameText = aliasStrText(globalObject, qualname);
    RETURN_IF_EXCEPTION(scope, false);
    String moduleText = aliasStrText(globalObject, module);
    RETURN_IF_EXCEPTION(scope, false);
    if (isStr(module) && moduleText == "builtins"_s)
        out.append(qualnameText);
    else
        out.append(moduleText, '.', qualnameText);
    return true;
}

// ---- Type variables: finding them, and putting types in their place ----

static std::optional<unsigned> aliasIndexIn(PyTuple* tuple, JSValue item)
{
    for (unsigned i = 0; i < tuple->length(); ++i) {
        if (tuple->at(i) == item)
            return i;
    }
    return std::nullopt;
}

static void aliasAppendAll(MarkedArgumentBuffer& buffer, PyTuple* tuple)
{
    for (unsigned i = 0; i < tuple->length(); ++i)
        buffer.append(tuple->at(i));
}

// The type variables in the arguments (a tuple or a list), each once, in the order they are met. Null: an exception.
static PyTuple* aliasMakeParameters(JSGlobalObject* globalObject, JSValue argsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!vm.isSafeToRecurseSoft()) {
        raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded in __parameter__ calculation"_s);
        return nullptr;
    }
    PyTuple* args = PyTuple::tryFrom(argsValue);
    if (!args) {
        args = tupleFromList(globalObject, asList(argsValue));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    MarkedArgumentBuffer parameters;
    auto add = [&](JSValue item) {
        for (unsigned i = 0; i < parameters.size(); ++i) {
            if (parameters.at(i) == item)
                return;
        }
        parameters.append(item);
    };
    JSString* substName = internString(vm, "__typing_subst__"_s);
    JSString* parametersName = internString(vm, "__parameters__"_s);
    for (unsigned i = 0; i < args->length(); ++i) {
        JSValue item = args->at(i);
        // The __parameters__ of a class is a descriptor, or about the class itself.
        if (PyType::is(item))
            continue;
        bool isVariable = hasAttr(globalObject, item, substName);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (isVariable) {
            add(item);
            continue;
        }
        JSValue inner = getAttrIfPresent(globalObject, item, parametersName);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!inner && (PyTuple::is(item) || isList(item))) {
            inner = aliasMakeParameters(globalObject, item);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        if (auto* tuple = inner ? PyTuple::tryFrom(inner) : nullptr) {
            for (unsigned j = 0; j < tuple->length(); ++j)
                add(tuple->at(j));
        }
    }
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, spanOf(parameters)));
}

// If the object has type variables of its own, as list[T] has, object[...] with what takes the place of each.
// `items` goes with `parameters`.
static JSValue aliasSubstituteWithin(JSGlobalObject* globalObject, JSValue object, PyTuple* parameters, std::span<const JSValue> items)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue inner = getAttrIfPresent(globalObject, object, internString(vm, "__parameters__"_s));
    RETURN_IF_EXCEPTION(scope, { });
    auto* innerParameters = inner ? PyTuple::tryFrom(inner) : nullptr;
    if (!innerParameters || !innerParameters->length())
        return object;

    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < innerParameters->length(); ++i) {
        JSValue argument = innerParameters->at(i);
        if (auto index = aliasIndexIn(parameters, argument)) {
            argument = items[*index];
            // A TypeVarTuple stands for any number of types.
            if (typeOf(globalObject, parameters->at(*index))->lookup(Special::Iter) && PyTuple::is(argument)) {
                aliasAppendAll(arguments, PyTuple::from(argument));
                continue;
            }
        }
        arguments.append(argument);
    }
    RELEASE_AND_RETURN(scope, getItem(globalObject, object, PyTuple::create(globalObject, spanOf(arguments))));
}

static bool aliasIsUnpackedTypeVarTuple(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyType::is(argument))
        return false;
    JSValue flag = getAttrIfPresent(globalObject, argument, internString(vm, "__typing_is_unpacked_typevartuple__"_s));
    RETURN_IF_EXCEPTION(scope, false);
    if (!flag)
        return false;
    RELEASE_AND_RETURN(scope, isTrue(globalObject, flag));
}

// For *tuple[int, str]: (int, str). Empty, with no exception, for anything else.
static JSValue aliasUnpackedTupleArguments(JSGlobalObject* globalObject, JSValue argument)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyRecord* alias = tryAlias(globalObject, argument); alias && aliasIsStarred(alias) && alias->get(AliasOrigin) == JSValue(rt.tupleType()))
        return alias->get(AliasArgs);
    JSValue result = getAttrIfPresent(globalObject, argument, internString(vm, "__typing_unpacked_tuple_args__"_s));
    RETURN_IF_EXCEPTION(scope, { });
    return result && !isNone(result) ? result : JSValue();
}

// The subscript as a tuple, with the unpacked tuples of known length in it replaced by their items. Null: an exception.
static PyTuple* aliasUnpackArguments(JSGlobalObject* globalObject, JSValue item)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue single[] = { item };
    std::span<const JSValue> items = single;
    if (auto* tuple = PyTuple::tryFrom(item))
        items = tuple->span();

    MarkedArgumentBuffer result;
    for (JSValue element : items) {
        if (!PyType::is(element)) {
            JSValue inner = aliasUnpackedTupleArguments(globalObject, element);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (auto* tuple = inner ? PyTuple::tryFrom(inner) : nullptr; tuple && !(tuple->length() && tuple->at(tuple->length() - 1) == rt.ellipsis())) {
                aliasAppendAll(result, tuple);
                continue;
            }
        }
        result.append(element);
    }
    ensureStillAliveHere(item); // `items` may point into it.
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, spanOf(result)));
}

// self[item]: the arguments (a tuple or a list) with each of the parameters replaced by the part of `item` that goes with it.
// Null: an exception.
static PyTuple* aliasSubstituteParameters(JSGlobalObject* globalObject, JSValue self, JSValue argsValue, PyTuple* parameters, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!vm.isSafeToRecurseSoft()) {
        raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded"_s);
        return nullptr;
    }
    unsigned parameterCount = parameters->length();
    if (!parameterCount) {
        String text = aliasReprText(globalObject, self);
        RETURN_IF_EXCEPTION(scope, nullptr);
        raiseTypeError(globalObject, scope, text, " is not a generic class"_s);
        return nullptr;
    }

    item = aliasUnpackArguments(globalObject, item);
    RETURN_IF_EXCEPTION(scope, nullptr);
    // A ParamSpec or a TypeVarTuple has a say in how the subscript is divided among the parameters.
    JSString* prepareName = internString(vm, "__typing_prepare_subst__"_s);
    for (unsigned i = 0; i < parameterCount; ++i) {
        JSValue prepare = getAttrIfPresent(globalObject, parameters->at(i), prepareName);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!prepare || isNone(prepare))
            continue;
        item = call(globalObject, prepare, self, PyTuple::is(item) ? item : JSValue(PyTuple::create(globalObject, { item })));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    JSValue single[] = { item };
    std::span<const JSValue> items = single;
    if (auto* tuple = PyTuple::tryFrom(item))
        items = tuple->span();
    if (items.size() != parameterCount) {
        String text = aliasReprText(globalObject, self);
        RETURN_IF_EXCEPTION(scope, nullptr);
        raiseTypeError(globalObject, scope, "Too "_s, items.size() > parameterCount ? "many"_s : "few"_s, " arguments for "_s, text, "; actual "_s, items.size(), ", expected "_s, parameterCount);
        return nullptr;
    }

    PyTuple* args = PyTuple::tryFrom(argsValue);
    if (!args) {
        args = tupleFromList(globalObject, asList(argsValue));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    JSString* substName = internString(vm, "__typing_subst__"_s);
    MarkedArgumentBuffer result;
    for (unsigned i = 0; i < args->length(); ++i) {
        JSValue argument = args->at(i);
        if (PyType::is(argument)) {
            result.append(argument);
            continue;
        }
        // Callable[[T], int] and the like.
        if (PyTuple::is(argument) || isList(argument)) {
            JSValue inner = aliasSubstituteParameters(globalObject, self, argument, parameters, item);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (isList(argument)) {
                inner = listFromIterable(globalObject, inner);
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
            result.append(inner);
            continue;
        }
        bool unpack = aliasIsUnpackedTypeVarTuple(globalObject, argument);
        RETURN_IF_EXCEPTION(scope, nullptr);
        JSValue subst = getAttrIfPresent(globalObject, argument, substName);
        RETURN_IF_EXCEPTION(scope, nullptr);
        JSValue replacement;
        if (subst) {
            auto index = aliasIndexIn(parameters, argument);
            if (!index) {
                // It may have become a type variable since the parameters were worked out.
                String text = aliasReprText(globalObject, argument);
                RETURN_IF_EXCEPTION(scope, nullptr);
                raiseTypeError(globalObject, scope, "argument "_s, text, " with __typing_subst__ was not found in __parameters__"_s);
                return nullptr;
            }
            replacement = call(globalObject, subst, items[*index]);
        } else
            replacement = aliasSubstituteWithin(globalObject, argument, parameters, items);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!unpack) {
            result.append(replacement);
            continue;
        }
        auto* tuple = PyTuple::tryFrom(replacement);
        if (!tuple) {
            raiseTypeError(globalObject, scope, "expected __typing_subst__ of "_s, aliasFullTypeName(globalObject, argument), " objects to return a tuple, not "_s, aliasFullTypeName(globalObject, replacement));
            return nullptr;
        }
        aliasAppendAll(result, tuple);
    }
    ensureStillAliveHere(item); // `items` may point into it.
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, spanOf(result)));
}

// ---- GenericAlias ----

static PyRecord* aliasCreate(JSGlobalObject* globalObject, PyType* type, JSValue origin, JSValue arguments)
{
    VM& vm = getVM(globalObject);
    if (!PyTuple::is(arguments))
        arguments = PyTuple::create(globalObject, { arguments });
    PyRecord* alias = PyRecord::create(globalObject, type, AliasFieldCount);
    alias->set(vm, AliasOrigin, origin);
    alias->set(vm, AliasArgs, arguments);
    return alias;
}

JSValue makeGenericAlias(JSGlobalObject* globalObject, JSValue origin, JSValue arguments)
{
    return aliasCreate(globalObject, runtime(globalObject).genericAliasType(), origin, arguments);
}

static JSValue aliasNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "GenericAlias"_s, args)) || failed(scope, checkPositional(globalObject, "GenericAlias"_s, args, 2, 2)))
        return { };
    RELEASE_AND_RETURN(scope, aliasCreate(globalObject, PyType::from(cls), args[0], args[1]));
}

static JSValue aliasRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* alias = aliasRecord(self);
    PyTuple* args = aliasArgs(alias);

    StrBuilder out;
    if (aliasIsStarred(alias))
        out.append('*');
    aliasAppendTypeRepr(globalObject, out, alias->get(AliasOrigin));
    RETURN_IF_EXCEPTION(scope, { });
    out.append('[');
    for (unsigned i = 0; i < args->length(); ++i) {
        if (i)
            out.append(", "_s);
        JSValue argument = args->at(i);
        if (!isExactList(argument)) {
            aliasAppendTypeRepr(globalObject, out, argument);
            RETURN_IF_EXCEPTION(scope, { });
            continue;
        }
        // The types that go with a ParamSpec.
        JSArray* list = asList(argument);
        out.append('[');
        for (unsigned j = 0, count = listLength(list); j < count; ++j) {
            if (j)
                out.append(", "_s);
            // The __repr__ of an item may have shortened the list.
            if (j >= listLength(list))
                return raiseIndexError(globalObject, scope, "list index out of range"_s);
            aliasAppendTypeRepr(globalObject, out, listGet(list, j));
            RETURN_IF_EXCEPTION(scope, { });
        }
        out.append(']');
    }
    // tuple[()]
    if (!args->length())
        out.append("()"_s);
    out.append(']');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, out));
}

// Null: an exception.
static PyTuple* aliasParameters(JSGlobalObject* globalObject, PyRecord* record, unsigned argsField, unsigned parametersField)
{
    if (JSValue parameters = record->get(parametersField))
        return PyTuple::from(parameters);
    PyTuple* parameters = aliasMakeParameters(globalObject, record->get(argsField));
    if (parameters)
        record->set(getVM(globalObject), parametersField, parameters);
    return parameters;
}

static JSValue aliasGetItem(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* alias = aliasRecord(self);
    PyTuple* parameters = aliasParameters(globalObject, alias, AliasArgs, AliasParameters);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* arguments = aliasSubstituteParameters(globalObject, self, alias->get(AliasArgs), parameters, item);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* result = aliasCreate(globalObject, runtime(globalObject).genericAliasType(), alias->get(AliasOrigin), arguments);
    result->number = alias->number;
    return result;
}

static JSValue aliasHash(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* alias = aliasRecord(self);
    int64_t originHash = hash(globalObject, alias->get(AliasOrigin));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t argsHash = hash(globalObject, alias->get(AliasArgs));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t result = originHash ^ argsHash;
    RELEASE_AND_RETURN(scope, makeInt(globalObject, result == -1 ? static_cast<int64_t>(-2) : result));
}

// list[int]() makes a list, which is told, if it can be, what made it.
static JSValue aliasCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue object = call(globalObject, aliasRecord(self)->get(AliasOrigin), args);
    RETURN_IF_EXCEPTION(scope, { });
    setAttr(globalObject, object, internString(vm, "__orig_class__"_s), self);
    if (scope.exception()) [[unlikely]] {
        scope.release();
        if (!clearPendingExceptionIfMatches(globalObject, rt.AttributeError()) && !clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            return { };
    }
    return object;
}

// The attributes of the alias itself. All others are those of the origin...
static constexpr ASCIILiteral aliasOwnAttributes[] = {
    "__class__"_s,
    "__origin__"_s,
    "__args__"_s,
    "__unpacked__"_s,
    "__parameters__"_s,
    "__typing_unpacked_tuple_args__"_s,
    "__mro_entries__"_s,
    "__reduce_ex__"_s,
    "__reduce__"_s,
};

// ... but for these, which the alias must not seem to have because the origin does.
static constexpr ASCIILiteral aliasBlockedAttributes[] = {
    "__bases__"_s,
    "__copy__"_s,
    "__deepcopy__"_s,
};

static JSValue aliasGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!name)
        return { };
    String text = stringOf(name);
    if (text.startsWith("__"_s)) {
        for (auto blocked : aliasBlockedAttributes) {
            if (text == blocked)
                RELEASE_AND_RETURN(scope, genericGetAttr(globalObject, self, name));
        }
        for (auto own : aliasOwnAttributes) {
            if (text == own)
                RELEASE_AND_RETURN(scope, genericGetAttr(globalObject, self, name));
        }
    }
    RELEASE_AND_RETURN(scope, getAttr(globalObject, aliasRecord(self)->get(AliasOrigin), name));
}

static JSValue aliasEq(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* a = aliasRecord(self);
    PyRecord* b = tryAlias(globalObject, other);
    if (!b)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    if (aliasIsStarred(a) != aliasIsStarred(b))
        return jsBoolean(false);
    bool sameOrigin = isEqual(globalObject, a->get(AliasOrigin), b->get(AliasOrigin));
    RETURN_IF_EXCEPTION(scope, { });
    if (!sameOrigin)
        return jsBoolean(false);
    RELEASE_AND_RETURN(scope, richCompare(globalObject, a->get(AliasArgs), b->get(AliasArgs), CompareOperator::Eq));
}

static JSValue aliasNe(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    JSValue equal = aliasEq(globalObject, self, other);
    if (!equal || equal == runtime(globalObject).notImplemented())
        return equal;
    return jsBoolean(equal != jsBoolean(true));
}

static JSValue aliasOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return makeUnion(globalObject, self, other); }
static JSValue aliasROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return makeUnion(globalObject, other, self); }

static JSValue aliasMroEntries(JSGlobalObject* globalObject, JSValue self, JSValue)
{
    return PyTuple::create(globalObject, { aliasRecord(self)->get(AliasOrigin) });
}

static JSValue aliasInstanceCheck(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "isinstance() argument 2 cannot be a parameterized generic"_s);
}

static JSValue aliasSubclassCheck(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "issubclass() argument 2 cannot be a parameterized generic"_s);
}

static JSValue aliasIter(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* iterator = PyRecord::create(globalObject, runtime(globalObject).genericAliasIteratorType(), 1);
    iterator->set(getVM(globalObject), 0, self);
    return iterator;
}

static JSValue aliasReduce(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* alias = aliasRecord(self);
    if (!aliasIsStarred(alias))
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { JSValue(alias->type()), JSValue(PyTuple::create(globalObject, { alias->get(AliasOrigin), alias->get(AliasArgs) })) }));
    // next(iter(alias)) is the alias, starred.
    JSValue iterator = aliasIter(globalObject, makeGenericAlias(globalObject, alias->get(AliasOrigin), alias->get(AliasArgs)));
    JSValue next = aliasGetBuiltin(globalObject, "next"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { next, JSValue(PyTuple::create(globalObject, { iterator })) }));
}

static JSValue aliasDir(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // dir(origin)
    JSValue origin = aliasRecord(self)->get(AliasOrigin);
    JSValue method = lookupSpecial(globalObject, origin, Special::Dir);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return raiseTypeError(globalObject, scope, "object does not provide __dir__"_s);
    JSValue names = call(globalObject, method);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* result = listFromIterable(globalObject, names);
    RETURN_IF_EXCEPTION(scope, { });
    listSort(globalObject, result, { }, false);
    RETURN_IF_EXCEPTION(scope, { });

    for (auto own : aliasOwnAttributes) {
        JSString* name = internString(vm, own);
        bool isThere = contains(globalObject, result, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isThere) {
            listAppend(globalObject, result, name);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return result;
}

static JSValue aliasGetOrigin(JSGlobalObject*, JSValue self) { return aliasRecord(self)->get(AliasOrigin); }
static JSValue aliasGetArgs(JSGlobalObject*, JSValue self) { return aliasRecord(self)->get(AliasArgs); }
static JSValue aliasGetUnpacked(JSGlobalObject*, JSValue self) { return jsBoolean(aliasIsStarred(aliasRecord(self))); }
static JSValue aliasGetParameters(JSGlobalObject* globalObject, JSValue self) { return aliasParameters(globalObject, aliasRecord(self), AliasArgs, AliasParameters); }

static JSValue aliasGetUnpackedTupleArgs(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* alias = aliasRecord(self);
    if (aliasIsStarred(alias) && alias->get(AliasOrigin) == JSValue(runtime(globalObject).tupleType()))
        return alias->get(AliasArgs);
    return jsUndefined();
}

static constexpr NativeDef aliasMethods[] = {
    PY_NEW(aliasNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", aliasRepr, "($self, /)"),
    PY_SLOT("__hash__", aliasHash, "($self, /)"),
    PY_SLOT("__call__", aliasCall, "($self, *args, **kwargs)"),
    PY_SLOT("__getattribute__", aliasGetAttribute, "($self, name, /)"),
    PY_SLOT("__eq__", aliasEq, "($self, value, /)"),
    PY_SLOT("__ne__", aliasNe, "($self, value, /)"),
    PY_SLOT("__iter__", aliasIter, "($self, /)"),
    PY_SLOT("__or__", aliasOr, "($self, value, /)"),
    PY_SLOT("__ror__", aliasROr, "($self, value, /)"),
    PY_SLOT("__getitem__", aliasGetItem, "($self, key, /)"),
    PY_METHOD("__mro_entries__", aliasMroEntries, "($self, object, /)"),
    PY_METHOD("__instancecheck__", aliasInstanceCheck, "($self, object, /)"),
    PY_METHOD("__subclasscheck__", aliasSubclassCheck, "($self, object, /)"),
    PY_METHOD("__reduce__", aliasReduce, "($self, /)"),
    PY_METHOD("__dir__", aliasDir, "($self, /)"),
};

static constexpr NativeGetSet aliasGetSets[] = {
    PY_MEMBER("__origin__", aliasGetOrigin),
    PY_MEMBER("__args__", aliasGetArgs),
    PY_MEMBER("__unpacked__", aliasGetUnpacked),
    { "__parameters__"_s, aliasGetParameters },
    { "__typing_unpacked_tuple_args__"_s, aliasGetUnpackedTupleArgs },
};

// iter(tuple[int]) gives one item, *tuple[int]: that is how `*tuple[int]` in a subscript or a signature is evaluated.

static JSValue aliasIteratorIter(JSGlobalObject*, JSValue self) { return self; }

static JSValue aliasIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* iterator = aliasRecord(self);
    JSValue value = iterator->get(0);
    if (!value)
        return raiseStopIteration(globalObject, scope);
    iterator->set(vm, 0, { });
    PyRecord* alias = aliasRecord(value);
    PyRecord* starred = aliasCreate(globalObject, runtime(globalObject).genericAliasType(), alias->get(AliasOrigin), alias->get(AliasArgs));
    starred->number = true;
    return starred;
}

static JSValue aliasIteratorReduce(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iter = aliasGetBuiltin(globalObject, "iter"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = aliasRecord(self)->get(0);
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { iter, JSValue(PyTuple::create(globalObject, { value ? value : JSValue(runtime(globalObject).emptyTuple()) })) }));
}

static constexpr NativeDef aliasIteratorMethods[] = {
    PY_SLOT("__iter__", aliasIteratorIter, "($self, /)"),
    PY_SLOT("__next__", aliasIteratorNext, "($self, /)"),
    PY_METHOD("__reduce__", aliasIteratorReduce, "($self, /)"),
};

// ---- Union ----

// The members of the union: a tuple. Null: the value is not a union.
PyTuple* unionArguments(JSGlobalObject* globalObject, JSValue value)
{
    PyRecord* record = tryUnion(globalObject, value);
    return record ? unionArgs(record) : nullptr;
}

// What `|` makes a union of without asking the module typing.
static bool unionAccepts(JSGlobalObject* globalObject, JSValue value)
{
    if (isNone(value) || PyType::is(value))
        return true;
    Runtime& rt = runtime(globalObject);
    PyType* type = rt.typeOf(value);
    return type == rt.unionType() || type == rt.sentinelType() || type == rt.typeAliasTypeType() || type->isSubtypeOf(rt.genericAliasType());
}

namespace {

// Gathers the members of a union: those of the unions among them in their place, and none twice.
class UnionBuilder {
public:
    // `isChecked`: anything that typing takes for a type will do, and a str is a forward reference.
    UnionBuilder(JSGlobalObject* globalObject, bool isChecked)
        : m_globalObject(globalObject)
        , m_isChecked(isChecked)
    {
    }

    // False: an exception. So for the others.
    bool initialize()
    {
        m_args = newList(m_globalObject);
        if (!m_args)
            return false;
        m_hashable = PySet::create(m_globalObject);
        return true;
    }

    bool add(JSValue argument)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        Runtime& rt = runtime(m_globalObject);
        if (isNone(argument))
            argument = rt.noneType();
        else if (PyRecord* record = tryUnion(m_globalObject, argument))
            RELEASE_AND_RETURN(scope, addAll(unionArgs(record)));
        if (m_isChecked && !unionAccepts(m_globalObject, argument)) {
            argument = checkType(argument);
            RETURN_IF_EXCEPTION(scope, false);
            if (!argument)
                return false;
        }
        RELEASE_AND_RETURN(scope, addUnchecked(argument));
    }

    bool addAll(PyTuple* tuple)
    {
        auto scope = DECLARE_THROW_SCOPE(getVM(m_globalObject));
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (failed(scope, add(tuple->at(i))))
                return false;
        }
        return true;
    }

    // The union, or its only member. Empty: an exception.
    JSValue finish()
    {
        Runtime& rt = runtime(m_globalObject);
        VM& vm = rt.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        unsigned count = listLength(m_args);
        if (!count)
            RELEASE_AND_RETURN(scope, raiseTypeError(m_globalObject, scope, "Cannot take a Union of no types."_s));
        if (count == 1)
            return listGet(m_args, 0);

        PyTuple* args = tupleFromList(m_globalObject, m_args);
        RETURN_IF_EXCEPTION(scope, { });
        PySet* hashable = m_hashable->copy(m_globalObject, rt.frozensetType());
        RETURN_IF_EXCEPTION(scope, { });
        PyTuple* unhashable = nullptr;
        if (m_unhashable) {
            unhashable = tupleFromList(m_globalObject, m_unhashable);
            RETURN_IF_EXCEPTION(scope, { });
        }
        PyRecord* result = PyRecord::create(m_globalObject, rt.unionType(), UnionFieldCount);
        result->set(vm, UnionArgs, args);
        result->set(vm, UnionHashableArgs, hashable);
        if (unhashable)
            result->set(vm, UnionUnhashableArgs, unhashable);
        return result;
    }

private:
    // typing._type_check(argument, message)
    JSValue checkType(JSValue argument)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue typing = importModule(m_globalObject, "typing"_s);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue function = getAttr(m_globalObject, typing, internString(vm, "_type_check"_s));
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, call(m_globalObject, function, argument, makeStr(vm, "Union[arg, ...]: each arg must be a type."_s)));
    }

    bool addUnchecked(JSValue argument)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        int64_t argumentHash = hash(m_globalObject, argument);
        if (scope.exception()) {
            // It cannot be hashed, so it is looked for the slow way.
            takePendingException(m_globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            if (!m_unhashable) {
                m_unhashable = newList(m_globalObject);
                RETURN_IF_EXCEPTION(scope, false);
            } else {
                bool isThere = contains(m_globalObject, m_unhashable, argument);
                RETURN_IF_EXCEPTION(scope, false);
                if (isThere)
                    return true;
            }
            listAppend(m_globalObject, m_unhashable, argument);
            RETURN_IF_EXCEPTION(scope, false);
        } else {
            bool isNew = m_hashable->add(m_globalObject, argument, argumentHash);
            RETURN_IF_EXCEPTION(scope, false);
            if (!isNew)
                return true;
        }
        listAppend(m_globalObject, m_args, argument);
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }

    JSGlobalObject* m_globalObject;
    bool m_isChecked;
    JSArray* m_args { nullptr };
    PySet* m_hashable { nullptr };
    JSArray* m_unhashable { nullptr };
};

}

JSValue makeUnion(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!unionAccepts(globalObject, left) || !unionAccepts(globalObject, right))
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    UnionBuilder builder(globalObject, false);
    if (failed(scope, builder.initialize()) || failed(scope, builder.add(left)) || failed(scope, builder.add(right)))
        return { };
    RELEASE_AND_RETURN(scope, builder.finish());
}

// Union[arguments]
static JSValue unionFromSubscript(JSGlobalObject* globalObject, JSValue arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UnionBuilder builder(globalObject, true);
    if (failed(scope, builder.initialize()))
        return { };
    auto* tuple = PyTuple::tryFrom(arguments);
    if (tuple && tuple->type() != runtime(globalObject).tupleType())
        tuple = nullptr;
    if (failed(scope, tuple ? builder.addAll(tuple) : builder.add(arguments)))
        return { };
    RELEASE_AND_RETURN(scope, builder.finish());
}

static JSValue unionCheckedOr(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UnionBuilder builder(globalObject, true);
    if (failed(scope, builder.initialize()) || failed(scope, builder.add(left)) || failed(scope, builder.add(right)))
        return { };
    RELEASE_AND_RETURN(scope, builder.finish());
}

static JSValue unionOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return unionCheckedOr(globalObject, self, other); }
static JSValue unionROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return unionCheckedOr(globalObject, other, self); }
static JSValue unionClassGetItem(JSGlobalObject* globalObject, JSValue, JSValue arguments) { return unionFromSubscript(globalObject, arguments); }

static JSValue unionHash(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = aliasRecord(self);
    // With a member that cannot be hashed, two unions could be equal and hash differently.
    if (JSValue unhashableValue = record->get(UnionUnhashableArgs)) {
        PyTuple* unhashable = PyTuple::from(unhashableValue);
        // The complaint of the first is as good as any.
        for (unsigned i = 0; i < unhashable->length(); ++i) {
            hash(globalObject, unhashable->at(i));
            RETURN_IF_EXCEPTION(scope, { });
        }
        return raiseTypeError(globalObject, scope, "union contains "_s, unhashable->length(), " unhashable elements"_s);
    }
    int64_t result = hash(globalObject, record->get(UnionHashableArgs));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeInt(globalObject, result));
}

static bool unionsAreEqual(JSGlobalObject* globalObject, PyRecord* a, PyRecord* b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool equal = isEqual(globalObject, a->get(UnionHashableArgs), b->get(UnionHashableArgs));
    RETURN_IF_EXCEPTION(scope, false);
    if (!equal)
        return false;
    JSValue aValue = a->get(UnionUnhashableArgs);
    JSValue bValue = b->get(UnionUnhashableArgs);
    if (!aValue || !bValue)
        return !aValue && !bValue;

    PyTuple* aUnhashable = PyTuple::from(aValue);
    PyTuple* bUnhashable = PyTuple::from(bValue);
    if (aUnhashable->length() != bUnhashable->length())
        return false;
    auto isSubset = [&](PyTuple* inner, PyTuple* outer) {
        for (unsigned i = 0; i < inner->length(); ++i) {
            bool isThere = contains(globalObject, outer, inner->at(i));
            RETURN_IF_EXCEPTION(scope, false);
            if (!isThere)
                return false;
        }
        return true;
    };
    bool result = isSubset(aUnhashable, bUnhashable);
    RETURN_IF_EXCEPTION(scope, false);
    if (!result)
        return false;
    RELEASE_AND_RETURN(scope, isSubset(bUnhashable, aUnhashable));
}

template<bool wantEqual>
static JSValue unionEquality(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* b = tryUnion(globalObject, other);
    if (!b)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    bool equal = unionsAreEqual(globalObject, aliasRecord(self), b);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(equal == wantEqual);
}

static JSValue unionRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* args = unionArgs(aliasRecord(self));
    StrBuilder out;
    for (unsigned i = 0; i < args->length(); ++i) {
        if (i)
            out.append(" | "_s);
        aliasAppendTypeRepr(globalObject, out, args->at(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, out));
}

static JSValue unionGetItem(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = aliasRecord(self);
    PyTuple* parameters = aliasParameters(globalObject, record, UnionArgs, UnionParameters);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* arguments = aliasSubstituteParameters(globalObject, self, record->get(UnionArgs), parameters, item);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, unionFromSubscript(globalObject, arguments));
}

// The module typing expects the __module__ of the type, not an AttributeError.
static JSValue unionGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!name)
        return { };
    if (stringOf(name) == "__module__"_s)
        RELEASE_AND_RETURN(scope, getAttr(globalObject, aliasRecord(self)->type(), name));
    RELEASE_AND_RETURN(scope, genericGetAttr(globalObject, self, name));
}

static JSValue unionMroEntries(JSGlobalObject* globalObject, JSValue self, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = aliasReprText(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseTypeError(globalObject, scope, "Cannot subclass "_s, text);
}

static JSValue unionGetArgs(JSGlobalObject*, JSValue self) { return aliasRecord(self)->get(UnionArgs); }
static JSValue unionGetName(JSGlobalObject* globalObject, JSValue) { return internString(getVM(globalObject), "Union"_s); }
static JSValue unionGetOrigin(JSGlobalObject* globalObject, JSValue) { return runtime(globalObject).unionType(); }
static JSValue unionGetParameters(JSGlobalObject* globalObject, JSValue self) { return aliasParameters(globalObject, aliasRecord(self), UnionArgs, UnionParameters); }

static constexpr NativeDef unionMethods[] = {
    PY_SLOT("__repr__", unionRepr, "($self, /)"),
    PY_SLOT("__hash__", unionHash, "($self, /)"),
    PY_SLOT("__getattribute__", unionGetAttribute, "($self, name, /)"),
    PY_SLOT("__eq__", unionEquality<true>, "($self, value, /)"),
    PY_SLOT("__ne__", unionEquality<false>, "($self, value, /)"),
    PY_SLOT("__or__", unionOr, "($self, value, /)"),
    PY_SLOT("__ror__", unionROr, "($self, value, /)"),
    PY_SLOT("__getitem__", unionGetItem, "($self, key, /)"),
    PY_METHOD("__mro_entries__", unionMroEntries, "($self, bases, /)"),
    PY_CLASSMETHOD("__class_getitem__", unionClassGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet unionGetSets[] = {
    PY_MEMBER("__args__", unionGetArgs),
    { "__name__"_s, unionGetName },
    { "__qualname__"_s, unionGetName },
    { "__origin__"_s, unionGetOrigin },
    { "__parameters__"_s, unionGetParameters },
};

// ---- The types ----

void initializeGenericAliasTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.genericAliasType()->initializeBuiltin(globalObject, { "types.GenericAlias"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasWeakref }, aliasMethods, aliasGetSets,
        "Represent a PEP 585 generic type\n\nFor example, for t = list[int], t.__origin__ is list and t.__args__\nis (int,)."_s }, rt.typeType(), rt.objectType());
    rt.genericAliasIteratorType()->initializeBuiltin(globalObject, { "generic_alias_iterator"_s, { }, aliasIteratorMethods, { }, { } }, rt.typeType(), rt.objectType());
    rt.unionType()->initializeBuiltin(globalObject, { "typing.Union"_s, { TypeFlag::HasWeakref }, unionMethods, unionGetSets,
        "Represent a union type\n\nE.g. for int | str"_s }, rt.typeType(), rt.objectType());
}

} // namespace Python
} // namespace JSC
