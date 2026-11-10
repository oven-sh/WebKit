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
    exchange->setRunsWithoutFrame(true);
    exchange->setShowsRegistersToCollector(true);
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

static void allocatorLeavesOperandsOfRarePatchWhereTheyLie()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock rare = out.newBlock();
    LBasicBlock usual = out.newBlock();
    LBasicBlock behindLoop = out.newBlock();
    LValue buffer = test.argumentGPR(0);
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

static void encoderLeavesMoveOutOfResultRegisterToCalleeThatDoesIt()
{
    enum Case : unsigned { CalleeFillsOperand, CalleeFillsNothing, ResultRegisterIsFilledBehindMove, numberOfCases };
    TestProcedure test;
    Output& out = test.out();
    LValue buffer = test.argumentGPR(0);
    LValue other = test.argumentGPR(1);
    const Reg result = Reg(GPRInfo::returnValueGPR);
    const Reg filled = test.argumentGPR(2)->argumentReg();
    const Reg holdsBuffer = test.argumentGPR(3)->argumentReg();

    PatchOperation* takers[numberOfCases];
    for (unsigned index = 0; index < numberOfCases; ++index) {
        PatchOperation* maker = out.patchpoint(Int64);
        maker->resultConstraints = { ValueRep::reg(result) };
        maker->effects = Effects::none();
        maker->effects.writesLocalState = true;
        maker->setGenerator([index](CCallHelpers& jit, const PatchParameters&) {
            jit.move(CCallHelpers::TrustedImm64(40 + index), GPRInfo::returnValueGPR);
        });
        PatchOperation* taker = out.patchpoint(Void);
        takers[index] = taker;
        taker->append(maker, ValueRep::reg(filled));
        taker->append(buffer, ValueRep::reg(holdsBuffer));
        if (index == ResultRegisterIsFilledBehindMove)
            taker->append(other, ValueRep::reg(result));
        taker->setBringsOperandsToTheirRegisters(true);
        if (index != CalleeFillsNothing)
            taker->setRegisterThatCalleeFillsOutOfResult(filled);
        taker->effects = Effects::none();
        taker->effects.writesLocalState = true;
        taker->setGenerator([index](CCallHelpers& jit, const PatchParameters& parameters) {
            if (parameters.value()->leavesMoveOutOfResultToCallee())
                jit.move(GPRInfo::returnValueGPR, parameters[0].gpr());
            jit.store64(parameters[0].gpr(), CCallHelpers::Address(parameters[1].gpr(), index * sizeof(uint64_t)));
        });
    }
    out.ret(out.constInt64(0));
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, takers[CalleeFillsOperand]->leavesMoveOutOfResultToCallee());
    AOT_ALLOCATION_TEST_CHECK(test, !takers[CalleeFillsNothing]->leavesMoveOutOfResultToCallee());
    AOT_ALLOCATION_TEST_CHECK(test, !takers[ResultRegisterIsFilledBehindMove]->leavesMoveOutOfResultToCallee());
    std::array<uint64_t, numberOfCases> stored { };
    AOT_ALLOCATION_TEST_CHECK(test, !test.call<uint64_t>(stored.data(), 7, 0, 0));
    AOT_ALLOCATION_TEST_CHECK(test, stored[0] == 40 && stored[1] == 41 && stored[2] == 42);
}

static void encoderLeavesPairOfMovesToCalleeThatDoesBoth()
{
    enum Case : unsigned { CalleeDoesBoth, CalleeMovesAnotherRegisterIntoResult, CalleeDoesFirstAlone, SourceIsFilledBehindPair, numberOfCases };
    TestProcedure test;
    Output& out = test.out();
    LValue buffer = test.argumentGPR(0);
    LValue other = test.argumentGPR(1);
    LValue third = test.argumentGPR(4);
    const Reg result = Reg(GPRInfo::returnValueGPR);
    const Reg holdsOther = test.argumentGPR(1)->argumentReg();
    const Reg filled = test.argumentGPR(2)->argumentReg();
    const Reg holdsBuffer = test.argumentGPR(3)->argumentReg();

    PatchOperation* takers[numberOfCases];
    for (unsigned index = 0; index < numberOfCases; ++index) {
        PatchOperation* maker = out.patchpoint(Int64);
        maker->resultConstraints = { ValueRep::reg(result) };
        maker->effects = Effects::none();
        maker->effects.writesLocalState = true;
        maker->setGenerator([index](CCallHelpers& jit, const PatchParameters&) {
            jit.move(CCallHelpers::TrustedImm64(40 + index), GPRInfo::returnValueGPR);
        });
        PatchOperation* taker = out.patchpoint(Void);
        takers[index] = taker;
        taker->append(maker, ValueRep::reg(filled));
        taker->append(buffer, ValueRep::reg(holdsBuffer));
        taker->append(other, ValueRep::reg(result));
        if (index == SourceIsFilledBehindPair)
            taker->append(third, ValueRep::reg(holdsOther));
        taker->setRegisterThatCalleeFillsOutOfResult(filled);
        if (index != CalleeDoesFirstAlone)
            taker->setRegisterThatCalleeMovesIntoResult(index == CalleeMovesAnotherRegisterIntoResult ? holdsBuffer : holdsOther);
        taker->effects = Effects::none();
        taker->effects.writesLocalState = true;
        taker->setGenerator([index](CCallHelpers& jit, const PatchParameters& parameters) {
            if (parameters.value()->leavesMoveOutOfResultToCallee()) {
                jit.move(GPRInfo::returnValueGPR, parameters[0].gpr());
                jit.move(parameters.value()->registerThatCalleeMovesIntoResult().gpr(), GPRInfo::returnValueGPR);
            }
            jit.store64(parameters[0].gpr(), CCallHelpers::Address(parameters[1].gpr(), 2 * index * sizeof(uint64_t)));
            jit.store64(parameters[2].gpr(), CCallHelpers::Address(parameters[1].gpr(), (2 * index + 1) * sizeof(uint64_t)));
        });
    }
    out.ret(out.constInt64(0));
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, takers[CalleeDoesBoth]->leavesMoveOutOfResultToCallee());
    AOT_ALLOCATION_TEST_CHECK(test, !takers[CalleeMovesAnotherRegisterIntoResult]->leavesMoveOutOfResultToCallee());
    AOT_ALLOCATION_TEST_CHECK(test, !takers[CalleeDoesFirstAlone]->leavesMoveOutOfResultToCallee());
    AOT_ALLOCATION_TEST_CHECK(test, !takers[SourceIsFilledBehindPair]->leavesMoveOutOfResultToCallee());
    std::array<uint64_t, 2 * numberOfCases> stored { };
    AOT_ALLOCATION_TEST_CHECK(test, !test.call<uint64_t>(stored.data(), 7, 0, 0, 9));
    for (unsigned index = 0; index < numberOfCases; ++index)
        AOT_ALLOCATION_TEST_CHECK(test, stored[2 * index] == 40 + index && stored[2 * index + 1] == 7);
}

static void encoderLeavesMoveOutOfResultRegisterToCalleeThatEndsItsBlock()
{
    TestProcedure test;
    Output& out = test.out();
    LValue buffer = test.argumentGPR(0);
    const Reg result = Reg(GPRInfo::returnValueGPR);
    const Reg filled = test.argumentGPR(2)->argumentReg();
    const Reg holdsBuffer = test.argumentGPR(3)->argumentReg();
    LBasicBlock whenEven = out.newBlock();
    LBasicBlock whenOdd = out.newBlock();

    PatchOperation* maker = out.patchpoint(Int64);
    maker->resultConstraints = { ValueRep::reg(result) };
    maker->effects = Effects::none();
    maker->effects.writesLocalState = true;
    maker->setGenerator([](CCallHelpers& jit, const PatchParameters&) {
        jit.move(CCallHelpers::TrustedImm64(41), GPRInfo::returnValueGPR);
    });
    PatchOperation* taker = out.patchpoint(Void);
    taker->append(maker, ValueRep::reg(filled));
    taker->append(buffer, ValueRep::reg(holdsBuffer));
    taker->setRegisterThatCalleeFillsOutOfResult(filled);
    taker->effects = Effects::none();
    taker->effects.writesLocalState = true;
    taker->effects.terminal = true;
    taker->setGenerator([](CCallHelpers& jit, const PatchParameters& parameters) {
        if (parameters.value()->leavesMoveOutOfResultToCallee())
            jit.move(GPRInfo::returnValueGPR, parameters[0].gpr());
        jit.store64(parameters[0].gpr(), CCallHelpers::Address(parameters[1].gpr()));
        MacroAssembler::Label* first = parameters.successorLabels()[0];
        MacroAssembler::Label* second = parameters.successorLabels()[1];
        CCallHelpers::Jump toSecond = jit.branchTest64(CCallHelpers::NonZero, parameters[0].gpr(), CCallHelpers::TrustedImm32(1));
        CCallHelpers::Jump toFirst;
        if (!parameters.fallsThroughToSuccessor(0))
            toFirst = jit.jump();
        parameters.addLatePath([=](CCallHelpers& jit) {
            toSecond.linkTo(*second, &jit);
            if (toFirst.isSet())
                toFirst.linkTo(*first, &jit);
        });
    });
    out.appendSuccessor(normalEdge(whenEven));
    out.appendSuccessor(normalEdge(whenOdd));
    out.appendTo(whenEven);
    out.ret(out.constInt64(2));
    out.appendTo(whenOdd);
    out.ret(out.constInt64(1));
    compileOrFail(test);

    AOT_ALLOCATION_TEST_CHECK(test, taker->owner()->last() == taker && taker->leavesMoveOutOfResultToCallee());
    uint64_t stored = 0;
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(&stored, 7, 0, 0) == 1 && stored == 41);
}

static void encoderStepsOverStoreOfAnotherRegisterOnTheWayToCalleeThatDoesMove()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock rare = out.newBlock();
    LBasicBlock usual = out.newBlock();
    LValue buffer = test.argumentGPR(0);
    LValue kept = test.argumentGPR(1);
    const Reg result = Reg(GPRInfo::returnValueGPR);
    const Reg filled = test.argumentGPR(2)->argumentReg();
    const Reg holdsBuffer = test.argumentGPR(3)->argumentReg();
    out.branch(out.testNonZero64(kept, 1), rareEdge(rare), normalEdge(usual));

    out.appendTo(rare);
    PatchOperation* maker = out.patchpoint(Int64);
    maker->resultConstraints = { ValueRep::reg(result) };
    maker->effects = Effects::none();
    maker->effects.writesLocalState = true;
    maker->setGenerator([](CCallHelpers& jit, const PatchParameters&) {
        jit.move(CCallHelpers::TrustedImm64(43), GPRInfo::returnValueGPR);
    });
    PatchOperation* taker = out.patchpoint(Void);
    taker->append(maker, ValueRep::reg(filled));
    taker->append(buffer, ValueRep::reg(holdsBuffer));
    taker->setRegisterThatCalleeFillsOutOfResult(filled);
    taker->clobberLate(registerFileFor(test.procedure().traits()).allocatableIn(Bank::GP));
    taker->effects = Effects::none();
    taker->effects.writesLocalState = true;
    taker->setGenerator([](CCallHelpers& jit, const PatchParameters& parameters) {
        if (parameters.value()->leavesMoveOutOfResultToCallee())
            jit.move(GPRInfo::returnValueGPR, parameters[0].gpr());
        jit.store64(parameters[0].gpr(), CCallHelpers::Address(parameters[1].gpr()));
        forEachRegister(parameters.value()->lateClobberedMask(), [&](Reg reg) {
            if (reg.isGPR())
                jit.move(CCallHelpers::TrustedImm64(-1), reg.gpr());
        });
    });
    out.ret(kept);

    out.appendTo(usual);
    out.ret(kept);
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    const BlockSteps& steps = allocation.stepsOf(rare);
    uint32_t move = steps.begin;
    while (move < steps.end && !(allocation.step(move).kind == StepKind::Move && allocation.step(move).destination == Location::reg(filled) && allocation.step(move).sources[0] == Location::reg(result)))
        ++move;
    unsigned numberOfStoresBetween = 0;
    for (uint32_t index = move + 1; index < steps.end && allocation.step(index).kind != StepKind::Execute; ++index)
        numberOfStoresBetween += allocation.step(index).kind == StepKind::Save;
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().order().of(rare).isRare() && move < steps.end && numberOfStoresBetween);
    AOT_ALLOCATION_TEST_CHECK(test, taker->leavesMoveOutOfResultToCallee());
    uint64_t stored = 0;
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(&stored, 7, 0, 0) == 7 && stored == 43);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(&stored, 8, 0, 0) == 8);
}

static unsigned allocationTestNumberOfBlocksSplitLate(TestProcedure& test)
{
    const Order& order = test.procedure().order();
    return static_cast<unsigned>(std::ranges::count_if(order.blocksInOrder(), [&](const Block* block) {
        return order.of(block).wasSplitLate;
    }));
}

static void allocationTestAppendPatchThatBrings(TestProcedure& test, LValue value, Reg wanted, RegisterMask elsewhere)
{
    PatchOperation* patch = test.out().patchpoint(Void);
    patch->append(value, ValueRep::reg(wanted));
    patch->clobberLate(elsewhere & ~maskOf(wanted));
    patch->setGenerator([](CCallHelpers&, const PatchParameters&) { });
}

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

static std::optional<uint64_t> allocationTestInstructionsForValuesThatOutliveTheirBlockAndBlocksThatAskWhatLivesOnTheirWays(unsigned number)
{
    TestProcedure test;
    Output& out = test.out();
    LValue selector = test.argumentGPR(0);
    LValue base = test.argumentGPR(1);
    Vector<LValue> readBehindEveryOtherBlock;
    for (unsigned index = 0; index < 12; ++index)
        readBehindEveryOtherBlock.append(out.opaque(out.add(base, out.constInt64(index))));
    Vector<LBasicBlock> blocks = allocationTestAppendSwitch(test, selector, number + 1);
    for (unsigned index = 0; index < number; ++index) {
        out.appendTo(blocks[index]);
        LValue value = out.opaque(out.add(selector, out.constInt64(index)));
        LBasicBlock returns = out.newBlock();
        out.jump(returns);
        out.appendTo(returns);
        out.ret(value);
    }
    out.appendTo(blocks[number]);
    LValue sum = out.int64Zero;
    for (LValue value : readBehindEveryOtherBlock)
        sum = out.add(sum, value);
    out.ret(sum);
    return instructionsOfCurrentThreadIn([&] {
        prepareOrFail(test);
    });
}

static void allocatorWorkGrowsAsValuesThatOutliveTheirBlockAndBlocksThatAskWhatLivesOnTheirWaysDo()
{
    constexpr unsigned number = 2000;
    std::optional<uint64_t> smallest = allocationTestInstructionsForValuesThatOutliveTheirBlockAndBlocksThatAskWhatLivesOnTheirWays(number);
    std::optional<uint64_t> middle = allocationTestInstructionsForValuesThatOutliveTheirBlockAndBlocksThatAskWhatLivesOnTheirWays(4 * number);
    std::optional<uint64_t> large = allocationTestInstructionsForValuesThatOutliveTheirBlockAndBlocksThatAskWhatLivesOnTheirWays(16 * number);
    AOT_TEST_CHECK(growsAsSizeDoes(smallest, middle));
    AOT_TEST_CHECK(growsAsSizeDoes(middle, large));
}

static void allocatorWorkGrowsAsEdgesWithMovesIntoOneBlockDo()
{
    constexpr unsigned numberOfEdges = 10000;
    std::optional<uint64_t> smaller = allocationTestInstructionsForEdgesWithMovesIntoOneBlock(numberOfEdges);
    std::optional<uint64_t> large = allocationTestInstructionsForEdgesWithMovesIntoOneBlock(4 * numberOfEdges);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

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

static std::optional<uint64_t> allocationTestInstructionsForRareWaysBackInBehindHeadOfFramedPart(unsigned numberOfGuards)
{
    TestProcedure test;
    Output& out = test.out();
    LValue selector = test.argumentGPR(0);
    LValue value = test.argumentGPR(1);
    LBasicBlock head = out.newBlock();
    Vector<LBasicBlock> rareWays;
    Vector<LBasicBlock> meetings;
    for (unsigned number = 0; number < numberOfGuards; ++number) {
        rareWays.append(out.newBlock());
        meetings.append(out.newBlock());
    }
    Vector<LBasicBlock> guards = allocationTestAppendSwitch(test, selector, numberOfGuards);
    for (unsigned number = 0; number < numberOfGuards; ++number) {
        out.appendTo(guards[number]);
        out.branch(out.testNonZero64(value, number + 1), rareEdge(rareWays[number]), normalEdge(head));
    }
    out.appendTo(head);
    allocationTestAppendCall(test, nullptr);
    out.jump(normalEdge(meetings[0]));
    for (unsigned number = 0; number < numberOfGuards; ++number) {
        out.appendTo(meetings[number]);
        allocationTestAppendCall(test, nullptr);
        if (number + 1 < numberOfGuards)
            out.jump(normalEdge(meetings[number + 1]));
        else
            out.ret(selector);
        out.appendTo(rareWays[number]);
        allocationTestAppendCall(test, nullptr);
        out.jump(normalEdge(meetings[number]));
    }
    std::optional<uint64_t> instructions = instructionsOfCurrentThreadIn([&] {
        prepareOrFail(test);
    });
    AOT_TEST_CHECK(test.procedure().order().of(meetings.last()).predecessors.size() == 2);
    return instructions;
}

static void allocatorWorkGrowsAsRareWaysBackInBehindHeadOfFramedPartDo()
{
    constexpr unsigned numberOfGuards = 2000;
    std::optional<uint64_t> smaller = allocationTestInstructionsForRareWaysBackInBehindHeadOfFramedPart(numberOfGuards);
    std::optional<uint64_t> large = allocationTestInstructionsForRareWaysBackInBehindHeadOfFramedPart(4 * numberOfGuards);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

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

static void allocatorDefinesArmThatDiesInSelectWhereResultIsWanted()
{
    uint64_t noted = 0;
    TestProcedure test;
    Output& out = test.out();
    LValue selector = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LValue whenOdd = out.load64(allocationTestWordAt(test, buffer, 0));
    LValue otherwise = out.load64(allocationTestWordAt(test, buffer, 1));
    LValue chosen = out.select(out.testNonZero64(selector, 1), whenOdd, otherwise);
    PatchOperation* reader = out.patch(Void);
    reader->append(chosen, ValueRep::reg(GPRInfo::argumentGPR5));
    reader->setGenerator([place = &noted](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        AOT_TEST_CHECK(params.size() == 1 && params[0] == ValueRep::reg(GPRInfo::argumentGPR5));
        GPRReg address = jit.scratchRegister();
        jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
        jit.store64(GPRInfo::argumentGPR5, CCallHelpers::Address(address));
    });
    out.ret(selector);
    compileOrFail(test);

    AllocationTestText text(test);
    const Allocation& allocation = test.procedure().allocation();
    const Reg wanted = Reg(GPRInfo::argumentGPR5);
    AOT_ALLOCATION_TEST_CHECK(test, chosen->opcode() == Select && allocation.registerAtDefinitionOf(chosen) == wanted && !text.count("move"_s, chosen));
    if (isX86_64())
        AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(whenOdd) == wanted || allocation.registerAtDefinitionOf(otherwise) == wanted);

    for (uint64_t isOdd : { 0u, 1u }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(isOdd, words.data()) == isOdd);
        AOT_ALLOCATION_TEST_CHECK(test, noted == words[isOdd ? 0 : 1]);
    }
}

static void allocatorLeavesRegisterThatSelectInLoopWantsToArmMadeInLoop()
{
    uint64_t noted = 0;
    TestProcedure test;
    Output& out = test.out();
    LValue turns = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LValue madeAhead = out.load64(allocationTestWordAt(test, buffer, 0));
    LBasicBlock head = out.newBlock();
    LBasicBlock end = out.newBlock();
    ValueFromBlock firstTurns = out.anchor(turns);
    out.jump(head);

    out.appendTo(head);
    LValue turnsLeft = out.phi(Int64, firstTurns);
    LValue madeInLoop = out.load64(allocationTestWordAt(test, buffer, 1));
    LValue chosen = out.select(out.testNonZero64(turnsLeft, 1), madeAhead, madeInLoop);
    PatchOperation* reader = out.patch(Void);
    reader->append(chosen, ValueRep::reg(GPRInfo::argumentGPR5));
    reader->setGenerator([place = &noted](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        AOT_TEST_CHECK(params.size() == 1 && params[0] == ValueRep::reg(GPRInfo::argumentGPR5));
        GPRReg address = jit.scratchRegister();
        jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
        jit.store64(GPRInfo::argumentGPR5, CCallHelpers::Address(address));
    });
    LValue nextTurnsLeft = out.sub(turnsLeft, out.constInt64(1));
    out.addIncomingToPhi(turnsLeft, out.anchor(nextTurnsLeft));
    out.branch(out.notZero64(nextTurnsLeft), normalEdge(head), normalEdge(end));

    out.appendTo(end);
    out.ret(nextTurnsLeft);
    compileOrFail(test);

    AllocationTestText text(test);
    const Allocation& allocation = test.procedure().allocation();
    const Reg wanted = Reg(GPRInfo::argumentGPR5);
    AOT_ALLOCATION_TEST_CHECK(test, test.procedure().order().of(head).loop != noLoop && test.procedure().order().of(madeAhead->owner()).loop == noLoop);
    AOT_ALLOCATION_TEST_CHECK(test, chosen->opcode() == Select && allocation.registerAtDefinitionOf(chosen) == wanted && !text.count("move"_s, chosen));
    if (isX86_64())
        AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(madeAhead) != wanted && allocation.registerAtDefinitionOf(madeInLoop) == wanted);

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    AOT_ALLOCATION_TEST_CHECK(test, !test.call<uint64_t>(uint64_t { 4 }, words.data()) && noted == words[0]);
    words[0] = ~words[0];
    AOT_ALLOCATION_TEST_CHECK(test, !test.call<uint64_t>(uint64_t { 3 }, words.data()) && noted == words[0]);
}

static void allocatorLeavesPhiWhereCallInItsBlockTakesIt()
{
    uint64_t noted = 0;
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    LValue argument = test.argumentGPR(0);
    LBasicBlock body = out.newBlock();
    ValueFromBlock handedOn = out.anchor(argument);
    out.jump(body);

    out.appendTo(body);
    LValue parameter = out.phi(Int64, handedOn);
    PatchOperation* call = out.patch(Void);
    call->append(parameter, ValueRep::reg(GPRInfo::argumentGPR0));
    call->clobberLate((file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP)) & ~file.calleeSaved);
    call->setGenerator([place = &noted](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        AOT_TEST_CHECK(params.size() == 1 && params[0] == ValueRep::reg(GPRInfo::argumentGPR0));
        GPRReg address = jit.scratchRegister();
        jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
        jit.store64(GPRInfo::argumentGPR0, CCallHelpers::Address(address));
        jit.move(CCallHelpers::TrustedImm64(-1), GPRInfo::argumentGPR0);
    });
    out.ret(out.add(parameter, out.constInt64(1)));
    compileOrFail(test);

    AllocationTestText text(test);
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, parameter->opcode() == Phi && test.procedure().liveness().of(parameter).has(ValueFlag::LivesAcrossCall));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(parameter) == Reg(GPRInfo::argumentGPR0));
    AOT_ALLOCATION_TEST_CHECK(test, !text.count("move"_s, argument) && text.count("move"_s, parameter) == 1);
    AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(parameter) && allocationTestCountOf(allocation.usedCalleeSaves()) == 1);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 41 }) == 42 && noted == 41);
}

static void allocatorGivesArgumentThatLivesAcrossCallItsHomeWhereItIsDefined()
{
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    LValue tested = test.argumentGPR(0);
    LValue kept = test.argumentGPR(1);
    LValue buffer = test.argumentGPR(2);
    LBasicBlock slow = out.newBlock();
    LBasicBlock behind = out.newBlock();
    out.branch(out.testNonZero64(tested, 1), rareEdge(slow), normalEdge(behind));

    out.appendTo(slow);
    allocationTestAppendCall(test, nullptr);
    out.jump(normalEdge(behind));

    out.appendTo(behind);
    PatchOperation* call = allocationTestAppendCall(test, nullptr);
    out.store64(kept, allocationTestWordAt(test, buffer, 0));
    out.store64(kept, allocationTestWordAt(test, buffer, 1));
    out.ret(out.add(kept, 1));
    compileOrFail(test);

    AllocationTestText text(test);
    const Order& order = test.procedure().order();
    const Liveness& liveness = test.procedure().liveness();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, order.of(slow).isRare() && !allocation.hasFramedPart());
    for (LValue value : { kept, buffer }) {
        AOT_ALLOCATION_TEST_CHECK(test, value->opcode() == ArgumentReg && liveness.of(value).has(ValueFlag::LivesAcrossCall) && liveness.of(value).lastUse > order.earlyPositionOf(call));
        AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(value) && (maskOf(allocation.registerAtDefinitionOf(value)) & file.calleeSaved));
        AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(value) && !text.count("move"_s, value) && !text.count("save"_s, value) && !text.count("reload"_s, value));
    }
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(tested) == Reg(GPRInfo::argumentGPR0));

    for (uint64_t first : { 0u, 1u }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        std::array<uint64_t, 64> expected = words;
        expected[0] = 41;
        expected[1] = 41;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(first, uint64_t { 41 }, words.data()) == 42);
        AOT_ALLOCATION_TEST_CHECK(test, words == expected);
    }
}

static void allocatorGivesCalleeSavesAtEntryToArgumentsThatAreReadMost()
{
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    const unsigned numberOfArguments = allocationTestCountOf(file.calleeSavedIn(Bank::GP)) + 1;
    if (numberOfArguments > GPRInfo::numberOfArgumentRegisters)
        return;
    Vector<LValue> arguments;
    for (unsigned index = 0; index < numberOfArguments; ++index)
        arguments.append(test.argumentGPR(index));
    allocationTestAppendCall(test, nullptr);
    LValue mixed = arguments[0];
    for (unsigned index = 1; index < numberOfArguments; ++index)
        mixed = out.add(out.bitXor(out.add(mixed, arguments[index]), arguments[index]), arguments[index]);
    out.ret(mixed);
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, !allocation.hasFramedPart());
    AOT_ALLOCATION_TEST_CHECK(test, allocation.isSpilled(arguments[0]) && allocation.isStoredBehindDefinition(arguments[0]) && allocation.registerAtDefinitionOf(arguments[0]) == arguments[0]->argumentReg());
    for (unsigned index = 1; index < numberOfArguments; ++index)
        AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(arguments[index]) && (maskOf(allocation.registerAtDefinitionOf(arguments[index])) & file.calleeSaved));

    std::array<uint64_t, 8> passed { 3, 50, 700, 9000, 11000, 130000, 1700000, 19000000 };
    uint64_t expected = passed[0];
    for (unsigned index = 1; index < numberOfArguments; ++index)
        expected = ((expected + passed[index]) ^ passed[index]) + passed[index];
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(passed[0], passed[1], passed[2], passed[3], passed[4], passed[5], passed[6], passed[7]) == expected);
}

static void allocatorLeavesArgumentWhereCallInItsBlockTakesIt()
{
    uint64_t noted = 0;
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileFor(test.procedure().traits());
    LValue argument = test.argumentGPR(0);
    PatchOperation* call = out.patch(Void);
    call->append(argument, ValueRep::reg(GPRInfo::argumentGPR0));
    call->clobberLate((file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP)) & ~file.calleeSaved);
    call->setGenerator([place = &noted](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        AOT_TEST_CHECK(params.size() == 1 && params[0] == ValueRep::reg(GPRInfo::argumentGPR0));
        GPRReg address = jit.scratchRegister();
        jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
        jit.store64(GPRInfo::argumentGPR0, CCallHelpers::Address(address));
        jit.move(CCallHelpers::TrustedImm64(-1), GPRInfo::argumentGPR0);
    });
    out.ret(out.add(argument, out.constInt64(1)));
    compileOrFail(test);

    AllocationTestText text(test);
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, argument->opcode() == ArgumentReg && test.procedure().liveness().of(argument).has(ValueFlag::LivesAcrossCall));
    AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(argument) == Reg(GPRInfo::argumentGPR0) && text.count("move"_s, argument) == 1);
    AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(argument) && allocationTestCountOf(allocation.usedCalleeSaves()) == 1);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 41 }) == 42 && noted == 41);
}

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
        AOT_ALLOCATION_TEST_CHECK(test, allocation.registerAtDefinitionOf(value) && (maskOf(allocation.registerAtDefinitionOf(value)) & file.calleeSaved));
        AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(value) && allocationTestRegisterAtHead(test, join, value) == allocation.registerAtDefinitionOf(value));
        AOT_ALLOCATION_TEST_CHECK(test, !text.count("move"_s, value) && !text.count("save"_s, value) && !text.count("reload"_s, value));
    }

    for (uint64_t first : { 0u, 1u, 2u, 7u }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        std::array<uint64_t, 64> expected = words;
        expected[0] = 41;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(first, uint64_t { 41 }, words.data()) == 42);
        AOT_ALLOCATION_TEST_CHECK(test, words == expected);
    }
}

static void allocatorLeavesOutOfHeadEnteredFromSideWhatNoWayOnReads()
{
    TestProcedure test;
    Output& out = test.out();
    LValue ways = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock behindFirstTest = out.newBlock();
    LBasicBlock outerHead = out.newBlock();
    LBasicBlock returns = out.newBlock();
    LBasicBlock innerHead = out.newBlock();
    LBasicBlock reads = out.newBlock();
    LBasicBlock leaves = out.newBlock();
    LBasicBlock leavesForOuterHead = out.newBlock();
    LBasicBlock leavesForInnerHead = out.newBlock();
    LBasicBlock staysBehindRead = out.newBlock();
    LBasicBlock entersAgain = out.newBlock();
    LBasicBlock leavesWithoutValue = out.newBlock();
    LBasicBlock staysBehindSecondEntry = out.newBlock();
    LBasicBlock entersLast = out.newBlock();
    LBasicBlock rareJoin = out.newBlock();
    auto note = [&](unsigned index) {
        out.store64(out.constInt64(index), allocationTestWordAt(test, buffer, index));
    };

    LValue value = out.load64(allocationTestWordAt(test, buffer, 0));
    out.branch(out.testNonZero64(ways, 1), rareEdge(reads), normalEdge(behindFirstTest));

    out.appendTo(behindFirstTest);
    out.branch(out.testNonZero64(value, 2), unlikelyEdge(innerHead), normalEdge(outerHead));

    out.appendTo(outerHead);
    note(1);
    out.branch(out.testNonZero64(ways, 4), rareEdge(entersAgain), normalEdge(returns));

    out.appendTo(returns);
    out.ret(out.constInt64(100));

    out.appendTo(innerHead);
    note(2);
    out.jump(rareEdge(entersLast));

    out.appendTo(reads);
    out.store64(value, allocationTestWordAt(test, buffer, 3));
    out.branch(out.testNonZero64(ways, 8), normalEdge(leaves), normalEdge(staysBehindRead));

    out.appendTo(leaves);
    out.branch(out.testNonZero64(ways, 16), normalEdge(leavesForOuterHead), normalEdge(leavesForInnerHead));

    out.appendTo(leavesForOuterHead);
    note(4);
    out.jump(normalEdge(outerHead));

    out.appendTo(leavesForInnerHead);
    note(5);
    out.jump(normalEdge(innerHead));

    out.appendTo(staysBehindRead);
    note(6);
    out.jump(normalEdge(rareJoin));

    out.appendTo(entersAgain);
    out.branch(out.testNonZero64(ways, 32), normalEdge(leavesWithoutValue), normalEdge(staysBehindSecondEntry));

    out.appendTo(leavesWithoutValue);
    note(7);
    out.jump(normalEdge(innerHead));

    out.appendTo(staysBehindSecondEntry);
    note(8);
    out.jump(normalEdge(rareJoin));

    out.appendTo(entersLast);
    note(9);
    out.jump(normalEdge(rareJoin));

    out.appendTo(rareJoin);
    out.ret(out.constInt64(200));
    compileOrFail(test);

    const Order& order = test.procedure().order();
    for (const Block* rare : { reads, leavesForOuterHead, leavesForInnerHead, entersAgain, leavesWithoutValue, entersLast, rareJoin })
        AOT_ALLOCATION_TEST_CHECK(test, order.of(rare).isRare() && order.of(rare).rareRegion == order.of(reads).rareRegion && order.headOf(innerHead) < order.headOf(rare));
    AOT_ALLOCATION_TEST_CHECK(test, order.headOf(outerHead) < order.headOf(innerHead) && order.of(outerHead).isLoopHead && order.of(innerHead).isLoopHead);
    AOT_ALLOCATION_TEST_CHECK(test, order.loops()[order.of(innerHead).loop].parent == order.of(outerHead).loop);
    AOT_ALLOCATION_TEST_CHECK(test, !allocationTestRegisterAtHead(test, outerHead, value) && !allocationTestRegisterAtHead(test, innerHead, value));

    struct Way {
        uint64_t ways;
        uint64_t value;
        uint64_t result;
        std::initializer_list<unsigned> noted;
    };
    for (const Way& way : std::initializer_list<Way> {
        { 0, 0, 100, { 1 } },
        { 4, 0, 200, { 1, 8 } },
        { 4 | 32, 0, 200, { 1, 7, 2, 9 } },
        { 0, 2, 200, { 2, 9 } },
        { 1, 40, 200, { 3, 6 } },
        { 1 | 8, 40, 200, { 3, 5, 2, 9 } },
        { 1 | 8 | 16, 40, 100, { 3, 4, 1 } },
        { 1 | 8 | 16 | 4, 40, 200, { 3, 4, 1, 8 } },
        { 1 | 8 | 16 | 4 | 32, 40, 200, { 3, 4, 1, 7, 2, 9 } },
    }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        words[0] = way.value;
        std::array<uint64_t, 64> expected = words;
        for (unsigned index : way.noted)
            expected[index] = index == 3 ? way.value : index;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way.ways, words.data()) == way.result);
        AOT_ALLOCATION_TEST_CHECK(test, words == expected);
    }
}

static void orderLaysNoBlockAheadOfAllItsPredecessors()
{
    TestProcedure test;
    Output& out = test.out();
    LValue ways = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock asksForRareWay = out.newBlock();
    LBasicBlock returnsEarly = out.newBlock();
    LBasicBlock entersLoopInItsMiddle = out.newBlock();
    LBasicBlock tail = out.newBlock();
    LBasicBlock middle = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock returns = out.newBlock();
    LBasicBlock entersFirst = out.newBlock();
    LBasicBlock entersFromLoop = out.newBlock();
    LBasicBlock rareJoin = out.newBlock();
    LBasicBlock leavesForTail = out.newBlock();
    LBasicBlock returnsRarely = out.newBlock();
    auto note = [&](unsigned index) {
        out.store64(out.constInt64(index), allocationTestWordAt(test, buffer, index));
    };

    out.branch(out.testNonZero64(ways, 1), normalEdge(entersLoopInItsMiddle), normalEdge(asksForRareWay));

    out.appendTo(asksForRareWay);
    note(1);
    out.branch(out.testNonZero64(ways, 2), rareEdge(entersFirst), normalEdge(returnsEarly));

    out.appendTo(returnsEarly);
    out.ret(out.constInt64(100));

    out.appendTo(entersLoopInItsMiddle);
    note(2);
    out.jump(normalEdge(middle));

    out.appendTo(tail);
    note(3);
    out.jump(normalEdge(middle));

    out.appendTo(middle);
    note(4);
    out.branch(out.testNonZero64(ways, 4), rareEdge(entersFromLoop), normalEdge(latch));

    out.appendTo(latch);
    LValue left = out.sub(out.load64(allocationTestWordAt(test, buffer, 0)), 1);
    out.store64(left, allocationTestWordAt(test, buffer, 0));
    out.branch(left, normalEdge(tail), normalEdge(returns));

    out.appendTo(returns);
    out.ret(out.constInt64(200));

    out.appendTo(entersFirst);
    note(5);
    out.jump(normalEdge(rareJoin));

    out.appendTo(entersFromLoop);
    note(6);
    out.jump(normalEdge(rareJoin));

    out.appendTo(rareJoin);
    note(7);
    out.branch(out.testNonZero64(ways, 8), normalEdge(leavesForTail), normalEdge(returnsRarely));

    out.appendTo(leavesForTail);
    note(8);
    out.jump(normalEdge(tail));

    out.appendTo(returnsRarely);
    out.ret(out.constInt64(300));
    compileOrFail(test);

    const Order& order = test.procedure().order();
    for (const Block* rare : { entersFirst, entersFromLoop, rareJoin, leavesForTail, returnsRarely })
        AOT_ALLOCATION_TEST_CHECK(test, order.of(rare).isRare() && order.of(rare).rareRegion == order.of(rareJoin).rareRegion && order.headOf(middle) < order.headOf(rare));
    for (const Block* block : order.blocksInOrder())
        AOT_ALLOCATION_TEST_CHECK(test, block->isEntry() || order.headOf(order.of(block).predecessors.front()) < order.headOf(block));
    AOT_ALLOCATION_TEST_CHECK(test, !order.of(tail).isRare() && order.headOf(latch) < order.headOf(tail));

    struct Way {
        uint64_t ways;
        uint64_t rounds;
        uint64_t result;
        std::initializer_list<unsigned> noted;
    };
    for (const Way& way : std::initializer_list<Way> {
        { 1, 1, 200, { 2, 4 } },
        { 1, 3, 200, { 2, 4, 3 } },
        { 0, 1, 100, { 1 } },
        { 2, 1, 300, { 1, 5, 7 } },
        { 2 | 8, 1, 200, { 1, 5, 7, 8, 3, 4 } },
        { 2 | 8, 4, 200, { 1, 5, 7, 8, 3, 4 } },
        { 1 | 4, 1, 300, { 2, 4, 6, 7 } },
    }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        words[0] = way.rounds;
        std::array<uint64_t, 64> expected = words;
        expected[0] = way.result == 200 ? 0 : way.rounds;
        for (unsigned index : way.noted)
            expected[index] = index;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way.ways, words.data()) == way.result);
        AOT_ALLOCATION_TEST_CHECK(test, words == expected);
    }
}

static void allocatorJudgesSecondEntranceOfCycleAsItsOuterHead()
{
    TestProcedure test;
    Output& out = test.out();
    LValue ways = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock ahead = out.newBlock();
    LBasicBlock outerHead = out.newBlock();
    LBasicBlock middleHead = out.newBlock();
    LBasicBlock innerHead = out.newBlock();
    LBasicBlock behindInner = out.newBlock();
    LBasicBlock behindMiddle = out.newBlock();
    LBasicBlock behindOuter = out.newBlock();
    LBasicBlock returns = out.newBlock();
    LBasicBlock reads = out.newBlock();
    LBasicBlock entersLast = out.newBlock();
    LBasicBlock rareJoin = out.newBlock();
    auto set = [&](unsigned index, uint64_t word) {
        out.store64(out.constInt64(word), allocationTestWordAt(test, buffer, index));
    };
    auto count = [&](unsigned index) {
        LValue left = out.sub(out.load64(allocationTestWordAt(test, buffer, index)), 1);
        out.store64(left, allocationTestWordAt(test, buffer, index));
        return left;
    };

    LValue value = out.load64(allocationTestWordAt(test, buffer, 0));
    out.branch(out.testNonZero64(ways, 1), rareEdge(reads), normalEdge(ahead));

    out.appendTo(ahead);
    out.branch(out.testNonZero64(value, 2), normalEdge(middleHead), normalEdge(outerHead));

    out.appendTo(outerHead);
    set(1, 1);
    set(5, 2);
    out.jump(normalEdge(middleHead));

    out.appendTo(middleHead);
    set(2, 2);
    set(4, 2);
    out.jump(normalEdge(innerHead));

    out.appendTo(innerHead);
    out.branch(count(4), normalEdge(innerHead), normalEdge(behindInner));

    out.appendTo(behindInner);
    out.branch(count(5), normalEdge(middleHead), normalEdge(behindMiddle));

    out.appendTo(behindMiddle);
    out.branch(count(6), normalEdge(outerHead), normalEdge(behindOuter));

    out.appendTo(behindOuter);
    out.branch(out.testNonZero64(ways, 4), rareEdge(entersLast), normalEdge(returns));

    out.appendTo(returns);
    out.ret(out.constInt64(100));

    out.appendTo(reads);
    out.store64(value, allocationTestWordAt(test, buffer, 3));
    out.jump(normalEdge(rareJoin));

    out.appendTo(entersLast);
    set(9, 9);
    out.jump(normalEdge(rareJoin));

    out.appendTo(rareJoin);
    out.ret(out.constInt64(200));
    compileOrFail(test);

    const Order& order = test.procedure().order();
    AOT_ALLOCATION_TEST_CHECK(test, order.headOf(outerHead) < order.headOf(middleHead) && order.headOf(middleHead) < order.headOf(innerHead) && order.headOf(innerHead) < order.headOf(behindOuter) && order.headOf(behindOuter) < order.headOf(reads));
    AOT_ALLOCATION_TEST_CHECK(test, order.of(outerHead).isLoopHead && order.of(middleHead).isLoopHead && order.of(innerHead).isLoopHead);
    AOT_ALLOCATION_TEST_CHECK(test, order.loops()[order.of(innerHead).loop].parent == order.of(middleHead).loop && order.loops()[order.of(middleHead).loop].parent == order.of(outerHead).loop);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(reads).isRare() && order.of(reads).rareRegion == order.of(entersLast).rareRegion);
    AOT_ALLOCATION_TEST_CHECK(test, !!allocationTestRegisterAtHead(test, outerHead, value) == !!allocationTestRegisterAtHead(test, middleHead, value));

    struct Way {
        uint64_t ways;
        uint64_t value;
        uint64_t roundsOfOuterLoop;
        uint64_t result;
        std::initializer_list<unsigned> noted;
    };
    for (const Way& way : std::initializer_list<Way> {
        { 0, 0, 1, 100, { 1, 2 } },
        { 0, 0, 3, 100, { 1, 2 } },
        { 0, 2, 1, 100, { 2 } },
        { 0, 2, 2, 100, { 1, 2 } },
        { 4, 0, 2, 200, { 1, 2, 9 } },
        { 1, 40, 1, 200, { 3 } },
    }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        words[0] = way.value;
        words[5] = 1;
        words[6] = way.roundsOfOuterLoop;
        std::array<uint64_t, 64> expected = words;
        if (!(way.ways & 1))
            expected[4] = expected[5] = expected[6] = 0;
        for (unsigned index : way.noted)
            expected[index] = index == 3 ? way.value : index;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way.ways, words.data()) == way.result);
        AOT_ALLOCATION_TEST_CHECK(test, words == expected);
    }
}

static void allocatorPassesOverReadOffAnotherSideEntranceAsOwnHeadDoes()
{
    TestProcedure test;
    Output& out = test.out();
    LValue ways = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock ahead = out.newBlock();
    LBasicBlock parts = out.newBlock();
    LBasicBlock thenHead = out.newBlock();
    LBasicBlock otherwise = out.newBlock();
    LBasicBlock readsIt = out.newBlock();
    LBasicBlock joins = out.newBlock();
    LBasicBlock returns = out.newBlock();
    LBasicBlock lateHead = out.newBlock();
    LBasicBlock entersFirst = out.newBlock();
    LBasicBlock leavesForThenHead = out.newBlock();
    LBasicBlock entersSecond = out.newBlock();
    LBasicBlock entersThird = out.newBlock();
    LBasicBlock leavesForLateHead = out.newBlock();
    LBasicBlock entersLast = out.newBlock();
    LBasicBlock stays = out.newBlock();
    LBasicBlock rareJoin = out.newBlock();
    auto note = [&](unsigned index) {
        out.store64(out.constInt64(index), allocationTestWordAt(test, buffer, index));
    };

    LValue value = out.load64(allocationTestWordAt(test, buffer, 0));
    out.branch(out.testNonZero64(ways, 1), rareEdge(entersFirst), normalEdge(ahead));

    out.appendTo(ahead);
    out.branch(out.testNonZero64(value, 2), unlikelyEdge(lateHead), normalEdge(parts));

    out.appendTo(parts);
    out.branch(out.testNonZero64(ways, 2), normalEdge(thenHead), normalEdge(otherwise));

    out.appendTo(thenHead);
    note(1);
    out.jump(normalEdge(joins));

    out.appendTo(otherwise);
    out.branch(out.testNonZero64(ways, 4), rareEdge(entersSecond), normalEdge(readsIt));

    out.appendTo(readsIt);
    out.store64(value, allocationTestWordAt(test, buffer, 3));
    out.jump(normalEdge(joins));

    out.appendTo(joins);
    out.branch(out.testNonZero64(ways, 8), rareEdge(entersThird), normalEdge(returns));

    out.appendTo(returns);
    out.ret(out.constInt64(100));

    out.appendTo(lateHead);
    note(2);
    out.jump(rareEdge(entersLast));

    out.appendTo(entersFirst);
    out.branch(out.testNonZero64(ways, 16), normalEdge(leavesForThenHead), normalEdge(stays));

    out.appendTo(leavesForThenHead);
    note(4);
    out.jump(normalEdge(thenHead));

    out.appendTo(entersSecond);
    note(6);
    out.jump(normalEdge(rareJoin));

    out.appendTo(entersThird);
    out.branch(out.testNonZero64(ways, 32), normalEdge(leavesForLateHead), normalEdge(stays));

    out.appendTo(leavesForLateHead);
    note(5);
    out.jump(normalEdge(lateHead));

    out.appendTo(entersLast);
    note(7);
    out.jump(normalEdge(rareJoin));

    out.appendTo(stays);
    note(8);
    out.jump(normalEdge(rareJoin));

    out.appendTo(rareJoin);
    out.ret(out.constInt64(200));
    compileOrFail(test);

    const Order& order = test.procedure().order();
    AOT_ALLOCATION_TEST_CHECK(test, order.headOf(parts) < order.headOf(thenHead) && order.headOf(thenHead) < order.headOf(otherwise) && order.headOf(otherwise) < order.headOf(readsIt) && order.headOf(readsIt) < order.headOf(joins));
    AOT_ALLOCATION_TEST_CHECK(test, order.headOf(joins) < order.headOf(lateHead) && order.headOf(lateHead) < order.headOf(entersFirst));
    AOT_ALLOCATION_TEST_CHECK(test, order.of(thenHead).isLoopHead && order.of(lateHead).isLoopHead && order.loops()[order.of(lateHead).loop].parent == order.of(thenHead).loop);
    AOT_ALLOCATION_TEST_CHECK(test, !allocationTestRegisterAtHead(test, thenHead, value) && !allocationTestRegisterAtHead(test, lateHead, value));

    struct Way {
        uint64_t ways;
        uint64_t value;
        uint64_t result;
        std::initializer_list<unsigned> noted;
    };
    for (const Way& way : std::initializer_list<Way> {
        { 2, 40, 100, { 1 } },
        { 0, 40, 100, { 3 } },
        { 4, 40, 200, { 6 } },
        { 2 | 8, 40, 200, { 1, 8 } },
        { 2 | 8 | 32, 40, 200, { 1, 5, 2, 7 } },
        { 8 | 32, 40, 200, { 3, 5, 2, 7 } },
        { 0, 42, 200, { 2, 7 } },
        { 1, 40, 200, { 8 } },
        { 1 | 16, 40, 100, { 4, 1 } },
        { 1 | 16 | 8 | 32, 40, 200, { 4, 1, 5, 2, 7 } },
    }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        words[0] = way.value;
        std::array<uint64_t, 64> expected = words;
        for (unsigned index : way.noted)
            expected[index] = index == 3 ? way.value : index;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way.ways, words.data()) == way.result);
        AOT_ALLOCATION_TEST_CHECK(test, words == expected);
    }
}

static void allocatorAsksEachLoopAroundBlockInTurn()
{
    ProcedureTraits traits;
    traits.checksAllocation = true;
    traits.shouldUseFallbackBlockOrder = true;
    TestProcedure test(traits);
    Output& out = test.out();
    LValue ways = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock second = out.newBlock();
    LBasicBlock third = out.newBlock();
    LBasicBlock outerHead = out.newBlock();
    LBasicBlock readsIt = out.newBlock();
    LBasicBlock innerHead = out.newBlock();
    LBasicBlock ends = out.newBlock();
    LBasicBlock side = out.newBlock();
    LBasicBlock returns = out.newBlock();
    LBasicBlock rare = out.newBlock();
    LBasicBlock leavesForOuterHead = out.newBlock();
    LBasicBlock leavesForInnerHead = out.newBlock();
    auto note = [&](unsigned index) {
        out.store64(out.constInt64(index), allocationTestWordAt(test, buffer, index));
    };

    LValue value = out.load64(allocationTestWordAt(test, buffer, 0));
    out.branch(out.testNonZero64(ways, 1), normalEdge(second), normalEdge(side));

    out.appendTo(second);
    out.branch(out.testNonZero64(ways, 2), normalEdge(third), normalEdge(side));

    out.appendTo(third);
    out.branch(out.testNonZero64(ways, 4), normalEdge(outerHead), normalEdge(innerHead));

    out.appendTo(outerHead);
    note(1);
    out.jump(normalEdge(readsIt));

    out.appendTo(readsIt);
    out.store64(value, allocationTestWordAt(test, buffer, 3));
    out.jump(normalEdge(ends));

    out.appendTo(innerHead);
    note(2);
    out.jump(normalEdge(ends));

    out.appendTo(ends);
    out.ret(out.constInt64(100));

    out.appendTo(side);
    out.branch(out.testNonZero64(ways, 8), rareEdge(rare), normalEdge(returns));

    out.appendTo(returns);
    out.ret(out.constInt64(200));

    out.appendTo(rare);
    note(4);
    out.branch(out.testNonZero64(ways, 16), normalEdge(leavesForOuterHead), normalEdge(leavesForInnerHead));

    out.appendTo(leavesForOuterHead);
    note(5);
    out.jump(normalEdge(outerHead));

    out.appendTo(leavesForInnerHead);
    note(6);
    out.jump(normalEdge(innerHead));
    compileOrFail(test);

    const Order& order = test.procedure().order();
    AOT_ALLOCATION_TEST_CHECK(test, order.headOf(outerHead) < order.headOf(innerHead) && order.headOf(innerHead) < order.headOf(side) && order.headOf(side) < order.headOf(rare));
    AOT_ALLOCATION_TEST_CHECK(test, order.of(outerHead).isLoopHead && order.of(innerHead).isLoopHead && order.loops()[order.of(innerHead).loop].parent == order.of(outerHead).loop);
    AOT_ALLOCATION_TEST_CHECK(test, order.of(side).loop == order.of(innerHead).loop && order.loops()[order.of(side).loop].hasSideEntrance && order.loops()[order.of(outerHead).loop].hasSideEntrance);
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, outerHead, value) && !allocationTestRegisterAtHead(test, innerHead, value));

    struct Way {
        uint64_t ways;
        uint64_t result;
        std::initializer_list<unsigned> noted;
    };
    for (const Way& way : std::initializer_list<Way> {
        { 0, 200, { } },
        { 1, 200, { } },
        { 1 | 2, 100, { 2 } },
        { 1 | 2 | 4, 100, { 1, 3 } },
        { 8, 100, { 4, 6, 2 } },
        { 8 | 16, 100, { 4, 5, 1, 3 } },
        { 1 | 8 | 16, 100, { 4, 5, 1, 3 } },
    }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        words[0] = 40;
        std::array<uint64_t, 64> expected = words;
        for (unsigned index : way.noted)
            expected[index] = index == 3 ? 40 : index;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(way.ways, words.data()) == way.result);
        AOT_ALLOCATION_TEST_CHECK(test, words == expected);
    }
}

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

template<bool isReadBehindJoin>
static void allocatorLaysPhiOfJoinInWordOfPhiThatIsHandedToIt()
{
    constexpr unsigned numberOfCounts = 24;
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
    Vector<LValue> cameRound;
    Vector<LValue> joined;
    for (unsigned index = 0; index < numberOfCounts; ++index) {
        cameRound.append(out.phiIn(head, Int64));
        joined.append(out.phiIn(join, Int64));
        out.upsilon(out.constInt64(index), cameRound.last());
    }
    out.upsilon(rounds, left);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    allocationTestAppendCall(test, nullptr);
    out.branch(out.testNonZero64(left, 1), normalEdge(counts), normalEdge(skips));

    out.appendTo(counts);
    for (unsigned index = 0; index < numberOfCounts; ++index)
        out.upsilon(out.add(cameRound[index], index + 1), joined[index]);
    out.jump(normalEdge(join));

    out.appendTo(skips);
    for (unsigned index = 0; index < numberOfCounts; ++index)
        out.upsilon(cameRound[index], joined[index]);
    out.jump(normalEdge(join));

    out.appendTo(join);
    allocationTestAppendCall(test, nullptr);
    for (unsigned index = 0; index < numberOfCounts; ++index)
        out.upsilon(isReadBehindJoin ? out.sub(out.add(joined[index], joined[index]), cameRound[index]) : joined[index], cameRound[index]);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    for (unsigned index = 0; index < numberOfCounts; ++index)
        out.store64(cameRound[index], allocationTestWordAt(test, buffer, index));
    out.ret(rounds);
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    Block* sourceOfBackEdge = allocationTestSourceOfBackEdge(test.procedure().order(), head);
    AOT_ALLOCATION_TEST_CHECK(test, sourceOfBackEdge);
    unsigned numberThatShare = 0;
    Vector<SpillIndex> spillsThatAreShared;
    for (unsigned index = 0; index < numberOfCounts; ++index) {
        if (!allocation.isSpilled(cameRound[index]) || allocation.spillOf(cameRound[index]) != allocation.spillOf(joined[index]))
            continue;
        ++numberThatShare;
        spillsThatAreShared.append(allocation.spillOf(joined[index]));
        AOT_ALLOCATION_TEST_CHECK(test, allocation.spills()[spillsThatAreShared.last()].isSharedByPhis);
        for (LValue phi : { cameRound[index], joined[index] })
            AOT_ALLOCATION_TEST_CHECK(test, allocation.isWrittenByEdges(phi) && !allocation.isStoredBehindDefinition(phi));
    }
    AOT_ALLOCATION_TEST_CHECK(test, isReadBehindJoin ? !numberThatShare : numberThatShare >= numberOfCounts / 2);
    for (const Block* block : { static_cast<const Block*>(skips), static_cast<const Block*>(sourceOfBackEdge) }) {
        const BlockSteps& steps = allocation.stepsOf(block);
        for (uint32_t index = steps.beginOfEdgeMoves; index < steps.endOfEdgeMoves; ++index) {
            Location destination = allocation.step(index).destination;
            AOT_ALLOCATION_TEST_CHECK(test, !destination.isSpill() || !spillsThatAreShared.contains(destination.spillIndex()));
        }
    }

    for (uint64_t numberOfRounds : { 0u, 1u, 2u, 3u, 7u }) {
        std::array<uint64_t, numberOfCounts> counted;
        counted.fill(~uint64_t { 0 });
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, counted.data()) == numberOfRounds);
        for (unsigned index = 0; index < numberOfCounts; ++index) {
            uint64_t expected = index;
            for (uint64_t round = numberOfRounds; round; --round) {
                uint64_t atJoin = expected + (round & 1 ? index + 1 : 0);
                expected = isReadBehindJoin ? atJoin + atJoin - expected : atJoin;
            }
            AOT_ALLOCATION_TEST_CHECK(test, counted[index] == expected);
        }
    }
}

template<bool isReadBehindJoin>
static void allocatorLaysPhiOfJoinInWordOfValueThatIsHandedToIt()
{
    constexpr unsigned numberOfWords = 24;
    TestProcedure test;
    Output& out = test.out();
    LValue changes = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock changed = out.newBlock();
    LBasicBlock unchanged = out.newBlock();
    LBasicBlock join = out.newBlock();
    Vector<LValue> read = allocationTestLoadWords(test, buffer, 0, numberOfWords, Int64);
    Vector<LValue> joined;
    for (unsigned index = 0; index < numberOfWords; ++index)
        joined.append(out.phiIn(join, Int64));
    allocationTestAppendCall(test, nullptr);
    out.branch(changes, normalEdge(changed), normalEdge(unchanged));

    out.appendTo(changed);
    for (unsigned index = 0; index < numberOfWords; ++index)
        out.upsilon(out.constInt64(index + 1), joined[index]);
    out.jump(normalEdge(join));

    out.appendTo(unchanged);
    for (unsigned index = 0; index < numberOfWords; ++index)
        out.upsilon(read[index], joined[index]);
    out.jump(normalEdge(join));

    out.appendTo(join);
    allocationTestAppendCall(test, nullptr);
    for (unsigned index = 0; index < numberOfWords; ++index)
        out.store64(isReadBehindJoin ? out.add(joined[index], read[index]) : joined[index], allocationTestWordAt(test, buffer, index));
    out.ret(changes);
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    unsigned numberThatShare = 0;
    Vector<SpillIndex> spillsThatAreShared;
    for (unsigned index = 0; index < numberOfWords; ++index) {
        AOT_ALLOCATION_TEST_CHECK(test, read[index]->opcode() == Load);
        if (!allocation.isSpilled(read[index]) || allocation.spillOf(read[index]) != allocation.spillOf(joined[index]))
            continue;
        ++numberThatShare;
        spillsThatAreShared.append(allocation.spillOf(joined[index]));
        const SpillFacts& word = allocation.spills()[spillsThatAreShared.last()];
        AOT_ALLOCATION_TEST_CHECK(test, word.isSharedByPhis && word.value == read[index]);
        AOT_ALLOCATION_TEST_CHECK(test, allocation.isStoredBehindDefinition(read[index]) && !allocation.isWrittenByEdges(read[index]));
        AOT_ALLOCATION_TEST_CHECK(test, allocation.isWrittenByEdges(joined[index]) && !allocation.isStoredBehindDefinition(joined[index]));
    }
    AOT_ALLOCATION_TEST_CHECK(test, isReadBehindJoin ? !numberThatShare : numberThatShare >= numberOfWords / 2);
    const BlockSteps& steps = allocation.stepsOf(unchanged);
    for (uint32_t index = steps.beginOfEdgeMoves; index < steps.endOfEdgeMoves; ++index) {
        Location destination = allocation.step(index).destination;
        AOT_ALLOCATION_TEST_CHECK(test, !destination.isSpill() || !spillsThatAreShared.contains(destination.spillIndex()));
    }

    for (uint64_t isChanged : { 0u, 1u }) {
        std::array<uint64_t, numberOfWords> words;
        for (unsigned index = 0; index < numberOfWords; ++index)
            words[index] = 100 + index;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(isChanged, words.data()) == isChanged);
        for (unsigned index = 0; index < numberOfWords; ++index)
            AOT_ALLOCATION_TEST_CHECK(test, words[index] == (isChanged ? index + 1 : 100 + index) + (isReadBehindJoin ? 100 + index : 0));
    }
}

static void allocatorRotatesPhisThatLieInMemory()
{
    constexpr unsigned numberOfCounts = 24;
    TestProcedure test;
    Output& out = test.out();
    LValue rounds = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(head, Int64);
    Vector<LValue> phis;
    for (unsigned index = 0; index < numberOfCounts; ++index) {
        phis.append(out.phiIn(head, Int64));
        out.upsilon(out.constInt64(index * index + 7), phis.last());
    }
    out.upsilon(rounds, left);
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(left, normalEdge(body), normalEdge(exit));

    out.appendTo(body);
    allocationTestAppendCall(test, nullptr);
    for (unsigned index = 0; index < numberOfCounts; ++index)
        out.upsilon(phis[(index + 1) % numberOfCounts], phis[index]);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    for (unsigned index = 0; index < numberOfCounts; ++index)
        out.store64(phis[index], allocationTestWordAt(test, buffer, index));
    out.ret(rounds);
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    unsigned numberInMemory = 0;
    for (unsigned index = 0; index < numberOfCounts; ++index) {
        if (!allocation.isSpilled(phis[index]))
            continue;
        ++numberInMemory;
        for (unsigned other = 0; other < index; ++other)
            AOT_ALLOCATION_TEST_CHECK(test, allocation.spillOf(phis[other]) != allocation.spillOf(phis[index]));
    }
    AOT_ALLOCATION_TEST_CHECK(test, numberInMemory >= numberOfCounts / 2);

    for (uint64_t numberOfRounds : { 0u, 1u, 2u, 5u, 24u, 25u }) {
        std::array<uint64_t, numberOfCounts> rotated;
        rotated.fill(~uint64_t { 0 });
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, rotated.data()) == numberOfRounds);
        for (unsigned index = 0; index < numberOfCounts; ++index) {
            uint64_t from = (index + numberOfRounds) % numberOfCounts;
            AOT_ALLOCATION_TEST_CHECK(test, rotated[index] == from * from + 7);
        }
    }
}

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

static void allocatorCountsWhatCrossesPatchesOfLoopAgainstRegistersTheyLeave()
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
    for (unsigned count = 0; count < 4; ++count) {
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
    LValue madeInTurn = out.opaque(out.add(sum, addend));
    allocationTestAppendPatchThatTakes(test, file.allocatableIn(Bank::GP) & ~leftAlone);
    out.upsilon(out.add(madeInTurn, sum), sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.store64(sum, allocationTestWordAt(test, buffer, 0));
    out.ret(through);
    compileOrFail(test);

    AllocationTestText text(test);
    const Allocation& allocation = test.procedure().allocation();
    const LoopFacts& facts = test.procedure().liveness().of(test.procedure().order().loops()[0], test.procedure().order());
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestCountOf(leftAlone) == 4 && facts.pressureAcrossPatches[static_cast<unsigned>(Bank::GP)] == 4);
    AOT_ALLOCATION_TEST_CHECK(test, allocation.isSpilled(through) && text.isStoredOnceBehindDefinition(through));
    for (LValue value : { addend, left, sum, madeInTurn })
        AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(value));
    for (const Block* block : { head, body })
        AOT_ALLOCATION_TEST_CHECK(test, !text.count("reload"_s, nullptr, block) && !text.count("save"_s, nullptr, block));

    constexpr uint64_t added = 5;
    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        uint64_t expected = 0;
        for (uint64_t round = 0; round < numberOfRounds; ++round)
            expected = expected + added + expected;
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, added, words.data()) == words[1]);
        AOT_ALLOCATION_TEST_CHECK(test, words[0] == expected);
    }
}

static void allocatorCountsWhatLoopReadsOnlyAheadOfPatchAmongWhatCrossesIt()
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
    for (unsigned count = 0; count < 4; ++count) {
        leftAlone |= others & ~(others - 1);
        others &= others - 1;
    }
    LValue through = out.load64(allocationTestWordAt(test, buffer, 1));
    LValue readAheadOfPatch = out.load64(allocationTestWordAt(test, buffer, 2));
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
    LValue madeInTurn = out.opaque(out.add(sum, readAheadOfPatch));
    allocationTestAppendPatchThatTakes(test, file.allocatableIn(Bank::GP) & ~leftAlone);
    out.upsilon(out.add(madeInTurn, addend), sum);
    out.upsilon(out.sub(left, 1), left);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.store64(sum, allocationTestWordAt(test, buffer, 0));
    out.ret(through);
    compileOrFail(test);

    AllocationTestText text(test);
    const Allocation& allocation = test.procedure().allocation();
    const LoopFacts& facts = test.procedure().liveness().of(test.procedure().order().loops()[0], test.procedure().order());
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestCountOf(leftAlone) == 4 && facts.pressureAcrossPatches[static_cast<unsigned>(Bank::GP)] == 4);
    AOT_ALLOCATION_TEST_CHECK(test, allocation.isSpilled(through) && text.isStoredOnceBehindDefinition(through));
    for (const Block* block : { head, body })
        AOT_ALLOCATION_TEST_CHECK(test, !text.count("reload"_s, nullptr, block) && !text.count("save"_s, nullptr, block));

    constexpr uint64_t added = 5;
    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, added, words.data()) == words[1]);
        AOT_ALLOCATION_TEST_CHECK(test, words[0] == numberOfRounds * (added + words[2]));
    }
}

static void allocatorLoadsNothingAtJoinForRareArm()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = allocationTestCountOf(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP)) + 2;
    LValue selector = test.argumentGPR(0);
    LValue buffer = test.argumentGPR(1);
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

static Vector<unsigned> allocationTestInstructionsNamingWords(TestProcedure& test)
{
    const Allocation& allocation = test.procedure().allocation();
    const Frame& frame = test.procedure().frame();
    Vector<unsigned> result(FillWith { }, frame.bytesBelowFrameRecord() / sizeof(uint64_t) + 1, 0u);
    auto note = [&](Location location) {
        if (location.isSpill())
            ++result[static_cast<size_t>((7 - frame.offsetFromFPOf(location.spillIndex())) / 8)];
    };
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

static void slotsCountInstructionsThatNameWordAndNotReaders()
{
    TestProcedure test;
    Output& out = test.out();
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
    Vector<uint8_t> setup { 0x55, 0x48, 0x89, 0xe5 };
    for (size_t index = saves.registerCount(); index--;) {
        unsigned number = saves.at(index).reg().gpr();
        if (number >= 8)
            setup.append(0x41);
        setup.append(0x50 | (number & 7));
    }
    unsigned bytesLeft = frame.bytesBelowFrameRecord() - count * sizeof(CPURegister);
    AOT_ALLOCATION_TEST_CHECK(test, bytesLeft < 128);
    if (bytesLeft)
        setup.appendList<uint8_t>({ 0x48, 0x83, 0xec, static_cast<uint8_t>(bytesLeft) });
    AOT_ALLOCATION_TEST_CHECK(test, !std::ranges::search(test.linkedCode(), setup.span()).empty());

    std::array<uint64_t, 64> words = allocationTestWords<uint64_t>();
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(words.data()) == allocationTestMixedAll(std::span<const uint64_t>(words).first(count)));
}

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
    AOT_ALLOCATION_TEST_CHECK(test, loopFacts.pressure[static_cast<unsigned>(Bank::GP)] == numberOfWords + 3);
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, head, constant));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("materialize"_s, constant) == 1 && !text.count("materialize"_s, constant, body) && !test.procedure().allocation().usedCalleeSaves());

    for (uint64_t numberOfRounds : { 0u, 1u, 3u }) {
        std::array<uint64_t, 64> memory = allocationTestWords<uint64_t>();
        AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(numberOfRounds, memory.data()) == (sumIsReadBehindLoop ? numberOfRounds * added : 0));
        AOT_ALLOCATION_TEST_CHECK(test, !numberOfRounds || memory[0] == numberOfRounds * added);
    }
}

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
    AOT_ALLOCATION_TEST_CHECK(test, number->opcode() == NumberOf && extended->opcode() == ZExt32 && isFreeWhenCoalesced(test.procedure().liveness(), extended));
    AOT_ALLOCATION_TEST_CHECK(test, std::ranges::count(test.procedure().liveness().of(order.loops()[order.of(head).loop], order).valuesReadFromOutside, number) == (isX86_64() ? 0 : 1));
    AOT_ALLOCATION_TEST_CHECK(test, text.count("materialize"_s, number) == 1 && text.count("materialize"_s, number, body) == (isX86_64() ? 1u : 0u));
    AOT_ALLOCATION_TEST_CHECK(test, allocationTestRegisterAtHead(test, head, number).isSet() == !isX86_64());
}

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

static void allocatorStoresNothingForRegisterThatReturnTakes()
{
    std::array<uint64_t, 3> elements { 11, 22, 33 };
    std::array<const uint64_t*, 1> ownerInMemory { elements.data() };

    TestProcedure test;
    Output& out = test.out();
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
    LValue selector = test.argumentGPR(1);
    LValue address = test.argumentGPR(0);
    PatchOperation* owner = out.patch(Int64);
    owner->effects = Effects::none();
    owner->setRunsWithoutFrame(true);
    owner->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    owner->appendSomeRegister(address);
    owner->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
        jit.move(params[1].gpr(), params[0].gpr());
    });
    LValue storage = out.loadPtr(out.address(storageOfOwner, owner));
    LBasicBlock returnsConstant = out.newBlock();
    LBasicBlock readsStorage = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), normalEdge(returnsConstant), normalEdge(readsStorage));

    out.appendTo(returnsConstant);
    out.ret(out.constInt64(5));

    out.appendTo(readsStorage);
    out.ret(out.load64(out.address(test.heaps().argumentBuffer, storage, 2 * sizeof(uint64_t))));
    compileOrFail(test);

    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_ALLOCATION_TEST_CHECK(test, storage->has(OperationFlag::IsInnerPointer) && test.procedure().liveness().of(owner).has(ValueFlag::IsKeptForCollector));
    AOT_ALLOCATION_TEST_CHECK(test, order.headOf(returnsConstant) < order.headOf(readsStorage) && allocationTestRegisterAtHead(test, returnsConstant, owner) == Reg(GPRInfo::returnValueGPR));
    AOT_ALLOCATION_TEST_CHECK(test, !allocation.isSpilled(owner) && test.procedure().frame().coverage() == FrameCoverage::None);

    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 1 }) == 5);
    AOT_ALLOCATION_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 0 }) == 33);
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
    LValue nextSum = out.add(sum, out.opaque(extended));
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

struct AllocationTestSumWithShift {
    LValue (*build)(Output&, LValue, LValue);
    uint64_t (*compute)(uint64_t, uint64_t);
};

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

#undef AOT_ALLOCATION_TEST_CHECK

} // namespace Machine

std::span<const NativeTest> machineAllocationTests()
{
    static constexpr NativeTest tests[] = {
        { "machine-allocation/edge-that-gets-moves-is-split"_s, Machine::allocatorSplitsEdgeThatGetsMoves },
        { "machine-allocation/operands-of-a-rare-patch-stay-where-they-lie"_s, Machine::allocatorLeavesOperandsOfRarePatchWhereTheyLie },
        { "machine-allocation/work-grows-as-the-edges-with-moves-into-one-block-do"_s, Machine::allocatorWorkGrowsAsEdgesWithMovesIntoOneBlockDo },
        { "machine-allocation/work-grows-as-the-values-that-outlive-their-block-and-the-blocks-that-ask-what-lives-on-their-ways-do"_s, Machine::allocatorWorkGrowsAsValuesThatOutliveTheirBlockAndBlocksThatAskWhatLivesOnTheirWaysDo },
        { "machine-allocation/work-grows-as-the-guards-ahead-of-the-framed-part-do"_s, Machine::allocatorWorkGrowsAsGuardsAheadOfFramedPartDo },
        { "machine-allocation/work-grows-as-the-rare-ways-back-in-behind-the-head-of-the-framed-part-do"_s, Machine::allocatorWorkGrowsAsRareWaysBackInBehindHeadOfFramedPartDo },
        { "machine-allocation/work-grows-as-the-edges-of-a-switch-that-get-moves-do"_s, Machine::allocatorWorkGrowsAsEdgesOfSwitchThatGetMovesDo },
        { "machine-allocation/operands-of-a-patch-that-ends-the-function-stay-where-they-lie"_s, Machine::allocatorLeavesOperandsOfPatchThatEndsFunctionWhereTheyLie },
        { "machine-allocation/nothing-is-kept-where-the-generator-makes-a-constant"_s, Machine::allocatorKeepsNothingWhereGeneratorMakesConstant },
        { "machine-allocation/a-move-out-of-the-result-register-is-left-to-the-callee-that-does-it"_s, Machine::encoderLeavesMoveOutOfResultRegisterToCalleeThatDoesIt },
        { "machine-allocation/a-move-out-of-the-result-register-is-left-to-the-callee-that-ends-its-block"_s, Machine::encoderLeavesMoveOutOfResultRegisterToCalleeThatEndsItsBlock },
        { "machine-allocation/a-pair-of-moves-is-left-to-the-callee-that-does-both"_s, Machine::encoderLeavesPairOfMovesToCalleeThatDoesBoth },
        { "machine-allocation/a-store-of-another-register-stands-between-the-move-and-the-callee-that-does-it"_s, Machine::encoderStepsOverStoreOfAnotherRegisterOnTheWayToCalleeThatDoesMove },
        { "machine-allocation/three-number-phis-rotate"_s, Machine::allocatorRotatesPhisBySwapping<double, 3> },
        { "machine-allocation/numbers-across-a-call-take-callee-saved-registers"_s, Machine::allocatorKeepsValuesAcrossCallInCalleeSavedRegisters<double> },
        { "machine-allocation/integer-loses-its-register-to-a-loop-under-pressure"_s, Machine::allocatorDecidesByPressureWhatKeepsItsRegisterThroughLoop<uint64_t, true> },
        { "machine-allocation/number-sum-read-behind-its-loop-comes-round-in-its-register"_s, Machine::allocatorBringsSumThatIsReadBehindLoopRoundInItsRegister<double, true> },
        { "machine-allocation/nothing-is-spilled-ahead-of-a-join-that-a-rare-block-leads-back-to"_s, Machine::allocatorSpillsNothingAheadOfJoinThatRareBlockLeadsBackTo },
        { "machine-allocation/a-head-entered-from-the-side-of-a-loop-holds-nothing-that-no-way-on-reads"_s, Machine::allocatorLeavesOutOfHeadEnteredFromSideWhatNoWayOnReads },
        { "machine-allocation/no-block-is-laid-ahead-of-all-its-predecessors"_s, Machine::orderLaysNoBlockAheadOfAllItsPredecessors },
        { "machine-allocation/the-second-entrance-of-a-cycle-is-judged-as-its-outer-head-is"_s, Machine::allocatorJudgesSecondEntranceOfCycleAsItsOuterHead },
        { "machine-allocation/a-read-off-another-side-entrance-is-passed-over-as-the-loops-own-head-passes-it-over"_s, Machine::allocatorPassesOverReadOffAnotherSideEntranceAsOwnHeadDoes },
        { "machine-allocation/each-loop-around-a-block-is-asked-in-turn"_s, Machine::allocatorAsksEachLoopAroundBlockInTurn },
        { "machine-allocation/sum-comes-round-in-its-register-past-a-join-that-a-rare-block-leads-back-to"_s, Machine::allocatorBringsSumRoundInItsRegisterPastJoinThatRareBlockLeadsBackTo },
        { "machine-allocation/value-for-a-phi-is-made-in-the-register-of-the-phi"_s, Machine::allocatorDefinesValueInRegisterOfPhiWhoseHeadIsFixed },
        { "machine-allocation/an-arm-that-dies-in-a-select-is-made-where-the-result-is-wanted"_s, Machine::allocatorDefinesArmThatDiesInSelectWhereResultIsWanted },
        { "machine-allocation/the-register-that-a-select-in-a-loop-wants-is-left-to-the-arm-made-in-the-loop"_s, Machine::allocatorLeavesRegisterThatSelectInLoopWantsToArmMadeInLoop },
        { "machine-allocation/a-phi-stays-where-the-call-in-its-block-takes-it"_s, Machine::allocatorLeavesPhiWhereCallInItsBlockTakesIt },
        { "machine-allocation/an-argument-that-lives-across-a-call-takes-its-home-where-it-is-defined"_s, Machine::allocatorGivesArgumentThatLivesAcrossCallItsHomeWhereItIsDefined },
        { "machine-allocation/an-argument-stays-where-the-call-in-its-block-takes-it"_s, Machine::allocatorLeavesArgumentWhereCallInItsBlockTakesIt },
        { "machine-allocation/at-the-entry-the-registers-that-callees-keep-go-to-the-arguments-that-are-read-most"_s, Machine::allocatorGivesCalleeSavesAtEntryToArgumentsThatAreReadMost },
        { "machine-allocation/count-that-one-arm-adds-to-comes-round-in-its-register"_s, Machine::allocatorBringsCountThatOneArmAddsToRoundInItsRegister },
        { "machine-allocation/phi-of-a-join-lies-in-the-word-of-the-phi-that-is-handed-to-it"_s, Machine::allocatorLaysPhiOfJoinInWordOfPhiThatIsHandedToIt<false> },
        { "machine-allocation/phi-of-a-join-has-a-word-of-its-own-where-what-is-handed-to-it-lives-on"_s, Machine::allocatorLaysPhiOfJoinInWordOfPhiThatIsHandedToIt<true> },
        { "machine-allocation/phi-of-a-join-lies-in-the-word-of-the-value-that-is-handed-to-it"_s, Machine::allocatorLaysPhiOfJoinInWordOfValueThatIsHandedToIt<false> },
        { "machine-allocation/phi-of-a-join-has-a-word-of-its-own-where-the-value-that-is-handed-to-it-lives-on"_s, Machine::allocatorLaysPhiOfJoinInWordOfValueThatIsHandedToIt<true> },
        { "machine-allocation/phis-that-lie-in-memory-rotate"_s, Machine::allocatorRotatesPhisThatLieInMemory },
        { "machine-allocation/what-only-lives-through-a-loop-gives-way-for-the-registers-its-patches-leave-alone"_s, Machine::allocatorGivesRegistersThatPatchesLeaveAloneToWhatLoopUses },
        { "machine-allocation/what-crosses-the-patches-of-a-loop-is-counted-against-the-registers-they-leave"_s, Machine::allocatorCountsWhatCrossesPatchesOfLoopAgainstRegistersTheyLeave },
        { "machine-allocation/what-a-loop-reads-only-ahead-of-a-patch-is-counted-among-what-crosses-it"_s, Machine::allocatorCountsWhatLoopReadsOnlyAheadOfPatchAmongWhatCrossesIt },
        { "machine-allocation/nothing-is-loaded-at-a-join-for-a-rare-arm"_s, Machine::allocatorLoadsNothingAtJoinForRareArm },
        { "machine-allocation/nothing-is-loaded-on-the-ways-into-a-rare-join"_s, Machine::allocatorLoadsNothingOnWaysIntoRareJoin },
        { "machine-allocation/nothing-is-loaded-at-a-join-for-a-read-behind-a-call"_s, Machine::allocatorLoadsNothingAtJoinForReadBehindCall },
        { "machine-allocation/join-holds-what-is-read-past-a-call-in-one-arm"_s, Machine::allocatorKeepsValuesAtJoinForReadPastCallInOneArm },
        { "machine-allocation/word-that-a-register-serves-gives-way-to-words-that-are-loaded"_s, Machine::slotsCountInstructionsThatNameWordAndNotReaders },
        { "machine-allocation/values-that-take-turns-in-a-word-count-together"_s, Machine::slotsCountInstructionsOfAllThatTakeTurnsInWord },
        { "machine-allocation/stack-slot-lies-in-reach-of-the-frame-pointer-where-enough-instructions-name-it"_s, Machine::slotsKeepStackSlotInReachOfFramePointerWhereEnoughInstructionsNameIt },
        { "machine-allocation/decoded-reference-with-offset-base-is-one-instruction"_s, Machine::encoderDecodesReferenceInOneInstruction<true> },
        { "machine-allocation/scaled-index-is-added-to-a-pinned-register-with-a-constant"_s, Machine::encoderAddsScaledIndexToPinnedRegisterWithConstant },
        { "machine-allocation/scaled-index-from-an-earlier-block-is-added-where-the-sum-sinks"_s, Machine::encoderAddsScaledIndexFromEarlierBlockWhereSumSinks },
        { "machine-allocation/shifted-index-that-is-the-first-child-is-added-in-one-instruction"_s, Machine::encoderAddsShiftedIndexThatIsFirstChild },
        { "machine-allocation/decoded-reference-or-marker-has-no-copy"_s, Machine::encoderDecodesReferenceOrMarkerWithoutCopy },
        { "machine-allocation/shifted-index-that-one-instruction-may-not-add"_s, Machine::encoderAddsShiftedIndexThatOneInstructionMayNotAdd },
        { "machine-allocation/sum-and-constant-are-added-in-one-instruction-beside-an-operand-that-lives-on"_s, Machine::encoderAddsSumAndConstantInOneInstruction<true> },
#if CPU(X86_64)
        { "machine-allocation/frame-setup-pushes-the-callee-saves"_s, Machine::frameSetupPushesCalleeSaves },
        { "machine-allocation/tests-that-leave-for-one-rare-block-are-chained"_s, Machine::encoderChainsTestsThatLeaveForOneRareBlock },
#endif
        { "machine-allocation/integer-constant-is-made-once-ahead-of-a-loop"_s, Machine::allocatorMakesConstantOnceAheadOfLoop<uint64_t> },
        { "machine-allocation/spilled-values-are-loaded-once-ahead-of-a-loop"_s, Machine::allocatorLoadsSpilledValuesOnceAheadOfLoop },
        { "machine-allocation/constant-that-a-full-loop-would-evict-is-not-kept"_s, Machine::allocatorKeepsNoConstantThatLoopWouldEvict<false> },
        { "machine-allocation/phi-read-behind-its-loop-counts-up-to-its-last-read-in-the-loop"_s, Machine::scanCountsPhiThatIsReadBehindLoopUpToItsLastReadInLoop<true> },
        { "machine-allocation/phi-read-in-its-loop-alone-counts-the-same"_s, Machine::scanCountsPhiThatIsReadBehindLoopUpToItsLastReadInLoop<false> },
        { "machine-allocation/constant-whose-reader-takes-its-register-is-not-kept"_s, Machine::allocatorKeepsNoConstantWhoseReaderTakesItsRegister },
        { "machine-allocation/value-returned-behind-a-loop-is-made-in-the-return-register"_s, Machine::allocatorDefinesValueReturnedBehindLoopInReturnRegister },
        { "machine-allocation/phis-that-a-rare-chain-behind-a-later-call-reads-stay-out-of-memory"_s, Machine::allocatorKeepsPhisThatRareChainBehindLaterCallReadsOutOfMemory },
        { "machine-allocation/what-a-rare-arm-alone-reads-is-counted-once"_s, Machine::allocatorCountsWhatRareArmAloneReadsOnce },
        { "machine-allocation/owner-that-the-other-arm-reads-outlives-its-storage"_s, Machine::allocatorKeepsOwnerThatOtherArmReadsWhileItsStorageIsRead },
        { "machine-allocation/what-the-return-register-holds-at-a-return-goes-without-a-store"_s, Machine::allocatorStoresNothingForRegisterThatReturnTakes },
        { "machine-allocation/rare-arm-costs-the-hot-path-nothing-for-numbers"_s, Machine::allocatorKeepsRareArmOffHotPath<double> },
        { "machine-allocation/pinned-constant-is-read-where-it-lies"_s, Machine::allocatorReadsPinnedConstantWhereItLies },
        { "machine-allocation/zero-extension-of-a-count-that-starts-at-a-truncated-word-is-an-instruction"_s, Machine::allocatorExtendsCountOfLoop<false> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
