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

// property. This is the part of CPython's Objects/descrobject.c that is about it.

namespace JSC { namespace Python {

using Kind = PyNativeFunction::Kind;

namespace PropertyField {
enum Field : unsigned { Get, Set, Delete, Doc };
}

// Empty rather than None, where there is none.
static JSValue functionOf(PyNativeObject* property, unsigned field)
{
    JSValue function = property->field(field);
    return function && !isNone(function) ? function : JSValue();
}

// What it was told that it is called, or failing that what its getter is called. Empty if neither.
static JSValue nameOfProperty(JSGlobalObject* globalObject, PyNativeObject* property)
{
    VM& vm = globalObject->vm();
    if (JSValue name = property->getDirect(vm, vm.pythonNames().private_propertyName))
        return name;
    JSValue getter = functionOf(property, PropertyField::Get);
    if (!getter)
        return { };
    return getAttributeIfPresent(globalObject, getter, vm.pythonNames().dunder_name);
}

static JSValue raiseHasNo(JSGlobalObject* globalObject, PyNativeObject* property, JSValue instance, ASCIILiteral what)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue name = nameOfProperty(globalObject, property);
    RETURN_IF_EXCEPTION(scope, { });
    String className = reprOfString(qualifiedNameWithoutModule(globalObject, typeOf(globalObject, instance)));
    if (!name)
        return raise(globalObject, scope, BuiltinType::AttributeError, concatenate("property of "_s, className, " object has no "_s, what));
    String nameText = repr(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    return raise(globalObject, scope, BuiltinType::AttributeError, concatenate("property "_s, nameText, " of "_s, className, " object has no "_s, what));
}

JSValue getProperty(JSGlobalObject* globalObject, PyNativeObject* property, JSValue instance)
{
    JSValue getter = functionOf(property, PropertyField::Get);
    if (!getter)
        return raiseHasNo(globalObject, property, instance, "getter"_s);
    return callForInstance(globalObject, getter, instance);
}

// An empty value deletes.
void setProperty(JSGlobalObject* globalObject, PyNativeObject* property, JSValue instance, JSValue value)
{
    JSValue function = functionOf(property, value ? PropertyField::Set : PropertyField::Delete);
    if (!function) {
        raiseHasNo(globalObject, property, instance, value ? "setter"_s : "deleter"_s);
        return;
    }
    if (value)
        callForInstance(globalObject, function, instance, value);
    else
        callForInstance(globalObject, function, instance);
}

// property(fget=None, fset=None, fdel=None, doc=None)
PYTHON_NATIVE(propertyInit)
{
    NATIVE_PROLOGUE();
    auto* property = asNativeObject(args[0]);
    for (unsigned i = 0; i < 3; ++i) {
        JSValue function = args.at(i + 1);
        property->setField(vm, i, function && !isNone(function) ? function : JSValue());
    }
    property->setField(vm, PropertyField::Doc, JSValue());
    property->putDirect(vm, names.private_propertyName, JSValue());

    // If it is given no docstring and the getter has one, that is it.
    bool isGettersDoc = false;
    JSValue doc = args.at(4);
    if (doc && isNone(doc))
        doc = { };
    if (JSValue getter = functionOf(property, PropertyField::Get); !doc && getter) {
        doc = getAttributeIfPresent(globalObject, getter, names.dunder_doc);
        RETURN_IF_EXCEPTION(scope, { });
        if (doc && isNone(doc))
            doc = { };
        isGettersDoc = !!doc;
    }
    property->putDirect(vm, names.private_isGettersDoc, jsBoolean(isGettersDoc));

    if (isExactly(globalObject, property, realm->typeProperty())) {
        property->setField(vm, PropertyField::Doc, doc);
        RETURN_NONE();
    }
    // An instance of a class derived from this has it as an attribute of its own, or the __doc__ of the class would hide it.
    setAttribute(globalObject, property, names.dunder_doc, doc ? doc : jsUndefined());
    if (scope.exception() && (isGettersDoc || !catchException(globalObject, BuiltinType::AttributeError)))
        return { };
    RETURN_NONE();
}

// @x.getter, @x.setter and @x.deleter: another, of the same class, with one function replaced.
PYTHON_NATIVE(propertyWith)
{
    auto replaced = unpack<unsigned>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto* original = asNativeObject(args[0]);
    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < 3; ++i) {
        JSValue function = i == replaced && !isNone(args[1]) ? args[1] : functionOf(original, i);
        arguments.append(function ? function : jsUndefined());
    }
    // If the docstring was the getter's, it is to be the new getter's.
    JSValue doc = original->field(PropertyField::Doc);
    bool takesDocFromGetter = original->getDirect(vm, names.private_isGettersDoc).isTrue() && !isNone(arguments.at(0));
    arguments.append(takesDocFromGetter || !doc ? jsUndefined() : doc);

    JSValue copy = call(globalObject, typeOf(globalObject, original)->object(), arguments);
    RETURN_IF_EXCEPTION(scope, { });
    if (isInstance(globalObject, copy, realm->typeProperty()))
        asObject(copy)->putDirect(vm, names.private_propertyName, original->getDirect(vm, names.private_propertyName));
    return JSValue::encode(copy);
}

PYTHON_NATIVE(propertyGet)
{
    NATIVE_PROLOGUE();
    JSValue instance = args[1];
    if (isNone(instance)) {
        if (args.size() < 3 || isNone(args[2]))
            return JSValue::encode(raiseTypeError(globalObject, scope, "__get__(None, None) is invalid"_s));
        return JSValue::encode(args[0]);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(getProperty(globalObject, asNativeObject(args[0]), instance)));
}

PYTHON_NATIVE(propertySet)
{
    NATIVE_PROLOGUE();
    setProperty(globalObject, asNativeObject(args[0]), args[1], args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(propertyDelete)
{
    NATIVE_PROLOGUE();
    setProperty(globalObject, asNativeObject(args[0]), args[1], JSValue());
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(propertySetName)
{
    NATIVE_PROLOGUE();
    if (args.size() != 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("__set_name__() takes 2 positional arguments but "_s, args.size() - 1, " were given"_s)));
    asNativeObject(args[0])->putDirect(vm, names.private_propertyName, args[2]);
    RETURN_NONE();
}

// Whether something says that it is abstract: _PyObject_IsAbstract() of Objects/object.c.
bool isAbstract(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value)
        return false;
    JSValue flag = getAttributeIfPresent(globalObject, value, Identifier::fromString(vm, "__isabstractmethod__"_s));
    RETURN_IF_EXCEPTION(scope, false);
    if (!flag)
        return false;
    RELEASE_AND_RETURN(scope, isTrue(globalObject, flag));
}

template<unsigned index>
static JSValue getPropertyField(JSGlobalObject*, JSValue self)
{
    JSValue value = asNativeObject(self)->field(index);
    return value ? value : jsUndefined();
}

void initializeProperty(JSGlobalObject* globalObject)
{
    PyRealm* realm = globalObject->pyRealm();
    PyType* property = realm->typeProperty();
    addMethods(globalObject, property, {
        { "__init__"_s, propertyInit, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__get__"_s, propertyGet },
        { "__set__"_s, propertySet },
        { "__delete__"_s, propertyDelete },
        { "__set_name__"_s, propertySetName, Kind::Method, 0, "__set_name__($self, /, *args)"_s },
        { "getter"_s, propertyWith, Kind::Method, pack(PropertyField::Get) },
        { "setter"_s, propertyWith, Kind::Method, pack(PropertyField::Set) },
        { "deleter"_s, propertyWith, Kind::Method, pack(PropertyField::Delete) },
    });
    addMember(globalObject, property, "fget"_s, getPropertyField<PropertyField::Get>);
    addMember(globalObject, property, "fset"_s, getPropertyField<PropertyField::Set>);
    addMember(globalObject, property, "fdel"_s, getPropertyField<PropertyField::Delete>);
    addMember(globalObject, property, "__doc__"_s, getPropertyField<PropertyField::Doc>, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        asNativeObject(self)->setField(globalObject->vm(), PropertyField::Doc, value);
    });
    addGetSet(globalObject, property, "__name__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue name = nameOfProperty(globalObject, asNativeObject(self));
        RETURN_IF_EXCEPTION(scope, { });
        if (!name)
            return raise(globalObject, scope, BuiltinType::AttributeError, "'property' object has no attribute '__name__'"_s);
        return name;
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        asNativeObject(self)->putDirect(globalObject->vm(), globalObject->vm().pythonNames().private_propertyName, value);
    });
    addGetSet(globalObject, property, "__isabstractmethod__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        for (unsigned i = 0; i < 3; ++i) {
            bool result = isAbstract(globalObject, functionOf(asNativeObject(self), i));
            RETURN_IF_EXCEPTION(scope, { });
            if (result)
                return jsBoolean(true);
        }
        return jsBoolean(false);
    });
    // A staticmethod and a classmethod are abstract if what they wrap is.
    for (PyType* type : { realm->typeStaticMethod(), realm->typeClassMethod() }) {
        addGetSet(globalObject, type, "__isabstractmethod__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
            return jsBoolean(isAbstract(globalObject, asNativeObject(self)->field(0)));
        });
    }
}

} } // namespace JSC::Python
