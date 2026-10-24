/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static LayoutID provenBaseOf(const Program& program, LayoutID id)
{
    const LayoutFlow& flow = program.at(id).flow;
    return flow.baseState == BaseState::Layout ? flow.base : LayoutID();
}

InheritedRows::InheritedRows(const Program& program, const SlotRows& rows, std::span<const LayoutStorage> layoutStorage)
{
    unsigned numberOfLayouts = program.layouts.size();
    auto isCompiledObject = [&](LayoutID id) {
        return layoutStorage[id.index()] == LayoutStorage::CompiledObject;
    };

    Vector<LayoutID> roots(numberOfLayouts);
    m_depths.fill(0, numberOfLayouts);
    BitVector isPacked(numberOfLayouts);
    m_isInFamily.ensureSize(numberOfLayouts);
    for (unsigned index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        LayoutID root = id;
        for (LayoutID base = provenBaseOf(program, id); base; base = provenBaseOf(program, base)) {
            RELEASE_ASSERT(m_depths[index] < numberOfLayouts);
            ++m_depths[index];
            root = base;
            if (!isCompiledObject(id))
                continue;
            isPacked.quickSet(base.index());
            m_isInFamily.quickSet(base.index());
            m_isInFamily.quickSet(index);
        }
        roots[index] = root;
        if (isCompiledObject(id))
            isPacked.quickSet(index);
    }

    for (unsigned index : isPacked) {
        if (m_layoutsByDepth.size() <= m_depths[index])
            m_layoutsByDepth.grow(m_depths[index] + 1);
        m_layoutsByDepth[m_depths[index]].append(LayoutID(index));
    }

    m_declaringRow = Vector<SlotRow>(rows.count(), [](size_t row) {
        return static_cast<SlotRow>(row);
    });
    Vector<std::tuple<uint32_t, SlotRow, SlotRow>> members;
    Vector<std::pair<SlotRow, SlotRow>> rowsInUnallocatedBases;
    for (unsigned index : m_isInFamily) {
        LayoutID id(index);
        auto add = [&](SlotRow row, LayoutID declaringLayout, SlotRow declaringRow) {
            m_declaringRow[row] = declaringRow;
            if (isCompiledObject(id))
                members.append({ m_depths[declaringLayout.index()], declaringRow, row });
            else
                rowsInUnallocatedBases.append({ declaringRow, row });
        };
        for (FieldIndex field = 0; field < rows.numberOfFieldsOf(id); ++field) {
            FieldReference declaration = declarationOf(program, { id, field });
            add(rows.rowOf(id, field), declaration.layout, rows.rowOf(declaration.layout, declaration.field));
        }
        LayoutID root = roots[index];
        add(rows.extensionWordOf(id), root, rows.extensionWordOf(root));
        add(rows.extraPropertiesWordOf(id), root, rows.extraPropertiesWordOf(root));
    }
    AOT::sort(members);

    m_rows.reserveInitialCapacity(members.size());
    for (auto [depth, declaringRow, row] : members) {
        if (m_families.isEmpty() || m_families.last().declaringRow != declaringRow)
            m_families.append(Family { declaringRow, static_cast<uint32_t>(m_rows.size()), 0 });
        ++m_families.last().numberOfRows;
        m_rows.append(row);
    }

    AOT::sort(rowsInUnallocatedBases);
    m_rowsInUnallocatedBases = WTF::map(rowsInUnallocatedBases, [](const std::pair<SlotRow, SlotRow>& entry) {
        return entry.second;
    });
    for (Family& family : m_families) {
        auto found = std::ranges::equal_range(rowsInUnallocatedBases, family.declaringRow, { }, &std::pair<SlotRow, SlotRow>::first);
        family.firstRowInUnallocatedBases = static_cast<uint32_t>(found.begin() - rowsInUnallocatedBases.begin());
        family.numberOfRowsInUnallocatedBases = static_cast<uint32_t>(found.size());
    }
}

static void add(SlotRowUse& sum, const SlotRowUse& use)
{
    sum.accessWeight = saturatedSum(sum.accessWeight, use.accessWeight);
    sum.fillHeat += use.fillHeat;
    sum.holderHeat += use.holderHeat;
    sum.hasHoldersMadeInLoop |= use.hasHoldersMadeInLoop;
    sum.initializingSites += use.initializingSites;
    sum.sitesInitializingWithValue += use.sitesInitializingWithValue;
    sum.storesOfConstructorWithValue += use.storesOfConstructorWithValue;
    sum.storesOfUndefined += use.storesOfUndefined;
    sum.isRequiredByDeclaringType |= use.isRequiredByDeclaringType;
    sum.isOptionalInTypeOfThis |= use.isOptionalInTypeOfThis;
    sum.environmentUses += use.environmentUses;
    sum.storesWithoutAllocation += use.storesWithoutAllocation;
    sum.storesIntoFilledSlots += use.storesIntoFilledSlots;
    sum.storesIntoNewObjects += use.storesIntoNewObjects;
    sum.privateDefinitionsByName += use.privateDefinitionsByName;
}

void shareUsesOfInheritedRows(const InheritedRows& inheritedRows, SlotUses& uses)
{
    for (SlotRowUse& use : uses.rows)
        use.initializingSitesOfOwnLayout = use.initializingSites;
    auto share = [&](SlotRow row, SlotRowUse sum) {
        sum.initializingSitesOfOwnLayout = uses.rows[row].initializingSitesOfOwnLayout;
        uses.rows[row] = sum;
    };
    for (const InheritedRows::Family& family : inheritedRows.families()) {
        SlotRowUse sum;
        for (SlotRow row : inheritedRows.rowsOf(family))
            add(sum, uses.rows[row]);
        for (SlotRow row : inheritedRows.rowsInUnallocatedBasesOf(family))
            add(sum, uses.rows[row]);
        for (SlotRow row : inheritedRows.rowsOf(family))
            share(row, sum);
        share(family.declaringRow, sum);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
