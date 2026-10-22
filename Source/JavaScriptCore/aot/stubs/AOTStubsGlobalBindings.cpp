/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsGlobalBindings.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeWords.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "JSGlobalObject.h"

namespace JSC { namespace AOT {

// On x86-64 the immediate and the result share a register, which takes the shortest move.
StubSignature signatureOfGetGlobalBinding(StubKey)
{
    StubSignature signature;
    signature.result = StubResult::GPR;
    signature.firstResult = GPRInfo::returnValueGPR;
    signature.immediate = isARM64() ? gprOf(stubImmediateRegister) : GPRInfo::returnValueGPR;
    return signature;
}

// What the record holds: a constant, or the place of a value under the Structure, and the epoch of the scripts' bindings, that it was filed under. A record that holds nothing has a Structure that no object has.
void generateGetGlobalBinding(StubAssembler& jit)
{
    const auto scope = static_cast<GlobalBindingScope>(jit.key().form());
    const GPRReg result = jit.result();
#if CPU(X86_64)
    const GPRReg number = jit.allocateScratch(stubImmediateRegister);
    jit.zeroExtend32ToWord(jit.immediate(), number);
#else
    const GPRReg number = jit.immediate();
#endif
    const GPRReg record = jit.allocateScratch(StubRegister::T10);
    const GPRReg global = jit.allocateScratch(StubRegister::T11);
    JumpList callsOperation;

    static_assert(sizeof(GlobalBinding) == 3 * sizeof(uint64_t));
    jit.getEffectiveAddress(BaseIndex(number, number, CCallHelpers::TimesTwo), record);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.counters.globalBindings)), result);
    jit.getEffectiveAddress(BaseIndex(result, record, CCallHelpers::TimesEight), record);
    jit.load64(Address(record, OBJECT_OFFSETOF(GlobalBinding, constant)), result);
    Jump isNoConstant = jit.branchTest64(CCallHelpers::Zero, result);
    jit.returnToCaller();

    isNoConstant.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), global);
    jit.load32(Address(global, JSCell::structureIDOffset()), result);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, result, Address(record, OBJECT_OFFSETOF(GlobalBinding, structureID))));
    if (scope == GlobalBindingScope::ScriptsAndGlobalObject) {
        jit.load32(Address(global, JSGlobalObject::globalLexicalBindingEpochOffset()), result);
        callsOperation.append(jit.branch32(CCallHelpers::NotEqual, result, Address(record, OBJECT_OFFSETOF(GlobalBinding, lexicalBindingEpoch))));
    }
    jit.load64(Address(record, OBJECT_OFFSETOF(GlobalBinding, location)), record);
    Jump isAddress = jit.branch64(CCallHelpers::GreaterThanOrEqual, record, TrustedImm32(0));
    jit.loadPtr(Address(global, JSObject::butterflyOffset()), global);
    jit.add64(global, record);
    isAddress.link(&jit);
    jit.load64(Address(record), result);
    callsOperation.append(jit.branchTest64(CCallHelpers::Zero, result));
    jit.returnToCaller();

    callsOperation.link(&jit);
    Entry operation = scope == GlobalBindingScope::ScriptsAndGlobalObject ? Entry::operationAOTGetGlobalBinding : Entry::operationAOTGetGlobalObjectBinding;
    jit.callOperationAndReturn(operation, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = number });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
