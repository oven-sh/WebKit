/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFamilies.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTThreads.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static bool comesFirstBySizeAndContentInFamilies(const TypeTable& types, const DeclaredSetForFamilies& first, const DeclaredSetForFamilies& second)
{
    if (first.size != second.size)
        return first.size < second.size;
    return compareByContent(types.at(first.layouts).untyped(), types.at(second.layouts).untyped()) < 0;
}

static SetsOfLayoutsWritten setsWrittenOnPlacesForFamilies(const Program& program, const SetsOfLayoutsWritten& writtenInBodies)
{
    SetsOfLayoutsWritten sets = writtenInBodies;
    auto note = [&](const DeclaredType& declared) {
        if (!declared.written() || declared.isExpected() || holdsOnlyPrimitives(declared.written()))
            return;
        if (LayoutSetID set = program.at(declared.written()).unnamedLayouts; set && !holdsOneMember(set))
            sets.add(set.bits());
    };
    for (const Function& function : program.functions) {
        note(function.thisType);
        for (const DeclaredType& declared : function.parameters)
            note(declared);
        for (const DeclaredType& declared : function.results)
            note(declared);
        for (const DeclaredCapture& declared : function.captures)
            note(declared);
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        for (const Field& field : layout.fields.span().first(baseWhoseFieldListIsCopiedWhole(program, LayoutID(index)) ? layout.ownFieldCount : layout.fields.size()))
            note(field.declared);
    }
    for (const Variable& variable : program.variables)
        note(variable.declared);
    return sets;
}

static void removeRepeatedSetsForFamilies(const ParallelLoops& loops, Vector<DeclaredSetForFamilies>& declaredSets)
{
    sortAndRemoveDuplicates(loops, declaredSets, [](const DeclaredSetForFamilies& first, const DeclaredSetForFamilies& second) {
        return std::tie(first.layouts, first.source) < std::tie(second.layouts, second.source);
    }, [](const DeclaredSetForFamilies& first, const DeclaredSetForFamilies& second) {
        return first.layouts == second.layouts;
    });
}

static Vector<DeclaredSetForFamilies> setsDeclaredInProgramForFamilies(const Program& program, const SetsOfLayoutsWritten* writtenInBodies, const ParallelLoops& loops)
{
    const TypeTable& types = program.types;
    unsigned numberOfLayouts = static_cast<unsigned>(program.layouts.size());
    std::span<const TypeID> writtenTypes = writtenInBodies ? types.typesOfWireIndexes() : std::span<const TypeID>();
    SetsOfLayoutsWritten setsWrittenOnPlaces;
    if (writtenInBodies)
        setsWrittenOnPlaces = setsWrittenOnPlacesForFamilies(program, *writtenInBodies);
    Vector<DeclaredSetForFamilies> declaredSets = collectInParallel<DeclaredSetForFamilies>(loops, numberOfLayouts + static_cast<unsigned>(writtenTypes.size()), [&](unsigned index, Vector<DeclaredSetForFamilies>& found) {
        LayoutSetID set;
        SourceOfSetForFamilies source = SourceOfSetForFamilies::WrittenTypeAlone;
        if (index < numberOfLayouts) {
            if (program.hasDeclaredMembers(LayoutID(index)))
                set = program.declaredMembersOf(LayoutID(index));
            auto isClass = [&](LayoutID member) {
                return program.at(member).kind == LayoutKind::Instance;
            };
            if (isClass(LayoutID(index)))
                source = SourceOfSetForFamilies::Class;
            else
                source = set && std::ranges::all_of(membersOf(types, set), isClass) ? SourceOfSetForFamilies::OtherLayoutOfClassesAlone : SourceOfSetForFamilies::OtherLayout;
        } else if (TypeID type = writtenTypes[index - numberOfLayouts]; type && !holdsOnlyPrimitives(type))
            set = types.at(type).unnamedLayouts;
        if (index >= numberOfLayouts && set && !setsWrittenOnPlaces.contains(set.bits()))
            return;
        if (!writtenInBodies && source > SourceOfSetForFamilies::OtherLayoutOfClassesAlone)
            return;
        if (set && !holdsOneMember(set))
            found.append(DeclaredSetForFamilies { set, source, types.at(set).size() });
    });
    for (LayoutSetID set : program.membersOfParentsOfOneClass())
        declaredSets.append(DeclaredSetForFamilies { set, SourceOfSetForFamilies::Class, types.at(set).size() });
    removeRepeatedSetsForFamilies(loops, declaredSets);
    return declaredSets;
}

Vector<DeclaredSetForFamilies> setsDeclaredForFamilies(const Program& program, const SetsOfLayoutsWritten& writtenInBodies, const ParallelLoops& loops)
{
    return setsDeclaredInProgramForFamilies(program, &writtenInBodies, loops);
}

static Vector<DeclaredSetForFamilies> setsInOrderOfAdmissionToFamilies(const TypeTable& types, std::span<const DeclaredSetForFamilies> declaredInProgram, std::span<const LayoutSetID> layoutsThatMet, const ParallelLoops& loops)
{
    Vector<DeclaredSetForFamilies> declaredSets(declaredInProgram);
    if (!layoutsThatMet.empty()) {
        for (LayoutSetID set : layoutsThatMet)
            declaredSets.append(DeclaredSetForFamilies { set, SourceOfSetForFamilies::MetInFlow, types.at(set).size() });
        removeRepeatedSetsForFamilies(loops, declaredSets);
    }
    parallelSort(loops, declaredSets.mutableSpan(), [&](const DeclaredSetForFamilies& first, const DeclaredSetForFamilies& second) {
        if (first.source != second.source)
            return first.source < second.source;
        return comesFirstBySizeAndContentInFamilies(types, first, second);
    });
    return declaredSets;
}

static size_t placeOfSetInFamilies(LayoutSetID set, size_t numberOfPlaces)
{
    uint32_t hash = set.bits() * 0x9e3779b1u;
    return (hash ^ (hash >> 15)) & (numberOfPlaces - 1);
}

Families::Families(Program& program, ClassesAloneTag, const ParallelLoops& loops)
    : Families(program, setsDeclaredInProgramForFamilies(program, nullptr, loops).span(), { }, loops)
{
}

Families::Families(Program& program, const SetsOfLayoutsWritten& writtenInBodies, std::span<const LayoutSetID> layoutsThatMet, const ParallelLoops& loops)
    : Families(program, setsDeclaredInProgramForFamilies(program, &writtenInBodies, loops).span(), layoutsThatMet, loops)
{
}

Families::Families(Program& program, std::span<const DeclaredSetForFamilies> declaredInProgram, std::span<const LayoutSetID> layoutsThatMet, const ParallelLoops& loops)
{
    const TypeTable& types = program.types;
    constexpr uint32_t none = UINT32_MAX;

    Vector<DeclaredSetForFamilies> declaredSets = setsInOrderOfAdmissionToFamilies(types, declaredInProgram, layoutsThatMet, loops);
    Vector<DeclaredSetForFamilies> admitted;
    Vector<uint32_t> placesAbove;
    Vector<uint32_t> placesFarAbove;
    Vector<uint32_t> nearestPlaces;
    nearestPlaces.fill(none, program.layouts.size());
    Vector<uint32_t> numbersOfLayoutsInBoth;
    Vector<uint32_t> lastLookMet;
    Vector<uint32_t, 16> met;
    Vector<LayoutID, 16> membersInNoFamilyInside;
    uint32_t look = 0;

    auto outermostNoLargerThan = [&](uint32_t place, uint32_t size) {
        while (true) {
            uint32_t farAbove = placesFarAbove[place];
            if (farAbove != place) {
                placesFarAbove[place] = placesFarAbove[farAbove];
                place = farAbove;
                continue;
            }
            uint32_t above = placesAbove[place];
            if (above == none || admitted[above].size > size)
                return place;
            placesFarAbove[place] = above;
            place = above;
        }
    };

    for (uint32_t index = 0; index < declaredSets.size(); ++index) {
        const DeclaredSetForFamilies& declared = declaredSets[index];
        if (index && declaredSets[index - 1].source != declared.source) {
            for (uint32_t place = 0; place < placesFarAbove.size(); ++place)
                placesFarAbove[place] = place;
        }
        SetView<LayoutID, LayoutSetID> members = types.at(declared.layouts);

        bool cutsAcrossFamily = false;
        bool hasMetMember = false;
        uint32_t placeAround = none;
        auto noteFamilyAround = [&](uint32_t place) {
            cutsAcrossFamily |= std::exchange(hasMetMember, true) && place != placeAround;
            placeAround = place;
        };
        auto beginLook = [&] {
            ++look;
            met.shrink(0);
            membersInNoFamilyInside.shrink(0);
            hasMetMember = false;
        };
        auto meet = [&](LayoutID member) {
            uint32_t nearest = nearestPlaces[member.index()];
            if (nearest == none || admitted[nearest].size > declared.size) {
                membersInNoFamilyInside.append(member);
                noteFamilyAround(nearest);
                return none;
            }
            uint32_t inside = outermostNoLargerThan(nearest, declared.size);
            if (std::exchange(lastLookMet[inside], look) != look) {
                numbersOfLayoutsInBoth[inside] = 0;
                met.append(inside);
                noteFamilyAround(placesAbove[inside]);
            }
            ++numbersOfLayoutsInBoth[inside];
            return inside;
        };

        uint32_t halfOfSet = none;
        beginLook();
        members.anyOf([&](LayoutID member) {
            uint32_t inside = meet(member);
            if (inside != none && admitted[inside].size >= declared.size - admitted[inside].size)
                halfOfSet = inside;
            return cutsAcrossFamily || halfOfSet != none;
        });
        if (halfOfSet != none && !cutsAcrossFamily) {
            beginLook();
            uint32_t numberOfMembersBeside = 0;
            forEachAddedMember(members.untyped(), types.at(admitted[halfOfSet].layouts).untyped(), [&](uint32_t member) {
                ++numberOfMembersBeside;
                if (!cutsAcrossFamily)
                    meet(LayoutID(member));
            });
            lastLookMet[halfOfSet] = look;
            numbersOfLayoutsInBoth[halfOfSet] = declared.size - numberOfMembersBeside;
            met.append(halfOfSet);
            noteFamilyAround(placesAbove[halfOfSet]);
        }
        cutsAcrossFamily |= std::ranges::any_of(met, [&](uint32_t place) {
            return numbersOfLayoutsInBoth[place] != admitted[place].size;
        });
        if (cutsAcrossFamily)
            continue;

        uint32_t place = admitted.size();
        for (uint32_t inside : met) {
            placesAbove[inside] = place;
            placesFarAbove[inside] = place;
        }
        for (LayoutID member : membersInNoFamilyInside)
            nearestPlaces[member.index()] = place;
        admitted.append(declared);
        placesAbove.append(placeAround);
        placesFarAbove.append(place);
        numbersOfLayoutsInBoth.append(0);
        lastLookMet.append(0);
    }

    Vector<uint32_t> placesInOrder(admitted.size(), [](size_t place) {
        return static_cast<uint32_t>(place);
    });
    parallelSort(loops, placesInOrder.mutableSpan(), [&](uint32_t first, uint32_t second) {
        return comesFirstBySizeAndContentInFamilies(types, admitted[first], admitted[second]);
    });
    Vector<FamilyID> familiesOfPlaces;
    familiesOfPlaces.grow(admitted.size());
    for (uint32_t family = 0; family < placesInOrder.size(); ++family)
        familiesOfPlaces[placesInOrder[family]] = FamilyID(family);
    auto familyOfPlace = [&](uint32_t place) {
        return place == none ? FamilyID() : familiesOfPlaces[place];
    };

    m_layouts.reserveInitialCapacity(admitted.size());
    for (uint32_t place : placesInOrder)
        m_layouts.append(admitted[place].layouts);
    m_nearestFamilyOfLayout.reserveInitialCapacity(nearestPlaces.size());
    for (uint32_t place : nearestPlaces)
        m_nearestFamilyOfLayout.append(familyOfPlace(place));

    m_waysUp.grow(admitted.size());
    for (uint32_t family = size(); family--;) {
        WayUp& way = m_waysUp[family];
        way.above = familyOfPlace(placesAbove[placesInOrder[family]]);
        if (!way.above) {
            way.farAbove = FamilyID(family);
            continue;
        }
        RELEASE_ASSERT(way.above.index() > family);
        const WayUp& wayOfAbove = m_waysUp[way.above.index()];
        const WayUp& wayOfFarAbove = m_waysUp[wayOfAbove.farAbove.index()];
        way.depth = wayOfAbove.depth + 1;
        way.farAbove = wayOfAbove.depth - wayOfFarAbove.depth == wayOfFarAbove.depth - m_waysUp[wayOfFarAbove.farAbove.index()].depth ? wayOfFarAbove.farAbove : way.above;
    }

    if (m_layouts.isEmpty())
        return;
    m_familiesBySet.fill(FamilyID(), std::bit_ceil(2 * static_cast<size_t>(m_layouts.size())));
    for (uint32_t family = 0; family < size(); ++family) {
        size_t place = placeOfSetInFamilies(m_layouts[family], m_familiesBySet.size());
        while (m_familiesBySet[place])
            place = (place + 1) & (m_familiesBySet.size() - 1);
        m_familiesBySet[place] = FamilyID(family);
    }
}

FamilyID Families::familyOf(LayoutSetID set) const
{
    if (!liesInProgram(set) || m_familiesBySet.isEmpty())
        return { };
    for (size_t place = placeOfSetInFamilies(set, m_familiesBySet.size());; place = (place + 1) & (m_familiesBySet.size() - 1)) {
        FamilyID family = m_familiesBySet[place];
        if (!family || m_layouts[family.index()] == set)
            return family;
    }
}

FamilyID Families::nearestCommonFamilyOfFamiliesOrNone(FamilyID first, FamilyID second) const
{
    if (!first || !second)
        return { };
    auto wayOf = [&](FamilyID family) -> const WayUp& {
        return m_waysUp[family.index()];
    };
    if (wayOf(first).depth < wayOf(second).depth)
        std::swap(first, second);
    uint32_t depth = wayOf(second).depth;
    while (wayOf(first).depth > depth)
        first = wayOf(wayOf(first).farAbove).depth < depth ? wayOf(first).above : wayOf(first).farAbove;
    while (first != second) {
        if (!wayOf(first).depth)
            return { };
        bool meetFarAbove = wayOf(first).farAbove == wayOf(second).farAbove;
        first = meetFarAbove ? wayOf(first).above : wayOf(first).farAbove;
        second = meetFarAbove ? wayOf(second).above : wayOf(second).farAbove;
    }
    return first;
}

FamilyID Families::nearestCommonFamily(LayoutID first, LayoutID second) const
{
    return nearestCommonFamilyOfFamiliesOrNone(nearestFamilyOf(first), nearestFamilyOf(second));
}

FamilyID Families::nearestCommonFamily(FamilyID family, LayoutID layout) const
{
    return nearestCommonFamilyOfFamiliesOrNone(family, nearestFamilyOf(layout));
}

FamilyID Families::nearestCommonFamily(FamilyID first, FamilyID second) const
{
    return nearestCommonFamilyOfFamiliesOrNone(first, second);
}

FamilyID Families::smallestFamilyHolding(const TypeArena& arena, LayoutSetID set) const
{
    SetView<LayoutID, LayoutSetID> members = arena.at(set);
    LayoutID first = members.first();
    FamilyID family;
    members.anyOf([&](LayoutID member) {
        if (member == first)
            return false;
        family = family ? nearestCommonFamily(family, member) : nearestCommonFamily(first, member);
        return !family;
    });
    return family;
}

static bool isOneThingInEveryRunForFamilies(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Builtin:
    case ObjectKind::HostObject:
    case ObjectKind::Closure:
    case ObjectKind::FunctionPrototype:
    case ObjectKind::Namespace:
    case ObjectKind::ModuleBuiltin:
    case ObjectKind::Prototype:
        return true;
    case ObjectKind::Unknown:
    case ObjectKind::ResultOfBuiltin:
    case ObjectKind::EscapedFunctionsOfHolder:
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedWithoutShape:
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::ConstructedObject:
    case ObjectKind::Instance:
    case ObjectKind::EscapedOfLayout:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ClassPartsID classPartsWidenedForFamilies(TypeArena& arena, const Families& families, ClassPartsID classes, ContentsWidenedToFamilies& found)
{
    if (!classes || homeOf(classes) == IDHome::Value)
        return classes;

    Vector<ClassPart, 8> parts(arena.at(classes));
    bool hasChanged = false;
    for (ClassPart& part : parts) {
        for (TypeID& contents : part.writtenContents) {
            if (!contents || holdsOnlyPrimitives(contents))
                continue;
            TypeID widened = widenedToFamily(arena, families, contents, SetsThatNoFamilyHolds::Stay, &found);
            if (widened == contents)
                continue;
            contents = arena.at(widened).isAnything() ? TypeID() : widened;
            hasChanged = true;
        }
    }
    return hasChanged ? arena.internClassParts(parts.span()) : classes;
}

static constexpr size_t mostLayoutsOfSetForFamilies = 4;

static LayoutSetID withFamiliesOfMembersThatShareOneForFamilies(TypeArena& arena, const Families& families, LayoutSetID set)
{
    Vector<LayoutID, mostLayoutsOfSetForFamilies> members;
    arena.at(set).forEach([&](LayoutID member) {
        members.append(member);
    });
    for (size_t first = 0; first < members.size(); ++first) {
        for (size_t second = first + 1; second < members.size(); ++second) {
            if (FamilyID family = families.nearestCommonFamily(members[first], members[second]))
                set = arena.layoutSets().unionOf(set, families.layoutsOf(family));
        }
    }
    return set;
}

TypeID widenedToFamily(TypeArena& arena, const Families& families, TypeID type, SetsThatNoFamilyHolds sets, ContentsWidenedToFamilies* foundAround)
{
    ASSERT(type);
    if (holdsOnlyPrimitives(type))
        return type;

    Type record = arena.at(type);
    bool namesObjectsThatGiveWay = record.traitsOfNamed.containsAny({ ObjectTrait::IsOtherObject, ObjectTrait::IsMerged });
    if (!namesObjectsThatGiveWay) {
        if (record.anyObject)
            return type;
        bool classesStay = !record.unnamedClasses || homeOf(record.unnamedClasses) == IDHome::Value;
        if (classesStay && (!record.unnamedLayouts || holdsOneMember(record.unnamedLayouts) || families.familyOf(record.unnamedLayouts)))
            return type;
    }
    if (TypeID found = foundAround ? foundAround->get(type.index()) : TypeID())
        return found;

    TypeParts parts = record;
    Vector<BuiltinClassID, 4> classesOfObjectsWithoutName;
    if (namesObjectsThatGiveWay) {
        const ObjectTable& objects = arena.objects();
        parts.objects = arena.objectSets().filtered(record.objects, [&](ObjectID object) {
            const ObjectRecord& named = objects.at(object);
            if (isOneThingInEveryRunForFamilies(named.kind))
                return true;
            if (named.has(ObjectTrait::HasClass)) {
                if (!classesOfObjectsWithoutName.contains(named.builtinClass))
                    classesOfObjectsWithoutName.append(named.builtinClass);
            } else if (named.has(ObjectTrait::HasLayout) && !named.has(ObjectTrait::MayHaveLayoutBelow))
                parts.unnamedLayouts = arena.layoutSets().with(parts.unnamedLayouts, named.layout);
            else {
                (named.has(ObjectTrait::IsCallable) ? parts.anyFunction : parts.anyObject) = true;
                parts.seeds.add(UnknownSeed::ObjectWithoutLayout);
            }
            return false;
        });
    }

    if (!parts.anyObject && parts.unnamedLayouts && !holdsOneMember(parts.unnamedLayouts) && !families.familyOf(parts.unnamedLayouts)) {
        if (FamilyID family = families.smallestFamilyHolding(arena, parts.unnamedLayouts))
            parts.unnamedLayouts = families.layoutsOf(family);
        else if (sets == SetsThatNoFamilyHolds::BecomeAnyObject || arena.at(parts.unnamedLayouts).size() > mostLayoutsOfSetForFamilies)
            parts.anyObject = true;
        else {
            parts.unnamedLayouts = withFamiliesOfMembersThatShareOneForFamilies(arena, families, parts.unnamedLayouts);
            parts.anyObject = arena.at(parts.unnamedLayouts).size() > mostLayoutsOfSetForFamilies;
        }
    }
    ContentsWidenedToFamilies foundHere;
    if (!parts.anyObject)
        parts.unnamedClasses = classPartsWidenedForFamilies(arena, families, parts.unnamedClasses, foundAround ? *foundAround : foundHere);

    TypeID widened = parts == static_cast<const TypeParts&>(record) ? type : arena.intern(parts);
    for (BuiltinClassID builtinClass : classesOfObjectsWithoutName)
        widened = arena.unionOf(widened, arena.builtinClass(builtinClass));
    if (foundAround)
        foundAround->add(type.index(), widened);
    return widened;
}

static TypeID withLayoutsOfFamilyForFamilies(TypeArena& arena, const Families& families, TypeID type, const Type& record, FamilyID family)
{
    TypeParts parts = record;
    if (!family)
        parts.anyObject = true;
    else if (LayoutSetID layouts = families.layoutsOf(family); layouts != record.unnamedLayouts)
        parts.unnamedLayouts = layouts;
    else
        return type;
    return arena.intern(parts);
}

static void widenLayoutsOfBothSidesForFamilies(TypeArena& arena, const Families& families, TypeID& first, TypeID& second)
{
    if (first == second || holdsOnlyPrimitives(first) || holdsOnlyPrimitives(second))
        return;
    Type recordOfFirst = arena.at(first);
    Type recordOfSecond = arena.at(second);
    LayoutSetID layoutsOfFirst = recordOfFirst.unnamedLayouts;
    LayoutSetID layoutsOfSecond = recordOfSecond.unnamedLayouts;
    if (!layoutsOfFirst || !layoutsOfSecond || layoutsOfFirst == layoutsOfSecond)
        return;

    LayoutID onlyLayoutOfFirst = holdsOneMember(layoutsOfFirst) ? LayoutID(positionOf(layoutsOfFirst)) : LayoutID();
    LayoutID onlyLayoutOfSecond = holdsOneMember(layoutsOfSecond) ? LayoutID(positionOf(layoutsOfSecond)) : LayoutID();
    FamilyID familyOfFirst = onlyLayoutOfFirst ? FamilyID() : families.familyOf(layoutsOfFirst);
    FamilyID familyOfSecond = onlyLayoutOfSecond ? FamilyID() : families.familyOf(layoutsOfSecond);
    if ((!onlyLayoutOfFirst && !familyOfFirst) || (!onlyLayoutOfSecond && !familyOfSecond))
        return;

    FamilyID common;
    if (familyOfFirst && familyOfSecond)
        common = families.nearestCommonFamily(familyOfFirst, familyOfSecond);
    else if (familyOfFirst)
        common = families.nearestCommonFamily(familyOfFirst, onlyLayoutOfSecond);
    else if (familyOfSecond)
        common = families.nearestCommonFamily(familyOfSecond, onlyLayoutOfFirst);
    else
        common = families.nearestCommonFamily(onlyLayoutOfFirst, onlyLayoutOfSecond);
    if (!common && arena.at(layoutsOfFirst).size() + arena.at(layoutsOfSecond).size() <= mostLayoutsOfSetForFamilies)
        return;

    first = withLayoutsOfFamilyForFamilies(arena, families, first, recordOfFirst, common);
    if (common)
        second = withLayoutsOfFamilyForFamilies(arena, families, second, recordOfSecond, common);
}

TypeID joinOf(TypeArena& arena, const Families& families, TypeID first, TypeID second)
{
    widenLayoutsOfBothSidesForFamilies(arena, families, first, second);
    return widenedToFamily(arena, families, arena.unionOf(first, second));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
