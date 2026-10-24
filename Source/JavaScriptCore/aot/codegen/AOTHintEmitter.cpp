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
#include "AOTInductionVariables.h"
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

static bool readsLengthOfData(InlineBuiltin builtin)
{
    return builtin == InlineBuiltin::ArrayLength || builtin == InlineBuiltin::StringLength || builtin == InlineBuiltin::TypedArrayLength;
}

static bool isLengthOfDataWhereComputed(FunctionAnalyses& analyses, ValueID id)
{
    const Value& value = analyses.function().at(analyses.identityOf(id));
    switch (value.opcode) {
    case Opcode::GetArrayLength:
    case Opcode::GetStringLength:
    case Opcode::GetTypedArrayLength:
        return true;
    case Opcode::CallInline:
        return readsLengthOfData(value.inlineBuiltin());
    case Opcode::CallBuiltin:
        return analyses.program().builtins().at(value.overload()).builtin == Builtins::TypedArrayPrototypeLength;
    default:
        return false;
    }
}

static bool isLengthOfData(FunctionAnalyses& analyses, ValueID id)
{
    const Function& function = analyses.function();
    ValueLocation location = analyses.useLists().locationOf(analyses.identityOf(id));
    if (location.placement != ValuePlacement::Parameter)
        return isLengthOfDataWhereComputed(analyses, id);
    if (location.block == entryBlock)
        return false;
    return std::ranges::all_of(analyses.cfg().incomingEdges(location.block), [&](Edge edge) {
        if (edge.isHandlerEdge())
            return false;
        const Terminator& terminator = function.at(edge.from).terminator;
        const Target& target = terminator.targets[edge.targetIndex];
        if (location.index < target.arguments.size())
            return isLengthOfDataWhereComputed(analyses, target.arguments[location.index]);
        return terminator.kind == TerminatorKind::TryCallInline && &target == &terminator.done() && readsLengthOfData(terminator.inlineBuiltin());
    });
}

static bool countsUpToLengthOfData(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    return std::ranges::any_of(analyses.inductionVariables().of(loop).variables, [&](const InductionVariable& counter) {
        if (counter.direction != InductionDirection::Increasing || counter.step.kind != InductionStepKind::Constant)
            return false;
        const InductionTest* test = counter.boundingTest();
        return test && isLengthOfData(analyses, test->bound);
    });
}

static bool likelyPathsAlwaysComplete(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    const Function& function = analyses.function();
    const NaturalLoops& loops = analyses.naturalLoops();
    for (unsigned index = 0; index < loop.size(); ++index) {
        BlockID id = loop.at(index);
        if (loops.innerMostLoopOf(id) != &loop)
            return false;
        const Block& block = function.at(id);
        if (block.frequency == BlockFrequency::Rare)
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

bool HintEmitter::endsWithinLengthOfData(BlockID block)
{
    const NaturalLoop* loop = analyses().naturalLoops().innerMostLoopOf(block);
    return loop && countsUpToLengthOfData(analyses(), *loop) && likelyPathsAlwaysComplete(analyses(), *loop);
}

// In a function without handlers an allocation that finds no room throws by itself.
bool HintEmitter::allocatesWhereErrorsAreCaught(BlockID block)
{
    if (!analyses().cfg().hasHandlers())
        return false;
    const NaturalLoop* loop = analyses().naturalLoops().innerMostLoopOf(block);
    for (unsigned index = 0; index < loop->size(); ++index) {
        BlockID member = loop->at(index);
        for (ValueID id : m_function.at(member).values) {
            if (m_function.at(id).opcode != Opcode::LoopHint && analyses().effectsOf(id).onAnyEdge().mayCollect)
                return true;
        }
        if (analyses().effectsOfTerminator(member).onAnyEdge().mayCollect)
            return true;
    }
    return false;
}

void HintEmitter::generateLoopHint(const ValueContext& value)
{
    if (generator().validates())
        emitter<BarrierEmitter>().validateBeforeCollection(value);
    if (!endsWithinLengthOfData(value.block()) || allocatesWhereErrorsAreCaught(value.block()))
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
            emitFaultingLoad(jit, params, records, site);
    });
}

void HintEmitter::emitPoll(const ValueContext& value)
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    patchpoint->isLoopPoll = !emitter<ExceptionEmitter>().unwindsWithinFunction(value);
    Effects effects = value.effects().onAnyEdge();
    RELEASE_ASSERT(effects.mayBeTerminated && effects.mayThrow && !effects.mayThrowByRule());
    // Handling a trap stops the thread for the collector or ends it. It looks at no object of the program.
    emitter<RuntimeCallEmitter>().applyEffects(patchpoint, effects, ArgumentBufferAccess::None, false, RuntimeCallEmitter::CellConversions::None);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(value);

    if (hasFaultingLoopPolls) {
        patchpoint->setRunsWithoutFrame(true);
        patchpoint->setGenerator([records = &m_records, site](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            emitFaultingLoad(jit, params, records, site);
        });
        emitter<ExceptionEmitter>().attachLandingPad(value, patchpoint, site, true);
        return;
    }

    patchpoint->clobberLate(inputs().stubCatalog.summaryOf(pollSlowKey).clobbered);
    m_records.totals.entriesUsed.set(static_cast<unsigned>(AOT::Entry::operationAOTPollSlow));
    patchpoint->setGenerator([records = &m_records, site](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        emitCompareAndCall(jit, params, records, site);
    });
    emitter<ExceptionEmitter>().attachLandingPad(value, patchpoint, site, true);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
