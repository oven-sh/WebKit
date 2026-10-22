/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTRelationalSummaries.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowResults.h"

namespace JSC { namespace AOT { namespace Typed {

// relational-summary: what the link says of every function that has a record, when flow's results have been applied.
void runRelationalSummaryTestCommand(TestCommandContext& context)
{
    if (!context.arguments.empty())
        return printBadArgument(context.out, context.arguments[0]);
    FlowResults results = analyzeValueFlow(*context.program, context.program->types, context.build);
    applyFlowResults(*context.program, results, context.build);
    context.program->setForm(IRForm::Prepared);
    if (const RelationalSummaries* summaries = results.relationalSummaries())
        summaries->dump(context.out, *context.program);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
