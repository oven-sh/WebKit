/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTNonMovingVector.h"
#include "AOTSchemaRecords.h"
#include "AOTSharedTypeMemo.h"
#include "AOTType.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <concepts>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/SegmentedVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

class ParallelLoops;
class Statistics;

namespace Typed {

class HashedIndex {
public:
    std::optional<uint32_t> find(uint32_t hash, NOESCAPE const Invocable<bool(uint32_t position)> auto& isEqual) const
    {
        if (m_slots.isEmpty())
            return std::nullopt;
        size_t mask = m_slots.size() - 1;
        for (size_t index = hash & mask; m_slots[index].positionPlusOne; index = (index + 1) & mask) {
            const Slot& slot = m_slots[index];
            if (slot.hash == hash && isEqual(slot.positionPlusOne - 1))
                return slot.positionPlusOne - 1;
        }
        return std::nullopt;
    }

    void add(uint32_t hash, uint32_t position);

    // Between reserve() and didAddConcurrently(), any number of threads may call addConcurrently() and find().
    void reserve(const ParallelLoops&, size_t numberOfAdditions);
    void addConcurrently(uint32_t hash, uint32_t position);
    void didAddConcurrently(size_t numberOfAdditions) { m_size += static_cast<unsigned>(numberOfAdditions); }

    void prefetch(uint32_t hash) const
    {
        if (!m_slots.isEmpty())
            __builtin_prefetch(&m_slots[hash & (m_slots.size() - 1)]);
    }

private:
    // Zero for a free slot.
    struct alignas(8) Slot {
        uint32_t hash;
        uint32_t positionPlusOne;
    };

    void insert(Slot);

    Vector<Slot> m_slots;
    unsigned m_size { 0 };
};

void add(Hasher&, const LayoutSet&);
void add(Hasher&, const CalleeSet&);
void add(Hasher&, const PropertyKey&);

template<typename Record, typename RecordID, size_t segmentSize, typename Storage = SegmentedVector<Record, segmentSize>>
class InternedRecords {
public:
    const Record& at(RecordID id) const { return m_records[id.index()]; }
    unsigned size() const { return m_records.size(); }

    std::optional<RecordID> find(const Record& record, uint32_t hash) const
    {
        auto position = m_index.find(hash, [&](uint32_t candidate) {
            return m_records[candidate] == record;
        });
        if (!position)
            return std::nullopt;
        return RecordID(*position);
    }

    void prefetch(uint32_t hash) const { m_index.prefetch(hash); }

    RecordID findOrAdd(Record&& record, uint32_t hash)
    {
        if (auto existing = find(record, hash))
            return *existing;
        RecordID id(m_records.size());
        m_records.append(WTF::move(record));
        m_index.add(hash, id.index());
        return id;
    }

private:
    template<typename, typename, typename> friend class OrderedInterning;

    Storage m_records;
    HashedIndex m_index;
};

class TypeInterningPhase;

class TypeTable {
    WTF_MAKE_NONCOPYABLE(TypeTable);
public:
    explicit TypeTable(const BuiltinTable&, const Program* = nullptr);
    ~TypeTable();

    static constexpr TypeID never { 0u };
    static constexpr TypeID undefined { 1u };
    static constexpr TypeID boolean { 2u };
    static constexpr TypeID int32 { 3u };
    static constexpr TypeID integer { 4u };
    static constexpr TypeID number { 5u };
    static constexpr TypeID string { 6u };
    static constexpr TypeID function { 7u };
    static constexpr TypeID object { 8u };
    static constexpr TypeID anything { 9u };
    static constexpr TypeID null { 10u };
    static constexpr TypeID nullish { 11u };
    static constexpr TypeID absent { 12u };
    static constexpr TypeID guard { 13u };
    static constexpr TypeID length { 14u };
    // `anything` is every value of the language. A location with a dead zone, and a slot that may be empty, holds this.
    static constexpr TypeID anythingOrAbsent { 15u };
    static constexpr TypeID valuesOf(TypeID held) { return held == anythingOrAbsent ? anything : held; }

    static constexpr uint32_t provisionalBit = 1u << 31;
    static constexpr uint32_t storageShift = 20;
    static constexpr uint32_t maxRecordsPerStorage = 1u << storageShift;
    static constexpr uint32_t maxStorages = 2047;

    template<typename Tag> static constexpr bool isProvisional(ID<Tag> id) { return id && id.index() & provisionalBit; }
    template<typename IDType> static constexpr IDType provisionalID(uint32_t storage, uint32_t position) { return IDType(provisionalBit | storage << storageShift | position); }
    template<typename Tag> static constexpr uint32_t storageNumberOf(ID<Tag> provisional) { return (provisional.index() & ~provisionalBit) >> storageShift; }
    template<typename Tag> static constexpr uint32_t positionOf(ID<Tag> provisional) { return provisional.index() & (maxRecordsPerStorage - 1); }

    const BuiltinTable& builtins() const { return m_builtins; }
    const Program* program() const { return m_program; }

    const Type& at(TypeID id) const { return isProvisional(id) ? provisionalAt(id) : m_types.at(id); }
    const LayoutSet& at(LayoutSetID id) const { return isProvisional(id) ? provisionalAt(id) : m_layoutSets.at(id); }
    const CalleeSet& at(CalleeSetID id) const { return isProvisional(id) ? provisionalAt(id) : m_calleeSets.at(id); }
    const PropertyKey& at(PropertyKeyID id) const { return isProvisional(id) ? provisionalAt(id) : m_propertyKeys.at(id); }

    unsigned size() const { return m_types.size(); }
    unsigned numberOfLayoutSets() const { return m_layoutSets.size(); }
    unsigned numberOfCalleeSets() const { return m_calleeSets.size(); }
    unsigned numberOfPropertyKeys() const { return m_propertyKeys.size(); }
    // Flow makes keys of texts that calls pass. No operation of the program spells such a key: that it is a key at all is something an analysis found.
    void didInternKeysThatOperationsSpell() { m_numberOfKeysThatOperationsSpell = m_propertyKeys.size(); }
    bool isSpelledByOperations(PropertyKeyID key) const { return key.index() < m_numberOfKeysThatOperationsSpell; }

    // What a read of the position is proven to give. Nothing checks what enters a position that is only expected to hold its type, and a location with a dead zone holds `absent` whatever else it holds.
    TypeID provenTypeOf(const DeclaredType&) const;

    std::optional<TypeID> find(const Type& canonical) const;
    std::optional<PropertyKeyID> find(const PropertyKey&) const;

    TypeID intern(const Type&);
    TypeID intern(Type&&);
    LayoutSetID intern(LayoutSet&&);
    CalleeSetID intern(CalleeSet&&);
    PropertyKeyID intern(const PropertyKey&);

    static uint32_t hashOfCanonical(const Type&);
    static uint32_t hashOfCanonical(const LayoutSet&);
    TypeID internCanonicalWithHash(Type&&, uint32_t hash);
    LayoutSetID internCanonicalWithHash(LayoutSet&&, uint32_t hash);
    void prefetchForType(uint32_t hash) const { m_types.prefetch(hash); }
    void prefetchForLayoutSet(uint32_t hash) const { m_layoutSets.prefetch(hash); }

    template<typename Record> auto& recordsForOrderedInterning()
    {
        RELEASE_ASSERT(!m_openPhase);
        if constexpr (std::same_as<Record, Type>)
            return m_types;
        else if constexpr (std::same_as<Record, LayoutSet>)
            return m_layoutSets;
        else if constexpr (std::same_as<Record, CalleeSet>)
            return m_calleeSets;
        else {
            static_assert(std::same_as<Record, PropertyKey>);
            return m_propertyKeys;
        }
    }

    bool hasOpenPhase() const { return m_openPhase; }

    // Null outside a loop whose threads share what they compute from ids that the table holds for good (ProgramCaches).
    SharedTypeMemo* sharedMemo(SharedAnswers answers) const { return m_sharedMemos[static_cast<size_t>(answers)].load(std::memory_order_acquire); }
    void setSharedMemos(SharedTypeMemo* aboutTypes, SharedTypeMemo* aboutProgram) const
    {
        m_sharedMemos[static_cast<size_t>(SharedAnswers::AboutTypes)].store(aboutTypes, std::memory_order_release);
        m_sharedMemos[static_cast<size_t>(SharedAnswers::AboutProgram)].store(aboutProgram, std::memory_order_release);
    }
    // Answers about types hold from one such loop to the next. Between two loops they lie here.
    std::unique_ptr<SharedTypeMemo> takeSavedSharedMemo() const { return std::exchange(m_savedSharedMemo, nullptr); }
    void saveSharedMemo(std::unique_ptr<SharedTypeMemo>&& aboutTypes) const { m_savedSharedMemo = WTF::move(aboutTypes); }

    void addTo(Statistics&) const;

private:
    friend class ScratchTypeArena;
    friend class TypeArena;
    friend class TypeInterningPhase;

    const Type& provisionalAt(TypeID) const;
    const LayoutSet& provisionalAt(LayoutSetID) const;
    const CalleeSet& provisionalAt(CalleeSetID) const;
    const PropertyKey& provisionalAt(PropertyKeyID) const;

    const TypeInterningPhase* phaseOfProvisionalIDs() const;
    TypeInterningPhase& attachScratchArena() const;
    void detachScratchArena(const TypeInterningPhase&) const;
    void destroyPhaseOfScratchArenas() const;

    const BuiltinTable& m_builtins;
    const Program* m_program;
    template<typename Record, typename RecordID> using RecordsThatNeverMove = InternedRecords<Record, RecordID, 0, NonMovingVector<Record>>;

    RecordsThatNeverMove<Type, TypeID> m_types;
    RecordsThatNeverMove<LayoutSet, LayoutSetID> m_layoutSets;
    RecordsThatNeverMove<CalleeSet, CalleeSetID> m_calleeSets;
    RecordsThatNeverMove<PropertyKey, PropertyKeyID> m_propertyKeys;
    unsigned m_numberOfKeysThatOperationsSpell { UINT_MAX };
    mutable TypeInterningPhase* m_openPhase { nullptr };
    // For the scratch arenas that are made while no phase is open, on whatever thread.
    mutable std::atomic<TypeInterningPhase*> m_phaseOfScratchArenas { nullptr };
    mutable std::atomic<unsigned> m_numberOfArenasInPhaseOfScratchArenas { 0 };
    mutable Lock m_phaseOfScratchArenasLock;
    mutable std::array<std::atomic<SharedTypeMemo*>, numberOfSharedAnswerKinds> m_sharedMemos { };
    mutable std::unique_ptr<SharedTypeMemo> m_savedSharedMemo;
    uint64_t m_numberOfProvisionalTypes { 0 };
};

static_assert(TypeTable::storageNumberOf(TypeID()) == TypeTable::maxStorages, "The null id must not decode to a storage.");
static_assert(TypeTable::storageNumberOf(TypeID(TypeID::hashTableDeletedIndex)) == TypeTable::maxStorages, "The deleted id must not decode to a storage.");

void canonicalize(LayoutSet&);
void canonicalize(CalleeSet&);

bool isSubsetOf(const LayoutSet&, const LayoutSet&);
bool overlaps(const LayoutSet&, const LayoutSet&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
