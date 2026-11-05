/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTConvention.h"
#include "AOTFacts.h"
#include "AOTGrowthOfWork.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTPlan.h"
#include "AOTProgramFile.h"
#include "AOTStackTestPlan.h"
#include "AOTTailCallPlan.h"
#include "AOTTestHarness.h"
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> stackTestPlanTests();

using namespace Typed;

enum class CallInChainForStackTestPlanTests : uint8_t {
    ComesBack,
    InTailPosition,
};

static void appendHeadForStackTestPlanTests(StringBuilder& text, unsigned numberOfFunctions)
{
    for (unsigned function = 1; function <= numberOfFunctions; ++function)
        text.append("variable g"_s, function - 1, " name=\"c"_s, function, "\" module=m0 declared={type=f"_s, function, " flags=[IsFromCompiler]} initialization=Function function=f"_s, function, " flags=[IsImmutable]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
}

static void appendHeadOfFunctionForStackTestPlanTests(StringBuilder& text, unsigned function)
{
    text.append("function f"_s, function, " name=\"c"_s, function, "\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
}

static String chainOfCallsForStackTestPlanTests(CallInChainForStackTestPlanTests kind, unsigned numberOfFunctions)
{
    StringBuilder text;
    appendHeadForStackTestPlanTests(text, numberOfFunctions);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    for (unsigned function = 1; function < numberOfFunctions; ++function) {
        appendHeadOfFunctionForStackTestPlanTests(text, function);
        text.append("    v1 = GetModuleVariable variable=g"_s, function, "\n    v2 = Call v1, -, v0 calleeName=\"c"_s, function + 1, "\"\n"_s);
        switch (kind) {
        case CallInChainForStackTestPlanTests::ComesBack:
            text.append("    v3 = PropGet v2 key=\"x\"\n    Return v3\n}\n"_s);
            break;
        case CallInChainForStackTestPlanTests::InTailPosition:
            text.append("    ReturnCall v2\n}\n"_s);
            break;
        }
    }
    appendHeadOfFunctionForStackTestPlanTests(text, numberOfFunctions);
    text.append("    v1 = PropGet v0 key=\"x\"\n    Return v1\n}\n"_s);
    return text.toString();
}

static void withPlanForStackTestPlanTests(const String& text, NOESCAPE const Invocable<void(const Program&, const Facts&, const Plan&)> auto& functor)
{
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        AOT_TEST_CHECK(stages.facts());
        AOT_TEST_CHECK(stages.plan());
        functor(stages.program(), *stages.facts(), *stages.plan());
    });
}

static void testEveryFunctionOfChainOfTailCallsEnds()
{
    constexpr unsigned numberOfFunctions = 40;
    withPlanForStackTestPlanTests(chainOfCallsForStackTestPlanTests(CallInChainForStackTestPlanTests::InTailPosition, numberOfFunctions), [&](const Program&, const Facts&, const Plan& plan) {
        for (unsigned function = 1; function <= numberOfFunctions; ++function)
            AOT_TEST_CHECK(plan.endsItsTailCalls(FunctionID(function)));
    });
}

std::span<const NativeTest> stackTestPlanTests()
{
    static constexpr NativeTest tests[] {
        { "stack-test-plan/every-function-of-a-chain-of-tail-calls-ends"_s, testEveryFunctionOfChainOfTailCallsEnds },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
