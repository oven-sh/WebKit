/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyJSView.h"

#include "AsyncIteratorPrototype.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "FunctionPrototype.h"
#include "IteratorOperations.h"
#include "JSArrayIterator.h"
#include "JSCInlines.h"
#include "JSIteratorPrototype.h"
#include "JSPromise.h"
#include "ObjectConstructor.h"
#include "PropertyNameArray.h"
#include "ProxyObject.h"
#include "PyAbstract.h"
#include "PyDateTime.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyGenerator.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyJSClass.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "TypeError.h"

namespace JSC {
namespace Python {

// ---- What kind of object it is ----

static ALWAYS_INLINE bool jsViewHas(PyType* type, Special special)
{
    JSValue method = type->lookup(special);
    return method && !isNone(method); // `__iter__ = None` in a class says that it has none.
}

static ALWAYS_INLINE bool jsViewIsMapping(JSObject* object, PyType* type)
{
    return object->type() == PyDictType || type->has(TypeFlag::Mapping);
}

static bool jsViewIsIterable(JSObject* object, PyType* type)
{
    if (JSValue method = type->lookup(Special::Iter))
        return !isNone(method);
    return jsViewHas(type, Special::GetItem) && !jsViewIsMapping(object, type);
}

static bool jsViewIsAwaitable(JSObject* object, PyType* type)
{
    if (jsViewHas(type, Special::Await))
        return true;
    auto* generator = PyGenerator::tryFrom(object);
    return generator && (generator->kind() == PyGenerator::Kind::Coroutine || generator->isIterableCoroutine);
}

// The number an instance of float, of int or of a class derived from either stands for. Empty: it is no such thing.
static JSValue jsViewNumberOf(JSValue value)
{
    if (!value.isCell())
        return { };
    switch (value.asCell()->type()) {
    case PyFloatType:
        return jsNumber(uncheckedDowncast<PyFloat>(value.asCell())->value());
    case PyIntType:
        return uncheckedDowncast<PyInt>(value.asCell())->value();
    default:
        return { };
    }
}

// What has a length and can be iterated over, and is no mapping, is an array to JSON.
static bool jsViewIsArrayLike(JSObject* object, PyType* type)
{
    return jsViewHas(type, Special::Len) && jsViewIsIterable(object, type) && !jsViewIsMapping(object, type);
}

// A date, a time or a datetime is to JSON what a Date is: its text by ISO 8601.
static bool jsViewIsDateOrTime(JSValue object)
{
    return DateTime::PyDate::is(object) || DateTime::PyTimeOfDay::is(object);
}

static ALWAYS_INLINE JSString* jsViewName(VM& vm, PropertyName propertyName)
{
    return jsString(vm, String(propertyName.uid()));
}

static ALWAYS_INLINE bool jsViewIsDunder(StringView name)
{
    return name.length() > 4 && name.startsWith("__"_s) && name.endsWith("__"_s);
}

static ALWAYS_INLINE bool jsViewFound(JSObject* object, PropertySlot& slot, JSValue value, unsigned attributes = static_cast<unsigned>(PropertyAttribute::None))
{
    slot.setValue(object, attributes, valueForJS(value));
    return true;
}

static void jsViewAddKeysOfDict(JSGlobalObject*, PropertyNameArrayBuilder&, PyDict*, bool skipDunder = false);

// ---- Items ----

// mapping[key]. Empty: it has no such key, or an exception. (Not by trying: a defaultdict would make the item.)
static JSValue jsViewMappingItem(JSGlobalObject* globalObject, JSObject* object, PyType* type, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* dict = PyDict::tryFrom(object)) {
        JSValue method = type->lookup(Special::GetItem);
        if (method == rt.dictType()->lookup(Special::GetItem) || method == rt.frozendictType()->lookup(Special::GetItem))
            RELEASE_AND_RETURN(scope, dict->get(globalObject, key));
    }
    bool isThere = contains(globalObject, object, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isThere)
        return { };
    RELEASE_AND_RETURN(scope, getItem(globalObject, object, key));
}

bool JSView::getOwnPropertySlotByIndex(JSObject* object, unsigned index, PropertySlot& slot)
{
    JSGlobalObject* globalObject = object->realm();
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    slot.disableCaching();
    if (slot.isVMInquiry()) {
        slot.setIsTaintedByOpaqueObject();
        return false;
    }
    if (auto* tuple = PyTuple::tryFrom(object); tuple && tuple->type() == rt.tupleType())
        return index < tuple->length() && jsViewFound(object, slot, tuple->at(index));

    PyType* type = rt.typeOf(object);
    JSValue item;
    if (jsViewIsMapping(object, type)) {
        item = jsViewMappingItem(globalObject, object, type, jsNumber(index));
        RETURN_IF_EXCEPTION(scope, false);
        if (!item) {
            item = jsViewMappingItem(globalObject, object, type, jsString(vm, String::number(index)));
            RETURN_IF_EXCEPTION(scope, false);
        }
    } else if (jsViewHas(type, Special::GetItem)) {
        item = getItem(globalObject, object, jsNumber(index));
        if (scope.exception()) [[unlikely]] {
            scope.release();
            clearPendingExceptionIfMatches(globalObject, rt.LookupError());
            return false;
        }
    }
    return item && jsViewFound(object, slot, item);
}

// ---- Properties ----

static bool jsViewSymbolProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, PropertySlot& slot)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto& names = *vm.propertyNames;
    constexpr unsigned hidden = static_cast<unsigned>(PropertyAttribute::DontEnum);
    PyType* type = rt.typeOf(object);
    if (symbolPropertyOfClasses(globalObject, type, propertyName, slot))
        return true;
    if (propertyName == names.iteratorSymbol)
        return jsViewIsIterable(object, type) && jsViewFound(object, slot, rt.jsViewObject(JSViewObject::Iterator), hidden);
    if (propertyName == names.asyncIteratorSymbol)
        return jsViewHas(type, Special::AIter) && jsViewFound(object, slot, rt.jsViewObject(JSViewObject::AsyncIterator), hidden);
    if (propertyName == names.toStringTagSymbol)
        return isPyObject(object) && jsViewFound(object, slot, type->nameString(), hidden);
    return false;
}

// What JS looks for in an object that can do what this one can.
static bool jsViewAdaptorProperty(JSGlobalObject* globalObject, JSObject* object, PyType* type, PropertyName propertyName, PropertySlot& slot)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = *vm.propertyNames;
    constexpr unsigned hidden = static_cast<unsigned>(PropertyAttribute::DontEnum);

    if (propertyName == names.then)
        return jsViewIsAwaitable(object, type) && jsViewFound(object, slot, rt.jsViewObject(JSViewObject::Then), hidden);
    if (propertyName == names.next)
        return jsViewHas(type, Special::Next) && jsViewFound(object, slot, rt.jsViewObject(JSViewObject::Next), hidden);
    if (propertyName == names.returnKeyword)
        return jsViewHas(type, Special::Next) && jsViewFound(object, slot, rt.jsViewObject(JSViewObject::Return), hidden);
    if (propertyName == names.length) {
        if (!jsViewHas(type, Special::Len))
            return false;
        int64_t size = length(globalObject, object);
        RETURN_IF_EXCEPTION(scope, false);
        return jsViewFound(object, slot, jsNumber(static_cast<double>(size)), hidden);
    }
    if (propertyName == names.toJSON) {
        bool hasOne = jsViewNumberOf(object) || PyStr::is(object) || jsViewIsDateOrTime(object) || jsViewIsArrayLike(object, type);
        return hasOne && jsViewFound(object, slot, rt.jsViewObject(JSViewObject::ToJSON), hidden);
    }
    if (!isPyObject(object)) {
        if (propertyName == names.prototype) {
            auto* pyType = PyType::tryFrom(object);
            return pyType && jsViewFound(object, slot, prototypeForJS(globalObject, pyType), hidden);
        }
        // A PyFunction has the name of any function of JS.
        if (propertyName != names.name || object->type() != InternalFunctionType)
            return false;
        JSValue name = getAttrIfPresent(globalObject, object, rt.names.dName());
        RETURN_IF_EXCEPTION(scope, false);
        return name && jsViewFound(object, slot, name, hidden);
    }
    if (propertyName == names.constructor)
        return jsViewFound(object, slot, type, hidden);
    // `extends` asks for it: see PyObject::getConstructData().
    if (propertyName == names.prototype)
        return jsViewHas(type, Special::MroEntries) && jsViewFound(object, slot, jsNull(), hidden);
    if (object->type() != PyExceptionType)
        return false;
    if (propertyName == names.name)
        return jsViewFound(object, slot, type->nameString(), hidden);
    if (propertyName == names.message) {
        JSString* text = str(globalObject, object);
        RETURN_IF_EXCEPTION(scope, false);
        return jsViewFound(object, slot, text, hidden);
    }
    if (propertyName == names.cause) {
        JSValue cause = uncheckedDowncast<PyException>(object)->cause(); // `raise ... from cause`
        return cause && !isNone(cause) && jsViewFound(object, slot, cause, hidden);
    }
    if (propertyName == names.stack) {
        String text = formatException(globalObject, object);
        RETURN_IF_EXCEPTION(scope, false);
        return jsViewFound(object, slot, makeStr(vm, text), hidden);
    }
    // What code written for Node asks an error of the system: error.code === "ENOENT", error.path.
    if (type->isSubtypeOf(rt.OSError()) && !propertyName.isSymbol()) {
        auto* exception = uncheckedDowncast<PyException>(object);
        StringView name = propertyName.uid();
        if (name == "code"_s) {
            JSValue number = exception->field(ExceptionField::osErrorErrno);
            ASCIILiteral code = number && number.isInt32() ? nameOfErrno(number.asInt32()) : ASCIILiteral();
            return !code.isNull() && jsViewFound(object, slot, jsNontrivialString(vm, String(code)), hidden);
        }
        if (name == "path"_s || name == "dest"_s) {
            JSValue file = exception->field(name == "path"_s ? ExceptionField::osErrorFilename : ExceptionField::osErrorFilename2);
            return file && !isNone(file) && jsViewFound(object, slot, valueForJS(file), hidden);
        }
    }
    return false;
}

// Whether Object.keys() lists the attribute: see JSView::getOwnPropertyNames().
static bool jsViewIsEnumerableAttribute(JSGlobalObject* globalObject, JSObject* object, PyType* type, JSString* name)
{
    if (jsViewIsMapping(object, type) || (jsViewHas(type, Special::Len) && jsViewHas(type, Special::GetItem)))
        return false;
    if (auto* pyType = PyType::tryFrom(object))
        return pyType->dict()->get(name) && !jsViewIsDunder(stringOf(name));
    if (instanceAttribute(globalObject, object, name))
        return object->type() != PyModuleType || !jsViewIsDunder(stringOf(name));
    PyTuple* mro = type->mro();
    for (unsigned i = 0; mro && i < mro->length(); ++i) {
        PyTuple* slots = PyType::from(mro->at(i))->slots();
        for (unsigned j = 0; slots && j < slots->length(); ++j) {
            if (WTF::equal(asString(slots->at(j))->tryGetValueImpl(), name->tryGetValueImpl()))
                return true;
        }
    }
    return false;
}

bool JSView::getOwnPropertySlot(JSObject* object, PropertyName propertyName, PropertySlot& slot)
{
    JSGlobalObject* globalObject = object->realm();
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    slot.disableCaching();
    if (slot.isVMInquiry()) {
        slot.setIsTaintedByOpaqueObject();
        return false;
    }
    if (propertyName.isSymbol())
        RELEASE_AND_RETURN(scope, jsViewSymbolProperty(globalObject, object, propertyName, slot));
    if (std::optional<uint32_t> index = parseIndex(propertyName))
        RELEASE_AND_RETURN(scope, getOwnPropertySlotByIndex(object, *index, slot));

    PyType* type = rt.typeOf(object);
    JSString* name = jsViewName(vm, propertyName);
    if (jsViewIsMapping(object, type)) {
        JSValue item = jsViewMappingItem(globalObject, object, type, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (item)
            return jsViewFound(object, slot, item);
    }
    // All of JS takes the name of an error to say what kind it is, which AttributeError.name and the like do not.
    bool isNameOfError = object->type() == PyExceptionType && (propertyName == vm.propertyNames->name || propertyName == vm.propertyNames->stack);
    if (!isNameOfError) {
        JSValue value = object->type() == PyExceptionType ? attributeOfExceptionForJS(globalObject, uncheckedDowncast<PyException>(object), name) : getAttrIfPresent(globalObject, object, name);
        RETURN_IF_EXCEPTION(scope, false);
        // So with the name of a class. What ImportError has under `name` is how its instances find theirs.
        if (value && propertyName == vm.propertyNames->name && PyType::is(object) && rt.typeOf(value)->lookup(Special::Get))
            value = { };
        if (value) {
            // Only who asks for a descriptor cares.
            bool isHidden = slot.internalMethodType() == PropertySlot::InternalMethodType::GetOwnProperty && !jsViewIsEnumerableAttribute(globalObject, object, type, name);
            return jsViewFound(object, slot, value, static_cast<unsigned>(isHidden ? PropertyAttribute::DontEnum : PropertyAttribute::None));
        }
    }
    RELEASE_AND_RETURN(scope, jsViewAdaptorProperty(globalObject, object, type, propertyName, slot));
}

bool JSView::getOwnPropertySlotOfListSubclass(JSObject* object, PropertyName propertyName, PropertySlot& slot)
{
    JSGlobalObject* globalObject = object->realm();
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    slot.disableCaching();
    if (slot.isVMInquiry()) {
        slot.setIsTaintedByOpaqueObject();
        return false;
    }
    JSString* name = jsViewName(vm, propertyName);
    PyDict* dict = instanceDict(globalObject, object, false);
    bool isInDict = dict && dict->get(name);
    bool isThere = isInDict;
    PyTuple* mro = rt.typeOf(object)->mro();
    for (unsigned i = 0; !isThere && mro && i < mro->length(); ++i) {
        PyType* type = PyType::from(mro->at(i));
        isThere = type != rt.listType() && type != rt.objectType() && type->dict()->get(name);
    }
    if (!isThere)
        return false;
    JSValue value = getAttrIfPresent(globalObject, object, name);
    RETURN_IF_EXCEPTION(scope, false);
    return value && jsViewFound(object, slot, value, static_cast<unsigned>(isInDict ? PropertyAttribute::None : PropertyAttribute::DontEnum));
}

void JSView::getOwnPropertyNamesOfListSubclass(JSObject* object, PropertyNameArrayBuilder& builder)
{
    JSGlobalObject* globalObject = object->realm();
    if (!builder.includeStringProperties())
        return;
    if (PyDict* dict = instanceDict(globalObject, object, false))
        jsViewAddKeysOfDict(globalObject, builder, dict);
}

// An assignment that Python refuses is one to a read-only property: silent in sloppy mode.
static bool jsViewWriteFailed(JSGlobalObject* globalObject, bool shouldThrow)
{
    Runtime& rt = runtime(globalObject);
    if (!shouldThrow && !clearPendingExceptionIfMatches(globalObject, rt.AttributeError()))
        clearPendingExceptionIfMatches(globalObject, rt.TypeError());
    return false;
}

bool JSView::putByIndex(JSObject* object, unsigned index, JSValue value, bool shouldThrow)
{
    JSGlobalObject* globalObject = object->realm();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    setItem(globalObject, object, jsNumber(index), value);
    if (!scope.exception()) [[likely]]
        return true;
    RELEASE_AND_RETURN(scope, jsViewWriteFailed(globalObject, shouldThrow));
}

bool JSView::put(JSObject* object, PropertyName propertyName, JSValue value, bool shouldThrow)
{
    JSGlobalObject* globalObject = object->realm();
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(!propertyName.isSymbol());
    if (std::optional<uint32_t> index = parseIndex(propertyName))
        RELEASE_AND_RETURN(scope, putByIndex(object, *index, value, shouldThrow));
    JSString* name = jsViewName(vm, propertyName);
    if (jsViewIsMapping(object, rt.typeOf(object)))
        setItem(globalObject, object, name, value);
    else
        setAttr(globalObject, object, name, value);
    if (!scope.exception()) [[likely]]
        return true;
    RELEASE_AND_RETURN(scope, jsViewWriteFailed(globalObject, shouldThrow));
}

// Deleting what is not there succeeds, as it does in JS.
static bool jsViewDeleteItem(JSGlobalObject* globalObject, JSObject* object, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    delItem(globalObject, object, key);
    if (!scope.exception()) [[likely]]
        return true;
    RELEASE_AND_RETURN(scope, clearPendingExceptionIfMatches(globalObject, rt.LookupError()));
}

bool JSView::deletePropertyByIndex(JSObject* object, unsigned index)
{
    return jsViewDeleteItem(object->realm(), object, jsNumber(index));
}

bool JSView::deleteProperty(JSObject* object, PropertyName propertyName)
{
    JSGlobalObject* globalObject = object->realm();
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(!propertyName.isSymbol());
    if (std::optional<uint32_t> index = parseIndex(propertyName))
        RELEASE_AND_RETURN(scope, deletePropertyByIndex(object, *index));
    JSString* name = jsViewName(vm, propertyName);
    if (jsViewIsMapping(object, rt.typeOf(object)))
        RELEASE_AND_RETURN(scope, jsViewDeleteItem(globalObject, object, name));
    bool isThere = hasAttr(globalObject, object, name);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isThere)
        return true;
    delAttr(globalObject, object, name);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

bool JSView::defineOwnProperty(JSObject* object, PropertyName propertyName, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    JSGlobalObject* globalObject = object->realm();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (descriptor.isAccessorDescriptor())
        return typeError(globalObject, scope, shouldThrow, "A Python object cannot be given an accessor property"_s);
    if (!descriptor.value())
        return true;
    RELEASE_AND_RETURN(scope, put(object, propertyName, descriptor.value(), shouldThrow));
}

bool propertiesArePythons(JSObject* object)
{
    return isPyObject(object) || PyType::is(object) || PyFunction::is(object) || PyNativeFunction::is(object) || PyListObject::is(object) || object->inherits<PyPrototypeView>();
}

// ---- Keys ----

static void jsViewAddKey(JSGlobalObject* globalObject, PropertyNameArrayBuilder& builder, JSValue key, bool skipDunder = false)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isStr(key)) {
        Identifier identifier = strValue(key)->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (!skipDunder || !jsViewIsDunder(identifier.string()))
            builder.add(identifier);
        return;
    }
    if (!isExactInt(key))
        return;
    if (key.isUInt32()) {
        builder.add(key.asUInt32());
        return;
    }
    JSString* text = str(globalObject, key);
    RETURN_IF_EXCEPTION(scope, void());
    Identifier identifier = text->toIdentifier(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    builder.add(identifier);
}

static void jsViewAddKeysOfDict(JSGlobalObject* globalObject, PropertyNameArrayBuilder& builder, PyDict* dict, bool skipDunder)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue key;
    JSValue value;
    for (unsigned position = 0; dict->next(position, key, value);) {
        jsViewAddKey(globalObject, builder, key, skipDunder);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void JSView::getOwnPropertyNames(JSObject* object, PropertyNameArrayBuilder& builder, DontEnumPropertiesMode mode)
{
    JSGlobalObject* globalObject = object->realm();
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!builder.includeStringProperties())
        return;

    PyType* type = rt.typeOf(object);
    if (auto* dict = PyDict::tryFrom(object); dict && type->lookup(Special::Iter) == rt.dictType()->lookup(Special::Iter)) {
        jsViewAddKeysOfDict(globalObject, builder, dict);
        RETURN_IF_EXCEPTION(scope, void());
    } else if (jsViewIsMapping(object, type)) {
        forEach(globalObject, object, [&](JSValue key) {
            jsViewAddKey(globalObject, builder, key);
            return true;
        });
        RETURN_IF_EXCEPTION(scope, void());
    } else if (jsViewHas(type, Special::Len) && jsViewHas(type, Special::GetItem)) {
        int64_t size = length(globalObject, object);
        RETURN_IF_EXCEPTION(scope, void());
        for (int64_t i = 0; i < size && i <= MAX_ARRAY_INDEX; ++i)
            builder.add(static_cast<uint32_t>(i));
    } else {
        // What is no container has its attributes to show.
        if (auto* pyType = PyType::tryFrom(object))
            jsViewAddKeysOfDict(globalObject, builder, pyType->dict(), true);
        else if (auto* instance = PyInstance::tryFrom(object); instance && !instance->dict()) {
            instance->forEachAttribute(globalObject, [&](JSString* name, JSValue) {
                builder.add(Identifier::fromString(vm, name->tryGetValue()));
                return true;
            });
        } else if (PyDict* dict = instanceDict(globalObject, object, false))
            jsViewAddKeysOfDict(globalObject, builder, dict, object->type() == PyModuleType);
        RETURN_IF_EXCEPTION(scope, void());

        // Those of the __slots__ that have a value.
        PyTuple* mro = type->mro();
        for (unsigned i = 0; mro && i < mro->length(); ++i) {
            PyTuple* slots = PyType::from(mro->at(i))->slots();
            for (unsigned j = 0; slots && j < slots->length(); ++j) {
                bool isSet = hasAttr(globalObject, object, asString(slots->at(j)));
                RETURN_IF_EXCEPTION(scope, void());
                if (isSet) {
                    jsViewAddKey(globalObject, builder, slots->at(j));
                    RETURN_IF_EXCEPTION(scope, void());
                }
            }
        }
    }

    if (mode == DontEnumPropertiesMode::Exclude)
        return;
    // All that can be asked for: dir(object).
    JSValue method = type->lookup(Special::Dir);
    if (!method)
        return;
    JSValue names = callSpecial(globalObject, method, object);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    forEach(globalObject, names, [&](JSValue name) {
        if (isStr(name))
            jsViewAddKey(globalObject, builder, name);
        return true;
    });
}

// ---- What all Python objects inherit ----

// `this`. What JS has put around an object of Python's (a Proxy) or made of one (Object.create()) inherits these functions too, and they
// are about that object then: to Python the thing around it is an object of JS, whose str() is its toString().
JSValue objectOfPythonWithin(JSValue self)
{
    for (JSValue value = self; value.isObject();) {
        JSObject* object = asObject(value);
        if (isPyObject(object) || PyType::is(object) || PyFunction::is(object) || PyNativeFunction::is(object) || PyListObject::is(object))
            return object;
        if (auto* proxy = dynamicDowncast<ProxyObject>(object))
            value = proxy->target();
        else
            value = object->getPrototypeDirect();
    }
    return self;
}

static JSValue jsViewThis(CallFrame* callFrame)
{
    return objectOfPythonWithin(callFrame->thisValue());
}

JSC_DEFINE_HOST_FUNCTION(jsViewToString, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSValue::encode(str(globalObject, jsViewThis(callFrame)));
}

// [Symbol.for("nodejs.util.inspect.custom")](): what console.log() and util.inspect() show in Node and Bun. repr(object); for
// an exception, what Python prints when nothing catches it.
JSC_DEFINE_HOST_FUNCTION(jsViewInspect, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue self = jsViewThis(callFrame);
    if (!PyException::is(self))
        RELEASE_AND_RETURN(scope, JSValue::encode(repr(globalObject, self)));
    String text = formatException(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    while (text.endsWith('\n'))
        text = text.left(text.length() - 1);
    return JSValue::encode(makeStr(vm, text));
}

JSC_DEFINE_HOST_FUNCTION(jsViewValueOf, (JSGlobalObject*, CallFrame* callFrame))
{
    JSValue self = jsViewThis(callFrame);
    if (JSValue number = jsViewNumberOf(self))
        return JSValue::encode(number);
    if (PyStr::is(self))
        return JSValue::encode(strValue(self));
    return JSValue::encode(self);
}

JSC_DEFINE_HOST_FUNCTION(jsViewToPrimitive, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue self = jsViewThis(callFrame);
    JSValue hint = callFrame->argument(0);
    String hintText = hint.isString() ? asString(hint)->value(globalObject) : String();
    RETURN_IF_EXCEPTION(scope, { });

    if (hintText != "string"_s) {
        if (JSValue number = jsViewNumberOf(self))
            return JSValue::encode(number);
    }
    if (hintText == "number"_s) {
        PyType* type = rt.typeOf(self);
        if (jsViewHas(type, Special::Float)) {
            double value = toDouble(globalObject, self);
            RETURN_IF_EXCEPTION(scope, { });
            return JSValue::encode(jsNumber(value));
        }
        if (jsViewHas(type, Special::Index))
            RELEASE_AND_RETURN(scope, JSValue::encode(toIndex(globalObject, self)));
        return JSValue::encode(jsNaN());
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(str(globalObject, self)));
}

JSC_DEFINE_HOST_FUNCTION(jsViewToJSON, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue self = jsViewThis(callFrame);
    if (JSValue number = jsViewNumberOf(self))
        return JSValue::encode(number);
    if (PyStr::is(self))
        return JSValue::encode(strValue(self));
    if (jsViewIsDateOrTime(self)) {
        JSValue text = callMethod(globalObject, self, internString(vm, "isoformat"_s));
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(valueForJS(text));
    }
    JSArray* array = newList(globalObject);
    forEach(globalObject, self, [&](JSValue item) {
        listAppend(globalObject, array, valueForJS(item));
        return true;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(array);
}

// ---- Iteration ----

// What [Symbol.iterator]() returns is an object of JS that has the Python iterator out of sight, so that what the iterator
// calls its own attributes does not matter.
static constexpr unsigned jsViewIteratorField = 0;

static JSValue jsViewWrapIterator(JSGlobalObject* globalObject, JSViewObject prototype, JSValue iterator)
{
    JSObject* wrapper = constructEmptyObject(globalObject, runtime(globalObject).jsViewObject(prototype), 1);
    setHiddenField(globalObject, wrapper, jsViewIteratorField, iterator);
    return wrapper;
}

// The Python iterator `this` stands for: what a wrapper holds, or the object itself (iterator.next()).
static JSValue jsViewIteratorOf(JSGlobalObject* globalObject, JSValue self)
{
    if (!self.isObject() || isPyObject(self))
        return self;
    JSValue iterator = hiddenField(globalObject, self, jsViewIteratorField);
    return iterator ? iterator : self;
}

static PyGenerator* jsViewPlainGenerator(JSValue iterator)
{
    auto* generator = PyGenerator::tryFrom(iterator);
    return generator && generator->kind() == PyGenerator::Kind::Generator ? generator : nullptr;
}

JSC_DEFINE_HOST_FUNCTION(jsViewIterator, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A dict is to JS what a Map is: [key, value] pairs. That is what takes a dict wherever an object or a Map of options,
    // headers or entries goes, since what can be iterated over is taken to be a list of pairs.
    if (auto* dict = PyDict::tryFrom(jsViewThis(callFrame))) {
        JSArray* entries = newList(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue key, value;
        for (unsigned position = 0; dict->next(position, key, value);) {
            JSArray* entry = newList(globalObject, std::initializer_list<JSValue> { valueForJS(key), valueForJS(value) });
            RETURN_IF_EXCEPTION(scope, { });
            listAppend(globalObject, entries, entry);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return JSValue::encode(JSArrayIterator::create(vm, globalObject->arrayIteratorStructure(), entries, IterationKind::Values));
    }
    JSValue iterator = getIter(globalObject, jsViewThis(callFrame));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(jsViewWrapIterator(globalObject, JSViewObject::IteratorPrototype, iterator)));
}

// next(value): for a generator, send(value).
JSC_DEFINE_HOST_FUNCTION(jsViewNext, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = jsViewIteratorOf(globalObject, jsViewThis(callFrame));
    JSValue item;
    JSValue result;
    if (auto* generator = jsViewPlainGenerator(iterator))
        item = generatorSend(globalObject, generator, generator->isStarted() ? callFrame->argument(0) : jsUndefined(), result);
    else {
        // Not iterNext(): the value of the StopIteration is what `yield*` gives.
        Runtime& rt = runtime(globalObject);
        JSValue method = rt.typeOf(iterator)->lookup(Special::Next);
        if (!method)
            return JSValue::encode(raiseTypeError(globalObject, scope, '\'', typeName(globalObject, iterator), "' object is not an iterator"_s));
        item = callSpecial(globalObject, method, iterator);
        if (failed(scope, item) && pendingExceptionMatches(globalObject, rt.StopIteration()))
            result = PyException::from(takePendingException(globalObject))->field(ExceptionField::stopIterationValue);
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (item)
        RELEASE_AND_RETURN(scope, JSValue::encode(createIteratorResultObject(globalObject, valueForJS(item), false)));
    RELEASE_AND_RETURN(scope, JSValue::encode(createIteratorResultObject(globalObject, result ? valueForJS(result) : jsUndefined(), true)));
}

// return(value): the loop is left early, so what has a close() is closed.
JSC_DEFINE_HOST_FUNCTION(jsViewReturn, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = jsViewIteratorOf(globalObject, jsViewThis(callFrame));
    if (auto* generator = jsViewPlainGenerator(iterator))
        generatorClose(globalObject, generator);
    else {
        JSValue close = getAttrIfPresent(globalObject, iterator, rt.names.close());
        RETURN_IF_EXCEPTION(scope, { });
        if (close)
            call(globalObject, close);
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(createIteratorResultObject(globalObject, callFrame->argument(0), true)));
}

// throw(exception): raised in a generator where it is suspended; anything else just fails with it.
JSC_DEFINE_HOST_FUNCTION(jsViewThrow, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = jsViewIteratorOf(globalObject, jsViewThis(callFrame));
    JSValue thrown = callFrame->argument(0);
    auto* generator = jsViewPlainGenerator(iterator);
    if (!generator) {
        throwException(globalObject, scope, thrown);
        return { };
    }
    JSValue result;
    JSValue item = generatorThrow(globalObject, generator, exceptionFromThrownValue(globalObject, thrown), result);
    RETURN_IF_EXCEPTION(scope, { });
    if (item)
        RELEASE_AND_RETURN(scope, JSValue::encode(createIteratorResultObject(globalObject, valueForJS(item), false)));
    RELEASE_AND_RETURN(scope, JSValue::encode(createIteratorResultObject(globalObject, result ? valueForJS(result) : jsUndefined(), true)));
}

// ---- Awaiting ----

// `await awaitable`, for a host without an event loop: what suspends for anything but to give way cannot go on.
static JSValue jsViewRunAwaitable(JSGlobalObject* globalObject, JSValue awaitable)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = getAwaitable(globalObject, awaitable, 0);
    RETURN_IF_EXCEPTION(scope, { });
    while (true) {
        JSValue result;
        JSValue yielded;
        if (auto* generator = PyGenerator::tryFrom(iterator); generator && generator->kind() != PyGenerator::Kind::AsyncGenerator) {
            yielded = generatorSend(globalObject, generator, jsUndefined(), result);
            RETURN_IF_EXCEPTION(scope, { });
        } else {
            JSValue method = rt.typeOf(iterator)->lookup(Special::Next);
            if (!method)
                return raiseTypeError(globalObject, scope, '\'', typeName(globalObject, iterator), "' object is not an iterator"_s);
            yielded = callSpecial(globalObject, method, iterator);
            if (scope.exception()) {
                if (!pendingExceptionMatches(globalObject, rt.StopIteration()))
                    return { };
                result = PyException::from(takePendingException(globalObject))->field(ExceptionField::stopIterationValue);
                yielded = { };
            }
        }
        if (!yielded)
            return result ? result : jsUndefined();
        if (!isNone(yielded))
            return raiseRuntimeError(globalObject, scope, "there is no event loop to await this on"_s);
    }
}

// The promise of what the function returns, or raises.
template<typename Function>
static JSPromise* jsViewPromiseOf(JSGlobalObject* globalObject, const Function& function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    JSValue result = function();
    if (scope.exception()) [[unlikely]]
        RELEASE_AND_RETURN(scope, promise->rejectWithCaughtException(vm, scope));
    scope.release();
    promise->resolve(globalObject, vm, valueForJS(result));
    return promise;
}

// The promise of what `await awaitable` gives: it runs on the event loop of the host.
static JSValue jsViewPromiseOfAwaitable(JSGlobalObject* globalObject, JSValue awaitable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue run = eventLoopFunction(globalObject, "run"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (run)
        RELEASE_AND_RETURN(scope, call(globalObject, run, awaitable));
    RELEASE_AND_RETURN(scope, jsViewPromiseOf(globalObject, [&] {
        return jsViewRunAwaitable(globalObject, awaitable);
    }));
}

// promise.then(onFulfilled, onRejected)
static JSValue jsViewChain(JSGlobalObject* globalObject, JSValue promise, JSValue onFulfilled, JSValue onRejected)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue then = promise.get(globalObject, vm.propertyNames->then);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    arguments.append(onFulfilled);
    arguments.append(onRejected);
    RELEASE_AND_RETURN(scope, JSC::call(globalObject, then, JSC::getCallData(then), promise, arguments));
}

JSC_DEFINE_HOST_FUNCTION(jsViewThen, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue promise = jsViewPromiseOfAwaitable(globalObject, jsViewThis(callFrame));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(jsViewChain(globalObject, promise, callFrame->argument(0), callFrame->argument(1))));
}

JSC_DEFINE_HOST_FUNCTION(jsViewAsyncIterator, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = getAIter(globalObject, jsViewThis(callFrame));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(jsViewWrapIterator(globalObject, JSViewObject::AsyncIteratorPrototype, iterator)));
}

// What the promise of iterator.__anext__() is followed by.
JSC_DEFINE_HOST_FUNCTION(jsViewAsyncItem, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSValue::encode(createIteratorResultObject(globalObject, callFrame->argument(0), false));
}

JSC_DEFINE_HOST_FUNCTION(jsViewAsyncEnd, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue thrown = callFrame->argument(0);
    if (rt.typeOf(thrown)->isSubtypeOf(rt.StopAsyncIteration()))
        RELEASE_AND_RETURN(scope, JSValue::encode(createIteratorResultObject(globalObject, jsUndefined(), true)));
    throwException(globalObject, scope, thrown);
    return { };
}

// A promise that is rejected with the exception that is propagating.
static JSValue jsViewRejectedPromise(JSGlobalObject* globalObject)
{
    return jsViewPromiseOf(globalObject, [] { return JSValue(); });
}

JSC_DEFINE_HOST_FUNCTION(jsViewAsyncNext, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = jsViewIteratorOf(globalObject, jsViewThis(callFrame));
    JSValue promise;
    if (JSValue method = rt.typeOf(iterator)->lookup(Special::ANext)) {
        JSValue awaitable = callSpecial(globalObject, method, iterator);
        if (!failed(scope, awaitable))
            promise = jsViewPromiseOfAwaitable(globalObject, awaitable);
    } else
        raiseTypeError(globalObject, scope, '\'', typeName(globalObject, iterator), "' object is not an async iterator"_s);
    if (scope.exception()) [[unlikely]]
        RELEASE_AND_RETURN(scope, JSValue::encode(jsViewRejectedPromise(globalObject)));
    RELEASE_AND_RETURN(scope, JSValue::encode(jsViewChain(globalObject, promise, rt.jsViewObject(JSViewObject::AsyncItem), rt.jsViewObject(JSViewObject::AsyncEnd))));
}

JSC_DEFINE_HOST_FUNCTION(jsViewAsyncReturn, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = jsViewIteratorOf(globalObject, jsViewThis(callFrame));
    JSValue result = createIteratorResultObject(globalObject, callFrame->argument(0), true);
    JSValue promise;
    JSValue close = getAttrIfPresent(globalObject, iterator, internString(vm, "aclose"_s));
    if (!scope.exception() && close) {
        JSValue awaitable = call(globalObject, close);
        if (!failed(scope, awaitable))
            promise = jsViewPromiseOfAwaitable(globalObject, awaitable);
    }
    if (scope.exception()) [[unlikely]]
        RELEASE_AND_RETURN(scope, JSValue::encode(jsViewRejectedPromise(globalObject)));
    if (!promise)
        RELEASE_AND_RETURN(scope, JSValue::encode(JSPromise::resolvedPromise(globalObject, result)));
    // () => result
    MarkedArgumentBuffer bound;
    bound.append(result);
    JSValue constant = JSBoundFunction::create(vm, globalObject, asObject(runtime(globalObject).jsViewObject(JSViewObject::Identity)), jsUndefined(), bound, 0, nullptr, SourceTaintedOrigin::Untainted);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(jsViewChain(globalObject, promise, constant, jsUndefined())));
}

JSC_DEFINE_HOST_FUNCTION(jsViewIdentity, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(callFrame->argument(0));
}

// ---- Calls from JS ----

bool hasKeywordMark(JSGlobalObject* globalObject, JSObject* object)
{
    if (!globalObject->pythonRuntime())
        return false;
    return isValidOffset(object->structure()->get(getVM(globalObject), PropertyName(runtime(globalObject).keywordsKey())));
}

JSObject* makeKeywordMarker(JSGlobalObject* globalObject)
{
    JSObject* object = constructEmptyObject(globalObject);
    object->putDirect(getVM(globalObject), PropertyName(runtime(globalObject).keywordsKey()), jsBoolean(true), static_cast<unsigned>(PropertyAttribute::DontEnum));
    return object;
}

bool keywordsOfMarker(JSGlobalObject* globalObject, JSValue marker, PyTuple*& names, MarkedArgumentBuffer& values)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(marker);
    PropertyNameArrayBuilder keys(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, globalObject, keys, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, false);
    unsigned count = keys.size();
    names = count ? PyTuple::createUninitialized(vm, rt.keywordNamesStructure(), rt.tupleType(), count) : nullptr;
    for (unsigned i = 0; i < count; ++i)
        names->initializeAt(vm, i, jsString(vm, keys[i].string()));
    for (unsigned i = 0; i < count; ++i) {
        JSValue value = object->get(globalObject, keys[i]);
        RETURN_IF_EXCEPTION(scope, false);
        values.append(value);
    }
    return true;
}

JSValue callWithKeywordMarker(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    return withArgumentsFromJS(globalObject, callFrame, [&](const CallArgs& args) {
        return call(globalObject, callFrame->jsCallee(), args);
    });
}

bool isCalledByPython(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = getVM(globalObject);
    if (callFrame->callerFrameOrEntryFrame() == vm.topEntryFrame) {
        auto* call = runtime(globalObject).runtimeCall;
        return call && (!call->entryFrame || call->entryFrame == vm.topEntryFrame);
    }
    CallFrame* caller = callFrame->callerFrame();
    return !caller->isNativeCalleeFrame() && caller->codeBlock() && caller->codeBlock()->unlinkedCodeBlock()->isPython();
}

bool isBeingCalledByPython(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    if (!isCalledByPython(globalObject, callFrame))
        return false;
    if (callFrame->callerFrameOrEntryFrame() == getVM(globalObject).topEntryFrame)
        return true;
    // The register still has what the instruction called: what an instruction reads is alive while it is under way (liveLocalsOfFrame()).
    CallFrame* caller = callFrame->callerFrame();
    const JSInstruction* instruction = caller->codeBlock()->instructions().at(caller->bytecodeIndex()).ptr();
    VirtualRegister callee;
    switch (instruction->opcodeID()) {
    case op_call:
        callee = instruction->as<OpCall>().m_callee;
        break;
    case op_call_varargs:
        callee = instruction->as<OpCallVarargs>().m_callee;
        break;
    default:
        return true;
    }
    // It is this callee, or a method or an object with a __call__, in whose place the interpreter has entered this callee. Otherwise a
    // function of JS was called, and has left its frame to this one (a tail call).
    return !isForeignCallee(caller->r(callee).jsValue());
}

JSValue resultForCaller(JSGlobalObject* globalObject, CallFrame* callFrame, JSValue result)
{
    if (!result || !result.isCell()) [[likely]]
        return result;
    JSType type = result.asCell()->type();
    if (type != PyFloatType && (type != PyGeneratorType || uncheckedDowncast<PyGenerator>(result.asCell())->kind() != PyGenerator::Kind::Coroutine)) [[likely]]
        return result;

    VM& vm = getVM(globalObject);
    if (isCalledByPython(globalObject, callFrame))
        return result;
    if (type == PyFloatType)
        return valueForJS(result);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue run = eventLoopFunction(globalObject, "run"_s);
    bool hasRaised = scope.exception();
    if (!run && !hasRaised)
        return result;
    JSValue promise = !hasRaised ? call(globalObject, run, result) : JSValue();
    if (scope.exception()) [[unlikely]]
        RELEASE_AND_RETURN(scope, JSPromise::create(vm, globalObject->promiseStructure())->rejectWithCaughtException(vm, scope));
    return promise;
}

// kwargs({ a: 1 }): as the last argument of a call of something in Python, a=1.
JSC_DEFINE_HOST_FUNCTION(jsViewKwargs, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue argument = callFrame->argument(0);
    if (!argument.isObject())
        return throwVMTypeError(globalObject, scope, "kwargs() takes an object"_s);
    JSObject* object = asObject(argument);

    PropertyNameArrayBuilder keys(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, globalObject, keys, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* marker = makeKeywordMarker(globalObject);
    for (auto& key : keys) {
        JSValue value = object->get(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        marker->putDirectMayBeIndex(globalObject, key, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(marker);
}

// float(2): the float 2.0, to pass to Python. (A number that is whole is an int there.)
JSC_DEFINE_HOST_FUNCTION(jsViewFloat, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double value = callFrame->argument(0).toNumber(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(makeFloat(globalObject, value)));
}

// method(function () { ... }): a function for Python, whose first argument is the `this` of the function.
JSC_DEFINE_HOST_FUNCTION(jsViewMethod, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSValue::encode(call(globalObject, jsMethodType(runtime(globalObject)), callFrame->argument(0)));
}

JSC_DEFINE_HOST_FUNCTION(jsViewImport, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = callFrame->argument(0).toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(importModule(globalObject, internString(vm, name))));
}

// ---- Making it all ----

void initializeJSView(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto& names = *vm.propertyNames;
    constexpr unsigned hidden = static_cast<unsigned>(PropertyAttribute::DontEnum);

    auto makeObject = [&](JSViewObject which, JSObject* prototype) {
        JSObject* object = constructEmptyObject(globalObject, prototype);
        rt.setJSViewObject(which, object);
        return object;
    };
    auto makePrototype = [&](JSViewObject which, JSObject* prototype) {
        JSObject* object = makeObject(which, prototype);
        object->didBecomePrototype(vm);
        return object;
    };
    auto makeFunction = [&](ASCIILiteral name, unsigned length, NativeFunction function) {
        return JSFunction::create(vm, globalObject, length, name, function, ImplementationVisibility::Public);
    };
    auto addFunction = [&](JSObject* object, PropertyName key, ASCIILiteral name, unsigned length, NativeFunction function) {
        object->putDirect(vm, key, makeFunction(name, length, function), hidden);
    };
    Identifier inspectSymbol = Identifier::fromUid(vm, vm.symbolRegistry().symbolForKey("nodejs.util.inspect.custom"_s).ptr());
    auto addConversions = [&](JSObject* prototype) {
        addFunction(prototype, inspectSymbol, "[nodejs.util.inspect.custom]"_s, 0, jsViewInspect);
        addFunction(prototype, names.toString, "toString"_s, 0, jsViewToString);
        addFunction(prototype, names.toLocaleString, "toLocaleString"_s, 0, jsViewToString);
        addFunction(prototype, names.valueOf, "valueOf"_s, 0, jsViewValueOf);
        addFunction(prototype, names.toPrimitiveSymbol, "[Symbol.toPrimitive]"_s, 1, jsViewToPrimitive);
    };

    addConversions(makePrototype(JSViewObject::ObjectPrototype, globalObject->objectPrototype()));
    addConversions(makePrototype(JSViewObject::FunctionPrototype, globalObject->functionPrototype()));
    // An exception is shown as an Error is, "ValueError: message", by what Error.prototype has.
    addFunction(makePrototype(JSViewObject::ExceptionPrototype, globalObject->errorPrototype()), inspectSymbol, "[nodejs.util.inspect.custom]"_s, 0, jsViewInspect);
    addFunction(makePrototype(JSViewObject::TypeErrorPrototype, globalObject->errorStructure(ErrorType::TypeError)->storedPrototypeObject()), inspectSymbol, "[nodejs.util.inspect.custom]"_s, 0, jsViewInspect);

    JSObject* iteratorPrototype = makePrototype(JSViewObject::IteratorPrototype, globalObject->iteratorPrototype());
    rt.setJSViewObject(JSViewObject::Next, makeFunction("next"_s, 1, jsViewNext));
    rt.setJSViewObject(JSViewObject::Return, makeFunction("return"_s, 1, jsViewReturn));
    iteratorPrototype->putDirect(vm, names.next, rt.jsViewObject(JSViewObject::Next), hidden);
    iteratorPrototype->putDirect(vm, names.returnKeyword, rt.jsViewObject(JSViewObject::Return), hidden);
    addFunction(iteratorPrototype, names.throwKeyword, "throw"_s, 1, jsViewThrow);

    JSObject* asyncIteratorPrototype = makePrototype(JSViewObject::AsyncIteratorPrototype, globalObject->asyncIteratorPrototype());
    addFunction(asyncIteratorPrototype, names.next, "next"_s, 0, jsViewAsyncNext);
    addFunction(asyncIteratorPrototype, names.returnKeyword, "return"_s, 1, jsViewAsyncReturn);

    rt.setJSViewObject(JSViewObject::Iterator, makeFunction("[Symbol.iterator]"_s, 0, jsViewIterator));
    rt.setJSViewObject(JSViewObject::AsyncIterator, makeFunction("[Symbol.asyncIterator]"_s, 0, jsViewAsyncIterator));
    rt.setJSViewObject(JSViewObject::Then, makeFunction("then"_s, 2, jsViewThen));
    rt.setJSViewObject(JSViewObject::AsyncItem, makeFunction("item"_s, 1, jsViewAsyncItem));
    rt.setJSViewObject(JSViewObject::AsyncEnd, makeFunction("end"_s, 1, jsViewAsyncEnd));
    rt.setJSViewObject(JSViewObject::Identity, makeFunction("identity"_s, 1, jsViewIdentity));
    rt.setJSViewObject(JSViewObject::ToJSON, makeFunction("toJSON"_s, 0, jsViewToJSON));

    JSObject* namespaceObject = makeObject(JSViewObject::Namespace, globalObject->objectPrototype());
    constexpr unsigned visible = static_cast<unsigned>(PropertyAttribute::None);
    namespaceObject->putDirect(vm, Identifier::fromString(vm, "kwargs"_s), makeFunction("kwargs"_s, 1, jsViewKwargs), visible);
    namespaceObject->putDirect(vm, Identifier::fromString(vm, "float"_s), makeFunction("float"_s, 1, jsViewFloat), visible);
    namespaceObject->putDirect(vm, Identifier::fromString(vm, "import"_s), makeFunction("import"_s, 1, jsViewImport), visible);
    namespaceObject->putDirect(vm, Identifier::fromString(vm, "method"_s), makeFunction("method"_s, 1, jsViewMethod), visible);
}

} // namespace Python
} // namespace JSC
