/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineTestSupport.h"
#include "AOTSavedFrame.h"
#include "AOTSlotRepresentation.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "StackAlignment.h"
#include <algorithm>
#include <array>
#include <bit>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

// No request goes to the builder between patch() and the last word of the setup: the next request seals the Patch.
// No constant here is one that findPinnedConstant() knows: the tests run without an instance.
// The wrapper of TestProcedure restores every register that the C convention wants kept, so a test that asks whether compiled code restores them has to look before it returns.

namespace JSC { namespace AOT { namespace Machine {

// Only behind a prepare() or compile() that went well.
[[noreturn]] static void failFrameTest(const TestProcedure& test, const char* file, int line, const char* condition)
{
    dataLogLn(test.printedProcedure(), test.printedAllocation(), test.printedFrame());
    failNativeTest(file, line, condition);
}

#define AOT_FRAME_TEST_CHECK(test, ...) do { \
        if (!(__VA_ARGS__)) [[unlikely]] \
            failFrameTest(test, __FILE__, __LINE__, #__VA_ARGS__); \
    } while (false)

static constexpr int32_t bytesOfFrameTestWord = sizeof(uint64_t);
// The frame pointer lies this far below the stack pointer of the entry: the record, less the return address where the call has pushed it.
static constexpr int32_t bytesPushedByFrameTestSetup = isX86_64() ? bytesOfFrameTestWord : 2 * bytesOfFrameTestWord;

static RegisterFile registerFileOfFrameTest(TestProcedure& test)
{
    return registerFileFor(test.procedure().traits());
}

static RegisterMask callerSavedRegistersOfFrameTest(TestProcedure& test)
{
    RegisterFile file = registerFileOfFrameTest(test);
    return (file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP)) & ~file.calleeSaved;
}

static unsigned numberOfFrameTestRegisters(RegisterMask mask)
{
    return static_cast<unsigned>(std::popcount(mask));
}

// The same whatever the traits.
static unsigned numberOfCalleeSavedGPRsInFrameTests()
{
    return numberOfFrameTestRegisters(registerFileFor(ProcedureTraits()).calleeSavedIn(Bank::GP));
}

static uint64_t sumOfFrameTestValues(uint64_t seed, unsigned count)
{
    uint64_t sum = 0;
    for (unsigned index = 0; index < count; ++index)
        sum += seed + index + 1;
    return sum;
}

// The seed plus 1, 2 and so on. Nothing can compute one again, so each needs a place of its own while it is live.
static Vector<Operation*> defineFrameTestValues(Output& out, Operation* seed, unsigned count)
{
    Vector<Operation*> values;
    for (unsigned index = 0; index < count; ++index)
        values.append(out.opaque(out.add(seed, index + 1)));
    return values;
}

static Operation* addUpFrameTestValues(Output& out, Operation* first, std::span<Operation* const> values)
{
    Operation* sum = first;
    for (Operation* value : values)
        sum = out.add(sum, value);
    return sum;
}

static void emitFrameTestPoison(CCallHelpers& jit, RegisterMask registers)
{
    GPRReg poisoned = InvalidGPRReg;
    forEachRegister(registers, [&](Reg reg) {
        if (!reg.isGPR())
            return;
        poisoned = reg.gpr();
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(poison)), poisoned);
    });
    AOT_TEST_CHECK(poisoned != InvalidGPRReg);
    forEachRegister(registers, [&](Reg reg) {
        if (reg.isFPR())
            jit.move64ToDouble(poisoned, reg.fpr());
    });
}

// Writes every register that no callee saves. The collector may run in it.
static PatchOperation* appendFrameTestCall(TestProcedure& test)
{
    RegisterMask clobbered = callerSavedRegistersOfFrameTest(test);
    PatchOperation* call = test.out().patch(Void);
    call->clobberLate(clobbered);
    call->setGenerator([clobbered](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(params.frameIsSetUp());
        emitFrameTestPoison(jit, clobbered);
    });
    return call;
}

struct FrameTestMark {
    MacroAssembler::Label label;
    unsigned runs { 0 };
    bool frameIsSetUp { false };
};

static PatchOperation* appendFrameTestMark(Output& out, FrameTestMark& mark, bool runsWithoutFrame)
{
    PatchOperation* patch = out.patch(Void);
    patch->effects = Effects::none();
    patch->effects.writesLocalState = true;
    patch->setRunsWithoutFrame(runsWithoutFrame);
    patch->setGenerator([&mark](CCallHelpers& jit, const PatchParameters& params) {
        ++mark.runs;
        mark.label = jit.label();
        mark.frameIsSetUp = params.frameIsSetUp();
        jit.nop();
    });
    return patch;
}

// Zero where the stack pointer is aligned and lies as far below the frame pointer as the frame says.
static PatchOperation* appendFrameTestProbe(Output& out)
{
    PatchOperation* probe = out.patch(Int64);
    probe->effects = Effects::none();
    probe->effects.writesLocalState = true;
    probe->numGPScratchRegisters = 1;
    probe->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        AOT_TEST_CHECK(params.frameIsSetUp());
        GPRReg result = params[0].gpr();
        GPRReg stackPointer = params.gpScratch(0);
        jit.move(CCallHelpers::stackPointerRegister, stackPointer);
        jit.move(stackPointer, result);
        jit.and64(CCallHelpers::TrustedImm32(static_cast<int32_t>(stackAlignmentBytes() - 1)), result);
        jit.add64(CCallHelpers::TrustedImm32(static_cast<int32_t>(params.frame().bytesBelowFrameRecord())), stackPointer);
        jit.xor64(CCallHelpers::framePointerRegister, stackPointer);
        jit.or64(stackPointer, result);
    });
    return probe;
}

struct FrameTestSetups {
    Vector<MacroAssembler::Label, 4> labels;
    bool sizesAgree { true };
};

// A setup may write nothing more behind the check, and the range of the frame begins where the setup ends. The nop keeps the label ahead of it.
static void noteFrameTestSetup(void* context, CCallHelpers& jit, const Frame& frame, const StackCheckSite& site)
{
    FrameTestSetups& setups = *static_cast<FrameTestSetups*>(context);
    setups.labels.append(jit.label());
    jit.nop();
    setups.sizesAgree = setups.sizesAgree && site.bytesBelowFrameRecord == frame.bytesBelowFrameRecord()
        && site.isAllocated == (site.bytesBelowFrameRecord <= maxBytesAllocatedBeforeStackCheck);
}

static bool frameTestRangesCover(const Procedure& proc, MacroAssembler::Label label)
{
    for (const FrameRange& range : proc.frameRanges()) {
        if (MacroAssembler::differenceBetween(range.begin, label) >= 0 && MacroAssembler::differenceBetween(label, range.end) > 0)
            return true;
    }
    return false;
}

static void checkFrameTestRanges(TestProcedure& test)
{
    std::span<const FrameRange> ranges = test.procedure().frameRanges();
    AOT_FRAME_TEST_CHECK(test, !ranges.empty());
    for (size_t index = 0; index < ranges.size(); ++index) {
        AOT_FRAME_TEST_CHECK(test, ranges[index].begin.isSet() && ranges[index].end.isSet());
        AOT_FRAME_TEST_CHECK(test, MacroAssembler::differenceBetween(ranges[index].begin, ranges[index].end) >= 0);
        AOT_FRAME_TEST_CHECK(test, !index || MacroAssembler::differenceBetween(ranges[index - 1].end, ranges[index].begin) >= 0);
    }
}

static int32_t topOfFrameTestSlots(const Frame& frame)
{
    return frame.savesLieAtBottom() ? 0 : -bytesOfFrameTestWord * static_cast<int32_t>(roundUpToMultipleOf<2>(frame.numberOfCalleeSaves()));
}

// The words of a frame are counted down from the frame pointer, the first being 1.
static bool frameTestWordHolds(unsigned location, intptr_t offsetFromFP, unsigned numberOfBytes)
{
    intptr_t begin = -static_cast<intptr_t>(bytesOfFrameTestWord) * static_cast<intptr_t>(location);
    return offsetFromFP < begin + bytesOfFrameTestWord && offsetFromFP + static_cast<intptr_t>(numberOfBytes) > begin;
}

// The one form of a frame: the run of callee saves below the record, then what SlotAssignment lays out, then the outgoing words.
static void checkFrameTestLayout(TestProcedure& test)
{
    Procedure& proc = test.procedure();
    const SlotAssignment& slots = proc.slots();
    const Frame& frame = proc.frame();
    AOT_FRAME_TEST_CHECK(test, frame.hasFrame() && frame.frameSize() == frame.bytesBelowFrameRecord());
    AOT_FRAME_TEST_CHECK(test, !(frame.bytesBelowFrameRecord() % stackAlignmentBytes()));

    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    unsigned numberOfSaves = frame.numberOfCalleeSaves();
    AOT_FRAME_TEST_CHECK(test, numberOfSaves == numberOfFrameTestRegisters(frame.calleeSavesMask()));
    AOT_FRAME_TEST_CHECK(test, !(frame.calleeSavesMask() & ~registerFileOfFrameTest(test).calleeSaved));
    for (unsigned index = 0; index < numberOfSaves; ++index) {
        const RegisterAtOffset& save = saves.at(index);
        AOT_FRAME_TEST_CHECK(test, frame.calleeSavesMask() & maskOf(save.reg()));
        AOT_FRAME_TEST_CHECK(test, save.offset() == (frame.savesLieAtBottom() ? static_cast<ptrdiff_t>(bytesOfFrameTestWord) * static_cast<ptrdiff_t>(index) - static_cast<ptrdiff_t>(frame.bytesBelowFrameRecord()) : -static_cast<ptrdiff_t>(bytesOfFrameTestWord) * static_cast<ptrdiff_t>(numberOfSaves - index)));
        AOT_FRAME_TEST_CHECK(test, !index || saves.at(index - 1).reg().index() < save.reg().index());
    }

    struct Place {
        intptr_t begin;
        intptr_t end;
        Position first;
        Position last;
    };
    Vector<Place> places;
    intptr_t top = topOfFrameTestSlots(frame);
    intptr_t bottom = static_cast<intptr_t>(proc.callArgAreaSizeInBytes()) - static_cast<intptr_t>(frame.bytesBelowFrameRecord());
    auto addPlace = [&](intptr_t offset, unsigned numberOfBytes, unsigned alignment, uint32_t distance, Position first, Position last) {
        Place place { offset, offset + static_cast<intptr_t>(numberOfBytes), first, last };
        AOT_FRAME_TEST_CHECK(test, place.begin == top - static_cast<intptr_t>(distance));
        AOT_FRAME_TEST_CHECK(test, !(place.begin % static_cast<intptr_t>(alignment)));
        AOT_FRAME_TEST_CHECK(test, place.begin >= bottom && place.end <= top);
        for (const Place& other : places)
            AOT_FRAME_TEST_CHECK(test, place.begin >= other.end || place.end <= other.begin || place.first > other.last || place.last < other.first);
        places.append(place);
    };
    std::span<const SpillFacts> spills = proc.allocation().spills();
    for (SpillIndex index = 0; index < spills.size(); ++index) {
        unsigned numberOfBytes = bytesForWidth(spills[index].width);
        AOT_FRAME_TEST_CHECK(test, spills[index].begin <= spills[index].end);
        addPlace(frame.offsetFromFPOf(index), numberOfBytes, numberOfBytes, slots.distanceOf(index), spills[index].begin, spills[index].end);
    }
    for (const StackSlot* slot : proc.stackSlots())
        addPlace(slot->offsetFromFP(), slot->byteSize(), slot->byteSize() >= 16 ? 16 : 8, slots.distanceOf(slot), 0, noPosition);
}

// Where the first argument is odd: a rare block with a call that comes back. Where its bit 1 is set: a rare block with a call that returns 1000. Else the sum of both arguments.
struct FrameTestWithRareCalls {
    explicit FrameTestWithRareCalls(const ProcedureTraits& traits)
        : test(traits)
    {
        Output& out = test.out();
        test.procedure().setStackCheckGenerator(noteFrameTestSetup, &setups);
        Operation* a = test.argumentGPR(0);
        Operation* b = test.argumentGPR(1);
        rejoining = out.newBlock();
        Block* middle = out.newBlock();
        leaving = out.newBlock();
        Block* end = out.newBlock();

        appendFrameTestMark(out, markOutside, true);
        ValueFromBlock fromEntry = out.anchor(out.int64Zero);
        out.branch(out.testNonZero64(a, 1), rareEdge(rejoining), normalEdge(middle));

        out.appendTo(rejoining);
        appendFrameTestMark(out, markInRejoiningBlock, false);
        appendFrameTestCall(test);
        ValueFromBlock fromRareBlock = out.anchor(appendFrameTestProbe(out));
        out.jump(middle);

        out.appendTo(middle);
        Operation* probe = out.phi(Int64, fromEntry, fromRareBlock);
        out.branch(out.testNonZero64(a, 2), rareEdge(leaving), normalEdge(end));

        out.appendTo(leaving);
        appendFrameTestMark(out, markInLeavingBlock, false);
        appendFrameTestCall(test);
        out.ret(out.add(appendFrameTestProbe(out), 1000));

        out.appendTo(end);
        out.ret(out.add(out.add(a, b), probe));
        compileOrFail(test);
    }

    void checkAnswers()
    {
        AOT_FRAME_TEST_CHECK(test, markOutside.runs == 1 && markInRejoiningBlock.runs == 1 && markInLeavingBlock.runs == 1);
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 4 }, uint64_t { 10 }) == 14);
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 5 }, uint64_t { 10 }) == 15);
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 6 }, uint64_t { 10 }) == 1000);
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 7 }, uint64_t { 10 }) == 1000);
    }

    TestProcedure test;
    FrameTestSetups setups;
    FrameTestMark markOutside;
    FrameTestMark markInRejoiningBlock;
    FrameTestMark markInLeavingBlock;
    Block* rejoining { nullptr };
    Block* leaving { nullptr };
};

static void testMachineFrameCoversRareBlocksAlone()
{
    FrameTestWithRareCalls function { ProcedureTraits() };
    TestProcedure& test = function.test;
    Procedure& proc = test.procedure();
    const Order& order = proc.order();
    const Allocation& allocation = proc.allocation();
    const Frame& frame = proc.frame();

    AOT_FRAME_TEST_CHECK(test, frame.coverage() == FrameCoverage::RareRegions && test.printedFrame().startsWith("coverage = RareRegions\n"_s));
    AOT_FRAME_TEST_CHECK(test, !frame.calleeSavesMask() && !allocation.usedCalleeSaves() && !allocation.needsFrameOutsideRareRegions());
    for (Block* block : { function.rejoining, function.leaving })
        AOT_FRAME_TEST_CHECK(test, order.of(block).isRare() && allocation.needsFrameIn(order.of(block).rareRegion));
    AOT_FRAME_TEST_CHECK(test, order.of(function.rejoining).rareRegion != order.of(function.leaving).rareRegion);
    checkFrameTestLayout(test);

    AOT_FRAME_TEST_CHECK(test, function.setups.labels.size() == 2 && function.setups.sizesAgree);
    AOT_FRAME_TEST_CHECK(test, !function.markOutside.frameIsSetUp && function.markInRejoiningBlock.frameIsSetUp && function.markInLeavingBlock.frameIsSetUp);
    checkFrameTestRanges(test);
    AOT_FRAME_TEST_CHECK(test, !frameTestRangesCover(proc, proc.entrypointLabel(0)) && !frameTestRangesCover(proc, function.markOutside.label));
    AOT_FRAME_TEST_CHECK(test, frameTestRangesCover(proc, function.markInRejoiningBlock.label) && frameTestRangesCover(proc, function.markInLeavingBlock.label));
    for (const FrameRange& range : proc.frameRanges())
        AOT_FRAME_TEST_CHECK(test, MacroAssembler::differenceBetween(proc.rareCodeStart(), range.begin) >= 0);
    for (MacroAssembler::Label setup : function.setups.labels)
        AOT_FRAME_TEST_CHECK(test, MacroAssembler::differenceBetween(proc.rareCodeStart(), setup) >= 0 && !frameTestRangesCover(proc, setup));
    function.checkAnswers();
}

// A long function with two ways that return. The block that the first way seldom jumps into is small: on x86-64 it stands behind the Return of that way, within reach of a short jump, and not behind the second way.
static void testMachineSmallColdBlockStandsNearItsJump(bool isRare)
{
    constexpr unsigned numberOfSums = 40;
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock first = out.newBlock();
    LBasicBlock second = out.newBlock();
    LBasicBlock rare = out.newBlock();
    LBasicBlock behind = out.newBlock();
    LValue a = test.argumentGPR(0);
    LValue b = test.argumentGPR(1);
    out.branch(out.testNonZero64(a, 1), normalEdge(first), normalEdge(second));

    FrameTestMark markInRareBlock;
    FrameTestMark markInSecondWay;
    out.appendTo(first);
    out.branch(out.testNonZero64(a, 2), isRare ? rareEdge(rare) : unlikelyEdge(rare), normalEdge(behind));
    out.appendTo(rare);
    appendFrameTestMark(out, markInRareBlock, true);
    out.ret(out.add(b, 1000));
    out.appendTo(behind);
    out.ret(out.add(a, b));

    out.appendTo(second);
    appendFrameTestMark(out, markInSecondWay, true);
    LValue sum = a;
    for (unsigned index = 0; index < numberOfSums; ++index)
        sum = out.bitXor(out.add(sum, b), out.constInt64(index + 3));
    out.ret(sum);
    compileOrFail(test);

    Procedure& proc = test.procedure();
    AOT_FRAME_TEST_CHECK(test, proc.order().of(rare).isRare() == isRare && proc.order().of(rare).layoutPart == (isRare ? LayoutPart::Rare : LayoutPart::Unlikely));
    AOT_FRAME_TEST_CHECK(test, markInRareBlock.runs == 1 && markInSecondWay.runs == 1);
    bool standsNearItsJump = MacroAssembler::differenceBetween(markInRareBlock.label, markInSecondWay.label) > 0;
    AOT_FRAME_TEST_CHECK(test, standsNearItsJump == isX86_64());
    AOT_FRAME_TEST_CHECK(test, MacroAssembler::differenceBetween(markInRareBlock.label, proc.rareCodeStart()) > 0 == (standsNearItsJump || !isRare));
    AOT_FRAME_TEST_CHECK(test, proc.rangesOfRareCodeAmongOtherCode().size() == (standsNearItsJump && isRare));
    if (standsNearItsJump && isRare) {
        const RangeOfRareCode& range = proc.rangesOfRareCodeAmongOtherCode().front();
        AOT_FRAME_TEST_CHECK(test, MacroAssembler::differenceBetween(range.begin, markInRareBlock.label) >= 0 && MacroAssembler::differenceBetween(markInRareBlock.label, range.end) > 0);
        AOT_FRAME_TEST_CHECK(test, MacroAssembler::differenceBetween(range.end, markInSecondWay.label) >= 0);
    }

    auto expected = [&](uint64_t a, uint64_t b) {
        if (!(a & 1)) {
            uint64_t sum = a;
            for (unsigned index = 0; index < numberOfSums; ++index)
                sum = (sum + b) ^ (index + 3);
            return sum;
        }
        return a & 2 ? b + 1000 : a + b;
    };
    for (uint64_t argument : { 4u, 5u, 6u, 7u })
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(argument, uint64_t { 10 }) == expected(argument, 10));
}

static void testMachineSmallRareRegionStandsAmongOtherCode()
{
    testMachineSmallColdBlockStandsNearItsJump(true);
}

static void testMachineSmallRunOfUnlikelyBlocksStandsNearItsJump()
{
    testMachineSmallColdBlockStandsNearItsJump(false);
}

static void testMachineFrameIsSetUpAtEntryWhereTraitsAskForIt()
{
    for (unsigned reason = 0; reason < 3; ++reason) {
        ProcedureTraits traits;
        traits.hasHandlers = !reason;
        traits.rejectsOnThrow = reason == 1;
        traits.savesFrameOnSuspend = reason == 2;
        FrameTestWithRareCalls function(traits);
        TestProcedure& test = function.test;
        Procedure& proc = test.procedure();

        AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::WholeFunction && test.printedFrame().startsWith("coverage = WholeFunction\n"_s));
        checkFrameTestLayout(test);
        AOT_FRAME_TEST_CHECK(test, function.setups.labels.size() == 1 && function.setups.sizesAgree);
        AOT_FRAME_TEST_CHECK(test, function.markOutside.frameIsSetUp && function.markInRejoiningBlock.frameIsSetUp && function.markInLeavingBlock.frameIsSetUp);
        checkFrameTestRanges(test);
        AOT_FRAME_TEST_CHECK(test, !frameTestRangesCover(proc, proc.entrypointLabel(0)) && !frameTestRangesCover(proc, function.setups.labels[0]));
        AOT_FRAME_TEST_CHECK(test, MacroAssembler::differenceBetween(function.setups.labels[0], function.markOutside.label) >= 0);
        for (const FrameTestMark* mark : { &function.markOutside, &function.markInRejoiningBlock, &function.markInLeavingBlock })
            AOT_FRAME_TEST_CHECK(test, frameTestRangesCover(proc, mark->label));
        function.checkAnswers();
    }
}

// By Reg::index(): what each callee-saved register held at the entry. Then the frame pointer of the function, then the sum that it computed.
static constexpr unsigned framePointerInFrameTestRecord = 64;
static constexpr unsigned sumInFrameTestRecord = 65;
using FrameTestRecord = std::array<uint64_t, 66>;

static CCallHelpers::Address wordOfFrameTestRecord(GPRReg record, unsigned index)
{
    return CCallHelpers::Address(record, static_cast<int32_t>(index * sizeof(uint64_t)));
}

static unsigned runFrameTestOfCalleeSaves(unsigned numberOfIntegers, unsigned numberOfNumbers)
{
    constexpr uint64_t a = 3000;
    constexpr double x = 0.25;
    TestProcedure test;
    Output& out = test.out();
    RegisterMask calleeSaved = registerFileOfFrameTest(test).calleeSaved;
    Operation* seed = test.argumentGPR(0);
    Operation* argumentWithRecord = test.argumentGPR(1);
    Operation* seedOfNumbers = test.argumentFPR(0);

    // Nothing that is defined ahead of it lives across a call, so no callee-saved register has been written yet.
    PatchOperation* atEntry = out.patch(Void);
    atEntry->appendSomeRegister(argumentWithRecord);
    atEntry->setGenerator([calleeSaved](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg record = params[0].gpr();
        AOT_TEST_CHECK(params.frameIsSetUp() && !(maskOf(Reg(record)) & calleeSaved));
        forEachRegister(calleeSaved, [&](Reg reg) {
            if (reg.isGPR())
                jit.store64(reg.gpr(), wordOfFrameTestRecord(record, reg.index()));
            else
                jit.storeDouble(reg.fpr(), wordOfFrameTestRecord(record, reg.index()));
        });
        jit.store64(CCallHelpers::framePointerRegister, wordOfFrameTestRecord(record, framePointerInFrameTestRecord));
    });

    Operation* record = out.opaque(argumentWithRecord);
    Vector<Operation*> integers = defineFrameTestValues(out, seed, numberOfIntegers);
    Vector<Operation*> numbers;
    for (unsigned index = 0; index < numberOfNumbers; ++index)
        numbers.append(out.opaque(out.add(seedOfNumbers, out.constDouble(index + 1.5))));
    appendFrameTestCall(test);
    Operation* sumOfNumbers = addUpFrameTestValues(out, out.constDouble(0), numbers.span());
    Operation* sum = out.add(addUpFrameTestValues(out, out.int64Zero, integers.span()), out.bitCast(sumOfNumbers, Int64));
    out.store64(sum, out.address(test.heaps().argumentBuffer, record, sumInFrameTestRecord * sizeof(uint64_t)));

    // Returns zero where the save area holds what the registers held at the entry, and the teardown puts all of it back.
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
            jit.load64(wordOfFrameTestRecord(record, index), expected);
            jit.xor64(expected, word);
            jit.or64(word, differences);
        };

        jit.move(CCallHelpers::TrustedImm32(0), differences);
        for (const RegisterAtOffset& save : frame.calleeSaveRegisterAtOffsetList()) {
            jit.load64(CCallHelpers::Address(CCallHelpers::framePointerRegister, static_cast<int32_t>(save.offset())), word);
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
        jit.sub64(CCallHelpers::TrustedImm32(bytesPushedByFrameTestSetup), word);
        compareWordWithRecord(framePointerInFrameTestRecord);
        jit.move(differences, GPRInfo::returnValueGPR);
        jit.ret();
    });

    compileOrFail(test);
    const Frame& frame = test.procedure().frame();
    AOT_FRAME_TEST_CHECK(test, frame.coverage() == FrameCoverage::WholeFunction);
    AOT_FRAME_TEST_CHECK(test, frame.calleeSavesMask() && frame.calleeSavesMask() == test.procedure().allocation().usedCalleeSaves());
    AOT_FRAME_TEST_CHECK(test, test.printedFrame().contains("calleeSaves = ["_s));
    checkFrameTestLayout(test);

    double expectedSumOfNumbers = 0;
    for (unsigned index = 0; index < numberOfNumbers; ++index)
        expectedSumOfNumbers += x + (index + 1.5);
    FrameTestRecord words { };
    AOT_FRAME_TEST_CHECK(test, !test.call<uint64_t>(a, words.data(), x));
    AOT_FRAME_TEST_CHECK(test, words[sumInFrameTestRecord] == sumOfFrameTestValues(a, numberOfIntegers) + std::bit_cast<uint64_t>(expectedSumOfNumbers));
    return frame.numberOfCalleeSaves();
}

static void testMachineFrameCalleeSavesAscendBelowRecord()
{
    bool sawOddNumber = false;
    bool sawEvenNumber = false;
    for (unsigned numberOfNumbers : { 0u, 3u }) {
        for (unsigned numberOfIntegers = 0; numberOfIntegers <= numberOfCalleeSavedGPRsInFrameTests() + 1; ++numberOfIntegers) {
            bool isOdd = runFrameTestOfCalleeSaves(numberOfIntegers, numberOfNumbers) % 2;
            sawOddNumber = sawOddNumber || isOdd;
            sawEvenNumber = sawEvenNumber || !isOdd;
        }
    }
    AOT_TEST_CHECK(sawOddNumber && sawEvenNumber);
}

static RegisterMask registersLostAtFrameTestSuspension(TestProcedure& test)
{
    RegisterFile file = registerFileOfFrameTest(test);
    RegisterMask lost = file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP);
    if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
        lost &= ~maskOf(Reg(SavedFrame::preservedRegister));
    return lost;
}

// Stands for what an earlier call has left where the frame comes to lie.
static constexpr uint64_t leftoverInFrameTest = 0x1ef701ef701ef701ull;

// A saved frame holds whole pairs of words: SavedFrame::numberOfWordsSkippedFor() leaves it the word below a kept part with an odd number of words.
static unsigned lowestFrameTestWordKept(const Frame& frame)
{
    return frame.bytesBelowFrameRecord() / bytesOfFrameTestWord - SavedFrame::numberOfWordsSkippedFor(frame.bytesDeadAtSuspensions() / bytesOfFrameTestWord);
}

// The words of a saved frame that the back end keeps free of leftovers. Every resumption writes the callee saves anew, and the StackSlots are the emitters'.
static bool isFrameTestWordKeptClean(const Procedure& proc, unsigned location)
{
    const Frame& frame = proc.frame();
    if (location <= frame.numberOfCalleeSaves() || location > lowestFrameTestWordKept(frame))
        return false;
    for (const StackSlot* slot : proc.stackSlots()) {
        if (!slot->isDeadAtSuspensions() && frameTestWordHolds(location, slot->offsetFromFP(), slot->byteSize()))
            return false;
    }
    return true;
}

// Fills those words. Nothing has been stored to the frame where it stands. What the setup has cleared stays cleared.
static PatchOperation* appendFrameTestLeftovers(Output& out)
{
    PatchOperation* patch = out.patch(Void);
    patch->effects = Effects::none();
    patch->effects.writesLocalState = true;
    patch->numGPScratchRegisters = 1;
    patch->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        const Frame& frame = params.frame();
        AOT_TEST_CHECK(params.frameIsSetUp());
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(leftoverInFrameTest)), params.gpScratch(0));
        for (unsigned location = 1; location <= lowestFrameTestWordKept(frame); ++location) {
            int32_t offset = -bytesOfFrameTestWord * static_cast<int32_t>(location);
            bool isClearedAtSetup = offset >= frame.beginOfWordsClearedAtSetup() && offset < frame.endOfWordsClearedAtSetup();
            if (isFrameTestWordKeptClean(params.proc(), location) && !isClearedAtSetup)
                jit.store64(params.gpScratch(0), CCallHelpers::Address(CCallHelpers::framePointerRegister, offset));
        }
    });
    return patch;
}

// Adds to `count` how many of those words hold the leftover. Writes the other two registers.
static void emitFrameTestCountOfLeftovers(CCallHelpers& jit, const Procedure& proc, uint64_t leftover, GPRReg count, GPRReg word, GPRReg pattern)
{
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(leftover)), pattern);
    for (unsigned location = 1; location <= lowestFrameTestWordKept(proc.frame()); ++location) {
        if (!isFrameTestWordKeptClean(proc, location))
            continue;
        jit.load64(CCallHelpers::Address(CCallHelpers::framePointerRegister, -bytesOfFrameTestWord * static_cast<int32_t>(location)), word);
        CCallHelpers::Jump isNoLeftover = jit.branch64(CCallHelpers::NotEqual, word, pattern);
        jit.add64(CCallHelpers::TrustedImm32(1), count);
        isNoLeftover.link(&jit);
    }
}

// By a store in front of the suspension, or by whoever suspends the function there for the first time.
static bool isFrameTestWordClearedAt(const Frame& frame, const PatchOperation* suspension, unsigned location)
{
    unsigned lowestWordKept = lowestFrameTestWordKept(frame);
    return suspension->frameWordsClearedInFrontOfSuspension().get(location)
        || (location <= lowestWordKept && location + suspension->numberOfFrameWordsToClearAtFirstSuspension() > lowestWordKept);
}

static unsigned numberOfFrameTestWordsClearedInFrontOf(const PatchOperation* suspension)
{
    const BitSpan& words = suspension->frameWordsClearedInFrontOfSuspension();
    unsigned count = 0;
    for (size_t word = 0; word < words.size(); ++word)
        count += words.get(word);
    return count;
}

enum class FrameTestSuspension : uint8_t {
    // The function goes on without being suspended, as at an await whose value is there already. The frame stays as it is.
    GoesOn,
    // Nobody has suspended the function before. It clears what emitAllocateSavedFrame() clears.
    // The two words behind the one that `sumOfRoots` points at get the number of leftovers that the saved frame keeps, and the number that were there before it cleared anything.
    IsFirst,
    IsLater,
};

// Stands for a suspension and the resumption behind it: whatever a saved frame does not keep comes back as poison.
// Then it adds each root, read from the place that the Patch is told, to the word that `sumOfRoots` points at, which is a root as well.
static PatchOperation* appendFrameTestSuspension(TestProcedure& test, Operation* sumOfRoots, std::span<Operation* const> roots, FrameTestSuspension kind = FrameTestSuspension::IsLater, uint64_t leftover = leftoverInFrameTest)
{
    RegisterMask lost = registersLostAtFrameTestSuspension(test);
    PatchOperation* suspension = test.out().patch(Void);
    suspension->append(sumOfRoots, ValueRep::LateColdAny);
    for (Operation* root : roots)
        suspension->append(root, ValueRep::LateColdAny);
    suspension->clobberLate(lost);
    suspension->setIsSuspension(true);
    suspension->setGenerator([lost, kind, leftover](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        constexpr GPRReg address = GPRInfo::argumentGPR0;
        constexpr GPRReg word = GPRInfo::argumentGPR1;
        constexpr GPRReg sum = GPRInfo::argumentGPR2;
        constexpr GPRReg pattern = GPRInfo::argumentGPR3;
        const Frame& frame = params.frame();
        const std::optional<BitSpan>& deadWords = params.value()->frameWordsDeadAcrossSuspension();
        AOT_TEST_CHECK(params.frameIsSetUp() && deadWords);
        unsigned numberOfWords = frame.bytesBelowFrameRecord() / bytesOfFrameTestWord;
        unsigned numberOfWordsKept = numberOfWords - frame.bytesDeadAtSuspensions() / bytesOfFrameTestWord;

        for (unsigned index = 0; index < params.size(); ++index) {
            const ValueRep& rep = params[index];
            if (rep.isGPR() && rep.gpr() == SavedFrame::preservedRegister)
                continue;
            unsigned numberOfBytes = sizeOf(params.value()->child(index)->type());
            AOT_TEST_CHECK(rep.isStack() && rep.offsetFromFP() < 0 && !(rep.offsetFromFP() % static_cast<int32_t>(numberOfBytes)));
            for (unsigned location = 1; location <= numberOfWords; ++location)
                AOT_TEST_CHECK(!frameTestWordHolds(location, rep.offsetFromFP(), numberOfBytes) || (location <= numberOfWordsKept && !deadWords->get(location)));
        }

        if (kind == FrameTestSuspension::IsFirst) {
            unsigned lowestWordKept = lowestFrameTestWordKept(frame);
            unsigned numberOfWordsToClear = params.value()->numberOfFrameWordsToClearAtFirstSuspension();
            AOT_TEST_CHECK(numberOfWordsToClear <= SavedFrame::maxClearedWords && frame.numberOfCalleeSaves() + numberOfWordsToClear <= lowestWordKept);
            params[0].emitRestore(jit, address);
            jit.move(CCallHelpers::TrustedImm32(0), sum);
            emitFrameTestCountOfLeftovers(jit, params.proc(), leftover, sum, word, pattern);
            jit.store64(sum, CCallHelpers::Address(address, 2 * bytesOfFrameTestWord));
            jit.move(CCallHelpers::TrustedImm32(0), sum);
            for (unsigned location = lowestWordKept; location + numberOfWordsToClear > lowestWordKept; --location)
                jit.store64(sum, CCallHelpers::Address(CCallHelpers::framePointerRegister, -bytesOfFrameTestWord * static_cast<int32_t>(location)));
            emitFrameTestCountOfLeftovers(jit, params.proc(), leftover, sum, word, pattern);
            jit.store64(sum, CCallHelpers::Address(address, bytesOfFrameTestWord));
        }

        emitFrameTestPoison(jit, lost);
        for (unsigned location = 1; location <= numberOfWords && kind != FrameTestSuspension::GoesOn; ++location) {
            if (location > numberOfWordsKept || deadWords->get(location))
                jit.store64(word, CCallHelpers::Address(CCallHelpers::framePointerRegister, -bytesOfFrameTestWord * static_cast<int32_t>(location)));
        }

        params[0].emitRestore(jit, address);
        jit.load64(CCallHelpers::Address(address), sum);
        for (unsigned index = 1; index < params.size(); ++index) {
            params[index].emitRestore(jit, word);
            if (params.value()->child(index)->type() == Int32)
                jit.zeroExtend32ToWord(word, word);
            jit.add64(word, sum);
        }
        jit.store64(sum, CCallHelpers::Address(address));
        emitFrameTestPoison(jit, maskOf(Reg(address)) | maskOf(Reg(word)) | maskOf(Reg(sum)));
    });
    return suspension;
}

static void checkFrameTestSavesOfSavedFrame(TestProcedure& test)
{
    RegisterFile file = registerFileOfFrameTest(test);
    const Frame& frame = test.procedure().frame();
    RegisterMask saves = frame.calleeSavesMask();
    RegisterMask used = test.procedure().allocation().usedCalleeSaves();
    AOT_FRAME_TEST_CHECK(test, !file.calleeSavedIn(Bank::FP) && !(saves & ~file.calleeSavedIn(Bank::GP)));
    AOT_FRAME_TEST_CHECK(test, maskOf(frame.calleeSavesClobberedAtSuspensions()) == (saves & file.calleeSavedClobberedAtSuspensions));
#if CPU(ARM64)
    // emitMoveRegistersSavedInFrame() counts pairs from the end of the list.
    RegisterMask shortestRun = 0;
    for (size_t index = suspensionClobberedGPRs.size(); (used & ~shortestRun) && index; index -= 2)
        shortestRun |= maskOf(Reg(suspensionClobberedGPRs[index - 1])) | maskOf(Reg(suspensionClobberedGPRs[index - 2]));
    AOT_FRAME_TEST_CHECK(test, saves == shortestRun && !(used & ~saves));
#elif CPU(X86_64)
    AOT_FRAME_TEST_CHECK(test, saves == used && !(saves & ~(maskOf(Reg(SavedFrame::preservedRegister)) | maskOf(Reg(suspensionClobberedGPRs[0])))));
#endif
}

static void checkFrameTestPartsOfSavedFrame(TestProcedure& test)
{
    Procedure& proc = test.procedure();
    const Frame& frame = proc.frame();
    intptr_t top = topOfFrameTestSlots(frame);
    intptr_t bottomOfKeptPart = static_cast<intptr_t>(frame.bytesDeadAtSuspensions()) - static_cast<intptr_t>(frame.bytesBelowFrameRecord());
    AOT_FRAME_TEST_CHECK(test, top - bottomOfKeptPart == static_cast<intptr_t>(proc.slots().bytesKeptAtSuspensions()));
    AOT_FRAME_TEST_CHECK(test, proc.slots().numberOfWordsKeptAtSuspensions() * sizeof(uint64_t) == proc.slots().bytesKeptAtSuspensions());

    std::span<const SpillFacts> spills = proc.allocation().spills();
    for (SpillIndex index = 0; index < spills.size(); ++index) {
        intptr_t offset = frame.offsetFromFPOf(index);
        if (spills[index].livesAcrossSuspension)
            AOT_FRAME_TEST_CHECK(test, offset >= bottomOfKeptPart);
        else
            AOT_FRAME_TEST_CHECK(test, offset + static_cast<intptr_t>(bytesForWidth(spills[index].width)) <= bottomOfKeptPart);
    }
    for (const StackSlot* slot : proc.stackSlots()) {
        if (slot->isDeadAtSuspensions())
            AOT_FRAME_TEST_CHECK(test, slot->offsetFromFP() + static_cast<intptr_t>(slot->byteSize()) <= bottomOfKeptPart);
        else
            AOT_FRAME_TEST_CHECK(test, slot->offsetFromFP() >= bottomOfKeptPart);
    }
    // A call that returns ahead of its first suspension pays nothing for the words that suspensions keep.
    AOT_FRAME_TEST_CHECK(test, frame.beginOfWordsClearedAtSetup() == frame.endOfWordsClearedAtSetup() && !test.printedFrame().contains("wordsClearedAtSetup"_s));
}

// Answers how many words of the part that suspensions keep are listed as dead, over all suspensions.
// The range of `valueNotDefinedOnEveryPath` may reach over a suspension at which the value is not defined. Its word may be cleared there.
static unsigned checkFrameTestDeadWords(TestProcedure& test, const Operation* valueNotDefinedOnEveryPath = nullptr)
{
    Procedure& proc = test.procedure();
    const Frame& frame = proc.frame();
    std::span<const SpillFacts> spills = proc.allocation().spills();
    unsigned numberOfSaves = frame.numberOfCalleeSaves();
    unsigned numberOfWordsKept = (frame.bytesBelowFrameRecord() - frame.bytesDeadAtSuspensions()) / bytesOfFrameTestWord;
    unsigned numberOfDeadWords = 0;
    String text = test.printedFrame();

    std::span<PatchOperation* const> suspensions = proc.allocation().suspensions();
    for (uint32_t indexOfSuspension = 0; indexOfSuspension < suspensions.size(); ++indexOfSuspension) {
        const PatchOperation* suspension = suspensions[indexOfSuspension];
        // A range goes by the order. Whether a path leads on from the suspension to a read is for the run to show: what a suspension does not hold comes back as poison.
        const BitSpan& spillsHeld = proc.allocation().spillsHeldAt(indexOfSuspension);
        const std::optional<BitSpan>& deadWords = suspension->frameWordsDeadAcrossSuspension();
        Position early = proc.order().earlyPositionOf(suspension);
        Position late = proc.order().latePositionOf(suspension);
        auto isCleared = [&](unsigned location) {
            return isFrameTestWordClearedAt(frame, suspension, location);
        };
        AOT_FRAME_TEST_CHECK(test, deadWords && suspension->numberOfFrameWordsToClearAtFirstSuspension() <= SavedFrame::maxClearedWords);
        AOT_FRAME_TEST_CHECK(test, text.contains(makeString("suspension @"_s, suspension->index(), ": dead words ["_s)));
        for (unsigned location = 1; location <= numberOfSaves; ++location)
            AOT_FRAME_TEST_CHECK(test, !deadWords->get(location) && !isCleared(location));
        AOT_FRAME_TEST_CHECK(test, !(numberOfSaves % 2) || deadWords->get(numberOfSaves + 1));

        for (unsigned location = roundUpToMultipleOf<2>(numberOfSaves) + 1; location <= numberOfWordsKept; ++location) {
            bool isHeld = false;
            bool mayBeHeld = false;
            bool isHeldInFront = false;
            for (SpillIndex index = 0; index < spills.size(); ++index) {
                const SpillFacts& spill = spills[index];
                bool isHeldHere = spillsHeld.get(index);
                AOT_FRAME_TEST_CHECK(test, !isHeldHere || (spill.livesAcrossSuspension && spill.begin <= late && spill.end >= early));
                if (!frameTestWordHolds(location, frame.offsetFromFPOf(index), bytesForWidth(spill.width)))
                    continue;
                isHeld |= isHeldHere;
                mayBeHeld |= spill.begin <= late && spill.end >= early;
                isHeldInFront |= isHeldHere && spill.value != valueNotDefinedOnEveryPath && spill.begin < late;
            }
            for (const StackSlot* slot : proc.stackSlots()) {
                if (frameTestWordHolds(location, slot->offsetFromFP(), slot->byteSize()))
                    isHeld = mayBeHeld = isHeldInFront = true;
            }
            AOT_FRAME_TEST_CHECK(test, !isHeld || !deadWords->get(location));
            AOT_FRAME_TEST_CHECK(test, mayBeHeld || deadWords->get(location));
            // What the suspension defines is written behind it, so its word may be cleared.
            AOT_FRAME_TEST_CHECK(test, !isHeldInFront || !isCleared(location));
            numberOfDeadWords += deadWords->get(location);
        }
    }
    return numberOfDeadWords;
}

// More values live across the second suspension than places have become free by then, so some of their words are new: at the first suspension nothing has stored to those.
static void runFrameTestOfFunctionThatSavesItsFrame(unsigned numberAcrossCall, unsigned numberAcrossSecond, bool firstSuspensionGoesOn)
{
    constexpr uint64_t a = 5000;
    constexpr unsigned numberAcrossBoth = 2;
    constexpr unsigned numberAcrossFirst = 4;
    // Beside those four, a root that dies at the first suspension and a sum that is read behind it.
    constexpr unsigned mostPlacesFreeBehindFirst = numberAcrossFirst + 2;
    AOT_TEST_CHECK(numberAcrossSecond > mostPlacesFreeBehindFirst);
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    const AbstractHeap& frameSlots = test.heaps().frameSlots;
    proc.requestCallArgAreaSizeInBytes(3 * sizeof(uint64_t));
    StackSlot* keptSlot = proc.addStackSlot(2 * sizeof(uint64_t));
    StackSlot* deadSlot = proc.addStackSlot(sizeof(uint64_t));
    deadSlot->setIsDeadAtSuspensions(true);

    // Neither argument lives across anything, so nothing has been stored to the frame where its words are filled.
    Operation* seed = test.argumentGPR(0);
    Operation* argumentWithSums = test.argumentGPR(1);
    appendFrameTestLeftovers(out);
    Operation* sums = out.opaque(argumentWithSums);
    Vector<Operation*> acrossBoth = defineFrameTestValues(out, seed, numberAcrossBoth);
    Vector<Operation*> acrossFirst = defineFrameTestValues(out, out.add(seed, 100), numberAcrossFirst);
    Operation* rootThatDies = out.opaque(out.add(seed, 500));
    Operation* halfWordRoot = out.opaque(out.castToInt32(out.add(seed, 600)));
    out.store64(out.add(seed, 7), out.address(frameSlots, out.slotBase(keptSlot), sizeof(uint64_t)));
    out.store64(out.add(seed, 9), out.address(frameSlots, out.slotBase(deadSlot)));
    Vector<Operation*> acrossCall = defineFrameTestValues(out, out.add(seed, 200), numberAcrossCall);
    appendFrameTestCall(test);
    Operation* sumAcrossCall = addUpFrameTestValues(out, out.load64(out.address(frameSlots, out.slotBase(deadSlot))), acrossCall.span());

    Vector<Operation*> rootsOfFirst = acrossBoth;
    rootsOfFirst.appendVector(acrossFirst);
    rootsOfFirst.append(rootThatDies);
    rootsOfFirst.append(halfWordRoot);
    PatchOperation* first = appendFrameTestSuspension(test, sums, rootsOfFirst.span(), firstSuspensionGoesOn ? FrameTestSuspension::GoesOn : FrameTestSuspension::IsFirst);
    Operation* seedOfSecond = addUpFrameTestValues(out, sumAcrossCall, acrossFirst.span());
    Vector<Operation*> acrossSecond = defineFrameTestValues(out, seedOfSecond, numberAcrossSecond);

    Vector<Operation*> rootsOfSecond = acrossBoth;
    rootsOfSecond.appendVector(acrossSecond);
    rootsOfSecond.append(halfWordRoot);
    PatchOperation* second = appendFrameTestSuspension(test, sums, rootsOfSecond.span(), firstSuspensionGoesOn ? FrameTestSuspension::IsFirst : FrameTestSuspension::IsLater);
    Operation* result = out.load64(out.address(frameSlots, out.slotBase(keptSlot), sizeof(uint64_t)));
    result = addUpFrameTestValues(out, result, acrossBoth.span());
    result = addUpFrameTestValues(out, result, acrossSecond.span());
    out.ret(out.add(result, out.zeroExtPtr(halfWordRoot)));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::WholeFunction);
    AOT_FRAME_TEST_CHECK(test, allocation.suspensions().size() == 2 && allocation.suspensions()[0] == first && allocation.suspensions()[1] == second);
    checkFrameTestLayout(test);
    checkFrameTestSavesOfSavedFrame(test);
    checkFrameTestPartsOfSavedFrame(test);
    AOT_FRAME_TEST_CHECK(test, checkFrameTestDeadWords(test));

    // The new words lie at the bottom, so a number names them as far as it reaches. Stores clear the others.
    unsigned fewestNewWords = numberAcrossSecond - mostPlacesFreeBehindFirst;
    unsigned numberOfWordsToClear = first->numberOfFrameWordsToClearAtFirstSuspension();
    AOT_FRAME_TEST_CHECK(test, numberOfWordsToClear >= std::min<unsigned>(fewestNewWords, SavedFrame::maxClearedWords));
    AOT_FRAME_TEST_CHECK(test, numberOfWordsToClear + numberOfFrameTestWordsClearedInFrontOf(first) >= fewestNewWords);
    AOT_FRAME_TEST_CHECK(test, test.printedFrame().contains(makeString("], lowest words cleared at first suspension = "_s, numberOfWordsToClear)));
    AOT_FRAME_TEST_CHECK(test, (numberOfFrameTestWordsClearedInFrontOf(first) + numberOfFrameTestWordsClearedInFrontOf(second) > 0) == test.printedFrame().contains("words cleared in front = ["_s));

    unsigned numberOfSpillsKept = 0;
    for (const SpillFacts& spill : allocation.spills())
        numberOfSpillsKept += spill.livesAcrossSuspension;
    AOT_FRAME_TEST_CHECK(test, numberOfSpillsKept >= numberAcrossBoth + numberAcrossFirst);
    AOT_FRAME_TEST_CHECK(test, numberAcrossCall <= numberOfCalleeSavedGPRsInFrameTests() || numberOfSpillsKept < allocation.spills().size());

    uint64_t halfWord = static_cast<uint32_t>(a + 600);
    uint64_t expectedSeedOfSecond = a + 9 + sumOfFrameTestValues(a + 200, numberAcrossCall) + sumOfFrameTestValues(a + 100, numberAcrossFirst);
    uint64_t livingToTheEnd = sumOfFrameTestValues(a, numberAcrossBoth) + sumOfFrameTestValues(expectedSeedOfSecond, numberAcrossSecond) + halfWord;
    uint64_t expectedSumOfRoots = sumOfFrameTestValues(a, numberAcrossBoth) + sumOfFrameTestValues(a + 100, numberAcrossFirst) + a + 500 + halfWord + livingToTheEnd;
    std::array<uint64_t, 3> words { 0, poison, poison };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data()) == a + 7 + livingToTheEnd);
    AOT_FRAME_TEST_CHECK(test, words[0] == expectedSumOfRoots && !words[1]);
    // Where the first suspension is the first to suspend the function, it finds the new words as the earlier call left them.
    AOT_FRAME_TEST_CHECK(test, firstSuspensionGoesOn || words[2] >= std::min<unsigned>(fewestNewWords, SavedFrame::maxClearedWords) - 1);
}

static void testMachineFrameOfFunctionThatSavesItsFrame()
{
    for (unsigned numberAcrossCall = 0; numberAcrossCall <= numberOfCalleeSavedGPRsInFrameTests() + 2; ++numberAcrossCall) {
        for (unsigned numberAcrossSecond : { 12u, 30u }) {
            runFrameTestOfFunctionThatSavesItsFrame(numberAcrossCall, numberAcrossSecond, false);
            runFrameTestOfFunctionThatSavesItsFrame(numberAcrossCall, numberAcrossSecond, true);
        }
    }
}

// The arm stores to words that the way around it leaves as the earlier call left them. The suspension behind the join keeps them either way.
static void runFrameTestOfWordsStoredOnOnePath(bool takesArm)
{
    constexpr uint64_t a = 7000;
    constexpr unsigned numberInArm = 3;
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* argumentWithSeed = test.argumentGPR(0);
    Operation* argumentWithSums = test.argumentGPR(1);
    Operation* choice = test.argumentGPR(2);
    appendFrameTestLeftovers(out);
    Operation* seed = out.opaque(argumentWithSeed);
    Operation* sums = out.opaque(argumentWithSums);
    Block* arm = out.newBlock();
    Block* join = out.newBlock();
    ValueFromBlock fromEntry = out.anchor(out.int64Zero);
    out.branch(out.testNonZero64(choice, 1), normalEdge(arm), normalEdge(join));

    out.appendTo(arm);
    Vector<Operation*> inArm = defineFrameTestValues(out, seed, numberInArm);
    appendFrameTestSuspension(test, sums, inArm.span(), takesArm ? FrameTestSuspension::IsFirst : FrameTestSuspension::IsLater);
    ValueFromBlock fromArm = out.anchor(addUpFrameTestValues(out, out.int64Zero, inArm.span()));
    out.jump(join);

    out.appendTo(join);
    Operation* sumOfArm = out.phi(Int64, fromEntry, fromArm);
    Operation* behindJoin = out.opaque(out.add(seed, 50));
    appendFrameTestSuspension(test, sums, singleElementSpan(behindJoin), takesArm ? FrameTestSuspension::IsLater : FrameTestSuspension::IsFirst);
    out.ret(out.add(out.add(sumOfArm, behindJoin), seed));

    compileOrFail(test);
    AOT_FRAME_TEST_CHECK(test, test.procedure().allocation().suspensions().size() == 2);
    checkFrameTestLayout(test);
    checkFrameTestDeadWords(test);

    uint64_t expectedSumOfArm = takesArm ? sumOfFrameTestValues(a, numberInArm) : 0;
    std::array<uint64_t, 3> words { 0, poison, poison };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { takesArm }) == expectedSumOfArm + a + 50 + a);
    AOT_FRAME_TEST_CHECK(test, words[0] == expectedSumOfArm + a + 50 && !words[1]);
}

static void testMachineFrameClearsWordsStoredOnOnePath()
{
    runFrameTestOfWordsStoredOnOnePath(false);
    runFrameTestOfWordsStoredOnOnePath(true);
}

// In the order of allocation the shared block may stand between the definition of the value and its last read, though no path leads from the one through it to the other.
// The range of the value then reaches over the suspension there, which a path gets to without the store. Nothing reads the value from there on, so its word is cleared like one that nothing holds.
static void testMachineFrameKeepsNoLeftoverInWordHeldByRangeAlone()
{
    constexpr uint64_t a = 9000;
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* choice = test.argumentGPR(2);
    Operation* seed = out.opaque(test.argumentGPR(0));
    Operation* sums = out.opaque(test.argumentGPR(1));
    Block* defining = out.newBlock();
    Block* shared = out.newBlock();
    Block* reading = out.newBlock();
    out.branch(out.testNonZero64(choice, 1), normalEdge(defining), normalEdge(shared));

    out.appendTo(defining);
    Operation* value = out.opaque(out.add(seed, 1));
    out.branch(out.testNonZero64(choice, 2), unlikelyEdge(reading), normalEdge(shared));

    out.appendTo(shared);
    PatchOperation* suspensionInShared = appendFrameTestSuspension(test, sums, { }, FrameTestSuspension::IsFirst, a + 1);
    out.ret(seed);

    out.appendTo(reading);
    PatchOperation* suspensionInReading = appendFrameTestSuspension(test, sums, singleElementSpan(value), FrameTestSuspension::IsFirst, a + 1);
    out.ret(value);

    compileOrFail(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    const Frame& frame = test.procedure().frame();
    AOT_FRAME_TEST_CHECK(test, allocation.suspensions().size() == 2);
    checkFrameTestLayout(test);
    checkFrameTestPartsOfSavedFrame(test);
    checkFrameTestDeadWords(test, value);

    AOT_FRAME_TEST_CHECK(test, allocation.isSpilled(value));
    const SpillFacts& spill = allocation.spills()[allocation.spillOf(value)];
    unsigned location = static_cast<unsigned>(-frame.offsetFromFPOf(allocation.spillOf(value)) / bytesOfFrameTestWord);
    bool rangeReachesOverShared = spill.begin < order.latePositionOf(suspensionInShared) && spill.end >= order.earlyPositionOf(suspensionInShared);
    AOT_FRAME_TEST_CHECK(test, !isFrameTestWordClearedAt(frame, suspensionInReading, location));
    AOT_FRAME_TEST_CHECK(test, !rangeReachesOverShared || isFrameTestWordClearedAt(frame, suspensionInShared, location));

    // The first run leaves the value where the frame of the second one comes to lie.
    std::array<uint64_t, 3> words { 0, poison, poison };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 3 }) == a + 1 && words[0] == a + 1);
    words = { 0, poison, poison };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 0 }) == a && !words[0] && !words[1]);
}

// The block that reads the value stands behind the block with the suspension, as a landing pad stands behind every block that leads to it. So the range of the value reaches over the suspension.
// Every path to the suspension has stored the value, and none leads on from it to the read. A suspension that kept the word would put the value back on the stack with every resumption.
static void testMachineFrameKeepsNoSpillThatNoPathReadsBehindSuspension()
{
    constexpr uint64_t a = 13000;
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* choice = test.argumentGPR(2);
    Operation* seed = out.opaque(test.argumentGPR(0));
    Operation* sums = out.opaque(test.argumentGPR(1));
    Operation* value = out.opaque(out.add(seed, 1));
    Block* goingOn = out.newBlock();
    Block* reading = out.newBlock();
    out.branch(out.testNonZero64(choice, 1), unlikelyEdge(reading), normalEdge(goingOn));

    out.appendTo(goingOn);
    PatchOperation* suspension = appendFrameTestSuspension(test, sums, { });
    out.ret(seed);

    out.appendTo(reading);
    appendFrameTestCall(test);
    out.ret(value);

    compileOrFail(test);
    const Order& order = test.procedure().order();
    const Allocation& allocation = test.procedure().allocation();
    AOT_FRAME_TEST_CHECK(test, allocation.suspensions().size() == 1 && allocation.suspensions()[0] == suspension);
    checkFrameTestLayout(test);
    checkFrameTestPartsOfSavedFrame(test);
    checkFrameTestDeadWords(test);

    AOT_FRAME_TEST_CHECK(test, allocation.isSpilled(value));
    const SpillFacts& spill = allocation.spills()[allocation.spillOf(value)];
    AOT_FRAME_TEST_CHECK(test, spill.begin < order.earlyPositionOf(suspension) && spill.end > order.latePositionOf(suspension));
    AOT_FRAME_TEST_CHECK(test, !spill.livesAcrossSuspension && !allocation.spillsHeldAt(0).get(allocation.spillOf(value)));

    std::array<uint64_t, 3> words { 0, poison, poison };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 1 }) == a + 1 && !words[0]);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 0 }) == a && !words[0]);
}

// More spills than a word of a set has bits. The block of the suspension names none of them. Those that are read behind the join are live through it. Those that the other way alone reads are not.
static void testMachineFrameKeepsManySpillsThatLiveThroughBlockOfSuspension()
{
    constexpr uint64_t a = 17000;
    constexpr unsigned numberOfEach = 100;
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* choice = out.opaque(test.argumentGPR(2));
    Operation* seed = out.opaque(test.argumentGPR(0));
    Operation* sums = out.opaque(test.argumentGPR(1));
    Vector<Operation*> readBehindJoin = defineFrameTestValues(out, seed, numberOfEach);
    Vector<Operation*> readOnOtherWay = defineFrameTestValues(out, out.add(seed, 1000), numberOfEach);
    Block* suspending = out.newBlock();
    Block* reading = out.newBlock();
    Block* join = out.newBlock();
    out.branch(out.testNonZero64(choice, 1), unlikelyEdge(reading), normalEdge(suspending));

    out.appendTo(suspending);
    PatchOperation* suspension = appendFrameTestSuspension(test, sums, { });
    ValueFromBlock fromSuspending = out.anchor(out.int64Zero);
    out.jump(join);

    out.appendTo(reading);
    appendFrameTestCall(test);
    ValueFromBlock fromReading = out.anchor(addUpFrameTestValues(out, out.int64Zero, readOnOtherWay.span()));
    out.jump(join);

    out.appendTo(join);
    Operation* sumOfOtherWay = out.phi(Int64, fromSuspending, fromReading);
    out.ret(addUpFrameTestValues(out, sumOfOtherWay, readBehindJoin.span()));

    compileOrFail(test);
    const Allocation& allocation = test.procedure().allocation();
    AOT_FRAME_TEST_CHECK(test, allocation.suspensions().size() == 1 && allocation.suspensions()[0] == suspension);
    checkFrameTestLayout(test);
    checkFrameTestDeadWords(test);
    AOT_FRAME_TEST_CHECK(test, allocation.spills().size() > 64);
    for (Operation* value : readBehindJoin)
        AOT_FRAME_TEST_CHECK(test, allocation.isSpilled(value) && allocation.spillsHeldAt(0).get(allocation.spillOf(value)) && allocation.spills()[allocation.spillOf(value)].livesAcrossSuspension);
    for (Operation* value : readOnOtherWay)
        AOT_FRAME_TEST_CHECK(test, !allocation.isSpilled(value) || !allocation.spillsHeldAt(0).get(allocation.spillOf(value)));

    std::array<uint64_t, 3> words { 0, poison, poison };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 0 }) == sumOfFrameTestValues(a, numberOfEach));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 1 }) == sumOfFrameTestValues(a, numberOfEach) + sumOfFrameTestValues(a + 1000, numberOfEach));
}

// More spills than a word of a set has bits, defined in front of a loop. The block with the suspension reads every second one and names none of the others, which are read behind the loop.
// So the block is left by its head for the ones that it reads, and then the edge that leads back brings it the others.
static void testMachineFrameKeepsManySpillsThatLiveRoundLoopWithSuspension()
{
    constexpr uint64_t a = 19000;
    constexpr unsigned numberOfEach = 100;
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* rounds = out.opaque(test.argumentGPR(2));
    Operation* seed = out.opaque(test.argumentGPR(0));
    Operation* sums = out.opaque(test.argumentGPR(1));
    Vector<Operation*> readInFrontOfLoop = defineFrameTestValues(out, out.add(seed, 1000), numberOfEach);
    Operation* sumInFrontOfLoop = addUpFrameTestValues(out, out.int64Zero, readInFrontOfLoop.span());
    Vector<Operation*> readInLoop;
    Vector<Operation*> readBehindLoop;
    for (unsigned index = 0; index < numberOfEach; ++index) {
        readInLoop.append(out.opaque(out.add(seed, 2 * index + 1)));
        readBehindLoop.append(out.opaque(out.add(seed, 2 * index + 2)));
    }
    Block* head = out.newBlock();
    Block* suspending = out.newBlock();
    Block* latch = out.newBlock();
    Block* end = out.newBlock();
    ValueFromBlock roundsAtEntry = out.anchor(rounds);
    ValueFromBlock sumAtEntry = out.anchor(sumInFrontOfLoop);
    out.jump(head);

    out.appendTo(head);
    Operation* roundsLeft = out.phi(Int64, roundsAtEntry);
    Operation* sum = out.phi(Int64, sumAtEntry);
    out.branch(roundsLeft, normalEdge(suspending), normalEdge(end));

    out.appendTo(suspending);
    PatchOperation* suspension = appendFrameTestSuspension(test, sums, { });
    Operation* sumOfRound = addUpFrameTestValues(out, sum, readInLoop.span());
    out.jump(latch);

    out.appendTo(latch);
    out.addIncomingToPhi(roundsLeft, out.anchor(out.sub(roundsLeft, 1)));
    out.addIncomingToPhi(sum, out.anchor(sumOfRound));
    out.jump(head);

    out.appendTo(end);
    out.ret(addUpFrameTestValues(out, sum, readBehindLoop.span()));

    compileOrFail(test);
    const Allocation& allocation = test.procedure().allocation();
    AOT_FRAME_TEST_CHECK(test, allocation.suspensions().size() == 1 && allocation.suspensions()[0] == suspension);
    checkFrameTestLayout(test);
    checkFrameTestDeadWords(test);
    AOT_FRAME_TEST_CHECK(test, allocation.spills().size() > 128);
    // The suspension poisons every register that it loses and every word that it does not keep, so a value that is not kept shows in the sum.
    constexpr uint64_t sumOfWhatLoopReads = numberOfEach * a + numberOfEach * numberOfEach;
    constexpr uint64_t sumOfWhatIsReadBehindLoop = numberOfEach * a + numberOfEach * (numberOfEach + 1);
    for (uint64_t numberOfRounds : { 0, 1, 3 }) {
        std::array<uint64_t, 3> words { 0, poison, poison };
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), numberOfRounds) == sumOfFrameTestValues(a + 1000, numberOfEach) + numberOfRounds * sumOfWhatLoopReads + sumOfWhatIsReadBehindLoop);
    }

    // A suspension leaves one register as it is. The value that goes round the loop in it has no spill, or one that is written behind the loop.
    unsigned numberKeptOutsideSpills = 0;
    for (const Vector<Operation*>& values : { readInLoop, readBehindLoop }) {
        for (Operation* value : values) {
            bool isHeld = allocation.isSpilled(value) && allocation.spillsHeldAt(0).get(allocation.spillOf(value));
            AOT_FRAME_TEST_CHECK(test, !isHeld || allocation.spills()[allocation.spillOf(value)].livesAcrossSuspension);
            numberKeptOutsideSpills += !isHeld;
        }
    }
    AOT_FRAME_TEST_CHECK(test, numberKeptOutsideSpills <= 1);
    for (Operation* value : readInFrontOfLoop)
        AOT_FRAME_TEST_CHECK(test, !allocation.isSpilled(value) || !allocation.spillsHeldAt(0).get(allocation.spillOf(value)));
}

// On x86-64.
static bool oneByteReachesFrameTestSpill(TestProcedure& test, const Operation* value)
{
    const Frame& frame = test.procedure().frame();
    MacroAssembler::Address address = frame.addressInFrame(frame.offsetFromFPOf(test.procedure().allocation().spillOf(value)));
    return isRepresentableAs<int8_t>(static_cast<int64_t>(address.offset));
}

// A frame that is saved, each part of it larger than one byte of offset reaches.
//  - Three instructions name a StackSlot of twenty words that suspensions keep and that was made first. It gives way to the words that more instructions name, and what it holds is read back behind both suspensions.
//  - A StackSlot of two words that no suspension keeps is named in front of and behind each of twelve calls, so the stack pointer reaches it.
//  - One of four half words that suspensions keep is read behind each of twelve calls. The one that shares its word goes with it.
//  - The later half of the values that suspensions keep is read once more than the earlier half. Those out of reach still lie in the order in which they were spilled.
static void testMachineFrameThatIsSavedMovesStackSlotsAndHalfWordsWhole()
{
    constexpr uint64_t a = 31000;
    constexpr unsigned numberOfEach = 40;
    constexpr unsigned numberOfHalfWords = 4;
    constexpr unsigned busyHalfWord = 2;
    constexpr unsigned neighborOfBusyHalfWord = 3;
    constexpr unsigned numberOfReads = 12;
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    StackSlot* keptSlot = proc.addStackSlot(20 * sizeof(uint64_t));
    StackSlot* busySlot = proc.addStackSlot(2 * sizeof(uint64_t));
    busySlot->setIsDeadAtSuspensions(true);
    auto wordOfKeptSlot = [&] {
        return out.address(test.heaps().frameSlots, out.slotBase(keptSlot), 7 * sizeof(uint64_t));
    };
    auto wordOfBusySlot = [&] {
        return out.address(test.heaps().frameSlots, out.slotBase(busySlot), sizeof(uint64_t));
    };

    Operation* argumentWithSeed = test.argumentGPR(0);
    Operation* argumentWithSums = test.argumentGPR(1);
    appendFrameTestLeftovers(out);
    Operation* seed = out.opaque(argumentWithSeed);
    Operation* sums = out.opaque(argumentWithSums);
    out.store64(out.add(seed, 7), wordOfKeptSlot());
    Vector<Operation*> kept = defineFrameTestValues(out, seed, numberOfEach);
    std::span<Operation* const> readOnceMore = kept.span().subspan(numberOfEach / 2);
    Vector<Operation*> halfWords;
    for (unsigned index = 0; index < numberOfHalfWords; ++index)
        halfWords.append(out.opaque(out.castToInt32(out.add(seed, 600 + index))));
    Vector<Operation*> acrossCalls = defineFrameTestValues(out, out.add(seed, 1000), numberOfEach);
    appendFrameTestCall(test);
    Operation* sum = addUpFrameTestValues(out, out.int64Zero, acrossCalls.span());
    for (unsigned read = 0; read < numberOfReads; ++read) {
        out.store64(sum, wordOfBusySlot());
        appendFrameTestCall(test);
        sum = out.add(out.load64(wordOfBusySlot()), 1);
    }

    Vector<Operation*> roots = kept;
    roots.appendVector(halfWords);
    PatchOperation* first = appendFrameTestSuspension(test, sums, roots.span(), FrameTestSuspension::IsFirst);
    sum = addUpFrameTestValues(out, out.add(sum, out.load64(wordOfKeptSlot())), readOnceMore);
    for (unsigned read = 0; read < numberOfReads; ++read) {
        appendFrameTestCall(test);
        sum = out.add(sum, out.zeroExtPtr(halfWords[busyHalfWord]));
    }
    PatchOperation* second = appendFrameTestSuspension(test, sums, roots.span());
    sum = addUpFrameTestValues(out, out.add(sum, out.load64(wordOfKeptSlot())), kept.span());
    for (Operation* halfWord : halfWords)
        sum = out.add(sum, out.zeroExtPtr(halfWord));
    out.ret(sum);

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    const Frame& frame = proc.frame();
    AOT_FRAME_TEST_CHECK(test, allocation.suspensions().size() == 2 && allocation.suspensions()[0] == first && allocation.suspensions()[1] == second);
    checkFrameTestLayout(test);
    checkFrameTestSavesOfSavedFrame(test);
    checkFrameTestPartsOfSavedFrame(test);
    checkFrameTestDeadWords(test);

    auto offsetOf = [&](const Operation* value) {
        return frame.offsetFromFPOf(allocation.spillOf(value));
    };
    AOT_FRAME_TEST_CHECK(test, allocation.isSpilled(halfWords[busyHalfWord]) && allocation.isSpilled(halfWords[neighborOfBusyHalfWord]));
    AOT_FRAME_TEST_CHECK(test, std::abs(offsetOf(halfWords[busyHalfWord]) - offsetOf(halfWords[neighborOfBusyHalfWord])) == static_cast<int32_t>(sizeof(uint32_t)));
    AOT_FRAME_TEST_CHECK(test, (7 - offsetOf(halfWords[busyHalfWord])) / bytesOfFrameTestWord == (7 - offsetOf(halfWords[neighborOfBusyHalfWord])) / bytesOfFrameTestWord);
    if (isX86_64()) {
        AOT_FRAME_TEST_CHECK(test, keptSlot->offsetFromFP() + static_cast<intptr_t>(keptSlot->byteSize()) <= -128);
        MacroAssembler::Address lastWordOfBusySlot = frame.addressInFrame(busySlot->offsetFromFP() + bytesOfFrameTestWord);
        AOT_FRAME_TEST_CHECK(test, lastWordOfBusySlot.base == MacroAssembler::stackPointerRegister && isRepresentableAs<int8_t>(static_cast<int64_t>(lastWordOfBusySlot.offset)));
        AOT_FRAME_TEST_CHECK(test, offsetOf(halfWords[busyHalfWord]) >= -128 && offsetOf(halfWords[neighborOfBusyHalfWord]) >= -128);

        unsigned numberOutOfReach = 0;
        unsigned numberReadOnceMoreOutOfReach = 0;
        int32_t offsetOfLastOutOfReach = 0;
        for (unsigned index = 0; index < numberOfEach; ++index) {
            if (!allocation.isSpilled(kept[index]) || oneByteReachesFrameTestSpill(test, kept[index]))
                continue;
            AOT_FRAME_TEST_CHECK(test, offsetOf(kept[index]) < offsetOfLastOutOfReach);
            offsetOfLastOutOfReach = offsetOf(kept[index]);
            ++numberOutOfReach;
            numberReadOnceMoreOutOfReach += index >= numberOfEach / 2;
        }
        AOT_FRAME_TEST_CHECK(test, numberReadOnceMoreOutOfReach && numberOutOfReach > numberReadOnceMoreOutOfReach);
    }

    uint64_t sumOfKept = sumOfFrameTestValues(a, numberOfEach);
    uint64_t sumOfHalfWords = 0;
    for (unsigned index = 0; index < numberOfHalfWords; ++index)
        sumOfHalfWords += static_cast<uint32_t>(a + 600 + index);
    uint64_t expected = sumOfFrameTestValues(a + 1000, numberOfEach) + numberOfReads + 2 * (a + 7) + sumOfFrameTestValues(a + numberOfEach / 2, numberOfEach / 2)
        + numberOfReads * static_cast<uint32_t>(a + 600 + busyHalfWord) + sumOfKept + sumOfHalfWords;
    std::array<uint64_t, 3> words { 0, poison, poison };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data()) == expected);
    AOT_FRAME_TEST_CHECK(test, words[0] == 2 * (sumOfKept + sumOfHalfWords) && !words[1]);
}

// An immediate in the code of a suspension holds the index of the word of one operand. That operand is defined last and read least, so by the order of the spills and by its readers alike it would lie far down.
// More instructions name the StackSlot than any word, so it lies in reach of the frame pointer, and the operand still comes first.
static void testMachineFrameWordThatSuspensionNamesByNumberComesFirst()
{
    constexpr uint64_t a = 29000;
    constexpr unsigned numberOfOthers = 40;
    constexpr unsigned numberOfRounds = 15;
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    StackSlot* busySlot = test.procedure().addStackSlot(2 * sizeof(uint64_t));
    auto wordOfBusySlot = [&] {
        return out.address(test.heaps().frameSlots, out.slotBase(busySlot), sizeof(uint64_t));
    };
    Operation* seed = out.opaque(test.argumentGPR(0));
    Operation* sums = out.opaque(test.argumentGPR(1));
    Vector<Operation*> roots = defineFrameTestValues(out, seed, numberOfOthers);
    Operation* named = out.opaque(out.add(seed, 500));
    roots.append(named);
    // The first operand is `sums`.
    constexpr unsigned childThatIsNamed = numberOfOthers + 1;
    PatchOperation* suspension = appendFrameTestSuspension(test, sums, roots.span());
    suspension->setOperandEncodedAsFrameWordIndex(childThatIsNamed);
    Operation* sum = addUpFrameTestValues(out, out.int64Zero, roots.span());
    for (unsigned round = 0; round < numberOfRounds; ++round) {
        out.store64(sum, wordOfBusySlot());
        appendFrameTestCall(test);
        sum = out.add(out.load64(wordOfBusySlot()), 1);
    }
    out.ret(addUpFrameTestValues(out, sum, roots.span().first(numberOfOthers)));

    compileOrFail(test);
    const Allocation& allocation = test.procedure().allocation();
    const Frame& frame = test.procedure().frame();
    checkFrameTestLayout(test);
    checkFrameTestDeadWords(test);
    Location placeOfNamed;
    for (const Step& step : allocation.steps()) {
        if (step.kind == StepKind::Execute && step.operation == suspension)
            placeOfNamed = allocation.patchAssignment(step.patchAssignment).locations[childThatIsNamed];
    }
    AOT_FRAME_TEST_CHECK(test, placeOfNamed.isSpill() && placeOfNamed.spillIndex() == allocation.spillOf(named));
    AOT_FRAME_TEST_CHECK(test, test.procedure().slots().numberOfWordsKeptAtSuspensions() >= numberOfOthers);
    if (isX86_64()) {
        AOT_FRAME_TEST_CHECK(test, frame.offsetFromFPOf(placeOfNamed.spillIndex()) == topOfFrameTestSlots(frame) - bytesOfFrameTestWord);
        AOT_FRAME_TEST_CHECK(test, busySlot->offsetFromFP() >= -128);
    }

    uint64_t sumOfOthers = sumOfFrameTestValues(a, numberOfOthers);
    std::array<uint64_t, 3> words { 0, poison, poison };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data()) == 2 * sumOfOthers + a + 500 + numberOfRounds);
    AOT_FRAME_TEST_CHECK(test, words[0] == sumOfOthers + a + 500);
}

// A root from outside a rare region that dies at the suspension there is saved in the region, wherever it loses its register. No position says that every path has written its place,
// so the word can be neither relied on nor cleared at the suspension.
static void testMachineFrameClearsAtSetupWordsThatSavesWrite()
{
    constexpr uint64_t a = 11000;
    constexpr uint64_t answer = 5;
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* argumentWithSeed = test.argumentGPR(0);
    Operation* argumentWithSums = test.argumentGPR(1);
    Operation* choice = test.argumentGPR(2);
    appendFrameTestLeftovers(out);
    Operation* value = out.opaque(out.add(argumentWithSeed, 1));
    Operation* sums = out.opaque(argumentWithSums);
    // Both are read outside the region too, so neither sinks into it.
    out.store64(value, out.address(test.heaps().argumentBuffer, sums, 3 * sizeof(uint64_t)));
    Block* rare = out.newBlock();
    Block* end = out.newBlock();
    out.branch(out.testNonZero64(choice, 1), rareEdge(rare), normalEdge(end));

    out.appendTo(rare);
    appendFrameTestSuspension(test, sums, singleElementSpan(value), FrameTestSuspension::IsFirst);
    out.jump(end);

    out.appendTo(end);
    out.ret(out.constInt64(answer));

    compileOrFail(test);
    const Frame& frame = test.procedure().frame();
    std::span<const SpillFacts> spills = test.procedure().allocation().spills();
    checkFrameTestLayout(test);
    checkFrameTestDeadWords(test);

    unsigned numberOfSavesKept = 0;
    for (SpillIndex index = 0; index < spills.size(); ++index) {
        if (!spills[index].isSaveInRareRegion || !spills[index].livesAcrossSuspension)
            continue;
        ++numberOfSavesKept;
        AOT_FRAME_TEST_CHECK(test, frame.offsetFromFPOf(index) >= frame.beginOfWordsClearedAtSetup() && frame.offsetFromFPOf(index) < frame.endOfWordsClearedAtSetup());
    }
    AOT_FRAME_TEST_CHECK(test, numberOfSavesKept && test.printedFrame().contains("wordsClearedAtSetup = ["_s));

    std::array<uint64_t, 4> words { 0, poison, poison, 0 };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 1 }) == answer);
    AOT_FRAME_TEST_CHECK(test, words[0] == a + 1 && !words[1] && words[3] == a + 1);
    words = { 0, poison, poison, 0 };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 0 }) == answer);
    AOT_FRAME_TEST_CHECK(test, !words[0] && words[1] == poison && words[3] == a + 1);
}

static SpillIndex findFrameTestPointerBeside(const Allocation& allocation, const Operation* reference)
{
    SpillIndex result = noSpill;
    std::span<const SpillFacts> spills = allocation.spills();
    for (SpillIndex index = 0; index < spills.size(); ++index) {
        if (!spills[index].isPointerBesideNarrowReference || spills[index].value != reference)
            continue;
        AOT_TEST_CHECK(result == noSpill);
        result = index;
    }
    return result;
}

enum class FrameTestReference : uint8_t {
    LivesAcrossSafepoint,
    DiesAheadOfSafepoint,
    LivesAcrossPatchThatCannotCollect,
    NamesPermanentCell,
    IsNotMarked,
};

static bool frameTestReferenceGetsPointer(FrameTestReference kind)
{
    TestProcedure test;
    Output& out = test.out();
    Operation* argument = test.argumentGPR(0);
    Operation* reference;
    if (kind == FrameTestReference::NamesPermanentCell) {
        reference = out.load32(out.address(AbstractHeap(test.heaps().argumentBuffer.index(), 0, HeapTrait::HoldsPermanentCell), argument));
        AOT_TEST_CHECK(reference->has(OperationFlag::NamesPermanentCell));
    } else
        reference = out.opaque(out.castToInt32(argument));
    if (kind != FrameTestReference::IsNotMarked)
        reference->markAsNarrowReference();
    // A copy, an extension, a shift or a sum still names the cell, and the reference lives as long as it does. One bit of it names nothing.
    Operation* returned = kind == FrameTestReference::DiesAheadOfSafepoint ? out.opaque(out.bitAnd(reference, 1)) : reference;
    PatchOperation* call = appendFrameTestCall(test);
    call->effects.mayCollect = kind != FrameTestReference::LivesAcrossPatchThatCannotCollect;
    out.ret(out.zeroExtPtr(returned));

    prepareOrFail(test);
    bool needsPointer = test.procedure().liveness().of(reference).has(ValueFlag::NeedsPointerBesideIt);
    AOT_FRAME_TEST_CHECK(test, needsPointer == (findFrameTestPointerBeside(test.procedure().allocation(), reference) != noSpill));
    return needsPointer;
}

static void testMachineFrameKeepsPointerOnlyWhereCollectorNeedsIt()
{
    AOT_TEST_CHECK(frameTestReferenceGetsPointer(FrameTestReference::LivesAcrossSafepoint));
    AOT_TEST_CHECK(!frameTestReferenceGetsPointer(FrameTestReference::DiesAheadOfSafepoint));
    AOT_TEST_CHECK(!frameTestReferenceGetsPointer(FrameTestReference::LivesAcrossPatchThatCannotCollect));
    AOT_TEST_CHECK(!frameTestReferenceGetsPointer(FrameTestReference::NamesPermanentCell));
    AOT_TEST_CHECK(!frameTestReferenceGetsPointer(FrameTestReference::IsNotMarked));
}

// The sum sinks to the head of a rare block, ahead of a safepoint there, and takes in its shift and its extension. The rules of the collector come to it behind that.
static void testMachineFrameSunkSumOfNarrowReferenceLivesAcrossSafepoint()
{
    constexpr uint32_t word = 0x80000100u;
    for (int64_t offset : { int64_t { 0 }, int64_t { 0x800000 } }) {
        TestProcedure test;
        Output& out = test.out();
        Operation* buffer = test.argumentGPR(0);
        Operation* selector = test.argumentGPR(1);
        Operation* reference = out.load32(out.address(test.heaps().argumentBuffer, buffer));
        reference->markAsNarrowReference();
        Operation* base = out.add(out.argumentRegister(cellCageBaseGPR), out.constIntPtr(offset));
        Operation* extended = out.zeroExtPtr(reference);
        Operation* sum = out.add(base, out.shl(extended, cell32Shift));
        Block* slow = out.newBlock();
        Block* end = out.newBlock();
        out.branch(out.testNonZero64(selector, 1), rareEdge(slow), normalEdge(end));

        out.appendTo(slow);
        appendFrameTestCall(test)->effects.mayCollect = true;
        out.store64(sum, out.address(test.heaps().argumentBuffer, buffer, 8));
        out.jump(end);

        out.appendTo(end);
        out.ret(selector);
        compileOrFail(test);

        const Liveness& liveness = test.procedure().liveness();
        AOT_FRAME_TEST_CHECK(test, liveness.isSunk(sum) && addsScaledIndex(liveness, sum) && liveness.isFused(extended) && !liveness.isFused(reference));
        AOT_FRAME_TEST_CHECK(test, liveness.of(sum).has(ValueFlag::LivesAcrossSafepoint));
        AOT_FRAME_TEST_CHECK(test, liveness.of(reference).has(ValueFlag::NeedsPointerBesideIt) == !!offset);
        AOT_FRAME_TEST_CHECK(test, (findFrameTestPointerBeside(test.procedure().allocation(), reference) != noSpill) == !!offset);
        for (uint64_t goesSlowWay : { 0u, 1u }) {
            std::array<uint64_t, 2> words { (uint64_t { 7 } << 32) + word, 0 };
            AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(words.data(), goesSlowWay) == goesSlowWay);
            AOT_FRAME_TEST_CHECK(test, words[1] == (goesSlowWay ? (uint64_t { word } << cell32Shift) + static_cast<uint64_t>(offset) : 0));
        }
    }
}

// The owner is read for the last time ahead of the call. What it owns is read behind it.
static void runFrameTestOfPointerIntoStorage(bool heapHoldsInnerPointer)
{
    std::array<uint64_t, 3> elements { 11, 22, 33 };
    std::array<const uint64_t*, 1> ownerInMemory { elements.data() };

    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    OptionSet<HeapTrait> traits;
    if (heapHoldsInnerPointer)
        traits.add(HeapTrait::HoldsInnerPointer);
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, traits);
    Operation* argumentWithOwner = test.argumentGPR(0);
    Operation* index = test.argumentGPR(1);
    Operation* owner = out.opaque(argumentWithOwner);
    Operation* storage = out.loadPtr(out.address(storageOfOwner, owner));
    Operation* element = out.add(storage, out.shl(index, 3));
    PatchOperation* call = appendFrameTestCall(test);
    Operation* read = out.load64(out.address(test.heaps().argumentBuffer, element));
    out.ret(read);

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    const ValueFacts& facts = proc.liveness().of(owner);
    AOT_FRAME_TEST_CHECK(test, storage->has(OperationFlag::IsInnerPointer) == heapHoldsInnerPointer && element->has(OperationFlag::IsInnerPointer) == heapHoldsInnerPointer);
    AOT_FRAME_TEST_CHECK(test, facts.lastUse != noPosition);
    if (heapHoldsInnerPointer) {
        AOT_FRAME_TEST_CHECK(test, facts.lastUse >= proc.order().earlyPositionOf(read) && facts.has(ValueFlag::LivesAcrossCall));
        AOT_FRAME_TEST_CHECK(test, allocation.isSpilled(owner) || (maskOf(allocation.registerAtDefinitionOf(owner)) & registerFileOfFrameTest(test).calleeSaved));
    } else
        AOT_FRAME_TEST_CHECK(test, facts.lastUse < proc.order().earlyPositionOf(call) && !facts.has(ValueFlag::LivesAcrossCall) && !allocation.isSpilled(owner));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 2 }) == 33);
}

static void testMachineFrameOwnerOutlivesInnerPointer()
{
    runFrameTestOfPointerIntoStorage(true);
    runFrameTestOfPointerIntoStorage(false);
}

enum class FrameTestAddressCarrier : uint8_t {
    OpaqueCopy,
    Selection,
    JoinOfForwardEdges,
    HeadOfLoop,
};

// None of the carriers is a sum, and each has a range of its own.
static std::optional<FailureKind> failureOfFrameTestWithCarriedAddress(FrameTestAddressCarrier carrier, bool isReadBehindSuspension)
{
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    const AbstractHeap& frameSlots = test.heaps().frameSlots;
    Operation* value = test.argumentGPR(0);
    Operation* index = test.argumentGPR(1);
    Operation* sums = out.opaque(test.argumentGPR(2));
    Operation* otherPointer = test.argumentGPR(3);
    Operation* slot = out.lockedStackSlot(2 * sizeof(uint64_t));

    Operation* address = nullptr;
    switch (carrier) {
    case FrameTestAddressCarrier::OpaqueCopy:
        address = out.opaque(slot);
        break;
    case FrameTestAddressCarrier::Selection:
        address = out.select(out.testNonZero64(index, 1), slot, otherPointer);
        break;
    case FrameTestAddressCarrier::JoinOfForwardEdges: {
        Block* whenOdd = out.newBlock();
        Block* join = out.newBlock();
        ValueFromBlock fromEntry = out.anchor(otherPointer);
        out.branch(out.testNonZero64(index, 1), normalEdge(whenOdd), normalEdge(join));
        out.appendTo(whenOdd);
        ValueFromBlock fromArm = out.anchor(slot);
        out.jump(join);
        out.appendTo(join);
        address = out.phi(Int64, fromEntry, fromArm);
        break;
    }
    case FrameTestAddressCarrier::HeadOfLoop: {
        // The back edge alone brings the address, and it stands behind the head in the order.
        Block* head = out.newBlock();
        Block* body = out.newBlock();
        Block* end = out.newBlock();
        ValueFromBlock addressAtEntry = out.anchor(out.int64Zero);
        ValueFromBlock roundsAtEntry = out.anchor(index);
        out.jump(head);
        out.appendTo(head);
        address = out.phi(Int64, addressAtEntry);
        Operation* roundsLeft = out.phi(Int64, roundsAtEntry);
        out.branch(roundsLeft, normalEdge(body), normalEdge(end));
        out.appendTo(body);
        out.addIncomingToPhi(address, out.anchor(out.add(slot, out.shl(index, 3))));
        out.addIncomingToPhi(roundsLeft, out.anchor(out.sub(roundsLeft, 1)));
        out.jump(head);
        out.appendTo(end);
        break;
    }
    }
    AOT_TEST_CHECK(!address->hasNoPosition());

    out.store64(value, out.address(frameSlots, address));
    Operation* read = isReadBehindSuspension ? nullptr : out.load64(out.address(frameSlots, address));
    appendFrameTestSuspension(test, sums, { });
    if (isReadBehindSuspension)
        read = out.load64(out.address(frameSlots, address));
    out.ret(read);

    if (test.prepare())
        return std::nullopt;
    AOT_TEST_CHECK(test.procedure().failure());
    return test.procedure().failure()->kind;
}

static void testMachineFrameAddressCarriedAcrossSuspensionIsRefused()
{
    AOT_TEST_CHECK(failureOfFrameTestWithCarriedAddress(FrameTestAddressCarrier::OpaqueCopy, true) == FailureKind::FrameAddressAcrossSuspension);
    AOT_TEST_CHECK(failureOfFrameTestWithCarriedAddress(FrameTestAddressCarrier::Selection, true) == FailureKind::FrameAddressAcrossSuspension);
    AOT_TEST_CHECK(failureOfFrameTestWithCarriedAddress(FrameTestAddressCarrier::JoinOfForwardEdges, true) == FailureKind::FrameAddressAcrossSuspension);
    AOT_TEST_CHECK(failureOfFrameTestWithCarriedAddress(FrameTestAddressCarrier::HeadOfLoop, true) == FailureKind::FrameAddressAcrossSuspension);
    AOT_TEST_CHECK(!failureOfFrameTestWithCarriedAddress(FrameTestAddressCarrier::OpaqueCopy, false));
    AOT_TEST_CHECK(!failureOfFrameTestWithCarriedAddress(FrameTestAddressCarrier::Selection, false));
    AOT_TEST_CHECK(!failureOfFrameTestWithCarriedAddress(FrameTestAddressCarrier::JoinOfForwardEdges, false));
    AOT_TEST_CHECK(!failureOfFrameTestWithCarriedAddress(FrameTestAddressCarrier::HeadOfLoop, false));
}

// A saved frame is visited by its map, which lists the roots. The pointer beside a reference keeps the cell alive only while the frame is on the stack.
static void runFrameTestOfNarrowReferenceAcrossSuspension(bool isListedAsRoot)
{
    constexpr uint64_t word = (uint64_t { 7 } << 32) + 0x80000100u;
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* argument = test.argumentGPR(0);
    Operation* sums = out.opaque(test.argumentGPR(1));
    Operation* reference = out.opaque(out.castToInt32(argument));
    reference->markAsNarrowReference();
    std::span<Operation* const> roots;
    if (isListedAsRoot)
        roots = singleElementSpan(reference);
    appendFrameTestSuspension(test, sums, roots);
    out.ret(out.zeroExtPtr(reference));

    if (!isListedAsRoot) {
        AOT_TEST_CHECK(!test.prepare());
        const std::optional<Failure>& failure = test.procedure().failure();
        AOT_TEST_CHECK(failure && failure->kind == FailureKind::NarrowReferenceWithoutPointer && failure->operation == reference->index());
        return;
    }

    compileOrFail(test);
    const Allocation& allocation = test.procedure().allocation();
    SpillIndex pointer = findFrameTestPointerBeside(allocation, reference);
    AOT_FRAME_TEST_CHECK(test, pointer != noSpill && allocation.spills()[pointer].livesAcrossSuspension);
    checkFrameTestPartsOfSavedFrame(test);
    checkFrameTestDeadWords(test);
    uint64_t sumOfRoots = 0;
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, &sumOfRoots) == static_cast<uint32_t>(word) && sumOfRoots == static_cast<uint32_t>(word));
}

static void testMachineFrameNarrowReferenceAcrossSuspensionIsRoot()
{
    runFrameTestOfNarrowReferenceAcrossSuspension(true);
    runFrameTestOfNarrowReferenceAcrossSuspension(false);
}

static void runFrameTestOfAlignment(unsigned numberAcrossCall, unsigned bytesOfOutgoingWords, unsigned bytesOfSlot)
{
    constexpr uint64_t a = 640;
    TestProcedure test;
    Output& out = test.out();
    test.procedure().requestCallArgAreaSizeInBytes(bytesOfOutgoingWords);
    Vector<Operation*> values = defineFrameTestValues(out, test.argumentGPR(0), numberAcrossCall);
    appendFrameTestCall(test);
    Operation* misalignment = appendFrameTestProbe(out);
    if (bytesOfSlot)
        misalignment = out.add(misalignment, out.bitAnd(out.lockedStackSlot(bytesOfSlot), bytesOfSlot >= 16 ? 15 : 7));
    out.ret(addUpFrameTestValues(out, misalignment, values.span()));

    compileOrFail(test);
    AOT_FRAME_TEST_CHECK(test, test.procedure().frame().coverage() == FrameCoverage::WholeFunction);
    checkFrameTestLayout(test);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a) == sumOfFrameTestValues(a, numberAcrossCall));
}

static void testMachineFrameKeepsStackPointerAligned()
{
    for (unsigned numberAcrossCall = 0; numberAcrossCall <= numberOfCalleeSavedGPRsInFrameTests() + 2; ++numberAcrossCall) {
        for (unsigned bytesOfOutgoingWords : { 0u, 8u, 40u }) {
            for (unsigned bytesOfSlot : { 0u, 8u, 16u, 24u })
                runFrameTestOfAlignment(numberAcrossCall, bytesOfOutgoingWords, bytesOfSlot);
        }
    }
}

// The probe stands behind the setup, so it finds a stack pointer that was moved once, on whichever side of the check.
static void runFrameTestOfStackCheck(unsigned bytesOfSlot, bool isAllocatedAheadOfCheck)
{
    constexpr uint64_t a = 1200;
    TestProcedure test;
    Output& out = test.out();
    FrameTestSetups setups;
    test.procedure().setStackCheckGenerator(noteFrameTestSetup, &setups);
    Operation* argument = test.argumentGPR(0);
    appendFrameTestCall(test);
    Operation* misalignment = appendFrameTestProbe(out);
    misalignment = out.add(misalignment, out.bitAnd(out.lockedStackSlot(bytesOfSlot), 15));
    out.ret(out.add(argument, misalignment));

    compileOrFail(test);
    const Frame& frame = test.procedure().frame();
    AOT_FRAME_TEST_CHECK(test, frame.coverage() == FrameCoverage::WholeFunction);
    AOT_FRAME_TEST_CHECK(test, (frame.bytesBelowFrameRecord() <= maxBytesAllocatedBeforeStackCheck) == isAllocatedAheadOfCheck);
    checkFrameTestLayout(test);
    AOT_FRAME_TEST_CHECK(test, setups.labels.size() == 1 && setups.sizesAgree);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a) == a);
}

static void testMachineFrameIsAllocatedOnEitherSideOfStackCheck()
{
    runFrameTestOfStackCheck(2 * sizeof(uint64_t), true);
    runFrameTestOfStackCheck(maxBytesAllocatedBeforeStackCheck + 2 * sizeof(uint64_t), false);
}

struct FrameTestSetupsThatCount {
    FrameTestSetups setups;
    uint64_t numberRun { 0 };
};

static void noteAndCountFrameTestSetup(void* context, CCallHelpers& jit, const Frame& frame, const StackCheckSite& site)
{
    FrameTestSetupsThatCount& counted = *static_cast<FrameTestSetupsThatCount*>(context);
    noteFrameTestSetup(&counted.setups, jit, frame, site);
    // Every register but the scratch registers may hold a value here.
    constexpr GPRReg address = GPRInfo::argumentGPR0;
    constexpr GPRReg count = GPRInfo::argumentGPR1;
    jit.pushToSave(address);
    jit.pushToSave(count);
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(&counted.numberRun)), address);
    jit.load64(CCallHelpers::Address(address), count);
    jit.add64(CCallHelpers::TrustedImm32(1), count);
    jit.store64(count, CCallHelpers::Address(address));
    jit.popToRestore(count);
    jit.popToRestore(address);
}

// Two parts call, each behind a test that needs no frame, and the way past both returns at once: each part sets the frame up at its own head. Each has a rare way out behind its call, and the two meet in one rare block,
// which stands below the head of neither.
static void runFrameTestOfRareBlockSharedByTwoFramedParts(bool sharedBlockReads, bool sharedBlockGoesOnWithoutFrame = false)
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* shared = out.newBlock();
    for (unsigned part = 0; part < 2; ++part) {
        Block* calls = out.newBlock();
        Block* goesOn = out.newBlock();
        Block* arm = out.newBlock();
        Block* leaves = out.newBlock();
        Block* past = out.newBlock();
        out.branch(out.testNonZero64(a, uint64_t { 1 } << part), normalEdge(calls), normalEdge(past));
        out.appendTo(calls);
        appendFrameTestCall(test);
        out.branch(out.testNonZero64(a, 4), rareEdge(arm), normalEdge(goesOn));
        out.appendTo(goesOn);
        out.ret(out.add(out.add(a, b), 10 + part));
        // The arm calls too, so its rare region needs the frame.
        out.appendTo(arm);
        appendFrameTestCall(test);
        out.branch(out.testNonZero64(a, 8), normalEdge(leaves), normalEdge(shared));
        out.appendTo(leaves);
        out.ret(out.add(out.add(a, b), 20 + part));
        out.appendTo(past);
    }
    Block* end = out.newBlock();
    out.jump(end);
    out.appendTo(end);
    out.ret(out.add(out.add(a, b), 1000));
    out.appendTo(shared);
    if (sharedBlockGoesOnWithoutFrame)
        out.jump(end);
    else if (sharedBlockReads)
        out.ret(out.add(out.add(a, b), 100));
    else
        out.unreachable();
    compileOrFail(test);

    const Procedure& proc = test.procedure();
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, sharedBlockGoesOnWithoutFrame || (allocation.headsOfFramedPart().size() == 2 && allocation.usedCalleeSaves() && proc.order().of(shared).isRare() && !allocation.isInFramedPart(shared)));
    // Its rare region needs the frame, so it runs in the frame that whichever part has set up, and sets none up.
    AOT_FRAME_TEST_CHECK(test, sharedBlockGoesOnWithoutFrame || (proc.frame().coverage() == FrameCoverage::FramedPart && proc.frame().holdsStepsOf(shared) && !proc.frame().isSetUpAtHeadOf(shared)));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 0 }, uint64_t { 7 }) == 7 + 1000);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 7 }) == 1 + 7 + 10);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 2 }, uint64_t { 7 }) == 2 + 7 + 11);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 13 }, uint64_t { 7 }) == 13 + 7 + 20);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 14 }, uint64_t { 7 }) == 14 + 7 + 21);
    if (sharedBlockGoesOnWithoutFrame) {
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 5 }, uint64_t { 7 }) == 5 + 7 + 1000);
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 6 }, uint64_t { 7 }) == 6 + 7 + 1000);
        return;
    }
    if (!sharedBlockReads)
        return;
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 5 }, uint64_t { 7 }) == 5 + 7 + 100);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 6 }, uint64_t { 7 }) == 6 + 7 + 100);
}

static void testMachineFrameRareBlockSharedByTwoFramedPartsReadsWhatLivesAcrossTheirCalls() { runFrameTestOfRareBlockSharedByTwoFramedParts(true); }

// An odd first argument returns the second at once. Otherwise a loop calls once for every two of the first argument.
static void testMachineFrameIsSetUpAheadOfLoopThatNeedsIt()
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    FrameTestSetupsThatCount setups;
    FrameTestMark markOfGuard;
    proc.setStackCheckGenerator(noteAndCountFrameTestSetup, &setups);
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* returning = out.newBlock();
    Block* before = out.newBlock();
    Block* loop = out.newBlock();
    Block* done = out.newBlock();

    out.branch(out.testNonZero64(a, 1), normalEdge(returning), normalEdge(before));
    out.appendTo(returning);
    appendFrameTestMark(out, markOfGuard, true);
    out.ret(b);
    out.appendTo(before);
    ValueFromBlock firstCount = out.anchor(out.int64Zero);
    ValueFromBlock firstSum = out.anchor(b);
    out.jump(loop);
    out.appendTo(loop);
    Operation* count = out.phi(Int64, firstCount);
    Operation* sum = out.phi(Int64, firstSum);
    appendFrameTestCall(test);
    Operation* nextCount = out.add(count, 2);
    Operation* nextSum = out.add(sum, nextCount);
    out.addIncomingToPhi(count, out.anchor(nextCount));
    out.addIncomingToPhi(sum, out.anchor(nextSum));
    out.branch(out.lessThan(nextCount, a), normalEdge(loop), normalEdge(done));
    out.appendTo(done);
    out.ret(nextSum);

    compileOrFail(test);
    const Block* head = proc.allocation().onlyHeadOfFramedPart();
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && head);
    AOT_FRAME_TEST_CHECK(test, proc.order().of(loop).loop != noLoop && proc.order().of(head).loop == noLoop && proc.allocation().isInFramedPart(loop));
    AOT_FRAME_TEST_CHECK(test, setups.setups.labels.size() == 1 && !markOfGuard.frameIsSetUp);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 7 }, uint64_t { 100 }) == 100 && !setups.numberRun);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 6 }, uint64_t { 100 }) == 112 && setups.numberRun == 1);
}

// A setup may stop the function for lack of stack. Whoever catches that must not find half of the function done.
// `largestFrameSetUpWithoutStackCheck`: a setup of such a frame stops nothing, and the store may stand ahead of it.
static void runFrameTestOfStoreBesideGuard(bool storeStandsAheadOfGuard, unsigned largestFrameSetUpWithoutStackCheck = 0)
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    proc.setLargestFrameSetUpWithoutStackCheck(largestFrameSetUpWithoutStackCheck);
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Operation* buffer = test.argumentGPR(2);
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();

    if (storeStandsAheadOfGuard)
        out.store64(b, out.address(test.heaps().argumentBuffer, buffer));
    out.branch(out.testNonZero64(a, 1), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    if (!storeStandsAheadOfGuard)
        out.store64(b, out.address(test.heaps().argumentBuffer, buffer));
    out.ret(b);
    out.appendTo(calling);
    appendFrameTestCall(test);
    out.ret(out.add(a, b));

    compileOrFail(test);
    bool frameFits = proc.frame().frameSize() <= largestFrameSetUpWithoutStackCheck;
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == (storeStandsAheadOfGuard && !frameFits ? FrameCoverage::WholeFunction : FrameCoverage::FramedPart));
    AOT_FRAME_TEST_CHECK(test, !proc.allocation().onlyHeadOfFramedPart() == (storeStandsAheadOfGuard && !largestFrameSetUpWithoutStackCheck));
    AOT_FRAME_TEST_CHECK(test, proc.allocation().writesMemoryAheadOfFramedPart() == (storeStandsAheadOfGuard && largestFrameSetUpWithoutStackCheck));
    uint64_t word = 0;
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 9 }, &word) == 9 && word == 9);
    word = 0;
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 2 }, uint64_t { 9 }, &word) == 11 && word == (storeStandsAheadOfGuard ? uint64_t { 9 } : uint64_t { 0 }));
}

static void testMachineFrameIsSetUpAheadOfWhateverWritesOnWayToIt()
{
    runFrameTestOfStoreBesideGuard(true);
    runFrameTestOfStoreBesideGuard(false);
    runFrameTestOfStoreBesideGuard(true, 240);
    runFrameTestOfStoreBesideGuard(false, 240);
    // The frame comes out larger than what goes untested, so it is set up at the entry after all.
    runFrameTestOfStoreBesideGuard(true, 8);
}

static void buildFrameTestWithValuesDefinedAheadOfGuard(TestProcedure& test, unsigned count)
{
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Vector<Operation*> values = defineFrameTestValues(out, test.argumentGPR(1), count);
    Operation* number = out.opaque(out.add(test.argumentFPR(0), out.constDouble(1.5)));
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();
    out.branch(out.testNonZero64(a, 1), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    out.ret(a);
    out.appendTo(calling);
    appendFrameTestCall(test);
    out.ret(out.add(addUpFrameTestValues(out, out.int64Zero, values.span()), out.bitCast(number, Int64)));
}

// More values than callees save are defined ahead of the guard and read behind the call. None is stored on the way that returns at once.
static void testMachineFrameStoresAtHeadWhatIsDefinedAheadOfIt()
{
    constexpr uint64_t b = 5000;
    constexpr double x = 0.25;
    constexpr unsigned numberBeyondRegisters = 3;
    TestProcedure test;
    unsigned count = numberOfCalleeSavedGPRsInFrameTests() + numberBeyondRegisters;
    buildFrameTestWithValuesDefinedAheadOfGuard(test, count);
    compileOrFail(test);

    Procedure& proc = test.procedure();
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart);
    checkFrameTestLayout(test);
    std::span<const SpillFacts> spills = allocation.spills();
    unsigned numberStoredAtHead = 0;
    for (SpillIndex spill = 0; spill < spills.size(); ++spill) {
        if (!spills[spill].isStoredAtHeadOfFramedPart)
            continue;
        ++numberStoredAtHead;
        AOT_FRAME_TEST_CHECK(test, !allocation.isInFramedPart(spills[spill].value->owner()) && allocation.spillOf(spills[spill].value) == spill && !allocation.isStoredBehindDefinition(spills[spill].value));
        unsigned numberOfSaves = 0;
        for (uint32_t index = allocation.headsOfFramedPart()[0].beginOfSteps; index < allocation.headsOfFramedPart()[0].endOfSteps; ++index)
            numberOfSaves += allocation.step(index).kind == StepKind::Save && allocation.step(index).destination == Location::spill(spill);
        AOT_FRAME_TEST_CHECK(test, numberOfSaves == 1);
    }
    AOT_FRAME_TEST_CHECK(test, numberStoredAtHead >= numberBeyondRegisters && numberStoredAtHead == spills.size());
    AOT_FRAME_TEST_CHECK(test, allocation.headsOfFramedPart()[0].endOfSteps - allocation.headsOfFramedPart()[0].beginOfSteps == numberStoredAtHead);
    AOT_FRAME_TEST_CHECK(test, test.printedAllocation().contains("isStoredAtHeadOfFramedPart"_s));

    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, b, x) == 1);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 2 }, b, x) == sumOfFrameTestValues(b, count) + std::bit_cast<uint64_t>(x + 1.5));
}

// The first argument is a narrow reference, made ahead of the guard and read behind a safepoint. An odd one returns at once.
// `passesRareSafepoint`: ahead of the guard a rare block that comes back may collect as well, where bit 1 is set.
static void runFrameTestOfNarrowReferenceAheadOfGuard(bool passesRareSafepoint)
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    RegisterMask clobbered = callerSavedRegistersOfFrameTest(test);
    Operation* argument = test.argumentGPR(0);
    Operation* destination = test.argumentGPR(1);
    Operation* reference = out.opaque(out.castToInt32(argument));
    reference->markAsNarrowReference();
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();
    if (passesRareSafepoint) {
        Block* slow = out.newBlock();
        Block* guard = out.newBlock();
        out.branch(out.testNonZero64(argument, 2), rareEdge(slow), normalEdge(guard));
        out.appendTo(slow);
        appendFrameTestCall(test);
        out.jump(guard);
        out.appendTo(guard);
    }
    out.branch(out.testNonZero64(argument, 1), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    out.ret(out.constInt64(5));
    out.appendTo(calling);
    PatchOperation* safepoint = out.patch(Void);
    safepoint->appendSomeRegister(destination);
    safepoint->clobberLate(clobbered);
    safepoint->setGenerator([clobbered, reference](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg pointer = params[0].gpr() == GPRInfo::argumentGPR0 ? GPRInfo::argumentGPR1 : GPRInfo::argumentGPR0;
        SpillIndex spill = findFrameTestPointerBeside(params.proc().allocation(), reference);
        AOT_TEST_CHECK(params.frameIsSetUp() && spill != noSpill);
        jit.load64(CCallHelpers::Address(CCallHelpers::framePointerRegister, params.frame().offsetFromFPOf(spill)), pointer);
        jit.store64(pointer, CCallHelpers::Address(params[0].gpr()));
        emitFrameTestPoison(jit, clobbered);
    });
    out.ret(out.zeroExtPtr(reference));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.liveness().of(reference).has(ValueFlag::NeedsPointerBesideIt) && allocation.onlyHeadOfFramedPart() == calling);
    AOT_FRAME_TEST_CHECK(test, allocation.needsFrameAheadOfFramedPart() == passesRareSafepoint);
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == (passesRareSafepoint ? FrameCoverage::WholeFunction : FrameCoverage::FramedPart));
    SpillIndex pointer = findFrameTestPointerBeside(allocation, reference);
    AOT_FRAME_TEST_CHECK(test, pointer != noSpill);
    const BlockSteps& stepsOfKeeper = allocation.stepsOf(passesRareSafepoint ? reference->owner() : calling);
    unsigned numberOfStores = 0;
    for (uint32_t index = 0; index < allocation.steps().size(); ++index) {
        if (allocation.step(index).kind != StepKind::KeepPointer)
            continue;
        ++numberOfStores;
        AOT_FRAME_TEST_CHECK(test, allocation.step(index).destination == Location::spill(pointer) && stepsOfKeeper.begin <= index && index < stepsOfKeeper.end);
    }
    AOT_FRAME_TEST_CHECK(test, numberOfStores == 1);

    for (uint64_t low : { 0x80000100u, 0x80000101u, 0x80000102u, 0x80000103u }) {
        uint64_t word = (uint64_t { 7 } << 32) + low;
        uint64_t found = 0;
        bool returnsAtOnce = word & 1;
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, &found) == (returnsAtOnce ? uint64_t { 5 } : low));
        AOT_FRAME_TEST_CHECK(test, found == (returnsAtOnce ? uint64_t { 0 } : low << cell32Shift));
    }
}

static void testMachineFrameKeepsPointerAtHeadOfFramedPart() { runFrameTestOfNarrowReferenceAheadOfGuard(false); }
static void testMachineFrameKeepsPointerAtDefinitionForSafepointAheadOfFramedPart() { runFrameTestOfNarrowReferenceAheadOfGuard(true); }

// No call anywhere. One arm holds more values at one place than there are registers that no caller counts on: it is framed by itself, and the way around it runs in the frame of the caller.
static void testMachineFrameIsSetUpForArmThatRunsOutOfRegisters()
{
    constexpr uint64_t word = 0x1000;
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    unsigned count = numberOfFrameTestRegisters(registerFileOfFrameTest(test).allocatableIn(Bank::GP)) + 2;
    Operation* argument = test.argumentGPR(0);
    Block* returning = out.newBlock();
    Block* crowded = out.newBlock();
    out.branch(out.testNonZero64(argument, 1), normalEdge(returning), normalEdge(crowded));
    out.appendTo(returning);
    out.ret(out.constInt64(5));
    out.appendTo(crowded);
    Vector<Operation*> values;
    for (unsigned index = 0; index < count; ++index)
        values.append(out.opaque(out.add(argument, 2 * index)));
    Operation* sum = out.int64Zero;
    for (Operation* value : values)
        sum = out.add(sum, value);
    out.ret(sum);

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, allocation.onlyHeadOfFramedPart() == crowded && !allocation.needsFrameAheadOfFramedPart() && proc.frame().coverage() == FrameCoverage::FramedPart);
    uint64_t expected = 0;
    for (unsigned index = 0; index < count; ++index)
        expected += word + 2 * index;
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word + 1) == 5);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word) == expected);
}

// A value that the head of the framed part stores has lost its register ahead of a rare region. Each of two siblings in the region loads it, saves it around a call and reads it again:
// the one that the walk comes to second comes in without the save of the first. `pressure`: how many more values are alive where the region is entered.
static void runFrameTestOfSiblingsInRareRegion(unsigned pressure, bool& valueCameInWithoutRegister)
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* slow = out.newBlock();
    Block* first = out.newBlock();
    Block* second = out.newBlock();
    Block* meeting = out.newBlock();
    Block* guard = out.newBlock();
    Block* calling = out.newBlock();
    Block* returning = out.newBlock();

    Operation* kept = out.add(a, b);
    Vector<Operation*> many = defineValues(test, b, pressure, Int64);
    Operation* sum = pressure ? sumOf(test, many.span()) : out.int64Zero;
    out.branch(out.testNonZero64(a, 4), rareEdge(slow), normalEdge(guard));
    out.appendTo(slow);
    out.branch(out.testNonZero64(a, 8), normalEdge(first), normalEdge(second));
    out.appendTo(first);
    Operation* inFirst = out.add(kept, 1);
    appendFrameTestCall(test);
    ValueFromBlock fromFirst = out.anchor(out.add(kept, inFirst));
    out.jump(meeting);
    out.appendTo(second);
    Operation* inSecond = out.add(kept, 2);
    appendFrameTestCall(test);
    ValueFromBlock fromSecond = out.anchor(out.add(kept, inSecond));
    out.jump(meeting);
    out.appendTo(meeting);
    out.ret(out.add(out.phi(Int64, fromFirst, fromSecond), sum));
    out.appendTo(guard);
    out.branch(out.testNonZero64(a, 1), normalEdge(calling), normalEdge(returning));
    out.appendTo(calling);
    appendFrameTestCall(test);
    out.ret(out.add(out.add(kept, sum), appendFrameTestProbe(out)));
    out.appendTo(returning);
    out.ret(sum);

    compileOrFail(test);
    AOT_FRAME_TEST_CHECK(test, proc.order().of(slow).isRare() && proc.order().of(first).isRare() && proc.order().of(second).isRare());
    bool isBoundAtHead = false;
    for (const RegisterBinding& binding : proc.allocation().registersAtHeadOf(slow))
        isBoundAtHead |= binding.value == kept;
    valueCameInWithoutRegister |= !isBoundAtHead && proc.allocation().isSpilled(kept);
    uint64_t sumOfMany = test.call<uint64_t>(uint64_t { 0 }, uint64_t { 10 });
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 12 }, uint64_t { 10 }) == 2 * 22 + 1 + sumOfMany);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 4 }, uint64_t { 10 }) == 2 * 14 + 2 + sumOfMany);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 10 }) == 11 + sumOfMany);
}

static void testMachineFrameBlockOfRareRegionDoesNotReadWhatItsSiblingSaved()
{
    bool valueCameInWithoutRegister = false;
    for (unsigned pressure = 0; pressure <= 32; ++pressure)
        runFrameTestOfSiblingsInRareRegion(pressure, valueCameInWithoutRegister);
    AOT_TEST_CHECK(valueCameInWithoutRegister);
}

// `if (a & 1) call(); if (a & 2) call(); [call();] return a + b`: a way leads from the first part that calls to the second. Where the way past both calls nothing, each part has its head.
// Where every way calls in the end, one head stands above all of it, behind the guard.
static void runFrameTestOfPartsThatLeadToOneAnother(bool everyWayCalls)
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    FrameTestSetupsThatCount setups;
    FrameTestMark markOfGuard;
    proc.setStackCheckGenerator(noteAndCountFrameTestSetup, &setups);
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* returning = out.newBlock();
    Block* firstTest = out.newBlock();
    Block* first = out.newBlock();
    Block* secondTest = out.newBlock();
    Block* second = out.newBlock();
    Block* end = out.newBlock();

    out.branch(out.testNonZero64(a, 8), normalEdge(returning), normalEdge(firstTest));
    out.appendTo(returning);
    appendFrameTestMark(out, markOfGuard, true);
    out.ret(b);
    out.appendTo(firstTest);
    out.branch(out.testNonZero64(a, 1), normalEdge(first), normalEdge(secondTest));
    out.appendTo(first);
    appendFrameTestCall(test);
    out.jump(secondTest);
    out.appendTo(secondTest);
    out.branch(out.testNonZero64(a, 2), normalEdge(second), normalEdge(end));
    out.appendTo(second);
    appendFrameTestCall(test);
    out.jump(end);
    out.appendTo(end);
    if (everyWayCalls)
        appendFrameTestCall(test);
    out.ret(out.add(a, b));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && !markOfGuard.frameIsSetUp);
    if (everyWayCalls)
        AOT_FRAME_TEST_CHECK(test, allocation.onlyHeadOfFramedPart() == firstTest && setups.setups.labels.size() == 1);
    else
        AOT_FRAME_TEST_CHECK(test, allocation.headsOfFramedPart().size() == 2 && allocation.isHeadOfFramedPart(first) && allocation.isHeadOfFramedPart(second) && setups.setups.labels.size() == 2);
    checkFrameTestRanges(test);
    checkFrameTestLayout(test);
    for (uint64_t argument : { 0, 1, 2, 3 }) {
        setups.numberRun = 0;
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(argument, uint64_t { 10 }) == argument + 10);
        AOT_FRAME_TEST_CHECK(test, setups.numberRun == (everyWayCalls ? 1 : static_cast<uint64_t>(std::popcount(argument))));
    }
    setups.numberRun = 0;
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 8 }, uint64_t { 10 }) == 10 && !setups.numberRun);
}

static void testMachineFrameHeadStandsWhereEveryWayOnCalls() { runFrameTestOfPartsThatLeadToOneAnother(true); }

static void testMachineFrameOfRareRegionAheadOfFramedPartIsItsOwn()
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    FrameTestSetupsThatCount setups;
    FrameTestMark markInRareBlock;
    FrameTestMark markOfGuard;
    proc.setStackCheckGenerator(noteAndCountFrameTestSetup, &setups);
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* slow = out.newBlock();
    Block* guard = out.newBlock();
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();

    ValueFromBlock fromEntry = out.anchor(out.int64Zero);
    out.branch(out.testNonZero64(a, 2), rareEdge(slow), normalEdge(guard));
    out.appendTo(slow);
    appendFrameTestMark(out, markInRareBlock, false);
    appendFrameTestCall(test);
    ValueFromBlock fromRareBlock = out.anchor(out.add(appendFrameTestProbe(out), 1000));
    out.jump(guard);
    out.appendTo(guard);
    Operation* extra = out.phi(Int64, fromEntry, fromRareBlock);
    out.branch(out.testNonZero64(a, 1), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    appendFrameTestMark(out, markOfGuard, true);
    out.ret(out.add(b, extra));
    out.appendTo(calling);
    appendFrameTestCall(test);
    out.ret(out.add(out.add(a, b), out.add(extra, appendFrameTestProbe(out))));

    compileOrFail(test);
    const Frame& frame = proc.frame();
    AOT_FRAME_TEST_CHECK(test, frame.coverage() == FrameCoverage::FramedPart && proc.allocation().onlyHeadOfFramedPart() == calling);
    AOT_FRAME_TEST_CHECK(test, proc.order().of(slow).isRare() && !proc.allocation().isInFramedPart(slow) && frame.holdsStepsOf(slow) && frame.isSetUpAtHeadOf(slow) && frame.isTornDownAtEndOf(slow));
    AOT_FRAME_TEST_CHECK(test, setups.setups.labels.size() == 2 && setups.setups.sizesAgree && markInRareBlock.frameIsSetUp && !markOfGuard.frameIsSetUp);
    checkFrameTestRanges(test);
    checkFrameTestLayout(test);
    struct Case {
        uint64_t a;
        uint64_t expected;
        uint64_t numberOfSetups;
    };
    for (Case testCase : { Case { 1, 10, 0 }, Case { 3, 1010, 1 }, Case { 4, 14, 1 }, Case { 6, 1016, 2 } }) {
        setups.numberRun = 0;
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(testCase.a, uint64_t { 10 }) == testCase.expected && setups.numberRun == testCase.numberOfSetups);
    }
}

// Both ways meet in a block that computes on. With a StackSlot an address in the frame may live as long as the function runs, so the frame stays to the end.
static void runFrameTestOfWaysThatMeet(bool hasStackSlot)
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    FrameTestMark markBehindJoin;
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* calling = out.newBlock();
    Block* join = out.newBlock();

    ValueFromBlock fromEntry = out.anchor(b);
    out.branch(out.testNonZero64(a, 1), normalEdge(join), normalEdge(calling));
    out.appendTo(calling);
    appendFrameTestCall(test);
    Operation* sum = out.add(a, b);
    if (hasStackSlot)
        sum = out.add(sum, out.bitAnd(out.lockedStackSlot(16), 15));
    ValueFromBlock fromCall = out.anchor(sum);
    out.jump(join);
    out.appendTo(join);
    Operation* joined = out.phi(Int64, fromEntry, fromCall);
    appendFrameTestMark(out, markBehindJoin, true);
    out.ret(out.mul(joined, 3));

    compileOrFail(test);
    const Frame& frame = proc.frame();
    AOT_FRAME_TEST_CHECK(test, proc.allocation().onlyHeadOfFramedPart() == calling && !proc.allocation().needsFrameAheadOfFramedPart());
    AOT_FRAME_TEST_CHECK(test, frame.coverage() == (hasStackSlot ? FrameCoverage::WholeFunction : FrameCoverage::FramedPart));
    AOT_FRAME_TEST_CHECK(test, markBehindJoin.runs == 1 && markBehindJoin.frameIsSetUp == hasStackSlot);
    if (!hasStackSlot)
        AOT_FRAME_TEST_CHECK(test, frame.isTornDownAtEndOf(calling) && !frame.holdsStepsOf(join) && test.printedFrame().contains("torn down at the end of"_s));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 10 }) == 30);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 2 }, uint64_t { 10 }) == 36);
}

static void testMachineFrameIsTornDownWhereWayLeavesFramedPart()
{
    runFrameTestOfWaysThatMeet(false);
    runFrameTestOfWaysThatMeet(true);
}

// Both ways hand their answer to one block that returns it. The ways in the frame share its teardown, and the way without a frame returns where it stands.
static void testMachineFrameWayWithoutFrameReturnsInPlaceOfJump()
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();
    Block* other = out.newBlock();
    Block* last = out.newBlock();
    Block* end = out.newBlock();

    out.branch(out.testNonZero64(a, 1), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    ValueFromBlock fromGuard = out.anchor(out.add(b, 1));
    out.jump(end);
    out.appendTo(calling);
    appendFrameTestCall(test);
    out.branch(out.testNonZero64(a, 2), normalEdge(other), normalEdge(last));
    out.appendTo(other);
    ValueFromBlock fromOther = out.anchor(out.add(b, 2));
    out.jump(end);
    out.appendTo(last);
    ValueFromBlock fromLast = out.anchor(out.add(a, b));
    out.jump(end);
    out.appendTo(end);
    out.ret(out.phi(Int64, fromGuard, fromOther, fromLast));

    compileOrFail(test);
    const Frame& frame = proc.frame();
    AOT_FRAME_TEST_CHECK(test, frame.coverage() == FrameCoverage::FramedPart && proc.allocation().onlyHeadOfFramedPart() == calling && !proc.allocation().isInFramedPart(end));
    AOT_FRAME_TEST_CHECK(test, frame.holdsStepsOf(end) && !frame.isSetUpAtHeadOf(end) && frame.returnsInPlaceOfJumpAtEndOf(returning) && !frame.holdsStepsOf(returning));
    for (const Block* block : proc.order().blocksInOrder())
        AOT_FRAME_TEST_CHECK(test, !frame.isTornDownAtEndOf(block));
    AOT_FRAME_TEST_CHECK(test, test.printedFrame().contains("return in place of the jump of"_s));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 10 }) == 11);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 2 }, uint64_t { 10 }) == 12);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 4 }, uint64_t { 10 }) == 14);
}

#undef AOT_FRAME_TEST_CHECK

} // namespace Machine

// x0 takes the first argument and the result. rdi and rax are two.
static constexpr ASCIILiteral argumentLiesInReturnRegister = isARM64() ? "machine/AOTMachineAllocator*.cpp: a way that returns at once takes the register of an argument that other ways read: the argument is stored ahead of the guards, so the frame covers the whole function, and a Phi that is returned gets another register"_s : ASCIILiteral();

std::span<const NativeTest> machineFrameTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/frame/rare-blocks-with-calls-have-frames-of-their-own"_s, Machine::testMachineFrameCoversRareBlocksAlone },
        { "machine/frame/small-rare-region-stands-among-the-other-code"_s, Machine::testMachineSmallRareRegionStandsAmongOtherCode },
        { "machine/frame/small-run-of-unlikely-blocks-stands-near-its-jump"_s, Machine::testMachineSmallRunOfUnlikelyBlocksStandsNearItsJump },
        { "machine/frame/handlers-rejection-and-suspension-set-it-up-at-entry"_s, Machine::testMachineFrameIsSetUpAtEntryWhereTraitsAskForIt },
        { "machine/frame/callee-saves-ascend-below-the-record-and-come-back"_s, Machine::testMachineFrameCalleeSavesAscendBelowRecord },
        { "machine/frame/function-that-saves-its-frame"_s, Machine::testMachineFrameOfFunctionThatSavesItsFrame },
        { "machine/frame/words-stored-on-one-path-are-cleared-at-the-suspension"_s, Machine::testMachineFrameClearsWordsStoredOnOnePath },
        { "machine/frame/word-held-by-its-range-alone-keeps-no-leftover"_s, Machine::testMachineFrameKeepsNoLeftoverInWordHeldByRangeAlone },
        { "machine/frame/spill-that-no-path-reads-behind-a-suspension-is-not-kept"_s, Machine::testMachineFrameKeepsNoSpillThatNoPathReadsBehindSuspension },
        { "machine/frame/many-spills-that-live-through-the-block-of-a-suspension-are-kept"_s, Machine::testMachineFrameKeepsManySpillsThatLiveThroughBlockOfSuspension },
        { "machine/frame/many-spills-that-live-round-a-loop-with-a-suspension-are-kept"_s, Machine::testMachineFrameKeepsManySpillsThatLiveRoundLoopWithSuspension },
        { "machine/frame/stack-slots-and-half-words-of-a-frame-that-is-saved-move-whole"_s, Machine::testMachineFrameThatIsSavedMovesStackSlotsAndHalfWordsWhole },
        { "machine/frame/word-that-a-suspension-names-by-its-number-comes-first"_s, Machine::testMachineFrameWordThatSuspensionNamesByNumberComesFirst },
        { "machine/frame/words-that-saves-write-are-cleared-at-setup"_s, Machine::testMachineFrameClearsAtSetupWordsThatSavesWrite },
        { "machine/frame/pointer-is-kept-only-where-the-collector-needs-it"_s, Machine::testMachineFrameKeepsPointerOnlyWhereCollectorNeedsIt },
        { "machine/frame/sunk-sum-of-a-narrow-reference-lives-across-a-safepoint"_s, Machine::testMachineFrameSunkSumOfNarrowReferenceLivesAcrossSafepoint },
        { "machine/frame/owner-outlives-inner-pointer"_s, Machine::testMachineFrameOwnerOutlivesInnerPointer },
        { "machine/frame/frame-address-carried-across-a-suspension-is-refused"_s, Machine::testMachineFrameAddressCarriedAcrossSuspensionIsRefused },
        { "machine/frame/narrow-reference-across-a-suspension-is-a-root"_s, Machine::testMachineFrameNarrowReferenceAcrossSuspensionIsRoot },
        { "machine/frame/stack-pointer-stays-aligned"_s, Machine::testMachineFrameKeepsStackPointerAligned },
        { "machine/frame/frame-is-allocated-on-either-side-of-its-stack-check"_s, Machine::testMachineFrameIsAllocatedOnEitherSideOfStackCheck },
        { "machine/frame/rare-block-that-two-framed-parts-share-reads-what-lives-across-their-calls"_s, Machine::testMachineFrameRareBlockSharedByTwoFramedPartsReadsWhatLivesAcrossTheirCalls },
        { "machine/frame/loop-that-needs-it-finds-it-set-up"_s, Machine::testMachineFrameIsSetUpAheadOfLoopThatNeedsIt },
        { "machine/frame/whatever-writes-on-the-way-to-it-follows-the-setup"_s, Machine::testMachineFrameIsSetUpAheadOfWhateverWritesOnWayToIt },
        { "machine/frame/what-is-defined-ahead-of-it-is-stored-at-its-head"_s, Machine::testMachineFrameStoresAtHeadWhatIsDefinedAheadOfIt },
        { "machine/frame/pointer-beside-a-reference-from-ahead-of-it-is-kept-at-its-head"_s, Machine::testMachineFrameKeepsPointerAtHeadOfFramedPart },
        { "machine/frame/safepoint-ahead-of-it-finds-the-pointer-kept"_s, Machine::testMachineFrameKeepsPointerAtDefinitionForSafepointAheadOfFramedPart },
        { "machine/frame/arm-that-runs-out-of-registers-is-framed-by-itself"_s, Machine::testMachineFrameIsSetUpForArmThatRunsOutOfRegisters },
        { "machine/frame/rare-region-ahead-of-it-has-a-frame-of-its-own"_s, Machine::testMachineFrameOfRareRegionAheadOfFramedPartIsItsOwn },
        { "machine/frame/block-of-a-rare-region-does-not-read-what-its-sibling-saved"_s, Machine::testMachineFrameBlockOfRareRegionDoesNotReadWhatItsSiblingSaved },
        { "machine/frame/head-stands-where-every-way-on-calls"_s, Machine::testMachineFrameHeadStandsWhereEveryWayOnCalls },
        { "machine/frame/way-that-leaves-it-tears-it-down"_s, Machine::testMachineFrameIsTornDownWhereWayLeavesFramedPart },
        { "machine/frame/way-without-it-returns-in-place-of-a-jump"_s, Machine::testMachineFrameWayWithoutFrameReturnsInPlaceOfJump, argumentLiesInReturnRegister },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
