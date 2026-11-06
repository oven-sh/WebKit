/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyJSClass.h"

#include "FunctionPrototype.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSPromiseConstructor.h"
#include "JSTypedArrayViewConstructor.h"
#include "JSWeakMapInlines.h"
#include "PropertyNameArray.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyJSView.h"
#include "PyStr.h"
#include "PyThreads.h"

namespace JSC {
namespace Python {

// ---- JSMethod ----

enum JSMethodField : unsigned { JSMethodFunction, JSMethodQualname, JSMethodFieldCount };

static JSValue functionOfJSMethod(JSValue method) { return uncheckedDowncast<PyRecord>(method.asCell())->get(JSMethodFunction); }

JSValue makeJSMethod(JSGlobalObject* globalObject, JSValue function)
{
    PyRecord* method = PyRecord::create(globalObject, jsMethodType(runtime(globalObject)), JSMethodFieldCount);
    method->set(getVM(globalObject), JSMethodFunction, function);
    return method;
}

static JSValue jsMethodNew(JSGlobalObject* globalObject, JSValue, JSValue function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isForeignCallee(function) || !asObject(function)->isCallable())
        return raiseTypeError(globalObject, scope, "JSMethod() argument must be a JavaScript function, not "_s, typeName(globalObject, function));
    return makeJSMethod(globalObject, function);
}

static JSValue jsMethodCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.isEmpty()) {
        JSString* name = str(globalObject, self);
        RETURN_IF_EXCEPTION(scope, { });
        return raiseTypeError(globalObject, scope, "unbound method "_s, stringOf(name), " needs an argument"_s);
    }
    RELEASE_AND_RETURN(scope, callForeignFunction(globalObject, functionOfJSMethod(self), args.withoutFirst(), args[0]));
}

static JSValue jsMethodGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue)
{
    return isNone(instance) ? self : JSValue(PyMethod::create(globalObject, self, instance));
}

// What it does not have itself is looked for on the function, where whatever is set goes: __name__, __isabstractmethod__, __doc__.
static JSValue jsMethodGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    PyType* type = typeOf(globalObject, self);
    if (JSValue descriptor = type->lookup(name))
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, descriptor, self, type));
    RELEASE_AND_RETURN(scope, getAttr(globalObject, functionOfJSMethod(self), name));
}

static JSValue jsMethodSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue nameValue, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (typeOf(globalObject, self)->lookup(name))
        RELEASE_AND_RETURN(scope, (genericSetAttr(globalObject, self, name, value), jsUndefined()));
    setAttr(globalObject, functionOfJSMethod(self), name, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue jsMethodDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    delAttr(globalObject, functionOfJSMethod(self), name);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue jsMethodRepr(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue name = getAttr(globalObject, self, rt.names.dQualname());
    RETURN_IF_EXCEPTION(scope, { });
    JSString* text = str(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    String address = addressForRepr(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString("<JavaScript method "_s, stringOf(text), " at "_s, address, '>'));
}

static JSValue jsMethodWrapped(JSGlobalObject*, JSValue self) { return functionOfJSMethod(self); }

// "Class.method", for one that was in the body of a class.
static JSValue jsMethodGetQualname(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue qualname = uncheckedDowncast<PyRecord>(self.asCell())->get(JSMethodQualname))
        return qualname;
    return getAttr(globalObject, functionOfJSMethod(self), runtime(globalObject).names.dQualname());
}

static void jsMethodSetQualname(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !isStr(value)) {
        raiseTypeError(globalObject, scope, "__qualname__ must be set to a string object"_s);
        return;
    }
    uncheckedDowncast<PyRecord>(self.asCell())->set(vm, JSMethodQualname, value);
}

static JSValue jsMethodSetName(JSGlobalObject* globalObject, JSValue self, JSValue owner, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto* record = uncheckedDowncast<PyRecord>(self.asCell());
    if (auto* type = PyType::tryFrom(owner); type && isStr(name) && !record->get(JSMethodQualname))
        record->set(vm, JSMethodQualname, makeStr(vm, makeString(stringOf(type->qualname()), '.', stringOf(strValue(name)))));
    return jsUndefined();
}

static constexpr NativeDef jsMethodMethods[] = {
    PY_NEW(jsMethodNew, "($type, function, /)"),
    PY_SLOT("__call__", jsMethodCall, "($self, *args, **kwargs)"),
    PY_SLOT("__get__", jsMethodGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__getattribute__", jsMethodGetAttribute, "($self, name, /)"),
    PY_SLOT("__setattr__", jsMethodSetAttr, "($self, name, value, /)"),
    PY_SLOT("__delattr__", jsMethodDelAttr, "($self, name, /)"),
    PY_SLOT("__repr__", jsMethodRepr, "($self, /)"),
    PY_METHOD("__set_name__", jsMethodSetName, "($self, owner, name, /)"),
};
static constexpr NativeGetSet jsMethodGetSets[] = {
    PY_MEMBER("__wrapped__", jsMethodWrapped),
    { "__qualname__"_s, jsMethodGetQualname, jsMethodSetQualname },
};
static constexpr TypeDef jsMethodTypeDef { "_js.JSMethod"_s, { TypeFlag::OwnLayout }, jsMethodMethods, jsMethodGetSets, { } };
PyType* jsMethodType(Runtime& rt) { return rt.typeFor(jsMethodTypeDef); }

static bool isJSMethod(Runtime& rt, JSValue value)
{
    return value && value.isCell() && value.asCell()->type() == PyObjectType && uncheckedDowncast<PyObject>(value.asCell())->type() == jsMethodType(rt);
}

// `static get x() {}`: the getter is called with the class as `this`. (To assign to Class.x replaces it, as it does any attribute
// of a class that its metaclass has nothing to say about.)
enum JSStaticAccessorField : unsigned { JSStaticAccessorGetter, JSStaticAccessorFieldCount };

static JSValue jsStaticAccessorGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue owner)
{
    JSValue cls = owner && !isNone(owner) ? owner : JSValue(typeOf(globalObject, instance));
    return callForeignFunction(globalObject, uncheckedDowncast<PyRecord>(self.asCell())->get(JSStaticAccessorGetter), CallArgs(), cls);
}

static constexpr NativeDef jsStaticAccessorMethods[] = {
    PY_SLOT("__get__", jsStaticAccessorGet, "($self, instance, owner=None, /)"),
};
static constexpr TypeDef jsStaticAccessorTypeDef { "_js.JSStaticAccessor"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, jsStaticAccessorMethods, { }, { } };

// ---- Type.prototype ----

PyType* jsTypedArrayType(Runtime&); // PyJSBuffers.cpp

const ClassInfo PyPrototypeView::s_info = { "PyPrototypeView"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyPrototypeView) };

PyPrototypeView::PyPrototypeView(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

template<typename Visitor>
void PyPrototypeView::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyPrototypeView>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_type);
}

DEFINE_VISIT_CHILDREN(PyPrototypeView);

JSObject* prototypeOfClassOfJS(PyType* type)
{
    JSObject* prototype = type->jsPrototype();
    return prototype && !prototype->inherits<PyPrototypeView>() ? prototype : nullptr;
}

JSObject* prototypeForJS(JSGlobalObject* globalObject, PyType* type)
{
    if (JSObject* prototype = type->jsPrototype())
        return prototype;
    return PyPrototypeView::of(globalObject, type);
}

// The first in the MRO. Null: the type derives from none, or has no MRO yet.
static PyType* firstClassOfJS(PyType* type)
{
    PyTuple* mro = type->mro();
    for (unsigned i = 0; mro && i < mro->length(); ++i) {
        if (PyType* next = PyType::from(mro->at(i)); prototypeOfClassOfJS(next))
            return next;
    }
    return nullptr;
}

bool PyPrototypeView::makesInstanceOf(JSObject* object)
{
    if (m_instances == Instances::Unknown) [[unlikely]] {
        PyType* type = m_type.get();
        if (!type->mro())
            return false;
        Runtime& rt = runtime(realm());
        if (!firstClassOfJS(type))
            m_instances = Instances::NotOfJS;
        else if (type->isSubtypeOf(rt.jsMapType()))
            m_instances = Instances::Maps;
        else if (type->isSubtypeOf(rt.jsPromiseType()))
            m_instances = Instances::Promises;
        else
            m_instances = type->isSubtypeOf(jsTypedArrayType(rt)) ? Instances::TypedArrays : Instances::Objects;
    }
    // As in Runtime::typeOfSlow(): what the object is without a class of Python's.
    switch (object->type()) {
    case JSMapType:
        return m_instances == Instances::Maps || m_instances == Instances::Objects;
    case JSPromiseType:
        return m_instances == Instances::Promises || m_instances == Instances::Objects;
    case ArrayType:
    case DerivedArrayType:
    case JSFunctionType:
    case InternalFunctionType:
    case JSGeneratorType:
    case JSArrayIteratorType:
    case JSMapIteratorType:
    case JSSetIteratorType:
    case JSStringIteratorType:
    case JSRegExpStringIteratorType:
    case JSIteratorHelperType:
    case JSWrapForValidIteratorType:
        return false;
    default:
        return m_instances == Instances::Objects || (m_instances == Instances::TypedArrays && isTypedArrayType(object->type()));
    }
}

PyType* typeByPrototype(JSObject* object)
{
    JSValue prototype = object->getPrototypeDirect();
    if (!prototype.isCell())
        return nullptr;
    auto* view = dynamicDowncast<PyPrototypeView>(prototype.asCell());
    // (Object.create(list.prototype) is no list, and Object.create(Derived.prototype) no Uint8Array.)
    return view && view->makesInstanceOf(object) ? view->type() : nullptr;
}

PyPrototypeView* PyPrototypeView::of(JSGlobalObject* globalObject, PyType* type)
{
    ASSERT(!prototypeOfClassOfJS(type));
    if (JSObject* view = type->jsPrototype())
        return uncheckedDowncast<PyPrototypeView>(view);
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // As the instances have it: what all objects of Python's inherit comes last.
    JSObject* parent;
    if (PyType* base = type->base())
        parent = prototypeForJS(globalObject, base);
    else
        parent = rt.jsViewObject(JSViewObject::ObjectPrototype);
    Structure* structure = createStructure(vm, globalObject, parent);
    auto* view = new (NotNull, allocateCell<PyPrototypeView>(vm)) PyPrototypeView(vm, structure);
    view->finishCreation(vm);
    view->m_type.set(vm, view, type);
    view->didBecomePrototype(vm);
    type->setJSPrototype(vm, view);
    return view;
}

PyPrototypeView* PyPrototypeView::createSuper(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // For what has a symbol for a key, which goes by the first bases alone.
    JSObject* parent = type->base() ? prototypeForJS(globalObject, type->base()) : rt.jsViewObject(JSViewObject::ObjectPrototype);
    Structure* structure = createStructure(vm, globalObject, parent);
    auto* view = new (NotNull, allocateCell<PyPrototypeView>(vm)) PyPrototypeView(vm, structure);
    view->finishCreation(vm);
    view->m_type.set(vm, view, type);
    view->m_isSuper = true;
    view->m_instances = Instances::NotOfJS;
    view->didBecomePrototype(vm);
    return view;
}

// What the types have for the name that follow the type in the MRO of `start`, a type derived from it; with the type itself, if asked.
static JSValue lookupFollowing(PyType* start, PyType* type, bool includingType, JSString* name)
{
    PyTuple* mro = start->mro();
    if (!mro)
        return { };
    unsigned i = 0;
    while (i < mro->length() && mro->at(i).asCell() != type)
        ++i;
    for (i += includingType ? 0 : 1; i < mro->length(); ++i) {
        PyType* next = PyType::from(mro->at(i));
        // From a class of JS on it is for JS to look: its prototype follows this one.
        if (prototypeOfClassOfJS(next))
            break;
        if (JSValue found = next->dict()->get(name))
            return found;
    }
    return { };
}

// (class Both(Plain, js.Map): the __dict__ that Plain gives its instances is not for one that has none.)
static JSValue lookupForInstanceOfDerivedClass(PyType* start, PyType* type, bool includingType, JSString* name)
{
    JSValue found = lookupFollowing(start, type, includingType, name);
    return found && start->has(TypeFlag::AttributesAreProperties) && isDictDescriptorOfInstances(found) ? JSValue() : found;
}

JSValue lookupBeforeClassOfJS(PyType* type, JSString* name)
{
    return lookupForInstanceOfDerivedClass(type, type, true, name);
}

JSObject* prototypeBehindViews(JSObject* object)
{
    JSValue prototype = object->getPrototypeDirect();
    while (prototype.isObject() && prototype.inherits<PyPrototypeView>())
        prototype = asObject(prototype)->getPrototypeDirect();
    return prototype.isObject() ? asObject(prototype) : nullptr;
}

// Type.prototype.name.call(instance, ...) is Type.name(instance, ...).
static constexpr unsigned unboundMethodField = 0;

static JSC_DECLARE_HOST_FUNCTION(callUnboundMethod);
JSC_DEFINE_HOST_FUNCTION(callUnboundMethod, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSValue function = hiddenField(globalObject, callFrame->jsCallee(), unboundMethodField);
    JSValue result = withArgumentsFromJS(globalObject, callFrame, [&](const CallArgs& args) {
        return withFirstArgument(callFrame->thisValue(), args, [&](const CallArgs& all) {
            // As wherever JS calls a function of Python's: what it has no parameter for is dropped.
            unsigned most = all.size();
            if (auto* native = PyNativeFunction::tryFrom(function); native && !native->def().signature.isVariadic)
                most = native->def().signature.maxPositional + 1;
            else if (auto* pyFunction = PyFunction::tryFrom(function); pyFunction && !pyFunction->codeInfo().flags.contains(CodeFlag::VarArgs))
                most = pyFunction->codeInfo().argCount;
            if (all.size() > most && !all.hasKeywords())
                return call(globalObject, function, CallArgs(all.positional().first(most)));
            return call(globalObject, function, all);
        });
    });
    return JSValue::encode(valueForJS(result));
}

static JSValue unboundForJS(JSGlobalObject* globalObject, JSString* name, JSValue found)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (isJSMethod(rt, found))
        return functionOfJSMethod(found);
    bool isMethod = PyFunction::is(found);
    if (auto* native = PyNativeFunction::tryFrom(found))
        isMethod = native->kind() == NativeKind::Method || native->kind() == NativeKind::SlotWrapper;
    if (!isMethod)
        return valueForJS(found);
    JSFunction* function = JSFunction::create(vm, globalObject, 0, stringOf(name), callUnboundMethod, ImplementationVisibility::Public);
    setHiddenField(globalObject, function, unboundMethodField, found);
    return function;
}

bool PyPrototypeView::getOwnPropertySlot(JSObject* object, JSGlobalObject*, PropertyName propertyName, PropertySlot& slot)
{
    JSGlobalObject* globalObject = object->realm();
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (propertyName.isSymbol())
        RELEASE_AND_RETURN(scope, Base::getOwnPropertySlot(object, globalObject, propertyName, slot));
    slot.disableCaching();
    if (slot.isVMInquiry()) {
        slot.setIsTaintedByOpaqueObject();
        return false;
    }
    constexpr unsigned hidden = static_cast<unsigned>(PropertyAttribute::DontEnum);
    auto* view = uncheckedDowncast<PyPrototypeView>(object);
    PyType* type = view->type();
    if (propertyName == vm.propertyNames->constructor) {
        if (view->isSuper())
            return false;
        slot.setValue(object, hidden, type);
        return true;
    }
    JSString* name = internString(vm, String(propertyName.uid()));

    // As super(type, receiver) has it: the receiver is an instance of the type, or a class derived from it.
    JSValue receiver = slot.thisValue();
    JSValue instance = receiver;
    PyType* start = receiver && receiver != JSValue(object) ? rt.typeOf(receiver) : nullptr;
    if (start && !start->isSubtypeOf(type)) {
        start = PyType::tryFrom(receiver);
        instance = { };
    }
    if (!start || !start->isSubtypeOf(type) || !start->mro()) {
        JSValue found = lookupFollowing(type, type, !view->isSuper(), name);
        if (!found)
            return false;
        slot.setValue(object, hidden, unboundForJS(globalObject, name, found));
        return true;
    }
    JSValue found = lookupForInstanceOfDerivedClass(start, type, !view->isSuper(), name);
    if (!found)
        return false;
    // `name in instance` runs no getter.
    if (slot.internalMethodType() == PropertySlot::InternalMethodType::HasProperty) {
        slot.setValue(object, hidden, jsUndefined());
        return true;
    }
    JSValue bound = bindDescriptor(globalObject, found, instance, start);
    RETURN_IF_EXCEPTION(scope, false);
    slot.setValue(object, hidden, valueForJS(bound));
    return true;
}

bool PyPrototypeView::getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot& slot)
{
    slot.disableCaching();
    return false;
}

bool PyPrototypeView::put(JSCell* cell, JSGlobalObject*, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    auto* view = uncheckedDowncast<PyPrototypeView>(cell);
    JSGlobalObject* globalObject = view->realm();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (propertyName.isSymbol())
        RELEASE_AND_RETURN(scope, Base::put(cell, globalObject, propertyName, value, slot));
    // super.name = value; instance.name = value, where the instance is an object of JS.
    if (slot.thisValue() != JSValue(cell)) [[unlikely]] {
        Runtime& rt = runtime(globalObject);
        if (PyType* start = rt.typeOf(slot.thisValue()); start->isSubtypeOf(view->type())) {
            JSValue found = lookupForInstanceOfDerivedClass(start, view->type(), !view->isSuper(), internString(vm, String(propertyName.uid())));
            if (JSValue setter = found ? rt.typeOf(found)->lookup(Special::Set) : JSValue()) {
                callSpecial(globalObject, setter, found, slot.thisValue(), value);
                RETURN_IF_EXCEPTION(scope, false);
                return true;
            }
            // As a property of a prototype that holds a value: it hides what the prototypes behind have, and the receiver gets its own.
            if (found)
                RELEASE_AND_RETURN(scope, JSObject::definePropertyOnReceiver(globalObject, propertyName, value, slot));
        }
        // The classes of Python's have nothing for it. The class of JS's may have a setter.
        if (JSObject* parent = prototypeBehindViews(view))
            RELEASE_AND_RETURN(scope, parent->methodTable()->put(parent, globalObject, propertyName, value, slot));
        RELEASE_AND_RETURN(scope, JSObject::definePropertyOnReceiver(globalObject, propertyName, value, slot));
    }
    PropertyDescriptor descriptor(value, 0);
    RELEASE_AND_RETURN(scope, defineOwnProperty(view, globalObject, propertyName, descriptor, slot.isStrictMode()));
}

bool PyPrototypeView::deleteProperty(JSCell* cell, JSGlobalObject*, PropertyName propertyName, DeletePropertySlot& slot)
{
    auto* view = uncheckedDowncast<PyPrototypeView>(cell);
    JSGlobalObject* globalObject = view->realm();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (propertyName.isSymbol())
        RELEASE_AND_RETURN(scope, Base::deleteProperty(cell, globalObject, propertyName, slot));
    JSString* name = internString(vm, String(propertyName.uid()));
    if (!view->type()->dict()->get(name))
        return true;
    delAttr(globalObject, view->type(), name);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

// As the methods of a class of JS, none can be enumerated.
void PyPrototypeView::getOwnPropertyNames(JSObject* object, JSGlobalObject*, PropertyNameArrayBuilder& builder, DontEnumPropertiesMode mode)
{
    JSGlobalObject* globalObject = object->realm();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (mode == DontEnumPropertiesMode::Include && builder.includeStringProperties()) {
        builder.add(vm.propertyNames->constructor);
        PyDict* dict = uncheckedDowncast<PyPrototypeView>(object)->type()->dict();
        JSValue key;
        JSValue value;
        for (unsigned position = 0; dict->next(position, key, value);) {
            if (!isStr(key))
                continue;
            Identifier identifier = strValue(key)->toIdentifier(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
            builder.add(identifier);
        }
    }
    RELEASE_AND_RETURN(scope, Base::getOwnPropertyNames(object, globalObject, builder, mode));
}

// What a property of a class of JS is in a class of Python's. Empty: an exception.
static JSValue attributeForProperty(JSGlobalObject* globalObject, const PropertyDescriptor& descriptor, bool isStatic)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto isFunctionOfJS = [](JSValue value) {
        return value && isForeignCallee(value) && asObject(value)->isCallable();
    };
    if (descriptor.isAccessorDescriptor()) {
        JSValue getter = descriptor.getterPresent() && isFunctionOfJS(descriptor.getter()) ? descriptor.getter() : JSValue();
        JSValue setter = descriptor.setterPresent() && isFunctionOfJS(descriptor.setter()) ? descriptor.setter() : JSValue();
        if (isStatic) {
            PyRecord* accessor = PyRecord::create(globalObject, rt.typeFor(jsStaticAccessorTypeDef), JSStaticAccessorFieldCount);
            accessor->set(vm, JSStaticAccessorGetter, getter ? getter : jsUndefined());
            return accessor;
        }
        RELEASE_AND_RETURN(scope, call(globalObject, rt.propertyType(), getter ? makeJSMethod(globalObject, getter) : jsUndefined(), setter ? makeJSMethod(globalObject, setter) : jsUndefined()));
    }
    JSValue value = descriptor.value();
    if (!isFunctionOfJS(value) || foreignMethodKind(value.asCell()) != ForeignMethodKind::TakesThis)
        return value ? value : jsUndefined();
    JSValue method = makeJSMethod(globalObject, value);
    if (isStatic)
        RELEASE_AND_RETURN(scope, makeClassMethod(globalObject, method));
    return method;
}

bool PyPrototypeView::defineOwnProperty(JSObject* object, JSGlobalObject*, PropertyName propertyName, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    JSGlobalObject* globalObject = object->realm();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (propertyName.isSymbol())
        RELEASE_AND_RETURN(scope, Base::defineOwnProperty(object, globalObject, propertyName, descriptor, shouldThrow));
    if (!descriptor.isAccessorDescriptor() && !descriptor.value())
        return true;
    JSValue attribute = attributeForProperty(globalObject, descriptor, false);
    RETURN_IF_EXCEPTION(scope, false);
    setAttr(globalObject, uncheckedDowncast<PyPrototypeView>(object)->type(), internString(vm, String(propertyName.uid())), attribute);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

bool symbolPropertyOfClasses(JSGlobalObject* globalObject, PyType* type, PropertyName propertyName, PropertySlot& slot)
{
    PyTuple* mro = type->mro();
    for (unsigned i = 0; mro && i < mro->length(); ++i) {
        JSObject* view = PyType::from(mro->at(i))->jsPrototype();
        if (view && JSObject::getOwnPropertySlot(view, globalObject, propertyName, slot))
            return true;
    }
    return false;
}

// ---- class Derived extends Base ----

JSValue constructBaseOfDerivedClass(JSGlobalObject* globalObject, PyType* type, PyType* derived, const CallArgs& args)
{
    return typeCall(globalObject, type, args, derived);
}

const ClassInfo PySuperConstructor::s_info = { "PySuperConstructor"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PySuperConstructor) };

static JSC_DECLARE_HOST_FUNCTION(callSuperConstructor);
static JSC_DECLARE_HOST_FUNCTION(constructSuperConstructor);

JSC_DEFINE_HOST_FUNCTION(callSuperConstructor, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "super(...) is for the constructor of a class"_s);
}

JSC_DEFINE_HOST_FUNCTION(constructSuperConstructor, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = uncheckedDowncast<PySuperConstructor>(callFrame->jsCallee())->type();
    PyType* derived = PyType::tryFrom(callFrame->newTarget());
    if (!derived || !derived->isSubtypeOf(type))
        return throwVMTypeError(globalObject, scope, "super(...) is for the constructor of a class"_s);
    RELEASE_AND_RETURN(scope, JSValue::encode(valueForJS(withArgumentsFromJS(globalObject, callFrame, [&](const CallArgs& args) {
        return typeCall(globalObject, type, args, derived, true);
    }))));
}

PySuperConstructor::PySuperConstructor(VM& vm, Structure* structure)
    : Base(vm, structure, callSuperConstructor, constructSuperConstructor)
{
}

template<typename Visitor>
void PySuperConstructor::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PySuperConstructor>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_type);
}

DEFINE_VISIT_CHILDREN(PySuperConstructor);

PySuperConstructor* PySuperConstructor::create(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    Structure* structure = createStructure(vm, globalObject, type->base() ? JSValue(type->base()) : jsNull());
    auto* function = new (NotNull, allocateCell<PySuperConstructor>(vm)) PySuperConstructor(vm, structure);
    function->finishCreation(vm);
    function->m_originalName.set(vm, function, jsEmptyString(vm));
    function->m_type.set(vm, function, type);
    return function;
}

bool PySuperConstructor::getOwnPropertySlot(JSObject* object, JSGlobalObject*, PropertyName propertyName, PropertySlot& slot)
{
    JSGlobalObject* globalObject = object->realm();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (propertyName.isSymbol())
        RELEASE_AND_RETURN(scope, Base::getOwnPropertySlot(object, globalObject, propertyName, slot));
    slot.disableCaching();
    if (slot.isVMInquiry()) {
        slot.setIsTaintedByOpaqueObject();
        return false;
    }
    PyType* type = uncheckedDowncast<PySuperConstructor>(object)->type();
    PyType* start = PyType::tryFrom(slot.thisValue());
    if (!start || !start->isSubtypeOf(type))
        start = type;
    JSValue found = lookupFollowing(start, type, false, internString(vm, String(propertyName.uid())));
    if (!found)
        return false;
    JSValue bound = bindDescriptor(globalObject, found, { }, start);
    RETURN_IF_EXCEPTION(scope, false);
    slot.setValue(object, static_cast<unsigned>(PropertyAttribute::DontEnum), valueForJS(bound));
    return true;
}

// Derived.__new__(cls, ...): `self` is the constructor.
static JSValue jsClassNew(JSGlobalObject* globalObject, JSValue constructor, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.isEmpty() || !PyType::is(args[0]))
        return raiseTypeError(globalObject, scope, "__new__(X): X is not a type object"_s);
    RELEASE_AND_RETURN(scope, constructForeignFunction(globalObject, constructor, args.withoutFirst(), args[0]));
}

// The constructor has seen to it.
static JSValue jsClassInit(JSGlobalObject*, JSValue, const CallArgs&) { return jsUndefined(); }

static bool isInitOfConstructor(JSValue init)
{
    auto* native = PyNativeFunction::tryFrom(init);
    return native && native->def().pointer.variadicWithSelf == jsClassInit;
}

bool hasInitOfItsOwn(PyType* type) { return !isInitOfConstructor(type->lookup(Special::Init)); }

bool constructorInitializes(PyType* type) { return isInitOfConstructor(type->dict()->get(runtime(type->realm()).name(Special::Init))); }

bool isFunctionOfJS(Runtime& rt, JSValue value)
{
    return isJSMethod(rt, value) || (value.isCell() && foreignMethodKind(value.asCell()) != ForeignMethodKind::NotAMethod);
}

// Whether there is anything for the constructor to do that making an instance as Python does would leave out.
static bool hasWorkForConstructor(JSObject* constructor)
{
    auto* function = dynamicDowncast<JSFunction>(constructor);
    if (!function || function->isHostFunction())
        return true;
    UnlinkedFunctionExecutable* executable = function->jsExecutable()->unlinkedExecutable();
    return !executable->isBuiltinDefaultClassConstructor()
        || executable->needsClassFieldInitializer() == NeedsClassFieldInitializer::Yes
        || executable->privateBrandRequirement() == PrivateBrandRequirement::Needed;
}

static constexpr NativeDef jsClassNewDef = PY_FUNCTION("__new__", jsClassNew, "($self, *args, **kwargs)");
static constexpr NativeDef jsClassInitDef = PY_METHOD("__init__", jsClassInit, "($self, *args, **kwargs)");

// Calls the function with the key and the descriptor of each property the object itself has. False: an exception.
template<typename Function>
static bool forEachOwnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyNameMode mode, const Function& function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyNameArrayBuilder keys(vm, mode, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, globalObject, keys, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, false);
    for (auto& key : keys) {
        PropertyDescriptor descriptor;
        bool isThere = object->getOwnPropertyDescriptor(globalObject, key, descriptor);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isThere)
            continue;
        function(key, descriptor);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

JSValue deriveClassFromJS(JSGlobalObject* lexicalGlobalObject, JSValue constructorValue, JSValue prototypeValue, JSValue superclass)
{
    if (!PyType::is(superclass) && !isPyObject(superclass))
        return constructorValue;
    UNUSED_PARAM(lexicalGlobalObject);
    JSGlobalObject* globalObject = asObject(superclass)->realm();
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* constructor = asObject(constructorValue);
    JSObject* prototype = asObject(prototypeValue);
    auto& names = *vm.propertyNames;

    JSValue nameValue = constructor->get(globalObject, names.name);
    RETURN_IF_EXCEPTION(scope, { });
    JSString* name = nameValue.isString() ? asString(nameValue) : jsEmptyString(vm);

    auto fill = [&](JSValue ns) -> JSValue {
        auto add = [&](JSString* key, JSValue value) {
            setItem(globalObject, ns, key, value);
        };
        add(rt.names.dModule(), internString(vm, "js"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (hasWorkForConstructor(constructor)) {
            add(rt.name(Special::New), PyNativeFunction::create(globalObject, jsClassNewDef, NativeKind::Function, constructor, { }));
            RETURN_IF_EXCEPTION(scope, { });
            add(rt.name(Special::Init), PyNativeFunction::create(globalObject, jsClassInitDef, NativeKind::Method, { }, rt.objectType()));
            RETURN_IF_EXCEPTION(scope, { });
        }
        for (bool isStatic : { false, true }) {
            bool succeeded = forEachOwnProperty(globalObject, isStatic ? constructor : prototype, PropertyNameMode::Strings, [&](const Identifier& key, const PropertyDescriptor& descriptor) {
                if (isStatic ? key == names.length || key == names.name || key == names.prototype : key == names.constructor)
                    return;
                JSValue attribute = attributeForProperty(globalObject, descriptor, isStatic);
                RETURN_IF_EXCEPTION(scope, void());
                add(internString(vm, key.string()), attribute);
            });
            if (failed(scope, succeeded))
                return { };
        }
        return jsUndefined();
    };
    JSValue result = buildClass(globalObject, name, PyTuple::create(globalObject, { superclass }), CallArgs(), fill);
    RETURN_IF_EXCEPTION(scope, { });
    PyType* type = PyType::tryFrom(result);
    if (!type)
        return result;

    // What Python has no names for stays with JS.
    PyPrototypeView* view = PyPrototypeView::of(globalObject, type);
    bool succeeded = forEachOwnProperty(globalObject, prototype, PropertyNameMode::Symbols, [&](const Identifier& key, const PropertyDescriptor& descriptor) {
        view->methodTable()->defineOwnProperty(view, globalObject, key, descriptor, true);
    });
    if (failed(scope, succeeded))
        return { };
    succeeded = forEachOwnProperty(globalObject, constructor, PropertyNameMode::Symbols, [&](const Identifier& key, const PropertyDescriptor& descriptor) {
        type->methodTable()->defineOwnProperty(type, globalObject, key, descriptor, true);
    });
    if (failed(scope, succeeded))
        return { };
    prototype->putDirect(vm, names.constructor, type, static_cast<unsigned>(PropertyAttribute::DontEnum));
    // `super` goes by the MRO, which classes that derive from this one and from others have a say in.
    prototype->setPrototypeDirect(vm, PyPrototypeView::createSuper(globalObject, type));
    constructor->setPrototypeDirect(vm, PySuperConstructor::create(globalObject, type));
    return type;
}

// ---- class Derived(Base), where Base is a class of JS ----

static JSValue classOfJSInit(JSGlobalObject*, JSValue self, const CallArgs&);
static bool isInitOfClassOfJS(JSValue init)
{
    auto* native = PyNativeFunction::tryFrom(init);
    return native && native->def().pointer.variadicWithSelf == classOfJSInit;
}

// Base.__new__(cls, ...) is `new Base(...)`, and there is no running the constructor of a class of JS a second time, or later. So
// Base.__init__() has nothing to do. A class with an __init__(self, name) that calls super().__init__() means Base to be constructed
// without arguments, though, and it has been constructed with the name: that is said, not passed over. What the constructor got is
// remembered until __init__ has seen it, for the instances of classes that have an __init__ of their own.
static JSWeakMap* argumentsOfConstructors(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (JSValue map = rt.slot(Runtime::Slot::ArgumentsOfJSConstructors))
        return uncheckedDowncast<JSWeakMap>(map);
    JSWeakMap* map = JSWeakMap::create(rt.vm(), globalObject->weakMapStructure());
    rt.setSlot(Runtime::Slot::ArgumentsOfJSConstructors, map);
    return map;
}

static PyTuple* tupleOfArguments(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    PyTuple* tuple = PyTuple::create(globalObject, args.size() + 2 * args.keywordCount());
    unsigned next = 0;
    for (unsigned i = 0; i < args.size(); ++i)
        tuple->initializeAt(vm, next++, args[i]);
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        tuple->initializeAt(vm, next++, args.keywordName(i));
        tuple->initializeAt(vm, next++, args.keywordValue(i));
    }
    return tuple;
}

static JSValue classOfJSNew(JSGlobalObject* globalObject, JSValue constructor, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyType* cls = args.isEmpty() ? nullptr : PyType::tryFrom(args[0])) {
        PyType* type = typeForClassOfJS(globalObject, asObject(constructor));
        RETURN_IF_EXCEPTION(scope, { });
        if (!cls->isSubtypeOf(type))
            return raiseTypeError(globalObject, scope, type->name(), ".__new__("_s, cls->name(), "): "_s, cls->name(), " is not a subtype of "_s, type->name());
        // The constructor of the class of JS's that comes first calls the others.
        if (PyType* first = firstClassOfJS(cls); first != type)
            return raiseTypeError(globalObject, scope, type->name(), ".__new__("_s, cls->name(), ") is not safe"_s, first ? ", use "_s : ""_s, first ? first->name() : String(), first ? ".__new__()"_s : ""_s);
    }
    JSValue object = jsClassNew(globalObject, constructor, args);
    RETURN_IF_EXCEPTION(scope, { });
    if (object.isObject() && !isInitOfClassOfJS(PyType::from(args[0])->lookup(Special::Init)))
        argumentsOfConstructors(globalObject)->set(vm, asObject(object), tupleOfArguments(globalObject, args.withoutFirst()));
    return object;
}

static JSValue classOfJSInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!self.isObject())
        return jsUndefined();
    JSWeakMap* map = argumentsOfConstructors(globalObject);
    JSValue remembered = map->get(asObject(self));
    if (!remembered.isCell())
        return jsUndefined();
    map->remove(asObject(self));
    PyTuple* given = tupleOfArguments(globalObject, args);
    PyTuple* constructed = PyTuple::from(remembered);
    bool isSame = given->length() == constructed->length();
    for (unsigned i = 0; isSame && i < given->length(); ++i)
        isSame = given->at(i) == constructed->at(i) || (given->at(i).isString() && constructed->at(i).isString() && stringOf(asString(given->at(i))) == stringOf(asString(constructed->at(i))));
    if (isSame)
        return jsUndefined();
    return raiseTypeError(globalObject, scope, "__init__() got other arguments than the constructor, which has run: a class of JavaScript is constructed by __new__, with the arguments of the call. To give it others, define __new__(cls, ...) and call super().__new__(cls, ...)"_s);
}

static constexpr NativeDef classOfJSNewDef = PY_FUNCTION("__new__", classOfJSNew, "($self, *args, **kwargs)");
static constexpr NativeDef classOfJSInitDef = PY_METHOD("__init__", classOfJSInit, "($self, *args, **kwargs)");

// A hidden field of the type that stands for the class. (Its __new__ knows the constructor too, but can be deleted.)
static constexpr unsigned constructorField = 0;

static JSObject* constructorOfClassOfJS(PyType* type)
{
    ASSERT(prototypeOfClassOfJS(type));
    return asObject(hiddenField(type->realm(), type, constructorField));
}

JSValue staticMemberOfClassOfJS(JSGlobalObject* globalObject, PyType* type, JSString* name)
{
    PyType* first = firstClassOfJS(type);
    if (!first)
        return { };
    // Every function has these, and the class has its own.
    String text = stringOf(name);
    if (text == "name"_s || text == "length"_s || text == "prototype"_s)
        return { };
    return attributeFromPrototypeOfJS(globalObject, constructorOfClassOfJS(first), type, name);
}

// To JS a class inherits from the class it extends: that is where it finds Symbol.species, which has no name in Python.
// `self`: the type that has this method. It begins with super().__init_subclass__(**kwargs).
static JSValue jsClassInitSubclass(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* cls = args.isEmpty() ? nullptr : PyType::tryFrom(args[0]);
    if (!cls)
        return raiseTypeError(globalObject, scope, "__init_subclass__() is a method of classes"_s);
    JSValue next;
    if (PyTuple* mro = cls->mro()) {
        unsigned i = 0;
        while (i < mro->length() && mro->at(i) != self)
            ++i;
        for (++i; i < mro->length() && !next; ++i)
            next = PyType::from(mro->at(i))->dict()->get(rt.name(Special::InitSubclass));
    }
    JSValue inherited = bindDescriptor(globalObject, next ? next : rt.objectType()->dict()->get(rt.name(Special::InitSubclass)), { }, cls);
    RETURN_IF_EXCEPTION(scope, { });
    call(globalObject, inherited, args.withoutFirst());
    RETURN_IF_EXCEPTION(scope, { });
    if (PyType* base = cls->base())
        cls->setPrototypeDirect(vm, prototypeOfClassOfJS(base) ? constructorOfClassOfJS(base) : static_cast<JSObject*>(base));
    return jsUndefined();
}
static constexpr NativeDef jsClassInitSubclassDef = PY_FUNCTION("__init_subclass__", jsClassInitSubclass, "($self, *args, **kwargs)");

PyType* typeForClassOfJS(JSGlobalObject* globalObject, JSObject* constructor)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = *vm.propertyNames;
    // A constructor that is a plain function has been bound to what it was read off, as a method is.
    while (auto* bound = dynamicDowncast<JSBoundFunction>(constructor))
        constructor = bound->targetFunction();

    JSWeakMap* types;
    if (JSValue map = rt.slot(Runtime::Slot::TypesOfJSClasses))
        types = uncheckedDowncast<JSWeakMap>(map);
    else {
        types = JSWeakMap::create(vm, globalObject->weakMapStructure());
        rt.setSlot(Runtime::Slot::TypesOfJSClasses, types);
    }
    if (JSValue known = types->get(constructor); known.isCell())
        return PyType::from(known);

    auto refuse = [&](ASCIILiteral why) -> PyType* {
        String text = reprForMessage(globalObject, constructor);
        RETURN_IF_EXCEPTION(scope, nullptr);
        raiseTypeError(globalObject, scope, "cannot derive a class from "_s, text, ": "_s, why);
        return nullptr;
    };
    if (!constructor->isConstructor())
        return refuse("it is not a constructor"_s);
    JSValue prototype = constructor->get(globalObject, names.prototype);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!prototype.isObject())
        return refuse("it has no prototype"_s);
    // To Python their instances are not objects of JS.
    JSGlobalObject* realm = constructor->realm();
    for (JSValue object = prototype; object.isObject();) {
        if (object == JSValue(realm->arrayPrototype()))
            return refuse("an array is a list"_s);
        if (object == JSValue(realm->functionPrototype()) || object == JSValue(realm->stringPrototype()) || object == JSValue(realm->numberPrototype())
            || object == JSValue(realm->booleanPrototype()) || object == JSValue(realm->symbolPrototype()) || object == JSValue(realm->bigIntPrototype()))
            return refuse("its instances are no plain objects"_s);
        object = asObject(object)->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    JSValue parent = constructor->getPrototype(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyType* base;
    bool extendsClassOfJS = parent.isObject() && asObject(parent)->isConstructor() && !PyType::is(parent);
    if (extendsClassOfJS) {
        base = typeForClassOfJS(globalObject, asObject(parent));
        RETURN_IF_EXCEPTION(scope, nullptr);
    } else if (constructor == realm->promiseConstructor())
        base = rt.jsPromiseType();
    else if (prototype == JSValue(realm->mapPrototype()))
        base = rt.jsMapType();
    else
        base = constructor->inherits<JSTypedArrayViewConstructor>() ? jsTypedArrayType(rt) : rt.jsObjectType();

    JSValue nameValue = constructor->get(globalObject, names.name);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyDict* ns = PyDict::create(globalObject);
    ns->set(vm, rt.names.dModule(), internString(vm, "js"_s));
    ns->set(vm, rt.names.dSlots(), PyTuple::create(globalObject, 0));
    ns->set(vm, rt.name(Special::New), PyNativeFunction::create(globalObject, classOfJSNewDef, NativeKind::Function, constructor, { }));
    ns->set(vm, rt.name(Special::Init), PyNativeFunction::create(globalObject, classOfJSInitDef, NativeKind::Method, { }, rt.objectType()));
    JSValue made = call(globalObject, rt.typeType(), nameValue.isString() ? nameValue : JSValue(jsEmptyString(vm)), PyTuple::create(globalObject, { base }), ns);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyType* type = PyType::from(made);
    if (!extendsClassOfJS) {
        addMethodsOfDerivedClasses(globalObject, type);
        JSValue initSubclass = makeClassMethod(globalObject, PyNativeFunction::create(globalObject, jsClassInitSubclassDef, NativeKind::Function, type, { }));
        RETURN_IF_EXCEPTION(scope, nullptr);
        type->dict()->set(vm, rt.name(Special::InitSubclass), initSubclass);
        type->didChange(vm);
    }
    // An instance is made by one class of JS.
    type->setSolidBase(vm, type);
    type->setJSPrototype(vm, asObject(prototype));
    setHiddenField(globalObject, type, constructorField, constructor);
    types->set(vm, constructor, type);
    return type;
}

void setNameOfDerivedClass(JSGlobalObject*, JSCell* cell, JSValue name)
{
    PyType* type = PyType::tryFrom(cell);
    if (!type || !name.isString() || type->nameString()->length())
        return;
    VM& vm = type->vm();
    JSString* interned = internString(type->realm(), asString(name));
    type->setNameString(vm, interned);
    type->setQualname(vm, interned);
}

JSC_DEFINE_HOST_FUNCTION(deriveClassFromJSFunction, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSValue::encode(deriveClassFromJS(globalObject, callFrame->argument(0), callFrame->argument(1), callFrame->argument(2)));
}

} // namespace Python
} // namespace JSC
