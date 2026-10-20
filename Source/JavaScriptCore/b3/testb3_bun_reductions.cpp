/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3BulkMemoryValue.h"
#include "B3EliminateDeadCode.h"
#include "B3FoldPathConstants.h"
#include "B3IntRange.h"
#include "B3OptimizeAssociativeExpressionTrees.h"
#include "B3ThreadJumps.h"

static constexpr int64_t tag = static_cast<int64_t>(0xfffe000000000000ull);

static Value* intConstant(Graph& graph, BasicBlock* block, Type type, int64_t value)
{
    return type == Int32 ? graph.int32(block, static_cast<int32_t>(value)) : graph.int64(block, value);
}

static Value* appendBoxedWord(Graph& graph, BasicBlock* block, Value* operand, B3::Opcode extension, int64_t addend)
{
    return graph.make(block, Add, graph.make(block, extension, graph.make(block, Trunc, operand)), graph.int64(block, addend));
}

static Policy jumpThreadingPolicy()
{
    return policyWith([](Policy& policy) {
        policy.upstream.maxTailDuplicationBlockSize = 1;
    });
}

namespace {

struct PhiInputs {
    int64_t fromLeft;
    int64_t fromRight;
    int64_t constant;
};

} // anonymous namespace

static void testMemoryNumberingIrreducibleCycle()
{
    for (bool hasTwoEntries : { false, true }) {
        TwoLoads loads;
        Graph& graph = loads.graph;
        BasicBlock* reload = loads.proc.addBlock();
        BasicBlock* latch = loads.proc.addBlock();
        BasicBlock* exit = loads.proc.addBlock();
        if (hasTwoEntries)
            graph.branch(loads.root, loads.operand, latch, reload);
        else
            graph.jump(loads.root, reload);
        graph.store(reload, loads.load(reload), loads.otherPointer, 0, secondHeap);
        graph.branch(reload, loads.operand, latch, exit);
        graph.store(latch, loads.operand, loads.otherPointer, 8, secondHeap);
        graph.jump(latch, reload);
        graph.ret(exit, loads.first);
        CHECK_EQ(loads.reduceAndCountLoads(), hasTwoEntries ? 2u : 1u);
    }
}

static void testMemoryNumberingExpandedCopy()
{
    constexpr unsigned expandedCopyLoadCount = 1;
    for (bool overwritesLoadedHeap : { true, false }) {
        TwoLoads loads;
        BulkMemoryValue* copy = loads.root->appendNew<BulkMemoryValue>(loads.proc, MemoryCopy, Origin(), loads.otherPointer, loads.otherPointer, loads.graph.int64(loads.root, 8));
        copy->setReadRange(HeapRange(70, 71));
        copy->setWriteRange(overwritesLoadedHeap ? firstHeap : HeapRange(71, 72));
        loads.returnBothLoads(loads.root);
        CHECK_EQ(loads.reduceAndCountLoads(), expandedCopyLoadCount + (overwritesLoadedHeap ? 2u : 1u));
        CHECK_EQ(numberOfValues(loads.proc, MemoryCopy), 0u);
        int64_t memory[1] = { 3 };
        int64_t otherMemory[1] = { 4 };
        CHECK_EQ(loads.run(memory, otherMemory, 0), 303);
        CHECK_EQ(otherMemory[0], 4);
    }
}

static void checkFoldedConversionWritesNothing(bool writesLoadedHeap, bool isFolded)
{
    TwoLoads loads;
    Graph& graph = loads.graph;
    BasicBlock* root = loads.root;
    Value* number = graph.make(root, IToD, graph.make(root, Trunc, loads.operand));
    auto appendConversion = [&](uint64_t id) {
        PatchpointValue* conversion = graph.emptyPatchpoint(root, Int32);
        conversion->pureFunction = PureFunction { id, true };
        if (writesLoadedHeap)
            conversion->effects.writes = firstHeap;
        conversion->append(number, ValueRep::SomeRegister);
        conversion->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.truncateDoubleToInt32(params[1].fpr(), params[0].gpr());
        });
        return graph.make(root, ZExt32, conversion);
    };
    Value* converted = appendConversion(9);
    Value* loadedBetweenConversions = loads.load(root);
    Value* convertedAgain = appendConversion(isFolded ? 9 : 10);
    Value* scaledLoad = graph.make(root, Mul, loadedBetweenConversions, graph.int64(root, 10000));
    loads.returnBothLoads(root, graph.make(root, Add, graph.make(root, Add, converted, convertedAgain), scaledLoad));

    unsigned remainingConversionCount = isFolded ? 1 : 2;
    CHECK_EQ(loads.reduceAndCountLoads(), writesLoadedHeap ? 1 + remainingConversionCount : 1u);
    CHECK_EQ(numberOfValues(loads.proc, Patchpoint), remainingConversionCount);
    int64_t memory[1] = { 3 };
    CHECK_EQ(loads.run(memory, memory, 5), 30303 + 2 * 5);
    CHECK_EQ(loads.run(memory, memory, -1), 30303 + 2 * 0xffffffffll);
}

static void testMemoryNumberingFoldedConversionWritesNothing()
{
    for (bool writesLoadedHeap : { false, true }) {
        for (bool isFolded : { true, false })
            checkFoldedConversionWritesNothing(writesLoadedHeap, isFolded);
    }
}

static auto conditionWithConstant(B3::Opcode opcode, Type constantType, int64_t constant)
{
    return [=](Graph& graph, BasicBlock* block, Value* phi) {
        return graph.make(block, opcode, phi, intConstant(graph, block, constantType, constant));
    };
}

template<typename Functor>
static void appendPhiTest(Graph& graph, TailWithTwoPredecessors& blocks, Type type, const PhiInputs& inputs, const Functor& appendCondition)
{
    Value* phi = graph.phi(blocks.tail, type);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, intConstant(graph, blocks.left, type, inputs.fromLeft), phi);
    graph.upsilon(blocks.right, intConstant(graph, blocks.right, type, inputs.fromRight), phi);
    graph.branch(blocks.tail, appendCondition(graph, blocks.tail, phi), blocks.taken, blocks.notTaken);
}

template<typename Functor>
static void checkThreadsLikeCompiledCode(Type type, const PhiInputs& inputs, const Functor& appendCondition)
{
    int64_t unthreadedResults[2] = { 0, 0 };
    for (bool threads : { false, true }) {
        Procedure proc(jumpThreadingPolicy());
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        appendPhiTest(graph, blocks, type, inputs, appendCondition);
        if (threads) {
            blocks.thread(graph);
            BasicBlock* const predecessors[2] = { blocks.left, blocks.right };
            for (unsigned i = 0; i < 2; ++i) {
                CHECK(predecessors[i]->last()->opcode() == Jump);
                CHECK(predecessors[i]->successorBlock(0) == (unthreadedResults[i] == 10 ? blocks.taken : blocks.notTaken));
            }
        } else
            blocks.joinPaths(graph);
        auto code = compileProc(proc);
        int64_t memory[1];
        for (unsigned i = 0; i < 2; ++i) {
            int64_t result = invoke<int64_t>(*code, static_cast<int64_t>(!i), static_cast<int64_t>(0), memory);
            CHECK(result == 10 || result == 20);
            if (threads)
                CHECK_EQ(result, unthreadedResults[i]);
            else
                unthreadedResults[i] = result;
        }
    }
}

static Vector<PhiInputs> phiInputsFor(Type type)
{
    IntRange top = IntRange::top(type);
    return { { 0, 1, 1 }, { -1, 5, 33 }, { top.min(), top.max(), 65 }, { 7, -7, -1 } };
}

static void testThreadJumpsFoldsLikeReduceStrength()
{
    for (Type type : { Int32, Int64 }) {
        for (B3::Opcode opcode : { Add, Sub, BitAnd, BitOr, BitXor, Shl, SShr, ZShr, Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual }) {
            bool isShift = opcode == Shl || opcode == SShr || opcode == ZShr;
            for (const PhiInputs& inputs : phiInputsFor(type))
                checkThreadsLikeCompiledCode(type, inputs, conditionWithConstant(opcode, isShift ? Type(Int32) : type, inputs.constant));
        }
    }

    struct Conversion {
        B3::Opcode opcode;
        Type operandType;
        Type resultType;
    };
    for (Conversion conversion : { Conversion { Trunc, Int64, Int32 }, Conversion { SExt32, Int32, Int64 }, Conversion { ZExt32, Int32, Int64 }, Conversion { SExt8, Int32, Int32 }, Conversion { SExt16, Int32, Int32 } }) {
        Vector<PhiInputs> allInputs = phiInputsFor(conversion.operandType);
        allInputs.append(PhiInputs { -1, 0xff, -1 });
        for (const PhiInputs& inputs : allInputs) {
            checkThreadsLikeCompiledCode(conversion.operandType, inputs, [&](Graph& graph, BasicBlock* block, Value* phi) {
                return graph.make(block, Equal, graph.make(block, conversion.opcode, phi), intConstant(graph, block, conversion.resultType, inputs.constant));
            });
        }
    }
}

template<typename AppendInput, typename ComputeInput>
static void checkThreadsByRange(B3::Opcode relation, int64_t bound, TriState outcome, const AppendInput& appendInput, const ComputeInput& computeInput)
{
    Procedure proc(jumpThreadingPolicy());
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    Value* phi = graph.phi(blocks.tail, Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, appendInput(graph, blocks.left, blocks.first), phi);
    graph.upsilon(blocks.right, blocks.second, phi);
    graph.branch(blocks.tail, graph.make(blocks.tail, relation, phi, graph.int64(blocks.tail, bound)), blocks.taken, blocks.notTaken);
    blocks.thread(graph);
    BasicBlock* expectedSuccessor = blocks.tail;
    if (outcome != TriState::Indeterminate)
        expectedSuccessor = outcome == TriState::True ? blocks.taken : blocks.notTaken;
    CHECK(blocks.left->successorBlock(0) == expectedSuccessor);
    CHECK(blocks.right->successorBlock(0) == blocks.tail);

    auto code = compileProc(proc);
    int64_t memory[1];
    auto run = [&](int64_t first, int64_t second) {
        return invoke<int64_t>(*code, first, second, memory);
    };
    auto expectedResult = [&](int64_t input) {
        return holds(relation, input, bound) ? 10 : 20;
    };
    for (int64_t first : { 1, 2, 3, 4, 6, 7, -1 })
        CHECK_EQ(run(first, 0), expectedResult(computeInput(first)));
    const int64_t secondOperands[] = { bound - 1, bound, bound + 1, 5, -5 };
    for (int64_t second : secondOperands)
        CHECK_EQ(run(0, second), expectedResult(second));
}

static void testThreadJumpsDecidesByRange()
{
    for (B3::Opcode extension : { ZExt32, SExt32 }) {
        checkThreadsByRange(AboveEqual, tag, extension == ZExt32 ? TriState::True : TriState::Indeterminate, [&](Graph& graph, BasicBlock* block, Value* operand) {
            return appendBoxedWord(graph, block, operand, extension, tag);
        }, [&](int64_t operand) {
            return tag + (extension == ZExt32 ? static_cast<int64_t>(static_cast<uint32_t>(operand)) : static_cast<int64_t>(static_cast<int32_t>(operand)));
        });
    }

    constexpr int64_t addend = -4;
    constexpr int64_t bound = 2;
    for (B3::Opcode relation : { Above, BelowEqual, LessThan, GreaterEqual }) {
        for (bool isAlwaysNegative : { true, false }) {
            int64_t mask = isAlwaysNegative ? 3 : 7;
            TriState outcome = isAlwaysNegative ? triState(holds(relation, -1, bound)) : TriState::Indeterminate;
            checkThreadsByRange(relation, bound, outcome, [&](Graph& graph, BasicBlock* block, Value* operand) {
                return graph.make(block, Add, graph.make(block, BitAnd, operand, graph.int64(block, mask)), graph.int64(block, addend));
            }, [&](int64_t operand) {
                return (operand & mask) + addend;
            });
        }
    }
}

static void testThreadJumpsUsesConditionAcrossJoin()
{
    Procedure proc(jumpThreadingPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* region = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    BasicBlock* other = proc.addBlock();
    BasicBlock* tail = proc.addBlock();
    BasicBlock* taken = proc.addBlock();
    BasicBlock* notTaken = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(proc, root);
    Value* x = arguments[0];
    Value* y = arguments[1];
    Value* pointer = arguments[2];
    auto isFive = [&](BasicBlock* block) {
        return graph.make(block, Equal, x, graph.int64(block, 5));
    };
    graph.branch(root, isFive(root), region, other);
    graph.branch(region, y, left, right);
    graph.store(left, x, pointer, 0, firstHeap);
    graph.jump(left, join);
    graph.store(right, y, pointer, 0, firstHeap);
    graph.jump(right, join);
    PatchpointValue* uncloneable = join->appendNew<PatchpointValue>(proc, Void, Origin(), cloningForbidden(Patchpoint));
    uncloneable->effects = Effects::none();
    uncloneable->effects.writesLocalState = true;
    uncloneable->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
    graph.jump(join, tail);
    graph.store(other, x, pointer, 0, firstHeap);
    graph.jump(other, tail);
    graph.branch(tail, isFive(tail), taken, notTaken);
    graph.ret(taken, graph.int64(taken, 10));
    graph.ret(notTaken, graph.int64(notTaken, 20));
    resetAndValidate(proc);
    threadJumps(proc);
    fixSSA(proc);
    validate(proc);
    CHECK(left->successorBlock(0) == join);
    CHECK(right->successorBlock(0) == join);
    CHECK(join->successorBlock(0) == taken);
    CHECK(other->successorBlock(0) == notTaken);

    auto code = compileProc(proc);
    int64_t memory[1];
    auto run = [&](int64_t xValue, int64_t yValue) {
        return invoke<int64_t>(*code, xValue, yValue, memory);
    };
    CHECK_EQ(run(5, 1), 10);
    CHECK_EQ(run(5, 0), 10);
    CHECK_EQ(run(4, 1), 20);
}

static void testThreadJumpsUsesImpliedComparison()
{
    for (int64_t bound : { 10, 3 }) {
        Procedure proc(jumpThreadingPolicy());
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        graph.branch(blocks.root, graph.make(blocks.root, LessThan, blocks.first, graph.int64(blocks.root, 5)), blocks.left, blocks.right);
        graph.branch(blocks.tail, graph.make(blocks.tail, LessThan, blocks.first, graph.int64(blocks.tail, bound)), blocks.taken, blocks.notTaken);
        blocks.thread(graph);
        CHECK(blocks.left->successorBlock(0) == (bound == 10 ? blocks.taken : blocks.tail));
        CHECK(blocks.right->successorBlock(0) == (bound == 10 ? blocks.tail : blocks.notTaken));

        auto code = compileProc(proc);
        int64_t memory[1];
        for (int64_t first : { -1, 2, 4, 7, 12 })
            CHECK_EQ(invoke<int64_t>(*code, first, static_cast<int64_t>(0), memory), first < bound ? 10 : 20);
    }
}

static void testThreadJumpsIgnoresConditionOfEarlierIteration()
{
    Procedure proc(jumpThreadingPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* latch = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t>(proc, root);
    Value* pointer = arguments[0];
    Value* second = arguments[1];
    graph.jump(root, header);
    Value* loaded = graph.load(header, Int64, pointer, 0, firstHeap);
    graph.branch(header, loaded, body, exit);
    graph.store(body, graph.int64(body, 0), pointer, 0, firstHeap);
    graph.branch(body, second, left, right);
    graph.jump(left, latch);
    graph.jump(right, latch);
    graph.jump(latch, header);
    graph.ret(exit, graph.int64(exit, 30));
    resetAndValidate(proc);
    threadJumps(proc);
    fixSSA(proc);
    validate(proc);
    CHECK_EQ(numberOfValues(proc, Load), 1u);
    CHECK(header->last()->opcode() == Branch);
    CHECK(header->last()->child(0) == loaded);
    CHECK(header->predecessors().containsIf([&](BasicBlock* predecessor) {
        return predecessor != root;
    }));

    auto code = compileProc(proc);
    for (int64_t secondValue : { 0, 1 }) {
        int64_t memory[1] = { 7 };
        CHECK_EQ(invoke<int64_t>(*code, memory, secondValue), 30);
        CHECK_EQ(memory[0], 0);
    }
}

static void testThreadJumpsLeavesNoTemporaries()
{
    Procedure proc(jumpThreadingPolicy());
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    appendPhiTest(graph, blocks, Int64, PhiInputs { -1, 5, 33 }, conditionWithConstant(Add, Int64, 33));
    blocks.joinPaths(graph);
    CHECK(threadJumps(proc));

    IndexSet<Value*> valuesInBlocks;
    for (BasicBlock* block : proc) {
        for (Value* value : *block)
            valuesInBlocks.add(value);
    }
    for (Value* value : proc.values())
        CHECK(valuesInBlocks.contains(value));
}

static void testTruncOfSumWithZeroLowHalf()
{
    for (int64_t addend : { tag, tag | 1 }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        Value* operand = cCallArgumentValues<int64_t>(proc, root)[0];
        graph.ret(root, graph.make(root, ZExt32, graph.make(root, Trunc, appendBoxedWord(graph, root, operand, ZExt32, addend))));
        resetAndValidate(proc);
        reduceStrengthOnce(proc);
        CHECK_EQ(numberOfValues(proc, Add), addend == tag ? 0u : 1u);

        auto code = compileProc(proc);
        const int64_t operands[] = { 0, -1, 1ll << 40 | 3 };
        for (int64_t value : operands)
            CHECK_EQ(invoke<int64_t>(*code, value), static_cast<int64_t>(static_cast<uint32_t>(value) + static_cast<uint32_t>(addend)));
    }
}

static int32_t lowWordOf(B3::Opcode opcode, int64_t left, int64_t right)
{
    uint64_t a = static_cast<uint64_t>(left);
    uint64_t b = static_cast<uint64_t>(right);
    switch (opcode) {
    case Add:
        return static_cast<int32_t>(a + b);
    case Sub:
        return static_cast<int32_t>(a - b);
    case Mul:
        return static_cast<int32_t>(a * b);
    case BitAnd:
        return static_cast<int32_t>(a & b);
    case BitOr:
        return static_cast<int32_t>(a | b);
    case BitXor:
        return static_cast<int32_t>(a ^ b);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static constexpr int32_t wordsToCombine[] = { 0, 1, -1, 7, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(), 0x12345678 };

static unsigned numberOfWideValues(Procedure& proc, B3::Opcode opcode)
{
    return numberOfValues(proc, [&](Value* value) {
        return value->opcode() == opcode && value->type() == Int64;
    });
}

static void testLowWordOfWideOperationIsNarrowOperation()
{
    for (B3::Opcode opcode : { Add, Sub, Mul, BitAnd, BitOr, BitXor }) {
        for (B3::Opcode extension : { SExt32, ZExt32 }) {
            Procedure proc(strengthReductionPolicy());
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            auto words = cCallArgumentValues<int32_t, int32_t, int32_t>(proc, root);
            auto combine = [&](Value* left, Value* right) {
                return graph.make(root, Trunc, graph.make(root, opcode, graph.make(root, extension, left), graph.make(root, extension, right)));
            };
            graph.ret(root, combine(combine(words[0], words[1]), words[2]));
            resetAndValidate(proc);
            reduceStrengthOnce(proc);
            CHECK_EQ(numberOfWideValues(proc, opcode), 0u);
            CHECK_EQ(numberOfValues(proc, opcode), 2u);
            CHECK_EQ(numberOfValues(proc, extension), 0u);

            auto code = compileProc(proc);
            for (int32_t a : wordsToCombine) {
                for (int32_t b : wordsToCombine)
                    CHECK_EQ(invoke<int32_t>(*code, a, b, 5), lowWordOf(opcode, lowWordOf(opcode, a, b), 5));
            }
        }
    }
}

static void testLowWordOfNestedWideOperations()
{
    Procedure proc(strengthReductionPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto operands = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root);
    Value* product = graph.make(root, Mul, graph.make(root, Add, operands[0], operands[1]), graph.make(root, BitXor, operands[2], graph.int64(root, 0x100000003ll)));
    graph.ret(root, graph.make(root, Trunc, product));
    resetAndValidate(proc);
    while (reduceStrengthOnce(proc)) { }
    for (B3::Opcode opcode : { Add, Mul, BitXor })
        CHECK_EQ(numberOfWideValues(proc, opcode), 0u);

    auto code = compileProc(proc);
    CHECK_EQ(invoke<int32_t>(*code, 1ll << 40 | 6, 1ll << 33 | 1, 1ll << 50 | 4), 7 * (4 ^ 3));
}

// The comparison of a sum with its own low word is how a client asks whether the sum of two words overflows.
static void testWideOperationWithWideUserStaysWide()
{
    for (bool hasWideUser : { true, false }) {
        Procedure proc(strengthReductionPolicy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto words = cCallArgumentValues<int32_t, int32_t>(proc, root);
        Value* sum = graph.make(root, Add, graph.make(root, SExt32, words[0]), graph.make(root, SExt32, words[1]));
        Value* low = graph.make(root, Trunc, sum);
        Value* fits = hasWideUser ? graph.make(root, Equal, graph.make(root, SExt32, low), sum) : graph.int32(root, 1);
        graph.ret(root, graph.select(root, fits, low, graph.int32(root, 99)));
        resetAndValidate(proc);
        reduceStrengthOnce(proc);
        CHECK_EQ(numberOfWideValues(proc, Add), hasWideUser ? 1u : 0u);
        CHECK_EQ(numberOfValues(proc, Add), 1u);

        auto code = compileProc(proc);
        constexpr int32_t maxWord = std::numeric_limits<int32_t>::max();
        CHECK_EQ(invoke<int32_t>(*code, 3, 4), 7);
        CHECK_EQ(invoke<int32_t>(*code, maxWord, 1), hasWideUser ? 99 : std::numeric_limits<int32_t>::min());
    }
}

static void testLowWordOfWideShiftAndDivisionStayWide()
{
    for (B3::Opcode opcode : { Shl, SShr, ZShr, Div }) {
        Procedure proc(strengthReductionPolicy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto operands = cCallArgumentValues<int64_t, int64_t>(proc, root);
        Value* right = opcode == Div ? operands[1] : graph.make(root, Trunc, operands[1]);
        graph.ret(root, graph.make(root, Trunc, graph.make(root, opcode, operands[0], right)));
        resetAndValidate(proc);
        reduceStrengthOnce(proc);
        CHECK_EQ(numberOfWideValues(proc, opcode), 1u);
    }
}

static Policy associativeOperationPolicy(bool chainsAssociativeOperations)
{
    return policyWith([&](Policy& policy) {
        policy.optimization.chainsAssociativeOperations = chainsAssociativeOperations;
    });
}

static unsigned numberOfJoinedSubtrees(Procedure& proc, B3::Opcode opcode)
{
    return numberOfValues(proc, [&](Value* value) {
        return value->opcode() == opcode && value->child(0)->opcode() == opcode && value->child(1)->opcode() == opcode;
    });
}

static void testChainOfAssociativeOperationsStaysChain()
{
    constexpr unsigned wordCount = 9;
    for (B3::Opcode opcode : { Add, BitXor, BitOr }) {
        for (bool chainsAssociativeOperations : { true, false }) {
            Procedure proc(associativeOperationPolicy(chainsAssociativeOperations));
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            Value* pointer = cCallArgumentValues<int32_t*>(proc, root)[0];
            Value* result = graph.load(root, Int32, pointer, 0, firstHeap);
            for (unsigned i = 1; i < wordCount; ++i)
                result = graph.make(root, opcode, result, graph.load(root, Int32, pointer, i * sizeof(int32_t), firstHeap));
            graph.ret(root, result);
            resetAndValidate(proc);
            CHECK_EQ(optimizeAssociativeExpressionTrees(proc), !chainsAssociativeOperations);
            eliminateDeadCode(proc);
            validate(proc);
            CHECK_EQ(numberOfValues(proc, opcode), wordCount - 1);
            CHECK_EQ(!numberOfJoinedSubtrees(proc, opcode), chainsAssociativeOperations);

            int32_t words[wordCount] = { 1, 2, 4, 8, 16, 32, 64, 128, 256 };
            CHECK_EQ(invoke<int32_t>(*compileProc(proc), words), 511);
        }
    }
}

static void testChainOfAssociativeOperationsCombinesLeaves()
{
    for (bool chainsAssociativeOperations : { true, false }) {
        Procedure proc(associativeOperationPolicy(chainsAssociativeOperations));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto words = cCallArgumentValues<int32_t, int32_t, int32_t, int32_t, int32_t>(proc, root);
        Value* repeated = words[0];
        Value* result = graph.make(root, BitXor, repeated, graph.int32(root, 1));
        for (Value* operand : { words[1], graph.int32(root, 2), repeated, words[2], words[3], words[4] })
            result = graph.make(root, BitXor, result, operand);
        graph.ret(root, result);
        resetAndValidate(proc);
        CHECK(optimizeAssociativeExpressionTrees(proc));
        eliminateDeadCode(proc);
        validate(proc);
        CHECK_EQ(numberOfValues(proc, BitXor), 4u);
        CHECK_EQ(!numberOfJoinedSubtrees(proc, BitXor), chainsAssociativeOperations);
        CHECK_EQ(invoke<int32_t>(*compileProc(proc), 1000, 16, 32, 64, 128), 16 ^ 32 ^ 64 ^ 128 ^ 3);
    }
}

static void checkSignComparison(B3::Opcode outer, B3::Opcode leftTest, B3::Opcode rightTest, Type leftType, Type rightType)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    auto appendSignTest = [&](B3::Opcode opcode, Value* argument, Type type) {
        Value* operand = type == Int32 ? graph.make(root, Trunc, argument) : argument;
        return graph.make(root, opcode, operand, intConstant(graph, root, type, 0));
    };
    Value* left = appendSignTest(leftTest, arguments[0], leftType);
    Value* right = appendSignTest(rightTest, arguments[1], rightType);
    graph.ret(root, graph.make(root, ZExt32, graph.make(root, outer, left, right)));
    resetAndValidate(proc);
    reduceStrengthOnce(proc);
    if (leftType == rightType) {
        CHECK_EQ(numberOfValues(proc, BitXor), 1u);
        CHECK_EQ(numberOfValues(proc, [](Value* value) {
            return value->opcode() == Equal || value->opcode() == NotEqual || value->opcode() == LessThan || value->opcode() == GreaterEqual;
        }), 1u);
    } else
        CHECK_EQ(numberOfValues(proc, BitXor), 0u);

    auto code = compileProc(proc);
    for (int64_t leftValue : { -3, 0, 4 }) {
        for (int64_t rightValue : { -3, 0, 4 }) {
            bool areEqual = holds(leftTest, leftValue, 0) == holds(rightTest, rightValue, 0);
            CHECK_EQ(invoke<int64_t>(*code, leftValue, rightValue), static_cast<int64_t>(areEqual == (outer == Equal)));
        }
    }
}

static void testSignComparisonBecomesExclusiveOr()
{
    for (B3::Opcode outer : { Equal, NotEqual }) {
        for (B3::Opcode leftTest : { LessThan, GreaterEqual }) {
            for (B3::Opcode rightTest : { LessThan, GreaterEqual }) {
                checkSignComparison(outer, leftTest, rightTest, Int32, Int32);
                checkSignComparison(outer, leftTest, rightTest, Int64, Int64);
                checkSignComparison(outer, leftTest, rightTest, Int32, Int64);
            }
        }
    }
}

static void testBranchOnKnownConditionAtLevelOne()
{
    for (bool usesPolicy : { true, false }) {
        Procedure proc(usesPolicy ? strengthReductionPolicy() : Policy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* taken = proc.addBlock();
        BasicBlock* notTaken = proc.addBlock();
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
        returnOneOf(taken, 1, 2);
        returnOneOf(notTaken, 3, 4);
        resetAndValidate(proc);
        proc.setOptLevel(1);
        reduceStrength(proc, ReduceStrengthPass::Initial);
        validate(proc);
        CHECK_EQ(numberOfValues(proc, Branch), usesPolicy ? 1u : 3u);

        auto code = compileProc(proc);
        int64_t memory[2] = { 0, 0 };
        CHECK_EQ(invoke<int64_t>(*code, 5, memory), 1);
        CHECK_EQ(invoke<int64_t>(*code, 6, memory), 4);
        CHECK_EQ(memory[0], 5);
        CHECK_EQ(memory[1], 6);
    }
}

static Policy constantReplacementPolicy(bool foldsImpliedComparisons)
{
    return policyWith([&](Policy& policy) {
        policy.optimization.foldsImpliedComparisons = foldsImpliedComparisons;
    });
}

namespace {

struct ComparisonOfReplacedConstant {
    B3::Opcode comparison;
    int64_t replaced;
    int64_t replacement;
    int64_t constant;
    bool needsSelect;
};

} // anonymous namespace

static void checkComparisonOfReplacedConstant(const ComparisonOfReplacedConstant& test, Type type, B3::Opcode condition, bool usesPolicy)
{
    Procedure proc(constantReplacementPolicy(usesPolicy));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* operand = type == Int32 ? graph.make(root, Trunc, argument) : argument;
    Value* replacement = intConstant(graph, root, type, test.replacement);
    Value* isReplaced = graph.make(root, condition, operand, intConstant(graph, root, type, test.replaced));
    Value* select = condition == Equal ? graph.select(root, isReplaced, replacement, operand) : graph.select(root, isReplaced, operand, replacement);
    graph.ret(root, graph.make(root, ZExt32, graph.make(root, test.comparison, select, intConstant(graph, root, type, test.constant))));
    resetAndValidate(proc);
    reduceStrengthOnce(proc);
    eliminateDeadCode(proc);
    CHECK_EQ(numberOfValues(proc, Select), test.needsSelect || !usesPolicy ? 1u : 0u);

    auto code = compileProc(proc);
    for (int64_t value : { test.replaced, test.replacement, test.constant, test.constant - 1, test.constant + 1, static_cast<int64_t>(0), static_cast<int64_t>(-1), static_cast<int64_t>(1000) }) {
        int64_t selected = value == test.replaced ? test.replacement : value;
        CHECK_EQ(invoke<int64_t>(*code, value), static_cast<int64_t>(holds(test.comparison, selected, test.constant)));
    }
}

static void testComparisonLooksThroughConstantReplacement()
{
    static constexpr ComparisonOfReplacedConstant tests[] = {
        { AboveEqual, 0, 10, 16, false },
        { AboveEqual, 0, 10, 5, true },
        { Below, 0, 10, 16, false },
        { Below, 0, 10, 10, true },
        { LessThan, 7, -3, 100, false },
        { LessThan, 7, -3, 0, true },
        { GreaterEqual, -9, -8, -7, false },
        { Above, -1, 3, 2, false },
        { Above, -1, 1, 2, true },
        { Equal, 0, 10, 16, false },
        { NotEqual, 0, 10, 16, false },
        { Equal, 0, 10, 10, true },
        { NotEqual, 0, 10, 10, true },
        { Equal, 0, 10, 0, false },
        { NotEqual, 0, 10, 0, false },
        { Equal, 4, 10, 10, true },
        { NotEqual, 4, 10, 4, false },
    };
    for (const ComparisonOfReplacedConstant& test : tests) {
        for (Type type : { Int32, Int64 }) {
            for (B3::Opcode condition : { Equal, NotEqual }) {
                for (bool usesPolicy : { true, false })
                    checkComparisonOfReplacedConstant(test, type, condition, usesPolicy);
            }
        }
    }
}

static void checkComparisonOfReplacedNaN(double replacement, bool takesNumber, bool comparesBits, bool needsSelect)
{
    constexpr uint64_t replacedBits = 0x7ffc000000000000ull;
    constexpr uint64_t firstMarkerBits = 0x7ffa000000000000ull;

    Procedure proc(constantReplacementPolicy(true));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* argument = takesNumber ? cCallArgumentValues<double>(proc, root)[0] : cCallArgumentValues<int64_t>(proc, root)[0];
    Value* cast = graph.make(root, BitwiseCast, argument);
    Value* number = takesNumber ? argument : cast;
    Value* isReplaced = graph.make(root, Equal, takesNumber ? cast : argument, graph.int64(root, static_cast<int64_t>(replacedBits)));
    Value* select = graph.select(root, isReplaced, graph.number(root, replacement), number);
    Value* comparison = comparesBits
        ? graph.make(root, GreaterEqual, graph.make(root, BitwiseCast, select), graph.int64(root, static_cast<int64_t>(firstMarkerBits)))
        : graph.make(root, EqualOrUnordered, select, graph.number(root, 0));
    graph.ret(root, graph.make(root, ZExt32, comparison));
    resetAndValidate(proc);
    reduceStrengthOnce(proc);
    eliminateDeadCode(proc);
    CHECK_EQ(numberOfValues(proc, Select), needsSelect ? 1u : 0u);
    if (comparesBits && !needsSelect)
        CHECK_EQ(numberOfValues(proc, BitwiseCast), takesNumber ? 1u : 0u);

    auto code = compileProc(proc);
    for (uint64_t bits : { replacedBits, firstMarkerBits, std::bit_cast<uint64_t>(replacement), std::bit_cast<uint64_t>(0.0), std::bit_cast<uint64_t>(-0.0), std::bit_cast<uint64_t>(1.5), std::bit_cast<uint64_t>(-2.0) }) {
        uint64_t selectedBits = bits == replacedBits ? std::bit_cast<uint64_t>(replacement) : bits;
        double selected = std::bit_cast<double>(selectedBits);
        bool expected = comparesBits ? static_cast<int64_t>(selectedBits) >= static_cast<int64_t>(firstMarkerBits) : selected != selected || !selected;
        int64_t result = takesNumber ? invoke<int64_t>(*code, std::bit_cast<double>(bits)) : invoke<int64_t>(*code, static_cast<int64_t>(bits));
        CHECK_EQ(result, static_cast<int64_t>(expected));
    }
}

static void testComparisonLooksThroughReplacementOfNaN()
{
    double otherNaN = std::bit_cast<double>(0x7ffa000000000000ull);
    for (bool takesNumber : { false, true }) {
        for (bool comparesBits : { false, true }) {
            checkComparisonOfReplacedNaN(otherNaN, takesNumber, comparesBits, false);
            checkComparisonOfReplacedNaN(1.5, takesNumber, comparesBits, true);
        }
    }
}

static void checkReplacementOfExcludedConstant(bool excludesReplaced, bool returnsBits)
{
    constexpr uint64_t replacedBits = 0x7ffc000000000000ull;
    constexpr uint64_t replacementBits = 0x7ffa000000000000ull;
    int64_t bound = static_cast<int64_t>(excludesReplaced ? replacementBits : replacedBits + 1);

    Procedure proc(constantReplacementPolicy(true));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* atLeastBound = proc.addBlock();
    BasicBlock* belowBound = proc.addBlock();
    Value* number = cCallArgumentValues<double>(proc, root)[0];
    Value* isReplaced = graph.make(root, Equal, graph.make(root, BitwiseCast, number), graph.int64(root, static_cast<int64_t>(replacedBits)));
    Value* select = graph.select(root, isReplaced, graph.number(root, std::bit_cast<double>(replacementBits)), number);
    Value* bitsOfSelect = graph.make(root, BitwiseCast, select);
    graph.branch(root, graph.make(root, GreaterEqual, bitsOfSelect, graph.int64(root, bound)), atLeastBound, belowBound);
    graph.ret(atLeastBound, graph.int64(atLeastBound, 0));
    graph.ret(belowBound, returnsBits ? bitsOfSelect : graph.make(belowBound, BitwiseCast, graph.make(belowBound, Add, select, graph.number(belowBound, 0))));
    resetAndValidate(proc);
    reduceStrengthOnce(proc);
    foldPathConstants(proc);
    eliminateDeadCode(proc);
    validate(proc);
    CHECK_EQ(numberOfValues(proc, Select), excludesReplaced ? 0u : 1u);

    auto code = compileProc(proc);
    for (uint64_t bits : { replacedBits, replacementBits, replacedBits + 1, std::bit_cast<uint64_t>(1.0), std::bit_cast<uint64_t>(1.5), std::bit_cast<uint64_t>(-2.0) }) {
        uint64_t selectedBits = bits == replacedBits ? replacementBits : bits;
        uint64_t expectedBits = static_cast<int64_t>(selectedBits) >= bound ? 0 : selectedBits;
        CHECK_EQ(static_cast<uint64_t>(invoke<int64_t>(*code, std::bit_cast<double>(bits))), expectedBits);
    }
}

static void testReplacementOfExcludedConstantFoldsOnPath()
{
    for (bool excludesReplaced : { true, false }) {
        for (bool returnsBits : { true, false })
            checkReplacementOfExcludedConstant(excludesReplaced, returnsBits);
    }
}

void addBunReductionTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testMemoryNumberingIrreducibleCycle());
    RUN(testMemoryNumberingExpandedCopy());
    RUN(testMemoryNumberingFoldedConversionWritesNothing());
    RUN(testThreadJumpsFoldsLikeReduceStrength());
    RUN(testThreadJumpsDecidesByRange());
    RUN(testThreadJumpsUsesConditionAcrossJoin());
    RUN(testThreadJumpsUsesImpliedComparison());
    RUN(testThreadJumpsIgnoresConditionOfEarlierIteration());
    RUN(testThreadJumpsLeavesNoTemporaries());
    RUN(testTruncOfSumWithZeroLowHalf());
    RUN(testLowWordOfWideOperationIsNarrowOperation());
    RUN(testLowWordOfNestedWideOperations());
    RUN(testWideOperationWithWideUserStaysWide());
    RUN(testLowWordOfWideShiftAndDivisionStayWide());
    RUN(testChainOfAssociativeOperationsStaysChain());
    RUN(testChainOfAssociativeOperationsCombinesLeaves());
    RUN(testSignComparisonBecomesExclusiveOr());
    RUN(testBranchOnKnownConditionAtLevelOne());
    RUN(testComparisonLooksThroughConstantReplacement());
    RUN(testComparisonLooksThroughReplacementOfNaN());
    RUN(testReplacementOfExcludedConstantFoldsOnPath());
}

#endif // ENABLE(B3_JIT)
