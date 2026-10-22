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
#include "AOTStubProgramTables.h"
#include "AOTStubRegisters.h"
#include "AOTStubsStrings.h"
#include "JSString.h"
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

using Saves = OperationCall::Saves;

static StubSignature tableComparisonSignature(StubRegister subject)
{
    StubSignature signature;
    signature.addOperand(gprOf(subject));
    signature.immediate = gprOf(stubImmediateRegister);
    signature.result = StubResult::GPR;
    signature.firstResult = gprOf(StubRegister::R0);
    return signature;
}

static StubSignature registerComparisonSignature()
{
    StubSignature signature;
    signature.addOperand(gprOf(comparedSubjectRegister));
    signature.addOperand(gprOf(comparedTextRegister));
    signature.result = StubResult::Flags;
    signature.trueCondition = MacroAssembler::Zero;
    return signature;
}

static StubSignature wordConversionSignature()
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::T11));
    signature.result = StubResult::GPR;
    signature.firstResult = gprOf(StubRegister::R0);
    return signature;
}

StubSignature signatureOfStringEqualsLiteral(StubKey)
{
    return tableComparisonSignature(comparedSubjectRegister);
}

StubSignature signatureOfValueEqualsLiteral(StubKey)
{
    return tableComparisonSignature(comparedSubjectRegister);
}

StubSignature signatureOfStringEqualsLiteralOfLength(StubKey)
{
    return registerComparisonSignature();
}

StubSignature signatureOfValueEqualsLiteralOfLength(StubKey)
{
    return registerComparisonSignature();
}

StubSignature signatureOfLiteralStringToString(StubKey)
{
    return wordConversionSignature();
}

StubSignature signatureOfToLiteralString(StubKey)
{
    return wordConversionSignature();
}

StubSignature signatureOfShortStringOfValue(StubKey)
{
    return wordConversionSignature();
}

StubSignature signatureOfLooseEqualsLiteral(StubKey)
{
    return tableComparisonSignature(StubRegister::R0);
}

void generateStringEqualsLiteral(StubAssembler& jit)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1));
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    static_assert(sizeof(ImageStringRecord) == sizeof(uint64_t) && !OBJECT_OFFSETOF(ImageStringRecord, rangeOrIndex) && sizeof(ImageStringWord) == sizeof(uint32_t));
#else
    static_assert(sizeof(ImageStringHeader) == 3 * sizeof(uint64_t) && sizeof(ImageStringWord) == sizeof(uint32_t));
#endif
    const GPRReg subject = jit.operand(0);
    const GPRReg stringIndex = jit.immediate();
    const GPRReg result = jit.result();
    const GPRReg fiber = jit.allocateScratch(StubRegister::T12);
    // On x86-64 this is the register of fiber, on ARM64 the one of the result.
    const GPRReg implArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg stringIndexArgument = jit.allocateScratch(StubRegister::A1);
    const GPRReg header = stringIndexArgument;
    // The leaf overwrites it anyway.
    const GPRReg length = jit.allocateScratch(StubRegister::A2);
    const GPRReg headers = length;
    RELEASE_ASSERT(result == GPRInfo::returnValueGPR);
    RELEASE_ASSERT(noOverlap(subject, stringIndex, fiber, stringIndexArgument, length) && noOverlap(subject, stringIndex, implArgument, stringIndexArgument, length));
    JumpList isNotEqual;
    JumpList hasNoHeader;

    jit.bindEntry(equalsLiteral(ComparedOperand::Value));
    isNotEqual.append(jit.branchIfNotCell(subject));
    isNotEqual.append(jit.branchIfNotType(subject, StringType));
    jit.bindEntry(jit.key());
    jit.loadPtr(Address(subject, JSString::offsetOfValue()), fiber);
    Jump isRope = jit.branchIfRopeStringImpl(fiber);
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    const GPRReg number = header;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringWords)), number);
    jit.load32(BaseIndex(number, stringIndex, CCallHelpers::TimesFour), number);
    hasNoHeader.append(jit.branchTest32(CCallHelpers::NonZero, number, TrustedImm32(ImageString::hasNoHeaderFlag)));
    jit.and32(TrustedImm32(ImageString::hashMask), number);
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
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaders)), header);
    Jump isSameImpl = jit.branchPtr(CCallHelpers::Equal, fiber, header);
    hasNoHeader.append(jit.jump());
#else
    // The header of the text is a StringImpl: a string made of the text has that very impl, and most others have another length.
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringWords)), header);
    jit.load32(BaseIndex(header, stringIndex, CCallHelpers::TimesFour), header);
    hasNoHeader.append(jit.branchTest32(CCallHelpers::NonZero, header, TrustedImm32(ImageString::hasNoHeaderFlag)));
    jit.and32(TrustedImm32(ImageString::hashMask), header);
    jit.getEffectiveAddress(BaseIndex(header, header, CCallHelpers::TimesTwo), header);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), headers);
    jit.getEffectiveAddress(BaseIndex(headers, header, CCallHelpers::TimesEight), header);
    Jump isSameImpl = jit.branchPtr(CCallHelpers::Equal, fiber, header);
    jit.load32(Address(header, OBJECT_OFFSETOF(ImageStringHeader, length)), length);
    // A string without text and the empty text have a header of zeros.
    hasNoHeader.append(jit.branchTest32(CCallHelpers::Zero, length));
    isNotEqual.append(jit.branch32(CCallHelpers::NotEqual, Address(fiber, StringImpl::lengthMemoryOffset()), length));
    jit.move(fiber, implArgument);
    jit.callLeaf(Entry::operationAOTStringImplsAreEqual);
    jit.returnToCaller();
#endif

    isSameImpl.link(&jit);
    jit.move(TrustedImm32(1), result);
    jit.returnToCaller();

    isNotEqual.link(&jit);
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();

    hasNoHeader.link(&jit);
    jit.move(fiber, implArgument);
    jit.move(stringIndex, stringIndexArgument);
    jit.callLeaf(Entry::operationAOTStringImplEqualsImageString);
    jit.returnToCaller();

    isRope.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringEqualsLiteral, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = subject, .thirdArgumentIn = stringIndex });
}

// Packs as packStringIntoRegister does. Overwrites characters.
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
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSRopeString::isSubstringInPointer)));
    loadSubstringBaseAndOffset(jit, subject, fiber, offsetInBase);
    jit.loadPtr(Address(fiber, JSString::offsetOfValue()), fiber);
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), characters);
    jit.addPtr(offsetInBase, characters);
    jit.jump().linkTo(hasCharacters, &jit);

    isNotEqual.link(&jit);
    jit.returnFlagsConstant(false);

    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringEqualsLiteralInRegister, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = subject, .thirdArgumentIn = text, .valueDestination = isEqual }, [&] {
        jit.compareOnFlags32(isEqual, TrustedImm32(1));
    });
}

static void callLiteralStringOperationAndReturn(StubAssembler& jit, Entry operation, GPRReg value)
{
    jit.callOperationAndReturn(operation, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = value });
}

void generateLiteralStringToString(StubAssembler& jit)
{
    const GPRReg word = jit.operand(0);
    const GPRReg result = jit.result();
    const GPRReg scaledNumber = jit.allocateScratch(StubRegister::T12);
    Jump isNotCreated;

    jit.move(word, result);
    jit.add64(TrustedImm32(-static_cast<int32_t>(emptyLiteralStringWord)), word, scaledNumber);
    Jump isNotLiteralString = jit.branch64(CCallHelpers::AboveOrEqual, scaledNumber, TrustedImm32(minCell32 - emptyLiteralStringWord));
    if (jit.options().useWideReferences) {
        static_assert(2 << literalStringShift == sizeof(JSCell*));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.wideLiteralStrings)), result);
        jit.load64(BaseIndex(result, scaledNumber, CCallHelpers::TimesTwo), result);
        isNotCreated = jit.branchTest64(CCallHelpers::Zero, result);
    } else {
        static_assert(1 << literalStringShift == sizeof(uint32_t));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.literalStrings)), result);
        jit.load32(BaseIndex(result, scaledNumber, CCallHelpers::TimesOne), result);
        isNotCreated = jit.branchTest32(CCallHelpers::Zero, result);
        decodeCell32(jit, result, result);
    }
    isNotLiteralString.link(&jit);
    jit.returnToCaller();

    isNotCreated.link(&jit);
    callLiteralStringOperationAndReturn(jit, Entry::operationAOTLiteralStringToString, word);
}

void findLiteralStringWord(StubAssembler& jit, const LiteralStringProbeRegisters& registers, JumpList& isEmpty, JumpList& isNotLiteral, JumpList& isNotCached)
{
    const GPRReg string = registers.string;
    const GPRReg impl = registers.impl;
    const GPRReg entry = registers.entry;
    const GPRReg length = entry;
    const GPRReg tables = registers.tables;
    const GPRReg cache = tables;
    const GPRReg cachedImpl = tables;
    const GPRReg maxLength = registers.maxLength;
    RELEASE_ASSERT(noOverlap(string, impl, entry, tables, maxLength));

    // A string of the image has the answer in its header. (The fiber of a rope is an address outside of the image, or no address.)
    const GPRReg headers = tables;
    const GPRReg sizeOfHeaders = maxLength;
    jit.loadPtr(Address(string, JSString::offsetOfValue()), impl);
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaders)), headers);
#else
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), headers);
#endif
    jit.load32(Address(headers, OBJECT_OFFSETOF(ImageStringHeader, literalStringWord)), sizeOfHeaders);
    jit.sub64(impl, headers, entry);
    Jump isOutsideOfImage = jit.branch64(CCallHelpers::AboveOrEqual, entry, sizeOfHeaders);
    jit.load32(Address(impl, OBJECT_OFFSETOF(ImageStringHeader, literalStringWord)), entry);
    isNotLiteral.append(jit.branchTest32(CCallHelpers::Zero, entry));
    Jump isAnsweredByHeader = jit.jump();

    isOutsideOfImage.link(&jit);
    // A header that was made for a constant: the literal strings have theirs in the image.
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.startOfStringHeaders)), tables);
    jit.sub64(impl, tables, entry);
    isNotLiteral.append(jit.branch64(CCallHelpers::Below, entry, Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.sizeOfStringHeaders))));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), tables);
    isNotCached.append(jit.branchTest32(CCallHelpers::Zero, programTableCount(tables, ProgramTableIndex::literalStringLimits)));
    loadProgramTable(jit, tables, ProgramTableIndex::literalStringLimits, maxLength);
    jit.load32(Address(maxLength, OBJECT_OFFSETOF(LiteralStringLimits, maxLength)), maxLength);
    Jump isFlat = jit.branchTestPtr(CCallHelpers::Zero, impl, TrustedImm32(JSString::isRopeInPointer));
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    isNotCached.append(jit.branch32(CCallHelpers::BelowOrEqual, length, maxLength));
    isNotLiteral.append(jit.jump());

    isFlat.link(&jit);
    jit.load32(Address(impl, StringImpl::lengthMemoryOffset()), length);
    isEmpty.append(jit.branchTest32(CCallHelpers::Zero, length));
    isNotLiteral.append(jit.branch32(CCallHelpers::Above, length, maxLength));
    jit.urshift64(impl, TrustedImm32(literalStringCacheIndexShift), entry);
    jit.and32(TrustedImm32((1 << literalStringCacheIndexBits) - 1), entry);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.literalStringCache)), cache);
    jit.load64(BaseIndex(cache, entry, CCallHelpers::TimesEight), entry);
    jit.urshift64(TrustedImm32(literalStringCacheImplShift), impl);
    jit.urshift64(entry, TrustedImm32(literalStringCacheWordBits), cachedImpl);
    isNotCached.append(jit.branch64(CCallHelpers::NotEqual, cachedImpl, impl));
    jit.and32(TrustedImm32((1 << literalStringCacheWordBits) - 1), entry);
    isNotLiteral.append(jit.branchTest32(CCallHelpers::Zero, entry));
    jit.lshift32(TrustedImm32(literalStringCacheWordShift), entry);
    isAnsweredByHeader.link(&jit);
}

void generateToLiteralString(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg result = jit.result();
    const GPRReg impl = jit.allocateScratch(StubRegister::T12);
    const GPRReg word = jit.allocateScratch(StubRegister::T13);
    const GPRReg tables = jit.allocateScratch(StubRegister::T14);
    JumpList isEmpty;
    JumpList returnsOperand;
    JumpList isNotCached;

    returnsOperand.append(jit.branchIfNotCell(value));
    returnsOperand.append(jit.branchTest64(CCallHelpers::Zero, value));
    findLiteralStringWord(jit, { .string = value, .impl = impl, .entry = word, .tables = tables, .maxLength = result }, isEmpty, returnsOperand, isNotCached);
    jit.move(word, result);
    jit.returnToCaller();

    isEmpty.link(&jit);
    jit.move(TrustedImm32(emptyLiteralStringWord), result);
    jit.returnToCaller();

    returnsOperand.link(&jit);
    jit.move(value, result);
    jit.returnToCaller();

    isNotCached.link(&jit);
    callLiteralStringOperationAndReturn(jit, Entry::operationAOTToLiteralString, value);
}

void generateShortStringOfValue(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg result = jit.result();
    const GPRReg characters = jit.allocateScratch(StubRegister::T12);
    const GPRReg length = jit.allocateScratch(StubRegister::T13);
    const GPRReg shift = jit.allocateScratch(StubRegister::T14);
    constexpr int32_t smallestPageSize = 4096;
    static_assert(maxShortStringLength == 7 && noShortString == UINT64_MAX);
    JumpList notShortString;
    JumpList slowCases;

    notShortString.append(jit.branchIfNotCell(value));
    notShortString.append(jit.branchIfNotType(value, StringType));
    jit.loadPtr(Address(value, JSString::offsetOfValue()), characters);
    Jump isRope = jit.branchTestPtr(CCallHelpers::NonZero, characters, TrustedImm32(JSString::isRopeInPointer));
    jit.load32(Address(characters, StringImpl::lengthMemoryOffset()), length);
    notShortString.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxShortStringLength)));
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, length);
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    Label hasCharacters = jit.label();
    jit.lshift32(length, TrustedImm32(3), shift);
    jit.neg32(shift);
    jit.add32(TrustedImm32(8 * maxShortStringLength), shift);
    // An eight-byte load at the first character could reach into the next page, which may not be mapped.
    jit.and32(TrustedImm32(smallestPageSize - 1), characters, result);
    Jump isNearEndOfPage = jit.branch32(CCallHelpers::Above, result, TrustedImm32(smallestPageSize - static_cast<int32_t>(sizeof(uint64_t))));
    jit.load64(Address(characters), result);
    jit.lshift64(shift, result);
    jit.lshift64(TrustedImm32(8), result);
    jit.add32(TrustedImm32(8 - shortStringLengthBits), shift);
    jit.urshift64(shift, result);
    jit.or64(length, result);
    jit.returnToCaller();

    isNearEndOfPage.link(&jit);
    jit.load64(BaseIndex(characters, length, CCallHelpers::TimesOne, -static_cast<int32_t>(sizeof(uint64_t))), result);
    jit.urshift64(shift, result);
    jit.urshift64(TrustedImm32(8), result);
    jit.lshift64(TrustedImm32(shortStringLengthBits), result);
    jit.or64(length, result);
    jit.returnToCaller();

    isEmpty.link(&jit);
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();

    isRope.link(&jit);
    jit.load32(Address(value, JSRopeString::offsetOfLength()), length);
    notShortString.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxShortStringLength)));
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, characters, TrustedImm32(JSRopeString::isSubstringInPointer)));
    loadSubstringBaseAndOffset(jit, value, characters, shift);
    jit.loadPtr(Address(characters, JSString::offsetOfValue()), characters);
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    jit.addPtr(shift, characters);
    jit.jump().linkTo(hasCharacters, &jit);

    notShortString.link(&jit);
    jit.move(TrustedImm64(-1), result);
    jit.returnToCaller();

    slowCases.link(&jit);
    callLiteralStringOperationAndReturn(jit, Entry::operationAOTShortStringOfString, value);
}

void generateLooseEqualsLiteral(StubAssembler& jit)
{
    constexpr StubKey target = equalsLiteral(ComparedOperand::String);
    const GPRReg value = jit.operand(0);
    const GPRReg stringIndex = jit.immediate();
    const GPRReg result = jit.result();
    const GPRReg type = jit.allocateScratch(StubRegister::T10);
    const GPRReg subject = jit.allocateScratch(comparedSubjectRegister);
    ASSERT(signatureOf(target).operands[0] == Reg(subject));
    ASSERT(signatureOf(target).immediate == stringIndex);
    JumpList isNotEqual;

    isNotEqual.append(jit.branchIfNotCell(value));
    jit.load8(Address(value, JSCell::typeInfoTypeOffset()), type);
    Jump isObject = jit.branch32(CCallHelpers::AboveOrEqual, type, TrustedImm32(ObjectType));
    isNotEqual.append(jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(StringType)));
    jit.move(value, subject);
    jit.jumpToStub(target);

    isNotEqual.link(&jit);
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();

    isObject.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTCompareEqToString, { .secondArgumentIn = value, .thirdArgumentIn = stringIndex });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
