/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirFrameEmission.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirCustom.h"
#include "AirGenerationContext.h"
#include "AirInstInlines.h"
#include "AirLimits.h"
#include "AirOpcodeUtils.h"
#include "AllowMacroScratchRegisterUsageIf.h"
#include "AssemblyHelpersSpoolers.h"
#include "CCallHelpers.h"
#include <array>

namespace JSC { namespace B3 { namespace Air {

static void emitStackCheck(CCallHelpers& jit, GenerationContext& context, unsigned bytesAllocated, unsigned bytesBelow)
{
    if (StackCheckGenerator* generator = context.code->stackCheckGenerator())
        generator->run(jit, context, StackCheckSite { bytesAllocated, bytesBelow, 0, 0 });
}

static bool mayUseMacroScratchRegisters(const Inst& enterFrame)
{
    RegisterSet scratchRegisters;
    for (const Arg& arg : enterFrame.args().subspan(1, EnterFrameCustom::numberOfScratchRegisters(enterFrame)))
        scratchRegisters.add(arg.reg(), IgnoreVectors);
    return scratchRegisters.subsumes(RegisterSet::macroClobberedGPRs());
}

#if CPU(ARM64)
static unsigned preIndexedSaveCount(const RegisterAtOffsetList& list)
{
    return list.registerCount() > 1 && list.at(0).reg().isGPR() == list.at(1).reg().isGPR() ? 2 : 1;
}

static void pushCalleeSaves(CCallHelpers& jit, const RegisterAtOffsetList& list)
{
    size_t count = list.registerCount();
    if (!count)
        return;
    constexpr auto sp = CCallHelpers::stackPointerRegister;
    int size = -static_cast<int>(list.at(0).offset());
    RELEASE_ASSERT(size > 0 && !(size % stackAlignmentBytes()));
    Reg first = list.at(0).reg();
    size_t i = preIndexedSaveCount(list);
    if (i == 2 && first.isGPR())
        jit.m_assembler.stp<64>(first.gpr(), list.at(1).reg().gpr(), sp, PairPreIndex(-size));
    else if (i == 2)
        jit.m_assembler.stp<64>(first.fpr(), list.at(1).reg().fpr(), sp, PairPreIndex(-size));
    else if (first.isGPR())
        jit.m_assembler.str<64>(first.gpr(), sp, PreIndex(-size));
    else
        jit.m_assembler.str<64>(first.fpr(), sp, PreIndex(-size));
    AssemblyHelpers::StoreRegSpooler spooler(jit, GPRInfo::callFrameRegister);
    for (; i < count && list.at(i).reg().isGPR(); ++i)
        spooler.storeGPR(list.at(i));
    spooler.finalizeGPR();
    for (; i < count; ++i)
        spooler.storeFPR(list.at(i));
    spooler.finalizeFPR();
}

static void popCalleeSaves(CCallHelpers& jit, const RegisterAtOffsetList& list)
{
    size_t count = list.registerCount();
    if (!count)
        return;
    constexpr auto sp = CCallHelpers::stackPointerRegister;
    int size = -static_cast<int>(list.at(0).offset());
    Reg first = list.at(0).reg();
    size_t preIndexedCount = preIndexedSaveCount(list);
    size_t i = preIndexedCount;
    AssemblyHelpers::LoadRegSpooler spooler(jit, GPRInfo::callFrameRegister);
    for (; i < count && list.at(i).reg().isGPR(); ++i)
        spooler.loadGPR(list.at(i));
    spooler.finalizeGPR();
    for (; i < count; ++i)
        spooler.loadFPR(list.at(i));
    spooler.finalizeFPR();
    if (preIndexedCount == 2 && first.isGPR())
        jit.m_assembler.ldp<64>(first.gpr(), list.at(1).reg().gpr(), sp, PairPostIndex(size));
    else if (preIndexedCount == 2)
        jit.m_assembler.ldp<64>(first.fpr(), list.at(1).reg().fpr(), sp, PairPostIndex(size));
    else if (first.isGPR())
        jit.m_assembler.ldr<64>(first.gpr(), sp, PostIndex(size));
    else
        jit.m_assembler.ldr<64>(first.fpr(), sp, PostIndex(size));
}
#endif

#if CPU(X86_64)
static void pushCalleeSaves(CCallHelpers& jit, const RegisterAtOffsetList& list)
{
    for (size_t i = list.registerCount(); i--;)
        jit.push(list.at(i).reg().gpr());
}

static void popCalleeSaves(CCallHelpers& jit, const RegisterAtOffsetList& list)
{
    for (size_t i = 0; i < list.registerCount(); ++i)
        jit.pop(list.at(i).reg().gpr());
}
#endif

#if CPU(X86_64)
static unsigned bytesBelowReturnAddress(const Code& code)
{
    return code.frameSize() + static_cast<unsigned>(sizeof(CPURegister));
}

static void allocateOnStack(CCallHelpers& jit, unsigned bytes)
{
    // This push only allocates the word, but a conservative collector reads what it stores for as long as the frame is there. Any other register may name a cell that
    // nothing else names: the result register holds what the caller's last call returned, which is often the argument. This one always holds an address of the stack.
    if (bytes == sizeof(CPURegister))
        jit.push(CCallHelpers::framePointerRegister);
    else if (bytes)
        jit.subPtr(CCallHelpers::TrustedImm32(bytes), CCallHelpers::stackPointerRegister);
}

static void releaseOnStack(CCallHelpers& jit, unsigned bytes, std::optional<GPRReg> deadRegister)
{
    if (bytes == sizeof(CPURegister) && deadRegister)
        jit.pop(*deadRegister);
    else if (bytes)
        jit.addPtr(CCallHelpers::TrustedImm32(bytes), CCallHelpers::stackPointerRegister);
}

static constexpr unsigned maxWordsWalked = AirLimits::maxFrameSizeWithPushedRegisters / sizeof(CPURegister) + 1;

// Operands come in pairs: a register and the word of the frame that it is pushed into or popped out of.
static std::array<GPRReg, maxWordsWalked> registersByWord(std::span<const Arg> operands, unsigned numberOfWords)
{
    RELEASE_ASSERT(!(operands.size() % 2) && numberOfWords <= maxWordsWalked);
    std::array<GPRReg, maxWordsWalked> result;
    result.fill(InvalidGPRReg);
    for (size_t i = 0; i < operands.size(); i += 2) {
        unsigned word = static_cast<unsigned>(operands[i + 1].value());
        RELEASE_ASSERT(word < numberOfWords && result[word] == InvalidGPRReg);
        result[word] = operands[i].gpr();
    }
    return result;
}

// From the top of the frame down: a push for each word that a register goes into, one move of the stack pointer for each stretch between them.
static void emitFrameSetupWithPushes(CCallHelpers& jit, GenerationContext& context, std::span<const Arg> pushedSpills)
{
    Code& code = *context.code;
    const FrameLayout& layout = code.frameLayout();
    RELEASE_ASSERT(layout.slotArea == SlotArea::BelowFrameRecord);
    RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
    const unsigned numberOfSaves = static_cast<unsigned>(saves.registerCount());
    const unsigned numberOfWords = numberOfWordsAllocatedAtFrameSetup(code);
    auto sources = registersByWord(pushedSpills, numberOfWords);

    if (layout.hasFrameRecord)
        jit.emitFunctionPrologue();
    unsigned word = 0;
    unsigned end = numberOfWords;
    if (layout.calleeSaveForm == CalleeSaveForm::PushedBelowFrameRecord) {
        pushCalleeSaves(jit, saves);
        word = numberOfSaves;
    } else if (layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom)
        end -= numberOfSaves;
    unsigned wordsToAllocate = 0;
    for (; word < end; ++word) {
        if (sources[word] == InvalidGPRReg) {
            ++wordsToAllocate;
            continue;
        }
        allocateOnStack(jit, wordsToAllocate * sizeof(CPURegister));
        wordsToAllocate = 0;
        jit.push(sources[word]);
    }
    allocateOnStack(jit, wordsToAllocate * sizeof(CPURegister));
    if (layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom)
        pushCalleeSaves(jit, saves);

    const unsigned frameSize = code.frameSize();
    if (StackCheckGenerator* generator = code.stackCheckGenerator())
        generator->run(jit, context, layout.hasFrameRecord ? StackCheckSite { frameSize, frameSize, 0, 0 } : StackCheckSite { 0, frameSize, bytesBelowReturnAddress(code), 0 });
    if (layout.calleeSaveForm == CalleeSaveForm::Stored) {
        for (const RegisterAtOffset& entry : saves) {
            if (entry.reg().isGPR())
                jit.storePtr(entry.reg().gpr(), code.addressInFrame(entry.offset()));
            else
                jit.storeDouble(entry.reg().fpr(), code.addressInFrame(entry.offset()));
        }
    }
}

// From the stack pointer up: a pop for each word that goes into a register.
static void emitFrameTeardownWithPops(CCallHelpers& jit, const Code& code, std::optional<GPRReg> deadRegister, std::span<const Arg> poppedReloads)
{
    const FrameLayout& layout = code.frameLayout();
    RELEASE_ASSERT(layout.slotArea == SlotArea::BelowFrameRecord);
    RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
    const unsigned numberOfSaves = static_cast<unsigned>(saves.registerCount());
    const unsigned numberOfWords = numberOfWordsAllocatedAtFrameSetup(code);
    auto destinations = registersByWord(poppedReloads, numberOfWords);

    unsigned word = 0;
    unsigned end = numberOfWords;
    switch (layout.calleeSaveForm) {
    case CalleeSaveForm::PushedAtFrameBottom:
        popCalleeSaves(jit, saves);
        word = numberOfSaves;
        break;
    case CalleeSaveForm::PushedBelowFrameRecord:
        end -= numberOfSaves;
        break;
    case CalleeSaveForm::Stored:
        for (const RegisterAtOffset& entry : saves) {
            if (entry.reg().isGPR())
                jit.loadPtr(code.addressInFrame(entry.offset()), entry.reg().gpr());
            else
                jit.loadDouble(code.addressInFrame(entry.offset()), entry.reg().fpr());
        }
        break;
    }
    unsigned wordsToRelease = 0;
    for (; word < end; ++word) {
        if (destinations[word] == InvalidGPRReg) {
            ++wordsToRelease;
            continue;
        }
        releaseOnStack(jit, wordsToRelease * sizeof(CPURegister), deadRegister);
        wordsToRelease = 0;
        jit.pop(destinations[word]);
    }
    releaseOnStack(jit, wordsToRelease * sizeof(CPURegister), deadRegister);
    if (layout.calleeSaveForm == CalleeSaveForm::PushedBelowFrameRecord)
        popCalleeSaves(jit, saves);
    if (layout.hasFrameRecord)
        jit.emitFunctionEpilogueWithEmptyFrame();
}

static void emitFrameSetupWithoutRecord(CCallHelpers& jit, GenerationContext& context)
{
    Code& code = *context.code;
    const FrameLayout& layout = code.frameLayout();
    RELEASE_ASSERT(layout.slotArea == SlotArea::BelowFrameRecord);
    RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
    const unsigned frameSize = code.frameSize();
    const unsigned bytesToAllocate = bytesBelowReturnAddress(code);
    const unsigned bytesOfSaves = static_cast<unsigned>(saves.registerCount() * sizeof(CPURegister));
    auto checkStack = [&](unsigned bytesAllocated) {
        if (StackCheckGenerator* generator = code.stackCheckGenerator())
            generator->run(jit, context, StackCheckSite { 0, frameSize, bytesAllocated, bytesToAllocate - bytesAllocated });
    };

    if (layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom) {
        allocateOnStack(jit, bytesToAllocate - bytesOfSaves);
        pushCalleeSaves(jit, saves);
        checkStack(bytesToAllocate);
        return;
    }

    unsigned pushedBytes = 0;
    if (layout.calleeSaveForm == CalleeSaveForm::PushedBelowFrameRecord) {
        pushCalleeSaves(jit, saves);
        pushedBytes = bytesOfSaves;
    }
    bool checksBeforeAllocating = frameSize > code.policy().frame.maxBytesAllocatedBeforeStackCheck;
    if (checksBeforeAllocating)
        checkStack(pushedBytes);
    allocateOnStack(jit, bytesToAllocate - pushedBytes);
    if (!checksBeforeAllocating)
        checkStack(bytesToAllocate);
    if (layout.calleeSaveForm == CalleeSaveForm::Stored) {
        for (const RegisterAtOffset& entry : saves) {
            if (entry.reg().isGPR())
                jit.storePtr(entry.reg().gpr(), code.addressInFrame(entry.offset()));
            else
                jit.storeDouble(entry.reg().fpr(), code.addressInFrame(entry.offset()));
        }
    }
}

static void emitCalleeSaveLoadsWithoutRecord(CCallHelpers& jit, const Code& code)
{
    for (const RegisterAtOffset& entry : code.calleeSaveRegisterAtOffsetList()) {
        if (entry.reg().isGPR())
            jit.loadPtr(code.addressInFrame(entry.offset()), entry.reg().gpr());
        else
            jit.loadDouble(code.addressInFrame(entry.offset()), entry.reg().fpr());
    }
}

static void emitFrameTeardownWithoutRecord(CCallHelpers& jit, const Code& code, std::optional<GPRReg> deadRegister)
{
    RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
    const unsigned bytesToRelease = bytesBelowReturnAddress(code);
    const unsigned bytesOfSaves = static_cast<unsigned>(saves.registerCount() * sizeof(CPURegister));
    switch (code.frameLayout().calleeSaveForm) {
    case CalleeSaveForm::PushedAtFrameBottom:
        popCalleeSaves(jit, saves);
        releaseOnStack(jit, bytesToRelease - bytesOfSaves, deadRegister);
        return;
    case CalleeSaveForm::PushedBelowFrameRecord:
        releaseOnStack(jit, bytesToRelease - bytesOfSaves, deadRegister);
        popCalleeSaves(jit, saves);
        return;
    case CalleeSaveForm::Stored:
        emitCalleeSaveLoadsWithoutRecord(jit, code);
        releaseOnStack(jit, bytesToRelease, deadRegister);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}
#endif

void emitFrameSetup(CCallHelpers& jit, GenerationContext& context, const Inst& enterFrame)
{
    Code& code = *context.code;
    const FrameLayout& layout = code.frameLayout();
    ASSERT(layout.isPlanned && layout.hasFrame && !context.frameIsSetUp);
    AllowMacroScratchRegisterUsageIf allowScratch(jit, mayUseMacroScratchRegisters(enterFrame));
#if CPU(X86_64)
    std::span<const Arg> pushedSpills = enterFrame.args().subspan(1 + EnterFrameCustom::numberOfScratchRegisters(enterFrame) + code.calleeSaveRegisterAtOffsetList().registerCount());
    if (!pushedSpills.empty()) {
        emitFrameSetupWithPushes(jit, context, pushedSpills);
        return;
    }
    if (!layout.hasFrameRecord) {
        emitFrameSetupWithoutRecord(jit, context);
        return;
    }
#endif
    RELEASE_ASSERT(layout.hasFrameRecord);
    RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
    unsigned frameSize = code.frameSize();
#if CPU(ARM64)
    if (layout.slotArea == SlotArea::AboveFrameRecord) {
        jit.tagReturnAddress();
        jit.m_assembler.stp<64>(GPRInfo::callFrameRegister, CCallHelpers::linkRegister, CCallHelpers::stackPointerRegister, PairPreIndex(-static_cast<int>(layout.bytesAboveFrameRecord + sizeof(CallerFrameAndPC))));
        jit.move(CCallHelpers::stackPointerRegister, GPRInfo::callFrameRegister);
        emitStackCheck(jit, context, 0, code.bytesBelowFrameRecord());
        pushCalleeSaves(jit, saves);
        return;
    }
#endif
    RELEASE_ASSERT(layout.slotArea == SlotArea::BelowFrameRecord);
    jit.emitFunctionPrologue();
    unsigned pushedBytes = 0;
#if CPU(X86_64)
    unsigned bytesOfSaves = static_cast<unsigned>(saves.registerCount() * sizeof(CPURegister));
    if (layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom) {
        allocateOnStack(jit, frameSize - bytesOfSaves);
        pushCalleeSaves(jit, saves);
        emitStackCheck(jit, context, frameSize, frameSize);
        return;
    }
    if (layout.calleeSaveForm == CalleeSaveForm::PushedBelowFrameRecord) {
        pushCalleeSaves(jit, saves);
        pushedBytes = bytesOfSaves;
    }
#endif
    auto allocate = [&] {
        if (frameSize != pushedBytes)
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(frameSize), CCallHelpers::stackPointerRegister);
    };
    if (frameSize <= code.policy().frame.maxBytesAllocatedBeforeStackCheck) {
        allocate();
        emitStackCheck(jit, context, frameSize, frameSize);
    } else {
        emitStackCheck(jit, context, pushedBytes, frameSize);
        allocate();
    }
    if (layout.calleeSaveForm == CalleeSaveForm::Stored)
        jit.emitSave(saves);
    else
        RELEASE_ASSERT(isX86_64() && layout.calleeSaveForm == CalleeSaveForm::PushedBelowFrameRecord);
}

void emitCalleeSaveRestore(CCallHelpers& jit, const Code& code)
{
    const FrameLayout& layout = code.frameLayout();
    RELEASE_ASSERT(layout.isPlanned && layout.hasFrame && layout.slotArea == SlotArea::BelowFrameRecord);
#if CPU(X86_64)
    if (!layout.hasFrameRecord) {
        emitCalleeSaveLoadsWithoutRecord(jit, code);
        return;
    }
#endif
    jit.emitRestore(code.calleeSaveRegisterAtOffsetList());
}

std::optional<GPRReg> registerDeadBehindFrameTeardown(const Code& code, BasicBlock* block, unsigned indexOfLeaveFrame)
{
    if (indexOfLeaveFrame + 1 >= block->size())
        return std::nullopt;
    Inst& next = block->at(indexOfLeaveFrame + 1);
    // A return passes on its operand and the registers the callee saves.
    if (isReturn(next.kind.opcode) && !code.policy().registers.savedByCallee.contains(registerDeadAtReturns, IgnoreVectors))
        return registerDeadAtReturns;
#if CPU(X86_64)
    if (next.kind.opcode == Patch && next.extraEarlyClobberedRegs().contains(CCallHelpers::s_scratchRegister, IgnoreVectors))
        return CCallHelpers::s_scratchRegister;
#endif
    return std::nullopt;
}

unsigned numberOfWordsAllocatedAtFrameSetup(const Code& code)
{
    return code.frameSize() / sizeof(CPURegister) + !code.frameLayout().hasFrameRecord;
}

void emitFrameTeardown(CCallHelpers& jit, const Code& code, std::optional<GPRReg> deadRegister, std::span<const Arg> poppedReloads)
{
    const FrameLayout& layout = code.frameLayout();
    ASSERT(layout.isPlanned && layout.hasFrame);
#if CPU(X86_64)
    if (!poppedReloads.empty()) {
        emitFrameTeardownWithPops(jit, code, deadRegister, poppedReloads);
        return;
    }
    if (!layout.hasFrameRecord) {
        emitFrameTeardownWithoutRecord(jit, code, deadRegister);
        return;
    }
#else
    UNUSED_PARAM(deadRegister);
    RELEASE_ASSERT(poppedReloads.empty());
#endif
    RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
#if CPU(ARM64)
    if (layout.slotArea == SlotArea::AboveFrameRecord) {
        popCalleeSaves(jit, saves);
        jit.m_assembler.ldp<64>(GPRInfo::callFrameRegister, CCallHelpers::linkRegister, CCallHelpers::stackPointerRegister, PairPostIndex(static_cast<int>(layout.bytesAboveFrameRecord + sizeof(CallerFrameAndPC))));
        return;
    }
#endif
#if CPU(X86_64)
    if (layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom) {
        popCalleeSaves(jit, saves);
        if (code.frameSize() == saves.registerCount() * sizeof(CPURegister))
            jit.emitFunctionEpilogueWithEmptyFrame();
        else
            jit.m_assembler.leave();
        return;
    }
#endif
    jit.emitRestore(saves);
    if (!code.frameSize()) {
        jit.emitFunctionEpilogueWithEmptyFrame();
        return;
    }
#if CPU(X86_64)
    jit.m_assembler.leave();
#else
    jit.emitFunctionEpilogue();
#endif
}

unsigned maxBytesOfFrameTeardown(const Code& code)
{
    const FrameLayout& layout = code.frameLayout();
    ASSERT(layout.isPlanned);
    if (!layout.hasFrame)
        return 0;
    unsigned numberOfSaves = static_cast<unsigned>(code.calleeSaveRegisters().numberOfSetRegisters());
#if CPU(X86_64)
    constexpr unsigned maxBytesOfPop = 2;
    constexpr unsigned maxBytesOfLoad = 9;
    constexpr unsigned bytesOfLeave = 1;
    if (!layout.hasFrameRecord) {
        constexpr unsigned maxBytesOfLoadOffStackPointer = maxBytesOfLoad + 1;
        constexpr unsigned maxBytesOfAdd = 7;
        constexpr unsigned bytesOfPopIntoDeadRegister = 1;
        static_assert(registerDeadAtReturns < X86Registers::r8);
        unsigned bytesReleasedAtOnce = code.frameSize() + static_cast<unsigned>(sizeof(CPURegister)) - (layout.calleeSaveForm == CalleeSaveForm::Stored ? 0 : numberOfSaves * static_cast<unsigned>(sizeof(CPURegister)));
        unsigned bytesOfRelease = !bytesReleasedAtOnce ? 0 : bytesReleasedAtOnce == sizeof(CPURegister) ? bytesOfPopIntoDeadRegister : maxBytesOfAdd;
        return numberOfSaves * (layout.calleeSaveForm == CalleeSaveForm::Stored ? maxBytesOfLoadOffStackPointer : maxBytesOfPop) + bytesOfRelease;
    }
    return numberOfSaves * (layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom ? maxBytesOfPop : maxBytesOfLoad) + bytesOfLeave;
#elif CPU(ARM64)
    constexpr unsigned bytesPerInstruction = 4;
    constexpr unsigned maxEpilogueInstructions = 2;
    return (numberOfSaves + maxEpilogueInstructions) * bytesPerInstruction;
#else
    UNUSED_VARIABLE(numberOfSaves);
    UNREACHABLE_FOR_PLATFORM();
    return 0;
#endif
}

void GenerationContext::setFrameIsSetUp(CCallHelpers& jit, bool isSetUp)
{
    if (frameIsSetUp == isSetUp)
        return;
    frameIsSetUp = isSetUp;
    MacroAssembler::Label label = jit.labelIgnoringWatchpoints();
    if (isSetUp)
        frameRanges.append(FrameRange { label, { } });
    else
        frameRanges.last().end = label;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
