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
#include "PythonImport.h"

// list[int] and int | str: types.GenericAlias and typing.Union. This is CPython's Objects/genericaliasobject.c and Objects/unionobject.c.

namespace JSC { namespace Python {

using Kind = PyNativeFunction::Kind;


namespace AliasField {
enum Field : unsigned { Origin, Arguments, Parameters, IsStarred };
}
namespace UnionField {
enum Field : unsigned { Arguments, Hashable, Unhashable, Parameters };
}

bool isGenericAlias(JSGlobalObject* globalObject, JSValue value)
{
    return value.isCell() && value.asCell()->type() == PyNativeObjectType && isInstance(globalObject, value, globalObject->pyRealm()->typeGenericAlias());
}

bool isUnion(JSGlobalObject* globalObject, JSValue value)
{
    return value.isCell() && value.asCell()->type() == PyNativeObjectType && typeOf(globalObject, value) == globalObject->pyRealm()->typeUnion();
}

PyTuple* argumentsOfUnion(JSValue value)
{
    return asTuple(asNativeObject(value)->field(UnionField::Arguments));
}

static bool isStarred(PyNativeObject* alias) { return alias->field(AliasField::IsStarred).isTrue(); }

static void appendAll(MarkedArgumentBuffer& buffer, PyTuple* tuple)
{
    for (auto& item : tuple->span())
        buffer.append(item.get());
}

// What is in a tuple or a list.
static void appendItems(JSGlobalObject* globalObject, MarkedArgumentBuffer& buffer, JSValue sequence)
{
    if (isTuple(sequence))
        return appendAll(buffer, asTuple(sequence));
    collect(globalObject, sequence, buffer);
}

static bool hasAttribute(JSGlobalObject* globalObject, JSValue value, const Identifier& name)
{
    return !!getAttributeIfPresent(globalObject, value, name);
}

static size_t indexOf(const MarkedArgumentBuffer& buffer, JSValue item)
{
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (buffer.at(i) == item)
            return i;
    }
    return notFound;
}

static size_t indexOf(PyTuple* tuple, JSValue item)
{
    for (unsigned i = 0; i < tuple->length(); ++i) {
        if (tuple->at(i) == item)
            return i;
    }
    return notFound;
}

// How a type is written where it is an argument to another: _Py_typing_type_repr() of Objects/typevarobject.c.
void appendTypeRepr(JSGlobalObject* globalObject, StringBuilder& out, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();

    if (value == realm->ellipsis())
        return out.append("..."_s);
    if (value == realm->typeNoneType())
        return out.append("None"_s);

    auto useRepr = [&] {
        String text = repr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, void());
        out.append(text);
    };

    // It looks like a GenericAlias.
    bool hasOrigin = hasAttribute(globalObject, value, names.dunder_origin);
    RETURN_IF_EXCEPTION(scope, void());
    if (hasOrigin) {
        bool hasArguments = hasAttribute(globalObject, value, names.dunder_args);
        RETURN_IF_EXCEPTION(scope, void());
        if (hasArguments)
            return useRepr();
    }

    JSValue qualifiedName = getAttributeIfPresent(globalObject, value, names.dunder_qualname);
    RETURN_IF_EXCEPTION(scope, void());
    if (!qualifiedName)
        return useRepr();
    JSValue module = getAttributeIfPresent(globalObject, value, names.dunder_module);
    RETURN_IF_EXCEPTION(scope, void());
    if (!module || isNone(module))
        return useRepr();

    // It looks like a class.
    String name = str(globalObject, qualifiedName);
    RETURN_IF_EXCEPTION(scope, void());
    String moduleName = str(globalObject, module);
    RETURN_IF_EXCEPTION(scope, void());
    if (module.isString() && moduleName == "builtins"_s)
        return out.append(name);
    out.append(moduleName, '.', name);
}

// ---- Type variables, and putting things in place of them

// The type variables among some arguments, each once: _Py_make_parameters().
PyTuple* makeParameters(JSGlobalObject* globalObject, JSValue arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (!vm.isSafeToRecurse()) [[unlikely]] {
        raise(globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded in __parameter__ calculation"_s);
        return nullptr;
    }

    MarkedArgumentBuffer items;
    appendItems(globalObject, items, arguments);
    RETURN_IF_EXCEPTION(scope, nullptr);
    MarkedArgumentBuffer parameters;
    auto add = [&] (JSValue parameter) {
        if (indexOf(parameters, parameter) == notFound)
            parameters.append(parameter);
    };
    for (size_t i = 0; i < items.size(); ++i) {
        JSValue item = items.at(i);
        // A class is no type variable, and could have a __parameters__ that is for its instances.
        if (isClass(item))
            continue;
        bool isVariable = hasAttribute(globalObject, item, names.dunder_typing_subst);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (isVariable) {
            add(item);
            continue;
        }
        JSValue inner = getAttributeIfPresent(globalObject, item, names.dunder_parameters);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!inner && (isTuple(item) || isList(item))) {
            inner = makeParameters(globalObject, item);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        if (inner && isTuple(inner)) {
            for (auto& parameter : asTuple(inner)->span())
                add(parameter.get());
        }
    }
    return PyTuple::createFromArguments(globalObject, parameters);
}

// If `object` is generic, as list[T] is, it with what is given put in place of its type variables. Otherwise itself.
static JSValue substituteInGeneric(JSGlobalObject* globalObject, JSValue object, PyTuple* parameters, const MarkedArgumentBuffer& given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSValue inner = getAttributeIfPresent(globalObject, object, names.dunder_parameters);
    RETURN_IF_EXCEPTION(scope, { });
    if (!inner || !isTuple(inner) || !asTuple(inner)->length())
        return object;
    MarkedArgumentBuffer arguments;
    for (auto& entry : asTuple(inner)->span()) {
        JSValue argument = entry.get();
        if (size_t index = indexOf(parameters, argument); index != notFound) {
            JSValue parameter = parameters->at(index);
            argument = given.at(index);
            // A TypeVarTuple stands for as many as it is given.
            if (typeOf(globalObject, parameter)->lookup(vm, names.dunder_iter) && isTuple(argument)) {
                appendAll(arguments, asTuple(argument));
                continue;
            }
        }
        arguments.append(argument);
    }
    RELEASE_AND_RETURN(scope, getItem(globalObject, object, PyTuple::createFromArguments(globalObject, arguments)));
}

static bool isUnpackedTypeVarTuple(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isClass(argument))
        return false;
    JSValue flag = getAttributeIfPresent(globalObject, argument, vm.pythonNames().dunder_typing_is_unpacked_typevartuple);
    RETURN_IF_EXCEPTION(scope, false);
    if (!flag)
        return false;
    RELEASE_AND_RETURN(scope, isTrue(globalObject, flag));
}

// The arguments of *tuple[int, str]. Empty if it is no such thing.
static JSValue unpackedTupleArguments(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isGenericAlias(globalObject, argument)) {
        auto* alias = asNativeObject(argument);
        if (isStarred(alias) && alias->field(AliasField::Origin) == globalObject->pyRealm()->typeTuple())
            return alias->field(AliasField::Arguments);
    }
    JSValue result = getAttributeIfPresent(globalObject, argument, vm.pythonNames().dunder_typing_unpacked_tuple_args);
    RETURN_IF_EXCEPTION(scope, { });
    if (!result || isNone(result))
        return { };
    return result;
}

// What is between the brackets, with each *tuple[int, str] replaced by int, str.
static JSValue unpackArguments(JSGlobalObject* globalObject, JSValue item)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer items;
    if (isTuple(item))
        appendAll(items, asTuple(item));
    else
        items.append(item);
    MarkedArgumentBuffer result;
    for (size_t i = 0; i < items.size(); ++i) {
        JSValue argument = items.at(i);
        if (!isClass(argument)) {
            JSValue inner = unpackedTupleArguments(globalObject, argument);
            RETURN_IF_EXCEPTION(scope, { });
            if (inner && isTuple(inner)) {
                auto* tuple = asTuple(inner);
                // tuple[int, ...] is of any length, and stays as it is.
                if (!(tuple->length() && tuple->at(tuple->length() - 1) == globalObject->pyRealm()->ellipsis())) {
                    appendAll(result, tuple);
                    continue;
                }
            }
        }
        result.append(argument);
    }
    return PyTuple::createFromArguments(globalObject, result);
}

// `arguments` with what is in `item` put in place of `parameters`: _Py_subs_parameters().
//     t = list[T];          t[int]      -> (int,)
//     t = dict[str, T];     t[int]      -> (str, int)
//     t = dict[T, list[S]]; t[str, int] -> (str, list[int])
//     t = list[[T]];        t[str]      -> ([str],)
PyTuple* substituteParameters(JSGlobalObject* globalObject, JSValue self, JSValue arguments, PyTuple* parameters, JSValue item)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    unsigned parameterCount = parameters->length();
    if (!parameterCount) {
        String text = repr(globalObject, self);
        RETURN_IF_EXCEPTION(scope, nullptr);
        raiseTypeError(globalObject, scope, concatenate(text, " is not a generic class"_s));
        return nullptr;
    }
    item = unpackArguments(globalObject, item);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (auto& parameter : parameters->span()) {
        JSValue prepare = getAttributeIfPresent(globalObject, parameter.get(), names.dunder_typing_prepare_subst);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!prepare || isNone(prepare))
            continue;
        item = call(globalObject, prepare, self, isTuple(item) ? item : JSValue(PyTuple::create(globalObject, { item })));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    MarkedArgumentBuffer given;
    if (isTuple(item))
        appendAll(given, asTuple(item));
    else
        given.append(item);
    if (given.size() != parameterCount) {
        String text = repr(globalObject, self);
        RETURN_IF_EXCEPTION(scope, nullptr);
        raiseTypeError(globalObject, scope, concatenate("Too "_s, given.size() > parameterCount ? "many"_s : "few"_s, " arguments for "_s, text, "; actual "_s, given.size(), ", expected "_s, parameterCount));
        return nullptr;
    }

    MarkedArgumentBuffer old;
    appendItems(globalObject, old, arguments);
    RETURN_IF_EXCEPTION(scope, nullptr);
    MarkedArgumentBuffer result;
    for (size_t i = 0; i < old.size(); ++i) {
        JSValue argument = old.at(i);
        if (isClass(argument)) {
            result.append(argument);
            continue;
        }
        if (isTuple(argument) || isList(argument)) {
            PyTuple* inner = substituteParameters(globalObject, self, argument, parameters, item);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (isTuple(argument))
                result.append(inner);
            else {
                MarkedArgumentBuffer items;
                appendAll(items, inner);
                result.append(newList(globalObject, items));
            }
            continue;
        }
        bool unpacks = isUnpackedTypeVarTuple(globalObject, argument);
        RETURN_IF_EXCEPTION(scope, nullptr);
        JSValue substitute = getAttributeIfPresent(globalObject, argument, names.dunder_typing_subst);
        RETURN_IF_EXCEPTION(scope, nullptr);
        JSValue replaced;
        if (substitute) {
            size_t index = indexOf(parameters, argument);
            RELEASE_ASSERT(index != notFound);
            replaced = call(globalObject, substitute, given.at(index));
        } else
            replaced = substituteInGeneric(globalObject, argument, parameters, given);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!unpacks) {
            result.append(replaced);
            continue;
        }
        if (!isTuple(replaced)) {
            raiseTypeError(globalObject, scope, concatenate("expected __typing_subst__ of "_s, typeName(globalObject, argument), " objects to return a tuple, not "_s, typeName(globalObject, replaced)));
            return nullptr;
        }
        appendAll(result, asTuple(replaced));
    }
    return PyTuple::createFromArguments(globalObject, result);
}

template<unsigned argumentsField, unsigned parametersField>
static PyTuple* parametersOf(JSGlobalObject* globalObject, PyNativeObject* self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue parameters = self->field(parametersField))
        return asTuple(parameters);
    PyTuple* parameters = makeParameters(globalObject, self->field(argumentsField));
    RETURN_IF_EXCEPTION(scope, nullptr);
    self->setField(vm, parametersField, parameters);
    return parameters;
}

// ---- types.GenericAlias

static void setUpGenericAlias(JSGlobalObject* globalObject, PyNativeObject* alias, JSValue origin, JSValue arguments, bool starred)
{
    VM& vm = globalObject->vm();
    if (!isTuple(arguments))
        arguments = PyTuple::create(globalObject, { arguments });
    alias->setField(vm, AliasField::Origin, origin);
    alias->setField(vm, AliasField::Arguments, arguments);
    alias->setField(vm, AliasField::IsStarred, jsBoolean(starred));
}

JSValue newGenericAlias(JSGlobalObject* globalObject, JSValue origin, JSValue arguments, bool starred)
{
    auto* alias = PyNativeObject::create(globalObject, BuiltinType::GenericAlias);
    setUpGenericAlias(globalObject, alias, origin, arguments, starred);
    return alias;
}

PYTHON_NATIVE(genericAliasNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* alias = PyNativeObject::create(vm, asType(args[0])->instanceStructure());
    setUpGenericAlias(globalObject, alias, args[1], args[2], false);
    return JSValue::encode(alias);
}

// cls[item], for a built-in class that is generic
PYTHON_NATIVE(genericClassGetItem)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newGenericAlias(globalObject, args[0], args[1], false));
}

PYTHON_NATIVE(genericAliasRepr)
{
    NATIVE_PROLOGUE();
    auto* alias = asNativeObject(args[0]);
    TextBuilder out;
    if (isStarred(alias))
        out.append('*');
    appendTypeRepr(globalObject, out, alias->field(AliasField::Origin));
    RETURN_IF_EXCEPTION(scope, { });
    out.append('[');
    auto* arguments = asTuple(alias->field(AliasField::Arguments));
    for (unsigned i = 0; i < arguments->length(); ++i) {
        if (i)
            out.append(", "_s);
        JSValue argument = arguments->at(i);
        // Callable[[int, str], bool]
        if (isList(argument) && typeOf(globalObject, argument) == realm->typeList()) {
            out.append('[');
            // ga_repr_items_list(): as many as there were to begin with. Showing one can run anything.
            for (unsigned j = 0, length = asList(argument)->length(); j < length; ++j) {
                if (j)
                    out.append(", "_s);
                if (j >= asList(argument)->length())
                    return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "list index out of range"_s));
                JSValue item = listGet(globalObject, asList(argument), j);
                RETURN_IF_EXCEPTION(scope, { });
                appendTypeRepr(globalObject, out, item);
                RETURN_IF_EXCEPTION(scope, { });
            }
            out.append(']');
            continue;
        }
        appendTypeRepr(globalObject, out, argument);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!arguments->length())
        out.append("()"_s);
    out.append(']');
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, out.tryFinish())));
}

PYTHON_NATIVE(genericAliasGetItem)
{
    NATIVE_PROLOGUE();
    auto* alias = asNativeObject(args[0]);
    PyTuple* parameters = parametersOf<AliasField::Arguments, AliasField::Parameters>(globalObject, alias);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* arguments = substituteParameters(globalObject, alias, alias->field(AliasField::Arguments), parameters, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(newGenericAlias(globalObject, alias->field(AliasField::Origin), arguments, isStarred(alias)));
}

PYTHON_NATIVE(genericAliasHash)
{
    NATIVE_PROLOGUE();
    auto* alias = asNativeObject(args[0]);
    int64_t origin = hash(globalObject, alias->field(AliasField::Origin));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t arguments = hash(globalObject, alias->field(AliasField::Arguments));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, origin ^ arguments));
}

// list[int]() is list(), which is told what it was made by if it can be told anything.
PYTHON_NATIVE(genericAliasCall)
{
    NATIVE_PROLOGUE();
    auto* alias = asNativeObject(args[0]);
    ArgList arguments = args.allFrom(1);
    JSValue object = callWithKeywords(globalObject, alias->field(AliasField::Origin), arguments, args.keywordNames());
    RETURN_IF_EXCEPTION(scope, { });
    setAttribute(globalObject, object, names.dunder_orig_class, alias);
    if (scope.exception()) {
        if (!catchException(globalObject, BuiltinType::AttributeError) && !catchException(globalObject, BuiltinType::TypeError))
            return { };
    }
    return JSValue::encode(object);
}

// Those of its own that it does not pass on to what it is an alias of.
static bool isOwnAttributeOfGenericAlias(const CommonNames& names, const Identifier& name)
{
    for (const Identifier* own : { &names.dunder_class, &names.dunder_origin, &names.dunder_args, &names.dunder_unpacked, &names.dunder_parameters,
        &names.dunder_typing_unpacked_tuple_args, &names.dunder_mro_entries, &names.dunder_reduce_ex, &names.dunder_reduce }) {
        if (*own == name)
            return true;
    }
    return false;
}

PYTHON_NATIVE(genericAliasGetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    // Nor these, which it does not have. It is no class, and copying it is not copying what it is an alias of.
    bool isBlocked = *name == names.dunder_bases || *name == names.dunder_copy || *name == names.dunder_deepcopy;
    if (!isBlocked && !isOwnAttributeOfGenericAlias(names, *name))
        RELEASE_AND_RETURN(scope, JSValue::encode(getAttribute(globalObject, asNativeObject(args[0])->field(AliasField::Origin), *name)));
    JSValue value = genericGetAttribute(globalObject, args[0], *name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', typeName(globalObject, args[0]), "' object has no attribute '"_s, name->string(), '\'')));
    return JSValue::encode(value);
}

PYTHON_NATIVE(genericAliasCompare)
{
    NATIVE_PROLOGUE();
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    if (!isGenericAlias(globalObject, args[1]) || (op != ComparisonOperator::Eq && op != ComparisonOperator::NotEq))
        RETURN_NOT_IMPLEMENTED();
    auto* a = asNativeObject(args[0]);
    auto* b = asNativeObject(args[1]);
    bool wantsEqual = op == ComparisonOperator::Eq;
    if (isStarred(a) != isStarred(b))
        return JSValue::encode(jsBoolean(!wantsEqual));
    bool sameOrigin = isEqual(globalObject, a->field(AliasField::Origin), b->field(AliasField::Origin));
    RETURN_IF_EXCEPTION(scope, { });
    if (!sameOrigin)
        return JSValue::encode(jsBoolean(!wantsEqual));
    JSValue sameArguments = compare(globalObject, ComparisonOperator::Eq, a->field(AliasField::Arguments), b->field(AliasField::Arguments));
    RETURN_IF_EXCEPTION(scope, { });
    if (wantsEqual)
        return JSValue::encode(sameArguments);
    return JSValue::encode(jsBoolean(!sameArguments.isTrue()));
}

PYTHON_NATIVE(genericAliasMroEntries)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyTuple::create(globalObject, { asNativeObject(args[0])->field(AliasField::Origin) }));
}

PYTHON_NATIVE(genericAliasInstanceCheck)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raiseTypeError(globalObject, scope, "isinstance() argument 2 cannot be a parameterized generic"_s));
}

PYTHON_NATIVE(genericAliasSubclassCheck)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raiseTypeError(globalObject, scope, "issubclass() argument 2 cannot be a parameterized generic"_s));
}

PYTHON_NATIVE(genericAliasReduce)
{
    NATIVE_PROLOGUE();
    auto* alias = asNativeObject(args[0]);
    if (isStarred(alias)) {
        JSValue iterator = getIterator(globalObject, newGenericAlias(globalObject, alias->field(AliasField::Origin), alias->field(AliasField::Arguments), false));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue next = getBuiltin(globalObject, "next"_s);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(PyTuple::create(globalObject, { next, PyTuple::create(globalObject, { iterator }) }));
    }
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, alias)->object(), PyTuple::create(globalObject, { alias->field(AliasField::Origin), alias->field(AliasField::Arguments) }) }));
}

PYTHON_NATIVE(genericAliasDir)
{
    NATIVE_PROLOGUE();
    JSValue list = dirOf(globalObject, asNativeObject(args[0])->field(AliasField::Origin));
    RETURN_IF_EXCEPTION(scope, { });
    for (const Identifier* own : { &names.dunder_class, &names.dunder_origin, &names.dunder_args, &names.dunder_unpacked, &names.dunder_parameters,
        &names.dunder_typing_unpacked_tuple_args, &names.dunder_mro_entries, &names.dunder_reduce_ex, &names.dunder_reduce }) {
        JSString* name = jsString(vm, own->string());
        bool isThere = contains(globalObject, list, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isThere)
            listAppend(globalObject, asList(list), name);
    }
    return JSValue::encode(list);
}

// *tuple[int, str] goes through tuple[int, str], which gives one thing: itself, starred.
PYTHON_NATIVE(genericAliasIter)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyNativeObject::create(globalObject, BuiltinType::GenericAliasIterator, args[0]));
}

PYTHON_NATIVE(returnSelf)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(args[0]);
}

PYTHON_NATIVE(genericAliasIteratorNext)
{
    NATIVE_PROLOGUE();
    auto* iterator = asNativeObject(args[0]);
    JSValue object = iterator->field(0);
    if (!object)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    auto* alias = asNativeObject(object);
    iterator->setField(vm, 0, JSValue());
    return JSValue::encode(newGenericAlias(globalObject, alias->field(AliasField::Origin), alias->field(AliasField::Arguments), true));
}

PYTHON_NATIVE(genericAliasIteratorReduce)
{
    NATIVE_PROLOGUE();
    JSValue iter = getBuiltin(globalObject, "iter"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue object = asNativeObject(args[0])->field(0);
    return JSValue::encode(PyTuple::create(globalObject, { iter, PyTuple::create(globalObject, { object ? object : JSValue(PyTuple::create(globalObject, 0)) }) }));
}

// ---- typing.Union

static bool isUnionable(JSGlobalObject* globalObject, JSValue value)
{
    return isNone(value) || isClass(value) || isGenericAlias(globalObject, value) || isUnion(globalObject, value) || isTypeAlias(globalObject, value);
}

namespace {

// What a union is made from, one at a time. Nothing is in a union twice, and a union in a union is what is in it.
class UnionBuilder {
public:
    UnionBuilder(JSGlobalObject* globalObject, bool isChecked)
        : m_globalObject(globalObject)
        , m_isChecked(isChecked)
    {
        m_hashable.append(PySet::create(globalObject->vm(), globalObject->pyRealm()->structureFor(BuiltinType::FrozenSet)));
    }

    // False if something has been raised.
    bool add(JSValue argument)
    {
        VM& vm = m_globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (isNone(argument))
            argument = m_globalObject->pyRealm()->typeNoneType();
        else if (isUnion(m_globalObject, argument))
            RELEASE_AND_RETURN(scope, addAll(argumentsOfUnion(argument)));
        if (m_isChecked) {
            argument = check(argument);
            RETURN_IF_EXCEPTION(scope, false);
        }
        RELEASE_AND_RETURN(scope, addUnchecked(argument));
    }

    bool addAll(PyTuple* tuple)
    {
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (!add(tuple->at(i)))
                return false;
        }
        return true;
    }

    JSValue finish()
    {
        VM& vm = m_globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (m_arguments.isEmpty())
            return raiseTypeError(m_globalObject, scope, "Cannot take a Union of no types."_s);
        if (m_arguments.size() == 1)
            return m_arguments.at(0);
        return PyNativeObject::create(m_globalObject, BuiltinType::Union, PyTuple::createFromArguments(m_globalObject, m_arguments), m_hashable.at(0),
            m_unhashable.isEmpty() ? JSValue() : JSValue(PyTuple::createFromArguments(m_globalObject, m_unhashable)));
    }

private:
    // What is neither a class nor made of classes is for the library to judge: a string may stand for a class that is yet to come.
    JSValue check(JSValue argument)
    {
        VM& vm = m_globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (isUnionable(m_globalObject, argument))
            return argument;
        JSValue typing = importModule(m_globalObject, "typing"_s);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue function = getAttribute(m_globalObject, typing, Identifier::fromString(vm, "_type_check"_s));
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, call(m_globalObject, function, argument, jsNontrivialString(vm, "Union[arg, ...]: each arg must be a type."_s)));
    }

    bool addUnchecked(JSValue argument)
    {
        VM& vm = m_globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        hash(m_globalObject, argument);
        if (scope.exception()) {
            // Whatever it was that was raised.
            if (!catchException(m_globalObject, BuiltinType::BaseException))
                return false;
            for (size_t i = 0; i < m_unhashable.size(); ++i) {
                bool isSame = isEqual(m_globalObject, m_unhashable.at(i), argument);
                RETURN_IF_EXCEPTION(scope, false);
                if (isSame)
                    return true;
            }
            m_unhashable.append(argument);
        } else {
            bool wasAdded = false;
            uncheckedDowncast<PySet>(m_hashable.at(0).asCell())->add(m_globalObject, argument, &wasAdded);
            RETURN_IF_EXCEPTION(scope, false);
            if (!wasAdded)
                return true;
        }
        m_arguments.append(argument);
        return true;
    }

    JSGlobalObject* m_globalObject;
    bool m_isChecked;
    MarkedArgumentBuffer m_arguments;
    MarkedArgumentBuffer m_hashable; // One thing: a frozenset.
    MarkedArgumentBuffer m_unhashable;
};

} // namespace

// a | b, for classes and what is made of them: _Py_union_type_or().
JSValue unionOf(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    if (!isUnionable(globalObject, a) || !isUnionable(globalObject, b))
        return globalObject->pyRealm()->notImplemented();
    UnionBuilder builder(globalObject, false);
    if (!builder.add(a) || !builder.add(b))
        return { };
    return builder.finish();
}

// Union[arguments]
JSValue unionFrom(JSGlobalObject* globalObject, JSValue arguments)
{
    UnionBuilder builder(globalObject, true);
    bool isExactlyTuple = isTuple(arguments) && typeOf(globalObject, arguments) == globalObject->pyRealm()->typeTuple();
    if (isExactlyTuple ? !builder.addAll(asTuple(arguments)) : !builder.add(arguments))
        return { };
    return builder.finish();
}

// __or__ and __ror__ of a class and of a GenericAlias
PYTHON_SHARED_NATIVE(typeOr)
{
    NATIVE_PROLOGUE();
    bool isReflected = unpack<bool>(callFrame, 0);
    RELEASE_AND_RETURN(scope, JSValue::encode(unionOf(globalObject, args[isReflected], args[!isReflected])));
}

PYTHON_NATIVE(unionOr)
{
    NATIVE_PROLOGUE();
    bool isReflected = unpack<bool>(callFrame, 0);
    UnionBuilder builder(globalObject, true);
    if (!builder.add(args[isReflected]) || !builder.add(args[!isReflected]))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(builder.finish()));
}

PYTHON_NATIVE(unionHash)
{
    NATIVE_PROLOGUE();
    auto* self = asNativeObject(args[0]);
    if (JSValue unhashable = self->field(UnionField::Unhashable)) {
        // Whichever of them says why is left to say it.
        for (auto& argument : asTuple(unhashable)->span()) {
            hash(globalObject, argument.get());
            RETURN_IF_EXCEPTION(scope, { });
        }
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("union contains "_s, asTuple(unhashable)->length(), " unhashable elements"_s)));
    }
    int64_t result = hash(globalObject, self->field(UnionField::Hashable));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, result));
}

static bool unionsAreEqual(JSGlobalObject* globalObject, PyNativeObject* a, PyNativeObject* b)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool sameHashable = isEqual(globalObject, a->field(UnionField::Hashable), b->field(UnionField::Hashable));
    RETURN_IF_EXCEPTION(scope, false);
    if (!sameHashable)
        return false;
    JSValue first = a->field(UnionField::Unhashable);
    JSValue second = b->field(UnionField::Unhashable);
    if (!first || !second)
        return !first && !second;
    if (asTuple(first)->length() != asTuple(second)->length())
        return false;
    for (auto [these, those] : { std::pair { first, second }, std::pair { second, first } }) {
        for (auto& argument : asTuple(these)->span()) {
            bool isThere = contains(globalObject, those, argument.get());
            RETURN_IF_EXCEPTION(scope, false);
            if (!isThere)
                return false;
        }
    }
    return true;
}

PYTHON_NATIVE(unionCompare)
{
    NATIVE_PROLOGUE();
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    if (!isUnion(globalObject, args[1]) || (op != ComparisonOperator::Eq && op != ComparisonOperator::NotEq))
        RETURN_NOT_IMPLEMENTED();
    bool areEqual = unionsAreEqual(globalObject, asNativeObject(args[0]), asNativeObject(args[1]));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(areEqual == (op == ComparisonOperator::Eq)));
}

PYTHON_NATIVE(unionRepr)
{
    NATIVE_PROLOGUE();
    TextBuilder out;
    PyTuple* arguments = argumentsOfUnion(args[0]);
    for (unsigned i = 0; i < arguments->length(); ++i) {
        if (i)
            out.append(" | "_s);
        appendTypeRepr(globalObject, out, arguments->at(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, out.tryFinish())));
}

PYTHON_NATIVE(unionGetItem)
{
    NATIVE_PROLOGUE();
    auto* self = asNativeObject(args[0]);
    PyTuple* parameters = parametersOf<UnionField::Arguments, UnionField::Parameters>(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* arguments = substituteParameters(globalObject, self, self->field(UnionField::Arguments), parameters, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(unionFrom(globalObject, arguments)));
}

PYTHON_NATIVE(unionGetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    // The library expects it of what stands for a type.
    if (*name == names.dunder_module)
        RELEASE_AND_RETURN(scope, JSValue::encode(getAttribute(globalObject, typeOf(globalObject, args[0]), *name)));
    JSValue value = genericGetAttribute(globalObject, args[0], *name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', typeName(globalObject, args[0]), "' object has no attribute '"_s, name->string(), '\'')));
    return JSValue::encode(value);
}

PYTHON_NATIVE(unionClassGetItem)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(unionFrom(globalObject, args[1])));
}

PYTHON_NATIVE(unionMroEntries)
{
    NATIVE_PROLOGUE();
    String text = repr(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("Cannot subclass "_s, text)));
}

template<unsigned index>
static JSValue getField(JSGlobalObject*, JSValue self)
{
    return asNativeObject(self)->field(index);
}

void initializeGenericAliasAndUnion(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    for (PyType* type : { realm->typeGenericAlias(), realm->typeGenericAliasIterator(), realm->typeUnion() })
        type->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, type));

    auto addComparisons = [&] (PyType* type, NativeFunction function) {
        addMethods(globalObject, type, {
            { "__lt__"_s, function, Kind::Method, pack(ComparisonOperator::Lt) },
            { "__le__"_s, function, Kind::Method, pack(ComparisonOperator::LtE) },
            { "__eq__"_s, function, Kind::Method, pack(ComparisonOperator::Eq) },
            { "__ne__"_s, function, Kind::Method, pack(ComparisonOperator::NotEq) },
            { "__gt__"_s, function, Kind::Method, pack(ComparisonOperator::Gt) },
            { "__ge__"_s, function, Kind::Method, pack(ComparisonOperator::GtE) },
        });
    };

    PyType* alias = realm->typeGenericAlias();
    addMethods(globalObject, alias, {
        { "__new__"_s, genericAliasNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__repr__"_s, genericAliasRepr },
        { "__getitem__"_s, genericAliasGetItem },
        { "__hash__"_s, genericAliasHash },
        { "__call__"_s, genericAliasCall },
        { "__getattribute__"_s, genericAliasGetAttribute },
        { "__mro_entries__"_s, genericAliasMroEntries },
        { "__instancecheck__"_s, genericAliasInstanceCheck },
        { "__subclasscheck__"_s, genericAliasSubclassCheck },
        { "__reduce__"_s, genericAliasReduce },
        { "__dir__"_s, genericAliasDir },
        { "__iter__"_s, genericAliasIter },
        { "__or__"_s, typeOr, Kind::Method, pack(false) },
        { "__ror__"_s, typeOr, Kind::Method, pack(true) },
    });
    addComparisons(alias, genericAliasCompare);
    addMember(globalObject, alias, "__origin__"_s, getField<AliasField::Origin>);
    addMember(globalObject, alias, "__args__"_s, getField<AliasField::Arguments>);
    addMember(globalObject, alias, "__unpacked__"_s, getField<AliasField::IsStarred>);
    addGetSet(globalObject, alias, "__parameters__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        return parametersOf<AliasField::Arguments, AliasField::Parameters>(globalObject, asNativeObject(self));
    });
    addGetSet(globalObject, alias, "__typing_unpacked_tuple_args__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        auto* alias = asNativeObject(self);
        return isStarred(alias) && alias->field(AliasField::Origin) == globalObject->pyRealm()->typeTuple() ? alias->field(AliasField::Arguments) : jsUndefined();
    });

    addMethods(globalObject, realm->typeGenericAliasIterator(), {
        { "__iter__"_s, returnSelf },
        { "__next__"_s, genericAliasIteratorNext },
        { "__reduce__"_s, genericAliasIteratorReduce },
    });

    PyType* unionType = realm->typeUnion();
    addMethods(globalObject, unionType, {
        { "__hash__"_s, unionHash },
        { "__repr__"_s, unionRepr },
        { "__getitem__"_s, unionGetItem },
        { "__getattribute__"_s, unionGetAttribute },
        { "__or__"_s, unionOr, Kind::Method, pack(false) },
        { "__ror__"_s, unionOr, Kind::Method, pack(true) },
        { "__mro_entries__"_s, unionMroEntries },
        { "__class_getitem__"_s, unionClassGetItem, Kind::ClassMethod },
    });
    addComparisons(unionType, unionCompare);
    addMember(globalObject, unionType, "__args__"_s, getField<UnionField::Arguments>);
    auto name = [] (JSGlobalObject* globalObject, JSValue) -> JSValue { return jsNontrivialString(globalObject->vm(), "Union"_s); };
    addGetSet(globalObject, unionType, "__name__"_s, name);
    addGetSet(globalObject, unionType, "__qualname__"_s, name);
    addGetSet(globalObject, unionType, "__origin__"_s, [] (JSGlobalObject* globalObject, JSValue) -> JSValue { return globalObject->pyRealm()->typeUnion(); });
    addGetSet(globalObject, unionType, "__parameters__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        return parametersOf<UnionField::Arguments, UnionField::Parameters>(globalObject, asNativeObject(self));
    });

    addMethods(globalObject, realm->typeType(), {
        { "__or__"_s, typeOr, Kind::Method, pack(false) },
        { "__ror__"_s, typeOr, Kind::Method, pack(true) },
    });

    // The built-in classes that are generic.
    for (unsigned i = 0; i < numberOfBuiltinTypes; ++i) {
        PyType* type = realm->type(static_cast<BuiltinType>(i));
        // Those two have something of their own to do.
        if (type != unionType && type != realm->typeGeneric())
            addClassGetItemIfGeneric(globalObject, type);
    }
}

void addClassGetItemIfGeneric(JSGlobalObject* globalObject, PyType* type)
{
    addMethodsThatCPythonHas(globalObject, type, { { "__class_getitem__"_s, genericClassGetItem, PyNativeFunction::Kind::ClassMethod } });
}

} } // namespace JSC::Python
