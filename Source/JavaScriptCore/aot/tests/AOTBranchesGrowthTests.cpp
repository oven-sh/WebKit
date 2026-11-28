/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTGrowthOfWork.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTOptionWords.h"
#include "AOTProgramFile.h"
#include "AOTTestHarness.h"
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> branchesGrowthTests();

using namespace Typed;

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

static String wordsHandedOnRoundLoopWithTwoWaysRoundForBranchesGrowthTests(unsigned numberOfWords)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"spin\" module=m0 length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {\n  b0(v0: anything, v1: anything):\n"_s);
    unsigned value = 2;
    for (unsigned word = 0; word < numberOfWords; ++word) {
        text.append("    v"_s, value, " = Integer integer="_s, word, "\n    v"_s, value + 1, " = ElemGet v0, v"_s, value, "\n"_s);
        value += 2;
    }
    unsigned zero = value++;
    unsigned one = value++;
    text.append("    v"_s, zero, " = Integer integer=0\n    v"_s, one, " = Integer integer=1\n    Jump -> next b1("_s);
    for (unsigned word = 0; word < numberOfWords; ++word)
        text.append("v"_s, 3 + 2 * word, ", "_s);
    text.append("v"_s, zero, ")\n  b1("_s);
    unsigned firstWord = value;
    for (unsigned word = 0; word <= numberOfWords; ++word)
        text.append(word ? ", "_s : ""_s, "v"_s, value++, ": anything"_s);
    unsigned turn = firstWord + numberOfWords;
    auto handOn = [&](unsigned newest, unsigned nextTurn) {
        text.append("    Jump -> next b1(v"_s, newest);
        for (unsigned word = 0; word + 1 < numberOfWords; ++word)
            text.append(", v"_s, firstWord + word);
        text.append(", v"_s, nextTurn, ")\n"_s);
    };
    ++value;
    text.append("):\n    LoopHint\n    v"_s, value, " = Less v"_s, turn, ", v1\n    Branch v"_s, value, " -> taken b2(), notTaken b5()\n"_s);
    ++value;
    unsigned nextTurn = value + 1;
    text.append("  b2():\n    v"_s, value, " = Add v"_s, turn, ", v"_s, one, "\n    v"_s, nextTurn, " = BitOr v"_s, value, ", v"_s, zero, "\n"_s);
    text.append("    v"_s, value + 2, " = BitAnd v"_s, nextTurn, ", v"_s, one, "\n    v"_s, value + 3, " = StrictEq v"_s, value + 2, ", v"_s, zero, "\n    Branch v"_s, value + 3, " -> taken b4(), notTaken b3()\n"_s);
    value += 4;
    text.append("  b3():\n    v"_s, value, " = BitXor v"_s, firstWord, ", v"_s, nextTurn, "\n"_s);
    handOn(value++, nextTurn);
    text.append("  b4():\n    v"_s, value, " = Add v"_s, firstWord, ", v"_s, nextTurn, "\n    v"_s, value + 1, " = BitOr v"_s, value, ", v"_s, zero, "\n"_s);
    handOn(value + 1, nextTurn);
    value += 2;
    text.append("  b5():\n"_s);
    unsigned mixed = firstWord;
    for (unsigned word = 1; word < numberOfWords; ++word) {
        text.append("    v"_s, value, " = BitXor v"_s, mixed, ", v"_s, firstWord + word, "\n"_s);
        mixed = value++;
    }
    text.append("    Return v"_s, mixed, "\n}\n"_s);
    return text.toString();
}

static String freshCellsThatLiveOverJoinsForBranchesGrowthTests(unsigned numberOfCells)
{
    StringBuilder text;
    text.append("layout L0 module=m0 fields=[{key=\"lid\" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"big\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
    for (unsigned cell = 0; cell < numberOfCells; ++cell) {
        unsigned value = 1 + 3 * cell;
        unsigned block = 3 * cell;
        if (cell)
            text.append("  b"_s, block, "():\n"_s);
        text.append("    v"_s, value, " = NewObject -, v0 layout=L0 fields=[0]\n    v"_s, value + 1, " = FinishObject v"_s, value, "\n    v"_s, value + 2, " = ToBoolean v0\n"_s);
        text.append("    Branch v"_s, value + 2, " -> taken b"_s, block + 1, "(), notTaken b"_s, block + 2, "()\n"_s);
        text.append("  b"_s, block + 1, "():\n    Jump -> next b"_s, block + 3, "()\n  b"_s, block + 2, "():\n    Jump -> next b"_s, block + 3, "()\n"_s);
    }
    text.append("  b"_s, 3 * numberOfCells, "():\n"_s);
    for (unsigned cell = 0; cell < numberOfCells; ++cell)
        text.append("    v"_s, 1 + 3 * numberOfCells + cell, " = PropGet v"_s, 2 + 3 * cell, " key=\"lid\"\n"_s);
    text.append("    Return v0\n}\n"_s);
    return text.toString();
}

static String branchesWhoseWaysHandOnWhatNextTurnReadsForBranchesGrowthTests(unsigned numberOfBranches)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"turns\" module=m0 length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {\n  b0(v0: anything, v1: anything):\n    Jump -> next b1("_s);
    for (unsigned branch = 0; branch < numberOfBranches; ++branch)
        text.append(branch ? ", v0"_s : "v0"_s);
    text.append(")\n  b1("_s);
    unsigned firstWord = 2;
    for (unsigned branch = 0; branch < numberOfBranches; ++branch)
        text.append(branch ? ", "_s : ""_s, "v"_s, firstWord + branch, ": anything"_s);
    unsigned value = firstWord + numberOfBranches + 1;
    unsigned firstAnswer = value;
    text.append("):\n    LoopHint\n"_s);
    for (unsigned branch = 0; branch < numberOfBranches; ++branch)
        text.append("    v"_s, value++, " = Call v0, -, v"_s, firstWord + branch, "\n"_s);
    text.append("    v"_s, value, " = ToBoolean v"_s, firstAnswer, "\n    Branch v"_s, value, " -> taken b2(), notTaken b"_s, 3 + 3 * numberOfBranches, "()\n  b2():\n"_s);
    ++value;
    unsigned firstArrival = value + 1;
    for (unsigned branch = 0; branch < numberOfBranches; ++branch) {
        unsigned block = 3 + 3 * branch;
        text.append("    v"_s, value, " = ToBoolean v"_s, firstAnswer + branch, "\n    Branch v"_s, value, " -> taken b"_s, block, "(), notTaken b"_s, block + 1, "()\n"_s);
        text.append("  b"_s, block, "():\n    Jump -> next b"_s, block + 2, "(v0)\n  b"_s, block + 1, "():\n    Jump -> next b"_s, block + 2, "(v1)\n"_s);
        text.append("  b"_s, block + 2, "(v"_s, value + 1, ": anything):\n"_s);
        value += 2;
    }
    text.append("    Jump -> next b1("_s);
    for (unsigned branch = 0; branch < numberOfBranches; ++branch)
        text.append(branch ? ", "_s : ""_s, "v"_s, firstArrival + 2 * branch);
    text.append(")\n  b"_s, 3 + 3 * numberOfBranches, "():\n    Return v0\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfWalkForBranchesGrowthTests(const String& text, FunctionID function, Wire::CompileOptions options = makeOptionsOfTestedProgram())
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(options);
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

static void testWalkGrowsAsCasesOfSwitchDo()
{
    constexpr unsigned numberOfCases = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForBranchesGrowthTests(switchWithManyCasesForBranchesGrowthTests(numberOfCases), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForBranchesGrowthTests(switchWithManyCasesForBranchesGrowthTests(4 * numberOfCases), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testWalkGrowsAsWordsHandedOnRoundLoopWithTwoWaysRoundDo()
{
    constexpr unsigned numberOfWords = 5;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForBranchesGrowthTests(wordsHandedOnRoundLoopWithTwoWaysRoundForBranchesGrowthTests(numberOfWords), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForBranchesGrowthTests(wordsHandedOnRoundLoopWithTwoWaysRoundForBranchesGrowthTests(4 * numberOfWords), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testWalkThatEndsLivesGrowsAsFreshCellsThatLiveOverJoinsDo()
{
    constexpr unsigned numberOfCells = 100;
    Wire::CompileOptions options = makeOptionsOfTestedProgram();
    AOT_TEST_CHECK(!applyOptionWord(options, "testing=CheckProofsAtRunTime"_s));
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForBranchesGrowthTests(freshCellsThatLiveOverJoinsForBranchesGrowthTests(numberOfCells), FunctionID(1u), options);
    std::optional<uint64_t> large = instructionsOfWalkForBranchesGrowthTests(freshCellsThatLiveOverJoinsForBranchesGrowthTests(4 * numberOfCells), FunctionID(1u), options);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testWalkGrowsAsBranchesWhoseWaysHandOnWhatNextTurnReadsDo()
{
    constexpr unsigned numberOfBranches = 100;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForBranchesGrowthTests(branchesWhoseWaysHandOnWhatNextTurnReadsForBranchesGrowthTests(numberOfBranches), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForBranchesGrowthTests(branchesWhoseWaysHandOnWhatNextTurnReadsForBranchesGrowthTests(4 * numberOfBranches), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

std::span<const NativeTest> branchesGrowthTests()
{
    static constexpr NativeTest tests[] {
        { "branches-growth/the-walk-grows-as-the-cases-of-a-switch-do"_s, testWalkGrowsAsCasesOfSwitchDo },
        { "branches-growth/the-walk-grows-as-the-words-handed-on-round-a-loop-with-two-ways-round-do"_s, testWalkGrowsAsWordsHandedOnRoundLoopWithTwoWaysRoundDo },
        { "branches-growth/the-walk-that-ends-lives-grows-as-the-fresh-cells-that-live-over-joins-do"_s, testWalkThatEndsLivesGrowsAsFreshCellsThatLiveOverJoinsDo },
        { "branches-growth/the-walk-grows-as-the-branches-whose-ways-hand-on-what-the-next-turn-reads-do"_s, testWalkGrowsAsBranchesWhoseWaysHandOnWhatNextTurnReadsDo },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
