/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClosure.h"
#include "AOTGrouping.h"
#include "AOTLimits.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static AllocationID appendAllocation(AllocationTable& table, PlacedAllocation&& allocation)
{
    AllocationID id(table.allocations.size());
    table.allocations.append(WTF::move(allocation));
    return id;
}

static void appendClosureAllocations(AllocationTable& table, LayoutID closureLayout, unsigned maxClosureCaptures)
{
    static_assert(!(Closure::allocationSize(1) % 16));
    static_assert(defaultLimitValue(LimitID::MaxCaptures, hostArchitecture()) <= Closure::maxNumberOfCaptures);
    RELEASE_ASSERT(maxClosureCaptures <= Closure::maxNumberOfCaptures);
    for (unsigned index = 0; index <= maxClosureCaptures / 2; ++index) {
        PlacedAllocation allocation { .layout = closureLayout };
        allocation.numberOfCaptures = static_cast<uint16_t>(2 * index + 1);
        table.closureAllocations.append(appendAllocation(table, WTF::move(allocation)));
    }
}

static void groupByLayout(AllocationTable& table, size_t layoutCount)
{
    groupByIndex(layoutCount, table.firstOfLayout, table.byLayout, [&](const auto& visit) {
        for (size_t i = 0; i < table.allocations.size(); ++i)
            visit(table.allocations[i].layout.index(), AllocationID(i));
    });
}

AllocationTable numberAllocations(const PlacementContext& context, const PlacedSites& sites)
{
    auto& program = context.program;
    AllocationTable table;
    table.allocations.reserveInitialCapacity(sites.maxClosureCaptures / 2 + 1 + sites.allocations.size());
    appendClosureAllocations(table, context.closureLayout, sites.maxClosureCaptures);
    for (auto& use : sites.allocations) {
        auto& layout = program.at(declaredLayoutOf(program, use.layout));
        appendAllocation(table, {
            .layout = use.layout,
            .constructedBy = layout.kind == LayoutKind::Instance ? layout.constructor : FunctionID(),
            .initialFields = use.initialFields,
            .holdsInnerObjects = use.holdsInnerObjects,
        });
    }
    groupByLayout(table, program.layouts.size());
    return table;
}

static uint32_t shapeAllocatedBy(const ShapeTable& shapes, const AllocationUse& use)
{
    ShapeRange range = shapes.shapesOf(use.layout);
    return range.first + (use.keysAreNeverEnumerated && range.count == 2 ? 1 : 0);
}

void finishAllocations(const PlacementContext& context, const ShapeTable& shapes, const PlacedOrderWords& orderWords, std::span<const AllocationUse> uses, AllocationTable& table)
{
    auto& allocations = table.allocations;
    size_t numberOfClosureAllocations = table.closureAllocations.size();
    RELEASE_ASSERT(allocations.size() == numberOfClosureAllocations + uses.size());
    RELEASE_ASSERT(orderWords.ofAllocation.size() == allocations.size());

    for (AllocationID id : table.closureAllocations) {
        auto& allocation = allocations[id.index()];
        allocation.shape = shapes.generalShapeOf(context.closureLayout);
        allocation.size = static_cast<uint32_t>(WTF::roundUpToMultipleOf<16>(Closure::allocationSize(allocation.numberOfCaptures)));
        allocation.orderWord = 0;
    }

    for (size_t i = 0; i < uses.size(); ++i) {
        size_t row = numberOfClosureAllocations + i;
        auto& allocation = allocations[row];
        allocation.shape = shapeAllocatedBy(shapes, uses[i]);
        const PlannedLayout& planned = context.slots().layout(allocation.layout);
        allocation.size = allocation.holdsInnerObjects ? planned.sizeWithInnerObjects : planned.size;
        bool keysAreNeverEnumerated = shapes.isAssigned(allocation.shape) && shapes.shape(allocation.shape).keysAreNeverEnumerated;
        allocation.orderWord = keysAreNeverEnumerated ? unorderedOrderWord : orderWords.ofAllocation[row];
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
