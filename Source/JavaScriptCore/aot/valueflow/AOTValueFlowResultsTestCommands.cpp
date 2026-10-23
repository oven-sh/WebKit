/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTIRPrinter.h"
#include "AOTSort.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowResults.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static FlowResults analyzeAndApply(TestCommandContext& context, const ListenerForSetsOutsideTypes* setsOutsideTypes = nullptr)
{
    FlowResults results = analyzeValueFlow(*context.program, context.program->types, context.build, setsOutsideTypes);
    applyFlowResults(*context.program, results, context.build);
    context.program->setForm(IRForm::Prepared);
    return results;
}

// valueflow-results: the tables that the stage AnalyzeFlow hands on, then the sets that name an object outside the type of the place where it arrives.
void runValueFlowResultsTestCommand(TestCommandContext& context)
{
    for (const String& word : context.arguments) {
        if (word != "unguarded"_s)
            return printBadArgument(context.out, word);
    }
    prepareForFlow(context);
    Vector<String> setsOutsideTypes;
    auto describe = [&](const String& place, const Type& held, const Type& declared) {
        StringPrintStream line;
        line.print(place, " holds "_s);
        printType(line, *context.program, held);
        line.print(", declared "_s);
        printType(line, *context.program, declared);
        setsOutsideTypes.append(line.toString());
    };
    ListenerForSetsOutsideTypes listener(describe);
    analyzeAndApply(context, &listener).dump(context.out, *context.program);
    AOT::sort(setsOutsideTypes, codePointCompareLessThan);
    for (const String& line : setsOutsideTypes)
        context.out.println("outside-type "_s, line);
}

// valueflow-apply [split]: the program with the tables applied, validated and verified. split: see prepareAsInFrontOfFlow().
void runValueFlowApplyTestCommand(TestCommandContext& context)
{
    for (const String& word : context.arguments) {
        if (word != "split"_s && word != "unguarded"_s)
            return printBadArgument(context.out, word);
    }
    prepareForFlow(context);
    FlowResults results = analyzeAndApply(context);
    const Program& program = *context.program;
    verifyProgram(program, IRForm::Prepared, context.build, VerifierInputs { .flow = &results, .isOptimizing = true });
    printProgram(context.out, program);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
