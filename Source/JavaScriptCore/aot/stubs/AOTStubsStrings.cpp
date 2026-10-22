/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTRuntimeTable.h"
#include "AOTSlotRepresentation.h"
#include "AOTStringBuilderStorage.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "JSString.h"
#include <unicode/utf16.h>
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

StubSignature signatureOfDistinctStringsAreEqual(StubKey key)
{
    return signatureOfStringsAreEqual(key);
}

StubSignature signatureOfStringBuilderAppendString(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    return signature;
}

namespace {

// No move reaches beyond the characters: the last one of each length ends where they end, and lies over the one in front of it where it has to.
struct CharacterCopy {
    NEVER_INLINE void moveTwice(unsigned width) const
    {
        const BaseIndex lastOfSource(source, length, CCallHelpers::TimesOne, -static_cast<int32_t>(width));
        const BaseIndex lastOfTarget(target, length, CCallHelpers::TimesOne, -static_cast<int32_t>(width));
        switch (width) {
        case 8:
            jit.load64(Address(source), first);
            jit.load64(lastOfSource, second);
            jit.store64(first, Address(target));
            jit.store64(second, lastOfTarget);
            return;
        case 4:
            jit.load32(Address(source), first);
            jit.load32(lastOfSource, second);
            jit.store32(first, Address(target));
            jit.store32(second, lastOfTarget);
            return;
        case 2:
            jit.load16(Address(source), first);
            jit.load16(lastOfSource, second);
            jit.store16(first, Address(target));
            jit.store16(second, lastOfTarget);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    NEVER_INLINE void moveSixteenAt(GPRReg index, int32_t displacement) const
    {
        jit.load64(BaseIndex(source, index, CCallHelpers::TimesOne, displacement), first);
        jit.load64(BaseIndex(source, index, CCallHelpers::TimesOne, displacement + 8), second);
        jit.store64(first, BaseIndex(target, index, CCallHelpers::TimesOne, displacement));
        jit.store64(second, BaseIndex(target, index, CCallHelpers::TimesOne, displacement + 8));
    }

    // Without a list of jumps, returns to the caller of the stub.
    NEVER_INLINE void copy(JumpList* isCopied) const
    {
        auto leave = [&] {
            if (isCopied)
                isCopied->append(jit.jump());
            else
                jit.returnToCaller();
        };
        Jump isLong = jit.branch32(CCallHelpers::Above, length, TrustedImm32(16));
        for (unsigned width : { 8u, 4u, 2u }) {
            Jump isShorter = jit.branch32(CCallHelpers::Below, length, TrustedImm32(width));
            moveTwice(width);
            leave();
            isShorter.link(&jit);
        }
        Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, length);
        jit.load8(Address(source), first);
        jit.store8(first, Address(target));
        if (isCopied)
            isCopied->append(isEmpty);
        else
            isEmpty.link(&jit);
        leave();

        isLong.link(&jit);
        jit.move(TrustedImm32(0), offset);
        Label loop = jit.label();
        moveSixteenAt(offset, 0);
        jit.add32(TrustedImm32(16), offset);
        jit.add32(TrustedImm32(16), offset, first);
        jit.branch32(CCallHelpers::BelowOrEqual, first, length).linkTo(loop, &jit);
        moveSixteenAt(length, -16);
        if (!isCopied)
            jit.returnToCaller();
    }

    // The source holds the first word of the rope. The target ends behind the last piece. Whoever meets a piece that is itself a rope leaves by the slow path.
    NEVER_INLINE void copyPiecesOfRope(GPRReg rope, JumpList& slowPath) const
    {
        auto copyPiece = [&] {
            JumpList isCopied;
            jit.loadPtr(Address(source, JSString::offsetOfValue()), source);
            slowPath.append(jit.branchIfRopeStringImpl(source));
            jit.load32(Address(source, StringImpl::lengthMemoryOffset()), length);
            jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
            copy(&isCopied);
            isCopied.link(&jit);
            jit.add64(length, target);
        };
        jit.and64(TrustedImm32(static_cast<int32_t>(JSRopeString::stringMask)), source);
        copyPiece();
        loadSubstringBaseAndOffset(jit, rope, source, offset);
        copyPiece();
        jit.load64(Address(rope, JSRopeString::offsetOfFiber2()), source);
        jit.urshift64(TrustedImm32(JSRopeString::CompactFibers::shiftOfFiber2InUpperWord), source);
        Jump hasTwoPieces = jit.branchTestPtr(CCallHelpers::Zero, source);
        copyPiece();
        hasTwoPieces.link(&jit);
    }

    StubAssembler& jit;
    GPRReg source;
    GPRReg target;
    GPRReg length;
    GPRReg first;
    GPRReg second;
    GPRReg offset;
};

} // anonymous namespace

// A flat string of 8-bit characters that is short enough to be copied and fits into the room that the buffer has left. Everything else is the operation's.
void generateStringBuilderAppendString(StubAssembler& jit)
{
    using Storage = StringBuilderStorage;
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));
    const GPRReg builder = jit.operand(0);
    const GPRReg string = jit.operand(1);
    const GPRReg source = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg target = jit.allocateScratch(StubRegister::T12);
    const GPRReg first = jit.allocateScratch(StubRegister::T14);
    const GPRReg second = jit.allocateScratch(firstStubOperandRegister);
    const GPRReg offset = jit.allocateScratch(StubRegister::T9);
    RELEASE_ASSERT(noOverlap(builder, string, offset, source, length, target, first, second));
    const CharacterCopy characters { jit, source, target, length, first, second, offset };
    JumpList slowPath;

    jit.loadPtr(Address(string, JSString::offsetOfValue()), source);
    Jump isRope = jit.branchIfRopeStringImpl(source);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, Address(source, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(source, StringImpl::lengthMemoryOffset()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, Address(builder, Storage::offsetOfMinLengthAppendedByReference())));
    jit.load32(Address(builder, Storage::offsetOfPosition()), target);
    jit.add64(length, target, first);
    jit.load32(Address(builder, Storage::offsetOfLimit()), second);
    slowPath.append(jit.branch64(CCallHelpers::Above, first, second));
    jit.store32(first, Address(builder, Storage::offsetOfPosition()));
    jit.addPtr(Address(builder, Storage::offsetOfCharacters()), target);
    jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
    characters.copy(nullptr);

    // A short rope of flat 8-bit strings, as `first + " " + last` kept in a field is: its pieces are copied one after the other.
    isRope.link(&jit);
    slowPath.append(jit.branchTestPtr(CCallHelpers::NonZero, source, TrustedImm32(JSRopeString::isSubstringInPointer)));
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, source, TrustedImm32(JSRopeString::is8BitInPointer)));
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, Address(builder, Storage::offsetOfMinLengthAppendedByReference())));
    jit.load32(Address(builder, Storage::offsetOfPosition()), target);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, target));
    jit.add64(length, target, first);
    jit.load32(Address(builder, Storage::offsetOfLimit()), second);
    slowPath.append(jit.branch64(CCallHelpers::Above, first, second));
    jit.addPtr(Address(builder, Storage::offsetOfCharacters()), target);
    characters.copyPiecesOfRope(string, slowPath);
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    jit.add32(length, Address(builder, Storage::offsetOfPosition()));
    jit.returnToCaller();

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringBuilderAppendString, { .returns = OperationCall::Returns::Void, .saves = Saves::CallerSavedRegisters });
}

StubSignature signatureOfStackStringBuilderAppendString(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) });
}

// The same two kinds of strings as above, and a piece of a flat string. The cursor is left alone, so the slow path starts with nothing done.
void generateStackStringBuilderAppendString(StubAssembler& jit)
{
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2) && gprOf(StubRegister::A3) == operationArgumentGPR(3));
    const GPRReg builder = jit.operand(0);
    const GPRReg cursor = jit.operand(1);
    const GPRReg string = jit.operand(2);
    const GPRReg end = jit.result();
    const GPRReg source = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg first = jit.allocateScratch(StubRegister::T12);
    const GPRReg second = jit.allocateScratch(StubRegister::T9);
    const GPRReg offset = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(builder, cursor, string, end, source, length, first, second, offset));
    const Address limit(builder, OBJECT_OFFSETOF(StackStringBuilder, limit));
    JumpList slowPath;

    jit.loadPtr(Address(string, JSString::offsetOfValue()), source);
    Jump isRope = jit.branchIfRopeStringImpl(source);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, Address(source, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(source, StringImpl::lengthMemoryOffset()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(StackStringBuilder::minLengthAppendedByReference)));
    jit.add64(length, cursor, end);
    slowPath.append(jit.branchPtr(CCallHelpers::Above, end, limit));
    jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
    Label copy = jit.label();
    CharacterCopy { jit, source, cursor, length, first, second, offset }.copy(nullptr);

    isRope.link(&jit);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, source, TrustedImm32(JSRopeString::is8BitInPointer)));
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(StackStringBuilder::minLengthAppendedByReference)));
    jit.add64(length, cursor, end);
    slowPath.append(jit.branchPtr(CCallHelpers::Above, end, limit));
    Jump hasPieces = jit.branchTestPtr(CCallHelpers::Zero, source, TrustedImm32(JSRopeString::isSubstringInPointer));
    loadSubstringBaseAndOffset(jit, string, source, offset);
    jit.loadPtr(Address(source, JSString::offsetOfValue()), source);
    jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
    jit.addPtr(offset, source);
    jit.jump().linkTo(copy, &jit);

    hasPieces.link(&jit);
    jit.move(cursor, end);
    CharacterCopy { jit, source, end, length, first, second, offset }.copyPiecesOfRope(string, slowPath);
    jit.returnToCaller();

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStackStringBuilderAppend, { .saves = Saves::CallerSavedRegisters });
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

StubSignature signatureOfStringCodePointAt(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

void generateStringCodePointAt(StubAssembler& jit)
{
    const GPRReg string = jit.operand(0);
    const GPRReg position = jit.operand(1);
    const GPRReg codePoint = jit.result();
    const GPRReg impl = jit.allocateScratch(StubRegister::T9);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg trail = jit.allocateScratch(StubRegister::T10);
    const GPRReg characters = impl;
    const GPRReg surrogateBits = length;
    RELEASE_ASSERT(noOverlap(string, position, codePoint, impl, length, trail));
    const TrustedImm32 surrogateMask(~0x3ff);

    auto readFromImpl = [&] {
        JumpList done;
        jit.move(TrustedImm64(undefinedInteger), codePoint);
        jit.load32(Address(impl, StringImpl::lengthMemoryOffset()), length);
        done.append(jit.branch64(CCallHelpers::AboveOrEqual, position, length));
        Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
        jit.loadPtr(Address(impl, StringImpl::dataOffset()), characters);
        jit.load8(BaseIndex(characters, position, CCallHelpers::TimesOne), codePoint);
        done.append(jit.jump());

        is16Bit.link(&jit);
        jit.loadPtr(Address(impl, StringImpl::dataOffset()), characters);
        jit.load16(BaseIndex(characters, position, CCallHelpers::TimesTwo), codePoint);
        jit.and32(surrogateMask, codePoint, trail);
        done.append(jit.branch32(CCallHelpers::NotEqual, trail, TrustedImm32(0xd800)));
        jit.sub32(TrustedImm32(1), length);
        done.append(jit.branch64(CCallHelpers::AboveOrEqual, position, length));
        jit.load16(BaseIndex(characters, position, CCallHelpers::TimesTwo, sizeof(char16_t)), trail);
        jit.and32(surrogateMask, trail, surrogateBits);
        done.append(jit.branch32(CCallHelpers::NotEqual, surrogateBits, TrustedImm32(0xdc00)));
        jit.lshift32(TrustedImm32(10), codePoint);
        jit.add32(trail, codePoint);
        jit.sub32(TrustedImm32(U16_SURROGATE_OFFSET), codePoint);
        done.link(&jit);
    };

    jit.loadPtr(Address(string, JSString::offsetOfValue()), impl);
    Jump isRope = jit.branchIfRopeStringImpl(impl);
    readFromImpl();
    jit.returnToCaller();

    isRope.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTFlatten, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = string, .valueDestination = impl }, readFromImpl);
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
    branchIfBytesDiffer(jit, { first, second, length, flagsOfBoth, word }, differ, DifferingWords::MayBeUnloaded);
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
    jit.bindEntry(distinctStringsAreEqual());
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
