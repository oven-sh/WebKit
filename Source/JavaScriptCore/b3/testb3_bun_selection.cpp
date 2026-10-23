/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_selection.h"
#include "testb3_bun_compare_chains.h"

#if ENABLE(B3_JIT)

#include "AirBlockEntryClobberSpecial.h"
#include "AirEliminateDeadCode.h"
#include "AirGenerate.h"
#include "AirValidate.h"
#include "B3ArgumentRegValue.h"
#include "B3CheckValue.h"
#include "B3ExtractValue.h"
#include "B3Generate.h"
#include "B3ImmediateForms.h"
#include "B3LowerToAir.h"
#include "B3PatchpointValue.h"
#include "B3UseCounts.h"
#include <wtf/HashSet.h>

static unsigned numberOfDeadMoves(Procedure& proc)
{
    UncheckedKeyHashSet<Air::Tmp> usedTmps;
    for (Air::BasicBlock* block : proc.code()) {
        for (Air::Inst& inst : *block) {
            inst.forEachTmp([&](Air::Tmp& tmp, Air::Arg::Role role, Bank, Width) {
                if (Air::Arg::isAnyUse(role))
                    usedTmps.add(tmp);
            });
        }
    }
    return numberOfInsts(proc, [&](Air::Inst& inst) {
        if (inst.kind.opcode != Air::Move && inst.kind.opcode != Air::Move32 && inst.kind.opcode != Air::MoveDouble)
            return false;
        const Air::Arg& destination = inst.args()[1];
        return destination.isTmp() && !destination.tmp().isReg() && !usedTmps.contains(destination.tmp());
    });
}

static void testUseCountsDuplicateChild()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root);
    Value* first = arguments[0];
    Value* second = arguments[1];
    Value* third = arguments[2];
    Value* sum = graph.make(root, Add, first, first);
    Value* difference = graph.make(root, Sub, sum, second);
    graph.ret(root, graph.select(root, second, second, difference));

    UseCounts useCounts(proc);
    CHECK_EQ(useCounts.numUses(first), 2u);
    CHECK_EQ(useCounts.numUsingInstructions(first), 1u);
    CHECK_EQ(useCounts.numUses(second), 3u);
    CHECK_EQ(useCounts.numUsingInstructions(second), 2u);
    CHECK_EQ(useCounts.numUses(third), 0u);
    CHECK_EQ(useCounts.numUses(sum), 1u);
    CHECK_EQ(useCounts.numUsingInstructions(difference), 1u);
}

template<typename Functor>
static unsigned instructionCount(const Policy& policy, bool extends, const Functor& makeWord)
{
    Procedure proc(policy);
    proc.setOptLevel(2);
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t*, int64_t>(proc, root);
    Value* pointer = arguments[0];
    Value* wide = arguments[1];
    Value* first = graph.load(root, Int32, pointer, 0, firstHeap);
    Value* second = graph.load(root, Int32, pointer, 4, firstHeap);
    Value* word = makeWord(graph, root, first, second, wide);
    graph.ret(root, extends ? graph.make(root, ZExt32, word) : word);
    lowerToAirForTesting(proc);
    if (extends) {
        CHECK_EQ(numberOfInsts(proc, [](Air::Inst& inst) {
            return inst.kind.opcode == Air::Move32 && inst.args()[0].isTmp() && inst.args()[1].isTmp();
        }), 1u);
    }
    Air::prepareForGeneration(proc.code());
    return numberOfInsts(proc);
}

static void testRedundantZeroExtension()
{
    auto binary = [](B3::Opcode opcode) {
        return [opcode](Graph& graph, BasicBlock* block, Value* first, Value* second, Value*) {
            return graph.make(block, opcode, first, second);
        };
    };
    auto shift = [](B3::Opcode opcode, int32_t amount) {
        return [opcode, amount](Graph& graph, BasicBlock* block, Value* first, Value*, Value*) {
            return graph.make(block, opcode, first, graph.int32(block, amount));
        };
    };
    auto truncation = [](Graph& graph, BasicBlock* block, Value*, Value*, Value* wide) {
        return graph.make(block, Trunc, graph.make(block, Add, wide, wide));
    };
    for (const Policy& policy : { Policy(), selectionPolicy() }) {
        for (B3::Opcode opcode : { Add, Sub, Mul, BitAnd, BitOr, BitXor, Equal, LessThan, Below })
            CHECK_EQ(instructionCount(policy, true, binary(opcode)), instructionCount(policy, false, binary(opcode)));
        CHECK_EQ(instructionCount(policy, true, shift(Shl, 3)), instructionCount(policy, false, shift(Shl, 3)));
        CHECK_EQ(instructionCount(policy, true, shift(ZShr, 31)), instructionCount(policy, false, shift(ZShr, 31)));
        CHECK_EQ(instructionCount(policy, true, truncation), instructionCount(policy, false, truncation) + 1);
    }
}

static void testBranchOnExactSumFitsInt32()
{
    enum class Right : uint8_t { IsWord, IsConstant, IsWideConstant };
    struct Case {
        B3::Opcode opcode;
        B3::Opcode comparison;
        Right right;
        bool sumHasOtherUserInBlock;
    };
    constexpr int64_t constant = 100;
    constexpr int64_t wideConstant = 1ll << 31;
    constexpr int64_t overflowMarker = 1ll << 40;
    auto build = [&](Procedure& proc, Case testCase) {
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* fits = proc.addBlock();
        BasicBlock* overflows = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
        Value* left = graph.make(root, SExt32, graph.make(root, Trunc, arguments[0]));
        Value* right = testCase.right == Right::IsWord ? graph.make(root, SExt32, graph.make(root, Trunc, arguments[1])) : graph.int64(root, testCase.right == Right::IsConstant ? constant : wideConstant);
        Value* exact = graph.make(root, testCase.opcode, left, right);
        Value* low = graph.make(root, Trunc, exact);
        Value* extendedSum = testCase.sumHasOtherUserInBlock ? graph.make(root, ZExt32, low) : nullptr;
        Value* isInt32 = graph.make(root, testCase.comparison, graph.make(root, SExt32, low), exact);
        if (testCase.comparison == Equal)
            graph.branch(root, isInt32, FrequentedBlock(fits), rarely(overflows));
        else
            graph.branch(root, isInt32, rarely(overflows), FrequentedBlock(fits));
        graph.ret(fits, extendedSum ? extendedSum : graph.make(fits, ZExt32, low));
        graph.ret(overflows, graph.make(overflows, Add, extendedSum ? extendedSum : graph.make(overflows, ZExt32, low), graph.int64(overflows, overflowMarker)));
        proc.resetReachability();
    };
    for (B3::Opcode opcode : { Add, Sub, Mul }) {
        for (B3::Opcode comparison : { Equal, NotEqual }) {
            for (Right right : { Right::IsWord, Right::IsConstant, Right::IsWideConstant }) {
                for (bool sumHasOtherUserInBlock : { false, true }) {
                    for (bool usesPolicy : { false, true }) {
                        Case testCase { opcode, comparison, right, sumHasOtherUserInBlock };
                        Procedure lowered(usesPolicy ? selectionPolicy() : Policy());
                        build(lowered, testCase);
                        lowerToAirForTesting(lowered);
                        unsigned numberOfBranchesOnOverflow = 0;
                        for (Air::BasicBlock* block : lowered.code()) {
                            for (Air::Inst& inst : *block)
                                numberOfBranchesOnOverflow += inst.kind.opcode == Air::BranchAdd32 || inst.kind.opcode == Air::BranchSub32 || inst.kind.opcode == Air::BranchMul32;
                        }
                        bool rightIsSelected = opcode == Mul ? right == Right::IsWord : right != Right::IsWideConstant;
                        CHECK_EQ(numberOfBranchesOnOverflow, usesPolicy && rightIsSelected && !sumHasOtherUserInBlock ? 1u : 0u);

                        Procedure proc(usesPolicy ? selectionPolicy() : Policy());
                        build(proc, testCase);
                        auto code = compileProc(proc);
                        for (int64_t a : { 0ll, 1ll, -1ll, 100ll, -100ll, 0x7fffffffll, 0x7fffff9cll, 0x7fffff9bll, -0x80000000ll, -0x7fffff9cll, -0x7fffff9dll, 0x123456787fffffffll, 0x1234567880000000ll }) {
                            for (int64_t b : { 0ll, 1ll, -1ll, 0x7fffffffll, -0x80000000ll, 0x7654321000000001ll }) {
                                int64_t x = static_cast<int32_t>(a);
                                int64_t y = right == Right::IsWord ? static_cast<int64_t>(static_cast<int32_t>(b)) : right == Right::IsConstant ? constant : wideConstant;
                                int64_t sum = opcode == Add ? x + y : opcode == Sub ? x - y : x * y;
                                int64_t expected = static_cast<int64_t>(static_cast<uint32_t>(sum)) + (sum == static_cast<int32_t>(sum) ? 0 : overflowMarker);
                                CHECK_EQ(invoke<int64_t>(*code, a, b), expected);
                            }
                        }
                    }
                }
            }
        }
    }
}

static void testBranchOnInt32OverflowInMemory()
{
    enum class Right : uint8_t { IsWord, IsConstant };
    enum class Extra : uint8_t { None, LoadAfterStore, StoreBeforeStore, OtherUserOfOldValue };
    struct Case {
        B3::Opcode opcode;
        B3::Opcode comparison;
        Right right;
        Extra extra;
    };
    constexpr int64_t constant = 100;
    constexpr int64_t overflowMarker = 1ll << 40;
    auto build = [&](Procedure& proc, Case testCase) {
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* fits = proc.addBlock();
        BasicBlock* overflows = proc.addBlock();
        auto arguments = cCallArgumentValues<int32_t*, int64_t>(proc, root);
        Value* pointer = arguments[0];
        Value* oldValue = graph.load(root, Int32, pointer, 4, firstHeap);
        Value* right = testCase.right == Right::IsWord ? graph.make(root, SExt32, graph.make(root, Trunc, arguments[1])) : graph.int64(root, constant);
        Value* exact = graph.make(root, testCase.opcode, graph.make(root, SExt32, oldValue), right);
        Value* low = graph.make(root, Trunc, exact);
        if (testCase.extra == Extra::StoreBeforeStore)
            graph.store(root, graph.int32(root, 7), pointer, 8, firstHeap);
        graph.store(root, low, pointer, 4, firstHeap);
        Value* result = graph.int64(root, 0);
        if (testCase.extra == Extra::LoadAfterStore)
            result = graph.make(root, ZExt32, graph.load(root, Int32, pointer, 4, firstHeap));
        if (testCase.extra == Extra::OtherUserOfOldValue)
            result = graph.make(root, ZExt32, oldValue);
        Value* isInt32 = graph.make(root, testCase.comparison, graph.make(root, SExt32, low), exact);
        if (testCase.comparison == Equal)
            graph.branch(root, isInt32, FrequentedBlock(fits), rarely(overflows));
        else
            graph.branch(root, isInt32, rarely(overflows), FrequentedBlock(fits));
        graph.ret(fits, result);
        graph.ret(overflows, graph.make(overflows, Add, result, graph.int64(overflows, overflowMarker)));
        proc.resetReachability();
    };
    for (B3::Opcode opcode : { Add, Sub }) {
        for (B3::Opcode comparison : { Equal, NotEqual }) {
            for (Right right : { Right::IsWord, Right::IsConstant }) {
                for (Extra extra : { Extra::None, Extra::LoadAfterStore, Extra::StoreBeforeStore, Extra::OtherUserOfOldValue }) {
                    for (bool usesPolicy : { false, true }) {
                        Case testCase { opcode, comparison, right, extra };
                        Procedure lowered(usesPolicy ? selectionPolicy() : Policy());
                        build(lowered, testCase);
                        lowerToAirForTesting(lowered);
                        unsigned numberOfUpdatesInMemory = numberOfInsts(lowered, [](Air::Inst& inst) {
                            return (inst.kind.opcode == Air::BranchAdd32 || inst.kind.opcode == Air::BranchSub32) && inst.args().size() == 3 && inst.args()[2].isAddr();
                        });
                        CHECK_EQ(numberOfUpdatesInMemory, isX86() && usesPolicy && extra == Extra::None ? 1u : 0u);

                        Procedure proc(usesPolicy ? selectionPolicy() : Policy());
                        build(proc, testCase);
                        auto code = compileProc(proc);
                        for (int64_t a : { 0ll, 1ll, -1ll, 100ll, -100ll, 0x7fffffffll, 0x7fffff9cll, 0x7fffff9bll, -0x80000000ll, -0x7fffff9cll, -0x7fffff9dll }) {
                            for (int64_t b : { 0ll, 1ll, -1ll, 0x7fffffffll, -0x80000000ll, 0x7654321000000001ll }) {
                                int64_t y = right == Right::IsWord ? static_cast<int64_t>(static_cast<int32_t>(b)) : constant;
                                int64_t sum = opcode == Add ? a + y : a - y;
                                int32_t words[4] = { 11, static_cast<int32_t>(a), 22, 33 };
                                int64_t result = extra == Extra::LoadAfterStore ? static_cast<int64_t>(static_cast<uint32_t>(sum)) : extra == Extra::OtherUserOfOldValue ? static_cast<int64_t>(static_cast<uint32_t>(a)) : 0;
                                CHECK_EQ(invoke<int64_t>(*code, words, b), result + (sum == static_cast<int32_t>(sum) ? 0 : overflowMarker));
                                CHECK_EQ(words[0], 11);
                                CHECK_EQ(words[1], static_cast<int32_t>(static_cast<uint32_t>(sum)));
                                CHECK_EQ(words[2], extra == Extra::StoreBeforeStore ? 7 : 22);
                                CHECK_EQ(words[3], 33);
                            }
                        }
                    }
                }
            }
        }
    }
}

static void testExactSumIsReadAfterJoin()
{
    Procedure proc(selectionPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* overflows = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(proc, root);
    Value* pointer = arguments[2];
    Value* exact = graph.make(root, Add, graph.make(root, SExt32, graph.make(root, Trunc, arguments[0])), graph.make(root, SExt32, graph.make(root, Trunc, arguments[1])));
    Value* low = graph.make(root, Trunc, exact);
    graph.branch(root, graph.make(root, NotEqual, graph.make(root, SExt32, low), exact), rarely(overflows), FrequentedBlock(join));
    graph.store(overflows, graph.int64(overflows, 1), pointer, 0, firstHeap);
    graph.jump(overflows, join);
    graph.ret(join, graph.make(join, ZExt32, low));
    auto code = compileProc(proc);
    for (int64_t a : { 0ll, -1ll, 0x7fffffffll, -0x80000000ll }) {
        for (int64_t b : { 0ll, 1ll, -1ll, 0x7fffffffll }) {
            int64_t sum = a + b;
            int64_t didOverflow = 0;
            CHECK_EQ(invoke<int64_t>(*code, a, b, &didOverflow), static_cast<int64_t>(static_cast<uint32_t>(sum)));
            CHECK_EQ(didOverflow, sum == static_cast<int32_t>(sum) ? 0 : 1);
        }
    }
}

static void testZeroExtensionClearsUpperHalf()
{
    struct Case {
        B3::Opcode opcode;
        uint32_t (*reference)(uint32_t, uint32_t);
    };
    for (Case testCase : { Case { Add, [](uint32_t a, uint32_t b) { return a + b; } }, Case { Sub, [](uint32_t a, uint32_t b) { return a - b; } }, Case { Mul, [](uint32_t a, uint32_t b) { return a * b; } },
        Case { BitOr, [](uint32_t a, uint32_t b) { return a | b; } }, Case { BitXor, [](uint32_t a, uint32_t b) { return a ^ b; } }, Case { BitAnd, [](uint32_t a, uint32_t b) { return a & b; } } }) {
        for (const Policy& policy : { Policy(), selectionPolicy(), optimizingPolicy() }) {
            Procedure proc(policy);
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
            Value* first = graph.make(root, Trunc, arguments[0]);
            Value* second = graph.make(root, Trunc, arguments[1]);
            graph.ret(root, graph.make(root, ZExt32, graph.make(root, testCase.opcode, first, second)));
            auto code = compileProc(proc);
            for (int64_t a : { 0ll, 1ll, -1ll, 0x7fffffffll, 0x80000000ll, 0x123456789abcdef0ll }) {
                for (int64_t b : { 0ll, 1ll, -1ll, 0x7fffffffll, -0xfffffffdll })
                    CHECK_EQ(invoke<uint64_t>(*code, a, b), static_cast<uint64_t>(testCase.reference(static_cast<uint32_t>(a), static_cast<uint32_t>(b))));
            }
        }
    }
}

static void testLow32BitsOfLoad64WithScaledIndex()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t>(proc, root);
    Value* offset = root->appendNew<Value>(proc, Shl, Origin(), arguments[1], root->appendNew<Const32Value>(proc, Origin(), 3));
    Value* address = root->appendNew<Value>(proc, Add, Origin(), arguments[0], offset);
    Value* word = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), address);
    Value* low = root->appendNew<Value>(proc, BitAnd, Origin(), word, root->appendNew<Const64Value>(proc, Origin(), 0xffffffff));
    root->appendNew<Value>(proc, Return, Origin(), low);

    auto code = compileProc(proc);
    int64_t words[2] = { 0x1111111122222222, 0x3333333344444444 };
    CHECK_EQ(invoke<int64_t>(*code, words, 0), 0x22222222);
    CHECK_EQ(invoke<int64_t>(*code, words, 1), 0x44444444);
}

static void testSelectOnInt64WithZeroLowHalf()
{
    for (unsigned optLevel : { 1u, 2u }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t, int64_t>(proc, root);
        Value* predicate = graph.make(root, Div, arguments[0], arguments[2]);
        Value* ifTrue = graph.make(root, Div, arguments[1], arguments[2]);
        graph.ret(root, graph.select(root, predicate, ifTrue, arguments[3]));
        auto code = compileProc(proc, optLevel);
        for (int64_t value : { static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(-1), static_cast<int64_t>(1ll << 32), static_cast<int64_t>(3ll << 32), static_cast<int64_t>(-1ll << 32), static_cast<int64_t>(0xfffe000000000000ull), std::numeric_limits<int64_t>::min() })
            CHECK_EQ(invoke<int64_t>(*code, value, 7, 1, 9), value ? 7 : 9);
    }
}

static void testDeclaredWidthsOf64BitSources()
{
    auto declaredWidths = [](Air::Inst inst) {
        Vector<Width> widths;
        inst.forEachArg([&](Air::Arg&, Air::Arg::Role, Bank, Width width) {
            widths.append(width);
        });
        return widths;
    };
    Air::Tmp a(GPRInfo::regT0);
    Air::Tmp b(GPRInfo::regT1);
    Air::Tmp c(GPRInfo::regT2);
    Air::Arg condition = Air::Arg::resCond(MacroAssembler::NonZero);

    Vector<Width> widths = declaredWidths(Air::Inst(Air::ExtractRegister64, nullptr, a, b, Air::Arg::imm(8), c));
    CHECK_EQ(widths[0], Width64);
    CHECK_EQ(widths[1], Width64);
    CHECK_EQ(widths[3], Width64);

    widths = declaredWidths(Air::Inst(Air::MoveConditionallyTest64, nullptr, condition, a, b, c, c));
    CHECK_EQ(widths[1], Width64);
    CHECK_EQ(widths[2], Width64);

    widths = declaredWidths(Air::Inst(Air::MoveConditionallyTest64, nullptr, condition, a, b, c, c, c));
    CHECK_EQ(widths[1], Width64);
    CHECK_EQ(widths[2], Width64);
}

static void testMul64ByImmediate()
{
    for (int64_t factor : { 3ll, 10ll, -7ll, 1000003ll, 0x7fffffffll, 0x123456789ll }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        graph.ret(root, graph.make(root, Mul, cCallArgumentValues<int64_t>(proc, root)[0], graph.int64(root, factor)));
        auto code = compileProc(proc);
        for (int64_t operand : { 0ll, 1ll, -1ll, 12345ll, 0x100000001ll })
            CHECK_EQ(invoke<int64_t>(*code, operand), static_cast<int64_t>(static_cast<uint64_t>(operand) * static_cast<uint64_t>(factor)));
    }
}

static void testStoreOfIncrementedAddressAtAddress()
{
    constexpr int32_t increment = 152;
    for (int32_t offsetOfStore : { 0, increment }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        Value* base = cCallArgumentValues<intptr_t>(proc, root)[0];
        Value* address = graph.make(root, Add, base, graph.int64(root, increment));
        root->appendNew<MemoryValue>(proc, Store, Origin(), address, base, offsetOfStore);
        graph.ret(root, address);
        auto code = compileProc(proc);

        std::array<intptr_t, increment / sizeof(intptr_t) + 1> words { };
        intptr_t expected = std::bit_cast<intptr_t>(words.data()) + increment;
        CHECK_EQ(invoke<intptr_t>(*code, std::bit_cast<intptr_t>(words.data())), expected);
        CHECK_EQ(words[offsetOfStore / sizeof(intptr_t)], expected);
    }
}

enum class IncrementedSelect : uint8_t {
    IncrementIsThen,
    IncrementIsElse,
    AddsTwo,
    SumHasSecondUse,
};

static void buildIncrementedSelect(Graph& graph, IncrementedSelect shape)
{
    BasicBlock* root = graph.proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t, int64_t>(graph.proc, root);
    Value* a = arguments[0];
    Value* b = arguments[1];
    Value* c = arguments[2];
    Value* d = arguments[3];
    Value* condition = graph.make(root, LessThan, a, b);
    Value* sum = graph.make(root, Add, c, graph.int64(root, shape == IncrementedSelect::AddsTwo ? 2 : 1));
    Value* selected = shape == IncrementedSelect::IncrementIsElse ? graph.select(root, condition, d, sum) : graph.select(root, condition, sum, d);
    graph.ret(root, shape == IncrementedSelect::SumHasSecondUse ? graph.make(root, Add, selected, sum) : selected);
}

static int64_t expectedIncrementedSelect(IncrementedSelect shape, int64_t a, int64_t b, int64_t c, int64_t d)
{
    switch (shape) {
    case IncrementedSelect::IncrementIsThen:
        return a < b ? c + 1 : d;
    case IncrementedSelect::IncrementIsElse:
        return a < b ? d : c + 1;
    case IncrementedSelect::AddsTwo:
        return a < b ? c + 2 : d;
    case IncrementedSelect::SumHasSecondUse:
        return (a < b ? c + 1 : d) + c + 1;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

static void testSelectOfIncrementedValue()
{
    struct Operands {
        int64_t a;
        int64_t b;
    };
    for (IncrementedSelect shape : { IncrementedSelect::IncrementIsThen, IncrementedSelect::IncrementIsElse, IncrementedSelect::AddsTwo, IncrementedSelect::SumHasSecondUse }) {
        for (bool usesPolicy : { false, true }) {
            bool isFused = isARM64() && usesPolicy && (shape == IncrementedSelect::IncrementIsThen || shape == IncrementedSelect::IncrementIsElse);
            Procedure lowered(usesPolicy ? selectionPolicy() : Policy());
            Graph loweredGraph { lowered };
            buildIncrementedSelect(loweredGraph, shape);
            lowerToAirForTesting(lowered);
            CHECK_EQ(numberOfInsts(lowered, Air::MoveConditionallyIncremented64), isFused ? 1u : 0u);
            if (isFused)
                CHECK_EQ(numberOfInsts(lowered, Air::Add64), 0u);

            Procedure proc(usesPolicy ? selectionPolicy() : Policy());
            Graph graph { proc };
            buildIncrementedSelect(graph, shape);
            auto code = compileProc(proc);
            for (Operands operands : { Operands { 1, 2 }, Operands { 2, 1 }, Operands { 2, 2 } }) {
                for (int64_t c : { static_cast<int64_t>(10), static_cast<int64_t>(-1) }) {
                    int64_t d = 20;
                    CHECK_EQ(invoke<int64_t>(*code, operands.a, operands.b, c, d), expectedIncrementedSelect(shape, operands.a, operands.b, c, d));
                }
            }
        }
    }
}

static void buildBranchOnLoad16(Graph& graph)
{
    Procedure& proc = graph.proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* yes = proc.addBlock();
    BasicBlock* no = proc.addBlock();
    Value* pointer = cCallArgumentValues<uint16_t*>(proc, root)[0];
    graph.branch(root, graph.make(root, Equal, root->appendNew<MemoryValue>(proc, Load16Z, Origin(), pointer, 2), graph.int32(root, 0x1234)), yes, no);
    graph.ret(yes, graph.int64(yes, 1));
    graph.ret(no, graph.int64(no, 2));
}

static void testBranchOnLoad16()
{
    struct Case {
        uint16_t neighbor;
        uint16_t word;
        int64_t expected;
    };
    for (bool usesPolicy : { false, true }) {
        Procedure lowered(usesPolicy ? selectionPolicy() : Policy());
        Graph loweredGraph { lowered };
        buildBranchOnLoad16(loweredGraph);
        lowerToAirForTesting(lowered);
        CHECK_EQ(numberOfInsts(lowered, Air::Branch16), isX86() && usesPolicy ? 1u : 0u);

        Procedure proc(usesPolicy ? selectionPolicy() : Policy());
        Graph graph { proc };
        buildBranchOnLoad16(graph);
        auto code = compileProc(proc);
        for (Case testCase : { Case { 0, 0x1234, 1 }, Case { 0, 0x1235, 2 }, Case { 0xffff, 0x1234, 1 } }) {
            uint16_t memory[3] = { testCase.neighbor, testCase.word, testCase.neighbor };
            CHECK_EQ(invoke<int64_t>(*code, memory), testCase.expected);
        }
    }
}

static unsigned numberOfMovesFromRegister(Procedure& proc, GPRReg reg)
{
    return numberOfInsts(proc, [&](Air::Inst& inst) {
        return inst.kind.opcode == Air::Move && inst.args()[0].isTmp() && inst.args()[0].tmp() == Air::Tmp(reg);
    });
}

static void buildSumOfArgumentRegisters(Graph& graph)
{
    BasicBlock* root = graph.proc.addBlock();
    Value* first = root->appendNew<ArgumentRegValue>(graph.proc, Origin(), GPRInfo::argumentGPR0);
    Value* second = root->appendNew<ArgumentRegValue>(graph.proc, Origin(), GPRInfo::argumentGPR1);
    graph.ret(root, graph.make(root, Add, first, second));
}

static void testPinnedRegisterIsNamedDirectly()
{
    struct Case {
        bool isPinned;
        bool namesPinnedRegistersDirectly;
        unsigned expectedMoves;
    };
    for (Case testCase : { Case { true, true, 0 }, Case { true, false, 1 }, Case { false, true, 1 } }) {
        Policy casePolicy = policyWith([&](Policy& policy) {
            policy.addressing.namesPinnedRegistersDirectly = testCase.namesPinnedRegistersDirectly;
            if (testCase.isPinned)
                policy.air.registers.pinned.add(GPRInfo::argumentGPR1, IgnoreVectors);
        });
        Procedure lowered(casePolicy);
        Graph loweredGraph { lowered };
        buildSumOfArgumentRegisters(loweredGraph);
        lowerToAirForTesting(lowered);
        CHECK_EQ(numberOfMovesFromRegister(lowered, GPRInfo::argumentGPR1), testCase.expectedMoves);

        Procedure proc(casePolicy);
        Graph graph { proc };
        buildSumOfArgumentRegisters(graph);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(3), static_cast<int64_t>(4)), 7);
    }
}

static void testDefaultPolicySelectsUpstreamInstructions()
{
    constexpr GPRReg pinnedRegister = GPRInfo::argumentGPR5;
    Procedure proc;
    proc.pinRegister(pinnedRegister);
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* fits = proc.addBlock();
    BasicBlock* overflows = proc.addBlock();
    BasicBlock* selection = proc.addBlock();
    BasicBlock* yes = proc.addBlock();
    BasicBlock* no = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t, int64_t, uint16_t*>(proc, root);
    Value* a = arguments[0];
    Value* b = arguments[1];
    Value* c = arguments[2];
    Value* d = arguments[3];
    Value* pointer = arguments[4];

    Value* exact = graph.make(root, Add, graph.make(root, SExt32, graph.make(root, Trunc, a)), graph.make(root, SExt32, graph.make(root, Trunc, b)));
    Value* low = graph.make(root, Trunc, exact);
    graph.branch(root, graph.make(root, Equal, graph.make(root, SExt32, low), exact), FrequentedBlock(fits), rarely(overflows));

    Value* sumOfBranch = graph.phi(selection, Int64);
    graph.upsilon(fits, graph.make(fits, ZExt32, low), sumOfBranch);
    graph.jump(fits, selection);
    graph.upsilon(overflows, graph.make(overflows, Add, graph.make(overflows, ZExt32, low), graph.int64(overflows, 1ll << 40)), sumOfBranch);
    graph.jump(overflows, selection);

    Value* increment = graph.make(selection, Add, c, graph.int64(selection, 1));
    Value* selected = graph.select(selection, graph.make(selection, LessThan, a, b), increment, d);
    Value* sumOfSelect = graph.make(selection, Add, sumOfBranch, selected);
    Value* halfWord = selection->appendNew<MemoryValue>(proc, Load16Z, Origin(), pointer, 2);
    graph.branch(selection, graph.make(selection, Equal, halfWord, graph.int32(selection, 0x1234)), yes, no);

    Value* sumOfLoad = graph.phi(join, Int64);
    graph.upsilon(yes, graph.int64(yes, 1), sumOfLoad);
    graph.jump(yes, join);
    graph.upsilon(no, graph.int64(no, 2), sumOfLoad);
    graph.jump(no, join);
    Value* pinned = join->appendNew<ArgumentRegValue>(proc, Origin(), pinnedRegister);
    graph.ret(join, graph.make(join, Add, graph.make(join, Add, sumOfSelect, sumOfLoad), pinned));

    lowerToAirForTesting(proc);
    for (Air::Opcode opcode : { Air::BranchAdd32, Air::BranchSub32, Air::Branch16, Air::MoveConditionallyIncremented32, Air::MoveConditionallyIncremented64, Air::MoveConditionallyIncrementedTest32, Air::MoveConditionallyIncrementedTest64 })
        CHECK_EQ(numberOfInsts(proc, opcode), 0u);
    CHECK_EQ(numberOfMovesFromRegister(proc, pinnedRegister), 1u);
}

static void testPatchFormsAreFilled()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
    PatchpointValue* patchpoint = graph.emptyPatchpoint(root, Int64);
    patchpoint->appendSomeRegister(x);
    CheckValue* check = root->appendNew<CheckValue>(proc, Check, Origin(), graph.make(root, Equal, x, graph.int64(root, 0)));
    check->append(x);
    graph.ret(root, patchpoint);

    CHECK(!patchpoint->areConstraintsFrozen());
    CHECK(!check->areConstraintsFrozen());
    lowerToAirForTesting(proc);
    CHECK(patchpoint->areConstraintsFrozen());
    CHECK(check->areConstraintsFrozen());
    CHECK_EQ(numberOfInsts(proc, Air::Patch), 2u);
    CHECK_EQ(numberOfInsts(proc, [](Air::Inst& inst) {
        return inst.kind.opcode == Air::Patch && !inst.hasCurrentForms();
    }), 0u);
}

static void buildSumWithFoldedConstant(Graph& graph)
{
    BasicBlock* root = graph.proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(graph.proc, root)[0];
    graph.ret(root, graph.make(root, Add, x, graph.int64(root, 8)));
}

static void testFoldedConstantIsRemoved()
{
    Procedure lowered;
    Graph loweredGraph { lowered };
    buildSumWithFoldedConstant(loweredGraph);
    lowerToAirForTesting(lowered);
    CHECK_EQ(numberOfDeadMoves(lowered), 1u);
    Air::eliminateDeadCode(lowered.code());
    CHECK_EQ(numberOfDeadMoves(lowered), 0u);

    Procedure proc;
    Graph graph { proc };
    buildSumWithFoldedConstant(graph);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(1)), 9);
}

static void appendPairToPhi(Graph& graph, BasicBlock* block, Value* phi, int32_t first, int32_t second)
{
    PatchpointValue* patchpoint = graph.emptyPatchpoint(block, phi->type());
    patchpoint->resultConstraints = { ValueRep::SomeRegister, ValueRep::SomeRegister };
    patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.move(CCallHelpers::TrustedImm32(first), params[0].gpr());
        jit.move(CCallHelpers::TrustedImm32(second), params[1].gpr());
    });
    graph.upsilon(block, patchpoint, phi);
}

static void buildFirstTupleComponent(Graph& graph)
{
    Procedure& proc = graph.proc;
    Type pair = proc.addTuple({ Int64, Int64 });
    BasicBlock* root = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    graph.branch(root, cCallArgumentValues<int64_t>(proc, root)[0], left, right);
    Value* phi = graph.phi(join, pair);
    appendPairToPhi(graph, left, phi, 1, 2);
    graph.jump(left, join);
    appendPairToPhi(graph, right, phi, 3, 4);
    graph.jump(right, join);
    graph.ret(join, join->appendNew<ExtractValue>(proc, Origin(), Int64, phi, 0));
}

static void testUnusedTupleComponentIsRemoved()
{
    Procedure lowered;
    Graph loweredGraph { lowered };
    buildFirstTupleComponent(loweredGraph);
    lowerToAirForTesting(lowered);
    CHECK(numberOfDeadMoves(lowered) >= 1u);
    Air::eliminateDeadCode(lowered.code());
    CHECK_EQ(numberOfDeadMoves(lowered), 0u);

    Procedure proc;
    Graph graph { proc };
    buildFirstTupleComponent(graph);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(1)), 1);
    CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(0)), 3);
}

static void buildTerminalPatchpoint(Graph& graph)
{
    Procedure& proc = graph.proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* next = proc.addBlock();
    BasicBlock* other = proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
    PatchpointValue* patchpoint = graph.emptyPatchpoint(root, Int64);
    patchpoint->effects.terminal = true;
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    patchpoint->appendSomeRegister(x);
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        CCallHelpers::Jump toSecond = jit.branchTest64(CCallHelpers::NonZero, params[1].gpr());
        jit.move(CCallHelpers::TrustedImm32(42), GPRInfo::returnValueGPR);
        CCallHelpers::Jump toFirst;
        if (!params.fallsThroughToSuccessor(0))
            toFirst = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            toSecond.linkTo(*labels[1], &jit);
            if (toFirst.isSet())
                toFirst.linkTo(*labels[0], &jit);
        });
    });
    root->setSuccessors(FrequentedBlock(next), FrequentedBlock(other));
    graph.ret(next, graph.make(next, Add, patchpoint, graph.int64(next, 1)));
    graph.ret(other, graph.int64(other, 7));
}

static void testTerminalPatchpointResultInRegister()
{
    for (const Policy& policy : { Policy(), optimizingPolicy() }) {
        Procedure lowered(policy);
        Graph loweredGraph { lowered };
        buildTerminalPatchpoint(loweredGraph);
        lowerToAirForTesting(lowered);

        Procedure proc(policy);
        Graph graph { proc };
        buildTerminalPatchpoint(graph);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(0)), 43);
        CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(1)), 7);
    }
}

static constexpr unsigned numberOfLiveValues = 10;
static_assert(numberOfLiveValues > GPRInfo::numberOfArgumentRegisters);

static constexpr int64_t leftMarker = 1ll << 20;
static constexpr int64_t rightMarker = 1ll << 21;
static constexpr int64_t padMarker = 1ll << 22;

struct PatchpointsWithPads {
    PatchpointValue* left;
    PatchpointValue* right;
};

static void returnSum(Graph& graph, BasicBlock* block, const Vector<Value*>& values, int64_t marker)
{
    Value* sum = graph.int64(block, marker);
    for (Value* value : values)
        sum = graph.make(block, Add, sum, value);
    graph.ret(block, sum);
}

static void emitGarbage(CCallHelpers& jit, const RegisterSet& registers)
{
    constexpr int64_t garbage = 0x0bad0bad0bad0badll;
    registers.forEach([&](Reg reg) {
        jit.move(CCallHelpers::TrustedImm64(garbage), reg.gpr());
    });
}

static PatchpointValue* appendPatchpointWithPad(Graph& graph, BasicBlock* block, Value* takesPad, const Vector<Value*>& liveValues, int64_t marker, const RegisterSet& lateClobbers = { })
{
    Procedure& proc = graph.proc;
    BasicBlock* continuation = proc.addBlock();
    BasicBlock* pad = proc.addBlock();
    RegisterSet clobbers = proc.mutableGPRs();
    clobbers.exclude(RegisterSet::argumentGPRs());

    PatchpointValue* patchpoint = graph.emptyPatchpoint(block);
    patchpoint->effects.terminal = true;
    patchpoint->appendSomeRegister(takesPad);
    patchpoint->clobberLate(lateClobbers);
    patchpoint->clobberLateOnSuccessor(1, clobbers);
    patchpoint->setGenerator([clobbers, lateClobbers](CCallHelpers& jit, const StackmapGenerationParams& params) {
        CCallHelpers::Jump takesPad = jit.branchTest64(CCallHelpers::NonZero, params[0].gpr());
        emitGarbage(jit, lateClobbers);
        CCallHelpers::Jump toContinuation = jit.jump();
        takesPad.link(&jit);
        emitGarbage(jit, lateClobbers);
        emitGarbage(jit, clobbers);
        CCallHelpers::Jump toPad = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            toContinuation.linkTo(*labels[0], &jit);
            toPad.linkTo(*labels[1], &jit);
        });
    });
    block->setSuccessors(FrequentedBlock(continuation), rarely(pad));
    returnSum(graph, continuation, liveValues, marker);
    returnSum(graph, pad, liveValues, marker + padMarker);
    return patchpoint;
}

static PatchpointsWithPads buildPatchpointsWithPads(Graph& graph)
{
    Procedure& proc = graph.proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t, int64_t>(proc, root);
    Value* pointer = arguments[0];
    Value* takesLeft = arguments[1];
    Value* takesPad = arguments[2];
    Vector<Value*> liveValues;
    for (unsigned i = 0; i < numberOfLiveValues; ++i)
        liveValues.append(graph.load(root, Int64, pointer, static_cast<int32_t>(i * sizeof(int64_t)), firstHeap));
    graph.branch(root, takesLeft, left, right);
    return { appendPatchpointWithPad(graph, left, takesPad, liveValues, leftMarker), appendPatchpointWithPad(graph, right, takesPad, liveValues, rightMarker) };
}

static Air::BasicBlock* findBlockEndingWith(Procedure& proc, Value* terminal)
{
    for (Air::BasicBlock* block : proc.code()) {
        if (block->last().origin == terminal)
            return block;
    }
    return nullptr;
}

static bool isBlockEntryClobber(Air::Inst& inst)
{
    return inst.kind.opcode == Air::Patch && inst.args()[0].special()->clobbersAtBlockEntry(inst);
}

static void checkBlockEntryClobbers(const Policy& policy)
{
    Procedure proc(policy);
    Graph graph { proc };
    PatchpointsWithPads patchpoints = buildPatchpointsWithPads(graph);
    RegisterSet clobbers = patchpoints.left->lateClobberedOnSuccessor(1);
    CHECK(!clobbers.isEmpty());
    CHECK(patchpoints.left->lateClobberedOnSuccessor(0).isEmpty());
    CHECK(patchpoints.right->lateClobberedOnSuccessor(1) == clobbers);
    generateToAir(proc);
    Air::validate(proc.code());

    Vector<Air::Special*> specials;
    for (PatchpointValue* patchpoint : { patchpoints.left, patchpoints.right }) {
        Air::BasicBlock* block = findBlockEndingWith(proc, patchpoint);
        CHECK(block);
        CHECK_EQ(block->numSuccessors(), 2u);
        CHECK(block->successorBlock(0)->at(0).kind.opcode != Air::Patch);
        Air::Inst& padEntry = block->successorBlock(1)->at(0);
        CHECK(isBlockEntryClobber(padEntry));
        specials.append(padEntry.args()[0].special());
    }
    CHECK(specials[0] == specials[1]);
    CHECK(static_cast<Air::BlockEntryClobberSpecial*>(specials[0])->clobberedRegisters() == clobbers);
    CHECK_EQ(numberOfInsts(proc, isBlockEntryClobber), 2u);
}

static void testSuccessorClobbersBecomeBlockEntryClobbers()
{
    for (const Policy& policy : { Policy(), optimizingPolicy() }) {
        checkBlockEntryClobbers(policy);

        Procedure proc(policy);
        Graph graph { proc };
        buildPatchpointsWithPads(graph);
        auto code = compileProc(proc);
        int64_t memory[numberOfLiveValues] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };
        constexpr int64_t sum = 1023;
        for (bool takesLeft : { false, true }) {
            for (bool takesPad : { false, true }) {
                int64_t expected = sum + (takesLeft ? leftMarker : rightMarker) + (takesPad ? padMarker : 0);
                CHECK_EQ(invoke<int64_t>(*code, memory, static_cast<int64_t>(takesLeft), static_cast<int64_t>(takesPad)), expected);
            }
        }
    }
}

static void testValuesSurvivePatchpointAndSuccessorClobbers()
{
    for (const Policy& policy : { Policy(), optimizingPolicy() }) {
        Procedure proc(policy);
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* usual = proc.addBlock();
        BasicBlock* site = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t*, int64_t, int64_t>(proc, root).eager();
        Vector<Value*> liveValues;
        for (unsigned i = 0; i < numberOfLiveValues; ++i)
            liveValues.append(graph.load(root, Int64, arguments[0], static_cast<int32_t>(i * sizeof(int64_t)), firstHeap));
        graph.branch(root, arguments[1], rarely(site), FrequentedBlock(usual));
        returnSum(graph, usual, liveValues, leftMarker);
        appendPatchpointWithPad(graph, site, arguments[2], liveValues, rightMarker, RegisterSet::argumentGPRs());

        auto code = compileProc(proc);
        int64_t memory[numberOfLiveValues] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };
        constexpr int64_t sum = 1023;
        CHECK_EQ(invoke<int64_t>(*code, memory, static_cast<int64_t>(0), static_cast<int64_t>(0)), sum + leftMarker);
        CHECK_EQ(invoke<int64_t>(*code, memory, static_cast<int64_t>(1), static_cast<int64_t>(0)), sum + rightMarker);
        CHECK_EQ(invoke<int64_t>(*code, memory, static_cast<int64_t>(1), static_cast<int64_t>(1)), sum + rightMarker + padMarker);
    }
}

static void testPatchpointRunsWithoutFrame()
{
    struct Case {
        StackmapValue* value;
        bool runsWithoutFrame;
    };
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
    Vector<Case> cases;
    for (bool runsWithoutFrame : { false, true }) {
        cases.append(Case { graph.emptyPatchpoint(root), runsWithoutFrame });
        cases.append(Case { root->appendNew<CheckValue>(proc, Check, Origin(), graph.make(root, Equal, x, graph.int64(root, 0))), runsWithoutFrame });
    }
    graph.ret(root, x);
    for (const Case& testCase : cases) {
        CHECK(!testCase.value->runsWithoutFrame());
        testCase.value->setRunsWithoutFrame(testCase.runsWithoutFrame);
    }

    lowerToAirForTesting(proc);
    CHECK_EQ(numberOfInsts(proc, Air::Patch), 4u);
    for (const Case& testCase : cases) {
        CHECK_EQ(numberOfInsts(proc, [&](Air::Inst& inst) {
            return inst.kind.opcode == Air::Patch && inst.origin == testCase.value && inst.needsFrame() == !testCase.runsWithoutFrame;
        }), 1u);
    }
}

static void testChecksShareLatePath()
{
    constexpr int64_t failed = 77;
    struct Case {
        bool sharesLatePath;
        bool hasOneGenerator;
        bool hasOneOperand;
        unsigned numberOfLatePaths;
    };
    for (Case testCase : { Case { true, true, true, 1 }, Case { false, true, true, 2 }, Case { true, false, true, 2 }, Case { true, true, false, 2 } }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
        unsigned numberOfLatePaths = 0;
        auto createGenerator = [&] {
            return createSharedTask<StackmapGeneratorFunction>([&](CCallHelpers& jit, const StackmapGenerationParams&) {
                ++numberOfLatePaths;
                jit.move(CCallHelpers::TrustedImm64(failed), GPRInfo::returnValueGPR);
                jit.emitFunctionEpilogue();
                jit.ret();
            });
        };
        RefPtr<StackmapGenerator> generator = createGenerator();
        for (unsigned index = 0; index < 2; ++index) {
            CheckValue* check = root->appendNew<CheckValue>(proc, Check, Origin(), graph.make(root, Equal, arguments[index], graph.int64(root, 5)));
            check->append(ConstrainedValue(arguments[testCase.hasOneOperand ? 0 : index], ValueRep::SomeRegister));
            check->setSharesLatePath(testCase.sharesLatePath);
            check->setGenerator(index && !testCase.hasOneGenerator ? createGenerator() : generator);
        }
        graph.ret(root, graph.make(root, Add, arguments[0], arguments[1]));

        auto code = compileProc(proc);
        CHECK_EQ(numberOfLatePaths, testCase.numberOfLatePaths);
        CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(1), static_cast<int64_t>(2)), 3);
        CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(5), static_cast<int64_t>(2)), failed);
        CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(1), static_cast<int64_t>(5)), failed);
    }
}

static unsigned numberOfConstantMoves(Procedure& proc, int64_t constant)
{
    return numberOfInsts(proc, [&](Air::Inst& inst) {
        if (inst.kind.opcode != Air::Move)
            return false;
        const Air::Arg& source = inst.args()[0];
        return (source.isImm() || source.isBigImm()) && source.value() == constant;
    });
}

static bool isShift(B3::Opcode opcode)
{
    return opcode == Shl || opcode == SShr || opcode == ZShr || opcode == RotR || opcode == RotL;
}

struct ImmediateFormCase {
    B3::Opcode opcode;
    Type type;
    int64_t constant;
    bool constantIsRightChild;
    bool isBranchCondition;
};

static void checkImmediateForm(const ImmediateFormCase& testCase)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    if (testCase.type == Int32)
        argument = graph.make(root, Trunc, argument);
    Value* constant = testCase.type == Int64 && !isShift(testCase.opcode) ? graph.int64(root, testCase.constant) : graph.int32(root, static_cast<int32_t>(testCase.constant));
    Value* operation = testCase.constantIsRightChild ? graph.make(root, testCase.opcode, argument, constant) : graph.make(root, testCase.opcode, constant, argument);
    if (testCase.isBranchCondition) {
        BasicBlock* yes = proc.addBlock();
        BasicBlock* no = proc.addBlock();
        graph.branch(root, operation, yes, no);
        graph.ret(yes, argument);
        graph.ret(no, argument);
    } else
        graph.ret(root, operation);

    lowerToAirForTesting(proc);
    Air::eliminateDeadCode(proc.code());
    if (isSelectedAsImmediate(operation, constant))
        CHECK_EQ(numberOfConstantMoves(proc, constant->asInt()), 0u);
}

static void checkImmediateFormsOfOpcode(B3::Opcode opcode, bool hasBranchForm)
{
    constexpr int64_t constants[] = { 0, 1, 31, 255, 4095, 4096, 65535, -1, -4096, 0x7fffffff, 0x80000000ll, static_cast<int64_t>(0xff00ff00ff00ff00ull) };
    for (Type type : { Int32, Int64 }) {
        for (int64_t constant : constants) {
            for (bool isBranchCondition : { false, true }) {
                if (isBranchCondition && !hasBranchForm)
                    continue;
                checkImmediateForm({ opcode, type, constant, true, isBranchCondition });
                if (!isShift(opcode))
                    checkImmediateForm({ opcode, type, constant, false, isBranchCondition });
            }
        }
    }
}

static void testImmediateFormsMatchSelection()
{
    for (B3::Opcode opcode : { Add, Sub, Shl, SShr, ZShr, RotR, BitAnd, BitOr, BitXor })
        checkImmediateFormsOfOpcode(opcode, false);
    if (isX86_64())
        checkImmediateFormsOfOpcode(RotL, false);
    for (B3::Opcode opcode : { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual })
        checkImmediateFormsOfOpcode(opcode, true);

    constexpr int64_t wideConstant = 0x123456789all;
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* constant = graph.int64(root, wideConstant);
    Value* sum = graph.make(root, Add, argument, constant);
    graph.ret(root, sum);
    CHECK(!isSelectedAsImmediate(sum, constant));
    lowerToAirForTesting(proc);
    Air::eliminateDeadCode(proc.code());
    CHECK_EQ(numberOfConstantMoves(proc, wideConstant), 1u);
}

void addBunSelectionTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testUseCountsDuplicateChild());
    RUN(testRedundantZeroExtension());
    RUN(testBranchOnExactSumFitsInt32());
    RUN(testBranchOnInt32OverflowInMemory());
    RUN(testExactSumIsReadAfterJoin());
    RUN(testZeroExtensionClearsUpperHalf());
    RUN(testLow32BitsOfLoad64WithScaledIndex());
    RUN(testSelectOnInt64WithZeroLowHalf());
    RUN(testDeclaredWidthsOf64BitSources());
    RUN(testMul64ByImmediate());
    RUN(testStoreOfIncrementedAddressAtAddress());
    RUN(testSelectOfIncrementedValue());
    RUN(testBranchOnLoad16());
    RUN(testPinnedRegisterIsNamedDirectly());
    RUN(testDefaultPolicySelectsUpstreamInstructions());
    RUN(testPatchFormsAreFilled());
    RUN(testFoldedConstantIsRemoved());
    RUN(testUnusedTupleComponentIsRemoved());
    RUN(testTerminalPatchpointResultInRegister());
    RUN(testSuccessorClobbersBecomeBlockEntryClobbers());
    RUN(testValuesSurvivePatchpointAndSuccessorClobbers());
    RUN(testPatchpointRunsWithoutFrame());
    RUN(testChecksShareLatePath());
    RUN(testImmediateFormsMatchSelection());
    addBunCompareChainTests(config, tasks);
}

#endif // ENABLE(B3_JIT)
