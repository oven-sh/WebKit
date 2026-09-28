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

#include "IteratorOperations.h"
#include "JSONObject.h"
#include "ObjectConstructor.h"
#include "ObjectPrototypeInlines.h"
#include "PyDict.h"

// What each language sees of what is the other's. "The two languages" in README.md says why it is as it is.

namespace JSC { namespace Python {

// ---- What Python sees of a JavaScript object: the methods of the class JSObject
//
// Its attributes are its properties, which the object model sees to. Its items are its properties too.

PYTHON_NATIVE(objectGetItem)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__getitem__"_s, 2, 2))
        return { };
    auto property = args[1].toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    PropertySlot slot(args[0], PropertySlot::InternalMethodType::Get);
    bool found = asObject(args[0])->getPropertySlot(globalObject, property, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, args[1]));
    RELEASE_AND_RETURN(scope, JSValue::encode(slot.getValue(globalObject, property)));
}

PYTHON_NATIVE(objectSetItem)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__setitem__"_s, 3, 3))
        return { };
    auto property = args[1].toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    PutPropertySlot slot(args[0], true);
    asObject(args[0])->methodTable()->put(asObject(args[0]), globalObject, property, args[2], slot);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(objectDelItem)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__delitem__"_s, 2, 2))
        return { };
    auto property = args[1].toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    bool found = asObject(args[0])->hasProperty(globalObject, property);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, args[1]));
    JSCell::deleteProperty(asObject(args[0]), globalObject, property);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// len(): what JavaScript calls length, or size.
PYTHON_NATIVE(objectLen)
{
    NATIVE_PROLOGUE();
    for (const Identifier& name : { vm.propertyNames->length, vm.propertyNames->size }) {
        JSValue value = asObject(args[0])->get(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (value.isNumber() && value.asNumber() >= 0 && value.asNumber() == std::trunc(value.asNumber()))
            return JSValue::encode(intFromDouble(globalObject, value.asNumber()));
    }
    return JSValue::encode(raiseTypeError(globalObject, scope, makeString("object of type '"_s, typeName(globalObject, args[0]), "' has no len()"_s)));
}

// An object is true to JavaScript, whatever is in it.
PYTHON_NATIVE(objectBool)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsBoolean(true));
}

// key in object: object.has(key) where there is such a thing, as a Map and a Set and much else have. Otherwise, whether it is a property.
PYTHON_NATIVE(objectContains)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__contains__"_s, 2, 2))
        return { };
    JSObject* object = asObject(args[0]);
    JSValue has = object->get(globalObject, vm.propertyNames->has);
    RETURN_IF_EXCEPTION(scope, { });
    if (has.isCallable()) {
        MarkedArgumentBuffer arguments;
        arguments.append(args[1]);
        JSValue result = JSC::call(globalObject, has, object, arguments, "has is not a function"_s);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(jsBoolean(result.toBoolean(globalObject)));
    }
    auto property = args[1].toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(object->hasProperty(globalObject, property))));
}

// What JavaScript can iterate, Python can.
PYTHON_NATIVE(objectIter)
{
    NATIVE_PROLOGUE();
    JSValue function = asObject(args[0])->get(globalObject, vm.propertyNames->iteratorSymbol);
    RETURN_IF_EXCEPTION(scope, { });
    if (function.isCallable()) {
        JSValue iterator = JSC::call(globalObject, function, args[0], ArgList(), "Symbol.iterator is not a function"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (iterator.isObject()) {
            JSValue next = asObject(iterator)->get(globalObject, vm.propertyNames->next);
            RETURN_IF_EXCEPTION(scope, { });
            return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::JavaScript, iterator, next));
        }
    }
    return JSValue::encode(raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, args[0]), "' object is not iterable"_s)));
}

// isinstance(value, constructor): value instanceof constructor
PYTHON_NATIVE(objectInstanceCheck)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__instancecheck__"_s, 2, 2))
        return { };
    if (!args[0].isCallable())
        return JSValue::encode(raiseTypeError(globalObject, scope, "isinstance() arg 2 must be a type, a tuple of types, or a union"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(asObject(args[0])->hasInstance(globalObject, args[1]))));
}

// Constructor.new(...): new Constructor(...). Python has no word for it, and JavaScript tells calling from constructing.
PYTHON_NATIVE(objectNew)
{
    NATIVE_PROLOGUE();
    auto constructData = JSC::getConstructData(args[0]);
    if (constructData.type == CallData::Type::None)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, args[0]), "' object is not a constructor"_s)));
    MarkedArgumentBuffer arguments;
    for (unsigned i = 1; i < args.size(); ++i)
        arguments.append(args[i]);
    if (args.keywordCount()) {
        // As for a call: an object, after the rest.
        JSObject* options = constructEmptyObject(globalObject);
        for (unsigned i = 0; i < args.keywordCount(); ++i) {
            auto name = args.keywordName(i)->toIdentifier(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            options->putDirect(vm, name, args.keywordValue(i));
        }
        arguments.append(options);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(JSC::construct(globalObject, args[0], constructData, arguments)));
}

PYTHON_NATIVE(objectStr)
{
    NATIVE_PROLOGUE();
    String text = args[0].toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

PYTHON_NATIVE(objectRepr)
{
    NATIVE_PROLOGUE();
    JSObject* object = asObject(args[0]);
    if (object->isCallable()) {
        JSValue name = object->get(globalObject, vm.propertyNames->name);
        RETURN_IF_EXCEPTION(scope, { });
        String text = name.isString() ? String(asString(name)->value(globalObject)) : String();
        return JSValue::encode(jsString(vm, makeString("<JSFunction "_s, text.isEmpty() ? "(anonymous)"_str : text, '>')));
    }
    // A plain object, as it would be written.
    if (object->type() == FinalObjectType) {
        // It cannot be if it goes round in a circle, or has a BigInt in it. That is a TypeError.
        String text = JSONStringify(globalObject, object, 0u);
        if (scope.exception()) [[unlikely]] {
            if (!catchException(globalObject, BuiltinType::TypeError))
                return { };
        } else if (!text.isNull())
            return JSValue::encode(jsString(vm, text));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(objectPrototypeToString(globalObject, object)));
}

// Every name that is a property of it, or of what it inherits from.
PYTHON_NATIVE(objectDir)
{
    NATIVE_PROLOGUE();
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    UncheckedKeyHashSet<UniquedStringImpl*> seen;
    for (JSValue cursor = args[0]; cursor.isObject();) {
        PropertyNameArrayBuilder properties(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        asObject(cursor)->methodTable()->getOwnPropertyNames(asObject(cursor), globalObject, properties, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, { });
        for (auto& name : properties) {
            if (seen.add(name.impl()).isNewEntry)
                listAppend(globalObject, result, jsString(vm, name.string()));
        }
        cursor = asObject(cursor)->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

static JSValue getFunctionName(JSGlobalObject* globalObject, JSValue self)
{
    return asObject(self)->get(globalObject, globalObject->vm().propertyNames->name);
}

void initializeJavaScriptTypes(JSGlobalObject* globalObject)
{
    PyRealm* realm = globalObject->pyRealm();
    addMethods(globalObject, realm->typeJSObject(), {
        { "__getitem__"_s, objectGetItem },
        { "__setitem__"_s, objectSetItem },
        { "__delitem__"_s, objectDelItem },
        { "__len__"_s, objectLen },
        { "__bool__"_s, objectBool },
        { "__contains__"_s, objectContains },
        { "__iter__"_s, objectIter },
        { "__instancecheck__"_s, objectInstanceCheck },
        { "__str__"_s, objectStr },
        { "__repr__"_s, objectRepr },
        { "__dir__"_s, objectDir },
        { "new"_s, objectNew },
    });
    addGetSet(globalObject, realm->typeJSFunction(), "__name__"_s, getFunctionName);
}

// ---- What JavaScript sees of what is Python's

// obj.toString(): str(obj)
static JSC_DECLARE_HOST_FUNCTION(javaScriptToString);
JSC_DEFINE_HOST_FUNCTION(javaScriptToString, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = str(globalObject, callFrame->thisValue());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

// obj[Symbol.iterator](): iter(obj)
static JSC_DECLARE_HOST_FUNCTION(javaScriptIterator);
JSC_DEFINE_HOST_FUNCTION(javaScriptIterator, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSValue::encode(getIterator(globalObject, callFrame->thisValue()));
}

// iterator.next(): next(iterator), as { value, done }
static JSC_DECLARE_HOST_FUNCTION(javaScriptNext);
JSC_DEFINE_HOST_FUNCTION(javaScriptNext, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = iteratorNext(globalObject, callFrame->thisValue());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(createIteratorResultObject(globalObject, value ? value : jsUndefined(), !value));
}

// obj.toJSON(), which JSON.stringify() asks for: a dict as an object, and anything else that can be gone through as an array.
static JSC_DECLARE_HOST_FUNCTION(javaScriptToJSON);
JSC_DEFINE_HOST_FUNCTION(javaScriptToJSON, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue self = callFrame->thisValue();
    if (isDict(self)) {
        JSObject* result = constructEmptyObject(globalObject);
        uncheckedDowncast<PyDict>(self.asCell())->forEach(globalObject, [&] (JSValue key, JSValue value) {
            // As json.dumps() has it: a key is a string, or a number, True, False or None, which are written as they are in JSON.
            String name;
            if (key.isString())
                name = asString(key)->value(globalObject);
            else if (key.isBoolean())
                name = key.isTrue() ? "true"_s : "false"_s;
            else if (isNone(key))
                name = "null"_s;
            else if (classify(key))
                name = repr(globalObject, key);
            else {
                raiseTypeError(globalObject, scope, makeString("keys must be str, int, float, bool or None, not "_s, typeName(globalObject, key)));
                return false;
            }
            result->putDirectMayBeIndex(globalObject, Identifier::fromString(vm, name), value);
            return true;
        });
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(result);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(listFromIterable(globalObject, self)));
}

JSObject* createJavaScriptFunctions(VM& vm, JSGlobalObject* globalObject)
{
    JSObject* object = constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
    auto add = [&] (ASCIILiteral name, NativeFunction function) {
        object->putDirect(vm, Identifier::fromString(vm, name), JSFunction::create(vm, globalObject, 0, String(name), function, ImplementationVisibility::Public));
    };
    add("toString"_s, javaScriptToString);
    add("iterator"_s, javaScriptIterator);
    add("next"_s, javaScriptNext);
    add("toJSON"_s, javaScriptToJSON);
    return object;
}

JSValue getPropertyForJavaScript(JSGlobalObject* globalObject, JSValue receiver, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    PyType* type = typeOf(globalObject, receiver);
    auto function = [&] (ASCIILiteral which) { return realm->javaScriptFunctions()->getDirect(vm, Identifier::fromString(vm, which)); };

    if (name.isSymbol()) {
        if (name == vm.propertyNames->iteratorSymbol && !isType(receiver) && (type->lookup(vm, names.dunder_iter) || type->lookup(vm, names.dunder_getitem)))
            return function("iterator"_s);
        return { };
    }

    JSValue value = getAttributeIfPresent(globalObject, receiver, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    if (isType(receiver))
        return { };

    // Names that Python has no use for, and that JavaScript expects. An exception is left to Error.prototype, which makes "name: message".
    if (name == vm.propertyNames->toString && !type->isExceptionType())
        return function("toString"_s);
    bool isMapping = type->hasFlag(PyType::IsMapping);
    bool isSet = type->isSubtypeOf(realm->typeSet()) || type->isSubtypeOf(realm->typeFrozenSet());
    if (name == vm.propertyNames->toJSON && (isMapping || isSet || type->hasFlag(PyType::IsSequence)))
        return function("toJSON"_s);
    // How many: an array has a length, and a Map and a Set have a size.
    if (name == ((isMapping || isSet) ? vm.propertyNames->size : vm.propertyNames->length) && type->lookup(vm, names.dunder_len)) {
        int64_t count = length(globalObject, receiver);
        RETURN_IF_EXCEPTION(scope, { });
        return intFromInt64(globalObject, count);
    }
    if (name == vm.propertyNames->next && type->lookup(vm, names.dunder_next))
        return function("next"_s);
    if (type->isExceptionType()) {
        if (name == vm.propertyNames->message) {
            String text = str(globalObject, receiver);
            RETURN_IF_EXCEPTION(scope, { });
            return jsString(vm, text);
        }
        if (name == vm.propertyNames->name)
            return type->name();
        if (name == vm.propertyNames->stack)
            return jsString(vm, formatException(globalObject, receiver));
    }
    return { };
}

} } // namespace JSC::Python
