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

// Whoever creates the array of a match reads the VM's. Code that asks for Found reads the RegExp's, where operationAOTRegExpFind leaves the offsets too.
enum class OffsetVectorOwner : bool { VM, RegExp };

struct MatchAnswers {
    Entry generic;
    // Without one the answer is the array of the match.
    std::optional<int64_t> match;
    int64_t miss;
    OffsetVectorOwner offsetVectorOwner;
};

StubSignature signatureOfRegExpMatch(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

StubSignature signatureOfFinishRegExpExec(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
    if (static_cast<FinishRegExpExecForm>(key.form()) == FinishRegExpExecForm::WithoutObject)
        signature.addOperand(gprOf(StubRegister::T9));
    return signature;
}

StubSignature signatureOfStringMatchRegExp(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

StubSignature signatureOfStringReplaceRegExpMayNotMatch(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) });
}

StubSignature signatureOfNewRegExpLiteral(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3), gprOf(StubRegister::T9) });
}

static MatchAnswers answersOf(RegExpMatchResult result)
{
    switch (result) {
    case RegExpMatchResult::Array:
        return { Entry::operationAOTRegExpExec, std::nullopt, JSValue::encode(jsNull()), OffsetVectorOwner::VM };
    case RegExpMatchResult::Boolean:
        return { Entry::operationAOTRegExpTest, 1, 0, OffsetVectorOwner::VM };
    case RegExpMatchResult::Found:
        return { Entry::operationAOTRegExpFind, 1, 0, OffsetVectorOwner::RegExp };
    case RegExpMatchResult::BoxedBoolean:
        return { Entry::operationAOTRegExpTestBoxed, JSValue::encode(jsBoolean(true)), JSValue::encode(jsBoolean(false)), OffsetVectorOwner::VM };
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

static void loadOffsetVector(StubAssembler& jit, OffsetVectorOwner owner, GPRReg regExp, GPRReg output)
{
    switch (owner) {
    case OffsetVectorOwner::VM:
        loadMatchOutput(jit, output);
        return;
    case OffsetVectorOwner::RegExp:
        jit.loadPtr(Address(regExp, RegExp::offsetOfOvector() + FixedVector<int>::offsetOfStorage()), output);
        jit.addPtr(TrustedImm32(FixedVector<int>::Storage::offsetOfData()), output);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
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

static void callMatchingCode(StubAssembler& jit, const MatchRegisters& registers, std::span<const GPRReg> savedOperands, std::optional<GPRReg> start, OffsetVectorOwner owner)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg startArgument = jit.reuseScratch(StubRegister::A1);
    const GPRReg lengthArgument = jit.reuseScratch(StubRegister::A2);
    const GPRReg output = jit.reuseScratch(StubRegister::A3);
    const GPRReg context = jit.reuseScratch(StubRegister::A4);
    ASSERT(characters == gprOf(StubRegister::A0));
    pushMatchFrame(jit, savedOperands);
    jit.move(subjectLength, lengthArgument);
    // start may share its register with output.
    if (start)
        jit.move(*start, startArgument);
    else
        jit.move(TrustedImm32(0), startArgument);
    loadOffsetVector(jit, owner, regExp, output);
    storeMatchingContext(jit, context);
    jit.move(CCallHelpers::stackPointerRegister, context);
    jit.call(scratch, Yarr8BitPtrTag);
    popMatchFrame(jit, scratch, savedOperands);
}

static void loadAtom(StubAssembler& jit, const MatchRegisters& registers, JumpList& hasNoAtom, JumpList& slowCases)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg atom = scratch;
    jit.loadPtr(Address(regExp, RegExp::offsetOfAtom()), atom);
    hasNoAtom.append(jit.branchTestPtr(CCallHelpers::Zero, atom));
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(atom, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
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

static void returnMiss(StubAssembler& jit, const MatchAnswers& answers)
{
    jit.move(TrustedImm64(answers.miss), jit.result());
    jit.returnToCaller();
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
    callMatchingCode(jit, registers, operands.all(), start, answers.offsetVectorOwner);
    Jump didNotMiss = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    restIsTooShort.link(&jit);
    resetLastIndex(jit, regExpObject);
    returnMiss(jit, answers);

    didNotMiss.link(&jit);
    slowCases.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm64(0)));
    jit.jump().linkTo(hasFound, &jit);
}

// A pattern made at run time has no code. Most of them run on a flat program, which needs nothing of the VM.
static void matchOnFlatProgram(StubAssembler& jit, const MatchRegisters& registers, const MatchOperands& operands, const MatchAnswers& answers, JumpList& slowCases, Label hasFound)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg program = scratch;
    const GPRReg programArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg charactersArgument = jit.reuseScratch(StubRegister::A1);
    const GPRReg lengthArgument = jit.reuseScratch(StubRegister::A2);
    const GPRReg output = jit.reuseScratch(StubRegister::A3);
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1) && leafArgumentGPR(2) == gprOf(StubRegister::A2) && leafArgumentGPR(3) == gprOf(StubRegister::A3));
    static_assert(sizeof(std::unique_ptr<Yarr::BytecodePattern>) == sizeof(void*) && sizeof(std::unique_ptr<Yarr::FlatProgram>) == sizeof(void*));
    ASSERT(characters == programArgument);

    slowCases.append(jit.branch8(CCallHelpers::NotEqual, Address(regExp, RegExp::offsetOfState()), TrustedImm32(RegExp::stateWithBytecode())));
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    if (answers.offsetVectorOwner == OffsetVectorOwner::VM)
        slowCases.append(jit.branch32(CCallHelpers::Above, Address(regExp, RegExp::offsetOfNumSubpatterns()), TrustedImm32(VMState::maxGroupsInRegExpMatchOutput)));
    const GPRReg bytecode = length;
    jit.loadPtr(Address(regExp, RegExp::offsetOfRegExpBytecode()), bytecode);
    jit.loadPtr(Address(bytecode, Yarr::BytecodePattern::offsetOfFlatProgram()), program);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, program));
    bumpRuntimeCounter(jit, RuntimeCounterID::RegExpMatchesInterpreted, length);
    Jump isTooShort = jit.branch32(CCallHelpers::Below, subjectLength, Address(regExp, RegExp::offsetOfMinimumSize()));

    pushMatchFrame(jit, operands.all());
    jit.move(subjectLength, lengthArgument);
    jit.move(characters, charactersArgument);
    loadOffsetVector(jit, answers.offsetVectorOwner, regExp, output);
    jit.move(program, programArgument);
    jit.callLeaf(Entry::operationAOTRegExpMatchFlatProgram8, bytesPushedInMatchFrame);
    popMatchFrame(jit, scratch, operands.all());
    Jump didNotMiss = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    isTooShort.link(&jit);
    returnMiss(jit, answers);

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
    Jump hasNoCode = branchIfNotCompiled(jit, regExp);
    loadMatchingCode(jit, registers, slowCases);
    Jump isTooShort = jit.branch32(CCallHelpers::Below, subjectLength, Address(regExp, RegExp::offsetOfMinimumSize()));
    callMatchingCode(jit, registers, operands.all(), std::nullopt, answers.offsetVectorOwner);
    Label isSearched = jit.label();
    Jump didNotMiss = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    isTooShort.link(&jit);
    Label isMiss = jit.label();
    returnMiss(jit, answers);

    // Whoever creates the array takes a string that is no rope. The offsets of the match are the same in the flat string.
    didNotMiss.link(&jit);
    slowCases.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm64(0)));
    flattenSubject(jit, operands.string, { operands.regExpObject, operands.string }, hasFound);

    hasNoCode.link(&jit);
    JumpList hasNoAtom;
    loadAtom(jit, registers, hasNoAtom, slowCases);
    searchForAtom(jit, registers, operands.all());
    jit.branch64(CCallHelpers::Equal, scratch, TrustedImm64(noMatch)).linkTo(isMiss, &jit);
    // The search leaves no offsets.
    needsFlatSubject.append(jit.jump());

    hasNoAtom.link(&jit);
    matchOnFlatProgram(jit, registers, operands, answers, slowCases, isSearched);
}

static void matchAtom(StubAssembler& jit, const MatchRegisters& registers, const MatchOperands& operands, const MatchAnswers& answers, JumpList& slowCases, Label hasFound)
{
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg output = regExp;
    JumpList hasNoAtom;
    loadAtom(jit, registers, hasNoAtom, slowCases);
    searchForAtom(jit, registers, operands.all());
    Jump isFound = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    returnMiss(jit, answers);

    isFound.link(&jit);
    loadRegExp(jit, operands.regExpObject, regExp);
    jit.loadPtr(Address(regExp, RegExp::offsetOfAtom()), length);
    jit.load32(Address(length, StringImpl::lengthMemoryOffset()), length);
    jit.add32(scratch, length);
    loadOffsetVector(jit, answers.offsetVectorOwner, regExp, output);
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

struct MatchEndRegisters {
    GPRReg regExp;
    GPRReg flags;
    GPRReg offsets;
    GPRReg globalObject;
    GPRReg scratch;
};

static void advanceLastIndexAndRecordMatch(StubAssembler& jit, const MatchOperands& operands, const MatchEndRegisters& registers, OffsetVectorOwner owner)
{
    const auto [regExp, flags, offsets, globalObject, scratch] = registers;
    loadRegExp(jit, operands.regExpObject, regExp);
    jit.load16(Address(regExp, RegExp::offsetOfFlags()), flags);
    loadOffsetVector(jit, owner, regExp, offsets);
    // Whoever starts at lastIndex has seen that it is writable.
    Jump keepsLastIndex = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(globalFlag | stickyFlag));
    storeEndOfMatchInLastIndex(jit, operands.regExpObject, offsets, scratch);
    keepsLastIndex.link(&jit);
    recordMatch(jit, { .regExp = regExp, .string = operands.string, .offsets = offsets, .globalObject = globalObject, .scratch = scratch });
}

static void finishMatchAndReturn(StubAssembler& jit, const MatchRegisters& registers, const MatchOperands& operands, const MatchAnswers& answers)
{
    if (!answers.match) {
        RELEASE_ASSERT(answers.offsetVectorOwner == OffsetVectorOwner::VM);
        checkStubOperands(finishRegExpExec(FinishRegExpExecForm::WithObject), { operands.regExpObject, operands.string });
        jit.jumpToStub(finishRegExpExec(FinishRegExpExecForm::WithObject));
        return;
    }
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    advanceLastIndexAndRecordMatch(jit, operands, { .regExp = regExp, .flags = length, .offsets = characters, .globalObject = impl, .scratch = scratch }, answers.offsetVectorOwner);
    jit.move(TrustedImm64(*answers.match), jit.result());
    jit.returnToCaller();
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

// start arrives with the number of groups.
[[nodiscard]] static AssemblyHelpers::FreeListRefill allocateMatchArrayStorage(StubAssembler& jit, const MatchArrayRegisters& registers, JumpList& fails)
{
    static_assert(MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
    constexpr unsigned wordsBesideGroups = offsetOfButterflyInMatchArrayStorage / sizeof(EncodedJSValue) + 1;
    const GPRReg numberOfElements = registers.start;
    const GPRReg butterfly = registers.butterfly;
    const GPRReg allocator = registers.allocator;
    const GPRReg scratch = registers.array;

    jit.add32(TrustedImm32(wordsBesideGroups + 1), registers.start, scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, scratch, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    fails.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, butterfly, allocator, scratch, fails);
    jit.addPtr(TrustedImm32(offsetOfButterflyInMatchArrayStorage), butterfly);
    jit.add32(TrustedImm32(1), numberOfElements);
    jit.store32(numberOfElements, Address(butterfly, Butterfly::offsetOfPublicLength()));
    jit.store32(numberOfElements, Address(butterfly, Butterfly::offsetOfVectorLength()));
    return refill;
}

static void storeMatchArrayProperties(StubAssembler& jit, const MatchArrayRegisters& registers)
{
    const GPRReg butterfly = registers.butterfly;
    const GPRReg index = registers.array;
    jit.load32(matchOutputEntry(registers.offsets, 0), index);
    boxInt32(jit, index, index);
    jit.store64(index, matchArrayProperty(butterfly, RegExpMatchesArrayIndexPropertyOffset));
    jit.storePtr(registers.string, matchArrayProperty(butterfly, RegExpMatchesArrayInputPropertyOffset));
    jit.store64(TrustedImm32(JSValue::ValueUndefined), matchArrayProperty(butterfly, RegExpMatchesArrayGroupsPropertyOffset));
    for (PropertyOffset unused = RegExpMatchesArrayGroupsPropertyOffset + 1; unused < endOfMatchArrayProperties; ++unused)
        jit.store64(TrustedImm32(0), matchArrayProperty(butterfly, unused));
}

[[nodiscard]] static AssemblyHelpers::FreeListRefill storeMatchArrayElements(StubAssembler& jit, const MatchArrayRegisters& registers, JumpList& fails)
{
    const auto [string, butterfly, offsets, index, piece, start, length, allocator, array] = registers;
    JumpList tookNoPart;
    jit.move(TrustedImm32(0), index);
    Label next = jit.label();
    jit.load32(BaseIndex(offsets, index, CCallHelpers::TimesEight), start);
    jit.load32(BaseIndex(offsets, index, CCallHelpers::TimesEight, sizeof(int)), length);
    jit.move(TrustedImm32(JSValue::ValueUndefined), piece);
    tookNoPart.append(jit.branch32(CCallHelpers::LessThan, start, TrustedImm32(0)));
    tookNoPart.append(jit.branchSub32(CCallHelpers::Signed, start, length));
    AssemblyHelpers::FreeListRefill refill = makeSubstringOfFlatString(jit, { .string = string, .start = start, .length = length, .result = piece, .firstScratch = allocator, .secondScratch = array }, fails);
    tookNoPart.link(&jit);
    jit.store64(piece, BaseIndex(butterfly, index, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::Below, index, Address(butterfly, Butterfly::offsetOfPublicLength())).linkTo(next, &jit);
    return refill;
}

// Nothing here calls anything, so the collector sees either the whole array or none of its parts. Leaves 0 in array when an allocator has no room.
static void tryCreateMatchArray(StubAssembler& jit, const MatchArrayRegisters& registers)
{
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::RegExpMatchesArray);
    const GPRReg array = registers.array;
    const GPRReg header = registers.piece;
    JumpList fails;

    AssemblyHelpers::FreeListRefill storageRefill = allocateMatchArrayStorage(jit, registers, fails);
    loadMatchOutput(jit, registers.offsets);
    storeMatchArrayProperties(jit, registers);
    AssemblyHelpers::FreeListRefill pieceRefill = storeMatchArrayElements(jit, registers, fails);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator)), registers.allocator);
    AssemblyHelpers::FreeListRefill arrayRefill = allocateCellWithLateRefill(jit, array, registers.allocator, header, fails);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header)), header);
    jit.storePair64(header, registers.butterfly, array, TrustedImm32(0));
    fenceAfterAllocation(jit);
    Jump isCreated = jit.jump();

    emitLateRefill(jit, storageRefill, fails);
    emitLateRefill(jit, pieceRefill, fails);
    emitLateRefill(jit, arrayRefill, fails);
    fails.link(&jit);
    jit.move(TrustedImm32(0), array);
    isCreated.link(&jit);
}

// Leaves the number of groups in numberOfGroups.
static void branchUnlessMatchArrayIsPlain(StubAssembler& jit, GPRReg regExpObject, GPRReg regExp, GPRReg numberOfGroups, JumpList& isNotPlain)
{
    loadRegExp(jit, regExpObject, regExp);
    isNotPlain.append(jit.branchTest16(CCallHelpers::NonZero, Address(regExp, RegExp::offsetOfFlags()), TrustedImm32(hasIndicesFlag)));
    // Only a RegExp with rare data has named groups.
    isNotPlain.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(regExp, RegExp::offsetOfRareData())));
    jit.load32(Address(regExp, RegExp::offsetOfNumSubpatterns()), numberOfGroups);
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
    const GPRReg butterfly = gprOf(StubRegister::T13);
    const GPRReg numberOfGroups = scratch;
    JumpList needsOperation;

    if (hasObject)
        branchUnlessMatchArrayIsPlain(jit, objectOrRecord, regExp, numberOfGroups, needsOperation);
    needsOperation.append(jit.branch32(CCallHelpers::Above, numberOfGroups, TrustedImm32(maxGroupsOfMatchArray)));
    needsOperation.append(branchIfHavingABadTime(jit));
    needsOperation.append(jit.branchTestPtr(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator))));

    // A thunk of a literal, which jumps here, is known to lose no more registers than an operation does.
    jit.preserving({ objectOrRecord, butterfly }, [&] {
        tryCreateMatchArray(jit, {
            .string = string,
            .butterfly = jit.reuseScratch(StubRegister::T13),
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
        advanceLastIndexAndRecordMatch(jit, { .regExpObject = objectOrRecord, .string = string }, { .regExp = regExp, .flags = flags, .offsets = jit.result(), .globalObject = globalObject, .scratch = scratch }, OffsetVectorOwner::VM);
    jit.move(array, jit.result());
    jit.returnToCaller();

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

void generateRegExpMatch(StubAssembler& jit)
{
    const auto form = static_cast<RegExpMatchResult>(jit.key().form());
    const MatchAnswers answers = answersOf(form);
    const MatchOperands operands { .regExpObject = jit.clobberOperand(0), .string = jit.clobberOperand(1) };
    const auto [regExpObject, string] = operands;
    const MatchRegisters registers = allocateMatchRegisters(jit);
    const auto [scratch, impl, regExp, characters, subjectLength, length] = registers;
    const GPRReg flags = length;
    JumpList slowCases;

    Label begin = jit.label();
    Jump isRope = loadFlatSubject(jit, registers, string);
    Label hasCharacters = jit.label();
    jit.load64(Address(regExpObject, RegExpObject::offsetOfLastIndex()), scratch);
    slowCases.append(jit.branchIfNotNumber(scratch));
    loadRegExpAndFlags(jit, registers, regExpObject);
    Jump usesLastIndex = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(globalFlag | stickyFlag));
    Jump hasNoCode = branchIfNotCompiled(jit, regExp);
    loadMatchingCode(jit, registers, slowCases);
    Jump isTooShort = jit.branch32(CCallHelpers::Below, subjectLength, Address(regExp, RegExp::offsetOfMinimumSize()));
    callMatchingCode(jit, registers, operands.all(), std::nullopt, answers.offsetVectorOwner);
    Jump didNotMiss = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    isTooShort.link(&jit);
    returnMiss(jit, answers);

    didNotMiss.link(&jit);
    slowCases.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm64(0)));
    Label hasFound = jit.label();
    finishMatchAndReturn(jit, registers, operands, answers);

    isRope.link(&jit);
    JumpList isNotSubstring;
    loadSubstringCharacters(jit, string, impl, characters, subjectLength, isNotSubstring);
    switch (form) {
    case RegExpMatchResult::Array:
        matchSubstringForArray(jit, registers, operands, answers, isNotSubstring, slowCases, hasFound);
        break;
    case RegExpMatchResult::Boolean:
    case RegExpMatchResult::Found:
    case RegExpMatchResult::BoxedBoolean:
        jit.jump().linkTo(hasCharacters, &jit);
        break;
    }

    isNotSubstring.link(&jit);
    flattenSubject(jit, string, { regExpObject, string }, begin);

    usesLastIndex.link(&jit);
    matchFromLastIndex(jit, registers, operands, answers, slowCases, hasFound);

    hasNoCode.link(&jit);
    matchAtom(jit, registers, operands, answers, slowCases, hasFound);

    slowCases.link(&jit);
    jit.callOperationAndReturn(answers.generic, { });
}

static void checkOriginalRegExp(StubAssembler& jit, GPRReg regExpObject, GPRReg scratch, JumpList& slowCases)
{
    jit.load32(Address(regExpObject, JSCell::structureIDOffset()), scratch);
    slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, Instance::offsetOfReceiverStructureID(Receiver::RegExp))));
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
    JumpList slowCases;

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
    callMatchingCode(jit, registers, savedOperands, std::nullopt, OffsetVectorOwner::VM);
    Jump didNotMiss = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm64(noMatch));
    isTooShort.link(&jit);
    Label isMiss = jit.label();
    jit.move(string, jit.result());
    jit.returnToCaller();

    static_assert(operationArgumentGPR(4) == gprOf(StubRegister::A4));
    const GPRReg oneBehindStartOfFirstMatch = jit.reuseScratch(StubRegister::A4);
    didNotMiss.link(&jit);
    slowCases.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm64(0)));
    Label isFound = jit.label();
    jit.add32(TrustedImm32(1), scratch, oneBehindStartOfFirstMatch);
    Jump hasArguments = jit.jump();

    isRope.link(&jit);
    JumpList isNotSubstring;
    loadSubstringCharacters(jit, string, impl, characters, subjectLength, isNotSubstring);
    jit.jump().linkTo(hasCharacters, &jit);

    isNotSubstring.link(&jit);
    flattenSubject(jit, string, { string, regExpObject, replacement }, begin);

    hasNoCode.link(&jit);
    loadAtom(jit, registers, slowCases, slowCases);
    resetLastIndexIfGlobal(jit, flags, regExpObject);
    searchForAtom(jit, registers, savedOperands);
    jit.branch64(CCallHelpers::Equal, scratch, TrustedImm64(noMatch)).linkTo(isMiss, &jit);
    jit.jump().linkTo(isFound, &jit);

    slowCases.link(&jit);
    jit.move(TrustedImm32(0), oneBehindStartOfFirstMatch);
    hasArguments.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringReplaceRegExpString, { });
}

void generateStringMatchRegExp(StubAssembler& jit)
{
    const GPRReg string = jit.clobberOperand(0);
    const GPRReg regExpObject = jit.clobberOperand(1);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    JumpList slowCases;
    checkOriginalRegExp(jit, regExpObject, scratch, slowCases);
    loadRegExp(jit, regExpObject, scratch);
    slowCases.append(jit.branchTest16(CCallHelpers::NonZero, Address(scratch, RegExp::offsetOfFlags()), TrustedImm32(globalFlag)));
    jit.swap(string, regExpObject);
    jit.jumpToStub(regExpMatch(RegExpMatchResult::Array));

    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringMatchRegExp, { });
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
