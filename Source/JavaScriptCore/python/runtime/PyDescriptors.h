/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyDict.h"
#include "PyNative.h"
#include "Symbol.h"

namespace JSC {
namespace Python {

class PyProperty final : public PyObject {
public:
    PY_DECLARE_CELL(PyProperty, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyProperty* create(VM&, Structure*, PyType*);

    // None where there is none.
    WriteBarrier<Unknown> getter;
    WriteBarrier<Unknown> setter;
    WriteBarrier<Unknown> deleter;
    WriteBarrier<Unknown> doc;
    WriteBarrier<Unknown> name; // Empty: not known.
    bool docIsFromGetter { false };

private:
    PyProperty(VM&, Structure*, PyType*);
};

// An object that is about one other: classmethod, staticmethod, mappingproxy, and some that only the interpreter makes.
class PyWrapperObject final : public PyObject {
public:
    PY_DECLARE_CELL(PyWrapperObject, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyWrapperObject* create(VM&, Structure*, PyType*, JSValue wrapped);
    static PyWrapperObject* create(JSGlobalObject*, PyType*, JSValue wrapped);

    JSValue wrapped() const { return m_wrapped.get(); }
    void setWrapped(VM& vm, JSValue value) { m_wrapped.set(vm, this, value); }
    PyDict* dict() const { return m_dict.get(); }
    void setDict(VM& vm, PyDict* dict) { m_dict.setMayBeNull(vm, this, dict); }

private:
    PyWrapperObject(VM&, Structure*, PyType*);

    WriteBarrier<Unknown> m_wrapped;
    WriteBarrier<PyDict> m_dict;
};

class PySuper final : public PyObject {
public:
    PY_DECLARE_CELL(PySuper, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PySuper* create(VM&, Structure*, PyType*);

    WriteBarrier<PyType> thisClass; // super(thisClass, object)
    WriteBarrier<Unknown> object; // Empty: unbound.
    WriteBarrier<PyType> objectType; // The class whose MRO is searched.

private:
    PySuper(VM&, Structure*, PyType*);
};

class PyGetSetDescriptor final : public PyObject {
public:
    PY_DECLARE_CELL(PyGetSetDescriptor, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyGetSetDescriptor* create(JSGlobalObject*, const NativeGetSet&, PyType* owner);

    const NativeGetSet& def() const { return *m_def; }
    PyType* owner() const { return m_owner.get(); }

private:
    PyGetSetDescriptor(VM&, Structure*, PyType*, const NativeGetSet&);

    const NativeGetSet* m_def;
    WriteBarrier<PyType> m_owner;
};

// What a name in __slots__ becomes. The instance keeps the value as a property that only the descriptor knows the key of.
class PyMemberDescriptor final : public PyObject {
public:
    PY_DECLARE_CELL(PyMemberDescriptor, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyMemberDescriptor* create(JSGlobalObject*, JSString* name, PyType* owner);

    JSString* name() const { return m_name.get(); }
    PyType* owner() const { return m_owner.get(); }
    Symbol* key() const { return m_key.get(); }

private:
    PyMemberDescriptor(VM&, Structure*, PyType*);

    WriteBarrier<JSString> m_name;
    WriteBarrier<PyType> m_owner;
    WriteBarrier<Symbol> m_key;
};

} // namespace Python
} // namespace JSC
