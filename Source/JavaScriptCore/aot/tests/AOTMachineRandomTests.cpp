/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineTarget.h"
#include "AOTMachineTestSupport.h"
#include "AOTStubAssembler.h"
#include <array>
#include <cstring>
#include <expected>
#include <limits>
#include <wtf/DataLog.h>
#include <wtf/WeakRandom.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

struct RandomTestCase {
    RandomProcedureOptions options { };
    bool savesFrameOnSuspend { false };
    bool usesFeaturesOfHost { false };
    std::optional<uint32_t> branchReach { };
};

static constexpr unsigned numberOfSeedsInRandomTest = 300;
static constexpr unsigned numberOfSeedsInRandomDeterminismTest = 100;
static constexpr unsigned numberOfSeedsInRandomCostTest = 96;
static constexpr unsigned numberOfArgumentPairsInRandomTest = 6;
static constexpr int64_t argumentsOfRandomTest[] = { 0, 1, -1, 2, 5, 13, 31, 100, 65537, 0x7fffffff, -0x80000000ll, 0x123456789ll, std::numeric_limits<int64_t>::min() };

static ProcedureTraits traitsOfRandomTest(const RandomTestCase& testCase)
{
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = testCase.savesFrameOnSuspend;
    traits.hasHandlers = testCase.options.withLandingPads;
    if (testCase.usesFeaturesOfHost)
        traits.features = CPUFeatureSet::host();
    return traits;
}

[[noreturn]] static void failRandomTest(const RandomTestCase& testCase, TestProcedure& test, int line, const char* condition)
{
    const RandomProcedureOptions& options = testCase.options;
    dataLogLn("seed ", options.seed, ": ", options.numberOfBlocks, " blocks, ", options.operationsPerBlock, " operations per block, calls ", options.withCalls,
        ", rare regions ", options.withRareRegions, ", landing pads ", options.withLandingPads, ", numbers ", options.withNumbers, ", scaled sums ", options.withScaledSums,
        ", saves its frame ", testCase.savesFrameOnSuspend, ", features of the host ", testCase.usesFeaturesOfHost);
    if (const std::optional<Failure>& failure = test.procedure().failure())
        dataLogLn("reported: ", failure->kind, " at @", failure->operation);
    dataLogLn(test.printedProcedure(), test.printedAllocation(), test.printedFrame());
    failNativeTest(__FILE__, line, condition);
}

#define AOT_RANDOM_TEST_CHECK(testCase, test, ...) do { \
        if (!(__VA_ARGS__)) [[unlikely]] \
            failRandomTest(testCase, test, __LINE__, #__VA_ARGS__); \
    } while (false)

struct RandomTestCensus {
    void add(const Procedure& procedure)
    {
        bool hasLandingPad = false;
        bool hasRareEdge = false;
        bool hasNumber = false;
        for (const Block* block : procedure.blocks()) {
            hasLandingPad |= block->isLandingPad();
            for (const Edge& edge : block->successors())
                hasRareEdge |= edge.isRare();
        }
        for (uint32_t index = 0; index < procedure.numberOfOperations(); ++index)
            hasNumber |= procedure.operationAt(index)->type().isFloat();
        withLandingPads += hasLandingPad;
        withRareEdges += hasRareEdge;
        withNumbers += hasNumber;
    }

    unsigned withLandingPads { 0 };
    unsigned withRareEdges { 0 };
    unsigned withNumbers { 0 };
    unsigned withSeveralAnswers { 0 };
    unsigned withScaledSumsInOneInstruction { 0 };
    unsigned withSelectsThatTestTheirArm { 0 };
};

static void countScaledSumsOfRandomTest(const Procedure& procedure, RandomTestCensus& census)
{
    const Liveness& liveness = procedure.liveness();
    bool addsInOneInstruction = false;
    bool testsArm = false;
    for (uint32_t index = 0; index < procedure.numberOfOperations(); ++index) {
        const Operation* operation = procedure.operationAt(index);
        if (operation->hasNoPosition() || liveness.of(operation).has(ValueFlag::IsDropped))
            continue;
        addsInOneInstruction |= addsScaledIndex(liveness, operation);
        if (operation->opcode() != Select || liveness.isFused(operation))
            continue;
        forEachLeaf(liveness, operation, [&](const Operation* user, unsigned childIndex, const Operation* leaf) {
            testsArm |= user->child(childIndex) != leaf;
        });
    }
    census.withScaledSumsInOneInstruction += addsInOneInstruction;
    census.withSelectsThatTestTheirArm += testsArm;
}

static void placePinnedConstantsForRandomTest(const Procedure& procedure, TestInstance& instance)
{
    for (uint32_t index = 0; index < procedure.numberOfOperations(); ++index) {
        const Operation* operation = procedure.operationAt(index);
        if (!operation->isConstant())
            continue;
        uint64_t bits = static_cast<uint64_t>(operation->constantBits());
        std::optional<PinnedConstant> pinned = findPinnedConstant(bits);
        if (!pinned || !pinned->offset)
            continue;
        int32_t offset = *pinned->offset;
        AOT_TEST_CHECK(pinned->reg == instanceGPR);
        AOT_TEST_CHECK(offset >= -static_cast<int32_t>(sizeof(instance.wordsBelow)));
        AOT_TEST_CHECK(offset + static_cast<int32_t>(sizeof(bits)) <= static_cast<int32_t>(sizeof(instance.words)));
        std::memcpy(static_cast<uint8_t*>(instance.pointer()) + offset, &bits, sizeof(bits));
    }
}

static void runRandomTestCase(const RandomTestCase& testCase, RandomTestCensus& census)
{
    TestProcedure test(traitsOfRandomTest(testCase));
    auto program = buildRandomProcedure(test, testCase.options);
    census.add(test.procedure());

    TestInstance instance;
    AOT_RANDOM_TEST_CHECK(testCase, test, test.procedure().traits().checksAllocation);
    AOT_RANDOM_TEST_CHECK(testCase, test, test.prepare());
    AOT_RANDOM_TEST_CHECK(testCase, test, test.compile(instance.pointer(), testCase.branchReach));
    placePinnedConstantsForRandomTest(test.procedure(), instance);
    countScaledSumsOfRandomTest(test.procedure(), census);

    WeakRandom random(static_cast<unsigned>(testCase.options.seed) + 0x9e3779b9u);
    auto nextArgument = [&] {
        if (random.getUint32(2))
            return static_cast<int64_t>(random.getUint64());
        return argumentsOfRandomTest[random.getUint32(std::size(argumentsOfRandomTest))];
    };
    int64_t firstAnswer = 0;
    bool hasSeveralAnswers = false;
    for (unsigned round = 0; round < numberOfArgumentPairsInRandomTest; ++round) {
        int64_t a = nextArgument();
        int64_t b = nextArgument();
        int64_t expected = interpret(*program, a, b);
        int64_t answer = test.call<int64_t>(a, b);
        if (answer != expected)
            dataLogLn("called with ", a, " and ", b, " the code answers ", answer, ", the interpreter ", expected);
        AOT_RANDOM_TEST_CHECK(testCase, test, answer == expected);
        if (!round)
            firstAnswer = answer;
        hasSeveralAnswers |= answer != firstAnswer;
    }
    census.withSeveralAnswers += hasSeveralAnswers;
}

static void runRandomTestCases(RandomTestCase testCase)
{
    RandomTestCensus census;
    for (unsigned count = 0; count < numberOfSeedsInRandomTest; ++count, ++testCase.options.seed)
        runRandomTestCase(testCase, census);

    const RandomProcedureOptions& options = testCase.options;
    AOT_TEST_CHECK(!!census.withLandingPads == options.withLandingPads);
    AOT_TEST_CHECK(census.withRareEdges || !(options.withRareRegions || options.withLandingPads));
    AOT_TEST_CHECK(!!census.withNumbers == options.withNumbers);
    AOT_TEST_CHECK(2 * census.withSeveralAnswers > numberOfSeedsInRandomTest);
    if (options.withScaledSums) {
        AOT_TEST_CHECK(4 * census.withScaledSumsInOneInstruction > numberOfSeedsInRandomTest);
        AOT_TEST_CHECK(10 * census.withSelectsThatTestTheirArm > numberOfSeedsInRandomTest);
    }
}

static void testRandomProceduresWithEverything()
{
    runRandomTestCases({ .options = { .seed = 1 } });
}

static void testRandomProceduresOfPlainIntegers()
{
    runRandomTestCases({ .options = { .seed = 1001, .withCalls = false, .withRareRegions = false, .withLandingPads = false, .withNumbers = false } });
}

static void testRandomProceduresWithCalls()
{
    runRandomTestCases({ .options = { .seed = 2001, .withRareRegions = false, .withLandingPads = false, .withNumbers = false } });
}

static void testRandomProceduresWithRareRegions()
{
    runRandomTestCases({ .options = { .seed = 3001, .withCalls = false, .withLandingPads = false, .withNumbers = false } });
}

static void testRandomProceduresWithLandingPads()
{
    runRandomTestCases({ .options = { .seed = 4001, .withCalls = false, .withRareRegions = false, .withNumbers = false } });
}

static void testRandomProceduresWithNumbers()
{
    runRandomTestCases({ .options = { .seed = 5001, .withCalls = false, .withRareRegions = false, .withLandingPads = false } });
}

static void testRandomProceduresOfManySmallBlocks()
{
    runRandomTestCases({ .options = { .seed = 6001, .numberOfBlocks = 48, .operationsPerBlock = 2 } });
}

static void testRandomProceduresOfFewLongBlocks()
{
    runRandomTestCases({ .options = { .seed = 7001, .numberOfBlocks = 3, .operationsPerBlock = 48 } });
}

static void testRandomProceduresThatSaveTheirFrame()
{
    runRandomTestCases({ .options = { .seed = 8001 }, .savesFrameOnSuspend = true });
}

static void testRandomProceduresWithFeaturesOfHost()
{
    runRandomTestCases({ .options = { .seed = 9001 }, .usesFeaturesOfHost = true });
}

static void testRandomProceduresWithShortBranchReach()
{
    runRandomTestCases({ .options = { .seed = 14000 }, .branchReach = 256 });
}

static void testRandomProceduresWithScaledSums()
{
    runRandomTestCases({ .options = { .seed = 12001, .withScaledSums = true } });
    runRandomTestCases({ .options = { .seed = 13001, .withCalls = false, .withLandingPads = false, .withNumbers = false, .withScaledSums = true } });
    runRandomTestCases({ .options = { .seed = 14001, .withScaledSums = true }, .savesFrameOnSuspend = true });
}

static constexpr ASCIILiteral stepsOfRandomTestBuild[] = { "operations"_s, "order"_s, "liveness"_s, "allocation"_s, "frame"_s };

struct RandomTestBuild {
    std::array<String, std::size(stepsOfRandomTestBuild)> texts;
    RelocatableCode code;
};

static RandomTestBuild buildForRandomTest(const RandomTestCase& testCase, size_t bytesAheadInArena)
{
    TestProcedure test(traitsOfRandomTest(testCase));
    test.procedure().arena().allocate(bytesAheadInArena);
    auto program = buildRandomProcedure(test, testCase.options);
    AOT_RANDOM_TEST_CHECK(testCase, test, test.prepare());

    CCallHelpers jit;
    RelocationRecorder recorder(jit, AssemblerPolicy::forImage(test.procedure().traits().features));
    readTagsFromInstance(jit);
    generate(test.procedure(), jit, recorder);
    AOT_RANDOM_TEST_CHECK(testCase, test, !test.procedure().failure());
    std::expected<RelocatableCode, LinkFailure> code = recorder.finalize(LinkBuffer::Profile::FTL, nullptr);
    AOT_RANDOM_TEST_CHECK(testCase, test, code.has_value());
    return { { test.printedProcedure(), test.printedOrder(), test.printedLiveness(), test.printedAllocation(), test.printedFrame() }, WTF::move(*code) };
}

static void testRandomProceduresAreBuiltTheSameWayTwice()
{
    constexpr size_t bytesAheadOfSecondBuild = 40 * KB + 8;

    RandomTestCase testCase { .options = { .seed = 10001 } };
    for (unsigned count = 0; count < numberOfSeedsInRandomDeterminismTest; ++count, ++testCase.options.seed) {
        RandomTestBuild first = buildForRandomTest(testCase, 0);
        RandomTestBuild second = buildForRandomTest(testCase, bytesAheadOfSecondBuild);
        auto expectSame = [&](bool isSame, ASCIILiteral what) {
            if (!isSame)
                dataLogLn("two builds of seed ", testCase.options.seed, " differ in the ", what);
            AOT_TEST_CHECK(isSame);
        };
        for (size_t step = 0; step < std::size(stepsOfRandomTestBuild); ++step) {
            AOT_TEST_CHECK(!first.texts[step].isNull());
            if (first.texts[step] != second.texts[step])
                dataLogLn(first.texts[step], "\n", second.texts[step]);
            expectSame(first.texts[step] == second.texts[step], stepsOfRandomTestBuild[step]);
        }
        AOT_TEST_CHECK(!first.code.bytes.isEmpty());
        expectSame(first.code.bytes == second.code.bytes, "bytes"_s);
        expectSame(first.code.relocations == second.code.relocations, "relocations"_s);
    }
}

struct RandomTestCost {
    uint64_t operations { 0 };
    uint64_t arenaBytes { 0 };
};

static RandomTestCost costOfRandomTestCases(unsigned scale)
{
    constexpr unsigned smallestNumberOfBlocks = 48;

    RandomTestCost cost;
    for (unsigned index = 0; index < numberOfSeedsInRandomCostTest; ++index) {
        RandomTestCase testCase { .options = { .seed = 11001 + index, .numberOfBlocks = scale * (smallestNumberOfBlocks + index % smallestNumberOfBlocks) } };
        TestProcedure test(traitsOfRandomTest(testCase));
        auto program = buildRandomProcedure(test, testCase.options);
        AOT_RANDOM_TEST_CHECK(testCase, test, test.compile());
        cost.operations += test.procedure().numberOfOperations();
        cost.arenaBytes += test.procedure().arena().bytesAllocatedSinceReset();
    }
    return cost;
}

static void testRandomProceduresCostArenaBytesByTheOperation()
{
    RandomTestCost smaller = costOfRandomTestCases(1);
    RandomTestCost larger = costOfRandomTestCases(2);
    bool isFarLarger = 2 * larger.operations >= 3 * smaller.operations;
    bool isLinear = 20 * larger.arenaBytes * smaller.operations <= 21 * smaller.arenaBytes * larger.operations;
    if (!isFarLarger || !isLinear)
        dataLogLn(smaller.operations, " operations take ", smaller.arenaBytes, " bytes of the arena, ", larger.operations, " operations take ", larger.arenaBytes);
    AOT_TEST_CHECK(isFarLarger);
    AOT_TEST_CHECK(isLinear);
}

static void testRandomProceduresOfSeedsThatFailed()
{
    static constexpr uint64_t seeds[] = {
        6266, 8891, 9734, 9868, 10624, 12937, 18561, 19979, 23266, 33278, 42721, 55426, 64842, 66030, 99693, 103626, 117513, 123622, 144003, 147917, 160462, 196822, 229040, 238557, 308821, 336899, 362685, 391435,
    };
    for (uint64_t seed : seeds) {
        String reason = fuzzOneProcedure(seed, AllocationFuzzRequest { }.numberOfBlocks);
        if (!reason.isNull())
            dataLogLn("seed ", seed, ": ", reason);
        AOT_TEST_CHECK(reason.isNull());
    }
    static constexpr std::pair<uint64_t, unsigned> seedsWithBlocks[] = { { 412999, 16 }, { 476261, 16 }, { 709891, 32 } };
    for (auto [seed, numberOfBlocks] : seedsWithBlocks) {
        String reason = fuzzOneProcedure(seed, numberOfBlocks);
        if (!reason.isNull())
            dataLogLn("seed ", seed, " with ", numberOfBlocks, " blocks: ", reason);
        AOT_TEST_CHECK(reason.isNull());
    }
}

} // namespace Machine

std::span<const NativeTest> machineRandomTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/random/seeds-that-failed"_s, Machine::testRandomProceduresOfSeedsThatFailed },
        { "machine/random/everything"_s, Machine::testRandomProceduresWithEverything },
        { "machine/random/plain-integers"_s, Machine::testRandomProceduresOfPlainIntegers },
        { "machine/random/calls"_s, Machine::testRandomProceduresWithCalls },
        { "machine/random/rare-regions"_s, Machine::testRandomProceduresWithRareRegions },
        { "machine/random/landing-pads"_s, Machine::testRandomProceduresWithLandingPads },
        { "machine/random/numbers"_s, Machine::testRandomProceduresWithNumbers },
        { "machine/random/many-small-blocks"_s, Machine::testRandomProceduresOfManySmallBlocks },
        { "machine/random/few-long-blocks"_s, Machine::testRandomProceduresOfFewLongBlocks },
        { "machine/random/functions-that-save-their-frame"_s, Machine::testRandomProceduresThatSaveTheirFrame },
        { "machine/random/features-of-the-host"_s, Machine::testRandomProceduresWithFeaturesOfHost },
        { "machine/random/scaled-sums"_s, Machine::testRandomProceduresWithScaledSums },
        { "machine/random/short-branch-reach"_s, Machine::testRandomProceduresWithShortBranchReach },
        { "machine/random/same-seed-same-bytes"_s, Machine::testRandomProceduresAreBuiltTheSameWayTwice },
        { "machine/random/twice-the-operations-cost-twice-the-arena"_s, Machine::testRandomProceduresCostArenaBytesByTheOperation },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
