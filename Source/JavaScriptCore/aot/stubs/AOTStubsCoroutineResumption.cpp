/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCoroutines.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "AOTStubEntrypoints.h"
#include "AOTStubsCallAdapters.h"
#include "AOTStubsCalls.h"
#include "AOTStubsExceptions.h"
#include "CallFrame.h"
#include "EntryFrame.h"
#include "StackAlignment.h"
#include "StackManager.h"
#include "VM.h"
#include "VMEntryRecord.h"

namespace JSC { namespace AOT {

static constexpr int32_t generatorSlotOffset = Generator::generatorSlotInStubFrame * static_cast<int32_t>(sizeof(void*));
static_assert(generatorSlotOffset < 0 && -generatorSlotOffset <= static_cast<int32_t>(stackAlignmentBytes()));
static constexpr int32_t sizeOfStepGeneratorFrame = sizeof(CallerFrameAndPC) + stackAlignmentBytes();

CodePtr<JSEntryPtrTag> handlerOfJobEntrypoint() { return entrypointInCurrentImage(Image::FrequentStub::EnterHandlerOfJob, enterHandlerOfJob()); }
CodePtr<JSEntryPtrTag> savedFrameEntrypoint() { return entrypointInCurrentImage(Image::FrequentStub::EnterSavedFrame, enterSavedFrame()); }

static void emitUnmodifiedGeneratorCheck(StubAssembler& jit, GPRReg generator, GPRReg scratch, JumpList& otherwise)
{
    otherwise.append(jit.branchTest8(CCallHelpers::NonZero, Address(generator, Generator::offsetOfHasOtherPrototype())));
    otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(generator, JSObject::butterflyOffset())));
    jit.load32(Address(generator, JSCell::structureIDOffset()), scratch);
    Jump isAsAllPlainOnesAreMade = jit.branch32(CCallHelpers::Equal, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.plainGeneratorHeader)));
    loadStructure(jit, generator, scratch);
    jit.loadPtr(Address(scratch, Structure::prototypeOffset()), scratch);
    jit.load32(Address(scratch, JSCell::structureIDOffset()), scratch);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.emptyGeneratorPrototypeStructureID))));
    isAsAllPlainOnesAreMade.link(&jit);
}

static void emitGeneratorClassCheck(StubAssembler& jit, GPRReg generator, GPRReg scratch, GPRReg other, JumpList& otherwise)
{
    otherwise.append(jit.branchIfNotCell(generator));
    loadStructure(jit, generator, scratch);
    jit.loadPtr(Address(scratch, Structure::classInfoOffset()), scratch);
    jit.loadRuntimePointer(Entry::GeneratorClassInfo, other);
    otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, other));
}

static void emitStepCheck(StubAssembler& jit, GeneratorStepCheck check, GPRReg generator, GPRReg scratch, GPRReg other, JumpList& otherwise)
{
    switch (check) {
    case GeneratorStepCheck::IsUnmodifiedGenerator:
        emitGeneratorClassCheck(jit, generator, scratch, other, otherwise);
        emitUnmodifiedGeneratorCheck(jit, generator, scratch, otherwise);
        return;
    case GeneratorStepCheck::IsGenerator:
        emitGeneratorClassCheck(jit, generator, scratch, other, otherwise);
        return;
    case GeneratorStepCheck::IsUnmodified:
        emitUnmodifiedGeneratorCheck(jit, generator, scratch, otherwise);
        return;
    case GeneratorStepCheck::None:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitReleaseHeapFrame(StubAssembler& jit, GPRReg frame, GPRReg scratch)
{
    JumpList isLeftToCollector;
    isLeftToCollector.append(jit.branchTest8(CCallHelpers::Zero, Address(frame, SavedFrame::offsetOfNumberOfSavedRegisterPairs()), TrustedImm32(SavedFrame::liesOnHeapBit)));
    isLeftToCollector.append(jit.branch8(CCallHelpers::NotEqual, Address(frame, JSCell::cellStateOffset()), TrustedImm32(static_cast<int32_t>(CellState::DefinitelyWhite))));
    isLeftToCollector.append(jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced))));
    jit.load32(Address(frame, SavedFrame::offsetOfCapacityOfHeapFrame()), scratch);
    jit.and32(TrustedImm32(numberOfReleasedHeapFrames - 1), scratch);
    jit.storePtr(frame, BaseIndex(instanceGPR, scratch, CCallHelpers::TimesEight, AOT_INSTANCE_OFFSET(coroutines.releasedHeapFrames)));
    isLeftToCollector.link(&jit);
}

StubSignature signatureOfStepGenerator(StubKey) { return signatureWithResumeMode({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }

void generateStepGenerator(StubAssembler& jit)
{
    const auto check = static_cast<GeneratorStepCheck>(jit.key().form());
    const GPRReg generator = jit.clobberOperand(0);
    const GPRReg result = jit.result();
    const GPRReg status = jit.secondResult();
    const GPRReg saved = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    static_assert(static_cast<int32_t>(Generator::State::Running) < 0 && static_cast<int32_t>(Generator::State::Completed) < 0);
    static_assert(static_cast<int32_t>(Generator::State::SuspendedAtStart) >= 0 && static_cast<int32_t>(Generator::State::SuspendedAtYield) >= 0);
    RELEASE_ASSERT(noOverlap(generator, jit.operand(1), jit.operand(2), status, saved, scratch, instanceGPR));
    RELEASE_ASSERT(noOverlap(result, status, scratch));
    checkStubOperands(enterGeneratorFrame(), { generator, jit.operand(1), jit.operand(2) });

    JumpList isNotStepped;
    emitStepCheck(jit, check, generator, status, scratch, isNotStepped);
    jit.load32(Address(generator, Generator::offsetOfState()), status);
    isNotStepped.append(jit.branch32(CCallHelpers::LessThan, status, TrustedImm32(0)));
    jit.loadPtr(Address(generator, Generator::offsetOfSavedFrame()), saved);
    jit.load32(Address(saved, JSCell::structureIDOffset()), scratch);
    isNotStepped.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));

    JumpList hasNoLeafStep;
    jit.load64(Address(saved, SavedFrame::offsetOfAsyncContext()), scratch);
    hasNoLeafStep.append(jit.branch64(CCallHelpers::Below, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(numberTag))));
    hasNoLeafStep.append(jit.branchTest32(CCallHelpers::NonZero, jit.operand(1)));
    jit.zeroExtend32ToWord(scratch, scratch);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.code)), scratch);
#if CPU(X86_64)
    jit.call(scratch, JSEntryPtrTag);
    hasNoLeafStep.append(jit.branchTest64(CCallHelpers::Zero, result));
#else
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(stackAlignmentBytes()), sp);
    jit.storePtr(generator, Address(fp, generatorSlotOffset));
    jit.call(scratch, JSEntryPtrTag);
    Jump hasStepped = jit.branchTest64(CCallHelpers::NonZero, result);
    jit.loadPtr(Address(fp, generatorSlotOffset), generator);
    jit.emitFunctionEpilogue();
    hasNoLeafStep.append(jit.jump());
    hasStepped.link(&jit);
    jit.emitFunctionEpilogue();
#endif
    jit.move(TrustedImm32(static_cast<int32_t>(GeneratorStepStatus::Yielded)), status);
    jit.returnToCaller();

    hasNoLeafStep.link(&jit);
    jit.store32(TrustedImm32(static_cast<int32_t>(Generator::State::Running)), Address(generator, Generator::offsetOfState()));

    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(stackAlignmentBytes()), sp);
    jit.storePtr(generator, Address(fp, generatorSlotOffset));
    jit.move(saved, generator);
    jit.callStub(enterGeneratorFrame());
    jit.bindReturnLabel(StubFrameKind::GeneratorBody);
    jit.loadPtr(Address(fp, generatorSlotOffset), status);
    jit.emitFunctionEpilogue();

    jit.load32(Address(status, Generator::offsetOfState()), scratch);
    Jump didYield = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<int32_t>(Generator::State::Running)));
    jit.store32(TrustedImm32(static_cast<int32_t>(Generator::State::Completed)), Address(status, Generator::offsetOfState()));
    jit.loadPtr(Address(status, Generator::offsetOfSavedFrame()), saved);
    jit.storePtr(MacroAssembler::TrustedImmPtr(nullptr), Address(status, Generator::offsetOfSavedFrame()));
    emitReleaseHeapFrame(jit, saved, scratch);
    jit.move(TrustedImm32(static_cast<int32_t>(GeneratorStepStatus::Completed)), status);
    jit.returnToCaller();

    didYield.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(GeneratorStepStatus::Yielded)), status);
    jit.returnToCaller();

    isNotStepped.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(GeneratorStepStatus::NotStepped)), status);
    jit.move(TrustedImm64(JSValue::ValueUndefined), result);
    jit.returnToCaller();
}

StubSignature signatureOfEnterSavedFrame(StubKey) { return { }; }
StubSignature signatureOfEnterHandlerOfJob(StubKey) { return { }; }
StubSignature signatureOfHandlerOfJobReturn(StubKey) { return { }; }
StubSignature signatureOfResumeNextJob(StubKey) { return { }; }

void generateEnterSavedFrame(StubAssembler& jit)
{
    const GPRReg scratch = jit.reuseScratch(StubRegister::T11);
    emitEnterAdapter(jit);
    const GPRReg saved = jit.reuseScratch(StubRegister::A0);
    const GPRReg mode = jit.reuseScratch(StubRegister::A1);
    const GPRReg value = jit.reuseScratch(StubRegister::A2);
    const GPRReg next = jit.reuseScratch(StubRegister::T10);
    const GPRReg vm = scratch;
    const Address calleeSlot = CCallHelpers::addressFor(CallFrameSlot::callee);
    const Address endsJob(fp, AdapterFrame::offsetOfNewTarget);
    auto coroutineWord = [](ptrdiff_t offset) { return Address(instanceGPR, static_cast<int32_t>(offset)); };
    static_assert(tracksNoAsyncContext.word.base == AwaitConditionBase::VM && tracksNoAsyncContext.word.sizeInBytes == 1 && tracksNoAsyncContext.test == AwaitConditionTest::IsZero);
    RELEASE_ASSERT(noOverlap(calleeGPR, saved, mode, value, scratch));
    RELEASE_ASSERT(noOverlap(GPRInfo::returnValueGPR, next, scratch) && noOverlap(saved, mode, value, next, scratch));
    checkStubOperands(restoreSavedFrameToStack(), { saved, mode, value });

    jit.move(calleeGPR, saved);
    jit.load32(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::ResumeMode))), mode);
    jit.lshift32(mode, TrustedImm32(1), scratch);
    jit.urshift32(TrustedImm32(1), mode);
    jit.or32(scratch, mode);
    jit.and32(TrustedImm32(3), mode);
    jit.load64(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::Value))), value);
    jit.load8(coroutineWord(AOT_INSTANCE_OFFSET(coroutines.nextEntryEndsJob)), scratch);
    jit.store8(TrustedImm32(0), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.nextEntryEndsJob)));
    jit.store64(scratch, endsJob);

    Label enter = jit.label();
    jit.callStub(restoreSavedFrameToStack());
    jit.bindReturnLabel(StubFrameKind::SavedFrameAdapter);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.resumableFrame)));

    JumpList leave;
    JumpList leaveWithSentinel;
    JumpList entersDriver;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
    leave.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(vm, VM::exceptionOffset())));
    leave.append(jit.branchPtr(CCallHelpers::Below, sp, trapAwareSoftStackLimit()));
    jit.loadPtr(calleeSlot, next);
    Jump isNotAsyncFunction = jit.branch8(CCallHelpers::NotEqual, Address(next, SavedFrame::offsetOfKind()), TrustedImm32(static_cast<int32_t>(CoroutineKind::AsyncFunction)));

    jit.loadPtr(coroutineWord(AOT_INSTANCE_OFFSET(coroutines.waiterToResume)), next);
    Jump hasNoWaiter = jit.branchTestPtr(CCallHelpers::Zero, next);
    emitPutAsyncContextOfJobInPlace(jit, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.waiterAsyncContext)), scratch, value, leave);
    jit.load64(coroutineWord(AOT_INSTANCE_OFFSET(coroutines.waiterResolution)), value);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.waiterToResume)));
    jit.store64(TrustedImm32(0), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.waiterResolution)));
    jit.store64(TrustedImm32(0), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.waiterAsyncContext)));
    jit.move(next, saved);
    jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Next)), mode);
    Jump hasFrame = jit.jump();

    hasNoWaiter.link(&jit);
    leave.append(jit.branchTest64(CCallHelpers::Zero, endsJob));

    const GPRReg slot = jit.reuseScratch(StubRegister::A3);
    const GPRReg driver = jit.reuseScratch(StubRegister::T9);
    const GeneratorJobRegisters job { next, slot, scratch, driver, saved, mode, value };
    RELEASE_ASSERT(noOverlap(saved, mode, value, next, scratch, slot, driver));
    Label takeJob = jit.label();
    emitLoadHeadJob(jit, { next, slot, scratch }, leaveWithSentinel);
    Jump resumesFunction = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.resumeJobWords)));
    Label hasOtherJob = jit.label();
    Jump resumesDriver = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.driverResumeJobWord)));
    Jump finishesYield = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.yieldAwaitedJobWord)));
    Jump resumesBody = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.bodyCallJobWords)));
    JumpList callsHandler;
    callsHandler.append(jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.reactionJobWords))));
    Jump followsThenable = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.resolveThenableJobWord)));
    Jump resolvesPromise = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.resolveWithoutHandlerJobWord)));
    callsHandler.append(jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.reactionJobWords) + sizeof(uint64_t))));
    JumpList callsHandlerUnderContext;
    callsHandlerUnderContext.append(jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.reactionJobWords) + 2 * sizeof(uint64_t))));
    callsHandlerUnderContext.append(jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.reactionJobWords) + 3 * sizeof(uint64_t))));
    Jump callsFunction = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.performMicrotaskJobWord)));
    Jump takesElement = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.promiseAllResolveJobWord)));
    Jump throwsIntoFunction = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.resumeJobWords) + sizeof(uint64_t)));
    leaveWithSentinel.append(jit.branch64(CCallHelpers::NotEqual, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.bodyCallJobWords) + sizeof(uint64_t))));

    jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Throw)), mode);
    Jump hasModeOfBody = jit.jump();
    resumesBody.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Next)), mode);
    hasModeOfBody.link(&jit);
    emitTakeBodyCallJob(jit, job, leaveWithSentinel);
    entersDriver.append(jit.jump());

    resumesDriver.link(&jit);
    emitTakeDriverResumeJob(jit, job, leaveWithSentinel);
    entersDriver.append(jit.jump());

    finishesYield.link(&jit);
    JumpList hasQueuedDriver;
    emitTakeYieldAwaitedJob(jit, job, leaveWithSentinel, hasQueuedDriver);
    entersDriver.append(jit.jump());
    hasQueuedDriver.linkTo(takeJob, &jit);

    followsThenable.link(&jit);
    emitRunResolveThenableJob(jit, slot, { driver, saved, slot }, { mode, value, next, scratch }, leaveWithSentinel);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredFrame)));
    jit.jump().linkTo(takeJob, &jit);

    resolvesPromise.link(&jit);
    emitRunResolveWithoutHandlerJob(jit, slot, driver, GPRInfo::returnValueGPR, { mode, value, next, scratch }, leaveWithSentinel);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredFrame)));
    jit.jump().linkTo(takeJob, &jit);

    takesElement.link(&jit);
    emitRunPromiseAllResolveJob(jit, { next, slot, scratch }, { driver, saved, value, mode }, GPRInfo::returnValueGPR == saved ? jit.reuseScratch(StubRegister::A4) : GPRInfo::returnValueGPR, leaveWithSentinel);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredFrame)));
    jit.jump().linkTo(takeJob, &jit);

    throwsIntoFunction.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Throw)), mode);
    Jump hasModeOfFunction = jit.jump();

    JumpList leaveBehindSuspension;
    jit.align();
    jit.bindEntry(resumeNextJob());
    leaveBehindSuspension.append(jit.branchPtr(CCallHelpers::Below, sp, trapAwareSoftStackLimit()));
    leaveBehindSuspension.append(jit.branchTest64(CCallHelpers::Zero, endsJob));
    JumpList hasNoJob;
    emitLoadHeadJob(jit, { next, slot, scratch }, hasNoJob);
    Jump isOtherJob = jit.branch64(CCallHelpers::NotEqual, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.resumeJobWords)));
    resumesFunction.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Next) | resumesFunctionOfJobBit), mode);
    hasModeOfFunction.link(&jit);
    emitTakeFunctionResumeJob(jit, { next, slot, scratch }, saved, value, leaveBehindSuspension);

    hasFrame.link(&jit);
    jit.storePtr(saved, calleeSlot);
    jit.storePtr(saved, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredFrame)));
    jit.loadPtr(Address(fp), next);
    jit.storePtr(saved, Address(next, EntryFrame::vmEntryRecordOffset() + OBJECT_OFFSETOF(VMEntryRecord, m_context)));
    jit.store8(TrustedImm32(SavedFrame::isEnteredByJobLoopFlag), Address(saved, SavedFrame::offsetOfFlags()));
    jit.jump().linkTo(enter, &jit);

    isOtherJob.link(&jit);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.resumableFrame)));
    jit.jump().linkTo(hasOtherJob, &jit);
    hasNoJob.link(&jit);
    leaveBehindSuspension.link(&jit);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.resumableFrame)));
    leaveWithSentinel.append(jit.jump());

    const Address generatorArgument = CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::Generator)));
    isNotAsyncFunction.link(&jit);
    {
        const GPRReg yielded = GPRInfo::returnValueGPR;
        const GPRReg producer = slot;
        const GPRReg consumer = mode;
        const GPRReg consumerFrame = driver;
        RELEASE_ASSERT(noOverlap(yielded, producer, consumer, consumerFrame, value, next, scratch));
        leave.append(jit.branch8(CCallHelpers::NotEqual, Address(next, SavedFrame::offsetOfKind()), TrustedImm32(static_cast<int32_t>(CoroutineKind::AsyncGenerator))));
        leave.append(jit.branchTest64(CCallHelpers::Zero, endsJob));
        jit.load64(generatorArgument, producer);
        JumpList consumerHasToWait;
        emitDeliverYieldToSuspendedConsumer(jit, { yielded, producer, consumer, consumerFrame, next, scratch }, consumerHasToWait);
        jit.move(yielded, value);
        jit.move(consumerFrame, saved);
        jit.move(consumer, driver);
        jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Next)), mode);
        entersDriver.append(jit.jump());

        consumerHasToWait.link(&jit);
        Jump isNotCell = jit.branchIfNotCell(yielded);
        jit.branchIfType(yielded, SentinelType).linkTo(takeJob, &jit);
        isNotCell.link(&jit);
        emitAwaitYieldedValue(jit, { producer, yielded, next, scratch }, { mode, value, next, scratch }, leave);
        jit.jump().linkTo(takeJob, &jit);
    }

    {
        const GPRReg promise = driver;
        const GPRReg handler = mode;
        const GPRReg result = GPRInfo::returnValueGPR;
#if CPU(X86_64)
        Jump returnsFulfillment;
        Label hasResult;
#endif
        auto callHandler = [&](bool hasArgument, JumpList& doesNotGoOn) {
            jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredFrame)));
            jit.storePtr(handler, calleeSlot);
            jit.loadPtr(Address(fp), next);
            jit.storePtr(handler, Address(next, EntryFrame::vmEntryRecordOffset() + OBJECT_OFFSETOF(VMEntryRecord, m_context)));
            if (hasArgument)
                jit.shuffleRegisters<GPRReg, 2>({ handler, value }, { calleeGPR, argumentGPR(0) });
            else
                jit.move(handler, calleeGPR);
            jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
            jit.callStub(hasArgument ? callValue(1) : callValueCell(0));
            jit.bindReturnLabel(StubFrameKind::HandlerOfJob);
#if CPU(X86_64)
            if (hasArgument) {
                jit.bindEntry(handlerOfJobReturn());
                returnsFulfillment = jit.branchTest8(CCallHelpers::NonZero, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.handlerReturnsFulfillment)));
                hasResult = jit.label();
            }
#endif
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
            doesNotGoOn.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(vm, VM::exceptionOffset())));
            doesNotGoOn.append(jit.branchPtr(CCallHelpers::Below, sp, trapAwareSoftStackLimit()));
            doesNotGoOn.append(jit.branchTest64(CCallHelpers::Zero, endsJob));
        };

        callsHandlerUnderContext.link(&jit);
        emitTakeReactionJob(jit, ReactionJob::HasAsyncContext, { next, slot, scratch }, promise, handler, value, leaveWithSentinel);
        Jump hasTakenJob = jit.jump();
        callsHandler.link(&jit);
        emitTakeReactionJob(jit, ReactionJob::HasNoAsyncContext, { next, slot, scratch }, promise, handler, value, leaveWithSentinel);
        hasTakenJob.link(&jit);
        Label entersHandler = jit.label();
        jit.store64(promise, generatorArgument);
        jit.storePtr(promise, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.promiseOfEnteredHandler)));
        callHandler(true, leave);
        jit.load64(generatorArgument, promise);
        RELEASE_ASSERT(noOverlap(result, promise, slot, mode, value, next, scratch));
        emitResolvePromise(jit, promise, result, slot, { mode, value, next, scratch }, leave);
        Label hasSettledPromise = jit.label();
        jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.promiseOfEnteredHandler)));
        jit.jump().linkTo(takeJob, &jit);

#if CPU(X86_64)
        returnsFulfillment.link(&jit);
        JumpList needsPromise;
        jit.store8(TrustedImm32(0), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.handlerReturnsFulfillment)));
        needsPromise.append(jit.branchPtr(CCallHelpers::Below, sp, trapAwareSoftStackLimit()));
        needsPromise.append(jit.branchTest64(CCallHelpers::Zero, endsJob));
        jit.load64(generatorArgument, promise);
        emitQueueResolutionWithFulfillment(jit, promise, result, slot, { mode, value, next, scratch }, needsPromise);
        jit.jump().linkTo(hasSettledPromise, &jit);

        needsPromise.link(&jit);
        jit.move(result, value);
        jit.callStub(newPromise());
        jit.store64(value, Address(result, JSPromise::offsetOfSlot()));
        jit.move(TrustedImm64(static_cast<int64_t>(fulfilledPromiseBits)), scratch);
        jit.store64(scratch, Address(result, JSPromise::offsetOfPacked()));
        jit.jump().linkTo(hasResult, &jit);
#else
        UNUSED_VARIABLE(hasSettledPromise);
#endif

        callsFunction.link(&jit);
        emitTakePerformMicrotaskJob(jit, { next, slot, scratch }, handler, leaveWithSentinel);
        Label entersFunction = jit.label();
        callHandler(false, leaveWithSentinel);
        jit.jump().linkTo(takeJob, &jit);

        jit.align();
        jit.bindEntry(enterHandlerOfJob());
        emitEnterAdapter(jit);
        jit.load8(coroutineWord(AOT_INSTANCE_OFFSET(coroutines.nextEntryEndsJob)), scratch);
        jit.store8(TrustedImm32(0), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.nextEntryEndsJob)));
        jit.store64(scratch, endsJob);
        jit.load64(generatorArgument, promise);
        jit.load64(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::State))), handler);
        jit.load64(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::Value))), value);
        jit.branchIfNotCell(promise).linkTo(entersFunction, &jit);
        jit.jump().linkTo(entersHandler, &jit);
    }

    entersDriver.link(&jit);
    jit.store64(driver, generatorArgument);
    jit.storePtr(driver, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredAsyncGenerator)));
    jit.storePtr(saved, calleeSlot);
    jit.storePtr(saved, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredFrame)));
    jit.loadPtr(Address(fp), next);
    jit.storePtr(driver, Address(next, EntryFrame::vmEntryRecordOffset() + OBJECT_OFFSETOF(VMEntryRecord, m_context)));
    jit.jump().linkTo(enter, &jit);

    leaveWithSentinel.link(&jit);
    jit.loadPtr(coroutineWord(AOT_INSTANCE_OFFSET(coroutines.asyncGeneratorSentinel)), GPRInfo::returnValueGPR);
    leave.link(&jit);
    emitAdapterEpilogue(jit);
}

#if CPU(ARM64)
void emitMoveRegistersSavedInFrame(StubAssembler& jit, GPRReg word, unsigned shift, RegistersSavedInFrame direction)
{
    auto move = [&](GPRReg first, GPRReg second, int32_t offset) {
        if (direction == RegistersSavedInFrame::AreRestored)
            jit.loadPair64(fp, TrustedImm32(offset), first, second);
        else
            jit.storePair64(first, second, fp, TrustedImm32(offset));
    };
    static_assert(SavedFrame::savedRegisterBits == 2);
    static_assert(suspensionClobberedGPRs[5] == ARM64Registers::x24 && suspensionClobberedGPRs[4] == ARM64Registers::x23 && suspensionClobberedGPRs[3] == ARM64Registers::x22);
    static_assert(suspensionClobberedGPRs[2] == ARM64Registers::x21 && suspensionClobberedGPRs[1] == ARM64Registers::x20 && suspensionClobberedGPRs[0] == ARM64Registers::x19);
    JumpList isDone;
    isDone.append(jit.branchTest32(CCallHelpers::Zero, word, TrustedImm32(3 << shift)));
    move(ARM64Registers::x23, ARM64Registers::x24, -16);
    isDone.append(jit.branchTest32(CCallHelpers::Zero, word, TrustedImm32(2 << shift)));
    move(ARM64Registers::x21, ARM64Registers::x22, -32);
    isDone.append(jit.branchTest32(CCallHelpers::Zero, word, TrustedImm32(1 << shift)));
    move(ARM64Registers::x19, ARM64Registers::x20, -48);
    isDone.link(&jit);
}
#elif CPU(X86_64)
static constexpr int32_t savedPreservedRegisterBit = SavedFrame::functionSavesPreservedRegister >> SavedFrame::liveSlotMapOffsetBits;
static constexpr int32_t savedR15Bit = SavedFrame::functionSavesR15 >> SavedFrame::liveSlotMapOffsetBits;
static_assert(savedPreservedRegisterBit == 1 && savedR15Bit == 2);
static_assert(1u << SavedFrame::liveSlotMapOffsetBits == SavedFrame::functionSavesPreservedRegister && 2u << SavedFrame::liveSlotMapOffsetBits == SavedFrame::functionSavesR15);
static_assert(suspensionClobberedGPRs.size() == 1 && suspensionClobberedGPRs[0] == X86Registers::r15);

void emitMoveRegistersSavedInFrame(StubAssembler& jit, GPRReg word, unsigned shift, RegistersSavedInFrame direction)
{
    auto move = [&](GPRReg reg, int wordsBelowFramePointer) {
        const Address place(fp, -wordsBelowFramePointer * static_cast<int>(sizeof(CPURegister)));
        if (direction == RegistersSavedInFrame::AreRestored)
            jit.loadPtr(place, reg);
        else
            jit.storePtr(reg, place);
    };
    Jump doesNotSaveR15 = jit.branchTest32(CCallHelpers::Zero, word, TrustedImm32(savedR15Bit << shift));
    move(X86Registers::r15, 1);
    Jump savesOnlyR15 = jit.branchTest32(CCallHelpers::Zero, word, TrustedImm32(savedPreservedRegisterBit << shift));
    move(SavedFrame::preservedRegister, 2);
    Jump hasMovedBoth = jit.jump();
    doesNotSaveR15.link(&jit);
    Jump savesNothing = jit.branchTest32(CCallHelpers::Zero, word, TrustedImm32(savedPreservedRegisterBit << shift));
    move(SavedFrame::preservedRegister, 1);
    savesNothing.link(&jit);
    savesOnlyR15.link(&jit);
    hasMovedBoth.link(&jit);
}
#endif

struct RestoredFrame { GPRReg saved, mode, value, count, scratch; };

static void emitLoadNumbersOfWords(StubAssembler& jit, GPRReg saved, GPRReg kept, GPRReg skippedAtBottom, GPRReg inFrame)
{
    jit.load16(Address(saved, SavedFrame::offsetOfNumberOfWords()), kept);
    jit.load16(Address(saved, SavedFrame::offsetOfNumberOfWordsSkippedAtBottom()), skippedAtBottom);
    jit.load8(Address(saved, SavedFrame::offsetOfNumberOfSavedRegisterPairs()), inFrame);
#if CPU(X86_64)
    static_assert(SavedFrame::skippedAtTopFor(3) == 2 && !SavedFrame::skippedAtTopFor(2) && !SavedFrame::skippedAtTopFor(1) && !SavedFrame::skippedAtTopFor(0));
    jit.add32(TrustedImm32(1), inFrame);
    jit.urshift32(TrustedImm32(2), inFrame);
    jit.getEffectiveAddress(BaseIndex(skippedAtBottom, inFrame, CCallHelpers::TimesTwo), inFrame);
#elif CPU(ARM64)
    static_assert(SavedFrame::skippedAtTopFor(3) == 6 && SavedFrame::skippedAtTopFor(1) == 2);
    jit.addLeftShift64(skippedAtBottom, inFrame, TrustedImm32(1), inFrame);
#endif
    jit.add32(kept, inFrame);
}

#if CPU(X86_64)
static void emitCopyFrameToStack(StubAssembler& jit, const RestoredFrame& frame)
{
    const auto [saved, mode, value, count, scratch] = frame;
    const GPRReg second = jit.allocateScratch(StubRegister::A3);
    const GPRReg destination = jit.allocateScratch(StubRegister::T10);
    const FPRReg pair = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg secondPair = jit.allocateScratch(FPRInfo::fpRegT1);
    RELEASE_ASSERT(saved == X86Registers::edi && mode == X86Registers::esi && second == X86Registers::ecx);
    RELEASE_ASSERT(noOverlap(saved, mode, value, count, scratch, second, destination, instanceGPR));

    const GPRReg skippedAtBottom = second;
    emitLoadNumbersOfWords(jit, saved, count, skippedAtBottom, scratch);
    jit.lshift64(TrustedImm32(3), scratch);
    jit.addPtr(TrustedImm32(sizeOfStepGeneratorFrame - static_cast<int32_t>(sizeof(void*))), sp, destination);
    jit.subPtr(scratch, destination);
    jit.checkStackBeforeEntering(destination);

    jit.emitFunctionPrologue();
    jit.subPtr(scratch, sp);
    jit.getEffectiveAddress(BaseIndex(sp, skippedAtBottom, CCallHelpers::TimesEight), destination);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
    Jump isSmall = jit.branch32(CCallHelpers::Below, count, TrustedImm32(SavedFrame::minWordsForStringMove));
    jit.move(count, X86Registers::ecx);
    jit.move(mode, count);
    jit.move(saved, scratch);
    jit.addPtr(TrustedImm32(SavedFrame::offsetOfWords()), saved, X86Registers::esi);
    jit.move(destination, X86Registers::edi);
    jit.x86RepeatMoveString64();
    jit.move(scratch, saved);
    jit.move(count, mode);
    Jump isMoved = jit.jump();
    isSmall.link(&jit);
    auto copyPair = [&](FPRReg vector, int32_t wordsBelowCount, bool stores) {
        const int32_t offset = -(wordsBelowCount + 2) * static_cast<int32_t>(sizeof(uint64_t));
        if (stores)
            jit.storeVector(vector, BaseIndex(destination, count, CCallHelpers::TimesEight, offset));
        else
            jit.loadVector(BaseIndex(saved, count, CCallHelpers::TimesEight, SavedFrame::offsetOfWords() + offset), vector);
    };
    Jump isMultipleOfFour = jit.branchTest32(CCallHelpers::Zero, count, TrustedImm32(2));
    copyPair(pair, 0, false);
    copyPair(pair, 0, true);
    Jump isPair = jit.branchSub32(CCallHelpers::Zero, TrustedImm32(2), count);
    isMultipleOfFour.link(&jit);
    Label next = jit.label();
    copyPair(pair, 0, false);
    copyPair(secondPair, 2, false);
    copyPair(pair, 0, true);
    copyPair(secondPair, 2, true);
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(4), count).linkTo(next, &jit);
    isPair.link(&jit);
    isEmpty.link(&jit);
    isMoved.link(&jit);
}
#elif CPU(ARM64)
static void emitCopyFrameToStack(StubAssembler& jit, const RestoredFrame& frame)
{
    const auto [saved, mode, value, count, source] = frame;
    const GPRReg destination = jit.allocateScratch(StubRegister::T10);
    const GPRReg first = jit.allocateScratch(StubRegister::T12);
    const GPRReg second = jit.allocateScratch(StubRegister::T13);
    const FPRReg firstVector = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg secondVector = jit.allocateScratch(FPRInfo::fpRegT1);
    const GPRReg inFrame = first;
    const GPRReg skippedAtBottom = second;
    RELEASE_ASSERT(noOverlap(saved, mode, value, count, source, destination, first, second, instanceGPR));

    emitLoadNumbersOfWords(jit, saved, count, skippedAtBottom, inFrame);
    jit.move(sp, destination);
    jit.subLeftShift64(destination, inFrame, TrustedImm32(3), destination);
    jit.addPtr(TrustedImm32(sizeOfStepGeneratorFrame - static_cast<int32_t>(sizeof(CallerFrameAndPC))), destination, source);
    jit.checkStackBeforeEntering(source);

    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(sizeof(CallerFrameAndPC)), destination);
    jit.move(destination, sp);
    jit.addLeftShift64(destination, skippedAtBottom, TrustedImm32(3), destination);
    jit.addPtr(TrustedImm32(SavedFrame::offsetOfWords()), saved, source);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
    Jump isMultipleOfFour = jit.branchTest32(CCallHelpers::Zero, count, TrustedImm32(2));
    jit.loadPair64(CCallHelpers::PostIndexAddress(source, 2 * sizeof(uint64_t)), first, second);
    jit.storePair64(first, second, CCallHelpers::PostIndexAddress(destination, 2 * sizeof(uint64_t)));
    Jump isMoved = jit.branchSub32(CCallHelpers::Zero, TrustedImm32(2), count);
    isMultipleOfFour.link(&jit);
    Label next = jit.label();
    jit.loadPairVector(CCallHelpers::PostIndexAddress(source, 4 * sizeof(uint64_t)), firstVector, secondVector);
    jit.storePairVector(firstVector, secondVector, CCallHelpers::PostIndexAddress(destination, 4 * sizeof(uint64_t)));
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(4), count).linkTo(next, &jit);
    isMoved.link(&jit);
    isEmpty.link(&jit);
}
#endif

static void emitContinueAtSite(StubAssembler& jit, const RestoredFrame& frame)
{
    const auto [saved, mode, value, flags, resumeAddress] = frame;
    const GPRReg result = jit.reuseScratch(StubRegister::R0);
    const GPRReg resumeMode = gprOf(resumeModeRegister);
    static_assert(static_cast<int32_t>(ResumeMode::Throw) == 1 && !(static_cast<int32_t>(ResumeMode::Next) & 1));
    RELEASE_ASSERT(flags == resumeMode);
    RELEASE_ASSERT(noOverlap(result, mode, value, resumeMode, resumeAddress));

    jit.load8(Address(saved, SavedFrame::offsetOfFlags()), flags);
    jit.loadPtr(Address(saved, SavedFrame::offsetOfResumeAddress()), resumeAddress);
    jit.move(value, result);
#if CPU(X86_64)
    static_assert(!static_cast<int32_t>(AsyncGeneratorStepStatus::Result));
    Jump isOther = jit.branchTest32(CCallHelpers::Zero, mode, TrustedImm32(resumesFunctionOfJobBit));
    jit.storePtr(fp, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.resumableFrame)));
    jit.move(TrustedImm32(static_cast<int32_t>(AsyncGeneratorStepStatus::Result)), resumeMode);
    emitSiteContinuationNote(jit);
    jit.farJump(resumeAddress, JSEntryPtrTag);
    isOther.link(&jit);
#endif
    Jump isNotEnteredByJobLoop = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(SavedFrame::isEnteredByJobLoopFlag));
    Jump mustSuspendAtAwaits = jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(awaitMustSuspend)));
    jit.storePtr(fp, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.resumableFrame)));
    isNotEnteredByJobLoop.link(&jit);
    mustSuspendAtAwaits.link(&jit);
    Jump isAtYield = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(SavedFrame::isSuspendedAtYieldFlag));
    Jump throws = jit.branchTest32(CCallHelpers::NonZero, mode, TrustedImm32(static_cast<int32_t>(ResumeMode::Throw)));
    jit.urshift32(TrustedImm32(SavedFrame::stepStatusShift), resumeMode);
    emitSiteContinuationNote(jit);
    jit.farJump(resumeAddress, JSEntryPtrTag);

    isAtYield.link(&jit);
    jit.move(mode, resumeMode);
    emitSiteContinuationNote(jit);
    jit.farJump(resumeAddress, JSEntryPtrTag);

    throws.link(&jit);
    jit.store64(value, Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.exceptions.thrownValue)));
#if CPU(X86_64)
    jit.push(resumeAddress);
#elif CPU(ARM64)
    jit.move(resumeAddress, CCallHelpers::linkRegister);
#endif
    jit.jumpToStub(handleException(false));
}

static void emitContinueAtEntrypoint(StubAssembler& jit, const RestoredFrame& frame)
{
    const auto [saved, mode, value, flags, resumeAddress] = frame;
    const GPRReg frameOfBody = jit.reuseScratch(StubRegister::T10);
    static_assert(areDistinctOnEveryISA(StubRegister::A0, StubRegister::A1, StubRegister::A2, StubRegister::T9, StubRegister::T10, StubRegister::T11));
    static_assert(static_cast<int32_t>(ResumeMode::Throw) == 1 && !(static_cast<int32_t>(ResumeMode::Next) & 1));
    RELEASE_ASSERT(noOverlap(saved, mode, value, flags, resumeAddress, frameOfBody));

    jit.store32(TrustedImm32(0), Address(saved, SavedFrame::offsetOfLiveSlotMapOffsetPlusOne()));
    jit.load8(Address(saved, SavedFrame::offsetOfFlags()), flags);
    jit.loadPtr(Address(saved, SavedFrame::offsetOfResumeAddress()), resumeAddress);
    Jump isNotEnteredByJobLoop = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(SavedFrame::isEnteredByJobLoopFlag));
    Jump mustSuspendAtAwaits = jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(awaitMustSuspend)));
    jit.subPtr(sp, TrustedImm32(static_cast<int32_t>(prologueStackPointerDelta())), frameOfBody);
    jit.storePtr(frameOfBody, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.resumableFrame)));
    isNotEnteredByJobLoop.link(&jit);
    mustSuspendAtAwaits.link(&jit);
    Jump isAtYield = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(SavedFrame::isSuspendedAtYieldFlag));
    jit.and32(TrustedImm32(static_cast<int32_t>(ResumeMode::Throw)), mode);
    jit.lshift32(TrustedImm32(SavedFrame::throwsIntoAwaitShift), mode);
    jit.urshift32(TrustedImm32(SavedFrame::stepStatusShift), flags);
    jit.or32(flags, mode);
    isAtYield.link(&jit);
    jit.farJump(resumeAddress, JSEntryPtrTag);
}

StubSignature signatureOfEnterGeneratorFrame(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }

void generateEnterGeneratorFrame(StubAssembler& jit)
{
    const GPRReg saved = jit.clobberOperand(0);
    const GPRReg resumeAddress = jit.allocateScratch(StubRegister::T11);
    RELEASE_ASSERT(noOverlap(saved, jit.operand(1), jit.operand(2), resumeAddress, instanceGPR));
    checkStubOperands(restoreSavedFrameToStack(), { saved, jit.operand(1), jit.operand(2) });
    Jump isHeapFrame = jit.branchTest8(CCallHelpers::NonZero, Address(saved, SavedFrame::offsetOfNumberOfSavedRegisterPairs()), TrustedImm32(SavedFrame::liesOnHeapBit));
    jit.jumpToStub(restoreSavedFrameToStack());
    isHeapFrame.link(&jit);
    jit.loadPtr(Address(saved, SavedFrame::offsetOfResumeAddress()), resumeAddress);
    jit.farJump(resumeAddress, JSEntryPtrTag);
}

StubSignature signatureOfRestoreSavedFrameToStack(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }

void generateRestoreSavedFrameToStack(StubAssembler& jit)
{
    const RestoredFrame frame {
        .saved = jit.clobberOperand(0),
        .mode = jit.clobberOperand(1),
        .value = jit.clobberOperand(2),
        .count = jit.allocateScratch(StubRegister::T9),
        .scratch = jit.allocateScratch(StubRegister::T11),
    };
    Jump liesOnHeap = jit.branchTest8(CCallHelpers::NonZero, Address(frame.saved, SavedFrame::offsetOfNumberOfSavedRegisterPairs()), TrustedImm32(SavedFrame::liesOnHeapBit));
    emitCopyFrameToStack(jit, frame);

    const GPRReg bits = frame.scratch;
    jit.load8(Address(frame.saved, SavedFrame::offsetOfNumberOfSavedRegisterPairs()), bits);
    emitMoveRegistersSavedInFrame(jit, bits, 0, RegistersSavedInFrame::AreStored);
#if CPU(X86_64)
    Jump doesNotSavePreservedRegister = jit.branchTest32(CCallHelpers::Zero, bits, TrustedImm32(savedPreservedRegisterBit));
    jit.load64(Address(frame.saved, SavedFrame::offsetOfPreservedRegister()), SavedFrame::preservedRegister);
    doesNotSavePreservedRegister.link(&jit);
#endif

    const Address version(frame.saved, SavedFrame::offsetOfVersion());
    const Address liveSlotMapOffsetPlusOne(frame.saved, SavedFrame::offsetOfLiveSlotMapOffsetPlusOne());
#if CPU(X86_64)
    Jump collectorMayRead = jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced)));
    jit.store32(TrustedImm32(0), liveSlotMapOffsetPlusOne);
    Label isOnStack = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        collectorMayRead.link(&jit);
        jit.add32(TrustedImm32(1), version);
        jit.store32(TrustedImm32(0), liveSlotMapOffsetPlusOne);
        jit.add32(TrustedImm32(1), version);
        jit.jump().linkTo(isOnStack, &jit);
    });
#else
    jit.add32(TrustedImm32(1), version);
    jit.storeFence();
    jit.store32(TrustedImm32(0), liveSlotMapOffsetPlusOne);
    jit.storeFence();
    jit.add32(TrustedImm32(1), version);
#endif

    emitContinueAtSite(jit, frame);

    liesOnHeap.link(&jit);
    emitContinueAtEntrypoint(jit, frame);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
