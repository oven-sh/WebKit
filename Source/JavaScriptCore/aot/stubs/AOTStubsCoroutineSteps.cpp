/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCoroutines.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTCoroutineWords.h"
#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "CallFrame.h"
#include "CompleteSubspace.h"
#include "FunctionRareData.h"
#include "InternalFunctionAllocationProfile.h"
#include "JSAsyncGenerator.h"
#include "JSCJSValueInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "MarkedSpace.h"
#include "StackAlignment.h"

namespace JSC { namespace AOT {

using Field = JSAsyncGenerator::Field;

// What the stubs know about the coroutine that iterates: these two bits and the consumerOfStep bits.
static constexpr int32_t consumerMayContinue = 1;
static constexpr int32_t consumerIsAsyncGenerator = 2;
static_assert(consumerMayContinue == SavedFrame::isEnteredByJobLoopFlag);

// The frame of RunAsyncGeneratorStep. Its caller is the frame of the consumer.
struct StepFrame {
    static constexpr int32_t offsetOfProducer = asyncGeneratorSlotInStubFrame * static_cast<int32_t>(sizeof(void*));
    static constexpr int32_t offsetOfConsumer = offsetOfProducer - 8;
    static constexpr int32_t offsetOfSiteWord = offsetOfConsumer - 8;
    static constexpr int32_t offsetOfDriver = offsetOfSiteWord - 8;
    static constexpr unsigned size = 32;
};
static_assert(StepFrame::offsetOfConsumer == consumerSlotInStubFrame * static_cast<int32_t>(sizeof(void*)) && StepFrame::offsetOfDriver == driverSlotInStubFrame * static_cast<int32_t>(sizeof(void*)));
static_assert(StepFrame::offsetOfSiteWord == siteWordSlotInStubFrame * static_cast<int32_t>(sizeof(void*)));
static_assert(StepFrame::offsetOfProducer == -8 && -StepFrame::offsetOfDriver == static_cast<int32_t>(StepFrame::size) && !(StepFrame::size % stackAlignmentBytes()));

static constexpr int32_t stateOf(JSAsyncGenerator::AsyncGeneratorSuspendReason reason) { return (1 << JSAsyncGenerator::reasonShift) | static_cast<int32_t>(reason); }
static constexpr int32_t suspendedAtYield = stateOf(JSAsyncGenerator::AsyncGeneratorSuspendReason::Yield);
static constexpr int32_t hasYieldedWithoutAwait = stateOf(JSAsyncGenerator::AsyncGeneratorSuspendReason::YieldNoAwait);
static_assert(!static_cast<int32_t>(JSAsyncGenerator::AsyncGeneratorState::Init));

static constexpr const AwaitCondition& noJobIsQueued = awaitConditionsOnState[1];
static_assert(noJobIsQueued.word.base == AwaitConditionBase::MicrotaskQueue && noJobIsQueued.test == AwaitConditionTest::EqualsOther);

static Address fieldOf(GPRReg generator, Field field) { return Address(generator, JSAsyncGenerator::offsetOfInternalField(static_cast<unsigned>(field))); }
static TrustedImm64 boxedInt32(int32_t number) { return TrustedImm64(JSValue::encode(jsNumber(number))); }

static Jump branchIfRealmWatchpointSetIsNotWatched(StubAssembler& jit, ptrdiff_t offsetInGlobalObject, GPRReg scratch)
{
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    return branchIfWatchpointSetIsNotWatched(jit, Address(scratch, static_cast<int32_t>(offsetInGlobalObject)), scratch);
}

// The iterator, the driver of the consumer, the value sent, the site word. The empty value stands for a next() without an argument.
StubSignature signatureOfStepAsyncGenerator(StubKey) { return signatureWithResumeMode({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::T9) }); }

// The generator, the settlement target of its request, the value sent, the generator's SavedFrame, the bits about the consumer, the site word.
StubSignature signatureOfRunAsyncGeneratorStep(StubKey)
{
    return signatureWithResumeMode({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3), gprOf(StubRegister::T13), gprOf(StubRegister::T9) });
}

// The iterator, the driver of the consumer, the value sent, the bits about the consumer, the site word.
StubSignature signatureOfStepAsyncIteratorInEngine(StubKey)
{
    return signatureWithResumeMode({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::T13), gprOf(StubRegister::T9) });
}

// One step of "for await" over an async generator of compiled code, as asyncIteratorNextWithDriver does it for an idle generator: the request is written as JSAsyncGenerator::enqueue writes the first one, and the body is entered from here.
void generateStepAsyncGenerator(StubAssembler& jit)
{
    using ResumeModeOfRequest = JSAsyncGenerator::AsyncGeneratorResumeMode;
    const auto form = static_cast<StepConsumer>(jit.key().form());
    const bool awaitsCall = form == StepConsumer::AsyncFunctionAwaitingCall || form == StepConsumer::AsyncGeneratorAwaitingCall;
    const StepConsumer consumerKind = form == StepConsumer::AsyncFunction || form == StepConsumer::AsyncFunctionAwaitingCall ? StepConsumer::AsyncFunction : StepConsumer::AsyncGenerator;
    const GPRReg iterator = jit.operand(0);
    const GPRReg driver = jit.clobberOperand(1);
    const GPRReg value = jit.operand(2);
    const GPRReg siteWord = jit.operand(3);
    const GPRReg saved = jit.allocateScratch(StubRegister::A3);
    const GPRReg word = jit.allocateScratch(StubRegister::A4);
    const GPRReg other = jit.allocateScratch(StubRegister::A5);
    const GPRReg consumer = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(iterator, driver, value, siteWord, saved, word, other, consumer, instanceGPR));

    JumpList mayNotContinue;
    jit.move(TrustedImm32((consumerKind == StepConsumer::AsyncGenerator ? consumerIsAsyncGenerator : 0) | (awaitsCall ? consumerOfStepAwaitsCall : 0)), consumer);
    emitBranchUnlessFrameMayContinue(jit, { InvalidGPRReg, InvalidGPRReg, word, other }, mayNotContinue);
    jit.or32(TrustedImm32(consumerMayContinue), consumer);
    mayNotContinue.link(&jit);

    JumpList stepsInEngine;
    if (awaitsCall) {
        Jump keepsNoAsyncContext = emitBranchIfTracksNoAsyncContext(jit, word);
        jit.or32(TrustedImm32(consumerOfStepAwaitsPromise), consumer);
        keepsNoAsyncContext.link(&jit);
    }
    std::optional<AssemblyHelpers::FreeListRefill> refill;
    JumpList isNotAllocatedHere;
    if (consumerKind == StepConsumer::AsyncFunction) {
        // The iterator resumes the SavedFrame of the function. A function that has not been suspended yet and may not continue gets it here: it is suspended before the step is over, whoever takes the step.
        jit.loadPtr(Address(fp, sizeof(void*)), word);
        Jump hasNoSavedFrame = jit.branchPtr(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(StubFrameKind::SavedFrameAdapter)));
        jit.loadPtr(Address(fp), word);
        jit.loadPtr(Address(word, static_cast<int>(CallFrameSlot::callee) * static_cast<int>(sizeof(Register))), driver);
        Jump hasDriver = jit.jump();

        hasNoSavedFrame.link(&jit);
        stepsInEngine.append(jit.branchTest32(CCallHelpers::NonZero, consumer, TrustedImm32(consumerMayContinue)));
        refill = emitAllocateSavedFrame(jit, CoroutineKind::AsyncFunction, siteWord, { driver, saved, word, other }, isNotAllocatedHere);
        jit.or32(TrustedImm32(consumerOfStepHasNewSavedFrame), consumer);
        hasDriver.link(&jit);
    }

    // State and ResumeMode always hold int32 numbers, so their low halves tell them apart.
    stepsInEngine.append(jit.branchIfNotType(iterator, JSAsyncGeneratorType));
    // Nothing is sent: the wrapper around a sync iterator has to know, a generator is resumed with undefined.
    Jump isSent = jit.branchIfNotEmpty(value);
    jit.move(TrustedImm64(JSValue::ValueUndefined), value);
    isSent.link(&jit);
    jit.load32(fieldOf(iterator, Field::State), word);
    Jump isSuspendedAtStart = jit.branchTest32(CCallHelpers::Zero, word);
    stepsInEngine.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(suspendedAtYield)));
    isSuspendedAtStart.link(&jit);
    stepsInEngine.append(jit.branch32(CCallHelpers::NotEqual, fieldOf(iterator, Field::ResumeMode), TrustedImm32(static_cast<int32_t>(ResumeModeOfRequest::Empty))));
    // A generator of the interpreter has no SavedFrame, and one of another realm runs with that realm's instance register.
    jit.load64(fieldOf(iterator, Field::Frame), saved);
    stepsInEngine.append(jit.branchIfNotCell(saved));
    jit.load32(Address(saved, JSCell::structureIDOffset()), word);
    stepsInEngine.append(jit.branch32(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
    // The engine remembers the generator, once per collection.
    stepsInEngine.append(branchIfBarrierIsNeeded(jit, iterator, word));
    stepsInEngine.append(branchIfRealmWatchpointSetIsNotWatched(jit, OBJECT_OFFSETOF(JSGlobalObject, m_promiseSpeciesWatchpointSet), other));

    jit.store64(value, fieldOf(iterator, Field::ResumeValue));
    jit.store64(boxedInt32(static_cast<int32_t>(ResumeModeOfRequest::Normal)), fieldOf(iterator, Field::ResumeMode));
    jit.store64(driver, fieldOf(iterator, Field::ResumePromise));
    jit.store64(boxedInt32(static_cast<int32_t>(JSAsyncGenerator::AsyncGeneratorState::Executing)), fieldOf(iterator, Field::State));
    checkStubOperands(runAsyncGeneratorStep(), { iterator, driver, value, saved, consumer, siteWord });
    jit.jumpToStub(runAsyncGeneratorStep());

    stepsInEngine.link(&jit);
    Label entersEngine = jit.label();
    checkStubOperands(stepAsyncIteratorInEngine(), { iterator, driver, value, consumer, siteWord });
    jit.jumpToStub(stepAsyncIteratorInEngine());

    if (refill) {
        emitLateRefill(jit, *refill, isNotAllocatedHere);
        isNotAllocatedHere.link(&jit);
        // The operation makes it. The register that a SavedFrame keeps may hold the promise of the function, and the operation does not see it.
        jit.or32(TrustedImm32(consumerOfStepNeedsSavedFrame), consumer);
        if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
            jit.move(SavedFrame::preservedRegister, driver);
        else
            jit.move(TrustedImm32(0), driver);
        jit.jump().linkTo(entersEngine, &jit);
    }
}

// JSAsyncGenerator::dequeue for a generator with one request, and the end of asyncGeneratorYield: the generator is suspended at its yield.
static void emitFinishOnlyRequestAtYield(StubAssembler& jit, GPRReg generator, GPRReg scratch)
{
    jit.move(TrustedImm64(JSValue::ValueUndefined), scratch);
    jit.store64(boxedInt32(static_cast<int32_t>(JSAsyncGenerator::AsyncGeneratorResumeMode::Empty)), fieldOf(generator, Field::ResumeMode));
    jit.store64(scratch, fieldOf(generator, Field::ResumeValue));
    jit.store64(scratch, fieldOf(generator, Field::ResumePromise));
    jit.store64(boxedInt32(suspendedAtYield), fieldOf(generator, Field::State));
}

// The generator has yielded a value that needs no await, no job comes first, and its request is the only one. Stores nothing.
static void emitBranchUnlessYieldEndsStep(StubAssembler& jit, GPRReg generator, GPRReg yielded, GPRReg word, GPRReg other, JumpList& otherwise)
{
    jit.load32(fieldOf(generator, Field::State), word);
    Jump needsNoAwait = jit.branch32(CCallHelpers::Equal, word, TrustedImm32(hasYieldedWithoutAwait));
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(suspendedAtYield)));
    emitBranchIfMayBeThenable(jit, yielded, word, other, otherwise);
    needsNoAwait.link(&jit);
    emitBranchUnlessHolds(jit, noJobIsQueued, { InvalidGPRReg, InvalidGPRReg, word, other }, otherwise);
    otherwise.append(jit.branch64(CCallHelpers::NotEqual, fieldOf(generator, Field::Queue), TrustedImm64(JSValue::ValueNull)));
    // The body may have given Object.prototype a "then", which the iterator result that is not made here would inherit.
    otherwise.append(branchIfRealmWatchpointSetIsNotWatched(jit, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet), other));
}

static void emitPushStepFrame(StubAssembler& jit, GPRReg producer, GPRReg consumer, GPRReg siteWord, GPRReg driver)
{
    static_assert(StepFrame::offsetOfProducer == StepFrame::offsetOfConsumer + 8 && StepFrame::offsetOfSiteWord == StepFrame::offsetOfDriver + 8);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(StepFrame::size), sp);
    jit.storePair64(consumer, producer, fp, TrustedImm32(StepFrame::offsetOfConsumer));
    jit.storePair64(driver, siteWord, fp, TrustedImm32(StepFrame::offsetOfDriver));
}

// A body has taken the word for its own frame, and an adapter that the engine went through has cleared it.
static void emitGiveBackRightToContinue(StubAssembler& jit, GPRReg consumer, GPRReg stored)
{
    jit.move(TrustedImm32(0), stored);
    Jump mayNotContinue = jit.branchTest32(CCallHelpers::Zero, consumer, TrustedImm32(consumerMayContinue));
    Jump mustSuspendAtAwaits = jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(awaitMustSuspend)));
    jit.loadPtr(Address(fp), stored);
    mayNotContinue.link(&jit);
    mustSuspendAtAwaits.link(&jit);
    jit.storePtr(stored, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.frameThatMayContinue)));
}

void generateRunAsyncGeneratorStep(StubAssembler& jit)
{
    const GPRReg firstOperand = jit.clobberOperand(0);
    const GPRReg secondOperand = jit.clobberOperand(1);
    const GPRReg sent = jit.clobberOperand(2);
    const GPRReg saved = jit.clobberOperand(3);
    const GPRReg consumer = jit.clobberOperand(4);
    const GPRReg siteWord = jit.clobberOperand(5);
    const GPRReg word = jit.allocateScratch(StubRegister::A4);
    const GPRReg other = jit.allocateScratch(StubRegister::A5);
    const GPRReg result = jit.result();
    const GPRReg status = jit.secondResult();
    // Where the operations take it.
    const GPRReg generator = secondOperand;
    RELEASE_ASSERT(firstOperand == operationArgumentGPR(0) && generator == operationArgumentGPR(1) && sent == operationArgumentGPR(2) && saved == operationArgumentGPR(3));
    RELEASE_ASSERT(noOverlap(firstOperand, secondOperand, sent, saved, consumer, siteWord, word, other, instanceGPR));
    RELEASE_ASSERT(noOverlap(result, generator, sent, saved, consumer, siteWord, word, other) && status == siteWord);

    // What willEnterAsyncGeneratorBody does: the consumer suspends as soon as the generator does, so the body may continue if the consumer may. The body is suspended at a yield.
    jit.and32(TrustedImm32(consumerMayContinue), consumer, word);
    jit.or32(TrustedImm32(SavedFrame::isSuspendedAtYieldFlag), word);
    jit.store8(word, Address(saved, SavedFrame::offsetOfFlags()));
    emitPushStepFrame(jit, firstOperand, consumer, siteWord, secondOperand);
    jit.move(saved, firstOperand);
    jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Next)), secondOperand);
    checkStubOperands(restoreSavedFrameToStack(), { firstOperand, secondOperand, sent });
    jit.callStub(restoreSavedFrameToStack());
    jit.bindReturnLabel(StubFrameKind::AsyncGeneratorBody);

    JumpList suspendsConsumer;
    JumpList finishesInEngine;
    JumpList needsOperation;
    jit.loadPair64(fp, TrustedImm32(StepFrame::offsetOfConsumer), consumer, generator);
    emitGiveBackRightToContinue(jit, consumer, word);
    // Whoever returned the sentinel has seen to the rest: a body that waits for a step of its own, or the operation that rejected the request of a body that threw.
    Jump isNotCell = jit.branchIfNotCell(result);
    Jump isNotSentinel = jit.branchIfNotType(result, SentinelType);
    suspendsConsumer.append(jit.branchTest32(CCallHelpers::Zero, consumer, TrustedImm32(consumerOfStepAwaitsPromise)));
    needsOperation.append(jit.jump());
    isNotSentinel.link(&jit);
    isNotCell.link(&jit);

    // What follows is asyncGeneratorDispatchSuspend, asyncGeneratorYield and asyncGeneratorCompleteStep for a step that tryAwaitForStepInProgress and tryFinishStepInProgress finish, with the request of the stub as the only one.
    finishesInEngine.append(jit.branchTestPtr(CCallHelpers::Zero, word));
    emitBranchUnlessYieldEndsStep(jit, generator, result, word, other, finishesInEngine);
    emitFinishOnlyRequestAtYield(jit, generator, word);
    jit.move(TrustedImm32(static_cast<int32_t>(AsyncGeneratorStepStatus::Yielded)), status);
    jit.emitFunctionEpilogue();
    jit.returnToCaller();

    // The consumer has to wait. What asyncGeneratorDispatchSuspend does with a yielded value then: it is awaited, and the job behind the await finishes the step.
    finishesInEngine.link(&jit);
    needsOperation.append(jit.branchTest32(CCallHelpers::NonZero, consumer, TrustedImm32(consumerOfStepAwaitsPromise)));
    emitAwaitYieldedValue(jit, { generator, result, word, other }, { sent, saved, word, other }, needsOperation);
    suspendsConsumer.append(jit.jump());

    needsOperation.link(&jit);
    jit.move(result, operationArgumentGPR(2));
    jit.and32(TrustedImm32(consumerMayContinue), consumer, operationArgumentGPR(3));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(Entry::operationAOTFinishAsyncIteratorStep, jit.reuseScratch(StubRegister::Operation));

    Label afterOperation = jit.label();
    Jump hasThrown = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.load32(Address(fp, StepFrame::offsetOfConsumer), consumer);
    emitGiveBackRightToContinue(jit, consumer, word);
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.stepStatus)), status);
    static_assert(!static_cast<int32_t>(AsyncGeneratorStepStatus::Result));
    suspendsConsumer.append(jit.branchTest32(CCallHelpers::Zero, status));
    jit.emitFunctionEpilogue();
    jit.returnToCaller();

    // The result register has what the consumer has to await. With the frame gone the stack is as at the entry, so the frame of the consumer is saved with the return address of this site.
    suspendsConsumer.link(&jit);
    jit.loadPair64(fp, TrustedImm32(StepFrame::offsetOfDriver), secondOperand, siteWord);
    jit.emitFunctionEpilogue();
    Jump isAsyncGenerator = jit.branchTest32(CCallHelpers::NonZero, consumer, TrustedImm32(consumerIsAsyncGenerator));
    Jump hasNewSavedFrame = jit.branchTest32(CCallHelpers::NonZero, consumer, TrustedImm32(consumerOfStepHasNewSavedFrame));
    jit.move(result, secondOperand);
    checkStubOperands(awaitBySavingFrame(), { secondOperand, siteWord });
    jit.jumpToStub(awaitBySavingFrame());

    hasNewSavedFrame.link(&jit);
    jit.move(secondOperand, sent);
    jit.move(result, secondOperand);
    checkStubOperands(awaitWithSavedFrame(), { secondOperand, sent, siteWord });
    jit.jumpToStub(awaitWithSavedFrame());

    isAsyncGenerator.link(&jit);
    checkStubOperands(suspendGenerator(GeneratorSuspension::AsyncAwait), { secondOperand, result, siteWord });
    jit.jumpToStub(suspendGenerator(GeneratorSuspension::AsyncAwait));

    hasThrown.link(&jit);
    jit.emitFunctionEpilogue();
    jit.jumpToExceptionHandler();

    // The operation finds the frame of the consumer as the caller of the stub's frame.
    jit.align();
    jit.bindEntry(stepAsyncIteratorInEngine());
    checkStubOperands(stepAsyncIteratorInEngine(), { firstOperand, secondOperand, sent, consumer, siteWord });
    emitPushStepFrame(jit, firstOperand, consumer, siteWord, secondOperand);
    jit.move(firstOperand, operationArgumentGPR(1));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(Entry::operationAOTAsyncIteratorStep, jit.reuseScratch(StubRegister::Operation));
    jit.jump().linkTo(afterOperation, &jit);
}

// An async generator of this instance that waits at an await, which is also where it waits for a step of its own. Stores nothing.
static void emitLoadFrameOfAwaitingGenerator(StubAssembler& jit, GPRReg generator, GPRReg frame, GPRReg scratch, JumpList& otherwise)
{
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, fieldOf(generator, Field::State), TrustedImm32(stateOf(JSAsyncGenerator::AsyncGeneratorSuspendReason::Await))));
    jit.load64(fieldOf(generator, Field::Frame), frame);
    otherwise.append(jit.branchIfNotCell(frame));
    jit.load32(Address(frame, JSCell::structureIDOffset()), scratch);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, Address(frame, SavedFrame::offsetOfLiveSlotMapOffsetPlusOne())));
}

static void emitStoreExecutingState(StubAssembler& jit, GPRReg generator)
{
    jit.store64(boxedInt32(static_cast<int32_t>(JSAsyncGenerator::AsyncGeneratorState::Executing)), fieldOf(generator, Field::State));
}

enum class WaitingDriver : uint8_t { IsEnteredNext, MayGetJob };

// The driver of a step is the SavedFrame of an async function or an async generator.
static void emitLoadFrameOfWaitingDriver(StubAssembler& jit, WaitingDriver what, GPRReg driver, GPRReg frame, GPRReg scratch, JumpList& otherwise)
{
    otherwise.append(jit.branchIfNotCell(driver));
    jit.load32(Address(driver, JSCell::structureIDOffset()), scratch);
    Jump isSavedFrame = jit.branch32(CCallHelpers::Equal, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader)));
    otherwise.append(jit.branchIfNotType(driver, JSAsyncGeneratorType));
    emitLoadFrameOfAwaitingGenerator(jit, driver, frame, scratch, otherwise);
    if (what == WaitingDriver::IsEnteredNext)
        emitStoreExecutingState(jit, driver);
    Jump hasFrame = jit.jump();
    isSavedFrame.link(&jit);
    jit.move(driver, frame);
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, Address(frame, SavedFrame::offsetOfLiveSlotMapOffsetPlusOne())));
    hasFrame.link(&jit);
}

static constexpr int32_t flagsOfStep(AsyncGeneratorStepStatus status) { return SavedFrame::isEnteredByJobLoopFlag | static_cast<int32_t>(status) << SavedFrame::stepStatusShift; }

void emitDeliverYieldToSuspendedConsumer(StubAssembler& jit, const YieldDeliveryRegisters& registers, JumpList& slowPath)
{
    const auto [yielded, producer, driver, consumerFrame, word, other] = registers;
    RELEASE_ASSERT(noOverlap(yielded, producer, driver, consumerFrame, word, other, instanceGPR));

    slowPath.append(jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(awaitMustSuspend))));
    emitBranchUnlessYieldEndsStep(jit, producer, yielded, word, other, slowPath);
    jit.load64(fieldOf(producer, Field::ResumePromise), driver);
    emitLoadFrameOfWaitingDriver(jit, WaitingDriver::IsEnteredNext, driver, consumerFrame, word, slowPath);

    emitFinishOnlyRequestAtYield(jit, producer, word);
    jit.store8(TrustedImm32(flagsOfStep(AsyncGeneratorStepStatus::Yielded)), Address(consumerFrame, SavedFrame::offsetOfFlags()));
}

void emitAwaitYieldedValue(StubAssembler& jit, const AwaitRegisters& registers, const JobRegisters& job, JumpList& isLeftToEngine)
{
    const GPRReg generator = registers.savedFrame;
    isLeftToEngine.append(jit.branch32(CCallHelpers::NotEqual, fieldOf(generator, Field::State), TrustedImm32(suspendedAtYield)));
    emitRunGeneratorJobWhenSettled(jit, InternalMicrotask::AsyncGeneratorYieldAwaited, registers, job, std::nullopt, isLeftToEngine);
    jit.store64(boxedInt32(stateOf(JSAsyncGenerator::AsyncGeneratorSuspendReason::Await)), fieldOf(generator, Field::State));
}

// asyncGeneratorBodyCallNormal.
void emitTakeBodyCallJob(StubAssembler& jit, const GeneratorJobRegisters& registers, JumpList& isLeftToJobLoop)
{
    const auto [queue, slot, word, driver, saved, mode, value] = registers;
    RELEASE_ASSERT(noOverlap(queue, slot, word, driver, saved, mode, value, instanceGPR));
    jit.load64(argumentOfJob(slot, 2), driver);
    emitLoadFrameOfAwaitingGenerator(jit, driver, saved, word, isLeftToJobLoop);
    emitPutAsyncContextOfJobInPlace(jit, argumentOfJob(slot, 3), word, value, isLeftToJobLoop);
    jit.load64(argumentOfJob(slot, 1), value);
    emitDequeueHeadJob(jit, queue, word);
    emitStoreExecutingState(jit, driver);
    jit.store8(TrustedImm32(SavedFrame::isEnteredByJobLoopFlag), Address(saved, SavedFrame::offsetOfFlags()));
}

// asyncGeneratorDriverResume for a step that was queued bare: the first argument says whether the producer is done, the second is the value.
void emitTakeDriverResumeJob(StubAssembler& jit, const GeneratorJobRegisters& registers, JumpList& isLeftToJobLoop)
{
    const auto [queue, slot, word, driver, saved, mode, value] = registers;
    const GPRReg flags = mode;
    static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1) && !(JSValue::ValueFalse & 1));
    static_assert(static_cast<int32_t>(AsyncGeneratorStepStatus::Completed) == static_cast<int32_t>(AsyncGeneratorStepStatus::Yielded) + 1);
    RELEASE_ASSERT(noOverlap(queue, slot, word, driver, saved, mode, value, instanceGPR));
    jit.load64(argumentOfJob(slot, 0), flags);
    jit.xor64(TrustedImm32(JSValue::ValueFalse), flags);
    isLeftToJobLoop.append(jit.branchTest64(CCallHelpers::NonZero, flags, TrustedImm32(~1)));
    jit.load64(argumentOfJob(slot, 2), driver);
    emitLoadFrameOfWaitingDriver(jit, WaitingDriver::MayGetJob, driver, saved, word, isLeftToJobLoop);
    emitPutAsyncContextOfJobInPlace(jit, argumentOfJob(slot, 3), word, value, isLeftToJobLoop);
    Jump isAsyncFunction = jit.branchPtr(CCallHelpers::Equal, driver, saved);
    emitStoreExecutingState(jit, driver);
    isAsyncFunction.link(&jit);
    jit.load64(argumentOfJob(slot, 1), value);
    emitDequeueHeadJob(jit, queue, word);
    jit.lshift32(TrustedImm32(SavedFrame::stepStatusShift), flags);
    jit.add32(TrustedImm32(flagsOfStep(AsyncGeneratorStepStatus::Yielded)), flags);
    jit.store8(flags, Address(saved, SavedFrame::offsetOfFlags()));
    jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Next)), mode);
}

// asyncGeneratorYieldAwaited for a fulfilled await, with one request, which a driver of this instance waits for.
void emitTakeYieldAwaitedJob(StubAssembler& jit, const GeneratorJobRegisters& registers, JumpList& isLeftToJobLoop, JumpList& hasQueuedDriver)
{
    const auto [queue, slot, word, driver, saved, mode, value] = registers;
    const GPRReg producer = mode;
    RELEASE_ASSERT(noOverlap(queue, slot, word, driver, saved, mode, value, instanceGPR));
    jit.load64(argumentOfJob(slot, 2), producer);
    // The number of the place where the body goes on is part of the state. Compiled code has one number for all.
    isLeftToJobLoop.append(jit.branch32(CCallHelpers::NotEqual, fieldOf(producer, Field::State), TrustedImm32(stateOf(JSAsyncGenerator::AsyncGeneratorSuspendReason::Await))));
    isLeftToJobLoop.append(jit.branch64(CCallHelpers::NotEqual, fieldOf(producer, Field::Queue), TrustedImm64(JSValue::ValueNull)));
    isLeftToJobLoop.append(branchIfRealmWatchpointSetIsNotWatched(jit, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet), word));
    jit.load64(fieldOf(producer, Field::ResumePromise), driver);
    emitLoadFrameOfWaitingDriver(jit, WaitingDriver::MayGetJob, driver, saved, word, isLeftToJobLoop);
    isLeftToJobLoop.append(jit.branchTest8(CCallHelpers::Zero, Address(queue, MicrotaskQueue::offsetOfIsScheduledToRun())));
    emitPutAsyncContextOfJobInPlace(jit, argumentOfJob(slot, 3), word, value, isLeftToJobLoop);
    jit.load64(argumentOfJob(slot, 1), value);
    emitDequeueHeadJob(jit, queue, word);
    emitFinishOnlyRequestAtYield(jit, producer, word);

    jit.load64(Address(queue, MicrotaskQueue::offsetOfStart()), word);
    Jump isNextToRun = jit.branch64(CCallHelpers::Equal, word, Address(queue, MicrotaskQueue::offsetOfEnd()));
    // The ring has the room of the job that has just been taken, and the other conditions have been tested.
    JumpList cannotHappen;
    const JobRegisters job { queue, slot, saved, word };
    emitReserveJob(jit, job, cannotHappen);
    emitFillJob(jit, { InternalMicrotask::AsyncGeneratorDriverResume, JSPromise::Status::Fulfilled, value, driver, JobAsyncContext::Current, JSValue::ValueFalse }, job);
    hasQueuedDriver.append(jit.jump());
    cannotHappen.link(&jit);
    jit.breakpoint();

    isNextToRun.link(&jit);
    Jump isAsyncFunction = jit.branchPtr(CCallHelpers::Equal, driver, saved);
    emitStoreExecutingState(jit, driver);
    isAsyncFunction.link(&jit);
    jit.store8(TrustedImm32(flagsOfStep(AsyncGeneratorStepStatus::Yielded)), Address(saved, SavedFrame::offsetOfFlags()));
    jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Next)), mode);
}

StubSignature signatureOfNewGenerator(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1) }); }

// createGenerator where InternalFunction::createSubclassStructure finds the Structure that the closure remembers.
void generateNewGenerator(StubAssembler& jit)
{
    const auto kind = static_cast<NewGeneratorKind>(jit.key().form());
    const bool isAsync = kind == NewGeneratorKind::Async;
    const GPRReg closure = jit.operand(0);
    const GPRReg generator = jit.result();
    const GPRReg allocator = jit.allocateScratch(StubRegister::T9);
    const GPRReg structure = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    constexpr int32_t rareDataTag = static_cast<int32_t>(JSFunction::rareDataTag);
    constexpr int32_t offsetOfStructureID = static_cast<int32_t>(FunctionRareData::offsetOfInternalFunctionAllocationProfile() + InternalFunctionAllocationProfile::offsetOfStructureID());
    static_assert(JSObject::butterflyOffset() == sizeof(uint64_t) && sizeof(Generator) == 4 * sizeof(uint64_t));
    static_assert(Generator::offsetOfSavedFrame() == 2 * sizeof(uint64_t) && Generator::offsetOfState() == 3 * sizeof(uint64_t) && Generator::offsetOfHasOtherPrototype() == Generator::offsetOfState() + 4);
    static_assert(!static_cast<int32_t>(Generator::State::SuspendedAtStart));
    RELEASE_ASSERT(closure == operationArgumentGPR(1));
    RELEASE_ASSERT(noOverlap(closure, generator, allocator, structure, scratch, instanceGPR));

    // closureFormOf gives every generator function a full closure, which has the layout of a JSFunction.
    JumpList slowCases;
    jit.loadPtr(Address(closure, JSFunction::offsetOfExecutableOrRareData()), structure);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, structure, TrustedImm32(rareDataTag)));
    jit.load32(Address(structure, offsetOfStructureID - rareDataTag), structure);
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, structure));
    decodeStructureID(jit, structure);
    jit.loadRuntimePointer(isAsync ? Entry::AsyncGeneratorClassInfo : Entry::GeneratorClassInfo, scratch);
    slowCases.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(structure, Structure::classInfoOffset())));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    slowCases.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(structure, Structure::realmOffset())));

    if (isAsync)
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.asyncGeneratorAllocator)), allocator);
    else {
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
        jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(sizeof(Generator)) * sizeof(Allocator)), allocator);
    }
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    const AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, generator, allocator, scratch, slowCases);
    jit.emitStoreStructureWithTypeInfo(structure, generator, scratch);
    jit.store64(TrustedImm32(0), Address(generator, JSObject::butterflyOffset()));
    if (isAsync) {
        const auto initialValues = JSAsyncGenerator::initialValues();
        jit.move(TrustedImm64(JSValue::ValueUndefined), scratch);
        for (unsigned index = 0; index < initialValues.size(); ++index) {
            const Address field(generator, JSAsyncGenerator::offsetOfInternalField(index));
            if (initialValues[index].isUndefined())
                jit.store64(scratch, field);
            else
                jit.store64(TrustedImm64(JSValue::encode(initialValues[index])), field);
        }
    } else {
        // The remembered Structure was made for a prototype that is an object.
        jit.store64(TrustedImm32(0), Address(generator, Generator::offsetOfSavedFrame()));
        jit.store64(TrustedImm32(0), Address(generator, Generator::offsetOfState()));
    }
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowCases);
    slowCases.link(&jit);
    OperationCall call {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
    };
    call.constantArguments[0] = OperationCall::ConstantArgument { operationArgumentGPR(2), static_cast<uint32_t>(isAsync) };
    jit.callOperationAndReturn(Entry::operationAOTNewGenerator, call);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
