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
#if ENABLE(AOT)
    static ptrdiff_t offsetOfCompactTable() { return OBJECT_OFFSETOF(JSOrderedHashTable, m_compactTable); }
    bool hasCompactTable() const { return !m_storage && m_compactTable; }
    static constexpr ptrdiff_t offsetOfAliveInCompactTable = 12;
#endif

    void finishCreation(VM& vm) { Base::finishCreation(vm); }
    void finishCreation(JSGlobalObject* globalObject, VM& vm, HashTable* base)
    {
        auto scope = DECLARE_THROW_SCOPE(vm);
        Base::finishCreation(vm);

#if ENABLE(AOT)
        if (!base->m_storage && base->m_compactTable) [[unlikely]] {
            copyCompactTableOf(vm, base);
            return;
        }
#endif
        if (base->hasStorage()) {
            Storage* storage = Helper::copy(globalObject, base->storageRef());
            RETURN_IF_EXCEPTION(scope, void());
            m_storage.set(vm, this, storage);
        }
    }

    ALWAYS_INLINE JSValue* getKeySlot(JSGlobalObject* globalObject, JSValue key, uint32_t hash)
    {
        if (hasStorage())
            return Helper::find(globalObject, storageRef(), key, hash).entryKeySlot;
        return nullptr;
    }

    ALWAYS_INLINE bool has(JSGlobalObject* globalObject, JSValue key)
    {
        if (m_storage) {
            auto result = Helper::find(globalObject, storageRef(), key);
            return result.entryKeyIndex != Helper::InvalidTableIndex;
        }
#if ENABLE(AOT)
        if (m_compactTable) [[unlikely]]
            return !!findInCompactTable(globalObject, key);
#endif
        return false;
    }

    ALWAYS_INLINE void add(JSGlobalObject* globalObject, JSValue key, JSValue value = { })
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

#if ENABLE(AOT)
        if (m_compactTable) [[unlikely]] {
            bool isAdded = tryAddToCompactTable(globalObject, key, value);
            RETURN_IF_EXCEPTION(scope, void());
            if (isAdded)
                return;
        }
#endif
        materializeIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, void());

        RELEASE_AND_RETURN(scope, Helper::add(globalObject, this, storageRef(), key, value));
    }
    ALWAYS_INLINE void addNormalized(JSGlobalObject* globalObject, JSValue key, JSValue value, uint32_t hash)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

#if ENABLE(AOT)
        if (m_compactTable) [[unlikely]] {
            bool isAdded = tryAddToCompactTable(globalObject, key, value);
            RETURN_IF_EXCEPTION(scope, void());
            if (isAdded)
                return;
        }
#endif
        materializeIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, void());

        RELEASE_AND_RETURN(scope, Helper::addNormalized(globalObject, this, storageRef(), key, value, hash));
    }

    ALWAYS_INLINE bool remove(JSGlobalObject* globalObject, JSValue key)
    {
        if (m_storage)
            return Helper::remove(globalObject, this, storageRef(), key);
#if ENABLE(AOT)
        if (m_compactTable) [[unlikely]]
            return removeFromCompactTable(globalObject, key);
#endif
        return false;
    }
    ALWAYS_INLINE bool removeNormalized(JSGlobalObject* globalObject, JSValue key, uint32_t hash)
    {
        if (m_storage)
            return Helper::removeNormalized(globalObject, this, storageRef(), key, hash);
#if ENABLE(AOT)
        if (m_compactTable) [[unlikely]]
            return removeFromCompactTable(globalObject, key);
#endif
        return false;
    }

    ALWAYS_INLINE uint32_t size()
    {
        if (m_storage)
            return Helper::aliveEntryCount(storageRef());
#if ENABLE(AOT)
        if (JSCell* table = m_compactTable.get()) [[unlikely]]
            return *std::bit_cast<const uint32_t*>(std::bit_cast<uintptr_t>(table) + offsetOfAliveInCompactTable);
#endif
        return 0;
    }

    ALWAYS_INLINE void clear(JSGlobalObject* globalObject)
    {
        if (m_storage)
            Helper::clear(globalObject, this, storageRef());
#if ENABLE(AOT)
        else if (m_compactTable) [[unlikely]]
            clearCompactTable();
#endif
    }

    ALWAYS_INLINE void materializeIfNeeded(JSGlobalObject* globalObject)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

        if (hasStorage())
            return;

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
        hasStorage();
        return m_storage.get();
    }

    ALWAYS_INLINE JSCell* storageOrSentinel(VM& vm)
    {
        if (hasStorage())
            return m_storage.get();
        return vm.orderedHashTableSentinel();
    }

    ALWAYS_INLINE Storage& storageRef()
    {
        ASSERT(m_storage);
        return *m_storage.get();
    }

    ALWAYS_INLINE bool hasStorage()
    {
        if (m_storage) [[likely]]
            return true;
#if ENABLE(AOT)
        if (m_compactTable) [[unlikely]] {
            convertCompactTable();
            return true;
        }
#endif
        return false;
    }

    WriteBarrier<Storage> m_storage;
#if ENABLE(AOT)
    WriteBarrier<JSCell> m_compactTable;

    JS_EXPORT_PRIVATE void convertCompactTable();
    JS_EXPORT_PRIVATE void copyCompactTableOf(VM&, JSOrderedHashTable*);
    JS_EXPORT_PRIVATE uint32_t sizeOfCompactTable();
    JS_EXPORT_PRIVATE void clearCompactTable();
    JS_EXPORT_PRIVATE JSValue findInCompactTable(JSGlobalObject*, JSValue key);
    JS_EXPORT_PRIVATE bool removeFromCompactTable(JSGlobalObject*, JSValue key);
    JS_EXPORT_PRIVATE bool tryAddToCompactTable(JSGlobalObject*, JSValue key, JSValue);
#endif
};

class JSOrderedHashMap : public JSOrderedHashTable<MapTraits> {
    using Base = JSOrderedHashTable<MapTraits>;

public:
    JSOrderedHashMap(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    template<typename FindKeyFunctor>
    ALWAYS_INLINE JSValue getImpl(JSGlobalObject* globalObject, JSValue key, NOESCAPE const FindKeyFunctor& findKeyFunctor)
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
#if ENABLE(AOT)
        if (m_compactTable) [[unlikely]]
            RELEASE_AND_RETURN(scope, findInCompactTable(globalObject, key));
#else
        UNUSED_PARAM(key);
#endif
        return { };
    }
    ALWAYS_INLINE JSValue get(JSGlobalObject* globalObject, JSValue key)
    {
        JSValue result = getImpl(globalObject, key, [&](Storage& storage) ALWAYS_INLINE_LAMBDA {
            return Helper::find(globalObject, storage, key);
        });
        return result.isEmpty() ? jsUndefined() : result;
    }
    ALWAYS_INLINE JSValue get(JSGlobalObject* globalObject, JSValue key, uint32_t hash)
    {
        JSValue result = getImpl(globalObject, key, [&](Storage& storage) ALWAYS_INLINE_LAMBDA {
            return Helper::find(globalObject, storage, key, hash);
        });
        return result.isEmpty() ? jsUndefined() : result;
    }
    ALWAYS_INLINE JSValue get(TableIndex keyIndex)
    {
        ASSERT(m_storage);
        return Helper::get(storageRef(), keyIndex + 1);
    }

    template<typename GetValueFunctor>
    ALWAYS_INLINE JSValue getOrInsert(JSGlobalObject* globalObject, JSValue key, NOESCAPE const GetValueFunctor& getValueFunctor)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

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
