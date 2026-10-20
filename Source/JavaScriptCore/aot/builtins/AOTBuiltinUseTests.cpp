/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinUses.h"

#if ENABLE(AOT)

#include "AOTResolveBuiltinsInternal.h"
#include "AOTTypeTestSite.h"
#include <algorithm>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// The rewrite may ask no analysis, so the report asks none either: both make the same plan, and the planner adopts an allocation as the proof of a reported test.
static BuiltinUseInputs inputsOf(FunctionPassContext& context, std::span<const Value> passedTests)
{
    return { context.program, context.function, context.types, context.inputs.flow, context.analyses.effectContext(), nullptr, passedTests };
}

void reportBuiltinUseTests(TestSiteReporter& reporter)
{
    BuiltinUseInputs inputs = inputsOf(reporter.context(), { });
    reporter.forEachCandidate([&](const Value& candidate) {
        if (!builtinUseOpcodes.contains(candidate.opcode))
            return;
        // A CallBuiltin is no tail call, and the rewrite runs while insertions are queued, when a ReturnCall can no longer be demoted.
        if (tailCallRequirementOf(reporter.function(), reporter.point().block, reporter.use()))
            return;

        BuiltinUsePlan plan = planBuiltinUse(inputs, reporter.use(), reporter.point());
        if (!plan || plan.isProven())
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
    RELEASE_ASSERT(plan.isProven());

    // A ScopedLambda hands its functor each argument as an lvalue.
    auto insertBeforeUse = [&](auto&& value) {
        return rewriter.insertBeforeUse(WTF::move(value));
    };
    InsertBeforeCall insert = insertBeforeUse;
    applyBuiltinUse({ rewriter.context(), rewriter.use(), rewriter.point().block, insert }, plan);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
