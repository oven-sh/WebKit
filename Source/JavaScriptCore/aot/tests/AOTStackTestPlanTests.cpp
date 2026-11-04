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
    // function c1(v) { return c2(v).x }
    ComesBack,
    // function c1(v) { return c2(v) }
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

// sink(c1)  function c1(v) { .. c2(v) .. }  function c2(v) { .. c3(v) .. }  ..  function cN(v) { return v.x }
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

// The first half of the functions each call every function of the other half. The text grows as the square of the number.
static String everyCallerCallsEveryCalleeForStackTestPlanTests(unsigned numberOfCallers)
{
    StringBuilder text;
    appendHeadForStackTestPlanTests(text, 2 * numberOfCallers);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    for (unsigned caller = 1; caller <= numberOfCallers; ++caller)
        text.append("    v"_s, 2 * caller, " = NewClosure function=f"_s, caller, "\n    v"_s, 2 * caller + 1, " = Call v1, -, v"_s, 2 * caller, '\n');
    text.append("    Return v1\n}\n"_s);
    for (unsigned caller = 1; caller <= numberOfCallers; ++caller) {
        appendHeadOfFunctionForStackTestPlanTests(text, caller);
        for (unsigned callee = 1; callee <= numberOfCallers; ++callee)
            text.append("    v"_s, 2 * callee - 1, " = GetModuleVariable variable=g"_s, numberOfCallers + callee - 1, "\n    v"_s, 2 * callee, " = Call v"_s, 2 * callee - 1, ", -, v0 calleeName=\"c"_s, numberOfCallers + callee, "\"\n"_s);
        text.append("    Return v0\n}\n"_s);
    }
    for (unsigned callee = numberOfCallers + 1; callee <= 2 * numberOfCallers; ++callee) {
        appendHeadOfFunctionForStackTestPlanTests(text, callee);
        text.append("    v1 = PropGet v0 key=\"x\"\n    Return v1\n}\n"_s);
    }
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

// The two questions by themselves, over a plan that is whole: what else the plan does with so many functions is not asked about.
static std::optional<uint64_t> instructionsOfBothPlansForStackTestPlanTests(const String& text)
{
    std::optional<uint64_t> instructions;
    withPlanForStackTestPlanTests(text, [&](const Program& program, const Facts& facts, const Plan& plan) {
        instructions = instructionsOfAllThreadsIn([&] {
            StackTestPlan tests = planStackTests(program, facts, plan);
            BitVector functionsThatEnd = findFunctionsThatEndTheirTailCalls(program, facts);
            AOT_TEST_CHECK(tests.coveredFramesOfCall.size() == program.functions.size());
            AOT_TEST_CHECK(functionsThatEnd.size() >= program.functions.size());
        });
    });
    return instructions;
}

static void testPlansGrowAsChainOfCallsDoes()
{
    constexpr unsigned numberOfFunctions = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfBothPlansForStackTestPlanTests(chainOfCallsForStackTestPlanTests(CallInChainForStackTestPlanTests::ComesBack, numberOfFunctions));
    std::optional<uint64_t> large = instructionsOfBothPlansForStackTestPlanTests(chainOfCallsForStackTestPlanTests(CallInChainForStackTestPlanTests::ComesBack, 4 * numberOfFunctions));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testPlansGrowAsChainOfTailCallsDoes()
{
    constexpr unsigned numberOfFunctions = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfBothPlansForStackTestPlanTests(chainOfCallsForStackTestPlanTests(CallInChainForStackTestPlanTests::InTailPosition, numberOfFunctions));
    std::optional<uint64_t> large = instructionsOfBothPlansForStackTestPlanTests(chainOfCallsForStackTestPlanTests(CallInChainForStackTestPlanTests::InTailPosition, 4 * numberOfFunctions));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

// Twice the callers and twice the callees are four times the calls.
static void testPlansGrowAsCallsOfEveryCalleeByEveryCallerDo()
{
    constexpr unsigned numberOfCallers = 60;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfBothPlansForStackTestPlanTests(everyCallerCallsEveryCalleeForStackTestPlanTests(numberOfCallers));
    std::optional<uint64_t> large = instructionsOfBothPlansForStackTestPlanTests(everyCallerCallsEveryCalleeForStackTestPlanTests(2 * numberOfCallers));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

// Below a test stand at most maxNestedFunctionsWithoutStackCheck frames that make none, however long the chain is.
static void testNoMoreFramesAreCoveredThanThereIsRoomFor()
{
    constexpr unsigned numberOfFunctions = 40;
    withPlanForStackTestPlanTests(chainOfCallsForStackTestPlanTests(CallInChainForStackTestPlanTests::ComesBack, numberOfFunctions), [&](const Program&, const Facts&, const Plan& plan) {
        const StackTestPlan& tests = plan.conventions().stackTests();
        for (unsigned function = 1; function <= numberOfFunctions; ++function) {
            unsigned framesOfCall = numberOfFunctions - function + 1;
            bool isCovered = plan.conventions().stackCheckOf({ FunctionID(function), CodeRole::Body }) == StackCheck::CoveredByCallers;
            AOT_TEST_CHECK(isCovered == (framesOfCall <= maxNestedFunctionsWithoutStackCheck));
            AOT_TEST_CHECK(tests.coveredFramesOfCall[function] == std::min<unsigned>(framesOfCall, maxNestedFunctionsWithoutStackCheck));
        }
    });
}

static void testEveryFunctionOfChainOfTailCallsEnds()
{
    constexpr unsigned numberOfFunctions = 40;
    withPlanForStackTestPlanTests(chainOfCallsForStackTestPlanTests(CallInChainForStackTestPlanTests::InTailPosition, numberOfFunctions), [&](const Program& program, const Facts& facts, const Plan& plan) {
        BitVector functionsThatEnd = findFunctionsThatEndTheirTailCalls(program, facts);
        for (unsigned function = 1; function <= numberOfFunctions; ++function) {
            AOT_TEST_CHECK(plan.endsItsTailCalls(FunctionID(function)));
            AOT_TEST_CHECK(makesOnlyTailCallsThatEnd(program, facts, functionsThatEnd, FunctionID(function)));
            AOT_TEST_CHECK(!callsItselfInTailPosition(facts, FunctionID(function)));
        }
    });
}

std::span<const NativeTest> stackTestPlanTests()
{
    static constexpr NativeTest tests[] {
        { "stack-test-plan/the-plans-grow-as-a-chain-of-calls-does"_s, testPlansGrowAsChainOfCallsDoes },
        { "stack-test-plan/the-plans-grow-as-a-chain-of-tail-calls-does"_s, testPlansGrowAsChainOfTailCallsDoes },
        { "stack-test-plan/the-plans-grow-as-the-calls-of-every-callee-by-every-caller-do"_s, testPlansGrowAsCallsOfEveryCalleeByEveryCallerDo },
        { "stack-test-plan/no-more-frames-are-covered-than-there-is-room-for"_s, testNoMoreFramesAreCoveredThanThereIsRoomFor },
        { "stack-test-plan/every-function-of-a-chain-of-tail-calls-ends"_s, testEveryFunctionOfChainOfTailCallsEnds },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
