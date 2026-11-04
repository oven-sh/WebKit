/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineFrameForms.h"
#include "AOTMachineTestSupport.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "StackAlignment.h"
#include <array>
#include <bit>
#include <wtf/DataLog.h>

// No request goes to the builder between patch() and the last word of the setup: the next request seals the Patch.
// The wrapper of TestProcedure restores every register that the C convention wants kept, so a test that asks whether compiled code restores them has to look before it returns.

namespace JSC { namespace AOT { namespace Machine {

[[noreturn]] static void failFrameFormTest(const TestProcedure& test, const char* file, int line, const char* condition)
{
    dataLogLn(test.printedProcedure(), test.printedAllocation(), test.printedFrame());
    failNativeTest(file, line, condition);
}

#define AOT_FRAME_FORM_TEST_CHECK(test, ...) do { \
        if (!(__VA_ARGS__)) [[unlikely]] \
            failFrameFormTest(test, __FILE__, __LINE__, #__VA_ARGS__); \
    } while (false)

static constexpr int32_t bytesOfFrameFormTestWord = sizeof(uint64_t);
// From where the stack pointer stands at the entry down to the place of the frame record.
static constexpr int32_t bytesFromEntryToFrameFormRecord = isX86_64() ? bytesOfFrameFormTestWord : 2 * bytesOfFrameFormTestWord;

// By Reg::index(): what a callee-saved register held at the entry. Then these two.
static constexpr unsigned stackPointerInFrameFormRecord = 64;
static constexpr unsigned sumInFrameFormRecord = 65;
using FrameFormRecord = std::array<uint64_t, 66>;

static CCallHelpers::Address wordOfFrameFormRecord(GPRReg record, unsigned index)
{
    return CCallHelpers::Address(record, static_cast<int32_t>(index * sizeof(uint64_t)));
}

static ProcedureTraits traitsOfFrameFormTest(bool chargesForCalleeSaves, bool mayOmitFrameRecord)
{
    ProcedureTraits traits;
    traits.chargesForCalleeSaves = chargesForCalleeSaves;
    traits.mayOmitFrameRecord = mayOmitFrameRecord;
    return traits;
}

static RegisterMask calleeSavedRegistersOfFrameFormTest(TestProcedure& test)
{
    return registerFileFor(test.procedure().traits()).calleeSaved;
}

static unsigned numberOfCalleeSavedGPRsInFrameFormTests()
{
    return static_cast<unsigned>(std::popcount(registerFileFor(ProcedureTraits()).calleeSavedIn(Bank::GP)));
}

// Writes every register that no callee saves.
static PatchOperation* appendCallOfFrameFormTest(TestProcedure& test)
{
    RegisterFile file = registerFileFor(test.procedure().traits());
    RegisterMask clobbered = (file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP)) & ~file.calleeSaved;
    PatchOperation* call = test.out().patch(Void);
    call->clobberLate(clobbered);
    call->setGenerator([clobbered](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(params.frameIsSetUp());
        GPRReg poisoned = InvalidGPRReg;
        forEachRegister(clobbered, [&](Reg reg) {
            if (!reg.isGPR())
                return;
            poisoned = reg.gpr();
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(poison)), poisoned);
        });
        AOT_TEST_CHECK(poisoned != InvalidGPRReg);
        forEachRegister(clobbered, [&](Reg reg) {
            if (reg.isFPR())
                jit.move64ToDouble(poisoned, reg.fpr());
        });
    });
    return call;
}

// Zero where the stack pointer is aligned.
static Operation* appendAlignmentProbeOfFrameFormTest(Output& out)
{
    PatchOperation* probe = out.patch(Int64);
    probe->effects = Effects::none();
    probe->effects.writesLocalState = true;
    probe->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(params.frameIsSetUp());
        jit.move(CCallHelpers::stackPointerRegister, params[0].gpr());
        jit.and64(CCallHelpers::TrustedImm32(static_cast<int32_t>(stackAlignmentBytes() - 1)), params[0].gpr());
    });
    return probe;
}

struct FrameFormSetupProbe {
    uint64_t firstSlotAtCheck { 0 };
    unsigned numberOfSetups { 0 };
    MacroAssembler::Label check;
};

// Notes what the first word of the slots holds where the stack is checked: behind whatever the setup pushes, ahead of every store.
static void probeFirstSlotAtStackCheck(void* context, CCallHelpers& jit, const Frame& frame, const StackCheckSite&)
{
    FrameFormSetupProbe& probe = *static_cast<FrameFormSetupProbe*>(context);
    ++probe.numberOfSetups;
    probe.check = jit.label();
#if CPU(X86_64)
    // Every other register may hold an argument.
    constexpr GPRReg word = X86Registers::eax;
    GPRReg address = jit.scratchRegister();
    CCallHelpers::Address slot = frame.addressInFrame(frame.topOfSlots() - bytesOfFrameFormTestWord);
    jit.push(word);
    if (slot.base == CCallHelpers::stackPointerRegister)
        slot.offset += bytesOfFrameFormTestWord;
    jit.load64(slot, word);
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(&probe.firstSlotAtCheck)), address);
    jit.store64(word, CCallHelpers::Address(address));
    jit.pop(word);
#else
    UNUSED_PARAM(frame);
#endif
}

#if CPU(X86_64)
static const Operation* valueInFirstSlotOfFrameFormTest(TestProcedure& test)
{
    const Frame& frame = test.procedure().frame();
    std::span<const SpillFacts> spills = test.procedure().allocation().spills();
    for (SpillIndex spill = 0; spill < spills.size(); ++spill) {
        if (frame.offsetFromFPOf(spill) == frame.topOfSlots() - bytesOfFrameFormTestWord)
            return spills[spill].value;
    }
    return nullptr;
}

static unsigned bytesOfPushInFrameFormTest(Reg reg)
{
    return reg.gpr() >= X86Registers::r8 ? 2 : 1;
}
#endif

// f(seed, record). Notes what the callee-saved registers and the stack pointer held at the entry, defines seed + 1, seed + 2 and so on, calls, and adds them up, the first `numberReadTwice` of them twice.
// Leaves with zero where the save area holds what the registers held and the teardown has put all of it back.
static void buildFrameFormTestThatComparesRegisters(TestProcedure& test, unsigned numberReadTwice, unsigned numberReadOnce)
{
    Output& out = test.out();
    RegisterMask calleeSaved = calleeSavedRegistersOfFrameFormTest(test);
    Operation* seed = test.argumentGPR(0);
    Operation* argumentWithRecord = test.argumentGPR(1);

    // Nothing that is defined ahead of it lives across a call, so no callee-saved register has been written yet.
    PatchOperation* atEntry = out.patch(Void);
    atEntry->appendSomeRegister(argumentWithRecord);
    atEntry->numGPScratchRegisters = 1;
    atEntry->setGenerator([calleeSaved](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg record = params[0].gpr();
        GPRReg stackPointer = params.gpScratch(0);
        AOT_TEST_CHECK(params.frameIsSetUp() && !((maskOf(Reg(record)) | maskOf(Reg(stackPointer))) & calleeSaved));
        forEachRegister(calleeSaved, [&](Reg reg) {
            if (reg.isGPR())
                jit.store64(reg.gpr(), wordOfFrameFormRecord(record, reg.index()));
            else
                jit.storeDouble(reg.fpr(), wordOfFrameFormRecord(record, reg.index()));
        });
        jit.move(CCallHelpers::stackPointerRegister, stackPointer);
        jit.add64(CCallHelpers::TrustedImm32(static_cast<int32_t>(params.frame().bytesBelowFrameRecord()) + bytesFromEntryToFrameFormRecord), stackPointer);
        jit.store64(stackPointer, wordOfFrameFormRecord(record, stackPointerInFrameFormRecord));
    });

    Operation* record = out.opaque(argumentWithRecord);
    Vector<Operation*> values;
    for (unsigned index = 0; index < numberReadTwice + numberReadOnce; ++index)
        values.append(out.opaque(out.add(seed, out.constInt64(index + 1))));
    appendCallOfFrameFormTest(test);
    Operation* sum = out.int64Zero;
    for (Operation* value : values)
        sum = out.add(sum, value);
    sum = out.opaque(sum);
    for (unsigned index = 0; index < numberReadTwice; ++index)
        sum = out.add(sum, values[index]);
    out.store64(sum, out.address(test.heaps().argumentBuffer, record, sumInFrameFormRecord * sizeof(uint64_t)));

    PatchOperation* leave = out.patch(Void);
    leave->append(record, ValueRep::reg(GPRInfo::argumentGPR1));
    leave->effects.terminal = true;
    leave->setGenerator([calleeSaved](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        constexpr GPRReg expected = GPRInfo::argumentGPR0;
        constexpr GPRReg record = GPRInfo::argumentGPR1;
        constexpr GPRReg differences = GPRInfo::argumentGPR2;
        constexpr GPRReg word = GPRInfo::argumentGPR3;
        const Frame& frame = params.frame();
        AOT_TEST_CHECK(params.frameIsSetUp() && params.size() == 1 && params[0] == ValueRep::reg(record));
        auto compareWordWithRecord = [&](unsigned index) {
            jit.load64(wordOfFrameFormRecord(record, index), expected);
            jit.xor64(expected, word);
            jit.or64(word, differences);
        };

        jit.move(CCallHelpers::TrustedImm32(0), differences);
        for (const RegisterAtOffset& save : frame.calleeSaveRegisterAtOffsetList()) {
            jit.load64(frame.addressInFrame(save.offset()), word);
            compareWordWithRecord(save.reg().index());
        }
        emitFrameTeardown(jit, frame);
        forEachRegister(calleeSaved, [&](Reg reg) {
            if (reg.isGPR())
                jit.move(reg.gpr(), word);
            else
                jit.moveDoubleTo64(reg.fpr(), word);
            compareWordWithRecord(reg.index());
        });
        jit.move(CCallHelpers::stackPointerRegister, word);
        compareWordWithRecord(stackPointerInFrameFormRecord);
        jit.move(differences, GPRInfo::returnValueGPR);
        jit.ret();
    });

    compileOrFail(test);
}

static void runFrameFormTestThatComparesRegisters(TestProcedure& test, unsigned numberReadTwice, unsigned numberReadOnce)
{
    constexpr uint64_t seed = 5000;
    uint64_t expectedSum = 0;
    for (unsigned index = 0; index < numberReadTwice + numberReadOnce; ++index)
        expectedSum += (seed + index + 1) * (index < numberReadTwice ? 2 : 1);
    FrameFormRecord words { };
    AOT_FRAME_FORM_TEST_CHECK(test, !test.call<uint64_t>(seed, words.data()));
    AOT_FRAME_FORM_TEST_CHECK(test, words[sumInFrameFormRecord] == expectedSum);
}

// ---- F5

// f(a): a + 1 lives across a call and is read `numberOfReads` times behind it.
static Operation* buildFrameFormTestOfValueAcrossCall(TestProcedure& test, unsigned numberOfReads)
{
    Output& out = test.out();
    Operation* value = out.opaque(out.add(test.argumentGPR(0), out.constInt64(1)));
    appendCallOfFrameFormTest(test);
    Operation* sum = out.constInt64(5);
    for (unsigned read = 0; read < numberOfReads; ++read)
        sum = out.opaque(out.add(sum, value));
    out.ret(sum);
    compileOrFail(test);
    return value;
}

static void testMachineFrameFormValueReadOnceTakesNoNewSavedRegister()
{
    TestProcedure test(traitsOfFrameFormTest(true, false));
    Operation* value = buildFrameFormTestOfValueAcrossCall(test, 1);
    AOT_FRAME_FORM_TEST_CHECK(test, !test.procedure().allocation().usedCalleeSaves() && test.procedure().allocation().isStoredBehindDefinition(value));
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 40 }) == 46);

    TestProcedure uncharged(traitsOfFrameFormTest(false, false));
    Operation* sameValue = buildFrameFormTestOfValueAcrossCall(uncharged, 1);
    AOT_FRAME_FORM_TEST_CHECK(uncharged, uncharged.procedure().allocation().usedCalleeSaves() && !uncharged.procedure().allocation().isSpilled(sameValue));
    AOT_FRAME_FORM_TEST_CHECK(uncharged, uncharged.call<uint64_t>(uint64_t { 40 }) == 46);
}

static void testMachineFrameFormValueReadTwiceTakesSavedRegister()
{
    TestProcedure test(traitsOfFrameFormTest(true, false));
    Operation* value = buildFrameFormTestOfValueAcrossCall(test, 2);
    AOT_FRAME_FORM_TEST_CHECK(test, std::popcount(test.procedure().allocation().usedCalleeSaves()) == 1 && !test.procedure().allocation().isSpilled(value));
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 40 }) == 87);
}

// f(a, n): n turns, each of which calls and then notes a + 1. The one read stands in the loop, so in memory the value would be loaded at every turn.
static void testMachineFrameFormValueThatLoopReadsTakesSavedRegister()
{
    TestProcedure test(traitsOfFrameFormTest(true, false));
    Output& out = test.out();
    uint64_t noted = 0;
    Operation* value = out.opaque(out.add(test.argumentGPR(0), out.constInt64(1)));
    Operation* turns = test.argumentGPR(1);
    Block* head = out.newBlock();
    Block* end = out.newBlock();

    ValueFromBlock firstTurns = out.anchor(turns);
    out.jump(head);

    out.appendTo(head);
    Operation* turnsLeft = out.phi(Int64, firstTurns);
    appendCallOfFrameFormTest(test);
    PatchOperation* reader = out.patch(Void);
    reader->effects = Effects::none();
    reader->effects.writesLocalState = true;
    reader->appendSomeRegister(value);
    reader->setGenerator([place = &noted](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg address = jit.scratchRegister();
        jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
        jit.store64(params[0].gpr(), CCallHelpers::Address(address));
    });
    Operation* nextTurnsLeft = out.sub(turnsLeft, out.constInt64(1));
    out.addIncomingToPhi(turnsLeft, out.anchor(nextTurnsLeft));
    out.branch(out.notZero64(nextTurnsLeft), normalEdge(head), normalEdge(end));

    out.appendTo(end);
    out.ret(nextTurnsLeft);
    compileOrFail(test);

    AOT_FRAME_FORM_TEST_CHECK(test, !test.procedure().allocation().isSpilled(value) && test.procedure().allocation().usedCalleeSaves());
    // It is defined and read once a turn.
    AOT_FRAME_FORM_TEST_CHECK(test, !test.procedure().allocation().isSpilled(turnsLeft));
    AOT_FRAME_FORM_TEST_CHECK(test, !test.call<uint64_t>(uint64_t { 9 }, uint64_t { 4 }) && noted == 10);
}

// f(a): a + 1 lives across a call. The usual way reads it once, a rare block three times: what a rare region reads it loads for itself.
static void testMachineFrameFormReadsOfRareRegionsDoNotCount()
{
    TestProcedure test(traitsOfFrameFormTest(true, false));
    Output& out = test.out();
    Operation* seed = test.argumentGPR(0);
    Operation* value = out.opaque(out.add(seed, out.constInt64(1)));
    Block* rare = out.newBlock();
    Block* usual = out.newBlock();
    appendCallOfFrameFormTest(test);
    out.branch(out.testNonZero64(seed, out.constInt64(1)), rareEdge(rare), normalEdge(usual));
    out.appendTo(rare);
    out.ret(out.add(out.opaque(out.add(value, value)), value));
    out.appendTo(usual);
    out.ret(out.add(value, out.constInt64(5)));
    compileOrFail(test);

    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().order().of(rare).isRare() && test.procedure().allocation().isSpilled(value));
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 20 }) == 26);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 21 }) == 66);
}

// One value pays for the first register of a pair. On ARM64 the second is stored and loaded by the same two instructions, so a value that is read once takes it.
static void testMachineFrameFormSecondSavedRegisterOfPairIsFree()
{
    TestProcedure test(traitsOfFrameFormTest(true, false));
    Output& out = test.out();
    Operation* seed = test.argumentGPR(0);
    Operation* readTwice = out.opaque(out.add(seed, out.constInt64(1)));
    Operation* readOnce = out.opaque(out.add(seed, out.constInt64(2)));
    appendCallOfFrameFormTest(test);
    Operation* sum = out.opaque(out.add(readTwice, readOnce));
    out.ret(out.add(sum, readTwice));
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    AOT_FRAME_FORM_TEST_CHECK(test, !allocation.isSpilled(readTwice));
    AOT_FRAME_FORM_TEST_CHECK(test, allocation.isSpilled(readOnce) == !isARM64());
    AOT_FRAME_FORM_TEST_CHECK(test, std::popcount(allocation.usedCalleeSaves()) == (isARM64() ? 2 : 1));
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 100 }) == 101 + 102 + 101);
}

// ---- F3

// f(a, b, c, d): all four live across a call. There are fewer callee-saved registers than that on x86-64, so some are stored behind their definitions, which stand right behind the setup.
static void testMachineFrameFormArgumentsStoredBehindSetupArePushed()
{
    constexpr std::array<uint64_t, 4> arguments { 0x1111222233330001ull, 0x1111222233330020ull, 0x1111222233330300ull, 0x1111222233334000ull };
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    FrameFormSetupProbe probe;
    proc.setStackCheckGenerator(probeFirstSlotAtStackCheck, &probe);
    std::array<Operation*, 4> values;
    for (unsigned index = 0; index < values.size(); ++index)
        values[index] = test.argumentGPR(index);
    appendCallOfFrameFormTest(test);
    Operation* sum = out.int64Zero;
    for (Operation* value : values)
        sum = out.add(sum, value);
    out.ret(sum);
    compileOrFail(test);

    const Frame& frame = proc.frame();
    AOT_FRAME_FORM_TEST_CHECK(test, frame.coverage() == FrameCoverage::WholeFunction && probe.numberOfSetups == 1);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(arguments[0], arguments[1], arguments[2], arguments[3]) == arguments[0] + arguments[1] + arguments[2] + arguments[3]);
#if CPU(X86_64)
    std::span<const SpillFacts> spills = proc.allocation().spills();
    AOT_FRAME_FORM_TEST_CHECK(test, spills.size() == values.size() - numberOfCalleeSavedGPRsInFrameFormTests() && frame.hasFrameRecord());
    const Operation* first = valueInFirstSlotOfFrameFormTest(test);
    AOT_FRAME_FORM_TEST_CHECK(test, first && first->opcode() == ArgumentReg);
    for (unsigned index = 0; index < values.size(); ++index)
        AOT_FRAME_FORM_TEST_CHECK(test, values[index] != first || probe.firstSlotAtCheck == arguments[index]);

    // push rbp, mov rbp, rsp, and a push for every other word: no move of the stack pointer and no store.
    unsigned bytesOfSetup = 4;
    for (const RegisterAtOffset& save : frame.calleeSaveRegisterAtOffsetList())
        bytesOfSetup += bytesOfPushInFrameFormTest(save.reg());
    for (const SpillFacts& spill : spills)
        bytesOfSetup += bytesOfPushInFrameFormTest(spill.value->argumentReg());
    AOT_FRAME_FORM_TEST_CHECK(test, numberOfWordsBesideSavesOf(frame) == spills.size());
    AOT_FRAME_FORM_TEST_CHECK(test, MacroAssembler::differenceBetween(proc.entrypointLabel(0), probe.check) == static_cast<ptrdiff_t>(bytesOfSetup));
#endif
}

// f(a, b, c, d): returns b at once where a is odd. The other way calls with all four alive, so its head sets the frame up and saves what finds no callee-saved register.
static void testMachineFrameFormSavesOfHeadArePushed()
{
    constexpr std::array<uint64_t, 4> arguments { 0x2222333344440002ull, 0x2222333344440020ull, 0x2222333344440300ull, 0x2222333344444000ull };
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    FrameFormSetupProbe probe;
    proc.setStackCheckGenerator(probeFirstSlotAtStackCheck, &probe);
    std::array<Operation*, 4> values;
    for (unsigned index = 0; index < values.size(); ++index)
        values[index] = test.argumentGPR(index);
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();
    out.branch(out.testNonZero64(values[0], out.constInt64(1)), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    out.ret(values[1]);
    out.appendTo(calling);
    appendCallOfFrameFormTest(test);
    Operation* sum = out.int64Zero;
    for (Operation* value : values)
        sum = out.add(sum, value);
    out.ret(sum);
    compileOrFail(test);

    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(arguments[0] + 1, arguments[1], arguments[2], arguments[3]) == arguments[1] && !probe.firstSlotAtCheck);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(arguments[0], arguments[1], arguments[2], arguments[3]) == arguments[0] + arguments[1] + arguments[2] + arguments[3]);
#if CPU(X86_64)
    AOT_FRAME_FORM_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && proc.allocation().onlyHeadOfFramedPart() == calling && probe.numberOfSetups == 1);
    const Operation* first = valueInFirstSlotOfFrameFormTest(test);
    AOT_FRAME_FORM_TEST_CHECK(test, first && proc.allocation().spills()[proc.allocation().spillOf(first)].isStoredAtHeadOfFramedPart);
    for (unsigned index = 0; index < values.size(); ++index)
        AOT_FRAME_FORM_TEST_CHECK(test, values[index] != first || probe.firstSlotAtCheck == arguments[index]);
#endif
}

// f(a, x): the number lives across a call, and x86-64 has no floating-point register that callees save. There is no push of such a register.
static void testMachineFrameFormNumberStoredBehindSetupIsNotPushed()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* number = test.argumentFPR(0);
    Operation* integer = test.argumentGPR(0);
    appendCallOfFrameFormTest(test);
    out.ret(out.add(out.bitCast(number, Int64), integer));
    compileOrFail(test);
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().allocation().isSpilled(number) == isX86_64());
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 3 }, 2.5) == std::bit_cast<uint64_t>(2.5) + 3);
}

// The outgoing words lie where the stack pointer stands, so the saves cannot.
static void testMachineFrameFormOutgoingWordsKeepSavesOnTop()
{
    constexpr uint64_t seedOfRun = 600;
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    proc.requestCallArgAreaSizeInBytes(stackAlignmentBytes());
    unsigned count = numberOfCalleeSavedGPRsInFrameFormTests() + 1;
    Operation* seed = test.argumentGPR(0);
    Vector<Operation*> values;
    for (unsigned index = 0; index < count; ++index)
        values.append(out.opaque(out.add(seed, out.constInt64(index + 1))));
    PatchOperation* call = appendCallOfFrameFormTest(test);
    call->append(seed, ValueRep::stackArgument(0));
    Operation* sum = out.int64Zero;
    for (Operation* value : values)
        sum = out.add(sum, value);
    out.ret(sum);
    compileOrFail(test);

    const Frame& frame = proc.frame();
    AOT_FRAME_FORM_TEST_CHECK(test, frame.calleeSavesMask() && proc.slots().bytesInAll() && !frame.savesLieAtBottom() && frame.hasFrameRecord());
    uint64_t expected = 0;
    for (unsigned index = 0; index < count; ++index)
        expected += seedOfRun + index + 1;
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(seedOfRun) == expected);
}

static void testMachineFrameFormSavedRegistersAtBottomComeBack()
{
    for (unsigned numberBeyondRegisters : { 1u, 2u, 3u }) {
        TestProcedure test;
        // The record takes a callee-saved register too.
        unsigned count = numberOfCalleeSavedGPRsInFrameFormTests() - 1 + numberBeyondRegisters;
        buildFrameFormTestThatComparesRegisters(test, count, 0);
        const Frame& frame = test.procedure().frame();
        AOT_FRAME_FORM_TEST_CHECK(test, frame.calleeSavesMask() && test.procedure().slots().bytesInAll() && frame.hasFrameRecord());
        AOT_FRAME_FORM_TEST_CHECK(test, frame.savesLieAtBottom() == isX86_64());
        AOT_FRAME_FORM_TEST_CHECK(test, test.printedFrame().contains("callee saves lie at the bottom"_s) == isX86_64());
        AOT_FRAME_FORM_TEST_CHECK(test, frame.calleeSaveRegisterAtOffsetList().at(0).offset() == offsetOfFirstSaveOf(frame));
        if (frame.savesLieAtBottom())
            AOT_FRAME_FORM_TEST_CHECK(test, !frame.topOfSlots() && offsetOfFirstSaveOf(frame) == -static_cast<int32_t>(frame.bytesBelowFrameRecord()));
        runFrameFormTestThatComparesRegisters(test, count, 0);
    }
}

// ---- F4

// f(): calls, and nothing lives across the call. The frame is there for the return address of the callee alone.
static void testMachineFrameFormOfFunctionThatOnlyCallsHasNoRecord()
{
    for (bool mayOmitFrameRecord : { true, false }) {
        TestProcedure test(traitsOfFrameFormTest(false, mayOmitFrameRecord));
        Output& out = test.out();
        appendCallOfFrameFormTest(test);
        out.ret(out.add(appendAlignmentProbeOfFrameFormTest(out), out.constInt64(7)));
        compileOrFail(test);

        const Frame& frame = test.procedure().frame();
        AOT_FRAME_FORM_TEST_CHECK(test, frame.hasFrame() && !frame.numberOfCalleeSaves() && !frame.bytesBelowFrameRecord());
        AOT_FRAME_FORM_TEST_CHECK(test, frame.hasFrameRecord() == !(mayOmitFrameRecord && isX86_64()));
        AOT_FRAME_FORM_TEST_CHECK(test, test.printedFrame().contains("no frame record"_s) == !frame.hasFrameRecord());
        AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>() == 7);
    }
}

static void testMachineFrameFormSavedRegistersWithoutRecordComeBack()
{
    // The record of the test takes the first callee-saved register.
    for (unsigned count = 0; count < numberOfCalleeSavedGPRsInFrameFormTests(); ++count) {
        TestProcedure test(traitsOfFrameFormTest(false, true));
        buildFrameFormTestThatComparesRegisters(test, count, 0);
        const Frame& frame = test.procedure().frame();
        AOT_FRAME_FORM_TEST_CHECK(test, frame.numberOfCalleeSaves() == count + 1 && !test.procedure().slots().bytesInAll());
        AOT_FRAME_FORM_TEST_CHECK(test, frame.hasFrameRecord() == !isX86_64());
        if (!frame.hasFrameRecord()) {
            // The last of the run lies at the place of the record, and an odd number of words below the return address keeps the stack pointer aligned.
            AOT_FRAME_FORM_TEST_CHECK(test, frame.calleeSaveRegisterAtOffsetList().at(count).offset() == 0);
            AOT_FRAME_FORM_TEST_CHECK(test, frame.bytesBelowFrameRecord() == (count + 1) / 2 * stackAlignmentBytes());
        }
        runFrameFormTestThatComparesRegisters(test, count, 0);
    }
}

// f(a): a + 1 is read once behind a call, so it lies in memory, and the frame holds nothing else: the word lies where the stack pointer stands.
static void testMachineFrameFormOneWordWithoutRecordLiesAtStackPointer()
{
    TestProcedure test(traitsOfFrameFormTest(true, true));
    Operation* value = buildFrameFormTestOfValueAcrossCall(test, 1);
    const Frame& frame = test.procedure().frame();
    const Allocation& allocation = test.procedure().allocation();
    AOT_FRAME_FORM_TEST_CHECK(test, allocation.isSpilled(value) && !frame.numberOfCalleeSaves());
    AOT_FRAME_FORM_TEST_CHECK(test, frame.hasFrameRecord() == !isX86_64());
    if (!frame.hasFrameRecord()) {
        AOT_FRAME_FORM_TEST_CHECK(test, !frame.bytesBelowFrameRecord() && !frame.offsetFromFPOf(allocation.spillOf(value)));
        CCallHelpers::Address address = frame.addressInFrame(frame.offsetFromFPOf(allocation.spillOf(value)));
        AOT_FRAME_FORM_TEST_CHECK(test, address.base == CCallHelpers::stackPointerRegister && !address.offset);
    }
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 70 }) == 76);
}

// Two words would be reached by addresses that are a byte longer than those off the frame pointer.
static void testMachineFrameFormWithTwoWordsKeepsItsRecord()
{
    TestProcedure test(traitsOfFrameFormTest(true, true));
    Output& out = test.out();
    Operation* seed = test.argumentGPR(0);
    Operation* first = out.opaque(out.add(seed, out.constInt64(1)));
    Operation* second = out.opaque(out.add(seed, out.constInt64(2)));
    appendCallOfFrameFormTest(test);
    out.ret(out.add(first, second));
    compileOrFail(test);
    if (isX86_64())
        AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().allocation().isSpilled(first) && test.procedure().allocation().isSpilled(second));
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().frame().hasFrameRecord());
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 10 }) == 23);
}

static void buildFrameFormTestWithPatchThatReadsFramePointer(TestProcedure& test)
{
    Output& out = test.out();
    appendCallOfFrameFormTest(test);
    // Zero where the frame pointer register points at the place of the record.
    PatchOperation* reader = out.patch(Int64);
    reader->effects = Effects::none();
    reader->effects.writesLocalState = true;
    reader->setNeedsFrameRecord(true);
    reader->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(params.frameIsSetUp() && params.frame().hasFrameRecord());
        jit.move(CCallHelpers::stackPointerRegister, params[0].gpr());
        jit.add64(CCallHelpers::TrustedImm32(static_cast<int32_t>(params.frame().bytesBelowFrameRecord())), params[0].gpr());
        jit.xor64(CCallHelpers::framePointerRegister, params[0].gpr());
    });
    out.ret(reader);
}

static void testMachineFrameFormPatchThatNeedsRecordKeepsIt()
{
    TestProcedure test(traitsOfFrameFormTest(false, true));
    buildFrameFormTestWithPatchThatReadsFramePointer(test);
    compileOrFail(test);
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().allocation().holdsPatchThatCountsOnFrameRecord() && test.procedure().frame().hasFrameRecord());
    AOT_FRAME_FORM_TEST_CHECK(test, !test.call<uint64_t>());
}

// f(a): a + 1 lies in memory where a Patch takes it wherever it lies. The generator reads the word off the frame pointer, as ValueRep::emitRestore() and the thunks for arguments do.
static void testMachineFrameFormGeneratorToldOfPlaceInFrameKeepsRecord()
{
    TestProcedure test(traitsOfFrameFormTest(true, true));
    Output& out = test.out();
    Operation* value = out.opaque(out.add(test.argumentGPR(0), out.constInt64(1)));
    appendCallOfFrameFormTest(test);
    PatchOperation* reader = out.patch(Int64);
    reader->effects = Effects::none();
    reader->effects.writesLocalState = true;
    reader->append(value, ValueRep::ColdAny);
    reader->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(params.frameIsSetUp() && params.size() == 2);
        params[1].emitRestore(jit, params[0].gpr());
    });
    out.ret(reader);
    compileOrFail(test);
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().allocation().isSpilled(value) && test.procedure().allocation().holdsPatchThatCountsOnFrameRecord());
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().frame().hasFrameRecord());
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 30 }) == 31);
}

static void testMachineFrameFormUseOfFramePointerKeepsRecord()
{
    TestProcedure test(traitsOfFrameFormTest(false, true));
    Output& out = test.out();
    appendCallOfFrameFormTest(test);
    // The word at the frame pointer is the frame pointer of the caller, which lies above.
    Operation* framePointer = out.opaque(out.framePointer());
    out.ret(out.sub(framePointer, out.opaque(out.framePointer())));
    compileOrFail(test);
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().namesFramePointer() && test.procedure().frame().hasFrameRecord());
    AOT_FRAME_FORM_TEST_CHECK(test, !test.call<uint64_t>());
}

static void testMachineFrameFormHandlersKeepRecord()
{
    ProcedureTraits traits = traitsOfFrameFormTest(false, true);
    traits.hasHandlers = true;
    TestProcedure test(traits);
    Output& out = test.out();
    appendCallOfFrameFormTest(test);
    out.ret(out.constInt64(3));
    compileOrFail(test);
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().frame().coverage() == FrameCoverage::WholeFunction && test.procedure().frame().hasFrameRecord());
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>() == 3);
}

static void testMachineFrameFormCheckerRefusesFrameWithoutRecordThatPatchCountsOn()
{
    TestProcedure test(traitsOfFrameFormTest(false, true));
    buildFrameFormTestWithPatchThatReadsFramePointer(test);
    prepareOrFail(test);
    Procedure& proc = test.procedure();
    AOT_FRAME_FORM_TEST_CHECK(test, proc.frame().hasFrameRecord() && !proc.frame().numberOfCalleeSaves() && checkFrame(proc));
    proc.frameForTesting().setHasFrameRecordForTesting(false);
    AOT_FRAME_FORM_TEST_CHECK(test, !checkFrame(proc) && proc.failure() && proc.failure()->kind == FailureKind::NeedsFrameWhereNoneIsSetUp);
}

// Every row at once, with more and fewer values than there are registers: whatever form the frame takes, the caller gets its registers and its stack pointer back.
static void testMachineFrameFormsGiveEverythingBack()
{
    for (bool chargesForCalleeSaves : { false, true }) {
        for (bool mayOmitFrameRecord : { false, true }) {
            for (unsigned numberReadTwice = 0; numberReadTwice <= numberOfCalleeSavedGPRsInFrameFormTests() + 1; ++numberReadTwice) {
                for (unsigned numberReadOnce = 0; numberReadOnce < 4; ++numberReadOnce) {
                    TestProcedure test(traitsOfFrameFormTest(chargesForCalleeSaves, mayOmitFrameRecord));
                    buildFrameFormTestThatComparesRegisters(test, numberReadTwice, numberReadOnce);
                    runFrameFormTestThatComparesRegisters(test, numberReadTwice, numberReadOnce);
                }
            }
        }
    }
}

#undef AOT_FRAME_FORM_TEST_CHECK

} // namespace Machine

std::span<const NativeTest> machineFrameFormTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/frame-forms/value-read-once-behind-a-call-takes-no-new-saved-register"_s, Machine::testMachineFrameFormValueReadOnceTakesNoNewSavedRegister },
        { "machine/frame-forms/value-read-twice-behind-a-call-takes-a-saved-register"_s, Machine::testMachineFrameFormValueReadTwiceTakesSavedRegister },
        { "machine/frame-forms/value-that-a-loop-reads-takes-a-saved-register"_s, Machine::testMachineFrameFormValueThatLoopReadsTakesSavedRegister },
        { "machine/frame-forms/reads-of-rare-regions-do-not-count"_s, Machine::testMachineFrameFormReadsOfRareRegionsDoNotCount },
        { "machine/frame-forms/second-saved-register-of-a-pair-is-free"_s, Machine::testMachineFrameFormSecondSavedRegisterOfPairIsFree },
        { "machine/frame-forms/arguments-stored-behind-the-setup-are-pushed"_s, Machine::testMachineFrameFormArgumentsStoredBehindSetupArePushed },
        { "machine/frame-forms/saves-of-a-head-are-pushed"_s, Machine::testMachineFrameFormSavesOfHeadArePushed },
        { "machine/frame-forms/number-stored-behind-the-setup-is-not-pushed"_s, Machine::testMachineFrameFormNumberStoredBehindSetupIsNotPushed },
        { "machine/frame-forms/outgoing-words-keep-the-saves-on-top"_s, Machine::testMachineFrameFormOutgoingWordsKeepSavesOnTop },
        { "machine/frame-forms/saved-registers-at-the-bottom-come-back"_s, Machine::testMachineFrameFormSavedRegistersAtBottomComeBack },
        { "machine/frame-forms/function-that-only-calls-has-no-record"_s, Machine::testMachineFrameFormOfFunctionThatOnlyCallsHasNoRecord },
        { "machine/frame-forms/saved-registers-without-a-record-come-back"_s, Machine::testMachineFrameFormSavedRegistersWithoutRecordComeBack },
        { "machine/frame-forms/one-word-without-a-record-lies-at-the-stack-pointer"_s, Machine::testMachineFrameFormOneWordWithoutRecordLiesAtStackPointer },
        { "machine/frame-forms/two-words-keep-the-record"_s, Machine::testMachineFrameFormWithTwoWordsKeepsItsRecord },
        { "machine/frame-forms/patch-that-needs-the-record-keeps-it"_s, Machine::testMachineFrameFormPatchThatNeedsRecordKeepsIt },
        { "machine/frame-forms/generator-told-of-a-place-in-the-frame-keeps-the-record"_s, Machine::testMachineFrameFormGeneratorToldOfPlaceInFrameKeepsRecord },
        { "machine/frame-forms/use-of-the-frame-pointer-keeps-the-record"_s, Machine::testMachineFrameFormUseOfFramePointerKeepsRecord },
        { "machine/frame-forms/handlers-keep-the-record"_s, Machine::testMachineFrameFormHandlersKeepRecord },
        { "machine/frame-forms/checker-refuses-a-frame-without-a-record-that-a-patch-counts-on"_s, Machine::testMachineFrameFormCheckerRefusesFrameWithoutRecordThatPatchCountsOn },
        { "machine/frame-forms/every-form-gives-everything-back"_s, Machine::testMachineFrameFormsGiveEverythingBack },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
