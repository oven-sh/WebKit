/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHintEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTCodeRecords.h"
#include "AOTCodeSites.h"
#include "AOTCompileUnit.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTExceptionEmitter.h"
#include "AOTInstance.h"
#include "AOTPoisonedCell.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStaleAnswers.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsOperations.h"
#include "AOTThrowEmitter.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "LinkBuffer.h"
#include "StackManager.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr ptrdiff_t offsetOfTrapAwareSoftStackLimit = AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit();
static_assert(static_cast<int64_t>(StackManager::StopRequestMarkerValue) < 0);

static constexpr StubKey pollSlowKey = callOperationPreservingRegisters({ .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });

static void emitFaultingLoad(CCallHelpers& jit, const Machine::PatchParameters& params, CodeRecords* records, uint32_t site, bool leavesOwedError)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    bool isFrameless = !params.frameIsSetUp();
    CCallHelpers::Label label = jit.labelIgnoringWatchpoints();
    size_t start = jit.m_assembler.codeSize();
    ptrdiff_t offset = InstanceLayout::offsetOfLoopPollPage() + (isFrameless ? offsetOfLeafPollInLoopPollPage : 0) + (leavesOwedError ? offsetOfPollLeavingOwedErrorInLoopPollPage : 0);
#if CPU(ARM64)
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

static void emitCompareAndCall(CCallHelpers& jit, const Machine::PatchParameters& params, CodeRecords* records, uint32_t site, bool leavesOwedError)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    RELEASE_ASSERT(params.frameIsSetUp());
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

void HintEmitter::generateLoopHint(const ValueContext& value)
{
    emitPoll(value);
}

void HintEmitter::generateEndCellLifetime(const ValueContext& value)
{
    emitLifetimeEnd(value, value->cellOperand());
}

void HintEmitter::generateAskObjectAgain(const ValueContext& value)
{
    if (!emitter<AccessEmitter>().verifiesFastPathAnswers(value->key()))
        return;
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    ValueID object = value->objectOperand();
    if (FunctionID function = value->function()) {
        if (!conventions().exists(conventions().closureEntryOf(function)))
            return;
        LBasicBlock goesOn = nullptr;
        if (ValueID onlyWhere = value->onlyWhereOperand()) {
            LBasicBlock asks = newBlock();
            goesOn = newBlock();
            branch(in<Condition>(onlyWhere), { normalEdge(asks), normalEdge(goesOn) });
            m_out.appendTo(asks);
        }
        int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(value->functionIsGetter() ? KindOfAnswer::Accessor : KindOfAnswer::Value, value->asksChainAbovePrototype() ? StubThatAnswers::ChainAbovePrototype : StubThatAnswers::FunctionThatNameFinds));
        callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { conversions.box(value, get(object), typeOf(object)), boxInt32(numberOf(m_mentions.add(NumberSpace::ClosureFunction, function))), identifierNumber(value->key()), word32(kindAndStub) });
        if (goesOn) {
            m_out.jump(goesOn);
            m_out.appendTo(goesOn);
        }
        return;
    }
    ValueID answered = value->answeredOperand();
    int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Value, StubThatAnswers::StateOfWalk) | (value->answerIsMethod() ? accessorIsNoAnswerOfWalk : 0));
    callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { conversions.box(value, get(object), typeOf(object)), conversions.box(value, get(answered), typeOf(answered)), identifierNumber(value->key()), word32(kindAndStub) });
}

void HintEmitter::generateGuessFailsUnasked(const ValueContext& value)
{
    uint32_t period = build().options().speculationFailurePeriod;
    RELEASE_ASSERT(period);
    int32_t step = static_cast<int32_t>(drawBySeed(build().options().shuffleSeed, DrawnFor::StepOfGuessesThatFail, 0, 0) | 1);
    Machine::PatchOperation* drawn = m_out.patchpoint(Machine::Int64);
    drawn->effects = Machine::Effects::none();
    drawn->effects.writesLocalState = true;
    drawn->setRunsWithoutFrame(true);
    drawn->clobber(RegisterSet::macroClobberedGPRs());
    drawn->setGenerator([step](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::Address state(instanceGPR, AOT_INSTANCE_OFFSET(runtime.exceptions.stateOfGuessesThatFail));
        jit.load32(state, params[0].gpr());
        jit.add32(CCallHelpers::TrustedImm32(step), params[0].gpr());
        jit.store32(params[0].gpr(), state);
    });
    defineTest(value, belowOrEqual(Integer(drawn), integer(UINT32_MAX / period)));
}

void HintEmitter::generateBeginEffectsCheck(const ValueContext& value)
{
    emitter<RuntimeCallEmitter>().beginWatchOfWhatMayRunAgain(value);
    defineTest(value, belowOrEqual(integer(1), integer(0)));
}

void HintEmitter::generateCheckIterationRestart(const ValueContext& value)
{
    emitter<RuntimeCallEmitter>().endWatchOfWhatRunsAgain(value, definitionOf(value->beginningOperand()));
}

void HintEmitter::generateCheckInlineeFallback(const ValueContext& value)
{
    emitter<RuntimeCallEmitter>().endWatchOfWhatRunsAgain(value, definitionOf(value->beginningOperand()));
}

void HintEmitter::generateCheckReloadBypass(const ValueContext& value)
{
    emitter<RuntimeCallEmitter>().endWatchOfWhatRunsAgain(value, definitionOf(value->beginningOperand()));
}

bool HintEmitter::shouldEmitLifetimeEnds()
{
    return sourcesOfProofsCheckedUnder(build().options().testing) & bitOfSourceOfProofs(ProofSource::Lifetime);
}

void HintEmitter::emitLifetimeEnd(const OperationContext& context, ValueID cell)
{
    const Operation& made = operationAt(definitionOf(cell));
    if ((made.opcode == Opcode::NewObject && made.storage() == AllocationStorage::Frame) || emitter<ConstantEmitter>().closureLentLiesInFrame(definitionOf(cell)))
        return;
    FunctionID madeIn = generator().code().inlineFrames()[made.inlineFrame].callee;
    m_out.suspendDeclaredAccesses();
    callVoidOperation(context, RuntimeOperation::PoisonCell, { in<CellPointer>(cell), word32(static_cast<int32_t>(madeIn.index())), word32(static_cast<int32_t>(made.position)) });
    m_out.resumeDeclaredAccesses();
}

void HintEmitter::noteThatCellIsLent(const ValueContext& made, CellPointer cell)
{
    FunctionID madeIn = generator().code().inlineFrames()[made->inlineFrame].callee;
    m_out.suspendDeclaredAccesses();
    callVoidOperation(made, RuntimeOperation::PoisonCell, { cell, word32(static_cast<int32_t>(madeIn.index() | PoisonedCell::isLentForCallAhead)), word32(static_cast<int32_t>(made->position)) });
    m_out.resumeDeclaredAccesses();
}

void HintEmitter::crashIfLifetimeEnded(const OperationContext& context, CellPointer cell)
{
    if (!shouldEmitLifetimeEnds())
        return;
    Word32 type(m_out.load8ZeroExt32(m_out.address(m_heaps.collectorState, cell.value(), JSCell::typeInfoTypeOffset())));
    emitter<ThrowEmitter>().requireOrCrash(context, notEqual(type, word32(ObjectType)), boxCell(cell), ProofSource::Lifetime);
}

void HintEmitter::pollWhereNoFrameIsSetUp(const OperationContext& context)
{
    RELEASE_ASSERT(hasFaultingLoopPolls);
    Machine::PatchOperation* patchpoint = m_out.patchpoint(Machine::Void);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->setRunsWithoutFrame(true);
    patchpoint->setShowsRegistersToCollector(true);
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(context);
    patchpoint->setGenerator([records = &m_records, site](CCallHelpers& jit, const Machine::PatchParameters& params) {
        if (!params.frameIsSetUp())
            emitFaultingLoad(jit, params, records, site, false);
    });
}

void HintEmitter::emitPoll(const ValueContext& value)
{
    Machine::PatchOperation* patchpoint = m_out.patchpoint(Machine::Void);
    Effects effects = effectsOf(value);
    RELEASE_ASSERT(effects.mayBeTerminated && effects.mayThrow.isDeclaredOnlyForLackOfMemory());
    effects.writes.remove(AbstractHeapSet { AbstractHeap::RuntimeState });
    emitter<RuntimeCallEmitter>().applyEffects(patchpoint, effects, ArgumentBufferAccess::None, false, RuntimeCallEmitter::CellConversions::None);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(value);

    bool leavesOwedError = m_code.marksOf(value.id()).contains(SweepMark::LeavesOwedError);
    if (hasFaultingLoopPolls) {
        patchpoint->setRunsWithoutFrame(!isARM64());
        patchpoint->setShowsRegistersToCollector(true);
        patchpoint->setGenerator([records = &m_records, site, leavesOwedError](CCallHelpers& jit, const Machine::PatchParameters& params) {
            emitFaultingLoad(jit, params, records, site, leavesOwedError);
        });
        emitter<ExceptionEmitter>().attachLandingPad(value, patchpoint, site, true);
        return;
    }

    patchpoint->clobberLate(inputs().stubCatalog.summaryOf(pollSlowKey).clobbered);
    m_records.totals.entriesUsed.set(static_cast<unsigned>(AOT::Entry::operationAOTPollSlow));
    patchpoint->setGenerator([records = &m_records, site, leavesOwedError](CCallHelpers& jit, const Machine::PatchParameters& params) {
        emitCompareAndCall(jit, params, records, site, leavesOwedError);
    });
    emitter<ExceptionEmitter>().attachLandingPad(value, patchpoint, site, true);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
