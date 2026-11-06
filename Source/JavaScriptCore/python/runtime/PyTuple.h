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

    // For PyTuple_SetItem() of the C API, the one thing that writes to a tuple after it has been made: see there.
    // PyTuple_New() has made it.
    bool isMadeByC() const { return m_isMadeByC; }
    void setIsMadeByC() { m_isMadeByC = true; }
    // The references to it that C has handed on, or has seen somebody take: none, one, or more. Nobody counts the program's: once it has the tuple, they are more.
    unsigned holdersThatCKnowsOf() const { return m_holdersThatCKnowsOf; }
    void addHolderThatCKnowsOf(bool isProgram)
    {
        if (isProgram)
            m_holdersThatCKnowsOf = 2;
        else if (m_holdersThatCKnowsOf < 2)
            ++m_holdersThatCKnowsOf;
    }

    static constexpr ptrdiff_t offsetOfLength() { return OBJECT_OFFSETOF(PyTuple, m_length); }
    static constexpr ptrdiff_t offsetOfItems() { return sizeof(PyTuple); }

protected:
    static PyTuple* create(JSGlobalObject*, const JSValue* first, ptrdiff_t step, unsigned length);

    PyTuple(VM&, Structure*, PyType*, unsigned length);
    PyTuple(VM&, Structure*, PyType*, unsigned length, const JSValue* first, ptrdiff_t step);

    WriteBarrier<Unknown>* items() { return storageBehind<WriteBarrier<Unknown>>(this); }
    const WriteBarrier<Unknown>* items() const { return storageBehind<const WriteBarrier<Unknown>>(this); }

    unsigned m_length;
    bool m_isMadeByC { false };
    uint8_t m_holdersThatCKnowsOf { 0 };
    int64_t m_hash { -1 };
};

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
