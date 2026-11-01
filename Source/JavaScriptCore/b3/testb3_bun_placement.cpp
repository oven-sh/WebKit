/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3BlockRarity.h"
#include "B3CycleForest.h"
#include "B3EliminateDeadCode.h"
#include "B3EstimateFrequencies.h"
#include "B3FixSSA.h"
#include "B3SinkValues.h"

static void testSinkValuesMovesValueIntoRareUser()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* product = graph.make(blocks.root, Mul, blocks.operand, blocks.operand);
    Value* sum = graph.make(blocks.root, Add, product, blocks.selector);
    blocks.finish(graph, sum, blocks.operand, blocks.operand);

    CHECK(sinkValues(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(blocks.root, Mul), 0u);
    CHECK_EQ(numberOfValues(blocks.root, Add), 0u);
    CHECK_EQ(numberOfValues(blocks.firstRare, Mul), 1u);
    CHECK_EQ(numberOfValues(blocks.firstRare, Add), 1u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, nullptr), 26);
    CHECK_EQ(invoke<int64_t>(*code, 2, 5, nullptr), 5);
    CHECK_EQ(invoke<int64_t>(*code, 3, 5, nullptr), 5);
}

static void testSinkValuesClonesValueForTwoRareUsers()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* product = graph.make(blocks.root, Mul, blocks.operand, blocks.operand);
    blocks.finish(graph, product, graph.make(blocks.secondRare, Add, product, product), blocks.operand);

    CHECK(sinkValues(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(blocks.root, Mul), 0u);
    CHECK_EQ(numberOfValues(blocks.next, Mul), 0u);
    CHECK_EQ(numberOfValues(blocks.firstRare, Mul), 1u);
    CHECK_EQ(numberOfValues(blocks.secondRare, Mul), 1u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, nullptr), 25);
    CHECK_EQ(invoke<int64_t>(*code, 2, 5, nullptr), 50);
    CHECK_EQ(invoke<int64_t>(*code, 3, 5, nullptr), 5);
}

static void testSinkValuesClonesOnlyPureFunctionForTwoRareUsers()
{
    enum class Purity : uint8_t { None, OfID, OfIDAndChildren };
    for (Purity purity : { Purity::None, Purity::OfID, Purity::OfIDAndChildren }) {
        Procedure proc;
        Graph graph { proc };
        BranchToRareBlocks blocks(graph);
        PatchpointValue* patchpoint = graph.emptyPatchpoint(blocks.root, Int64);
        if (purity == Purity::OfID)
            patchpoint->pureFunction = PureFunction { 7 };
        if (purity == Purity::OfIDAndChildren) {
            patchpoint->pureFunction = PureFunction { 7, true };
            patchpoint->appendSomeRegister(blocks.operand);
        }
        blocks.finish(graph, patchpoint, graph.make(blocks.secondRare, Add, patchpoint, blocks.operand), blocks.operand);

        CHECK_EQ(sinkValues(proc), purity != Purity::None);
        validate(proc);
        CHECK_EQ(numberOfValues(blocks.root, Patchpoint), purity != Purity::None ? 0u : 1u);
        CHECK_EQ(numberOfValues(blocks.next, Patchpoint), 0u);
        CHECK_EQ(numberOfValues(blocks.firstRare, Patchpoint), purity != Purity::None ? 1u : 0u);
        CHECK_EQ(numberOfValues(blocks.secondRare, Patchpoint), purity != Purity::None ? 1u : 0u);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 1, 5, nullptr), 0);
        CHECK_EQ(invoke<int64_t>(*code, 2, 5, nullptr), 5);
        CHECK_EQ(invoke<int64_t>(*code, 3, 5, nullptr), 5);
    }
}

static void testSinkValuesKeepsValueUsedOnHotPath()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* product = graph.make(blocks.root, Mul, blocks.operand, blocks.operand);
    blocks.finish(graph, product, blocks.operand, product);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(blocks.root, Mul), 1u);
    CHECK_EQ(numberOfValues(proc, Mul), 1u);
}

static void testSinkValuesKeepsValueWithEquallyFrequentUser()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph, 0.75);
    Value* product = graph.make(blocks.root, Mul, blocks.operand, blocks.operand);
    blocks.finish(graph, product, blocks.operand, blocks.operand);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(blocks.root, Mul), 1u);
}

static void testSinkValuesKeepsValueUsedInSameBlock()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* product = graph.make(blocks.root, Mul, blocks.operand, blocks.operand);
    graph.store(blocks.root, product, blocks.pointer, 0, firstHeap);
    blocks.finish(graph, product, blocks.operand, blocks.operand);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(blocks.root, Mul), 1u);
}

static void testSinkValuesClonesComparisonFusedIntoBranchForRareUser()
{
    enum class OtherUser : uint8_t { IsRare, IsAsFrequent, IsInSameBlock };
    for (OtherUser otherUser : { OtherUser::IsRare, OtherUser::IsAsFrequent, OtherUser::IsInSameBlock }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* other = proc.addBlock(otherUser == OtherUser::IsAsFrequent ? 1 : 0.01);
        BasicBlock* hot = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(proc, root);
        Value* pointer = arguments[2];
        Value* isLess = graph.make(root, LessThan, arguments[0], arguments[1]);
        if (otherUser == OtherUser::IsInSameBlock)
            graph.store(root, graph.make(root, ZExt32, isLess), pointer, 0, firstHeap);
        graph.branch(root, isLess, FrequentedBlock(hot), rarely(other));
        graph.ret(other, graph.make(other, Add, graph.make(other, ZExt32, isLess), graph.int64(other, 10)));
        graph.ret(hot, graph.int64(hot, 1));
        proc.resetReachability();
        validate(proc);

        CHECK_EQ(sinkValues(proc), otherUser == OtherUser::IsRare);
        validate(proc);
        CHECK_EQ(numberOfValues(root, LessThan), 1u);
        CHECK_EQ(numberOfValues(other, LessThan), otherUser == OtherUser::IsRare ? 1u : 0u);
        CHECK_EQ(root->last()->child(0), isLess);
        auto code = compileProc(proc);
        int64_t memory[1] = { 7 };
        CHECK_EQ(invoke<int64_t>(*code, 1, 2, memory), 1);
        CHECK_EQ(invoke<int64_t>(*code, 2, 1, memory), 10);
        CHECK_EQ(invoke<int64_t>(*code, 2, 2, memory), 10);
    }
}

static void testSinkValuesClonesAddressTakenIntoAccessesForRareUsers()
{
    enum class OtherUsers : uint8_t { OneIsRare, TwoAreRare, OneIsOnHotPath, OneIsInSameBlock };
    for (OtherUsers otherUsers : { OtherUsers::OneIsRare, OtherUsers::TwoAreRare, OtherUsers::OneIsOnHotPath, OtherUsers::OneIsInSameBlock }) {
        Procedure proc;
        Graph graph { proc };
        BranchToRareBlocks blocks(graph);
        Value* scaled = graph.make(blocks.root, Shl, blocks.operand, graph.int32(blocks.root, 3));
        Value* address = graph.make(blocks.root, Add, blocks.pointer, scaled);
        Value* loaded = graph.load(blocks.root, Int64, address, 0, firstHeap);
        if (otherUsers == OtherUsers::OneIsInSameBlock)
            graph.store(blocks.root, address, blocks.pointer, 0, firstHeap);
        Value* loadedAgain = graph.load(blocks.hot, Int64, address, 0, firstHeap);
        Value* onHotPath = graph.make(blocks.hot, Add, loaded, loadedAgain);
        if (otherUsers == OtherUsers::OneIsOnHotPath)
            onHotPath = graph.make(blocks.hot, Add, onHotPath, address);
        blocks.finish(graph, address, otherUsers == OtherUsers::TwoAreRare ? address : blocks.operand, onHotPath);

        bool clones = otherUsers == OtherUsers::OneIsRare || otherUsers == OtherUsers::TwoAreRare;
        CHECK_EQ(sinkValues(proc), clones);
        validate(proc);
        for (B3::Opcode opcode : { Add, Shl }) {
            CHECK_EQ(numberOfValues(blocks.root, opcode), 1u);
            CHECK_EQ(numberOfValues(blocks.firstRare, opcode), clones ? 1u : 0u);
            CHECK_EQ(numberOfValues(blocks.secondRare, opcode), otherUsers == OtherUsers::TwoAreRare ? 1u : 0u);
        }
        CHECK_EQ(loaded->lastChild(), address);
        CHECK_EQ(loadedAgain->lastChild(), address);
        CHECK_EQ(blocks.firstRare->last()->child(0) == address, !clones);

        auto code = compileProc(proc);
        int64_t memory[2] = { 7, 10 };
        int64_t addressOfSecond = std::bit_cast<int64_t>(&memory[1]);
        CHECK_EQ(invoke<int64_t>(*code, 1, 1, memory), addressOfSecond);
        CHECK_EQ(invoke<int64_t>(*code, 2, 1, memory), otherUsers == OtherUsers::TwoAreRare ? addressOfSecond : 1);
        CHECK_EQ(invoke<int64_t>(*code, 3, 1, memory), 20 + (otherUsers == OtherUsers::OneIsOnHotPath ? addressOfSecond : 0));
    }
}

static void testSinkValuesClonesAddressTakenIntoTrappingAccessWhoseBaseIsRegister()
{
    for (bool baseIsRegister : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BranchToRareBlocks blocks(graph);
        CHECK_EQ(blocks.pointer->opcode(), ArgumentReg);
        Value* base = baseIsRegister ? blocks.pointer : graph.make(blocks.root, Opaque, blocks.pointer);
        Value* address = graph.make(blocks.root, Add, base, graph.make(blocks.root, Shl, blocks.operand, graph.int32(blocks.root, 3)));
        Value* loaded = blocks.root->appendNew<MemoryValue>(proc, trapping(Load), Int64, Origin(), address, 0);
        blocks.finish(graph, address, blocks.operand, loaded);

        CHECK_EQ(sinkValues(proc), baseIsRegister);
        validate(proc);
        CHECK_EQ(numberOfValues(blocks.root, Add), 1u);
        CHECK_EQ(numberOfValues(blocks.firstRare, Add), baseIsRegister ? 1u : 0u);
        CHECK_EQ(loaded->lastChild(), address);

        auto code = compileProc(proc);
        int64_t memory[2] = { 7, 10 };
        CHECK_EQ(invoke<int64_t>(*code, 1, 1, memory), std::bit_cast<int64_t>(&memory[1]));
        CHECK_EQ(invoke<int64_t>(*code, 3, 1, memory), 10);
    }
}

static void testSinkValuesMovesAddressWhoseAccessesAreAllRare()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* scaled = graph.make(blocks.root, Shl, blocks.operand, graph.int32(blocks.root, 3));
    Value* address = graph.make(blocks.root, Add, blocks.pointer, scaled);
    blocks.finish(graph, graph.load(blocks.firstRare, Int64, address, 0, firstHeap), blocks.operand, blocks.operand);

    CHECK(sinkValues(proc));
    validate(proc);
    for (B3::Opcode opcode : { Add, Shl }) {
        CHECK_EQ(numberOfValues(blocks.root, opcode), 0u);
        CHECK_EQ(numberOfValues(blocks.firstRare, opcode), 1u);
    }
    auto code = compileProc(proc);
    int64_t memory[2] = { 7, 10 };
    CHECK_EQ(invoke<int64_t>(*code, 1, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 3, 1, memory), 1);
}

static void testSinkValuesKeepsAddressThatIsAlsoStored()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* address = graph.make(blocks.root, Add, blocks.pointer, graph.make(blocks.root, Shl, blocks.operand, graph.int32(blocks.root, 3)));
    graph.store(blocks.root, address, address, 0, firstHeap);
    blocks.finish(graph, address, blocks.operand, blocks.operand);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(proc, Add), 1u);
    auto code = compileProc(proc);
    int64_t memory[2] = { 7, 10 };
    CHECK_EQ(invoke<int64_t>(*code, 1, 1, memory), std::bit_cast<int64_t>(&memory[1]));
    CHECK_EQ(memory[1], std::bit_cast<int64_t>(&memory[1]));
}

static void testSinkValuesSinksWithinIrreducibleCycle()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* first = proc.addBlock();
    BasicBlock* rare = proc.addBlock(0.001);
    BasicBlock* second = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* left = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root);
    Value* entersAtFirst = arguments[0];
    Value* iterations = arguments[1];
    Value* factor = arguments[2];
    graph.set(root, left, iterations);
    graph.set(root, sum, graph.int64(root, 0));
    graph.branch(root, graph.make(root, Equal, entersAtFirst, graph.int64(root, 1)), first, second);
    Value* product = graph.make(first, Mul, factor, graph.get(first, left));
    graph.branch(first, graph.make(first, BitAnd, graph.get(first, left), graph.int64(first, 3)), FrequentedBlock(second), rarely(rare));
    graph.increase(rare, sum, product);
    graph.jump(rare, second);
    graph.increase(second, left, -1);
    graph.increase(second, sum, 1);
    graph.branch(second, graph.make(second, GreaterThan, graph.get(second, left), graph.int64(second, 0)), first, exit);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
    CHECK_EQ(numberOfValues(first, Mul), 1u);

    CHECK(sinkValues(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(first, Mul), 0u);
    CHECK_EQ(numberOfValues(rare, Mul), 1u);
    auto code = compileProc(proc);
    for (int64_t entersAtFirst : { 0, 1 }) {
        for (int64_t iterations : { 1, 4, 9 }) {
            int64_t expected = 0;
            bool isInFirst = entersAtFirst;
            for (int64_t remaining = iterations;;) {
                if (isInFirst && !(remaining & 3))
                    expected += 5 * remaining;
                --remaining;
                ++expected;
                if (remaining <= 0)
                    break;
                isInFirst = true;
            }
            CHECK_EQ(invoke<int64_t>(*code, entersAtFirst, iterations, 5), expected);
        }
    }
}

static void testSinkValuesKeepsLoads()
{
    for (Mutability mutability : { Mutability::Mutable, Mutability::Immutable }) {
        Procedure proc;
        Graph graph { proc };
        BranchToRareBlocks blocks(graph);
        MemoryValue* loaded = graph.load(blocks.root, Int64, blocks.pointer, 0, firstHeap);
        loaded->setReadsMutability(mutability);
        graph.store(blocks.root, blocks.operand, blocks.pointer, 0, firstHeap);
        blocks.finish(graph, loaded, blocks.operand, blocks.operand);

        CHECK(!sinkValues(proc));
        CHECK_EQ(numberOfValues(blocks.root, Load), 1u);
        int64_t memory[1] = { 11 };
        CHECK_EQ(compileAndRun<int64_t>(proc, 1, 5, memory), 11);
    }
}

static void testSinkValuesReadsStackSlotWhereWaysMeet()
{
    enum class Form : uint8_t { Plain, ReadStandsOneBlockAbove, WriteBehindOneRead, WriteBelowUpsilon, OtherOffset, OtherSlot, SecondUser, NoStackSlot, OneWayBringsNoRead };
    struct Case {
        Form form;
        unsigned remainingLoads;
    };
    const Case cases[] = {
        { Form::Plain, 1 },
        { Form::ReadStandsOneBlockAbove, 1 },
        { Form::WriteBehindOneRead, 2 },
        { Form::WriteBelowUpsilon, 2 },
        { Form::OtherOffset, 2 },
        { Form::OtherSlot, 2 },
        { Form::SecondUser, 2 },
        { Form::NoStackSlot, 2 },
        { Form::OneWayBringsNoRead, 1 },
    };
    for (const Case& testCase : cases) {
        Form form = testCase.form;
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* left = proc.addBlock();
        BasicBlock* belowLeft = proc.addBlock();
        BasicBlock* right = proc.addBlock();
        BasicBlock* join = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(proc, root);
        Air::StackSlot* slot = proc.addStackSlot(2 * sizeof(int64_t));
        Air::StackSlot* otherSlot = proc.addStackSlot(2 * sizeof(int64_t));
        auto address = [&](BasicBlock* block, Air::StackSlot* stackSlot) -> Value* {
            return form == Form::NoStackSlot ? arguments[2] : block->appendNew<SlotBaseValue>(proc, Origin(), stackSlot);
        };
        for (Air::StackSlot* stackSlot : { slot, otherSlot }) {
            graph.store(root, arguments[1], address(root, stackSlot), 0, firstHeap);
            graph.store(root, graph.int64(root, 7), address(root, stackSlot), 8, firstHeap);
        }
        Value* met = graph.phi(join, Int64);
        graph.branch(root, arguments[0], left, right);

        Value* readOnLeft = graph.load(left, Int64, address(left, slot), 0, firstHeap);
        if (form == Form::WriteBehindOneRead)
            graph.store(left, graph.int64(left, 9), address(left, slot), 0, firstHeap);
        graph.upsilon(form == Form::WriteBelowUpsilon ? left : belowLeft, readOnLeft, met);
        if (form == Form::Plain) {
            // Read and Upsilon in one block.
            left->values().removeFirst(readOnLeft);
            belowLeft->values().insert(0, readOnLeft);
            Value* base = readOnLeft->lastChild();
            left->values().removeFirst(base);
            belowLeft->values().insert(0, base);
        }
        if (form == Form::WriteBelowUpsilon)
            graph.store(belowLeft, graph.int64(belowLeft, 9), address(belowLeft, slot), 0, firstHeap);
        graph.jump(left, belowLeft);
        graph.jump(belowLeft, join);

        Value* broughtOnRight = form == Form::OneWayBringsNoRead ? arguments[1]
            : graph.load(right, Int64, address(right, form == Form::OtherSlot ? otherSlot : slot), form == Form::OtherOffset ? 8 : 0, firstHeap);
        graph.upsilon(right, broughtOnRight, met);
        graph.jump(right, join);

        Value* result = met;
        if (form == Form::SecondUser)
            result = graph.make(join, Add, met, graph.make(join, Sub, readOnLeft, readOnLeft));
        graph.ret(join, result);
        if (form == Form::SecondUser) {
            // The read has to stand above both of its users.
            left->values().removeFirst(readOnLeft);
            root->values().insert(root->size() - 1, readOnLeft);
            Value* base = readOnLeft->lastChild();
            left->values().removeFirst(base);
            root->values().insert(root->size() - 2, base);
        }

        proc.resetReachability();
        bool isMoved = testCase.remainingLoads == 1 && form != Form::OneWayBringsNoRead;
        CHECK_EQ(sinkValues(proc), isMoved);
        eliminateDeadCode(proc);
        CHECK_EQ(numberOfValues(proc, Load), testCase.remainingLoads);
        CHECK_EQ(numberOfValues(proc, Phi), isMoved ? 0u : 1u);
        CHECK_EQ(numberOfValues(join, Load), isMoved ? 1u : 0u);
        int64_t memory[2] = { 0, 0 };
        auto code = compileProc(proc);
        for (int64_t goesLeft : { 1, 0 }) {
            int64_t expected = !goesLeft && form == Form::OtherOffset ? 7 : 5;
            CHECK_EQ(invoke<int64_t>(*code, goesLeft, 5, memory), expected);
        }
    }
}

static void testSinkValuesKeepsOpaque()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* opaque = graph.make(blocks.root, Opaque, blocks.operand);
    blocks.finish(graph, opaque, blocks.operand, blocks.operand);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(blocks.root, Opaque), 1u);
}

static void testSinkValuesDoesNotSinkIntoLoop()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* preHeader = proc.addBlock(0.01);
    BasicBlock* loop = proc.addBlock(0.1);
    BasicBlock* exit = proc.addBlock(0.01);
    BasicBlock* hot = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* bound = arguments[0];
    Value* operand = arguments[1];
    Value* product = graph.make(root, Mul, operand, operand);
    Value* index = graph.phi(loop, Int64);
    graph.branch(root, graph.make(root, GreaterThan, bound, graph.int64(root, 0)), rarely(preHeader), FrequentedBlock(hot));
    graph.upsilon(preHeader, graph.int64(preHeader, 0), index);
    graph.jump(preHeader, loop);
    Value* next = graph.make(loop, Add, index, product);
    graph.upsilon(loop, next, index);
    graph.branch(loop, graph.make(loop, LessThan, next, bound), loop, exit);
    graph.ret(exit, next);
    graph.ret(hot, operand);
    proc.resetReachability();
    validate(proc);

    CHECK(sinkValues(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(root, Mul), 0u);
    CHECK_EQ(numberOfValues(preHeader, Mul), 1u);
    CHECK_EQ(numberOfValues(loop, Mul), 0u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 10, 2), 12);
    CHECK_EQ(invoke<int64_t>(*code, 0, 2), 2);
}

static void testSinkValuesDoesNotSinkFromPreHeaderIntoLoop()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* loop = proc.addBlock(0.1);
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* bound = arguments[0];
    Value* product = graph.make(root, Mul, arguments[1], arguments[1]);
    Value* index = graph.phi(loop, Int64);
    graph.upsilon(root, graph.int64(root, 0), index);
    graph.jump(root, loop);
    Value* next = graph.make(loop, Add, index, product);
    graph.upsilon(loop, next, index);
    graph.branch(loop, graph.make(loop, LessThan, next, bound), loop, exit);
    graph.ret(exit, next);
    proc.resetReachability();

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(root, Mul), 1u);
}

static void testSinkValuesMovesInductionStepToFirstUse()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* loop = proc.addBlock(10);
    BasicBlock* latch = proc.addBlock(9);
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t*>(proc, root);
    Value* bound = arguments[0];
    Value* pointer = arguments[1];
    Value* index = graph.phi(loop, Int64);
    Value* sum = graph.phi(loop, Int64);
    graph.upsilon(root, graph.int64(root, 0), index);
    graph.upsilon(root, graph.int64(root, 0), sum);
    graph.jump(root, loop);
    Value* next = graph.make(loop, Add, index, graph.int64(loop, 1));
    Value* address = graph.make(loop, Add, pointer, graph.make(loop, Shl, index, graph.int32(loop, 3)));
    Value* element = graph.load(loop, Int64, address, 0, firstHeap);
    Value* total = graph.make(loop, Add, element, sum);
    Value* comparison = graph.make(loop, LessThan, next, bound);
    graph.branch(loop, comparison, latch, exit);
    graph.upsilon(latch, next, index);
    graph.upsilon(latch, total, sum);
    graph.jump(latch, loop);
    graph.ret(exit, total);
    proc.resetReachability();
    validate(proc);

    CHECK(sinkValues(proc));
    validate(proc);
    CHECK_EQ(next->owner, loop);
    CHECK(loop->values().find(total) < loop->values().find(next));
    CHECK_EQ(loop->values().find(next) + 1, loop->values().find(comparison));
    int64_t elements[] = { 1, 2, 4, 8 };
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 4, elements), 15);
    CHECK_EQ(invoke<int64_t>(*code, 1, elements), 1);
}

// for (link = first; link; link = link->next) sum += link->value;
static void testSinkValuesSinksPointerChasingLoadBelowOtherLoads()
{
    struct Link {
        int64_t value;
        Link* next;
    };
    enum class Between : uint8_t { Addition, Store, Nothing };
    for (Between between : { Between::Addition, Between::Store, Between::Nothing }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* loop = proc.addBlock(10);
        BasicBlock* latch = proc.addBlock(9);
        BasicBlock* exit = proc.addBlock();
        Value* link = graph.phi(loop, Int64);
        Value* sum = graph.phi(loop, Int64);
        graph.upsilon(root, cCallArgumentValues<Link*>(proc, root)[0], link);
        graph.upsilon(root, graph.int64(root, 0), sum);
        graph.jump(root, loop);
        Value* value = graph.load(loop, Int64, link, 0, firstHeap);
        Value* total = between == Between::Nothing ? graph.make(loop, Add, value, sum) : nullptr;
        Value* next = graph.load(loop, Int64, link, 8, secondHeap);
        if (between == Between::Store)
            graph.store(loop, value, link, 0, firstHeap);
        if (!total)
            total = graph.make(loop, Add, value, sum);
        graph.branch(loop, next, latch, exit);
        graph.upsilon(latch, next, link);
        graph.upsilon(latch, total, sum);
        graph.jump(latch, loop);
        graph.ret(exit, total);
        proc.resetReachability();
        validate(proc);

        CHECK_EQ(sinkValues(proc), between == Between::Addition);
        validate(proc);
        CHECK_EQ(loop->values().find(total) < loop->values().find(next), between != Between::Store);
        Link last { 4, nullptr };
        Link middle { 2, &last };
        Link first { 1, &middle };
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, &first), 7);
        CHECK_EQ(invoke<int64_t>(*code, &last), 4);
    }
}

// Whether a cycle with two entries is ordered is asked in the middle of the phase, and the answer reads every value of every block.
static void testSinkValuesMovesInductionStepBeforeAskingAboutIrreducibleCycle()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* first = proc.addBlock();
    BasicBlock* rare = proc.addBlock(0.001);
    BasicBlock* second = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root);
    Value* entersAtFirst = arguments[0];
    Value* iterations = arguments[1];
    Value* factor = arguments[2];
    Value* leftAtFirst = graph.phi(first, Int64);
    Value* sumAtFirst = graph.phi(first, Int64);
    Value* leftAtSecond = graph.phi(second, Int64);
    Value* sumAtSecond = graph.phi(second, Int64);

    graph.emptyPatchpoint(root);
    for (Value* left : { leftAtFirst, leftAtSecond })
        graph.upsilon(root, iterations, left);
    for (Value* sum : { sumAtFirst, sumAtSecond })
        graph.upsilon(root, graph.int64(root, 0), sum);
    graph.branch(root, graph.make(root, Equal, entersAtFirst, graph.int64(root, 1)), first, second);

    Value* product = graph.make(first, Mul, factor, leftAtFirst);
    Value* step = graph.make(first, Add, leftAtFirst, graph.int64(first, 7));
    Value* lowBits = graph.make(first, BitAnd, leftAtFirst, graph.int64(first, 3));
    Value* sumInFirst = graph.make(first, BitXor, sumAtFirst, step);
    graph.upsilon(first, leftAtFirst, leftAtSecond);
    graph.upsilon(first, sumInFirst, sumAtSecond);
    graph.branch(first, lowBits, FrequentedBlock(second), rarely(rare));

    graph.upsilon(rare, leftAtFirst, leftAtSecond);
    graph.upsilon(rare, graph.make(rare, BitXor, sumInFirst, product), sumAtSecond);
    graph.jump(rare, second);

    Value* left = graph.make(second, Sub, leftAtSecond, graph.int64(second, 1));
    Value* sum = graph.make(second, BitXor, sumAtSecond, left);
    graph.upsilon(second, left, leftAtFirst);
    graph.upsilon(second, sum, sumAtFirst);
    graph.branch(second, graph.make(second, GreaterThan, left, graph.int64(second, 0)), first, exit);
    graph.ret(exit, sum);
    proc.resetReachability();
    validate(proc);
    CHECK(first->values().find(step) + 1 < first->values().find(sumInFirst));

    CHECK(sinkValues(proc));
    validate(proc);
    CHECK_EQ(first->values().find(step) + 1, first->values().find(sumInFirst));
    CHECK_EQ(numberOfValues(first, Mul), 0u);
    CHECK_EQ(numberOfValues(rare, Mul), 1u);
    auto code = compileProc(proc);
    for (int64_t entersAtFirst : { 0, 1 }) {
        for (int64_t iterations : { 1, 4, 9 }) {
            int64_t expected = 0;
            bool isInFirst = entersAtFirst;
            for (int64_t remaining = iterations;;) {
                if (isInFirst) {
                    expected ^= remaining + 7;
                    if (!(remaining & 3))
                        expected ^= 5 * remaining;
                }
                isInFirst = true;
                expected ^= --remaining;
                if (remaining <= 0)
                    break;
            }
            CHECK_EQ(invoke<int64_t>(*code, entersAtFirst, iterations, 5), expected);
        }
    }
}

static void testValidateRefusesNullEntryInBlock()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* first = graph.int64(root, 1);
    graph.ret(root, graph.int64(root, 2));
    CHECK(!findViolatedRule(proc));
    root->at(0) = nullptr;
    CHECK(findViolatedRule(proc) == ValidationRule::Upstream);
    root->at(0) = first;
    CHECK(!findViolatedRule(proc));
}

static void testSinkValuesLeavesInductionStepAboveAccessThatTakesItAsAddress()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* loop = proc.addBlock(10);
    BasicBlock* latch = proc.addBlock(9);
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t*>(proc, root);
    Value* first = arguments[0];
    Value* last = arguments[1];
    Value* pointer = graph.phi(loop, Int64);
    Value* sum = graph.phi(loop, Int64);
    graph.upsilon(root, first, pointer);
    graph.upsilon(root, graph.int64(root, 0), sum);
    graph.jump(root, loop);
    Value* next = graph.make(loop, Add, pointer, graph.int64(loop, 8));
    Value* element = graph.load(loop, Int64, next, 0, firstHeap);
    Value* total = graph.make(loop, Add, element, sum);
    Value* comparison = graph.make(loop, Below, next, last);
    graph.branch(loop, comparison, latch, exit);
    graph.upsilon(latch, next, pointer);
    graph.upsilon(latch, total, sum);
    graph.jump(latch, loop);
    graph.ret(exit, total);
    proc.resetReachability();
    validate(proc);

    sinkValues(proc);
    validate(proc);
    CHECK(loop->values().find(next) < loop->values().find(element));
    int64_t elements[] = { 1, 2, 4, 8 };
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, elements, elements + 3), 14);
    CHECK_EQ(invoke<int64_t>(*code, elements, elements + 1), 2);
}

static void testSinkValuesLeavesAtMostEightLoops()
{
    for (unsigned numberOfLoops : { 8u, 9u }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* preHeader = proc.addBlock(0.01);
        Vector<BasicBlock*> loops;
        Vector<Value*> indices;
        for (unsigned k = 0; k < numberOfLoops; ++k) {
            loops.append(proc.addBlock(0.1));
            indices.append(graph.phi(loops[k], Int64));
        }
        BasicBlock* exit = proc.addBlock(0.01);
        BasicBlock* hot = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
        Value* bound = arguments[0];
        Value* operand = arguments[1];
        Value* product = root->appendNew<Value>(proc, Mul, numberedOrigin(1), operand, operand);
        graph.branch(root, graph.make(root, GreaterThan, bound, graph.int64(root, 0)), rarely(preHeader), FrequentedBlock(hot));
        graph.upsilon(preHeader, graph.int64(preHeader, 0), indices[0]);
        graph.jump(preHeader, loops[0]);
        Value* next = nullptr;
        for (unsigned k = 0; k < numberOfLoops; ++k) {
            BasicBlock* loop = loops[k];
            bool isLast = k == numberOfLoops - 1;
            next = graph.make(loop, Add, indices[k], isLast ? product : graph.int64(loop, 1));
            graph.upsilon(loop, next, indices[k]);
            if (!isLast)
                graph.upsilon(loop, graph.int64(loop, 0), indices[k + 1]);
            graph.branch(loop, graph.make(loop, LessThan, next, bound), loop, isLast ? exit : loops[k + 1]);
        }
        graph.ret(exit, next);
        graph.ret(hot, operand);
        proc.resetReachability();
        validate(proc);

        bool canLeaveLoops = numberOfLoops == 8;
        CHECK_EQ(sinkValues(proc), canLeaveLoops);
        validate(proc);
        CHECK_EQ(numberOfValues(root, Mul), canLeaveLoops ? 0u : 1u);
        CHECK_EQ(numberOfValues(preHeader, Mul), canLeaveLoops ? 1u : 0u);
        CHECK_EQ(limitHits(proc, Limit::MaxLoopsLeftBySinking) > 0, !canLeaveLoops);
        CHECK_EQ(hasNumber(proc.report().firstOriginOf(Limit::MaxLoopsLeftBySinking), 1), !canLeaveLoops);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 3, 2), 4);
        CHECK_EQ(invoke<int64_t>(*code, 0, 2), 2);
    }
}

struct CountedIrreducibleCycle {
    CountedIrreducibleCycle(Graph& graph, BasicBlock* from)
        : from(from)
        , first(graph.proc.addBlock(0.1))
        , second(graph.proc.addBlock(0.1))
        , exit(graph.proc.addBlock())
    {
    }

    void finish(Graph& graph, Value* entersAtFirst, Value* iterations, Value* stepInFirst)
    {
        Value* leftInFirst = graph.phi(first, Int64);
        Value* leftInSecond = graph.phi(second, Int64);
        graph.upsilon(from, iterations, leftInFirst);
        graph.upsilon(from, iterations, leftInSecond);
        graph.branch(from, entersAtFirst, first, second);
        graph.upsilon(first, graph.make(first, Sub, leftInFirst, stepInFirst), leftInSecond);
        graph.jump(first, second);
        left = graph.make(second, Sub, leftInSecond, graph.int64(second, 1));
        graph.upsilon(second, left, leftInFirst);
        graph.branch(second, graph.make(second, GreaterThan, left, graph.int64(second, 0)), first, exit);
    }

    BasicBlock* from;
    BasicBlock* first;
    BasicBlock* second;
    BasicBlock* exit;
    Value* left { nullptr };
};

static void testSinkValuesSinksAfterAndBetweenIrreducibleCycles()
{
    for (bool hasCycleAfterRareBlock : { false, true }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root);
        Value* selector = arguments[0];
        Value* operand = arguments[1];
        Value* iterations = arguments[2];
        Value* product = graph.make(root, Mul, operand, operand);
        Value* entersAtFirst = graph.make(root, Equal, selector, graph.int64(root, 1));
        CountedIrreducibleCycle cycle(graph, root);
        cycle.finish(graph, entersAtFirst, iterations, graph.int64(cycle.first, 1));
        BasicBlock* rare = proc.addBlock(0.01);
        BasicBlock* hot = proc.addBlock();
        graph.branch(cycle.exit, graph.make(cycle.exit, Equal, selector, graph.int64(cycle.exit, 2)), rarely(rare), FrequentedBlock(hot));
        Value* sum = graph.make(rare, Add, product, selector);
        if (hasCycleAfterRareBlock) {
            CountedIrreducibleCycle next(graph, rare);
            next.finish(graph, entersAtFirst, iterations, graph.int64(next.first, 1));
            graph.ret(next.exit, sum);
        } else
            graph.ret(rare, sum);
        graph.ret(hot, cycle.left);
        proc.resetReachability();
        validate(proc);

        CHECK(sinkValues(proc));
        validate(proc);
        CHECK_EQ(numberOfValues(root, Mul), 0u);
        CHECK_EQ(numberOfValues(rare, Mul), 1u);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 2, 5, 7), 27);
        CHECK_EQ(invoke<int64_t>(*code, 1, 5, 7), -1);
        CHECK_EQ(invoke<int64_t>(*code, 3, 5, 7), 0);
    }
}

static void testSinkValuesDoesNotSinkIntoIrreducibleCycle()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root);
    Value* product = graph.make(root, Mul, arguments[1], arguments[1]);
    CountedIrreducibleCycle cycle(graph, root);
    cycle.finish(graph, graph.make(root, Equal, arguments[0], graph.int64(root, 1)), arguments[2], product);
    graph.ret(cycle.exit, cycle.left);
    proc.resetReachability();
    validate(proc);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(root, Mul), 1u);
    CHECK_EQ(numberOfValues(proc, Mul), 1u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 1, 1, 7), -1);
    CHECK_EQ(invoke<int64_t>(*code, 3, 1, 7), 0);
}

static void testCycleForestNestedLoops()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* outerHeader = proc.addBlock();
    BasicBlock* innerHeader = proc.addBlock();
    BasicBlock* innerBody = proc.addBlock();
    BasicBlock* outerLatch = proc.addBlock();
    BasicBlock* selfLoop = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* condition = cCallArgumentValues<int32_t>(proc, root)[0];
    graph.jump(root, outerHeader);
    graph.branch(outerHeader, condition, innerHeader, selfLoop);
    graph.branch(innerHeader, condition, innerBody, outerLatch);
    graph.jump(innerBody, innerHeader);
    graph.jump(outerLatch, outerHeader);
    graph.branch(selfLoop, condition, selfLoop, exit);
    graph.ret(exit, condition);
    proc.resetReachability();

    CycleForest forest(proc);
    CHECK_EQ(forest.size(), 3u);
    unsigned outer = forest.innermost(outerHeader);
    unsigned inner = forest.innermost(innerHeader);
    unsigned self = forest.innermost(selfLoop);
    CHECK(outer != CycleForest::none && inner != CycleForest::none && self != CycleForest::none);
    CHECK(outer != inner && outer != self && inner != self);
    CHECK_EQ(forest.innermost(root), CycleForest::none);
    CHECK_EQ(forest.innermost(exit), CycleForest::none);
    CHECK_EQ(forest.innermost(innerBody), inner);
    CHECK_EQ(forest.innermost(outerLatch), outer);
    CHECK(forest[outer].header() == outerHeader && forest[inner].header() == innerHeader && forest[self].header() == selfLoop);
    CHECK(forest[outer].isReducible() && forest[inner].isReducible() && forest[self].isReducible());
    CHECK_EQ(forest[outer].parent, CycleForest::none);
    CHECK_EQ(forest[inner].parent, outer);
    CHECK_EQ(forest[self].parent, CycleForest::none);
    CHECK_EQ(forest.depth(outer), 1u);
    CHECK_EQ(forest.depth(inner), 2u);
    CHECK_EQ(forest.depth(CycleForest::none), 0u);
    CHECK(forest.isHeader(outerHeader) && forest.isHeader(innerHeader) && forest.isHeader(selfLoop));
    CHECK(!forest.isHeader(root) && !forest.isHeader(innerBody) && !forest.isHeader(outerLatch));
    CHECK(forest.contains(outer, innerBody) && forest.contains(outer, outerLatch) && forest.contains(inner, innerBody));
    CHECK(!forest.contains(inner, outerLatch) && !forest.contains(outer, selfLoop) && !forest.contains(outer, root));
    CHECK_EQ(forest.nearestCommon(inner, outer), outer);
    CHECK_EQ(forest.nearestCommon(inner, self), CycleForest::none);

    auto& postOrder = forest.nodesInPostOrder();
    CHECK_EQ(postOrder.size(), 7u);
    CHECK(postOrder.last() == root);
    for (BasicBlock* block : proc)
        CHECK(postOrder.contains(block));
    CHECK(postOrder.find(exit) < postOrder.find(selfLoop));
    CHECK(postOrder.find(selfLoop) < postOrder.find(outerHeader));
    CHECK(postOrder.find(innerBody) < postOrder.find(innerHeader));
    CHECK(postOrder.find(outerLatch) < postOrder.find(innerHeader));
    CHECK(postOrder.find(innerHeader) < postOrder.find(outerHeader));
}

static void testCycleForestAcyclic()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Value* condition = cCallArgumentValues<int32_t>(proc, root)[0];
    graph.branch(root, condition, left, right);
    graph.jump(left, join);
    graph.jump(right, join);
    graph.ret(join, condition);
    proc.resetReachability();

    CycleForest forest(proc);
    CHECK_EQ(forest.size(), 0u);
    for (BasicBlock* block : proc) {
        CHECK_EQ(forest.innermost(block), CycleForest::none);
        CHECK(!forest.isHeader(block));
    }
    CHECK_EQ(forest.nodesInPostOrder().size(), 4u);
    CHECK(forest.nodesInPostOrder().first() == join);
    CHECK(forest.nodesInPostOrder().last() == root);
}

struct IrreducibleCycle {
    IrreducibleCycle(Graph& graph, FrequencyClass firstEntryClass, FrequencyClass secondEntryClass = FrequencyClass::Normal)
        : root(graph.proc.addBlock())
        , first(graph.proc.addBlock())
        , second(graph.proc.addBlock())
        , exit(graph.proc.addBlock())
    {
        condition = cCallArgumentValues<int32_t>(graph.proc, root)[0];
        graph.branch(root, condition, FrequentedBlock(first, firstEntryClass), FrequentedBlock(second, secondEntryClass));
    }

    void finish(Graph& graph)
    {
        graph.jump(first, second);
        graph.branch(second, condition, first, exit);
        graph.ret(exit, condition);
        graph.proc.resetReachability();
    }

    BasicBlock* root;
    BasicBlock* first;
    BasicBlock* second;
    BasicBlock* exit;
    Value* condition;
};

static void testCycleForestIrreducibleCycle()
{
    for (FrequencyClass firstEntryClass : { FrequencyClass::Normal, FrequencyClass::Rare }) {
        Procedure proc;
        Graph graph { proc };
        IrreducibleCycle blocks(graph, firstEntryClass);
        blocks.finish(graph);
        CHECK(!proc.naturalLoops().numLoops());

        CycleForest forest(proc);
        CHECK_EQ(forest.size(), 1u);
        CHECK(!forest[0].isReducible());
        CHECK_EQ(forest[0].entries.size(), 2u);
        CHECK(forest[0].entries.contains(blocks.first) && forest[0].entries.contains(blocks.second));
        CHECK(forest[0].header() == (firstEntryClass == FrequencyClass::Normal ? blocks.first : blocks.second));
        CHECK_EQ(forest.innermost(blocks.first), 0u);
        CHECK_EQ(forest.innermost(blocks.second), 0u);
        CHECK_EQ(forest.innermost(blocks.root), CycleForest::none);
        CHECK_EQ(forest.nodesInPostOrder().size(), 4u);
        CHECK(forest.nodesInPostOrder().last() == blocks.root);
    }
}

static void testCycleForestPrefersPollingEntryAsHeader()
{
    for (bool isLoopPoll : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        PollState state;
        IrreducibleCycle blocks(graph, FrequencyClass::Normal);
        appendPoll(graph, blocks.second, state)->isLoopPoll = isLoopPoll;
        blocks.finish(graph);

        CycleForest forest(proc);
        CHECK_EQ(forest.size(), 1u);
        CHECK(forest[0].header() == (isLoopPoll ? blocks.second : blocks.first));
        CHECK_EQ(forest[0].entries.size(), 2u);
    }
}

static void testCycleForestPrefersLikelyEntryAsHeader()
{
    for (FrequencyClass firstEntryClass : { FrequencyClass::Normal, FrequencyClass::Unlikely, FrequencyClass::Rare }) {
        for (FrequencyClass secondEntryClass : { FrequencyClass::Normal, FrequencyClass::Unlikely, FrequencyClass::Rare }) {
            Procedure proc;
            Graph graph { proc };
            IrreducibleCycle blocks(graph, firstEntryClass, secondEntryClass);
            blocks.finish(graph);
            blocks.root->setFrequency(0);

            CycleForest forest(proc);
            CHECK_EQ(forest.size(), 1u);
            bool onlySecondIsLikely = firstEntryClass == FrequencyClass::Rare && secondEntryClass != FrequencyClass::Rare;
            CHECK(forest[0].header() == (onlySecondIsLikely ? blocks.second : blocks.first));
        }
    }
}

static void testCycleForestPrefersLikelyEntryToBlockThatPollsRarely()
{
    Procedure proc(policyWith([](Policy& policy) {
        policy.optimization.estimatesFrequencies = true;
    }));
    Graph graph { proc };
    PollState state;
    BasicBlock* root = proc.addBlock();
    BasicBlock* rareCopy = proc.addBlock();
    BasicBlock* loop = proc.addBlock();
    BasicBlock* latch = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root).eager();
    graph.branch(root, arguments[0], FrequentedBlock(loop), rarely(rareCopy));
    appendPoll(graph, rareCopy, state);
    graph.jump(rareCopy, loop);
    graph.branch(loop, arguments[1], FrequentedBlock(latch), rarely(rareCopy));
    graph.branch(latch, arguments[0], loop, exit);
    graph.ret(exit, arguments[0]);
    proc.resetReachability();

    CycleForest forest(proc);
    CHECK(forest[forest.innermost(rareCopy)].header() == loop);
    estimateFrequencies(proc);
    CHECK(loop->frequency() > 1);
    CHECK(rareCopy->frequency() < 0.1);
}

static void testCycleForestEntryBehindRareBlockIsNotLikely()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* rare = proc.addBlock();
    BasicBlock* enteredRarely = proc.addBlock();
    BasicBlock* enteredOften = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* condition = cCallArgumentValues<int64_t>(proc, root)[0];
    graph.branch(root, condition, FrequentedBlock(enteredOften), rarely(rare));
    graph.jump(rare, enteredRarely);
    graph.jump(enteredRarely, enteredOften);
    graph.branch(enteredOften, condition, rarely(enteredRarely), FrequentedBlock(exit));
    graph.ret(exit, condition);
    proc.resetReachability();

    CycleForest forest(proc);
    CHECK_EQ(forest.size(), 1u);
    CHECK_EQ(forest[0].entries.size(), 2u);
    CHECK(forest[0].header() == enteredOften);
}

static bool isNear(double frequency, double expected) { return std::abs(frequency - expected) < expected / 1000; }
static void appendOops(Graph& graph, BasicBlock* block) { block->appendNew<Value>(graph.proc, Oops, Origin()); }

static void testEstimateFrequenciesTreatsReturningPathAsUnlikely()
{
    for (bool returnsAreUnlikely : { true, false }) {
        Procedure proc(policyWith([&](Policy& policy) {
            policy.optimization.returnsAreUnlikely = returnsAreUnlikely;
        }));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* earlyReturn = proc.addBlock();
        BasicBlock* continuation = proc.addBlock();
        BasicBlock* coldReturn = proc.addBlock();
        BasicBlock* nextContinuation = proc.addBlock();
        BasicBlock* thenReturn = proc.addBlock();
        BasicBlock* elseReturn = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root).eager();
        graph.branch(root, arguments[0], FrequentedBlock(earlyReturn), FrequentedBlock(continuation));
        graph.ret(earlyReturn, graph.int64(earlyReturn, 1));
        graph.branch(continuation, arguments[1], rarely(coldReturn), FrequentedBlock(nextContinuation));
        graph.ret(coldReturn, graph.int64(coldReturn, 2));
        graph.branch(nextContinuation, arguments[2], FrequentedBlock(thenReturn), FrequentedBlock(elseReturn));
        graph.ret(thenReturn, graph.int64(thenReturn, 3));
        graph.ret(elseReturn, graph.int64(elseReturn, 4));

        proc.resetReachability();
        estimateFrequencies(proc);
        double continuingShare = returnsAreUnlikely ? 0.8 : 0.5;
        CHECK(isNear(root->frequency(), 1));
        CHECK(isNear(earlyReturn->frequency(), 1 - continuingShare));
        CHECK(isNear(continuation->frequency(), continuingShare));
        CHECK(isNear(coldReturn->frequency(), continuingShare / 1024));
        CHECK(isNear(nextContinuation->frequency(), continuingShare * 1023 / 1024));
        CHECK(isNear(thenReturn->frequency(), nextContinuation->frequency() / 2));
        CHECK(isNear(elseReturn->frequency(), nextContinuation->frequency() / 2));
    }
}

static void testEstimateFrequenciesPropagatesWeightsBackward()
{
    for (bool slowPathEndsInOops : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* earlyReturn = proc.addBlock();
        BasicBlock* secondBranch = proc.addBlock();
        BasicBlock* thenCase = proc.addBlock();
        BasicBlock* elseCase = proc.addBlock();
        BasicBlock* slowPath = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root).eager();
        graph.branch(root, arguments[0], earlyReturn, secondBranch);
        graph.ret(earlyReturn, graph.int64(earlyReturn, 1));
        graph.branch(secondBranch, arguments[1], thenCase, elseCase);
        graph.jump(thenCase, slowPath);
        graph.jump(elseCase, slowPath);
        if (slowPathEndsInOops)
            appendOops(graph, slowPath);
        else
            graph.ret(slowPath, graph.int64(slowPath, 2));

        resetAndValidate(proc);
        estimateFrequencies(proc);
        double share = slowPathEndsInOops ? 1.0 / 1024 : 0.5;
        CHECK(isNear(root->frequency(), 1));
        CHECK(isNear(earlyReturn->frequency(), 1 - share));
        CHECK(isNear(secondBranch->frequency(), share));
        CHECK(isNear(slowPath->frequency(), share));
        CHECK_EQ(root->successor(1).isRare(), slowPathEndsInOops);
        CHECK(root->successor(0).isLikely());
        CHECK(secondBranch->successor(0).isLikely() && secondBranch->successor(1).isLikely());
    }
}

struct LoopWithSlowPaths {
    explicit LoopWithSlowPaths(Graph& graph, double storedFrequency = 1)
        : graph(graph)
        , storedFrequency(storedFrequency)
    {
        Value* condition = cCallArgumentValues<int64_t>(graph.proc, root)[0];
        graph.jump(root, header);
        graph.branch(header, condition, trapEntry, body);
        graph.jump(trapEntry, trap);
        appendOops(graph, trap);
        graph.branch(body, condition, slowPathEntry, fastPath);
        graph.jump(slowPathEntry, slowPath);
        slowPathEntry->successor(0).frequency() = FrequencyClass::Rare;
        graph.jump(slowPath, latch);
        graph.branch(fastPath, condition, unlikely(unlikelyPath), FrequentedBlock(latch));
        graph.jump(unlikelyPath, latch);
        graph.branch(latch, condition, header, done);
        graph.ret(done, condition);
        resetAndValidate(graph.proc);
    }

    BasicBlock* addBlock() { return graph.proc.addBlock(storedFrequency); }

    Graph& graph;
    double storedFrequency;
    BasicBlock* root { addBlock() };
    BasicBlock* header { addBlock() };
    BasicBlock* trapEntry { addBlock() };
    BasicBlock* trap { addBlock() };
    BasicBlock* body { addBlock() };
    BasicBlock* slowPathEntry { addBlock() };
    BasicBlock* slowPath { addBlock() };
    BasicBlock* fastPath { addBlock() };
    BasicBlock* unlikelyPath { addBlock() };
    BasicBlock* latch { addBlock() };
    BasicBlock* done { addBlock() };
};

static void testEstimateFrequenciesMarksRareEdgesInLoops()
{
    Procedure proc;
    Graph graph { proc };
    LoopWithSlowPaths loop(graph);

    estimateFrequencies(proc);
    CHECK(loop.header->successor(0).isRare() && loop.header->successor(1).isLikely());
    CHECK(loop.trapEntry->successor(0).isLikely());
    CHECK(loop.body->successor(0).isRare() && loop.body->successor(1).isLikely());
    CHECK(loop.fastPath->successor(0).frequency() == FrequencyClass::Unlikely && loop.fastPath->successor(1).isLikely());
    CHECK(loop.latch->successor(0).isLikely() && loop.latch->successor(1).isLikely());
    CHECK(loop.trapEntry->frequency() < loop.body->frequency() / 100);
    CHECK(loop.slowPathEntry->frequency() < loop.fastPath->frequency() / 100);
}

static void testEstimateFrequenciesKeepsWeightsInLoops()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* earlyLatch = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* done = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root).eager();
    graph.jump(root, header);
    graph.branch(header, arguments[0], earlyLatch, body);
    graph.jump(earlyLatch, header);
    graph.branch(body, arguments[1], header, done);
    graph.ret(done, graph.int64(done, 1));

    resetAndValidate(proc);
    estimateFrequencies(proc);
    CHECK(header->successor(0).isLikely() && header->successor(1).isLikely());
    CHECK(isNear(header->frequency(), 10));
    CHECK(isNear(earlyLatch->frequency(), header->frequency() / 2));
    CHECK(earlyLatch->frequency() == body->frequency());
}

static void testEstimateFrequenciesFollowsBooleanPhi()
{
    enum class ColdArm : uint8_t { Then, Else, Neither };
    enum class Negation : uint8_t { None, ByBitXor, ByEqual };
    for (ColdArm coldArm : { ColdArm::Then, ColdArm::Else, ColdArm::Neither }) {
        for (Negation negation : { Negation::None, Negation::ByBitXor, Negation::ByEqual }) {
            Procedure proc;
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            BasicBlock* truePredecessor = proc.addBlock();
            BasicBlock* falsePredecessor = proc.addBlock();
            BasicBlock* join = proc.addBlock();
            BasicBlock* thenCase = proc.addBlock();
            BasicBlock* elseCase = proc.addBlock();
            auto arguments = cCallArgumentValues<int64_t>(proc, root);
            Value* phi = graph.phi(join, Int32);
            graph.branch(root, arguments[0], truePredecessor, falsePredecessor);
            graph.upsilon(truePredecessor, graph.int32(truePredecessor, 1), phi);
            graph.jump(truePredecessor, join);
            graph.upsilon(falsePredecessor, graph.int32(falsePredecessor, 0), phi);
            graph.jump(falsePredecessor, join);
            Value* condition = phi;
            if (negation == Negation::ByBitXor)
                condition = graph.make(join, BitXor, phi, graph.int32(join, 1));
            if (negation == Negation::ByEqual)
                condition = graph.make(join, Equal, phi, graph.int32(join, 0));
            graph.branch(join, condition, coldArm == ColdArm::Then ? rarely(thenCase) : FrequentedBlock(thenCase), coldArm == ColdArm::Else ? rarely(elseCase) : FrequentedBlock(elseCase));
            graph.ret(thenCase, graph.int64(thenCase, 1));
            graph.ret(elseCase, graph.int64(elseCase, 2));

            proc.resetReachability();
            estimateFrequencies(proc);
            if (coldArm == ColdArm::Neither) {
                CHECK(isNear(truePredecessor->frequency(), 0.5) && isNear(falsePredecessor->frequency(), 0.5));
                CHECK(root->successor(0).isLikely() && root->successor(1).isLikely());
                continue;
            }
            bool truePredecessorIsCold = (coldArm == ColdArm::Then) == (negation == Negation::None);
            CHECK(isNear((truePredecessorIsCold ? falsePredecessor : truePredecessor)->frequency(), 1023.0 / 1024));
            CHECK(isNear((truePredecessorIsCold ? truePredecessor : falsePredecessor)->frequency(), 1.0 / 1024));
            CHECK_EQ(root->successor(0).isRare(), truePredecessorIsCold);
            CHECK_EQ(root->successor(1).isRare(), !truePredecessorIsCold);
        }
    }
}

struct Estimate {
    friend bool operator==(const Estimate&, const Estimate&) = default;

    Vector<double> blockFrequencies;
    Vector<FrequencyClass> edgeClasses;
};

static Estimate collectEstimate(Procedure& proc)
{
    Estimate result;
    for (BasicBlock* block : proc) {
        result.blockFrequencies.append(block->frequency());
        for (const FrequentedBlock& successor : block->successors())
            result.edgeClasses.append(successor.frequency());
    }
    return result;
}

static void testEstimateFrequenciesIgnoresStoredFrequency()
{
    Vector<Estimate> estimates;
    for (double storedFrequency : { 1.0, 1000.0, 0.0 }) {
        Procedure proc;
        Graph graph { proc };
        LoopWithSlowPaths loop(graph, storedFrequency);
        estimateFrequencies(proc);
        estimates.append(collectEstimate(proc));
    }
    CHECK(estimates[1] == estimates[0] && estimates[2] == estimates[0]);
}

static void testEstimateFrequenciesIsIdempotent()
{
    Procedure proc;
    Graph graph { proc };
    LoopWithSlowPaths loop(graph);

    estimateFrequencies(proc);
    Estimate first = collectEstimate(proc);
    estimateFrequencies(proc);
    CHECK(collectEstimate(proc) == first);
}

static double estimateBranchTargetFrequency(FrequencyClass edgeClass)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* target = proc.addBlock();
    BasicBlock* other = proc.addBlock();
    graph.branch(root, cCallArgumentValues<int64_t>(proc, root)[0], FrequentedBlock(target, edgeClass), FrequentedBlock(other));
    graph.ret(target, graph.int64(target, 1));
    graph.ret(other, graph.int64(other, 2));

    proc.resetReachability();
    estimateFrequencies(proc);
    CHECK(root->successor(0).frequency() == edgeClass && root->successor(1).isLikely());
    CHECK(isNear(target->frequency() + other->frequency(), 1));
    return target->frequency();
}

static void testEstimateFrequenciesOfWayOutOfCycleSeldomGoneRound()
{
    Procedure proc(policyWith([](Policy& policy) {
        policy.optimization.estimatesFrequencies = true;
    }));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* choice = proc.addBlock();
    BasicBlock* fastLoop = proc.addBlock();
    BasicBlock* rareCode = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root).eager();
    graph.jump(root, choice);
    graph.branch(choice, arguments[0], FrequentedBlock(fastLoop), rarely(rareCode));
    graph.branch(fastLoop, arguments[1], fastLoop, exit);
    graph.branch(rareCode, arguments[1], choice, exit);
    graph.ret(exit, arguments[0]);
    proc.resetReachability();

    estimateFrequencies(proc);
    CHECK(fastLoop->frequency() > 9);
    CHECK(exit->frequency() > 0.9);
}

static void testEstimateFrequenciesOfUnlikelyEdge()
{
    double normalShare = estimateBranchTargetFrequency(FrequencyClass::Normal);
    double unlikelyShare = estimateBranchTargetFrequency(FrequencyClass::Unlikely);
    double rareShare = estimateBranchTargetFrequency(FrequencyClass::Rare);
    CHECK(isNear(normalShare, 0.5) && isNear(rareShare, 1.0 / 1024));
    CHECK(rareShare * 4 < unlikelyShare && unlikelyShare * 4 < normalShare);
}

static void testUpdateFrequenciesRunsOnlyAfterChanges()
{
    for (bool estimatesFrequencies : { true, false }) {
        Procedure proc(policyWith([&](Policy& policy) {
            policy.optimization.estimatesFrequencies = estimatesFrequencies;
        }));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock(3);
        BasicBlock* trap = proc.addBlock(3);
        BasicBlock* hot = proc.addBlock(3);
        BasicBlock* tail = proc.addBlock(3);
        graph.branch(root, cCallArgumentValues<int64_t>(proc, root)[0], trap, hot);
        appendOops(graph, trap);
        graph.jump(hot, tail);
        graph.ret(tail, graph.int64(tail, 1));
        resetAndValidate(proc);
        CHECK(!proc.blockRarity().isRare(trap));

        proc.updateFrequencies();
        CHECK(isNear(hot->frequency(), estimatesFrequencies ? 1023.0 / 1024 : 3));
        CHECK_EQ(root->successor(0).isRare(), estimatesFrequencies);
        CHECK_EQ(proc.blockRarity().isRare(trap), estimatesFrequencies);

        hot->setFrequency(7);
        proc.updateFrequencies();
        CHECK_EQ(hot->frequency(), 7.0);

        proc.invalidateCFG();
        proc.updateFrequencies();
        CHECK(isNear(hot->frequency(), estimatesFrequencies ? 1023.0 / 1024 : 7));

        hot->setFrequency(7);
        BasicBlock* added = proc.addBlock(3);
        graph.jump(added, tail);
        hot->successorBlock(0) = added;
        proc.resetReachability();
        proc.updateFrequencies();
        CHECK(isNear(hot->frequency(), estimatesFrequencies ? 1023.0 / 1024 : 7));
        CHECK(isNear(added->frequency(), estimatesFrequencies ? 1023.0 / 1024 : 3));
    }
}

static void testUpdateEdgeClassesLeavesBlockFrequenciesForLater()
{
    Policy estimatingPolicy = policyWith([](Policy& policy) {
        policy.optimization.estimatesFrequencies = true;
    });
    constexpr double storedFrequency = 3;
    auto hasStoredFrequencies = [&](Procedure& proc) {
        for (BasicBlock* block : proc) {
            if (block->frequency() != storedFrequency)
                return false;
        }
        return true;
    };

    Procedure reference(estimatingPolicy);
    Graph referenceGraph { reference };
    LoopWithSlowPaths referenceLoop(referenceGraph, storedFrequency);
    reference.updateFrequencies();
    Estimate expected = collectEstimate(reference);

    enum class Event : uint8_t { None, Invalidation, SwappedSuccessors };
    for (Event event : { Event::None, Event::Invalidation, Event::SwappedSuccessors }) {
        Procedure proc(estimatingPolicy);
        Graph graph { proc };
        LoopWithSlowPaths loop(graph, storedFrequency);
        proc.updateEdgeClasses();
        CHECK(collectEstimate(proc).edgeClasses == expected.edgeClasses);
        CHECK(proc.blockRarity().isRare(loop.trap));
        CHECK(hasStoredFrequencies(proc));

        switch (event) {
        case Event::None:
            break;
        case Event::Invalidation:
            proc.invalidateCFG();
            break;
        case Event::SwappedSuccessors:
            proc.willReorderSuccessors();
            CHECK(collectEstimate(proc) == expected);
            std::swap(loop.body->successor(0), loop.body->successor(1));
            proc.didReorderSuccessors();
            break;
        }
        proc.updateFrequencies();
        CHECK(collectEstimate(proc).blockFrequencies == expected.blockFrequencies);
    }
}

static void testBlockRarity()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* rareBlock = proc.addBlock();
    BasicBlock* rareSuccessor = proc.addBlock();
    BasicBlock* next = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    BasicBlock* unlikelyBlock = proc.addBlock();
    BasicBlock* rareAfterUnlikely = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* condition = cCallArgumentValues<int32_t>(proc, root)[0];
    graph.branch(root, condition, rarely(rareBlock), FrequentedBlock(next));
    graph.jump(rareBlock, rareSuccessor);
    graph.jump(rareSuccessor, join);
    graph.branch(next, condition, unlikely(unlikelyBlock), FrequentedBlock(join));
    graph.ret(join, condition);
    graph.branch(unlikelyBlock, condition, rarely(rareAfterUnlikely), FrequentedBlock(header));
    graph.ret(rareAfterUnlikely, condition);
    graph.branch(header, condition, body, exit);
    graph.jump(body, header);
    graph.ret(exit, condition);
    resetAndValidate(proc);

    BlockRarity& rarity = proc.blockRarity();
    for (BasicBlock* block : { root, next, join })
        CHECK(rarity.of(block) == FrequencyClass::Normal);
    for (BasicBlock* block : { unlikelyBlock, header, body, exit })
        CHECK(rarity.of(block) == FrequencyClass::Unlikely);
    for (BasicBlock* block : { rareBlock, rareSuccessor, rareAfterUnlikely })
        CHECK(rarity.of(block) == FrequencyClass::Rare);
    CHECK(rarity.isRare(rareSuccessor) && !rarity.isRare(body) && !rarity.isRare(join));
    CHECK(rarity.isRarerThan(rareBlock, unlikelyBlock) && rarity.isRarerThan(unlikelyBlock, root));
    CHECK(!rarity.isRarerThan(unlikelyBlock, rareBlock) && !rarity.isRarerThan(header, body) && !rarity.isRarerThan(join, root));

    root->successor(0).frequency() = FrequencyClass::Unlikely;
    proc.invalidateCFG();
    CHECK(proc.blockRarity().of(rareSuccessor) == FrequencyClass::Unlikely);
    CHECK(proc.blockRarity().of(join) == FrequencyClass::Normal);
}

void addBunPlacementTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testSinkValuesMovesValueIntoRareUser());
    RUN(testSinkValuesClonesValueForTwoRareUsers());
    RUN(testSinkValuesClonesOnlyPureFunctionForTwoRareUsers());
    RUN(testSinkValuesKeepsValueUsedOnHotPath());
    RUN(testSinkValuesKeepsValueWithEquallyFrequentUser());
    RUN(testSinkValuesKeepsValueUsedInSameBlock());
    RUN(testSinkValuesClonesComparisonFusedIntoBranchForRareUser());
    RUN(testSinkValuesClonesAddressTakenIntoAccessesForRareUsers());
    RUN(testSinkValuesClonesAddressTakenIntoTrappingAccessWhoseBaseIsRegister());
    RUN(testSinkValuesMovesAddressWhoseAccessesAreAllRare());
    RUN(testSinkValuesKeepsAddressThatIsAlsoStored());
    RUN(testSinkValuesSinksWithinIrreducibleCycle());
    RUN(testSinkValuesKeepsLoads());
    RUN(testSinkValuesReadsStackSlotWhereWaysMeet());
    RUN(testSinkValuesKeepsOpaque());
    RUN(testSinkValuesDoesNotSinkIntoLoop());
    RUN(testSinkValuesDoesNotSinkFromPreHeaderIntoLoop());
    RUN(testSinkValuesMovesInductionStepToFirstUse());
    RUN(testSinkValuesSinksPointerChasingLoadBelowOtherLoads());
    RUN(testSinkValuesMovesInductionStepBeforeAskingAboutIrreducibleCycle());
    RUN(testValidateRefusesNullEntryInBlock());
    RUN(testSinkValuesLeavesInductionStepAboveAccessThatTakesItAsAddress());
    RUN(testSinkValuesLeavesAtMostEightLoops());
    RUN(testSinkValuesSinksAfterAndBetweenIrreducibleCycles());
    RUN(testSinkValuesDoesNotSinkIntoIrreducibleCycle());
    RUN(testCycleForestNestedLoops());
    RUN(testCycleForestAcyclic());
    RUN(testCycleForestIrreducibleCycle());
    RUN(testCycleForestPrefersPollingEntryAsHeader());
    RUN(testCycleForestPrefersLikelyEntryAsHeader());
    RUN(testCycleForestPrefersLikelyEntryToBlockThatPollsRarely());
    RUN(testCycleForestEntryBehindRareBlockIsNotLikely());
    RUN(testEstimateFrequenciesTreatsReturningPathAsUnlikely());
    RUN(testEstimateFrequenciesPropagatesWeightsBackward());
    RUN(testEstimateFrequenciesMarksRareEdgesInLoops());
    RUN(testEstimateFrequenciesKeepsWeightsInLoops());
    RUN(testEstimateFrequenciesFollowsBooleanPhi());
    RUN(testEstimateFrequenciesIgnoresStoredFrequency());
    RUN(testEstimateFrequenciesIsIdempotent());
    RUN(testEstimateFrequenciesOfWayOutOfCycleSeldomGoneRound());
    RUN(testEstimateFrequenciesOfUnlikelyEdge());
    RUN(testUpdateFrequenciesRunsOnlyAfterChanges());
    RUN(testUpdateEdgeClassesLeavesBlockFrequenciesForLater());
    RUN(testBlockRarity());
}

#endif // ENABLE(B3_JIT)
