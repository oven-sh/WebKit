/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinUses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTResolveBuiltinsInternal.h"
#include "AOTTypeTestSite.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

static BuiltinUseInputs inputsOf(FunctionPassContext& context, std::span<const Value> passedTests)
{
    return { context.program, context.function, context.types, context.inputs.flow, context.analyses.effectContext(), nullptr, passedTests, !context.build.options().obfuscates() };
}

void reportBuiltinUseTests(TestSiteReporter& reporter)
{
    BuiltinUseInputs inputs = inputsOf(reporter.context(), { });
    reporter.forEachCandidate([&](const Value& candidate) {
        if (!builtinUseOpcodes.contains(candidate.opcode))
            return;
        BuiltinUsePlan plan = planBuiltinUse(inputs, reporter.use(), reporter.point());
        if (!plan || plan.isProven())
            return;
        bool staysTailCall = plan.kind == BuiltinUseKind::LibraryCall && !plan.assumedResult;
        if (tailCallRequirementOf(reporter.function(), reporter.point().block, reporter.use()) && !staysTailCall)
            return;

        Vector<Value, 4> tests = createTestsOf(plan);
        bool hasFailedTest = std::ranges::any_of(tests, [&](const Value& test) {
            return reporter.findEarlierTest(test) == EarlierTest::Failed;
        });
        if (hasFailedTest)
            return;

        reporter.report(tests.span(), effectsWhenApplied(inputs, plan));
    });
}

void rewriteTestedBuiltinUse(TestSiteRewriter& rewriter)
{
    BuiltinUseInputs inputs = inputsOf(rewriter.context(), rewriter.tests());
    BuiltinUsePlan plan = planBuiltinUse(inputs, rewriter.use(), rewriter.point());
    if (!plan.isProven())
        return;

    auto insertBeforeUse = [&](auto&& value) {
        return rewriter.insertBeforeUse(WTF::move(value));
    };
    InsertBeforeCall insert = insertBeforeUse;
    applyBuiltinUse({ rewriter.context(), rewriter.use(), rewriter.point().block, insert }, plan);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
