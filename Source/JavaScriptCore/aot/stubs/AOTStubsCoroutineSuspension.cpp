/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCoroutines.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "AOTStubsBarriers.h"
#include "CallFrame.h"
#include "JSAsyncGenerator.h"
#include "JSCJSValueInlines.h"
#include "JSPromise.h"
#include "StackAlignment.h"

namespace JSC { namespace AOT {

struct AwaitRegisters {
    GPRReg savedFrame;
    GPRReg awaited;
    GPRReg word;
    GPRReg other;
};

static Address emitAddressOf(StubAssembler& jit, const AwaitConditionWord& word, const AwaitRegisters& registers)
{
    const int32_t offset = static_cast<int32_t>(word.offset);
    switch (word.base) {
    case AwaitConditionBase::SavedFrame:
        return Address(registers.savedFrame, offset);
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

// Leaves the tested word in registers.word after EqualsOther and HasNoBit, and after HasAnyBit on a word wider than a byte.
static void emitBranchUnlessHolds(StubAssembler& jit, const AwaitCondition& condition, const AwaitRegisters& registers, JumpList& doesNotHold)
{
    const auto [savedFrame, awaited, word, other] = registers;
    const unsigned sizeInBytes = condition.word.sizeInBytes;
    RELEASE_ASSERT(noOverlap(savedFrame, awaited, word, other, instanceGPR));
    switch (condition.test) {
    case AwaitConditionTest::HasAnyBit:
        if (sizeInBytes == 1) {
            RELEASE_ASSERT(condition.bits <= UINT8_MAX);
            doesNotHold.append(jit.branchTest8(CCallHelpers::Zero, emitAddressOf(jit, condition.word, registers), TrustedImm32(static_cast<int32_t>(condition.bits))));
            return;
        }
        emitLoadWord(jit, emitAddressOf(jit, condition.word, registers), sizeInBytes, word);
        jit.move(TrustedImm64(static_cast<int64_t>(condition.bits)), other);
        doesNotHold.append(jit.branchTest64(CCallHelpers::Zero, word, other));
        return;
    case AwaitConditionTest::HasNoBit:
        emitLoadWord(jit, emitAddressOf(jit, condition.word, registers), sizeInBytes, word);
        jit.move(TrustedImm64(static_cast<int64_t>(condition.bits)), other);
        doesNotHold.append(jit.branchTest64(CCallHelpers::NonZero, word, other));
        return;
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
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Expects a copy of the awaited value in result.
static void emitLoadSettledValue(StubAssembler& jit, const AwaitRegisters& registers, GPRReg result, JumpList& isOtherObject, JumpList& mustSuspend)
{
    const auto [savedFrame, awaited, word, other] = registers;
    static_assert(awaitConditionsOnObject.size() == 2 && awaitConditionsOnObject[0] == awaitedHasPromiseStructure && awaitConditionsOnObject[1] == awaitedIsFulfilled);
    RELEASE_ASSERT(noOverlap(result, awaited, word, other));
    Jump notCell = jit.branchIfNotCell(awaited);
    Jump isObject = jit.branchIfObject(awaited);
    // The sentinel says that a generator resumes the function.
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

static Label emitFunctionReturn(StubAssembler& jit)
{
    Label returnToCallerOfFunction = jit.label();
    jit.emitFunctionEpilogue();
    jit.ret();
    return returnToCallerOfFunction;
}

// Returns to the caller of the suspended function, so the callee saves reloaded here are not clobbers of the stub.
static void emitLeaveFunction(StubAssembler& jit, GPRReg siteWord, GPRReg scratch, Label returnToCallerOfFunction)
{
#if CPU(X86_64)
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    Jump doesNotSaveR15 = jit.branchTest32(CCallHelpers::Zero, siteWord, TrustedImm32(SavedFrame::functionSavesR15));
    jit.loadPtr(Address(fp, -static_cast<int>(sizeof(CPURegister))), X86Registers::r15);
    Jump savedOnlyR15 = jit.branchTest32(CCallHelpers::Zero, siteWord, TrustedImm32(SavedFrame::functionSavesPreservedRegister));
    jit.loadPtr(Address(fp, -2 * static_cast<int>(sizeof(CPURegister))), SavedFrame::preservedRegister);
    Jump hasRestoredBoth = jit.jump();
    doesNotSaveR15.link(&jit);
    Jump hasSavedNothing = jit.branchTest32(CCallHelpers::Zero, siteWord, TrustedImm32(SavedFrame::functionSavesPreservedRegister));
    jit.loadPtr(Address(fp, -static_cast<int>(sizeof(CPURegister))), SavedFrame::preservedRegister);
    hasSavedNothing.link(&jit);
    savedOnlyR15.link(&jit);
    hasRestoredBoth.link(&jit);
#else
    emitMovePairsSavedInFrame(jit, siteWord, SavedFrame::liveSlotMapOffsetBits, PairsSavedInFrame::AreRestored);
#endif
    // Returning through the label keeps calls and returns paired for the return address predictor.
    jit.loadLabelAddress(returnToCallerOfFunction, scratch);
#if CPU(X86_64)
    jit.storePtr(scratch, Address(sp));
    jit.ret();
#else
    jit.m_assembler.ret(scratch);
#endif
}

// Calls the operation with the instance as its first and the suspension words as its sixth argument. The others are already in their registers.
static void emitCallOperationAndLeaveFunction(StubAssembler& jit, Entry operation, GPRReg siteWord, Label returnToCallerOfFunction)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr int32_t offsetOfSiteWord = OBJECT_OFFSETOF(SuspensionWords, siteWord);
    const GPRReg instance = jit.reuseScratch(StubRegister::A0);
    const GPRReg suspensionWords = jit.reuseScratch(StubRegister::A5);
    const GPRReg scratch = suspensionWords;
    static_assert(sizeof(SuspensionWords) == stackAlignmentBytes());
    static_assert(!offsetOfSiteWord && OBJECT_OFFSETOF(SuspensionWords, preservedRegister) == sizeof(CPURegister));
    jit.emitFunctionPrologue();
#if CPU(X86_64)
    jit.push(SavedFrame::preservedRegister);
    jit.push(siteWord);
#else
    jit.subPtr(TrustedImm32(sizeof(SuspensionWords)), sp);
    jit.store64(siteWord, Address(sp, offsetOfSiteWord));
#endif
    jit.move(sp, suspensionWords);
    jit.move(instanceGPR, instance);
    jit.callOperation(operation, jit.reuseScratch(StubRegister::Operation));
    jit.load32(Address(sp, offsetOfSiteWord), siteWord);
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    emitLeaveFunction(jit, siteWord, scratch, returnToCallerOfFunction);
    exception.link(&jit);
    jit.jumpToExceptionHandler();
}

// Calls operation(instance, subject, frame, bottom, resumeAddress, suspensionWords). The subject, an awaited value or a generator, is already in its argument register.
static void emitSuspend(StubAssembler& jit, Entry operation, GPRReg siteWord, Label returnToCallerOfFunction)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
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
    emitCallOperationAndLeaveFunction(jit, operation, siteWord, returnToCallerOfFunction);
}

struct FrameCopyRegisters {
    GPRReg subject;
    GPRReg saved;
    GPRReg count;
    GPRReg cursor;
    GPRReg word;
    GPRReg second;
};

static void emitWillWrite(StubAssembler& jit, GPRReg saved)
{
    jit.add32(TrustedImm32(1), Address(saved, SavedFrame::offsetOfVersion()));
    jit.storeFence();
}

static void emitDidWrite(StubAssembler& jit, GPRReg saved)
{
    jit.storeFence();
    jit.add32(TrustedImm32(1), Address(saved, SavedFrame::offsetOfVersion()));
}

// The stack is as at the entry of the stub. The number of words is even and may be zero.
static void emitCopyFrameWords(StubAssembler& jit, const FrameCopyRegisters& registers)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    const auto [subject, saved, count, cursor, word, second] = registers;
    jit.load32(Address(saved, SavedFrame::offsetOfNumberOfWords()), count);
#if CPU(X86_64)
    constexpr int32_t offsetOfBottom = sizeof(void*);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
    RELEASE_ASSERT(subject == X86Registers::esi && second == X86Registers::edi && count == X86Registers::ecx);
    Jump isSmall = jit.branch32(CCallHelpers::Below, count, TrustedImm32(SavedFrame::minWordsForStringMove));
    jit.move(subject, cursor);
    jit.addPtr(TrustedImm32(offsetOfBottom), sp, X86Registers::esi);
    jit.addPtr(TrustedImm32(SavedFrame::offsetOfWords()), saved, X86Registers::edi);
    jit.x86RepeatMoveString64();
    jit.move(cursor, subject);
    Jump isMoved = jit.jump();
    isSmall.link(&jit);
    Label next = jit.label();
    jit.load64(BaseIndex(sp, count, CCallHelpers::TimesEight, offsetOfBottom - 8), word);
    jit.load64(BaseIndex(sp, count, CCallHelpers::TimesEight, offsetOfBottom - 16), second);
    jit.store64(word, BaseIndex(saved, count, CCallHelpers::TimesEight, SavedFrame::offsetOfWords() - 8));
    jit.store64(second, BaseIndex(saved, count, CCallHelpers::TimesEight, SavedFrame::offsetOfWords() - 16));
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(2), count).linkTo(next, &jit);
    isEmpty.link(&jit);
    isMoved.link(&jit);
#elif CPU(ARM64)
    const GPRReg source = cursor;
    const GPRReg destination = jit.allocateScratch(StubRegister::T11);
    const FPRReg firstVector = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg secondVector = jit.allocateScratch(FPRInfo::fpRegT1);
    RELEASE_ASSERT(noOverlap(subject, saved, count, source, destination, word, second));
    jit.move(sp, source);
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

// The SavedFrame has held this frame before. The write barrier is left to the caller.
static void emitSaveFrame(StubAssembler& jit, const FrameCopyRegisters& registers, GPRReg siteWord, uint8_t flags)
{
    const GPRReg saved = registers.saved;
    const GPRReg word = registers.word;
    emitWillWrite(jit, saved);
    emitCopyFrameWords(jit, registers);
#if CPU(X86_64)
    jit.store64(SavedFrame::preservedRegister, Address(saved, SavedFrame::offsetOfPreservedRegister()));
#endif
    jit.loadCallerReturnAddress(0, word);
    jit.storePtr(word, Address(saved, SavedFrame::offsetOfResumeAddress()));
    jit.store8(TrustedImm32(flags), Address(saved, SavedFrame::offsetOfFlags()));
    jit.and32(TrustedImm32((1 << SavedFrame::liveSlotMapOffsetBits) - 1), siteWord, word);
    jit.add32(TrustedImm32(1), word);
    jit.store32(word, Address(saved, SavedFrame::offsetOfLiveSlotMapOffsetPlusOne()));
    emitDidWrite(jit, saved);
}

// The barrier takes its owner in the first operand register of stubs and overwrites the register of the site word.
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
            jit.callStub(writeBarrier());
        });
        jit.emitFunctionEpilogue();
        jit.jump().linkTo(afterBarrier, &jit);
    });
}

// registers.awaited is a promise of the realm. Nothing is stored before the last exit.
static void emitResumeWhenPromiseSettles(StubAssembler& jit, const AwaitRegisters& registers, JumpList& needsOperation)
{
    const auto [savedFrame, promise, word, other] = registers;
    emitBranchUnlessHolds(jit, tracksNoAsyncContext, registers, needsOperation);
    emitBranchUnlessHolds(jit, awaitedTakesInlineReaction, registers, needsOperation);
    jit.move(TrustedImm64(static_cast<int64_t>(resumeReactionBits)), other);
    jit.or64(other, word);
    jit.store64(savedFrame, Address(promise, JSPromise::offsetOfSlot()));
    jit.store64(word, Address(promise, JSPromise::offsetOfPacked()));
}

// registers.awaited is what a call without a promise returned. Leaves in it the cell that now refers to the waiter.
static void emitResumeWhenCallSettles(StubAssembler& jit, const AwaitRegisters& registers, JumpList& needsOperation)
{
    using Continuation = SavedFrame::Continuation;
    const auto [waiter, awaited, word, other] = registers;
    const Address continuation(awaited, SavedFrame::offsetOfContinuation());

    jit.load32(Address(awaited, JSCell::structureIDOffset()), word);
    Jump isPromise = jit.branch32(CCallHelpers::Equal, word, Address(instanceGPR, offsetOfPromiseHeader));
    needsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
    Jump hasNoContinuation = jit.branch8(CCallHelpers::Equal, continuation, TrustedImm32(static_cast<int32_t>(Continuation::None)));
    needsOperation.append(jit.branch8(CCallHelpers::NotEqual, continuation, TrustedImm32(static_cast<int32_t>(Continuation::Promise))));
    jit.loadPtr(Address(awaited, SavedFrame::offsetOfPromiseOrWaiter()), awaited);

    isPromise.link(&jit);
    emitResumeWhenPromiseSettles(jit, registers, needsOperation);
    Jump isRegistered = jit.jump();

    hasNoContinuation.link(&jit);
    emitBranchUnlessHolds(jit, tracksNoAsyncContext, registers, needsOperation);
    jit.store64(TrustedImm64(JSValue::ValueUndefined), Address(waiter, SavedFrame::offsetOfAsyncContext()));
    jit.storePtr(waiter, Address(awaited, SavedFrame::offsetOfPromiseOrWaiter()));
    jit.store8(TrustedImm32(static_cast<int32_t>(Continuation::Waiter)), continuation);
    isRegistered.link(&jit);
}

StubSignature signatureOfAwaitBySavingFrame(StubKey key)
{
    switch (static_cast<AwaitedOperand>(key.form())) {
    case AwaitedOperand::Value:
        return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::T9) });
    case AwaitedOperand::ResultOfCallWithoutPromise:
        return StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::T9) });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateAwaitBySavingFrame(StubAssembler& jit)
{
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    const bool awaitsCallWithoutPromise = static_cast<AwaitedOperand>(jit.key().form()) == AwaitedOperand::ResultOfCallWithoutPromise;
    const GPRReg result = jit.result();
    const GPRReg awaited = awaitsCallWithoutPromise ? jit.allocateScratch(StubRegister::A1) : jit.operand(0);
    const GPRReg siteWord = jit.clobberOperand(1);
    const GPRReg savedFrame = jit.allocateScratch(StubRegister::A2);
    const GPRReg count = jit.allocateScratch(StubRegister::A3);
    const GPRReg word = jit.allocateScratch(StubRegister::A4);
    const GPRReg other = jit.allocateScratch(StubRegister::A5);
#if CPU(X86_64)
    const GPRReg second = jit.allocateScratch(StubRegister::A0);
#elif CPU(ARM64)
    const GPRReg second = jit.allocateScratch(StubRegister::T10);
#endif
    const AwaitRegisters registers { savedFrame, awaited, word, other };
    RELEASE_ASSERT(awaited == operationArgumentGPR(1) && savedFrame == operationArgumentGPR(2));

    JumpList mustSuspend;
    JumpList isOtherObject;
    if (awaitsCallWithoutPromise)
        jit.move(result, awaited);
    // The job loop only enters functions that have been suspended before.
    jit.loadPtr(Address(fp, sizeof(void*)), word);
    Jump isFirstSave = jit.branchPtr(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(StubFrameKind::SavedFrameAdapter)));
    jit.loadPtr(Address(fp), word);
    jit.loadPtr(Address(word, static_cast<int>(CallFrameSlot::callee) * static_cast<int>(sizeof(Register))), savedFrame);
    for (const AwaitCondition& condition : awaitConditionsOnState)
        emitBranchUnlessHolds(jit, condition, registers, mustSuspend);
    if (!awaitsCallWithoutPromise)
        jit.move(awaited, result);
    emitLoadSettledValue(jit, registers, result, isOtherObject, mustSuspend);
    jit.returnToCaller();

    Label returnToCallerOfFunction = emitFunctionReturn(jit);

    isOtherObject.link(&jit);
    if (awaitsCallWithoutPromise) {
        // Unless the callee returned its SavedFrame, it has finished and this object is its result.
        mustSuspend.append(jit.branch32(CCallHelpers::Equal, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.savedFrameHeader))));
        jit.returnToCaller();
    }

    mustSuspend.link(&jit);
    emitSaveFrame(jit, { awaited, savedFrame, count, word, other, second }, siteWord, 0);
    JumpList needsOperation;
    needsOperation.append(jit.branchIfNotCell(awaited));
    if (awaitsCallWithoutPromise) {
        jit.move(awaited, second);
        emitResumeWhenCallSettles(jit, { savedFrame, second, word, other }, needsOperation);
        emitBarrierPreservingOperands(jit, second, result, siteWord, word);
    } else {
        emitBranchUnlessHolds(jit, awaitedHasPromiseStructure, registers, needsOperation);
        emitResumeWhenPromiseSettles(jit, registers, needsOperation);
        emitBarrierPreservingOperands(jit, awaited, result, siteWord, word);
    }
    emitBarrierPreservingOperands(jit, savedFrame, result, siteWord, word);
    jit.loadPtr(Address(savedFrame, SavedFrame::offsetOfPromiseOrWaiter()), result);
    emitLeaveFunction(jit, siteWord, other, returnToCallerOfFunction);

    needsOperation.link(&jit);
    emitCallOperationAndLeaveFunction(jit, awaitsCallWithoutPromise ? Entry::operationAOTAwaitCallWithSavedFrame : Entry::operationAOTAwaitWithSavedFrame, siteWord, returnToCallerOfFunction);

    isFirstSave.link(&jit);
    emitSuspend(jit, awaitsCallWithoutPromise ? Entry::operationAOTSaveFrameAndAwaitCall : Entry::operationAOTSaveFrameAndAwait, siteWord, returnToCallerOfFunction);
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

StubSignature signatureOfSuspendGenerator(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::R0), gprOf(StubRegister::T9) });
    signature.result = StubResult::TwoGPRs;
    signature.secondResult = gprOf(resumeModeRegister);
    return signature;
}

// Writes neither result: RestoreSavedFrameToStack does, when control comes back to the site.
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
#if CPU(X86_64)
    const GPRReg second = jit.allocateScratch(StubRegister::A0);
#elif CPU(ARM64)
    const GPRReg second = jit.allocateScratch(StubRegister::T10);
#endif
    RELEASE_ASSERT(generator == operationArgumentGPR(1));
    RELEASE_ASSERT(noOverlap(generator, value, siteWord, saved, count, cursor, word, second, instanceGPR));

    // loadLabelAddress needs a bound label and nothing may fall into its code, so that code precedes the entry.
    Label returnToCallerOfFunction = emitFunctionReturn(jit);
    jit.align();
    jit.bindEntry(jit.key());

    Jump isFirstSave = emitLoadSavedFrame(jit, form, generator, saved);
    emitSaveFrame(jit, { generator, saved, count, cursor, word, second }, siteWord, form == GeneratorSuspension::AsyncAwait ? 0 : SavedFrame::isSuspendedAtYieldFlag);
    emitStoreSuspendedState(jit, form, generator);
    emitBarrierPreservingOperands(jit, saved, value, siteWord, word);
    emitLeaveFunction(jit, siteWord, word, returnToCallerOfFunction);

    // The first suspension is the initial yield, whose value nobody reads, so the call may overwrite it.
    isFirstSave.link(&jit);
    emitSuspend(jit, Entry::operationAOTSaveFrameAndYield, siteWord, returnToCallerOfFunction);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
