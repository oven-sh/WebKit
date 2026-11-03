/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyDescriptors.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTypes.h"

namespace JSC {
namespace Python {

// property, classmethod, staticmethod, super, mappingproxy, and the descriptors of attributes implemented in C++.
// (Objects/descrobject.c, funcobject.c and typeobject.c in CPython are the reference.)

PY_DEFINE_CELL(PyProperty);
PY_DEFINE_CELL(PyWrapperObject);
PY_DEFINE_CELL(PySuper);
PY_DEFINE_CELL(PyGetSetDescriptor);
PY_DEFINE_CELL(PyMemberDescriptor);

// ---- property ----

PyProperty::PyProperty(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyProperty* PyProperty::create(VM& vm, Structure* structure, PyType* type)
{
    auto* property = new (NotNull, allocateCell<PyProperty>(vm)) PyProperty(vm, structure, type);
    property->finishCreation(vm);
    property->getter.setWithoutWriteBarrier(jsUndefined());
    property->setter.setWithoutWriteBarrier(jsUndefined());
    property->deleter.setWithoutWriteBarrier(jsUndefined());
    property->doc.setWithoutWriteBarrier(jsUndefined());
    return property;
}

template<typename Visitor>
void PyProperty::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyProperty>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->getter);
    visitor.append(thisObject->setter);
    visitor.append(thisObject->deleter);
    visitor.append(thisObject->doc);
    visitor.append(thisObject->name);
}

DEFINE_VISIT_CHILDREN(PyProperty);

static JSValue propertyNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    return PyProperty::create(rt.vm(), rt.structure<PyProperty>(), PyType::from(cls));
}

static JSValue propertyInit(JSGlobalObject* globalObject, JSValue self, JSValue getter, JSValue setter, JSValue deleter, JSValue doc)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* property = PyProperty::from(self);
    property->getter.set(vm, property, getter ? getter : jsUndefined());
    property->setter.set(vm, property, setter ? setter : jsUndefined());
    property->deleter.set(vm, property, deleter ? deleter : jsUndefined());
    property->name.clear();
    property->docIsFromGetter = false;

    // Without a docstring of its own, it has that of the getter.
    JSValue propertyDoc = doc && !isNone(doc) ? doc : JSValue();
    if (!propertyDoc && getter && !isNone(getter)) {
        propertyDoc = getAttrIfPresent(globalObject, getter, rt.names.dDoc());
        RETURN_IF_EXCEPTION(scope, { });
        if (propertyDoc && isNone(propertyDoc))
            propertyDoc = JSValue();
        if (propertyDoc)
            property->docIsFromGetter = true;
    }
    if (property->type() == rt.propertyType()) {
        property->doc.set(vm, property, propertyDoc ? propertyDoc : jsUndefined());
        return jsUndefined();
    }
    // In a subclass, __doc__ is an ordinary attribute. One with __slots__ may have no place for it: that is not an error, unless
    // it is the getter's docstring that is lost.
    setAttr(globalObject, self, rt.names.dDoc(), propertyDoc ? propertyDoc : jsUndefined());
    if (scope.exception() && (property->docIsFromGetter || !clearPendingExceptionIfMatches(globalObject, rt.AttributeError()))) [[unlikely]]
        return { };
    return jsUndefined();
}

// Empty with no exception: it is not known.
static JSValue propertyName(JSGlobalObject* globalObject, PyProperty* property)
{
    if (JSValue name = property->name.get())
        return name;
    if (isNone(property->getter.get()))
        return { };
    return getAttrIfPresent(globalObject, property->getter.get(), runtime(globalObject).names.dName());
}

static JSValue raiseNoAccessor(JSGlobalObject* globalObject, PyProperty* property, JSValue object, ASCIILiteral what)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue name = propertyName(globalObject, property);
    RETURN_IF_EXCEPTION(scope, { });
    String owner = strRepr(stringOf(rt.typeOf(object)->qualname()));
    if (name)
        return raise(globalObject, scope, rt.AttributeError(), makeString("property "_s, reprForMessage(globalObject, name), " of "_s, owner, " object has no "_s, what));
    return raise(globalObject, scope, rt.AttributeError(), makeString("property of "_s, owner, " object has no "_s, what));
}

static JSValue propertyGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue)
{
    if (isNone(instance))
        return self;
    auto* property = PyProperty::from(self);
    JSValue getter = property->getter.get();
    if (isNone(getter))
        return raiseNoAccessor(globalObject, property, instance, "getter"_s);
    return call(globalObject, getter, instance);
}

static JSValue propertySet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* property = PyProperty::from(self);
    JSValue setter = property->setter.get();
    if (isNone(setter))
        RELEASE_AND_RETURN(scope, raiseNoAccessor(globalObject, property, instance, "setter"_s));
    call(globalObject, setter, instance, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue propertyDelete(JSGlobalObject* globalObject, JSValue self, JSValue instance)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* property = PyProperty::from(self);
    JSValue deleter = property->deleter.get();
    if (isNone(deleter))
        RELEASE_AND_RETURN(scope, raiseNoAccessor(globalObject, property, instance, "deleter"_s));
    call(globalObject, deleter, instance);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

// A copy with one of the three functions replaced.
static JSValue propertyCopy(JSGlobalObject* globalObject, JSValue self, JSValue getter, JSValue setter, JSValue deleter)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* old = PyProperty::from(self);
    auto choose = [](JSValue given, JSValue existing) { return !given || isNone(given) ? existing : given; };
    getter = choose(getter, old->getter.get());
    setter = choose(setter, old->setter.get());
    deleter = choose(deleter, old->deleter.get());
    JSValue doc = old->docIsFromGetter && !isNone(getter) ? jsUndefined() : old->doc.get();
    JSValue result = call(globalObject, old->type(), getter, setter, deleter, doc);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto* copy = PyProperty::tryFrom(result))
        copy->name.set(vm, copy, old->name.get());
    return result;
}

static JSValue propertyGetter(JSGlobalObject* globalObject, JSValue self, JSValue function) { return propertyCopy(globalObject, self, function, { }, { }); }
static JSValue propertySetter(JSGlobalObject* globalObject, JSValue self, JSValue function) { return propertyCopy(globalObject, self, { }, function, { }); }
static JSValue propertyDeleter(JSGlobalObject* globalObject, JSValue self, JSValue function) { return propertyCopy(globalObject, self, { }, { }, function); }

static JSValue propertySetName(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "property.__set_name__"_s, args)))
        return { };
    if (args.size() != 2)
        return raiseTypeError(globalObject, scope, "__set_name__() takes 2 positional arguments but "_s, args.size(), " were given"_s);
    auto* property = PyProperty::from(self);
    property->name.set(vm, property, args[1]);
    return jsUndefined();
}

static JSValue propertyGetFGet(JSGlobalObject*, JSValue self) { return PyProperty::from(self)->getter.get(); }
static JSValue propertyGetFSet(JSGlobalObject*, JSValue self) { return PyProperty::from(self)->setter.get(); }
static JSValue propertyGetFDel(JSGlobalObject*, JSValue self) { return PyProperty::from(self)->deleter.get(); }
static JSValue propertyGetDoc(JSGlobalObject*, JSValue self) { return PyProperty::from(self)->doc.get(); }

static void propertySetDoc(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    auto* property = PyProperty::from(self);
    property->doc.set(getVM(globalObject), property, value ? value : jsUndefined());
}

static JSValue propertyGetNameAttribute(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue name = propertyName(globalObject, PyProperty::from(self));
    RETURN_IF_EXCEPTION(scope, { });
    if (!name)
        return raise(globalObject, scope, rt.AttributeError(), "'property' object has no attribute '__name__'"_s);
    return name;
}

static void propertySetNameAttribute(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    auto* property = PyProperty::from(self);
    property->name.set(getVM(globalObject), property, value);
}

// Whether any of the values says it is abstract.
static JSValue anyIsAbstract(JSGlobalObject* globalObject, std::initializer_list<JSValue> values)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (JSValue value : values) {
        JSValue flag = getAttrIfPresent(globalObject, value, rt.names.dIsAbstractMethod());
        RETURN_IF_EXCEPTION(scope, { });
        if (!flag)
            continue;
        bool isAbstract = isTrue(globalObject, flag);
        RETURN_IF_EXCEPTION(scope, { });
        if (isAbstract)
            return jsBoolean(true);
    }
    return jsBoolean(false);
}

static JSValue propertyGetIsAbstract(JSGlobalObject* globalObject, JSValue self)
{
    auto* property = PyProperty::from(self);
    return anyIsAbstract(globalObject, { property->getter.get(), property->setter.get(), property->deleter.get() });
}

static constexpr NativeDef propertyMethods[] = {
    PY_NEW(propertyNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", propertyInit, "($self, /, fget=None, fset=None, fdel=None, doc=None)"),
    PY_SLOT("__get__", propertyGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__set__", propertySet, "($self, instance, value, /)"),
    PY_SLOT("__delete__", propertyDelete, "($self, instance, /)"),
    PY_METHOD("getter", propertyGetter, "($self, object, /)"),
    PY_METHOD("setter", propertySetter, "($self, object, /)"),
    PY_METHOD("deleter", propertyDeleter, "($self, object, /)"),
    PY_METHOD("__set_name__", propertySetName, "!($self, owner, name, /)"),
};

static constexpr NativeGetSet propertyGetSets[] = {
    PY_MEMBER("fget", propertyGetFGet),
    PY_MEMBER("fset", propertyGetFSet),
    PY_MEMBER("fdel", propertyGetFDel),
    PY_WRITABLE_MEMBER("__doc__", propertyGetDoc, propertySetDoc),
    { "__name__"_s, propertyGetNameAttribute, propertySetNameAttribute },
    { "__isabstractmethod__"_s, propertyGetIsAbstract },
};

// ---- classmethod, staticmethod ----

PyWrapperObject::PyWrapperObject(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyWrapperObject* PyWrapperObject::create(VM& vm, Structure* structure, PyType* type, JSValue wrapped)
{
    auto* wrapper = new (NotNull, allocateCell<PyWrapperObject>(vm)) PyWrapperObject(vm, structure, type);
    wrapper->finishCreation(vm);
    wrapper->m_wrapped.set(vm, wrapper, wrapped);
    return wrapper;
}

PyWrapperObject* PyWrapperObject::create(JSGlobalObject* globalObject, PyType* type, JSValue wrapped)
{
    Runtime& rt = runtime(globalObject);
    return create(rt.vm(), rt.structure<PyWrapperObject>(), type, wrapped);
}

template<typename Visitor>
void PyWrapperObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyWrapperObject>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_wrapped);
    visitor.append(thisObject->m_dict);
}

DEFINE_VISIT_CHILDREN(PyWrapperObject);

// As functools.wraps(): the wrapper passes for what it wraps. False: an exception.
static bool wrapperSetCallable(JSGlobalObject* globalObject, PyWrapperObject* wrapper, JSValue function)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    wrapper->setWrapped(vm, function);
    for (JSString* name : { rt.names.dModule(), rt.names.dName(), rt.names.dQualname(), rt.names.dDoc() }) {
        JSValue value = getAttrIfPresent(globalObject, function, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (!value)
            continue;
        setAttr(globalObject, wrapper, name, value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

void copyAttributesOfWrapped(JSGlobalObject* globalObject, PyWrapperObject* wrapper)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    wrapper->setCopiesAttributesLater(false);
    if (failed(scope, wrapperSetCallable(globalObject, wrapper, wrapper->wrapped())))
        takePendingException(globalObject);
}

JSValue makeStaticMethod(JSGlobalObject* globalObject, JSValue function)
{
    auto* wrapper = PyWrapperObject::create(globalObject, runtime(globalObject).staticmethodType(), jsUndefined());
    return wrapperSetCallable(globalObject, wrapper, function) ? JSValue(wrapper) : JSValue();
}

JSValue makeClassMethod(JSGlobalObject* globalObject, JSValue function)
{
    auto* wrapper = PyWrapperObject::create(globalObject, runtime(globalObject).classmethodType(), jsUndefined());
    return wrapperSetCallable(globalObject, wrapper, function) ? JSValue(wrapper) : JSValue();
}

// Both __new__ and __init__ take the function.
template<bool isStatic>
static bool wrapperCheckArguments(JSGlobalObject* globalObject, const CallArgs& args)
{
    constexpr ASCIILiteral name = isStatic ? "staticmethod"_s : "classmethod"_s;
    return rejectKeywords(globalObject, name, args) && checkPositional(globalObject, name, args, 1, 1);
}

template<bool isStatic>
static JSValue wrapperNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, wrapperCheckArguments<isStatic>(globalObject, args)))
        return { };
    auto* wrapper = PyWrapperObject::create(globalObject, PyType::from(cls), jsUndefined());
    RELEASE_AND_RETURN(scope, wrapperSetCallable(globalObject, wrapper, args[0]) ? JSValue(wrapper) : JSValue());
}

template<bool isStatic>
static JSValue wrapperInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, wrapperCheckArguments<isStatic>(globalObject, args)))
        return { };
    RELEASE_AND_RETURN(scope, wrapperSetCallable(globalObject, PyWrapperObject::from(self), args[0]) ? jsUndefined() : JSValue());
}

static JSValue classmethodGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue owner)
{
    JSValue cls = owner && !isNone(owner) ? owner : JSValue(typeOf(globalObject, instance));
    return PyMethod::create(globalObject, PyWrapperObject::from(self)->wrapped(), cls);
}

static JSValue staticmethodGet(JSGlobalObject*, JSValue self, JSValue, JSValue) { return PyWrapperObject::from(self)->wrapped(); }
static JSValue staticmethodCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args) { return call(globalObject, PyWrapperObject::from(self)->wrapped(), args); }

static JSValue wrapperRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* wrapper = PyWrapperObject::from(self);
    JSString* text = repr(globalObject, wrapper->wrapped());
    RETURN_IF_EXCEPTION(scope, { });
    String string = text->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString('<', wrapper->type()->name(), '(', string, ")>"_s));
}

static JSValue wrapperGetWrapped(JSGlobalObject*, JSValue self) { return PyWrapperObject::from(self)->wrapped(); }
static JSValue wrapperGetIsAbstract(JSGlobalObject* globalObject, JSValue self) { return anyIsAbstract(globalObject, { PyWrapperObject::from(self)->wrapped() }); }

// __annotations__ and __annotate__ are those of what is wrapped, unless they have been set. Once asked for, they are kept.
static JSValue wrapperGetFromWrapped(JSGlobalObject* globalObject, JSValue self, JSString* name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = instanceDict(globalObject, self, true);
    if (JSValue value = dict->get(name))
        return value;
    JSValue value = getAttr(globalObject, PyWrapperObject::from(self)->wrapped(), name);
    RETURN_IF_EXCEPTION(scope, { });
    dict->set(vm, name, value);
    return value;
}

static void wrapperSetInDict(JSGlobalObject* globalObject, JSValue self, JSString* name, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = instanceDict(globalObject, self, true);
    if (value)
        dict->set(vm, name, value);
    else if (!dict->remove(vm, name)) {
        bool isStatic = rt.typeOf(self)->isSubtypeOf(rt.staticmethodType());
        raise(globalObject, scope, rt.AttributeError(), makeString('\'', isStatic ? "staticmethod"_s : "classmethod"_s, "' object has no attribute '"_s, stringOf(name), '\''));
    }
}

static JSValue wrapperGetAnnotations(JSGlobalObject* globalObject, JSValue self) { return wrapperGetFromWrapped(globalObject, self, runtime(globalObject).names.dAnnotations()); }
static void wrapperSetAnnotations(JSGlobalObject* globalObject, JSValue self, JSValue value) { wrapperSetInDict(globalObject, self, runtime(globalObject).names.dAnnotations(), value); }
static JSValue wrapperGetAnnotate(JSGlobalObject* globalObject, JSValue self) { return wrapperGetFromWrapped(globalObject, self, runtime(globalObject).names.dAnnotate()); }
static void wrapperSetAnnotate(JSGlobalObject* globalObject, JSValue self, JSValue value) { wrapperSetInDict(globalObject, self, runtime(globalObject).names.dAnnotate(), value); }

static constexpr NativeDef classmethodMethods[] = {
    PY_NEW(wrapperNew<false>, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", wrapperInit<false>, "($self, *args, **kwargs)"),
    PY_SLOT("__get__", classmethodGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__repr__", wrapperRepr, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeDef staticmethodMethods[] = {
    PY_NEW(wrapperNew<true>, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", wrapperInit<true>, "($self, *args, **kwargs)"),
    PY_SLOT("__get__", staticmethodGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__call__", staticmethodCall, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", wrapperRepr, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet wrapperGetSets[] = {
    PY_MEMBER("__func__", wrapperGetWrapped),
    PY_MEMBER("__wrapped__", wrapperGetWrapped),
    { "__isabstractmethod__"_s, wrapperGetIsAbstract },
    { "__annotations__"_s, wrapperGetAnnotations, wrapperSetAnnotations },
    { "__annotate__"_s, wrapperGetAnnotate, wrapperSetAnnotate },
};

// ---- mappingproxy ----

JSValue makeMappingProxy(JSGlobalObject* globalObject, JSValue mapping)
{
    return PyWrapperObject::create(globalObject, runtime(globalObject).mappingProxyType(), mapping);
}

static ALWAYS_INLINE JSValue proxied(JSValue self) { return PyWrapperObject::from(self)->wrapped(); }

static JSValue mappingProxyNew(JSGlobalObject* globalObject, JSValue, JSValue mapping)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!typeOf(globalObject, mapping)->lookup(Special::GetItem) || isList(mapping) || PyTuple::is(mapping))
        return raiseTypeError(globalObject, scope, "mappingproxy() argument must be a mapping, not "_s, typeName(globalObject, mapping));
    RELEASE_AND_RETURN(scope, makeMappingProxy(globalObject, mapping));
}

static JSValue mappingProxyLen(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = length(globalObject, proxied(self));
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(static_cast<double>(size));
}

static JSValue mappingProxyGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key) { return getItem(globalObject, proxied(self), key); }
static JSValue mappingProxyIter(JSGlobalObject* globalObject, JSValue self) { return getIter(globalObject, proxied(self)); }
static JSValue mappingProxyStr(JSGlobalObject* globalObject, JSValue self) { return str(globalObject, proxied(self)); }

static JSValue mappingProxyContains(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool found = contains(globalObject, proxied(self), key);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

static JSValue mappingProxyRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* text = repr(globalObject, proxied(self));
    RETURN_IF_EXCEPTION(scope, { });
    String string = text->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString("mappingproxy("_s, string, ')'));
}

template<CompareOperator op>
static JSValue mappingProxyCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    if constexpr (op != CompareOperator::Eq && op != CompareOperator::NotEq)
        return rt.notImplemented();
    JSValue mapping = proxied(self);
    // The __eq__ of the other gets to see what it is compared with. A dict that the proxy is there to protect (the namespace of a
    // class, for one) must not get out that way: what gets out is a copy.
    if (auto* dict = PyDict::tryFrom(mapping); dict && dict->type() == rt.dictType()) {
        PyType* otherType = rt.typeOf(other);
        if (otherType != rt.dictType() && otherType != rt.frozendictType() && otherType != rt.mappingProxyType() && otherType != orderedDictType(globalObject))
            mapping = dict->copy(globalObject);
    }
    return richCompare(globalObject, mapping, other, op);
}

static JSValue mappingProxyHash(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t result = hash(globalObject, proxied(self));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeInt(globalObject, result));
}

static JSValue unwrapProxy(JSGlobalObject* globalObject, JSValue value)
{
    auto* wrapper = PyWrapperObject::tryFrom(value);
    return wrapper && wrapper->type() == runtime(globalObject).mappingProxyType() ? wrapper->wrapped() : value;
}

static JSValue mappingProxyOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return binaryOperation(globalObject, BinaryOperator::BitOr, unwrapProxy(globalObject, self), unwrapProxy(globalObject, other)); }
static JSValue mappingProxyROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return binaryOperation(globalObject, BinaryOperator::BitOr, unwrapProxy(globalObject, other), unwrapProxy(globalObject, self)); }

static JSValue mappingProxyIOr(JSGlobalObject* globalObject, JSValue self, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "'|=' is not supported by "_s, typeName(globalObject, self), "; use '|' instead"_s);
}

static JSValue mappingProxyGet(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    return callMethod(globalObject, proxied(self), runtime(globalObject).names.get(), key, defaultValue ? defaultValue : jsUndefined());
}

static JSValue mappingProxyKeys(JSGlobalObject* globalObject, JSValue self) { return callMethod(globalObject, proxied(self), runtime(globalObject).names.keys()); }
static JSValue mappingProxyValues(JSGlobalObject* globalObject, JSValue self) { return callMethod(globalObject, proxied(self), runtime(globalObject).names.values()); }
static JSValue mappingProxyItems(JSGlobalObject* globalObject, JSValue self) { return callMethod(globalObject, proxied(self), runtime(globalObject).names.items()); }
static JSValue mappingProxyCopy(JSGlobalObject* globalObject, JSValue self) { return callMethod(globalObject, proxied(self), runtime(globalObject).names.copy()); }
static JSValue mappingProxyReversed(JSGlobalObject* globalObject, JSValue self) { return callMethod(globalObject, proxied(self), runtime(globalObject).name(Special::Reversed)); }

static constexpr NativeDef mappingProxyMethods[] = {
    PY_NEW(mappingProxyNew, "($type, /, mapping)"),
    PY_SLOT("__len__", mappingProxyLen, "($self, /)"),
    PY_SLOT("__getitem__", mappingProxyGetItem, "($self, key, /)"),
    PY_SLOT("__contains__", mappingProxyContains, "($self, key, /)"),
    PY_SLOT("__iter__", mappingProxyIter, "($self, /)"),
    PY_SLOT("__repr__", mappingProxyRepr, "($self, /)"),
    PY_SLOT("__str__", mappingProxyStr, "($self, /)"),
    PY_SLOT("__hash__", mappingProxyHash, "($self, /)"),
    PY_SLOT("__eq__", mappingProxyCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", mappingProxyCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", mappingProxyCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", mappingProxyCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", mappingProxyCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", mappingProxyCompare<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__or__", mappingProxyOr, "($self, value, /)"),
    PY_SLOT("__ror__", mappingProxyROr, "($self, value, /)"),
    PY_SLOT("__ior__", mappingProxyIOr, "($self, value, /)"),
    PY_METHOD("get", mappingProxyGet, "($self, key, default=None, /)"),
    PY_METHOD("keys", mappingProxyKeys, "($self, /)"),
    PY_METHOD("values", mappingProxyValues, "($self, /)"),
    PY_METHOD("items", mappingProxyItems, "($self, /)"),
    PY_METHOD("copy", mappingProxyCopy, "($self, /)"),
    PY_METHOD("__reversed__", mappingProxyReversed, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

// ---- super ----

PySuper::PySuper(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PySuper* PySuper::create(VM& vm, Structure* structure, PyType* type)
{
    auto* object = new (NotNull, allocateCell<PySuper>(vm)) PySuper(vm, structure, type);
    object->finishCreation(vm);
    return object;
}

template<typename Visitor>
void PySuper::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PySuper>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->thisClass);
    visitor.append(thisObject->object);
    visitor.append(thisObject->objectType);
}

DEFINE_VISIT_CHILDREN(PySuper);

static JSValue superNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    return PySuper::create(rt.vm(), rt.structure<PySuper>(), PyType::from(cls));
}

// The class whose MRO super(type, object) searches: that of the object, or the object if it is itself a subclass.
static PyType* superCheck(JSGlobalObject* globalObject, PyType* type, JSValue object)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* objectAsType = PyType::tryFrom(object); objectAsType && objectAsType->isSubtypeOf(type))
        return objectAsType;
    PyType* objectType = rt.typeOf(object);
    if (objectType->isSubtypeOf(type))
        return objectType;
    // A proxy may claim another class.
    JSValue claimed = getAttrIfPresent(globalObject, object, rt.names.dClass());
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (auto* claimedType = PyType::tryFrom(claimed); claimedType && claimedType != objectType && claimedType->isSubtypeOf(type))
        return claimedType;
    raiseTypeError(globalObject, scope, "super(type, obj): obj ("_s, PyType::is(object) ? "type"_s : "instance of"_s, ' ', PyType::is(object) ? PyType::from(object)->name() : objectType->name(), ") is not an instance or subtype of type ("_s, type->name(), ")."_s);
    return nullptr;
}

static JSValue superInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "super"_s, args)) || failed(scope, checkPositional(globalObject, "super()"_s, args, 0, 2)))
        return { };
    auto* object = PySuper::from(self);

    JSValue typeValue = args.get(0);
    JSValue instance = args.get(1);
    if (!typeValue) {
        // super(): the class being defined and the first argument of the method that is running.
        if (!findSuperArguments(globalObject, typeValue, instance))
            return { };
    }
    PyType* type = PyType::tryFrom(typeValue);
    if (!type)
        return raiseTypeError(globalObject, scope, "super() argument 1 must be a type, not "_s, typeName(globalObject, typeValue));
    PyType* objectType = nullptr;
    if (instance && isNone(instance))
        instance = JSValue();
    if (instance) {
        objectType = superCheck(globalObject, type, instance);
        RETURN_IF_EXCEPTION(scope, { });
    }
    object->thisClass.set(vm, object, type);
    object->object.set(vm, object, instance);
    object->objectType.setMayBeNull(vm, object, objectType);
    return jsUndefined();
}

static JSValue superGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = PySuper::from(self);
    PyType* startType = object->objectType.get();
    // super().__class__ is super itself.
    // (A class has no MRO yet while the mro() of its metaclass runs.)
    if (startType && startType->mro() && !WTF::equal(name->tryGetValueImpl(), rt.names.dClass()->tryGetValueImpl())) {
        PyTuple* mro = startType->mro();
        unsigned count = mro->length();
        unsigned i = 0;
        // What follows the class in the MRO.
        while (i + 1 < count && mro->at(i).asCell() != object->thisClass.get())
            ++i;
        for (++i; i < count; ++i) {
            JSValue found = PyType::from(mro->at(i))->dict()->get(name);
            if (!found)
                continue;
            JSValue instance = object->object.get();
            RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, found, instance == JSValue(startType) ? JSValue() : instance, startType));
        }
    }
    RELEASE_AND_RETURN(scope, genericGetAttr(globalObject, self, name));
}

static JSValue superGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PySuper::from(self);
    if (isNone(instance) || object->object)
        return self;
    // super.__new__(super)
    if (!object->thisClass)
        return raiseTypeError(globalObject, scope, "super(type, obj): the super object was not initialized"_s);
    if (object->type() != rt.superType())
        RELEASE_AND_RETURN(scope, call(globalObject, object->type(), object->thisClass.get(), instance));
    PyType* objectType = superCheck(globalObject, object->thisClass.get(), instance);
    RETURN_IF_EXCEPTION(scope, { });
    PySuper* bound = PySuper::create(vm, rt.structure<PySuper>(), rt.superType());
    bound->thisClass.set(vm, bound, object->thisClass.get());
    bound->object.set(vm, bound, instance);
    bound->objectType.set(vm, bound, objectType);
    return bound;
}

static JSValue superRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* object = PySuper::from(self);
    String className = object->thisClass ? object->thisClass->name() : "NULL"_str;
    if (object->objectType)
        return makeStr(globalObject, makeString("<super: <class '"_s, className, "'>, <"_s, object->objectType->name(), " object>>"_s));
    return makeStr(globalObject, makeString("<super: <class '"_s, className, "'>, NULL>"_s));
}

static JSValue superGetThisClass(JSGlobalObject*, JSValue self)
{
    PyType* type = PySuper::from(self)->thisClass.get();
    return type ? JSValue(type) : jsUndefined();
}
static JSValue superGetSelf(JSGlobalObject*, JSValue self)
{
    JSValue object = PySuper::from(self)->object.get();
    return object ? object : jsUndefined();
}
static JSValue superGetSelfClass(JSGlobalObject*, JSValue self)
{
    PyType* type = PySuper::from(self)->objectType.get();
    return type ? JSValue(type) : jsUndefined();
}

static constexpr NativeDef superMethods[] = {
    PY_NEW(superNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", superInit, "($self, *args, **kwargs)"),
    PY_SLOT("__getattribute__", superGetAttribute, "($self, name, /)"),
    PY_SLOT("__get__", superGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__repr__", superRepr, "($self, /)"),
};

static constexpr NativeGetSet superGetSets[] = {
    PY_MEMBER("__thisclass__", superGetThisClass),
    PY_MEMBER("__self__", superGetSelf),
    PY_MEMBER("__self_class__", superGetSelfClass),
};

// ---- getset_descriptor, member_descriptor ----

PyGetSetDescriptor::PyGetSetDescriptor(VM& vm, Structure* structure, PyType* type, const NativeGetSet& def)
    : Base(vm, structure, type)
    , m_def(&def)
{
}

PyGetSetDescriptor* PyGetSetDescriptor::create(JSGlobalObject* globalObject, const NativeGetSet& def, PyType* owner)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* descriptor = new (NotNull, allocateCell<PyGetSetDescriptor>(vm)) PyGetSetDescriptor(vm, rt.structure<PyGetSetDescriptor>(), def.isMember ? rt.memberDescriptorType() : rt.getSetDescriptorType(), def);
    descriptor->finishCreation(vm);
    descriptor->m_owner.set(vm, descriptor, owner);
    return descriptor;
}

template<typename Visitor>
void PyGetSetDescriptor::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyGetSetDescriptor>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_owner);
}

DEFINE_VISIT_CHILDREN(PyGetSetDescriptor);

PyMemberDescriptor::PyMemberDescriptor(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyMemberDescriptor* PyMemberDescriptor::create(JSGlobalObject* globalObject, JSString* name, PyType* owner)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* descriptor = new (NotNull, allocateCell<PyMemberDescriptor>(vm)) PyMemberDescriptor(vm, rt.structure<PyMemberDescriptor>(), rt.memberDescriptorType());
    descriptor->finishCreation(vm);
    descriptor->m_name.set(vm, descriptor, name);
    descriptor->m_owner.set(vm, descriptor, owner);

    // The value is a property of the instance. Two classes whose instances may become instances of the other (__class__ can be
    // assigned) have the same names in __slots__, as do their bases one by one: so the key goes by the name and by how many
    // classes up the line lay out instances, which also tells a slot from one of the same name in a base class.
    unsigned depth = 0;
    for (PyType* type = owner->base(); type; type = type->base()) {
        type = type->solidBase();
        ++depth;
    }
    JSValue keys = rt.slot(Runtime::Slot::MemberKeys);
    if (!keys) {
        keys = PyDict::create(globalObject);
        rt.setSlot(Runtime::Slot::MemberKeys, keys);
    }
    JSString* keyName = internString(vm, makeString(depth, ':', stringOf(name)));
    JSValue key = PyDict::from(keys)->get(keyName);
    if (!key) {
        key = Symbol::create(vm, PrivateSymbolImpl::create(*stringOf(name).impl()).get());
        PyDict::from(keys)->set(vm, keyName, key);
    }
    descriptor->m_key.set(vm, descriptor, uncheckedDowncast<Symbol>(key.asCell()));
    return descriptor;
}

template<typename Visitor>
void PyMemberDescriptor::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyMemberDescriptor>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_name);
    visitor.append(thisObject->m_owner);
    visitor.append(thisObject->m_key);
}

DEFINE_VISIT_CHILDREN(PyMemberDescriptor);

String descriptorRepr(JSGlobalObject* globalObject, ASCIILiteral kind, StringView name, PyType* owner)
{
    if (owner == runtime(globalObject).objectType())
        return makeString('<', kind, " '"_s, name, "'>"_s);
    return makeString('<', kind, " '"_s, name, "' of '"_s, owner->nameInMessages(), "' objects>"_s);
}

static bool checkDescriptorInstance(JSGlobalObject* globalObject, PyType* owner, const String& name, JSValue instance)
{
    if (typeOf(globalObject, instance)->isSubtypeOf(owner)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "descriptor '"_s, name, "' for '"_s, owner->nameInMessages(), "' objects doesn't apply to a '"_s, typeName(globalObject, instance), "' object"_s);
    return false;
}

static JSValue getSetGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(instance))
        return self;
    auto* descriptor = PyGetSetDescriptor::from(self);
    if (failed(scope, checkDescriptorInstance(globalObject, descriptor->owner(), descriptor->def().name, instance)))
        return { };
    RELEASE_AND_RETURN(scope, descriptor->def().getter(globalObject, instance));
}

static JSValue getSetAssign(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* descriptor = PyGetSetDescriptor::from(self);
    if (failed(scope, checkDescriptorInstance(globalObject, descriptor->owner(), descriptor->def().name, instance)))
        return { };
    if (!descriptor->def().setter) {
        if (descriptor->def().isMember)
            return raise(globalObject, scope, rt.AttributeError(), "readonly attribute"_s);
        return raise(globalObject, scope, rt.AttributeError(), makeString("attribute '"_s, descriptor->def().name, "' of '"_s, descriptor->owner()->nameInMessages(), "' objects is not writable"_s));
    }
    descriptor->def().setter(globalObject, instance, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue getSetSet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue value) { return getSetAssign(globalObject, self, instance, value); }
static JSValue getSetDelete(JSGlobalObject* globalObject, JSValue self, JSValue instance) { return getSetAssign(globalObject, self, instance, { }); }

static JSValue getSetRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* descriptor = PyGetSetDescriptor::from(self);
    return makeStr(globalObject, descriptorRepr(globalObject, descriptor->def().isMember ? "member"_s : "attribute"_s, descriptor->def().name, descriptor->owner()));
}

static JSValue getSetGetName(JSGlobalObject* globalObject, JSValue self) { return internString(getVM(globalObject), PyGetSetDescriptor::from(self)->def().name); }
static JSValue getSetGetObjClass(JSGlobalObject*, JSValue self) { return PyGetSetDescriptor::from(self)->owner(); }
static JSValue getSetGetQualname(JSGlobalObject* globalObject, JSValue self)
{
    auto* descriptor = PyGetSetDescriptor::from(self);
    return makeStr(globalObject, makeString(stringOf(descriptor->owner()->qualname()), '.', descriptor->def().name));
}

static JSValue descriptorGetDoc(JSGlobalObject* globalObject, JSValue self)
{
    auto* descriptor = PyGetSetDescriptor::tryFrom(self);
    if (!descriptor)
        return jsUndefined();
    if (!descriptor->def().doc.isNull())
        return makeStr(getVM(globalObject), descriptor->def().doc);
    return docstringFor(globalObject, makeString(descriptor->owner()->nameInMessages(), '.', descriptor->def().name));
}

static constexpr NativeDef getSetMethods[] = {
    PY_SLOT("__get__", getSetGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__set__", getSetSet, "($self, instance, value, /)"),
    PY_SLOT("__delete__", getSetDelete, "($self, instance, /)"),
    PY_SLOT("__repr__", getSetRepr, "($self, /)"),
};

static constexpr NativeGetSet getSetGetSets[] = {
    PY_MEMBER("__name__", getSetGetName),
    { "__qualname__"_s, getSetGetQualname },
    PY_MEMBER("__objclass__", getSetGetObjClass),
    { "__doc__"_s, descriptorGetDoc },
};

// A member_descriptor is one of two cells: that of a name in __slots__, or that of a NativeGetSet that is a member.

static JSValue memberGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue owner)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyGetSetDescriptor::is(self))
        RELEASE_AND_RETURN(scope, getSetGet(globalObject, self, instance, owner));
    if (isNone(instance))
        return self;
    auto* descriptor = PyMemberDescriptor::from(self);
    if (failed(scope, checkDescriptorInstance(globalObject, descriptor->owner(), stringOf(descriptor->name()), instance)))
        return { };
    RELEASE_AND_RETURN(scope, memberDescriptorGet(globalObject, descriptor, instance));
}

static JSValue memberAssign(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue value)
{
    if (PyGetSetDescriptor::is(self))
        return getSetAssign(globalObject, self, instance, value);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* descriptor = PyMemberDescriptor::from(self);
    if (failed(scope, checkDescriptorInstance(globalObject, descriptor->owner(), stringOf(descriptor->name()), instance)))
        return { };
    memberDescriptorSet(globalObject, descriptor, instance, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue memberSet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue value) { return memberAssign(globalObject, self, instance, value); }
static JSValue memberDelete(JSGlobalObject* globalObject, JSValue self, JSValue instance) { return memberAssign(globalObject, self, instance, { }); }

static JSValue memberRepr(JSGlobalObject* globalObject, JSValue self)
{
    if (PyGetSetDescriptor::is(self))
        return getSetRepr(globalObject, self);
    auto* descriptor = PyMemberDescriptor::from(self);
    return makeStr(globalObject, descriptorRepr(globalObject, "member"_s, stringOf(descriptor->name()), descriptor->owner()));
}

static JSValue memberGetName(JSGlobalObject* globalObject, JSValue self) { return PyGetSetDescriptor::is(self) ? getSetGetName(globalObject, self) : JSValue(PyMemberDescriptor::from(self)->name()); }
static JSValue memberGetObjClass(JSGlobalObject* globalObject, JSValue self) { return PyGetSetDescriptor::is(self) ? getSetGetObjClass(globalObject, self) : JSValue(PyMemberDescriptor::from(self)->owner()); }
static JSValue memberGetQualname(JSGlobalObject* globalObject, JSValue self)
{
    if (PyGetSetDescriptor::is(self))
        return getSetGetQualname(globalObject, self);
    auto* descriptor = PyMemberDescriptor::from(self);
    return makeStr(globalObject, makeString(stringOf(descriptor->owner()->qualname()), '.', stringOf(descriptor->name())));
}

static JSValue memberReduce(JSGlobalObject* globalObject, JSValue self)
{
    JSValue getattr = currentBuiltin(globalObject, internString(getVM(globalObject), "getattr"_s));
    if (!getattr)
        return { };
    return PyTuple::create(globalObject, { getattr, PyTuple::create(globalObject, { memberGetObjClass(globalObject, self), memberGetName(globalObject, self) }) });
}

static constexpr NativeDef memberMethods[] = {
    PY_SLOT("__get__", memberGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__set__", memberSet, "($self, instance, value, /)"),
    PY_SLOT("__delete__", memberDelete, "($self, instance, /)"),
    PY_SLOT("__repr__", memberRepr, "($self, /)"),
    PY_METHOD("__reduce__", memberReduce, "($self, /)"),
};

static constexpr NativeGetSet memberGetSets[] = {
    PY_MEMBER("__name__", memberGetName),
    { "__qualname__"_s, memberGetQualname },
    PY_MEMBER("__objclass__", memberGetObjClass),
    { "__doc__"_s, descriptorGetDoc },
};

void initializeDescriptorTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    auto define = [&](PyType* type, ASCIILiteral name, OptionSet<TypeFlag> flags, std::span<const NativeDef> methods, std::span<const NativeGetSet> getsets) {
        type->initializeBuiltin(globalObject, { name, flags, methods, getsets, { } }, rt.typeType(), rt.objectType());
    };
    define(rt.propertyType(), "property"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, propertyMethods, propertyGetSets);
    define(rt.classmethodType(), "classmethod"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasDict }, classmethodMethods, wrapperGetSets);
    rt.classmethodType()->addGetSets(globalObject, dictGetSet());
    define(rt.staticmethodType(), "staticmethod"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasDict }, staticmethodMethods, wrapperGetSets);
    rt.staticmethodType()->addGetSets(globalObject, dictGetSet());
    define(rt.mappingProxyType(), "mappingproxy"_s, { TypeFlag::Mapping }, mappingProxyMethods, { });
    define(rt.superType(), "super"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, superMethods, superGetSets);
    define(rt.getSetDescriptorType(), "getset_descriptor"_s, { }, getSetMethods, getSetGetSets);
    define(rt.memberDescriptorType(), "member_descriptor"_s, { }, memberMethods, memberGetSets);
}

} // namespace Python
} // namespace JSC
