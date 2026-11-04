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
#include "AOTLeavePoints.h"
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

// class Box { size() { return 1 } }  sink(new Box().size())
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

// function once(x) { return x }  function ping(x) { return pong(x) }  function pong(x) { return ping(x) }  once(1); ping(1)
static constexpr ASCIILiteral oneFunctionCalledOnceAndTwoThatCallEachOther = R"IR(variable g0 name="once" module=m0 declared={type=f1 flags=[IsFromCompiler]} initialization=Function function=f1 flags=[IsImmutable]
variable g1 name="ping" module=m0 declared={type=f2 flags=[IsFromCompiler]} initialization=Function function=f2 flags=[IsImmutable]
variable g2 name="pong" module=m0 declared={type=f3 flags=[IsFromCompiler]} initialization=Function function=f3 flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    v1 = GetModuleVariable variable=g0
    v2 = Call v1, -, v0 calleeName="once"
    v3 = GetModuleVariable variable=g1
    v4 = Call v3, -, v0 calleeName="ping"
    v5 = Undefined
    Return v5
}
function f1 name="once" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Return v0
}
function f2 name="ping" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetModuleVariable variable=g2
    v2 = Call v1, -, v0 calleeName="pong"
    Return v2
}
function f3 name="pong" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetModuleVariable variable=g1
    v2 = Call v1, -, v0 calleeName="ping"
    Return v2
}
)IR"_s;

// Only their number counts: a test of the order lays down who names whom.
static String sevenFunctionsForQueueTests()
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    for (unsigned index = 0; index < 7; ++index)
        text.append("function f"_s, index, " module=m0"_s, index ? ""_s : " kind=Module"_s, " thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Undefined\n    Return v0\n}\n"_s);
    return text.toString();
}

// const box0 = { run() {} };  function use0(o) { return o.run(); }  use0(box0);  and so on: as many functions lie under the one name as there are functions that look the name up.
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

namespace {

struct ProgramOfQueueTests {
    const Typed::CallingConventions& conventions;
    const Typed::LinkTables& linked;
    Typed::Arrivals& arrivals;
};

struct NamingInQueueTests {
    uint32_t referrer;
    uint32_t named;
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
        functor(ProgramOfQueueTests { prepared->plan->conventions(), prepared->facts->linked(), *prepared->arrivals });
    });
}

static void withProgramOfQueueTests(NOESCAPE const Invocable<void(const ProgramOfQueueTests&)> auto& functor)
{
    withProgramOfQueueTests(String(classThatOnlyKnownCodeUses), functor);
}

// The tables of the program with another order of cycles. `cycles` and `ranks`: of the first functions. Every other function is alone, names nobody, and nobody names it.
static Typed::LinkTables tablesWithOrderForQueueTests(const Typed::LinkTables& linked, std::span<const uint32_t> cycles, std::span<const uint32_t> ranks, std::span<const NamingInQueueTests> namings)
{
    Typed::LinkTables tables = linked;
    size_t numberOfFunctions = tables.functions.size();
    AOT_TEST_CHECK(cycles.size() == ranks.size() && cycles.size() <= numberOfFunctions);
    tables.rankOfCycle.fill(0, numberOfFunctions);
    for (size_t index = 0; index < numberOfFunctions; ++index) {
        tables.functions[index].cycle = index < cycles.size() ? cycles[index] : static_cast<uint32_t>(index);
        tables.functions[index].rank = index < ranks.size() ? ranks[index] : 0;
        tables.rankOfCycle[tables.functions[index].cycle] = tables.functions[index].rank;
    }

    Vector<Vector<uint32_t>> awaited(numberOfFunctions);
    for (const NamingInQueueTests& naming : namings) {
        uint32_t referrer = cycles[naming.referrer];
        uint32_t named = cycles[naming.named];
        if (referrer != named && !awaited[named].contains(referrer))
            awaited[named].append(referrer);
    }
    tables.firstCycleAwaited.shrink(0);
    tables.cyclesAwaited.shrink(0);
    tables.cyclesNamed.shrink(0);
    for (Vector<uint32_t>& list : awaited) {
        std::ranges::sort(list);
        tables.firstCycleAwaited.append(static_cast<uint32_t>(tables.cyclesAwaited.size()));
        tables.cyclesAwaited.appendVector(list);
    }
    tables.firstCycleAwaited.append(static_cast<uint32_t>(tables.cyclesAwaited.size()));
    tables.listCyclesNamed();
    return tables;
}

static void testHeaviestUnitsStandOneToARun()
{
    constexpr size_t lengthOfRun = UnitQueue::mostUnitsTakenAtOnce;
    for (uint32_t numberOfUnits : { 0u, 1u, 2u, 31u, 32u, 33u, 64u, 100u, 1000u }) {
        Vector<uint32_t> lightestFirst(numberOfUnits, [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        Vector<uint32_t> inRuns = UnitQueue::inRunsOfUnitQueue(lightestFirst.span());
        AOT_TEST_CHECK(inRuns.size() == numberOfUnits);

        Vector<uint32_t> sorted = inRuns;
        std::ranges::sort(sorted);
        AOT_TEST_CHECK(sorted == lightestFirst);

        // The run that is taken first has the heaviest unit for its newest, the next run the next, and no run has a second one of them.
        size_t numberOfRuns = (numberOfUnits + lengthOfRun - 1) / lengthOfRun;
        for (size_t run = 0; run < numberOfRuns; ++run) {
            size_t end = numberOfUnits - run * lengthOfRun;
            size_t begin = end > lengthOfRun ? end - lengthOfRun : 0;
            AOT_TEST_CHECK(inRuns[end - 1] == numberOfUnits - 1 - run);
            for (size_t place = begin; place + 1 < end; ++place)
                AOT_TEST_CHECK(inRuns[place] < numberOfUnits - numberOfRuns);
        }
    }
}

// A mention that comes again does nothing: the runs are made of what has come into the queue.
static void testSeedsGoToSharedListInRuns()
{
    withProgramOfQueueTests([](const ProgramOfQueueTests& program) {
        for (unsigned widthOfLoops : { 1u, 4u }) {
            constexpr unsigned first = 0;
            constexpr unsigned second = 1;
            ParallelLoops loops(widthOfLoops);
            UnitQueue queue(program.conventions, 4);
            Vector<Typed::CodeUnit> seeds;
            Vector<Typed::CodeUnit> queued;
            for (uint32_t index = 0; index < program.conventions.numberOfUnits(); ++index) {
                Typed::CodeUnit unit = program.conventions.unitAt(index);
                if (!program.conventions.exists(unit))
                    continue;
                if (unit.role == Typed::CodeRole::GenericCallEntry && program.conventions.hasFusedCallEntry(unit.function))
                    continue;
                seeds.append(unit);
                seeds.append(unit);
                queued.append(unit);
            }
            AOT_TEST_CHECK(queued.size() >= 2);
            queue.addSeeds(seeds.span(), loops);

            Vector<uint32_t> places(queued.size(), [](size_t index) {
                return static_cast<uint32_t>(index);
            });
            Vector<uint32_t> inRuns = UnitQueue::inRunsOfUnitQueue(places.span());
            size_t sizeOfFirstRun = std::min((queued.size() + 1) / 2, UnitQueue::mostUnitsTakenAtOnce);

            // No thread has kept any. So whoever comes first begins with the heaviest unit, and the next with the newest of what the first has left in the list.
            std::optional<Typed::CodeUnit> unitOfFirst = queue.take(first);
            AOT_TEST_CHECK(unitOfFirst && *unitOfFirst == queued.last());
            std::optional<Typed::CodeUnit> unitOfSecond = queue.take(second);
            AOT_TEST_CHECK(unitOfSecond && *unitOfSecond == queued[inRuns[queued.size() - sizeOfFirstRun - 1]]);
            queue.didFinish(first);
            queue.didFinish(second);

            // take() waits for what the other thread holds, so each has a thread of its own.
            std::atomic<size_t> numberTaken { 2 };
            auto takeTheRest = [&](unsigned thread) {
                for (; queue.take(thread); ++numberTaken)
                    queue.didFinish(thread);
            };
            Ref<Thread> threadOfSecond = Thread::create("AOT unit queue test"_s, [&] {
                takeTheRest(second);
            });
            takeTheRest(first);
            threadOfSecond->waitForCompletion();
            AOT_TEST_CHECK(numberTaken.load() == queued.size());
        }
    });
}

// What mention() asks of each mention by finished code. It follows from the tables, so it is the same whichever cycle happens to be done.
//   f1 > f2 > f3 > f4 . f2 > f5 . f5 and f6 name one another . f0 names nobody, and has the lowest rank
static void testTablesLeadFromReferrerToReferrer()
{
    withProgramOfQueueTests(sevenFunctionsForQueueTests(), [&](const ProgramOfQueueTests& program) {
        static constexpr uint32_t cycles[] { 0, 1, 2, 3, 4, 5, 5 };
        static constexpr uint32_t ranks[] { 0, 0, 1, 2, 3, 2, 2 };
        static constexpr NamingInQueueTests namings[] { { 1, 2 }, { 2, 3 }, { 3, 4 }, { 2, 5 }, { 6, 5 }, { 5, 6 } };
        AOT_TEST_CHECK(program.linked.functions.size() >= std::size(cycles));
        Typed::LinkTables tables = tablesWithOrderForQueueTests(program.linked, cycles, ranks, namings);
        UnitQueue queue(program.conventions, tables, program.arrivals, 1);
        auto leadsTo = [&](uint32_t referrer, uint32_t function) {
            return queue.leadsTo(FunctionID(referrer), FunctionID(function));
        };

        AOT_TEST_CHECK(leadsTo(1, 1));
        AOT_TEST_CHECK(leadsTo(1, 2));
        // Through others: f1 copies f2 in line, which copies f3, which calls f4.
        AOT_TEST_CHECK(leadsTo(1, 3));
        AOT_TEST_CHECK(leadsTo(1, 4));
        AOT_TEST_CHECK(leadsTo(2, 4));
        // Two that name one another.
        AOT_TEST_CHECK(leadsTo(5, 6));
        AOT_TEST_CHECK(leadsTo(6, 5));
        AOT_TEST_CHECK(leadsTo(2, 6));
        AOT_TEST_CHECK(leadsTo(1, 6));

        // A lower rank alone leads nowhere: the cycle of f4 does not wait for that of f0.
        AOT_TEST_CHECK(!leadsTo(0, 4));
        AOT_TEST_CHECK(!leadsTo(0, 1));
        AOT_TEST_CHECK(!leadsTo(4, 1));
        AOT_TEST_CHECK(!leadsTo(2, 1));
        AOT_TEST_CHECK(!leadsTo(3, 5));
        AOT_TEST_CHECK(!leadsTo(5, 3));
        AOT_TEST_CHECK(!leadsTo(5, 4));
        AOT_TEST_CHECK(!leadsTo(6, 2));
    });
}

// A list for each function under a name of all who look the name up has as many entries as the square of the program: at a million lines it no longer fits a Vector, and the build ended there.
// A node for the name ties all who look it up to all who lie under it. Each of these functions is named by who makes its closure and by the one call that enters it.
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
            // The order is there: a function under the name waits for the function that calls it, and for no other that looks the name up.
            UnitQueue queue(program.conventions, program.linked, program.arrivals, 1);
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

// Without tables nothing is ordered, so whoever likes may mention whatever there is.
static void testEveryFunctionLeadsToEveryOtherWithoutTables()
{
    withProgramOfQueueTests([&](const ProgramOfQueueTests& program) {
        UnitQueue queue(program.conventions, 1);
        AOT_TEST_CHECK(queue.leadsTo(FunctionID(0u), FunctionID(2u)));
        AOT_TEST_CHECK(queue.leadsTo(FunctionID(2u), FunctionID(0u)));
    });
}

// Only a function that takes what its callers have left waits for them. Whatever else is named comes into the queue at once, while the unit that names it is still open.
// Where every cycle waits for the cycles of its referrers, the second take() never returns.
static void testOnlyWhoeverTakesArrivalsWaits()
{
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Typed::Program> parsed = Typed::parseProgram(Typed::withStandInLibrary(String(oneFunctionCalledOnceAndTwoThatCallEachOther)), build);
        AOT_TEST_CHECK(parsed);
        Typed::StagedBuild stages(WTF::move(parsed), build);
        const PreparedProgram* prepared = stages.prepared();
        AOT_TEST_CHECK(prepared);

        constexpr unsigned threadOfBody = 0;
        constexpr unsigned otherThread = 1;
        Typed::FunctionID body(0u);
        Typed::CodeUnit once { Typed::FunctionID(1u), Typed::CodeRole::Body };
        Typed::CodeUnit ping { Typed::FunctionID(2u), Typed::CodeRole::Body };
        AOT_TEST_CHECK(prepared->arrivals->takesWhatWasLeft(once.function));
        AOT_TEST_CHECK(!prepared->arrivals->takesWhatWasLeft(ping.function));

        ParallelLoops loops(1);
        UnitQueue queue(prepared->plan->conventions(), prepared->facts->linked(), *prepared->arrivals, 2);
        Vector<Typed::CodeUnit> seeds = Typed::unitsNamedOutsideCode(*prepared->program, *prepared->facts, *prepared->plan);
        queue.addSeeds(seeds.span(), loops);

        // A unit of the module's body stays open.
        for (;;) {
            std::optional<Typed::CodeUnit> unit = queue.take(threadOfBody);
            AOT_TEST_CHECK(unit);
            if (unit->function == body)
                break;
            queue.didFinish(threadOfBody);
        }
        queue.mention(otherThread, once);
        queue.mention(otherThread, ping);
        std::optional<Typed::CodeUnit> taken = queue.take(otherThread);
        AOT_TEST_CHECK(taken && *taken == ping);
        queue.didFinish(otherThread);

        // Its cycle is done: whoever finishes it brings in what has waited.
        queue.didFinish(threadOfBody);
        bool hasTakenOnce = false;
        while (std::optional<Typed::CodeUnit> unit = queue.take(threadOfBody)) {
            hasTakenOnce |= *unit == once;
            queue.didFinish(threadOfBody);
        }
        AOT_TEST_CHECK(hasTakenOnce);
    });
}

// What a thread has mentioned and not begun is there for a thread that holds nothing, while the first is still at its unit.
// Where a thread gives units away only when it asks for its next one, the take() of the other never returns.
static void testThreadThatHoldsNothingTakesFromAnother()
{
    withProgramOfQueueTests([](const ProgramOfQueueTests& program) {
        constexpr unsigned busy = 0;
        constexpr unsigned idle = 1;
        Vector<Typed::CodeUnit> units;
        for (uint32_t index = 0; index < program.conventions.numberOfUnits(); ++index) {
            Typed::CodeUnit unit = program.conventions.unitAt(index);
            if (!program.conventions.exists(unit))
                continue;
            if (unit.role == Typed::CodeRole::GenericCallEntry && program.conventions.hasFusedCallEntry(unit.function))
                continue;
            units.append(unit);
        }
        AOT_TEST_CHECK(units.size() >= 4);

        ParallelLoops loops(1);
        UnitQueue queue(program.conventions, 2);
        queue.addSeeds(units.span().first(1), loops);
        std::optional<Typed::CodeUnit> open = queue.take(busy);
        AOT_TEST_CHECK(open && *open == units[0]);
        for (size_t index = 1; index < 4; ++index)
            queue.mention(busy, units[index]);

        // The older half of three, and of that the newest first.
        std::optional<Typed::CodeUnit> taken = queue.take(idle);
        AOT_TEST_CHECK(taken && *taken == units[2]);
        queue.didFinish(idle);
        taken = queue.take(idle);
        AOT_TEST_CHECK(taken && *taken == units[1]);
        queue.didFinish(idle);
        // The only one that is left too.
        taken = queue.take(idle);
        AOT_TEST_CHECK(taken && *taken == units[3]);
        queue.didFinish(idle);

        queue.didFinish(busy);
        AOT_TEST_CHECK(!queue.take(busy));
        AOT_TEST_CHECK(!queue.take(idle));
    });
}

// const thenable = { then(resolve) { resolve(11); resolve(12); } };  thenable.then(function () {});  sink(thenable);
// `then` has a guard, a fast version and a slow one, and it leads to the function that it is handed.
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

// One thread takes every unit. The fast version notes two points, one of them twice and the later one first. The slow version mentions the Body of f2, which nobody has mentioned.
// `isInCycleOfSlowVersion`: f2 stands in the cycle of f1.
static void takeUnitsOfMethodWithAGuard(UnitQueue& queue, std::span<const Typed::CodeUnit> seeds, size_t numberOfFunctions, bool isInCycleOfSlowVersion)
{
    constexpr unsigned thread = 0;
    Typed::FunctionID method(1u);
    Typed::CodeUnit fastVersion { method, Typed::CodeRole::Body };
    Typed::CodeUnit guard { method, Typed::CodeRole::ParameterGuard };
    Typed::CodeUnit slowVersion { method, Typed::CodeRole::SlowBody };
    Typed::CodeUnit handed { Typed::FunctionID(2u), Typed::CodeRole::Body };

    Typed::LeavePoints points;
    points.prepare(numberOfFunctions);
    queue.closePointsIn(points);
    ParallelLoops loops(1);
    queue.addSeeds(seeds, loops);

    Vector<Typed::CodeUnit> taken;
    while (std::optional<Typed::CodeUnit> unit = queue.take(thread)) {
        if (*unit == fastVersion) {
            AOT_TEST_CHECK(!points.isClosed(method) && points.mayHave(method, 6));
            points.note(method, 6);
            points.note(method, 2);
            points.note(method, 6);
        }
        if (*unit == slowVersion) {
            AOT_TEST_CHECK(points.isClosed(method));
            std::span<const uint32_t> noted = points.of(method);
            AOT_TEST_CHECK(noted.size() == 2 && noted[0] == 2 && noted[1] == 6);
            // The cycle of the slow version is not done, so neither is one that waits for it: a mention of a unit that was dropped would end the process.
            queue.mention(thread, handed);
        }
        if (*unit == handed) {
            AOT_TEST_CHECK(points.isClosed(handed.function) == isInCycleOfSlowVersion);
            AOT_TEST_CHECK(points.mayHave(method, 2) && !points.mayHave(method, 4));
            points.note(method, 2);
        }
        taken.append(*unit);
        queue.didFinish(thread);
    }
    size_t placeOfSlowVersion = taken.find(slowVersion);
    AOT_TEST_CHECK(placeOfSlowVersion != notFound && taken.find(fastVersion) < placeOfSlowVersion && taken.find(guard) < placeOfSlowVersion);
    AOT_TEST_CHECK(taken.find(handed) != notFound && taken.find(handed) > placeOfSlowVersion);
    AOT_TEST_CHECK(points.of(method).size() == 2);
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
        UnitQueue queue(prepared.plan->conventions(), prepared.facts->linked(), *prepared.arrivals, 1);
        takeUnitsOfMethodWithAGuard(queue, seeds, prepared.program->functions.size(), false);
    });
}

static void testUnitThatASlowVersionBringsIntoItsOwnCycleFindsThePointsClosed()
{
    withMethodWithAGuard([](const PreparedProgram& prepared, std::span<const Typed::CodeUnit> seeds) {
        constexpr uint32_t cycles[] { 0, 1, 1 };
        constexpr uint32_t ranks[] { 0, 1, 1 };
        constexpr NamingInQueueTests namings[] { { 0, 1 }, { 0, 2 }, { 1, 2 } };
        Typed::LinkTables tables = tablesWithOrderForQueueTests(prepared.facts->linked(), cycles, ranks, namings);
        UnitQueue queue(prepared.plan->conventions(), tables, *prepared.arrivals, 1);
        takeUnitsOfMethodWithAGuard(queue, seeds, prepared.program->functions.size(), true);
    });
}

static void testSlowVersionsComeBehindAllOtherUnitsWithoutTables()
{
    withMethodWithAGuard([](const PreparedProgram& prepared, std::span<const Typed::CodeUnit> seeds) {
        UnitQueue queue(prepared.plan->conventions(), 1);
        takeUnitsOfMethodWithAGuard(queue, seeds, prepared.program->functions.size(), true);
    });
}

std::span<const NativeTest> unitQueueTests()
{
    static constexpr NativeTest tests[] {
        { "unit-queue/heaviest-units-stand-one-to-a-run"_s, testHeaviestUnitsStandOneToARun },
        { "unit-queue/seeds-go-to-the-shared-list-in-runs"_s, testSeedsGoToSharedListInRuns },
        { "unit-queue/a-thread-that-holds-nothing-takes-from-another"_s, testThreadThatHoldsNothingTakesFromAnother },
        { "unit-queue/tables-lead-from-referrer-to-referrer"_s, testTablesLeadFromReferrerToReferrer },
        { "unit-queue/every-function-leads-to-every-other-without-tables"_s, testEveryFunctionLeadsToEveryOtherWithoutTables },
        { "unit-queue/who-may-name-a-function-grows-with-the-program"_s, testWhoMayNameAFunctionGrowsWithTheProgram },
        { "unit-queue/only-whoever-takes-arrivals-waits"_s, testOnlyWhoeverTakesArrivalsWaits },
        { "unit-queue/a-slow-version-comes-behind-the-other-units-of-its-cycle"_s, testSlowVersionComesBehindTheOtherUnitsOfItsCycle },
        { "unit-queue/a-unit-that-a-slow-version-brings-into-its-own-cycle-finds-the-points-closed"_s, testUnitThatASlowVersionBringsIntoItsOwnCycleFindsThePointsClosed },
        { "unit-queue/slow-versions-come-behind-all-other-units-without-tables"_s, testSlowVersionsComeBehindAllOtherUnitsWithoutTables },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
