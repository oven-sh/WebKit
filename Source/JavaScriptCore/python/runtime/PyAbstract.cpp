/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyAbstract.h"

#include "FrameTracers.h"
#include "FunctionCodeBlock.h"
#include "JSCInlines.h"
#include "LLIntThunks.h"
#include "PyAbstractInlines.h"
#include "PyBytes.h"
#include "PyDescriptors.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyGenerator.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PySet.h"
#include "PyStr.h"
#include "TopExceptionScope.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// ---- Instance dictionaries ----

PyDict* instanceDict(JSGlobalObject* globalObject, JSValue object, bool create)
{
    if (!object.isCell())
        return nullptr;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSCell* cell = object.asCell();

    auto lazily = [&](auto* holder) -> PyDict* {
        PyDict* dict = holder->dict();
        if (!dict && create) {
            dict = PyDict::create(globalObject);
            holder->setDict(vm, dict);
        }
        return dict;
    };

    switch (cell->type()) {
    case PyInstanceType: {
        auto* instance = uncheckedDowncast<PyInstance>(cell);
        if (!instance->type()->has(TypeFlag::HasDict))
            return nullptr;
        if (!create && !instance->hasAttributes(vm))
            return instance->dict();
        return instance->ensureDict(globalObject);
    }
    case PyModuleType:
        return uncheckedDowncast<PyModule>(cell)->dict();
    case PyExceptionType:
        return lazily(uncheckedDowncast<PyException>(cell));
    case JSFunctionType:
        if (PyFunction::is(cell))
            return lazily(uncheckedDowncast<PyFunction>(cell));
        return nullptr;
    case StringType:
    case HeapBigIntType:
    case ArrayType:
    case InternalFunctionType:
        return nullptr;
    default:
        break;
    }

    if (!rt.typeOf(object)->has(TypeFlag::HasDict))
        return nullptr;
    if (cell->inherits<PyWrapperObject>()) {
        auto* wrapper = uncheckedDowncast<PyWrapperObject>(cell);
        if (wrapper->copiesAttributesLater()) [[unlikely]]
            copyAttributesOfWrapped(globalObject, wrapper);
        return lazily(wrapper);
    }
    // An instance of a class that derives from a built-in one. There is no field for the dict.
    JSObject* jsObject = asObject(cell);
    if (JSValue dict = jsObject->getDirect(vm, rt.dictKey()))
        return PyDict::from(dict);
    if (!create)
        return nullptr;
    PyDict* dict = PyDict::create(globalObject);
    jsObject->putDirect(vm, rt.dictKey(), dict, PropertyAttribute::DontEnum | 0);
    return dict;
}

JSValue instanceAttribute(JSGlobalObject* globalObject, JSValue object, JSString* name)
{
    PyDict* dict;
    if (auto* instance = PyInstance::tryFrom(object)) [[likely]] {
        dict = instance->dict();
        if (!dict) [[likely]]
            return instance->getAttribute(getVM(globalObject), name);
    } else
        dict = instanceDict(globalObject, object, false);
    if (!dict)
        return { };
    if (!dict->hasOnlyStringKeys()) [[unlikely]]
        return dict->get(globalObject, name);
    return dict->get(name);
}

// For a name that is an instance of a class derived from str.
static JSValue instanceAttributeByKey(JSGlobalObject* globalObject, JSValue object, JSValue key)
{
    if (PyDict* dict = instanceDict(globalObject, object, false))
        return dict->get(globalObject, key);
    // An instance without attributes has no dict here. CPython's has, and a dict asks a key for its hash before anything else.
    if (auto* instance = PyInstance::tryFrom(object); instance && instance->type()->has(TypeFlag::HasDict))
        hashForContainer(globalObject, key, "a dict key"_s);
    return { };
}

bool setInstanceDict(JSGlobalObject* globalObject, JSValue object, PyDict* dict)
{
    if (!object.isCell())
        return false;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSCell* cell = object.asCell();
    switch (cell->type()) {
    case PyInstanceType:
        if (!uncheckedDowncast<PyInstance>(cell)->type()->has(TypeFlag::HasDict))
            return false;
        uncheckedDowncast<PyInstance>(cell)->setDict(globalObject, dict);
        return true;
    case PyExceptionType:
        uncheckedDowncast<PyException>(cell)->setDict(vm, dict);
        return true;
    case JSFunctionType:
        if (!PyFunction::is(cell))
            return false;
        uncheckedDowncast<PyFunction>(cell)->setDict(vm, dict);
        return true;
    case PyModuleType:
    case StringType:
    case HeapBigIntType:
    case ArrayType:
    case InternalFunctionType:
        return false;
    default:
        break;
    }
    if (!rt.typeOf(object)->has(TypeFlag::HasDict))
        return false;
    if (cell->inherits<PyWrapperObject>()) {
        // What it has yet to copy from what it wraps would have been in the dict that goes.
        uncheckedDowncast<PyWrapperObject>(cell)->setCopiesAttributesLater(false);
        uncheckedDowncast<PyWrapperObject>(cell)->setDict(vm, dict);
        return true;
    }
    asObject(cell)->putDirect(vm, rt.dictKey(), dict, PropertyAttribute::DontEnum | 0);
    return true;
}

// ---- Descriptors ----

static ALWAYS_INLINE bool isMethodDescriptor(JSValue descriptor)
{
    if (!descriptor.isCell())
        return false;
    JSCell* cell = descriptor.asCell();
    if (cell->type() == JSFunctionType)
        return PyFunction::is(cell);
    if (PyNativeFunction::is(cell)) {
        NativeKind kind = uncheckedDowncast<PyNativeFunction>(cell)->kind();
        return kind == NativeKind::Method || kind == NativeKind::SlotWrapper;
    }
    return false;
}

bool isCalledWithInstanceFirst(JSValue descriptor) { return isMethodDescriptor(descriptor); }

JSValue memberDescriptorGet(JSGlobalObject* globalObject, PyMemberDescriptor* descriptor, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = asObject(object)->getDirect(vm, PropertyName(&descriptor->key()->uid()));
    // Which attribute of which object it was is for whoever asked to say: getAttr(). For super().name that is the super object.
    if (!value)
        return raise(globalObject, scope, runtime(globalObject).AttributeError(), makeString('\'', typeNameForMessage(globalObject, typeOf(globalObject, object)), "' object has no attribute '"_s, stringOf(descriptor->name()), '\''));
    return value;
}

void memberDescriptorSet(JSGlobalObject* globalObject, PyMemberDescriptor* descriptor, JSValue object, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* holder = asObject(object);
    PropertyName key(&descriptor->key()->uid());
    if (value) {
        holder->putDirect(vm, key, value, PropertyAttribute::DontEnum | 0);
        return;
    }
    if (!holder->getDirect(vm, key)) {
        raise(globalObject, scope, runtime(globalObject).AttributeError(), stringOf(descriptor->name()));
        return;
    }
    DeletePropertySlot slot;
    JSObject::deleteProperty(holder, globalObject, key, slot);
}

// A class may have, as an attribute of its own, the method of a type it has nothing to do with: class C: __len__ = list.__len__
static ALWAYS_INLINE bool nativeMethodAppliesTo(Runtime& rt, PyNativeFunction* method, JSValue object)
{
    PyType* owner = method->ownerType();
    PyType* type = rt.typeOf(object);
    return type == owner || type->isSubtypeOf(owner);
}

static NEVER_INLINE JSValue raiseDescriptorDoesNotApply(JSGlobalObject* globalObject, const String& name, PyType* owner, JSValue object)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    return raiseTypeError(globalObject, scope, "descriptor '"_s, name, "' for '"_s, owner->nameInMessages(), "' objects doesn't apply to a '"_s, typeName(globalObject, object), "' object"_s);
}

// descriptor.__get__(object, type). What the class of the descriptor has under that name is called as it is, with the descriptor in
// front. It is not bound first, as the other special methods are: a staticmethod gets three arguments, a classmethod is not callable.
// For a function that comes to the same, also for one of JS, which has the descriptor as `this`.
static JSValue callGet(JSGlobalObject* globalObject, JSValue getter, JSValue descriptor, JSValue object, PyType* type)
{
    if (!object)
        object = jsUndefined();
    if (isMethodDescriptor(getter) || (getter.isCell() && getter.asCell()->type() == JSFunctionType)) [[likely]]
        return callSpecial(globalObject, getter, descriptor, object, type);
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    if (failed(scope, rt.tryEnterFrame())) [[unlikely]]
        return { };
    JSValue result = call(globalObject, getter, descriptor, object, type);
    rt.leaveFrame();
    scope.release();
    return result;
}

// The class of what was found in a class, if that class may make a descriptor of it. Null: it is a function, which bindDescriptor()
// knows about, or a value of a type that has no such methods.
static ALWAYS_INLINE PyType* typeOfPossibleDescriptor(Runtime& rt, JSValue descriptor)
{
    if (!descriptor.isCell())
        return nullptr;
    JSCell* cell = descriptor.asCell();
    switch (cell->type()) {
    case JSFunctionType:
    case StringType:
    case HeapBigIntType:
    case ArrayType:
        return nullptr;
    case InternalFunctionType:
        // A class is an instance of its metaclass.
        return PyType::is(cell) ? uncheckedDowncast<PyType>(cell)->type() : nullptr;
    default:
        return rt.typeOf(descriptor);
    }
}

JSValue bindDescriptor(JSGlobalObject* globalObject, JSValue descriptor, JSValue object, PyType* type)
{
    if (!descriptor.isCell())
        return descriptor;
    Runtime& rt = runtime(globalObject);
    JSCell* cell = descriptor.asCell();
    switch (cell->type()) {
    case JSFunctionType:
        if (PyFunction::is(cell) || foreignMethodKind(cell) != ForeignMethodKind::NotAMethod)
            return object ? JSValue(PyMethod::create(globalObject, descriptor, object)) : descriptor;
        return descriptor;
    case InternalFunctionType:
        if (auto* native = PyNativeFunction::tryFrom(descriptor)) {
            switch (native->kind()) {
            case NativeKind::Method:
            case NativeKind::SlotWrapper:
                if (!object)
                    return descriptor;
                if (!nativeMethodAppliesTo(rt, native, object)) [[unlikely]]
                    return raiseDescriptorDoesNotApply(globalObject, native->def().name, native->ownerType(), object);
                return PyNativeFunction::createBound(globalObject, native, object);
            case NativeKind::ClassMethod:
                if (type != native->ownerType() && !type->isSubtypeOf(native->ownerType())) [[unlikely]] {
                    auto scope = DECLARE_THROW_SCOPE(rt.vm());
                    return raiseTypeError(globalObject, scope, "descriptor '"_s, native->def().name, "' requires a subtype of '"_s, native->ownerType()->nameInMessages(), "' but received '"_s, type->nameInMessages(), '\'');
                }
                return PyNativeFunction::createBound(globalObject, native, type);
            default:
                return descriptor;
            }
        }
        break;
    default:
        break;
    }
    PyType* descriptorType = typeOfPossibleDescriptor(rt, descriptor);
    JSValue getter = descriptorType ? descriptorType->lookup(Special::Get) : JSValue();
    if (!getter)
        return descriptor;
    if (!rt.vm().isSafeToRecurseSoft()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(rt.vm());
        return raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded"_s);
    }
    return callGet(globalObject, getter, descriptor, object, type);
}

// ---- Attributes ----

static NEVER_INLINE JSValue raiseNoAttribute(JSGlobalObject* globalObject, ThrowScope& scope, JSValue object, JSString* name, JSValue key = { })
{
    return raiseAttributeError(globalObject, scope, object, key ? key : JSValue(name), makeString('\'', typeName(globalObject, object), "' object has no attribute '"_s, stringOf(name), '\''));
}

// `nameIsKey`, here and below: getattr() and its kin were given an instance of a class derived from str, `key`, whose text is `name`.
// CPython has names as the keys of dicts, so such a name is looked up as a dict looks up any key: its __hash__ and its __eq__ say what
// it finds.
template<bool nameIsKey>
static ALWAYS_INLINE JSValue genericGetAttrImpl(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue key, bool suppress)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    PyType* type = rt.typeOf(object);
    JSValue descriptor = nameIsKey ? type->lookupKey(globalObject, key) : type->lookup(name);
    if constexpr (nameIsKey)
        RETURN_IF_EXCEPTION(scope, { });
    JSValue getter;
    bool bindsDirectly = false;
    if (descriptor) {
        if (isMethodDescriptor(descriptor))
            bindsDirectly = true;
        else if (PyType* descriptorType = typeOfPossibleDescriptor(rt, descriptor)) {
            JSCell* cell = descriptor.asCell();
            // The data descriptors written in C++.
            if (cell->type() == PyObjectType) {
                const ClassInfo* info = cell->classInfo();
                if (info == PyGetSetDescriptor::info()) {
                    auto* getSet = uncheckedDowncast<PyGetSetDescriptor>(cell);
                    if (type != getSet->owner() && !type->isSubtypeOf(getSet->owner())) [[unlikely]]
                        RELEASE_AND_RETURN(scope, raiseDescriptorDoesNotApply(globalObject, getSet->def().name, getSet->owner(), object));
                    RELEASE_AND_RETURN(scope, getSet->def().getter(globalObject, object));
                }
                if (info == PyMemberDescriptor::info()) {
                    auto* member = uncheckedDowncast<PyMemberDescriptor>(cell);
                    if (type != member->owner() && !type->isSubtypeOf(member->owner())) [[unlikely]]
                        RELEASE_AND_RETURN(scope, raiseDescriptorDoesNotApply(globalObject, stringOf(member->name()), member->owner(), object));
                    RELEASE_AND_RETURN(scope, memberDescriptorGet(globalObject, member, object));
                }
                // As property.__get__(). (A class derived from property may have its own.)
                if (info == PyProperty::info() && uncheckedDowncast<PyProperty>(cell)->type() == rt.propertyType()) {
                    if (JSValue function = uncheckedDowncast<PyProperty>(cell)->getter.get(); !isNone(function))
                        RELEASE_AND_RETURN(scope, call(globalObject, function, object));
                }
            }
            getter = descriptorType->lookup(Special::Get);
            if (getter && (descriptorType->lookup(Special::Set) || descriptorType->lookup(Special::Delete)))
                RELEASE_AND_RETURN(scope, callGet(globalObject, getter, descriptor, object, type));
        } else if (descriptor.isCell() && (descriptor.asCell()->type() == InternalFunctionType || descriptor.asCell()->type() == JSFunctionType))
            bindsDirectly = true; // A class method, a function of JS, or nothing to bind.
    }

    JSValue value = nameIsKey ? instanceAttributeByKey(globalObject, object, key) : instanceAttribute(globalObject, object, name);
    EXCEPTION_ASSERT(!scope.exception() || !value);
    if (value)
        return value;
    RETURN_IF_EXCEPTION(scope, { });
    // object.__getattribute__(cls, name): a class is an instance of its metaclass, and its dict the dict of that instance. So what
    // the class itself defines is found, as it is there.
    if (PyType* itself = PyType::tryFrom(object); itself && itself->isHeapType()) [[unlikely]] {
        value = itself->dict()->get(globalObject, nameIsKey ? key : JSValue(name));
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
    }

    if (bindsDirectly)
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, descriptor, object, type));
    if (getter)
        RELEASE_AND_RETURN(scope, callGet(globalObject, getter, descriptor, object, type));
    if (descriptor)
        return descriptor;
    if (suppress)
        return { };
    return raiseNoAttribute(globalObject, scope, object, name, key);
}

JSValue genericGetAttr(JSGlobalObject* globalObject, JSValue object, JSString* name, bool suppress)
{
    return genericGetAttrImpl<false>(globalObject, object, name, { }, suppress);
}

NEVER_INLINE JSValue genericGetAttrByKey(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue key, bool suppress)
{
    return genericGetAttrImpl<true>(globalObject, object, name, key, suppress);
}

template<bool nameIsKey>
static ALWAYS_INLINE void genericSetAttrImpl(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue key, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = rt.typeOf(object);
    JSValue descriptor = nameIsKey ? type->lookupKey(globalObject, key) : type->lookup(name);
    if constexpr (nameIsKey)
        RETURN_IF_EXCEPTION(scope, void());
    if (PyType* descriptorType = descriptor ? typeOfPossibleDescriptor(rt, descriptor) : nullptr) {
        JSCell* cell = descriptor.asCell();
        if (cell->type() == PyObjectType) {
            const ClassInfo* info = cell->classInfo();
            if (info == PyGetSetDescriptor::info()) {
                auto& def = uncheckedDowncast<PyGetSetDescriptor>(cell)->def();
                if (PyType* owner = uncheckedDowncast<PyGetSetDescriptor>(cell)->owner(); type != owner && !type->isSubtypeOf(owner)) [[unlikely]] {
                    scope.release();
                    raiseDescriptorDoesNotApply(globalObject, def.name, owner, object);
                    return;
                }
                if (!def.setter) {
                    if (def.isMember)
                        raise(globalObject, scope, rt.AttributeError(), "readonly attribute"_s);
                    else
                        raise(globalObject, scope, rt.AttributeError(), makeString("attribute '"_s, def.name, "' of '"_s, uncheckedDowncast<PyGetSetDescriptor>(cell)->owner()->nameInMessages(), "' objects is not writable"_s));
                    return;
                }
                RELEASE_AND_RETURN(scope, def.setter(globalObject, object, value));
            }
            if (info == PyMemberDescriptor::info()) {
                auto* member = uncheckedDowncast<PyMemberDescriptor>(cell);
                if (type != member->owner() && !type->isSubtypeOf(member->owner())) [[unlikely]] {
                    scope.release();
                    raiseDescriptorDoesNotApply(globalObject, stringOf(member->name()), member->owner(), object);
                    return;
                }
                RELEASE_AND_RETURN(scope, memberDescriptorSet(globalObject, member, object, value));
            }
            // As property.__set__().
            if (value && info == PyProperty::info() && uncheckedDowncast<PyProperty>(cell)->type() == rt.propertyType()) {
                if (JSValue function = uncheckedDowncast<PyProperty>(cell)->setter.get(); !isNone(function)) {
                    scope.release();
                    call(globalObject, function, object, value);
                    return;
                }
            }
        }
        JSValue setter = descriptorType->lookup(Special::Set);
        JSValue deleter = descriptorType->lookup(Special::Delete);
        if (setter || deleter) {
            if (value) {
                if (!setter) {
                    raise(globalObject, scope, rt.AttributeError(), "__set__"_s);
                    return;
                }
                scope.release();
                callSpecial(globalObject, setter, descriptor, object, value);
                return;
            }
            if (!deleter) {
                raise(globalObject, scope, rt.AttributeError(), "__delete__"_s);
                return;
            }
            scope.release();
            callSpecial(globalObject, deleter, descriptor, object);
            return;
        }
    }

    if (auto* instance = PyInstance::tryFrom(object); !nameIsKey && instance && instance->type()->has(TypeFlag::HasDict) && (!instance->dict() || instance->dict()->hasOnlyStringKeys())) [[likely]] {
        if (value)
            RELEASE_AND_RETURN(scope, instance->setAttribute(globalObject, name, value));
        bool deleted = instance->deleteAttribute(globalObject, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (!deleted)
            raiseNoAttribute(globalObject, scope, object, name);
        return;
    }

    PyDict* dict = instanceDict(globalObject, object, true);
    if (!dict) {
        String text = stringOf(name);
        if (descriptor)
            raise(globalObject, scope, rt.AttributeError(), makeString('\'', type->nameInMessages(), "' object attribute '"_s, text, "' is read-only"_s));
        else {
            // The hint is for classes that leave setting attributes to object.
            bool isGeneric = isNative(type->lookup(Special::SetAttr), objectSetAttr);
            raiseAttributeError(globalObject, scope, object, nameIsKey ? key : JSValue(name), makeString('\'', type->nameInMessages(), "' object has no attribute '"_s, text, '\'', isGeneric ? " and no __dict__ for setting new attributes"_s : ""_s));
        }
        return;
    }
    if (value) {
        if (nameIsKey || !dict->hasOnlyStringKeys()) [[unlikely]]
            RELEASE_AND_RETURN(scope, dict->set(globalObject, nameIsKey ? key : JSValue(name), value));
        dict->set(vm, name, value);
        return;
    }
    bool removed;
    if (!nameIsKey && dict->hasOnlyStringKeys()) [[likely]]
        removed = dict->remove(vm, name);
    else {
        removed = dict->remove(globalObject, nameIsKey ? key : JSValue(name));
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (!removed)
        raiseNoAttribute(globalObject, scope, object, name, key);
}

void genericSetAttr(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue value)
{
    genericSetAttrImpl<false>(globalObject, object, name, { }, value);
}

NEVER_INLINE void genericSetAttrByKey(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue key, JSValue value)
{
    genericSetAttrImpl<true>(globalObject, object, name, key, value);
}

// An AttributeError that comes out of the lookup of an attribute without saying which (`raise AttributeError` in a __getattr__ or
// a property) is told, as in CPython: what may have been meant instead is worked out from that.
void completeAttributeError(JSGlobalObject* globalObject, JSValue thrown, JSValue object, JSValue name)
{
    Runtime& rt = runtime(globalObject);
    PyException* exception = exceptionOfThrownValueIfKnown(globalObject, thrown);
    if (!exception || !exception->type()->isSubtypeOf(rt.AttributeError()))
        return;
    if (exception->field(ExceptionField::attributeErrorName) || exception->field(ExceptionField::attributeErrorObj))
        return;
    exception->setField(rt.vm(), ExceptionField::attributeErrorName, name);
    exception->setField(rt.vm(), ExceptionField::attributeErrorObj, object);
}

static NEVER_INLINE void completeAttributeError(JSGlobalObject* globalObject, JSValue object, JSValue name)
{
    if (Exception* thrown = getVM(globalObject).exceptionForInspection())
        completeAttributeError(globalObject, thrown->value(), object, name);
}

static JSValue callGetAttrHook(JSGlobalObject* globalObject, JSValue hook, JSValue object, JSValue name, bool suppress)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    JSValue result = callSpecial(globalObject, hook, object, name);
    if (failed(scope, result)) [[unlikely]] {
        if (suppress)
            clearPendingExceptionIfMatches(globalObject, runtime(globalObject).AttributeError());
        else
            completeAttributeError(globalObject, object, name);
    }
    return result;
}

JSValue getAttrOfAbsent(JSGlobalObject* globalObject, JSValue object, JSString* name)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    JSValue hook = typeOf(globalObject, object)->lookup(Special::GetAttr);
    if (!hook)
        return raiseNoAttribute(globalObject, scope, object, name);
    RELEASE_AND_RETURN(scope, callGetAttrHook(globalObject, hook, object, name, false));
}

static JSValue getAttrImpl(JSGlobalObject* globalObject, JSValue object, JSString* name, bool suppress)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(!name->isRope());

    PyType* type = rt.typeOf(object);
    JSValue getAttribute = type->lookup(Special::GetAttribute);
    JSValue fallback = type->lookup(Special::GetAttr);

    JSValue result;
    if (isNative(getAttribute, objectGetAttribute)) [[likely]]
        result = genericGetAttr(globalObject, object, name, suppress || fallback);
    else if (isNative(getAttribute, typeGetAttribute) && PyType::is(object))
        result = typeGetAttr(globalObject, PyType::from(object), name, suppress || fallback);
    else if (isNative(getAttribute, moduleGetAttribute) && PyModule::is(object))
        result = moduleGetAttr(globalObject, PyModule::from(object), name, suppress || fallback);
    else if (!getAttribute) [[unlikely]] {
        // `object` is not in the MRO. If there is a __getattr__, it comes after the usual search, as ever. Else nothing is found.
        if (fallback)
            result = genericGetAttr(globalObject, object, name, true);
        else if (!suppress)
            return raiseNoAttribute(globalObject, scope, object, name);
    } else
        result = callSpecial(globalObject, getAttribute, object, name);
    EXCEPTION_ASSERT(!scope.exception() || !result);
    if (result) [[likely]]
        return result;

    if (scope.exception()) {
        if (!suppress && !fallback) {
            completeAttributeError(globalObject, object, name);
            return { };
        }
        if (!clearPendingExceptionIfMatches(globalObject, rt.AttributeError()))
            return { };
    }
    if (!fallback)
        return { };
    RELEASE_AND_RETURN(scope, callGetAttrHook(globalObject, fallback, object, name, suppress));
}

JSValue getAttr(JSGlobalObject* globalObject, JSValue object, JSString* name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_AND_RETURN(scope, getAttrImpl(globalObject, object, name, false));
}

JSValue getAttrIfPresent(JSGlobalObject* globalObject, JSValue object, JSString* name)
{
    return getAttrImpl(globalObject, object, name, true);
}

bool hasAttr(JSGlobalObject* globalObject, JSValue object, JSString* name)
{
    return !!getAttrImpl(globalObject, object, name, true);
}

// The class has no __setattr__, or no __delattr__: `object` is not in its MRO.
NEVER_INLINE void raiseCannotSetAttr(JSGlobalObject* globalObject, JSValue object, JSValue name, bool isDeletion)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    PyType* type = typeOf(globalObject, object);
    // The two are one to CPython: a class that has either is asked, and then lacks the other.
    if (type->lookup(isDeletion ? Special::SetAttr : Special::DelAttr)) {
        raise(globalObject, scope, runtime(globalObject).AttributeError(), isDeletion ? "__delattr__"_s : "__setattr__"_s);
        return;
    }
    bool canBeRead = type->lookup(Special::GetAttribute) || type->lookup(Special::GetAttr);
    raiseTypeError(globalObject, scope, '\'', type->nameInMessages(), canBeRead ? "' object has only read-only attributes ("_s : "' object has no attributes ("_s, isDeletion ? "del ."_s : "assign to ."_s, stringOf(strValue(name)), ')');
}

void setAttr(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue value)
{
    ASSERT(value);
    JSValue method = typeOf(globalObject, object)->lookup(Special::SetAttr);
    if (isNative(method, objectSetAttr)) [[likely]]
        return genericSetAttr(globalObject, object, name, value);
    if (!method) [[unlikely]]
        return raiseCannotSetAttr(globalObject, object, name, false);
    callSpecial(globalObject, method, object, name, value);
}

void delAttr(JSGlobalObject* globalObject, JSValue object, JSString* name)
{
    JSValue method = typeOf(globalObject, object)->lookup(Special::DelAttr);
    if (isNative(method, objectDelAttr)) [[likely]]
        return genericSetAttr(globalObject, object, name, { });
    if (!method) [[unlikely]]
        return raiseCannotSetAttr(globalObject, object, name, true);
    callSpecial(globalObject, method, object, name);
}

// getattr(object, key), hasattr(object, key). __getattribute__ and __getattr__ get the key itself.
NEVER_INLINE JSValue getAttrByKey(JSGlobalObject* globalObject, JSValue object, JSValue key, bool suppress)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    PyType* type = rt.typeOf(object);
    JSValue getAttribute = type->lookup(Special::GetAttribute);
    JSValue fallback = type->lookup(Special::GetAttr);
    JSValue result;
    if (getAttribute) [[likely]]
        result = callSpecial(globalObject, getAttribute, object, key);
    else if (fallback) // As in getAttrImpl().
        result = genericGetAttrByKey(globalObject, object, strValue(key), key, false);
    else
        result = raiseNoAttribute(globalObject, scope, object, strValue(key), key);
    EXCEPTION_ASSERT(!!scope.exception() == !result);
    if (result) [[likely]]
        return result;
    if (!suppress && !fallback) {
        completeAttributeError(globalObject, object, key);
        return { };
    }
    if (!clearPendingExceptionIfMatches(globalObject, rt.AttributeError()) || !fallback)
        return { };
    RELEASE_AND_RETURN(scope, callGetAttrHook(globalObject, fallback, object, key, suppress));
}

NEVER_INLINE void setAttrByKey(JSGlobalObject* globalObject, JSValue object, JSValue key, JSValue value)
{
    JSValue method = typeOf(globalObject, object)->lookup(value ? Special::SetAttr : Special::DelAttr);
    if (!method) [[unlikely]]
        return raiseCannotSetAttr(globalObject, object, key, !value);
    if (value)
        callSpecial(globalObject, method, object, key, value);
    else
        callSpecial(globalObject, method, object, key);
}

JSValue getMethod(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue& self)
{
    Runtime& rt = runtime(globalObject);
    self = JSValue();
    PyType* type = rt.typeOf(object);
    if (!isNative(type->lookup(Special::GetAttribute), objectGetAttribute) || type->lookup(Special::GetAttr))
        return getAttr(globalObject, object, name);

    JSValue descriptor = type->lookup(name);
    if (!descriptor || !isMethodDescriptor(descriptor))
        return genericGetAttr(globalObject, object, name);
    if (JSValue value = instanceAttribute(globalObject, object, name))
        return value;
    if (rt.vm().exceptionForInspection()) [[unlikely]]
        return { };
    self = object;
    return descriptor;
}

// ---- What the instructions remember ----

// The key of the property of an instance that holds the attribute, for a name that is not a rope. Null: no property does.
static UniquedStringImpl* propertyKeyOfAttribute(JSString* name)
{
    StringImpl* impl = name->tryGetValueImpl();
    if (!impl->isAtom())
        return nullptr;
    auto* key = static_cast<UniquedStringImpl*>(impl);
    return parseIndex(*key) ? nullptr : key;
}

static bool isDataDescriptor(Runtime& rt, JSValue descriptor)
{
    PyType* type = descriptor ? typeOfPossibleDescriptor(rt, descriptor) : nullptr;
    return type && (type->lookup(Special::Set) || type->lookup(Special::Delete));
}

// What was found in `type` stays there while that has its version. But what it does as a descriptor is up to its own class, which
// may be given a __get__ or a __set__ at any time, unless it is a built-in one that cannot be changed. If `type` is derived from that class, as an enum is
// from the class of its members, its version changes then; and objectSetClass() sees to it that it does when such a value is given
// another class.
static bool mayBecomeAnotherKindOfDescriptor(Runtime& rt, JSValue descriptor, PyType* type)
{
    if (!descriptor || !descriptor.isCell() || descriptor.asCell()->type() == JSFunctionType || PyNativeFunction::is(descriptor.asCell()))
        return false;
    PyType* descriptorType = rt.typeOf(descriptor);
    // (A module may be given another class, whatever its class is.)
    if (!descriptorType->isHeapType() && !descriptorType->has(TypeFlag::Mutable) && !PyModule::is(descriptor))
        return false;
    if (!type->isSubtypeOf(descriptorType))
        return true;
    descriptorType->setHasInstancesAmongAttributes();
    return false;
}

enum class MethodWrapper : uint8_t { None, StaticMethod, ClassMethod };
static MethodWrapper methodWrapperKind(Runtime& rt, JSValue value)
{
    if (!value.isCell() || value.asCell()->type() != PyObjectType)
        return MethodWrapper::None;
    PyType* type = uncheckedDowncast<PyObject>(value.asCell())->type();
    return type == rt.staticmethodType() ? MethodWrapper::StaticMethod : type == rt.classmethodType() ? MethodWrapper::ClassMethod : MethodWrapper::None;
}

static bool describeAttributeOfClass(Runtime& rt, PyType* cls, JSString* name, bool forCall, AttributeCache& cache)
{
    // As in typeGetAttr().
    PyType* metatype = cls->type();
    if (!isNative(metatype->lookup(Special::GetAttribute), typeGetAttribute))
        return false;
    if (JSValue metaAttribute = metatype->lookup(name)) {
        PyType* attributeType = rt.typeOf(metaAttribute);
        if (attributeType->lookup(Special::Set) || attributeType->lookup(Special::Delete) || mayBecomeAnotherKindOfDescriptor(rt, metaAttribute, metatype))
            return false;
    }
    JSValue attribute = cls->lookup(name);
    if (!attribute || mayBecomeAnotherKindOfDescriptor(rt, attribute, cls))
        return false;
    cache.structureID = cls->structureID();
    cache.payload = JSValue::encode(attribute);
    cache.offset = AttributeCache::valueOfClass;

    // As in bindDescriptor(), without an object.
    if (!attribute.isCell() || attribute.asCell()->type() == JSFunctionType)
        return true;
    if (auto* native = PyNativeFunction::tryFrom(attribute))
        return native->kind() != NativeKind::ClassMethod;
    switch (methodWrapperKind(rt, attribute)) {
    case MethodWrapper::StaticMethod:
        cache.offset = AttributeCache::staticMethodOfClass;
        return true;
    case MethodWrapper::ClassMethod:
        cache.offset = AttributeCache::classMethodOfClass;
        return forCall; // Otherwise the attribute is a new bound method each time.
    case MethodWrapper::None:
        break;
    }
    return !rt.typeOf(attribute)->lookup(Special::Get);
}

// All but the versions. `type`: whose version is to be remembered, if any.
static AttributeDescription describeAttributeOfObject(JSGlobalObject* globalObject, JSCell* cell, JSString* name, bool forCall, AttributeCache& cache, PyType*& type, uint64_t timesLookupRanCode)
{
    using enum AttributeDescription;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSValue object = cell;
    JSType jsType = cell->type();

    // As in moduleGetAttr().
    if (jsType == PyModuleType) {
        auto* module = uncheckedDowncast<PyModule>(cell);
        if (module->type() != rt.moduleType() || rt.moduleType()->lookup(name) || !module->dict()->hasOnlyStringKeys())
            return None;
        StringImpl* impl = name->tryGetValueImpl();
        if (impl == rt.names.dSpec()->tryGetValueImpl() || impl == rt.names.dLoader()->tryGetValueImpl())
            return None;
        int32_t index = module->dict()->indexOf(name);
        if (index < 0)
            return None;
        cache.structureID = cell->structureID();
        cache.offset = AttributeCache::moduleEntry;
        cache.typeVersion = index;
        cache.payload = module->dict()->watchedVersion();
        return OfAllAlike;
    }

    // As in genericGetAttr() and getMethod().
    bool isPython = isPyObjectType(jsType);
    if (!isPython && jsType != StringType && jsType != ArrayType)
        return None;
    PyType* typeOfObject = rt.typeOf(object);
    if (!isNative(typeOfObject->lookup(Special::GetAttribute), objectGetAttribute))
        return None;
    JSValue descriptor = typeOfObject->lookup(name);
    // From here on: whether what is found out holds for every object with the Structure, while the class has its version.
    AttributeDescription description = mayBecomeAnotherKindOfDescriptor(rt, descriptor, typeOfObject) ? OfThisObjectNow : OfAllAlike;

    if (!isPython) {
        if (!forCall || !descriptor || !isMethodDescriptor(descriptor))
            return None;
        cache.jsType = jsType;
        cache.payload = JSValue::encode(descriptor);
        return description;
    }

    type = typeOfObject;
    bool hasHook = !!type->lookup(Special::GetAttr);
    // Where the class has a __getattr__ a method is bound before the arguments are evaluated: getMethod(). One of a built-in type may
    // refuse that, and goes by another name once it is bound.
    if (forCall && hasHook && PyNativeFunction::is(descriptor))
        return None;
    bool isData = isDataDescriptor(rt, descriptor);
    // The Structure is looked at when the lookups are over. Code that one of them ran may have given the object another, and a
    // collection may have freed this one.
    if (PyType::timesLookupRanCode() != timesLookupRanCode) [[unlikely]]
        return None;
    Structure* structure = cell->structure();
    cache.structureID = cell->structureID();
    if (structure->isDictionary())
        description = OfThisObjectNow;

    // One of the __slots__.
    if (descriptor && descriptor.isCell() && descriptor.asCell()->type() == PyObjectType && descriptor.asCell()->classInfo() == PyMemberDescriptor::info()) {
        auto* member = uncheckedDowncast<PyMemberDescriptor>(descriptor.asCell());
        if (type != member->owner() && !type->isSubtypeOf(member->owner()))
            return None;
        cache.offset = structure->get(vm, PropertyName(&member->key()->uid()));
        return isValidOffset(cache.offset) ? description : None;
    }
    // A field of a named tuple.
    if (jsType == PyTupleType && !forCall) {
        if (int64_t index = indexOfTupleGetter(globalObject, descriptor); index >= 0) {
            cache.offset = AttributeCache::tupleItem;
            cache.payload = index;
            return description;
        }
    }
    // A property. If its getter raises AttributeError, __getattr__ is asked: that is for C++.
    if (!forCall && descriptor && descriptor.isCell() && descriptor.asCell()->type() == PyObjectType && rt.typeOf(descriptor) == rt.propertyType() && !hasHook) {
        cache.offset = AttributeCache::propertyGetter;
        cache.payload = JSValue::encode(descriptor);
        return description;
    }
    if (isData)
        return None;

    // What the object holds itself. Its Structure tells, if the attributes are properties: those of an instance that has not been asked
    // for its __dict__.
    PyDict* dict = nullptr;
    if (jsType == PyInstanceType) {
        dict = uncheckedDowncast<PyInstance>(cell)->dict();
        if (!dict) {
            UniquedStringImpl* key = propertyKeyOfAttribute(name);
            if (!key)
                return None;
            cache.offset = structure->get(vm, PropertyName(key));
            // No other object of the Structure has a dict either: PyInstance::ensureDict().
            if (isValidOffset(cache.offset))
                return description;
            if (isValidOffset(structure->get(vm, rt.instanceHasDictKey())))
                description = OfThisObjectNow;
        }
    } else if (type->has(TypeFlag::HasDict)) {
        bool isWrapper = cell->inherits<PyWrapperObject>();
        if (isWrapper && uncheckedDowncast<PyWrapperObject>(cell)->copiesAttributesLater())
            return None;
        dict = instanceDict(globalObject, object, false);
        // The others have their __dict__, once they have one, as a property: see instanceDict().
        if (jsType == PyExceptionType || isWrapper)
            description = OfThisObjectNow;
    }
    if (dict) {
        if (!dict->hasOnlyStringKeys())
            return None;
        description = OfThisObjectNow;
        if (JSValue value = dict->get(name)) {
            cache.payload = JSValue::encode(value);
            cache.offset = forCall ? AttributeCache::unboundClassValue : AttributeCache::classValue;
            return description;
        }
    }

    if (!descriptor)
        return Absent;
    cache.payload = JSValue::encode(descriptor);
    if (isMethodDescriptor(descriptor)) {
        cache.offset = AttributeCache::classValue;
        return forCall ? description : None; // Otherwise the attribute is a new bound method each time.
    }
    switch (methodWrapperKind(rt, descriptor)) {
    case MethodWrapper::StaticMethod:
        cache.offset = AttributeCache::staticMethod;
        return description;
    case MethodWrapper::ClassMethod:
        cache.offset = AttributeCache::classMethod;
        return forCall ? description : None;
    case MethodWrapper::None:
        break;
    }
    // What is left is the attribute as it is, if there is nothing to bind.
    if (descriptor.isCell()) {
        if (auto* native = PyNativeFunction::tryFrom(descriptor); native && native->kind() == NativeKind::ClassMethod)
            return None;
        if (rt.typeOf(descriptor)->lookup(Special::Get))
            return None;
    }
    cache.offset = forCall ? AttributeCache::unboundClassValue : AttributeCache::classValue;
    return description;
}

AttributeDescription describeAttribute(JSGlobalObject* globalObject, JSValue object, JSString* name, bool forCall, AttributeCache& cache)
{
    using enum AttributeDescription;
    if (!object.isCell())
        return None;
    Runtime& rt = runtime(globalObject);
    uint64_t timesLookupRanCode = PyType::timesLookupRanCode();
    JSCell* cell = object.asCell();
    PyType* type = nullptr;
    PyType* cls = PyType::is(cell) ? uncheckedDowncast<PyType>(cell) : nullptr;
    AttributeDescription description;
    if (cls)
        description = describeAttributeOfClass(rt, cls, name, forCall, cache) ? OfAllAlike : None;
    else
        description = describeAttributeOfObject(globalObject, cell, name, forCall, cache, type, timesLookupRanCode);
    // Then anything may have become of the class, the object, and the frame that holds it.
    if (PyType::timesLookupRanCode() != timesLookupRanCode) [[unlikely]]
        return None;
    if (description != OfAllAlike)
        return description;
    // A type is asked for its version last: it has only so many to give.
    if (cls) {
        cache.typeVersion = cls->version();
        cache.metatypeVersion = cls->type()->version();
        return cache.typeVersion && cache.metatypeVersion ? OfAllAlike : OfThisObjectNow;
    }
    if (!type)
        return description;
    cache.typeVersion = type->version();
    return cache.typeVersion ? OfAllAlike : OfThisObjectNow;
}

bool describeSpecialMethod(JSGlobalObject* globalObject, JSValue object, Special special, AttributeCache& cache)
{
    if (!object.isCell() || !isPyObjectType(object.asCell()->type()))
        return false;
    Runtime& rt = runtime(globalObject);
    PyType* type = rt.typeOf(object);
    uint64_t timesLookupRanCode = PyType::timesLookupRanCode();
    JSValue descriptor = type->lookup(special);
    if (!descriptor || !isMethodDescriptor(descriptor) || PyType::timesLookupRanCode() != timesLookupRanCode)
        return false;
    cache.structureID = object.asCell()->structureID();
    cache.typeVersion = type->version();
    cache.payload = JSValue::encode(descriptor);
    cache.offset = AttributeCache::classValue;
    return !!cache.typeVersion;
}

bool describeSetAttribute(JSGlobalObject* globalObject, JSValue object, JSString* name, Structure* oldStructure, SetAttributeCache& cache)
{
    // As in genericSetAttr().
    if (!object.isCell() || !isPyObjectType(object.asCell()->type()))
        return false;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* cell = uncheckedDowncast<PyObject>(object.asCell());
    PyType* type = cell->type();
    uint64_t timesLookupRanCode = PyType::timesLookupRanCode();
    if (!isNative(type->lookup(Special::SetAttr), objectSetAttr))
        return false;
    JSValue descriptor = type->lookup(name);
    if (mayBecomeAnotherKindOfDescriptor(rt, descriptor, type))
        return false;
    bool isSlot = descriptor && descriptor.isCell() && descriptor.asCell()->type() == PyObjectType && descriptor.asCell()->classInfo() == PyMemberDescriptor::info();
    if (!isSlot && isDataDescriptor(rt, descriptor))
        return false;
    // As in describeAttributeOfObject(): the Structure is looked at when the lookups are over.
    if (PyType::timesLookupRanCode() != timesLookupRanCode) [[unlikely]]
        return false;

    Structure* structure = cell->structure();
    if (structure->isDictionary() || oldStructure->isDictionary())
        return false;
    UniquedStringImpl* key;
    if (isSlot)
        key = &uncheckedDowncast<PyMemberDescriptor>(descriptor.asCell())->key()->uid();
    else {
        if (cell->JSCell::type() != PyInstanceType || !type->has(TypeFlag::HasDict))
            return false;
        // Whether the attributes are properties, so that the Structure tells which there are.
        if (uncheckedDowncast<PyInstance>(cell)->dict() || isValidOffset(structure->get(vm, rt.instanceHasDictKey())))
            return false;
        key = propertyKeyOfAttribute(name);
        if (!key)
            return false;
    }
    cache.offset = structure->get(vm, PropertyName(key));
    if (!isValidOffset(cache.offset))
        return false;
    cache.oldStructureID = oldStructure->id();
    if (structure != oldStructure) {
        // The instruction does not allocate.
        if (structure->previousID() != oldStructure || structure->outOfLineCapacity() != oldStructure->outOfLineCapacity())
            return false;
        cache.newStructureID = structure->id();
    }
    cache.typeVersion = type->version();
    return !!cache.typeVersion;
}

JSValue lookupSpecial(JSGlobalObject* globalObject, JSValue object, Special special)
{
    PyType* type = typeOf(globalObject, object);
    JSValue method = type->lookup(special);
    if (!method)
        return { };
    return bindDescriptor(globalObject, method, object, type);
}

JSValue getSpecialMethod(JSGlobalObject* globalObject, JSValue object, Special special, JSValue& self)
{
    PyType* type = typeOf(globalObject, object);
    JSValue method = type->lookup(special);
    if (!method)
        return { };
    if (isMethodDescriptor(method)) {
        self = object;
        return method;
    }
    return bindDescriptor(globalObject, method, object, type);
}

// ---- Calls ----

#if (CPU(ARM64) || CPU(X86_64)) && CPU(ADDRESS64) && !ENABLE(C_LOOP)
extern "C" void llint_python_generator_resume(); // LowLevelInterpreter64.asm
#endif

// What Interpreter::executeCall() comes to in the common case. It guards the code it is about to enter against being replaced
// or thrown away, which does not happen to code that stays in the interpreter. False: this is not the common case.
template<bool isResumption = false>
static ALWAYS_INLINE bool tryEnterFunction(VM& vm, PyFunction* function, JSValue thisValue, const JSValue* arguments, size_t count, JSValue& result)
{
#if (CPU(ARM64) || CPU(X86_64)) && CPU(ADDRESS64) && !ENABLE(C_LOOP)
    FunctionExecutable* executable = function->functionExecutable();
    CodeBlock* codeBlock = executable->codeBlockForCall();
    // These entry points make a frame for the arguments there are, which has to hold all the parameters.
    if (!codeBlock || count > 6 || count + 1 < codeBlock->numParameters() || !vm.entryScope || vm.disallowVMEntryCount || !vm.isSafeToRecurseSoft()) [[unlikely]]
        return false;
    void* entry = executable->generatedJITCodeAddressForCall();
#if !CPU(ARM64E)
    if constexpr (isResumption)
        entry = reinterpret_cast<void*>(llint_python_generator_resume);
#endif
    vm.clearLastException();
    EncodedJSValue encoded;
    switch (count) {
    case 0:
        encoded = vmEntryToJavaScriptWith0Arguments(entry, &vm, codeBlock, function, thisValue, nullptr);
        break;
    case 1:
        encoded = vmEntryToJavaScriptWith1Arguments(entry, &vm, codeBlock, function, thisValue, nullptr, arguments[0]);
        break;
    case 2:
        encoded = vmEntryToJavaScriptWith2Arguments(entry, &vm, codeBlock, function, thisValue, nullptr, arguments[0], arguments[1]);
        break;
    case 3:
        encoded = vmEntryToJavaScriptWith3Arguments(entry, &vm, codeBlock, function, thisValue, nullptr, arguments[0], arguments[1], arguments[2]);
        break;
    case 4:
        encoded = vmEntryToJavaScriptWith4Arguments(entry, &vm, codeBlock, function, thisValue, nullptr, arguments[0], arguments[1], arguments[2], arguments[3]);
        break;
    case 5:
        encoded = vmEntryToJavaScriptWith5Arguments(entry, &vm, codeBlock, function, thisValue, nullptr, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
        break;
    default:
        encoded = vmEntryToJavaScriptWith6Arguments(entry, &vm, codeBlock, function, thisValue, nullptr, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5]);
        break;
    }
    vm.didEnterVM = true;
    result = JSValue::decode(encoded);
    return true;
#else
    UNUSED_PARAM(vm);
    UNUSED_PARAM(function);
    UNUSED_PARAM(thisValue);
    UNUSED_PARAM(arguments);
    UNUSED_PARAM(count);
    UNUSED_PARAM(result);
    return false;
#endif
}

// The frame of this function is on the stack for as long as the function it calls runs, and the collector takes every word of it
// that looks like a reference for one. What such a frame has room for and does not write still holds what earlier calls left
// there: `del x; gc.collect()` would not free what x was. So this is a function of its own, with room for little, and not a part
// of call(), which needs buffers for its other cases; and call() is gone from the stack when this runs.
static NEVER_INLINE JSValue callJSFunction(JSGlobalObject* globalObject, JSObject* function, const CallArgs& args)
{
    JSValue thisValue = args.hasKeywords() ? JSValue(args.keywordNames()) : jsUndefined();
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    Runtime::RuntimeCall record { rt.runtimeCall };
    rt.runtimeCall = &record;
    JSValue result;
    if (!tryEnterFunction(rt.vm(), uncheckedDowncast<PyFunction>(function), thisValue, args.values(), args.totalCount(), result)) [[unlikely]] {
        CallData callData = JSC::getCallData(function);
        ASSERT(callData.type != CallData::Type::None);
        ArgList list(reinterpret_cast<EncodedJSValue*>(const_cast<JSValue*>(args.values())), args.totalCount());
        result = JSC::call(globalObject, function, callData, thisValue, list);
    }
    rt.runtimeCall = record.previous;
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

JSValue callBodyOfGenerator(JSGlobalObject* globalObject, PyFunction* body, std::span<const JSValue> arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result;
    if (!tryEnterFunction<true>(vm, body, jsUndefined(), arguments.data(), arguments.size(), result)) [[unlikely]] {
        if (!vm.isSafeToRecurseSoft()) [[unlikely]]
            return raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded"_s);
        ArgList list(reinterpret_cast<EncodedJSValue*>(const_cast<JSValue*>(arguments.data())), arguments.size());
        result = JSC::call(globalObject, body, JSC::getCallData(body), jsUndefined(), list);
    }
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

JSValue call(JSGlobalObject* globalObject, JSValue callable, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!vm.isSafeToRecurseSoft()) [[unlikely]]
        return raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded"_s);

    if (callable.isCell()) {
        JSCell* cell = callable.asCell();
        switch (cell->type()) {
        case JSFunctionType:
            if (!PyFunction::is(cell))
                RELEASE_AND_RETURN(scope, callForeignFunction(globalObject, cell, args));
            RELEASE_AND_RETURN(scope, callJSFunction(globalObject, asObject(cell), args));
        case InternalFunctionType: {
            auto native = uncheckedDowncast<InternalFunction>(cell)->nativeFunctionFor(CodeSpecializationKind::CodeForCall);
            if (native == TaggedNativeFunction(callNativeFunction))
                RELEASE_AND_RETURN(scope, uncheckedDowncast<PyNativeFunction>(cell)->invoke(globalObject, args));
            if (native == TaggedNativeFunction(callType)) {
                auto* type = uncheckedDowncast<PyType>(cell);
                if (type->type() == rt.typeType()) [[likely]]
                    RELEASE_AND_RETURN(scope, typeCall(globalObject, type, args));
                break;
            }
            RELEASE_AND_RETURN(scope, callForeignFunction(globalObject, cell, args));
        }
        case PyMethodType: {
            auto* method = uncheckedDowncast<PyMethod>(cell);
            if (JSValue function = method->function(); function.isCell() && function.asCell()->type() == JSFunctionType && !PyFunction::is(function.asCell())) [[unlikely]]
                RELEASE_AND_RETURN(scope, callForeignMethod(globalObject, function, method->self(), args));
            RELEASE_AND_RETURN(scope, withFirstArgument(method->self(), args, [&](const CallArgs& all) {
                return call(globalObject, method->function(), all);
            }));
        }
        default:
            break;
        }
    }

    PyType* type = rt.typeOf(callable);
    JSValue method = type->lookup(Special::Call);
    if (!method) {
        if (callable.isObject() && !isPyObject(callable.asCell()) && asObject(callable)->isCallable())
            RELEASE_AND_RETURN(scope, callForeignFunction(globalObject, callable, args));
        scope.release();
        throwNotCallable(globalObject, callable);
        return { };
    }
    RELEASE_AND_RETURN(scope, callSpecial(globalObject, method, callable, args));
}

JSValue callSpecial(JSGlobalObject* globalObject, JSValue method, JSValue self, const CallArgs& args)
{
    // Whoever takes a method of `object` for granted finds none in a class whose metaclass has given it an MRO without `object`. What
    // CPython has an answer of its own for looks before it calls (getAttrImpl(), setAttr(), reprObject(), ...): this is for the rest.
    if (!method) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
        return raiseTypeError(globalObject, scope, "type '"_s, typeName(globalObject, self), "' has no such method: 'object' is not in its MRO"_s);
    }
    if (method.isCell()) {
        JSCell* cell = method.asCell();
        if (cell->type() == InternalFunctionType) {
            if (auto* native = PyNativeFunction::tryFrom(method)) {
                NativeKind kind = native->kind();
                if (kind == NativeKind::SlotWrapper || kind == NativeKind::Method) [[likely]] {
                    if (nativeMethodAppliesTo(runtime(globalObject), native, self)) [[likely]]
                        return native->invokeWithSelf(globalObject, self, args);
                    // Which refuses.
                    return withFirstArgument(self, args, [&](const CallArgs& all) {
                        return native->invoke(globalObject, all);
                    });
                }
            }
        } else if (cell->type() == JSFunctionType && PyFunction::is(cell)) {
            return withFirstArgument(self, args, [&](const CallArgs& all) {
                return callJSFunction(globalObject, asObject(cell), all);
            });
        }
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue bound = bindDescriptor(globalObject, method, self, typeOf(globalObject, self));
    RETURN_IF_EXCEPTION(scope, { });
    // After A.__call__ = A(), to call an A is to call an A. That has to end, although it may use no stack: these are tail calls.
    Runtime& rt = runtime(globalObject);
    if (failed(scope, rt.tryEnterFrame())) [[unlikely]]
        return { };
    JSValue result = call(globalObject, bound, args);
    rt.leaveFrame();
    scope.release();
    return result;
}

JSValue callOperatorMethod(JSGlobalObject* globalObject, JSValue method, JSValue self, const CallArgs& args)
{
    if (isMethodDescriptor(method)) [[likely]]
        return callSpecial(globalObject, method, self, args);
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    JSValue bound = bindDescriptor(globalObject, method, self, rt.typeOf(self));
    if (failed(scope, bound)) [[unlikely]]
        RELEASE_AND_RETURN(scope, clearPendingExceptionIfMatches(globalObject, rt.AttributeError()) ? rt.notImplemented() : JSValue());
    if (failed(scope, rt.tryEnterFrame())) [[unlikely]]
        return { };
    JSValue result = call(globalObject, bound, args);
    rt.leaveFrame();
    scope.release();
    return result;
}

// For the special methods that an object can do without. What the type has under the name may be any descriptor, and an
// AttributeError from its __get__ says that there is no such method after all: then `method` is emptied and nothing is raised.
template<typename... Arguments>
static JSValue callSpecialIfThere(JSGlobalObject* globalObject, JSValue& method, JSValue self, Arguments... arguments)
{
    if (isMethodDescriptor(method)) [[likely]]
        return callSpecial(globalObject, method, self, arguments...);
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    JSValue bound = bindDescriptor(globalObject, method, self, rt.typeOf(self));
    if (failed(scope, bound)) {
        if (clearPendingExceptionIfMatches(globalObject, rt.AttributeError()))
            method = { };
        return { };
    }
    RELEASE_AND_RETURN(scope, call(globalObject, bound, arguments...));
}

template<typename... Arguments>
static ALWAYS_INLINE JSValue callSpecialIfThere(JSGlobalObject* globalObject, PyType* type, Special special, JSValue& method, JSValue self, Arguments... arguments)
{
    if (canCallNativeSpecial<Arguments...>(globalObject, type, special, method)) [[likely]]
        return callNativeSpecial(globalObject, method, self, arguments...);
    return callSpecialIfThere(globalObject, method, self, arguments...);
}

JSValue callWithTupleAndDict(JSGlobalObject* globalObject, JSValue callable, PyTuple* positional, PyDict* keywords)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!keywords || keywords->isEmpty())
        RELEASE_AND_RETURN(scope, call(globalObject, callable, CallArgs(positional->span())));

    MarkedArgumentBuffer buffer;
    for (JSValue value : positional->span())
        buffer.append(value);
    PyTuple* names = PyTuple::createUninitialized(vm, rt.keywordNamesStructure(), rt.tupleType(), keywords->size());
    unsigned index = 0;
    JSValue key, value;
    for (unsigned position = 0; keywords->next(position, key, value);) {
        if (!isStr(key))
            return raiseTypeError(globalObject, scope, "keywords must be strings"_s);
        names->initializeAt(vm, index++, strValue(key));
        buffer.append(value);
    }
    RELEASE_AND_RETURN(scope, call(globalObject, callable, CallArgs(reinterpret_cast<const JSValue*>(ArgList(buffer).data()), positional->length(), names)));
}

bool isCallable(JSGlobalObject* globalObject, JSValue value)
{
    if (!value.isCell())
        return false;
    JSCell* cell = value.asCell();
    switch (cell->type()) {
    case JSFunctionType:
    case InternalFunctionType:
    case PyMethodType:
        return true;
    default:
        break;
    }
    if (typeOf(globalObject, value)->lookup(Special::Call))
        return true;
    return cell->isObject() && !isPyObject(cell) && asObject(cell)->isCallable();
}

// ---- Text ----

static JSValue checkString(JSGlobalObject* globalObject, JSValue value, JSValue result, ASCIILiteral method)
{
    if (!result || isStr(result)) [[likely]]
        return result;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, typeOf(globalObject, value)), '.', method, "() must return a str, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, result)));
}

JSValue reprObject(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!vm.isSafeToRecurseSoft()) [[unlikely]]
        return raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded while getting the repr of an object"_s);
    PyType* type = typeOf(globalObject, value);
    JSValue method = type->lookup(Special::Repr);
    // `object` is not in the MRO.
    if (!method) [[unlikely]]
        RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString('<', type->nameInMessages(), " object at "_s, addressForRepr(globalObject, value), '>')));
    JSValue result = callSpecial(globalObject, type, Special::Repr, method, value);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, checkString(globalObject, value, result, "__repr__"_s));
}

JSString* repr(JSGlobalObject* globalObject, JSValue value)
{
    JSValue result = reprObject(globalObject, value);
    return result ? strValue(result) : nullptr;
}

JSValue strObject(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isString())
        return value;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!vm.isSafeToRecurseSoft()) [[unlikely]]
        return raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded while getting the str of an object"_s);
    PyType* type = typeOf(globalObject, value);
    JSValue method = type->lookup(Special::Str);
    if (!method) [[unlikely]]
        RELEASE_AND_RETURN(scope, reprObject(globalObject, value));
    JSValue result = callSpecial(globalObject, type, Special::Str, method, value);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, checkString(globalObject, value, result, "__str__"_s));
}

JSString* str(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isString())
        return asString(value);
    JSValue result = strObject(globalObject, value);
    return result ? strValue(result) : nullptr;
}

JSString* ascii(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* text = repr(globalObject, value);
    RETURN_IF_EXCEPTION(scope, nullptr);
    auto view = text->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (view->containsOnlyASCII())
        return text;
    RELEASE_AND_RETURN(scope, makeStrUnlessTooLong(globalObject, asciiEscape(view)));
}

JSValue asciiObject(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = reprObject(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    auto view = strValue(result)->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (view->containsOnlyASCII())
        return result;
    RELEASE_AND_RETURN(scope, makeStrUnlessTooLong(globalObject, asciiEscape(view)));
}

JSString* format(JSGlobalObject* globalObject, JSValue value, JSString* spec)
{
    JSValue result = formatObject(globalObject, value, spec);
    return result ? strValue(result) : nullptr;
}

JSValue formatObject(JSGlobalObject* globalObject, JSValue value, JSString* spec)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isEmptySpec = !spec || !spec->length();
    // What most f-strings hold.
    if (isEmptySpec) {
        if (value.isString())
            return asString(value);
        if (isExactInt(value))
            RELEASE_AND_RETURN(scope, str(globalObject, value));
    }
    JSValue method = rt.typeOf(value)->lookup(Special::Format);
    if (!method) [[unlikely]]
        return raiseTypeError(globalObject, scope, "Type "_s, rt.typeOf(value)->nameInMessages(), " doesn't define __format__"_s);
    JSValue result = callSpecial(globalObject, method, value, spec ? spec : rt.names.empty());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isStr(result))
        return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, rt.typeOf(value)), ".__format__() must return a str, not "_s, typeNameForMessage(globalObject, rt.typeOf(result)));
    return result;
}

String reprForMessage(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    // The message may be about an exception that is propagating, which is set aside: nothing runs while one is.
    SuspendExceptionScope suspended(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    String result;
    JSString* text = repr(globalObject, value);
    if (!scope.exception())
        result = text->value(globalObject);
    if (scope.exception()) {
        scope.clearException();
        result = "<?>"_s;
    }
    return result;
}

ReprGuard::ReprGuard(JSGlobalObject* globalObject, JSValue value)
    : m_globalObject(globalObject)
    , m_cell(value.asCell())
{
    auto& stack = runtime(globalObject).reprStack;
    m_isRecursive = stack.contains(m_cell);
    if (!m_isRecursive)
        stack.append(m_cell);
}

ReprGuard::~ReprGuard()
{
    if (m_isRecursive)
        return;
    auto& stack = runtime(m_globalObject).reprStack;
    ASSERT(stack.last() == m_cell);
    stack.removeLast();
}

// ---- Identity, hashing, truth ----

bool isIdentical(JSValue a, JSValue b)
{
    if (a == b)
        return true;
    // The same number may be tagged in two ways, and None is two values.
    if (a.isNumber() && b.isNumber())
        return a.asNumber() == b.asNumber();
    // Cells that share their text are one str: that is what interning amounts to.
    if (a.isString() && b.isString()) {
        const StringImpl* impl = asString(a)->tryGetValueImpl();
        return impl && impl == asString(b)->tryGetValueImpl();
    }
    return a.isUndefinedOrNull() && b.isUndefinedOrNull();
}

int64_t hash(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isString()) {
        JSString* string = asString(value);
        if (string->isRope()) {
            string->value(globalObject);
            RETURN_IF_EXCEPTION(scope, -1);
        }
        return hashOfString(string->tryGetValueImpl());
    }
    if (value.isInt32())
        return value.asInt32() == -1 ? -2 : value.asInt32();
    if (value.isDouble())
        return hashOfDouble(value.asDouble());

    PyType* type = rt.typeOf(value);
    JSValue method = type->lookup(Special::Hash);
    if (canCallNativeSpecial<>(globalObject, type, Special::Hash, method)) [[likely]] {
        if (auto rawHash = uncheckedDowncast<PyNativeFunction>(method.asCell())->def().rawHash) [[likely]]
            RELEASE_AND_RETURN(scope, rawHash(globalObject, value));
    }
    JSValue result;
    if (method && !isNone(method)) {
        result = callSpecialIfThere(globalObject, type, Special::Hash, method, value);
        RETURN_IF_EXCEPTION(scope, -1);
    }
    if (!method || isNone(method)) {
        raiseTypeError(globalObject, scope, "unhashable type: '"_s, type->nameInMessages(), '\'');
        return -1;
    }
    if (!isInt(result)) {
        raiseTypeError(globalObject, scope, "__hash__ method should return an integer"_s);
        return -1;
    }
    result = intValue(result);
    if (auto small = tryInt64(result))
        return *small == -1 ? -2 : *small;
    return hashOfInt(result);
}

int64_t hashForContainer(JSGlobalObject* globalObject, JSValue value, ASCIILiteral what)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t result = hash(globalObject, value);
    if (!scope.exception()) [[likely]]
        return result;
    // (Nothing to look at: a termination.)
    JSValue exception = pendingException(globalObject);
    if (!exception || rt.typeOf(exception) != rt.TypeError())
        return -1;
    takePendingException(globalObject);
    JSString* message = str(globalObject, exception);
    RETURN_IF_EXCEPTION(scope, -1);
    String text = message->value(globalObject);
    RETURN_IF_EXCEPTION(scope, -1);
    raiseTypeError(globalObject, scope, "cannot use '"_s, typeNameForMessage(globalObject, rt.typeOf(value)), "' as "_s, what, " ("_s, text, ')');
    return -1;
}

bool isTrue(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isBoolean())
        return value.asBoolean();
    if (value.isUndefinedOrNull())
        return false;
    if (value.isInt32())
        return !!value.asInt32();
    if (value.isDouble())
        return !!value.asDouble(); // A NaN is true.

    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSCell* cell = value.asCell();
    switch (cell->type()) {
    case StringType:
        return !!uncheckedDowncast<JSString>(cell)->length();
    case ArrayType:
        return !!uncheckedDowncast<JSArray>(cell)->length();
    case HeapBigIntType:
        return !uncheckedDowncast<JSBigInt>(cell)->isZero();
    default:
        break;
    }

    PyType* type = rt.typeOf(value);
    bool hasBool = false;
    if (JSValue method = type->lookup(Special::Bool)) {
        hasBool = true;
        if (isNone(method)) {
            raiseTypeError(globalObject, scope, '\'', type->nameInMessages(), "' cannot be interpreted as a boolean"_s);
            return false;
        }
        JSValue result = callSpecialIfThere(globalObject, type, Special::Bool, method, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (method && !result.isBoolean()) {
            raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__bool__() must return a bool, not "_s, typeNameForMessage(globalObject, rt.typeOf(result)));
            return false;
        }
        if (method)
            return result.asBoolean();
    }
    if (JSValue method = type->lookup(Special::Len)) {
        if (hasBool) [[unlikely]] {
            // The class has a __bool__ that says it is not there. Only then may __len__ say the same, and return what is no length.
            JSValue result = callSpecialIfThere(globalObject, method, value);
            RETURN_IF_EXCEPTION(scope, false);
            if (!method)
                return true;
            RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
        }
        int64_t size = length(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        return !!size;
    }
    return true;
}

// ---- Comparison ----

static constexpr Special comparisonMethods[] = { Special::Eq, Special::Ne, Special::Lt, Special::Le, Special::Gt, Special::Ge };
static constexpr CompareOperator swappedOperators[] = { CompareOperator::Eq, CompareOperator::NotEq, CompareOperator::Gt, CompareOperator::GtE, CompareOperator::Lt, CompareOperator::LtE };
static constexpr ASCIILiteral operatorSymbols[] = { "=="_s, "!="_s, "<"_s, "<="_s, ">"_s, ">="_s };

template<typename T>
static ALWAYS_INLINE bool compareOrdered(T a, T b, CompareOperator op)
{
    switch (op) {
    case CompareOperator::Eq:
        return a == b;
    case CompareOperator::NotEq:
        return a != b;
    case CompareOperator::Lt:
        return a < b;
    case CompareOperator::LtE:
        return a <= b;
    case CompareOperator::Gt:
        return a > b;
    case CompareOperator::GtE:
        return a >= b;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static JSValue richCompareOrNotImplemented(JSGlobalObject*, JSValue, JSValue, CompareOperator);

std::optional<bool> compareForJS(JSGlobalObject*, JSOperator op, JSValue a, JSValue b)
{
    JSGlobalObject* globalObject = realmOfOperands(a, b);
    static constexpr CompareOperator operators[] = { CompareOperator::Lt, CompareOperator::LtE, CompareOperator::Gt, CompareOperator::GtE };
    ASSERT(op >= JSOperator::Less);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = richCompare(globalObject, a, b, operators[static_cast<unsigned>(op) - static_cast<unsigned>(JSOperator::Less)]);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
}

std::optional<bool> equalForJS(JSGlobalObject*, JSValue a, JSValue b)
{
    JSGlobalObject* globalObject = realmOfOperands(a, b);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = richCompareOrNotImplemented(globalObject, a, b, CompareOperator::Eq);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (result == runtime(globalObject).notImplemented())
        return std::nullopt;
    RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
}

// a < b asks up to two methods, until one of them returns something else than NotImplemented. As with a + b there are two who go through
// them, C++ and the interpreter: see askMethods() in PyOperators.cpp, and ComparisonProgress in PyBytecode.h.

// The result, or NotImplemented, or, if the caller wants it so, nothing and a function for it to call.
static ALWAYS_INLINE JSValue askComparisonMethods(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op, unsigned progress, MethodsWrittenInPython methods)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue notImplemented = rt.notImplemented();
    Special special = comparisonMethods[static_cast<unsigned>(op)];
    Special reflected = comparisonMethods[static_cast<unsigned>(swappedOperators[static_cast<unsigned>(op)])];

    while (progress != ComparisonProgress::Everything) {
        // A method that has run may have given an operand a new class.
        PyType* typeA = rt.typeOf(a);
        PyType* typeB = rt.typeOf(b);
        JSValue method;
        bool isReflected = false;
        switch (progress) {
        case ComparisonProgress::Nothing:
            if (typeA != typeB && typeB->isSubtypeOf(typeA)) {
                method = typeB->lookup(reflected);
                if (method) {
                    isReflected = true;
                    progress = ComparisonProgress::ReflectedFirst;
                    break;
                }
            }
            method = typeA->lookup(special);
            progress = ComparisonProgress::Left;
            break;
        case ComparisonProgress::ReflectedFirst:
            method = typeA->lookup(special);
            progress = ComparisonProgress::Everything;
            break;
        default:
            method = typeB->lookup(reflected);
            isReflected = true;
            progress = ComparisonProgress::Everything;
            break;
        }
        if (!method)
            continue;
        if (methods == MethodsWrittenInPython::AreLeftToCaller) {
            if (PyFunction* function = PyFunction::tryFrom(method); function && canBeCalledByOperator(function)) {
                rt.methodToCall = { function, progress | (isReflected ? ComparisonProgress::IsReflected : 0), JSValue::encode(a), JSValue::encode(b) };
                return { };
            }
        }
        JSValue result = callOperatorMethod(globalObject, isReflected ? typeB : typeA, isReflected ? reflected : special, method, isReflected ? b : a, isReflected ? a : b);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
    }
    return notImplemented;
}

static ALWAYS_INLINE JSValue goOnWithComparison(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op, unsigned progress, MethodsWrittenInPython methods)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = askComparisonMethods(globalObject, a, b, op, progress, methods);
    RETURN_IF_EXCEPTION(scope, { });
    if (result != rt.notImplemented()) [[likely]]
        return result;
    switch (op) {
    case CompareOperator::Eq:
        return jsBoolean(isIdentical(a, b));
    case CompareOperator::NotEq:
        return jsBoolean(!isIdentical(a, b));
    default:
        return raiseTypeError(globalObject, scope, '\'', operatorSymbols[static_cast<unsigned>(op)], "' not supported between instances of '"_s, rt.typeOf(a)->nameInMessages(), "' and '"_s, rt.typeOf(b)->nameInMessages(), '\'');
    }
}

// What needs no method. Empty: it does. An exception: the stack is at its end.
static ALWAYS_INLINE JSValue tryCompareWithoutMethods(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op)
{
    // Numbers that are not boxed compare as JS numbers do, whether they are ints or floats.
    if (a.isNumber() && b.isNumber())
        return jsBoolean(compareOrdered(a.asNumber(), b.asNumber(), op));
    if (a.isString() && b.isString())
        return strCompare(globalObject, asString(a), asString(b), op);
    VM& vm = getVM(globalObject);
    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded in comparison"_s);
    }
    return { };
}

// Apart from what comes first, which is all there is to most comparisons that get this far.
template<MethodsWrittenInPython methods>
static NEVER_INLINE JSValue askComparisonMethodsFromTheStart(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op)
{
    return goOnWithComparison(globalObject, a, b, op, ComparisonProgress::Nothing, methods);
}

template<MethodsWrittenInPython methods>
static ALWAYS_INLINE JSValue richCompareImpl(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = tryCompareWithoutMethods(globalObject, a, b, op);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;
    RELEASE_AND_RETURN(scope, askComparisonMethodsFromTheStart<methods>(globalObject, a, b, op));
}

JSValue richCompare(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op)
{
    return richCompareImpl<MethodsWrittenInPython::AreCalled>(globalObject, a, b, op);
}

JSValue beginComparison(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op)
{
    return richCompareImpl<MethodsWrittenInPython::AreLeftToCaller>(globalObject, a, b, op);
}

JSValue continueComparison(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op, unsigned progress, MethodsWrittenInPython methods)
{
    return goOnWithComparison(globalObject, a, b, op, progress & ~ComparisonProgress::IsReflected, methods);
}

static JSValue richCompareOrNotImplemented(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = tryCompareWithoutMethods(globalObject, a, b, op);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;
    RELEASE_AND_RETURN(scope, askComparisonMethods(globalObject, a, b, op, ComparisonProgress::Nothing, MethodsWrittenInPython::AreCalled));
}

bool haveSameText(JSGlobalObject* globalObject, JSString* a, JSString* b)
{
    return a->equal(globalObject, b);
}

bool richCompareBool(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op)
{
    if (isIdentical(a, b)) {
        if (op == CompareOperator::Eq)
            return true;
        if (op == CompareOperator::NotEq)
            return false;
    }
    if (a.isString() && b.isString() && (op == CompareOperator::Eq || op == CompareOperator::NotEq)) {
        bool equal = asString(a)->equal(globalObject, asString(b));
        return equal == (op == CompareOperator::Eq);
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = richCompare(globalObject, a, b, op);
    RETURN_IF_EXCEPTION(scope, false);
    if (result.isBoolean())
        return result.asBoolean();
    RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
}

JSValue compare(JSGlobalObject* globalObject, JSValue a, JSValue b, CompareOperator op)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    switch (op) {
    case CompareOperator::Is:
        return jsBoolean(isIdentical(a, b));
    case CompareOperator::IsNot:
        return jsBoolean(!isIdentical(a, b));
    case CompareOperator::In:
    case CompareOperator::NotIn: {
        bool found = contains(globalObject, b, a);
        RETURN_IF_EXCEPTION(scope, { });
        return jsBoolean(found == (op == CompareOperator::In));
    }
    case CompareOperator::ExceptionMatch: {
        bool matches = exceptionMatches(globalObject, a, b);
        RETURN_IF_EXCEPTION(scope, { });
        return jsBoolean(matches);
    }
    default:
        RELEASE_AND_RETURN(scope, richCompare(globalObject, a, b, op));
    }
}

// ---- Containers ----

int64_t length(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value.isCell()) {
        JSCell* cell = value.asCell();
        switch (cell->type()) {
        case ArrayType:
            return uncheckedDowncast<JSArray>(cell)->length();
        case StringType:
            RELEASE_AND_RETURN(scope, strLength(globalObject, uncheckedDowncast<JSString>(cell)));
        default:
            break;
        }
    }
    PyType* type = rt.typeOf(value);
    JSValue method = type->lookup(Special::Len);
    if (!method) {
        raiseTypeError(globalObject, scope, "object of type '"_s, type->nameInMessages(), "' has no len()"_s);
        return -1;
    }
    JSValue result = callSpecial(globalObject, type, Special::Len, method, value);
    RETURN_IF_EXCEPTION(scope, -1);
    if (result.isInt32() && result.asInt32() >= 0) [[likely]]
        return result.asInt32();
    result = toIndex(globalObject, result);
    RETURN_IF_EXCEPTION(scope, -1);
    if (intSign(result) < 0) {
        raiseValueError(globalObject, scope, "__len__() should return >= 0"_s);
        return -1;
    }
    RELEASE_AND_RETURN(scope, toSsize(globalObject, result, rt.OverflowError()));
}

int64_t lengthHint(JSGlobalObject* globalObject, JSValue value, int64_t defaultValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = rt.typeOf(value);
    if (type->lookup(Special::Len)) {
        int64_t size = length(globalObject, value);
        EXCEPTION_ASSERT(!!scope.exception() == (size < 0));
        if (size >= 0)
            return size;
        if (!clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            return -1;
    }
    JSValue method = type->lookup(Special::LengthHint);
    if (!method)
        return defaultValue;
    JSValue result = callSpecial(globalObject, method, value);
    if (failed(scope, result)) {
        if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            return defaultValue;
        return -1;
    }
    if (result == rt.notImplemented())
        return defaultValue;
    if (!isInt(result)) {
        raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__length_hint__() must return an int, not "_s, typeNameForMessage(globalObject, rt.typeOf(result)));
        return -1;
    }
    auto converted = tryInt64(intValue(result));
    if (!converted) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        return -1;
    }
    int64_t size = *converted;
    if (size < 0) {
        raiseValueError(globalObject, scope, typeNameForMessage(globalObject, type), ".__length_hint__() must return a non-negative int"_s);
        return -1;
    }
    return size;
}

// A built-in type may have the items of a sequence and not those of a mapping (a deque): its __getitem__ and the like take an index.
// Between brackets, what is no index is refused in other words than the method's, and one that is too large is an IndexError.
static ALWAYS_INLINE bool takesIndexOnly(JSValue method)
{
    auto* native = dynamicDowncast<PyNativeFunction>(method);
    return native && (native->def().flags & SequenceOperation);
}

static JSValue indexIntoSequence(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasIndex(globalObject, key))
        return raiseTypeError(globalObject, scope, "sequence index must be integer, not '"_s, typeName(globalObject, key), '\'');
    int64_t index = toSsize(globalObject, key, runtime(globalObject).IndexError());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeInt(globalObject, index));
}

JSValue getItem(JSGlobalObject* globalObject, JSValue object, JSValue key)
{
    return getItemInline(globalObject, object, key);
}

JSValue getItemByMethod(JSGlobalObject* globalObject, JSValue object, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyType* type = rt.typeOf(object);
    if (JSValue method = type->lookup(Special::GetItem)) {
        if (takesIndexOnly(method)) [[unlikely]] {
            key = indexIntoSequence(globalObject, key);
            RETURN_IF_EXCEPTION(scope, { });
        }
        RELEASE_AND_RETURN(scope, callSpecial(globalObject, type, Special::GetItem, method, object, key));
    }

    if (PyType* asType = PyType::tryFrom(object)) {
        // list[int]
        if (asType == rt.typeType())
            RELEASE_AND_RETURN(scope, makeGenericAlias(globalObject, object, key));
        JSValue method = getAttrIfPresent(globalObject, object, rt.name(Special::ClassGetItem));
        RETURN_IF_EXCEPTION(scope, { });
        if (method && !isNone(method))
            RELEASE_AND_RETURN(scope, call(globalObject, method, key));
        return raiseTypeError(globalObject, scope, "type '"_s, asType->nameInMessages(), "' is not subscriptable"_s);
    }
    return raiseTypeError(globalObject, scope, '\'', type->nameInMessages(), "' object is not subscriptable"_s);
}

// Whether the first class of the MRO that has the name is one of the program's.
static bool isGivenByClassOfProgram(Runtime& rt, PyType* type, Special special)
{
    PyTuple* mro = type->mro();
    for (unsigned i = 0; type->isHeapType() && i < mro->length(); ++i) {
        auto* base = uncheckedDowncast<PyType>(mro->at(i).asCell());
        if (base->dict()->get(rt.name(special)))
            return base->isHeapType();
    }
    return false;
}

bool hasTableOfSequenceMethods(PyType* type)
{
    return type->isHeapType() || isMadeAtRunTimeInCPython(type) || type->lookup(Special::Len) || type->lookup(Special::Contains);
}

void setItem(JSGlobalObject* globalObject, JSValue object, JSValue key, JSValue value)
{
    setItemInline(globalObject, object, key, value);
}

void setItemByMethod(JSGlobalObject* globalObject, JSValue object, JSValue key, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyType* type = rt.typeOf(object);
    JSValue method = type->lookup(Special::SetItem);
    if (!method) {
        // In CPython the two are one: a class with either is asked, and is found not to have the other.
        if (isGivenByClassOfProgram(rt, type, Special::DelItem)) {
            raise(globalObject, scope, rt.AttributeError(), "__setitem__"_s);
            return;
        }
        // An index into a sequence is looked at before the sequence is found not to take it.
        if (hasTableOfSequenceMethods(type) && hasIndex(globalObject, key)) {
            toSsize(globalObject, key, rt.IndexError());
            RETURN_IF_EXCEPTION(scope, void());
        }
        raiseTypeError(globalObject, scope, '\'', type->nameInMessages(), "' object does not support item assignment"_s);
        return;
    }
    if (takesIndexOnly(method)) [[unlikely]] {
        key = indexIntoSequence(globalObject, key);
        RETURN_IF_EXCEPTION(scope, void());
    }
    scope.release();
    callSpecial(globalObject, type, Special::SetItem, method, object, key, value);
}

void delItem(JSGlobalObject* globalObject, JSValue object, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, object);
    JSValue method = type->lookup(Special::DelItem);
    if (!method) {
        if (isGivenByClassOfProgram(runtime(globalObject), type, Special::SetItem)) {
            raise(globalObject, scope, runtime(globalObject).AttributeError(), "__delitem__"_s);
            return;
        }
        // CPython words it otherwise for an index into a sequence, which it looks at first.
        bool isIndexIntoSequence = hasTableOfSequenceMethods(type) && hasIndex(globalObject, key);
        if (isIndexIntoSequence) {
            toSsize(globalObject, key, runtime(globalObject).IndexError());
            RETURN_IF_EXCEPTION(scope, void());
        }
        raiseTypeError(globalObject, scope, '\'', type->nameInMessages(), isIndexIntoSequence ? "' object doesn't support item deletion"_s : "' object does not support item deletion"_s);
        return;
    }
    if (takesIndexOnly(method)) [[unlikely]] {
        key = indexIntoSequence(globalObject, key);
        RETURN_IF_EXCEPTION(scope, void());
    }
    scope.release();
    callSpecial(globalObject, type, Special::DelItem, method, object, key);
}

// Anything with a __getitem__ that a class of the program's gave it may be a sequence, and can then be iterated over without an __iter__. A
// built-in type that is a sequence has an __iter__, or says that its __getitem__ is that of a sequence (the arrays of ctypes); the
// __getitem__ of the others (re.Match, int | str) takes keys.
static bool isIterableThroughGetItem(Runtime& rt, PyType* type)
{
    if (type->has(TypeFlag::DictSubclass))
        return false;
    JSValue method = type->lookup(Special::GetItem);
    auto* native = PyNativeFunction::tryFrom(method);
    if (!native)
        return !!method;
    if (native->def().flags & ItemOfSequence)
        return true;
    // It goes by who has the name, not by what is under it: `__getitem__ = items.__getitem__` in a class statement.
    return isGivenByClassOfProgram(rt, type, Special::GetItem);
}

bool contains(JSGlobalObject* globalObject, JSValue container, JSValue item)
{
    return containsInline(globalObject, container, item);
}

bool containsByMethod(JSGlobalObject* globalObject, JSValue container, JSValue item)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyType* type = rt.typeOf(container);
    JSValue method = type->lookup(Special::Contains);
    if (method && !isNone(method)) {
        JSValue result = callSpecialIfThere(globalObject, type, Special::Contains, method, container, item);
        RETURN_IF_EXCEPTION(scope, false);
        if (method && result.isBoolean())
            return result.asBoolean();
        if (method)
            RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
    }
    if (method) {
        raiseTypeError(globalObject, scope, '\'', type->nameInMessages(), "' object is not a container"_s);
        return false;
    }
    JSValue iterator = getIter(globalObject, container);
    if (failed(scope, iterator)) {
        if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            raiseTypeError(globalObject, scope, "argument of type '"_s, type->nameInMessages(), "' is not a container or iterable"_s);
        return false;
    }
    while (true) {
        JSValue candidate = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!candidate)
            return false;
        bool found = isEqual(globalObject, candidate, item);
        RETURN_IF_EXCEPTION(scope, false);
        if (found)
            return true;
    }
}

// ---- Iteration ----

JSValue getIter(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isCell() && value.asCell()->type() == ArrayType)
        RELEASE_AND_RETURN(scope, PyIterator::create(globalObject, PyIterator::Kind::List, value));

    PyType* type = rt.typeOf(value);
    JSValue method = type->lookup(Special::Iter);
    bool hadMethod = !!method;
    if (method && !isNone(method)) {
        JSValue iterator = callSpecialIfThere(globalObject, type, Special::Iter, method, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (method && !isIterator(globalObject, iterator))
            return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__iter__() must return an iterator, not "_s, typeNameForMessage(globalObject, rt.typeOf(iterator)));
        if (method)
            return iterator;
    }
    if (!method && isIterableThroughGetItem(rt, type)) {
        // After an __iter__ that was not there, the same goes for __getitem__, which is only asked for.
        if (JSValue getItem = type->lookup(Special::GetItem); hadMethod && !isMethodDescriptor(getItem)) [[unlikely]] {
            if (failed(scope, bindDescriptor(globalObject, getItem, value, type))) {
                takePendingException(globalObject);
                return raiseTypeError(globalObject, scope, '\'', type->nameInMessages(), "' object is not iterable"_s);
            }
        }
        RELEASE_AND_RETURN(scope, PyIterator::create(globalObject, PyIterator::Kind::Sequence, value));
    }
    return raiseTypeError(globalObject, scope, '\'', type->nameInMessages(), "' object is not iterable"_s);
}

void Items::initializeWithStr(JSString* string)
{
    // (Out of memory.)
    if (string->value(m_globalObject)->isNull())
        return;
    if (!string->is8Bit())
        return initializeWithIterator(string);
    m_cell = string;
    m_kind = Kind::Latin1Str;
}

void Items::initializeWithIterator(JSValue iterable)
{
    JSValue iterator = getIter(m_globalObject, iterable);
    if (!iterator)
        return;
    // An iterator is an object.
    m_cell = iterator.asCell();
    if (m_cell->type() != PyObjectType || m_cell->classInfo() != PyIterator::info())
        m_kind = Kind::Iterator;
    else
        m_kind = uncheckedDowncast<PyIterator>(m_cell)->kind() == PyIterator::Kind::Range ? Kind::Range : Kind::BuiltinIterator;
}

bool isIterator(JSGlobalObject* globalObject, JSValue value)
{
    JSValue method = typeOf(globalObject, value)->lookup(Special::Next);
    return method && !isNone(method);
}

JSValue iterNext(JSGlobalObject* globalObject, JSValue iterator)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (iterator.isCell()) {
        JSCell* cell = iterator.asCell();
        if (cell->type() == PyObjectType) {
            const ClassInfo* info = cell->classInfo();
            if (info == PyIterator::info())
                RELEASE_AND_RETURN(scope, uncheckedDowncast<PyIterator>(cell)->next(globalObject));
            if (info == PyMultiIterator::info() && !uncheckedDowncast<PyMultiIterator>(cell)->type()->isHeapType()) {
                JSValue result = uncheckedDowncast<PyMultiIterator>(cell)->next(globalObject);
                EXCEPTION_ASSERT(!scope.exception() || !result);
                // A StopIteration out of the function of a map or a filter ends the iteration like any other.
                if (!result) [[unlikely]]
                    clearPendingExceptionIfMatches(globalObject, rt.StopIteration());
                return result;
            }
        } else if (cell->type() == PyGeneratorType) {
            auto* generator = uncheckedDowncast<PyGenerator>(cell);
            if (generator->kind() == PyGenerator::Kind::Generator) {
                JSValue result;
                if (generator->state() >= 0) [[likely]] {
                    PyGenerator::Outcome outcome = generator->resume(globalObject, jsUndefined(), JSGenerator::ResumeMode::NormalMode, result);
                    EXCEPTION_ASSERT(!!scope.exception() == (outcome == PyGenerator::Outcome::Threw));
                    if (outcome == PyGenerator::Outcome::Yielded) [[likely]]
                        return result;
                    // The generator of a lambda lets a StopIteration out (CodeInfo::isLambda), which ends the iteration like any other.
                    if (outcome == PyGenerator::Outcome::Threw)
                        clearPendingExceptionIfMatches(globalObject, rt.StopIteration());
                    return { };
                }
                RELEASE_AND_RETURN(scope, generatorSend(globalObject, generator, jsUndefined(), result));
            }
        }
    }

    PyType* type = rt.typeOf(iterator);
    JSValue method = type->lookup(Special::Next);
    if (!method || isNone(method))
        return raiseTypeError(globalObject, scope, '\'', type->name(), "' object is not an iterator"_s);
    JSValue result = callSpecial(globalObject, type, Special::Next, method, iterator);
    EXCEPTION_ASSERT(!scope.exception() || !result);
    if (result) [[likely]]
        return result;
    clearPendingExceptionIfMatches(globalObject, rt.StopIteration());
    return { };
}

// ---- isinstance, issubclass ----

static JSValue abstractBases(JSGlobalObject* globalObject, JSValue cls)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue bases = getAttrIfPresent(globalObject, cls, runtime(globalObject).names.dBases());
    RETURN_IF_EXCEPTION(scope, { });
    if (bases && !PyTuple::is(bases))
        return { };
    return bases;
}

static bool abstractIsSubclass(JSGlobalObject* globalObject, JSValue derived, JSValue cls)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    while (true) {
        if (derived == cls)
            return true;
        JSValue bases = abstractBases(globalObject, derived);
        RETURN_IF_EXCEPTION(scope, false);
        if (!bases)
            return false;
        PyTuple* tuple = PyTuple::from(bases);
        if (!tuple->length())
            return false;
        if (tuple->length() == 1) {
            derived = tuple->at(0);
            continue;
        }
        // Whatever has a __bases__ counts as a class here, and that may lead anywhere, or round in circles.
        if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
            raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded in __issubclass__"_s);
            return false;
        }
        for (unsigned i = 0; i < tuple->length(); ++i) {
            bool result = abstractIsSubclass(globalObject, tuple->at(i), cls);
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return true;
        }
        return false;
    }
}

// type.__instancecheck__
bool isInstanceWithoutHook(JSGlobalObject* globalObject, JSValue object, JSValue cls)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyType* type = PyType::tryFrom(cls)) {
        PyType* objectType = rt.typeOf(object);
        if (objectType->isSubtypeOf(type))
            return true;
        // An object may claim another class.
        JSValue claimed = getAttrIfPresent(globalObject, object, rt.names.dClass());
        RETURN_IF_EXCEPTION(scope, false);
        if (PyType* claimedType = PyType::tryFrom(claimed); claimedType && claimedType != objectType)
            return claimedType->isSubtypeOf(type);
        return false;
    }
    JSValue bases = abstractBases(globalObject, cls);
    RETURN_IF_EXCEPTION(scope, false);
    if (!bases) {
        raiseTypeError(globalObject, scope, "isinstance() arg 2 must be a type, a tuple of types, or a union"_s);
        return false;
    }
    JSValue claimed = getAttrIfPresent(globalObject, object, rt.names.dClass());
    RETURN_IF_EXCEPTION(scope, false);
    if (!claimed)
        return false;
    RELEASE_AND_RETURN(scope, abstractIsSubclass(globalObject, claimed, cls));
}

// type.__subclasscheck__
bool isSubclassWithoutHook(JSGlobalObject* globalObject, JSValue derived, JSValue cls)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* derivedType = PyType::tryFrom(derived);
    PyType* type = PyType::tryFrom(cls);
    if (derivedType && type)
        return derivedType->isSubtypeOf(type);
    JSValue bases = abstractBases(globalObject, derived);
    RETURN_IF_EXCEPTION(scope, false);
    if (!bases) {
        raiseTypeError(globalObject, scope, "issubclass() arg 1 must be a class"_s);
        return false;
    }
    bases = abstractBases(globalObject, cls);
    RETURN_IF_EXCEPTION(scope, false);
    if (!bases) {
        raiseTypeError(globalObject, scope, "issubclass() arg 2 must be a class, a tuple of classes, or a union"_s);
        return false;
    }
    RELEASE_AND_RETURN(scope, abstractIsSubclass(globalObject, derived, cls));
}

bool isInstance(JSGlobalObject* globalObject, JSValue object, JSValue cls)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyType* objectType = rt.typeOf(object);
    if (cls.isCell() && cls.asCell() == objectType)
        return true;
    PyType* classType = rt.typeOf(cls);
    if (classType == rt.typeType())
        RELEASE_AND_RETURN(scope, isInstanceWithoutHook(globalObject, object, cls));
    if (auto* tuple = PyTuple::tryFrom(cls)) {
        if (!vm.isSafeToRecurseSoft()) {
            raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded in __instancecheck__"_s);
            return false;
        }
        for (unsigned i = 0; i < tuple->length(); ++i) {
            bool result = isInstance(globalObject, object, tuple->at(i));
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return true;
        }
        return false;
    }
    if (PyTuple* members = unionArguments(globalObject, cls))
        RELEASE_AND_RETURN(scope, isInstance(globalObject, object, members));
    if (JSValue method = classType->lookup(Special::InstanceCheck)) {
        JSValue result = callSpecial(globalObject, method, cls, object);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
    }
    RELEASE_AND_RETURN(scope, isInstanceWithoutHook(globalObject, object, cls));
}

bool isSubclass(JSGlobalObject* globalObject, JSValue derived, JSValue cls)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyType* classType = rt.typeOf(cls);
    if (classType == rt.typeType()) {
        if (derived == cls)
            return true;
        RELEASE_AND_RETURN(scope, isSubclassWithoutHook(globalObject, derived, cls));
    }
    if (auto* tuple = PyTuple::tryFrom(cls)) {
        if (!vm.isSafeToRecurseSoft()) {
            raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded in __subclasscheck__"_s);
            return false;
        }
        for (unsigned i = 0; i < tuple->length(); ++i) {
            bool result = isSubclass(globalObject, derived, tuple->at(i));
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return true;
        }
        return false;
    }
    if (PyTuple* members = unionArguments(globalObject, cls))
        RELEASE_AND_RETURN(scope, isSubclass(globalObject, derived, members));
    if (JSValue method = classType->lookup(Special::SubclassCheck)) {
        JSValue result = callSpecial(globalObject, method, cls, derived);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
    }
    RELEASE_AND_RETURN(scope, isSubclassWithoutHook(globalObject, derived, cls));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
