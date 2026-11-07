/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTGrowthOfWork.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTPlan.h"
#include "AOTProgramFile.h"
#include "AOTRemovableFeature.h"
#include "AOTTestHarness.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> stringStraysTests();

using namespace Typed;

static String valuesThatClimbLadderForStringStraysTests(unsigned numberOfValues)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Box\" module=m0 constructor=f2 fields=[{key=\"width\" declared={type=anything} flags=[IsRequired]}]\n"_s);
    text.append("layout L1 module=m0 fields=[{key=\"width\" declared={type=anything} flags=[IsRequired]}]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = NewClosure function=f2\n    v4 = NewClass v3 layout=L0\n    v5 = FinishClass v4 layout=L0\n    v6 = Construct v5\n    v7 = Integer integer=1\n"_s);
    text.append("    v8 = NewObject -, v7 layout=L1 fields=[0]\n    v9 = FinishObject v8\n    v10 = Call v1, -, v2, v6, v9\n    Return v10\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
    for (unsigned value = 0; value < numberOfValues; ++value) {
        unsigned first = 1 + 3 * value;
        text.append("    v"_s, first, " = PropGet v0 key=\"next\"\n    v"_s, first + 1, " = PropGet v"_s, first, " key=\"width\"\n    PropSet v"_s, first, ", v"_s, first + 1, " key=\"tag\"\n"_s);
    }
    text.append("    Return v0\n}\n"_s);
    text.append("function f2 name=\"Box\" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {\n"_s);
    text.append("  b0(v0: anything):\n    v1 = GetConstructPrototype v0 layout=L0\n    v2 = NewObject v1 layout=L0\n    v3 = Integer integer=1\n    PropSet v2, v3 key=\"width\"\n    Return v2\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfWalkForStringStraysTests(const String& text, FunctionID function)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        });
    });
    return instructions;
}

static void testWalkGrowsAsValuesThatClimbLadderDo()
{
    constexpr unsigned numberOfValues = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForStringStraysTests(valuesThatClimbLadderForStringStraysTests(numberOfValues), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForStringStraysTests(valuesThatClimbLadderForStringStraysTests(4 * numberOfValues), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

std::span<const NativeTest> stringStraysTests()
{
    static constexpr NativeTest tests[] {
        { "string-strays/the-walk-grows-as-the-values-that-climb-the-ladder-of-names-do"_s, testWalkGrowsAsValuesThatClimbLadderDo },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
