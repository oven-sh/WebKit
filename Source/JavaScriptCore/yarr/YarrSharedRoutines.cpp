/*
 * Copyright (C) 2009-2026 Apple Inc. All rights reserved.
 * Copyright (C) 2019-2026 the V8 project authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "YarrSharedRoutines.h"

#if ENABLE(YARR_JIT)

#include "YarrClassTable.h"
#include "YarrCodeAddressing.h"
#include "YarrSharedTables.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Yarr {

SharedRoutine::Kind SharedRoutine::kind() const
{
    return WTF::switchOn(payload,
        [](const MatchSet&) { return Kind::MatchSet; },
        [](const ReadCodePoint&) { return Kind::ReadCodePoint; },
        [](const SlowCodePointRead&) { return Kind::SlowCodePointRead; },
        [](const MatchCodePointAgainstClassTable&) { return Kind::MatchCodePointAgainstClassTable; },
        [](const ClearCaptures&) { return Kind::ClearCaptures; },
        [](const FindFirstOfClass&) { return Kind::FindFirstOfClass; },
        [](const CanonicalEquivalence&) { return Kind::CanonicalEquivalence; });
}

void add(Hasher& hasher, const SharedRoutine& routine)
{
    add(hasher, routine.kind());
    WTF::switchOn(routine.payload,
        [&](const SharedRoutine::MatchSet& payload) {
            add(hasher, payload.character, payload.scratch, payload.matches.size(), payload.matches);
            for (auto& range : payload.ranges)
                add(hasher, range.begin, range.end);
        },
        [](const SharedRoutine::ReadCodePoint&) { },
        [](const SharedRoutine::SlowCodePointRead&) { },
        [&](const SharedRoutine::MatchCodePointAgainstClassTable& payload) {
            add(hasher, payload.isInverted, payload.pairAdvance, payload.inputCheck);
        },
        [](const SharedRoutine::ClearCaptures&) { },
        [&](const SharedRoutine::FindFirstOfClass& payload) {
            add(hasher, payload.words, payload.matchesAllNonASCII, payload.charSize);
        },
        [&](const SharedRoutine::CanonicalEquivalence& payload) {
            add(hasher, payload.mode);
        });
}

bool canGenerateSharedRoutine(const SharedRoutine& routine)
{
    constexpr bool isSupportedCPU = isX86_64() || isARM64();
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
    constexpr bool decodesSurrogatePairs = true;
#else
    constexpr bool decodesSurrogatePairs = false;
#endif
#if ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
    constexpr bool comparesWideBackreferences = true;
#else
    constexpr bool comparesWideBackreferences = false;
#endif
    return WTF::switchOn(routine.payload,
        [](const SharedRoutine::MatchSet&) { return isSupportedCPU; },
        [](const SharedRoutine::ReadCodePoint&) { return decodesSurrogatePairs; },
        [](const SharedRoutine::SlowCodePointRead&) { return decodesSurrogatePairs; },
        [](const SharedRoutine::MatchCodePointAgainstClassTable&) { return decodesSurrogatePairs && isSupportedCPU; },
        [](const SharedRoutine::ClearCaptures&) { return isX86_64(); },
        [](const SharedRoutine::FindFirstOfClass&) { return isSupportedCPU; },
        [](const SharedRoutine::CanonicalEquivalence&) { return comparesWideBackreferences && rangeHiEntriesAboveBMP().has_value(); });
}

static void generateMatchSet(CCallHelpers& jit, const SharedRoutine::MatchSet& payload)
{
    RELEASE_ASSERT(payload.character != payload.scratch);
    MacroAssembler::JumpList matched;
    size_t matchIndex = 0;
    size_t rangeIndex = 0;
    while (matchIndex < payload.matches.size() || rangeIndex < payload.ranges.size()) {
        bool isMatchNext = rangeIndex == payload.ranges.size()
            || (matchIndex < payload.matches.size() && payload.matches[matchIndex] < payload.ranges[rangeIndex].begin);
        if (isMatchNext) {
            matched.append(jit.branch32(MacroAssembler::Equal, payload.character, MacroAssembler::Imm32(payload.matches[matchIndex++])));
            continue;
        }
        auto& range = payload.ranges[rangeIndex++];
        jit.sub32(payload.character, MacroAssembler::Imm32(range.begin), payload.scratch);
        matched.append(jit.branch32(MacroAssembler::BelowOrEqual, payload.scratch, MacroAssembler::TrustedImm32(range.end - range.begin)));
    }
    jit.move(MacroAssembler::TrustedImm32(0), payload.scratch);
    jit.ret();
    matched.link(&jit);
    jit.move(MacroAssembler::TrustedImm32(1), payload.scratch);
    jit.ret();
}

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
void emitReadSurrogatePair(CCallHelpers& jit, MacroAssembler::RegisterID resultReg, MacroAssembler::JumpList& slowCases)
{
    YarrJITDefaultRegisters regs;

    jit.add64(MacroAssembler::TrustedImm32(4), regs.regUnicodeInputAndTrail, regs.unicodeAndSubpatternIdTemp);
    slowCases.append(jit.branchPtr(MacroAssembler::Above, regs.unicodeAndSubpatternIdTemp, regs.endOfStringAddress));
    emitVerifySubjectRead(jit, MacroAssembler::Address(regs.regUnicodeInputAndTrail), 4, regs.input, regs.length, 1, __LINE__);
    jit.load32(MacroAssembler::Address(regs.regUnicodeInputAndTrail), resultReg);
    jit.and32(surrogateTagMask, resultReg, regs.unicodeAndSubpatternIdTemp);
    slowCases.append(jit.branch32(MacroAssembler::NotEqual, regs.unicodeAndSubpatternIdTemp, surrogatePairTags));
#if CPU(ARM64)
    jit.urshift32(resultReg, MacroAssembler::TrustedImm32(16), regs.unicodeAndSubpatternIdTemp);
    jit.insertBitField32(resultReg, MacroAssembler::TrustedImm32(10), MacroAssembler::TrustedImm32(10), regs.unicodeAndSubpatternIdTemp);
    jit.add32(MacroAssembler::TrustedImm32(0x10000), regs.unicodeAndSubpatternIdTemp, resultReg);
#else
    jit.and32(MacroAssembler::TrustedImm32(0xffff), resultReg, regs.unicodeAndSubpatternIdTemp);
    jit.lshift32(MacroAssembler::TrustedImm32(10), regs.unicodeAndSubpatternIdTemp);
    jit.urshift32(resultReg, MacroAssembler::TrustedImm32(16), resultReg);
    jit.getEffectiveAddress(MacroAssembler::BaseIndex(regs.unicodeAndSubpatternIdTemp, resultReg, MacroAssembler::TimesOne, -U16_SURROGATE_OFFSET), resultReg);
#endif
}
#endif

static void generateReadCodePoint(CCallHelpers& jit)
{
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
    MacroAssembler::JumpList slowCases;
    emitReadSurrogatePair(jit, YarrJITDefaultRegisters::regT0, slowCases);
    jit.ret();
    slowCases.link(&jit);
    tryReadUnicodeCharSlowImpl(jit);
    jit.ret();
#else
    UNUSED_PARAM(jit);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

static void generateSlowCodePointRead(CCallHelpers& jit)
{
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
    tryReadUnicodeCharSlowImpl(jit);
    jit.ret();
#else
    UNUSED_PARAM(jit);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && (CPU(X86_64) || CPU(ARM64))
struct ClassTableMatcherGenerator {
    static constexpr MacroAssembler::RegisterID character = YarrJITDefaultRegisters::regT0;
    static constexpr MacroAssembler::RegisterID unitAddress = YarrJITDefaultRegisters::regUnicodeInputAndTrail;
    static constexpr MacroAssembler::RegisterID temp = YarrJITDefaultRegisters::unicodeAndSubpatternIdTemp;
    static constexpr MacroAssembler::RegisterID table = classTableRegister;

    void returnWordBit()
    {
#if CPU(X86_64)
        jit.m_assembler.bt_ir(character, temp);
#else
        jit.urshift32(temp, character, temp);
#endif
        jit.ret();
    }

    void returnBlockBit()
    {
#if CPU(X86_64)
        jit.getEffectiveAddress(MacroAssembler::BaseIndex(table, temp, MacroAssembler::TimesFour), table);
        jit.urshift32(character, MacroAssembler::TrustedImm32(5), temp);
#else
        jit.addUnsignedRightShift32(temp, character, MacroAssembler::TrustedImm32(5), temp);
#endif
        jit.load32(MacroAssembler::BaseIndex(table, temp, MacroAssembler::TimesFour), temp);
        returnWordBit();
    }

    void loadIndexEntry(MacroAssembler::BaseIndex address)
    {
#if CPU(X86_64)
        jit.load16SignedExtendTo64(address, temp);
#else
        jit.load16SignedExtendTo32(address, temp);
#endif
    }

    void returnIsMember(bool isMember)
    {
#if CPU(X86_64)
        if (isMember)
            jit.m_assembler.stc();
        else
            jit.m_assembler.clc();
#else
        jit.move(MacroAssembler::TrustedImm32(isMember), temp);
#endif
        jit.ret();
    }

    void decodeSurrogates(MacroAssembler::JumpList& hasCodePoint)
    {
        jit.urshift32(character, MacroAssembler::TrustedImm32(11), temp);
        hasCodePoint.append(jit.branch32(MacroAssembler::NotEqual, temp, surrogateTagShiftedRight11));
        MacroAssembler::Jump isTrail = jit.branchTest32(MacroAssembler::NonZero, character, MacroAssembler::TrustedImm32(0x400));

        jit.add64(MacroAssembler::TrustedImm32(2 * sizeof(char16_t)), unitAddress, temp);
        hasCodePoint.append(jit.branchPtr(MacroAssembler::Above, temp, regs.endOfStringAddress));
        jit.load16Unaligned(MacroAssembler::Address(unitAddress, sizeof(char16_t)), temp);
        jit.urshift32(temp, MacroAssembler::TrustedImm32(10), unitAddress);
        hasCodePoint.append(jit.branch32(MacroAssembler::NotEqual, unitAddress, MacroAssembler::TrustedImm32(0xdc00 >> 10)));
#if CPU(X86_64)
        jit.lshift32(MacroAssembler::TrustedImm32(10), character);
        jit.getEffectiveAddress(MacroAssembler::BaseIndex(character, temp, MacroAssembler::TimesOne, -U16_SURROGATE_OFFSET), character);
#else
        jit.insertBitField32(character, MacroAssembler::TrustedImm32(10), MacroAssembler::TrustedImm32(10), temp);
        jit.add32(MacroAssembler::TrustedImm32(0x10000), temp, character);
#endif
        hasCodePoint.append(jit.jump());

        isTrail.link(&jit);
        hasCodePoint.append(jit.branchPtr(MacroAssembler::BelowOrEqual, unitAddress, regs.input));
        jit.load16Unaligned(MacroAssembler::Address(unitAddress, -static_cast<int32_t>(sizeof(char16_t))), temp);
        jit.urshift32(MacroAssembler::TrustedImm32(10), temp);
        hasCodePoint.append(jit.branch32(MacroAssembler::NotEqual, temp, MacroAssembler::TrustedImm32(0xd800 >> 10)));
        jit.move(MacroAssembler::TrustedImm32(errorCodePoint), character);
        returnIsMember(isInverted);
    }

    void testCodePoint(MacroAssembler::JumpList& isMember, MacroAssembler::JumpList& isNotMember)
    {
        jit.urshift32(character, MacroAssembler::TrustedImm32(8), temp);
        jit.load32(MacroAssembler::Address(table, ClassTableLayout::limitOffset), unitAddress);
        MacroAssembler::Jump isPastTable = jit.branch32(MacroAssembler::AboveOrEqual, temp, unitAddress);
        jit.add64(MacroAssembler::TrustedImm32(ClassTableLayout::upperIndexOffset - ClassTableLayout::firstSurrogatePage * sizeof(int16_t)), table, unitAddress);
        loadIndexEntry(MacroAssembler::BaseIndex(unitAddress, temp, MacroAssembler::TimesTwo));
#if CPU(X86_64)
        jit.getEffectiveAddress(MacroAssembler::BaseIndex(table, temp, MacroAssembler::TimesFour), table);
        jit.urshift32(character, MacroAssembler::TrustedImm32(5), temp);
        jit.load32(MacroAssembler::BaseIndex(table, temp, MacroAssembler::TimesFour), temp);
        isMember.append(jit.branchTestBit32(MacroAssembler::NonZero, temp, character));
#else
        jit.addUnsignedRightShift32(temp, character, MacroAssembler::TrustedImm32(5), temp);
        jit.load32(MacroAssembler::BaseIndex(table, temp, MacroAssembler::TimesFour), temp);
        jit.urshift32(temp, character, temp);
        isMember.append(jit.branchTest32(MacroAssembler::NonZero, temp, MacroAssembler::TrustedImm32(1)));
#endif
        isNotMember.append(jit.jump());

        isPastTable.link(&jit);
        jit.load32(MacroAssembler::Address(table, ClassTableLayout::trailingRangesOffset), temp);
        isNotMember.append(jit.branchTest32(MacroAssembler::Zero, temp));
        jit.addPtr(temp, table);
        jit.load32(MacroAssembler::Address(table), unitAddress);
        MacroAssembler::Label nextRange(&jit);
        jit.load32(MacroAssembler::Address(table, sizeof(uint32_t)), temp);
        jit.sub32(character, temp, temp);
        isMember.append(jit.branch32(MacroAssembler::BelowOrEqual, temp, MacroAssembler::Address(table, 2 * sizeof(uint32_t))));
        jit.addPtr(MacroAssembler::TrustedImm32(2 * sizeof(uint32_t)), table);
        jit.branchSub32(MacroAssembler::NonZero, MacroAssembler::TrustedImm32(1), unitAddress).linkTo(nextRange, &jit);
    }

    void generate()
    {
#if CPU(ARM64)
        MacroAssembler::RegisterID dataTemp = jit.getCachedDataTempRegisterIDAndInvalidate();
        ASSERT_UNUSED(dataTemp, dataTemp == table);
#endif
        MacroAssembler::JumpList hasNoInput;
        if (inputCheck != InputCheck::DoneByCaller)
            hasNoInput.append(jit.branch32(inputCheck == InputCheck::FailsIfIndexIsLength ? MacroAssembler::Equal : MacroAssembler::Above, regs.index, regs.length));
        jit.load16Unaligned(MacroAssembler::Address(unitAddress), character);
#if CPU(X86_64)
        MacroAssembler::Jump isPastFirstPage = jit.branch32(MacroAssembler::AboveOrEqual, character, MacroAssembler::TrustedImm32(0x100));
        jit.urshift32(character, MacroAssembler::TrustedImm32(5), temp);
        jit.load32(MacroAssembler::BaseIndex(table, temp, MacroAssembler::TimesFour, ClassTableLayout::firstBlockOffset), temp);
        returnWordBit();
        isPastFirstPage.link(&jit);
#endif
        jit.urshift32(character, MacroAssembler::TrustedImm32(8), temp);
        MacroAssembler::Jump isSurrogateOrAbove = jit.branch32(MacroAssembler::AboveOrEqual, temp, MacroAssembler::TrustedImm32(ClassTableLayout::firstSurrogatePage));
        loadIndexEntry(MacroAssembler::BaseIndex(table, temp, MacroAssembler::TimesTwo));
        returnBlockBit();

        isSurrogateOrAbove.link(&jit);
        MacroAssembler::JumpList hasCodePoint;
        decodeSurrogates(hasCodePoint);

        hasCodePoint.link(&jit);
        MacroAssembler::JumpList isMember;
        MacroAssembler::JumpList isNotMember;
        testCodePoint(isMember, isNotMember);

        auto& termFails = isInverted ? isMember : isNotMember;
        auto& termMatches = isInverted ? isNotMember : isMember;
        if (!isInverted)
            termFails.append(jit.jump());
        termMatches.link(&jit);
        if (pairAdvance != SurrogatePairAdvance::ByCaller) {
            MacroAssembler::Jump isSingleUnit = jit.branch32(MacroAssembler::Below, character, MacroAssembler::TrustedImm32(0x10000));
            if (pairAdvance == SurrogatePairAdvance::ClaimsSecondUnit)
                termFails.append(jit.branch32(MacroAssembler::Equal, regs.index, regs.length));
            else {
                jit.add32(MacroAssembler::TrustedImm32(1), regs.index, temp);
                termFails.append(jit.branch32(MacroAssembler::Equal, temp, regs.length));
            }
            jit.add32(MacroAssembler::TrustedImm32(1), regs.index);
            isSingleUnit.link(&jit);
        }
        returnIsMember(!isInverted);
        termFails.link(&jit);
        hasNoInput.link(&jit);
        returnIsMember(isInverted);
    }

    CCallHelpers& jit;
    const bool isInverted;
    const SurrogatePairAdvance pairAdvance;
    const InputCheck inputCheck;
    YarrJITDefaultRegisters regs { };
};
#endif

static void generateMatchCodePointAgainstClassTable(CCallHelpers& jit, const SharedRoutine::MatchCodePointAgainstClassTable& payload)
{
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && (CPU(X86_64) || CPU(ARM64))
    ClassTableMatcherGenerator { jit, payload.isInverted, payload.pairAdvance, payload.inputCheck }.generate();
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(payload);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

static void generateClearCaptures(CCallHelpers& jit)
{
#if CPU(X86_64)
    YarrJITDefaultRegisters regs;
    jit.push(X86Registers::edi);
    jit.push(X86Registers::ecx);
    jit.getEffectiveAddress(MacroAssembler::BaseIndex(regs.output, MacroAssembler::s_scratchRegister, MacroAssembler::TimesOne), X86Registers::edi);
    jit.move(regs.regT0, X86Registers::ecx);
    jit.move(MacroAssembler::TrustedImm64(-1), X86Registers::eax);
    jit.m_assembler.rep_stosq();
    jit.pop(X86Registers::ecx);
    jit.pop(X86Registers::edi);
    jit.ret();
#else
    UNUSED_PARAM(jit);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

#if CPU(X86_64) || CPU(ARM64)
struct FirstOfClassSearchGenerator {
    static constexpr int32_t charactersInVector = 16;
    static constexpr MacroAssembler::RegisterID character = YarrJITDefaultRegisters::regT0;
    static constexpr MacroAssembler::RegisterID bitmap = YarrJITDefaultRegisters::regT1;
    static constexpr MacroAssembler::RegisterID scratch = YarrJITDefaultRegisters::regT2;
    static constexpr MacroAssembler::RegisterID limit = YarrJITDefaultRegisters::regT3;
#if CPU(X86_64)
    static constexpr FPRReg bitmapBytes = YarrJITDefaultRegisters::vectorTemp0;
    static constexpr FPRReg byteIndexMask = YarrJITDefaultRegisters::vectorTemp1;
    static constexpr FPRReg singleBitTable = YarrJITDefaultRegisters::vectorTemp2;
    static constexpr FPRReg bitIndexMask = YarrJITDefaultRegisters::vectorTemp3;
    static constexpr FPRReg greatestByte = YarrJITDefaultRegisters::vectorTemp4;
    static constexpr FPRReg characters = YarrJITDefaultRegisters::vectorInput0;
    static constexpr FPRReg upperCharacters = YarrJITDefaultRegisters::vectorInput1;
    static constexpr FPRReg byteIndexes = upperCharacters;
    static constexpr FPRReg bitmapByte = YarrJITDefaultRegisters::vectorScratch0;
    static constexpr FPRReg characterBit = YarrJITDefaultRegisters::vectorScratch1;
#endif

    bool canUseVectors() const
    {
#if CPU(X86_64)
        return jit.supportsSupplementalSSE3() && (!isWide || jit.supportsSSE4_1());
#else
        return true;
#endif
    }

    void verifySubjectRead(unsigned characterCount, unsigned site)
    {
        emitVerifySubjectRead(jit, subject, characterCount << unitShift, regs.input, regs.length, unitShift, site);
    }

    void testBit(MacroAssembler::RegisterID word)
    {
#if CPU(X86_64)
        isMember.append(jit.branchTestBit64(MacroAssembler::NonZero, word, character));
#else
        jit.urshift64(word, character, scratch);
        isMember.append(jit.branchTest64(MacroAssembler::NonZero, scratch, MacroAssembler::TrustedImm32(1)));
#endif
    }

    void testOneCharacter()
    {
        MacroAssembler::JumpList notMember;
        verifySubjectRead(1, __LINE__);
        if (isWide) {
            jit.load16(subject, character);
            (matchesAllNonASCII ? isMember : notMember).append(jit.branch32(MacroAssembler::AboveOrEqual, character, MacroAssembler::TrustedImm32(0x80)));
        } else {
            jit.load8(subject, character);
            if (matchesAllNonASCII)
                isMember.append(jit.branchTest32(MacroAssembler::NonZero, character, MacroAssembler::TrustedImm32(0x80)));
        }
        bool isBelow128 = matchesAllNonASCII || isWide;
        switch (words) {
        case ClassWords::Low:
            notMember.append(jit.branchTest32(MacroAssembler::NonZero, character, MacroAssembler::TrustedImm32(isBelow128 ? 0x40 : 0xc0)));
            testBit(bitmap);
            break;
        case ClassWords::High:
            if (isBelow128)
                notMember.append(jit.branchTest32(MacroAssembler::Zero, character, MacroAssembler::TrustedImm32(0x40)));
            else {
                jit.and32(MacroAssembler::TrustedImm32(0xc0), character, scratch);
                notMember.append(jit.branch32(MacroAssembler::NotEqual, scratch, MacroAssembler::TrustedImm32(0x40)));
            }
            testBit(bitmap);
            break;
        case ClassWords::Both:
            if (!isBelow128)
                notMember.append(jit.branchTest32(MacroAssembler::NonZero, character, MacroAssembler::TrustedImm32(0x80)));
            jit.urshift32(character, MacroAssembler::TrustedImm32(6), scratch);
            jit.load64(MacroAssembler::BaseIndex(bitmap, scratch, MacroAssembler::TimesEight), scratch);
            testBit(scratch);
            break;
        }
        notMember.link(&jit);
        jit.add32(MacroAssembler::TrustedImm32(1), regs.index);
    }

    void loadVectorConstants()
    {
#if CPU(X86_64)
        auto& assembler = jit.m_assembler;
        switch (words) {
        case ClassWords::Low:
            assembler.movq_rr(bitmap, bitmapBytes);
            break;
        case ClassWords::High:
            assembler.movq_rr(bitmap, bitmapBytes);
            assembler.pslldq_i8r(8, bitmapBytes);
            break;
        case ClassWords::Both:
            assembler.movdqu_mr(0, bitmap, bitmapBytes);
            break;
        }
        jit.move(MacroAssembler::TrustedImm32(0x0f0f0f0f), character);
        assembler.movd_rr(character, byteIndexMask);
        assembler.pshufd_i8rr(0, byteIndexMask, byteIndexMask);
        jit.move(MacroAssembler::TrustedImm32(0x07070707), character);
        assembler.movd_rr(character, bitIndexMask);
        assembler.pshufd_i8rr(0, bitIndexMask, bitIndexMask);
        jit.move(MacroAssembler::TrustedImm64(0x8040201008040201ULL), character);
        assembler.movq_rr(character, singleBitTable);
        assembler.punpcklqdq_rr(singleBitTable, singleBitTable);
        if (isWide) {
            jit.move(MacroAssembler::TrustedImm32(0x00ff00ff), character);
            assembler.movd_rr(character, greatestByte);
            assembler.pshufd_i8rr(0, greatestByte, greatestByte);
        }
#else
        auto repeated = [](uint64_t half) {
            v128_t value;
            value.u64x2[0] = value.u64x2[1] = half;
            return value;
        };
        switch (words) {
        case ClassWords::Low:
            jit.move64ToDouble(bitmap, regs.vectorTemp0);
            break;
        case ClassWords::High:
            jit.moveZeroToVector(regs.vectorTemp0);
            jit.vectorReplaceLane(SIMDLane::i64x2, MacroAssembler::TrustedImm32(1), bitmap, regs.vectorTemp0);
            break;
        case ClassWords::Both:
            jit.loadVector(MacroAssembler::Address(bitmap), regs.vectorTemp0);
            break;
        }
        jit.move128ToVector(repeated(0x07070707'07070707ULL), regs.vectorTemp1);
        jit.move128ToVector(repeated(0x80402010'08040201ULL), regs.vectorTemp2);
        if (matchesAllNonASCII)
            jit.move128ToVector(repeated(0x80808080'80808080ULL), regs.vectorTemp3);
        if (isWide)
            jit.move128ToVector(repeated(0x00ff00ff'00ff00ffULL), regs.vectorTemp4);
#endif
    }

    MacroAssembler::Jump testVector()
    {
        verifySubjectRead(charactersInVector, __LINE__);
#if CPU(X86_64)
        auto& assembler = jit.m_assembler;
        if (isWide) {
            assembler.movdqu_mr(0, regs.input, regs.index, MacroAssembler::TimesTwo, characters);
            assembler.movdqu_mr(16, regs.input, regs.index, MacroAssembler::TimesTwo, upperCharacters);
            assembler.pminuw_rr(greatestByte, characters);
            assembler.pminuw_rr(greatestByte, upperCharacters);
            assembler.packuswb_rr(upperCharacters, characters);
        } else
            assembler.movdqu_mr(0, regs.input, regs.index, MacroAssembler::TimesOne, characters);
        assembler.pmovmskb_rr(characters, scratch);
        assembler.movaps_rr(characters, byteIndexes);
        assembler.psrlw_i8r(3, byteIndexes);
        assembler.pand_rr(byteIndexMask, byteIndexes);
        assembler.pand_rr(bitIndexMask, characters);
        assembler.movaps_rr(bitmapBytes, bitmapByte);
        assembler.pshufb_rr(byteIndexes, bitmapByte);
        assembler.movaps_rr(singleBitTable, characterBit);
        assembler.pshufb_rr(characters, characterBit);
        assembler.pand_rr(characterBit, bitmapByte);
        assembler.pcmpeqb_rr(characterBit, bitmapByte);
        assembler.pmovmskb_rr(bitmapByte, character);
        if (matchesAllNonASCII)
            jit.or32(scratch, character);
        else {
            jit.not32(scratch);
            jit.and32(scratch, character);
        }
        return jit.branchTest32(MacroAssembler::NonZero, character);
#else
        constexpr SIMDInfo bytes { SIMDLane::i8x16, SIMDSignMode::Unsigned };
        constexpr SIMDInfo whole { SIMDLane::v128, SIMDSignMode::None };
        jit.loadVector(subject, regs.vectorInput0);
        if (isWide) {
            constexpr SIMDInfo words16 { SIMDLane::i16x8, SIMDSignMode::Unsigned };
            jit.loadVector(MacroAssembler::BaseIndex(regs.input, regs.index, MacroAssembler::TimesTwo, 16), regs.vectorInput1);
            jit.vectorMin(words16, regs.vectorInput0, regs.vectorTemp4, regs.vectorInput0);
            jit.vectorMin(words16, regs.vectorInput1, regs.vectorTemp4, regs.vectorInput1);
            jit.vectorUnzipEven(bytes, regs.vectorInput0, regs.vectorInput1, regs.vectorInput0);
        }
        jit.vectorUshrInt8(regs.vectorInput0, 3, regs.vectorScratch1);
        jit.vectorAnd(whole, regs.vectorInput0, regs.vectorTemp1, regs.vectorScratch0);
        jit.vectorSwizzle(regs.vectorTemp0, regs.vectorScratch1, regs.vectorScratch2);
        jit.vectorSwizzle(regs.vectorTemp2, regs.vectorScratch0, regs.vectorScratch3);
        jit.vectorTest(bytes, regs.vectorScratch2, regs.vectorScratch3, regs.vectorScratch0);
        if (matchesAllNonASCII) {
            jit.vectorTest(bytes, regs.vectorInput0, regs.vectorTemp3, regs.vectorScratch1);
            jit.vectorOr(whole, regs.vectorScratch0, regs.vectorScratch1, regs.vectorScratch0);
        }
        jit.vectorShrnInt8(regs.vectorScratch0, 4, regs.vectorScratch0);
        jit.moveDoubleTo64(regs.vectorScratch0, character);
        return jit.branchTest64(MacroAssembler::NonZero, character);
#endif
    }

    void generate()
    {
        const bool usesVectors = canUseVectors();
        MacroAssembler::JumpList reachedEnd;
        MacroAssembler::Jump isLong;
        reachedEnd.append(jit.branch32(MacroAssembler::AboveOrEqual, regs.index, regs.length));
        if (usesVectors) {
            testOneCharacter();
            jit.add32(MacroAssembler::TrustedImm32(charactersInVector), regs.index, limit);
            isLong = jit.branch32(MacroAssembler::BelowOrEqual, limit, regs.length);
            reachedEnd.append(jit.branch32(MacroAssembler::AboveOrEqual, regs.index, regs.length));
        }
        MacroAssembler::Label next = jit.label();
        testOneCharacter();
        jit.branch32(MacroAssembler::Below, regs.index, regs.length).linkTo(next, &jit);
        reachedEnd.link(&jit);
        MacroAssembler::Label fails = jit.label();
        jit.move(regs.length, regs.index);
        jit.ret();

        isMember.link(&jit);
        jit.ret();

        if (!usesVectors)
            return;

        isLong.link(&jit);
        jit.sub32(regs.length, MacroAssembler::TrustedImm32(charactersInVector), limit);
        loadVectorConstants();
        MacroAssembler::Label nextVector = jit.label();
        MacroAssembler::Jump isInVector = testVector();
        jit.add32(MacroAssembler::TrustedImm32(charactersInVector), regs.index);
        jit.branch32(MacroAssembler::BelowOrEqual, regs.index, limit).linkTo(nextVector, &jit);
        jit.branch32(MacroAssembler::AboveOrEqual, regs.index, regs.length).linkTo(fails, &jit);
        jit.move(limit, regs.index);
        jit.jump().linkTo(nextVector, &jit);

        isInVector.link(&jit);
#if CPU(X86_64)
        jit.m_assembler.bsf_rr(character, character);
#else
        jit.reverseBits64(character, character);
        jit.countLeadingZeros64(character, character);
        jit.urshift64(MacroAssembler::TrustedImm32(2), character);
#endif
        jit.add32(character, regs.index);
        jit.ret();
    }

    CCallHelpers& jit;
    const ClassWords words;
    const bool matchesAllNonASCII;
    const bool isWide;
    const unsigned unitShift { isWide ? 1u : 0u };
    YarrJITDefaultRegisters regs { };
    MacroAssembler::BaseIndex subject { regs.input, regs.index, isWide ? MacroAssembler::TimesTwo : MacroAssembler::TimesOne };
    MacroAssembler::JumpList isMember { };
};
#endif

static void generateFindFirstOfClass(CCallHelpers& jit, const SharedRoutine::FindFirstOfClass& payload)
{
#if CPU(X86_64) || CPU(ARM64)
    FirstOfClassSearchGenerator { jit, payload.words, payload.matchesAllNonASCII, payload.charSize == CharSize::Char16 }.generate();
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(payload);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

static void generateCanonicalEquivalence(CCallHelpers& jit, const SharedRoutine::CanonicalEquivalence& payload, CodeAddressing& addressing)
{
#if ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
    constexpr auto first = areCanonicallyEquivalentCharArgReg;
    constexpr auto second = areCanonicallyEquivalentPattCharArgReg;
    constexpr auto scratch = areCanonicallyEquivalentCanonicalModeArgReg;
    jit.pushToSave(second);
    MacroAssembler::JumpList areNotEquivalent;
    MacroAssembler::JumpList haveForms;
    MacroAssembler::Jump firstIsAboveBMP = jit.branch32(MacroAssembler::AboveOrEqual, first, MacroAssembler::TrustedImm32(0x10000));
    areNotEquivalent.append(jit.branch32(MacroAssembler::AboveOrEqual, second, MacroAssembler::TrustedImm32(0x10000)));
    addressing.moveAddress(bmpCanonicalFormsTable(payload.mode), scratch);
    jit.load16(MacroAssembler::BaseIndex(scratch, first, MacroAssembler::TimesTwo), first);
    jit.load16(MacroAssembler::BaseIndex(scratch, second, MacroAssembler::TimesTwo), second);
    haveForms.append(jit.jump());
    firstIsAboveBMP.link(&jit);
    areNotEquivalent.append(jit.branch32(MacroAssembler::Below, second, MacroAssembler::TrustedImm32(0x10000)));
    if (payload.mode == CanonicalMode::Unicode) {
        for (auto character : { first, second }) {
            for (auto& range : *rangeHiEntriesAboveBMP()) {
                jit.move(character, scratch);
                jit.sub32(MacroAssembler::TrustedImm32(range.begin), scratch);
                MacroAssembler::Jump isOutside = jit.branch32(MacroAssembler::Above, scratch, MacroAssembler::TrustedImm32(range.end - range.begin));
                jit.sub32(MacroAssembler::TrustedImm32(range.value), character);
                isOutside.link(&jit);
            }
        }
    }
    haveForms.link(&jit);
    jit.compare32(MacroAssembler::Equal, first, second, first);
    MacroAssembler::Jump isDone = jit.jump();
    areNotEquivalent.link(&jit);
    jit.move(MacroAssembler::TrustedImm32(0), first);
    isDone.link(&jit);
    jit.popToRestore(second);
    jit.ret();
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(payload);
    UNUSED_PARAM(addressing);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

void generateSharedRoutine(CCallHelpers& jit, const SharedRoutine& routine, CodeAddressing& addressing)
{
    RELEASE_ASSERT(canGenerateSharedRoutine(routine));
    jit.tagReturnAddress();
    WTF::switchOn(routine.payload,
        [&](const SharedRoutine::MatchSet& payload) { generateMatchSet(jit, payload); },
        [&](const SharedRoutine::ReadCodePoint&) { generateReadCodePoint(jit); },
        [&](const SharedRoutine::SlowCodePointRead&) { generateSlowCodePointRead(jit); },
        [&](const SharedRoutine::MatchCodePointAgainstClassTable& payload) { generateMatchCodePointAgainstClassTable(jit, payload); },
        [&](const SharedRoutine::ClearCaptures&) { generateClearCaptures(jit); },
        [&](const SharedRoutine::FindFirstOfClass& payload) { generateFindFirstOfClass(jit, payload); },
        [&](const SharedRoutine::CanonicalEquivalence& payload) { generateCanonicalEquivalence(jit, payload, addressing); });
}

} } // namespace JSC::Yarr

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(YARR_JIT)
