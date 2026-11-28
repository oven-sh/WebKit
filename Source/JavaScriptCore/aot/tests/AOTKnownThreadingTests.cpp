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
#include "AOTTestHarness.h"
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> knownThreadingTests();

using namespace Typed;

enum class JoinInRowForKnownThreadingTests : uint8_t {
    FedByJoinInFront,
    ReadBehind,
};

static String joinsInRowForKnownThreadingTests(JoinInRowForKnownThreadingTests kind, unsigned numberOfJoins)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {\n  b0(v0: anything, v1: anything):\n"_s);
    text.append("    v2 = ToBoolean v0\n"_s);
    unsigned value = 3;
    unsigned block = 1;
    switch (kind) {
    case JoinInRowForKnownThreadingTests::FedByJoinInFront:
        text.append("    Branch v2 -> taken b1(), notTaken b2()\n  b1():\n    v3 = PropGet v1 key=\"k\"\n    Jump -> next b3(v3)\n  b2():\n    v4 = Boolean\n    Jump -> next b3(v4)\n"_s);
        value = 5;
        block = 3;
        for (unsigned join = 0; join < numberOfJoins; ++join) {
            text.append("  b"_s, block, "(v"_s, value, "):\n    v"_s, value + 1, " = ToBoolean v"_s, value, "\n"_s);
            text.append("    Branch v"_s, value + 1, " -> taken b"_s, block + 1, "(), notTaken b"_s, block + 2, "(v"_s, value, ")\n"_s);
            text.append("  b"_s, block + 1, "():\n    v"_s, value + 2, " = PropGet v1 key=\"k\"\n    Jump -> next b"_s, block + 2, "(v"_s, value + 2, ")\n"_s);
            value += 3;
            block += 2;
        }
        text.append("  b"_s, block, "(v"_s, value, "):\n    Return v"_s, value, "\n}\n"_s);
        break;
    case JoinInRowForKnownThreadingTests::ReadBehind:
        for (unsigned join = 0; join < numberOfJoins; ++join) {
            text.append("    Branch v2 -> taken b"_s, block, "(), notTaken b"_s, block + 1, "()\n"_s);
            text.append("  b"_s, block, "():\n    v"_s, value, " = Integer integer=1\n    Jump -> next b"_s, block + 2, "(v"_s, value, ")\n"_s);
            text.append("  b"_s, block + 1, "():\n    v"_s, value + 1, " = String string=\"a\"\n    Jump -> next b"_s, block + 2, "(v"_s, value + 1, ")\n"_s);
            text.append("  b"_s, block + 2, "(v"_s, value + 2, "):\n    v"_s, value + 3, " = TypeTest v"_s, value + 2, " testedType=number\n"_s);
            text.append("    Branch v"_s, value + 3, " -> taken b"_s, block + 3, "(), notTaken b"_s, block + 4, "()\n"_s);
            text.append("  b"_s, block + 3, "():\n    PropSet v1, v"_s, value + 2, " key=\"n\"\n    Jump -> next b"_s, block + 5, "()\n"_s);
            text.append("  b"_s, block + 4, "():\n    PropSet v1, v"_s, value + 2, " key=\"s\"\n    Jump -> next b"_s, block + 5, "()\n"_s);
            text.append("  b"_s, block + 5, "():\n"_s);
            value += 6;
            block += 6;
        }
        text.append("    Return v0\n}\n"_s);
        break;
    }
    return text.toString();
}

static std::optional<uint64_t> instructionsOfWalkForKnownThreadingTests(const String& text)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        AOT_TEST_CHECK(stages.code(CodeUnit { FunctionID(1u), CodeRole::Body }));
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.code(CodeUnit { FunctionID(1u), CodeRole::Body }));
        });
    });
    return instructions;
}

static void testWalkGrowsAsJoinsFedByJoinInFrontDo()
{
    constexpr unsigned numberOfJoins = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForKnownThreadingTests(joinsInRowForKnownThreadingTests(JoinInRowForKnownThreadingTests::FedByJoinInFront, numberOfJoins));
    std::optional<uint64_t> large = instructionsOfWalkForKnownThreadingTests(joinsInRowForKnownThreadingTests(JoinInRowForKnownThreadingTests::FedByJoinInFront, 4 * numberOfJoins));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testWalkGrowsAsJoinsWhoseValuesAreReadBehindThemDo()
{
    constexpr unsigned numberOfJoins = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForKnownThreadingTests(joinsInRowForKnownThreadingTests(JoinInRowForKnownThreadingTests::ReadBehind, numberOfJoins));
    std::optional<uint64_t> large = instructionsOfWalkForKnownThreadingTests(joinsInRowForKnownThreadingTests(JoinInRowForKnownThreadingTests::ReadBehind, 4 * numberOfJoins));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

std::span<const NativeTest> knownThreadingTests()
{
    static constexpr NativeTest tests[] {
        { "known-threading/the-walk-grows-as-the-joins-fed-by-the-join-in-front-do"_s, testWalkGrowsAsJoinsFedByJoinInFrontDo },
        { "known-threading/the-walk-grows-as-the-joins-whose-values-are-read-behind-them-do"_s, testWalkGrowsAsJoinsWhoseValuesAreReadBehindThemDo },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
