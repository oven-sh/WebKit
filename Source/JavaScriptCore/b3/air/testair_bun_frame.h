/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

static B3::Policy policyWithAirFrame()
{
    B3::Policy policy;
    letAirEmitFrame(policy.air);
    policy.air.frame.shrinkWraps = false;
    return policy;
}

static void appendCall(B3::Procedure& proc, BasicBlock* block)
{
    block->append(Patch, addCallLikePatchpoint(proc), Arg::special(patchpointSpecialOf(proc.code())));
}

enum class FrameLayoutCase : uint8_t { Default, SlotsAboveFrameRecord, CalleeSavesPushedAtFrameBottom, SpillsPushedAtFrameSetup, SlotsOrderedByUse };

static constexpr FrameLayoutCase allFrameLayoutCases[] = {
    FrameLayoutCase::Default, FrameLayoutCase::SlotsAboveFrameRecord, FrameLayoutCase::CalleeSavesPushedAtFrameBottom,
    FrameLayoutCase::SpillsPushedAtFrameSetup, FrameLayoutCase::SlotsOrderedByUse,
};

static B3::Policy policyWithFrameLayout(FrameLayoutCase layoutCase)
{
    B3::Policy policy = policyWithAirFrame();
    FramePolicy& frame = policy.air.frame;
    switch (layoutCase) {
    case FrameLayoutCase::Default:
        break;
    case FrameLayoutCase::SlotsAboveFrameRecord:
        frame.mayPlaceSlotsAboveFrameRecord = true;
        break;
    case FrameLayoutCase::CalleeSavesPushedAtFrameBottom:
        frame.mayPushCalleeSaves = true;
        break;
    case FrameLayoutCase::SpillsPushedAtFrameSetup:
        frame.mayPushCalleeSaves = true;
        frame.mayPushSpillsAtFrameSetup = true;
        break;
    case FrameLayoutCase::SlotsOrderedByUse:
        frame.ordersSlotsByUse = true;
        frame.prefersStackPointerAddresses = true;
        break;
    }
    return policy;
}

static CalleeSaveForm expectedCalleeSaveForm(FrameLayoutCase layoutCase, bool hasCalleeSaves)
{
    if (!hasCalleeSaves)
        return CalleeSaveForm::Stored;
    if (isARM64() && layoutCase == FrameLayoutCase::SlotsAboveFrameRecord)
        return CalleeSaveForm::PushedBelowFrameRecord;
    if (!isX86_64())
        return CalleeSaveForm::Stored;
    if (layoutCase == FrameLayoutCase::CalleeSavesPushedAtFrameBottom || layoutCase == FrameLayoutCase::SpillsPushedAtFrameSetup)
        return CalleeSaveForm::PushedAtFrameBottom;
    return CalleeSaveForm::PushedBelowFrameRecord;
}

static void checkFrameLayout(Code& code, FrameLayoutCase layoutCase, bool hasCalleeSaves)
{
    const FrameLayout& layout = code.frameLayout();
    CHECK(layout.isPlanned);
    CHECK(layout.hasFrame);
    CHECK(layout.coverage == FrameCoverage::EveryPathBecauseShrinkWrappingIsOff);
    CHECK((layout.slotArea == SlotArea::AboveFrameRecord) == (isARM64() && layoutCase == FrameLayoutCase::SlotsAboveFrameRecord));
    CHECK(layout.calleeSaveForm == expectedCalleeSaveForm(layoutCase, hasCalleeSaves));
    CHECK(!!layout.numberOfSpillsPushedAtFrameSetup == (isX86_64() && layoutCase == FrameLayoutCase::SpillsPushedAtFrameSetup));
}

struct FunctionWithFrame {
    static constexpr unsigned maxNumberOfCalleeSaves = 3;
    static constexpr unsigned numberOfSpilledArguments = 2;
    static constexpr int64_t savedValue = 3;
    static constexpr int64_t spilledValues[numberOfSpilledArguments] = { 5, 7 };

    FunctionWithFrame(B3::Procedure& proc, unsigned numberOfCalleeSaves, unsigned numberOfSpillSlots, bool spillsVector = false)
    {
        constexpr GPRReg calleeSaves[maxNumberOfCalleeSaves] = { GPRInfo::regCS0, GPRInfo::regCS1, GPRInfo::regCS2 };
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        Tmp buffer = argumentTmp(3);
        Tmp scratch = argumentTmp(4);
        Tmp vector(FPRInfo::fpRegT0);

        for (unsigned i = 0; i < numberOfCalleeSaves; ++i)
            root->append(Move, nullptr, argumentTmp(0), Tmp(calleeSaves[i]));
        Vector<StackSlot*> slots;
        for (unsigned i = 0; i < numberOfSpillSlots; ++i) {
            slots.append(code.addStackSlot(8, StackSlotKind::Spill));
            if (i < numberOfSpilledArguments) {
                root->append(Move, nullptr, argumentTmp(i + 1), Arg::stack(slots[i]));
                expectedResult += spilledValues[i];
                continue;
            }
            root->append(Move, nullptr, argumentTmp(1), scratch);
            root->append(Add64, nullptr, Arg::imm(i), scratch);
            root->append(Move, nullptr, scratch, Arg::stack(slots[i]));
            expectedResult += spilledValues[0] + i;
        }
        StackSlot* bufferSlot = nullptr;
        StackSlot* smallSlot = nullptr;
        if (spillsVector) {
            bufferSlot = code.addStackSlot(8, StackSlotKind::Spill);
            smallSlot = code.addStackSlot(4, StackSlotKind::Spill);
            vectorSlot = code.addStackSlot(16, StackSlotKind::Spill);
            root->append(Move, nullptr, buffer, Arg::stack(bufferSlot));
            root->append(Move32, nullptr, argumentTmp(2), Arg::stack(smallSlot));
            root->append(MoveVector, nullptr, Arg::addr(buffer), vector);
            root->append(MoveVector, nullptr, vector, Arg::stack(vectorSlot));
        }

        appendCall(proc, root);

        root->append(Move, nullptr, Arg::imm(0), returnTmp());
        for (StackSlot* slot : slots) {
            root->append(Move, nullptr, Arg::stack(slot), scratch);
            root->append(Add64, nullptr, scratch, returnTmp());
        }
        if (spillsVector) {
            root->append(Move32, nullptr, Arg::stack(smallSlot), scratch);
            root->append(Add64, nullptr, scratch, returnTmp());
            expectedResult += spilledValues[1];
            root->append(Move, nullptr, Arg::stack(bufferSlot), buffer);
            root->append(MoveVector, nullptr, Arg::stack(vectorSlot), vector);
            root->append(MoveVector, nullptr, vector, Arg::addr(buffer, 16));
        }
        for (unsigned i = 0; i < numberOfCalleeSaves; ++i) {
            root->append(Add64, nullptr, Tmp(calleeSaves[i]), returnTmp());
            expectedResult += savedValue;
        }
        appendReturn(root);
    }

    void checkResult(const Compilation& compilation) const
    {
        alignas(16) uint64_t buffer[4] = { 0x0123456789abcdefull, 0xfedcba9876543210ull, 0, 0 };
        CHECK(invoke<int64_t>(compilation, savedValue, spilledValues[0], spilledValues[1], buffer) == expectedResult);
        if (!vectorSlot)
            return;
        CHECK(buffer[2] == buffer[0]);
        CHECK(buffer[3] == buffer[1]);
    }

    StackSlot* vectorSlot { nullptr };
    int64_t expectedResult { 0 };
};

static void buildTwoRareCalls(B3::Procedure& proc)
{
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* next = code.addBlock();
    BasicBlock* done = code.addBlock();
    BasicBlock* calls[] = { code.addBlock(0.01), code.addBlock(0.01) };
    appendBranchTest(root, argumentTmp(0), rarely(calls[0]), next);
    appendBranchTest(next, argumentTmp(1), rarely(calls[1]), done);
    appendReturn(done, 0);
    for (unsigned i = 0; i < std::size(calls); ++i) {
        StackSlot* slot = code.addStackSlot(8, StackSlotKind::Spill);
        calls[i]->append(Move, nullptr, argumentTmp(2), Arg::stack(slot));
        appendCall(proc, calls[i]);
        calls[i]->append(Move, nullptr, Arg::stack(slot), returnTmp());
        calls[i]->append(Add64, nullptr, Arg::imm(i), returnTmp());
        appendReturn(calls[i]);
    }
}

static void checkTwoRareCalls(const Compilation& compilation)
{
    CHECK(invoke<int64_t>(compilation, 1ll, 0ll, 9ll) == 9);
    CHECK(invoke<int64_t>(compilation, 0ll, 1ll, 9ll) == 10);
    CHECK(invoke<int64_t>(compilation, 0ll, 0ll, 9ll) == 0);
}

void testCalleeSavesAreFoundAfterSpillFixing()
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    StackSlot* slot = code.addStackSlot(8, StackSlotKind::Spill);
    Tmp saved(GPRInfo::regCS0);
    root->append(Move, nullptr, argumentTmp(0), Arg::stack(slot));
    root->append(Move, nullptr, Arg::stack(slot), saved);
    root->append(Move, nullptr, saved, returnTmp());
    appendReturn(root);

    auto compilation = compile(proc);
    CHECK(code.calleeSaveRegisters().contains(GPRInfo::regCS0, IgnoreVectors));
    CHECK(!code.calleeSaveRegisters().contains(GPRInfo::regCS1, IgnoreVectors));
    CHECK(invoke<int64_t>(*compilation, 9ll) == 9);
}

#if CPU(ARM64) || CPU(X86_64)
static void checkSameCalleeSaves(const RegisterSet& actual, const RegisterSet& expected)
{
#if CPU(ARM64)
    constexpr GPRReg calleeSaves[] = {
        ARM64Registers::x19, ARM64Registers::x20, ARM64Registers::x21, ARM64Registers::x22, ARM64Registers::x23,
        ARM64Registers::x24, ARM64Registers::x25, ARM64Registers::x26, ARM64Registers::x27, ARM64Registers::x28,
    };
#else
    constexpr GPRReg calleeSaves[] = { X86Registers::ebx, X86Registers::r12, X86Registers::r13, X86Registers::r14, X86Registers::r15 };
#endif
    for (GPRReg reg : calleeSaves)
        CHECK(actual.contains(reg, IgnoreVectors) == expected.contains(reg, IgnoreVectors));
}
#endif

void testSuspensionSuffix()
{
#if CPU(ARM64) || CPU(X86_64)
    auto checkSavedRegisters = [](RegisterSet suspensionSet, std::optional<GPRReg> named, RegisterSet clobberedByPatch, RegisterSet expected) {
        B3::Policy policy = policyWithAirFrame();
        policy.air.registers.calleeSavesClobberedAtSuspensions = suspensionSet;
        B3::Procedure proc(policy);
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        if (!clobberedByPatch.isEmpty()) {
            B3::PatchpointValue* patchpoint = addPatchpoint(proc);
            patchpoint->clobberLate(clobberedByPatch);
            patchpoint->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
            root->append(Patch, patchpoint, Arg::special(patchpointSpecialOf(code)));
        }
        if (named) {
            root->append(Move, nullptr, argumentTmp(0), Tmp(*named));
            root->append(Move, nullptr, Tmp(*named), returnTmp());
        } else
            root->append(Move, nullptr, argumentTmp(0), returnTmp());
        appendReturn(root);

        auto compilation = compile(proc);
        CHECK(invoke<int64_t>(*compilation, 5ll) == 5);
        checkSameCalleeSaves(code.calleeSaveRegisters(), expected);
        RegisterSet expectedSuffix = expected;
        expectedSuffix.filter(suspensionSet);
        CHECK(code.frameLayout().numberOfSavesClobberedAtSuspensions == expectedSuffix.numberOfSetRegisters());
    };
#endif

#if CPU(ARM64)
    using namespace ARM64Registers;
    RegisterSet suspensionSet(x19, x20, x21, x22, x23, x24);
    checkSavedRegisters(suspensionSet, x22, RegisterSet(), RegisterSet(x21, x22, x23, x24));
    checkSavedRegisters(suspensionSet, x19, RegisterSet(), suspensionSet);
    checkSavedRegisters(suspensionSet, std::nullopt, suspensionSet, RegisterSet());
    checkSavedRegisters(RegisterSet(), x22, RegisterSet(), RegisterSet(x22));
#elif CPU(X86_64)
    RegisterSet suspensionSet(X86Registers::r15);
    checkSavedRegisters(suspensionSet, X86Registers::r15, RegisterSet(), suspensionSet);
    checkSavedRegisters(suspensionSet, std::nullopt, suspensionSet, RegisterSet());
    checkSavedRegisters(RegisterSet(), std::nullopt, suspensionSet, suspensionSet);
#endif
}

#if CPU(ARM64)
enum class SlotsAboveFrameRecordCase : uint8_t { Placed, FieldOff, HasCallArgument, TooLarge, ReadsIncomingArgument, HasFixedSlot };

static void checkNoSlotOverlaps(Code& code, StackSlot* fixedSlot)
{
    intptr_t begin = fixedSlot->offsetFromFP();
    intptr_t end = begin + static_cast<intptr_t>(fixedSlot->byteSize());
    for (StackSlot* slot : code.stackSlots()) {
        if (slot != fixedSlot)
            CHECK(!WTF::rangesOverlap(begin, end, slot->offsetFromFP(), slot->offsetFromFP() + static_cast<intptr_t>(slot->byteSize())));
    }
}

static void checkSlotsAboveFrameRecord(SlotsAboveFrameRecordCase testCase)
{
    using Case = SlotsAboveFrameRecordCase;
    constexpr intptr_t fixedOffset = -104;
    int64_t memory[80];
    for (unsigned i = 0; i < std::size(memory); ++i)
        memory[i] = (i + 1) * 1000003ll;

    B3::Policy policy = policyWithAirFrame();
    policy.air.frame.mayPlaceSlotsAboveFrameRecord = testCase != Case::FieldOff;
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    int64_t expectedSum = buildSumAcrossCalls(proc, testCase == Case::TooLarge ? 80 : 20, 1, memory);
    if (testCase == Case::HasCallArgument)
        code[0]->insts().insert(0, Inst(Move, nullptr, argumentTmp(1), Arg::callArg(0)));
    if (testCase == Case::ReadsIncomingArgument)
        code[0]->insts().insert(0, Inst(Move, nullptr, Arg::addr(Tmp(GPRInfo::callFrameRegister), static_cast<int32_t>(sizeof(CallerFrameAndPC))), argumentTmp(1)));
    StackSlot* fixedSlot = nullptr;
    if (testCase == Case::HasFixedSlot) {
        fixedSlot = code.addStackSlot(8, StackSlotKind::Locked);
        fixedSlot->setOffsetFromFP(fixedOffset);
    }

    auto compilation = compile(proc);
    CHECK(invoke<int64_t>(*compilation, memory) == expectedSum);

    const FrameLayout& layout = code.frameLayout();
    CHECK(layout.isPlanned);
    if (testCase != Case::Placed) {
        CHECK(layout.slotArea == SlotArea::BelowFrameRecord);
        CHECK(layout.calleeSaveForm == CalleeSaveForm::Stored);
        CHECK(!layout.bytesAboveFrameRecord);
        if (fixedSlot) {
            CHECK(fixedSlot->offsetFromFP() == fixedOffset);
            checkNoSlotOverlaps(code, fixedSlot);
        }
        return;
    }
    CHECK(layout.slotArea == SlotArea::AboveFrameRecord);
    CHECK(layout.calleeSaveForm == CalleeSaveForm::PushedBelowFrameRecord);
    CHECK(layout.bytesAboveFrameRecord);
    CHECK(!(layout.bytesAboveFrameRecord % 16));
    CHECK(code.frameSize() == layout.bytesAboveFrameRecord + WTF::roundUpToMultipleOf<16>(code.calleeSaveStackSlot()->byteSize()));
    for (StackSlot* slot : code.stackSlots()) {
        if (slot != code.calleeSaveStackSlot())
            CHECK(slot->offsetFromFP() >= static_cast<intptr_t>(sizeof(CallerFrameAndPC)));
    }
}

void testSlotsAboveFrameRecord()
{
    using Case = SlotsAboveFrameRecordCase;
    for (Case testCase : { Case::Placed, Case::FieldOff, Case::HasCallArgument, Case::TooLarge, Case::ReadsIncomingArgument, Case::HasFixedSlot })
        checkSlotsAboveFrameRecord(testCase);
}
#endif

#if CPU(X86_64)
struct ManySlotFrame {
    static constexpr unsigned numberOfSlots = 40;
    static constexpr unsigned hotSlotExtraUseCount = 10;
    static constexpr unsigned coldLockedSlotSize = 128;
    static constexpr intptr_t minOneByteDisplacement = std::numeric_limits<int8_t>::min();
    static constexpr intptr_t maxOneByteDisplacement = std::numeric_limits<int8_t>::max();

    ManySlotFrame(const B3::Policy& policy, unsigned firstHotSlot, unsigned numberOfHotSlots, StackSlotKind hotSlotKind = StackSlotKind::Spill, bool usesCalleeSave = false)
        : proc(policy)
        , firstHotSlot(firstHotSlot)
        , numberOfHotSlots(numberOfHotSlots)
    {
        constexpr int64_t base = 5;
        proc.setOptLevel(2);
        Code& code = proc.code();
        root = code.addBlock();
        Tmp sum = Tmp(GPRInfo::returnValueGPR);
        Tmp scratch = Tmp(GPRInfo::regT1);
        Tmp saved = Tmp(GPRInfo::regCS0);
        if (usesCalleeSave)
            root->append(Move, nullptr, Tmp(GPRInfo::argumentGPR0), saved);
        if (hotSlotKind == StackSlotKind::Locked)
            root->append(Move, nullptr, Tmp(GPRInfo::argumentGPR0), Arg::stack(code.addStackSlot(coldLockedSlotSize, StackSlotKind::Locked)));
        for (unsigned i = 0; i < numberOfSlots; ++i) {
            bool isHot = i >= firstHotSlot && i < firstHotSlot + numberOfHotSlots;
            slots.append(code.addStackSlot(8, isHot ? hotSlotKind : StackSlotKind::Spill));
            root->append(Move, nullptr, Tmp(GPRInfo::argumentGPR0), scratch);
            root->append(Add64, nullptr, Arg::imm(i + 1), scratch);
            root->append(Move, nullptr, scratch, Arg::stack(slots.last()));
        }
        root->append(Move, nullptr, Arg::imm(0), sum);
        for (StackSlot* slot : slots)
            root->append(Add64, nullptr, Arg::stack(slot), sum);
        int64_t expectedSum = numberOfSlots * (numberOfSlots + 1) / 2 + numberOfSlots * base;
        for (unsigned hotSlot = firstHotSlot; hotSlot < firstHotSlot + numberOfHotSlots; ++hotSlot) {
            for (unsigned i = 0; i < hotSlotExtraUseCount; ++i)
                root->append(Add64, nullptr, Arg::stack(slots[hotSlot]), sum);
            expectedSum += hotSlotExtraUseCount * (hotSlot + 1 + base);
        }
        if (usesCalleeSave) {
            root->append(Add64, nullptr, saved, sum);
            expectedSum += base;
        }
        root->append(Ret64, nullptr, sum);
        auto compilation = compile(proc);
        CHECK(invoke<int64_t>(*compilation, base) == expectedSum);
    }

    unsigned stackPointerAddressCount()
    {
        unsigned result = 0;
        for (Inst& inst : *root) {
            for (Arg& arg : inst.args())
                result += arg.isAddr() && arg.base() == Tmp(MacroAssembler::stackPointerRegister);
        }
        return result;
    }

    void checkStackPointerAddressesNameSlots()
    {
        intptr_t frameSize = proc.code().frameSize();
        for (Inst& inst : *root) {
            for (Arg& arg : inst.args()) {
                if (!arg.isAddr() || arg.base() != Tmp(MacroAssembler::stackPointerRegister))
                    continue;
                CHECK(arg.offset() >= 0);
                CHECK(arg.offset() <= maxOneByteDisplacement);
                CHECK(slots.containsIf([&](StackSlot* slot) {
                    return slot->offsetFromFP() + frameSize == arg.offset();
                }));
            }
        }
    }

    bool hotSlotsAreWithinOneByteDisplacement()
    {
        intptr_t frameSize = proc.code().frameSize();
        for (unsigned hotSlot = firstHotSlot; hotSlot < firstHotSlot + numberOfHotSlots; ++hotSlot) {
            intptr_t offsetFromFP = slots[hotSlot]->offsetFromFP();
            if (offsetFromFP < minOneByteDisplacement && offsetFromFP + frameSize > maxOneByteDisplacement)
                return false;
        }
        return true;
    }

    B3::Procedure proc;
    unsigned firstHotSlot;
    unsigned numberOfHotSlots;
    BasicBlock* root;
    Vector<StackSlot*> slots;
};

void testHotStackSlotIsNearFramePointer()
{
    constexpr unsigned hotSlot = ManySlotFrame::numberOfSlots - 10;
    auto isNear = [](bool ordersSlotsByUse, bool enablesAllocatorFields) {
        B3::Policy policy = policyWithAirFrame();
        policy.air.frame.ordersSlotsByUse = ordersSlotsByUse;
        if (enablesAllocatorFields)
            enableAllAllocatorFields(policy.air);
        return ManySlotFrame(policy, hotSlot, 1).slots[hotSlot]->offsetFromFP() >= ManySlotFrame::minOneByteDisplacement;
    };
    CHECK(isNear(true, false));
    CHECK(!isNear(false, false));
    CHECK(!isNear(false, true));
}

void testFarStackSlotsUseStackPointer()
{
    constexpr unsigned hotSlot = ManySlotFrame::numberOfSlots - 10;
    B3::Policy policy = policyWithAirFrame();
    CHECK(!ManySlotFrame(policy, hotSlot, 1).stackPointerAddressCount());
    policy.air.frame.prefersStackPointerAddresses = true;
    CHECK(ManySlotFrame(policy, hotSlot, 1).stackPointerAddressCount());

    // fixObviousSpills() reads the last slot's value from the register that still holds it, so that slot never reaches memory.
    constexpr unsigned numberOfSlotsInMemory = ManySlotFrame::numberOfSlots - 1;
    constexpr unsigned wordsNearFramePointer = 128 / sizeof(CPURegister);
    constexpr unsigned wordsNearStackPointerAboveOneSave = wordsNearFramePointer - 1;
    constexpr unsigned numberOfHotSlots = wordsNearFramePointer + wordsNearStackPointerAboveOneSave;
    static_assert(numberOfHotSlots < numberOfSlotsInMemory);
    policy.air.frame.mayPushCalleeSaves = true;
    for (bool ordersSlotsByUse : { false, true }) {
        policy.air.frame.ordersSlotsByUse = ordersSlotsByUse;
        ManySlotFrame frame(policy, numberOfSlotsInMemory - numberOfHotSlots, numberOfHotSlots, StackSlotKind::Spill, true);
        CHECK(frame.proc.code().frameLayout().calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom);
        CHECK(frame.stackPointerAddressCount());
        frame.checkStackPointerAddressesNameSlots();
        CHECK(frame.hotSlotsAreWithinOneByteDisplacement() == ordersSlotsByUse);
    }
}

void testLockedSlotsAreOrderedWithSpills()
{
    constexpr unsigned hotSlot = ManySlotFrame::numberOfSlots - 10;
    for (bool ordersSlotsByUse : { false, true }) {
        B3::Policy policy = policyWithAirFrame();
        policy.air.frame.ordersSlotsByUse = ordersSlotsByUse;
        ManySlotFrame frame(policy, hotSlot, 1, StackSlotKind::Locked);
        CHECK((frame.slots[hotSlot]->offsetFromFP() >= ManySlotFrame::minOneByteDisplacement) == ordersSlotsByUse);
    }
}

enum class CalleeSavesAtFrameBottomCase : uint8_t { Pushed, FieldOff, HasCallArgument, LargeFrame };

static void checkCalleeSavesAtFrameBottom(CalleeSavesAtFrameBottomCase testCase)
{
    using Case = CalleeSavesAtFrameBottomCase;
    constexpr unsigned numberOfExtraSlots = 70;
    static_assert(numberOfExtraSlots * sizeof(CPURegister) > B3::AirLimits::maxFrameSizeWithPushedRegisters);
    B3::Policy policy = policyWithAirFrame();
    policy.air.frame.mayPushCalleeSaves = testCase != Case::FieldOff;
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    StackSlot* first = code.addStackSlot(8, StackSlotKind::Spill);
    StackSlot* second = code.addStackSlot(8, StackSlotKind::Spill);
    Tmp saved(GPRInfo::regCS0);

    root->append(Move, nullptr, argumentTmp(0), saved);
    root->append(Move, nullptr, argumentTmp(1), Arg::stack(first));
    root->append(Move, nullptr, argumentTmp(2), Arg::stack(second));
    Vector<StackSlot*> extraSlots;
    if (testCase == Case::LargeFrame) {
        for (unsigned i = 0; i < numberOfExtraSlots; ++i) {
            extraSlots.append(code.addStackSlot(8, StackSlotKind::Spill));
            root->append(Move, nullptr, argumentTmp(0), Arg::stack(extraSlots.last()));
        }
    }
    if (testCase == Case::HasCallArgument)
        root->append(Move, nullptr, argumentTmp(0), Arg::callArg(0));
    appendCall(proc, root);
    for (StackSlot* slot : extraSlots)
        root->append(Add64, nullptr, Arg::stack(slot), argumentTmp(3));
    root->append(Move, nullptr, saved, returnTmp());
    root->append(Add64, nullptr, Arg::stack(first), returnTmp());
    root->append(Add64, nullptr, Arg::stack(second), returnTmp());
    appendReturn(root);

    auto compilation = compile(proc);
    CHECK(invoke<int64_t>(*compilation, 1ll, 2ll, 3ll) == 6);
    StackSlot* saveSlot = code.calleeSaveStackSlot();
    if (testCase != Case::Pushed) {
        CHECK(code.frameLayout().calleeSaveForm == CalleeSaveForm::PushedBelowFrameRecord);
        CHECK(saveSlot->offsetFromFP() == -static_cast<intptr_t>(saveSlot->byteSize()));
        return;
    }
    CHECK(code.frameLayout().calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom);
    CHECK(saveSlot->offsetFromFP() == -static_cast<intptr_t>(code.frameSize()));
}

void testCalleeSavesPushedAtFrameBottom()
{
    using Case = CalleeSavesAtFrameBottomCase;
    for (Case testCase : { Case::Pushed, Case::FieldOff, Case::HasCallArgument, Case::LargeFrame })
        checkCalleeSavesAtFrameBottom(testCase);
}

static Vector<Reg> registersPushedAtFrameSetup(Code& code)
{
    Vector<Reg> result;
    size_t numberOfSaves = code.calleeSaveRegisterAtOffsetList().registerCount();
    for (BasicBlock* block : code) {
        for (Inst& inst : *block) {
            if (inst.kind.opcode != EnterFrame)
                continue;
            for (Arg& arg : inst.args().subspan(1 + EnterFrameCustom::numberOfScratchRegisters(inst) + numberOfSaves))
                result.append(arg.reg());
        }
    }
    return result;
}

static void checkEntryBlockHasNoSpills(BasicBlock* block)
{
    bool isBeforePatch = true;
    for (Inst& inst : *block) {
        CHECK(inst);
        CHECK(inst.kind.opcode != Nop);
        if (inst.kind.opcode == Patch)
            isBeforePatch = false;
        if (!isBeforePatch)
            continue;
        for (Arg& arg : inst.args())
            CHECK(!arg.isMemory());
    }
}

enum class SpillsAtFrameSetupCase : uint8_t { Pushed, PushedWithCalleeSave, CopiedFromArgument, Overwritten, AfterPatch, FieldOff };

static void checkSpillsAtFrameSetup(SpillsAtFrameSetupCase testCase)
{
    using Case = SpillsAtFrameSetupCase;
    B3::Policy policy = policyWithAirFrame();
    policy.air.frame.mayPushCalleeSaves = true;
    policy.air.frame.mayPushSpillsAtFrameSetup = testCase != Case::FieldOff;
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    StackSlot* slots[3];
    for (StackSlot*& slot : slots)
        slot = code.addStackSlot(8, StackSlotKind::Spill);
    Tmp saved(GPRInfo::regCS0);
    Tmp firstSource = argumentTmp(0);

    if (testCase == Case::PushedWithCalleeSave)
        root->append(Move, nullptr, argumentTmp(3), saved);
    if (testCase == Case::Overwritten)
        root->append(Add64, nullptr, Arg::imm(1), argumentTmp(0));
    if (testCase == Case::AfterPatch)
        appendCall(proc, root);
    if (testCase == Case::CopiedFromArgument) {
        firstSource = returnTmp();
        root->append(Move, nullptr, argumentTmp(0), firstSource);
    }
    root->append(Move, nullptr, firstSource, Arg::stack(slots[0]));
    for (unsigned i = 1; i < 3; ++i)
        root->append(Move, nullptr, argumentTmp(i), Arg::stack(slots[i]));
    appendCall(proc, root);
    root->append(Move, nullptr, Arg::stack(slots[0]), returnTmp());
    root->append(Add64, nullptr, Arg::stack(slots[1]), returnTmp());
    root->append(Add64, nullptr, Arg::stack(slots[2]), returnTmp());
    if (testCase == Case::PushedWithCalleeSave)
        root->append(Add64, nullptr, saved, returnTmp());
    appendReturn(root);

    auto compilation = compile(proc);
    int64_t result = invoke<int64_t>(*compilation, 1ll, 2ll, 4ll, 8ll);
    if (testCase != Case::AfterPatch)
        CHECK(result == 7 + (testCase == Case::Overwritten ? 1 : 0) + (testCase == Case::PushedWithCalleeSave ? 8 : 0));

    Vector<Reg> pushed = registersPushedAtFrameSetup(code);
    CHECK(code.frameLayout().numberOfSpillsPushedAtFrameSetup == pushed.size());
    if (testCase != Case::Pushed && testCase != Case::PushedWithCalleeSave && testCase != Case::CopiedFromArgument) {
        CHECK(pushed.isEmpty());
        return;
    }
    CHECK(pushed.size() == 3);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(pushed[i] == argumentTmp(i).reg());
        CHECK(slots[i]->offsetFromFP() == -static_cast<intptr_t>(sizeof(CPURegister) * (i + 1)));
    }
    checkEntryBlockHasNoSpills(code[0]);
}

static void checkNoSpillIsPushedAtTwoFrameSetups()
{
    B3::Policy policy = policyWithAirFrame();
    policy.air.frame.shrinkWraps = true;
    policy.air.frame.mayPushSpillsAtFrameSetup = true;
    B3::Procedure proc(policy);
    buildTwoRareCalls(proc);
    auto compilation = compile(proc);
    checkTwoRareCalls(*compilation);
    CHECK(proc.code().frameLayout().numberOfFrameSetups == 2);
    CHECK(!proc.code().frameLayout().numberOfSpillsPushedAtFrameSetup);
    CHECK(registersPushedAtFrameSetup(proc.code()).isEmpty());
}

void testSpillsPushedAtFrameSetup()
{
    using Case = SpillsAtFrameSetupCase;
    for (Case testCase : { Case::Pushed, Case::PushedWithCalleeSave, Case::CopiedFromArgument, Case::Overwritten, Case::AfterPatch, Case::FieldOff })
        checkSpillsAtFrameSetup(testCase);
    checkNoSpillIsPushedAtTwoFrameSetups();
}

void testPushedOperandSurvivesCopyForwarding()
{
    B3::Policy policy = policyWithAirFrame();
    policy.air.frame.shrinkWraps = true;
    policy.air.frame.mayPushSpillsAtFrameSetup = true;
    policy.air.peepholes.forwardsRegisterCopies = true;
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* withCall = code.addBlock();
    BasicBlock* withoutCall = code.addBlock();
    StackSlot* slot = code.addStackSlot(8, StackSlotKind::Spill);

    root->append(Move, nullptr, argumentTmp(0), argumentTmp(1));
    appendBranchTest(root, argumentTmp(2), withCall, withoutCall);
    withCall->append(Move, nullptr, argumentTmp(1), Arg::stack(slot));
    appendCall(proc, withCall);
    withCall->append(Move, nullptr, Arg::stack(slot), returnTmp());
    appendReturn(withCall);
    appendReturn(withoutCall, 0);

    auto compilation = compile(proc);
    CHECK(code.frameLayout().coverage == FrameCoverage::SomePaths);
    CHECK(code.frameLayout().numberOfSpillsPushedAtFrameSetup == 1);
    CHECK(registersPushedAtFrameSetup(code).size() == 1);
    CHECK(invoke<int64_t>(*compilation, 7ll, 100ll, 1ll) == 7);
    CHECK(invoke<int64_t>(*compilation, 7ll, 100ll, 0ll) == 0);
}
#endif

void testSixteenByteSlotsStayAligned()
{
    if (!Options::useWasmSIMD())
        return;
    constexpr unsigned numberOfSpillSlots = 20;
    static_assert(numberOfSpillSlots >= B3::AirLimits::minSlotsToOrderByUse);
    for (FrameLayoutCase layoutCase : allFrameLayoutCases) {
        for (unsigned numberOfCalleeSaves = 0; numberOfCalleeSaves <= FunctionWithFrame::maxNumberOfCalleeSaves; ++numberOfCalleeSaves) {
            B3::Policy policy = policyWithFrameLayout(layoutCase);
            policy.upstream.allowsSIMD = true;
            B3::Procedure proc(policy, nullptr, true);
            FunctionWithFrame function(proc, numberOfCalleeSaves, numberOfSpillSlots, true);
            auto compilation = compile(proc);
            checkFrameLayout(proc.code(), layoutCase, numberOfCalleeSaves);
            CHECK(!(function.vectorSlot->offsetFromFP() % 16));
            function.checkResult(*compilation);
        }
    }
}

static constexpr GPRReg stackCheckScratch = GPRInfo::nonArgGPR0;

static B3::Policy policyWithStackCheckScratch(FrameLayoutCase layoutCase)
{
    B3::Policy policy = policyWithFrameLayout(layoutCase);
    policy.air.frame.registersClobberedByFrameSetup.add(stackCheckScratch, IgnoreVectors);
    return policy;
}

struct RecordedStackChecks {
    Vector<StackCheckSite> sites;
    std::unique_ptr<Compilation> compilation;
};

static RecordedStackChecks compileAndRecordStackChecks(B3::Procedure& proc)
{
    RecordedStackChecks result;
    proc.code().setStackCheckGenerator(createSharedTask<StackCheckGeneratorFunction>([&](CCallHelpers& jit, GenerationContext&, const StackCheckSite& site) {
        result.sites.append(site);
        jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(site.bytesAllocatedBelowFrameRecord), stackCheckScratch);
        CCallHelpers::Jump isAllocated = jit.branchPtr(CCallHelpers::Equal, stackCheckScratch, CCallHelpers::stackPointerRegister);
        jit.breakpoint();
        isAllocated.link(&jit);
    }));
    result.compilation = compile(proc);
    proc.code().setStackCheckGenerator(nullptr);
    CHECK(result.sites.size() == numberOfInsts(proc.code(), EnterFrame));
    return result;
}

static void checkStackCheckSite(const StackCheckSite& site, unsigned bytesAllocatedBelowFrameRecord, unsigned bytesBelowFrameRecord)
{
    CHECK(site.bytesAllocatedBelowFrameRecord == bytesAllocatedBelowFrameRecord);
    CHECK(site.bytesBelowFrameRecord == bytesBelowFrameRecord);
}

static void checkStackCheckSiteOfLayout(FrameLayoutCase layoutCase)
{
    B3::Procedure proc(policyWithStackCheckScratch(layoutCase));
    FunctionWithFrame function(proc, 1, FunctionWithFrame::numberOfSpilledArguments);
    RecordedStackChecks recorded = compileAndRecordStackChecks(proc);
    Code& code = proc.code();
    checkFrameLayout(code, layoutCase, true);
    unsigned bytesBelowFrameRecord = code.frameSize() - code.frameLayout().bytesAboveFrameRecord;
    bool checksBeforeAllocating = code.frameLayout().slotArea == SlotArea::AboveFrameRecord;
    CHECK(recorded.sites.size() == 1);
    checkStackCheckSite(recorded.sites[0], checksBeforeAllocating ? 0 : bytesBelowFrameRecord, bytesBelowFrameRecord);
    function.checkResult(*recorded.compilation);
}

static void checkStackCheckSiteOfLargeFrame()
{
    B3::Policy policy = policyWithStackCheckScratch(FrameLayoutCase::Default);
    policy.air.frame.maxBytesAllocatedBeforeStackCheck = 0;
    B3::Procedure proc(policy);
    FunctionWithFrame function(proc, 1, FunctionWithFrame::numberOfSpilledArguments);
    RecordedStackChecks recorded = compileAndRecordStackChecks(proc);
    Code& code = proc.code();
    checkFrameLayout(code, FrameLayoutCase::Default, true);
    unsigned bytesPushed = code.frameLayout().calleeSaveForm == CalleeSaveForm::PushedBelowFrameRecord ? code.calleeSaveStackSlot()->byteSize() : 0;
    CHECK(bytesPushed < code.frameSize());
    CHECK(recorded.sites.size() == 1);
    checkStackCheckSite(recorded.sites[0], bytesPushed, code.frameSize());
    function.checkResult(*recorded.compilation);
}

static void checkStackCheckSitePerFrameSetup()
{
    B3::Policy policy = policyWithStackCheckScratch(FrameLayoutCase::Default);
    policy.air.frame.shrinkWraps = true;
    B3::Procedure proc(policy);
    buildTwoRareCalls(proc);
    RecordedStackChecks recorded = compileAndRecordStackChecks(proc);
    CHECK(recorded.sites.size() == 2);
    for (const StackCheckSite& site : recorded.sites)
        checkStackCheckSite(site, proc.code().frameSize(), proc.code().frameSize());
    checkTwoRareCalls(*recorded.compilation);
}

void testStackCheckSites()
{
    for (FrameLayoutCase layoutCase : allFrameLayoutCases)
        checkStackCheckSiteOfLayout(layoutCase);
    checkStackCheckSiteOfLargeFrame();
    checkStackCheckSitePerFrameSetup();
}

void testTeardownSizeBound()
{
    for (FrameLayoutCase layoutCase : allFrameLayoutCases) {
        for (unsigned numberOfCalleeSaves = 0; numberOfCalleeSaves <= FunctionWithFrame::maxNumberOfCalleeSaves; ++numberOfCalleeSaves) {
            B3::Procedure proc(policyWithFrameLayout(layoutCase));
            FunctionWithFrame function(proc, numberOfCalleeSaves, FunctionWithFrame::numberOfSpilledArguments);
            Code& code = proc.code();
            prepareForGeneration(code);
            ObservedCode observed = generateObservedCode(proc);
            checkFrameLayout(code, layoutCase, numberOfCalleeSaves);

            unsigned numberOfTeardowns = 0;
            for (const InstRange& range : observed.ranges) {
                if (range.opcode != LeaveFrame)
                    continue;
                ++numberOfTeardowns;
                CHECK(range.end - range.begin <= static_cast<ptrdiff_t>(maxBytesOfFrameTeardown(code)));
            }
            CHECK(numberOfTeardowns == 1);
            function.checkResult(*observed.generated.compilation);
        }
    }

    B3::Policy policy = policyWithAirFrame();
    policy.air.frame.shrinkWraps = true;
    B3::Procedure leaf(policy);
    leaf.setOptLevel(2);
    appendReturn(leaf.code().addBlock(), 1);
    prepareForGeneration(leaf.code());
    CHECK(!leaf.code().frameLayout().hasFrame);
    CHECK(!maxBytesOfFrameTeardown(leaf.code()));
}

static StackSlot* addRandomStackSlot(Code& code, WeakRandom& random)
{
    constexpr unsigned spillSizes[] = { 4, 8, 16 };
    constexpr unsigned lockedSizes[] = { 1, 3, 5, 7, 9, 13, 17, 33 };
    if (random.getUint32(8))
        return code.addStackSlot(spillSizes[random.getUint32(std::size(spillSizes))], StackSlotKind::Spill);
    return code.addStackSlot(lockedSizes[random.getUint32(std::size(lockedSizes))], StackSlotKind::Locked);
}

static void appendSlotStore(BasicBlock* block, StackSlot* slot)
{
    B3::Air::Opcode opcode = slot->isLocked() ? Store8 : slot->byteSize() == 4 ? Move32 : Move;
    block->append(opcode, nullptr, argumentTmp(0), Arg::stack(slot));
}

static void appendSlotLoad(BasicBlock* block, StackSlot* slot)
{
    B3::Air::Opcode opcode = slot->isLocked() ? Load8 : slot->byteSize() == 4 ? Move32 : Move;
    block->append(opcode, nullptr, Arg::stack(slot), argumentTmp(1));
}

static void appendSparseSlotAccesses(BasicBlock* block, const Vector<StackSlot*>& slots, WeakRandom& random)
{
    unsigned numberOfAccesses = slots.size() * (1 + random.getUint32(6));
    for (unsigned i = 0; i < numberOfAccesses; ++i) {
        StackSlot* slot = slots[random.getUint32(slots.size())];
        if (random.getUint32(2))
            appendSlotStore(block, slot);
        else
            appendSlotLoad(block, slot);
    }
}

static void appendDenseSlotAccesses(BasicBlock* block, const Vector<StackSlot*>& slots)
{
    size_t numberOfShortLivedSlots = slots.size() / 40;
    size_t firstLateSlot = slots.size() - numberOfShortLivedSlots;
    for (size_t i = 0; i < firstLateSlot; ++i)
        appendSlotStore(block, slots[i]);
    for (size_t i = 0; i < numberOfShortLivedSlots; ++i)
        appendSlotLoad(block, slots[i]);
    for (size_t i = firstLateSlot; i < slots.size(); ++i)
        appendSlotStore(block, slots[i]);
    for (size_t i = numberOfShortLivedSlots; i < slots.size(); ++i)
        appendSlotLoad(block, slots[i]);
}

static Vector<BitVector> computeSpillSlotInterference(Code& code, BasicBlock* block)
{
    Vector<BitVector> interference(code.stackSlots().size());
    BitVector liveSlots;
    for (unsigned instIndex = block->size(); instIndex--;) {
        block->at(instIndex).forEachArg([&](Arg& arg, Arg::Role role, B3::Bank, Width) {
            if (!arg.isStack() || !arg.stackSlot()->isSpill())
                return;
            unsigned slotIndex = arg.stackSlot()->index();
            if (!Arg::isAnyDef(role)) {
                liveSlots.set(slotIndex);
                return;
            }
            if (!liveSlots.get(slotIndex))
                return;
            liveSlots.clear(slotIndex);
            for (size_t otherIndex : liveSlots) {
                interference[slotIndex].set(otherIndex);
                interference[otherIndex].set(slotIndex);
            }
        });
    }
    return interference;
}

static void checkSpillSlotOffsetsAreUpstream(Code& code, const Vector<BitVector>& interference)
{
    Vector<intptr_t> allocatedOffsets(code.stackSlots().size());
    Vector<StackSlot*> lockedSlots;
    for (StackSlot* slot : code.stackSlots()) {
        CHECK(slot->offsetFromFP() < 0);
        allocatedOffsets[slot->index()] = slot->offsetFromFP();
        if (slot->isLocked())
            lockedSlots.append(slot);
        else
            slot->setOffsetFromFP(0);
    }

    for (StackSlot* slot : code.stackSlots()) {
        if (slot->isLocked())
            continue;
        Vector<StackSlot*> otherSlots = lockedSlots;
        for (size_t otherIndex : interference[slot->index()]) {
            StackSlot* otherSlot = code.stackSlots()[otherIndex];
            if (otherSlot->offsetFromFP())
                otherSlots.append(otherSlot);
        }
        std::ranges::sort(otherSlots, std::ranges::greater { }, &StackSlot::offsetFromFP);
        assign(slot, otherSlots);
        CHECK(slot->offsetFromFP() == allocatedOffsets[slot->index()]);
    }
}

void testDefaultStackOffsetsAreUpstream()
{
    constexpr unsigned numberOfGraphs = 200;
    constexpr unsigned graphsPerDenseGraph = 20;
    constexpr unsigned minSlotsOfDenseGraph = 300;
    static_assert(minSlotsOfDenseGraph > B3::AirLimits::minAssignedSlotsToWalkComplement);
    for (unsigned seed = 1; seed <= numberOfGraphs; ++seed) {
        WeakRandom random(seed);
        bool isDense = !(seed % graphsPerDenseGraph);
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        Vector<StackSlot*> slots;
        unsigned numberOfSlots = isDense ? minSlotsOfDenseGraph + random.getUint32(300) : 2 + random.getUint32(60);
        for (unsigned i = 0; i < numberOfSlots; ++i)
            slots.append(addRandomStackSlot(code, random));
        if (random.getUint32(2))
            root->append(Move, nullptr, argumentTmp(0), Tmp(GPRInfo::regCS0));
        if (isDense)
            appendDenseSlotAccesses(root, slots);
        else
            appendSparseSlotAccesses(root, slots, random);
        appendReturn(root);

        Vector<BitVector> interference = computeSpillSlotInterference(code, root);
        StackColoring coloring = allocateStackByGraphColoring(code);
        CHECK(coloring.unitsOfSlot.isEmpty());
        CHECK(code.stackIsAllocated());
        checkSpillSlotOffsetsAreUpstream(code, interference);
    }
}

#if CPU(ARM64)
#define RUN_BUN_FRAME_TESTS_OF_CPU() do { \
        RUN(testSlotsAboveFrameRecord()); \
    } while (false)
#elif CPU(X86_64)
#define RUN_BUN_FRAME_TESTS_OF_CPU() do { \
        RUN(testHotStackSlotIsNearFramePointer()); \
        RUN(testFarStackSlotsUseStackPointer()); \
        RUN(testCalleeSavesPushedAtFrameBottom()); \
        RUN(testSpillsPushedAtFrameSetup()); \
        RUN(testPushedOperandSurvivesCopyForwarding()); \
        RUN(testLockedSlotsAreOrderedWithSpills()); \
    } while (false)
#else
#define RUN_BUN_FRAME_TESTS_OF_CPU() do { } while (false)
#endif

#define RUN_BUN_FRAME_TESTS() do { \
        RUN(testCalleeSavesAreFoundAfterSpillFixing()); \
        RUN(testSuspensionSuffix()); \
        RUN_BUN_FRAME_TESTS_OF_CPU(); \
        RUN(testSixteenByteSlotsStayAligned()); \
        RUN(testStackCheckSites()); \
        RUN(testTeardownSizeBound()); \
        RUN(testDefaultStackOffsetsAreUpstream()); \
    } while (false)
