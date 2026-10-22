/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include <algorithm>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Membership {
    GuardBitID member;
    GuardBitID set;

    friend auto operator<=>(const Membership&, const Membership&) = default;
};

} // anonymous namespace

static std::tuple<LayoutID, ClassSide, MemberGuardKind, PropertyKeyID> keyOf(const GuardUse& guard)
{
    return { guard.classLayout, guard.side, guard.kind, guard.key };
}

static GuardUse guardOf(const MemberConditionRecord& condition)
{
    switch (condition.kind) {
    case MemberConditionKind::MemberUnchanged:
        return { .classLayout = condition.layout, .side = condition.side, .kind = MemberGuardKind::Member, .key = condition.key };
    case MemberConditionKind::PrototypeLinkUnchanged:
        return { .classLayout = condition.layout, .side = condition.side, .kind = MemberGuardKind::PrototypeLink };
    case MemberConditionKind::ClassEvaluatedOnce:
        return { .classLayout = condition.layout, .side = ClassSide::Static, .kind = MemberGuardKind::EvaluatedOnce };
    case MemberConditionKind::InstancePrototypeUnchanged:
        return { .classLayout = condition.layout, .side = ClassSide::Prototype, .kind = MemberGuardKind::InstancePrototype };
    case MemberConditionKind::MemberNotHidden:
        return { .classLayout = condition.layout, .side = ClassSide::Prototype, .kind = MemberGuardKind::NotHidden, .key = condition.key };
    case MemberConditionKind::MemberPassedByStores:
        return { .classLayout = condition.layout, .side = ClassSide::Prototype, .kind = MemberGuardKind::PassedByStores, .key = condition.key };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<GuardUse, 4> guardsOf(PayloadListView<MemberConditionRecord> conditions)
{
    Vector<GuardUse, 4> guards(conditions.size(), [&](size_t index) {
        return guardOf(conditions[index]);
    });
    std::ranges::sort(guards, { }, keyOf);
    guards.shrink(guards.size() - std::ranges::unique(guards, { }, keyOf).size());
    return guards;
}

static MemberConditionKind recordKindOf(MemberCondition::Kind kind)
{
    switch (kind) {
    case MemberCondition::Kind::MemberUnchanged:
        return MemberConditionKind::MemberUnchanged;
    case MemberCondition::Kind::PrototypeLinkUnchanged:
        return MemberConditionKind::PrototypeLinkUnchanged;
    case MemberCondition::Kind::ClassEvaluatedOnce:
        return MemberConditionKind::ClassEvaluatedOnce;
    case MemberCondition::Kind::InstancePrototypeUnchanged:
        return MemberConditionKind::InstancePrototypeUnchanged;
    case MemberCondition::Kind::MemberNotHidden:
        return MemberConditionKind::MemberNotHidden;
    case MemberCondition::Kind::MemberPassedByStores:
        return MemberConditionKind::MemberPassedByStores;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<GuardUse, 4> guardsOf(std::span<const MemberCondition> conditions)
{
    Vector<GuardUse, 4> guards(conditions.size(), [&](size_t index) {
        const MemberCondition& condition = conditions[index];
        return guardOf(MemberConditionRecord { .kind = recordKindOf(condition.kind), .layout = condition.layout, .side = condition.side, .key = condition.key });
    });
    std::ranges::sort(guards, { }, keyOf);
    guards.shrink(guards.size() - std::ranges::unique(guards, { }, keyOf).size());
    return guards;
}

// A condition without a bit never holds, and neither does a set with such a condition. The bits of classes stand in the order of their guards, so the members come out ascending.
static bool findMembers(const GuardBitTable& table, std::span<const GuardUse> guards, Vector<GuardBitID, 4>& members)
{
    members.shrink(0);
    for (const GuardUse& guard : guards) {
        GuardBitID member = table.find(guard.classLayout, guard.side, guard.kind, guard.key);
        if (!member)
            return false;
        members.append(member);
    }
    return true;
}

static void indexSetsByMember(Vector<Membership>&& memberships, GuardBitTable& table)
{
    std::ranges::sort(memberships);
    table.firstSetOfClassBit = Vector<uint32_t>(FillWith { }, table.numberOfClassBits + 1, 0u);
    table.setsOfClassBits.reserveInitialCapacity(memberships.size());
    for (const Membership& membership : memberships) {
        ++table.firstSetOfClassBit[membership.member.index() + 1];
        table.setsOfClassBits.append(membership.set);
    }
    for (size_t index = 0; index < table.numberOfClassBits; ++index)
        table.firstSetOfClassBit[index + 1] += table.firstSetOfClassBit[index];
}

static bool hasOwnStaticMember(const Program& program, const Layout& layout, PropertyKeyID key)
{
    NameClass nameClass = program.nameClassOf(key);
    if (nameClass == NameClass::Length || nameClass == NameClass::Prototype || key == program.keyOf(BuiltinNames::name))
        return true;
    bool isMethod = std::ranges::any_of(layout.classFlow.members, [&](const ClassMember& member) {
        return member.side == ClassSide::Static && member.key == key;
    });
    return isMethod || std::ranges::any_of(layout.staticFields, [&](const StaticField& field) {
        return field.key == key;
    });
}

// A constructor function is there, and a program may change it, long before its first construction defines the class. A site that constructs tests what the function has as its prototype
// property, so it allocates by the class only once the class is defined.
// What a class object does not have itself is looked for in the class objects above it. Those may be there, and a program may change them, while nothing has created this one:
// compiled code answers `Below.name` without it.
// A guard that holds from the start waits for nothing: whoever changes a class above reaches it through the records, defined or not.
bool isAwaitedBeforeClassExists(const Program& program, const PlacedGuardBit& placed)
{
    if (placed.side != ClassSide::Static || placed.holdsWhileClear)
        return false;
    const Layout& layout = program.at(placed.classLayout);
    if (layout.has(LayoutFlags::IsConstructorFunction))
        return true;
    return placed.kind == MemberGuardKind::Member && layout.has(LayoutFlags::HasHeritage) && !hasOwnStaticMember(program, layout, placed.key);
}

// The class of a constructor function is defined by the first allocation that is carried out, so a set that speaks of one holds only once somebody has looked. So does a set with a guard that is waited for.
bool setHoldsWhileClear(const Program& program, const GuardBitTable& table, std::span<const GuardBitID> members)
{
    return std::ranges::none_of(members, [&](GuardBitID member) {
        const PlacedGuardBit& placed = table.bits[member.index()];
        return program.at(placed.classLayout).has(LayoutFlags::IsConstructorFunction) || isAwaitedBeforeClassExists(program, placed);
    });
}

// The bits of sets stand behind the bits of classes, in the order of their lists of members. They belong to no class, and every 64 of them fill a word of their own.
void placeGuardSets(const PlacementContext& context, const PlacedSites& sites, GuardBitTable& table)
{
    table.numberOfClassBits = table.bits.size();
    table.firstMemberOfSet.append(0);
    Vector<Membership> memberships;
    Vector<GuardBitID, 4> members;
    uint32_t numberOfSets = 0;
    for (const GuardSetUse& use : sites.guardSets) {
        if (!findMembers(table, use.guards.span(), members))
            continue;
        GuardBitID set(table.bits.size());
        table.bits.append(PlacedGuardBit { .bit = numberOfSets++ % MemberGuardData::bitsPerWord, .holdsWhileClear = setHoldsWhileClear(context.program, table, members.span()) });
        table.membersOfSets.appendVector(members);
        table.firstMemberOfSet.append(static_cast<uint32_t>(table.membersOfSets.size()));
        for (GuardBitID member : members)
            memberships.append({ member, set });
    }
    indexSetsByMember(WTF::move(memberships), table);
}

GuardBitID GuardBitTable::find(PayloadListView<MemberConditionRecord> conditions) const
{
    return find(guardsOf(conditions).span());
}

GuardBitID GuardBitTable::find(std::span<const GuardUse> guards) const
{
    Vector<GuardBitID, 4> members;
    if (!findMembers(*this, guards, members))
        return { };
    if (members.size() == 1)
        return members[0];
    size_t first = numberOfClassBits;
    size_t end = bits.size();
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        if (std::ranges::lexicographical_compare(membersOf(GuardBitID(middle)), members))
            first = middle + 1;
        else
            end = middle;
    }
    if (first == bits.size() || !std::ranges::equal(membersOf(GuardBitID(first)), members))
        return { };
    return GuardBitID(first);
}

std::span<const PlacedGuardBit> GuardBitTable::ofSets() const
{
    return bits.span().subspan(numberOfClassBits);
}

std::span<const GuardBitID> GuardBitTable::membersOf(GuardBitID set) const
{
    if (set.index() < numberOfClassBits || set.index() >= bits.size())
        return { };
    size_t index = set.index() - numberOfClassBits;
    uint32_t first = firstMemberOfSet[index];
    return membersOfSets.span().subspan(first, firstMemberOfSet[index + 1] - first);
}

std::span<const GuardBitID> GuardBitTable::setsContaining(GuardBitID member) const
{
    if (member.index() >= numberOfClassBits)
        return { };
    uint32_t first = firstSetOfClassBit[member.index()];
    return setsOfClassBits.span().subspan(first, firstSetOfClassBit[member.index() + 1] - first);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
