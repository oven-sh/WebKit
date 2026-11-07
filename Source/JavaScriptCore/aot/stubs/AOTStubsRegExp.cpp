/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsRegExp.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRecordTypes.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsArrays.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsRegExpFrames.h"
#include "AOTStubsStrings.h"
#include "Butterfly.h"
#include "CompleteSubspace.h"
#include "IndexingHeader.h"
#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSString.h"
#include "MarkedSpace.h"
#include "MatchResult.h"
#include "RegExp.h"
#include "RegExpCachedResult.h"
#include "RegExpGlobalData.h"
#include "RegExpMatchesArray.h"
#include "RegExpObject.h"
#include "YarrInterpreter.h"
#include "YarrJIT.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

struct MatchRegisters {
    GPRReg scratch;
    GPRReg impl;
    GPRReg regExp;
    GPRReg characters;
    GPRReg subjectLength;
    GPRReg length;
};

struct MatchOperands {
    GPRReg regExpObject;
    GPRReg string;

    std::array<GPRReg, 2> all() const { return { regExpObject, string }; }
};

struct MatchAnswers {
    Entry generic;
    std::optional<int64_t> match;
    int64_t miss;
};

StubSignature signatureOfRegExpMatch(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }
StubSignature signatureOfStringReplaceRegExpMayNotMatch(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) }); }
StubSignature signatureOfNewRegExpLiteral(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3), gprOf(StubRegister::T9) }); }

StubSignature signatureOfFinishRegExpExec(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
    if (static_cast<FinishRegExpExecForm>(key.form()) == FinishRegExpExecForm::WithoutObject)
        signature.addOperand(gprOf(StubRegister::T9));
    return signature;
}

static MatchAnswers answersOf(RegExpMatchResult result)
{
    switch (result) {
    case RegExpMatchResult::Array:
        return { Entry::operationAOTRegExpExec, std::nullopt, JSValue::encode(jsNull()) };
    case RegExpMatchResult::Boolean:
        return { Entry::operationAOTRegExpTest, 1, 0 };
    case RegExpMatchResult::BoxedBoolean:
        return { Entry::operationAOTRegExpTestBoxed, JSValue::encode(jsBoolean(true)), JSValue::encode(jsBoolean(false)) };
    case RegExpMatchResult::Parts:
        return { Entry::operationAOTRegExpMatchInParts, JSValue::encode(jsBoolean(true)), JSValue::encode(jsBoolean(false)) };
    case RegExpMatchResult::ArrayForStringMatch:
        return { Entry::operationAOTStringMatchRegExp, std::nullopt, JSValue::encode(jsNull()) };
    case RegExpMatchResult::IndexForStringSearch:
        return { Entry::operationAOTStringSearchRegExp, std::nullopt, JSValue::encode(jsNumber(-1)) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MatchRegisters allocateMatchRegisters(StubAssembler& jit)
{
    return {
        .scratch = jit.allocateScratch(StubRegister::T9),
        .impl = jit.allocateScratch(StubRegister::T10),
        .regExp = jit.allocateScratch(StubRegister::T11),
        .characters = jit.allocateScratch(StubRegister::A0),
        .subjectLength = jit.allocateScratch(StubRegister::T13),
#if CPU(ARM64)
        .length = jit.allocateScratch(StubRegister::T12),
#else
        .length = jit.result(),
#endif
    };
}

[[nodiscard]] static Jump loadFlatSubject(StubAssembler& jit, const MatchRegisters& registers, GPRReg string)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    jit.loadPtr(Address(string, JSString::offsetOfValue()), impl);
    Jump isRope = jit.branchIfRopeStringImpl(impl);
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), characters);
    jit.load32(Address(impl, StringImpl::lengthMemoryOffset()), subjectLength);
    return isRope;
}

static void loadRegExpAndFlags(StubAssembler& jit, const MatchRegisters& registers, GPRReg regExpObject)
{
    const GPRReg flags = registers.length;
    loadRegExp(jit, regExpObject, registers.regExp);
    jit.load16(Address(registers.regExp, RegExp::offsetOfFlags()), flags);
}

[[nodiscard]] static Jump branchIfNotCompiled(StubAssembler& jit, GPRReg regExp)
{
    return jit.branch8(CCallHelpers::NotEqual, Address(regExp, RegExp::offsetOfState()), TrustedImm32(RegExp::stateWithImageCode()));
}

static void loadMatchingCode(StubAssembler& jit, const MatchRegisters& registers, JumpList& slowCases)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg flags = length;
    jit.loadPtr(Address(regExp, RegExp::offsetOfRegExpJITCode()), scratch);
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.loadPtr(Address(scratch, Yarr::YarrCodeBlock::offsetOf8BitCode()), scratch);
    Jump is8Bit = jit.jump();
    is16Bit.link(&jit);
    slowCases.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(unicodeFlags)));
    jit.loadPtr(Address(scratch, Yarr::YarrCodeBlock::offsetOf16BitCode()), scratch);
    is8Bit.link(&jit);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
}

static void callMatchingCode(StubAssembler& jit, const MatchRegisters& registers, std::span<const GPRReg> savedOperands, std::optional<GPRReg> start)
{
    ASSERT(registers.characters == gprOf(StubRegister::A0));
    callMatchingCodeInFrame(jit, savedOperands, registers.subjectLength, start, registers.scratch, [&](GPRReg output) {
        loadMatchOutput(jit, output);
    }, [&] {
        jit.call(registers.scratch, Yarr8BitPtrTag);
    });
}

static void loadAtom(StubAssembler& jit, const MatchRegisters& registers, JumpList& hasNoAtom, JumpList& slowCases, JumpList& isInNoNarrowText)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg atom = scratch;
    jit.loadPtr(Address(regExp, RegExp::offsetOfAtom()), atom);
    hasNoAtom.append(jit.branchTestPtr(CCallHelpers::Zero, atom));
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    Jump atomIsNarrow = jit.branchTest32(CCallHelpers::NonZero, Address(atom, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.loadPtr(Address(atom, StringImpl::dataOffset()), atom);
    jit.load16(Address(atom), atom);
    isInNoNarrowText.append(jit.branch32(CCallHelpers::Above, atom, TrustedImm32(0xff)));
    slowCases.append(jit.jump());
    atomIsNarrow.link(&jit);
}

static void searchForAtom(StubAssembler& jit, const MatchRegisters& registers, std::span<const GPRReg> savedOperands)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg atom = scratch;
    const GPRReg lengthArgument = jit.reuseScratch(StubRegister::A1);
    const GPRReg atomCharacters = jit.reuseScratch(StubRegister::A2);
    const GPRReg atomLength = jit.reuseScratch(StubRegister::A3);
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1) && leafArgumentGPR(2) == gprOf(StubRegister::A2));
    static_assert(leafArgumentGPR(3) == gprOf(StubRegister::A3) && leafArgumentGPR(4) == gprOf(StubRegister::A4));
    ASSERT(characters == leafArgumentGPR(0));
    pushMatchFrame(jit, savedOperands);
    jit.move(subjectLength, lengthArgument);
    jit.loadPtr(Address(atom, StringImpl::dataOffset()), atomCharacters);
    jit.load32(Address(atom, StringImpl::lengthMemoryOffset()), atomLength);
    Jump isOneCharacter = jit.branch32(CCallHelpers::Equal, atomLength, TrustedImm32(1));
    jit.move(TrustedImm32(0), jit.reuseScratch(StubRegister::A4));
    jit.callLeaf(Entry::operationAOTStringFind8, bytesPushedInMatchFrame);
    Jump isSearched = jit.jump();

    isOneCharacter.link(&jit);
    const GPRReg character = atomCharacters;
    const GPRReg start = atomLength;
    jit.load8(Address(atomCharacters), character);
    jit.move(TrustedImm32(0), start);
    jit.callLeaf(Entry::operationAOTStringFindCharacter8, bytesPushedInMatchFrame);

    isSearched.link(&jit);
    popMatchFrame(jit, scratch, savedOperands);
}

static void matchFromLastIndex(StubAssembler& jit, const MatchRegisters& registers, const MatchOperands& operands, const MatchAnswers& answers, JumpList& slowCases, Label hasFound)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg regExpObject = operands.regExpObject;
    const GPRReg lastIndex = scratch;
    const GPRReg flags = length;
    const GPRReg start = jit.reuseScratch(StubRegister::A3);
    slowCases.append(jit.branchIfNotInt32(lastIndex));
    slowCases.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(unicodeFlags)));
    slowCases.append(branchIfReadOnlyLastIndex(jit, regExpObject));
    jit.zeroExtend32ToWord(lastIndex, start);
    slowCases.append(jit.branch32(CCallHelpers::Above, start, subjectLength));
    slowCases.append(branchIfNotCompiled(jit, regExp));
    loadMatchingCode(jit, registers, slowCases);
    jit.sub32(subjectLength, start, length);
    Jump restIsTooShort = jit.branch32(CCallHelpers::Below, length, Address(regExp, RegExp::offsetOfMinimumSize()));
    callMatchingCode(jit, registers, operands.all(), start);
    Jump didNotMiss = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    restIsTooShort.link(&jit);
    resetLastIndex(jit, regExpObject);
    returnResult(jit, TrustedImm64(answers.miss));

    didNotMiss.link(&jit);
    slowCases.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm64(0)));
    jit.jump().linkTo(hasFound, &jit);
}

static void matchOnFlatProgram(StubAssembler& jit, const MatchRegisters& registers, const MatchOperands& operands, const MatchAnswers& answers, JumpList& slowCases, Label hasFound)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg program = scratch;
    const GPRReg programArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg charactersArgument = jit.reuseScratch(StubRegister::A1);
    const GPRReg lengthArgument = jit.reuseScratch(StubRegister::A2);
    const GPRReg output = jit.reuseScratch(StubRegister::A3);
    const GPRReg instanceArgument = jit.reuseScratch(StubRegister::A4);
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1) && leafArgumentGPR(2) == gprOf(StubRegister::A2) && leafArgumentGPR(3) == gprOf(StubRegister::A3) && leafArgumentGPR(4) == gprOf(StubRegister::A4));
    static_assert(sizeof(std::unique_ptr<Yarr::BytecodePattern>) == sizeof(void*) && sizeof(std::unique_ptr<Yarr::FlatProgram>) == sizeof(void*));
    ASSERT(characters == programArgument);
    RELEASE_ASSERT(noOverlap(impl, programArgument, charactersArgument, lengthArgument, output));
    RELEASE_ASSERT(noOverlap(programArgument, charactersArgument, lengthArgument, output, instanceArgument, instanceGPR));

    slowCases.append(jit.branch8(CCallHelpers::NotEqual, Address(regExp, RegExp::offsetOfState()), TrustedImm32(RegExp::stateWithBytecode())));
    slowCases.append(jit.branch32(CCallHelpers::Above, Address(regExp, RegExp::offsetOfNumSubpatterns()), TrustedImm32(VMState::maxGroupsInRegExpMatchOutput)));
    const GPRReg bytecode = length;
    jit.loadPtr(Address(regExp, RegExp::offsetOfRegExpBytecode()), bytecode);
    jit.loadPtr(Address(bytecode, Yarr::BytecodePattern::offsetOfFlatProgram()), program);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, program));

    pushMatchFrame(jit, operands.all());
    jit.move(subjectLength, lengthArgument);
    jit.move(characters, charactersArgument);
    loadMatchOutput(jit, output);
    jit.move(program, programArgument);
    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    jit.move(instanceGPR, instanceArgument);
    jit.callLeaf(Entry::operationAOTRegExpMatchFlatProgram8, bytesPushedInMatchFrame);
    Jump isSearched = jit.jump();
    is16Bit.link(&jit);
    jit.move(instanceGPR, instanceArgument);
    jit.callLeaf(Entry::operationAOTRegExpMatchFlatProgram16, bytesPushedInMatchFrame);
    isSearched.link(&jit);
    popMatchFrame(jit, scratch, operands.all());
    Jump didNotMiss = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    returnResult(jit, TrustedImm64(answers.miss));

    didNotMiss.link(&jit);
    slowCases.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm64(0)));
    jit.jump().linkTo(hasFound, &jit);
}

static void matchSubstringForArray(StubAssembler& jit, const MatchRegisters& registers, const MatchOperands& operands, const MatchAnswers& answers, JumpList& needsFlatSubject, JumpList& slowCases, Label hasFound)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg flags = length;
    jit.load64(Address(operands.regExpObject, RegExpObject::offsetOfLastIndex()), scratch);
    slowCases.append(jit.branchIfNotNumber(scratch));
    loadRegExpAndFlags(jit, registers, operands.regExpObject);
    slowCases.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(globalFlag | stickyFlag)));
    Jump isTooShort = jit.branch32(CCallHelpers::Below, subjectLength, Address(regExp, RegExp::offsetOfMinimumSize()));
    Jump hasNoCode = branchIfNotCompiled(jit, regExp);
    loadMatchingCode(jit, registers, slowCases);
    callMatchingCode(jit, registers, operands.all(), std::nullopt);
    Label isSearched = jit.label();
    Jump didNotMiss = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    isTooShort.link(&jit);
    Label isMiss = jit.label();
    returnResult(jit, TrustedImm64(answers.miss));

    didNotMiss.link(&jit);
    slowCases.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm64(0)));
    flattenSubject(jit, operands.string, { operands.regExpObject, operands.string }, hasFound);

    hasNoCode.link(&jit);
    JumpList hasNoAtom;
    JumpList isInNoNarrowText;
    loadAtom(jit, registers, hasNoAtom, slowCases, isInNoNarrowText);
    isInNoNarrowText.linkTo(isMiss, &jit);
    searchForAtom(jit, registers, operands.all());
    jit.branch64(CCallHelpers::Equal, scratch, TrustedImm64(noMatch)).linkTo(isMiss, &jit);
    needsFlatSubject.append(jit.jump());

    hasNoAtom.link(&jit);
    matchOnFlatProgram(jit, registers, operands, answers, slowCases, isSearched);
}

static void matchAtom(StubAssembler& jit, const MatchRegisters& registers, const MatchOperands& operands, const MatchAnswers& answers, JumpList& slowCases, Label hasFound)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg output = regExp;
    JumpList hasNoAtom;
    JumpList isInNoNarrowText;
    loadAtom(jit, registers, hasNoAtom, slowCases, isInNoNarrowText);
    searchForAtom(jit, registers, operands.all());
    Jump isFound = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    isInNoNarrowText.link(&jit);
    returnResult(jit, TrustedImm64(answers.miss));

    isFound.link(&jit);
    loadRegExp(jit, operands.regExpObject, regExp);
    jit.loadPtr(Address(regExp, RegExp::offsetOfAtom()), length);
    jit.load32(Address(length, StringImpl::lengthMemoryOffset()), length);
    jit.add32(scratch, length);
    loadMatchOutput(jit, output);
    jit.store32(scratch, matchOutputEntry(output, 0));
    jit.store32(length, matchOutputEntry(output, 1));
    jit.jump().linkTo(hasFound, &jit);

    hasNoAtom.link(&jit);
    matchOnFlatProgram(jit, registers, operands, answers, slowCases, hasFound);
}

void recordMatch(StubAssembler& jit, const MatchRecordRegisters& registers)
{
    constexpr ptrdiff_t cachedResult = JSGlobalObject::regExpGlobalDataOffset() + RegExpGlobalData::offsetOfCachedResult();
    constexpr ptrdiff_t matchResult = cachedResult + RegExpCachedResult::offsetOfResult();
    static_assert(sizeof(MatchResult::start) == sizeof(uint64_t) && sizeof(MatchResult::end) == sizeof(uint64_t));
    const auto [regExp, string, offsets, globalObject, scratch] = registers;
    RELEASE_ASSERT(noOverlap(regExp, string, offsets, globalObject, scratch, instanceGPR));

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), globalObject);
    jit.storePtr(regExp, Address(globalObject, cachedResult + RegExpCachedResult::offsetOfLastRegExp()));
    jit.storePtr(string, Address(globalObject, cachedResult + RegExpCachedResult::offsetOfLastInput()));
    jit.load32(matchOutputEntry(offsets, 0), scratch);
    jit.store64(scratch, Address(globalObject, matchResult + OBJECT_OFFSETOF(MatchResult, start)));
    jit.load32(matchOutputEntry(offsets, 1), scratch);
    jit.store64(scratch, Address(globalObject, matchResult + OBJECT_OFFSETOF(MatchResult, end)));
    jit.store8(TrustedImm32(0), Address(globalObject, cachedResult + RegExpCachedResult::offsetOfReified()));
    jit.store8(TrustedImm32(0), Address(globalObject, cachedResult + RegExpCachedResult::offsetOfOneCharacterMatch()));

    Jump needsBarrier = branchIfBarrierIsNeeded(jit, globalObject, scratch);
    Label isRecorded = jit.label();
    jit.addOutOfLinePart([needsBarrier, isRecorded, owner = globalObject](StubAssembler& part) {
        needsBarrier.link(&part);
        part.emitFunctionPrologue();
        part.move(owner, part.clobberRegister(firstStubOperandRegister));
        part.callStub(writeBarrier());
        part.emitFunctionEpilogue();
        part.jump().linkTo(isRecorded, &part);
    });
}

static void advanceLastIndexAndRecordMatch(StubAssembler& jit, GPRReg regExpObject, GPRReg flags, const MatchRecordRegisters& registers)
{
    loadRegExp(jit, regExpObject, registers.regExp);
    jit.load16(Address(registers.regExp, RegExp::offsetOfFlags()), flags);
    loadMatchOutput(jit, registers.offsets);
    Jump keepsLastIndex = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(globalFlag | stickyFlag));
    storeEndOfMatchInLastIndex(jit, regExpObject, registers.offsets, registers.scratch);
    keepsLastIndex.link(&jit);
    if (jit.options().recordsLastMatch)
        recordMatch(jit, registers);
}

static void recordMatchThatKeepsLastIndex(StubAssembler& jit, GPRReg regExpObject, const MatchRecordRegisters& registers)
{
    if (!jit.options().recordsLastMatch)
        return;
    loadRegExp(jit, regExpObject, registers.regExp);
    loadMatchOutput(jit, registers.offsets);
    recordMatch(jit, registers);
}

static void finishMatchAndReturn(StubAssembler& jit, RegExpMatchResult form, const MatchRegisters& registers, const MatchOperands& operands, const MatchAnswers& answers, bool keepsLastIndex)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const MatchRecordRegisters record { .regExp = regExp, .string = operands.string, .offsets = characters, .globalObject = impl, .scratch = scratch };
    switch (form) {
    case RegExpMatchResult::ArrayForStringMatch:
        jit.swap(operands.regExpObject, operands.string);
        checkStubOperands(finishRegExpExec(FinishRegExpExecForm::WithObject), { operands.string, operands.regExpObject });
        jit.jumpToStub(finishRegExpExec(FinishRegExpExecForm::WithObject));
        return;
    case RegExpMatchResult::Array:
        checkStubOperands(finishRegExpExec(FinishRegExpExecForm::WithObject), { operands.regExpObject, operands.string });
        jit.jumpToStub(finishRegExpExec(FinishRegExpExecForm::WithObject));
        return;
    case RegExpMatchResult::IndexForStringSearch:
        recordMatchThatKeepsLastIndex(jit, operands.regExpObject, record);
        loadMatchOutput(jit, jit.result());
        jit.load32(matchOutputEntry(jit.result(), 0), jit.result());
        boxInt32(jit, jit.result(), jit.result());
        jit.returnToCaller();
        return;
    case RegExpMatchResult::Boolean:
    case RegExpMatchResult::BoxedBoolean:
    case RegExpMatchResult::Parts:
        if (keepsLastIndex)
            recordMatchThatKeepsLastIndex(jit, operands.regExpObject, record);
        else
            advanceLastIndexAndRecordMatch(jit, operands.regExpObject, length, record);
        returnResult(jit, TrustedImm64(*answers.match));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr size_t offsetOfButterflyInMatchArrayStorage = initialOutOfLineCapacity * sizeof(EncodedJSValue) + sizeof(IndexingHeader);
static constexpr unsigned maxGroupsOfMatchArray = (MarkedSpace::largeCutoff - offsetOfButterflyInMatchArrayStorage) / sizeof(EncodedJSValue) - 1;
static constexpr PropertyOffset endOfMatchArrayProperties = firstOutOfLineOffset + initialOutOfLineCapacity;
static_assert(RegExpMatchesArrayIndexPropertyOffset == firstOutOfLineOffset && RegExpMatchesArrayGroupsPropertyOffset > RegExpMatchesArrayInputPropertyOffset && RegExpMatchesArrayGroupsPropertyOffset < endOfMatchArrayProperties);

struct MatchArrayRegisters {
    GPRReg string;
    GPRReg butterfly;
    GPRReg offsets;
    GPRReg index;
    GPRReg piece;
    GPRReg start;
    GPRReg length;
    GPRReg allocator;
    GPRReg array;
};

static Address matchArrayProperty(GPRReg butterfly, PropertyOffset offset)
{
    return Address(butterfly, static_cast<int32_t>(offsetInButterfly(offset) * static_cast<intptr_t>(sizeof(EncodedJSValue))));
}

static void tryCreateMatchArray(StubAssembler& jit, const MatchArrayRegisters& registers)
{
    static_assert(MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
    constexpr unsigned wordsBesideGroups = offsetOfButterflyInMatchArrayStorage / sizeof(EncodedJSValue) + 1;
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::RegExpMatchesArray);
    const auto [string, butterfly, offsets, index, piece, start, length, allocator, array] = registers;
    const GPRReg numberOfElements = start;
    const GPRReg scratch = array;
    const GPRReg header = piece;
    JumpList fails;
    JumpList tookNoPart;

    jit.add32(TrustedImm32(wordsBesideGroups + 1), start, scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    fails.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    AssemblyHelpers::FreeListRefill storageRefill = allocateCellWithLateRefill(jit, butterfly, allocator, scratch, fails);
    jit.addPtr(TrustedImm32(offsetOfButterflyInMatchArrayStorage), butterfly);
    jit.add32(TrustedImm32(1), numberOfElements);
    jit.store32(numberOfElements, Address(butterfly, Butterfly::offsetOfPublicLength()));
    jit.store32(numberOfElements, Address(butterfly, Butterfly::offsetOfVectorLength()));

    loadMatchOutput(jit, offsets);
    jit.load32(matchOutputEntry(offsets, 0), scratch);
    boxInt32(jit, scratch, scratch);
    jit.store64(scratch, matchArrayProperty(butterfly, RegExpMatchesArrayIndexPropertyOffset));
    jit.storePtr(string, matchArrayProperty(butterfly, RegExpMatchesArrayInputPropertyOffset));
    jit.store64(TrustedImm32(JSValue::ValueUndefined), matchArrayProperty(butterfly, RegExpMatchesArrayGroupsPropertyOffset));
    for (PropertyOffset unused = RegExpMatchesArrayGroupsPropertyOffset + 1; unused < endOfMatchArrayProperties; ++unused)
        jit.store64(TrustedImm32(0), matchArrayProperty(butterfly, unused));

    jit.move(TrustedImm32(0), index);
    Label next = jit.label();
    jit.load32(BaseIndex(offsets, index, CCallHelpers::TimesEight), start);
    jit.load32(BaseIndex(offsets, index, CCallHelpers::TimesEight, sizeof(int)), length);
    jit.move(TrustedImm32(JSValue::ValueUndefined), piece);
    tookNoPart.append(jit.branch32(CCallHelpers::LessThan, start, TrustedImm32(0)));
    tookNoPart.append(jit.branchSub32(CCallHelpers::Signed, start, length));
    AssemblyHelpers::FreeListRefill pieceRefill = makeSubstringOfFlatString(jit, { .string = string, .start = start, .length = length, .result = piece, .firstScratch = allocator, .secondScratch = array, .alsoKept = RegisterSet(butterfly, offsets, index) }, fails);
    tookNoPart.link(&jit);
    jit.store64(piece, BaseIndex(butterfly, index, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::Below, index, Address(butterfly, Butterfly::offsetOfPublicLength())).linkTo(next, &jit);

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator)), allocator);
    AssemblyHelpers::FreeListRefill arrayRefill = allocateCellWithLateRefill(jit, array, allocator, header, fails);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header)), header);
    jit.storePair64(header, butterfly, array, TrustedImm32(0));
    fenceAfterAllocation(jit);
    Jump isCreated = jit.jump();

    emitLateRefill(jit, storageRefill, fails);
    emitLateRefill(jit, pieceRefill, fails);
    emitLateRefill(jit, arrayRefill, fails);
    fails.link(&jit);
    jit.move(TrustedImm32(0), array);
    isCreated.link(&jit);
}

void generateFinishRegExpExec(StubAssembler& jit)
{
    static_assert(operationArgumentGPR(1) == gprOf(StubRegister::A1) && operationArgumentGPR(2) == gprOf(StubRegister::A2));
    static_assert(operationArgumentGPR(3) == gprOf(StubRegister::A3) && operationArgumentGPR(4) == gprOf(StubRegister::A4));
    static_assert(JSObject::butterflyOffset() == sizeof(JSCell) && sizeof(JSArray) == sizeof(JSCell) + sizeof(void*));
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::RegExpMatchesArray);
    const bool hasObject = static_cast<FinishRegExpExecForm>(jit.key().form()) == FinishRegExpExecForm::WithObject;
    const GPRReg objectOrRecord = jit.clobberOperand(0);
    const GPRReg string = jit.operand(1);
    const GPRReg scratch = hasObject ? jit.allocateScratch(StubRegister::T9) : jit.clobberOperand(2);
    const GPRReg globalObject = jit.allocateScratch(StubRegister::T10);
    const GPRReg regExp = jit.allocateScratch(StubRegister::T11);
    const GPRReg flags = jit.allocateScratch(StubRegister::T12);
    const GPRReg array = jit.allocateScratch(StubRegister::A3);
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T13);
    const GPRReg numberOfGroups = scratch;
    JumpList needsOperation;

    if (hasObject) {
        loadRegExp(jit, objectOrRecord, regExp);
        needsOperation.append(jit.branchTest16(CCallHelpers::NonZero, Address(regExp, RegExp::offsetOfFlags()), TrustedImm32(hasIndicesFlag)));
        needsOperation.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(regExp, RegExp::offsetOfRareData())));
        jit.load32(Address(regExp, RegExp::offsetOfNumSubpatterns()), numberOfGroups);
    }
    needsOperation.append(jit.branch32(CCallHelpers::Above, numberOfGroups, TrustedImm32(maxGroupsOfMatchArray)));
    needsOperation.append(branchIfHavingABadTime(jit));
    needsOperation.append(jit.branchTestPtr(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator))));

    jit.preserving({ objectOrRecord }, [&] {
        tryCreateMatchArray(jit, {
            .string = string,
            .butterfly = butterfly,
            .offsets = regExp,
            .index = objectOrRecord,
            .piece = jit.result(),
            .start = numberOfGroups,
            .length = globalObject,
            .allocator = flags,
            .array = array,
        });
    });
    needsOperation.append(jit.branchTest64(CCallHelpers::Zero, array));
    if (hasObject)
        advanceLastIndexAndRecordMatch(jit, objectOrRecord, flags, { .regExp = regExp, .string = string, .offsets = jit.result(), .globalObject = globalObject, .scratch = scratch });
    returnResult(jit, array);

    needsOperation.link(&jit);
    const GPRReg record = jit.reuseScratch(StubRegister::A3);
    if (hasObject)
        jit.move(TrustedImm32(static_cast<int32_t>(noRegExpRecord)), record);
    else {
        jit.move(objectOrRecord, record);
        jit.move(TrustedImm32(0), objectOrRecord);
    }
    jit.move(TrustedImm32(static_cast<int32_t>(RegExpMatchForm::Exec)), jit.reuseScratch(StubRegister::A4));
    jit.callOperationAndReturn(Entry::operationAOTRegExpMatchLiteral, { });
}

static void checkOriginalRegExp(StubAssembler& jit, GPRReg regExpObject, GPRReg scratch, JumpList& slowCases)
{
    jit.load32(Address(regExpObject, JSCell::structureIDOffset()), scratch);
    slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, Instance::offsetOfReceiverStructureID(Receiver::RegExp))));
}

void generateRegExpMatch(StubAssembler& jit)
{
    const auto form = static_cast<RegExpMatchResult>(jit.key().form());
    const MatchAnswers answers = answersOf(form);
    const bool stringIsInFront = takesStringInFrontOfRegExp(form);
    const MatchOperands operands { .regExpObject = jit.clobberOperand(stringIsInFront ? 1 : 0), .string = jit.clobberOperand(stringIsInFront ? 0 : 1) };
    const auto [regExpObject, string] = operands;
    const MatchRegisters registers = allocateMatchRegisters(jit);
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg flags = length;
    JumpList slowCases;

    if (stringIsInFront)
        checkOriginalRegExp(jit, regExpObject, scratch, slowCases);
    Label begin = jit.label();
    Jump isRope = loadFlatSubject(jit, registers, string);
    Label hasCharacters = jit.label();
    jit.load64(Address(regExpObject, RegExpObject::offsetOfLastIndex()), scratch);
    if (form == RegExpMatchResult::IndexForStringSearch)
        slowCases.append(jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(JSValue::encode(jsNumber(0)))));
    else
        slowCases.append(jit.branchIfNotNumber(scratch));
    loadRegExpAndFlags(jit, registers, regExpObject);
    Jump usesLastIndex = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(globalFlag | stickyFlag));
    Jump isTooShort = jit.branch32(CCallHelpers::Below, subjectLength, Address(regExp, RegExp::offsetOfMinimumSize()));
    Jump hasNoCode = branchIfNotCompiled(jit, regExp);
    loadMatchingCode(jit, registers, slowCases);
    callMatchingCode(jit, registers, operands.all(), std::nullopt);
    Jump didNotMiss = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    isTooShort.link(&jit);
    returnResult(jit, TrustedImm64(answers.miss));

    didNotMiss.link(&jit);
    slowCases.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm64(0)));
    if (answers.match)
        finishMatchAndReturn(jit, form, registers, operands, answers, true);
    Label hasFound = jit.label();
    finishMatchAndReturn(jit, form, registers, operands, answers, false);

    isRope.link(&jit);
    JumpList isNotSubstring;
    loadSubstringCharacters(jit, string, impl, characters, subjectLength, isNotSubstring);
    switch (form) {
    case RegExpMatchResult::Array:
    case RegExpMatchResult::ArrayForStringMatch:
        matchSubstringForArray(jit, registers, operands, answers, isNotSubstring, slowCases, hasFound);
        break;
    case RegExpMatchResult::Boolean:
    case RegExpMatchResult::BoxedBoolean:
    case RegExpMatchResult::IndexForStringSearch:
    case RegExpMatchResult::Parts:
        jit.jump().linkTo(hasCharacters, &jit);
        break;
    }

    isNotSubstring.link(&jit);
    flattenSubject(jit, string, { regExpObject, string }, begin);

    if (form == RegExpMatchResult::IndexForStringSearch)
        slowCases.append(usesLastIndex);
    else {
        usesLastIndex.link(&jit);
        if (form == RegExpMatchResult::ArrayForStringMatch)
            slowCases.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(globalFlag)));
        matchFromLastIndex(jit, registers, operands, answers, slowCases, hasFound);
    }

    hasNoCode.link(&jit);
    matchAtom(jit, registers, operands, answers, slowCases, hasFound);

    slowCases.link(&jit);
    jit.callOperationAndReturn(answers.generic, { });
}

static void resetLastIndexIfGlobal(StubAssembler& jit, GPRReg flags, GPRReg regExpObject)
{
    Jump isNotGlobal = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(globalFlag));
    resetLastIndex(jit, regExpObject);
    isNotGlobal.link(&jit);
}

void generateStringReplaceRegExpMayNotMatch(StubAssembler& jit)
{
    const GPRReg string = jit.clobberOperand(0);
    const GPRReg regExpObject = jit.clobberOperand(1);
    const GPRReg replacement = jit.clobberOperand(2);
    const MatchRegisters registers = allocateMatchRegisters(jit);
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg flags = length;
    const std::array savedOperands { string, regExpObject, replacement };
    const bool takesAnyValue = static_cast<ReplacementOfMatches>(jit.key().form()) == ReplacementOfMatches::AnyValue;
    JumpList slowCases;
    JumpList isNeitherTextNorFunction;

    if (takesAnyValue) {
        isNeitherTextNorFunction.append(jit.branchIfNotCell(replacement));
        Jump isText = jit.branchIfString(replacement);
        Jump isFunction = jit.branchIfType(replacement, jsFunctionTypes);
        isNeitherTextNorFunction.append(jit.branchIfNotType(replacement, slimClosureTypes));
        isText.link(&jit);
        isFunction.link(&jit);
    }

    Label begin = jit.label();
    Jump isRope = loadFlatSubject(jit, registers, string);
    Label hasCharacters = jit.label();

    checkOriginalRegExp(jit, regExpObject, scratch, slowCases);
    jit.load64(Address(regExpObject, RegExpObject::offsetOfLastIndex()), scratch);
    slowCases.append(jit.branchIfNotNumber(scratch));

    loadRegExpAndFlags(jit, registers, regExpObject);
    slowCases.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(stickyFlag)));
    Jump isNotGlobal = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(globalFlag));
    slowCases.append(branchIfReadOnlyLastIndex(jit, regExpObject));
    isNotGlobal.link(&jit);
    Jump hasNoCode = branchIfNotCompiled(jit, regExp);
    loadMatchingCode(jit, registers, slowCases);
    resetLastIndexIfGlobal(jit, flags, regExpObject);
    Jump isTooShort = jit.branch32(CCallHelpers::Below, subjectLength, Address(regExp, RegExp::offsetOfMinimumSize()));
    callMatchingCode(jit, registers, savedOperands, std::nullopt);
    Jump didNotMiss = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    isTooShort.link(&jit);
    Label isMiss = jit.label();
    returnResult(jit, string);

    static_assert(operationArgumentGPR(4) == gprOf(StubRegister::A4));
    const GPRReg startOfFirstMatchPlusOne = jit.reuseScratch(StubRegister::A4);
    didNotMiss.link(&jit);
    slowCases.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm64(0)));
    Label isFound = jit.label();
    jit.add32(TrustedImm32(1), scratch, startOfFirstMatchPlusOne);
    Jump hasArguments = jit.jump();

    isRope.link(&jit);
    JumpList isNotSubstring;
    loadSubstringCharacters(jit, string, impl, characters, subjectLength, isNotSubstring);
    jit.jump().linkTo(hasCharacters, &jit);

    isNotSubstring.link(&jit);
    flattenSubject(jit, string, { string, regExpObject, replacement }, begin);

    hasNoCode.link(&jit);
    JumpList isInNoNarrowText;
    loadAtom(jit, registers, slowCases, slowCases, isInNoNarrowText);
    resetLastIndexIfGlobal(jit, flags, regExpObject);
    searchForAtom(jit, registers, savedOperands);
    jit.branch64(CCallHelpers::Equal, scratch, TrustedImm64(noMatch)).linkTo(isMiss, &jit);
    jit.jump().linkTo(isFound, &jit);

    isInNoNarrowText.link(&jit);
    resetLastIndexIfGlobal(jit, flags, regExpObject);
    jit.jump().linkTo(isMiss, &jit);

    slowCases.link(&jit);
    jit.move(TrustedImm32(0), startOfFirstMatchPlusOne);
    hasArguments.link(&jit);
    Jump isNoText = jit.branchIfNotString(replacement);
    jit.callOperationAndReturn(Entry::operationAOTStringReplaceRegExpString, { });

    isNoText.link(&jit);
    isNeitherTextNorFunction.link(&jit);
    jit.callOperationAndReturn(takesAnyValue ? Entry::operationAOTStringReplaceRegExp : Entry::operationAOTStringReplaceRegExpWithFunction, { });
}

void generateNewRegExpLiteral(StubAssembler& jit)
{
    const GPRReg result = jit.result();
    jit.clobberOperand(0);
    jit.clobberOperand(1);
    jit.clobberOperand(2);
    const GPRReg shared = jit.operand(3);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::RegExpObject);
    static_assert(sizeof(RegExpObject) == 4 * sizeof(uint64_t) && JSObject::butterflyOffset() == sizeof(uint64_t) && RegExpObject::offsetOfRegExpAndFlags() == 2 * sizeof(uint64_t) && RegExpObject::offsetOfLastIndex() == 3 * sizeof(uint64_t));
    JumpList slowCases;
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, shared));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator)), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, result, allocator, scratch, slowCases);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header)), scratch);
    jit.store64(scratch, Address(result));
    jit.store64(TrustedImm32(0), Address(result, JSObject::butterflyOffset()));
    loadRegExp(jit, shared, scratch);
    jit.store64(scratch, Address(result, RegExpObject::offsetOfRegExpAndFlags()));
    resetLastIndex(jit, result);
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewRegExpLiteral, { });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
