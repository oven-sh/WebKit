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
#include "AOTExceptionEmitter.h"
#include "AOTFunctionAnalyses.h"
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

static void emitFaultingLoad(CCallHelpers& jit, const B3::StackmapGenerationParams& params, CodeRecords* records, uint32_t site, bool leavesOwedError)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    bool isFrameless = !params.context().frameIsSetUp;
    CCallHelpers::Label label = jit.labelIgnoringWatchpoints();
    size_t start = jit.m_assembler.codeSize();
    jit.load32(CCallHelpers::Address(instanceGPR, InstanceLayout::offsetOfLoopPollPage() + (isFrameless ? offsetOfLeafPollInLoopPollPage : 0) + (leavesOwedError ? offsetOfPollLeavingOwedErrorInLoopPollPage : 0)), jit.scratchRegister());
    RELEASE_ASSERT(jit.m_assembler.codeSize() - start == sizeOfLoopPoll);
    jit.addLinkTask([=](LinkBuffer& linkBuffer) {
        records->traps.pollSites.append(PollSite { linkBuffer.offsetOf(label), isFrameless, OptionalIndex { site } });
    });
}

static void emitCompareAndCall(CCallHelpers& jit, const B3::StackmapGenerationParams& params, CodeRecords* records, uint32_t site, bool leavesOwedError)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    RELEASE_ASSERT(params.context().frameIsSetUp);
    CCallHelpers::Jump isInterrupted = jit.branchTest64(CCallHelpers::Signed, CCallHelpers::Address(instanceGPR, offsetOfTrapAwareSoftStackLimit));
    CCallHelpers::Label done = jit.label();
    params.addLatePath([=](CCallHelpers& jit) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        isInterrupted.link(&jit);
        if (leavesOwedError)
            jit.store32(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.exceptions.pollLeavesOwedError)));
        records->stubCalls.call(jit, pollSlowKey, offsetInRuntimeTable(AOT::Entry::operationAOTPollSlow), OptionalIndex { site });
        jit.jump().linkTo(done, &jit);
    });
}

static Expected<bool> likelyPathsAlwaysComplete(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    const Function& function = analyses.function();
    const NaturalLoops& loops = analyses.naturalLoops();
    for (unsigned index = 0; index < loop.size(); ++index) {
        BlockID id = loop.at(index);
        if (loops.innerMostLoopOf(id) != &loop)
            return false;
        const Block& block = function.at(id);
        if (block.frequency.predicted() == BlockFrequency::Rare)
            continue;
        for (ValueID value : block.values) {
            if (function.at(value).opcode != Opcode::LoopHint && !analyses.effectsOf(value).onAnyEdge().alwaysCompletes())
                return false;
        }
        if (!analyses.effectsOfTerminator(id).onAnyEdge().alwaysCompletes())
            return false;
    }
    return true;
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

void HintEmitter::pollWhereNoFrameIsSetUp(const OperationContext& context)
{
    RELEASE_ASSERT(hasFaultingLoopPolls);
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->setRunsWithoutFrame(true);
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(context);
    patchpoint->setGenerator([records = &m_records, site](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        if (!params.context().frameIsSetUp)
            emitFaultingLoad(jit, params, records, site, false);
    });
}

void HintEmitter::emitPoll(const ValueContext& value)
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    patchpoint->isLoopPoll = !emitter<ExceptionEmitter>().unwindsWithinFunction(value);
    const NaturalLoop* loop = analyses().naturalLoops().innerMostLoopOf(value.block());
    patchpoint->iterationsAreShort = loop && likelyPathsAlwaysComplete(analyses(), *loop).predicted();
    Effects effects = value.effects().onAnyEdge();
    RELEASE_ASSERT(effects.mayBeTerminated && effects.mayThrow && !effects.mayThrowByRule());
    // Handling a trap stops the thread for the collector or ends it. It looks at no object of the program, runs none of its code and looks no name up:
    // of the state of the run time that generated code reads, a poll that comes back has changed what a collection changes.
    effects.writes.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
    emitter<RuntimeCallEmitter>().applyEffects(patchpoint, effects, ArgumentBufferAccess::None, false, RuntimeCallEmitter::CellConversions::None, RuntimeCallEmitter::SlotsOfSites::StayAsTheyAre);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(value);

    bool leavesOwedError = value->leavesOwedError();
    if (hasFaultingLoopPolls) {
        patchpoint->setRunsWithoutFrame(true);
        patchpoint->setGenerator([records = &m_records, site, leavesOwedError](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            emitFaultingLoad(jit, params, records, site, leavesOwedError);
        });
        emitter<ExceptionEmitter>().attachLandingPad(value, patchpoint, site, true);
        return;
    }

    patchpoint->clobberLate(inputs().stubCatalog.summaryOf(pollSlowKey).clobbered);
    m_records.totals.entriesUsed.set(static_cast<unsigned>(AOT::Entry::operationAOTPollSlow));
    patchpoint->setGenerator([records = &m_records, site, leavesOwedError](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        emitCompareAndCall(jit, params, records, site, leavesOwedError);
    });
    emitter<ExceptionEmitter>().attachLandingPad(value, patchpoint, site, true);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
