/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineFrame.h"

#if ENABLE(AOT) && CPU(X86_64)

#include "AOTMachineFrameForms.h"
#include "AOTMachineTarget.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "StackAlignment.h"

namespace JSC { namespace AOT { namespace Machine {

static_assert(2 * sizeof(CPURegister) == stackAlignmentBytes(), "the return address and the pushed frame pointer together leave the stack pointer aligned behind the frame record");

// rbp has to point at this frame's record wherever a restore or a teardown is written.
static CCallHelpers::Address x86FrameWordAt(ptrdiff_t offsetFromFP)
{
    return CCallHelpers::Address(CCallHelpers::framePointerRegister, safeCast<int32_t>(offsetFromFP));
}

// [begin, end) are offsets from the frame pointer. There is no red zone: what lies below the stack pointer belongs to whoever interrupts the function.
static bool x86FrameHoldsWords(const Frame& frame, ptrdiff_t begin, ptrdiff_t end)
{
    constexpr ptrdiff_t wordSize = sizeof(CPURegister);
    return begin <= end && end <= 0
        && begin >= -static_cast<ptrdiff_t>(frame.bytesBelowFrameRecord())
        && !(begin % wordSize) && !(end % wordSize);
}

static void clearWordsOfX86Frame(CCallHelpers& jit, int32_t begin, int32_t end)
{
    constexpr int32_t wordSize = sizeof(CPURegister);
    if (begin == end)
        return;

    const GPRReg zero = jit.scratchRegister();
    jit.xor32(zero, zero);
    for (int32_t offset = begin; offset < end; offset += wordSize)
        jit.store64(zero, x86FrameWordAt(offset));
}

// General registers alone, one word apart, the last right below the record.
static bool x86FrameSavesCanBePushed(const RegisterAtOffsetList& saves)
{
    constexpr ptrdiff_t wordSize = sizeof(CPURegister);
    size_t count = saves.registerCount();
    for (size_t index = 0; index < count; ++index) {
        const RegisterAtOffset& save = saves.at(index);
        if (!save.reg().isGPR() || save.offset() != -static_cast<ptrdiff_t>(count - index) * wordSize)
            return false;
    }
    return true;
}

// Below the saves lies nothing, or the one word that makes their number even. A pop of each word then takes one or two bytes for the four of a load.
static bool x86FrameHoldsNothingButPushedSaves(const Frame& frame)
{
    constexpr size_t wordSize = sizeof(CPURegister);
    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    size_t count = saves.registerCount();
    return count && frame.bytesBelowFrameRecord() == (count + count % 2) * wordSize && x86FrameSavesCanBePushed(saves);
}

void emitFrameSetup(CCallHelpers& jit, const Procedure& proc)
{
    constexpr ptrdiff_t wordSize = sizeof(CPURegister);
    const Frame& frame = proc.frame();
    RELEASE_ASSERT(frame.hasFrame());
    if (!frame.hasFrameRecord() || frame.savesLieAtBottom()) {
        emitFrameSetup(jit, proc, { });
        return;
    }
    const unsigned bytesBelowFrameRecord = frame.bytesBelowFrameRecord();
    RELEASE_ASSERT(!(bytesBelowFrameRecord % stackAlignmentBytes()));

    // Clearing words takes the assembler's scratch register, and the stack check may too.
    AllowMacroScratchRegisterUsage allowScratch(jit);

    jit.push(CCallHelpers::framePointerRegister);
    jit.move(CCallHelpers::stackPointerRegister, CCallHelpers::framePointerRegister);

    const bool allocatesBeforeCheck = bytesBelowFrameRecord <= maxBytesAllocatedBeforeStackCheck;
    // The run of saves ends at the record, so a push of each, the last of the run first, stores it where it belongs: one or two bytes for four.
    // A check that leaves by throwing finds the registers as the caller left them, whether they are stored or not.
    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    const bool pushesSaves = allocatesBeforeCheck && x86FrameSavesCanBePushed(saves);
    unsigned bytesPushed = 0;
    if (pushesSaves) {
        for (size_t index = saves.registerCount(); index--;) {
            jit.push(saves.at(index).reg().gpr());
            bytesPushed += wordSize;
        }
    }
    RELEASE_ASSERT(bytesPushed <= bytesBelowFrameRecord);
    auto allocate = [&] {
        // Nothing reads the word that makes the number of saves even, and the frame pointer names no cell: one byte for four.
        if (bytesPushed && bytesBelowFrameRecord - bytesPushed == wordSize)
            jit.push(CCallHelpers::framePointerRegister);
        else if (bytesBelowFrameRecord != bytesPushed)
            jit.sub64(CCallHelpers::TrustedImm32(safeCast<int32_t>(bytesBelowFrameRecord - bytesPushed)), CCallHelpers::stackPointerRegister);
    };

    if (allocatesBeforeCheck)
        allocate();
    // The check may call a stub, whose return address lands below the stack pointer, or leave by throwing, which unwinds from the record. Nothing below the record is written yet but the saves.
    if (StackCheckGenerator generator = proc.stackCheckGenerator())
        generator(proc.stackCheckContext(), jit, frame, StackCheckSite { bytesBelowFrameRecord, allocatesBeforeCheck, frame.m_nothingCountsOnStackCheck });
    if (!allocatesBeforeCheck)
        allocate();

    for (const RegisterAtOffset& save : saves) {
        ASSERT(save.width() == Width64);
        RELEASE_ASSERT(x86FrameHoldsWords(frame, save.offset(), save.offset() + wordSize));
        if (pushesSaves)
            continue;
        if (save.reg().isGPR())
            jit.store64(save.reg().gpr(), x86FrameWordAt(save.offset()));
        else
            jit.storeDouble(save.reg().fpr(), x86FrameWordAt(save.offset()));
    }

    const int32_t beginOfWordsToClear = frame.m_beginOfWordsClearedAtSetup;
    const int32_t endOfWordsToClear = frame.m_endOfWordsClearedAtSetup;
    RELEASE_ASSERT(x86FrameHoldsWords(frame, beginOfWordsToClear, endOfWordsToClear));
    RELEASE_ASSERT(beginOfWordsToClear == endOfWordsToClear || endOfWordsToClear <= -static_cast<ptrdiff_t>(frame.numberOfCalleeSaves()) * wordSize);
    clearWordsOfX86Frame(jit, beginOfWordsToClear, endOfWordsToClear);
}

void emitCalleeSaveRestore(CCallHelpers& jit, const Frame& frame)
{
    RELEASE_ASSERT(frame.hasFrame());
    // Whoever restores the saves and goes on reading the frame needs the record, and says so.
    RELEASE_ASSERT(frame.hasFrameRecord());
    for (const RegisterAtOffset& save : frame.calleeSaveRegisterAtOffsetList()) {
        ASSERT(save.width() == Width64);
        if (save.reg().isGPR())
            jit.load64(x86FrameWordAt(save.offset()), save.reg().gpr());
        else
            jit.loadDouble(x86FrameWordAt(save.offset()), save.reg().fpr());
    }
}

void emitFrameTeardown(CCallHelpers& jit, const Frame& frame)
{
    if (!frame.hasFrameRecord() || frame.savesLieAtBottom()) {
        emitTeardownOfFrameInOtherForm(jit, frame);
        return;
    }

    if (x86FrameHoldsNothingButPushedSaves(frame)) {
        const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
        if (saves.registerCount() % 2)
            jit.pop(scratchGPR);
        for (const RegisterAtOffset& save : saves)
            jit.pop(save.reg().gpr());
        jit.pop(CCallHelpers::framePointerRegister);
        return;
    }

    // There is no red zone: the saves are read before the stack pointer passes them.
    emitCalleeSaveRestore(jit, frame);

    // leave is mov rsp, rbp; pop rbp in one byte. The macro assembler has no name for it.
    if (frame.bytesBelowFrameRecord())
        jit.m_assembler.leave();
    else
        jit.pop(CCallHelpers::framePointerRegister);
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(X86_64)
