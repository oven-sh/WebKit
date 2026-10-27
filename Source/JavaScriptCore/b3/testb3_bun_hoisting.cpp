/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3DominatorJumpPointers.h"
#include "B3Dominators.h"
#include "B3EliminateDeadCode.h"
#include "B3HoistLoopInvariantValues.h"
#include "B3MovedLoadCheck.h"
#include "B3RegisterPressure.h"
#include <wtf/WeakRandom.h>

static Policy byFrequencyPolicy()
{
    return policyWith([](Policy& policy) {
        policy.optimization.placesConstantsByFrequency = true;
    });
}

static unsigned int64ConstantCount(BasicBlock* block, int64_t constant)
{
    unsigned result = 0;
    for (Value* value : *block)
        result += value->isInt64(constant);
    return result;
}

static void testMoveConstantsMaterializesByFrequency()
{
    constexpr int64_t large = 0x123456789abcll;
    struct Case {
        bool byFrequency;
        double userFrequency;
        bool isMaterializedAtUsers;
    };
    for (Case testCase : { Case { true, 0.01, true }, Case { false, 0.01, false }, Case { true, 0.75, false } }) {
        Procedure proc(policyWith([&](Policy& policy) {
            policy.optimization.placesConstantsByFrequency = testCase.byFrequency;
        }));
        Graph graph { proc };
        BranchToRareBlocks blocks(graph, testCase.userFrequency);
        Value* inFirstRare = graph.make(blocks.firstRare, Add, blocks.operand, graph.int64(blocks.firstRare, large));
        Value* inSecondRare = graph.make(blocks.secondRare, Mul, blocks.operand, graph.int64(blocks.secondRare, large));
        blocks.finish(graph, inFirstRare, inSecondRare, blocks.operand);

        moveConstants(proc);
        validate(proc);
        CHECK_EQ(int64ConstantCount(blocks.root, large), testCase.isMaterializedAtUsers ? 0u : 1u);
        CHECK_EQ(int64ConstantCount(blocks.firstRare, large), testCase.isMaterializedAtUsers ? 1u : 0u);
        CHECK_EQ(int64ConstantCount(blocks.secondRare, large), testCase.isMaterializedAtUsers ? 1u : 0u);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 1, 5, nullptr), large + 5);
        CHECK_EQ(invoke<int64_t>(*code, 2, 5, nullptr), large * 5);
        CHECK_EQ(invoke<int64_t>(*code, 3, 5, nullptr), 5);
    }
}

static void testMoveConstantsByFrequencyHoistsOutOfLoop()
{
    constexpr int64_t large = 0x123456789abcll;
    Procedure proc(byFrequencyPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    loop.header->setFrequency(10);
    loop.body->setFrequency(10);
    graph.increase(loop.body, loop.sum, large);
    loop.endBody(loop.body);

    moveConstants(proc);
    validate(proc);
    CHECK_EQ(int64ConstantCount(loop.root, large), 1u);
    CHECK_EQ(int64ConstantCount(loop.body, large), 0u);
    CHECK_EQ(run(*compileProc(proc), 3), 3 * large);
}

// Upstream keeps whichever of the two constants a hash table yields first.
static void testMoveConstantsSharesNegatedAddend()
{
    constexpr int64_t large = 0x123456789abcll;
    for (bool byFrequency : { true, false }) {
        for (int64_t firstAddend : { large, -large }) {
            Procedure proc(policyWith([&](Policy& policy) {
                policy.optimization.placesConstantsByFrequency = byFrequency;
            }));
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
            Value* first = graph.make(root, Add, arguments[0], graph.int64(root, firstAddend));
            Value* second = graph.make(root, Add, arguments[1], graph.int64(root, -firstAddend));
            graph.ret(root, graph.make(root, BitXor, first, second));
            resetAndValidate(proc);

            moveConstants(proc);
            eliminateDeadCode(proc);
            validate(proc);
            CHECK_EQ(numberOfValues(proc, Add), 1u);
            CHECK_EQ(numberOfValues(proc, Sub), 1u);
            CHECK_EQ(int64ConstantCount(root, large) + int64ConstantCount(root, -large), 1u);
            if (byFrequency)
                CHECK_EQ(int64ConstantCount(root, large), 1u);
            CHECK_EQ(invoke<int64_t>(*compileProc(proc), static_cast<int64_t>(5), static_cast<int64_t>(7)), (5 + firstAddend) ^ (7 - firstAddend));
        }
    }
}

static unsigned linearDepth(B3::Dominators& dominators, BasicBlock* block)
{
    unsigned result = 0;
    while ((block = dominators.idom(block)))
        ++result;
    return result;
}

static BasicBlock* linearDominatorAtDepth(B3::Dominators& dominators, BasicBlock* block, unsigned depth)
{
    for (unsigned steps = linearDepth(dominators, block) - depth; steps--;)
        block = dominators.idom(block);
    return block;
}

static BasicBlock* linearNearestCommonDominator(B3::Dominators& dominators, BasicBlock* a, BasicBlock* b)
{
    unsigned commonDepth = std::min(linearDepth(dominators, a), linearDepth(dominators, b));
    a = linearDominatorAtDepth(dominators, a, commonDepth);
    b = linearDominatorAtDepth(dominators, b, commonDepth);
    while (a != b) {
        a = dominators.idom(a);
        b = dominators.idom(b);
    }
    return a;
}

static BasicBlock* linearLeastFrequentBetween(B3::Dominators& dominators, BasicBlock* from, BasicBlock* to)
{
    BasicBlock* least = nullptr;
    for (BasicBlock* block = from; block != to; block = dominators.idom(block)) {
        if (!least || block->frequency() < least->frequency())
            least = block;
    }
    return least;
}

static void testDominatorJumpPointersMatchLinearWalk()
{
    constexpr unsigned numberOfMainBlocks = 200;
    Procedure proc;
    Graph graph { proc };
    Vector<BasicBlock*> mainBlocks;
    Vector<BasicBlock*> sideBlocks;
    for (unsigned k = 0; k < numberOfMainBlocks; ++k)
        mainBlocks.append(proc.addBlock());
    for (unsigned k = 0; k + 1 < numberOfMainBlocks; ++k)
        sideBlocks.append(proc.addBlock());
    Value* x = cCallArgumentValues<int64_t>(proc, mainBlocks[0])[0];
    for (unsigned k = 0; k + 1 < numberOfMainBlocks; ++k) {
        graph.branch(mainBlocks[k], x, mainBlocks[k + 1], sideBlocks[k]);
        graph.ret(sideBlocks[k], graph.int64(sideBlocks[k], 0));
    }
    graph.ret(mainBlocks.last(), graph.int64(mainBlocks.last(), 0));
    WeakRandom random(1);
    for (BasicBlock* block : proc)
        block->setFrequency(random.get());
    proc.resetReachability();
    validate(proc);

    DominatorJumpPointers pointers(proc);
    B3::Dominators& dominators = proc.dominators();
    const std::array<BasicBlock*, 5> samples { mainBlocks[199], sideBlocks[150], mainBlocks[77], sideBlocks[3], mainBlocks[0] };
    for (BasicBlock* a : samples) {
        CHECK_EQ(pointers.depth(a), linearDepth(dominators, a));
        CHECK(!pointers.leastFrequentBetween(a, a));
        for (BasicBlock* b : samples)
            CHECK(pointers.nearestCommonDominator(a, b) == linearNearestCommonDominator(dominators, a, b));
        for (unsigned depth : { 0u, pointers.depth(a) / 2, pointers.depth(a) }) {
            BasicBlock* dominator = pointers.dominatorAtDepth(a, depth);
            CHECK(dominator == linearDominatorAtDepth(dominators, a, depth));
            CHECK(pointers.leastFrequentBetween(a, dominator) == linearLeastFrequentBetween(dominators, a, dominator));
        }
    }
}

static void checkPlacementBelowLongChains(unsigned length, bool expectsLongWalk)
{
    constexpr int64_t large = 0x123456789abcll;
    constexpr int64_t operandValue = 5;
    Procedure proc(byFrequencyPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* second = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* selector = arguments[0];
    Value* operand = arguments[1];
    const std::array<B3::Opcode, 3> opcodes { Add, Mul, BitXor };
    std::array<Vector<BasicBlock*>, 3> chains;
    for (unsigned chain = 0; chain < chains.size(); ++chain) {
        for (unsigned position = 0; position < length; ++position)
            chains[chain].append(proc.addBlock(position == 1 ? 0.01 : 1));
        for (unsigned position = 0; position + 1 < length; ++position)
            graph.jump(chains[chain][position], chains[chain][position + 1]);
        BasicBlock* last = chains[chain].last();
        graph.ret(last, graph.make(last, opcodes[chain], operand, graph.int64(last, large)));
    }
    graph.branch(root, graph.make(root, Equal, selector, graph.int64(root, 0)), chains[0][0], second);
    graph.branch(second, graph.make(second, Equal, selector, graph.int64(second, 1)), chains[1][0], chains[2][0]);
    proc.resetReachability();

    moveConstants(proc);
    validate(proc);
    for (const Vector<BasicBlock*>& chain : chains)
        CHECK_EQ(int64ConstantCount(chain[1], large), 1u);
    CHECK_EQ(numberOfValues(proc, [&](Value* value) { return value->isInt64(large); }), 3u);
    CHECK_EQ(!!limitHits(proc, Limit::MaxLinearDominatorSteps), expectsLongWalk);

    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(0), operandValue), operandValue + large);
    CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(1), operandValue), operandValue * large);
    CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(2), operandValue), operandValue ^ large);
}

static void testVirtualTreeMatchesPathWalk()
{
    constexpr unsigned maxLinearSteps = defaultValueOf(Limit::MaxLinearDominatorSteps);
    checkPlacementBelowLongChains(8, false);
    checkPlacementBelowLongChains(maxLinearSteps, false);
    checkPlacementBelowLongChains(maxLinearSteps + 1, true);
    checkPlacementBelowLongChains(100, true);
}

static void testConstantPlacementWithNaNFrequency()
{
    constexpr int64_t large = 0x123456789abcll;
    Procedure proc(byFrequencyPolicy());
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* inFirstRare = graph.make(blocks.firstRare, Add, blocks.operand, graph.int64(blocks.firstRare, large));
    Value* inSecondRare = graph.make(blocks.secondRare, Mul, blocks.operand, graph.int64(blocks.secondRare, large));
    blocks.finish(graph, inFirstRare, inSecondRare, blocks.operand);
    blocks.firstRare->setFrequency(std::numeric_limits<double>::quiet_NaN());

    moveConstants(proc);
    validate(proc);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, nullptr), large + 5);
    CHECK_EQ(invoke<int64_t>(*code, 2, 5, nullptr), large * 5);
    CHECK_EQ(invoke<int64_t>(*code, 3, 5, nullptr), 5);
}

static BranchToRareBlocks buildBranchWithLargeConstants(Procedure& proc, int64_t large, bool hasDouble)
{
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* inFirstRare = graph.make(blocks.firstRare, Add, blocks.operand, graph.int64(blocks.firstRare, large));
    Value* inSecondRare = graph.make(blocks.secondRare, Mul, blocks.operand, graph.int64(blocks.secondRare, large));
    graph.store(blocks.root, graph.make(blocks.root, Add, blocks.operand, graph.int64(blocks.root, large + 1)), blocks.pointer, 0, firstHeap);
    if (hasDouble)
        graph.store(blocks.root, graph.number(blocks.root, 0.1), blocks.pointer, 8, firstHeap);
    blocks.finish(graph, inFirstRare, inSecondRare, blocks.operand);
    return blocks;
}

static void testNoWideConstantsSkipsFirstHoist()
{
    constexpr int64_t large = 0x123456789abcll;
    Procedure integersOnly;
    Procedure withDouble;
    BranchToRareBlocks integerBlocks = buildBranchWithLargeConstants(integersOnly, large, false);
    BranchToRareBlocks doubleBlocks = buildBranchWithLargeConstants(withDouble, large, true);

    moveConstants(integersOnly);
    moveConstants(withDouble);
    validate(integersOnly);
    validate(withDouble);
    for (int64_t constant : { large, large + 1 }) {
        CHECK_EQ(int64ConstantCount(integerBlocks.root, constant), int64ConstantCount(doubleBlocks.root, constant));
        CHECK_EQ(int64ConstantCount(integerBlocks.firstRare, constant), int64ConstantCount(doubleBlocks.firstRare, constant));
        CHECK_EQ(int64ConstantCount(integerBlocks.secondRare, constant), int64ConstantCount(doubleBlocks.secondRare, constant));
    }
}

namespace {

struct LoopWithBranches {
    explicit LoopWithBranches(Graph& graph)
        : graph(graph)
        , root(graph.proc.addBlock())
        , header(graph.proc.addBlock())
        , arm(graph.proc.addBlock())
        , rareTarget(graph.proc.addBlock())
        , latch(graph.proc.addBlock())
        , exit(graph.proc.addBlock())
        , index(graph.proc.addVariable(Int64))
        , sum(graph.proc.addVariable(Int64))
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(graph.proc, root);
        bound = arguments[0];
        pointer = arguments[1];
        operand = arguments[2];
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, header);
    }

    void appendInvariantLoads(unsigned numberOfLoads)
    {
        for (unsigned i = 0; i < numberOfLoads; ++i) {
            MemoryValue* loaded = graph.load(header, Int64, pointer, i * 8, firstHeap);
            loaded->setOrigin(numberedOrigin(i + 1));
            graph.increase(header, sum, loaded);
        }
    }

    bool hoist(unsigned numberOfExtraBlocks = 0, FrequencyClass bypassClass = FrequencyClass::Normal)
    {
        BasicBlock* next = graph.proc.addBlock();
        graph.branch(header, graph.make(header, BitAnd, graph.get(header, index), graph.int64(header, 1)), FrequentedBlock(arm), FrequentedBlock(next, bypassClass));
        BasicBlock* chainEnd = arm;
        for (unsigned i = 0; i < numberOfExtraBlocks; ++i) {
            BasicBlock* extraBlock = graph.proc.addBlock();
            graph.jump(chainEnd, extraBlock);
            chainEnd = extraBlock;
        }
        graph.jump(chainEnd, next);
        graph.branch(next, graph.make(next, Equal, graph.get(next, index), graph.int64(next, 1000)), rarely(rareTarget), FrequentedBlock(latch));
        graph.jump(rareTarget, latch);
        graph.increase(latch, index, 1);
        graph.branch(latch, graph.make(latch, LessThan, graph.get(latch, index), bound), header, exit);
        graph.ret(exit, graph.get(exit, sum));
        graph.proc.resetReachability();
        fixSSA(graph.proc);
        bool result = hoistLoopInvariantValues(graph.proc);
        validate(graph.proc);
        return result;
    }

    Graph& graph;
    BasicBlock* root;
    BasicBlock* header;
    BasicBlock* arm;
    BasicBlock* rareTarget;
    BasicBlock* latch;
    BasicBlock* exit;
    Variable* index;
    Variable* sum;
    Value* bound;
    Value* pointer;
    Value* operand;
};

} // anonymous namespace

static Policy likelyPathsPolicy(bool onlyFromLikelyPaths)
{
    return policyWith([&](Policy& policy) {
        policy.loops.hoistsOnlyFromLikelyPaths = onlyFromLikelyPaths;
    });
}

static void checkSumOfInvariantLoads(Procedure& proc, unsigned numberOfLoads)
{
    int64_t memory[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    CHECK(numberOfLoads <= std::size(memory));
    CHECK_EQ(run(*compileProc(proc), 2, memory), static_cast<int64_t>(numberOfLoads * (numberOfLoads + 1)));
}

static PatchpointValue* appendClobberingPatchpoint(Graph& graph, BasicBlock* block, unsigned numberOfPreservedGPRs)
{
    PatchpointValue* result = graph.emptyPatchpoint(block);
    result->effects.writesLocalState = true;
    const Vector<Reg>& allocatableGPRs = graph.proc.code().regsInPriorityOrder(GP);
    RegisterSet clobberedGPRs;
    for (unsigned i = 0; i + numberOfPreservedGPRs < allocatableGPRs.size(); ++i)
        clobberedGPRs.add(allocatableGPRs[i], IgnoreVectors);
    result->clobberLate(clobberedGPRs);
    return result;
}

static void testHoistLoopInvariantValuesOnlyFromBlocksOnEveryLikelyPath()
{
    struct Case {
        bool onlyFromLikelyPaths;
        FrequencyClass bypassClass;
        bool isHoistedFromArm;
    };
    for (Case testCase : { Case { true, FrequencyClass::Normal, false }, Case { true, FrequencyClass::Unlikely, true }, Case { true, FrequencyClass::Rare, true }, Case { false, FrequencyClass::Normal, true } }) {
        Procedure proc(likelyPathsPolicy(testCase.onlyFromLikelyPaths));
        Graph graph { proc };
        LoopWithBranches loop(graph);
        graph.increase(loop.header, loop.sum, graph.make(loop.header, Mul, loop.operand, loop.operand));
        graph.increase(loop.arm, loop.sum, graph.make(loop.arm, BitOr, loop.operand, loop.bound));
        graph.increase(loop.latch, loop.sum, graph.make(loop.latch, BitXor, loop.operand, loop.bound));

        CHECK(loop.hoist(0, testCase.bypassClass));
        CHECK_EQ(valueCountInLoops(proc, Mul), 0u);
        CHECK_EQ(valueCountInLoops(proc, BitXor), 0u);
        CHECK_EQ(valueCountInLoops(proc, BitOr), testCase.isHoistedFromArm ? 0u : 1u);
        CHECK_EQ(run(*compileProc(proc), 4, nullptr, 9), 4 * 81 + 2 * (9 | 4) + 4 * (9 ^ 4));
    }
}

static void testHoistLoopInvariantValuesLimitsLoadsByPreservedRegisters()
{
    constexpr unsigned numberOfLoads = 10;
    constexpr unsigned maxHoistedLoads = defaultValueOf(Limit::MaxHoistedLoadsPerLoop);
    static_assert(maxHoistedLoads < numberOfLoads);
    struct Case {
        bool onlyFromLikelyPaths;
        bool clobbersRegisters;
        bool isOnLikelyPath;
        unsigned numberOfHoistedLoads;
        unsigned numberOfLimitHits;
    };
    for (Case testCase : { Case { true, true, true, 2, 0 }, Case { true, false, true, maxHoistedLoads, 1 }, Case { true, true, false, maxHoistedLoads, 1 }, Case { false, true, true, numberOfLoads, 0 } }) {
        Procedure proc(likelyPathsPolicy(testCase.onlyFromLikelyPaths));
        Graph graph { proc };
        LoopWithBranches loop(graph);
        loop.appendInvariantLoads(numberOfLoads);
        unsigned numberOfAllocatableGPRs = allocatableRegisters(proc).gprs;
        CHECK(numberOfAllocatableGPRs > maxHoistedLoads);
        unsigned numberOfPreservedGPRs = testCase.clobbersRegisters ? 2u : numberOfAllocatableGPRs;
        PatchpointValue* patchpoint = appendClobberingPatchpoint(graph, testCase.isOnLikelyPath ? loop.latch : loop.rareTarget, numberOfPreservedGPRs);
        CHECK_EQ(registersPreservedBy(proc, patchpoint).gprs, numberOfPreservedGPRs);

        CHECK(loop.hoist());
        CHECK_EQ(valueCountInLoops(proc, Load), numberOfLoads - testCase.numberOfHoistedLoads);
        CHECK_EQ(limitHits(proc, Limit::MaxHoistedLoadsPerLoop), testCase.numberOfLimitHits);
        checkSumOfInvariantLoads(proc, numberOfLoads);
    }
}

static void testHoistLoopInvariantValuesLimitsHoistedLoads()
{
    constexpr unsigned defaultLimit = defaultValueOf(Limit::MaxHoistedLoadsPerLoop);
    struct Case {
        bool onlyFromLikelyPaths;
        unsigned limit;
        unsigned numberOfLoads;
        unsigned numberOfHoistedLoads;
    };
    for (Case testCase : { Case { true, defaultLimit, defaultLimit, defaultLimit }, Case { true, defaultLimit, defaultLimit + 1, defaultLimit }, Case { true, defaultLimit, defaultLimit + 2, defaultLimit }, Case { true, 3, 10, 3 }, Case { false, defaultLimit, defaultLimit + 2, defaultLimit + 2 } }) {
        Policy policy = likelyPathsPolicy(testCase.onlyFromLikelyPaths);
        policy.limits.set(Limit::MaxHoistedLoadsPerLoop, testCase.limit);
        Procedure proc(policy);
        Graph graph { proc };
        LoopWithBranches loop(graph);
        loop.appendInvariantLoads(testCase.numberOfLoads);

        CHECK(loop.hoist());
        bool reachesLimit = testCase.numberOfHoistedLoads < testCase.numberOfLoads;
        CHECK_EQ(valueCountInLoops(proc, Load), testCase.numberOfLoads - testCase.numberOfHoistedLoads);
        CHECK_EQ(limitHits(proc, Limit::MaxHoistedLoadsPerLoop), reachesLimit ? 1u : 0u);
        if (reachesLimit)
            CHECK(hasNumber(proc.report().firstOriginOf(Limit::MaxHoistedLoadsPerLoop), testCase.limit + 1));
        checkSumOfInvariantLoads(proc, testCase.numberOfLoads);
    }
}

static void testHoistLoopInvariantValuesHoistsImmutableLoadPastStores()
{
    for (Mutability mutability : { Mutability::Immutable, Mutability::Mutable }) {
        for (bool onlyFromLikelyPaths : { true, false }) {
            for (bool pastWrites : { true, false }) {
                Procedure proc(policyWith([&](Policy& policy) {
                    policy.loops.hoistsOnlyFromLikelyPaths = onlyFromLikelyPaths;
                    policy.loops.hoistsImmutableLoadsPastWrites = pastWrites;
                }));
                Graph graph { proc };
                LoopWithBranches loop(graph);
                MemoryValue* loaded = graph.load(loop.header, Int64, loop.pointer, 0, firstHeap);
                loaded->setReadsMutability(mutability);
                graph.increase(loop.header, loop.sum, loaded);
                graph.store(loop.latch, loop.operand, loop.pointer, 8, firstHeap);

                loop.hoist();
                CHECK_EQ(valueCountInLoops(proc, Load), mutability == Mutability::Immutable && pastWrites ? 0u : 1u);
                int64_t memory[2] = { 6, 0 };
                CHECK_EQ(run(*compileProc(proc), 4, memory, 3), 24);
                CHECK_EQ(memory[1], 3);
            }
        }
    }
}

static void testMovedLoadCheckLetsWordGetFilled()
{
    constexpr int32_t reported = 42;
    for (bool mayGetFilled : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int32_t*, int64_t>(proc, root);
        Value* given = graph.make(root, Trunc, arguments[1]);
        std::optional<MovedLoad> moved = MovedLoad::describe(graph.load(root, Int32, arguments[0], 0, firstHeap));
        moved->mayGetFilled = mayGetFilled;
        root->append(createMovedLoadCheck(proc, *moved, arguments[0], given, createSharedTask<void(CCallHelpers&, const MovedLoad&, GPRReg, GPRReg)>([](CCallHelpers& jit, const MovedLoad&, GPRReg, GPRReg) {
            jit.move(CCallHelpers::TrustedImm32(reported), GPRInfo::returnValueGPR);
            jit.emitFunctionEpilogue();
            jit.ret();
        })));
        graph.ret(root, graph.int32(root, 0));

        auto code = compileProc(proc);
        auto resultFor = [&](int32_t found, int32_t givenToUsers) {
            return invoke<int32_t>(*code, &found, static_cast<int64_t>(givenToUsers));
        };
        CHECK_EQ(resultFor(5, 5), 0);
        CHECK_EQ(resultFor(5, 0), mayGetFilled ? 0 : reported);
        CHECK_EQ(resultFor(5, 3), reported);
        CHECK_EQ(resultFor(0, 3), reported);
    }
}

static void testHoistFromLargeLoop()
{
    Procedure proc(likelyPathsPolicy(true));
    Graph graph { proc };
    LoopWithBranches loop(graph);
    graph.increase(loop.header, loop.sum, graph.make(loop.header, Mul, loop.operand, loop.operand));
    graph.increase(loop.arm, loop.sum, graph.make(loop.arm, BitOr, loop.operand, loop.bound));
    graph.increase(loop.latch, loop.sum, graph.make(loop.latch, BitXor, loop.operand, loop.bound));

    CHECK(loop.hoist(70));
    CHECK_EQ(valueCountInLoops(proc, Mul), 0u);
    CHECK_EQ(valueCountInLoops(proc, BitXor), 0u);
    CHECK_EQ(valueCountInLoops(proc, BitOr), 1u);
    CHECK_EQ(run(*compileProc(proc), 4, nullptr, 9), 4 * 81 + 2 * (9 | 4) + 4 * (9 ^ 4));
}

static void testHoistWithEarlyLikelyExit()
{
    struct Case {
        bool onlyFromLikelyPaths;
        FrequencyClass exitClass;
        bool isHoistedFromBody;
    };
    for (Case testCase : { Case { true, FrequencyClass::Normal, false }, Case { true, FrequencyClass::Unlikely, true }, Case { true, FrequencyClass::Rare, true }, Case { false, FrequencyClass::Normal, true } }) {
        Procedure proc(likelyPathsPolicy(testCase.onlyFromLikelyPaths));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Int64);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
        Value* bound = arguments[0];
        Value* extra = arguments[2];
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, header);
        Value* limit = graph.make(header, BitOr, bound, extra);
        graph.branch(header, graph.make(header, LessThan, graph.get(header, index), limit), FrequentedBlock(body), FrequentedBlock(exit, testCase.exitClass));
        graph.increase(body, sum, graph.make(body, Mul, extra, extra));
        graph.increase(body, index, 1);
        graph.jump(body, header);
        graph.ret(exit, graph.get(exit, sum));
        proc.resetReachability();
        fixSSA(proc);

        hoistLoopInvariantValues(proc);
        validate(proc);
        CHECK_EQ(valueCountInLoops(proc, BitOr), 0u);
        CHECK_EQ(valueCountInLoops(proc, Mul), testCase.isHoistedFromBody ? 0u : 1u);
        CHECK_EQ(run(*compileProc(proc), 4, nullptr, 1), 5);
    }
}

static int64_t sumAcrossInnerLoop(int64_t bound, int64_t extra)
{
    int64_t sum = 0;
    for (int64_t i = 0; i < bound; ++i) {
        for (int64_t j = 0; j < i; ++j)
            sum += 1 + (extra | bound);
        sum += extra * extra;
    }
    return sum;
}

static void testHoistAcrossInnerLoop()
{
    for (bool onlyFromLikelyPaths : { true, false }) {
        Procedure proc(likelyPathsPolicy(onlyFromLikelyPaths));
        Graph graph { proc };
        TopTestedLoop outer(graph);
        BasicBlock* innerHeader = proc.addBlock();
        BasicBlock* innerBody = proc.addBlock();
        BasicBlock* outerLatch = proc.addBlock();
        Variable* innerIndex = proc.addVariable(Int64);
        graph.set(outer.body, innerIndex, graph.int64(outer.body, 0));
        graph.jump(outer.body, innerHeader);
        graph.branch(innerHeader, graph.make(innerHeader, LessThan, graph.get(innerHeader, innerIndex), graph.get(innerHeader, outer.index)), innerBody, outerLatch);
        graph.increase(innerBody, outer.sum, 1);
        graph.increase(innerBody, outer.sum, graph.make(innerBody, BitOr, outer.extra, outer.bound));
        graph.increase(innerBody, innerIndex, 1);
        graph.jump(innerBody, innerHeader);
        graph.increase(outerLatch, outer.sum, graph.make(outerLatch, Mul, outer.extra, outer.extra));
        outer.endBody(outerLatch);
        outer.rotate();

        hoistLoopInvariantValues(proc);
        validate(proc);
        CHECK_EQ(valueCountInLoops(proc, Mul), 0u);
        checkValueCountInEachLoop(proc, BitOr, { 0, onlyFromLikelyPaths ? 1u : 0u });
        auto code = compileProc(proc);
        for (int64_t bound : { 0, 1, 4 })
            CHECK_EQ(run(*code, bound, nullptr, 3), sumAcrossInnerLoop(bound, 3));
    }
}

static void testHoistWhenNoLikelyPathEndsIteration()
{
    for (bool onlyFromLikelyPaths : { true, false }) {
        Procedure proc(likelyPathsPolicy(onlyFromLikelyPaths));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* outerHeader = proc.addBlock();
        BasicBlock* innerHeader = proc.addBlock();
        BasicBlock* innerBody = proc.addBlock();
        BasicBlock* outerLatch = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Variable* innerIndex = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Int64);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
        Value* bound = arguments[0];
        Value* extra = arguments[2];
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, outerHeader);
        graph.set(outerHeader, innerIndex, graph.int64(outerHeader, 0));
        graph.jump(outerHeader, innerHeader);
        graph.branch(innerHeader, graph.make(innerHeader, LessThan, graph.get(innerHeader, innerIndex), graph.get(innerHeader, index)), FrequentedBlock(innerBody), rarely(outerLatch));
        graph.increase(innerBody, sum, 1);
        graph.increase(innerBody, sum, graph.make(innerBody, BitOr, extra, bound));
        graph.increase(innerBody, innerIndex, 1);
        graph.jump(innerBody, innerHeader);
        graph.increase(outerLatch, sum, graph.make(outerLatch, Mul, extra, extra));
        graph.increase(outerLatch, index, 1);
        graph.branch(outerLatch, graph.make(outerLatch, LessThan, graph.get(outerLatch, index), bound), outerHeader, exit);
        graph.ret(exit, graph.get(exit, sum));
        proc.resetReachability();
        fixSSA(proc);

        hoistLoopInvariantValues(proc);
        validate(proc);
        checkValueCountInEachLoop(proc, BitOr, { 0, 0 });
        checkValueCountInEachLoop(proc, Mul, { 0, onlyFromLikelyPaths ? 1u : 0u });
        auto code = compileProc(proc);
        for (int64_t iterations : { 1, 4 })
            CHECK_EQ(run(*code, iterations, nullptr, 3), sumAcrossInnerLoop(iterations, 3));
    }
}

static void testControlDependentLoadStaysDespiteLikelyPath()
{
    for (bool onlyFromLikelyPaths : { true, false }) {
        Procedure proc(likelyPathsPolicy(onlyFromLikelyPaths));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* bails = proc.addBlock();
        BasicBlock* latch = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Int64);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
        Value* bound = arguments[0];
        Value* pointer = arguments[1];
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, header);
        graph.branch(header, graph.make(header, Equal, pointer, graph.int64(header, 0)), rarely(bails), FrequentedBlock(latch));
        graph.ret(bails, graph.int64(bails, -1));
        graph.increase(latch, sum, graph.load(latch, Int64, pointer, 0, firstHeap));
        graph.increase(latch, index, 1);
        graph.branch(latch, graph.make(latch, LessThan, graph.get(latch, index), bound), header, exit);
        graph.ret(exit, graph.get(exit, sum));
        proc.resetReachability();
        fixSSA(proc);

        hoistLoopInvariantValues(proc);
        validate(proc);
        CHECK_EQ(valueCountInLoops(proc, Load), 1u);
        auto code = compileProc(proc);
        CHECK_EQ(run(*code, 4), -1);
        int64_t memory[1] = { 6 };
        CHECK_EQ(run(*code, 4, memory), 24);
    }
}

// A poll leaves for reasons of its own. Any other value that may leave can be what makes the load safe.
static void testControlDependentLoadIsHoistedAcrossPoll()
{
    for (bool isPoll : { true, false }) {
        Procedure proc(likelyPathsPolicy(true));
        Graph graph { proc };
        PollState state;
        BasicBlock* root = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Int64);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
        Value* bound = arguments[0];
        Value* pointer = arguments[1];
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, body);
        PatchpointValue* poll = appendPoll(graph, body, state);
        poll->effects.exitsSideways = true;
        poll->isLoopPoll = isPoll;
        graph.increase(body, sum, graph.load(body, Int64, pointer, 0, firstHeap));
        graph.increase(body, index, 1);
        graph.branch(body, graph.make(body, LessThan, graph.get(body, index), bound), body, exit);
        graph.ret(exit, graph.get(exit, sum));
        proc.resetReachability();
        fixSSA(proc);

        hoistLoopInvariantValues(proc);
        validate(proc);
        CHECK_EQ(valueCountInLoops(proc, Load), isPoll ? 0u : 1u);
        int64_t memory[1] = { 6 };
        CHECK_EQ(run(*compileProc(proc), 4, memory), 24);
        CHECK_EQ(state.numberOfPolls, 4);
    }
}

void addBunHoistingTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testMoveConstantsMaterializesByFrequency());
    RUN(testMoveConstantsByFrequencyHoistsOutOfLoop());
    RUN(testMoveConstantsSharesNegatedAddend());
    RUN(testDominatorJumpPointersMatchLinearWalk());
    RUN(testVirtualTreeMatchesPathWalk());
    RUN(testConstantPlacementWithNaNFrequency());
    RUN(testNoWideConstantsSkipsFirstHoist());
    RUN(testHoistLoopInvariantValuesOnlyFromBlocksOnEveryLikelyPath());
    RUN(testHoistLoopInvariantValuesLimitsLoadsByPreservedRegisters());
    RUN(testHoistLoopInvariantValuesLimitsHoistedLoads());
    RUN(testHoistLoopInvariantValuesHoistsImmutableLoadPastStores());
    RUN(testMovedLoadCheckLetsWordGetFilled());
    RUN(testHoistFromLargeLoop());
    RUN(testHoistWithEarlyLikelyExit());
    RUN(testHoistAcrossInnerLoop());
    RUN(testHoistWhenNoLikelyPathEndsIteration());
    RUN(testControlDependentLoadStaysDespiteLikelyPath());
    RUN(testControlDependentLoadIsHoistedAcrossPoll());
}

#endif // ENABLE(B3_JIT)
