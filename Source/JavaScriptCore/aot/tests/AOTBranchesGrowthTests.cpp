/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTGrowthOfWork.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTTestHarness.h"
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> branchesGrowthTests();

using namespace Typed;

// function pick(v) { switch (v) { case 0: break; case 1: v.seen; break; case 2: break; .. } return v; }  sink(pick)
static String switchWithManyCasesForBranchesGrowthTests(unsigned numberOfCases)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"pick\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    Switch v0 cases=["_s);
    for (unsigned number = 0; number < numberOfCases; ++number)
        text.append(number ? ", "_s : ""_s, number);
    text.append("] -> "_s);
    for (unsigned number = 0; number < numberOfCases; ++number)
        text.append("case b"_s, number + 1, "(), "_s);
    unsigned behind = numberOfCases + 1;
    text.append("otherwise b"_s, behind, "()\n"_s);
    unsigned value = 1;
    for (unsigned number = 0; number < numberOfCases; ++number) {
        text.append("  b"_s, number + 1, "():\n"_s);
        if (number % 2)
            text.append("    v"_s, value++, " = PropGet v0 key=\"seen\"\n"_s);
        text.append("    Jump -> next b"_s, behind, "()\n"_s);
    }
    text.append("  b"_s, behind, "():\n    Return v0\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfWalkForBranchesGrowthTests(const String& text, FunctionID function)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        // The first request runs the whole stage, for what callers leave their callees.
        AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        });
    });
    return instructions;
}

// Each case is asked where it comes to, and the target of a case is found by passing those in front of it. Where that is done anew for each case, the work is the square of their number.
static void testWalkGrowsAsCasesOfSwitchDo()
{
    constexpr unsigned numberOfCases = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfWalkForBranchesGrowthTests(switchWithManyCasesForBranchesGrowthTests(numberOfCases), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForBranchesGrowthTests(switchWithManyCasesForBranchesGrowthTests(4 * numberOfCases), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

std::span<const NativeTest> branchesGrowthTests()
{
    static constexpr NativeTest tests[] {
        { "branches-growth/the-walk-grows-as-the-cases-of-a-switch-do"_s, testWalkGrowsAsCasesOfSwitchDo },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
