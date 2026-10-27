/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTable.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTStatistics.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <functional>
#include <utility>
#include <wtf/Atomics.h>

namespace JSC { namespace AOT { namespace Typed {

void HashedIndex::add(uint32_t hash, uint32_t position)
{
    constexpr size_t initialNumberOfSlots = 16;
    RELEASE_ASSERT(position != UINT32_MAX);
    if ((static_cast<size_t>(m_size) + 1) * 3 > m_slots.size() * 2) {
        size_t numberOfSlots = std::max(initialNumberOfSlots, m_slots.size() * 2);
        Vector<Slot> oldSlots = std::exchange(m_slots, Vector<Slot>(FillWith { }, numberOfSlots, Slot { }));
        for (const Slot& slot : oldSlots) {
            if (slot.positionPlusOne)
                insert(slot);
        }
    }
    insert({ hash, position + 1 });
    ++m_size;
}

void HashedIndex::reserve(const ParallelLoops& loops, size_t numberOfAdditions)
{
    constexpr size_t initialNumberOfSlots = 16;
    constexpr size_t slotsPerTask = 1 << 14;
    size_t numberOfSlots = std::max(initialNumberOfSlots, m_slots.size());
    while ((m_size + numberOfAdditions) * 3 > numberOfSlots * 2)
        numberOfSlots *= 2;
    if (numberOfSlots == m_slots.size())
        return;
    Vector<Slot> oldSlots = std::exchange(m_slots, { });
    static_assert(!VectorTraits<Slot>::needsInitialization);
    m_slots.grow(numberOfSlots);
    forEachIndexInParallel(loops, static_cast<unsigned>((numberOfSlots + slotsPerTask - 1) / slotsPerTask), [&](unsigned task) {
        zeroSpan(m_slots.mutableSpan().subspan(task * slotsPerTask, std::min(slotsPerTask, numberOfSlots - task * slotsPerTask)));
    });
    forEachIndexInParallel(loops, static_cast<unsigned>((oldSlots.size() + slotsPerTask - 1) / slotsPerTask), [&](unsigned task) {
        size_t end = std::min(oldSlots.size(), (static_cast<size_t>(task) + 1) * slotsPerTask);
        for (size_t index = static_cast<size_t>(task) * slotsPerTask; index < end; ++index) {
            if (oldSlots[index].positionPlusOne)
                addConcurrently(oldSlots[index].hash, oldSlots[index].positionPlusOne - 1);
        }
    });
}

void HashedIndex::addConcurrently(uint32_t hash, uint32_t position)
{
    static_assert(sizeof(Slot) == sizeof(uint64_t));
    uint64_t bits = std::bit_cast<uint64_t>(Slot { hash, position + 1 });
    size_t mask = m_slots.size() - 1;
    for (size_t index = hash & mask; ; index = (index + 1) & mask) {
        uint64_t expected = 0;
        if (AtomicRef<uint64_t>(reinterpret_cast<uint64_t&>(m_slots[index])).compare_exchange_strong(expected, bits, std::memory_order_release, std::memory_order_relaxed))
            return;
    }
}

void HashedIndex::insert(Slot slot)
{
    size_t mask = m_slots.size() - 1;
    size_t index = slot.hash & mask;
    while (m_slots[index].positionPlusOne)
        index = (index + 1) & mask;
    m_slots[index] = slot;
}

void add(Hasher& hasher, const LayoutSet& set)
{
    add(hasher, set.listsEveryLayout(), static_cast<uint32_t>(set.written().size()), set.written());
}

void add(Hasher& hasher, const CalleeSet& set)
{
    add(hasher, set.isComplete, static_cast<uint32_t>(set.functions.size()), static_cast<uint32_t>(set.builtins.size()));
    add(hasher, set.functions, set.builtins);
}

void add(Hasher& hasher, const PropertyKey& key)
{
    add(hasher, key.kind, key.text, key.owner);
    add(hasher, key.symbol ? static_cast<uint32_t>(*key.symbol) + 1 : 0u);
    add(hasher, key.module);
    add(hasher, key.callNumber ? static_cast<uint64_t>(*key.callNumber) + 1 : uint64_t { 0 });
    add(hasher, key.site);
}

template<typename Record, typename RecordID, size_t segmentSize, typename Storage>
static RecordID internCanonical(InternedRecords<Record, RecordID, segmentSize, Storage>& records, Record&& record)
{
    uint32_t hash = computeHash(record);
    RecordID id = records.findOrAdd(WTF::move(record), hash);
    RELEASE_ASSERT(records.size() < TypeTable::provisionalBit);
    return id;
}

TypeTable::TypeTable(const BuiltinTable& builtins, const Program* program)
    : m_builtins(builtins)
    , m_program(program)
{
    auto internWellKnownType = [&](TypeID expected, const Type& type) {
        TypeID id = intern(type);
        RELEASE_ASSERT(id == expected);
    };
    internWellKnownType(never, Type::never());
    internWellKnownType(undefined, Type::fromPrimitives(Primitive::Undefined));
    internWellKnownType(boolean, Type::fromPrimitives(Primitive::Boolean));
    internWellKnownType(int32, Type::integer(Range::int32()));
    internWellKnownType(integer, Type::integer(Range::full()));
    internWellKnownType(number, Type::fromPrimitives(Primitive::Number));
    internWellKnownType(string, Type::fromPrimitives(Primitive::String));
    internWellKnownType(function, Type::function());
    internWellKnownType(object, Type::object());
    internWellKnownType(anything, Type::anything());
    internWellKnownType(null, Type::fromPrimitives(Primitive::Null));
    internWellKnownType(nullish, Type::fromPrimitives(Primitive::UndefinedOrNull));
    internWellKnownType(absent, Type::fromPrimitives(Primitive::Absent));
    internWellKnownType(guard, Type::forOpaque(OpaqueKind::Guard));
    internWellKnownType(length, Type::integer(Range::length()));
    internWellKnownType(anythingOrAbsent, Type::anything().withPrimitives(Primitive::Absent));
}

ProvenInEveryVersion TypeTable::provenTypeOf(const DeclaredType& declared) const
{
    if (std::optional<ProvenInEveryVersion> established = declared.established())
        return *established;
    // Only the compiler fills a position with what is no value of the language, and only it leaves one empty.
    TypeID predicted = declared.expected().predicted();
    const Type& expected = at(predicted);
    if (!expected.isJavaScriptValue())
        return Proven::byPosition(predicted);
    return Proven::byPosition(expected.primitives & Primitive::Absent ? anythingOrAbsent : anything);
}

std::optional<TypeID> TypeTable::find(const Type& canonical) const
{
    return m_types.find(canonical, computeHash(canonical));
}

std::optional<PropertyKeyID> TypeTable::find(const PropertyKey& key) const
{
    return m_propertyKeys.find(key, computeHash(key));
}

TypeID TypeTable::intern(const Type& type)
{
    return intern(Type(type));
}

TypeID TypeTable::intern(Type&& type)
{
    RELEASE_ASSERT(!m_openPhase);
    type.canonicalize();
    ASSERT(!type.layouts || at(type.layouts).listsEveryLayout());
    return internCanonical(m_types, WTF::move(type));
}

uint32_t TypeTable::hashOfCanonical(const Type& type)
{
    return computeHash(type);
}

uint32_t TypeTable::hashOfCanonical(const LayoutSet& set)
{
    return computeHash(set);
}

TypeID TypeTable::internCanonicalWithHash(Type&& type, uint32_t hash)
{
    RELEASE_ASSERT(!m_openPhase);
    ASSERT(hash == computeHash(type));
    TypeID id = m_types.findOrAdd(WTF::move(type), hash);
    RELEASE_ASSERT(m_types.size() < provisionalBit);
    return id;
}

LayoutSetID TypeTable::internCanonicalWithHash(LayoutSet&& set, uint32_t hash)
{
    RELEASE_ASSERT(!m_openPhase);
    ASSERT(hash == computeHash(set));
    LayoutSetID id = m_layoutSets.findOrAdd(WTF::move(set), hash);
    RELEASE_ASSERT(m_layoutSets.size() < provisionalBit);
    return id;
}

LayoutSetID TypeTable::intern(LayoutSet&& set)
{
    RELEASE_ASSERT(!m_openPhase);
    canonicalize(set);
    return internCanonical(m_layoutSets, WTF::move(set));
}

CalleeSetID TypeTable::intern(CalleeSet&& set)
{
    RELEASE_ASSERT(!m_openPhase);
    canonicalize(set);
    return internCanonical(m_calleeSets, WTF::move(set));
}

PropertyKeyID TypeTable::intern(const PropertyKey& key)
{
    RELEASE_ASSERT(!m_openPhase);
    return internCanonical(m_propertyKeys, PropertyKey(key));
}

void TypeTable::addTo(Statistics& statistics) const
{
    statistics.count(StatisticID::TypesInterned, size());
    statistics.count(StatisticID::ProvisionalTypesInterned, m_numberOfProvisionalTypes);
    statistics.count(StatisticID::LayoutSetsInterned, numberOfLayoutSets());
    statistics.count(StatisticID::CalleeSetsInterned, numberOfCalleeSets());
}

template<typename T>
static void canonicalizeList(Vector<T>& values)
{
    if (std::ranges::adjacent_find(values, std::ranges::greater_equal { }) == values.end())
        return;
    AOT::sort(values);
    removeRepeatedElements(values);
}

void canonicalize(LayoutSet& set)
{
    canonicalizeList(set.members);
}

void canonicalize(CalleeSet& set)
{
    canonicalizeList(set.functions);
    canonicalizeList(set.builtins);
}

bool isSubsetOf(const LayoutSet& a, const LayoutSet& b)
{
    if (!b.listsEveryLayout())
        return true;
    if (!a.listsEveryLayout())
        return false;
    std::span<const LayoutID> inA = a.completeMembers();
    std::span<const LayoutID> inB = b.completeMembers();
    if (inA.size() > inB.size())
        return false;
    if (inA.size() * 16 < inB.size()) {
        return std::ranges::all_of(inA, [&](LayoutID member) {
            return std::ranges::binary_search(inB, member);
        });
    }
    return std::ranges::includes(inB, inA);
}

bool overlaps(const LayoutSet& a, const LayoutSet& b)
{
    if (!a.listsEveryLayout() || !b.listsEveryLayout())
        return true;
    std::span<const LayoutID> inA = a.completeMembers();
    std::span<const LayoutID> inB = b.completeMembers();
    size_t indexInA = 0;
    size_t indexInB = 0;
    while (indexInA < inA.size() && indexInB < inB.size()) {
        if (inA[indexInA] == inB[indexInB])
            return true;
        if (inA[indexInA] < inB[indexInB])
            ++indexInA;
        else
            ++indexInB;
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
