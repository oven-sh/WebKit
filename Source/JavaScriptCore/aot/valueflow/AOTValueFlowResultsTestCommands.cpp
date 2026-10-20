/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowAnalysis.h"
#include "AOTIRPrinter.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowResults.h"
#include "AOTVerifier.h"

namespace JSC { namespace AOT { namespace Typed {

static FlowResults analyzeAndApply(TestCommandContext& context, Vector<String>* setsOutsideTypes = nullptr)
{
    FlowResults results = analyzeValueFlow(*context.program, context.program->types, context.build, setsOutsideTypes);
    applyFlowResults(*context.program, results, context.build);
    context.program->setForm(IRForm::Prepared);
    return results;
}

// valueflow-results: the tables that the stage AnalyzeFlow hands on, then the sets that name an object outside the type of the place where it arrives.
void runValueFlowResultsTestCommand(TestCommandContext& context)
{
    if (!context.arguments.empty())
        return printBadArgument(context.out, context.arguments[0]);
    Vector<String> setsOutsideTypes;
    analyzeAndApply(context, &setsOutsideTypes).dump(context.out, *context.program);
    for (const String& line : setsOutsideTypes)
        context.out.println("outside-type "_s, line);
}

// valueflow-apply: the program with the tables applied, validated and verified.
void runValueFlowApplyTestCommand(TestCommandContext& context)
{
    if (!context.arguments.empty())
        return printBadArgument(context.out, context.arguments[0]);
    FlowResults results = analyzeAndApply(context);
    const Program& program = *context.program;
    validateFlowResults(program, results, context.build);
    verifyProgram(program, IRForm::Prepared, context.build, VerifierInputs { .flow = &results, .isOptimizing = true });
    printProgram(context.out, program);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
