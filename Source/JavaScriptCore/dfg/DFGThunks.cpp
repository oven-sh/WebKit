/*
 * Copyright (C) 2011-2022 Apple Inc. All rights reserved.
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

#include "config.h"
#include "DFGThunks.h"

#if ENABLE(DFG_JIT)

#include "AssemblyHelpersSpoolers.h"
#include "CCallHelpers.h"
#include "DFGJITCode.h"
#include "DFGOSRExit.h"
#include "DFGOSRExitCompilerCommon.h"
#include "FPRInfo.h"
#include "GPRInfo.h"
#include "LinkBuffer.h"
#include "MacroAssembler.h"
#include "ProbeContext.h"
#include "VMLite.h"

namespace JSC {

// UNGIL U-T3 emitter (defined in jit/AssemblyHelpers.cpp). Self-declaration,
// mirroring FTLOSRExitCompiler.cpp's pattern — no header owns this form yet.
void loadVMLite(AssemblyHelpers&, GPRReg);

namespace DFG {

// UNGIL §A.1.6 (ANNEX A16) — U-T4a, DFG half of the FTL U-T4b fix
// (FTLSaveRestore.cpp materializeBakedScratchBufferDataPointer; duplicated
// locally so DFG does not depend on an ENABLE(FTL_JIT) TU). Resolves the
// CURRENT lite's ScratchBuffer for a process-wide ScratchBufferRegistry
// index and leaves its dataBuffer() pointer in `dest`. Clobbers only `dest`;
// the loads are address-dependent against the release-publishing install
// (VMLite::ensureScratchBufferAtIndex).
static void materializePerLiteScratchBufferDataPointer(CCallHelpers& jit, unsigned bakedIndex, GPRReg dest)
{
    ASSERT(bakedIndex < VMLite::maxScratchSegments * VMLite::scratchSegmentSize);
    loadVMLite(jit, dest);
    jit.loadPtr(
        CCallHelpers::Address(
            dest,
            static_cast<int32_t>(VMLite::offsetOfScratchSegments() + static_cast<ptrdiff_t>(bakedIndex >> VMLite::scratchSegmentShift) * sizeof(void*))),
        dest);
    jit.loadPtr(
        CCallHelpers::Address(
            dest,
            static_cast<int32_t>(static_cast<ptrdiff_t>(bakedIndex & (VMLite::scratchSegmentSize - 1)) * sizeof(void*))),
        dest);
    jit.addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(OBJECT_OFFSETOF(ScratchBuffer, m_buffer))), dest);
}

MacroAssemblerCodeRef<JITThunkPtrTag> osrExitGenerationThunkGenerator(VM& vm)
{
    CCallHelpers jit(nullptr);

    // This needs to happen before we use the scratch buffer because this function also uses the scratch buffer.
    adjustFrameAndStackInOSRExitCompilerThunk<DFG::JITCode>(jit, vm, JITType::DFGJIT);

    // UNGIL §A.1.3/§A.1.6 (U-T4a — the DFG half of FTL's U-T4b fix): this
    // thunk is shared by every thread of the VM. gilOff, (a) osrExitIndex /
    // osrExitJumpDestination are per-lite Group-3 words (a baked &vm store
    // would let two concurrently exiting threads clobber each other's exit
    // index — the loser recovers the WRONG exit's values), and (b) the full
    // register dump below must go to the CURRENT lite's buffer (a baked
    // buffer address lets two concurrently exiting threads interleave their
    // register files — the loser reloads the other thread's registers, which
    // surfaced as cross-object property values in
    // JSTests/threads/jit/spawned-thread-butterfly-stress.js). GIL-on /
    // flag-off keeps today's baked-absolute emission byte-for-byte.
    const bool perLiteMode = vm.gilOff();

    size_t scratchSize = sizeof(EncodedJSValue) * (GPRInfo::numberOfRegisters + FPRInfo::numberOfRegisters);
    unsigned bakedIndex = std::numeric_limits<unsigned>::max();
    EncodedJSValue* buffer = nullptr;
    if (perLiteMode) [[unlikely]]
        bakedIndex = vm.allocateBakedScratchBufferIndex(scratchSize);
    else {
        ScratchBuffer* scratchBuffer = vm.scratchBufferForSize(scratchSize);
        buffer = static_cast<EncodedJSValue*>(scratchBuffer->dataBuffer());
    }

#if CPU(ARM64)
    constexpr GPRReg bufferGPR = CCallHelpers::memoryTempRegister;
    constexpr unsigned firstGPR = 0;
#elif CPU(X86_64)
    GPRReg bufferGPR = jit.scratchRegister();
    constexpr unsigned firstGPR = 0;
#else
    GPRReg bufferGPR = GPRInfo::toRegister(0);
    constexpr unsigned firstGPR = 1;
#endif

    if constexpr (firstGPR) {
        // We're using the firstGPR as the bufferGPR, and need to save it manually.
        // (Platforms taking this leg have no gilOff support — App. R5 — so the
        // baked-absolute store is the only mode here.)
        RELEASE_ASSERT(GPRInfo::numberOfRegisters >= 1);
        RELEASE_ASSERT(bufferGPR == GPRInfo::toRegister(0));
        RELEASE_ASSERT(!perLiteMode);
#if USE(JSVALUE64)
        jit.store64(bufferGPR, buffer);
#else
        jit.store32(bufferGPR, buffer);
#endif
    }

#if USE(JSVALUE64)
    // The trampoline put the exit index in numberTagRegister; publish it for
    // operationCompileOSRExit. gilOff: the CURRENT lite's Group-3 word —
    // bufferGPR is the per-arch reserved assembler temp, free this early.
    if (perLiteMode) [[unlikely]] {
        loadVMLite(jit, bufferGPR);
        jit.store32(GPRInfo::numberTagRegister, CCallHelpers::Address(bufferGPR, static_cast<int32_t>(VMLite::offsetOfPrimitives() + VMLitePrimitives::offsetOf_osrExitIndex())));
    } else
        jit.store32(GPRInfo::numberTagRegister, &vm.osrExitIndex);
#endif

    if (perLiteMode) [[unlikely]]
        materializePerLiteScratchBufferDataPointer(jit, bakedIndex, bufferGPR);
    else
        jit.move(CCallHelpers::TrustedImmPtr(buffer), bufferGPR);

    CCallHelpers::StoreRegSpooler storeSpooler(jit, bufferGPR);

    for (unsigned i = firstGPR; i < GPRInfo::numberOfRegisters; ++i) {
        ptrdiff_t offset = i * sizeof(CPURegister);
        storeSpooler.storeGPR({ GPRInfo::toRegister(i), offset, conservativeWidthWithoutVectors(GPRInfo::toRegister(i)) });
    }
    storeSpooler.finalizeGPR();

    for (unsigned i = 0; i < FPRInfo::numberOfRegisters; ++i) {
        ptrdiff_t offset = (GPRInfo::numberOfRegisters + i) * sizeof(double);
        storeSpooler.storeFPR({ FPRInfo::toRegister(i), offset, conservativeWidthWithoutVectors(FPRInfo::toRegister(i)) });
    }
    storeSpooler.finalizeFPR();

    // This will implicitly pass GPRInfo::callFrameRegister as the first argument based on the operation type.
    jit.setupArguments<decltype(operationCompileOSRExit)>(bufferGPR);
    jit.prepareCallOperation(vm);
    jit.callOperation<OperationPtrTag>(operationCompileOSRExit);

    if (perLiteMode) [[unlikely]]
        materializePerLiteScratchBufferDataPointer(jit, bakedIndex, bufferGPR);
    else
        jit.move(CCallHelpers::TrustedImmPtr(buffer), bufferGPR);
    CCallHelpers::LoadRegSpooler loadSpooler(jit, bufferGPR);

    for (unsigned i = firstGPR; i < GPRInfo::numberOfRegisters; ++i) {
        ptrdiff_t offset = i * sizeof(CPURegister);
        loadSpooler.loadGPR({ GPRInfo::toRegister(i), offset, conservativeWidthWithoutVectors(GPRInfo::toRegister(i)) });
    }
    loadSpooler.finalizeGPR();

    for (unsigned i = 0; i < FPRInfo::numberOfRegisters; ++i) {
        ptrdiff_t offset = (GPRInfo::numberOfRegisters + i) * sizeof(double);
        loadSpooler.loadFPR({ FPRInfo::toRegister(i), offset, conservativeWidthWithoutVectors(FPRInfo::toRegister(i)) });
    }
    loadSpooler.finalizeFPR();

    if constexpr (firstGPR) {
        // We're using the firstGPR as the bufferGPR, and need to restore it manually.
        ASSERT(bufferGPR == GPRInfo::toRegister(0));
#if USE(JSVALUE64)
        jit.load64(buffer, bufferGPR);
#else
        jit.load32(buffer, bufferGPR);
#endif
    }

    // gilOff: operationCompileOSRExit published the destination through the
    // exiting thread's lite; every real register is restored, so only the
    // reserved temp (bufferGPR) may be clobbered for the indirection.
    if (perLiteMode) [[unlikely]] {
        loadVMLite(jit, bufferGPR);
        jit.farJump(CCallHelpers::Address(bufferGPR, static_cast<int32_t>(VMLite::offsetOfPrimitives() + VMLitePrimitives::offsetOf_osrExitJumpDestination())), OSRExitPtrTag);
    } else
        jit.farJump(MacroAssembler::AbsoluteAddress(&vm.osrExitJumpDestination), OSRExitPtrTag);

    LinkBuffer patchBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::DFGThunk);
    return FINALIZE_THUNK(patchBuffer, JITThunkPtrTag, nullptr, "DFG OSR exit generation thunk");
}

MacroAssemblerCodeRef<JITThunkPtrTag> osrEntryThunkGenerator(VM& vm)
{
    AssemblyHelpers jit(nullptr);

    // We get passed the address of a scratch buffer in GPRInfo::returnValueGPR2.
    // The first 8-byte slot of the buffer is the frame size. The second 8-byte slot
    // is the pointer to where we are supposed to jump. The remaining bytes are
    // the new call frame header followed by the locals.
    
    ptrdiff_t offsetOfFrameSize = 0; // This is the DFG frame count.
    ptrdiff_t offsetOfTargetPC = offsetOfFrameSize + sizeof(EncodedJSValue);
    ptrdiff_t offsetOfPayload = offsetOfTargetPC + sizeof(EncodedJSValue);
    ptrdiff_t offsetOfLocals = offsetOfPayload + sizeof(Register) * CallFrame::headerSizeInRegisters;
    
    jit.move(GPRInfo::returnValueGPR2, GPRInfo::regT0);
    jit.loadPtr(MacroAssembler::Address(GPRInfo::regT0, offsetOfFrameSize), GPRInfo::regT1); // Load the frame size.
    jit.negPtr(GPRInfo::regT1, GPRInfo::regT2);
    jit.getEffectiveAddress(MacroAssembler::BaseIndex(GPRInfo::callFrameRegister, GPRInfo::regT2, MacroAssembler::TimesEight), MacroAssembler::stackPointerRegister);
    
    // Copying locals and header from scratch buffer to the new CallFrame. This also replaces
    MacroAssembler::Label loop = jit.label();
    jit.subPtr(MacroAssembler::TrustedImm32(1), GPRInfo::regT1);
    jit.negPtr(GPRInfo::regT1, GPRInfo::regT4);
    jit.loadValue(MacroAssembler::BaseIndex(GPRInfo::regT0, GPRInfo::regT1, MacroAssembler::TimesEight, offsetOfLocals), JSRInfo::jsRegT32);
    jit.storeValue(JSRInfo::jsRegT32, MacroAssembler::BaseIndex(GPRInfo::callFrameRegister, GPRInfo::regT4, MacroAssembler::TimesEight, -static_cast<intptr_t>(sizeof(Register))));
    jit.branchPtr(MacroAssembler::NotEqual, GPRInfo::regT1, MacroAssembler::TrustedImmPtr(std::bit_cast<void*>(-static_cast<intptr_t>(CallFrame::headerSizeInRegisters)))).linkTo(loop, &jit);
    
    jit.loadPtr(MacroAssembler::Address(GPRInfo::regT0, offsetOfTargetPC), GPRInfo::regT1);
    MacroAssembler::Jump ok = jit.branchPtr(MacroAssembler::Above, GPRInfo::regT1, MacroAssembler::TrustedImmPtr(std::bit_cast<void*>(static_cast<intptr_t>(1000))));
    jit.abortWithReason(DFGUnreasonableOSREntryJumpDestination);

    ok.link(&jit);

    jit.jitAssertCodeBlockOnCallFrameIsOptimizingJIT(GPRInfo::regT2);

    if (vm.gilOff()) [[unlikely]] {
        // UNGIL §A.1.3 (U-T4b): topEntryFrame is per-lite Group-3 state
        // GIL-off (doVMEntry publishes through the lite; the VM-block word is
        // inert spare storage). This per-VM thunk is shared by every thread
        // OSR-entering this VM, so resolve the CURRENT lite instead of baking
        // &vm.topEntryFrame. regT3 is dead here (the scratch-buffer copy loop
        // is done; only regT1 [target PC] and callFrameRegister are live), so
        // a caller-save base with the plain skip list restores every VM
        // callee save — same shape as the DFGOSRExit.cpp GenericUnwind
        // gilOff arm. ARM64: regT3 is not the data temp (loadVMLite contract).
        jit.loadVMLite(GPRInfo::regT3);
        jit.loadPtr(MacroAssembler::Address(GPRInfo::regT3, static_cast<int32_t>(VMLite::offsetOfPrimitives() + VMLitePrimitives::offsetOf_topEntryFrame())), GPRInfo::regT3);
        jit.restoreCalleeSavesFromVMEntryFrameCalleeSavesBufferImpl(GPRInfo::regT3, RegisterSet::stackRegisters());
    } else
        jit.restoreCalleeSavesFromEntryFrameCalleeSavesBuffer(vm.topEntryFrame);
    jit.emitMaterializeTagCheckRegisters();
#if USE(JSVALUE64)
    jit.emitGetFromCallFrameHeaderPtr(CallFrameSlot::codeBlock, GPRInfo::jitDataRegister);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::jitDataRegister, CodeBlock::offsetOfJITData()), GPRInfo::jitDataRegister);
#endif

    jit.farJump(GPRInfo::regT1, GPRInfo::callFrameRegister);

    LinkBuffer patchBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::DFGOSREntry);
    return FINALIZE_THUNK(patchBuffer, JITThunkPtrTag, nullptr, "DFG OSR entry thunk");
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
