/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "BigIntConstructor.h"
#include "BooleanConstructor.h"
#include "FunctionPrototype.h"
#include "IteratorOperations.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSPromise.h"
#include "JSSetInlines.h"
#include "JSWeakMapInlines.h"
#include "NumberConstructor.h"
#include "ObjectConstructor.h"
#include "PropertyNameArray.h"
#include "ProxyConstructor.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyGenerator.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyJSClass.h"
#include "PyJSView.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyTypes.h"
#include "StringConstructor.h"
#include "SymbolConstructor.h"

namespace JSC {
namespace Python {

// What the objects and functions of JS are to Python.
//
// Numbers, strings, booleans, arrays, undefined and null are Python values as they are. Any other object is an instance of
// JSObject, or of JSFunction if it can be called: its properties are its attributes, and also its items. (JSPromise, JSIterator
// and JSGenerator are JSObjects that can also be awaited, asked for the next item, sent to and closed.)

static Identifier propertyKeyFor(JSGlobalObject* globalObject, JSString* name)
{
    return name->toIdentifier(globalObject);
}

// Whether the object is the `prototype` of a constructor. What is read off one is JS's spelling of an unbound method:
// String.prototype.toUpperCase.call("abc").
static bool isPrototypeOfConstructor(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = getVM(globalObject);
    JSValue constructor = object->getDirect(vm, vm.propertyNames->constructor);
    if (!constructor || !constructor.isObject())
        return false;
    return asObject(constructor)->getDirect(vm, vm.propertyNames->prototype) == JSValue(object);
}

// A function that is a property is a method: it is called with the object as `this`, as in object.method(). The method of an object is
// one thing, however often it is read: object.method is object.method, and emitter.off(name, object.method) finds what on() was given.
static JSValue bindIfFunction(JSGlobalObject* globalObject, JSObject* object, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (!value.isObject() || isPyObject(value) || PyFunction::is(value) || PyType::is(value) || PyNativeFunction::is(value))
        return value;
    JSObject* function = asObject(value);
    if (!function->isCallable() || function->inherits<JSBoundFunction>())
        return value;
    // A class is not a method of what holds it: js.Promise is Promise itself.
    if (auto* jsFunction = dynamicDowncast<JSFunction>(function); jsFunction && !jsFunction->isHostFunction() && jsFunction->jsExecutable()->isClassConstructorFunction())
        return value;
    // Nor is what a module exports a method of its namespace.
    if (function->inherits<InternalFunction>() || object->type() == ModuleNamespaceObjectType || isPrototypeOfConstructor(globalObject, object))
        return value;
    // object -> function -> bound function, all of it weak: a bound function lasts as long as both.
    auto mapIn = [&](JSValue found, auto&& remember) {
        // JSWeakMap::get() gives undefined for what is not there.
        if (found && found.isCell())
            return uncheckedDowncast<JSWeakMap>(found);
        JSWeakMap* map = JSWeakMap::create(vm, globalObject->weakMapStructure());
        remember(map);
        return map;
    };
    JSWeakMap* objects = mapIn(rt.slot(Runtime::Slot::BoundMethodsOfJS), [&](JSWeakMap* map) { rt.setSlot(Runtime::Slot::BoundMethodsOfJS, map); });
    JSWeakMap* methods = mapIn(objects->get(object), [&](JSWeakMap* map) { objects->set(vm, object, map); });
    if (JSValue bound = methods->get(function); bound.isCell())
        return bound;
    JSBoundFunction* bound = JSBoundFunction::create(vm, globalObject, function, object, ArgList(), 0, nullptr, SourceTaintedOrigin::Untainted);
    if (bound) [[likely]]
        methods->set(vm, function, bound);
    return bound;
}

static constexpr ASCIILiteral namesOfMappingMethods[] = { "keys"_s, "values"_s, "items"_s, "get"_s, "pop"_s, "popitem"_s, "setdefault"_s, "update"_s, "clear"_s };
static bool isNameOfMappingMethod(Runtime&, JSString* name)
{
    String string = stringOf(name);
    for (ASCIILiteral candidate : namesOfMappingMethods) {
        if (string == candidate)
            return true;
    }
    return false;
}

static JSValue jsObjectGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* object = asObject(self);
    Identifier key = propertyKeyFor(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });

    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    bool found = object->getPropertySlot(globalObject, key, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (found) {
        JSValue value = slot.getValue(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, bindIfFunction(globalObject, object, value));
    }
    // A function with properties of its own is still that when it has been taken from an object as a method.
    if (auto* bound = dynamicDowncast<JSBoundFunction>(object)) {
        bool targetHasIt = bound->targetFunction()->hasProperty(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (targetHasIt)
            RELEASE_AND_RETURN(scope, jsObjectGetAttribute(globalObject, bound->targetFunction(), nameValue));
    }
    // keys(), get(), update() and the like are for an object that is a mapping of its properties. What JS iterates over otherwise is not:
    // dict(iterator) takes pairs from it.
    if (isNameOfMappingMethod(rt, name)) {
        bool isIterable = hasIteratorMethod(globalObject, object);
        RETURN_IF_EXCEPTION(scope, { });
        if (isIterable)
            return raiseAttributeError(globalObject, scope, self, name, makeString('\'', typeName(globalObject, self), "' object has no attribute '"_s, stringOf(name), '\''));
    }
    // vars(object), function.__dict__.update(): its attributes are its properties, and it is the mapping of those itself. (Not a
    // descriptor of the type: who goes past __getattribute__, as inspect.getattr_static() does, expects a dict.)
    if (WTF::equal(name->tryGetValueImpl(), rt.names.dDict()->tryGetValueImpl()))
        return self;
    // What every Python object has: __class__, __dir__, and so on.
    RELEASE_AND_RETURN(scope, genericGetAttr(globalObject, self, name));
}

JSValue attributeFromPrototypeOfJS(JSGlobalObject* globalObject, JSObject* prototype, JSValue object, JSString* name)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier key = propertyKeyFor(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    bool found = prototype->getPropertySlot(globalObject, key, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found)
        return { };
    JSValue value = slot.getValue(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, bindIfFunction(globalObject, asObject(object), value));
}

static JSValue jsObjectSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue nameValue, JSValue value)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    Identifier key = propertyKeyFor(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    PutPropertySlot slot(self, true);
    asObject(self)->methodTable()->put(asObject(self), globalObject, key, value, slot);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue jsObjectDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    Identifier key = propertyKeyFor(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* object = asObject(self);
    bool had = object->hasProperty(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!had)
        return raiseAttributeError(globalObject, scope, self, name, makeString('\'', typeName(globalObject, self), "' object has no attribute '"_s, stringOf(name), '\''));
    bool isDeleted = JSCell::deleteProperty(object, globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    // As in strict code.
    if (!isDeleted) {
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
        return { };
    }
    return jsUndefined();
}

// object[key]: a str or an int is the name of a property. False: a TypeError.
static bool propertyKeyForItem(JSGlobalObject* globalObject, JSValue key, Identifier& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isStr(key)) {
        result = propertyKeyFor(globalObject, strValue(key));
        return !scope.exception();
    }
    if (isInt(key)) {
        String text = intToString(globalObject, intValue(key));
        RETURN_IF_EXCEPTION(scope, false);
        result = Identifier::fromString(vm, text);
        return true;
    }
    if (key.isSymbol()) {
        result = Identifier::fromUid(asSymbol(key)->privateName());
        return true;
    }
    raiseTypeError(globalObject, scope, "the keys of a JavaScript object are str or int, not "_s, typeName(globalObject, key));
    return false;
}

// The items of an object are its properties. Those of a Map are its entries: that is the mapping of JS whose keys can be anything.
static JSValue jsObjectGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* map = dynamicDowncast<JSMap>(self)) {
        bool found = map->has(globalObject, valueForJS(key));
        RETURN_IF_EXCEPTION(scope, { });
        if (!found)
            return raiseKeyError(globalObject, scope, key);
        RELEASE_AND_RETURN(scope, map->get(globalObject, valueForJS(key)));
    }
    Identifier name;
    if (failed(scope, propertyKeyForItem(globalObject, key, name)))
        return { };
    JSObject* object = asObject(self);
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    bool found = object->getPropertySlot(globalObject, name, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found)
        return raiseKeyError(globalObject, scope, key);
    RELEASE_AND_RETURN(scope, slot.getValue(globalObject, name));
}

static JSValue jsObjectSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* map = dynamicDowncast<JSMap>(self)) {
        map->set(globalObject, valueForJS(key), valueForJS(value));
        RETURN_IF_EXCEPTION(scope, { });
        return jsUndefined();
    }
    Identifier name;
    if (failed(scope, propertyKeyForItem(globalObject, key, name)))
        return { };
    PutPropertySlot slot(self, true);
    asObject(self)->methodTable()->put(asObject(self), globalObject, name, value, slot);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue jsObjectDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* map = dynamicDowncast<JSMap>(self)) {
        bool had = map->remove(globalObject, valueForJS(key));
        RETURN_IF_EXCEPTION(scope, { });
        if (!had)
            return raiseKeyError(globalObject, scope, key);
        return jsUndefined();
    }
    Identifier name;
    if (failed(scope, propertyKeyForItem(globalObject, key, name)))
        return { };
    JSObject* object = asObject(self);
    bool had = object->hasProperty(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!had)
        return raiseKeyError(globalObject, scope, key);
    bool isDeleted = JSCell::deleteProperty(object, globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isDeleted) {
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
        return { };
    }
    return jsUndefined();
}

static JSValue jsObjectContains(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* map = dynamicDowncast<JSMap>(self))
        RELEASE_AND_RETURN(scope, jsBoolean(map->has(globalObject, valueForJS(key))));
    if (auto* set = dynamicDowncast<JSSet>(self))
        RELEASE_AND_RETURN(scope, jsBoolean(set->has(globalObject, valueForJS(key))));
    Identifier name;
    if (failed(scope, propertyKeyForItem(globalObject, key, name))) {
        takePendingException(globalObject);
        return jsBoolean(false);
    }
    bool has = asObject(self)->hasProperty(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(has);
}

static JSArray* ownPropertyNames(JSGlobalObject* globalObject, JSObject* object, DontEnumPropertiesMode mode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, globalObject, names, mode);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (auto& name : names) {
        listAppend(globalObject, list, jsString(vm, name.string()));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return list;
}

// ---- Iterators of JS, as iterators of Python ----

// The fields of the records that those are. The iterator is empty once it is exhausted.
enum JSIteratorField : unsigned { JSIteratorObject, JSIteratorNextMethod, JSIteratorThen, JSIteratorFieldCount };

static JSValue jsIteratorSelf(JSGlobalObject*, JSValue self) { return self; }

static JSValue jsIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* record = uncheckedDowncast<PyRecord>(self.asCell());
    JSValue iterator = record->get(JSIteratorObject);
    if (!iterator)
        return raiseStopIteration(globalObject, scope);
    JSValue result = iteratorStep(globalObject, { iterator, record->get(JSIteratorNextMethod) });
    RETURN_IF_EXCEPTION(scope, { });
    if (result.isFalse()) {
        record->set(vm, JSIteratorObject, { });
        return raiseStopIteration(globalObject, scope);
    }
    RELEASE_AND_RETURN(scope, iteratorValue(globalObject, result));
}

static constexpr NativeDef jsIteratorMethods[] = {
    PY_SLOT("__iter__", jsIteratorSelf, "($self, /)"),
    PY_SLOT("__next__", jsIteratorNext, "($self, /)"),
};
static constexpr TypeDef jsIteratorTypeDef { "_js.iterator"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, jsIteratorMethods, { }, { } };

// What the promise that next() returns is fulfilled with, made what `await iterator.__anext__()` is to give.
static JSC_DECLARE_HOST_FUNCTION(jsAsyncIteratorResult);
JSC_DEFINE_HOST_FUNCTION(jsAsyncIteratorResult, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = callFrame->argument(0);
    if (!result.isObject())
        return throwVMTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
    bool isDone = iteratorComplete(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    if (isDone) {
        raise(globalObject, scope, runtime(globalObject).StopAsyncIteration(), String());
        return { };
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(iteratorValue(globalObject, result)));
}

static JSValue jsAsyncIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* record = uncheckedDowncast<PyRecord>(self.asCell());
    MarkedArgumentBuffer noArguments;
    JSValue nextMethod = record->get(JSIteratorNextMethod);
    JSValue result = JSC::call(globalObject, nextMethod, JSC::getCallData(nextMethod), record->get(JSIteratorObject), noArguments);
    RETURN_IF_EXCEPTION(scope, { });
    JSPromise* promise = JSPromise::resolvedPromise(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue then = promise->get(globalObject, vm.propertyNames->then);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    arguments.append(record->get(JSIteratorThen));
    RELEASE_AND_RETURN(scope, JSC::call(globalObject, then, JSC::getCallData(then), promise, arguments));
}

static constexpr NativeDef jsAsyncIteratorMethods[] = {
    PY_SLOT("__aiter__", jsIteratorSelf, "($self, /)"),
    PY_SLOT("__anext__", jsAsyncIteratorNext, "($self, /)"),
};
static constexpr TypeDef jsAsyncIteratorTypeDef { "_js.async_iterator"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, jsAsyncIteratorMethods, { }, { } };

// What JS iterates, if the object is iterable there; otherwise the names of its properties, as for a dict.
static JSValue jsObjectIter(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(self);
    bool isIterable = hasIteratorMethod(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isIterable) {
        JSArray* names = ownPropertyNames(globalObject, object, DontEnumPropertiesMode::Exclude);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, getIter(globalObject, names));
    }
    IterationRecord iteration = iteratorForIterable(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = PyRecord::create(globalObject, runtime(globalObject).typeFor(jsIteratorTypeDef), JSIteratorFieldCount);
    record->set(vm, JSIteratorObject, iteration.iterator);
    record->set(vm, JSIteratorNextMethod, iteration.nextMethod);
    return record;
}

// `async for` is `for await`.
static JSValue jsObjectAsyncIter(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    IterationRecord iteration = getAsyncIterator(*globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = PyRecord::create(globalObject, runtime(globalObject).typeFor(jsAsyncIteratorTypeDef), JSIteratorFieldCount);
    record->set(vm, JSIteratorObject, iteration.iterator);
    record->set(vm, JSIteratorNextMethod, iteration.nextMethod);
    record->set(vm, JSIteratorThen, JSFunction::create(vm, globalObject, 1, String(), jsAsyncIteratorResult, ImplementationVisibility::Private));
    return record;
}

// keys(), values(), items(), for an object that has no properties of these names: it is a mapping of its properties, so dict(object).
enum class JSObjectPart : uint8_t { Keys, Values, Items };
static JSValue jsObjectParts(JSGlobalObject* globalObject, JSValue self, JSObjectPart part)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* names = ownPropertyNames(globalObject, asObject(self), DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, { });
    if (part == JSObjectPart::Keys)
        return names;
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < listLength(names); ++i) {
        JSValue name = listGet(names, i);
        JSValue value = getItem(globalObject, self, name);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, result, part == JSObjectPart::Values ? value : JSValue(PyTuple::create(globalObject, { name, value })));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}
static JSValue jsObjectKeys(JSGlobalObject* globalObject, JSValue self) { return jsObjectParts(globalObject, self, JSObjectPart::Keys); }
static JSValue jsObjectValues(JSGlobalObject* globalObject, JSValue self) { return jsObjectParts(globalObject, self, JSObjectPart::Values); }
static JSValue jsObjectItems(JSGlobalObject* globalObject, JSValue self) { return jsObjectParts(globalObject, self, JSObjectPart::Items); }

static JSValue jsObjectLen(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(self);
    for (auto& key : { vm.propertyNames->length, vm.propertyNames->size }) {
        JSValue value = object->get(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSmallInt(value) && smallIntValue(value) >= 0)
            return value;
    }
    // As in jsObjectIter(): it is the object's properties that are counted only if it is they that are iterated over. A generator
    // has no length, and code that asks for one to see whether there is one must be told so.
    bool isIterable = hasIteratorMethod(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (isIterable)
        return raiseTypeError(globalObject, scope, "object of type '"_s, typeName(globalObject, self), "' has no len()"_s);
    JSArray* names = ownPropertyNames(globalObject, object, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(listLength(names));
}

// Every object is true to JS. To Python a container is false when it is empty, so that goes for what is a container to it: what JS
// iterates over and has a length or size, and a plain object, which is a mapping of its properties. Anything else is true, as an
// instance of a class in Python is: a function, a promise, an Error, a Date, a Response.
static JSValue jsObjectBool(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(self);
    if (object->isCallable())
        return jsBoolean(true);
    bool isIterable = hasIteratorMethod(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (isIterable) {
        for (auto& key : { vm.propertyNames->length, vm.propertyNames->size }) {
            JSValue value = object->get(globalObject, key);
            RETURN_IF_EXCEPTION(scope, { });
            if (isSmallInt(value) && smallIntValue(value) >= 0)
                return jsBoolean(!!smallIntValue(value));
        }
        return jsBoolean(true);
    }
    JSValue prototype = object->getPrototypeDirect();
    if (object->type() != FinalObjectType || !(prototype.isNull() || prototype == JSValue(object->realm()->objectPrototype())))
        return jsBoolean(true);
    JSValue size = jsObjectLen(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(!!smallIntValue(size));
}

// What collections.abc.MutableMapping makes of __getitem__, __setitem__, __delitem__, __iter__ and __len__.
template<size_t length>
struct MappingMethodName {
    constexpr MappingMethodName(const char (&text)[length]) { std::copy_n(text, length, characters); }
    char characters[length];
};
template<MappingMethodName name>
static JSValue jsObjectMappingMethod(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, "_collections_abc"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue mapping = getAttr(globalObject, module, internString(vm, "MutableMapping"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue function = getAttr(globalObject, mapping, internString(vm, ASCIILiteral::fromLiteralUnsafe(name.characters)));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, withFirstArgument(self, args, [&](const CallArgs& all) {
        return call(globalObject, function, all);
    }));
}

// MutableMapping.pop() asks `self` for a mark of the class.
static JSValue jsObjectPop(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = jsObjectGetItem(globalObject, self, key);
    if (!value) {
        if (!defaultValue)
            return { };
        scope.release();
        return clearPendingExceptionIfMatches(globalObject, runtime(globalObject).KeyError()) ? defaultValue : JSValue();
    }
    scope.release();
    return jsObjectDelItem(globalObject, self, key) ? value : JSValue();
}

static JSValue jsObjectDir(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    UncheckedKeyHashSet<String> seen;
    for (JSValue object = self; object.isObject();) {
        JSArray* names = ownPropertyNames(globalObject, asObject(object), DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, { });
        // Function.prototype.arguments and caller throw for all functions but the sloppy ones of old: nothing to be listed among what
        // a function has.
        bool isFunctionPrototype = object == JSValue(asObject(object)->realm()->functionPrototype());
        for (unsigned i = 0; i < listLength(names); ++i) {
            String name = stringOf(asString(listGet(names, i)));
            if (name.isEmpty() || (isFunctionPrototype && (name == "arguments"_s || name == "caller"_s)) || !seen.add(name).isNewEntry)
                continue;
            listAppend(globalObject, result, listGet(names, i));
            RETURN_IF_EXCEPTION(scope, { });
        }
        object = asObject(object)->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue jsObjectRepr(JSGlobalObject*, JSValue self);
static JSValue jsObjectStr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, ensureMainThreadForJS(globalObject)))
        return { };
    // An object without a prototype, or the namespace of a module, has nothing by which JS could make a string of it.
    bool canConvert = false;
    for (auto& key : { vm.propertyNames->toPrimitiveSymbol, vm.propertyNames->toString, vm.propertyNames->valueOf }) {
        canConvert = asObject(self)->hasProperty(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (canConvert)
            break;
    }
    if (!canConvert)
        RELEASE_AND_RETURN(scope, jsObjectRepr(globalObject, self));
    RELEASE_AND_RETURN(scope, self.toString(globalObject));
}

static JSValue jsObjectRepr(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // An instance of a class written in Python.
    if (typeOf(globalObject, self)->isHeapType())
        RELEASE_AND_RETURN(scope, objectRepr(globalObject, self));
    String className = JSObject::calculatedClassName(asObject(self));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeStr(vm, makeString("<JavaScript "_s, className.isEmpty() ? "object"_str : className, " at "_s, addressForRepr(globalObject, self), '>')));
}

// ---- Symbols, which are no objects ----

static JSValue jsSymbolRepr(JSGlobalObject* globalObject, JSValue self)
{
    return asSymbol(self)->toString(globalObject);
}

static JSValue jsSymbolDescription(JSGlobalObject* globalObject, JSValue self)
{
    JSString* description = asSymbol(self)->description(getVM(globalObject));
    return description ? JSValue(description) : jsUndefined();
}

static constexpr NativeDef jsSymbolMethods[] = {
    PY_SLOT("__repr__", jsSymbolRepr, "($self, /)"),
};
static constexpr NativeGetSet jsSymbolGetSets[] = {
    { "description"_s, jsSymbolDescription },
};
static constexpr TypeDef jsSymbolTypeDef { "_js.JSSymbol"_s, { TypeFlag::DisallowInstantiation }, jsSymbolMethods, jsSymbolGetSets, { } };
PyType* jsSymbolType(Runtime& rt) { return rt.typeFor(jsSymbolTypeDef); }

static JSValue jsFunctionRepr(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue name = asObject(self)->get(globalObject, vm.propertyNames->name);
    RETURN_IF_EXCEPTION(scope, { });
    String text = name.isString() ? asString(name)->value(globalObject).data : String();
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeStr(vm, makeString("<JavaScript function "_s, text, " at "_s, addressForRepr(globalObject, self), '>')));
}

// ---- Calls from Python ----

JSValue valueForJS(JSValue value)
{
    if (!value || !value.isCell() || value.asCell()->type() != PyFloatType)
        return value;
    auto* number = uncheckedDowncast<PyFloat>(value.asCell());
    // An instance of a class derived from float is an object like any other.
    if (number->type() != runtime(number->structure()->realm()).floatType())
        return value;
    return jsNumber(purifyNaN(number->value()));
}

bool isForeignCallee(JSValue callee)
{
    if (!callee.isObject())
        return false;
    switch (callee.asCell()->type()) {
    case JSFunctionType:
        return !PyFunction::is(callee);
    case InternalFunctionType:
        return !PyType::is(callee) && !PyNativeFunction::is(callee);
    default:
        return !isPyObjectType(callee.asCell()->type());
    }
}

// f(1, a=2, b=3) is f(1, { a: 2, b: 3 }): that is how functions take named arguments in JS.
template<typename NameAt, typename ValueAt>
static JSObject* objectOfKeywordArguments(JSGlobalObject* globalObject, unsigned count, const NameAt& nameAt, const ValueAt& valueAt)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = makeKeywordMarker(globalObject);
    for (unsigned i = 0; i < count; ++i) {
        Identifier key = propertyKeyFor(globalObject, nameAt(i));
        RETURN_IF_EXCEPTION(scope, nullptr);
        object->putDirectMayBeIndex(globalObject, key, valueForJS(valueAt(i)));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return object;
}

// To call a class is to make an instance, in Python. The classes of JS are what `class` declares and the constructors that the
// engine and the host implement: Map, Date, Response. Those have a prototype for their instances; what can merely be constructed as
// well as called, as a mock function of a test runner can, is called. Not a plain function, which may be meant either way
// (for `new`: f.new()), and not the constructors of primitive values, which make one when they are called.
static bool hasPrototypeForInstances(JSObject* function)
{
    JSValue prototype = function->getDirect(function->vm(), function->vm().propertyNames->prototype);
    return (prototype && prototype.isObject()) || function->inherits<ProxyConstructor>();
}

static bool isClassOfJS(JSCell* callee)
{
    while (auto* bound = dynamicDowncast<JSBoundFunction>(callee))
        callee = bound->targetFunction();
    if (callee->inherits<StringConstructor>() || callee->inherits<NumberConstructor>() || callee->inherits<BooleanConstructor>()
        || callee->inherits<SymbolConstructor>() || callee->inherits<BigIntConstructor>())
        return false;
    if (auto* function = dynamicDowncast<JSFunction>(callee)) {
        if (function->isHostFunction())
            return function->nativeConstructor() != callHostFunctionAsConstructor && hasPrototypeForInstances(function);
        return function->jsExecutable()->isClassConstructorFunction();
    }
    auto* function = dynamicDowncast<InternalFunction>(callee);
    return function && function->nativeFunctionFor(CodeSpecializationKind::CodeForConstruct) != callHostFunctionAsConstructor && hasPrototypeForInstances(function);
}

bool prepareForeignCall(JSGlobalObject* globalObject, CallFrame* calleeFrame)
{
    if (!ensureMainThreadForJS(globalObject))
        return false;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = calleeFrame->argumentCount();
    if (JSValue names = calleeFrame->thisValue(); names.isCell() && names.asCell()->type() == PyKwNamesType) {
        auto* tuple = uncheckedDowncast<PyTuple>(names.asCell());
        unsigned positional = count - tuple->length();
        JSObject* object = objectOfKeywordArguments(globalObject, tuple->length(),
            [&](unsigned i) { return asString(tuple->at(i)); },
            [&](unsigned i) { return calleeFrame->uncheckedArgument(positional + i); });
        RETURN_IF_EXCEPTION(scope, false);
        calleeFrame->setArgument(positional, object);
        count = positional + 1;
        calleeFrame->setArgumentCountIncludingThis(count + 1);
        calleeFrame->setThisValue(jsUndefined());
    }
    for (unsigned i = 0; i < count; ++i) {
        JSValue argument = calleeFrame->uncheckedArgument(i);
        if (argument.isCell() && argument.asCell()->type() == PyFloatType)
            calleeFrame->setArgument(i, valueForJS(argument));
    }
    JSValue callee = calleeFrame->guaranteedJSValueCallee();
    if (!isClassOfJS(callee.asCell()))
        return false;
    calleeFrame->setThisValue(callee); // new.target
    return true;
}

static void argumentsForJS(JSGlobalObject* globalObject, const CallArgs& args, MarkedArgumentBuffer& list)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (unsigned i = 0; i < args.size(); ++i)
        list.append(valueForJS(args[i]));
    if (!args.hasKeywords())
        return;
    JSObject* object = objectOfKeywordArguments(globalObject, args.keywordCount(),
        [&](unsigned i) { return args.keywordName(i); },
        [&](unsigned i) { return args.keywordValue(i); });
    RETURN_IF_EXCEPTION(scope, void());
    list.append(object);
}

JSValue callForeignFunction(JSGlobalObject* globalObject, JSValue self, const CallArgs& args, JSValue thisValue)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer list;
    argumentsForJS(globalObject, args, list);
    RETURN_IF_EXCEPTION(scope, { });
    // What these return when something is thrown is not the empty value.
    JSValue result = isClassOfJS(self.asCell())
        ? JSValue(JSC::construct(globalObject, self, list, "not a constructor"_s))
        : JSC::call(globalObject, self, JSC::getCallData(self), thisValue ? valueForJS(thisValue) : jsUndefined(), list);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

JSValue constructForeignFunction(JSGlobalObject* globalObject, JSValue constructor, const CallArgs& args, JSValue newTarget)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CallData constructData = JSC::getConstructData(constructor);
    if (constructData.type == CallData::Type::None)
        return raiseTypeError(globalObject, scope, "this JavaScript function is not a constructor"_s);
    MarkedArgumentBuffer list;
    argumentsForJS(globalObject, args, list);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* result = JSC::construct(globalObject, constructor, constructData, list, newTarget ? newTarget : constructor);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

ForeignMethodKind foreignMethodKind(JSCell* cell)
{
    auto* function = dynamicDowncast<JSFunction>(cell);
    if (!function || PyFunction::is(cell) || function->isHostFunction())
        return ForeignMethodKind::NotAMethod;
    FunctionExecutable* executable = function->jsExecutable();
    if (executable->isClassConstructorFunction())
        return ForeignMethodKind::NotAMethod;
    return isArrowFunctionParseMode(executable->parseMode()) ? ForeignMethodKind::TakesFirstArgument : ForeignMethodKind::TakesThis;
}

JSValue callForeignMethod(JSGlobalObject* globalObject, JSValue function, JSValue self, const CallArgs& args)
{
    if (foreignMethodKind(function.asCell()) == ForeignMethodKind::TakesThis)
        return callForeignFunction(globalObject, function, args, self);
    return withFirstArgument(self, args, [&](const CallArgs& all) {
        return callForeignFunction(globalObject, function, all);
    });
}

// function.new(...): `new function(...)`
static JSValue jsFunctionNew(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    return constructForeignFunction(globalObject, self, args);
}

// A function found in a class is a method of the instances, as one of Python's is.
static JSValue jsFunctionGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue)
{
    if (isNone(instance) || foreignMethodKind(self.asCell()) == ForeignMethodKind::NotAMethod)
        return self;
    return PyMethod::create(globalObject, self, instance);
}

static JSValue jsFunctionCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args) { return callForeignFunction(globalObject, self, args); }

// isinstance(value, function): `value instanceof function`
static JSValue jsFunctionInstanceCheck(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = asObject(self)->hasInstance(globalObject, valueForJS(value));
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

// class Derived(function): see PyJSClass.h.
static JSValue jsFunctionMroEntries(JSGlobalObject* globalObject, JSValue self, JSValue)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    PyType* type = typeForClassOfJS(globalObject, asObject(self));
    return type ? JSValue(PyTuple::create(globalObject, { type })) : JSValue();
}

// issubclass(cls, function): whether the instances of the class inherit from function.prototype.
static JSValue jsFunctionSubclassCheck(JSGlobalObject* globalObject, JSValue self, JSValue cls)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* function = asObject(self);
    while (auto* bound = dynamicDowncast<JSBoundFunction>(function))
        function = bound->targetFunction();
    JSValue wanted = function->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue prototype;
    if (PyType* type = PyType::tryFrom(cls))
        prototype = prototypeForJS(globalObject, type);
    else if (cls.isObject()) {
        prototype = asObject(cls)->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, { });
    }
    while (prototype && prototype.isObject()) {
        if (prototype == wanted)
            return jsBoolean(true);
        prototype = asObject(prototype)->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsBoolean(false);
}

static constexpr NativeDef jsObjectMethods[] = {
    PY_SLOT("__getattribute__", jsObjectGetAttribute, "($self, name, /)"),
    PY_SLOT("__setattr__", jsObjectSetAttr, "($self, name, value, /)"),
    PY_SLOT("__delattr__", jsObjectDelAttr, "($self, name, /)"),
    PY_SLOT("__getitem__", jsObjectGetItem, "($self, key, /)"),
    PY_SLOT("__setitem__", jsObjectSetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", jsObjectDelItem, "($self, key, /)"),
    PY_SLOT("__contains__", jsObjectContains, "($self, key, /)"),
    PY_SLOT("__iter__", jsObjectIter, "($self, /)"),
    PY_SLOT("__aiter__", jsObjectAsyncIter, "($self, /)"),
    PY_SLOT("__len__", jsObjectLen, "($self, /)"),
    PY_SLOT("__bool__", jsObjectBool, "($self, /)"),
    PY_SLOT("__repr__", jsObjectRepr, "($self, /)"),
    PY_SLOT("__str__", jsObjectStr, "($self, /)"),
    PY_METHOD("__dir__", jsObjectDir, "($self, /)"),
    PY_METHOD("keys", jsObjectKeys, "($self, /)"),
    PY_METHOD("values", jsObjectValues, "($self, /)"),
    PY_METHOD("items", jsObjectItems, "($self, /)"),
    PY_METHOD("get", jsObjectMappingMethod<"get">, "($self, *args, **kwargs)"),
    PY_METHOD("pop", jsObjectPop, "($self, key, default=<unrepresentable>, /)"),
    PY_METHOD("popitem", jsObjectMappingMethod<"popitem">, "($self, *args, **kwargs)"),
    PY_METHOD("setdefault", jsObjectMappingMethod<"setdefault">, "($self, *args, **kwargs)"),
    PY_METHOD("update", jsObjectMappingMethod<"update">, "($self, *args, **kwargs)"),
    PY_METHOD("clear", jsObjectMappingMethod<"clear">, "($self, *args, **kwargs)"),
};

// function.__name__ and __qualname__, which decorators copy and messages show: function.name
static JSValue jsFunctionName(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Not "bound name", for a method that has been read off its object.
    JSObject* function = asObject(self);
    while (auto* bound = dynamicDowncast<JSBoundFunction>(function))
        function = bound->targetFunction();
    JSValue name = function->get(globalObject, vm.propertyNames->name);
    RETURN_IF_EXCEPTION(scope, { });
    return name.isString() ? name : JSValue(jsEmptyString(vm));
}

// The function written in JS behind the value, and how many of its parameters bind() has filled in. Null: it is written in C++.
static FunctionExecutable* executableOfFunction(JSValue self, unsigned& boundArguments)
{
    JSObject* function = asObject(self);
    boundArguments = 0;
    while (auto* bound = dynamicDowncast<JSBoundFunction>(function)) {
        boundArguments += bound->boundArgsLength();
        function = bound->targetFunction();
    }
    auto* jsFunction = dynamicDowncast<JSFunction>(function);
    if (!jsFunction || jsFunction->isHostFunction() || PyFunction::is(jsFunction))
        return nullptr;
    FunctionExecutable* executable = jsFunction->jsExecutable();
    return executable->isBuiltinFunction() ? nullptr : executable;
}

// function.__text_signature__, which is where inspect.signature() looks for what is no function of Python's: "(a, b=None, /, *rest)", from
// the text of the parameters. They go by position. To pass None, which is undefined, for one that has a default gives the default,
// so that is what the default is to Python. A pattern ({ a, b }, [x]) has no name: arg1. None: there is no telling.
static JSValue jsFunctionTextSignature(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    unsigned skipped = 0;
    FunctionExecutable* executable = executableOfFunction(self, skipped);
    if (!executable)
        return jsUndefined();
    if (executable->unlinkedExecutable()->isBuiltinDefaultClassConstructor())
        return makeStr(vm, executable->unlinkedExecutable()->constructorKind() == ConstructorKind::Extends ? "(*args)"_s : "()"_s);
    unsigned start = executable->parametersStartOffset();
    StringView text = executable->source().provider()->getRange(start, start + executable->source().length());

    auto isNameStart = [](char16_t c) { return isASCIIAlpha(c) || c == '_'; };
    auto isNamePart = [](char16_t c) { return isASCIIAlphanumeric(c) || c == '_'; };
    static constexpr ASCIILiteral keywords[] = {
        "False"_s, "None"_s, "True"_s, "and"_s, "as"_s, "assert"_s, "async"_s, "await"_s, "break"_s, "class"_s, "continue"_s, "def"_s, "del"_s, "elif"_s, "else"_s,
        "except"_s, "finally"_s, "for"_s, "from"_s, "global"_s, "if"_s, "import"_s, "in"_s, "is"_s, "lambda"_s, "nonlocal"_s, "not"_s, "or"_s, "pass"_s, "raise"_s,
        "return"_s, "try"_s, "while"_s, "with"_s, "yield"_s,
    };
    StringBuilder positional;
    String rest;
    unsigned index = 0;
    unsigned count = 0;
    bool hasDefaults = false;
    unsigned i = 0;
    unsigned length = text.length();
    bool hasParentheses = length && text[0] == '(';
    if (hasParentheses)
        ++i;
    auto skipSpaceAndComments = [&] {
        while (i < length) {
            if (isUnicodeCompatibleASCIIWhitespace(text[i]))
                ++i;
            else if (text[i] == '/' && i + 1 < length && text[i + 1] == '/') {
                while (i < length && text[i] != '\n')
                    ++i;
            } else if (text[i] == '/' && i + 1 < length && text[i + 1] == '*') {
                i += 2;
                while (i + 1 < length && !(text[i] == '*' && text[i + 1] == '/'))
                    ++i;
                i += 2;
            } else
                break;
        }
    };
    while (true) {
        skipSpaceAndComments();
        if (i >= length)
            return jsUndefined();
        if (text[i] == ')' && hasParentheses)
            break;
        bool isRest = text.substring(i).startsWith("..."_s);
        if (isRest) {
            i += 3;
            skipSpaceAndComments();
        }
        unsigned nameStart = i;
        while (i < length && (i == nameStart ? isNameStart(text[i]) : isNamePart(text[i])))
            ++i;
        String name = text.substring(nameStart, i - nameStart).toString();
        // The rest of the parameter: what is left of a name Python could not spell, a pattern, a default.
        bool hasDefault = false;
        bool isPlainName = !name.isEmpty();
        unsigned depth = 0;
        for (; i < length; ++i) {
            char16_t c = text[i];
            if (c == '"' || c == '\'' || c == '`') {
                for (++i; i < length && text[i] != c; ++i) {
                    if (text[i] == '\\')
                        ++i;
                }
                continue;
            }
            if (c == '(' || c == '[' || c == '{')
                ++depth;
            else if (c == ')' || c == ']' || c == '}') {
                if (!depth)
                    break;
                --depth;
            } else if (!depth && c == ',')
                break;
            else if (!depth && c == '=') {
                if (i + 1 < length && text[i + 1] == '>' && !hasParentheses)
                    break;
                hasDefault = true;
            } else if (!depth && !hasDefault && !isUnicodeCompatibleASCIIWhitespace(c))
                isPlainName = false;
            if (depth && !hasDefault)
                isPlainName = false;
        }
        for (ASCIILiteral keyword : keywords)
            isPlainName = isPlainName && name != keyword;
        if (!isPlainName)
            name = makeString("arg"_s, index);
        if (isRest)
            rest = name;
        else if (index++ >= skipped) {
            hasDefaults |= hasDefault;
            positional.append(count++ ? ", "_s : ""_s, name, hasDefaults ? "=None"_s : ""_s);
        }
        if (!hasParentheses || i >= length || text[i] != ',')
            break;
        ++i;
    }
    return makeStr(vm, makeString('(', positional.toString(), count ? ", /"_s : ""_s, rest.isNull() ? ""_s : count ? ", *"_s : "*"_s, rest.isNull() ? String(emptyString()) : rest, ')'));
}

// By which inspect.iscoroutinefunction() knows what it cannot tell by the code object: an async function gives what is to be awaited.
static JSValue jsFunctionCoroutineMark(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned boundArguments;
    FunctionExecutable* executable = executableOfFunction(self, boundArguments);
    if (!executable || !isAsyncFunctionParseMode(executable->parseMode()) || isAsyncGeneratorParseMode(executable->parseMode()))
        return jsUndefined();
    JSValue inspect = importModule(globalObject, "inspect"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getAttr(globalObject, inspect, internString(vm, "_is_coroutine_mark"_s)));
}

static JSValue jsFunctionModule(JSGlobalObject* globalObject, JSValue) { return internString(getVM(globalObject), "js"_s); }
static JSValue jsFunctionNone(JSGlobalObject*, JSValue) { return jsUndefined(); }

static constexpr NativeGetSet jsFunctionGetSets[] = {
    { "__name__"_s, jsFunctionName },
    { "__qualname__"_s, jsFunctionName },
    { "__module__"_s, jsFunctionModule },
    { "__doc__"_s, jsFunctionNone },
    { "__text_signature__"_s, jsFunctionTextSignature },
    { "_is_coroutine_marker"_s, jsFunctionCoroutineMark },
};

// copy.copy() and copy.deepcopy() of a function give the function, as for those of Python.
static JSValue jsFunctionCopy(JSGlobalObject*, JSValue self) { return self; }
static JSValue jsFunctionDeepCopy(JSGlobalObject*, JSValue self, JSValue) { return self; }

static constexpr NativeDef jsFunctionMethods[] = {
    PY_SLOT("__call__", jsFunctionCall, "($self, *args, **kwargs)"),
    PY_SLOT("__get__", jsFunctionGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__repr__", jsFunctionRepr, "($self, /)"),
    PY_METHOD("__copy__", jsFunctionCopy, "($self, /)"),
    PY_METHOD("__deepcopy__", jsFunctionDeepCopy, "($self, memo, /)"),
    PY_METHOD("new", jsFunctionNew, "($self, *args, **kwargs)"),
    PY_METHOD("__instancecheck__", jsFunctionInstanceCheck, "($self, instance, /)"),
    PY_METHOD("__subclasscheck__", jsFunctionSubclassCheck, "($self, subclass, /)"),
    PY_METHOD("__mro_entries__", jsFunctionMroEntries, "($self, bases, /)"),
};

// ---- The iterators and generators of JS are iterators and generators ----

// iterator.method([argument]), and of the { value, done } that gives the value, or StopIteration(value).
static JSValue stepIteratorOfJS(JSGlobalObject* globalObject, JSValue self, const Identifier& method, JSValue argument = { })
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue function = asObject(self)->get(globalObject, method);
    RETURN_IF_EXCEPTION(scope, { });
    CallData callData = JSC::getCallData(function);
    if (callData.type == CallData::Type::None)
        return raiseTypeError(globalObject, scope, "this JavaScript iterator has no "_s, method.string(), "()"_s);
    MarkedArgumentBuffer arguments;
    if (argument)
        arguments.append(valueForJS(argument));
    JSValue result = JSC::call(globalObject, function, callData, self, arguments);
    RETURN_IF_EXCEPTION(scope, { });
    if (!result.isObject())
        return raiseTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
    bool isDone = iteratorComplete(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = iteratorValue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    if (isDone)
        return raiseStopIteration(globalObject, scope, value);
    return value;
}

static JSValue jsIteratorObjectNext(JSGlobalObject* globalObject, JSValue self)
{
    return stepIteratorOfJS(globalObject, self, getVM(globalObject).propertyNames->next);
}

static JSValue jsGeneratorSend(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    return stepIteratorOfJS(globalObject, self, getVM(globalObject).propertyNames->next, value);
}

// generator.throw(exception), or throw(type[, value]) as `raise type(value)`. What JS catches is what it would if Python raised it.
static JSValue jsGeneratorThrow(JSGlobalObject* globalObject, JSValue self, JSValue exception, JSValue value, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* type = PyType::tryFrom(exception); type && type->has(TypeFlag::ExceptionSubclass)) {
        exception = value && !isNone(value) ? call(globalObject, type, value) : call(globalObject, type);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!PyException::is(exception))
        return raiseTypeError(globalObject, scope, "exceptions must be classes or instances deriving from BaseException, not "_s, typeName(globalObject, exception));
    throwPythonException(globalObject, scope, exception);
    JSValue thrown = scope.exception()->value();
    (void)scope.tryClearException();
    RELEASE_AND_RETURN(scope, stepIteratorOfJS(globalObject, self, vm.propertyNames->throwKeyword, thrown));
}

// generator.return(), which runs what `finally` holds as GeneratorExit does in Python.
static JSValue jsGeneratorClose(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    stepIteratorOfJS(globalObject, self, vm.propertyNames->returnKeyword);
    if (!scope.exception())
        return raiseRuntimeError(globalObject, scope, "generator ignored GeneratorExit"_s);
    if (!clearPendingExceptionIfMatches(globalObject, runtime(globalObject).StopIteration()))
        return { };
    return jsUndefined();
}

// The properties of an object of JS come before what its type has in Python, and throw() is both.
static JSValue jsGeneratorGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (stringOf(name) == "throw"_s)
        RELEASE_AND_RETURN(scope, genericGetAttr(globalObject, self, name));
    RELEASE_AND_RETURN(scope, jsObjectGetAttribute(globalObject, self, nameValue));
}

static constexpr NativeDef jsIteratorObjectMethods[] = {
    PY_SLOT("__iter__", jsIteratorSelf, "($self, /)"),
    PY_SLOT("__next__", jsIteratorObjectNext, "($self, /)"),
};
static constexpr TypeDef jsIteratorObjectTypeDef { "_js.JSIterator"_s, { TypeFlag::DisallowInstantiation }, jsIteratorObjectMethods, { }, { } };
PyType* jsIteratorObjectType(Runtime& rt) { return rt.typeFor(jsIteratorObjectTypeDef, rt.jsObjectType()); }

static constexpr NativeDef jsGeneratorMethods[] = {
    PY_SLOT("__getattribute__", jsGeneratorGetAttribute, "($self, name, /)"),
    PY_METHOD("send", jsGeneratorSend, "($self, value, /)"),
    PY_METHOD("throw", jsGeneratorThrow, "($self, typ, val=None, tb=None, /)"),
    PY_METHOD("close", jsGeneratorClose, "($self, /)"),
};
static constexpr TypeDef jsGeneratorTypeDef { "_js.JSGenerator"_s, { TypeFlag::DisallowInstantiation }, jsGeneratorMethods, { }, { } };
PyType* jsGeneratorType(Runtime& rt) { return rt.typeFor(jsGeneratorTypeDef, jsIteratorObjectType(rt)); }

// `await promise`
static JSValue jsPromiseAwait(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue wait = eventLoopFunction(globalObject, "wait"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!wait)
        return raiseRuntimeError(globalObject, scope, "there is no event loop to wait for a JavaScript promise on"_s);
    RELEASE_AND_RETURN(scope, call(globalObject, wait, self));
}

// A promise is to asyncio what a coroutine object is (asyncio.iscoroutine()): what the call of an async function gave, to be awaited or
// made a task of. So create_task(), ensure_future(), gather(), run_until_complete() and run() take one, on whichever loop. (A future it
// is not: that belongs to one loop and can be cancelled.) A task runs a coroutine of its own for it (taskInit()); these are for whoever
// else drives a coroutine by hand.
static constexpr unsigned awaitIteratorField = 0;
static JSValue jsPromiseAwaitIterator(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue iterator = hiddenField(globalObject, self, awaitIteratorField))
        return iterator;
    JSValue iterator = jsPromiseAwait(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    setHiddenField(globalObject, self, awaitIteratorField, iterator);
    return iterator;
}

static JSValue jsPromiseSend(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = jsPromiseAwaitIterator(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callMethod(globalObject, iterator, runtime(globalObject).names.send(), value));
}

static JSValue jsPromiseThrow(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = jsPromiseAwaitIterator(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue method = getAttr(globalObject, iterator, runtime(globalObject).names.throw_());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, method, args));
}

static JSValue jsPromiseClose(JSGlobalObject* globalObject, JSValue self)
{
    JSValue iterator = hiddenField(globalObject, self, awaitIteratorField);
    if (!iterator)
        return jsUndefined();
    return callMethod(globalObject, iterator, runtime(globalObject).names.close());
}

static constexpr NativeDef jsPromiseMethods[] = {
    PY_SLOT("__await__", jsPromiseAwait, "($self, /)"),
    PY_METHOD("send", jsPromiseSend, "($self, value, /)"),
    PY_METHOD("throw", jsPromiseThrow, "($self, *args)"),
    PY_METHOD("close", jsPromiseClose, "($self, /)"),
};

// What `except` catches when JS throws the value: the value, if it is an exception of Python's, or the JSError that stands for it.
static JSValue jsExceptionFor(JSGlobalObject* globalObject, JSValue thrown)
{
    return exceptionFromThrownValue(globalObject, thrown);
}

// A method that the language itself calls, as the language finds it: in the class alone, bound to the object. None: there is none.
static JSValue jsSpecialMethod(JSGlobalObject* globalObject, JSValue object, JSValue nameValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    PyType* type = rt.typeOf(object);
    JSValue found = type->lookup(name);
    if (!found)
        return jsUndefined();
    RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, found, object, type));
}

static constexpr NativeDef jsModuleFunctions[] = {
    PY_FUNCTION("exception_for", jsExceptionFor, "(thrown, /)"),
    PY_FUNCTION("special_method", jsSpecialMethod, "(object, name, /)"),
};

// The module _js: what Python code needs to name when it deals with JS. (`import js` is the global object of JS.)
void initializeJSModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->add(globalObject, "JSError"_s, rt.JSError());
    module->add(globalObject, "JSObject"_s, rt.jsObjectType());
    module->add(globalObject, "JSFunction"_s, rt.jsFunctionType());
    module->add(globalObject, "JSPromise"_s, rt.jsPromiseType());
    module->add(globalObject, "JSIterator"_s, jsIteratorObjectType(rt));
    module->add(globalObject, "JSGenerator"_s, jsGeneratorType(rt));
    module->add(globalObject, "JSMethod"_s, jsMethodType(rt));
    module->addFunctions(globalObject, jsModuleFunctions);
}

void initializeInteropTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.jsObjectType()->initializeBuiltin(globalObject, { "JSObject"_s, { TypeFlag::DisallowInstantiation, TypeFlag::HasWeakref, TypeFlag::Base, TypeFlag::OwnLayout }, jsObjectMethods, { }, { } }, rt.typeType(), rt.objectType());
    rt.jsFunctionType()->initializeBuiltin(globalObject, { "JSFunction"_s, { TypeFlag::DisallowInstantiation, TypeFlag::HasWeakref }, jsFunctionMethods, jsFunctionGetSets, { } }, rt.typeType(), rt.jsObjectType());
    rt.jsPromiseType()->initializeBuiltin(globalObject, { "JSPromise"_s, { TypeFlag::DisallowInstantiation, TypeFlag::HasWeakref, TypeFlag::Base, TypeFlag::OwnLayout }, jsPromiseMethods, { }, { } }, rt.typeType(), rt.jsObjectType());
}

} // namespace Python
} // namespace JSC
