/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3ConvertBranchesToSelects.h"

static bool convertBranchesToSelectsAndValidate(Procedure& proc)
{
    bool result = convertBranchesToSelects(proc);
    validate(proc);
    return result;
}

static Value* elementAtIndex(Graph& graph, TopTestedLoop& loop, BasicBlock* block)
{
    Value* offset = graph.make(block, Shl, graph.get(block, loop.index), graph.int32(block, 3));
    return graph.load(block, Int64, graph.make(block, Add, loop.pointer, offset), 0, firstHeap);
}

static void testConvertBranchesToSelectsConvertsTriangle()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* taken = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Value* loaded = elementAtIndex(graph, loop, loop.body);
    graph.branch(loop.body, graph.make(loop.body, GreaterThan, loaded, loop.extra), taken, join);
    graph.increase(taken, loop.sum, 1);
    graph.jump(taken, join);
    loop.endBody(join);
    checkValueCountInEachLoop(proc, Branch, { 2 });

    CHECK(convertBranchesToSelectsAndValidate(proc));
    checkValueCountInEachLoop(proc, Branch, { 1 });
    checkValueCountInEachLoop(proc, Select, { 1 });
    auto code = compileProc(proc);
    int64_t memory[6] = { 5, -3, 9, 4, 4, 100 };
    CHECK_EQ(run(*code, 6, memory, 4), 3);
    CHECK_EQ(run(*code, 6, memory, 100), 0);
    CHECK_EQ(run(*code, 6, memory, -4), 6);
    CHECK_EQ(run(*code, 1, memory, 4), 1);
    CHECK_EQ(run(*code, 0, memory, 4), 0);
}

// A branch that mostly goes one way is given up for one value, not for more: what its unlikely side computes would be computed every time.
static void testConvertBranchesToSelectsKeepsBranchWithUnlikelySideThatComputes()
{
    for (bool computes : { false, true }) {
        Procedure proc;
        Graph graph { proc };
        TopTestedLoop loop(graph);
        BasicBlock* taken = proc.addBlock();
        BasicBlock* join = proc.addBlock();
        Value* loaded = elementAtIndex(graph, loop, loop.body);
        graph.branch(loop.body, graph.make(loop.body, GreaterThan, loaded, loop.extra), unlikely(taken), FrequentedBlock(join));
        if (computes)
            graph.increase(taken, loop.sum, 1);
        else
            graph.set(taken, loop.sum, loaded);
        graph.jump(taken, join);
        loop.endBody(join);

        CHECK_EQ(convertBranchesToSelectsAndValidate(proc), !computes);
        checkValueCountInEachLoop(proc, Branch, { computes ? 2u : 1u });
        checkValueCountInEachLoop(proc, Select, { computes ? 0u : 1u });
        auto code = compileProc(proc);
        int64_t memory[6] = { 5, -3, 9, 4, 4, 100 };
        CHECK_EQ(run(*code, 6, memory, 4), computes ? 3 : 100);
        CHECK_EQ(run(*code, 5, memory, 4), computes ? 2 : 9);
        CHECK_EQ(run(*code, 6, memory, 100), 0);
    }
}

static void testConvertBranchesToSelectsConvertsDiamond()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* taken = proc.addBlock();
    BasicBlock* notTaken = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Value* loaded = elementAtIndex(graph, loop, loop.body);
    graph.branch(loop.body, graph.make(loop.body, BitAnd, loaded, graph.int64(loop.body, 1)), taken, notTaken);
    graph.increase(taken, loop.sum, loaded);
    graph.jump(taken, join);
    graph.set(notTaken, loop.sum, graph.make(notTaken, Sub, graph.get(notTaken, loop.sum), loop.extra));
    graph.jump(notTaken, join);
    loop.endBody(join);
    checkValueCountInEachLoop(proc, Branch, { 2 });

    CHECK(convertBranchesToSelectsAndValidate(proc));
    checkValueCountInEachLoop(proc, Branch, { 1 });
    checkValueCountInEachLoop(proc, Select, { 1 });
    auto code = compileProc(proc);
    int64_t memory[5] = { 5, 2, -7, 0, 11 };
    CHECK_EQ(run(*code, 5, memory, 10), 5 - 10 - 7 - 10 + 11);
    CHECK_EQ(run(*code, 2, memory, 1), 4);
    CHECK_EQ(run(*code, 0, memory, 1), 0);
}

static void testConvertBranchesToSelectsConvertsNestedTest()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    Variable* length = proc.addVariable(Int64);
    BasicBlock* goesOn = proc.addBlock();
    BasicBlock* isLongest = proc.addBlock();
    BasicBlock* innerJoin = proc.addBlock();
    BasicBlock* ends = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    loop.root->removeLast(proc);
    graph.set(loop.root, length, graph.int64(loop.root, 0));
    graph.jump(loop.root, loop.header);
    Value* loaded = elementAtIndex(graph, loop, loop.body);
    graph.branch(loop.body, graph.make(loop.body, GreaterEqual, loaded, graph.int64(loop.body, 0)), goesOn, ends);
    graph.increase(goesOn, length, 1);
    graph.branch(goesOn, graph.make(goesOn, GreaterThan, graph.get(goesOn, length), graph.get(goesOn, loop.sum)), isLongest, innerJoin);
    graph.set(isLongest, loop.sum, graph.get(isLongest, length));
    graph.jump(isLongest, innerJoin);
    graph.jump(innerJoin, join);
    graph.set(ends, length, graph.int64(ends, 0));
    graph.jump(ends, join);
    loop.endBody(join);
    checkValueCountInEachLoop(proc, Branch, { 3 });

    CHECK(convertBranchesToSelectsAndValidate(proc));
    checkValueCountInEachLoop(proc, Branch, { 1 });
    auto code = compileProc(proc);
    int64_t memory[10] = { 1, 2, -1, 3, 4, 5, 0, -2, 6, 7 };
    CHECK_EQ(run(*code, 10, memory), 4);
    CHECK_EQ(run(*code, 3, memory), 2);
    CHECK_EQ(run(*code, 0, memory), 0);
    int64_t negative[3] = { -1, -2, -3 };
    CHECK_EQ(run(*code, 3, negative), 0);
}

static void checkReachedLimit(Procedure& proc, std::optional<Limit> reachedLimit)
{
    for (Limit limit : { Limit::MaxSelectNestingDepth, Limit::MaxSpeculatedValuesPerSelect, Limit::MaxSelectsPerJoin, Limit::MaxValuesSearchedForDominatingLoad })
        CHECK_EQ(!!limitHits(proc, limit), reachedLimit == limit);
}

template<typename ConditionFunctor, typename ArmFunctor>
static bool convertsTriangle(const ConditionFunctor& makeCondition, const ArmFunctor& fillArm, FrequencyClass frequencyOfArm = FrequencyClass::Normal, std::optional<Limit> reachedLimit = std::nullopt)
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* taken = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Value* loaded = elementAtIndex(graph, loop, loop.body);
    Value* condition = makeCondition(graph, loop, loaded);
    condition->setOrigin(numberedOrigin(1));
    graph.branch(loop.body, condition, FrequentedBlock(taken, frequencyOfArm), FrequentedBlock(join));
    fillArm(graph, loop, taken, loaded);
    graph.jump(taken, join);
    loop.endBody(join);
    bool result = convertBranchesToSelectsAndValidate(proc);
    checkReachedLimit(proc, reachedLimit);
    CHECK_EQ(hasNumber(proc.report().firstOriginOf(Limit::MaxValuesSearchedForDominatingLoad), 1), reachedLimit == Limit::MaxValuesSearchedForDominatingLoad);
    return result;
}

static void testConvertBranchesToSelectsKeepsOtherBranches()
{
    auto onLoadedValue = [](Graph& graph, TopTestedLoop& loop, Value* loaded) {
        return graph.make(loop.body, GreaterThan, loaded, loop.extra);
    };
    auto onIndex = [](Graph& graph, TopTestedLoop& loop, Value*) {
        return graph.make(loop.body, BitAnd, graph.get(loop.body, loop.index), graph.int64(loop.body, 1));
    };
    auto onValueFromOutsideLoop = [](Graph& graph, TopTestedLoop& loop, Value*) {
        return graph.make(loop.body, GreaterThan, loop.extra, loop.bound);
    };
    auto counts = [](Graph& graph, TopTestedLoop& loop, BasicBlock* arm, Value*) {
        graph.increase(arm, loop.sum, 1);
    };
    CHECK(convertsTriangle(onLoadedValue, counts));
    CHECK(!convertsTriangle(onIndex, counts));
    CHECK(!convertsTriangle(onValueFromOutsideLoop, counts));
    CHECK(!convertsTriangle(onLoadedValue, counts, FrequencyClass::Rare));
    CHECK(!convertsTriangle(onLoadedValue, [](Graph& graph, TopTestedLoop& loop, BasicBlock* arm, Value* loaded) {
        graph.increase(arm, loop.sum, 1);
        graph.store(arm, loaded, loop.pointer, 0, secondHeap);
    }));
    CHECK(!convertsTriangle(onLoadedValue, [](Graph& graph, TopTestedLoop& loop, BasicBlock* arm, Value*) {
        graph.increase(arm, loop.sum, graph.load(arm, Int64, loop.pointer, 0, firstHeap));
    }));
    CHECK(!convertsTriangle(onLoadedValue, [](Graph& graph, TopTestedLoop& loop, BasicBlock* arm, Value*) {
        graph.increase(arm, loop.sum, 1);
        graph.callLikePatchpoint(arm);
    }));
    for (bool onlyKeepsChildrenAlive : { false, true }) {
        CHECK_EQ(convertsTriangle(onLoadedValue, [&](Graph& graph, TopTestedLoop& loop, BasicBlock* arm, Value*) {
            graph.increase(arm, loop.sum, 1);
            PatchpointValue* patchpoint = graph.emptyPatchpoint(arm);
            patchpoint->effects.writesLocalState = true;
            patchpoint->effects.reads = HeapRange::top();
            patchpoint->append(loop.pointer, ValueRep::ColdAny);
            patchpoint->onlyKeepsChildrenAlive = onlyKeepsChildrenAlive;
        }), onlyKeepsChildrenAlive);
    }
    CHECK(!convertsTriangle(onLoadedValue, [](Graph& graph, TopTestedLoop& loop, BasicBlock* arm, Value* loaded) {
        graph.set(arm, loop.sum, graph.make(arm, Div, graph.get(arm, loop.sum), loaded));
    }));
    CHECK(!convertsTriangle(onLoadedValue, [](Graph& graph, TopTestedLoop& loop, BasicBlock* arm, Value* loaded) {
        Value* value = graph.get(arm, loop.sum);
        for (unsigned i = 0; i < 3; ++i)
            value = graph.make(arm, BitXor, graph.make(arm, Add, value, loop.extra), loaded);
        graph.set(arm, loop.sum, value);
    }, FrequencyClass::Normal, Limit::MaxSpeculatedValuesPerSelect));
}

static void testConvertBranchesToSelectsKeepsBranchOutsideLoop()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* taken = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Variable* result = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
    Value* loaded = graph.load(root, Int64, arguments[1], 0, firstHeap);
    graph.set(root, result, graph.int64(root, 0));
    graph.branch(root, graph.make(root, GreaterThan, loaded, arguments[2]), taken, join);
    graph.increase(taken, result, 1);
    graph.jump(taken, join);
    graph.ret(join, graph.get(join, result));
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
    CHECK(!convertBranchesToSelectsAndValidate(proc));
}

enum class ChosenValues : uint8_t {
    Compared,
    ComparedInOtherOrder,
    ComparedWithEqualConstant,
    FirstAndThird,
};

template<typename T>
static bool convertsChoice(B3::Opcode comparison, ChosenValues chosen, bool isDiamond)
{
    constexpr bool isNumber = std::is_floating_point_v<T>;
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* taken = proc.addBlock();
    BasicBlock* notTaken = isDiamond ? proc.addBlock() : nullptr;
    BasicBlock* join = proc.addBlock();
    auto arguments = cCallArgumentValues<T, T, T>(proc, root);
    auto seven = [&](BasicBlock* block) -> Value* {
        if constexpr (isNumber)
            return graph.number(block, 7);
        else
            return graph.int64(block, 7);
    };
    Value* a = arguments[0];
    Value* b = chosen == ChosenValues::ComparedWithEqualConstant ? seven(root) : arguments[1];
    Value* c = arguments[2];
    Value* ifTrue = chosen == ChosenValues::ComparedInOtherOrder ? b : a;
    Value* ifFalse = chosen == ChosenValues::ComparedInOtherOrder ? a : chosen == ChosenValues::FirstAndThird ? c : b;
    if (chosen == ChosenValues::ComparedWithEqualConstant)
        ifFalse = seven(root);
    Value* phi = proc.add<Value>(Phi, a->type(), Origin());
    if (!isDiamond)
        graph.upsilon(root, ifFalse, phi);
    graph.branch(root, graph.make(root, comparison, a, b), taken, isDiamond ? notTaken : join);
    graph.upsilon(taken, ifTrue, phi);
    graph.jump(taken, join);
    if (isDiamond) {
        graph.upsilon(notTaken, ifFalse, phi);
        graph.jump(notTaken, join);
    }
    join->append(phi);
    graph.ret(join, phi);
    proc.resetReachability();
    validate(proc);

    bool isConverted = convertBranchesToSelectsAndValidate(proc);
    CHECK_EQ(numberOfValues(proc, Branch), isConverted ? 0u : 1u);
    CHECK_EQ(numberOfValues(proc, Select), isConverted ? 1u : 0u);
    auto code = compileProc(proc);
    Vector<T> operands { static_cast<T>(-3), static_cast<T>(0), static_cast<T>(7), static_cast<T>(9) };
    if constexpr (isNumber) {
        operands.append(-0.0);
        operands.append(PNaN);
    }
    for (T x : operands) {
        for (T y : operands) {
            T right = chosen == ChosenValues::ComparedWithEqualConstant ? static_cast<T>(7) : y;
            bool isTrue = false;
            switch (comparison) {
            case LessThan:
                isTrue = x < right;
                break;
            case GreaterThan:
                isTrue = x > right;
                break;
            case LessEqual:
                isTrue = x <= right;
                break;
            case GreaterEqual:
                isTrue = x >= right;
                break;
            case Below:
                isTrue = static_cast<uint64_t>(x) < static_cast<uint64_t>(right);
                break;
            default:
                isTrue = x == right;
                break;
            }
            T third = static_cast<T>(5);
            T whenTrue = chosen == ChosenValues::ComparedInOtherOrder ? right : x;
            T whenFalse = chosen == ChosenValues::ComparedInOtherOrder ? x : chosen == ChosenValues::FirstAndThird ? third : right;
            T expected = isTrue ? whenTrue : whenFalse;
            T result = invoke<T>(*code, x, y, third);
            CHECK(!memcmp(&result, &expected, sizeof(T)));
        }
    }
    return isConverted;
}

// The smaller or the larger of two values needs no loop around it and no load in front of it.
static void testConvertBranchesToSelectsConvertsChoiceBetweenComparedValues()
{
    for (bool isDiamond : { false, true }) {
        for (ChosenValues chosen : { ChosenValues::Compared, ChosenValues::ComparedInOtherOrder, ChosenValues::ComparedWithEqualConstant }) {
            for (B3::Opcode comparison : { LessThan, GreaterThan, LessEqual, GreaterEqual, Below })
                CHECK(convertsChoice<int64_t>(comparison, chosen, isDiamond));
            for (B3::Opcode comparison : { LessThan, GreaterThan })
                CHECK(convertsChoice<double>(comparison, chosen, isDiamond));
            for (B3::Opcode comparison : { LessEqual, GreaterEqual })
                CHECK_EQ(convertsChoice<double>(comparison, chosen, isDiamond), isARM64());
            CHECK(!convertsChoice<int64_t>(Equal, chosen, isDiamond));
            CHECK(!convertsChoice<double>(Equal, chosen, isDiamond));
        }
        CHECK(!convertsChoice<int64_t>(LessThan, ChosenValues::FirstAndThird, isDiamond));
        CHECK(!convertsChoice<double>(LessThan, ChosenValues::FirstAndThird, isDiamond));
    }
}

static void checkSelectsAtJoin(unsigned variableCount, std::optional<Limit> reachedLimit)
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* taken = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Vector<Variable*> written;
    for (unsigned i = 0; i < variableCount; ++i)
        written.append(proc.addVariable(Int64));
    for (Variable* variable : written)
        graph.set(loop.body, variable, graph.int64(loop.body, 0));
    Value* loaded = elementAtIndex(graph, loop, loop.body);
    graph.branch(loop.body, graph.make(loop.body, GreaterThan, loaded, loop.extra), taken, join);
    for (Variable* variable : written)
        graph.set(taken, variable, loop.extra);
    graph.jump(taken, join);
    for (Variable* variable : written)
        graph.increase(join, loop.sum, graph.get(join, variable));
    loop.endBody(join);
    checkValueCountInEachLoop(proc, Branch, { 2 });

    bool isConverted = !reachedLimit;
    CHECK_EQ(convertBranchesToSelectsAndValidate(proc), isConverted);
    checkReachedLimit(proc, reachedLimit);
    checkValueCountInEachLoop(proc, Branch, { isConverted ? 1u : 2u });
    checkValueCountInEachLoop(proc, Select, { isConverted ? variableCount : 0u });
    auto code = compileProc(proc);
    int64_t memory[3] = { 5, -3, 9 };
    CHECK_EQ(run(*code, 3, memory, 4), static_cast<int64_t>(2 * variableCount * 4));
}

static void checkNestedTests(unsigned nestingDepth, std::optional<Limit> reachedLimit)
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    Vector<BasicBlock*> heads { loop.body };
    Vector<BasicBlock*> joins { proc.addBlock() };
    for (unsigned level = 0; level < nestingDepth; ++level) {
        heads.append(proc.addBlock());
        joins.append(proc.addBlock());
    }
    BasicBlock* innermostArm = proc.addBlock();
    Vector<Value*> conditions { graph.make(loop.body, GreaterThan, elementAtIndex(graph, loop, loop.body), loop.extra) };
    for (unsigned level = 0; level < nestingDepth; ++level)
        conditions.append(graph.make(loop.body, BitAnd, graph.get(loop.body, loop.index), graph.int64(loop.body, 1ll << level)));
    for (unsigned level = 0; level <= nestingDepth; ++level)
        graph.branch(heads[level], conditions[level], level < nestingDepth ? heads[level + 1] : innermostArm, joins[level]);
    graph.increase(innermostArm, loop.sum, 1);
    graph.jump(innermostArm, joins.last());
    for (unsigned level = nestingDepth; level; --level)
        graph.jump(joins[level], joins[level - 1]);
    loop.endBody(joins.first());
    checkValueCountInEachLoop(proc, Branch, { nestingDepth + 2 });

    bool isConverted = !reachedLimit;
    CHECK_EQ(convertBranchesToSelectsAndValidate(proc), isConverted);
    checkReachedLimit(proc, reachedLimit);
    checkValueCountInEachLoop(proc, Branch, { isConverted ? 1u : nestingDepth + 2 });
    checkValueCountInEachLoop(proc, Select, { isConverted ? nestingDepth + 1 : 0u });
    auto code = compileProc(proc);
    int64_t memory[8] = { 9, 0, 9, 9, 0, 9, 9, 0 };
    int64_t testedBits = (1ll << nestingDepth) - 1;
    for (int64_t threshold : { 4, 9 }) {
        for (int64_t bound : { 0, 4, 8 }) {
            int64_t expected = 0;
            for (int64_t index = 0; index < bound; ++index)
                expected += memory[index] > threshold && (index & testedBits) == testedBits;
            CHECK_EQ(run(*code, bound, memory, threshold), expected);
        }
    }
}

static void testConvertBranchesToSelectsCountsLimits()
{
    constexpr unsigned maxSpeculatedValues = defaultValueOf(Limit::MaxSpeculatedValuesPerSelect);
    constexpr unsigned maxSearchedValues = defaultValueOf(Limit::MaxValuesSearchedForDominatingLoad);
    constexpr unsigned maxSelects = defaultValueOf(Limit::MaxSelectsPerJoin);
    constexpr unsigned maxNestingDepth = defaultValueOf(Limit::MaxSelectNestingDepth);

    auto onLoadedValue = [](Graph& graph, TopTestedLoop& loop, Value* loaded) {
        return graph.make(loop.body, GreaterThan, loaded, loop.extra);
    };
    auto counts = [](Graph& graph, TopTestedLoop& loop, BasicBlock* arm, Value*) {
        graph.increase(arm, loop.sum, 1);
    };

    auto addsExtra = [](unsigned additionCount) {
        return [additionCount](Graph& graph, TopTestedLoop& loop, BasicBlock* arm, Value*) {
            Value* value = graph.get(arm, loop.sum);
            for (unsigned i = 0; i < additionCount; ++i)
                value = graph.make(arm, Add, value, loop.extra);
            graph.set(arm, loop.sum, value);
        };
    };
    CHECK(convertsTriangle(onLoadedValue, addsExtra(maxSpeculatedValues)));
    CHECK(!convertsTriangle(onLoadedValue, addsExtra(maxSpeculatedValues + 1), FrequencyClass::Normal, Limit::MaxSpeculatedValuesPerSelect));

    auto onAdditionChain = [](unsigned searchedValueCount) {
        return [searchedValueCount](Graph& graph, TopTestedLoop& loop, Value* loaded) {
            constexpr unsigned searchedValuesOutsideChain = 3;
            Value* chain = loaded;
            for (unsigned i = searchedValuesOutsideChain; i < searchedValueCount; ++i)
                chain = graph.make(loop.body, Add, chain, loop.extra);
            return graph.make(loop.body, GreaterThan, chain, loop.extra);
        };
    };
    CHECK(convertsTriangle(onAdditionChain(maxSearchedValues), counts));
    CHECK(!convertsTriangle(onAdditionChain(maxSearchedValues + 1), counts, FrequencyClass::Normal, Limit::MaxValuesSearchedForDominatingLoad));

    checkSelectsAtJoin(maxSelects, std::nullopt);
    checkSelectsAtJoin(maxSelects + 1, Limit::MaxSelectsPerJoin);

    checkNestedTests(maxNestingDepth, std::nullopt);
    checkNestedTests(maxNestingDepth + 1, Limit::MaxSelectNestingDepth);
}

void addBunSelectTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testConvertBranchesToSelectsConvertsTriangle());
    RUN(testConvertBranchesToSelectsKeepsBranchWithUnlikelySideThatComputes());
    RUN(testConvertBranchesToSelectsConvertsDiamond());
    RUN(testConvertBranchesToSelectsConvertsNestedTest());
    RUN(testConvertBranchesToSelectsKeepsOtherBranches());
    RUN(testConvertBranchesToSelectsKeepsBranchOutsideLoop());
    RUN(testConvertBranchesToSelectsConvertsChoiceBetweenComparedValues());
    RUN(testConvertBranchesToSelectsCountsLimits());
}

#endif // ENABLE(B3_JIT)
