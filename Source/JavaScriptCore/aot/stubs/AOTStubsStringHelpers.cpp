/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "AOTRuntimeWords.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "CellState.h"
#include "JSString.h"
#include <algorithm>
#include <bit>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

void loadSubstringBaseAndOffset(StubAssembler& jit, GPRReg string, GPRReg base, GPRReg offset)
{
    using CompactFibers = JSRopeString::CompactFibers;
    RELEASE_ASSERT(noOverlap(string, base, offset));
    static_assert(CompactFibers::widthOfFiber1LowerInLengthWord == 32 && CompactFibers::shiftOfFiber2InUpperWord == 16 && JSRopeString::offsetOfFiber2() == JSRopeString::offsetOfFiber1Lower() + 4);
    static_assert(std::endian::native == std::endian::little);
#if CPU(ARM64)
    static_assert(JSRopeString::offsetOfFiber1Lower() == JSRopeString::offsetOfLength() + 4 && !(JSRopeString::offsetOfLength() % sizeof(uint64_t)));
    jit.loadPair64(Address(string, JSRopeString::offsetOfLength()), base, offset);
    jit.urshift64(TrustedImm32(CompactFibers::widthOfFiber1LowerInLengthWord), base);
    jit.insertBitField64(offset, TrustedImm32(CompactFibers::widthOfFiber1LowerInLengthWord), TrustedImm32(CompactFibers::shiftOfFiber2InUpperWord), base);
    jit.urshift64(TrustedImm32(CompactFibers::shiftOfFiber2InUpperWord), offset);
#else
    jit.load32(Address(string, JSRopeString::offsetOfFiber1Lower()), base);
    jit.load16(Address(string, JSRopeString::offsetOfFiber2()), offset);
    jit.lshift64(TrustedImm32(CompactFibers::widthOfFiber1LowerInLengthWord), offset);
    jit.or64(offset, base);
    jit.load64(Address(string, JSRopeString::offsetOfFiber2()), offset);
    jit.urshift64(TrustedImm32(CompactFibers::shiftOfFiber2InUpperWord), offset);
#endif
}

void packSubstringFibers(StubAssembler& jit, GPRReg base, GPRReg offset, GPRReg length, GPRReg lengthWord)
{
    using CompactFibers = JSRopeString::CompactFibers;
    const GPRReg upperWord = base;
    RELEASE_ASSERT(noOverlap(base, offset, length, lengthWord));
#if CPU(ARM64)
    jit.orLeftShift64(length, base, TrustedImm32(CompactFibers::shiftOfFiber1LowerInLengthWord), lengthWord);
    jit.urshift64(TrustedImm32(CompactFibers::widthOfFiber1LowerInLengthWord), base);
    jit.orLeftShift64(base, offset, TrustedImm32(CompactFibers::shiftOfFiber2InUpperWord), upperWord);
#else
    jit.move(base, lengthWord);
    jit.lshift64(TrustedImm32(CompactFibers::shiftOfFiber1LowerInLengthWord), lengthWord);
    jit.or64(length, lengthWord);
    jit.urshift64(TrustedImm32(CompactFibers::widthOfFiber1LowerInLengthWord), base);
    jit.lshift64(TrustedImm32(CompactFibers::shiftOfFiber2InUpperWord), offset);
    jit.or64(offset, upperWord);
#endif
}

void loadSubstringCharacters(StubAssembler& jit, GPRReg string, GPRReg impl, GPRReg characters, GPRReg length, JumpList& isNotSubstring)
{
    RELEASE_ASSERT(noOverlap(string, impl, characters, length));
    isNotSubstring.append(jit.branchTestPtr(CCallHelpers::Zero, impl, TrustedImm32(JSRopeString::isSubstringInPointer)));
    jit.load32(Address(string, JSRopeString::offsetOfLength()), length);
    loadSubstringBaseAndOffset(jit, string, impl, characters);
    jit.loadPtr(Address(impl, JSString::offsetOfValue()), impl);
    Jump is8Bit = jit.branchTest32(CCallHelpers::NonZero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.lshift64(TrustedImm32(1), characters);
    is8Bit.link(&jit);
    jit.addPtr(Address(impl, StringImpl::dataOffset()), characters);
}

void load8BitCharactersOfSubstring(StubAssembler& jit, GPRReg string, GPRReg fiber, GPRReg offset, JumpList& isOther)
{
    isOther.append(jit.branchTestPtr(CCallHelpers::Zero, fiber, TrustedImm32(JSRopeString::isSubstringInPointer)));
    loadSubstringBaseAndOffset(jit, string, fiber, offset);
    jit.loadPtr(Address(fiber, JSString::offsetOfValue()), fiber);
    isOther.append(jit.branchTest32(CCallHelpers::Zero, Address(fiber, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(fiber, StringImpl::dataOffset()), fiber);
    jit.addPtr(offset, fiber);
}

static void loadZeroExtended(StubAssembler& jit, unsigned width, auto address, GPRReg word)
{
    switch (width) {
    case 8:
        jit.load64(address, word);
        return;
    case 4:
        jit.load32(address, word);
        return;
    case 2:
        jit.load16Unaligned(address, word);
        return;
    case 1:
        jit.load8(address, word);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void forEachWordCovering(StubAssembler& jit, std::initializer_list<GPRReg> starts, GPRReg size, const auto& visit, StartsOfEqualBytes startsOfEqualBytes = StartsOfEqualBytes::EndAnywhere)
{
    auto atStart = [](GPRReg bytes) {
        return Address(bytes);
    };
    JumpList isDone;
    JumpList isDoneWithStartsInPlace;

    constexpr int32_t wordSize = sizeof(uint64_t);
    Jump isShorter = jit.branch32(CCallHelpers::Below, size, TrustedImm32(wordSize));
    const GPRReg offsetFromLastWord = size;
    for (GPRReg start : starts)
        jit.addPtr(size, start);
    jit.neg64(size);
    Jump isAtLastWord = jit.branchAdd64(CCallHelpers::PositiveOrZero, TrustedImm32(wordSize), offsetFromLastWord);
    Label nextWord = jit.label();
    visit(wordSize, [&](GPRReg end) {
        return BaseIndex(end, offsetFromLastWord, CCallHelpers::TimesOne, -wordSize);
    });
    jit.branchAdd64(CCallHelpers::Signed, TrustedImm32(wordSize), offsetFromLastWord).linkTo(nextWord, &jit);
    isAtLastWord.link(&jit);
    visit(wordSize, [](GPRReg end) {
        return Address(end, -wordSize);
    });
    isDone.append(jit.jump());

    for (int32_t width : { 4, 2 }) {
        isShorter.link(&jit);
        isShorter = jit.branch32(CCallHelpers::Below, size, TrustedImm32(width));
        visit(width, atStart);
        visit(width, [&](GPRReg bytes) {
            return BaseIndex(bytes, size, CCallHelpers::TimesOne, -width);
        });
        isDoneWithStartsInPlace.append(jit.jump());
    }

    isShorter.link(&jit);
    isDone.append(jit.branchTest32(CCallHelpers::Zero, size));
    visit(1, atStart);
    isDoneWithStartsInPlace.link(&jit);
    if (startsOfEqualBytes == StartsOfEqualBytes::EndBehindBytes) {
        for (GPRReg start : starts)
            jit.addPtr(size, start);
    }
    isDone.link(&jit);
}

void branchIfBytesDiffer(StubAssembler& jit, const ComparedBytes& compared, JumpList& differ, DifferingWords differingWords, StartsOfEqualBytes startsOfEqualBytes)
{
    auto [first, second, size, firstWord, secondWord] = compared;
    RELEASE_ASSERT(noOverlap(first, second, size, firstWord, secondWord));
    forEachWordCovering(jit, { first, second }, size, [&](unsigned width, auto addressIn) {
        loadZeroExtended(jit, width, addressIn(first), firstWord);
#if CPU(X86_64)
        if (differingWords == DifferingWords::MayBeUnloaded && width == sizeof(uint32_t)) {
            differ.append(jit.branch32(CCallHelpers::NotEqual, addressIn(second), firstWord));
            return;
        }
#endif
        loadZeroExtended(jit, width, addressIn(second), secondWord);
        differ.append(jit.branch64(CCallHelpers::NotEqual, firstWord, secondWord));
    }, startsOfEqualBytes);
    UNUSED_PARAM(differingWords);
}

void branchUnlessASCIIOutsideRange(StubAssembler& jit, const ScannedBytes& scanned, uint8_t first, uint8_t last, JumpList& isNot, CodeUnitWidth codeUnitWidth)
{
    auto [bytes, size, word, atLeastFirst, aboveLast, addendForFirst, addendForLast] = scanned;
    RELEASE_ASSERT(noOverlap(bytes, size, word, atLeastFirst, aboveLast, addendForFirst, addendForLast));
    RELEASE_ASSERT(first <= last && last < 0x7f);
    const uint64_t oneInEachCharacter = codeUnitWidth == CodeUnitWidth::EightBits ? 0x0101010101010101ULL : 0x0001000100010001ULL;
    const uint64_t bitsAboveASCII = (codeUnitWidth == CodeUnitWidth::EightBits ? 0x80 : 0xff80) * oneInEachCharacter;

    jit.move(TrustedImm64((0x80 - first) * oneInEachCharacter), addendForFirst);
    jit.move(TrustedImm64((0x80 - (last + 1)) * oneInEachCharacter), addendForLast);
    forEachWordCovering(jit, { bytes }, size, [&](unsigned width, auto addressIn) {
        loadZeroExtended(jit, width, addressIn(bytes), word);
        jit.add64(word, addendForFirst, atLeastFirst);
        jit.add64(word, addendForLast, aboveLast);
        jit.xor64(aboveLast, atLeastFirst);
        jit.or64(word, atLeastFirst);
        isNot.append(jit.branchTest64(CCallHelpers::NonZero, atLeastFirst, TrustedImm64(bitsAboveASCII)));
    });
}

void searchForAtom(StubAssembler& jit, const AtomSearch& search, JumpList& isFound, JumpList& isNotFound)
{
    static_assert(JSCell::typeInfoFlagsOffset() == JSCell::typeInfoTypeOffset() + 1);
    constexpr int32_t flagInTypeAndFlags = (ConstantCellWords::flagOfStringConstant | TextInCell::flag) << 8;
    constexpr int32_t typeAndFlagsOfAtom = StringType | (TypeInfo(StringType, JSString::StructureFlags).inlineTypeFlags() | TypeInfoPerCellBit) << 8 | flagInTypeAndFlags;
    const GPRReg element = search.element;
    const GPRReg typeAndFlags = search.scratch;
    const GPRReg impl = search.scratch;

    JumpList isNoCell;
    Label nextElement = jit.label();
    jit.load64(Address(search.cursor), element);
    isNoCell.append(jit.branchIfNotCell(element));
    isNoCell.append(jit.branchTest64(CCallHelpers::Zero, element));
    jit.load16(Address(element, JSCell::typeInfoTypeOffset()), typeAndFlags);
    jit.or32(TrustedImm32(flagInTypeAndFlags), typeAndFlags);
    Jump cellDoesNotSay = jit.branch32(CCallHelpers::NotEqual, typeAndFlags, TrustedImm32(typeAndFlagsOfAtom));
    isFound.append(jit.branchPtr(CCallHelpers::Equal, Address(element, JSString::offsetOfValue()), search.atom));
    isNoCell.link(&jit);
    Label passOver = jit.label();
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), search.cursor);
    jit.branchPtr(CCallHelpers::Below, search.cursor, search.end).linkTo(nextElement, &jit);
    isNotFound.append(jit.jump());

    cellDoesNotSay.link(&jit);
    jit.branchIfNotType(element, StringType).linkTo(passOver, &jit);
    jit.loadPtr(Address(element, JSString::offsetOfValue()), impl);
    isFound.append(jit.branchPtr(CCallHelpers::Equal, impl, search.atom));
    Jump isRope = jit.branchIfRopeStringImpl(impl);
    jit.branchTest32(CCallHelpers::NonZero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom())).linkTo(passOver, &jit);
    isRope.link(&jit);
}

Jump branchIfSurvivedCollection(StubAssembler& jit, GPRReg string)
{
    static_assert(!static_cast<unsigned>(CellState::PossiblyBlack));
    return jit.branchTest8(CCallHelpers::Zero, Address(string, JSCell::cellStateOffset()));
}

void loadFirstCharactersOfRope(StubAssembler& jit, GPRReg string, GPRReg piece, GPRReg word, GPRReg characters, JumpList& isUnknown)
{
    RELEASE_ASSERT(noOverlap(string, piece, word, characters));
    constexpr int32_t maxDepth = 8;
    const GPRReg depth = characters;
    jit.move(TrustedImm32(maxDepth), depth);
    jit.move(string, piece);
    Label next = jit.label();
    jit.loadPtr(Address(piece, JSString::offsetOfValue()), word);
    Jump isRope = jit.branchIfRopeStringImpl(word);
    isUnknown.append(jit.branchTest32(CCallHelpers::Zero, Address(word, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    isUnknown.append(jit.branchTest32(CCallHelpers::Zero, Address(word, StringImpl::lengthMemoryOffset())));
    jit.loadPtr(Address(word, StringImpl::dataOffset()), characters);
    Jump isLoaded = jit.jump();

    isRope.link(&jit);
    Jump isSubstring = jit.branchTestPtr(CCallHelpers::NonZero, word, TrustedImm32(JSRopeString::isSubstringInPointer));
    jit.and64(TrustedImm32(static_cast<int32_t>(JSRopeString::stringMask)), word, piece);
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(1), depth).linkTo(next, &jit);
    isUnknown.append(jit.jump());

    isSubstring.link(&jit);
    const GPRReg base = word;
    loadSubstringBaseAndOffset(jit, piece, base, characters);
    jit.loadPtr(Address(base, JSString::offsetOfValue()), base);
    isUnknown.append(jit.branchTest32(CCallHelpers::Zero, Address(base, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.addPtr(Address(base, StringImpl::dataOffset()), characters);
    isLoaded.link(&jit);
}

void flattenSubject(StubAssembler& jit, GPRReg string, std::initializer_list<GPRReg> preservedGPRs, Label continuation)
{
    const GPRReg exception = jit.reuseScratch(StubRegister::T9);
    RELEASE_ASSERT(std::ranges::find(preservedGPRs, exception) == preservedGPRs.end());
    jit.emitFunctionPrologue();
    jit.preserving(preservedGPRs, [&] {
        jit.move(string, operationArgumentGPR(1));
        jit.move(instanceGPR, operationArgumentGPR(0));
        jit.callOperation(Entry::operationAOTFlatten, jit.reuseScratch(StubRegister::Operation));
        jit.move(GPRInfo::returnValueGPR2, exception);
    });
    jit.emitFunctionEpilogue();
    jit.branchTestPtr(CCallHelpers::Zero, exception).linkTo(continuation, &jit);
    jit.jumpToExceptionHandler();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
