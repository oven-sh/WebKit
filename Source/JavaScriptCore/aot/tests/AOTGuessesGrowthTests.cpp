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
#include "AOTShellProgramTestSupport.h"
#include "AOTTestHarness.h"
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> guessesGrowthTests();

using namespace Typed;

static String manyUnionsOfRecordsThatAreAlikeForGuessesGrowthTests(unsigned numberOfUnions)
{
    StringBuilder text;
    auto field = [&](ASCIILiteral key) {
        text.append("{key=\""_s, key, "\" declared={type=anything} flags=[IsRequired]}"_s);
    };
    for (unsigned number = 0; number < numberOfUnions; ++number) {
        unsigned first = 4 * number;
        text.append("layout L"_s, first, " module=m0 fields=["_s);
        field("a"_s);
        text.append(", "_s);
        field("kind"_s);
        text.append("] ownFieldCount=2\nlayout L"_s, first + 1, " module=m0 fields=["_s);
        field("kind"_s);
        text.append(", "_s);
        field("b"_s);
        text.append("] ownFieldCount=2\nlayout L"_s, first + 2, " module=m0 fields=["_s);
        field("x"_s);
        text.append(", "_s);
        field("parent"_s);
        text.append(", "_s);
        field("y"_s);
        text.append("] ownFieldCount=3\nlayout L"_s, first + 3, " module=m0 fields=["_s);
        field("p"_s);
        text.append(", "_s);
        field("q"_s);
        text.append("] ownFieldCount=2\n"_s);
    }
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Integer integer=1\n"_s);
    unsigned value = 1;
    for (unsigned number = 0; number < numberOfUnions; ++number) {
        unsigned closure = value++;
        text.append("    v"_s, closure, " = NewClosure function=f"_s, number + 1, "\n"_s);
        for (unsigned member = 0; member < 4; ++member) {
            bool hasThreeFields = member == 2;
            text.append("    v"_s, value, " = NewObject -, v0, v0"_s, hasThreeFields ? ", v0"_s : ""_s, " layout=L"_s, 4 * number + member, " fields=[0, 1"_s, hasThreeFields ? ", 2]\n"_s : "]\n"_s);
            text.append("    v"_s, value + 1, " = FinishObject v"_s, value, "\n"_s);
            text.append("    v"_s, value + 2, " = Call v"_s, closure, ", -, v"_s, value + 1, "\n"_s);
            value += 3;
        }
    }
    text.append("    Return v0\n}\n"_s);
    for (unsigned number = 0; number < numberOfUnions; ++number) {
        unsigned first = 4 * number;
        StringBuilder type;
        type.append("L"_s, first, "|L"_s, first + 1, "|L"_s, first + 2, "|L"_s, first + 3);
        String written = type.toString();
        text.append("function f"_s, number + 1, " module=m0 length=1 thisType={type=anything} parameters=[{type="_s, written, " flags=[IsExpected]}] results=[{type=anything}] {\n"_s);
        text.append("  b0(v0: "_s, written, "):\n    v1 = PropGet v0 key=\"kind\"\n    v2 = PropGet v0 key=\"parent\"\n    v3 = Add v1, v2\n    Return v3\n}\n"_s);
    }
    return text.toString();
}

static std::optional<uint64_t> instructionsOfPlanForGuessesGrowthTests(const String& text)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        AOT_TEST_CHECK(stages.facts());
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.plan());
        });
    });
    return instructions;
}

static void testPlanGrowsAsUnionsOfRecordsThatAreAlikeDo()
{
    constexpr unsigned numberOfUnions = 400;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfPlanForGuessesGrowthTests(manyUnionsOfRecordsThatAreAlikeForGuessesGrowthTests(numberOfUnions));
    std::optional<uint64_t> large = instructionsOfPlanForGuessesGrowthTests(manyUnionsOfRecordsThatAreAlikeForGuessesGrowthTests(4 * numberOfUnions));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

std::span<const NativeTest> guessesGrowthTests()
{
    static constexpr NativeTest tests[] {
        { "guesses-growth/the-plan-grows-as-the-unions-of-records-that-are-alike-do"_s, testPlanGrowsAsUnionsOfRecordsThatAreAlikeDo },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
