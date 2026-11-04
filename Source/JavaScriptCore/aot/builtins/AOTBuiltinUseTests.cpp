/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinUses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinPristine.h"
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

static bool callsMemberReadUnderTest(const TestSiteReporter& reporter, const Value& call)
{
    if (call.opcode != Opcode::Call || !call.calleeKey())
        return false;
    ValueID read = sameObjectThroughCasts(reporter.function(), call.calleeOperand()).valueInFrontOfFacts();
    return reporter.function().at(read).opcode == Opcode::PropGet && reporter.isClaimed(read);
}

// `f.apply(..)`: a test of the receiver was to say what the read of the member gives. Where that test has failed only the value itself tells, and the arm behind the failed test may ask it.
// Only for a function: the test asks for a closure as it was created, so a bound or a native function never passes, and its `apply` is the built-in all the same. An instance of another class fails
// its test when somebody has meddled with it, and the arm stays generic code.
static void findFailedTestsOfMemberRead(TestSiteReporter& reporter, const BuiltinUseInputs& inputs, const Value& call)
{
    if (call.opcode != Opcode::Call || !call.calleeKey())
        return;
    ValueID read = sameObjectThroughCasts(reporter.function(), call.calleeOperand()).valueInFrontOfFacts();
    if (reporter.function().at(read).opcode != Opcode::PropGet || reporter.analyses().useLists().locationOf(read).placement != ValuePlacement::InBlock)
        return;
    BuiltinUsePlan plan = planBuiltinUse(inputs, read, reporter.analyses().useLists().pointOf(read));
    if (!plan || plan.isProven())
        return;
    for (const Value& test : createTestsOf(plan)) {
        if (test.opcode == Opcode::IsPristine && pristineTestOf(reporter.program().builtins(), test.builtinClass()) == PristineTest::ClosureAsCreated)
            reporter.findEarlierTest(test);
    }
}

void reportBuiltinUseTests(TestSiteReporter& reporter)
{
    BuiltinUseInputs inputs = inputsOf(reporter.context(), { });
    inputs.identities = &reporter.context().analyses;
    reporter.forEachCandidate(builtinUseOpcodes, [&](const Value& candidate) {
        if (!builtinUseOpcodes.contains(candidate.opcode))
            return;
        BuiltinUsePlan plan = planBuiltinUse(inputs, reporter.use(), reporter.point());
        if (!plan || plan.isProven())
            return;
        bool staysTailCall = plan.kind == BuiltinUseKind::LibraryCall && !plan.assumedResult;
        if (tailCallRequirementOf(reporter.function(), reporter.point().block, reporter.use()) && !staysTailCall) {
            // Where the tests of the read pass, the callee is the built-in, and ResolveBuiltins gives the tail call up in the next round. A test for another callee would send that path to the call by value.
            if (callsMemberReadUnderTest(reporter, candidate))
                reporter.claimUse();
            return;
        }

        Vector<Value, 4> tests = createTestsOf(plan);
        bool hasFailedTest = std::ranges::any_of(tests, [&](const Value& test) {
            return reporter.findEarlierTest(test) == EarlierTest::Failed;
        });
        if (hasFailedTest)
            return;

        findFailedTestsOfMemberRead(reporter, inputs, candidate);
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
