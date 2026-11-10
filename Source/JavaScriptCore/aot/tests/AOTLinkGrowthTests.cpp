/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTFacts.h"
#include "AOTFamilies.h"
#include "AOTGrowthOfWork.h"
#include "AOTIRParser.h"
#include "AOTLinkFlow.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTTestHarness.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <set>
#include <vector>
#include <wtf/Threading.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> linkGrowthTests();

using namespace Typed;

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

static void testLinkGrowsAsLayoutsOfCopiedFamilyDo()
{
    constexpr unsigned numberOfLayouts = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfFactsForLinkGrowthTests(copyThroughFamilyOfManyLayoutsForLinkGrowthTests(numberOfLayouts));
    std::optional<uint64_t> large = instructionsOfFactsForLinkGrowthTests(copyThroughFamilyOfManyLayoutsForLinkGrowthTests(4 * numberOfLayouts));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String patternBehindManyEarlyReturnsForLinkGrowthTests(unsigned numberOfReturns)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("variable g0 name=\"late\" module=m0 declared={type=f1} initialization=Function function=f1 flags=[IsImmutable]\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Undefined\n    Return v0\n}\n"_s);
    text.append("function f1 name=\"late\" module=m0 position=10 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
    for (unsigned number = 0; number < numberOfReturns; ++number)
        text.append("    Branch v0 -> taken b"_s, 2 * number + 1, "(), notTaken b"_s, 2 * number + 2, "()\n  b"_s, 2 * number + 1, "():\n    Return v0\n  b"_s, 2 * number + 2, "():\n"_s);
    text.append("    v1 = Builtin builtin=RegExp\n    v2 = Builtin builtin=GlobalThis\n    v3 = PropGet v2 key=\"String\" [isFreeName]\n    v4 = String string=\"te\"\n    v5 = Call v3, -, v4\n"_s);
    text.append("    v6 = String string=\"la\"\n    v7 = Add v6, v5\n    v8 = Construct v1, -, v7\n    Return v8\n}\n"_s);
    return text.toString();
}

static size_t numberOfRegExpTextsWorkedOutForLinkGrowthTests(const String& text)
{
    size_t numberOfTexts = 0;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        AOT_TEST_CHECK(stages.facts());
        numberOfTexts = stages.facts()->regExpTextsWorkedOut().size() + stages.facts()->regExpTextsInSight().size();
    });
    return numberOfTexts;
}

static void testRehearsalOpensAsManyRegionsAsNumberSays()
{
    AOT_TEST_CHECK(numberOfRegExpTextsWorkedOutForLinkGrowthTests(patternBehindManyEarlyReturnsForLinkGrowthTests(1024)) == 1);
    AOT_TEST_CHECK(!numberOfRegExpTextsWorkedOutForLinkGrowthTests(patternBehindManyEarlyReturnsForLinkGrowthTests(1025)));
}

static void testCalleesOfCallStayAsListedInRecordAndBeyondIt()
{
    using Callees = LinkFlow::CalleesOfCall;
    constexpr uint32_t mostOfEach = 40;
    auto functionAt = [](uint32_t place) {
        return FunctionID(1000 - place);
    };
    auto builtinAt = [](uint32_t place) {
        return BuiltinID(500 - place);
    };
    auto sharedAt = [](uint32_t place) {
        return 7 * place;
    };
    auto holdsFirst = [&](const Callees& callees, uint32_t number) {
        if (callees.listed().size() != number || callees.builtins().size() != number || callees.shared().size() != number)
            return false;
        for (uint32_t place = 0; place < number; ++place) {
            if (callees.listed()[place] != functionAt(place) || callees.builtins()[place] != builtinAt(place) || callees.shared()[place] != sharedAt(place))
                return false;
        }
        return callees.mayEnterLost && !callees.mayEnterUnseen;
    };
    auto holdsNothing = [](const Callees& callees) {
        return callees.listed().isEmpty() && callees.builtins().isEmpty() && callees.shared().isEmpty() && !callees.mayEnterLost && !callees.mayEnterUnseen;
    };

    Vector<Callees> rows;
    Callees callees;
    AOT_TEST_CHECK(holdsNothing(callees));
    callees.mayEnterLost = true;
    for (uint32_t number = 0; number <= mostOfEach; ++number) {
        AOT_TEST_CHECK(holdsFirst(callees, number));
        rows.append(callees);
        callees.list(functionAt(number));
        callees.add(builtinAt(number));
        callees.add(builtinAt(number / 2));
        callees.addShared(sharedAt(number));
        callees.addShared(sharedAt(number / 2));
    }
    for (uint32_t number = 0; number <= mostOfEach; ++number)
        AOT_TEST_CHECK(holdsFirst(rows[number], number));

    for (uint32_t number : { 1u, 2u, 3u, mostOfEach }) {
        Callees taken(WTF::move(rows[number]));
        AOT_TEST_CHECK(holdsFirst(taken, number) && holdsNothing(rows[number]));
        rows[number] = taken;
        taken.list(functionAt(number));
        taken.mayEnterUnseen = true;
        AOT_TEST_CHECK(holdsFirst(rows[number], number) && taken.listed().size() == number + 1);
        taken = WTF::move(rows[number]);
        AOT_TEST_CHECK(holdsFirst(taken, number));
        rows[number] = WTF::move(taken);
        AOT_TEST_CHECK(holdsFirst(rows[number], number));
    }

    Callees growsWhileGoneThrough;
    growsWhileGoneThrough.addShared(sharedAt(1));
    for (size_t place = 0; place < growsWhileGoneThrough.shared().size(); ++place) {
        AOT_TEST_CHECK(growsWhileGoneThrough.shared()[place] == sharedAt(1));
        growsWhileGoneThrough.add(builtinAt(0));
        growsWhileGoneThrough.add(builtinAt(1));
    }
    AOT_TEST_CHECK(growsWhileGoneThrough.shared().size() == 1 && growsWhileGoneThrough.builtins().size() == 2);

    constexpr std::array<uint32_t, 3> replacement { 5, 3, 4 };
    for (uint32_t number = 0; number <= mostOfEach; ++number) {
        Callees& row = rows[number];
        row.sortListedAndBuiltins();
        AOT_TEST_CHECK(row.listed().size() == number && std::ranges::is_sorted(row.listed()));
        AOT_TEST_CHECK(row.builtins().size() == number && std::ranges::is_sorted(row.builtins()));
        row.removeListedMatching([](FunctionID callee) {
            return !!(callee.index() % 2);
        });
        AOT_TEST_CHECK(row.listed().size() == (number + 1) / 2 && std::ranges::is_sorted(row.listed()) && row.listed().contains(functionAt(0)) == !!number && !row.listed().contains(functionAt(1)));
        row.replaceShared(replacement);
        AOT_TEST_CHECK(std::ranges::equal(row.shared(), replacement));
        row.clearShared();
        row.clearBuiltins();
        AOT_TEST_CHECK(row.shared().isEmpty() && row.builtins().isEmpty() && row.listed().size() == (number + 1) / 2 && row.mayEnterLost);
    }
}

static void testTellingNameOfFunctionAgainDoesNothing()
{
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        String text = makeString(
            "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s,
            "function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Undefined\n    Return v0\n}\n"_s,
            "function f1 name=\"plain\" module=m0 position=10 thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Undefined\n    Return v0\n}\n"_s);
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(program);
        const ParallelLoops& loops = build.parallelLoops();
        ThinSummaries summaries = scanBodies(*program, build.diagnostics(), loops);
        Families families(*program, Families::ClassesAlone, loops);
        BodyIndexTable bodies(static_cast<uint32_t>(program->functions.size()), loops.width());
        OwnedLooksAtBodies looks = makeEmptyLooksAtBodies(bodies);
        LinkFlow flow(*program, summaries, families, *looks, allMechanisms, build);
        for (uint32_t function : { 0u, 1u }) {
            AOT_TEST_CHECK(!isLibraryCode(*program, program->at(FunctionID(function))));
            flow.tellNameThatBecomesText(FunctionID(function));
            flow.tellNameThatBecomesText(FunctionID(function));
        }
    });
}

static void testEightThreadsGetOneNumberForEachSetOfFour()
{
    constexpr unsigned numberOfThreads = 8;
    constexpr uint32_t numberOfSets = 20000;
    constexpr uint32_t steps[numberOfThreads] = { 1, 3, 7, 11, 13, 17, 19, 19999 };
    using Members = std::array<FunctionID, SetsOfFunctionsInLinkFlow::mostMembers>;
    auto sortedMembersOf = [](uint32_t index, Members& members) {
        size_t size = 2 + index % 3;
        for (size_t place = 0; place < size; ++place)
            members[place] = FunctionID(1 + index * 4 + static_cast<uint32_t>(place));
        return std::span<const FunctionID>(members).first(size);
    };

    SetsOfFunctionsInLinkFlow sets;
    std::array<std::vector<uint32_t>, numberOfThreads> numbersOfThread;
    std::array<bool, numberOfThreads> everySetWasReadBack { };
    std::atomic<unsigned> numberOfThreadsAtStart { 0 };
    Vector<Ref<Thread>> threads;
    for (unsigned thread = 0; thread < numberOfThreads; ++thread) {
        threads.append(Thread::create("AOT sets of four test"_s, [&, thread] {
            std::vector<uint32_t>& numbers = numbersOfThread[thread];
            numbers.resize(numberOfSets);
            bool wasReadBack = true;
            ++numberOfThreadsAtStart;
            while (numberOfThreadsAtStart.load() < numberOfThreads)
                Thread::yield();
            for (uint32_t turn = 0; turn < numberOfSets; ++turn) {
                uint32_t index = static_cast<uint32_t>((static_cast<uint64_t>(turn) * steps[thread] + thread * 2500) % numberOfSets);
                Members members { };
                std::span<const FunctionID> sortedMembers = sortedMembersOf(index, members);
                numbers[index] = sets.numberOf(sortedMembers);
                wasReadBack &= std::ranges::equal(sets.membersOf(numbers[index]), sortedMembers);
            }
            everySetWasReadBack[thread] = wasReadBack;
        }));
    }
    for (Ref<Thread>& thread : threads)
        thread->waitForCompletion();

    for (unsigned thread = 0; thread < numberOfThreads; ++thread) {
        AOT_TEST_CHECK(everySetWasReadBack[thread]);
        AOT_TEST_CHECK(numbersOfThread[thread] == numbersOfThread[0]);
    }
    std::set<uint32_t> distinct(numbersOfThread[0].begin(), numbersOfThread[0].end());
    AOT_TEST_CHECK(distinct.size() == numberOfSets && sets.size() == numberOfSets);
}

static void testEightThreadsFindWhatOneOfThemWasGiven()
{
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        String text = makeString(
            "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s,
            "function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Undefined\n    Return v0\n}\n"_s);
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(program);
        const ParallelLoops& loops = build.parallelLoops();
        ThinSummaries summaries = scanBodies(*program, build.diagnostics(), loops);
        Families families(*program, Families::ClassesAlone, loops);
        BodyIndexTable bodies(static_cast<uint32_t>(program->functions.size()), loops.width());
        OwnedLooksAtBodies looks = makeEmptyLooksAtBodies(bodies);
        LinkFlow flow(*program, summaries, families, *looks, allMechanisms, build);

        constexpr unsigned numberOfThreads = 8;
        constexpr uint32_t numberOfQuestions = 20000;
        constexpr uint32_t steps[numberOfThreads] = { 1, 3, 7, 11, 13, 17, 19, 19999 };
        constexpr LinkFlow::MadeOnDemand kind = LinkFlow::MadeOnDemand::Contents;
        auto wordsOf = [](uint32_t index) {
            return static_cast<uint64_t>(index) << 32 | (index * 7);
        };
        auto givenFor = [](uint32_t index) {
            return index * 3 + 1;
        };
        AOT_TEST_CHECK(!flow.findWhatSomeLaneWasGiven(kind, wordsOf(0)));

        std::array<bool, numberOfThreads> everyAnswerWasTheOneGiven { };
        std::atomic<unsigned> numberOfThreadsAtStart { 0 };
        Vector<Ref<Thread>> threads;
        for (unsigned thread = 0; thread < numberOfThreads; ++thread) {
            threads.append(Thread::create("AOT given to some lane test"_s, [&, thread] {
                bool wasTheOneGiven = true;
                ++numberOfThreadsAtStart;
                while (numberOfThreadsAtStart.load() < numberOfThreads)
                    Thread::yield();
                for (uint32_t turn = 0; turn < numberOfQuestions; ++turn) {
                    uint32_t index = static_cast<uint32_t>((static_cast<uint64_t>(turn) * steps[thread] + thread * 2500) % numberOfQuestions);
                    if (std::optional<uint32_t> known = flow.findWhatSomeLaneWasGiven(kind, wordsOf(index)))
                        wasTheOneGiven &= *known == givenFor(index);
                    else
                        flow.keepWhatSomeLaneWasGiven(kind, wordsOf(index), givenFor(index));
                }
                everyAnswerWasTheOneGiven[thread] = wasTheOneGiven;
            }));
        }
        for (Ref<Thread>& thread : threads)
            thread->waitForCompletion();

        for (unsigned thread = 0; thread < numberOfThreads; ++thread)
            AOT_TEST_CHECK(everyAnswerWasTheOneGiven[thread]);
        for (uint32_t index = 0; index < numberOfQuestions; ++index)
            AOT_TEST_CHECK(flow.findWhatSomeLaneWasGiven(kind, wordsOf(index)) == givenFor(index));
        AOT_TEST_CHECK(!flow.findWhatSomeLaneWasGiven(LinkFlow::MadeOnDemand::ContentsOfFamily, wordsOf(0)));
    });
}

std::span<const NativeTest> linkGrowthTests()
{
    static constexpr NativeTest tests[] {
        { "link-growth/link-grows-as-the-layouts-of-a-family-that-is-copied-do"_s, testLinkGrowsAsLayoutsOfCopiedFamilyDo },
        { "link-growth/callees-of-a-call-stay-as-listed-in-the-record-and-beyond-it"_s, testCalleesOfCallStayAsListedInRecordAndBeyondIt },
        { "link-growth/the-rehearsal-of-regexp-texts-opens-as-many-regions-as-a-number-says"_s, testRehearsalOpensAsManyRegionsAsNumberSays },
        { "link-growth/telling-the-name-of-a-function-again-does-nothing"_s, testTellingNameOfFunctionAgainDoesNothing },
        { "link-growth/eight-threads-get-one-number-for-each-set-of-four"_s, testEightThreadsGetOneNumberForEachSetOfFour },
        { "link-growth/eight-threads-find-what-one-of-them-was-given"_s, testEightThreadsFindWhatOneOfThemWasGiven },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
