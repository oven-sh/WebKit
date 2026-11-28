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
static constexpr int32_t bytesFromEntryToFrameFormRecord = isX86_64() ? bytesOfFrameFormTestWord : 2 * bytesOfFrameFormTestWord;

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

static void probeFirstSlotAtStackCheck(void* context, CCallHelpers& jit, const Frame& frame, const StackCheckSite&)
{
    FrameFormSetupProbe& probe = *static_cast<FrameFormSetupProbe*>(context);
    ++probe.numberOfSetups;
    probe.check = jit.label();
#if CPU(X86_64)
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

static void buildFrameFormTestThatComparesRegisters(TestProcedure& test, unsigned numberReadTwice, unsigned numberReadOnce)
{
    Output& out = test.out();
    RegisterMask calleeSaved = calleeSavedRegistersOfFrameFormTest(test);
    Operation* seed = test.argumentGPR(0);
    Operation* argumentWithRecord = test.argumentGPR(1);

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
    AOT_FRAME_FORM_TEST_CHECK(test, !test.procedure().allocation().isSpilled(turnsLeft));
    AOT_FRAME_FORM_TEST_CHECK(test, !test.call<uint64_t>(uint64_t { 9 }, uint64_t { 4 }) && noted == 10);
}

static void testMachineFrameFormReadsOfRareRegionsDoNotCount()
{
    TestProcedure test(traitsOfFrameFormTest(true, false));
    Output& out = test.out();
    Operation* seed = test.argumentGPR(0);
    Operation* secondOfPair = isARM64() ? test.argumentGPR(1) : nullptr;
    Operation* value = out.opaque(out.add(seed, out.constInt64(1)));
    Block* rare = out.newBlock();
    Block* usual = out.newBlock();
    appendCallOfFrameFormTest(test);
    out.branch(out.testNonZero64(seed, out.constInt64(1)), rareEdge(rare), normalEdge(usual));
    out.appendTo(rare);
    out.ret(out.add(out.opaque(out.add(value, value)), value));
    out.appendTo(usual);
    Operation* result = out.add(value, out.constInt64(5));
    out.ret(secondOfPair ? out.add(result, secondOfPair) : result);
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    AOT_FRAME_FORM_TEST_CHECK(test, std::popcount(allocation.usedCalleeSaves()) == (isARM64() ? 2 : 1));
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().order().of(rare).isRare() && allocation.isSpilled(value));
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 20 }, uint64_t { 0 }) == 26);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 21 }, uint64_t { 0 }) == 66);
}

enum class LastReadOfArgument : uint8_t { InFixedRegister, InSomeRegister, AsColdOperand, ByPhiAtJump, ByReturn };

static Operation* buildFrameFormTestOfArgumentReadBehindCalls(TestProcedure& test, std::span<uint64_t> noted, LastReadOfArgument lastRead)
{
    Output& out = test.out();
    Operation* argument = test.argumentGPR(0);
    Operation* chooses = lastRead == LastReadOfArgument::ByPhiAtJump ? test.argumentGPR(1) : nullptr;
    for (unsigned read = 0; read < noted.size(); ++read) {
        appendCallOfFrameFormTest(test);
        PatchOperation* reader = out.patch(Void);
        reader->effects = Effects::none();
        reader->effects.writesLocalState = true;
        bool isLast = read + 1 == noted.size();
        if (isLast && lastRead == LastReadOfArgument::AsColdOperand) {
            reader->append(argument, ValueRep::ColdAny);
            reader->setGenerator([](CCallHelpers&, const PatchParameters&) { });
            continue;
        }
        if (isLast && lastRead == LastReadOfArgument::InSomeRegister)
            reader->appendSomeRegister(argument);
        else
            reader->append(argument, ValueRep::reg(GPRInfo::argumentGPR2));
        reader->setGenerator([place = &noted[read]](CCallHelpers& jit, const PatchParameters& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg address = jit.scratchRegister();
            jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
            jit.store64(params[0].gpr(), CCallHelpers::Address(address));
        });
    }
    if (lastRead == LastReadOfArgument::ByReturn || lastRead == LastReadOfArgument::ByPhiAtJump)
        appendCallOfFrameFormTest(test);
    if (lastRead == LastReadOfArgument::ByReturn)
        out.ret(argument);
    else if (lastRead == LastReadOfArgument::ByPhiAtJump) {
        Block* hands = out.newBlock();
        Block* other = out.newBlock();
        Block* join = out.newBlock();
        out.branch(out.testNonZero64(chooses, out.constInt64(1)), normalEdge(hands), normalEdge(other));
        out.appendTo(hands);
        ValueFromBlock handed = out.anchor(argument);
        out.jump(join);
        out.appendTo(other);
        ValueFromBlock constant = out.anchor(out.constInt64(7));
        out.jump(join);
        out.appendTo(join);
        Operation* either = out.phi(Int64, handed);
        out.addIncomingToPhi(either, constant);
        out.ret(either);
    } else
        out.ret(out.constInt64(7));
    compileOrFail(test);
    return argument;
}

static constexpr uint64_t wordOfArgumentReadBehindCalls = 0x1111222233334444ull;

static bool holdsWordOfArgumentReadBehindCalls(std::span<const uint64_t> noted, size_t number)
{
    for (size_t index = 0; index < number; ++index) {
        if (noted[index] != wordOfArgumentReadBehindCalls)
            return false;
    }
    return true;
}

static void checkThatArgumentReadBehindCallsTakesNoNewSavedRegister(LastReadOfArgument lastRead)
{
    constexpr uint64_t word = wordOfArgumentReadBehindCalls;
    bool readsOnceMoreBehindPatches = lastRead == LastReadOfArgument::ByPhiAtJump || lastRead == LastReadOfArgument::ByReturn;
    std::array<uint64_t, 2> noted { };
    TestProcedure test(traitsOfFrameFormTest(true, false));
    Operation* argument = buildFrameFormTestOfArgumentReadBehindCalls(test, std::span(noted).first(readsOnceMoreBehindPatches ? 1 : 2), lastRead);
    AOT_FRAME_FORM_TEST_CHECK(test, !test.procedure().allocation().usedCalleeSaves() && test.procedure().allocation().isSpilled(argument));
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(word, uint64_t { 1 }) == (readsOnceMoreBehindPatches ? word : 7));
    AOT_FRAME_FORM_TEST_CHECK(test, holdsWordOfArgumentReadBehindCalls(noted, lastRead == LastReadOfArgument::InFixedRegister ? 2 : 1));
    if (lastRead == LastReadOfArgument::ByPhiAtJump)
        AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(word, uint64_t { 0 }) == 7);
}

static void testMachineFrameFormArgumentReadIntoFixedRegistersTakesNoNewSavedRegister()
{
    checkThatArgumentReadBehindCallsTakesNoNewSavedRegister(LastReadOfArgument::InFixedRegister);
}

static void testMachineFrameFormArgumentReadAsColdOperandTakesNoNewSavedRegister()
{
    checkThatArgumentReadBehindCallsTakesNoNewSavedRegister(LastReadOfArgument::AsColdOperand);
}

static void testMachineFrameFormArgumentHandedToPhiAtJumpTakesNoNewSavedRegister()
{
    checkThatArgumentReadBehindCallsTakesNoNewSavedRegister(LastReadOfArgument::ByPhiAtJump);
}

static void testMachineFrameFormArgumentThatIsReturnedTakesNoNewSavedRegister()
{
    checkThatArgumentReadBehindCallsTakesNoNewSavedRegister(LastReadOfArgument::ByReturn);
}

static void testMachineFrameFormArgumentThatSlotServesLessCheaplyTakesSavedRegister()
{
    constexpr uint64_t word = wordOfArgumentReadBehindCalls;
    auto holdsWordIn = holdsWordOfArgumentReadBehindCalls;

    std::array<uint64_t, 2> withOtherRead { };
    TestProcedure other(traitsOfFrameFormTest(true, false));
    Operation* readInSomeRegister = buildFrameFormTestOfArgumentReadBehindCalls(other, withOtherRead, LastReadOfArgument::InSomeRegister);
    AOT_FRAME_FORM_TEST_CHECK(other, std::popcount(other.procedure().allocation().usedCalleeSaves()) == 1 && !other.procedure().allocation().isSpilled(readInSomeRegister));
    AOT_FRAME_FORM_TEST_CHECK(other, other.call<uint64_t>(word) == 7 && holdsWordIn(withOtherRead, 2));

    std::array<uint64_t, 5> many { };
    TestProcedure often(traitsOfFrameFormTest(true, false));
    Operation* readOften = buildFrameFormTestOfArgumentReadBehindCalls(often, many, LastReadOfArgument::InFixedRegister);
    AOT_FRAME_FORM_TEST_CHECK(often, std::popcount(often.procedure().allocation().usedCalleeSaves()) == 1 && !often.procedure().allocation().isSpilled(readOften));
    AOT_FRAME_FORM_TEST_CHECK(often, often.call<uint64_t>(word) == 7 && holdsWordIn(many, 5));
}

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

    unsigned bytesOfSetup = 4;
    for (const RegisterAtOffset& save : frame.calleeSaveRegisterAtOffsetList())
        bytesOfSetup += bytesOfPushInFrameFormTest(save.reg());
    for (const SpillFacts& spill : spills)
        bytesOfSetup += bytesOfPushInFrameFormTest(spill.value->argumentReg());
    AOT_FRAME_FORM_TEST_CHECK(test, numberOfWordsBesideSavesOf(frame) == spills.size());
    AOT_FRAME_FORM_TEST_CHECK(test, MacroAssembler::differenceBetween(proc.entrypointLabel(0), probe.check) == static_cast<ptrdiff_t>(bytesOfSetup));
#endif
}

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

static void testMachineFrameFormWhatCallAtHeadTakesStaysWhereItLies()
{
    uint64_t noted = 0;
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    RegisterFile file = registerFileFor(proc.traits());
    Operation* taken = test.argumentGPR(0);
    Operation* tested = test.argumentGPR(1);
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();
    out.branch(out.testNonZero64(tested, out.constInt64(1)), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    out.ret(tested);
    out.appendTo(calling);
    PatchOperation* call = out.patch(Void);
    call->append(taken, ValueRep::reg(GPRInfo::argumentGPR0));
    call->clobberLate((file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP)) & ~file.calleeSaved);
    call->setGenerator([place = &noted](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        AOT_TEST_CHECK(params.frameIsSetUp() && params.size() == 1 && params[0] == ValueRep::reg(GPRInfo::argumentGPR0));
        GPRReg address = jit.scratchRegister();
        jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
        jit.store64(GPRInfo::argumentGPR0, CCallHelpers::Address(address));
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(poison)), GPRInfo::argumentGPR0);
    });
    out.ret(out.add(taken, tested));
    compileOrFail(test);

    const Allocation& allocation = proc.allocation();
    AOT_FRAME_FORM_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.onlyHeadOfFramedPart() == calling);
    unsigned numberOfMoves = 0;
    for (const Step& step : allocation.steps())
        numberOfMoves += step.kind == StepKind::Move && step.operation == taken;
    AOT_FRAME_FORM_TEST_CHECK(test, numberOfMoves == 1 && !allocation.isSpilled(taken));
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 40 }, uint64_t { 3 }) == 3 && !noted);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 40 }, uint64_t { 2 }) == 42 && noted == 40);
}

static void testMachineFrameFormCalleeSavesAtHeadGoToWhatIsReadMost()
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    const unsigned numberKept = numberOfCalleeSavedGPRsInFrameFormTests() + 1;
    if (numberKept + 1 > GPRInfo::numberOfArgumentRegisters)
        return;
    Operation* tested = test.argumentGPR(0);
    Vector<Operation*> kept;
    for (unsigned index = 0; index < numberKept; ++index)
        kept.append(test.argumentGPR(index + 1));
    unsigned readLeast = 0;
    for (unsigned index = 1; index < numberKept; ++index) {
        if (kept[index]->argumentReg().index() < kept[readLeast]->argumentReg().index())
            readLeast = index;
    }
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();
    out.branch(out.testNonZero64(tested, out.constInt64(1)), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    out.ret(tested);
    out.appendTo(calling);
    appendCallOfFrameFormTest(test);
    Operation* mixed = kept[readLeast];
    for (unsigned index = 0; index < numberKept; ++index) {
        if (index != readLeast)
            mixed = out.add(out.bitXor(out.add(mixed, kept[index]), kept[index]), kept[index]);
    }
    out.ret(mixed);
    compileOrFail(test);

    const Allocation& allocation = proc.allocation();
    AOT_FRAME_FORM_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.onlyHeadOfFramedPart() == calling);
    for (unsigned index = 0; index < numberKept; ++index)
        AOT_FRAME_FORM_TEST_CHECK(test, allocation.isSpilled(kept[index]) == (index == readLeast));

    std::array<uint64_t, 8> passed { 2, 50, 700, 9000, 11000, 130000, 1700000, 19000000 };
    uint64_t expected = passed[readLeast + 1];
    for (unsigned index = 0; index < numberKept; ++index) {
        if (index != readLeast)
            expected = ((expected + passed[index + 1]) ^ passed[index + 1]) + passed[index + 1];
    }
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(passed[0], passed[1], passed[2], passed[3], passed[4], passed[5], passed[6], passed[7]) == expected);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 3 }, passed[1], passed[2], passed[3], passed[4], passed[5], passed[6], passed[7]) == 3);
}

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
    for (unsigned count = 0; count < numberOfCalleeSavedGPRsInFrameFormTests(); ++count) {
        TestProcedure test(traitsOfFrameFormTest(false, true));
        buildFrameFormTestThatComparesRegisters(test, count, 0);
        const Frame& frame = test.procedure().frame();
        AOT_FRAME_FORM_TEST_CHECK(test, frame.numberOfCalleeSaves() == count + 1 && !test.procedure().slots().bytesInAll());
        AOT_FRAME_FORM_TEST_CHECK(test, frame.hasFrameRecord() == !isX86_64());
        if (!frame.hasFrameRecord()) {
            AOT_FRAME_FORM_TEST_CHECK(test, frame.calleeSaveRegisterAtOffsetList().at(count).offset() == 0);
            AOT_FRAME_FORM_TEST_CHECK(test, frame.bytesBelowFrameRecord() == (count + 1) / 2 * stackAlignmentBytes());
        }
        runFrameFormTestThatComparesRegisters(test, count, 0);
    }
}

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

static void testMachineFrameFormWithTwoWordsHasNoRecord()
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
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().frame().hasFrameRecord() == !isX86_64());
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 10 }) == 23);
}

static void testMachineFrameFormWithManyWordsHasNoRecord()
{
    constexpr unsigned numberOfWords = 24;
    TestProcedure test(traitsOfFrameFormTest(false, true));
    Output& out = test.out();
    Operation* seed = test.argumentGPR(0);
    Operation* words[numberOfWords];
    for (unsigned index = 0; index < numberOfWords; ++index)
        words[index] = out.opaque(out.add(seed, out.constInt64(int64_t { 1 } << index)));
    appendCallOfFrameFormTest(test);
    Operation* sum = words[0];
    for (unsigned index = 1; index < numberOfWords; index += 2)
        sum = out.add(sum, words[index]);
    sum = out.opaque(sum);
    appendCallOfFrameFormTest(test);
    for (unsigned index = 2; index < numberOfWords; index += 2)
        sum = out.add(sum, words[index]);
    out.ret(sum);
    compileOrFail(test);
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().frame().hasFrameRecord() == !isX86_64());
    if (isX86_64())
        AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().frame().bytesBelowFrameRecord() > 128);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 3 }) == numberOfWords * 3 + (uint64_t { 1 } << numberOfWords) - 1);
}

static void appendReaderOfFrameFormTest(TestProcedure& test, Operation* value)
{
    PatchOperation* reader = test.out().patch(Void);
    reader->effects = Effects::none();
    reader->effects.writesLocalState = true;
    reader->setRunsWithoutFrame(true);
    reader->appendSomeRegister(value);
    reader->setGenerator([](CCallHelpers&, const PatchParameters&) { });
}

static void testMachineFrameFormReadInLaterArmBehindJoinIsOutOfReach()
{
    TestProcedure test(traitsOfFrameFormTest(true, true));
    Output& out = test.out();
    Operation* choice = test.argumentGPR(1);
    Operation* value = out.opaque(out.add(test.argumentGPR(0), out.constInt64(1)));
    Block* calling = out.newBlock();
    Block* other = out.newBlock();
    Block* left = out.newBlock();
    Block* right = out.newBlock();
    Block* join = out.newBlock();
    Block* end = out.newBlock();
    out.branch(out.testNonZero64(choice, out.constInt64(1)), normalEdge(calling), normalEdge(other));

    out.appendTo(calling);
    appendReaderOfFrameFormTest(test, value);
    appendCallOfFrameFormTest(test);
    out.jump(end);

    out.appendTo(other);
    out.branch(out.testNonZero64(choice, out.constInt64(2)), normalEdge(left), normalEdge(right));
    out.appendTo(left);
    appendReaderOfFrameFormTest(test, choice);
    out.jump(join);
    out.appendTo(right);
    out.jump(join);
    out.appendTo(join);
    appendReaderOfFrameFormTest(test, value);
    out.jump(end);

    out.appendTo(end);
    out.ret(out.constInt64(5));
    compileOrFail(test);
    const Order& order = test.procedure().order();
    AOT_FRAME_FORM_TEST_CHECK(test, order.headOf(calling) < order.headOf(join) && order.headOf(join) < order.headOf(end));
    AOT_FRAME_FORM_TEST_CHECK(test, !test.procedure().allocation().isSpilled(value) && !test.procedure().allocation().usedCalleeSaves());
    for (uint64_t way = 0; way < 4; ++way)
        AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 7 }, way) == 5);
}

static void testMachineFrameFormReadInLoopThatRareBlockBehindItEntersIsWithinReach()
{
    TestProcedure test(traitsOfFrameFormTest(true, true));
    Output& out = test.out();
    uint64_t noted = 0;
    Operation* base = test.argumentGPR(0);
    Operation* choice = test.argumentGPR(1);
    Operation* turns = test.argumentGPR(2);
    Block* outerHead = out.newBlock();
    Block* body = out.newBlock();
    Block* deciding = out.newBlock();
    Block* entering = out.newBlock();
    Block* decidingRarely = out.newBlock();
    Block* enteringRarely = out.newBlock();
    Block* head = out.newBlock();
    Block* behind = out.newBlock();
    Block* rareBehind = out.newBlock();
    Block* rareJoin = out.newBlock();
    Block* latch = out.newBlock();
    Block* end = out.newBlock();
    ValueFromBlock firstOuterTurns = out.anchor(out.constInt64(2));
    out.jump(outerHead);

    out.appendTo(outerHead);
    Operation* outerTurnsLeft = out.phi(Int64, firstOuterTurns);
    out.branch(out.notZero64(outerTurnsLeft), normalEdge(body), normalEdge(end));

    out.appendTo(body);
    Operation* value = out.opaque(out.add(base, outerTurnsLeft));
    out.branch(out.testNonZero64(choice, out.constInt64(1)), normalEdge(deciding), normalEdge(entering));

    out.appendTo(deciding);
    appendReaderOfFrameFormTest(test, choice);
    out.jump(rareEdge(decidingRarely));

    out.appendTo(entering);
    ValueFromBlock turnsOfUsualWay = out.anchor(turns);
    out.jump(head);

    out.appendTo(decidingRarely);
    out.branch(out.testNonZero64(choice, out.constInt64(4)), normalEdge(rareJoin), normalEdge(enteringRarely));

    out.appendTo(enteringRarely);
    appendReaderOfFrameFormTest(test, choice);
    ValueFromBlock turnsOfRareWay = out.anchor(turns);
    out.jump(head);

    out.appendTo(head);
    Operation* turnsLeft = out.phi(Int64, turnsOfUsualWay, turnsOfRareWay);
    PatchOperation* reader = out.patch(Void);
    reader->effects = Effects::none();
    reader->effects.writesLocalState = true;
    reader->setRunsWithoutFrame(true);
    reader->appendSomeRegister(value);
    reader->setGenerator([place = &noted](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg address = jit.scratchRegister();
        jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
        jit.store64(params[0].gpr(), CCallHelpers::Address(address));
    });
    Operation* nextTurnsLeft = out.sub(turnsLeft, out.constInt64(1));
    out.addIncomingToPhi(turnsLeft, out.anchor(nextTurnsLeft));
    out.branch(out.notZero64(nextTurnsLeft), normalEdge(head), normalEdge(behind));

    out.appendTo(behind);
    out.branch(out.testNonZero64(choice, out.constInt64(8)), rareEdge(rareBehind), normalEdge(latch));

    out.appendTo(rareBehind);
    appendReaderOfFrameFormTest(test, turns);
    out.jump(rareJoin);

    out.appendTo(rareJoin);
    appendReaderOfFrameFormTest(test, base);
    out.jump(latch);

    out.appendTo(latch);
    out.addIncomingToPhi(outerTurnsLeft, out.anchor(out.sub(outerTurnsLeft, out.constInt64(1))));
    out.jump(outerHead);

    out.appendTo(end);
    out.ret(out.constInt64(5));
    compileOrFail(test);
    const Order& order = test.procedure().order();
    AOT_FRAME_FORM_TEST_CHECK(test, order.of(enteringRarely).isReached() && deciding->numSuccessors() == 1 && deciding->successor(0).block() == decidingRarely);
    AOT_FRAME_FORM_TEST_CHECK(test, order.headOf(deciding) < order.headOf(head) && order.headOf(head) < order.headOf(decidingRarely) && order.headOf(decidingRarely) < order.headOf(enteringRarely));
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 7 }, uint64_t { 5 }, uint64_t { 2 }) == 5 && !noted);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 7 }, uint64_t { 1 }, uint64_t { 2 }) == 5 && noted == 8);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 8 }, uint64_t { 0 }, uint64_t { 2 }) == 5 && noted == 9);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 9 }, uint64_t { 8 }, uint64_t { 2 }) == 5 && noted == 10);
}

static void testMachineFrameFormArmThatOnlyLeadsOnToSharedNeedIsNoHead()
{
    TestProcedure test(traitsOfFrameFormTest(true, true));
    Output& out = test.out();
    Operation* choice = test.argumentGPR(0);
    Block* returning = out.newBlock();
    Block* deciding = out.newBlock();
    Block* arm = out.newBlock();
    Block* asking = out.newBlock();
    Block* returningToo = out.newBlock();
    Block* calling = out.newBlock();
    out.branch(out.testNonZero64(choice, out.constInt64(1)), normalEdge(deciding), normalEdge(returning));
    out.appendTo(returning);
    out.ret(out.constInt64(1));
    out.appendTo(deciding);
    out.branch(out.testNonZero64(choice, out.constInt64(2)), normalEdge(arm), normalEdge(asking));
    out.appendTo(arm);
    appendReaderOfFrameFormTest(test, choice);
    out.jump(calling);
    out.appendTo(asking);
    out.branch(out.testNonZero64(choice, out.constInt64(4)), normalEdge(calling), normalEdge(returningToo));
    out.appendTo(returningToo);
    out.ret(out.constInt64(2));
    out.appendTo(calling);
    appendCallOfFrameFormTest(test);
    out.ret(out.constInt64(3));
    compileOrFail(test);
    const Allocation& allocation = test.procedure().allocation();
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().frame().coverage() == FrameCoverage::FramedPart);
    AOT_FRAME_FORM_TEST_CHECK(test, allocation.onlyHeadOfFramedPart() && !allocation.isInFramedPart(arm));
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 0 }) == 1 && test.call<uint64_t>(uint64_t { 1 }) == 2 && test.call<uint64_t>(uint64_t { 3 }) == 3 && test.call<uint64_t>(uint64_t { 5 }) == 3);
}

static void buildFrameFormTestWithPatchThatReadsFramePointer(TestProcedure& test)
{
    Output& out = test.out();
    appendCallOfFrameFormTest(test);
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

static void buildFrameFormTestWithGeneratorThatGoesByFormOfFrame(TestProcedure& test, unsigned numberOfValuesAcrossCall)
{
    Output& out = test.out();
    Vector<Operation*, 16> values;
    for (unsigned index = 0; index < numberOfValuesAcrossCall; ++index)
        values.append(out.opaque(out.add(test.argumentGPR(0), out.constInt64(index + 1))));
    appendCallOfFrameFormTest(test);
    PatchOperation* reader = out.patch(Int64);
    reader->effects = Effects::none();
    reader->effects.writesLocalState = true;
    reader->setFindsOperandsInFrameWithoutRecord(true);
    reader->append(values[0], ValueRep::ColdAny);
    reader->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(params.frameIsSetUp() && params.size() == 2 && params[1].isStack());
        jit.load64(params.frame().addressInFrame(params[1].offsetFromFP()), params[0].gpr());
    });
    Operation* sum = reader;
    for (unsigned index = 1; index < numberOfValuesAcrossCall; ++index)
        sum = out.add(sum, values[index]);
    out.ret(sum);
}

static void testMachineFrameFormGeneratorThatGoesByFormOfFrameNeedsNoRecord()
{
    TestProcedure test(traitsOfFrameFormTest(true, true));
    buildFrameFormTestWithGeneratorThatGoesByFormOfFrame(test, 1);
    compileOrFail(test);
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().allocation().holdsPatchWithOperandInFrame() && !test.procedure().allocation().holdsPatchThatCountsOnFrameRecord());
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().frame().hasFrameRecord() == !isX86_64());
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 30 }) == 31);
}

static void testMachineFrameFormManyPlacesInFrameKeepRecordBesideSuchGenerator()
{
    TestProcedure test(traitsOfFrameFormTest(true, true));
    buildFrameFormTestWithGeneratorThatGoesByFormOfFrame(test, 12);
    compileOrFail(test);
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().allocation().holdsPatchWithOperandInFrame() && !test.procedure().allocation().holdsPatchThatCountsOnFrameRecord());
    AOT_FRAME_FORM_TEST_CHECK(test, test.procedure().frame().hasFrameRecord());
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 30 }) == 12 * 30 + 78);
}

static void testMachineFrameFormSavesOfRareRegionArePushedAndPopped()
{
    constexpr uint64_t a = 0x3333444455550001ull;
    constexpr uint64_t b = 0x3333444455550020ull;
    TestProcedure test(traitsOfFrameFormTest(true, true));
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* first = test.argumentGPR(0);
    Operation* second = test.argumentGPR(1);
    Block* slow = out.newBlock();
    Block* done = out.newBlock();
    MacroAssembler::Label beginOfCall;
    MacroAssembler::Label endOfCall;

    out.branch(out.testNonZero64(first, out.constInt64(1)), rareEdge(slow), normalEdge(done));
    out.appendTo(slow);
    RegisterFile file = registerFileFor(proc.traits());
    RegisterMask clobbered = (file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP)) & ~file.calleeSaved;
    PatchOperation* call = out.patch(Void);
    call->clobberLate(clobbered);
    call->setGenerator([&, clobbered](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(params.frameIsSetUp());
        beginOfCall = jit.label();
        forEachRegister(clobbered, [&](Reg reg) {
            if (reg.isGPR())
                jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(poison)), reg.gpr());
        });
        endOfCall = jit.label();
    });
    out.jump(done);
    out.appendTo(done);
    out.ret(out.sub(first, second));
    compileOrFail(test);

    AOT_FRAME_FORM_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::RareRegions && proc.frameRanges().size() == 1);
    AOT_FRAME_FORM_TEST_CHECK(test, proc.allocation().spills().size() == 2 && proc.allocation().spills()[0].isSaveInRareRegion && proc.allocation().spills()[1].isSaveInRareRegion);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(a, b) == a - b);
    AOT_FRAME_FORM_TEST_CHECK(test, test.call<uint64_t>(a - 1, b) == a - 1 - b);
#if CPU(X86_64)
    AOT_FRAME_FORM_TEST_CHECK(test, !proc.frame().hasFrameRecord());
    AOT_FRAME_FORM_TEST_CHECK(test, !MacroAssembler::differenceBetween(proc.frameRanges()[0].begin, beginOfCall));
    AOT_FRAME_FORM_TEST_CHECK(test, !MacroAssembler::differenceBetween(endOfCall, proc.frameRanges()[0].end));
#endif
}

static void testMachineFrameFormUseOfFramePointerKeepsRecord()
{
    TestProcedure test(traitsOfFrameFormTest(false, true));
    Output& out = test.out();
    appendCallOfFrameFormTest(test);
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
    AOT_FRAME_FORM_TEST_CHECK(test, !checkFrame(proc) && proc.failure() && proc.failure()->kind == FailureKind::MissingFrameSetup);
}

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
        { "machine/frame-forms/an-argument-read-into-fixed-registers-takes-no-new-saved-register"_s, Machine::testMachineFrameFormArgumentReadIntoFixedRegistersTakesNoNewSavedRegister },
        { "machine/frame-forms/an-argument-read-as-a-cold-operand-takes-no-new-saved-register"_s, Machine::testMachineFrameFormArgumentReadAsColdOperandTakesNoNewSavedRegister },
        { "machine/frame-forms/an-argument-handed-to-a-phi-at-a-jump-takes-no-new-saved-register"_s, Machine::testMachineFrameFormArgumentHandedToPhiAtJumpTakesNoNewSavedRegister },
        { "machine/frame-forms/an-argument-that-is-returned-takes-no-new-saved-register"_s, Machine::testMachineFrameFormArgumentThatIsReturnedTakesNoNewSavedRegister },
        { "machine/frame-forms/an-argument-that-a-slot-serves-less-cheaply-takes-a-saved-register"_s, Machine::testMachineFrameFormArgumentThatSlotServesLessCheaplyTakesSavedRegister },
        { "machine/frame-forms/second-saved-register-of-a-pair-is-free"_s, Machine::testMachineFrameFormSecondSavedRegisterOfPairIsFree },
        { "machine/frame-forms/arguments-stored-behind-the-setup-are-pushed"_s, Machine::testMachineFrameFormArgumentsStoredBehindSetupArePushed },
        { "machine/frame-forms/saves-of-a-head-are-pushed"_s, Machine::testMachineFrameFormSavesOfHeadArePushed },
        { "machine/frame-forms/what-the-call-at-a-head-takes-stays-where-it-lies"_s, Machine::testMachineFrameFormWhatCallAtHeadTakesStaysWhereItLies },
        { "machine/frame-forms/at-a-head-the-registers-that-callees-keep-go-to-what-is-read-most"_s, Machine::testMachineFrameFormCalleeSavesAtHeadGoToWhatIsReadMost },
        { "machine/frame-forms/number-stored-behind-the-setup-is-not-pushed"_s, Machine::testMachineFrameFormNumberStoredBehindSetupIsNotPushed },
        { "machine/frame-forms/outgoing-words-keep-the-saves-on-top"_s, Machine::testMachineFrameFormOutgoingWordsKeepSavesOnTop },
        { "machine/frame-forms/saved-registers-at-the-bottom-come-back"_s, Machine::testMachineFrameFormSavedRegistersAtBottomComeBack },
        { "machine/frame-forms/function-that-only-calls-has-no-record"_s, Machine::testMachineFrameFormOfFunctionThatOnlyCallsHasNoRecord },
        { "machine/frame-forms/saved-registers-without-a-record-come-back"_s, Machine::testMachineFrameFormSavedRegistersWithoutRecordComeBack },
        { "machine/frame-forms/one-word-without-a-record-lies-at-the-stack-pointer"_s, Machine::testMachineFrameFormOneWordWithoutRecordLiesAtStackPointer },
        { "machine/frame-forms/two-words-have-no-record"_s, Machine::testMachineFrameFormWithTwoWordsHasNoRecord },
        { "machine/frame-forms/many-words-have-no-record"_s, Machine::testMachineFrameFormWithManyWordsHasNoRecord },
        { "machine/frame-forms/a-read-in-a-later-arm-behind-a-join-is-out-of-reach"_s, Machine::testMachineFrameFormReadInLaterArmBehindJoinIsOutOfReach },
        { "machine/frame-forms/a-read-in-a-loop-that-a-rare-block-behind-it-enters-is-within-reach"_s, Machine::testMachineFrameFormReadInLoopThatRareBlockBehindItEntersIsWithinReach },
        { "machine/frame-forms/an-arm-that-only-leads-on-to-a-shared-need-is-no-head"_s, Machine::testMachineFrameFormArmThatOnlyLeadsOnToSharedNeedIsNoHead },
        { "machine/frame-forms/patch-that-needs-the-record-keeps-it"_s, Machine::testMachineFrameFormPatchThatNeedsRecordKeepsIt },
        { "machine/frame-forms/generator-told-of-a-place-in-the-frame-keeps-the-record"_s, Machine::testMachineFrameFormGeneratorToldOfPlaceInFrameKeepsRecord },
        { "machine/frame-forms/generator-that-goes-by-the-form-of-the-frame-needs-no-record"_s, Machine::testMachineFrameFormGeneratorThatGoesByFormOfFrameNeedsNoRecord },
        { "machine/frame-forms/many-places-in-the-frame-keep-the-record-beside-such-a-generator"_s, Machine::testMachineFrameFormManyPlacesInFrameKeepRecordBesideSuchGenerator },
        { "machine/frame-forms/saves-of-a-rare-region-are-pushed-and-popped"_s, Machine::testMachineFrameFormSavesOfRareRegionArePushedAndPopped },
        { "machine/frame-forms/use-of-the-frame-pointer-keeps-the-record"_s, Machine::testMachineFrameFormUseOfFramePointerKeepsRecord },
        { "machine/frame-forms/handlers-keep-the-record"_s, Machine::testMachineFrameFormHandlersKeepRecord },
        { "machine/frame-forms/checker-refuses-a-frame-without-a-record-that-a-patch-counts-on"_s, Machine::testMachineFrameFormCheckerRefusesFrameWithoutRecordThatPatchCountsOn },
        { "machine/frame-forms/every-form-gives-everything-back"_s, Machine::testMachineFrameFormsGiveEverythingBack },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
