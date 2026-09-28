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

#include "config.h"
#include "PyDict.h"

#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PyTuple.h"
#include "PythonOperations.h"

namespace JSC {

// ---- PyHashStorage

const ClassInfo PyHashStorage::s_info = { "PyHashStorage"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(PyHashStorage) };

template<typename Visitor>
void PyHashStorage::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyHashStorage>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.appendValues(thisObject->slots(), static_cast<size_t>(thisObject->m_capacity) * thisObject->m_stride);
}

DEFINE_VISIT_CHILDREN(PyHashStorage);

PyHashStorage* PyHashStorage::create(VM& vm, Structure* structure, unsigned capacity, unsigned indexSize, unsigned stride)
{
    ASSERT(hasOneBitSet(indexSize));
    auto* storage = new (NotNull, allocateCell<PyHashStorage>(vm, allocationSize(capacity, indexSize, stride))) PyHashStorage(vm, structure, capacity, indexSize, stride);
    for (size_t i = 0; i < static_cast<size_t>(capacity) * stride; ++i)
        storage->slots()[i].clear();
    for (unsigned i = 0; i < indexSize; ++i)
        storage->index(i) = emptyIndex;
    storage->finishCreation(vm);
    return storage;
}

Structure* PyHashStorage::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CellType, StructureFlags), info());
}

// ---- PyHashTable

template<typename Visitor>
void PyHashTable::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyHashTable>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_storage);
}

DEFINE_VISIT_CHILDREN(PyHashTable);

static ALWAYS_INLINE uint32_t foldHash(int64_t hash)
{
    return static_cast<uint32_t>(hash) ^ static_cast<uint32_t>(static_cast<uint64_t>(hash) >> 32);
}

// The hash of a key, with an error that says what it was wanted for.
int64_t PyHashTable::hashOfKey(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hash = Python::hash(globalObject, key);
    Exception* exception = scope.exception();
    if (!exception) [[likely]]
        return hash;
    JSValue error = exception->value();
    if (Python::typeOf(globalObject, error) != globalObject->pyRealm()->typeTypeError() || !scope.tryClearException())
        return 0;
    String reason = Python::str(globalObject, error);
    RETURN_IF_EXCEPTION(scope, 0);
    Python::raiseTypeError(globalObject, scope, makeString("cannot use '"_s, Python::typeName(globalObject, key), type() == PyDictType ? "' as a dict key ("_s : "' as a set element ("_s, reason, ')'));
    return 0;
}

int PyHashTable::find(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Even of an empty table, since asking it about a list is an error.
    int64_t hash = hashOfKey(globalObject, key);
    RETURN_IF_EXCEPTION(scope, raised);
    RELEASE_AND_RETURN(scope, find(globalObject, key, foldHash(hash)));
}

int PyHashTable::find(JSGlobalObject* globalObject, JSValue key, uint32_t hash)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    while (true) {
        PyHashStorage* storage = m_storage.get();
        if (!storage)
            return notFound;
        unsigned mask = storage->indexMask();
        unsigned slot = hash & mask;
        uint32_t perturb = hash;
        bool restart = false;
        while (true) {
            uint32_t entry = storage->index(slot);
            if (entry == PyHashStorage::emptyIndex)
                return notFound;
            if (entry != PyHashStorage::deletedIndex && storage->hash(entry) == hash) {
                JSValue candidate = storage->key(entry).get();
                if (Python::isIdentical(candidate, key))
                    return entry;
                bool isEqual = Python::isEqual(globalObject, candidate, key);
                RETURN_IF_EXCEPTION(scope, raised);
                // Which can run anything, and that may have changed this table.
                if (m_storage.get() != storage || storage->key(entry).get() != candidate) {
                    restart = true;
                    break;
                }
                if (isEqual)
                    return entry;
            }
            perturb >>= 5;
            slot = (slot * 5 + perturb + 1) & mask;
        }
        if (!restart)
            return notFound;
    }
}

void PyHashTable::insertIndex(PyHashStorage& storage, uint32_t hash, unsigned entry)
{
    unsigned mask = storage.indexMask();
    unsigned slot = hash & mask;
    uint32_t perturb = hash;
    while (storage.index(slot) < PyHashStorage::deletedIndex) {
        perturb >>= 5;
        slot = (slot * 5 + perturb + 1) & mask;
    }
    storage.index(slot) = entry;
}

// Makes room for another entry. What has been removed is dropped, so the entries move.
void PyHashTable::grow(VM& vm, JSGlobalObject* globalObject)
{
    unsigned indexSize = 8;
    while (static_cast<uint64_t>(indexSize) * 2 / 3 <= static_cast<uint64_t>(m_size) * 2)
        indexSize <<= 1;
    PyHashStorage* old = m_storage.get();
    PyHashStorage* storage = PyHashStorage::create(vm, globalObject->pyRealm()->hashStorageStructure(), indexSize * 2 / 3, indexSize, m_stride);
    unsigned used = 0;
    for (unsigned entry = 0; entry < m_used; ++entry) {
        JSValue key = old->key(entry).get();
        if (!key)
            continue;
        storage->key(used).set(vm, storage, key);
        if (m_stride == 2)
            storage->value(used).set(vm, storage, old->value(entry).get());
        storage->hash(used) = old->hash(entry);
        insertIndex(*storage, old->hash(entry), used);
        ++used;
    }
    m_storage.set(vm, this, storage);
    m_used = used;
}

bool PyHashTable::add(JSGlobalObject* globalObject, JSValue key, JSValue value, bool* wasAdded, bool replace)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    int64_t fullHash = hashOfKey(globalObject, key);
    RETURN_IF_EXCEPTION(scope, false);
    uint32_t hash = foldHash(fullHash);
    int entry = find(globalObject, key, hash);
    RETURN_IF_EXCEPTION(scope, false);
    if (wasAdded)
        *wasAdded = entry == notFound;
    if (entry != notFound) {
        if (replace && m_stride == 2)
            setValueAt(vm, entry, value);
        return true;
    }

    if (!m_storage || m_used == m_storage->capacity())
        grow(vm, globalObject);
    PyHashStorage* storage = m_storage.get();
    storage->key(m_used).set(vm, storage, key);
    if (m_stride == 2)
        storage->value(m_used).set(vm, storage, value);
    storage->hash(m_used) = hash;
    insertIndex(*storage, hash, m_used);
    ++m_used;
    ++m_size;
    ++m_version;
    return true;
}

void PyHashTable::removeEntry(VM&, unsigned entry)
{
    PyHashStorage* storage = m_storage.get();
    uint32_t hash = storage->hash(entry);
    unsigned mask = storage->indexMask();
    unsigned slot = hash & mask;
    uint32_t perturb = hash;
    while (storage->index(slot) != entry) {
        perturb >>= 5;
        slot = (slot * 5 + perturb + 1) & mask;
    }
    storage->index(slot) = PyHashStorage::deletedIndex;
    storage->key(entry).clear();
    if (m_stride == 2)
        storage->value(entry).clear();
    --m_size;
    ++m_version;
}

JSValue PyHashTable::remove(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int entry = find(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (entry == notFound)
        return { };
    JSValue result = m_stride == 2 ? valueAt(entry) : keyAt(entry);
    removeEntry(vm, entry);
    return result;
}

void PyHashTable::clear(VM&)
{
    m_storage.clear();
    m_used = 0;
    m_size = 0;
    ++m_version;
}

void PyHashTable::copyFrom(VM& vm, JSGlobalObject* globalObject, PyHashTable& other)
{
    ASSERT(!m_size && m_stride == other.m_stride);
    if (!other.m_size)
        return;
    // As growing does, from the other's storage.
    m_storage.set(vm, this, other.m_storage.get());
    m_used = other.m_used;
    m_size = other.m_size;
    grow(vm, globalObject);
    ++m_version;
}

JSValue PyHashTable::getString(JSGlobalObject* globalObject, const String& key)
{
    PyHashStorage* storage = m_storage.get();
    if (!storage)
        return { };
    uint32_t hash = foldHash(Python::hashOfString(key));
    unsigned mask = storage->indexMask();
    unsigned slot = hash & mask;
    uint32_t perturb = hash;
    while (true) {
        uint32_t entry = storage->index(slot);
        if (entry == PyHashStorage::emptyIndex)
            return { };
        if (entry != PyHashStorage::deletedIndex && storage->hash(entry) == hash) {
            JSValue candidate = storage->key(entry).get();
            if (candidate.isString() && asString(candidate)->value(globalObject).data == key)
                return m_stride == 2 ? storage->value(entry).get() : candidate;
        }
        perturb >>= 5;
        slot = (slot * 5 + perturb + 1) & mask;
    }
}

// ---- PyDict

const ClassInfo PyDict::s_info = { "dict"_s, &JSNonFinalObject::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyDict) };

PyDict* PyDict::create(VM& vm, Structure* structure)
{
    auto* dict = new (NotNull, allocateCell<PyDict>(vm)) PyDict(vm, structure);
    dict->finishCreation(vm);
    return dict;
}

PyDict* PyDict::create(JSGlobalObject* globalObject)
{
    return create(globalObject->vm(), globalObject->pyRealm()->structureFor(BuiltinType::Dict));
}

Structure* PyDict::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(PyDictType, StructureFlags), info());
}

template<typename Visitor>
void PyDict::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyDict>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_backing);
}

DEFINE_VISIT_CHILDREN(PyDict);

PyDict* PyDict::backedBy(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = globalObject->vm();
    auto& name = vm.pythonNames().private_dict;
    if (JSValue existing = object->getDirect(vm, name))
        return asDict(existing);
    PyDict* dict = create(globalObject);
    dict->m_backing.set(vm, dict, object);
    object->putDirect(vm, name, dict);
    return dict;
}

void PyDict::becomeBackedBy(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = globalObject->vm();
    ASSERT(!m_backing);
    // Comparing strings with strings runs nothing, so the table stays as it is meanwhile.
    for (unsigned entry = 0; entry < Base::entryCount(); ++entry) {
        JSValue key = Base::keyAt(entry);
        if (!key || !key.isString())
            continue;
        auto name = asString(key)->toIdentifier(globalObject);
        if (Python::isIndexLike(name))
            continue;
        object->putDirect(vm, name, Base::valueAt(entry));
        Base::removeEntry(vm, entry);
    }
    m_backing.set(vm, this, object);
    object->putDirect(vm, vm.pythonNames().private_dict, this);
}

void PyDict::detach(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* object = m_backing.get();
    ASSERT(object);
    PyDict* items = create(globalObject);
    items->copyFrom(globalObject, *this);
    clear(globalObject);
    JSCell::deleteProperty(object, globalObject, vm.pythonNames().private_dict);
    m_backing.clear();
    copyFrom(globalObject, *items);
}

unsigned PyDict::backingSize() const
{
    unsigned size = 0;
    m_backing->structure()->forEachProperty(m_backing->vm(), [&] (const PropertyTableEntry& entry) {
        if (!(entry.attributes() & PropertyAttribute::DontEnum) && !entry.key()->isSymbol())
            ++size;
        return true;
    });
    return size;
}

JSObject* PyDict::ensureBacking(JSGlobalObject* globalObject)
{
    if (!m_backing)
        becomeBackedBy(globalObject, constructEmptyObject(globalObject->vm(), globalObject->nullPrototypeObjectStructure()));
    return m_backing.get();
}

PyTuple* PyDict::backingKeys(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    m_backing->structure()->getPropertyNamesFromStructure(vm, names, DontEnumPropertiesMode::Exclude);
    PyTuple* keys = PyTuple::create(globalObject, names.size());
    for (unsigned i = 0; i < names.size(); ++i)
        keys->initializeAt(vm, i, jsString(vm, names[i].string()));
    return keys;
}

bool PyDict::isInBacking(JSGlobalObject* globalObject, JSValue key, Identifier& name)
{
    if (!m_backing) [[likely]]
        return false;
    // An instance of a class derived from str is the same key as the string in it, unless the class says otherwise.
    if (!key.isString())
        return false;
    name = asString(key)->toIdentifier(globalObject);
    // See Python::getStoredAttribute().
    return !Python::isIndexLike(name);
}

JSValue PyDict::get(JSGlobalObject* globalObject, JSValue key)
{
    Identifier name;
    if (isInBacking(globalObject, key, name)) [[unlikely]]
        return Python::getStoredAttribute(globalObject->vm(), m_backing.get(), name);
    int entry = Base::find(globalObject, key);
    return entry < 0 ? JSValue() : Base::valueAt(entry);
}

bool PyDict::contains(JSGlobalObject* globalObject, JSValue key)
{
    Identifier name;
    if (isInBacking(globalObject, key, name)) [[unlikely]]
        return !!Python::getStoredAttribute(globalObject->vm(), m_backing.get(), name);
    return Base::find(globalObject, key) >= 0;
}

bool PyDict::add(JSGlobalObject* globalObject, JSValue key, JSValue value, bool* wasAdded, bool replace)
{
    Identifier name;
    if (!isInBacking(globalObject, key, name)) [[likely]]
        return Base::add(globalObject, key, value, wasAdded, replace);
    VM& vm = globalObject->vm();
    bool isPresent = !!Python::getStoredAttribute(vm, m_backing.get(), name);
    if (wasAdded)
        *wasAdded = !isPresent;
    if (!isPresent || replace)
        Python::putStoredAttribute(vm, m_backing.get(), name, value);
    return true;
}

JSValue PyDict::remove(JSGlobalObject* globalObject, JSValue key)
{
    Identifier name;
    if (!isInBacking(globalObject, key, name)) [[likely]]
        return Base::remove(globalObject, key);
    JSValue value = Python::getStoredAttribute(globalObject->vm(), m_backing.get(), name);
    if (value)
        Python::deleteStoredAttribute(globalObject, m_backing.get(), name);
    return value;
}

bool PyDict::removeLast(JSGlobalObject* globalObject, JSValue& key, JSValue& value)
{
    VM& vm = globalObject->vm();
    for (unsigned entry = Base::entryCount(); entry--;) {
        key = Base::keyAt(entry);
        if (!key)
            continue;
        value = Base::valueAt(entry);
        Base::removeEntry(vm, entry);
        return true;
    }
    if (!m_backing)
        return false;
    PyTuple* keys = backingKeys(globalObject);
    if (!keys->length())
        return false;
    key = keys->at(keys->length() - 1);
    value = remove(globalObject, key);
    return true;
}

void PyDict::clear(JSGlobalObject* globalObject)
{
    Base::clear(globalObject->vm());
    if (!m_backing)
        return;
    PyTuple* keys = backingKeys(globalObject);
    for (auto& key : keys->span())
        remove(globalObject, key.get());
}

void PyDict::copyFrom(JSGlobalObject* globalObject, PyDict& other)
{
    if (!m_backing && !other.m_backing) [[likely]] {
        Base::copyFrom(globalObject->vm(), globalObject, other);
        return;
    }
    other.forEach(globalObject, [&] (JSValue key, JSValue value) {
        return add(globalObject, key, value);
    });
}

JSValue PyDict::getString(JSGlobalObject* globalObject, const String& key)
{
    if (m_backing) [[unlikely]] {
        auto name = Identifier::fromString(globalObject->vm(), key);
        if (!Python::isIndexLike(name))
            return Python::getStoredAttribute(globalObject->vm(), m_backing.get(), name);
    }
    return Base::getString(globalObject, key);
}

bool PyDict::setString(JSGlobalObject* globalObject, const String& key, JSValue value)
{
    return add(globalObject, jsString(globalObject->vm(), key), value);
}

// ---- PySet

const ClassInfo PySet::s_info = { "set"_s, &JSNonFinalObject::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PySet) };

PySet* PySet::create(VM& vm, Structure* structure)
{
    auto* set = new (NotNull, allocateCell<PySet>(vm)) PySet(vm, structure);
    set->finishCreation(vm);
    return set;
}

PySet* PySet::create(JSGlobalObject* globalObject)
{
    return create(globalObject->vm(), globalObject->pyRealm()->structureFor(BuiltinType::Set));
}

Structure* PySet::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(PySetType, StructureFlags), info());
}

} // namespace JSC
