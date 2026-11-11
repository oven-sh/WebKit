/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTCallingConventions.h"
#include "AOTCompileQueue.h"
#include "AOTFacts.h"
#include "AOTIRParser.h"
#include "AOTLinkTables.h"
#include "AOTNativeTest.h"
#include "AOTPlan.h"
#include "AOTProgramFile.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTStages.h"
#include "AOTTestHarness.h"
#include <algorithm>
#include <atomic>
#include <wtf/Threading.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> unitQueueTests();

static constexpr ASCIILiteral classThatOnlyKnownCodeUses = R"IR(layout L0 kind=Instance name="Box" module=m0 constructor=f1
module m0 path="main.js" displayPath="main.js" body=f0 namespaceLayout=L(m0)
entry module=m0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    v2 = NewClosure function=f2
    DefineMethod v1, v2 layout=L0 key="size"
    v4 = FinishClass v1 layout=L0
    v5 = Construct v4
    v6 = PropGet v5 key="size"
    v7 = Call v6, v5 calleeKey="size"
    v8 = Builtin builtin=GlobalThis
    v9 = PropGet v8 key="sink" [isFreeName]
    v10 = Call v9, -, v7 calleeName="sink"
    Return v10
}
function f1 name="Box" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f2 name="size" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    Return v0
}
)IR"_s;

static String featuresUnderOneNameForQueueTests(unsigned numberOfFeatures)
{
    StringBuilder text;
    for (unsigned index = 0; index < numberOfFeatures; ++index)
        text.append("layout L"_s, index, " module=m0 fields=[{key=\"run\" declared={type=anything} flags=[IsRequired]}]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n"_s);
    for (unsigned index = 0; index < numberOfFeatures; ++index) {
        unsigned value = 5 * index;
        text.append("    v"_s, value, " = NewClosure function=f"_s, 2 * index + 1, '\n');
        text.append("    v"_s, value + 1, " = NewObject -, v"_s, value, " layout=L"_s, index, " fields=[0]\n"_s);
        text.append("    v"_s, value + 2, " = FinishObject v"_s, value + 1, '\n');
        text.append("    v"_s, value + 3, " = NewClosure function=f"_s, 2 * index + 2, '\n');
        text.append("    v"_s, value + 4, " = Call v"_s, value + 3, ", -, v"_s, value + 2, '\n');
    }
    text.append("    v"_s, 5 * numberOfFeatures, " = Undefined\n    Return v"_s, 5 * numberOfFeatures, "\n}\n"_s);
    for (unsigned index = 0; index < numberOfFeatures; ++index) {
        text.append("function f"_s, 2 * index + 1, " name=\"run\" module=m0 thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Undefined\n    Return v0\n}\n"_s);
        text.append("function f"_s, 2 * index + 2, " module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    v1 = PropGet v0 key=\"run\"\n    v2 = Call v1, v0 calleeKey=\"run\"\n    Return v2\n}\n"_s);
    }
    return text.toString();
}

static String tableWhoseFunctionsReadItForQueueTests(unsigned numberOfFunctions)
{
    StringBuilder text;
    text.append("layout L0 module=m0 fields=["_s);
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        text.append(index ? ", "_s : ""_s, "{key=\"step"_s, index, "\" declared={type=anything} flags=[IsRequired]}"_s);
    text.append("]\nvariable g0 name=\"table\" module=m0 declared={type=anything|absent} flags=[IsImmutable]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n"_s);
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        text.append("    v"_s, index, " = NewClosure function=f"_s, index + 1, '\n');
    text.append("    v"_s, numberOfFunctions, " = NewObject -"_s);
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        text.append(", v"_s, index);
    text.append(" layout=L0 fields=["_s);
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        text.append(index ? ", "_s : ""_s, index);
    text.append("]\n    v"_s, numberOfFunctions + 1, " = FinishObject v"_s, numberOfFunctions, '\n');
    text.append("    PutModuleVariable v"_s, numberOfFunctions + 1, " variable=g0\n"_s);
    text.append("    v"_s, numberOfFunctions + 3, " = Call v0, -, v0\n    Return v"_s, numberOfFunctions + 3, "\n}\n"_s);
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        text.append("function f"_s, index + 1, " module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
        text.append("    v1 = GetModuleVariable variable=g0\n    v2 = CheckInitialized v1 name=\"table\"\n    v3 = ElemGet v2, v0\n    v4 = Call v3, -, v0\n    Return v4\n}\n"_s);
    }
    return text.toString();
}

namespace {

struct ProgramOfQueueTests {
    const Typed::Program& program;
    const Typed::CallingConventions& conventions;
    const Typed::LinkTables& linked;
    Typed::Arrivals& arrivals;
};

} // anonymous namespace

static void withProgramOfQueueTests(const String& text, NOESCAPE const Invocable<void(const ProgramOfQueueTests&)> auto& functor)
{
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Typed::Program> parsed = Typed::parseProgram(Typed::withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        Typed::StagedBuild stages(WTF::move(parsed), build);
        const PreparedProgram* prepared = stages.prepared();
        AOT_TEST_CHECK(prepared);
        functor(ProgramOfQueueTests { *prepared->program, prepared->plan->conventions(), prepared->facts->linked(), *prepared->arrivals });
    });
}

static void withProgramOfQueueTests(NOESCAPE const Invocable<void(const ProgramOfQueueTests&)> auto& functor)
{
    withProgramOfQueueTests(String(classThatOnlyKnownCodeUses), functor);
}

static void testWhoMayNameAFunctionGrowsWithTheProgram()
{
    struct Sizes {
        size_t referrers { 0 };
        size_t cyclesAwaited { 0 };
    };
    auto sizesWith = [](unsigned numberOfFeatures) {
        Sizes sizes;
        withProgramOfQueueTests(featuresUnderOneNameForQueueTests(numberOfFeatures), [&](const ProgramOfQueueTests& program) {
            sizes = { program.linked.referrers.size(), program.linked.cyclesAwaited.size() };
            UnitQueue queue(program.program, program.conventions, program.linked, program.arrivals, 1);
            AOT_TEST_CHECK(queue.leadsTo(FunctionID(2u), FunctionID(1u)));
            AOT_TEST_CHECK(!queue.leadsTo(FunctionID(2u), FunctionID(2 * numberOfFeatures - 1)));
            AOT_TEST_CHECK(!queue.leadsTo(FunctionID(1u), FunctionID(2u)));
            AOT_TEST_CHECK(program.arrivals.takesWhatWasLeft(FunctionID(1u)));
            AOT_TEST_CHECK(program.linked.leadsTo(FunctionID(2u), FunctionID(1u)));
            AOT_TEST_CHECK(!program.linked.leadsTo(FunctionID(2u), FunctionID(2 * numberOfFeatures - 1)));
        });
        return sizes;
    };
    Sizes smaller = sizesWith(64);
    Sizes large = sizesWith(128);
    AOT_TEST_CHECK(smaller.referrers >= 3 * 64);
    AOT_TEST_CHECK(large.referrers <= 2 * smaller.referrers);
    AOT_TEST_CHECK(large.cyclesAwaited <= 2 * smaller.cyclesAwaited);
}

static void testReadersOfATableUnderUnknownKeysNameOneGroup()
{
    auto referrersWith = [](unsigned numberOfFunctions) {
        size_t referrers = 0;
        withProgramOfQueueTests(tableWhoseFunctionsReadItForQueueTests(numberOfFunctions), [&](const ProgramOfQueueTests& program) {
            referrers = program.linked.referrers.size();
            AOT_TEST_CHECK(program.linked.leadsTo(FunctionID(1u), FunctionID(numberOfFunctions)));
            AOT_TEST_CHECK(program.linked.leadsTo(FunctionID(numberOfFunctions), FunctionID(1u)));
        });
        return referrers;
    };
    size_t smaller = referrersWith(64);
    size_t large = referrersWith(128);
    AOT_TEST_CHECK(smaller >= 2 * 64);
    AOT_TEST_CHECK(large <= 2 * smaller);
}

static constexpr ASCIILiteral methodWithAGuardThatCallsWhatItIsHanded = R"IR(layout L0 module=m0 fields=[{key="then" declared={type=anything} flags=[IsRequired]}]
module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f1
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    v3 = NewClosure function=f2
    v4 = PropGet v2 key="then"
    v5 = Call v4, v2, v3 calleeKey="then"
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="sink" [isFreeName]
    v8 = Call v7, -, v2
    Return v8
}
function f1 name="then" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=11
    v2 = Call v0, -, v1 calleeName="resolve"
    v3 = Integer integer=12
    v4 = Call v0, -, v3 calleeName="resolve"
    Return v4
}
function f2 module=m0 thisType={type=anything} results=[{type=anything}] {
  b0():
    v0 = Undefined
    Return v0
}
)IR"_s;

static void takeUnitsOfMethodWithAGuard(UnitQueue& queue, std::span<const Typed::CodeUnit> seeds)
{
    constexpr unsigned thread = 0;
    Typed::FunctionID method(1u);
    Typed::CodeUnit fastVersion { method, Typed::CodeRole::Body };
    Typed::CodeUnit guard { method, Typed::CodeRole::ParameterGuard };
    Typed::CodeUnit slowVersion { method, Typed::CodeRole::SlowBody };
    Typed::CodeUnit handed { Typed::FunctionID(2u), Typed::CodeRole::Body };

    ParallelLoops loops(1);
    queue.addSeeds(seeds, loops);

    Vector<Typed::CodeUnit> taken;
    while (std::optional<Typed::CodeUnit> unit = queue.take(thread)) {
        if (*unit == slowVersion) {
            queue.mention(thread, handed);
        }
        taken.append(*unit);
        queue.didFinish(thread);
    }
    size_t placeOfSlowVersion = taken.find(slowVersion);
    AOT_TEST_CHECK(placeOfSlowVersion != notFound && taken.find(fastVersion) < placeOfSlowVersion && taken.find(guard) < placeOfSlowVersion);
    AOT_TEST_CHECK(taken.find(handed) != notFound && taken.find(handed) > placeOfSlowVersion);
}

static void withMethodWithAGuard(NOESCAPE const Invocable<void(const PreparedProgram&, std::span<const Typed::CodeUnit> seeds)> auto& functor)
{
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Typed::Program> parsed = Typed::parseProgram(Typed::withStandInLibrary(String(methodWithAGuardThatCallsWhatItIsHanded)), build);
        AOT_TEST_CHECK(parsed);
        Typed::StagedBuild stages(WTF::move(parsed), build);
        const PreparedProgram* prepared = stages.prepared();
        AOT_TEST_CHECK(prepared);
        AOT_TEST_CHECK(prepared->plan->conventions().exists({ Typed::FunctionID(1u), Typed::CodeRole::SlowBody }));
        AOT_TEST_CHECK(prepared->facts->linked().leadsTo(Typed::FunctionID(1u), Typed::FunctionID(2u)));
        Vector<Typed::CodeUnit> seeds = Typed::unitsNamedOutsideCode(*prepared->program, *prepared->facts, *prepared->plan);
        functor(*prepared, seeds.span());
    });
}

static void testSlowVersionComesBehindTheOtherUnitsOfItsCycle()
{
    withMethodWithAGuard([](const PreparedProgram& prepared, std::span<const Typed::CodeUnit> seeds) {
        UnitQueue queue(*prepared.program, prepared.plan->conventions(), prepared.facts->linked(), *prepared.arrivals, 1);
        takeUnitsOfMethodWithAGuard(queue, seeds);
    });
}

static void testSeedIsMentionedByWhoeverDoesNotLeadToIt()
{
    withMethodWithAGuard([](const PreparedProgram& prepared, std::span<const Typed::CodeUnit> seeds) {
        constexpr unsigned thread = 0;
        Typed::FunctionID bodyOfModule(0u);
        Typed::CodeUnit handed { Typed::FunctionID(2u), Typed::CodeRole::Body };
        AOT_TEST_CHECK(!prepared.facts->linked().leadsTo(handed.function, bodyOfModule));
        AOT_TEST_CHECK(!prepared.facts->linked().function(bodyOfModule).isInQueueFromStart);
        AOT_TEST_CHECK(!prepared.arrivals->takesWhatWasLeft(bodyOfModule));
        auto seed = std::ranges::find(seeds, bodyOfModule, &Typed::CodeUnit::function);
        AOT_TEST_CHECK(seed != seeds.end());

        UnitQueue queue(*prepared.program, prepared.plan->conventions(), prepared.facts->linked(), *prepared.arrivals, 1);
        ParallelLoops loops(1);
        queue.addSeeds(seeds, loops);
        bool hasMentionedSeed = false;
        while (std::optional<Typed::CodeUnit> unit = queue.take(thread)) {
            if (unit->function == Typed::FunctionID(1u))
                queue.mention(thread, handed);
            if (*unit == handed) {
                queue.mention(thread, *seed);
                hasMentionedSeed = true;
            }
            queue.didFinish(thread);
        }
        AOT_TEST_CHECK(hasMentionedSeed);
    });
}

std::span<const NativeTest> unitQueueTests()
{
    static constexpr NativeTest tests[] {
        { "unit-queue/who-may-name-a-function-grows-with-the-program"_s, testWhoMayNameAFunctionGrowsWithTheProgram },
        { "unit-queue/readers-of-a-table-under-unknown-keys-name-one-group"_s, testReadersOfATableUnderUnknownKeysNameOneGroup },
        { "unit-queue/a-slow-version-comes-behind-the-other-units-of-its-cycle"_s, testSlowVersionComesBehindTheOtherUnitsOfItsCycle },
        { "unit-queue/a-seed-is-mentioned-by-whoever-does-not-lead-to-it"_s, testSeedIsMentionedByWhoeverDoesNotLeadToIt },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
