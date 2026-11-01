/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHintEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTChooseCallingConventions.h"
#include "AOTCodeRecords.h"
#include "AOTCodeSites.h"
#include "AOTConvention.h"
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
    ptrdiff_t offset = InstanceLayout::offsetOfLoopPollPage() + (isFrameless ? offsetOfLeafPollInLoopPollPage : 0) + (leavesOwedError ? offsetOfPollLeavingOwedErrorInLoopPollPage : 0);
#if CPU(ARM64)
    // Nobody wants the word. The stub that the handler of the fault enters comes back through a scratch register.
    static_assert(sizeOfWordLoadedByLoopPoll == sizeof(uint64_t));
    RELEASE_ASSERT(ARM64Assembler::canEncodePImmOffset<64>(static_cast<int32_t>(offset)));
    jit.m_assembler.ldr<64>(ARM64Registers::zr, instanceGPR, static_cast<unsigned>(offset));
#else
    jit.load32(CCallHelpers::Address(instanceGPR, offset), jit.scratchRegister());
#endif
    RELEASE_ASSERT(jit.m_assembler.codeSize() - start == sizeOfLoopPoll);
    jit.addLinkTask([=](LinkBuffer& linkBuffer) {
        records->traps.pollSites.append(PollSite { linkBuffer.offsetOf(label), isFrameless, OptionalIndex { site } });
    });
}

static void emitCompareAndCall(CCallHelpers& jit, const B3::StackmapGenerationParams& params, CodeRecords* records, uint32_t site, bool leavesOwedError)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    RELEASE_ASSERT(params.context().frameIsSetUp);
    // On ARM64 a comparison with zero is a test of the sign bit, which sets no flags first.
    CCallHelpers::Jump isInterrupted = jit.branch64(CCallHelpers::LessThan, CCallHelpers::Address(instanceGPR, offsetOfTrapAwareSoftStackLimit), CCallHelpers::TrustedImm32(0));
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
    , m_entryParameters(m_function)
{
}

void HintEmitter::generateLoopHint(const ValueContext& value)
{
    if (generator().validates())
        emitter<BarrierEmitter>().validateBeforeCollection(value);
    emitPoll(value);
}

static constexpr OpcodeSet opcodesThatMaySuspend = [] {
    OpcodeSet result;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (maySuspend(opcode))
            result.add(opcode);
    }
    return result;
}();

// `x = null` stores to no place that the collector reads: the object stays in whatever register or stack slot held it until something else is put there.
void HintEmitter::generateReleaseHint(const ValueContext& value)
{
    // A number, a boolean or a narrow reference is nothing that the collector takes for a pointer.
    Operand released = get(value->valueOperand());
    if (!released.value || released.value->type() != B3::Int64 || released.value->hasInt64())
        return;
    // A parameter that came on the stack is still in the word that the caller stored it to. Nothing reads that word again: this is the store that the program wrote.
    // A function that was suspended runs on above another frame: the word is then somebody else's.
    bool mayBeResumed = m_function.role == FunctionRole::CoroutineBody || analyses().opcodesOfValues().intersects(opcodesThatMaySuspend);
    if (entryConventionOf(m_function) == EntryConvention::Direct && !mayBeResumed) {
        if (std::optional<uint32_t> parameter = m_entryParameters.indexOf(value->valueOperand())) {
            ArgumentLocation location = inputs().conventions.locationsOf(m_program, m_id).parameters[*parameter];
            if (location.kind == ArgumentLocation::Kind::Stack)
                m_out.store64(m_out.constInt64(0), m_out.address(m_heaps.argumentBuffer, m_out.framePointer(), offsetOfStackWordFromFrame(location.stackIndex)));
        }
    }
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    patchpoint->append(released.value, B3::ValueRep::ColdAny);
    patchpoint->effects = B3::Effects::none();
    patchpoint->effects.writesLocalState = true;
    patchpoint->setReleasesDeadValues(true);
    generator().procedure().setReleasesDeadValues(true);
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        RegisterSet registers = params.value()->registersToRelease();
        registers.exclude(params.usedRegisters());
        registers.forEach([&](Reg reg) {
            jit.move(CCallHelpers::TrustedImm32(0), reg.gpr());
        });
    });
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
    RELEASE_ASSERT(effects.mayBeTerminated && effects.mayThrow.isDeclaredOnlyForLackOfMemory());
    // Handling a trap stops the thread for the collector or ends it. It looks at no object of the program, runs none of its code and looks no name up:
    // of the state of the run time that generated code reads, a poll that comes back has changed what a collection changes.
    effects.writes.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
    emitter<RuntimeCallEmitter>().applyEffects(patchpoint, effects, ArgumentBufferAccess::None, false, RuntimeCallEmitter::CellConversions::None, RuntimeCallEmitter::SlotsOfSites::StayAsTheyAre);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(value);

    bool leavesOwedError = value->leavesOwedError();
    if (hasFaultingLoopPolls) {
        // Without the poll's need, a frame that a block in the loop needs is set up in that block wherever the way back to the head of the loop has a rare edge,
        // as the way out of an inner loop has: in every turn. On ARM64 the frame stays around the loop, as it was while the poll was a call.
        patchpoint->setRunsWithoutFrame(!isARM64());
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
