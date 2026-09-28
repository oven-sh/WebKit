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

#pragma once

#include "JSObject.h"

namespace JSC {

// What a dict or a set keeps its entries in. It is of a fixed size: to grow, a table gets another. So the collector can look at one
// at any time, without a lock.
//
// As in CPython, the entries are in the order they were added, and beside them is a hash table of indices into them.
class PyHashStorage final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    static constexpr uint32_t emptyIndex = UINT32_MAX;
    static constexpr uint32_t deletedIndex = UINT32_MAX - 1;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.cellSpace();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    // `stride` is 2 for keys and values, and 1 for keys alone. `indexSize` is a power of two.
    static PyHashStorage* create(VM&, Structure*, unsigned capacity, unsigned indexSize, unsigned stride);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    unsigned capacity() const { return m_capacity; }
    unsigned indexMask() const { return m_indexSize - 1; }
    unsigned indexSize() const { return m_indexSize; }

    WriteBarrier<Unknown>& key(unsigned entry) { return slots()[entry * m_stride]; }
    WriteBarrier<Unknown>& value(unsigned entry) { return slots()[entry * m_stride + 1]; }
    uint32_t& hash(unsigned entry) { return hashes()[entry]; }
    uint32_t& index(unsigned slot) { return hashes()[m_capacity + slot]; }

private:
    PyHashStorage(VM& vm, Structure* structure, unsigned capacity, unsigned indexSize, unsigned stride)
        : Base(vm, structure)
        , m_capacity(capacity)
        , m_indexSize(indexSize)
        , m_stride(stride)
    {
    }

    static size_t allocationSize(unsigned capacity, unsigned indexSize, unsigned stride)
    {
        return sizeof(PyHashStorage) + static_cast<size_t>(capacity) * stride * sizeof(WriteBarrier<Unknown>) + (static_cast<size_t>(capacity) + indexSize) * sizeof(uint32_t);
    }

    WriteBarrier<Unknown>* slots() { return std::bit_cast<WriteBarrier<Unknown>*>(this + 1); }
    uint32_t* hashes() { return std::bit_cast<uint32_t*>(slots() + static_cast<size_t>(m_capacity) * m_stride); }

    unsigned m_capacity;
    unsigned m_indexSize;
    unsigned m_stride;
};

// What dict and set have in common.
class PyHashTable : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.cellSpace();
    }

    DECLARE_VISIT_CHILDREN;

    static constexpr int notFound = -1;
    static constexpr int raised = -2;

    unsigned size() const { return m_size; }

    // The entry with the key, notFound, or raised: comparing keys can run anything.
    int find(JSGlobalObject*, JSValue key);
    int64_t hashOfKey(JSGlobalObject*, JSValue key);
    int find(JSGlobalObject*, JSValue key, uint32_t hash);
    // False if it raised. `wasAdded` is whether there was no such key.
    bool add(JSGlobalObject*, JSValue key, JSValue value, bool* wasAdded = nullptr, bool replace = true);
    // The value that was removed (or the key, of a set); empty if there was none or it raised.
    JSValue remove(JSGlobalObject*, JSValue key);
    void removeEntry(VM&, unsigned entry);
    void clear(VM&);
    void copyFrom(VM&, JSGlobalObject*, PyHashTable&);

    // To go through the entries: from 0 up to entryCount(), skipping those whose key is empty. Adding to the table while doing so may
    // move them, which version() tells.
    unsigned entryCount() const { return m_used; }
    JSValue keyAt(unsigned entry) const { return m_storage->key(entry).get(); }
    JSValue valueAt(unsigned entry) const { return m_storage->value(entry).get(); }
    void setValueAt(VM& vm, unsigned entry, JSValue value) { m_storage->value(entry).set(vm, m_storage.get(), value); }
    // Changes whenever a key is added or removed.
    unsigned version() const { return m_version; }

    // With a string that is known to be one, and cannot raise.
    JSValue getString(JSGlobalObject*, const String&);

protected:
    PyHashTable(VM& vm, Structure* structure, unsigned stride)
        : Base(vm, structure)
        , m_stride(stride)
    {
    }

private:
    void grow(VM&, JSGlobalObject*);
    void insertIndex(PyHashStorage&, uint32_t hash, unsigned entry);

    WriteBarrier<PyHashStorage> m_storage;
    unsigned m_used { 0 }; // Entries taken, of which some may have been removed.
    unsigned m_size { 0 };
    unsigned m_version { 0 };
    unsigned m_stride;
};

class PyDict final : public PyHashTable {
public:
    using Base = PyHashTable;

    DECLARE_EXPORT_INFO;

    static PyDict* create(VM&, Structure*);
    static PyDict* create(JSGlobalObject*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    // Empty if there is none, or if it raised.
    JSValue get(JSGlobalObject*, JSValue key);
    bool set(JSGlobalObject* globalObject, JSValue key, JSValue value) { return add(globalObject, key, value); }
    bool setString(JSGlobalObject*, const String& key, JSValue value);

private:
    PyDict(VM& vm, Structure* structure)
        : Base(vm, structure, 2)
    {
    }
};

// set and frozenset.
class PySet final : public PyHashTable {
public:
    using Base = PyHashTable;

    DECLARE_EXPORT_INFO;

    static PySet* create(VM&, Structure*);
    static PySet* create(JSGlobalObject*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    bool add(JSGlobalObject* globalObject, JSValue key, bool* wasAdded = nullptr) { return Base::add(globalObject, key, JSValue(), wasAdded, false); }

private:
    PySet(VM& vm, Structure* structure)
        : Base(vm, structure, 1)
    {
    }
};

inline bool isDict(JSValue value) { return value.isCell() && value.asCell()->type() == PyDictType; }
inline bool isSet(JSValue value) { return value.isCell() && value.asCell()->type() == PySetType; }

} // namespace JSC
