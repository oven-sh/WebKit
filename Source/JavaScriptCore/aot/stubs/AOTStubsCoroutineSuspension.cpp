/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCoroutines.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTRelocation.h"
#include "AOTSavedFrame.h"
#include "AOTStubsBarriers.h"
#include "CallFrame.h"
#include "CompleteSubspace.h"
#include "InternalFieldTuple.h"
#include "JSAsyncGenerator.h"
#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSPromise.h"
#include "MarkedSpace.h"
#include "StackAlignment.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

static Address emitAddressOf(StubAssembler& jit, const AwaitConditionWord& word, const AwaitRegisters& registers)
{
    const int32_t offset = static_cast<int32_t>(word.offset);
    switch (word.base) {
    case AwaitConditionBase::Instance:
        return Address(instanceGPR, offset);
    case AwaitConditionBase::VM:
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), registers.other);
        return Address(registers.other, offset);
    case AwaitConditionBase::MicrotaskQueue:
        jit.loadPtr(Address(instanceGPR, offsetOfMicrotaskQueue), registers.other);
        return Address(registers.other, offset);
    case AwaitConditionBase::AwaitedCellHeader:
    case AwaitConditionBase::AwaitedPromise:
        return Address(registers.awaited, offset);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitLoadWord(StubAssembler& jit, Address address, unsigned sizeInBytes, GPRReg result)
{
    switch (sizeInBytes) {
    case 1:
        jit.load8(address, result);
        return;
    case 4:
        jit.load32(address, result);
        return;
    case 8:
        jit.load64(address, result);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Jump branchIfNotZero(StubAssembler& jit, Address address, unsigned sizeInBytes)
{
    switch (sizeInBytes) {
    case 1:
        return jit.branchTest8(CCallHelpers::NonZero, address);
    case 4:
        return jit.branchTest32(CCallHelpers::NonZero, address);
    case 8:
        return jit.branchTest64(CCallHelpers::NonZero, address);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Jump branchIfNotEqual(StubAssembler& jit, GPRReg word, Address other, unsigned sizeInBytes)
{
    switch (sizeInBytes) {
    case 4:
        return jit.branch32(CCallHelpers::NotEqual, word, other);
    case 8:
        return jit.branch64(CCallHelpers::NotEqual, word, other);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void emitBranchUnlessHolds(StubAssembler& jit, const AwaitCondition& condition, const AwaitRegisters& registers, JumpList& doesNotHold)
{
    const auto [savedFrame, awaited, word, other] = registers;
    const unsigned sizeInBytes = condition.word.sizeInBytes;
    RELEASE_ASSERT(noOverlap(savedFrame, awaited, word, other, instanceGPR));
    switch (condition.test) {
    case AwaitConditionTest::HasAnyBit:
    case AwaitConditionTest::HasNoBit: {
        const bool wantsBit = condition.test == AwaitConditionTest::HasAnyBit;
        if (wantsBit && sizeInBytes == 1) {
            RELEASE_ASSERT(condition.bits <= UINT8_MAX);
            doesNotHold.append(jit.branchTest8(CCallHelpers::Zero, emitAddressOf(jit, condition.word, registers), TrustedImm32(static_cast<int32_t>(condition.bits))));
            return;
        }
        emitLoadWord(jit, emitAddressOf(jit, condition.word, registers), sizeInBytes, word);
        jit.move(TrustedImm64(static_cast<int64_t>(condition.bits)), other);
        doesNotHold.append(jit.branchTest64(wantsBit ? CCallHelpers::Zero : CCallHelpers::NonZero, word, other));
        return;
    }
    case AwaitConditionTest::IsZero:
        doesNotHold.append(branchIfNotZero(jit, emitAddressOf(jit, condition.word, registers), sizeInBytes));
        return;
    case AwaitConditionTest::EqualsOther: {
        RELEASE_ASSERT(condition.other.sizeInBytes == sizeInBytes);
        Address address = emitAddressOf(jit, condition.word, registers);
        emitLoadWord(jit, address, sizeInBytes, word);
        Address otherAddress = condition.other.base == condition.word.base ? Address(address.base, static_cast<int32_t>(condition.other.offset)) : emitAddressOf(jit, condition.other, registers);
        doesNotHold.append(branchIfNotEqual(jit, word, otherAddress, sizeInBytes));
        return;
    }
    case AwaitConditionTest::EqualsFramePointer:
        RELEASE_ASSERT(sizeInBytes == sizeof(void*));
        doesNotHold.append(jit.branchPtr(CCallHelpers::NotEqual, emitAddressOf(jit, condition.word, registers), GPRInfo::callFrameRegister));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Jump emitBranchIfTracksNoAsyncContext(StubAssembler& jit, GPRReg vm)
{
    static_assert(tracksNoAsyncContext.word.base == AwaitConditionBase::VM && tracksNoAsyncContext.word.sizeInBytes == 1 && tracksNoAsyncContext.test == AwaitConditionTest::IsZero);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
    return jit.branchTest8(CCallHelpers::Zero, Address(vm, static_cast<int32_t>(tracksNoAsyncContext.word.offset)));
}

void emitLoadCurrentAsyncContext(StubAssembler& jit, GPRReg context)
{
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), context);
    jit.loadPtr(Address(context, JSGlobalObject::offsetOfAsyncContextData()), context);
    jit.load64(Address(context, InternalFieldTuple::offsetOfInternalField(0)), context);
}

static void emitStoreCurrentAsyncContext(StubAssembler& jit, TrustedImm64 context, GPRReg scratch)
{
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    jit.loadPtr(Address(scratch, JSGlobalObject::offsetOfAsyncContextData()), scratch);
    jit.store64(context, Address(scratch, InternalFieldTuple::offsetOfInternalField(0)));
}

void emitBranchUnlessFrameMayContinue(StubAssembler& jit, const AwaitRegisters& registers, JumpList& mustSuspend)
{
    for (const AwaitCondition& condition : awaitConditionsOnState)
        emitBranchUnlessHolds(jit, condition, registers, mustSuspend);
}

static constexpr Address resumableFrameAddress(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.resumableFrame));
static constexpr int32_t maxBytesOfAwaitingFrames = 128 * KB;

static void emitBranchUnlessCallIsAwaited(StubAssembler& jit, GPRReg returnAddress, GPRReg awaitStub, GPRReg scratch, JumpList& otherwise, unsigned sizeOfHeadOfAwaitStub = 0)
{
    RELEASE_ASSERT(noOverlap(returnAddress, awaitStub, scratch, instanceGPR));
#if CPU(X86_64)
    constexpr int32_t moveIntoSiteWord = moveIntoSiteWordOfAwaitedCall[0] | moveIntoSiteWordOfAwaitedCall[1] << 8;
    constexpr int32_t call = 0xe8;
    constexpr int32_t offsetOfCall = 6;
    constexpr int32_t offsetOfDisplacement = offsetOfCall + 1;
    constexpr int32_t offsetAfterCall = offsetOfDisplacement + sizeof(int32_t);
    static_assert(gprOf(resumeModeRegister) == X86Registers::r10);
    otherwise.append(jit.branch16(CCallHelpers::NotEqual, Address(returnAddress), TrustedImm32(moveIntoSiteWord)));
    otherwise.append(jit.branch8(CCallHelpers::NotEqual, Address(returnAddress, offsetOfCall), TrustedImm32(call)));
    jit.load32SignedExtendTo64(Address(returnAddress, offsetOfDisplacement), scratch);
    jit.getEffectiveAddress(BaseIndex(returnAddress, scratch, CCallHelpers::TimesOne, offsetAfterCall + static_cast<int32_t>(sizeOfHeadOfAwaitStub)), scratch);
    otherwise.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, awaitStub));
#elif CPU(ARM64)
    RELEASE_ASSERT(!sizeOfHeadOfAwaitStub);
    constexpr int32_t branchWithLink = static_cast<int32_t>(0x94000000u);
    constexpr int32_t displacementMask = 0x3ffffff;
    JumpList isAwaited;
    jit.sub64(awaitStub, returnAddress, scratch);
    jit.urshift64(TrustedImm32(2), scratch);
    for (int32_t offset : { 4, 8 }) {
        jit.sub32(TrustedImm32(1), scratch);
        jit.and32(TrustedImm32(displacementMask), scratch);
        jit.or32(TrustedImm32(branchWithLink), scratch);
        if (offset == 4)
            isAwaited.append(jit.branch32(CCallHelpers::Equal, scratch, Address(returnAddress, offset)));
        else
            otherwise.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(returnAddress, offset)));
    }
    isAwaited.link(&jit);
#endif
}

#if CPU(X86_64)
void emitBranchUnlessResultIsAwaited(StubAssembler& jit, Address returnAddressOnStack, const AwaitedResultRegisters& registers, JumpList& otherwise)
{
    const auto [returnAddress, awaitStub, scratch] = registers;
    jit.loadPtr(returnAddressOnStack, returnAddress);
    jit.loadStubAddress(awaitBySavingFrame(), awaitStub);
    emitBranchUnlessCallIsAwaited(jit, returnAddress, awaitStub, scratch, otherwise, sizeOfHeadOfStub(entryInFrontOfStub));
}

void emitReturnFulfillmentIfTakenInsteadOfPromise(StubAssembler& jit, GPRReg value, GPRReg result, const AwaitedResultRegisters& registers)
{
    JumpList isNotAwaited;
    emitBranchUnlessResultIsAwaited(jit, Address(sp), registers, isNotAwaited);
    if (value != result)
        jit.move(value, result);
    jit.returnToCaller();

    isNotAwaited.link(&jit);
    JumpList needsPromise;
    jit.loadStubAddress(handlerOfJobReturn(), registers.scratch);
    needsPromise.append(jit.branchPtr(CCallHelpers::NotEqual, registers.returnAddress, registers.scratch));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), registers.returnAddress);
    needsPromise.append(branchIfWatchpointSetIsNotWatched(jit, Address(registers.returnAddress, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet)), registers.scratch));
    jit.store8(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.handlerReturnsFulfillment)));
    if (value != result)
        jit.move(value, result);
    jit.returnToCaller();
    needsPromise.link(&jit);
}
#endif

struct AwaitingCallerRegisters { GPRReg frame, returnAddress, awaitStub, scratch; };

static void emitBranchUnlessCallersAwait(StubAssembler& jit, const AwaitingCallerRegisters& registers, JumpList& otherwise)
{
    const auto [frame, returnAddress, awaitStub, scratch] = registers;
    RELEASE_ASSERT(noOverlap(frame, returnAddress, awaitStub, scratch, instanceGPR));
    const GPRReg vm = scratch;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
    otherwise.append(jit.branchTest8(CCallHelpers::NonZero, Address(vm, static_cast<int32_t>(tracksNoAsyncContext.word.offset))));
    jit.loadPtr(Address(vm, VM::topEntryFrameOffset()), scratch);
    jit.subPtr(fp, scratch);
    otherwise.append(jit.branch64(CCallHelpers::Above, scratch, TrustedImm32(maxBytesOfAwaitingFrames)));
    jit.loadStubAddress(awaitCallBySavingFrame(), awaitStub);
    jit.move(fp, frame);
    Label nextFrame = jit.label();
    jit.loadPtr(Address(frame, sizeof(void*)), returnAddress);
    emitBranchUnlessCallIsAwaited(jit, returnAddress, awaitStub, scratch, otherwise);
    jit.loadPtr(Address(frame), frame);
    jit.branchPtr(CCallHelpers::NotEqual, resumableFrameAddress, frame).linkTo(nextFrame, &jit);
}

static void emitLoadSettledValue(StubAssembler& jit, const AwaitRegisters& registers, GPRReg result, JumpList& isOtherObject, JumpList& mustSuspend)
{
    const auto [savedFrame, awaited, word, other] = registers;
    static_assert(awaitConditionsOnObject.size() == 2 && awaitConditionsOnObject[0] == awaitedHasPromiseStructure && awaitConditionsOnObject[1] == awaitedIsFulfilled);
    RELEASE_ASSERT(noOverlap(result, awaited, word, other));
    Jump notCell = jit.branchIfNotCell(awaited);
    Jump isObject = jit.branchIfObject(awaited);
    mustSuspend.append(jit.branchIfType(awaited, SentinelType));
    Jump notObject = jit.jump();
    isObject.link(&jit);
    emitBranchUnlessHolds(jit, awaitedHasPromiseStructure, registers, isOtherObject);
    emitBranchUnlessHolds(jit, awaitedIsFulfilled, registers, mustSuspend);
    jit.move(TrustedImm64(static_cast<int64_t>(promiseIsHandledBits)), other);
    jit.or64(other, word);
    jit.store64(word, Address(awaited, JSPromise::offsetOfPacked()));
    jit.load64(Address(awaited, JSPromise::offsetOfSlot()), result);
    notCell.link(&jit);
    notObject.link(&jit);
}

static void emitLoadSettledResolution(StubAssembler& jit, const AwaitRegisters& registers, GPRReg result, JumpList& isOtherObject, JumpList& mustSuspend)
{
    const auto [savedFrame, awaited, word, other] = registers;
    RELEASE_ASSERT(noOverlap(result, awaited, word, other));
    Jump notCell = jit.branchIfNotCell(awaited);
    Jump isObject = jit.branchIfObject(awaited);
    mustSuspend.append(jit.branchIfType(awaited, SentinelType));
    Jump notObject = jit.jump();
    isObject.link(&jit);
    emitBranchUnlessHolds(jit, awaitedHasPromiseStructure, registers, isOtherObject);
    emitBranchUnlessHolds(jit, awaitedIsFulfilled, registers, mustSuspend);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), word);
    mustSuspend.append(branchIfWatchpointSetIsNotWatched(jit, Address(word, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet)), other));
    mustSuspend.append(branchIfWatchpointSetIsNotWatched(jit, Address(word, OBJECT_OFFSETOF(JSGlobalObject, m_promiseSpeciesWatchpointSet)), other));
    jit.load64(Address(awaited, JSPromise::offsetOfSlot()), result);
    emitBranchIfMayBeThenable(jit, result, word, other, mustSuspend);
    jit.load64(Address(awaited, JSPromise::offsetOfPacked()), word);
    jit.move(TrustedImm64(static_cast<int64_t>(promiseIsHandledBits)), other);
    jit.or64(other, word);
    jit.store64(word, Address(awaited, JSPromise::offsetOfPacked()));
    notCell.link(&jit);
    notObject.link(&jit);
}

static void emitBranchUnlessQueueMaySettle(StubAssembler& jit, GPRReg awaited, JumpList& mustSuspend)
{
    mustSuspend.append(jit.branchPtr(CCallHelpers::NotEqual, awaited, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.lastPromiseResolvedWithSettled))));
}

static void emitSettleAwaitedPromise(StubAssembler& jit, GPRReg awaited, GPRReg siteWord, JumpList& mustSuspend)
{
    jit.preserving({ awaited, siteWord }, [&] {
        jit.recordClobbers(RegisterSet { leafArgumentGPR(0), leafArgumentGPR(1) });
        if (awaited != leafArgumentGPR(1))
            jit.move(awaited, leafArgumentGPR(1));
        jit.move(instanceGPR, leafArgumentGPR(0));
        jit.callLeaf(Entry::operationAOTSettleAwaitedPromise, 2 * sizeof(CPURegister));
        jit.move(GPRInfo::returnValueGPR, leafFunctionGPR);
    });
    mustSuspend.append(jit.branchTest32(CCallHelpers::Zero, leafFunctionGPR));
}

void emitSiteContinuationNote(StubAssembler& jit)
{
#if CPU(X86_64)
    constexpr uint8_t clearCarry = 0xf8;
    jit.m_assembler.buffer().putByte(static_cast<int8_t>(clearCarry));
#else
    UNUSED_PARAM(jit);
#endif
}

static Label emitFunctionReturn(StubAssembler& jit)
{
    Label returnToCallerOfFunction = jit.label();
    jit.emitFunctionEpilogue();
    jit.ret();
    return returnToCallerOfFunction;
}

static void emitReturnToCallerOfFunction(StubAssembler& jit, GPRReg scratch, Label returnToCallerOfFunction)
{
#if CPU(X86_64)
    constexpr uint8_t setCarry = 0xf9;
    jit.loadPtr(Address(sp), scratch);
    Jump siteDoesNotLeaveFunction = jit.branch32(CCallHelpers::NotEqual, Address(scratch), TrustedImm32(std::bit_cast<int32_t>(leaveFunctionIfCarryIsSet)));
    jit.m_assembler.buffer().putByte(static_cast<int8_t>(setCarry));
    jit.ret();
    siteDoesNotLeaveFunction.link(&jit);
    jit.loadLabelAddress(returnToCallerOfFunction, scratch);
    jit.storePtr(scratch, Address(sp));
    jit.ret();
#else
    jit.loadLabelAddress(returnToCallerOfFunction, scratch);
    jit.m_assembler.ret(scratch);
#endif
}

static void emitLeaveFunction(StubAssembler& jit, GPRReg siteWord, GPRReg scratch, Label returnToCallerOfFunction)
{
    emitMoveRegistersSavedInFrame(jit, siteWord, SavedFrame::liveSlotMapOffsetBits, RegistersSavedInFrame::AreRestored);
    emitReturnToCallerOfFunction(jit, scratch, returnToCallerOfFunction);
}

static constexpr int32_t jobsWorthUnexpectedReturns = 16;

static void emitLeaveFunctionOfJob(StubAssembler& jit, GPRReg siteWord, GPRReg scratch, GPRReg jobs, Label returnToCallerOfFunction)
{
    emitMoveRegistersSavedInFrame(jit, siteWord, SavedFrame::liveSlotMapOffsetBits, RegistersSavedInFrame::AreRestored);
    const Address loopOfJobs(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(StubFrameKind::SavedFrameAdapter));
    jit.loadPtr(Address(instanceGPR, offsetOfMicrotaskQueue), scratch);
    jit.load64(Address(scratch, MicrotaskQueue::offsetOfEnd()), jobs);
#if CPU(X86_64)
    jit.sub64(Address(scratch, MicrotaskQueue::offsetOfStart()), jobs);
#else
    jit.load64(Address(scratch, MicrotaskQueue::offsetOfStart()), scratch);
    jit.sub64(scratch, jobs);
#endif
    Jump fewJobsWait = jit.branch64(CCallHelpers::BelowOrEqual, jobs, TrustedImm32(jobsWorthUnexpectedReturns));
    jit.loadPtr(Address(fp, sizeof(void*)), scratch);
    Jump isNotJob = jit.branchPtr(CCallHelpers::NotEqual, scratch, loopOfJobs);
    jit.emitFunctionEpilogue();
#if CPU(X86_64)
    jit.addPtr(TrustedImm32(sizeof(void*)), sp);
#endif
    jit.farJump(scratch, JSEntryPtrTag);
    fewJobsWait.link(&jit);
    isNotJob.link(&jit);
    emitReturnToCallerOfFunction(jit, scratch, returnToCallerOfFunction);
}

static void emitCallOperationAndLeaveFunction(StubAssembler& jit, Entry operation, GPRReg siteWord, Label returnToCallerOfFunction, GPRReg valueToReturn = InvalidGPRReg)
{
    constexpr int32_t offsetOfSiteWord = OBJECT_OFFSETOF(SuspensionWords, siteWord);
    const GPRReg instance = jit.reuseScratch(StubRegister::A0);
    const GPRReg suspensionWords = jit.reuseScratch(StubRegister::A5);
    const GPRReg scratch = suspensionWords;
    static_assert(sizeof(SuspensionWords) == stackAlignmentBytes());
    static_assert(!offsetOfSiteWord && OBJECT_OFFSETOF(SuspensionWords, preservedRegister) == sizeof(CPURegister));
#if CPU(X86_64)
    jit.move(SavedFrame::preservedRegister, scratch);
#endif
    emitMoveRegistersSavedInFrame(jit, siteWord, SavedFrame::liveSlotMapOffsetBits, RegistersSavedInFrame::AreRestored);
    jit.emitFunctionPrologue();
    if (valueToReturn != InvalidGPRReg) {
        jit.subPtr(TrustedImm32(stackAlignmentBytes()), sp);
        jit.store64(valueToReturn, Address(sp));
    }
#if CPU(X86_64)
    jit.push(scratch);
    jit.push(siteWord);
#else
    jit.subPtr(TrustedImm32(sizeof(SuspensionWords)), sp);
    jit.store64(siteWord, Address(sp, offsetOfSiteWord));
#endif
    jit.move(sp, suspensionWords);
    jit.move(instanceGPR, instance);
    jit.callOperation(operation, jit.reuseScratch(StubRegister::Operation));
    if (valueToReturn != InvalidGPRReg) {
        RELEASE_ASSERT(noOverlap(valueToReturn, GPRInfo::returnValueGPR2));
        jit.load64(Address(sp, sizeof(SuspensionWords)), valueToReturn);
    }
#if CPU(X86_64)
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.emitFunctionEpilogue();
    emitReturnToCallerOfFunction(jit, scratch, returnToCallerOfFunction);
    exception.link(&jit);
    jit.load64(Address(sp, OBJECT_OFFSETOF(SuspensionWords, preservedRegister)), SavedFrame::preservedRegister);
    jit.emitFunctionEpilogue();
#else
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    emitReturnToCallerOfFunction(jit, scratch, returnToCallerOfFunction);
    exception.link(&jit);
#endif
    jit.jumpToExceptionHandler();
}

static void emitSuspend(StubAssembler& jit, Entry operation, GPRReg siteWord, Label returnToCallerOfFunction, GPRReg valueToReturn = InvalidGPRReg)
{
    const GPRReg frame = jit.reuseScratch(StubRegister::A2);
    const GPRReg bottom = jit.reuseScratch(StubRegister::A3);
    const GPRReg resumeAddress = jit.reuseScratch(StubRegister::A4);
    jit.move(fp, frame);
#if CPU(X86_64)
    jit.addPtr(TrustedImm32(sizeof(void*)), sp, bottom);
#else
    jit.move(sp, bottom);
#endif
    jit.loadCallerReturnAddress(0, resumeAddress);
    emitCallOperationAndLeaveFunction(jit, operation, siteWord, returnToCallerOfFunction, valueToReturn);
}

struct FrameCopyRegisters { GPRReg subject, saved, count, cursor, word, second; };
#if CPU(X86_64)
static constexpr StubRegister secondRegisterOfFrameCopy = StubRegister::A0;
#elif CPU(ARM64)
static constexpr StubRegister secondRegisterOfFrameCopy = StubRegister::T10;
#endif

#if CPU(X86_64)
struct FrameCopyScratch { };
static FrameCopyScratch allocateFrameCopyScratch(StubAssembler&) { return { }; }
#elif CPU(ARM64)
struct FrameCopyScratch { GPRReg destination; FPRReg firstVector, secondVector; };
static FrameCopyScratch allocateFrameCopyScratch(StubAssembler& jit)
{
    return { jit.allocateScratch(StubRegister::T11), jit.allocateScratch(FPRInfo::fpRegT0), jit.allocateScratch(FPRInfo::fpRegT1) };
}
#endif

static void emitCopyFrameWords(StubAssembler& jit, const FrameCopyRegisters& registers, const FrameCopyScratch& scratch)
{
    const auto [subject, saved, count, cursor, word, second] = registers;
    const GPRReg source = cursor;
    jit.load16(Address(saved, SavedFrame::offsetOfNumberOfWords()), count);
    jit.load16(Address(saved, SavedFrame::offsetOfNumberOfWordsSkippedAtBottom()), word);
#if CPU(X86_64)
    constexpr int32_t offsetOfBottom = sizeof(void*);
    constexpr int32_t bytesPerWord = sizeof(uint64_t);
    UNUSED_PARAM(scratch);
    auto copyPair = [&](int32_t wordsBelowCount) {
        jit.load64(BaseIndex(source, count, CCallHelpers::TimesEight, -(wordsBelowCount + 2) * bytesPerWord), word);
        jit.load64(BaseIndex(source, count, CCallHelpers::TimesEight, -(wordsBelowCount + 1) * bytesPerWord), second);
        jit.store64(word, BaseIndex(saved, count, CCallHelpers::TimesEight, SavedFrame::offsetOfWords() - (wordsBelowCount + 2) * bytesPerWord));
        jit.store64(second, BaseIndex(saved, count, CCallHelpers::TimesEight, SavedFrame::offsetOfWords() - (wordsBelowCount + 1) * bytesPerWord));
    };
    RELEASE_ASSERT(subject == X86Registers::esi && second == X86Registers::edi && count == X86Registers::ecx);
    RELEASE_ASSERT(noOverlap(subject, saved, count, source, word, second));
    jit.getEffectiveAddress(BaseIndex(sp, word, CCallHelpers::TimesEight, offsetOfBottom), source);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
    Jump isSmall = jit.branch32(CCallHelpers::Below, count, TrustedImm32(SavedFrame::minWordsForStringMove));
    jit.move(subject, word);
    jit.move(source, X86Registers::esi);
    jit.addPtr(TrustedImm32(SavedFrame::offsetOfWords()), saved, X86Registers::edi);
    jit.x86RepeatMoveString64();
    jit.move(word, subject);
    Jump isMoved = jit.jump();
    isSmall.link(&jit);
    Jump isMultipleOfFour = jit.branchTest32(CCallHelpers::Zero, count, TrustedImm32(2));
    copyPair(0);
    Jump isPair = jit.branchSub32(CCallHelpers::Zero, TrustedImm32(2), count);
    isMultipleOfFour.link(&jit);
    Label next = jit.label();
    copyPair(0);
    copyPair(2);
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(4), count).linkTo(next, &jit);
    isPair.link(&jit);
    isEmpty.link(&jit);
    isMoved.link(&jit);
#elif CPU(ARM64)
    const auto [destination, firstVector, secondVector] = scratch;
    RELEASE_ASSERT(noOverlap(subject, saved, count, source, destination, word, second));
    jit.move(sp, source);
    jit.addLeftShift64(source, word, TrustedImm32(3), source);
    jit.addPtr(TrustedImm32(SavedFrame::offsetOfWords()), saved, destination);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
    Jump isMultipleOfFour = jit.branchTest32(CCallHelpers::Zero, count, TrustedImm32(2));
    jit.loadPair64(CCallHelpers::PostIndexAddress(source, 2 * sizeof(uint64_t)), word, second);
    jit.storePair64(word, second, CCallHelpers::PostIndexAddress(destination, 2 * sizeof(uint64_t)));
    Jump isMoved = jit.branchSub32(CCallHelpers::Zero, TrustedImm32(2), count);
    isMultipleOfFour.link(&jit);
    Label next = jit.label();
    jit.loadPairVector(CCallHelpers::PostIndexAddress(source, 4 * sizeof(uint64_t)), firstVector, secondVector);
    jit.storePairVector(firstVector, secondVector, CCallHelpers::PostIndexAddress(destination, 4 * sizeof(uint64_t)));
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(4), count).linkTo(next, &jit);
    isMoved.link(&jit);
    isEmpty.link(&jit);
#endif
}

static void emitSaveRegisterAndResumeAddress(StubAssembler& jit, GPRReg saved, GPRReg scratch)
{
#if CPU(X86_64)
    jit.store64(SavedFrame::preservedRegister, Address(saved, SavedFrame::offsetOfPreservedRegister()));
#endif
    jit.loadCallerReturnAddress(0, scratch);
    jit.storePtr(scratch, Address(saved, SavedFrame::offsetOfResumeAddress()));
}

static void emitSaveLiveSlotMapOffset(StubAssembler& jit, GPRReg siteWord, GPRReg saved, GPRReg scratch)
{
    jit.and32(TrustedImm32((1 << SavedFrame::liveSlotMapOffsetBits) - 1), siteWord, scratch);
    jit.add32(TrustedImm32(1), scratch);
    jit.store32(scratch, Address(saved, SavedFrame::offsetOfLiveSlotMapOffsetPlusOne()));
}

static void emitSaveNewFrame(StubAssembler& jit, const FrameCopyRegisters& registers, const FrameCopyScratch& scratch, GPRReg siteWord)
{
    emitCopyFrameWords(jit, registers, scratch);
    emitSaveRegisterAndResumeAddress(jit, registers.saved, registers.word);
    emitSaveLiveSlotMapOffset(jit, siteWord, registers.saved, registers.word);
    fenceAfterAllocation(jit);
}

enum class FrameMayLieOnHeap : bool { No, Yes };
static void emitSaveFrame(StubAssembler& jit, const FrameCopyRegisters& registers, const FrameCopyScratch& scratch, GPRReg siteWord, FrameMayLieOnHeap mayLieOnHeap, uint8_t flags, uint8_t flagsKept = 0)
{
    const GPRReg saved = registers.saved;
    const GPRReg word = registers.word;
    const Address version(saved, SavedFrame::offsetOfVersion());
    jit.add32(TrustedImm32(1), version);
    jit.storeFence();
    Jump liesOnHeap;
    if (mayLieOnHeap == FrameMayLieOnHeap::Yes)
        liesOnHeap = jit.branchTest8(CCallHelpers::NonZero, Address(saved, SavedFrame::offsetOfNumberOfSavedRegisterPairs()), TrustedImm32(SavedFrame::liesOnHeapBit));
    emitCopyFrameWords(jit, registers, scratch);
    emitSaveRegisterAndResumeAddress(jit, saved, word);
    if (mayLieOnHeap == FrameMayLieOnHeap::Yes)
        liesOnHeap.link(&jit);
    if (flagsKept) {
        jit.load8(Address(saved, SavedFrame::offsetOfFlags()), word);
        jit.and32(TrustedImm32(flagsKept), word);
        if (flags)
            jit.or32(TrustedImm32(flags), word);
        jit.store8(word, Address(saved, SavedFrame::offsetOfFlags()));
    } else
        jit.store8(TrustedImm32(flags), Address(saved, SavedFrame::offsetOfFlags()));
    emitSaveLiveSlotMapOffset(jit, siteWord, saved, word);
    jit.storeFence();
    jit.add32(TrustedImm32(1), version);
}

static void emitSubtractNumberOfWordsSkippedAtTop(StubAssembler& jit, GPRReg siteWord, GPRReg number, GPRReg scratch)
{
#if CPU(X86_64)
    constexpr uint32_t bothRegisters = SavedFrame::functionSavesPreservedRegister | SavedFrame::functionSavesR15;
    static_assert(SavedFrame::skippedAtTopOf(bothRegisters) == 2 && !SavedFrame::skippedAtTopOf(SavedFrame::functionSavesPreservedRegister) && !SavedFrame::skippedAtTopOf(SavedFrame::functionSavesR15) && !SavedFrame::skippedAtTopOf(0));
    jit.and32(TrustedImm32(bothRegisters), siteWord, scratch);
    Jump savesFewer = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(bothRegisters));
    jit.sub32(TrustedImm32(2), number);
    savesFewer.link(&jit);
#elif CPU(ARM64)
    static_assert(SavedFrame::skippedAtTopOf(3u << SavedFrame::liveSlotMapOffsetBits) == 6 && SavedFrame::skippedAtTopOf(1u << SavedFrame::liveSlotMapOffsetBits) == 2);
    extractBits(jit, siteWord, SavedFrame::liveSlotMapOffsetBits, SavedFrame::savedRegisterBits, scratch);
    jit.lshift32(TrustedImm32(1), scratch);
    jit.sub32(scratch, number);
#endif
}

AssemblyHelpers::FreeListRefill emitAllocateSavedFrame(StubAssembler& jit, CoroutineKind kind, GPRReg siteWord, const NewSavedFrameRegisters& registers, JumpList& needsOperation)
{
    using Continuation = SavedFrame::Continuation;
    const auto [saved, count, allocator, scratch] = registers;
    const GPRReg skippedAtBottom = scratch;
    static_assert(hasOneBitSet(MarkedSpace::sizeStep) && sizeof(Allocator) == sizeof(void*));
    static_assert(JSObject::butterflyOffset() == sizeof(uint64_t));
    static_assert(SavedFrame::offsetOfNumberOfWordsSkippedAtBottom() == SavedFrame::offsetOfNumberOfWords() + 2);
    static_assert(SavedFrame::offsetOfVersion() == SavedFrame::offsetOfNumberOfWords() + 4 && !(SavedFrame::offsetOfNumberOfWords() % sizeof(uint64_t)));
    static_assert(SavedFrame::offsetOfContinuation() == SavedFrame::offsetOfKind() + 1 && SavedFrame::offsetOfNumberOfSavedRegisterPairs() == SavedFrame::offsetOfKind() + 2 && SavedFrame::offsetOfFlags() == SavedFrame::offsetOfKind() + 3);
    static_assert(SavedFrame::offsetOfPromiseOrWaiter() == SavedFrame::offsetOfResumeAddress() + 8 && SavedFrame::offsetOfAsyncContext() == SavedFrame::offsetOfPreservedRegister() + 8);
    static_assert(!static_cast<unsigned>(Continuation::None) && !JSValue::ValueEmpty);
    RELEASE_ASSERT(noOverlap(saved, count, allocator, scratch, siteWord, instanceGPR));

#if CPU(X86_64)
    jit.addPtr(TrustedImm32(-static_cast<int32_t>(sizeof(void*))), fp, count);
    jit.sub64(sp, count);
#else
    jit.move(sp, scratch);
    jit.sub64(fp, scratch, count);
#endif
    jit.urshift64(TrustedImm32(3), count);
    emitSubtractNumberOfWordsSkippedAtTop(jit, siteWord, count, saved);
    extractBits(jit, siteWord, SavedFrame::skippedPairsShift, SavedFrame::skippedPairsBits, skippedAtBottom);
    Jump skipsManyPairs = jit.branch32(CCallHelpers::Equal, skippedAtBottom, TrustedImm32(SavedFrame::manySkippedPairs));
    Label hasSkippedPairs = jit.label();
    jit.addOutOfLinePart([skipsManyPairs, hasSkippedPairs, siteWord, skippedAtBottom](StubAssembler& jit) {
        skipsManyPairs.link(&jit);
        jit.urshift64(siteWord, TrustedImm32(SavedFrame::manySkippedPairsShift), skippedAtBottom);
        jit.and32(TrustedImm32(UINT16_MAX), skippedAtBottom);
        jit.jump().linkTo(hasSkippedPairs, &jit);
    });
    jit.lshift32(TrustedImm32(1), skippedAtBottom);
    jit.sub32(skippedAtBottom, count);
    needsOperation.append(jit.branch32(CCallHelpers::Above, count, TrustedImm32(SavedFrame::maxWordsAllocatedInStub)));

    static_assert(MarkedSpace::sizeStep == 2 * sizeof(uint64_t) && !(sizeof(SavedFrame) % MarkedSpace::sizeStep));
    constexpr ptrdiff_t offsetOfAllocatorWithoutWords = CompleteSubspace::offsetOfAllocatorForSizeStep() + sizeof(SavedFrame) / MarkedSpace::sizeStep * sizeof(Allocator);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, count, CCallHelpers::TimesFour, offsetOfAllocatorWithoutWords), allocator);
    jit.lshift32(TrustedImm32(16), skippedAtBottom);
    jit.or32(skippedAtBottom, count);
    needsOperation.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    const AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, saved, allocator, scratch, needsOperation);

    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader)), scratch);
#if CPU(ARM64)
    jit.storePair64(scratch, ARM64Registers::zr, saved, TrustedImm32(0));
    jit.storePair64(ARM64Registers::zr, saved, saved, TrustedImm32(SavedFrame::offsetOfResumeAddress()));
    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, saved, TrustedImm32(SavedFrame::offsetOfPreservedRegister()));
#else
    jit.store64(scratch, Address(saved));
    jit.store64(TrustedImm32(0), Address(saved, JSObject::butterflyOffset()));
    jit.store64(TrustedImm32(0), Address(saved, SavedFrame::offsetOfResumeAddress()));
    jit.store64(saved, Address(saved, SavedFrame::offsetOfPromiseOrWaiter()));
    jit.store64(TrustedImm32(0), Address(saved, SavedFrame::offsetOfAsyncContext()));
#endif
    jit.store64(count, Address(saved, SavedFrame::offsetOfNumberOfWords()));
    static_assert(SavedFrame::offsetOfKind() == SavedFrame::offsetOfLiveSlotMapOffsetPlusOne() + 4 && !(SavedFrame::offsetOfLiveSlotMapOffsetPlusOne() % sizeof(uint64_t)) && SavedFrame::liveSlotMapOffsetBits >= 16);
    jit.and32(TrustedImm32(((1 << SavedFrame::savedRegisterBits) - 1) << SavedFrame::liveSlotMapOffsetBits), siteWord, scratch);
    if (static_cast<int32_t>(kind)) {
        jit.urshift32(TrustedImm32(SavedFrame::liveSlotMapOffsetBits - 16), scratch);
        jit.or32(TrustedImm32(static_cast<int32_t>(kind)), scratch);
        jit.lshift64(TrustedImm32(32), scratch);
    } else
        jit.lshift64(TrustedImm32(32 + 16 - SavedFrame::liveSlotMapOffsetBits), scratch);
    jit.store64(scratch, Address(saved, SavedFrame::offsetOfLiveSlotMapOffsetPlusOne()));

    if (kind == CoroutineKind::AsyncFunction) {
        const GPRReg promise = scratch;
        extractBits(jit, siteWord, SavedFrame::promiseLocationShift, SavedFrame::promiseLocationBits, promise);
#if CPU(X86_64)
        static_assert(!SavedFrame::locationInRegister);
        Jump isInRegister = jit.branchNeg64(CCallHelpers::Zero, promise);
#else
        jit.neg64(promise);
#endif
        jit.load64(BaseIndex(fp, promise, CCallHelpers::TimesEight), promise);
#if CPU(X86_64)
        Jump isLoaded = jit.jump();
        isInRegister.link(&jit);
        jit.move(SavedFrame::preservedRegister, promise);
        isLoaded.link(&jit);
#endif
        Jump hasNoPromise = jit.branchIfNotCell(promise);
        jit.storePtr(promise, Address(saved, SavedFrame::offsetOfPromiseOrWaiter()));
        jit.store8(TrustedImm32(static_cast<int32_t>(Continuation::Promise)), Address(saved, SavedFrame::offsetOfContinuation()));
        hasNoPromise.link(&jit);
    }
    fenceAfterAllocation(jit);
    return refill;
}

static void emitBarrierPreservingOperands(StubAssembler& jit, GPRReg owner, GPRReg firstStubOperand, GPRReg siteWord, GPRReg scratch)
{
    RELEASE_ASSERT(firstStubOperand == gprOf(firstStubOperandRegister));
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, owner, scratch);
    Label afterBarrier = jit.label();
    jit.addOutOfLinePart([needsBarrier, afterBarrier, owner, firstStubOperand, siteWord](StubAssembler& jit) {
        needsBarrier.link(&jit);
        jit.emitFunctionPrologue();
        jit.preserving({ firstStubOperand, siteWord }, [&] {
            jit.move(owner, firstStubOperand);
            jit.callStub(writeBarrier().withCallerFrame(CallerFrame::KeepsNoFPRs));
        });
        jit.emitFunctionEpilogue();
        jit.jump().linkTo(afterBarrier, &jit);
    });
}

static void emitResumeWhenPromiseSettles(StubAssembler& jit, const AwaitRegisters& registers, JumpList& takesNoInlineReaction, JumpList& needsOperation, uint64_t reactionBits = resumeReactionBits)
{
    const auto [savedFrame, promise, word, other] = registers;
    emitBranchUnlessHolds(jit, awaitedTakesInlineReaction, registers, takesNoInlineReaction);
    jit.move(TrustedImm64(static_cast<int64_t>(reactionBits)), other);
    jit.or64(other, word);
    Jump capturesNothing = emitBranchIfTracksNoAsyncContext(jit, other);
    emitLoadCurrentAsyncContext(jit, other);
    Jump isCell = jit.branchIfCell(other);
    needsOperation.append(jit.branch64(CCallHelpers::NotEqual, other, TrustedImm32(JSValue::ValueUndefined)));
    Jump isUndefined = jit.jump();
    isCell.link(&jit);
    jit.or64(other, word);
    jit.or64(TrustedImm64(static_cast<int64_t>(static_cast<uint64_t>(JSPromise::inlineReactionAsyncContextFlag) << promiseFlagsShift)), word);
    isUndefined.link(&jit);
    capturesNothing.link(&jit);
    jit.store64(savedFrame, Address(promise, JSPromise::offsetOfSlot()));
    jit.store64(word, Address(promise, JSPromise::offsetOfPacked()));
}

static void emitResumeWhenCallSettles(StubAssembler& jit, const AwaitRegisters& registers, JumpList& isResultOfCallee, JumpList& takesNoInlineReaction, JumpList& needsOperation)
{
    using Continuation = SavedFrame::Continuation;
    const auto [waiter, awaited, word, other] = registers;
    const Address continuation(awaited, SavedFrame::offsetOfContinuation());

    jit.load32(Address(awaited, JSCell::structureIDOffset()), word);
    Jump isPromise = jit.branch32(CCallHelpers::Equal, word, Address(instanceGPR, offsetOfPromiseHeader));
    isResultOfCallee.append(jit.branch32(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
    Jump hasNoContinuation = jit.branch8(CCallHelpers::Equal, continuation, TrustedImm32(static_cast<int32_t>(Continuation::None)));
    needsOperation.append(jit.branch8(CCallHelpers::NotEqual, continuation, TrustedImm32(static_cast<int32_t>(Continuation::Promise))));
    jit.loadPtr(Address(awaited, SavedFrame::offsetOfPromiseOrWaiter()), awaited);

    isPromise.link(&jit);
    emitResumeWhenPromiseSettles(jit, registers, takesNoInlineReaction, needsOperation);
    Jump isRegistered = jit.jump();

    hasNoContinuation.link(&jit);
    Jump capturesNothing = emitBranchIfTracksNoAsyncContext(jit, word);
    emitLoadCurrentAsyncContext(jit, word);
    jit.store64(word, Address(waiter, SavedFrame::offsetOfAsyncContext()));
    Jump hasCaptured = jit.jump();
    capturesNothing.link(&jit);
    jit.store64(TrustedImm64(JSValue::ValueUndefined), Address(waiter, SavedFrame::offsetOfAsyncContext()));
    hasCaptured.link(&jit);
    jit.storePtr(waiter, Address(awaited, SavedFrame::offsetOfPromiseOrWaiter()));
    jit.store8(TrustedImm32(static_cast<int32_t>(Continuation::Waiter)), continuation);
    isRegistered.link(&jit);
}

#if CPU(X86_64)
static void emitAwaitSettledValueInJob(StubAssembler& jit, const AwaitRegisters& registers, GPRReg siteWord, GPRReg result, GPRReg count, GPRReg second, uint8_t flagsKept, Label returnToCallerOfFunction, JumpList& otherwise)
{
    const auto [savedFrame, awaited, word, other] = registers;
    const JobRegisters job { count, word, other, result };
    const GPRReg value = second;
    const GPRReg jobsThatWait = result;
    RELEASE_ASSERT(noOverlap(savedFrame, awaited, word, other, siteWord, result, count, second, instanceGPR));

    otherwise.append(branchIfBarrierIsNeeded(jit, savedFrame, word));
    jit.move(awaited, value);
    Jump isNotCell = jit.branchIfNotCell(awaited);
    jit.load32(Address(awaited, JSCell::structureIDOffset()), word);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, word, Address(instanceGPR, offsetOfPromiseHeader)));
    constexpr unsigned bitsPerByte = 8;
    static_assert(!(promiseFlagsShift % bitsPerByte) && awaitedIsFulfilled.bits == static_cast<uint64_t>(JSPromise::Status::Fulfilled) << promiseFlagsShift);
    otherwise.append(jit.branchTest8(CCallHelpers::Zero, Address(awaited, JSPromise::offsetOfPacked() + promiseFlagsShift / bitsPerByte), TrustedImm32(static_cast<int32_t>(JSPromise::Status::Fulfilled))));
    jit.load64(Address(awaited, JSPromise::offsetOfSlot()), value);
    isNotCell.link(&jit);

    emitReserveJob(jit, job, otherwise);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.resumeJobWords)), result);
    jit.store64(result, Address(job.slot, QueuedTask::offsetOfDispatcher()));
    jit.move(TrustedImm64(JSValue::ValueUndefined), result);
    jit.store64(result, argumentOfJob(job.slot, 0));
    jit.store64(value, argumentOfJob(job.slot, 1));
    jit.store64(savedFrame, argumentOfJob(job.slot, 2));
    jit.store64(result, argumentOfJob(job.slot, 3));
    Jump tracksNoContext = emitBranchIfTracksNoAsyncContext(jit, result);
    emitLoadCurrentAsyncContext(jit, result);
    jit.store64(result, argumentOfJob(job.slot, 3));
    tracksNoContext.link(&jit);
    jit.store64(job.next, Address(job.queue, MicrotaskQueue::offsetOfEnd()));
    jit.move(job.next, jobsThatWait);
    jit.sub64(Address(job.queue, MicrotaskQueue::offsetOfStart()), jobsThatWait);

    emitCopyFrameWords(jit, { awaited, savedFrame, count, word, other, second }, { });
    emitSaveRegisterAndResumeAddress(jit, savedFrame, word);
    if (flagsKept) {
        jit.load8(Address(savedFrame, SavedFrame::offsetOfFlags()), word);
        jit.and32(TrustedImm32(flagsKept), word);
        jit.store8(word, Address(savedFrame, SavedFrame::offsetOfFlags()));
    } else
        jit.store8(TrustedImm32(0), Address(savedFrame, SavedFrame::offsetOfFlags()));
    emitSaveLiveSlotMapOffset(jit, siteWord, savedFrame, word);
    emitMoveRegistersSavedInFrame(jit, siteWord, SavedFrame::liveSlotMapOffsetBits, RegistersSavedInFrame::AreRestored);
    Jump fewJobsWait = jit.branch64(CCallHelpers::BelowOrEqual, jobsThatWait, TrustedImm32(jobsWorthUnexpectedReturns));
    jit.emitFunctionEpilogue();
    jit.addPtr(TrustedImm32(sizeof(void*)), sp);
    jit.jumpToStub(resumeNextJob());

    fewJobsWait.link(&jit);
    jit.loadPtr(Address(savedFrame, SavedFrame::offsetOfPromiseOrWaiter()), result);
    emitReturnToCallerOfFunction(jit, other, returnToCallerOfFunction);
}
#endif

StubSignature signatureOfAwaitBySavingFrame(StubKey key)
{
    switch (static_cast<AwaitedOperand>(key.form())) {
    case AwaitedOperand::Value:
    case AwaitedOperand::ValueInModuleBody:
    case AwaitedOperand::Resolution:
        return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::T9) }).fillingOperandInFront(0);
    case AwaitedOperand::ResultOfCallWithoutPromise:
        return StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::T9) });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfAwaitWithSavedFrame(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::T9) }); }

struct AwaitOperations {
    Entry withNewSavedFrame;
    Entry withSavedFrame;
};

static AwaitOperations operationsOf(AwaitedOperand form)
{
    switch (form) {
    case AwaitedOperand::Value:
        return { Entry::operationAOTSaveFrameAndAwait, Entry::operationAOTAwaitWithSavedFrame };
    case AwaitedOperand::ResultOfCallWithoutPromise:
        return { Entry::operationAOTSaveFrameAndAwaitCall, Entry::operationAOTAwaitCallWithSavedFrame };
    case AwaitedOperand::ValueInModuleBody:
        return { Entry::operationAOTSaveFrameAndAwaitInModuleBody, Entry::operationAOTAwaitWithSavedFrameInModuleBody };
    case AwaitedOperand::Resolution:
        return { Entry::operationAOTSaveFrameAndAwaitResolution, Entry::operationAOTAwaitResolutionWithSavedFrame };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitReturnAwaitedValue(StubAssembler& jit, GPRReg siteWord)
{
    static_assert(!static_cast<int32_t>(AsyncGeneratorStepStatus::Result));
    RELEASE_ASSERT(siteWord == gprOf(resumeModeRegister));
    jit.move(TrustedImm32(static_cast<int32_t>(AsyncGeneratorStepStatus::Result)), siteWord);
    emitSiteContinuationNote(jit);
    jit.returnToCaller();
}

void generateAwaitBySavingFrame(StubAssembler& jit)
{
    const auto form = static_cast<AwaitedOperand>(jit.key().form());
    const bool awaitsCallWithoutPromise = form == AwaitedOperand::ResultOfCallWithoutPromise;
    const bool awaitsResolution = form == AwaitedOperand::Resolution;
    const bool queuesJobInOperation = form == AwaitedOperand::ValueInModuleBody || awaitsResolution;
    const AwaitOperations operations = operationsOf(form);
    const GPRReg result = jit.result();
    const GPRReg awaited = awaitsCallWithoutPromise ? jit.allocateScratch(StubRegister::A1) : jit.operand(0);
    const GPRReg siteWord = jit.clobberOperand(1);
    const GPRReg savedFrame = jit.allocateScratch(StubRegister::A2);
    const GPRReg count = jit.allocateScratch(StubRegister::A3);
    const GPRReg word = jit.allocateScratch(StubRegister::A4);
    const GPRReg other = jit.allocateScratch(StubRegister::A5);
    const GPRReg second = jit.allocateScratch(secondRegisterOfFrameCopy);
    const AwaitRegisters registers { savedFrame, awaited, word, other };
    RELEASE_ASSERT(awaited == operationArgumentGPR(1) && savedFrame == operationArgumentGPR(2));

    JumpList mustSuspend;
    JumpList isOtherFrame;
    JumpList isOtherObject;
    JumpList queueHasJobs;
    JumpList queueHasJobsUnderCallers;
    Label mayContinue;
    Label mayContinueUnderCallers;
    const AwaitCondition& queueIsEmpty = awaitConditionsOnState[1];
    if (awaitsCallWithoutPromise)
        jit.move(result, awaited);
    if (form != AwaitedOperand::ValueInModuleBody) {
        emitBranchUnlessHolds(jit, awaitConditionsOnState[0], registers, isOtherFrame);
        mayContinue = jit.label();
        emitBranchUnlessHolds(jit, queueIsEmpty, registers, queueHasJobs);
    }
    switch (form) {
    case AwaitedOperand::Value:
        jit.move(awaited, result);
        emitLoadSettledValue(jit, registers, result, isOtherObject, mustSuspend);
        emitReturnAwaitedValue(jit, siteWord);
        break;
    case AwaitedOperand::ResultOfCallWithoutPromise:
        emitLoadSettledValue(jit, registers, result, isOtherObject, mustSuspend);
        emitSiteContinuationNote(jit);
        jit.returnToCaller();
        break;
    case AwaitedOperand::ValueInModuleBody:
        mustSuspend.append(jit.jump());
        break;
    case AwaitedOperand::Resolution:
        jit.move(awaited, result);
        emitLoadSettledResolution(jit, registers, result, isOtherObject, mustSuspend);
        emitReturnAwaitedValue(jit, siteWord);
        break;
    }

    Label returnToCallerOfFunction = emitFunctionReturn(jit);

    isOtherObject.link(&jit);
    if (awaitsCallWithoutPromise) {
        mustSuspend.append(jit.branch32(CCallHelpers::Equal, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
        emitSiteContinuationNote(jit);
        jit.returnToCaller();
    }
    if (form == AwaitedOperand::Value)
        mustSuspend.append(jit.branch32(CCallHelpers::Equal, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
    if (form == AwaitedOperand::Value || awaitsResolution) {
        emitBranchIfMayBeThenable(jit, awaited, word, other, mustSuspend);
        emitReturnAwaitedValue(jit, siteWord);
    }

    if (form != AwaitedOperand::ValueInModuleBody) {
        static_assert(mustSuspendAtNextAwaitBit == 1);
        isOtherFrame.link(&jit);
        jit.loadPtr(resumableFrameAddress, word);
        mustSuspend.append(jit.branchTestPtr(CCallHelpers::Zero, word));
        if (awaitsCallWithoutPromise) {
            const GPRReg frameOfCallee = savedFrame;
            jit.subPtr(sp, TrustedImm32(static_cast<int32_t>(prologueStackPointerDelta())), other);
            jit.move(word, frameOfCallee);
            jit.andPtr(TrustedImm32(~mustSuspendAtNextAwaitBit), frameOfCallee);
            Jump calleeDidNotResume = jit.branchPtr(CCallHelpers::NotEqual, frameOfCallee, other);
            Jump tracksNone = emitBranchIfTracksNoAsyncContext(jit, other);
            emitStoreCurrentAsyncContext(jit, TrustedImm64(JSValue::ValueUndefined), other);
            tracksNone.link(&jit);
            jit.andPtr(TrustedImm32(mustSuspendAtNextAwaitBit), word);
            jit.orPtr(fp, word);
            jit.storePtr(word, resumableFrameAddress);
            jit.branchPtr(CCallHelpers::Equal, word, fp).linkTo(mayContinue, &jit);
            mustSuspend.append(jit.jump());
            calleeDidNotResume.link(&jit);
        }

        JumpList isForeignFrameObject;
        mayContinueUnderCallers = jit.label();
        emitBranchUnlessHolds(jit, queueIsEmpty, registers, queueHasJobsUnderCallers);
        if (!awaitsCallWithoutPromise)
            jit.move(awaited, result);
        if (awaitsResolution)
            emitLoadSettledResolution(jit, registers, result, isForeignFrameObject, mustSuspend);
        else
            emitLoadSettledValue(jit, registers, result, isForeignFrameObject, mustSuspend);
        Label isSettled = jit.label();
        emitBranchUnlessCallersAwait(jit, { savedFrame, count, other, word }, mustSuspend);
        jit.storePtr(fp, resumableFrameAddress);
        if (awaitsCallWithoutPromise) {
            emitSiteContinuationNote(jit);
            jit.returnToCaller();
        } else
            emitReturnAwaitedValue(jit, siteWord);

        isForeignFrameObject.link(&jit);
        if (!awaitsResolution)
            mustSuspend.append(jit.branch32(CCallHelpers::Equal, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
        if (!awaitsCallWithoutPromise)
            emitBranchIfMayBeThenable(jit, awaited, word, other, mustSuspend);
        jit.jump().linkTo(isSettled, &jit);
    }

    if (!queueHasJobs.empty()) {
        queueHasJobs.link(&jit);
        emitBranchUnlessQueueMaySettle(jit, awaited, mustSuspend);
        emitSettleAwaitedPromise(jit, awaited, siteWord, mustSuspend);
        jit.jump().linkTo(mayContinue, &jit);
    }
    if (!queueHasJobsUnderCallers.empty()) {
        queueHasJobsUnderCallers.link(&jit);
        emitBranchUnlessQueueMaySettle(jit, awaited, mustSuspend);
        emitBranchUnlessCallersAwait(jit, { savedFrame, count, other, word }, mustSuspend);
        emitSettleAwaitedPromise(jit, awaited, siteWord, mustSuspend);
        jit.jump().linkTo(mayContinueUnderCallers, &jit);
    }

    mustSuspend.link(&jit);
    jit.loadPtr(Address(fp, sizeof(void*)), word);
    Jump isFirstSave = jit.branchPtr(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(StubFrameKind::SavedFrameAdapter)));
    jit.loadPtr(Address(fp), word);
    jit.loadPtr(Address(word, static_cast<int>(CallFrameSlot::callee) * static_cast<int>(sizeof(Register))), savedFrame);
#if CPU(X86_64)
    if (!queuesJobInOperation) {
        JumpList otherwise;
        emitAwaitSettledValueInJob(jit, { savedFrame, awaited, word, other }, siteWord, result, count, second, form == AwaitedOperand::Value ? SavedFrame::goesOnInContextOfItsAwaitFlag : 0, returnToCallerOfFunction, otherwise);
        otherwise.link(&jit);
    }
#endif
    if (form == AwaitedOperand::Value) {
        checkStubOperands(awaitWithSavedFrame(), { awaited, savedFrame, siteWord });
        jit.bindEntry(awaitWithSavedFrame());
    }
    const FrameCopyScratch copyScratch = allocateFrameCopyScratch(jit);
    emitSaveFrame(jit, { awaited, savedFrame, count, word, other, second }, copyScratch, siteWord, FrameMayLieOnHeap::No, 0, form == AwaitedOperand::Value ? SavedFrame::goesOnInContextOfItsAwaitFlag : 0);
    Label isSaved = jit.label();

    JumpList needsOperation;
    if (!queuesJobInOperation) {
        JumpList isResumedByJob;
        JumpList isResumedByOthers;
        JumpList isOtherObjectInSavedFrame;
        JumpList takesNoInlineReaction;
        const GPRReg promise = awaitsCallWithoutPromise ? second : awaited;
        isResumedByJob.append(jit.branchIfNotCell(awaited));
        if (awaitsCallWithoutPromise) {
            jit.move(awaited, second);
            emitResumeWhenCallSettles(jit, { savedFrame, second, word, other }, isResumedByJob, takesNoInlineReaction, needsOperation);
            emitBarrierPreservingOperands(jit, second, result, siteWord, word);
        } else {
            Jump isObject = jit.branchIfObject(awaited);
            isResumedByOthers.append(jit.branchIfType(awaited, SentinelType));
            isResumedByJob.append(jit.jump());
            isObject.link(&jit);
            emitBranchUnlessHolds(jit, awaitedHasPromiseStructure, registers, isOtherObjectInSavedFrame);
            emitResumeWhenPromiseSettles(jit, registers, takesNoInlineReaction, needsOperation);
            emitBarrierPreservingOperands(jit, awaited, result, siteWord, word);
        }
        isResumedByOthers.link(&jit);
        Label isRegistered = jit.label();
        emitBarrierPreservingOperands(jit, savedFrame, result, siteWord, word);
        jit.loadPtr(Address(savedFrame, SavedFrame::offsetOfPromiseOrWaiter()), result);
        emitLeaveFunctionOfJob(jit, siteWord, other, word, returnToCallerOfFunction);

        isResumedByJob.link(&jit);
        Label queuesJob = jit.label();
        const JobRegisters job { count, word, other, second };
        emitReserveJob(jit, job, needsOperation);
        Label fillsJob = jit.label();
        emitFillJob(jit, { InternalMicrotask::CompiledAsyncFunctionResume, JSPromise::Status::Fulfilled, awaited, savedFrame, JobAsyncContext::Current }, job);
        jit.jump().linkTo(isRegistered, &jit);

        RELEASE_ASSERT(noOverlap(result, awaited, savedFrame, siteWord, count, word, other, second));
        takesNoInlineReaction.link(&jit);
        emitBranchUnlessHolds(jit, awaitedIsFulfilled, { savedFrame, promise, word, other }, needsOperation);
        jit.load64(Address(promise, JSPromise::offsetOfSlot()), result);
        emitReserveJob(jit, job, needsOperation);
        jit.move(result, awaited);
        jit.jump().linkTo(fillsJob, &jit);

        if (!isOtherObjectInSavedFrame.empty()) {
            isOtherObjectInSavedFrame.link(&jit);
            Jump isFrameOfCallee = jit.branch32(CCallHelpers::Equal, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader)));
            emitBranchIfMayBeThenable(jit, awaited, word, other, needsOperation);
            jit.jump().linkTo(queuesJob, &jit);

            isFrameOfCallee.link(&jit);
            jit.move(awaited, second);
            emitResumeWhenCallSettles(jit, { savedFrame, second, word, other }, needsOperation, needsOperation, needsOperation);
            emitBarrierPreservingOperands(jit, second, result, siteWord, word);
            jit.jump().linkTo(isRegistered, &jit);
        }
    }

    needsOperation.link(&jit);
    emitCallOperationAndLeaveFunction(jit, operations.withSavedFrame, siteWord, returnToCallerOfFunction);

    isFirstSave.link(&jit);
    JumpList createsInOperation;
    const AssemblyHelpers::FreeListRefill refill = emitAllocateSavedFrame(jit, CoroutineKind::AsyncFunction, siteWord, { savedFrame, count, word, other }, createsInOperation);
    emitSaveNewFrame(jit, { awaited, savedFrame, count, word, other, second }, copyScratch, siteWord);
    jit.jump().linkTo(isSaved, &jit);

    emitLateRefill(jit, refill, createsInOperation);
    createsInOperation.link(&jit);
    emitSuspend(jit, operations.withNewSavedFrame, siteWord, returnToCallerOfFunction);
}

void emitRunGeneratorJobWhenSettled(StubAssembler& jit, InternalMicrotask task, const AwaitRegisters& registers, const JobRegisters& job, std::optional<GPRReg> siteWord, JumpList& isLeftToEngine)
{
    const auto [generator, awaited, word, other] = registers;
    RELEASE_ASSERT(noOverlap(generator, awaited, job.queue, job.slot, job.next, job.scratch, instanceGPR));
    RELEASE_ASSERT(!siteWord || noOverlap(*siteWord, generator, awaited, job.queue, job.slot, job.next, job.scratch));

    JumpList isResumedByJob;
    isResumedByJob.append(jit.branchIfNotCell(awaited));
    Jump isObject = jit.branchIfObject(awaited);
    isLeftToEngine.append(jit.branchIfType(awaited, SentinelType));
    isResumedByJob.append(jit.jump());

    isObject.link(&jit);
    JumpList isOtherObject;
    emitBranchUnlessHolds(jit, awaitedHasPromiseStructure, registers, isOtherObject);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), other);
    isLeftToEngine.append(branchIfWatchpointSetIsNotWatched(jit, Address(other, OBJECT_OFFSETOF(JSGlobalObject, m_promiseSpeciesWatchpointSet)), other));
    if (!siteWord)
        isLeftToEngine.append(branchIfBarrierIsNeeded(jit, awaited, other));
    JumpList takesNoInlineReaction;
    emitResumeWhenPromiseSettles(jit, registers, takesNoInlineReaction, isLeftToEngine, inlineReactionBitsOf(task));
    if (siteWord)
        emitBarrierPreservingOperands(jit, awaited, awaited, *siteWord, word);
    Jump isRegistered = jit.jump();

    takesNoInlineReaction.link(&jit);
    emitBranchUnlessHolds(jit, awaitedIsFulfilled, registers, isLeftToEngine);
    emitReserveJob(jit, job, isLeftToEngine);
    jit.load64(Address(awaited, JSPromise::offsetOfSlot()), awaited);
    Jump fillsJob = jit.jump();

    isOtherObject.link(&jit);
    emitBranchIfMayBeThenable(jit, awaited, word, other, isLeftToEngine);
    isResumedByJob.link(&jit);
    emitReserveJob(jit, job, isLeftToEngine);
    fillsJob.link(&jit);
    emitFillJob(jit, { task, JSPromise::Status::Fulfilled, awaited, generator, JobAsyncContext::Current }, job);

    isRegistered.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.asyncGeneratorSentinel)), awaited);
}

[[nodiscard]] static Jump emitLoadSavedFrame(StubAssembler& jit, GeneratorSuspension form, GPRReg generator, GPRReg saved)
{
    switch (form) {
    case GeneratorSuspension::Yield:
        jit.loadPtr(Address(generator, Generator::offsetOfSavedFrame()), saved);
        return jit.branchTestPtr(CCallHelpers::Zero, saved);
    case GeneratorSuspension::AsyncAwait:
    case GeneratorSuspension::AsyncYield:
    case GeneratorSuspension::AsyncYieldAfterAwait:
        jit.load64(Address(generator, JSAsyncGenerator::offsetOfInternalField(static_cast<unsigned>(JSAsyncGenerator::Field::Frame))), saved);
        return jit.branchIfNotCell(saved);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitStoreSavedFrame(StubAssembler& jit, GeneratorSuspension form, GPRReg generator, GPRReg saved)
{
    switch (form) {
    case GeneratorSuspension::Yield:
        jit.storePtr(saved, Address(generator, Generator::offsetOfSavedFrame()));
        return;
    case GeneratorSuspension::AsyncAwait:
    case GeneratorSuspension::AsyncYield:
    case GeneratorSuspension::AsyncYieldAfterAwait:
        jit.store64(saved, Address(generator, JSAsyncGenerator::offsetOfInternalField(static_cast<unsigned>(JSAsyncGenerator::Field::Frame))));
        jit.store64(saved, Address(generator, JSAsyncGenerator::offsetOfInternalField(static_cast<unsigned>(JSAsyncGenerator::Field::Next))));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitStoreSuspendedState(StubAssembler& jit, GeneratorSuspension form, GPRReg generator)
{
    using Reason = JSAsyncGenerator::AsyncGeneratorSuspendReason;
    auto storeReason = [&](Reason reason) {
        const int32_t state = (1 << JSAsyncGenerator::reasonShift) | static_cast<int32_t>(reason);
        jit.store64(TrustedImm64(JSValue::encode(jsNumber(state))), Address(generator, JSAsyncGenerator::offsetOfInternalField(static_cast<unsigned>(JSAsyncGenerator::Field::State))));
    };
    switch (form) {
    case GeneratorSuspension::Yield:
        jit.store32(TrustedImm32(static_cast<int32_t>(Generator::State::SuspendedAtYield)), Address(generator, Generator::offsetOfState()));
        return;
    case GeneratorSuspension::AsyncAwait:
        storeReason(Reason::Await);
        return;
    case GeneratorSuspension::AsyncYield:
        storeReason(Reason::YieldNoAwait);
        return;
    case GeneratorSuspension::AsyncYieldAfterAwait:
        storeReason(Reason::Yield);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<CoroutineKind> kindOfGeneratorAtInitialYield(GeneratorSuspension form)
{
    switch (form) {
    case GeneratorSuspension::Yield:
        return CoroutineKind::Generator;
    case GeneratorSuspension::AsyncYield:
        return CoroutineKind::AsyncGenerator;
    case GeneratorSuspension::AsyncAwait:
    case GeneratorSuspension::AsyncYieldAfterAwait:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfSuspendGenerator(StubKey) { return signatureWithResumeMode({ gprOf(StubRegister::A1), gprOf(StubRegister::R0), gprOf(StubRegister::T9) }); }

void generateSuspendGenerator(StubAssembler& jit)
{
    const auto form = static_cast<GeneratorSuspension>(jit.key().form());
    const GPRReg generator = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg siteWord = jit.clobberOperand(2);
    const GPRReg saved = jit.allocateScratch(StubRegister::A2);
    const GPRReg count = jit.allocateScratch(StubRegister::A3);
    const GPRReg cursor = jit.allocateScratch(StubRegister::A4);
    const GPRReg word = jit.allocateScratch(StubRegister::A5);
    const GPRReg second = jit.allocateScratch(secondRegisterOfFrameCopy);
    RELEASE_ASSERT(generator == operationArgumentGPR(1));
    RELEASE_ASSERT(noOverlap(generator, value, siteWord, saved, count, cursor, word, second, instanceGPR));

    Label returnToCallerOfFunction = emitFunctionReturn(jit);
    jit.align();
    jit.bindEntry(jit.key());

    if (form == GeneratorSuspension::AsyncAwait) {
        const GPRReg awaited = saved;
        const AwaitRegisters registers { InvalidGPRReg, awaited, cursor, word };
        JumpList mustSuspend;
        emitBranchUnlessFrameMayContinue(jit, registers, mustSuspend);
        jit.move(value, awaited);
        emitLoadSettledValue(jit, registers, value, mustSuspend, mustSuspend);
        jit.load64(Address(generator, JSAsyncGenerator::offsetOfInternalField(static_cast<unsigned>(JSAsyncGenerator::Field::Frame))), cursor);
        jit.store8(TrustedImm32(SavedFrame::hasPassedAwaitOnStackFlag), Address(cursor, SavedFrame::offsetOfFlags()));
        emitReturnAwaitedValue(jit, siteWord);
        mustSuspend.link(&jit);
    }

    Jump isFirstSave = emitLoadSavedFrame(jit, form, generator, saved);
    emitStoreSuspendedState(jit, form, generator);
    Label hasSavedFrame = jit.label();
    const uint8_t keptAtAwait = form == GeneratorSuspension::AsyncAwait ? SavedFrame::goesOnInContextOfItsAwaitFlag : 0;
    emitSaveFrame(jit, { generator, saved, count, cursor, word, second }, allocateFrameCopyScratch(jit), siteWord, form == GeneratorSuspension::Yield ? FrameMayLieOnHeap::No : FrameMayLieOnHeap::Yes, form == GeneratorSuspension::AsyncAwait ? 0 : SavedFrame::isSuspendedAtYieldFlag, form == GeneratorSuspension::Yield ? 0 : SavedFrame::hasPassedAwaitOnStackFlag | keptAtAwait);
    emitBarrierPreservingOperands(jit, saved, value, siteWord, word);
    if (form == GeneratorSuspension::AsyncAwait) {
        JumpList isLeftToEngine;
        emitRunGeneratorJobWhenSettled(jit, InternalMicrotask::AsyncGeneratorBodyCallNormal, { generator, value, cursor, word }, { saved, count, cursor, word }, siteWord, isLeftToEngine);
        isLeftToEngine.link(&jit);
    }
    emitLeaveFunction(jit, siteWord, word, returnToCallerOfFunction);

    isFirstSave.link(&jit);
    if (form == GeneratorSuspension::Yield) {
        Jump isInitialYield = jit.branch32(CCallHelpers::NotEqual, Address(generator, Generator::offsetOfState()), TrustedImm32(static_cast<int32_t>(Generator::State::Running)));
        emitStoreSuspendedState(jit, form, generator);
        isInitialYield.link(&jit);
    }
    JumpList createsInOperation;
    if (std::optional<CoroutineKind> kind = kindOfGeneratorAtInitialYield(form)) {
        const AssemblyHelpers::FreeListRefill refill = emitAllocateSavedFrame(jit, *kind, siteWord, { saved, count, cursor, word }, createsInOperation);
        emitStoreSavedFrame(jit, form, generator, saved);
        emitBarrierPreservingOperands(jit, generator, value, siteWord, word);
        jit.jump().linkTo(hasSavedFrame, &jit);
        emitLateRefill(jit, refill, createsInOperation);
    }
    createsInOperation.link(&jit);
    emitSuspend(jit, Entry::operationAOTSaveFrameAndYield, siteWord, returnToCallerOfFunction, value);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
