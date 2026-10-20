/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3_bun_selection.h"

#if ENABLE(B3_JIT)

#include "B3CompareChains.h"
#include <limits>

struct CompareChainGraph {
    explicit CompareChainGraph(const Policy& policy)
        : proc(policy)
        , root(proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t, double>(proc, root);
        x = arguments[0];
        y = arguments[1];
        number = arguments[2];
    }

    Value* make(B3::Opcode opcode, Value* left, Value* right) { return graph.make(root, opcode, left, right); }
    Value* compareX(B3::Opcode relation, int64_t bound) { return make(relation, x, graph.int64(root, bound)); }
    Value* compareY(B3::Opcode relation, int64_t bound) { return make(relation, y, graph.int64(root, bound)); }
    Value* compareNumber(B3::Opcode relation, double bound) { return make(relation, number, graph.number(root, bound)); }
    Value* compareWords(B3::Opcode relation) { return make(relation, word(x), word(y)); }
    Value* word(Value* wide) { return graph.make(root, Trunc, wide); }
    Value* negate(Value* value) { return make(Equal, value, graph.int32(root, 0)); }
    Value* maskedWord(Value* wide, int32_t mask) { return make(BitAnd, word(wide), graph.int32(root, mask)); }
    void addUser(Value* value) { graph.make(root, ZExt32, value); }

    Value* joinComparisons(unsigned numberOfLogicOps)
    {
        Value* result = make(LessThan, x, y);
        for (unsigned i = 0; i < numberOfLogicOps; ++i)
            result = make(BitAnd, result, compareX(NotEqual, i));
        return result;
    }

    Value* joinNegatedChains(unsigned numberOfLogicOps)
    {
        Value* result = make(BitOr, make(LessThan, x, y), compareY(Equal, 7));
        for (unsigned i = 1; i < numberOfLogicOps; ++i)
            result = make(BitAnd, negate(result), compareX(NotEqual, i));
        return result;
    }

    void branchOn(Value* condition)
    {
        BasicBlock* taken = proc.addBlock();
        BasicBlock* notTaken = proc.addBlock();
        graph.branch(root, condition, taken, notTaken);
        graph.ret(taken, graph.int64(taken, 1));
        graph.ret(notTaken, graph.int64(notTaken, 0));
    }

    void returnIsNonZero(Value* condition)
    {
        Value* zero = condition->type() == Int64 ? graph.int64(root, 0) : graph.int32(root, 0);
        graph.ret(root, graph.make(root, ZExt32, make(NotEqual, condition, zero)));
    }

    Procedure proc;
    Graph graph { proc };
    BasicBlock* root;
    Value* x;
    Value* y;
    Value* number;
};

template<typename Functor>
static void checkBranchEqualsValue(const Policy& policy, const Functor& buildCondition)
{
    CompareChainGraph branching(policy);
    branching.branchOn(buildCondition(branching));
    auto branchingCode = compileProc(branching.proc);

    CompareChainGraph returning(policy);
    returning.returnIsNonZero(buildCondition(returning));
    auto returningCode = compileProc(returning.proc);

    constexpr int64_t integers[] = { -1, 0, 1, 2, 3, 6, 7, 8 };
    constexpr double numbers[] = { 0.5, 2, 3, std::numeric_limits<double>::quiet_NaN() };
    for (int64_t x : integers) {
        for (int64_t y : integers) {
            for (double number : numbers)
                CHECK_EQ(invoke<int64_t>(*branchingCode, x, y, number), invoke<int64_t>(*returningCode, x, y, number));
        }
    }
}

inline bool isPredictedAsCompareChain(Procedure& proc, Value* condition)
{
    UseCounts useCounts(proc);
    return isSelectedAsCompareChain(proc, condition, [&](Value* value) {
        return useCounts.numUses(value) == 1;
    });
}

template<typename Functor>
static void checkCompareChain(const Policy& policy, bool isSelectedOnARM64, const Functor& buildCondition)
{
    CompareChainGraph test(policy);
    Value* condition = buildCondition(test);
    test.branchOn(condition);

    bool isPredicted = isPredictedAsCompareChain(test.proc, condition);
    CHECK_EQ(isPredicted, isARM64() && isSelectedOnARM64);

    lowerToAirForTesting(test.proc);
    CHECK_EQ(numberOfInsts(test.proc, Air::BranchOnFlags), isPredicted ? 1u : 0u);

    checkBranchEqualsValue(policy, buildCondition);
}

template<typename Functor>
static void checkIsCompareChain(const Functor& buildCondition)
{
    checkCompareChain(Policy::fromProcessOptions(), true, buildCondition);
}

template<typename Functor>
static void checkIsNotCompareChain(const Functor& buildCondition)
{
    checkCompareChain(Policy::fromProcessOptions(), false, buildCondition);
}

static void testCompareChainsMatchSelection()
{
    for (B3::Opcode logicOp : { BitAnd, BitOr }) {
        checkIsCompareChain([&](CompareChainGraph& test) {
            return test.make(logicOp, test.make(LessThan, test.x, test.y), test.compareX(GreaterEqual, 2));
        });
        checkIsCompareChain([&](CompareChainGraph& test) {
            return test.make(logicOp, test.compareWords(Below), test.compareNumber(LessThan, 1));
        });
        checkIsCompareChain([&](CompareChainGraph& test) {
            return test.make(logicOp, test.compareNumber(GreaterEqual, 1), test.compareNumber(NotEqual, 2));
        });
        for (bool chainIsLeft : { true, false }) {
            checkIsCompareChain([&](CompareChainGraph& test) {
                Value* chain = test.make(BitOr, test.make(LessThan, test.x, test.y), test.compareX(Equal, 7));
                Value* comparison = test.compareY(NotEqual, 2);
                return chainIsLeft ? test.make(logicOp, chain, comparison) : test.make(logicOp, comparison, chain);
            });
        }
    }
    checkIsCompareChain([](CompareChainGraph& test) {
        return test.joinComparisons(Limits::maxCompareChainSize);
    });
    checkIsCompareChain([](CompareChainGraph& test) {
        Value* isBitSet = test.make(NotEqual, test.maskedWord(test.x, 2), test.graph.int32(test.root, 0));
        return test.make(BitAnd, isBitSet, test.compareY(LessThan, 5));
    });

    checkIsNotCompareChain([](CompareChainGraph& test) {
        return test.joinComparisons(Limits::maxCompareChainSize + 1);
    });
    checkIsNotCompareChain([](CompareChainGraph& test) {
        Value* first = test.make(BitAnd, test.make(LessThan, test.x, test.y), test.compareY(LessThan, 5));
        Value* second = test.make(BitAnd, test.make(LessThan, test.y, test.x), test.compareX(LessThan, 5));
        return test.make(BitOr, first, second);
    });
    checkIsNotCompareChain([](CompareChainGraph& test) {
        return test.make(LessThan, test.x, test.y);
    });
    checkIsNotCompareChain([](CompareChainGraph& test) {
        return test.make(BitAnd, test.x, test.y);
    });
    checkIsNotCompareChain([](CompareChainGraph& test) {
        return test.make(BitAnd, test.make(LessThan, test.x, test.y), test.word(test.y));
    });
    checkIsNotCompareChain([](CompareChainGraph& test) {
        return test.make(BitAnd, test.make(LessThan, test.x, test.y), test.graph.int32(test.root, 1));
    });
    checkIsNotCompareChain([](CompareChainGraph& test) {
        Value* inverted = test.make(BitXor, test.compareX(Equal, 7), test.graph.int32(test.root, 1));
        return test.make(BitAnd, test.make(LessThan, test.x, test.y), inverted);
    });
}

static void testNegatedCompareChainsMatchSelection()
{
    checkIsCompareChain([](CompareChainGraph& test) {
        return test.make(BitAnd, test.negate(test.make(LessThan, test.x, test.y)), test.compareX(NotEqual, 2));
    });
    checkIsCompareChain([](CompareChainGraph& test) {
        return test.make(BitAnd, test.negate(test.compareNumber(LessThan, 1)), test.compareX(NotEqual, 2));
    });
    checkIsCompareChain([](CompareChainGraph& test) {
        return test.make(BitOr, test.negate(test.negate(test.make(LessThan, test.x, test.y))), test.compareX(Equal, 2));
    });
    checkIsCompareChain([](CompareChainGraph& test) {
        return test.joinNegatedChains(Limits::maxCompareChainSize);
    });
    checkIsCompareChain([](CompareChainGraph& test) {
        return test.make(BitAnd, test.negate(test.maskedWord(test.x, 6)), test.compareY(NotEqual, 2));
    });
    checkIsCompareChain([](CompareChainGraph& test) {
        Value* result = test.negate(test.maskedWord(test.x, 6));
        for (unsigned i = 0; i < Limits::maxCompareChainSize; ++i)
            result = test.make(BitOr, result, test.negate(test.maskedWord(test.y, 3 << i)));
        return result;
    });

    checkIsNotCompareChain([](CompareChainGraph& test) {
        return test.joinNegatedChains(Limits::maxCompareChainSize + 1);
    });
    checkIsNotCompareChain([](CompareChainGraph& test) {
        Value* first = test.make(BitAnd, test.make(LessThan, test.x, test.y), test.compareY(LessThan, 5));
        Value* second = test.make(BitAnd, test.make(LessThan, test.y, test.x), test.compareX(LessThan, 5));
        return test.make(BitOr, test.negate(first), test.negate(second));
    });
    checkIsNotCompareChain([](CompareChainGraph& test) {
        Value* first = test.make(BitOr, test.make(LessThan, test.x, test.y), test.compareY(LessThan, 5));
        Value* second = test.make(BitOr, test.make(LessThan, test.y, test.x), test.compareX(LessThan, 5));
        return test.make(BitAnd, test.negate(first), second);
    });
    checkIsNotCompareChain([](CompareChainGraph& test) {
        return test.negate(test.make(BitAnd, test.make(LessThan, test.x, test.y), test.compareX(NotEqual, 2)));
    });
}

static void testCompareChainSearchLimit()
{
    constexpr Limit limit = Limit::MaxCompareChainSearchSize;
    constexpr uint64_t loweredLimit = 3;
    static_assert(loweredLimit < defaultValueOf(limit));

    for (uint64_t searchLimit : { defaultValueOf(limit), loweredLimit }) {
        Policy policy = Policy::fromProcessOptions();
        policy.limits.set(limit, searchLimit);
        for (bool exceedsLimit : { false, true }) {
            auto joinWithMaskedWord = [&](CompareChainGraph& test) {
                Value* masked = test.word(test.x);
                for (uint64_t i = 0; i < searchLimit - 1 + exceedsLimit; ++i) {
                    masked = test.make(BitAnd, masked, test.word(test.y));
                    masked->setOrigin(numberedOrigin(i + 1));
                }
                return test.make(BitAnd, test.negate(masked), test.compareY(NotEqual, 2));
            };
            checkCompareChain(policy, !exceedsLimit, joinWithMaskedWord);

            CompareChainGraph test(policy);
            bool reachesLimit = isARM64() && exceedsLimit;
            CHECK_EQ(isPredictedAsCompareChain(test.proc, joinWithMaskedWord(test)), isARM64() && !exceedsLimit);
            CHECK_EQ(limitHits(test.proc, limit), reachesLimit ? 1u : 0u);
            CHECK_EQ(hasNumber(test.proc.report().firstOriginOf(limit), 1), reachesLimit);
        }
    }
}

static void testCompareChainsWithSharedValues()
{
    checkIsNotCompareChain([](CompareChainGraph& test) {
        Value* shared = test.make(LessThan, test.x, test.y);
        test.addUser(shared);
        return test.make(BitAnd, shared, test.compareX(NotEqual, 2));
    });
    checkIsNotCompareChain([](CompareChainGraph& test) {
        Value* shared = test.make(BitOr, test.make(LessThan, test.x, test.y), test.compareX(Equal, 7));
        test.addUser(shared);
        return test.make(BitAnd, shared, test.compareY(NotEqual, 2));
    });
    checkIsCompareChain([](CompareChainGraph& test) {
        Value* shared = test.make(BitOr, test.make(LessThan, test.x, test.y), test.compareX(Equal, 7));
        test.addUser(shared);
        return test.make(BitAnd, test.negate(shared), test.compareY(NotEqual, 2));
    });
    checkIsCompareChain([](CompareChainGraph& test) {
        Value* shared = test.joinComparisons(Limits::maxCompareChainSize);
        test.addUser(shared);
        return test.make(BitAnd, test.negate(shared), test.compareY(NotEqual, 2));
    });
}

inline void addBunCompareChainTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testCompareChainsMatchSelection());
    RUN(testNegatedCompareChainsMatchSelection());
    RUN(testCompareChainSearchLimit());
    RUN(testCompareChainsWithSharedValues());
}

#endif // ENABLE(B3_JIT)
