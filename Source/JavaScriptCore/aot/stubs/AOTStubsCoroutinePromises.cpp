/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCoroutines.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsOperations.h"
#include "CallFrame.h"
#include "JSGlobalObject.h"
#include "JSPromise.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "MicrotaskQueue.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

StubSignature signatureOfNewPromise(StubKey)
{
#if CPU(X86_64)
    return StubSignature::withGPRResult({ }).fillingKeptRegisterInFront(X86Registers::edx);
#else
    return StubSignature::withGPRResult({ });
#endif
}

struct NewPromiseRegisters { GPRReg promise, allocator, scratch; };

static AssemblyHelpers::FreeListRefill emitAllocatePromise(StubAssembler& jit, const NewPromiseRegisters& registers, JumpList& slowCases)
{
    const auto [promise, allocator, scratch] = registers;
    RELEASE_ASSERT(noOverlap(promise, allocator, scratch, instanceGPR));
    static_assert(JSPromise::offsetOfSlot() == JSPromise::offsetOfPacked() + static_cast<ptrdiff_t>(sizeof(uint64_t)));
    static_assert(JSObject::butterflyOffset() == sizeof(uint64_t) && !JSValue::ValueEmpty);
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
    return refill;
}

void generateNewPromise(StubAssembler& jit)
{
    JumpList slowCases;
    auto refill = emitAllocatePromise(jit, { jit.result(), jit.allocateScratch(StubRegister::T9), jit.allocateScratch(StubRegister::T11) }, slowCases);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowCases);
    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewPromise, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false });
}

StubSignature signatureOfNewPromiseBeforeBody(StubKey key) { return StubSignature::withGPRResult({ }).withResultIn(directArgumentGPR(key.form())); }

void generateNewPromiseBeforeBody(StubAssembler& jit)
{
#if CPU(X86_64)
    const GPRReg promise = jit.result();
    const GPRReg returnAddress = jit.allocateScratch(StubRegister::T13);
    const GPRReg argumentClobberedByNewPromise = gprOf(promise == gprOf(StubRegister::T11) ? StubRegister::T10 : StubRegister::T11);
    constexpr int32_t offsetOfReturnAddressOfCaller = sizeof(void*);
    JumpList resultIsUsedOtherwise;
    emitBranchUnlessResultIsAwaited(jit, Address(sp, offsetOfReturnAddressOfCaller), { returnAddress, promise, jit.allocateScratch(StubRegister::R0) }, resultIsUsedOtherwise);
    jit.move(TrustedImm64(JSValue::ValueUndefined), promise);
    jit.returnToCaller();

    resultIsUsedOtherwise.link(&jit);
    JumpList slowCases;
    auto refill = emitAllocatePromise(jit, { promise, jit.allocateScratch(StubRegister::T9), jit.reuseScratch(StubRegister::R0) }, slowCases);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowCases);
    slowCases.link(&jit);
    jit.pop(returnAddress);
    jit.emitFunctionPrologue();
    jit.preserving({ returnAddress, argumentClobberedByNewPromise }, [&] {
        jit.callStub(newPromise());
        jit.move(GPRInfo::returnValueGPR, promise);
    });
    jit.emitFunctionEpilogueWithEmptyFrame();
    jit.farJump(returnAddress, NoPtrTag);
#else
    UNUSED_PARAM(jit);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

StubSignature signatureOfReturnFulfilledPromise(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::R0) }); }

void generateReturnFulfilledPromise(StubAssembler& jit)
{
#if CPU(X86_64)
    const GPRReg value = jit.clobberOperand(0);
    const GPRReg promise = jit.allocateScratch(StubRegister::T10);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T9);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    RELEASE_ASSERT(value == jit.result());
    emitReturnFulfillmentIfTakenInsteadOfPromise(jit, value, value, { promise, allocator, scratch });
    JumpList slowCases;
    auto refill = emitAllocatePromise(jit, { promise, allocator, scratch }, slowCases);
    Label isAllocated = jit.label();
    jit.move(TrustedImm64(static_cast<int64_t>(fulfilledPromiseBits)), scratch);
    jit.store64(value, Address(promise, JSPromise::offsetOfSlot()));
    jit.store64(scratch, Address(promise, JSPromise::offsetOfPacked()));
    jit.move(promise, value);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowCases);
    slowCases.link(&jit);
    jit.emitFunctionPrologue();
    jit.preserving({ value }, [&] {
        jit.callStub(newPromise());
        jit.move(GPRInfo::returnValueGPR, promise);
    });
    jit.emitFunctionEpilogueWithEmptyFrame();
    jit.jump().linkTo(isAllocated, &jit);
#else
    UNUSED_PARAM(jit);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

StubSignature signatureOfPromiseThen(StubKey key)
{
    const bool isOnFulfilled = static_cast<PromiseHandler>(key.form()) == PromiseHandler::OnFulfilled;
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(isOnFulfilled ? StubRegister::A2 : StubRegister::A3), gprOf(StubRegister::A4) });
}

void generatePromiseThen(StubAssembler& jit)
{
    const bool isOnFulfilled = static_cast<PromiseHandler>(jit.key().form()) == PromiseHandler::OnFulfilled;
    const GPRReg promise = jit.operand(0);
    const GPRReg handler = jit.operand(1);
    const GPRReg derived = jit.operand(2);
    const GPRReg given = jit.result();
    const GPRReg otherHandler = jit.allocateScratch(isOnFulfilled ? StubRegister::A3 : StubRegister::A2);
    const GPRReg word = jit.allocateScratch(StubRegister::A5);
    const GPRReg other = otherHandler;
    const auto kind = isOnFulfilled ? JSPromise::InlineReactionKind::FulfillHandler : JSPromise::InlineReactionKind::RejectHandler;
    const uint64_t reactionBits = (static_cast<uint64_t>(JSPromise::isHandledFlag) | static_cast<uint64_t>(kind) << JSPromise::inlineReactionKindShift) << promiseFlagsShift;
    RELEASE_ASSERT(promise == operationArgumentGPR(1) && derived == operationArgumentGPR(4) && handler == operationArgumentGPR(isOnFulfilled ? 2 : 3));
    RELEASE_ASSERT(noOverlap(promise, handler, derived, otherHandler, word, given, instanceGPR));

    JumpList needsOperation;
    JumpList takesNoInlineReaction;
    Jump capturesNothing = emitBranchIfTracksNoAsyncContext(jit, other);
    emitLoadCurrentAsyncContext(jit, other);
    needsOperation.append(jit.branch64(CCallHelpers::NotEqual, other, TrustedImm64(JSValue::ValueUndefined)));
    capturesNothing.link(&jit);
    emitBranchUnlessHolds(jit, awaitedTakesInlineReaction, { InvalidGPRReg, promise, word, other }, takesNoInlineReaction);
    needsOperation.append(branchIfBarrierIsNeeded(jit, promise, other));
    jit.or64(derived, word);
    jit.or64(TrustedImm64(static_cast<int64_t>(reactionBits)), word);
    jit.store64(handler, Address(promise, JSPromise::offsetOfSlot()));
    jit.store64(word, Address(promise, JSPromise::offsetOfPacked()));
    jit.move(derived, given);
    jit.returnToCaller();

    takesNoInlineReaction.link(&jit);
    if (isOnFulfilled) {
        const JobRegisters job { jit.allocateScratch(StubRegister::T9), given, jit.allocateScratch(StubRegister::T12), word };
        RELEASE_ASSERT(noOverlap(promise, handler, derived, other, job.queue, job.slot, job.next, job.scratch, instanceGPR));
        emitBranchUnlessHolds(jit, awaitedIsFulfilled, { InvalidGPRReg, promise, word, other }, needsOperation);
        emitReserveJob(jit, job, needsOperation);
        jit.store64(derived, argumentOfJob(job.slot, 0));
        jit.store64(handler, argumentOfJob(job.slot, 1));
        jit.load64(Address(promise, JSPromise::offsetOfSlot()), other);
        jit.store64(other, argumentOfJob(job.slot, 2));
        jit.move(TrustedImm64(JSValue::ValueUndefined), other);
        jit.store64(other, argumentOfJob(job.slot, 3));
        emitPublishJob(jit, InternalMicrotask::PromiseReactionJob, JSPromise::Status::Fulfilled, job);
        jit.move(derived, given);
        jit.returnToCaller();
    } else {
        const JobRegisters job { jit.allocateScratch(StubRegister::T9), given, jit.allocateScratch(StubRegister::T12), word };
        RELEASE_ASSERT(noOverlap(promise, handler, derived, other, job.queue, job.slot, job.next, job.scratch, instanceGPR));
        emitBranchUnlessHolds(jit, awaitedIsFulfilled, { InvalidGPRReg, promise, word, other }, needsOperation);
        emitReserveJob(jit, job, needsOperation);
        jit.store64(derived, argumentOfJob(job.slot, 0));
        jit.load64(Address(promise, JSPromise::offsetOfSlot()), other);
        jit.store64(other, argumentOfJob(job.slot, 1));
        jit.move(TrustedImm64(JSValue::ValueUndefined), other);
        jit.store64(other, argumentOfJob(job.slot, 2));
        jit.store64(other, argumentOfJob(job.slot, 3));
        emitPublishJob(jit, InternalMicrotask::PromiseResolveWithoutHandlerJob, JSPromise::Status::Fulfilled, job);
        jit.move(derived, given);
        jit.returnToCaller();
    }

    needsOperation.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), otherHandler);
    jit.callOperationAndReturn(Entry::operationAOTPromiseThen, { .returns = OperationCall::Returns::Value });
}

void emitBranchIfMayBeThenable(StubAssembler& jit, GPRReg value, GPRReg structure, GPRReg scratch, JumpList& mayBeThenable)
{
    Jump isNotCell = jit.branchIfNotCell(value);
    Jump isNotObject = jit.branchIfNotObject(value);
    mayBeThenable.append(jit.branchIfType(value, JSPromiseType));
    loadStructure(jit, value, structure);
    jit.load32(Address(structure, Structure::bitFieldOffset()), scratch);
    jit.and32(TrustedImm32(Structure::s_definitelyNonThenableStateBits), scratch);
    mayBeThenable.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<int32_t>(Structure::DefinitelyNonThenableState::NonThenable) << Structure::s_definitelyNonThenableStateShift)));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    mayBeThenable.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(structure, Structure::realmOffset())));
    mayBeThenable.append(branchIfWatchpointSetIsNotWatched(jit, Address(scratch, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet)), scratch));
    isNotObject.link(&jit);
    isNotCell.link(&jit);
}

static_assert(noOverlap(resultGPROfPreservingCall(), gprOf(StubRegister::T9), gprOf(StubRegister::T11)));

StubSignature signatureOfIsNotThenable(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ Reg(resultGPROfPreservingCall()) });
    signature.firstResult = resultGPROfPreservingCall();
    return signature;
}

void generateIsNotThenable(StubAssembler& jit)
{
    const GPRReg value = jit.clobberOperand(0);
    const GPRReg result = jit.result();
    const GPRReg structure = jit.allocateScratch(StubRegister::T9);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    RELEASE_ASSERT(value == result);
    JumpList mayBeThenable;
    emitBranchIfMayBeThenable(jit, value, structure, scratch, mayBeThenable);
    jit.move(TrustedImm32(1), result);
    jit.returnToCaller();

    mayBeThenable.link(&jit);
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();
}

static void emitBarrierInAdapterFrame(StubAssembler& jit, GPRReg owner, GPRReg scratch)
{
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, owner, scratch);
    Label afterBarrier = jit.label();
    jit.addOutOfLinePart([needsBarrier, afterBarrier, owner](StubAssembler& jit) {
        needsBarrier.link(&jit);
        jit.move(owner, gprOf(firstStubOperandRegister));
        jit.callStub(writeBarrier().withCallerFrame(CallerFrame::KeepsNoFPRs));
        jit.jump().linkTo(afterBarrier, &jit);
    });
}

static void emitLoadReactionCell(StubAssembler& jit, GPRReg packed, GPRReg cell)
{
    constexpr unsigned numberOfFlagBits = 64 - promiseFlagsShift;
    jit.lshift64(packed, TrustedImm32(numberOfFlagBits), cell);
    jit.urshift64(TrustedImm32(numberOfFlagBits), cell);
}

static void emitQueueResolutionWithPromise(StubAssembler& jit, GPRReg promise, GPRReg resolution, GPRReg other, const JobRegisters& job, JumpList& needsEngine)
{
    needsEngine.append(jit.branch64(CCallHelpers::Equal, promise, resolution));
    jit.load32(Address(resolution, JSCell::structureIDOffset()), other);
    needsEngine.append(jit.branch32(CCallHelpers::NotEqual, other, Address(instanceGPR, offsetOfPromiseHeader)));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), other);
    needsEngine.append(branchIfWatchpointSetIsNotWatched(jit, Address(other, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet)), other));
    emitReserveJob(jit, job, needsEngine);
    jit.store64(resolution, argumentOfJob(job.slot, 0));
    jit.store64(promise, argumentOfJob(job.slot, 1));
    jit.move(TrustedImm64(JSValue::ValueUndefined), other);
    jit.store64(other, argumentOfJob(job.slot, 2));
    jit.store64(TrustedImm32(0), argumentOfJob(job.slot, 3));
    Jump capturesNothing = emitBranchIfTracksNoAsyncContext(jit, other);
    emitLoadCurrentAsyncContext(jit, other);
    jit.store64(other, argumentOfJob(job.slot, 2));
    capturesNothing.link(&jit);
    emitPublishJob(jit, InternalMicrotask::PromiseResolveThenableJobFast, JSPromise::Status::Pending, job);
}

static void emitFulfillPromiseWithoutBarrier(StubAssembler& jit, GPRReg promise, GPRReg value, GPRReg other, const JobRegisters& job, JumpList& mayBeThenable, JumpList& needsEngine)
{
    const GPRReg word = job.scratch;
    constexpr auto flagsOfReaction = [](JSPromise::InlineReactionKind kind) {
        return static_cast<int32_t>(JSPromise::isHandledFlag | static_cast<uint16_t>(kind) << JSPromise::inlineReactionKindShift);
    };
    constexpr uint64_t fulfilledBit = static_cast<uint64_t>(JSPromise::Status::Fulfilled) << promiseFlagsShift;
    constexpr uint64_t firstFunctionIsCalledBit = static_cast<uint64_t>(JSPromise::isFirstResolvingFunctionCalledFlag) << promiseFlagsShift;
    static_assert(static_cast<uint64_t>(JSPromise::inlineReactionAsyncContextFlag) << promiseFlagsShift == 1ull << 62);
    RELEASE_ASSERT(noOverlap(promise, value, other, job.queue, job.slot, job.next, job.scratch, instanceGPR));

    auto settle = [&] {
        jit.move(TrustedImm64(static_cast<int64_t>(firstFunctionIsCalledBit)), other);
        jit.and64(other, word);
        jit.or64(TrustedImm64(static_cast<int64_t>(promiseIsHandledBits | fulfilledBit)), word);
        jit.store64(value, Address(promise, JSPromise::offsetOfSlot()));
        jit.store64(word, Address(promise, JSPromise::offsetOfPacked()));
    };

    emitBranchIfMayBeThenable(jit, value, word, other, mayBeThenable);
    jit.load64(Address(promise, JSPromise::offsetOfPacked()), word);
    jit.move(TrustedImm64(static_cast<int64_t>(~pendingPromiseWithoutReactionBits)), other);
    Jump hasReaction = jit.branchTest64(CCallHelpers::NonZero, word, other);
    jit.or64(TrustedImm64(static_cast<int64_t>(fulfilledBit)), word);
    jit.store64(value, Address(promise, JSPromise::offsetOfSlot()));
    jit.store64(word, Address(promise, JSPromise::offsetOfPacked()));
    JumpList isFulfilled;
    isFulfilled.append(jit.jump());

    hasReaction.link(&jit);
    jit.urshift64(word, TrustedImm32(promiseFlagsShift), other);
    jit.and32(TrustedImm32(~static_cast<int32_t>(JSPromise::isFirstResolvingFunctionCalledFlag)), other);
    Jump hasNoFulfillHandler = jit.branch32(CCallHelpers::NotEqual, other, TrustedImm32(flagsOfReaction(JSPromise::InlineReactionKind::FulfillHandler)));

    emitReserveJob(jit, job, needsEngine);
    jit.load64(Address(promise, JSPromise::offsetOfPacked()), word);
    emitLoadReactionCell(jit, word, other);
    jit.store64(other, argumentOfJob(job.slot, 0));
    jit.load64(Address(promise, JSPromise::offsetOfSlot()), other);
    jit.store64(other, argumentOfJob(job.slot, 1));
    jit.store64(value, argumentOfJob(job.slot, 2));
    jit.store64(TrustedImm32(0), argumentOfJob(job.slot, 3));
    settle();
    emitPublishJob(jit, InternalMicrotask::PromiseReactionJob, JSPromise::Status::Fulfilled, job);
    isFulfilled.append(jit.jump());

    hasNoFulfillHandler.link(&jit);
    Jump hasNoHandler = jit.branch32(CCallHelpers::NotEqual, other, TrustedImm32(flagsOfReaction(JSPromise::InlineReactionKind::RejectHandler)));
    emitReserveJob(jit, job, needsEngine);
    jit.load64(Address(promise, JSPromise::offsetOfPacked()), word);
    emitLoadReactionCell(jit, word, other);
    jit.store64(other, argumentOfJob(job.slot, 0));
    jit.store64(value, argumentOfJob(job.slot, 1));
    jit.move(TrustedImm64(JSValue::ValueUndefined), other);
    jit.store64(other, argumentOfJob(job.slot, 2));
    jit.store64(TrustedImm32(0), argumentOfJob(job.slot, 3));
    settle();
    emitPublishJob(jit, InternalMicrotask::PromiseResolveWithoutHandlerJob, JSPromise::Status::Fulfilled, job);
    isFulfilled.append(jit.jump());

    hasNoHandler.link(&jit);
    jit.and32(TrustedImm32(~static_cast<int32_t>(JSPromise::inlineReactionMicrotaskMask | JSPromise::inlineReactionAsyncContextFlag)), other);
    needsEngine.append(jit.branch32(CCallHelpers::NotEqual, other, TrustedImm32(flagsOfReaction(JSPromise::InlineReactionKind::InternalMicrotask))));
    emitReserveJob(jit, job, needsEngine);
    jit.load64(Address(promise, JSPromise::offsetOfPacked()), word);
    emitLoadReactionCell(jit, word, other);
    jit.store64(other, argumentOfJob(job.slot, 3));
    Jump hasNoCell = jit.branchTest64(CCallHelpers::Zero, other);
    jit.store64(other, argumentOfJob(job.slot, 0));
    jit.lshift64(word, TrustedImm32(1), other);
    Jump hasAsyncContext = jit.branchTest64(CCallHelpers::Signed, other);
    jit.store64(TrustedImm32(0), argumentOfJob(job.slot, 3));
    Jump hasCell = jit.jump();
    hasAsyncContext.link(&jit);
    hasNoCell.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), other);
    jit.store64(other, argumentOfJob(job.slot, 0));
    hasCell.link(&jit);
    jit.store64(value, argumentOfJob(job.slot, 1));
    jit.load64(Address(promise, JSPromise::offsetOfSlot()), other);
    jit.store64(other, argumentOfJob(job.slot, 2));
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

void emitFulfillPromise(StubAssembler& jit, GPRReg promise, GPRReg value, GPRReg other, const JobRegisters& job, JumpList& mayBeThenable, JumpList& needsEngine)
{
    emitFulfillPromiseWithoutBarrier(jit, promise, value, other, job, mayBeThenable, needsEngine);
    Jump storesNoCell = jit.branchIfNotCell(value);
    emitBarrierInAdapterFrame(jit, promise, job.scratch);
    storesNoCell.link(&jit);
}

void emitQueueResolutionWithFulfillment(StubAssembler& jit, GPRReg promise, GPRReg value, GPRReg other, const JobRegisters& job, JumpList& isNotQueued)
{
    RELEASE_ASSERT(noOverlap(promise, value, other, job.queue, job.slot, job.next, job.scratch, instanceGPR));
    emitReserveJob(jit, job, isNotQueued);
    jit.store64(value, argumentOfJob(job.slot, 0));
    jit.store64(promise, argumentOfJob(job.slot, 1));
    jit.move(TrustedImm64(JSValue::ValueUndefined), other);
    jit.store64(other, argumentOfJob(job.slot, 2));
    jit.store64(TrustedImm32(0), argumentOfJob(job.slot, 3));
    Jump capturesNothing = emitBranchIfTracksNoAsyncContext(jit, other);
    emitLoadCurrentAsyncContext(jit, other);
    jit.store64(other, argumentOfJob(job.slot, 2));
    capturesNothing.link(&jit);
    emitPublishJob(jit, InternalMicrotask::PromiseResolveThenableJobFast, JSPromise::Status::Pending, job);
}

void emitResolvePromise(StubAssembler& jit, GPRReg promise, GPRReg value, GPRReg other, const JobRegisters& job, JumpList& needsEngine)
{
    JumpList mayBeThenable;
    emitFulfillPromise(jit, promise, value, other, job, mayBeThenable, needsEngine);
    Jump isFulfilled = jit.jump();
    mayBeThenable.link(&jit);
    emitQueueResolutionWithPromise(jit, promise, value, other, job, needsEngine);
    isFulfilled.link(&jit);
}

static void emitDequeueJob(StubAssembler& jit, const JobRegisters& job, PrefetchedJobs kind)
{
    jit.loadPtr(Address(instanceGPR, offsetOfMicrotaskQueue), job.queue);
    emitDequeueHeadJobAndPrefetch(jit, job.queue, job.scratch, kind);
}

void emitRunResolveThenableJob(StubAssembler& jit, GPRReg head, const ResolveThenableRegisters& registers, const JobRegisters& job, JumpList& isLeftToJobLoop)
{
    const auto [promise, promiseToResolve, other] = registers;
    RELEASE_ASSERT(noOverlap(promise, promiseToResolve, other, job.queue, job.slot, job.next, job.scratch, instanceGPR));
    RELEASE_ASSERT(noOverlap(head, promise, promiseToResolve));
    const GPRReg value = promise;
    jit.load64(argumentOfJob(head, 0), promise);
    jit.load64(argumentOfJob(head, 1), promiseToResolve);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), other);
    isLeftToJobLoop.append(branchIfWatchpointSetIsNotWatched(jit, Address(other, OBJECT_OFFSETOF(JSGlobalObject, m_promiseSpeciesWatchpointSet)), other));
    JumpList isValue;
    isValue.append(jit.branchIfNotCell(value));
    jit.load32(Address(promise, JSCell::structureIDOffset()), other);
    Jump hasOtherStructure = jit.branch32(CCallHelpers::NotEqual, other, Address(instanceGPR, offsetOfPromiseHeader));

    JumpList isNotPending;
    emitBranchUnlessHolds(jit, awaitedTakesInlineReaction, { InvalidGPRReg, promise, job.scratch, other }, isNotPending);
    jit.or64(promiseToResolve, job.scratch);
    jit.or64(TrustedImm64(static_cast<int64_t>(inlineReactionBitsOf(InternalMicrotask::PromiseResolveWithoutHandlerJob))), job.scratch);
    jit.move(TrustedImm64(JSValue::ValueUndefined), other);
    jit.store64(other, Address(promise, JSPromise::offsetOfSlot()));
    jit.store64(job.scratch, Address(promise, JSPromise::offsetOfPacked()));
    emitBarrierInAdapterFrame(jit, promise, other);
    Jump isRegistered = jit.jump();

    isNotPending.link(&jit);
    emitBranchUnlessHolds(jit, awaitedIsFulfilled, { InvalidGPRReg, promise, job.scratch, other }, isLeftToJobLoop);
    emitReserveJob(jit, job, isLeftToJobLoop);
    jit.load64(Address(promise, JSPromise::offsetOfSlot()), value);
    Jump hasValue = jit.jump();

    hasOtherStructure.link(&jit);
    isLeftToJobLoop.append(jit.branchIfType(promise, JSPromiseType));
    isValue.link(&jit);
    emitReserveJob(jit, job, isLeftToJobLoop);
    hasValue.link(&jit);
    jit.store64(promiseToResolve, argumentOfJob(job.slot, 0));
    jit.store64(value, argumentOfJob(job.slot, 1));
    jit.move(TrustedImm64(JSValue::ValueUndefined), other);
    jit.store64(other, argumentOfJob(job.slot, 2));
    jit.store64(TrustedImm32(0), argumentOfJob(job.slot, 3));
    emitPublishJob(jit, InternalMicrotask::PromiseResolveWithoutHandlerJob, JSPromise::Status::Fulfilled, job);
    isRegistered.link(&jit);
    emitDequeueJob(jit, job, PrefetchedJobs::ThenableResolutions);
}

void emitRunResolveWithoutHandlerJob(StubAssembler& jit, GPRReg head, GPRReg promise, GPRReg value, const JobRegisters& job, JumpList& isLeftToJobLoop)
{
    RELEASE_ASSERT(noOverlap(head, promise, value, job.queue, job.slot, job.next, job.scratch, instanceGPR));
    jit.load64(argumentOfJob(head, 0), promise);
    isLeftToJobLoop.append(jit.branchIfNotCell(promise));
    jit.load32(Address(promise, JSCell::structureIDOffset()), job.scratch);
    isLeftToJobLoop.append(jit.branch32(CCallHelpers::NotEqual, job.scratch, Address(instanceGPR, offsetOfPromiseHeader)));
    jit.load64(argumentOfJob(head, 1), value);
    emitFulfillPromise(jit, promise, value, head, job, isLeftToJobLoop, isLeftToJobLoop);
    emitDequeueJob(jit, job, PrefetchedJobs::Resolutions);
}

void emitRunPromiseAllResolveJob(StubAssembler& jit, const HeadJobRegisters& head, const PromiseAllJobRegisters& registers, GPRReg scratch, JumpList& isLeftToJobLoop)
{
    const auto [queue, slot, word] = head;
    const auto [context, values, value, index] = registers;
    const GPRReg butterfly = word;
    RELEASE_ASSERT(noOverlap(queue, slot, word, context, values, value, index, scratch, instanceGPR));

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

    const GPRReg promise = context;
    jit.load64(Address(context, JSPromiseCombinatorsGlobalContext::offsetOfPromise()), promise);
    emitFulfillPromise(jit, promise, values, scratch, { queue, slot, value, word }, isLeftToJobLoop, isLeftToJobLoop);
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
    Jump done = jit.jump();

    isQueued.link(&jit);
    emitReserveJob(jit, registers, needsOperation);
    loadAsyncContext(registers.scratch);
    jit.store64(registers.scratch, argumentOfJob(registers.slot, 3));
    beforeHandOver();
    emitFillJob(jit, { InternalMicrotask::CompiledAsyncFunctionResume, JSPromise::Status::Fulfilled, value, waiter, JobAsyncContext::IsStored }, registers);
    done.link(&jit);
}

StubSignature signatureOfResolveReturnedPromise(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) }); }

void generateResolveReturnedPromise(StubAssembler& jit)
{
    const GPRReg promise = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg result = jit.result();
    const GPRReg other = jit.allocateScratch(StubRegister::T12);
    const JobRegisters job { jit.allocateScratch(StubRegister::T9), jit.allocateScratch(StubRegister::T10), jit.allocateScratch(StubRegister::T11), result };
    constexpr uint64_t reactionMask = static_cast<uint64_t>(JSPromise::inlineReactionKindMask | JSPromise::inlineReactionMicrotaskMask) << promiseFlagsShift;
    RELEASE_ASSERT(promise == operationArgumentGPR(1) && value == operationArgumentGPR(2) && jit.operand(2) == operationArgumentGPR(3));
    RELEASE_ASSERT(noOverlap(promise, value, jit.operand(2), result, other, job.queue, job.slot, job.next, instanceGPR));

    JumpList needsOperation;
    needsOperation.append(jit.branchTestPtr(CCallHelpers::Zero, promise));
    jit.load64(Address(promise, JSPromise::offsetOfPacked()), result);
    jit.and64(TrustedImm64(static_cast<int64_t>(reactionMask)), result);
    jit.move(TrustedImm64(static_cast<int64_t>(resumeReactionBits & reactionMask)), other);
    needsOperation.append(jit.branch64(CCallHelpers::Equal, result, other));
    emitFulfillPromiseWithoutBarrier(jit, promise, value, other, job, needsOperation, needsOperation);
    jit.move(promise, result);
    Jump valueIsNotCell = jit.branchIfNotCell(value);
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, promise, other);
    valueIsNotCell.link(&jit);
    jit.returnToCaller();

    needsBarrier.link(&jit);
    jit.emitFunctionPrologue();
    jit.callStub(writeBarrier());
    jit.emitFunctionEpilogue();
    jit.returnToCaller();

    needsOperation.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTResolvePromiseOrReturnResult, { });
}

StubSignature signatureOfReturnAsyncResult(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }).fillingOperandInFront(1); }

static AsyncResult asyncResultOf(StubKey key) { return static_cast<AsyncResult>(key.form() / numberOfSetsSavedInAsyncFrame); }

StubSignature signatureOfReturnAsyncResultInTailPosition(StubKey key)
{
    if (asyncResultOf(key) == AsyncResult::Undefined)
        return StubSignature::withGPRResult({ gprOf(StubRegister::A1) });
    return signatureOfReturnAsyncResult(key);
}

static void emitLeaveAsyncFrame(StubAssembler& jit, RegistersSavedInAsyncFrame saved)
{
#if CPU(X86_64)
    auto restore = [&](GPRReg reg, int wordsBelowFramePointer) {
        jit.loadPtr(Address(fp, -wordsBelowFramePointer * static_cast<int>(sizeof(CPURegister))), reg);
    };
    switch (saved) {
    case RegistersSavedInAsyncFrame::None:
        break;
    case RegistersSavedInAsyncFrame::RBX:
        restore(X86Registers::ebx, 1);
        break;
    case RegistersSavedInAsyncFrame::R15:
        restore(X86Registers::r15, 1);
        break;
    case RegistersSavedInAsyncFrame::Both:
        restore(X86Registers::ebx, 2);
        restore(X86Registers::r15, 1);
        break;
    }
    jit.m_assembler.leave();
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(saved);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

static void emitReturnAsyncResult(StubAssembler& jit, std::optional<RegistersSavedInAsyncFrame> savedInFrameToLeave, AsyncResult returned)
{
    auto leaveFrame = [&] {
        if (savedInFrameToLeave)
            emitLeaveAsyncFrame(jit, *savedInFrameToLeave);
    };
    const GPRReg promise = jit.clobberOperand(0);
    const GPRReg value = returned == AsyncResult::Undefined ? jit.allocateScratch(StubRegister::A2) : jit.operand(1);
    const GPRReg result = jit.result();
    const GPRReg structure = jit.allocateScratch(StubRegister::T11);
    const GPRReg waiter = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = result;
    const JobRegisters job { jit.allocateScratch(StubRegister::T9), jit.allocateScratch(StubRegister::T10), structure, scratch };
    RELEASE_ASSERT(promise == operationArgumentGPR(1) && value == operationArgumentGPR(2));
    RELEASE_ASSERT(noOverlap(promise, value, result, structure, waiter, job.queue, job.slot, instanceGPR));
    RELEASE_ASSERT(result == gprOf(firstStubOperandRegister));

    JumpList slowCases;
    auto branchIfMayBeThenable = [&] {
        if (returned != AsyncResult::Undefined)
            emitBranchIfMayBeThenable(jit, value, structure, scratch, slowCases);
    };
    if (returned == AsyncResult::Undefined)
        jit.move(TrustedImm64(JSValue::ValueUndefined), value);
    Jump hasPromise = jit.branchIfCell(promise);
    jit.move(TrustedImm32(0), promise);
    branchIfMayBeThenable();
    jit.loadPtr(Address(fp, sizeof(void*)), scratch);
    Jump hasBeenSuspended = jit.branchPtr(CCallHelpers::Equal, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(StubFrameKind::SavedFrameAdapter)));
    Label returnsValue = jit.label();
    jit.move(value, result);
    leaveFrame();
    jit.returnToCaller();

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
    branchIfMayBeThenable();
    jit.store64(value, Address(promise, JSPromise::offsetOfSlot()));
    jit.move(TrustedImm64(static_cast<int64_t>(fulfilledPromiseBits)), scratch);
    jit.store64(scratch, Address(promise, JSPromise::offsetOfPacked()));
    Label isFulfilled = jit.label();
    jit.move(promise, result);
    Jump needsBarrier;
    if (returned != AsyncResult::Undefined) {
        Jump valueIsNotCell = jit.branchIfNotCell(value);
        needsBarrier = branchIfBarrierIsNeeded(jit, promise, structure);
        valueIsNotCell.link(&jit);
    }
    leaveFrame();
    jit.returnToCaller();

    if (returned != AsyncResult::Undefined) {
        needsBarrier.link(&jit);
        leaveFrame();
        jit.emitFunctionPrologue();
        jit.callStub(writeBarrier());
        jit.emitFunctionEpilogue();
        jit.returnToCaller();
    }

    auto emitHandOverToWaiterOfPromise = [&](const ScopedLambda<void(GPRReg)>& loadAsyncContext) {
        branchIfMayBeThenable();
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

    hasOtherBits.link(&jit);
    constexpr unsigned numberOfFlagBits = 64 - promiseFlagsShift;
    jit.urshift64(TrustedImm32(promiseFlagsShift), scratch);
    slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<int32_t>(resumeReactionBits >> promiseFlagsShift | JSPromise::inlineReactionAsyncContextFlag))));
    emitHandOverToWaiterOfPromise([&](GPRReg asyncContext) {
        jit.load64(Address(promise, JSPromise::offsetOfPacked()), asyncContext);
        jit.lshift64(TrustedImm32(numberOfFlagBits), asyncContext);
        jit.urshift64(TrustedImm32(numberOfFlagBits), asyncContext);
    });

    slowCases.link(&jit);
    const GPRReg frame = jit.reuseScratch(StubRegister::A3);
    RELEASE_ASSERT(frame == operationArgumentGPR(3) && noOverlap(promise, value, frame));
    jit.move(fp, frame);
    leaveFrame();
    checkStubOperands(resolveReturnedPromise(), { promise, value, frame });
    jit.jumpToStub(resolveReturnedPromise());
}

void generateReturnAsyncResult(StubAssembler& jit)
{
    emitReturnAsyncResult(jit, std::nullopt, AsyncResult::Any);
}

void generateReturnAsyncResultInTailPosition(StubAssembler& jit)
{
    emitReturnAsyncResult(jit, static_cast<RegistersSavedInAsyncFrame>(jit.key().form() % numberOfSetsSavedInAsyncFrame), asyncResultOf(jit.key()));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
