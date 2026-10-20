/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHintEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTCodeRecords.h"
#include "AOTCodeSites.h"
#include "AOTInstance.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsOperations.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "LinkBuffer.h"
#include "StackManager.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr ptrdiff_t offsetOfTrapAwareSoftStackLimit = AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit();
static_assert(static_cast<int64_t>(StackManager::StopRequestMarkerValue) < 0);

static constexpr StubKey pollSlowKey = callOperationPreservingRegisters({ .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });

static void emitFaultingLoad(CCallHelpers& jit, const B3::StackmapGenerationParams& params, CodeRecords* records, uint32_t site)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    bool isFrameless = !params.context().frameIsSetUp;
    CCallHelpers::Label label = jit.labelIgnoringWatchpoints();
    size_t start = jit.m_assembler.codeSize();
    jit.load32(CCallHelpers::Address(instanceGPR, InstanceLayout::offsetOfLoopPollPage() + (isFrameless ? offsetOfLeafPollInLoopPollPage : 0)), jit.scratchRegister());
    RELEASE_ASSERT(jit.m_assembler.codeSize() - start == sizeOfLoopPoll);
    jit.addLinkTask([=](LinkBuffer& linkBuffer) {
        records->traps.pollSites.append(PollSite { linkBuffer.offsetOf(label), isFrameless, OptionalIndex { site } });
    });
}

static void emitCompareAndCall(CCallHelpers& jit, const B3::StackmapGenerationParams& params, CodeRecords* records, uint32_t site)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    RELEASE_ASSERT(params.context().frameIsSetUp);
    CCallHelpers::Jump isInterrupted = jit.branchTest64(CCallHelpers::Signed, CCallHelpers::Address(instanceGPR, offsetOfTrapAwareSoftStackLimit));
    CCallHelpers::Label done = jit.label();
    params.addLatePath([=](CCallHelpers& jit) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        isInterrupted.link(&jit);
        records->stubCalls.call(jit, pollSlowKey, offsetInRuntimeTable(AOT::Entry::operationAOTPollSlow), OptionalIndex { site });
        jit.jump().linkTo(done, &jit);
    });
}

HintEmitter::HintEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void HintEmitter::generateLoopHint(const ValueContext& value)
{
    if (generator().validates())
        emitter<BarrierEmitter>().validateBeforeCollection(value);
    emitPoll(value);
}

void HintEmitter::emitPoll(const ValueContext& value)
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    patchpoint->isLoopPoll = true;
    Effects effects = value.effects().onAnyEdge();
    RELEASE_ASSERT(effects.mayBeTerminated && !effects.mayThrow);
    emitter<RuntimeCallEmitter>().applyEffects(patchpoint, effects, ArgumentBufferAccess::None);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(value);

    if (hasFaultingLoopPolls) {
        patchpoint->setRunsWithoutFrame(true);
        patchpoint->setGenerator([records = &m_records, site](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            emitFaultingLoad(jit, params, records, site);
        });
        return;
    }

    patchpoint->clobberLate(inputs().stubCatalog.summaryOf(pollSlowKey).clobbered);
    m_records.totals.entriesUsed.set(static_cast<unsigned>(AOT::Entry::operationAOTPollSlow));
    patchpoint->setGenerator([records = &m_records, site](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        emitCompareAndCall(jit, params, records, site);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
