/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockUtils.h"
#include "B3InferSwitches.h"
#include "B3LowerMacros.h"
#include "B3SplitBranchConditions.h"

static void buildSwitchOnSmallValues(Procedure& proc, const Vector<int64_t>& values, uint64_t numberOfTargets = 1)
{
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* otherwise = proc.addBlock();
    Vector<BasicBlock*> targets;
    for (uint64_t i = 0; i < numberOfTargets; ++i) {
        targets.append(proc.addBlock());
        graph.ret(targets.last(), graph.int64(targets.last(), i + 1));
    }
    SwitchValue* switchValue = root->appendNew<SwitchValue>(proc, numberedOrigin(1), cCallArgumentValues<int64_t>(proc, root)[0]);
    switchValue->setFallThrough(FrequentedBlock(otherwise));
    for (size_t i = 0; i < values.size(); ++i)
        switchValue->appendCase(SwitchCase(values[i], FrequentedBlock(targets[i % numberOfTargets])));
    graph.ret(otherwise, graph.int64(otherwise, 0));
}

static Policy bitTestPolicy(uint64_t maxTargets = defaultValueOf(Limit::MaxBitTestTargets))
{
    return policyWith([&](Policy& policy) {
        policy.switchLowering.usesBitTests = true;
        policy.limits.set(Limit::MaxBitTestTargets, maxTargets);
    });
}

static void checkBitTestTargetLimit(uint64_t maxTargets)
{
    const Vector<int64_t> values { 0, 1, 2, 3 };
    for (uint64_t numberOfTargets : { maxTargets, maxTargets + 1 }) {
        bool reachesLimit = numberOfTargets > maxTargets;
        {
            Procedure proc(bitTestPolicy(maxTargets));
            buildSwitchOnSmallValues(proc, values, numberOfTargets);
            proc.resetReachability();
            CHECK(lowerMacros(proc));
            CHECK_EQ(numberOfValues(proc, Switch), 0u);
            CHECK_EQ(numberOfValues(proc, ZShr), reachesLimit ? 0 : numberOfTargets - 1);
            CHECK_EQ(limitHits(proc, Limit::MaxBitTestTargets), reachesLimit ? 1u : 0u);
            CHECK_EQ(hasNumber(proc.report().firstOriginOf(Limit::MaxBitTestTargets), 1), reachesLimit);
        }
        Procedure proc(bitTestPolicy(maxTargets));
        buildSwitchOnSmallValues(proc, values, numberOfTargets);
        auto code = compileProc(proc);
        for (int64_t value = -2; value <= 6; ++value) {
            size_t index = values.find(value);
            CHECK_EQ(invoke<int64_t>(*code, value), index == notFound ? 0 : static_cast<int64_t>(index % numberOfTargets + 1));
        }
    }
}

static void testSwitchOnSmallValuesBecomesBitTestWithoutSubtraction()
{
    struct Case {
        Vector<int64_t> values;
        bool subtracts;
        bool testsBit;
    };
    {
        Procedure proc;
        buildSwitchOnSmallValues(proc, { 9, 10, 13, 32 });
        proc.resetReachability();
        CHECK(lowerMacros(proc));
        CHECK_EQ(numberOfValues(proc, Switch), 0u);
        CHECK_EQ(numberOfValues(proc, ZShr), 0u);
    }
    for (const Case& testCase : { Case { { 9, 10, 13, 32 }, false, true }, Case { { 0, 1, 4, 23 }, false, true }, Case { { 109, 110, 113, 132 }, true, true }, Case { { 40, 50, 60, 70 }, true, true }, Case { { -3, -2, 1, 20 }, true, true }, Case { { 48, 49, 50, 51, 52, 53 }, true, false } }) {
        {
            Procedure proc(bitTestPolicy());
            buildSwitchOnSmallValues(proc, testCase.values);
            proc.resetReachability();
            CHECK(lowerMacros(proc));
            CHECK_EQ(numberOfValues(proc, Switch), 0u);
            CHECK_EQ(numberOfValues(proc, Sub), testCase.subtracts ? 1u : 0u);
            CHECK_EQ(numberOfValues(proc, ZShr), testCase.testsBit ? 1u : 0u);
            CHECK_EQ(limitHits(proc, Limit::MaxBitTestTargets), 0u);
        }
        Procedure proc(bitTestPolicy());
        buildSwitchOnSmallValues(proc, testCase.values);
        auto code = compileProc(proc);
        for (int64_t value = -140; value <= 200; ++value)
            CHECK_EQ(invoke<int64_t>(*code, value), static_cast<int64_t>(testCase.values.contains(value)));
        for (int64_t value : { static_cast<int64_t>(9) + (static_cast<int64_t>(1) << 32), static_cast<int64_t>(1) << 40, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max() })
            CHECK_EQ(invoke<int64_t>(*code, value), 0);
    }
    static_assert(defaultValueOf(Limit::MaxBitTestTargets) == 3);
    checkBitTestTargetLimit(defaultValueOf(Limit::MaxBitTestTargets));
    checkBitTestTargetLimit(2);
}

static Policy valueTablePolicy()
{
    return policyWith([&](Policy& policy) {
        policy.switchLowering.usesValueTables = true;
    });
}

enum class ArmOfSwitch : uint8_t { Returns, JoinsWithConstant, JoinsWithSum };

static void buildSwitchThatChoosesNumbers(Procedure& proc, int64_t firstCase, const Vector<int64_t>& numbers, int64_t otherwiseNumber, ArmOfSwitch form, bool lastArmCalls = false, std::optional<int64_t> maskOfSubject = std::nullopt)
{
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* subject = arguments[0];
    Value* addend = arguments[1];
    if (maskOfSubject)
        subject = graph.make(root, BitAnd, subject, graph.int64(root, *maskOfSubject));
    Value* phi = graph.phi(join, Int64);
    graph.ret(join, phi);
    auto addArm = [&](int64_t number, bool calls) {
        BasicBlock* arm = proc.addBlock();
        if (calls)
            graph.callLikePatchpoint(arm);
        Value* constant = graph.int64(arm, number);
        switch (form) {
        case ArmOfSwitch::Returns:
            graph.ret(arm, constant);
            return arm;
        case ArmOfSwitch::JoinsWithConstant:
            graph.upsilon(arm, constant, phi);
            break;
        case ArmOfSwitch::JoinsWithSum:
            graph.upsilon(arm, number ? graph.make(arm, Add, addend, constant) : addend, phi);
            break;
        }
        arm->appendNew<Value>(proc, Jump, Origin());
        arm->setSuccessors(FrequentedBlock(join));
        return arm;
    };
    SwitchValue* switchValue = root->appendNew<SwitchValue>(proc, Origin(), subject);
    switchValue->setFallThrough(FrequentedBlock(addArm(otherwiseNumber, false)));
    for (size_t i = 0; i < numbers.size(); ++i)
        switchValue->appendCase(SwitchCase(firstCase + static_cast<int64_t>(i), FrequentedBlock(addArm(numbers[i], lastArmCalls && i + 1 == numbers.size()))));
}

static void testSwitchWhoseArmsChooseNumbersLoadsFromTable()
{
    struct Case {
        int64_t firstCase;
        Vector<int64_t> numbers;
        int64_t otherwise;
        B3::Opcode load;
    };
    const Case cases[] {
        { 0, { 3, 1, 4, 1, 5, 9, 2, 6 }, 250, Load8Z },
        { 10, { 300, 65535, 256, 0, 4096, 77, 12345, 1 }, 65000, Load16Z },
        { -3, { -1, 2147483647, -2147483647 - 1, 70000, -70000, 0, 255, -256 }, -7, Load },
        { 5, { 1, 2, 3, 4, 5, 6, 7, static_cast<int64_t>(1) << 40 }, -1, Load },
    };
    for (const Case& testCase : cases) {
        for (ArmOfSwitch form : { ArmOfSwitch::Returns, ArmOfSwitch::JoinsWithConstant, ArmOfSwitch::JoinsWithSum }) {
            {
                Procedure proc(valueTablePolicy());
                buildSwitchThatChoosesNumbers(proc, testCase.firstCase, testCase.numbers, testCase.otherwise, form);
                proc.resetReachability();
                CHECK(lowerMacros(proc));
                proc.resetReachability();
                CHECK_EQ(numberOfValues(proc, Switch), 0u);
                CHECK_EQ(numberOfValues(proc, Patchpoint), 0u);
                CHECK_EQ(numberOfValues(proc, testCase.load), 1u);
            }
            for (bool usesTable : { true, false }) {
                Procedure proc(usesTable ? valueTablePolicy() : Policy());
                buildSwitchThatChoosesNumbers(proc, testCase.firstCase, testCase.numbers, testCase.otherwise, form);
                auto code = compileProc(proc);
                int64_t size = static_cast<int64_t>(testCase.numbers.size());
                auto check = [&](int64_t value) {
                    uint64_t index = static_cast<uint64_t>(value) - static_cast<uint64_t>(testCase.firstCase);
                    int64_t chosen = index < static_cast<uint64_t>(size) ? testCase.numbers[index] : testCase.otherwise;
                    CHECK_EQ((invoke<int64_t>(*code, value, static_cast<int64_t>(1000))), form == ArmOfSwitch::JoinsWithSum ? 1000 + chosen : chosen);
                };
                for (int64_t value = testCase.firstCase - 4; value < testCase.firstCase + size + 4; ++value)
                    check(value);
                for (int64_t value : { std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), testCase.firstCase + (static_cast<int64_t>(1) << 32) })
                    check(value);
            }
        }
    }
    const Vector<int64_t> sevenNumbers { 3, 1, 4, 1, 5, 9, 2 };
    for (int64_t mask : { 7, 15 }) {
        {
            Procedure proc(valueTablePolicy());
            buildSwitchThatChoosesNumbers(proc, 0, sevenNumbers, 250, ArmOfSwitch::JoinsWithConstant, false, mask);
            proc.resetReachability();
            CHECK(lowerMacros(proc));
            CHECK_EQ(numberOfValues(proc, Load8Z), 1u);
            CHECK_EQ(numberOfValues(proc, Select), mask == 7 ? 0u : 1u);
        }
        Procedure proc(valueTablePolicy());
        buildSwitchThatChoosesNumbers(proc, 0, sevenNumbers, 250, ArmOfSwitch::JoinsWithConstant, false, mask);
        auto code = compileProc(proc);
        for (int64_t value = -20; value <= 40; ++value) {
            uint64_t index = static_cast<uint64_t>(value & mask);
            CHECK_EQ((invoke<int64_t>(*code, value, static_cast<int64_t>(0))), index < sevenNumbers.size() ? sevenNumbers[index] : 250);
        }
    }

    Procedure proc(valueTablePolicy());
    buildSwitchThatChoosesNumbers(proc, 0, { 3, 1, 4, 1, 5, 9, 2, 6 }, 250, ArmOfSwitch::JoinsWithConstant, true);
    proc.resetReachability();
    CHECK(lowerMacros(proc));
    CHECK_EQ(numberOfValues(proc, Load8Z), 0u);
}

template<typename Functor>
static void buildChainOfEqualityTests(Graph& graph, unsigned degree, const Functor& sourceAndCaseValue)
{
    Procedure& proc = graph.proc;
    BasicBlock* block = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, block);
    Value* first = arguments[0];
    Value* second = arguments[1];
    for (unsigned i = 0; i < degree; ++i) {
        BasicBlock* target = proc.addBlock();
        BasicBlock* next = proc.addBlock();
        auto [usesSecond, caseValue] = sourceAndCaseValue(i);
        graph.branch(block, graph.make(block, Equal, usesSecond ? second : first, graph.int64(block, caseValue)), target, next);
        graph.ret(target, graph.int64(target, i));
        block = next;
    }
    graph.ret(block, graph.int64(block, -1));
    proc.resetReachability();
}

static void testInferSwitchesManyCases()
{
    constexpr unsigned degree = 400;
    Procedure proc;
    Graph graph { proc };
    auto caseValueAt = [](unsigned i) -> int64_t {
        if (i == 200)
            return 5 * 7;
        if (i == 250)
            return std::numeric_limits<int64_t>::min();
        if (i == 300)
            return std::numeric_limits<int64_t>::max();
        return static_cast<int64_t>(i) * 7;
    };
    buildChainOfEqualityTests(graph, degree, [&](unsigned i) {
        return std::pair { false, caseValueAt(i) };
    });

    CHECK(inferSwitches(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(proc, Branch), 0u);
    CHECK_EQ(numberOfValues(proc, Switch), 1u);
    CHECK_EQ(proc.at(0)->last()->as<SwitchValue>()->numCaseValues(), degree - 1);
    auto code = compileProc(proc);
    for (unsigned i = 0; i < degree; ++i)
        CHECK_EQ(invoke<int64_t>(*code, caseValueAt(i), 0), static_cast<int64_t>(i == 200 ? 5 : i));
    CHECK_EQ(invoke<int64_t>(*code, 1, 0), -1);
    CHECK_EQ(invoke<int64_t>(*code, 200 * 7, 0), -1);
}

// `a == v0 || a == v1 || a == v2 || a == v3` or `a != v0 && a != v1 && a != v2 && a != v3`, used as a value.
static void buildComparisonChainUsedAsValue(Procedure& proc, B3::Opcode comparison, const Vector<int32_t>& values)
{
    Graph graph { proc };
    BasicBlock* block = proc.addBlock();
    BasicBlock* shortCircuit = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Value* subject = graph.make(block, Trunc, cCallArgumentValues<int64_t>(proc, block)[0]);
    Value* phi = graph.phi(join, Int32);
    for (unsigned i = 0; i + 1 < values.size(); ++i) {
        BasicBlock* next = proc.addBlock();
        Value* condition = graph.make(block, comparison, subject, graph.int32(block, values[i]));
        if (comparison == Equal)
            graph.branch(block, condition, shortCircuit, next);
        else
            graph.branch(block, condition, next, shortCircuit);
        block = next;
    }
    graph.upsilon(block, graph.make(block, comparison, subject, graph.int32(block, values.last())), phi);
    graph.jump(block, join);
    graph.upsilon(shortCircuit, graph.int32(shortCircuit, comparison == Equal), phi);
    graph.jump(shortCircuit, join);
    graph.ret(join, phi);
    proc.resetReachability();
}

static void testInferSwitchesMergesComparisonUsedAsValue()
{
    struct Case {
        Vector<int32_t> values;
        bool fitsInOneBitTest;
    };
    for (B3::Opcode comparison : { Equal, NotEqual }) {
        for (const Case& testCase : { Case { { 32, 9, 10, 13 }, true }, Case { { 100, 163, 101, 140 }, true }, Case { { 32, 9, 10, 73 }, false }, Case { { 32, 9, 10 }, false } }) {
            for (bool usesPolicy : { false, true }) {
                bool isMerged = usesPolicy && testCase.fitsInOneBitTest;
                Procedure inferred(usesPolicy ? bitTestPolicy() : Policy());
                buildComparisonChainUsedAsValue(inferred, comparison, testCase.values);
                CHECK(inferSwitches(inferred));
                validate(inferred);
                CHECK_EQ(numberOfValues(inferred, Switch), 1u);
                CHECK_EQ(inferred.at(0)->last()->as<SwitchValue>()->numCaseValues(), testCase.values.size() - !isMerged);
                CHECK_EQ(numberOfValues(inferred, comparison), isMerged ? 0u : 1u);
                CHECK(lowerMacros(inferred));
                validate(inferred);
                if (isMerged) {
                    CHECK_EQ(numberOfValues(inferred, Branch), 0u);
                    CHECK_EQ(numberOfValues(inferred, ZShr), 1u);
                }

                Procedure proc(usesPolicy ? bitTestPolicy() : Policy());
                buildComparisonChainUsedAsValue(proc, comparison, testCase.values);
                auto code = compileProc(proc);
                for (int64_t value = -70; value <= 240; ++value)
                    CHECK_EQ(invoke<int32_t>(*code, value), static_cast<int32_t>(testCase.values.contains(static_cast<int32_t>(value)) == (comparison == Equal)));
            }
        }
    }
}

// Both targets of the bit test only produce a constant.
static void testBitTestBetweenConstantsIsBranchless()
{
    struct Case {
        int64_t ifMatched;
        int64_t otherwise;
        bool usesSelect;
    };
    const Vector<int64_t> values { 9, 10, 13, 32 };
    for (const Case& testCase : { Case { 1, 0, false }, Case { 0, 1, false }, Case { 7, 6, true }, Case { -1, 40, true } }) {
        for (bool usesPhi : { false, true }) {
            auto build = [&](Procedure& proc) {
                Graph graph { proc };
                BasicBlock* root = proc.addBlock();
                BasicBlock* matched = proc.addBlock();
                BasicBlock* otherwise = proc.addBlock();
                BasicBlock* join = proc.addBlock();
                Value* phi = graph.phi(join, Int64);
                SwitchValue* switchValue = root->appendNew<SwitchValue>(proc, Origin(), cCallArgumentValues<int64_t>(proc, root)[0]);
                switchValue->setFallThrough(FrequentedBlock(otherwise));
                for (int64_t value : values)
                    switchValue->appendCase(SwitchCase(value, FrequentedBlock(matched)));
                auto produce = [&](BasicBlock* block, int64_t constant) {
                    if (!usesPhi) {
                        graph.ret(block, graph.int64(block, constant));
                        return;
                    }
                    graph.upsilon(block, graph.int64(block, constant), phi);
                    graph.jump(block, join);
                };
                produce(matched, testCase.ifMatched);
                produce(otherwise, testCase.otherwise);
                graph.ret(join, phi);
                proc.resetReachability();
            };
            Procedure lowered(bitTestPolicy());
            build(lowered);
            CHECK(lowerMacros(lowered));
            validate(lowered);
            CHECK_EQ(numberOfValues(lowered, Branch), 0u);
            CHECK_EQ(numberOfValues(lowered, ZShr), 1u);
            CHECK_EQ(numberOfValues(lowered, Select), testCase.usesSelect ? 1u : 0u);

            Procedure proc(bitTestPolicy());
            build(proc);
            auto code = compileProc(proc);
            for (int64_t value = -70; value <= 140; ++value)
                CHECK_EQ(invoke<int64_t>(*code, value), values.contains(value) ? testCase.ifMatched : testCase.otherwise);
        }
    }
}

static void testInferSwitchesStopsAtRareEdge()
{
    constexpr unsigned degree = 8;
    for (bool stopsAtRareEdges : { false, true }) {
        for (FrequencyClass edgeToThirdTest : { FrequencyClass::Normal, FrequencyClass::Unlikely, FrequencyClass::Rare }) {
            Procedure proc(policyWith([&](Policy& policy) { policy.optimization.stopsSwitchInferenceAtRareEdges = stopsAtRareEdges; }));
            Graph graph { proc };
            BasicBlock* block = proc.addBlock();
            Value* subject = cCallArgumentValues<int64_t>(proc, block)[0];
            for (unsigned i = 0; i < degree; ++i) {
                BasicBlock* target = proc.addBlock();
                BasicBlock* next = proc.addBlock();
                bool nextIsThirdTest = i == 1;
                graph.branch(block, graph.make(block, Equal, subject, graph.int64(block, i * 10)), FrequentedBlock(target), FrequentedBlock(next, nextIsThirdTest ? edgeToThirdTest : FrequencyClass::Normal));
                graph.ret(target, graph.int64(target, i));
                block = next;
            }
            graph.ret(block, graph.int64(block, -1));
            proc.resetReachability();

            CHECK(inferSwitches(proc));
            validate(proc);
            bool stops = stopsAtRareEdges && edgeToThirdTest == FrequencyClass::Rare;
            CHECK_EQ(numberOfValues(proc, Branch), 0u);
            CHECK_EQ(numberOfValues(proc, Switch), stops ? 2u : 1u);
            CHECK_EQ(proc.at(0)->last()->as<SwitchValue>()->numCaseValues(), stops ? 2u : degree);
            auto code = compileProc(proc);
            for (unsigned i = 0; i < degree; ++i)
                CHECK_EQ(invoke<int64_t>(*code, i * 10), static_cast<int64_t>(i));
            CHECK_EQ(invoke<int64_t>(*code, 5), -1);
            CHECK_EQ(invoke<int64_t>(*code, degree * 10), -1);
        }
    }
}

static void testInferSwitchesIgnoresTestOfOtherValue()
{
    constexpr unsigned degree = 300;
    Procedure proc;
    Graph graph { proc };
    buildChainOfEqualityTests(graph, degree, [&](unsigned i) {
        return std::pair { i >= 200, static_cast<int64_t>(i) };
    });

    CHECK(inferSwitches(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(proc, Branch), 0u);
    CHECK_EQ(numberOfValues(proc, Switch), 2u);
    CHECK_EQ(proc.at(0)->last()->as<SwitchValue>()->numCaseValues(), 200u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 150, 250), 150);
    CHECK_EQ(invoke<int64_t>(*code, 250, 250), 250);
    CHECK_EQ(invoke<int64_t>(*code, 250, 150), -1);
}

namespace {

struct BranchOnTwoConditions {
    struct Expectation {
        int64_t x;
        int64_t y;
        int64_t result;
    };

    BranchOnTwoConditions()
        : root(proc.addBlock())
        , taken(proc.addBlock())
        , notTaken(proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(proc, root);
        x = arguments[0];
        y = arguments[1];
        pointer = arguments[2];
    }

    Value* int64(int64_t value) { return graph.int64(root, value); }
    Value* make(B3::Opcode opcode, Value* child) { return graph.make(root, opcode, child); }
    Value* make(B3::Opcode opcode, Value* left, Value* right) { return graph.make(root, opcode, left, right); }
    Value* isBitSet(Value* value, int64_t bit) { return make(NotEqual, make(BitAnd, value, int64(bit)), int64(0)); }
    Value* isLess() { return make(LessThan, y, x); }
    void storeAsWord(Value* boolean) { graph.store(root, make(ZExt32, boolean), pointer, 0, firstHeap); }

    bool split(Value* condition)
    {
        graph.branch(root, condition, taken, notTaken);
        graph.ret(taken, graph.int64(taken, 1));
        graph.ret(notTaken, graph.int64(notTaken, 2));
        proc.resetReachability();
        validate(proc);
        bool result = splitBranchConditions(proc);
        validate(proc);
        return result;
    }

    void checkSecondBranch(BasicBlock* block, Value* second)
    {
        CHECK(block != root && block != taken && block != notTaken);
        CHECK(block->last()->opcode() == Branch);
        CHECK(block->last()->child(0) == second);
        CHECK(block->successorBlock(0) == taken);
        CHECK(block->successorBlock(1) == notTaken);
        CHECK(block->values().contains(second));
    }

    void checkResults(std::initializer_list<Expectation> expectations, bool storesY = false)
    {
        auto code = compileProc(proc);
        for (const Expectation& expectation : expectations) {
            int64_t memory[1] = { 3 };
            CHECK_EQ(invoke<int64_t>(*code, expectation.x, expectation.y, memory), expectation.result);
            CHECK_EQ(memory[0], storesY ? expectation.y : 3);
        }
    }

    Procedure proc;
    Graph graph { proc };
    BasicBlock* root;
    BasicBlock* taken;
    BasicBlock* notTaken;
    Value* x;
    Value* y;
    Value* pointer;
};

} // anonymous namespace

static unsigned numberOfJoinedConditions(Procedure& proc)
{
    return numberOfValues(proc, [](Value* value) {
        return (value->opcode() == BitAnd || value->opcode() == BitOr) && value->type() == Int32;
    });
}

static void testSplitBranchConditionsOfAnd()
{
    BranchOnTwoConditions test;
    Value* first = test.isBitSet(test.x, 4);
    Value* second = test.isLess();
    CHECK(test.split(test.make(BitAnd, first, second)));

    CHECK_EQ(numberOfValues(test.proc, Branch), 2u);
    CHECK_EQ(numberOfJoinedConditions(test.proc), 0u);
    CHECK(test.root->last()->child(0) == first);
    CHECK(test.root->successorBlock(1) == test.notTaken);
    test.checkSecondBranch(test.root->successorBlock(0), second);
    test.checkResults({ { 4, 3, 1 }, { 4, 5, 2 }, { 0, -1, 2 }, { 0, 5, 2 } });
}

static void testSplitBranchConditionsOfOr()
{
    BranchOnTwoConditions test;
    Value* first = test.isBitSet(test.x, 4);
    Value* second = test.isLess();
    CHECK(test.split(test.make(BitOr, first, second)));

    CHECK_EQ(numberOfValues(test.proc, Branch), 2u);
    CHECK_EQ(numberOfJoinedConditions(test.proc), 0u);
    CHECK(test.root->last()->child(0) == first);
    CHECK(test.root->successorBlock(0) == test.taken);
    test.checkSecondBranch(test.root->successorBlock(1), second);
    test.checkResults({ { 4, 3, 1 }, { 4, 5, 1 }, { 0, -1, 1 }, { 0, 5, 2 } });
}

static void testSplitBranchConditionsNested()
{
    BranchOnTwoConditions test;
    Value* first = test.isBitSet(test.x, 4);
    Value* second = test.isBitSet(test.y, 4);
    Value* firstAndSecond = test.make(BitAnd, first, second);
    Value* third = test.isBitSet(test.x, 8);
    CHECK(test.split(test.make(BitAnd, firstAndSecond, third)));

    CHECK_EQ(numberOfValues(test.proc, Branch), 3u);
    CHECK_EQ(numberOfJoinedConditions(test.proc), 0u);
    CHECK(test.root->last()->child(0) == first);
    test.checkResults({ { 12, 4, 1 }, { 4, 4, 2 }, { 12, 0, 2 }, { 8, 4, 2 } });
}

static void testSplitBranchConditionsKeepsLoadAboveStore()
{
    for (bool hasStore : { true, false }) {
        BranchOnTwoConditions test;
        Value* first = test.isBitSet(test.x, 4);
        Value* loaded = test.graph.load(test.root, Int64, test.pointer, 0, firstHeap);
        if (hasStore)
            test.graph.store(test.root, test.y, test.pointer, 0, firstHeap);
        Value* second = test.make(LessThan, loaded, test.x);
        CHECK(test.split(test.make(BitAnd, first, second)));

        CHECK_EQ(numberOfValues(test.proc, Branch), 2u);
        CHECK_EQ(numberOfValues(test.proc, Load), 1u);
        CHECK_EQ(numberOfValues(test.root, Load), hasStore ? 1u : 0u);
        test.checkSecondBranch(test.root->successorBlock(0), second);
        test.checkResults({ { 4, 9, 1 }, { -4, 9, 2 }, { 8, 9, 2 }, { 0, 9, 2 } }, hasStore);
    }
}

template<typename Functor>
static void checkIsNotSplit(const Functor& buildCondition)
{
    BranchOnTwoConditions test;
    CHECK(!test.split(buildCondition(test)));
    CHECK_EQ(numberOfValues(test.proc, Branch), 1u);
}

static void testSplitBranchConditionsKeepsOtherConditions()
{
    checkIsNotSplit([](BranchOnTwoConditions& test) {
        Value* condition = test.make(BitAnd, test.isBitSet(test.x, 4), test.isLess());
        test.storeAsWord(condition);
        return condition;
    });
    for (bool firstHasSecondUser : { true, false }) {
        checkIsNotSplit([&](BranchOnTwoConditions& test) {
            Value* first = test.isBitSet(test.x, 4);
            Value* second = test.isLess();
            test.storeAsWord(firstHasSecondUser ? first : second);
            return test.make(BitAnd, first, second);
        });
    }
    checkIsNotSplit([](BranchOnTwoConditions& test) {
        Value* operand = test.isLess();
        return test.make(BitAnd, operand, operand);
    });
    checkIsNotSplit([](BranchOnTwoConditions& test) {
        return test.make(BitAnd, test.isLess(), test.graph.int32(test.root, 1));
    });
    checkIsNotSplit([](BranchOnTwoConditions& test) {
        return test.make(BitAnd, test.make(Trunc, test.x), test.make(Trunc, test.y));
    });
    for (bool firstIsBoolean : { true, false }) {
        checkIsNotSplit([&](BranchOnTwoConditions& test) {
            Value* boolean = test.isLess();
            Value* word = test.make(Trunc, test.y);
            return firstIsBoolean ? test.make(BitAnd, boolean, word) : test.make(BitAnd, word, boolean);
        });
    }
    checkIsNotSplit([](BranchOnTwoConditions& test) {
        return test.make(BitOr,test.make(Equal, test.x, test.int64(1)), test.make(Equal, test.x, test.int64(2)));
    });
    checkIsNotSplit([](BranchOnTwoConditions& test) {
        Value* condition = test.make(BitAnd, test.isBitSet(test.x, 4), test.isLess());
        BasicBlock* entry = test.root;
        test.root = test.proc.addBlock();
        test.graph.jump(entry, test.root);
        return condition;
    });
}

static void testSplitBranchConditionsLeavesCompareChainsToARM64()
{
    BranchOnTwoConditions test;
    Value* first = test.make(LessThan, test.x, test.y);
    Value* second = test.make(LessThan, test.y, test.int64(5));
    CHECK_EQ(test.split(test.make(BitAnd, first, second)), !isARM64());

    CHECK_EQ(numberOfValues(test.proc, Branch), isARM64() ? 1u : 2u);
    test.checkResults({ { 1, 2, 1 }, { 2, 1, 2 }, { 1, 7, 2 }, { 9, 7, 2 } });
}

static void testSplitBranchConditionsSplitsUnselectableCompareChains()
{
    {
        BranchOnTwoConditions test;
        Value* first = test.make(BitAnd, test.make(LessThan, test.x, test.y), test.make(LessThan, test.y, test.int64(5)));
        Value* second = test.make(BitAnd, test.make(LessThan, test.y, test.x), test.make(LessThan, test.x, test.int64(5)));
        CHECK(test.split(test.make(BitOr, first, second)));

        CHECK_EQ(numberOfValues(test.proc, Branch), isARM64() ? 2u : 4u);
        CHECK_EQ(numberOfJoinedConditions(test.proc), isARM64() ? 2u : 0u);
        test.checkResults({ { 1, 2, 1 }, { 2, 1, 1 }, { 1, 7, 2 }, { 7, 1, 2 }, { 3, 3, 2 } });
    }
    {
        BranchOnTwoConditions test;
        Value* condition = test.make(LessThan, test.x, test.y);
        for (int64_t bound = 10; bound < 15; ++bound)
            condition = test.make(BitAnd, condition, test.make(LessThan, test.x, test.int64(bound)));
        CHECK(test.split(condition));

        CHECK_EQ(numberOfValues(test.proc, Branch), isARM64() ? 2u : 6u);
        CHECK_EQ(numberOfJoinedConditions(test.proc), isARM64() ? 4u : 0u);
        test.checkResults({ { 1, 2, 1 }, { 2, 1, 2 }, { 12, 20, 2 }, { 20, 30, 2 } });
    }
    {
        BranchOnTwoConditions test;
        Value* shared = test.make(LessThan, test.x, test.y);
        test.make(ZExt32, shared);
        Value* first = test.make(BitAnd, shared, test.make(LessThan, test.y, test.int64(5)));
        CHECK(test.split(test.make(BitAnd, first, test.make(LessThan, test.x, test.int64(3)))));

        CHECK_EQ(numberOfValues(test.proc, Branch), 2u);
        CHECK_EQ(numberOfJoinedConditions(test.proc), 1u);
        CHECK(test.root->last()->child(0) == first);
        test.checkResults({ { 1, 2, 1 }, { 2, 1, 2 }, { 1, 7, 2 }, { 3, 4, 2 } });
    }
}

static void testLargePredecessorLists()
{
    Procedure proc;
    constexpr unsigned numberOfPredecessors = 200;
    BasicBlock* target = proc.addBlock();
    Vector<BasicBlock*> blocks;
    for (unsigned i = 0; i < numberOfPredecessors; ++i)
        blocks.append(proc.addBlock());

    LargePredecessorLists<BasicBlock> lists;
    for (BasicBlock* block : blocks)
        CHECK(lists.add(target, block));
    CHECK_EQ(target->numPredecessors(), numberOfPredecessors);
    for (BasicBlock* block : blocks)
        CHECK(!lists.add(target, block));
    CHECK_EQ(target->numPredecessors(), numberOfPredecessors);

    for (unsigned i = 0; i < numberOfPredecessors; i += 2) {
        CHECK(lists.remove(target, blocks[i]));
        CHECK(!lists.remove(target, blocks[i]));
    }
    CHECK_EQ(target->numPredecessors(), numberOfPredecessors / 2);
    for (unsigned i = 0; i < numberOfPredecessors; ++i)
        CHECK_EQ(target->containsPredecessor(blocks[i]), !!(i & 1));

    CHECK(lists.replace(target, blocks[1], blocks[0]));
    CHECK(target->containsPredecessor(blocks[0]) && !target->containsPredecessor(blocks[1]));
    CHECK_EQ(target->numPredecessors(), numberOfPredecessors / 2);
    CHECK(lists.replace(target, blocks[3], blocks[5]));
    CHECK(target->containsPredecessor(blocks[5]) && !target->containsPredecessor(blocks[3]));
    CHECK_EQ(target->numPredecessors(), numberOfPredecessors / 2 - 1);
    CHECK(!lists.replace(target, blocks[2], blocks[5]));

    for (unsigned i = 0; i < numberOfPredecessors; ++i)
        lists.remove(target, blocks[i]);
    CHECK_EQ(target->numPredecessors(), 0u);
    CHECK(lists.add(target, blocks[7]));
    CHECK(!lists.add(target, blocks[7]));
    CHECK_EQ(target->numPredecessors(), 1u);
}

static void testLargePredecessorListsDuplicatePredecessor()
{
    Procedure proc;
    BasicBlock* target = proc.addBlock();
    Vector<BasicBlock*> blocks;
    for (unsigned i = 0; i < 100; ++i) {
        blocks.append(proc.addBlock());
        target->predecessors().append(blocks.last());
    }
    target->predecessors().append(blocks[0]);

    LargePredecessorLists<BasicBlock> lists;
    CHECK(!lists.add(target, blocks[50]));
    BasicBlock* other = proc.addBlock();
    CHECK(lists.add(target, other));
    CHECK_EQ(target->numPredecessors(), 102u);
    CHECK(lists.remove(target, blocks[50]));
    CHECK(!target->containsPredecessor(blocks[50]));
    CHECK_EQ(target->numPredecessors(), 101u);
}

static void testBreakCriticalEdgesManyPredecessors()
{
    constexpr unsigned degree = 150;
    Procedure proc;
    Graph graph { proc };
    BasicBlock* block = proc.addBlock();
    BasicBlock* shared = proc.addBlock();
    Value* operand = cCallArgumentValues<int64_t>(proc, block)[0];
    Value* phi = graph.phi(shared, Int64);
    for (unsigned i = 0; i < degree; ++i) {
        BasicBlock* next = proc.addBlock();
        graph.upsilon(block, graph.int64(block, i * 2), phi);
        graph.branch(block, graph.make(block, Equal, operand, graph.int64(block, i)), shared, next);
        block = next;
    }
    graph.ret(block, graph.int64(block, -1));
    graph.ret(shared, phi);
    proc.resetReachability();
    CHECK_EQ(shared->numPredecessors(), degree);

    breakCriticalEdges(proc);
    validate(proc);
    CHECK_EQ(shared->numPredecessors(), degree);
    for (BasicBlock* predecessor : shared->predecessors()) {
        CHECK_EQ(predecessor->numSuccessors(), 1u);
        CHECK_EQ(predecessor->numPredecessors(), 1u);
        CHECK(predecessor->last()->opcode() == Jump);
    }
    CHECK_EQ(numberOfBlocks(proc), 2 * degree + 2);
    auto code = compileProc(proc);
    for (unsigned i = 0; i < degree; ++i)
        CHECK_EQ(invoke<int64_t>(*code, i), static_cast<int64_t>(i * 2));
    CHECK_EQ(invoke<int64_t>(*code, degree), -1);
}

static void testResetReachabilityRemovesUpsilonsOfDeadPhis()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* dead = proc.addBlock();
    BasicBlock* live = proc.addBlock();
    Value* operand = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* deadPhi = graph.phi(dead, Int64);
    Value* livePhi = graph.phi(live, Int64);
    graph.upsilon(root, operand, deadPhi);
    graph.upsilon(root, operand, livePhi);
    graph.jump(root, live);
    graph.ret(dead, deadPhi);
    graph.ret(live, livePhi);

    proc.resetReachability();
    validate(proc);
    CHECK_EQ(numberOfBlocks(proc), 2u);
    CHECK_EQ(numberOfValues(proc, Upsilon), 1u);
    CHECK_EQ(numberOfValues(proc, Phi), 1u);
    CHECK_EQ(compileAndRun<int64_t>(proc, 42), 42);
}

void addBunControlFlowTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testSwitchOnSmallValuesBecomesBitTestWithoutSubtraction());
    RUN(testInferSwitchesManyCases());
    RUN(testInferSwitchesMergesComparisonUsedAsValue());
    RUN(testBitTestBetweenConstantsIsBranchless());
    RUN(testInferSwitchesStopsAtRareEdge());
    RUN(testSwitchWhoseArmsChooseNumbersLoadsFromTable());
    RUN(testInferSwitchesIgnoresTestOfOtherValue());
    RUN(testSplitBranchConditionsOfAnd());
    RUN(testSplitBranchConditionsOfOr());
    RUN(testSplitBranchConditionsNested());
    RUN(testSplitBranchConditionsKeepsLoadAboveStore());
    RUN(testSplitBranchConditionsKeepsOtherConditions());
    RUN(testSplitBranchConditionsLeavesCompareChainsToARM64());
    RUN(testSplitBranchConditionsSplitsUnselectableCompareChains());
    RUN(testLargePredecessorLists());
    RUN(testLargePredecessorListsDuplicatePredecessor());
    RUN(testBreakCriticalEdgesManyPredecessors());
    RUN(testResetReachabilityRemovesUpsilonsOfDeadPhis());
}

#endif // ENABLE(B3_JIT)
