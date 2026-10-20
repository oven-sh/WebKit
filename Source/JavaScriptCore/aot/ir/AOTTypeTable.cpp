/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTable.h"

#if ENABLE(AOT)

#include "AOTStatistics.h"
#include <algorithm>
#include <functional>
#include <utility>

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
    add(hasher, set.isComplete, static_cast<uint32_t>(set.members.size()), set.members);
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
    add(hasher, key.position ? static_cast<uint64_t>(*key.position) + 1 : uint64_t { 0 });
    add(hasher, key.site);
}

template<typename Record, typename RecordID, size_t segmentSize>
static RecordID internCanonical(InternedRecords<Record, RecordID, segmentSize>& records, Record&& record)
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
    RELEASE_ASSERT(!m_openPhase);
    Type canonical = type;
    canonical.canonicalize();
    ASSERT(!canonical.layouts || at(canonical.layouts).isComplete);
    return internCanonical(m_types, WTF::move(canonical));
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
    std::ranges::sort(values);
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
    if (!b.isComplete)
        return true;
    if (!a.isComplete)
        return false;
    return a.members.size() <= b.members.size() && std::ranges::includes(b.members, a.members);
}

bool overlaps(const LayoutSet& a, const LayoutSet& b)
{
    if (!a.isComplete || !b.isComplete)
        return true;
    size_t indexInA = 0;
    size_t indexInB = 0;
    while (indexInA < a.members.size() && indexInB < b.members.size()) {
        if (a.members[indexInA] == b.members[indexInB])
            return true;
        if (a.members[indexInA] < b.members[indexInB])
            ++indexInA;
        else
            ++indexInB;
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
