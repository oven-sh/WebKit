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
    // A2-amend round 4: one extra trailing slot in perLiteMode carrying the
    // same-VM-validated address of the osrExitJumpDestination word (per-lite
    // copy, or VM-block fallback in Release). The validation must run where
    // free GPRs exist (after the register dump): bufferGPR is the x86_64
    // assembler scratch (r11), and a branchPtr against a TrustedImmPtr
    // materializes the immediate INTO r11 — using bufferGPR as the compare
    // base there self-clobbers (this exact bug shipped in the first cut of
    // this guard: always-foreign compare => Debug trap / Release wild jump).
    const ptrdiff_t validatedDestinationSlotOffset = scratchSize;
    unsigned bakedIndex = std::numeric_limits<unsigned>::max();
    EncodedJSValue* buffer = nullptr;
    if (perLiteMode) [[unlikely]]
        bakedIndex = vm.allocateBakedScratchBufferIndex(scratchSize + sizeof(EncodedJSValue));
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
        jit.store64(bufferGPR, buffer);
    }

#if USE(JSVALUE64)
    // The trampoline put the exit index in numberTagRegister; publish it for
    // operationCompileOSRExit. gilOff: deferred until after the register
    // dump below (see the same-VM guard block) — only bufferGPR is free
    // this early, and the guard needs it as the branchPtr immediate scratch.
    if (!perLiteMode)
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

#if USE(JSVALUE64)
    if (perLiteMode) [[unlikely]] {
        // Same-VM guard (K4 table-I Group-3 row addendum; A2-amend round 4 —
        // the one sibling Group-3 reader/writer pair the round-3 audit
        // missed): osrExitIndex / osrExitJumpDestination are consumed and
        // published by operationCompileOSRExit through VM::group3Primitives(),
        // whose lite arm requires lite->vm == &vm (this thunk is keyed per-VM
        // in vm.jitStubs, so &vm is an emission-time immediate). A foreign
        // gilOff lite would make this store invisible to that read, and the
        // final indirect farJump through a foreign/stale per-lite word is a
        // wild PC (the A4 face). Validate ONCE here, where the register dump
        // above has freed regT0 (restored by the load spooler later), and
        // stash the validated destination word ADDRESS in the extra scratch
        // slot; the post-restore jump (only bufferGPR free there, and on
        // x86_64 bufferGPR doubles as the branchPtr immediate scratch) then
        // needs no compare. Never-taken under JSLock::didAcquireLock; ASSERT
        // builds fail-stop instead of silently diverging (family disposition).
        loadVMLite(jit, GPRInfo::regT0);
        auto foreignLite = jit.branchPtr(CCallHelpers::NotEqual, CCallHelpers::Address(GPRInfo::regT0, static_cast<int32_t>(VMLite::offsetOfVM())), CCallHelpers::TrustedImmPtr(&vm));
        jit.store32(GPRInfo::numberTagRegister, CCallHelpers::Address(GPRInfo::regT0, static_cast<int32_t>(VMLite::offsetOfPrimitives() + VMLitePrimitives::offsetOf_osrExitIndex())));
        jit.addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(VMLite::offsetOfPrimitives() + VMLitePrimitives::offsetOf_osrExitJumpDestination())), GPRInfo::regT0);
        auto destinationResolved = jit.jump();
        foreignLite.link(&jit);
        // A4-amend (control-flow-integrity tightening): the validated word
        // feeds the final indirect farJump, so the foreign-lite arm
        // fail-stops in ALL build flavors — a Release writer-symmetry
        // fallback to the shared VM-block destination word would narrow
        // the A4 wild-pc window, not close it. Never-taken under
        // JSLock::didAcquireLock; deterministic trap at a known PC beats a
        // narrowed wild jump.
        jit.breakpoint();
        destinationResolved.link(&jit);
        materializePerLiteScratchBufferDataPointer(jit, bakedIndex, bufferGPR); // The guard block clobbered the x86_64 scratch (== bufferGPR).
        jit.storePtr(GPRInfo::regT0, CCallHelpers::Address(bufferGPR, static_cast<int32_t>(validatedDestinationSlotOffset)));
    }
#endif

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
        jit.load64(buffer, bufferGPR);
    }

    // gilOff: operationCompileOSRExit published the destination through the
    // exiting thread's lite; every real register is restored, so only the
    // reserved temp (bufferGPR) may be clobbered for the indirection.
#if USE(JSVALUE64)
    if (perLiteMode) [[unlikely]] {
        // Jump through the same-VM-validated destination word address stashed
        // by the guard block above (per-lite copy, or the VM-block word in
        // the Release foreign-lite fallback). Only bufferGPR may be clobbered
        // here — every real register was just restored — which is exactly why
        // the validation could not live here (see the guard block comment).
        materializePerLiteScratchBufferDataPointer(jit, bakedIndex, bufferGPR);
        jit.loadPtr(CCallHelpers::Address(bufferGPR, static_cast<int32_t>(validatedDestinationSlotOffset)), bufferGPR);
        jit.farJump(CCallHelpers::Address(bufferGPR), OSRExitPtrTag);
    } else
        jit.farJump(MacroAssembler::AbsoluteAddress(&vm.osrExitJumpDestination), OSRExitPtrTag);
#else
    // The slot-initializing guard block above is JSVALUE64-only, so the
    // validated-destination scratch slot is never written here; jumping
    // through it would be a wild PC. gilOff is 64-bit-only (App. R5), so
    // perLiteMode must be impossible on this leg — fail-stop, mirroring the
    // firstGPR convention above.
    RELEASE_ASSERT(!perLiteMode);
    jit.farJump(MacroAssembler::AbsoluteAddress(&vm.osrExitJumpDestination), OSRExitPtrTag);
#endif

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
    jit.emitGetFromCallFrameHeaderPtr(CallFrameSlot::codeBlock, GPRInfo::jitDataRegister);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::jitDataRegister, CodeBlock::offsetOfJITData()), GPRInfo::jitDataRegister);

    jit.farJump(GPRInfo::regT1, GPRInfo::callFrameRegister);

    LinkBuffer patchBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::DFGOSREntry);
    return FINALIZE_THUNK(patchBuffer, JITThunkPtrTag, nullptr, "DFG OSR entry thunk");
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
