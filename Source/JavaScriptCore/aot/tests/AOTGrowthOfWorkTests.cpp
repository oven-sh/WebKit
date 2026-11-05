/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTFacts.h"
#include "AOTGrowthOfWork.h"
#include "AOTIRParser.h"
#include "AOTLinkTables.h"
#include "AOTNativeTest.h"
#include "AOTPlan.h"
#include "AOTProgramFile.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTTestHarness.h"
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> growthOfWorkTests();

using namespace Typed;

static String classWithManyMethodsForGrowthTests(unsigned numberOfMethods)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Wide\" module=m0 constructor=f1\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0 namespaceLayout=L(m0)\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = NewClosure function=f1\n    v1 = NewClass v0 layout=L0\n"_s);
    unsigned value = 2;
    for (unsigned method = 0; method < numberOfMethods; ++method, value += 2) {
        text.append("    v"_s, value, " = NewClosure function=f"_s, method + 2, '\n');
        text.append("    DefineMethod v1, v"_s, value, " layout=L0 index="_s, method, " key=\"m"_s, method, "\"\n"_s);
    }
    unsigned classObject = value++;
    unsigned object = value++;
    text.append("    v"_s, classObject, " = FinishClass v1 layout=L0\n    v"_s, object, " = Construct v"_s, classObject, '\n');
    for (unsigned method = 0; method < numberOfMethods; ++method, value += 2) {
        text.append("    v"_s, value, " = PropGet v"_s, object, " key=\"m"_s, method, "\"\n"_s);
        text.append("    v"_s, value + 1, " = Call v"_s, value, ", v"_s, object, " calleeKey=\"m"_s, method, "\"\n"_s);
    }
    text.append("    Return v"_s, object, "\n}\n"_s);
    text.append("function f1 name=\"Wide\" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {\n"_s);
    text.append("  b0(v0: anything):\n    v1 = GetConstructPrototype v0 layout=L0\n    v2 = NewObject v1 layout=L0\n    Return v2\n}\n"_s);
    for (unsigned method = 0; method < numberOfMethods; ++method) {
        text.append("function f"_s, method + 2, " name=\"m"_s, method, "\" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {\n"_s);
        text.append("  b0():\n    v0 = Integer integer=1\n    Return v0\n}\n"_s);
    }
    return text.toString();
}

static void testEachOfManyMembersIsFoundByItsKey()
{
    constexpr uint32_t numberOfKeys = 300000;
    constexpr uint32_t step = 7919;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(classWithManyMethodsForGrowthTests(1)), build);
        AOT_TEST_CHECK(program);
        LayoutID wide(0u);
        LinkTables tables = conservativeLinkTables(*program);
        tables.layouts[wide.index()].listsEveryMemberAbove = true;

        auto keyOf = [&](uint32_t number) {
            return PropertyKeyID(static_cast<uint32_t>(static_cast<uint64_t>(number) * step % numberOfKeys));
        };
        auto append = [&](MethodKind kind, PropertyKeyID key, FunctionID function) {
            LinkedMember member;
            member.classLayout = wide;
            member.kind = kind;
            member.index = static_cast<uint32_t>(tables.members.size());
            member.key = key;
            member.function = function;
            member.everyWriterIsKnown = true;
            tables.members.append(member);
        };
        for (uint32_t number = 1; number < numberOfKeys; ++number)
            append(number % 3 == 1 ? MethodKind::Getter : MethodKind::Method, keyOf(number), FunctionID(number));
        for (uint32_t number = 1; number < numberOfKeys; ++number) {
            if (number % 3)
                append(number % 3 == 1 ? MethodKind::Setter : MethodKind::Method, keyOf(number), FunctionID(numberOfKeys + number));
        }
        tables.indexMembersByKey();

        Vector<FunctionID, 4> functions;
        for (uint32_t number = 1; number < numberOfKeys; ++number) {
            MemberFound found = tables.findMember(*program, wide, ClassSide::Prototype, keyOf(number));
            functions.shrink(0);
            bool isTold = tables.findFunctionsLaidAbove(*program, wide, keyOf(number), functions);
            switch (number % 3) {
            case 0:
                AOT_TEST_CHECK(found.kind == MemberFound::Kind::Method && found.method == FunctionID(number) && !found.getter && !found.setter);
                AOT_TEST_CHECK(isTold && functions.size() == 1 && functions[0] == FunctionID(number));
                break;
            case 1:
                AOT_TEST_CHECK(found.kind == MemberFound::Kind::Accessor && !found.method && found.getter == FunctionID(number) && found.setter == FunctionID(numberOfKeys + number));
                AOT_TEST_CHECK(!isTold);
                break;
            case 2:
                AOT_TEST_CHECK(found.kind == MemberFound::Kind::Unknown);
                AOT_TEST_CHECK(isTold && functions.size() == 2 && functions[0] == FunctionID(number) && functions[1] == FunctionID(numberOfKeys + number));
                break;
            }
            AOT_TEST_CHECK(tables.membersUnder(wide, ClassSide::Static, keyOf(number)).empty());
        }
        AOT_TEST_CHECK(tables.findMember(*program, wide, ClassSide::Prototype, keyOf(0)).kind == MemberFound::Kind::Nothing);
    });
}

static std::optional<uint64_t> instructionsOfPlanForGrowthTests(const String& text)
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

static void testPlanGrowsAsMembersOfClassDo()
{
    constexpr unsigned numberOfMethods = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfPlanForGrowthTests(classWithManyMethodsForGrowthTests(numberOfMethods));
    std::optional<uint64_t> large = instructionsOfPlanForGrowthTests(classWithManyMethodsForGrowthTests(4 * numberOfMethods));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String functionWithManyParametersForGrowthTests(unsigned numberOfParameters)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length="_s, numberOfParameters, " thisType={type=anything} parameters=["_s);
    for (unsigned parameter = 0; parameter < numberOfParameters; ++parameter)
        text.append(parameter ? ", "_s : ""_s, "{type=anything}"_s);
    text.append("] results=[{type=anything}] {\n  b0("_s);
    for (unsigned parameter = 0; parameter < numberOfParameters; ++parameter)
        text.append(parameter ? ", v"_s : "v"_s, parameter, ": anything"_s);
    text.append("):\n    Return v0\n}\n"_s);
    return text.toString();
}

static void testFunctionWithMoreParametersThanConventionCountsIsRefused()
{
    for (auto [numberOfParameters, isRefused] : { std::pair { 60000u, false }, std::pair { 70000u, true } }) {
        runOnBuildThread([&] {
            BuildContext build(makeOptionsOfTestedProgram());
            std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(functionWithManyParametersForGrowthTests(numberOfParameters)), build);
            AOT_TEST_CHECK(parsed);
            StagedBuild stages(WTF::move(parsed), build);
            AOT_TEST_CHECK(stages.facts());
            AOT_TEST_CHECK(!stages.plan() == isRefused);
            build.diagnostics().finish(nullptr);
            bool namesLimit = std::ranges::any_of(build.diagnostics().userDiagnostics(), [](const UserDiagnostic& diagnostic) {
                return diagnostic.kind == UserDiagnosticKind::LimitExceeded && diagnostic.limit == LimitID::MaxReservedStackArguments && diagnostic.function == FunctionID(1u);
            });
            AOT_TEST_CHECK(namesLimit == isRefused);
        });
    }
}

static String classesBelowOneClassForGrowthTests(unsigned numberOfClasses)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Base\" module=m0 constructor=f1\n"_s);
    for (unsigned number = 0; number < numberOfClasses; ++number)
        text.append("layout L"_s, number + 1, " kind=Instance name=\"C"_s, number, "\" module=m0 base=L0 constructor=f"_s, number + 2, " supertypes=[L0] flags=[HasHeritage]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = NewClass v2 layout=L0\n    v4 = FinishClass v3 layout=L0\n    v5 = Convert v4 declared={type=f1 flags=[IsFromCompiler]}\n"_s);
    unsigned value = 6;
    for (unsigned number = 0; number < numberOfClasses; ++number, value += 7) {
        text.append("    v"_s, value, " = ResolveHeritage v5\n    v"_s, value + 1, " = NewClosure function=f"_s, number + 2, "\n    v"_s, value + 2, " = NewClass v"_s, value + 1, ", v5, v"_s, value, " layout=L"_s, number + 1, '\n');
        text.append("    v"_s, value + 3, " = FinishClass v"_s, value + 2, " layout=L"_s, number + 1, "\n    v"_s, value + 4, " = Convert v"_s, value + 3, " declared={type=f"_s, number + 2, " flags=[IsFromCompiler]}\n"_s);
        text.append("    v"_s, value + 5, " = Construct v"_s, value + 4, ", -\n    v"_s, value + 6, " = Call v1, -, v"_s, value + 5, '\n');
    }
    text.append("    v"_s, value, " = Undefined\n    Return v"_s, value, "\n}\n"_s);
    text.append("function f1 name=\"Base\" module=m0 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] {\n  b0(v0: anything):\n"_s);
    text.append("    v1 = GetConstructPrototype v0 layout=L0\n    v2 = NewObject v1 layout=L0\n    v3 = Convert v2 declared={type=L0}\n    Return v3\n}\n"_s);
    for (unsigned number = 0; number < numberOfClasses; ++number) {
        text.append("function f"_s, number + 2, " name=\"C"_s, number, "\" module=m0 kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] {\n"_s);
        text.append("  b0(v0: function, v1: anything):\n    v2 = GetSuperBase v0\n    v3 = SuperConstruct v2, v1 layout=L"_s, number + 1, "\n    v4 = Convert v3 declared={type=L"_s, number + 1, "}\n    Return v4\n}\n"_s);
    }
    return text.toString();
}

static std::optional<uint64_t> instructionsOfFactsForGrowthTests(const String& text)
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

static void testFactsGrowAsClassesBelowOneClassDo()
{
    constexpr unsigned numberOfClasses = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfFactsForGrowthTests(classesBelowOneClassForGrowthTests(numberOfClasses));
    std::optional<uint64_t> large = instructionsOfFactsForGrowthTests(classesBelowOneClassForGrowthTests(4 * numberOfClasses));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

enum class LoopInRowForGrowthTests : uint8_t {
    Empty,
    OverList,
};

static String loopsInRowForGrowthTests(LoopInRowForGrowthTests kind, unsigned numberOfLoops)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=boolean}] results=[{type=anything}] {\n  b0(v0: boolean):\n    v1 = NewArray v0 contents=anything\n"_s);
    unsigned value = 2;
    unsigned block = 1;
    for (unsigned loop = 0; loop < numberOfLoops; ++loop) {
        switch (kind) {
        case LoopInRowForGrowthTests::Empty:
            text.append("    Jump -> next b"_s, block, "()\n  b"_s, block, "():\n    LoopHint\n    Branch v0 -> taken b"_s, block, "(), notTaken b"_s, block + 1, "()\n  b"_s, block + 1, "():\n"_s);
            value += 1;
            block += 2;
            break;
        case LoopInRowForGrowthTests::OverList:
            text.append("    v"_s, value, " = IteratorOpen v1\n    Jump -> next b"_s, block, "()\n  b"_s, block, "():\n    LoopHint\n"_s);
            text.append("    v"_s, value + 2, " = IteratorResume v"_s, value, "\n    v"_s, value + 3, " = ResultAt v"_s, value + 2, "\n    v"_s, value + 4, " = ResultAt v"_s, value + 2, " index=1\n"_s);
            text.append("    Branch v"_s, value + 3, " -> taken b"_s, block + 3, "(), notTaken b"_s, block + 1, "()\n"_s);
            text.append("  b"_s, block + 1, "() handler=b"_s, block + 2, "():\n    Jump -> next b"_s, block, "()\n"_s);
            text.append("  b"_s, block + 2, "() role=IterationCleanup subject=v"_s, value, ":\n    v"_s, value + 5, " = CatchException\n    IteratorClose v"_s, value, " completion=Throw\n    Rethrow v"_s, value + 5, "\n"_s);
            text.append("  b"_s, block + 3, "():\n"_s);
            value += 7;
            block += 4;
            break;
        }
    }
    text.append("    Return v0\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfWalkForGrowthTests(const String& text, FunctionID function)
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

static void testWalkGrowsAsLoopsInRowDo()
{
    constexpr unsigned numberOfLoops = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::Empty, numberOfLoops), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::Empty, 4 * numberOfLoops), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String loopsUpToOneBoundForGrowthTests(unsigned numberOfPairs)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=int[0, 4294967295]}] results=[{type=anything}] {\n  b0(v0: int[0, 4294967295]):\n"_s);
    text.append("    v1 = Integer\n    v2 = Convert v1 declared={type=number}\n"_s);
    constexpr ASCIILiteral asNumber = " declared={type=number}\n"_s;
    unsigned value = 3;
    unsigned block = 1;
    unsigned sum = 2;
    for (unsigned pair = 0; pair < numberOfPairs; ++pair, value += 22, block += 6) {
        text.append("    v"_s, value, " = Integer\n    v"_s, value + 1, " = Convert v"_s, value, asNumber, "    Jump -> next b"_s, block, "(v"_s, sum, ", v"_s, value + 1, ")\n"_s);
        text.append("  b"_s, block, "(v"_s, value + 2, ": number, v"_s, value + 3, ": number):\n    LoopHint\n    v"_s, value + 5, " = Less v"_s, value + 3, ", v0\n"_s);
        text.append("    Branch v"_s, value + 5, " -> taken b"_s, block + 1, "(), notTaken b"_s, block + 2, "()\n"_s);
        text.append("  b"_s, block + 1, "():\n    v"_s, value + 6, " = Add v"_s, value + 2, ", v"_s, value + 3, "\n    v"_s, value + 7, " = Convert v"_s, value + 6, asNumber);
        text.append("    v"_s, value + 8, " = Increment v"_s, value + 3, "\n    v"_s, value + 9, " = Convert v"_s, value + 8, asNumber, "    Jump -> next b"_s, block, "(v"_s, value + 7, ", v"_s, value + 9, ")\n"_s);
        text.append("  b"_s, block + 2, "():\n    v"_s, value + 10, " = Integer integer=2\n    v"_s, value + 11, " = Convert v"_s, value + 10, asNumber, "    Jump -> next b"_s, block + 3, "(v"_s, value + 2, ", v"_s, value + 11, ")\n"_s);
        text.append("  b"_s, block + 3, "(v"_s, value + 12, ": number, v"_s, value + 13, ": number):\n    LoopHint\n    v"_s, value + 15, " = ToNumeric v"_s, value + 13, "\n    v"_s, value + 16, " = Decrement v"_s, value + 15, '\n');
        text.append("    v"_s, value + 17, " = Convert v"_s, value + 16, asNumber, "    v"_s, value + 18, " = Integer\n    v"_s, value + 19, " = Greater v"_s, value + 15, ", v"_s, value + 18, '\n');
        text.append("    Branch v"_s, value + 19, " -> taken b"_s, block + 4, "(), notTaken b"_s, block + 5, "()\n"_s);
        text.append("  b"_s, block + 4, "():\n    v"_s, value + 20, " = Add v"_s, value + 12, ", v"_s, value + 17, "\n    v"_s, value + 21, " = Convert v"_s, value + 20, asNumber, "    Jump -> next b"_s, block + 3, "(v"_s, value + 21, ", v"_s, value + 17, ")\n"_s);
        text.append("  b"_s, block + 5, "():\n"_s);
        sum = value + 12;
    }
    text.append("    Return v"_s, sum, "\n}\n"_s);
    return text.toString();
}

static void testWalkGrowsAsLoopsUpToOneBoundDo()
{
    constexpr unsigned numberOfPairs = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(loopsUpToOneBoundForGrowthTests(numberOfPairs), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsUpToOneBoundForGrowthTests(4 * numberOfPairs), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testWalkGrowsAsIterationsInRowDo()
{
    constexpr unsigned numberOfLoops = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::OverList, numberOfLoops), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::OverList, 4 * numberOfLoops), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

enum class ReadInRowForGrowthTests : uint8_t {
    OfOneOfThreeElements,
    OfElementOfItsOwn,
};

static String earlyReturnsInRowForGrowthTests(ReadInRowForGrowthTests kind, unsigned numberOfStatements)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Item\" module=m0 constructor=f2 fields=[{key=\"skip\" declared={type=boolean} flags=[IsRequired]}, {key=\"weight\" declared={type=number} flags=[IsRequired]}] ownFieldCount=2\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0 namespaceLayout=L(m0)\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module flags=[IsChecked] thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = NewClosure function=f1\n    v1 = NewClosure function=f2\n"_s);
    text.append("    v2 = NewClass v1 layout=L0\n    v3 = FinishClass v2 layout=L0\n    v4 = Boolean\n    v5 = Integer integer=2\n    v6 = Construct v3, -, v4, v5\n"_s);
    text.append("    v7 = Builtin builtin=JSON\n    v8 = PropGet v7 key=\"parse\"\n    v9 = String string=\"{}\"\n    v10 = Call v8, v7, v9 calleeKey=\"parse\"\n"_s);
    text.append("    v11 = NewArray v6, v10, v6 contents=undefined|null|L0\n    v12 = Integer\n    Jump -> next b1(v12)\n"_s);
    text.append("  b1(v13: number):\n    LoopHint\n    v15 = Integer integer=300\n    v16 = Less v13, v15\n    Branch v16 -> taken b2(), notTaken b3()\n"_s);
    text.append("  b2():\n    v17 = Call v0, -, v11\n    v18 = Increment v13\n    Jump -> next b1(v18)\n  b3():\n    Return v13\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 flags=[IsChecked] length=1 thisType={type=anything} parameters=[{type=undefined|null|Array<undefined|null|L0> flags=[OmitsNull, OmitsUndefined]}] results=[{type=anything}] {\n"_s);
    text.append("  b0(v0: undefined|null|Array<undefined|null|L0>):\n"_s);
    unsigned value = 1;
    unsigned block = 1;
    Vector<unsigned> kept;
    for (unsigned statement = 0; statement < numberOfStatements; ++statement) {
        text.append("    v"_s, value, " = Integer integer="_s, kind == ReadInRowForGrowthTests::OfElementOfItsOwn ? statement : statement % 3, '\n');
        text.append("    v"_s, value + 1, " = ElemGet v0, v"_s, value, "\n    v"_s, value + 2, " = PropGet v"_s, value + 1, " key=\"skip\"\n    v"_s, value + 3, " = ToBoolean v"_s, value + 2, '\n');
        text.append("    Branch v"_s, value + 3, " -> taken b"_s, block, "(), notTaken b"_s, block + 1, "()\n  b"_s, block, "():\n    Return v"_s, value, "\n  b"_s, block + 1, "():\n"_s);
        text.append("    v"_s, value + 4, " = PropGet v"_s, value + 1, " key=\"weight\"\n"_s);
        kept.append(value + 4);
        value += 5;
        block += 2;
    }
    text.append("    v"_s, value, " = NewArray "_s);
    for (unsigned place = 0; place < kept.size(); ++place)
        text.append(place ? ", v"_s : "v"_s, kept[place]);
    text.append(" contents=anything\n    Return v"_s, value, "\n}\n"_s);
    text.append("function f2 name=\"Item\" module=m0 kind=BaseConstructor flags=[HasNewTarget, IsChecked, HasExplicitName] length=2 thisType={type=anything} parameters=[{type=boolean}, {type=number}] results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {\n"_s);
    text.append("  b0(v0: anything, v1: boolean, v2: number):\n    v3 = GetConstructPrototype v0 layout=L0\n    v4 = NewObject v3 layout=L0\n    PropSet v4, v1 key=\"skip\"\n    PropSet v4, v2 key=\"weight\"\n    Return v4\n}\n"_s);
    return text.toString();
}

template<ReadInRowForGrowthTests kind, unsigned numberOfStatements>
static void testWalkGrowsAsEarlyReturnsThatEachLeaveValueAliveDo()
{
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(earlyReturnsInRowForGrowthTests(kind, numberOfStatements), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(earlyReturnsInRowForGrowthTests(kind, 4 * numberOfStatements), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

enum class StatementInRowForGrowthTests : uint8_t {
    TestThatLeaves,
    Loop,
    UnderOneHandler,
};

static String valuesReadAtEndForGrowthTests(StatementInRowForGrowthTests kind, unsigned numberOfStatements)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\"\n"_s);
    text.append("function f0 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
    bool isUnderHandler = kind == StatementInRowForGrowthTests::UnderOneHandler;
    unsigned handler = 2 * numberOfStatements + 2;
    auto head = [&](unsigned block) {
        text.append("  b"_s, block, "()"_s);
        if (isUnderHandler)
            text.append(" handler=b"_s, handler, "()"_s);
        text.append(":\n"_s);
    };
    unsigned value = 1;
    unsigned block = 1;
    Vector<unsigned> made;
    if (isUnderHandler) {
        text.append("    Jump -> next b1()\n"_s);
        head(block++);
    }
    for (unsigned statement = 0; statement < numberOfStatements; ++statement) {
        made.append(value);
        text.append("    v"_s, value, " = Call v0\n    v"_s, value + 1, " = ToBoolean v"_s, value, '\n');
        switch (kind) {
        case StatementInRowForGrowthTests::TestThatLeaves:
            text.append("    Branch v"_s, value + 1, " -> taken b"_s, block + 1, "(), notTaken b"_s, block, "()\n"_s);
            head(block);
            text.append("    Throw v"_s, value, '\n');
            head(block + 1);
            break;
        case StatementInRowForGrowthTests::Loop:
            text.append("    Jump -> next b"_s, block, "()\n"_s);
            head(block);
            text.append("    LoopHint\n    Branch v"_s, value + 1, " -> taken b"_s, block, "(), notTaken b"_s, block + 1, "()\n"_s);
            head(block + 1);
            ++value;
            break;
        case StatementInRowForGrowthTests::UnderOneHandler:
            text.append("    Branch v"_s, value + 1, " -> taken b"_s, block + 1, "(), notTaken b"_s, block, "()\n"_s);
            head(block);
            text.append("    Jump -> next b"_s, block + 1, "()\n"_s);
            head(block + 1);
            break;
        }
        value += 2;
        block += 2;
    }
    text.append("    v"_s, value, " = Call v0, -"_s);
    for (unsigned read : made)
        text.append(", v"_s, read);
    text.append("\n    Return v"_s, value, '\n');
    if (isUnderHandler)
        text.append("  b"_s, handler, "() role=CatchHandler:\n    v"_s, value + 1, " = CatchValue\n    Return v0\n"_s);
    text.append("}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfLivenessForGrowthTests(const String& text)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(text, build);
        AOT_TEST_CHECK(program);
        BodyIndexTable indexes(static_cast<uint32_t>(program->functions.size()), 1u);
        const BodyIndex& index = indexes.build(FunctionID(0u), program->at(FunctionID(0u)), 0);
        BodyLiveness liveness;
        instructions = instructionsOfAllThreadsIn([&] {
            liveness.compute(index);
        });
        AOT_TEST_CHECK(liveness.lastUse(ValueID(1u)).index() > index.numberOfValues() - 4);
    });
    return instructions;
}

template<StatementInRowForGrowthTests kind>
static void testLivenessGrowsAsValuesReadAtEndDo()
{
    constexpr unsigned numberOfStatements = 4000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfLivenessForGrowthTests(valuesReadAtEndForGrowthTests(kind, numberOfStatements));
    std::optional<uint64_t> large = instructionsOfLivenessForGrowthTests(valuesReadAtEndForGrowthTests(kind, 4 * numberOfStatements));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testChainOfJoinedTextsCostsWhatItsLastTextHolds()
{
    constexpr uint32_t numberOfJoins = 100000;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(classWithManyMethodsForGrowthTests(1)), build);
        AOT_TEST_CHECK(program);
        TypeArena arena(program->types, ArenaKeeps::UntilReset);
        StringID textOfNull = program->strings.textOfNull();
        StringID textOfTrue = program->strings.textOf(true);
        StringID textOfFalse = program->strings.textOf(false);
        StringID textOfUndefined = program->strings.textOfUndefined();
        auto spells = [&](StringID text, ASCIILiteral expected) {
            return std::ranges::equal(arena.bytesOf(text), byteCast<uint8_t>(expected.span8()));
        };

        StringID appended = textOfNull;
        StringID prepended = textOfTrue;
        StringID wrapped = textOfNull;
        for (uint32_t join = 0; join < numberOfJoins; ++join) {
            appended = arena.internJoinedTexts(appended, textOfNull);
            prepended = arena.internJoinedTexts(textOfTrue, prepended);
            wrapped = arena.internJoinedTexts(arena.internJoinedTexts(textOfFalse, wrapped), textOfUndefined);
        }
        AOT_TEST_CHECK(arena.numberOfBytesOf(appended) == 4 * (numberOfJoins + 1) && arena.numberOfBytesOf(prepended) == 4 * (numberOfJoins + 1) && arena.numberOfBytesOf(wrapped) == 4 + 14 * numberOfJoins);
        AOT_TEST_CHECK(!arena.isWideText(appended));
        for (auto [text, part] : { std::pair { appended, textOfNull }, std::pair { prepended, textOfTrue } }) {
            std::span<const uint8_t> bytes = arena.bytesOf(text);
            AOT_TEST_CHECK(bytes.size() == 4 * (numberOfJoins + 1));
            for (size_t place = 0; place < bytes.size(); place += 4)
                AOT_TEST_CHECK(std::ranges::equal(bytes.subspan(place, 4), arena.bytesOf(part)));
        }
        std::span<const uint8_t> bytesOfWrapped = arena.bytesOf(wrapped);
        AOT_TEST_CHECK(std::ranges::equal(bytesOfWrapped.subspan(5 * numberOfJoins - 5, 23), byteCast<uint8_t>("falsenullundefinedundef"_s.span8())));
        AOT_TEST_CHECK(std::ranges::equal(bytesOfWrapped.first(10), byteCast<uint8_t>("falsefalse"_s.span8())) && std::ranges::equal(bytesOfWrapped.last(9), arena.bytesOf(textOfUndefined)));

        StringID again = textOfNull;
        for (uint32_t join = 0; join < numberOfJoins; ++join)
            again = arena.internJoinedTexts(again, textOfNull);
        AOT_TEST_CHECK(again == appended);

        StringID leftFirst = arena.internJoinedTexts(arena.internJoinedTexts(textOfTrue, textOfNull), textOfFalse);
        StringID rightFirst = arena.internJoinedTexts(textOfTrue, arena.internJoinedTexts(textOfNull, textOfFalse));
        AOT_TEST_CHECK(leftFirst == rightFirst && spells(leftFirst, "truenullfalse"_s));
        AOT_TEST_CHECK(arena.internText(byteCast<uint8_t>("truenullfalse"_s.span8())) == leftFirst);
        StringID ofBytes = arena.internText(byteCast<uint8_t>("falsetrue"_s.span8()));
        AOT_TEST_CHECK(arena.internJoinedTexts(textOfFalse, textOfTrue) == ofBytes && arena.internJoinedTexts(textOfTrue, textOfFalse) != ofBytes);

        constexpr std::array<uint8_t, 4> lead { 'a', 0xed, 0xa0, 0xbd };
        constexpr std::array<uint8_t, 4> trail { 0xed, 0xb8, 0x80, 'b' };
        constexpr std::array<uint8_t, 6> pair { 'a', 0xf0, 0x9f, 0x98, 0x80, 'b' };
        StringID joinedHalves = arena.internJoinedTexts(arena.internText(lead), arena.internText(trail));
        AOT_TEST_CHECK(std::ranges::equal(arena.bytesOf(joinedHalves), pair) && arena.isWideText(joinedHalves) && arena.numberOfBytesOf(joinedHalves) == 6);
        StringID halvesApart = arena.internJoinedTexts(arena.internText(trail), arena.internText(lead));
        AOT_TEST_CHECK(arena.numberOfBytesOf(halvesApart) == 8 && arena.bytesOf(halvesApart).size() == 8 && arena.isWideText(halvesApart));
        AOT_TEST_CHECK(std::ranges::equal(arena.bytesOf(arena.internJoinedTexts(halvesApart, arena.internText(trail))).last(6), pair));

        arena.reset();
        AOT_TEST_CHECK(spells(arena.internJoinedTexts(textOfNull, textOfNull), "nullnull"_s));
    });
}

static void testTextsThatAddToOneAnotherAndAreEachReadCostWhatLastOneHolds()
{
    constexpr uint32_t numberOfJoins = 100000;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(classWithManyMethodsForGrowthTests(1)), build);
        AOT_TEST_CHECK(program);
        TypeArena arena(program->types, ArenaKeeps::UntilReset);
        StringID textOfNull = program->strings.textOfNull();
        StringID textOfTrue = program->strings.textOf(true);
        StringID first = arena.internJoinedTexts(textOfNull, textOfTrue);
        arena.bytesOf(first);
        struct Row {
            StringID text;
            ptrdiff_t place { 0 };
            unsigned numberOfPlaces { 0 };
        };
        std::array<Row, 2> rows { Row { first }, Row { arena.internJoinedTexts(textOfTrue, textOfTrue) } };
        for (uint32_t join = 0; join < numberOfJoins; ++join) {
            arena.internText(byteCast<uint8_t>(String::number(join).span8()));
            for (Row& row : rows) {
                row.text = arena.internJoinedTexts(arena.internJoinedTexts(row.text, textOfNull), textOfTrue);
                std::span<const uint8_t> bytes = arena.bytesOf(row.text);
                AOT_TEST_CHECK(bytes.size() == 8 * (static_cast<size_t>(join) + 2) && std::ranges::equal(bytes.last(8), byteCast<uint8_t>("nulltrue"_s.span8())));
                ptrdiff_t place = bytes.data() - arena.bytesOf(first).data();
                row.numberOfPlaces += place != std::exchange(row.place, place);
                AOT_TEST_CHECK(row.numberOfPlaces <= 32);
            }
        }
        AOT_TEST_CHECK(std::ranges::equal(arena.bytesOf(rows[0].text).first(16), byteCast<uint8_t>("nulltruenulltrue"_s.span8())) && std::ranges::equal(arena.bytesOf(rows[1].text).first(16), byteCast<uint8_t>("truetruenulltrue"_s.span8())));

        auto spells = [&](StringID made, ASCIILiteral expected) {
            return std::ranges::equal(arena.bytesOf(made), byteCast<uint8_t>(expected.span8()));
        };
        StringID stem = arena.internJoinedTexts(textOfTrue, textOfNull);
        arena.bytesOf(stem);
        StringID left = arena.internJoinedTexts(stem, textOfTrue);
        StringID right = arena.internJoinedTexts(stem, textOfNull);
        AOT_TEST_CHECK(spells(left, "truenulltrue"_s) && arena.bytesOf(left).data() == arena.bytesOf(stem).data());
        AOT_TEST_CHECK(spells(right, "truenullnull"_s) && arena.bytesOf(right).data() != arena.bytesOf(stem).data());
        AOT_TEST_CHECK(spells(stem, "truenull"_s) && spells(left, "truenulltrue"_s) && spells(first, "nulltrue"_s));
    });
}

static String sumOfTextsForGrowthTests(unsigned numberOfTerms)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = String string=\"ab\"\n"_s);
    for (unsigned term = 1; term < numberOfTerms; ++term)
        text.append("    v"_s, term, " = Add v"_s, term - 1, ", v0\n"_s);
    text.append("    Return v"_s, numberOfTerms - 1, "\n}\n"_s);
    return text.toString();
}

static void testWalkGrowsAsTermsOfSumOfTextsDo()
{
    constexpr unsigned numberOfTerms = 6000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(numberOfTerms), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(4 * numberOfTerms), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testWalkGrowsAsTermsOfSumOfTextsDoPastLongestTextThatIsMade()
{
    constexpr unsigned numberOfTerms = 20000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(numberOfTerms), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(4 * numberOfTerms), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String graphsAskedForInOneJobForGrowthTests(unsigned numberOfGraphs, unsigned modulesOfGraph)
{
    StringBuilder text;
    text.append("module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"_s);
    unsigned numberOfModules = numberOfGraphs * modulesOfGraph;
    for (unsigned module = 1; module <= numberOfModules; ++module) {
        text.append("module m"_s, module, " path=\"/e"_s, module, ".js\" displayPath=\"e"_s, module, ".js\" body=f"_s, module);
        if (module % modulesOfGraph)
            text.append(" requests=[{target=m"_s, module + 1, " completes=true}]"_s);
        text.append('\n');
    }
    text.append("entry module=m0\nfunction f0 name=\"main\" kind=Module results=[{type=anything}] {\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"begin\" [isFreeName]\n    v2 = Call v1, -\n"_s);
    unsigned value = 3;
    for (unsigned graph = 0; graph < numberOfGraphs; ++graph, ++value)
        text.append("    v"_s, value, " = ImportModule module=m"_s, graph * modulesOfGraph + 1, " referrer=m0\n"_s);
    text.append("    v"_s, value, " = PropGet v0 key=\"end\" [isFreeName]\n    v"_s, value + 1, " = Call v"_s, value, ", -, v"_s, value - 1, "\n    Return\n}\n"_s);
    for (unsigned module = 1; module <= numberOfModules; ++module)
        text.append("function f"_s, module, " name=\"e"_s, module, "\" module=m"_s, module, " kind=Module results=[{type=anything}] {\n    Return\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfRunForGrowthTests(const String& text)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(program);
        output = compileWithMechanisms(WTF::move(program), build, mechanismsOfWay(""_s));
        AOT_TEST_CHECK(output->hasImage() && !output->section(Wire::ResultSection::EntryModuleIds).empty());
    });
    TemporaryFile image(".image"_s, output->section(Wire::ResultSection::Image));
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString("let atBegin; globalThis.begin = () => { atBegin = aotInstructionsOfThread(); }; globalThis.end = last => last.then(() => print(atBegin === null ? \"none\" : aotInstructionsOfThread() - atBegin)); runCompiledEntryPoint("_s, output->records<uint32_t>(Wire::ResultSection::EntryModuleIds)[0], ");"_s),
    };
    ChildProcessResult result = ChildProcessLauncher(shellPath(), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    StringView printed = StringView(result.output).trim(isASCIIWhitespace<char16_t>);
    if (printed == "none"_s)
        return std::nullopt;
    std::optional<uint64_t> instructions = parseInteger<uint64_t>(printed);
    AOT_TEST_CHECK(instructions);
    return instructions;
}

static void testRunGrowsAsGraphsAskedForInOneJobDo()
{
    constexpr unsigned numberOfGraphs = 1000;
    std::optional<uint64_t> smaller = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(numberOfGraphs, 1));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(4 * numberOfGraphs, 1));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testRunGrowsAsGraphsOfSeveralLevelsAskedForInOneJobDo()
{
    constexpr unsigned numberOfGraphs = 1000;
    std::optional<uint64_t> smaller = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(numberOfGraphs, 3));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(4 * numberOfGraphs, 3));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String awaitsBehindLongListForGrowthTests(unsigned numberOfClosures, unsigned numberListedAtOnce)
{
    StringBuilder text;
    text.append("module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 name=\"main\" kind=Module results=[{type=anything}] {\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"begin\" [isFreeName]\n    v2 = Call v1, -\n"_s);
    text.append("    v3 = NewClosure function=f1\n    v4 = Call v3, -\n    v5 = Call v3, -\n    v6 = PropGet v0 key=\"end\" [isFreeName]\n    v7 = Call v6, -, v5\n    Return\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 flags=[IsAsync] thisType={type=anything} results=[{type=anything}] {\n  b0():\n"_s);
    unsigned next = 0;
    for (; next < numberListedAtOnce; ++next)
        text.append("    v"_s, next, " = NewClosure function=f2\n"_s);
    unsigned list = next++;
    text.append("    v"_s, list, " = NewArray v0"_s);
    for (unsigned closure = 1; closure < numberListedAtOnce; ++closure)
        text.append(", v"_s, closure);
    text.append(" contents=anything\n"_s);
    for (unsigned closure = numberListedAtOnce; closure < numberOfClosures; ++closure, next += 2)
        text.append("    v"_s, next, " = NewClosure function=f2\n    AppendElement v"_s, list, ", v"_s, next, "\n"_s);
    text.append("    v"_s, next, " = Integer\n    Jump -> next b1(v"_s, next, ")\n"_s);
    text.append("  b1(v"_s, next + 1, ": number):\n    LoopHint\n    v"_s, next + 3, " = Integer integer="_s, numberOfClosures, "\n    v"_s, next + 4, " = Less v"_s, next + 1, ", v"_s, next + 3, "\n"_s);
    text.append("    Branch v"_s, next + 4, " -> taken b2(), notTaken b3()\n  b2():\n    v"_s, next + 5, " = ElemGet v"_s, list, ", v"_s, next + 1, "\n    v"_s, next + 6, " = Call v"_s, next + 5, ", -\n"_s);
    text.append("    v"_s, next + 7, " = Null\n    v"_s, next + 8, " = Await v"_s, next + 7, "\n    v"_s, next + 9, " = Increment v"_s, next + 1, "\n    Jump -> next b1(v"_s, next + 9, ")\n"_s);
    text.append("  b3():\n    Return v"_s, next + 1, "\n}\n"_s);
    text.append("function f2 name=\"call\" module=m0 kind=Arrow thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Integer\n    Return v0\n}\n"_s);
    return text.toString();
}

static void testRunGrowsAsAwaitsBehindLongListDo()
{
    constexpr unsigned numberOfClosures = 2000;
    std::optional<uint64_t> smaller = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(numberOfClosures, 32));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(4 * numberOfClosures, 32));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String classDefinedMemberByMemberForGrowthTests(unsigned numberOfMethods)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Wide\" module=m0 constructor=f1\nmodule m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 name=\"main\" kind=Module results=[{type=anything}] {\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"begin\" [isFreeName]\n    v2 = Call v1, -\n"_s);
    text.append("    v3 = NewClosure function=f1\n    v4 = NewClass v3 layout=L0\n"_s);
    unsigned value = 5;
    for (unsigned method = 0; method < numberOfMethods; ++method, value += 2) {
        text.append("    v"_s, value, " = NewClosure function=f"_s, method + 3, '\n');
        text.append("    DefineMethod v4, v"_s, value, " layout=L0 index="_s, method, " key=\"m"_s, method, "\"\n"_s);
    }
    text.append("    v"_s, value, " = FinishClass v4 layout=L0\n    v"_s, value + 1, " = NewClosure function=f2\n    v"_s, value + 2, " = Call v"_s, value + 1, ", -\n"_s);
    text.append("    v"_s, value + 3, " = PropGet v0 key=\"end\" [isFreeName]\n    v"_s, value + 4, " = Call v"_s, value + 3, ", -, v"_s, value + 2, "\n    Return\n}\n"_s);
    text.append("function f1 name=\"Wide\" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {\n"_s);
    text.append("  b0(v0: anything):\n    v1 = GetConstructPrototype v0 layout=L0\n    v2 = NewObject v1 layout=L0\n    Return v2\n}\n"_s);
    text.append("function f2 name=\"later\" module=m0 flags=[IsAsync] thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Integer\n    Return v0\n}\n"_s);
    for (unsigned method = 0; method < numberOfMethods; ++method)
        text.append("function f"_s, method + 3, " module=m0 kind=Method thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Integer integer=1\n    Return v0\n}\n"_s);
    return text.toString();
}

static void testRunGrowsAsMembersThatClassDefinesOneByOneDo()
{
    constexpr unsigned numberOfMethods = 1000;
    std::optional<uint64_t> smaller = instructionsOfRunForGrowthTests(classDefinedMemberByMemberForGrowthTests(numberOfMethods));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(classDefinedMemberByMemberForGrowthTests(4 * numberOfMethods));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testResumptionCostsWhatWasKeptHoweverLargeFrameIs()
{
    constexpr unsigned numberOfClosures = 2000;
    std::optional<uint64_t> smaller = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(numberOfClosures, numberOfClosures));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(4 * numberOfClosures, 4 * numberOfClosures));
    AOT_TEST_CHECK(smaller && large);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

std::span<const NativeTest> growthOfWorkTests()
{
    static constexpr NativeTest tests[] {
        { "growth-of-work/each-of-many-members-of-a-class-is-found-by-its-key"_s, testEachOfManyMembersIsFoundByItsKey },
        { "growth-of-work/the-plan-grows-as-the-members-of-a-class-do"_s, testPlanGrowsAsMembersOfClassDo },
        { "growth-of-work/a-function-with-more-parameters-than-a-convention-counts-is-refused"_s, testFunctionWithMoreParametersThanConventionCountsIsRefused },
        { "growth-of-work/the-facts-grow-as-the-classes-below-one-class-do"_s, testFactsGrowAsClassesBelowOneClassDo },
        { "growth-of-work/the-walk-grows-as-the-loops-in-a-row-do"_s, testWalkGrowsAsLoopsInRowDo },
        { "growth-of-work/the-walk-grows-as-the-loops-up-to-one-bound-do"_s, testWalkGrowsAsLoopsUpToOneBoundDo },
        { "growth-of-work/the-walk-grows-as-the-early-returns-that-each-leave-a-value-alive-do"_s, testWalkGrowsAsEarlyReturnsThatEachLeaveValueAliveDo<ReadInRowForGrowthTests::OfOneOfThreeElements, 1000> },
        { "growth-of-work/the-walk-grows-as-the-early-returns-behind-guesses-of-their-own-that-each-leave-a-value-alive-do"_s, testWalkGrowsAsEarlyReturnsThatEachLeaveValueAliveDo<ReadInRowForGrowthTests::OfElementOfItsOwn, 150> },
        { "growth-of-work/the-liveness-of-a-body-grows-as-the-values-read-at-its-end-do"_s, testLivenessGrowsAsValuesReadAtEndDo<StatementInRowForGrowthTests::TestThatLeaves> },
        { "growth-of-work/the-liveness-of-a-body-grows-as-the-loops-that-values-live-across-do"_s, testLivenessGrowsAsValuesReadAtEndDo<StatementInRowForGrowthTests::Loop> },
        { "growth-of-work/the-liveness-of-a-body-grows-as-the-blocks-under-one-handler-do"_s, testLivenessGrowsAsValuesReadAtEndDo<StatementInRowForGrowthTests::UnderOneHandler> },
        { "growth-of-work/the-walk-grows-as-the-iterations-in-a-row-do"_s, testWalkGrowsAsIterationsInRowDo },
        { "growth-of-work/a-chain-of-joined-texts-costs-what-its-last-text-holds"_s, testChainOfJoinedTextsCostsWhatItsLastTextHolds },
        { "growth-of-work/texts-that-add-to-one-another-and-are-each-read-cost-what-the-last-one-holds"_s, testTextsThatAddToOneAnotherAndAreEachReadCostWhatLastOneHolds },
        { "growth-of-work/the-walk-grows-as-the-terms-of-a-sum-of-texts-do"_s, testWalkGrowsAsTermsOfSumOfTextsDo },
        { "growth-of-work/the-walk-grows-as-the-terms-of-a-sum-of-texts-do-past-the-longest-text-that-is-made"_s, testWalkGrowsAsTermsOfSumOfTextsDoPastLongestTextThatIsMade },
        { "growth-of-work/a-run-grows-as-the-graphs-asked-for-in-one-job-do"_s, testRunGrowsAsGraphsAskedForInOneJobDo },
        { "growth-of-work/a-run-grows-as-the-graphs-of-several-levels-asked-for-in-one-job-do"_s, testRunGrowsAsGraphsOfSeveralLevelsAskedForInOneJobDo },
        { "growth-of-work/a-run-grows-as-the-awaits-behind-a-long-list-do"_s, testRunGrowsAsAwaitsBehindLongListDo },
        { "growth-of-work/a-run-grows-as-the-members-that-a-class-defines-one-by-one-do"_s, testRunGrowsAsMembersThatClassDefinesOneByOneDo },
        { "growth-of-work/a-resumption-costs-what-was-kept-however-large-the-frame-is"_s, testResumptionCostsWhatWasKeptHoweverLargeFrameIs, "emitCopyFrameToStack() clears every word that no suspension keeps, at every resumption: a call with K arguments ahead of K awaits costs the square of K. The clearing may not go (8 of 721 programs then hold an object for ever); the resumed frame has to be shorter"_s },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
