/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionContext.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Match {
    SelectionGroup group;
};

} // anonymous namespace

void selectTestAndBranch(SelectionContext& context)
{
    const Function& function = context.function();
    Vector<Match> matches;
    for (BlockID block : context.analyses().cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            continue;
        ValueID condition = terminator.conditionOperand();
        if (branchesOf(function.at(condition).opcode) != Branches::May || !context.isOnlyBranchedOn(condition))
            continue;
        matches.append(Match { groupOfOne(condition) });
    }

    context.rewriteGroups(SelectionRuleID::TestAndBranch, matches, [](GroupRewriter& rewriter, const Match&) {
        rewriter.emitTestAndBranch();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
