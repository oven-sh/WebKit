/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaRecords.h"
#include "AOTType.h"
#include <atomic>
#include <optional>
#include <span>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/SegmentedVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

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

private:
    // Zero for a free slot.
    struct Slot {
        uint32_t hash { 0 };
        uint32_t positionPlusOne { 0 };
    };

    void insert(Slot);

    Vector<Slot> m_slots;
    unsigned m_size { 0 };
};

void add(Hasher&, const LayoutSet&);
void add(Hasher&, const CalleeSet&);
void add(Hasher&, const PropertyKey&);

template<typename Record, typename RecordID, size_t segmentSize>
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
    SegmentedVector<Record, segmentSize> m_records;
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

    std::optional<TypeID> find(const Type& canonical) const;
    std::optional<PropertyKeyID> find(const PropertyKey&) const;

    TypeID intern(const Type&);
    LayoutSetID intern(LayoutSet&&);
    CalleeSetID intern(CalleeSet&&);
    PropertyKeyID intern(const PropertyKey&);

    bool hasOpenPhase() const { return m_openPhase; }

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
    InternedRecords<Type, TypeID, 64> m_types;
    InternedRecords<LayoutSet, LayoutSetID, 64> m_layoutSets;
    InternedRecords<CalleeSet, CalleeSetID, 64> m_calleeSets;
    InternedRecords<PropertyKey, PropertyKeyID, 64> m_propertyKeys;
    mutable TypeInterningPhase* m_openPhase { nullptr };
    // For the scratch arenas that are made while no phase is open, on whatever thread.
    mutable std::atomic<TypeInterningPhase*> m_phaseOfScratchArenas { nullptr };
    mutable std::atomic<unsigned> m_numberOfArenasInPhaseOfScratchArenas { 0 };
    mutable Lock m_phaseOfScratchArenasLock;
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
