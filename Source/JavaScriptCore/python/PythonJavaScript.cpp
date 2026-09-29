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

#include "FunctionConstructor.h"
#include "FunctionPrototype.h"
#include "IteratorOperations.h"
#include "JSBoundFunction.h"
#include "JSONObject.h"
#include "JSPromise.h"
#include "JSPromiseConstructor.h"
#include "JSPromisePrototype.h"
#include "ObjectConstructor.h"
#include "ObjectPrototypeInlines.h"
#include "PyDict.h"
#include "PythonGenerators.h"
#include "TopExceptionScope.h"

// What each language sees of what is the other's. "The two languages" in README.md says why it is as it is.

namespace JSC { namespace Python {

// ---- What Python sees of a JavaScript object: the methods of the class JSObject
//
// Its attributes are its properties, which the object model sees to. Its items are its properties too.

PYTHON_NATIVE(objectGetItem)
{
    NATIVE_PROLOGUE();
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
    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("object of type '"_s, typeName(globalObject, args[0]), "' has no len()"_s)));
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
    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate('\'', typeName(globalObject, args[0]), "' object is not iterable"_s)));
}

// isinstance(value, constructor): value instanceof constructor
PYTHON_NATIVE(objectInstanceCheck)
{
    NATIVE_PROLOGUE();
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
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate('\'', typeName(globalObject, args[0]), "' object is not a constructor"_s)));
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
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<JSFunction "_s, text.isEmpty() ? "(anonymous)"_str : text, '>'))));
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

// A function that has been bound to an object and nothing else is to Python what a bound method is.
static JSBoundFunction* tryMethodOfObject(JSValue value)
{
    auto* function = dynamicDowncast<JSBoundFunction>(value);
    return function && !function->boundArgsLength() ? function : nullptr;
}

PYTHON_NATIVE(functionEq)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* self = tryMethodOfObject(args[0]);
    auto* other = tryMethodOfObject(args.at(1));
    if (!self || !other)
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(jsBoolean(self->targetFunction() == other->targetFunction() && isIdentical(self->boundThis(), other->boundThis())));
}

PYTHON_NATIVE(functionHash)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* self = tryMethodOfObject(args[0]);
    if (!self || !self->boundThis().isCell())
        return JSValue::encode(intFromInt64(globalObject, hashOfPointer(args[0].asCell())));
    int64_t result = hashOfPointer(self->boundThis().asCell()) ^ hashOfPointer(self->targetFunction());
    return JSValue::encode(intFromInt64(globalObject, result == -1 ? -2 : result));
}

static JSValue getFunctionSelf(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* method = tryMethodOfObject(self))
        return method->boundThis();
    return raise(globalObject, scope, BuiltinType::AttributeError, "'JSFunction' object has no attribute '__self__'"_s);
}

static JSValue getFunctionFunc(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* method = tryMethodOfObject(self))
        return method->targetFunction();
    return raise(globalObject, scope, BuiltinType::AttributeError, "'JSFunction' object has no attribute '__func__'"_s);
}

void initializeJavaScriptTypes(JSGlobalObject* globalObject)
{
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    addMethods(globalObject, realm->typeJSObject(), {
        { "__getitem__"_s, objectGetItem, Kind::Wrapper, 0, "($self, key, /)"_s },
        { "__setitem__"_s, objectSetItem, Kind::Wrapper, 0, "($self, key, value, /)"_s },
        { "__delitem__"_s, objectDelItem, Kind::Wrapper, 0, "($self, key, /)"_s },
        { "__len__"_s, objectLen, Kind::Wrapper, 0, "($self, /)"_s },
        { "__bool__"_s, objectBool, Kind::Wrapper, 0, "($self, /)"_s },
        { "__contains__"_s, objectContains, Kind::Wrapper, 0, "($self, key, /)"_s },
        { "__iter__"_s, objectIter, Kind::Wrapper, 0, "($self, /)"_s },
        { "__instancecheck__"_s, objectInstanceCheck, Kind::Method, 0, "($self, instance, /)"_s },
        { "__str__"_s, objectStr },
        { "__repr__"_s, objectRepr },
        { "__dir__"_s, objectDir },
    });
    addMethods(globalObject, realm->typeJSFunction(), {
        { "new"_s, objectNew, Kind::Method, 0, "($self, /, *args)"_s },
        { "__eq__"_s, functionEq },
        { "__hash__"_s, functionHash },
    });
    // These are Object and Function.
    VM& vm = globalObject->vm();
    realm->typeJSObject()->setJavaScriptClass(vm, globalObject->objectConstructor(), globalObject->objectPrototype());
    realm->typeJSFunction()->setJavaScriptClass(vm, globalObject->functionConstructor(), globalObject->functionPrototype());
    realm->typeJSPromise()->setJavaScriptClass(vm, globalObject->promiseConstructor(), globalObject->promisePrototype());
    addGetSet(globalObject, realm->typeJSObject(), "__dict__"_s, getInstanceDict, setInstanceDict);
    // There can be a WeakRef to any object, and so can there be a weakref.ref.
    addGetSet(globalObject, realm->typeJSObject(), "__weakref__"_s, getWeakReferences);
    addGetSet(globalObject, realm->typeJSFunction(), "__name__"_s, getFunctionName);
    addGetSet(globalObject, realm->typeJSFunction(), "__self__"_s, getFunctionSelf);
    addGetSet(globalObject, realm->typeJSFunction(), "__func__"_s, getFunctionFunc);
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
    if (auto* boxed = dynamicDowncast<PyBoxedValue>(self))
        return JSValue::encode(boxed->value());
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
                raiseTypeError(globalObject, scope, concatenate("keys must be str, int, float, bool or None, not "_s, typeName(globalObject, key)));
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

// awaitable.then(), .catch() and .finally(): those of the promise for what awaiting it comes to. It is `then` that makes JavaScript wait for something.
template<typename GetName>
static EncodedJSValue callMethodOfPromise(JSGlobalObject* globalObject, CallFrame* callFrame, const GetName& getName)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSPromise* promise = toPromise(globalObject, callFrame->thisValue());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue method = promise->get(globalObject, getName(vm));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(JSC::call(globalObject, method, JSC::getCallData(method), promise, ArgList(callFrame))));
}

static JSC_DECLARE_HOST_FUNCTION(javaScriptThen);
JSC_DEFINE_HOST_FUNCTION(javaScriptThen, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return callMethodOfPromise(globalObject, callFrame, [] (VM& vm) -> const Identifier& { return vm.propertyNames->then; });
}

static JSC_DECLARE_HOST_FUNCTION(javaScriptCatch);
JSC_DEFINE_HOST_FUNCTION(javaScriptCatch, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return callMethodOfPromise(globalObject, callFrame, [] (VM& vm) -> const Identifier& { return vm.propertyNames->catchKeyword; });
}

static JSC_DECLARE_HOST_FUNCTION(javaScriptFinally);
JSC_DEFINE_HOST_FUNCTION(javaScriptFinally, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return callMethodOfPromise(globalObject, callFrame, [] (VM& vm) -> const Identifier& { return vm.propertyNames->finallyKeyword; });
}

// obj[Symbol.asyncIterator](): aiter(obj)
static JSC_DECLARE_HOST_FUNCTION(javaScriptAsyncIterator);
JSC_DEFINE_HOST_FUNCTION(javaScriptAsyncIterator, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSValue::encode(getAsyncIterator(globalObject, callFrame->thisValue()));
}

// The next(), return() and throw() of an asynchronous iterator, each of which gives a promise for { value, done }.
enum class AsyncStep : uint8_t { Next, Return, Throw };
static EncodedJSValue stepAsyncIterator(JSGlobalObject* globalObject, CallFrame* callFrame, AsyncStep step)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue self = callFrame->thisValue();
    JSValue given = callFrame->argument(0);
    JSValue awaitable;
    JSValue settlement = jsBoolean(true);
    switch (step) {
    case AsyncStep::Next:
        if (given.isUndefined()) {
            JSValue instance;
            JSValue method = lookupSpecial(globalObject, self, vm.pythonNames().dunder_anext, instance);
            if (method)
                awaitable = callMethod(globalObject, method, instance);
        } else {
            JSValue send = getAttribute(globalObject, self, Identifier::fromString(vm, "asend"_s));
            if (send)
                awaitable = call(globalObject, send, given);
        }
        break;
    case AsyncStep::Return: {
        settlement = createIteratorResultObject(globalObject, given, true);
        JSValue close = getAttributeIfPresent(globalObject, self, Identifier::fromString(vm, "aclose"_s));
        if (!scope.exception() && !close)
            return JSValue::encode(JSPromise::resolvedPromise(globalObject, settlement));
        if (close)
            awaitable = call(globalObject, close);
        break;
    }
    case AsyncStep::Throw: {
        JSValue method = getAttribute(globalObject, self, Identifier::fromString(vm, "athrow"_s));
        if (method)
            awaitable = call(globalObject, method, given);
        break;
    }
    }
    // What goes wrong is what the promise is rejected with, whenever it goes wrong.
    if (scope.exception()) [[unlikely]]
        return JSValue::encode(JSPromise::rejectedPromiseWithCaughtException(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(toPromise(globalObject, awaitable, settlement)));
}

static JSC_DECLARE_HOST_FUNCTION(javaScriptAsyncNext);
JSC_DEFINE_HOST_FUNCTION(javaScriptAsyncNext, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return stepAsyncIterator(globalObject, callFrame, AsyncStep::Next);
}

static JSC_DECLARE_HOST_FUNCTION(javaScriptAsyncReturn);
JSC_DEFINE_HOST_FUNCTION(javaScriptAsyncReturn, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return stepAsyncIterator(globalObject, callFrame, AsyncStep::Return);
}

// generator.return(value), of a generator that is written in Python: generator.close(). So it is what `break` in a `for (... of ...)` does, and whatever else has done with an iterator
// before it has finished. GeneratorExit is raised in it, so that `with` in it is left as it is when something has gone wrong, and not as if all were well.
static JSC_DECLARE_HOST_FUNCTION(javaScriptGeneratorReturn);
JSC_DEFINE_HOST_FUNCTION(javaScriptGeneratorReturn, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* generator = dynamicDowncast<JSGenerator>(callFrame->thisValue());
    if (!generator)
        return throwVMTypeError(globalObject, scope, "|this| should be a generator"_s);
    JSValue returned = generatorClose(globalObject, generator);
    RETURN_IF_EXCEPTION(scope, { });
    // What it returns on being told to stop, if it returns anything, and otherwise what it was told to.
    return JSValue::encode(createIteratorResultObject(globalObject, isNone(returned) ? callFrame->argument(0) : returned, true));
}

static JSC_DECLARE_HOST_FUNCTION(javaScriptAsyncThrow);
JSC_DEFINE_HOST_FUNCTION(javaScriptAsyncThrow, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return stepAsyncIterator(globalObject, callFrame, AsyncStep::Throw);
}

// For `async with` over something of JavaScript's: calls the method that it is given first, and gives a promise for what comes of it.
static JSC_DECLARE_HOST_FUNCTION(javaScriptDisposeAndWait);
JSC_DEFINE_HOST_FUNCTION(javaScriptDisposeAndWait, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = callFrame->argument(0);
    JSValue result = JSC::call(globalObject, method, JSC::getCallData(method), callFrame->thisValue(), ArgList());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(JSPromise::resolvedPromise(globalObject, result)));
}

// obj[Symbol.toPrimitive](hint): what stands for it where JavaScript wants a number or a string. Number(obj) is float(obj), or failing that what it
// is as an index, and String(obj) is str(obj).
static JSC_DECLARE_HOST_FUNCTION(javaScriptToPrimitive);
JSC_DEFINE_HOST_FUNCTION(javaScriptToPrimitive, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSValue self = callFrame->thisValue();
    JSValue hint = callFrame->argument(0);
    bool wantsString = hint.isString() && asString(hint)->value(globalObject).data == "string"_s;
    RETURN_IF_EXCEPTION(scope, { });
    if (!wantsString) {
        for (const Identifier* name : { &names.dunder_float, &names.dunder_index, &names.dunder_int }) {
            JSValue instance;
            JSValue method = lookupSpecial(globalObject, self, *name, instance);
            RETURN_IF_EXCEPTION(scope, { });
            if (!method)
                continue;
            JSValue result = callMethod(globalObject, method, instance);
            RETURN_IF_EXCEPTION(scope, { });
            if (!result.isNumber() && !result.isHeapBigInt())
                return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(typeName(globalObject, self), '.', name->string(), " returned non-"_s, name == &names.dunder_float ? "float"_s : "int"_s, " (type "_s, typeName(globalObject, result), ')')));
            return JSValue::encode(result);
        }
    }
    String text = str(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

static JSC_DECLARE_HOST_FUNCTION(javaScriptIsTrue);

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
    add("then"_s, javaScriptThen);
    add("catch"_s, javaScriptCatch);
    add("finally"_s, javaScriptFinally);
    add("asyncIterator"_s, javaScriptAsyncIterator);
    add("asyncNext"_s, javaScriptAsyncNext);
    add("asyncReturn"_s, javaScriptAsyncReturn);
    add("generatorReturn"_s, javaScriptGeneratorReturn);
    add("asyncThrow"_s, javaScriptAsyncThrow);
    add("disposeAndWait"_s, javaScriptDisposeAndWait);
    add("toPrimitive"_s, javaScriptToPrimitive);
    add("isTrue"_s, javaScriptIsTrue);
    return object;
}

// Whether it is one of the kinds of cell that has the methods of PYTHON_DECLARE_EXOTIC_METHODS.
static bool asksClassFirst(JSCell* cell)
{
    switch (cell->type()) {
    case PyInstanceType:
    case PyDictType:
    case PySetType:
    case PyTupleType:
    case PyBoxedValueType:
        return true;
    default:
        return cell->inherits<PyDerivedList>() || cell->inherits<PyDerivedBytes>() || cell->inherits<PyException>();
    }
}

JSValue getPropertyForJavaScript(JSGlobalObject* globalObject, JSValue receiver, PropertyName name, PyType* from)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    PyType* type = typeOf(globalObject, receiver);
    auto function = [&] (ASCIILiteral which) { return realm->javaScriptFunctions()->getDirect(vm, Identifier::fromString(vm, which)); };

    // A generator is one to JavaScript already, and inherits what it does with one. All but how it is told to stop.
    bool isGenerator = type == realm->typeGenerator() && !isClass(receiver);
    if (isGenerator) {
        if (name == vm.propertyNames->returnKeyword)
            return function("generatorReturn"_s);
        if (name == vm.propertyNames->next || name == vm.propertyNames->throwKeyword || name == vm.propertyNames->iteratorSymbol)
            return { };
    }

    if (name.isSymbol()) {
        if (name == vm.propertyNames->iteratorSymbol && !isClass(receiver) && (type->lookup(vm, names.dunder_iter) || type->lookup(vm, names.dunder_getitem)))
            return function("iterator"_s);
        if (name == vm.propertyNames->asyncIteratorSymbol && !isClass(receiver) && type->lookup(vm, names.dunder_aiter))
            return function("asyncIterator"_s);
        // An exception is left to Error.prototype, as it is for toString().
        if (name == vm.propertyNames->toPrimitiveSymbol && !isClass(receiver) && !type->isExceptionType())
            return function("toPrimitive"_s);
        // A context manager is what `using` can be used with.
        if (name == vm.propertyNames->disposeSymbol && !isClass(receiver) && type->lookup(vm, names.dunder_exit))
            return globalObject->linkTimeConstant(LinkTimeConstant::pythonExitContext);
        if (name == vm.propertyNames->asyncDisposeSymbol && !isClass(receiver) && type->lookup(vm, names.dunder_aexit))
            return globalObject->linkTimeConstant(LinkTimeConstant::pythonAsyncExitContext);
        return { };
    }

    // If the class comes before the instance, it was asked before what the instance has was looked at, and is not asked twice.
    bool wasAsked = asksClassFirst(receiver.asCell()) && classComesBeforeInstance(globalObject, type, name, AttributeAccess::Get);
    if (isClass(receiver) && asType(receiver)->javaScriptConstructor()) {
        // What the constructor has as a function is for JavaScript to find, which is in the middle of looking.
        RELEASE_AND_RETURN(scope, getTypeAttribute(globalObject, asType(receiver), name, ClassIsFunctionToo::No, from));
    }
    // What comes before `from` in the order are classes of JavaScript's. Either they have been looked at on the way here, or this is `super.name` in one
    // of them and they are not wanted.
    if (type != from && !isClass(receiver) && !(type->hooks(globalObject) & (PyType::HasCustomGetAttribute | PyType::HasGetAttr))) {
        if (JSValue attribute = type->lookupFrom(vm, from, name))
            RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, attribute, receiver, type));
    } else if (!wasAsked) {
        JSValue value = getAttributeIfPresent(globalObject, receiver, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
    }
    if (isClass(receiver)) {
        // A class of JavaScript's has a name.
        if (name == vm.propertyNames->name)
            return asType(receiver)->name();
        // And is written out as its source, which there is none of here to speak of.
        if (name == vm.propertyNames->toString)
            return function("toString"_s);
        return { };
    }

    // Names that Python has no use for, and that JavaScript expects. An exception is left to Error.prototype, which makes "name: message".
    if (name == vm.propertyNames->constructor)
        return type->object();
    // What is called has a name.
    if (name == vm.propertyNames->name && receiver.isCallable())
        RELEASE_AND_RETURN(scope, getAttributeIfPresent(globalObject, receiver, names.dunder_name));
    if (name == vm.propertyNames->toString && !type->isExceptionType())
        return function("toString"_s);
    bool isMapping = type->hasFlag(PyType::IsMapping);
    bool isSet = type->isSubtypeOf(realm->typeSet()) || type->isSubtypeOf(realm->typeFrozenSet());
    if (name == vm.propertyNames->toJSON && (isMapping || isSet || type->hasFlag(PyType::IsSequence) || type->layout() == PyType::Layout::Boxed))
        return function("toJSON"_s);
    // How many: an array has a length, and a Map and a Set have a size.
    if (name == ((isMapping || isSet) ? vm.propertyNames->size : vm.propertyNames->length) && type->lookup(vm, names.dunder_len)) {
        int64_t count = length(globalObject, receiver);
        RETURN_IF_EXCEPTION(scope, { });
        return intFromInt64(globalObject, count);
    }
    if (name == vm.propertyNames->next && type->lookup(vm, names.dunder_next))
        return function("next"_s);
    // What can be awaited is what JavaScript calls a thenable, so that it waits for it: `await`, Promise.all() and the rest go by `then`.
    if (type->lookup(vm, names.dunder_await)) {
        if (name == vm.propertyNames->then)
            return function("then"_s);
        if (name == vm.propertyNames->catchKeyword)
            return function("catch"_s);
        if (name == vm.propertyNames->finallyKeyword)
            return function("finally"_s);
    }
    if (type->lookup(vm, names.dunder_anext)) {
        if (name == vm.propertyNames->next)
            return function("asyncNext"_s);
        if (name == vm.propertyNames->returnKeyword)
            return function("asyncReturn"_s);
        if (name == vm.propertyNames->throwKeyword)
            return function("asyncThrow"_s);
    }
    return { };
}

bool isCalledByPython(VM& vm, CallFrame* callFrame)
{
    // What is written in C++ comes back into the engine to make a call, and that is passed over.
    EntryFrame* entryFrame = vm.topEntryFrame;
    CallFrame* caller = callFrame->callerFrame(entryFrame);
    if (!caller || caller->isNativeCalleeFrame())
        return false;
    if (CodeBlock* codeBlock = caller->codeBlock())
        return codeBlock->source().provider()->isPython();
    JSCell* callee = caller->jsCallee();
    return callee->inherits<PyNativeFunction>() || callee->type() == PyTypeType || callee->type() == PyBoundMethodType || callee->type() == PyInstanceType;
}

// ---- Classes that JavaScript made

bool isJavaScriptClass(JSCell* cell)
{
    if (cell->type() == JSFunctionType) {
        auto* function = uncheckedDowncast<JSFunction>(cell);
        // What a bound function makes is what the function that it is bound to makes.
        if (function->inherits<PyNativeFunction>() || function->inherits<JSBoundFunction>() || isPythonFunction(function))
            return false;
        return function->isConstructor();
    }
    return cell->type() == InternalFunctionType && asObject(cell)->isConstructor();
}

// F.prototype. It is a property with a value, of a function, so nothing of the program's is run to get it. A function may have none until it is asked for.
static JSObject* prototypeOfConstructor(JSGlobalObject* globalObject, JSObject* constructor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(constructor, PropertySlot::InternalMethodType::GetOwnProperty);
    bool found = constructor->methodTable()->getOwnPropertySlot(constructor, globalObject, vm.propertyNames->prototype, slot);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return nullptr;
    }
    if (!found || !slot.isValue())
        return nullptr;
    return slot.getPureResult().getObject();
}

// C.__new__(cls, ...) for a class of JavaScript's is Reflect.construct(C, [...], cls).
PYTHON_SHARED_NATIVE(javaScriptClassNew)
{
    NATIVE_PROLOGUE();
    JSObject* constructor = asType(uncheckedDowncast<PyNativeFunction>(callFrame->jsCallee())->owner())->javaScriptConstructor();
    MarkedArgumentBuffer arguments;
    for (unsigned i = 1; i < args.size(); ++i)
        arguments.append(args[i]);
    if (args.keywordCount()) {
        // As for calling a function of JavaScript's.
        JSObject* options = constructEmptyObject(globalObject);
        for (unsigned i = 0; i < args.keywordCount(); ++i) {
            auto name = args.keywordName(i)->toIdentifier(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            options->putDirect(vm, name, args.keywordValue(i));
        }
        arguments.append(options);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(construct(globalObject, constructor, JSC::getConstructData(constructor), arguments, args[0])));
}

PYTHON_NATIVE(javaScriptClassInit)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsUndefined());
}

PyType* classFor(JSObject* constructor)
{
    JSGlobalObject* globalObject = constructor->globalObject();
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    ASSERT(isJavaScriptClass(constructor));
    if (JSValue known = constructor->getDirect(vm, names.private_class))
        return uncheckedDowncast<PyType>(known.asCell());
    // If it has none, what it makes has Object.prototype.
    JSObject* prototype = prototypeOfConstructor(globalObject, constructor);
    if (!prototype)
        prototype = globalObject->objectPrototype();
    // It is derived from what its instances inherit from, and not from what the constructor does, where those are not the same.
    PyType* type = PyType::createForJavaScript(vm, globalObject, constructor, prototype, classForPrototype(globalObject, prototype->getPrototypeDirect()));
    constructor->putDirect(vm, names.private_class, type, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    addInstanceDescriptors(globalObject, type, !type->base()->hasFlag(PyType::HasInstanceDict), !type->base()->hasFlag(PyType::HasWeakReferences));
    // In JavaScript there is one step to making an instance where in Python there are two. So it is all in the first.
    using Kind = PyNativeFunction::Kind;
    addMethods(globalObject, type, {
        { "__new__"_s, javaScriptClassNew, Kind::New, 0, "($type, /, *args, **kwargs)"_s, PyNativeFunction::Arguments::AreNotChecked },
        { "__init__"_s, javaScriptClassInit, Kind::Wrapper, 0, "($self, /, *args, **kwargs)"_s, PyNativeFunction::Arguments::AreNotChecked },
    });
    return type;
}

PyType* classForPrototype(JSGlobalObject* globalObject, JSValue prototype)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    for (; prototype.isObject(); prototype = asObject(prototype)->getPrototypeDirect()) {
        if (isType(prototype))
            return asType(prototype);
        JSObject* object = asObject(prototype);
        if (object == globalObject->objectPrototype())
            break;
        JSValue constructor = object->getDirect(vm, vm.propertyNames->constructor);
        if (!constructor || !constructor.isCell() || !isJavaScriptClass(constructor.asCell()))
            continue;
        if (JSValue known = asObject(constructor)->getDirect(vm, vm.pythonNames().private_class)) {
            if (asType(known)->javaScriptPrototype() == object)
                return asType(known);
            continue;
        }
        if (prototypeOfConstructor(globalObject, asObject(constructor)) != object)
            continue;
        return classFor(asObject(constructor));
    }
    return realm->typeJSObject();
}

PyType* metatypeOfJavaScriptClass(JSGlobalObject* globalObject, JSObject* constructor)
{
    // It is that of the first class of Python's that it is derived from.
    for (JSValue parent = constructor->getPrototypeDirect(); parent.isObject(); parent = asObject(parent)->getPrototypeDirect()) {
        if (isType(parent))
            return asType(parent)->metatype();
    }
    return globalObject->pyRealm()->typeType();
}

PyType* tryClass(JSGlobalObject* globalObject, JSValue value)
{
    if (isType(value))
        return asType(value);
    UNUSED_PARAM(globalObject);
    if (value.isCell() && isJavaScriptClass(value.asCell()))
        return classFor(asObject(value));
    return nullptr;
}

JSValue operateFromJavaScript(JSGlobalObject* globalObject, OverloadableOperator op, JSValue left, JSValue right, bool isInPlace)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto binary = [&] (BinaryOperator which) { RELEASE_AND_RETURN(scope, binaryOperation(globalObject, which, isInPlace, left, right)); };
    // A comparison is a boolean to JavaScript, whatever it is that __lt__ returns.
    auto comparison = [&] (ComparisonOperator which) -> JSValue {
        JSValue result = compare(globalObject, which, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, jsBoolean(isTrue(globalObject, result)));
    };
    switch (op) {
    case OverloadableOperator::Add:
        // With a string of JavaScript's it is the object that is asked, and the string is not: in Python a str will be added to nothing but a str, and
        // "value: " + object is how a program in JavaScript says what something is. If the object has nothing to say, that is what it gets.
        if (left.isString() || right.isString()) {
            auto& names = vm.pythonNames();
            bool objectIsOnTheLeft = !left.isString();
            JSValue object = objectIsOnTheLeft ? left : right;
            JSValue string = objectIsOnTheLeft ? right : left;
            PyType* type = typeOf(globalObject, object);
            for (const Identifier* name : { objectIsOnTheLeft && isInPlace ? &names.inPlaceMethod(BinaryOperator::Add) : nullptr, objectIsOnTheLeft ? &names.method(BinaryOperator::Add) : &names.reflectedMethod(BinaryOperator::Add) }) {
                JSValue method = name ? type->lookup(vm, *name) : JSValue();
                if (!method)
                    continue;
                // What a built-in sequence has can only put it together with another of its kind, and has nothing to say about a string. As in binaryOperation(). A Template has: it will
                // not be added to one, so that nobody takes the one for the other (PEP 750).
                if (isSequenceSlot(globalObject, method) && !(name == &names.inPlaceMethod(BinaryOperator::Add) && type->hasFlag(PyType::IsHeapType)) && !type->isSubtypeOf(globalObject->pyRealm()->typeTemplate()))
                    continue;
                JSValue result = call(globalObject, method, object, string);
                RETURN_IF_EXCEPTION(scope, { });
                if (result != globalObject->pyRealm()->notImplemented())
                    return result;
            }
            return { };
        }
        return binary(BinaryOperator::Add);
    case OverloadableOperator::Subtract:
        return binary(BinaryOperator::Sub);
    case OverloadableOperator::Multiply:
        return binary(BinaryOperator::Mult);
    case OverloadableOperator::Divide:
        return binary(BinaryOperator::Div);
    case OverloadableOperator::Remainder:
        return binary(BinaryOperator::Mod);
    case OverloadableOperator::Exponentiate:
        return binary(BinaryOperator::Pow);
    case OverloadableOperator::LeftShift:
        return binary(BinaryOperator::LShift);
    case OverloadableOperator::RightShift:
        return binary(BinaryOperator::RShift);
    case OverloadableOperator::BitwiseAnd:
        return binary(BinaryOperator::BitAnd);
    case OverloadableOperator::BitwiseOr:
        return binary(BinaryOperator::BitOr);
    case OverloadableOperator::BitwiseXor:
        return binary(BinaryOperator::BitXor);
    case OverloadableOperator::Negate:
        RELEASE_AND_RETURN(scope, unaryOperation(globalObject, UnaryOperator::USub, left));
    case OverloadableOperator::BitwiseNot:
        RELEASE_AND_RETURN(scope, unaryOperation(globalObject, UnaryOperator::Invert, left));
    case OverloadableOperator::Equal:
        return comparison(ComparisonOperator::Eq);
    case OverloadableOperator::Less:
        return comparison(ComparisonOperator::Lt);
    case OverloadableOperator::LessOrEqual:
        return comparison(ComparisonOperator::LtE);
    case OverloadableOperator::Greater:
        return comparison(ComparisonOperator::Gt);
    case OverloadableOperator::GreaterOrEqual:
        return comparison(ComparisonOperator::GtE);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isPythonObject(JSGlobalObject* globalObject, JSValue value)
{
    return isType(value) || !typeOf(globalObject, value)->hasFlag(PyType::IsJavaScript);
}

bool getOwnPropertySlotFromJavaScript(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot, bool (*ordinary)(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isGetOrHas = slot.internalMethodType() == PropertySlot::InternalMethodType::Get || slot.internalMethodType() == PropertySlot::InternalMethodType::HasProperty;
    // An attribute whose name is an index to JavaScript is no property, so there is nothing ordinary to find: see getStoredAttribute().
    if (isIndexLike(name)) [[unlikely]] {
        if (slot.isVMInquiry())
            return false;
        JSValue value = isGetOrHas ? getAttributeIfPresent(globalObject, object, name) : getStoredAttribute(vm, object, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (!value)
            return false;
        slot.setValue(object, static_cast<unsigned>(PropertyAttribute::None), value);
        return true;
    }
    if (!isGetOrHas || slot.thisValue() != JSValue(object))
        RELEASE_AND_RETURN(scope, ordinary(object, globalObject, name, slot));

    // If the class has something to say about the attribute, it is asked here, before what the instance has is looked at.
    PyType* type = typeOf(globalObject, object);
    if (classComesBeforeInstance(globalObject, type, name, AttributeAccess::Get)) {
        JSValue value = getAttributeIfPresent(globalObject, object, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (!value)
            return false;
        slot.setValue(object, static_cast<unsigned>(PropertyAttribute::None), value);
        return true;
    }

    bool found = ordinary(object, globalObject, name, slot);
    RETURN_IF_EXCEPTION(scope, false);
    if (found) {
        // That it has nothing to say can be relied on until it is given something.
        if (slot.internalMethodType() == PropertySlot::InternalMethodType::Get && type->instanceAccessIsAsFound().isStillValid())
            slot.setWatchpointSet(type->instanceAccessIsAsFound());
        else
            slot.disableCaching();
    }
    return found;
}

bool setPropertyFromJavaScript(JSGlobalObject* globalObject, JSValue receiver, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // FIXME: Setting an attribute that the class has nothing to say about could be remembered, if a PutPropertySlot could say what has to hold,
    // as a PropertySlot can.
    slot.disableCaching();
    if (!isPythonObject(globalObject, receiver))
        RELEASE_AND_RETURN(scope, JSObject::definePropertyOnReceiver(globalObject, name, value, slot));
    setAttribute(globalObject, receiver, name, value);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

bool deletePropertyFromJavaScript(JSGlobalObject* globalObject, JSValue receiver, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, receiver);
    // To JavaScript, deleting what is not there is done as soon as it is asked for. What a __delattr__ of the class's own means by
    // AttributeError is not for us to say, nor what a property does. A slot with nothing in it is something that is not there.
    bool isForTheClassToSay = type->hooks(globalObject) & PyType::HasCustomSetAttr;
    if (JSValue found = type->lookup(vm, name); found && !isForTheClassToSay) {
        auto* native = dynamicDowncast<PyGetSetDescriptor>(found);
        isForTheClassToSay = !(native && native->isMember()) && isDataDescriptor(globalObject, found);
    }
    deleteAttribute(globalObject, receiver, name);
    if (!scope.exception())
        return true;
    return !isForTheClassToSay && catchException(globalObject, BuiltinType::AttributeError);
}

bool definePropertyFromJavaScript(JSGlobalObject* globalObject, JSObject* receiver, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (name.isSymbol())
        RELEASE_AND_RETURN(scope, JSObject::defineOwnProperty(receiver, globalObject, name, descriptor, shouldThrow));
    // An attribute is a value, that can be seen, set and deleted unless the class says otherwise. There is nowhere to keep anything else about it.
    if (descriptor.isAccessorDescriptor())
        return typeError(globalObject, scope, shouldThrow, "An attribute of a Python object cannot be an accessor. A property is defined by its class."_s);
    if (!descriptor.writable() || !descriptor.enumerable() || !descriptor.configurable())
        return typeError(globalObject, scope, shouldThrow, "An attribute of a Python object cannot be made read-only, hidden or permanent"_s);
    setAttribute(globalObject, receiver, name, descriptor.value() ? descriptor.value() : jsUndefined());
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

// __exit__ or __aexit__, called with what was thrown, or with nothing.
static JSValue callExitMethod(JSGlobalObject* globalObject, CallFrame* callFrame, const Identifier& name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue manager = callFrame->thisValue();
    JSValue self;
    JSValue method = lookupSpecial(globalObject, manager, name, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return raiseTypeError(globalObject, scope, concatenate('\'', typeName(globalObject, manager), "' object has no "_s, name.string()));
    MarkedArgumentBuffer arguments;
    if (callFrame->argument(0).toBoolean(globalObject)) {
        JSValue thrown = callFrame->argument(1);
        JSValue traceback = thrown.isObject() ? asObject(thrown)->getDirect(vm, vm.pythonNames().private_traceback) : JSValue();
        arguments.append(typeOf(globalObject, thrown)->object());
        arguments.append(thrown);
        arguments.append(traceback ? traceback : jsUndefined());
    } else {
        for (unsigned i = 0; i < 3; ++i)
            arguments.append(jsUndefined());
    }
    RELEASE_AND_RETURN(scope, callMethod(globalObject, method, self, arguments));
}

// bool(value), for what a promise comes to.
JSC_DEFINE_HOST_FUNCTION(javaScriptIsTrue, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSValue::encode(jsBoolean(isTrue(globalObject, callFrame->argument(0))));
}

} // namespace Python

JSC_DEFINE_HOST_FUNCTION(pythonEnterContext, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue manager = callFrame->argument(0);
    const Identifier& name = callFrame->argument(1).toBoolean(globalObject) ? vm.pythonNames().dunder_aenter : vm.pythonNames().dunder_enter;
    JSValue self;
    JSValue method = Python::lookupSpecial(globalObject, manager, name, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(Python::raiseTypeError(globalObject, scope, Python::concatenate('\'', Python::typeName(globalObject, manager), "' object does not support the context manager protocol (missed "_s, name.string(), " method)"_s)));
    RELEASE_AND_RETURN(scope, JSValue::encode(Python::callMethod(globalObject, method, self)));
}

JSC_DEFINE_HOST_FUNCTION(pythonExitContext, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = Python::callExitMethod(globalObject, callFrame, vm.pythonNames().dunder_exit);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(Python::isTrue(globalObject, result))));
}

JSC_DEFINE_HOST_FUNCTION(pythonAsyncExitContext, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue awaitable = Python::callExitMethod(globalObject, callFrame, vm.pythonNames().dunder_aexit);
    if (scope.exception()) [[unlikely]]
        return JSValue::encode(JSPromise::rejectedPromiseWithCaughtException(globalObject, scope));
    JSPromise* promise = Python::toPromise(globalObject, awaitable);
    JSValue then = promise->get(globalObject, vm.propertyNames->then);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    arguments.append(globalObject->pyRealm()->javaScriptFunction("isTrue"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(JSC::call(globalObject, then, JSC::getCallData(then), promise, arguments)));
}

} // namespace JSC
