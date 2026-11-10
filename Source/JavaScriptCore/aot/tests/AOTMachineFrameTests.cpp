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

namespace JSC { namespace AOT { namespace Machine {

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

static void checkFrameTestMarkThatFollowsFrameSetupBesideCall(bool testsStackAheadOfFramedPart)
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Block* calls = out.newBlock();
    Block* leaves = out.newBlock();
    FrameTestMark mark;
    out.branch(out.testNonZero64(a, 1), normalEdge(calls), normalEdge(leaves));
    out.appendTo(calls);
    appendFrameTestCall(test);
    out.ret(out.add(a, 100));
    out.appendTo(leaves);
    PatchOperation* patch = appendFrameTestMark(out, mark, true);
    patch->setFollowsFrameSetup(true);
    patch->setTestsStackAheadOfFramedPart(testsStackAheadOfFramedPart);
    out.ret(out.add(a, 1));
    compileOrFail(test);

    const Procedure& proc = test.procedure();
    AOT_FRAME_TEST_CHECK(test, mark.runs == 1 && mark.frameIsSetUp == !testsStackAheadOfFramedPart);
    AOT_FRAME_TEST_CHECK(test, !testsStackAheadOfFramedPart || (proc.frame().coverage() == FrameCoverage::FramedPart && !proc.frame().holdsStepsOf(leaves)));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 3 }) == 103);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 4 }) == 5);
}

enum class EndOfFrameTestPatch : uint8_t { GoesOn, GoesOnIntoOops, EndsBlockAndGoesOn, EndsFunction };

static bool scanRefusesPatchThatMayCollectWithoutFrame(EndOfFrameTestPatch end, bool showsRegistersToCollector)
{
    ProcedureTraits traits;
    traits.hasHandlers = end == EndOfFrameTestPatch::EndsBlockAndGoesOn;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* kept = test.argumentGPR(0);
    PatchOperation* patch = out.patch(Void);
    AOT_TEST_CHECK(patch->effects.mayCollect);
    patch->setRunsWithoutFrame(true);
    patch->setShowsRegistersToCollector(showsRegistersToCollector);
    patch->setGenerator([](CCallHelpers&, const PatchParameters&) { });
    switch (end) {
    case EndOfFrameTestPatch::GoesOn:
        out.ret(kept);
        break;
    case EndOfFrameTestPatch::GoesOnIntoOops:
        out.unreachable();
        break;
    case EndOfFrameTestPatch::EndsBlockAndGoesOn: {
        Block* continuation = out.newBlock();
        Block* landingPad = out.newBlock();
        out.appendSuccessor(normalEdge(continuation));
        out.appendSuccessor(rareEdge(landingPad));
        patch->clobberLateOnSuccessor(1, RegisterSet());
        patch->effects.terminal = true;
        out.appendTo(continuation);
        out.ret(kept);
        out.appendTo(landingPad);
        out.ret(kept);
        break;
    }
    case EndOfFrameTestPatch::EndsFunction:
        patch->effects.terminal = true;
        break;
    }
    if (test.prepare())
        return false;
    const std::optional<Failure>& failure = test.procedure().failure();
    AOT_TEST_CHECK(failure && failure->kind == FailureKind::MissingFrameSetup && failure->operation == patch->index());
    return true;
}

static void testMachineFramePatchThatMayCollectWithoutFrameShowsItsRegistersOrEndsFunction()
{
    AOT_TEST_CHECK(!scanRefusesPatchThatMayCollectWithoutFrame(EndOfFrameTestPatch::GoesOn, true));
    AOT_TEST_CHECK(scanRefusesPatchThatMayCollectWithoutFrame(EndOfFrameTestPatch::GoesOn, false));
    AOT_TEST_CHECK(!scanRefusesPatchThatMayCollectWithoutFrame(EndOfFrameTestPatch::GoesOnIntoOops, true));
    AOT_TEST_CHECK(scanRefusesPatchThatMayCollectWithoutFrame(EndOfFrameTestPatch::GoesOnIntoOops, false));
    AOT_TEST_CHECK(!scanRefusesPatchThatMayCollectWithoutFrame(EndOfFrameTestPatch::EndsBlockAndGoesOn, true));
    AOT_TEST_CHECK(scanRefusesPatchThatMayCollectWithoutFrame(EndOfFrameTestPatch::EndsBlockAndGoesOn, false));
    AOT_TEST_CHECK(!scanRefusesPatchThatMayCollectWithoutFrame(EndOfFrameTestPatch::EndsFunction, false));
}

static void testMachineFramePatchThatTestsStackItselfStandsAheadOfFramedPart()
{
    checkFrameTestMarkThatFollowsFrameSetupBesideCall(false);
    checkFrameTestMarkThatFollowsFrameSetupBesideCall(true);
}

static void testMachineFramePatchThatTestsStackItselfGetsFrameWhereNothingElseNeedsOne()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Block* one = out.newBlock();
    Block* other = out.newBlock();
    FrameTestMark mark;
    out.branch(out.testNonZero64(a, 1), normalEdge(one), normalEdge(other));
    out.appendTo(one);
    out.ret(out.add(a, 100));
    out.appendTo(other);
    PatchOperation* patch = appendFrameTestMark(out, mark, true);
    patch->setFollowsFrameSetup(true);
    patch->setTestsStackAheadOfFramedPart(true);
    out.ret(out.add(a, 1));
    compileOrFail(test);

    AOT_FRAME_TEST_CHECK(test, test.procedure().frame().coverage() == FrameCoverage::WholeFunction);
    AOT_FRAME_TEST_CHECK(test, mark.runs == 1 && mark.frameIsSetUp);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 3 }) == 103);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 4 }) == 5);
}

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

static bool frameTestWordHolds(unsigned location, intptr_t offsetFromFP, unsigned numberOfBytes)
{
    intptr_t begin = -static_cast<intptr_t>(bytesOfFrameTestWord) * static_cast<intptr_t>(location);
    return offsetFromFP < begin + bytesOfFrameTestWord && offsetFromFP + static_cast<intptr_t>(numberOfBytes) > begin;
}

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

static PatchOperation* appendFrameTestSuspension(TestProcedure& test, Operation* sumOfRoots, std::span<Operation* const> roots)
{
    RegisterMask lost = registersLostAtFrameTestSuspension(test);
    PatchOperation* suspension = test.out().patch(Void);
    suspension->append(sumOfRoots, ValueRep::LateColdAny);
    for (Operation* root : roots)
        suspension->append(root, ValueRep::LateColdAny);
    suspension->clobberLate(lost);
    suspension->setIsSuspension(true);
    suspension->setGenerator([lost](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        constexpr GPRReg address = GPRInfo::argumentGPR0;
        constexpr GPRReg word = GPRInfo::argumentGPR1;
        constexpr GPRReg sum = GPRInfo::argumentGPR2;
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

        emitFrameTestPoison(jit, lost);
        for (unsigned location = 1; location <= numberOfWords; ++location) {
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
    AOT_FRAME_TEST_CHECK(test, frame.beginOfWordsClearedAtSetup() == frame.endOfWordsClearedAtSetup() && !test.printedFrame().contains("wordsClearedAtSetup"_s));
}

static unsigned checkFrameTestDeadWords(TestProcedure& test)
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
        const BitSpan& liveSpills = proc.allocation().liveSpillsAt(indexOfSuspension);
        const std::optional<BitSpan>& deadWords = suspension->frameWordsDeadAcrossSuspension();
        Position early = proc.order().earlyPositionOf(suspension);
        Position late = proc.order().latePositionOf(suspension);
        AOT_FRAME_TEST_CHECK(test, deadWords);
        AOT_FRAME_TEST_CHECK(test, text.contains(makeString("suspension @"_s, suspension->index(), ": dead words ["_s)));
        for (unsigned location = 1; location <= numberOfSaves; ++location)
            AOT_FRAME_TEST_CHECK(test, !deadWords->get(location));
        AOT_FRAME_TEST_CHECK(test, !(numberOfSaves % 2) || deadWords->get(numberOfSaves + 1));

        for (unsigned location = roundUpToMultipleOf<2>(numberOfSaves) + 1; location <= numberOfWordsKept; ++location) {
            bool isHeld = false;
            bool mayBeHeld = false;
            for (SpillIndex index = 0; index < spills.size(); ++index) {
                const SpillFacts& spill = spills[index];
                bool isHeldHere = liveSpills.get(index);
                AOT_FRAME_TEST_CHECK(test, !isHeldHere || (spill.livesAcrossSuspension && spill.begin <= late && spill.end >= early));
                if (!frameTestWordHolds(location, frame.offsetFromFPOf(index), bytesForWidth(spill.width)))
                    continue;
                isHeld |= isHeldHere;
                mayBeHeld |= spill.begin <= late && spill.end >= early;
            }
            for (const StackSlot* slot : proc.stackSlots()) {
                if (frameTestWordHolds(location, slot->offsetFromFP(), slot->byteSize()))
                    isHeld = mayBeHeld = true;
            }
            AOT_FRAME_TEST_CHECK(test, !isHeld || !deadWords->get(location));
            AOT_FRAME_TEST_CHECK(test, mayBeHeld || deadWords->get(location));
            numberOfDeadWords += deadWords->get(location);
        }
    }
    return numberOfDeadWords;
}

static void runFrameTestOfFunctionThatSavesItsFrame(unsigned numberAcrossCall, unsigned numberAcrossSecond)
{
    constexpr uint64_t a = 5000;
    constexpr unsigned numberAcrossBoth = 2;
    constexpr unsigned numberAcrossFirst = 4;
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

    Operation* seed = test.argumentGPR(0);
    Operation* argumentWithSums = test.argumentGPR(1);
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
    PatchOperation* first = appendFrameTestSuspension(test, sums, rootsOfFirst.span());
    Operation* seedOfSecond = addUpFrameTestValues(out, sumAcrossCall, acrossFirst.span());
    Vector<Operation*> acrossSecond = defineFrameTestValues(out, seedOfSecond, numberAcrossSecond);

    Vector<Operation*> rootsOfSecond = acrossBoth;
    rootsOfSecond.appendVector(acrossSecond);
    rootsOfSecond.append(halfWordRoot);
    PatchOperation* second = appendFrameTestSuspension(test, sums, rootsOfSecond.span());
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
    AOT_FRAME_TEST_CHECK(test, words[0] == expectedSumOfRoots);
}

static void testMachineFrameOfFunctionThatSavesItsFrame()
{
    for (unsigned numberAcrossCall = 0; numberAcrossCall <= numberOfCalleeSavedGPRsInFrameTests() + 2; ++numberAcrossCall) {
        for (unsigned numberAcrossSecond : { 12u, 30u })
            runFrameTestOfFunctionThatSavesItsFrame(numberAcrossCall, numberAcrossSecond);
    }
}

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
    AOT_FRAME_TEST_CHECK(test, !spill.livesAcrossSuspension && !allocation.liveSpillsAt(0).get(allocation.spillOf(value)));

    std::array<uint64_t, 3> words { 0, poison, poison };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 1 }) == a + 1 && !words[0]);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 0 }) == a && !words[0]);
}

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
        AOT_FRAME_TEST_CHECK(test, allocation.isSpilled(value) && allocation.liveSpillsAt(0).get(allocation.spillOf(value)) && allocation.spills()[allocation.spillOf(value)].livesAcrossSuspension);
    for (Operation* value : readOnOtherWay)
        AOT_FRAME_TEST_CHECK(test, !allocation.isSpilled(value) || !allocation.liveSpillsAt(0).get(allocation.spillOf(value)));

    std::array<uint64_t, 3> words { 0, poison, poison };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 0 }) == sumOfFrameTestValues(a, numberOfEach));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 1 }) == sumOfFrameTestValues(a, numberOfEach) + sumOfFrameTestValues(a + 1000, numberOfEach));
}

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
    constexpr uint64_t sumOfWhatLoopReads = numberOfEach * a + numberOfEach * numberOfEach;
    constexpr uint64_t sumOfWhatIsReadBehindLoop = numberOfEach * a + numberOfEach * (numberOfEach + 1);
    for (uint64_t numberOfRounds : { 0, 1, 3 }) {
        std::array<uint64_t, 3> words { 0, poison, poison };
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), numberOfRounds) == sumOfFrameTestValues(a + 1000, numberOfEach) + numberOfRounds * sumOfWhatLoopReads + sumOfWhatIsReadBehindLoop);
    }

    unsigned numberKeptOutsideSpills = 0;
    for (const Vector<Operation*>& values : { readInLoop, readBehindLoop }) {
        for (Operation* value : values) {
            bool isHeld = allocation.isSpilled(value) && allocation.liveSpillsAt(0).get(allocation.spillOf(value));
            AOT_FRAME_TEST_CHECK(test, !isHeld || allocation.spills()[allocation.spillOf(value)].livesAcrossSuspension);
            numberKeptOutsideSpills += !isHeld;
        }
    }
    AOT_FRAME_TEST_CHECK(test, numberKeptOutsideSpills <= 1);
    for (Operation* value : readInFrontOfLoop)
        AOT_FRAME_TEST_CHECK(test, !allocation.isSpilled(value) || !allocation.liveSpillsAt(0).get(allocation.spillOf(value)));
}

static bool oneByteReachesFrameTestSpill(TestProcedure& test, const Operation* value)
{
    const Frame& frame = test.procedure().frame();
    MacroAssembler::Address address = frame.addressInFrame(frame.offsetFromFPOf(test.procedure().allocation().spillOf(value)));
    return isRepresentableAs<int8_t>(static_cast<int64_t>(address.offset));
}

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
    PatchOperation* first = appendFrameTestSuspension(test, sums, roots.span());
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
    AOT_FRAME_TEST_CHECK(test, words[0] == 2 * (sumOfKept + sumOfHalfWords));
}

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
    Operation* value = out.opaque(out.add(argumentWithSeed, 1));
    Operation* sums = out.opaque(argumentWithSums);
    out.store64(value, out.address(test.heaps().argumentBuffer, sums, 3 * sizeof(uint64_t)));
    Block* rare = out.newBlock();
    Block* end = out.newBlock();
    out.branch(out.testNonZero64(choice, 1), rareEdge(rare), normalEdge(end));

    out.appendTo(rare);
    appendFrameTestSuspension(test, sums, singleElementSpan(value));
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
    AOT_FRAME_TEST_CHECK(test, words[0] == a + 1 && words[3] == a + 1);
    words = { 0, poison, poison, 0 };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(a, words.data(), uint64_t { 0 }) == answer);
    AOT_FRAME_TEST_CHECK(test, !words[0] && words[3] == a + 1);
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

static void testMachineFrameRareBlockThatWaysOutOfTwoFramedPartsEnterReadsWhatLivesAcrossTheirCalls()
{
    TestProcedure test;
    Output& out = test.out();
    uint64_t noted = 0;
    uint64_t notedOnOtherWay = 0;
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* shared = out.newBlock();
    Block* first = out.newBlock();
    Block* rareJoin = out.newBlock();
    Block* begin = out.newBlock();
    auto appendReader = [&](Operation* value, uint64_t* place) {
        PatchOperation* reader = out.patch(Void);
        reader->effects = Effects::none();
        reader->effects.writesLocalState = true;
        reader->setRunsWithoutFrame(true);
        reader->appendSomeRegister(value);
        reader->setGenerator([place](CCallHelpers& jit, const PatchParameters& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg address = jit.scratchRegister();
            jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
            jit.store64(params[0].gpr(), CCallHelpers::Address(address));
        });
    };
    out.branch(out.testNonZero64(a, 16), rareEdge(first), normalEdge(begin));
    out.appendTo(begin);
    for (unsigned part = 0; part < 2; ++part) {
        Block* calls = out.newBlock();
        Block* goesOn = out.newBlock();
        Block* past = out.newBlock();
        out.branch(out.testNonZero64(a, uint64_t { 1 } << part), normalEdge(calls), normalEdge(past));
        out.appendTo(calls);
        appendFrameTestCall(test);
        out.branch(out.testNonZero64(a, 4), rareEdge(shared), normalEdge(goesOn));
        out.appendTo(goesOn);
        out.ret(out.add(out.add(a, b), 10 + part));
        out.appendTo(past);
    }
    out.ret(out.add(out.add(a, b), 1000));
    out.appendTo(first);
    appendReader(b, &notedOnOtherWay);
    out.jump(normalEdge(rareJoin));
    out.appendTo(shared);
    appendReader(out.add(a, b), &noted);
    out.jump(normalEdge(rareJoin));
    out.appendTo(rareJoin);
    out.ret(out.constInt64(7));
    compileOrFail(test);

    const Procedure& proc = test.procedure();
    const Order& order = proc.order();
    AOT_FRAME_TEST_CHECK(test, order.of(shared).isRare() && order.of(shared).entersRareRegion && order.of(shared).rareRegion == order.of(first).rareRegion && order.headOf(first) < order.headOf(shared));
    AOT_FRAME_TEST_CHECK(test, proc.allocation().headsOfFramedPart().size() == 2 && proc.allocation().usedCalleeSaves());
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 5 }, uint64_t { 7 }) == 7 && noted == 12);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 6 }, uint64_t { 8 }) == 7 && noted == 14);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 16 }, uint64_t { 9 }) == 7 && noted == 14 && notedOnOtherWay == 9);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 7 }) == 1 + 7 + 10);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 2 }, uint64_t { 7 }) == 2 + 7 + 11);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 0 }, uint64_t { 7 }) == 7 + 1000);
}

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

static void runFrameTestOfWayOutThatRareRegionLeadsBackTo(bool wayOutGoesOn, bool standsInLoopThatTurns)
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    FrameTestSetupsThatCount setups;
    FrameTestMark markOnWayOut;
    proc.setStackCheckGenerator(noteAndCountFrameTestSetup, &setups);
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* before = out.newBlock();
    Block* turning = out.newBlock();
    Block* guard = out.newBlock();
    Block* wayOut = out.newBlock();
    Block* returning = out.newBlock();
    Block* working = out.newBlock();
    Block* done = out.newBlock();
    Block* slow = out.newBlock();
    Block* slowAgain = out.newBlock();
    Block* slowOn = out.newBlock();
    Block* leaving = standsInLoopThatTurns ? out.newBlock() : nullptr;
    Block* comingRound = standsInLoopThatTurns ? out.newBlock() : nullptr;

    out.branch(out.testIsZero64(a, 32), normalEdge(before), normalEdge(returning));
    out.appendTo(before);
    ValueFromBlock firstCount = out.anchor(out.int64Zero);
    out.jump(turning);
    out.appendTo(turning);
    Operation* count = out.phi(Int64, firstCount);
    out.branch(out.testNonZero64(a, 8), rareEdge(slow), normalEdge(guard));
    out.appendTo(guard);
    out.branch(out.testNonZero64(a, 1), normalEdge(wayOut), normalEdge(working));
    out.appendTo(wayOut);
    appendFrameTestMark(out, markOnWayOut, true);
    if (wayOutGoesOn) {
        Block* onward = out.newBlock();
        out.jump(onward);
        out.appendTo(onward);
    }
    out.ret(b);
    out.appendTo(returning);
    out.ret(b);
    out.appendTo(working);
    appendFrameTestCall(test);
    Operation* nextCount = out.add(count, 1);
    out.branch(out.testNonZero64(a, 4), rareEdge(slowAgain), normalEdge(done));
    out.appendTo(done);
    if (standsInLoopThatTurns) {
        Block* last = out.newBlock();
        out.addIncomingToPhi(count, out.anchor(nextCount));
        out.branch(out.lessThan(nextCount, out.constInt64(3)), normalEdge(turning), normalEdge(leaving));
        out.appendTo(leaving);
        appendFrameTestCall(test);
        out.branch(out.testNonZero64(a, 64), rareEdge(comingRound), normalEdge(last));
        out.appendTo(comingRound);
        Operation* countAgain = out.add(nextCount, 1);
        out.addIncomingToPhi(count, out.anchor(countAgain));
        out.branch(out.lessThan(countAgain, out.constInt64(6)), normalEdge(turning), normalEdge(slowOn));
        out.appendTo(last);
    }
    out.ret(out.add(out.add(a, b), nextCount));
    out.appendTo(slow);
    appendFrameTestCall(test);
    out.branch(out.testNonZero64(a, 16), normalEdge(wayOut), normalEdge(slowOn));
    out.appendTo(slowAgain);
    appendFrameTestCall(test);
    out.jump(slowOn);
    out.appendTo(slowOn);
    appendFrameTestCall(test);
    out.ret(out.add(b, 1000));

    compileOrFail(test);
    const Frame& frame = proc.frame();
    const Allocation& allocation = proc.allocation();
    const Order& order = proc.order();
    AOT_FRAME_TEST_CHECK(test, order.of(slow).isRare() && order.of(slow).rareRegion == order.of(slowAgain).rareRegion);
    AOT_FRAME_TEST_CHECK(test, frame.coverage() == FrameCoverage::FramedPart);
    if (standsInLoopThatTurns) {
        AOT_FRAME_TEST_CHECK(test, order.of(turning).isLoopHead && order.of(working).loop == order.of(turning).loop && order.of(leaving).loop == order.of(turning).loop);
        AOT_FRAME_TEST_CHECK(test, allocation.isHeadOfFramedPart(before) && allocation.headsOfFramedPart().size() == 1 && markOnWayOut.frameIsSetUp);
    } else {
        AOT_FRAME_TEST_CHECK(test, order.of(wayOut).isLoopHead && order.of(working).loop == order.of(wayOut).loop && order.headOf(working) < order.headOf(slow));
        AOT_FRAME_TEST_CHECK(test, allocation.isHeadOfFramedPart(working) && !allocation.isInFramedPart(guard) && !allocation.isInFramedPart(wayOut));
        AOT_FRAME_TEST_CHECK(test, !frame.holdsStepsOf(guard) && !frame.holdsStepsOf(wayOut) && !markOnWayOut.frameIsSetUp);
    }
    checkFrameTestRanges(test);
    checkFrameTestLayout(test);
    struct Case {
        uint64_t a;
        uint64_t expected;
        uint64_t numberOfSetups;
    };
    const uint64_t turns = standsInLoopThatTurns ? 3 : 1;
    const uint64_t setupsOnWayOut = standsInLoopThatTurns ? 1 : 0;
    for (Case testCase : { Case { 32, 10, 0 }, Case { 1, 10, setupsOnWayOut }, Case { 0, 10 + turns, 1 }, Case { 4, 1010, 1 }, Case { 8, 1010, 1 }, Case { 24, 10, 1 }, Case { 64, standsInLoopThatTurns ? uint64_t { 1010 } : uint64_t { 75 }, 1 } }) {
        setups.numberRun = 0;
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(testCase.a, uint64_t { 10 }) == testCase.expected && setups.numberRun == testCase.numberOfSetups);
    }
}

static void testMachineFrameWayOutThatRareRegionLeadsBackToHoldsNoHeadBack() { runFrameTestOfWayOutThatRareRegionLeadsBackTo(false, false); }
static void testMachineFrameWayOutThatGoesOnAndNeverComesRoundHoldsNoHeadBack() { runFrameTestOfWayOutThatRareRegionLeadsBackTo(true, false); }
static void testMachineFrameRareWayThatComesRoundKeepsHeadAheadOfLoop() { runFrameTestOfWayOutThatRareRegionLeadsBackTo(false, true); }

static void testMachineFrameLoopThatTurnsInsideStretchThatNeverTurnsFindsFrameSetUpAheadOfIt()
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    FrameTestSetupsThatCount setups;
    FrameTestMark markOnWayOut;
    proc.setStackCheckGenerator(noteAndCountFrameTestSetup, &setups);
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* guard = out.newBlock();
    Block* wayOut = out.newBlock();
    Block* working = out.newBlock();
    Block* turning = out.newBlock();
    Block* done = out.newBlock();
    Block* last = out.newBlock();
    Block* slow = out.newBlock();
    Block* slowAgain = out.newBlock();
    Block* slowOn = out.newBlock();

    out.branch(out.testNonZero64(a, 8), rareEdge(slow), normalEdge(guard));
    out.appendTo(guard);
    out.branch(out.testNonZero64(a, 1), normalEdge(wayOut), normalEdge(working));
    out.appendTo(wayOut);
    appendFrameTestMark(out, markOnWayOut, true);
    out.ret(b);
    out.appendTo(working);
    ValueFromBlock firstCount = out.anchor(out.int64Zero);
    out.jump(turning);
    out.appendTo(turning);
    Operation* count = out.phi(Int64, firstCount);
    appendFrameTestCall(test);
    Operation* nextCount = out.add(count, 1);
    out.addIncomingToPhi(count, out.anchor(nextCount));
    out.branch(out.lessThan(nextCount, out.constInt64(3)), normalEdge(turning), normalEdge(done));
    out.appendTo(done);
    out.branch(out.testNonZero64(a, 4), rareEdge(slowAgain), normalEdge(last));
    out.appendTo(last);
    out.ret(out.add(out.add(a, b), nextCount));
    out.appendTo(slow);
    appendFrameTestCall(test);
    out.branch(out.testNonZero64(a, 16), normalEdge(wayOut), normalEdge(slowOn));
    out.appendTo(slowAgain);
    appendFrameTestCall(test);
    out.jump(slowOn);
    out.appendTo(slowOn);
    appendFrameTestCall(test);
    out.ret(out.add(b, 1000));

    compileOrFail(test);
    const Frame& frame = proc.frame();
    const Allocation& allocation = proc.allocation();
    const Order& order = proc.order();
    AOT_FRAME_TEST_CHECK(test, order.of(wayOut).isLoopHead && order.of(turning).isLoopHead && order.of(working).loop == order.of(wayOut).loop);
    AOT_FRAME_TEST_CHECK(test, order.loops()[order.of(turning).loop].parent == order.of(wayOut).loop && order.loops()[order.of(wayOut).loop].parent == noLoop);
    AOT_FRAME_TEST_CHECK(test, frame.coverage() == FrameCoverage::FramedPart && allocation.isHeadOfFramedPart(working) && allocation.isInFramedPart(turning) && !allocation.isHeadOfFramedPart(turning));
    AOT_FRAME_TEST_CHECK(test, !allocation.isInFramedPart(guard) && !allocation.isInFramedPart(wayOut) && !frame.holdsStepsOf(wayOut) && !markOnWayOut.frameIsSetUp);
    checkFrameTestRanges(test);
    checkFrameTestLayout(test);
    struct Case {
        uint64_t a;
        uint64_t expected;
        uint64_t numberOfSetups;
    };
    for (Case testCase : { Case { 1, 10, 0 }, Case { 0, 13, 1 }, Case { 4, 1010, 1 }, Case { 8, 1010, 1 }, Case { 24, 10, 1 } }) {
        setups.numberRun = 0;
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(testCase.a, uint64_t { 10 }) == testCase.expected && setups.numberRun == testCase.numberOfSetups);
    }
}

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
    Block* slow = nullptr;
    auto appendSafepointThatWritesPointerDown = [&](int32_t offset) {
        PatchOperation* safepoint = out.patch(Void);
        safepoint->appendSomeRegister(destination);
        safepoint->clobberLate(clobbered);
        safepoint->setGenerator([clobbered, reference, offset](CCallHelpers& jit, const PatchParameters& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg pointer = params[0].gpr() == GPRInfo::argumentGPR0 ? GPRInfo::argumentGPR1 : GPRInfo::argumentGPR0;
            SpillIndex spill = findFrameTestPointerBeside(params.proc().allocation(), reference);
            AOT_TEST_CHECK(params.frameIsSetUp() && spill != noSpill);
            jit.load64(CCallHelpers::Address(CCallHelpers::framePointerRegister, params.frame().offsetFromFPOf(spill)), pointer);
            jit.store64(pointer, CCallHelpers::Address(params[0].gpr(), offset));
            emitFrameTestPoison(jit, clobbered);
        });
    };
    if (passesRareSafepoint) {
        slow = out.newBlock();
        Block* guard = out.newBlock();
        out.branch(out.testNonZero64(argument, 2), rareEdge(slow), normalEdge(guard));
        out.appendTo(slow);
        appendSafepointThatWritesPointerDown(sizeof(uint64_t));
        out.jump(guard);
        out.appendTo(guard);
    }
    out.branch(out.testNonZero64(argument, 1), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    out.ret(out.constInt64(5));
    out.appendTo(calling);
    appendSafepointThatWritesPointerDown(0);
    out.ret(out.zeroExtPtr(reference));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.liveness().of(reference).has(ValueFlag::NeedsPointerBesideIt) && allocation.onlyHeadOfFramedPart() == calling);
    AOT_FRAME_TEST_CHECK(test, !allocation.needsFrameAheadOfFramedPart() && proc.frame().coverage() == FrameCoverage::FramedPart);
    AOT_FRAME_TEST_CHECK(test, !proc.frame().holdsStepsOf(reference->owner()) && !proc.frame().holdsStepsOf(returning));
    AOT_FRAME_TEST_CHECK(test, !slow || (proc.order().of(slow).isRare() && !allocation.isInFramedPart(slow)));
    SpillIndex pointer = findFrameTestPointerBeside(allocation, reference);
    AOT_FRAME_TEST_CHECK(test, pointer != noSpill);
    auto numberOfStoresIn = [&](const Block* block) {
        const BlockSteps& steps = allocation.stepsOf(block);
        unsigned number = 0;
        for (uint32_t index = steps.begin; index < steps.end; ++index)
            number += allocation.step(index).kind == StepKind::KeepPointer && allocation.step(index).destination == Location::spill(pointer);
        return number;
    };
    unsigned numberOfStores = 0;
    for (const Step& step : allocation.steps())
        numberOfStores += step.kind == StepKind::KeepPointer;
    AOT_FRAME_TEST_CHECK(test, numberOfStoresIn(calling) == 1 && (!slow || numberOfStoresIn(slow) == 1) && numberOfStores == (slow ? 2u : 1u));

    for (uint64_t low : { 0x80000100u, 0x80000101u, 0x80000102u, 0x80000103u }) {
        uint64_t word = (uint64_t { 7 } << 32) + low;
        std::array<uint64_t, 2> found { };
        bool returnsAtOnce = word & 1;
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, found.data()) == (returnsAtOnce ? uint64_t { 5 } : low));
        AOT_FRAME_TEST_CHECK(test, found[0] == (returnsAtOnce ? uint64_t { 0 } : low << cell32Shift));
        AOT_FRAME_TEST_CHECK(test, found[1] == (slow && (word & 2) ? low << cell32Shift : uint64_t { 0 }));
    }
}

static void testMachineFrameKeepsPointerAtHeadOfFramedPart() { runFrameTestOfNarrowReferenceAheadOfGuard(false); }

enum class WayWithoutReference : uint8_t {
    OutOfEntry,
    PastBlockLaidBehindDefinition,
    OutOfPatchThatDefinesIt,
};

static void runFrameTestOfReferenceThatSomeWayHasNotDefined(WayWithoutReference way)
{
    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* argument = test.argumentGPR(0);
    Block* defining = out.newBlock();
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();
    Block* done = out.newBlock();
    Block* late = out.newBlock();
    Block* other = out.newBlock();
    Block* meeting = out.newBlock();

    Block* mid = way == WayWithoutReference::PastBlockLaidBehindDefinition ? out.newBlock() : nullptr;
    Operation* reference = nullptr;
    if (way == WayWithoutReference::OutOfPatchThatDefinesIt) {
        PatchOperation* patch = out.patch(Int32);
        patch->appendSomeRegister(argument);
        patch->effects = Effects::none();
        patch->effects.terminal = true;
        patch->setRunsWithoutFrame(true);
        patch->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
            jit.zeroExtend32ToWord(params[1].gpr(), params[0].gpr());
            CCallHelpers::Jump toOther = jit.branchTest32(CCallHelpers::Zero, params[0].gpr(), CCallHelpers::TrustedImm32(4));
            CCallHelpers::Jump toDefining;
            if (!params.fallsThroughToSuccessor(0))
                toDefining = jit.jump();
            MacroAssembler::Label* labelOfDefining = params.successorLabels()[0];
            MacroAssembler::Label* labelOfOther = params.successorLabels()[1];
            params.addLatePath([=](CCallHelpers& jit) {
                toOther.linkTo(*labelOfOther, &jit);
                if (toDefining.isSet())
                    toDefining.linkTo(*labelOfDefining, &jit);
            });
        });
        out.appendSuccessor(normalEdge(defining));
        out.appendSuccessor(normalEdge(other));
        reference = patch;
    } else
        out.branch(out.testNonZero64(argument, 4), normalEdge(defining), normalEdge(mid ? mid : other));
    if (mid) {
        Block* leaving = out.newBlock();
        out.appendTo(mid);
        out.branch(out.testNonZero64(argument, 8), normalEdge(other), normalEdge(leaving));
        out.appendTo(leaving);
        out.ret(out.constInt64(11));
    }
    out.appendTo(defining);
    if (!reference)
        reference = out.opaque(out.castToInt32(argument));
    reference->markAsNarrowReference();
    out.branch(out.testNonZero64(argument, 1), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    out.ret(out.constInt64(5));
    out.appendTo(calling);
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
    Operation* storage = out.loadPtr(out.address(storageOfOwner, out.shl(out.zeroExtPtr(reference), out.constInt32(cell32Shift))));
    appendFrameTestCall(test)->effects.mayCollect = true;
    out.branch(out.testNonZero64(out.load64(out.address(test.heaps().argumentBuffer, storage, 0)), 2), rareEdge(late), normalEdge(done));
    out.appendTo(done);
    ValueFromBlock fromDone = out.anchor(out.constInt64(7));
    out.jump(meeting);
    out.appendTo(late);
    ValueFromBlock fromLate = out.anchor(out.zeroExtPtr(reference));
    out.jump(meeting);
    out.appendTo(other);
    appendFrameTestCall(test);
    ValueFromBlock fromOther = out.anchor(out.constInt64(9));
    out.jump(meeting);
    out.appendTo(meeting);
    out.ret(out.phi(Int64, fromDone, fromLate, fromOther));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    const Order& order = proc.order();
    AOT_FRAME_TEST_CHECK(test, proc.liveness().of(reference).has(ValueFlag::NeedsPointerBesideIt) && proc.liveness().of(reference).has(ValueFlag::IsKeptForCollector) && allocation.isSpilled(reference) && proc.frame().coverage() == FrameCoverage::FramedPart);
    AOT_FRAME_TEST_CHECK(test, order.headOf(defining) < order.headOf(other));
    AOT_FRAME_TEST_CHECK(test, !mid || (order.headOf(defining) < order.headOf(mid) && order.headOf(mid) < order.headOf(other) && !allocation.isInFramedPart(mid)));
    AOT_FRAME_TEST_CHECK(test, order.headOf(other) < order.headOf(late) && order.of(late).isRare() && !order.of(other).isRare());
    const BlockSteps& stepsOfKeeper = allocation.stepsOf(calling);
    unsigned numberOfStores = 0;
    for (uint32_t index = 0; index < allocation.steps().size(); ++index) {
        if (allocation.step(index).kind != StepKind::KeepPointer)
            continue;
        ++numberOfStores;
        AOT_FRAME_TEST_CHECK(test, stepsOfKeeper.begin <= index && index < stepsOfKeeper.end);
    }
    AOT_FRAME_TEST_CHECK(test, numberOfStores == 1);

    constexpr uint64_t high = uint64_t { 7 } << 32;
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(high + 0x80000108u) == 9);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(high + 0x80000100u) == (mid ? 11u : 9u));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(high + 0x80000105u) == 5);
}

static void testMachineFrameKeepsNoPointerWhereSomeWayHasNotDefinedReference() { runFrameTestOfReferenceThatSomeWayHasNotDefined(WayWithoutReference::OutOfEntry); }
static void testMachineFrameKeepsNoPointerWhereWayPastLaterBlockHasNotDefinedReference() { runFrameTestOfReferenceThatSomeWayHasNotDefined(WayWithoutReference::PastBlockLaidBehindDefinition); }
static void testMachineFrameKeepsNoPointerBehindOtherSuccessorOfPatchThatDefinesReference() { runFrameTestOfReferenceThatSomeWayHasNotDefined(WayWithoutReference::OutOfPatchThatDefinesIt); }

static void testMachineFrameCoversWholeFunctionWhereStoreAtHeadOfDeclinedFramedPartIsItsOnlyUseOutsideRareRegions()
{
    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    const AbstractHeap& frameSlots = test.heaps().frameSlots;
    StackSlot* slot = proc.addStackSlot(sizeof(uint64_t));
    RegisterFile file = registerFileOfFrameTest(test);
    const unsigned numberOfRegisters = numberOfFrameTestRegisters(file.allocatableIn(Bank::GP) & ~file.calleeSaved);
    const unsigned numberMadeInTurn = numberOfRegisters - 4;
    constexpr uint64_t numberOfTurns = 3;

    Operation* argument = test.argumentGPR(0);
    Operation* first = test.argumentGPR(1);
    Operation* second = test.argumentGPR(2);
    RegisterMask takenInLoop = maskOf(Reg(GPRInfo::argumentGPR1)) | maskOf(Reg(GPRInfo::argumentGPR2));
    Block* leaving = out.newBlock();
    Block* inFront = out.newBlock();
    Block* turning = out.newBlock();
    Block* behind = out.newBlock();
    Block* calling = out.newBlock();
    Block* onward = out.newBlock();
    Block* shared = out.newBlock();

    out.branch(out.testNonZero64(argument, 1), normalEdge(inFront), normalEdge(leaving));
    out.appendTo(leaving);
    ValueFromBlock fromLeaving = out.anchor(out.constInt64(5));
    out.jump(shared);
    out.appendTo(inFront);
    ValueFromBlock firstTurn = out.anchor(out.int64Zero);
    ValueFromBlock firstSum = out.anchor(out.int64Zero);
    out.jump(turning);
    out.appendTo(turning);
    Operation* turn = out.phi(Int64, firstTurn);
    Operation* sum = out.phi(Int64, firstSum);
    PatchOperation* taking = out.patch(Void);
    taking->effects = Effects::none();
    taking->effects.writesLocalState = true;
    taking->setRunsWithoutFrame(true);
    taking->clobberLate(takenInLoop);
    taking->setGenerator([takenInLoop](CCallHelpers& jit, const PatchParameters&) {
        emitFrameTestPoison(jit, takenInLoop);
    });
    Vector<Operation*> madeInTurn = defineFrameTestValues(out, argument, numberMadeInTurn);
    Operation* nextSum = addUpFrameTestValues(out, sum, madeInTurn.span());
    Operation* nextTurn = out.add(turn, 1);
    out.addIncomingToPhi(turn, out.anchor(nextTurn));
    out.addIncomingToPhi(sum, out.anchor(nextSum));
    out.branch(out.below(nextTurn, out.constInt64(numberOfTurns)), normalEdge(turning), normalEdge(behind));
    out.appendTo(behind);
    ValueFromBlock fromBehind = out.anchor(nextSum);
    out.branch(out.testNonZero64(argument, 2), rareEdge(calling), normalEdge(onward));
    out.appendTo(calling);
    out.store64(out.add(first, second), out.address(frameSlots, out.slotBase(slot)));
    appendFrameTestCall(test);
    ValueFromBlock fromCalling = out.anchor(out.load64(out.address(frameSlots, out.slotBase(slot))));
    out.jump(onward);
    out.appendTo(onward);
    ValueFromBlock fromOnward = out.anchor(out.add(out.phi(Int64, fromBehind, fromCalling), 2));
    out.jump(shared);
    out.appendTo(shared);
    out.ret(out.add(out.phi(Int64, fromLeaving, fromOnward), 1));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, allocation.hasFramedPart() && !allocation.needsFrameAheadOfFramedPart() && !allocation.usedCalleeSaves());
    AOT_FRAME_TEST_CHECK(test, allocation.isInFramedPart(onward) && !allocation.isInFramedPart(shared) && proc.order().of(calling).isRare());
    for (const Operation* value : { first, second })
        AOT_FRAME_TEST_CHECK(test, allocation.isSpilled(value) && allocation.spills()[allocation.spillOf(value)].isStoredAtHeadOfFramedPart);
    for (const SpillFacts& spill : allocation.spills())
        AOT_FRAME_TEST_CHECK(test, spill.value == first || spill.value == second || spill.isSaveInRareRegion);
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::WholeFunction);

    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 4 }, uint64_t { 100 }, uint64_t { 20 }) == 6);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 5 }, uint64_t { 100 }, uint64_t { 20 }) == numberOfTurns * sumOfFrameTestValues(5, numberMadeInTurn) + 3);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 7 }, uint64_t { 100 }, uint64_t { 20 }) == 123);
}

static void testMachineFrameKeepsPointerAtHeadBehindPartsThatSpilledReference()
{
    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* argument = test.argumentGPR(0);
    Block* choosing = out.newBlock();
    Block* returning = out.newBlock();
    Block* first = out.newBlock();
    Block* second = out.newBlock();
    Block* joining = out.newBlock();
    Block* ending = out.newBlock();
    Block* third = out.newBlock();

    Operation* reference = out.opaque(out.castToInt32(argument));
    reference->markAsNarrowReference();
    out.branch(out.testNonZero64(argument, 1), normalEdge(first), normalEdge(choosing));
    out.appendTo(choosing);
    out.branch(out.testNonZero64(argument, 8), normalEdge(second), normalEdge(returning));
    out.appendTo(returning);
    out.ret(out.constInt64(5));
    out.appendTo(first);
    appendFrameTestCall(test);
    ValueFromBlock fromFirst = out.anchor(out.opaque(out.constInt64(0)));
    out.jump(joining);
    out.appendTo(second);
    appendFrameTestCall(test);
    ValueFromBlock fromSecond = out.anchor(out.opaque(out.constInt64(2)));
    out.jump(joining);
    out.appendTo(joining);
    out.branch(out.testNonZero64(out.phi(Int64, fromFirst, fromSecond), 2), normalEdge(ending), normalEdge(third));
    out.appendTo(ending);
    out.ret(out.constInt64(7));
    out.appendTo(third);
    appendFrameTestCall(test);
    out.ret(out.zeroExtPtr(reference));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.liveness().of(reference).has(ValueFlag::NeedsPointerBesideIt) && allocation.isSpilled(reference) && allocation.headsOfFramedPart().size() == 3 && allocation.isHeadOfFramedPart(third));
    for (const RegisterBinding& binding : allocation.registersAtHeadOf(third))
        AOT_FRAME_TEST_CHECK(test, binding.value != reference);
    unsigned numberOfStores = 0;
    for (uint32_t index = allocation.stepsOf(third).begin; index < allocation.stepsOf(third).end; ++index)
        numberOfStores += allocation.step(index).kind == StepKind::KeepPointer;
    AOT_FRAME_TEST_CHECK(test, numberOfStores == 1);

    constexpr uint64_t high = uint64_t { 7 } << 32;
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(high + 0x80000101u) == 0x80000101u);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(high + 0x80000108u) == 7);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(high + 0x80000100u) == 5);
}
static void testMachineFrameKeepsPointerAtRareSafepointAheadOfFramedPart() { runFrameTestOfNarrowReferenceAheadOfGuard(true); }

static unsigned numberOfFrameTestPointersKeptIn(const Allocation& allocation, const Block* block)
{
    uint32_t begin = block ? allocation.stepsOf(block).begin : 0;
    uint32_t end = block ? allocation.stepsOf(block).end : static_cast<uint32_t>(allocation.steps().size());
    unsigned number = 0;
    for (uint32_t index = begin; index < end; ++index)
        number += allocation.step(index).kind == StepKind::KeepPointer;
    return number;
}

static void testMachineFrameKeepsPointerAtSafepointOfRareRegionEnteredFromFramedPartAndFromAheadOfIt()
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    RegisterMask clobbered = callerSavedRegistersOfFrameTest(test);
    Operation* argument = test.argumentGPR(0);
    Operation* destination = test.argumentGPR(1);
    Operation* reference = out.opaque(out.castToInt32(argument));
    reference->markAsNarrowReference();
    Block* guard = out.newBlock();
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();
    Block* done = out.newBlock();
    Block* slow = out.newBlock();
    auto appendSafepointThatWritesPointerDown = [&](int32_t offset) {
        PatchOperation* safepoint = out.patch(Void);
        safepoint->appendSomeRegister(destination);
        safepoint->clobberLate(clobbered);
        safepoint->setGenerator([clobbered, reference, offset](CCallHelpers& jit, const PatchParameters& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg pointer = params[0].gpr() == GPRInfo::argumentGPR0 ? GPRInfo::argumentGPR1 : GPRInfo::argumentGPR0;
            SpillIndex spill = findFrameTestPointerBeside(params.proc().allocation(), reference);
            AOT_TEST_CHECK(params.frameIsSetUp() && spill != noSpill);
            jit.load64(CCallHelpers::Address(CCallHelpers::framePointerRegister, params.frame().offsetFromFPOf(spill)), pointer);
            jit.store64(pointer, CCallHelpers::Address(params[0].gpr(), offset));
            jit.store64(CCallHelpers::TrustedImm32(0), CCallHelpers::Address(CCallHelpers::framePointerRegister, params.frame().offsetFromFPOf(spill)));
            emitFrameTestPoison(jit, clobbered);
        });
    };
    out.branch(out.testNonZero64(argument, 2), rareEdge(slow), normalEdge(guard));
    out.appendTo(guard);
    out.branch(out.testNonZero64(argument, 1), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    out.ret(out.constInt64(5));
    out.appendTo(calling);
    appendSafepointThatWritesPointerDown(0);
    out.branch(out.testNonZero64(argument, 4), rareEdge(slow), normalEdge(done));
    out.appendTo(done);
    out.ret(out.zeroExtPtr(reference));
    out.appendTo(slow);
    appendSafepointThatWritesPointerDown(sizeof(uint64_t));
    out.ret(out.add(out.zeroExtPtr(reference), 1));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.liveness().of(reference).has(ValueFlag::NeedsPointerBesideIt) && proc.order().of(slow).isRare());
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.onlyHeadOfFramedPart() == calling && !allocation.isInFramedPart(slow));
    AOT_FRAME_TEST_CHECK(test, !proc.frame().holdsStepsOf(reference->owner()) && !proc.frame().holdsStepsOf(guard) && !proc.frame().holdsStepsOf(returning));
    AOT_FRAME_TEST_CHECK(test, numberOfFrameTestPointersKeptIn(allocation, calling) == 1 && numberOfFrameTestPointersKeptIn(allocation, slow) == 1 && numberOfFrameTestPointersKeptIn(allocation, nullptr) == 2);

    for (uint64_t low : { 0x80000100u, 0x80000101u, 0x80000102u, 0x80000103u, 0x80000104u, 0x80000105u, 0x80000106u }) {
        uint64_t word = (uint64_t { 7 } << 32) + low;
        std::array<uint64_t, 2> found { };
        bool goesSlowAtOnce = word & 2;
        bool returnsAtOnce = !goesSlowAtOnce && (word & 1);
        bool calls = !goesSlowAtOnce && !returnsAtOnce;
        bool goesSlow = goesSlowAtOnce || (calls && (word & 4));
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, found.data()) == (returnsAtOnce ? uint64_t { 5 } : low + goesSlow));
        AOT_FRAME_TEST_CHECK(test, found[0] == (calls ? low << cell32Shift : uint64_t { 0 }));
        AOT_FRAME_TEST_CHECK(test, found[1] == (goesSlow ? low << cell32Shift : uint64_t { 0 }));
    }
}

enum class ShapeOfRareRegionAheadOfFramedPart : uint8_t {
    TwoSafepointsInOneBlock,
    TwoEntries,
    EndsFunction,
};

static void runFrameTestOfRareRegionAheadOfFramedPart(ShapeOfRareRegionAheadOfFramedPart shape)
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    RegisterMask clobbered = callerSavedRegistersOfFrameTest(test);
    Operation* argument = test.argumentGPR(0);
    Operation* destination = test.argumentGPR(1);
    Operation* reference = out.opaque(out.castToInt32(argument));
    reference->markAsNarrowReference();
    Block* second = out.newBlock();
    Block* guard = out.newBlock();
    Block* returning = out.newBlock();
    Block* calling = out.newBlock();
    Block* slow = out.newBlock();
    Block* otherEntry = out.newBlock();
    Block* meeting = out.newBlock();
    auto appendSafepointThatWritesPointerDown = [&](int32_t offset, bool findsPointer = true) {
        PatchOperation* safepoint = out.patch(Void);
        safepoint->appendSomeRegister(destination);
        safepoint->clobberLate(clobbered);
        safepoint->setGenerator([clobbered, reference, offset, findsPointer](CCallHelpers& jit, const PatchParameters& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg pointer = params[0].gpr() == GPRInfo::argumentGPR0 ? GPRInfo::argumentGPR1 : GPRInfo::argumentGPR0;
            SpillIndex spill = findFrameTestPointerBeside(params.proc().allocation(), reference);
            AOT_TEST_CHECK(params.frameIsSetUp() && spill != noSpill);
            if (findsPointer) {
                jit.load64(CCallHelpers::Address(CCallHelpers::framePointerRegister, params.frame().offsetFromFPOf(spill)), pointer);
                jit.store64(pointer, CCallHelpers::Address(params[0].gpr(), offset));
                jit.store64(CCallHelpers::TrustedImm32(0), CCallHelpers::Address(CCallHelpers::framePointerRegister, params.frame().offsetFromFPOf(spill)));
            }
            emitFrameTestPoison(jit, clobbered);
        });
    };
    out.branch(out.testNonZero64(argument, 2), rareEdge(slow), normalEdge(second));
    out.appendTo(second);
    out.branch(out.testNonZero64(argument, 4), rareEdge(otherEntry), normalEdge(guard));
    out.appendTo(guard);
    out.branch(out.testNonZero64(argument, 1), normalEdge(returning), normalEdge(calling));
    out.appendTo(returning);
    out.ret(out.constInt64(5));
    out.appendTo(calling);
    appendSafepointThatWritesPointerDown(0);
    out.ret(out.zeroExtPtr(reference));
    out.appendTo(slow);
    appendSafepointThatWritesPointerDown(sizeof(uint64_t), shape != ShapeOfRareRegionAheadOfFramedPart::EndsFunction);
    if (shape == ShapeOfRareRegionAheadOfFramedPart::TwoSafepointsInOneBlock)
        appendSafepointThatWritesPointerDown(2 * sizeof(uint64_t));
    if (shape == ShapeOfRareRegionAheadOfFramedPart::EndsFunction)
        out.ret(out.constInt64(6));
    else
        out.jump(meeting);
    out.appendTo(otherEntry);
    out.jump(meeting);
    out.appendTo(meeting);
    appendSafepointThatWritesPointerDown(3 * sizeof(uint64_t));
    out.ret(out.add(out.zeroExtPtr(reference), 1));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    const Order& order = proc.order();
    AOT_FRAME_TEST_CHECK(test, proc.liveness().of(reference).has(ValueFlag::NeedsPointerBesideIt) && order.of(slow).isRare() && order.of(meeting).isRare());
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.onlyHeadOfFramedPart() == calling && !allocation.isInFramedPart(slow) && !allocation.isInFramedPart(meeting));
    AOT_FRAME_TEST_CHECK(test, !proc.frame().holdsStepsOf(reference->owner()) && !proc.frame().holdsStepsOf(second) && !proc.frame().holdsStepsOf(guard) && !proc.frame().holdsStepsOf(returning));
    AOT_FRAME_TEST_CHECK(test, numberOfFrameTestPointersKeptIn(allocation, calling) == 1 && numberOfFrameTestPointersKeptIn(allocation, meeting) == 1);
    switch (shape) {
    case ShapeOfRareRegionAheadOfFramedPart::TwoSafepointsInOneBlock:
        AOT_FRAME_TEST_CHECK(test, numberOfFrameTestPointersKeptIn(allocation, slow) == 2);
        break;
    case ShapeOfRareRegionAheadOfFramedPart::TwoEntries:
        AOT_FRAME_TEST_CHECK(test, numberOfFrameTestPointersKeptIn(allocation, slow) == 1 && order.of(slow).rareRegion == order.of(meeting).rareRegion && order.of(slow).entersRareRegion && order.of(meeting).predecessors.size() == 2);
        break;
    case ShapeOfRareRegionAheadOfFramedPart::EndsFunction:
        AOT_FRAME_TEST_CHECK(test, !numberOfFrameTestPointersKeptIn(allocation, slow) && order.of(slow).rareRegion != order.of(meeting).rareRegion);
        break;
    }

    for (uint64_t low : { 0x80000100u, 0x80000101u, 0x80000102u, 0x80000103u, 0x80000104u, 0x80000105u, 0x80000106u }) {
        uint64_t word = (uint64_t { 7 } << 32) + low;
        std::array<uint64_t, 4> found { };
        uint64_t pointer = low << cell32Shift;
        bool goesSlow = word & 2;
        bool entersOtherwise = !goesSlow && (word & 4);
        bool returnsAtOnce = !goesSlow && !entersOtherwise && (word & 1);
        bool endsInSlow = goesSlow && shape == ShapeOfRareRegionAheadOfFramedPart::EndsFunction;
        bool meets = (goesSlow && !endsInSlow) || entersOtherwise;
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, found.data()) == (endsInSlow ? uint64_t { 6 } : returnsAtOnce ? uint64_t { 5 } : low + meets));
        AOT_FRAME_TEST_CHECK(test, found[0] == (!goesSlow && !entersOtherwise && !returnsAtOnce ? pointer : uint64_t { 0 }));
        AOT_FRAME_TEST_CHECK(test, found[1] == (goesSlow && !endsInSlow ? pointer : uint64_t { 0 }));
        AOT_FRAME_TEST_CHECK(test, found[2] == (goesSlow && shape == ShapeOfRareRegionAheadOfFramedPart::TwoSafepointsInOneBlock ? pointer : uint64_t { 0 }));
        AOT_FRAME_TEST_CHECK(test, found[3] == (meets ? pointer : uint64_t { 0 }));
    }
}

static void testMachineFrameKeepsPointerAtEachOfTwoSafepointsInRareBlockAheadOfFramedPart() { runFrameTestOfRareRegionAheadOfFramedPart(ShapeOfRareRegionAheadOfFramedPart::TwoSafepointsInOneBlock); }
static void testMachineFrameKeepsPointerWhereTwoEntriesOfRareRegionAheadOfFramedPartMeet() { runFrameTestOfRareRegionAheadOfFramedPart(ShapeOfRareRegionAheadOfFramedPart::TwoEntries); }
static void testMachineFrameKeepsNoPointerInRareBlockAheadOfFramedPartThatEndsFunction() { runFrameTestOfRareRegionAheadOfFramedPart(ShapeOfRareRegionAheadOfFramedPart::EndsFunction); }

static void testMachineFrameReferenceKeptForInnerPointerReadBehindRareSafepointAheadOfFramedPartHasItsPointerThere()
{
    std::array<uint64_t, 2> elements { 11, 22 };
    std::array<const uint64_t*, 1> ownerInMemory { elements.data() };

    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    RegisterMask clobbered = callerSavedRegistersOfFrameTest(test);
    Operation* argument = test.argumentGPR(0);
    Operation* destination = test.argumentGPR(1);
    Operation* base = test.argumentGPR(2);
    Operation* reference = out.opaque(out.castToInt32(argument));
    reference->markAsNarrowReference();
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
    Operation* storage = out.loadPtr(out.address(storageOfOwner, out.add(base, out.shl(out.zeroExtPtr(reference), out.constInt32(cell32Shift)))));
    Block* first = out.newBlock();
    Block* slow = out.newBlock();
    Block* on = out.newBlock();
    Block* second = out.newBlock();
    auto appendSafepointThatWritesPointerDown = [&] {
        PatchOperation* safepoint = out.patch(Void);
        safepoint->effects.mayCollect = true;
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
    };
    out.branch(out.testNonZero64(argument, 1), unlikelyEdge(second), normalEdge(first));
    out.appendTo(first);
    out.branch(out.testNonZero64(argument, 2), rareEdge(slow), normalEdge(on));
    out.appendTo(slow);
    appendSafepointThatWritesPointerDown();
    out.jump(on);
    out.appendTo(on);
    out.ret(out.load64(out.address(test.heaps().argumentBuffer, storage, 0)));
    out.appendTo(second);
    appendSafepointThatWritesPointerDown();
    out.ret(out.load64(out.address(test.heaps().argumentBuffer, storage, sizeof(uint64_t))));

    compileOrFail(test);
    const ValueFacts& facts = proc.liveness().of(reference);
    AOT_FRAME_TEST_CHECK(test, storage->has(OperationFlag::IsInnerPointer) && facts.has(ValueFlag::IsKeptForCollector) && facts.has(ValueFlag::NeedsPointerBesideIt));
    AOT_FRAME_TEST_CHECK(test, proc.order().of(slow).isRare() && !proc.order().of(second).isRare() && proc.order().of(second).orderIndex > proc.order().of(on).orderIndex);
    AOT_FRAME_TEST_CHECK(test, findFrameTestPointerBeside(proc.allocation(), reference) != noSpill);

    for (uint64_t low : { 0x80000100u, 0x80000101u, 0x80000102u, 0x80000103u }) {
        uint64_t word = (uint64_t { 7 } << 32) + low;
        uint64_t found = 0;
        bool takesSecondWay = word & 1;
        bool passesSafepoint = takesSecondWay || (word & 2);
        uint64_t baseOfOwner = reinterpret_cast<uint64_t>(ownerInMemory.data()) - (low << cell32Shift);
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, &found, baseOfOwner) == (takesSecondWay ? uint64_t { 22 } : uint64_t { 11 }));
        AOT_FRAME_TEST_CHECK(test, found == (passesSafepoint ? low << cell32Shift : uint64_t { 0 }));
    }
}

static void runFrameTestOfNarrowReferenceBesideRareSafepointAheadOfFramedPart(bool isReadBehindRareSafepoint)
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    RegisterMask clobbered = callerSavedRegistersOfFrameTest(test);
    Operation* argument = test.argumentGPR(0);
    Operation* destination = test.argumentGPR(1);
    Operation* reference = out.opaque(out.castToInt32(argument));
    reference->markAsNarrowReference();
    Block* stretch = out.newBlock();
    Block* slow = out.newBlock();
    Block* on = out.newBlock();
    Block* other = out.newBlock();
    auto appendSafepointThatWritesPointerDown = [&] {
        PatchOperation* safepoint = out.patch(Void);
        safepoint->effects.mayCollect = true;
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
    };
    out.branch(out.testNonZero64(argument, 1), unlikelyEdge(other), normalEdge(stretch));
    out.appendTo(stretch);
    out.branch(out.testNonZero64(argument, 2), rareEdge(slow), normalEdge(on));
    out.appendTo(slow);
    if (isReadBehindRareSafepoint)
        appendSafepointThatWritesPointerDown();
    else
        appendFrameTestCall(test)->effects.mayCollect = true;
    out.jump(on);
    out.appendTo(on);
    out.ret(isReadBehindRareSafepoint ? out.zeroExtPtr(reference) : out.constInt64(5));
    out.appendTo(other);
    appendSafepointThatWritesPointerDown();
    out.ret(out.zeroExtPtr(reference));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.liveness().of(reference).has(ValueFlag::NeedsPointerBesideIt) && proc.order().of(slow).isRare() && !proc.order().of(other).isRare());
    AOT_FRAME_TEST_CHECK(test, proc.order().of(other).orderIndex > proc.order().of(on).orderIndex);
    AOT_FRAME_TEST_CHECK(test, findFrameTestPointerBeside(allocation, reference) != noSpill);

    for (uint64_t low : { 0x80000100u, 0x80000101u, 0x80000102u, 0x80000103u }) {
        uint64_t word = (uint64_t { 7 } << 32) + low;
        uint64_t found = 0;
        bool takesOtherWay = word & 1;
        bool passesRareSafepoint = !takesOtherWay && (word & 2);
        bool writesPointerDown = takesOtherWay || (passesRareSafepoint && isReadBehindRareSafepoint);
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, &found) == (takesOtherWay || isReadBehindRareSafepoint ? low : uint64_t { 5 }));
        AOT_FRAME_TEST_CHECK(test, found == (writesPointerDown ? low << cell32Shift : uint64_t { 0 }));
    }
    if (isReadBehindRareSafepoint)
        return;

    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.onlyHeadOfFramedPart() == other && !allocation.needsFrameAheadOfFramedPart());
    AOT_FRAME_TEST_CHECK(test, !proc.frame().holdsStepsOf(stretch) && !proc.frame().holdsStepsOf(on));
    const BlockSteps& stepsOfKeeper = allocation.stepsOf(other);
    unsigned numberOfStores = 0;
    for (uint32_t index = 0; index < allocation.steps().size(); ++index) {
        if (allocation.step(index).kind != StepKind::KeepPointer)
            continue;
        ++numberOfStores;
        AOT_FRAME_TEST_CHECK(test, stepsOfKeeper.begin <= index && index < stepsOfKeeper.end);
    }
    AOT_FRAME_TEST_CHECK(test, numberOfStores == 1);
}

static void testMachineFrameReferenceDeadAtRareSafepointAheadOfFramedPartWaitsForHeadWithItsPointer() { runFrameTestOfNarrowReferenceBesideRareSafepointAheadOfFramedPart(false); }
static void testMachineFrameReferenceReadBehindRareSafepointAheadOfFramedPartHasItsPointerThere() { runFrameTestOfNarrowReferenceBesideRareSafepointAheadOfFramedPart(true); }

static void testMachineFrameStretchThatBindsFewValuesHasNoFrameThoughUnlikelyWayLaterInOrderReadsMore()
{
    constexpr uint64_t word = 0x1000;
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    const RegisterFile file = registerFileOfFrameTest(test);
    unsigned half = numberOfFrameTestRegisters(file.allocatableIn(Bank::GP) & ~file.calleeSaved) - 2;
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* slow = out.newBlock();
    Block* choosing = out.newBlock();
    Block* calling = out.newBlock();
    Block* stretch = out.newBlock();
    Vector<Operation*> readOnUnlikelyWayAlone;
    for (unsigned index = 0; index < half; ++index)
        readOnUnlikelyWayAlone.append(out.opaque(out.add(b, 2 * index)));
    out.branch(out.testNonZero64(a, 1), unlikelyEdge(slow), normalEdge(choosing));
    out.appendTo(choosing);
    out.branch(out.testNonZero64(a, 2), normalEdge(calling), normalEdge(stretch));
    out.appendTo(calling);
    appendFrameTestCall(test);
    out.ret(out.add(a, 100));
    out.appendTo(stretch);
    Vector<Operation*> readOnStretch;
    for (unsigned index = 0; index < half; ++index)
        readOnStretch.append(out.opaque(out.add(a, 4 * index)));
    Operation* sumOnStretch = out.int64Zero;
    for (Operation* value : readOnStretch)
        sumOnStretch = out.add(sumOnStretch, value);
    out.ret(sumOnStretch);
    out.appendTo(slow);
    appendFrameTestCall(test);
    Operation* sumOnUnlikelyWay = out.int64Zero;
    for (Operation* value : readOnUnlikelyWayAlone)
        sumOnUnlikelyWay = out.add(sumOnUnlikelyWay, value);
    out.ret(sumOnUnlikelyWay);

    compileOrFail(test);
    uint64_t expectedOnStretch = 0;
    uint64_t expectedOnUnlikelyWay = 0;
    for (unsigned index = 0; index < half; ++index) {
        expectedOnStretch += word + 4 * index;
        expectedOnUnlikelyWay += 7 + 2 * index;
    }
    AOT_FRAME_TEST_CHECK(test, !proc.order().of(slow).isRare() && proc.order().of(slow).orderIndex > proc.order().of(stretch).orderIndex);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, uint64_t { 7 }) == expectedOnStretch);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word + 1, uint64_t { 7 }) == expectedOnUnlikelyWay);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word + 2, uint64_t { 7 }) == word + 102);
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && !proc.frame().holdsStepsOf(stretch));
}

static PatchOperation* appendFrameTestCallThatLooksForWordAsCollectorDoes(TestProcedure& test, uint64_t word, uint64_t* numberOfTimesFound)
{
    RegisterMask clobbered = callerSavedRegistersOfFrameTest(test);
    RegisterMask kept = registerFileOfFrameTest(test).calleeSavedIn(Bank::GP);
    PatchOperation* call = test.out().patch(Void);
    call->clobberLate(clobbered);
    call->effects.mayCollect = true;
    call->setNeedsFrameRecord(true);
    call->setGenerator([clobbered, kept, word, numberOfTimesFound](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(params.frameIsSetUp());
        emitFrameTestPoison(jit, clobbered);
        const GPRReg cursor = GPRInfo::argumentGPR0;
        const GPRReg wanted = GPRInfo::argumentGPR1;
        const GPRReg held = GPRInfo::argumentGPR2;
        AOT_TEST_CHECK(!((maskOf(Reg(cursor)) | maskOf(Reg(wanted)) | maskOf(Reg(held))) & ~clobbered));
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(word)), wanted);
        CCallHelpers::JumpList found;
        forEachRegister(kept, [&](Reg reg) {
            found.append(jit.branch64(CCallHelpers::Equal, reg.gpr(), wanted));
        });
        jit.move(CCallHelpers::stackPointerRegister, cursor);
        CCallHelpers::Label next = jit.label();
        CCallHelpers::Jump isNotFound = jit.branch64(CCallHelpers::AboveOrEqual, cursor, CCallHelpers::framePointerRegister);
        jit.load64(CCallHelpers::Address(cursor), held);
        found.append(jit.branch64(CCallHelpers::Equal, held, wanted));
        jit.add64(CCallHelpers::TrustedImm32(sizeof(uint64_t)), cursor);
        jit.jump().linkTo(next, &jit);
        found.link(&jit);
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(numberOfTimesFound))), cursor);
        jit.load64(CCallHelpers::Address(cursor), held);
        jit.add64(CCallHelpers::TrustedImm32(1), held);
        jit.store64(held, CCallHelpers::Address(cursor));
        isNotFound.link(&jit);
        emitFrameTestPoison(jit, clobbered);
    });
    return call;
}

static void testMachineFrameSecondHeadSavesOwnerThatStepsReadUnderFirstAlone()
{
    std::array<uint64_t, 3> elements { 11, 22, 33 };
    std::array<const uint64_t*, 1> ownerInMemory { elements.data() };
    uint64_t numberOfTimesFound = 0;

    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    const unsigned numberKeptByCalls = numberOfFrameTestRegisters(registerFileOfFrameTest(test).calleeSavedIn(Bank::GP));
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
    Operation* owner = out.opaque(test.argumentGPR(0));
    Operation* selector = test.argumentGPR(1);
    Operation* storage = out.loadPtr(out.address(storageOfOwner, owner));
    Block* first = out.newBlock();
    Block* other = out.newBlock();
    Block* second = out.newBlock();
    Block* leaving = out.newBlock();
    Vector<Operation*> readMore;
    for (unsigned index = 0; index < numberKeptByCalls; ++index)
        readMore.append(out.opaque(out.add(selector, 2 + index)));
    out.branch(out.testNonZero64(selector, 1), normalEdge(first), normalEdge(other));
    out.appendTo(other);
    out.branch(out.testNonZero64(selector, 2), normalEdge(second), normalEdge(leaving));
    for (Block* head : { first, second }) {
        out.appendTo(head);
        appendFrameTestCallThatLooksForWordAsCollectorDoes(test, reinterpret_cast<uint64_t>(ownerInMemory.data()), &numberOfTimesFound);
        Operation* sum = out.load64(out.address(test.heaps().argumentBuffer, storage, 2 * sizeof(uint64_t)));
        if (head == first)
            sum = out.add(sum, out.load64(out.address(test.heaps().argumentBuffer, out.loadPtr(out.address(test.heaps().argumentBuffer, owner)), sizeof(uint64_t))));
        for (Operation* value : readMore)
            sum = out.add(out.add(sum, value), out.opaque(value));
        out.ret(sum);
    }
    out.appendTo(leaving);
    out.ret(out.add(selector, 100));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, storage->has(OperationFlag::IsInnerPointer) && proc.liveness().of(owner).has(ValueFlag::IsKeptForCollector));
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.isHeadOfFramedPart(first) && allocation.isHeadOfFramedPart(second) && !proc.frame().holdsStepsOf(leaving) && allocation.isSpilled(owner));
    for (Block* head : { first, second }) {
        const Allocation::HeadOfFramedPart& steps = allocation.headOfFramedPartAbove(head);
        bool savesOwner = false;
        for (uint32_t index = steps.beginOfSteps; index < steps.endOfSteps; ++index)
            savesOwner |= allocation.step(index).destination == Location::spill(allocation.spillOf(owner));
        AOT_FRAME_TEST_CHECK(test, savesOwner);
    }
    auto expected = [&](uint64_t chosen, uint64_t read) {
        for (unsigned index = 0; index < numberKeptByCalls; ++index)
            read += 2 * (chosen + 2 + index);
        return read;
    };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 1 }) == expected(1, 33 + 22) && numberOfTimesFound == 1);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 2 }) == expected(2, 33) && numberOfTimesFound == 2);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 4 }) == 104 && numberOfTimesFound == 2);
}

static void testMachineFrameHeadSavesOwnerThatCollectorAloneReads()
{
    std::array<uint64_t, 3> elements { 11, 22, 33 };
    std::array<const uint64_t*, 1> ownerInMemory { elements.data() };
    uint64_t numberOfTimesFound = 0;

    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    const unsigned numberKeptByCalls = numberOfFrameTestRegisters(registerFileOfFrameTest(test).calleeSavedIn(Bank::GP));
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
    Operation* owner = out.opaque(test.argumentGPR(0));
    Operation* selector = test.argumentGPR(1);
    Operation* storage = out.loadPtr(out.address(storageOfOwner, owner));
    Block* calling = out.newBlock();
    Block* leaving = out.newBlock();
    Vector<Operation*> readMore;
    for (unsigned index = 0; index < numberKeptByCalls; ++index)
        readMore.append(out.opaque(out.add(selector, 2 + index)));
    out.branch(out.testNonZero64(selector, 1), normalEdge(calling), normalEdge(leaving));
    out.appendTo(calling);
    appendFrameTestCallThatLooksForWordAsCollectorDoes(test, reinterpret_cast<uint64_t>(ownerInMemory.data()), &numberOfTimesFound);
    Operation* sum = out.load64(out.address(test.heaps().argumentBuffer, storage, 2 * sizeof(uint64_t)));
    for (Operation* value : readMore)
        sum = out.add(out.add(sum, value), out.opaque(value));
    out.ret(sum);
    out.appendTo(leaving);
    out.ret(out.add(selector, 100));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, storage->has(OperationFlag::IsInnerPointer) && proc.liveness().of(owner).has(ValueFlag::IsKeptForCollector));
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.isHeadOfFramedPart(calling) && !proc.frame().holdsStepsOf(leaving) && allocation.isSpilled(owner));
    uint64_t expected = 33;
    for (unsigned index = 0; index < numberKeptByCalls; ++index)
        expected += 2 * (1 + 2 + index);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 1 }) == expected);
    AOT_FRAME_TEST_CHECK(test, numberOfTimesFound == 1);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 2 }) == 102 && numberOfTimesFound == 1);
}

static PatchOperation* appendFrameTestPollThatLooksForWordAsCollectorDoes(TestProcedure& test, uint64_t word, uint64_t* numberOfTimesFound)
{
    constexpr GPRReg wanted = GPRInfo::argumentGPR0;
    constexpr GPRReg held = GPRInfo::argumentGPR5;
    RegisterMask clobbered = maskOf(Reg(wanted)) | maskOf(Reg(held));
    RegisterMask looked = registerFileOfFrameTest(test).allocatableIn(Bank::GP) & ~clobbered;
    PatchOperation* poll = test.out().patch(Void);
    poll->clobberEarly(clobbered);
    poll->clobberLate(clobbered);
    poll->effects.mayCollect = true;
    poll->setRunsWithoutFrame(true);
    poll->setShowsRegistersToCollector(true);
    poll->setGenerator([looked, word, numberOfTimesFound](CCallHelpers& jit, const PatchParameters& params) {
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(word)), wanted);
        CCallHelpers::JumpList found;
        forEachRegister(looked & params.usedRegistersMask(), [&](Reg reg) {
            found.append(jit.branch64(CCallHelpers::Equal, reg.gpr(), wanted));
        });
        if (params.frameIsSetUp()) {
            for (unsigned offset = 0; offset < params.frame().frameSize(); offset += sizeof(uint64_t)) {
                jit.load64(CCallHelpers::Address(CCallHelpers::stackPointerRegister, static_cast<int32_t>(offset)), held);
                found.append(jit.branch64(CCallHelpers::Equal, held, wanted));
            }
        }
        CCallHelpers::Jump isNotFound = jit.jump();
        found.link(&jit);
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(numberOfTimesFound))), wanted);
        jit.load64(CCallHelpers::Address(wanted), held);
        jit.add64(CCallHelpers::TrustedImm32(1), held);
        jit.store64(held, CCallHelpers::Address(wanted));
        isNotFound.link(&jit);
    });
    return poll;
}

static void testMachineFrameOwnerIsHeldAtPollOfLoopOutsidePartThatSpilledIt()
{
    std::array<uint64_t, 3> elements { 11, 22, 33 };
    std::array<const uint64_t*, 1> ownerInMemory { elements.data() };
    uint64_t numberOfTimesFound = 0;

    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    const unsigned numberKeptByCalls = numberOfFrameTestRegisters(registerFileOfFrameTest(test).calleeSavedIn(Bank::GP));
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
    Operation* owner = test.argumentGPR(0);
    Operation* selector = test.argumentGPR(1);
    Operation* storage = out.loadPtr(out.address(storageOfOwner, owner));
    Block* calling = out.newBlock();
    Block* goingRound = out.newBlock();
    Block* head = out.newBlock();
    Block* reading = out.newBlock();
    Block* readingRarely = out.newBlock();
    Block* latch = out.newBlock();
    Block* end = out.newBlock();
    Vector<Operation*> readMore;
    for (unsigned index = 0; index + 1 < numberKeptByCalls; ++index)
        readMore.append(out.opaque(out.add(selector, 2 + index)));
    out.branch(out.testNonZero64(selector, 1), normalEdge(calling), normalEdge(goingRound));
    out.appendTo(calling);
    appendFrameTestCall(test)->effects.mayCollect = true;
    Operation* sumOfArm = out.add(out.load64(out.address(test.heaps().argumentBuffer, storage)), out.load64(out.address(test.heaps().argumentBuffer, storage, sizeof(uint64_t))));
    for (Operation* value : readMore)
        sumOfArm = out.add(out.add(sumOfArm, value), out.opaque(value));
    ValueFromBlock sumFromCalling = out.anchor(sumOfArm);
    ValueFromBlock roundsFromCalling = out.anchor(out.constInt64(2));
    out.jump(head);
    out.appendTo(goingRound);
    ValueFromBlock sumFromGoingRound = out.anchor(out.opaque(out.add(selector, 5)));
    ValueFromBlock roundsFromGoingRound = out.anchor(out.constInt64(2));
    out.jump(head);
    out.appendTo(head);
    Operation* sum = out.phi(Int64, sumFromCalling, sumFromGoingRound);
    Operation* roundsLeft = out.phi(Int64, roundsFromCalling, roundsFromGoingRound);
    appendFrameTestPollThatLooksForWordAsCollectorDoes(test, reinterpret_cast<uint64_t>(ownerInMemory.data()), &numberOfTimesFound);
    out.branch(roundsLeft, normalEdge(reading), normalEdge(end));
    out.appendTo(reading);
    Operation* sumOfRound = out.add(sum, out.load64(out.address(test.heaps().argumentBuffer, storage, 2 * sizeof(uint64_t))));
    ValueFromBlock sumFromReading = out.anchor(sumOfRound);
    out.branch(out.testNonZero64(selector, 4), rareEdge(readingRarely), normalEdge(latch));
    out.appendTo(readingRarely);
    appendFrameTestCall(test);
    ValueFromBlock sumFromReadingRarely = out.anchor(out.add(sumOfRound, out.load64(out.address(test.heaps().argumentBuffer, storage, sizeof(uint64_t)))));
    out.jump(latch);
    out.appendTo(latch);
    out.addIncomingToPhi(sum, out.anchor(out.phi(Int64, sumFromReading, sumFromReadingRarely)));
    out.addIncomingToPhi(roundsLeft, out.anchor(out.sub(roundsLeft, 1)));
    out.jump(head);
    out.appendTo(end);
    out.ret(out.add(sum, out.load64(out.address(test.heaps().argumentBuffer, storage))));

    compileOrFail(test);
    AOT_FRAME_TEST_CHECK(test, storage->has(OperationFlag::IsInnerPointer) && proc.liveness().of(owner).has(ValueFlag::IsKeptForCollector));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 2 }) == 7 + 2 * 33 + 11);
    AOT_FRAME_TEST_CHECK(test, numberOfTimesFound == 3);
    uint64_t expected = 11 + 22 + 2 * 33 + 11;
    for (unsigned index = 0; index + 1 < numberKeptByCalls; ++index)
        expected += 2 * (3 + 2 + index);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 3 }) == expected);
    AOT_FRAME_TEST_CHECK(test, numberOfTimesFound == 6);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 6 }) == 11 + 2 * (33 + 22) + 11 && numberOfTimesFound == 9);
}

enum class WhereTwoHeadsMeet : uint8_t { InUsualBlock, InRareBlock };

static bool checkOwnerIsHeldAtPollBehindTwoHeads(unsigned numberOfSums, bool keepsFramedPart, WhereTwoHeadsMeet whereTheyMeet = WhereTwoHeadsMeet::InUsualBlock)
{
    std::array<uint64_t, 3> elements { 11, 22, 33 };
    std::array<const uint64_t*, 1> ownerInMemory { elements.data() };
    uint64_t numberOfTimesFound = 0;

    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    const unsigned numberKeptByCalls = numberOfFrameTestRegisters(registerFileOfFrameTest(test).calleeSavedIn(Bank::GP));
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
    Operation* owner = test.argumentGPR(0);
    Operation* selector = test.argumentGPR(1);
    Operation* storage = out.loadPtr(out.address(storageOfOwner, owner));
    Block* first = out.newBlock();
    Block* choosing = out.newBlock();
    Block* second = out.newBlock();
    Block* leaving = out.newBlock();
    Block* behind = out.newBlock();
    Vector<Operation*> readMore;
    for (unsigned index = 0; index + 1 < numberKeptByCalls; ++index)
        readMore.append(out.opaque(out.add(selector, 2 + index)));
    out.branch(out.testNonZero64(selector, 1), normalEdge(first), normalEdge(choosing));
    out.appendTo(choosing);
    out.branch(out.testNonZero64(selector, 2), normalEdge(second), normalEdge(leaving));
    out.appendTo(leaving);
    out.ret(out.add(selector, 100));
    auto appendArm = [&](Block* arm, unsigned offset) {
        out.appendTo(arm);
        appendFrameTestCall(test)->effects.mayCollect = true;
        Operation* sum = out.add(out.load64(out.address(test.heaps().argumentBuffer, storage, offset)), out.load64(out.address(test.heaps().argumentBuffer, storage, offset)));
        for (Operation* value : readMore)
            sum = out.add(out.add(sum, value), out.opaque(value));
        Vector<ValueFromBlock> results;
        results.append(out.anchor(sum));
        for (Operation* value : defineFrameTestValues(out, sum, numberOfSums))
            results.append(out.anchor(value));
        if (whereTheyMeet == WhereTwoHeadsMeet::InRareBlock) {
            Block* returning = out.newBlock();
            out.branch(out.testNonZero64(selector, 8), rareEdge(behind), normalEdge(returning));
            out.appendTo(returning);
            out.ret(sum);
        } else
            out.jump(behind);
        return results;
    };
    Vector<ValueFromBlock> fromFirst = appendArm(first, 0);
    Vector<ValueFromBlock> fromSecond = appendArm(second, sizeof(uint64_t));
    out.appendTo(behind);
    Vector<Operation*> sums;
    for (unsigned index = 0; index <= numberOfSums; ++index)
        sums.append(out.phi(Int64, fromFirst[index], fromSecond[index]));
    appendFrameTestPollThatLooksForWordAsCollectorDoes(test, reinterpret_cast<uint64_t>(ownerInMemory.data()), &numberOfTimesFound);
    out.ret(addUpFrameTestValues(out, out.load64(out.address(test.heaps().argumentBuffer, storage, 2 * sizeof(uint64_t))), sums.span()));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, storage->has(OperationFlag::IsInnerPointer) && proc.liveness().of(owner).has(ValueFlag::IsKeptForCollector));
    if (keepsFramedPart)
        AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.isHeadOfFramedPart(first) && allocation.isHeadOfFramedPart(second) && !allocation.isInFramedPart(behind) && allocation.isSpilled(owner));
    if (keepsFramedPart) {
        AOT_FRAME_TEST_CHECK(test, std::ranges::any_of(allocation.registersAtHeadOf(behind), [&](const RegisterBinding& binding) {
            return binding.value == owner;
        }));
    }
    auto expectedOf = [&](uint64_t chosen, uint64_t element) {
        uint64_t sum = 2 * element;
        for (unsigned index = 0; index + 1 < numberKeptByCalls; ++index)
            sum += 2 * (chosen + 2 + index);
        return 33 + sum + sumOfFrameTestValues(sum, numberOfSums);
    };
    AOT_FRAME_TEST_CHECK(test, proc.order().of(behind).isRare() == (whereTheyMeet == WhereTwoHeadsMeet::InRareBlock));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 9 }) == expectedOf(9, 11) && numberOfTimesFound == 1);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 10 }) == expectedOf(10, 22) && numberOfTimesFound == 2);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(ownerInMemory.data(), uint64_t { 4 }) == 104 && numberOfTimesFound == 2);
    return proc.frame().coverage() == FrameCoverage::FramedPart;
}

static void testMachineFrameOwnerThatPartSpilledIsLoadedBackWhereWayLeavesPart()
{
    checkOwnerIsHeldAtPollBehindTwoHeads(0, true);
}

static void testMachineFrameOwnerThatPartSpilledIsHeldThoughNoRegisterIsFreeWhereWayLeavesPart()
{
    RegisterFile file = registerFileFor(ProcedureTraits());
    const unsigned numberNoCallerCountsOn = numberOfFrameTestRegisters(file.allocatableIn(Bank::GP) & ~file.calleeSavedIn(Bank::GP));
    unsigned numberThatKeepFramedPart = 0;
    bool framedPartWasGivenUp = false;
    for (unsigned numberOfSums = 0; numberOfSums <= numberNoCallerCountsOn + 1; ++numberOfSums) {
        bool keepsFramedPart = checkOwnerIsHeldAtPollBehindTwoHeads(numberOfSums, false);
        AOT_TEST_CHECK(!keepsFramedPart || !framedPartWasGivenUp);
        numberThatKeepFramedPart += keepsFramedPart;
        framedPartWasGivenUp |= !keepsFramedPart;
    }
    AOT_TEST_CHECK(numberThatKeepFramedPart && framedPartWasGivenUp);
}

static void testMachineFrameOwnerThatPartSpilledIsLoadedBackWhereWayLeavesPartIntoRareBlock()
{
    checkOwnerIsHeldAtPollBehindTwoHeads(0, true, WhereTwoHeadsMeet::InRareBlock);
}

static void testMachineFrameHeadSavesOnlyWhatIsReadUnderIt()
{
    constexpr uint64_t word = 0x1000;
    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    const unsigned numberKeptByCalls = numberOfFrameTestRegisters(registerFileOfFrameTest(test).calleeSavedIn(Bank::GP));
    Operation* a = test.argumentGPR(0);
    Block* first = out.newBlock();
    Block* choosing = out.newBlock();
    Block* second = out.newBlock();
    Block* leaving = out.newBlock();
    Operation* readLeast = out.opaque(out.add(a, 1));
    Vector<Operation*> readMore;
    for (unsigned index = 0; index < numberKeptByCalls; ++index)
        readMore.append(out.opaque(out.add(a, 2 + index)));
    out.branch(out.testNonZero64(a, 1), normalEdge(first), normalEdge(choosing));
    out.appendTo(first);
    appendFrameTestCall(test);
    Operation* sum = readLeast;
    for (Operation* value : readMore)
        sum = out.add(out.add(sum, value), out.opaque(value));
    out.ret(sum);
    out.appendTo(choosing);
    out.branch(out.testNonZero64(a, 2), normalEdge(second), normalEdge(leaving));
    out.appendTo(second);
    appendFrameTestCall(test);
    Operation* betweenCalls = out.opaque(readLeast);
    appendFrameTestCall(test);
    out.ret(out.add(out.add(betweenCalls, readLeast), out.opaque(readLeast)));
    out.appendTo(leaving);
    out.ret(out.add(a, 100));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    uint64_t expectedOfFirst = word + 2;
    for (unsigned index = 0; index < numberKeptByCalls; ++index)
        expectedOfFirst += 2 * (word + 1 + 2 + index);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word + 1) == expectedOfFirst);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word + 2) == 3 * (word + 3));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word) == word + 100);
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.isHeadOfFramedPart(first) && allocation.isHeadOfFramedPart(second) && allocation.isSpilled(readLeast));
    const Allocation::HeadOfFramedPart& headOfFirst = allocation.headOfFramedPartAbove(first);
    const Allocation::HeadOfFramedPart& headOfSecond = allocation.headOfFramedPartAbove(second);
    AOT_FRAME_TEST_CHECK(test, headOfFirst.endOfSteps - headOfFirst.beginOfSteps == 1 && allocation.step(headOfFirst.beginOfSteps).operation == readLeast);
    AOT_FRAME_TEST_CHECK(test, headOfSecond.endOfSteps == headOfSecond.beginOfSteps);
}

static void testMachineFrameHeadSavesArgumentThatColdOperandAloneReadsUnderIt()
{
    constexpr uint64_t word = 0x1000;
    constexpr uint64_t wordOfArgument = 0x1111222233334444ull;
    std::array<uint64_t, 2> noted { };
    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* a = test.argumentGPR(0);
    Operation* argument = test.argumentGPR(1);
    Block* first = out.newBlock();
    Block* choosing = out.newBlock();
    Block* second = out.newBlock();
    Block* leaving = out.newBlock();
    out.branch(out.testNonZero64(a, 1), normalEdge(first), normalEdge(choosing));
    out.appendTo(first);
    for (uint64_t& place : noted) {
        appendFrameTestCall(test);
        PatchOperation* reader = out.patch(Void);
        reader->effects = Effects::none();
        reader->effects.writesLocalState = true;
        reader->append(argument, ValueRep::reg(GPRInfo::argumentGPR2));
        reader->setGenerator([place = &place](CCallHelpers& jit, const PatchParameters& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg address = jit.scratchRegister();
            jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
            jit.store64(params[0].gpr(), CCallHelpers::Address(address));
        });
    }
    out.ret(out.constInt64(1));
    out.appendTo(choosing);
    out.branch(out.testNonZero64(a, 2), normalEdge(second), normalEdge(leaving));
    out.appendTo(second);
    appendFrameTestCall(test);
    PatchOperation* coldReader = out.patch(Int64);
    coldReader->effects = Effects::none();
    coldReader->effects.writesLocalState = true;
    coldReader->setFindsOperandsInFrameWithoutRecord(true);
    coldReader->append(argument, ValueRep::ColdAny);
    coldReader->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(params.frameIsSetUp() && params.size() == 2 && params[1].isStack());
        jit.load64(params.frame().addressInFrame(params[1].offsetFromFP()), params[0].gpr());
    });
    out.ret(coldReader);
    out.appendTo(leaving);
    out.ret(out.add(a, 100));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.isHeadOfFramedPart(first) && allocation.isHeadOfFramedPart(second));
    AOT_FRAME_TEST_CHECK(test, allocation.isSpilled(argument) && !allocation.usedCalleeSaves());
    const Allocation::HeadOfFramedPart& headOfSecond = allocation.headOfFramedPartAbove(second);
    AOT_FRAME_TEST_CHECK(test, headOfSecond.endOfSteps - headOfSecond.beginOfSteps == 1 && allocation.step(headOfSecond.beginOfSteps).operation == argument);
    AOT_FRAME_TEST_CHECK(test, !allocation.leavesOutSavesAtHeads());
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word + 2, wordOfArgument) == wordOfArgument);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word + 1, wordOfArgument) == 1 && noted[0] == wordOfArgument && noted[1] == wordOfArgument);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, wordOfArgument) == word + 100);
}

static void testMachineFrameSaveThatNothingReadsUnderItsHeadIsLeftOutAndNoted()
{
    constexpr uint64_t word = 0x1000;
    constexpr uint64_t wordOfArgument = 0x1111222233334444ull;
    std::array<uint64_t, 3> noted { };
    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* a = test.argumentGPR(0);
    Operation* argument = test.argumentGPR(1);
    Block* first = out.newBlock();
    Block* choosing = out.newBlock();
    Block* second = out.newBlock();
    Block* leaving = out.newBlock();
    auto note = [&](uint64_t& place, ValueRep constraint) {
        PatchOperation* reader = out.patch(Void);
        reader->effects = Effects::none();
        reader->effects.writesLocalState = true;
        reader->append(argument, constraint);
        reader->setGenerator([place = &place](CCallHelpers& jit, const PatchParameters& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg address = jit.scratchRegister();
            jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(place)), address);
            jit.store64(params[0].gpr(), CCallHelpers::Address(address));
        });
    };
    out.branch(out.testNonZero64(a, 1), normalEdge(first), normalEdge(choosing));
    out.appendTo(first);
    note(noted[0], ValueRep::reg(GPRInfo::argumentGPR2));
    appendFrameTestCall(test);
    out.ret(out.constInt64(1));
    out.appendTo(choosing);
    out.branch(out.testNonZero64(a, 2), normalEdge(second), normalEdge(leaving));
    out.appendTo(second);
    for (unsigned place = 1; place < noted.size(); ++place) {
        appendFrameTestCall(test);
        note(noted[place], ValueRep::reg(GPRInfo::argumentGPR2));
    }
    out.ret(out.constInt64(2));
    out.appendTo(leaving);
    out.ret(out.add(a, 100));

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.isHeadOfFramedPart(first) && allocation.isHeadOfFramedPart(second));
    AOT_FRAME_TEST_CHECK(test, allocation.isSpilled(argument) && !allocation.usedCalleeSaves());
    const Allocation::HeadOfFramedPart& headOfFirst = allocation.headOfFramedPartAbove(first);
    const Allocation::HeadOfFramedPart& headOfSecond = allocation.headOfFramedPartAbove(second);
    AOT_FRAME_TEST_CHECK(test, headOfFirst.endOfSteps == headOfFirst.beginOfSteps);
    AOT_FRAME_TEST_CHECK(test, headOfSecond.endOfSteps - headOfSecond.beginOfSteps == 1 && allocation.step(headOfSecond.beginOfSteps).operation == argument);
    AOT_FRAME_TEST_CHECK(test, allocation.leavesOutSavesAtHeads());
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word + 1, wordOfArgument) == 1 && noted[0] == wordOfArgument && !noted[1] && !noted[2]);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word + 2, wordOfArgument) == 2 && noted[1] == wordOfArgument && noted[2] == wordOfArgument);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, wordOfArgument) == word + 100);
}

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

static void testMachineFrameBothHeadsAboveMeetingSaveWhatStepsReadInMeetingAlone()
{
    ProcedureTraits traits;
    traits.chargesForCalleeSaves = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    const unsigned numberKeptByCalls = numberOfFrameTestRegisters(registerFileOfFrameTest(test).calleeSavedIn(Bank::GP));
    Operation* a = test.argumentGPR(0);
    Operation* readInMeetingAlone = out.opaque(test.argumentGPR(1));
    Block* returning = out.newBlock();
    Block* guard = out.newBlock();
    Block* slow = out.newBlock();
    Block* head = out.newBlock();
    Block* meeting = out.newBlock();
    Vector<Operation*> readMore;
    for (unsigned index = 0; index < numberKeptByCalls; ++index)
        readMore.append(out.opaque(out.add(a, 2 + index)));

    out.branch(out.testNonZero64(a, 2), rareEdge(slow), normalEdge(guard));
    out.appendTo(guard);
    out.branch(out.testNonZero64(a, 1), normalEdge(returning), normalEdge(head));
    out.appendTo(returning);
    out.ret(out.add(a, 100));
    out.appendTo(head);
    appendFrameTestCall(test);
    ValueFromBlock fromHead = out.anchor(out.int64Zero);
    out.jump(meeting);
    out.appendTo(slow);
    appendFrameTestCall(test);
    ValueFromBlock fromRareWay = out.anchor(out.constInt64(1000));
    out.jump(meeting);
    out.appendTo(meeting);
    Operation* sum = out.phi(Int64, fromHead, fromRareWay);
    appendFrameTestCall(test);
    sum = out.add(sum, readInMeetingAlone);
    for (Operation* value : readMore)
        sum = out.add(out.add(sum, value), out.opaque(value));
    out.ret(sum);

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && proc.order().of(slow).isRare());
    AOT_FRAME_TEST_CHECK(test, allocation.isHeadOfFramedPart(head) && allocation.isHeadOfFramedPart(slow) && allocation.isInFramedPart(meeting) && !allocation.isHeadOfFramedPart(meeting) && allocation.isSpilled(readInMeetingAlone));
    unsigned numberOfHeadsThatSaveIt = 0;
    allocation.everyHeadOfFramedPartAbove(meeting, [&](const Allocation::HeadOfFramedPart& above) {
        for (uint32_t index = above.beginOfSteps; index < above.endOfSteps; ++index)
            numberOfHeadsThatSaveIt += allocation.step(index).destination == Location::spill(allocation.spillOf(readInMeetingAlone));
        return true;
    });
    AOT_FRAME_TEST_CHECK(test, numberOfHeadsThatSaveIt == 2);
    auto expected = [&](uint64_t chosen, uint64_t read) {
        for (unsigned index = 0; index < numberKeptByCalls; ++index)
            read += 2 * (chosen + 2 + index);
        return read;
    };
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 10 }) == 101);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 4 }, uint64_t { 10 }) == expected(4, 10));
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 6 }, uint64_t { 10 }) == expected(6, 1010));
}

static void testMachineFrameRareWayThatComesBackInBehindHeadJoinsFramedPart()
{
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    FrameTestSetupsThatCount setups;
    proc.setStackCheckGenerator(noteAndCountFrameTestSetup, &setups);
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* returning = out.newBlock();
    Block* guard = out.newBlock();
    Block* slow = out.newBlock();
    Block* head = out.newBlock();
    Block* meeting = out.newBlock();

    out.branch(out.testNonZero64(a, 2), rareEdge(slow), normalEdge(guard));
    out.appendTo(guard);
    out.branch(out.testNonZero64(a, 1), normalEdge(returning), normalEdge(head));
    out.appendTo(returning);
    out.ret(b);
    out.appendTo(head);
    appendFrameTestCall(test);
    ValueFromBlock fromHead = out.anchor(out.int64Zero);
    out.jump(meeting);
    out.appendTo(slow);
    appendFrameTestCall(test);
    ValueFromBlock fromRareWay = out.anchor(out.constInt64(1000));
    out.jump(meeting);
    out.appendTo(meeting);
    Operation* extra = out.phi(Int64, fromHead, fromRareWay);
    appendFrameTestCall(test);
    out.ret(out.add(out.add(a, b), extra));

    compileOrFail(test);
    const Frame& frame = proc.frame();
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, frame.coverage() == FrameCoverage::FramedPart && proc.order().of(slow).isRare());
    AOT_FRAME_TEST_CHECK(test, allocation.isHeadOfFramedPart(head) && allocation.isHeadOfFramedPart(slow) && allocation.isInFramedPart(meeting) && !allocation.isHeadOfFramedPart(meeting));
    AOT_FRAME_TEST_CHECK(test, frame.isSetUpAtHeadOf(head) && frame.isSetUpAtHeadOf(slow) && !frame.isSetUpAtHeadOf(meeting) && !frame.isTornDownAtEndOf(head) && !frame.isTornDownAtEndOf(slow));
    unsigned numberOfHeadsAboveMeeting = 0;
    allocation.everyHeadOfFramedPartAbove(meeting, [&](const Allocation::HeadOfFramedPart& above) {
        AOT_FRAME_TEST_CHECK(test, above.block == head || above.block == slow);
        ++numberOfHeadsAboveMeeting;
        return true;
    });
    AOT_FRAME_TEST_CHECK(test, numberOfHeadsAboveMeeting == 2);
    checkFrameTestRanges(test);
    checkFrameTestLayout(test);
    struct Case {
        uint64_t a;
        uint64_t expected;
        uint64_t numberOfSetups;
    };
    for (Case testCase : { Case { 1, 10, 0 }, Case { 4, 14, 1 }, Case { 6, 1016, 1 } }) {
        setups.numberRun = 0;
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(testCase.a, uint64_t { 10 }) == testCase.expected && setups.numberRun == testCase.numberOfSetups);
    }
}

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

static void testMachineFrameCallThatEndsFunctionTakesItsOperandsWithoutSpareRegisters()
{
    constexpr unsigned numberOfOperands = 6;
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* operands[numberOfOperands];
    for (unsigned index = 0; index < numberOfOperands; ++index)
        operands[index] = test.argumentGPR(index);
    Block* leaving = out.newBlock();
    Block* calling = out.newBlock();

    out.branch(out.testNonZero64(operands[1], 1), normalEdge(leaving), normalEdge(calling));
    out.appendTo(leaving);
    PatchOperation* leave = out.patch(Void);
    for (unsigned index = 0; index < numberOfOperands; ++index)
        leave->append(operands[(index + 1) % numberOfOperands], ValueRep::reg(operands[index]->argumentReg()));
    leave->effects = Effects::none();
    leave->effects.writesLocalState = true;
    leave->effects.terminal = true;
    leave->setRunsWithoutFrame(true);
    leave->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(!params.frameIsSetUp() && params.size() == numberOfOperands);
        jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
        for (unsigned index = 1; index < numberOfOperands; ++index) {
            jit.lshift64(CCallHelpers::TrustedImm32(4), GPRInfo::returnValueGPR);
            jit.add64(params[index].gpr(), GPRInfo::returnValueGPR);
        }
        jit.ret();
    });
    out.appendTo(calling);
    appendFrameTestCall(test);
    Operation* sum = operands[0];
    for (unsigned index = 1; index < numberOfOperands; ++index)
        sum = out.add(sum, operands[index]);
    out.ret(sum);

    compileOrFail(test);
    const Frame& frame = proc.frame();
    const Allocation& allocation = proc.allocation();
    AOT_FRAME_TEST_CHECK(test, frame.coverage() == FrameCoverage::FramedPart && allocation.onlyHeadOfFramedPart() == calling && !frame.holdsStepsOf(leaving));
    RegisterMask named = 0;
    for (Operation* operand : operands)
        named |= maskOf(operand->argumentReg());
    const BlockSteps& steps = allocation.stepsOf(leaving);
    unsigned numberOfRegistersBesideNamedOnes = 0;
    RegisterMask seen = named;
    for (uint32_t index = steps.begin; index < steps.end; ++index) {
        const Step& step = allocation.step(index);
        AOT_FRAME_TEST_CHECK(test, step.kind != StepKind::Reload && step.kind != StepKind::Save);
        if (step.kind != StepKind::Move || !step.destination.isRegister() || (seen & maskOf(step.destination.reg())))
            continue;
        seen |= maskOf(step.destination.reg());
        ++numberOfRegistersBesideNamedOnes;
    }
    AOT_FRAME_TEST_CHECK(test, numberOfRegistersBesideNamedOnes <= 1);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 3 }, uint64_t { 4 }, uint64_t { 5 }, uint64_t { 6 }, uint64_t { 7 }) == 0x45671);
    AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 2 }, uint64_t { 4 }, uint64_t { 5 }, uint64_t { 6 }, uint64_t { 7 }) == 25);
}

static void testMachineFrameKeepsPointerAtSafepointOfRareRegionInLoopBehindFramedPart()
{
    constexpr uint64_t low = 0x80000100u;
    constexpr uint64_t numberOfTurns = 3;
    uint64_t numberOfTimesFound = 0;

    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* reference = out.opaque(out.castToInt32(test.argumentGPR(0)));
    reference->markAsNarrowReference();
    Operation* selector = test.argumentGPR(1);
    Block* calling = out.newBlock();
    Block* passing = out.newBlock();
    Block* turning = out.newBlock();
    Block* slow = out.newBlock();
    Block* on = out.newBlock();
    Block* done = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), normalEdge(calling), normalEdge(passing));
    out.appendTo(calling);
    appendFrameTestCallThatLooksForWordAsCollectorDoes(test, low << cell32Shift, &numberOfTimesFound);
    ValueFromBlock countBehindCall = out.anchor(out.int64Zero);
    ValueFromBlock sumBehindCall = out.anchor(out.int64Zero);
    out.jump(turning);
    out.appendTo(passing);
    ValueFromBlock countWithoutCall = out.anchor(out.int64Zero);
    ValueFromBlock sumWithoutCall = out.anchor(out.int64Zero);
    out.jump(turning);
    out.appendTo(turning);
    Operation* count = out.phi(Int64, countBehindCall, countWithoutCall);
    Operation* sum = out.phi(Int64, sumBehindCall, sumWithoutCall);
    out.branch(out.testNonZero64(selector, 2), rareEdge(slow), normalEdge(on));
    out.appendTo(slow);
    appendFrameTestCallThatLooksForWordAsCollectorDoes(test, low << cell32Shift, &numberOfTimesFound);
    out.jump(on);
    out.appendTo(on);
    Operation* nextSum = out.add(sum, out.zeroExtPtr(reference));
    Operation* nextCount = out.add(count, 1);
    out.addIncomingToPhi(count, out.anchor(nextCount));
    out.addIncomingToPhi(sum, out.anchor(nextSum));
    out.branch(out.lessThan(nextCount, out.constInt64(numberOfTurns)), normalEdge(turning), normalEdge(done));
    out.appendTo(done);
    out.ret(nextSum);

    compileOrFail(test);
    const Allocation& allocation = proc.allocation();
    const Order& order = proc.order();
    AOT_FRAME_TEST_CHECK(test, proc.liveness().of(reference).has(ValueFlag::NeedsPointerBesideIt) && order.of(turning).isLoopHead && order.of(slow).isRare() && order.of(slow).loop == order.of(turning).loop);
    AOT_FRAME_TEST_CHECK(test, proc.frame().coverage() == FrameCoverage::FramedPart && allocation.onlyHeadOfFramedPart() == calling && !allocation.isInFramedPart(turning) && !allocation.isInFramedPart(slow) && !allocation.isInFramedPart(on));
    AOT_FRAME_TEST_CHECK(test, !proc.frame().holdsStepsOf(reference->owner()) && !proc.frame().holdsStepsOf(passing) && !proc.frame().holdsStepsOf(turning) && !proc.frame().holdsStepsOf(on) && !proc.frame().holdsStepsOf(done));
    AOT_FRAME_TEST_CHECK(test, numberOfFrameTestPointersKeptIn(allocation, calling) == 1 && numberOfFrameTestPointersKeptIn(allocation, slow) == 1 && numberOfFrameTestPointersKeptIn(allocation, nullptr) == 2);

    uint64_t word = (uint64_t { 7 } << 32) + low;
    for (uint64_t ways = 0; ways < 4; ++ways) {
        numberOfTimesFound = 0;
        AOT_FRAME_TEST_CHECK(test, test.call<uint64_t>(word, ways) == numberOfTurns * low);
        AOT_FRAME_TEST_CHECK(test, numberOfTimesFound == (ways & 1) + (ways & 2 ? numberOfTurns : 0));
    }
}

#undef AOT_FRAME_TEST_CHECK

} // namespace Machine


std::span<const NativeTest> machineFrameTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/frame/rare-blocks-with-calls-have-frames-of-their-own"_s, Machine::testMachineFrameCoversRareBlocksAlone },
        { "machine/frame/a-head-saves-the-owner-that-the-collector-alone-reads"_s, Machine::testMachineFrameHeadSavesOwnerThatCollectorAloneReads },
        { "machine/frame/a-second-head-saves-the-owner-that-steps-read-under-the-first-alone"_s, Machine::testMachineFrameSecondHeadSavesOwnerThatStepsReadUnderFirstAlone },
        { "machine/frame/patch-that-may-collect-without-a-frame-shows-its-registers-or-ends-the-function"_s, Machine::testMachineFramePatchThatMayCollectWithoutFrameShowsItsRegistersOrEndsFunction },
        { "machine/frame/the-owner-is-held-at-the-poll-of-a-loop-outside-the-part-that-spilled-it"_s, Machine::testMachineFrameOwnerIsHeldAtPollOfLoopOutsidePartThatSpilledIt },
        { "machine/frame/an-owner-that-the-part-spilled-is-loaded-back-where-the-way-leaves-the-part"_s, Machine::testMachineFrameOwnerThatPartSpilledIsLoadedBackWhereWayLeavesPart },
        { "machine/frame/an-owner-that-the-part-spilled-is-loaded-back-where-the-way-leaves-the-part-into-a-rare-block"_s, Machine::testMachineFrameOwnerThatPartSpilledIsLoadedBackWhereWayLeavesPartIntoRareBlock },
        { "machine/frame/an-owner-that-the-part-spilled-is-held-though-no-register-is-free-where-the-way-leaves-the-part"_s, Machine::testMachineFrameOwnerThatPartSpilledIsHeldThoughNoRegisterIsFreeWhereWayLeavesPart },
        { "machine/frame/a-head-saves-only-what-is-read-under-it"_s, Machine::testMachineFrameHeadSavesOnlyWhatIsReadUnderIt },
        { "machine/frame/a-head-saves-the-argument-that-a-cold-operand-alone-reads-under-it"_s, Machine::testMachineFrameHeadSavesArgumentThatColdOperandAloneReadsUnderIt },
        { "machine/frame/a-save-that-nothing-reads-under-its-head-is-left-out-and-noted"_s, Machine::testMachineFrameSaveThatNothingReadsUnderItsHeadIsLeftOutAndNoted },
        { "machine/frame/a-stretch-that-binds-few-values-has-no-frame-though-an-unlikely-way-later-in-the-order-reads-more"_s, Machine::testMachineFrameStretchThatBindsFewValuesHasNoFrameThoughUnlikelyWayLaterInOrderReadsMore },
        { "machine/frame/patch-that-tests-the-stack-itself-stands-ahead-of-the-framed-part"_s, Machine::testMachineFramePatchThatTestsStackItselfStandsAheadOfFramedPart },
        { "machine/frame/patch-that-tests-the-stack-itself-gets-a-frame-where-nothing-else-needs-one"_s, Machine::testMachineFramePatchThatTestsStackItselfGetsFrameWhereNothingElseNeedsOne },
        { "machine/frame/small-rare-region-stands-among-the-other-code"_s, Machine::testMachineSmallRareRegionStandsAmongOtherCode },
        { "machine/frame/small-run-of-unlikely-blocks-stands-near-its-jump"_s, Machine::testMachineSmallRunOfUnlikelyBlocksStandsNearItsJump },
        { "machine/frame/handlers-rejection-and-suspension-set-it-up-at-entry"_s, Machine::testMachineFrameIsSetUpAtEntryWhereTraitsAskForIt },
        { "machine/frame/callee-saves-ascend-below-the-record-and-come-back"_s, Machine::testMachineFrameCalleeSavesAscendBelowRecord },
        { "machine/frame/function-that-saves-its-frame"_s, Machine::testMachineFrameOfFunctionThatSavesItsFrame },
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
        { "machine/frame/rare-block-that-ways-out-of-two-framed-parts-enter-reads-what-lives-across-their-calls"_s, Machine::testMachineFrameRareBlockThatWaysOutOfTwoFramedPartsEnterReadsWhatLivesAcrossTheirCalls },
        { "machine/frame/loop-that-needs-it-finds-it-set-up"_s, Machine::testMachineFrameIsSetUpAheadOfLoopThatNeedsIt },
        { "machine/frame/a-way-out-that-a-rare-region-leads-back-to-holds-no-head-back"_s, Machine::testMachineFrameWayOutThatRareRegionLeadsBackToHoldsNoHeadBack },
        { "machine/frame/a-way-out-that-goes-on-and-never-comes-round-holds-no-head-back"_s, Machine::testMachineFrameWayOutThatGoesOnAndNeverComesRoundHoldsNoHeadBack },
        { "machine/frame/a-rare-way-that-comes-round-keeps-the-head-ahead-of-the-loop"_s, Machine::testMachineFrameRareWayThatComesRoundKeepsHeadAheadOfLoop },
        { "machine/frame/a-loop-that-turns-inside-a-stretch-that-never-turns-finds-the-frame-set-up-ahead-of-it"_s, Machine::testMachineFrameLoopThatTurnsInsideStretchThatNeverTurnsFindsFrameSetUpAheadOfIt },
        { "machine/frame/whatever-writes-on-the-way-to-it-follows-the-setup"_s, Machine::testMachineFrameIsSetUpAheadOfWhateverWritesOnWayToIt },
        { "machine/frame/what-is-defined-ahead-of-it-is-stored-at-its-head"_s, Machine::testMachineFrameStoresAtHeadWhatIsDefinedAheadOfIt },
        { "machine/frame/pointer-beside-a-reference-from-ahead-of-it-is-kept-at-its-head"_s, Machine::testMachineFrameKeepsPointerAtHeadOfFramedPart },
        { "machine/frame/no-pointer-is-kept-where-some-way-has-not-defined-the-reference"_s, Machine::testMachineFrameKeepsNoPointerWhereSomeWayHasNotDefinedReference },
        { "machine/frame/no-pointer-is-kept-where-a-way-past-a-later-block-has-not-defined-the-reference"_s, Machine::testMachineFrameKeepsNoPointerWhereWayPastLaterBlockHasNotDefinedReference },
        { "machine/frame/no-pointer-is-kept-behind-the-other-successor-of-the-patch-that-defines-the-reference"_s, Machine::testMachineFrameKeepsNoPointerBehindOtherSuccessorOfPatchThatDefinesReference },
        { "machine/frame/the-pointer-is-kept-at-a-head-behind-parts-that-spilled-the-reference"_s, Machine::testMachineFrameKeepsPointerAtHeadBehindPartsThatSpilledReference },
        { "machine/frame/a-reference-kept-for-an-inner-pointer-that-is-read-behind-a-rare-safepoint-ahead-of-the-framed-part-has-its-pointer-there"_s, Machine::testMachineFrameReferenceKeptForInnerPointerReadBehindRareSafepointAheadOfFramedPartHasItsPointerThere },
        { "machine/frame/a-reference-that-is-dead-at-a-rare-safepoint-ahead-of-the-framed-part-waits-for-a-head-with-its-pointer"_s, Machine::testMachineFrameReferenceDeadAtRareSafepointAheadOfFramedPartWaitsForHeadWithItsPointer },
        { "machine/frame/a-reference-that-is-read-behind-a-rare-safepoint-ahead-of-the-framed-part-has-its-pointer-there"_s, Machine::testMachineFrameReferenceReadBehindRareSafepointAheadOfFramedPartHasItsPointerThere },
        { "machine/frame/the-whole-function-is-covered-where-a-store-at-the-head-of-a-declined-framed-part-is-its-only-use-outside-rare-regions"_s, Machine::testMachineFrameCoversWholeFunctionWhereStoreAtHeadOfDeclinedFramedPartIsItsOnlyUseOutsideRareRegions },
        { "machine/frame/safepoint-ahead-of-it-finds-the-pointer-kept"_s, Machine::testMachineFrameKeepsPointerAtRareSafepointAheadOfFramedPart },
        { "machine/frame/safepoint-of-a-rare-region-entered-from-it-and-from-ahead-of-it-finds-the-pointer-kept"_s, Machine::testMachineFrameKeepsPointerAtSafepointOfRareRegionEnteredFromFramedPartAndFromAheadOfIt },
        { "machine/frame/each-of-two-safepoints-in-a-rare-block-ahead-of-it-finds-the-pointer-kept"_s, Machine::testMachineFrameKeepsPointerAtEachOfTwoSafepointsInRareBlockAheadOfFramedPart },
        { "machine/frame/safepoint-where-two-entries-of-a-rare-region-ahead-of-it-meet-finds-the-pointer-kept"_s, Machine::testMachineFrameKeepsPointerWhereTwoEntriesOfRareRegionAheadOfFramedPartMeet },
        { "machine/frame/rare-block-ahead-of-it-that-ends-the-function-keeps-no-pointer"_s, Machine::testMachineFrameKeepsNoPointerInRareBlockAheadOfFramedPartThatEndsFunction },
        { "machine/frame/safepoint-of-a-rare-region-in-a-loop-behind-it-finds-the-pointer-kept-in-every-turn"_s, Machine::testMachineFrameKeepsPointerAtSafepointOfRareRegionInLoopBehindFramedPart },
        { "machine/frame/arm-that-runs-out-of-registers-is-framed-by-itself"_s, Machine::testMachineFrameIsSetUpForArmThatRunsOutOfRegisters },
        { "machine/frame/rare-region-ahead-of-it-has-a-frame-of-its-own"_s, Machine::testMachineFrameOfRareRegionAheadOfFramedPartIsItsOwn },
        { "machine/frame/a-rare-way-that-comes-back-in-behind-the-head-joins-the-framed-part"_s, Machine::testMachineFrameRareWayThatComesBackInBehindHeadJoinsFramedPart },
        { "machine/frame/both-heads-above-a-meeting-save-what-steps-read-in-the-meeting-alone"_s, Machine::testMachineFrameBothHeadsAboveMeetingSaveWhatStepsReadInMeetingAlone },
        { "machine/frame/block-of-a-rare-region-does-not-read-what-its-sibling-saved"_s, Machine::testMachineFrameBlockOfRareRegionDoesNotReadWhatItsSiblingSaved },
        { "machine/frame/head-stands-where-every-way-on-calls"_s, Machine::testMachineFrameHeadStandsWhereEveryWayOnCalls },
        { "machine/frame/way-that-leaves-it-tears-it-down"_s, Machine::testMachineFrameIsTornDownWhereWayLeavesFramedPart },
        { "machine/frame/way-without-it-returns-in-place-of-a-jump"_s, Machine::testMachineFrameWayWithoutFrameReturnsInPlaceOfJump },
        { "machine/frame/call-that-ends-the-function-takes-its-operands-without-spare-registers"_s, Machine::testMachineFrameCallThatEndsFunctionTakesItsOperandsWithoutSpareRegisters },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
