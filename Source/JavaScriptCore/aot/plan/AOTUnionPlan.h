/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTSlotPlan.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// The layouts of the program, each once, in the order in which computeShapeTable() hands out shape numbers to those that have cells. In it the layouts of every family of Facts::families() stand side by side, a layout in front of what is written below it,
// further sets that code tests for as far as that can be had, and the most tested first. ANY ORDER IS RIGHT. Empty, as with TypedOperations withheld: the order of today.
Vector<uint32_t> orderOfLayoutsForShapeNumbers(Program&, const Facts&, const SlotPlan&); // plan/AOTUnionShapeNumbers.cpp

// The tables of placeFieldsForSlotPlan(), which are local to plan/AOTSlotPlan.cpp, as plain spans.
struct RowsOfSlotPlan {
    static constexpr uint32_t noPlace = UINT32_MAX;
    // One more entry than there are layouts. The row of a declared field: firstRowOfLayout[layout] + field.
    std::span<const uint32_t> firstRowOfLayout;
    // By row.
    std::span<uint64_t> offsets;
    // By row. noPlace: nobody has placed it yet. Rows with one number lie at one offset in one form.
    std::span<uint32_t> places;
    // By layout: behind its last placed field.
    std::span<uint64_t> ends;
    const BitVector& layoutsWithHiddenWords;
    // By row: the bytes that the slot takes in the cell, a power of two up to eight, at which it is also aligned. 0: it does not lie in the cell, and is passed over. EMPTY, as from placeFieldsForSlotPlan(): eight for every row.
    std::span<const uint8_t> widths { };
};
// WHERE IN THE CELL A SLOT LIES. Behind placeFieldsSharedByFamiliesForSlotPlan(), in front of the layouts one by one. It places rows that have no place yet and never moves one that has. Whatever it leaves, the caller lays at the end as today.
// WHOEVER LAYS THE CELLS AGAIN LATER (chooseStorageOfFields(), once storage and forms are chosen) CALLS IT AGAIN over its own arrays, every row at noPlace, with `widths`: else what was agreed here is packed apart.
// `hasCells`: objects of the layout are compiled objects with cells of their own. `addPlace`: a new number for RowsOfSlotPlan::places. `bytesKeptZero`: by layout, emptied and grown here; PlannedLayout::bytesKeptZero takes it.
void placeFieldsOfUnions(const Program&, const LinkTables&, const Families&, RowsOfSlotPlan, NOESCAPE const ScopedLambda<bool(LayoutID)>& hasCells, NOESCAPE const ScopedLambda<uint32_t()>& addPlace, Vector<Vector<ReservedBytes>>& bytesKeptZero); // plan/AOTUnionSlots.cpp

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
