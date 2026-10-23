/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool hasUnorderedAllocation(const PlacedSites& sites, LayoutID layout)
{
    auto allocations = std::ranges::equal_range(sites.allocations.span(), layout, { }, &AllocationUse::layout);
    return std::ranges::any_of(allocations, &AllocationUse::keysAreNeverEnumerated);
}

ShapeVariants chooseShapeVariants(const PlacementContext& context, const PlacedSites& sites)
{
    const Program& program = context.program;
    const FlowResults& flow = context.inputs.flow;
    auto isNeverEnumerated = [&](AllocationClassID allocationClass) {
        return flow.at(allocationClass).keys == KeyEnumeration::NeverEnumerated;
    };

    ShapeVariants variants { .ordered = BitVector(program.layouts.size()), .unordered = BitVector(program.layouts.size()) };
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (context.isEnvironment(id) || id == context.closureLayout) {
            variants.ordered.quickSet(index);
            continue;
        }
        if (!context.isCompiledObject(id))
            continue;
        auto classes = program.at(id).flow.allocationClasses.span();
        bool someOrdered = !context.slots().layout(id).keysAreNeverEnumerated;
        bool someUnordered = !someOrdered || (std::ranges::any_of(classes, isNeverEnumerated) && hasUnorderedAllocation(sites, id));
        variants.ordered.quickSet(index, someOrdered);
        variants.unordered.quickSet(index, someUnordered);
    }
    return variants;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
