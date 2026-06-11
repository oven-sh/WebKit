/*
 *  Copyright (C) 2004-2022 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include "JSExportMacros.h"
#include "Options.h"
#include "PropertyOffset.h"
#include "Structure.h"
#include "VM.h"
#include "WriteBarrier.h"
#include <wtf/Atomics.h>
#include <wtf/HashTable.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>


#define DUMP_PROPERTYMAP_STATS 0
#define DUMP_PROPERTYMAP_COLLISIONS 0

#define PROPERTY_MAP_DELETED_ENTRY_KEY ((UniquedStringImpl*)1)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class Heap;

// SPEC-objectmodel §6 (Task 9): per-server-heap butterfly quarantine epochs
// (ButterflyQuarantineEpochs - Lock + stable map Heap* -> Atomic<uint64_t>,
// defined in runtime/ConcurrentButterfly.cpp). The epoch of a heap is bumped
// by a Heap::addStopTheWorldSafepointHook adapter (§10 manifest entry 4c)
// once per collection of THAT heap, legacy and shared protocols alike, while
// the world is stopped. A quarantined deleted out-of-line slot stamped with
// epoch E becomes reusable only once the owning heap's epoch exceeds E: a
// crossed epoch proves every mutator passed a safepoint after the deletion,
// so no racing reader still holds a stale offset/slot pointer into the slot
// (I18, relying on I34's no-poll rule).
//
// butterflyQuarantineEpochSlot() get-or-creates the heap's counter; the
// returned Atomic's ADDRESS is stable for the process lifetime (entries are
// never removed), so PropertyTable caches it at first quarantine and reads it
// lock-free thereafter. registerButterflyQuarantineEpochHook() idempotently
// registers the bump adapter for a heap; the integrator wires it into VM/Heap
// init (manifest entry 4c, see docs/threads/INTEGRATE-objectmodel.md) BEFORE
// a second client can attach. If the hook is not (yet) registered the epoch
// never advances and quarantined slots are simply never reused - safe.
JS_EXPORT_PRIVATE WTF::Atomic<uint64_t>& butterflyQuarantineEpochSlot(JSC::Heap&);
JS_EXPORT_PRIVATE void registerButterflyQuarantineEpochHook(JSC::Heap&); // THREADS-INTEGRATE(objectmodel): called from VM init (manifest entry 4c)

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(PropertyTable);

#if DUMP_PROPERTYMAP_STATS

struct PropertyTableStats {
    std::atomic<unsigned> numFinds;
    std::atomic<unsigned> numCollisions;
    std::atomic<unsigned> numLookups;
    std::atomic<unsigned> numLookupProbing;
    std::atomic<unsigned> numAdds;
    std::atomic<unsigned> numRemoves;
    std::atomic<unsigned> numRehashes;
    std::atomic<unsigned> numReinserts;
};

JS_EXPORT_PRIVATE extern PropertyTableStats* propertyTableStats;

#endif

// compact <-> non-compact PropertyTable
// We need to maintain two things, one is PropertyOffset and one is unsigned index in index buffer of PropertyTable.
// But both are typically small. It is possible that we can get optimized table if both are fit in uint8_t, that's
// compact PropertyTable.
//
// PropertyOffset can be offseted with firstOutOfLineOffset since we can get out-of-line property easily, but this
// offset is small enough (64 currently), so that we can still assume that most of property offsets are < 256.
//
// 1. If property offset gets larger than 255, then we get non-compact PropertyTable. It requires at least 191 (255 - 64) properties.
//    In that case, PropertyTable size should be 256 since it is power-of-two.
// 2. If index gets larger than 255, then we get non-compact PropertyTable. But we are using 0 and 255 for markers. Thus, if we get 253
//    used counts, then we need to change the table.
//
// So, typical scenario is that, once 128th property is added, then we extend the table via rehashing. At that time, we change the
// table from compact to non-compact mode.
//
//  index-size  table-capacity    compact   v.s. non-compact
//     16             8              80              192
//     32            16             160              384
//     64            32             320              768
//    128            64             640             1536
//    256           128            1280             3072
//    512           256             N/A             6144     // After 512 size, compact PropertyTable does not work. All table gets non-compact.

class PropertyTable final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    template<typename CellType, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.propertyTableSpace();
    }

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);
    DECLARE_VISIT_CHILDREN;

    DECLARE_EXPORT_INFO;

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    using KeyType = UniquedStringImpl*;
    using ValueType = PropertyTableEntry;

    // Constructor is passed an initial capacity, a PropertyTable to copy, or both.
    static PropertyTable* create(VM&, unsigned initialCapacity);
    static PropertyTable* clone(VM&, const PropertyTable&);
    static PropertyTable* clone(VM&, unsigned initialCapacity, const PropertyTable&);
    ~PropertyTable();

    // Find a value in the table.
    std::tuple<PropertyOffset, unsigned> get(const KeyType&);
    // Add a value to the table
    [[nodiscard]] std::tuple<PropertyOffset, unsigned, bool> add(VM&, const ValueType& entry);
    // Remove a value from the table.
    std::tuple<PropertyOffset, unsigned> take(VM&, const KeyType&);
    PropertyOffset updateAttributeIfExists(const KeyType&, unsigned attributes);

    PropertyOffset renumberPropertyOffsets(JSObject*, unsigned inlineCapacity, Vector<JSValue>&);

    struct FindResult {
        unsigned entryIndex;
        unsigned index;
        PropertyOffset offset;
        unsigned attributes;
    };

    FindResult find(const KeyType&);
    std::tuple<PropertyOffset, unsigned, bool> addAfterFind(VM&, const ValueType& entry, FindResult&&);

    void seal();
    void freeze();

    bool isSealed() const;
    bool isFrozen() const;

    // Returns the number of values in the hashtable.
    unsigned size() const;

    // Checks if there are any values in the hashtable.
    bool isEmpty() const;

    // Number of slots in the property storage array in use, included deletedOffsets.
    unsigned propertyStorageSize() const;

    // Used to maintain a list of unused entries in the property storage.
    //
    // SPEC-objectmodel §6 (Task 9, flag-on ONLY - flag-off is today's single
    // Reusable list, I22): m_deletedOffsets is split into
    //   - Reusable (m_deletedOffsets, today's member): offsets takeDeletedOffset()
    //     may hand out. Flag-on it is fed SOLELY by epoch promotion
    //     (releaseQuarantinedSlots) - plus inline offsets, which are never
    //     quarantined (manifest entry 7b: inline slots live in the cell, are
    //     read/written as one atomic EncodedJSValue, and are outside the
    //     butterfly-offset aliasing hazard I18 guards against).
    //   - Quarantined (m_quarantinedDeletedOffsets): EVERY deleted out-of-line
    //     offset, stamped with the owning heap's ButterflyQuarantineEpochs value
    //     at deletion - dictionary-mode deletes AND non-dictionary
    //     removePropertyTransition, NO bypass (§6 eligibility is TOTAL).
    // takeDeletedOffset() draws ONLY from Reusable; hasDeletedOffset() lazily
    // promotes stamps < the owning heap's current epoch first (I18).
    //
    // Lock context (§6, r14): these are reached from Structure::add via
    // nextOffset (StructureInlines.h) and from Structure::remove - the
    // surrounding table mutation holds that Structure's m_lock, or the table is
    // still private to its creating thread (L6: stolen/cloned/materialized
    // tables are private until their new Structure publishes). Promotion and
    // the quarantine-epoch registry lock are leaves under it (heap §6 ranking);
    // nothing here allocates in the GC heap (O1: Vector storage is fastMalloc).
    void clearDeletedOffsets();
    bool hasDeletedOffset();
    PropertyOffset takeDeletedOffset();
    void addDeletedOffset(PropertyOffset);

    // S6 L3/L4 in-place-edit stamp (see m_concurrentEditCount).
    // PROTOCOL (§3.25, re-derived after the wave-5 review): writers bump the
    // stamp AFTER the in-place mutation completes, still under the table's
    // serialization (cell lock). A lock-free reader snapshots the stamp, does
    // its lock-free reads, then ACQUIRES THE LOCK and rechecks the stamp:
    //   - any edit that completed in the window bumped after its mutation,
    //     so the recheck differs and the reader RESTARTs;
    //   - an edit still in progress at recheck time is impossible (the
    //     reader holds the lock the writer mutates under).
    // A pre-mutation-only bump would be a one-sided seqlock: a snapshot
    // landing after the bump but mid-mutation would validate torn reads.
    // The recheck MUST happen under the lock; there is no odd/in-progress
    // marker, so a lock-free recheck is NOT sound.
    uint32_t concurrentEditCount() const { return m_concurrentEditCount.load(std::memory_order_acquire); }
    ALWAYS_INLINE void bumpConcurrentEditCount()
    {
        if (Options::useJSThreads()) [[unlikely]]
            m_concurrentEditCount.store(m_concurrentEditCount.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    // SPEC-objectmodel §9.4 (frozen name): move every quarantined offset whose
    // stamp is < currentEpoch onto the Reusable list. Caller holds the table's
    // serialization context (see the lock-context note above).
    void releaseQuarantinedSlots(uint64_t currentEpoch);

    struct QuarantinedDeletedOffset {
        PropertyOffset offset;
        uint64_t epoch; // Owning heap's ButterflyQuarantineEpochs stamp at deletion (§6).
    };

    // TSAN family 25 quarantine counters: relaxed mirror read - lock-free
    // count readers must not dereference the Vector (see the member comment).
    unsigned quarantinedDeletedOffsetCount() const { return concurrentRelaxedLoad(m_quarantinedDeletedOffsetCount); }
    bool quarantinedDeletedOffsetsContains(PropertyOffset) const; // debug/assert helper

    PropertyOffset nextOffset(PropertyOffset inlineCapacity);

    // Copy this PropertyTable, ensuring the copy has at least the capacity provided.
    PropertyTable* copy(VM&, unsigned newCapacity);

#ifndef NDEBUG
    size_t sizeInMemory();
    void checkConsistency();
#endif

    template<typename Functor>
    void forEachProperty(const Functor&) const;

    static constexpr unsigned EmptyEntryIndex = 0;

private:
    PropertyTable(VM&, unsigned initialCapacity);
    PropertyTable(VM&, const PropertyTable&);
    PropertyTable(VM&, unsigned initialCapacity, const PropertyTable&);

    PropertyTable(const PropertyTable&);

    void finishCreation(VM&);

    // Used to insert a value known not to be in the table, and where we know capacity to be available.
    template<typename Index, typename Entry>
    void reinsert(Index*, Entry*, const ValueType& entry);

    static bool canFitInCompact(const ValueType& entry) { return entry.offset() <= UINT8_MAX; }

    // Rehash the table. Used to grow, or to recover deleted slots.
    void rehash(VM&, unsigned newCapacity, bool canStayCompact);

    // The capacity of the table of values is half of the size of the index.
    unsigned tableCapacity() const;

    // We keep an extra deleted slot after the array to make iteration work,
    // and to use for deleted values. Index values into the array are 1-based,
    // so this is tableCapacity() + 1.
    // For example, if m_tableSize is 16, then tableCapacity() is 8 - but the
    // values array is actually 9 long (the 9th used for the deleted value/
    // iteration guard). The 8 valid entries are numbered 1..8, so the
    // deleted index is 9 (0 being reserved for empty).
    unsigned deletedEntryIndex() const;

    // Used in iterator creation/progression.
    template<typename T>
    static T* skipDeletedEntries(T* valuePtr, T* endValuePtr);

    // total number of  used entries in the values array - by either valid entries, or deleted ones.
    unsigned usedCount() const;

    // The size in bytes of data needed for by the table.
    size_t dataSize(bool isCompact);
    static size_t dataSize(bool isCompact, unsigned indexSize);

    // Calculates the appropriate table size (rounds up to a power of two).
    static unsigned sizeForCapacity(unsigned capacity);

    // Check if capacity is available.
    bool canInsert(const ValueType&);

    void remove(VM&, KeyType, unsigned entryIndex, unsigned index);

    template<typename Index, typename Entry>
    ALWAYS_INLINE FindResult findImpl(const Index*, const Entry*, const KeyType&);

    bool isCompact() const { return indexVector() & isCompactFlag; }

    template<typename Functor>
    void forEachPropertyMutable(const Functor&);

    // The table of values lies after the hash index.
    static CompactPropertyTableEntry* tableFromIndexVector(uint8_t* index, unsigned indexSize)
    {
        return std::bit_cast<CompactPropertyTableEntry*>(index + indexSize);
    }
    static const CompactPropertyTableEntry* tableFromIndexVector(const uint8_t* index, unsigned indexSize)
    {
        return std::bit_cast<const CompactPropertyTableEntry*>(index + indexSize);
    }
    static PropertyTableEntry* tableFromIndexVector(uint32_t* index, unsigned indexSize)
    {
        return std::bit_cast<PropertyTableEntry*>(index + indexSize);
    }
    static const PropertyTableEntry* tableFromIndexVector(const uint32_t* index, unsigned indexSize)
    {
        return std::bit_cast<const PropertyTableEntry*>(index + indexSize);
    }

    CompactPropertyTableEntry* tableFromIndexVector(uint8_t* index) { return tableFromIndexVector(index, indexSize()); }
    const CompactPropertyTableEntry* tableFromIndexVector(const uint8_t* index) const { return tableFromIndexVector(index, indexSize()); }
    PropertyTableEntry* tableFromIndexVector(uint32_t* index) { return tableFromIndexVector(index, indexSize()); }
    const PropertyTableEntry* tableFromIndexVector(const uint32_t* index) const { return tableFromIndexVector(index, indexSize()); }

    CompactPropertyTableEntry* tableEndFromIndexVector(uint8_t* index)
    {
        return tableFromIndexVector(index) + usedCount();
    }
    const CompactPropertyTableEntry* tableEndFromIndexVector(const uint8_t* index) const
    {
        return tableFromIndexVector(index) + usedCount();
    }
    PropertyTableEntry* tableEndFromIndexVector(uint32_t* index)
    {
        return tableFromIndexVector(index) + usedCount();
    }
    const PropertyTableEntry* tableEndFromIndexVector(const uint32_t* index) const
    {
        return tableFromIndexVector(index) + usedCount();
    }

    static uintptr_t allocateIndexVector(bool isCompact, unsigned indexSize);
    static uintptr_t allocateZeroedIndexVector(bool isCompact, unsigned indexSize);
    static void destroyIndexVector(uintptr_t indexVector);

    template<typename Func>
    static ALWAYS_INLINE auto withIndexVector(uintptr_t indexVector, Func&& function) -> decltype(auto)
    {
        if (indexVector & isCompactFlag)
            return function(std::bit_cast<uint8_t*>(indexVector & indexVectorMask));
        return function(std::bit_cast<uint32_t*>(indexVector & indexVectorMask));
    }

    template<typename Func>
    ALWAYS_INLINE auto withIndexVector(Func&& function) const -> decltype(auto)
    {
        return withIndexVector(indexVector(), std::forward<Func>(function));
    }

    static constexpr uintptr_t isCompactFlag = 0x1;
    static constexpr uintptr_t indexVectorMask = ~isCompactFlag;

    // SPEC-objectmodel §6 (Task 9): out-of-line .cpp slow path of
    // addDeletedOffset's flag-on quarantine leg - stamps the owning heap's
    // current epoch and caches the heap's epoch slot on first use.
    void quarantineDeletedOffset(PropertyOffset);

    // TSAN family 25 (TSAN-TRIAGE.md §3.25; OM §6/L6): the table's counted /
    // index header words below are MUTATED only under the table's
    // serialization (the owning Structure's m_lock, or the table is still
    // private to its creating thread, L6), but are READ lock-free by
    // concurrent readers that re-validate (compiler-thread size() /
    // forEachProperty re-validation reads, e.g. Structure.cpp's
    // table->size() != values.size() check and forEachPropertyConcurrently).
    // Mixed plain access is UB, so both sides go through relaxed atomics:
    // identical plain-mov codegen on every supported target, flag-off
    // behavior and semantics unchanged. Mutation stays locked - these are
    // NOT lock-free writers; relaxed load+store (not RMW) is sufficient
    // because every writer already holds the serialization. Same pattern and
    // rationale as Structure::concurrentRelaxedLoad/Store (§9.1).
    template<typename T>
    static ALWAYS_INLINE T concurrentRelaxedLoad(const T& field)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        T result;
        __atomic_load(const_cast<T*>(&field), &result, __ATOMIC_RELAXED);
        return result;
    }

    template<typename T>
    static ALWAYS_INLINE void concurrentRelaxedStore(T& field, std::type_identity_t<T> value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        __atomic_store(&field, &value, __ATOMIC_RELAXED);
    }

    ALWAYS_INLINE unsigned indexSize() const { return concurrentRelaxedLoad(m_indexSize); }
    ALWAYS_INLINE unsigned indexMask() const { return concurrentRelaxedLoad(m_indexMask); }
    ALWAYS_INLINE uintptr_t indexVector() const { return concurrentRelaxedLoad(m_indexVector); }
    ALWAYS_INLINE unsigned keyCount() const { return concurrentRelaxedLoad(m_keyCount); }
    ALWAYS_INLINE unsigned deletedCount() const { return concurrentRelaxedLoad(m_deletedCount); }
    // Relaxed mirror of m_deletedOffsets->size(): lets lock-free count
    // readers (propertyStorageSize) avoid dereferencing the unique_ptr /
    // Vector internals, which a locked mutator may be reallocating.
    ALWAYS_INLINE unsigned deletedOffsetCount() const { return concurrentRelaxedLoad(m_deletedOffsetCount); }

    unsigned m_indexSize;
    unsigned m_indexMask;
    uintptr_t m_indexVector;
    unsigned m_keyCount;
    unsigned m_deletedCount;
    // SPEC-objectmodel S6 L3/L4 (flag-on only; flag-off never read): monotonic
    // count of in-place edits (adds/takes/attribute updates), bumped under the
    // serialization the edit already holds (cell lock + m_lock). Consumed by
    // deletePropertyNamedConcurrent's cacheable-dictionary leg: a dictionary's
    // structureID never changes on in-place adds, so a remove-transition
    // planned from a table CLONE must re-validate THIS count under the cell
    // lock before publishing - otherwise every add between clone and
    // publication is silently orphaned (lost adds, I21/I37).
    std::atomic<uint32_t> m_concurrentEditCount { 0 };
    std::unique_ptr<Vector<PropertyOffset>> m_deletedOffsets; // §6 Reusable list flag-on; the only list flag-off (I22).
    std::unique_ptr<Vector<QuarantinedDeletedOffset>> m_quarantinedDeletedOffsets; // §6 Quarantined list; flag-on only.
    // TSAN family 25 quarantine counters (OM §6): relaxed mirrors of the two
    // deleted-offset Vector sizes, updated (relaxed store) at every list
    // mutation - which always holds the table's serialization - and read
    // (relaxed load) by lock-free count readers. The Vectors themselves are
    // only ever dereferenced under the serialization.
    unsigned m_deletedOffsetCount { 0 };
    unsigned m_quarantinedDeletedOffsetCount { 0 };
    WTF::Atomic<uint64_t>* m_quarantineEpochSlot { nullptr }; // Cached owning-heap slot (stable address); set at first quarantine, read via relaxed load.

    static constexpr unsigned MinimumTableSize = 16;
    static_assert(MinimumTableSize >= 16, "compact index is uint8_t and we should keep 16 byte aligned entries after this array");
};

template<typename Index, typename Entry>
PropertyTable::FindResult PropertyTable::findImpl(const Index* indexVector, const Entry* table, const KeyType& key)
{
    unsigned hash = IdentifierRepHash::hash(key);
    unsigned indexMask = this->indexMask();
    unsigned probeCount = 0;
    unsigned index = hash & indexMask;

#if DUMP_PROPERTYMAP_STATS
    ++propertyTableStats->numFinds;
#endif

    while (true) {
        unsigned entryIndex = indexVector[index];
        if (entryIndex == EmptyEntryIndex)
            return FindResult { entryIndex, index, invalidOffset, 0 };
        const auto& entry = table[entryIndex - 1];
        if (key == entry.key()) {
            ASSERT(!m_deletedOffsets || !m_deletedOffsets->contains(entry.offset()));
            ASSERT(!quarantinedDeletedOffsetsContains(entry.offset())); // §6: quarantined slots are never live entries (I18).
            return FindResult { entryIndex, index, entry.offset(), entry.attributes() };
        }

#if DUMP_PROPERTYMAP_STATS
        ++propertyTableStats->numCollisions;
#endif

#if DUMP_PROPERTYMAP_COLLISIONS
        dataLog("PropertyTable collision for ", key, " (", hash, ")\n");
        dataLog("Collided with ", entry.key(), "(", IdentifierRepHash::hash(entry.key()), ")\n");
#endif

        ++probeCount;
        index = (index + probeCount) & indexMask;
    }
}

inline PropertyTable::FindResult PropertyTable::find(const KeyType& key)
{
    ASSERT(key);
    ASSERT(key->isAtom() || key->isSymbol());
    return withIndexVector([&](auto* vector) {
        return findImpl(vector, tableFromIndexVector(vector), key);
    });
}

inline std::tuple<PropertyOffset, unsigned> PropertyTable::get(const KeyType& key)
{
    ASSERT(key);
    ASSERT(key->isAtom() || key->isSymbol());
    ASSERT(key != PROPERTY_MAP_DELETED_ENTRY_KEY);

    if (!keyCount())
        return std::tuple { invalidOffset, 0 };

    FindResult result = find(key);
    return std::tuple { result.offset, result.attributes };
}

[[nodiscard]] inline std::tuple<PropertyOffset, unsigned, bool> PropertyTable::add(VM& vm, const ValueType& entry)
{
    ASSERT(!m_deletedOffsets || !m_deletedOffsets->contains(entry.offset()));
    ASSERT(!quarantinedDeletedOffsetsContains(entry.offset())); // §6/I18: a quarantined offset must not be re-added before promotion.

    // Look for a value with a matching key already in the array.
    FindResult result = find(entry.key());
    if (result.offset != invalidOffset)
        return std::tuple { result.offset, result.attributes, false };
    return addAfterFind(vm, entry, WTF::move(result));
}

ALWAYS_INLINE std::tuple<PropertyOffset, unsigned, bool> PropertyTable::addAfterFind(VM& vm, const ValueType& entry, FindResult&& result)
{
#if DUMP_PROPERTYMAP_STATS
    ++propertyTableStats->numAdds;
#endif

    // Ref the key
    entry.key()->ref();

    // ensure capacity is available.
    if (!canInsert(entry)) {
        rehash(vm, keyCount() + 1, canFitInCompact(entry));
        result = find(entry.key());
        ASSERT(result.offset == invalidOffset);
        ASSERT(result.entryIndex == EmptyEntryIndex);
    }

    // Allocate a slot in the hashtable, and set the index to reference this.
    ASSERT(!isCompact() || usedCount() < UINT8_MAX);
    unsigned index = result.index;
    unsigned entryIndex = usedCount() + 1;
    withIndexVector([&](auto* vector) {
        vector[index] = entryIndex;
        tableFromIndexVector(vector)[entryIndex - 1] = entry;
    });

    // TSAN family 25: in-place insert under the table's serialization vs
    // lock-free concurrent size() readers - relaxed store (§3.25).
    concurrentRelaxedStore(m_keyCount, keyCount() + 1);

    bumpConcurrentEditCount(); // S6 L3/L4: in-place insert — bump AFTER the mutation (see protocol comment).

    return std::tuple { entry.offset(), entry.attributes(), true };
}

inline void PropertyTable::remove(VM& vm, KeyType key, unsigned entryIndex, unsigned index)
{
#if DUMP_PROPERTYMAP_STATS
    ++propertyTableStats->numRemoves;
#endif

    // Replace this one element with the deleted sentinel. Also clear out
    // the entry so we can iterate all the entries as needed.
    withIndexVector([&](auto* vector) {
        vector[index] = deletedEntryIndex();
        tableFromIndexVector(vector)[entryIndex - 1].setKey(PROPERTY_MAP_DELETED_ENTRY_KEY);
    });
    key->deref();

    // TSAN family 25: in-place removal under the table's serialization vs
    // lock-free concurrent size() readers - relaxed stores (§3.25).
    ASSERT(keyCount() >= 1);
    concurrentRelaxedStore(m_keyCount, keyCount() - 1);
    concurrentRelaxedStore(m_deletedCount, deletedCount() + 1);

    if (deletedCount() * 4 >= indexSize())
        rehash(vm, keyCount(), true);
}

inline std::tuple<PropertyOffset, unsigned> PropertyTable::take(VM& vm, const KeyType& key)
{
    FindResult result = find(key);
    if (result.offset != invalidOffset) {
        remove(vm, key, result.entryIndex, result.index);
        bumpConcurrentEditCount(); // S6 L3/L4: in-place removal — bump AFTER the mutation (see protocol comment).
    }
    return std::tuple { result.offset, result.attributes };
}

inline PropertyOffset PropertyTable::updateAttributeIfExists(const KeyType& key, unsigned attributes)
{
    return withIndexVector([&](auto* vector) -> PropertyOffset {
        auto* table = tableFromIndexVector(vector);
        FindResult result = findImpl(vector, table, key);
        if (result.offset == invalidOffset)
            return invalidOffset;
        table[result.entryIndex - 1].setAttributes(attributes);
        bumpConcurrentEditCount(); // S6 L3/L4: in-place attribute edit — bump AFTER the mutation (see protocol comment).
        return result.offset;
    });
}

// returns the number of values in the hashtable.
inline unsigned PropertyTable::size() const
{
    // Read lock-free by concurrent re-validating readers (§3.25).
    return keyCount();
}

inline bool PropertyTable::isEmpty() const
{
    return !keyCount();
}

inline unsigned PropertyTable::propertyStorageSize() const
{
    // Quarantined slots still occupy property storage: maxOffset/outOfLineSize
    // never shrink while a slot is quarantined (§6 D1/I30 - the slot stays
    // GC-visited and dereferenceable by tardy readers until released).
    // TSAN family 25 quarantine counters: both list sizes are read through
    // their relaxed mirrors - never dereference the Vectors lock-free.
    return size() + deletedOffsetCount() + quarantinedDeletedOffsetCount();
}

inline void PropertyTable::clearDeletedOffsets()
{
    // Sole caller is renumberPropertyOffsets (flattenDictionaryStructure):
    // storage is renumbered/compacted, so quarantined offsets die with the old
    // layout. Sound flag-on because F3 runs flattening of shared objects under
    // a per-event stop-the-world (every mutator passed a safepoint => no stale
    // reader holds a pre-flatten offset, I18/I34).
    m_deletedOffsets = nullptr;
    m_quarantinedDeletedOffsets = nullptr;
    concurrentRelaxedStore(m_deletedOffsetCount, 0u);
    concurrentRelaxedStore(m_quarantinedDeletedOffsetCount, 0u);
}

inline bool PropertyTable::hasDeletedOffset()
{
    if (Options::useJSThreads()) [[unlikely]] {
        // §6 lazy promotion (I18/I19): Reusable is fed SOLELY by epoch
        // promotion. The cached slot is read lock-free (stable address); the
        // surrounding table mutation already holds the Structure's m_lock or
        // owns the table privately (L6), which serializes the list edits.
        if (m_quarantinedDeletedOffsets && !m_quarantinedDeletedOffsets->isEmpty()) {
            WTF::Atomic<uint64_t>* epochSlot = concurrentRelaxedLoad(m_quarantineEpochSlot);
            ASSERT(epochSlot);
            releaseQuarantinedSlots(epochSlot->load(std::memory_order_acquire));
        }
    }
    return m_deletedOffsets && !m_deletedOffsets->isEmpty();
}

inline PropertyOffset PropertyTable::takeDeletedOffset()
{
    // §6/I18: draws ONLY from the Reusable list. Quarantined entries reach it
    // solely through releaseQuarantinedSlots() (hasDeletedOffset() promotes
    // lazily before this is reached via nextOffset()).
    PropertyOffset offset = m_deletedOffsets->takeLast();
    concurrentRelaxedStore(m_deletedOffsetCount, static_cast<unsigned>(m_deletedOffsets->size()));
    return offset;
}

inline void PropertyTable::addDeletedOffset(PropertyOffset offset)
{
    ASSERT(!m_deletedOffsets || !m_deletedOffsets->contains(offset));
    ASSERT(!quarantinedDeletedOffsetsContains(offset));
    if (Options::useJSThreads()) [[unlikely]] {
        // §6 eligibility is TOTAL: EVERY deleted offset - inline AND
        // out-of-line - is quarantined (dictionary-mode deletes AND
        // non-dictionary removePropertyTransition; NO bypass). Review round 2:
        // inline slots were previously exempt, but the tardy-access ALIASING
        // hazard (THREAD.md: a tardy read of deleted f must never alias a
        // newly added g) applies to inline slots identically - dictionary
        // structures mutate in place, so a stale reader's structure check
        // passes across delete(f)+add(g). Inline-slot atomicity only rules
        // out tearing, not aliasing. The D1 jsUndefined release-store and the
        // nextOffset() skip-past-quarantined accounting already handle inline
        // offsets.
        quarantineDeletedOffset(offset);
        return;
    }
    if (!m_deletedOffsets)
        m_deletedOffsets = makeUnique<Vector<PropertyOffset>>();
    m_deletedOffsets->append(offset);
    concurrentRelaxedStore(m_deletedOffsetCount, static_cast<unsigned>(m_deletedOffsets->size()));
}

inline bool PropertyTable::quarantinedDeletedOffsetsContains(PropertyOffset offset) const
{
    if (!m_quarantinedDeletedOffsets)
        return false;
    for (const auto& entry : *m_quarantinedDeletedOffsets) {
        if (entry.offset == offset)
            return true;
    }
    return false;
}

inline PropertyOffset PropertyTable::nextOffset(PropertyOffset inlineCapacity)
{
    if (hasDeletedOffset()) // Flag-on this also performs the §6 lazy promotion.
        return takeDeletedOffset();

    unsigned propertyNumber = size();
    if (Options::useJSThreads()) [[unlikely]] {
        // §6: quarantined slots still occupy storage numbers (the prefix
        // invariant is keyCount + deleted == allocated property numbers, and
        // the Reusable list is empty here), so fresh offsets are allocated
        // PAST them - live storage never shrinks while quarantined (I18/I30).
        propertyNumber += quarantinedDeletedOffsetCount();
    }
    return offsetForPropertyNumber(propertyNumber, inlineCapacity);
}

inline PropertyTable* PropertyTable::copy(VM& vm, unsigned newCapacity)
{
    ASSERT(newCapacity >= keyCount());

    // Fast case; if the new table will be the same m_indexSize as this one, we can memcpy it,
    // save rehashing all keys.
    if (sizeForCapacity(newCapacity) == indexSize())
        return PropertyTable::clone(vm, *this);
    return PropertyTable::clone(vm, newCapacity, *this);
}

#ifndef NDEBUG
inline size_t PropertyTable::sizeInMemory()
{
    size_t result = sizeof(PropertyTable) + dataSize(isCompact());
    if (m_deletedOffsets)
        result += (m_deletedOffsets->capacity() * sizeof(PropertyOffset));
    if (m_quarantinedDeletedOffsets)
        result += (m_quarantinedDeletedOffsets->capacity() * sizeof(QuarantinedDeletedOffset));
    return result;
}
#endif

template<typename Index, typename Entry>
inline void PropertyTable::reinsert(Index* indexVector, Entry* table, const ValueType& entry)
{
#if DUMP_PROPERTYMAP_STATS
    ++propertyTableStats->numReinserts;
#endif

    // Used to insert a value known not to be in the table, and where
    // we know capacity to be available.
    ASSERT(canInsert(entry));

    unsigned hash = IdentifierRepHash::hash(entry.key());
    unsigned indexMask = this->indexMask();
    unsigned probeCount = 0;
    unsigned index = hash & indexMask;

    // Reinsert must not conflict with the keys since all entries are existing ones.
    // Plus, there is no deleted entries too. We should just check emptyness, that's it.
    while (true) {
        unsigned entryIndex = indexVector[index];
        if (entryIndex == EmptyEntryIndex)
            break;
        ASSERT(table[entryIndex - 1].key() != entry.key());
        ++probeCount;
        index = (index + probeCount) & indexMask;
    }

    ASSERT(!isCompact() || usedCount() < UINT8_MAX);
    unsigned entryIndex = usedCount() + 1;
    indexVector[index] = entryIndex;
    table[entryIndex - 1] = entry;

    // Relaxed: reinsert runs during rehash of a published table (addAfterFind
    // slow path) while concurrent readers may load size() lock-free (§3.25).
    concurrentRelaxedStore(m_keyCount, keyCount() + 1);
}

inline void PropertyTable::rehash(VM& vm, unsigned newCapacity, bool canStayCompact)
{
#if DUMP_PROPERTYMAP_STATS
    ++propertyTableStats->numRehashes;
#endif

    uintptr_t oldIndexVector = indexVector();
    bool oldIsCompact = oldIndexVector & isCompactFlag;
    unsigned oldIndexSize = indexSize();
    unsigned oldUsedCount = usedCount();
    size_t oldDataSize = dataSize(oldIsCompact, oldIndexSize);

    // TSAN family 25 (§3.25): rehash mutates the index header words under the
    // table's serialization while concurrent readers may load them lock-free
    // and re-validate (L6). Relaxed stores keep the words tear-free per word;
    // cross-word consistency is the readers' re-validation problem by spec.
    concurrentRelaxedStore(m_indexSize, sizeForCapacity(newCapacity));
    concurrentRelaxedStore(m_indexMask, indexSize() - 1);
    concurrentRelaxedStore(m_keyCount, 0u);
    concurrentRelaxedStore(m_deletedCount, 0u);

    // Once table gets non-compact, we do not change it back to compact again.
    // This is because some of property offset can be larger than UINT8_MAX already.
    bool isCompact = canStayCompact && oldIsCompact && tableCapacity() < UINT8_MAX;
    concurrentRelaxedStore(m_indexVector, allocateZeroedIndexVector(isCompact, indexSize()));
    withIndexVector([&](auto* vector) {
        auto* table = tableFromIndexVector(vector);
        withIndexVector(oldIndexVector, [&](const auto* oldVector) {
            const auto* oldCursor = tableFromIndexVector(oldVector, oldIndexSize);
            const auto* oldEnd = oldCursor + oldUsedCount;
            for (; oldCursor != oldEnd; ++oldCursor) {
                if (oldCursor->key() == PROPERTY_MAP_DELETED_ENTRY_KEY)
                    continue;
                ASSERT(canInsert(*oldCursor));
                reinsert(vector, table, *oldCursor);
            }
        });
    });
    destroyIndexVector(oldIndexVector);

    size_t newDataSize = dataSize(this->isCompact());
    if (oldDataSize < newDataSize)
        vm.heap.reportExtraMemoryAllocated(this, newDataSize - oldDataSize);
}

inline unsigned PropertyTable::tableCapacity() const { return indexSize() >> 1; }

inline unsigned PropertyTable::deletedEntryIndex() const { return tableCapacity() + 1; }

template<typename T>
inline T* PropertyTable::skipDeletedEntries(T* valuePtr, T* endValuePtr)
{
    while (valuePtr < endValuePtr && valuePtr->key() == PROPERTY_MAP_DELETED_ENTRY_KEY)
        ++valuePtr;
    return valuePtr;
}

inline unsigned PropertyTable::usedCount() const
{
    // Total number of  used entries in the values array - by either valid entries, or deleted ones.
    // Read lock-free by concurrent iteration (forEachProperty end bound, §3.25).
    return keyCount() + deletedCount();
}

inline size_t PropertyTable::dataSize(bool isCompact, unsigned indexSize)
{
    if (isCompact)
        return indexSize * sizeof(uint8_t) + ((indexSize >> 1) + 1) * sizeof(CompactPropertyTableEntry);
    return indexSize * sizeof(uint32_t) + ((indexSize >> 1) + 1) * sizeof(PropertyTableEntry);
}

inline size_t PropertyTable::dataSize(bool isCompact)
{
    // The size in bytes of data needed for by the table.
    // Ensure that this function can be called concurrently.
    return dataSize(isCompact, indexSize());
}

ALWAYS_INLINE uintptr_t PropertyTable::allocateIndexVector(bool isCompact, unsigned indexSize)
{
    return std::bit_cast<uintptr_t>(PropertyTableMalloc::malloc(PropertyTable::dataSize(isCompact, indexSize))) | (isCompact ? isCompactFlag : 0);
}

ALWAYS_INLINE uintptr_t PropertyTable::allocateZeroedIndexVector(bool isCompact, unsigned indexSize)
{
    return std::bit_cast<uintptr_t>(PropertyTableMalloc::zeroedMalloc(PropertyTable::dataSize(isCompact, indexSize))) | (isCompact ? isCompactFlag : 0);
}

ALWAYS_INLINE void PropertyTable::destroyIndexVector(uintptr_t indexVector)
{
    PropertyTableMalloc::free(std::bit_cast<void*>(indexVector & indexVectorMask));
}

inline unsigned PropertyTable::sizeForCapacity(unsigned capacity)
{
    if (capacity < MinimumTableSize / 2)
        return MinimumTableSize;
    return roundUpToPowerOfTwo(capacity + 1) * 2;
}

inline bool PropertyTable::canInsert(const ValueType& entry)
{
    if (usedCount() >= tableCapacity())
        return false;
    if (!isCompact())
        return true;
    return canFitInCompact(entry);
}

template<typename Functor>
inline void PropertyTable::forEachProperty(const Functor& functor) const
{
    withIndexVector([&](const auto* vector) {
        const auto* cursor = tableFromIndexVector(vector);
        const auto* end = tableEndFromIndexVector(vector);
        for (; cursor != end; ++cursor) {
            if (cursor->key() == PROPERTY_MAP_DELETED_ENTRY_KEY)
                continue;
            if (functor(*cursor) == IterationStatus::Done)
                return;
        }
    });
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
