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

Vector<uint32_t> orderOfLayoutsForShapeNumbers(Program&, const Facts&, const SlotPlan&);

struct RowsOfSlotPlan {
    static constexpr uint32_t noPlace = UINT32_MAX;
    std::span<const uint32_t> firstRowOfLayout;
    std::span<uint64_t> offsets;
    std::span<uint32_t> places;
    std::span<uint64_t> ends;
    const BitVector& layoutsWithHiddenWords;
    std::span<const uint8_t> widths { };
};
void placeFieldsOfUnions(const Program&, const LinkTables&, const Families&, RowsOfSlotPlan, NOESCAPE const ScopedLambda<bool(LayoutID)>& hasCells, NOESCAPE const ScopedLambda<uint32_t()>& addPlace);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
