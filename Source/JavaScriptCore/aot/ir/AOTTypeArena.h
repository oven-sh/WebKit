/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStoragePool.h"
#include "AOTThreshold.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <optional>
#include <span>
#include "AOTBitVector.h"
#include <wtf/ConcurrentVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionRemap;
class StringRemap;

class TypeRemap {
public:
    TypeID operator()(TypeID id) const { return TypeTable::isProvisional(id) ? m_types.of(id) : id; }
    LayoutSetID operator()(LayoutSetID id) const { return TypeTable::isProvisional(id) ? m_layoutSets.of(id) : id; }
    CalleeSetID operator()(CalleeSetID id) const { return TypeTable::isProvisional(id) ? m_calleeSets.of(id) : id; }
    PropertyKeyID operator()(PropertyKeyID id) const { return TypeTable::isProvisional(id) ? m_propertyKeys.of(id) : id; }

    template<typename IDType> void apply(IDType& id) const { id = (*this)(id); }

    bool isIdentity() const { return m_isIdentity; }
    bool isIdentityFor(unsigned task) const { return !m_tasksWithNewRecords.get(task); }

private:
    friend class TypeInterningPhase;

    template<typename IDType>
    struct FinalIDs {
        IDType of(IDType provisional) const
        {
            uint32_t number = TypeTable::storageNumberOf(provisional);
            uint32_t position = TypeTable::positionOf(provisional);
            RELEASE_ASSERT(number < byStorage.size() && position < byStorage[number].size());
            IDType finalID = byStorage[number][position];
            RELEASE_ASSERT(finalID);
            return finalID;
        }

        Vector<Vector<IDType>> byStorage;
    };

    FinalIDs<TypeID> m_types;
    FinalIDs<LayoutSetID> m_layoutSets;
    FinalIDs<CalleeSetID> m_calleeSets;
    FinalIDs<PropertyKeyID> m_propertyKeys;
    BitVector m_tasksWithNewRecords;
    bool m_isIdentity { true };
};

class TypeInterningPhase {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(TypeInterningPhase);
    WTF_MAKE_NONCOPYABLE(TypeInterningPhase);
public:
    enum ScratchTag { Scratch };

    TypeInterningPhase(TypeTable&, unsigned numberOfTasks);
    TypeInterningPhase(const TypeTable&, unsigned numberOfTasks, ScratchTag);
    ~TypeInterningPhase();

    unsigned addTasks(unsigned count);

    TypeRemap finish(const StringRemap&, const ParallelLoops* = nullptr);
    TypeRemap finish(std::span<const unsigned> taskOrder, const FunctionRemap&, const StringRemap&, const ParallelLoops* = nullptr);
    void discard();

    static uint64_t currentPhaseSerialNumber(const TypeTable&);

private:
    friend class TypeArena;
    friend class TypeTable;

    template<typename Record>
    struct ProvisionalRecords {
        ConcurrentVector<Record, 64> records;
        HashedIndex index;
        Vector<uint32_t> hashes;
        Vector<uint32_t> lastLease;
    };

    struct Storage {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Storage);

        explicit Storage(uint32_t number)
            : number(number)
        {
        }

        template<typename Record> ProvisionalRecords<Record>& recordsOf();
        template<typename Record> const ProvisionalRecords<Record>& recordsOf() const;

        uint32_t number;
        uint32_t lease { 0 };
        std::atomic<bool> isLeased { true };
        ProvisionalRecords<Type> types;
        ProvisionalRecords<LayoutSet> layoutSets;
        ProvisionalRecords<CalleeSet> calleeSets;
        ProvisionalRecords<PropertyKey> propertyKeys;
    };

    struct FirstUses {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FirstUses);

        template<typename IDType> Vector<IDType>& listOf();

        Vector<PropertyKeyID> propertyKeys;
        Vector<LayoutSetID> layoutSets;
        Vector<CalleeSetID> calleeSets;
        Vector<TypeID> types;
    };

    struct Merge;
    struct ParallelMerge;
    enum OwnedByTableTag { OwnedByTable };

    TypeInterningPhase(const TypeTable&, OwnedByTableTag);

    template<typename Record, typename IDType> static const Record& recordAt(const TypeInterningPhase*, IDType);
    static uint64_t nextSerialNumber();

    Storage& leaseStorage();
    FirstUses& createFirstUses(unsigned task);
    void returnStorages(std::span<Storage* const>);
    void close();
    TypeRemap merge(std::span<const unsigned> taskOrder, const FunctionRemap*, const StringRemap&, const ParallelLoops*);

    const TypeTable& m_table;
    TypeTable* m_committedTable { nullptr };
    const uint64_t m_serialNumber { nextSerialNumber() };
    StoragePool<Storage, TypeTable::maxStorages> m_storages;
    Lock m_firstUsesGrowthLock;
    ConcurrentVector<std::unique_ptr<FirstUses>, 1024> m_firstUses;
    bool m_isOpen { true };
    bool m_isOpenPhaseOfTable { true };
};

template<> inline auto TypeInterningPhase::Storage::recordsOf<Type>() -> ProvisionalRecords<Type>& { return types; }
template<> inline auto TypeInterningPhase::Storage::recordsOf<LayoutSet>() -> ProvisionalRecords<LayoutSet>& { return layoutSets; }
template<> inline auto TypeInterningPhase::Storage::recordsOf<CalleeSet>() -> ProvisionalRecords<CalleeSet>& { return calleeSets; }
template<> inline auto TypeInterningPhase::Storage::recordsOf<PropertyKey>() -> ProvisionalRecords<PropertyKey>& { return propertyKeys; }

template<> inline auto TypeInterningPhase::Storage::recordsOf<Type>() const -> const ProvisionalRecords<Type>& { return types; }
template<> inline auto TypeInterningPhase::Storage::recordsOf<LayoutSet>() const -> const ProvisionalRecords<LayoutSet>& { return layoutSets; }
template<> inline auto TypeInterningPhase::Storage::recordsOf<CalleeSet>() const -> const ProvisionalRecords<CalleeSet>& { return calleeSets; }
template<> inline auto TypeInterningPhase::Storage::recordsOf<PropertyKey>() const -> const ProvisionalRecords<PropertyKey>& { return propertyKeys; }

template<> inline Vector<TypeID>& TypeInterningPhase::FirstUses::listOf<TypeID>() { return types; }
template<> inline Vector<LayoutSetID>& TypeInterningPhase::FirstUses::listOf<LayoutSetID>() { return layoutSets; }
template<> inline Vector<CalleeSetID>& TypeInterningPhase::FirstUses::listOf<CalleeSetID>() { return calleeSets; }
template<> inline Vector<PropertyKeyID>& TypeInterningPhase::FirstUses::listOf<PropertyKeyID>() { return propertyKeys; }

class TypeMemo {
public:
    static constexpr uint64_t keyOf(TypeID first, TypeID second) { return static_cast<uint64_t>(first.index()) << 32 | second.index(); }

    TypeID find(uint64_t key) const
    {
        if (m_slots.isEmpty())
            return { };
        size_t mask = m_slots.size() - 1;
        for (size_t index = firstIndexOf(key); ; index = (index + 1) & mask) {
            const Slot& slot = m_slots[index];
            if (!slot.type)
                return { };
            if (slot.key == key)
                return slot.type;
        }
    }

    void add(uint64_t key, TypeID);

private:
    struct Slot {
        uint64_t key { 0 };
        TypeID type;
    };

    size_t firstIndexOf(uint64_t key) const { return static_cast<size_t>((key * 0x9e3779b97f4a7c15ull) >> m_shift); }
    void insert(Slot);

    Vector<Slot> m_slots;
    unsigned m_size { 0 };
    unsigned m_shift { 64 };
};

class TypeArena {
    WTF_MAKE_NONCOPYABLE(TypeArena);
public:
    explicit TypeArena(TypeTable&);
    TypeArena(TypeInterningPhase&, unsigned task);
    TypeArena(TypeInterningPhase&, TypeInterningPhase::ScratchTag);
    ~TypeArena();

    TypeID intern(const Type&);
    TypeID intern(Type&&);
    LayoutSetID intern(LayoutSet&&);
    CalleeSetID intern(CalleeSet&&);
    PropertyKeyID intern(const PropertyKey&);

    Type adopt(const Type& fromOtherArena);
    TypeID adopt(TypeID fromOtherArena);
    LayoutSetID adopt(LayoutSetID fromOtherArena);
    CalleeSetID adopt(CalleeSetID fromOtherArena);
    template<typename Tag> bool owns(ID<Tag> id) const { return !TypeTable::isProvisional(id) || hasLeasedStorage(TypeTable::storageNumberOf(id)); }
    bool hasUsedProvisionalRecords() const { return m_firstUses; }
    PropertyKeyID adopt(PropertyKeyID fromOtherArena);

    LayoutSetID completeLayoutSet(std::span<const LayoutID> unsortedMembers);
    PropertyKeyID textKey(StringID);
    PropertyKeyID wellKnownSymbolKey(WellKnownSymbol);
    PropertyKeyID registeredSymbolKey(StringID description);

    const Type& at(TypeID id) const { return m_table.at(id); }
    const LayoutSet& at(LayoutSetID id) const { return m_table.at(id); }
    const CalleeSet& at(CalleeSetID id) const { return m_table.at(id); }
    const PropertyKey& at(PropertyKeyID id) const { return m_table.at(id); }
    const TypeTable& table() const { return m_table; }
    const BuiltinTable& builtins() const { return m_table.builtins(); }

    TypeID unionOf(TypeID, TypeID);
    TypeID intersectionOf(TypeID, TypeID);
    TypeID differenceOf(TypeID, TypeID);
    TypeID withPrimitives(TypeID, uint16_t);
    TypeID withoutPrimitives(TypeID, uint16_t);

    LayoutSetID unionOf(LayoutSetID, LayoutSetID);
    LayoutSetID intersectionOf(LayoutSetID, LayoutSetID);
    LayoutSetID differenceOf(LayoutSetID, LayoutSetID);

    static std::optional<SharedTypeMemo::Key> sharedKeyOf(SharedTypeMemo::Relation relation, TypeID first, TypeID second)
    {
        if (TypeTable::isProvisional(first) || TypeTable::isProvisional(second))
            return std::nullopt;
        return SharedTypeMemo::keyOf(relation, first, second);
    }

    TypeID sharedAnswer(SharedAnswers answers, std::optional<SharedTypeMemo::Key> key, NOESCAPE const Invocable<TypeID()> auto& compute)
    {
        SharedTypeMemo* shared = key ? m_table.sharedMemo(answers) : nullptr;
        if (!shared)
            return compute();
        TypeID known = shared->find(*key);
        if (known && !shared->verifiesAnswers()) [[likely]]
            return known;
        uint64_t unshareableEventCountBefore = m_unshareableEventCount;
        TypeID answer = compute();
        if (known) {
            RELEASE_ASSERT(known == answer);
            return answer;
        }
        if (m_unshareableEventCount == unshareableEventCountBefore && !TypeTable::isProvisional(answer))
            shared->add(*key, answer);
        return answer;
    }

    void markCurrentAnswersUnshareable() { ++m_unshareableEventCount; }
    uint64_t unshareableEventCount() const { return m_unshareableEventCount; }

private:
    enum class LayoutSetOperation : uint8_t { Union, Intersection, Difference };
    static constexpr size_t numberOfLayoutSetOperations = 3;
    AOT_CLASS_THRESHOLD(size_t, minCachedSetSize, 8);

    template<typename Result> using LayoutSetPairMap = UncheckedKeyHashMap<uint64_t, Result, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    template<typename Record, typename RecordID> RecordID internCanonical(Record&&);
    template<typename Record, typename RecordID> std::optional<RecordID> useExistingIn(TypeInterningPhase::Storage&, const Record&, uint32_t hash);
    template<typename Record, typename RecordID> RecordID didUse(TypeInterningPhase::Storage&, uint32_t position);

    bool hasLeasedStorage(uint32_t number) const;
    bool shouldCacheResult(LayoutSetID, LayoutSetID) const;
    LayoutSetID cachedResult(LayoutSetOperation, LayoutSetID, LayoutSetID);
    LayoutSetID computeResult(LayoutSetOperation, LayoutSetID, LayoutSetID);
    LayoutSetID internCompleteSet(Vector<LayoutID>&& sortedMembers);

    const TypeTable& m_table;
    TypeTable* m_serialTable { nullptr };
    TypeInterningPhase* m_phase { nullptr };
    std::optional<unsigned> m_task;
    TypeInterningPhase::FirstUses* m_firstUses { nullptr };
    Vector<TypeInterningPhase::Storage*, 1> m_storages;
    std::array<LayoutSetPairMap<LayoutSetID>, numberOfLayoutSetOperations> m_layoutSetResultCache;
    TypeMemo m_unions;
    TypeMemo m_intersections;
    UncheckedKeyHashMap<LayoutSetID, LayoutSetID> m_adoptedLayoutSets;
    uint64_t m_unshareableEventCount { 0 };
};

class ScratchTypeArena {
    WTF_MAKE_NONCOPYABLE(ScratchTypeArena);
public:
    explicit ScratchTypeArena(const TypeTable&);

    TypeArena& arena() { return m_arena; }
    operator TypeArena&() { return m_arena; }

private:
    struct Attachment {
        explicit Attachment(const TypeTable&);
        ~Attachment();

        const TypeTable& table;
        TypeInterningPhase& phase;
    };

    Attachment m_attachment;
    TypeArena m_arena;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
