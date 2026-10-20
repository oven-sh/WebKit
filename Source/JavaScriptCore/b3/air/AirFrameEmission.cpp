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
#include "AirInst.h"
#include "AllowMacroScratchRegisterUsageIf.h"
#include "AssemblyHelpersSpoolers.h"
#include "CCallHelpers.h"

namespace JSC { namespace B3 { namespace Air {

static void emitStackCheck(CCallHelpers& jit, GenerationContext& context, unsigned bytesAllocated, unsigned bytesBelow)
{
    if (StackCheckGenerator* generator = context.code->stackCheckGenerator())
        generator->run(jit, context, StackCheckSite { bytesAllocated, bytesBelow });
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

void emitFrameSetup(CCallHelpers& jit, GenerationContext& context, const Inst& enterFrame)
{
    Code& code = *context.code;
    const FrameLayout& layout = code.frameLayout();
    ASSERT(layout.isPlanned && layout.hasFrame && !context.frameIsSetUp);
    AllowMacroScratchRegisterUsageIf allowScratch(jit, mayUseMacroScratchRegisters(enterFrame));
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
    std::span<const Arg> pushedSpills = enterFrame.args().last(layout.numberOfSpillsPushedAtFrameSetup);
    bool hasSavesAtBottom = layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom;
    if (hasSavesAtBottom || !pushedSpills.empty()) {
        unsigned above = frameSize - (hasSavesAtBottom ? bytesOfSaves : 0);
        for (const Arg& spill : pushedSpills)
            jit.push(spill.gpr());
        unsigned left = above - static_cast<unsigned>(pushedSpills.size() * sizeof(CPURegister));
        // Any register serves: this push only allocates the word.
        if (left == sizeof(CPURegister))
            jit.push(GPRInfo::returnValueGPR);
        else if (left)
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(above), CCallHelpers::stackPointerRegister);
        if (hasSavesAtBottom)
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
    jit.emitRestore(code.calleeSaveRegisterAtOffsetList());
}

void emitFrameTeardown(CCallHelpers& jit, const Code& code)
{
    const FrameLayout& layout = code.frameLayout();
    ASSERT(layout.isPlanned && layout.hasFrame);
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
