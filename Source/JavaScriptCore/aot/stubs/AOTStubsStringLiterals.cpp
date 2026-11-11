/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStringLiterals.h"

#if ENABLE(AOT)

#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTOperationData.h"
#include "AOTRuntimeTable.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsStrings.h"
#include "JSString.h"
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

using Saves = OperationCall::Saves;

static StubSignature tableComparisonSignature(StubRegister subject)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(subject) });
    signature.immediate = gprOf(stubImmediateRegister);
    return signature;
}

static StubSignature registerComparisonSignature()
{
    StubSignature signature = StubSignature::withoutResult({ gprOf(comparedSubjectRegister), gprOf(comparedTextRegister) });
    signature.result = StubResult::Flags;
    signature.trueCondition = MacroAssembler::Zero;
    return signature;
}

StubSignature signatureOfStringEqualsLiteral(StubKey) { return tableComparisonSignature(comparedSubjectRegister).standingBehindEntryThatFillsOperandInFront(0); }
StubSignature signatureOfValueEqualsLiteral(StubKey) { return tableComparisonSignature(comparedSubjectRegister).fillingOperandInFront(0); }
StubSignature signatureOfStringEqualsLiteralOfLength(StubKey) { return registerComparisonSignature().standingBehindEntryThatFillsOperandInFront(0); }
StubSignature signatureOfValueEqualsLiteralOfLength(StubKey) { return registerComparisonSignature().fillingOperandInFront(0); }
StubSignature signatureOfNarrowCharactersOfString(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T11) });
    signature.result = StubResult::TwoGPRs;
    signature.secondResult = gprOf(StubRegister::T12);
    return signature;
}

void generateStringEqualsLiteral(StubAssembler& jit)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1));
    static_assert(sizeof(ImageStringRecord) == sizeof(uint64_t) && !OBJECT_OFFSETOF(ImageStringRecord, rangeOrIndex) && sizeof(ImageStringWord) == sizeof(uint32_t));
    static_assert(sizeof(ImageStringHeader) == 3 * sizeof(uint64_t) && sizeof(ImageStringWord) == sizeof(uint32_t));
    const unsigned lengthOfLiteral = jit.key().count();
    const GPRReg subject = jit.operand(0);
    const GPRReg stringIndex = jit.immediate();
    const GPRReg result = jit.result();
    const GPRReg fiber = jit.allocateScratch(StubRegister::T12);
    const GPRReg implArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg stringIndexArgument = jit.allocateScratch(StubRegister::A1);
    const GPRReg header = stringIndexArgument;
    const GPRReg length = jit.allocateScratch(StubRegister::A2);
    const GPRReg headers = length;
    RELEASE_ASSERT(result == GPRInfo::returnValueGPR);
    RELEASE_ASSERT(noOverlap(subject, stringIndex, fiber, stringIndexArgument, length) && noOverlap(subject, stringIndex, implArgument, stringIndexArgument, length));
    JumpList isNotEqual;
    JumpList hasNoHeader;

    jit.bindEntry(equalsLiteral(ComparedOperand::Value, lengthOfLiteral));
    isNotEqual.append(jit.branchIfNotCell(subject));
    isNotEqual.append(jit.branchIfNotType(subject, StringType));
    jit.bindEntry(jit.key());
    jit.loadPtr(Address(subject, JSString::offsetOfValue()), fiber);
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
    if (lengthOfLiteral)
        isNotEqual.append(jit.branch32(CCallHelpers::NotEqual, Address(fiber, StringImpl::lengthMemoryOffset()), TrustedImm32(lengthOfLiteral)));
    Jump isSameImpl;
    const GPRReg number = header;
    if (lengthOfLiteral && jit.options().isPositionIndependent) {
        isRope.link(&jit);
        jit.jumpToStub(equalsLiteral(ComparedOperand::String));
        isNotEqual.link(&jit);
        returnResult(jit, TrustedImm32(0));
        return;
    }
    auto loadNumberOfHeader = [&](JumpList& hasNone) {
        jit.zeroExtend32ToWord(stringIndex, number);
        Jump isIdentifier = jit.branch32(CCallHelpers::Above, Address(instanceGPR, AOT_INSTANCE_OFFSET(image.endOfIdentifiers)), number);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringWords)), number);
        jit.load32(BaseIndex(number, stringIndex, CCallHelpers::TimesFour), number);
        hasNone.append(jit.branchTest32(CCallHelpers::NonZero, number, TrustedImm32(ImageString::hasNoHeaderFlag)));
        jit.and32(TrustedImm32(ImageString::hashMask), number);
        isIdentifier.link(&jit);
    };
    loadNumberOfHeader(hasNoHeader);
    if (lengthOfLiteral) {
        jit.getEffectiveAddress(BaseIndex(number, number, CCallHelpers::TimesTwo), header);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), headers);
        jit.getEffectiveAddress(BaseIndex(headers, header, CCallHelpers::TimesEight), header);
        isSameImpl = jit.branchPtr(CCallHelpers::Equal, fiber, header);
        jit.load32(Address(fiber, StringImpl::flagsOffset()), length);
        jit.and32(Address(header, StringImpl::flagsOffset()), length);
        isNotEqual.append(jit.branchTest32(CCallHelpers::NonZero, length, TrustedImm32(StringImpl::flagIsAtom())));
        if (lengthOfLiteral >= sizeof(uint64_t)) {
            hasNoHeader.append(jit.branchTest32(CCallHelpers::Zero, length, TrustedImm32(StringImpl::flagIs8Bit())));
            jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
            jit.loadPtr(Address(header, StringImpl::dataOffset()), header);
            for (unsigned offset = 0; offset < lengthOfLiteral; offset += sizeof(uint64_t)) {
                int32_t start = std::min<unsigned>(offset, lengthOfLiteral - sizeof(uint64_t));
                jit.load64(Address(fiber, start), length);
                isNotEqual.append(jit.branch64(CCallHelpers::NotEqual, length, Address(header, start)));
            }
            isSameImpl.link(&jit);
            returnResult(jit, TrustedImm32(1));
        }
        hasNoHeader.link(&jit);
        Label mayBeEqual = jit.label();
        jit.jumpToStub(equalsLiteral(ComparedOperand::String));
        if (lengthOfLiteral < sizeof(uint64_t)) {
            isSameImpl.link(&jit);
            returnResult(jit, TrustedImm32(1));
        }
        isRope.link(&jit);
        jit.branch32(CCallHelpers::Equal, Address(subject, JSRopeString::offsetOfLength()), TrustedImm32(lengthOfLiteral)).linkTo(mayBeEqual, &jit);
        isNotEqual.link(&jit);
        returnResult(jit, TrustedImm32(0));
        return;
    }
    if (jit.options().isPositionIndependent) {
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringRecords)), length);
        jit.load64(BaseIndex(length, number, CCallHelpers::TimesEight), length);
        hasNoHeader.append(jit.branchTest64(CCallHelpers::Zero, length));
        Jump hasInlineRange = jit.branchTest32(CCallHelpers::NonZero, length, TrustedImm32(ImageString::lengthMask));
        static_assert(ImageString::lengthBits > CCallHelpers::TimesFour);
        jit.urshift32(TrustedImm32(ImageString::lengthBits), length);
        jit.lshift32(TrustedImm32(CCallHelpers::TimesFour), length);
        jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.listedStrings)), length);
        jit.load32(Address(length), length);
        hasInlineRange.link(&jit);
        jit.and32(TrustedImm32(ImageString::lengthMask), length);
        hasNoHeader.append(jit.branchTest32(CCallHelpers::Zero, length));
        isNotEqual.append(jit.branch32(CCallHelpers::NotEqual, Address(fiber, StringImpl::lengthMemoryOffset()), length));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaderOffsets)), headers);
        jit.load32(BaseIndex(headers, number, CCallHelpers::TimesFour), header);
        jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(lazyStrings.headers)), header);
        isSameImpl = jit.branchPtr(CCallHelpers::Equal, fiber, header);
        hasNoHeader.append(jit.jump());
    } else {
        jit.getEffectiveAddress(BaseIndex(number, number, CCallHelpers::TimesTwo), header);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), headers);
        jit.getEffectiveAddress(BaseIndex(headers, header, CCallHelpers::TimesEight), header);
        isSameImpl = jit.branchPtr(CCallHelpers::Equal, fiber, header);
        jit.load32(Address(header, OBJECT_OFFSETOF(ImageStringHeader, length)), length);
        hasNoHeader.append(jit.branchTest32(CCallHelpers::Zero, length));
        isNotEqual.append(jit.branch32(CCallHelpers::NotEqual, Address(fiber, StringImpl::lengthMemoryOffset()), length));
        jit.move(fiber, implArgument);
        jit.callLeaf(Entry::operationAOTStringImplsAreEqual);
        jit.returnToCaller();
    }

    isSameImpl.link(&jit);
    returnResult(jit, TrustedImm32(1));

    isNotEqual.link(&jit);
    Label answersNo = jit.label();
    returnResult(jit, TrustedImm32(0));

    hasNoHeader.link(&jit);
    jit.move(fiber, implArgument);
    jit.move(stringIndex, stringIndexArgument);
    jit.callLeaf(Entry::operationAOTStringImplEqualsImageString);
    jit.returnToCaller();

    isRope.link(&jit);
    if (!jit.options().isPositionIndependent) {
        JumpList hasNoLength;
        loadNumberOfHeader(hasNoLength);
        jit.getEffectiveAddress(BaseIndex(number, number, CCallHelpers::TimesTwo), header);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), headers);
        jit.getEffectiveAddress(BaseIndex(headers, header, CCallHelpers::TimesEight), header);
        jit.load32(Address(header, OBJECT_OFFSETOF(ImageStringHeader, length)), length);
        hasNoLength.append(jit.branchTest32(CCallHelpers::Zero, length));
        jit.branch32(CCallHelpers::NotEqual, Address(subject, JSRopeString::offsetOfLength()), length).linkTo(answersNo, &jit);
        hasNoLength.link(&jit);
    }
    static_assert(leafArgumentGPR(2) == gprOf(StubRegister::A2));
    JumpList needsOperation;
    load8BitCharactersOfSubstring(jit, subject, fiber, length, needsOperation);
    jit.move(fiber, implArgument);
    jit.load32(Address(subject, JSRopeString::offsetOfLength()), stringIndexArgument);
    jit.move(stringIndex, length);
    jit.callLeaf(Entry::operationAOTCharactersEqualImageString);
    jit.returnToCaller();

    needsOperation.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringEqualsLiteral, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = subject, .thirdArgumentIn = stringIndex });
}

static void loadPackedCharacters(StubAssembler& jit, unsigned length, GPRReg characters, GPRReg packed)
{
    switch (length) {
    case 1:
        jit.load8(Address(characters), packed);
        break;
    case 2:
        jit.load16(Address(characters), packed);
        break;
    case 3:
        jit.load16(Address(characters), packed);
        jit.load8(Address(characters, 2), characters);
        jit.lshift32(TrustedImm32(16), characters);
        jit.or32(characters, packed);
        break;
    case 4:
        jit.load32(Address(characters), packed);
        break;
    case 5:
    case 6:
    case 7:
        jit.load32(Address(characters), packed);
        if (length == 5)
            jit.load8(Address(characters, 4), characters);
        else if (length == 6)
            jit.load16(Address(characters, 4), characters);
        else
            jit.load32(Address(characters, 3), characters);
        jit.lshift64(TrustedImm32(32), characters);
        jit.or64(characters, packed);
        break;
    default:
        RELEASE_ASSERT(length == 8);
        jit.load64(Address(characters), packed);
        break;
    }
}

void generateStringEqualsLiteralOfLength(StubAssembler& jit)
{
    const unsigned length = jit.key().count();
    const GPRReg subject = jit.operand(0);
    const GPRReg text = jit.operand(1);
    const GPRReg fiber = jit.allocateScratch(StubRegister::T12);
    const GPRReg characters = fiber;
    const GPRReg isEqual = fiber;
    const GPRReg packed = jit.allocateScratch(StubRegister::T13);
    const GPRReg offsetInBase = packed;
    JumpList isNotEqual;
    JumpList slowCases;

    jit.bindEntry(equalsLiteralOfLength(ComparedOperand::Value, length));
    isNotEqual.append(jit.branchIfNotCell(subject));
    isNotEqual.append(jit.branchIfNotType(subject, StringType));
    jit.bindEntry(jit.key());
    jit.loadPtr(Address(subject, JSString::offsetOfValue()), fiber);
    Jump isRope = jit.branchTestPtr(CCallHelpers::NonZero, fiber, TrustedImm32(JSString::isRopeInPointer));
    if (!length) {
        jit.load32(Address(fiber, StringImpl::lengthMemoryOffset()), packed);
        jit.returnFlags32(CCallHelpers::Equal, packed, text);

        isRope.link(&jit);
        jit.load32(Address(subject, JSRopeString::offsetOfLength()), packed);
        jit.returnFlags32(CCallHelpers::Equal, packed, text);

        isNotEqual.link(&jit);
        jit.returnFlagsConstant(false);
        return;
    }
    isNotEqual.append(jit.branch32(CCallHelpers::NotEqual, Address(fiber, StringImpl::lengthMemoryOffset()), TrustedImm32(length)));
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), characters);
    Label hasCharacters = jit.label();
    loadPackedCharacters(jit, length, characters, packed);
    if (length > 4)
        jit.returnFlags64(CCallHelpers::Equal, packed, text);
    else
        jit.returnFlags32(CCallHelpers::Equal, packed, text);

    isRope.link(&jit);
    isNotEqual.append(jit.branch32(CCallHelpers::NotEqual, Address(subject, JSRopeString::offsetOfLength()), TrustedImm32(length)));
    Jump hasPieces = jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSRopeString::isSubstringInPointer));
    JumpList baseIs16Bit;
    load8BitCharactersOfSubstring(jit, subject, characters, offsetInBase, baseIs16Bit);
    jit.jump().linkTo(hasCharacters, &jit);

    baseIs16Bit.link(&jit);
    jit.lshift64(TrustedImm32(1), offsetInBase);
    jit.addPtr(Address(fiber, StringImpl::dataOffset()), offsetInBase);
    jit.move(offsetInBase, characters);
    Jump has16BitCharacters = jit.jump();
    is16Bit.link(&jit);
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), characters);
    has16BitCharacters.link(&jit);
    jit.load16(Address(characters), packed);
    jit.zeroExtend8To32(text, characters);
    if (length == 1)
        jit.returnFlags32(CCallHelpers::Equal, packed, characters);
    else {
        isNotEqual.append(jit.branch32(CCallHelpers::NotEqual, packed, characters));
        slowCases.append(jit.jump());
    }

    hasPieces.link(&jit);
    const GPRReg count = gprOf(StubRegister::T10);
    const GPRReg unit = gprOf(StubRegister::T14);
    RELEASE_ASSERT(noOverlap(subject, text, fiber, packed, count, unit));
    jit.preserving({ count, unit }, [&] {
        JumpList hasOtherPiece;
        auto packPiece = [&] {
            jit.loadPtr(Address(fiber, JSString::offsetOfValue()), fiber);
            hasOtherPiece.append(jit.branchIfRopeStringImpl(fiber));
            hasOtherPiece.append(jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
            jit.load32(Address(fiber, StringImpl::lengthMemoryOffset()), count);
            jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
            Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, count);
            Label packsUnit = jit.label();
            jit.sub32(TrustedImm32(1), count);
            jit.load8(BaseIndex(fiber, count, CCallHelpers::TimesOne), unit);
            jit.lshift64(TrustedImm32(8), packed);
            jit.or64(unit, packed);
            jit.branchTest32(CCallHelpers::NonZero, count).linkTo(packsUnit, &jit);
            isEmpty.link(&jit);
        };
        jit.move(TrustedImm32(0), packed);
        jit.load64(Address(subject, JSRopeString::offsetOfFiber2()), fiber);
        jit.urshift64(TrustedImm32(JSRopeString::CompactFibers::shiftOfFiber2InUpperWord), fiber);
        Jump hasTwoPieces = jit.branchTestPtr(CCallHelpers::Zero, fiber);
        packPiece();
        hasTwoPieces.link(&jit);
        loadSubstringBaseAndOffset(jit, subject, fiber, unit);
        packPiece();
        jit.loadPtr(Address(subject, JSString::offsetOfValue()), fiber);
        jit.and64(TrustedImm32(static_cast<int32_t>(JSRopeString::stringMask)), fiber);
        packPiece();
        jit.move(TrustedImm32(0), fiber);
        Jump isPacked = jit.jump();
        hasOtherPiece.link(&jit);
        jit.move(TrustedImm32(1), fiber);
        isPacked.link(&jit);
    });
    slowCases.append(jit.branchTest32(CCallHelpers::NonZero, fiber));
    if (length == 7) {
        jit.move(packed, fiber);
        jit.urshift64(TrustedImm32(24), fiber);
        jit.lshift64(TrustedImm32(32), fiber);
        jit.zeroExtend32ToWord(packed, packed);
        jit.or64(fiber, packed);
    }
    if (length > 4)
        jit.returnFlags64(CCallHelpers::Equal, packed, text);
    else
        jit.returnFlags32(CCallHelpers::Equal, packed, text);

    isNotEqual.link(&jit);
    jit.returnFlagsConstant(false);

    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringEqualsLiteralInRegister, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = subject, .thirdArgumentIn = text, .valueDestination = isEqual }, [&] {
        jit.compareOnFlags32(isEqual, TrustedImm32(1));
    });
}

void generateNarrowCharactersOfString(StubAssembler& jit)
{
    const GPRReg string = jit.operand(0);
    const GPRReg characters = jit.result();
    const GPRReg length = jit.secondResult();
    const GPRReg offset = jit.allocateScratch(StubRegister::T9);
    const GPRReg index = offset;
    RELEASE_ASSERT(noOverlap(string, characters, length, offset));

    JumpList isNoSubstringOfLatin1;
    JumpList isOther;
    jit.loadPtr(Address(string, JSString::offsetOfValue()), characters);
    load8BitCharactersOfSubstring(jit, string, characters, offset, isNoSubstringOfLatin1);
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    jit.returnToCaller();

    isNoSubstringOfLatin1.link(&jit);
    const GPRReg value = length;
    const GPRReg flags = offset;
    jit.loadPtr(Address(string, JSString::offsetOfValue()), value);
    Jump isRope = jit.branchIfRopeStringImpl(value);
    jit.loadPtr(Address(value, StringImpl::dataOffset()), characters);
    jit.load32(Address(value, StringImpl::flagsOffset()), flags);
    jit.load32(Address(value, StringImpl::lengthMemoryOffset()), length);
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(StringImpl::flagIs8Bit()));
    jit.returnToCaller();

    isRope.link(&jit);
    isOther.append(jit.branchTestPtr(CCallHelpers::Zero, value, TrustedImm32(JSRopeString::isSubstringInPointer)));
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    jit.lshift64(TrustedImm32(1), offset);
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    jit.addPtr(offset, characters);

    is16Bit.link(&jit);
    constexpr int32_t highByte = 1;
    isOther.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(std::tuple_size_v<decltype(NarrowCharactersOfString::characters)>)));
    jit.move(TrustedImm32(0), index);
    Label next = jit.label();
    isOther.append(jit.branch32(CCallHelpers::AboveOrEqual, index, length));
    Jump isWide = jit.branchTest8(CCallHelpers::NonZero, BaseIndex(characters, index, CCallHelpers::TimesTwo, highByte));
    jit.add32(TrustedImm32(1), index);
    jit.jump().linkTo(next, &jit);

    isWide.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(NarrowCharactersOfString::lengthOfNoNarrowText)), length);
    jit.returnToCaller();

    isOther.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNarrowCharactersOfString, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = string }, [&] {
        jit.load32(Address(characters, OBJECT_OFFSETOF(NarrowCharactersOfString, length)), length);
        jit.addPtr(TrustedImm32(OBJECT_OFFSETOF(NarrowCharactersOfString, characters)), characters);
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
