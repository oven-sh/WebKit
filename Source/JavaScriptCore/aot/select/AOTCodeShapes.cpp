/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeShapes.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"

namespace JSC { namespace AOT { namespace Typed {

static CodeShape computeCodeShape(FunctionPassContext& pass, BlockID block)
{
    if (!pass.analyses.cfg().isReachable(block))
        return CodeShape::Compact;
    return codeShapeFor(pass.heatOf(block), pass.runsAtMostOnce(block), pass.function.at(block).frequency);
}

PassOutcome chooseCodeShapes(FunctionPassContext& pass, ChosenShapes chosen)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    for (uint32_t index = 0; index < pass.function.blocks.size(); ++index) {
        BlockID id(index);
        Block& block = pass.function.at(id);
        if (chosen == ChosenShapes::KeepChosen && block.shape != CodeShape::Unchosen)
            continue;
        CodeShape shape = computeCodeShape(pass, id);
        if (shape == block.shape)
            continue;
        block.shape = shape;
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
