/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
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
#include "FTLLazySlowPath.h"

#if ENABLE(FTL_JIT)

#include "CodeBlock.h"
#include "LinkBuffer.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace FTL {

WTF_MAKE_TZONE_ALLOCATED_IMPL(LazySlowPath);

LazySlowPath::~LazySlowPath() = default;

void LazySlowPath::initialize(
    CodeLocationJump<JSInternalPtrTag> patchableJump, CodeLocationLabel<JSInternalPtrTag> done,
    CodeLocationLabel<ExceptionHandlerPtrTag> exceptionTarget,
    const RegisterSet& usedRegisters, CallSiteIndex callSiteIndex, RefPtr<Generator> generator
    )
{
    m_patchableJump = patchableJump;
    m_done = done;
    m_exceptionTarget = exceptionTarget;
    m_usedRegisters = usedRegisters.toScalarRegisterSet();
    m_callSiteIndex = callSiteIndex;
    m_generator = generator;
}

void LazySlowPath::generate(CodeBlock* codeBlock)
{
    RELEASE_ASSERT(!m_stub);

    CCallHelpers jit(codeBlock);
    GenerationParams params;
    CCallHelpers::JumpList exceptionJumps;
    params.exceptionJumps = m_exceptionTarget ? &exceptionJumps : nullptr;
    params.lazySlowPath = this;

    m_generator->run(jit, params);

    params.doneJumps.linkThunk(m_done, &jit);
    if (m_exceptionTarget)
        exceptionJumps.linkThunk(m_exceptionTarget, &jit);
    LinkBuffer linkBuffer(jit, codeBlock, LinkBuffer::Profile::FTLThunk, JITCompilationMustSucceed);
    m_stub = FINALIZE_CODE_FOR(codeBlock, linkBuffer, JITStubRoutinePtrTag, nullptr, "Lazy slow path call stub");

    // UNGIL U-T4b: gilOff, other mutators may be concurrently EXECUTING the
    // patchable jump; repatchJump rewrites an unaligned rel32 on x86_64 with
    // no atomicity guarantee (cross-modifying code; ISB1 only licenses
    // in-stop patching). Leave the jump pointing at the generation thunk —
    // operationCompileFTLLazySlowPath returns this stub for every subsequent
    // traversal, and the thunk tail-calls it; the protocol stays data-only.
    // T8: instead of re-acquiring ftlLazySlowPathGenerationLock on every such
    // traversal, release-publish the tagged code pointer here so the
    // operation's lock-free acquire-load fast path observes a fully
    // constructed stub (write-once double-checked publication). GIL-on keeps
    // the repatch byte-for-byte and never touches m_stubCodePtr.
    if (!codeBlock->vm().gilOff()) [[likely]]
        MacroAssembler::repatchJump(m_patchableJump, CodeLocationLabel<JITStubRoutinePtrTag>(m_stub.code()));
    else
        m_stubCodePtr.store(m_stub.code().taggedPtr(), std::memory_order_release);
}

} } // namespace JSC::FTL

#endif // ENABLE(FTL_JIT)
