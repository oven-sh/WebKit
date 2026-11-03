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

PyDictStorage::PyDictStorage(VM& vm, Structure* structure, unsigned log2Size)
    : Base(vm, structure)
    , m_log2Size(log2Size)
    , m_capacity(capacityFor(log2Size))
{
    memset(static_cast<void*>(entries()), 0, m_capacity * sizeof(Entry));
    memset(indices(), 0xff, static_cast<size_t>(indexWidth(log2Size)) << log2Size);
}

PyDictStorage* PyDictStorage::create(VM& vm, Structure* structure, unsigned log2Size)
{
    auto* storage = new (NotNull, allocateCell<PyDictStorage>(vm, allocationSize(log2Size))) PyDictStorage(vm, structure, log2Size);
    storage->finishCreation(vm);
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

PyDict* PyDict::createWithCapacity(JSGlobalObject* globalObject, unsigned capacity)
{
    PyDict* dict = create(globalObject);
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

int64_t hashOfString(StringImpl* impl)
{
    // Never -1, which says that hashing failed.
    return impl->length() ? impl->hash() : 0;
}

static constexpr unsigned perturbShift = 5;

int64_t PyDict::hashKey(JSGlobalObject* globalObject, JSValue key)
{
    if (key.isString() && !asString(key)->isRope()) [[likely]]
        return hashOfString(asString(key));
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

int32_t PyDict::findString(JSString* key, uint32_t hash, uint32_t* slotResult)
{
    PyDictStorage* storage = m_storage.get();
    if (!storage)
        return -1;
    StringImpl* impl = key->tryGetValueImpl();
    ASSERT(impl);
    Entry* entries = storage->entries();
    uint32_t* hashes = storage->hashes();
    uint32_t mask = storage->mask();
    uint32_t perturb = hash;
    uint32_t slot = hash & mask;
    while (true) {
        int32_t index = storage->indexAt(slot);
        if (index == PyDictStorage::emptyIndex)
            return -1;
        if (index >= 0 && hashes[index] == hash) {
            JSValue candidate = entries[index].key.get();
            // A key that is an instance of a class derived from str goes by its text here: this is how attributes are found.
            if (candidate.isString() || (candidate.isCell() && candidate.asCell()->type() == PyStrType)) {
                JSString* string = strValue(candidate);
                if (string == key || (!string->isRope() && stringsAreEqual(string->tryGetValueImpl(), impl))) {
                    if (slotResult)
                        *slotResult = slot;
                    return index;
                }
            }
        }
        perturb >>= perturbShift;
        slot = (slot * 5 + perturb + 1) & mask;
    }
}

int32_t PyDict::find(JSGlobalObject* globalObject, JSValue key, uint32_t hash, uint32_t* slotResult)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (key.isString()) {
        JSString* string = asString(key);
        if (string->isRope()) {
            string->value(globalObject);
            RETURN_IF_EXCEPTION(scope, -1);
        }
        int32_t index = findString(string, hash, slotResult);
        // What is not a str may still equal one: an instance of a subclass of str, for one.
        if (index >= 0 || m_hasOnlyStringKeys)
            return index;
    }

restart:
    PyDictStorage* storage = m_storage.get();
    if (!storage)
        return -1;
    Entry* entries = storage->entries();
    uint32_t* hashes = storage->hashes();
    uint32_t mask = storage->mask();
    uint32_t perturb = hash;
    uint32_t slot = hash & mask;
    while (true) {
        int32_t index = storage->indexAt(slot);
        if (index == PyDictStorage::emptyIndex)
            return -1;
        if (index >= 0 && hashes[index] == hash) {
            JSValue candidate = entries[index].key.get();
            bool equal;
            if (isIdentical(candidate, key))
                equal = true;
            else if (candidate.isNumber() && key.isNumber())
                equal = candidate.asNumber() == key.asNumber();
            else {
                equal = richCompareBool(globalObject, candidate, key, CompareOperator::Eq);
                RETURN_IF_EXCEPTION(scope, -1);
                // __eq__ may have changed the dict.
                if (m_storage.get() != storage || entries[index].key.get() != candidate)
                    goto restart;
            }
            if (equal) {
                if (slotResult)
                    *slotResult = slot;
                return index;
            }
        }
        perturb >>= perturbShift;
        slot = (slot * 5 + perturb + 1) & mask;
    }
}

void PyDict::resize(VM& vm, unsigned minimumCapacity)
{
    unsigned log2Size = PyDictStorage::minLog2Size;
    while (PyDictStorage::capacityFor(log2Size) < minimumCapacity)
        ++log2Size;
    RELEASE_ASSERT(log2Size < 31);

    Structure* structure = m_storage ? m_storage->structure() : Runtime::from(this->structure()->realm()).structure<PyDictStorage>();
    PyDictStorage* newStorage = PyDictStorage::create(vm, structure, log2Size);
    if (PyDictStorage* old = m_storage.get()) {
        Entry* oldEntries = old->entries();
        uint32_t* oldHashes = old->hashes();
        Entry* entries = newStorage->entries();
        uint32_t* hashes = newStorage->hashes();
        uint32_t mask = newStorage->mask();
        unsigned count = 0;
        for (unsigned i = 0; i < old->entryCount(); ++i) {
            if (!oldEntries[i].key)
                continue;
            // The storage is new, so no barrier is needed.
            entries[count].key.setWithoutWriteBarrier(oldEntries[i].key.get());
            entries[count].value.setWithoutWriteBarrier(oldEntries[i].value.get());
            uint32_t hash = oldHashes[i];
            hashes[count] = hash;
            uint32_t perturb = hash;
            uint32_t slot = hash & mask;
            while (newStorage->indexAt(slot) != PyDictStorage::emptyIndex) {
                perturb >>= perturbShift;
                slot = (slot * 5 + perturb + 1) & mask;
            }
            newStorage->setIndexAt(slot, count);
            ++count;
        }
        newStorage->setEntryCount(count);
        newStorage->setFilledSlotCount(count);
        ASSERT(count == m_used);
    }
    m_storage.set(vm, this, newStorage);
    keysChanged();
}

void PyDict::reserve(VM& vm, unsigned additional)
{
    unsigned needed = m_used + additional;
    if (!m_storage || m_storage->capacity() < needed)
        resize(vm, needed);
}

void PyDict::insertNew(VM& vm, JSValue key, uint32_t hash, JSValue value)
{
    PyDictStorage* storage = m_storage.get();
    // (popitem() gives back the entry, not the slot.)
    if (!storage || storage->entryCount() == storage->capacity() || storage->filledSlotCount() >= storage->capacity()) {
        // A table of at least three times what is in use, as CPython grows: that also gets rid of the deleted entries.
        unsigned log2Size = PyDictStorage::minLog2Size;
        while ((static_cast<uint64_t>(1) << log2Size) < static_cast<uint64_t>(m_used) * 3)
            ++log2Size;
        resize(vm, PyDictStorage::capacityFor(log2Size));
        storage = m_storage.get();
    }
    uint32_t mask = storage->mask();
    uint32_t perturb = hash;
    uint32_t slot = hash & mask;
    while (storage->indexAt(slot) >= 0) {
        perturb >>= perturbShift;
        slot = (slot * 5 + perturb + 1) & mask;
    }
    if (storage->indexAt(slot) == PyDictStorage::emptyIndex)
        storage->setFilledSlotCount(storage->filledSlotCount() + 1);
    unsigned index = storage->entryCount();
    storage->entries()[index].key.set(vm, storage, key);
    storage->entries()[index].value.set(vm, storage, value);
    storage->hashes()[index] = hash;
    storage->setIndexAt(slot, index);
    storage->setEntryCount(index + 1);
    ++m_used;
    keysChanged();
    if (!key.isString())
        m_hasOnlyStringKeys = false;
}

void PyDict::removeAt(VM&, int32_t index, uint32_t slot)
{
    PyDictStorage* storage = m_storage.get();
    storage->setIndexAt(slot, PyDictStorage::deletedIndex);
    storage->entries()[index].key.clear();
    storage->entries()[index].value.clear();
    --m_used;
    keysChanged();
}

JSValue PyDict::get(JSGlobalObject* globalObject, JSValue key, int64_t hash)
{
    int32_t index = find(globalObject, key, fold(hash));
    return index < 0 ? JSValue() : m_storage->entries()[index].value.get();
}

JSValue PyDict::get(JSGlobalObject* globalObject, JSValue key)
{
    if (!m_used && key.isString())
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hash = hashKey(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, get(globalObject, key, hash));
}

void PyDict::set(JSGlobalObject* globalObject, JSValue key, int64_t hash, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t folded = fold(hash);
    int32_t index = find(globalObject, key, folded);
    RETURN_IF_EXCEPTION(scope, void());
    if (index >= 0) {
        m_storage->entries()[index].value.set(vm, m_storage.get(), value);
        return;
    }
    insertNew(vm, key, folded, value);
}

void PyDict::set(JSGlobalObject* globalObject, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hash = hashKey(globalObject, key);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, set(globalObject, key, hash, value));
}

JSValue PyDict::setDefault(JSGlobalObject* globalObject, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hash = hashKey(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    uint32_t folded = fold(hash);
    int32_t index = find(globalObject, key, folded);
    RETURN_IF_EXCEPTION(scope, { });
    if (index >= 0)
        return m_storage->entries()[index].value.get();
    insertNew(vm, key, folded, value);
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
    int32_t index = find(globalObject, key, fold(hash), &slot);
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
    return findString(key, fold(hashOfString(key)));
}

JSValue PyDict::get(JSString* key)
{
    if (!m_used)
        return { };
    int32_t index = findString(key, fold(hashOfString(key)));
    return index < 0 ? JSValue() : m_storage->entries()[index].value.get();
}

void PyDict::set(VM& vm, JSString* key, JSValue value)
{
    uint32_t hash = fold(hashOfString(key));
    int32_t index = findString(key, hash);
    if (index >= 0) {
        m_storage->entries()[index].value.set(vm, m_storage.get(), value);
        return;
    }
    insertNew(vm, key, hash, value);
}

JSValue PyDict::take(VM& vm, JSString* key)
{
    uint32_t slot;
    int32_t index = findString(key, fold(hashOfString(key)), &slot);
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
    keysChanged();
}

PyDict* PyDict::copy(JSGlobalObject* globalObject)
{
    PyDict* result = create(globalObject);
    result->takeItemsOf(getVM(globalObject), this);
    return result;
}

void PyDict::takeItemsOf(VM& vm, PyDict* other)
{
    ASSERT(!m_used && other != this);
    PyDictStorage* storage = other->m_storage.get();
    if (!storage || !other->m_used)
        return;
    reserve(vm, other->m_used);
    for (unsigned i = 0; i < storage->entryCount(); ++i) {
        Entry& entry = storage->entries()[i];
        if (entry.key)
            insertNew(vm, entry.key.get(), storage->hashes()[i], entry.value.get());
    }
}

bool PyDict::next(unsigned& position, JSValue& key, JSValue& value) const
{
    PyDictStorage* storage = m_storage.get();
    if (!storage)
        return false;
    unsigned count = storage->entryCount();
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
    hash = m_storage->hashes()[position - 1];
    return true;
}

bool PyDict::previous(unsigned& position, JSValue& key, JSValue& value) const
{
    PyDictStorage* storage = m_storage.get();
    if (!storage)
        return false;
    position = std::min(position, storage->entryCount());
    while (position) {
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

    uint32_t hash = storage->hashes()[index];
    uint32_t mask = storage->mask();
    uint32_t perturb = hash;
    uint32_t slot = hash & mask;
    while (storage->indexAt(slot) != static_cast<int32_t>(index)) {
        perturb >>= perturbShift;
        slot = (slot * 5 + perturb + 1) & mask;
    }
    removeAt(vm, index, slot);
    // What follows it is all deleted.
    storage->setEntryCount(index);
    return true;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
