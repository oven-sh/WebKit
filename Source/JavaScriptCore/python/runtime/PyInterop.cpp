/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "IteratorOperations.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyNameArray.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyTypes.h"

namespace JSC {
namespace Python {

// What the objects and functions of JS are to Python.
//
// Numbers, strings, booleans, arrays, undefined and null are Python values as they are. Any other object is an instance of
// JSObject, or of JSFunction if it can be called: its properties are its attributes, and also its items.

static Identifier propertyKeyFor(JSGlobalObject* globalObject, JSString* name)
{
    return name->toIdentifier(globalObject);
}

// A function that is a property is a method: it is called with the object as `this`, as in object.method().
static JSValue bindIfFunction(JSGlobalObject* globalObject, JSObject* object, JSValue value)
{
    VM& vm = getVM(globalObject);
    if (!value.isObject() || isPyObject(value) || PyFunction::is(value) || PyType::is(value) || PyNativeFunction::is(value))
        return value;
    JSObject* function = asObject(value);
    if (!function->isCallable() || function->inherits<JSBoundFunction>())
        return value;
    // A class is not a method of what holds it: js.Promise is Promise itself.
    if (auto* jsFunction = dynamicDowncast<JSFunction>(function); jsFunction && !jsFunction->isHostFunction() && jsFunction->jsExecutable()->isClassConstructorFunction())
        return value;
    if (function->inherits<InternalFunction>())
        return value;
    return JSBoundFunction::create(vm, globalObject, function, object, ArgList(), 0, nullptr, SourceTaintedOrigin::Untainted);
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
    if (auto* bound = dynamicDowncast<JSBoundFunction>(object))
        RELEASE_AND_RETURN(scope, jsObjectGetAttribute(globalObject, bound->targetFunction(), nameValue));
    // What every Python object has: __class__, __dir__, and so on.
    RELEASE_AND_RETURN(scope, genericGetAttr(globalObject, self, name));
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
    JSCell::deleteProperty(object, globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
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

static JSValue jsObjectGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier name;
    if (!propertyKeyForItem(globalObject, key, name))
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
    Identifier name;
    if (!propertyKeyForItem(globalObject, key, name))
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
    Identifier name;
    if (!propertyKeyForItem(globalObject, key, name))
        return { };
    JSObject* object = asObject(self);
    bool had = object->hasProperty(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!had)
        return raiseKeyError(globalObject, scope, key);
    JSCell::deleteProperty(object, globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue jsObjectContains(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier name;
    if (!propertyKeyForItem(globalObject, key, name)) {
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
    JSArray* items;
    if (isIterable) {
        // FIXME: All at once. An iterator over the JS iterator would take the items as they are asked for.
        items = newList(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        forEachInIterable(globalObject, object, [&](VM&, JSGlobalObject*, JSValue item) {
            listAppend(globalObject, items, item);
        });
    } else
        items = ownPropertyNames(globalObject, object, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getIter(globalObject, items));
}

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
    JSArray* names = ownPropertyNames(globalObject, object, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(listLength(names));
}

static JSValue jsObjectDir(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (JSValue object = self; object.isObject();) {
        JSArray* names = ownPropertyNames(globalObject, asObject(object), DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, { });
        listExtend(globalObject, result, names);
        RETURN_IF_EXCEPTION(scope, { });
        object = asObject(object)->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue jsObjectStr(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    return self.toString(globalObject);
}

static JSValue jsObjectRepr(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String className = JSObject::calculatedClassName(asObject(self));
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString("<JavaScript "_s, className.isEmpty() ? "object"_str : className, " at "_s, addressForRepr(globalObject, self), '>'));
}

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
    return makeStr(vm, makeString("<JavaScript function "_s, text, " at "_s, addressForRepr(globalObject, self), '>'));
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
    JSObject* object = constructEmptyObject(globalObject);
    for (unsigned i = 0; i < count; ++i) {
        Identifier key = propertyKeyFor(globalObject, nameAt(i));
        RETURN_IF_EXCEPTION(scope, nullptr);
        object->putDirectMayBeIndex(globalObject, key, valueForJS(valueAt(i)));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return object;
}

void prepareForeignCall(JSGlobalObject* globalObject, CallFrame* calleeFrame)
{
    if (!ensureMainThreadForJS(globalObject))
        return;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = calleeFrame->argumentCount();
    if (JSValue names = calleeFrame->thisValue(); names.isCell() && names.asCell()->type() == PyKwNamesType) {
        auto* tuple = uncheckedDowncast<PyTuple>(names.asCell());
        unsigned positional = count - tuple->length();
        JSObject* object = objectOfKeywordArguments(globalObject, tuple->length(),
            [&](unsigned i) { return asString(tuple->at(i)); },
            [&](unsigned i) { return calleeFrame->uncheckedArgument(positional + i); });
        RETURN_IF_EXCEPTION(scope, void());
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

JSValue callForeignFunction(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer list;
    argumentsForJS(globalObject, args, list);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSC::call(globalObject, self, JSC::getCallData(self), jsUndefined(), list));
}

// function.new(...): `new function(...)`
static JSValue jsFunctionNew(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CallData constructData = JSC::getConstructData(self);
    if (constructData.type == CallData::Type::None)
        return raiseTypeError(globalObject, scope, "this JavaScript function is not a constructor"_s);
    MarkedArgumentBuffer list;
    argumentsForJS(globalObject, args, list);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSC::construct(globalObject, self, constructData, list));
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
    PY_SLOT("__len__", jsObjectLen, "($self, /)"),
    PY_SLOT("__repr__", jsObjectRepr, "($self, /)"),
    PY_SLOT("__str__", jsObjectStr, "($self, /)"),
    PY_METHOD("__dir__", jsObjectDir, "($self, /)"),
};

static constexpr NativeDef jsFunctionMethods[] = {
    PY_SLOT("__call__", callForeignFunction, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", jsFunctionRepr, "($self, /)"),
    PY_METHOD("new", jsFunctionNew, "($self, *args, **kwargs)"),
};

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

static constexpr NativeDef jsPromiseMethods[] = {
    PY_SLOT("__await__", jsPromiseAwait, "($self, /)"),
};

// What `except` catches when JS throws the value: the value, if it is an exception of Python's, or the JSError that stands for it.
static JSValue jsExceptionFor(JSGlobalObject* globalObject, JSValue thrown)
{
    return exceptionFromThrownValue(globalObject, thrown);
}

static constexpr NativeDef jsModuleFunctions[] = {
    PY_FUNCTION("exception_for", jsExceptionFor, "(thrown, /)"),
};

// The module _js: what Python code needs to name when it deals with JS. (`import js` is the global object of JS.)
void initializeJSModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->add(globalObject, "JSError"_s, rt.JSError());
    module->add(globalObject, "JSObject"_s, rt.jsObjectType());
    module->add(globalObject, "JSFunction"_s, rt.jsFunctionType());
    module->add(globalObject, "JSPromise"_s, rt.jsPromiseType());
    module->addFunctions(globalObject, jsModuleFunctions);
}

void initializeInteropTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.jsObjectType()->initializeBuiltin(globalObject, { "JSObject"_s, { TypeFlag::DisallowInstantiation }, jsObjectMethods, { }, { } }, rt.typeType(), rt.objectType());
    rt.jsFunctionType()->initializeBuiltin(globalObject, { "JSFunction"_s, { TypeFlag::DisallowInstantiation }, jsFunctionMethods, { }, { } }, rt.typeType(), rt.jsObjectType());
    rt.jsPromiseType()->initializeBuiltin(globalObject, { "JSPromise"_s, { TypeFlag::DisallowInstantiation }, jsPromiseMethods, { }, { } }, rt.typeType(), rt.jsObjectType());
}

} // namespace Python
} // namespace JSC
