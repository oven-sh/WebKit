/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "JSString.h"
#include <algorithm>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

void loadSubstringBaseAndOffset(StubAssembler& jit, GPRReg string, GPRReg base, GPRReg offset)
{
    using CompactFibers = JSRopeString::CompactFibers;
    RELEASE_ASSERT(noOverlap(string, base, offset));
    static_assert(CompactFibers::widthOfFiber1LowerInLengthWord == 32 && CompactFibers::shiftOfFiber2InUpperWord == 16, "load32 and load16 read the two parts of fiber 1");
    jit.load16(Address(string, JSRopeString::offsetOfFiber2()), offset);
    jit.load32(Address(string, JSRopeString::offsetOfFiber1Lower()), base);
#if CPU(ARM64)
    jit.orLeftShift64(base, offset, TrustedImm32(CompactFibers::widthOfFiber1LowerInLengthWord), base);
#else
    jit.lshift64(TrustedImm32(CompactFibers::widthOfFiber1LowerInLengthWord), offset);
    jit.or64(offset, base);
#endif
    jit.load64(Address(string, JSRopeString::offsetOfFiber2()), offset);
    jit.urshift64(TrustedImm32(CompactFibers::shiftOfFiber2InUpperWord), offset);
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

// visit(width, addressIn) emits code for one word of that many bytes, at addressIn(start) for each of the starts. The words go up in address, lie inside the size bytes
// and cover them. The last one may overlap the one before it. Overwrites the starts and size.
static void forEachWordCovering(StubAssembler& jit, std::initializer_list<GPRReg> starts, GPRReg size, const auto& visit)
{
    auto atStart = [](GPRReg bytes) {
        return Address(bytes);
    };
    JumpList isDone;
    auto visitFirstAndLastWord = [&](unsigned width) {
        visit(width, atStart);
        visit(width, [&](GPRReg bytes) {
            return BaseIndex(bytes, size, CCallHelpers::TimesOne, -static_cast<int32_t>(width));
        });
        isDone.append(jit.jump());
    };

    constexpr int32_t wordSize = sizeof(uint64_t);
    Jump isBelowWordSize = jit.branch32(CCallHelpers::Below, size, TrustedImm32(wordSize));
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

    isBelowWordSize.link(&jit);
    Jump isBelowFour = jit.branch32(CCallHelpers::Below, size, TrustedImm32(4));
    visitFirstAndLastWord(4);

    isBelowFour.link(&jit);
    Jump isBelowTwo = jit.branch32(CCallHelpers::Below, size, TrustedImm32(2));
    visitFirstAndLastWord(2);

    isBelowTwo.link(&jit);
    isDone.append(jit.branchTest32(CCallHelpers::Zero, size));
    visit(1, atStart);
    isDone.link(&jit);
}

void branchIfBytesDiffer(StubAssembler& jit, const ComparedBytes& compared, JumpList& differ, DifferingWords differingWords)
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
    });
    UNUSED_PARAM(differingWords);
}

void branchUnlessASCIIOutsideRange(StubAssembler& jit, const ScannedBytes& scanned, uint8_t first, uint8_t last, JumpList& isNot)
{
    auto [bytes, size, word, atLeastFirst, aboveLast, addendForFirst, addendForLast] = scanned;
    RELEASE_ASSERT(noOverlap(bytes, size, word, atLeastFirst, aboveLast, addendForFirst, addendForLast));
    RELEASE_ASSERT(first <= last && last < 0x7f);
    constexpr uint64_t oneInEachByte = 0x0101010101010101ULL;
    constexpr uint64_t topBitOfEachByte = 0x80 * oneInEachByte;

    // Added to an ASCII byte, the addend sets the top bit from the given byte on, and carries nothing into the next byte.
    jit.move(TrustedImm64((0x80 - first) * oneInEachByte), addendForFirst);
    jit.move(TrustedImm64((0x80 - (last + 1)) * oneInEachByte), addendForLast);
    forEachWordCovering(jit, { bytes }, size, [&](unsigned width, auto addressIn) {
        loadZeroExtended(jit, width, addressIn(bytes), word);
        jit.add64(word, addendForFirst, atLeastFirst);
        jit.add64(word, addendForLast, aboveLast);
        jit.xor64(aboveLast, atLeastFirst);
        jit.or64(word, atLeastFirst);
        isNot.append(jit.branchTest64(CCallHelpers::NonZero, atLeastFirst, TrustedImm64(topBitOfEachByte)));
    });
}

void flattenSubject(StubAssembler& jit, GPRReg string, std::initializer_list<GPRReg> preservedGPRs, Label continuation)
{
    const GPRReg exception = jit.reuseScratch(StubRegister::T9);
    RELEASE_ASSERT(std::ranges::find(preservedGPRs, exception) == preservedGPRs.end());
    // The frame record is also what aligns sp for the call on x86-64.
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
