/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3CaseCollectionInlines.h"
#include "B3EliminateDeadCode.h"
#include "B3InferSwitches.h"
#include <array>

struct InferredSwitch {
    Vector<std::pair<int64_t, int64_t>> resultByCaseValue;
    int64_t fallThroughResult { 0 };

    friend bool operator==(const InferredSwitch&, const InferredSwitch&) = default;
};

static int64_t returnedConstant(BasicBlock* block)
{
    CHECK(block->last()->opcode() == Return);
    return block->last()->child(0)->asInt64();
}

static InferredSwitch inferSingleSwitch(Procedure& proc)
{
    proc.resetReachability();
    validate(proc);
    CHECK(inferSwitches(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(proc, Branch), 0u);
    CHECK_EQ(numberOfValues(proc, Switch), 1u);

    BasicBlock* root = proc.at(0);
    InferredSwitch result;
    for (SwitchCase switchCase : root->last()->as<SwitchValue>()->cases(root))
        result.resultByCaseValue.append(std::pair { switchCase.caseValue(), returnedConstant(switchCase.targetBlock()) });
    std::ranges::sort(result.resultByCaseValue);
    result.fallThroughResult = returnedConstant(root->fallThrough().block());
    return result;
}

template<typename Functor>
static void buildBranchChain(Graph& graph, unsigned degree, const Functor& caseValueAt)
{
    Procedure& proc = graph.proc;
    BasicBlock* block = proc.addBlock();
    Value* subject = cCallArgumentValues<int64_t>(proc, block)[0];
    for (unsigned i = 0; i < degree; ++i) {
        BasicBlock* target = proc.addBlock();
        BasicBlock* next = proc.addBlock();
        graph.branch(block, graph.make(block, Equal, subject, graph.int64(block, caseValueAt(i))), target, next);
        graph.ret(target, graph.int64(target, i));
        block = next;
    }
    graph.ret(block, graph.int64(block, -1));
}

template<typename Functor>
static void buildSwitchPair(Graph& graph, unsigned degree, unsigned firstSwitchDegree, const Functor& caseValueAt)
{
    Procedure& proc = graph.proc;
    BasicBlock* first = proc.addBlock();
    BasicBlock* second = proc.addBlock();
    BasicBlock* fallThrough = proc.addBlock();
    Value* subject = cCallArgumentValues<int64_t>(proc, first)[0];
    SwitchValue* firstSwitch = first->appendNew<SwitchValue>(proc, Origin(), subject);
    firstSwitch->setFallThrough(FrequentedBlock(second));
    SwitchValue* secondSwitch = second->appendNew<SwitchValue>(proc, Origin(), subject);
    secondSwitch->setFallThrough(FrequentedBlock(fallThrough));
    for (unsigned i = 0; i < degree; ++i) {
        BasicBlock* target = proc.addBlock();
        graph.ret(target, graph.int64(target, i));
        (i < firstSwitchDegree ? firstSwitch : secondSwitch)->appendCase(SwitchCase(caseValueAt(i), FrequentedBlock(target)));
    }
    graph.ret(fallThrough, graph.int64(fallThrough, -1));
}

static void testInferSwitchesGrowsInPlaceLikeGeneralPath()
{
    constexpr unsigned threshold = Limits::minCasesToGrowSwitchInPlace;
    constexpr unsigned numberOfRepeatedTests = 4;
    for (unsigned degree : { threshold - 1, threshold, 300u }) {
        unsigned numberOfCases = degree - numberOfRepeatedTests;
        auto caseValueAt = [&](unsigned i) -> int64_t {
            return static_cast<int64_t>(i < numberOfCases ? i : i - numberOfCases) * 3;
        };

        Procedure generalPathProc;
        Graph generalPathGraph { generalPathProc };
        buildSwitchPair(generalPathGraph, degree, std::min(degree / 2, threshold - 1), caseValueAt);
        InferredSwitch expected = inferSingleSwitch(generalPathProc);
        CHECK_EQ(expected.resultByCaseValue.size(), numberOfCases);
        for (unsigned i = 0; i < numberOfCases; ++i)
            CHECK(expected.resultByCaseValue[i] == std::pair(caseValueAt(i), static_cast<int64_t>(i)));
        CHECK_EQ(expected.fallThroughResult, -1);

        Procedure chainProc;
        Graph chainGraph { chainProc };
        buildBranchChain(chainGraph, degree, caseValueAt);
        CHECK(inferSingleSwitch(chainProc) == expected);

        if (numberOfCases > threshold) {
            Procedure severalCasesProc;
            Graph severalCasesGraph { severalCasesProc };
            buildSwitchPair(severalCasesGraph, degree, threshold, caseValueAt);
            CHECK(inferSingleSwitch(severalCasesProc) == expected);
        }

        auto code = compileProc(chainProc);
        for (unsigned i = 0; i < degree; ++i)
            CHECK_EQ(invoke<int64_t>(*code, caseValueAt(i)), caseValueAt(i) / 3);
        CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(1)), -1);
    }
}

static void buildLadder(Graph& graph)
{
    constexpr unsigned numberOfRungs = 64;
    constexpr unsigned rungsPerIncrease = 8;
    constexpr unsigned numberOfVariables = 4;
    Procedure& proc = graph.proc;
    Vector<BasicBlock*> rungs;
    for (unsigned i = 0; i < numberOfRungs; ++i)
        rungs.append(proc.addBlock());
    Vector<Variable*> variables;
    for (unsigned i = 0; i < numberOfVariables; ++i)
        variables.append(proc.addVariable(Int64));

    Value* argument = cCallArgumentValues<int64_t>(proc, rungs[0])[0];
    for (Variable* variable : variables)
        graph.set(rungs[0], variable, argument);
    for (unsigned i = 0; i + 1 < numberOfRungs; ++i) {
        BasicBlock* rung = rungs[i];
        if (i && !(i % rungsPerIncrease)) {
            for (Variable* variable : variables)
                graph.increase(rung, variable, 1);
        }
        if (i + 2 < numberOfRungs)
            graph.branch(rung, graph.make(rung, BitAnd, argument, graph.int64(rung, 1ll << (i % 63))), rungs[i + 1], rungs[i + 2]);
        else
            graph.jump(rung, rungs[i + 1]);
    }
    BasicBlock* lastRung = rungs.last();
    Value* sum = graph.int64(lastRung, 0);
    for (Variable* variable : variables)
        sum = graph.make(lastRung, Add, sum, graph.get(lastRung, variable));
    graph.ret(lastRung, sum);
    proc.resetReachability();
    validate(proc);
}

struct LadderInSSA {
    Vector<unsigned> phiCountByBlock;
};

static LadderInSSA fixSSAInLadder()
{
    Procedure proc;
    Graph graph { proc };
    buildLadder(graph);
    CHECK(fixSSA(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(proc, B3::Get), 0u);
    CHECK_EQ(numberOfValues(proc, B3::Set), 0u);
    LadderInSSA result;
    for (BasicBlock* block : proc)
        result.phiCountByBlock.append(numberOfValues(block, Phi));
    return result;
}

static void testFixSSAPlacesPhisInLadder()
{
    CHECK(fixSSAInLadder().phiCountByBlock.containsIf([](unsigned phiCount) {
        return phiCount;
    }));
}

template<typename Functor>
static bool survivesDeadCodeElimination(const Functor& appendValue)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* pointer = cCallArgumentValues<int64_t*>(proc, root)[0];
    Value* value = appendValue(graph, root, pointer);
    graph.ret(root, graph.int64(root, 0));
    proc.resetReachability();
    validate(proc);

    B3::Opcode opcode = value->opcode();
    bool mustExecute = value->effects().mustExecute();
    eliminateDeadCode(proc);
    validate(proc);
    CHECK_EQ(numberOfValues(proc, opcode), static_cast<unsigned>(mustExecute));
    return mustExecute;
}

static void checkMemoryAccessesMatchEffects(bool traps, HeapRange range, HeapRange fenceRange)
{
    auto kindFor = [&](B3::Opcode opcode) {
        return traps ? trapping(opcode) : B3::Kind(opcode);
    };
    bool loadMustExecute = traps || fenceRange;
    bool storeMustExecute = loadMustExecute || range;

    for (B3::Opcode opcode : { Load8Z, Load8S, Load16Z, Load16S }) {
        CHECK_EQ(survivesDeadCodeElimination([&](Graph& graph, BasicBlock* block, Value* pointer) -> Value* {
            return block->appendNew<MemoryValue>(graph.proc, kindFor(opcode), Origin(), pointer, 0, range, fenceRange);
        }), loadMustExecute);
    }
    CHECK_EQ(survivesDeadCodeElimination([&](Graph& graph, BasicBlock* block, Value* pointer) -> Value* {
        return block->appendNew<MemoryValue>(graph.proc, kindFor(Load), Int64, Origin(), pointer, 0, range, fenceRange);
    }), loadMustExecute);

    for (B3::Opcode opcode : { Store8, Store16 }) {
        CHECK_EQ(survivesDeadCodeElimination([&](Graph& graph, BasicBlock* block, Value* pointer) -> Value* {
            return block->appendNew<MemoryValue>(graph.proc, kindFor(opcode), Origin(), graph.int32(block, 1), pointer, 0, range, fenceRange);
        }), storeMustExecute);
    }
    CHECK_EQ(survivesDeadCodeElimination([&](Graph& graph, BasicBlock* block, Value* pointer) -> Value* {
        return block->appendNew<MemoryValue>(graph.proc, kindFor(Store), Origin(), graph.int64(block, 1), pointer, 0, range, fenceRange);
    }), storeMustExecute);
}

static void testEliminateDeadCodeMatchesEffects()
{
    for (bool traps : { false, true }) {
        for (HeapRange range : { HeapRange(), firstHeap }) {
            for (HeapRange fenceRange : { HeapRange(), secondHeap })
                checkMemoryAccessesMatchEffects(traps, range, fenceRange);
        }
    }

    struct EffectsCase {
        void (*configure)(Effects&);
        bool mustExecute;
    };
    const std::array effectsCases {
        EffectsCase { [](Effects&) { }, false },
        EffectsCase { [](Effects& effects) { effects.reads = firstHeap; }, false },
        EffectsCase { [](Effects& effects) { effects.readsPinned = true; }, false },
        EffectsCase { [](Effects& effects) { effects.readsLocalState = true; }, false },
        EffectsCase { [](Effects& effects) { effects.controlDependent = true; }, false },
        EffectsCase { [](Effects& effects) { effects.writes = firstHeap; }, true },
        EffectsCase { [](Effects& effects) { effects.writesPinned = true; }, true },
        EffectsCase { [](Effects& effects) { effects.writesLocalState = true; }, true },
        EffectsCase { [](Effects& effects) { effects = Effects::forCheck(); }, true },
        EffectsCase { [](Effects& effects) { effects.fence = true; }, true },
    };
    for (const EffectsCase& effectsCase : effectsCases) {
        CHECK_EQ(survivesDeadCodeElimination([&](Graph& graph, BasicBlock* block, Value*) -> Value* {
            PatchpointValue* patchpoint = graph.emptyPatchpoint(block);
            effectsCase.configure(patchpoint->effects);
            return patchpoint;
        }), effectsCase.mustExecute);
        CHECK_EQ(survivesDeadCodeElimination([&](Graph& graph, BasicBlock* block, Value*) -> Value* {
            Effects effects = Effects::none();
            effectsCase.configure(effects);
            return block->appendNew<CCallValue>(graph.proc, Void, Origin(), effects, graph.int64(block, 0));
        }), effectsCase.mustExecute);
    }
}

static void testEliminateDeadCodeKeepsPhiCycleWithLiveUser()
{
    for (bool hasLiveUser : { false, true }) {
        for (bool hasUnusedVariable : { false, true }) {
            Procedure proc;
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            BasicBlock* loop = proc.addBlock();
            BasicBlock* exit = proc.addBlock();
            if (hasUnusedVariable)
                proc.addVariable(Int64);

            Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
            Value* index = graph.phi(loop, Int64);
            Value* first = graph.phi(loop, Int64);
            Value* second = graph.phi(loop, Int64);
            graph.upsilon(root, graph.int64(root, 0), index);
            graph.upsilon(root, graph.int64(root, 10), first);
            graph.upsilon(root, graph.int64(root, 20), second);
            graph.jump(root, loop);

            Value* nextIndex = graph.make(loop, Add, index, graph.int64(loop, 1));
            graph.upsilon(loop, nextIndex, index);
            graph.upsilon(loop, second, first);
            graph.upsilon(loop, graph.make(loop, Add, first, graph.int64(loop, 1)), second);
            graph.branch(loop, graph.make(loop, LessThan, nextIndex, bound), loop, exit);
            graph.ret(exit, hasLiveUser ? first : nextIndex);
            proc.resetReachability();
            validate(proc);

            CHECK_EQ(eliminateDeadCode(proc), !hasLiveUser);
            validate(proc);
            CHECK_EQ(numberOfValues(proc, Phi), hasLiveUser ? 3u : 1u);
            CHECK_EQ(numberOfValues(proc, Upsilon), hasLiveUser ? 6u : 2u);
            CHECK_EQ(numberOfValues(proc, Add), hasLiveUser ? 2u : 1u);
            CHECK(proc.variables().isEmpty());

            auto code = compileProc(proc);
            for (int64_t boundValue : { 0, 1, 2, 3, 10 }) {
                int64_t numberOfIterations = std::max<int64_t>(boundValue, 1);
                int64_t expectedFirst = 10;
                int64_t expectedSecond = 20;
                for (int64_t i = 1; i < numberOfIterations; ++i) {
                    int64_t previousFirst = std::exchange(expectedFirst, expectedSecond);
                    expectedSecond = previousFirst + 1;
                }
                CHECK_EQ(invoke<int64_t>(*code, boundValue), hasLiveUser ? expectedFirst : numberOfIterations);
            }
        }
    }
}

inline void addBunForkedPassTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testInferSwitchesGrowsInPlaceLikeGeneralPath());
    RUN(testFixSSAPlacesPhisInLadder());
    RUN(testEliminateDeadCodeMatchesEffects());
    RUN(testEliminateDeadCodeKeepsPhiCycleWithLiveUser());
}

#endif // ENABLE(B3_JIT)
