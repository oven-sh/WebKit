/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTSlotPlan.h"
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Plan;
class Program;

// ---- What other files call

// computeSlotPlanParts(), behind the layouts and in front of planNames(). With TypedSlots withheld it does nothing.
void chooseStorageOfFields(Program&, const Facts&, BuildContext&, SlotPlanParts&); // AOTFieldsStorage.cpp
// checkPlan(). Reports InternalErrorKind::PlacementCheckFailed for each rule that does not hold.
void checkStorageOfFields(const Program&, const Facts&, const Plan&, BuildContext&); // AOTFieldsStorage.cpp

// ---- What the files of plan/AOTFields* hand one another

bool hasTypedSlotsForFields(const Facts&);

inline bool hasCellsForFields(const SlotPlanParts& parts, LayoutID id)
{
    const PlannedLayout& planned = parts.layouts[id.index()];
    return planned.holdsCompiledObjects() && !planned.slots.isEmpty();
}

// The layouts of objects that a written base or a declared family ties together, as findLayoutsWithHiddenWordsForSlotPlan() of AOTSlotPlan.cpp unites them. What they share lies at one place in all of them.
struct GroupsOfLayoutsForFields {
    static constexpr uint32_t noGroup = UINT32_MAX;

    // By layout: the lowest layout of its group. noGroup: the layout is none of objects.
    Vector<uint32_t> groupOf;
    // The layouts of objects, group by group, ascending.
    Vector<uint32_t> layouts;
};
GroupsOfLayoutsForFields groupLayoutsForFields(const Program&, const Families&);
// By group. A cell with a byte of its own, PlannedLayout::isEscapedOffset, is laid for good, and so is all that shares places with it.
BitVector findGroupsLaidForGoodForFields(const SlotPlanParts&, const GroupsOfLayoutsForFields&);

// The fields under one name in the layouts of one group: ONE decision.
struct UnitsOfFields {
    static constexpr uint32_t noUnit = UINT32_MAX;

    struct Unit {
        uint32_t group { 0 };
        uint32_t firstRow { 0 };
        uint32_t numberOfRows { 0 };
    };

    std::span<const FieldReference> rowsOf(const Unit& unit) const LIFETIME_BOUND { return rows.span().subspan(unit.firstRow, unit.numberOfRows); }
    uint32_t rowOf(FieldReference field) const { return firstRowOfLayout[field.layout.index()] + field.field; }
    uint32_t numberOfRows() const { return firstRowOfLayout.last(); }

    // Group by group, and within a group by the first field that has the name. No number of a key decides an order.
    Vector<Unit> units;
    // Of a unit: ascending.
    Vector<FieldReference> rows;
    // One more entry than there are layouts. A row for each field that a layout lists and, behind those, for each that the plan has appended.
    Vector<uint32_t> firstRowOfLayout;
    // By row. noUnit: a field without a key, or one that another field of its layout hides.
    Vector<uint32_t> unitOfRow;
};
// chooseStorageOfFields() calls it in front of laying the cells. It sets PlacedSlot::representation and type, one form for each group and name, and no offset.
void chooseFormsOfSlots(Program&, const Facts&, BuildContext&, SlotPlanParts&, const GroupsOfLayoutsForFields&, const UnitsOfFields&); // AOTFieldsSlotForms.cpp
UnitsOfFields collectUnitsOfFields(const Program&, const SlotPlanParts&, const GroupsOfLayoutsForFields&);

// The sizes that the collector hands out: a cell has the size of its size class whatever it holds.
class CellSizesForFields {
public:
    explicit CellSizesForFields(const BuildContext&);

    uint64_t cellSizeFor(uint64_t bytes) const;

private:
    Vector<size_t> m_sizes;
};

// A property under a name like any other: no private name, brand or hidden field, no accessor, no attributes of its own, nothing that is put off. By the records of the program and by Facts.
bool isPlainFieldByRecords(const Program&, const Facts&, FieldReference);
// The same by what the plan has made of the slot so far.
bool isPlainSlotOfField(const PlannedLayout&, FieldIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
