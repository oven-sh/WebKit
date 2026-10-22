/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTRelationalSummariesInternal.h"
#include "AOTSupportTestCommands.h"

namespace JSC { namespace AOT { namespace Typed {

// origins-before-flow: what the record of each function says of its results when no callee is followed.
void runOriginsBeforeFlowTestCommand(TestCommandContext& context)
{
    if (!context.arguments.empty())
        return printBadArgument(context.out, context.arguments[0]);
    const Program& program = *context.program;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        for (uint32_t result = 0; result < program.functions[index].results.size(); ++result) {
            ResultOrigin origin = originOfResultBeforeFlow(program, FunctionID(index), result);
            if (origin.isFullyListed)
                context.out.println("f"_s, index, " result"_s, result, ": "_s, origin);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
