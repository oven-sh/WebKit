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
#include "AOTStubsExceptions.h"
#include "CallFrame.h"
#include "StackAlignment.h"
#include "StackManager.h"

namespace JSC { namespace AOT {

static constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
static constexpr GPRReg fp = GPRInfo::callFrameRegister;

static constexpr int32_t generatorSlotOffset = Generator::generatorSlotInStubFrame * static_cast<int32_t>(sizeof(void*));
static_assert(generatorSlotOffset < 0 && -generatorSlotOffset <= static_cast<int32_t>(stackAlignmentBytes()));

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

// resumeModeBitsOfEngineMode as code.
static void emitConvertEngineResumeMode(StubAssembler& jit, GPRReg mode, GPRReg scratch)
{
    jit.lshift32(mode, TrustedImm32(1), scratch);
    jit.urshift32(TrustedImm32(1), mode);
    jit.or32(scratch, mode);
    jit.and32(TrustedImm32(3), mode);
}

void generateEnterSavedFrame(StubAssembler& jit)
{
    const GPRReg scratch = jit.reuseScratch(StubRegister::T11);
    emitEnterAdapter(jit, scratch);
    const GPRReg saved = jit.reuseScratch(StubRegister::A0);
    const GPRReg mode = jit.reuseScratch(StubRegister::A1);
    const GPRReg value = jit.reuseScratch(StubRegister::A2);
    RELEASE_ASSERT(noOverlap(calleeGPR, saved, mode, value, scratch));
    checkRestoreOperands(saved, mode, value);

    jit.move(calleeGPR, saved);
    jit.load32(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::ResumeMode))), mode);
    emitConvertEngineResumeMode(jit, mode, scratch);
    jit.load64(CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::Value))), value);
    jit.callStub(restoreSavedFrameToStack());
    jit.bindReturnLabel(StubFrameKind::SavedFrameAdapter);
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

// Checks the stack, pushes the frame record and copies the words below it. The number of words is even and may be zero.
#if CPU(X86_64)
static void emitCopyFrameToStack(StubAssembler& jit, const RestoredFrame& frame)
{
    const auto [saved, mode, value, count, scratch] = frame;
    const GPRReg second = jit.allocateScratch(StubRegister::A3);
    RELEASE_ASSERT(saved == X86Registers::edi && mode == X86Registers::esi && second == X86Registers::ecx);
    RELEASE_ASSERT(noOverlap(saved, mode, value, count, scratch, second, instanceGPR));

    jit.load32(Address(saved, SavedFrame::offsetOfNumberOfWords()), count);
    jit.lshift64(count, TrustedImm32(3), scratch);
    jit.subPtr(sp, scratch, second);
    jit.subPtr(TrustedImm32(maxStackUsedWithoutCheck), second);
    jit.linkToStub(jit.branchPtr(CCallHelpers::Below, second, softStackLimit()), throwStackOverflow());

    jit.emitFunctionPrologue();
    jit.subPtr(scratch, sp);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
    Jump isSmall = jit.branch32(CCallHelpers::Below, count, TrustedImm32(SavedFrame::minWordsForStringMove));
    jit.move(count, X86Registers::ecx);
    jit.move(mode, count);
    jit.move(saved, scratch);
    jit.addPtr(TrustedImm32(SavedFrame::offsetOfWords()), saved, X86Registers::esi);
    jit.move(sp, X86Registers::edi);
    jit.x86RepeatMoveString64();
    jit.move(scratch, saved);
    jit.move(count, mode);
    Jump isMoved = jit.jump();
    isSmall.link(&jit);
    Label next = jit.label();
    jit.load64(BaseIndex(saved, count, CCallHelpers::TimesEight, SavedFrame::offsetOfWords() - 8), scratch);
    jit.load64(BaseIndex(saved, count, CCallHelpers::TimesEight, SavedFrame::offsetOfWords() - 16), second);
    jit.store64(scratch, BaseIndex(sp, count, CCallHelpers::TimesEight, -8));
    jit.store64(second, BaseIndex(sp, count, CCallHelpers::TimesEight, -16));
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
    RELEASE_ASSERT(noOverlap(saved, mode, value, count, source, destination, first, second, instanceGPR));

    jit.load32(Address(saved, SavedFrame::offsetOfNumberOfWords()), count);
    jit.move(sp, destination);
    jit.subLeftShift64(destination, count, TrustedImm32(3), destination);
    jit.subPtr(destination, TrustedImm32(maxStackUsedWithoutCheck), source);
    jit.linkToStub(jit.branchPtr(CCallHelpers::Below, source, softStackLimit()), throwStackOverflow());

    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(sizeof(CallerFrameAndPC)), destination);
    jit.move(destination, sp);
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
    Jump isAtYield = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(SavedFrame::isSuspendedAtYieldFlag));
    Jump throws = jit.branchTest32(CCallHelpers::NonZero, mode, TrustedImm32(static_cast<int32_t>(ResumeMode::Throw)));
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
