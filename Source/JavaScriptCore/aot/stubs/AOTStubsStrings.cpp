/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTRuntimeTable.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "JSString.h"
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

using Saves = OperationCall::Saves;

static constexpr int32_t maxLengthComparedWithoutCall = defaultLimitValue(LimitID::MaxInlineStringCompareLength, hostArchitecture());

static StubSignature signatureOfCharactersInPlace()
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T12) });
    signature.firstResult = gprOf(StubRegister::T12);
    return signature;
}

StubSignature signatureOfRopeCharacters(StubKey)
{
    return signatureOfCharactersInPlace();
}

StubSignature signatureOfFlattenString(StubKey)
{
    return signatureOfCharactersInPlace();
}

StubSignature signatureOfStringsAreEqual(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::T11), gprOf(StubRegister::T12) });
}

StubSignature signatureOfCompareStrings(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::T11), gprOf(StubRegister::T12) });
}

static void loadSubstringCharactersAndReturn(StubAssembler& jit, GPRReg stringAndCharacters, GPRReg base, GPRReg offset)
{
    const GPRReg impl = base;
    loadSubstringBaseAndOffset(jit, stringAndCharacters, base, offset);
    jit.loadPtr(Address(base, JSString::offsetOfValue()), impl);
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), stringAndCharacters);
    Jump is8Bit = jit.branchTest32(CCallHelpers::NonZero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.lshift64(TrustedImm32(1), offset);
    is8Bit.link(&jit);
    jit.addPtr(offset, stringAndCharacters);
    jit.returnToCaller();
}

void generateRopeCharacters(StubAssembler& jit)
{
    const GPRReg string = jit.clobberOperand(0);
    const GPRReg data = jit.result();
    const GPRReg fiber = jit.allocateScratch(StubRegister::T9);
    const GPRReg offset = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(string == data);
    auto loadImplCharacters = [&] {
        jit.loadPtr(Address(fiber, StringImpl::dataOffset()), data);
    };
    jit.loadPtr(Address(string, JSString::offsetOfValue()), fiber);
    Jump isFlat = jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSString::isRopeInPointer));
    Jump notSubstring = jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSRopeString::isSubstringInPointer));
    loadSubstringCharactersAndReturn(jit, string, fiber, offset);

    isFlat.link(&jit);
    loadImplCharacters();
    jit.returnToCaller();

    notSubstring.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTFlatten, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = string, .valueDestination = fiber }, loadImplCharacters);
}

void generateFlattenString(StubAssembler& jit)
{
    const GPRReg string = jit.operand(0);
    const GPRReg fiber = jit.allocateScratch(StubRegister::T9);
    RELEASE_ASSERT(string == jit.result());

    jit.loadPtr(Address(string, JSString::offsetOfValue()), fiber);
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    jit.returnToCaller();

    isRope.link(&jit);
    // The returned impl is unused. Sending it to the scratch register makes the call restore every other register, including the cell's.
    jit.callOperationAndReturn(Entry::operationAOTFlatten, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = string, .valueDestination = fiber });
}

void loadStringLength(StubAssembler& jit, GPRReg string, GPRReg fiber, GPRReg length)
{
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    jit.load32(Address(fiber, StringImpl::lengthMemoryOffset()), length);
    Jump isLoaded = jit.jump();
    isRope.link(&jit);
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    isLoaded.link(&jit);
}

static void compareImplsAndReturn(StubAssembler& jit, Entry leaf, GPRReg firstImpl, GPRReg secondImpl)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1));
    // On x86-64 the second operand arrives in the register of the leaf's first argument.
    if (jit.operand(1) == leafArgumentGPR(0))
        jit.clobberOperand(1);
    const GPRReg firstArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg secondArgument = jit.reuseScratch(StubRegister::A1);
    RELEASE_ASSERT(jit.result() == GPRInfo::returnValueGPR);
    RELEASE_ASSERT(noOverlap(firstImpl, secondImpl, firstArgument, secondArgument));
    jit.move(firstImpl, firstArgument);
    jit.move(secondImpl, secondArgument);
    jit.callLeaf(leaf);
    jit.returnToCaller();
}

static void loadCharacters(StubAssembler& jit, GPRReg firstImplAndCharacters, GPRReg secondImplAndCharacters)
{
    jit.loadPtr(Address(firstImplAndCharacters, StringImpl::dataOffset()), firstImplAndCharacters);
    jit.loadPtr(Address(secondImplAndCharacters, StringImpl::dataOffset()), secondImplAndCharacters);
}

void branchIfCharactersDiffer(StubAssembler& jit, const ComparedImpls& impls, JumpList& differ, JumpList& needsLeaf)
{
    auto [first, second, length, flagsOfBoth, word] = impls;
    RELEASE_ASSERT(noOverlap(first, second, length, flagsOfBoth, word));

    needsLeaf.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthComparedWithoutCall)));
    Jump areBoth8Bit = jit.branchTest32(CCallHelpers::NonZero, flagsOfBoth, TrustedImm32(StringImpl::flagIs8Bit()));
    needsLeaf.append(jit.branchTest32(CCallHelpers::NonZero, Address(first, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    needsLeaf.append(jit.branchTest32(CCallHelpers::NonZero, Address(second, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.lshift32(TrustedImm32(1), length);
    areBoth8Bit.link(&jit);
    loadCharacters(jit, first, second);
    branchIfBytesDiffer(jit, { first, second, length, flagsOfBoth, word }, differ);
}

void generateStringsAreEqual(StubAssembler& jit)
{
    const GPRReg first = jit.operand(0);
    const GPRReg second = jit.operand(1);
    const GPRReg result = jit.result();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    const GPRReg firstFiber = jit.allocateScratch(StubRegister::T13);
    const GPRReg secondFiber = jit.allocateScratch(StubRegister::T14);
    const GPRReg word = jit.allocateScratch(StubRegister::A1);
    const GPRReg length = result;

    JumpList areEqual;
    JumpList areNotEqual;
    JumpList needsLeaf;
    areEqual.append(jit.branchPtr(CCallHelpers::Equal, first, second));
    jit.loadPtr(Address(first, JSString::offsetOfValue()), firstFiber);
    jit.loadPtr(Address(second, JSString::offsetOfValue()), secondFiber);
    jit.or64(firstFiber, secondFiber, scratch);
    Jump oneIsRope = jit.branchTestPtr(CCallHelpers::NonZero, scratch, TrustedImm32(JSString::isRopeInPointer));
    areEqual.append(jit.branchPtr(CCallHelpers::Equal, firstFiber, secondFiber));
    jit.load32(Address(firstFiber, StringImpl::lengthMemoryOffset()), length);
    areNotEqual.append(jit.branch32(CCallHelpers::NotEqual, Address(secondFiber, StringImpl::lengthMemoryOffset()), length));
    jit.load32(Address(firstFiber, StringImpl::flagsOffset()), scratch);
    jit.and32(Address(secondFiber, StringImpl::flagsOffset()), scratch);
    areNotEqual.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(StringImpl::flagIsAtom())));
    branchIfCharactersDiffer(jit, { firstFiber, secondFiber, length, scratch, word }, areNotEqual, needsLeaf);

    areEqual.link(&jit);
    jit.move(TrustedImm32(1), result);
    jit.returnToCaller();

    areNotEqual.link(&jit);
    Label returnFalse = jit.label();
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();

    needsLeaf.link(&jit);
    compareImplsAndReturn(jit, Entry::operationAOTStringImplsAreEqual, firstFiber, secondFiber);

    oneIsRope.link(&jit);
    loadStringLength(jit, first, firstFiber, length);
    loadStringLength(jit, second, secondFiber, scratch);
    jit.branch32(CCallHelpers::NotEqual, length, scratch).linkTo(returnFalse, &jit);
    jit.callOperationAndReturn(Entry::operationAOTStringsAreEqual, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = first, .thirdArgumentIn = second });
}

void branchOnOrderOfFlatStrings(StubAssembler& jit, const OrderedFlatStrings& strings, JumpList& needsLeaf, JumpList& isLess, JumpList& isEqual)
{
    auto [first, second, firstImpl, secondImpl, length, firstWord, secondWord] = strings;
    RELEASE_ASSERT(noOverlap(first, second, firstImpl, secondImpl, length, firstWord, secondWord));

    JumpList differ;
    jit.load32(Address(firstImpl, StringImpl::flagsOffset()), firstWord);
    jit.and32(Address(secondImpl, StringImpl::flagsOffset()), firstWord);
    needsLeaf.append(jit.branchTest32(CCallHelpers::Zero, firstWord, TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(firstImpl, StringImpl::lengthMemoryOffset()), length);
    jit.load32(Address(secondImpl, StringImpl::lengthMemoryOffset()), firstWord);
    jit.moveConditionally32(CCallHelpers::Above, length, firstWord, firstWord, length);
    needsLeaf.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthComparedWithoutCall)));
    loadCharacters(jit, firstImpl, secondImpl);
    branchIfBytesDiffer(jit, { firstImpl, secondImpl, length, firstWord, secondWord }, differ);

    jit.loadPtr(Address(first, JSString::offsetOfValue()), firstImpl);
    jit.loadPtr(Address(second, JSString::offsetOfValue()), secondImpl);
    jit.load32(Address(firstImpl, StringImpl::lengthMemoryOffset()), firstWord);
    jit.load32(Address(secondImpl, StringImpl::lengthMemoryOffset()), secondWord);
    isEqual.append(jit.branch32(CCallHelpers::Equal, firstWord, secondWord));
    Jump compareWords = jit.jump();

    differ.link(&jit);
    jit.byteSwap64(firstWord);
    jit.byteSwap64(secondWord);
    compareWords.link(&jit);
    isLess.append(jit.branch64(CCallHelpers::Below, firstWord, secondWord));
}

// The result is -1, 0 or 1, sign-extended to 64 bits.
void generateCompareStrings(StubAssembler& jit)
{
    const GPRReg first = jit.operand(0);
    const GPRReg second = jit.operand(1);
    const GPRReg result = jit.result();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    const GPRReg firstFiber = jit.allocateScratch(StubRegister::T13);
    const GPRReg secondFiber = jit.allocateScratch(StubRegister::T14);
    const GPRReg word = jit.allocateScratch(StubRegister::A1);

    JumpList needsLeaf;
    JumpList isLess;
    JumpList isEqual;
    isEqual.append(jit.branchPtr(CCallHelpers::Equal, first, second));
    jit.loadPtr(Address(first, JSString::offsetOfValue()), firstFiber);
    jit.loadPtr(Address(second, JSString::offsetOfValue()), secondFiber);
    jit.or64(firstFiber, secondFiber, scratch);
    Jump oneIsRope = jit.branchTestPtr(CCallHelpers::NonZero, scratch, TrustedImm32(JSString::isRopeInPointer));
    branchOnOrderOfFlatStrings(jit, { first, second, firstFiber, secondFiber, result, scratch, word }, needsLeaf, isLess, isEqual);
    jit.move(TrustedImm32(1), result);
    jit.returnToCaller();

    isLess.link(&jit);
    jit.move(TrustedImm64(-1), result);
    jit.returnToCaller();

    isEqual.link(&jit);
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();

    needsLeaf.link(&jit);
    compareImplsAndReturn(jit, Entry::operationAOTCompareStringImpls, firstFiber, secondFiber);

    oneIsRope.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTCompareStrings, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = first, .thirdArgumentIn = second });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
