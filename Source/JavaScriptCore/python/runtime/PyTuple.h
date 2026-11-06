/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyObject.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

class PyTuple : public PyObject {
public:
    PY_DECLARE_CELL(PyTuple, PyObject, PyTupleType);
    DECLARE_VISIT_CHILDREN;

    // The names of the keyword arguments of a call are such a tuple, told apart by the type of the cell.
    static Structure* createKeywordNamesStructure(VM& vm, JSGlobalObject* globalObject)
    {
        return Structure::create(vm, globalObject, jsNull(), TypeInfo(PyKwNamesType, StructureFlags), info());
    }

    static size_t allocationSize(unsigned length) { return sizeof(PyTuple) + length * sizeof(WriteBarrier<Unknown>); }

    // The items are None until they are set, which must be before anything else sees the tuple.
    static PyTuple* createUninitialized(VM&, Structure*, PyType*, unsigned length);

    // Of type tuple.
    static PyTuple* create(JSGlobalObject*, unsigned length);
    static PyTuple* create(JSGlobalObject*, std::span<const JSValue>);
    static PyTuple* create(JSGlobalObject* globalObject, std::initializer_list<JSValue> items) { return create(globalObject, std::span<const JSValue> { items.begin(), items.size() }); }
    static PyTuple* createFromNegativeIndexed(JSGlobalObject*, const JSValue* first, unsigned length);
    static PyTuple* empty(JSGlobalObject*);

    unsigned length() const { return m_length; }
    JSValue at(unsigned index) const
    {
        ASSERT(index < m_length);
        return items()[index].get();
    }
    void initializeAt(VM& vm, unsigned index, JSValue value)
    {
        ASSERT(index < m_length);
        items()[index].set(vm, this, value);
    }
    std::span<const JSValue> span() const { return { reinterpret_cast<const JSValue*>(items()), m_length }; }

    // hash(tuple), once it has been worked out; -1 until then. (The items do not change, and what is hashable keeps its hash.)
    int64_t cachedHash() const { return m_hash; }
    void setCachedHash(int64_t hash) { m_hash = hash; }

    // The one kind of tuple that is written to after it has been made: PyTuple_New() of the C API makes it, and PyTuple_SetItem() fills it.
    bool isFilledByC() const { return m_isFilledByC; }
    void setIsFilledByC() { m_isFilledByC = true; }

    static constexpr ptrdiff_t offsetOfLength() { return OBJECT_OFFSETOF(PyTuple, m_length); }
    static constexpr ptrdiff_t offsetOfItems() { return sizeof(PyTuple); }

protected:
    static PyTuple* create(JSGlobalObject*, const JSValue* first, ptrdiff_t step, unsigned length);

    PyTuple(VM&, Structure*, PyType*, unsigned length);
    PyTuple(VM&, Structure*, PyType*, unsigned length, const JSValue* first, ptrdiff_t step);

    WriteBarrier<Unknown>* items() { return storageBehind<WriteBarrier<Unknown>>(this); }
    const WriteBarrier<Unknown>* items() const { return storageBehind<const WriteBarrier<Unknown>>(this); }

    unsigned m_length;
    bool m_isFilledByC { false };
    int64_t m_hash { -1 };
};

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
