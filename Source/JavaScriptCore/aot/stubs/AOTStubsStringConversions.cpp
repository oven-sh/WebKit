/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "JSCJSValueInlines.h"
#include "NumericStrings.h"

namespace JSC { namespace AOT {

StubSignature signatureOfIntegerToString(StubKey)
{
#if CPU(ARM64)
    return StubSignature::withGPRResult({ gprOf(StubRegister::T11) });
#else
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1) });
#endif
}

StubSignature signatureOfConcatenateKeyOfPair(StubKey)
{
    return StubSignature::withGPRResult({ operationArgumentGPR(1), operationArgumentGPR(2) });
}

void locateKeysOfPair(StubAssembler& jit, GPRReg first, GPRReg second, GPRReg set, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(first, second, set, scratch));
    // StringKeyCache::hashOfPair().
    jit.getEffectiveAddress(BaseIndex(first, second, CCallHelpers::TimesEight), set);
    jit.mul32(TrustedImm32(static_cast<int32_t>(StringKeyCache::keyOfPairMultiplier)), set, set);
    jit.urshift32(TrustedImm32(StringKeyCache::keyOfPairHashShift), set);
#if CPU(X86_64)
    UNUSED_PARAM(scratch);
    jit.and64(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.keyOfPairMask)), set);
#else
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.keyOfPairMask)), scratch);
    jit.and64(scratch, set);
#endif
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.keysOfPairs)), set);
}

// The operation joins the parts and enters the pair.
void generateConcatenateKeyOfPair(StubAssembler& jit)
{
    const GPRReg first = jit.operand(0);
    const GPRReg second = jit.operand(1);
    const GPRReg set = jit.result();
#if CPU(X86_64)
    locateKeysOfPair(jit, first, second, set, InvalidGPRReg);
#else
    locateKeysOfPair(jit, first, second, set, jit.allocateScratch(StubRegister::T12));
#endif
    for (unsigned way = 0; way < StringKeyCache::waysOfKeysOfPairs; ++way) {
        const int32_t entry = way * sizeof(StringKeyCache::KeyOfPair);
        Jump hasOtherFirst = jit.branch64(CCallHelpers::NotEqual, Address(set, entry + OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, first)), first);
        Jump hasOtherSecond = jit.branch64(CCallHelpers::NotEqual, Address(set, entry + OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, second)), second);
        jit.loadPtr(Address(set, entry + OBJECT_OFFSETOF(StringKeyCache::KeyOfPair, key)), set);
        jit.returnToCaller();
        hasOtherFirst.link(&jit);
        hasOtherSecond.link(&jit);
    }
    auto use = static_cast<ConcatenatedKeyForm>(jit.key().form()) == ConcatenatedKeyForm::Name ? UseOfJoinedText::LikelyName : UseOfJoinedText::LikelyKey;
    jit.callOperationAndReturn(Entry::operationAOTConcatenatePair, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedGPRs,
        .spilledArguments = OperationCall::SpilledArguments { 2, static_cast<uint32_t>(use) },
    });
}

// The caches hold int32 keys. The operation takes every integer.
void generateIntegerToString(StubAssembler& jit)
{
    const GPRReg result = jit.result();
    const GPRReg value = jit.operand(0);
#if CPU(ARM64)
    const GPRReg first = jit.allocateScratch(StubRegister::T12);
    const GPRReg second = jit.allocateScratch(StubRegister::T13);
    const GPRReg high = jit.allocateScratch(StubRegister::T14);
#else
    const GPRReg first = result;
    const GPRReg second = jit.allocateScratch(StubRegister::T10);
    const GPRReg high = jit.allocateScratch(StubRegister::A2);
    static_assert(gprOf(StubRegister::R0) == X86Registers::eax && gprOf(StubRegister::A2) == X86Registers::edx, "mul writes its product to rdx:rax");
#endif
    static_assert(sizeof(NumericStrings::StringWithJSString) == 2 * sizeof(void*) && sizeof(NumericStrings::CacheEntryWithJSString<int>) == 3 * sizeof(void*));
    JumpList slow;
    Jump isSmall = jit.branch64(CCallHelpers::Below, value, TrustedImm32(NumericStrings::cacheSize));
    jit.signExtend32ToPtr(value, first);
    slow.append(jit.branch64(CCallHelpers::NotEqual, first, value));
    // StringKeyCache::tryFindIntegerKey().
    if (!jit.options().useWideReferences) {
        JumpList isNoIntegerKey;
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.integerKeys)), second);
        isNoIntegerKey.append(jit.branchTestPtr(CCallHelpers::Zero, second));
        jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.integerKeyMask)), high);
        jit.and64(value, high);
        jit.load64(BaseIndex(second, high, CCallHelpers::TimesEight), second);
        jit.move(second, high);
        jit.urshift64(TrustedImm32(32), high);
        isNoIntegerKey.append(jit.branch32(CCallHelpers::NotEqual, high, value));
        jit.zeroExtend32ToWord(second, second);
        isNoIntegerKey.append(jit.branchTest32(CCallHelpers::Zero, second));
        decodeCell32(jit, second, result);
        jit.returnToCaller();
        isNoIntegerKey.link(&jit);
    }
    jit.zeroExtend32ToWord(value, first);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(numbers.hashSecrets[1])), second);
    jit.xor64(first, second);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(numbers.hashSecrets[0])), high);
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
    slow.append(jit.branch32(CCallHelpers::NotEqual, Address(second, NumericStrings::CacheEntryWithJSString<int>::offsetOfKey()), value));
    jit.loadPtr(Address(second, NumericStrings::CacheEntryWithJSString<int>::offsetOfJSString()), result);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, result));
    jit.returnToCaller();

    isSmall.link(&jit);
    jit.zeroExtend32ToWord(value, first);
    jit.lshift64(TrustedImm32(1), first);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.smallIntStrings)), second);
    jit.loadPtr(BaseIndex(second, first, CCallHelpers::TimesEight, NumericStrings::StringWithJSString::offsetOfJSString()), result);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, result));
    jit.returnToCaller();

    slow.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTIntToString, { .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = value });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
