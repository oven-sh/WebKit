/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineFrameForms.h"

#if ENABLE(AOT) && CPU(X86_64)

#include "AOTMachineTarget.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "StackAlignment.h"

namespace JSC { namespace AOT { namespace Machine {

static constexpr unsigned bytesOfPushedFrameWord = sizeof(CPURegister);

static void allocateWordsOfPushedFrame(CCallHelpers& jit, unsigned numberOfWords)
{
    if (numberOfWords == 1)
        jit.push(CCallHelpers::framePointerRegister);
    else if (numberOfWords)
        jit.sub64(CCallHelpers::TrustedImm32(safeCast<int32_t>(numberOfWords * bytesOfPushedFrameWord)), CCallHelpers::stackPointerRegister);
}

static void pushSavesOfFrame(CCallHelpers& jit, const RegisterAtOffsetList& saves)
{
    for (size_t index = saves.registerCount(); index--;)
        jit.push(saves.at(index).reg().gpr());
}

static void popSavesOfFrame(CCallHelpers& jit, const RegisterAtOffsetList& saves)
{
    for (const RegisterAtOffset& save : saves)
        jit.pop(save.reg().gpr());
}

void emitFrameSetup(CCallHelpers& jit, const Procedure& proc, std::span<const GPRReg> registersPushedIntoFirstSlots)
{
    const Frame& frame = proc.frame();
    RELEASE_ASSERT(frame.hasFrame());
    if (frame.hasFrameRecord() && !frame.savesLieAtBottom() && registersPushedIntoFirstSlots.empty()) {
        emitFrameSetup(jit, proc);
        return;
    }

    const unsigned bytesBelowFrameRecord = frame.bytesBelowFrameRecord();
    RELEASE_ASSERT(!(bytesBelowFrameRecord % stackAlignmentBytes()) && bytesBelowFrameRecord <= maxBytesAllocatedBeforeStackCheck);
    RELEASE_ASSERT(frame.beginOfWordsClearedAtSetup() == frame.endOfWordsClearedAtSetup());
    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    for (size_t index = 0; index < saves.registerCount(); ++index)
        RELEASE_ASSERT(saves.at(index).reg().isGPR() && saves.at(index).offset() == offsetOfFirstSaveOf(frame) + static_cast<ptrdiff_t>(index * bytesOfPushedFrameWord));

    AllowMacroScratchRegisterUsage allowScratch(jit);

    if (frame.hasFrameRecord()) {
        jit.push(CCallHelpers::framePointerRegister);
        jit.move(CCallHelpers::stackPointerRegister, CCallHelpers::framePointerRegister);
    }
    if (!frame.savesLieAtBottom())
        pushSavesOfFrame(jit, saves);

    unsigned wordsToAllocate = numberOfWordsBesideSavesOf(frame);
    if (!registersPushedIntoFirstSlots.empty()) {
        const unsigned wordsAboveSlots = numberOfWordsAboveSlotsOf(frame);
        const unsigned wordsPushed = static_cast<unsigned>(registersPushedIntoFirstSlots.size());
        RELEASE_ASSERT(wordsAboveSlots + wordsPushed <= wordsToAllocate);
        allocateWordsOfPushedFrame(jit, wordsAboveSlots);
        for (GPRReg reg : registersPushedIntoFirstSlots)
            jit.push(reg);
        wordsToAllocate -= wordsAboveSlots + wordsPushed;
    }
    allocateWordsOfPushedFrame(jit, wordsToAllocate);
    if (frame.savesLieAtBottom())
        pushSavesOfFrame(jit, saves);

    if (StackCheckGenerator generator = proc.stackCheckGenerator())
        generator(proc.stackCheckContext(), jit, frame, StackCheckSite { bytesBelowFrameRecord, true, frame.nothingCountsOnStackCheck() });
}

void emitTeardownOfFrameInOtherForm(CCallHelpers& jit, const Frame& frame)
{
    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    if (frame.savesLieAtBottom()) {
        RELEASE_ASSERT(frame.hasFrameRecord() && numberOfWordsBesideSavesOf(frame));
        popSavesOfFrame(jit, saves);
        jit.m_assembler.leave();
        return;
    }

    RELEASE_ASSERT(!frame.hasFrameRecord());
    unsigned wordsBelowSaves = numberOfWordsBesideSavesOf(frame);
    if (wordsBelowSaves == 1)
        jit.pop(scratchGPR);
    else if (wordsBelowSaves)
        jit.add64(CCallHelpers::TrustedImm32(safeCast<int32_t>(wordsBelowSaves * bytesOfPushedFrameWord)), CCallHelpers::stackPointerRegister);
    popSavesOfFrame(jit, saves);
}

void emitTeardownOfFrameThatPopsWords(CCallHelpers& jit, const Frame& frame, std::span<const GPRReg> registersPoppedOutOfWords)
{
    RELEASE_ASSERT(!frame.hasFrameRecord() && !frame.savesLieAtBottom() && registersPoppedOutOfWords.size() == numberOfWordsBesideSavesOf(frame));
    unsigned numberOfWordsThatNobodyWants = 0;
    auto giveBackWordsThatNobodyWants = [&] {
        if (numberOfWordsThatNobodyWants == 1)
            jit.pop(scratchGPR);
        else if (numberOfWordsThatNobodyWants)
            jit.add64(CCallHelpers::TrustedImm32(safeCast<int32_t>(numberOfWordsThatNobodyWants * bytesOfPushedFrameWord)), CCallHelpers::stackPointerRegister);
        numberOfWordsThatNobodyWants = 0;
    };
    for (GPRReg reg : registersPoppedOutOfWords) {
        if (reg == InvalidGPRReg) {
            ++numberOfWordsThatNobodyWants;
            continue;
        }
        giveBackWordsThatNobodyWants();
        jit.pop(reg);
    }
    giveBackWordsThatNobodyWants();
    popSavesOfFrame(jit, frame.calleeSaveRegisterAtOffsetList());
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(X86_64)
