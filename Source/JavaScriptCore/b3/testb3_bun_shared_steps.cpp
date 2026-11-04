/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

using CountWithField = unsigned (*)(bool field);

static void checkStepFollowsPolicy(CountWithField countWith, unsigned countAfterStep, unsigned countWithoutStep)
{
    CHECK_EQ(countWith(true), countAfterStep);
    CHECK_EQ(countWith(false), countWithoutStep);
}

static unsigned selectCountAfterOptimization(bool specializesSelects)
{
    constexpr int32_t exitValue = 1000;
    unsigned result = 0;
    auto inspect = [&](Procedure& optimized) {
        result = numberOfValues(optimized, Select);
    };
    Policy policy = policyWith([&](Policy& configured) {
        configured.upstream.specializesSelects = specializesSelects;
    });
    Procedure proc(policyWithInspection(policy, inspect));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* condition = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* ifTrue = graph.int64(root, -42);
    Value* ifFalse = graph.int64(root, 35);
    Value* addend = graph.int64(root, 42);
    Value* sum = graph.make(root, Add, graph.select(root, condition, ifTrue, ifFalse), addend);
    CheckValue* check = root->appendNew<CheckValue>(proc, Check, Origin(), sum);
    check->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.move(CCallHelpers::TrustedImm32(exitValue), GPRInfo::returnValueGPR);
        jit.emitFunctionEpilogue();
        jit.ret();
    });
    graph.ret(root, sum);

    auto code = compileProc(proc, 2);
    for (int64_t argument : { 0, 1, -1 })
        CHECK_EQ(invoke<int64_t>(*code, argument), argument ? 0 : exitValue);
    return result;
}

static void testSpecializeSelectFollowsPolicy()
{
    checkStepFollowsPolicy(selectCountAfterOptimization, 0u, 1u);
}

static unsigned branchCountAfterOptimization(bool duplicatesTails)
{
    unsigned result = 0;
    auto inspect = [&](Procedure& optimized) {
        result = numberOfValues(optimized, Branch);
    };
    Policy policy = policyWith([&](Policy& configured) {
        configured.upstream.duplicatesTails = duplicatesTails;
    });
    Procedure proc(policyWithInspection(policy, inspect));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.branch(blocks.tail, blocks.second, blocks.taken, blocks.notTaken);
    blocks.joinPaths(graph);

    auto code = compileProc(proc, 2);
    for (int64_t first : { 0, 3 }) {
        for (int64_t second : { 0, 5 }) {
            int64_t memory = -1;
            CHECK_EQ(invoke<int64_t>(*code, first, second, &memory), second ? 10 : 20);
            CHECK_EQ(memory, first ? first : second);
        }
    }
    return result;
}

static void testDuplicateTailsFollowsPolicy()
{
    checkStepFollowsPolicy(branchCountAfterOptimization, 3u, 2u);
}

static unsigned identityCountBeforeLowering(bool canonicalizesIncrements)
{
    constexpr int64_t increment = 8;
    unsigned result = 0;
    Policy policy = policyWith([&](Policy& configured) {
        configured.upstream.canonicalizesIncrements = canonicalizesIncrements;
    });
    policy.clientPhases.append(ClientPhase { ClientPhase::Point::BeforeLowerToAir, "countIdentities"_s, createSharedTask<bool(Procedure&)>([&](Procedure& lowered) {
        result = numberOfValues(lowered, Identity);
        return false;
    }) });
    Procedure proc(policy);
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* pointer = cCallArgumentValues<int64_t*>(proc, root)[0];
    Value* loaded = graph.load(root, Int64, pointer, 0, firstHeap);
    Value* incremented = graph.make(root, Add, pointer, graph.int64(root, increment));
    graph.ret(root, graph.make(root, BitXor, loaded, incremented));

    auto code = compileProc(proc, 2);
    int64_t memory = 5;
    CHECK_EQ(invoke<int64_t>(*code, &memory), memory ^ (std::bit_cast<int64_t>(&memory) + increment));
    return result;
}

static void testCanonicalizeIncrementsFollowsPolicy()
{
    checkStepFollowsPolicy(identityCountBeforeLowering, isARM64() ? 1u : 0u, 0u);
}

void addBunSharedStepTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testSpecializeSelectFollowsPolicy());
    RUN(testDuplicateTailsFollowsPolicy());
    RUN(testCanonicalizeIncrementsFollowsPolicy());
}

#endif // ENABLE(B3_JIT)
