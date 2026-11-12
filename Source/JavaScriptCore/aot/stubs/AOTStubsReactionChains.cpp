/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCoroutines.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClosureWord.h"
#include "AOTInstance.h"
#include "AOTReactionChain.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsOperations.h"
#include "CompleteSubspace.h"
#include "JSGlobalObject.h"
#include "JSPromise.h"
#include "MarkedSpace.h"
#include "Microtask.h"
#include "MicrotaskQueue.h"

namespace JSC { namespace AOT {

static_assert(exists(newReactionChain(ReactionChain::maxWords)) && !exists(newReactionChain(ReactionChain::maxWords + 1)));
static_assert(static_cast<unsigned>(ReactionChainReceiver::FulfilledValue) + 1 == descriptorOf(StubFamily::StartReactionChain).numberOfForms);
static_assert(sizeof(CoroutineWords::handlerOfReactionChain) == (2 + ReactionChain::maxCapturesOfHandler) * sizeof(uint64_t));

static constexpr GPRReg wordOfReactionChainGPR(unsigned word) { return operationArgumentGPR(2 + word); }
static constexpr unsigned numberOfWordsInRegisters(StubKey key) { return std::min(key.count(), ReactionChain::maxWordsInRegisters); }

StubSignature signatureOfNewReactionChain(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(shortStubImmediateRegister), gprOf(StubRegister::A1), gprOf(StubRegister::A5) });
    for (unsigned word = 0; word < numberOfWordsInRegisters(key); ++word)
        signature.addOperand(wordOfReactionChainGPR(word));
    return signature;
}

void generateNewReactionChain(StubAssembler& jit)
{
    const unsigned numberOfWords = jit.key().count();
    const GPRReg description = jit.operand(0);
    const GPRReg handlers = jit.operand(1);
    const GPRReg promise = jit.operand(2);
    const GPRReg chain = jit.result();
    const GPRReg allocator = jit.allocateScratch(scratchBesideShortStubImmediate);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    const unsigned size = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(ReactionChain::allocationSize(numberOfWords));
    static_assert(ReactionChain::offsetOfIndex() == ReactionChain::offsetOfDescription() + 4 && !(ReactionChain::offsetOfDescription() % 8) && !JSValue::ValueEmpty);
    static_assert(ReactionChain::allocationSize(ReactionChain::maxWords) <= MarkedSpace::preciseCutoff && sizeof(Allocator) == sizeof(void*));
    RELEASE_ASSERT(noOverlap(description, handlers, promise, chain, allocator, scratch, instanceGPR));

    JumpList slowCases;
    slowCases.append(jit.branchTest64(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.reactionChainHeader))));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
    jit.loadPtr(Address(allocator, static_cast<int32_t>(CompleteSubspace::offsetOfAllocatorForSizeStep() + size / MarkedSpace::sizeStep * sizeof(Allocator))), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, chain, allocator, scratch, slowCases, size);
    Label isAllocated = jit.label();
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.reactionChainHeader)), scratch);
    jit.store64(scratch, Address(chain));
    jit.store64(promise, Address(chain, ReactionChain::offsetOfPromise()));
    jit.zeroExtend32ToWord(description, scratch);
    jit.store64(scratch, Address(chain, ReactionChain::offsetOfDescription()));
    jit.store64(handlers, Address(chain, ReactionChain::offsetOfHandlers()));
    jit.store64(TrustedImm32(0), Address(chain, ReactionChain::offsetOfHandlers() + 8));
    for (unsigned word = 0; word < numberOfWords; ++word) {
        Address place(chain, ReactionChain::offsetOfWords() + word * 8);
        if (word < numberOfWordsInRegisters(jit.key()))
            jit.store64(wordOfReactionChainGPR(word), place);
        else
            jit.store64(TrustedImm32(0), place);
    }
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowCases);
    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewReactionChain, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = description }, [&] {
        jit.jump().linkTo(isAllocated, &jit);
    });
}

static void emitQueueReactionChainStep(StubAssembler& jit, GPRReg chain, GPRReg value, GPRReg scratch, const JobRegisters& job)
{
    RELEASE_ASSERT(noOverlap(chain, value, scratch, job.slot, job.next, job.queue));
    jit.store64(chain, argumentOfJob(job.slot, 0));
    jit.store64(value, argumentOfJob(job.slot, 1));
    jit.move(TrustedImm64(JSValue::ValueUndefined), scratch);
    jit.store64(scratch, argumentOfJob(job.slot, 2));
    jit.store64(TrustedImm32(0), argumentOfJob(job.slot, 3));
    emitPublishJob(jit, InternalMicrotask::CompiledReactionChainStep, JSPromise::Status::Fulfilled, job);
}

StubSignature signatureOfStartReactionChain(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }

void generateStartReactionChain(StubAssembler& jit)
{
    const bool isOfValue = static_cast<ReactionChainReceiver>(jit.key().form()) == ReactionChainReceiver::FulfilledValue;
    const GPRReg receiver = jit.operand(0);
    const GPRReg chain = jit.operand(1);
    const GPRReg other = jit.allocateScratch(StubRegister::A3);
    const GPRReg word = jit.allocateScratch(StubRegister::A5);
    const JobRegisters job { jit.allocateScratch(StubRegister::T9), jit.allocateScratch(StubRegister::R0), jit.allocateScratch(StubRegister::T12), word };
    RELEASE_ASSERT(receiver == operationArgumentGPR(1) && chain == operationArgumentGPR(2));
    RELEASE_ASSERT(noOverlap(receiver, chain, other, word, job.queue, job.slot, job.next, instanceGPR));

    JumpList needsOperation;
    Jump capturesNothing = emitBranchIfTracksNoAsyncContext(jit, other);
    emitLoadCurrentAsyncContext(jit, other);
    needsOperation.append(jit.branch64(CCallHelpers::NotEqual, other, TrustedImm64(JSValue::ValueUndefined)));
    capturesNothing.link(&jit);
    GPRReg value = receiver;
    if (!isOfValue) {
        JumpList takesNoInlineReaction;
        emitBranchUnlessHolds(jit, awaitedTakesInlineReaction, { InvalidGPRReg, receiver, word, other }, takesNoInlineReaction);
        needsOperation.append(branchIfBarrierIsNeeded(jit, receiver, other));
        jit.or64(chain, word);
        jit.or64(TrustedImm64(static_cast<int64_t>(inlineReactionBitsOf(InternalMicrotask::CompiledReactionChainStep))), word);
        jit.move(TrustedImm64(JSValue::ValueUndefined), other);
        jit.store64(other, Address(receiver, JSPromise::offsetOfSlot()));
        jit.store64(word, Address(receiver, JSPromise::offsetOfPacked()));
        jit.returnToCaller();

        takesNoInlineReaction.link(&jit);
        emitBranchUnlessHolds(jit, awaitedIsFulfilled, { InvalidGPRReg, receiver, word, other }, needsOperation);
        value = other;
    }
    emitReserveJob(jit, job, needsOperation);
    if (!isOfValue)
        jit.load64(Address(receiver, JSPromise::offsetOfSlot()), value);
    emitQueueReactionChainStep(jit, chain, value, job.scratch, job);
    jit.returnToCaller();

    needsOperation.link(&jit);
    jit.callOperationAndReturn(isOfValue ? Entry::operationAOTStartReactionChainOfValue : Entry::operationAOTStartReactionChain, { .returns = OperationCall::Returns::Void, .checksException = false });
}

void emitTakeReactionChainStepJob(StubAssembler& jit, const HeadJobRegisters& registers, const ReactionChainRegisters& chainRegisters, JumpList& isForOtherStatus, JumpList& isLeftToJobLoop)
{
    const auto [queue, slot, scratch] = registers;
    const auto [chain, promise, target, handler, argument] = chainRegisters;
    const GPRReg index = handler;
    const GPRReg captures = argument;
    const GPRReg word = target;
    static_assert(ReactionChain::bitsOfHandler == 16 && ReactionChain::captureMask == 7 && ReactionChain::bitsOfCapture == 4);
    RELEASE_ASSERT(noOverlap(queue, slot, scratch, chain, promise, target, handler, argument, instanceGPR));
    auto wordOfHandler = [](unsigned wordIndex) { return Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.handlerOfReactionChain) + wordIndex * static_cast<int32_t>(sizeof(uint64_t))); };

    jit.load64(argumentOfJob(slot, 0), chain);
    isLeftToJobLoop.append(jit.branchTest64(CCallHelpers::NonZero, argumentOfJob(slot, 3), TrustedImm32(~static_cast<int32_t>(JSValue::ValueUndefined))));
    emitPutAsyncContextOfJobInPlace(jit, std::nullopt, scratch, handler, isLeftToJobLoop);
    jit.loadPtr(Address(chain, ReactionChain::offsetOfPromise()), promise);
    jit.load32(Address(chain, ReactionChain::offsetOfIndex()), index);
    jit.load16(BaseIndex(chain, index, CCallHelpers::TimesTwo, ReactionChain::offsetOfHandlers()), captures);
    isForOtherStatus.append(jit.branchTest32(CCallHelpers::NonZero, captures, TrustedImm32(ReactionChain::isRejectHandlerBit)));

    jit.load32(Address(chain, ReactionChain::offsetOfDescription()), word);
    jit.and32(TrustedImm32(ClosureWord::functionMask), word);
    jit.add32(index, word);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), scratch);
    jit.load64(BaseIndex(scratch, word, CCallHelpers::TimesEight), word);
    jit.store64(word, wordOfHandler(1));
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0])), scratch);
    jit.store64(scratch, wordOfHandler(0));
    JumpList areCaptured;
    for (unsigned capture = 0; capture < ReactionChain::maxCapturesOfHandler; ++capture) {
        areCaptured.append(jit.branchTest32(CCallHelpers::Zero, captures));
        jit.and32(TrustedImm32(ReactionChain::captureMask), captures, scratch);
        jit.load64(BaseIndex(chain, scratch, CCallHelpers::TimesEight, ReactionChain::offsetOfWords() - static_cast<int32_t>(sizeof(uint64_t))), scratch);
        jit.store64(scratch, wordOfHandler(2 + capture));
        jit.urshift32(TrustedImm32(ReactionChain::bitsOfCapture), captures);
    }
    areCaptured.link(&jit);

    jit.load32(Address(chain, ReactionChain::offsetOfDescription()), scratch);
    jit.urshift32(TrustedImm32(ReactionChain::lastHandlerShift), scratch);
    jit.and32(TrustedImm32((1 << ReactionChain::lastHandlerBits) - 1), scratch);
    jit.move(chain, target);
    Jump isNotLast = jit.branch32(CCallHelpers::NotEqual, scratch, index);
    jit.move(promise, target);
    isNotLast.link(&jit);
    jit.load64(argumentOfJob(slot, 1), argument);
    emitDequeueHeadJobAndPrefetch(jit, queue, scratch, PrefetchedJobs::Resolutions);
    jit.addPtr(TrustedImm32(AOT_INSTANCE_OFFSET(coroutines.handlerOfReactionChain)), instanceGPR, handler);
}

void emitBranchIfReactionChainContinues(StubAssembler& jit, GPRReg chain, GPRReg index, GPRReg scratch, JumpList& goesOn)
{
    jit.load32(Address(chain, ReactionChain::offsetOfDescription()), scratch);
    jit.urshift32(TrustedImm32(ReactionChain::lastHandlerShift), scratch);
    jit.and32(TrustedImm32((1 << ReactionChain::lastHandlerBits) - 1), scratch);
    goesOn.append(jit.branch32(CCallHelpers::NotEqual, scratch, index));
}

void emitAdvanceReactionChain(StubAssembler& jit, GPRReg chain)
{
    jit.add32(TrustedImm32(1), Address(chain, ReactionChain::offsetOfIndex()));
}

void emitResolveReactionChain(StubAssembler& jit, GPRReg chain, GPRReg value, GPRReg other, const JobRegisters& job, JumpList& needsEngine)
{
    RELEASE_ASSERT(noOverlap(chain, value, other, job.queue, job.slot, job.next, job.scratch, instanceGPR));
    JumpList mayBeThenable;
    emitBranchIfMayBeThenable(jit, value, other, job.scratch, mayBeThenable);
    emitReserveJob(jit, job, needsEngine);
    emitQueueReactionChainStep(jit, chain, value, other, job);
    Jump isQueued = jit.jump();
    mayBeThenable.link(&jit);
    emitQueueResolutionWithPromise(jit, chain, value, other, job, needsEngine);
    isQueued.link(&jit);
    emitAdvanceReactionChain(jit, chain);
}

void emitRunSettleReactionChainJob(StubAssembler& jit, GPRReg head, GPRReg chain, GPRReg value, GPRReg structureID, const JobRegisters& job, JumpList& isLeftToJobLoop)
{
    const GPRReg other = head;
    RELEASE_ASSERT(noOverlap(head, chain, value, job.queue, job.slot, job.next, job.scratch, instanceGPR));
    isLeftToJobLoop.append(jit.branch32(CCallHelpers::NotEqual, structureID, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.reactionChainHeader))));
    jit.load64(argumentOfJob(head, 1), value);
    emitBranchIfMayBeThenable(jit, value, other, job.scratch, isLeftToJobLoop);
    emitReserveJob(jit, job, isLeftToJobLoop);
    emitQueueReactionChainStep(jit, chain, value, other, job);
    jit.loadPtr(Address(instanceGPR, offsetOfMicrotaskQueue), job.queue);
    emitDequeueHeadJobAndPrefetch(jit, job.queue, job.scratch, PrefetchedJobs::Resolutions);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
