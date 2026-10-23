/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint32_t noSet = UINT32_MAX;

struct RowSet : WTF::UnionFind<RowSet> { };

} // anonymous namespace

void JoinedRows::collectAccesses(const SlotPlanContext& context, const SlotUses& uses)
{
    const Program& program = context.program;
    Vector<const SlotAccessKey*> keys;
    uint32_t numberOfReceivers = 0;
    for (const SlotAccessUse& use : uses.accesses) {
        const SlotAccessKey& access = use.access;
        if (!access.receivers)
            continue;
        if (!keys.isEmpty() && keys.last()->key == access.key && keys.last()->receivers == access.receivers) {
            m_accesses.last().weight = saturatedSum(m_accesses.last().weight, use.weight);
            continue;
        }
        keys.append(&access);
        m_accesses.append(Access { numberOfReceivers, 0, use.weight });
        numberOfReceivers += program.at(access.receivers).members.size();
    }

    m_rowsOfAccesses.grow(numberOfReceivers);
    forEachIndexInParallel(context.build.parallelLoops(), m_accesses.size(), [&](unsigned index) {
        Access& access = m_accesses[index];
        for (LayoutID member : program.at(keys[index]->receivers).members) {
            if (!context.isCompiledObject(member))
                continue;
            if (auto field = program.at(member).findField(keys[index]->key))
                m_rowsOfAccesses[access.firstRow + access.numberOfRows++] = context.rows.rowOf(member, *field);
        }
    });
}

JoinedRows::JoinedRows(const SlotPlanContext& context, const SlotUses& uses)
{
    collectAccesses(context, uses);

    uint32_t numberOfRows = context.rows.count();
    Vector<RowSet> forest(numberOfRows);
    auto rootOf = [&](SlotRow row) {
        return static_cast<SlotRow>(forest[row].find() - forest.begin());
    };
    auto join = [&](std::span<const SlotRow> rows) {
        for (SlotRow row : rows.subspan(1))
            forest[row].unify(&forest[rows[0]]);
    };
    for (const InheritedRows::Family& family : context.inheritedRows.families()) {
        if (!context.rows.isHiddenWord(family.declaringRow))
            join(context.inheritedRows.rowsOf(family));
    }
    for (const Access& access : m_accesses) {
        if (access.numberOfRows)
            join(rowsOf(access));
    }

    Vector<uint32_t> setOfRoot(FillWith { }, numberOfRows, noSet);
    for (const Access& access : m_accesses) {
        if (access.numberOfRows < 2)
            continue;
        uint32_t& index = setOfRoot[rootOf(rowsOf(access)[0])];
        if (index == noSet) {
            index = m_sets.size();
            m_sets.append(Set { });
        }
        m_sets[index].weight = saturatedSum(m_sets[index].weight, access.weight);
    }
    auto setOf = [&](SlotRow row) {
        return setOfRoot[rootOf(row)];
    };

    m_accesses.removeAllMatching([&](const Access& access) {
        return !access.numberOfRows || setOf(rowsOf(access)[0]) == noSet;
    });
    AOT::stableSort(m_accesses, { }, [&](const Access& access) {
        return setOf(rowsOf(access)[0]);
    });
    for (uint32_t index = 0; index < m_accesses.size(); ++index) {
        Set& joined = m_sets[setOf(rowsOf(m_accesses[index])[0])];
        if (!joined.numberOfAccesses++)
            joined.firstAccess = index;
    }

    for (SlotRow row = 0; row < numberOfRows; ++row) {
        if (uint32_t index = setOf(row); index != noSet)
            ++m_sets[index].numberOfRows;
    }
    uint32_t numberOfJoinedRows = 0;
    for (Set& joined : m_sets) {
        joined.firstRow = numberOfJoinedRows;
        numberOfJoinedRows += std::exchange(joined.numberOfRows, 0);
    }
    m_rows.grow(numberOfJoinedRows);
    for (SlotRow row = 0; row < numberOfRows; ++row) {
        uint32_t index = setOf(row);
        if (index == noSet)
            continue;
        Set& joined = m_sets[index];
        m_rows[joined.firstRow + joined.numberOfRows++] = row;
    }

    AOT::sort(m_sets, [](const Set& a, const Set& b) {
        return std::tuple(b.weight, a.firstRow) < std::tuple(a.weight, b.firstRow);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
