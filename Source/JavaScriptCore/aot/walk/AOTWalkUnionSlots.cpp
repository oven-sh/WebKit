/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `function scaleOf(s: Circle | Square) { return s.scale }`, where both keep the field in the cell in one form at different offsets: one operation on the slot, whose code finds the offset by the shape word.
// It only answers AOTWalkTypedAccesses.cpp, out of the plan alone: the answers are kept from unit to unit, so they hang neither on a loop nor on the state. At a suspension and at a throw: nothing.
// Withheld with TypedOperations, which asks.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTShapeTest.h"
#include "AOTSlotPlan.h"
#include "AOTSlotRepresentation.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// What a closed read by name costs where the place at its site answers.
static constexpr unsigned instructionsOfClosedAccessInUnionSlots = 14;
// The image keeps equal tables once, so a table costs its bytes once for a key and a set of layouts. Each unit that has such an access makes the table anew, which is what this bounds: as many shapes as a flat table takes, a byte each.
static constexpr size_t mostBytesOfOffsetTableInUnionSlots = maxShapesInFlatTable;

// The smallest family that holds every member. Null: there is none, or the set has one member.
FamilyID familyAroundSetOfLayouts(const Walker& walker, LayoutSetID set)
{
    if (!set || holdsOneMember(set))
        return { };
    TypeArena& types = walker.types();
    const Families& families = walker.facts().families();
    SetView<LayoutID, LayoutSetID> view = types.at(set);
    if (view.isList()) {
        std::span<const LayoutID> members = view.list();
        FamilyID family;
        for (LayoutID member : members.subspan(1)) {
            family = family ? families.nearestCommonFamily(family, member) : families.nearestCommonFamily(members[0], member);
            if (!family)
                break;
        }
        return family;
    }
    // The families that hold one layout stand in one another, so the first of them that holds the set is the smallest that does. All but one of a thousand members: the set has its parts in common with the
    // family, and the comparison passes those over.
    for (FamilyID family = families.nearestFamilyOf(view.first()); family; family = families.nearestFamilyAbove(family)) {
        LayoutSetID layouts = families.layoutsOf(family);
        if (types.at(layouts).size() >= view.size() && types.layoutSets().isSubset(set, layouts))
            return family;
    }
    return { };
}

// A test or a case of the program has left some members of a family: `switch (s.tag) { case "circle": case "square": s.scale }`.
bool mayAccessThroughSetOfLayouts(const Walker& walker, LayoutSetID set)
{
    return !!familyAroundSetOfLayouts(walker, set);
}

// No emitter of an access under a private name finds an offset by the shape: it compares the brand and reads at one place.
static bool isNameOfPropertyInUnionSlots(const Program& program, PropertyKeyID key)
{
    if (!key || liesInThread(key))
        return false;
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return true;
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The list that SlotAccessEmitter::distanceBehindLowestOffset() makes, and the question that it asks.
bool paysToFindOffsetByShape(const Walker& walker, std::span<const LayoutID> receivers, PropertyKeyID key, bool isStore)
{
    const Program& program = walker.program();
    if (!isNameOfPropertyInUnionSlots(program, key))
        return false;

    const SlotPlan& slots = walker.plan().slots();
    const ShapeTable& shapes = walker.plan().shapes();
    Vector<DistanceOfShapes, 8> members;
    uint32_t lowestOffset = UINT32_MAX;
    for (LayoutID receiver : receivers) {
        std::optional<FieldIndex> field = slots.findField(program, receiver, key);
        if (!field)
            return false;
        ShapeRange range = shapes.shapesOf(receiver);
        if (range.isEmpty()) {
            // A store writes without asking whether the object has the slot.
            if (isStore)
                return false;
            continue;
        }
        uint32_t offset = slots.slot(receiver, *field).offset;
        lowestOffset = std::min(lowestOffset, offset);
        members.append(DistanceOfShapes { range, offset });
    }
    if (members.isEmpty())
        return false;
    for (DistanceOfShapes& member : members)
        member.distance -= lowestOffset;

    DistancesByShape distances = distancesByShape(members.span());
    if (distances.form == DistanceForm::Comparisons)
        return true;
    return distances.numberOfInstructions() < instructionsOfClosedAccessInUnionSlots && distances.table.size() <= mostBytesOfOffsetTableInUnionSlots;
}

// `"x" in point`, where the plan keeps the field of every receiver in a form without an empty state: no bits of such a slot say that it is empty, so the property is there for the life of the object.
bool everyReceiverKeepsFieldInSlotThatIsNeverEmpty(const Walker& walker, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    const SlotPlan& slots = walker.plan().slots();
    return !receivers.empty() && std::ranges::all_of(receivers, [&](LayoutID receiver) {
        std::optional<FieldIndex> listed = slots.findField(walker.program(), receiver, key);
        if (!listed)
            return false;
        const PlacedSlot& slot = slots.slot(receiver, *listed);
        return slot.hasStorage() && !slot.holdsAccessor && !canLeaveSlot(slot.representation);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
