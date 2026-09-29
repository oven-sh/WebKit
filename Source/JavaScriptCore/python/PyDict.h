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
#include "PyInstance.h"

namespace JSC {

class PyTuple;

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
    // Null if there is no room for it.
    static PyHashStorage* tryCreate(VM&, Structure*, unsigned capacity, unsigned indexSize, unsigned stride);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    unsigned capacity() const { return m_capacity; }
    unsigned indexMask() const { return m_indexSize - 1; }
    unsigned indexSize() const { return m_indexSize; }

    WriteBarrier<Unknown>& key(unsigned entry) { return slots()[static_cast<size_t>(entry) * m_stride]; }
    WriteBarrier<Unknown>& value(unsigned entry) { return slots()[static_cast<size_t>(entry) * m_stride + 1]; }
    uint32_t& hash(unsigned entry) { return hashes()[entry]; }
    uint32_t& index(unsigned slot) { return hashes()[static_cast<size_t>(m_capacity) + slot]; }

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
        return offsetOfSlots() + static_cast<size_t>(capacity) * stride * sizeof(WriteBarrier<Unknown>) + (static_cast<size_t>(capacity) + indexSize) * sizeof(uint32_t);
    }

    // The collector reads them while they are being written, and it is only of what is aligned that it cannot see half.
    static constexpr size_t offsetOfSlots() { return WTF::roundUpToMultipleOf<sizeof(WriteBarrier<Unknown>)>(sizeof(PyHashStorage)); }
    WriteBarrier<Unknown>* slots() { return std::bit_cast<WriteBarrier<Unknown>*>(std::bit_cast<char*>(this) + offsetOfSlots()); }
    uint32_t* hashes() { return std::bit_cast<uint32_t*>(slots() + static_cast<size_t>(m_capacity) * m_stride); }

    unsigned m_capacity;
    unsigned m_indexSize;
    unsigned m_stride;
};

// What dict and set have in common.
class PyHashTable : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesPut;

    DECLARE_VISIT_CHILDREN;

    static constexpr int notFound = -1;
    static constexpr int raised = -2;

    unsigned size() const { return m_size; }

    // The entry with the key, notFound, or raised: comparing keys can run anything.
    int find(JSGlobalObject*, JSValue key);
    int64_t hashOfKey(JSGlobalObject*, JSValue key);
    // What is kept of a hash, and is what the others here take.
    static uint32_t foldHash(int64_t hash) { return static_cast<uint32_t>(hash) ^ static_cast<uint32_t>(static_cast<uint64_t>(hash) >> 32); }
    int find(JSGlobalObject*, JSValue key, uint32_t hash);
    // False if it raised. `wasAdded` is whether there was no such key.
    bool add(JSGlobalObject*, JSValue key, JSValue value, bool* wasAdded = nullptr, bool replace = true);
    // The same, of a key that comes from another table, which has its hash: hashAt(). It is not asked for it again.
    bool addWithHash(JSGlobalObject*, JSValue key, uint32_t hash, JSValue value, bool* wasAdded = nullptr, bool replace = true);
    // The value that was removed (or the key, of a set); empty if there was none or it raised.
    JSValue remove(JSGlobalObject*, JSValue key);
    void removeEntry(VM&, unsigned entry);
    void clear(VM&);
    void copyFrom(VM&, JSGlobalObject*, PyHashTable&);
    // What is in the other, which is left with nothing.
    void takeFrom(VM&, PyHashTable&);
    // After a good deal may have been removed: if more than a quarter of the places in the index are ones that have been given up, it is made again without them, as CPython's set is.
    void tidyAfterRemoving(VM&, JSGlobalObject*);

    // To go through the entries: from 0 up to entryCount(), skipping those whose key is empty. Adding to the table while doing so may
    // move them, which version() tells.
    unsigned entryCount() const { return m_used; }
    // There is nothing before this one. It has not been removed, and nor has the last, unless there are none at all.
    unsigned firstEntry() const { return m_first; }
    JSValue keyAt(unsigned entry) const { return m_storage->key(entry).get(); }
    uint32_t hashAt(unsigned entry) const { return m_storage->hash(entry); }
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
    bool grow(VM&, JSGlobalObject*);
    // Whether it took a place in the index that nothing had had.
    bool insertIndex(PyHashStorage&, uint32_t hash, unsigned entry);

    WriteBarrier<PyHashStorage> m_storage;
    // The entries are from m_first to m_used, of which some in between may have been removed. What is removed from either end is done with there and then, so that taking from an end does not go over what
    // was taken before.
    unsigned m_first { 0 };
    unsigned m_used { 0 };
    // Places in the index that have or have had something in them. Looking for what is not there ends at one that has not, so there are always to be some of those.
    unsigned m_filled { 0 };
    unsigned m_size { 0 };
    unsigned m_version { 0 };
    unsigned m_stride;
};

// A dict can be backed by an object. Then its items with strings for keys are the properties of that object, and only the others are
// in its own table. That is what the __dict__ of an instance or of a module is, and globals(): the attributes are where property
// access finds them, and the dict is another way of getting at them, so neither can be out of date. A dict that is given to exec()
// to be its globals becomes one.
//
// The items in the object come before the others. That is not the order in which they were added if strings and other keys are
// mixed, which is the one way in which such a dict differs.
class PyDict final : public PyHashTable {
public:
    using Base = PyHashTable;
    PYTHON_DECLARE_EXOTIC_METHODS
    PYTHON_OVERLOADS_OPERATORS

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pyDictSpace<mode>();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    static PyDict* create(VM&, Structure*);
    static PyDict* create(JSGlobalObject*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    // The dict that is backed by the object. There is one, which the object remembers.
    static PyDict* backedBy(JSGlobalObject*, JSObject*);
    // The object that it is backed by, which is made now if it had none: a bare object, that is nothing but somewhere to keep properties.
    JSObject* ensureBacking(JSGlobalObject*);
    // Moves what has strings for keys to the object, which is to have no properties yet.
    void becomeBackedBy(JSGlobalObject*, JSObject*);
    // Takes what is in the object for its own, and has no more to do with it. The object is left with no such properties.
    void detach(JSGlobalObject*);
    JSObject* backing() const { return m_backing.get(); }

    unsigned size() const { return Base::size() + (m_backing ? backingSize() : 0); }
    // Empty if there is none, or if it raised.
    JSValue get(JSGlobalObject*, JSValue key);
    // False if there is none, or if it raised.
    bool contains(JSGlobalObject*, JSValue key);
    bool set(JSGlobalObject* globalObject, JSValue key, JSValue value) { return add(globalObject, key, value); }
    bool add(JSGlobalObject*, JSValue key, JSValue value, bool* wasAdded = nullptr, bool replace = true);
    JSValue remove(JSGlobalObject*, JSValue key);
    // The last item. False if there are none.
    bool removeLast(JSGlobalObject*, JSValue& key, JSValue& value);
    void clear(JSGlobalObject*);
    void copyFrom(JSGlobalObject*, PyDict&);
    JSValue getString(JSGlobalObject*, const String&);
    bool setString(JSGlobalObject*, const String& key, JSValue value);

    // Calls the function with each key and value in order, until it returns false. What is added meanwhile may or may not be seen, and
    // what is removed is not.
    template<typename Function> void forEach(JSGlobalObject*, const Function&);

    // For iterators: the keys that are in the object now, and the rest of the dict.
    PyTuple* backingKeys(JSGlobalObject*);
    JS_EXPORT_PRIVATE Vector<RefPtr<UniquedStringImpl>, 16> backingNames(VM&);
    JS_EXPORT_PRIVATE JSValue backingValue(VM&, PropertyName);
    PyHashTable& ownTable() { return *this; }

    // Where an item is depends on the key, so there is no going through them by number.
    unsigned entryCount() const = delete;
    JSValue keyAt(unsigned) const = delete;
    JSValue valueAt(unsigned) const = delete;
    void removeEntry(VM&, unsigned) = delete;
    int find(JSGlobalObject*, JSValue) = delete;

private:
    PyDict(VM& vm, Structure* structure)
        : Base(vm, structure, 2)
    {
    }

    JS_EXPORT_PRIVATE unsigned backingSize() const;
    // The property that a key stands for, if this dict is backed and the key is a string.
    bool isInBacking(JSGlobalObject*, JSValue key, Identifier&);

    WriteBarrier<JSObject> m_backing;
};

// set and frozenset.
class PySet final : public PyHashTable {
public:
    using Base = PyHashTable;
    PYTHON_DECLARE_EXOTIC_METHODS
    PYTHON_OVERLOADS_OPERATORS

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pySetSpace<mode>();
    }

    DECLARE_EXPORT_INFO;

    static PySet* create(VM&, Structure*);
    static PySet* create(JSGlobalObject*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    bool addWithHash(JSGlobalObject* globalObject, JSValue key, uint32_t hash) { return Base::addWithHash(globalObject, key, hash, JSValue(), nullptr, false); }
    bool add(JSGlobalObject* globalObject, JSValue key, bool* wasAdded = nullptr) { return Base::add(globalObject, key, JSValue(), wasAdded, false); }

private:
    PySet(VM& vm, Structure* structure)
        : Base(vm, structure, 1)
    {
    }
};

template<typename Function>
void PyDict::forEach(JSGlobalObject* globalObject, const Function& function)
{
    VM& vm = globalObject->vm();
    if (m_backing) {
        for (auto& name : backingNames(vm)) {
            // It may have gone since, or become something else.
            JSValue value = backingValue(vm, name.get());
            if (value && !function(jsString(vm, String(name.get())), value))
                return;
        }
    }
    for (unsigned entry = Base::firstEntry(); entry < Base::entryCount(); ++entry) {
        JSValue key = Base::keyAt(entry);
        if (key && !function(key, Base::valueAt(entry)))
            return;
    }
}

inline bool isDict(JSValue value) { return value.isCell() && value.asCell()->type() == PyDictType; }
inline PyDict* asDict(JSValue value) { return uncheckedDowncast<PyDict>(value.asCell()); }
inline bool isSet(JSValue value) { return value.isCell() && value.asCell()->type() == PySetType; }

} // namespace JSC
