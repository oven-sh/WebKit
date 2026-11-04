/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyHooks.h"
#include "PyNative.h"
#include "PyRuntime.h"

namespace JSC {

class PropertyNameArrayBuilder;

namespace Python {

// What Python objects are to JS.
//
// Numbers, strings, booleans, None, lists and functions are values of both languages as they are. Every other Python object is
// an exotic object to JS, as a Proxy is: the classes of cells forward the operations on properties to the functions below.
//
//   object.name          getattr(object, name); undefined (after the prototype) if there is no such attribute
//   mapping.name         mapping["name"] if it has that key, else the attribute
//   sequence[0]          sequence[0]; undefined if that is an IndexError
//   object.name = v      setattr, or mapping["name"] = v; `delete` likewise
//   Object.keys(object)  the keys of a mapping that are str or int; the indices of a sequence; the names in __dict__ and
//                        __slots__ of anything else (for a module or a class, without the __dunder__ ones)
//   object(...)          object(...); the last argument may be kwargs({ name: value }), for name=value
//   new Class(...)       Class(...)
//   x instanceof Class   isinstance(x, Class)
//
// What JS expects of an object that can do a thing is there when, and only when, the Python object can do it and has no
// attribute of that name itself: [Symbol.iterator] (__iter__, or __getitem__ of what is not a mapping; a dict gives [key, value]
// pairs, as a Map does), next() and return() (__next__),
// [Symbol.asyncIterator] (__aiter__), then() (__await__), length (__len__), toJSON() (numbers, str, isoformat() of a date, time or
// datetime, and what has a length and can be iterated over but is no mapping: an array), constructor (the type), and message and
// cause of an exception. (Its name and stack are always those of JS: the name of the type, and the traceback.)
// The prototype all Python objects share has toString() (str), valueOf(), [Symbol.toPrimitive] and, for console.log(),
// [Symbol.for("nodejs.util.inspect.custom")] (repr); that of exceptions inherits from Error.prototype.
//
// None of it can be cached: the structures say so (ProhibitsPropertyCaching).

namespace JSView {

// `object` is a PyObject, a PyType, a PyFunction or a PyNativeFunction. These use the global object the object belongs to.
// False: there is no such property, or an exception.
bool getOwnPropertySlot(JSObject*, PropertyName, PropertySlot&);
bool getOwnPropertySlotByIndex(JSObject*, unsigned index, PropertySlot&);
bool put(JSObject*, PropertyName, JSValue, bool shouldThrow);
bool putByIndex(JSObject*, unsigned index, JSValue, bool shouldThrow);
bool deleteProperty(JSObject*, PropertyName);
bool deletePropertyByIndex(JSObject*, unsigned index);
void getOwnPropertyNames(JSObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
bool defineOwnProperty(JSObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);

// An instance of a class derived from list is an array, with all that Array.prototype has. What it has besides are the
// attributes in its __dict__ and those of its classes other than list and object (whose sort() and pop() are not those of JS).
bool getOwnPropertySlotOfListSubclass(JSObject*, PropertyName, PropertySlot&);
void getOwnPropertyNamesOfListSubclass(JSObject*, PropertyNameArrayBuilder&);

} // namespace JSView

// The entries of the method table of a class of cells that are Python objects. Properties whose keys are symbols, which Python
// knows nothing of, are kept as those of any object of JS are; so is what the base class has when Python has nothing by the name.
#define PY_DEFINE_JS_VIEW(Class) \
    bool Class::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot) \
    { \
        VM& vm = getVM(globalObject); \
        auto scope = DECLARE_THROW_SCOPE(vm); \
        if (!propertyName.isPrivateName()) { \
            bool isFound = JSView::getOwnPropertySlot(object, propertyName, slot); \
            RETURN_IF_EXCEPTION(scope, false); \
            if (isFound) \
                return true; \
        } \
        RELEASE_AND_RETURN(scope, Base::getOwnPropertySlot(object, globalObject, propertyName, slot)); \
    } \
    bool Class::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject*, unsigned index, PropertySlot& slot) \
    { \
        return JSView::getOwnPropertySlotByIndex(object, index, slot); \
    } \
    bool Class::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot) \
    { \
        if (propertyName.isSymbol()) \
            return Base::put(cell, globalObject, propertyName, value, slot); \
        if (slot.thisValue() != JSValue(cell)) [[unlikely]] \
            return JSObject::definePropertyOnReceiver(globalObject, propertyName, value, slot); \
        return JSView::put(asObject(cell), propertyName, value, slot.isStrictMode()); \
    } \
    bool Class::putByIndex(JSCell* cell, JSGlobalObject*, unsigned index, JSValue value, bool shouldThrow) \
    { \
        return JSView::putByIndex(asObject(cell), index, value, shouldThrow); \
    } \
    bool Class::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot) \
    { \
        if (propertyName.isSymbol()) \
            return Base::deleteProperty(cell, globalObject, propertyName, slot); \
        return JSView::deleteProperty(asObject(cell), propertyName); \
    } \
    bool Class::deletePropertyByIndex(JSCell* cell, JSGlobalObject*, unsigned index) \
    { \
        return JSView::deletePropertyByIndex(asObject(cell), index); \
    } \
    void Class::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& builder, DontEnumPropertiesMode mode) \
    { \
        VM& vm = getVM(globalObject); \
        auto scope = DECLARE_THROW_SCOPE(vm); \
        JSView::getOwnPropertyNames(object, builder, mode); \
        RETURN_IF_EXCEPTION(scope, void()); \
        RELEASE_AND_RETURN(scope, Base::getOwnPropertyNames(object, globalObject, builder, mode)); \
    } \
    bool Class::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool shouldThrow) \
    { \
        if (propertyName.isSymbol()) \
            return Base::defineOwnProperty(object, globalObject, propertyName, descriptor, shouldThrow); \
        return JSView::defineOwnProperty(object, propertyName, descriptor, shouldThrow); \
    }

// Makes the objects of JS that the view is made of (Runtime::jsViewObject()). They come before everything else of a Runtime:
// the structures need the prototypes.
void initializeJSView(JSGlobalObject*);

// ---- Calls from JS ----

// Keyword arguments are an object to JS. f(1, a=2) in Python calls a function of JS with (1, { a: 2 }), and f(1, kwargs({ a: 2 })) in JS
// is f(1, a=2). Both are plain objects with a mark that JS does not see. As the last argument of a call that JS makes of something
// of Python's, an object with the mark is keyword arguments: so a function of JS can pass on what it was given, super(...args).
bool hasKeywordMark(JSGlobalObject*, JSObject*);
JSObject* makeKeywordMarker(JSGlobalObject*); // Empty.
ALWAYS_INLINE bool isKeywordMarker(JSGlobalObject* globalObject, JSValue value)
{
    return value && value.isCell() && value.asCell()->type() == FinalObjectType && hasKeywordMark(globalObject, asObject(value));
}
// As isCalledByPython(), for a callee that is just starting, and exact. `return f(...)` in strict code of JS is a tail call: f then
// returns to whoever called that code, which may be Python code that never heard of f.
bool isBeingCalledByPython(JSGlobalObject*, CallFrame*);
// `callFrame` is that of something of Python's, which is just starting.
ALWAYS_INLINE bool hasKeywordMarker(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    size_t count = callFrame->argumentCount();
    return count && isKeywordMarker(globalObject, callFrame->uncheckedArgument(count - 1)) && !isBeingCalledByPython(globalObject, callFrame);
}
// The names in it (a tuple as calls pass them; null if there are none), whose values are added to the buffer. False: an exception.
bool keywordsOfMarker(JSGlobalObject*, JSValue marker, PyTuple*& names, MarkedArgumentBuffer& values);
// For a host function that finds one: calls the callee of the frame with the arguments spelled out.
JSValue callWithKeywordMarker(JSGlobalObject*, CallFrame*);
// function(CallArgs) with the arguments of a frame that JS made, whose `this` is not about them.
template<typename Function>
JSValue withArgumentsFromJS(JSGlobalObject* globalObject, CallFrame* callFrame, const Function& function)
{
    unsigned count = static_cast<unsigned>(callFrame->argumentCount());
    const JSValue* values = reinterpret_cast<const JSValue*>(callFrame->addressOfArgumentsStart());
    if (!count || !isKeywordMarker(globalObject, values[count - 1])) [[likely]]
        return function(CallArgs(values, count));
    MarkedArgumentBuffer buffer;
    for (unsigned i = 0; i + 1 < count; ++i)
        buffer.append(values[i]);
    PyTuple* names = nullptr;
    if (!keywordsOfMarker(globalObject, values[count - 1], names, buffer))
        return { };
    return function(CallArgs(reinterpret_cast<const JSValue*>(buffer.data()), count - 1, names));
}

// What a host function that is Python to its callers returns: JS code gets valueForJS(result).
// resultForCaller() is in PyHooks.h.

} // namespace Python
} // namespace JSC
