/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeShapes.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

bool runsNoBlockMoreOftenThanEntry(const CFG& cfg)
{
    if (cfg.hasCycle())
        return false;
    constexpr double maxNumberOfRareEdges = frequencyConstants.maxUnlikelyEdgeShare / frequencyConstants.rareEdgeShare;
    return cfg.largestNumberOfSuccessors() <= maxNumberOfRareEdges;
}

static bool isCompactAtEveryFrequencyUpToEntry(FunctionPassContext& pass)
{
    return codeShapeFor(pass.inputs.heat.activationsOf(pass.program, pass.id) * roundingOfFrequencySums, false, BlockFrequency::Normal) == CodeShape::Compact;
}

static CodeShape computeCodeShape(FunctionPassContext& pass, BlockID block, std::optional<bool>& isCompactEverywhere)
{
    BlockFrequency recordedFrequency = pass.function.at(block).frequency;
    if (recordedFrequency == BlockFrequency::Rare || !pass.analyses.cfg().isReachable(block) || pass.runsAtMostOnce(block))
        return CodeShape::Compact;
    if (!isCompactEverywhere)
        isCompactEverywhere = isCompactAtEveryFrequencyUpToEntry(pass) && runsNoBlockMoreOftenThanEntry(pass.analyses.cfg());
    if (*isCompactEverywhere)
        return CodeShape::Compact;
    // The long forms in line pay where the code is at hand when it runs. Of a very large program most is not, and most never runs.
    double heat = pass.heatOf(block);
    CodeShape shape = codeShapeFor(heat, false, recordedFrequency, pass.inputs.heat.leastHeatOfFastCode(pass.program, pass.id, pass.analyses.blockFrequencies().frequencyOf(block)));
    if (pass.inputs.heat.isAmongHottestCode(heat))
        return shape;
    if (recordedFrequency != BlockFrequency::Normal)
        return CodeShape::Compact;
    return shape == CodeShape::Fast ? CodeShape::Balanced : shape;
}

PassOutcome chooseCodeShapes(FunctionPassContext& pass, ChosenShapes chosen)
{
    std::optional<bool> isCompactEverywhere;
    PassOutcome outcome = PassOutcome::Unchanged;
    for (uint32_t index = 0; index < pass.function.blocks.size(); ++index) {
        BlockID id(index);
        Block& block = pass.function.edit(id);
        if (chosen == ChosenShapes::KeepChosen && block.shape != CodeShape::Unchosen)
            continue;
        CodeShape shape = computeCodeShape(pass, id, isCompactEverywhere);
        if (shape == block.shape)
            continue;
        block.shape = shape;
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
