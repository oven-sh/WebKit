/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include <algorithm>
#include <tuple>

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
    for (unsigned index : m_isInFamily) {
        LayoutID id(index);
        auto add = [&](SlotRow row, LayoutID declaringLayout, SlotRow declaringRow) {
            m_declaringRow[row] = declaringRow;
            if (isCompiledObject(id))
                members.append({ m_depths[declaringLayout.index()], declaringRow, row });
        };
        for (FieldIndex field = 0; field < rows.numberOfFieldsOf(id); ++field) {
            FieldReference declaration = declarationOf(program, { id, field });
            add(rows.rowOf(id, field), declaration.layout, rows.rowOf(declaration.layout, declaration.field));
        }
        LayoutID root = roots[index];
        add(rows.extensionWordOf(id), root, rows.extensionWordOf(root));
        add(rows.extraPropertiesWordOf(id), root, rows.extraPropertiesWordOf(root));
    }
    std::ranges::sort(members);

    m_rows.reserveInitialCapacity(members.size());
    for (auto [depth, declaringRow, row] : members) {
        if (m_families.isEmpty() || m_families.last().declaringRow != declaringRow)
            m_families.append(Family { declaringRow, static_cast<uint32_t>(m_rows.size()), 0 });
        ++m_families.last().numberOfRows;
        m_rows.append(row);
    }
}

static void add(SlotRowUse& sum, const SlotRowUse& use)
{
    sum.accessWeight = saturatedSum(sum.accessWeight, use.accessWeight);
    sum.fillWeight = saturatedSum(sum.fillWeight, use.fillWeight);
    sum.holderWeight = saturatedSum(sum.holderWeight, use.holderWeight);
    sum.initializingSites += use.initializingSites;
    sum.sitesInitializingWithValue += use.sitesInitializingWithValue;
    sum.storesOfConstructorWithValue += use.storesOfConstructorWithValue;
    sum.isRequiredByDeclaringType |= use.isRequiredByDeclaringType;
    sum.storesOfOtherThanDeferredClosure += use.storesOfOtherThanDeferredClosure;
    sum.environmentUses += use.environmentUses;
    sum.integerUses += use.integerUses;
    sum.doubleUses += use.doubleUses;
    sum.doubleStores += use.doubleStores;
    sum.rangeTestedStores += use.rangeTestedStores;
    sum.storesWithoutAllocation += use.storesWithoutAllocation;
    sum.storesIntoFilledSlots += use.storesIntoFilledSlots;
    sum.wideStoresWithoutAllocation += use.wideStoresWithoutAllocation;
    sum.privateDefinitionsByName += use.privateDefinitionsByName;
}

void shareUsesOfInheritedRows(const InheritedRows& inheritedRows, SlotUses& uses)
{
    for (const InheritedRows::Family& family : inheritedRows.families()) {
        SlotRowUse sum;
        for (SlotRow row : inheritedRows.rowsOf(family))
            add(sum, uses.rows[row]);
        for (SlotRow row : inheritedRows.rowsOf(family))
            uses.rows[row] = sum;
        uses.rows[family.declaringRow] = sum;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
