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

void chooseStorageOfFields(Program&, const Facts&, BuildContext&, SlotPlanParts&);
void checkStorageOfFields(const Program&, const Facts&, const Plan&, BuildContext&);

bool hasTypedSlotsForFields(const Facts&);

inline bool hasCellsForFields(const SlotPlanParts& parts, LayoutID id)
{
    const PlannedLayout& planned = parts.layouts[id.index()];
    return planned.holdsCompiledObjects() && !planned.slots.isEmpty();
}

struct GroupsOfLayoutsForFields {
    static constexpr uint32_t noGroup = UINT32_MAX;

    Vector<uint32_t> groupOf;
    Vector<uint32_t> layouts;
};
GroupsOfLayoutsForFields groupLayoutsForFields(const Program&, const Families&);
BitVector findGroupsLaidForGoodForFields(const SlotPlanParts&, const GroupsOfLayoutsForFields&);

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

    Vector<Unit> units;
    Vector<FieldReference> rows;
    Vector<uint32_t> firstRowOfLayout;
    Vector<uint32_t> unitOfRow;
};
void chooseFormsOfSlots(Program&, const Facts&, BuildContext&, SlotPlanParts&, const GroupsOfLayoutsForFields&, const UnitsOfFields&);
UnitsOfFields collectUnitsOfFields(const Program&, const SlotPlanParts&, const GroupsOfLayoutsForFields&);

class CellSizesForFields {
public:
    explicit CellSizesForFields(const BuildContext&);

    uint64_t cellSizeFor(uint64_t bytes) const;

private:
    Vector<size_t> m_sizes;
};

bool isPlainFieldByRecords(const Program&, const Facts&, FieldReference);
bool isPlainSlotOfField(const PlannedLayout&, FieldIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
