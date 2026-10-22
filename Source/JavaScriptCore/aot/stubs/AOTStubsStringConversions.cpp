/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "JSString.h"
#include "NumericStrings.h"
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

StubSignature signatureOfIntegerToString(StubKey)
{
    StubSignature signature;
#if CPU(ARM64)
    signature.addOperand(gprOf(StubRegister::T11));
#else
    signature.addOperand(gprOf(StubRegister::A1));
#endif
    signature.result = StubResult::GPR;
    signature.firstResult = gprOf(StubRegister::R0);
    return signature;
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
    jit.callOperationAndReturn(Entry::operationAOTIntToString, {
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = value,
    });
}

// StringKeyCache::tryFindCaseConversion(): what the conversion of this very string gave the last time.
static void returnRememberedCaseConversion(StubAssembler& jit, StringKeyCache::LetterCase letterCase, GPRReg string, GPRReg source, GPRReg index, GPRReg entry)
{
    RELEASE_ASSERT(noOverlap(string, source, index, entry, instanceGPR, cellCageBaseGPR));
    if (jit.options().useWideReferences)
        return;
    JumpList isNotRemembered;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.caseConversions)), entry);
    isNotRemembered.append(jit.branchTestPtr(CCallHelpers::Zero, entry));
    // Of a string outside the cage this has more than 32 bits, and equals the upper half of no entry.
    jit.move(string, source);
    jit.sub64(cellCageBaseGPR, source);
    jit.urshift64(TrustedImm32(cell32Shift), source);
    jit.mul32(TrustedImm32(static_cast<int32_t>(StringKeyCache::caseConversionMultiplier)), source, index);
    jit.urshift32(TrustedImm32(StringKeyCache::caseConversionShift), index);
    jit.lshift32(TrustedImm32(1), index);
    jit.load64(BaseIndex(entry, index, CCallHelpers::TimesEight, static_cast<int32_t>(static_cast<size_t>(letterCase) * sizeof(uint64_t))), entry);
    jit.move(entry, index);
    jit.urshift64(TrustedImm32(32), index);
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, index, source));
    jit.zeroExtend32ToWord(entry, entry);
    decodeCell32(jit, entry, jit.result());
    jit.returnToCaller();

    isNotRemembered.link(&jit);
}

static PreludeExit emitUnchangedCasePrelude(StubAssembler& jit, StringKeyCache::LetterCase letterCase)
{
    const uint8_t firstConvertedLetter = letterCase == StringKeyCache::LetterCase::Lower ? 'A' : 'a';
    const uint8_t lastConvertedLetter = letterCase == StringKeyCache::LetterCase::Lower ? 'Z' : 'z';
    // A call that preserves registers has none to scan with.
    if (jit.key().family() != StubFamily::CallOperation || jit.signature().result != StubResult::GPR)
        return PreludeExit::CallsOperation;

    constexpr GPRReg string = operationArgumentGPR(1);
    const GPRReg impl = jit.allocateScratch(StubRegister::A2);
    const ScannedBytes scanned {
        .bytes = impl,
        .size = jit.allocateScratch(StubRegister::A3),
        .word = jit.allocateScratch(StubRegister::A4),
        .atLeastFirst = jit.allocateScratch(StubRegister::A5),
        .aboveLast = jit.allocateScratch(StubRegister::R0),
        .addendForFirst = jit.allocateScratch(StubRegister::T9),
        .addendForLast = jit.allocateScratch(StubRegister::T12),
    };
    RELEASE_ASSERT(noOverlap(string, scanned.bytes, scanned.size, scanned.word, scanned.atLeastFirst, scanned.aboveLast, scanned.addendForFirst, scanned.addendForLast));

    JumpList mayChange;
    jit.loadPtr(Address(string, JSString::offsetOfValue()), impl);
    mayChange.append(jit.branchIfRopeStringImpl(impl));
    mayChange.append(jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(impl, StringImpl::lengthMemoryOffset()), scanned.size);
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), scanned.bytes);
    branchUnlessASCIIOutsideRange(jit, scanned, firstConvertedLetter, lastConvertedLetter, mayChange);
    jit.move(string, jit.result());
    jit.returnToCaller();

    mayChange.link(&jit);
    returnRememberedCaseConversion(jit, letterCase, string, scanned.size, scanned.word, scanned.bytes);
    return PreludeExit::CallsOperation;
}

PreludeExit emitToLowerCasePrelude(StubAssembler& jit)
{
    return emitUnchangedCasePrelude(jit, StringKeyCache::LetterCase::Lower);
}

PreludeExit emitToUpperCasePrelude(StubAssembler& jit)
{
    return emitUnchangedCasePrelude(jit, StringKeyCache::LetterCase::Upper);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
