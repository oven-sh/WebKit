/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyDict.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyNumbers.h"
#include "PyRuntime.h"
#include "PyStr.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// ---- PyDictStorage ----

const ClassInfo PyDictStorage::s_info = { "PyDictStorage"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(PyDictStorage) };

PyDictStorage::PyDictStorage(VM& vm, Structure* structure, unsigned log2Size, bool hasWideHashes)
    : Base(vm, structure)
    , m_hasWideHashes(hasWideHashes)
    , m_log2Size(log2Size)
    , m_capacity(capacityFor(log2Size))
{
    memset(static_cast<void*>(entries()), 0, m_capacity * sizeof(Entry));
    memset(indices(), 0xff, static_cast<size_t>(indexWidth(log2Size)) << log2Size);
}

PyDictStorage* PyDictStorage::create(VM& vm, Structure* structure, unsigned log2Size, bool hasWideHashes)
{
    auto* storage = new (NotNull, allocateCell<PyDictStorage>(vm, allocationSize(log2Size, hasWideHashes))) PyDictStorage(vm, structure, log2Size, hasWideHashes);
    storage->finishCreation(vm);
    return storage;
}

PyDictStorage* PyDictStorage::createCopy(VM& vm, PyDictStorage* other, bool hasWideHashes)
{
    PyDictStorage* storage = create(vm, other->structure(), other->m_log2Size, hasWideHashes);
    // (The cell is new, so no barrier is needed.)
    for (unsigned i = 0; i < other->m_entryCount; ++i) {
        storage->entries()[i].key.setWithoutWriteBarrier(other->entries()[i].key.get());
        storage->entries()[i].value.setWithoutWriteBarrier(other->entries()[i].value.get());
    }
    if (hasWideHashes == other->m_hasWideHashes) {
        // The hashes and the index lie behind the entries.
        memcpy(static_cast<void*>(storage->entries() + storage->m_capacity), other->entries() + other->m_capacity, allocationSize(other->m_log2Size, hasWideHashes) - sizeof(PyDictStorage) - other->m_capacity * sizeof(Entry));
    } else {
        for (unsigned i = 0; i < other->m_entryCount; ++i)
            storage->setHashAt(i, other->hashAt(i));
        memcpy(storage->indices(), other->indices(), static_cast<size_t>(indexWidth(other->m_log2Size)) << other->m_log2Size);
    }
    storage->m_entryCount = other->m_entryCount;
    storage->m_insertionCount = other->m_insertionCount;
    return storage;
}

template<typename Visitor>
void PyDictStorage::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyDictStorage>(cell);
    Base::visitChildren(thisObject, visitor);
    // All of them, used or not: the count may be changing.
    visitor.appendValues(&thisObject->entries()->key, thisObject->m_capacity * 2);
}

DEFINE_VISIT_CHILDREN(PyDictStorage);

// ---- PyDict ----

PY_DEFINE_CELL(PyDict);

PyDict::PyDict(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyDict* PyDict::create(VM& vm, Structure* structure, PyType* type)
{
    auto* dict = new (NotNull, allocateCell<PyDict>(vm)) PyDict(vm, structure, type);
    dict->finishCreation(vm);
    return dict;
}

PyDict* PyDict::create(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return create(rt.vm(), rt.structure<PyDict>(), rt.dictType());
}

PyDict* PyDict::createWithCapacity(JSGlobalObject* globalObject, unsigned capacity, bool onlyStringKeys)
{
    PyDict* dict = create(globalObject);
    dict->m_hasOnlyStringKeys = onlyStringKeys;
    dict->m_isKeyed = !onlyStringKeys;
    if (capacity)
        dict->resize(getVM(globalObject), capacity);
    return dict;
}

template<typename Visitor>
void PyDict::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyDict>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_storage);
}

DEFINE_VISIT_CHILDREN(PyDict);

// The slots of the index that a search goes through, which are CPython's: what is kept with a key, taken for an unsigned number, says where
// it begins, and the rest of its bits come in five at a time. (A program can tell, by which of its __eq__ are asked, and how often.)
class SlotSequence {
public:
    ALWAYS_INLINE SlotSequence(int64_t kept, uint32_t mask)
        : m_perturb(static_cast<uint64_t>(kept))
        , m_mask(mask)
        , m_slot(static_cast<uint32_t>(kept) & mask)
    {
    }
    ALWAYS_INLINE uint32_t slot() const { return m_slot; }
    ALWAYS_INLINE void advance()
    {
        m_perturb >>= 5;
        m_slot = (m_slot * 5 + static_cast<uint32_t>(m_perturb) + 1) & m_mask;
    }

private:
    uint64_t m_perturb;
    uint32_t m_mask;
    uint32_t m_slot;
};

// See "By what a key is placed" in PyDict.h.
//
// What a search for a str may cost before the dict becomes keyed. A slot looked at costs 1: a table has two slots in three taken at most,
// so a search for a key that is not there looks at three on average, and one in 10^11 looks at more than 64 by chance. A slot with
// another str that has the same 24 bits costs 16 more: chance brings that about once in 16 million, so four in one search are somebody's
// doing. (Without that, keys chosen to collide would do their worst in many dicts of 64 keys each.)
static constexpr unsigned defaultLongestSearchByTextHash = 64;
static constexpr unsigned costOfSameTextHash = 16;
// A StringImpl's hash has 24 bits. In a table of 2^21 slots, which is for more than 699,050 entries, three of them are left above the number of
// the slot, and one str in 24 or more shares all 24 with another, so searches get longer. Measured: up to that size to go by hash() gains
// nothing; a dict of 1.4 million strs is made in 72 % of the time, read in 78 %, and asked for what it has not in 56 %.
static constexpr unsigned defaultLog2SizeToBeKeyedFrom = 21;
#if ASSERT_ENABLED
static unsigned longestSearchByTextHash = defaultLongestSearchByTextHash;
static unsigned log2SizeToBeKeyedFrom = defaultLog2SizeToBeKeyedFrom;
void PyDict::setLimitsForTesting(std::optional<unsigned> longestSearch, std::optional<unsigned> log2Size)
{
    longestSearchByTextHash = longestSearch.value_or(defaultLongestSearchByTextHash);
    log2SizeToBeKeyedFrom = log2Size.value_or(defaultLog2SizeToBeKeyedFrom);
}
#else
static constexpr unsigned longestSearchByTextHash = defaultLongestSearchByTextHash;
static constexpr unsigned log2SizeToBeKeyedFrom = defaultLog2SizeToBeKeyedFrom;
#endif

NEVER_INLINE int64_t PyDict::keyedHashToKeepOfString(JSString* string) const
{
    int64_t hash = hashOfString(string->tryGetValueImpl());
    return keepsWholeHashes() ? hash : static_cast<int64_t>(foldHash(hash));
}

ALWAYS_INLINE int64_t PyDict::hashToKeepOfString(JSString* string) const
{
    ASSERT(!string->isRope());
    if (!m_isKeyed) [[likely]]
        return string->tryGetValueImpl()->hash();
    return keyedHashToKeepOfString(string);
}

ALWAYS_INLINE int64_t PyDict::hashToKeep(JSValue key, int64_t hash) const
{
    return key.isString() ? hashToKeepOfString(asString(key)) : hash;
}

// Of a key that is in the table, or was about to be put there, when what is kept for strs has changed.
ALWAYS_INLINE int64_t PyDict::keyedHashToKeep(JSValue key, int64_t kept) const
{
    ASSERT(m_isKeyed);
    return key.isString() ? keyedHashToKeepOfString(asString(key)) : kept;
}

// The index anew, for a table of strs.
void PyDict::placeStringsByHash()
{
    ASSERT(m_isKeyed && m_hasOnlyStringKeys);
    PyDictStorage* storage = m_storage.get();
    Entry* entries = storage->entries();
    uint32_t mask = storage->mask();
    memset(storage->indices(), 0xff, static_cast<size_t>(PyDictStorage::indexWidth(storage->log2Size())) << storage->log2Size());
    for (unsigned i = 0; i < storage->entryCount(); ++i) {
        JSValue key = entries[i].key.get();
        if (!key)
            continue;
        int64_t kept = keyedHashToKeepOfString(asString(key));
        storage->setHashAt(i, kept);
        SlotSequence sequence(kept, mask);
        while (storage->indexAt(sequence.slot()) != PyDictStorage::emptyIndex)
            sequence.advance();
        storage->setIndexAt(sequence.slot(), i);
    }
}

void PyDict::becomeKeyed()
{
    ASSERT(!m_isKeyed && !keepsWholeHashes());
    m_isKeyed = true;
    if (m_storage)
        placeStringsByHash();
}

// The entries stay where they are here too, but in a table with room for the hashes.
NEVER_INLINE void PyDict::keepWholeHashesOfStrings(VM& vm)
{
    ASSERT(!keepsWholeHashes());
    m_keepsWholeHashesOfStrings = true;
    m_isKeyed = true;
    if (PyDictStorage* storage = m_storage.get()) {
        PyDictStorage* wide = PyDictStorage::createCopy(vm, storage, true);
        vm.mutatorFence();
        m_storage.set(vm, this, wide);
        placeStringsByHash();
    }
}

static ALWAYS_INLINE bool isInstanceOfStrSubclass(JSValue value) { return value.isCell() && value.asCell()->type() == PyStrType; }

// What looking for `key`, which is not a str, comes to in a dict of strs. `hash`: hash(key), or what somebody has kept as that.
PyDict::SearchAmongStrings PyDict::searchAmongStringsFor(JSGlobalObject* globalObject, JSValue key, int64_t hash)
{
    if (!key.isCell())
        return SearchAmongStrings::IsInVain;
    Runtime& rt = runtime(globalObject);
    PyType* type = rt.typeOf(key);
    JSValue equality = type->lookup(Special::Eq);
    JSValue hashing = type->lookup(Special::Hash);
    if (isInstanceOfStrSubclass(key)) {
        // It is as good as the str it holds if its class leaves both questions to str, and `hash` is what str says. That need not be: a
        // set or another dict may have kept it since before the class was changed. (Nor has str hashed what is still a rope.)
        if (equality == rt.strType()->lookup(Special::Eq) && hashing == rt.strType()->lookup(Special::Hash)) {
            if (StringImpl* impl = strValue(key)->tryGetValueImpl(); impl && hashOfString(impl) == hash)
                return SearchAmongStrings::GoesByText;
        }
        return SearchAmongStrings::AsksKeys;
    }
    // Of the methods written in C++, none but str's says that its object is equal to a str, unless it passes the question on.
    if (type->isNativeSpecialTaking(Special::Eq, equality, 1) && type->isNativeSpecialTaking(Special::Hash, hashing, 0) && !type->has(TypeFlag::AnswersForAnother)) [[likely]]
        return SearchAmongStrings::IsInVain;
    return SearchAmongStrings::AsksKeys;
}

int64_t PyDict::hashIfNeeded(JSGlobalObject* globalObject, JSValue key)
{
    if (key.isString()) [[likely]]
        return 0;
    return hash(globalObject, key);
}

ALWAYS_INLINE int64_t PyDict::hashKey(JSGlobalObject* globalObject, JSValue key)
{
    if (key.isString()) [[likely]]
        return 0;
    return hashForContainer(globalObject, key, type()->has(TypeFlag::DictSubclass) ? "a dict key"_s : "a frozendict key"_s);
}

static ALWAYS_INLINE bool stringsAreEqual(StringImpl* a, StringImpl* b)
{
    if (a == b)
        return true;
    if (a->isAtom() && b->isAtom())
        return false;
    return WTF::equal(a, b);
}

template<bool isKnownNotToBeKeyed>
ALWAYS_INLINE int32_t PyDict::findString(JSString* key, int64_t& kept, uint32_t* slotResult)
{
    ASSERT(!isKnownNotToBeKeyed || !m_isKeyed);
    kept = isKnownNotToBeKeyed ? key->tryGetValueImpl()->hash() : hashToKeepOfString(key);
    PyDictStorage* storage = m_storage.get();
    if (!storage)
        return -1;
    StringImpl* impl = key->tryGetValueImpl();
    Entry* entries = storage->entries();
    SlotSequence sequence(kept, storage->mask());
    unsigned steps = 0;
    while (true) {
        int32_t index = storage->indexAt(sequence.slot());
        if (index == PyDictStorage::emptyIndex)
            return -1;
        if (index >= 0 && storage->hashAt(index) == kept) {
            JSValue candidate = entries[index].key.get();
            // A key that is an instance of a class derived from str goes by its text here: this is how attributes are found.
            if (candidate.isString() || isInstanceOfStrSubclass(candidate)) {
                JSString* string = strValue(candidate);
                if (string == key || (!string->isRope() && stringsAreEqual(string->tryGetValueImpl(), impl))) {
                    if (slotResult)
                        *slotResult = sequence.slot();
                    return index;
                }
            }
            steps += costOfSameTextHash;
        }
        if (++steps > longestSearchByTextHash) [[unlikely]] {
            if (!m_isKeyed)
                return findStringOnceKeyed(key, kept, slotResult);
        }
        sequence.advance();
    }
}

NEVER_INLINE int32_t PyDict::findStringOnceKeyed(JSString* key, int64_t& kept, uint32_t* slotResult)
{
    becomeKeyed();
    return findString(key, kept, slotResult);
}

// (The ways of a str and of any other key part early, in functions of their own: neither is to pay for the registers of the other.)
int32_t PyDict::find(JSGlobalObject* globalObject, JSValue key, int64_t hash, uint32_t* slotResult)
{
    if (key.isString())
        return findStringAmongAll(globalObject, asString(key), slotResult);
    return findOther(globalObject, key, hash, slotResult);
}

NEVER_INLINE int32_t PyDict::findStringAmongStrings(JSString* key, uint32_t* slotResult)
{
    int64_t kept;
    return findString(key, kept, slotResult);
}

int32_t PyDict::findStringAmongAll(JSGlobalObject* globalObject, JSString* key, uint32_t* slotResult)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (key->isRope()) {
        key->value(globalObject);
        RETURN_IF_EXCEPTION(scope, -1);
    }
    // What is not a str may still equal one, or say that it does not although its text is the same. The keys are asked as the search comes by them.
    if (!m_hasOnlyStringKeys) [[unlikely]]
        RELEASE_AND_RETURN(scope, findOther(globalObject, key, hashOfString(key->tryGetValueImpl()), slotResult));
    int64_t kept;
    return findString(key, kept, slotResult);
}

// `key`: no str; or a str, not a rope, if the dict has had other keys. `hash`: hash(key).
int32_t PyDict::findOther(JSGlobalObject* globalObject, JSValue key, int64_t hash, uint32_t* slotResult)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

restart:
    if (!keepsWholeHashes()) [[unlikely]] {
        if (!m_used)
            return -1;
        // (The dict has been emptied meanwhile, and filled again.)
        if (key.isString()) [[unlikely]]
            RELEASE_AND_RETURN(scope, findStringAmongAll(globalObject, asString(key), slotResult));
        switch (searchAmongStringsFor(globalObject, key, hash)) {
        case SearchAmongStrings::IsInVain:
            return -1;
        case SearchAmongStrings::GoesByText:
            return findStringAmongStrings(strValue(key), slotResult);
        case SearchAmongStrings::AsksKeys:
            break;
        }
        // An __eq__ of the program's sees what a search comes by: the strs are to be where CPython has them, and that takes their hashes
        // whole. (To look the methods up was to search dicts, of which this may be one.)
        if (!keepsWholeHashes())
            keepWholeHashesOfStrings(vm);
    }

    PyDictStorage* storage = m_storage.get();
    if (!storage)
        return -1;
    Entry* entries = storage->entries();
    SlotSequence sequence(hash, storage->mask());
    while (true) {
        uint32_t slot = sequence.slot();
        int32_t index = storage->indexAt(slot);
        if (index == PyDictStorage::emptyIndex)
            return -1;
        if (index >= 0) [[likely]] {
            JSValue candidate = entries[index].key.get();
            bool equal;
            // The very object is found where the search comes by it, whatever hash it was stored under. Otherwise nobody is asked unless the
            // hashes are the same, whole. A program can tell both.
            if (candidate == key)
                equal = true;
            else if (storage->hashAt(index) != hash)
                equal = false;
            else if (isIdentical(candidate, key))
                equal = true;
            else if (candidate.isNumber() && key.isNumber())
                equal = candidate.asNumber() == key.asNumber();
            else if (candidate.isString() && key.isString())
                equal = stringsAreEqual(asString(candidate)->tryGetValueImpl(), asString(key)->tryGetValueImpl());
            else {
                equal = richCompareBool(globalObject, candidate, key, CompareOperator::Eq);
                RETURN_IF_EXCEPTION(scope, -1);
                // __eq__ may have changed the dict.
                if (m_storage.get() != storage || entries[index].key.get() != candidate)
                    goto restart;
                // Or have taken the key out and put it back: popitem() frees the entry, so it is the same again, but the slot may be another.
                if (storage->indexAt(slot) != index) [[unlikely]]
                    goto restart;
            }
            if (equal) {
                if (slotResult)
                    *slotResult = slot;
                return index;
            }
        }
        sequence.advance();
    }
}

void PyDict::resize(VM& vm, unsigned minimumCapacity, unsigned unusedAtFront)
{
    unsigned log2Size = PyDictStorage::minLog2Size;
    while (PyDictStorage::capacityFor(log2Size) < minimumCapacity)
        ++log2Size;
    RELEASE_ASSERT(log2Size < 31);

    Structure* structure = m_storage ? m_storage->structure() : Runtime::from(this->structure()->realm()).structure<PyDictStorage>();
    PyDictStorage* newStorage = PyDictStorage::create(vm, structure, log2Size, keepsWholeHashes());
    bool becomesKeyed = !m_isKeyed && (log2Size >= log2SizeToBeKeyedFrom || keepsWholeHashes());
    if (PyDictStorage* old = m_storage.get()) {
        // (A table with room for them has the hashes of strs whole.)
        bool rehashesStrings = m_isKeyed && newStorage->hasWideHashes() != old->hasWideHashes();
        Entry* oldEntries = old->entries();
        Entry* entries = newStorage->entries();
        uint32_t mask = newStorage->mask();
    again:
        if (becomesKeyed)
            m_isKeyed = true;
        unsigned count = unusedAtFront;
        for (unsigned i = 0; i < old->entryCount(); ++i) {
            if (!oldEntries[i].key)
                continue;
            // The storage is new, so no barrier is needed.
            entries[count].key.setWithoutWriteBarrier(oldEntries[i].key.get());
            entries[count].value.setWithoutWriteBarrier(oldEntries[i].value.get());
            int64_t hash = old->hashAt(i);
            if (becomesKeyed || rehashesStrings) [[unlikely]]
                hash = keyedHashToKeep(oldEntries[i].key.get(), hash);
            newStorage->setHashAt(count, hash);
            SlotSequence sequence(hash, mask);
            unsigned steps = 0;
            while (newStorage->indexAt(sequence.slot()) != PyDictStorage::emptyIndex) {
                // Keys that were far from each other in the old table may have been chosen to meet in this one.
                if (++steps > longestSearchByTextHash && !m_isKeyed) [[unlikely]] {
                    becomesKeyed = true;
                    memset(newStorage->indices(), 0xff, static_cast<size_t>(PyDictStorage::indexWidth(log2Size)) << log2Size);
                    goto again;
                }
                sequence.advance();
            }
            newStorage->setIndexAt(sequence.slot(), count);
            ++count;
        }
        newStorage->setEntryCount(count);
        newStorage->setInsertionCount(m_used);
        ASSERT(count == m_used + unusedAtFront);
    } else if (becomesKeyed)
        m_isKeyed = true;
    m_firstEntry = unusedAtFront;
    vm.mutatorFence();
    m_storage.set(vm, this, newStorage);
    keysChanged();
}

void PyDict::reserve(VM& vm, unsigned additional, bool onlyStringKeys)
{
    // When CPython does: a table that could hold what is to come, were it empty, is left to grow as it is filled.
    if (m_storage && m_storage->capacity() >= additional)
        return;
    if (!onlyStringKeys)
        m_hasOnlyStringKeys = false;
    resize(vm, m_used + additional);
}

static unsigned capacityToGrowTo(unsigned used)
{
    // A table of at least three times what is in use, as CPython grows: that also gets rid of the deleted entries.
    unsigned log2Size = PyDictStorage::minLog2Size;
    while ((static_cast<uint64_t>(1) << log2Size) < static_cast<uint64_t>(used) * 3)
        ++log2Size;
    return PyDictStorage::capacityFor(log2Size);
}

// Where in the index a new entry goes. `hash`: what to keep with the key, which is another if this makes the dict keyed.
ALWAYS_INLINE uint32_t PyDict::freeSlotFor(JSValue key, int64_t& hash)
{
    PyDictStorage* storage = m_storage.get();
    SlotSequence sequence(hash, storage->mask());
    unsigned steps = 0;
    while (storage->indexAt(sequence.slot()) >= 0) {
        // No search need have come this way before: the key may be from another dict.
        if (++steps > longestSearchByTextHash && !m_isKeyed) [[unlikely]] {
            becomeKeyed();
            hash = keyedHashToKeep(key, hash);
            sequence = SlotSequence(hash, storage->mask());
            continue;
        }
        sequence.advance();
    }
    return sequence.slot();
}

void PyDict::insertNew(VM& vm, JSValue key, int64_t hash, JSValue value)
{
    PyDictStorage* storage = m_storage.get();
    // With the first key that is not a str the table is made anew, as CPython's is.
    bool isFirstOfAnotherKind = m_hasOnlyStringKeys && !key.isString();
    if (isFirstOfAnotherKind)
        m_hasOnlyStringKeys = false;
    if (!storage || storage->entryCount() == storage->capacity() || storage->insertionCount() >= storage->capacity() || isFirstOfAnotherKind) {
        bool wasKeyed = m_isKeyed;
        resize(vm, capacityToGrowTo(m_used));
        if (m_isKeyed != wasKeyed) [[unlikely]]
            hash = keyedHashToKeep(key, hash);
        storage = m_storage.get();
    }
    uint32_t slot = freeSlotFor(key, hash);
    storage->setInsertionCount(storage->insertionCount() + 1);
    unsigned index = storage->entryCount();
    storage->entries()[index].key.set(vm, storage, key);
    storage->entries()[index].value.set(vm, storage, value);
    storage->setHashAt(index, hash);
    storage->setIndexAt(slot, index);
    storage->setEntryCount(index + 1);
    if (!m_used)
        m_firstEntry = index;
    ++m_used;
    keysChanged();
}

void PyDict::insertFirst(VM& vm, JSValue key, int64_t hash, JSValue value)
{
    if (!m_used)
        return insertNew(vm, key, hash, value);
    PyDictStorage* storage = m_storage.get();
    bool isFirstOfAnotherKind = m_hasOnlyStringKeys && !key.isString();
    if (isFirstOfAnotherKind)
        m_hasOnlyStringKeys = false;
    if (!m_firstEntry || storage->insertionCount() >= storage->capacity() || isFirstOfAnotherKind) {
        // As insertNew() grows it, with half of what is free at either end.
        unsigned capacity = capacityToGrowTo(m_used);
        bool wasKeyed = m_isKeyed;
        resize(vm, capacity, (capacity - m_used + 1) / 2);
        if (m_isKeyed != wasKeyed) [[unlikely]]
            hash = keyedHashToKeep(key, hash);
        storage = m_storage.get();
    }
    uint32_t slot = freeSlotFor(key, hash);
    storage->setInsertionCount(storage->insertionCount() + 1);
    unsigned index = --m_firstEntry;
    storage->entries()[index].key.set(vm, storage, key);
    storage->entries()[index].value.set(vm, storage, value);
    storage->setHashAt(index, hash);
    storage->setIndexAt(slot, index);
    ++m_used;
    keysChanged();
}

void PyDict::removeAt(VM&, int32_t index, uint32_t slot)
{
    PyDictStorage* storage = m_storage.get();
    storage->setIndexAt(slot, PyDictStorage::deletedIndex);
    storage->entries()[index].key.clear();
    storage->entries()[index].value.clear();
    --m_used;
    if (static_cast<unsigned>(index) == m_firstEntry) {
        unsigned count = storage->entryCount();
        while (m_firstEntry < count && !storage->entries()[m_firstEntry].key)
            ++m_firstEntry;
    }
    keysChanged();
}

// Where in the index an entry that is in use is.
uint32_t PyDict::slotOfEntry(int32_t index)
{
    PyDictStorage* storage = m_storage.get();
    SlotSequence sequence(storage->hashAt(index), storage->mask());
    while (storage->indexAt(sequence.slot()) != index)
        sequence.advance();
    return sequence.slot();
}

JSValue PyDict::get(JSGlobalObject* globalObject, JSValue key, int64_t hash)
{
    int32_t index = find(globalObject, key, hash);
    return index < 0 ? JSValue() : m_storage->entries()[index].value.get();
}

JSValue PyDict::get(JSGlobalObject* globalObject, JSValue key)
{
    if (key.isString() && !asString(key)->isRope()) [[likely]]
        return getString(globalObject, asString(key));
    return getOther(globalObject, key);
}

JSValue PyDict::getString(JSGlobalObject* globalObject, JSString* key)
{
    if (m_isKeyed) [[unlikely]]
        return m_hasOnlyStringKeys ? get(key) : get(globalObject, key, 0);
    int64_t kept;
    int32_t index = findString<true>(key, kept);
    return index < 0 ? JSValue() : m_storage->entries()[index].value.get();
}

JSValue PyDict::getOther(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (key.isString()) [[unlikely]] {
        if (!m_used)
            return { };
        asString(key)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, getString(globalObject, asString(key)));
    }
    int64_t hash = hashKey(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    int32_t index = findOther(globalObject, key, hash, nullptr);
    return index < 0 ? JSValue() : m_storage->entries()[index].value.get();
}

void PyDict::set(JSGlobalObject* globalObject, JSValue key, int64_t hash, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t index = find(globalObject, key, hash);
    RETURN_IF_EXCEPTION(scope, void());
    if (index >= 0) {
        m_storage->entries()[index].value.set(vm, m_storage.get(), value);
        return;
    }
    insertNew(vm, key, hashToKeep(key, hash), value);
}

void PyDict::set(JSGlobalObject* globalObject, JSValue key, JSValue value)
{
    if (key.isString() && !asString(key)->isRope()) [[likely]]
        return setString(globalObject, asString(key), value);
    return setOther(globalObject, key, value);
}

void PyDict::setString(JSGlobalObject* globalObject, JSString* key, JSValue value)
{
    VM& vm = getVM(globalObject);
    if (m_isKeyed) [[unlikely]]
        return m_hasOnlyStringKeys ? set(vm, key, value) : set(globalObject, key, 0, value);
    int64_t kept;
    int32_t index = findString<true>(key, kept);
    if (index >= 0) {
        m_storage->entries()[index].value.set(vm, m_storage.get(), value);
        return;
    }
    insertNew(vm, key, kept, value);
}

void PyDict::setOther(JSGlobalObject* globalObject, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (key.isString()) [[unlikely]] {
        asString(key)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, setString(globalObject, asString(key), value));
    }
    int64_t hash = hashKey(globalObject, key);
    RETURN_IF_EXCEPTION(scope, void());
    int32_t index = findOther(globalObject, key, hash, nullptr);
    RETURN_IF_EXCEPTION(scope, void());
    if (index >= 0) {
        m_storage->entries()[index].value.set(vm, m_storage.get(), value);
        return;
    }
    insertNew(vm, key, hashToKeep(key, hash), value);
}

JSValue PyDict::setDefault(JSGlobalObject* globalObject, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hash = hashKey(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    int32_t index = find(globalObject, key, hash);
    RETURN_IF_EXCEPTION(scope, { });
    if (index >= 0)
        return m_storage->entries()[index].value.get();
    insertNew(vm, key, hashToKeep(key, hash), value);
    return value;
}

JSValue PyDict::take(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hash = hashKey(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, take(globalObject, key, hash));
}

JSValue PyDict::take(JSGlobalObject* globalObject, JSValue key, int64_t hash)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t slot;
    int32_t index = find(globalObject, key, hash, &slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (index < 0)
        return { };
    JSValue value = m_storage->entries()[index].value.get();
    removeAt(vm, index, slot);
    return value;
}

uint64_t PyDict::newWatchedVersion()
{
    // One for all VMs: it is cheaper than finding the runtime of the dict.
    static std::atomic<uint64_t> last { notWatched };
    return last.fetch_add(1, std::memory_order_relaxed) + 1;
}

int32_t PyDict::indexOf(JSString* key)
{
    if (!m_used)
        return -1;
    int64_t kept;
    return findString(key, kept);
}

JSValue PyDict::get(JSString* key)
{
    if (!m_used)
        return { };
    int64_t kept;
    int32_t index = findString(key, kept);
    return index < 0 ? JSValue() : m_storage->entries()[index].value.get();
}

void PyDict::set(VM& vm, JSString* key, JSValue value)
{
    int64_t kept;
    int32_t index = findString(key, kept);
    if (index >= 0) {
        m_storage->entries()[index].value.set(vm, m_storage.get(), value);
        return;
    }
    insertNew(vm, key, kept, value);
}

JSValue PyDict::take(VM& vm, JSString* key)
{
    uint32_t slot;
    int64_t kept;
    int32_t index = findString(key, kept, &slot);
    if (index < 0)
        return { };
    JSValue value = m_storage->entries()[index].value.get();
    removeAt(vm, index, slot);
    return value;
}

void PyDict::clear(VM&)
{
    m_storage.clear();
    m_used = 0;
    m_hasOnlyStringKeys = true;
    m_isKeyed = false;
    m_keepsWholeHashesOfStrings = false;
    keysChanged();
}

PyDict* PyDict::copy(JSGlobalObject* globalObject)
{
    PyDict* result = create(globalObject);
    if (hasFewDeletedEntries())
        result->takeTableOf(getVM(globalObject), this);
    else
        result->takeItemsOf(getVM(globalObject), this);
    return result;
}

void PyDict::takeTableOf(VM& vm, PyDict* other)
{
    ASSERT(!m_used && other != this);
    if (!other->m_used)
        return;
    PyDictStorage* storage = PyDictStorage::createCopy(vm, other->m_storage.get(), other->m_storage->hasWideHashes());
    m_used = other->m_used;
    m_hasOnlyStringKeys = other->m_hasOnlyStringKeys;
    m_isKeyed = other->m_isKeyed;
    m_keepsWholeHashesOfStrings = other->m_keepsWholeHashesOfStrings;
    m_firstEntry = other->m_firstEntry;
    vm.mutatorFence();
    m_storage.set(vm, this, storage);
    keysChanged();
}

void PyDict::takeItemsOf(VM& vm, PyDict* other)
{
    ASSERT(!m_used && other != this);
    PyDictStorage* storage = other->m_storage.get();
    if (!storage || !other->m_used)
        return;
    // This dict may have had entries, and what is left of them in its table. It begins again, as the other is: what that keeps with its
    // entries is then good here.
    m_storage.clear();
    m_hasOnlyStringKeys = true;
    m_isKeyed = other->m_isKeyed;
    m_keepsWholeHashesOfStrings = other->m_keepsWholeHashesOfStrings;
    reserve(vm, other->m_used, other->m_hasOnlyStringKeys);
    for (unsigned i = 0; i < storage->entryCount(); ++i) {
        Entry& entry = storage->entries()[i];
        if (!entry.key)
            continue;
        int64_t kept = storage->hashAt(i);
        // Unless this dict has become keyed on the way, for whatever reason.
        if (m_isKeyed != other->m_isKeyed) [[unlikely]]
            kept = keyedHashToKeep(entry.key.get(), kept);
        insertNew(vm, entry.key.get(), kept, entry.value.get());
    }
}

bool PyDict::next(unsigned& position, JSValue& key, JSValue& value) const
{
    PyDictStorage* storage = m_storage.get();
    if (!storage)
        return false;
    unsigned count = storage->entryCount();
    if (position < m_firstEntry && m_used)
        position = m_firstEntry;
    while (position < count) {
        Entry& entry = storage->entries()[position++];
        if (entry.key) {
            key = entry.key.get();
            value = entry.value.get();
            return true;
        }
    }
    return false;
}

bool PyDict::next(unsigned& position, JSValue& key, JSValue& value, int64_t& hash) const
{
    if (!next(position, key, value))
        return false;
    hash = m_storage->hashAt(position - 1);
    return true;
}

bool PyDict::previous(unsigned& position, JSValue& key, JSValue& value) const
{
    PyDictStorage* storage = m_storage.get();
    if (!storage)
        return false;
    position = std::min(position, storage->entryCount());
    unsigned first = m_used ? m_firstEntry : 0;
    while (position > first) {
        Entry& entry = storage->entries()[--position];
        if (entry.key) {
            key = entry.key.get();
            value = entry.value.get();
            return true;
        }
    }
    return false;
}

bool PyDict::takeLast(VM& vm, JSValue& key, JSValue& value)
{
    if (!m_used)
        return false;
    PyDictStorage* storage = m_storage.get();
    unsigned index = storage->entryCount();
    while (!storage->entries()[index - 1].key)
        --index;
    --index;
    key = storage->entries()[index].key.get();
    value = storage->entries()[index].value.get();
    removeAt(vm, index, slotOfEntry(index));
    // What follows it is all deleted.
    storage->setEntryCount(index);
    return true;
}

bool PyDict::takeFirst(VM& vm, JSValue& key, JSValue& value)
{
    if (!m_used)
        return false;
    PyDictStorage* storage = m_storage.get();
    unsigned index = m_firstEntry;
    ASSERT(storage->entries()[index].key);
    key = storage->entries()[index].key.get();
    value = storage->entries()[index].value.get();
    removeAt(vm, index, slotOfEntry(index));
    return true;
}

bool PyDict::moveToEnd(JSGlobalObject* globalObject, JSValue key, int64_t hash, bool last)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!m_used)
        return false;
    uint32_t slot;
    int32_t index = find(globalObject, key, hash, &slot);
    RETURN_IF_EXCEPTION(scope, false);
    if (index < 0)
        return false;
    PyDictStorage* storage = m_storage.get();
    if (last) {
        unsigned end = storage->entryCount();
        while (!storage->entries()[end - 1].key)
            --end;
        storage->setEntryCount(end);
        if (static_cast<unsigned>(index) == end - 1)
            return true;
    } else if (static_cast<unsigned>(index) == m_firstEntry)
        return true;
    // The key that is in the dict stays, not the one that is equal to it.
    JSValue ownKey = storage->entries()[index].key.get();
    JSValue value = storage->entries()[index].value.get();
    int64_t ownHash = storage->hashAt(index);
    removeAt(vm, index, slot);
    if (last)
        insertNew(vm, ownKey, ownHash, value);
    else
        insertFirst(vm, ownKey, ownHash, value);
    return true;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
