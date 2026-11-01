/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3EliminateRedundantPhiCycles.h"
#include "B3LoopVersionPlan.h"

template<typename Functor>
static void buildRotatedLoopWithInvariantGuard(Graph& graph, TopTestedLoop& loop, const Functor& fillHotBlock, HeapRange loadedHeap = firstHeap)
{
    BasicBlock* rare = graph.proc.addBlock();
    BasicBlock* hot = graph.proc.addBlock();
    Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, loadedHeap);
    graph.branch(loop.body, graph.make(loop.body, Equal, loaded, loop.extra), FrequentedBlock(hot), rarely(rare));
    graph.increase(rare, loop.sum, 1000);
    graph.jump(rare, hot);
    graph.increase(hot, loop.sum, loaded);
    fillHotBlock(hot);
    loop.endBody(hot);
    loop.rotate();
}

static void checkLoopWithInvariantGuard(Procedure& proc)
{
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 9 }) {
        int64_t iterations = std::max<int64_t>(bound, 0);
        int64_t memory[2] = { 7, 0 };
        CHECK_EQ(run(*code, bound, memory, 7), iterations * 7);
        CHECK_EQ(run(*code, bound, memory, 8), iterations * 1007);
    }
}

static void appendUncloneablePatchpoint(Graph& graph, BasicBlock* block)
{
    PatchpointValue* patchpoint = block->appendNew<PatchpointValue>(graph.proc, Void, Origin(), cloningForbidden(Patchpoint));
    patchpoint->effects = Effects::none();
    patchpoint->effects.writesLocalState = true;
    patchpoint->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
}

static void appendIncrementingCall(Graph& graph, BasicBlock* block, Value* pointer, int32_t offset, const HeapSet& accessedHeaps)
{
    PatchpointValue* call = graph.callLikePatchpoint(block);
    call->append(pointer, ValueRep::SomeRegister);
    call->effects.reads = accessedHeaps;
    call->effects.writes = accessedHeaps;
    call->clobber(RegisterSet::macroClobberedGPRs());
    call->setGenerator([offset](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(params[0].gpr(), offset));
    });
}

static void testVersionLoopsHoistsLoadAndGuard()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    buildRotatedLoopWithInvariantGuard(graph, loop, [](BasicBlock*) { });
    checkValueCountInEachLoop(proc, Load, { 1 });

    CHECK(versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopVersioned, GaveUpReason::None));
    CHECK_EQ(proc.report().countOf(Counter::LoopsVersioned), 1u);
    checkValueCountInEachLoop(proc, Load, { 0, 1 });
    checkValueCountInEachLoop(proc, Branch, { 1, 2 });
    checkLoopWithInvariantGuard(proc);
}

static void testVersionLoopsHoistsGuardOnValueBeforeLoop()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* rare = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    graph.branch(loop.body, loop.extra, FrequentedBlock(hot), rarely(rare));
    graph.increase(rare, loop.sum, 1000);
    graph.jump(rare, hot);
    graph.increase(hot, loop.sum, 7);
    loop.endBody(hot);
    loop.rotate();
    checkValueCountInEachLoop(proc, Branch, { 2 });

    CHECK(versionLoopsAndValidate(proc));
    checkValueCountInEachLoop(proc, Branch, { 1, 2 });
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 9 }) {
        int64_t iterations = std::max<int64_t>(bound, 0);
        CHECK_EQ(run(*code, bound, nullptr, 1), iterations * 7);
        CHECK_EQ(run(*code, bound, nullptr, 0), iterations * 1007);
    }
}

static void testVersionLoopsSkipsLoopWithSingleNonGuardHoist()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.load(loop.body, Int64, loop.pointer, 0, firstHeap));
    loop.endBody(loop.body);
    loop.rotate();

    CHECK(!versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopNotVersioned, GaveUpReason::NothingToHoist));
    auto code = compileProc(proc);
    int64_t memory[1] = { 7 };
    for (int64_t bound : { -1, 0, 1, 2, 9 })
        CHECK_EQ(run(*code, bound, memory), std::max<int64_t>(bound, 0) * 7);
}

static void testVersionLoopsHoistsLoadPastStoreToDisjointHeap()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    buildRotatedLoopWithInvariantGuard(graph, loop, [&](BasicBlock* block) {
        graph.store(block, graph.get(block, loop.sum), loop.pointer, 8, secondHeap);
    });

    CHECK(versionLoopsAndValidate(proc));
    checkValueCountInEachLoop(proc, Load, { 0, 1 });
    checkValueCountInEachLoop(proc, Store, { 1, 1 });
    checkLoopWithInvariantGuard(proc);
}

static void testVersionLoopsKeepsLoadClobberedByLoop()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    buildRotatedLoopWithInvariantGuard(graph, loop, [&](BasicBlock* block) {
        graph.store(block, graph.get(block, loop.sum), loop.pointer, 8, bothHeaps);
    });

    CHECK(!versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopNotVersioned, GaveUpReason::NothingToHoist));
    checkLoopWithInvariantGuard(proc);
}

static void testVersionLoopsVersionsLoopWithCall()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    buildRotatedLoopWithInvariantGuard(graph, loop, [&](BasicBlock* block) {
        appendIncrementingCall(graph, block, loop.pointer, 8, proc.heapSets().unite(firstHeap, thirdHeap));
        graph.increase(block, loop.sum, graph.load(block, Int64, loop.pointer, 8, thirdHeap));
    }, secondHeap);
    checkValueCountInEachLoop(proc, Load, { 2 });

    CHECK(versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopVersioned, GaveUpReason::None));
    // The call writes, so the original loop ends in front of the guards of the fast version: the loop around both holds the hoisted load as well.
    checkValueCountInEachLoop(proc, Load, { 1, 3 });
    checkValueCountInEachLoop(proc, Patchpoint, { 1, 1 });
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 9 }) {
        int64_t iterations = std::max<int64_t>(bound, 0);
        for (int64_t extra : { 7, 8 }) {
            int64_t memory[2] = { 7, 0 };
            CHECK_EQ(run(*code, bound, memory, extra), iterations * (extra == 7 ? 7 : 1007) + sumBelow(iterations + 1));
            CHECK_EQ(memory[1], iterations);
        }
    }
}

static void testVersionLoopsSkipsLoopWithUncloneableValue()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    buildRotatedLoopWithInvariantGuard(graph, loop, [&](BasicBlock* block) {
        appendUncloneablePatchpoint(graph, block);
    });

    CHECK(!versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopNotVersioned, GaveUpReason::UncloneableValue));
    checkLoopWithInvariantGuard(proc);
}

static void testVersionLoopsSkipsLargeLoop()
{
    constexpr uint64_t numberOfOtherValues = 9;
    constexpr uint64_t numberOfHoistedValues = 3;
    constexpr uint64_t maxOperationsPerVersion = defaultValueOf(Limit::MaxClonedValuesPerLoopVersion) + numberOfHoistedValues - numberOfOtherValues;
    constexpr uint64_t maxOperationsPerRegion = defaultValueOf(Limit::MaxValuesPerHotRegion) - numberOfOtherValues;
    static_assert(maxOperationsPerVersion + 1 < maxOperationsPerRegion);
    for (uint64_t numberOfOperations : { maxOperationsPerVersion, maxOperationsPerVersion + 1, maxOperationsPerRegion, maxOperationsPerRegion + 1 }) {
        bool fitsVersion = numberOfOperations <= maxOperationsPerVersion;
        bool fitsRegion = numberOfOperations <= maxOperationsPerRegion;
        Procedure proc(remarkPolicy());
        Graph graph { proc };
        TopTestedLoop loop(graph);
        buildRotatedLoopWithInvariantGuard(graph, loop, [&](BasicBlock* block) {
            Value* value = graph.get(block, loop.index);
            for (uint64_t i = 0; i < numberOfOperations; ++i)
                value = graph.make(block, BitXor, value, graph.get(block, loop.index));
            graph.increase(block, loop.sum, graph.make(block, BitAnd, value, graph.int64(block, 0)));
        });
        loop.body->at(0)->setOrigin(numberedOrigin(1));

        CHECK_EQ(versionLoopsAndValidate(proc), fitsVersion);
        CHECK_EQ(hasRemark(proc, RemarkKind::LoopNotVersioned, GaveUpReason::LimitReached), !fitsVersion);
        CHECK_EQ(limitHits(proc, Limit::MaxClonedValuesPerLoopVersion) > 0, !fitsVersion && fitsRegion);
        CHECK_EQ(hasNumber(proc.report().firstOriginOf(Limit::MaxClonedValuesPerLoopVersion), 1), !fitsVersion && fitsRegion);
        CHECK_EQ(limitHits(proc, Limit::MaxValuesPerHotRegion) > 0, !fitsRegion);
        CHECK_EQ(hasNumber(proc.report().firstOriginOf(Limit::MaxValuesPerHotRegion), 1), !fitsRegion);
        checkLoopWithInvariantGuard(proc);
    }
}

static void testVersionLoopsKeepsDeepChainTargetInFastVersion()
{
    for (FrequencyClass chainEdgeClass : { FrequencyClass::Normal, FrequencyClass::Unlikely }) {
        Procedure proc(remarkPolicy());
        Graph graph { proc };
        TopTestedLoop loop(graph);
        BasicBlock* rare = proc.addBlock();
        BasicBlock* hot = proc.addBlock();
        BasicBlock* join = proc.addBlock();
        Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
        graph.branch(loop.body, graph.make(loop.body, Equal, loaded, loop.extra), FrequentedBlock(hot), rarely(rare));
        graph.increase(rare, loop.sum, 1000);
        graph.jump(rare, hot);
        graph.increase(hot, loop.sum, loaded);
        BasicBlock* test = hot;
        for (unsigned caseIndex = 0; caseIndex < 6; ++caseIndex) {
            BasicBlock* targetBlock = proc.addBlock();
            BasicBlock* next = caseIndex < 5 ? proc.addBlock() : join;
            Value* isCase = graph.make(test, Equal, graph.make(test, BitAnd, graph.get(test, loop.index), graph.int64(test, 7)), graph.int64(test, caseIndex));
            graph.branch(test, isCase, FrequentedBlock(targetBlock, chainEdgeClass), FrequentedBlock(next, chainEdgeClass));
            graph.store(targetBlock, graph.get(targetBlock, loop.index), loop.pointer, 8, secondHeap);
            graph.jump(targetBlock, join);
            test = next;
        }
        loop.endBody(join);
        loop.rotate();

        CHECK(versionLoopsAndValidate(proc));
        checkValueCountInEachLoop(proc, Store, { 6, 6 });
        checkValueCountInEachLoop(proc, Load, { 0, 1 });
        checkLoopWithInvariantGuard(proc);
    }
}

static void testVersionLoopsSkipsLoopWithNothingToHoist()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);
    loop.rotate();

    CHECK(!versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopNotVersioned, GaveUpReason::NothingToHoist));
    checkSumsIntegersBelowBound(proc);
}

static void testVersionLoopsOnlyInnermostLoops()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop outer(graph);
    BasicBlock* innerHeader = proc.addBlock();
    BasicBlock* innerBody = proc.addBlock();
    BasicBlock* outerLatch = proc.addBlock();
    Variable* innerIndex = proc.addVariable(Int64);
    Value* loaded = graph.load(outer.body, Int64, outer.pointer, 0, firstHeap);
    graph.increase(outer.body, outer.sum, graph.make(outer.body, Mul, loaded, outer.extra));
    graph.set(outer.body, innerIndex, graph.int64(outer.body, 0));
    graph.jump(outer.body, innerHeader);
    graph.branch(innerHeader, graph.make(innerHeader, LessThan, graph.get(innerHeader, innerIndex), graph.int64(innerHeader, 2)), innerBody, outerLatch);
    appendUncloneablePatchpoint(graph, innerBody);
    graph.increase(innerBody, innerIndex, 1);
    graph.jump(innerBody, innerHeader);
    outer.endBody(outerLatch);
    outer.rotate();

    CHECK(!versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopNotVersioned, GaveUpReason::NotInnermost));
    CHECK(hasRemark(proc, RemarkKind::LoopNotVersioned, GaveUpReason::UncloneableValue));
    int64_t memory[1] = { 5 };
    CHECK_EQ(run(*compileProc(proc), 3, memory, 2), 30);
}

// Around a call that writes, an iteration that left the fast version ends in front of its guards: the load is hoisted again and the fast version goes on.
static void testVersionLoopsReturnsToFastVersionAroundCallThatWrites()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* special = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    Value* increment = graph.make(loop.body, Add, graph.load(loop.body, Int64, loop.pointer, 0, firstHeap), graph.int64(loop.body, 1));
    appendIncrementingCall(graph, loop.body, loop.pointer, 8, secondHeap);
    Value* isSpecial = graph.make(loop.body, Equal, graph.make(loop.body, BitAnd, graph.get(loop.body, loop.index), graph.int64(loop.body, 3)), loop.extra);
    graph.branch(loop.body, isSpecial, rarely(special), FrequentedBlock(hot));
    graph.increase(special, loop.sum, 1000);
    appendIncrementingCall(graph, special, loop.pointer, 0, firstHeap);
    graph.jump(special, hot);
    graph.increase(hot, loop.sum, increment);
    loop.endBody(hot);
    loop.rotate();

    CHECK(versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopVersioned, GaveUpReason::None));
    B3::NaturalLoops& loops = proc.naturalLoops();
    CHECK_EQ(loops.numLoops(), 2u);
    unsigned numberOfLoopsInsideAnother = 0;
    for (unsigned i = 0; i < loops.numLoops(); ++i)
        numberOfLoopsInsideAnother += !!loops.innerMostOuterLoop(loops.loop(i));
    CHECK_EQ(numberOfLoopsInsideAnother, 1u);
    checkValueCountInEachLoop(proc, Load, { 0, 1 });
    auto code = compileProc(proc);
    for (int64_t bound : { 0, 1, 4, 5, 23 }) {
        for (int64_t specialRemainder : { -1, 0, 1, 3 }) {
            int64_t expectedWord = 5;
            int64_t expectedSum = 0;
            for (int64_t i = 0; i < bound; ++i) {
                expectedSum += expectedWord + 1;
                if ((i & 3) != specialRemainder)
                    continue;
                expectedSum += 1000;
                ++expectedWord;
            }
            int64_t memory[2] = { 5, 0 };
            CHECK_EQ(run(*code, bound, memory, specialRemainder), expectedSum);
            CHECK_EQ(memory[0], expectedWord);
            CHECK_EQ(memory[1], bound);
        }
    }
}

// The likely path gives the variable a hoisted value. A rare path that is copied into the fast version gives it another, which the next iteration must see.
static void testVersionLoopsIdentityInHeaderIsNoPhi()
{
    // if (flag) do { sum += changing word + each of the fixed words, times a factor, + (flag ? that word : 5); if (index == 5) changing word = 9; } while (++index <= bound);
    // On that path the Select is an Identity of its load. With this many loads some are left in the loop for versioning to hoist.
    enum class Form : uint8_t { WholePipeline, PhaseAlone, WholePipelineWithFault };
    for (Form form : { Form::WholePipeline, Form::PhaseAlone, Form::WholePipelineWithFault }) {
        bool runsWholePipeline = form != Form::PhaseAlone;
        for (unsigned numberOfLoads : { 1u, 12u, 16u }) {
            Policy policy = optimizingPolicy();
            policy.validation.takesIdentitiesInHeadersForPhis = form == Form::WholePipelineWithFault;
            Procedure proc(policy);
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            BasicBlock* guarded = proc.addBlock();
            BasicBlock* other = proc.addBlock();
            BasicBlock* header = proc.addBlock();
            BasicBlock* rare = proc.addBlock();
            BasicBlock* next = proc.addBlock();
            BasicBlock* exit = proc.addBlock();
            auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
            Value* pointer = arguments[1];
            Value* flag = arguments[2];
            Variable* index = proc.addVariable(Int64);
            Variable* sum = proc.addVariable(Int64);
            graph.set(root, index, graph.int64(root, 0));
            graph.set(root, sum, graph.int64(root, 0));
            graph.branch(root, flag, FrequentedBlock(guarded), FrequentedBlock(other));
            graph.ret(other, graph.int64(other, -5));
            graph.jump(guarded, header);
            Vector<Value*> loads;
            Vector<Value*> chosen;
            for (unsigned i = 0; i < numberOfLoads; ++i) {
                MemoryValue* load = graph.load(header, Int64, pointer, 8 * (i + 1), secondHeap);
                load->setReadsMutability(Mutability::Immutable);
                loads.append(load);
                chosen.append(runsWholePipeline ? graph.select(header, flag, load, graph.int64(header, 5)) : graph.make(header, Identity, load));
            }
            Value* changing = graph.load(header, Int64, pointer, 0, firstHeap);
            graph.branch(header, graph.make(header, Equal, graph.get(header, index), graph.int64(header, 5)), rarely(rare), FrequentedBlock(next));
            graph.store(rare, graph.int64(rare, 9), pointer, 0, firstHeap);
            graph.jump(rare, next);
            graph.increase(next, sum, changing);
            for (unsigned i = 0; i < numberOfLoads; ++i) {
                graph.increase(next, sum, graph.make(next, Mul, loads[i], graph.int64(next, i + 3)));
                graph.increase(next, sum, chosen[i]);
            }
            graph.increase(next, index, 1);
            graph.branch(next, graph.make(next, GreaterEqual, arguments[0], graph.get(next, index)), header, exit);
            graph.ret(exit, graph.get(exit, sum));
            proc.resetReachability();
            validate(proc);
            if (!runsWholePipeline) {
                fixSSA(proc);
                CHECK(versionLoopsAndValidate(proc));
            }

            if (form == Form::WholePipelineWithFault && numberOfLoads > 1) {
                // The variable that stands for a load is read in front of the fast loop, where nothing has set it.
                proc.setOptLevel(2);
                prepareForGeneration(proc);
                CHECK(proc.report().failure && proc.report().failure->kind == FailureKind::VariableReadBeforeSet);
                continue;
            }

            auto code = compileProc(proc, runsWholePipeline ? 2 : 0);
            int64_t memory[17];
            int64_t expected = 0;
            constexpr int64_t bound = 10;
            for (int64_t i = 0; i < 17; ++i)
                memory[i] = 1000 + 7 * i;
            for (int64_t turn = 0; turn <= bound; ++turn) {
                expected += turn <= 5 ? memory[0] : 9;
                for (unsigned i = 0; i < numberOfLoads; ++i)
                    expected += memory[i + 1] * (i + 4);
            }
            CHECK_EQ(invoke<int64_t>(*code, bound, memory, static_cast<int64_t>(3)), expected);
        }
    }
}

static void testVersionLoopsKeepsHeaderPhiThatCopiedRarePathChanges()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    Variable* last = proc.addVariable(Int64);
    Value* first = proc.add<Const64Value>(Origin(), 5);
    loop.root->values().insert(loop.root->size() - 1, first);
    loop.root->values().insert(loop.root->size() - 1, proc.add<VariableValue>(B3::Set, Origin(), last, first));
    BasicBlock* special = proc.addBlock();
    BasicBlock* ordinary = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* expected = proc.addBlock();
    Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
    graph.branch(loop.body, graph.make(loop.body, Below, loaded, graph.int64(loop.body, 100)), FrequentedBlock(expected), rarely(unexpected));
    graph.callLikePatchpoint(unexpected);
    graph.jump(unexpected, expected);
    graph.increase(expected, loop.sum, graph.get(expected, last));
    appendIncrementingCall(graph, expected, loop.pointer, 8, secondHeap);
    Value* isSpecial = graph.make(expected, Equal, graph.make(expected, BitAnd, graph.get(expected, loop.index), graph.int64(expected, 3)), loop.extra);
    graph.branch(expected, isSpecial, rarely(special), FrequentedBlock(ordinary));
    graph.set(special, last, graph.int64(special, 1000));
    graph.jump(special, hot);
    graph.set(ordinary, last, loaded);
    graph.jump(ordinary, hot);
    loop.endBody(hot);
    loop.rotate();

    CHECK(versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopVersioned, GaveUpReason::None));
    auto code = compileProc(proc);
    for (int64_t bound : { 0, 1, 2, 5, 23 }) {
        for (int64_t specialRemainder : { -1, 0, 1, 3 }) {
            for (int64_t word : { 5, 6 }) {
                int64_t expectedSum = 0;
                int64_t expectedLast = 5;
                for (int64_t i = 0; i < bound; ++i) {
                    expectedSum += expectedLast;
                    expectedLast = (i & 3) == specialRemainder ? 1000 : word;
                }
                int64_t memory[2] = { word, 0 };
                CHECK_EQ(run(*code, bound, memory, specialRemainder), expectedSum);
                CHECK_EQ(memory[1], bound);
            }
        }
    }
}

static void testVersionLoopsUncloneableRareEdgeContinuesInOriginalLoop()
{
    enum class RarePath : uint8_t { HasUncloneableValue, StoresToHoistedLocation, CallWritesHoistedLocation };
    for (RarePath rarePath : { RarePath::HasUncloneableValue, RarePath::StoresToHoistedLocation, RarePath::CallWritesHoistedLocation }) {
        Procedure proc(remarkPolicy());
        Graph graph { proc };
        TopTestedLoop loop(graph);
        BasicBlock* special = proc.addBlock();
        BasicBlock* hot = proc.addBlock();
        Value* increment = graph.make(loop.body, Add, graph.load(loop.body, Int64, loop.pointer, 0, firstHeap), graph.int64(loop.body, 1));
        graph.branch(loop.body, graph.make(loop.body, Equal, graph.get(loop.body, loop.index), loop.extra), rarely(special), FrequentedBlock(hot));
        graph.increase(special, loop.sum, 1000);
        switch (rarePath) {
        case RarePath::HasUncloneableValue:
            appendUncloneablePatchpoint(graph, special);
            break;
        case RarePath::StoresToHoistedLocation:
            graph.store(special, graph.int64(special, 8), loop.pointer, 0, firstHeap);
            break;
        case RarePath::CallWritesHoistedLocation:
            appendIncrementingCall(graph, special, loop.pointer, 0, proc.heapSets().unite(firstHeap, thirdHeap));
            break;
        }
        graph.jump(special, hot);
        graph.increase(hot, loop.sum, increment);
        loop.endBody(hot);
        loop.rotate();

        CHECK(versionLoopsAndValidate(proc));
        CHECK_EQ(valueCountInEachLoop(proc, Load).first(), 0u);
        CHECK_EQ(numberOfValues(proc, [](Value* value) { return value->opcode() == Load && value->lastChild()->opcode() != SlotBase; }), 2u);
        // What can be copied ends its turn in a copy, and the fast loop is entered again behind it.
        bool endsTurnInCopy = rarePath != RarePath::HasUncloneableValue;
        CHECK_EQ(numberOfValues(proc, [](Value* value) { return value->isInt64(1000); }), endsTurnInCopy ? 2u : 1u);
        CHECK_EQ(special->numPredecessors(), endsTurnInCopy ? 1u : 2u);
        for (BasicBlock* predecessor : special->predecessors())
            CHECK_EQ(predecessor->numSuccessors(), 1u);
        auto code = compileProc(proc);
        for (int64_t specialIndex : { -1, 0, 4, 9, 10 }) {
            int64_t expectedWord = 5;
            int64_t expectedSum = 0;
            for (int64_t i = 0; i < 10; ++i) {
                expectedSum += expectedWord + 1;
                if (i != specialIndex)
                    continue;
                expectedSum += 1000;
                switch (rarePath) {
                case RarePath::HasUncloneableValue:
                    break;
                case RarePath::StoresToHoistedLocation:
                    expectedWord = 8;
                    break;
                case RarePath::CallWritesHoistedLocation:
                    ++expectedWord;
                    break;
                }
            }
            int64_t memory[1] = { 5 };
            CHECK_EQ(run(*code, 10, memory, specialIndex), expectedSum);
            CHECK_EQ(memory[0], expectedWord);
        }
    }
}

static void testVersionLoopsCloneableRarePathReturnsToFastVersion()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* expected = proc.addBlock();
    BasicBlock* special = proc.addBlock();
    BasicBlock* ordinary = proc.addBlock();
    BasicBlock* calling = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    auto maskedIndex = [&](BasicBlock* block, int64_t mask) {
        return graph.make(block, BitAnd, graph.get(block, loop.index), graph.int64(block, mask));
    };
    Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, secondHeap);
    graph.branch(loop.body, graph.make(loop.body, Below, loaded, graph.int64(loop.body, 100)), FrequentedBlock(expected), rarely(unexpected));
    graph.callLikePatchpoint(unexpected);
    graph.jump(unexpected, expected);
    graph.branch(expected, graph.make(expected, Equal, maskedIndex(expected, 3), loop.extra), rarely(special), FrequentedBlock(ordinary));
    graph.increase(special, loop.sum, 1000);
    graph.jump(special, ordinary);
    graph.branch(ordinary, graph.make(ordinary, Equal, maskedIndex(ordinary, 7), graph.int64(ordinary, 5)), rarely(calling), FrequentedBlock(hot));
    appendIncrementingCall(graph, calling, loop.pointer, 8, proc.heapSets().unite(firstHeap, thirdHeap));
    graph.jump(calling, hot);
    graph.increase(hot, loop.sum, graph.make(hot, Mul, loaded, graph.get(hot, loop.index)));
    loop.endBody(hot);
    loop.rotate();
    checkValueCountInEachLoop(proc, Mul, { 1 });
    checkValueCountInEachLoop(proc, Patchpoint, { 2 });

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(numberOfValues(proc, [](Value* value) { return value->isInt64(1000); }), 2u);
    CHECK_EQ(numberOfValues(proc, Mul), 3u);
    CHECK_EQ(numberOfValues(proc, Load), 2u);
    CHECK_EQ(numberOfValues(proc, Patchpoint), 3u);
    checkValueCountInEachLoop(proc, Mul, { 1, 2 });
    checkValueCountInEachLoop(proc, Load, { 0, 1 });
    checkValueCountInEachLoop(proc, Patchpoint, { 1, 2 });
    CHECK_EQ(proc.naturalLoops().numLoops(), 2u);

    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 5, 40 }) {
        for (int64_t special : { -1, 0, 1, 3, 4 }) {
            for (int64_t factor : { 7, 200 }) {
                int64_t memory[2] = { factor, 0 };
                int64_t sum = 0;
                int64_t numberOfCalls = 0;
                for (int64_t i = 0; i < bound; ++i) {
                    sum += ((i & 3) == special ? 1000 : 0) + factor * i;
                    numberOfCalls += (i & 7) == 5;
                }
                CHECK_EQ(run(*code, bound, memory, special), sum);
                CHECK_EQ(memory[1], numberOfCalls);
            }
        }
    }
}

static void testVersionLoopsSidePathOfLoopThatCalls(FrequencyClass frequencyOfSidePath)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* expected = proc.addBlock();
    BasicBlock* special = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, secondHeap);
    graph.branch(loop.body, graph.make(loop.body, Below, loaded, graph.int64(loop.body, 100)), FrequentedBlock(expected), rarely(unexpected));
    graph.callLikePatchpoint(unexpected);
    graph.jump(unexpected, expected);
    Value* maskedIndex = graph.make(expected, BitAnd, graph.get(expected, loop.index), graph.int64(expected, 3));
    graph.branch(expected, graph.make(expected, Equal, maskedIndex, loop.extra), FrequentedBlock(special, frequencyOfSidePath), FrequentedBlock(hot));
    graph.increase(special, loop.sum, 1000);
    graph.jump(special, hot);
    appendIncrementingCall(graph, hot, loop.pointer, 8, proc.heapSets().unite(firstHeap, thirdHeap));
    graph.increase(hot, loop.sum, graph.make(hot, Mul, loaded, graph.get(hot, loop.index)));
    loop.endBody(hot);
    loop.rotate();
    checkValueCountInEachLoop(proc, Mul, { 1 });
    checkValueCountInEachLoop(proc, Patchpoint, { 2 });

    CHECK(versionLoopsAndValidate(proc));
    // A loop with a call is never unrolled, so an unlikely path jumps back into the fast loop instead of getting its own copy of the call and the product.
    // A rare path still gets that copy: it is cold code.
    unsigned copiesForSidePath = frequencyOfSidePath == FrequencyClass::Rare ? 1 : 0;
    CHECK_EQ(numberOfValues(proc, [](Value* value) { return value->isInt64(1000); }), 2u);
    CHECK_EQ(numberOfValues(proc, Mul), 2u + copiesForSidePath);
    CHECK_EQ(numberOfValues(proc, Patchpoint), 3u + copiesForSidePath);

    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 5, 40 }) {
        for (int64_t special : { -1, 0, 1, 3, 4 }) {
            for (int64_t factor : { 7, 200 }) {
                int64_t memory[2] = { factor, 0 };
                int64_t sum = 0;
                for (int64_t i = 0; i < bound; ++i)
                    sum += ((i & 3) == special ? 1000 : 0) + factor * i;
                CHECK_EQ(run(*code, bound, memory, special), sum);
                CHECK_EQ(memory[1], std::max<int64_t>(bound, 0));
            }
        }
    }
}

static void testVersionLoopsSkipsLoopWithInvariantBackEdgeTest()
{
    for (bool testChanges : { false, true }) {
        Procedure proc(remarkPolicy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* unexpected = proc.addBlock();
        BasicBlock* hot = proc.addBlock();
        BasicBlock* latch = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Int64);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
        Value* bound = arguments[0];
        Value* pointer = arguments[1];
        Value* extra = arguments[2];
        BasicBlock* preHeader = proc.addBlock();
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, preHeader);
        graph.jump(preHeader, body);
        Value* loaded = graph.load(body, Int64, pointer, 0, firstHeap);
        graph.branch(body, graph.make(body, Below, loaded, graph.int64(body, 100)), FrequentedBlock(hot), rarely(unexpected));
        graph.callLikePatchpoint(unexpected);
        graph.jump(unexpected, hot);
        graph.increase(hot, sum, loaded);
        graph.increase(hot, index, 1);
        graph.branch(hot, testChanges ? graph.make(hot, AboveEqual, graph.get(hot, index), bound) : extra, exit, latch);
        graph.jump(latch, body);
        graph.ret(exit, graph.get(exit, sum));
        proc.resetReachability();
        fixSSA(proc);
        validate(proc);

        CHECK_EQ(versionLoopsAndValidate(proc), testChanges);
        if (!testChanges)
            CHECK(hasRemark(proc, RemarkKind::LoopNotVersioned, GaveUpReason::NoSingleLikelyPath));
        auto code = compileProc(proc);
        for (int64_t factor : { 7, 200 }) {
            int64_t memory[1] = { factor };
            CHECK_EQ(run(*code, 3, memory, 1), testChanges ? 3 * factor : factor);
        }
    }
}

static void testVersionLoopsReloadsSingleInputPhiInExcursion()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* extraCase = proc.addBlock();
    BasicBlock* replaced = proc.addBlock();
    BasicBlock* merged = proc.addBlock();
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    Variable* key = proc.addVariable(Int64);
    Value* first = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
    Value* second = graph.load(loop.body, Int64, loop.pointer, 8, firstHeap);
    graph.branch(loop.body, graph.make(loop.body, Equal, graph.make(loop.body, BitAnd, graph.get(loop.body, loop.index), graph.int64(loop.body, 3)), graph.int64(loop.body, 2)), rarely(replaced), FrequentedBlock(extraCase));
    graph.set(extraCase, key, loop.extra);
    graph.jump(extraCase, merged);
    graph.set(replaced, key, graph.int64(replaced, 5));
    graph.jump(replaced, merged);
    graph.branch(merged, graph.make(merged, Equal, graph.get(merged, key), first), FrequentedBlock(hot), rarely(unexpected));
    graph.increase(unexpected, loop.sum, 100);
    graph.jump(unexpected, hot);
    graph.increase(hot, loop.sum, graph.make(hot, Add, graph.get(hot, key), second));
    loop.endBody(hot);
    loop.rotate();

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(numberOfValues(proc, [](Value* value) { return value->isInt64(100); }), 2u);
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 3, 4, 11 }) {
        for (int64_t extra : { 4, 5 }) {
            for (int64_t first : { 4, 5 }) {
                int64_t memory[2] = { first, 7 };
                int64_t sum = 0;
                for (int64_t i = 0; i < bound; ++i) {
                    int64_t chosen = (i & 3) == 2 ? 5 : extra;
                    sum += (chosen == first ? 0 : 100) + chosen + 7;
                }
                CHECK_EQ(run(*code, bound, memory, extra), sum);
            }
        }
    }
}

static void testVersionLoopsHoistsLoadRepeatingLoadBeforeLoop()
{
    enum class LoadBeforeLoop : uint8_t { IsImmutable, IsMutable, IsMutableAndOverwritten };
    for (LoadBeforeLoop loadBeforeLoop : { LoadBeforeLoop::IsImmutable, LoadBeforeLoop::IsMutable, LoadBeforeLoop::IsMutableAndOverwritten }) {
        Procedure proc(remarkPolicy());
        Graph graph { proc };
        TopTestedLoop loop(graph);
        BasicBlock* special = proc.addBlock();
        BasicBlock* guarded = proc.addBlock();
        BasicBlock* unexpected = proc.addBlock();
        BasicBlock* hot = proc.addBlock();
        MemoryValue* earlierLoad = proc.add<MemoryValue>(Load, Int64, Origin(), loop.pointer, 0, firstHeap);
        earlierLoad->setReadsMutability(loadBeforeLoop == LoadBeforeLoop::IsImmutable ? Mutability::Immutable : Mutability::Mutable);
        loop.root->appendNonTerminal(earlierLoad);
        if (loadBeforeLoop == LoadBeforeLoop::IsMutableAndOverwritten)
            loop.root->appendNonTerminal(proc.add<MemoryValue>(Store, Origin(), earlierLoad, loop.pointer, 0, firstHeap));
        graph.branch(loop.body, graph.make(loop.body, Equal, graph.get(loop.body, loop.index), earlierLoad), rarely(special), FrequentedBlock(guarded));
        graph.increase(special, loop.sum, 1000);
        graph.jump(special, guarded);
        Value* loaded = graph.load(guarded, Int64, loop.pointer, 0, firstHeap);
        graph.branch(guarded, graph.make(guarded, Equal, loaded, loop.extra), FrequentedBlock(hot), rarely(unexpected));
        graph.increase(unexpected, loop.sum, 100);
        graph.jump(unexpected, hot);
        graph.increase(hot, loop.sum, loaded);
        loop.endBody(hot);
        loop.rotate();

        proc.resetValueOwners();
        auto plans = LoopVersionPlanner(proc).plan(proc.naturalLoops().loop(0));
        CHECK_EQ(plans.size(), 1u);
        bool provesLoadRunsByPeelingGuard = plans[0].hoisted.numberOfPeeledValues;
        CHECK_EQ(provesLoadRunsByPeelingGuard, loadBeforeLoop == LoadBeforeLoop::IsMutableAndOverwritten);
        CHECK(versionLoopsAndValidate(proc));
        auto code = compileProc(proc);
        int64_t memory[1] = { 4 };
        CHECK_EQ(run(*code, 10, memory, 4), 1040);
        CHECK_EQ(run(*code, 10, memory, 5), 2040);
        CHECK_EQ(run(*code, 3, memory, 4), 12);
        CHECK_EQ(run(*code, 0, memory, 4), 0);
    }
}

// The remains of an `if` that lost one arm: the Phi is in the middle of the header, after the Upsilon that sets it on every iteration. Its other Upsilon is outside the loop, on no path into it.
static void testVersionLoopsPhiThatFollowsItsUpsilonInHeaderIsNotLoopCarried()
{
    for (bool runsPrecedingPhase : { true, false }) {
        Procedure proc(optimizingPolicy());
        Graph graph { proc };
        TopTestedLoop loop(graph);
        BasicBlock* special = proc.addBlock();
        BasicBlock* guarded = proc.addBlock();
        BasicBlock* unexpected = proc.addBlock();
        BasicBlock* hot = proc.addBlock();
        MemoryValue* earlierLoad = proc.add<MemoryValue>(Load, Int64, Origin(), loop.pointer, 0, firstHeap);
        loop.root->appendNonTerminal(earlierLoad);
        loop.root->appendNonTerminal(proc.add<MemoryValue>(Store, Origin(), earlierLoad, loop.pointer, 0, firstHeap));
        UpsilonValue* given = loop.body->appendNew<UpsilonValue>(proc, Origin(), loop.extra);
        Value* phi = graph.phi(loop.body, Int64);
        given->setPhi(phi);
        UpsilonValue* leftOver = proc.add<UpsilonValue>(Origin(), proc.add<Const64Value>(Origin(), 3));
        loop.exit->values().insert(0, leftOver->child(0));
        loop.exit->values().insert(1, leftOver);
        leftOver->setPhi(phi);
        graph.branch(loop.body, graph.make(loop.body, Equal, graph.make(loop.body, Add, graph.get(loop.body, loop.index), phi), earlierLoad), rarely(special), FrequentedBlock(guarded));
        graph.increase(special, loop.sum, 1000);
        graph.jump(special, guarded);
        Value* loaded = graph.load(guarded, Int64, loop.pointer, 0, firstHeap);
        graph.branch(guarded, graph.make(guarded, Equal, loaded, graph.int64(guarded, 4)), FrequentedBlock(hot), rarely(unexpected));
        graph.increase(unexpected, loop.sum, 100);
        graph.jump(unexpected, hot);
        graph.increase(hot, loop.sum, loaded);
        loop.endBody(hot);
        loop.rotate();
        CHECK_EQ(numberOfValues(proc, [&](Value* value) { return value == phi && value->opcode() == Phi; }), 1u);

        if (runsPrecedingPhase)
            CHECK(eliminateRedundantPhiCycles(proc));
        CHECK(versionLoopsAndValidate(proc));
        fixSSA(proc);
        if (!runsPrecedingPhase) {
            // The guard before the fast loop reads the Phi's variable, which nothing has set at that point.
            CHECK(proc.report().failure && proc.report().failure->kind == FailureKind::VariableReadBeforeSet);
            continue;
        }
        CHECK(!proc.report().failure);
        auto code = compileProc(proc);
        int64_t memory[1] = { 4 };
        CHECK_EQ(run(*code, 10, memory, 0), 1040);
        CHECK_EQ(run(*code, 10, memory, 4), 1040);
        CHECK_EQ(run(*code, 10, memory, 5), 40);
        CHECK_EQ(run(*code, 3, memory, 3), 1012);
        CHECK_EQ(run(*code, 0, memory, 4), 0);
    }
}

static void testVersionLoopsResolvesSingleInputPhiInUncountedLoop()
{
    for (bool keyHasOneInput : { true, false }) {
        Procedure proc(remarkPolicy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* preHeader = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* extraCase = proc.addBlock();
        BasicBlock* fetched = proc.addBlock();
        BasicBlock* merged = proc.addBlock();
        BasicBlock* unexpected = proc.addBlock();
        BasicBlock* expected = proc.addBlock();
        BasicBlock* odd = proc.addBlock();
        BasicBlock* even = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Int64);
        Variable* key = proc.addVariable(Int64);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
        Value* bound = arguments[0];
        Value* pointer = arguments[1];
        Value* extra = arguments[2];
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.branch(root, graph.make(root, LessThan, graph.int64(root, 0), bound), preHeader, exit);
        graph.jump(preHeader, header);
        if (keyHasOneInput)
            graph.branch(header, extra, FrequentedBlock(extraCase), rarely(fetched));
        else
            graph.branch(header, graph.make(header, BitAnd, graph.get(header, index), graph.int64(header, 2)), extraCase, fetched);
        graph.set(extraCase, key, extra);
        graph.jump(extraCase, merged);
        graph.set(fetched, key, graph.int64(fetched, 5));
        graph.jump(fetched, merged);
        graph.branch(merged, graph.make(merged, Equal, graph.get(merged, key), graph.load(merged, Int64, pointer, 0, firstHeap)), FrequentedBlock(expected), rarely(unexpected));
        graph.increase(unexpected, sum, 100);
        graph.jump(unexpected, expected);
        graph.increase(expected, sum, graph.load(expected, Int64, pointer, 8, firstHeap));
        graph.branch(expected, graph.make(expected, BitAnd, graph.get(expected, index), graph.int64(expected, 1)), odd, even);
        for (auto [block, step] : { std::pair { odd, 2 }, std::pair { even, 1 } }) {
            graph.increase(block, index, step);
            graph.branch(block, graph.make(block, LessThan, graph.get(block, index), bound), header, exit);
        }
        graph.ret(exit, graph.get(exit, sum));
        proc.resetReachability();
        fixSSA(proc);
        validate(proc);
        checkValueCountInEachLoop(proc, Load, { 2 });

        CHECK_EQ(versionLoopsAndValidate(proc), keyHasOneInput);
        if (keyHasOneInput)
            checkValueCountInEachLoop(proc, Load, { 0, 2 });
        else
            checkValueCountInEachLoop(proc, Load, { 2 });
        auto code = compileProc(proc);
        for (int64_t limit : { -1, 0, 1, 2, 3, 9 }) {
            for (int64_t extra : { 0, 4, 5 }) {
                for (int64_t first : { 4, 5 }) {
                    int64_t memory[2] = { first, 7 };
                    int64_t result = 0;
                    for (int64_t i = 0; i < limit; i += i & 1 ? 2 : 1) {
                        int64_t chosen = (keyHasOneInput ? extra : i & 2) ? extra : 5;
                        result += (chosen == first ? 0 : 100) + 7;
                    }
                    CHECK_EQ(run(*code, limit, memory, extra), result);
                }
            }
        }
    }
}

static void testVersionLoopsUnswitchesInvariantBranch()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* odd = proc.addBlock();
    BasicBlock* even = proc.addBlock();
    BasicBlock* next = proc.addBlock();
    Value* isOdd = graph.make(loop.body, NotEqual, graph.make(loop.body, BitAnd, loop.extra, graph.int64(loop.body, 1)), graph.int64(loop.body, 0));
    graph.branch(loop.body, isOdd, odd, even);
    graph.increase(odd, loop.sum, graph.get(odd, loop.index));
    graph.jump(odd, next);
    graph.increase(even, loop.sum, 1);
    graph.jump(even, next);
    loop.endBody(next);
    loop.rotate();

    CHECK(versionLoopsAndValidate(proc));
    checkValueCountInEachLoop(proc, Branch, { 1, 1, 2 });
    auto code = compileProc(proc);
    for (int64_t bound : { 0, 1, 10 }) {
        CHECK_EQ(run(*code, bound, nullptr, 3), sumBelow(bound));
        CHECK_EQ(run(*code, bound, nullptr, 4), bound);
    }
}

static void testVersionLoopsKeepsVariantBranch()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* odd = proc.addBlock();
    BasicBlock* even = proc.addBlock();
    BasicBlock* next = proc.addBlock();
    Value* isOdd = graph.make(loop.body, NotEqual, graph.make(loop.body, BitAnd, graph.get(loop.body, loop.index), graph.int64(loop.body, 1)), graph.int64(loop.body, 0));
    graph.branch(loop.body, isOdd, odd, even);
    graph.increase(odd, loop.sum, 100);
    graph.jump(odd, next);
    graph.increase(even, loop.sum, 1);
    graph.jump(even, next);
    loop.endBody(next);
    loop.rotate();

    CHECK(!versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopNotVersioned, GaveUpReason::NothingToHoist));
    CHECK_EQ(run(*compileProc(proc), 5), 203);
}

static void testVersionLoopsStoreInExcursionToResolvedHeaderPhi()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* odd = proc.addBlock();
    BasicBlock* even = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    BasicBlock* shrinks = proc.addBlock();
    BasicBlock* latch = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    Variable* count = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
    Value* pointer = arguments[1];
    Value* special = arguments[2];
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    graph.set(header, count, graph.load(header, Int64, pointer, 0, firstHeap));
    graph.branch(header, graph.make(header, LessThan, graph.get(header, index), graph.get(header, count)), body, exit);
    Value* current = graph.get(body, count);
    graph.increase(body, sum, graph.make(body, Add, graph.make(body, Mul, current, current), current));
    graph.branch(body, graph.make(body, BitAnd, graph.get(body, index), graph.int64(body, 1)), odd, even);
    graph.increase(odd, sum, graph.get(odd, count));
    graph.jump(odd, join);
    graph.increase(even, sum, graph.make(even, Add, graph.get(even, count), graph.get(even, count)));
    graph.jump(even, join);
    graph.branch(join, graph.make(join, Equal, graph.get(join, index), special), rarely(shrinks), FrequentedBlock(latch));
    graph.store(shrinks, graph.make(shrinks, Sub, graph.get(shrinks, count), graph.int64(shrinks, 1)), pointer, 0, firstHeap);
    graph.jump(shrinks, latch);
    graph.increase(latch, index, 1);
    graph.jump(latch, header);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
    CHECK(rotateLoops(proc));
    fixSSA(proc);
    validate(proc);

    versionLoopsAndValidate(proc);
    CHECK_EQ(numberOfValues(proc, Store), 1u);
    auto code = compileProc(proc);
    for (int64_t specialIndex : { -1, 0, 3, 8 }) {
        int64_t expectedCount = 10;
        int64_t expectedSum = 0;
        for (int64_t i = 0; i < expectedCount; ++i) {
            int64_t loaded = expectedCount;
            expectedSum += loaded * loaded + loaded + (i & 1 ? loaded : 2 * loaded);
            if (i == specialIndex)
                expectedCount = loaded - 1;
        }
        int64_t memory[1] = { 10 };
        CHECK_EQ(run(*code, 0, memory, specialIndex), expectedSum);
        CHECK_EQ(memory[0], expectedCount);
    }
}

// The count is read ahead of the loop and again at the end of each turn, and each turn stores under another address into the heap that it is read from.
// Told to overlook that store, the fast version lets the first read stand for all of them. What each turn reads is held against it.
static void testVersionLoopsTellsLogWhatStandsForVariableReadAgain()
{
    constexpr int64_t reported = -7;
    for (bool ignoresWrites : { false, true }) {
        Policy policy = remarkPolicy();
        policy.validation.logsDecidedComparisons = true;
        policy.validation.ignoresWritesToLocationsReloadedInLoops = ignoresWrites;
        Procedure proc(policy);
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Int64);
        Variable* count = proc.addVariable(Int64);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
        Value* pointer = arguments[1];
        Value* other = arguments[2];
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, header);
        graph.set(header, count, graph.load(header, Int64, pointer, 0, firstHeap));
        graph.branch(header, graph.make(header, LessThan, graph.get(header, index), graph.get(header, count)), body, exit);
        Value* current = graph.get(body, count);
        graph.increase(body, sum, graph.make(body, Add, graph.make(body, Mul, current, current), current));
        graph.store(body, graph.make(body, Sub, current, graph.int64(body, 1)), other, 0, firstHeap);
        graph.increase(body, index, 1);
        graph.jump(body, header);
        graph.ret(exit, graph.get(exit, sum));
        proc.resetReachability();
        fixSSA(proc);
        validate(proc);
        CHECK(rotateLoops(proc));
        fixSSA(proc);
        validate(proc);

        CHECK_EQ(versionLoopsAndValidate(proc), ignoresWrites);
        bool isChecked = proc.movedLoadLog().insertChecks(proc, nullptr, createSharedTask<void(CCallHelpers&, const MovedLoad&, GPRReg, GPRReg)>([](CCallHelpers& jit, const MovedLoad&, GPRReg, GPRReg) {
            jit.move(CCallHelpers::TrustedImm64(reported), GPRInfo::returnValueGPR);
            jit.emitFunctionEpilogue();
            jit.ret();
        }));
        CHECK_EQ(isChecked, ignoresWrites);
        auto code = compileProc(proc, 1);
        int64_t memory[2] = { 10, 0 };
        CHECK_EQ(run(*code, 0, memory, std::bit_cast<int64_t>(&memory[1])), 1100);
        CHECK_EQ(memory[1], 9);
        int64_t expected = 0;
        for (int64_t i = 0, left = 10; i < left; ++i, --left)
            expected += left * left + left;
        CHECK_EQ(run(*code, 0, memory, std::bit_cast<int64_t>(&memory[0])), ignoresWrites ? reported : expected);
    }
}

static void testVersionLoopsKeepsTerminalPatchpointReadingLoopWrites()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* rare = proc.addBlock();
    BasicBlock* tested = proc.addBlock();
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    Value* key = graph.load(loop.body, Int64, loop.pointer, 8, secondHeap);
    graph.branch(loop.body, graph.make(loop.body, Equal, key, loop.extra), FrequentedBlock(tested), rarely(rare));
    graph.increase(rare, loop.sum, 1000);
    graph.jump(rare, tested);
    PatchpointValue* patchpoint = tested->appendNew<PatchpointValue>(proc, Void, Origin());
    patchpoint->append(loop.pointer, ValueRep::SomeRegister);
    patchpoint->effects = Effects::none();
    patchpoint->effects.terminal = true;
    patchpoint->effects.reads = firstHeap;
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::Jump isExpected = jit.branch64(CCallHelpers::Equal, CCallHelpers::Address(params[0].gpr()), CCallHelpers::TrustedImm64(5));
        CCallHelpers::Jump isUnexpected = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            isExpected.linkTo(*labels[0], &jit);
            isUnexpected.linkTo(*labels[1], &jit);
        });
    });
    tested->setSuccessors(FrequentedBlock(hot), rarely(unexpected));
    graph.increase(unexpected, loop.sum, 100);
    graph.jump(unexpected, hot);
    graph.store(hot, graph.get(hot, loop.index), loop.pointer, 0, firstHeap);
    graph.increase(hot, loop.sum, 1);
    loop.endBody(hot);
    loop.rotate();

    versionLoopsAndValidate(proc);
    for (unsigned patchpointCount : valueCountInEachLoop(proc, Patchpoint))
        CHECK_EQ(patchpointCount, 1u);
    auto code = compileProc(proc);
    for (int64_t extra : { 9, 8 }) {
        int64_t expectedMemory[2] = { 5, 9 };
        int64_t expectedSum = 0;
        for (int64_t i = 0; i < 10; ++i) {
            if (expectedMemory[1] != extra)
                expectedSum += 1000;
            if (expectedMemory[0] != 5)
                expectedSum += 100;
            expectedMemory[0] = i;
            ++expectedSum;
        }
        int64_t memory[2] = { 5, 9 };
        CHECK_EQ(run(*code, 10, memory, extra), expectedSum);
    }
}

// A slot holds the address of a cell, or zero until the cell is made at its first use. Behind the join the address can be read through; what the slot holds cannot, until it has been tested.
static void testVersionLoopsTestsSlotBeforeReadingThroughIt()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* special = proc.addBlock();
    BasicBlock* usual = proc.addBlock();
    BasicBlock* isMade = proc.addBlock();
    BasicBlock* make = proc.addBlock();
    BasicBlock* merged = proc.addBlock();
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    proc.setKnownDereferenceableBytes(loop.pointer, 8);
    // What makes the version with resolved phis the better one.
    BasicBlock* given = proc.addBlock();
    BasicBlock* replaced = proc.addBlock();
    BasicBlock* chosen = proc.addBlock();
    Variable* key = proc.addVariable(Int64);
    graph.branch(loop.body, loop.extra, FrequentedBlock(given), rarely(replaced));
    graph.set(given, key, loop.extra);
    graph.jump(given, chosen);
    graph.set(replaced, key, graph.int64(replaced, 5));
    graph.jump(replaced, chosen);
    Value* mixed = graph.get(chosen, key);
    for (int64_t factor : { 3, 5, 7, 11, 13, 17 })
        mixed = graph.make(chosen, Add, graph.make(chosen, Mul, mixed, graph.int64(chosen, factor)), loop.bound);
    graph.increase(chosen, loop.sum, graph.make(chosen, BitAnd, mixed, graph.int64(chosen, 0)));
    graph.branch(chosen, graph.make(chosen, Equal, graph.get(chosen, loop.index), graph.int64(chosen, 5)), rarely(special), FrequentedBlock(usual));
    graph.ret(special, graph.int64(special, -1));
    Value* held = graph.load(usual, Int64, loop.pointer, 0, firstHeap);
    graph.branch(usual, held, FrequentedBlock(isMade), rarely(make));
    Value* cell = graph.phi(merged, Int64);
    graph.upsilon(isMade, held, cell);
    graph.jump(isMade, merged);
    PatchpointValue* made = make->appendNew<PatchpointValue>(proc, Int64, Origin());
    made->append(loop.pointer, ValueRep::SomeRegister);
    made->append(loop.extra, ValueRep::SomeRegister);
    made->effects = Effects::none();
    made->effects.controlDependent = true;
    made->effects.writes = firstHeap;
    made->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.store64(params[2].gpr(), CCallHelpers::Address(params[1].gpr()));
        jit.move(params[2].gpr(), params[0].gpr());
    });
    graph.upsilon(make, made, cell);
    graph.jump(make, merged);
    MemoryValue* kind = graph.load(merged, Int64, cell, 0, secondHeap);
    kind->setReadsMutability(Mutability::Immutable);
    graph.branch(merged, graph.make(merged, Equal, kind, graph.int64(merged, 7)), FrequentedBlock(hot), rarely(unexpected));
    graph.increase(unexpected, loop.sum, 100);
    graph.jump(unexpected, hot);
    graph.increase(hot, loop.sum, kind);
    loop.endBody(hot);
    loop.rotate();
    CHECK(kind->lastChild()->foldIdentity()->opcode() == Phi);
    proc.setKnownDereferenceableBytes(kind->lastChild()->foldIdentity(), 8);

    CHECK(versionLoopsAndValidate(proc));
    auto code = compileProc(proc);
    for (int64_t kindOfCell : { 7, 8 }) {
        for (bool isMadeBefore : { false, true }) {
            for (int64_t bound : { 0, 1, 2, 3, 9 }) {
                int64_t madeCell[1] = { kindOfCell };
                int64_t slot[1] = { isMadeBefore ? std::bit_cast<int64_t>(&madeCell[0]) : 0 };
                int64_t expected = bound > 5 ? -1 : bound * (kindOfCell + (kindOfCell == 7 ? 0 : 100));
                CHECK_EQ(run(*code, bound, slot, std::bit_cast<int64_t>(&madeCell[0])), expected);
                CHECK_EQ(slot[0], bound || isMadeBefore ? std::bit_cast<int64_t>(&madeCell[0]) : 0);
            }
        }
    }
}

void addBunLoopVersioningTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testVersionLoopsHoistsLoadAndGuard());
    RUN(testVersionLoopsHoistsGuardOnValueBeforeLoop());
    RUN(testVersionLoopsSkipsLoopWithSingleNonGuardHoist());
    RUN(testVersionLoopsHoistsLoadPastStoreToDisjointHeap());
    RUN(testVersionLoopsKeepsLoadClobberedByLoop());
    RUN(testVersionLoopsVersionsLoopWithCall());
    RUN(testVersionLoopsSkipsLoopWithUncloneableValue());
    RUN(testVersionLoopsSkipsLargeLoop());
    RUN(testVersionLoopsKeepsDeepChainTargetInFastVersion());
    RUN(testVersionLoopsSkipsLoopWithNothingToHoist());
    RUN(testVersionLoopsOnlyInnermostLoops());
    RUN(testVersionLoopsReturnsToFastVersionAroundCallThatWrites());
    RUN(testVersionLoopsKeepsHeaderPhiThatCopiedRarePathChanges());
    RUN(testVersionLoopsUncloneableRareEdgeContinuesInOriginalLoop());
    RUN(testVersionLoopsCloneableRarePathReturnsToFastVersion());
    RUN(testVersionLoopsSidePathOfLoopThatCalls(FrequencyClass::Unlikely));
    RUN(testVersionLoopsSidePathOfLoopThatCalls(FrequencyClass::Rare));
    RUN(testVersionLoopsSkipsLoopWithInvariantBackEdgeTest());
    RUN(testVersionLoopsReloadsSingleInputPhiInExcursion());
    RUN(testVersionLoopsHoistsLoadRepeatingLoadBeforeLoop());
    RUN(testVersionLoopsResolvesSingleInputPhiInUncountedLoop());
    RUN(testVersionLoopsUnswitchesInvariantBranch());
    RUN(testVersionLoopsKeepsVariantBranch());
    RUN(testVersionLoopsStoreInExcursionToResolvedHeaderPhi());
    RUN(testVersionLoopsTellsLogWhatStandsForVariableReadAgain());
    RUN(testVersionLoopsKeepsTerminalPatchpointReadingLoopWrites());
    RUN(testVersionLoopsTestsSlotBeforeReadingThroughIt());
    RUN(testVersionLoopsIdentityInHeaderIsNoPhi());
    RUN(testVersionLoopsPhiThatFollowsItsUpsilonInHeaderIsNotLoopCarried());
}

#endif // ENABLE(B3_JIT)
