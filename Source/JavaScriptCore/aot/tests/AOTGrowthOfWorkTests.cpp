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

// class Wide { m0() { return 1 } m1() { return 1 } .. }  const w = new Wide; w.m0(); w.m1(); ..
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

// One class with as many members as a generated client has calls. Where a lookup goes through the members of the class, this does not end.
static void testEachOfManyMembersIsFoundByItsKey()
{
    constexpr uint32_t numberOfKeys = 300000;
    // No divisor in common with the number of keys: the keys stand in another order than the members.
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
        // By the remainder of the number: a method, the two halves of an accessor far from each other, a method that is defined twice.
        // All but the first are numbers that the program has no key for: a lookup that finds a member asks nothing about its key. Where it finds none it asks what kind of name the key is.
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
    std::optional<uint64_t> small = instructionsOfPlanForGrowthTests(classWithManyMethodsForGrowthTests(numberOfMethods));
    std::optional<uint64_t> large = instructionsOfPlanForGrowthTests(classWithManyMethodsForGrowthTests(4 * numberOfMethods));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// function run(p0, p1, ..) { return p0; }  sink(run)
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

// A convention counts its parameters in sixteen bits. One parameter more stopped the compiler in a cast: 128,000 parameters, "panic: abort() called".
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

// class Base { }  class C0 extends Base { constructor() { super(); } }  sink(new C0);  class C1 extends Base ..
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

// Each class asks of the constructors above it what they answer with, and what Link knows of a constructor grows with the classes that call it. Where each class has that looked through anew, the work is the square of the classes.
static void testFactsGrowAsClassesBelowOneClassDo()
{
    constexpr unsigned numberOfClasses = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfFactsForGrowthTests(classesBelowOneClassForGrowthTests(numberOfClasses));
    std::optional<uint64_t> large = instructionsOfFactsForGrowthTests(classesBelowOneClassForGrowthTests(4 * numberOfClasses));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

enum class LoopInRowForGrowthTests : uint8_t {
    // while (c) { }
    Empty,
    // for (const x of list) { }
    OverList,
};

// function run(c) { const list = [c]; <loop> <loop> .. }  sink(run)
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
        // The first request runs the whole stage, for what callers leave their callees.
        AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        });
    });
    return instructions;
}

// The looks at a loop keep something for each loop that they come to. Where they make room for every loop of the body, the work is the square of the number of loops.
static void testWalkGrowsAsLoopsInRowDo()
{
    constexpr unsigned numberOfLoops = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::Empty, numberOfLoops), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::Empty, 4 * numberOfLoops), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// function run(end) { let sum = 0; for (let i = 0; i < end; i++) sum += i; { let k = 2; while (k-- > 0) sum += k; } .. return sum }  sink(run)
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

// Each loop up to `end` narrows `end` for its own turns, and each loop on `k` leaves a name behind that holds in all that follows. Whoever looks for a name of `end` along that list, or asks of each of its names
// whether the list holds it, does the square of the number of loops.
static void testWalkGrowsAsLoopsUpToOneBoundDo()
{
    constexpr unsigned numberOfPairs = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfWalkForGrowthTests(loopsUpToOneBoundForGrowthTests(numberOfPairs), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsUpToOneBoundForGrowthTests(4 * numberOfPairs), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// The position of an iteration by index comes round in a parameter of its head. Where the head or the edge that comes round goes through the iterations of the whole body, the work is the square of their number.
static void testWalkGrowsAsIterationsInRowDo()
{
    constexpr unsigned numberOfLoops = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::OverList, numberOfLoops), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::OverList, 4 * numberOfLoops), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

enum class StatementInRowForGrowthTests : uint8_t {
    // const x = make(); if (!x) throw x;
    TestThatLeaves,
    // const x = make(); while (x) { }
    Loop,
    // try { const x = make(); .. } catch { }   with all the statements in one try
    UnderOneHandler,
};

// function run(make) { <statement> <statement> .. return make(x0, x1, ..) }: each value is read at the end, so at each block as many values are live as statements stand in front of it.
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
        // The first value made is read at the end alone, and is live at the head of the last block in front of it.
        AOT_TEST_CHECK(liveness.lastUse(ValueID(1u)).index() > index.numberOfValues() - 4);
    });
    return instructions;
}

// A list of what is live at each head holds the square of the statements. So does going through what is live at the head of each loop, and uniting what is live at a handler with each block under it.
template<StatementInRowForGrowthTests kind>
static void testLivenessGrowsAsValuesReadAtEndDo()
{
    constexpr unsigned numberOfStatements = 4000;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfLivenessForGrowthTests(valuesReadAtEndForGrowthTests(kind, numberOfStatements));
    std::optional<uint64_t> large = instructionsOfLivenessForGrowthTests(valuesReadAtEndForGrowthTests(kind, 4 * numberOfStatements));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// `a + b + c + ..`, `.. + (c + (b + a))` and `"(" + ("(" + .. + ")") + ")"`, each as long as a generated table. A join costs the same however long its pieces are, and only the text that is read is written out.
// Where every text on the way is written out, they hold the square of the number of joins in bytes, and this does not end.
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

        // The same joins again, as the second arm of a guess makes them.
        StringID again = textOfNull;
        for (uint32_t join = 0; join < numberOfJoins; ++join)
            again = arena.internJoinedTexts(again, textOfNull);
        AOT_TEST_CHECK(again == appended);

        // Equal texts get one id, however they were put together.
        StringID leftFirst = arena.internJoinedTexts(arena.internJoinedTexts(textOfTrue, textOfNull), textOfFalse);
        StringID rightFirst = arena.internJoinedTexts(textOfTrue, arena.internJoinedTexts(textOfNull, textOfFalse));
        AOT_TEST_CHECK(leftFirst == rightFirst && spells(leftFirst, "truenullfalse"_s));
        AOT_TEST_CHECK(arena.internText(byteCast<uint8_t>("truenullfalse"_s.span8())) == leftFirst);
        StringID ofBytes = arena.internText(byteCast<uint8_t>("falsetrue"_s.span8()));
        AOT_TEST_CHECK(arena.internJoinedTexts(textOfFalse, textOfTrue) == ofBytes && arena.internJoinedTexts(textOfTrue, textOfFalse) != ofBytes);

        // The two halves of a pair are one character of the joined text: U+1F600.
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

// function run() { return "ab" + "ab" + "ab" + ..; }  sink(run)
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

// Folding knows every sum on the way. Where it writes each of them out, the work is the square of the number of terms.
static void testWalkGrowsAsTermsOfSumOfTextsDo()
{
    constexpr unsigned numberOfTerms = 6000;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(numberOfTerms), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(4 * numberOfTerms), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// The sum is longer than the longest text that folding makes, so it is joined of several when the program runs. Where each sum on the way leaves a join behind that nobody reads, with a text of its own,
// or where the known end of the sum is written out anew at each term, the work past that length is many thousand times the work in front of it.
static void testWalkGrowsAsTermsOfSumOfTextsDoPastLongestTextThatIsMade()
{
    constexpr unsigned numberOfTerms = 20000;
    InstructionCounts::begin();
    std::optional<uint64_t> small = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(numberOfTerms), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(4 * numberOfTerms), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// begin(); import("./e1.js"); import("./e2.js"); ..; end(import("./eN.js"))        e1.js: import "./e1-1.js"        e1-1.js: import "./e1-2.js"        ..
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

// Of the thread that runs the program, from its call of begin() until the promise that it hands to end() is fulfilled.
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

// Every step that the loader takes for a graph asks about the graphs that are being loaded: how many there are, whether one has this root. Where a step goes through them, the work is the square of their number.
static void testRunGrowsAsGraphsAskedForInOneJobDo()
{
    constexpr unsigned numberOfGraphs = 1000;
    std::optional<uint64_t> small = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(numberOfGraphs, 1));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(4 * numberOfGraphs, 1));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// A graph of several levels has a way down on which its modules wait, and the plan of each graph asks whether it has found what waits on the way of another.
static void testRunGrowsAsGraphsOfSeveralLevelsAskedForInOneJobDo()
{
    constexpr unsigned numberOfGraphs = 1000;
    std::optional<uint64_t> small = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(numberOfGraphs, 3));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(4 * numberOfGraphs, 3));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// async function run() { const calls = [() => 0, () => 0, ..]; let i = 0; for (; i < calls.length; i++) { calls[i](); await null; } return i; }        begin(); run(); end(run())
// Two run side by side: each finds the job of the other in the queue at every await, so it has to suspend.
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

// As the front end writes a long list: the first elements are listed, the others appended as they are made, so that none waits in the frame for the last.
static void testRunGrowsAsAwaitsBehindLongListDo()
{
    constexpr unsigned numberOfClosures = 2000;
    std::optional<uint64_t> small = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(numberOfClosures, 32));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(4 * numberOfClosures, 32));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// What one operation takes lies in the frame until then, and none of it lives across an await. Where each resumption goes through the whole frame, the work is the square of the number of operands.
static void testResumptionCostsWhatWasKeptHoweverLargeFrameIs()
{
    constexpr unsigned numberOfClosures = 2000;
    std::optional<uint64_t> small = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(numberOfClosures, numberOfClosures));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(4 * numberOfClosures, 4 * numberOfClosures));
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
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
        { "growth-of-work/the-liveness-of-a-body-grows-as-the-values-read-at-its-end-do"_s, testLivenessGrowsAsValuesReadAtEndDo<StatementInRowForGrowthTests::TestThatLeaves> },
        { "growth-of-work/the-liveness-of-a-body-grows-as-the-loops-that-values-live-across-do"_s, testLivenessGrowsAsValuesReadAtEndDo<StatementInRowForGrowthTests::Loop> },
        { "growth-of-work/the-liveness-of-a-body-grows-as-the-blocks-under-one-handler-do"_s, testLivenessGrowsAsValuesReadAtEndDo<StatementInRowForGrowthTests::UnderOneHandler> },
        { "growth-of-work/the-walk-grows-as-the-iterations-in-a-row-do"_s, testWalkGrowsAsIterationsInRowDo },
        { "growth-of-work/a-chain-of-joined-texts-costs-what-its-last-text-holds"_s, testChainOfJoinedTextsCostsWhatItsLastTextHolds },
        { "growth-of-work/the-walk-grows-as-the-terms-of-a-sum-of-texts-do"_s, testWalkGrowsAsTermsOfSumOfTextsDo },
        { "growth-of-work/the-walk-grows-as-the-terms-of-a-sum-of-texts-do-past-the-longest-text-that-is-made"_s, testWalkGrowsAsTermsOfSumOfTextsDoPastLongestTextThatIsMade },
        { "growth-of-work/a-run-grows-as-the-graphs-asked-for-in-one-job-do"_s, testRunGrowsAsGraphsAskedForInOneJobDo },
        { "growth-of-work/a-run-grows-as-the-graphs-of-several-levels-asked-for-in-one-job-do"_s, testRunGrowsAsGraphsOfSeveralLevelsAskedForInOneJobDo },
        { "growth-of-work/a-run-grows-as-the-awaits-behind-a-long-list-do"_s, testRunGrowsAsAwaitsBehindLongListDo },
        { "growth-of-work/a-resumption-costs-what-was-kept-however-large-the-frame-is"_s, testResumptionCostsWhatWasKeptHoweverLargeFrameIs, "coroutine-bytes: emitCopyFrameToStack() clears every word that no suspension keeps, at every resumption: a call with K arguments ahead of K awaits costs the square of K. The clearing may not go (8 of 721 programs then hold an object for ever); the resumed frame has to be shorter"_s },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
