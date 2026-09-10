/*
 * Copyright (C) 2015-2018 Apple Inc. All rights reserved.
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
#include "FTLOSRExitHandle.h"

#if ENABLE(FTL_JIT)

#include "FTLOSRExit.h"
#include "FTLState.h"
#include "FTLThunks.h"
#include "LinkBuffer.h"
#include "ProfilerCompilation.h"

namespace JSC { namespace FTL {

void OSRExitHandle::emitExitThunk(State& state, CCallHelpers& jit)
{
    Profiler::Compilation* compilation = state.graph.compilation();
    CCallHelpers::Label myLabel = jit.label();
    label = myLabel;
    jit.pushToSaveImmediateWithoutTouchingRegisters(CCallHelpers::TrustedImm32(m_index));
#if CPU(X86_64)
    // GIL off the patchable jump below is never repatched to the compiled ramp
    // (no patching of reachable code outside a stop), so every exit of an
    // already-compiled ramp went through the generation thunk and
    // operationCompileFTLOSRExit's published-pointer fast path. Read the
    // pointer compileStub publishes (release) here instead and jump to the
    // ramp directly once it exists; every register is live at an exit, so the
    // one we borrow is saved on the stack and the jump is a ret through it.
    // Flag-off / GIL-on: the patchable jump alone, as before.
    CCallHelpers::JumpList notYetCompiled;
    CCallHelpers::DataLabelPtr codePtrSlot;
    const bool readPublishedRamp = state.vm().gilOff();
    if (readPublishedRamp) {
        jit.subPtr(CCallHelpers::TrustedImm32(sizeof(void*)), CCallHelpers::stackPointerRegister); // slot for the ramp address
        jit.pushToSave(X86Registers::eax);
        codePtrSlot = jit.moveWithPatch(CCallHelpers::TrustedImmPtr(nullptr), X86Registers::eax); // &m_osrExit[i].m_codePtrForConcurrentReaders, patched at link
        jit.loadPtr(CCallHelpers::Address(X86Registers::eax), X86Registers::eax);
        notYetCompiled.append(jit.branchTestPtr(CCallHelpers::Zero, X86Registers::eax));
        jit.storePtr(X86Registers::eax, CCallHelpers::Address(CCallHelpers::stackPointerRegister, sizeof(void*)));
        jit.popToRestore(X86Registers::eax);
        jit.ret(); // pops the ramp address; the exit index stays pushed, as the ramp expects
        notYetCompiled.link(&jit);
        jit.popToRestore(X86Registers::eax);
        jit.addPtr(CCallHelpers::TrustedImm32(sizeof(void*)), CCallHelpers::stackPointerRegister);
    }
#else
    const bool readPublishedRamp = false;
    CCallHelpers::DataLabelPtr codePtrSlot;
#endif
    CCallHelpers::PatchableJump jump = jit.patchableJump();
    jump.linkThunk(CodeLocationLabel<JITThunkPtrTag>(state.vm().getCTIStub(osrExitGenerationThunkGenerator).code()), &jit);
    RefPtr<OSRExitHandle> self = this;
    jit.addLinkTask(
        [self, jump, myLabel, compilation, readPublishedRamp, codePtrSlot] (LinkBuffer& linkBuffer) {
            self->m_jitCode->m_osrExit[self->m_index].m_patchableJump = CodeLocationJump<JSInternalPtrTag>(linkBuffer.locationOf<JSInternalPtrTag>(jump));
            if (readPublishedRamp)
                linkBuffer.patch(codePtrSlot, &self->m_jitCode->m_osrExit[self->m_index].m_codePtrForConcurrentReaders); // m_osrExit is complete by link time, so the element no longer moves
            if (compilation)
                compilation->addOSRExitSite({ linkBuffer.locationOf<JSInternalPtrTag>(myLabel) });
        });
}

} } // namespace JSC::FTL

#endif // ENABLE(FTL_JIT)

