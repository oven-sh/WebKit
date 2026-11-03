/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyObject.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The table of a set, of a fixed size.
class PySetStorage final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject)
    {
        return Structure::create(vm, globalObject, jsNull(), TypeInfo(CellType, StructureFlags), info());
    }

    struct Entry {
        WriteBarrier<Unknown> key; // Empty: never used, or, with hash -1, deleted.
        int64_t hash;
    };

    static size_t allocationSize(size_t size) { return sizeof(PySetStorage) + size * sizeof(Entry); }
    static PySetStorage* create(VM&, Structure*, size_t size);

    size_t mask() const { return m_mask; }
    Entry* entries() { return storageBehind<Entry>(this); }

private:
    PySetStorage(VM&, Structure*, size_t size);

    size_t m_mask;
};

// set and frozenset. The table is probed, and so iterated, exactly as CPython's: programs should not depend on the order
// of a set, and do.
class PySet final : public PyObject {
public:
    PY_DECLARE_CELL(PySet, PyObject, PySetType);
    DECLARE_VISIT_CHILDREN;

    static PySet* create(VM&, Structure*, PyType*);
    static PySet* create(JSGlobalObject*); // Of type set.
    static PySet* createFrozen(JSGlobalObject*);

    size_t size() const { return m_used; }
    const void* storageIdentity() const { return m_storage.get(); }
    bool isEmpty() const { return !m_used; }

    // These may run __hash__ and __eq__, and so throw.
    bool contains(JSGlobalObject*, JSValue key);
    bool contains(JSGlobalObject*, JSValue key, int64_t hash);
    bool add(JSGlobalObject*, JSValue key); // False: it was there, or an exception.
    bool add(JSGlobalObject*, JSValue key, int64_t hash);
    bool remove(JSGlobalObject*, JSValue key); // False: it was not there, or an exception.
    bool remove(JSGlobalObject*, JSValue key, int64_t hash);

    void clear(VM&);
    PySet* copy(JSGlobalObject*, PyType* = nullptr);
    // set.update(other)
    void merge(JSGlobalObject*, PySet* other);
    void swapContents(VM&, PySet* other);
    // Makes room for that many more at once.
    void reserveFor(VM&, size_t additional);
    // After many removals.
    void purgeDeletedIfMany(VM&);
    // Removes any one. False: the set is empty.
    bool pop(VM&, JSValue& key);

    // for (size_t position = 0; set->next(position, key, hash);)
    bool next(size_t& position, JSValue& key, int64_t& hash) const;
    bool next(size_t& position, JSValue& key) const
    {
        int64_t hash;
        return next(position, key, hash);
    }

    bool hasCachedHash() const { return m_hasHash; }
    int64_t cachedHash() const { return m_hash; }
    void setCachedHash(int64_t hash)
    {
        m_hash = hash;
        m_hasHash = true;
    }

private:
    using Entry = PySetStorage::Entry;

    PySet(VM&, Structure*, PyType*);

    Entry* find(JSGlobalObject*, JSValue key, int64_t hash);
    void resize(VM&, size_t minimumUsed);
    static void insertClean(VM&, PySetStorage*, JSValue key, int64_t hash);

    WriteBarrier<PySetStorage> m_storage;
    size_t m_fill { 0 }; // Used and deleted.
    size_t m_used { 0 };
    size_t m_finger { 0 }; // Where pop() looks next.
    int64_t m_hash { 0 };
    bool m_hasHash { false };
};

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
