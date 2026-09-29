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
#include "PythonRuntimeFunctions.h"

#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PyDict.h"
#include "PyFrame.h"
#include "PyNativeFunction.h"
#include "PyInstance.h"
#include "PyObjects.h"
#include "PythonBytes.h"
#include "PythonGenerators.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonStrings.h"
#include "UnlinkedFunctionExecutable.h"

namespace JSC { namespace Python {

#define PYTHON_RUNTIME_FUNCTION(name) \
    static JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame))

#define PROLOGUE() \
    VM& vm = globalObject->vm(); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    [[maybe_unused]] PyRealm* realm = globalObject->pyRealm(); \
    [[maybe_unused]] auto argument = [&] (unsigned i) { return callFrame->uncheckedArgument(i); };

static bool isMarker(PyRealm* realm, JSValue value) { return value.isCell() && value.asCell() == realm->boundArgumentsMarker(); }

// ---- Names

static JSValue lookUpInNamespace(JSGlobalObject* globalObject, JSValue namespaceValue, JSString* name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A class derived from dict can have a __getitem__() of its own.
    if (isDict(namespaceValue) && namespaceValue.asCell()->structure() == globalObject->pyRealm()->structureFor(BuiltinType::Dict))
        RELEASE_AND_RETURN(scope, uncheckedDowncast<PyDict>(namespaceValue.asCell())->get(globalObject, name));
    // What __prepare__ gave, or exec() was given, can be any mapping.
    JSValue value = getItem(globalObject, namespaceValue, name);
    if (scope.exception()) {
        catchException(globalObject, BuiltinType::KeyError);
        return { };
    }
    return value;
}

PYTHON_RUNTIME_FUNCTION(loadName)
{
    PROLOGUE();
    JSString* name = asString(argument(3));
    JSValue value = lookUpInNamespace(globalObject, argument(0), name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    auto identifier = name->toIdentifier(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    GlobalLocation location;
    RELEASE_AND_RETURN(scope, JSValue::encode(loadGlobal(globalObject, asObject(argument(1)), asObject(argument(2)), identifier, location)));
}

PYTHON_RUNTIME_FUNCTION(loadFromNamespace)
{
    PROLOGUE();
    JSValue value = lookUpInNamespace(globalObject, argument(0), asString(argument(1)));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(value ? value : JSValue(realm->boundArgumentsMarker()));
}

// ---- Annotations

// What an __annotate__ begins with: if format > 2: raise NotImplementedError. There are ways of giving annotations that only the library knows.
PYTHON_RUNTIME_FUNCTION(checkAnnotationFormat)
{
    PROLOGUE();
    JSValue isBeyond = compare(globalObject, ComparisonOperator::Gt, argument(0), jsNumber(2));
    RETURN_IF_EXCEPTION(scope, { });
    bool result = isTrue(globalObject, isBeyond);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::NotImplementedError, JSValue()));
    return JSValue::encode(jsUndefined());
}

// setUpAnnotations(where, whether that is a mapping and not a module): if '__annotations__' not in locals(): __annotations__ = {}
PYTHON_RUNTIME_FUNCTION(setUpAnnotations)
{
    PROLOGUE();
    auto& names = vm.pythonNames();
    if (!argument(1).asBoolean()) {
        if (!getStoredAttribute(vm, asObject(argument(0)), names.dunder_annotations))
            putStoredAttribute(vm, asObject(argument(0)), names.dunder_annotations, PyDict::create(globalObject));
        return JSValue::encode(jsUndefined());
    }
    JSString* name = jsString(vm, names.dunder_annotations.string());
    JSValue present = lookUpInNamespace(globalObject, argument(0), name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!present) {
        scope.release();
        setItem(globalObject, argument(0), name, PyDict::create(globalObject));
    }
    return JSValue::encode(jsUndefined());
}

// ---- t"..."

// newInterpolation(value, how it was written, 's', 'r', 'a' or None, format specification)
PYTHON_RUNTIME_FUNCTION(runtimeNewInterpolation)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newInterpolation(globalObject, argument(0), argument(1), argument(2), argument(3)));
}

PYTHON_RUNTIME_FUNCTION(runtimeNewTemplate)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newTemplate(globalObject, argument(0), argument(1)));
}

// ---- Type parameters

// newTypeVar(name, what works out its bound or its constraints or None, which of the two)
PYTHON_RUNTIME_FUNCTION(runtimeNewTypeVar)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newTypeVar(globalObject, asString(argument(0)), isNone(argument(1)) ? JSValue() : argument(1), argument(2).asBoolean()));
}

PYTHON_RUNTIME_FUNCTION(runtimeNewParamSpec)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newParamSpec(globalObject, asString(argument(0))));
}

PYTHON_RUNTIME_FUNCTION(runtimeNewTypeVarTuple)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newTypeVarTuple(globalObject, asString(argument(0))));
}

PYTHON_RUNTIME_FUNCTION(runtimeSetTypeParameterDefault)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    setTypeParameterDefault(globalObject, argument(0), argument(1));
    return JSValue::encode(jsUndefined());
}

// newTypeAlias(name, its type parameters or None, what works out its value)
PYTHON_RUNTIME_FUNCTION(runtimeNewTypeAlias)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(newTypeAlias(globalObject, asString(argument(0)), argument(1), argument(2)));
}

PYTHON_RUNTIME_FUNCTION(runtimeSubscriptGeneric)
{
    PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(subscriptGeneric(globalObject, uncheckedDowncast<PyTuple>(argument(0).asCell()))));
}

static JSValue raiseNameError(JSGlobalObject* globalObject, ThrowScope& scope, JSString* name)
{
    return raiseNameError(globalObject, scope, name->value(globalObject));
}

PYTHON_RUNTIME_FUNCTION(deleteGlobal)
{
    PROLOGUE();
    JSObject* globals = asObject(argument(0));
    auto identifier = asString(argument(1))->toIdentifier(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (!getStoredAttribute(vm, globals, identifier))
        return JSValue::encode(raiseNameError(globalObject, scope, asString(argument(1))));
    deleteStoredAttribute(globalObject, globals, identifier);
    return JSValue::encode(jsUndefined());
}

PYTHON_RUNTIME_FUNCTION(deleteName)
{
    PROLOGUE();
    deleteItem(globalObject, argument(0), argument(1));
    if (scope.exception() && catchException(globalObject, BuiltinType::KeyError))
        return JSValue::encode(raiseNameError(globalObject, scope, asString(argument(1))));
    return JSValue::encode(jsUndefined());
}

// ---- Making things

PYTHON_RUNTIME_FUNCTION(newBytes)
{
    PROLOGUE();
    // The literal comes as a string with a character for each byte.
    String text = asString(argument(0))->value(globalObject);
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, text.is8Bit() ? byteCast<uint8_t>(text.span8()) : std::span<const uint8_t>())));
}

// 2j: the imaginary part, and then the real part if there is one, which there is no writing
PYTHON_RUNTIME_FUNCTION(newComplex)
{
    return JSValue::encode(PyComplex::create(globalObject, callFrame->argumentCount() > 1 ? callFrame->uncheckedArgument(1).asNumber() : 0, callFrame->uncheckedArgument(0).asNumber()));
}

// BUILD_STRING, of a tuple, where they may not all be strings: f"..." in a tree that a program made
PYTHON_RUNTIME_FUNCTION(joinStrings)
{
    PROLOGUE();
    PyTuple* pieces = asTuple(argument(0));
    TextBuilder result;
    for (unsigned i = 0; i < pieces->length(); ++i) {
        JSValue piece = pieces->at(i);
        JSString* string = stringIn(piece);
        if (!string)
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("sequence item "_s, i, ": expected str instance, "_s, typeName(globalObject, piece), " found"_s)));
        auto view = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        result.append(view.data);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, result.tryFinish())));
}

// A constant that is a frozenset, of a tuple. There is no writing one.
PYTHON_RUNTIME_FUNCTION(newFrozenSet)
{
    return JSValue::encode(setFromIterable(globalObject, globalObject->pyRealm()->typeFrozenSet()->instanceStructure(), callFrame->uncheckedArgument(0)));
}

// [1, 2, 3] and {1, 2, 3}: what is in them is a constant, and this is the list or the set.
PYTHON_RUNTIME_FUNCTION(listOfTuple)
{
    PyTuple* tuple = asTuple(callFrame->uncheckedArgument(0));
    return JSValue::encode(newList(globalObject, ArgList(std::bit_cast<EncodedJSValue*>(tuple->span().data()), tuple->length())));
}

PYTHON_RUNTIME_FUNCTION(setOfFrozenSet)
{
    PySet* set = PySet::create(globalObject);
    set->copyFrom(globalObject->vm(), globalObject, *uncheckedDowncast<PySet>(callFrame->uncheckedArgument(0).asCell()));
    return JSValue::encode(set);
}

PYTHON_RUNTIME_FUNCTION(newSlice)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PySlice::create(globalObject, argument(0), argument(1), argument(2)));
}

static String describeCallable(JSGlobalObject*, JSValue);

// listExtend(list, iterable, what is being called with them or a marker)
PYTHON_RUNTIME_FUNCTION(runtimeListExtend)
{
    PROLOGUE();
    // f(*iterable), with nothing else given by position, does not go by way of a list in CPython. It is PySequence_Tuple() that gathers them, and that does not ask how many there will be.
    if (argument(2) == realm->boundArgumentsMarker())
        listExtend(globalObject, asList(argument(0)), argument(1));
    else {
        JSArray* list = asList(argument(0));
        forEach(globalObject, argument(1), [&] (JSValue value) {
            listAppend(globalObject, list, value);
            return true;
        });
    }
    if (scope.exception() && !hasWhatItTakesToBeIterated(globalObject, argument(1)) && catchException(globalObject, BuiltinType::TypeError)) {
        if (argument(2) == realm->boundArgumentsMarker())
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("Value after * must be an iterable, not "_s, typeName(globalObject, argument(1)))));
        String callee = describeCallable(globalObject, argument(2));
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(callee, " argument after * must be an iterable, not "_s, typeName(globalObject, argument(1)))));
    }
    return JSValue::encode(jsUndefined());
}

PYTHON_RUNTIME_FUNCTION(runtimeListAppend)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    listAppend(globalObject, asList(argument(0)), argument(1));
    return JSValue::encode(jsUndefined());
}

PYTHON_RUNTIME_FUNCTION(listToTuple)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    JSArray* list = asList(argument(0));
    PyTuple* tuple = PyTuple::create(globalObject, list->length());
    for (unsigned i = 0; i < list->length(); ++i)
        tuple->initializeAt(vm, i, list->getIndexQuickly(i));
    return JSValue::encode(tuple);
}

// {*iterable}
PYTHON_RUNTIME_FUNCTION(setUpdate)
{
    PROLOGUE();
    auto* set = uncheckedDowncast<PySet>(argument(0).asCell());
    scope.release();
    forEach(globalObject, argument(1), [&] (JSValue item) {
        return set->add(globalObject, item);
    });
    return JSValue::encode(jsUndefined());
}

PYTHON_RUNTIME_FUNCTION(newSet)
{
    PROLOGUE();
    PySet* set = PySet::create(globalObject);
    for (unsigned i = 0; i < callFrame->argumentCount(); ++i) {
        set->add(globalObject, argument(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(set);
}

PYTHON_RUNTIME_FUNCTION(setAdd)
{
    PROLOGUE();
    scope.release();
    uncheckedDowncast<PySet>(argument(0).asCell())->add(globalObject, argument(1));
    return JSValue::encode(jsUndefined());
}

PYTHON_RUNTIME_FUNCTION(newDict)
{
    PROLOGUE();
    PyDict* dict = PyDict::create(globalObject);
    for (unsigned i = 0; i + 1 < callFrame->argumentCount(); i += 2) {
        dict->set(globalObject, argument(i), argument(i + 1));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(dict);
}

// Calls the function with each key and value of a mapping. False if it raised, or if it is not one, and then nothing is raised.
template<typename Function>
static bool forEachItem(JSGlobalObject* globalObject, JSValue mapping, const Function& function)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isGoneThroughAsDict(globalObject, mapping)) {
        asDict(mapping)->forEach(globalObject, [&] (JSValue key, JSValue value) {
            function(key, value);
            return !scope.exception();
        });
        return !scope.exception();
    }
    JSValue keysMethod = getAttributeIfPresent(globalObject, mapping, Identifier::fromString(vm, "keys"_s));
    RETURN_IF_EXCEPTION(scope, false);
    if (!keysMethod)
        return false;
    JSValue keys = call(globalObject, keysMethod);
    RETURN_IF_EXCEPTION(scope, false);
    MarkedArgumentBuffer collected;
    collect(globalObject, keys, collected);
    RETURN_IF_EXCEPTION(scope, false);
    for (unsigned i = 0; i < collected.size(); ++i) {
        JSValue value = getItem(globalObject, mapping, collected.at(i));
        RETURN_IF_EXCEPTION(scope, false);
        function(collected.at(i), value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

// {**mapping}
PYTHON_RUNTIME_FUNCTION(dictUpdate)
{
    PROLOGUE();
    auto* dict = uncheckedDowncast<PyDict>(argument(0).asCell());
    bool isMapping = forEachItem(globalObject, argument(1), [&] (JSValue key, JSValue value) {
        dict->set(globalObject, key, value);
    });
    RETURN_IF_EXCEPTION(scope, { });
    if (!isMapping)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate('\'', typeName(globalObject, argument(1)), "' object is not a mapping"_s)));
    return JSValue::encode(jsUndefined());
}

// f"{value!r:specification}"
PYTHON_RUNTIME_FUNCTION(formatValue)
{
    PROLOGUE();
    JSValue value = argument(0);
    switch (argument(1).asInt32()) {
    // What str() or repr() gives is then formatted in its turn, and if it is of a class derived from str that has a __format__() of its own, that is called.
    case 's':
        value = strObject(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        break;
    case 'r':
    case 'a':
        value = reprObject(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (argument(1).asInt32() == 'a') {
            if (String text = stringIn(value)->value(globalObject); !text.containsOnlyASCII())
                value = strOrMemoryError(globalObject, escapeNonASCII(text));
            RETURN_IF_EXCEPTION(scope, { });
        }
        break;
    default:
        break;
    }
    bool hasSpecification = callFrame->argumentCount() > 2;
    if (!hasSpecification && value.isString())
        return JSValue::encode(value);
    // It is one that was put together from what was written. In a tree that a program made it can be anything.
    JSString* given = hasSpecification ? stringIn(argument(2)) : nullptr;
    if (hasSpecification && !given)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, concatenate("Format specifier must be a string, not "_s, typeName(globalObject, argument(2)))));
    String specification = given ? String(given->value(globalObject).data) : emptyString();
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(format(globalObject, value, specification)));
}

// ---- Calls

// What something that is called is called, where what is wrong is how it was called: _PyObject_FunctionStr() of CPython's Objects/object.c.
static String describeCallable(JSGlobalObject* globalObject, JSValue callable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSValue qualifiedName = getAttributeIfPresent(globalObject, callable, names.dunder_qualname);
    RETURN_IF_EXCEPTION(scope, { });
    if (!qualifiedName)
        RELEASE_AND_RETURN(scope, str(globalObject, callable));
    String name = str(globalObject, qualifiedName);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue module = getAttributeIfPresent(globalObject, callable, names.dunder_module);
    RETURN_IF_EXCEPTION(scope, { });
    if (!module || isNone(module))
        return concatenate(name, "()"_s);
    String moduleName = str(globalObject, module);
    RETURN_IF_EXCEPTION(scope, { });
    if (module.isString() && moduleName == "builtins"_s)
        return concatenate(name, "()"_s);
    return concatenate(moduleName, '.', name, "()"_s);
}

// callKeywords(function, names, positional arguments..., values of the keywords...)
PYTHON_RUNTIME_FUNCTION(callKeywords)
{
    PROLOGUE();
    MarkedArgumentBuffer arguments;
    for (unsigned i = 2; i < callFrame->argumentCount(); ++i)
        arguments.append(argument(i));
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, argument(0), arguments, uncheckedDowncast<KeywordNames>(argument(1).asCell()), callFrame->thisValue())));
}

// callSpread(function, a list of the positional arguments, a dict of the keywords or None)
PYTHON_RUNTIME_FUNCTION(callSpread)
{
    PROLOGUE();
    JSArray* positional = asList(argument(1));
    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < positional->length(); ++i)
        arguments.append(positional->getIndexQuickly(i));
    if (arguments.hasOverflowed()) [[unlikely]]
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    if (isNone(argument(2)))
        RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, argument(0), arguments, nullptr, callFrame->thisValue())));

    auto* keywords = uncheckedDowncast<PyDict>(argument(2).asCell());
    KeywordNames* names = KeywordNames::create(vm, CopyOnWriteArrayWithContiguous, keywords->size());
    unsigned i = 0;
    keywords->forEach(globalObject, [&] (JSValue key, JSValue value) {
        names->setIndex(vm, i++, key);
        arguments.append(value);
        return true;
    });
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, argument(0), arguments, names, callFrame->thisValue())));
}

static bool addKeywordArgument(JSGlobalObject* globalObject, PyDict* keywords, JSValue name, JSValue value, JSValue callable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!name.isString()) {
        raiseTypeError(globalObject, scope, "keywords must be strings"_s);
        return false;
    }
    bool wasAdded;
    keywords->add(globalObject, name, value, &wasAdded, false);
    RETURN_IF_EXCEPTION(scope, false);
    if (!wasAdded) {
        raiseTypeError(globalObject, scope, concatenate(describeCallable(globalObject, callable), " got multiple values for keyword argument '"_s, asString(name)->value(globalObject).data, '\''));
        return false;
    }
    return true;
}

PYTHON_RUNTIME_FUNCTION(addKeyword)
{
    PROLOGUE();
    scope.release();
    addKeywordArgument(globalObject, uncheckedDowncast<PyDict>(argument(0).asCell()), argument(1), argument(2), argument(3));
    return JSValue::encode(jsUndefined());
}

// f(**mapping)
PYTHON_RUNTIME_FUNCTION(addKeywords)
{
    PROLOGUE();
    auto* keywords = uncheckedDowncast<PyDict>(argument(0).asCell());
    bool isMapping = forEachItem(globalObject, argument(1), [&] (JSValue key, JSValue value) {
        addKeywordArgument(globalObject, keywords, key, value, argument(2));
    });
    RETURN_IF_EXCEPTION(scope, { });
    if (!isMapping)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(describeCallable(globalObject, argument(2)), " argument after ** must be a mapping, not "_s, typeName(globalObject, argument(1)))));
    return JSValue::encode(jsUndefined());
}

static const FunctionInfo& infoOf(JSFunction* function)
{
    return *function->jsExecutable()->unlinkedExecutable()->pythonInfo();
}

// Says what is wrong with a call that gave so many arguments by position and no keywords.
static void raiseForArgumentCount(JSGlobalObject* globalObject, JSFunction* function, unsigned given)
{
    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < given; ++i)
        arguments.append(jsUndefined());
    MarkedArgumentBuffer bound;
    bindArguments(globalObject, function, infoOf(function), arguments, nullptr, bound);
}

// A function was given fewer arguments by position than it has such parameters, or too many. A tuple with the default of each
// parameter in that parameter's place, if that will do.
PYTHON_RUNTIME_FUNCTION(defaultsFor)
{
    PROLOGUE();
    auto& names = vm.pythonNames();
    auto* function = uncheckedDowncast<JSFunction>(argument(0).asCell());
    const FunctionInfo& info = infoOf(function);
    unsigned given = argument(1).asInt32();

    JSValue defaultsValue = function->getDirect(vm, names.private_defaults);
    PyTuple* defaults = defaultsValue ? uncheckedDowncast<PyTuple>(defaultsValue.asCell()) : nullptr;
    unsigned defaultCount = defaults ? std::min(defaults->length(), info.positionalCount) : 0;
    if (given > info.positionalCount || given + defaultCount < info.positionalCount) {
        raiseForArgumentCount(globalObject, function, given);
        ASSERT(scope.exception());
        return { };
    }
    if (JSValue aligned = function->getDirect(vm, names.private_alignedDefaults))
        return JSValue::encode(aligned);
    PyTuple* aligned = PyTuple::create(globalObject, info.positionalCount);
    for (unsigned i = 0; i < defaultCount; ++i)
        aligned->initializeAt(vm, info.positionalCount - defaultCount + i, defaults->at(defaults->length() - defaultCount + i));
    function->putDirect(vm, names.private_alignedDefaults, aligned);
    return JSValue::encode(aligned);
}

// The default of a parameter that can only be given by keyword, in a call with no keywords.
PYTHON_RUNTIME_FUNCTION(keywordDefault)
{
    PROLOGUE();
    auto* function = uncheckedDowncast<JSFunction>(argument(0).asCell());
    const FunctionInfo& info = infoOf(function);
    JSValue defaults = function->getDirect(vm, vm.pythonNames().private_kwdefaults);
    if (defaults) {
        if (JSValue value = uncheckedDowncast<PyDict>(defaults.asCell())->getString(globalObject, info.parameterNames[argument(1).asInt32()].string()))
            return JSValue::encode(value);
    }
    raiseForArgumentCount(globalObject, function, info.positionalCount);
    ASSERT(scope.exception());
    return { };
}

// ---- yield from

static JSValue stopIterationValue(JSGlobalObject* globalObject, JSValue exception)
{
    VM& vm = globalObject->vm();
    JSValue arguments = exception.isObject() ? asObject(exception)->getDirect(vm, vm.pythonNames().private_args) : JSValue();
    if (!arguments || !isTuple(arguments) || !uncheckedDowncast<PyTuple>(arguments.asCell())->length())
        return jsUndefined();
    return uncheckedDowncast<PyTuple>(arguments.asCell())->at(0);
}

// If StopIteration has been raised, it is caught and what it carries is given. Otherwise the result is empty.
JSValue catchStopIteration(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    Exception* exception = vm.exceptionForInspection();
    if (!exception)
        return { };
    JSValue value = exception->value();
    if (!catchException(globalObject, BuiltinType::StopIteration))
        return { };
    if (vm.isPythonWatched()) [[unlikely]]
        noteCaughtStopIteration(globalObject, value);
    return stopIterationValue(globalObject, value);
}

// One turn of `yield from iterator`: passes on what was sent to or thrown into this generator, and gives what to yield next. Gives
// the marker when the iterator is done, and then takeReturnValue() has what it returned.
PYTHON_RUNTIME_FUNCTION(yieldFromStep)
{
    PROLOGUE();
    JSValue iterator = argument(0);
    JSValue received = argument(1);
    bool wasThrown = argument(2).asBoolean();
    // So that gi_yieldfrom can say what this generator is waiting on.
    asObject(argument(3))->putDirect(vm, vm.pythonNames().private_yieldFrom, iterator);

    auto finish = [&] (JSValue returned) {
        asObject(argument(3))->putDirect(vm, vm.pythonNames().private_yieldFrom, jsUndefined());
        realm->setReturnValue(vm, returned);
        return JSValue::encode(realm->boundArgumentsMarker());
    };
    JSValue returned;
    if (vm.isPythonWatched()) [[unlikely]]
        forgetCaughtStopIteration(globalObject);
    JSValue yielded;
    bool cameOfThrow = false;
    if (!wasThrown && received.isCell() && received.asCell()->type() == JSCellButterflyType) [[unlikely]] {
        cameOfThrow = true;
        // Something was thrown into the iterator while this generator waited, and that was the end of the iterator: see resumeGenerator(). This is what came of it.
        auto* outcome = uncheckedDowncast<JSCellButterfly>(received.asCell());
        if (outcome->get(0).asBoolean())
            throwException(globalObject, scope, outcome->get(1));
        else
            returned = outcome->get(1);
    } else
        yielded = stepIterator(globalObject, iterator, received, wasThrown, returned);
    if (scope.exception()) [[unlikely]] {
        // It is not waiting on it any more, whatever it does about this.
        asObject(argument(3))->putDirect(vm, vm.pythonNames().private_yieldFrom, jsUndefined());
        return { };
    }
    if (!yielded && vm.isPythonWatched()) [[unlikely]] {
        if (CallFrame* caller = callerOf(callFrame)) {
            if (cameOfThrow)
                generatorHasReturnedOnThrowTo(globalObject, caller, caller->bytecodeIndex(), returned ? returned : jsUndefined());
            else if (iterator.isCell() && iterator.asCell()->type() == JSGeneratorType)
                generatorHasReturnedTo(globalObject, caller, caller->bytecodeIndex(), returned ? returned : jsUndefined());
            else
                tellOfCaughtStopIteration(globalObject, caller, caller->bytecodeIndex());
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return yielded ? JSValue::encode(yielded) : finish(returned);
}

PYTHON_RUNTIME_FUNCTION(takeReturnValue)
{
    UNUSED_PARAM(callFrame);
    JSValue value = globalObject->pyRealm()->takeReturnValue();
    return JSValue::encode(value ? value : jsUndefined());
}

// ---- Free variables that a function has as cells. See FunctionInfo::variablesGivenAsCells. The marker is for there being nothing in one.

PYTHON_RUNTIME_FUNCTION(runtimeCellGet)
{
    JSValue value = contentsOfCell(callFrame->uncheckedArgument(0));
    return JSValue::encode(value ? value : JSValue(globalObject->pyRealm()->boundArgumentsMarker()));
}

PYTHON_RUNTIME_FUNCTION(runtimeCellSet)
{
    JSValue value = callFrame->uncheckedArgument(1);
    setContentsOfCell(globalObject->vm(), callFrame->uncheckedArgument(0), value == JSValue(globalObject->pyRealm()->boundArgumentsMarker()) ? JSValue() : value);
    return JSValue::encode(jsUndefined());
}

// ---- async

PYTHON_RUNTIME_FUNCTION(runtimeNewCoroutine)
{
    bool isAsyncGenerator = callFrame->uncheckedArgument(1).asBoolean();
    JSGenerator* coroutine = newCoroutine(globalObject, callFrame->uncheckedArgument(0), isAsyncGenerator);
    int depth = globalObject->pyRealm()->coroutineOriginTrackingDepth;
    if (!depth || isAsyncGenerator) [[likely]]
        return JSValue::encode(coroutine);

    // sys.set_coroutine_origin_tracking_depth(): where it was made, for whoever finds that it was never awaited. Not the frame of the function itself, which has not begun.
    VM& vm = globalObject->vm();
    MarkedArgumentBuffer origin;
    for (CallFrame* frame = callerOf(callFrame); frame && depth; frame = callerOf(frame), --depth) {
        PyFrame* object = PyFrame::forCallFrame(vm, frame);
        origin.append(PyTuple::create(globalObject, { jsString(vm, object->executable()->source().provider()->sourceURL()), jsNumber(object->line(vm)), jsString(vm, object->functionInfo().name.string()) }));
    }
    coroutine->putDirect(vm, vm.pythonNames().private_origin, PyTuple::createFromArguments(globalObject, origin));
    return JSValue::encode(coroutine);
}

PYTHON_RUNTIME_FUNCTION(runtimeWrapAsyncYield)
{
    return JSValue::encode(wrapAsyncYield(globalObject, callFrame->uncheckedArgument(0)));
}

PYTHON_RUNTIME_FUNCTION(runtimeGetAwaitable)
{
    return JSValue::encode(getAwaitable(globalObject, callFrame->uncheckedArgument(0), callFrame->uncheckedArgument(1).asInt32()));
}

PYTHON_RUNTIME_FUNCTION(runtimeGetAsyncIterator)
{
    return JSValue::encode(getAsyncIterator(globalObject, callFrame->uncheckedArgument(0)));
}

PYTHON_RUNTIME_FUNCTION(runtimeGetAsyncNext)
{
    return JSValue::encode(getAsyncNext(globalObject, callFrame->uncheckedArgument(0)));
}

// What `yield from` iterates.
PYTHON_RUNTIME_FUNCTION(getYieldFromIterator)
{
    PROLOGUE();
    if (typeOf(globalObject, argument(0)) == realm->typeCoroutine()) {
        // A generator that can itself be awaited can hand on to one.
        if (argument(1).asBoolean())
            return JSValue::encode(argument(0));
        return JSValue::encode(raiseTypeError(globalObject, scope, "cannot 'yield from' a coroutine object in a non-coroutine generator"_s));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(getIterator(globalObject, argument(0))));
}

// ---- Exceptions

PYTHON_RUNTIME_FUNCTION(raiseAssertionError)
{
    PROLOGUE();
    return JSValue::encode(raise(globalObject, scope, BuiltinType::AssertionError, isMarker(realm, argument(0)) ? JSValue() : argument(0)));
}

PYTHON_RUNTIME_FUNCTION(reraise)
{
    PROLOGUE();
    Exception* handled = realm->handledThrown();
    if (!handled)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "No active exception to reraise"_s));
    throwException(globalObject, scope, JSValue(handled));
    return { };
}

// An exception, from what may be the class of one.
static JSValue normalizeException(JSGlobalObject* globalObject, JSValue value, ASCIILiteral complaint)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isClass(value) && asType(value)->isExceptionType()) {
        JSValue instance = call(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (!typeOf(globalObject, instance)->isExceptionType()) {
            String classText = repr(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            String typeText = repr(globalObject, typeOf(globalObject, instance));
            RETURN_IF_EXCEPTION(scope, { });
            return raiseTypeError(globalObject, scope, concatenate("calling "_s, classText, " should have returned an instance of BaseException, not "_s, typeText));
        }
        return instance;
    }
    if (!typeOf(globalObject, value)->isExceptionType())
        return raiseTypeError(globalObject, scope, complaint);
    return value;
}

// raise exception from cause. The cause is the marker if there is no `from`.
PYTHON_RUNTIME_FUNCTION(runtimeRaise)
{
    PROLOGUE();
    auto& names = vm.pythonNames();
    JSValue exception = normalizeException(globalObject, argument(0), "exceptions must derive from BaseException"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* object = asObject(exception);

    if (!isMarker(realm, argument(1))) {
        JSValue cause = argument(1);
        if (!isNone(cause)) {
            cause = normalizeException(globalObject, cause, "exception causes must derive from BaseException"_s);
            RETURN_IF_EXCEPTION(scope, { });
        }
        object->putDirect(vm, names.private_cause, cause);
        object->putDirect(vm, names.private_suppressContext, jsBoolean(true));
    }
    setContext(globalObject, object);
    throwException(globalObject, scope, exception);
    return { };
}

// What these are given and give back is an exception as it was thrown: what `catch` gives besides the value.
static JSValue previousOrMarker(PyRealm* realm)
{
    Exception* previous = realm->ownHandledException();
    return previous ? JSValue(previous) : JSValue(realm->boundArgumentsMarker());
}

PYTHON_RUNTIME_FUNCTION(pushHandledException)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    JSValue previous = previousOrMarker(realm);
    realm->setOwnHandledException(vm, uncheckedDowncast<Exception>(argument(0).asCell()));
    return JSValue::encode(previous);
}

// pushIfThrown(whether a try was left by an exception, what with): in a `finally` that an exception is passing through, that exception is being handled.
PYTHON_RUNTIME_FUNCTION(pushIfThrown)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    JSValue previous = previousOrMarker(realm);
    if (argument(0).asBoolean())
        realm->setOwnHandledException(vm, uncheckedDowncast<Exception>(argument(1).asCell()));
    return JSValue::encode(previous);
}

PYTHON_RUNTIME_FUNCTION(popHandledException)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    realm->setOwnHandledException(vm, isMarker(realm, argument(0)) ? nullptr : uncheckedDowncast<Exception>(argument(0).asCell()));
    return JSValue::encode(jsUndefined());
}

// An exception that was made and not caught, in the form that one that was caught has: the part of a group that an `except*` clause handles.
PYTHON_RUNTIME_FUNCTION(asThrown)
{
    PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(Exception::create(vm, argument(0)));
}

// ---- with

static JSValue loadContextMethod(JSGlobalObject* globalObject, JSValue manager, const Identifier& name, ASCIILiteral spelled, bool isAsync)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue self;
    JSValue method = lookupSpecial(globalObject, manager, name, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method) {
        auto& names = vm.pythonNames();
        PyType* type = typeOf(globalObject, manager);
        // type_has_special_method(): it has something by the name, and it is something that could be a method.
        auto has = [&] (const Identifier& other) {
            JSValue found = type->lookup(vm, other);
            return found && hasGet(globalObject, found);
        };
        bool hasOther = isAsync ? has(names.dunder_enter) && has(names.dunder_exit) : has(names.dunder_aenter) && has(names.dunder_aexit);
        if (hasOther)
            return raiseTypeError(globalObject, scope, concatenate('\'', type->nameString(globalObject), isAsync ? "' object does not support the asynchronous context manager protocol (missed "_s : "' object does not support the context manager protocol (missed "_s, spelled,
                isAsync ? " method) but it supports the context manager protocol. Did you mean to use 'with'?"_s : " method) but it supports the asynchronous context manager protocol. Did you mean to use 'async with'?"_s));
    }
    if (!method)
        return raiseTypeError(globalObject, scope, concatenate('\'', typeName(globalObject, manager), isAsync ? "' object does not support the asynchronous context manager protocol (missed "_s : "' object does not support the context manager protocol (missed "_s, spelled, " method)"_s));
    return self ? JSValue(PyBoundMethod::create(globalObject, method, self)) : method;
}

// What `using` would call when it is done with something of JavaScript's: its [Symbol.dispose], or for `await using` its [Symbol.asyncDispose] or failing
// that its [Symbol.dispose]. It is found as the engine finds it. Empty if it is not JavaScript's, or has none.
static JSValue loadDisposeMethod(JSGlobalObject* globalObject, JSValue manager, bool isAsync)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!manager.isObject() || !typeOf(globalObject, manager)->hasFlag(PyType::IsJavaScript))
        return { };
    JSValue method = isAsync ? asObject(manager)->get(globalObject, vm.propertyNames->asyncDisposeSymbol) : jsUndefined();
    RETURN_IF_EXCEPTION(scope, { });
    if (method.isUndefinedOrNull()) {
        method = asObject(manager)->get(globalObject, vm.propertyNames->disposeSymbol);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (method.isUndefinedOrNull())
        return { };
    JSValue find = globalObject->linkTimeConstant(isAsync ? LinkTimeConstant::getAsyncDisposeMethod : LinkTimeConstant::getDisposeMethod);
    method = call(globalObject, find, manager);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isAsync)
        RELEASE_AND_RETURN(scope, JSBoundFunction::bind(globalObject, vm.topCallFrame, asObject(method), manager, ArgList()));
    // JavaScript waits for whatever it gives, and Python for what can be awaited.
    MarkedArgumentBuffer bound;
    bound.append(method);
    RELEASE_AND_RETURN(scope, JSBoundFunction::bind(globalObject, vm.topCallFrame, globalObject->pyRealm()->javaScriptFunction("disposeAndWait"_s), manager, bound));
}

PYTHON_RUNTIME_FUNCTION(loadExit)
{
    PROLOGUE();
    auto& names = vm.pythonNames();
    bool isAsync = argument(1).asBoolean();
    if (JSValue dispose = loadDisposeMethod(globalObject, argument(0), isAsync); dispose || scope.exception())
        return JSValue::encode(dispose);
    JSValue exit = loadContextMethod(globalObject, argument(0), isAsync ? names.dunder_aexit : names.dunder_exit, isAsync ? "__aexit__"_s : "__exit__"_s, isAsync);
    RETURN_IF_EXCEPTION(scope, { });
    loadContextMethod(globalObject, argument(0), isAsync ? names.dunder_aenter : names.dunder_enter, isAsync ? "__aenter__"_s : "__enter__"_s, isAsync);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(exit);
}

PYTHON_RUNTIME_FUNCTION(callEnter)
{
    PROLOGUE();
    // There is nothing to entering what `using` can be used with: it is what it is from the start.
    if (typeOf(globalObject, argument(0))->hasFlag(PyType::IsJavaScript))
        return JSValue::encode(argument(1).asBoolean() ? awaitableFor(globalObject, argument(0)) : argument(0));
    JSValue self;
    JSValue method = lookupSpecial(globalObject, argument(0), argument(1).asBoolean() ? vm.pythonNames().dunder_aenter : vm.pythonNames().dunder_enter, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
}

// callExit(__exit__, the exception or None): what it gives, which if true says that the exception has been dealt with.
PYTHON_RUNTIME_FUNCTION(callExit)
{
    PROLOGUE();
    JSValue exception = argument(1);
    if (isNone(exception))
        RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, argument(0), jsUndefined(), jsUndefined(), jsUndefined())));
    JSValue traceback = exception.isObject() ? asObject(exception)->getDirect(vm, vm.pythonNames().private_traceback) : JSValue();
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, argument(0), typeOf(globalObject, exception)->object(), exception, traceback ? traceback : jsUndefined())));
}

// ---- match

// matchSequence(subject, count, hasStar): whether it is a sequence with so many elements, or at least so many.
PYTHON_RUNTIME_FUNCTION(matchSequence)
{
    PROLOGUE();
    if (!typeOf(globalObject, argument(0))->hasFlag(PyType::IsSequence))
        return JSValue::encode(jsBoolean(false));
    int64_t size = length(globalObject, argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t count = argument(1).asInt32();
    return JSValue::encode(jsBoolean(argument(2).asBoolean() ? size >= count : size == count));
}

// matchMapping(subject, count): whether it is a mapping with at least so many items.
PYTHON_RUNTIME_FUNCTION(matchMapping)
{
    PROLOGUE();
    if (!typeOf(globalObject, argument(0))->hasFlag(PyType::IsMapping))
        return JSValue::encode(jsBoolean(false));
    int64_t count = argument(1).asInt32();
    if (!count)
        return JSValue::encode(jsBoolean(true));
    int64_t size = length(globalObject, argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(size >= count));
}

// matchKeys(subject, keys): a tuple of what the mapping has for each of the keys, or the marker if it lacks one.
PYTHON_RUNTIME_FUNCTION(matchKeys)
{
    PROLOGUE();
    JSValue subject = argument(0);
    auto* keys = uncheckedDowncast<PyTuple>(argument(1).asCell());
    // By get(), so that a defaultdict is not made to invent what it does not have.
    JSValue self;
    JSValue get = loadMethod(globalObject, subject, Identifier::fromString(vm, "get"_s), self);
    RETURN_IF_EXCEPTION(scope, { });
    PySet* seen = PySet::create(globalObject);
    PyTuple* values = PyTuple::create(globalObject, keys->length());
    JSValue missing = realm->boundArgumentsMarker();
    for (unsigned i = 0; i < keys->length(); ++i) {
        JSValue key = keys->at(i);
        bool wasAdded;
        seen->add(globalObject, key, &wasAdded);
        RETURN_IF_EXCEPTION(scope, { });
        if (!wasAdded) {
            String text = repr(globalObject, key);
            RETURN_IF_EXCEPTION(scope, { });
            return JSValue::encode(raiseValueError(globalObject, scope, concatenate("mapping pattern checks duplicate key ("_s, text, ')')));
        }
        JSValue value = self ? call(globalObject, get, self, key, missing) : call(globalObject, get, key, missing);
        RETURN_IF_EXCEPTION(scope, { });
        if (value == missing)
            return JSValue::encode(missing);
        values->initializeAt(vm, i, value);
    }
    return JSValue::encode(values);
}

// matchRest(subject, keys): a dict of what is in the mapping under other keys than those.
PYTHON_RUNTIME_FUNCTION(matchRest)
{
    PROLOGUE();
    PyDict* rest = PyDict::create(globalObject);
    forEachItem(globalObject, argument(0), [&] (JSValue key, JSValue value) {
        rest->set(globalObject, key, value);
    });
    RETURN_IF_EXCEPTION(scope, { });
    for (auto& key : uncheckedDowncast<PyTuple>(argument(1).asCell())->span()) {
        rest->remove(globalObject, key.get());
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(rest);
}

// matchClass(subject, class, how many patterns are by position, the names of the others): a tuple of the attributes for the patterns to be
// matched against, or the marker if it is not an instance or lacks one of them.
PYTHON_RUNTIME_FUNCTION(matchClass)
{
    PROLOGUE();
    auto& names = vm.pythonNames();
    JSValue subject = argument(0);
    if (!isClass(argument(1)))
        return JSValue::encode(raiseTypeError(globalObject, scope, "called match pattern must be a class"_s));
    PyType* type = asType(argument(1));
    unsigned positional = argument(2).asInt32();
    auto* keywords = uncheckedDowncast<KeywordNames>(argument(3).asCell());
    JSValue mismatch = realm->boundArgumentsMarker();

    bool isInstance = isInstanceOf(globalObject, subject, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInstance)
        return JSValue::encode(mismatch);

    PyTuple* attributes = PyTuple::create(globalObject, positional + keywords->length());
    Vector<String, 8> seen;
    // False if there is no match, or it raised.
    auto take = [&] (unsigned index, JSValue name) -> bool {
        String text = asString(name)->value(globalObject);
        if (seen.contains(text)) {
            raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), "() got multiple sub-patterns for attribute "_s, reprOfString(text)));
            return false;
        }
        seen.append(text);
        JSValue value = getAttributeIfPresent(globalObject, subject, Identifier::fromString(vm, text));
        RETURN_IF_EXCEPTION(scope, false);
        if (!value)
            return false;
        attributes->initializeAt(vm, index, value);
        return true;
    };

    if (positional) {
        JSValue matchArguments = getAttributeIfPresent(globalObject, type, names.dunder_match_args);
        RETURN_IF_EXCEPTION(scope, { });
        bool matchesSelf = false;
        unsigned allowed;
        if (matchArguments) {
            if (!isTuple(matchArguments))
                return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), ".__match_args__ must be a tuple (got "_s, typeName(globalObject, matchArguments), ')')));
            allowed = uncheckedDowncast<PyTuple>(matchArguments.asCell())->length();
        } else {
            matchesSelf = type->hasFlag(PyType::MatchesSelf);
            allowed = matchesSelf;
        }
        if (allowed < positional)
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), "() accepts "_s, allowed, " positional sub-pattern"_s, allowed == 1 ? ""_s : "s"_s, " ("_s, positional, " given)"_s)));
        if (matchesSelf)
            attributes->initializeAt(vm, 0, subject);
        else {
            for (unsigned i = 0; i < positional; ++i) {
                JSValue name = uncheckedDowncast<PyTuple>(matchArguments.asCell())->at(i);
                if (!name.isString())
                    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("__match_args__ elements must be strings (got "_s, typeName(globalObject, name), ')')));
                bool found = take(i, name);
                RETURN_IF_EXCEPTION(scope, { });
                if (!found)
                    return JSValue::encode(mismatch);
            }
        }
    }
    for (unsigned i = 0; i < keywords->length(); ++i) {
        bool found = take(positional + i, keywords->get(i));
        RETURN_IF_EXCEPTION(scope, { });
        if (!found)
            return JSValue::encode(mismatch);
    }
    return JSValue::encode(attributes);
}

PYTHON_RUNTIME_FUNCTION(runtimeLength)
{
    PROLOGUE();
    int64_t size = length(globalObject, argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, size)));
}

// What is done with what an expression comes to at a prompt: sys.displayhook(value)
PYTHON_RUNTIME_FUNCTION(displayHook)
{
    PROLOGUE();
    JSValue hook = sysAttribute(globalObject, "displayhook"_s);
    if (!hook)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "lost sys.displayhook"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, hook, argument(0))));
}

// ---- Classes and modules

PYTHON_RUNTIME_FUNCTION(runtimeMatchExceptionGroup)
{
    PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(matchExceptionGroup(globalObject, callerOf(callFrame), argument(0), argument(1))));
}

PYTHON_RUNTIME_FUNCTION(runtimePrepareReraiseStar)
{
    PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(prepareReraiseStar(globalObject, argument(0), asList(argument(1)))));
}

// LOAD_BUILD_CLASS
PYTHON_RUNTIME_FUNCTION(loadBuildClass)
{
    PROLOGUE();
    JSValue function = getStoredAttribute(vm, asObject(argument(0)), Identifier::fromString(vm, "__build_class__"_s));
    if (!function)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::NameError, "__build_class__ not found"_s));
    return JSValue::encode(function);
}

PYTHON_RUNTIME_FUNCTION(runtimeBuildClass)
{
    PROLOGUE();
    JSArray* list = asList(argument(2));
    PyTuple* bases = PyTuple::create(globalObject, list->length());
    for (unsigned i = 0; i < list->length(); ++i)
        bases->initializeAt(vm, i, list->getIndexQuickly(i));
    PyDict* keywords = isNone(argument(3)) ? nullptr : uncheckedDowncast<PyDict>(argument(3).asCell());
    RELEASE_AND_RETURN(scope, JSValue::encode(buildClass(globalObject, argument(0), asString(argument(1)), bases, keywords)));
}

PYTHON_RUNTIME_FUNCTION(importName)
{
    PROLOGUE();
    String name = asString(argument(1))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(importModule(globalObject, asObject(argument(0)), name, argument(2), argument(3).asInt32(), argument(4).asBoolean())));
}

PYTHON_RUNTIME_FUNCTION(importFrom)
{
    PROLOGUE();
    auto name = asString(argument(1))->toIdentifier(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = getAttributeIfPresent(globalObject, argument(0), name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    // A module of the package that has not been made an attribute of it yet, as when packages import each other.
    JSValue packageName = getAttributeIfPresent(globalObject, argument(0), vm.pythonNames().dunder_name);
    RETURN_IF_EXCEPTION(scope, { });
    // A name that is no string is no name.
    if (packageName && !packageName.isString())
        packageName = { };
    if (packageName) {
        String package = asString(packageName)->value(globalObject);
        if (JSValue module = uncheckedDowncast<PyDict>(realm->modules())->getString(globalObject, concatenate(package, '.', name.string())))
            return JSValue::encode(module);
    }

    // What follows is the rest of _PyEval_ImportFrom(), which is all about what to say.
    // FIXME: If the module is in the way of one of the standard library's, or may be of some other, it says so. That goes by sys.path, the working directory and
    // sys.stdlib_module_names, and comes with the rest of importing.
    String shownName = repr(globalObject, argument(1));
    RETURN_IF_EXCEPTION(scope, { });
    String shownPackage = repr(globalObject, packageName ? packageName : JSValue(jsString(vm, String("<unknown module name>"_s))));
    RETURN_IF_EXCEPTION(scope, { });
    auto attribute = [&] (JSValue object, ASCIILiteral attributeName) { return getAttributeIfPresent(globalObject, object, Identifier::fromString(vm, attributeName)); };
    JSValue spec = getAttributeIfPresent(globalObject, argument(0), vm.pythonNames().dunder_spec);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue origin;
    bool isInitializing = false;
    if (spec) {
        // _PyModuleSpec_GetFileOrigin()
        JSValue hasLocation = attribute(spec, "has_location"_s);
        RETURN_IF_EXCEPTION(scope, { });
        bool isLocated = hasLocation && isTrue(globalObject, hasLocation);
        RETURN_IF_EXCEPTION(scope, { });
        if (isLocated) {
            origin = attribute(spec, "origin"_s);
            RETURN_IF_EXCEPTION(scope, { });
            if (origin && !origin.isString())
                origin = { };
        }
        // PyModule_GetFilenameObject()
        if (!origin && isInstance(globalObject, argument(0), realm->typeModule())) {
            origin = getStoredAttribute(vm, asObject(argument(0)), vm.pythonNames().dunder_file);
            if (origin && !origin.isString())
                origin = { };
        }
        // _PyModuleSpec_IsInitializing()
        JSValue initializing = attribute(spec, "_initializing"_s);
        RETURN_IF_EXCEPTION(scope, { });
        isInitializing = initializing && isTrue(globalObject, initializing);
        RETURN_IF_EXCEPTION(scope, { });
    }
    String location = origin ? concatenate(" ("_s, asString(origin)->value(globalObject).data, ')') : emptyString();
    String message = isInitializing
        ? concatenate("cannot import name "_s, shownName, " from partially initialized module "_s, shownPackage, " (most likely due to a circular import)"_s, location)
        : concatenate("cannot import name "_s, shownName, " from "_s, shownPackage, origin ? location : String(" (unknown location)"_s));

    // _PyErr_SetImportErrorWithNameFrom()
    JSValue error = call(globalObject, realm->typeImportError(), jsString(vm, message));
    RETURN_IF_EXCEPTION(scope, { });
    auto& names = vm.pythonNames();
    asObject(error)->putDirect(vm, names.field_name, packageName ? packageName : jsUndefined());
    asObject(error)->putDirect(vm, names.field_path, origin ? origin : jsUndefined());
    asObject(error)->putDirect(vm, names.field_nameFrom, argument(1));
    throwException(globalObject, scope, error);
    return { };
}

// from module import *
PYTHON_RUNTIME_FUNCTION(importStar)
{
    PROLOGUE();
    JSObject* globals = asObject(argument(1));
    JSValue all = getAttributeIfPresent(globalObject, argument(0), vm.pythonNames().dunder_all);
    RETURN_IF_EXCEPTION(scope, { });
    if (all) {
        MarkedArgumentBuffer names;
        collect(globalObject, all, names);
        RETURN_IF_EXCEPTION(scope, { });
        for (unsigned i = 0; i < names.size(); ++i) {
            if (!names.at(i).isString())
                return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("Item in __all__ must be str, not "_s, typeName(globalObject, names.at(i)))));
            auto name = asString(names.at(i))->toIdentifier(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue value = getAttribute(globalObject, argument(0), name);
            RETURN_IF_EXCEPTION(scope, { });
            globals->putDirect(vm, name, value);
        }
        return JSValue::encode(jsUndefined());
    }
    JSObject* module = tryModule(globalObject, argument(0));
    if (!module)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::ImportError, "from-import-* object has no __dict__ and no __all__"_s));
    // Everything whose name does not begin with an underscore.
    PropertyNameArrayBuilder properties(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    module->getOwnNonIndexPropertyNames(globalObject, properties, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, { });
    for (auto& name : properties) {
        if (name.string().startsWith('_'))
            continue;
        globals->putDirect(vm, name, module->getDirect(vm, name));
    }
    return JSValue::encode(jsUndefined());
}

JSObject* createRuntimeFunctions(VM& vm, JSGlobalObject* globalObject)
{
    JSObject* object = constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
    auto add = [&] (ASCIILiteral name, NativeFunction function) {
        object->putDirect(vm, Identifier::fromString(vm, name), PyNativeFunction::create(vm, globalObject, 0, String(name), function, PyNativeFunction::Kind::Function, nullptr, 0, ImplementationVisibility::Private));
    };
    add("loadName"_s, loadName);
    add("checkAnnotationFormat"_s, checkAnnotationFormat);
    add("newInterpolation"_s, runtimeNewInterpolation);
    add("newTemplate"_s, runtimeNewTemplate);
    add("newTypeVar"_s, runtimeNewTypeVar);
    add("newParamSpec"_s, runtimeNewParamSpec);
    add("newTypeVarTuple"_s, runtimeNewTypeVarTuple);
    add("setTypeParameterDefault"_s, runtimeSetTypeParameterDefault);
    add("newTypeAlias"_s, runtimeNewTypeAlias);
    add("subscriptGeneric"_s, runtimeSubscriptGeneric);
    add("setUpAnnotations"_s, setUpAnnotations);
    add("loadFromNamespace"_s, loadFromNamespace);
    add("deleteGlobal"_s, deleteGlobal);
    add("deleteName"_s, deleteName);
    add("newBytes"_s, newBytes);
    add("newComplex"_s, newComplex);
    add("newFrozenSet"_s, newFrozenSet);
    add("listOfTuple"_s, listOfTuple);
    add("setOfFrozenSet"_s, setOfFrozenSet);
    add("joinStrings"_s, joinStrings);
    add("newSlice"_s, newSlice);
    add("listExtend"_s, runtimeListExtend);
    add("listAppend"_s, runtimeListAppend);
    add("listToTuple"_s, listToTuple);
    add("setUpdate"_s, setUpdate);
    add("newSet"_s, newSet);
    add("setAdd"_s, setAdd);
    add("newDict"_s, newDict);
    add("dictUpdate"_s, dictUpdate);
    add("formatValue"_s, formatValue);
    add("callKeywords"_s, callKeywords);
    add("callSpread"_s, callSpread);
    add("addKeyword"_s, addKeyword);
    add("addKeywords"_s, addKeywords);
    add("defaultsFor"_s, defaultsFor);
    add("keywordDefault"_s, keywordDefault);
    add("yieldFromStep"_s, yieldFromStep);
    add("takeReturnValue"_s, takeReturnValue);
    add("newCoroutine"_s, runtimeNewCoroutine);
    add("loadBuildClass"_s, loadBuildClass);
    add("matchExceptionGroup"_s, runtimeMatchExceptionGroup);
    add("prepareReraiseStar"_s, runtimePrepareReraiseStar);
    add("cellGet"_s, runtimeCellGet);
    add("cellSet"_s, runtimeCellSet);
    add("wrapAsyncYield"_s, runtimeWrapAsyncYield);
    add("getAwaitable"_s, runtimeGetAwaitable);
    add("getAsyncIterator"_s, runtimeGetAsyncIterator);
    add("getAsyncNext"_s, runtimeGetAsyncNext);
    add("getYieldFromIterator"_s, getYieldFromIterator);
    object->putDirect(vm, Identifier::fromString(vm, "StopAsyncIteration"_s), globalObject->pyRealm()->typeStopAsyncIteration());
    add("raiseAssertionError"_s, raiseAssertionError);
    add("reraise"_s, reraise);
    add("raise"_s, runtimeRaise);
    add("pushHandledException"_s, pushHandledException);
    add("pushIfThrown"_s, pushIfThrown);
    add("popHandledException"_s, popHandledException);
    add("asThrown"_s, asThrown);
    add("loadExit"_s, loadExit);
    add("callEnter"_s, callEnter);
    add("callExit"_s, callExit);
    add("matchSequence"_s, matchSequence);
    add("matchMapping"_s, matchMapping);
    add("matchKeys"_s, matchKeys);
    add("matchRest"_s, matchRest);
    add("matchClass"_s, matchClass);
    add("length"_s, runtimeLength);
    add("displayHook"_s, displayHook);
    add("buildClass"_s, runtimeBuildClass);
    add("importName"_s, importName);
    add("importFrom"_s, importFrom);
    add("importStar"_s, importStar);
    object->putDirect(vm, Identifier::fromString(vm, "Ellipsis"_s), globalObject->pyRealm()->ellipsis());
    return object;
}

} } // namespace JSC::Python
