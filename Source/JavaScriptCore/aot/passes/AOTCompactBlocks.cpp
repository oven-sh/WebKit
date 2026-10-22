/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"

namespace JSC { namespace AOT { namespace Typed {

PassOutcome compactBlocks(FunctionPassContext& context)
{
    if (context.isLastStepOfSchedule && !context.build.options().verifiesAfterEveryPass)
        return compactFunction(context.function) ? PassOutcome::Changed : PassOutcome::Unchanged;

    Renumbering renumbering;
    if (!context.pendingTyping.compact(context.function, &renumbering))
        return PassOutcome::Unchanged;
    context.analyses.didRenumber(renumbering);
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
