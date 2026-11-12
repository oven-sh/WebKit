/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCoroutines.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "AOTStubsBarriers.h"
#include "InternalFieldTuple.h"
#include "JSCJSValueInlines.h"
#include "JSPromise.h"
#include "Microtask.h"
#include "MicrotaskQueue.h"

namespace JSC { namespace AOT {

static_assert(sizeof(QueuedTask) == 5 * sizeof(uint64_t) && QueuedTask::maxArguments == 4);
static_assert(!QueuedTask::offsetOfDispatcher() && QueuedTask::offsetOfArguments() == sizeof(uint64_t));

Address argumentOfJob(GPRReg slot, unsigned index)
{
    RELEASE_ASSERT(index < QueuedTask::maxArguments);
    return Address(slot, static_cast<int32_t>(QueuedTask::offsetOfArguments() + index * sizeof(EncodedJSValue)));
}

static void emitLoadAddressOfJob(StubAssembler& jit, GPRReg queue, GPRReg indexAndResult, GPRReg scratch)
{
    jit.getEffectiveAddress(BaseIndex(indexAndResult, indexAndResult, CCallHelpers::TimesFour), indexAndResult);
    jit.loadPtr(Address(queue, MicrotaskQueue::offsetOfBuffer()), scratch);
    jit.getEffectiveAddress(BaseIndex(scratch, indexAndResult, CCallHelpers::TimesEight), indexAndResult);
}

void emitReserveJob(StubAssembler& jit, const JobRegisters& registers, JumpList& needsOperation)
{
    const auto [queue, slot, next, scratch] = registers;
    RELEASE_ASSERT(noOverlap(queue, slot, next, scratch, instanceGPR));
    needsOperation.append(jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(awaitMustSuspend))));
    jit.loadPtr(Address(instanceGPR, offsetOfMicrotaskQueue), queue);
    needsOperation.append(jit.branchTest8(CCallHelpers::Zero, Address(queue, MicrotaskQueue::offsetOfIsScheduledToRun())));
    jit.load64(Address(queue, MicrotaskQueue::offsetOfEnd()), slot);
    jit.load64(Address(queue, MicrotaskQueue::offsetOfCapacityMask()), scratch);
    jit.add64(TrustedImm32(1), slot, next);
    jit.and64(scratch, next);
    needsOperation.append(jit.branch64(CCallHelpers::Equal, next, Address(queue, MicrotaskQueue::offsetOfStart())));
    emitLoadAddressOfJob(jit, queue, slot, scratch);
}

void emitFillJob(StubAssembler& jit, const JobOperands& operands, const JobRegisters& registers)
{
    const auto [queue, slot, next, scratch] = registers;
    const uint64_t taskAndStatus = static_cast<uint64_t>(operands.task) | static_cast<uint64_t>(operands.status) << 8;
    RELEASE_ASSERT(noOverlap(queue, slot, next, scratch, operands.resolution, operands.context, instanceGPR));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    jit.or64(TrustedImm64(static_cast<int64_t>(taskAndStatus << promiseFlagsShift)), scratch);
    jit.store64(scratch, Address(slot, QueuedTask::offsetOfDispatcher()));
    if (operands.first != JSValue::ValueUndefined)
        jit.store64(TrustedImm64(operands.first), argumentOfJob(slot, 0));
    jit.move(TrustedImm64(JSValue::ValueUndefined), scratch);
    if (operands.first == JSValue::ValueUndefined)
        jit.store64(scratch, argumentOfJob(slot, 0));
    jit.store64(operands.resolution, argumentOfJob(slot, 1));
    jit.store64(operands.context, argumentOfJob(slot, 2));
    if (operands.asyncContext == JobAsyncContext::Current) {
        jit.store64(scratch, argumentOfJob(slot, 3));
        Jump capturesNothing = emitBranchIfTracksNoAsyncContext(jit, scratch);
        emitLoadCurrentAsyncContext(jit, scratch);
        jit.store64(scratch, argumentOfJob(slot, 3));
        capturesNothing.link(&jit);
    }
    jit.store64(next, Address(queue, MicrotaskQueue::offsetOfEnd()));
}

void emitPublishJob(StubAssembler& jit, InternalMicrotask task, JSPromise::Status status, const JobRegisters& registers)
{
    const auto [queue, slot, next, scratch] = registers;
    const uint64_t taskAndStatus = static_cast<uint64_t>(task) | static_cast<uint64_t>(status) << 8;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    jit.or64(TrustedImm64(static_cast<int64_t>(taskAndStatus << promiseFlagsShift)), scratch);
    jit.store64(scratch, Address(slot, QueuedTask::offsetOfDispatcher()));
    jit.store64(next, Address(queue, MicrotaskQueue::offsetOfEnd()));
}

void emitLoadHeadJob(StubAssembler& jit, const HeadJobRegisters& registers, JumpList& noJob)
{
    const auto [queue, slot, word] = registers;
    RELEASE_ASSERT(noOverlap(queue, slot, word, instanceGPR));
    noJob.append(jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(awaitMustSuspend))));
    jit.loadPtr(Address(instanceGPR, offsetOfMicrotaskQueue), queue);
    jit.load64(Address(queue, MicrotaskQueue::offsetOfStart()), slot);
    noJob.append(jit.branch64(CCallHelpers::Equal, slot, Address(queue, MicrotaskQueue::offsetOfEnd())));
    emitLoadAddressOfJob(jit, queue, slot, word);
    jit.load64(Address(slot, QueuedTask::offsetOfDispatcher()), word);
}

void emitPutAsyncContextOfJobInPlace(StubAssembler& jit, std::optional<Address> contextOfJob, GPRReg tuple, GPRReg context, JumpList& isLeftToJobLoop)
{
    const Address current(tuple, InternalFieldTuple::offsetOfInternalField(0));
    RELEASE_ASSERT(noOverlap(tuple, context, instanceGPR));
    auto loadContextOfJob = [&] {
        if (!contextOfJob) {
            jit.move(TrustedImm64(JSValue::ValueUndefined), context);
            return;
        }
        jit.load64(*contextOfJob, context);
        Jump isNotEmpty = jit.branchTest64(CCallHelpers::NonZero, context);
        jit.move(TrustedImm64(JSValue::ValueUndefined), context);
        isNotEmpty.link(&jit);
    };

    Jump tracksNone = emitBranchIfTracksNoAsyncContext(jit, tuple);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), tuple);
    jit.loadPtr(Address(tuple, JSGlobalObject::offsetOfAsyncContextData()), tuple);
    loadContextOfJob();
    Jump isInPlace = jit.branch64(CCallHelpers::Equal, context, current);
    isLeftToJobLoop.append(jit.branchTest8(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.asyncContextIsPutBack))));
    if (contextOfJob) {
        Jump needsNoBarrier = jit.branchIfNotCell(context);
        isLeftToJobLoop.append(branchIfBarrierIsNeeded(jit, tuple, context));
        jit.load64(*contextOfJob, context);
        needsNoBarrier.link(&jit);
    }
    jit.store64(context, current);
    isInPlace.link(&jit);
    tracksNone.link(&jit);
}

void emitTakeReactionJob(StubAssembler& jit, ReactionJob kind, const HeadJobRegisters& registers, GPRReg promise, GPRReg handler, GPRReg argument, JumpList& isLeftToJobLoop)
{
    const auto [queue, slot, scratch] = registers;
    static_assert(!JSValue::ValueEmpty && !(JSValue::ValueNull & ~JSValue::ValueUndefined));
    RELEASE_ASSERT(noOverlap(queue, slot, scratch, promise, handler, argument, instanceGPR));
    jit.load64(argumentOfJob(slot, 0), promise);
    isLeftToJobLoop.append(jit.branchIfNotCell(promise));
    jit.load32(Address(promise, JSCell::structureIDOffset()), scratch);
    isLeftToJobLoop.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, offsetOfPromiseHeader)));
    switch (kind) {
    case ReactionJob::HasNoAsyncContext:
        isLeftToJobLoop.append(jit.branchTest64(CCallHelpers::NonZero, argumentOfJob(slot, 3), TrustedImm32(~static_cast<int32_t>(JSValue::ValueUndefined))));
        emitPutAsyncContextOfJobInPlace(jit, std::nullopt, scratch, handler, isLeftToJobLoop);
        break;
    case ReactionJob::HasAsyncContext:
        emitPutAsyncContextOfJobInPlace(jit, argumentOfJob(slot, 3), scratch, handler, isLeftToJobLoop);
        break;
    }
    jit.load64(argumentOfJob(slot, 1), handler);
    jit.load64(argumentOfJob(slot, 2), argument);
    emitDequeueHeadJobAndPrefetch(jit, queue, scratch, PrefetchedJobs::Reactions);
}

void emitTakePerformMicrotaskJob(StubAssembler& jit, const HeadJobRegisters& registers, GPRReg function, JumpList& isLeftToJobLoop)
{
    const auto [queue, slot, scratch] = registers;
    RELEASE_ASSERT(noOverlap(queue, slot, scratch, function, instanceGPR));
    jit.load64(argumentOfJob(slot, 0), function);
    isLeftToJobLoop.append(jit.branchIfNotCell(function));
    isLeftToJobLoop.append(branchIfNotFunctionType(jit, function, scratch));
    isLeftToJobLoop.append(jit.branchTest64(CCallHelpers::NonZero, argumentOfJob(slot, 2)));
    isLeftToJobLoop.append(jit.branchTest64(CCallHelpers::NonZero, argumentOfJob(slot, 3)));
    isLeftToJobLoop.append(jit.branch64(CCallHelpers::NotEqual, argumentOfJob(slot, 1), TrustedImm64(JSValue::ValueUndefined)));
    Jump hasNoAsyncContext = emitBranchIfTracksNoAsyncContext(jit, scratch);
    emitLoadCurrentAsyncContext(jit, scratch);
    isLeftToJobLoop.append(jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(JSValue::ValueUndefined)));
    hasNoAsyncContext.link(&jit);
    emitDequeueHeadJob(jit, queue, scratch);
}

void emitTakeFunctionResumeJob(StubAssembler& jit, const HeadJobRegisters& registers, GPRReg saved, GPRReg value, JumpList& isLeftToJobLoop)
{
    const auto [queue, slot, scratch] = registers;
    RELEASE_ASSERT(noOverlap(queue, slot, scratch, saved, value, instanceGPR));
    jit.load64(argumentOfJob(slot, 2), saved);
    jit.load32(Address(saved, JSCell::structureIDOffset()), scratch);
    isLeftToJobLoop.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
    emitPutAsyncContextOfJobInPlace(jit, argumentOfJob(slot, 3), scratch, value, isLeftToJobLoop);
    jit.load64(argumentOfJob(slot, 1), value);
    emitDequeueHeadJobAndPrefetch(jit, queue, scratch, PrefetchedJobs::FunctionResumes);
}

void emitDequeueHeadJob(StubAssembler& jit, GPRReg queue, GPRReg scratch)
{
    Jump hasMarkedNone = jit.branchTest64(CCallHelpers::Zero, Address(queue, MicrotaskQueue::offsetOfMarkedBefore()));
    jit.add64(TrustedImm32(-1), Address(queue, MicrotaskQueue::offsetOfMarkedBefore()));
    hasMarkedNone.link(&jit);
    jit.load64(Address(queue, MicrotaskQueue::offsetOfStart()), scratch);
    jit.add64(TrustedImm32(1), scratch);
#if CPU(X86_64)
    jit.and64(Address(queue, MicrotaskQueue::offsetOfCapacityMask()), scratch);
#else
    jit.load64(Address(queue, MicrotaskQueue::offsetOfCapacityMask()), queue);
    jit.and64(queue, scratch);
    jit.loadPtr(Address(instanceGPR, offsetOfMicrotaskQueue), queue);
#endif
    jit.store64(scratch, Address(queue, MicrotaskQueue::offsetOfStart()));
}

void emitDequeueHeadJobAndPrefetch(StubAssembler& jit, GPRReg queue, GPRReg scratch, PrefetchedJobs kind)
{
    emitDequeueHeadJob(jit, queue, scratch);
#if CPU(X86_64)
    constexpr int32_t jobsInBetween = 5;
    constexpr int32_t bytesPerCacheLine = 64;
    jit.add64(TrustedImm32(jobsInBetween), scratch);
    jit.and64(Address(queue, MicrotaskQueue::offsetOfCapacityMask()), scratch);
    jit.getEffectiveAddress(BaseIndex(scratch, scratch, CCallHelpers::TimesFour), scratch);
    jit.loadPtr(Address(queue, MicrotaskQueue::offsetOfBuffer()), queue);
    jit.getEffectiveAddress(BaseIndex(queue, scratch, CCallHelpers::TimesEight), scratch);
    auto prefetchArgument = [&](unsigned index) {
        jit.load64(argumentOfJob(scratch, index), queue);
        jit.prefetch(Address(queue));
    };
    switch (kind) {
    case PrefetchedJobs::Reactions:
    case PrefetchedJobs::ThenableResolutions:
        prefetchArgument(0);
        prefetchArgument(1);
        break;
    case PrefetchedJobs::Resolutions:
        prefetchArgument(0);
        break;
    case PrefetchedJobs::FunctionResumes:
        prefetchArgument(2);
        static_assert(sizeof(SavedFrame) == bytesPerCacheLine);
        jit.prefetch(Address(queue, bytesPerCacheLine));
        jit.prefetch(Address(queue, 2 * bytesPerCacheLine));
        break;
    }
#else
    UNUSED_PARAM(kind);
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
