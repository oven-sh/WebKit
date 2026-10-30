/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3CheckValue.h"
#include <array>

namespace {

struct Counts {
    friend bool operator==(const Counts&, const Counts&) = default;

    void dump(PrintStream& out) const { out.print(byteLoads, ", ", loads, ", ", branches); }

    unsigned byteLoads;
    unsigned loads;
    unsigned branches;
};

struct LoadCounts {
    friend bool operator==(const LoadCounts&, const LoadCounts&) = default;

    void dump(PrintStream& out) const { out.print(first, ", ", second, ", ", third); }

    unsigned first;
    unsigned second;
    unsigned third;
};

} // namespace

static constexpr std::array<HeapRange, 2> firstAndThirdHeaps { firstHeap, thirdHeap };

static void testMemoryNumberingEliminatesRedundantLoad()
{
    for (bool usesPolicy : { true, false }) {
        TwoLoads loads(Mutability::Mutable, usesPolicy ? strengthReductionPolicy() : Policy());
        CHECK_EQ(loads.remainingLoadCount(loads.root), usesPolicy ? 1u : 2u);
        int64_t memory[1] = { 3 };
        CHECK_EQ(loads.run(memory, memory, 0), 303);
    }
}

static void testMemoryNumberingKeepsLoadAfterAliasingStore()
{
    for (int32_t offset : { 0, 8 }) {
        TwoLoads loads;
        loads.graph.store(loads.root, loads.operand, loads.otherPointer, offset, firstHeap);
        CHECK_EQ(loads.remainingLoadCount(loads.root), 2u);
        int64_t memory[2] = { 3, 4 };
        CHECK_EQ(loads.run(memory, memory, 5), offset ? 303 : 503);
    }
}

static void testMemoryNumberingEliminatesLoadAfterDisjointStore()
{
    TwoLoads loads;
    loads.graph.store(loads.root, loads.operand, loads.otherPointer, 0, secondHeap);
    CHECK_EQ(loads.remainingLoadCount(loads.root), 1u);
    int64_t memory[1] = { 3 };
    int64_t otherMemory[1] = { 4 };
    CHECK_EQ(loads.run(memory, otherMemory, 5), 303);
    CHECK_EQ(otherMemory[0], 5);
}

static void testMemoryNumberingForwardsStoreToLoad()
{
    TwoLoads loads;
    loads.graph.store(loads.root, loads.operand, loads.pointer, 0, firstHeap);
    CHECK_EQ(loads.remainingLoadCount(loads.root), 1u);
    int64_t memory[1] = { 3 };
    CHECK_EQ(loads.run(memory, memory, 5), 503);
    CHECK_EQ(memory[0], 5);
}

static void testMemoryNumberingForwardsStoreOfLoadedValue()
{
    TwoLoads loads;
    loads.graph.store(loads.root, loads.first, loads.pointer, 0, firstHeap);
    CHECK_EQ(loads.remainingLoadCount(loads.root), 1u);
    int64_t memory[1] = { 3 };
    CHECK_EQ(loads.run(memory, memory, 5), 303);
    CHECK_EQ(memory[0], 3);
}

static void testMemoryNumberingDoesNotForwardNarrowStore()
{
    TwoLoads loads;
    loads.root->appendNew<MemoryValue>(loads.proc, Store8, Origin(), loads.graph.make(loads.root, Trunc, loads.operand), loads.pointer, 0, firstHeap);
    CHECK_EQ(loads.remainingLoadCount(loads.root), 2u);
    int64_t memory[1] = { 0x300 };
    CHECK_EQ(loads.run(memory, memory, 5), 0x300 + 0x305 * 100);
}

static void testMemoryNumberingHeapRangeWrites()
{
    struct Case {
        HeapRange writes;
        unsigned remainingLoadCount;
    };
    for (Case testCase : { Case { bothHeaps, 2 }, Case { HeapRange(0, 11), 2 }, Case { HeapRange::top(), 2 }, Case { HeapRange(11, 30), 1 }, Case { HeapRange(0, 10), 1 } }) {
        TwoLoads loads;
        loads.graph.emptyPatchpoint(loads.root)->effects.writes = testCase.writes;
        CHECK_EQ(loads.remainingLoadCount(loads.root), testCase.remainingLoadCount);
    }
}

static void testMemoryNumberingKeepsLoadAfterFence()
{
    TwoLoads loads;
    loads.root->appendNew<FenceValue>(loads.proc, Origin());
    CHECK_EQ(loads.remainingLoadCount(loads.root), 2u);
}

static void testMemoryNumberingImmutableLoads()
{
    {
        TwoLoads loads(Mutability::Immutable);
        loads.graph.emptyPatchpoint(loads.root)->effects.writes = HeapRange::top();
        CHECK_EQ(loads.remainingLoadCount(loads.root), 1u);
    }
    {
        TwoLoads loads(Mutability::Immutable);
        loads.graph.callLikePatchpoint(loads.root)->effects = Effects::forCall();
        CHECK_EQ(loads.remainingLoadCount(loads.root), 1u);
    }
}

static void testMutableLoadAfterImmutableLoad()
{
    TwoLoads loads(Mutability::Immutable);
    loads.graph.store(loads.root, loads.operand, loads.otherPointer, 0, firstHeap);
    loads.mutability = Mutability::Mutable;
    CHECK_EQ(loads.remainingLoadCount(loads.root), 2u);
    int64_t memory[1] = { 3 };
    CHECK_EQ(loads.run(memory, memory, 5) / 100, 5);
}

static void testMemoryNumberingAcrossDiamond()
{
    struct Case {
        std::optional<HeapRange> writesOnOnePath;
        unsigned remainingLoadCount;
    };
    for (Case testCase : { Case { std::nullopt, 1 }, Case { secondHeap, 1 }, Case { firstHeap, 2 }, Case { bothHeaps, 2 } }) {
        TwoLoads loads;
        Graph& graph = loads.graph;
        BasicBlock* left = loads.proc.addBlock();
        BasicBlock* right = loads.proc.addBlock();
        BasicBlock* join = loads.proc.addBlock();
        graph.branch(loads.root, loads.operand, left, right);
        if (testCase.writesOnOnePath)
            graph.store(left, loads.operand, loads.otherPointer, 0, *testCase.writesOnOnePath);
        graph.store(left, loads.operand, loads.otherPointer, 8, HeapRange(20, 21));
        graph.jump(left, join);
        graph.store(right, loads.operand, loads.otherPointer, 16, HeapRange(21, 22));
        graph.jump(right, join);
        CHECK_EQ(loads.remainingLoadCount(join), testCase.remainingLoadCount);
        int64_t memory[3] = { 3, 0, 0 };
        bool overwrites = testCase.writesOnOnePath && testCase.writesOnOnePath->overlaps(firstHeap);
        if (!testCase.writesOnOnePath || overwrites)
            CHECK_EQ(loads.run(memory, memory, 5), overwrites ? 503 : 303);
        memory[0] = 3;
        CHECK_EQ(loads.run(memory, memory, 0), 303);
    }
}

static void testMemoryNumberingAcrossLoops()
{
    for (bool loopOverwrites : { false, true }) {
        TwoLoads loads;
        Graph& graph = loads.graph;
        BasicBlock* loop = loads.proc.addBlock();
        BasicBlock* exit = loads.proc.addBlock();
        Value* index = graph.phi(loop, Int64);
        graph.upsilon(loads.root, graph.int64(loads.root, 0), index);
        graph.jump(loads.root, loop);
        Value* next = graph.make(loop, Add, index, loads.load(loop));
        graph.store(loop, next, loads.otherPointer, 0, loopOverwrites ? firstHeap : secondHeap);
        graph.upsilon(loop, next, index);
        graph.branch(loop, graph.make(loop, LessThan, next, loads.operand), loop, exit);
        CHECK_EQ(loads.remainingLoadCount(exit), loopOverwrites ? 3u : 1u);
        int64_t memory[1] = { 3 };
        int64_t otherMemory[1] = { 0 };
        CHECK_EQ(loads.run(memory, otherMemory, 7), 303);
        CHECK_EQ(otherMemory[0], 9);
        if (loopOverwrites)
            CHECK_EQ(loads.run(memory, memory, 7), 3 + 12 * 100);
    }
}

template<typename Functor>
static void checkFencedAccess(unsigned remainingLoadCount, const Functor& appendFencedAccess)
{
    TwoLoads loads;
    appendFencedAccess(loads);
    CHECK_EQ(loads.remainingLoadCount(loads.root), remainingLoadCount);
    int64_t memory[1] = { 3 };
    int64_t otherMemory[1] = { 4 };
    CHECK_EQ(loads.run(memory, otherMemory, 5), 303);
}

static void testMemoryNumberingFencedAccesses()
{
    checkFencedAccess(2, [](TwoLoads& loads) {
        loads.first->as<MemoryValue>()->setFenceRange(HeapRange::top());
    });
    checkFencedAccess(3, [](TwoLoads& loads) {
        loads.graph.load(loads.root, Int64, loads.otherPointer, 0, secondHeap)->setFenceRange(HeapRange::top());
    });
    checkFencedAccess(2, [](TwoLoads& loads) {
        loads.root->appendNew<MemoryValue>(loads.proc, Store, Origin(), loads.operand, loads.otherPointer, 0, secondHeap, HeapRange::top());
    });
}

static void testMemoryNumberingRecordedWriteLimits()
{
    constexpr unsigned defaultLimit = defaultValueOf(Limit::MaxRecordedWritesPerMemoryState);
    for (unsigned limit : { 4u, defaultLimit }) {
        for (unsigned numberOfStores : { limit, limit + 1 }) {
            bool reachesLimit = numberOfStores > limit;
            Policy policy = strengthReductionPolicy();
            policy.limits.set(Limit::MaxRecordedWritesPerMemoryState, limit);
            TwoLoads loads(Mutability::Mutable, policy);
            Graph& graph = loads.graph;
            BasicBlock* left = loads.proc.addBlock();
            BasicBlock* right = loads.proc.addBlock();
            BasicBlock* join = loads.proc.addBlock();
            graph.branch(loads.root, loads.operand, left, right);
            for (unsigned i = 0; i < numberOfStores; ++i) {
                graph.store(left, loads.operand, loads.otherPointer, 8 * i, HeapRange(60 + i, 61 + i));
                left->last()->setOrigin(numberedOrigin(i + 1));
            }
            graph.jump(left, join);
            graph.store(right, loads.operand, loads.otherPointer, 0, HeapRange(60, 61));
            graph.jump(right, join);
            loads.returnBothLoads(join);
            CHECK_EQ(loads.reduceAndCountLoads(), reachesLimit ? 2u : 1u);
            CHECK_EQ(limitHits(loads.proc, Limit::MaxRecordedWritesPerMemoryState), reachesLimit ? 1u : 0u);
            CHECK_EQ(hasNumber(loads.proc.report().firstOriginOf(Limit::MaxRecordedWritesPerMemoryState), limit + 1), reachesLimit);
            int64_t memory[1] = { 3 };
            int64_t otherMemory[defaultLimit + 1] = { };
            CHECK_EQ(loads.run(memory, otherMemory, 5), 303);
            CHECK_EQ(loads.run(memory, otherMemory, 0), 303);
        }
    }
}

static Value* appendHeapLoads(Graph& graph, BasicBlock* block, Value* pointer)
{
    Value* first = graph.load(block, Int64, pointer, 0, firstHeap);
    Value* second = graph.load(block, Int64, pointer, 8, secondHeap);
    Value* third = graph.load(block, Int64, pointer, 16, thirdHeap);
    return graph.make(block, Add, graph.make(block, Add, first, second), third);
}

static LoadCounts countLoadsPerHeap(Procedure& proc)
{
    auto countLoads = [&](HeapRange heap) {
        return numberOfValues(proc, [&](Value* value) {
            return value->opcode() == Load && value->as<MemoryValue>()->range() == heap;
        });
    };
    return { countLoads(firstHeap), countLoads(secondHeap), countLoads(thirdHeap) };
}

static PatchpointValue* appendWriter(Graph& graph, BasicBlock* block, std::span<const HeapRange> writes, Value* pointer, Value* operand)
{
    PatchpointValue* writer = block->appendNew<PatchpointValue>(graph.proc, Int64, Origin());
    writer->effects = Effects::none();
    writer->effects.writes = graph.proc.heapSets().create(writes);
    writer->appendSomeRegister(pointer);
    writer->appendSomeRegister(operand);
    writer->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        GPRReg pointerGPR = params[1].gpr();
        GPRReg operandGPR = params[2].gpr();
        jit.store64(operandGPR, CCallHelpers::Address(pointerGPR, 0));
        jit.store64(operandGPR, CCallHelpers::Address(pointerGPR, 16));
        jit.move(operandGPR, params[0].gpr());
    });
    return writer;
}

static LoadCounts countLoadsAroundWriter(std::span<const HeapRange> writes, std::optional<PureFunction> pureFunction)
{
    Procedure proc(strengthReductionPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t>(proc, root);
    Value* before = appendHeapLoads(graph, root, arguments[0]);
    PatchpointValue* writer = appendWriter(graph, root, writes, arguments[0], arguments[1]);
    writer->pureFunction = pureFunction;
    Value* after = appendHeapLoads(graph, root, arguments[0]);
    graph.ret(root, graph.make(root, Add, graph.make(root, Add, before, after), writer));
    reduceStrengthOnce(proc);
    LoadCounts result = countLoadsPerHeap(proc);
    int64_t memory[3] = { 1, 10, 100 };
    int64_t operand = 1000;
    CHECK_EQ(compileAndRun<int64_t>(proc, memory, operand), 111 + 2010 + 1000);
    return result;
}

static LoadCounts countLoadsAcrossLoop(std::span<const HeapRange> writes)
{
    Procedure proc(strengthReductionPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* loop = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t, int64_t>(proc, root).eager();
    Value* pointer = arguments[0];
    Value* index = graph.phi(loop, Int64);
    Value* sum = graph.phi(loop, Int64);
    graph.upsilon(root, graph.int64(root, 0), index);
    graph.upsilon(root, appendHeapLoads(graph, root, pointer), sum);
    graph.jump(root, loop);
    Value* nextSum = graph.make(loop, Add, sum, appendHeapLoads(graph, loop, pointer));
    appendWriter(graph, loop, writes, pointer, arguments[1]);
    Value* next = graph.make(loop, Add, index, graph.int64(loop, 1));
    graph.upsilon(loop, next, index);
    graph.upsilon(loop, nextSum, sum);
    graph.branch(loop, graph.make(loop, LessThan, next, arguments[2]), loop, exit);
    graph.ret(exit, nextSum);
    reduceStrengthOnce(proc);
    LoadCounts result = countLoadsPerHeap(proc);
    int64_t memory[3] = { 1, 10, 100 };
    int64_t operand = 1000;
    int64_t numberOfIterations = 2;
    CHECK_EQ(compileAndRun<int64_t>(proc, memory, operand, numberOfIterations), 111 + 111 + 2010);
    return result;
}

static void testMemoryNumberingWritesOfSets()
{
    CHECK_EQ(countLoadsAroundWriter(firstAndThirdHeaps, std::nullopt), (LoadCounts { 2, 1, 2 }));
    CHECK_EQ(countLoadsAroundWriter(singleElementSpan(allHeaps), std::nullopt), (LoadCounts { 2, 2, 2 }));
    CHECK_EQ(countLoadsAcrossLoop(firstAndThirdHeaps), (LoadCounts { 2, 1, 2 }));
    CHECK_EQ(countLoadsAcrossLoop(singleElementSpan(allHeaps)), (LoadCounts { 2, 2, 2 }));
}

static unsigned countAddsAroundCall(bool usesPolicy, bool haveSameOperands)
{
    Procedure proc(usesPolicy ? strengthReductionPolicy() : Policy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root);
    Value* before = graph.make(root, Add, arguments[0], arguments[1]);
    graph.callLikePatchpoint(root);
    Value* after = graph.make(root, Add, arguments[0], arguments[haveSameOperands ? 1 : 2]);
    graph.ret(root, graph.make(root, Mul, before, after));
    reduceStrengthOnce(proc);
    unsigned result = numberOfValues(proc, Add);
    CHECK_EQ(compileAndRun<int64_t>(proc, 2, 3, 4), haveSameOperands ? 25 : 30);
    return result;
}

static void testValueNumberingAcrossCalls()
{
    for (bool usesPolicy : { true, false }) {
        CHECK_EQ(countAddsAroundCall(usesPolicy, true), 1u);
        CHECK_EQ(countAddsAroundCall(usesPolicy, false), 2u);
    }
    for (bool writesMemory : { false, true }) {
        TwoLoads loads;
        PatchpointValue* call = loads.graph.callLikePatchpoint(loads.root);
        if (writesMemory)
            call->effects = Effects::forCall();
        CHECK_EQ(loads.remainingLoadCount(loads.root), writesMemory ? 2u : 1u);
    }
}

static unsigned remainingBranchesAfterDominatingBranch(bool usesPolicy, bool rejoins)
{
    Procedure proc(usesPolicy ? strengthReductionPolicy() : Policy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* taken = proc.addBlock();
    BasicBlock* notTaken = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t*>(proc, root);
    Value* operand = arguments[0];
    Value* pointer = arguments[1];
    auto isFive = [&](BasicBlock* block) {
        return graph.make(block, Equal, operand, graph.int64(block, 5));
    };
    auto returnOneOf = [&](BasicBlock* block, int64_t ifFive, int64_t otherwise) {
        BasicBlock* five = proc.addBlock();
        BasicBlock* other = proc.addBlock();
        graph.branch(block, isFive(block), five, other);
        graph.ret(five, graph.int64(five, ifFive));
        graph.ret(other, graph.int64(other, otherwise));
    };
    graph.branch(root, isFive(root), taken, notTaken);
    graph.store(taken, operand, pointer, 0, firstHeap);
    graph.store(notTaken, operand, pointer, 8, firstHeap);
    if (rejoins) {
        graph.jump(taken, join);
        graph.jump(notTaken, join);
        returnOneOf(join, 1, 4);
    } else {
        returnOneOf(taken, 1, 2);
        returnOneOf(notTaken, 3, 4);
        graph.ret(join, operand);
    }
    reduceStrengthOnce(proc);
    unsigned result = numberOfValues(proc, Branch);
    auto code = compileProc(proc);
    int64_t memory[2] = { 0, 0 };
    CHECK_EQ(invoke<int64_t>(*code, 5, memory), 1);
    CHECK_EQ(invoke<int64_t>(*code, 6, memory), 4);
    CHECK_EQ(memory[0], 5);
    CHECK_EQ(memory[1], 6);
    return result;
}

static void testBranchDominatedByBranchOnSameConditionIsFolded()
{
    CHECK_EQ(remainingBranchesAfterDominatingBranch(true, false), 1u);
    CHECK_EQ(remainingBranchesAfterDominatingBranch(false, false), 3u);
    CHECK_EQ(remainingBranchesAfterDominatingBranch(true, true), 2u);
}

static void appendCheck(Graph& graph, BasicBlock* block, Value* condition, int64_t resultOnExit)
{
    CheckValue* check = block->appendNew<CheckValue>(graph.proc, Check, Origin(), condition);
    check->setGenerator([resultOnExit](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.move(CCallHelpers::TrustedImm64(resultOnExit), GPRInfo::returnValueGPR);
        params.code().emitEpilogue(jit);
    });
}

static unsigned remainingChecksAfterDominatingBranch(bool usesPolicy, bool isKnownZero)
{
    Procedure proc(usesPolicy ? strengthReductionPolicy() : Policy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* five = proc.addBlock();
    BasicBlock* other = proc.addBlock();
    Value* operand = cCallArgumentValues<int64_t>(proc, root)[0];
    auto isFive = [&](BasicBlock* block) {
        return graph.make(block, Equal, operand, graph.int64(block, 5));
    };
    graph.branch(root, isFive(root), five, other);
    BasicBlock* checked = isKnownZero ? other : five;
    graph.callLikePatchpoint(checked);
    appendCheck(graph, checked, isFive(checked), 3);
    graph.ret(five, graph.int64(five, 1));
    graph.ret(other, graph.int64(other, 2));
    reduceStrengthOnce(proc);
    unsigned result = numberOfValues(proc, Check);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 5), isKnownZero ? 1 : 3);
    CHECK_EQ(invoke<int64_t>(*code, 6), 2);
    return result;
}

static void testCheckDominatedByBranchOnSameConditionIsRemoved()
{
    CHECK_EQ(remainingChecksAfterDominatingBranch(true, true), 0u);
    CHECK_EQ(remainingChecksAfterDominatingBranch(false, true), 1u);
    CHECK_EQ(remainingChecksAfterDominatingBranch(true, false), 1u);
}

static unsigned remainingBranchesAfterDominatingCheck(bool usesPolicy, int64_t checkedConstant)
{
    Procedure proc(usesPolicy ? strengthReductionPolicy() : Policy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* five = proc.addBlock();
    BasicBlock* other = proc.addBlock();
    Value* operand = cCallArgumentValues<int64_t>(proc, root)[0];
    auto isEqualTo = [&](int64_t constant) {
        return graph.make(root, Equal, operand, graph.int64(root, constant));
    };
    appendCheck(graph, root, isEqualTo(checkedConstant), 3);
    graph.callLikePatchpoint(root);
    graph.branch(root, isEqualTo(5), five, other);
    graph.ret(five, graph.int64(five, 1));
    graph.ret(other, graph.int64(other, 2));
    reduceStrengthOnce(proc);
    unsigned result = numberOfValues(proc, Branch);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 5), checkedConstant == 5 ? 3 : 1);
    CHECK_EQ(invoke<int64_t>(*code, 6), checkedConstant == 6 ? 3 : 2);
    CHECK_EQ(invoke<int64_t>(*code, 7), 2);
    return result;
}

static void testBranchDominatedByCheckOnSameConditionIsFolded()
{
    CHECK_EQ(remainingBranchesAfterDominatingCheck(true, 5), 0u);
    CHECK_EQ(remainingBranchesAfterDominatingCheck(false, 5), 0u);
    CHECK_EQ(remainingBranchesAfterDominatingCheck(true, 6), 1u);
}

static unsigned remainingPatchpointCount(bool usesPolicy, std::optional<uint64_t> firstID, std::optional<uint64_t> secondID)
{
    Procedure proc(usesPolicy ? strengthReductionPolicy() : Policy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    PatchpointValue* first = graph.emptyPatchpoint(root, Int64);
    if (firstID)
        first->pureFunction = PureFunction { *firstID };
    PatchpointValue* second = graph.emptyPatchpoint(root, Int64);
    if (secondID)
        second->pureFunction = PureFunction { *secondID };
    graph.ret(root, graph.make(root, Add, first, second));
    reduceStrengthOnce(proc);
    unsigned result = numberOfValues(proc, Patchpoint);
    CHECK_EQ(compileAndRun<int64_t>(proc), 0);
    return result;
}

static unsigned remainingChildPatchpointCount(std::optional<uint64_t> firstID, std::optional<uint64_t> secondID, unsigned numberOfChildren, bool haveSameChildren)
{
    Procedure proc(strengthReductionPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Vector<Value*> children;
    for (unsigned i = 0; i < numberOfChildren; ++i)
        children.append(graph.make(root, Add, arguments[0], graph.int64(root, i + 1)));
    auto appendPatchpoint = [&](std::optional<uint64_t> id) {
        PatchpointValue* patchpoint = graph.emptyPatchpoint(root, Int64);
        if (id)
            patchpoint->pureFunction = PureFunction { *id, true };
        for (Value* child : children)
            patchpoint->appendSomeRegister(child);
        return patchpoint;
    };
    Value* first = appendPatchpoint(firstID);
    if (!haveSameChildren)
        children.last() = arguments[1];
    Value* second = appendPatchpoint(secondID);
    graph.ret(root, graph.make(root, Add, first, second));
    reduceStrengthOnce(proc);
    return numberOfValues(proc, Patchpoint);
}

static void testPurePatchpointsAreValueNumbered()
{
    for (bool usesPolicy : { true, false }) {
        CHECK_EQ(remainingPatchpointCount(usesPolicy, 7, 7), usesPolicy ? 1u : 2u);
        CHECK_EQ(remainingPatchpointCount(usesPolicy, 7, 8), 2u);
        CHECK_EQ(remainingPatchpointCount(usesPolicy, std::nullopt, std::nullopt), 2u);
    }
    for (unsigned numberOfChildren : { 1u, 5u }) {
        CHECK_EQ(remainingChildPatchpointCount(100, 100, numberOfChildren, true), 1u);
        CHECK_EQ(remainingChildPatchpointCount(100, 100, numberOfChildren, false), 2u);
        CHECK_EQ(remainingChildPatchpointCount(100, 101, numberOfChildren, true), 2u);
        CHECK_EQ(remainingChildPatchpointCount(std::nullopt, std::nullopt, numberOfChildren, true), 2u);
    }
}

static void testPureFunctionWriteClobbersHeap()
{
    CHECK_EQ(countLoadsAroundWriter(firstAndThirdHeaps, PureFunction { 7, true }), (LoadCounts { 2, 1, 2 }));

    Procedure proc(strengthReductionPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto appendPureFunction = [&] {
        PatchpointValue* patchpoint = graph.emptyPatchpoint(root, Int64);
        patchpoint->pureFunction = PureFunction { 7 };
        patchpoint->effects.writes = proc.heapSets().create(firstAndThirdHeaps);
        return patchpoint;
    };
    Value* first = appendPureFunction();
    Value* second = appendPureFunction();
    graph.ret(root, graph.make(root, Add, first, second));
    reduceStrengthOnce(proc);
    CHECK_EQ(numberOfValues(proc, Patchpoint), 1u);
}

static void testStoreOfValueThatStackSlotHoldsIsRemoved()
{
    enum class Between : uint8_t { Nothing, ReaderOfSlot, StoreOfOtherValue, WriterOfSlot, Suspension };
    struct Case {
        Between between;
        bool isStackSlot;
        unsigned remainingStores;
    };
    const Case cases[] = {
        { Between::Nothing, true, 1 },
        { Between::ReaderOfSlot, true, 1 },
        { Between::StoreOfOtherValue, true, 3 },
        { Between::WriterOfSlot, true, 2 },
        { Between::Suspension, true, 2 },
        // Another thread, or the collector, may write what is no slot of the frame.
        { Between::ReaderOfSlot, false, 2 },
    };
    for (const Case& testCase : cases) {
        Procedure proc(strengthReductionPolicy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t*, int64_t>(proc, root);
        Air::StackSlot* slot = proc.addStackSlot(sizeof(int64_t));
        auto address = [&]() -> Value* {
            return testCase.isStackSlot ? root->appendNew<SlotBaseValue>(proc, Origin(), slot) : arguments[0];
        };
        graph.store(root, arguments[1], address(), 0, firstHeap);
        switch (testCase.between) {
        case Between::Nothing:
            break;
        case Between::ReaderOfSlot:
            graph.emptyPatchpoint(root, Void)->effects.reads = firstHeap;
            break;
        case Between::StoreOfOtherValue:
            graph.store(root, graph.int64(root, 7), address(), 0, firstHeap);
            break;
        case Between::WriterOfSlot:
            graph.emptyPatchpoint(root, Void)->effects.writes = firstHeap;
            break;
        case Between::Suspension: {
            PatchpointValue* suspension = graph.emptyPatchpoint(root, Void);
            suspension->effects.writesLocalState = true;
            suspension->setIsSuspension(true);
            break;
        }
        }
        graph.store(root, arguments[1], address(), 0, firstHeap);
        graph.ret(root, arguments[1]);
        reduceStrengthOnce(proc);
        CHECK_EQ(numberOfValues(proc, Store), testCase.remainingStores);
    }
}

static void testReduceStrengthMergesBlockChain()
{
    for (bool usesPolicy : { true, false }) {
        Procedure proc(usesPolicy ? strengthReductionPolicy() : Policy());
        Graph graph { proc };
        constexpr unsigned numberOfBlocksInChain = 40;
        Vector<BasicBlock*> blocks;
        for (unsigned i = 0; i < numberOfBlocksInChain; ++i)
            blocks.append(proc.addBlock());
        Value* pointer = cCallArgumentValues<int64_t*>(proc, blocks[0])[0];
        for (unsigned i = 0; i < numberOfBlocksInChain; ++i) {
            unsigned blockIndex = (i * 7) % numberOfBlocksInChain;
            unsigned nextBlockIndex = ((i + 1) * 7) % numberOfBlocksInChain;
            graph.store(blocks[blockIndex], graph.int64(blocks[blockIndex], i), pointer, i * 8, firstHeap);
            if (i + 1 < numberOfBlocksInChain)
                graph.jump(blocks[blockIndex], blocks[nextBlockIndex]);
            else
                graph.ret(blocks[blockIndex], pointer);
        }
        reduceStrengthOnce(proc);
        CHECK_EQ(numberOfBlocks(proc), 1u);
        CHECK_EQ(numberOfValues(proc, Store), numberOfBlocksInChain);
        int64_t memory[numberOfBlocksInChain] = { };
        compileAndRun<int64_t*>(proc, memory);
        for (unsigned i = 0; i < numberOfBlocksInChain; ++i)
            CHECK_EQ(memory[i], static_cast<int64_t>(i));
    }
}

static void testReduceStrengthEliminatesSingleValuePhi()
{
    for (bool isSingleValue : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* left = proc.addBlock();
        BasicBlock* right = proc.addBlock();
        BasicBlock* join = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(proc, root);
        Value* first = arguments[0];
        Value* second = arguments[1];
        Value* pointer = arguments[2];
        Value* phi = graph.phi(join, Int64);
        graph.branch(root, first, left, right);
        graph.store(left, first, pointer, 0, firstHeap);
        graph.upsilon(left, second, phi);
        graph.jump(left, join);
        graph.store(right, second, pointer, 0, firstHeap);
        graph.upsilon(right, isSingleValue ? second : first, phi);
        graph.jump(right, join);
        graph.ret(join, phi);
        reduceStrengthOnce(proc);
        CHECK_EQ(numberOfValues(proc, Phi), isSingleValue ? 0u : 1u);
        int64_t memory[1];
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 1, 2, memory), 2);
        CHECK_EQ(invoke<int64_t>(*code, 0, 2, memory), isSingleValue ? 2 : 0);
    }
}

static void testMemoryNumberingWhenEdgeIntoJoinIsRemoved()
{
    Procedure proc(strengthReductionPolicy());
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t*, int32_t, int32_t>(proc, root).eager();
    BasicBlock* stores = proc.addBlock();
    BasicBlock* storesElsewhere = proc.addBlock();
    BasicBlock* testsAgain = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    BasicBlock* leaves = proc.addBlock();
    HeapRange heap(10, 11);
    HeapRange otherHeap(11, 12);
    auto load = [&](BasicBlock* block) {
        MemoryValue* value = block->appendNew<MemoryValue>(proc, Load, Int32, Origin(), arguments[0]);
        value->setRange(heap);
        return value;
    };

    Value* before = load(root);
    root->setSuccessors(FrequentedBlock(stores), FrequentedBlock(testsAgain));
    root->appendNew<Value>(proc, Branch, Origin(), arguments[1]);

    stores->appendNew<MemoryValue>(proc, Store, Origin(), stores->appendNew<Const32Value>(proc, Origin(), 5), arguments[0], 0, heap);
    stores->setSuccessors(FrequentedBlock(storesElsewhere), FrequentedBlock(join));
    stores->appendNew<Value>(proc, Branch, Origin(), arguments[2]);

    storesElsewhere->appendNew<MemoryValue>(proc, Store, Origin(), storesElsewhere->appendNew<Const32Value>(proc, Origin(), 1), arguments[0], 4, otherHeap);
    storesElsewhere->setSuccessors(FrequentedBlock(join));
    storesElsewhere->appendNew<Value>(proc, Jump, Origin());

    testsAgain->setSuccessors(FrequentedBlock(join), FrequentedBlock(leaves));
    testsAgain->appendNew<Value>(proc, Branch, Origin(), arguments[1]);

    leaves->clearSuccessors();
    leaves->appendNew<Value>(proc, Return, Origin(), leaves->appendNew<Const32Value>(proc, Origin(), 0));

    Value* after = load(join);
    join->clearSuccessors();
    join->appendNew<Value>(proc, Return, Origin(), join->appendNew<Value>(proc, Add, Origin(), join->appendNew<Value>(proc, Mul, Origin(), before, join->appendNew<Const32Value>(proc, Origin(), 100)), after));

    proc.resetReachability();
    reduceStrength(proc, ReduceStrengthPass::Initial);

    auto code = compileProc(proc);
    for (int32_t alsoStoresElsewhere : { 0, 1 }) {
        int32_t memory[2] = { 1, 2 };
        CHECK_EQ(invoke<int32_t>(*code, memory, 1, alsoStoresElsewhere), 105);
        CHECK_EQ(memory[0], 5);
        CHECK_EQ(memory[1], alsoStoresElsewhere ? 1 : 2);
    }
    int32_t memory[2] = { 1, 2 };
    CHECK_EQ(invoke<int32_t>(*code, memory, 0, 1), 0);
    CHECK_EQ(memory[0], 1);
}

static void testDominatorsAfterLoopHeaderBecomesJump()
{
    Procedure proc(strengthReductionPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* outerHeader = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* latch = proc.addBlock();
    BasicBlock* outerTest = proc.addBlock();
    BasicBlock* outerLatch = proc.addBlock();
    BasicBlock* done = proc.addBlock();

    auto arguments = cCallArgumentValues<int64_t*, int64_t>(proc, root);
    Value* words = arguments[0];
    auto decrement = [&](BasicBlock* block, int32_t offset) {
        Value* result = graph.make(block, Sub, graph.load(block, Int64, words, offset, firstHeap), graph.int64(block, 1));
        graph.store(block, result, words, offset, firstHeap);
        return graph.make(block, GreaterThan, result, graph.int64(block, 0));
    };
    graph.jump(root, outerHeader);
    graph.store(outerHeader, graph.make(outerHeader, Add, graph.load(outerHeader, Int64, words, 0, firstHeap), graph.int64(outerHeader, 1)), words, 0, firstHeap);
    graph.jump(outerHeader, header);
    graph.branch(header, arguments[1], left, right);
    graph.jump(left, join);
    graph.jump(right, join);
    graph.jump(join, body);
    graph.branch(body, decrement(body, 8), latch, outerTest);
    graph.jump(latch, header);
    graph.branch(outerTest, decrement(outerTest, 16), outerLatch, done);
    graph.jump(outerLatch, outerHeader);
    graph.ret(done, graph.load(done, Int64, words, 0, firstHeap));

    proc.resetReachability();
    proc.setOptLevel(2);
    proc.dominators();
    reduceStrength(proc);
    validate(proc);

    auto code = compileProc(proc);
    int64_t memory[3] = { 0, 3, 2 };
    CHECK_EQ(invoke<int64_t>(*code, memory, 1), 2);
    CHECK_EQ(memory[1], -1);
    CHECK_EQ(memory[2], 0);
}

static constexpr HeapRange butterflyHeap { 50, 51 };
static constexpr HeapRange lengthHeap { 51, 52 };
static constexpr HeapRange elementHeap { 52, 53 };
static constexpr HeapRange indexingTypeHeap { 53, 54 };

static Value* appendElementRead(Graph& graph, BasicBlock*& block, Value* array, Value* index, BasicBlock* slowPath)
{
    auto branchToNextBlock = [&](Value* condition) {
        BasicBlock* next = graph.proc.addBlock();
        graph.branch(block, condition, FrequentedBlock(next), rarely(slowPath));
        block = next;
    };
    Value* indexingType = block->appendNew<MemoryValue>(graph.proc, Load8Z, Origin(), array, 4, indexingTypeHeap);
    branchToNextBlock(graph.make(block, Equal, graph.make(block, BitAnd, indexingType, graph.int32(block, 14)), graph.int32(block, 8)));
    Value* butterfly = graph.load(block, Int64, array, 8, butterflyHeap);
    Value* length = graph.make(block, ZExt32, graph.load(block, Int32, butterfly, -8, lengthHeap));
    branchToNextBlock(graph.make(block, Below, index, length));
    Value* offset = graph.make(block, Shl, index, graph.int32(block, 3));
    return graph.load(block, Int64, graph.make(block, Add, butterfly, offset), 0, elementHeap);
}

template<typename Functor>
static Counts countsAfterTwoElementReads(const Functor& between)
{
    Counts counts { };
    auto inspect = [&](Procedure& optimized) {
        counts = { numberOfValues(optimized, Load8Z), numberOfValues(optimized, Load), numberOfValues(optimized, Branch) };
    };
    Procedure proc(policyWithInspection(optimizingPolicy(), inspect));
    Graph graph { proc };
    BasicBlock* block = proc.addBlock();
    BasicBlock* slowPath = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t, int64_t>(proc, block);
    Value* array = arguments[0];
    Value* index = arguments[1];
    Value* operand = arguments[2];
    graph.ret(slowPath, graph.int64(slowPath, -1));

    Value* first = appendElementRead(graph, block, array, index, slowPath);
    between(graph, block, array, operand);
    Value* second = appendElementRead(graph, block, array, index, slowPath);
    graph.ret(block, graph.make(block, Add, first, graph.make(block, Mul, second, graph.int64(block, 100))));
    auto code = compileProc(proc, 2);

    struct {
        int32_t length = 4;
        int32_t padding = 0;
        int64_t elements[4] = { 3, 5, 7, 9 };
    } storage;
    struct {
        uint8_t header[8];
        void* butterfly;
        int64_t more[8];
    } object { };
    object.header[4] = 8;
    object.butterfly = storage.elements;
    CHECK_EQ(invoke<int64_t>(*code, &object, static_cast<int64_t>(2), static_cast<int64_t>(5)), 707);
    CHECK_EQ(invoke<int64_t>(*code, &object, static_cast<int64_t>(4), static_cast<int64_t>(5)), -1);
    return counts;
}

static void testElementReadTestsAreMerged()
{
    auto storeTo = [](HeapRange range) {
        return [range](Graph& graph, BasicBlock* block, Value* array, Value* operand) {
            graph.store(block, operand, array, 16, range);
        };
    };
    auto nothing = [](Graph&, BasicBlock*, Value*, Value*) { };
    auto writesArrayStorage = [](Graph& graph, BasicBlock* block, Value*, Value*) {
        graph.emptyPatchpoint(block)->effects.writes = HeapRange(50, 52);
    };
    auto clobbersRegisters = [](Graph& graph, BasicBlock* block, Value*, Value*) {
        graph.callLikePatchpoint(block);
    };
    auto call = [](Graph& graph, BasicBlock* block, Value*, Value*) {
        graph.callLikePatchpoint(block)->effects = Effects::forCall();
    };
    CHECK_EQ(countsAfterTwoElementReads(nothing), (Counts { 1, 3, 2 }));
    CHECK_EQ(countsAfterTwoElementReads(storeTo(elementHeap)), (Counts { 1, 4, 2 }));
    CHECK_EQ(countsAfterTwoElementReads(storeTo(lengthHeap)), (Counts { 1, 4, 3 }));
    CHECK_EQ(countsAfterTwoElementReads(storeTo(indexingTypeHeap)), (Counts { 2, 3, 3 }));
    CHECK_EQ(countsAfterTwoElementReads(writesArrayStorage), (Counts { 1, 6, 3 }));
    CHECK_EQ(countsAfterTwoElementReads(clobbersRegisters), (Counts { 1, 3, 2 }));
    CHECK_EQ(countsAfterTwoElementReads(call), (Counts { 2, 6, 4 }));
}

void addBunNumberingTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testMemoryNumberingEliminatesRedundantLoad());
    RUN(testMemoryNumberingKeepsLoadAfterAliasingStore());
    RUN(testMemoryNumberingEliminatesLoadAfterDisjointStore());
    RUN(testMemoryNumberingForwardsStoreToLoad());
    RUN(testMemoryNumberingForwardsStoreOfLoadedValue());
    RUN(testMemoryNumberingDoesNotForwardNarrowStore());
    RUN(testMemoryNumberingHeapRangeWrites());
    RUN(testMemoryNumberingKeepsLoadAfterFence());
    RUN(testMemoryNumberingImmutableLoads());
    RUN(testMutableLoadAfterImmutableLoad());
    RUN(testMemoryNumberingAcrossDiamond());
    RUN(testMemoryNumberingAcrossLoops());
    RUN(testMemoryNumberingFencedAccesses());
    RUN(testMemoryNumberingRecordedWriteLimits());
    RUN(testMemoryNumberingWritesOfSets());
    RUN(testValueNumberingAcrossCalls());
    RUN(testBranchDominatedByBranchOnSameConditionIsFolded());
    RUN(testCheckDominatedByBranchOnSameConditionIsRemoved());
    RUN(testBranchDominatedByCheckOnSameConditionIsFolded());
    RUN(testPurePatchpointsAreValueNumbered());
    RUN(testPureFunctionWriteClobbersHeap());
    RUN(testStoreOfValueThatStackSlotHoldsIsRemoved());
    RUN(testReduceStrengthMergesBlockChain());
    RUN(testReduceStrengthEliminatesSingleValuePhi());
    RUN(testMemoryNumberingWhenEdgeIntoJoinIsRemoved());
    RUN(testDominatorsAfterLoopHeaderBecomesJump());
    RUN(testElementReadTestsAreMerged());
}

#endif // ENABLE(B3_JIT)
