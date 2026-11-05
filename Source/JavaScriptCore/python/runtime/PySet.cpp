/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PySet.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTypes.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// set and frozenset. (Objects/setobject.c in CPython is the reference.)

const ClassInfo PySetStorage::s_info = { "PySetStorage"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(PySetStorage) };
PY_DEFINE_CELL(PySet);

static constexpr size_t minimumTableSize = 8;
static constexpr size_t linearProbes = 9;
static constexpr unsigned setPerturbShift = 5;

PySetStorage::PySetStorage(VM& vm, Structure* structure, size_t size)
    : Base(vm, structure)
    , m_mask(size - 1)
{
    Entry* table = entries();
    for (size_t i = 0; i < size; ++i) {
        table[i].key.clear();
        table[i].hash = 0;
    }
}

PySetStorage* PySetStorage::create(VM& vm, Structure* structure, size_t size)
{
    auto* storage = new (NotNull, allocateCell<PySetStorage>(vm, allocationSize(size))) PySetStorage(vm, structure, size);
    storage->finishCreation(vm);
    return storage;
}

template<typename Visitor>
void PySetStorage::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PySetStorage>(cell);
    Base::visitChildren(thisObject, visitor);
    Entry* table = thisObject->entries();
    for (size_t i = 0; i <= thisObject->m_mask; ++i)
        visitor.append(table[i].key);
}

DEFINE_VISIT_CHILDREN(PySetStorage);

PySet::PySet(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PySet* PySet::create(VM& vm, Structure* structure, PyType* type)
{
    auto* set = new (NotNull, allocateCell<PySet>(vm)) PySet(vm, structure, type);
    set->finishCreation(vm);
    return set;
}

PySet* PySet::create(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return create(rt.vm(), rt.structure<PySet>(), rt.setType());
}

PySet* PySet::createFrozen(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return create(rt.vm(), rt.structure<PySet>(), rt.frozensetType());
}

template<typename Visitor>
void PySet::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PySet>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_storage);
}

DEFINE_VISIT_CHILDREN(PySet);

// ---- The table ----

static ALWAYS_INLINE bool isUnused(const PySetStorage::Entry& entry) { return !entry.key && !entry.hash; }
static ALWAYS_INLINE bool isDeleted(const PySetStorage::Entry& entry) { return !entry.key && entry.hash == -1; }

void PySet::insertClean(VM& vm, PySetStorage* storage, JSValue key, int64_t hash)
{
    Entry* table = storage->entries();
    size_t mask = storage->mask();
    size_t perturb = hash;
    size_t i = static_cast<size_t>(hash) & mask;
    Entry* entry;
    while (true) {
        entry = &table[i];
        if (!entry->key)
            break;
        bool found = false;
        if (i + linearProbes <= mask) {
            for (size_t j = 0; j < linearProbes; ++j) {
                ++entry;
                if (!entry->key) {
                    found = true;
                    break;
                }
            }
        }
        if (found)
            break;
        perturb >>= setPerturbShift;
        i = (i * 5 + 1 + perturb) & mask;
    }
    entry->key.set(vm, storage, key);
    entry->hash = hash;
}

void PySet::resize(VM& vm, size_t minimumUsed)
{
    size_t newSize = minimumTableSize;
    while (newSize <= minimumUsed)
        newSize <<= 1;
    PySetStorage* old = m_storage.get();
    if (old && newSize == minimumTableSize && old->mask() + 1 == newSize && m_fill == m_used)
        return;
    PySetStorage* storage = PySetStorage::create(vm, Runtime::from(realm()).structure<PySetStorage>(), newSize);
    if (old) {
        Entry* table = old->entries();
        for (size_t i = 0; i <= old->mask(); ++i) {
            if (table[i].key)
                insertClean(vm, storage, table[i].key.get(), table[i].hash);
        }
    }
    m_fill = m_used;
    vm.mutatorFence();
    m_storage.set(vm, this, storage);
}

static ALWAYS_INLINE bool areSameString(JSValue a, JSValue b)
{
    if (!a.isString() || !b.isString())
        return false;
    const StringImpl* x = asString(a)->tryGetValueImpl();
    const StringImpl* y = asString(b)->tryGetValueImpl();
    return x && y && WTF::equal(x, y);
}

// The hash of an object may have changed since it was stored. What a search does when it comes by the very object it looks for, under another hash
// than it goes by: in a set it goes on; in a frozenset it has found it; in an instance of a class derived from frozenset it has found it if it
// is the search of `in`. (So CPython 3.15, measured. add() goes on in all of them.)
bool PySet::findsUnderAnotherHash(JSGlobalObject* globalObject, SearchFor searchFor) const
{
    Runtime& rt = runtime(globalObject);
    return type() == rt.frozensetType() || (searchFor == SearchFor::In && !type()->isSubtypeOf(rt.setType()));
}

PySet::Entry* PySet::find(JSGlobalObject* globalObject, JSValue key, int64_t hash, SearchFor searchFor)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
restart:
    PySetStorage* storage = m_storage.get();
    if (!storage)
        return nullptr;
    Entry* table = storage->entries();
    size_t mask = storage->mask();
    size_t perturb = hash;
    size_t i = static_cast<size_t>(hash) & mask;
    while (true) {
        Entry* entry = &table[i];
        size_t probes = i + linearProbes <= mask ? linearProbes : 0;
        do {
            if (isUnused(*entry))
                return nullptr;
            if (entry->hash == hash && entry->key) {
                JSValue candidate = entry->key.get();
                if (isIdentical(candidate, key) || areSameString(candidate, key))
                    return entry;
                bool equal = richCompareBool(globalObject, candidate, key, CompareOperator::Eq);
                RETURN_IF_EXCEPTION(scope, nullptr);
                // __eq__ may have changed the set.
                if (m_storage.get() != storage || entry->key.get() != candidate)
                    goto restart;
                if (equal)
                    return entry;
            } else if (entry->key.get() == key && findsUnderAnotherHash(globalObject, searchFor)) [[unlikely]]
                return entry;
            ++entry;
        } while (probes--);
        perturb >>= setPerturbShift;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

bool PySet::contains(JSGlobalObject* globalObject, JSValue key, int64_t hash, SearchFor searchFor)
{
    return !!find(globalObject, key, hash, searchFor);
}

bool PySet::add(JSGlobalObject* globalObject, JSValue key, int64_t hash)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
restart:
    // Also after an __eq__ that cleared the set.
    if (!m_storage)
        resize(vm, 0);
    PySetStorage* storage = m_storage.get();
    Entry* table = storage->entries();
    size_t mask = storage->mask();
    size_t perturb = hash;
    size_t i = static_cast<size_t>(hash) & mask;
    Entry* freeSlot = nullptr;
    Entry* entry;
    while (true) {
        entry = &table[i];
        size_t probes = i + linearProbes <= mask ? linearProbes : 0;
        bool foundUnused = false;
        do {
            if (isUnused(*entry)) {
                foundUnused = true;
                break;
            }
            if (entry->hash == hash && entry->key) {
                JSValue candidate = entry->key.get();
                if (isIdentical(candidate, key) || areSameString(candidate, key))
                    return false;
                bool equal = richCompareBool(globalObject, candidate, key, CompareOperator::Eq);
                RETURN_IF_EXCEPTION(scope, false);
                // (Believed whatever __eq__ has done to the set meanwhile, as in CPython.)
                if (equal)
                    return false;
                if (m_storage.get() != storage || entry->key.get() != candidate)
                    goto restart;
            } else if (isDeleted(*entry))
                freeSlot = entry;
            ++entry;
        } while (probes--);
        if (foundUnused)
            break;
        perturb >>= setPerturbShift;
        i = (i * 5 + 1 + perturb) & mask;
    }

    if (freeSlot) {
        // An __eq__ that was asked since may have put something there.
        if (!isDeleted(*freeSlot))
            goto restart;
        ++m_used;
        freeSlot->key.set(vm, storage, key);
        freeSlot->hash = hash;
        return true;
    }
    ++m_used;
    ++m_fill;
    entry->key.set(vm, storage, key);
    entry->hash = hash;
    if (m_fill * 5 >= mask * 3)
        resize(vm, m_used > 50000 ? m_used * 2 : m_used * 4);
    return true;
}

bool PySet::remove(JSGlobalObject* globalObject, JSValue key, int64_t hash)
{
    Entry* entry = find(globalObject, key, hash);
    if (!entry)
        return false;
    entry->key.clear();
    entry->hash = -1;
    --m_used;
    return true;
}

static ALWAYS_INLINE int64_t hashElement(JSGlobalObject* globalObject, JSValue key)
{
    if (key.isString() && !asString(key)->isRope()) [[likely]]
        return hashOfString(asString(key)->tryGetValueImpl());
    return hashForContainer(globalObject, key, "a set element"_s);
}

bool PySet::contains(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hash = hashElement(globalObject, key);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, contains(globalObject, key, hash));
}

bool PySet::add(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hash = hashElement(globalObject, key);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, add(globalObject, key, hash));
}

bool PySet::remove(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hash = hashElement(globalObject, key);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, remove(globalObject, key, hash));
}

void PySet::clear(VM&)
{
    m_storage.clear();
    m_fill = 0;
    m_used = 0;
    m_hasHash = false;
}

bool PySet::next(size_t& position, JSValue& key, int64_t& hash) const
{
    PySetStorage* storage = m_storage.get();
    if (!storage)
        return false;
    Entry* table = storage->entries();
    size_t mask = storage->mask();
    while (position <= mask && !table[position].key)
        ++position;
    if (position > mask)
        return false;
    key = table[position].key.get();
    hash = table[position].hash;
    ++position;
    return true;
}

bool PySet::pop(VM&, JSValue& key)
{
    if (!m_used)
        return false;
    PySetStorage* storage = m_storage.get();
    Entry* table = storage->entries();
    size_t mask = storage->mask();
    size_t i = m_finger & mask;
    while (!table[i].key)
        i = i == mask ? 0 : i + 1;
    key = table[i].key.get();
    table[i].key.clear();
    table[i].hash = -1;
    --m_used;
    m_finger = i + 1;
    return true;
}

// set.update(other), for another set.
static void mergeSet(JSGlobalObject* globalObject, PySet* set, PySet* other);

PySet* PySet::copy(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    PySet* result = create(rt.vm(), rt.structure<PySet>(), type ? type : this->type());
    mergeSet(globalObject, result, this);
    return result;
}

void PySet::merge(JSGlobalObject* globalObject, PySet* other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (other == this || !other->m_used)
        return;
    if (!m_storage)
        resize(vm, 0);
    // One big resize at the start, rather than several along the way.
    if ((m_fill + other->m_used) * 5 >= m_storage->mask() * 3)
        resize(vm, (m_used + other->m_used) * 2);

    PySetStorage* otherStorage = other->m_storage.get();
    Entry* otherTable = otherStorage->entries();
    if (!m_fill) {
        PySetStorage* storage = m_storage.get();
        if (storage->mask() == otherStorage->mask() && other->m_fill == other->m_used) {
            // The same layout will do.
            Entry* table = storage->entries();
            for (size_t i = 0; i <= otherStorage->mask(); ++i) {
                if (otherTable[i].key) {
                    table[i].key.set(vm, storage, otherTable[i].key.get());
                    table[i].hash = otherTable[i].hash;
                }
            }
        } else {
            for (size_t i = 0; i <= otherStorage->mask(); ++i) {
                if (otherTable[i].key)
                    insertClean(vm, storage, otherTable[i].key.get(), otherTable[i].hash);
            }
        }
        m_fill = other->m_used;
        m_used = other->m_used;
        return;
    }
    JSValue key;
    int64_t hash;
    for (size_t position = 0; other->next(position, key, hash);) {
        add(globalObject, key, hash);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void PySet::swapContents(VM& vm, PySet* other)
{
    PySetStorage* storage = m_storage.get();
    m_storage.setMayBeNull(vm, this, other->m_storage.get());
    other->m_storage.setMayBeNull(vm, other, storage);
    std::swap(m_fill, other->m_fill);
    std::swap(m_used, other->m_used);
    m_hasHash = false;
    other->m_hasHash = false;
}

void PySet::purgeDeletedIfMany(VM& vm)
{
    if (!m_storage || m_fill - m_used <= m_storage->mask() / 4)
        return;
    resize(vm, m_used > 50000 ? m_used * 2 : m_used * 4);
}

void PySet::reserveFor(VM& vm, size_t additional)
{
    if (!m_storage)
        resize(vm, 0);
    if ((m_fill + additional) * 5 >= m_storage->mask() * 3)
        resize(vm, (m_used + additional) * 2);
}

static void mergeSet(JSGlobalObject* globalObject, PySet* set, PySet* other)
{
    set->merge(globalObject, other);
}

// ---- Operations ----

static ALWAYS_INLINE bool isMutableSet(JSGlobalObject* globalObject, PySet* set) { return set->type()->isSubtypeOf(runtime(globalObject).setType()); }

// What an operation on an instance of a subclass makes: a plain set or frozenset.
static PySet* newSetLike(JSGlobalObject* globalObject, PySet* set)
{
    return isMutableSet(globalObject, set) ? PySet::create(globalObject) : PySet::createFrozen(globalObject);
}

static PySet* copyAsBaseType(JSGlobalObject* globalObject, PySet* set)
{
    PySet* result = newSetLike(globalObject, set);
    result->merge(globalObject, set);
    return result;
}

// A dict or a frozendict, not an instance of a class derived from one: its keys come with their hashes.
static PyDict* exactDict(Runtime& rt, JSValue value)
{
    auto* dict = PyDict::tryFrom(value);
    return dict && (dict->type() == rt.dictType() || dict->type() == rt.frozendictType()) ? dict : nullptr;
}

static void updateSet(JSGlobalObject* globalObject, PySet* set, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* otherSet = PySet::tryFrom(other))
        RELEASE_AND_RETURN(scope, set->merge(globalObject, otherSet));
    if (PyDict* dict = exactDict(rt, other)) {
        set->reserveFor(vm, dict->size());
        JSValue key, value;
        int64_t hash;
        for (unsigned position = 0; dict->next(position, key, value, hash);) {
            // (The dict does not keep that of a str.)
            if (key.isString())
                hash = hashOfString(asString(key)->tryGetValueImpl());
            set->add(globalObject, key, hash);
            RETURN_IF_EXCEPTION(scope, void());
        }
        return;
    }
    scope.release();
    forEach(globalObject, other, [&](JSValue item) {
        set->add(globalObject, item);
        return true;
    });
}

void setUpdate(JSGlobalObject* globalObject, JSValue set, JSValue iterable)
{
    updateSet(globalObject, PySet::from(set), iterable);
}

static uint64_t shuffleBits(uint64_t h) { return ((h ^ 89869747UL) ^ (h << 16)) * 3644798167UL; }

static int64_t hashOfSet(PySet* set)
{
    // Independent of the order of the elements.
    uint64_t hash = 0;
    JSValue key;
    int64_t elementHash;
    for (size_t position = 0; set->next(position, key, elementHash);)
        hash ^= shuffleBits(static_cast<uint64_t>(elementHash));
    hash ^= (static_cast<uint64_t>(set->size()) + 1) * 1927868237UL;
    hash ^= (hash >> 11) ^ (hash >> 25);
    hash = hash * 69069U + 907133923UL;
    if (hash == static_cast<uint64_t>(-1))
        hash = 590923713UL;
    return static_cast<int64_t>(hash);
}

// The hash of a key to look for. A set, which cannot be hashed, is looked for as the frozenset that equals it.
static bool hashForLookup(JSGlobalObject* globalObject, JSValue key, int64_t& hash)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    hash = hashElement(globalObject, key);
    if (!scope.exception()) [[likely]]
        return true;
    auto* set = PySet::tryFrom(key);
    if (!set || !isMutableSet(globalObject, set) || !clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
        return false;
    hash = hashOfSet(set);
    return true;
}

static PySet* intersection(JSGlobalObject* globalObject, PySet* set, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (other == JSValue(set))
        RELEASE_AND_RETURN(scope, copyAsBaseType(globalObject, set));
    PySet* result = newSetLike(globalObject, set);
    if (auto* otherSet = PySet::tryFrom(other)) {
        // Go through the smaller of the two.
        if (otherSet->size() > set->size())
            std::swap(set, otherSet);
        JSValue key;
        int64_t hash;
        for (size_t position = 0; otherSet->next(position, key, hash);) {
            bool found = set->contains(globalObject, key, hash);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (found) {
                result->add(globalObject, key, hash);
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
        }
        return result;
    }
    forEach(globalObject, other, [&](JSValue key) {
        // Not hashElement(): nothing says here that what cannot be hashed cannot be an element.
        int64_t hash = Python::hash(globalObject, key);
        RETURN_IF_EXCEPTION(scope, false);
        bool found = set->contains(globalObject, key, hash);
        RETURN_IF_EXCEPTION(scope, false);
        if (!found)
            return true;
        result->add(globalObject, key, hash);
        RETURN_IF_EXCEPTION(scope, false);
        // With all of the set found, the rest is not looked at (nor hashed, which shows).
        return result->size() < set->size();
    });
    RETURN_IF_EXCEPTION(scope, nullptr);
    return result;
}

void setDifferenceUpdate(JSGlobalObject* globalObject, PySet* set, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (other == JSValue(set))
        return set->clear(vm);
    if (auto* otherSet = PySet::tryFrom(other)) {
        // Of a much larger set, only what the two have in common matters.
        if ((otherSet->size() >> 3) > set->size()) {
            otherSet = intersection(globalObject, set, otherSet);
            RETURN_IF_EXCEPTION(scope, void());
        }
        JSValue key;
        int64_t hash;
        for (size_t position = 0; otherSet->next(position, key, hash);) {
            set->remove(globalObject, key, hash);
            RETURN_IF_EXCEPTION(scope, void());
        }
    } else {
        forEach(globalObject, other, [&](JSValue key) {
            set->remove(globalObject, key);
            RETURN_IF_EXCEPTION(scope, false);
            return true;
        });
        RETURN_IF_EXCEPTION(scope, void());
    }
    set->purgeDeletedIfMany(vm);
}

static PySet* difference(JSGlobalObject* globalObject, PySet* set, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* otherSet = PySet::tryFrom(other);
    PyDict* otherDict = exactDict(rt, other);
    size_t otherSize = otherSet ? otherSet->size() : otherDict ? otherDict->size() : 0;
    // Against something much smaller, or of unknown size: copy, then take away.
    if ((!otherSet && !otherDict) || (set->size() >> 2) > otherSize) {
        PySet* result = copyAsBaseType(globalObject, set);
        RETURN_IF_EXCEPTION(scope, nullptr);
        setDifferenceUpdate(globalObject, result, other);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return result;
    }
    PySet* result = newSetLike(globalObject, set);
    JSValue key;
    int64_t hash;
    for (size_t position = 0; set->next(position, key, hash);) {
        bool found = otherSet ? otherSet->contains(globalObject, key, hash) : !!otherDict->get(globalObject, key, hash);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!found) {
            result->add(globalObject, key, hash);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }
    return result;
}

void setSymmetricDifferenceUpdate(JSGlobalObject* globalObject, PySet* set, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (other == JSValue(set))
        return set->clear(vm);
    if (PyDict* dict = exactDict(runtime(globalObject), other)) {
        JSValue key, value;
        int64_t hash;
        for (unsigned position = 0; dict->next(position, key, value, hash);) {
            // (The dict does not keep that of a str.)
            if (key.isString())
                hash = hashOfString(asString(key)->tryGetValueImpl());
            bool removed = set->remove(globalObject, key, hash);
            RETURN_IF_EXCEPTION(scope, void());
            if (!removed) {
                set->add(globalObject, key, hash);
                RETURN_IF_EXCEPTION(scope, void());
            }
        }
        return;
    }
    auto* otherSet = PySet::tryFrom(other);
    if (!otherSet) {
        otherSet = newSetLike(globalObject, set);
        updateSet(globalObject, otherSet, other);
        RETURN_IF_EXCEPTION(scope, void());
    }
    JSValue key;
    int64_t hash;
    for (size_t position = 0; otherSet->next(position, key, hash);) {
        bool removed = set->remove(globalObject, key, hash);
        RETURN_IF_EXCEPTION(scope, void());
        if (!removed) {
            set->add(globalObject, key, hash);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
}

static bool isSubset(JSGlobalObject* globalObject, PySet* set, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* otherSet = PySet::tryFrom(other);
    if (!otherSet) {
        PySet* common = intersection(globalObject, set, other);
        RETURN_IF_EXCEPTION(scope, false);
        return common->size() == set->size();
    }
    if (set->size() > otherSet->size())
        return false;
    JSValue key;
    int64_t hash;
    for (size_t position = 0; set->next(position, key, hash);) {
        bool found = otherSet->contains(globalObject, key, hash);
        RETURN_IF_EXCEPTION(scope, false);
        if (!found)
            return false;
    }
    return true;
}

static bool isSuperset(JSGlobalObject* globalObject, PySet* set, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* otherSet = PySet::tryFrom(other))
        RELEASE_AND_RETURN(scope, isSubset(globalObject, otherSet, set));
    bool result = true;
    forEach(globalObject, other, [&](JSValue key) {
        result = set->contains(globalObject, key);
        return result;
    });
    RETURN_IF_EXCEPTION(scope, false);
    return result;
}

// ---- Methods ----

#define PY_SET_PROLOGUE() \
    VM& vm = getVM(globalObject); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    PySet* set = PySet::from(self); \
    UNUSED_VARIABLE(set)

static JSValue setNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    return PySet::create(rt.vm(), rt.structure<PySet>(), PyType::from(cls));
}

static JSValue setInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    PY_SET_PROLOGUE();
    if (failed(scope, rejectKeywords(globalObject, "set"_s, args)))
        return { };
    if (args.size() > 1)
        return raiseTypeError(globalObject, scope, typeName(globalObject, self), " expected at most 1 argument, got "_s, args.size());
    set->clear(vm);
    if (args.size()) {
        updateSet(globalObject, set, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue frozensetNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    if (failed(scope, rejectKeywordsInNew(globalObject, "frozenset"_s, type, rt.frozensetType(), args)))
        return { };
    if (args.size() > 1)
        return raiseTypeError(globalObject, scope, type->nameInMessages(), " expected at most 1 argument, got "_s, args.size());
    if (args.size() && type == rt.frozensetType()) {
        if (auto* existing = PySet::tryFrom(args[0]); existing && existing->type() == type)
            return existing;
    }
    PySet* result = PySet::create(vm, rt.structure<PySet>(), type);
    if (args.size()) {
        updateSet(globalObject, result, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue setRepr(JSGlobalObject* globalObject, JSValue self)
{
    PY_SET_PROLOGUE();
    Runtime& rt = runtime(globalObject);
    String name = set->type()->name();
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return makeStr(vm, makeString(name, "(...)"_s));
    if (set->isEmpty())
        return makeStr(vm, makeString(name, "()"_s));
    StrBuilder builder;
    bool isPlainSet = set->type() == rt.setType();
    if (!isPlainSet)
        builder.append(name, '(');
    builder.append('{');
    // What the reprs of the elements do must not upset the walk. (What is in the set, not what the __iter__ of a derived class gives.)
    JSArray* elements = newList(globalObject, set->size());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue element;
    for (size_t position = 0; set->next(position, element);) {
        listAppend(globalObject, elements, element);
        RETURN_IF_EXCEPTION(scope, { });
    }
    for (unsigned i = 0; i < listLength(elements); ++i) {
        if (i)
            builder.append(", "_s);
        JSString* item = repr(globalObject, listGet(elements, i));
        RETURN_IF_EXCEPTION(scope, { });
        auto view = item->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(view.data);
    }
    builder.append('}');
    if (!isPlainSet)
        builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue setLen(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, static_cast<uint64_t>(PySet::from(self)->size())); }

static JSValue setIter(JSGlobalObject* globalObject, JSValue self)
{
    PyIterator* iterator = PyIterator::create(globalObject, PyIterator::Kind::Set, self);
    iterator->remaining = PySet::from(self)->size();
    return iterator;
}

bool setContains(JSGlobalObject* globalObject, PySet* set, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hash;
    if (key.isString() && !asString(key)->isRope()) [[likely]]
        hash = hashOfString(asString(key)->tryGetValueImpl());
    else if (failed(scope, hashForLookup(globalObject, key, hash)))
        return false;
    RELEASE_AND_RETURN(scope, set->contains(globalObject, key, hash, PySet::SearchFor::In));
}

static JSValue setContainsMethod(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    PY_SET_PROLOGUE();
    bool found = setContains(globalObject, set, key);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

static int64_t frozensetHash(JSGlobalObject*, JSValue self)
{
    PySet* set = PySet::from(self);
    if (!set->hasCachedHash())
        set->setCachedHash(hashOfSet(set));
    return set->cachedHash();
}

template<CompareOperator op>
static JSValue setCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    PY_SET_PROLOGUE();
    auto* otherSet = PySet::tryFrom(other);
    if (!otherSet)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    bool result;
    switch (op) {
    case CompareOperator::Eq:
    case CompareOperator::NotEq:
        if (set->size() != otherSet->size() || (set->hasCachedHash() && otherSet->hasCachedHash() && set->cachedHash() != otherSet->cachedHash()))
            result = false;
        else {
            result = isSubset(globalObject, set, otherSet);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return jsBoolean(result == (op == CompareOperator::Eq));
    case CompareOperator::LtE:
        result = isSubset(globalObject, set, otherSet);
        break;
    case CompareOperator::GtE:
        result = isSubset(globalObject, otherSet, set);
        break;
    case CompareOperator::Lt:
        result = set->size() < otherSet->size() && isSubset(globalObject, set, otherSet);
        break;
    default:
        result = set->size() > otherSet->size() && isSubset(globalObject, otherSet, set);
        break;
    }
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue setAdd(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    PY_SET_PROLOGUE();
    set->add(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue setRemove(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    PY_SET_PROLOGUE();
    int64_t hash;
    if (failed(scope, hashForLookup(globalObject, key, hash)))
        return { };
    bool removed = set->remove(globalObject, key, hash);
    RETURN_IF_EXCEPTION(scope, { });
    if (!removed)
        return raiseKeyError(globalObject, scope, key);
    return jsUndefined();
}

static JSValue setDiscard(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    PY_SET_PROLOGUE();
    int64_t hash;
    if (failed(scope, hashForLookup(globalObject, key, hash)))
        return { };
    set->remove(globalObject, key, hash);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue setPop(JSGlobalObject* globalObject, JSValue self)
{
    PY_SET_PROLOGUE();
    JSValue key;
    if (!set->pop(vm, key))
        return raiseKeyError(globalObject, scope, makeStr(vm, "pop from an empty set"_s));
    return key;
}

static JSValue setClear(JSGlobalObject* globalObject, JSValue self)
{
    PySet::from(self)->clear(getVM(globalObject));
    return jsUndefined();
}

static JSValue setCopy(JSGlobalObject* globalObject, JSValue self) { return copyAsBaseType(globalObject, PySet::from(self)); }

static JSValue frozensetCopy(JSGlobalObject* globalObject, JSValue self)
{
    PySet* set = PySet::from(self);
    if (set->type() == runtime(globalObject).frozensetType())
        return set;
    return copyAsBaseType(globalObject, set);
}

static JSValue setUpdateMethod(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    PY_SET_PROLOGUE();
    if (failed(scope, rejectKeywords(globalObject, "update"_s, args)))
        return { };
    for (JSValue other : args.positional()) {
        updateSet(globalObject, set, other);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue setUnion(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    PY_SET_PROLOGUE();
    if (failed(scope, rejectKeywords(globalObject, "union"_s, args)))
        return { };
    PySet* result = copyAsBaseType(globalObject, set);
    RETURN_IF_EXCEPTION(scope, { });
    for (JSValue other : args.positional()) {
        if (other == self)
            continue;
        updateSet(globalObject, result, other);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static PySet* intersectionOfAll(JSGlobalObject* globalObject, PySet* set, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!args.size())
        RELEASE_AND_RETURN(scope, copyAsBaseType(globalObject, set));
    PySet* result = set;
    for (JSValue other : args.positional()) {
        result = intersection(globalObject, result, other);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return result;
}

static JSValue setIntersection(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "intersection"_s, args)))
        return { };
    RELEASE_AND_RETURN(scope, intersectionOfAll(globalObject, PySet::from(self), args));
}

static JSValue setIntersectionUpdate(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    PY_SET_PROLOGUE();
    if (failed(scope, rejectKeywords(globalObject, "intersection_update"_s, args)))
        return { };
    PySet* result = intersectionOfAll(globalObject, set, args);
    RETURN_IF_EXCEPTION(scope, { });
    set->swapContents(vm, result);
    return jsUndefined();
}

static JSValue setDifference(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    PY_SET_PROLOGUE();
    if (failed(scope, rejectKeywords(globalObject, "difference"_s, args)))
        return { };
    if (!args.size())
        RELEASE_AND_RETURN(scope, copyAsBaseType(globalObject, set));
    PySet* result = difference(globalObject, set, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 1; i < args.size(); ++i) {
        setDifferenceUpdate(globalObject, result, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue setDifferenceUpdateMethod(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    PY_SET_PROLOGUE();
    if (failed(scope, rejectKeywords(globalObject, "difference_update"_s, args)))
        return { };
    for (JSValue other : args.positional()) {
        setDifferenceUpdate(globalObject, set, other);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue setSymmetricDifference(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    PY_SET_PROLOGUE();
    PySet* result = newSetLike(globalObject, set);
    updateSet(globalObject, result, other);
    RETURN_IF_EXCEPTION(scope, { });
    setSymmetricDifferenceUpdate(globalObject, result, set);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue setSymmetricDifferenceUpdateMethod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    PY_SET_PROLOGUE();
    setSymmetricDifferenceUpdate(globalObject, set, other);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue setIsDisjoint(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    PY_SET_PROLOGUE();
    if (other == self)
        return jsBoolean(set->isEmpty());
    bool disjoint = true;
    // (An instance of a class derived from set or frozenset is gone through as any iterable is, and its elements are hashed again: so CPython.)
    Runtime& rt = runtime(globalObject);
    if (auto* otherSet = PySet::tryFrom(other); otherSet && (otherSet->type() == rt.setType() || otherSet->type() == rt.frozensetType())) {
        if (otherSet->size() > set->size())
            std::swap(set, otherSet);
        JSValue key;
        int64_t hash;
        for (size_t position = 0; otherSet->next(position, key, hash);) {
            bool found = set->contains(globalObject, key, hash);
            RETURN_IF_EXCEPTION(scope, { });
            if (found)
                return jsBoolean(false);
        }
        return jsBoolean(true);
    }
    forEach(globalObject, other, [&](JSValue key) {
        disjoint = !set->contains(globalObject, key);
        return disjoint;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(disjoint);
}

static JSValue setIsSubset(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    PY_SET_PROLOGUE();
    bool result = isSubset(globalObject, set, other);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue setIsSuperset(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    PY_SET_PROLOGUE();
    bool result = isSuperset(globalObject, set, other);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

// The operators take sets only.

enum class SetOperator : uint8_t { Or, And, Sub, Xor };

static JSValue setOperation(JSGlobalObject* globalObject, SetOperator op, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* a = PySet::tryFrom(left);
    auto* b = PySet::tryFrom(right);
    if (!a || !b)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    switch (op) {
    case SetOperator::Or: {
        PySet* result = copyAsBaseType(globalObject, a);
        RETURN_IF_EXCEPTION(scope, { });
        if (a != b) {
            result->merge(globalObject, b);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return result;
    }
    case SetOperator::And:
        RELEASE_AND_RETURN(scope, intersection(globalObject, a, b));
    case SetOperator::Sub:
        RELEASE_AND_RETURN(scope, difference(globalObject, a, b));
    case SetOperator::Xor:
        RELEASE_AND_RETURN(scope, setSymmetricDifference(globalObject, a, b));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<SetOperator op>
static JSValue setBinary(JSGlobalObject* globalObject, JSValue self, JSValue other) { return setOperation(globalObject, op, self, other); }
template<SetOperator op>
static JSValue setReflected(JSGlobalObject* globalObject, JSValue self, JSValue other) { return setOperation(globalObject, op, other, self); }

template<SetOperator op>
static JSValue setInPlace(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    PY_SET_PROLOGUE();
    auto* otherSet = PySet::tryFrom(other);
    if (!otherSet)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    switch (op) {
    case SetOperator::Or:
        set->merge(globalObject, otherSet);
        break;
    case SetOperator::And: {
        PySet* result = intersection(globalObject, set, otherSet);
        RETURN_IF_EXCEPTION(scope, { });
        set->swapContents(vm, result);
        break;
    }
    case SetOperator::Sub:
        setDifferenceUpdate(globalObject, set, otherSet);
        break;
    case SetOperator::Xor:
        setSymmetricDifferenceUpdate(globalObject, set, otherSet);
        break;
    }
    RETURN_IF_EXCEPTION(scope, { });
    return self;
}

static JSValue setReduce(JSGlobalObject* globalObject, JSValue self)
{
    PY_SET_PROLOGUE();
    JSArray* keys = listFromIterable(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue state = getObjectState(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { set->type(), PyTuple::create(globalObject, { JSValue(keys) }), state }));
}

static JSValue setSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    return makeInt(globalObject, static_cast<uint64_t>(200 + 16 * PySet::from(self)->size()));
}

#define PY_SET_SHARED_METHODS \
    PY_SLOT("__repr__", setRepr, "($self, /)"), \
    PY_SLOT("__len__", setLen, "($self, /)"), \
    PY_SLOT("__iter__", setIter, "($self, /)"), \
    PY_METHOD("__contains__", setContainsMethod, "($self, object, /)"), \
    PY_SLOT("__eq__", setCompare<CompareOperator::Eq>, "($self, value, /)"), \
    PY_SLOT("__ne__", setCompare<CompareOperator::NotEq>, "($self, value, /)"), \
    PY_SLOT("__lt__", setCompare<CompareOperator::Lt>, "($self, value, /)"), \
    PY_SLOT("__le__", setCompare<CompareOperator::LtE>, "($self, value, /)"), \
    PY_SLOT("__gt__", setCompare<CompareOperator::Gt>, "($self, value, /)"), \
    PY_SLOT("__ge__", setCompare<CompareOperator::GtE>, "($self, value, /)"), \
    PY_SLOT("__or__", setBinary<SetOperator::Or>, "($self, value, /)"), \
    PY_SLOT("__ror__", setReflected<SetOperator::Or>, "($self, value, /)"), \
    PY_SLOT("__and__", setBinary<SetOperator::And>, "($self, value, /)"), \
    PY_SLOT("__rand__", setReflected<SetOperator::And>, "($self, value, /)"), \
    PY_SLOT("__sub__", setBinary<SetOperator::Sub>, "($self, value, /)"), \
    PY_SLOT("__rsub__", setReflected<SetOperator::Sub>, "($self, value, /)"), \
    PY_SLOT("__xor__", setBinary<SetOperator::Xor>, "($self, value, /)"), \
    PY_SLOT("__rxor__", setReflected<SetOperator::Xor>, "($self, value, /)"), \
    PY_METHOD("difference", setDifference, "($self, /, *others)"), \
    PY_METHOD("intersection", setIntersection, "($self, /, *others)"), \
    PY_METHOD("isdisjoint", setIsDisjoint, "($self, other, /)"), \
    PY_METHOD("issubset", setIsSubset, "($self, other, /)"), \
    PY_METHOD("issuperset", setIsSuperset, "($self, other, /)"), \
    PY_METHOD("symmetric_difference", setSymmetricDifference, "($self, other, /)"), \
    PY_METHOD("union", setUnion, "($self, /, *others)"), \
    PY_METHOD("__reduce__", setReduce, "($self, /)"), \
    PY_METHOD("__sizeof__", setSizeOf, "($self, /)"), \
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)")

static constexpr NativeDef setMethods[] = {
    PY_NEW(setNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", setInit, "($self, *args, **kwargs)"),
    PY_SET_SHARED_METHODS,
    PY_SLOT("__ior__", setInPlace<SetOperator::Or>, "($self, value, /)"),
    PY_SLOT("__iand__", setInPlace<SetOperator::And>, "($self, value, /)"),
    PY_SLOT("__isub__", setInPlace<SetOperator::Sub>, "($self, value, /)"),
    PY_SLOT("__ixor__", setInPlace<SetOperator::Xor>, "($self, value, /)"),
    PY_METHOD("add", setAdd, "($self, object, /)"),
    PY_METHOD("clear", setClear, "($self, /)"),
    PY_METHOD("copy", setCopy, "($self, /)"),
    PY_METHOD("discard", setDiscard, "($self, object, /)"),
    PY_METHOD("difference_update", setDifferenceUpdateMethod, "($self, /, *others)"),
    PY_METHOD("intersection_update", setIntersectionUpdate, "($self, /, *others)"),
    PY_METHOD("pop", setPop, "($self, /)"),
    PY_METHOD("remove", setRemove, "($self, object, /)"),
    PY_METHOD("symmetric_difference_update", setSymmetricDifferenceUpdateMethod, "($self, other, /)"),
    PY_METHOD("update", setUpdateMethod, "($self, /, *others)"),
};

static constexpr NativeDef frozensetMethods[] = {
    PY_NEW(frozensetNew, "($type, *args, **kwargs)"),
    PY_SET_SHARED_METHODS,
    PY_HASH(frozensetHash),
    PY_METHOD("copy", frozensetCopy, "($self, /)"),
};

void initializeSetTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    rt.setType()->initializeBuiltin(globalObject, { "set"_s, { TypeFlag::Base, TypeFlag::MatchSelf, TypeFlag::OwnLayout, TypeFlag::HasWeakref, TypeFlag::GoesThroughArgument }, setMethods, { }, "Build an unordered collection of unique elements."_s }, rt.typeType(), rt.objectType());
    rt.setType()->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
    rt.setType()->didChange(vm);
    rt.frozensetType()->initializeBuiltin(globalObject, { "frozenset"_s, { TypeFlag::Base, TypeFlag::MatchSelf, TypeFlag::OwnLayout, TypeFlag::HasWeakref, TypeFlag::GoesThroughArgument }, frozensetMethods, { }, "Build an immutable unordered collection of unique elements."_s }, rt.typeType(), rt.objectType());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
