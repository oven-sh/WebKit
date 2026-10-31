/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubAssembler.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubRegisters.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsOperations.h"
#include "VM.h"
#include <algorithm>
#include <iterator>

namespace JSC { namespace AOT {

void StubAssembler::loadEntry(Entry entry, GPRReg result)
{
    loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), result);
    loadPtr(Address(result, offsetInRuntimeTable(entry)), result);
}

void StubAssembler::loadOperation(Entry entry, GPRReg result)
{
    recordCall(entry);
    loadEntry(entry, result);
}

void StubAssembler::loadRuntimePointer(Entry entry, GPRReg result)
{
    recordUse(entry);
    loadEntry(entry, result);
}

void StubAssembler::callOperation(Entry entry, GPRReg scratch)
{
    RELEASE_ASSERT(scratch != assertionScratchGPR);
#if CPU(X86_64)
    recordCall(entry);
    loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), scratch);
    prepareCallOperation();
    call(Address(scratch, offsetInRuntimeTable(entry)), OperationPtrTag);
#else
    loadOperation(entry, scratch);
    prepareCallOperation();
    call(scratch, OperationPtrTag);
#endif
}

void StubAssembler::prepareCallOperation()
{
    static_assert(gprOf(StubRegister::Operation) != assertionScratchGPR);
#if ASSERT_ENABLED
    loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), assertionScratchGPR);
    storePtr(GPRInfo::callFrameRegister, Address(assertionScratchGPR, VM::offsetOfTopCallFrame()));
#endif
}


void StubAssembler::callOperationAndReturn(Entry entry, const OperationCall& operationCall, const ScopedLambda<void()>& beforeReturn, SavedRegistersAreRead savedRegistersAreRead)
{
    // An operation that cannot throw leaves the register that reports an exception undefined.
    bool exceptionIsInRegister = operationCall.returns == OperationCall::Returns::Value || operationCall.returns == OperationCall::Returns::Void;
    RELEASE_ASSERT(!operationCall.checksException || !exceptionIsInRegister || effectsOf(entry).mayThrow);
    OperationCall call = operationCall;
    call.pollsBeforeReturning = mayFailToAllocate(entry, call.firstArgument);
    emitOperationCallAndReturn(entry, call, beforeReturn, savedRegistersAreRead);
}

// A stub may hand an operand back as its result: then the register comes back by being saved.
bool StubAssembler::resultsComeFromOperation(const OperationCall& operationCall) const
{
    RegisterSet returned;
    switch (operationCall.returns) {
    case OperationCall::Returns::Void:
        break;
    case OperationCall::Returns::Value:
        returned.add(operationCall.valueDestination.value_or(GPRInfo::returnValueGPR), IgnoreVectors);
        break;
    case OperationCall::Returns::Double:
        returned.add(FPRInfo::returnValueFPR, IgnoreVectors);
        break;
    case OperationCall::Returns::Pair:
        returned.add(GPRInfo::returnValueGPR, IgnoreVectors);
        returned.add(GPRInfo::returnValueGPR2, IgnoreVectors);
        break;
    }
    for (Reg result : { m_signature.firstResult, m_signature.secondResult }) {
        if (result && !returned.contains(result, IgnoreVectors))
            return false;
    }
    return m_signature.result != StubResult::Flags;
}

bool StubAssembler::resultsInFPRsComeFromOperation(const OperationCall& operationCall) const
{
    for (Reg result : { m_signature.firstResult, m_signature.secondResult }) {
        if (result && result.isFPR() && (operationCall.returns != OperationCall::Returns::Double || result != Reg(FPRInfo::returnValueFPR)))
            return false;
    }
    return true;
}

void StubAssembler::emitOperationCallAndReturn(const CalledOperation& operation, const OperationCall& givenCall, const ScopedLambda<void()>& beforeReturn, SavedRegistersAreRead savedRegistersAreRead)
{
    OperationCall operationCall = givenCall;
    const bool savesFPRs = operationCall.saves == OperationCall::Saves::CallerSavedRegisters || operationCall.saves == OperationCall::Saves::CallerSavedGPRsAndFewFPRs;
    const bool savesNoFPRsForCaller = callerKeepsNoFPRs() && savesFPRs && savedRegistersAreRead == SavedRegistersAreRead::ByCallerAlone && resultsInFPRsComeFromOperation(operationCall);
    if (savesNoFPRsForCaller)
        operationCall.saves = OperationCall::Saves::CallerSavedGPRs;
    RELEASE_ASSERT(m_savedRegisterStack.isEmpty());
    checkHasNoBorrowedFPRs();
    RELEASE_ASSERT(!operationCall.valueDestination || operationCall.returns == OperationCall::Returns::Value);
    if (operationCall.spilledArguments) {
        RELEASE_ASSERT(!operationCall.secondArgumentIn && !operationCall.thirdArgumentIn && !operationCall.fourthArgumentIn && !operationCall.fifthArgumentIn);
        RELEASE_ASSERT(!operationCall.constantArguments[0] && !operationCall.constantArguments[1]);
        RELEASE_ASSERT(operationCall.spilledArguments->count < numberOfOperationArgumentGPRs);
    }
    OperationCall::Saves saves = operationCall.saves;
    if (callerKeepsNoRegisters() && savedRegistersAreRead == SavedRegistersAreRead::ByCallerAlone && !operationCall.spilledArguments && resultsComeFromOperation(operationCall)) {
        saves = OperationCall::Saves::Nothing;
        for (unsigned index = 0; index < m_signature.numberOfOperands; ++index) {
            if (m_signature.operands[index].isGPR())
                clobberOperand(index);
        }
    }
    switch (saves) {
    case OperationCall::Saves::Nothing:
        emitOperationCallSavingNothing(operation, operationCall, beforeReturn);
        return;
    case OperationCall::Saves::CallerSavedGPRs:
    case OperationCall::Saves::CallerSavedGPRsAndFewFPRs:
        RELEASE_ASSERT(operationCall.returns != OperationCall::Returns::Double || savesNoFPRsForCaller);
        [[fallthrough]];
    case OperationCall::Saves::CallerSavedRegisters:
        RELEASE_ASSERT(operationCall.returns != OperationCall::Returns::Pair);
        emitOperationCallSavingRegisters(operation, operationCall, beforeReturn);
        return;
    }
}

void StubAssembler::loadCalledOperation(const CalledOperation& operation, GPRReg function)
{
    WTF::switchOn(operation,
        [&](Entry entry) {
            loadOperation(entry, function);
        },
        [&](GPRReg offset) {
            RELEASE_ASSERT(noOverlap(offset, function, operationArgumentGPR(0), operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3)));
            m_recording.hasUnknownEffects = true;
            loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), function);
            loadPtr(BaseIndex(function, offset, TimesOne), function);
        });
}

void StubAssembler::loadFirstArgument(OperationCall::FirstArgument firstArgument)
{
    constexpr GPRReg first = operationArgumentGPR(0);
    switch (firstArgument) {
    case OperationCall::FirstArgument::Instance:
        move(instanceGPR, first);
        return;
    case OperationCall::FirstArgument::GlobalObject:
        loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), first);
        return;
    case OperationCall::FirstArgument::VM:
        loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), first);
        return;
    case OperationCall::FirstArgument::None:
        return;
    }
}

StubAssembler::OperationToCall StubAssembler::setUpOperationCall(const CalledOperation& operation, const OperationCall& operationCall)
{
    constexpr GPRReg second = operationArgumentGPR(1);
    constexpr GPRReg third = operationArgumentGPR(2);
    constexpr GPRReg fourth = operationArgumentGPR(3);
    constexpr GPRReg fifth = operationArgumentGPR(4);
    const GPRReg function = reuseScratch(StubRegister::Operation);
    if (operationCall.fifthArgumentIn) {
        RELEASE_ASSERT(operationCall.secondArgumentIn != fifth && operationCall.thirdArgumentIn != fifth && operationCall.fourthArgumentIn != fifth);
        move(*operationCall.fifthArgumentIn, fifth);
    }
    if (operationCall.fourthArgumentIn) {
        RELEASE_ASSERT(operationCall.secondArgumentIn != fourth && operationCall.thirdArgumentIn != fourth);
        move(*operationCall.fourthArgumentIn, fourth);
    }
    if (operationCall.thirdArgumentIn) {
        RELEASE_ASSERT(operationCall.secondArgumentIn != third);
        move(*operationCall.thirdArgumentIn, third);
    }
    if (operationCall.secondArgumentIn)
        move(*operationCall.secondArgumentIn, second);
    for (const std::optional<OperationCall::ConstantArgument>& constant : operationCall.constantArguments) {
        if (!constant)
            continue;
        RELEASE_ASSERT(!operationCall.secondArgumentIn || constant->argument != second);
        RELEASE_ASSERT(!operationCall.thirdArgumentIn || constant->argument != third);
        RELEASE_ASSERT(!operationCall.fourthArgumentIn || constant->argument != fourth);
        RELEASE_ASSERT(!operationCall.fifthArgumentIn || constant->argument != fifth);
        move(TrustedImm32(static_cast<int32_t>(constant->value)), constant->argument);
    }
    loadFirstArgument(operationCall.firstArgument);
#if CPU(X86_64)
    // The call reads the operation from the table.
    return WTF::switchOn(operation,
        [&](Entry entry) {
            recordCall(entry);
            loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), function);
            return Address(function, offsetInRuntimeTable(entry));
        },
        [&](GPRReg offset) {
            RELEASE_ASSERT(noOverlap(offset, operationArgumentGPR(0), operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3)));
            m_recording.hasUnknownEffects = true;
#if ASSERT_ENABLED
            // prepareCallOperation() takes the register of the offset.
            static_assert(assertionScratchGPR == preservingCallImmediateGPR);
            loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), function);
            add64(offset, function);
            return Address(function);
#else
            recordClobber(offset);
            add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), offset);
            return Address(offset);
#endif
        });
#else
    loadCalledOperation(operation, function);
    return function;
#endif
}

// No stub has an operand in it, and an operation does not preserve it.
#if CPU(X86_64)
static constexpr GPRReg scratchAfterOperation = CCallHelpers::s_scratchRegister;
#else
static constexpr GPRReg scratchAfterOperation = ARM64Registers::x16;
#endif

void StubAssembler::emitOperationCallSavingNothing(const CalledOperation& operation, const OperationCall& operationCall, const ScopedLambda<void()>& beforeReturn)
{
    RELEASE_ASSERT(key().callerFrame() != CallerFrame::Frameless && !operationCall.spilledArguments);
    const bool callerIsStub = key().callerFrame() == CallerFrame::StubFrame;
    const OperationToCall function = setUpOperationCall(operation, operationCall);
    if (callerIsStub) {
#if CPU(ARM64)
        pushToSave(linkRegister);
#else
        subPtr(TrustedImm32(sizeof(CPURegister)), stackPointerRegister);
#endif
    } else
        emitFunctionPrologue();
    prepareCallOperation();
    call(function, OperationPtrTag);
    if (!callerIsStub)
        bindReturnLabel(StubFrameKind::ReturnsAfterOperation);
    if (callerIsStub) {
#if CPU(ARM64)
        popToRestore(linkRegister);
#else
        addPtr(TrustedImm32(sizeof(CPURegister)), stackPointerRegister);
#endif
    } else
        emitFunctionEpilogueWithEmptyFrame();
    // A stub that another stub called leaves the poll to its caller.
    const bool polls = operationCall.pollsBeforeReturning && !callerIsStub;
    static_assert(noOverlap(scratchAfterOperation, GPRInfo::returnValueGPR, GPRInfo::returnValueGPR2, instanceGPR));
    JumpList exceptionOrPoll;
    bool hasPolled = false;
    if (operationCall.checksException) {
        switch (operationCall.returns) {
        case OperationCall::Returns::Value:
        case OperationCall::Returns::Void: {
            const GPRReg exception = operationCall.returns == OperationCall::Returns::Value ? GPRInfo::returnValueGPR2 : GPRInfo::returnValueGPR;
            exceptionOrPoll.append(polls ? branchIfNonZeroOrPollIsDue(exception, scratchAfterOperation) : branchTestPtr(NonZero, exception));
            hasPolled = polls;
            break;
        }
        case OperationCall::Returns::Double:
        case OperationCall::Returns::Pair:
            loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratchAfterOperation);
            exceptionOrPoll.append(branchTestPtr(NonZero, Address(scratchAfterOperation, VM::exceptionOffset())));
            break;
        }
    }
    if (polls && !hasPolled)
        exceptionOrPoll.append(branchTestPtr(NonZero, pollIsDueWord()));
    Label afterPoll = label();
    if (operationCall.valueDestination) {
        recordClobber(*operationCall.valueDestination);
        move(GPRInfo::returnValueGPR, *operationCall.valueDestination);
    }
    beforeReturn();
    ret();
    if (exceptionOrPoll.empty())
        return;
    exceptionOrPoll.link(this);
    Jump hasNoException;
    if (polls && operationCall.checksException) {
        loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratchAfterOperation);
        hasNoException = branchTestPtr(Zero, Address(scratchAfterOperation, VM::exceptionOffset()));
    }
    if (operationCall.checksException)
        jumpToExceptionHandler();
    if (!polls)
        return;
    if (hasNoException.isSet())
        hasNoException.link(this);
    pollBeforeReturningFromOperation(afterPoll);
}

#if CPU(X86_64)
// The fault handler pushed the frame pointer of a frameless caller, then the address to resume at.
void StubAssembler::emitFramelessCallerEpilogue()
{
    RELEASE_ASSERT(key().callerFrame() == CallerFrame::Frameless);
    pop(s_scratchRegister);
    pop(GPRInfo::callFrameRegister);
    push(s_scratchRegister);
}

static constexpr GPRReg gprsSavedAroundOperation[] = { X86Registers::eax, X86Registers::ecx, X86Registers::edx, X86Registers::esi, X86Registers::edi, X86Registers::r8, X86Registers::r9, X86Registers::r10 };

template<typename Functor>
static void forEachFPRSavedAroundOperation(OperationCall::Saves saves, const Functor& functor)
{
    if (saves == OperationCall::Saves::CallerSavedGPRs)
        return;
    const RegisterSet unsaved = saves == OperationCall::Saves::CallerSavedGPRsAndFewFPRs ? floatingPointRegistersLostToAccessors(StubCallee::Operation) : RegisterSet();
    for (unsigned i = 0; i < 16; ++i) {
        FPRReg fpr = static_cast<FPRReg>(X86Registers::xmm0 + i);
        if (!unsaved.contains(fpr, IgnoreVectors))
            functor(fpr, i);
    }
}

void StubAssembler::emitOperationCallSavingRegisters(const CalledOperation& operation, const OperationCall& operationCall, const ScopedLambda<void()>& beforeReturn)
{
    RELEASE_ASSERT(key().callerFrame() != CallerFrame::Frameless);
    constexpr GPRReg sp = stackPointerRegister;
    constexpr GPRReg scratch = s_scratchRegister;
    constexpr auto& saved = gprsSavedAroundOperation;
    constexpr unsigned numberOfGPRs = std::size(saved);
    static_assert(!(numberOfGPRs % 2));
    static_assert(FPRInfo::returnValueFPR == X86Registers::xmm0);
    const bool returnsValue = operationCall.returns == OperationCall::Returns::Value;
    const bool returnsDouble = operationCall.returns == OperationCall::Returns::Double;
    const std::optional<GPRReg> valueDestination = operationCall.valueDestination;
    const unsigned numberOfSpillSlots = operationCall.spilledArguments ? std::max(operationCall.spilledArguments->count, 1u) : 0;
    RELEASE_ASSERT(!valueDestination || std::ranges::find(saved, *valueDestination) != std::end(saved));
    RegisterSet restored;
    for (GPRReg reg : saved) {
        if (returnsValue && reg == GPRInfo::returnValueGPR && !valueDestination)
            continue;
        if (reg == valueDestination || !m_recording.clobbered().contains(Reg(reg), IgnoreVectors))
            restored.add(reg);
    }
    forEachFPRSavedAroundOperation(operationCall.saves, [&](FPRReg fpr, unsigned) {
        if (!returnsDouble || fpr != FPRInfo::returnValueFPR)
            restored.add(fpr, IgnoreVectors);
    });

    // The arguments that go in memory lie at the stack pointer, the registers below the frame pointer.
    emitFunctionPrologue();
    // The frame has room for what is saved and for no more: a word that nobody writes holds what an earlier frame left there, and the operation may collect.
    // The floating-point registers lie nearest: a displacement of one byte reaches sixteen words below the frame pointer, and as many above the stack pointer.
    std::array<unsigned, 16> indexOfFPR { };
    std::array<unsigned, numberOfGPRs> indexOfGPR { };
    unsigned numberOfSlots = 0;
    forEachFPRSavedAroundOperation(operationCall.saves, [&](FPRReg fpr, unsigned i) {
        if (restored.contains(fpr, IgnoreVectors))
            indexOfFPR[i] = numberOfSlots++;
    });
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        if (restored.contains(Reg(saved[i]), IgnoreVectors))
            indexOfGPR[i] = numberOfSlots++;
    }
    const int32_t sizeOfFrame = alignedSizeOfRegisters(numberOfSpillSlots + numberOfSlots);
    if (sizeOfFrame)
        subPtr(TrustedImm32(sizeOfFrame), sp);
    auto slot = [&](unsigned index) {
        int32_t belowFramePointer = (index + 1) * sizeof(CPURegister);
        return belowFramePointer <= 128 ? Address(GPRInfo::callFrameRegister, -belowFramePointer) : Address(sp, sizeOfFrame - belowFramePointer);
    };
    auto slotOfGPR = [&](unsigned i) { return slot(indexOfGPR[i]); };
    auto slotOfFPR = [&](unsigned i) { return slot(indexOfFPR[i]); };
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        if (restored.contains(Reg(saved[i]), IgnoreVectors))
            store64(saved[i], slotOfGPR(i));
    }
    forEachFPRSavedAroundOperation(operationCall.saves, [&](FPRReg fpr, unsigned i) {
        if (restored.contains(fpr, IgnoreVectors))
            storeDouble(fpr, slotOfFPR(i));
    });
    withSavedRegisters(restored, [&] {
        if (operationCall.spilledArguments) {
            for (unsigned i = 0; i < operationCall.spilledArguments->count; ++i)
                store64(operationArgumentGPR(1 + i), Address(sp, i * sizeof(CPURegister)));
            move(sp, operationArgumentGPR(2));
            move(TrustedImm32(operationCall.spilledArguments->countWord), operationArgumentGPR(1));
        }
        const OperationToCall function = setUpOperationCall(operation, operationCall);
        prepareCallOperation();
        call(function, OperationPtrTag);
        bindReturnLabel(StubFrameKind::ReturnsAfterOperation);
    });
    const bool testsException = operationCall.checksException;
    const bool polls = operationCall.pollsBeforeReturning && key().callerFrame() != CallerFrame::StubFrame;
    if (testsException) {
        if (returnsDouble) {
            loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
            loadPtr(Address(scratch, VM::exceptionOffset()), scratch);
        } else
            move(returnsValue ? GPRInfo::returnValueGPR2 : GPRInfo::returnValueGPR, scratch);
        if (polls)
            or64(pollIsDueWord(), scratch);
    } else if (polls)
        loadPtr(pollIsDueWord(), scratch);
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        if (saved[i] == valueDestination)
            store64(GPRInfo::returnValueGPR, slotOfGPR(i));
    }
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        if (restored.contains(Reg(saved[i]), IgnoreVectors))
            load64(slotOfGPR(i), saved[i]);
    }
    forEachFPRSavedAroundOperation(operationCall.saves, [&](FPRReg fpr, unsigned i) {
        if (restored.contains(fpr, IgnoreVectors))
            loadDouble(slotOfFPR(i), fpr);
    });
    if (valueDestination)
        recordClobber(*valueDestination);
    emitFunctionEpilogue();
    Jump exceptionOrPoll;
    if (testsException || polls)
        exceptionOrPoll = branchTestPtr(NonZero, scratch);
    Label afterPoll = label();
    beforeReturn();
    ret();
    if (!exceptionOrPoll.isSet())
        return;
    exceptionOrPoll.link(this);
    Jump hasNoException;
    if (polls && testsException) {
        loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
        hasNoException = branchTestPtr(Zero, Address(scratch, VM::exceptionOffset()));
    }
    if (testsException) {
        if (key().callerFrame() == CallerFrame::StubFrame)
            emitFunctionEpilogue();
        jumpToStub(handleException(true));
    }
    if (!polls)
        return;
    if (hasNoException.isSet())
        hasNoException.link(this);
    pollBeforeReturningFromOperation(afterPoll);
}
#else

void StubAssembler::emitOperationCallSavingRegisters(const CalledOperation& operation, const OperationCall& operationCall, const ScopedLambda<void()>& beforeReturn)
{
    RELEASE_ASSERT(key().callerFrame() != CallerFrame::Frameless);
    constexpr GPRReg sp = stackPointerRegister;
    constexpr GPRReg scratch = ARM64Registers::x16;
    constexpr unsigned numberOfGPRs = 16;
    static_assert(GPRInfo::returnValueGPR == ARM64Registers::x0 && FPRInfo::returnValueFPR == ARM64Registers::q0);
    const bool returnsValue = operationCall.returns == OperationCall::Returns::Value;
    const bool returnsDouble = operationCall.returns == OperationCall::Returns::Double;
    const std::optional<GPRReg> valueDestination = operationCall.valueDestination;
    const bool valueStaysInRegister = returnsValue && !valueDestination;
    const unsigned numberOfFPRs = operationCall.saves == OperationCall::Saves::CallerSavedGPRs ? 0 : 24;
    auto gpr = [](unsigned i) { return static_cast<GPRReg>(ARM64Registers::x0 + i); };
    auto fpr = [](unsigned i) { return static_cast<FPRReg>(i < 8 ? ARM64Registers::q0 + i : ARM64Registers::q16 + (i - 8)); };
    const RegisterSet unsavedFPRs = operationCall.saves == OperationCall::Saves::CallerSavedGPRsAndFewFPRs ? floatingPointRegistersLostToAccessors(StubCallee::Operation) : RegisterSet();
    // They are stored and loaded two at a time.
    auto savesFPR = [&](unsigned i) { return !unsavedFPRs.contains(fpr(i & ~1u), IgnoreVectors) || !unsavedFPRs.contains(fpr(i | 1u), IgnoreVectors); };
    RELEASE_ASSERT(!valueDestination || static_cast<unsigned>(*valueDestination - ARM64Registers::x0) < numberOfGPRs);

    RegisterSet restored;
    for (unsigned i = valueStaysInRegister; i < numberOfGPRs; ++i)
        restored.add(gpr(i));
    for (unsigned i = returnsDouble; i < numberOfFPRs; ++i) {
        if (savesFPR(i))
            restored.add(fpr(i), IgnoreVectors);
    }
    // Compiled code pins x18 and no stub names it, so nobody sees an operation change it.
    restored.add(ARM64Registers::x18);

    emitFunctionPrologue();
    subPtr(TrustedImm32((numberOfGPRs + numberOfFPRs) * 8), sp);
    for (unsigned i = 0; i < numberOfGPRs; i += 2)
        storePair64(gpr(i), gpr(i + 1), sp, TrustedImm32(i * 8));
    for (unsigned i = 0; i < numberOfFPRs; i += 2) {
        if (savesFPR(i))
            storePairDouble(fpr(i), fpr(i + 1), Address(sp, (numberOfGPRs + i) * 8));
    }
    withSavedRegisters(restored, [&] {
        if (operationCall.spilledArguments) {
            move(TrustedImm32(operationCall.spilledArguments->countWord), operationArgumentGPR(1));
            addPtr(TrustedImm32(8), sp, operationArgumentGPR(2));
        }
        const OperationToCall function = setUpOperationCall(operation, operationCall);
        prepareCallOperation();
        call(function, OperationPtrTag);
        bindReturnLabel(StubFrameKind::ReturnsAfterOperation);
    });
    const bool testsException = operationCall.checksException;
    const bool polls = operationCall.pollsBeforeReturning && key().callerFrame() != CallerFrame::StubFrame;
    if (polls)
        loadPtr(pollIsDueWord(), scratch);
    if (testsException) {
        // The registers that report the exception are restored below.
        const GPRReg exception = returnsValue ? GPRInfo::returnValueGPR2 : GPRInfo::returnValueGPR;
        if (returnsDouble) {
            loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), exception);
            loadPtr(Address(exception, VM::exceptionOffset()), exception);
        }
        if (polls)
            or64(exception, scratch);
        else
            move(exception, scratch);
    }
    if (valueDestination) {
        recordClobber(*valueDestination);
        store64(GPRInfo::returnValueGPR, Address(sp, (*valueDestination - ARM64Registers::x0) * 8));
    }
    if (valueStaysInRegister)
        load64(Address(sp, 8), gpr(1));
    for (unsigned i = valueStaysInRegister ? 2 : 0; i < numberOfGPRs; i += 2)
        loadPair64(sp, TrustedImm32(i * 8), gpr(i), gpr(i + 1));
    if (returnsDouble)
        loadDouble(Address(sp, (numberOfGPRs + 1) * 8), fpr(1));
    for (unsigned i = returnsDouble ? 2 : 0; i < numberOfFPRs; i += 2) {
        if (savesFPR(i))
            loadPairDouble(Address(sp, (numberOfGPRs + i) * 8), fpr(i), fpr(i + 1));
    }
    emitFunctionEpilogue();
    Jump exceptionOrPoll;
    if (testsException || polls)
        exceptionOrPoll = branchTestPtr(NonZero, scratch);
    Label afterPoll = label();
    beforeReturn();
    ret();
    if (!exceptionOrPoll.isSet())
        return;
    exceptionOrPoll.link(this);
    Jump hasNoException;
    if (polls && testsException) {
        loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
        hasNoException = branchTestPtr(Zero, Address(scratch, VM::exceptionOffset()));
    }
    if (testsException) {
        if (key().callerFrame() == CallerFrame::StubFrame)
            emitFunctionEpilogue();
        jumpToStub(handleException(true));
    }
    if (!polls)
        return;
    if (hasNoException.isSet())
        hasNoException.link(this);
    pollBeforeReturningFromOperation(afterPoll);
}
#endif

static bool isCalledByFrontEnd(Entry entry)
{
    return std::ranges::any_of(operationFrontEnds(), [&](const OperationFrontEnd& frontEnd) {
        return frontEnd.raw == entry;
    });
}

void StubAssembler::emitOperationTailCall(const CalledOperation& operation, OperationCall::FirstArgument firstArgument)
{
    RELEASE_ASSERT(key().callerFrame() != CallerFrame::Frameless);
    // After a tail call there is no place to poll for the error of a failed allocation. A front end was called by a stub that polls.
    if (const Entry* entry = std::get_if<Entry>(&operation); entry && mayFailToAllocate(*entry, firstArgument) && !effectsOf(*entry).mayThrow && !isCalledByFrontEnd(*entry)) {
        callOperationAndReturn(*entry, { .firstArgument = firstArgument, .checksException = false });
        return;
    }
    // No operand is live here: the arguments are in place, and this is none of their registers.
    constexpr GPRReg function = gprOf(StubRegister::Operation);
    loadFirstArgument(firstArgument);
    loadCalledOperation(operation, function);
    farJump(function, OperationPtrTag);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
