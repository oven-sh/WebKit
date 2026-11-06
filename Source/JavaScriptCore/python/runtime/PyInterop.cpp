/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "AOTCompiledCallee.h"
#include "BigIntConstructor.h"
#include "BooleanConstructor.h"
#include "BuiltinNames.h"
#include "FunctionPrototype.h"
#include "IteratorOperations.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSGlobalProxy.h"
#include "JSMapInlines.h"
#include "JSModuleNamespaceObject.h"
#include "JSPromise.h"
#include "JSSetInlines.h"
#include "JSWeakMapInlines.h"
#include "NumberConstructor.h"
#include "ObjectConstructor.h"
#include "ObjectPrototype.h"
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

static bool isKeywordOfPython(StringView word)
{
    static constexpr ASCIILiteral keywords[] = {
        "False"_s, "None"_s, "True"_s, "and"_s, "as"_s, "assert"_s, "async"_s, "await"_s, "break"_s, "class"_s, "continue"_s, "def"_s, "del"_s, "elif"_s, "else"_s,
        "except"_s, "finally"_s, "for"_s, "from"_s, "global"_s, "if"_s, "import"_s, "in"_s, "is"_s, "lambda"_s, "nonlocal"_s, "not"_s, "or"_s, "pass"_s, "raise"_s,
        "return"_s, "try"_s, "while"_s, "with"_s, "yield"_s,
    };
    return std::ranges::any_of(keywords, [&](ASCIILiteral keyword) { return word == keyword; });
}

// ---- Looking at an object from a thread that may not run JS ----
//
// A thread other than the main one has the lock of the VM while it runs, so it may look at an object of JS, as whoever goes through
// sys.modules does (unittest: getattr(module, "__warningregistry__", None)). It may run no code of JS's or of the host's. So it gets
// what is there to be seen: a property that is a value, or that there is none. A getter, a proxy, a property that is made when it is
// first asked for, and a class with a way of its own to look properties up take the main thread.
enum class Inquiry : uint8_t { Absent, Value, NeedsCode };

static Inquiry inquireOwnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue& value)
{
    VM& vm = getVM(globalObject);
    // Who only looks runs nothing that could raise.
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Structure* structure = object->structure();
    unsigned attributes;
    bool isStored = isValidOffset(structure->get(vm, name, attributes));
    if (!isStored && structure->hasNonReifiedStaticProperties() && structure->findPropertyHashEntry(name))
        return Inquiry::NeedsCode;
    auto lookUp = object->methodTable()->getOwnPropertySlot;
    // A typed array has a way of its own for its elements alone, and the prototype that a class of Python's is to JS for names alone.
    if (isTypedArrayType(object->type()) && (name.isSymbol() || !isCanonicalNumericIndexString(name.uid())))
        lookUp = JSObject::getOwnPropertySlot;
    if (name.isSymbol() && object->classInfo() == PyPrototypeView::info())
        lookUp = JSObject::getOwnPropertySlot;
    bool isNamespace = lookUp == JSModuleNamespaceObject::getOwnPropertySlot;
    if (lookUp == JSFunction::info()->methodTable.getOwnPropertySlot) {
        // The name and length of a bound function are asked of its target.
        if (!isStored && object->inherits<JSBoundFunction>() && (name == vm.propertyNames->name || name == vm.propertyNames->length))
            return Inquiry::NeedsCode;
    } else if (lookUp != JSObject::getOwnPropertySlot && lookUp != JSGlobalObject::getOwnPropertySlot && !isNamespace)
        return Inquiry::NeedsCode;
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool found = lookUp(object, globalObject, name, slot);
    scope.assertNoException();
    // A namespace does not show what it exports to who is only looking, but for what it does not export it leaves the slot as it was.
    if (!found)
        return (isNamespace ? slot.isUnset() : !slot.isTaintedByOpaqueObject()) ? Inquiry::Absent : Inquiry::NeedsCode;
    if (!slot.isValue() || slot.isTaintedByOpaqueObject())
        return Inquiry::NeedsCode;
    value = slot.getValue(globalObject, name);
    scope.assertNoException();
    return Inquiry::Value;
}

static Inquiry inquireProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue& value)
{
    while (true) {
        if (auto* proxy = dynamicDowncast<JSGlobalProxy>(object))
            object = proxy->target();
        Inquiry result = inquireOwnProperty(globalObject, object, name, value);
        if (result != Inquiry::Absent)
            return result;
        if (object->structure()->typeInfo().overridesGetPrototype())
            return Inquiry::NeedsCode;
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject())
            return Inquiry::Absent;
        object = asObject(prototype);
    }
}

static ALWAYS_INLINE bool isKeywordWithUnderscore(const Identifier& key)
{
    StringView text = key.string();
    return text.length() >= 3 && text[text.length() - 1] == '_' && isKeywordOfPython(text.left(text.length() - 1));
}

static Identifier withoutUnderscore(VM& vm, const Identifier& key)
{
    return Identifier::fromString(vm, StringView(key.string()).left(key.length() - 1).toString());
}

// The property that an attribute of the object is. A name that is a keyword of Python cannot be written behind a dot, so it is written
// with an underscore behind it, as PEP 8 has it: Array.from_(), promise.finally_(). Unless the object has a property of that
// very name. False: an exception.
static bool propertyKeyForAttribute(JSGlobalObject* globalObject, JSObject* object, JSString* name, Identifier& key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    key = propertyKeyFor(globalObject, name);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isKeywordWithUnderscore(key)) [[likely]]
        return true;
    bool hasIt;
    if (runtime(globalObject).threads().isMainThread()) [[likely]] {
        hasIt = object->hasProperty(globalObject, key);
        RETURN_IF_EXCEPTION(scope, false);
    } else {
        JSValue value;
        Inquiry result = inquireProperty(globalObject, object, key, value);
        if (result == Inquiry::NeedsCode)
            RELEASE_AND_RETURN(scope, ensureMainThreadForJS(globalObject));
        hasIt = result == Inquiry::Value;
    }
    if (!hasIt)
        key = withoutUnderscore(vm, key);
    return true;
}

// Whether the object is the `prototype` of a constructor. What is read off one is JS's spelling of an unbound method:
// String.prototype.toUpperCase.call("abc").
static bool isPrototypeOfConstructor(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue constructor = object->getDirect(vm, vm.propertyNames->constructor);
    if (!constructor) [[likely]]
        return false;
    // In many classes of the host it is a getter written in C++ (Headers.prototype).
    if (constructor.isCell() && constructor.asCell()->type() == CustomGetterSetterType) [[unlikely]] {
        if (failed(scope, ensureMainThreadForJS(globalObject)))
            return false;
        constructor = object->get(globalObject, vm.propertyNames->constructor);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (!constructor.isObject())
        return false;
    return asObject(constructor)->getDirect(vm, vm.propertyNames->prototype) == JSValue(object);
}

// Function.prototype.bind.call(function, object), name and length included: they are worked out when they are asked for, if the function is
// sure to say then what it says now. Of a proxy, or a function that has been given another name, they are asked at once. This is
// functionProtoFuncBind() in runtime/FunctionPrototype.cpp, for a thread that may run no JS too: what changes there changes here.
static JSValue bindToObject(JSGlobalObject* globalObject, JSObject* function, JSObject* object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* jsFunction = dynamicDowncast<JSFunction>(function);
    bool areOriginal = jsFunction && jsFunction->canAssumeNameAndLengthAreOriginal(vm);
#if ENABLE(AOT)
    if (isSlimClosureType(function->type()))
        areOriginal = AOT::CompiledCallee::canAssumeNameAndLengthAreOriginal(vm, function);
#endif
    if (areOriginal) [[likely]]
        RELEASE_AND_RETURN(scope, JSBoundFunction::create(vm, globalObject, function, object, ArgList(), PNaN, nullptr, SourceTaintedOrigin::Untainted));
    JSValue lengthValue;
    JSValue nameValue;
    if (runtime(globalObject).threads().isMainThread()) [[likely]] {
        bool hasLength = function->hasOwnProperty(globalObject, vm.propertyNames->length);
        RETURN_IF_EXCEPTION(scope, { });
        if (hasLength) {
            lengthValue = function->get(globalObject, vm.propertyNames->length);
            RETURN_IF_EXCEPTION(scope, { });
        }
        nameValue = function->get(globalObject, vm.propertyNames->name);
        RETURN_IF_EXCEPTION(scope, { });
    } else if (inquireOwnProperty(globalObject, function, vm.propertyNames->length, lengthValue) == Inquiry::NeedsCode || inquireProperty(globalObject, function, vm.propertyNames->name, nameValue) == Inquiry::NeedsCode) {
        scope.release();
        ensureMainThreadForJS(globalObject);
        return { };
    }
    double length = lengthValue && lengthValue.isNumber() ? std::max(lengthValue.toIntegerOrInfinity(globalObject), 0.0) : 0;
    JSString* name = nameValue && nameValue.isString() ? asString(nameValue) : jsEmptyString(vm);
    RELEASE_AND_RETURN(scope, JSBoundFunction::create(vm, globalObject, function, object, ArgList(), length, name, SourceTaintedOrigin::Untainted));
}

static bool isClassWrittenInJS(JSFunction*);
static JSValue boundMethod(JSGlobalObject*, JSObject*, JSObject* function);

// A function that is a property is a method: it is called with the object as `this`, as in object.method(). The method of an object is
// one thing, however often it is read: object.method is object.method, and emitter.off(name, object.method) finds what on() was given.
static JSValue bindIfFunction(JSGlobalObject* globalObject, JSObject* object, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isObject() || isPyObject(value) || PyFunction::is(value) || PyType::is(value) || PyNativeFunction::is(value))
        return value;
    JSObject* function = asObject(value);
    if (!function->isCallable() || function->inherits<JSBoundFunction>())
        return value;
    // A class is not a method of what holds it: js.Promise is Promise itself.
    if (auto* jsFunction = dynamicDowncast<JSFunction>(function); jsFunction && !jsFunction->isHostFunction() && isClassWrittenInJS(jsFunction))
        return value;
    // Nor is what a module exports a method of its namespace.
    if (function->inherits<InternalFunction>() || object->type() == ModuleNamespaceObjectType)
        return value;
    bool isPrototype = isPrototypeOfConstructor(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (isPrototype)
        return value;
    RELEASE_AND_RETURN(scope, boundMethod(globalObject, object, function));
}

static JSValue boundMethod(JSGlobalObject* globalObject, JSObject* object, JSObject* function)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
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
    JSValue bound = methods->get(function);
    if (!bound.isCell()) {
        bound = bindToObject(globalObject, function, object);
        RETURN_IF_EXCEPTION(scope, { });
        methods->set(vm, function, bound);
    }
    return bound;
}

static bool isMappingByMethods(JSGlobalObject*, JSObject*);

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

static ALWAYS_INLINE bool isDunderDict(Runtime& rt, JSString* name)
{
    StringImpl* impl = name->tryGetValueImpl();
    StringImpl* dunderDict = rt.names.dDict()->tryGetValueImpl();
    return impl == dunderDict || (impl && !impl->isAtom() && WTF::equal(impl, dunderDict));
}

// A proxy has an answer to every read. Whether there is such a property is another question. `value`: the answer. Empty: there is none, or
// an exception.
static NEVER_INLINE JSValue valueIfProxyHasProperty(JSGlobalObject* globalObject, JSObject* holder, const Identifier& key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool hasIt = holder->hasProperty(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    return hasIt ? value : JSValue();
}

// holder[key], read for the receiver, which is the holder or inherits from it. `isOwn`: only what the holder has itself. On a thread other than
// the main one: what can be seen; `willBeBound`: and can be given to bindIfFunction() there. Empty: there is no such property, or an exception.
template<bool isOwn, bool willBeBound = false>
static ALWAYS_INLINE JSValue readProperty(JSGlobalObject* globalObject, JSObject* holder, JSObject* receiver, const Identifier& key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!runtime(globalObject).threads().isMainThread()) [[unlikely]] {
        JSValue value;
        Inquiry result = isOwn ? inquireOwnProperty(globalObject, holder, key, value) : inquireProperty(globalObject, holder, key, value);
        // To bind a function of JS's that is none of these asks it for its prototype.
        if (result == Inquiry::NeedsCode || (willBeBound && result == Inquiry::Value && value.isCallable() && !isPyObject(value) && !value.inherits<JSFunction>() && !value.inherits<InternalFunction>())) {
            scope.release();
            ensureMainThreadForJS(globalObject);
            return { };
        }
        return result == Inquiry::Value ? value : JSValue();
    }
    // Whether it has the property itself is [[GetOwnProperty]]. To a read, [[Get]], an object may answer with what it inherits too: a proxy
    // does, and the global object of a context of node:vm.
    if constexpr (isOwn) {
        PropertySlot slot(receiver, PropertySlot::InternalMethodType::GetOwnProperty);
        bool found = holder->methodTable()->getOwnPropertySlot(holder, globalObject, key, slot);
        RETURN_IF_EXCEPTION(scope, { });
        if (!found)
            return { };
        // (What it is, is for the `get` of a proxy to say.)
        if (!slot.isTaintedByOpaqueObject()) [[likely]]
            RELEASE_AND_RETURN(scope, slot.getValue(globalObject, key));
    }
    PropertySlot slot(receiver, PropertySlot::InternalMethodType::Get);
    bool found = isOwn ? holder->methodTable()->getOwnPropertySlot(holder, globalObject, key, slot) : holder->getPropertySlot(globalObject, key, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found)
        return { };
    // (The value is in the slot: the trap has run.)
    if (!isOwn && slot.isTaintedByOpaqueObject() && slot.slotBase()->type() == ProxyObjectType) [[unlikely]] {
        JSValue value = slot.getValue(globalObject, key);
        if (value.isUndefined())
            RELEASE_AND_RETURN(scope, valueIfProxyHasProperty(globalObject, holder, key, value));
        return value;
    }
    RELEASE_AND_RETURN(scope, slot.getValue(globalObject, key));
}

static JSValue getOfMappingsInPlaceOf(JSGlobalObject*, JSValue method, JSObject*);

// The same as an attribute of the receiver: a function is bound to it.
template<bool isOwn>
static ALWAYS_INLINE JSValue attributeFromProperty(JSGlobalObject* globalObject, JSObject* holder, JSObject* receiver, const Identifier& key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = readProperty<isOwn, true>(globalObject, holder, receiver, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return { };
    // mapping.get(key, default=None)
    if (key == vm.propertyNames->get) [[unlikely]] {
        JSValue method = getOfMappingsInPlaceOf(globalObject, value, receiver);
        RETURN_IF_EXCEPTION(scope, { });
        if (method)
            return method;
    }
    RELEASE_AND_RETURN(scope, bindIfFunction(globalObject, receiver, value));
}

JSValue attributeFromPropertyOfJS(JSGlobalObject* globalObject, JSObject* object, JSString* name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier key;
    if (failed(scope, propertyKeyForAttribute(globalObject, object, name, key)))
        return { };
    JSValue attribute = attributeFromProperty<false>(globalObject, object, object, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (attribute)
        return attribute;
    // A function with properties of its own is still that when it has been taken from an object as a method.
    if (auto* bound = dynamicDowncast<JSBoundFunction>(object))
        RELEASE_AND_RETURN(scope, attributeFromPropertyOfJS(globalObject, bound->targetFunction(), name));
    return { };
}

// The same for an instance of `type`, a class of Python's that derives from one of JS's. The chain of prototypes would say the same to JS, but
// what the classes of Python's have is not for JS here, so it is not made a value of JS's on the way (1.0 stays a float), and takes no JS,
// so any thread can have it.
static JSValue attributeOfInstanceOfDerivedClass(JSGlobalObject* globalObject, PyType* type, JSObject* object, JSString* name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto find = [&](const Identifier& key, JSString* name) -> JSValue {
        JSValue own = attributeFromProperty<true>(globalObject, object, object, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (own)
            return own;
        // As PyPrototypeView has it.
        if (key == vm.propertyNames->constructor)
            return type;
        // (lookup() remembers what it found, so it is asked first whether any class has the name.)
        if (type->lookup(name)) {
            if (JSValue found = lookupBeforeClassOfJS(type, name))
                return bindDescriptor(globalObject, found, object, type);
        }
        JSObject* prototype = prototypeBehindViews(object);
        return prototype ? attributeFromProperty<false>(globalObject, prototype, object, key) : JSValue();
    };
    Identifier key = propertyKeyFor(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue attribute = find(key, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (attribute || !isKeywordWithUnderscore(key)) [[likely]]
        return attribute;
    // As in propertyKeyForAttribute(). (A class written in JS has its methods by the names of JS.)
    Identifier keyword = withoutUnderscore(vm, key);
    RELEASE_AND_RETURN(scope, find(keyword, internString(vm, keyword.string())));
}

JSValue attributeOfObjectOfJS(JSGlobalObject* globalObject, JSObject* object, JSString* name)
{
    if (PyType* type = typeByPrototype(object))
        return attributeOfInstanceOfDerivedClass(globalObject, type, object, name);
    return attributeFromPropertyOfJS(globalObject, object, name);
}

static bool itemsAreProperties(JSGlobalObject*, JSObject*);

// `isOfDerivedClass`: the __getattribute__ that a class of Python's inherits from the class of JS's it derives from.
template<bool isOfDerivedClass>
static JSValue jsObjectGetAttributeImpl(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* object = asObject(self);
    // A property(), a slot or another data descriptor of the class comes before what the instance has. So does __dict__, which is the
    // mapping of its own properties. (Not a descriptor of the type: who goes past __getattribute__, as inspect.getattr_static() does,
    // expects a dict.)
    PyType* derivedClass = isOfDerivedClass ? typeByPrototype(object) : nullptr;
    if (derivedClass) {
        // (class Both(Plain, js.Map): the __dict__ that Plain gives its instances is not for one that has none.)
        if (JSValue descriptor = derivedClass->lookup(name); descriptor && !isDictDescriptorOfInstances(descriptor)) {
            PyType* descriptorType = rt.typeOf(descriptor);
            if (descriptorType->lookup(Special::Get) && (descriptorType->lookup(Special::Set) || descriptorType->lookup(Special::Delete)))
                RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, descriptor, self, derivedClass));
        }
        if (isDunderDict(rt, name))
            RELEASE_AND_RETURN(scope, propertiesOfObjectOfJS(globalObject, object));
    }
    JSValue property = derivedClass ? attributeOfInstanceOfDerivedClass(globalObject, derivedClass, object, name) : attributeFromPropertyOfJS(globalObject, object, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (property)
        return property;
    // js.null. None is undefined, and null has no name in JS that is a property of anything.
    if (object == globalObject->globalThis() && stringOf(name) == "null"_s)
        return jsNull();
    // keys(), get(), update() and the like are for an object that is a mapping, of its properties or by its methods. What else JS
    // iterates over is not: dict(iterator) takes pairs from it. Nor is a WeakSet.
    if (isNameOfMappingMethod(rt, name)) {
        if (failed(scope, ensureMainThreadForJS(globalObject)))
            return { };
        bool isIterable = hasIteratorMethod(globalObject, object);
        RETURN_IF_EXCEPTION(scope, { });
        bool byMethods = isIterable && isMappingByMethods(globalObject, object);
        RETURN_IF_EXCEPTION(scope, { });
        if (isIterable ? !byMethods : object->type() == JSWeakSetType)
            return raiseAttributeError(globalObject, scope, self, name, makeString('\'', typeName(globalObject, self), "' object has no attribute '"_s, stringOf(name), '\''));
    }
    // vars(object), function.__dict__.update(): its attributes are its properties, and it is the mapping of those itself, unless its
    // items are something else.
    if (isDunderDict(rt, name)) {
        bool isMappingOfProperties = itemsAreProperties(globalObject, object);
        RETURN_IF_EXCEPTION(scope, { });
        if (isMappingOfProperties)
            return self;
        RELEASE_AND_RETURN(scope, propertiesOfObjectOfJS(globalObject, object));
    }
    // What every Python object has: __class__, __dir__, and so on.
    RELEASE_AND_RETURN(scope, genericGetAttrOfType(globalObject, self, name));
}

static JSValue jsObjectGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    return jsObjectGetAttributeImpl<false>(globalObject, self, nameValue);
}

JSValue attributeFromPrototypeOfJS(JSGlobalObject* globalObject, JSObject* prototype, JSValue object, JSString* name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier key;
    if (failed(scope, propertyKeyForAttribute(globalObject, prototype, name, key)))
        return { };
    RELEASE_AND_RETURN(scope, attributeFromProperty<false>(globalObject, prototype, asObject(object), key));
}

// `delete object.key` in strict code. False: an exception.
static bool deletePropertyOrThrow(JSGlobalObject* globalObject, JSObject* object, const Identifier& key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isDeleted = JSCell::deleteProperty(object, globalObject, key);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isDeleted)
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
    return isDeleted;
}

static ALWAYS_INLINE void setOrDeletePropertyForAttribute(JSGlobalObject* globalObject, JSObject* object, JSString* name, JSValue value)
{
    if (!ensureMainThreadForJS(globalObject))
        return;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier key;
    if (failed(scope, propertyKeyForAttribute(globalObject, object, name, key)))
        return;
    // No other mapping can take the place of the properties, and a property of that name would be in the way of them.
    if (key.impl() == runtime(globalObject).names.dDict()->tryGetValueImpl()) [[unlikely]] {
        raiseAttributeError(globalObject, scope, object, name, makeString("attribute '__dict__' of '"_s, typeName(globalObject, object), "' objects is not writable"_s));
        return;
    }
    if (value) {
        PutPropertySlot slot(object, true);
        scope.release();
        object->methodTable()->put(object, globalObject, key, value, slot);
        return;
    }
    // What an instance of a class of Python's inherits is not its own to delete: `del instance.method`. (Of another object JS deletes
    // nothing then, and says nothing.)
    bool had = typeByPrototype(object) ? object->hasOwnProperty(globalObject, key) : object->hasProperty(globalObject, key);
    RETURN_IF_EXCEPTION(scope, void());
    if (!had) {
        raiseAttributeError(globalObject, scope, object, name, makeString('\'', typeName(globalObject, object), "' object has no attribute '"_s, stringOf(name), '\''));
        return;
    }
    scope.release();
    deletePropertyOrThrow(globalObject, object, key);
}

void setPropertyOfJSForAttribute(JSGlobalObject* globalObject, JSObject* object, JSString* name, JSValue value)
{
    setOrDeletePropertyForAttribute(globalObject, object, name, value);
}

// No value: delete.
template<bool isOfDerivedClass>
static JSValue jsObjectSetOrDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue nameValue, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    // What the class and all its bases have for the name is asked first, object among them, which is not on the chain of prototypes
    // (__class__). The property is what is left.
    if constexpr (isOfDerivedClass)
        genericSetAttrAsGiven(globalObject, self, name, nameValue, value);
    else
        setOrDeletePropertyForAttribute(globalObject, asObject(self), name, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

template<bool isOfDerivedClass>
static JSValue jsObjectDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    return jsObjectSetOrDelAttr<isOfDerivedClass>(globalObject, self, nameValue, { });
}

template<bool isOfDerivedClass>
static JSValue jsObjectStrImpl(JSGlobalObject*, JSValue self);

static constexpr NativeDef methodsOfDerivedClasses[] = {
    PY_SLOT("__getattribute__", jsObjectGetAttributeImpl<true>, "($self, name, /)"),
    PY_SLOT("__setattr__", jsObjectSetOrDelAttr<true>, "($self, name, value, /)"),
    PY_SLOT("__delattr__", jsObjectDelAttr<true>, "($self, name, /)"),
    PY_SLOT("__str__", jsObjectStrImpl<true>, "($self, /)"),
};

void addMethodsOfDerivedClasses(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    for (auto& def : methodsOfDerivedClasses)
        type->dict()->set(vm, internString(vm, def.name), PyNativeFunction::create(globalObject, def, NativeKind::SlotWrapper, { }, type));
    type->didChange(vm);
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

// Whether the object is a mapping by its methods: a Map, Headers, URLSearchParams, FormData. That is an object with a class that JS
// iterates over and that has get(), has() and keys().
static bool isMappingByMethods(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (object->type() == JSMapType)
        return true;
    if (typeOf(globalObject, object) == runtime(globalObject).jsPlainObjectType())
        return false;
    bool isIterable = hasIteratorMethod(globalObject, object);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isIterable)
        return false;
    for (auto& name : { vm.propertyNames->get, vm.propertyNames->has, vm.propertyNames->keys }) {
        JSValue method = object->get(globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (!method.isCallable())
            return false;
    }
    return true;
}

// What is in them is not their properties, and nobody can say what it is.
static bool isWeakCollection(JSObject* object)
{
    return object->type() == JSWeakMapType || object->type() == JSWeakSetType;
}

static JSValue raiseNotIterable(JSGlobalObject* globalObject, ThrowScope& scope, JSValue object)
{
    return raiseTypeError(globalObject, scope, '\'', typeName(globalObject, object), "' object is not iterable"_s);
}

// What the object has under the name, if that can be told without running code of the program's. As inquireProperty(), but that on the main
// thread what the host makes when it is first asked for is made, and that what the classes of Python's have is looked at as it is in them.
// (What goes wrong in the host is thrown.)
static Inquiry inquirePropertyOfHost(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue& value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    bool isMainThread = rt.threads().isMainThread();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Most objects show all there is to who only looks.
    if (isMainThread) [[likely]] {
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool found = object->getPropertySlot(globalObject, name, slot);
        scope.assertNoException();
        if (!slot.isTaintedByOpaqueObject()) [[likely]] {
            if (!found)
                return Inquiry::Absent;
            if (!slot.isValue())
                return Inquiry::NeedsCode;
            value = slot.getValue(globalObject, name);
            scope.assertNoException();
            return Inquiry::Value;
        }
    }
    for (JSObject* holder = object;;) {
        if (auto* proxy = dynamicDowncast<JSGlobalProxy>(holder))
            holder = proxy->target();
        if (auto* view = holder->classInfo() == PyPrototypeView::info() ? uncheckedDowncast<PyPrototypeView>(holder) : nullptr; view && !name.isSymbol()) {
            // A function is a method, whoever wrote it, and so is what staticmethod() and classmethod() have. Anything else may be a descriptor,
            // which would have to be asked.
            if (JSValue found = lookupBeforeClassOfJS(view->type(), internString(vm, String(name.uid())))) {
                value = found;
                PyType* type = rt.typeOf(found);
                bool isMethod = PyFunction::is(found) || PyNativeFunction::is(found) || type == rt.jsFunctionType() || type == jsMethodType(rt) || type == rt.staticmethodType() || type == rt.classmethodType();
                return isMethod ? Inquiry::Value : Inquiry::NeedsCode;
            }
            while (holder->classInfo() == PyPrototypeView::info() && holder->getPrototypeDirect().isObject())
                holder = asObject(holder->getPrototypeDirect());
        }
        // (What is Python's answers with its attributes.)
        if (holder->type() == ProxyObjectType || holder->structure()->typeInfo().overridesGetPrototype() || (propertiesArePythons(holder) && !name.isSymbol()))
            return Inquiry::NeedsCode;
        if (!isMainThread) [[unlikely]] {
            if (Inquiry result = inquireOwnProperty(globalObject, holder, name, value); result != Inquiry::Absent)
                return result;
        } else {
            PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
            bool found = holder->methodTable()->getOwnPropertySlot(holder, globalObject, name, slot);
            RETURN_IF_EXCEPTION(scope, Inquiry::NeedsCode);
            if (found) {
                if (!slot.isValue())
                    return Inquiry::NeedsCode;
                value = slot.getValue(globalObject, name);
                scope.assertNoException();
                return Inquiry::Value;
            }
        }
        JSValue prototype = holder->getPrototypeDirect();
        if (!prototype.isObject())
            return Inquiry::Absent;
        holder = asObject(prototype);
    }
}

// Whether iterating over the object gives the names of its properties, and object[name] their values. For who reads object.__dict__, which is
// to run nothing that the read of an attribute does not run: where it would take a getter to tell, no. (A proxy is asked: every read of
// an attribute of one runs its traps.)
static bool itemsAreProperties(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSType type = object->type(); type == JSMapType || type == JSSetType || isWeakCollection(object) || isTypedArrayType(type))
        return false;
    JSValue method;
    Inquiry result = inquirePropertyOfHost(globalObject, object, vm.propertyNames->iteratorSymbol, method);
    RETURN_IF_EXCEPTION(scope, false);
    if (result == Inquiry::NeedsCode && object->type() == ProxyObjectType && runtime(globalObject).threads().isMainThread()) {
        method = object->get(globalObject, vm.propertyNames->iteratorSymbol);
        RETURN_IF_EXCEPTION(scope, false);
        result = Inquiry::Value;
    }
    return result == Inquiry::Absent || (result == Inquiry::Value && method.isUndefinedOrNull());
}

// A plain object is a mapping by its type, which is all that `match` asks of anything else. One that JS iterates over is none.
bool plainObjectIsMapping(JSGlobalObject* globalObject, JSObject* object)
{
    return itemsAreProperties(globalObject, object);
}

// Whether object[key] goes by its methods: a mapping by its methods, or a WeakMap, which is one that cannot be iterated over.
static bool hasItemsByMethods(JSGlobalObject* globalObject, JSObject* object)
{
    return object->type() == JSWeakMapType || isMappingByMethods(globalObject, object);
}

// The get() of a Map, of Headers and the like, and of a WeakMap knows no default and ignores a second argument. To Python they are mappings,
// and every mapping has get(key, default=None). So for them `get` is a method written in C++ that has (jsMappingGet), and not the property.
// It is a get() written in C++ with one parameter that gives way. One written in JS says itself
// what its arguments mean (`get(key, options)` of a cache), and so may one of an addon, whose functions declare no parameters.
// To read an attribute is to run nothing that has nothing to do with it, so this goes by what can be told without: an object that has
// getters for its other methods, or is a proxy, keeps its get().
static bool givesWayToGetOfMappings(JSGlobalObject* globalObject, JSValue method, JSObject* object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // (What was compiled ahead of time is a host function to the engine, and written in JS all the same.)
    auto* function = dynamicDowncast<JSFunction>(method);
    if (!function || !function->isHostFunction() || function->isCompiledClosure() || function->inherits<JSBoundFunction>() || function->originalLength(vm) != 1)
        return false;
    // Map.prototype.get is JS's, as everything is that is read off a prototype: see bindIfFunction().
    bool isPrototype = isPrototypeOfConstructor(globalObject, object);
    RETURN_IF_EXCEPTION(scope, false);
    if (isPrototype)
        return false;
    // As hasItemsByMethods() has it.
    if (JSType type = object->type(); type == JSMapType || type == JSWeakMapType)
        return true;
    if (typeOf(globalObject, object) == runtime(globalObject).jsPlainObjectType())
        return false;
    for (PropertyName name : { PropertyName(vm.propertyNames->iteratorSymbol), PropertyName(vm.propertyNames->has), PropertyName(vm.propertyNames->keys) }) {
        JSValue other;
        Inquiry result = inquirePropertyOfHost(globalObject, object, name, other);
        RETURN_IF_EXCEPTION(scope, false);
        // (What staticmethod() has is taken to be a function.)
        if (result != Inquiry::Value || (!other.isCallable() && !isPyObject(other)))
            return false;
    }
    return true;
}

// object.name(...arguments)
static JSValue callMethodOfJS(JSGlobalObject* globalObject, JSObject* object, const Identifier& name, std::initializer_list<JSValue> arguments = { })
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = object->get(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    auto callData = JSC::getCallData(method);
    if (callData.type == CallData::Type::None)
        return raiseTypeError(globalObject, scope, "this JavaScript object has no method "_s, name.string(), "()"_s);
    MarkedArgumentBuffer list;
    for (JSValue argument : arguments)
        list.append(valueForJS(argument));
    JSValue result = JSC::call(globalObject, method, callData, object, list);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

// [...object.keys()]. Null: an exception.
static JSArray* keysByMethod(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue keys = callMethodOfJS(globalObject, object, vm.propertyNames->keys);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, listFromIterable(globalObject, keys));
}

// The Map, if these methods of it are Map's own: then there is no need to call them. A class derived from Map may have others.
static JSMap* mapWithItsOwnMethods(JSGlobalObject* globalObject, JSValue self, std::initializer_list<std::pair<const Identifier&, Intrinsic>> methods)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = dynamicDowncast<JSMap>(self);
    if (!map)
        return nullptr;
    for (auto& [name, intrinsic] : methods) {
        JSValue method = map->JSObject::get(globalObject, name);
        RETURN_IF_EXCEPTION(scope, nullptr);
        auto* function = dynamicDowncast<JSFunction>(method);
        if (!function || function->intrinsic() != intrinsic)
            return nullptr;
    }
    return map;
}

// mapping.has(key). Map's own need not be called.
static ALWAYS_INLINE bool hasKeyByMethod(JSGlobalObject* globalObject, JSObject* object, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSMap* map = mapWithItsOwnMethods(globalObject, object, { { vm.propertyNames->has, JSMapHasIntrinsic } });
    RETURN_IF_EXCEPTION(scope, false);
    if (map)
        RELEASE_AND_RETURN(scope, map->has(globalObject, valueForJS(key)));
    JSValue has = callMethodOfJS(globalObject, object, vm.propertyNames->has, { key });
    RETURN_IF_EXCEPTION(scope, false);
    return has.toBoolean(globalObject);
}

// The items of an object are its properties. Those of what is a mapping by its methods are what the methods say.
// `isAbsent`: there is none, which is no exception yet. `refusesOtherKeys`: what can name no property is a TypeError, as opposed to not there.
static JSValue itemOfObjectOfJS(JSGlobalObject* globalObject, JSValue self, JSValue key, bool& isAbsent, bool refusesOtherKeys)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(self);
    JSMap* map = mapWithItsOwnMethods(globalObject, self, { { vm.propertyNames->has, JSMapHasIntrinsic }, { vm.propertyNames->get, JSMapGetIntrinsic } });
    RETURN_IF_EXCEPTION(scope, { });
    if (map) {
        JSValue value = map->get(globalObject, valueForJS(key));
        RETURN_IF_EXCEPTION(scope, { });
        if (value.isUndefined()) {
            isAbsent = !map->has(globalObject, valueForJS(key));
            RETURN_IF_EXCEPTION(scope, { });
        }
        return value;
    }
    bool byMethods = hasItemsByMethods(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (byMethods) {
        isAbsent = !hasKeyByMethod(globalObject, object, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (isAbsent)
            return { };
        RELEASE_AND_RETURN(scope, callMethodOfJS(globalObject, object, vm.propertyNames->get, { key }));
    }
    if (!refusesOtherKeys && !isStr(key) && !isInt(key) && !key.isSymbol()) {
        isAbsent = true;
        return { };
    }
    Identifier name;
    if (failed(scope, propertyKeyForItem(globalObject, key, name)))
        return { };
    JSValue value = readProperty<false>(globalObject, object, object, name);
    RETURN_IF_EXCEPTION(scope, { });
    isAbsent = !value;
    return value;
}

static JSValue jsObjectGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isAbsent = false;
    JSValue value = itemOfObjectOfJS(globalObject, self, key, isAbsent, true);
    RETURN_IF_EXCEPTION(scope, { });
    if (isAbsent)
        return raiseKeyError(globalObject, scope, key);
    return value;
}

static JSValue jsObjectSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSMap* map = mapWithItsOwnMethods(globalObject, self, { { vm.propertyNames->set, JSMapSetIntrinsic } });
    RETURN_IF_EXCEPTION(scope, { });
    if (map) {
        map->set(globalObject, valueForJS(key), valueForJS(value));
        RETURN_IF_EXCEPTION(scope, { });
        return jsUndefined();
    }
    bool byMethods = hasItemsByMethods(globalObject, asObject(self));
    RETURN_IF_EXCEPTION(scope, { });
    if (byMethods) {
        callMethodOfJS(globalObject, asObject(self), vm.propertyNames->set, { key, value });
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
    JSMap* map = mapWithItsOwnMethods(globalObject, self, { { vm.propertyNames->has, JSMapHasIntrinsic }, { vm.propertyNames->deleteKeyword, JSMapDeleteIntrinsic } });
    RETURN_IF_EXCEPTION(scope, { });
    if (map) {
        bool had = map->remove(globalObject, valueForJS(key));
        RETURN_IF_EXCEPTION(scope, { });
        if (!had)
            return raiseKeyError(globalObject, scope, key);
        return jsUndefined();
    }
    bool byMethods = hasItemsByMethods(globalObject, asObject(self));
    RETURN_IF_EXCEPTION(scope, { });
    if (byMethods) {
        bool has = hasKeyByMethod(globalObject, asObject(self), key);
        RETURN_IF_EXCEPTION(scope, { });
        if (!has)
            return raiseKeyError(globalObject, scope, key);
        callMethodOfJS(globalObject, asObject(self), vm.propertyNames->deleteKeyword, { key });
        RETURN_IF_EXCEPTION(scope, { });
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
    scope.release();
    return deletePropertyOrThrow(globalObject, object, name) ? jsUndefined() : JSValue();
}

static JSValue jsObjectContains(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (asObject(self)->type() == JSMapType)
        RELEASE_AND_RETURN(scope, jsBoolean(hasKeyByMethod(globalObject, asObject(self), key)));
    if (auto* set = dynamicDowncast<JSSet>(self))
        RELEASE_AND_RETURN(scope, jsBoolean(set->has(globalObject, valueForJS(key))));
    bool byMethods = asObject(self)->type() == JSWeakSetType || hasItemsByMethods(globalObject, asObject(self));
    RETURN_IF_EXCEPTION(scope, { });
    if (byMethods)
        RELEASE_AND_RETURN(scope, jsBoolean(hasKeyByMethod(globalObject, asObject(self), key)));
    Identifier name;
    if (failed(scope, propertyKeyForItem(globalObject, key, name))) {
        takePendingException(globalObject);
        return jsBoolean(false);
    }
    bool has = asObject(self)->hasProperty(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(has);
}

static JSArray* listOfNames(JSGlobalObject* globalObject, const PropertyNameArrayBuilder& names, bool isReversed = false)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (size_t i = 0; i < names.size(); ++i) {
        listAppend(globalObject, list, jsString(vm, names[isReversed ? names.size() - 1 - i : i].string()));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return list;
}

static JSArray* ownPropertyNames(JSGlobalObject* globalObject, JSObject* object, DontEnumPropertiesMode mode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, globalObject, names, mode);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, listOfNames(globalObject, names));
}

// ---- Iterators of JS, as iterators of Python ----

// The fields of the records that those are. The iterator is empty once it is exhausted.
enum JSIteratorField : unsigned { JSIteratorObject, JSIteratorNextMethod, JSIteratorThen, JSIteratorIsOfLoop, JSIteratorFieldCount };

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

// iterator.return(), by which whoever stops before the end says so in JS: `for await` does when it is left. A promise.
static JSValue jsAsyncIteratorClose(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = uncheckedDowncast<PyRecord>(self.asCell())->get(JSIteratorObject);
    JSValue returnMethod = iterator.get(globalObject, vm.propertyNames->returnKeyword);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = jsUndefined();
    if (!returnMethod.isUndefinedOrNull()) {
        auto callData = JSC::getCallData(returnMethod);
        if (callData.type == CallData::Type::None)
            return throwTypeError(globalObject, scope, "iterator.return is not a function"_s), JSValue();
        MarkedArgumentBuffer noArguments;
        result = JSC::call(globalObject, returnMethod, callData, iterator, noArguments);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSPromise::resolvedPromise(globalObject, result));
}

static constexpr NativeDef jsAsyncIteratorMethods[] = {
    PY_SLOT("__aiter__", jsIteratorSelf, "($self, /)"),
    PY_SLOT("__anext__", jsAsyncIteratorNext, "($self, /)"),
    PY_METHOD("aclose", jsAsyncIteratorClose, "($self, /)"),
};
static constexpr TypeDef jsAsyncIteratorTypeDef { "_js.async_iterator"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, jsAsyncIteratorMethods, { }, { } };

// What JS iterates, if the object is iterable there; otherwise the names of its properties, as for a dict. The keys too of what is a
// mapping by its methods.
static JSValue jsObjectIter(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(self);
    bool byMethods = isMappingByMethods(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (byMethods) {
        JSValue keys = callMethodOfJS(globalObject, object, vm.propertyNames->keys);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, getIter(globalObject, keys));
    }
    bool isIterable = hasIteratorMethod(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isIterable) {
        if (isWeakCollection(object))
            return raiseNotIterable(globalObject, scope, self);
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

// An object of JS counts on whoever iterates over it to go by the protocol of JS, in which a loop that is left before the end tells
// the iterator (a timer is cleared, a stream is cancelled and unlocked). So `async for` does that, for the iterator it has made
// itself of an object of JS. One that the program made (aiter()) is the program's to close: aclose().
JSValue getAIterOfLoop(JSGlobalObject* globalObject, JSValue iterable)
{
    JSValue iterator = getAIter(globalObject, iterable);
    if (!iterator || iterator == iterable)
        return iterator;
    if (auto* record = PyRecord::tryFrom(iterator, runtime(globalObject).typeFor(jsAsyncIteratorTypeDef))) [[unlikely]]
        record->set(getVM(globalObject), JSIteratorIsOfLoop, jsBoolean(true));
    return iterator;
}

JSValue leaveAsyncFor(JSGlobalObject* globalObject, JSValue iterator, JSValue exception, bool waits)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* record = PyRecord::tryFrom(iterator, rt.typeFor(jsAsyncIteratorTypeDef));
    if (!record || !record->get(JSIteratorIsOfLoop)) [[likely]]
        return jsUndefined();
    record->set(vm, JSIteratorIsOfLoop, { });
    JSValue promise = jsAsyncIteratorClose(globalObject, iterator);
    RETURN_IF_EXCEPTION(scope, { });
    // A coroutine that is being closed cannot wait.
    if (!waits || (exception && rt.typeOf(exception)->isSubtypeOf(rt.GeneratorExit()))) {
        uncheckedDowncast<JSPromise>(promise.asCell())->markAsHandled();
        return jsUndefined();
    }
    return promise;
}

// keys(), values(), items(), for an object that has no properties of these names: it is a mapping of its properties, so dict(object).
enum class JSObjectPart : uint8_t { Keys, Values, Items };
static JSValue jsObjectParts(JSGlobalObject* globalObject, JSValue self, JSObjectPart part)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool byMethods = isMappingByMethods(globalObject, asObject(self));
    RETURN_IF_EXCEPTION(scope, { });
    if (!byMethods && isWeakCollection(asObject(self)))
        return raiseNotIterable(globalObject, scope, self);
    JSArray* names = byMethods ? keysByMethod(globalObject, asObject(self)) : ownPropertyNames(globalObject, asObject(self), DontEnumPropertiesMode::Exclude);
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

static bool isJSProperties(JSGlobalObject*, JSValue);
static JSObject* objectOfProperties(JSValue self);
static JSValue jsPropertiesCopy(JSGlobalObject*, JSValue self);

bool isMappingOfJSForJSON(JSGlobalObject* globalObject, JSValue value)
{
    return value.isCell() && (value.asCell()->type() == JSMapType || typeOf(globalObject, value) == runtime(globalObject).jsPlainObjectType());
}

JSArray* itemsOfMappingOfJS(JSGlobalObject* globalObject, JSObject* mapping)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (mapping->type() != JSMapType) {
        JSValue items = jsObjectItems(globalObject, mapping);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return asArray(items);
    }
    if (failed(scope, ensureMainThreadForJS(globalObject)))
        return nullptr;
    // A class of Python's with an items() of its own says what the items are, as one derived from dict does.
    if (PyType* type = typeByPrototype(mapping); type && lookupBeforeClassOfJS(type, runtime(globalObject).names.items())) {
        JSValue view = callMethod(globalObject, mapping, runtime(globalObject).names.items());
        RETURN_IF_EXCEPTION(scope, nullptr);
        RELEASE_AND_RETURN(scope, listFromIterable(globalObject, view));
    }
    // The entries as they are: a key need not be one that [] finds again (a float that JS was given in a list).
    JSValue entries = callMethodOfJS(globalObject, mapping, vm.propertyNames->builtinNames().entriesPublicName());
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* items = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    forEach(globalObject, entries, [&](JSValue entry) {
        // What is no pair is for the caller to complain about.
        JSArray* pair = tryAsList(entry);
        listAppend(globalObject, items, pair && listLength(pair) == 2 ? JSValue(PyTuple::create(globalObject, { listGet(pair, 0), listGet(pair, 1) })) : entry);
        return !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, nullptr);
    return items;
}

PyDict* dictOfJSProperties(JSGlobalObject* globalObject, JSValue value, JSObject*& object)
{
    if (!isJSProperties(globalObject, value))
        return nullptr;
    object = objectOfProperties(value);
    JSValue dict = jsPropertiesCopy(globalObject, value);
    return dict ? PyDict::from(dict) : nullptr;
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
    // As in jsObjectIter(): it is the object's properties that are counted only if it is they that are iterated over. A generator
    // has no length, and code that asks for one to see whether there is one must be told so.
    bool byMethods = isMappingByMethods(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    bool isIterable = hasIteratorMethod(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (!byMethods && (isIterable || isWeakCollection(object)))
        return raiseTypeError(globalObject, scope, "object of type '"_s, typeName(globalObject, self), "' has no len()"_s);
    JSArray* names = byMethods ? keysByMethod(globalObject, object) : ownPropertyNames(globalObject, object, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(listLength(names));
}

// Every object is true to JS. To Python a container is false when it is empty, so that goes for what is a container to it: what JS
// iterates over and has a length or size, what is a mapping by its methods, and a plain object, which is one of its properties.
// Anything else is true, as an instance of a class in Python is: a function, a promise, an Error, a Date, a Response.
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
        bool byMethods = isMappingByMethods(globalObject, object);
        RETURN_IF_EXCEPTION(scope, { });
        if (!byMethods)
            return jsBoolean(true);
    } else if (typeOf(globalObject, self) != runtime(globalObject).jsPlainObjectType())
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

// The get() of an object that is a mapping. As that of a dict, it goes by what is in the mapping, whatever __getitem__ a class derived from
// the object's has. A key that can name no property is not there, as for `in`.
static JSValue jsObjectGet(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isAbsent = false;
    JSValue value = itemOfObjectOfJS(globalObject, self, key, isAbsent, false);
    RETURN_IF_EXCEPTION(scope, { });
    return isAbsent ? (defaultValue ? defaultValue : jsUndefined()) : value;
}

// What is read for mapping.get: see givesWayToGetOfMappings(). It is the get() that was found which is called, and the has() of the mapping that is
// asked. `self`: that get(), bound to the mapping.
static JSValue jsMappingGet(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* method = uncheckedDowncast<JSBoundFunction>(self.asCell());
    JSObject* object = asObject(method->boundThis());
    JSValue value;
    // Map's own need not be called.
    if (auto* map = dynamicDowncast<JSMap>(object); map && uncheckedDowncast<JSFunction>(method->targetFunction())->intrinsic() == JSMapGetIntrinsic)
        value = map->get(globalObject, valueForJS(key));
    else {
        MarkedArgumentBuffer list;
        list.append(valueForJS(key));
        value = JSC::call(globalObject, method->targetFunction(), object, list, "not a function"_s);
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (!defaultValue || !value.isUndefinedOrNull())
        return value;
    bool has = hasKeyByMethod(globalObject, object, key);
    RETURN_IF_EXCEPTION(scope, { });
    return has ? value : defaultValue;
}
static constexpr NativeDef jsMappingGetDef = PY_METHOD_WITH_FLAGS("get", jsMappingGet, "($self, key, default=None)", OptionalParametersAreNotForJS);

// `method`: what was found for object.get. Empty: it stays, or an exception.
static JSValue getOfMappingsInPlaceOf(JSGlobalObject* globalObject, JSValue method, JSObject* object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool givesWay = givesWayToGetOfMappings(globalObject, method, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (!givesWay)
        return { };
    JSValue bound = boundMethod(globalObject, object, asObject(method));
    RETURN_IF_EXCEPTION(scope, { });
    return PyNativeFunction::create(globalObject, jsMappingGetDef, NativeKind::Function, bound, JSValue());
}

// MutableMapping.pop() asks `self` for a mark of the class.
static JSValue jsObjectPop(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = jsObjectGetItem(globalObject, self, key);
    if (failed(scope, value)) {
        if (!defaultValue)
            return { };
        scope.release();
        return clearPendingExceptionIfMatches(globalObject, runtime(globalObject).KeyError()) ? defaultValue : JSValue();
    }
    scope.release();
    return jsObjectDelItem(globalObject, self, key) ? value : JSValue();
}

// ---- object.__dict__, for an instance of a class of Python's that derives from one of JS's, and for what has other items than its properties ----

// The mapping of the object's own properties that have a str for a name: they are what another instance has in its __dict__. The object
// itself will not do: object[key] finds what it inherits too, the attributes of its class among that (so functools.cached_property
// would find itself), and a Map is the mapping of its entries. The elements of a typed array are no attributes of it.
enum JSPropertiesField : unsigned { JSPropertiesObject, JSPropertiesFieldCount };

static JSObject* objectOfProperties(JSValue self) { return asObject(uncheckedDowncast<PyRecord>(self.asCell())->get(JSPropertiesObject)); }

// False: an exception, or, without one, the key is the name of no attribute.
static bool nameOfAttributeFor(JSGlobalObject* globalObject, JSObject* object, JSValue key, Identifier& name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What cannot be hashed is refused, as by the dict. (A class derived from str may have a __hash__ of its own.)
    if (!isExactStr(key)) {
        hash(globalObject, key);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isStr(key))
            return false;
    }
    name = propertyKeyFor(globalObject, strValue(key));
    RETURN_IF_EXCEPTION(scope, false);
    return !isTypedArrayType(object->type()) || !isCanonicalNumericIndexString(name.impl());
}

// Whether the object has the property. `value`, if not null, takes what it is: to ask that may run a getter.
static bool ownPropertyFor(JSGlobalObject* globalObject, JSObject* object, JSValue key, Identifier& name, JSValue* value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isName = nameOfAttributeFor(globalObject, object, key, name);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isName)
        return false;
    if (value || !runtime(globalObject).threads().isMainThread()) {
        JSValue read = readProperty<true>(globalObject, object, object, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (value)
            *value = read;
        return !!read;
    }
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    RELEASE_AND_RETURN(scope, object->methodTable()->getOwnPropertySlot(object, globalObject, name, slot));
}

static JSValue jsPropertiesGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier name;
    JSValue value;
    bool found = ownPropertyFor(globalObject, objectOfProperties(self), key, name, &value);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found)
        return raiseKeyError(globalObject, scope, key);
    return value;
}

static JSValue jsPropertiesContains(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier name;
    bool found = ownPropertyFor(globalObject, objectOfProperties(self), key, name, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

// As a store into a __dict__, this goes past whatever the class has for the name: the property is defined, not assigned to. One that is
// there and holds a value is given the new one and stays as it is otherwise.
static JSValue jsPropertiesSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = objectOfProperties(self);
    Identifier name;
    bool isName = nameOfAttributeFor(globalObject, object, key, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isName) {
        if (isStr(key))
            return raiseTypeError(globalObject, scope, "an element of a typed array is no attribute of it"_s);
        return raiseTypeError(globalObject, scope, "the names of the properties of a JavaScript object are str, not "_s, typeName(globalObject, key));
    }
    PropertyDescriptor present;
    bool found = object->getOwnPropertyDescriptor(globalObject, name, present);
    RETURN_IF_EXCEPTION(scope, { });
    if (found && present.isDataDescriptor()) {
        PropertyDescriptor onlyValue;
        onlyValue.setValue(value);
        object->methodTable()->defineOwnProperty(object, globalObject, name, onlyValue, true);
    } else
        object->createDataProperty(globalObject, name, value, true);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

// `value`: takes what the property was, if it is not null.
static bool removeProperty(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue* value, bool& found)
{
    found = false;
    if (!ensureMainThreadForJS(globalObject))
        return false;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = objectOfProperties(self);
    Identifier name;
    found = ownPropertyFor(globalObject, object, key, name, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (!found)
        return true;
    RELEASE_AND_RETURN(scope, deletePropertyOrThrow(globalObject, object, name));
}

static JSValue jsPropertiesDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool found;
    if (failed(scope, removeProperty(globalObject, self, key, nullptr, found)))
        return { };
    if (!found)
        return raiseKeyError(globalObject, scope, key);
    return jsUndefined();
}

// False: an exception.
static bool namesOfProperties(JSGlobalObject* globalObject, JSValue self, PropertyNameArrayBuilder& names)
{
    if (!ensureMainThreadForJS(globalObject))
        return false;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = objectOfProperties(self);
    if (isTypedArrayType(object->type()))
        object->getOwnNonIndexPropertyNames(globalObject, names, DontEnumPropertiesMode::Include);
    else
        object->methodTable()->getOwnPropertyNames(object, globalObject, names, DontEnumPropertiesMode::Include);
    return !scope.exception();
}

template<bool isReversed>
static JSValue jsPropertiesIter(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    if (failed(scope, namesOfProperties(globalObject, self, names)))
        return { };
    JSArray* list = listOfNames(globalObject, names, isReversed);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getIter(globalObject, list));
}

static JSValue jsPropertiesLen(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    if (failed(scope, namesOfProperties(globalObject, self, names)))
        return { };
    return jsNumber(names.size());
}

// (MutableMapping.pop() asks `self` for a mark of the class.)
static JSValue jsPropertiesPop(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value;
    bool found;
    if (failed(scope, removeProperty(globalObject, self, key, &value, found)))
        return { };
    if (found)
        return value;
    if (defaultValue)
        return defaultValue;
    return raiseKeyError(globalObject, scope, key);
}

// The last, as of a dict. (MutableMapping.popitem() takes the first.)
static JSValue jsPropertiesPopItem(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    if (failed(scope, namesOfProperties(globalObject, self, names)))
        return { };
    if (!names.size())
        return raiseKeyError(globalObject, scope, makeStr(vm, "popitem(): dictionary is empty"_s));
    JSValue key = jsString(vm, names[names.size() - 1].string());
    JSValue value;
    bool found;
    if (failed(scope, removeProperty(globalObject, self, key, &value, found)))
        return { };
    return PyTuple::create(globalObject, { key, found ? value : jsUndefined() });
}

static JSValue jsPropertiesClear(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    if (failed(scope, namesOfProperties(globalObject, self, names)))
        return { };
    for (auto& name : names) {
        if (failed(scope, deletePropertyOrThrow(globalObject, objectOfProperties(self), name)))
            return { };
    }
    return jsUndefined();
}

// The dict that it stands for, as it is now.
static JSValue jsPropertiesCopy(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = PyDict::create(globalObject);
    dictUpdateFromMapping(globalObject, dict, self);
    RETURN_IF_EXCEPTION(scope, { });
    return dict;
}

// To copy.copy(), copy.deepcopy() and pickle it is that dict: `copy.deepcopy(self.__dict__)` in a __deepcopy__(). An empty one that is then
// given the items, so that they know of it before they come to what it holds, which may lead back to it.
static JSValue jsPropertiesReduce(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue dict = jsPropertiesCopy(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue items = callMethod(globalObject, dict, internString(vm, "items"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue iterator = getIter(globalObject, items);
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { runtime(globalObject).dictType(), PyTuple::create(globalObject, 0), jsUndefined(), jsUndefined(), iterator });
}

static JSValue jsPropertiesRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Every instance.__dict__ is another object, of the same properties.
    ReprGuard guard(globalObject, objectOfProperties(self));
    if (guard.isRecursive())
        return makeStr(vm, "{...}"_s);
    JSValue dict = jsPropertiesCopy(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, repr(globalObject, dict));
}

// With a dict, or another of these, as a dict: the result is one.
template<bool isReflected>
static JSValue jsPropertiesOr(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyDict::is(other) && !isJSProperties(globalObject, other))
        return runtime(globalObject).notImplemented();
    PyDict* dict = PyDict::create(globalObject);
    dictUpdateFromMapping(globalObject, dict, isReflected ? other : self);
    RETURN_IF_EXCEPTION(scope, { });
    dictUpdateFromMapping(globalObject, dict, isReflected ? self : other);
    RETURN_IF_EXCEPTION(scope, { });
    return dict;
}

static JSValue jsPropertiesIOr(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    callMethod(globalObject, self, internString(vm, "update"_s), other);
    RETURN_IF_EXCEPTION(scope, { });
    return self;
}

static constexpr NativeDef jsPropertiesMethods[] = {
    PY_SLOT("__getitem__", jsPropertiesGetItem, "($self, key, /)"),
    PY_SLOT("__setitem__", jsPropertiesSetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", jsPropertiesDelItem, "($self, key, /)"),
    PY_SLOT("__contains__", jsPropertiesContains, "($self, key, /)"),
    PY_SLOT("__iter__", jsPropertiesIter<false>, "($self, /)"),
    PY_SLOT("__len__", jsPropertiesLen, "($self, /)"),
    PY_SLOT("__repr__", jsPropertiesRepr, "($self, /)"),
    PY_SLOT("__eq__", jsObjectMappingMethod<"__eq__">, "($self, *args, **kwargs)"),
    PY_SLOT("__or__", jsPropertiesOr<false>, "($self, value, /)"),
    PY_SLOT("__ror__", jsPropertiesOr<true>, "($self, value, /)"),
    PY_SLOT("__ior__", jsPropertiesIOr, "($self, value, /)"),
    PY_METHOD("__reversed__", jsPropertiesIter<true>, "($self, /)"),
    PY_METHOD("__reduce__", jsPropertiesReduce, "($self, /)"),
    PY_METHOD("copy", jsPropertiesCopy, "($self, /)"),
    PY_METHOD("keys", jsObjectMappingMethod<"keys">, "($self, *args, **kwargs)"),
    PY_METHOD("values", jsObjectMappingMethod<"values">, "($self, *args, **kwargs)"),
    PY_METHOD("items", jsObjectMappingMethod<"items">, "($self, *args, **kwargs)"),
    PY_METHOD("get", jsObjectMappingMethod<"get">, "($self, *args, **kwargs)"),
    PY_METHOD("pop", jsPropertiesPop, "($self, key, default=<unrepresentable>, /)"),
    PY_METHOD("popitem", jsPropertiesPopItem, "($self, /)"),
    PY_METHOD("setdefault", jsObjectMappingMethod<"setdefault">, "($self, *args, **kwargs)"),
    PY_METHOD("update", jsObjectMappingMethod<"update">, "($self, *args, **kwargs)"),
    PY_METHOD("clear", jsPropertiesClear, "($self, /)"),
};

static void registerAsMutableMapping(JSGlobalObject* globalObject, std::initializer_list<PyType*> types)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, "_collections_abc"_s);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue mutableMapping = getAttr(globalObject, module, internString(vm, "MutableMapping"_s));
    RETURN_IF_EXCEPTION(scope, void());
    for (PyType* type : types) {
        callMethod(globalObject, mutableMapping, internString(vm, "register"_s), type);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

static constexpr TypeDef jsPropertiesTypeDef { "_js.JSProperties"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation, TypeFlag::Mapping }, jsPropertiesMethods, { }, { } };

static bool isJSProperties(JSGlobalObject* globalObject, JSValue value)
{
    return typeOf(globalObject, value) == runtime(globalObject).typeIfMadeFor(jsPropertiesTypeDef);
}

// Null: an exception.
static PyType* jsPropertiesType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = rt.typeFor(jsPropertiesTypeDef);
    // When it is first asked for: collections.abc.MutableMapping.register(JSProperties), and then, with __eq__ and without __hash__, it cannot be hashed.
    if (!type->dict()->get(rt.name(Special::Hash))) [[unlikely]] {
        registerAsMutableMapping(globalObject, { type });
        RETURN_IF_EXCEPTION(scope, nullptr);
        type->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
        type->didChange(vm);
    }
    return type;
}

JSValue propertiesOfObjectOfJS(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = jsPropertiesType(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = PyRecord::create(globalObject, type, JSPropertiesFieldCount);
    record->set(vm, JSPropertiesObject, object);
    return record;
}

// ---- `with object:` is `using`, and `async with object:` is `await using` ----

// object[Symbol.dispose], or for `await using` object[Symbol.asyncDispose] before that. Empty: a TypeError, or another exception.
static JSValue disposeMethod(JSGlobalObject* globalObject, JSValue self, bool isAsync)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue other;
    for (bool wantsAsync : { true, false }) {
        JSValue method = asObject(self)->get(globalObject, wantsAsync ? vm.propertyNames->asyncDisposeSymbol : vm.propertyNames->disposeSymbol);
        RETURN_IF_EXCEPTION(scope, { });
        if (!method.isCallable())
            continue;
        if (isAsync || !wantsAsync)
            return method;
        other = method;
    }
    return raiseTypeError(globalObject, scope, '\'', typeName(globalObject, self), "' object does not support the "_s, isAsync ? "asynchronous "_s : ""_s, "context manager protocol: it has no [Symbol."_s,
        isAsync ? "asyncDispose]() or [Symbol.dispose]()"_s : "dispose]()"_s, other ? " but it has [Symbol.asyncDispose](). Did you mean to use 'async with'?"_s : ""_s);
}

static JSValue dispose(JSGlobalObject* globalObject, JSValue self, bool isAsync)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = disposeMethod(globalObject, self, isAsync);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer noArguments;
    JSValue result = JSC::call(globalObject, method, JSC::getCallData(method), self, noArguments);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue jsObjectEnter(JSGlobalObject* globalObject, JSValue self)
{
    return disposeMethod(globalObject, self, false) ? self : JSValue();
}

// JS tells a disposable nothing of the exception that leaves the block, and it goes on.
static JSValue jsObjectExit(JSGlobalObject* globalObject, JSValue self, const CallArgs&)
{
    return dispose(globalObject, self, false) ? jsUndefined() : JSValue();
}

// An awaitable that has its result already. (A promise would ask the result whether it is a thenable.)
static JSValue jsReadyNext(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    return raiseStopIteration(globalObject, scope, uncheckedDowncast<PyRecord>(self.asCell())->get(0));
}

static constexpr NativeDef jsReadyMethods[] = {
    PY_SLOT("__await__", jsIteratorSelf, "($self, /)"),
    PY_SLOT("__iter__", jsIteratorSelf, "($self, /)"),
    PY_SLOT("__next__", jsReadyNext, "($self, /)"),
};
static constexpr TypeDef jsReadyTypeDef { "_js.ready"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, jsReadyMethods, { }, { } };

static JSValue jsObjectAsyncEnter(JSGlobalObject* globalObject, JSValue self)
{
    if (!disposeMethod(globalObject, self, true))
        return { };
    PyRecord* record = PyRecord::create(globalObject, runtime(globalObject).typeFor(jsReadyTypeDef), 1);
    record->set(getVM(globalObject), 0, self);
    return record;
}

static JSC_DECLARE_HOST_FUNCTION(jsGiveUndefined);
JSC_DEFINE_HOST_FUNCTION(jsGiveUndefined, (JSGlobalObject*, CallFrame*))
{
    return JSValue::encode(jsUndefined());
}

// A promise for None, whatever the method gives: what is true would swallow the exception.
static JSValue jsObjectAsyncExit(JSGlobalObject* globalObject, JSValue self, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = dispose(globalObject, self, true);
    RETURN_IF_EXCEPTION(scope, { });
    JSPromise* promise = JSPromise::resolvedPromise(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue then = promise->get(globalObject, vm.propertyNames->then);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    arguments.append(JSFunction::create(vm, globalObject, 0, String(), jsGiveUndefined, ImplementationVisibility::Private));
    RELEASE_AND_RETURN(scope, JSC::call(globalObject, then, JSC::getCallData(then), promise, arguments));
}

// The class of an object of JS cannot say whether the object can be disposed of, and what has an __exit__ in its class is a context
// manager to all who ask (contextlib.ExitStack.push(f) takes a function without one for a callback). So no class has these methods:
// it is the statement that finds them.
JSValue contextManagerMethodOfJS(JSGlobalObject* globalObject, JSValue object, SpecialMethodForWith which)
{
    static constexpr NativeDef methods[] = {
        PY_METHOD("__enter__", jsObjectEnter, "($self, /)"),
        PY_METHOD("__exit__", jsObjectExit, "($self, /, *args)"),
        PY_METHOD("__aenter__", jsObjectAsyncEnter, "($self, /)"),
        PY_METHOD("__aexit__", jsObjectAsyncExit, "($self, /, *args)"),
    };
    Runtime& rt = runtime(globalObject);
    if (!object.isObject() || isPyObject(object) || !rt.typeOf(object)->isSubtypeOf(rt.jsObjectType()))
        return { };
    if (!disposeMethod(globalObject, object, which == SpecialMethodForWith::AEnter || which == SpecialMethodForWith::AExit))
        return { };
    return PyNativeFunction::create(globalObject, methods[static_cast<unsigned>(which)], NativeKind::Function, object, rt.jsObjectType());
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

// String(object), of an object without a [Symbol.toPrimitive]: what toString() gives, or valueOf(). But Object.prototype.toString() is to JS what
// object.__str__() is to Python, for who has nothing of its own: where the instance has no other, str() is repr(), so that a __repr__() of
// the class serves for both. To find out takes the read, and String() reads once: so what was read is what is called.
static JSValue strOfInstanceOfDerivedClassByToString(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer noArguments;
    for (auto& name : { vm.propertyNames->toString, vm.propertyNames->valueOf }) {
        JSValue method = object->get(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        // (Of whichever realm.)
        if (auto* function = dynamicDowncast<JSFunction>(method); function && function->isHostFunction() && function->nativeFunction() == TaggedNativeFunction(objectProtoFuncToString))
            RELEASE_AND_RETURN(scope, repr(globalObject, object));
        auto callData = JSC::getCallData(method);
        if (callData.type == CallData::Type::None)
            continue;
        JSValue result = JSC::call(globalObject, method, callData, object, noArguments);
        RETURN_IF_EXCEPTION(scope, { });
        if (!result.isObject())
            RELEASE_AND_RETURN(scope, result.toString(globalObject));
    }
    throwTypeError(globalObject, scope, "No default value"_s);
    return { };
}

static JSValue jsObjectRepr(JSGlobalObject*, JSValue self);
template<bool isOfDerivedClass>
static JSValue jsObjectStrImpl(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, ensureMainThreadForJS(globalObject)))
        return { };
    // An object without a prototype, or the namespace of a module, has nothing by which JS could make a string of it. Nor has one that inherits
    // it from where the objects of Python's do, with nothing of Python's in it (Object.create(Class.prototype)): that is str(this).
    bool canConvert = false;
    bool hasToPrimitive = false;
    for (auto& key : { vm.propertyNames->toPrimitiveSymbol, vm.propertyNames->toString, vm.propertyNames->valueOf }) {
        PropertySlot slot(self, PropertySlot::InternalMethodType::HasProperty);
        bool found = asObject(self)->getPropertySlot(globalObject, key, slot);
        RETURN_IF_EXCEPTION(scope, { });
        if (!found)
            continue;
        canConvert = slot.slotBase() != runtime(globalObject).jsViewObject(JSViewObject::ObjectPrototype) || objectOfPythonWithin(self) != self;
        hasToPrimitive = key == vm.propertyNames->toPrimitiveSymbol;
        break;
    }
    if (!canConvert)
        RELEASE_AND_RETURN(scope, repr(globalObject, self));
    if (isOfDerivedClass && !hasToPrimitive)
        RELEASE_AND_RETURN(scope, strOfInstanceOfDerivedClassByToString(globalObject, asObject(self)));
    RELEASE_AND_RETURN(scope, self.toString(globalObject));
}

static JSValue jsObjectStr(JSGlobalObject* globalObject, JSValue self)
{
    return jsObjectStrImpl<false>(globalObject, self);
}

static JSValue jsObjectRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // An instance of a class written in Python.
    if (typeOf(globalObject, self)->isHeapType())
        RELEASE_AND_RETURN(scope, objectRepr(globalObject, self));
    // Another thread than the main one does without the name, which takes asking.
    String className = runtime(globalObject).threads().isMainThread() ? JSObject::calculatedClassName(asObject(self)) : String();
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
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue name;
    if (runtime(globalObject).threads().isMainThread()) [[likely]] {
        name = asObject(self)->get(globalObject, vm.propertyNames->name);
        RETURN_IF_EXCEPTION(scope, { });
    } else if (inquireProperty(globalObject, asObject(self), vm.propertyNames->name, name) != Inquiry::Value)
        name = jsUndefined();
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
    case DerivedArrayType:
        return !PyListObject::is(callee.asCell());
    default:
        return !isPyObjectType(callee.asCell()->type());
    }
}

JSFunction* beginCallWithSelf(JSGlobalObject* globalObject, JSValue callee)
{
    if (!callee.isCell())
        return nullptr;
    PyFunction* function;
    JSValue self;
    switch (callee.asCell()->type()) {
    case PyMethodType:
        function = PyFunction::tryFrom(uncheckedDowncast<PyMethod>(callee.asCell())->function());
        self = uncheckedDowncast<PyMethod>(callee.asCell())->self();
        break;
    case PyInstanceType:
        // (To look may run code of the program. Whatever that does, this is what was found.)
        function = PyFunction::tryFrom(uncheckedDowncast<PyObject>(callee.asCell())->type()->lookup(Special::Call));
        self = callee;
        break;
    default:
        return nullptr;
    }
    if (function)
        runtime(globalObject).selfOfCall = JSValue::encode(self);
    return function;
}

CallData callDataUnknownToJS(JSValue callee)
{
    CallData callData;
    if (callee.isCell() && PyListObject::is(callee.asCell()) && uncheckedDowncast<PyListObject>(callee.asCell())->type()->lookup(Special::Call)) {
        callData.type = CallData::Type::Native;
        callData.native.function = callPythonObject;
        callData.native.isBoundFunction = false;
        callData.native.isWasm = false;
    }
    return callData;
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

// To call a class is to make an instance, in Python. The classes of JS are what `class` declares, the functions that were written
// for `new` before there was `class`, and the constructors that the engine and the host implement: Map, Date, Response. Those have a
// prototype for their instances; what can merely be constructed as well as called, as a mock function of a test runner can, is called.
// So are the constructors of primitive values, which make one when they are called, and what the host says is meant to be called.
// (For `new` whatever this says: f.new().)
static bool hasPrototypeForInstances(JSObject* function)
{
    JSValue prototype = function->getDirect(function->vm(), function->vm().propertyNames->prototype);
    return (prototype && prototype.isObject()) || function->inherits<ProxyConstructor>();
}

static bool (*s_classIsCalled)(JSObject*);

void setClassIsCalledFunction(bool (*function)(JSObject*))
{
    s_classIsCalled = function;
}

static bool isClassOfHost(JSObject* function, TaggedNativeFunction constructor)
{
    return constructor != callHostFunctionAsConstructor && hasPrototypeForInstances(function) && !(s_classIsCalled && s_classIsCalled(function));
}

// Whether somebody has given the instances of the function something to inherit: methods in function.prototype, or a prototype for that
// (util.inherits()). Nobody does that to a function that is only ever called. Runs no code.
static bool hasSomethingForInstancesToInherit(JSFunction* function)
{
    VM& vm = function->vm();
    // It is made when it is first asked for.
    JSValue value = function->getDirect(vm, vm.propertyNames->prototype);
    if (!value || !value.isObject())
        return false;
    JSObject* prototype = asObject(value);
    if (prototype->getPrototypeDirect() != JSValue(function->realm()->objectPrototype()))
        return true;
    bool result = false;
    prototype->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        result = entry.key() != vm.propertyNames->constructor.impl();
        return !result;
    });
    return result;
}

static bool isClassWrittenInJS(JSFunction* function)
{
    FunctionExecutable* executable = function->jsExecutable();
    return executable->isClassConstructorFunction() || (executable->constructAbility() == ConstructAbility::CanConstruct && hasSomethingForInstancesToInherit(function));
}

bool isClassOfJS(JSCell* callee)
{
    while (auto* bound = dynamicDowncast<JSBoundFunction>(callee))
        callee = bound->targetFunction();
    if (callee->inherits<StringConstructor>() || callee->inherits<NumberConstructor>() || callee->inherits<BooleanConstructor>()
        || callee->inherits<SymbolConstructor>() || callee->inherits<BigIntConstructor>())
        return false;
    if (auto* function = dynamicDowncast<JSFunction>(callee))
        return function->isHostFunction() ? isClassOfHost(function, function->nativeConstructor()) : isClassWrittenInJS(function);
    auto* function = dynamicDowncast<InternalFunction>(callee);
    return function && isClassOfHost(function, function->nativeFunctionFor(CodeSpecializationKind::CodeForConstruct));
}

bool prepareForeignCall(JSGlobalObject* globalObject, CallFrame* calleeFrame)
{
    // A list is an array of JS. The caller says that it cannot be called, on whichever thread.
    if (!asObject(calleeFrame->guaranteedJSValueCallee())->isCallable())
        return false;
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
    if (isClassWrittenInJS(function))
        return ForeignMethodKind::NotAMethod;
    return isArrowFunctionParseMode(function->jsExecutable()->parseMode()) ? ForeignMethodKind::TakesFirstArgument : ForeignMethodKind::TakesThis;
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

// isinstance(value, function): `value instanceof function`. Of an exception that stands for what JS threw, it is that which is asked about.
static JSValue jsFunctionInstanceCheck(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = asObject(self)->hasInstance(globalObject, valueForJS(thrownValueFor(globalObject, value)));
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
    PY_SLOT("__setattr__", jsObjectSetOrDelAttr<false>, "($self, name, value, /)"),
    PY_SLOT("__delattr__", jsObjectDelAttr<false>, "($self, name, /)"),
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
    PY_METHOD("get", jsObjectGet, "($self, key, default=None)"),
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
        isPlainName = isPlainName && !isKeywordOfPython(name);
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
        RELEASE_AND_RETURN(scope, genericGetAttrOfType(globalObject, self, name));
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
    if (JSValue found = type->lookup(name))
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, found, object, type));
    for (auto [special, which] : { std::pair { Special::Enter, SpecialMethodForWith::Enter }, std::pair { Special::Exit, SpecialMethodForWith::Exit }, std::pair { Special::AEnter, SpecialMethodForWith::AEnter }, std::pair { Special::AExit, SpecialMethodForWith::AExit } }) {
        if (!isIdentical(name, rt.name(special)))
            continue;
        JSValue method = contextManagerMethodOfJS(globalObject, object, which);
        RETURN_IF_EXCEPTION(scope, { });
        if (method)
            return method;
    }
    return jsUndefined();
}

// _js.JSOSError, _js.JSFileNotFoundError and so on are made when they are first asked for.
static JSValue jsModuleGetAttr(JSGlobalObject* globalObject, JSValue nameValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    String text = stringOf(name);
    if (text.startsWith("JS"_s)) {
        PyType* base = PyType::tryFrom(rt.builtins()->get(internString(vm, text.substring(2))));
        if (base && !base->isHeapType() && base->isSubtypeOf(rt.OSError()))
            return jsErrorTypeFor(globalObject, base);
    }
    return raiseAttributeError(globalObject, scope, jsUndefined(), name, makeString("module '_js' has no attribute '"_s, text, '\''));
}

static constexpr NativeDef jsModuleFunctions[] = {
    PY_FUNCTION("__getattr__", jsModuleGetAttr, "(name, /)"),
    PY_FUNCTION("exception_for", jsExceptionFor, "(thrown, /)"),
    PY_FUNCTION("special_method", jsSpecialMethod, "(object, name, /)"),
};

PyType* jsTypedArrayType(Runtime&); // PyJSBuffers.cpp

// `import js` gives the global object of JS, whose properties are its attributes. The import system writes what it knows of a
// module into the module (__spec__, __loader__), and hands out what is in sys.modules once the module has run. So there is a module for
// it to write into, which puts the global object in its own place. Not before a program asks for it: a program that has nothing to do
// with JS finds nothing of JS's when it goes through sys.modules.
void initializeGlobalObjectModule(JSGlobalObject*, PyModule*) { }

void executeGlobalObjectModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    rt.modules()->set(rt.vm(), asString(module->dict()->get(rt.names.dName())), globalObject->globalThis());
}

// The module _js: what Python code needs to name when it deals with JS.
void initializeJSModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    PyType* propertiesType = jsPropertiesType(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    module->add(globalObject, "globalThis"_s, globalObject->globalThis());
    module->add(globalObject, "JSError"_s, rt.JSError());
    module->add(globalObject, "JSTypeError"_s, jsTypeErrorType(globalObject));
    module->add(globalObject, "JSObject"_s, rt.jsObjectType());
    module->add(globalObject, "JSPlainObject"_s, rt.jsPlainObjectType());
    module->add(globalObject, "JSFunction"_s, rt.jsFunctionType());
    module->add(globalObject, "JSPromise"_s, rt.jsPromiseType());
    module->add(globalObject, "JSIterator"_s, jsIteratorObjectType(rt));
    module->add(globalObject, "JSGenerator"_s, jsGeneratorType(rt));
    module->add(globalObject, "JSTypedArray"_s, jsTypedArrayType(rt));
    module->add(globalObject, "JSMap"_s, rt.jsMapType());
    module->add(globalObject, "JSProperties"_s, propertiesType);
    module->add(globalObject, "JSMethod"_s, jsMethodType(rt));
    module->addFunctions(globalObject, jsModuleFunctions);
}

// collections.abc.MutableMapping.register(JSPlainObject), and JSMap. The module of a type does that for it (`array` does), but these
// types have instances before anything is imported that knows of them.
void registerInteropTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    registerAsMutableMapping(globalObject, { rt.jsPlainObjectType(), rt.jsMapType() });
}

void initializeInteropTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.jsObjectType()->initializeBuiltin(globalObject, { "JSObject"_s, { TypeFlag::DisallowInstantiation, TypeFlag::HasWeakref, TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::AttributesAreProperties }, jsObjectMethods, { }, { } }, rt.typeType(), rt.objectType());
    rt.jsPlainObjectType()->initializeBuiltin(globalObject, { "JSPlainObject"_s, { TypeFlag::DisallowInstantiation, TypeFlag::HasWeakref, TypeFlag::Mapping }, { }, { }, { } }, rt.typeType(), rt.jsObjectType());
    rt.jsFunctionType()->initializeBuiltin(globalObject, { "JSFunction"_s, { TypeFlag::DisallowInstantiation, TypeFlag::HasWeakref }, jsFunctionMethods, jsFunctionGetSets, { } }, rt.typeType(), rt.jsObjectType());
    rt.jsPromiseType()->initializeBuiltin(globalObject, { "JSPromise"_s, { TypeFlag::DisallowInstantiation, TypeFlag::HasWeakref, TypeFlag::Base, TypeFlag::OwnLayout }, jsPromiseMethods, { }, { } }, rt.typeType(), rt.jsObjectType());
    // A Map has a type of its own, as a plain object has, which adds nothing to JSObject but what it says to others: that it is a mapping.
    rt.jsMapType()->initializeBuiltin(globalObject, { "_js.JSMap"_s, { TypeFlag::DisallowInstantiation, TypeFlag::HasWeakref, TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Mapping }, { }, { }, { } }, rt.typeType(), rt.jsObjectType());
}

} // namespace Python
} // namespace JSC
