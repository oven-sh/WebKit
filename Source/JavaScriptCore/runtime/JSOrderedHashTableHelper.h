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

#include <JavaScriptCore/DeferGC.h>
#include <JavaScriptCore/GCMemoryOperations.h>
#include <JavaScriptCore/HashMapHelper.h>
#include <JavaScriptCore/JSCellButterfly.h>
#include <JavaScriptCore/JSObject.h>
#include <JavaScriptCore/VMLite.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

struct MapTraits {
    // DataTable:  [ Key_0, Val_0, NextKeyIndexInChain_0, Key_1, Val_1, NextKeyIndexInChain_1, ... ]
    //             | <------------ Entry_0 -----------> | <------------ Entry_1 -----------> |
    static constexpr uint8_t EntrySize = 3;
    static constexpr bool hasValueData = true;
};

struct SetTraits {
    // DataTable:  [ Key_0, NextKeyIndexInChain_0, Key_1, NextKeyIndexInChain_1, ... ]
    //             | <-------- Entry_0 --------> | <-------- Entry_1 --------> |
    static constexpr uint8_t EntrySize = 2;
    static constexpr bool hasValueData = false;
};

template<typename Traits>
class JSOrderedHashTable;

// ################ NonObsolete Table ################
//
//                          Count                                      Value(s)                           ValueType                  WriteBarrier
//               -----------------------------------------------------------------------------------------------------------------------------------------
// TableStart ->             1            | AliveEntryCount                                            | TableSize                | NO
//                           1            | DeletedEntryCount                                          | TableSize                | NO
//                           1            | Capacity                                                   | TableSize                | NO
//                           1            | IterationEntry                                             | Entry                    | NO
//                       BucketCount      | HashTable:  { <BucketIndex, ChainStartKeyIndex>, ... }     | <TableIndex, TableIndex> | NO
//                      DataTableSize     | DataTable:  { <Entry_0, Data_0>, <Entry_1, Data_1>, ... }  | <TableIndex, JSValue>    | Yes (Key or Value)
//  TableEnd  ->   IterationSentinelSlots | IterationSentinel                                          | JSValue                  | NO
//
// ################ Obsolete Table from Rehash ################
//
//                      Count                                    Value(s)                            ValueType                  WriteBarrier
//               -------------------------------------------------------------------------------------------------------------------------------------
// TableStart ->           1          | NextTable                                                   | JSCell                   | Yes
//                         1          | DeletedEntryCount                                           | TableSize                | NO
//                  DeletedEntryCount | DeletedEntries: [DeletedEntry_i, ...]                       | Entry                    | NO
//  TableEnd  ->          ...         | NotUsed                                                     | ...
//
// ################ Obsolete Table from Clear ################
//
//                      Count                                    Value(s)                            ValueType                  WriteBarrier
//               -------------------------------------------------------------------------------------------------------------------------------------
// TableStart ->           1          | NextTable                                                   | JSCell                   | Yes
//                         1          | ClearedTableSentinel                                        | TableSize                | NO
//  TableEnd  ->          ...         | NotUsed                                                     | ...
//
//
// This is based on the idea of CloseTable introduced in https://wiki.mozilla.org/User:Jorend/Deterministic_hash_tables.
//
// NonObsolete Table: A CloseTable flattened into a single array.
//
// - AliveEntryCount: The number of alive entries in the table.
// - DeletedEntryCount: The number of deleted entries in the table.
// - Capacity: The capacity of entries in the table.
// - IterationEntry: The temporary index only used for iteration.
// - HashTable: CloseTable is based on closed hashing. So, the each bucket in the HashTable points to the start EntryKeyIndex in the collision chain.
//   - FIXME: We can get rid of HashTable by applying OpenTable.
// - DataTable: The actual data table holds the tuples of key, value, and NextKeyIndexInChain for each entry.
//
//      Iterator                                   Map/Set
//          |                                         |
//          v                                         v
//    ObsoleteTable_0 --> ObsoleteTable_1 --> NonObsoleteTable_2
//
// Obsolete Table: Each rehash and clear would create a new table and update the obsolete one with additional info. Those info are used to
// transit the iterator (pointing to the obsolete table) to the latest alive table.
//
// - NextTable: A transition from the obsolete tables to the latest-non-obsolete one.
// - DeletedEntries: An array of the previously deleted entries used for updating iterator's index.
// - ClearedTableSentinel: The sentinel to indicate whether the obsolete table is retired due to a clearance.
//
// Note that all elements in the JSCellButterfly are in JSValue type. However, only the key and value in the DataTable are real JSValues.
// The others are used as unsigned integers wrapped by JSValue.
template<typename Traits>
class JSOrderedHashTableHelper {
public:
    using Storage = JSCellButterfly;
    using Helper = JSOrderedHashTableHelper<Traits>;
    using HashTable = JSOrderedHashTable<Traits>;
    using TableSize = uint32_t;
    using Entry = TableSize;
    using TableIndex = TableSize;

    static constexpr TableSize ClearedTableSentinel = -1;
    static constexpr TableIndex InvalidTableIndex = -1;
    static constexpr uint8_t EntrySize = Traits::EntrySize;
    static constexpr uint8_t ChainOffset = Traits::EntrySize - 1;

    static constexpr uint8_t InitialCapacity = 8;
    static constexpr TableSize LargeCapacity = 2 << 15;
    static constexpr TableSize MemcpyCopyMinimumCapacity = 1024;

    static_assert(EntrySize == MapTraits::EntrySize || EntrySize == SetTraits::EntrySize);

    JSOrderedHashTableHelper() = delete;

    ALWAYS_INLINE static TableSize toNumber(JSValue number) { return static_cast<TableSize>(number.asInt32()); }
    ALWAYS_INLINE static constexpr TableSize asNumber(Storage& storage, TableIndex index) { return toNumber(get(storage, index)); }
    ALWAYS_INLINE static JSValue toJSValue(Entry entry)
    {
        JSValue value = JSValue();
        JSOrderedHashTableTraits::set(&value, entry);
        return value;
    }

    ALWAYS_INLINE static bool isDeleted(VM& vm, JSValue value) { return value.isCell() && value.asCell() == vm.orderedHashTableDeletedValue(); }
    ALWAYS_INLINE static constexpr bool isValidTableIndex(TableIndex index) { return index != InvalidTableIndex; }
    ALWAYS_INLINE static JSValue invalidTableIndex() { return toJSValue(InvalidTableIndex); }

    ALWAYS_INLINE static JSValue get(Storage& storage, TableIndex index) { return storage.toButterfly()->contiguous().atUnsafe(index).get(); }
    ALWAYS_INLINE static JSValue* slot(Storage& storage, TableIndex index) { return storage.toButterfly()->contiguous().atUnsafe(index).slot(); }

    ALWAYS_INLINE static void set(Storage& storage, TableIndex index, JSValue value) { return storage.toButterfly()->contiguous().atUnsafe(index).setWithoutWriteBarrier(value); }
    ALWAYS_INLINE static void set(Storage& storage, TableIndex index, TableSize number) { JSOrderedHashTableTraits::set(slot(storage, index), number); }
    ALWAYS_INLINE static void setWithWriteBarrier(VM& vm, Storage& storage, TableIndex index, JSValue value) { storage.toButterfly()->contiguous().atUnsafe(index).set(vm, &storage, value); }

    /* -------------------------------- AliveEntryCount, DeletedEntryCount, Capacity, and IterationEntry -------------------------------- */
    ALWAYS_INLINE static constexpr TableIndex aliveEntryCountIndex() { return 0; }
    ALWAYS_INLINE static constexpr TableIndex deletedEntryCountIndex() { return aliveEntryCountIndex() + 1; }
    ALWAYS_INLINE static constexpr TableIndex capacityIndex() { return deletedEntryCountIndex() + 1; }
    ALWAYS_INLINE static constexpr TableIndex iterationEntryIndex() { return capacityIndex() + 1; }

    ALWAYS_INLINE static constexpr size_t offsetInStorageForIndex(TableIndex index) { return JSCellButterfly::offsetOfData() + index * sizeof(uint64_t); }
    ALWAYS_INLINE static constexpr size_t offsetOfAliveEntryCount() { return offsetInStorageForIndex(aliveEntryCountIndex()); }

    ALWAYS_INLINE static constexpr TableSize aliveEntryCount(Storage& storage) { return asNumber(storage, aliveEntryCountIndex()); }
    ALWAYS_INLINE static constexpr TableSize deletedEntryCount(Storage& storage) { return asNumber(storage, deletedEntryCountIndex()); }
    ALWAYS_INLINE static constexpr TableSize usedCapacity(Storage& storage) { return aliveEntryCount(storage) + deletedEntryCount(storage); }
    ALWAYS_INLINE static constexpr TableSize capacity(Storage& storage) { return asNumber(storage, capacityIndex()); }
    ALWAYS_INLINE static TableSize iterationEntry(Storage& storage)
    {
        if (VMLite* lite = iterationLiteIfGILOff()) [[unlikely]]
            return lite->orderedHashTableIterationEntry;
        return asNumber(storage, iterationEntryIndex());
    }

    ALWAYS_INLINE static constexpr void incrementAliveEntryCount(Storage& storage) { JSOrderedHashTableTraits::increment(slot(storage, aliveEntryCountIndex())); }
    ALWAYS_INLINE static constexpr void decrementAliveEntryCount(Storage& storage) { JSOrderedHashTableTraits::decrement(slot(storage, aliveEntryCountIndex())); }
    ALWAYS_INLINE static constexpr void incrementDeletedEntryCount(Storage& storage) { JSOrderedHashTableTraits::increment(slot(storage, deletedEntryCountIndex())); }

    /* -------------------------------- Hash table -------------------------------- */
    ALWAYS_INLINE static constexpr TableSize bucketCount(TableSize capacity) { return capacity; }

    ALWAYS_INLINE static constexpr TableIndex hashTableStartIndex() { return iterationEntryIndex() + 1; }
    ALWAYS_INLINE static constexpr TableIndex hashTableEndIndex(TableSize capacity) { return hashTableStartIndex() + bucketCount(capacity) - 1; }

    ALWAYS_INLINE static constexpr TableIndex bucketIndex(TableIndex hashTableStartIndex, TableSize bucketCount, TableSize hash) { return hashTableStartIndex + (hash & (bucketCount - 1)); }
    ALWAYS_INLINE static constexpr TableIndex bucketIndex(TableSize capacity, uint32_t hash) { return bucketIndex(hashTableStartIndex(), bucketCount(capacity), hash); }

    /* -------------------------------- Data table -------------------------------- */
    // The DataTable is packed and only holds up to dataCapacity entries before a rehash is forced.
    // dataCapacity matches the rehash threshold in expandIfNeeded: 50% of capacity for small tables,
    // 75% for large. Sizing the DataTable this way avoids the dead tail that would otherwise sit
    // between the rehash threshold and the end of the buffer.
    ALWAYS_INLINE static constexpr TableSize dataCapacity(TableSize capacity)
    {
        if (capacity < LargeCapacity)
            return capacity / 2;
        return (capacity / 4) * 3;
    }
    ALWAYS_INLINE static constexpr TableSize dataTableSize(TableSize capacity) { return dataCapacity(capacity) * EntrySize; }

    ALWAYS_INLINE static constexpr TableIndex dataTableStartIndex(TableSize capacity) { return hashTableStartIndex() + bucketCount(capacity); }
    // Index of the last DataTable slot proper. The allocated storage extends one further
    // slot — the IterationSentinel — so use tableSize() / tableSizeSlow() when reasoning
    // about total length, not dataTableEndIndex().
    ALWAYS_INLINE static constexpr TableIndex dataTableEndIndex(TableSize capacity) { return dataTableStartIndex(capacity) + dataTableSize(capacity) - 1; }
    ALWAYS_INLINE static constexpr TableIndex entryDataStartIndex(TableIndex dataTableStartIndex, Entry entry) { return dataTableStartIndex + entry * EntrySize; }

    ALWAYS_INLINE static void setKeyOrValueData(VM& vm, Storage& storage, TableIndex index, JSValue value) { setWithWriteBarrier(vm, storage, index, value); }
    ALWAYS_INLINE static constexpr void deleteData(VM& vm, Storage& storage, TableIndex index) { return set(storage, index, vm.orderedHashTableDeletedValue()); }

    ALWAYS_INLINE static void addToChain(Storage& storage, TableIndex bucketIndex, TableIndex newChainStartKeyIndex)
    {
        JSValue prevChainStartKeyIndex = get(storage, bucketIndex);
        set(storage, bucketIndex, newChainStartKeyIndex);
        set(storage, newChainStartKeyIndex + ChainOffset, prevChainStartKeyIndex);
    }

    /* -------------------------------- JSOrderedHashTable -------------------------------- */
    // One trailing zero-initialized JSValue is allocated past the DataTable so the iterator
    // can safely read one slot past the last entry when the table is fully packed.
    static constexpr TableSize iterationSentinelSlots = 1;

    ALWAYS_INLINE static constexpr TableSize tableSize(TableSize capacity)
    {
        TableSize result = 4 /* AliveEntryCount, DeletedEntryCount, Capacity, and IterationEntry */
            + bucketCount(capacity) /* BucketCount */
            + dataTableSize(capacity) /* DataTableSize */
            + iterationSentinelSlots;
        ASSERT(result == tableSizeSlow(capacity));
        return result;
    }
    ALWAYS_INLINE static constexpr TableSize tableSizeSlow(TableSize capacity) { return dataTableEndIndex(capacity) + 1 + iterationSentinelSlots; }

    /* -------------------------------- Obsolete table -------------------------------- */
    ALWAYS_INLINE static constexpr bool isObsolete(Storage& storage) { return !!nextTable(storage); }
    ALWAYS_INLINE static constexpr Storage* nextTable(Storage& storage)
    {
        JSValue* value = slot(storage, aliveEntryCountIndex());
        if (!value->isInt32()) {
            ASSERT(is<Storage>(*value));
            return uncheckedDowncast<Storage>(*value);
        }
        return nullptr;
    }
    ALWAYS_INLINE static constexpr void setNextTable(VM& vm, Storage& storage, Storage* next) { setWithWriteBarrier(vm, storage, aliveEntryCountIndex(), next); }

    ALWAYS_INLINE static constexpr TableIndex deletedEntriesStartIndex() { return capacityIndex(); }

    ALWAYS_INLINE static constexpr bool isCleared(Storage& storage) { return asNumber(storage, deletedEntryCountIndex()) == ClearedTableSentinel; }
    ALWAYS_INLINE static constexpr void setClearedTableSentinel(Storage& storage) { set(storage, deletedEntryCountIndex(), ClearedTableSentinel); }

    /* ------------------------------------------------------------------------------------ */

    ALWAYS_INLINE static Storage* tryCreate(VM& vm, int length)
    {
        // FIXME: Why is this CopyOnWrite? We definitely modify it...
        return Storage::tryCreate(vm, vm.cellButterflyStructure(CopyOnWriteArrayWithContiguous), length);
    }
    ALWAYS_INLINE static Storage* tryCreate(JSGlobalObject* globalObject, TableSize aliveEntryCount = 0, TableSize deletedEntryCount = 0, TableSize capacity = InitialCapacity)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        ASSERT(!(capacity & (capacity - 1)));

        TableSize length = tableSize(capacity);
        if (length > IndexingHeader::maximumLength) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return nullptr;
        }

        Storage* storage = tryCreate(vm, length);
        if (!storage) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return nullptr;
        }

        Storage& storageRef = *storage;
        set(storageRef, aliveEntryCountIndex(), aliveEntryCount);
        set(storageRef, deletedEntryCountIndex(), deletedEntryCount);
        set(storageRef, capacityIndex(), capacity);
        return storage;
    }

    enum class UpdateDeletedEntries : uint8_t {
        Yes,
        No
    };
    template<UpdateDeletedEntries update = UpdateDeletedEntries::No>
    ALWAYS_INLINE static Storage* copyImpl(JSGlobalObject* globalObject, Storage& base, TableSize newCapacity)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

        TableSize baseCapacity = capacity(base);
        TableSize baseAliveEntryCount = aliveEntryCount(base);
        ASSERT(!isObsolete(base));
        ASSERT_UNUSED(baseAliveEntryCount, newCapacity >= std::max(static_cast<TableSize>(InitialCapacity), baseAliveEntryCount));
        ASSERT_UNUSED(baseCapacity, usedCapacity(base) <= baseCapacity);

        Storage* copy = tryCreate(globalObject, baseAliveEntryCount, 0, newCapacity);
        RETURN_IF_EXCEPTION(scope, nullptr);

        TableIndex baseEntryKeyIndex = dataTableStartIndex(baseCapacity);
        TableIndex baseDeletedEntriesIndex = deletedEntriesStartIndex();

        Storage& copyRef = *copy;
        TableIndex newEntryKeyIndex = dataTableStartIndex(newCapacity);
        TableIndex newHashTableStartIndex = hashTableStartIndex();
        TableIndex newBucketCount = bucketCount(newCapacity);

        for (Entry baseEntry = 0;; ++baseEntry, baseEntryKeyIndex += EntrySize) {
            JSValue baseKey = get(base, baseEntryKeyIndex);
            if (!baseKey)
                break;

            // Step 1: Copy DataTable only for the alive entries.
            if (isDeleted(vm, baseKey)) {
                if constexpr (update == UpdateDeletedEntries::Yes)
                    set(base, baseDeletedEntriesIndex++, baseEntry);
                continue;
            }

            // Step 2: Copy the key and value from the base table to the new table.
            setKeyOrValueData(vm, copyRef, newEntryKeyIndex, baseKey);
            if constexpr (Traits::hasValueData) {
                JSValue baseValue = get(base, baseEntryKeyIndex + 1);
                setKeyOrValueData(vm, copyRef, newEntryKeyIndex + 1, baseValue);
            }

            // Step 3: Compute for the hash value and add to the chain in the new table. Note that the
            // key stored in the base table is already normalized.
            TableSize hash = jsMapHash(globalObject, vm, baseKey);
            RETURN_IF_EXCEPTION(scope, nullptr);
            TableIndex newBucketIndex = bucketIndex(newHashTableStartIndex, newBucketCount, hash);
            addToChain(copyRef, newBucketIndex, newEntryKeyIndex);
            newEntryKeyIndex += EntrySize;
        }

        return copy;
    }
    ALWAYS_INLINE static Storage* copy(JSGlobalObject* globalObject, Storage& base)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        ASSERT(!isObsolete(base));

        TableSize capacity = Helper::capacity(base);
        if (!deletedEntryCount(base) && capacity >= MemcpyCopyMinimumCapacity) {
            Storage* result = tryCreate(globalObject, 0, 0, capacity);
            RETURN_IF_EXCEPTION(scope, nullptr);
            TableSize usedLength = dataTableStartIndex(capacity) + aliveEntryCount(base) * EntrySize;
            gcSafeMemcpy(slot(*result, 0), slot(base, 0), usedLength * sizeof(JSValue));
            vm.writeBarrier(result);
            return result;
        }

        RELEASE_AND_RETURN(scope, copyImpl<>(globalObject, base, capacity));
    }

    ALWAYS_INLINE static Storage* rehash(JSGlobalObject* globalObject, Storage& base, TableSize newCapacity)
    {
        return copyImpl<UpdateDeletedEntries::Yes>(globalObject, base, newCapacity);
    }

    ALWAYS_INLINE static void clear(JSGlobalObject* globalObject, HashTable* owner, Storage& base)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

        Storage* next = tryCreate(globalObject);
        RETURN_IF_EXCEPTION(scope, void());

        setClearedTableSentinel(base);
        setNextTable(vm, base, next);
        owner->m_storage.set(vm, owner, next);
    }

    struct FindResult {
        JSValue normalizedKey;
        uint32_t hash;
        TableIndex bucketIndex;
        TableIndex entryKeyIndex;
        // The keyIndex and keySlot may be redundant here. Let's leave it for now
        // since the following OpenTable patch can get rid of most of index related info for us.
        JSValue* entryKeySlot;
    };
    ALWAYS_INLINE static FindResult find(JSGlobalObject* globalObject, Storage& storage, JSValue key)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        ASSERT(!isObsolete(storage));

        if (!aliveEntryCount(storage))
            return { JSValue(), 0, InvalidTableIndex, InvalidTableIndex, nullptr };

        key = normalizeMapKey(key);
        TableSize hash = jsMapHash(globalObject, vm, key);
        RETURN_IF_EXCEPTION(scope, { });
        return find(globalObject, storage, key, hash);
    }
    ALWAYS_INLINE static FindResult find(JSGlobalObject* globalObject, Storage& storage, JSValue normalizedKey, TableSize hash)
    {
        VM& vm = getVM(globalObject);
        TableIndex bucketIndex = Helper::bucketIndex(capacity(storage), hash);
        ASSERT(!isObsolete(storage) && normalizeMapKey(normalizedKey) == normalizedKey);

        JSValue keyIndexValue = get(storage, bucketIndex);
        while (!keyIndexValue.isEmpty()) {
            TableIndex entryKeyIndex = toNumber(keyIndexValue);
            JSValue* entryKeySlot = slot(storage, entryKeyIndex);
            // Fixme: Maybe we can compress the searching path by updating the chain with non-deleted entry.
            if (!isDeleted(vm, *entryKeySlot) && areKeysEqual(globalObject, normalizedKey, *entryKeySlot))
                return { normalizedKey, hash, bucketIndex, entryKeyIndex, entryKeySlot };
            keyIndexValue = get(storage, entryKeyIndex + ChainOffset);
        }
        return { normalizedKey, hash, bucketIndex, InvalidTableIndex, nullptr };
    }

    ALWAYS_INLINE static Storage* expandIfNeeded(JSGlobalObject* globalObject, Storage& base)
    {
        ASSERT(!isObsolete(base));
        TableSize capacity = Helper::capacity(base);
        TableSize dataCapacity = Helper::dataCapacity(capacity);
        TableSize deletedEntryCount = Helper::deletedEntryCount(base);
        TableSize usedCapacity = Helper::aliveEntryCount(base) + deletedEntryCount;

        if (usedCapacity < dataCapacity)
            return nullptr;

        bool isSmallCapacity = capacity < LargeCapacity;
        TableSize expansionFactor = isSmallCapacity ? 4 : 2;
        TableSize newCapacity = Checked<TableSize>(capacity) * expansionFactor;
        if (deletedEntryCount >= (dataCapacity / 2)) {
            // No need to expand. Just clear the deleted entries.
            // FIXME: Can we do this in place?
            newCapacity = capacity;
        }
        return rehash(globalObject, base, newCapacity);
    }
    template<typename FindKeyFunctor>
    ALWAYS_INLINE static void addImpl(JSGlobalObject* globalObject, HashTable* owner, Storage& base, JSValue key, JSValue value, const FindKeyFunctor& findKeyFunctor)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        ASSERT(!isObsolete(base));

        auto result = findKeyFunctor();
        RETURN_IF_EXCEPTION(scope, void());

        if (isValidTableIndex(result.entryKeyIndex)) {
            if constexpr (Traits::hasValueData)
                setKeyOrValueData(vm, base, result.entryKeyIndex + 1, value);
            return;
        }

        scope.release();
        addImpl(globalObject, owner, base, key, value, result);
    }
    ALWAYS_INLINE static void addImpl(JSGlobalObject* globalObject, HashTable* owner, Storage& base, JSValue key, JSValue value, FindResult& result)
    {
        VM& vm = getVM(globalObject);
        // We're transitioning between states here, if a termination comes in we could leave the storage
        // in an inconsistent state. It's much easier to pause termination until the storage is updated.
        DeferTerminationForAWhile noTermination(vm);
        auto scope = DECLARE_THROW_SCOPE(vm);
        ASSERT(!isObsolete(base));
        ASSERT(!isValidTableIndex(result.entryKeyIndex));

        bool firstAliveEntry = result.normalizedKey.isEmpty();
        if (firstAliveEntry) [[unlikely]] {
            result.normalizedKey = normalizeMapKey(key);
            result.hash = jsMapHash(globalObject, vm, result.normalizedKey);
            RETURN_IF_EXCEPTION(scope, void());
        }

        Storage* newBuffer = expandIfNeeded(globalObject, base);
        RETURN_IF_EXCEPTION(scope, void());

        bool rehashed = newBuffer;
        Storage& storage = newBuffer ? *newBuffer : base;

        TableSize capacity = Helper::capacity(storage);
        Entry newEntry = usedCapacity(storage);
        TableIndex newEntryKeyIndex = entryDataStartIndex(dataTableStartIndex(capacity), newEntry);
        incrementAliveEntryCount(storage);

        if (rehashed || firstAliveEntry) [[unlikely]]
            result.bucketIndex = bucketIndex(capacity, result.hash);

        addToChain(storage, result.bucketIndex, newEntryKeyIndex);
        setKeyOrValueData(vm, storage, newEntryKeyIndex, result.normalizedKey);
        if constexpr (Traits::hasValueData)
            setKeyOrValueData(vm, storage, newEntryKeyIndex + 1, value);

        if (rehashed) [[unlikely]] {
            // Only commit the new buffer once everything is set up. This way if things change and we end up throwing an exception in the middle we're not left in an inconsistent state.
            ASSERT(&storage == newBuffer);
            setNextTable(vm, base, newBuffer);
            owner->m_storage.set(vm, owner, newBuffer);
        }
    }
    ALWAYS_INLINE static void add(JSGlobalObject* globalObject, HashTable* owner, Storage& storage, JSValue key, JSValue value)
    {
        addImpl(globalObject, owner, storage, key, value, [&]() ALWAYS_INLINE_LAMBDA {
            return find(globalObject, storage, key);
        });
    }
    ALWAYS_INLINE static void addNormalized(JSGlobalObject* globalObject, HashTable* owner, Storage& storage, JSValue normalizedKey, JSValue value, TableSize hash)
    {
        ASSERT(normalizeMapKey(normalizedKey) == normalizedKey);
        addImpl(globalObject, owner, storage, normalizedKey, value, [&]() ALWAYS_INLINE_LAMBDA {
            return find(globalObject, storage, normalizedKey, hash);
        });
    }

    ALWAYS_INLINE static void shrinkIfNeeded(JSGlobalObject* globalObject, HashTable* owner, Storage& base)
    {
        VM& vm = globalObject->vm();
        // We're transitioning between states here, if a termination comes in we could leave the storage
        // in an inconsistent state. It's much easier to pause termination until the storage is updated.
        DeferTerminationForAWhile noTermination(vm);
        auto scope = DECLARE_THROW_SCOPE(vm);
        ASSERT(!isObsolete(base));
        TableSize capacity = Helper::capacity(base);
        TableSize aliveEntryCount = Helper::aliveEntryCount(base);
        if (aliveEntryCount >= (capacity >> 3))
            return;
        if (capacity == InitialCapacity)
            return;

        Storage* newBuffer = rehash(globalObject, base, capacity / 2);
        RETURN_IF_EXCEPTION(scope, void());

        setNextTable(vm, base, newBuffer);
        owner->m_storage.set(vm, owner, newBuffer);
    }
    template<typename FindKeyFunctor>
    ALWAYS_INLINE static bool removeImpl(JSGlobalObject* globalObject, HashTable* owner, Storage& storage, const FindKeyFunctor& findKeyFunctor)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        ASSERT(!isObsolete(storage));

        auto result = findKeyFunctor();
        RETURN_IF_EXCEPTION(scope, false);

        if (!isValidTableIndex(result.entryKeyIndex))
            return false;

        deleteData(vm, storage, result.entryKeyIndex);
        if constexpr (Traits::hasValueData)
            deleteData(vm, storage, result.entryKeyIndex + 1);
        incrementDeletedEntryCount(storage);
        decrementAliveEntryCount(storage);

        scope.release();
        shrinkIfNeeded(globalObject, owner, storage);
        return true;
    }
    ALWAYS_INLINE static bool remove(JSGlobalObject* globalObject, HashTable* owner, Storage& storage, JSValue key)
    {
        return removeImpl(globalObject, owner, storage, [&]() ALWAYS_INLINE_LAMBDA {
            return find(globalObject, storage, key);
        });
    }
    ALWAYS_INLINE static bool removeNormalized(JSGlobalObject* globalObject, HashTable* owner, Storage& storage, JSValue normalizedKey, TableSize hash)
    {
        ASSERT(normalizeMapKey(normalizedKey) == normalizedKey);
        return removeImpl(globalObject, owner, storage, [&]() ALWAYS_INLINE_LAMBDA {
            return find(globalObject, storage, normalizedKey, hash);
        });
    }

    template<typename Functor>
    ALWAYS_INLINE static Storage& transit(Storage& storage, const Functor& functor)
    {
        Storage* ptr = &storage;
        while (isObsolete(*ptr)) {
            functor(*ptr);
            ptr = nextTable(*ptr);
        }
        return *ptr;
    }
    struct TransitionResult {
        Storage* storage;
        Entry entry;
        JSValue key;
        JSValue value;
    };
    // `from` is an entry of `obsolete`. Moves it to the same entry of the next
    // table: a clear starts the next table empty, and a rehash drops the deleted
    // entries before it.
    ALWAYS_INLINE static void adjustEntryForNextTable(Storage& obsolete, Entry& from)
    {
        if (!from)
            return;

        if (isCleared(obsolete)) {
            from = 0;
            return;
        }

        TableIndex deletedEntryCount = Helper::deletedEntryCount(obsolete);
        if (!deletedEntryCount)
            return;

        TableIndex start = deletedEntriesStartIndex();
        TableIndex end = start + deletedEntryCount;
        Entry fromCopy = from;
        for (TableIndex i = start; i < end; ++i) {
            Entry deletedEntry = toNumber(get(obsolete, i));
            if (deletedEntry >= fromCopy)
                break;
            --from;
        }
    }
    ALWAYS_INLINE static TransitionResult nextLiveEntry(VM& vm, Storage& candidate, Entry from)
    {
        ASSERT(!isObsolete(candidate));
        TableSize capacity = Helper::capacity(candidate);
        TableIndex entryKeyIndex = entryDataStartIndex(dataTableStartIndex(capacity), from);
        ASSERT_UNUSED(capacity, from <= dataCapacity(capacity));
        for (Entry entry = from;; ++entry, entryKeyIndex += EntrySize) {
            JSValue key = get(candidate, entryKeyIndex);
            if (!key)
                return { };

            if (isDeleted(vm, key))
                continue;

            JSValue value;
            if constexpr (Traits::hasValueData)
                value = get(candidate, entryKeyIndex + 1);
            return { &candidate, entry, key, value };
        }
    }
    ALWAYS_INLINE static TransitionResult transitAndNext(VM& vm, Storage& storage, Entry from)
    {
        if (vm.gilOff()) [[unlikely]] {
            // A table that is still current is read under its lock. An obsolete
            // table is never written again, so its header can be read after
            // the lock is dropped.
            Storage* current = &storage;
            for (;;) {
                Storage* next;
                {
                    Locker locker { current->cellLock() };
                    if (!isObsolete(*current))
                        return nextLiveEntry(vm, *current, from);
                    next = nextTable(*current);
                }
                adjustEntryForNextTable(*current, from);
                current = next;
            }
        }

        Storage& candidate = transit(storage, [&](Storage& obsolete) ALWAYS_INLINE_LAMBDA {
            adjustEntryForNextTable(obsolete, from);
        });
        return nextLiveEntry(vm, candidate, from);
    }

    ALWAYS_INLINE static JSValue getKey(Storage& storage, Entry entry)
    {
        ASSERT(!isObsolete(storage));
        return get(storage, entryDataStartIndex(dataTableStartIndex(capacity(storage)), entry));
    }
    ALWAYS_INLINE static JSValue getValue(Storage& storage, Entry entry)
    {
        ASSERT(!isObsolete(storage) && EntrySize == 3);
        return get(storage, entryDataStartIndex(dataTableStartIndex(capacity(storage)), entry) + 1);
    }
    ALWAYS_INLINE static JSCell* nextAndUpdateIterationEntry(VM& vm, Storage& storage, Entry entry)
    {
        auto result = transitAndNext(vm, storage, entry);
        if (!result.storage)
            return vm.orderedHashTableSentinel();
        if (vm.gilOff()) [[unlikely]] {
            VMLite& lite = VMLite::current();
            lite.orderedHashTableIterationEntry = result.entry;
            lite.orderedHashTableIterationKey = result.key;
            lite.orderedHashTableIterationValue = result.value;
            return result.storage;
        }
        set(*result.storage, iterationEntryIndex(), result.entry);
        return result.storage;
    }
    ALWAYS_INLINE static JSValue getIterationEntry(Storage& storage) { return toJSValue(iterationEntry(storage)); }
    ALWAYS_INLINE static JSValue getIterationEntryKey(Storage& storage)
    {
        if (VMLite* lite = iterationLiteIfGILOff()) [[unlikely]]
            return lite->orderedHashTableIterationKey;
        return getKey(storage, iterationEntry(storage));
    }
    ALWAYS_INLINE static JSValue getIterationEntryValue(Storage& storage)
    {
        if (VMLite* lite = iterationLiteIfGILOff()) [[unlikely]]
            return lite->orderedHashTableIterationValue;
        return getValue(storage, iterationEntry(storage));
    }

    // With the GIL off, the entry that nextAndUpdateIterationEntry found is
    // kept per thread, not in the table: the table is shared by every thread
    // that iterates it, and another thread can rehash it, which overwrites the
    // header that getKey and getValue read. Each caller reads the entry on the
    // thread that found it, before any other iteration on that thread.
    ALWAYS_INLINE static VMLite* iterationLiteIfGILOff()
    {
        if (!g_jscConfig.gilOffProcess) [[likely]]
            return nullptr;
        VMLite* lite = VMLite::currentIfExists();
        return lite && lite->gilOff ? lite : nullptr;
    }

    // ---- With the GIL off ----
    //
    // Several threads use one table. The owner's current table is read and
    // written only under that table's cell lock. Rehash and clear make a new
    // table, publish it as the old table's next table and as the owner's
    // current table, and never write the old table again, so an obsolete table
    // can be read without its lock. Nothing under the lock allocates, calls into
    // JS, or parks: the caller normalizes and hashes the key first, which also
    // resolves a rope key (a stored key was resolved when it was added), and a
    // new table is allocated before the lock is taken. The lock holder then
    // checks that the table it planned against is still current.

    // Calls func(table) with the owner's current table locked. The owner must
    // have a table.
    template<typename Func>
    ALWAYS_INLINE static decltype(auto) withCurrentTableLockedGILOff(HashTable* owner, const Func& func)
    {
        for (;;) {
            Storage* storage = owner->m_storage.get();
            ASSERT(storage);
            Locker locker { storage->cellLock() };
            if (isObsolete(*storage))
                continue;
            AssertNoGC assertNoGC;
            return func(*storage);
        }
    }

    // A thread that finds a table through its owner takes the table's lock
    // before it reads the table. So a new table is filled under its own lock,
    // and the lock orders the fill before every read of the published table.
    static void releaseFreshTableGILOff(Storage& fresh)
    {
        Locker locker { fresh.cellLock() };
    }

    // Returns the owner's table, creating it if the owner has none.
    static Storage* materializeGILOff(JSGlobalObject* globalObject, HashTable* owner)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (Storage* storage = owner->m_storage.get())
            return storage;
        Storage* fresh = tryCreate(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        releaseFreshTableGILOff(*fresh);
        WTF::storeStoreFence();
        Storage* expected = nullptr;
        if (Storage* winner = WTF::atomicCompareExchangeStrong(owner->m_storage.slot(), expected, fresh))
            return winner;
        vm.writeBarrier(owner, fresh);
        return fresh;
    }

    // Copies the live entries of `base` into the empty table `copy`. With
    // UpdateDeletedEntries::Yes, `base` is being retired by a rehash, and the
    // deleted entries are recorded in it for the iterators that still point at
    // it (see adjustEntryForNextTable).
    template<UpdateDeletedEntries update>
    static void fillTableGILOff(JSGlobalObject* globalObject, Storage& base, Storage& copy)
    {
        VM& vm = getVM(globalObject);
        TableSize baseCapacity = capacity(base);
        TableSize newCapacity = capacity(copy);
        ASSERT(!isObsolete(base));
        ASSERT(aliveEntryCount(base) <= dataCapacity(newCapacity));
        set(copy, aliveEntryCountIndex(), aliveEntryCount(base));

        TableIndex baseEntryKeyIndex = dataTableStartIndex(baseCapacity);
        TableIndex baseDeletedEntriesIndex = deletedEntriesStartIndex();
        TableIndex newEntryKeyIndex = dataTableStartIndex(newCapacity);
        TableIndex newHashTableStartIndex = hashTableStartIndex();
        TableIndex newBucketCount = bucketCount(newCapacity);

        for (Entry baseEntry = 0;; ++baseEntry, baseEntryKeyIndex += EntrySize) {
            JSValue baseKey = get(base, baseEntryKeyIndex);
            if (!baseKey)
                break;

            if (isDeleted(vm, baseKey)) {
                if constexpr (update == UpdateDeletedEntries::Yes)
                    set(base, baseDeletedEntriesIndex++, baseEntry);
                continue;
            }

            setKeyOrValueData(vm, copy, newEntryKeyIndex, baseKey);
            if constexpr (Traits::hasValueData)
                setKeyOrValueData(vm, copy, newEntryKeyIndex + 1, get(base, baseEntryKeyIndex + 1));

            TableSize hash = jsMapHashForAlreadyHashedValue(globalObject, vm, baseKey);
            addToChain(copy, bucketIndex(newHashTableStartIndex, newBucketCount, hash), newEntryKeyIndex);
            newEntryKeyIndex += EntrySize;
        }
    }

    // Replaces the owner's table `base` with a rehash into `newCapacity`. Does
    // nothing if `base` is no longer current, or no longer fits; the caller
    // then plans again against the current table.
    static void rehashGILOff(JSGlobalObject* globalObject, HashTable* owner, Storage& base, TableSize newCapacity)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        Storage* fresh = tryCreate(globalObject, 0, 0, newCapacity);
        RETURN_IF_EXCEPTION(scope, void());

        Locker locker { base.cellLock() };
        AssertNoGC assertNoGC;
        if (isObsolete(base) || owner->m_storage.get() != &base)
            return;
        if (aliveEntryCount(base) > dataCapacity(newCapacity))
            return;
        {
            Locker freshLocker { fresh->cellLock() };
            fillTableGILOff<UpdateDeletedEntries::Yes>(globalObject, base, *fresh);
        }
        WTF::storeStoreFence();
        setNextTable(vm, base, fresh);
        owner->m_storage.set(vm, owner, fresh);
    }

    // Adds an entry that find() did not find. The table has room.
    ALWAYS_INLINE static void insertGILOff(VM& vm, Storage& storage, JSValue normalizedKey, JSValue value, TableSize hash)
    {
        TableSize capacity = Helper::capacity(storage);
        Entry newEntry = usedCapacity(storage);
        ASSERT(newEntry < dataCapacity(capacity));
        TableIndex newEntryKeyIndex = entryDataStartIndex(dataTableStartIndex(capacity), newEntry);
        incrementAliveEntryCount(storage);
        addToChain(storage, bucketIndex(capacity, hash), newEntryKeyIndex);
        setKeyOrValueData(vm, storage, newEntryKeyIndex, normalizedKey);
        if constexpr (Traits::hasValueData)
            setKeyOrValueData(vm, storage, newEntryKeyIndex + 1, value);
    }

    static void addNormalizedGILOff(JSGlobalObject* globalObject, HashTable* owner, JSValue normalizedKey, JSValue value, TableSize hash)
    {
        VM& vm = getVM(globalObject);
        DeferTerminationForAWhile noTermination(vm);
        auto scope = DECLARE_THROW_SCOPE(vm);
        ASSERT(normalizeMapKey(normalizedKey) == normalizedKey);

        for (;;) {
            Storage* base = materializeGILOff(globalObject, owner);
            RETURN_IF_EXCEPTION(scope, void());

            TableSize newCapacity;
            {
                Locker locker { base->cellLock() };
                if (isObsolete(*base))
                    continue;
                AssertNoGC assertNoGC;

                auto result = find(globalObject, *base, normalizedKey, hash);
                if (isValidTableIndex(result.entryKeyIndex)) {
                    if constexpr (Traits::hasValueData)
                        setKeyOrValueData(vm, *base, result.entryKeyIndex + 1, value);
                    return;
                }

                // The same growth policy as expandIfNeeded.
                TableSize capacity = Helper::capacity(*base);
                TableSize dataCapacity = Helper::dataCapacity(capacity);
                TableSize deletedEntryCount = Helper::deletedEntryCount(*base);
                if (aliveEntryCount(*base) + deletedEntryCount < dataCapacity) {
                    insertGILOff(vm, *base, normalizedKey, value, hash);
                    return;
                }
                TableSize expansionFactor = capacity < LargeCapacity ? 4 : 2;
                newCapacity = Checked<TableSize>(capacity) * expansionFactor;
                if (deletedEntryCount >= (dataCapacity / 2))
                    newCapacity = capacity;
            }

            rehashGILOff(globalObject, owner, *base, newCapacity);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }

    static bool removeNormalizedGILOff(JSGlobalObject* globalObject, HashTable* owner, JSValue normalizedKey, TableSize hash)
    {
        VM& vm = getVM(globalObject);
        DeferTerminationForAWhile noTermination(vm);
        auto scope = DECLARE_THROW_SCOPE(vm);
        ASSERT(normalizeMapKey(normalizedKey) == normalizedKey);
        if (!owner->m_storage.get())
            return false;

        Storage* base = nullptr;
        TableSize shrinkTo = 0;
        bool removed = withCurrentTableLockedGILOff(owner, [&](Storage& storage) {
            auto result = find(globalObject, storage, normalizedKey, hash);
            if (!isValidTableIndex(result.entryKeyIndex))
                return false;
            deleteData(vm, storage, result.entryKeyIndex);
            if constexpr (Traits::hasValueData)
                deleteData(vm, storage, result.entryKeyIndex + 1);
            incrementDeletedEntryCount(storage);
            decrementAliveEntryCount(storage);

            // The same shrink policy as shrinkIfNeeded.
            TableSize capacity = Helper::capacity(storage);
            if (aliveEntryCount(storage) < (capacity >> 3) && capacity != InitialCapacity) {
                base = &storage;
                shrinkTo = capacity / 2;
            }
            return true;
        });

        // Shrinking only saves memory, so a shrink that loses a race is dropped.
        if (base) {
            rehashGILOff(globalObject, owner, *base, shrinkTo);
            RETURN_IF_EXCEPTION(scope, true);
        }
        return removed;
    }

    static void clearGILOff(JSGlobalObject* globalObject, HashTable* owner)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!owner->m_storage.get())
            return;
        Storage* fresh = tryCreate(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        releaseFreshTableGILOff(*fresh);
        WTF::storeStoreFence();
        withCurrentTableLockedGILOff(owner, [&](Storage& base) {
            setClearedTableSentinel(base);
            setNextTable(vm, base, fresh);
            owner->m_storage.set(vm, owner, fresh);
        });
    }

    // Returns a copy of the owner's table, or null if the owner has none.
    static Storage* copyGILOff(JSGlobalObject* globalObject, HashTable* owner)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        for (;;) {
            if (!owner->m_storage.get())
                return nullptr;
            TableSize plannedCapacity = withCurrentTableLockedGILOff(owner, [&](Storage& base) {
                return capacity(base);
            });
            Storage* fresh = tryCreate(globalObject, 0, 0, plannedCapacity);
            RETURN_IF_EXCEPTION(scope, nullptr);
            bool filled = withCurrentTableLockedGILOff(owner, [&](Storage& base) {
                if (capacity(base) != plannedCapacity)
                    return false;
                fillTableGILOff<UpdateDeletedEntries::No>(globalObject, base, *fresh);
                return true;
            });
            if (filled)
                return fresh;
        }
    }
};

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
