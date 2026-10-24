/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsRegExp.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRecordTypes.h"
#include "AOTRelocation.h"
#include "AOTRuntimeTable.h"
#include "AOTStubAssembler.h"
#include "AOTStubCatalog.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsRegExpFrames.h"
#include "AOTVector.h"
#include "JSCJSValueInlines.h"
#include "JSString.h"
#include "RegExp.h"
#include "RegExpObject.h"
#include <optional>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

struct LiteralMatchRegisters {
    std::optional<GPRReg> regExpObject;
    GPRReg string;
    GPRReg scratch;
    GPRReg characters;
    GPRReg subjectLength;
};

// The object stays as created, so lastIndex holds a number, and nothing observes the conversion that RegExpBuiltinExec applies to it.
struct LastIndexUse {
    bool isRead { false };
    bool isReset { false };
};

static constexpr uint16_t entryFor8BitSubjects = 0;
static constexpr uint16_t entryFor16BitSubjects = 1;

StubSignature signatureOfRegExpMatchLiteral(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::A1) });
    if (takesRegExpObject(key))
        signature.addOperand(gprOf(StubRegister::A2));
    signature.immediate = gprOf(stubImmediateRegister);
    signature.boundImmediates = StubBoundImmediates::Every;
    return signature;
}

static Entry genericOperationOf(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
        return Entry::operationAOTRegExpTestBoxed;
    case RegExpMatchForm::Exec:
        return Entry::operationAOTRegExpExec;
    case RegExpMatchForm::Search:
        return Entry::operationAOTStringSearchRegExp;
    case RegExpMatchForm::Match:
        return Entry::operationAOTStringMatchRegExp;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<RegExpMatchResult> resultOfStubForAnyRegExp(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
        return RegExpMatchResult::BoxedBoolean;
    case RegExpMatchForm::Exec:
        return RegExpMatchResult::Array;
    case RegExpMatchForm::Search:
    case RegExpMatchForm::Match:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool takesStringFirst(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
    case RegExpMatchForm::Exec:
        return false;
    case RegExpMatchForm::Search:
    case RegExpMatchForm::Match:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue missOf(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
        return jsBoolean(false);
    case RegExpMatchForm::Exec:
    case RegExpMatchForm::Match:
        return jsNull();
    case RegExpMatchForm::Search:
        return jsNumber(-1);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LastIndexUse lastIndexUseOf(RegExpMatchForm form, const RegExpLiteralConstants& constants)
{
    const LastIndexUse useOfExec { .isRead = constants.usesLastIndex, .isReset = constants.usesLastIndex };
    switch (form) {
    case RegExpMatchForm::Test:
    case RegExpMatchForm::Exec:
        return useOfExec;
    case RegExpMatchForm::Search:
        return { };
    case RegExpMatchForm::Match:
        if (constants.isGlobal)
            return { .isReset = true };
        return useOfExec;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void callGenericOperationAndReturn(StubAssembler& jit, RegExpMatchForm form, GPRReg regExpObject, GPRReg string)
{
    RELEASE_ASSERT(regExpObject == operationArgumentGPR(1) && string == operationArgumentGPR(2));
    if (takesStringFirst(form))
        jit.swap(regExpObject, string);
    jit.callOperationAndReturn(genericOperationOf(form), { });
}

static void callOperationWithoutObjectAndReturn(StubAssembler& jit, RegExpMatchForm form, GPRReg string)
{
    static_assert(operationArgumentGPR(2) == gprOf(StubRegister::A2) && operationArgumentGPR(3) == gprOf(StubRegister::A3));
    RELEASE_ASSERT(string == operationArgumentGPR(1));
    const GPRReg record = jit.reuseScratch(StubRegister::A2);
    if (std::optional<uint32_t> boundRecord = jit.key().immediate())
        jit.move(TrustedImm32(static_cast<int32_t>(*boundRecord)), record);
    else
        jit.move(jit.immediate(), record);
    jit.move(TrustedImm32(static_cast<int32_t>(form)), jit.reuseScratch(StubRegister::A3));
    jit.callOperationAndReturn(Entry::operationAOTRegExpMatchLiteralWithoutObject, { });
}

// What comes here most often is a subject that is a rope. The stub for any RegExp makes it flat and runs the code that the RegExp has. What it cannot do either it hands to the generic operation.
static void callMatchingOperationAndReturn(StubAssembler& jit, RegExpMatchForm form, std::optional<GPRReg> regExpObject, GPRReg string)
{
    if (std::optional<RegExpMatchResult> result = regExpObject ? resultOfStubForAnyRegExp(form) : std::nullopt) {
        checkStubOperands(regExpMatch(*result), { *regExpObject, string });
        jit.jumpToStub(regExpMatch(*result));
        return;
    }
    if (regExpObject)
        callGenericOperationAndReturn(jit, form, *regExpObject, string);
    else
        callOperationWithoutObjectAndReturn(jit, form, string);
}

// Returns the register that has the index where the match starts. Without one the match starts at 0.
static std::optional<GPRReg> loadStart(StubAssembler& jit, const LiteralMatchRegisters& registers, LastIndexUse lastIndexUse, JumpList& outOfLine)
{
    if (!lastIndexUse.isRead)
        return std::nullopt;
    const GPRReg lastIndex = registers.scratch;
    const GPRReg start = jit.allocateScratch(StubRegister::A3);
    jit.load64(Address(*registers.regExpObject, RegExpObject::offsetOfLastIndex()), lastIndex);
    outOfLine.append(jit.branchIfNotInt32(lastIndex));
    outOfLine.append(jit.branch32(CCallHelpers::LessThan, lastIndex, TrustedImm32(0)));
    jit.zeroExtend32ToWord(lastIndex, start);
    return start;
}

static void emitMatch(StubAssembler& jit, const LiteralMatchRegisters& registers, std::optional<GPRReg> start, CodeSymbol code)
{
    Vector<GPRReg, 2> savedOperands;
    if (registers.regExpObject)
        savedOperands.append(*registers.regExpObject);
    savedOperands.append(registers.string);
    const GPRReg startArgument = jit.reuseScratch(StubRegister::A1);
    const GPRReg lengthArgument = jit.reuseScratch(StubRegister::A2);
    const GPRReg output = jit.reuseScratch(StubRegister::A3);
    const GPRReg context = jit.reuseScratch(StubRegister::A4);
    RELEASE_ASSERT(registers.characters == gprOf(StubRegister::A0));
    pushMatchFrame(jit, savedOperands.span());
    jit.move(registers.subjectLength, lengthArgument);
    // start shares its register with output.
    if (start)
        jit.move(*start, startArgument);
    else
        jit.move(TrustedImm32(0), startArgument);
    loadMatchOutput(jit, output);
    storeMatchingContext(jit, context);
    jit.move(CCallHelpers::stackPointerRegister, context);
    jit.callCode(code);
    popMatchFrame(jit, registers.scratch, savedOperands.span());
}

static bool createsCell(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
    case RegExpMatchForm::Search:
        return false;
    case RegExpMatchForm::Exec:
    case RegExpMatchForm::Match:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// registers.scratch has the index where the match starts.
static void finishMatchWithoutCellAndReturn(StubAssembler& jit, RegExpMatchForm form, const RegExpLiteralConstants& constants, const LiteralMatchRegisters& registers)
{
    const GPRReg result = jit.result();
    const GPRReg offsets = registers.subjectLength;
    const GPRReg regExp = registers.characters;
    const bool advancesLastIndex = form == RegExpMatchForm::Test && constants.usesLastIndex;
    if (advancesLastIndex || constants.recordsMatch)
        loadMatchOutput(jit, offsets);
    if (advancesLastIndex)
        storeEndOfMatchInLastIndex(jit, *registers.regExpObject, offsets, regExp);
    if (constants.recordsMatch) {
        loadRegExp(jit, *registers.regExpObject, regExp);
        recordMatch(jit, { .regExp = regExp, .string = registers.string, .offsets = offsets, .globalObject = jit.reuseScratch(StubRegister::T10), .scratch = registers.scratch });
    }
    if (form == RegExpMatchForm::Test)
        jit.move(TrustedImm64(JSValue::encode(jsBoolean(true))), result);
    else if (constants.recordsMatch) {
        loadMatchOutput(jit, result);
        jit.load32(matchOutputEntry(result, 0), result);
        boxInt32(jit, result, result);
    } else
        boxInt32(jit, registers.scratch, result);
    jit.returnToCaller();
}

static void finishMatchAndReturn(StubAssembler& jit, RegExpMatchForm form, uint32_t record, const RegExpLiteralConstants& constants, const LiteralMatchRegisters& registers)
{
    if (!createsCell(form)) {
        finishMatchWithoutCellAndReturn(jit, form, constants, registers);
        return;
    }
    // String.prototype.match with a RegExp that is not global answers what exec answers.
    const bool answersLikeExec = form == RegExpMatchForm::Exec || !constants.isGlobal;
    if (registers.regExpObject && answersLikeExec) {
        checkStubOperands(finishRegExpExec(FinishRegExpExecForm::WithObject), { *registers.regExpObject, registers.string });
        jit.jumpToStub(finishRegExpExec(FinishRegExpExecForm::WithObject));
        return;
    }
    static_assert(operationArgumentGPR(3) == gprOf(StubRegister::A3) && operationArgumentGPR(4) == gprOf(StubRegister::A4));
    if (!registers.regExpObject) {
        RELEASE_ASSERT(answersLikeExec);
        const GPRReg objectOrRecord = jit.reuseScratch(StubRegister::A1);
        const GPRReg string = jit.reuseScratch(StubRegister::A2);
        jit.move(registers.string, string);
        if (!constants.hasNamedGroups) {
            jit.move(TrustedImm32(static_cast<int32_t>(record)), objectOrRecord);
            jit.move(TrustedImm32(static_cast<int32_t>(constants.numberOfGroups)), registers.scratch);
            checkStubOperands(finishRegExpExec(FinishRegExpExecForm::WithoutObject), { objectOrRecord, string, registers.scratch });
            jit.jumpToStub(finishRegExpExec(FinishRegExpExecForm::WithoutObject));
            return;
        }
        jit.move(TrustedImm32(0), objectOrRecord);
    }
    jit.move(TrustedImm32(static_cast<int32_t>(record)), jit.reuseScratch(StubRegister::A3));
    jit.move(TrustedImm32(static_cast<int32_t>(form)), jit.reuseScratch(StubRegister::A4));
    jit.callOperationAndReturn(Entry::operationAOTRegExpMatchLiteral, { });
}

void generateRegExpMatchLiteral(StubAssembler& jit)
{
    static_assert(noMatch == -1);
    const RegExpMatchForm form = matchFormOf(jit.key());
    const bool takesObject = takesRegExpObject(jit.key());
    const std::optional<GPRReg> regExpObject = takesObject ? std::optional<GPRReg>(jit.clobberOperand(0)) : std::nullopt;
    const GPRReg string = jit.clobberOperand(takesObject ? 1 : 0);
    if (!jit.key().immediate()) {
        callMatchingOperationAndReturn(jit, form, regExpObject, string);
        return;
    }

    const uint32_t record = *jit.key().immediate();
    const RegExpLiteralConstants constants = jit.bindingPolicy().regExpLiteral(record, matchesGlobalLiteral(jit.key()));
    // What the policy says about a record it does not know asks for the object.
    if (!regExpObject && (constants.usesLastIndex || constants.recordsMatch)) {
        callMatchingOperationAndReturn(jit, form, regExpObject, string);
        return;
    }
    const LastIndexUse lastIndexUse = lastIndexUseOf(form, constants);
    // Only registers that an operation clobbers: the summary of the body without an immediate stands for every bound one.
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    const GPRReg impl = jit.allocateScratch(StubRegister::T10);
    const GPRReg characters = jit.allocateScratch(StubRegister::A0);
    const GPRReg subjectLength = jit.allocateScratch(StubRegister::T11);
    const LiteralMatchRegisters registers { regExpObject, string, scratch, characters, subjectLength };
    JumpList miss;
    JumpList outOfLine;

    jit.loadPtr(Address(string, JSString::offsetOfValue()), impl);
    outOfLine.append(jit.branchIfRopeStringImpl(impl));
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), characters);
    jit.load32(Address(impl, StringImpl::lengthMemoryOffset()), subjectLength);

    if (lastIndexUse.isReset)
        outOfLine.append(branchIfReadOnlyLastIndex(jit, *regExpObject));

    const std::optional<GPRReg> start = loadStart(jit, registers, lastIndexUse, outOfLine);
    if (start) {
        jit.sub64(subjectLength, *start, scratch);
        miss.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm64(static_cast<int64_t>(constants.minimumSize))));
    } else if (constants.minimumSize)
        miss.append(jit.branch32(CCallHelpers::Below, subjectLength, TrustedImm32(static_cast<int32_t>(constants.minimumSize))));

    Jump is16Bit = jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    emitMatch(jit, registers, start, { SymbolSpace::RegExp, record, entryFor8BitSubjects });
    if (constants.matchesAtCodePointBoundaries)
        outOfLine.append(is16Bit);
    else {
        Jump isMatched = jit.jump();
        is16Bit.link(&jit);
        emitMatch(jit, registers, start, { SymbolSpace::RegExp, record, entryFor16BitSubjects });
        isMatched.link(&jit);
    }
    miss.append(jit.branch64(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(noMatch))));
    outOfLine.append(jit.branch64(CCallHelpers::LessThan, scratch, TrustedImm32(0)));
    finishMatchAndReturn(jit, form, record, constants, registers);

    miss.link(&jit);
    if (lastIndexUse.isReset)
        resetLastIndex(jit, *regExpObject);
    jit.move(TrustedImm64(JSValue::encode(missOf(form))), jit.result());
    jit.returnToCaller();

    outOfLine.link(&jit);
    callMatchingOperationAndReturn(jit, form, regExpObject, string);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
