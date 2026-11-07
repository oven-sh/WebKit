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
StubSignature signatureOfNarrowCharactersOfString(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::T11) }); }

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
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringWords)), number);
    jit.load32(BaseIndex(number, stringIndex, CCallHelpers::TimesFour), number);
    hasNoHeader.append(jit.branchTest32(CCallHelpers::NonZero, number, TrustedImm32(ImageString::hasNoHeaderFlag)));
    jit.and32(TrustedImm32(ImageString::hashMask), number);
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
        isRope.link(&jit);
        hasNoHeader.link(&jit);
        jit.jumpToStub(equalsLiteral(ComparedOperand::String));
        if (lengthOfLiteral < sizeof(uint64_t)) {
            isSameImpl.link(&jit);
            returnResult(jit, TrustedImm32(1));
        }
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
    returnResult(jit, TrustedImm32(0));

    hasNoHeader.link(&jit);
    jit.move(fiber, implArgument);
    jit.move(stringIndex, stringIndexArgument);
    jit.callLeaf(Entry::operationAOTStringImplEqualsImageString);
    jit.returnToCaller();

    isRope.link(&jit);
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
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
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
    load8BitCharactersOfSubstring(jit, subject, characters, offsetInBase, slowCases);
    jit.jump().linkTo(hasCharacters, &jit);

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
    jit.callOperationAndReturn(Entry::operationAOTNarrowCharactersOfString, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = jit.operand(0) });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
