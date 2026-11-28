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

StubSignature signatureOfDoubleToString(StubKey) { return StubSignature::withGPRResult({ FPRInfo::argumentFPR0 }); }

StubSignature signatureOfConcatenateKeyOfPair(StubKey)
{
    return StubSignature::withGPRResult({ operationArgumentGPR(1), operationArgumentGPR(2) });
}

void locateKeysOfPair(StubAssembler& jit, GPRReg first, GPRReg second, GPRReg set, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(first, second, set, scratch));
    jit.mul32(TrustedImm32(static_cast<int32_t>(StringKeyCache::secondKeyMultiplier)), second, set);
    jit.add32(first, set);
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

static void joinLongStringGenerically(StubAssembler& jit, GPRReg first, GPRReg second, GPRReg scratch)
{
    JumpList isNotLongString;
    const StubKey joinOfTwo = concatenate(2);
#if CPU(ARM64)
    const GPRReg firstOfJoin = jit.allocateScratch(StubRegister::T11);
    const GPRReg secondOfJoin = jit.reuseScratch(StubRegister::T12);
#else
    const GPRReg firstOfJoin = scratch;
    const GPRReg secondOfJoin = second;
#endif
    RELEASE_ASSERT(signatureOf(joinOfTwo).operands[0] == firstOfJoin && signatureOf(joinOfTwo).operands[1] == secondOfJoin);
    isNotLongString.append(jit.branchIfNotCell(first));
    isNotLongString.append(jit.branchIfNotType(first, StringType));
    isNotLongString.append(jit.branchIfNotCell(second));
    isNotLongString.append(jit.branchIfNotType(second, StringType));
    jit.loadPtr(Address(first, JSString::offsetOfValue()), scratch);
    Jump isRope = jit.branchIfRopeStringImpl(scratch);
    isNotLongString.append(jit.branch32(CCallHelpers::BelowOrEqual, Address(scratch, StringImpl::lengthMemoryOffset()), TrustedImm32(StringKeyCache::maxCachedKeyLength)));
    Jump isLong = jit.jump();
    isRope.link(&jit);
    isNotLongString.append(jit.branch32(CCallHelpers::BelowOrEqual, Address(first, JSRopeString::offsetOfLength()), TrustedImm32(StringKeyCache::maxCachedKeyLength)));
    isLong.link(&jit);
    jit.move(second, secondOfJoin);
    jit.move(first, firstOfJoin);
    jit.jumpToStub(joinOfTwo);
    isNotLongString.link(&jit);
}

void generateConcatenateKeyOfPair(StubAssembler& jit)
{
    const GPRReg first = jit.operand(0);
    const GPRReg second = jit.operand(1);
    const GPRReg set = jit.result();
    JumpList isTextOfInteger;
    if (!jit.options().useWideReferences) {
        const GPRReg place = jit.allocateScratch(StubRegister::T9);
        JumpList isNoInteger;
        isNoInteger.append(jit.branchPtr(CCallHelpers::NotEqual, Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(emptyString)), second));
        Jump isNoInt32 = jit.branchIfNotInt32(first);
        Jump isSmall = jit.branch32(CCallHelpers::Below, first, TrustedImm32(NumericStrings::cacheSize));
        isNoInteger.append(jit.branchTest32(CCallHelpers::Signed, first));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.integerKeys)), set);
        isTextOfInteger.append(jit.branchTestPtr(CCallHelpers::Zero, set));
        jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.integerKeyMask)), place);
        jit.and64(first, place);
        jit.load64(BaseIndex(set, place, CCallHelpers::TimesEight), set);
        jit.move(set, place);
        jit.urshift64(TrustedImm32(32), place);
        isTextOfInteger.append(jit.branch32(CCallHelpers::NotEqual, place, first));
        jit.zeroExtend32ToWord(set, set);
        isTextOfInteger.append(jit.branchTest32(CCallHelpers::Zero, set));
        decodeCell32(jit, set, set);
        jit.returnToCaller();
        isSmall.link(&jit);
        jit.zeroExtend32ToWord(first, place);
        jit.lshift64(TrustedImm32(1), place);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.smallIntStrings)), set);
        jit.loadPtr(BaseIndex(set, place, CCallHelpers::TimesEight, NumericStrings::StringWithJSString::offsetOfJSString()), set);
        isTextOfInteger.append(jit.branchTestPtr(CCallHelpers::Zero, set));
        jit.returnToCaller();
        isNoInt32.link(&jit);
        isNoInteger.append(jit.branchIfNotCell(first));
        isNoInteger.append(jit.branchIfNotType(first, StringType));
        returnResult(jit, first);
        isNoInteger.link(&jit);
    }
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
    if (!jit.options().useWideReferences) {
        JumpList isNoTextBehindNothing;
        isNoTextBehindNothing.append(jit.branchPtr(CCallHelpers::NotEqual, Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(emptyString)), first));
        isNoTextBehindNothing.append(jit.branchIfNotCell(second));
        isNoTextBehindNothing.append(jit.branchIfNotType(second, StringType));
        returnResult(jit, second);
        isNoTextBehindNothing.link(&jit);
    }
    joinLongStringGenerically(jit, first, second, set);
    isTextOfInteger.link(&jit);
    auto use = static_cast<ConcatenatedKeyForm>(jit.key().form()) == ConcatenatedKeyForm::Name ? UseOfJoinedText::LikelyName : UseOfJoinedText::LikelyKey;
    jit.callOperationAndReturn(Entry::operationAOTConcatenatePair, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedGPRs,
        .spilledArguments = OperationCall::SpilledArguments { 2, static_cast<uint32_t>(use) },
    });
}

void generateDoubleToString(StubAssembler& jit)
{
    const FPRReg number = jit.operandFPR(0);
    const GPRReg integer = jit.clobberRegister(signatureOf(integerToString()).operands[0].gpr());
    const FPRReg scratch = jit.allocateScratch(FPRInfo::argumentFPR1);
    JumpList isNotInt32;
    branchIfNotExactInt32(jit, number, integer, isNotInt32, scratch);
    jit.signExtend32ToPtr(integer, integer);
    jit.jumpToStub(integerToString());

    isNotInt32.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTDoubleToString, { .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false });
}

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
