/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTGrowthOfWork.h"
#include "AOTMachineTestSupport.h"
#include "AllowMacroScratchRegisterUsage.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <type_traits>
#include <wtf/DataLog.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

[[noreturn]] static void allocationTestFail(const TestProcedure& test, const char* file, int line, const char* condition)
{
    dataLogLn(test.printedProcedure(), test.printedAllocation());
    failNativeTest(file, line, condition);
}

#define AOT_ALLOCATION_TEST_CHECK(test, ...) do { \
        if (!(__VA_ARGS__)) [[unlikely]] \
            allocationTestFail(test, __FILE__, __LINE__, #__VA_ARGS__); \
    } while (false)

// printedAllocation(), read line by line. The verb of a line is the opcode of an operation that is executed, and else the kind of the step: move, reload, save, materialize, swap.
// The one store of a spilled value has no step, and the text shows it as a save behind the definition.
class AllocationTestText {
public:
    explicit AllocationTestText(const TestProcedure& test)
        : m_text(test.printedAllocation())
    {
        uint32_t block = UINT32_MAX;
        for (StringView line : StringView(m_text).split('\n')) {
            if (line.startsWith('#')) {
                block = parseIntegerAllowingTrailingJunk<uint32_t>(line.substring(1)).value_or(UINT32_MAX);
                continue;
            }
            if (!line.startsWith(' ')) {
                block = UINT32_MAX;
                continue;
            }
            StringView step = line.trim(isASCIIWhitespace<char16_t>);
            if (size_t assignment = step.find(" = "_s); assignment != notFound)
                step = step.substring(static_cast<unsigned>(assignment) + 3);
            uint32_t value = parseInteger<uint32_t>(line.substring(static_cast<unsigned>(line.reverseFind('@')) + 1)).value_or(UINT32_MAX);
            m_lines.append(Line { block, step.left(static_cast<unsigned>(step.find(' '))), value });
        }
    }

    // With a blank between them: "Store Jump".
    String verbsOf(const Block* block) const
    {
        StringBuilder result;
        for (const Line& line : m_lines) {
            if (line.block != block->index())
                continue;
            if (!result.isEmpty())
                result.append(' ');
            result.append(line.verb);
        }
        return result.toString();
    }

    // The lines with the verb, among those that end with the value and stand in the block where these are given.
    unsigned count(ASCIILiteral verb, const Operation* value = nullptr, const Block* block = nullptr) const
    {
        unsigned result = 0;
        for (const Line& line : m_lines)
            result += line.verb == verb && (!value || line.value == value->index()) && (!block || line.block == block->index());
        return result;
    }

    bool isStoredOnceBehindDefinition(const Operation* value) const
    {
        if (count("save"_s, value) != 1)
            return false;
        for (size_t index = 1; index < m_lines.size(); ++index) {
            if (m_lines[index].verb == "save"_s && m_lines[index].value == value->index())
                return m_lines[index - 1].value == value->index() && m_lines[index - 1].verb == descriptionOf(value->opcode()).name;
        }
        return false;
    }

    bool mentions(ASCIILiteral words) const { return m_text.contains(words); }

private:
    struct Line {
        uint32_t block;
        StringView verb;
        uint32_t value;
    };

    String m_text;
    Vector<Line> m_lines;
};

template<typename Word>
inline constexpr TypeKind allocationTestTypeOf = std::is_same_v<Word, double> ? Double : Int64;

static unsigned allocationTestCountOf(RegisterMask mask) { return static_cast<unsigned>(std::popcount(mask)); }

template<typename Word>
static std::array<Word, 64> allocationTestWords()
{
    std::array<Word, 64> words;
    for (unsigned index = 0; index < words.size(); ++index)
        words[index] = static_cast<Word>(index * index + 3 * index + 5);
    return words;
}

static TypedPointer allocationTestWordAt(TestProcedure& test, LValue base, unsigned index)
{
    return test.out().address(test.heaps().argumentBuffer, base, static_cast<ptrdiff_t>(index * sizeof(uint64_t)));
}

static Vector<LValue> allocationTestLoadWords(TestProcedure& test, LValue base, unsigned first, unsigned count, Type type)
{
    Vector<LValue> values;
    for (unsigned index = 0; index < count; ++index)
        values.append(test.out().load(allocationTestWordAt(test, base, first + index), type));
    return values;
}

// Twice what came before, less the value. No two values change places unseen, and the value is the operand that x86-64 can read from memory.
static LValue allocationTestMix(Output& out, LValue sum, LValue value) { return out.sub(out.add(sum, sum), value); }

static LValue allocationTestMixAll(Output& out, std::span<const LValue> values)
{
    LValue sum = values[0];
    for (LValue value : values.subspan(1))
        sum = allocationTestMix(out, sum, value);
    return sum;
}

template<typename Word>
static Word allocationTestMixed(Word sum, Word word) { return sum + sum - word; }

template<typename Word>
static Word allocationTestMixedAll(std::span<const Word> words)
{
    Word sum = words[0];
    for (Word word : words.subspan(1))
        sum = allocationTestMixed(sum, word);
    return sum;
}

// Adds one to the word that `counter` points at, if there is one, and leaves poison in every register that no callee saves.
static PatchOperation* allocationTestAppendCall(TestProcedure& test, LValue counter)
{
    RegisterFile file = registerFileFor(test.procedure().traits());
    RegisterMask clobberedGPRs = file.allocatableIn(Bank::GP) & ~file.calleeSaved;
    RegisterMask clobberedFPRs = file.allocatableIn(Bank::FP) & ~file.calleeSaved;

    PatchOperation* call = test.out().patchpoint(Void);
    if (counter)
        call->appendSomeRegister(counter);
    call->clobberLate(clobberedGPRs | clobberedFPRs);
    call->setGenerator([clobberedGPRs, clobberedFPRs, counts = !!counter](CCallHelpers& jit, const PatchParameters& parameters) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (counts)
            jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(parameters[0].gpr()));
        forEachRegister(clobberedGPRs, [&](Reg reg) {
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(poison)), reg.gpr());
        });
        GPRReg poisoned = Reg::fromIndex(static_cast<unsigned>(std::countr_zero(clobberedGPRs))).gpr();
        forEachRegister(clobberedFPRs, [&](Reg reg) {
            jit.move64ToDouble(poisoned, reg.fpr());
        });
    });
    return call;
}

static Reg allocationTestRegisterAtHead(TestProcedure& test, const Block* block, const Operation* value)
{
    for (const RegisterBinding& binding : test.procedure().allocation().registersAtHeadOf(block)) {
        if (binding.value == value)
            return binding.reg;
    }
    return Reg();
}

// How often an instruction reads the spilled value where it lies.
static unsigned allocationTestReadsInPlace(TestProcedure& test, const Operation* value)
{
    const Allocation& allocation = test.procedure().allocation();
    Location spill = Location::spill(allocation.spillOf(value));
    unsigned result = 0;
    for (const Step& step : allocation.steps()) {
        if (step.kind != StepKind::Execute)
            continue;
        for (Location source : step.sources)
            result += source == spill;
    }
    return result;
}

static Block* allocationTestSourceOfBackEdge(const Order& order, const Block* head)
{
    for (Block* predecessor : order.of(head).predecessors) {
        if (order.of(predecessor).orderIndex >= order.of(head).orderIndex)
            return predecessor;
    }
    return nullptr;
}

// Goes left where the first argument is odd.
struct AllocationTestDiamond {
    explicit AllocationTestDiamond(TestProcedure& test)
        : first(test.argumentGPR(0))
        , second(test.argumentGPR(1))
        , buffer(test.argumentGPR(2))
        , left(test.out().newBlock())
        , right(test.out().newBlock())
        , join(test.out().newBlock())
    {
        Output& out = test.out();
        out.branch(out.testNonZero64(first, 1), normalEdge(left), normalEdge(right));
    }

    LValue first;
    LValue second;
    LValue buffer;
    LBasicBlock left;
    LBasicBlock right;
    LBasicBlock join;
};

static void allocatorMakesNoMovesAtJoinOfAgreeingPredecessors()
{
    TestProcedure test;
    Output& out = test.out();
    AllocationTestDiamond diamond(test);

    out.appendTo(diamond.left);
    out.store64(diamond.first, allocationTestWordAt(test, diamond.buffer, 0));
    out.jump(normalEdge(diamond.join));

    out.appendTo(diamond.right);
    out.store64(diamond.second, allocationTestWordAt(test, diamond.buffer, 0));
    out.jump(normalEdge(diamond.join));

    out.appendTo(diamond.join);
    out.ret(allocationTestMix(out, diamond.first, diamond.second));
    compileOrFail(test);

    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, text.verbsOf(diamond.left) == "Store Jump"_s && text.verbsOf(diamond.right) == "Store Jump"_s);
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s) && !text.count("reload"_s) && !text.count("swap"_s));
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().allocation().registersAtHeadOf(diamond.join).size() == 2);
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, diamond.join, diamond.first) == diamond.first->argumentReg());
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, diamond.join, diamond.second) == diamond.second->argumentReg());
    AOT_ALLOCATION_TEST_CHECK(test, !test.procedure().frame().hasFrame());

    constexpr uint64_t second = 9;
    for (uint64_t first : { 4u, 7u }) {
        uint64_t stored = 0;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(first, second, &stored) == allocationTestMixed(first, second));
        AOT_ALLOCATION_TEST_CHECK(test, stored == ((first & 1) ? first : second));
    }
}

static void allocatorGivesSecondEdgeIntoJoinParallelMoves()
{
    TestProcedure test;
    Output& out = test.out();
    AllocationTestDiamond diamond(test);

    out.appendTo(diamond.left);
    out.store64(diamond.first, allocationTestWordAt(test, diamond.buffer, 0));
    out.store64(diamond.second, allocationTestWordAt(test, diamond.buffer, 1));
    out.jump(normalEdge(diamond.join));

    out.appendTo(diamond.right);
    PatchOperation* exchange = out.patchpoint(Void);
    exchange->append(diamond.first, ValueRep::reg(diamond.second->argumentReg()));
    exchange->append(diamond.second, ValueRep::reg(diamond.first->argumentReg()));
    exchange->appendSomeRegister(diamond.buffer);
    exchange->setGenerator([](CCallHelpers& jit, const PatchParameters& parameters) {
        jit.store64(parameters[0].gpr(), CCallHelpers::Address(parameters[2].gpr()));
        jit.store64(parameters[1].gpr(), CCallHelpers::Address(parameters[2].gpr(), sizeof(uint64_t)));
    });
    out.jump(normalEdge(diamond.join));

    out.appendTo(diamond.join);
    out.ret(allocationTestMix(out, diamond.first, diamond.second));
    compileOrFail(test);

    // Whichever arm comes first in the order fixes the head of the join, and the other one has to bring both values back.
    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    bool leftComesFirst = order.of(diamond.left).orderIndex < order.of(diamond.right).orderIndex;
    AOT_ALLOCATION_TEST_CHECK(test, text.verbsOf(leftComesFirst ? diamond.left : diamond.right).endsWith(leftComesFirst ? "Store Jump"_s : "Patch Jump"_s));
    String verbsOfSecondArm = text.verbsOf(leftComesFirst ? diamond.right : diamond.left);
    AOT_ALLOCATION_TEST_CHECK(test, verbsOfSecondArm.endsWith("move Jump"_s) || verbsOfSecondArm.endsWith("swap Jump"_s));
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s) && !text.count("reload"_s));

    const BlockSteps& steps = allocation.stepsOf(leftComesFirst ? diamond.right : diamond.left);
    bool movesFirst = false;
    bool movesSecond = false;
    for (uint32_t index = steps.beginOfEdgeMoves; index < steps.endOfEdgeMoves; ++index) {
        const Step& step = allocation.step(index);
        AOT_ALLOCATION_TEST_CHECK(test, step.kind == StepKind::Move || step.kind == StepKind::Swap);
        movesFirst |= step.operation == diamond.first || step.other == diamond.first;
        movesSecond |= step.operation == diamond.second || step.other == diamond.second;
    }
    AOT_ALLOCATION_TEST_CHECK(test, movesFirst && movesSecond);

    constexpr uint64_t second = 9;
    for (uint64_t first : { 4u, 7u }) {
        std::array<uint64_t, 2> stored { };
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(first, second, stored.data()) == allocationTestMixed(first, second));
        AOT_ALLOCATION_TEST_CHECK(test, stored[0] == first && stored[1] == second);
    }
}

// The arm exchanges two values and fixes the head of the join. The edge that leads straight from the Branch to the join has to exchange them too, so it gets a block, which holds the moves and nothing else.
static void allocatorSplitsEdgeThatGetsMoves()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock entry = out.m_block;
    LBasicBlock arm = out.newBlock();
    LBasicBlock join = out.newBlock();
    LValue first = test.argumentGPR(0);
    LValue second = test.argumentGPR(1);
    LValue buffer = test.argumentGPR(2);
    out.branch(out.testNonZero64(first, 1), normalEdge(arm), normalEdge(join));

    out.appendTo(arm);
    PatchOperation* exchange = out.patchpoint(Void);
    exchange->append(first, ValueRep::reg(second->argumentReg()));
    exchange->append(second, ValueRep::reg(first->argumentReg()));
    exchange->appendSomeRegister(buffer);
    exchange->setGenerator([](CCallHelpers& jit, const PatchParameters& parameters) {
        jit.store64(parameters[0].gpr(), CCallHelpers::Address(parameters[2].gpr()));
        jit.store64(parameters[1].gpr(), CCallHelpers::Address(parameters[2].gpr(), sizeof(uint64_t)));
    });
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.ret(allocationTestMix(out, first, second));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    Block* middle = entry->successor(1).block();
    AOT_ALLOCATION_TEST_CHECK(test, middle != join && order.of(middle).wasSplitLate && middle->size() == 1 && middle->successor(0).block() == join);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(middle).orderIndex == order.of(entry).orderIndex + 1 && order.of(middle).predecessors.size() == 1 && order.of(middle).predecessors.front() == entry);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(join).predecessors.size() == 2 && std::ranges::find(order.of(join).predecessors, middle) != order.of(join).predecessors.end());
    AOT_ALLOCATION_TEST_CHECK(test, order.headOf(middle) >= order.endOf(entry) && order.endOf(middle) <= order.headOf(arm));
    AOT_ALLOCATION_TEST_CHECK(test, text.verbsOf(arm).endsWith("Patch Jump"_s));
    String verbsOfMiddle = text.verbsOf(middle);
    AOT_ALLOCATION_TEST_CHECK(test, verbsOfMiddle.endsWith("move Jump"_s) || verbsOfMiddle.endsWith("swap Jump"_s));
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s) && !text.count("reload"_s));
    const BlockSteps& steps = allocation.stepsOf(middle);
    AOT_ALLOCATION_TEST_CHECK(test, steps.begin == steps.end && steps.beginOfEdgeMoves < steps.endOfEdgeMoves);

    constexpr uint64_t secondArgument = 9;
    for (uint64_t firstArgument : { 4u, 7u }) {
        std::array<uint64_t, 2> stored { };
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(firstArgument, secondArgument, stored.data()) == allocationTestMixed(firstArgument, secondArgument));
        AOT_ALLOCATION_TEST_CHECK(test, stored[0] == ((firstArgument & 1) ? firstArgument : 0) && stored[1] == ((firstArgument & 1) ? secondArgument : 0));
    }
}

// Two Patches of one kind hold two operands to each other's registers. The one in the rare block finds them where they lie and is told where they belong. The other stands in a loop and gets them exchanged.
static void allocatorLeavesOperandsOfRarePatchWhereTheyLie()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock rare = out.newBlock();
    LBasicBlock usual = out.newBlock();
    LBasicBlock behindLoop = out.newBlock();
    LValue buffer = test.argumentGPR(0);
    // In a rare region whatever is read further on in the order counts as live, so each Patch has operands of its own.
    LValue operands[2][2] { { test.argumentGPR(1), test.argumentGPR(2) }, { test.argumentGPR(3), test.argumentGPR(4) } };
    out.branch(out.testNonZero64(buffer, 8), rareEdge(rare), normalEdge(usual));

    PatchOperation* patches[2];
    LBasicBlock blocks[2] { rare, usual };
    for (unsigned index = 0; index < 2; ++index) {
        out.appendTo(blocks[index]);
        PatchOperation* patch = out.patchpoint(Void);
        patches[index] = patch;
        patch->append(operands[index][0], ValueRep::reg(operands[index][1]->argumentReg()));
        patch->append(operands[index][1], ValueRep::reg(operands[index][0]->argumentReg()));
        patch->append(buffer, ValueRep::reg(buffer->argumentReg()));
        patch->setBringsOperandsToTheirRegisters(true);
        patch->setGenerator([](CCallHelpers& jit, const PatchParameters& parameters) {
            jit.store64(parameters[0].gpr(), CCallHelpers::Address(parameters[2].gpr()));
            jit.store64(parameters[1].gpr(), CCallHelpers::Address(parameters[2].gpr(), sizeof(uint64_t)));
        });
        // No address of a word has the bit.
        if (blocks[index] == usual) {
            out.branch(out.testNonZero64(buffer, 4), normalEdge(usual), normalEdge(behindLoop));
            out.appendTo(behindLoop);
        }
        out.ret(out.constInt64(index));
    }
    compileOrFail(test);

    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().order().of(rare).isRare() && !test.procedure().order().of(usual).isRare() && test.procedure().order().of(usual).loop != noLoop);
    AOT_ALLOCATION_TEST_CHECK(test, text.verbsOf(rare).startsWith("Patch"_s));
    AOT_ALLOCATION_TEST_CHECK(test, text.verbsOf(usual).startsWith("swap Patch"_s) || text.verbsOf(usual).startsWith("move move Patch"_s) || text.verbsOf(usual).startsWith("move move move Patch"_s));
    AOT_ALLOCATION_TEST_CHECK(test, patches[0]->registerToBringOperandTo(0) == operands[0][1]->argumentReg() && patches[0]->registerToBringOperandTo(1) == operands[0][0]->argumentReg() && patches[0]->registerToBringOperandTo(2) == buffer->argumentReg());
    AOT_ALLOCATION_TEST_CHECK(test, patches[0]->constraintOf(0).kind() == ValueRep::ColdAny && patches[0]->constraintOf(1).kind() == ValueRep::ColdAny);
    AOT_ALLOCATION_TEST_CHECK(test, patches[0]->lateClobberedMask() == (maskOf(operands[0][0]->argumentReg()) | maskOf(operands[0][1]->argumentReg())));
    AOT_ALLOCATION_TEST_CHECK(test, !patches[1]->registerToBringOperandTo(0) && patches[1]->constraintOf(0).isReg() && !patches[1]->lateClobberedMask());

    alignas(16) std::array<uint64_t, 4> stored { };
    for (unsigned index = 0; index < 2; ++index) {
        uint64_t* buffer = stored.data() + (index ? 0 : 1);
        AOT_ALLOCATION_TEST_CHECK(test, !(reinterpret_cast<uintptr_t>(buffer) & 8) == !!index);
        stored = { };
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(buffer, 11, 12, 13, 14) == index);
        AOT_ALLOCATION_TEST_CHECK(test, buffer[0] == (index ? 13u : 11u) && buffer[1] == (index ? 14u : 12u));
    }
}

// The Patch stands in no loop and in no rare block. A constant is told to the generator and made nowhere.
static void allocatorLeavesOperandsOfPatchThatEndsFunctionWhereTheyLie()
{
    constexpr int64_t constant = 0x123456789;
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock ends = out.newBlock();
    LBasicBlock goesOn = out.newBlock();
    LValue buffer = test.argumentGPR(0);
    LValue operands[2] { test.argumentGPR(1), test.argumentGPR(2) };
    out.branch(out.testNonZero64(buffer, 8), normalEdge(ends), normalEdge(goesOn));

    out.appendTo(ends);
    PatchOperation* patch = out.patchpoint(Void);
    patch->append(operands[0], ValueRep::reg(operands[1]->argumentReg()));
    patch->append(out.constInt64(constant), ValueRep::reg(operands[0]->argumentReg()));
    patch->setBringsOperandsToTheirRegisters(true);
    patch->setGenerator([](CCallHelpers& jit, const PatchParameters&) {
        jit.breakpoint();
    });
    out.unreachable();

    out.appendTo(goesOn);
    out.ret(operands[1]);
    compileOrFail(test);

    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, !test.procedure().order().of(ends).isRare());
    AOT_ALLOCATION_TEST_CHECK(test, text.verbsOf(ends).startsWith("Patch"_s));
    AOT_ALLOCATION_TEST_CHECK(test, patch->registerToBringOperandTo(0) == operands[1]->argumentReg() && patch->registerToBringOperandTo(1) == operands[0]->argumentReg());
    AOT_ALLOCATION_TEST_CHECK(test, patch->constraintOf(0).kind() == ValueRep::ColdAny && patch->constraintOf(1).kind() == ValueRep::ColdAny);
    alignas(16) std::array<uint64_t, 2> words { };
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(words.data(), 11, 12) == 12);
}

// The constant lies in the register where the second Patch takes it, and is read there for the last time. The generator is told a constant and makes it anew, so nothing else may move into that register.
static void allocatorKeepsNothingWhereGeneratorMakesConstant()
{
    constexpr int64_t constant = 0x123456789;
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    LBasicBlock rare = out.newBlock();
    LBasicBlock usual = out.newBlock();
    LValue flag = test.argumentGPR(0);
    LValue kept = test.argumentGPR(1);
    Reg destination = test.argumentGPR(2)->argumentReg();
    RegisterMask others = file.allocatableIn(Bank::GP) & ~maskOf(destination);
    LValue made = out.constInt64(constant);

    PatchOperation* first = out.patchpoint(Void);
    first->appendSomeRegister(made);
    first->clobberEarly(others & ~maskOf(flag->argumentReg()) & ~maskOf(kept->argumentReg()));
    first->effects = Effects::none();
    first->effects.writesLocalState = true;
    first->setGenerator([destination](CCallHelpers&, const PatchParameters& parameters) {
        RELEASE_ASSERT(parameters[0].reg() == destination);
    });
    out.branch(out.testNonZero64(flag, 1), rareEdge(rare), normalEdge(usual));

    out.appendTo(rare);
    PatchOperation* patch = out.patchpoint(Void);
    patch->append(made, ValueRep::reg(destination));
    patch->setBringsOperandsToTheirRegisters(true);
    patch->clobberLate(others);
    patch->effects = Effects::none();
    patch->effects.writesLocalState = true;
    patch->setGenerator([destination](CCallHelpers& jit, const PatchParameters& parameters) {
        RELEASE_ASSERT(parameters[0].isConstant() && parameters[0].value() == constant);
        jit.move(CCallHelpers::TrustedImm64(constant), destination.gpr());
    });
    out.ret(kept);

    out.appendTo(usual);
    out.ret(out.constInt64(0));
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().order().of(rare).isRare() && patch->registerToBringOperandTo(0) == destination);
    AOT_ALLOCATION_TEST_CHECK(test, patch->lateClobberedMask() & maskOf(destination));
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(1, 42, 0) == 42);
    AOT_ALLOCATION_TEST_CHECK(test, !test.call<uint64_t>(0, 42, 0));
}

// The result is read in a register that the Patch may not give it, so it gets one of those that it may, and is moved.
static void allocatorHoldsResultOfPatchToItsRegisters()
{
    TestProcedure test;
    Output& out = test.out();
    LValue first = test.argumentGPR(0);
    Reg wanted = test.argumentGPR(1)->argumentReg();
    Reg allowed[2] { test.argumentGPR(2)->argumentReg(), test.argumentGPR(3)->argumentReg() };

    for (ValueRep::Kind kind : { ValueRep::SomeRegister, ValueRep::SomeEarlyRegister }) {
        PatchOperation* patch = out.patchpoint(Int64);
        patch->appendSomeRegister(first);
        patch->resultConstraints = { ValueRep(kind) };
        patch->holdResultsTo(RegisterSet(allowed[0], allowed[1]));
        patch->effects = Effects::none();
        patch->effects.writesLocalState = true;
        patch->setGenerator([allowed = RegisterSet(allowed[0], allowed[1])](CCallHelpers& jit, const PatchParameters& parameters) {
            RELEASE_ASSERT(allowed.contains(parameters[0].reg(), IgnoreVectors));
            jit.add64(CCallHelpers::TrustedImm32(1), parameters[1].gpr(), parameters[0].gpr());
        });
        PatchOperation* reader = out.patchpoint(Int64);
        reader->append(patch, ValueRep::reg(wanted));
        reader->effects = Effects::none();
        reader->effects.writesLocalState = true;
        reader->setGenerator([](CCallHelpers& jit, const PatchParameters& parameters) {
            jit.move(parameters[1].gpr(), parameters[0].gpr());
        });
        first = reader;
    }
    out.ret(first);
    compileOrFail(test);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(40, 0, 0, 0) == 42);
}

static unsigned allocationTestNumberOfBlocksSplitLate(TestProcedure& test)
{
    const Order& order = test.procedure().order();
    return static_cast<unsigned>(std::ranges::count_if(order.blocksInOrder(), [&](const Block* block) {
        return order.of(block).wasSplitLate;
    }));
}

// Holds the value to the register and leaves it nowhere else.
static void allocationTestAppendPatchThatBrings(TestProcedure& test, LValue value, Reg wanted, RegisterMask elsewhere)
{
    PatchOperation* patch = test.out().patchpoint(Void);
    patch->append(value, ValueRep::reg(wanted));
    patch->clobberLate(elsewhere & ~maskOf(wanted));
    patch->setGenerator([](CCallHelpers&, const PatchParameters&) { });
}

// The blocks that a Switch leads to, one for each case. The validator finds dominators by going up from a block, so a test of growth has no long row of blocks.
static Vector<LBasicBlock> allocationTestAppendSwitch(TestProcedure& test, LValue selector, unsigned numberOfCases)
{
    Output& out = test.out();
    Vector<LBasicBlock> targets;
    Vector<SwitchCase> cases;
    for (unsigned number = 0; number < numberOfCases; ++number) {
        targets.append(out.newBlock());
        cases.append(SwitchCase { static_cast<int64_t>(number), normalEdge(targets.last()) });
    }
    LBasicBlock otherwise = out.newBlock();
    out.switchOn(selector, cases.span(), normalEdge(otherwise));
    out.appendTo(otherwise);
    out.ret(selector);
    return targets;
}

// Many blocks, each of which may leave for one block that they share. What is read there lies now in one register, now in another, so every other edge gets a move and with it a block.
static std::optional<uint64_t> allocationTestInstructionsForEdgesWithMovesIntoOneBlock(unsigned numberOfEdges)
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock shared = out.newBlock();
    LValue selector = test.argumentGPR(0);
    LValue value = test.argumentGPR(1);
    Reg registers[2] { test.argumentGPR(2)->argumentReg(), test.argumentGPR(3)->argumentReg() };
    RegisterMask elsewhere = maskOf(value->argumentReg()) | maskOf(registers[0]) | maskOf(registers[1]);
    Vector<LBasicBlock> blocks = allocationTestAppendSwitch(test, selector, numberOfEdges);
    for (unsigned number = 0; number < numberOfEdges; ++number) {
        out.appendTo(blocks[number]);
        allocationTestAppendPatchThatBrings(test, value, registers[number % 2], elsewhere);
        LBasicBlock returns = out.newBlock();
        out.branch(out.testNonZero64(value, 1), normalEdge(shared), normalEdge(returns));
        out.appendTo(returns);
        out.ret(out.constInt64(number));
    }
    out.appendTo(shared);
    out.ret(value);
    std::optional<uint64_t> instructions = instructionsOfCurrentThreadIn([&] {
        prepareOrFail(test);
    });
    AOT_TEST_CHECK(test.procedure().order().of(shared).predecessors.size() == numberOfEdges);
    AOT_TEST_CHECK(allocationTestNumberOfBlocksSplitLate(test) >= numberOfEdges / 2 - 1);
    return instructions;
}

static void allocatorWorkGrowsAsEdgesWithMovesIntoOneBlockDo()
{
    constexpr unsigned numberOfEdges = 10000;
    std::optional<uint64_t> smaller = allocationTestInstructionsForEdgesWithMovesIntoOneBlock(numberOfEdges);
    std::optional<uint64_t> large = allocationTestInstructionsForEdgesWithMovesIntoOneBlock(4 * numberOfEdges);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

// Guards that leave for one Return, ahead of a part with a call, which alone gets the frame.
static std::optional<uint64_t> allocationTestInstructionsForGuardsAheadOfFramedPart(unsigned numberOfGuards)
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock shared = out.newBlock();
    LBasicBlock behindGuards = out.newBlock();
    LValue selector = test.argumentGPR(0);
    LValue value = test.argumentGPR(1);
    Vector<LBasicBlock> guards = allocationTestAppendSwitch(test, selector, numberOfGuards);
    for (unsigned number = 0; number < numberOfGuards; ++number) {
        out.appendTo(guards[number]);
        out.branch(out.testNonZero64(value, number + 1), normalEdge(shared), normalEdge(behindGuards));
    }
    out.appendTo(behindGuards);
    allocationTestAppendCall(test, nullptr);
    out.ret(selector);
    out.appendTo(shared);
    out.ret(value);
    std::optional<uint64_t> instructions = instructionsOfCurrentThreadIn([&] {
        prepareOrFail(test);
    });
    AOT_TEST_CHECK(test.procedure().order().of(shared).predecessors.size() == numberOfGuards);
    // On ARM64 the whole function has the frame.
    if (isX86_64())
        AOT_TEST_CHECK(test.procedure().allocation().onlyHeadOfFramedPart() == behindGuards && test.procedure().frame().coverage() == FrameCoverage::FramedPart);
    return instructions;
}

static void allocatorWorkGrowsAsGuardsAheadOfFramedPartDo()
{
    constexpr unsigned numberOfGuards = 5000;
    std::optional<uint64_t> smaller = allocationTestInstructionsForGuardsAheadOfFramedPart(numberOfGuards);
    std::optional<uint64_t> large = allocationTestInstructionsForGuardsAheadOfFramedPart(4 * numberOfGuards);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

// Two Switches lead to the same blocks. The first fixes their heads. At the second a value lies elsewhere, so each of its edges gets a move and with it a block.
static std::optional<uint64_t> allocationTestInstructionsForSwitchWhoseEdgesGetMoves(unsigned numberOfCases)
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock first = out.newBlock();
    LBasicBlock second = out.newBlock();
    LBasicBlock otherwise = out.newBlock();
    LValue selector = test.argumentGPR(0);
    LValue value = test.argumentGPR(1);
    Reg elsewhere = test.argumentGPR(2)->argumentReg();
    Vector<SwitchCase> cases;
    Vector<LBasicBlock> targets;
    for (unsigned number = 0; number < numberOfCases; ++number) {
        targets.append(out.newBlock());
        cases.append(SwitchCase { static_cast<int64_t>(number), normalEdge(targets.last()) });
    }
    out.branch(out.testNonZero64(value, 1), normalEdge(first), normalEdge(second));
    out.appendTo(first);
    out.switchOn(selector, cases.span(), normalEdge(otherwise));
    out.appendTo(second);
    allocationTestAppendPatchThatBrings(test, value, elsewhere, maskOf(value->argumentReg()));
    out.switchOn(selector, cases.span(), normalEdge(otherwise));
    for (unsigned number = 0; number < numberOfCases; ++number) {
        out.appendTo(targets[number]);
        out.ret(out.add(value, out.constInt64(number)));
    }
    out.appendTo(otherwise);
    out.ret(selector);
    std::optional<uint64_t> instructions = instructionsOfCurrentThreadIn([&] {
        prepareOrFail(test);
    });
    AOT_TEST_CHECK(allocationTestNumberOfBlocksSplitLate(test) >= numberOfCases);
    return instructions;
}

static void allocatorWorkGrowsAsEdgesOfSwitchThatGetMovesDo()
{
    constexpr unsigned numberOfCases = 5000;
    std::optional<uint64_t> smaller = allocationTestInstructionsForSwitchWhoseEdgesGetMoves(numberOfCases);
    std::optional<uint64_t> large = allocationTestInstructionsForSwitchWhoseEdgesGetMoves(4 * numberOfCases);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

// An operand under SomeRegister might be given the register that the generator writes for another operand before it reads this one. So the operands of such a Patch stay held to their registers.
static void allocatorHoldsOperandsToTheirRegistersBesideOneThatTakesSomeRegister()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock rare = out.newBlock();
    LBasicBlock usual = out.newBlock();
    LValue buffer = test.argumentGPR(0);
    LValue first = test.argumentGPR(1);
    LValue second = test.argumentGPR(2);
    out.branch(out.testNonZero64(buffer, 8), rareEdge(rare), normalEdge(usual));

    out.appendTo(rare);
    PatchOperation* patch = out.patchpoint(Void);
    patch->append(first, ValueRep::reg(second->argumentReg()));
    patch->append(second, ValueRep::reg(first->argumentReg()));
    patch->appendSomeRegister(buffer);
    patch->setBringsOperandsToTheirRegisters(true);
    patch->setGenerator([](CCallHelpers& jit, const PatchParameters& parameters) {
        jit.store64(parameters[0].gpr(), CCallHelpers::Address(parameters[2].gpr()));
        jit.store64(parameters[1].gpr(), CCallHelpers::Address(parameters[2].gpr(), sizeof(uint64_t)));
    });
    out.ret(out.constInt64(1));
    out.appendTo(usual);
    out.ret(out.constInt64(0));
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().order().of(rare).isRare());
    AOT_ALLOCATION_TEST_CHECK(test, patch->constraintOf(0) == ValueRep::reg(second->argumentReg()) && patch->constraintOf(1) == ValueRep::reg(first->argumentReg()));
    AOT_ALLOCATION_TEST_CHECK(test, !patch->registerToBringOperandTo(0) && !patch->registerToBringOperandTo(1) && !patch->lateClobberedMask());

    alignas(16) std::array<uint64_t, 4> stored { };
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(stored.data() + 1, 11, 12) == 1 && stored[1] == 11 && stored[2] == 12);
    AOT_ALLOCATION_TEST_CHECK(test, !test.call<uint64_t>(stored.data(), 11, 12) && !stored[0]);
}

static void allocatorKeepsPhiApartFromValueThatLivesOn()
{
    constexpr int64_t constant = 7;
    TestProcedure test;
    Output& out = test.out();
    AllocationTestDiamond diamond(test);
    LValue phi = out.phiIn(diamond.join, Int64);

    out.appendTo(diamond.left);
    out.upsilon(diamond.second, phi);
    out.jump(normalEdge(diamond.join));

    out.appendTo(diamond.right);
    out.upsilon(out.constInt64(constant), phi);
    out.jump(normalEdge(diamond.join));

    out.appendTo(diamond.join);
    out.ret(allocationTestMix(out, phi, diamond.second));
    compileOrFail(test);

    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, text.verbsOf(diamond.left) == "move Jump"_s && text.verbsOf(diamond.right) == "materialize Jump"_s);
    Reg registerOfPhi = allocationTestRegisterAtHead(test, diamond.join, phi);
    AOT_ALLOCATION_TEST_CHECK(test, registerOfPhi && registerOfPhi != allocationTestRegisterAtHead(test, diamond.join, diamond.second));

    constexpr uint64_t second = 9;
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 7 }, second) == allocationTestMixed(second, second));
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 4 }, second) == allocationTestMixed(static_cast<uint64_t>(constant), second));
}

// Each Phi of the loop takes the value of the next one on the way back, and the last that of the first.
template<typename Word, unsigned length>
static void allocatorRotatesPhisBySwapping()
{
    constexpr Type type = allocationTestTypeOf<Word>;
    TestProcedure test;
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    Vector<LValue> starts = allocationTestLoadWords(test, test.argumentGPR(1), 0, length, type);
    LBasicBlock loop = out.newBlock();
    LBasicBlock exit = out.newBlock();

    LValue counter = out.phiIn(loop, Int64);
    Vector<LValue> phis;
    for (LValue start : starts) {
        phis.append(out.phiIn(loop, type));
        out.upsilon(start, phis.last());
    }
    out.upsilon(out.int64Zero, counter);
    out.jump(normalEdge(loop));

    out.appendTo(loop);
    LValue nextCounter = out.add(counter, 1);
    for (unsigned index = 0; index < length; ++index)
        out.upsilon(phis[(index + 1) % length], phis[index]);
    out.upsilon(nextCounter, counter);
    out.branch(out.lessThan(nextCounter, rounds), normalEdge(loop), normalEdge(exit));

    out.appendTo(exit);
    out.ret(allocationTestMixAll(out, phis.span()));
    compileOrFail(test);

    AllocationTestText text(test);
    Block* sourceOfBackEdge = allocationTestSourceOfBackEdge(test.procedure().order(), loop);
    AOT_ALLOCATION_TEST_CHECK(test, sourceOfBackEdge);
    AOT_ALLOCATION_TEST_CHECK(test, text.count("swap"_s) == length - 1 && text.count("swap"_s, nullptr, sourceOfBackEdge) == length - 1);
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s) && !text.count("reload"_s));
    for (const Step& step : test.procedure().allocation().steps()) {
        if (step.kind == StepKind::Swap)
            AOT_ALLOCATION_TEST_CHECK(test, step.operation != step.other && phis.contains(step.operation) && phis.contains(step.other));
    }

    std::array<Word, 64> words = allocationTestWords<Word>();
    for (uint64_t numberOfRounds : { 0u, 1u, 2u, 3u, 4u, 7u }) {
        std::array<Word, length> rotated;
        std::copy_n(words.begin(), length, rotated.begin());
        for (uint64_t round = 1; round < numberOfRounds; ++round)
            std::rotate(rotated.begin(), rotated.begin() + 1, rotated.end());
        AOT_ALLOCATION_TEST_CHECK(test, test.call<Word>(numberOfRounds, words.data()) == allocationTestMixedAll(std::span<const Word>(rotated)));
    }
}

// Every value is read twice, once on the way up and once on the way down, so all of them are live at once.
static Vector<LValue> allocationTestBuildPressure(TestProcedure& test, unsigned count, Type type)
{
    Output& out = test.out();
    Vector<LValue> values = allocationTestLoadWords(test, test.argumentGPR(0), 0, count, type);
    Vector<LValue> reversed = values;
    reversed.reverse();
    LValue forward = allocationTestMixAll(out, values.span());
    out.ret(allocationTestMix(out, forward, allocationTestMixAll(out, reversed.span())));
    return values;
}

template<typename Word>
static void allocatorStoresSpilledValueOnceBehindItsDefinition()
{
    constexpr Type type = allocationTestTypeOf<Word>;
    constexpr unsigned numberBeyondRegisters = 4;
    TestProcedure test;
    unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).allocatableIn(bankOf(type))) + numberBeyondRegisters;
    Vector<LValue> values = allocationTestBuildPressure(test, count, type);
    compileOrFail(test);

    TestProcedure sameProcedure;
    allocationTestBuildPressure(sameProcedure, count, type);
    compileOrFail(sameProcedure);
    AOT_ALLOCATION_TEST_CHECK(test, test.printedAllocation() == sameProcedure.printedAllocation());

    AllocationTestText text(test);
    const Allocation& allocation = test.procedure().allocation();
    for (const Step& step : allocation.steps())
        AOT_ALLOCATION_TEST_CHECK(test, step.kind != StepKind::Save);

    unsigned numberSpilled = 0;
    for (unsigned index = 0; index < count; ++index) {
        LValue value = values[index];
        unsigned numberOfReloads = text.count("reload"_s, value);
        if (!allocation.isSpilled(value)) {
            AOT_ALLOCATION_TEST_CHECK(test, !numberOfReloads && !text.count("save"_s, value));
            continue;
        }
        ++numberSpilled;
        AOT_ALLOCATION_TEST_CHECK(test, text.isStoredOnceBehindDefinition(value));
        AOT_ALLOCATION_TEST_CHECK(test, !allocation.spills()[allocation.spillOf(value)].isSaveInRareRegion);
        unsigned numberOfReadsInPlace = allocationTestReadsInPlace(test, value);
        AOT_ALLOCATION_TEST_CHECK(test, numberOfReloads + numberOfReadsInPlace);
        // The first and the last value begin a sum, where they are doubled. Every other value is only ever subtracted.
        if (!isX86_64())
            AOT_ALLOCATION_TEST_CHECK(test, !numberOfReadsInPlace);
        else if (index && index + 1 < count)
            AOT_ALLOCATION_TEST_CHECK(test, !numberOfReloads);
    }
    AOT_ALLOCATION_TEST_CHECK(test, numberSpilled >= numberBeyondRegisters);
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().frame().coverage() == FrameCoverage::WholeFunction);

    std::array<Word, 64> words = allocationTestWords<Word>();
    std::span<const Word> wordsRead = std::span<const Word>(words).first(count);
    std::array<Word, 64> reversed { };
    std::reverse_copy(wordsRead.begin(), wordsRead.end(), reversed.begin());
    Word expected = allocationTestMixed(allocationTestMixedAll(wordsRead), allocationTestMixedAll(std::span<const Word>(reversed).first(count)));
    AOT_ALLOCATION_TEST_CHECK(test, test.call<Word>(words.data()) == expected);
}

template<typename Word>
static void allocatorKeepsValuesAcrossCallInCalleeSavedRegisters()
{
    constexpr Type type = allocationTestTypeOf<Word>;
    for (bool areMoreThanRegisters : { false, true }) {
        TestProcedure test;
        Output& out = test.out();
        RegisterFile file = registerFileFor(test.procedure().traits());
        unsigned numberOfCalleeSaves = allocationTestCountOf(file.calleeSavedIn(bankOf(type)));
        unsigned count = areMoreThanRegisters ? numberOfCalleeSaves + 2 : 1;

        LValue buffer = test.argumentGPR(0);
        LValue counter = test.argumentGPR(1);
        Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, type);
        allocationTestAppendCall(test, counter);
        out.ret(allocationTestMixAll(out, values.span()));
        compileOrFail(test);

        AllocationTestText text(test);
        const Allocation& allocation = test.procedure().allocation();
        RegisterMask taken = 0;
        for (LValue value : values) {
            AOT_ALLOCATION_TEST_CHECK(test, test.procedure().liveness().of(value).has(ValueFlag::LivesAcrossCall));
            if (allocation.isSpilled(value)) {
                AOT_ALLOCATION_TEST_CHECK(test, text.isStoredOnceBehindDefinition(value));
                AOT_ALLOCATION_TEST_CHECK(test, text.count("reload"_s, value) + allocationTestReadsInPlace(test, value));
                continue;
            }
            Reg reg = allocation.registerAtDefinitionOf(value);
            AOT_ALLOCATION_TEST_CHECK(test, reg && (maskOf(reg) & file.calleeSaved) && !(maskOf(reg) & taken));
            AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s, value) && !text.count("reload"_s, value));
            taken |= maskOf(reg);
        }
        AOT_ALLOCATION_TEST_CHECK(test, allocationTestCountOf(taken) == std::min(count, numberOfCalleeSaves));
        AOT_ALLOCATION_TEST_CHECK(test, allocation.usedCalleeSaves() == taken && text.mentions("usedCalleeSaves"_s) == !!taken);
        AOT_ALLOCATION_TEST_CHECK(test, (test.procedure().frame().calleeSavesMask() & taken) == taken);
        AOT_ALLOCATION_TEST_CHECK(test, test.procedure().frame().coverage() == FrameCoverage::WholeFunction);

        std::array<Word, 64> words = allocationTestWords<Word>();
        uint64_t numberOfCalls = 0;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<Word>(words.data(), &numberOfCalls) == allocationTestMixedAll(std::span<const Word>(words).first(count)));
        AOT_ALLOCATION_TEST_CHECK(test, numberOfCalls == 1);
    }
}

// `through` is defined ahead of the loop and read behind it.
template<typename Word, bool isUnderPressure>
static void allocatorDecidesByPressureWhatKeepsItsRegisterThroughLoop()
{
    constexpr Type type = allocationTestTypeOf<Word>;
    TestProcedure test;
    Output& out = test.out();
    unsigned numberOfRegisters = allocationTestCountOf(registerFileFor(test.procedure().traits()).allocatableIn(bankOf(type)));
    unsigned numberReadInLoop = numberOfRegisters + 4;

    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LValue through = out.load(allocationTestWordAt(test, buffer, 0), type);
    LValue start = out.load(allocationTestWordAt(test, buffer, 1), type);
    LBasicBlock loop = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue counter = out.phiIn(loop, Int64);
    LValue sum = out.phiIn(loop, type);
    out.upsilon(out.int64Zero, counter);
    out.upsilon(start, sum);
    out.jump(normalEdge(loop));

    out.appendTo(loop);
    LValue addend = sum;
    if constexpr (isUnderPressure)
        addend = allocationTestMixAll(out, allocationTestLoadWords(test, buffer, 2, numberReadInLoop, type).span());
    LValue nextSum = out.add(sum, addend);
    LValue nextCounter = out.add(counter, 1);
    out.upsilon(nextCounter, counter);
    out.upsilon(nextSum, sum);
    out.branch(out.lessThan(nextCounter, rounds), normalEdge(loop), normalEdge(exit));

    out.appendTo(exit);
    out.ret(allocationTestMix(out, nextSum, through));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(loop).isLoopHead && order.of(exit).loop == noLoop);
    const Loop& interval = order.loops()[order.of(loop).loop];
    unsigned pressure = test.procedure().liveness().of(interval, order).pressure[static_cast<unsigned>(bankOf(type))];
    AOT_ALLOCATION_TEST_CHECK(test, (pressure >= numberOfRegisters) == isUnderPressure);
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().allocation().isSpilled(through) == isUnderPressure);
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, loop, through).isSet() != isUnderPressure);
    if constexpr (isUnderPressure) {
        AOT_ALLOCATION_TEST_CHECK(test, text.isStoredOnceBehindDefinition(through));
        AOT_ALLOCATION_TEST_CHECK(test, text.count("reload"_s, through) == text.count("reload"_s, through, exit));
        AOT_ALLOCATION_TEST_CHECK(test, text.count("reload"_s, through) + allocationTestReadsInPlace(test, through) == 1);
    } else
        AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s, through) && !text.count("reload"_s, through));

    std::array<Word, 64> words = allocationTestWords<Word>();
    Word addedInEachRound = allocationTestMixedAll(std::span<const Word>(words).subspan(2, numberReadInLoop));
    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        Word expected = words[1];
        for (uint64_t round = 0; round < std::max<uint64_t>(numberOfRounds, 1); ++round)
            expected += isUnderPressure ? addedInEachRound : expected;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<Word>(numberOfRounds, words.data()) == allocationTestMixed(expected, words[0]));
    }
}

// for (; left; left--) sum += ..; return sum. The loop is left at its head alone, so in the body nothing needs the Phi behind the addition, though the read behind the loop stands further on in the order.
template<typename Word, bool sumIsFirstChild>
static void allocatorBringsSumThatIsReadBehindLoopRoundInItsRegister()
{
    constexpr Type type = allocationTestTypeOf<Word>;
    TestProcedure test;
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LValue start = out.load(allocationTestWordAt(test, buffer, 0), type);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, type);
    out.upsilon(rounds, left);
    out.upsilon(start, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    LValue addend = allocationTestMixAll(out, allocationTestLoadWords(test, buffer, 1, 2, type).span());
    LValue nextSum = sumIsFirstChild ? out.add(sum, addend) : out.add(addend, sum);
    out.upsilon(nextSum, sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.store(sum, allocationTestWordAt(test, buffer, 3));
    out.ret(rounds);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(head).isLoopHead && order.of(body).loop == order.of(head).loop && order.headOf(body) < order.headOf(exit));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(sum) && allocation.registerAtDefinitionOf(nextSum) == allocation.registerAtDefinitionOf(sum));
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("move"_s, nextSum) && !text.count("move"_s, sum) && !text.count("swap"_s));
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s) && !text.count("reload"_s));

    std::array<Word, 64> words = allocationTestWords<Word>();
    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        Word expected = words[0];
        for (uint64_t round = 0; round < numberOfRounds; ++round)
            expected += allocationTestMixed(words[1], words[2]);
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, words.data()) == numberOfRounds);
        AOT_ALLOCATION_TEST_CHECK(test, words[3] == expected);
    }
}

// Nothing comes round at a join that a rare block alone leads back to, so what only lives through it stays where it is. The call further on moves it to a register that the callee saves.
static void allocatorSpillsNothingAheadOfJoinThatRareBlockLeadsBackTo()
{
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    LValue start = test.argumentGPR(0);
    LValue kept = test.argumentGPR(1);
    LValue buffer = test.argumentGPR(2);
    LBasicBlock join = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock behind = out.newBlock();
    LValue word = out.phiIn(join, Int64);
    out.upsilon(start, word);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.branch(out.testNonZero64(word, 2), rareEdge(slow), normalEdge(behind));

    out.appendTo(slow);
    out.upsilon(out.sub(word, 2), word);
    out.jump(normalEdge(join));

    out.appendTo(behind);
    LValue doubled = out.add(word, word);
    PatchOperation* call = allocationTestAppendCall(test, nullptr);
    out.store64(kept, allocationTestWordAt(test, buffer, 0));
    out.ret(out.add(kept, 1));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Liveness& liveness = test.procedure().liveness();
    const Allocation& allocation = test.procedure().allocation();
    UNUSED_PARAM(doubled);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(join).isLoopHead && order.of(slow).isRare() && order.headOf(join) < order.headOf(slow) && order.headOf(slow) < order.headOf(behind));
    for (LValue value : { kept, buffer }) {
        AOT_ALLOCATION_TEST_CHECK(test, liveness.of(value).has(ValueFlag::LivesAcrossCall) && liveness.of(value).lastUse > order.earlyPositionOf(call));
        AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(value) && !(maskOf(allocation.registerAtDefinitionOf(value)) & file.calleeSaved));
        AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(value) && allocationTestRegisterAtHead(test, join, value) == allocation.registerAtDefinitionOf(value));
        AOT_ALLOCATION_TEST_CHECK(test, text.count("move"_s, value) == 1 && !text.count("save"_s, value) && !text.count("reload"_s, value));
    }

    for (uint64_t first : { 0u, 1u, 2u, 7u }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        std::array<uint64_t, 64> expected = words;
        expected[0] = 41;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(first, uint64_t { 41 }, words.data()) == 42);
        AOT_ALLOCATION_TEST_CHECK(test, words == expected);
    }
}

// Behind the addition the body has a join that a rare block behind it leads back to, which the order takes for a loop. Its head is judged as the block of the addition is, so it holds no sum of the outer head,
// and the sum is dead behind the addition as in a body without such a join.
static void allocatorBringsSumRoundInItsRegisterPastJoinThatRareBlockLeadsBackTo()
{
    TestProcedure test;
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, Int64);
    LValue word = out.phiIn(join, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    LValue nextSum = out.add(sum, left);
    out.upsilon(left, word);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.branch(out.testNonZero64(word, 2), rareEdge(slow), normalEdge(latch));

    out.appendTo(slow);
    out.upsilon(out.sub(word, 2), word);
    out.jump(normalEdge(join));

    out.appendTo(latch);
    out.store64(word, allocationTestWordAt(test, buffer, 1));
    out.upsilon(nextSum, sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.store64(sum, allocationTestWordAt(test, buffer, 0));
    out.ret(rounds);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(head).isLoopHead && order.of(join).isLoopHead && order.loops()[order.of(join).loop].parent == order.of(head).loop);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(slow).isRare() && order.headOf(body) < order.headOf(join) && order.headOf(join) < order.headOf(slow));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(sum) && allocation.registerAtDefinitionOf(nextSum) == allocation.registerAtDefinitionOf(sum));
    AOT_ALLOCATION_TEST_CHECK(test, !allocationTestRegisterAtHead(test, join, sum) && allocationTestRegisterAtHead(test, join, nextSum));
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("move"_s, nextSum) && !text.count("move"_s, sum) && !text.count("save"_s) && !text.count("reload"_s));

    for (uint64_t numberOfRounds : { 0u, 1u, 3u, 6u }) {
        std::array<uint64_t, 2> stored { ~uint64_t { 0 }, 0 };
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, stored.data()) == numberOfRounds);
        AOT_ALLOCATION_TEST_CHECK(test, stored[0] == numberOfRounds * (numberOfRounds + 1) / 2 && stored[1] == (numberOfRounds ? 1u : 0u));
    }
}

// The body has a way out of its own behind the addition, and what stands there reads the sum as it was at the head.
static void allocatorKeepsPhiThatLaterWayOutOfLoopReads()
{
    TestProcedure test;
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    LValue limit = test.argumentGPR(1);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LBasicBlock early = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    LValue nextSum = out.add(sum, left);
    out.branch(out.lessThan(nextSum, limit), normalEdge(latch), normalEdge(early));

    out.appendTo(latch);
    out.upsilon(nextSum, sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.ret(sum);

    out.appendTo(early);
    out.ret(allocationTestMix(out, sum, nextSum));
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(nextSum) != allocation.registerAtDefinitionOf(sum));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, early, sum) == allocation.registerAtDefinitionOf(sum));

    for (uint64_t numberOfRounds : { 0u, 1u, 4u }) {
        for (uint64_t most : { 1u, 8u, 100u }) {
            uint64_t expected = 0;
            for (uint64_t round = numberOfRounds; round; --round) {
                if (expected + round >= most) {
                    expected = allocationTestMixed(expected, expected + round);
                    break;
                }
                expected += round;
            }
            AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, most) == expected);
        }
    }
}

// The first way into the join leaves the Phi in a register other than the lowest free one, which is what the value of the second way would take by itself.
static void allocatorDefinesValueInRegisterOfPhiWhoseHeadIsFixed()
{
    TestProcedure test;
    Output& out = test.out();
    LValue selector = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock first = out.newBlock();
    LBasicBlock second = out.newBlock();
    LBasicBlock join = out.newBlock();
    LValue phi = out.phiIn(join, Int64);
    out.branch(out.testNonZero64(selector, 1), unlikelyEdge(second), normalEdge(first));

    out.appendTo(first);
    LValue other = out.load64(allocationTestWordAt(test, buffer, 0));
    LValue onFirstWay = out.load64(allocationTestWordAt(test, buffer, 1));
    out.store64(other, allocationTestWordAt(test, buffer, 3));
    out.upsilon(onFirstWay, phi);
    out.jump(normalEdge(join));

    out.appendTo(second);
    LValue onSecondWay = out.load64(allocationTestWordAt(test, buffer, 2));
    out.upsilon(onSecondWay, phi);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.store64(phi, allocationTestWordAt(test, buffer, 4));
    out.ret(selector);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(first).orderIndex < order.of(second).orderIndex);
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(other) != allocation.registerAtDefinitionOf(onFirstWay));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(phi) == allocation.registerAtDefinitionOf(onFirstWay));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(onSecondWay) == allocation.registerAtDefinitionOf(phi));
    AOT_ALLOCATION_TEST_CHECK(test, text.verbsOf(second) == "Load Jump"_s && !text.count("move"_s, onFirstWay));

    for (uint64_t goesSecondWay : { 0u, 1u }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(goesSecondWay, words.data()) == goesSecondWay);
        AOT_ALLOCATION_TEST_CHECK(test, words[4] == words[goesSecondWay ? 2 : 1]);
    }
}

// for (; left; left--) if (left & 1) count++; The arm that does not count hands the Phi of the loop on as it is, so the Phi of the join belongs in the register of that Phi.
// In the arm that counts nothing needs the Phi of the loop behind the addition: the other arm and what stands behind the loop read it, and no way leads there from this arm.
static void allocatorBringsCountThatOneArmAddsToRoundInItsRegister()
{
    TestProcedure test;
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock counts = out.newBlock();
    LBasicBlock skips = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue count = out.phiIn(head, Int64);
    LValue joined = out.phiIn(join, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, count);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    out.branch(out.testNonZero64(left, 1), normalEdge(counts), normalEdge(skips));

    out.appendTo(counts);
    LValue nextCount = out.add(count, 1);
    out.upsilon(nextCount, joined);
    out.jump(normalEdge(join));

    out.appendTo(skips);
    out.upsilon(count, joined);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.upsilon(joined, count);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.store64(count, allocationTestWordAt(test, buffer, 0));
    out.ret(rounds);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(skips).isReached() && order.of(counts).isReached() && order.of(join).loop == order.of(head).loop);
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(count) && allocation.registerAtDefinitionOf(joined) == allocation.registerAtDefinitionOf(count));
    AOT_ALLOCATION_TEST_CHECK(test, text.verbsOf(skips) == "Jump"_s && !text.count("move"_s, nullptr, join));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(nextCount) == allocation.registerAtDefinitionOf(count));
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("move"_s, nullptr, counts) && !text.count("swap"_s) && !text.count("save"_s) && !text.count("reload"_s));

    for (uint64_t numberOfRounds : { 0u, 1u, 2u, 7u }) {
        uint64_t counted = ~uint64_t { 0 };
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, &counted) == numberOfRounds);
        AOT_ALLOCATION_TEST_CHECK(test, counted == (numberOfRounds + 1) / 2);
    }
}

// Leaves poison in the registers.
static void allocationTestAppendPatchThatTakes(TestProcedure& test, RegisterMask gprs)
{
    PatchOperation* patch = test.out().patchpoint(Void);
    patch->clobberLate(gprs);
    patch->setGenerator([gprs](CCallHelpers& jit, const PatchParameters&) {
        forEachRegister(gprs, [&](Reg reg) {
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(poison)), reg.gpr());
        });
    });
}

// A stub that the loop calls on every turn writes the register in which the addend arrives, and the one that the sum would take by itself. Both give way once, on the edge into the loop.
static void allocatorKeepsWhatLoopReadsOutOfWayOfItsPatches()
{
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    LValue rounds = test.argumentGPR(0);
    LValue addend = test.argumentGPR(1);
    LValue buffer = test.argumentGPR(2);
    RegisterMask argumentRegisters = maskOf(rounds->argumentReg()) | maskOf(addend->argumentReg()) | maskOf(buffer->argumentReg());
    RegisterMask lowestFreeRegister = file.allocatableIn(Bank::GP) & ~file.calleeSaved & ~argumentRegisters;
    lowestFreeRegister &= ~(lowestFreeRegister - 1);
    RegisterMask taken = maskOf(addend->argumentReg()) | lowestFreeRegister;
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    allocationTestAppendPatchThatTakes(test, taken);
    LValue nextSum = out.add(sum, addend);
    LValue nextLeft = out.sub(left, 1);
    out.upsilon(nextSum, sum);
    out.upsilon(nextLeft, left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.store64(sum, allocationTestWordAt(test, buffer, 0));
    out.ret(addend);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(head).isLoopHead);
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().liveness().of(order.loops()[order.of(head).loop], order).registersTakenByPatches == taken);
    Reg registerOfAddend = allocationTestRegisterAtHead(test, head, addend);
    AOT_ALLOCATION_TEST_CHECK(test, registerOfAddend && !(maskOf(registerOfAddend) & taken));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(sum) && !(maskOf(allocation.registerAtDefinitionOf(sum)) & taken));
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("move"_s, nullptr, head) && !text.count("move"_s, nullptr, body) && !text.count("swap"_s));
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s) && !text.count("reload"_s));

    constexpr uint64_t added = 5;
    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        uint64_t stored = ~uint64_t { 0 };
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, added, &stored) == added);
        AOT_ALLOCATION_TEST_CHECK(test, stored == numberOfRounds * added);
    }
}

// The Patch of the loop leaves three registers alone. The loop has two Phis and reads one value, and a value that only lives through holds the first of the three. It gives way.
static void allocatorGivesRegistersThatPatchesLeaveAloneToWhatLoopUses()
{
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    LValue rounds = test.argumentGPR(0);
    LValue addend = test.argumentGPR(1);
    LValue buffer = test.argumentGPR(2);
    RegisterMask argumentRegisters = maskOf(rounds->argumentReg()) | maskOf(addend->argumentReg()) | maskOf(buffer->argumentReg());
    RegisterMask leftAlone = 0;
    RegisterMask others = file.allocatableIn(Bank::GP) & ~file.calleeSaved & ~argumentRegisters;
    for (unsigned count = 0; count < 3; ++count) {
        leftAlone |= others & ~(others - 1);
        others &= others - 1;
    }
    LValue through = out.load64(allocationTestWordAt(test, buffer, 1));
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    allocationTestAppendPatchThatTakes(test, file.allocatableIn(Bank::GP) & ~leftAlone);
    out.upsilon(out.add(sum, addend), sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.store64(sum, allocationTestWordAt(test, buffer, 0));
    out.ret(through);
    compileOrFail(test);

    AllocationTestText text(test);
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestCountOf(leftAlone) == 3 && (maskOf(allocation.registerAtDefinitionOf(through)) & leftAlone));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.isSpilled(through) && text.isStoredOnceBehindDefinition(through));
    RegisterMask held = 0;
    for (LValue value : { addend, left, sum }) {
        Reg reg = allocationTestRegisterAtHead(test, head, value);
        AOT_ALLOCATION_TEST_CHECK(test, reg && !allocation.isSpilled(value));
        held |= maskOf(reg);
    }
    AOT_ALLOCATION_TEST_CHECK(test, held == leftAlone);
    for (const Block* block : { head, body })
        AOT_ALLOCATION_TEST_CHECK(test, !text.count("move"_s, nullptr, block) && !text.count("reload"_s, nullptr, block) && !text.count("save"_s, nullptr, block) && !text.count("swap"_s, nullptr, block));

    constexpr uint64_t added = 5;
    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, added, words.data()) == words[1]);
        AOT_ALLOCATION_TEST_CHECK(test, words[0] == numberOfRounds * added);
    }
}

// More values than callees save registers live across the call of the loop, so some have spills. In the loop a rare arm alone reads them, and loads what it reads.
static void allocatorLoadsNothingOnWayRoundLoopForRareArm()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP)) + 2;
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    out.upsilon(rounds, left);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    allocationTestAppendCall(test, nullptr);
    out.branch(out.testNonZero64(left, 2), rareEdge(slow), normalEdge(latch));

    out.appendTo(slow);
    out.store64(allocationTestMixAll(out, values.span()), allocationTestWordAt(test, buffer, count));
    out.jump(normalEdge(latch));

    out.appendTo(latch);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.ret(allocationTestMixAll(out, values.span()));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(head).isLoopHead && order.of(slow).isRare() && order.of(slow).loop == order.of(head).loop);
    unsigned numberSpilled = 0;
    for (LValue value : values) {
        numberSpilled += allocation.isSpilled(value);
        for (const Block* block : { head, body, latch })
            AOT_ALLOCATION_TEST_CHECK(test, !text.count("reload"_s, value, block) && !text.count("move"_s, value, block));
        if (allocation.isSpilled(value))
            AOT_ALLOCATION_TEST_CHECK(test, !allocationTestRegisterAtHead(test, head, value) && text.isStoredOnceBehindDefinition(value));
    }
    AOT_ALLOCATION_TEST_CHECK(test, numberSpilled >= 2);

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t expected = allocationTestMixedAll(std::span<const uint64_t>(words).first(count));
    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        words[count] = 0;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, words.data()) == expected);
        AOT_ALLOCATION_TEST_CHECK(test, words[count] == (numberOfRounds >= 2 ? expected : 0));
    }
}

// One way into the join makes a call, which costs the spilled values their registers. Behind the join a rare arm alone reads them, so that way loads none of them for the join.
static void allocatorLoadsNothingAtJoinForRareArm()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP)) + 2;
    LValue selector = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    // It needs the frame at the entry. With a frame around the call alone, nothing would be spilled outside it, and the way out of it would load every value.
    PatchOperation* atEntry = out.patchpoint(Void);
    atEntry->effects = Effects::none();
    atEntry->effects.writesLocalState = true;
    atEntry->setGenerator([](CCallHelpers&, const PatchParameters&) { });
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    LBasicBlock calls = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock done = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), unlikelyEdge(calls), normalEdge(join));

    out.appendTo(calls);
    allocationTestAppendCall(test, nullptr);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.branch(out.testNonZero64(selector, 2), rareEdge(slow), normalEdge(done));

    out.appendTo(slow);
    out.store64(allocationTestMixAll(out, values.span()), allocationTestWordAt(test, buffer, count));
    out.jump(normalEdge(done));

    out.appendTo(done);
    out.ret(selector);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(slow).isRare() && order.of(join).predecessors.size() == 2 && order.of(join).predecessors.back() == calls);
    unsigned numberSpilled = 0;
    for (LValue value : values) {
        numberSpilled += allocation.isSpilled(value);
        AOT_ALLOCATION_TEST_CHECK(test, !text.count("reload"_s, value, calls));
    }
    AOT_ALLOCATION_TEST_CHECK(test, numberSpilled >= 2);

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t mixed = allocationTestMixedAll(std::span<const uint64_t>(words).first(count));
    for (uint64_t way : { 0u, 1u, 2u, 3u }) {
        words[count] = 0;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way, words.data()) == way);
        AOT_ALLOCATION_TEST_CHECK(test, words[count] == ((way & 2) ? mixed : 0));
    }
}

// Two ways lead to a block that calls and then reads. On the first the spilled values still lie in registers, and the call on the second has cost them those.
// The call behind the join takes the registers again, so the join holds none of the values and the second way loads nothing.
static void allocatorLoadsNothingAtJoinForReadBehindCall()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP)) + 2;
    LValue selector = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    LBasicBlock calls = out.newBlock();
    LBasicBlock join = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), unlikelyEdge(calls), normalEdge(join));

    out.appendTo(calls);
    allocationTestAppendCall(test, nullptr);
    out.jump(normalEdge(join));

    out.appendTo(join);
    allocationTestAppendCall(test, nullptr);
    out.ret(allocationTestMixAll(out, values.span()));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(join).predecessors.size() == 2 && order.of(join).predecessors.back() == calls);
    unsigned numberSpilled = 0;
    for (LValue value : values) {
        numberSpilled += allocation.isSpilled(value);
        AOT_ALLOCATION_TEST_CHECK(test, !text.count("reload"_s, value, calls));
    }
    AOT_ALLOCATION_TEST_CHECK(test, numberSpilled >= 2);

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t mixed = allocationTestMixedAll(std::span<const uint64_t>(words).first(count));
    for (uint64_t way : { 0u, 1u })
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way, words.data()) == mixed);
}

// The same twice over. The first join reads the values, so it holds them, and the way that has called loads them. That join has been left behind where the second one is fixed:
// what it held says nothing of the second, which holds none of the values.
static void allocatorLoadsNothingAtSecondJoinForReadBehindCall()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP)) + 2;
    LValue selector = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    LBasicBlock firstCalls = out.newBlock();
    LBasicBlock firstJoin = out.newBlock();
    LBasicBlock secondCalls = out.newBlock();
    LBasicBlock secondJoin = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), unlikelyEdge(firstCalls), normalEdge(firstJoin));

    out.appendTo(firstCalls);
    allocationTestAppendCall(test, nullptr);
    out.jump(normalEdge(firstJoin));

    out.appendTo(firstJoin);
    out.store64(allocationTestMixAll(out, values.span()), allocationTestWordAt(test, buffer, count));
    out.branch(out.testNonZero64(selector, 2), unlikelyEdge(secondCalls), normalEdge(secondJoin));

    out.appendTo(secondCalls);
    allocationTestAppendCall(test, nullptr);
    out.jump(normalEdge(secondJoin));

    out.appendTo(secondJoin);
    allocationTestAppendCall(test, nullptr);
    out.ret(allocationTestMixAll(out, values.span()));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(secondJoin).predecessors.size() == 2 && order.of(secondJoin).predecessors.back() == secondCalls);
    unsigned numberSpilled = 0;
    for (LValue value : values) {
        numberSpilled += allocation.isSpilled(value);
        AOT_ALLOCATION_TEST_CHECK(test, !text.count("reload"_s, value, secondCalls));
    }
    AOT_ALLOCATION_TEST_CHECK(test, numberSpilled >= 2);

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t mixed = allocationTestMixedAll(std::span<const uint64_t>(words).first(count));
    for (uint64_t way : { 0u, 1u, 2u, 3u }) {
        words[count] = 0;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way, words.data()) == mixed && words[count] == mixed);
    }
}

// Behind the join one arm calls and the other does not, and both lead on to the reads. The way past the call finds the values in their registers, so the join holds them, and the way that has called loads them.
static void allocatorKeepsValuesAtJoinForReadPastCallInOneArm()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP)) + 2;
    LValue selector = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    LBasicBlock calls = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock arm = out.newBlock();
    LBasicBlock reads = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), unlikelyEdge(calls), normalEdge(join));

    out.appendTo(calls);
    allocationTestAppendCall(test, nullptr);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.branch(out.testNonZero64(selector, 2), unlikelyEdge(arm), normalEdge(reads));

    out.appendTo(arm);
    allocationTestAppendCall(test, nullptr);
    out.jump(normalEdge(reads));

    out.appendTo(reads);
    out.ret(allocationTestMixAll(out, values.span()));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(join).predecessors.size() == 2 && order.of(join).predecessors.back() == calls && order.of(arm).orderIndex < order.of(reads).orderIndex);
    unsigned numberSpilled = 0;
    for (LValue value : values) {
        numberSpilled += allocation.isSpilled(value);
        AOT_ALLOCATION_TEST_CHECK(test, text.count("reload"_s, value, calls) == allocation.isSpilled(value));
    }
    AOT_ALLOCATION_TEST_CHECK(test, numberSpilled >= 2);

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t mixed = allocationTestMixedAll(std::span<const uint64_t>(words).first(count));
    for (uint64_t way : { 0u, 1u, 2u, 3u })
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way, words.data()) == mixed);
}

// Three ways lead into one rare block. On the first the spilled values still lie in registers, and the calls ahead of the other two have cost them those.
// The block loads what it reads, so no way into it loads anything: no block stands on any of the three edges.
static void allocatorLoadsNothingOnWaysIntoRareJoin()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP)) + 2;
    LValue selector = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    LBasicBlock first = out.newBlock();
    LBasicBlock second = out.newBlock();
    LBasicBlock done = out.newBlock();
    LBasicBlock shared = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), rareEdge(shared), normalEdge(first));

    out.appendTo(first);
    allocationTestAppendCall(test, nullptr);
    out.branch(out.testNonZero64(selector, 2), rareEdge(shared), normalEdge(second));

    out.appendTo(second);
    allocationTestAppendCall(test, nullptr);
    out.branch(out.testNonZero64(selector, 4), rareEdge(shared), normalEdge(done));

    out.appendTo(done);
    out.ret(allocationTestMixAll(out, values.span()));

    out.appendTo(shared);
    out.store64(allocationTestMixAll(out, values.span()), allocationTestWordAt(test, buffer, count));
    out.ret(selector);
    compileOrFail(test);

    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(shared).isRare() && order.of(shared).predecessors.size() == 3);
    unsigned numberSpilled = 0;
    for (LValue value : values)
        numberSpilled += allocation.isSpilled(value);
    AOT_ALLOCATION_TEST_CHECK(test, numberSpilled >= 2);
    for (const Block* wayIn : order.of(shared).predecessors)
        AOT_ALLOCATION_TEST_CHECK(test, wayIn->last()->opcode() == Branch && wayIn->numSuccessors() == 2);

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t mixed = allocationTestMixedAll(std::span<const uint64_t>(words).first(count));
    for (uint64_t way : { 0u, 1u, 2u, 4u, 6u }) {
        words[count] = 0;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way, words.data()) == (way ? way : mixed));
        AOT_ALLOCATION_TEST_CHECK(test, words[count] == (way ? mixed : 0));
    }
}

// On x86-64 a word of the frame is addressed from the end that reaches it with one byte, and from the frame pointer where neither does: that base needs no SIB byte.
static void encoderAddressesWordOfFrameFromNearerEnd()
{
    TestProcedure test;
    Output& out = test.out();
    constexpr unsigned count = 48;
    LValue buffer = test.argumentGPR(0);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    allocationTestAppendCall(test, nullptr);
    out.ret(allocationTestMixAll(out, values.span()));
    compileOrFail(test);

    const Frame& frame = test.procedure().frame();
    intptr_t bytes = frame.bytesBelowFrameRecord();
    AOT_ALLOCATION_TEST_CHECK(test, bytes > 2 * 128 + 64);
    auto isAddressedFrom = [&](intptr_t offsetFromFP, GPRReg base, intptr_t offset) {
        MacroAssembler::Address address = frame.addressInFrame(offsetFromFP);
        return address.base == base && address.offset == offset;
    };
    if (isX86_64()) {
        AOT_ALLOCATION_TEST_CHECK(test, isAddressedFrom(-8, MacroAssembler::framePointerRegister, -8));
        AOT_ALLOCATION_TEST_CHECK(test, isAddressedFrom(-128, MacroAssembler::framePointerRegister, -128));
        AOT_ALLOCATION_TEST_CHECK(test, isAddressedFrom(-136, MacroAssembler::framePointerRegister, -136));
        AOT_ALLOCATION_TEST_CHECK(test, isAddressedFrom(128 - bytes, MacroAssembler::framePointerRegister, 128 - bytes));
        AOT_ALLOCATION_TEST_CHECK(test, isAddressedFrom(120 - bytes, MacroAssembler::stackPointerRegister, 120));
    }
    AOT_ALLOCATION_TEST_CHECK(test, isAddressedFrom(-bytes, MacroAssembler::stackPointerRegister, 0));

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(words.data()) == allocationTestMixedAll(std::span<const uint64_t>(words).first(count)));
}

// On x86-64: the shorter the access, the smaller.
enum AllocationTestNearness : unsigned { AllocationTestNearFramePointer, AllocationTestNearStackPointer, AllocationTestFarFromBothEnds };

static AllocationTestNearness allocationTestNearnessOf(const Frame& frame, intptr_t offsetFromFP)
{
    MacroAssembler::Address address = frame.addressInFrame(offsetFromFP);
    if (!isRepresentableAs<int8_t>(static_cast<int64_t>(address.offset)))
        return AllocationTestFarFromBothEnds;
    return address.base == MacroAssembler::framePointerRegister ? AllocationTestNearFramePointer : AllocationTestNearStackPointer;
}

static AllocationTestNearness allocationTestNearnessOf(TestProcedure& test, const Operation* spilled)
{
    const Frame& frame = test.procedure().frame();
    return allocationTestNearnessOf(frame, frame.offsetFromFPOf(test.procedure().allocation().spillOf(spilled)));
}

// By the number of the word, counted down from the frame pointer, first word 1: how many instructions name a spill that lies in it. Of a procedure whose Patches have no operand in the frame.
static Vector<unsigned> allocationTestInstructionsNamingWords(TestProcedure& test)
{
    const Allocation& allocation = test.procedure().allocation();
    const Frame& frame = test.procedure().frame();
    Vector<unsigned> result(FillWith { }, frame.bytesBelowFrameRecord() / sizeof(uint64_t) + 1, 0u);
    auto note = [&](Location location) {
        if (location.isSpill())
            ++result[static_cast<size_t>((7 - frame.offsetFromFPOf(location.spillIndex())) / 8)];
    };
    // The store behind a definition has no step.
    std::span<const SpillFacts> spills = allocation.spills();
    for (SpillIndex spill = 0; spill < spills.size(); ++spill) {
        const SpillFacts& facts = spills[spill];
        if (facts.value && !facts.isSaveInRareRegion && !facts.isPointerBesideNarrowReference && allocation.isStoredBehindDefinition(facts.value))
            note(Location::spill(spill));
    }
    for (const Step& step : allocation.steps()) {
        if (step.kind == StepKind::Execute && step.operation->opcode() == Extract)
            continue;
        note(step.destination);
        for (Location source : step.sources)
            note(source);
    }
    return result;
}

// x86-64, read off the bytes of the code: how often a ModRM byte that names the frame pointer stands in front of that offset. mod 01 has one byte of offset behind it, mod 10 four.
static unsigned allocationTestNumberOfAccessesInCode(TestProcedure& test, int32_t offsetFromFP, unsigned bytesOfOffset)
{
    std::span<const uint8_t> code = test.linkedCode();
    const uint8_t modAndBase = bytesOfOffset == 1 ? 0x45 : 0x85;
    unsigned number = 0;
    for (size_t index = 0; index + bytesOfOffset < code.size(); ++index) {
        bool matches = (code[index] & 0xc7) == modAndBase;
        for (unsigned byte = 0; matches && byte < bytesOfOffset; ++byte)
            matches = code[index + 1 + byte] == static_cast<uint8_t>(static_cast<uint32_t>(offsetFromFP) >> (8 * byte));
        number += matches;
    }
    return number;
}

// Of a function without suspensions: no word with spills lies farther from the ends of the frame than one that fewer instructions name.
static void allocationTestCheckBusiestWordsLieNearest(TestProcedure& test)
{
    const Frame& frame = test.procedure().frame();
    Vector<unsigned> instructions = allocationTestInstructionsNamingWords(test);
    auto nearnessOfWord = [&](size_t word) {
        return allocationTestNearnessOf(frame, -static_cast<intptr_t>(word * sizeof(uint64_t)));
    };
    for (size_t farther = 1; farther < instructions.size(); ++farther) {
        for (size_t nearer = 1; nearer < instructions.size(); ++nearer) {
            if (instructions[farther] && instructions[nearer] && nearnessOfWord(farther) > nearnessOfWord(nearer))
                AOT_ALLOCATION_TEST_CHECK(test, instructions[farther] <= instructions[nearer]);
        }
    }
}

// Answers the value at `busy`, which is read behind each of that many calls. The others are read once.
static LValue allocationTestBuildFrameWithOneBusyWord(TestProcedure& test, unsigned count, unsigned busy, unsigned numberOfReads)
{
    Output& out = test.out();
    Vector<LValue> values = allocationTestLoadWords(test, test.argumentGPR(0), 0, count, Int64);
    allocationTestAppendCall(test, nullptr);
    LValue sum = allocationTestMixAll(out, values.span());
    for (unsigned read = 0; read < numberOfReads; ++read) {
        allocationTestAppendCall(test, nullptr);
        sum = allocationTestMix(out, sum, values[busy]);
    }
    out.ret(sum);
    compileOrFail(test);
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().allocation().isSpilled(values[busy]) && test.procedure().frame().bytesBelowFrameRecord() >= 40 * sizeof(uint64_t));
    return values[busy];
}

// Forty values wait in the frame, and one byte of offset reaches at most thirty-two of them from its two ends. Where each is read once, the first that are spilled lie in reach of the frame pointer,
// the next in reach of the stack pointer, and the last is out of reach. Read behind each of twenty calls it lies next to the frame pointer, and each of those reads has one byte of offset.
static void slotsGiveMostReadWordOfLargeFrameShortOffset()
{
    constexpr unsigned numberOfReads = 20;
    TestProcedure test;
    TestProcedure allReadOnce;
    const unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP)) + 40;
    const unsigned busy = count - 1;
    LValue readOften = allocationTestBuildFrameWithOneBusyWord(test, count, busy, numberOfReads);
    LValue readOnce = allocationTestBuildFrameWithOneBusyWord(allReadOnce, count, busy, 0);

    if (isX86_64()) {
        int32_t offsetOfReadOnce = allReadOnce.procedure().frame().offsetFromFPOf(allReadOnce.procedure().allocation().spillOf(readOnce));
        AOT_ALLOCATION_TEST_CHECK(allReadOnce, allocationTestNearnessOf(allReadOnce, readOnce) == AllocationTestFarFromBothEnds);
        AOT_ALLOCATION_TEST_CHECK(allReadOnce, allocationTestNumberOfAccessesInCode(allReadOnce, offsetOfReadOnce, 4) >= 2);

        int32_t offsetOfReadOften = test.procedure().frame().offsetFromFPOf(test.procedure().allocation().spillOf(readOften));
        AOT_ALLOCATION_TEST_CHECK(test, allocationTestNearnessOf(test, readOften) == AllocationTestNearFramePointer);
        AOT_ALLOCATION_TEST_CHECK(test, allocationTestNumberOfAccessesInCode(test, offsetOfReadOften, 1) > numberOfReads);
        AOT_ALLOCATION_TEST_CHECK(test, !allocationTestNumberOfAccessesInCode(test, offsetOfReadOften, 4));
        allocationTestCheckBusiestWordsLieNearest(test);
    }

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t expected = allocationTestMixedAll(std::span<const uint64_t>(words).first(count));
    for (unsigned read = 0; read < numberOfReads; ++read)
        expected = allocationTestMixed(expected, words[busy]);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(words.data()) == expected);
}

// Thirty operations in a row read one value, and a register serves them behind one load. Forty other values are loaded behind each of four calls, and they are more than the ends of the frame reach.
// What counts is the instructions that name a word, so the one value lies out of reach, however many operations read it.
static void slotsCountInstructionsThatNameWordAndNotReaders()
{
    TestProcedure test;
    Output& out = test.out();
    // The first values that are loaded stay in the registers that a callee saves.
    const unsigned numberInSavedRegisters = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP));
    const unsigned count = numberInSavedRegisters + 58;
    const unsigned servedByRegister = numberInSavedRegisters + 3;
    const unsigned firstLoadedOften = numberInSavedRegisters + 8;
    constexpr unsigned numberLoadedOften = 40;
    constexpr unsigned numberOfReadsInRow = 30;
    constexpr unsigned numberOfRounds = 4;
    LValue buffer = test.argumentGPR(0);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    std::span<const LValue> loadedOften = values.span().subspan(firstLoadedOften, numberLoadedOften);
    allocationTestAppendCall(test, nullptr);
    LValue sum = allocationTestMixAll(out, values.span());
    for (unsigned read = 0; read < numberOfReadsInRow; ++read)
        sum = allocationTestMix(out, sum, values[servedByRegister]);
    for (unsigned round = 0; round < numberOfRounds; ++round) {
        allocationTestAppendCall(test, nullptr);
        for (LValue value : loadedOften)
            sum = allocationTestMix(out, sum, value);
    }
    out.ret(allocationTestMix(out, sum, values[servedByRegister]));
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().allocation().isSpilled(values[servedByRegister]));
    if (isX86_64()) {
        allocationTestCheckBusiestWordsLieNearest(test);
        AOT_ALLOCATION_TEST_CHECK(test, allocationTestNearnessOf(test, values[servedByRegister]) == AllocationTestFarFromBothEnds);
    }

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t expected = allocationTestMixedAll(std::span<const uint64_t>(words).first(count));
    for (unsigned read = 0; read < numberOfReadsInRow; ++read)
        expected = allocationTestMixed(expected, words[servedByRegister]);
    for (unsigned round = 0; round < numberOfRounds; ++round) {
        for (unsigned index = 0; index < numberLoadedOften; ++index)
            expected = allocationTestMixed(expected, words[firstLoadedOften + index]);
    }
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(words.data()) == allocationTestMixed(expected, words[servedByRegister]));
}

// Two values take turns in one place, and four instructions name each. Five name each of the forty others, which are more than the ends of the frame reach.
// The word counts the instructions of both, so it lies next to the frame pointer. No sum lives across a call, so nothing else takes a turn in a place.
static void slotsCountInstructionsOfAllThatTakeTurnsInWord()
{
    TestProcedure test;
    Output& out = test.out();
    constexpr unsigned numberOfRounds = 4;
    constexpr unsigned numberOfReadsOfEach = 3;
    constexpr unsigned firstWordWritten = 48;
    const unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP)) + 40;
    unsigned nextWordWritten = firstWordWritten;
    LValue buffer = test.argumentGPR(0);
    auto write = [&](LValue value) {
        out.store64(value, allocationTestWordAt(test, buffer, nextWordWritten++));
    };
    Vector<LValue> others = allocationTestLoadWords(test, buffer, 0, count, Int64);
    // From another start each time: equal sums are worked out once.
    auto mixOthersFrom = [&](unsigned start) {
        LValue sum = out.constInt64(start);
        for (LValue other : others)
            sum = allocationTestMix(out, sum, other);
        return sum;
    };
    for (unsigned round = 1; round < numberOfRounds; ++round) {
        allocationTestAppendCall(test, nullptr);
        write(mixOthersFrom(round));
    }
    std::array<LValue, 2> takingTurns;
    for (unsigned index = 0; index < takingTurns.size(); ++index) {
        takingTurns[index] = out.load64(allocationTestWordAt(test, buffer, count + index));
        for (unsigned read = 0; read < numberOfReadsOfEach; ++read) {
            allocationTestAppendCall(test, nullptr);
            write(takingTurns[index]);
        }
    }
    allocationTestAppendCall(test, nullptr);
    out.ret(mixOthersFrom(numberOfRounds));
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    const Frame& frame = test.procedure().frame();
    AOT_ALLOCATION_TEST_CHECK(test, count + takingTurns.size() <= firstWordWritten && allocation.isSpilled(takingTurns[0]) && allocation.isSpilled(takingTurns[1]));
    int32_t offset = frame.offsetFromFPOf(allocation.spillOf(takingTurns[0]));
    AOT_ALLOCATION_TEST_CHECK(test, offset == frame.offsetFromFPOf(allocation.spillOf(takingTurns[1])));
    if (isX86_64()) {
        unsigned instructionsNamingWord = allocationTestInstructionsNamingWords(test)[static_cast<size_t>((7 - offset) / 8)];
        AOT_ALLOCATION_TEST_CHECK(test, instructionsNamingWord > numberOfRounds + 1 && instructionsNamingWord <= 2 * (numberOfReadsOfEach + 1));
        unsigned numberOfOthersOutOfReach = 0;
        for (LValue other : others)
            numberOfOthersOutOfReach += allocation.isSpilled(other) && allocationTestNearnessOf(test, other) == AllocationTestFarFromBothEnds;
        AOT_ALLOCATION_TEST_CHECK(test, numberOfOthersOutOfReach);
        AOT_ALLOCATION_TEST_CHECK(test, allocationTestNearnessOf(test, takingTurns[0]) == AllocationTestNearFramePointer);
        allocationTestCheckBusiestWordsLieNearest(test);
    }

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    const std::array<uint64_t, 64> wordsAtEntry = words;
    auto mixedFrom = [&](unsigned start) {
        uint64_t sum = start;
        for (uint64_t word : std::span<const uint64_t>(wordsAtEntry).first(count))
            sum = allocationTestMixed(sum, word);
        return sum;
    };
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(words.data()) == mixedFrom(numberOfRounds));
    unsigned nextWordRead = firstWordWritten;
    for (unsigned round = 1; round < numberOfRounds; ++round)
        AOT_ALLOCATION_TEST_CHECK(test, words[nextWordRead++] == mixedFrom(round));
    for (unsigned index = 0; index < takingTurns.size(); ++index) {
        for (unsigned read = 0; read < numberOfReadsOfEach; ++read)
            AOT_ALLOCATION_TEST_CHECK(test, words[nextWordRead++] == wordsAtEntry[count + index]);
    }
}

// Two StackSlots and forty spilled values. Two instructions name the large slot, which was made first and alone would fill the reach of the frame pointer. Twenty name the small one.
// So the small one lies in front, the spilled values follow it, and the large one begins where the reach of the frame pointer ends.
static void slotsKeepStackSlotInReachOfFramePointerWhereEnoughInstructionsNameIt()
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    constexpr unsigned numberOfRounds = 10;
    constexpr unsigned storedInLargeSlot = 2;
    const unsigned count = allocationTestCountOf(registerFileFor(proc.traits()).calleeSavedIn(Bank::GP)) + 40;
    StackSlot* largeSlot = proc.addStackSlot(24 * sizeof(uint64_t));
    StackSlot* smallSlot = proc.addStackSlot(2 * sizeof(uint64_t));
    auto wordOfLargeSlot = [&] {
        return out.address(test.heaps().frameSlots, out.slotBase(largeSlot), 5 * sizeof(uint64_t));
    };
    auto wordOfSmallSlot = [&] {
        return out.address(test.heaps().frameSlots, out.slotBase(smallSlot), sizeof(uint64_t));
    };
    LValue buffer = test.argumentGPR(0);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    out.store64(values[storedInLargeSlot], wordOfLargeSlot());
    allocationTestAppendCall(test, nullptr);
    LValue sum = allocationTestMixAll(out, values.span());
    for (unsigned round = 0; round < numberOfRounds; ++round) {
        out.store64(sum, wordOfSmallSlot());
        allocationTestAppendCall(test, nullptr);
        sum = out.add(out.load64(wordOfSmallSlot()), 1);
    }
    out.ret(allocationTestMix(out, sum, out.load64(wordOfLargeSlot())));
    compileOrFail(test);

    if (isX86_64()) {
        const Frame& frame = proc.frame();
        AOT_ALLOCATION_TEST_CHECK(test, allocationTestNearnessOf(frame, smallSlot->offsetFromFP()) == AllocationTestNearFramePointer);
        AOT_ALLOCATION_TEST_CHECK(test, largeSlot->offsetFromFP() + static_cast<intptr_t>(largeSlot->byteSize()) <= -128);
        unsigned numberNearFramePointer = 0;
        for (LValue value : values)
            numberNearFramePointer += proc.allocation().isSpilled(value) && allocationTestNearnessOf(test, value) == AllocationTestNearFramePointer;
        AOT_ALLOCATION_TEST_CHECK(test, numberNearFramePointer >= 8);
        allocationTestCheckBusiestWordsLieNearest(test);
    }

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t expected = allocationTestMixedAll(std::span<const uint64_t>(words).first(count)) + numberOfRounds;
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(words.data()) == allocationTestMixed(expected, words[storedInLargeSlot]));
}

#if CPU(X86_64)
// The run of callee saves ends at the frame record. So the setup pushes them, the last of the run first, and moves the stack pointer by what is left of the frame.
static void frameSetupPushesCalleeSaves()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP));
    LValue buffer = test.argumentGPR(0);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    allocationTestAppendCall(test, nullptr);
    out.ret(allocationTestMixAll(out, values.span()));
    compileOrFail(test);

    const Frame& frame = test.procedure().frame();
    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    AOT_ALLOCATION_TEST_CHECK(test, count && saves.registerCount() == count);
    // push rbp; mov rbp, rsp
    Vector<uint8_t> setup { 0x55, 0x48, 0x89, 0xe5 };
    for (size_t index = saves.registerCount(); index--;) {
        unsigned number = saves.at(index).reg().gpr();
        if (number >= 8)
            setup.append(0x41);
        setup.append(0x50 | (number & 7));
    }
    unsigned bytesLeft = frame.bytesBelowFrameRecord() - count * sizeof(CPURegister);
    AOT_ALLOCATION_TEST_CHECK(test, bytesLeft < 128);
    // sub rsp, imm8
    if (bytesLeft)
        setup.appendList<uint8_t>({ 0x48, 0x83, 0xec, static_cast<uint8_t>(bytesLeft) });
    AOT_ALLOCATION_TEST_CHECK(test, !std::ranges::search(test.linkedCode(), setup.span()).empty());

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(words.data()) == allocationTestMixedAll(std::span<const uint64_t>(words).first(count)));
}

// Code for shortenX86Jumps(): nops, jumps to one place behind them all, and a ret at that place.
class AllocationTestJumpsToOnePlace {
public:
    static constexpr uint8_t notEqual = 5;
    static constexpr uint8_t equal = 4;

    struct Jump {
        std::optional<uint8_t> condition;
        uint32_t destination;
        uint8_t length;
    };

    explicit AllocationTestJumpsToOnePlace(uint32_t target)
        : m_target(target)
    {
    }

    void appendJumpAt(uint32_t offset, std::optional<uint8_t> condition)
    {
        padTo(offset);
        if (condition) {
            m_code.append(0x0f);
            m_code.append(0x80 | *condition);
        } else
            m_code.append(0xe9);
        int32_t distance = static_cast<int32_t>(m_target - (m_code.size() + 4));
        m_code.append(std::span { reinterpret_cast<const uint8_t*>(&distance), 4 });
        m_jumps.append(X86LinkedJump { static_cast<uint32_t>(m_code.size()), m_target, condition ? X86JumpKind::Conditional : X86JumpKind::Unconditional, condition.value_or(0) });
    }

    // How many bytes it saves.
    size_t shorten(uint32_t startOfRareCode)
    {
        padTo(m_target);
        m_code.append(ret);
        Vector<X86JumpShift> shifts;
        size_t before = m_code.size();
        m_code.shrink(shortenX86Jumps(m_code.mutableSpan(), m_jumps, shifts, startOfRareCode));
        AOT_TEST_CHECK(m_code.last() == ret);
        return before - m_code.size();
    }

    uint32_t startOf(unsigned index) const
    {
        const X86LinkedJump& jump = m_jumps[index];
        return jump.from - (jump.isShort ? X86JumpShortener::shortJumpLength : X86JumpShortener::longJumpLength(jump.kind));
    }

    Jump jumpAt(uint32_t offset) const
    {
        auto wide = [&](uint32_t at) {
            int32_t distance;
            memcpy(&distance, m_code.span().subspan(at).data(), 4);
            return static_cast<uint32_t>(distance);
        };
        auto narrow = [&](uint32_t at) {
            return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(m_code[at])));
        };
        uint8_t first = m_code[offset];
        if (first == 0xe9)
            return { std::nullopt, offset + 5 + wide(offset + 1), 5 };
        if (first == 0xeb)
            return { std::nullopt, offset + 2 + narrow(offset + 1), 2 };
        if (first == 0x0f) {
            AOT_TEST_CHECK((m_code[offset + 1] & 0xf0) == 0x80);
            return { static_cast<uint8_t>(m_code[offset + 1] & 0xf), offset + 6 + wide(offset + 2), 6 };
        }
        AOT_TEST_CHECK((first & 0xf0) == 0x70);
        return { static_cast<uint8_t>(first & 0xf), offset + 2 + narrow(offset + 1), 2 };
    }

    // How many other jumps the way from the jump to the ret leads through, by the bytes. Each has the condition of the first, or none.
    unsigned numberOfRelaysBehind(unsigned index) const
    {
        Jump first = jumpAt(startOf(index));
        AOT_TEST_CHECK(first.length == m_jumps[index].from - startOf(index) && first.destination == m_jumps[index].to);
        unsigned result = 0;
        for (uint32_t offset = first.destination; m_code[offset] != ret; ++result) {
            AOT_TEST_CHECK(result < m_jumps.size());
            Jump relay = jumpAt(offset);
            AOT_TEST_CHECK(!relay.condition || relay.condition == first.condition);
            offset = relay.destination;
        }
        return result;
    }

    bool isShort(unsigned index) const { return m_jumps[index].isShort; }
    unsigned numberOfJumps() const { return m_jumps.size(); }

private:
    static constexpr uint8_t nop = 0x90;
    static constexpr uint8_t ret = 0xc3;

    void padTo(uint32_t offset)
    {
        AOT_TEST_CHECK(m_code.size() <= offset);
        while (m_code.size() < offset)
            m_code.append(nop);
    }

    uint32_t m_target;
    Vector<uint8_t> m_code;
    Vector<X86LinkedJump> m_jumps;
};

// Three tests jump to one place in the rare code, far behind them. The first two have one condition, so the first goes to the second, two bytes long, and the second goes all the way.
// Where an unconditional jump to that place stands nearby, all three go through it.
static void shortenerChainsFarJumpThroughNearJumpWithItsConditionAndTarget()
{
    using Jumps = AllocationTestJumpsToOnePlace;
    constexpr uint32_t target = 600;
    auto threeTests = [&] {
        Jumps jumps(target);
        jumps.appendJumpAt(0, Jumps::notEqual);
        jumps.appendJumpAt(16, Jumps::notEqual);
        jumps.appendJumpAt(32, Jumps::equal);
        return jumps;
    };

    for (uint32_t startOfRareCode : { UINT32_MAX, target + 1 }) {
        Jumps jumps = threeTests();
        AOT_TEST_CHECK(!jumps.shorten(startOfRareCode));
        for (unsigned index = 0; index < 3; ++index)
            AOT_TEST_CHECK(!jumps.isShort(index) && !jumps.numberOfRelaysBehind(index));
    }

    Jumps jumps = threeTests();
    AOT_TEST_CHECK(jumps.shorten(target) == 4);
    AOT_TEST_CHECK(jumps.isShort(0) && jumps.jumpAt(0).destination == jumps.startOf(1) && jumps.numberOfRelaysBehind(0) == 1);
    AOT_TEST_CHECK(!jumps.isShort(1) && !jumps.numberOfRelaysBehind(1));
    AOT_TEST_CHECK(!jumps.isShort(2) && !jumps.numberOfRelaysBehind(2));

    jumps = threeTests();
    jumps.appendJumpAt(48, std::nullopt);
    AOT_TEST_CHECK(jumps.shorten(target) == 12);
    for (unsigned index = 0; index < 3; ++index)
        AOT_TEST_CHECK(jumps.isShort(index) && jumps.jumpAt(jumps.startOf(index)).destination == jumps.startOf(3) && jumps.numberOfRelaysBehind(index) == 1);
    AOT_TEST_CHECK(!jumps.isShort(3) && !jumps.numberOfRelaysBehind(3));
}

// The second of three goes all the way. The first reaches it ahead, and the third reaches it 40 bytes back.
static void shortenerChainsFarJumpBackward()
{
    using Jumps = AllocationTestJumpsToOnePlace;
    Jumps jumps(600);
    for (uint32_t offset : { 0u, 100u, 140u })
        jumps.appendJumpAt(offset, Jumps::notEqual);
    AOT_TEST_CHECK(jumps.shorten(600) == 8);
    AOT_TEST_CHECK(!jumps.isShort(1) && !jumps.numberOfRelaysBehind(1));
    for (unsigned index : { 0u, 2u })
        AOT_TEST_CHECK(jumps.isShort(index) && jumps.jumpAt(jumps.startOf(index)).destination == jumps.startOf(1) && jumps.numberOfRelaysBehind(index) == 1);
}

// More jumps with one condition and one target than one of them is within reach of: two go all the way, and no way leads through both.
static void shortenerChainsNoJumpThroughTwo()
{
    using Jumps = AllocationTestJumpsToOnePlace;
    Jumps jumps(2000);
    for (uint32_t index = 0; index < 40; ++index)
        jumps.appendJumpAt(10 * index, Jumps::notEqual);
    AOT_TEST_CHECK(jumps.shorten(2000) == 38 * 4);
    for (unsigned index = 0; index < jumps.numberOfJumps(); ++index)
        AOT_TEST_CHECK(jumps.numberOfRelaysBehind(index) == jumps.isShort(index));
}

// As the code stands the first jump does not reach the target. It does once the second is short, so it goes there itself.
static void shortenerChainsNoJumpThatReachesItsTargetOnceOthersAreShort()
{
    using Jumps = AllocationTestJumpsToOnePlace;
    Jumps jumps(137);
    jumps.appendJumpAt(0, Jumps::notEqual);
    jumps.appendJumpAt(6, Jumps::notEqual);
    AOT_TEST_CHECK(jumps.shorten(137) == 8);
    for (unsigned index = 0; index < 2; ++index)
        AOT_TEST_CHECK(jumps.isShort(index) && !jumps.numberOfRelaysBehind(index));
}

// `movabs $signature, register`, which is found again among the linked bytes.
static void allocationTestAppendSignature(TestProcedure& test, uint64_t signature)
{
    RegisterFile file = registerFileFor(test.procedure().traits());
    RegisterMask candidates = file.allocatableIn(Bank::GP) & ~file.calleeSaved;
    Reg written = Reg::fromIndex(static_cast<unsigned>(std::bit_width(candidates)) - 1);
    PatchOperation* patch = test.out().patchpoint(Void);
    patch->clobberLate(RegisterMask { 1 } << written.index());
    patch->setGenerator([written, signature](CCallHelpers& jit, const PatchParameters&) {
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(signature)), written.gpr());
    });
}

// Of the first byte of the instruction.
static size_t allocationTestOffsetOfSignature(TestProcedure& test, uint64_t signature)
{
    constexpr size_t bytesAheadOfImmediate = 2;
    std::span<const uint8_t> code = test.linkedCode();
    for (size_t offset = bytesAheadOfImmediate; offset + sizeof(signature) <= code.size(); ++offset) {
        if (!memcmp(code.subspan(offset).data(), &signature, sizeof(signature)))
            return offset - bytesAheadOfImmediate;
    }
    allocationTestFail(test, __FILE__, __LINE__, "the signature stands in the code");
}

// Two tests with one condition leave for one rare block, which lies behind more code than a short jump reaches over. The encoder says where the rare code starts, so the first test goes through the second.
static void encoderChainsTestsThatLeaveForOneRareBlock()
{
    constexpr uint64_t behindFirstTest = 0x5a17c0de00000001;
    constexpr uint64_t behindSecondTest = 0x5a17c0de00000002;
    constexpr unsigned count = 40;
    TestProcedure test;
    Output& out = test.out();
    LValue selector = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock second = out.newBlock();
    LBasicBlock rest = out.newBlock();
    LBasicBlock shared = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), rareEdge(shared), normalEdge(second));

    out.appendTo(second);
    allocationTestAppendSignature(test, behindFirstTest);
    out.branch(out.testNonZero64(selector, 2), rareEdge(shared), normalEdge(rest));

    out.appendTo(rest);
    allocationTestAppendSignature(test, behindSecondTest);
    LValue sum = out.load(allocationTestWordAt(test, buffer, 0), Int64);
    for (unsigned index = 1; index < count; ++index)
        sum = allocationTestMix(out, sum, out.load(allocationTestWordAt(test, buffer, index), Int64));
    out.ret(sum);

    out.appendTo(shared);
    out.ret(out.constInt64(-1));
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().order().of(shared).isRare());
    std::span<const uint8_t> code = test.linkedCode();
    size_t endOfFirstTest = allocationTestOffsetOfSignature(test, behindFirstTest);
    size_t endOfSecondTest = allocationTestOffsetOfSignature(test, behindSecondTest);
    AOT_ALLOCATION_TEST_CHECK(test, endOfFirstTest >= 2 && endOfSecondTest >= endOfFirstTest + 6);
    constexpr uint8_t shortJumpIfNotZero = 0x75;
    AOT_ALLOCATION_TEST_CHECK(test, code[endOfSecondTest - 6] == 0x0f && code[endOfSecondTest - 5] == 0x85);
    int32_t distanceOfSecond;
    memcpy(&distanceOfSecond, code.subspan(endOfSecondTest - 4).data(), 4);
    AOT_ALLOCATION_TEST_CHECK(test, distanceOfSecond >= 128);
    AOT_ALLOCATION_TEST_CHECK(test, code[endOfFirstTest - 2] == shortJumpIfNotZero && endOfFirstTest + code[endOfFirstTest - 1] == endOfSecondTest - 6);

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t mixed = allocationTestMixedAll(std::span<const uint64_t>(words).first(count));
    for (uint64_t way : { 0u, 1u, 2u, 3u, 4u })
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way, words.data()) == ((way & 3) ? ~uint64_t { 0 } : mixed));
}
#endif

// No instruction takes the constant in itself, and no word of the instance holds it. The loop leaves registers over, so the edge into it makes the constant once.
template<typename Word>
static void allocatorMakesConstantOnceAheadOfLoop()
{
    constexpr Type type = allocationTestTypeOf<Word>;
    constexpr Word added = static_cast<Word>(0x0123456789abcdll);
    TestProcedure test;
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LValue constant;
    if constexpr (std::is_same_v<Word, double>)
        constant = out.constDouble(added);
    else
        constant = out.constInt64(static_cast<int64_t>(added));
    AOT_TEST_CHECK(!findPinnedConstant(static_cast<uint64_t>(constant->constantBits())));
    LValue start = out.load(allocationTestWordAt(test, buffer, 0), type);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock odd = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, type);
    out.upsilon(rounds, left);
    out.upsilon(start, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    LValue nextSum = out.add(sum, constant);
    out.branch(out.testNonZero64(left, 1), normalEdge(odd), normalEdge(latch));

    out.appendTo(odd);
    out.store64(left, allocationTestWordAt(test, buffer, 2));
    out.jump(normalEdge(latch));

    out.appendTo(latch);
    out.upsilon(nextSum, sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.store(sum, allocationTestWordAt(test, buffer, 1));
    out.ret(rounds);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(head).isLoopHead && order.of(latch).loop == order.of(head).loop);
    std::span<Operation* const> listed = test.procedure().liveness().of(order.loops()[order.of(head).loop], order).valuesReadFromOutside;
    AOT_ALLOCATION_TEST_CHECK(test, std::ranges::count(listed, constant) == 1);
    AOT_ALLOCATION_TEST_CHECK(test, text.count("materialize"_s, constant) == 1);
    for (const Block* block : order.blocksInOrder()) {
        if (order.of(block).loop != noLoop)
            AOT_ALLOCATION_TEST_CHECK(test, !text.count("materialize"_s, constant, block) && allocationTestRegisterAtHead(test, block, constant) == allocationTestRegisterAtHead(test, head, constant));
    }
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, head, constant));

    std::array<Word, 64> words = allocationTestWords<Word>();
    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        Word expected = words[0];
        for (uint64_t round = 0; round < numberOfRounds; ++round)
            expected += added;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, words.data()) == numberOfRounds);
        AOT_ALLOCATION_TEST_CHECK(test, words[1] == expected);
    }
}

// The call ahead of the loop costs the spilled values their registers. The loop makes no call and leaves registers over, so the edge into it loads them once.
static void allocatorLoadsSpilledValuesOnceAheadOfLoop()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP)) + 2;
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, count, Int64);
    LValue left = nullptr;
    LValue sum = nullptr;
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    left = out.phiIn(head, Int64);
    sum = out.phiIn(head, Int64);
    LValue roundsAsLoaded = out.load64(allocationTestWordAt(test, buffer, count));
    allocationTestAppendCall(test, nullptr);
    out.upsilon(roundsAsLoaded, left);
    out.upsilon(out.int64Zero, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    out.upsilon(out.add(sum, allocationTestMixAll(out, values.span())), sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.ret(sum);
    UNUSED_PARAM(rounds);
    compileOrFail(test);

    AllocationTestText text(test);
    const Allocation& allocation = test.procedure().allocation();
    unsigned numberSpilled = 0;
    for (LValue value : values) {
        numberSpilled += allocation.isSpilled(value);
        AOT_ALLOCATION_TEST_CHECK(test, !text.count("reload"_s, value, head) && !text.count("reload"_s, value, body));
        if (allocation.isSpilled(value))
            AOT_ALLOCATION_TEST_CHECK(test, text.count("reload"_s, value) == 1 && !allocationTestReadsInPlace(test, value));
    }
    AOT_ALLOCATION_TEST_CHECK(test, numberSpilled >= 2);

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    uint64_t mixed = allocationTestMixedAll(std::span<const uint64_t>(words).first(count));
    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        words[count] = numberOfRounds;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 0 }, words.data()) == numberOfRounds * mixed);
    }
}

// At its fullest the loop asks for every register that costs nothing to take: those that no callee saves, or those that its Patch leaves alone.
// Held at the head, the constant would be the first to lose its register, and every edge back would make it again. So it is made where it is read.
template<bool loopHasPatch>
static void allocatorKeepsNoConstantThatLoopWouldEvict()
{
    constexpr uint64_t added = 0x0123456789abcd;
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    RegisterMask savedByNoCallee = file.allocatableIn(Bank::GP) & ~file.calleeSaved;
    unsigned numberOfWords = loopHasPatch ? 0 : allocationTestCountOf(savedByNoCallee) - 3;
    RegisterMask leftAlone = 0;
    RegisterMask others = savedByNoCallee & ~(maskOf(rounds->argumentReg()) | maskOf(buffer->argumentReg()));
    for (unsigned count = 0; count < 3; ++count) {
        leftAlone |= others & ~(others - 1);
        others &= others - 1;
    }
    LValue constant = out.constInt64(static_cast<int64_t>(added));
    AOT_TEST_CHECK(!findPinnedConstant(added));
    LValue start = out.load64(allocationTestWordAt(test, buffer, 0));
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, Int64);
    out.upsilon(rounds, left);
    out.upsilon(start, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    if constexpr (loopHasPatch)
        allocationTestAppendPatchThatTakes(test, file.allocatableIn(Bank::GP) & ~leftAlone);
    LValue nextSum = out.add(sum, constant);
    LValue stored = left;
    if constexpr (!loopHasPatch)
        stored = allocationTestMixAll(out, allocationTestLoadWords(test, buffer, 2, numberOfWords, Int64).span());
    out.store64(stored, allocationTestWordAt(test, buffer, 1));
    out.upsilon(nextSum, sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.store64(sum, allocationTestWordAt(test, buffer, 0));
    out.ret(left);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(head).isLoopHead && order.of(body).loop == order.of(head).loop);
    const LoopFacts& loopFacts = test.procedure().liveness().of(order.loops()[order.of(head).loop], order);
    AOT_ALLOCATION_TEST_CHECK(test, std::ranges::count(loopFacts.valuesReadFromOutside, constant) == 1);
    if constexpr (loopHasPatch) {
        RegisterMask held = 0;
        for (LValue value : { buffer, left, sum }) {
            Reg reg = allocationTestRegisterAtHead(test, head, value);
            AOT_ALLOCATION_TEST_CHECK(test, reg);
            held |= maskOf(reg);
        }
        AOT_ALLOCATION_TEST_CHECK(test, held == leftAlone);
    } else
        AOT_ALLOCATION_TEST_CHECK(test, loopFacts.pressure[static_cast<unsigned>(Bank::GP)] >= allocationTestCountOf(savedByNoCallee));
    AOT_ALLOCATION_TEST_CHECK(test, !allocationTestRegisterAtHead(test, head, constant));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("materialize"_s, constant) == 1 && text.count("materialize"_s, constant, body) == 1);

    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        uint64_t expectedSum = words[0] + numberOfRounds * added;
        uint64_t expectedWord = words[1];
        if (numberOfRounds)
            expectedWord = loopHasPatch ? 1 : allocationTestMixedAll(std::span<const uint64_t>(words).subspan(2, numberOfWords));
        AOT_ALLOCATION_TEST_CHECK(test, !test.call<uint64_t>(numberOfRounds, words.data()));
        AOT_ALLOCATION_TEST_CHECK(test, words[0] == expectedSum && words[1] == expectedWord);
    }
}

// for (; left; left--) { sum += constant; mix of words } return sum. The loop is left at its head alone, so behind the addition the sum of the head is dead, however far its range goes in the order.
// Counted there, it would make the loop look one register fuller than it is. With one word fewer than fill the registers that no callee saves, the constant has room.
template<bool sumIsReadBehindLoop>
static void scanCountsPhiThatIsReadBehindLoopUpToItsLastReadInLoop()
{
    constexpr uint64_t added = 0x0123456789abcd;
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    unsigned numberSavedByNoCallee = allocationTestCountOf(file.allocatableIn(Bank::GP) & ~file.calleeSaved);
    unsigned numberOfWords = numberSavedByNoCallee - 4;
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LValue constant = out.constInt64(static_cast<int64_t>(added));
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    // The addition stands between the loads and their users, so that x86-64 fuses none of the loads.
    Vector<LValue> words = allocationTestLoadWords(test, buffer, 2, numberOfWords, Int64);
    LValue nextSum = out.add(sum, constant);
    out.store64(allocationTestMixAll(out, words.span()), allocationTestWordAt(test, buffer, 1));
    out.store64(nextSum, allocationTestWordAt(test, buffer, 0));
    out.upsilon(nextSum, sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.ret(sumIsReadBehindLoop ? sum : out.int64Zero);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Loop& loop = order.loops()[order.of(head).loop];
    const LoopFacts& loopFacts = test.procedure().liveness().of(loop, order);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(head).isLoopHead && loopFacts.endOfLastExitingBlock == order.endOf(head) && order.headOf(exit) == loop.end);
    // left, the sum as it goes on, the buffer and the words.
    AOT_ALLOCATION_TEST_CHECK(test, loopFacts.pressure[static_cast<unsigned>(Bank::GP)] == numberOfWords + 3);
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, head, constant));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("materialize"_s, constant) == 1 && !text.count("materialize"_s, constant, body) && !test.procedure().allocation().usedCalleeSaves());

    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        std::array<uint64_t, 64> memory = allocationTestWords<uint64_t>();
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, memory.data()) == (sumIsReadBehindLoop ? numberOfRounds * added : 0));
        AOT_ALLOCATION_TEST_CHECK(test, !numberOfRounds || memory[0] == numberOfRounds * added);
    }
}

// The rare block stands behind the join and leads back to it, so the order has a loop there. It is run through once: a constant held at the join would be made on every way in and read once.
static void allocatorKeepsNoConstantAtJoinThatRareBlockLeadsBackTo()
{
    constexpr uint64_t added = 0x0123456789abcd;
    TestProcedure test;
    Output& out = test.out();
    LValue first = test.argumentGPR(0);
    LValue second = test.argumentGPR(1);
    LValue constant = out.constInt64(static_cast<int64_t>(added));
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock done = out.newBlock();
    LValue word = out.phiIn(join, Int64);
    out.branch(out.testNonZero64(first, 1), normalEdge(left), normalEdge(right));

    out.appendTo(left);
    out.upsilon(first, word);
    out.jump(normalEdge(join));

    out.appendTo(right);
    out.upsilon(second, word);
    out.jump(normalEdge(join));

    out.appendTo(join);
    LValue sum = out.add(word, constant);
    out.branch(out.testNonZero64(word, 2), rareEdge(slow), normalEdge(done));

    out.appendTo(slow);
    out.upsilon(out.sub(word, 2), word);
    out.jump(normalEdge(join));

    out.appendTo(done);
    out.ret(sum);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(join).isLoopHead && order.of(slow).isRare() && order.headOf(join) < order.headOf(slow));
    AOT_ALLOCATION_TEST_CHECK(test, std::ranges::count(test.procedure().liveness().of(order.loops()[order.of(join).loop], order).valuesReadFromOutside, constant) == 1);
    AOT_ALLOCATION_TEST_CHECK(test, !allocationTestRegisterAtHead(test, join, constant));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("materialize"_s, constant) == 1 && text.count("materialize"_s, constant, join) == 1);

    for (auto [a, b] : { std::pair<uint64_t, uint64_t> { 5, 0 }, { 7, 0 }, { 4, 9 }, { 4, 10 } }) {
        uint64_t chosen = a & 1 ? a : b;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(a, b) == (chosen & ~uint64_t { 2 }) + added);
    }
}

// The extension is no instruction in the register that the number is made in, and takes that register over. Held through the loop, the number would be copied on every turn, which on x86-64 costs what making it does.
static void allocatorKeepsNoConstantWhoseReaderTakesItsRegister()
{
    TestProcedure test;
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    LValue number = out.number(0);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    LValue extended = out.zeroExtPtr(number);
    out.upsilon(out.sub(extended, sum), sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.ret(sum);
    prepareOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    AOT_ALLOCATION_TEST_CHECK(test, number->opcode() == NumberOf && extended->opcode() == ZExt32 && isFreeInRegisterOfChild(test.procedure().liveness(), extended));
    AOT_ALLOCATION_TEST_CHECK(test, std::ranges::count(test.procedure().liveness().of(order.loops()[order.of(head).loop], order).valuesReadFromOutside, number) == (isX86_64() ? 0 : 1));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("materialize"_s, number) == 1 && text.count("materialize"_s, number, body) == (isX86_64() ? 1u : 0u));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, head, number).isSet() == !isX86_64());
}

static void allocatorExtendsNumberWhereItIsMade()
{
    TestProcedure test;
    Output& out = test.out();
    LValue number = out.number(0);
    LValue extended = out.zeroExtPtr(number);
    out.ret(out.sub(extended, test.argumentGPR(0)));
    prepareOrFail(test);

    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, number->opcode() == NumberOf && extended->opcode() == ZExt32 && test.procedure().liveness().of(extended).has(ValueFlag::UpperHalfIsRead));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("materialize"_s, number) == 1 && !text.count("move"_s, number));
    unsigned numberOfExtensions = 0;
    for (const Step& step : test.procedure().allocation().steps()) {
        if (step.kind != StepKind::Execute || step.operation != extended)
            continue;
        ++numberOfExtensions;
        AOT_ALLOCATION_TEST_CHECK(test, step.destination.isRegister() && step.destination == step.sources[0]);
    }
    AOT_ALLOCATION_TEST_CHECK(test, numberOfExtensions == 1);
}

static void allocatorMakesConstantBelowOpaqueInRegisterOfResult()
{
    constexpr uint64_t bits = 0x0123456789abcd;
    TestProcedure test;
    Output& out = test.out();
    LValue constant = out.constInt64(static_cast<int64_t>(bits));
    AOT_TEST_CHECK(!findPinnedConstant(bits));
    LValue opaque = out.opaque(constant);
    out.ret(out.add(opaque, test.argumentGPR(0)));
    compileOrFail(test);

    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, opaque->opcode() == Opaque && !text.count("materialize"_s, constant) && !text.count("move"_s, constant));
    unsigned numberOfSteps = 0;
    for (const Step& step : test.procedure().allocation().steps()) {
        if (step.kind != StepKind::Execute || step.operation != opaque)
            continue;
        ++numberOfSteps;
        AOT_ALLOCATION_TEST_CHECK(test, step.destination.isRegister() && step.sources[0].isImmediate());
    }
    AOT_ALLOCATION_TEST_CHECK(test, numberOfSteps == 1);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 5 }) == bits + 5);
}

// What the loop reads is live to the end of the loop, which is the head of the block behind it. There it is dead, and the register that it held is free for what is returned.
static void allocatorDefinesValueReturnedBehindLoopInReturnRegister()
{
    TestProcedure test;
    Output& out = test.out();
    LValue first = test.argumentGPR(0);
    LValue rounds = test.argumentGPR(1);
    LValue buffer = test.argumentGPR(2);
    LValue loaded = out.load64(allocationTestWordAt(test, buffer, 0));
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    out.upsilon(out.add(out.add(sum, first), loaded), sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    LValue returned = out.add(sum, 1);
    out.ret(returned);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    Reg returnRegister(GPRInfo::returnValueGPR);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(head).isLoopHead && order.headOf(exit) == order.loops()[order.of(head).loop].end);
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, head, first) == returnRegister || allocationTestRegisterAtHead(test, head, loaded) == returnRegister);
    AOT_ALLOCATION_TEST_CHECK(test, !allocationTestRegisterAtHead(test, exit, first) && !allocationTestRegisterAtHead(test, exit, loaded));
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().allocation().registerAtDefinitionOf(returned) == returnRegister && !text.count("move"_s, nullptr, exit));

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    for (uint64_t numberOfRounds : { 0u, 1u, 3u })
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 9 }, numberOfRounds, words.data()) == numberOfRounds * (9 + words[0]) + 1);
}

// for (; left; left--) if (left & 1) { if (left & 2) count++; } Two arms hand the Phi of the loop on as it is, one of them a level further out. Each has one predecessor, which stands ahead of the arm that counts.
static void allocatorBringsCountThatInnerArmAddsToRoundInItsRegister()
{
    TestProcedure test;
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock inner = out.newBlock();
    LBasicBlock counts = out.newBlock();
    LBasicBlock skipsInner = out.newBlock();
    LBasicBlock joinInner = out.newBlock();
    LBasicBlock skipsOuter = out.newBlock();
    LBasicBlock joinOuter = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue count = out.phiIn(head, Int64);
    LValue joinedInner = out.phiIn(joinInner, Int64);
    LValue joinedOuter = out.phiIn(joinOuter, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, count);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    out.branch(out.testNonZero64(left, 1), normalEdge(inner), normalEdge(skipsOuter));

    out.appendTo(inner);
    out.branch(out.testNonZero64(left, 2), normalEdge(counts), normalEdge(skipsInner));

    out.appendTo(counts);
    LValue nextCount = out.add(count, 1);
    out.upsilon(nextCount, joinedInner);
    out.jump(normalEdge(joinInner));

    out.appendTo(skipsInner);
    out.upsilon(count, joinedInner);
    out.jump(normalEdge(joinInner));

    out.appendTo(joinInner);
    out.store64(left, allocationTestWordAt(test, buffer, 1));
    out.upsilon(joinedInner, joinedOuter);
    out.jump(normalEdge(joinOuter));

    out.appendTo(skipsOuter);
    out.upsilon(count, joinedOuter);
    out.jump(normalEdge(joinOuter));

    out.appendTo(joinOuter);
    out.upsilon(joinedOuter, count);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.store64(count, allocationTestWordAt(test, buffer, 0));
    out.ret(rounds);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    for (const Block* arm : { skipsInner, skipsOuter })
        AOT_ALLOCATION_TEST_CHECK(test, order.of(arm).predecessors.size() == 1 && order.of(order.of(arm).predecessors.front()).orderIndex < order.of(counts).orderIndex && order.of(counts).orderIndex < order.of(arm).orderIndex);
    Reg registerOfCount = allocation.registerAtDefinitionOf(count);
    AOT_ALLOCATION_TEST_CHECK(test, registerOfCount && allocation.registerAtDefinitionOf(nextCount) == registerOfCount);
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(joinedInner) == registerOfCount && allocation.registerAtDefinitionOf(joinedOuter) == registerOfCount);
    for (const Block* block : order.blocksInOrder()) {
        if (order.of(block).loop != noLoop)
            AOT_ALLOCATION_TEST_CHECK(test, !text.count("move"_s, nullptr, block));
    }
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("swap"_s) && !text.count("save"_s) && !text.count("reload"_s));

    for (uint64_t numberOfRounds : { 0u, 1u, 3u, 8u }) {
        std::array<uint64_t, 2> stored { ~uint64_t { 0 }, 0 };
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, stored.data()) == numberOfRounds);
        AOT_ALLOCATION_TEST_CHECK(test, stored[0] == (numberOfRounds + 1) / 4);
    }
}

// As a typed `count++` is built: the sum is tested, and a rare block that leaves reads the count as it was. So the count outlives the addition, and is dead only behind the test.
static void allocatorKeepsCountThatRareLeaveReadsUpToTest()
{
    TestProcedure test;
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    LValue limit = test.argumentGPR(1);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock counts = out.newBlock();
    LBasicBlock goesOn = out.newBlock();
    LBasicBlock leaves = out.newBlock();
    LBasicBlock skips = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue count = out.phiIn(head, Int64);
    LValue joined = out.phiIn(join, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, count);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    out.branch(out.testNonZero64(left, 1), normalEdge(counts), normalEdge(skips));

    out.appendTo(counts);
    LValue nextCount = out.add(count, 1);
    out.branch(out.lessThan(nextCount, limit), normalEdge(goesOn), rareEdge(leaves));

    out.appendTo(goesOn);
    out.upsilon(nextCount, joined);
    out.jump(normalEdge(join));

    out.appendTo(leaves);
    out.ret(out.add(count, 1000));

    out.appendTo(skips);
    out.upsilon(count, joined);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.upsilon(joined, count);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.ret(count);
    compileOrFail(test);

    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    Reg registerOfCount = allocation.registerAtDefinitionOf(count);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(leaves).isRare() && order.of(goesOn).isReached() && order.of(goesOn).predecessors.size() == 1);
    AOT_ALLOCATION_TEST_CHECK(test, registerOfCount && allocation.registerAtDefinitionOf(nextCount) != registerOfCount);
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, leaves, count) == registerOfCount && !allocationTestRegisterAtHead(test, goesOn, count));

    for (uint64_t numberOfRounds : { 0u, 1u, 4u, 7u }) {
        for (uint64_t most : { 1u, 3u, 100u }) {
            uint64_t counted = (numberOfRounds + 1) / 2;
            uint64_t expected = counted < most ? counted : most - 1 + 1000;
            AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, most) == expected);
        }
    }
}

// The loop leaves by a rare chain of two blocks, which ends in a block that another rare arm behind the call shares. So the whole region stands behind the call in the order,
// and by their ranges the Phis that the chain reads live across the call. No path leads from the call to the chain: the Phis take no callee-saved register and no spill, whose store would stand at the head of the loop.
static void allocatorKeepsPhisThatRareChainBehindLaterCallReadsOutOfMemory()
{
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    LValue rounds = test.argumentGPR(0);
    LValue limit = test.argumentGPR(1);
    LValue buffer = test.argumentGPR(2);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock behindLoop = out.newBlock();
    LBasicBlock done = out.newBlock();
    LBasicBlock leavesLoop = out.newBlock();
    LBasicBlock leavesLoopFurtherOn = out.newBlock();
    LBasicBlock leavesBehindCall = out.newBlock();
    LBasicBlock sharedEnd = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, Int64);
    out.upsilon(rounds, left);
    out.upsilon(out.int64Zero, sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(behindLoop));

    out.appendTo(body);
    LValue nextSum = out.add(sum, left);
    out.branch(out.lessThan(nextSum, limit), normalEdge(latch), rareEdge(leavesLoop));

    out.appendTo(latch);
    out.upsilon(nextSum, sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(behindLoop);
    PatchOperation* call = allocationTestAppendCall(test, nullptr);
    out.branch(out.testNonZero64(limit, 1), rareEdge(leavesBehindCall), normalEdge(done));

    out.appendTo(done);
    out.ret(out.load64(allocationTestWordAt(test, buffer, 0)));

    out.appendTo(leavesLoop);
    out.store64(left, allocationTestWordAt(test, buffer, 1));
    out.jump(normalEdge(leavesLoopFurtherOn));

    out.appendTo(leavesLoopFurtherOn);
    out.store64(sum, allocationTestWordAt(test, buffer, 2));
    out.jump(normalEdge(sharedEnd));

    out.appendTo(leavesBehindCall);
    out.store64(limit, allocationTestWordAt(test, buffer, 3));
    out.jump(normalEdge(sharedEnd));

    out.appendTo(sharedEnd);
    out.ret(out.load64(allocationTestWordAt(test, buffer, 4)));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Liveness& liveness = test.procedure().liveness();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(head).isLoopHead && order.of(leavesLoop).isRare() && order.of(leavesLoop).rareRegion == order.of(leavesBehindCall).rareRegion);
    AOT_ALLOCATION_TEST_CHECK(test, order.earlyPositionOf(call) < order.headOf(leavesLoop) && order.headOf(leavesLoop) < order.headOf(leavesLoopFurtherOn));
    for (LValue phi : { left, sum }) {
        AOT_ALLOCATION_TEST_CHECK(test, liveness.of(phi).lastUse > order.earlyPositionOf(call) && !liveness.of(phi).has(ValueFlag::LivesAcrossCall));
        AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(phi) && allocation.registerAtDefinitionOf(phi) && !(maskOf(allocation.registerAtDefinitionOf(phi)) & file.calleeSaved));
        AOT_ALLOCATION_TEST_CHECK(test, !allocationTestRegisterAtHead(test, behindLoop, phi) && allocationTestRegisterAtHead(test, leavesLoop, phi));
        AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s, phi) && !text.count("reload"_s, phi));
    }

    for (uint64_t numberOfRounds : { 0u, 1u, 4u }) {
        for (uint64_t most : { 2u, 5u, 100u, 101u }) {
            std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
            std::array<uint64_t, 64> expected = words;
            uint64_t result = words[0];
            uint64_t added = 0;
            bool hasLeft = false;
            for (uint64_t round = numberOfRounds; round && !hasLeft; --round) {
                if (added + round < most) {
                    added += round;
                    continue;
                }
                hasLeft = true;
                expected[1] = round;
                expected[2] = added;
                result = words[4];
            }
            if (!hasLeft && (most & 1)) {
                expected[3] = most;
                result = words[4];
            }
            AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, most, words.data()) == result);
            AOT_ALLOCATION_TEST_CHECK(test, words == expected);
        }
    }
}

// The pressure of the loop counts what a rare arm alone reads, and such a value asks for a spare register like one that only lives through. Counted twice, it would cost `through` its register.
static void allocatorCountsWhatRareArmAloneReadsOnce()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned numberOfRegisters = allocationTestCountOf(registerFileFor(test.procedure().traits()).allocatableIn(Bank::GP));
    unsigned numberOfWords = numberOfRegisters - 4;
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LValue through = out.load64(allocationTestWordAt(test, buffer, 0));
    LValue readByRareArm = out.load64(allocationTestWordAt(test, buffer, 1));
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    out.upsilon(rounds, left);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    out.store64(allocationTestMixAll(out, allocationTestLoadWords(test, buffer, 4, numberOfWords, Int64).span()), allocationTestWordAt(test, buffer, 2));
    out.branch(out.testNonZero64(left, 2), rareEdge(slow), normalEdge(latch));

    out.appendTo(slow);
    out.store64(readByRareArm, allocationTestWordAt(test, buffer, 3));
    out.jump(normalEdge(latch));

    out.appendTo(latch);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.ret(through);
    compileOrFail(test);

    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(head).isLoopHead && order.of(slow).isRare() && order.of(slow).loop == order.of(head).loop);
    unsigned pressure = test.procedure().liveness().of(order.loops()[order.of(head).loop], order).pressure[static_cast<unsigned>(Bank::GP)];
    // One of the registers that the pressure asks for is that of the value itself.
    bool bothFit = numberOfRegisters + 1 >= pressure + 2;
    AOT_ALLOCATION_TEST_CHECK(test, pressure + 2 >= numberOfRegisters);
    AOT_ALLOCATION_TEST_CHECK(test, allocation.isSpilled(through) == !bothFit && allocationTestRegisterAtHead(test, head, through).isSet() == bothFit);

    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        uint64_t mixed = allocationTestMixedAll(std::span<const uint64_t>(words).subspan(4, numberOfWords));
        uint64_t expectedOfRareArm = numberOfRounds >= 2 ? words[1] : words[3];
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, words.data()) == words[0]);
        AOT_ALLOCATION_TEST_CHECK(test, words[2] == (numberOfRounds ? mixed : allocationTestWords<uint64_t>()[2]) && words[3] == expectedOfRareArm);
    }
}

// The other arm reads the owner further on in the order, so the range of the owner ends behind the last read of its storage and the scan records no read for the collector.
// No way leads from this arm to that read. The collector needs the owner across the call all the same: a pointer at what a cell owns keeps nothing alive.
static void allocatorKeepsOwnerThatOtherArmReadsWhileItsStorageIsRead()
{
    std::array<uint64_t, 3> elements { 11, 22, 33 };
    std::array<const uint64_t*, 1> ownerInMemory { elements.data() };

    TestProcedure test;
    Output& out = test.out();
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
    LValue owner = out.opaque(test.argumentGPR(0));
    LValue selector = test.argumentGPR(1);
    LValue storage = out.loadPtr(out.address(storageOfOwner, owner));
    LBasicBlock readsStorage = out.newBlock();
    LBasicBlock readsOwner = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), normalEdge(readsStorage), normalEdge(readsOwner));

    out.appendTo(readsStorage);
    PatchOperation* call = allocationTestAppendCall(test, nullptr);
    call->effects.mayCollect = true;
    LValue element = out.load64(out.address(test.heaps().argumentBuffer, storage, 2 * sizeof(uint64_t)));
    out.ret(element);

    out.appendTo(readsOwner);
    out.ret(owner);
    compileOrFail(test);

    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    const ValueFacts& facts = test.procedure().liveness().of(owner);
    AOT_ALLOCATION_TEST_CHECK(test, storage->has(OperationFlag::IsInnerPointer) && facts.has(ValueFlag::IsKeptForCollector));
    AOT_ALLOCATION_TEST_CHECK(test, order.headOf(readsStorage) < order.headOf(readsOwner) && facts.lastUse > order.earlyPositionOf(element));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.isSpilled(owner) || allocationTestRegisterAtHead(test, readsStorage, owner));

    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 1 }) == 33);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 0 }) == reinterpret_cast<uint64_t>(ownerInMemory.data()));
}

// As a twin is entered: the primary entrypoint leads into both loops behind their starts.
// `atOuter` is defined ahead of the inner loop and read behind it in the order, in a register that calls keep. The way in from the side never defines it, and nothing in the inner loop reads it.
static void allocatorDropsWhatSideEntranceOfLoopLacks()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock laterEntry = test.procedure().addEntrypoint(1);
    LBasicBlock outer = out.newBlock();
    LBasicBlock between = out.newBlock();
    LBasicBlock inner = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock again = out.newBlock();
    LBasicBlock leave = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue atOuter = out.phiIn(outer, Int64);
    LValue atInner = out.phiIn(inner, Int64);
    LValue atBody = out.phiIn(body, Int64);
    out.upsilon(test.argumentGPR(0), atBody);
    out.jump(normalEdge(body));

    out.appendTo(laterEntry);
    out.upsilon(test.argumentGPR(0), atOuter);
    out.jump(normalEdge(outer));

    out.appendTo(outer);
    out.branch(out.testNonZero64(atOuter, 1), normalEdge(exit), normalEdge(between));

    out.appendTo(between);
    out.upsilon(atOuter, atInner);
    out.jump(normalEdge(inner));

    out.appendTo(inner);
    out.upsilon(atInner, atBody);
    out.jump(normalEdge(body));

    out.appendTo(body);
    allocationTestAppendCall(test, nullptr);
    LValue next = out.sub(atBody, 1);
    out.branch(out.testNonZero64(next, 2), normalEdge(again), normalEdge(leave));

    out.appendTo(again);
    out.upsilon(next, atInner);
    out.jump(normalEdge(inner));

    out.appendTo(leave);
    out.upsilon(next, atOuter);
    out.jump(normalEdge(outer));

    out.appendTo(exit);
    out.ret(atOuter);
    compileOrFail(test);

    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(outer).isLoopHead && order.of(inner).isLoopHead && order.of(body).loop == order.of(inner).loop);
    AOT_ALLOCATION_TEST_CHECK(test, order.headOf(outer) < order.headOf(inner) && order.loops()[order.of(inner).loop].end <= order.headOf(exit));
    AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(atOuter) && allocationTestRegisterAtHead(test, between, atOuter));
    AOT_ALLOCATION_TEST_CHECK(test, !allocationTestRegisterAtHead(test, inner, atOuter));

    for (uint64_t start : { 8u, 5u, 2u }) {
        uint64_t expected = start - 1;
        while ((expected & 3) != 1)
            --expected;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(start) == expected);
    }
}

// One entrypoint. Both arms lead to one rare block, which leads on to a block of the left one. That block stands behind both arms and the rare block, so no edge leads back.
// `onLeft` is defined on the left arm alone, and nothing behind the rare block reads it. Neither the rare block nor what it rejoins holds it.
static void allocatorDropsWhatWayThroughSharedRareBlockLacks()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock left = out.newBlock();
    LBasicBlock further = out.newBlock();
    LBasicBlock right = out.newBlock();
    LBasicBlock shared = out.newBlock();
    LBasicBlock rejoin = out.newBlock();
    LBasicBlock aside = out.newBlock();
    LBasicBlock furtherAside = out.newBlock();
    LBasicBlock other = out.newBlock();
    LValue word = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    out.branch(out.testNonZero64(word, 1), normalEdge(left), normalEdge(right));

    out.appendTo(left);
    LValue onLeft = out.add(word, 5);
    out.branch(out.testNonZero64(word, 2), unlikelyEdge(aside), normalEdge(further));

    out.appendTo(further);
    out.branch(out.testNonZero64(word, 4), rareEdge(shared), normalEdge(rejoin));

    out.appendTo(right);
    out.branch(out.testNonZero64(word, 4), rareEdge(shared), normalEdge(other));

    out.appendTo(shared);
    out.store64(word, allocationTestWordAt(test, buffer, 0));
    out.jump(normalEdge(rejoin));

    out.appendTo(rejoin);
    out.ret(out.add(word, word));

    out.appendTo(aside);
    out.store64(word, allocationTestWordAt(test, buffer, 1));
    out.jump(normalEdge(furtherAside));

    out.appendTo(furtherAside);
    out.ret(onLeft);

    out.appendTo(other);
    out.ret(out.sub(word, 1));
    compileOrFail(test);

    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(shared).isRare() && order.loops().empty());
    AOT_ALLOCATION_TEST_CHECK(test, order.headOf(aside) < order.headOf(right) && order.headOf(right) < order.headOf(shared) && order.headOf(shared) < order.headOf(rejoin));
    // Of the two ways into the rare block, the first in the order is the one that has the value.
    AOT_ALLOCATION_TEST_CHECK(test, order.of(shared).predecessors.front() == further);
    AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(onLeft) && allocationTestRegisterAtHead(test, further, onLeft));
    AOT_ALLOCATION_TEST_CHECK(test, !allocationTestRegisterAtHead(test, shared, onLeft) && !allocationTestRegisterAtHead(test, rejoin, onLeft));

    for (uint64_t argument : { 3u, 5u, 1u, 4u, 8u }) {
        bool goesLeft = argument & 1;
        bool goesAside = goesLeft && (argument & 2);
        bool goesRareWay = !goesAside && (argument & 4);
        uint64_t expected = argument - 1;
        if (goesAside)
            expected = argument + 5;
        else if (goesLeft || goesRareWay)
            expected = argument + argument;
        std::array<uint64_t, 2> stored { };
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(argument, stored.data()) == expected);
        AOT_ALLOCATION_TEST_CHECK(test, stored[0] == (goesRareWay ? argument : 0) && stored[1] == (goesAside ? argument : 0));
    }
}

// Two ways lead into one rare region. On the first `value` is live by its range, and a call there saves it. The second starts where the value is dead: the other arm alone reads it.
// Where the two meet, the second has no place to bring it from: the save of the first is none.
static void allocatorLoadsNothingFromSaveOfOtherWayIntoRareRegion()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock first = out.newBlock();
    LBasicBlock without = out.newBlock();
    LBasicBlock reading = out.newBlock();
    LBasicBlock on = out.newBlock();
    LBasicBlock saving = out.newBlock();
    LBasicBlock secondWayIn = out.newBlock();
    LBasicBlock shared = out.newBlock();
    LValue word = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LValue value = out.add(word, 5);
    out.branch(out.testNonZero64(word, 1), rareEdge(saving), normalEdge(first));

    out.appendTo(first);
    out.branch(out.testNonZero64(word, 2), normalEdge(without), normalEdge(reading));

    out.appendTo(without);
    out.branch(out.testNonZero64(word, 4), rareEdge(secondWayIn), normalEdge(on));

    out.appendTo(on);
    out.ret(out.add(word, word));

    out.appendTo(reading);
    out.ret(value);

    out.appendTo(saving);
    allocationTestAppendCall(test, nullptr);
    out.jump(normalEdge(shared));

    out.appendTo(secondWayIn);
    out.store64(word, allocationTestWordAt(test, buffer, 0));
    out.jump(normalEdge(shared));

    out.appendTo(shared);
    out.ret(out.sub(word, 1));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(saving).isRare() && order.of(secondWayIn).rareRegion == order.of(saving).rareRegion && order.of(shared).rareRegion == order.of(saving).rareRegion);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(saving).orderIndex < order.of(secondWayIn).orderIndex && order.of(secondWayIn).orderIndex < order.of(shared).orderIndex);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(without).orderIndex < order.of(reading).orderIndex && order.headOf(shared) < order.headOf(reading));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, saving, value) && !allocationTestRegisterAtHead(test, without, value));
    AOT_ALLOCATION_TEST_CHECK(test, !test.procedure().allocation().isSpilled(value) && text.count("save"_s, value, saving) == 1);
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("reload"_s, value, secondWayIn) && !allocationTestRegisterAtHead(test, shared, value));

    for (uint64_t argument : { 0u, 1u, 2u, 6u, 8u }) {
        bool goesSecondWayIn = !(argument & 1) && (argument & 2) && (argument & 4);
        uint64_t expected = argument + 5;
        if ((argument & 1) || goesSecondWayIn)
            expected = argument - 1;
        else if (argument & 2)
            expected = argument + argument;
        uint64_t stored = 0;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(argument, &stored) == expected);
        AOT_ALLOCATION_TEST_CHECK(test, stored == (goesSecondWayIn ? argument : 0));
    }
}

// `sum` is a Phi of a loop that is left at its head, and is read behind the loop. Both arms of a rare region read it. The first saves it round a call.
// The second reads it for the last time and then leads to the head of a loop of its own: the save of the first arm is no place to load it from there.
static void allocatorLoadsNothingFromSaveOfOtherRareArm()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock saving = out.newBlock();
    LBasicBlock looping = out.newBlock();
    LBasicBlock inner = out.newBlock();
    LBasicBlock behindInner = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue sum = out.phiIn(head, Int64);
    LValue left = out.phiIn(head, Int64);
    LValue turns = out.phiIn(inner, Int64);
    LValue nextSum = out.phiIn(latch, Int64);
    out.upsilon(test.argumentGPR(0), left);
    out.upsilon(test.argumentGPR(1), sum);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    out.upsilon(out.add(sum, 1), nextSum);
    out.branch(out.testNonZero64(left, 1), rareEdge(slow), normalEdge(latch));

    out.appendTo(slow);
    out.branch(out.testNonZero64(left, 2), rareEdge(looping), normalEdge(saving));

    out.appendTo(saving);
    allocationTestAppendCall(test, nullptr);
    out.upsilon(out.add(sum, 3), nextSum);
    out.jump(normalEdge(latch));

    out.appendTo(looping);
    LValue sumOfLoopingArm = out.add(sum, 5);
    out.upsilon(out.constInt64(2), turns);
    out.jump(normalEdge(inner));

    out.appendTo(inner);
    LValue turnsLeft = out.sub(turns, 1);
    out.upsilon(turnsLeft, turns);
    out.branch(turnsLeft, normalEdge(inner), normalEdge(behindInner));

    out.appendTo(behindInner);
    out.upsilon(sumOfLoopingArm, nextSum);
    out.jump(normalEdge(latch));

    out.appendTo(latch);
    out.upsilon(nextSum, sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.ret(sum);
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(saving).isRare() && order.of(looping).rareRegion == order.of(saving).rareRegion && order.of(inner).rareRegion == order.of(saving).rareRegion);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(saving).orderIndex < order.of(looping).orderIndex && order.of(inner).isLoopHead && order.of(looping).loop == order.of(head).loop);
    AOT_ALLOCATION_TEST_CHECK(test, order.loops()[order.of(head).loop].end <= order.headOf(exit));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, slow, sum) && !test.procedure().allocation().isSpilled(sum));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("save"_s, sum, saving) == 1 && text.count("save"_s, sum) == 1);
    AOT_ALLOCATION_TEST_CHECK(test, text.count("reload"_s, sum) == text.count("reload"_s, sum, saving));

    constexpr uint64_t start = 100;
    for (uint64_t rounds : { 0u, 1u, 2u, 3u, 7u }) {
        uint64_t expected = start;
        for (uint64_t round = rounds; round; --round)
            expected += !(round & 1) ? 1 : (round & 2) ? 5 : 3;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(rounds, start) == expected);
    }
}

template<typename Word>
static void allocatorKeepsRareArmOffHotPath()
{
    constexpr Type type = allocationTestTypeOf<Word>;
    TestProcedure test;
    Output& out = test.out();
    LValue selector = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LValue counter = test.argumentGPR(2);
    Vector<LValue> values = allocationTestLoadWords(test, buffer, 0, 2, type);
    LBasicBlock slow = out.newBlock();
    LBasicBlock join = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), rareEdge(slow), normalEdge(join));

    out.appendTo(slow);
    allocationTestAppendCall(test, counter);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.ret(allocationTestMixAll(out, values.span()));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(slow).isRare() && text.verbsOf(slow) == "save save Patch reload reload Jump"_s);
    for (const Block* block : order.blocksInOrder()) {
        if (!order.of(block).isRare())
            AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s, nullptr, block) && !text.count("reload"_s, nullptr, block));
    }
    for (LValue value : values)
        AOT_ALLOCATION_TEST_CHECK(test, !test.procedure().liveness().of(value).has(ValueFlag::LivesAcrossCall) && !allocation.isSpilled(value));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.spills().size() == values.size());
    for (const SpillFacts& spill : allocation.spills())
        AOT_ALLOCATION_TEST_CHECK(test, spill.isSaveInRareRegion && values.contains(spill.value));

    AOT_ALLOCATION_TEST_CHECK(test, !allocation.usedCalleeSaves() && !text.mentions("usedCalleeSaves"_s));
    AOT_ALLOCATION_TEST_CHECK(test, !allocation.needsFrameOutsideRareRegions() && !text.mentions("needsFrameOutsideRareRegions"_s));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.needsFrameIn(order.of(slow).rareRegion) && text.mentions("needsFrameIn rare region"_s));
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().frame().coverage() == FrameCoverage::RareRegions);

    std::array<Word, 64> words = allocationTestWords<Word>();
    for (uint64_t goesSlowWay : { 0u, 1u }) {
        uint64_t numberOfCalls = 0;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<Word>(goesSlowWay, words.data(), &numberOfCalls) == allocationTestMixed(words[0], words[1]));
        AOT_ALLOCATION_TEST_CHECK(test, numberOfCalls == goesSlowWay);
    }
}

// The constants are read again sooner than any of the values, so going by the distance of the next read alone would spill a value.
static void allocatorEvictsConstantsFirst()
{
    constexpr unsigned numberOfConstants = 4;
    auto constantAt = [](unsigned index) {
        return static_cast<int64_t>(0x0123456789abcdefll + index * 0x100000001ll);
    };

    TestProcedure test;
    Output& out = test.out();
    unsigned numberOfValues = allocationTestCountOf(registerFileFor(test.procedure().traits()).allocatableIn(Bank::GP)) - 3;
    LValue input = test.argumentGPR(0);
    LValue output = test.argumentGPR(1);
    auto storeConstants = [&](unsigned firstWord) {
        for (unsigned index = 0; index < numberOfConstants; ++index) {
            AOT_TEST_CHECK(!findPinnedConstant(static_cast<uint64_t>(constantAt(index))));
            out.store64(out.constInt64(constantAt(index)), allocationTestWordAt(test, output, firstWord + index));
        }
    };
    storeConstants(0);
    Vector<LValue> values = allocationTestLoadWords(test, input, 0, numberOfValues, Int64);
    storeConstants(numberOfConstants);
    out.ret(allocationTestMixAll(out, values.span()));
    compileOrFail(test);

    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().allocation().spills().empty());
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("save"_s) && !text.count("reload"_s));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("materialize"_s) > numberOfConstants);
    // There are more values than registers that no callee saves, and a callee-saved register is all that asks for a frame here.
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().frame().hasFrame() == !!test.procedure().allocation().usedCalleeSaves());

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    std::array<uint64_t, 2 * numberOfConstants> stored { };
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(words.data(), stored.data()) == allocationTestMixedAll(std::span<const uint64_t>(words).first(numberOfValues)));
    for (unsigned index = 0; index < stored.size(); ++index)
        AOT_ALLOCATION_TEST_CHECK(test, stored[index] == static_cast<uint64_t>(constantAt(index % numberOfConstants)));
}

// ARM64 has the tag in a register. x86-64 has it in a word of the instance, which the instruction reads.
static void allocatorReadsPinnedConstantWhereItLies()
{
    constexpr int64_t tag = JSValue::NumberTag;
    std::optional<PinnedConstant> pinned = findPinnedConstant(static_cast<uint64_t>(tag));
    AOT_TEST_CHECK(pinned);

    TestProcedure test;
    Output& out = test.out();
    LValue sum = out.add(test.argumentGPR(0), out.constInt64(tag));
    out.ret(sum);

    int64_t offsetOfTag = pinned->offset.value_or(0);
    AOT_TEST_CHECK(!(offsetOfTag % static_cast<int64_t>(sizeof(uint64_t))));
    int64_t wordOfTag = offsetOfTag / static_cast<int64_t>(sizeof(uint64_t));
    int64_t numberOfWordsEachWay = std::abs(wordOfTag) + 1;
    Vector<uint64_t> wordsAroundInstance(FillWith { }, static_cast<size_t>(2 * numberOfWordsEachWay), uint64_t { 0 });
    wordsAroundInstance[static_cast<size_t>(numberOfWordsEachWay + wordOfTag)] = static_cast<uint64_t>(tag);
    compileOrFail(test, &wordsAroundInstance[static_cast<size_t>(numberOfWordsEachWay)]);

    Location placeOfTag;
    for (const Step& step : test.procedure().allocation().steps()) {
        if (step.kind == StepKind::Execute && step.operation == sum)
            placeOfTag = step.sources[1];
    }
    if (pinned->offset)
        AOT_ALLOCATION_TEST_CHECK(test, placeOfTag.kind() == Location::InstanceWord && placeOfTag.offsetInInstance() == *pinned->offset);
    else
        AOT_ALLOCATION_TEST_CHECK(test, placeOfTag == Location::reg(pinned->reg));
    AOT_ALLOCATION_TEST_CHECK(test, !AllocationTestText(test).count("materialize"_s));

    for (uint64_t argument : { uint64_t { 0 }, uint64_t { 41 }, ~uint64_t { 0 } })
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(argument) == argument + static_cast<uint64_t>(tag));
}

static constexpr Origin allocationTestOriginOfExtension(7);

static ProcedureTraits allocationTestTraitsThatNoteOrigins()
{
    ProcedureTraits traits;
    traits.notesRangesOfOrigins = true;
    return traits;
}

static ptrdiff_t allocationTestBytesWrittenFor(TestProcedure& test, Origin origin)
{
    ptrdiff_t bytes = 0;
    for (const RangeOfOrigin& range : test.procedure().rangesOfOrigins()) {
        if (range.origin == origin)
            bytes += MacroAssembler::differenceBetween(range.begin, range.end);
    }
    return bytes;
}

// An instruction that writes 32 bits of a register clears the upper half. A Trunc is no instruction, so above its word lies what lay there in the Int64.
template<bool wordIsWrittenAs32Bits>
static void allocatorExtendsWordThatIsReadWhole()
{
    TestProcedure test(allocationTestTraitsThatNoteOrigins());
    Output& out = test.out();
    LValue first = out.castToInt32(test.argumentGPR(0));
    LValue word = wordIsWrittenAs32Bits ? out.add(first, out.castToInt32(test.argumentGPR(1))) : first;
    out.setOrigin(allocationTestOriginOfExtension);
    LValue extended = out.zeroExtPtr(word);
    out.setOrigin(Origin());
    out.ret(extended);
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, extended->opcode() == ZExt32 && test.procedure().liveness().of(extended).has(ValueFlag::UpperHalfIsRead));
    AOT_ALLOCATION_TEST_CHECK(test, !wordIsWrittenAs32Bits || allocation.registerAtDefinitionOf(extended) == allocation.registerAtDefinitionOf(word));
    AOT_ALLOCATION_TEST_CHECK(test, !allocationTestBytesWrittenFor(test, allocationTestOriginOfExtension) == wordIsWrittenAs32Bits);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 0xfffffff0fffffff0 }, uint64_t { 0x0000000100000020 }) == (wordIsWrittenAs32Bits ? 0x10u : 0xfffffff0u));
}

// The upper half of a Phi of 32 bits is clear where it is clear above every word that comes in: the count of a loop is an index as it stands. A Trunc among them brings along what lay above its word.
template<bool startsAtConstant>
static void allocatorExtendsCountOfLoop()
{
    TestProcedure test(allocationTestTraitsThatNoteOrigins());
    Output& out = test.out();
    LBasicBlock loop = out.newBlock();
    LBasicBlock behindLoop = out.newBlock();
    LValue end = out.castToInt32(test.argumentGPR(0));
    LValue count = out.phiIn(loop, Int32);
    LValue sum = out.phiIn(loop, Int64);
    out.upsilon(startsAtConstant ? out.constInt32(1) : out.castToInt32(test.argumentGPR(1)), count);
    out.upsilon(out.constInt64(0), sum);
    out.jump(normalEdge(loop));

    out.appendTo(loop);
    LValue nextCount = out.add(count, out.constInt32(1));
    out.setOrigin(allocationTestOriginOfExtension);
    LValue extended = out.zeroExtPtr(count);
    out.setOrigin(Origin());
    LValue nextSum = out.add(sum, extended);
    out.upsilon(nextCount, count);
    out.upsilon(nextSum, sum);
    out.branch(out.notEqual(nextCount, end), normalEdge(loop), normalEdge(behindLoop));

    out.appendTo(behindLoop);
    out.ret(nextSum);
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, extended->opcode() == ZExt32 && test.procedure().liveness().of(extended).has(ValueFlag::UpperHalfIsRead));
    AOT_ALLOCATION_TEST_CHECK(test, count->has(OperationFlag::UpperHalfMayBeSet) == !startsAtConstant);
    AOT_ALLOCATION_TEST_CHECK(test, !allocationTestBytesWrittenFor(test, allocationTestOriginOfExtension) == startsAtConstant);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 0xfffffff000000004 }, uint64_t { 0xfffffff000000001 }) == 6);
}

static const Step& allocationTestExecuteStepOf(TestProcedure& test, const Operation* operation)
{
    for (const Step& step : test.procedure().allocation().steps()) {
        if (step.kind == StepKind::Execute && step.operation == operation)
            return step;
    }
    allocationTestFail(test, __FILE__, __LINE__, "the operation is executed");
}

// base + (index << 3): one instruction of four bytes, which reads the index where it lies whether or not it lives on.
template<bool indexLivesOn>
static void encoderAddsShiftedIndexInOneInstruction()
{
    TestProcedure test(allocationTestTraitsThatNoteOrigins());
    Output& out = test.out();
    LValue base = test.argumentGPR(0);
    LValue index = test.argumentGPR(1);
    out.setOrigin(allocationTestOriginOfExtension);
    LValue sum = out.add(base, out.shl(index, 3));
    out.setOrigin(Origin());
    out.ret(indexLivesOn ? out.bitXor(sum, index) : sum);
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, sum->opcode() == Add && sum->child(1)->opcode() == Shl && addsScaledIndex(test.procedure().liveness(), sum));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestBytesWrittenFor(test, allocationTestOriginOfExtension) == 4);
    AOT_ALLOCATION_TEST_CHECK(test, !AllocationTestText(test).count("Shl"_s));
    const Step& step = allocationTestExecuteStepOf(test, sum);
    AOT_ALLOCATION_TEST_CHECK(test, step.sources[0] == Location::reg(allocation.registerAtDefinitionOf(base)) && step.sources[1] == Location::reg(allocation.registerAtDefinitionOf(index)) && step.sources[2].isImmediate());
    AOT_ALLOCATION_TEST_CHECK(test, !indexLivesOn || step.destination != step.sources[1]);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1000 }, uint64_t { 5 }) == (indexLivesOn ? (1040u ^ 5u) : 1040u));
}

// cage base + (reference << 3), and the same from a place further into the cage, as SlotEmitter::extensionAt() asks for it. The reference is read again behind the sum.
// On x86-64 the distance from the cage base is the displacement. ARM64 has no room for it, so the base is made in a register.
template<bool baseHasOffset>
static void encoderDecodesReferenceInOneInstruction()
{
    constexpr int64_t offset = baseHasOffset ? 0x800000 : 0;
    TestProcedure test(allocationTestTraitsThatNoteOrigins());
    Output& out = test.out();
    LValue buffer = test.argumentGPR(0);
    LValue reference = out.load32(allocationTestWordAt(test, buffer, 0));
    reference->markAsNarrowReference();
    LValue base = out.argumentRegister(cellCageBaseGPR);
    if (baseHasOffset)
        base = out.add(base, out.constIntPtr(offset));
    out.setOrigin(allocationTestOriginOfExtension);
    LValue extended = out.zeroExtPtr(reference);
    LValue pointer = out.add(base, out.shl(extended, 3));
    out.setOrigin(Origin());
    out.store32(reference, allocationTestWordAt(test, buffer, 1));
    out.ret(pointer);
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    const Allocation& allocation = test.procedure().allocation();
    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, base->hasNoPosition() && pointer->opcode() == Add && pointer->child(0) == base && extended->opcode() == ZExt32);
    AOT_ALLOCATION_TEST_CHECK(test, addsScaledIndex(liveness, pointer) && liveness.isFused(extended) && !liveness.isFused(reference) && reference->isNarrowReference());
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("Shl"_s) && !text.count("ZExt32"_s) && !text.count("move"_s, reference));

    const Step& step = allocationTestExecuteStepOf(test, pointer);
    AOT_ALLOCATION_TEST_CHECK(test, step.sources[1] == Location::reg(allocation.registerAtDefinitionOf(reference)) && step.destination != step.sources[1]);
    bool baseStandsInInstruction = baseHasOffset && isX86_64();
    AOT_ALLOCATION_TEST_CHECK(test, baseHasOffset || step.sources[0] == Location::reg(cellCageBaseGPR));
    AOT_ALLOCATION_TEST_CHECK(test, step.sources[0].isImmediate() == baseStandsInInstruction && text.count("materialize"_s) == (baseHasOffset && !baseStandsInInstruction));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestBytesWrittenFor(test, allocationTestOriginOfExtension) == (baseStandsInInstruction ? 8 : 4));

    std::array<uint64_t, 2> words { 0xdeadbeef80000005, 0 };
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(words.data()) == (uint64_t { 0x80000005 } << 3) + offset);
    AOT_ALLOCATION_TEST_CHECK(test, words[1] == 0x80000005);
}

// The constant beside a pinned register is the displacement of x86-64 where it has 32 bits. Any other is made in a register together with its base.
static void encoderAddsScaledIndexToPinnedRegisterWithConstant()
{
    TestInstance instance;
    instance.fillPinnedConstants();
    uint64_t addressOfInstance = reinterpret_cast<uintptr_t>(instance.pointer());
    for (int64_t constant : { int64_t { 8 }, int64_t { -8 }, int64_t { 0x7fffffff }, -int64_t { 0x80000000 }, int64_t { 0x80000000 }, int64_t { 0x123456789a } }) {
        TestProcedure test;
        Output& out = test.out();
        LValue base = out.add(out.argumentRegister(instanceGPR), out.constIntPtr(constant));
        LValue sum = out.add(base, out.shl(test.argumentGPR(0), 2));
        out.ret(sum);
        compileOrFail(test, instance.pointer());

        AOT_ALLOCATION_TEST_CHECK(test, base->hasNoPosition() && addsScaledIndex(test.procedure().liveness(), sum));
        AOT_ALLOCATION_TEST_CHECK(test, scaledSumAcceptsBase(*base) == (isX86_64() && isRepresentableAs<int32_t>(constant)));
        AOT_ALLOCATION_TEST_CHECK(test, allocationTestExecuteStepOf(test, sum).sources[0].isImmediate() == scaledSumAcceptsBase(*base));
        for (uint64_t index : { uint64_t { 0 }, uint64_t { 5 }, ~uint64_t { 0 }, uint64_t { 0x4000000000000001 } })
            AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(index) == addressOfInstance + static_cast<uint64_t>(constant) + (index << 2));
    }
}

// A rare block alone reads the sum, so the sum is computed there, and the shift with it.
static void encoderAddsScaledIndexWhereSumSinks()
{
    TestProcedure test(allocationTestTraitsThatNoteOrigins());
    Output& out = test.out();
    LValue selector = test.argumentGPR(0);
    LValue base = test.argumentGPR(1);
    LValue index = test.argumentGPR(2);
    LValue buffer = test.argumentGPR(3);
    out.setOrigin(allocationTestOriginOfExtension);
    LValue sum = out.add(base, out.shl(index, 3));
    out.setOrigin(Origin());
    LBasicBlock slow = out.newBlock();
    LBasicBlock join = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), rareEdge(slow), normalEdge(join));

    out.appendTo(slow);
    out.store64(sum, allocationTestWordAt(test, buffer, 0));
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.ret(index);
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, liveness.isSunk(sum) && addsScaledIndex(liveness, sum) && !liveness.isSunk(sum->child(1)));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("Add"_s, sum, slow) == 1 && text.count("Add"_s) == 1 && !text.count("Shl"_s));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestBytesWrittenFor(test, allocationTestOriginOfExtension) == 4);

    for (uint64_t goesSlowWay : { 0u, 1u }) {
        uint64_t stored = 0;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(goesSlowWay, uint64_t { 1000 }, uint64_t { 5 }, &stored) == 5);
        AOT_ALLOCATION_TEST_CHECK(test, stored == (goesSlowWay ? 1040u : 0u));
    }
}

// The shift is made ahead of the other operand, so the builder leaves it the first child.
static void encoderAddsShiftedIndexThatIsFirstChild()
{
    TestProcedure test(allocationTestTraitsThatNoteOrigins());
    Output& out = test.out();
    LValue base = test.argumentGPR(0);
    LValue index = test.argumentGPR(1);
    LValue shifted = out.shl(index, 2);
    LValue other = out.bitXor(base, index);
    out.setOrigin(allocationTestOriginOfExtension);
    LValue sum = out.add(shifted, other);
    out.setOrigin(Origin());
    out.ret(sum);
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, sum->opcode() == Add && sum->child(0) == shifted && sum->child(1) == other && addsScaledIndex(test.procedure().liveness(), sum));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestBytesWrittenFor(test, allocationTestOriginOfExtension) == 4 && !AllocationTestText(test).count("Shl"_s));
    const Step& step = allocationTestExecuteStepOf(test, sum);
    AOT_ALLOCATION_TEST_CHECK(test, step.sources[0] == Location::reg(allocation.registerAtDefinitionOf(index)) && step.sources[1].isImmediate() && step.sources[2] == Location::reg(allocation.registerAtDefinitionOf(other)));
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1000 }, uint64_t { 5 }) == 20u + (1000u ^ 5u));
}

// The shift stands in an earlier block than the sum, and a rare block alone reads the sum.
static void encoderAddsScaledIndexFromEarlierBlockWhereSumSinks()
{
    TestProcedure test(allocationTestTraitsThatNoteOrigins());
    Output& out = test.out();
    LValue selector = test.argumentGPR(0);
    LValue base = test.argumentGPR(1);
    LValue index = test.argumentGPR(2);
    LValue buffer = test.argumentGPR(3);
    LBasicBlock middle = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock join = out.newBlock();
    out.setOrigin(allocationTestOriginOfExtension);
    LValue shifted = out.shl(index, 3);
    out.setOrigin(Origin());
    out.branch(out.testNonZero64(selector, 2), normalEdge(middle), normalEdge(join));

    out.appendTo(middle);
    out.setOrigin(allocationTestOriginOfExtension);
    LValue sum = out.add(base, shifted);
    out.setOrigin(Origin());
    out.branch(out.testNonZero64(selector, 1), rareEdge(slow), normalEdge(join));

    out.appendTo(slow);
    out.store64(sum, allocationTestWordAt(test, buffer, 0));
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.ret(index);
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, shifted->owner() != sum->owner() && sum->child(1) == shifted);
    AOT_ALLOCATION_TEST_CHECK(test, liveness.isSunk(sum) && addsScaledIndex(liveness, sum) && liveness.isFused(shifted));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("Add"_s, sum, slow) == 1 && text.count("Add"_s) == 1 && !text.count("Shl"_s));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestBytesWrittenFor(test, allocationTestOriginOfExtension) == 4);

    for (uint64_t way : { 0u, 1u, 2u, 3u }) {
        uint64_t stored = 0;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way, uint64_t { 1000 }, uint64_t { 5 }, &stored) == 5);
        AOT_ALLOCATION_TEST_CHECK(test, stored == (way == 3 ? 1040u : 0u));
    }
}

static unsigned allocationTestNumberOfLeavesThatAreNoChildren(const Liveness& liveness, const Operation* operation)
{
    unsigned result = 0;
    forEachLeaf(liveness, operation, [&](const Operation* user, unsigned childIndex, const Operation* leaf) {
        result += user->child(childIndex) != leaf;
    });
    return result;
}

// The decoded reference where the word is above the markers, else the word, as ConversionEmitter::decode() asks for it.
// The test reads the extended word, so the word ends at its extension, which is no instruction: a sum, a comparison and a choice are left.
static void encoderDecodesReferenceOrMarkerWithoutCopy()
{
    constexpr uint32_t lastMarker = 10;
    TestProcedure test(allocationTestTraitsThatNoteOrigins());
    Output& out = test.out();
    LValue buffer = test.argumentGPR(0);
    LValue reference = out.load32(allocationTestWordAt(test, buffer, 0));
    reference->markAsNarrowReference();
    out.setOrigin(allocationTestOriginOfExtension);
    LValue marker = out.zeroExtPtr(reference);
    LValue isCell = out.above(reference, out.constInt32(lastMarker));
    LValue pointer = out.add(out.argumentRegister(cellCageBaseGPR), out.shl(marker, 3));
    LValue value = out.select(isCell, pointer, marker);
    out.setOrigin(Origin());
    out.store64(value, allocationTestWordAt(test, buffer, 1));
    out.ret(buffer);
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, value->opcode() == Select && value->child(0) == isCell && isCell->child(0) == reference && value->child(1) == pointer && value->child(2) == marker);
    AOT_ALLOCATION_TEST_CHECK(test, liveness.isFused(isCell) && addsScaledIndex(liveness, pointer) && !liveness.isFused(marker) && !liveness.isFused(reference));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestNumberOfLeavesThatAreNoChildren(liveness, value) == 1);
    const Step& step = allocationTestExecuteStepOf(test, value);
    AOT_ALLOCATION_TEST_CHECK(test, step.sources[0] == Location::reg(allocation.registerAtDefinitionOf(marker)) && step.sources[0] == step.sources[3] && step.sources[1].isImmediate());
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(marker) == allocation.registerAtDefinitionOf(reference));
    AllocationTestText text(test);
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("move"_s, reference) && !text.count("move"_s, marker));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestBytesWrittenFor(test, allocationTestOriginOfExtension) <= 12);

    for (uint32_t word : { 0u, 2u, lastMarker, lastMarker + 1, 0x80000005u, 0xffffffffu }) {
        std::array<uint64_t, 2> words { 0xdeadbeef00000000 | word, 0 };
        test.call<uint64_t>(words.data());
        AOT_ALLOCATION_TEST_CHECK(test, words[1] == (word > lastMarker ? uint64_t { word } << 3 : word));
    }
}

static bool allocationTestCompareWords(Opcode relation, uint32_t left, uint32_t right)
{
    int32_t signedLeft = static_cast<int32_t>(left);
    int32_t signedRight = static_cast<int32_t>(right);
    switch (relation) {
    case Equal:
        return left == right;
    case NotEqual:
        return left != right;
    case LessThan:
        return signedLeft < signedRight;
    case GreaterEqual:
        return signedLeft >= signedRight;
    case Above:
        return left > right;
    case BelowEqual:
        return left <= right;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whatever the relation, the extension, the arm and what lies above the word in its register, the test goes by the word.
static void encoderTestsWordInArmThatExtendsIt()
{
    constexpr uint32_t bound = 0xfffffffb;
    static constexpr uint64_t arguments[] = { 0, 5, 0x7fffffff, 0x80000000, bound - 1, bound, bound + 1, 0xffffffff, 0xdeadbeef00000005, 0xdeadbeeffffffffb, 0x1234567880000000 };
    for (Opcode relation : { Equal, NotEqual, LessThan, GreaterEqual, Above, BelowEqual }) {
        for (Opcode extension : { ZExt32, SExt32 }) {
            for (bool extensionIsFirstArm : { false, true }) {
                for (bool wordIsOnTheRight : { false, true }) {
                    TestProcedure test;
                    Output& out = test.out();
                    LValue word = out.castToInt32(test.argumentGPR(0));
                    LValue otherWord = out.castToInt32(test.argumentGPR(1));
                    LValue otherArm = test.argumentGPR(2);
                    LValue extended = out.unary(extension, word);
                    LValue holds = wordIsOnTheRight ? out.binary(relation, otherWord, word) : out.binary(relation, word, out.constInt32(static_cast<int32_t>(bound)));
                    LValue value = extensionIsFirstArm ? out.select(holds, extended, otherArm) : out.select(holds, otherArm, extended);
                    out.ret(value);
                    compileOrFail(test);

                    const Liveness& liveness = test.procedure().liveness();
                    AOT_ALLOCATION_TEST_CHECK(test, value->opcode() == Select && liveness.isFused(value->child(0)));
                    AOT_ALLOCATION_TEST_CHECK(test, allocationTestNumberOfLeavesThatAreNoChildren(liveness, value) == 1);
                    for (uint64_t argument : arguments) {
                        uint32_t low = static_cast<uint32_t>(argument);
                        uint64_t wide = extension == ZExt32 ? low : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(low)));
                        bool expected = wordIsOnTheRight ? allocationTestCompareWords(relation, bound, low) : allocationTestCompareWords(relation, low, bound);
                        uint64_t other = 0x0123456789abcdef;
                        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(argument, uint64_t { 0xfeedface00000000 } | bound, other) == (expected == extensionIsFirstArm ? wide : other));
                    }
                }
            }
        }
    }
}

struct AllocationTestSumWithShift {
    LValue (*build)(Output&, LValue, LValue);
    uint64_t (*compute)(uint64_t, uint64_t);
};

// The shifted index has a second reader, is both operands, is shifted further than an address of x86-64 scales, has 32 bits, stands below an extension that one target has a form for, or stands beside another.
// Whether or not one instruction adds it, the answer is that of the operations.
static void encoderAddsShiftedIndexThatOneInstructionMayNotAdd()
{
    static const AllocationTestSumWithShift sums[] = {
        { [](Output& out, LValue base, LValue index) -> LValue { return out.add(base, out.shl(out.zeroExtPtr(out.castToInt32(index)), 3)); }, [](uint64_t base, uint64_t index) -> uint64_t { return base + (static_cast<uint64_t>(static_cast<uint32_t>(index)) << 3); } },
        { [](Output& out, LValue base, LValue index) -> LValue { return out.add(base, out.shl(out.signExt32To64(out.castToInt32(index)), 2)); }, [](uint64_t base, uint64_t index) -> uint64_t { return base + (static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(index))) << 2); } },
        { [](Output& out, LValue base, LValue index) -> LValue { return out.add(base, out.shl(out.signExt32To64(out.castToInt32(index)), 5)); }, [](uint64_t base, uint64_t index) -> uint64_t { return base + (static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(index))) << 5); } },
        { [](Output& out, LValue base, LValue index) -> LValue { return out.add(base, out.shl(out.zeroExtPtr(out.add(out.castToInt32(index), 1)), 3)); }, [](uint64_t base, uint64_t index) -> uint64_t { return base + (static_cast<uint64_t>(static_cast<uint32_t>(index) + 1) << 3); } },
        { [](Output& out, LValue base, LValue index) -> LValue { return out.add(out.shl(base, 1), out.shl(index, 2)); }, [](uint64_t base, uint64_t index) -> uint64_t { return (base << 1) + (index << 2); } },
        { [](Output& out, LValue base, LValue index) -> LValue {
            LValue sum = out.add(base, out.shl(index, 3));
            return out.bitXor(out.mul(sum, sum), index);
        }, [](uint64_t base, uint64_t index) -> uint64_t { return ((base + (index << 3)) * (base + (index << 3))) ^ index; } },
        { [](Output& out, LValue base, LValue index) -> LValue {
            LValue shifted = out.shl(index, 3);
            return out.bitXor(out.add(base, shifted), shifted);
        }, [](uint64_t base, uint64_t index) -> uint64_t { return (base + (index << 3)) ^ (index << 3); } },
        { [](Output& out, LValue base, LValue index) -> LValue {
            LValue shifted = out.shl(index, 3);
            return out.bitXor(out.add(shifted, shifted), base);
        }, [](uint64_t base, uint64_t index) -> uint64_t { return ((index << 3) + (index << 3)) ^ base; } },
        { [](Output& out, LValue base, LValue index) -> LValue { return out.add(out.shl(index, 2), base); }, [](uint64_t base, uint64_t index) -> uint64_t { return (index << 2) + base; } },
        { [](Output& out, LValue base, LValue index) -> LValue {
            LValue shifted = out.shl(index, 2);
            return out.add(shifted, out.bitXor(base, index));
        }, [](uint64_t base, uint64_t index) -> uint64_t { return (index << 2) + (base ^ index); } },
        { [](Output& out, LValue base, LValue index) -> LValue { return out.add(base, out.shl(index, 4)); }, [](uint64_t base, uint64_t index) -> uint64_t { return base + (index << 4); } },
        { [](Output& out, LValue base, LValue index) -> LValue { return out.add(base, out.shl(index, 63)); }, [](uint64_t base, uint64_t index) -> uint64_t { return base + (index << 63); } },
        { [](Output& out, LValue, LValue index) -> LValue { return out.add(index, out.shl(index, 1)); }, [](uint64_t, uint64_t index) -> uint64_t { return index + (index << 1); } },
        { [](Output& out, LValue base, LValue index) -> LValue {
            return out.zeroExtPtr(out.add(out.castToInt32(base), out.shl(out.castToInt32(index), 3)));
        }, [](uint64_t base, uint64_t index) -> uint64_t { return static_cast<uint32_t>(base) + (static_cast<uint32_t>(index) << 3); } },
    };
    static constexpr std::array<uint64_t, 2> arguments[] = {
        { 1000, 5 },
        { 0xfffffffffffffff8, 0x2000000000000001 },
        { 0x8000000080000000, 0xf0000000f0000003 },
        { 0x0123456789abcdef, 0xffffffffffffffff },
    };
    for (const AllocationTestSumWithShift& sum : sums) {
        TestProcedure test;
        Output& out = test.out();
        out.ret(sum.build(out, test.argumentGPR(0), test.argumentGPR(1)));
        compileOrFail(test);
        for (auto [base, index] : arguments)
            AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(base, index) == sum.compute(base, index));
    }
}

// (a + b) + 26 of 32 bits. On x86-64 that is one lea of four bytes, whether or not an operand lives on.
template<bool operandLivesOn>
static void encoderAddsSumAndConstantInOneInstruction()
{
    TestProcedure test(allocationTestTraitsThatNoteOrigins());
    Output& out = test.out();
    LValue first = out.castToInt32(test.argumentGPR(0));
    LValue second = out.castToInt32(test.argumentGPR(1));
    out.setOrigin(allocationTestOriginOfExtension);
    LValue sum = out.add(out.add(first, second), 26);
    out.setOrigin(Origin());
    out.ret(out.zeroExtPtr(operandLivesOn ? out.bitXor(sum, second) : sum));
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, sum->opcode() == Add && sum->child(0)->opcode() == Add && sum->child(1)->isInt32(26));
    if (isX86_64())
        AOT_ALLOCATION_TEST_CHECK(test, allocationTestBytesWrittenFor(test, allocationTestOriginOfExtension) == 4);
    static constexpr std::array<uint64_t, 2> arguments[] = {
        { 1000, 5 },
        { 0xfffffff07fffffff, 0x1234567800000001 },
        { 0xdeadbeefffffffe6, 0xffffffff00000000 },
    };
    for (auto [a, b] : arguments) {
        uint32_t expected = static_cast<uint32_t>(a) + static_cast<uint32_t>(b) + 26;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(a, b) == (operandLivesOn ? expected ^ static_cast<uint32_t>(b) : expected));
    }
}

struct AllocationTestWordOperation {
    LValue (*build)(Output&, LValue, LValue);
    uint32_t (*compute)(uint32_t, uint32_t);
};

// The operands are Truncs of arguments that have something above their low halves, so an instruction that let any of it through would be seen.
static void allocatorFindsUpperHalfClearBehindOperationsThatClearIt()
{
    static const AllocationTestWordOperation operations[] = {
        { [](Output& out, LValue a, LValue b) -> LValue { return out.add(a, b); }, [](uint32_t a, uint32_t b) -> uint32_t { return a + b; } },
        { [](Output& out, LValue a, LValue) -> LValue { return out.add(a, 5); }, [](uint32_t a, uint32_t) -> uint32_t { return a + 5; } },
        { [](Output& out, LValue a, LValue b) -> LValue { return out.sub(a, b); }, [](uint32_t a, uint32_t b) -> uint32_t { return a - b; } },
        { [](Output& out, LValue a, LValue b) -> LValue { return out.mul(a, b); }, [](uint32_t a, uint32_t b) -> uint32_t { return a * b; } },
        { [](Output& out, LValue a, LValue) -> LValue { return out.neg(a); }, [](uint32_t a, uint32_t) -> uint32_t { return 0 - a; } },
        { [](Output& out, LValue a, LValue b) -> LValue { return out.bitAnd(a, b); }, [](uint32_t a, uint32_t b) -> uint32_t { return a & b; } },
        { [](Output& out, LValue a, LValue b) -> LValue { return out.bitOr(a, b); }, [](uint32_t a, uint32_t b) -> uint32_t { return a | b; } },
        { [](Output& out, LValue a, LValue b) -> LValue { return out.bitXor(a, b); }, [](uint32_t a, uint32_t b) -> uint32_t { return a ^ b; } },
        { [](Output& out, LValue a, LValue) -> LValue { return out.bitNot(a); }, [](uint32_t a, uint32_t) -> uint32_t { return ~a; } },
        { [](Output& out, LValue a, LValue) -> LValue { return out.shl(a, 1); }, [](uint32_t a, uint32_t) -> uint32_t { return a << 1; } },
        { [](Output& out, LValue a, LValue) -> LValue { return out.shl(a, 3); }, [](uint32_t a, uint32_t) -> uint32_t { return a << 3; } },
        { [](Output& out, LValue a, LValue) -> LValue { return out.aShr(a, 3); }, [](uint32_t a, uint32_t) -> uint32_t { return static_cast<uint32_t>(static_cast<int32_t>(a) >> 3); } },
        { [](Output& out, LValue a, LValue) -> LValue { return out.lShr(a, 3); }, [](uint32_t a, uint32_t) -> uint32_t { return a >> 3; } },
        { [](Output& out, LValue a, LValue) -> LValue { return out.ctlz32(a); }, [](uint32_t a, uint32_t) -> uint32_t { return static_cast<uint32_t>(std::countl_zero(a)); } },
        { [](Output& out, LValue a, LValue) -> LValue { return out.unary(SExt8, a); }, [](uint32_t a, uint32_t) -> uint32_t { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(a))); } },
        { [](Output& out, LValue a, LValue) -> LValue { return out.unary(SExt16, a); }, [](uint32_t a, uint32_t) -> uint32_t { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(a))); } },
        { [](Output& out, LValue a, LValue b) -> LValue { return out.equal(a, b); }, [](uint32_t a, uint32_t b) -> uint32_t { return a == b; } },
        { [](Output& out, LValue a, LValue b) -> LValue { return out.lessThan(a, b); }, [](uint32_t a, uint32_t b) -> uint32_t { return static_cast<int32_t>(a) < static_cast<int32_t>(b); } },
        { [](Output& out, LValue a, LValue b) -> LValue { return out.below(a, b); }, [](uint32_t a, uint32_t b) -> uint32_t { return a < b; } },
    };
    static constexpr std::array<uint64_t, 2> arguments[] = {
        { 0xfffffff080000001, 0x123456787fffffff },
        { 0xdeadbeef00000000, 0xffffffffffffffff },
        { 0x00000001000000ff, 0x80000000000000ff },
    };

    for (const AllocationTestWordOperation& operation : operations) {
        TestProcedure test;
        Output& out = test.out();
        LValue first = out.castToInt32(test.argumentGPR(0));
        LValue second = out.castToInt32(test.argumentGPR(1));
        LValue word = operation.build(out, first, second);
        out.ret(out.zeroExtPtr(word));
        compileOrFail(test);

        AOT_ALLOCATION_TEST_CHECK(test, clearsUpperHalfOfRegister(word->opcode()));
        for (auto [a, b] : arguments)
            AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(a, b) == operation.compute(static_cast<uint32_t>(a), static_cast<uint32_t>(b)));
    }
}

static uint64_t allocationTestHashed(uint64_t rounds, uint32_t word, bool wholeHashIsRead)
{
    int64_t hash = 0;
    for (; rounds; --rounds)
        hash = static_cast<int32_t>(static_cast<uint32_t>(hash) * 31 ^ word);
    return wholeHashIsRead ? static_cast<uint64_t>(hash) : static_cast<uint32_t>(hash);
}

// let h = 0; for (..) h = (h * 31 ^ c) | 0, in a variable that is kept as an Int64. Where bitwise operators alone read h, nobody sees what lies above its low half.
template<bool wholeHashIsRead>
static void allocatorExtendsWordOnItsWayRoundLoop()
{
    TestProcedure test(allocationTestTraitsThatNoteOrigins());
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    LValue word = out.castToInt32(test.argumentGPR(1));
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue hash = out.phiIn(head, Int64);
    LValue left = out.phiIn(head, Int64);
    out.upsilon(out.int64Zero, hash);
    out.upsilon(rounds, left);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    LValue mixed = out.bitXor(out.mul(out.castToInt32(hash), 31), word);
    out.setOrigin(allocationTestOriginOfExtension);
    LValue extended = out.signExt32To64(mixed);
    out.setOrigin(Origin());
    out.upsilon(extended, hash);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.ret(wholeHashIsRead ? hash : out.zeroExtPtr(out.castToInt32(hash)));
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, extended->opcode() == SExt32 && hash->has(OperationFlag::UpperHalfIsRead) == wholeHashIsRead);
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().liveness().of(extended).has(ValueFlag::UpperHalfIsRead) == wholeHashIsRead);
    AOT_ALLOCATION_TEST_CHECK(test, !!allocationTestBytesWrittenFor(test, allocationTestOriginOfExtension) == wholeHashIsRead);

    constexpr uint64_t argument = 0xdeadbeef9e3779b9;
    for (uint64_t numberOfRounds : { 0u, 1u, 2u, 5u })
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, argument) == allocationTestHashed(numberOfRounds, static_cast<uint32_t>(argument), wholeHashIsRead));
}

// The Int64 is read again behind the Trunc, so the result of the Trunc could not take its register. The one instruction that reads the low half reads it where the Int64 lies.
// With two readers the Trunc is a value like any other.
template<bool lowHalfIsReadTwice>
static void allocatorReadsLowHalfWhereWholeWordLies()
{
    TestProcedure test;
    Output& out = test.out();
    LValue whole = test.argumentGPR(0);
    LValue other = test.argumentGPR(1);
    LValue lowHalf = out.castToInt32(whole);
    LValue mixed = out.bitXor(lowHalf, out.castToInt32(other));
    if (lowHalfIsReadTwice)
        mixed = out.mul(mixed, lowHalf);
    out.ret(out.add(out.zeroExtPtr(mixed), whole));
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, lowHalf->opcode() == Trunc && test.procedure().liveness().isFused(lowHalf) == !lowHalfIsReadTwice);
    AOT_ALLOCATION_TEST_CHECK(test, AllocationTestText(test).count("Trunc"_s) == lowHalfIsReadTwice);

    constexpr uint64_t first = 0xfffffff080000001;
    constexpr uint64_t second = 0x123456787fffffff;
    uint32_t expected = static_cast<uint32_t>(first ^ second);
    if (lowHalfIsReadTwice)
        expected *= static_cast<uint32_t>(first);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(first, second) == expected + first);
}

#undef AOT_ALLOCATION_TEST_CHECK

} // namespace Machine

std::span<const NativeTest> machineAllocationTests()
{
    static constexpr NativeTest tests[] = {
        { "machine-allocation/join-of-agreeing-predecessors-has-no-moves"_s, Machine::allocatorMakesNoMovesAtJoinOfAgreeingPredecessors },
        { "machine-allocation/second-edge-into-a-join-gets-parallel-moves"_s, Machine::allocatorGivesSecondEdgeIntoJoinParallelMoves },
        { "machine-allocation/edge-that-gets-moves-is-split"_s, Machine::allocatorSplitsEdgeThatGetsMoves },
        { "machine-allocation/operands-of-a-rare-patch-stay-where-they-lie"_s, Machine::allocatorLeavesOperandsOfRarePatchWhereTheyLie },
        { "machine-allocation/operands-beside-one-that-takes-some-register-stay-held"_s, Machine::allocatorHoldsOperandsToTheirRegistersBesideOneThatTakesSomeRegister },
        { "machine-allocation/work-grows-as-the-edges-with-moves-into-one-block-do"_s, Machine::allocatorWorkGrowsAsEdgesWithMovesIntoOneBlockDo },
        { "machine-allocation/work-grows-as-the-guards-ahead-of-the-framed-part-do"_s, Machine::allocatorWorkGrowsAsGuardsAheadOfFramedPartDo },
        { "machine-allocation/work-grows-as-the-edges-of-a-switch-that-get-moves-do"_s, Machine::allocatorWorkGrowsAsEdgesOfSwitchThatGetMovesDo },
        { "machine-allocation/operands-of-a-patch-that-ends-the-function-stay-where-they-lie"_s, Machine::allocatorLeavesOperandsOfPatchThatEndsFunctionWhereTheyLie },
        { "machine-allocation/nothing-is-kept-where-the-generator-makes-a-constant"_s, Machine::allocatorKeepsNothingWhereGeneratorMakesConstant },
        { "machine-allocation/result-of-a-patch-is-held-to-its-registers"_s, Machine::allocatorHoldsResultOfPatchToItsRegisters },
        { "machine-allocation/phi-stays-apart-from-a-value-that-lives-on"_s, Machine::allocatorKeepsPhiApartFromValueThatLivesOn },
        { "machine-allocation/two-integer-phis-swap"_s, Machine::allocatorRotatesPhisBySwapping<uint64_t, 2> },
        { "machine-allocation/two-number-phis-swap"_s, Machine::allocatorRotatesPhisBySwapping<double, 2> },
        { "machine-allocation/three-integer-phis-rotate"_s, Machine::allocatorRotatesPhisBySwapping<uint64_t, 3> },
        { "machine-allocation/three-number-phis-rotate"_s, Machine::allocatorRotatesPhisBySwapping<double, 3> },
        { "machine-allocation/spilled-integer-is-stored-once-behind-its-definition"_s, Machine::allocatorStoresSpilledValueOnceBehindItsDefinition<uint64_t> },
        { "machine-allocation/spilled-number-is-stored-once-behind-its-definition"_s, Machine::allocatorStoresSpilledValueOnceBehindItsDefinition<double> },
        { "machine-allocation/integers-across-a-call-take-callee-saved-registers"_s, Machine::allocatorKeepsValuesAcrossCallInCalleeSavedRegisters<uint64_t> },
        { "machine-allocation/numbers-across-a-call-take-callee-saved-registers"_s, Machine::allocatorKeepsValuesAcrossCallInCalleeSavedRegisters<double> },
        { "machine-allocation/integer-keeps-its-register-through-a-small-loop"_s, Machine::allocatorDecidesByPressureWhatKeepsItsRegisterThroughLoop<uint64_t, false> },
        { "machine-allocation/number-keeps-its-register-through-a-small-loop"_s, Machine::allocatorDecidesByPressureWhatKeepsItsRegisterThroughLoop<double, false> },
        { "machine-allocation/integer-loses-its-register-to-a-loop-under-pressure"_s, Machine::allocatorDecidesByPressureWhatKeepsItsRegisterThroughLoop<uint64_t, true> },
        { "machine-allocation/number-loses-its-register-to-a-loop-under-pressure"_s, Machine::allocatorDecidesByPressureWhatKeepsItsRegisterThroughLoop<double, true> },
        { "machine-allocation/integer-sum-read-behind-its-loop-comes-round-in-its-register"_s, Machine::allocatorBringsSumThatIsReadBehindLoopRoundInItsRegister<uint64_t, true> },
        { "machine-allocation/number-sum-read-behind-its-loop-comes-round-in-its-register"_s, Machine::allocatorBringsSumThatIsReadBehindLoopRoundInItsRegister<double, true> },
        { "machine-allocation/integer-sum-as-second-operand-comes-round-in-its-register"_s, Machine::allocatorBringsSumThatIsReadBehindLoopRoundInItsRegister<uint64_t, false> },
        { "machine-allocation/number-sum-as-second-operand-comes-round-in-its-register"_s, Machine::allocatorBringsSumThatIsReadBehindLoopRoundInItsRegister<double, false> },
        { "machine-allocation/nothing-is-spilled-ahead-of-a-join-that-a-rare-block-leads-back-to"_s, Machine::allocatorSpillsNothingAheadOfJoinThatRareBlockLeadsBackTo },
        { "machine-allocation/sum-comes-round-in-its-register-past-a-join-that-a-rare-block-leads-back-to"_s, Machine::allocatorBringsSumRoundInItsRegisterPastJoinThatRareBlockLeadsBackTo },
        { "machine-allocation/phi-that-a-later-way-out-of-the-loop-reads-outlives-the-addition"_s, Machine::allocatorKeepsPhiThatLaterWayOutOfLoopReads },
        { "machine-allocation/value-for-a-phi-is-made-in-the-register-of-the-phi"_s, Machine::allocatorDefinesValueInRegisterOfPhiWhoseHeadIsFixed },
        { "machine-allocation/count-that-one-arm-adds-to-comes-round-in-its-register"_s, Machine::allocatorBringsCountThatOneArmAddsToRoundInItsRegister },
        { "machine-allocation/what-a-loop-reads-stays-out-of-the-way-of-its-patches"_s, Machine::allocatorKeepsWhatLoopReadsOutOfWayOfItsPatches },
        { "machine-allocation/what-only-lives-through-a-loop-gives-way-for-the-registers-its-patches-leave-alone"_s, Machine::allocatorGivesRegistersThatPatchesLeaveAloneToWhatLoopUses },
        { "machine-allocation/nothing-is-loaded-on-the-way-round-a-loop-for-a-rare-arm"_s, Machine::allocatorLoadsNothingOnWayRoundLoopForRareArm },
        { "machine-allocation/nothing-is-loaded-at-a-join-for-a-rare-arm"_s, Machine::allocatorLoadsNothingAtJoinForRareArm },
        { "machine-allocation/nothing-is-loaded-on-the-ways-into-a-rare-join"_s, Machine::allocatorLoadsNothingOnWaysIntoRareJoin },
        { "machine-allocation/nothing-is-loaded-at-a-join-for-a-read-behind-a-call"_s, Machine::allocatorLoadsNothingAtJoinForReadBehindCall },
        { "machine-allocation/nothing-is-loaded-at-a-second-join-for-a-read-behind-a-call"_s, Machine::allocatorLoadsNothingAtSecondJoinForReadBehindCall },
        { "machine-allocation/join-holds-what-is-read-past-a-call-in-one-arm"_s, Machine::allocatorKeepsValuesAtJoinForReadPastCallInOneArm },
        { "machine-allocation/word-of-the-frame-is-addressed-from-the-nearer-end"_s, Machine::encoderAddressesWordOfFrameFromNearerEnd },
        { "machine-allocation/most-read-word-of-a-large-frame-takes-a-short-offset"_s, Machine::slotsGiveMostReadWordOfLargeFrameShortOffset },
        { "machine-allocation/word-that-a-register-serves-gives-way-to-words-that-are-loaded"_s, Machine::slotsCountInstructionsThatNameWordAndNotReaders },
        { "machine-allocation/values-that-take-turns-in-a-word-count-together"_s, Machine::slotsCountInstructionsOfAllThatTakeTurnsInWord },
        { "machine-allocation/stack-slot-lies-in-reach-of-the-frame-pointer-where-enough-instructions-name-it"_s, Machine::slotsKeepStackSlotInReachOfFramePointerWhereEnoughInstructionsNameIt },
        { "machine-allocation/shifted-index-that-dies-is-added-in-one-instruction"_s, Machine::encoderAddsShiftedIndexInOneInstruction<false> },
        { "machine-allocation/shifted-index-that-lives-on-is-added-in-one-instruction"_s, Machine::encoderAddsShiftedIndexInOneInstruction<true> },
        { "machine-allocation/decoded-reference-whose-index-lives-on-is-one-instruction"_s, Machine::encoderDecodesReferenceInOneInstruction<false> },
        { "machine-allocation/decoded-reference-with-offset-base-is-one-instruction"_s, Machine::encoderDecodesReferenceInOneInstruction<true> },
        { "machine-allocation/scaled-index-is-added-to-a-pinned-register-with-a-constant"_s, Machine::encoderAddsScaledIndexToPinnedRegisterWithConstant },
        { "machine-allocation/scaled-index-is-added-where-the-sum-sinks"_s, Machine::encoderAddsScaledIndexWhereSumSinks },
        { "machine-allocation/scaled-index-from-an-earlier-block-is-added-where-the-sum-sinks"_s, Machine::encoderAddsScaledIndexFromEarlierBlockWhereSumSinks },
        { "machine-allocation/shifted-index-that-is-the-first-child-is-added-in-one-instruction"_s, Machine::encoderAddsShiftedIndexThatIsFirstChild },
        { "machine-allocation/decoded-reference-or-marker-has-no-copy"_s, Machine::encoderDecodesReferenceOrMarkerWithoutCopy },
        { "machine-allocation/select-tests-its-word-in-the-arm-that-extends-it"_s, Machine::encoderTestsWordInArmThatExtendsIt },
        { "machine-allocation/shifted-index-that-one-instruction-may-not-add"_s, Machine::encoderAddsShiftedIndexThatOneInstructionMayNotAdd },
        { "machine-allocation/sum-and-constant-are-added-in-one-instruction"_s, Machine::encoderAddsSumAndConstantInOneInstruction<false> },
        { "machine-allocation/sum-and-constant-are-added-in-one-instruction-beside-an-operand-that-lives-on"_s, Machine::encoderAddsSumAndConstantInOneInstruction<true> },
#if CPU(X86_64)
        { "machine-allocation/frame-setup-pushes-the-callee-saves"_s, Machine::frameSetupPushesCalleeSaves },
        { "machine-allocation/far-jump-is-chained-through-a-near-jump-with-its-condition-and-target"_s, Machine::shortenerChainsFarJumpThroughNearJumpWithItsConditionAndTarget },
        { "machine-allocation/far-jump-is-chained-backward"_s, Machine::shortenerChainsFarJumpBackward },
        { "machine-allocation/no-jump-is-chained-through-two"_s, Machine::shortenerChainsNoJumpThroughTwo },
        { "machine-allocation/jump-that-reaches-its-target-once-others-are-short-is-not-chained"_s, Machine::shortenerChainsNoJumpThatReachesItsTargetOnceOthersAreShort },
        { "machine-allocation/tests-that-leave-for-one-rare-block-are-chained"_s, Machine::encoderChainsTestsThatLeaveForOneRareBlock },
#endif
        { "machine-allocation/integer-constant-is-made-once-ahead-of-a-loop"_s, Machine::allocatorMakesConstantOnceAheadOfLoop<uint64_t> },
        { "machine-allocation/number-constant-is-made-once-ahead-of-a-loop"_s, Machine::allocatorMakesConstantOnceAheadOfLoop<double> },
        { "machine-allocation/spilled-values-are-loaded-once-ahead-of-a-loop"_s, Machine::allocatorLoadsSpilledValuesOnceAheadOfLoop },
        { "machine-allocation/constant-that-a-full-loop-would-evict-is-not-kept"_s, Machine::allocatorKeepsNoConstantThatLoopWouldEvict<false> },
        { "machine-allocation/constant-is-not-kept-in-a-register-that-a-patch-takes"_s, Machine::allocatorKeepsNoConstantThatLoopWouldEvict<true> },
        { "machine-allocation/phi-read-behind-its-loop-counts-up-to-its-last-read-in-the-loop"_s, Machine::scanCountsPhiThatIsReadBehindLoopUpToItsLastReadInLoop<true> },
        { "machine-allocation/phi-read-in-its-loop-alone-counts-the-same"_s, Machine::scanCountsPhiThatIsReadBehindLoopUpToItsLastReadInLoop<false> },
        { "machine-allocation/join-that-a-rare-block-leads-back-to-holds-no-constant"_s, Machine::allocatorKeepsNoConstantAtJoinThatRareBlockLeadsBackTo },
        { "machine-allocation/constant-whose-reader-takes-its-register-is-not-kept"_s, Machine::allocatorKeepsNoConstantWhoseReaderTakesItsRegister },
        { "machine-allocation/zero-extension-of-a-number-is-no-instruction"_s, Machine::allocatorExtendsNumberWhereItIsMade },
        { "machine-allocation/constant-below-an-opaque-is-made-in-the-register-of-the-result"_s, Machine::allocatorMakesConstantBelowOpaqueInRegisterOfResult },
        { "machine-allocation/value-returned-behind-a-loop-is-made-in-the-return-register"_s, Machine::allocatorDefinesValueReturnedBehindLoopInReturnRegister },
        { "machine-allocation/count-that-an-inner-arm-adds-to-comes-round-in-its-register"_s, Machine::allocatorBringsCountThatInnerArmAddsToRoundInItsRegister },
        { "machine-allocation/count-that-a-rare-leave-reads-outlives-the-addition"_s, Machine::allocatorKeepsCountThatRareLeaveReadsUpToTest },
        { "machine-allocation/phis-that-a-rare-chain-behind-a-later-call-reads-stay-out-of-memory"_s, Machine::allocatorKeepsPhisThatRareChainBehindLaterCallReadsOutOfMemory },
        { "machine-allocation/what-a-rare-arm-alone-reads-is-counted-once"_s, Machine::allocatorCountsWhatRareArmAloneReadsOnce },
        { "machine-allocation/owner-that-the-other-arm-reads-outlives-its-storage"_s, Machine::allocatorKeepsOwnerThatOtherArmReadsWhileItsStorageIsRead },
        { "machine-allocation/loop-entered-twice"_s, Machine::allocatorDropsWhatSideEntranceOfLoopLacks },
        { "machine-allocation/shared-rare-block"_s, Machine::allocatorDropsWhatWayThroughSharedRareBlockLacks },
        { "machine-allocation/save-of-the-other-arm"_s, Machine::allocatorLoadsNothingFromSaveOfOtherRareArm },
        { "machine-allocation/save-of-another-way-in"_s, Machine::allocatorLoadsNothingFromSaveOfOtherWayIntoRareRegion },
        { "machine-allocation/rare-arm-costs-the-hot-path-nothing-for-integers"_s, Machine::allocatorKeepsRareArmOffHotPath<uint64_t> },
        { "machine-allocation/rare-arm-costs-the-hot-path-nothing-for-numbers"_s, Machine::allocatorKeepsRareArmOffHotPath<double> },
        { "machine-allocation/eviction-prefers-constants"_s, Machine::allocatorEvictsConstantsFirst },
        { "machine-allocation/pinned-constant-is-read-where-it-lies"_s, Machine::allocatorReadsPinnedConstantWhereItLies },
        { "machine-allocation/zero-extension-of-a-32-bit-result-is-no-instruction"_s, Machine::allocatorExtendsWordThatIsReadWhole<true> },
        { "machine-allocation/zero-extension-of-a-truncated-word-is-an-instruction"_s, Machine::allocatorExtendsWordThatIsReadWhole<false> },
        { "machine-allocation/zero-extension-of-the-count-of-a-loop-is-no-instruction"_s, Machine::allocatorExtendsCountOfLoop<true> },
        { "machine-allocation/zero-extension-of-a-count-that-starts-at-a-truncated-word-is-an-instruction"_s, Machine::allocatorExtendsCountOfLoop<false> },
        { "machine-allocation/upper-half-is-clear-behind-every-32-bit-result"_s, Machine::allocatorFindsUpperHalfClearBehindOperationsThatClearIt },
        { "machine-allocation/extension-into-a-phi-that-is-only-truncated-is-no-instruction"_s, Machine::allocatorExtendsWordOnItsWayRoundLoop<false> },
        { "machine-allocation/extension-into-a-phi-that-is-read-whole-is-an-instruction"_s, Machine::allocatorExtendsWordOnItsWayRoundLoop<true> },
        { "machine-allocation/low-half-is-read-where-the-whole-word-lies"_s, Machine::allocatorReadsLowHalfWhereWholeWordLies<false> },
        { "machine-allocation/low-half-with-two-readers-is-a-value"_s, Machine::allocatorReadsLowHalfWhereWholeWordLies<true> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
