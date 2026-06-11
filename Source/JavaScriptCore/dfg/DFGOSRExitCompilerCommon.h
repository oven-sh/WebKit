/*
 * Copyright (C) 2013-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#if ENABLE(DFG_JIT)

#include "CCallHelpers.h"
#include "DFGOSRExit.h"
#include "DFGCommonData.h"
#include "DFGJITCode.h"
#include "FTLJITCode.h"
#include "RegisterSet.h"
#include "VMLite.h"
#include <limits>

namespace JSC {

// UNGIL U-T3 emitter (defined in jit/AssemblyHelpers.cpp). Self-declaration,
// mirroring that TU's own pattern — no header owns this form yet (the
// AssemblyHelpers.h member surface is macro-gated on
// JSC_ASSEMBLYHELPERS_HAS_LOAD_VMLITE, which the header-owning task defines).
void loadVMLite(AssemblyHelpers&, GPRReg);

namespace DFG {

void handleExitCounts(VM&, CCallHelpers&, const OSRExitBase&);
void reifyInlinedCallFrames(CCallHelpers&, const OSRExitBase&);
void adjustAndJumpToTarget(VM&, CCallHelpers&, const OSRExitBase&);
CCallHelpers::Address calleeSaveSlot(InlineCallFrame*, CodeBlock* baselineCodeBlock, GPRReg calleeSave);

template <typename JITCodeType>
void adjustFrameAndStackInOSRExitCompilerThunk(AssemblyHelpers& jit, VM& vm, JITType jitType)
{
    ASSERT(jitType == JITType::DFGJIT || jitType == JITType::FTLJIT);

    bool isFTLOSRExit = jitType == JITType::FTLJIT;
    RegisterSet registersToPreserve;
    registersToPreserve.add(GPRInfo::regT0, IgnoreVectors);
    if (isFTLOSRExit) {
        // FTL can use the scratch registers for values. The code below uses
        // the scratch registers. We need to preserve them before doing anything.
        registersToPreserve.merge(RegisterSet::macroClobberedGPRs());
    }

    size_t scratchSize = sizeof(void*) * registersToPreserve.numberOfSetGPRs();
    if (isFTLOSRExit)
        scratchSize += sizeof(void*);

    // UNGIL §A.1.6 (ANNEX A16, U-T4b): this thunk is shared by every thread
    // of its VM. gilOff, a baked buffer address would let two concurrently
    // exiting threads clobber each other's preserved registers, so the
    // gilOff-mode thunk bakes a process-wide ScratchBufferRegistry INDEX and
    // emits the frozen `loadVMLite -> segment -> [index]` sequence per use
    // (rematerialization per §A.1.2; clobbers only the dest GPR, which is
    // saved at each use site below). GIL-on/flag-off keeps the baked address
    // byte-for-byte.
    const bool bakedIndexMode = vm.gilOff();
    unsigned bakedIndex = std::numeric_limits<unsigned>::max();
    char* buffer = nullptr;
    if (bakedIndexMode) [[unlikely]]
        bakedIndex = vm.allocateBakedScratchBufferIndex(scratchSize);
    else {
        ScratchBuffer* scratchBuffer = vm.scratchBufferForSize(scratchSize);
        buffer = static_cast<char*>(scratchBuffer->dataBuffer());
    }
    auto materializeBufferBase = [&] (GPRReg dest) {
        loadVMLite(jit, dest);
        jit.loadPtr(
            MacroAssembler::Address(
                dest,
                static_cast<int32_t>(VMLite::offsetOfScratchSegments() + static_cast<ptrdiff_t>(bakedIndex >> VMLite::scratchSegmentShift) * sizeof(void*))),
            dest);
        jit.loadPtr(
            MacroAssembler::Address(
                dest,
                static_cast<int32_t>(static_cast<ptrdiff_t>(bakedIndex & (VMLite::scratchSegmentSize - 1)) * sizeof(void*))),
            dest);
        jit.addPtr(MacroAssembler::TrustedImm32(static_cast<int32_t>(OBJECT_OFFSETOF(ScratchBuffer, m_buffer))), dest);
    };

    jit.pushToSave(GPRInfo::regT1);
    if (bakedIndexMode) [[unlikely]]
        materializeBufferBase(GPRInfo::regT1);
    else
        jit.move(MacroAssembler::TrustedImmPtr(buffer), GPRInfo::regT1);

    unsigned storeOffset = 0;
    registersToPreserve.forEach([&](Reg reg) {
        jit.storePtr(reg.gpr(), MacroAssembler::Address(GPRInfo::regT1, storeOffset));
        storeOffset += sizeof(void*);
    });

    if (isFTLOSRExit) {
        // FTL OSRExits are entered via the code FTLExitThunkGenerator emits which does
        // pushToSaveImmediateWithoutTouchRegisters with the OSR exit index. We need to load
        // that top value and then push it back when we reset our SP.
        jit.loadPtr(MacroAssembler::Address(MacroAssembler::stackPointerRegister, MacroAssembler::pushToSaveByteOffset()), GPRInfo::regT0);
        jit.storePtr(GPRInfo::regT0, MacroAssembler::Address(GPRInfo::regT1, registersToPreserve.numberOfSetGPRs() * sizeof(void*)));
    }
    jit.popToRestore(GPRInfo::regT1);

    // We need to reset FP in the case of an exception.
    if (bakedIndexMode) [[unlikely]] {
        // UNGIL §A.1.3: callFrameForCatch is per-lite Group-3 state gilOff —
        // resolve the CURRENT thread's lite instead of baking &vm's slot.
        loadVMLite(jit, GPRInfo::regT0);
        jit.loadPtr(
            MacroAssembler::Address(
                GPRInfo::regT0,
                static_cast<int32_t>(VMLite::offsetOfPrimitives() + VMLitePrimitives::offsetOf_callFrameForCatch())),
            GPRInfo::regT0);
    } else
        jit.loadPtr(vm.addressOfCallFrameForCatch(), GPRInfo::regT0);
    MacroAssembler::Jump didNotHaveException = jit.branchTestPtr(MacroAssembler::Zero, GPRInfo::regT0);
    jit.move(GPRInfo::regT0, GPRInfo::callFrameRegister);
    didNotHaveException.link(&jit);
    // We need to make sure SP is correct in case of an exception.
    jit.loadPtr(MacroAssembler::Address(GPRInfo::callFrameRegister, CallFrameSlot::codeBlock * static_cast<int>(sizeof(Register))), GPRInfo::regT0);
    jit.loadPtr(MacroAssembler::Address(GPRInfo::regT0, CodeBlock::jitCodeOffset()), GPRInfo::regT0);
    jit.addPtr(MacroAssembler::TrustedImm32(JITCodeType::commonDataOffset()), GPRInfo::regT0);
    jit.load32(MacroAssembler::Address(GPRInfo::regT0, CommonData::frameRegisterCountOffset()), GPRInfo::regT0);
    // This does virtualRegisterForLocal(frameRegisterCount - 1)*sizeof(Register) where:
    // virtualRegisterForLocal(frameRegisterCount - 1)
    //     = VirtualRegister::localToOperand(frameRegisterCount - 1)
    //     = -1 - (frameRegisterCount - 1)
    //     = -frameRegisterCount
    jit.neg32(GPRInfo::regT0);
    jit.mul32(MacroAssembler::TrustedImm32(sizeof(Register)), GPRInfo::regT0, GPRInfo::regT0);
#if USE(JSVALUE64)
    jit.signExtend32ToPtr(GPRInfo::regT0, GPRInfo::regT0);
#endif
    jit.addPtr(GPRInfo::callFrameRegister, GPRInfo::regT0);
    jit.move(GPRInfo::regT0, MacroAssembler::stackPointerRegister);

    if (isFTLOSRExit) {
        // Leave space for saving the OSR Exit Index.
        jit.subPtr(MacroAssembler::TrustedImm32(MacroAssembler::pushToSaveByteOffset()), MacroAssembler::stackPointerRegister);
    }
    jit.pushToSave(GPRInfo::regT1);

    if (bakedIndexMode) [[unlikely]]
        materializeBufferBase(GPRInfo::regT1);
    else
        jit.move(MacroAssembler::TrustedImmPtr(buffer), GPRInfo::regT1);
    if (isFTLOSRExit) {
        // FTL OSRExits are entered via FTLExitThunkGenerator code with does
        // pushToSaveImmediateWithoutTouchRegisters. We need to load that top
        // register and then store it back when we have our SP back to a safe value.
        jit.loadPtr(MacroAssembler::Address(GPRInfo::regT1, registersToPreserve.numberOfSetGPRs() * sizeof(void*)), GPRInfo::regT0);
        jit.storePtr(GPRInfo::regT0, MacroAssembler::Address(MacroAssembler::stackPointerRegister, MacroAssembler::pushToSaveByteOffset()));
    }

    unsigned loadOffset = 0;
    registersToPreserve.forEach([&](Reg reg) {
        jit.loadPtr(MacroAssembler::Address(GPRInfo::regT1, loadOffset), reg.gpr());
        loadOffset += sizeof(void*);
    });
    jit.popToRestore(GPRInfo::regT1);
}


} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
