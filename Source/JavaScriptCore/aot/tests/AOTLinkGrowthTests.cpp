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
#include "AOTProgramFile.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTTestHarness.h"
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> linkGrowthTests();

using namespace Typed;

// interface Message { type }  function clone(m) { return { ...m }; }  const m1 = { type: 1, k1: 1 }; sink(m1); clone(m1);  const m2 = { type: 1, k2: 1 }; sink(m2); clone(m2);  ..
// Every literal has a layout of its own that is written as Message, so `m` is known by the family. Each is handed out of sight, so something comes to lie beside the fields of each.
static String copyThroughFamilyOfManyLayoutsForLinkGrowthTests(unsigned numberOfLayouts)
{
    StringBuilder text;
    text.append("layout L0 name=\"Message\" module=m0 fields=[{key=\"type\" declared={type=anything} flags=[IsRequired]}] flags=[IsNeverAllocated]\n"_s);
    text.append("layout L1 name=\"Copy\" module=m0 fields=[{key=\"type\" declared={type=anything} flags=[IsRequired]}]\n"_s);
    for (unsigned layout = 0; layout < numberOfLayouts; ++layout)
        text.append("layout L"_s, layout + 2, " module=m0 fields=[{key=\"type\" declared={type=anything} flags=[IsRequired]}, {key=\"k"_s, layout, "\" declared={type=anything} flags=[IsRequired]}] supertypes=[L0]\n"_s);
    text.append("module m0 path=\"main.ts\" displayPath=\"main.ts\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Integer integer=1\n"_s);
    unsigned value = 4;
    for (unsigned layout = 0; layout < numberOfLayouts; ++layout, value += 4) {
        text.append("    v"_s, value, " = NewObject -, v3, v3 layout=L"_s, layout + 2, " fields=[0, 1]\n    v"_s, value + 1, " = FinishObject v"_s, value, '\n');
        text.append("    v"_s, value + 2, " = Call v1, -, v"_s, value + 1, " calleeName=\"sink\"\n    v"_s, value + 3, " = Call v2, -, v"_s, value + 1, " calleeName=\"clone\"\n"_s);
    }
    text.append("    Return v3\n}\n"_s);
    text.append("function f1 name=\"clone\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
    text.append("    v1 = NewObject layout=L1\n    CopyDataProperties v1, v0\n    v3 = FinishObject v1\n    Return v3\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfFactsForLinkGrowthTests(const String& text)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.facts());
        });
    });
    return instructions;
}

// A copy is told by two nodes for each layout of its source. Where it goes through every field of every layout each time, one copy costs the square of the layouts.
static void testLinkGrowsAsLayoutsOfCopiedFamilyDo()
{
    constexpr unsigned numberOfLayouts = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfFactsForLinkGrowthTests(copyThroughFamilyOfManyLayoutsForLinkGrowthTests(numberOfLayouts));
    std::optional<uint64_t> large = instructionsOfFactsForLinkGrowthTests(copyThroughFamilyOfManyLayoutsForLinkGrowthTests(4 * numberOfLayouts));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

std::span<const NativeTest> linkGrowthTests()
{
    static constexpr NativeTest tests[] {
        { "link-growth/link-grows-as-the-layouts-of-a-family-that-is-copied-do"_s, testLinkGrowsAsLayoutsOfCopiedFamilyDo },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
