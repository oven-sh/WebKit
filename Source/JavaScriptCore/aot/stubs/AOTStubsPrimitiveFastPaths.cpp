/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPrimitiveFastPaths.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "JSCJSValue.h"
#include "NumericStrings.h"

namespace JSC { namespace AOT {

StubSignature signatureOfToNumberAsDouble(StubKey)
{
    static_assert(FPRInfo::fpRegT0 == FPRInfo::returnValueFPR);
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.result = StubResult::FPR;
    signature.firstResult = FPRInfo::fpRegT0;
    return signature;
}

void generateToNumberAsDouble(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const FPRReg result = jit.resultFPR();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);

    Jump notInt32 = jit.branch64(CCallHelpers::Below, value, numberTag);
    jit.convertInt32ToDouble(value, result);
    jit.returnToCaller();

    notInt32.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, value, numberTag);
    unboxDouble(jit, value, scratch, result);
    jit.returnToCaller();

    notNumber.link(&jit);
    static_assert((JSValue::ValueFalse | 1) == JSValue::ValueTrue);
    jit.move(value, scratch);
    jit.and64(TrustedImm32(~1), scratch);
    Jump notBoolean = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm32(JSValue::ValueFalse));
    jit.move(value, scratch);
    jit.and32(TrustedImm32(1), scratch);
    jit.convertInt32ToDouble(scratch, result);
    jit.returnToCaller();

    notBoolean.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTToNumber, {
        .returns = OperationCall::Returns::Double,
        .saves = OperationCall::Saves::CallerSavedRegisters,
    });
}

static void returnString(StubAssembler& jit, GPRReg string)
{
    jit.move(string, jit.result());
    jit.move(TrustedImm32(0), jit.secondResult());
    jit.returnToCaller();
}

StubSignature signatureOfToStringFrontEnd(StubKey)
{
    return operationFrontEndSignature(1);
}

void generateToStringFrontEnd(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg first = jit.allocateScratch(isARM64() ? StubRegister::T11 : StubRegister::R0);
    const GPRReg second = jit.allocateScratch(StubRegister::T10);
    const GPRReg high = jit.allocateScratch(isARM64() ? StubRegister::T9 : StubRegister::A2);
    static_assert(sizeof(NumericStrings::StringWithJSString) == 2 * sizeof(void*) && sizeof(NumericStrings::CacheEntryWithJSString<int>) == 3 * sizeof(void*));

    JumpList isNotKnown;
    isNotKnown.append(jit.branchIfNotInt32(value));
    Jump isSmall = jit.branch32(CCallHelpers::Below, value, TrustedImm32(NumericStrings::cacheSize));
    jit.zeroExtend32ToWord(value, first);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(numbers.hashSecrets) + sizeof(uint64_t)), second);
    jit.xor64(first, second);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(numbers.hashSecrets)), high);
    jit.xor64(high, first);
#if CPU(ARM64)
    jit.uMulHigh64(first, second, high);
    jit.mul64(second, first);
#else
    jit.x86UMulHigh64(second, first, high);
#endif
    jit.xor32(high, first);
    jit.and32(TrustedImm32(NumericStrings::cacheSize - 1), first);
    jit.getEffectiveAddress(BaseIndex(first, first, CCallHelpers::TimesTwo), first);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.intStrings)), second);
    jit.getEffectiveAddress(BaseIndex(second, first, CCallHelpers::TimesEight), second);
    isNotKnown.append(jit.branch32(CCallHelpers::NotEqual, Address(second, NumericStrings::CacheEntryWithJSString<int>::offsetOfKey()), value));
    jit.loadPtr(Address(second, NumericStrings::CacheEntryWithJSString<int>::offsetOfJSString()), second);
    isNotKnown.append(jit.branchTestPtr(CCallHelpers::Zero, second));
    returnString(jit, second);

    isSmall.link(&jit);
    jit.zeroExtend32ToWord(value, first);
    jit.lshift64(TrustedImm32(1), first);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.smallIntStrings)), second);
    jit.loadPtr(BaseIndex(second, first, CCallHelpers::TimesEight, NumericStrings::StringWithJSString::offsetOfJSString()), second);
    isNotKnown.append(jit.branchTestPtr(CCallHelpers::Zero, second));
    returnString(jit, second);

    isNotKnown.link(&jit);
    jit.tailCallOperation(Entry::RawToString, OperationCall::FirstArgument::Instance);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
