/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsRegExp.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRecordTypes.h"
#include "AOTRegExpRecord.h"
#include "AOTRuntimeTable.h"
#include "AOTStubAssembler.h"
#include "AOTStubCatalog.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsRegExpFrames.h"
#include "AOTStubsStrings.h"
#include "AOTVector.h"
#include "JSCJSValueInlines.h"
#include "JSString.h"
#include <array>
#include <bit>
#include <optional>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

struct LiteralMatchRegisters {
    std::optional<GPRReg> regExpObject;
    GPRReg string;
    GPRReg recordNumber;
    GPRReg scratch;
    GPRReg characters;
    GPRReg subjectLength;
};

struct LastIndexUseOfLiteral {
    bool isRead { false };
    bool isReset { false };
    bool isStoredAndPutBack { false };
};

struct MatchFormTraits {
    bool createsCell;
    JSValue miss;
    std::optional<RegExpMatchResult> resultOfStubForAnyRegExp;
    std::optional<Entry> operationForAnyRegExp;
};

static constexpr unsigned literalRecordSizeShift = 5;
static constexpr ptrdiff_t offsetOfLiteralRecordBitFields = OBJECT_OFFSETOF(ImageRegExp, rawFlags) + sizeof(ImageRegExp::rawFlags);
static_assert(sizeof(ImageRegExp) == 1u << literalRecordSizeShift);
static_assert(offsetOfLiteralRecordBitFields + 1 == OBJECT_OFFSETOF(ImageRegExp, rawSpecificPattern));

static uint8_t literalRecordBitFields(const ImageRegExp& record)
{
    return std::bit_cast<std::array<uint8_t, sizeof(ImageRegExp)>>(record)[offsetOfLiteralRecordBitFields];
}

static uint8_t literalRecordFormBits()
{
    static_assert(!static_cast<uint8_t>(RegExpRecordForm::Code) && static_cast<uint8_t>(RegExpRecordForm::NoMetadata) == 3);
    ImageRegExp record { };
    record.rawForm = static_cast<uint8_t>(RegExpRecordForm::NoMetadata);
    return literalRecordBitFields(record);
}

static uint8_t literalRecordNamedGroupsBit()
{
    ImageRegExp record { };
    record.hasNamedGroups = 1;
    return literalRecordBitFields(record);
}

static uint8_t literalRecordCodeFor8BitSubjectsAloneBit()
{
    ImageRegExp record { };
    record.hasCodeFor8BitSubjectsAlone = 1;
    return literalRecordBitFields(record);
}

StubSignature signatureOfRegExpMatchLiteral(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::A1) });
    if (takesRegExpObject(key))
        signature.addOperand(gprOf(StubRegister::A2));
    signature.immediate = gprOf(stubImmediateRegister);
    return takesRegExpObject(key) ? signature.fillingOperandInFront(1) : signature;
}

static MatchFormTraits traitsOfMatchForm(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
        return { false, jsBoolean(false), RegExpMatchResult::BoxedBoolean, std::nullopt };
    case RegExpMatchForm::Exec:
        return { true, jsNull(), RegExpMatchResult::Array, std::nullopt };
    case RegExpMatchForm::Search:
        return { false, jsNumber(-1), std::nullopt, Entry::operationAOTStringSearchRegExp };
    case RegExpMatchForm::Match:
        return { true, jsNull(), std::nullopt, Entry::operationAOTStringMatchRegExp };
    case RegExpMatchForm::Parts:
        return { false, jsBoolean(false), RegExpMatchResult::Parts, std::nullopt };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LastIndexUseOfLiteral lastIndexUseOfLiteral(RegExpMatchForm form, bool isGlobal, bool usesLastIndex)
{
    const LastIndexUseOfLiteral useOfExec { .isRead = usesLastIndex, .isReset = usesLastIndex };
    switch (form) {
    case RegExpMatchForm::Test:
    case RegExpMatchForm::Exec:
    case RegExpMatchForm::Parts:
        return useOfExec;
    case RegExpMatchForm::Search:
        return { .isStoredAndPutBack = usesLastIndex };
    case RegExpMatchForm::Match:
        if (isGlobal)
            return { .isReset = true };
        return useOfExec;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void matchLiteralOutOfLineAndReturn(StubAssembler& jit, RegExpMatchForm form, std::optional<GPRReg> regExpObject, GPRReg string)
{
    static_assert(operationArgumentGPR(2) == gprOf(StubRegister::A2) && operationArgumentGPR(3) == gprOf(StubRegister::A3));
    if (!regExpObject) {
        RELEASE_ASSERT(string == operationArgumentGPR(1));
        jit.move(jit.immediate(), jit.reuseScratch(StubRegister::A2));
        jit.move(TrustedImm32(static_cast<int32_t>(form)), jit.reuseScratch(StubRegister::A3));
        jit.callOperationAndReturn(Entry::operationAOTRegExpMatchLiteralWithoutObject, { });
        return;
    }

    const MatchFormTraits traits = traitsOfMatchForm(form);
    if (traits.resultOfStubForAnyRegExp) {
        checkStubOperands(regExpMatch(*traits.resultOfStubForAnyRegExp), { *regExpObject, string });
        jit.jumpToStub(regExpMatch(*traits.resultOfStubForAnyRegExp));
        return;
    }
    RELEASE_ASSERT(*regExpObject == operationArgumentGPR(1) && string == operationArgumentGPR(2));
    jit.swap(*regExpObject, string);
    jit.callOperationAndReturn(*traits.operationForAnyRegExp, { });
}

static void loadRecordOfLiteral(StubAssembler& jit, GPRReg recordNumber, GPRReg record)
{
    jit.lshift32(recordNumber, TrustedImm32(literalRecordSizeShift), record);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.regExps)), record);
}

static void loadMatchingCodeOfLiteral(StubAssembler& jit, GPRReg impl, GPRReg record, JumpList& is16Bit, JumpList& hasNoCode)
{
    const GPRReg code = record;
    hasNoCode.append(jit.branchTest8(CCallHelpers::NonZero, Address(record, offsetOfLiteralRecordBitFields), TrustedImm32(literalRecordFormBits())));
    is16Bit.append(jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(record, OBJECT_OFFSETOF(ImageRegExp, codeFor8Bit)), code);
}

static void loadMatchingCodeOfLiteralFor16Bit(StubAssembler& jit, GPRReg record, JumpList& hasNoCode)
{
    const GPRReg code = record;
    hasNoCode.append(jit.branchTest8(CCallHelpers::NonZero, Address(record, offsetOfLiteralRecordBitFields), TrustedImm32(literalRecordCodeFor8BitSubjectsAloneBit())));
    jit.load32(Address(record, OBJECT_OFFSETOF(ImageRegExp, codeFor16Bit)), code);
}

static std::optional<GPRReg> loadStartOfLiteralMatch(StubAssembler& jit, const LiteralMatchRegisters& registers, LastIndexUseOfLiteral lastIndexUse, JumpList& outOfLine)
{
    if (!lastIndexUse.isRead)
        return std::nullopt;
    const GPRReg start = jit.allocateScratch(StubRegister::A3);
    jit.load64(Address(*registers.regExpObject, RegExpObject::offsetOfLastIndex()), start);
    outOfLine.append(jit.branchIfNotInt32(start));
    outOfLine.append(jit.branch32(CCallHelpers::LessThan, start, TrustedImm32(0)));
    jit.zeroExtend32ToWord(start, start);
    return start;
}

static void matchLiteral(StubAssembler& jit, const LiteralMatchRegisters& registers, std::optional<GPRReg> start)
{
    Vector<GPRReg, maxSavedOperandsOfMatch> savedOperands;
    if (registers.regExpObject)
        savedOperands.append(*registers.regExpObject);
    savedOperands.append(registers.string);
    savedOperands.append(registers.recordNumber);
    RELEASE_ASSERT(registers.characters == gprOf(StubRegister::A0));
    callMatchingCodeInFrame(jit, savedOperands.span(), registers.subjectLength, start, registers.scratch, [&](GPRReg output) {
        loadMatchOutput(jit, output);
    }, [&] {
        jit.call(registers.scratch, Yarr8BitPtrTag);
    });
}

static void finishLiteralMatchWithoutCellAndReturn(StubAssembler& jit, RegExpMatchForm form, const LiteralMatchRegisters& registers, bool advancesLastIndex, bool recordsMatch)
{
    const GPRReg result = jit.result();
    const GPRReg offsets = registers.subjectLength;
    const GPRReg regExp = registers.characters;
    if (advancesLastIndex || recordsMatch)
        loadMatchOutput(jit, offsets);
    if (advancesLastIndex)
        storeEndOfMatchInLastIndex(jit, *registers.regExpObject, offsets, regExp);
    if (recordsMatch) {
        loadRegExp(jit, *registers.regExpObject, regExp);
        recordMatch(jit, { .regExp = regExp, .string = registers.string, .offsets = offsets, .globalObject = jit.reuseScratch(StubRegister::T10), .scratch = registers.scratch });
    }
    if (form == RegExpMatchForm::Test || form == RegExpMatchForm::Parts)
        jit.move(TrustedImm64(JSValue::encode(jsBoolean(true))), result);
    else if (recordsMatch) {
        loadMatchOutput(jit, result);
        jit.load32(matchOutputEntry(result, 0), result);
        boxInt32(jit, result, result);
    } else
        boxInt32(jit, registers.scratch, result);
    jit.returnToCaller();
}

static void finishLiteralMatchWithObjectAndReturn(StubAssembler& jit, RegExpMatchForm form, bool isGlobal, const LiteralMatchRegisters& registers)
{
    static_assert(operationArgumentGPR(3) == gprOf(StubRegister::A3) && operationArgumentGPR(4) == gprOf(StubRegister::A4));
    if (form == RegExpMatchForm::Exec || !isGlobal) {
        jit.loadPtr(Address(registers.string, JSString::offsetOfValue()), registers.scratch);
        Jump isRope = jit.branchIfRopeStringImpl(registers.scratch);
        checkStubOperands(finishRegExpExec(FinishRegExpExecForm::WithObject), { *registers.regExpObject, registers.string });
        jit.jumpToStub(finishRegExpExec(FinishRegExpExecForm::WithObject));
        isRope.link(&jit);
        checkStubOperands(finishRegExpExec(FinishRegExpExecForm::OnSubstringWithObject), { *registers.regExpObject, registers.string });
        jit.jumpToStub(finishRegExpExec(FinishRegExpExecForm::OnSubstringWithObject));
        return;
    }
    RELEASE_ASSERT(*registers.regExpObject == operationArgumentGPR(1) && registers.string == operationArgumentGPR(2));
    jit.move(registers.recordNumber, jit.reuseScratch(StubRegister::A3));
    jit.move(TrustedImm32(static_cast<int32_t>(form)), jit.reuseScratch(StubRegister::A4));
    jit.callOperationAndReturn(Entry::operationAOTRegExpMatchLiteral, { });
}

static void finishLiteralMatchAndReturn(StubAssembler& jit, RegExpMatchForm form, const LiteralMatchRegisters& registers)
{
    static_assert(operationArgumentGPR(3) == gprOf(StubRegister::A3) && operationArgumentGPR(4) == gprOf(StubRegister::A4));
    const GPRReg objectOrRecordNumber = jit.reuseScratch(StubRegister::A1);
    const GPRReg string = jit.reuseScratch(StubRegister::A2);
    const GPRReg record = registers.scratch;
    const GPRReg recordNumber = registers.recordNumber;
    jit.move(registers.string, string);
    loadRecordOfLiteral(jit, recordNumber, record);
    Jump hasNamedGroups = jit.branchTest8(CCallHelpers::NonZero, Address(record, offsetOfLiteralRecordBitFields), TrustedImm32(literalRecordNamedGroupsBit()));
    const GPRReg numberOfGroups = jit.reuseScratch(stubImmediateRegister);
    jit.move(recordNumber, objectOrRecordNumber);
    jit.load32(Address(record, OBJECT_OFFSETOF(ImageRegExp, numSubpatterns)), numberOfGroups);
    checkStubOperands(finishRegExpExec(FinishRegExpExecForm::WithoutObject), { objectOrRecordNumber, string, numberOfGroups });
    jit.jumpToStub(finishRegExpExec(FinishRegExpExecForm::WithoutObject));

    hasNamedGroups.link(&jit);
    jit.move(TrustedImm32(0), objectOrRecordNumber);
    jit.move(recordNumber, jit.reuseScratch(StubRegister::A3));
    jit.move(TrustedImm32(static_cast<int32_t>(form)), jit.reuseScratch(StubRegister::A4));
    jit.callOperationAndReturn(Entry::operationAOTRegExpMatchLiteral, { });
}

void generateRegExpMatchLiteral(StubAssembler& jit)
{
    static_assert(noMatch == -1);
    static_assert(areDistinctOnEveryISA(StubRegister::A0, StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A4, stubImmediateRegister, StubRegister::T11, StubRegister::T13), "the arguments of the matching code overwrite neither its address nor what the match frame saves");
    const RegExpMatchForm form = matchFormOf(jit.key());
    const bool takesObject = takesRegExpObject(jit.key());
    const bool isGlobal = matchesGlobalLiteral(jit.key());
    const std::optional<GPRReg> regExpObject = takesObject ? std::optional<GPRReg>(jit.clobberOperand(0)) : std::nullopt;
    const GPRReg string = jit.clobberOperand(takesObject ? 1 : 0);
    const bool usesLastIndex = takesObject && isGlobal;
    const LastIndexUseOfLiteral lastIndexUse = lastIndexUseOfLiteral(form, isGlobal, usesLastIndex);
    const GPRReg recordNumber = jit.immediate();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    const GPRReg impl = jit.allocateScratch(StubRegister::T10);
    const GPRReg characters = jit.allocateScratch(StubRegister::A0);
    const GPRReg subjectLength = jit.allocateScratch(StubRegister::T11);
    const LiteralMatchRegisters registers { regExpObject, string, recordNumber, scratch, characters, subjectLength };
    JumpList miss;
    JumpList outOfLine;

    jit.loadPtr(Address(string, JSString::offsetOfValue()), impl);
    Jump isRope = jit.branchIfRopeStringImpl(impl);
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), characters);
    jit.load32(Address(impl, StringImpl::lengthMemoryOffset()), subjectLength);
    Label hasCharacters = jit.label();

    if (lastIndexUse.isReset || lastIndexUse.isStoredAndPutBack)
        outOfLine.append(branchIfReadOnlyLastIndex(jit, *regExpObject));

    const std::optional<GPRReg> start = loadStartOfLiteralMatch(jit, registers, lastIndexUse, outOfLine);
    if (start)
        miss.append(jit.branch64(CCallHelpers::LessThan, subjectLength, *start));

    loadRecordOfLiteral(jit, recordNumber, scratch);
    if (takesObject && !usesLastIndex)
        outOfLine.append(jit.branchTest16(CCallHelpers::NonZero, Address(scratch, OBJECT_OFFSETOF(ImageRegExp, rawFlags)), TrustedImm32(stickyFlag)));
    JumpList is16Bit;
    loadMatchingCodeOfLiteral(jit, impl, scratch, is16Bit, outOfLine);
    Label hasCode = jit.label();
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.regExpCode)), scratch);
    matchLiteral(jit, registers, start);
    Label isSearched = jit.label();
    miss.append(jit.branch64(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(noMatch))));
    outOfLine.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm32(0)));
    if (!traitsOfMatchForm(form).createsCell)
        finishLiteralMatchWithoutCellAndReturn(jit, form, registers, (form == RegExpMatchForm::Test || form == RegExpMatchForm::Parts) && usesLastIndex, recordsMatchOfLiteral(jit.key()));
    else if (takesObject)
        finishLiteralMatchWithObjectAndReturn(jit, form, isGlobal, registers);
    else
        finishLiteralMatchAndReturn(jit, form, registers);

    miss.link(&jit);
    if (lastIndexUse.isReset)
        resetLastIndex(jit, *regExpObject);
    returnResult(jit, TrustedImm64(JSValue::encode(traitsOfMatchForm(form).miss)));

    isRope.link(&jit);
    loadSubstringCharacters(jit, string, impl, characters, subjectLength, outOfLine);
    jit.jump().linkTo(hasCharacters, &jit);

    is16Bit.link(&jit);
    Jump isUnderUnicode = jit.branchTest16(CCallHelpers::NonZero, Address(scratch, OBJECT_OFFSETOF(ImageRegExp, rawFlags)), TrustedImm32(unicodeFlags));
    loadMatchingCodeOfLiteralFor16Bit(jit, scratch, outOfLine);
    jit.jump().linkTo(hasCode, &jit);

    if (start)
        outOfLine.append(isUnderUnicode);
    else {
        isUnderUnicode.link(&jit);
        loadMatchingCodeOfLiteralFor16Bit(jit, scratch, outOfLine);
        jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.regExpCode)), scratch);
        matchLiteral(jit, registers, std::nullopt);
        rejectMatchInsidePair(jit, { .answer = scratch, .string = string, .impl = impl, .characters = characters, .length = subjectLength }, isSearched, outOfLine);
    }

    outOfLine.link(&jit);
    matchLiteralOutOfLineAndReturn(jit, form, regExpObject, string);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
