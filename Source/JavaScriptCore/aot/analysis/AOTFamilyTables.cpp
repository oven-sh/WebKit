/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFamilyTables.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTResultRules.h"
#include "AOTSharedMemo.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <tuple>
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

namespace {

template<typename Entry>
class ConcurrentCache {
    WTF_MAKE_NONCOPYABLE(ConcurrentCache);
public:
    ConcurrentCache() = default;

    const Entry* find(uint32_t hash, NOESCAPE const Invocable<bool(const Entry&)> auto& matches) const LIFETIME_BOUND
    {
        const Table* table = m_table.load(std::memory_order_acquire);
        return table ? findIn(*table, hash, matches) : nullptr;
    }

    const Entry& add(uint32_t hash, Entry&& entry, NOESCAPE const Invocable<bool(const Entry&)> auto& matches) LIFETIME_BOUND
    {
        Locker locker { m_lock };
        Table* table = m_table.load(std::memory_order_relaxed);
        if (table) {
            if (const Entry* existing = findIn(*table, hash, matches))
                return *existing;
        }
        if (!table || (m_nodes.size() + 1) * 2 > table->size())
            table = &grow();
        Node& node = m_nodes.alloc(hash, WTF::move(entry));
        insertInto(*table, node);
        return node.entry;
    }

private:
    struct Node {
        uint32_t hash;
        Entry entry;
    };
    using Table = Vector<std::atomic<const Node*>>;

    static const Entry* findIn(const Table& slots, uint32_t hash, NOESCAPE const Invocable<bool(const Entry&)> auto& matches)
    {
        size_t mask = slots.size() - 1;
        for (size_t index = hash & mask; const Node* node = slots[index].load(std::memory_order_acquire); index = (index + 1) & mask) {
            if (node->hash == hash && matches(node->entry))
                return &node->entry;
        }
        return nullptr;
    }

    static void insertInto(Table& slots, const Node& node)
    {
        size_t mask = slots.size() - 1;
        size_t index = node.hash & mask;
        while (slots[index].load(std::memory_order_relaxed))
            index = (index + 1) & mask;
        slots[index].store(&node, std::memory_order_release);
    }

    Table& grow() WTF_REQUIRES_LOCK(m_lock)
    {
        constexpr size_t initialSize = 16;
        auto table = makeUnique<Table>(m_tables.isEmpty() ? initialSize : m_tables.last()->size() * 2);
        for (auto& slot : *table)
            slot.store(nullptr, std::memory_order_relaxed);
        for (Node& node : m_nodes)
            insertInto(*table, node);
        m_tables.append(WTF::move(table));
        m_table.store(m_tables.last().get(), std::memory_order_release);
        return *m_tables.last();
    }

    Lock m_lock;
    std::atomic<Table*> m_table { nullptr };
    Vector<std::unique_ptr<Table>> m_tables WTF_GUARDED_BY_LOCK(m_lock);
    SegmentedVector<Node, 32> m_nodes WTF_GUARDED_BY_LOCK(m_lock);
};

template<typename Value, typename... Key>
class CacheByKey : ConcurrentCache<std::pair<std::tuple<Key...>, Value>> {
public:
    const Value& ensure(uint32_t hash, const std::tuple<Key...>& key, NOESCAPE const Invocable<Value()> auto& compute) LIFETIME_BOUND
    {
        auto matches = [&](const auto& entry) {
            return entry.first == key;
        };
        if (const auto* known = this->find(hash, matches))
            return known->second;
        return this->add(hash, { key, compute() }, matches).second;
    }
};

} // anonymous namespace

AOT_THRESHOLD(size_t, minCachedSetSize, 8);

static LayoutList listFromSorted(std::span<const LayoutID> sortedLayouts)
{
    LayoutList list;
    for (LayoutID layout : sortedLayouts)
        list.add(layout);
    return list;
}

static std::span<const LayoutID> membersListedIn(const Program& program, LayoutSetID set)
{
    if (!set)
        return { };
    return program.at(set).written().span();
}

static LayoutList computeAllocatedMembers(const Program& program, std::span<const LayoutID> members)
{
    LayoutList allocatedMembers;
    for (LayoutID layout : members) {
        if (program.at(layout).flow.allocation != AllocationState::NeverAllocated)
            allocatedMembers.add(layout);
    }
    return allocatedMembers;
}

struct FamilyTables::Cache {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Cache);

    struct Family {
        std::span<const LayoutID> members() const LIFETIME_BOUND { return hasOnlyAllocatedMembers ? allocatedMembers.span() : allMembers.span(); }

        uint32_t number { 0 };
        bool hasOnlyAllocatedMembers { true };
        Vector<LayoutID> allMembers;
        LayoutList allocatedMembers;
        ReceiverState state;
    };

    struct Candidates {
        const Family* receiverFamily { nullptr };
        LayoutList predicted;
        LayoutList ofExpectedType;
    };

    Cache(uint64_t phase, uint64_t generation)
        : phase(phase)
        , generation(generation)
    {
    }

    const Family& familyOf(const Program& program, LayoutSetID set) LIFETIME_BOUND
    {
        return *familiesBySet.ensure(set.hash(), { set }, [&]() -> const Family* {
            std::span<const LayoutID> members = membersListedIn(program, set);
            uint32_t hash = hashOfLayouts(members);
            auto hasSameMembers = [&](const Family& family) {
                return std::ranges::equal(family.members(), members);
            };
            if (const Family* family = families.find(hash, hasSameMembers))
                return family;

            Family family;
            family.state = receiverStateOf(program, members);
            family.allocatedMembers = computeAllocatedMembers(program, members);
            family.hasOnlyAllocatedMembers = family.allocatedMembers.size() == members.size();
            if (!family.hasOnlyAllocatedMembers)
                family.allMembers = Vector<LayoutID>(members);
            family.number = numberOfFamilies.fetch_add(1, std::memory_order_relaxed);
            return &families.add(hash, WTF::move(family), hasSameMembers);
        });
    }

    const uint64_t phase;
    const uint64_t generation;
    std::atomic<uint32_t> numberOfFamilies { 0 };
    ConcurrentCache<Family> families;
    CacheByKey<const Family*, LayoutSetID> familiesBySet;
    CacheByKey<FamilyMember, uint32_t, PropertyKeyID> members;
    CacheByKey<LayoutSetID, uint32_t, PropertyKeyID, TypeID, TypeLevel> excludedMembers;
    CacheByKey<Candidates, LayoutSetID, LayoutSetID, LayoutSetID> candidates;
    CacheByKey<bool, LayoutSetID, LayoutSetID> subsets;
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

std::span<const LayoutID> FamilyTables::allocatedMembersOf(LayoutSetID set) const
{
    return cache().familyOf(m_program, set).allocatedMembers.span();
}

ReceiverState FamilyTables::stateOf(LayoutSetID set) const
{
    if (set && m_program.at(set).mayHoldObjectsWithoutLayout())
        return { };
    std::span<const LayoutID> members = membersListedIn(m_program, set);
    if (members.size() < minCachedSetSize)
        return receiverStateOf(m_program, members);
    return cache().familyOf(m_program, set).state;
}

static FamilyMember computeMember(const Program& program, const LayoutList& allocatedMembers, PropertyKeyID key)
{
    FamilyMember member;
    Vector<LayoutID> holders;
    Vector<LayoutID> withoutField;
    for (LayoutID layout : allocatedMembers) {
        std::optional<FieldReference> field = findField(program, layout, key, BaseChain::Proven);
        if (!field) {
            withoutField.append(layout);
            continue;
        }
        FieldReference declaration = declarationOf(program, *field);
        if (holders.isEmpty()) {
            member.declaration = declaration;
            member.hasOneDeclaration = true;
        }
        holders.append(layout);
        member.hasOneDeclaration &= declaration == member.declaration;
    }

    ScratchTypeArena scratch(program.types);
    TypeArena& types = scratch;
    if (!holders.isEmpty())
        member.holders = types.intern(LayoutSet::complete(WTF::move(holders)));
    if (!withoutField.isEmpty())
        member.withoutField = types.intern(LayoutSet::complete(WTF::move(withoutField)));
    return member;
}

const FamilyMember& FamilyTables::memberOf(LayoutSetID set, PropertyKeyID key) const
{
    Cache& cache = this->cache();
    const Cache::Family& family = cache.familyOf(m_program, set);
    return cache.members.ensure(computeHash(family.number, key), { family.number, key }, [&] {
        return computeMember(m_program, family.allocatedMembers, key);
    });
}

auto FamilyTables::candidatesOf(LayoutSetID receiver, LayoutSetID expected, LayoutSetID stamp) const -> Candidates
{
    if (expected == receiver)
        expected = { };
    auto predictedMembers = [&](const LayoutList& proven, const LayoutList& expectedMembers) {
        const LayoutSet* stamped = stamp ? &m_program.at(stamp) : nullptr;
        LayoutList predicted;
        if (!receiver) {
            if (expected)
                predicted = expectedMembers;
            if (stamped)
                predicted.unite(listFromSorted(stamped->seen().predicted()));
            return predicted;
        }

        predicted = proven;
        if (expected)
            predicted.intersect(expectedMembers);
        if (stamped && stamped->listsEveryLayout())
            predicted.intersect(listFromSorted(stamped->completeMembers()));
        if (predicted.size() == proven.size())
            predicted.clear();
        return predicted;
    };
    auto membersOfExpectedType = [&](const LayoutList& expectedMembers) {
        return receiver || !expected ? LayoutList() : expectedMembers;
    };

    if (std::max({ membersListedIn(m_program, receiver).size(), membersListedIn(m_program, expected).size(), membersListedIn(m_program, stamp).size() }) < minCachedSetSize) {
        LayoutList proven = computeAllocatedMembers(m_program, membersListedIn(m_program, receiver));
        LayoutList expectedMembers = computeAllocatedMembers(m_program, membersListedIn(m_program, expected));
        LayoutList predicted = predictedMembers(proven, expectedMembers);
        return { WTF::move(proven), WTF::move(predicted), membersOfExpectedType(expectedMembers) };
    }

    Cache& cache = this->cache();
    const Cache::Candidates& known = cache.candidates.ensure(computeHash(receiver, expected, stamp), { receiver, expected, stamp }, [&] {
        const Cache::Family& receiverFamily = cache.familyOf(m_program, receiver);
        const LayoutList& expectedMembers = cache.familyOf(m_program, expected).allocatedMembers;
        return Cache::Candidates { &receiverFamily, predictedMembers(receiverFamily.allocatedMembers, expectedMembers), membersOfExpectedType(expectedMembers) };
    });
    return { known.receiverFamily->allocatedMembers, known.predicted, known.ofExpectedType };
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
        return Typed::isSubsetOf(first, second);
    };
    if (first.written().size() + second.written().size() < minCachedSetSize)
        return compute();
    return cache().subsets.ensure(computeHash(subset, superset), { subset, superset }, compute);
}

static LayoutSetID computeMembersWhoseFieldCannotHold(const Program& program, const LayoutList& allocatedMembers, PropertyKeyID key, TypeID read, TypeLevel level)
{
    ScratchTypeArena scratch(program.types);
    TypeArena& types = scratch;
    const Type& readType = program.at(read);
    UncheckedKeyHashMap<TypeID, bool> cannotHoldByFieldType;
    Vector<LayoutID> excluded;
    for (LayoutID layout : allocatedMembers) {
        std::optional<FieldReference> field = findField(program, layout, key, BaseChain::Proven);
        if (!field)
            continue;
        if (level == TypeLevel::Proven && (!staysFilled(program, *field) || (readType.mayBeUndefinedOrNull() && slotHoldsUndefinedAndNull(program, *field))))
            continue;
        TypeID fieldType = level == TypeLevel::Proven ? slotTypeOf(program, fieldAt(program, *field)) : fieldAt(program, *field).declared.expected().predicted();
        bool cannotHold = cannotHoldByFieldType.ensure(fieldType, [&] {
            return program.at(fieldType).intersectWith(readType, types).isNever();
        }).iterator->value;
        if (cannotHold)
            excluded.append(layout);
    }
    if (excluded.isEmpty())
        return { };
    return types.intern(LayoutSet::complete(WTF::move(excluded)));
}

LayoutSetID FamilyTables::membersWhoseFieldCannotHold(LayoutSetID set, PropertyKeyID key, TypeID read, TypeLevel level) const
{
    std::span<const LayoutID> members = membersListedIn(m_program, set);
    if (members.size() < minCachedSetSize)
        return computeMembersWhoseFieldCannotHold(m_program, computeAllocatedMembers(m_program, members), key, read, level);

    Cache& cache = this->cache();
    const Cache::Family& family = cache.familyOf(m_program, set);
    return cache.excludedMembers.ensure(computeHash(family.number, key, read, level), { family.number, key, read, level }, [&] {
        return computeMembersWhoseFieldCannotHold(m_program, family.allocatedMembers, key, read, level);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
