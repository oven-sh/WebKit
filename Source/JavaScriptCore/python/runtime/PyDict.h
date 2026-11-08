/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "GCMemoryOperations.h"
#include "JSString.h"
#include "PyHash.h"
#include "PyObject.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The table of a dict: the entries in the order they were inserted, the hashes of their keys, and an open-addressed index into
// the entries by hash. Its size is fixed, so the collector can visit one while the dict moves on to another.
//
// A table for strs only keeps 32 bits with each. One that holds other keys has room for 64, the whole hash: whoever takes the keys from the
// dict (a copy, a set) need not ask them for their hashes again, and the index is searched as CPython's is. A program can tell both.
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
    static PyDictStorage* createCopy(VM&, PyDictStorage*, bool hasWideHashes);

    unsigned log2Size() const { return m_log2Size; }
    uint32_t mask() const { return (1u << m_log2Size) - 1; }
    unsigned capacity() const { return m_capacity; }
    unsigned entryCount() const { return m_entryCount; }
    void setEntryCount(unsigned count) { m_entryCount = count; }
    // How many entries the table was made with or has been given since, whatever has become of them. No more than the capacity, as in CPython: so the
    // table is made anew when it is there, and some slots of the index stay empty, where a lookup ends.
    unsigned insertionCount() const { return m_insertionCount; }
    void setInsertionCount(unsigned count) { m_insertionCount = count; }

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
    // Its dict has gone on to another table, which holds what this one held. With reference counts it was freed there and then, and let go of nothing. Here it lies about until it is swept, and whatever
    // still finds it (a word on the stack is enough) would keep all that it names: so it names nothing from here on. That is also what whoever asks the dead what they held (PyFinalizationOrder.cpp) is to hear from it.
    // (Not so the table that clear() leaves: that is how the dict let go of the items.) The collector may be reading the entries meanwhile: it sees each as it was or empty.
    void leaveForAnother()
    {
        gcSafeZeroMemory(reinterpret_cast<JSValue*>(entries()), m_entryCount * sizeof(Entry));
    }

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
    unsigned m_insertionCount { 0 };
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

    // With any key, these may run __hash__ and __eq__, and so throw. `hash`: hash(key). For a key that is exactly a str it is not looked
    // at (hashIfNeeded() saves working it out).

    JSValue get(JSGlobalObject*, JSValue key); // Empty: not there, or an exception.
    JSValue get(JSGlobalObject*, JSValue key, int64_t hash);
    bool contains(JSGlobalObject* globalObject, JSValue key) { return !!get(globalObject, key); }
    void set(JSGlobalObject*, JSValue key, JSValue value);
    void set(JSGlobalObject*, JSValue key, int64_t hash, JSValue value);
    JSValue setDefault(JSGlobalObject*, JSValue key, JSValue value); // What the key now maps to.
    JSValue take(JSGlobalObject*, JSValue key); // Removes. Empty: was not there, or an exception.
    JSValue take(JSGlobalObject*, JSValue key, int64_t hash);
    bool remove(JSGlobalObject* globalObject, JSValue key) { return !!take(globalObject, key); }
    static int64_t hashIfNeeded(JSGlobalObject*, JSValue key); // -1: an exception.

#if ASSERT_ENABLED
    // When a dict becomes keyed (see below): after a search of more than so many steps, from a table of so many slots. Nothing: as usual.
    static void setLimitsForTesting(std::optional<unsigned> longestSearch, std::optional<unsigned> log2Size);
#endif

    // With a str that is not a rope, nothing else runs. Keys that are neither strs nor instances of classes derived from str are
    // passed over, though one might say that it equals the str, and an instance of a class derived from str goes by its text, whatever
    // its __eq__ would say: whoever minds asks hasOnlyStringKeys() first.

    // (False does not say that there is such a key now: there may have been one, or one may have been announced.)
    bool hasOnlyStringKeys() const { return m_hasOnlyStringKeys; }
    JSValue get(JSString* key);
    void set(VM&, JSString* key, JSValue value);
    JSValue take(VM&, JSString* key);
    bool remove(VM& vm, JSString* key) { return !!take(vm, key); }

    void clear(VM&);
    PyDict* copy(JSGlobalObject*); // Of type dict. No key is compared.
    // For a dict that is empty: the items of the other, in their order. Its keys differ from one another, so none is compared.
    void takeItemsOf(VM&, PyDict* other);
    // The same by a copy of the table as it is, with what deletions have left in it. These say when CPython does that, for copy() and for
    // update(): it shows in what later searches come by.
    void takeTableOf(VM&, PyDict* other);
    bool hasFewDeletedEntries() const { return !m_storage || m_used >= m_storage->entryCount() * 2 / 3; }
    bool hasTableThatFits() const
    {
        return m_storage && m_used == m_storage->entryCount() && (m_storage->log2Size() == PyDictStorage::minLog2Size || PyDictStorage::capacityFor(m_storage->log2Size() - 1) < m_used);
    }
    // Before so many keys are added.
    void reserve(VM&, unsigned additional, bool onlyStringKeys = true);

    // for (unsigned position = 0; dict->next(position, key, value);)
    // Entries added meanwhile may or may not be seen; a position stays valid unless the dict is cleared.
    bool next(unsigned& position, JSValue& key, JSValue& value) const;
    // With hash(key), as the dict remembers it: its __hash__ need not be asked again. Not for a key that is exactly a str: another dict does not
    // look at it, and anybody else can ask hash(), which runs no code of the program.
    bool next(unsigned& position, JSValue& key, JSValue& value, int64_t& hash) const;
    // From the last. Start at endPosition().
    bool previous(unsigned& position, JSValue& key, JSValue& value) const;
    unsigned endPosition() const { return m_storage ? m_storage->entryCount() : 0; }
    // Another when the entries have moved, or the dict has been cleared. (Also after a lookup that made a table with room for whole hashes, in which the
    // entries are where they were.)
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

    // hash() of a frozendict, once it has been asked for.
    bool hasCachedHash() const { return m_hash != -1; }
    int64_t cachedHash() const { return m_hash; }
    void setCachedHash(int64_t hash) { m_hash = hash; }

    static constexpr ptrdiff_t offsetOfStorage() { return OBJECT_OFFSETOF(PyDict, m_storage); }
    static constexpr ptrdiff_t offsetOfUsed() { return OBJECT_OFFSETOF(PyDict, m_used); }
    static constexpr ptrdiff_t offsetOfWatchedVersion() { return OBJECT_OFFSETOF(PyDict, m_watchedVersion); }

protected:
    PyDict(VM&, Structure*, PyType*);

private:
    using Entry = PyDictStorage::Entry;

    // ---- By what a key is placed ----
    //
    // hash(str) depends on a key of the process and takes reading the text (PyHash.h). A StringImpl remembers another hash of its text, of
    // 24 bits, the same in every process. A dict of nothing but strs places them by that one, which costs a load. It becomes "keyed", for
    // good (only clear() undoes it), and from then on places them by hash(),
    //  - when it is given a key that is not a str: such a key is placed by hash(), and may be equal to a str;
    //  - before it is asked for a key that might say it is equal to a str and have the hash of one: an object whose __eq__ or __hash__ is
    //    not written in C++ (searchAmongStringsFor());
    //  - when a search takes so long that somebody may have chosen the keys to collide, which is easy with a hash that is the same
    //    everywhere and cannot be done with hash();
    //  - when it gets so large that 24 bits tell too few keys apart.
    // In the first case the table is made anew, as CPython's is at that moment. In the others the index is rebuilt and nothing else: the entries
    // stay where they are, and no key is compared or asked for its hash. So it can happen in the middle of a lookup, while the dict is being
    // iterated over, and caches of where an entry is stay good. (In the second case they are in another table then, with room for whole
    // hashes; in the last two nothing is allocated.)
    //
    // What is kept with an entry ("kept" below), and tells where it goes:
    //  - StringImpl::hash() until the dict is keyed;
    //  - in a dict that is keyed for a long search or for its size, the 32 bits that hash() folds to;
    //  - hash(key) in all other cases.
    // So a dict that has been given a key other than a str, or asked for one whose __eq__ or __hash__ is the program's, has the hashes of all
    // its keys, and they are where CPython has them: what its searches come by, a program can tell.
    void becomeKeyed();
    void keepWholeHashesOfStrings(VM&);
    void placeStringsByHash();
    // What the table has room for.
    bool keepsWholeHashes() const { return !m_hasOnlyStringKeys || m_keepsWholeHashesOfStrings; }
    enum class SearchAmongStrings : uint8_t { IsInVain, GoesByText, AsksKeys };
    SearchAmongStrings searchAmongStringsFor(JSGlobalObject*, JSValue key, int64_t hash);
    uint32_t freeSlotFor(JSValue key, int64_t& hash);
    int64_t hashToKeepOfString(JSString*) const;
    int64_t keyedHashToKeepOfString(JSString*) const;
    int64_t keyedHashToKeep(JSValue key, int64_t kept) const;
    // `hash`: hash(key), not looked at for a str.
    int64_t hashToKeep(JSValue key, int64_t hash) const;
    int64_t hashKey(JSGlobalObject*, JSValue key); // hashIfNeeded(), with the message of a dict if the key has no hash.

    // The index of the entry, or -1. `slot`, if not null: where in the index it is. `hash`: hash(key), not looked at for a str.
    int32_t find(JSGlobalObject*, JSValue key, int64_t hash, uint32_t* slot = nullptr);
    // `kept`: what to keep with the key if it is now added.
    template<bool isKnownNotToBeKeyed = false> int32_t findString(JSString* key, int64_t& kept, uint32_t* slot = nullptr);
    int32_t findStringOnceKeyed(JSString* key, int64_t& kept, uint32_t* slot);
    int32_t findStringAmongStrings(JSString* key, uint32_t* slot);
    int32_t findStringAmongAll(JSGlobalObject*, JSString* key, uint32_t* slot);
    int32_t findOther(JSGlobalObject*, JSValue key, int64_t hash, uint32_t* slot);
    JSValue getString(JSGlobalObject*, JSString* key);
    JSValue getOther(JSGlobalObject*, JSValue key);
    void setString(JSGlobalObject*, JSString* key, JSValue);
    void setOther(JSGlobalObject*, JSValue key, JSValue);
    void insertNew(VM&, JSValue key, int64_t kept, JSValue value);
    void insertFirst(VM&, JSValue key, int64_t kept, JSValue value);
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
    bool m_isKeyed { false };
    bool m_keepsWholeHashesOfStrings { false };
    unsigned m_firstEntry { 0 }; // The entries before this one are deleted, or were never used. Means nothing while the dict is empty.
    int64_t m_hash { -1 }; // (The cell would be as large without: sizes go by 16 bytes.)
};

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
