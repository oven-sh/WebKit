/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSString.h"
#include "PyObject.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The table of a dict: the entries in the order they were inserted, the hashes of their keys, and an open-addressed index into
// the entries by hash. Its size is fixed, so the collector can visit one while the dict moves on to another.
//
// The hash of a str fits 32 bits. A table that holds other keys has room for 64, so that whoever takes the keys from the dict (a
// copy, a set) need not ask them for their hashes again, which a program can tell.
class alignas(WriteBarrier<Unknown>) PyDictStorage final : public JSCell {
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
        WriteBarrier<Unknown> key; // Empty: deleted.
        WriteBarrier<Unknown> value;
    };

    static constexpr int32_t emptyIndex = -1;
    static constexpr int32_t deletedIndex = -2;
    static constexpr unsigned minLog2Size = 3;

    static constexpr unsigned capacityFor(unsigned log2Size) { return static_cast<unsigned>((static_cast<uint64_t>(1) << log2Size) * 2 / 3); }
    static constexpr unsigned indexWidth(unsigned log2Size) { return log2Size < 8 ? 1 : log2Size < 16 ? 2 : 4; }
    static size_t allocationSize(unsigned log2Size, bool hasWideHashes)
    {
        size_t capacity = capacityFor(log2Size);
        return sizeof(PyDictStorage) + capacity * (sizeof(Entry) + (hasWideHashes ? sizeof(int64_t) : sizeof(uint32_t))) + (static_cast<size_t>(indexWidth(log2Size)) << log2Size);
    }

    static PyDictStorage* create(VM&, Structure*, unsigned log2Size, bool hasWideHashes);

    unsigned log2Size() const { return m_log2Size; }
    uint32_t mask() const { return (1u << m_log2Size) - 1; }
    unsigned capacity() const { return m_capacity; }
    unsigned entryCount() const { return m_entryCount; }
    void setEntryCount(unsigned count) { m_entryCount = count; }
    // How many slots of the table are not empty: in use, or marked as deleted. A lookup ends at an empty one, so some have to be.
    unsigned filledSlotCount() const { return m_filledSlotCount; }
    void setFilledSlotCount(unsigned count) { m_filledSlotCount = count; }

    Entry* entries() { return storageBehind<Entry>(this); }
    static constexpr ptrdiff_t offsetOfFirstValue() { return sizeof(PyDictStorage) + OBJECT_OFFSETOF(Entry, value); }
    bool hasWideHashes() const { return m_hasWideHashes; }
    int64_t hashAt(unsigned index) { return m_hasWideHashes ? wideHashes()[index] : static_cast<int64_t>(narrowHashes()[index]); }
    void setHashAt(unsigned index, int64_t hash)
    {
        if (m_hasWideHashes)
            wideHashes()[index] = hash;
        else {
            ASSERT(hash == static_cast<int64_t>(static_cast<uint32_t>(hash)));
            narrowHashes()[index] = static_cast<uint32_t>(hash);
        }
    }
    void* indices() { return m_hasWideHashes ? static_cast<void*>(wideHashes() + m_capacity) : static_cast<void*>(narrowHashes() + m_capacity); }

    int32_t indexAt(uint32_t slot)
    {
        if (m_log2Size < 8)
            return static_cast<int8_t*>(indices())[slot];
        if (m_log2Size < 16)
            return static_cast<int16_t*>(indices())[slot];
        return static_cast<int32_t*>(indices())[slot];
    }
    void setIndexAt(uint32_t slot, int32_t index)
    {
        if (m_log2Size < 8)
            static_cast<int8_t*>(indices())[slot] = static_cast<int8_t>(index);
        else if (m_log2Size < 16)
            static_cast<int16_t*>(indices())[slot] = static_cast<int16_t>(index);
        else
            static_cast<int32_t*>(indices())[slot] = index;
    }

private:
    PyDictStorage(VM&, Structure*, unsigned log2Size, bool hasWideHashes);

    uint32_t* narrowHashes() { return reinterpret_cast<uint32_t*>(entries() + m_capacity); }
    int64_t* wideHashes() { return reinterpret_cast<int64_t*>(entries() + m_capacity); }

    bool m_hasWideHashes;
    unsigned m_log2Size;
    unsigned m_capacity;
    unsigned m_entryCount { 0 }; // Including the deleted ones.
    unsigned m_filledSlotCount { 0 };
};

class PyDict : public PyObject {
public:
    PY_DECLARE_CELL(PyDict, PyObject, PyDictType);
    DECLARE_VISIT_CHILDREN;

    static PyDict* create(VM&, Structure*, PyType*);
    static PyDict* create(JSGlobalObject*); // Of type dict.
    // `onlyStringKeys`, here and below: false if some of the keys to come are not strs, which saves making the table twice.
    static PyDict* createWithCapacity(JSGlobalObject*, unsigned, bool onlyStringKeys = true);

    unsigned size() const { return m_used; }
    bool isEmpty() const { return !m_used; }

    // With any key, these may run __hash__ and __eq__, and so throw.

    JSValue get(JSGlobalObject*, JSValue key); // Empty: not there, or an exception.
    JSValue get(JSGlobalObject*, JSValue key, int64_t hash);
    bool contains(JSGlobalObject* globalObject, JSValue key) { return !!get(globalObject, key); }
    void set(JSGlobalObject*, JSValue key, JSValue value);
    void set(JSGlobalObject*, JSValue key, int64_t hash, JSValue value);
    JSValue setDefault(JSGlobalObject*, JSValue key, JSValue value); // What the key now maps to.
    JSValue take(JSGlobalObject*, JSValue key); // Removes. Empty: was not there, or an exception.
    JSValue take(JSGlobalObject*, JSValue key, int64_t hash);
    bool remove(JSGlobalObject* globalObject, JSValue key) { return !!take(globalObject, key); }

    // With a str that is not a rope, nothing else runs. Keys that are neither strs nor instances of classes derived from str are
    // passed over, though one might say that it equals the str: whoever minds asks hasOnlyStringKeys() first.

    // (False does not say that there is such a key now: there may have been one, or one may have been announced.)
    bool hasOnlyStringKeys() const { return m_hasOnlyStringKeys; }
    JSValue get(JSString* key);
    void set(VM&, JSString* key, JSValue value);
    JSValue take(VM&, JSString* key);
    bool remove(VM& vm, JSString* key) { return !!take(vm, key); }

    void clear(VM&);
    PyDict* copy(JSGlobalObject*); // Of type dict.
    // For a dict that is empty: the items of the other, in their order. Its keys differ from one another, so none is compared.
    void takeItemsOf(VM&, PyDict* other);
    void reserve(VM&, unsigned additional, bool onlyStringKeys = true);

    // for (unsigned position = 0; dict->next(position, key, value);)
    // Entries added meanwhile may or may not be seen; a position stays valid unless the dict is cleared.
    bool next(unsigned& position, JSValue& key, JSValue& value) const;
    // With the hash of the key, as the dict remembers it: its __hash__ need not be asked again.
    bool next(unsigned& position, JSValue& key, JSValue& value, int64_t& hash) const;
    // From the last. Start at endPosition().
    bool previous(unsigned& position, JSValue& key, JSValue& value) const;
    unsigned endPosition() const { return m_storage ? m_storage->entryCount() : 0; }
    // Another when the entries have moved, or the dict has been cleared.
    const void* storageIdentity() const { return m_storage.get(); }
    // Removes the last entry. False: the dict is empty.
    bool takeLast(VM&, JSValue& key, JSValue& value);
    // The same for the first. Neither takes longer the more entries have been removed: a dict will do as a queue.
    bool takeFirst(VM&, JSValue& key, JSValue& value);
    // Makes the entry of the key the last, or the first. False: there is none, or an exception.
    bool moveToEnd(JSGlobalObject*, JSValue key, int64_t hash, bool last);

    // Changes whenever a key is added or removed, or entries move.
    uint32_t keysVersion() const { return m_keysVersion; }

    // For inline caches. Where a str that is not a rope is among the entries, or -1.
    int32_t indexOf(JSString* key);
    JSValue valueAt(int32_t index) const { return m_storage->entries()[index].value.get(); }
    // A number that no other dict has or had, nor this one when it had other keys or had them in other places: while a dict
    // has the number, what was at an index is there. It costs a little to keep, so it is kept from the first time it is asked for.
    uint64_t watchedVersion()
    {
        if (m_watchedVersion == notWatched)
            m_watchedVersion = newWatchedVersion();
        return m_watchedVersion;
    }
    static constexpr uint64_t notWatched = 1; // Not 0, which is what a cache that has not been filled holds.
    // For whatever else was concluded from the number and no longer holds.
    void renewWatchedVersion() { keysChanged(); }

    static constexpr ptrdiff_t offsetOfStorage() { return OBJECT_OFFSETOF(PyDict, m_storage); }
    static constexpr ptrdiff_t offsetOfWatchedVersion() { return OBJECT_OFFSETOF(PyDict, m_watchedVersion); }

protected:
    PyDict(VM&, Structure*, PyType*);

private:
    using Entry = PyDictStorage::Entry;

    int64_t hashKey(JSGlobalObject*, JSValue key);
    // Where the search of the index starts.
    static uint32_t fold(int64_t hash) { return static_cast<uint32_t>(hash) ^ static_cast<uint32_t>(static_cast<uint64_t>(hash) >> 32); }

    // The index of the entry, or -1. `slot`, if not null: where in the index it is.
    int32_t find(JSGlobalObject*, JSValue key, int64_t hash, uint32_t* slot = nullptr);
    int32_t findString(JSString* key, int64_t hash, uint32_t* slot = nullptr);
    void insertNew(VM&, JSValue key, int64_t hash, JSValue value);
    void insertFirst(VM&, JSValue key, int64_t hash, JSValue value);
    void removeAt(VM&, int32_t index, uint32_t slot);
    uint32_t slotOfEntry(int32_t index);
    // `unusedAtFront`: how many entries to leave free before the first.
    void resize(VM&, unsigned minimumCapacity, unsigned unusedAtFront = 0);
    static uint64_t newWatchedVersion();
    void keysChanged()
    {
        ++m_keysVersion;
        if (m_watchedVersion != notWatched) [[unlikely]]
            m_watchedVersion = newWatchedVersion();
    }

    WriteBarrier<PyDictStorage> m_storage;
    uint64_t m_watchedVersion { notWatched };
    unsigned m_used { 0 };
    uint32_t m_keysVersion { 0 };
    bool m_hasOnlyStringKeys { true };
    unsigned m_firstEntry { 0 }; // The entries before this one are deleted, or were never used. Means nothing while the dict is empty.
};

// hash(str)
int64_t hashOfString(StringImpl*);
inline int64_t hashOfString(JSString* string)
{
    ASSERT(!string->isRope());
    return hashOfString(string->tryGetValueImpl());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
