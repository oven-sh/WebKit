/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCoroutines.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "AOTStubsBarriers.h"
#include "CallFrame.h"
#include "JSGlobalObject.h"
#include "JSPromise.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "MicrotaskQueue.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

StubSignature signatureOfNewPromise(StubKey)
{
    return StubSignature::withGPRResult({ });
}

void generateNewPromise(StubAssembler& jit)
{
    const GPRReg promise = jit.result();
    const GPRReg allocator = jit.allocateScratch(StubRegister::T9);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    static_assert(JSPromise::offsetOfSlot() == JSPromise::offsetOfPacked() + static_cast<ptrdiff_t>(sizeof(uint64_t)));
    static_assert(JSObject::butterflyOffset() == sizeof(uint64_t) && !JSValue::ValueEmpty);
    JumpList slowCases;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Promise)].allocator)), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    auto refill = allocateCellWithLateRefill(jit, promise, allocator, scratch, slowCases);
    jit.load64(Address(instanceGPR, offsetOfPromiseHeader), scratch);
#if CPU(ARM64)
    jit.storePair64(scratch, ARM64Registers::zr, promise, TrustedImm32(0));
    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, promise, TrustedImm32(JSPromise::offsetOfPacked()));
#else
    jit.store64(scratch, Address(promise));
    jit.store64(TrustedImm32(0), Address(promise, JSObject::butterflyOffset()));
    jit.store64(TrustedImm32(0), Address(promise, JSPromise::offsetOfPacked()));
    jit.store64(TrustedImm32(0), Address(promise, JSPromise::offsetOfSlot()));
#endif
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowCases);
    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewPromise, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false });
}

// The promise, the handler where the operation takes it, the derived promise.
StubSignature signatureOfPromiseThen(StubKey key)
{
    const bool isOnFulfilled = static_cast<PromiseHandler>(key.form()) == PromiseHandler::OnFulfilled;
    return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(isOnFulfilled ? StubRegister::A2 : StubRegister::A3), gprOf(StubRegister::A4) });
}

// JSPromise::performPromiseThen for a pending promise without a reaction, while no async context is to be captured: setInlineHandlerReaction.
void generatePromiseThen(StubAssembler& jit)
{
    const bool isOnFulfilled = static_cast<PromiseHandler>(jit.key().form()) == PromiseHandler::OnFulfilled;
    const GPRReg promise = jit.operand(0);
    const GPRReg handler = jit.operand(1);
    const GPRReg derived = jit.operand(2);
    const GPRReg otherHandler = jit.allocateScratch(isOnFulfilled ? StubRegister::A3 : StubRegister::A2);
    const GPRReg word = jit.allocateScratch(StubRegister::A5);
    const GPRReg other = otherHandler;
    constexpr auto kindOf = [](bool isOnFulfilled) {
        return isOnFulfilled ? JSPromise::InlineReactionKind::FulfillHandler : JSPromise::InlineReactionKind::RejectHandler;
    };
    const uint64_t reactionBits = (static_cast<uint64_t>(JSPromise::isHandledFlag) | static_cast<uint64_t>(kindOf(isOnFulfilled)) << JSPromise::inlineReactionKindShift) << promiseFlagsShift;
    RELEASE_ASSERT(promise == operationArgumentGPR(1) && derived == operationArgumentGPR(4) && handler == operationArgumentGPR(isOnFulfilled ? 2 : 3));
    RELEASE_ASSERT(noOverlap(promise, handler, derived, otherHandler, word, instanceGPR));

    JumpList needsOperation;
    emitBranchUnlessHolds(jit, awaitedTakesInlineReaction, { InvalidGPRReg, promise, word, other }, needsOperation);
    Jump capturesNothing = emitBranchIfTracksNoAsyncContext(jit, other);
    emitLoadCurrentAsyncContext(jit, other);
    needsOperation.append(jit.branch64(CCallHelpers::NotEqual, other, TrustedImm64(JSValue::ValueUndefined)));
    capturesNothing.link(&jit);
    needsOperation.append(branchIfBarrierIsNeeded(jit, promise, other));
    jit.or64(derived, word);
    jit.or64(TrustedImm64(static_cast<int64_t>(reactionBits)), word);
    jit.store64(handler, Address(promise, JSPromise::offsetOfSlot()));
    jit.store64(word, Address(promise, JSPromise::offsetOfPacked()));
    jit.returnToCaller();

    needsOperation.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), otherHandler);
    jit.callOperationAndReturn(Entry::operationAOTPromiseThen, { .returns = OperationCall::Returns::Void });
}

static void emitNonThenableCheck(StubAssembler& jit, GPRReg object, GPRReg structure, GPRReg scratch, JumpList& otherwise)
{
    loadStructure(jit, object, structure);
    jit.load32(Address(structure, Structure::bitFieldOffset()), scratch);
    jit.and32(TrustedImm32(Structure::s_definitelyNonThenableStateBits), scratch);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<int32_t>(Structure::DefinitelyNonThenableState::NonThenable) << Structure::s_definitelyNonThenableStateShift)));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(structure, Structure::realmOffset())));
    otherwise.append(branchIfWatchpointSetIsNotWatched(jit, Address(scratch, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet)), scratch));
}

void emitBranchIfMayBeThenable(StubAssembler& jit, GPRReg value, GPRReg structure, GPRReg scratch, JumpList& mayBeThenable)
{
    Jump isNotCell = jit.branchIfNotCell(value);
    Jump isNotObject = jit.branchIfNotObject(value);
    emitNonThenableCheck(jit, value, structure, scratch, mayBeThenable);
    isNotObject.link(&jit);
    isNotCell.link(&jit);
}

void emitFulfillPromise(StubAssembler& jit, GPRReg promise, GPRReg value, GPRReg other, const JobRegisters& job, JumpList& needsEngine)
{
    const GPRReg word = job.scratch;
    const GPRReg task = job.queue;
    constexpr auto flagsOfReaction = [](JSPromise::InlineReactionKind kind) {
        return static_cast<int32_t>(JSPromise::isHandledFlag | static_cast<uint16_t>(kind) << JSPromise::inlineReactionKindShift);
    };
    constexpr uint64_t fulfilledBit = static_cast<uint64_t>(JSPromise::Status::Fulfilled) << promiseFlagsShift;
    constexpr uint64_t firstFunctionIsCalledBit = static_cast<uint64_t>(JSPromise::isFirstResolvingFunctionCalledFlag) << promiseFlagsShift;
    constexpr unsigned numberOfFlagBits = 64 - promiseFlagsShift;
    static_assert(static_cast<uint64_t>(JSPromise::inlineReactionAsyncContextFlag) << promiseFlagsShift == 1ull << 62);
    RELEASE_ASSERT(noOverlap(promise, value, other, job.queue, job.slot, job.next, job.scratch, instanceGPR));

    auto loadCellOfReaction = [&] {
        jit.lshift64(word, TrustedImm32(numberOfFlagBits), other);
        jit.urshift64(TrustedImm32(numberOfFlagBits), other);
    };
    // The flags that a settled promise keeps, with the new state. Then the two words.
    auto settle = [&] {
        jit.move(TrustedImm64(static_cast<int64_t>(firstFunctionIsCalledBit)), other);
        jit.and64(other, word);
        jit.or64(TrustedImm64(static_cast<int64_t>(promiseIsHandledBits | fulfilledBit)), word);
        jit.store64(value, Address(promise, JSPromise::offsetOfSlot()));
        jit.store64(word, Address(promise, JSPromise::offsetOfPacked()));
    };

    emitBranchIfMayBeThenable(jit, value, word, other, needsEngine);
    needsEngine.append(branchIfBarrierIsNeeded(jit, promise, word));
    jit.load64(Address(promise, JSPromise::offsetOfPacked()), word);
    jit.move(TrustedImm64(static_cast<int64_t>(~pendingPromiseWithoutReactionBits)), other);
    Jump hasReaction = jit.branchTest64(CCallHelpers::NonZero, word, other);
    jit.or64(TrustedImm64(static_cast<int64_t>(fulfilledBit)), word);
    jit.store64(value, Address(promise, JSPromise::offsetOfSlot()));
    jit.store64(word, Address(promise, JSPromise::offsetOfPacked()));
    JumpList isFulfilled;
    isFulfilled.append(jit.jump());

    // The flags say that the promise is pending and handled and which reaction it has. The resolving functions may have been made and called or not.
    hasReaction.link(&jit);
    jit.urshift64(word, TrustedImm32(promiseFlagsShift), other);
    jit.and32(TrustedImm32(~static_cast<int32_t>(JSPromise::isFirstResolvingFunctionCalledFlag)), other);
    Jump hasNoHandler = jit.branch32(CCallHelpers::NotEqual, other, TrustedImm32(flagsOfReaction(JSPromise::InlineReactionKind::FulfillHandler)));

    // settleInlineHandler: queueMicrotask(vm, PromiseReactionJob, Fulfilled, the promise that the handler's result settles, the handler, the value).
    emitReserveJob(jit, job, needsEngine);
    jit.load64(Address(promise, JSPromise::offsetOfPacked()), word);
    loadCellOfReaction();
    jit.store64(other, argumentOfJob(job.slot, 0));
    jit.load64(Address(promise, JSPromise::offsetOfSlot()), other);
    jit.store64(other, argumentOfJob(job.slot, 1));
    jit.store64(value, argumentOfJob(job.slot, 2));
    jit.store64(TrustedImm32(0), argumentOfJob(job.slot, 3));
    settle();
    emitPublishJob(jit, InternalMicrotask::PromiseReactionJob, JSPromise::Status::Fulfilled, job);
    isFulfilled.append(jit.jump());

    // settleInlineInternalMicrotask for the three tasks that resume compiled code: queueMicrotask(vm, task, Fulfilled, undefined, the value, the coroutine, the async context it captured).
    hasNoHandler.link(&jit);
    jit.and32(TrustedImm32(JSPromise::inlineReactionMicrotaskMask), other, task);
    jit.and32(TrustedImm32(~static_cast<int32_t>(JSPromise::inlineReactionMicrotaskMask | JSPromise::inlineReactionAsyncContextFlag)), other);
    needsEngine.append(jit.branch32(CCallHelpers::NotEqual, other, TrustedImm32(flagsOfReaction(JSPromise::InlineReactionKind::InternalMicrotask))));
    JumpList resumesCompiledCode;
    for (InternalMicrotask resuming : { InternalMicrotask::CompiledAsyncFunctionResume, InternalMicrotask::AsyncGeneratorBodyCallNormal })
        resumesCompiledCode.append(jit.branch32(CCallHelpers::Equal, task, TrustedImm32(static_cast<int32_t>(resuming) << JSPromise::inlineReactionMicrotaskShift)));
    needsEngine.append(jit.branch32(CCallHelpers::NotEqual, task, TrustedImm32(static_cast<int32_t>(InternalMicrotask::AsyncGeneratorYieldAwaited) << JSPromise::inlineReactionMicrotaskShift)));
    resumesCompiledCode.link(&jit);
    // Without the flag the cell would be the job's first argument. These tasks have none.
    loadCellOfReaction();
    Jump hasNoCell = jit.branchTest64(CCallHelpers::Zero, other);
    jit.lshift64(word, TrustedImm32(1), other);
    needsEngine.append(jit.branchTest64(CCallHelpers::PositiveOrZero, other));
    hasNoCell.link(&jit);

    emitReserveJob(jit, job, needsEngine);
    jit.load64(Address(promise, JSPromise::offsetOfPacked()), word);
    loadCellOfReaction();
    jit.store64(other, argumentOfJob(job.slot, 3));
    jit.move(TrustedImm64(JSValue::ValueUndefined), other);
    jit.store64(other, argumentOfJob(job.slot, 0));
    jit.store64(value, argumentOfJob(job.slot, 1));
    jit.load64(Address(promise, JSPromise::offsetOfSlot()), other);
    jit.store64(other, argumentOfJob(job.slot, 2));
    // The first word of the job: the realm, the task and the status.
    jit.urshift64(word, TrustedImm32(promiseFlagsShift + JSPromise::inlineReactionMicrotaskShift), other);
    jit.and32(TrustedImm32(JSPromise::inlineReactionMicrotaskMask >> JSPromise::inlineReactionMicrotaskShift), other);
    jit.or32(TrustedImm32(static_cast<int32_t>(JSPromise::Status::Fulfilled) << 8), other);
    jit.lshift64(TrustedImm32(promiseFlagsShift), other);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), other);
    jit.store64(other, Address(job.slot, QueuedTask::offsetOfDispatcher()));
    settle();
    jit.store64(job.next, Address(job.queue, MicrotaskQueue::offsetOfEnd()));
    isFulfilled.link(&jit);
}

void emitRunPromiseAllResolveJob(StubAssembler& jit, const HeadJobRegisters& head, const PromiseAllJobRegisters& registers, GPRReg scratch, JumpList& isLeftToJobLoop)
{
    const auto [queue, slot, word] = head;
    const auto [context, values, value, index] = registers;
    const GPRReg butterfly = word;
    RELEASE_ASSERT(noOverlap(queue, slot, word, context, values, value, index, scratch, instanceGPR));

    // The array was made with a place for every element. Nobody but the jobs has seen it.
    auto loadOperands = [&](JumpList* otherwise) {
        jit.load64(argumentOfJob(slot, 0), context);
        jit.load64(Address(context, JSPromiseCombinatorsGlobalContext::offsetOfValues()), values);
        jit.load64(argumentOfJob(slot, 1), value);
        jit.load32(argumentOfJob(slot, 2), index);
        if (otherwise) {
            jit.load8(Address(values, JSCell::indexingTypeAndMiscOffset()), word);
            jit.and32(TrustedImm32(IndexingTypeMask), word);
            otherwise->append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(ArrayWithContiguous)));
        }
        jit.loadPtr(Address(values, JSObject::butterflyOffset()), butterfly);
        if (otherwise)
            otherwise->append(jit.branch32(CCallHelpers::AboveOrEqual, index, Address(butterfly, Butterfly::offsetOfPublicLength())));
    };

    loadOperands(&isLeftToJobLoop);
    Jump needsNoBarrier = jit.branchIfNotCell(value);
    isLeftToJobLoop.append(branchIfBarrierIsNeeded(jit, values, scratch));
    needsNoBarrier.link(&jit);
    Jump isNotLast = jit.branch64(CCallHelpers::NotEqual, Address(context, JSPromiseCombinatorsGlobalContext::offsetOfRemainingElementsCount()), TrustedImm32(1));

    // The last one: the promise is fulfilled with the array, which takes every register. Nobody looks at the array before the element is in it.
    const GPRReg promise = context;
    jit.load64(Address(context, JSPromiseCombinatorsGlobalContext::offsetOfPromise()), promise);
    emitFulfillPromise(jit, promise, values, scratch, { queue, slot, value, word }, isLeftToJobLoop);
    JumpList cannotHappen;
    emitLoadHeadJob(jit, head, cannotHappen);
    loadOperands(nullptr);

    isNotLast.link(&jit);
    jit.store64(value, BaseIndex(butterfly, index, CCallHelpers::TimesEight));
    jit.add64(TrustedImm32(-1), Address(context, JSPromiseCombinatorsGlobalContext::offsetOfRemainingElementsCount()));
    emitDequeueHeadJob(jit, queue, word);
    Jump isDone = jit.jump();
    cannotHappen.link(&jit);
    jit.breakpoint();
    isDone.link(&jit);
}

// The function of the frame pointer returns next, and the waiter is the one SavedFrame that waits for its result, which is no thenable.
// If the function may continue and no job is queued, the job of the waiter would be the next thing to run, and nothing of the program runs before the frame has returned to the adapter.
// So the adapter takes the waiter from the instance. Otherwise the job is queued. Every exit comes before beforeHandOver, which may overwrite registers.scratch.
// loadAsyncContext loads what the waiter captured when it was suspended, and runs before beforeHandOver.
static void emitHandOverToWaiter(StubAssembler& jit, GPRReg waiter, GPRReg value, const JobRegisters& registers, JumpList& needsOperation, const ScopedLambda<void(GPRReg)>& loadAsyncContext, const ScopedLambda<void()>& beforeHandOver)
{
    const AwaitRegisters conditionRegisters { InvalidGPRReg, InvalidGPRReg, registers.queue, registers.slot };
    RELEASE_ASSERT(noOverlap(waiter, value, registers.queue, registers.slot, registers.next, registers.scratch, instanceGPR));

    JumpList isQueued;
    emitBranchUnlessFrameMayContinue(jit, conditionRegisters, isQueued);
    isQueued.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.waiterToResume))));
    loadAsyncContext(registers.queue);
    beforeHandOver();
    jit.storePtr(waiter, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.waiterToResume)));
    jit.store64(value, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.waiterResolution)));
    jit.store64(registers.queue, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.waiterAsyncContext)));
    Jump isHandedOver = jit.jump();

    isQueued.link(&jit);
    emitReserveJob(jit, registers, needsOperation);
    loadAsyncContext(registers.scratch);
    emitStoreAsyncContextOfJob(jit, registers, registers.scratch);
    beforeHandOver();
    emitFillJob(jit, { InternalMicrotask::CompiledAsyncFunctionResume, JSPromise::Status::Fulfilled, value, waiter, JobAsyncContext::IsStored }, registers);
    isHandedOver.link(&jit);
}

StubSignature signatureOfReturnAsyncResult(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

void generateReturnAsyncResult(StubAssembler& jit)
{
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    const GPRReg promise = jit.clobberOperand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg result = jit.result();
    const GPRReg structure = jit.allocateScratch(StubRegister::T11);
    const GPRReg waiter = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = result;
    const JobRegisters job { jit.allocateScratch(StubRegister::T9), jit.allocateScratch(StubRegister::T10), structure, scratch };
    RELEASE_ASSERT(promise == operationArgumentGPR(1) && value == operationArgumentGPR(2));
    RELEASE_ASSERT(noOverlap(promise, value, result, structure, waiter, job.queue, job.slot, instanceGPR));
    RELEASE_ASSERT(result == gprOf(firstStubOperandRegister));

    JumpList slowCases;
    // The driver as the function got it: undefined where the caller awaits the call itself. The operation takes no promise as null.
    Jump hasPromise = jit.branchIfCell(promise);
    jit.move(TrustedImm32(0), promise);
    emitBranchIfMayBeThenable(jit, value, structure, scratch, slowCases);
    jit.loadPtr(Address(fp, sizeof(void*)), scratch);
    Jump hasBeenSuspended = jit.branchPtr(CCallHelpers::Equal, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(StubFrameKind::SavedFrameAdapter)));
    Label returnsValue = jit.label();
    jit.move(value, result);
    jit.returnToCaller();

    // The caller has awaited the call without a promise in between.
    hasBeenSuspended.link(&jit);
    jit.loadPtr(Address(fp), scratch);
    jit.loadPtr(Address(scratch, static_cast<int>(CallFrameSlot::callee) * static_cast<int>(sizeof(Register))), scratch);
    slowCases.append(jit.branch8(CCallHelpers::NotEqual, Address(scratch, SavedFrame::offsetOfContinuation()), TrustedImm32(static_cast<int32_t>(SavedFrame::Continuation::Waiter))));
    jit.loadPtr(Address(scratch, SavedFrame::offsetOfPromiseOrWaiter()), waiter);
    emitHandOverToWaiter(jit, waiter, value, job, slowCases, [&](GPRReg asyncContext) {
        jit.load64(Address(waiter, SavedFrame::offsetOfAsyncContext()), asyncContext);
    }, [] { });
    jit.jump().linkTo(returnsValue, &jit);

    hasPromise.link(&jit);
    jit.load64(Address(promise, JSPromise::offsetOfPacked()), scratch);
    Jump isTouched = jit.branchTest64(CCallHelpers::NonZero, scratch);
    emitBranchIfMayBeThenable(jit, value, structure, scratch, slowCases);
    jit.store64(value, Address(promise, JSPromise::offsetOfSlot()));
    jit.move(TrustedImm64(static_cast<int64_t>(fulfilledPromiseBits)), scratch);
    jit.store64(scratch, Address(promise, JSPromise::offsetOfPacked()));
    Label isFulfilled = jit.label();
    jit.move(promise, result);
    Jump valueIsNotCell = jit.branchIfNotCell(value);
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, promise, structure);
    valueIsNotCell.link(&jit);
    jit.returnToCaller();

    // The barrier takes its owner in the result register and preserves it.
    needsBarrier.link(&jit);
    jit.emitFunctionPrologue();
    jit.callStub(writeBarrier());
    jit.emitFunctionEpilogue();
    jit.returnToCaller();

    // The one reaction of the promise resumes a function of this instance. What follows is JSPromise::tryFulfillTakingInlineJob, or fulfillPromise where the job is queued.
    auto emitHandOverToWaiterOfPromise = [&](const ScopedLambda<void(GPRReg)>& loadAsyncContext) {
        emitBranchIfMayBeThenable(jit, value, structure, scratch, slowCases);
        jit.load64(Address(promise, JSPromise::offsetOfSlot()), waiter);
        jit.load32(Address(waiter, JSCell::structureIDOffset()), scratch);
        slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
        emitHandOverToWaiter(jit, waiter, value, job, slowCases, loadAsyncContext, [&] {
            jit.store64(value, Address(promise, JSPromise::offsetOfSlot()));
            jit.move(TrustedImm64(static_cast<int64_t>(fulfilledPromiseBits | promiseIsHandledBits)), scratch);
            jit.store64(scratch, Address(promise, JSPromise::offsetOfPacked()));
        });
        jit.jump().linkTo(isFulfilled, &jit);
    };
    isTouched.link(&jit);
    jit.move(TrustedImm64(static_cast<int64_t>(resumeReactionBits)), structure);
    Jump hasOtherBits = jit.branch64(CCallHelpers::NotEqual, scratch, structure);
    emitHandOverToWaiterOfPromise([&](GPRReg asyncContext) {
        jit.move(TrustedImm64(JSValue::ValueUndefined), asyncContext);
    });

    // The same reaction with the async context that the waiter captured in the place of the cell.
    hasOtherBits.link(&jit);
    constexpr unsigned numberOfFlagBits = 64 - promiseFlagsShift;
    jit.urshift64(TrustedImm32(promiseFlagsShift), scratch);
    slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<int32_t>(resumeReactionBits >> promiseFlagsShift | JSPromise::inlineReactionAsyncContextFlag))));
    emitHandOverToWaiterOfPromise([&](GPRReg asyncContext) {
        jit.load64(Address(promise, JSPromise::offsetOfPacked()), asyncContext);
        jit.lshift64(TrustedImm32(numberOfFlagBits), asyncContext);
        jit.urshift64(TrustedImm32(numberOfFlagBits), asyncContext);
    });

    // Not a tail call: the operation finds the async function's frame as the caller of the stub's frame.
    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTResolvePromiseOrReturnResult, { });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
