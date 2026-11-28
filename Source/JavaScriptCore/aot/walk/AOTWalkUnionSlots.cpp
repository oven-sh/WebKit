/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

static constexpr unsigned instructionsOfClosedAccessInUnionSlots = 14;
static constexpr size_t mostBytesOfOffsetTableInUnionSlots = maxShapesInFlatTable;

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
    for (FamilyID family = families.nearestFamilyOf(view.first()); family; family = families.nearestFamilyAbove(family)) {
        LayoutSetID layouts = families.layoutsOf(family);
        if (types.at(layouts).size() >= view.size() && types.layoutSets().isSubset(set, layouts))
            return family;
    }
    return { };
}

bool mayAccessThroughSetOfLayouts(const Walker& walker, LayoutSetID set)
{
    return !!familyAroundSetOfLayouts(walker, set);
}

static bool isNameOfPropertyInUnionSlots(const Program& program, PropertyKeyID key)
{
    if (!key || liesInThread(key))
        return false;
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::PrivateName:
        return true;
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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

bool everyReceiverKeepsFieldInSlotThatIsNeverEmpty(const Walker& walker, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    const SlotPlan& slots = walker.plan().slots();
    return !receivers.empty() && std::ranges::all_of(receivers, [&](LayoutID receiver) {
        std::optional<FieldIndex> listed = slots.findField(walker.program(), receiver, key);
        if (!listed)
            return false;
        const PlacedSlot& slot = slots.slot(receiver, *listed);
        return slot.hasStorage() && !slot.holdsAccessor && !mayBeCreatedByStore(slot.representation);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
