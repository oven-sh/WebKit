/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTGrowthOfWork.h"
#include "AOTIRParser.h"
#include "AOTLinkTables.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTTestHarness.h"
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> knownEffectsTests();

using namespace Typed;

// const pick = flag ? f1 : f2 ..; function caller() { pick(); pick(); .. }: every call may enter every lost closure.
static String callsOfLostClosuresForKnownEffectsTests(unsigned size)
{
    StringBuilder text;
    text.append("variable g0 name=\"pick\" module=m0 declared={type=anything}\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n"_s);
    unsigned value = 0;
    for (unsigned function = 0; function < size; ++function, value += 2) {
        text.append("    v"_s, value, " = NewClosure function=f"_s, function + 2, '\n');
        text.append("    PutModuleVariable v"_s, value, " variable=g0\n"_s);
    }
    text.append("    v"_s, value, " = NewClosure function=f1\n    v"_s, value + 1, " = Call v"_s, value, "\n    Return v"_s, value + 1, "\n}\n"_s);
    text.append("function f1 name=\"caller\" module=m0 thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = GetModuleVariable variable=g0\n"_s);
    for (unsigned call = 0; call < size; ++call)
        text.append("    v"_s, call + 1, " = Call v0\n"_s);
    text.append("    v"_s, size + 1, " = Undefined\n    Return v"_s, size + 1, "\n}\n"_s);
    for (unsigned function = 0; function < size; ++function) {
        text.append("function f"_s, function + 2, " name=\"one\" module=m0 thisType={type=anything} results=[{type=anything}] {\n"_s);
        text.append("  b0():\n    v0 = Integer integer=1\n    Return v0\n}\n"_s);
    }
    return text.toString();
}

static std::optional<uint64_t> instructionsOfFactsForKnownEffectsTests(const String& text, size_t& numberOfEffectsKept)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build, allMechanisms);
        const Facts* facts = nullptr;
        instructions = instructionsOfAllThreadsIn([&] {
            facts = stages.facts();
        });
        AOT_TEST_CHECK(facts);
        numberOfEffectsKept = facts->linked().known.effectsOfCallsOfSeveral.size();
    });
    return instructions;
}

// As many calls as there are functions that each may enter. A list of callees for each call, or a go over them, is the square.
static void testCallsOfSeveralGrowAsCallsAndCalleesDo()
{
    constexpr unsigned size = 1500;
    InstructionCounts::begin();
    size_t keptBySmall = 0;
    size_t keptByLarge = 0;
    std::optional<uint64_t> small = instructionsOfFactsForKnownEffectsTests(callsOfLostClosuresForKnownEffectsTests(size), keptBySmall);
    std::optional<uint64_t> large = instructionsOfFactsForKnownEffectsTests(callsOfLostClosuresForKnownEffectsTests(4 * size), keptByLarge);
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
    // Calls that enter the lost closures and nothing else share what they do.
    AOT_TEST_CHECK(keptBySmall == 2 && keptByLarge == 2);
}

std::span<const NativeTest> knownEffectsTests()
{
    static constexpr NativeTest tests[] {
        { "known-effects/the-calls-of-several-grow-as-calls-and-callees-do"_s, testCallsOfSeveralGrowAsCallsAndCalleesDo },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
