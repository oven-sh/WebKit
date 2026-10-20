/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubCalls.h"

#if ENABLE(AOT)

#include "AOTRuntimeValidation.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "StackAlignment.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

using CheckedRegisters = Vector<Reg, 64>;

static CheckedRegisters collectCheckedRegisters(const RegisterSet& clobbered)
{
    RegisterSet checked = RegisterSet::allScalarRegisters();
    checked.exclude(clobbered);
    checked.exclude(RegisterSet::stackRegisters());
    checked.exclude(RegisterSet::reservedHardwareRegisters());
    checked.exclude(RegisterSet::macroClobberedGPRs());
    checked.exclude(RegisterSet::macroClobberedFPRs());

    CheckedRegisters registers;
    checked.forEach([&](Reg reg) {
        registers.append(reg);
    });
    return registers;
}

static CCallHelpers::Address addressOfStoredRegister(CCallHelpers::Address storage, size_t index)
{
    return storage.withOffset(static_cast<int32_t>(index * sizeof(CPURegister)));
}

static CCallHelpers::Address addressOfStoredRegister(size_t index)
{
    return addressOfStoredRegister(CCallHelpers::Address(CCallHelpers::stackPointerRegister), index);
}

static void emitStores(CCallHelpers& jit, const CheckedRegisters& registers, CCallHelpers::Address storage = CCallHelpers::Address(CCallHelpers::stackPointerRegister))
{
    for (size_t index = 0; index < registers.size(); ++index) {
        Reg reg = registers[index];
        if (reg.isGPR())
            jit.store64(reg.gpr(), addressOfStoredRegister(storage, index));
        else
            jit.storeDouble(reg.fpr(), addressOfStoredRegister(storage, index));
    }
}

static void emitCompares(CCallHelpers& jit, const CheckedRegisters& registers, CCallHelpers::JumpList& mismatch)
{
    for (size_t index = 0; index < registers.size(); ++index) {
        Reg reg = registers[index];
        GPRReg bits;
        if (reg.isGPR())
            bits = reg.gpr();
        else {
            bits = jit.scratchRegister();
            jit.moveDoubleTo64(reg.fpr(), bits);
        }
        mismatch.append(jit.branch64(CCallHelpers::NotEqual, bits, addressOfStoredRegister(index)));
    }
}

// As StubAssembler::returnFlagsConstant. The instance's address is never zero.
static void emitFlags(CCallHelpers& jit, MacroAssembler::ResultCondition trueCondition, bool value)
{
    if (value == (trueCondition == MacroAssembler::Zero))
        jit.compareOnFlags64(instanceGPR, instanceGPR);
    else
        jit.testOnFlags64(instanceGPR, instanceGPR);
}

void emitCallCheckingPreservedRegisters(CCallHelpers& jit, const StubSummary& summary, const ScopedLambda<void()>& emitCall, const ScopedLambda<void()>& emitReport)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    AllowMacroScratchRegisterUsage allowScratch(jit);
    CheckedRegisters registers = collectCheckedRegisters(summary.clobbered);
    CCallHelpers::TrustedImm32 bytes(static_cast<int32_t>(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(registers.size() * sizeof(CPURegister))));

    jit.subPtr(bytes, sp);
    emitStores(jit, registers);
    emitCall();

    CCallHelpers::JumpList mismatch;
    CCallHelpers::JumpList done;
    auto emitChecks = [&] {
        emitCompares(jit, registers, mismatch);
        jit.addPtr(bytes, sp);
    };
    if (summary.signature.result == StubResult::Flags) {
        MacroAssembler::ResultCondition trueCondition = summary.signature.trueCondition;
        RELEASE_ASSERT(trueCondition == MacroAssembler::Zero || trueCondition == MacroAssembler::NonZero);
        CCallHelpers::Jump isTrue = jit.branchOnFlags(trueCondition == MacroAssembler::Zero ? CCallHelpers::Equal : CCallHelpers::NotEqual);
        emitChecks();
        emitFlags(jit, trueCondition, false);
        done.append(jit.jump());
        isTrue.link(&jit);
        emitChecks();
        emitFlags(jit, trueCondition, true);
    } else
        emitChecks();
    done.append(jit.jump());

    mismatch.link(&jit);
    emitReport();
    done.link(&jit);
}

void emitStoresOfKeptRegisters(CCallHelpers& jit, const KeptRegisterCheck& check)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    CheckedRegisters registers = collectCheckedRegisters(check.clobbered);
    RELEASE_ASSERT((registers.size() + 1) * sizeof(CPURegister) <= KeptRegisterCheck::storageSize);
    emitStores(jit, registers, check.storage);
    if (check.checksStackPointer) {
        jit.move(CCallHelpers::stackPointerRegister, jit.scratchRegister());
        jit.store64(jit.scratchRegister(), addressOfStoredRegister(check.storage, registers.size()));
    }
}

void emitChecksOfKeptRegisters(CCallHelpers& jit, const KeptRegisterCheck& check, ValidationFailureKind kind, std::optional<FunctionID> callee, const ScopedLambda<void()>& emitReport)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    CheckedRegisters registers = collectCheckedRegisters(check.clobbered);

    Vector<CCallHelpers::Jump, 64> mismatches;
    for (size_t index = 0; index < registers.size(); ++index) {
        Reg reg = registers[index];
        GPRReg bits;
        if (reg.isGPR())
            bits = reg.gpr();
        else {
            bits = jit.scratchRegister();
            jit.moveDoubleTo64(reg.fpr(), bits);
        }
        mismatches.append(jit.branch64(CCallHelpers::NotEqual, bits, addressOfStoredRegister(check.storage, index)));
    }
    if (check.checksStackPointer) {
        jit.move(CCallHelpers::stackPointerRegister, jit.scratchRegister());
        mismatches.append(jit.branch64(CCallHelpers::NotEqual, jit.scratchRegister(), addressOfStoredRegister(check.storage, registers.size())));
    }
    CCallHelpers::Jump done = jit.jump();

    CCallHelpers::JumpList report;
    uint32_t calleeIndex = callee ? callee->index() : ChangedRegister::unknownCallee;
    for (size_t index = 0; index < mismatches.size(); ++index) {
        Reg reg = index < registers.size() ? registers[index] : Reg(CCallHelpers::stackPointerRegister);
        mismatches[index].link(&jit);
        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNumber(ChangedRegister::encode(reg.index(), calleeIndex)))), operationArgumentGPR(1));
        report.append(jit.jump());
    }
    report.link(&jit);
    if (check.checksStackPointer) {
        jit.load64(addressOfStoredRegister(check.storage, registers.size()), jit.scratchRegister());
        jit.move(jit.scratchRegister(), CCallHelpers::stackPointerRegister);
    }
    // The report goes through the instance.
    if (size_t index = registers.find(Reg(instanceGPR)); index != notFound)
        jit.load64(addressOfStoredRegister(check.storage, index), instanceGPR);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(kind)), operationArgumentGPR(2));
    jit.move(CCallHelpers::TrustedImm32(check.caller.index()), operationArgumentGPR(3));
    jit.move(CCallHelpers::TrustedImm32(check.value), operationArgumentGPR(4));
    emitReport();
    done.link(&jit);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
