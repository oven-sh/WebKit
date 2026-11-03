/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_region_cloner.h"

#if ENABLE(B3_JIT)

#include "B3DominatingConditions.h"
#include "B3IntRange.h"

namespace {

struct BranchTargets {
    BranchTargets()
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
        x = arguments[0];
        y = arguments[1];
        graph.ret(taken, graph.int64(taken, 10));
        graph.ret(notTaken, graph.int64(notTaken, 20));
    }

    void branch(BasicBlock* from, Value* condition)
    {
        graph.branch(from, condition, taken, notTaken);
        resetAndValidate(proc);
    }

    Procedure proc;
    Graph graph { proc };
    BasicBlock* root { proc.addBlock() };
    BasicBlock* taken { proc.addBlock() };
    BasicBlock* notTaken { proc.addBlock() };
    Value* x;
    Value* y;
};

enum class ImplyingEdge : uint8_t {
    Taken,
    NotTaken,
    None,
};

} // anonymous namespace

static void checkCondition(std::optional<DominatingCondition> condition, Value* value, bool isNonZero)
{
    CHECK(condition);
    CHECK(condition->value == value);
    CHECK_EQ(condition->isNonZero, isNonZero);
}

static void testConditionAtHead()
{
    {
        BranchTargets blocks;
        blocks.branch(blocks.root, blocks.x);
        checkCondition(conditionAtHead(blocks.taken), blocks.x, true);
        checkCondition(conditionAtHead(blocks.notTaken), blocks.x, false);
        CHECK(!conditionAtHead(blocks.root));
        blocks.root->taken() = FrequentedBlock(blocks.proc.addBlock());
        CHECK(!conditionAtHead(blocks.taken));
    }
    {
        BranchTargets blocks;
        blocks.branch(blocks.root, blocks.graph.make(blocks.root, Equal, blocks.x, blocks.graph.int64(blocks.root, 0)));
        checkCondition(conditionAtHead(blocks.taken), blocks.x, false);
        checkCondition(conditionAtHead(blocks.notTaken), blocks.x, true);
    }
    {
        BranchTargets blocks;
        Value* below = blocks.graph.make(blocks.root, Below, blocks.x, blocks.y);
        blocks.branch(blocks.root, blocks.graph.make(blocks.root, BitXor, below, blocks.graph.int32(blocks.root, 1)));
        checkCondition(conditionAtHead(blocks.taken), below, false);
        checkCondition(conditionAtHead(blocks.notTaken), below, true);
    }
    {
        BranchTargets blocks;
        Graph& graph = blocks.graph;
        BasicBlock* test = blocks.proc.addBlock();
        Value* phi = graph.phi(test, Int32);
        graph.upsilon(blocks.root, graph.make(blocks.root, Trunc, blocks.x), phi);
        graph.jump(blocks.root, test);
        Value* bitXor = graph.make(test, BitXor, phi, graph.int32(test, 1));
        blocks.branch(test, bitXor);
        checkCondition(conditionAtHead(blocks.taken), bitXor, true);
        CHECK(!conditionAtHead(test));
    }
    {
        Procedure proc;
        Graph graph { proc };
        Diamond diamond(graph);
        diamond.returnSumFromJoin();
        CHECK(!conditionAtHead(diamond.join));
    }
    {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* successor = proc.addBlock();
        graph.branch(root, cCallArgumentValues<int64_t>(proc, root)[0], successor, successor);
        graph.ret(successor, graph.int64(successor, 0));
        resetAndValidate(proc);
        CHECK(!conditionAtHead(successor));
    }
}

static void checkChain(const DominatingConditions& conditions, DominatingConditions::Chain chain, std::initializer_list<DominatingCondition> expected)
{
    Vector<DominatingCondition> found;
    conditions.forEach(chain, [&](const DominatingCondition& condition) {
        found.append(condition);
        return IterationStatus::Continue;
    });
    CHECK_EQ(found.size(), expected.size());
    unsigned index = 0;
    for (const DominatingCondition& condition : expected)
        checkCondition(found[index++], condition.value, condition.isNonZero);
}

static void testConditionChains()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* outer = proc.addBlock();
    BasicBlock* outerElse = proc.addBlock();
    BasicBlock* inner = proc.addBlock();
    BasicBlock* innerElse = proc.addBlock();
    BasicBlock* innerJoin = proc.addBlock();
    BasicBlock* outerJoin = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* x = arguments[0];
    Value* y = arguments[1];
    graph.branch(root, x, outer, outerElse);
    graph.branch(outer, y, inner, innerElse);
    graph.jump(inner, innerJoin);
    graph.jump(innerElse, innerJoin);
    graph.jump(innerJoin, outerJoin);
    graph.jump(outerElse, outerJoin);
    graph.ret(outerJoin, x);
    resetAndValidate(proc);

    DominatingConditions conditions(proc);
    checkChain(conditions, conditions.chainAt(inner), { { y, true }, { x, true } });
    checkChain(conditions, conditions.chainAt(innerElse), { { y, false }, { x, true } });
    checkChain(conditions, conditions.chainAt(innerJoin), { { x, true } });
    checkChain(conditions, conditions.chainAt(outerElse), { { x, false } });
    checkChain(conditions, conditions.chainAt(outerJoin), { });
    checkChain(conditions, conditions.chainAt(root), { });

    DominatingConditions::Chain extended = conditions.extend(conditions.chainAt(inner), { x, false });
    checkChain(conditions, extended, { { x, false }, { y, true }, { x, true } });
    checkChain(conditions, conditions.chainAt(inner), { { y, true }, { x, true } });
}

static void testConditionQueriesStopAtLimit()
{
    constexpr unsigned limit = defaultValueOf(Limit::MaxConditionQueriesPerBlock);
    static_assert(limit < 63);
    Procedure proc;
    Graph graph { proc };
    Vector<BasicBlock*> blocks;
    for (unsigned i = 0; i <= limit + 1; ++i)
        blocks.append(proc.addBlock());
    BasicBlock* exit = proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(proc, blocks[0])[0];
    for (unsigned i = 0; i <= limit; ++i)
        graph.branch(blocks[i], graph.make(blocks[i], BitAnd, x, graph.int64(blocks[i], 1ll << i)), blocks[i + 1], exit);
    graph.jump(blocks[limit + 1], exit);
    graph.ret(exit, x);
    resetAndValidate(proc);

    DominatingConditions conditions(proc);
    auto numberOfCalls = [&](BasicBlock* block, IterationStatus status) {
        unsigned result = 0;
        conditions.forEach(conditions.chainAt(block), [&](const DominatingCondition&) {
            ++result;
            return status;
        });
        return result;
    };
    CHECK_EQ(numberOfCalls(blocks[limit], IterationStatus::Continue), limit);
    CHECK_EQ(limitHits(proc, Limit::MaxConditionQueriesPerBlock), 0u);
    CHECK_EQ(numberOfCalls(blocks[limit + 1], IterationStatus::Continue), limit);
    CHECK_EQ(limitHits(proc, Limit::MaxConditionQueriesPerBlock), 1u);
    CHECK_EQ(numberOfCalls(blocks[limit + 1], IterationStatus::Done), 1u);
    CHECK_EQ(limitHits(proc, Limit::MaxConditionQueriesPerBlock), 1u);
}

static void testConditionStackLayers()
{
    constexpr unsigned initialSize = 2;
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Vector<Value*> values;
    for (unsigned i = 0; i < 2 * initialSize; ++i)
        values.append(graph.int64(root, i));
    graph.ret(root, values[0]);
    resetAndValidate(proc);
    CHECK(values[initialSize - 1]->index() < initialSize);
    CHECK(values[initialSize]->index() >= initialSize);

    DominatingConditionStack stack;
    stack.reset(initialSize);
    for (unsigned first : { 0u, initialSize }) {
        Value* outerValue = values[first];
        Value* innerValue = values[first + 1];
        stack.pushLayer();
        CHECK(stack.add(outerValue, true));
        CHECK(!stack.add(outerValue, true));
        CHECK(stack.isNonZero(outerValue) == TriState::True);
        stack.pushLayer();
        CHECK(stack.add(innerValue, false));
        CHECK(stack.isNonZero(innerValue) == TriState::False);
        CHECK(stack.add(outerValue, false));
        CHECK(stack.isNonZero(outerValue) == TriState::False);
        stack.popLayer();
        CHECK(stack.isNonZero(innerValue) == TriState::Indeterminate);
        CHECK(stack.isNonZero(outerValue) == TriState::True);
        stack.popLayer();
        CHECK(stack.isNonZero(outerValue) == TriState::Indeterminate);
    }
}

static void testRangeImpliedBy()
{
    for (Type type : { Int32, Int64 }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
        Value* x = type == Int32 ? graph.make(root, Trunc, argument) : argument;
        auto compare = [&](B3::Opcode opcode, int64_t constant) {
            return graph.make(root, opcode, x, type == Int32 ? graph.int32(root, static_cast<int32_t>(constant)) : graph.int64(root, constant));
        };
        IntRange top = IntRange::top(type);
        int64_t low = top.min();
        int64_t high = top.max();
        struct Case {
            Value* comparison;
            IntRange rangeIfTrue;
            IntRange rangeIfFalse;
        };
        const Case cases[] = {
            { compare(Equal, 7), IntRange(7, 7), top },
            { compare(NotEqual, 7), top, IntRange(7, 7) },
            { compare(LessThan, 7), IntRange(low, 6), IntRange(7, high) },
            { compare(LessEqual, 7), IntRange(low, 7), IntRange(8, high) },
            { compare(GreaterThan, 7), IntRange(8, high), IntRange(low, 7) },
            { compare(GreaterEqual, 7), IntRange(7, high), IntRange(low, 6) },
            { compare(LessThan, low), top, top },
            { compare(GreaterThan, high), top, top },
        };
        Value* isLessThanThree = compare(LessThan, 3);
        Value* isLessThanFive = compare(LessThan, 5);
        Value* isLessThanTen = compare(LessThan, 10);
        graph.ret(root, argument);
        resetAndValidate(proc);

        for (const Case& testCase : cases) {
            CHECK(isComparisonWithConstant(testCase.comparison));
            CHECK_EQ(rangeImpliedBy(testCase.comparison, true), testCase.rangeIfTrue);
            CHECK_EQ(rangeImpliedBy(testCase.comparison, false), testCase.rangeIfFalse);
        }
        CHECK(evaluateImpliedComparison(isLessThanTen, isLessThanFive, true) == TriState::True);
        CHECK(evaluateImpliedComparison(isLessThanTen, isLessThanFive, false) == TriState::Indeterminate);
        CHECK(evaluateImpliedComparison(isLessThanThree, isLessThanFive, true) == TriState::Indeterminate);
        CHECK(evaluateImpliedComparison(isLessThanThree, isLessThanFive, false) == TriState::False);
    }
}

static void checkBranchImpliesRelation()
{
    struct Case {
        B3::Opcode opcode;
        bool swapsOperands;
        ImplyingEdge implyingEdge;
    };
    const Case cases[] = {
        { LessThan, false, ImplyingEdge::Taken },
        { GreaterThan, true, ImplyingEdge::Taken },
        { GreaterEqual, false, ImplyingEdge::NotTaken },
        { LessEqual, true, ImplyingEdge::NotTaken },
        { LessEqual, false, ImplyingEdge::None },
        { GreaterEqual, true, ImplyingEdge::None },
        { LessThan, true, ImplyingEdge::None },
        { GreaterThan, false, ImplyingEdge::None },
        { Below, false, ImplyingEdge::None },
        { Above, true, ImplyingEdge::None },
        { Equal, false, ImplyingEdge::None },
        { NotEqual, false, ImplyingEdge::None },
        { BitAnd, false, ImplyingEdge::None },
    };
    for (const Case& testCase : cases) {
        BranchTargets blocks;
        Value* left = testCase.swapsOperands ? blocks.y : blocks.x;
        Value* right = testCase.swapsOperands ? blocks.x : blocks.y;
        blocks.branch(blocks.root, blocks.graph.make(blocks.root, testCase.opcode, left, right));
        auto isImpliedAt = [&](BasicBlock* block) {
            return isRelationImpliedAt(blocks.proc, block, LessThan, blocks.x, blocks.y);
        };
        CHECK_EQ(isImpliedAt(blocks.taken), testCase.implyingEdge == ImplyingEdge::Taken);
        CHECK_EQ(isImpliedAt(blocks.notTaken), testCase.implyingEdge == ImplyingEdge::NotTaken);
        CHECK(!isImpliedAt(blocks.root));
    }
}

static void checkNegatedBranchImpliesRelation()
{
    BranchTargets blocks;
    Graph& graph = blocks.graph;
    Value* isLess = graph.make(blocks.root, LessThan, blocks.x, blocks.y);
    blocks.branch(blocks.root, graph.make(blocks.root, Equal, isLess, graph.int32(blocks.root, 0)));
    CHECK(!isRelationImpliedAt(blocks.proc, blocks.taken, LessThan, blocks.x, blocks.y));
    CHECK(isRelationImpliedAt(blocks.proc, blocks.notTaken, LessThan, blocks.x, blocks.y));
}

static void checkRelationQueries()
{
    BranchTargets blocks;
    Value* x = blocks.x;
    Value* y = blocks.y;
    blocks.branch(blocks.root, blocks.graph.make(blocks.root, LessThan, x, y));
    auto isImplied = [&](B3::Opcode relation, Value* left, Value* right) {
        return isRelationImpliedAt(blocks.proc, blocks.taken, relation, left, right);
    };
    CHECK(isImplied(LessThan, x, y));
    CHECK(isImplied(GreaterThan, y, x));
    CHECK(!isImplied(LessThan, y, x));
    CHECK(!isImplied(GreaterThan, x, y));
    CHECK(!isImplied(LessEqual, x, y));
    CHECK(!isImplied(GreaterEqual, x, y));
    CHECK(!isImplied(Below, x, y));
    CHECK(!isImplied(LessThan, x, x));
}

static void checkRelationOnEquivalentOperands()
{
    {
        BranchTargets blocks;
        Graph& graph = blocks.graph;
        Value* constant = graph.int64(blocks.root, 7);
        Value* equalConstant = graph.int64(blocks.root, 7);
        Value* smallerConstant = graph.int64(blocks.root, 6);
        blocks.branch(blocks.root, graph.make(blocks.root, LessThan, blocks.x, constant));
        CHECK(constant != equalConstant);
        CHECK(isRelationImpliedAt(blocks.proc, blocks.taken, LessThan, blocks.x, equalConstant));
        CHECK(isRelationImpliedAt(blocks.proc, blocks.taken, GreaterThan, equalConstant, blocks.x));
        CHECK(!isRelationImpliedAt(blocks.proc, blocks.taken, LessThan, blocks.x, smallerConstant));
        CHECK(!isRelationImpliedAt(blocks.proc, blocks.notTaken, LessThan, blocks.x, equalConstant));
    }
    {
        BranchTargets blocks;
        Graph& graph = blocks.graph;
        Value* xCopy = graph.make(blocks.root, Identity, blocks.x);
        Value* yCopy = graph.make(blocks.root, Identity, blocks.y);
        blocks.branch(blocks.root, graph.make(blocks.root, LessThan, xCopy, blocks.y));
        CHECK(isRelationImpliedAt(blocks.proc, blocks.taken, LessThan, blocks.x, blocks.y));
        CHECK(isRelationImpliedAt(blocks.proc, blocks.taken, LessThan, xCopy, yCopy));
        CHECK(!isRelationImpliedAt(blocks.proc, blocks.taken, LessThan, yCopy, xCopy));
        CHECK(!isRelationImpliedAt(blocks.proc, blocks.notTaken, LessThan, xCopy, yCopy));
    }
}

static void checkRelationInDominatedBlocks()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* guarded = proc.addBlock();
    BasicBlock* unguarded = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* innerJoin = proc.addBlock();
    BasicBlock* belowInnerJoin = proc.addBlock();
    BasicBlock* outerJoin = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* x = arguments[0];
    Value* y = arguments[1];
    graph.branch(root, graph.make(root, LessThan, x, y), guarded, unguarded);
    graph.branch(guarded, x, left, right);
    graph.jump(left, innerJoin);
    graph.jump(right, innerJoin);
    graph.branch(innerJoin, y, belowInnerJoin, outerJoin);
    graph.jump(belowInnerJoin, outerJoin);
    graph.jump(unguarded, outerJoin);
    graph.ret(outerJoin, x);
    resetAndValidate(proc);

    for (BasicBlock* block : { guarded, left, right, innerJoin, belowInnerJoin })
        CHECK(isRelationImpliedAt(proc, block, LessThan, x, y));
    for (BasicBlock* block : { root, unguarded, outerJoin })
        CHECK(!isRelationImpliedAt(proc, block, LessThan, x, y));
    CHECK_EQ(limitHits(proc, Limit::MaxConditionQueriesPerBlock), 0u);
}

static void checkRelationQueriesStopAtLimit()
{
    constexpr unsigned limit = 2;
    Procedure proc(policyWith([&](Policy& policy) {
        policy.limits.set(Limit::MaxConditionQueriesPerBlock, limit);
    }));
    Graph graph { proc };
    Vector<BasicBlock*> blocks;
    for (unsigned i = 0; i <= limit + 1; ++i)
        blocks.append(proc.addBlock());
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, blocks[0]);
    Value* x = arguments[0];
    Value* y = arguments[1];
    graph.branch(blocks[0], graph.make(blocks[0], LessThan, x, y), blocks[1], exit);
    for (unsigned i = 1; i <= limit; ++i)
        graph.branch(blocks[i], graph.make(blocks[i], BitAnd, x, graph.int64(blocks[i], 1ll << i)), blocks[i + 1], exit);
    graph.jump(blocks[limit + 1], exit);
    graph.ret(exit, x);
    blocks[limit + 1]->at(0)->setOrigin(numberedOrigin(1));
    resetAndValidate(proc);

    CHECK(isRelationImpliedAt(proc, blocks[limit], LessThan, x, y));
    CHECK_EQ(limitHits(proc, Limit::MaxConditionQueriesPerBlock), 0u);
    CHECK(!isRelationImpliedAt(proc, blocks[limit + 1], LessThan, x, y));
    CHECK_EQ(limitHits(proc, Limit::MaxConditionQueriesPerBlock), 1u);
    CHECK(hasNumber(proc.report().firstOriginOf(Limit::MaxConditionQueriesPerBlock), 1));
}

static void testRelationImpliedAt()
{
    checkBranchImpliesRelation();
    checkNegatedBranchImpliesRelation();
    checkRelationQueries();
    checkRelationOnEquivalentOperands();
    checkRelationInDominatedBlocks();
    checkRelationQueriesStopAtLimit();
}

static void testReturnsBoolAnswersOnceForEachValue()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    Value* number = root->appendNew<Value>(proc, Trunc, Origin(), root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0));
    Value* otherNumber = root->appendNew<Value>(proc, Trunc, Origin(), root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1));
    Value* boolean = root->appendNew<Value>(proc, Equal, Origin(), number, otherNumber);
    Value* otherBoolean = root->appendNew<Value>(proc, Below, Origin(), number, otherNumber);

    // As the definition says, worked out from the operands upward.
    Vector<std::pair<Value*, bool>> values {
        { number, false }, { otherNumber, false }, { boolean, true }, { otherBoolean, true },
        { root->appendIntConstant(proc, Origin(), Int32, 0), true }, { root->appendIntConstant(proc, Origin(), Int32, 1), true }, { root->appendIntConstant(proc, Origin(), Int32, 2), false },
    };
    WeakRandom random(4077);
    for (unsigned count = 0; count < 400; ++count) {
        auto [first, firstIsBoolean] = values[random.getUint32(values.size())];
        auto [second, secondIsBoolean] = values[random.getUint32(values.size())];
        switch (random.getUint32(5)) {
        case 0:
            values.append({ root->appendNew<Value>(proc, BitAnd, Origin(), first, second), firstIsBoolean || secondIsBoolean });
            break;
        case 1:
            values.append({ root->appendNew<Value>(proc, BitOr, Origin(), first, second), firstIsBoolean && secondIsBoolean });
            break;
        case 2:
            values.append({ root->appendNew<Value>(proc, BitXor, Origin(), first, second), firstIsBoolean && secondIsBoolean });
            break;
        case 3:
            values.append({ root->appendNew<Value>(proc, Select, Origin(), number, first, second), firstIsBoolean && secondIsBoolean });
            break;
        default:
            values.append({ root->appendNew<Value>(proc, Identity, Origin(), first), firstIsBoolean });
            break;
        }
    }
    for (auto [value, isBoolean] : values)
        CHECK_EQ(value->returnsBool(), isBoolean);

    // Each value is computed from the two in front of it: asked again for every way that leads to a value, neither question ends.
    auto lastOfChain = [&](Value* a, Value* b) {
        for (unsigned turn = 0; turn < 300; ++turn) {
            Value* next = root->appendNew<Value>(proc, BitXor, Origin(), root->appendNew<Value>(proc, BitAnd, Origin(), a, b), b);
            b = a;
            a = next;
        }
        return a;
    };
    CHECK(!lastOfChain(number, otherNumber)->returnsBool());
    CHECK(lastOfChain(boolean, otherBoolean)->returnsBool());
}

void addBunAnalysisTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testConditionAtHead());
    RUN(testConditionChains());
    RUN(testConditionQueriesStopAtLimit());
    RUN(testConditionStackLayers());
    RUN(testRangeImpliedBy());
    RUN(testRelationImpliedAt());
    RUN(testReturnsBoolAnswersOnceForEachValue());
}

#endif // ENABLE(B3_JIT)
