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

static constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
static constexpr GPRReg fp = GPRInfo::callFrameRegister;

static constexpr int32_t generatorSlotOffset = Generator::generatorSlotInStubFrame * static_cast<int32_t>(sizeof(void*));
static_assert(generatorSlotOffset < 0 && -generatorSlotOffset <= static_cast<int32_t>(stackAlignmentBytes()));

CodePtr<JSEntryPtrTag> handlerOfJobEntrypoint()
{
    static void* const entrypoint = tagCodePtr<JSEntryPtrTag>(addressOfStubInCurrentImage(enterHandlerOfJob()));
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(entrypoint);
}

CodePtr<JSEntryPtrTag> savedFrameEntrypoint()
{
    static void* const entrypoint = tagCodePtr<JSEntryPtrTag>(addressOfStubInCurrentImage(enterSavedFrame()));
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(entrypoint);
}

void emitUnmodifiedGeneratorCheck(StubAssembler& jit, GPRReg generator, GPRReg scratch, JumpList& otherwise)
{
    // The stored prototype is only known to be a cell while the flag is clear, so test the flag first.
    otherwise.append(jit.branchTest8(CCallHelpers::NonZero, Address(generator, Generator::offsetOfHasOtherPrototype())));
    otherwise.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(generator, JSObject::butterflyOffset())));
    loadStructure(jit, generator, scratch);
    jit.loadPtr(Address(scratch, Structure::prototypeOffset()), scratch);
    jit.load32(Address(scratch, JSCell::structureIDOffset()), scratch);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.emptyGeneratorPrototypeStructureID))));
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

static void checkRestoreOperands(GPRReg saved, GPRReg mode, GPRReg value)
{
    const StubSignature signature = signatureOfRestoreSavedFrameToStack(restoreSavedFrameToStack());
    RELEASE_ASSERT(signature.numberOfOperands == 3);
    RELEASE_ASSERT(signature.operands[0] == Reg(saved) && signature.operands[1] == Reg(mode) && signature.operands[2] == Reg(value));
}

StubSignature signatureOfStepGenerator(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
    signature.result = StubResult::TwoGPRs;
    signature.secondResult = gprOf(resumeModeRegister);
    return signature;
}

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
    checkRestoreOperands(generator, jit.operand(1), jit.operand(2));

    JumpList isNotStepped;
    emitStepCheck(jit, check, generator, status, scratch, isNotStepped);
    jit.load32(Address(generator, Generator::offsetOfState()), status);
    isNotStepped.append(jit.branch32(CCallHelpers::LessThan, status, TrustedImm32(0)));
    // A generator of another realm runs with that realm's instance register, which only the adapter sets up.
    jit.loadPtr(Address(generator, Generator::offsetOfSavedFrame()), saved);
    jit.load32(Address(saved, JSCell::structureIDOffset()), scratch);
    isNotStepped.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
    jit.store32(TrustedImm32(static_cast<int32_t>(Generator::State::Running)), Address(generator, Generator::offsetOfState()));

    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(stackAlignmentBytes()), sp);
    jit.storePtr(generator, Address(fp, generatorSlotOffset));
    jit.move(saved, generator);
    jit.callStub(restoreSavedFrameToStack());
    jit.bindReturnLabel(StubFrameKind::GeneratorBody);
    jit.loadPtr(Address(fp, generatorSlotOffset), status);
    jit.emitFunctionEpilogue();

    // A suspension stores its state. A body that returned left the state as it was.
    jit.load32(Address(status, Generator::offsetOfState()), scratch);
    Jump didYield = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<int32_t>(Generator::State::Running)));
    jit.store32(TrustedImm32(static_cast<int32_t>(Generator::State::Completed)), Address(status, Generator::offsetOfState()));
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

// resumeModeBitsOfEngineMode as code.
static void emitConvertEngineResumeMode(StubAssembler& jit, GPRReg mode, GPRReg scratch)
{
    jit.lshift32(mode, TrustedImm32(1), scratch);
    jit.urshift32(TrustedImm32(1), mode);
    jit.or32(scratch, mode);
    jit.and32(TrustedImm32(3), mode);
}

// When a coroutine has left its frame, the adapter goes on with the frame that would be entered next anyway.
void generateEnterSavedFrame(StubAssembler& jit)
{
    const GPRReg scratch = jit.reuseScratch(StubRegister::T11);
    emitEnterAdapter(jit, scratch);
    const GPRReg saved = jit.reuseScratch(StubRegister::A0);
    const GPRReg mode = jit.reuseScratch(StubRegister::A1);
    const GPRReg value = jit.reuseScratch(StubRegister::A2);
    const GPRReg next = jit.reuseScratch(StubRegister::T10);
    const GPRReg vm = scratch;
    const Address calleeSlot = CCallHelpers::addressFor(CallFrameSlot::callee);
    const Address endsJob(fp, AdapterFrame::offsetOfNewTarget);
    auto coroutineWord = [](ptrdiff_t offset) {
        return Address(instanceGPR, static_cast<int32_t>(offset));
    };
    static_assert(tracksNoAsyncContext.word.base == AwaitConditionBase::VM && tracksNoAsyncContext.word.sizeInBytes == 1 && tracksNoAsyncContext.test == AwaitConditionTest::IsZero);
    RELEASE_ASSERT(noOverlap(calleeGPR, saved, mode, value, scratch));
    RELEASE_ASSERT(noOverlap(GPRInfo::returnValueGPR, next, scratch) && noOverlap(saved, mode, value, next, scratch));
    checkRestoreOperands(saved, mode, value);

    jit.move(calleeGPR, saved);
    jit.load32(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::ResumeMode))), mode);
    emitConvertEngineResumeMode(jit, mode, scratch);
    jit.load64(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::Value))), value);
    jit.load8(coroutineWord(AOT_INSTANCE_OFFSET(coroutines.nextEntryEndsJob)), scratch);
    jit.store8(TrustedImm32(0), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.nextEntryEndsJob)));
    jit.store64(scratch, endsJob);

    Label enter = jit.label();
    jit.callStub(restoreSavedFrameToStack());
    jit.bindReturnLabel(StubFrameKind::SavedFrameAdapter);
    // Every way out of a restored frame comes through here.
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.frameThatMayContinue)));

    // C++ reads what the body of a generator returns. Nobody reads what an async function returns, so from the first test on the result register is free.
    JumpList leave;
    // Whatever left last has seen to its own resumption.
    JumpList leaveWithSentinel;
    JumpList entersDriver;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
    leave.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(vm, VM::exceptionOffset())));
    // A trap that waits to be handled moves this limit above every stack pointer.
    leave.append(jit.branchPtr(CCallHelpers::Below, sp, Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit())));
    jit.loadPtr(calleeSlot, next);
    Jump isNotAsyncFunction = jit.branch8(CCallHelpers::NotEqual, Address(next, SavedFrame::offsetOfKind()), TrustedImm32(static_cast<int32_t>(CoroutineKind::AsyncFunction)));

    jit.loadPtr(coroutineWord(AOT_INSTANCE_OFFSET(coroutines.waiterToResume)), next);
    Jump hasNoWaiter = jit.branchTestPtr(CCallHelpers::Zero, next);
    // The waiter runs under the async context that it has captured. Where the adapter cannot put it in place, resumeRemainingWaiters does.
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
    Jump resumesDriver = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.driverResumeJobWord)));
    Jump finishesYield = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.yieldAwaitedJobWord)));
    Jump resumesBody = jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.bodyCallJobWords)));
    JumpList callsHandler;
    callsHandler.append(jit.branch64(CCallHelpers::Equal, scratch, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.reactionJobWords))));
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

    takesElement.link(&jit);
    emitRunPromiseAllResolveJob(jit, { next, slot, scratch }, { driver, saved, value, mode }, GPRInfo::returnValueGPR == saved ? jit.reuseScratch(StubRegister::A4) : GPRInfo::returnValueGPR, leaveWithSentinel);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredFrame)));
    jit.jump().linkTo(takeJob, &jit);

    throwsIntoFunction.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Throw)), mode);
    Jump hasModeOfFunction = jit.jump();
    resumesFunction.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(ResumeMode::Next)), mode);
    hasModeOfFunction.link(&jit);
    emitTakeFunctionResumeJob(jit, { next, slot, scratch }, saved, value, leaveWithSentinel);

    // The stack walkers, SavedFrame::forFrame and the collector find the running SavedFrame in the callee slot.
    hasFrame.link(&jit);
    jit.storePtr(saved, calleeSlot);
    jit.storePtr(saved, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredFrame)));
    // A stack trace goes on from the context of the entry to the functions that await it. The adapter is entered from the entry frame.
    jit.loadPtr(Address(fp), next);
    jit.storePtr(saved, Address(next, EntryFrame::vmEntryRecordOffset() + OBJECT_OFFSETOF(VMEntryRecord, m_context)));
    jit.store8(TrustedImm32(SavedFrame::isEnteredByJobLoopFlag), Address(saved, SavedFrame::offsetOfFlags()));
    jit.jump().linkTo(enter, &jit);

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

    // What runInternalMicrotask does for a PromiseReactionJob. A throw leaves through the entry frame, and whoever entered the adapter rejects the promise.
    {
        const GPRReg promise = driver;
        const GPRReg handler = mode;
        const GPRReg result = GPRInfo::returnValueGPR;
        callsHandlerUnderContext.link(&jit);
        emitTakeReactionJob(jit, ReactionJob::HasAsyncContext, { next, slot, scratch }, promise, handler, value, leaveWithSentinel);
        Jump hasTakenJob = jit.jump();
        callsHandler.link(&jit);
        emitTakeReactionJob(jit, ReactionJob::HasNoAsyncContext, { next, slot, scratch }, promise, handler, value, leaveWithSentinel);
        hasTakenJob.link(&jit);
        Label entersHandler = jit.label();
        jit.store64(promise, generatorArgument);
        jit.storePtr(promise, coroutineWord(AOT_INSTANCE_OFFSET(coroutines.promiseOfEnteredHandler)));
        jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredFrame)));
        jit.storePtr(handler, calleeSlot);
        jit.loadPtr(Address(fp), next);
        jit.storePtr(handler, Address(next, EntryFrame::vmEntryRecordOffset() + OBJECT_OFFSETOF(VMEntryRecord, m_context)));
        jit.shuffleRegisters<GPRReg, 2>({ handler, value }, { calleeGPR, argumentGPR(0) });
        jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
        jit.callStub(callValue(1));
        jit.bindReturnLabel(StubFrameKind::HandlerOfJob);

        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
        leave.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(vm, VM::exceptionOffset())));
        leave.append(jit.branchPtr(CCallHelpers::Below, sp, Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit())));
        leave.append(jit.branchTest64(CCallHelpers::Zero, endsJob));
        jit.load64(generatorArgument, promise);
        RELEASE_ASSERT(noOverlap(result, promise, slot, mode, value, next, scratch));
        emitFulfillPromise(jit, promise, result, slot, { mode, value, next, scratch }, leave);
        jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.promiseOfEnteredHandler)));
        jit.jump().linkTo(takeJob, &jit);

        // What it does for a BunPerformMicrotaskJob. Whoever entered the adapter reports what the function throws.
        callsFunction.link(&jit);
        emitTakePerformMicrotaskJob(jit, { next, slot, scratch }, handler, leaveWithSentinel);
        Label entersFunction = jit.label();
        jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), coroutineWord(AOT_INSTANCE_OFFSET(coroutines.enteredFrame)));
        jit.storePtr(handler, calleeSlot);
        jit.loadPtr(Address(fp), next);
        jit.storePtr(handler, Address(next, EntryFrame::vmEntryRecordOffset() + OBJECT_OFFSETOF(VMEntryRecord, m_context)));
        jit.move(handler, calleeGPR);
        jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
        jit.callStub(callValueCell(0));
        jit.bindReturnLabel(StubFrameKind::HandlerOfJob);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
        leaveWithSentinel.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(vm, VM::exceptionOffset())));
        leaveWithSentinel.append(jit.branchPtr(CCallHelpers::Below, sp, Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit())));
        leaveWithSentinel.append(jit.branchTest64(CCallHelpers::Zero, endsJob));
        jit.jump().linkTo(takeJob, &jit);

        // The arguments lie where the adapter finds those of a coroutine: the promise, the handler, what the handler is called with. Without a promise the function is called without an argument.
        jit.align();
        jit.bindEntry(enterHandlerOfJob());
        emitEnterAdapter(jit, scratch);
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
void emitMovePairsSavedInFrame(StubAssembler& jit, GPRReg word, unsigned shift, PairsSavedInFrame direction)
{
    auto move = [&](GPRReg first, GPRReg second, int32_t offset) {
        if (direction == PairsSavedInFrame::AreRestored)
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
static_assert(suspensionClobberedGPRs.size() == 1 && suspensionClobberedGPRs[0] == X86Registers::r15);

static void emitStoreRegistersSavedInFrame(StubAssembler& jit, GPRReg bits)
{
    Jump doesNotSaveR15 = jit.branchTest32(CCallHelpers::Zero, bits, TrustedImm32(savedR15Bit));
    jit.storePtr(X86Registers::r15, Address(fp, -static_cast<int>(sizeof(CPURegister))));
    Jump savesOnlyR15 = jit.branchTest32(CCallHelpers::Zero, bits, TrustedImm32(savedPreservedRegisterBit));
    jit.storePtr(SavedFrame::preservedRegister, Address(fp, -2 * static_cast<int>(sizeof(CPURegister))));
    Jump hasStoredBoth = jit.jump();
    doesNotSaveR15.link(&jit);
    Jump savesNothing = jit.branchTest32(CCallHelpers::Zero, bits, TrustedImm32(savedPreservedRegisterBit));
    jit.storePtr(SavedFrame::preservedRegister, Address(fp, -static_cast<int>(sizeof(CPURegister))));
    savesNothing.link(&jit);
    savesOnlyR15.link(&jit);
    hasStoredBoth.link(&jit);
}
#endif

struct RestoredFrame {
    GPRReg saved;
    GPRReg mode;
    GPRReg value;
    GPRReg count;
    GPRReg scratch;
};

static Address softStackLimit()
{
    return Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit());
}

// The frame has the words kept, the words skipped above them and the words skipped below them. The three numbers lie in one word of the cell.
static void emitLoadNumbersOfWords(StubAssembler& jit, GPRReg saved, GPRReg kept, GPRReg skippedAtBottom, GPRReg inFrame)
{
    static_assert(SavedFrame::offsetOfNumberOfWordsSkippedAtTop() == SavedFrame::offsetOfNumberOfWords() + 2 && SavedFrame::offsetOfNumberOfWordsSkippedAtBottom() == SavedFrame::offsetOfNumberOfWords() + 3);
    jit.load32(Address(saved, SavedFrame::offsetOfNumberOfWords()), kept);
    jit.urshift32(kept, TrustedImm32(24), skippedAtBottom);
    extractBits(jit, kept, 16, 8, inFrame);
    jit.and32(TrustedImm32(UINT16_MAX), kept);
    jit.add32(skippedAtBottom, inFrame);
    jit.add32(kept, inFrame);
}

// Checks the stack, pushes the frame record and copies the words kept to their place below it.
#if CPU(X86_64)
static void emitCopyFrameToStack(StubAssembler& jit, const RestoredFrame& frame)
{
    const auto [saved, mode, value, count, scratch] = frame;
    const GPRReg second = jit.allocateScratch(StubRegister::A3);
    const GPRReg destination = jit.allocateScratch(StubRegister::T10);
    RELEASE_ASSERT(saved == X86Registers::edi && mode == X86Registers::esi && second == X86Registers::ecx);
    RELEASE_ASSERT(noOverlap(saved, mode, value, count, scratch, second, destination, instanceGPR));

    emitLoadNumbersOfWords(jit, saved, count, destination, scratch);
    jit.lshift64(TrustedImm32(3), scratch);
    jit.subPtr(sp, scratch, second);
    jit.subPtr(TrustedImm32(maxStackUsedWithoutCheck), second);
    jit.linkToStub(jit.branchPtr(CCallHelpers::Below, second, softStackLimit()), throwStackOverflow());

    jit.emitFunctionPrologue();
    jit.subPtr(scratch, sp);
    jit.getEffectiveAddress(BaseIndex(sp, destination, CCallHelpers::TimesEight), destination);
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
    Label next = jit.label();
    jit.load64(BaseIndex(saved, count, CCallHelpers::TimesEight, SavedFrame::offsetOfWords() - 8), scratch);
    jit.load64(BaseIndex(saved, count, CCallHelpers::TimesEight, SavedFrame::offsetOfWords() - 16), second);
    jit.store64(scratch, BaseIndex(destination, count, CCallHelpers::TimesEight, -8));
    jit.store64(second, BaseIndex(destination, count, CCallHelpers::TimesEight, -16));
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(2), count).linkTo(next, &jit);
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
    jit.subPtr(destination, TrustedImm32(maxStackUsedWithoutCheck), source);
    jit.linkToStub(jit.branchPtr(CCallHelpers::Below, source, softStackLimit()), throwStackOverflow());

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

// Leaving the function, and the unwinder when it passes the frame, reload callee saves from the frame's save area.
static void emitStoreResumerCalleeSaves(StubAssembler& jit, const RestoredFrame& frame)
{
    const GPRReg bits = frame.scratch;
    jit.load8(Address(frame.saved, SavedFrame::offsetOfNumberOfSavedRegisterPairs()), bits);
#if CPU(X86_64)
    emitStoreRegistersSavedInFrame(jit, bits);
    // A function that does not save the register leaves with it as it is, so it has to stay the resumer's.
    Jump doesNotSavePreservedRegister = jit.branchTest32(CCallHelpers::Zero, bits, TrustedImm32(savedPreservedRegisterBit));
    jit.load64(Address(frame.saved, SavedFrame::offsetOfPreservedRegister()), SavedFrame::preservedRegister);
    doesNotSavePreservedRegister.link(&jit);
#elif CPU(ARM64)
    emitMovePairsSavedInFrame(jit, bits, 0, PairsSavedInFrame::AreStored);
#endif
}

// From here on the words of the SavedFrame are stale, so it lists none of them. Until the copy is done a marker may read them.
static void emitMarkAsOnStack(StubAssembler& jit, GPRReg saved)
{
    const Address version(saved, SavedFrame::offsetOfVersion());
    jit.add32(TrustedImm32(1), version);
    jit.storeFence();
    jit.store32(TrustedImm32(0), Address(saved, SavedFrame::offsetOfLiveSlotMapOffsetPlusOne()));
    jit.storeFence();
    jit.add32(TrustedImm32(1), version);
}

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
    // Any other frame leaves the word alone, so a generator that is stepped by the frame that may continue does not take the right away.
    Jump isNotEnteredByJobLoop = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(SavedFrame::isEnteredByJobLoopFlag));
    Jump mustSuspendAtAwaits = jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(awaitMustSuspend)));
    jit.storePtr(fp, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.frameThatMayContinue)));
    isNotEnteredByJobLoop.link(&jit);
    mustSuspendAtAwaits.link(&jit);
    Jump isAtYield = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(SavedFrame::isSuspendedAtYieldFlag));
    Jump throws = jit.branchTest32(CCallHelpers::NonZero, mode, TrustedImm32(static_cast<int32_t>(ResumeMode::Throw)));
    // The site of a StepAsyncGenerator reads the status of the step. The site of an await ignores the register.
    jit.urshift32(TrustedImm32(SavedFrame::stepStatusShift), resumeMode);
    // The tag makes the assembler record that compiled code runs: the clobbers and effects of the stubs that call this one are computed from that.
    jit.farJump(resumeAddress, JSEntryPtrTag);

    isAtYield.link(&jit);
    jit.move(mode, resumeMode);
    jit.farJump(resumeAddress, JSEntryPtrTag);

    // The throw is raised at the call site of the suspension, whose landing pad is the handler of its block.
    throws.link(&jit);
    jit.store64(value, Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.exceptions.thrownValue)));
#if CPU(X86_64)
    jit.push(resumeAddress);
#elif CPU(ARM64)
    jit.move(resumeAddress, CCallHelpers::linkRegister);
#endif
    jit.jumpToStub(handleException(false));
}

StubSignature signatureOfRestoreSavedFrameToStack(StubKey)
{
    return StubSignature::withoutResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

// The frame record is pushed here, so the frame's return address is the label after the call of this stub, which tells who resumed the frame.
void generateRestoreSavedFrameToStack(StubAssembler& jit)
{
    const RestoredFrame frame {
        .saved = jit.clobberOperand(0),
        .mode = jit.clobberOperand(1),
        .value = jit.clobberOperand(2),
        .count = jit.allocateScratch(StubRegister::T9),
        .scratch = jit.allocateScratch(StubRegister::T11),
    };
    emitCopyFrameToStack(jit, frame);
    emitStoreResumerCalleeSaves(jit, frame);
    emitMarkAsOnStack(jit, frame.saved);
    emitContinueAtSite(jit, frame);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
