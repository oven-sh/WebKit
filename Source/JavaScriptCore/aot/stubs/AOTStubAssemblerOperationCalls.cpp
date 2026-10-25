/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubAssembler.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStubRegisters.h"
#include "AOTStubsExceptions.h"
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
    loadOperation(entry, scratch);
    prepareCallOperation();
    call(scratch, OperationPtrTag);
}

void StubAssembler::prepareCallOperation()
{
    static_assert(gprOf(StubRegister::Operation) != assertionScratchGPR);
#if ASSERT_ENABLED
    loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), assertionScratchGPR);
    storePtr(GPRInfo::callFrameRegister, Address(assertionScratchGPR, VM::offsetOfTopCallFrame()));
#endif
}

void StubAssembler::callOperationAndReturn(Entry entry, const OperationCall& operationCall, const ScopedLambda<void()>& beforeReturn)
{
    // An operation that cannot throw leaves the register that reports an exception undefined.
    bool exceptionIsInRegister = operationCall.returns == OperationCall::Returns::Value || operationCall.returns == OperationCall::Returns::Void;
    RELEASE_ASSERT(!operationCall.checksException || !exceptionIsInRegister || effectsOf(entry).mayThrow);
    emitOperationCallAndReturn(entry, operationCall, beforeReturn);
}

void StubAssembler::emitOperationCallAndReturn(const CalledOperation& operation, const OperationCall& operationCall, const ScopedLambda<void()>& beforeReturn)
{
    RELEASE_ASSERT(m_savedRegisterStack.isEmpty());
    checkHasNoBorrowedFPRs();
    RELEASE_ASSERT(!operationCall.valueDestination || operationCall.returns == OperationCall::Returns::Value);
    if (operationCall.spilledArguments) {
        RELEASE_ASSERT(!operationCall.secondArgumentIn && !operationCall.thirdArgumentIn && !operationCall.fourthArgumentIn && !operationCall.fifthArgumentIn);
        RELEASE_ASSERT(!operationCall.constantArguments[0] && !operationCall.constantArguments[1]);
        RELEASE_ASSERT(operationCall.spilledArguments->count < numberOfOperationArgumentGPRs);
    }
    switch (operationCall.saves) {
    case OperationCall::Saves::Nothing:
        emitOperationCallSavingNothing(operation, operationCall, beforeReturn);
        return;
    case OperationCall::Saves::CallerSavedGPRs:
        RELEASE_ASSERT(operationCall.returns != OperationCall::Returns::Double);
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

GPRReg StubAssembler::setUpOperationCall(const CalledOperation& operation, const OperationCall& operationCall)
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
    loadCalledOperation(operation, function);
    return function;
}

void StubAssembler::emitOperationCallSavingNothing(const CalledOperation& operation, const OperationCall& operationCall, const ScopedLambda<void()>& beforeReturn)
{
    RELEASE_ASSERT(key().callerFrame() != CallerFrame::Frameless && !operationCall.spilledArguments);
    const bool callerIsStub = key().callerFrame() == CallerFrame::StubFrame;
    const GPRReg function = setUpOperationCall(operation, operationCall);
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
    if (callerIsStub) {
#if CPU(ARM64)
        popToRestore(linkRegister);
#else
        addPtr(TrustedImm32(sizeof(CPURegister)), stackPointerRegister);
#endif
    } else
        emitFunctionEpilogueWithEmptyFrame();
    Jump exception;
    if (operationCall.checksException) {
        switch (operationCall.returns) {
        case OperationCall::Returns::Value:
            exception = branchTestPtr(NonZero, GPRInfo::returnValueGPR2);
            break;
        case OperationCall::Returns::Void:
            exception = branchTestPtr(NonZero, GPRInfo::returnValueGPR);
            break;
        case OperationCall::Returns::Double:
            loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), function);
            exception = branchTestPtr(NonZero, Address(function, VM::exceptionOffset()));
            break;
        case OperationCall::Returns::Pair: {
            static_assert(noOverlap(gprOf(StubRegister::A3), GPRInfo::returnValueGPR, GPRInfo::returnValueGPR2));
            GPRReg vm = reuseScratch(StubRegister::A3);
            loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
            exception = branchTestPtr(NonZero, Address(vm, VM::exceptionOffset()));
            break;
        }
        }
    }
    if (operationCall.valueDestination) {
        recordClobber(*operationCall.valueDestination);
        move(GPRInfo::returnValueGPR, *operationCall.valueDestination);
    }
    beforeReturn();
    ret();
    if (!operationCall.checksException)
        return;
    exception.link(this);
    jumpToExceptionHandler();
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

void StubAssembler::emitOperationCallSavingRegisters(const CalledOperation& operation, const OperationCall& operationCall, const ScopedLambda<void()>& beforeReturn)
{
    RELEASE_ASSERT(key().callerFrame() != CallerFrame::Frameless);
    constexpr GPRReg sp = stackPointerRegister;
    constexpr GPRReg scratch = s_scratchRegister;
    constexpr GPRReg saved[] = { X86Registers::eax, X86Registers::ecx, X86Registers::edx, X86Registers::esi, X86Registers::edi, X86Registers::r8, X86Registers::r9, X86Registers::r10 };
    constexpr unsigned numberOfGPRs = std::size(saved);
    static_assert(!(numberOfGPRs % 2));
    static_assert(FPRInfo::returnValueFPR == X86Registers::xmm0);
    const bool returnsValue = operationCall.returns == OperationCall::Returns::Value;
    const bool returnsDouble = operationCall.returns == OperationCall::Returns::Double;
    const std::optional<GPRReg> valueDestination = operationCall.valueDestination;
    const unsigned numberOfFPRs = operationCall.saves == OperationCall::Saves::CallerSavedRegisters ? 16 : 0;
    const unsigned numberOfSpillSlots = operationCall.spilledArguments ? std::max(operationCall.spilledArguments->count, 1u) : 0;
    auto fpr = [](unsigned i) { return static_cast<FPRReg>(X86Registers::xmm0 + i); };
    auto slot = [&](unsigned i) { return Address(sp, (numberOfSpillSlots + i) * sizeof(CPURegister)); };
    RELEASE_ASSERT(!valueDestination || std::ranges::find(saved, *valueDestination) != std::end(saved));

    RegisterSet restored;
    for (GPRReg reg : saved) {
        if (returnsValue && reg == GPRInfo::returnValueGPR && !valueDestination)
            continue;
        if (reg == valueDestination || !m_recording.clobbered().contains(Reg(reg), IgnoreVectors))
            restored.add(reg);
    }
    for (unsigned i = returnsDouble; i < numberOfFPRs; ++i)
        restored.add(fpr(i), IgnoreVectors);

    emitFunctionPrologue();
    subPtr(TrustedImm32(alignedSizeOfRegisters(numberOfSpillSlots + numberOfGPRs + numberOfFPRs)), sp);
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        if (restored.contains(Reg(saved[i]), IgnoreVectors))
            store64(saved[i], slot(i));
    }
    for (unsigned i = returnsDouble; i < numberOfFPRs; ++i)
        storeDouble(fpr(i), slot(numberOfGPRs + i));
    withSavedRegisters(restored, [&] {
        if (operationCall.spilledArguments) {
            for (unsigned i = 0; i < operationCall.spilledArguments->count; ++i)
                store64(operationArgumentGPR(1 + i), Address(sp, i * sizeof(CPURegister)));
            move(sp, operationArgumentGPR(2));
            move(TrustedImm32(operationCall.spilledArguments->countWord), operationArgumentGPR(1));
        }
        const GPRReg function = setUpOperationCall(operation, operationCall);
        prepareCallOperation();
        call(function, OperationPtrTag);
    });
    if (operationCall.checksException) {
        if (returnsDouble) {
            loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
            loadPtr(Address(scratch, VM::exceptionOffset()), scratch);
        } else
            move(returnsValue ? GPRInfo::returnValueGPR2 : GPRInfo::returnValueGPR, scratch);
    }
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        if (saved[i] == valueDestination)
            store64(GPRInfo::returnValueGPR, slot(i));
    }
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        if (restored.contains(Reg(saved[i]), IgnoreVectors))
            load64(slot(i), saved[i]);
    }
    for (unsigned i = returnsDouble; i < numberOfFPRs; ++i)
        loadDouble(slot(numberOfGPRs + i), fpr(i));
    if (valueDestination)
        recordClobber(*valueDestination);
    emitFunctionEpilogue();
    Jump exception;
    if (operationCall.checksException)
        exception = branchTestPtr(NonZero, scratch);
    beforeReturn();
    ret();
    if (!operationCall.checksException)
        return;
    exception.link(this);
    if (key().callerFrame() == CallerFrame::StubFrame)
        emitFunctionEpilogue();
    jumpToStub(handleException(true));
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
    const unsigned numberOfFPRs = operationCall.saves == OperationCall::Saves::CallerSavedRegisters ? 24 : 0;
    auto gpr = [](unsigned i) { return static_cast<GPRReg>(ARM64Registers::x0 + i); };
    auto fpr = [](unsigned i) { return static_cast<FPRReg>(i < 8 ? ARM64Registers::q0 + i : ARM64Registers::q16 + (i - 8)); };
    RELEASE_ASSERT(!valueDestination || static_cast<unsigned>(*valueDestination - ARM64Registers::x0) < numberOfGPRs);

    RegisterSet restored;
    for (unsigned i = valueStaysInRegister; i < numberOfGPRs; ++i)
        restored.add(gpr(i));
    for (unsigned i = returnsDouble; i < numberOfFPRs; ++i)
        restored.add(fpr(i), IgnoreVectors);
    // Compiled code pins x18 and no stub names it, so nobody sees an operation change it.
    restored.add(ARM64Registers::x18);

    emitFunctionPrologue();
    subPtr(TrustedImm32((numberOfGPRs + numberOfFPRs) * 8), sp);
    for (unsigned i = 0; i < numberOfGPRs; i += 2)
        storePair64(gpr(i), gpr(i + 1), sp, TrustedImm32(i * 8));
    for (unsigned i = 0; i < numberOfFPRs; i += 2)
        storePairDouble(fpr(i), fpr(i + 1), Address(sp, (numberOfGPRs + i) * 8));
    withSavedRegisters(restored, [&] {
        if (operationCall.spilledArguments) {
            move(TrustedImm32(operationCall.spilledArguments->countWord), operationArgumentGPR(1));
            addPtr(TrustedImm32(8), sp, operationArgumentGPR(2));
        }
        const GPRReg function = setUpOperationCall(operation, operationCall);
        prepareCallOperation();
        call(function, OperationPtrTag);
    });
    if (operationCall.checksException) {
        if (returnsDouble) {
            loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
            loadPtr(Address(scratch, VM::exceptionOffset()), scratch);
        } else
            move(returnsValue ? GPRInfo::returnValueGPR2 : GPRInfo::returnValueGPR, scratch);
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
    for (unsigned i = returnsDouble ? 2 : 0; i < numberOfFPRs; i += 2)
        loadPairDouble(Address(sp, (numberOfGPRs + i) * 8), fpr(i), fpr(i + 1));
    emitFunctionEpilogue();
    Jump exception;
    if (operationCall.checksException)
        exception = branchTestPtr(NonZero, scratch);
    beforeReturn();
    ret();
    if (!operationCall.checksException)
        return;
    exception.link(this);
    if (key().callerFrame() == CallerFrame::StubFrame)
        emitFunctionEpilogue();
    jumpToStub(handleException(true));
}
#endif

void StubAssembler::emitOperationTailCall(const CalledOperation& operation, OperationCall::FirstArgument firstArgument)
{
    RELEASE_ASSERT(key().callerFrame() != CallerFrame::Frameless);
    // No operand is live here: the arguments are in place, and this is none of their registers.
    constexpr GPRReg function = gprOf(StubRegister::Operation);
    loadFirstArgument(firstArgument);
    loadCalledOperation(operation, function);
    farJump(function, OperationPtrTag);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
