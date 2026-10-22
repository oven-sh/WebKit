/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionContext.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

void selectTestAndBranch(SelectionContext& context)
{
    const Function& function = context.function();
    Vector<BlockID, 16> blocks;
    for (BlockID block : context.analyses().cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            continue;
        ValueID condition = terminator.conditionOperand();
        if (branchesOf(function.at(condition).opcode) != Branches::May || !context.isOnlyBranchedOn(condition))
            continue;
        blocks.append(block);
    }

    context.fuseTestsWithBranches(blocks.span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
