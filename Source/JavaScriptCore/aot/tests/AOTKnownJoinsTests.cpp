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

std::span<const NativeTest> knownJoinsTests();

using namespace Typed;

static void appendModuleForKnownJoinsTests(StringBuilder& text)
{
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
}

// function run(c) { g0 = 1; g1 = 1; ..  if (c) g0 = 1; else g0 = 2; g0;  if (c) g1 = 1; else g1 = 2; g1; .. }  sink(run)
static String joinsBehindKnownVariablesForKnownJoinsTests(unsigned numberOfJoins)
{
    StringBuilder text;
    for (unsigned variable = 0; variable < numberOfJoins; ++variable)
        text.append("variable g"_s, variable, " name=\"g"_s, variable, "\" module=m0 declared={type=anything}\n"_s);
    appendModuleForKnownJoinsTests(text);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    v1 = Integer integer=1\n    v2 = Integer integer=2\n"_s);
    for (unsigned variable = 0; variable < numberOfJoins; ++variable)
        text.append("    PutModuleVariable v1 variable=g"_s, variable, '\n');
    unsigned value = 3 + numberOfJoins;
    unsigned block = 1;
    for (unsigned join = 0; join < numberOfJoins; ++join, value += 3, block += 3) {
        text.append("    Branch v0 -> taken b"_s, block, "(), notTaken b"_s, block + 1, "()\n"_s);
        text.append("  b"_s, block, "():\n    PutModuleVariable v1 variable=g"_s, join, "\n    Jump -> next b"_s, block + 2, "()\n"_s);
        text.append("  b"_s, block + 1, "():\n    PutModuleVariable v2 variable=g"_s, join, "\n    Jump -> next b"_s, block + 2, "()\n"_s);
        text.append("  b"_s, block + 2, "():\n    v"_s, value + 2, " = GetModuleVariable variable=g"_s, join, '\n');
    }
    text.append("    Return v0\n}\n"_s);
    return text.toString();
}

// function run(c) { const a0 = [c], a1 = [c], ..;  (c ? { x: 1 } : { x: 2 }).x; (c ? { x: 1 } : { x: 2 }).x; ..  c(a0); c(a1); .. }  sink(run)
static String joinsOfObjectsUnderLiveValuesForKnownJoinsTests(unsigned numberOfJoins)
{
    StringBuilder text;
    text.append("layout L0 module=m0 fields=[{key=\"x\" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1\n"_s);
    appendModuleForKnownJoinsTests(text);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    v1 = Integer integer=1\n    v2 = Integer integer=2\n"_s);
    for (unsigned live = 0; live < numberOfJoins; ++live)
        text.append("    v"_s, 3 + live, " = NewArray v0 contents=anything\n"_s);
    unsigned value = 3 + numberOfJoins;
    unsigned block = 1;
    for (unsigned join = 0; join < numberOfJoins; ++join, value += 6, block += 3) {
        text.append("    Branch v0 -> taken b"_s, block, "(), notTaken b"_s, block + 1, "()\n"_s);
        text.append("  b"_s, block, "():\n    v"_s, value, " = NewObject -, v1 layout=L0 fields=[0]\n    v"_s, value + 1, " = FinishObject v"_s, value, "\n    Jump -> next b"_s, block + 2, "(v"_s, value + 1, ")\n"_s);
        text.append("  b"_s, block + 1, "():\n    v"_s, value + 2, " = NewObject -, v2 layout=L0 fields=[0]\n    v"_s, value + 3, " = FinishObject v"_s, value + 2, "\n    Jump -> next b"_s, block + 2, "(v"_s, value + 3, ")\n"_s);
        text.append("  b"_s, block + 2, "(v"_s, value + 4, "):\n    v"_s, value + 5, " = PropGet v"_s, value + 4, " key=\"x\"\n"_s);
    }
    for (unsigned live = 0; live < numberOfJoins; ++live, ++value)
        text.append("    v"_s, value, " = Call v0, -, v"_s, 3 + live, '\n');
    text.append("    Return v0\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfWalkForKnownJoinsTests(const String& text)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        // The first request runs the whole stage.
        AOT_TEST_CHECK(stages.code(CodeUnit { FunctionID(1u), CodeRole::Body }));
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.code(CodeUnit { FunctionID(1u), CodeRole::Body }));
        });
    });
    return instructions;
}

// A join looks at the places that its ways do not share. Where it goes through all that either state holds, the work is the square of the number of joins.
static void testWalkGrowsAsJoinsBehindKnownVariablesDo()
{
    constexpr unsigned numberOfJoins = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfWalkForKnownJoinsTests(joinsBehindKnownVariablesForKnownJoinsTests(numberOfJoins));
    std::optional<uint64_t> large = instructionsOfWalkForKnownJoinsTests(joinsBehindKnownVariablesForKnownJoinsTests(4 * numberOfJoins));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// Whether an object has another name behind the join is told by where it was made. Where the join goes through the values that are live at its head, the work is the square of the number of joins.
static void testWalkGrowsAsJoinsOfObjectsUnderLiveValuesDo()
{
    constexpr unsigned numberOfJoins = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfWalkForKnownJoinsTests(joinsOfObjectsUnderLiveValuesForKnownJoinsTests(numberOfJoins));
    std::optional<uint64_t> large = instructionsOfWalkForKnownJoinsTests(joinsOfObjectsUnderLiveValuesForKnownJoinsTests(4 * numberOfJoins));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

std::span<const NativeTest> knownJoinsTests()
{
    static constexpr NativeTest tests[] {
        { "known-joins/the-walk-grows-as-the-joins-behind-known-variables-do"_s, testWalkGrowsAsJoinsBehindKnownVariablesDo },
        { "known-joins/the-walk-grows-as-the-joins-of-objects-under-live-values-do"_s, testWalkGrowsAsJoinsOfObjectsUnderLiveValuesDo },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
