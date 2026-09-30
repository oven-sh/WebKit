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

// __annotations__ and __annotate__, of a function, a class and a module. The one is what the other gives, kept once it has been asked for: PEP 649. These
// are the getters and setters of CPython's Objects/funcobject.c, typeobject.c and moduleobject.c.

namespace JSC { namespace Python {

// annotate(1), which is to say the annotations themselves and not something that stands for them.
static JSValue callAnnotate(JSGlobalObject* globalObject, JSValue annotate)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue annotations = call(globalObject, annotate, jsNumber(1));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isDict(annotations))
        return raiseTypeError(globalObject, scope, concatenate("__annotate__ returned non-dict of type '"_s, typeName(globalObject, annotations), '\''));
    return annotations;
}

static bool checkAnnotate(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, ASCIILiteral whenDeleted)
{
    if (!value) {
        raiseTypeError(globalObject, scope, whenDeleted);
        return false;
    }
    if (!isNone(value) && !isCallable(globalObject, value)) {
        raiseTypeError(globalObject, scope, "__annotate__ must be callable or None"_s);
        return false;
    }
    return true;
}

// ---- Functions

static JSValue getFunctionAnnotate(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    JSValue annotate = asObject(self)->getDirect(vm, vm.pythonNames().private_annotate);
    return annotate ? annotate : jsUndefined();
}

static void setFunctionAnnotate(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (!checkAnnotate(globalObject, scope, value, "__annotate__ cannot be deleted"_s))
        return;
    asObject(self)->putDirect(vm, names.private_annotate, value);
    if (!isNone(value))
        putDirectOrRemove(globalObject, asObject(self), names.private_annotations, JSValue());
}

static JSValue getFunctionAnnotations(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSObject* function = asObject(self);
    if (JSValue annotations = function->getDirect(vm, names.private_annotations))
        return annotations;
    JSValue annotate = function->getDirect(vm, names.private_annotate);
    JSValue annotations;
    if (annotate && isCallable(globalObject, annotate)) {
        annotations = callAnnotate(globalObject, annotate);
        RETURN_IF_EXCEPTION(scope, { });
    } else
        annotations = PyDict::create(globalObject);
    function->putDirect(vm, names.private_annotations, annotations);
    return annotations;
}

static void setFunctionAnnotations(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (value && isNone(value))
        value = { };
    if (value && !isDict(value)) {
        raiseTypeError(globalObject, scope, "__annotations__ must be set to a dict object"_s);
        return;
    }
    putDirectOrRemove(globalObject, asObject(self), names.private_annotations, value);
    putDirectOrRemove(globalObject, asObject(self), names.private_annotate, JSValue());
}

// ---- Classes

static bool checkIsHeapType(JSGlobalObject* globalObject, ThrowScope& scope, PyType* type, ASCIILiteral attribute)
{
    if (type->hasFlag(PyType::IsHeapType))
        return true;
    raise(globalObject, scope, BuiltinType::AttributeError, concatenate("type object '"_s, type->nameString(globalObject), "' has no attribute '"_s, attribute, '\''));
    return false;
}

static bool checkIsMutable(JSGlobalObject* globalObject, ThrowScope& scope, PyType* type, ASCIILiteral attribute)
{
    if (!type->isImmutable())
        return true;
    raiseTypeError(globalObject, scope, concatenate("cannot set '"_s, attribute, "' attribute of immutable type '"_s, type->nameString(globalObject), '\''));
    return false;
}

static JSValue getTypeAnnotate(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyType* type = asType(self);
    if (!checkIsHeapType(globalObject, scope, type, "__annotate__"_s))
        return { };
    // What was set outright comes first.
    JSValue annotate = type->lookupOwn(vm, names.dunder_annotate);
    if (!annotate)
        annotate = type->lookupOwn(vm, names.dunder_annotate_func);
    if (annotate)
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, annotate, JSValue(), type));
    type->setAttribute(vm, names.dunder_annotate_func, jsUndefined());
    return jsUndefined();
}

static void setTypeAnnotate(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyType* type = asType(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "cannot delete __annotate__ attribute"_s);
        return;
    }
    if (!checkIsMutable(globalObject, scope, type, "__annotate__"_s) || !checkAnnotate(globalObject, scope, value, "cannot delete __annotate__ attribute"_s))
        return;
    type->setAttribute(vm, names.dunder_annotate_func, value);
    if (!isNone(value))
        type->deleteAttribute(vm, globalObject, names.dunder_annotations_cache);
}

static JSValue getTypeAnnotations(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyType* type = asType(self);
    if (!checkIsHeapType(globalObject, scope, type, "__annotations__"_s))
        return { };
    // It is there already under `from __future__ import annotations`.
    JSValue annotations = type->lookupOwn(vm, names.dunder_annotations);
    if (!annotations)
        annotations = type->lookupOwn(vm, names.dunder_annotations_cache);
    if (annotations)
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, annotations, JSValue(), type));
    JSValue annotate = getAttribute(globalObject, self, names.dunder_annotate);
    RETURN_IF_EXCEPTION(scope, { });
    if (isCallable(globalObject, annotate)) {
        annotations = callAnnotate(globalObject, annotate);
        RETURN_IF_EXCEPTION(scope, { });
    } else
        annotations = PyDict::create(globalObject);
    type->setAttribute(vm, names.dunder_annotations_cache, annotations);
    return annotations;
}

static void setTypeAnnotations(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyType* type = asType(self);
    if (!checkIsMutable(globalObject, scope, type, "__annotations__"_s))
        return;
    // It is kept where it is, if it is in the class already, and otherwise where what __annotate__ gave would be.
    bool isInClass = !!type->lookupOwn(vm, names.dunder_annotations);
    const Identifier& where = isInClass ? names.dunder_annotations : names.dunder_annotations_cache;
    if (value)
        type->setAttribute(vm, where, value);
    else {
        bool wasThere = type->deleteAttribute(vm, globalObject, where);
        RETURN_IF_EXCEPTION(scope, void());
        if (!wasThere) {
            raise(globalObject, scope, BuiltinType::AttributeError, "__annotations__"_s);
            return;
        }
    }
    if (isInClass) {
        type->deleteAttribute(vm, globalObject, names.dunder_annotations_cache);
        RETURN_IF_EXCEPTION(scope, void());
    }
    type->deleteAttribute(vm, globalObject, names.dunder_annotate_func);
    RETURN_IF_EXCEPTION(scope, void());
    type->deleteAttribute(vm, globalObject, names.dunder_annotate);
}

// ---- Modules

static JSValue getModuleAnnotate(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    if (JSValue annotate = getStoredAttribute(vm, asObject(self), names.dunder_annotate))
        return annotate;
    putStoredAttribute(vm, asObject(self), names.dunder_annotate, jsUndefined());
    return jsUndefined();
}

static void setModuleAnnotate(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (!checkAnnotate(globalObject, scope, value, "cannot delete __annotate__ attribute"_s))
        return;
    putStoredAttribute(vm, asObject(self), names.dunder_annotate, value);
    if (!isNone(value) && getStoredAttribute(vm, asObject(self), names.dunder_annotations))
        deleteStoredAttribute(globalObject, asObject(self), names.dunder_annotations);
}

static JSValue getModuleAnnotations(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSObject* module = asObject(self);
    if (JSValue annotations = getStoredAttribute(vm, module, names.dunder_annotations))
        return annotations;
    // While it is still being run there may be more to come, so what there is so far is not kept.
    bool isInitializing = false;
    if (JSValue spec = getStoredAttribute(vm, module, names.dunder_spec); spec && !isNone(spec)) {
        JSValue flag = getAttributeIfPresent(globalObject, spec, Identifier::fromString(vm, "_initializing"_s));
        RETURN_IF_EXCEPTION(scope, { });
        isInitializing = flag && isTrue(globalObject, flag);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue annotate = getStoredAttribute(vm, module, names.dunder_annotate);
    JSValue annotations;
    if (annotate && isCallable(globalObject, annotate)) {
        annotations = callAnnotate(globalObject, annotate);
        RETURN_IF_EXCEPTION(scope, { });
    } else
        annotations = PyDict::create(globalObject);
    if (!isInitializing)
        putStoredAttribute(vm, module, names.dunder_annotations, annotations);
    return annotations;
}

static void setModuleAnnotations(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSObject* module = asObject(self);
    if (value)
        putStoredAttribute(vm, module, names.dunder_annotations, value);
    else {
        if (!getStoredAttribute(vm, module, names.dunder_annotations)) {
            raise(globalObject, scope, BuiltinType::AttributeError, "__annotations__"_s);
            return;
        }
        deleteStoredAttribute(globalObject, module, names.dunder_annotations);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (getStoredAttribute(vm, module, names.dunder_annotate))
        deleteStoredAttribute(globalObject, module, names.dunder_annotate);
}

// ---- classmethod and staticmethod, which have those of what they wrap, kept once they have been asked for

template<const Identifier CommonNames::* name>
static JSValue getWrappedAttribute(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& attribute = vm.pythonNames().*name;
    if (JSValue kept = getStoredAttribute(vm, asObject(self), attribute))
        return kept;
    JSValue value = getAttribute(globalObject, uncheckedDowncast<PyNativeObject>(self.asCell())->field(0), attribute);
    RETURN_IF_EXCEPTION(scope, { });
    putStoredAttribute(vm, asObject(self), attribute, value);
    return value;
}

template<const Identifier CommonNames::* name>
static void setWrappedAttribute(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& attribute = vm.pythonNames().*name;
    if (value) {
        putStoredAttribute(vm, asObject(self), attribute, value);
        return;
    }
    if (!getStoredAttribute(vm, asObject(self), attribute)) {
        raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', typeName(globalObject, self), "' object has no attribute '"_s, attribute.string(), '\''));
        return;
    }
    scope.release();
    deleteStoredAttribute(globalObject, asObject(self), attribute);
}

void initializeAnnotations(JSGlobalObject* globalObject)
{
    PyRealm* realm = globalObject->pyRealm();
    for (PyType* type : { realm->typeClassMethod(), realm->typeStaticMethod() }) {
        addGetSet(globalObject, type, "__annotate__"_s, getWrappedAttribute<&CommonNames::dunder_annotate>, setWrappedAttribute<&CommonNames::dunder_annotate>);
        addGetSet(globalObject, type, "__annotations__"_s, getWrappedAttribute<&CommonNames::dunder_annotations>, setWrappedAttribute<&CommonNames::dunder_annotations>);
    }
    addGetSet(globalObject, realm->typeFunction(), "__annotate__"_s, getFunctionAnnotate, setFunctionAnnotate);
    addGetSet(globalObject, realm->typeFunction(), "__annotations__"_s, getFunctionAnnotations, setFunctionAnnotations);
    addGetSet(globalObject, realm->typeType(), "__annotate__"_s, getTypeAnnotate, setTypeAnnotate);
    addGetSet(globalObject, realm->typeType(), "__annotations__"_s, getTypeAnnotations, setTypeAnnotations);
    addGetSet(globalObject, realm->typeModule(), "__annotate__"_s, getModuleAnnotate, setModuleAnnotate);
    addGetSet(globalObject, realm->typeModule(), "__annotations__"_s, getModuleAnnotations, setModuleAnnotations);
}

} } // namespace JSC::Python
