/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTCaseConversions.h"
#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStringBuilderStorage.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSString.h"
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

using Saves = OperationCall::Saves;

static constexpr int32_t maxLengthComparedWithoutCall = 32;
static constexpr int32_t maxLengthSearchedByBytes = 15;

static StubSignature signatureOfCharactersInPlace()
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T12) });
    signature.firstResult = gprOf(StubRegister::T12);
    return signature;
}

StubSignature signatureOfRopeCharacters(StubKey) { return signatureOfCharactersInPlace(); }
StubSignature signatureOfSubstringCharacters(StubKey)
{
    StubSignature signature = signatureOfCharactersInPlace();
    signature.result = StubResult::TwoGPRs;
    signature.secondResult = gprOf(StubRegister::T9);
    return signature;
}
StubSignature signatureOfFlattenString(StubKey) { return signatureOfCharactersInPlace(); }
StubSignature signatureOfStringCodeUnitAt(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T12), gprOf(StubRegister::T11) }); }
StubSignature signatureOfStringsAreEqual(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T11), gprOf(StubRegister::T12) }); }
StubSignature signatureOfDistinctStringsAreEqual(StubKey key) { return signatureOfStringsAreEqual(key); }
StubSignature signatureOfStringEqualsValue(StubKey key) { return signatureOfStringsAreEqual(key); }
StubSignature signatureOfCompareStrings(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T11), gprOf(StubRegister::T12) }); }
StubSignature signatureOfFindCharacter8(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) }); }
StubSignature signatureOfFindText8(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3), gprOf(StubRegister::A4) }); }
StubSignature signatureOfStringBuilderAppendString(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }
StubSignature signatureOfStringBuilderDiscard(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::A1) }); }
StubSignature signatureOfStackStringBuilderAppendString(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) }); }

namespace {

struct CopyIntoStringBuilder {
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

    NEVER_INLINE void copyPiecesOfRope(GPRReg rope, JumpList& slowPath) const
    {
        auto copyPiece = [&] {
            JumpList isCopied;
            jit.loadPtr(Address(source, JSString::offsetOfValue()), source);
            slowPath.append(jit.branchIfRopeStringImpl(source));
            slowPath.append(jit.branchTest32(CCallHelpers::Zero, Address(source, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
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
    const CopyIntoStringBuilder characters { jit, source, target, length, first, second, offset };
    JumpList slowPath;

    jit.loadPtr(Address(string, JSString::offsetOfValue()), source);
    Jump isRope = jit.branchIfRopeStringImpl(source);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, Address(source, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(source, StringImpl::lengthMemoryOffset()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(Storage::minLengthAppendedByReference)));
    jit.load32(Address(builder, Storage::offsetOfPosition()), target);
    Jump hasText = jit.branchTest32(CCallHelpers::NonZero, target);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(Storage::minLengthAdoptedByFinish)));
    hasText.link(&jit);
    jit.add64(length, target, first);
    jit.load32(Address(builder, Storage::offsetOfLimit()), second);
    slowPath.append(jit.branch64(CCallHelpers::Above, first, second));
    jit.store32(first, Address(builder, Storage::offsetOfPosition()));
    jit.addPtr(Address(builder, Storage::offsetOfCharacters()), target);
    jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
    characters.copy(nullptr);

    isRope.link(&jit);
    slowPath.append(jit.branchTestPtr(CCallHelpers::NonZero, source, TrustedImm32(JSRopeString::isSubstringInPointer)));
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(Storage::minLengthAppendedByReference)));
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

void generateStringBuilderDiscard(StubAssembler& jit)
{
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1));
    const GPRReg builder = jit.operand(0);
    const Address pool(instanceGPR, AOT_INSTANCE_OFFSET(stringBuilder.freeStorage));
    JumpList slowPath;

    slowPath.append(jit.branchTestPtr(CCallHelpers::NonZero, pool));
    slowPath.append(jit.branchTest32(CCallHelpers::NonZero, Address(builder, StringBuilderStorage::offsetOfPosition())));
    slowPath.append(jit.branchTest32(CCallHelpers::NonZero, Address(builder, StringBuilderStorage::offsetOfLengthOutsideBuffer())));
    jit.storePtr(builder, pool);
    jit.returnToCaller();

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringBuilderDiscard, { .returns = OperationCall::Returns::Void, .saves = Saves::CallerSavedRegisters, .checksException = false });
}

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
    CopyIntoStringBuilder { jit, source, cursor, length, first, second, offset }.copy(nullptr);

    isRope.link(&jit);
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    slowPath.append(jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(StackStringBuilder::minLengthAppendedByReference)));
    jit.add64(length, cursor, end);
    slowPath.append(jit.branchPtr(CCallHelpers::Above, end, limit));
    Jump hasPieces = jit.branchTestPtr(CCallHelpers::Zero, source, TrustedImm32(JSRopeString::isSubstringInPointer));
    loadSubstringBaseAndOffset(jit, string, source, offset);
    jit.loadPtr(Address(source, JSString::offsetOfValue()), source);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, Address(source, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(source, StringImpl::dataOffset()), source);
    jit.addPtr(offset, source);
    jit.jump().linkTo(copy, &jit);

    hasPieces.link(&jit);
    jit.move(cursor, end);
    CopyIntoStringBuilder { jit, source, end, length, first, second, offset }.copyPiecesOfRope(string, slowPath);
    jit.returnToCaller();

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStackStringBuilderAppend, { .saves = Saves::CallerSavedRegisters });
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
    loadSubstringBaseAndOffset(jit, string, fiber, offset);
    jit.loadPtr(Address(fiber, JSString::offsetOfValue()), fiber);
    loadImplCharacters();
    Jump is8Bit = jit.branchTest32(CCallHelpers::NonZero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.lshift64(TrustedImm32(1), offset);
    is8Bit.link(&jit);
    jit.addPtr(offset, data);
    jit.returnToCaller();

    isFlat.link(&jit);
    loadImplCharacters();
    jit.returnToCaller();

    notSubstring.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTFlatten, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = string, .valueDestination = fiber }, loadImplCharacters);
}

void generateSubstringCharacters(StubAssembler& jit)
{
    const GPRReg string = jit.clobberOperand(0);
    const GPRReg characters = jit.result();
    const GPRReg impl = jit.secondResult();
    const GPRReg offset = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(string == characters && noOverlap(characters, impl, offset));
    loadSubstringBaseAndOffset(jit, string, impl, offset);
    jit.loadPtr(Address(impl, JSString::offsetOfValue()), impl);
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), characters);
    Jump is8Bit = jit.branchTest32(CCallHelpers::NonZero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.lshift64(TrustedImm32(1), offset);
    is8Bit.link(&jit);
    jit.addPtr(offset, characters);
    jit.returnToCaller();
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
    jit.callOperationAndReturn(Entry::operationAOTFlatten, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = string, .valueDestination = fiber });
}

void generateStringCodeUnitAt(StubAssembler& jit)
{
    const GPRReg string = jit.operand(0);
    const GPRReg index = jit.operand(1);
    const GPRReg unit = jit.result();
    const GPRReg fiber = jit.allocateScratch(StubRegister::T9);
    RELEASE_ASSERT(noOverlap(string, index, unit, fiber));

    jit.loadPtr(Address(string, JSString::offsetOfValue()), fiber);
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    jit.zeroExtend32ToWord(index, unit);
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
    jit.load8(BaseIndex(fiber, unit, CCallHelpers::TimesOne), unit);
    jit.returnToCaller();

    is16Bit.link(&jit);
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
    jit.load16(BaseIndex(fiber, unit, CCallHelpers::TimesTwo), unit);
    jit.returnToCaller();

    isRope.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringCodeUnitAt, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = string, .thirdArgumentIn = index });
}

static void findCodeUnitAndReturn(StubAssembler& jit, GPRReg characters, GPRReg length, GPRReg codeUnit, GPRReg position, GPRReg index, GPRReg unit)
{
    RELEASE_ASSERT(characters == leafArgumentGPR(0) && length == leafArgumentGPR(1) && codeUnit == leafArgumentGPR(2) && position == leafArgumentGPR(3));
    Jump isPastEnd = jit.branch32(CCallHelpers::AboveOrEqual, position, length);
    Jump needsLeaf = jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthSearchedByBytes));
    jit.zeroExtend32ToWord(position, index);
    Label next = jit.label();
    jit.load8(BaseIndex(characters, index, CCallHelpers::TimesOne), unit);
    Jump isFound = jit.branch32(CCallHelpers::Equal, unit, codeUnit);
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::Below, index, length).linkTo(next, &jit);

    isPastEnd.link(&jit);
    returnResult(jit, TrustedImm64(-1));

    isFound.link(&jit);
    returnResult(jit, index);

    needsLeaf.link(&jit);
    jit.callLeaf(Entry::operationAOTStringFindCharacter8);
    jit.returnToCaller();
}

void generateFindCharacter8(StubAssembler& jit)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1));
    static_assert(leafArgumentGPR(2) == gprOf(StubRegister::A2) && leafArgumentGPR(3) == gprOf(StubRegister::A3));
    const GPRReg characters = jit.operand(0);
    const GPRReg length = jit.operand(1);
    const GPRReg codeUnit = jit.operand(2);
    const GPRReg position = jit.operand(3);
    const GPRReg result = jit.result();
    const GPRReg index = result == characters ? jit.allocateScratch(StubRegister::A5) : result;
    const GPRReg unit = jit.allocateScratch(StubRegister::A4);
    RELEASE_ASSERT(result == GPRInfo::returnValueGPR);
    RELEASE_ASSERT(noOverlap(characters, length, codeUnit, position, index, unit));

    findCodeUnitAndReturn(jit, characters, length, codeUnit, position, index, unit);
}

void generateFindText8(StubAssembler& jit)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1) && leafArgumentGPR(2) == gprOf(StubRegister::A2));
    static_assert(leafArgumentGPR(3) == gprOf(StubRegister::A3) && leafArgumentGPR(4) == gprOf(StubRegister::A4));
    for (unsigned index : { 2, 3, 4 })
        jit.clobberOperand(index);
    const GPRReg characters = jit.operand(0);
    const GPRReg length = jit.operand(1);
    const GPRReg sought = jit.operand(2);
    const GPRReg lengthOfSought = jit.operand(3);
    const GPRReg position = jit.operand(4);
    const GPRReg result = jit.result();
    const GPRReg index = result == characters ? jit.allocateScratch(StubRegister::A5) : result;
    RELEASE_ASSERT(result == GPRInfo::returnValueGPR);
    RELEASE_ASSERT(noOverlap(characters, length, sought, lengthOfSought, position, index));

    Jump isLonger = jit.branch32(CCallHelpers::NotEqual, lengthOfSought, TrustedImm32(1));
    jit.load8(Address(sought), sought);
    jit.move(position, lengthOfSought);
    findCodeUnitAndReturn(jit, characters, length, sought, lengthOfSought, index, position);

    isLonger.link(&jit);
    jit.callLeaf(Entry::operationAOTStringFind8);
    jit.returnToCaller();
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

void branchIfCharactersDiffer(StubAssembler& jit, const ComparedImpls& impls, JumpList& differ, JumpList& needsLeaf)
{
    auto [first, second, length, flagsOfBoth, word] = impls;
    RELEASE_ASSERT(noOverlap(first, second, length, flagsOfBoth, word));

    Jump isLong = jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthComparedWithoutCall));
    auto countBytes = [&] {
        Jump areBoth8Bit = jit.branchTest32(CCallHelpers::NonZero, flagsOfBoth, TrustedImm32(StringImpl::flagIs8Bit()));
        needsLeaf.append(jit.branchTest32(CCallHelpers::NonZero, Address(first, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        needsLeaf.append(jit.branchTest32(CCallHelpers::NonZero, Address(second, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        jit.lshift32(TrustedImm32(1), length);
        areBoth8Bit.link(&jit);
    };
    countBytes();
    jit.loadPtr(Address(first, StringImpl::dataOffset()), first);
    jit.loadPtr(Address(second, StringImpl::dataOffset()), second);
    branchIfBytesDiffer(jit, { first, second, length, flagsOfBoth, word }, differ, DifferingWords::MayBeUnloaded);
    Jump areSame = jit.jump();

    isLong.link(&jit);
    static_assert(maxLengthComparedWithoutCall >= static_cast<int32_t>(sizeof(uint64_t)));
    countBytes();
    const GPRReg otherWord = flagsOfBoth;
    jit.loadPtr(Address(first, StringImpl::dataOffset()), word);
    jit.load64(Address(word), word);
    jit.loadPtr(Address(second, StringImpl::dataOffset()), otherWord);
    jit.load64(Address(otherWord), otherWord);
    differ.append(jit.branch64(CCallHelpers::NotEqual, word, otherWord));
    jit.loadPtr(Address(first, StringImpl::dataOffset()), word);
    jit.load64(BaseIndex(word, length, CCallHelpers::TimesOne, -static_cast<int32_t>(sizeof(uint64_t))), word);
    jit.loadPtr(Address(second, StringImpl::dataOffset()), otherWord);
    jit.load64(BaseIndex(otherWord, length, CCallHelpers::TimesOne, -static_cast<int32_t>(sizeof(uint64_t))), otherWord);
    differ.append(jit.branch64(CCallHelpers::NotEqual, word, otherWord));
    needsLeaf.append(jit.jump());

    areSame.link(&jit);
}

static void load8BitCharactersInPlace(StubAssembler& jit, GPRReg string, GPRReg fiber, GPRReg offset, JumpList& isOther)
{
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    isOther.append(jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
    Jump hasCharacters = jit.jump();
    isRope.link(&jit);
    load8BitCharactersOfSubstring(jit, string, fiber, offset, isOther);
    hasCharacters.link(&jit);
}

void branchIfCharactersInPlaceDiffer(StubAssembler& jit, const ComparedStrings& strings, JumpList& differ, JumpList& needsOperation)
{
    auto [first, second, firstFiber, secondFiber, length, offset, word] = strings;
    needsOperation.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthComparedWithoutCall)));
    load8BitCharactersInPlace(jit, first, firstFiber, offset, needsOperation);
    load8BitCharactersInPlace(jit, second, secondFiber, offset, needsOperation);
    branchIfBytesDiffer(jit, { firstFiber, secondFiber, length, offset, word }, differ, DifferingWords::MayBeUnloaded);
}

void branchUnlessTextStandsAtPlace(StubAssembler& jit, const TextSoughtAtOnePlace& text, PlaceOfSoughtText where, JumpList& isNotThere, JumpList& needsOperation)
{
    auto [string, sought, place, characters, soughtCharacters, length, word] = text;
    RELEASE_ASSERT(noOverlap(string, sought, place, characters, soughtCharacters, length, word));
    JumpList standsThere;
    auto findPlace = [&] {
        isNotThere.append(jit.branch32(CCallHelpers::Below, word, length));
        jit.sub32(length, word);
        switch (where) {
        case PlaceOfSoughtText::IsStart:
            break;
        case PlaceOfSoughtText::IsEnd:
            jit.move(word, place);
            break;
        case PlaceOfSoughtText::IsGivenAndLast:
            needsOperation.append(jit.branch32(CCallHelpers::Below, place, word));
            [[fallthrough]];
        case PlaceOfSoughtText::IsGiven:
            isNotThere.append(jit.branch32(CCallHelpers::Above, place, word));
            break;
        }
    };
    jit.loadPtr(Address(string, JSString::offsetOfValue()), characters);
    jit.loadPtr(Address(sought, JSString::offsetOfValue()), soughtCharacters);

    Jump stringIsRope = jit.branchIfRopeStringImpl(characters);
    Jump soughtIsRope = jit.branchIfRopeStringImpl(soughtCharacters);
    jit.load32(Address(soughtCharacters, StringImpl::lengthMemoryOffset()), length);
    jit.load32(Address(characters, StringImpl::lengthMemoryOffset()), word);
    findPlace();
    standsThere.append(jit.branchTest32(CCallHelpers::Zero, length));
    needsOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    needsOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(soughtCharacters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    jit.loadPtr(Address(soughtCharacters, StringImpl::dataOffset()), soughtCharacters);
    if (where != PlaceOfSoughtText::IsStart)
        jit.addPtr(place, characters);
    jit.load8(Address(characters), word);
    jit.load8(Address(soughtCharacters), place);
    isNotThere.append(jit.branch32(CCallHelpers::NotEqual, word, place));
    standsThere.append(jit.branch32(CCallHelpers::Equal, length, TrustedImm32(1)));
    needsOperation.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthComparedWithoutCall)));
    Jump hasCharactersOfBoth = jit.jump();

    stringIsRope.link(&jit);
    soughtIsRope.link(&jit);
    loadStringLength(jit, sought, soughtCharacters, length);
    loadStringLength(jit, string, characters, word);
    findPlace();
    needsOperation.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthComparedWithoutCall)));
    JumpList hasNoCharactersInPlace;
    load8BitCharactersInPlace(jit, string, characters, word, where == PlaceOfSoughtText::IsEnd ? hasNoCharactersInPlace : needsOperation);
    if (where != PlaceOfSoughtText::IsStart)
        jit.addPtr(place, characters);
    jit.and32(TrustedImm32(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), soughtCharacters, word);
    Jump isJoined = jit.branch32(CCallHelpers::Equal, word, TrustedImm32(JSString::isRopeInPointer));
    load8BitCharactersInPlace(jit, sought, soughtCharacters, word, needsOperation);
    hasCharactersOfBoth.link(&jit);
    branchIfBytesDiffer(jit, { characters, soughtCharacters, length, place, word }, isNotThere, DifferingWords::MayBeUnloaded);
    standsThere.append(jit.jump());

    isJoined.link(&jit);
    const GPRReg part = place;
    auto comparePart = [&] {
        jit.loadPtr(Address(part, JSString::offsetOfValue()), soughtCharacters);
        loadStringLength(jit, part, soughtCharacters, length);
        load8BitCharactersInPlace(jit, part, soughtCharacters, word, needsOperation);
        branchIfBytesDiffer(jit, { characters, soughtCharacters, length, place, word }, isNotThere, DifferingWords::MayBeUnloaded, StartsOfEqualBytes::EndBehindBytes);
    };
    jit.and64(TrustedImm32(static_cast<int32_t>(JSRopeString::stringMask)), soughtCharacters, part);
    comparePart();
    loadSubstringBaseAndOffset(jit, sought, part, word);
    comparePart();
    jit.load64(Address(sought, JSRopeString::offsetOfFiber2()), part);
    jit.urshift64(TrustedImm32(JSRopeString::CompactFibers::shiftOfFiber2InUpperWord), part);
    standsThere.append(jit.branchTestPtr(CCallHelpers::Zero, part));
    comparePart();
    if (where == PlaceOfSoughtText::IsEnd) {
        standsThere.append(jit.jump());
        hasNoCharactersInPlace.link(&jit);
        jit.loadPtr(Address(string, JSString::offsetOfValue()), characters);
        needsOperation.append(jit.branchIfNotRopeStringImpl(characters));
        needsOperation.append(jit.branchTestPtr(CCallHelpers::NonZero, characters, TrustedImm32(JSRopeString::isSubstringInPointer)));
        needsOperation.append(jit.branchTest32(CCallHelpers::Zero, length));
        needsOperation.append(jit.branchIfRopeStringImpl(soughtCharacters));
        needsOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(soughtCharacters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        const GPRReg lastPart = characters;
        loadSubstringBaseAndOffset(jit, string, lastPart, word);
        Jump hasTwoParts = jit.branchTestPtr(CCallHelpers::Zero, word);
        jit.move(word, lastPart);
        hasTwoParts.link(&jit);
        jit.loadPtr(Address(lastPart, JSString::offsetOfValue()), lastPart);
        needsOperation.append(jit.branchIfRopeStringImpl(lastPart));
        needsOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(lastPart, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        jit.load32(Address(lastPart, StringImpl::lengthMemoryOffset()), word);
        needsOperation.append(jit.branchTest32(CCallHelpers::Zero, word));
        jit.loadPtr(Address(lastPart, StringImpl::dataOffset()), lastPart);
        jit.load8(BaseIndex(lastPart, word, CCallHelpers::TimesOne, -1), lastPart);
        jit.loadPtr(Address(soughtCharacters, StringImpl::dataOffset()), word);
        jit.load8(BaseIndex(word, length, CCallHelpers::TimesOne, -1), word);
        isNotThere.append(jit.branch32(CCallHelpers::NotEqual, lastPart, word));
        needsOperation.append(jit.jump());
    }
    standsThere.link(&jit);
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
    Label areDistinct = jit.label();
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
    Label returnTrue = jit.label();
    returnResult(jit, TrustedImm32(1));

    areNotEqual.link(&jit);
    Label returnFalse = jit.label();
    returnResult(jit, TrustedImm32(0));

    needsLeaf.link(&jit);
    compareImplsAndReturn(jit, Entry::operationAOTStringImplsAreEqual, firstFiber, secondFiber);

    oneIsRope.link(&jit);
    loadStringLength(jit, first, firstFiber, length);
    loadStringLength(jit, second, secondFiber, scratch);
    jit.branch32(CCallHelpers::NotEqual, length, scratch).linkTo(returnFalse, &jit);
    JumpList differ;
    JumpList needsOperation;
    branchIfCharactersInPlaceDiffer(jit, { first, second, firstFiber, secondFiber, length, scratch, word }, differ, needsOperation);
    returnResult(jit, TrustedImm32(1));

    differ.linkTo(returnFalse, &jit);
    needsOperation.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringsAreEqual, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = first, .thirdArgumentIn = second });

    jit.bindEntry(stringEqualsValue());
    jit.branchPtr(CCallHelpers::Equal, first, second).linkTo(returnTrue, &jit);
    jit.branchIfNotCell(second).linkTo(returnFalse, &jit);
    jit.branchIfNotString(second).linkTo(returnFalse, &jit);
    jit.jump().linkTo(areDistinct, &jit);
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
    jit.loadPtr(Address(firstImpl, StringImpl::dataOffset()), firstImpl);
    jit.loadPtr(Address(secondImpl, StringImpl::dataOffset()), secondImpl);
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
    returnResult(jit, TrustedImm32(1));

    isLess.link(&jit);
    returnResult(jit, TrustedImm64(-1));

    isEqual.link(&jit);
    returnResult(jit, TrustedImm32(0));

    needsLeaf.link(&jit);
    compareImplsAndReturn(jit, Entry::operationAOTCompareStringImpls, firstFiber, secondFiber);

    oneIsRope.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTCompareStrings, { .saves = Saves::CallerSavedRegisters, .secondArgumentIn = first, .thirdArgumentIn = second });
}

PreludeExit emitStringLocaleComparePrelude(StubAssembler& jit)
{
    if (jit.key().family() != StubFamily::CallOperation || jit.signature().result != StubResult::GPR)
        return PreludeExit::CallsOperation;

    static_assert(areDistinctOnEveryISA(StubRegister::R0, StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A4, StubRegister::A5, StubRegister::T9, StubRegister::T12));
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));
    constexpr int32_t wordSize = sizeof(uint64_t);
    constexpr GPRReg left = operationArgumentGPR(1);
    constexpr GPRReg right = operationArgumentGPR(2);
    const GPRReg leftCharacters = jit.allocateScratch(StubRegister::A3);
    const GPRReg rightCharacters = jit.allocateScratch(StubRegister::A4);
    const GPRReg remaining = jit.allocateScratch(StubRegister::A5);
    const GPRReg leftUnit = jit.allocateScratch(StubRegister::T9);
    const GPRReg rightUnit = jit.allocateScratch(StubRegister::T12);
    const GPRReg weights = jit.allocateScratch(StubRegister::R0);
    const GPRReg nonASCIIBits = weights;

    JumpList isEqual;
    JumpList callsOperation;
    JumpList comparesBytes;
    JumpList commonPartIsEqual;

    isEqual.append(jit.branchPtr(CCallHelpers::Equal, left, right));
    jit.loadPtr(Address(left, JSString::offsetOfValue()), leftCharacters);
    jit.loadPtr(Address(right, JSString::offsetOfValue()), rightCharacters);
    jit.or64(leftCharacters, rightCharacters, leftUnit);
    callsOperation.append(jit.branchTestPtr(CCallHelpers::NonZero, leftUnit, TrustedImm32(JSString::isRopeInPointer)));
    isEqual.append(jit.branchPtr(CCallHelpers::Equal, leftCharacters, rightCharacters));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), leftUnit);
    callsOperation.append(jit.branchTest8(CCallHelpers::Zero, Address(leftUnit, JSGlobalObject::offsetOfCanDoASCIIUCADUCETLocaleCompare())));
    jit.load32(Address(leftCharacters, StringImpl::flagsOffset()), leftUnit);
    jit.load32(Address(rightCharacters, StringImpl::flagsOffset()), rightUnit);
    jit.and32(rightUnit, leftUnit);
    callsOperation.append(jit.branchTest32(CCallHelpers::Zero, leftUnit, TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(leftCharacters, StringImpl::lengthMemoryOffset()), remaining);
    jit.load32(Address(rightCharacters, StringImpl::lengthMemoryOffset()), rightUnit);
    Jump leftIsNotLonger = jit.branch32(CCallHelpers::BelowOrEqual, remaining, rightUnit);
    jit.move(rightUnit, remaining);
    leftIsNotLonger.link(&jit);
    jit.loadPtr(Address(leftCharacters, StringImpl::dataOffset()), leftCharacters);
    jit.loadPtr(Address(rightCharacters, StringImpl::dataOffset()), rightCharacters);

    comparesBytes.append(jit.branch32(CCallHelpers::Below, remaining, TrustedImm32(wordSize)));
    jit.move(TrustedImm64(0x8080808080808080ll), nonASCIIBits);
    Label wordLoop = jit.label();
    jit.load64(Address(leftCharacters), leftUnit);
    jit.load64(Address(rightCharacters), rightUnit);
    comparesBytes.append(jit.branchTest64(CCallHelpers::NonZero, leftUnit, nonASCIIBits));
    comparesBytes.append(jit.branchTest64(CCallHelpers::NonZero, rightUnit, nonASCIIBits));
    Jump wordsDiffer = jit.branch64(CCallHelpers::NotEqual, leftUnit, rightUnit);
    jit.addPtr(TrustedImm32(wordSize), leftCharacters);
    jit.addPtr(TrustedImm32(wordSize), rightCharacters);
    jit.sub32(TrustedImm32(wordSize), remaining);
    jit.branch32(CCallHelpers::AboveOrEqual, remaining, TrustedImm32(wordSize)).linkTo(wordLoop, &jit);
    comparesBytes.append(jit.jump());
    wordsDiffer.link(&jit);
    jit.xor64(rightUnit, leftUnit);
    jit.countTrailingZeros64(leftUnit, leftUnit);
    jit.urshift64(TrustedImm32(3), leftUnit);
    jit.add64(leftUnit, leftCharacters);
    jit.add64(leftUnit, rightCharacters);
    jit.sub32(leftUnit, remaining);

    comparesBytes.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(collation.ducetLevel1Weights)), weights);
    commonPartIsEqual.append(jit.branchTest32(CCallHelpers::Zero, remaining));
    Label byteLoop = jit.label();
    jit.load8(Address(leftCharacters), leftUnit);
    jit.load8(Address(rightCharacters), rightUnit);
    Jump bytesDiffer = jit.branch32(CCallHelpers::NotEqual, leftUnit, rightUnit);
    callsOperation.append(jit.branch32(CCallHelpers::AboveOrEqual, leftUnit, TrustedImm32(0x80)));
    Label advance = jit.label();
    jit.addPtr(TrustedImm32(1), leftCharacters);
    jit.addPtr(TrustedImm32(1), rightCharacters);
    jit.sub32(TrustedImm32(1), remaining);
    jit.branchTest32(CCallHelpers::NonZero, remaining).linkTo(byteLoop, &jit);
    commonPartIsEqual.append(jit.jump());

    bytesDiffer.link(&jit);
    jit.load8(BaseIndex(weights, leftUnit, CCallHelpers::TimesOne), leftUnit);
    jit.load8(BaseIndex(weights, rightUnit, CCallHelpers::TimesOne), rightUnit);
    callsOperation.append(jit.branchTest32(CCallHelpers::Zero, leftUnit));
    callsOperation.append(jit.branchTest32(CCallHelpers::Zero, rightUnit));
    jit.branch32(CCallHelpers::Equal, leftUnit, rightUnit).linkTo(advance, &jit);
    Jump leftIsGreater = jit.branch32(CCallHelpers::Above, leftUnit, rightUnit);
    returnResult(jit, TrustedImm64(-1));
    leftIsGreater.link(&jit);
    returnResult(jit, TrustedImm32(1));

    commonPartIsEqual.link(&jit);
    jit.loadPtr(Address(left, JSString::offsetOfValue()), leftUnit);
    jit.loadPtr(Address(right, JSString::offsetOfValue()), rightUnit);
    jit.load32(Address(leftUnit, StringImpl::lengthMemoryOffset()), leftUnit);
    jit.load32(Address(rightUnit, StringImpl::lengthMemoryOffset()), rightUnit);
    callsOperation.append(jit.branch32(CCallHelpers::Equal, leftUnit, rightUnit));
    Jump leftIsLonger = jit.branch32(CCallHelpers::Above, leftUnit, rightUnit);
    jit.load8(Address(rightCharacters), rightUnit);
    jit.load8(BaseIndex(weights, rightUnit, CCallHelpers::TimesOne), rightUnit);
    callsOperation.append(jit.branchTest32(CCallHelpers::Zero, rightUnit));
    returnResult(jit, TrustedImm64(-1));
    leftIsLonger.link(&jit);
    jit.load8(Address(leftCharacters), leftUnit);
    jit.load8(BaseIndex(weights, leftUnit, CCallHelpers::TimesOne), leftUnit);
    callsOperation.append(jit.branchTest32(CCallHelpers::Zero, leftUnit));
    returnResult(jit, TrustedImm32(1));

    isEqual.link(&jit);
    returnResult(jit, TrustedImm32(0));

    callsOperation.link(&jit);
    return PreludeExit::CallsOperation;
}

static void returnRememberedCaseConversion(StubAssembler& jit, CaseConversions::LetterCase letterCase, GPRReg string, GPRReg source, GPRReg index, GPRReg table, GPRReg entry, GPRReg sourceOfEntry)
{
    RELEASE_ASSERT(noOverlap(string, source, index, table, entry, sourceOfEntry, instanceGPR, cellCageBaseGPR));
    static_assert(cell32Shift == MarkedBlockCage::shift);
    if (jit.options().useWideReferences)
        return;
    JumpList isRemembered;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.caseConversions)), table);
    Jump hasNoTable = jit.branchTestPtr(CCallHelpers::Zero, table);
    jit.move(string, source);
    jit.sub64(cellCageBaseGPR, source);
    jit.urshift64(TrustedImm32(cell32Shift), source);
    jit.mul32(TrustedImm32(static_cast<int32_t>(CaseConversions::multiplier)), source, index);
    jit.urshift32(TrustedImm32(CaseConversions::shift), index);
    jit.lshift32(TrustedImm32(CaseConversions::setShift), index);
    for (size_t way = 0; way < CaseConversions::ways; ++way) {
        jit.load64(BaseIndex(table, index, CCallHelpers::TimesEight, static_cast<int32_t>(CaseConversions::indexOf(0, letterCase, way) * sizeof(uint64_t))), entry);
        jit.urshift64(entry, TrustedImm32(32), sourceOfEntry);
        isRemembered.append(jit.branch64(CCallHelpers::Equal, sourceOfEntry, source));
    }
    Jump isNotRemembered = jit.jump();

    isRemembered.link(&jit);
    jit.zeroExtend32ToWord(entry, entry);
    decodeCell32(jit, entry, jit.result());
    jit.returnToCaller();

    hasNoTable.link(&jit);
    isNotRemembered.link(&jit);
}

static PreludeExit emitUnchangedCasePrelude(StubAssembler& jit, CaseConversions::LetterCase letterCase)
{
    const uint8_t firstConvertedLetter = letterCase == CaseConversions::LetterCase::Lower ? 'A' : 'a';
    const uint8_t lastConvertedLetter = letterCase == CaseConversions::LetterCase::Lower ? 'Z' : 'z';
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
    returnResult(jit, string);

    mayChange.link(&jit);
    returnRememberedCaseConversion(jit, letterCase, string, scanned.size, scanned.word, scanned.bytes, scanned.atLeastFirst, scanned.addendForFirst);
    return PreludeExit::CallsOperation;
}

PreludeExit emitToLowerCasePrelude(StubAssembler& jit) { return emitUnchangedCasePrelude(jit, CaseConversions::LetterCase::Lower); }
PreludeExit emitToUpperCasePrelude(StubAssembler& jit) { return emitUnchangedCasePrelude(jit, CaseConversions::LetterCase::Upper); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
