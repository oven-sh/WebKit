/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFamilyTables.h"

#if ENABLE(AOT)

#include "AOTFieldValueNarrowing.h"
#include "AOTIRProgram.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTResultRules.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/Locker.h>
#include <wtf/SegmentedVector.h>

namespace JSC { namespace AOT { namespace Typed {

void ReceiverState::meetWith(const ReceiverState& other)
{
    bool areNeverEnumerated = keys == KeyEnumeration::NeverEnumerated && other.keys == KeyEnumeration::NeverEnumerated;
    impossibleMutations = impossibleMutations & other.impossibleMutations;
    keys = areNeverEnumerated ? KeyEnumeration::NeverEnumerated : KeyEnumeration::Unknown;
    addedKeysAreComplete = addedKeysAreComplete && other.addedKeysAreComplete;
}

// A hash table whose lookups take no lock. Entries are never removed and never move.
template<typename Entry>
class FamilyTables::ConcurrentCache {
    WTF_MAKE_NONCOPYABLE(ConcurrentCache);
public:
    ConcurrentCache() = default;

    const Entry* find(uint32_t hash, NOESCAPE const Invocable<bool(const Entry&)> auto& matches) const LIFETIME_BOUND
    {
        const Table* table = m_table.load(std::memory_order_acquire);
        return table ? table->find(hash, matches) : nullptr;
    }

    const Entry& add(uint32_t hash, Entry&& entry, NOESCAPE const Invocable<bool(const Entry&)> auto& matches) LIFETIME_BOUND
    {
        Locker locker { m_lock };
        Table* table = m_table.load(std::memory_order_relaxed);
        if (table) {
            if (const Entry* existing = table->find(hash, matches))
                return *existing;
        }
        if (!table || (m_nodes.size() + 1) * 2 > table->slots.size())
            table = &grow();
        Node& node = m_nodes.alloc(hash, WTF::move(entry));
        table->insert(node);
        return node.entry;
    }

private:
    struct Node {
        Node(uint32_t hash, Entry&& entry)
            : hash(hash)
            , entry(WTF::move(entry))
        {
        }

        uint32_t hash;
        Entry entry;
    };

    struct Table {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Table);

        explicit Table(size_t size)
            : slots(size)
        {
            for (auto& slot : slots)
                slot.store(nullptr, std::memory_order_relaxed);
        }

        const Entry* find(uint32_t hash, NOESCAPE const Invocable<bool(const Entry&)> auto& matches) const
        {
            size_t mask = slots.size() - 1;
            for (size_t index = hash & mask; const Node* node = slots[index].load(std::memory_order_acquire); index = (index + 1) & mask) {
                if (node->hash == hash && matches(node->entry))
                    return &node->entry;
            }
            return nullptr;
        }

        void insert(const Node& node)
        {
            size_t mask = slots.size() - 1;
            size_t index = node.hash & mask;
            while (slots[index].load(std::memory_order_relaxed))
                index = (index + 1) & mask;
            slots[index].store(&node, std::memory_order_release);
        }

        Vector<std::atomic<const Node*>> slots;
    };

    Table& grow() WTF_REQUIRES_LOCK(m_lock)
    {
        constexpr size_t initialSize = 16;
        auto table = makeUnique<Table>(m_tables.isEmpty() ? initialSize : m_tables.last()->slots.size() * 2);
        for (Node& node : m_nodes)
            table->insert(node);
        m_tables.append(WTF::move(table));
        m_table.store(m_tables.last().get(), std::memory_order_release);
        return *m_tables.last();
    }

    Lock m_lock;
    std::atomic<Table*> m_table { nullptr };
    // A lookup on another thread may still be probing an old table.
    Vector<std::unique_ptr<Table>> m_tables WTF_GUARDED_BY_LOCK(m_lock);
    SegmentedVector<Node, 32> m_nodes WTF_GUARDED_BY_LOCK(m_lock);
};

struct FamilyTables::Family {
    std::span<const LayoutID> members() const LIFETIME_BOUND { return hasOnlyAllocatedMembers ? allocatedMembers.span() : allMembers.span(); }

    uint32_t number { 0 };
    bool hasOnlyAllocatedMembers { true };
    Vector<LayoutID> allMembers;
    LayoutList allocatedMembers;
    ReceiverState state;
};

struct FamilyTables::Cache {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Cache);

    struct SetEntry {
        LayoutSetID set;
        const Family* family { nullptr };
    };

    struct MemberEntry {
        uint32_t family { 0 };
        PropertyKeyID key;
        FamilyMember member;
    };

    struct CandidatesEntry {
        LayoutSetID receiver;
        LayoutSetID expected;
        LayoutSetID stamp;
        const Family* receiverFamily { nullptr };
        LayoutList predicted;
    };

    struct SubsetEntry {
        LayoutSetID subset;
        LayoutSetID superset;
        bool isSubset { false };
    };

    struct ExcludedMembersEntry {
        uint32_t family { 0 };
        PropertyKeyID key;
        TypeID read;
        TypeLevel level { TypeLevel::Proven };
        LayoutSetID members;
    };

    Cache(uint64_t phase, uint64_t generation)
        : phase(phase)
        , generation(generation)
    {
    }

    const uint64_t phase;
    const uint64_t generation;
    std::atomic<uint32_t> numberOfFamilies { 0 };
    ConcurrentCache<Family> families;
    ConcurrentCache<SetEntry> familiesBySet;
    ConcurrentCache<MemberEntry> members;
    ConcurrentCache<CandidatesEntry> candidates;
    ConcurrentCache<SubsetEntry> subsets;
    ConcurrentCache<ExcludedMembersEntry> excludedMembers;
};

FamilyTables::FamilyTables(const Program& program)
    : m_program(program)
{
}

FamilyTables::~FamilyTables() = default;

auto FamilyTables::cache() const -> Cache&
{
    uint64_t phase = TypeInterningPhase::currentPhaseSerialNumber(m_program.types);
    Cache* cache = m_cache.load(std::memory_order_acquire);
    if (cache && cache->phase == phase && cache->generation == m_generation.load(std::memory_order_acquire)) [[likely]]
        return *cache;
    return replaceCache(phase);
}

auto FamilyTables::replaceCache(uint64_t phase) const -> Cache&
{
    CacheList expiredCaches;
    Locker locker { m_lock };
    uint64_t generation = m_generation.load(std::memory_order_acquire);
    Cache* cache = m_cache.load(std::memory_order_relaxed);
    if (cache && cache->phase == phase && cache->generation == generation)
        return *cache;
    if (cache && cache->phase != phase)
        expiredCaches = std::exchange(m_previousPhaseCaches, std::exchange(m_currentPhaseCaches, { }));
    m_currentPhaseCaches.append(makeUnique<Cache>(phase, generation));
    m_cache.store(m_currentPhaseCaches.last().get(), std::memory_order_release);
    return *m_currentPhaseCaches.last();
}

AOT_THRESHOLD(size_t, minCachedSetSize, 8);

static uint32_t hashLayouts(std::span<const LayoutID> layouts)
{
    uint64_t hash = layouts.size();
    for (LayoutID layout : layouts)
        hash = (hash ^ layout.index()) * 0x9E3779B97F4A7C15ull;
    return static_cast<uint32_t>(hash >> 32);
}

static LayoutList listFromSorted(std::span<const LayoutID> sortedLayouts)
{
    LayoutList list;
    for (LayoutID layout : sortedLayouts)
        list.add(layout);
    return list;
}

std::span<const LayoutID> FamilyTables::listedMembers(LayoutSetID set) const
{
    if (!set)
        return { };
    return m_program.at(set).members.span();
}

LayoutList FamilyTables::computeAllocatedMembers(std::span<const LayoutID> members) const
{
    LayoutList allocatedMembers;
    for (LayoutID layout : members) {
        if (m_program.at(layout).flow.allocation != AllocationState::NeverAllocated)
            allocatedMembers.add(layout);
    }
    return allocatedMembers;
}

auto FamilyTables::computeFamily(std::span<const LayoutID> members) const -> Family
{
    Family family;
    family.state = receiverStateOf(m_program, members);
    family.allocatedMembers = computeAllocatedMembers(members);
    family.hasOnlyAllocatedMembers = family.allocatedMembers.size() == members.size();
    if (!family.hasOnlyAllocatedMembers)
        family.allMembers = Vector<LayoutID>(members);
    return family;
}

auto FamilyTables::familyOf(Cache& cache, LayoutSetID set) const -> const Family&
{
    auto matchesSet = [&](const Cache::SetEntry& entry) {
        return entry.set == set;
    };
    if (const Cache::SetEntry* known = cache.familiesBySet.find(set.hash(), matchesSet))
        return *known->family;

    // Arenas give the same members different provisional ids. They all share one family.
    std::span<const LayoutID> members = listedMembers(set);
    uint32_t hash = hashLayouts(members);
    auto hasSameMembers = [&](const Family& family) {
        return std::ranges::equal(family.members(), members);
    };
    const Family* family = cache.families.find(hash, hasSameMembers);
    if (!family) {
        Family computed = computeFamily(members);
        computed.number = cache.numberOfFamilies.fetch_add(1, std::memory_order_relaxed);
        family = &cache.families.add(hash, WTF::move(computed), hasSameMembers);
    }
    return *cache.familiesBySet.add(set.hash(), Cache::SetEntry { set, family }, matchesSet).family;
}

std::span<const LayoutID> FamilyTables::allocatedMembersOf(LayoutSetID set) const
{
    return familyOf(cache(), set).allocatedMembers.span();
}

ReceiverState FamilyTables::stateOf(LayoutSetID set) const
{
    if (set && m_program.at(set).hasMembersWithoutLayout)
        return { };
    std::span<const LayoutID> members = listedMembers(set);
    if (members.size() < minCachedSetSize)
        return receiverStateOf(m_program, members);
    return familyOf(cache(), set).state;
}

FamilyMember FamilyTables::computeMember(const Family& family, PropertyKeyID key) const
{
    FamilyMember member;
    Vector<LayoutID> holders;
    Vector<LayoutID> withoutField;
    SortedSet<TypeID, 4> fieldTypes;
    for (LayoutID layout : family.allocatedMembers) {
        std::optional<FieldReference> field = findField(m_program, layout, key, BaseChain::Proven);
        if (!field) {
            withoutField.append(layout);
            continue;
        }
        FieldReference declaration = declarationOf(m_program, *field);
        if (holders.isEmpty()) {
            member.declaration = declaration;
            member.hasOneDeclaration = true;
            member.isAlwaysFilled = true;
        }
        holders.append(layout);
        member.hasOneDeclaration &= declaration == member.declaration;
        member.isAlwaysFilled &= staysFilled(m_program, *field);

        const DeclaredType& declared = fieldAt(m_program, *field).declared;
        member.hasExpectedField |= declared.flags.contains(PositionFlags::IsExpected);
        fieldTypes.add(declared.type);
    }

    ScratchTypeArena scratch(m_program.types);
    TypeArena& types = scratch;
    if (!holders.isEmpty())
        member.holders = types.intern(LayoutSet { WTF::move(holders), true });
    if (!withoutField.isEmpty())
        member.withoutField = types.intern(LayoutSet { WTF::move(withoutField), true });

    if (fieldTypes.isEmpty()) {
        member.unionOfFieldTypes = TypeTable::never;
        member.intersectionOfFieldTypes = TypeTable::anything;
        return member;
    }
    member.unionOfFieldTypes = fieldTypes[0];
    member.intersectionOfFieldTypes = fieldTypes[0];
    for (TypeID type : fieldTypes.span().subspan(1)) {
        member.unionOfFieldTypes = types.unionOf(member.unionOfFieldTypes, type);
        member.intersectionOfFieldTypes = types.intersectionOf(member.intersectionOfFieldTypes, type);
    }
    return member;
}

const FamilyMember& FamilyTables::memberOf(LayoutSetID set, PropertyKeyID key) const
{
    Cache& cache = this->cache();
    const Family& family = familyOf(cache, set);
    uint32_t hash = computeHash(family.number, key);
    auto matches = [&](const Cache::MemberEntry& entry) {
        return entry.family == family.number && entry.key == key;
    };
    if (const Cache::MemberEntry* known = cache.members.find(hash, matches))
        return known->member;
    return cache.members.add(hash, { family.number, key, computeMember(family, key) }, matches).member;
}

static LayoutList computePredictedMembers(const LayoutList* proven, const LayoutList* expected, const LayoutSet* stamp)
{
    if (!proven) {
        LayoutList predicted;
        if (expected)
            predicted = *expected;
        if (stamp)
            predicted.unite(listFromSorted(stamp->members.span()));
        return predicted;
    }

    LayoutList predicted = *proven;
    if (expected)
        predicted.intersect(*expected);
    if (stamp && stamp->isComplete)
        predicted.intersect(listFromSorted(stamp->members.span()));
    if (predicted.size() == proven->size())
        predicted.clear();
    return predicted;
}

auto FamilyTables::candidatesOf(LayoutSetID receiver, LayoutSetID expected, LayoutSetID stamp) const -> Candidates
{
    if (expected == receiver)
        expected = { };
    auto predictedMembers = [&](const LayoutList& proven, const LayoutList& expectedMembers) {
        return computePredictedMembers(receiver ? &proven : nullptr, expected ? &expectedMembers : nullptr, stamp ? &m_program.at(stamp) : nullptr);
    };

    if (std::max({ listedMembers(receiver).size(), listedMembers(expected).size(), listedMembers(stamp).size() }) < minCachedSetSize) {
        LayoutList proven = computeAllocatedMembers(listedMembers(receiver));
        LayoutList predicted = predictedMembers(proven, computeAllocatedMembers(listedMembers(expected)));
        return { WTF::move(proven), WTF::move(predicted) };
    }

    Cache& cache = this->cache();
    uint32_t hash = computeHash(receiver, expected, stamp);
    auto matches = [&](const Cache::CandidatesEntry& entry) {
        return entry.receiver == receiver && entry.expected == expected && entry.stamp == stamp;
    };
    const Cache::CandidatesEntry* entry = cache.candidates.find(hash, matches);
    if (!entry) {
        const Family& receiverFamily = familyOf(cache, receiver);
        entry = &cache.candidates.add(hash, { receiver, expected, stamp, &receiverFamily, predictedMembers(receiverFamily.allocatedMembers, familyOf(cache, expected).allocatedMembers) }, matches);
    }
    return { entry->receiverFamily->allocatedMembers, entry->predicted };
}

bool FamilyTables::isSubsetOf(LayoutSetID subset, LayoutSetID superset) const
{
    if (!subset || subset == superset)
        return true;
    if (!superset)
        return false;

    const LayoutSet& first = m_program.at(subset);
    const LayoutSet& second = m_program.at(superset);
    auto compute = [&] {
        return (!first.hasMembersWithoutLayout || second.hasMembersWithoutLayout) && Typed::isSubsetOf(first, second);
    };
    if (first.members.size() + second.members.size() < minCachedSetSize)
        return compute();

    Cache& cache = this->cache();
    uint32_t hash = computeHash(subset, superset);
    auto matches = [&](const Cache::SubsetEntry& entry) {
        return entry.subset == subset && entry.superset == superset;
    };
    if (const Cache::SubsetEntry* known = cache.subsets.find(hash, matches))
        return known->isSubset;
    return cache.subsets.add(hash, { subset, superset, compute() }, matches).isSubset;
}

LayoutSetID FamilyTables::computeMembersWhoseFieldCannotHold(const LayoutList& allocatedMembers, PropertyKeyID key, TypeID read, TypeLevel level) const
{
    ScratchTypeArena scratch(m_program.types);
    TypeArena& types = scratch;
    const Type& readType = m_program.at(read);
    UncheckedKeyHashMap<TypeID, bool> cannotHoldByFieldType;
    Vector<LayoutID> excluded;
    for (LayoutID layout : allocatedMembers) {
        std::optional<FieldReference> field = findField(m_program, layout, key, BaseChain::Proven);
        if (!field)
            continue;
        // Every read of a field that stays filled, by slot or by name, finds what the slot holds.
        if (level == TypeLevel::Proven && (!staysFilled(m_program, *field) || (readType.mayBeUndefinedOrNull() && slotHoldsUndefinedAndNull(m_program, *field))))
            continue;
        TypeID fieldType = level == TypeLevel::Proven ? slotTypeOf(m_program, fieldAt(m_program, *field)) : fieldAt(m_program, *field).declared.type;
        bool cannotHold = cannotHoldByFieldType.ensure(fieldType, [&] {
            return m_program.at(fieldType).intersectWith(readType, types).isNever();
        }).iterator->value;
        if (cannotHold)
            excluded.append(layout);
    }
    if (excluded.isEmpty())
        return { };
    return types.intern(LayoutSet { WTF::move(excluded), true });
}

LayoutSetID FamilyTables::membersWhoseFieldCannotHold(LayoutSetID set, PropertyKeyID key, TypeID read, TypeLevel level) const
{
    std::span<const LayoutID> members = listedMembers(set);
    if (members.size() < minCachedSetSize)
        return computeMembersWhoseFieldCannotHold(computeAllocatedMembers(members), key, read, level);

    Cache& cache = this->cache();
    const Family& family = familyOf(cache, set);
    uint32_t hash = computeHash(family.number, key, read, level);
    auto matches = [&](const Cache::ExcludedMembersEntry& entry) {
        return entry.family == family.number && entry.key == key && entry.read == read && entry.level == level;
    };
    if (const Cache::ExcludedMembersEntry* known = cache.excludedMembers.find(hash, matches))
        return known->members;
    return cache.excludedMembers.add(hash, { family.number, key, read, level, computeMembersWhoseFieldCannotHold(family.allocatedMembers, key, read, level) }, matches).members;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
