/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyWeakRef.h"

#include "HeapIterationScope.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTypes.h"
#include "SubspaceInlines.h"

namespace JSC {
namespace Python {

// Weak references and the module _weakref. (Objects/weakrefobject.c and Modules/_weakref.c in CPython are the reference.)

PY_DEFINE_CELL(PyWeakRef);

// weakref.ref(object) returns the same reference each time, for as long as the program holds on to it, and so does
// weakref.proxy(object). The object knows them through a hidden field: a weak reference to the one, whose callback field holds a
// weak reference to the other.
static constexpr unsigned sharedWeakRefsField = Runtime::numberOfHiddenKeys - 1;

uint64_t nextWeakRefSerial()
{
    static std::atomic<uint64_t> last;
    return ++last;
}

PyWeakRef::PyWeakRef(VM& vm, Structure* structure, PyType* type, JSCell* referent)
    : Base(vm, structure, type)
    , m_referent(referent)
    , m_serial(nextWeakRefSerial())
{
}

PyWeakRef* PyWeakRef::create(VM& vm, Structure* structure, PyType* type, JSCell* referent, JSValue callback)
{
    auto* weakRef = new (NotNull, allocateCell<PyWeakRef>(vm)) PyWeakRef(vm, structure, type, referent);
    weakRef->finishCreation(vm);
    weakRef->m_callback.set(vm, weakRef, callback);
    weakRef->m_thread = structure->realm()->pythonRuntime()->currentThread();
    return weakRef;
}

template<typename Visitor>
void PyWeakRef::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyWeakRef>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_callback);
}

DEFINE_VISIT_CHILDREN(PyWeakRef);

void PyWeakRef::reconcileWeakReferencesAtGCEnd(VM& vm, CollectionScope)
{
    if (!m_referent || vm.heap.isMarked(m_referent))
        return;
    m_referent = nullptr;
    if (m_callback && !m_isInternal)
        structure()->realm()->pythonRuntime()->queueWeakRefCallback(this);
}

JSValue weakRefGet(JSValue weakRef)
{
    JSCell* referent = PyWeakRef::from(weakRef)->referent();
    return referent ? JSValue(referent) : jsUndefined();
}

static bool supportsWeakRefs(JSGlobalObject* globalObject, JSValue object)
{
    return object.isObject() && typeOf(globalObject, object)->has(TypeFlag::HasWeakref);
}

static PyWeakRef* sharedWeakRefs(JSGlobalObject* globalObject, JSValue object)
{
    JSValue field = hiddenField(globalObject, object, sharedWeakRefsField);
    return field ? PyWeakRef::from(field) : nullptr;
}

static PyWeakRef* newWeakRef(JSGlobalObject* globalObject, PyType* type, JSValue object, JSValue callback)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!supportsWeakRefs(globalObject, object)) {
        raiseTypeError(globalObject, scope, "cannot create weak reference to '"_s, typeName(globalObject, object), "' object"_s);
        return nullptr;
    }
    if (callback && isNone(callback))
        callback = JSValue();

    // One without a callback is as good as another.
    bool isProxy = type == rt.weakProxyType() || type == rt.weakCallableProxyType();
    if (callback || (!isProxy && type != rt.weakRefType()))
        return PyWeakRef::create(vm, rt.weakRefStructure(), type, object.asCell(), callback);

    PyWeakRef* toRef = sharedWeakRefs(globalObject, object);
    if (!toRef) {
        auto makeInternal = [&](JSValue next) {
            PyWeakRef* internal = PyWeakRef::create(vm, rt.weakRefStructure(), rt.weakRefType(), nullptr, next);
            internal->setIsInternal();
            return internal;
        };
        toRef = makeInternal(makeInternal({ }));
        setHiddenField(globalObject, object, sharedWeakRefsField, toRef);
    }
    PyWeakRef* holder = isProxy ? PyWeakRef::from(toRef->callback()) : toRef;
    // (One that has died before its referent is of no use: see Runtime::letWeakRefsDie().)
    if (JSCell* existing = holder->referent(); existing && uncheckedDowncast<PyWeakRef>(existing)->type() == type && uncheckedDowncast<PyWeakRef>(existing)->referent())
        return uncheckedDowncast<PyWeakRef>(existing);
    PyWeakRef* weakRef = PyWeakRef::create(vm, rt.weakRefStructure(), type, object.asCell(), { });
    holder->setReferent(weakRef);
    return weakRef;
}

PyWeakRef* makeWeakRef(JSGlobalObject* globalObject, JSValue object, JSValue callback)
{
    return newWeakRef(globalObject, runtime(globalObject).weakRefType(), object, callback);
}

// ---- weakref.ref ----

static JSValue weakRefNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A subclass may take keywords, which are for its __init__.
    if (failed(scope, checkPositional(globalObject, "ref"_s, args, 1, 2)))
        return { };
    RELEASE_AND_RETURN(scope, newWeakRef(globalObject, PyType::from(cls), args[0], args.get(1)));
}

static JSValue weakRefInit(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "ref"_s, args)) || failed(scope, checkPositional(globalObject, "ref"_s, args, 1, 2)))
        return { };
    return jsUndefined();
}

static JSValue weakRefCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "weakref"_s, args)) || failed(scope, checkPositional(globalObject, "weakref"_s, args, 0, 0)))
        return { };
    return weakRefGet(self);
}

static JSValue weakRefRepr(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSCell* referent = PyWeakRef::from(self)->referent();
    String address = addressForRepr(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!referent)
        return makeStr(vm, makeString("<weakref at "_s, address, "; dead>"_s));
    // In the type of the referent, not in the referent.
    JSValue name = rt.typeOf(referent)->lookup(rt.names.dName());
    if (name) {
        name = bindDescriptor(globalObject, name, referent, rt.typeOf(referent));
        RETURN_IF_EXCEPTION(scope, { });
    }
    String addressOfReferent = addressForRepr(globalObject, referent);
    RETURN_IF_EXCEPTION(scope, { });
    String prefix = makeString("<weakref at "_s, address, "; to '"_s, typeNameForMessage(globalObject, rt.typeOf(referent)), "' at "_s, addressOfReferent);
    if (name && isStr(name)) {
        String text = strValue(name)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        return makeStr(vm, makeString(prefix, " ("_s, text, ")>"_s));
    }
    return makeStr(vm, makeString(prefix, '>'));
}

static int64_t weakRefHash(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* weakRef = PyWeakRef::from(self);
    if (!weakRef->hasHash()) {
        JSCell* referent = weakRef->referent();
        if (!referent) {
            raiseTypeError(globalObject, scope, "weak object has gone away"_s);
            return -1;
        }
        int64_t result = hash(globalObject, referent);
        RETURN_IF_EXCEPTION(scope, -1);
        weakRef->setHash(result);
    }
    return weakRef->hash();
}

// While both referents are alive they are what is compared; otherwise the references themselves.
template<CompareOperator op>
static JSValue weakRefEquality(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    auto* otherRef = PyWeakRef::tryFrom(other);
    if (!otherRef || !otherRef->type()->isSubtypeOf(rt.weakRefType()))
        return rt.notImplemented();
    JSCell* a = PyWeakRef::from(self)->referent();
    JSCell* b = otherRef->referent();
    if (!a || !b)
        return jsBoolean((self == other) == (op == CompareOperator::Eq));
    return richCompare(globalObject, a, b, op);
}

static JSValue weakRefGetCallback(JSGlobalObject*, JSValue self)
{
    auto* weakRef = PyWeakRef::from(self);
    JSValue callback = weakRef->callback();
    return callback && weakRef->referent() ? callback : jsUndefined();
}

static constexpr NativeDef weakRefMethods[] = {
    PY_NEW(weakRefNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", weakRefInit, "($self, *args, **kwargs)"),
    PY_SLOT("__call__", weakRefCall, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", weakRefRepr, "($self, /)"),
    PY_HASH(weakRefHash),
    PY_SLOT("__eq__", weakRefEquality<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", weakRefEquality<CompareOperator::NotEq>, "($self, value, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet weakRefGetSets[] = {
    PY_MEMBER("__callback__", weakRefGetCallback),
};

// ---- weakref.proxy ----

static bool isWeakProxy(JSGlobalObject* globalObject, JSValue value)
{
    auto* weakRef = PyWeakRef::tryFrom(value);
    if (!weakRef)
        return false;
    Runtime& rt = runtime(globalObject);
    return weakRef->type() == rt.weakProxyType() || weakRef->type() == rt.weakCallableProxyType();
}

// What a proxy stands for; any other value is itself. Empty: a ReferenceError.
static JSValue unwrapProxy(JSGlobalObject* globalObject, JSValue value)
{
    if (!isWeakProxy(globalObject, value))
        return value;
    if (JSCell* referent = PyWeakRef::from(value)->referent())
        return referent;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, rt.ReferenceError(), "weakly-referenced object no longer exists"_s);
}

#define UNWRAP(variable) \
    variable = unwrapProxy(globalObject, variable); \
    if (!variable) \
        return { }
// In a function that has a ThrowScope.
#define UNWRAP_IN_SCOPE(variable) \
    variable = unwrapProxy(globalObject, variable); \
    if (failed(scope, variable)) \
        return { }

template<BinaryOperator op>
static JSValue proxyBinary(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    UNWRAP(self);
    UNWRAP(other);
    return binaryOperation(globalObject, op, self, other);
}

template<BinaryOperator op>
static JSValue proxyReflected(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    UNWRAP(self);
    UNWRAP(other);
    return binaryOperation(globalObject, op, other, self);
}

template<UnaryOperator op>
static JSValue proxyUnary(JSGlobalObject* globalObject, JSValue self)
{
    UNWRAP(self);
    return unaryOperation(globalObject, op, self);
}

template<CompareOperator op>
static JSValue proxyCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    UNWRAP(self);
    UNWRAP(other);
    return richCompare(globalObject, self, other, op);
}

static JSValue proxyPow(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue modulus)
{
    UNWRAP(self);
    UNWRAP(other);
    if (modulus) {
        UNWRAP(modulus);
    }
    return power(globalObject, self, other, modulus ? modulus : jsUndefined());
}

static JSValue proxyRPow(JSGlobalObject* globalObject, JSValue self, JSValue other, JSValue modulus)
{
    UNWRAP(self);
    UNWRAP(other);
    return power(globalObject, other, self, modulus ? modulus : jsUndefined());
}

static JSValue proxyDivmod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    UNWRAP(self);
    UNWRAP(other);
    return divmod(globalObject, self, other);
}

static JSValue proxyRDivmod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    UNWRAP(self);
    UNWRAP(other);
    return divmod(globalObject, other, self);
}

static JSValue proxyAbs(JSGlobalObject* globalObject, JSValue self)
{
    UNWRAP(self);
    return absolute(globalObject, self);
}

static JSValue proxyInt(JSGlobalObject* globalObject, JSValue self)
{
    UNWRAP(self);
    return toInt(globalObject, self);
}

static JSValue proxyIndex(JSGlobalObject* globalObject, JSValue self)
{
    UNWRAP(self);
    return toIndex(globalObject, self);
}

static JSValue proxyFloat(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNWRAP_IN_SCOPE(self);
    double value = toDouble(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeFloat(globalObject, value));
}

static JSValue proxyBool(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNWRAP_IN_SCOPE(self);
    bool value = isTrue(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(value);
}

static JSValue proxyLen(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNWRAP_IN_SCOPE(self);
    int64_t size = length(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(static_cast<double>(size));
}

static JSValue proxyContains(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNWRAP_IN_SCOPE(self);
    bool found = contains(globalObject, self, item);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

static JSValue proxyGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    UNWRAP(self);
    return getItem(globalObject, self, key);
}

static JSValue proxySetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNWRAP_IN_SCOPE(self);
    setItem(globalObject, self, key, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue proxyDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNWRAP_IN_SCOPE(self);
    delItem(globalObject, self, key);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue proxyIter(JSGlobalObject* globalObject, JSValue self)
{
    UNWRAP(self);
    return getIter(globalObject, self);
}

static JSValue proxyNext(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNWRAP_IN_SCOPE(self);
    PyType* type = rt.typeOf(self);
    JSValue method = type->lookup(Special::Next);
    if (!method || isNone(method))
        return raiseTypeError(globalObject, scope, "Weakref proxy referenced a non-iterator '"_s, type->name(), "' object"_s);
    RELEASE_AND_RETURN(scope, callSpecial(globalObject, method, self));
}

static JSValue proxyCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    UNWRAP(self);
    return call(globalObject, self, args);
}

static JSValue proxyStr(JSGlobalObject* globalObject, JSValue self)
{
    UNWRAP(self);
    return str(globalObject, self);
}

static JSValue proxyRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    JSCell* referent = PyWeakRef::from(self)->referent();
    String address = addressForRepr(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!referent)
        return makeStr(globalObject, makeString("<weakproxy at "_s, address, "; dead>"_s));
    String addressOfReferent = addressForRepr(globalObject, referent);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(globalObject, makeString("<weakproxy at "_s, address, "; to '"_s, typeNameForMessage(globalObject, typeOf(globalObject, referent)), "' at "_s, addressOfReferent, '>'));
}

static JSValue proxyGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!name)
        return { };
    UNWRAP_IN_SCOPE(self);
    RELEASE_AND_RETURN(scope, getAttrAsGiven(globalObject, self, name, nameValue));
}

static JSValue proxySetAttr(JSGlobalObject* globalObject, JSValue self, JSValue nameValue, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    UNWRAP_IN_SCOPE(self);
    setAttrAsGiven(globalObject, self, name, nameValue, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue proxyDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    UNWRAP_IN_SCOPE(self);
    delAttrAsGiven(globalObject, self, name, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue proxyBytes(JSGlobalObject* globalObject, JSValue self)
{
    UNWRAP(self);
    return callMethod(globalObject, self, runtime(globalObject).name(Special::Bytes));
}

static JSValue proxyReversed(JSGlobalObject* globalObject, JSValue self)
{
    UNWRAP(self);
    return callMethod(globalObject, self, runtime(globalObject).name(Special::Reversed));
}

#undef UNWRAP
#undef UNWRAP_IN_SCOPE

#define PROXY_BINARY(name, op) \
    PY_SLOT("__" name "__", proxyBinary<BinaryOperator::op>, "($self, value, /)"), \
    PY_SLOT("__r" name "__", proxyReflected<BinaryOperator::op>, "($self, value, /)"), \
    PY_SLOT("__i" name "__", proxyBinary<BinaryOperator::InPlace##op>, "($self, value, /)")

#define PROXY_METHODS \
    PROXY_BINARY("add", Add), \
    PROXY_BINARY("sub", Sub), \
    PROXY_BINARY("mul", Mult), \
    PROXY_BINARY("matmul", MatMult), \
    PROXY_BINARY("truediv", Div), \
    PROXY_BINARY("floordiv", FloorDiv), \
    PROXY_BINARY("mod", Mod), \
    PROXY_BINARY("lshift", LShift), \
    PROXY_BINARY("rshift", RShift), \
    PROXY_BINARY("and", BitAnd), \
    PROXY_BINARY("or", BitOr), \
    PROXY_BINARY("xor", BitXor), \
    PY_SLOT("__pow__", proxyPow, "($self, value, mod=None, /)"), \
    PY_SLOT("__rpow__", proxyRPow, "($self, value, mod=None, /)"), \
    PY_SLOT("__ipow__", proxyBinary<BinaryOperator::InPlacePow>, "($self, value, /)"), \
    PY_SLOT("__divmod__", proxyDivmod, "($self, value, /)"), \
    PY_SLOT("__rdivmod__", proxyRDivmod, "($self, value, /)"), \
    PY_SLOT("__neg__", proxyUnary<UnaryOperator::Negative>, "($self, /)"), \
    PY_SLOT("__pos__", proxyUnary<UnaryOperator::Positive>, "($self, /)"), \
    PY_SLOT("__invert__", proxyUnary<UnaryOperator::Invert>, "($self, /)"), \
    PY_SLOT("__abs__", proxyAbs, "($self, /)"), \
    PY_SLOT("__int__", proxyInt, "($self, /)"), \
    PY_SLOT("__float__", proxyFloat, "($self, /)"), \
    PY_SLOT("__index__", proxyIndex, "($self, /)"), \
    PY_SLOT("__bool__", proxyBool, "($self, /)"), \
    PY_SLOT("__len__", proxyLen, "($self, /)"), \
    PY_SLOT("__contains__", proxyContains, "($self, key, /)"), \
    PY_SLOT("__getitem__", proxyGetItem, "($self, key, /)"), \
    PY_SLOT("__setitem__", proxySetItem, "($self, key, value, /)"), \
    PY_SLOT("__delitem__", proxyDelItem, "($self, key, /)"), \
    PY_SLOT("__iter__", proxyIter, "($self, /)"), \
    PY_SLOT("__next__", proxyNext, "($self, /)"), \
    PY_SLOT("__str__", proxyStr, "($self, /)"), \
    PY_SLOT("__repr__", proxyRepr, "($self, /)"), \
    PY_SLOT("__getattribute__", proxyGetAttribute, "($self, name, /)"), \
    PY_SLOT("__setattr__", proxySetAttr, "($self, name, value, /)"), \
    PY_SLOT("__delattr__", proxyDelAttr, "($self, name, /)"), \
    PY_SLOT("__eq__", proxyCompare<CompareOperator::Eq>, "($self, value, /)"), \
    PY_SLOT("__ne__", proxyCompare<CompareOperator::NotEq>, "($self, value, /)"), \
    PY_SLOT("__lt__", proxyCompare<CompareOperator::Lt>, "($self, value, /)"), \
    PY_SLOT("__le__", proxyCompare<CompareOperator::LtE>, "($self, value, /)"), \
    PY_SLOT("__gt__", proxyCompare<CompareOperator::Gt>, "($self, value, /)"), \
    PY_SLOT("__ge__", proxyCompare<CompareOperator::GtE>, "($self, value, /)"), \
    PY_METHOD("__bytes__", proxyBytes, "($self, /)"), \
    PY_METHOD("__reversed__", proxyReversed, "($self, /)")

static constexpr NativeDef weakProxyMethods[] = {
    PROXY_METHODS,
};

static constexpr NativeDef weakCallableProxyMethods[] = {
    PROXY_METHODS,
    PY_SLOT("__call__", proxyCall, "($self, *args, **kwargs)"),
};

#undef PROXY_METHODS
#undef PROXY_BINARY

void initializeWeakRefTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    rt.weakRefType()->initializeBuiltin(globalObject, { "weakref.ReferenceType"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, weakRefMethods, weakRefGetSets, { } }, rt.typeType(), rt.objectType());
    for (auto [type, name, methods] : { std::tuple { rt.weakProxyType(), "weakref.ProxyType"_s, std::span<const NativeDef> { weakProxyMethods } }, std::tuple { rt.weakCallableProxyType(), "weakref.CallableProxyType"_s, std::span<const NativeDef> { weakCallableProxyMethods } } }) {
        type->initializeBuiltin(globalObject, { name, { TypeFlag::DisallowInstantiation }, methods, { }, { } }, rt.typeType(), rt.objectType());
        type->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
        type->didChange(vm);
    }
}

// ---- The module _weakref ----

static JSValue weakrefProxy(JSGlobalObject* globalObject, JSValue object, JSValue callback)
{
    Runtime& rt = runtime(globalObject);
    return newWeakRef(globalObject, isCallable(globalObject, object) ? rt.weakCallableProxyType() : rt.weakProxyType(), object, callback);
}

// All the weak references to an object that are alive. There is no list of them: the heap is searched. The shared reference
// comes first, then the shared proxy, then the others, the latest first.
static JSArray* weakRefsTo(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    Vector<PyWeakRef*> found; // Nothing is allocated before they are in the list.
    if (object.isObject()) {
        PyWeakRef* shared = sharedWeakRefs(globalObject, object);
        JSCell* sharedRef = shared ? shared->referent() : nullptr;
        JSCell* sharedProxy = shared ? PyWeakRef::from(shared->callback())->referent() : nullptr;
        if (auto* space = vm.heap.pyWeakRefSpace<SubspaceAccess::Concurrently>()) {
            HeapIterationScope iterationScope(vm.heap);
            space->forEachLiveCell([&](HeapCell* cell, HeapCell::Kind) {
                auto* weakRef = static_cast<PyWeakRef*>(cell);
                if (weakRef->referent() == object.asCell() && !weakRef->isInternal())
                    found.append(weakRef);
            });
        }
        auto rank = [&](PyWeakRef* weakRef) { return weakRef == sharedRef ? 0 : weakRef == sharedProxy ? 1 : 2; };
        std::ranges::sort(found, [&](PyWeakRef* a, PyWeakRef* b) {
            return rank(a) != rank(b) ? rank(a) < rank(b) : a->serial() > b->serial();
        });
    }
    MarkedArgumentBuffer values;
    for (PyWeakRef* weakRef : found)
        values.append(weakRef);
    return newList(globalObject, spanOf(values));
}

JSValue firstWeakRefTo(JSGlobalObject* globalObject, JSValue object)
{
    if (PyWeakRef* shared = sharedWeakRefs(globalObject, object)) {
        if (JSCell* weakRef = shared->referent())
            return weakRef;
    }
    JSArray* all = weakRefsTo(globalObject, object);
    return all && listLength(all) ? listGet(all, 0) : jsUndefined();
}

static JSValue weakrefGetWeakRefs(JSGlobalObject* globalObject, JSValue object) { return weakRefsTo(globalObject, object); }
static JSValue weakrefGetWeakRefCount(JSGlobalObject* globalObject, JSValue object)
{
    JSArray* list = weakRefsTo(globalObject, object);
    return list ? jsNumber(listLength(list)) : JSValue();
}

// del dct[key], if the value there is a weak reference that is dead.
static JSValue weakrefRemoveDeadWeakRef(JSGlobalObject* globalObject, JSValue dictValue, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isDict(dictValue))
        return raiseTypeError(globalObject, scope, "_remove_dead_weakref() argument 1 must be dict, not "_s, typeName(globalObject, dictValue));
    PyDict* dict = PyDict::from(dictValue);
    JSValue value = dict->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto* weakRef = PyWeakRef::tryFrom(value); weakRef && !weakRef->referent()) {
        dict->remove(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static constexpr NativeDef weakrefFunctions[] = {
    PY_FUNCTION("proxy", weakrefProxy, "(object, callback=None, /)"),
    PY_FUNCTION("getweakrefs", weakrefGetWeakRefs, "(object, /)"),
    PY_FUNCTION("getweakrefcount", weakrefGetWeakRefCount, "(object, /)"),
    PY_FUNCTION("_remove_dead_weakref", weakrefRemoveDeadWeakRef, "(dct, key, /)"),
};

void initializeWeakrefModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->addFunctions(globalObject, weakrefFunctions);
    module->add(globalObject, "ref"_s, rt.weakRefType());
    module->add(globalObject, "ReferenceType"_s, rt.weakRefType());
    module->add(globalObject, "ProxyType"_s, rt.weakProxyType());
    module->add(globalObject, "CallableProxyType"_s, rt.weakCallableProxyType());
}

} // namespace Python
} // namespace JSC
