/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
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
#include "FTLJITFinalizer.h"

#if ENABLE(FTL_JIT)

#include "CodeBlockWithJITType.h"
#include "DFGPlan.h"
#include "FTLState.h"
#include "ProfilerDatabase.h"
#include "PropertyInlineCache.h"
#include "ThunkGenerators.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/ThreadSanitizerSupport.h>

namespace JSC { namespace FTL {

WTF_MAKE_TZONE_ALLOCATED_IMPL(JITFinalizer);

JITFinalizer::JITFinalizer(DFG::Plan& plan)
    : Finalizer(plan)
{
}

JITFinalizer::~JITFinalizer() = default;

size_t JITFinalizer::codeSize()
{
    return m_codeSize;
}

bool JITFinalizer::finalize()
{
    VM& vm = *m_plan.vm();
    WTF::crossModifyingCodeFence();

    m_plan.runMainThreadFinalizationTasks();

    CodeBlock* codeBlock = m_plan.codeBlock();
    m_jitCode->setSize(m_codeSize);
    codeBlock->setJITCode(*m_jitCode);

    if (Options::useHandlerICInFTL()) {
        // Handler-IC sites baked a pointer to this JITCode's JITData into the
        // generated code; fill it in now that the codeBlock knows its FTL frame
        // shape. Note: codeBlock->stackPointerOffset() is only correct after
        // setJITCode() above (it consults the installed jitType/jitCode).
        m_jitCode->initializeHandlerICJITData(codeBlock->globalObject(), codeBlock->stackPointerOffset() * sizeof(Register));

        // Each handler IC dispatches through its m_handler chain on the very
        // first execution, so install the shared slow-path handler as the
        // initial (terminal) node. This must run on the main thread (it touches
        // VM::m_sharedJITStubs) and before the code is installed/executable —
        // both hold here: plan finalization is a main-thread operation and
        // installCode() runs after finalize() returns.
        for (auto* propertyCache : m_jitCode->common.m_handlerPropertyInlineCaches)
            propertyCache->initializeHandlerForOptimizingJIT(codeBlock);
        RELEASE_ASSERT(m_jitCode->common.m_repatchingPropertyInlineCaches.isEmpty());
#if TSAN_ENABLED
        // TSAN §4.4 retired-artifact audit (TSAN-RESULTS residual 1, closed
        // 2026-06-09): same release-publish annotation as the DFG/Baseline
        // install points (DFGJITFinalizer.cpp / CodeBlock.cpp) — pairs with
        // the TSAN_ANNOTATE_HAPPENS_AFTER in ICSlowPathCallFrameTracer. The
        // FTL handler ICs are reached by sibling lites via pointers baked
        // into the installed code; lifetime is the leaked-flag-on JITCode
        // (retireOptimizedJITCode) per SPEC-jit §5.3/I7. No-op outside TSAN.
        for (auto* propertyCache : m_jitCode->common.m_handlerPropertyInlineCaches)
            TSAN_ANNOTATE_HAPPENS_BEFORE(propertyCache);
#endif
    }

    if (Options::dumpFTLCodeSize()) [[unlikely]] {
        auto* baselineCodeBlock = codeBlock->baselineAlternative();
        size_t baselineCodeSize = 0;
        if (auto jitCode = baselineCodeBlock->jitCode())
            baselineCodeSize = jitCode->size();
        dataLogLn("FTL: name:(", codeBlock->inferredNameWithHash(), "),codeSize:(", m_jitCode->size(), "),nodes:(", m_jitCode->numberOfCompiledDFGNodes(), "),baselineCodeSize:(", baselineCodeSize, "),bytecodeCost:(", baselineCodeBlock->bytecodeCost(), ")");
    }

    if (m_plan.compilation()) [[unlikely]]
        vm.m_perBytecodeProfiler->addCompilation(codeBlock, *m_plan.compilation());

    // The codeBlock is now responsible for keeping many things alive (e.g. frozen values)
    // that were previously kept alive by the plan.
    vm.writeBarrier(codeBlock);

    return true;
}

} } // namespace JSC::FTL

#endif // ENABLE(FTL_JIT)
