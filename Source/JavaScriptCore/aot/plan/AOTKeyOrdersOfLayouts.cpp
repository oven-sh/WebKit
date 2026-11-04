/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Mechanism::TypedSlots, what the order of keys answers for. An object lists its keys in the order in which it got them, and its cell holds that order as a number of the image.
// Where Link has proven that nobody ever asks, the layout keeps no order. A field out of line that makers only give undefined is listed in the order and takes no granule. The first Extension has the granules of what every maker goes on to fill.
// At a suspension and at a throw: nothing, it is decided once, in front of Compile. Withheld: every layout keeps its orders, and the first Extension has the granule of the store that makes it.

#include "config.h"
#include "AOTKeyOrdersOfLayouts.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTPlan.h"
#include "AOTSlotPlan.h"
#include <algorithm>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

bool keysOfLayoutAreNeverListed(const Program& program, const Facts& facts, LayoutID id)
{
    const LinkTables& linked = facts.linked();
    if (!withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedSlots) || id.index() >= linked.layouts.size() || !isFoundByShapeTest(program.at(id)))
        return false;
    const LinkedLayout& layout = linked.layout(id);
    return !layout.reachesUnseenCode && !layout.keysMayBeListed;
}

void dropKeyOrdersIfKeysAreNeverListed(const Program& program, const Facts& facts, LayoutID id, PlannedLayout& planned)
{
    planned.keysAreNeverEnumerated = planned.holdsCompiledObjects() && keysOfLayoutAreNeverListed(program, facts, id);
    if (!planned.keysAreNeverEnumerated)
        return;
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field)
        planned.slots[field].mayExistWithoutStorage = false;
}

static bool isOnlyGivenUndefinedByMakersInKeyOrdersOfLayouts(const LinkTables& linked, FieldReference field)
{
    const LinkedCell* cell = linked.findCellOfField(field);
    return cell && cell->isOnlyGivenUndefinedByMakers;
}

// `node.jsDoc = undefined` in every maker. The order of keys says that the property is there, and a read that finds no granule gives undefined: so the slot has to take undefined.
static bool mayBeListedWithoutGranuleInKeyOrdersOfLayouts(const Program& program, const LinkTables& linked, FieldReference field, const PlacedSlot& slot)
{
    if (slot.type && !(program.at(slot.type).primitives & Primitive::Undefined))
        return false;
    return !linked.isAppendedFieldRequiredBySomeWrittenType(field) || isOnlyGivenUndefinedByMakersInKeyOrdersOfLayouts(linked, field);
}

// Each granule that comes behind the first Extension is a new Extension and a copy. What some makers leave out would be room that their objects pay for, so it asks for its granule when its store comes.
static uint64_t granulesThatEveryMakerFillsInKeyOrdersOfLayouts(const LinkTables& linked, LayoutID id, const PlannedLayout& planned)
{
    FieldIndex firstAppendedField = planned.numberOfFields() - static_cast<FieldIndex>(planned.appendedKeys.size());
    uint64_t granules = 0;
    bool isFirstRun = true;
    linked.forEachRunOfMakers(id, [&](std::span<const PropertyKeyID> keys) {
        uint64_t granulesOfRun = 0;
        for (PropertyKeyID key : keys) {
            size_t place = planned.appendedKeys.find(key);
            if (place == notFound)
                continue;
            FieldIndex field = firstAppendedField + static_cast<FieldIndex>(place);
            const PlacedSlot& slot = planned.slots[field];
            if (slot.isOutOfLine() && !isOnlyGivenUndefinedByMakersInKeyOrdersOfLayouts(linked, { id, field }))
                granulesOfRun |= granuleBitOf(slot.offset);
        }
        granules = isFirstRun ? granulesOfRun : granules & granulesOfRun;
        isFirstRun = false;
    });
    return granules;
}

void finishLayoutForKeyOrders(const Program& program, const Facts& facts, LayoutID id, PlannedLayout& planned)
{
    if (!withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedSlots) || !planned.holdsCompiledObjects())
        return;
    const LinkTables& linked = facts.linked();

    // Only a key order tells a field without a granule from one that the object lacks.
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        PlacedSlot& slot = planned.slots[field];
        if (slot.isOutOfLine())
            slot.mayExistWithoutStorage = (!slot.isLeftOutByMakers || slot.mayExistWithoutStorage) && !planned.keysAreNeverEnumerated && mayBeListedWithoutGranuleInKeyOrdersOfLayouts(program, linked, { id, field }, slot);
    }
    planned.granulesOfFirstExtension = granulesThatEveryMakerFillsInKeyOrdersOfLayouts(linked, id, planned);
}

void checkKeyOrdersOfLayouts(const Program& program, const Facts& facts, const Plan& plan, BuildContext& build)
{
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const PlannedLayout& planned = plan.layout(id);
        ShapeRange shapes = plan.shapes().shapesOf(id);
        bool holds = true;
        for (uint32_t shape = shapes.first; shape < shapes.end(); ++shape)
            holds &= plan.shapes().shape(shape).keysAreNeverEnumerated == planned.keysAreNeverEnumerated;

        uint64_t granulesOfSlots = 0;
        for (const PlacedSlot& slot : planned.slots.span().first(planned.numberOfFields())) {
            if (slot.isOutOfLine())
                granulesOfSlots |= granuleBitOf(slot.offset);
            holds &= !slot.mayExistWithoutStorage || !planned.keysAreNeverEnumerated;
        }
        holds &= !(planned.granulesOfFirstExtension & ~granulesOfSlots);
        if (planned.keysAreNeverEnumerated)
            holds &= planned.holdsCompiledObjects() && keysOfLayoutAreNeverListed(program, facts, id) && !plan.keyOrders().numberOfOrdersOf(id);
        if (!holds)
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
