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

#include "PyWeakReference.h"
#include "PythonSignals.h"
#include "TopExceptionScope.h"

// Weak references: Objects/weakrefobject.c and Modules/_weakref.c of CPython. What they are made of is in PyWeakReference.h.
//
// What is otherwise here is when. In CPython an object is no more the moment that nothing refers to it, and the callbacks of the weak references to it are called there and then. Here it is no more when the collector
// finds that out, which is some time afterwards, and the callbacks are called when Python code is next between one thing and another after that: see doPendingWork().

namespace JSC { namespace Python {

static PyWeakReference* asReference(JSValue value) { return uncheckedDowncast<PyWeakReference>(value.asCell()); }
static bool isReferenceOrProxy(JSValue value) { return value.isCell() && value.asCell()->inherits<PyWeakReference>(); }

static bool isProxy(JSGlobalObject* globalObject, JSValue value)
{
    if (!isReferenceOrProxy(value))
        return false;
    PyRealm* realm = globalObject->pyRealm();
    PyType* type = typeOf(globalObject, value);
    return type == realm->typeWeakProxy() || type == realm->typeWeakCallableProxy();
}

// ---- The list of the references to an object

struct BasicReferences {
    PyWeakReference* reference { nullptr };
    PyWeakReference* proxy { nullptr };
};

// get_basic_refs(): those that have no callback, of which there is need of only one of each kind. They come first.
static BasicReferences basicReferences(JSGlobalObject* globalObject, PyWeakReference* head)
{
    BasicReferences result;
    if (!head || head->callback())
        return result;
    // Not one of a class that is derived from ref.
    if (typeOf(globalObject, head) == globalObject->pyRealm()->typeWeakReference()) {
        result.reference = head;
        head = head->next();
    }
    if (head && !head->callback() && isProxy(globalObject, head))
        result.proxy = head;
    return result;
}

// get_or_create_weakref()
static JSValue getOrCreateReference(JSGlobalObject* globalObject, PyType* type, JSValue object, JSValue callback)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    if (!object.isObject() || !typeOf(globalObject, object)->hasFlag(PyType::HasWeakReferences))
        return raiseTypeError(globalObject, scope, concatenate("cannot create weak reference to '"_s, typeName(globalObject, object), "' object"_s));
    if (callback && isNone(callback))
        callback = { };
    JSObject* referent = asObject(object);
    bool isPlainReference = type == realm->typeWeakReference();
    bool isPlainProxy = type == realm->typeWeakProxy() || type == realm->typeWeakCallableProxy();
    PyWeakReferenceList* list = PyWeakReferenceList::ensure(vm, referent);
    if (!callback) {
        auto basic = basicReferences(globalObject, list->head());
        if (isPlainReference && basic.reference)
            return basic.reference;
        if (isPlainProxy && basic.proxy)
            return basic.proxy;
    }
    auto* reference = PyWeakReference::create(vm, type->instanceStructure(), referent, callback);
    // insert_weakref(). Making it may have set off a collection, so the list is looked at afresh.
    auto basic = basicReferences(globalObject, list->head());
    PyWeakReference* previous = !callback && isPlainReference ? nullptr : !callback && isPlainProxy ? basic.reference : basic.proxy ? basic.proxy : basic.reference;
    if (previous)
        PyWeakReferenceList::insertAfter(previous, reference);
    else
        list->insertAtHead(reference);
    return reference;
}

JSValue newWeakReference(JSGlobalObject* globalObject, JSValue object, JSValue callback)
{
    return getOrCreateReference(globalObject, globalObject->pyRealm()->typeWeakReference(), object, callback);
}

JSValue getWeakReferences(JSGlobalObject* globalObject, JSValue self)
{
    auto* list = PyWeakReferenceList::of(globalObject->vm(), asObject(self));
    return list && list->head() ? JSValue(list->head()) : jsUndefined();
}

// ---- ref

// parse_weakref_init_args()
static bool parseArguments(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, ASCIILiteral function)
{
    unsigned given = args.size() - 1;
    if (given >= 1 && given <= 2)
        return true;
    raiseTypeError(globalObject, scope, concatenate(function, " expected at "_s, given ? "most 2"_s : "least 1"_s, " argument"_s, given ? "s"_s : ""_s, ", got "_s, given));
    return false;
}

PYTHON_NATIVE(referenceNew)
{
    NATIVE_PROLOGUE();
    // What is given by name is for the __init__() of a class derived from this.
    if (!parseArguments(globalObject, scope, args, "__new__"_s))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(getOrCreateReference(globalObject, asType(args[0]), args[1], args.size() > 2 ? args[2] : JSValue())));
}

PYTHON_NATIVE(referenceInit)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "ref"_s) || !parseArguments(globalObject, scope, args, "__init__"_s))
        return { };
    RETURN_NONE();
}

PYTHON_NATIVE(referenceCall)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "weakref"_s))
        return { };
    if (args.size() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("weakref expected 0 arguments, got "_s, args.size() - 1)));
    JSObject* referent = asReference(args[0])->referent();
    return JSValue::encode(referent ? JSValue(referent) : jsUndefined());
}

PYTHON_NATIVE(referenceHash)
{
    NATIVE_PROLOGUE();
    PyWeakReference* self = asReference(args[0]);
    if (self->cachedHash() != -1)
        return JSValue::encode(intFromInt64(globalObject, self->cachedHash()));
    JSObject* referent = self->referent();
    if (!referent)
        return JSValue::encode(raiseTypeError(globalObject, scope, "weak object has gone away"_s));
    int64_t result = hash(globalObject, referent);
    RETURN_IF_EXCEPTION(scope, { });
    self->setCachedHash(result);
    return JSValue::encode(intFromInt64(globalObject, result));
}

PYTHON_NATIVE(referenceRepr)
{
    NATIVE_PROLOGUE();
    PyWeakReference* self = asReference(args[0]);
    JSObject* referent = self->referent();
    if (!referent)
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<weakref at "_s, addressOf(self), "; dead>"_s))));
    // _PyObject_LookupSpecial(), of which what goes wrong is passed over.
    String name = emptyString();
    JSValue holder;
    JSValue found = lookupSpecial(globalObject, referent, names.dunder_name, holder);
    if (scope.exception()) [[unlikely]] {
        if (!scope.tryClearException())
            return { };
        found = { };
    }
    // A function of that name would be a method, which is no name.
    if (found && !holder && isInstance(globalObject, found, realm->typeStr())) {
        name = concatenate(" ("_s, str(globalObject, found), ')');
        RETURN_IF_EXCEPTION(scope, { });
    }
    String type = fullyQualifiedTypeName(globalObject, referent);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<weakref at "_s, addressOf(self), "; to '"_s, type, "' at "_s, addressOf(referent), name, '>'))));
}

// They are equal if what they refer to is, and if that has gone, if they are the one reference.
PYTHON_NATIVE(referenceCompare)
{
    NATIVE_PROLOGUE();
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    auto isReference = [&] (JSValue value) { return isReferenceOrProxy(value); };
    if (!isEquality(op) || !isReference(args[0]) || !isReference(args[1]))
        RETURN_NOT_IMPLEMENTED();
    JSObject* referent = asReference(args[0])->referent();
    JSObject* other = asReference(args[1])->referent();
    if (!referent || !other)
        return JSValue::encode(jsBoolean((args[0] == args[1]) == (op == ComparisonOperator::Eq)));
    RELEASE_AND_RETURN(scope, JSValue::encode(compare(globalObject, op, referent, other)));
}

static JSValue getCallback(JSGlobalObject*, JSValue self)
{
    // Once what it refers to has gone, the callback is called and is let go of. It is as good as gone before that has been got round to.
    PyWeakReference* reference = asReference(self);
    return reference->referent() && reference->callback() ? reference->callback() : jsUndefined();
}

// ---- proxy

// UNWRAP(): what a proxy stands for, or anything else as it is. Empty, having raised, if what it stood for has gone.
static JSValue unwrap(JSGlobalObject* globalObject, JSValue value)
{
    if (!isProxy(globalObject, value))
        return value;
    if (JSObject* referent = asReference(value)->referent())
        return referent;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    return raise(globalObject, scope, BuiltinType::ReferenceError, "weakly-referenced object no longer exists"_s);
}

#define UNWRAP(name, value) \
    JSValue name = unwrap(globalObject, value); \
    RETURN_IF_EXCEPTION(scope, { });

enum class Form : uint8_t { Plain, Reflected, InPlace };

PYTHON_NATIVE(proxyBinary)
{
    NATIVE_PROLOGUE();
    auto form = unpack<Form>(callFrame, 1);
    UNWRAP(self, args[0]);
    UNWRAP(other, args[1]);
    if (form == Form::Reflected)
        std::swap(self, other);
    RELEASE_AND_RETURN(scope, JSValue::encode(binaryOperation(globalObject, unpack<BinaryOperator>(callFrame, 0), form == Form::InPlace, self, other)));
}

PYTHON_NATIVE(proxyDivmod)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    UNWRAP(other, args[1]);
    if (unpack<Form>(callFrame, 1) == Form::Reflected)
        std::swap(self, other);
    RELEASE_AND_RETURN(scope, JSValue::encode(divmod(globalObject, self, other)));
}

PYTHON_NATIVE(proxyPower)
{
    NATIVE_PROLOGUE();
    auto form = unpack<Form>(callFrame, 1);
    UNWRAP(self, args[0]);
    UNWRAP(other, args[1]);
    UNWRAP(modulus, args.size() > 2 ? args[2] : jsUndefined());
    if (form == Form::Reflected)
        std::swap(self, other);
    if (form == Form::InPlace && isNone(modulus))
        RELEASE_AND_RETURN(scope, JSValue::encode(binaryOperation(globalObject, BinaryOperator::Pow, true, self, other)));
    RELEASE_AND_RETURN(scope, JSValue::encode(power(globalObject, self, other, modulus)));
}

enum class Unary : uint8_t { Negative, Positive, Absolute, Invert, Int, Float, Index, Str, Iter, Bytes, Reversed };

PYTHON_NATIVE(proxyUnary)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    scope.release();
    switch (unpack<Unary>(callFrame, 0)) {
    case Unary::Negative:
        return JSValue::encode(unaryOperation(globalObject, UnaryOperator::USub, self));
    case Unary::Positive:
        return JSValue::encode(unaryOperation(globalObject, UnaryOperator::UAdd, self));
    case Unary::Invert:
        return JSValue::encode(unaryOperation(globalObject, UnaryOperator::Invert, self));
    case Unary::Absolute:
        return JSValue::encode(absolute(globalObject, self));
    case Unary::Int:
        return JSValue::encode(numberLong(globalObject, self));
    case Unary::Float:
        return JSValue::encode(call(globalObject, realm->typeFloat(), self));
    case Unary::Index:
        return JSValue::encode(toInt(globalObject, self));
    case Unary::Str:
        return JSValue::encode(strObject(globalObject, self));
    case Unary::Iter:
        return JSValue::encode(getIterator(globalObject, self));
    case Unary::Bytes:
        return JSValue::encode(call(globalObject, getAttribute(globalObject, self, names.dunder_bytes)));
    case Unary::Reversed:
        return JSValue::encode(call(globalObject, getAttribute(globalObject, self, names.dunder_reversed)));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PYTHON_NATIVE(proxyRepr)
{
    NATIVE_PROLOGUE();
    PyWeakReference* self = asReference(args[0]);
    JSObject* referent = self->referent();
    if (!referent)
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<weakproxy at "_s, addressOf(self), "; dead>"_s))));
    String type = fullyQualifiedTypeName(globalObject, referent);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<weakproxy at "_s, addressOf(self), "; to '"_s, type, "' at "_s, addressOf(referent), '>'))));
}

PYTHON_NATIVE(proxyGetAttribute)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    UNWRAP(nameValue, args[1]);
    auto name = attributeName(globalObject, scope, nameValue);
    if (!name)
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(getAttribute(globalObject, self, *name)));
}

PYTHON_NATIVE(proxySetAttribute)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    auto name = attributeName(globalObject, scope, args[1]);
    if (!name)
        return { };
    scope.release();
    if (args.size() > 2)
        setAttribute(globalObject, self, *name, args[2]);
    else
        deleteAttribute(globalObject, self, *name);
    RETURN_NONE();
}

PYTHON_NATIVE(proxyCompare)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    UNWRAP(other, args[1]);
    RELEASE_AND_RETURN(scope, JSValue::encode(compare(globalObject, unpack<ComparisonOperator>(callFrame, 0), self, other)));
}

PYTHON_NATIVE(proxyBool)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(isTrue(globalObject, self))));
}

PYTHON_NATIVE(proxyContains)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(contains(globalObject, self, args[1]))));
}

PYTHON_NATIVE(proxyLength)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    int64_t result = length(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, result));
}

PYTHON_NATIVE(proxyGetItem)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    UNWRAP(key, args[1]);
    RELEASE_AND_RETURN(scope, JSValue::encode(getItem(globalObject, self, key)));
}

PYTHON_NATIVE(proxySetItem)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    scope.release();
    if (args.size() > 2)
        setItem(globalObject, self, args[1], args[2]);
    else
        deleteItem(globalObject, self, args[1]);
    RETURN_NONE();
}

PYTHON_NATIVE(proxyNext)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    if (!typeOf(globalObject, self)->lookup(vm, names.dunder_next))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("Weakref proxy referenced a non-iterator '"_s, typeName(globalObject, self), "' object"_s)));
    JSValue result = iteratorNext(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!result)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    return JSValue::encode(result);
}

PYTHON_NATIVE(proxyCall)
{
    NATIVE_PROLOGUE();
    UNWRAP(self, args[0]);
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, self, args.allFrom(1), args.keywordNames())));
}

// ---- The module

PYTHON_NATIVE(weakrefGetCount)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    int32_t count = 0;
    if (args[0].isObject() && typeOf(globalObject, args[0])->hasFlag(PyType::HasWeakReferences)) {
        if (auto* list = PyWeakReferenceList::of(vm, asObject(args[0]))) {
            for (PyWeakReference* reference = list->head(); reference; reference = reference->next())
                ++count;
        }
    }
    return JSValue::encode(jsNumber(count));
}

PYTHON_NATIVE(weakrefGetReferences)
{
    NATIVE_PROLOGUE();
    MarkedArgumentBuffer references;
    if (args[0].isObject() && typeOf(globalObject, args[0])->hasFlag(PyType::HasWeakReferences)) {
        if (auto* list = PyWeakReferenceList::of(vm, asObject(args[0]))) {
            for (PyWeakReference* reference = list->head(); reference; reference = reference->next())
                references.append(reference);
        }
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, references)));
}

PYTHON_NATIVE(weakrefProxy)
{
    NATIVE_PROLOGUE();
    PyType* type = isCallable(globalObject, args[0]) ? realm->typeWeakCallableProxy() : realm->typeWeakProxy();
    RELEASE_AND_RETURN(scope, JSValue::encode(getOrCreateReference(globalObject, type, args[0], args.size() > 1 ? args[1] : JSValue())));
}

// Takes a key out of a dict if what it has for it is a weak reference to what has gone: _PyDict_DelItemIf().
PYTHON_NATIVE(weakrefRemoveDead)
{
    NATIVE_PROLOGUE();
    if (!isDict(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_remove_dead_weakref() argument 1 must be dict, not "_s, typeNameOfArgument(globalObject, args[0]))));
    PyDict* dict = asDict(args[0]);
    // It is hashed as anything is, and not as a key is, which is said otherwise if it cannot be.
    hash(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = dict->get(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        RETURN_NONE();
    if (!isReferenceOrProxy(value))
        return JSValue::encode(raiseTypeError(globalObject, scope, "not a weakref"_s));
    if (!asReference(value)->referent()) {
        dict->remove(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

JSObject* createWeakrefModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    JSObject* module = newBuiltinModule(globalObject, "_weakref"_s);
    for (auto [name, type] : { std::pair { "ref"_s, realm->typeWeakReference() }, std::pair { "ReferenceType"_s, realm->typeWeakReference() }, std::pair { "ProxyType"_s, realm->typeWeakProxy() }, std::pair { "CallableProxyType"_s, realm->typeWeakCallableProxy() } })
        module->putDirect(vm, Identifier::fromString(vm, name), type);
    addFunction(globalObject, module, "getweakrefcount"_s, weakrefGetCount);
    addFunction(globalObject, module, "getweakrefs"_s, weakrefGetReferences);
    addFunction(globalObject, module, "proxy"_s, weakrefProxy);
    addFunction(globalObject, module, "_remove_dead_weakref"_s, weakrefRemoveDead);
    return module;
}

void initializeWeakReferenceTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& names = vm.pythonNames();
    using Kind = PyNativeFunction::Kind;
    constexpr auto notChecked = PyNativeFunction::Arguments::AreNotChecked;
    for (PyType* type : { realm->typeWeakReference(), realm->typeWeakProxy(), realm->typeWeakCallableProxy() })
        type->setInstanceStructure(vm, PyWeakReference::createStructure(vm, globalObject, type));

    PyType* reference = realm->typeWeakReference();
    addMethods(globalObject, reference, {
        { "__new__"_s, referenceNew, Kind::New, 0, { }, notChecked },
        { "__init__"_s, referenceInit, Kind::Method, 0, { }, notChecked },
        { "__call__"_s, referenceCall, Kind::Method, 0, { }, notChecked },
        { "__hash__"_s, referenceHash },
        { "__repr__"_s, referenceRepr },
    });
    addComparisons(globalObject, reference, referenceCompare);
    addMember(globalObject, reference, "__callback__"_s, getCallback);

    for (PyType* proxy : { realm->typeWeakProxy(), realm->typeWeakCallableProxy() }) {
        for (unsigned i = 0; i < numberOfBinaryOperators; ++i) {
            auto op = static_cast<BinaryOperator>(i);
            if (op == BinaryOperator::Pow)
                continue;
            for (auto [name, form] : { std::pair { &names.method(op), Form::Plain }, std::pair { &names.reflectedMethod(op), Form::Reflected }, std::pair { &names.inPlaceMethod(op), Form::InPlace } })
                proxy->putDirect(vm, *name, PyNativeFunction::create(vm, globalObject, 1, name->string(), proxyBinary, Kind::Method, proxy, pack(op, form)));
        }
        addMethods(globalObject, proxy, {
            { "__divmod__"_s, proxyDivmod, Kind::Method, pack(0, Form::Plain) },
            { "__rdivmod__"_s, proxyDivmod, Kind::Method, pack(0, Form::Reflected) },
            { "__pow__"_s, proxyPower, Kind::Method, pack(0, Form::Plain) },
            { "__rpow__"_s, proxyPower, Kind::Method, pack(0, Form::Reflected) },
            { "__ipow__"_s, proxyPower, Kind::Method, pack(0, Form::InPlace) },
            { "__neg__"_s, proxyUnary, Kind::Method, pack(Unary::Negative) },
            { "__pos__"_s, proxyUnary, Kind::Method, pack(Unary::Positive) },
            { "__abs__"_s, proxyUnary, Kind::Method, pack(Unary::Absolute) },
            { "__invert__"_s, proxyUnary, Kind::Method, pack(Unary::Invert) },
            { "__int__"_s, proxyUnary, Kind::Method, pack(Unary::Int) },
            { "__float__"_s, proxyUnary, Kind::Method, pack(Unary::Float) },
            { "__index__"_s, proxyUnary, Kind::Method, pack(Unary::Index) },
            { "__str__"_s, proxyUnary, Kind::Method, pack(Unary::Str) },
            { "__iter__"_s, proxyUnary, Kind::Method, pack(Unary::Iter) },
            { "__repr__"_s, proxyRepr },
            { "__getattribute__"_s, proxyGetAttribute },
            { "__setattr__"_s, proxySetAttribute },
            { "__delattr__"_s, proxySetAttribute },
            { "__bool__"_s, proxyBool },
            { "__contains__"_s, proxyContains },
            { "__len__"_s, proxyLength },
            { "__getitem__"_s, proxyGetItem },
            { "__setitem__"_s, proxySetItem },
            { "__delitem__"_s, proxySetItem },
            { "__next__"_s, proxyNext },
        });
        addMethodsThatCPythonHas(globalObject, proxy, {
            { "__bytes__"_s, proxyUnary, Kind::Method, pack(Unary::Bytes) },
            { "__reversed__"_s, proxyUnary, Kind::Method, pack(Unary::Reversed) },
            { "__call__"_s, proxyCall, Kind::Method, 0, { }, notChecked },
        });
        addComparisons(globalObject, proxy, proxyCompare);
        // What it is equal to can change, when what it stands for goes.
        proxy->putDirect(vm, names.dunder_hash, jsUndefined());
    }
}

// ---- What is put off

void doPendingWork(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    vm.setHasPythonWork(false);

    // handle_callback(), for each. Those of the references to an object are called beginning with the one that was made last.
    MarkedArgumentBuffer references;
    for (PyWeakReference* reference = vm.takePythonReferencesToCall(); reference; reference = reference->takeNextToCall())
        references.append(reference);
    Vector<unsigned, 8> order(references.size(), [] (size_t i) { return static_cast<unsigned>(i); });
    std::ranges::sort(order, [&] (unsigned a, unsigned b) { return asReference(references.at(a))->order() > asReference(references.at(b))->order(); });
    for (unsigned i : order) {
        PyWeakReference* reference = asReference(references.at(i));
        JSValue callback = reference->takeCallback();
        JSGlobalObject* realmOfReference = reference->globalObject();
        call(realmOfReference, callback, reference);
        if (scope.exception()) [[unlikely]] {
            if (vm.hasPendingTerminationException())
                return;
            reportUnraisableShowing(realmOfReference, "Exception ignored while calling weakref callback"_s, callback);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }

    // It says that there are none before it looks, so that one that comes meanwhile is not lost.
    if (vm.takePythonSignal()) {
        handleSignals(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
    }
    RELEASE_AND_RETURN(scope, reportSignalWakeupErrors(globalObject));
}

} } // namespace JSC::Python
