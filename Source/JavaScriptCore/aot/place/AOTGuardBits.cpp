/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>
#include <span>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!(defaultLimitValue(LimitID::MaxGuardBitsPerClassSide, hostArchitecture()) % MemberGuardData::bitsPerWord));

static std::tuple<LayoutID, ClassSide, MemberGuardKind, PropertyKeyID> guardKeyOf(const GuardUse& guard)
{
    return { guard.classLayout, guard.side, guard.kind, guard.key };
}

static GuardUse guardOf(const auto& condition)
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

static Vector<GuardUse, 4> guardsOfConditions(const auto& conditions)
{
    Vector<GuardUse, 4> guards(conditions.size(), [&](size_t index) {
        return guardOf(conditions[index]);
    });
    AOT::sortInLine(guards, { }, guardKeyOf);
    guards.shrink(guards.size() - std::ranges::unique(guards, { }, guardKeyOf).size());
    return guards;
}

Vector<GuardUse, 4> guardsOf(PayloadListView<MemberConditionRecord> conditions)
{
    return guardsOfConditions(conditions);
}

Vector<GuardUse, 4> guardsOf(std::span<const MemberCondition> conditions)
{
    return guardsOfConditions(conditions);
}

GuardBitTable placeGuardBits(const PlacementContext& context, const PlacedSites& sites)
{
    GuardBitTable table;
    table.bits.reserveInitialCapacity(sites.guards.size());
    uint64_t limit = context.build.limit(LimitID::MaxGuardBitsPerClassSide);
    for (std::span<const GuardUse> remaining = sites.guards.span(); !remaining.empty();) {
        LayoutID classLayout = remaining.front().classLayout;
        RELEASE_ASSERT(context.program.at(classLayout).kind == LayoutKind::Instance);
        std::span<const GuardUse> guards = remaining.first(std::ranges::find_if_not(remaining, [&](const GuardUse& guard) {
            return guard.classLayout == classLayout && guard.side == remaining.front().side;
        }) - remaining.begin());
        remaining = remaining.subspan(guards.size());

        Vector<GuardUse> heaviest;
        if (guards.size() > limit) {
            heaviest = Vector<GuardUse>(guards);
            AOT::sort(heaviest, [](const GuardUse& a, const GuardUse& b) {
                return std::tuple { b.weight.predicted(), guardKeyOf(a) } < std::tuple { a.weight.predicted(), guardKeyOf(b) };
            });
            for (size_t index = limit; index < heaviest.size(); ++index)
                context.build.gaveUp(LimitID::MaxGuardBitsPerClassSide, { .layout = classLayout });
            heaviest.shrink(limit);
            AOT::sort(heaviest, { }, guardKeyOf);
            guards = heaviest.span();
        }
        bool holdsWhileClear = context.slots().classCreation(classLayout).guardsHoldWhileClear;
        for (uint32_t bit = 0; bit < guards.size(); ++bit)
            table.bits.append(PlacedGuardBit { .classLayout = classLayout, .side = guards[bit].side, .kind = guards[bit].kind, .key = guards[bit].key, .bit = bit, .holdsWhileClear = holdsWhileClear });
    }
    return table;
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

static bool hasOwnStaticMember(const Program& program, const Layout& layout, PropertyKeyID key)
{
    NameClass nameClass = program.nameClassOf(key);
    if (nameClass == NameClass::Length || nameClass == NameClass::Prototype || key == program.keyOf(BuiltinNames::name))
        return true;
    bool isMethod = std::ranges::any_of(layout.classFlow.members, [&](const ClassMember& member) {
        return member.side == ClassSide::Static && member.key == key;
    });
    return isMethod || std::ranges::contains(layout.staticFields, key, &StaticField::key);
}

// A constructor function is there, and a program may change it, long before its first construction defines the class.
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
// A set that holds while clear may speak of a class of created objects, and then it holds before the class has its prototype object. That is right only because the run time hears of
// every object that Object.create makes there below anything else, from the first one on: StructureTable::usualVariantOf().
bool setHoldsWhileClear(const Program& program, const GuardBitTable& table, std::span<const GuardBitID> members)
{
    return std::ranges::none_of(members, [&](GuardBitID member) {
        const PlacedGuardBit& placed = table.bits[member.index()];
        const Layout& layout = program.at(placed.classLayout);
        return layout.has(LayoutFlags::IsConstructorFunction) || std::ranges::any_of(layout.classFlow.members, &ClassMember::isAssignedByProgram) || isAwaitedBeforeClassExists(program, placed);
    });
}

// The bits of sets stand behind the bits of classes, in the order of their lists of members. They belong to no class, and every 64 of them fill a word of their own.
void placeGuardSets(const PlacementContext& context, const PlacedSites& sites, GuardBitTable& table)
{
    table.numberOfClassBits = table.bits.size();
    table.firstMemberOfSet.append(0);
    Vector<std::pair<GuardBitID, GuardBitID>> setsByMember;
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
            setsByMember.append({ member, set });
    }

    AOT::sortInLine(setsByMember);
    table.firstSetOfClassBit = Vector<uint32_t>(FillWith { }, table.numberOfClassBits + 1, 0u);
    table.setsOfClassBits.reserveInitialCapacity(setsByMember.size());
    for (auto [member, set] : setsByMember) {
        ++table.firstSetOfClassBit[member.index() + 1];
        table.setsOfClassBits.append(set);
    }
    for (size_t index = 0; index < table.numberOfClassBits; ++index)
        table.firstSetOfClassBit[index + 1] += table.firstSetOfClassBit[index];
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
    auto sets = std::views::iota(numberOfClassBits, static_cast<size_t>(bits.size()));
    auto found = std::ranges::partition_point(sets, [&](size_t set) {
        return std::ranges::lexicographical_compare(membersOf(GuardBitID(set)), members);
    });
    return found != sets.end() && std::ranges::equal(membersOf(GuardBitID(*found)), members) ? GuardBitID(*found) : GuardBitID();
}

std::span<const GuardBitID> GuardBitTable::membersOf(GuardBitID set) const
{
    if (set.index() < numberOfClassBits || set.index() >= bits.size())
        return { };
    size_t index = set.index() - numberOfClassBits;
    return membersOfSets.span().subspan(firstMemberOfSet[index], firstMemberOfSet[index + 1] - firstMemberOfSet[index]);
}

std::span<const GuardBitID> GuardBitTable::setsContaining(GuardBitID member) const
{
    if (member.index() >= numberOfClassBits)
        return { };
    return setsOfClassBits.span().subspan(firstSetOfClassBit[member.index()], firstSetOfClassBit[member.index() + 1] - firstSetOfClassBit[member.index()]);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
