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

// ---- Features by the condition of their line. Of the engine's own table one line has a condition on an operand: Intl.supportedValuesOf(key) reads the data of collations where the key is "collation".

static bool usesFeatureForStringStraysTests(ASCIILiteral operations, RemovableFeature feature)
{
    bool uses = false;
    runOnBuildThread([&] {
        String text = makeString("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\nfunction f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=IntlSupportedValuesOf\n"_s, operations, "}\n"_s);
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        const Facts* facts = stages.facts();
        AOT_TEST_CHECK(facts);
        uses = std::ranges::contains(facts->featureUses(), static_cast<uint32_t>(feature), &FeatureUseRecord::feature);
    });
    return uses;
}

static constexpr ASCIILiteral asksForCalendarsForStringStraysTests = "    v1 = String string=\"calendar\"\n    v2 = Call v0, -, v1\n    Return v2\n"_s;

static void testLineIsLeftOutWhereNoCallInSightMayFulfilIt()
{
    // Intl.supportedValuesOf("calendar")
    AOT_TEST_CHECK(usesFeatureForStringStraysTests(asksForCalendarsForStringStraysTests, RemovableFeature::IntlOther));
    AOT_TEST_CHECK(!usesFeatureForStringStraysTests(asksForCalendarsForStringStraysTests, RemovableFeature::IntlCollation));
    // Intl.supportedValuesOf("cal" + key)
    AOT_TEST_CHECK(!usesFeatureForStringStraysTests("    v1 = Builtin builtin=GlobalThis\n    v2 = PropGet v1 key=\"key\" [isFreeName]\n    v3 = String string=\"cal\"\n    v4 = Add v3, v2\n    v5 = Call v0, -, v4\n    Return v5\n"_s, RemovableFeature::IntlCollation));
}

static void testLineHoldsWhereCallInSightMayFulfilIt()
{
    // Intl.supportedValuesOf("collation")
    AOT_TEST_CHECK(usesFeatureForStringStraysTests("    v1 = String string=\"collation\"\n    v2 = Call v0, -, v1\n    Return v2\n"_s, RemovableFeature::IntlCollation));
    // Intl.supportedValuesOf("calendar"); Intl.supportedValuesOf("collation")
    AOT_TEST_CHECK(usesFeatureForStringStraysTests("    v1 = String string=\"calendar\"\n    v2 = Call v0, -, v1\n    v3 = String string=\"collation\"\n    v4 = Call v0, -, v3\n    Return v4\n"_s, RemovableFeature::IntlCollation));
    // Intl.supportedValuesOf(key)
    AOT_TEST_CHECK(usesFeatureForStringStraysTests("    v1 = Builtin builtin=GlobalThis\n    v2 = PropGet v1 key=\"key\" [isFreeName]\n    v3 = Call v0, -, v2\n    Return v3\n"_s, RemovableFeature::IntlCollation));
    // Intl.supportedValuesOf("col" + key)
    AOT_TEST_CHECK(usesFeatureForStringStraysTests("    v1 = Builtin builtin=GlobalThis\n    v2 = PropGet v1 key=\"key\" [isFreeName]\n    v3 = String string=\"col\"\n    v4 = Add v3, v2\n    v5 = Call v0, -, v4\n    Return v5\n"_s, RemovableFeature::IntlCollation));
    // Intl.supportedValuesOf(key + "calendar")
    AOT_TEST_CHECK(usesFeatureForStringStraysTests("    v1 = Builtin builtin=GlobalThis\n    v2 = PropGet v1 key=\"key\" [isFreeName]\n    v3 = String string=\"calendar\"\n    v4 = Add v2, v3\n    v5 = Call v0, -, v4\n    Return v5\n"_s, RemovableFeature::IntlCollation));
    // Intl.supportedValuesOf(): native code may have a text of its own for what is left out.
    AOT_TEST_CHECK(usesFeatureForStringStraysTests("    v1 = Call v0\n    Return v1\n"_s, RemovableFeature::IntlCollation));
}

static void testLineHoldsWhereSomebodyElseHoldsFunction()
{
    // Intl.supportedValuesOf("calendar"); sink(Intl.supportedValuesOf)
    AOT_TEST_CHECK(usesFeatureForStringStraysTests("    v1 = String string=\"calendar\"\n    v2 = Call v0, -, v1\n    v3 = Builtin builtin=GlobalThis\n    v4 = PropGet v3 key=\"sink\" [isFreeName]\n    v5 = Call v4, -, v0\n    Return v5\n"_s, RemovableFeature::IntlCollation));
    // Intl.supportedValuesOf("calendar"); sink(Intl)
    AOT_TEST_CHECK(usesFeatureForStringStraysTests("    v1 = String string=\"calendar\"\n    v2 = Call v0, -, v1\n    v3 = Builtin builtin=GlobalThis\n    v4 = PropGet v3 key=\"sink\" [isFreeName]\n    v5 = Builtin builtin=Intl\n    v6 = Call v4, -, v5\n    Return v6\n"_s, RemovableFeature::IntlCollation));
}

// ---- The table of literal strings

// function pick0(v) { return typeof v === "number" ? "a0" : "b0"; } ..  sink(pick0, ..)
static String textsThatMeetForStringStraysTests(unsigned numberOfFunctions)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    for (unsigned function = 0; function < numberOfFunctions; ++function) {
        text.append("    v"_s, 2 + 2 * function, " = NewClosure function=f"_s, function + 1, '\n');
        text.append("    v"_s, 3 + 2 * function, " = Call v1, -, v"_s, 2 + 2 * function, '\n');
    }
    text.append("    Return v0\n}\n"_s);
    for (unsigned function = 0; function < numberOfFunctions; ++function) {
        text.append("function f"_s, function + 1, " module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n"_s);
        text.append("  b0(v0: anything):\n    v1 = TypeTest v0 testedType=number\n    v2 = String string=\"a"_s, function, "\"\n    v3 = String string=\"b"_s, function, "\"\n"_s);
        text.append("    Branch v1 -> taken b1(), notTaken b2()\n  b1():\n    Jump -> next b3(v2)\n  b2():\n    Jump -> next b3(v3)\n  b3(v4):\n    Return v4\n}\n"_s);
    }
    return text.toString();
}

static std::optional<uint64_t> instructionsOfPlanForStringStraysTests(const String& text, size_t fewestLiteralStrings)
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
        AOT_TEST_CHECK(stages.plan()->literalStrings().strings().size() >= fewestLiteralStrings);
    });
    return instructions;
}

static void testPlanGrowsAsTextsThatMeetDo()
{
    constexpr unsigned numberOfFunctions = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfPlanForStringStraysTests(textsThatMeetForStringStraysTests(numberOfFunctions), 2 * numberOfFunctions);
    std::optional<uint64_t> large = instructionsOfPlanForStringStraysTests(textsThatMeetForStringStraysTests(4 * numberOfFunctions), 8 * numberOfFunctions);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

// ---- The ladder of names

// class Box { constructor() { this.width = 1; } }  function run(p) { const x0 = p.next; x0.tag = x0.width; const x1 = p.next; .. }  sink(run, new Box, { width: 1 })
// Two layouts have `width` and none has `tag`: each value goes up the whole ladder and finds nothing.
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
        // The first request runs the whole stage, for what callers leave their callees.
        AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        });
    });
    return instructions;
}

// Where each value that climbs the ladder reads the body for itself, the work is the square of the body.
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
        { "string-strays/a-feature-line-is-left-out-where-no-call-in-sight-may-fulfil-it"_s, testLineIsLeftOutWhereNoCallInSightMayFulfilIt, "facts/AOTFactsOfProgram.cpp: features: a line that hangs on an operand holds wherever its row is reached: the look at the operands waits in staging/strings-3 (F11) for its four marks"_s },
        { "string-strays/a-feature-line-holds-where-a-call-in-sight-may-fulfil-it"_s, testLineHoldsWhereCallInSightMayFulfilIt },
        { "string-strays/a-feature-line-holds-where-somebody-else-holds-the-function"_s, testLineHoldsWhereSomebodyElseHoldsFunction },
        { "string-strays/the-plan-grows-as-the-texts-that-meet-do"_s, testPlanGrowsAsTextsThatMeetDo, "plan/AOTPlan.cpp: literal strings: the empty text alone has a number: the table of the texts that meet waits in staging/strings-3 (T1) for its three marks and its price"_s },
        { "string-strays/the-walk-grows-as-the-values-that-climb-the-ladder-of-names-do"_s, testWalkGrowsAsValuesThatClimbLadderDo },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
