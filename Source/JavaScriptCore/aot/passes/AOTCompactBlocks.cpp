/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"

namespace JSC { namespace AOT { namespace Typed {

PassOutcome compactBlocks(FunctionPassContext& context)
{
    if (!compactFunction(context.function))
        return PassOutcome::Unchanged;
    context.analyses.didChangeControlFlow();
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
