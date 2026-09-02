/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
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

#include <JavaScriptCore/JSOrderedHashTableHelper.h>

namespace JSC {

template<typename Traits>
class JSOrderedHashTable : public JSNonFinalObject {
    using Base = JSNonFinalObject;

public:
    using HashTable = JSOrderedHashTable<Traits>;
    using Helper = JSOrderedHashTableHelper<Traits>;
    using Storage = JSCellButterfly;
    using TableIndex = typename Helper::TableIndex;

    DECLARE_VISIT_CHILDREN;

    JSOrderedHashTable(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    static ptrdiff_t offsetOfStorage() { return OBJECT_OFFSETOF(JSOrderedHashTable, m_storage); }

    void finishCreation(VM& vm) { Base::finishCreation(vm); }
    void finishCreation(JSGlobalObject* globalObject, VM& vm, HashTable* base)
    {
        auto scope = DECLARE_THROW_SCOPE(vm);
        Base::finishCreation(vm);

        if (vm.gilOff()) [[unlikely]] {
            Storage* storage = Helper::copyGILOff(globalObject, base);
            RETURN_IF_EXCEPTION(scope, void());
            if (storage)
                m_storage.set(vm, this, storage);
            return;
        }

        if (base->m_storage) {
            Storage* storage = Helper::copy(globalObject, base->storageRef());
            RETURN_IF_EXCEPTION(scope, void());
            m_storage.set(vm, this, storage);
        }
    }

    // Only for the DFG and FTL, which do not inline Map and Set operations
    // with the GIL off: the slot is not protected once the table lock is gone.
    ALWAYS_INLINE JSValue* getKeySlot(JSGlobalObject* globalObject, JSValue key, uint32_t hash)
    {
        ASSERT(!getVM(globalObject).gilOff());
        if (m_storage)
            return Helper::find(globalObject, storageRef(), key, hash).entryKeySlot;
        return nullptr;
    }

    ALWAYS_INLINE bool has(JSGlobalObject* globalObject, JSValue key)
    {
        if (getVM(globalObject).gilOff()) [[unlikely]]
            return hasGILOff(globalObject, key);
        if (m_storage) {
            auto result = Helper::find(globalObject, storageRef(), key);
            return result.entryKeyIndex != Helper::InvalidTableIndex;
        }
        return false;
    }

    ALWAYS_INLINE void add(JSGlobalObject* globalObject, JSValue key, JSValue value = { })
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

        if (vm.gilOff()) [[unlikely]] {
            JSValue normalizedKey = normalizeMapKey(key);
            uint32_t hash = jsMapHash(globalObject, vm, normalizedKey);
            RETURN_IF_EXCEPTION(scope, void());
            RELEASE_AND_RETURN(scope, Helper::addNormalizedGILOff(globalObject, this, normalizedKey, value, hash));
        }

        materializeIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, void());

        RELEASE_AND_RETURN(scope, Helper::add(globalObject, this, storageRef(), key, value));
    }
    ALWAYS_INLINE void addNormalized(JSGlobalObject* globalObject, JSValue key, JSValue value, uint32_t hash)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

        if (vm.gilOff()) [[unlikely]]
            RELEASE_AND_RETURN(scope, Helper::addNormalizedGILOff(globalObject, this, key, value, hash));

        materializeIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, void());

        RELEASE_AND_RETURN(scope, Helper::addNormalized(globalObject, this, storageRef(), key, value, hash));
    }

    ALWAYS_INLINE bool remove(JSGlobalObject* globalObject, JSValue key)
    {
        if (getVM(globalObject).gilOff()) [[unlikely]]
            return removeGILOff(globalObject, key);
        if (m_storage)
            return Helper::remove(globalObject, this, storageRef(), key);
        return false;
    }
    ALWAYS_INLINE bool removeNormalized(JSGlobalObject* globalObject, JSValue key, uint32_t hash)
    {
        if (getVM(globalObject).gilOff()) [[unlikely]]
            return Helper::removeNormalizedGILOff(globalObject, this, key, hash);
        if (m_storage)
            return Helper::removeNormalized(globalObject, this, storageRef(), key, hash);
        return false;
    }

    ALWAYS_INLINE uint32_t size()
    {
        if (Helper::iterationLiteIfGILOff()) [[unlikely]] {
            if (!m_storage)
                return 0;
            return Helper::withCurrentTableLockedGILOff(this, [](Storage& storage) {
                return Helper::aliveEntryCount(storage);
            });
        }
        if (m_storage)
            return Helper::aliveEntryCount(storageRef());
        return 0;
    }

    ALWAYS_INLINE void clear(JSGlobalObject* globalObject)
    {
        if (getVM(globalObject).gilOff()) [[unlikely]] {
            Helper::clearGILOff(globalObject, this);
            return;
        }
        if (m_storage)
            Helper::clear(globalObject, this, storageRef());
    }

    ALWAYS_INLINE void materializeIfNeeded(JSGlobalObject* globalObject)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

        if (m_storage)
            return;

        if (vm.gilOff()) [[unlikely]] {
            Helper::materializeGILOff(globalObject, this);
            return;
        }

        Storage* storage = Helper::tryCreate(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        m_storage.set(vm, this, storage);
    }

    ALWAYS_INLINE JSCell* tryGetStorage(JSGlobalObject* globalObject)
    {
        materializeIfNeeded(globalObject);
        return m_storage.get();
    }

    ALWAYS_INLINE JSCell* storage()
    {
        return m_storage.get();
    }

    ALWAYS_INLINE JSCell* storageOrSentinel(VM& vm)
    {
        if (m_storage)
            return m_storage.get();
        return vm.orderedHashTableSentinel();
    }

    ALWAYS_INLINE Storage& storageRef()
    {
        ASSERT(m_storage);
        return *m_storage.get();
    }

    // With the GIL off, the key is normalized and hashed before the table lock
    // is taken, because hashing a rope key resolves it (see the helper).
    bool hasGILOff(JSGlobalObject* globalObject, JSValue key)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!m_storage)
            return false;
        JSValue normalizedKey = normalizeMapKey(key);
        uint32_t hash = jsMapHash(globalObject, vm, normalizedKey);
        RETURN_IF_EXCEPTION(scope, false);
        return Helper::withCurrentTableLockedGILOff(this, [&](Storage& storage) {
            return Helper::isValidTableIndex(Helper::find(globalObject, storage, normalizedKey, hash).entryKeyIndex);
        });
    }

    bool removeGILOff(JSGlobalObject* globalObject, JSValue key)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!m_storage)
            return false;
        JSValue normalizedKey = normalizeMapKey(key);
        uint32_t hash = jsMapHash(globalObject, vm, normalizedKey);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, Helper::removeNormalizedGILOff(globalObject, this, normalizedKey, hash));
    }

    WriteBarrier<Storage> m_storage;
};

class JSOrderedHashMap : public JSOrderedHashTable<MapTraits> {
    using Base = JSOrderedHashTable<MapTraits>;

public:
    JSOrderedHashMap(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    template<typename FindKeyFunctor>
    ALWAYS_INLINE JSValue getImpl(JSGlobalObject* globalObject, const FindKeyFunctor& findKeyFunctor)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

        if (m_storage) {
            Storage& storage = storageRef();
            auto result = findKeyFunctor(storage);
            RETURN_IF_EXCEPTION(scope, { });

            if (!Helper::isValidTableIndex(result.entryKeyIndex))
                return { };
            return Helper::get(storage, result.entryKeyIndex + 1);
        }
        return { };
    }
    ALWAYS_INLINE JSValue get(JSGlobalObject* globalObject, JSValue key)
    {
        VM& vm = getVM(globalObject);
        if (vm.gilOff()) [[unlikely]] {
            auto scope = DECLARE_THROW_SCOPE(vm);
            JSValue normalizedKey = normalizeMapKey(key);
            uint32_t hash = jsMapHash(globalObject, vm, normalizedKey);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue result = getGILOff(globalObject, normalizedKey, hash);
            return result.isEmpty() ? jsUndefined() : result;
        }
        JSValue result = getImpl(globalObject, [&](Storage& storage) ALWAYS_INLINE_LAMBDA {
            return Helper::find(globalObject, storage, key);
        });
        return result.isEmpty() ? jsUndefined() : result;
    }
    ALWAYS_INLINE JSValue get(JSGlobalObject* globalObject, JSValue key, uint32_t hash)
    {
        if (getVM(globalObject).gilOff()) [[unlikely]] {
            JSValue result = getGILOff(globalObject, key, hash);
            return result.isEmpty() ? jsUndefined() : result;
        }
        JSValue result = getImpl(globalObject, [&](Storage& storage) ALWAYS_INLINE_LAMBDA {
            return Helper::find(globalObject, storage, key, hash);
        });
        return result.isEmpty() ? jsUndefined() : result;
    }
    // Only for the DFG and FTL, like getKeySlot.
    ALWAYS_INLINE JSValue get(TableIndex keyIndex)
    {
        ASSERT(m_storage);
        return Helper::get(storageRef(), keyIndex + 1);
    }

    template<typename GetValueFunctor>
    ALWAYS_INLINE JSValue getOrInsert(JSGlobalObject* globalObject, JSValue key, const GetValueFunctor& getValueFunctor)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

        if (vm.gilOff()) [[unlikely]] {
            // The value is computed with no lock held, because that can call
            // into JS. Another thread can add the key meanwhile; the add below
            // then overwrites its value.
            JSValue normalizedKey = normalizeMapKey(key);
            uint32_t hash = jsMapHash(globalObject, vm, normalizedKey);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue value = getGILOff(globalObject, normalizedKey, hash);
            if (!value.isEmpty())
                return value;
            value = getValueFunctor();
            RETURN_IF_EXCEPTION(scope, { });
            Helper::addNormalizedGILOff(globalObject, this, normalizedKey, value, hash);
            RETURN_IF_EXCEPTION(scope, { });
            return value;
        }

        materializeIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, { });

        Storage& storage = storageRef();

        JSValue value;

        auto result = Helper::find(globalObject, storage, key);
        RETURN_IF_EXCEPTION(scope, { });

        if (Helper::isValidTableIndex(result.entryKeyIndex))
            value = Helper::get(storage, result.entryKeyIndex + 1);
        else {
            value = getValueFunctor();
            RETURN_IF_EXCEPTION(scope, { });

            // Call to getValueFunctor can modify our state, so we need to re-check the index
            // There is a chance that callback inserts an entry for this |key|.
            add(globalObject, key, value);
            RETURN_IF_EXCEPTION(scope, { });
        }

        return value;
    }

    // Returns the empty value when the key is absent.
    JSValue getGILOff(JSGlobalObject* globalObject, JSValue normalizedKey, uint32_t hash)
    {
        if (!m_storage)
            return { };
        return Helper::withCurrentTableLockedGILOff(this, [&](Storage& storage) -> JSValue {
            auto result = Helper::find(globalObject, storage, normalizedKey, hash);
            if (!Helper::isValidTableIndex(result.entryKeyIndex))
                return { };
            return Helper::get(storage, result.entryKeyIndex + 1);
        });
    }

    static JSCell* createSentinel(VM& vm) { return Helper::tryCreate(vm, 0); }
    static Symbol* createDeletedValue(VM& vm) { return Symbol::create(vm); }
};

class JSOrderedHashSet : public JSOrderedHashTable<SetTraits> {
    using Base = JSOrderedHashTable<SetTraits>;

public:
    JSOrderedHashSet(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};

} // namespace JSC
