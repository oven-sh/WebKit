/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsValues.h"

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTDoubleStringCache.h"
#include "AOTImageText.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubAssembler.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsField.h"
#include "AOTStubsOperations.h"
#include "AOTStubsStrings.h"
#include "AOTVector.h"
#include "JSBigInt.h"
#include "JSCJSValueInlines.h"
#include "JSCell.h"
#include "JSString.h"
#include "JSType.h"
#include "JSTypeInfo.h"
#include "NumericStrings.h"
#include "StackAlignment.h"
#include "TypeofType.h"
#include <bit>
#include <initializer_list>
#include <ranges>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

using Saves = OperationCall::Saves;

StubSignature signatureOfToBoolean(StubKey key)
{
    return StubSignature::withGPRResult({ key.operandRegister().value_or(gprOf(StubRegister::R0)) });
}

StubSignature signatureOfValueIsTruthy(StubKey key)
{
    return signatureWithFlagsResult({ key.operandRegister().value_or(gprOf(StubRegister::R0)) }, MacroAssembler::NonZero);
}

StubSignature signatureOfLargeValueIsTruthy(StubKey key) { return signatureOfValueIsTruthy(key); }

StubSignature signatureOfIsCallableCell(StubKey key)
{
    return signatureWithFlagsResult({ key.operandRegister().value_or(gprOf(StubRegister::R0)) }, MacroAssembler::Zero);
}

StubSignature signatureOfEqual(StubKey key)
{
    if (static_cast<Equality>(key.form()) == Equality::StrictInFlags)
        return signatureWithFlagsResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1) }, MacroAssembler::Zero);
    return StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1) });
}

void callBinaryOperation(StubAssembler& jit, Entry entry)
{
    ASSERT(jit.operand(0) == gprOf(StubRegister::R0) && jit.operand(1) == gprOf(StubRegister::A1));
    jit.callOperationAndReturn(entry, { .secondArgumentIn = jit.operand(0), .thirdArgumentIn = jit.operand(1) });
}

static void compareTypeOfCellOnFlags(StubAssembler& jit, GPRReg cell, JSType type, [[maybe_unused]] GPRReg scratch)
{
#if CPU(X86_64)
    jit.m_assembler.cmpb_im(type, JSCell::typeInfoTypeOffset(), cell);
#else
    jit.load8(Address(cell, JSCell::typeInfoTypeOffset()), scratch);
    jit.compareOnFlags32(scratch, TrustedImm32(type));
#endif
}

static void generateTruthiness(StubAssembler& jit, bool answersInFlags)
{
    const GPRReg value = jit.operand(0);
    std::optional<OwnedScratchRegister> ownedScratch;
    if (isX86_64())
        ownedScratch.emplace(jit);
    const GPRReg scratch = ownedScratch ? ownedScratch->gpr() : jit.allocateScratch(value == gprOf(StubRegister::T9) ? StubRegister::T10 : StubRegister::T9);

    auto returnConstant = [&](bool constant) {
        if (answersInFlags)
            return jit.returnFlagsConstant(constant);
        jit.move(TrustedImm32(constant), jit.result());
        jit.returnToCaller();
    };
    auto returnIsNotZero = [&](GPRReg word) {
        if (answersInFlags)
            return jit.returnFlagsTest32(word);
        jit.test32(CCallHelpers::NonZero, word, word, jit.result());
        jit.returnToCaller();
    };
    auto answerForNumber = [&] {
        Jump notInt32 = jit.branch64(CCallHelpers::Below, value, numberTag);
        returnIsNotZero(value);

        notInt32.link(&jit);
        static_assert(!(JSValue::NumberTag + JSValue::DoubleEncodeOffset));
        jit.add64(numberTag, value, scratch);
        jit.lshift64(scratch, TrustedImm32(1), scratch);
        jit.sub64(TrustedImm32(1), scratch);
        jit.urshift64(TrustedImm32(53), scratch);
        if (answersInFlags)
            return jit.returnFlags32(CCallHelpers::NotEqual, scratch, TrustedImm32(0x7ff));
        jit.compare32(CCallHelpers::NotEqual, scratch, TrustedImm32(0x7ff), jit.result());
        jit.returnToCaller();
    };
    auto answerForCell = [&] {
        static_assert(ObjectType > StringType && ObjectType > SymbolType && ObjectType > HeapBigIntType);
        compareTypeOfCellOnFlags(jit, value, static_cast<JSType>(ObjectType - 1), scratch);
        Jump notObject = jit.branchOnFlags(CCallHelpers::BelowOrEqual);
        if (answersInFlags)
            jit.returnToCaller();
        else
            returnConstant(true);

        notObject.link(&jit);
        compareTypeOfCellOnFlags(jit, value, StringType, scratch);
        Jump notString = jit.branchOnFlags(CCallHelpers::NotEqual);
        jit.loadPtr(Address(value, JSString::offsetOfValue()), scratch);
        loadStringLength(jit, value, scratch, scratch);
        returnIsNotZero(scratch);

        notString.link(&jit);
        compareTypeOfCellOnFlags(jit, value, HeapBigIntType, scratch);
        Jump notBigInt = jit.branchOnFlags(CCallHelpers::NotEqual);
        jit.load32(Address(value, JSBigInt::offsetOfLength()), scratch);
        returnIsNotZero(scratch);

        notBigInt.link(&jit);
        returnConstant(true);
    };

    static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined);
    static_assert(JSValue::ValueTrue & 1 && !((JSValue::ValueFalse | JSValue::ValueNull | JSValue::ValueUndefined | JSValue::ValueEmpty) & 1));
    Jump isCellOrNumber = jit.branch64(CCallHelpers::Above, value, TrustedImm32(JSValue::ValueUndefined));
    if (answersInFlags) {
        jit.returnFlagsTest32(value, TrustedImm32(1));

        isCellOrNumber.link(&jit);
        jit.bindEntry(jit.key().withFamily(StubFamily::LargeValueIsTruthy));
        Jump notCell = jit.branchIfNotCell(value);
        answerForCell();
        notCell.link(&jit);
        answerForNumber();
    } else {
        jit.and32(TrustedImm32(1), value, jit.result());
        jit.returnToCaller();

        isCellOrNumber.link(&jit);
        Jump isCell = jit.branchIfCell(value);
        answerForNumber();
        isCell.link(&jit);
        answerForCell();
    }
}

void generateIsCallableCell(StubAssembler& jit)
{
    const GPRReg cell = jit.operand(0);
    const GPRReg scratch = jit.allocateScratch(cell == gprOf(StubRegister::T9) ? StubRegister::T10 : StubRegister::T9);

    Jump isNotFunction = branchIfNotFunctionType(jit, cell, scratch);
    jit.returnFlagsConstant(true);

    isNotFunction.link(&jit);
    Jump mayOverrideCallData = jit.branchTest8(CCallHelpers::NonZero, Address(cell, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetCallData));
    jit.returnFlagsConstant(false);

    mayOverrideCallData.link(&jit);
    OperationCall call { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = cell };
    call.valueDestination = scratch;
    jit.callOperationAndReturn(Entry::operationAOTIsCallable, call, [&] {
        jit.compareOnFlags32(scratch, TrustedImm32(1));
    });
}

StubSignature signatureOfTypeOf(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::R0) }); }

void generateTypeOf(StubAssembler& jit)
{
    const GPRReg value = jit.clobberOperand(0);
    RELEASE_ASSERT(value == jit.result());
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    auto returnType = [&](TypeofType type) {
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(typeStrings) + static_cast<ptrdiff_t>(type) * sizeof(JSCell*)), jit.result());
        jit.returnToCaller();
    };

    Jump isCell = jit.branchIfCell(value);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, value, numberTag);
    returnType(TypeofType::Number);

    notNumber.link(&jit);
    Jump notUndefined = jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(JSValue::ValueUndefined));
    returnType(TypeofType::Undefined);
    notUndefined.link(&jit);
    Jump isNull = jit.branch64(CCallHelpers::Equal, value, TrustedImm32(JSValue::ValueNull));
    returnType(TypeofType::Boolean);

    isCell.link(&jit);
    jit.load8(Address(value, JSCell::typeInfoTypeOffset()), scratch);
    Jump notString = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(StringType));
    returnType(TypeofType::String);
    notString.link(&jit);
    Jump isObject = jit.branch32(CCallHelpers::AboveOrEqual, scratch, TrustedImm32(ObjectType));
    Jump isBigInt = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(HeapBigIntType));
    returnType(TypeofType::Symbol);
    isBigInt.link(&jit);
    returnType(TypeofType::BigInt);

    isObject.link(&jit);
    Jump notFunction = branchIfNotFunctionType(jit, value, scratch);
    returnType(TypeofType::Function);
    notFunction.link(&jit);
    Jump asksItsClass = jit.branchTest8(CCallHelpers::NonZero, Address(value, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetCallData | MasqueradesAsUndefined));
    isNull.link(&jit);
    returnType(TypeofType::Object);

    asksItsClass.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTTypeOf, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = value });
}

void generateToBoolean(StubAssembler& jit)
{
    generateTruthiness(jit, false);
}

void generateValueIsTruthy(StubAssembler& jit)
{
    generateTruthiness(jit, true);
}

static JumpList branchIfFunction(StubAssembler& jit, GPRReg cell, GPRReg scratch)
{
    static_assert(functionTypes.last < slimClosureTypes.first);
    JumpList isFunction;
    isFunction.append(jit.branch8(CCallHelpers::GreaterThanOrEqual, Address(cell, offsetOfSlimClosureWord + ClosureWord::topByteShift / 8), TrustedImm32(ClosureWord::minTopByteOfCalleeTaking(ClosureWord::maxNumberOfParameters))));
    jit.load8(Address(cell, JSCell::typeInfoTypeOffset()), scratch);
    jit.sub32(TrustedImm32(functionTypes.first), scratch);
    isFunction.append(jit.branch32(CCallHelpers::BelowOrEqual, scratch, TrustedImm32(functionTypes.last - functionTypes.first)));
    jit.sub32(TrustedImm32(slimClosureTypes.first - functionTypes.first), scratch);
    isFunction.append(jit.branch32(CCallHelpers::BelowOrEqual, scratch, TrustedImm32(slimClosureTypes.last - slimClosureTypes.first)));
    return isFunction;
}

StubSignature signatureOfThrowUnlessCallable(StubKey)
{
    return StubSignature::withoutResult({ calleeGPR }).fillingOperandInFront(0);
}

void generateThrowUnlessCallable(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);

    Jump notCell = jit.branchIfNotCell(value);
    JumpList isFunction = branchIfFunction(jit, value, scratch);
    notCell.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTThrowUnlessCallable, { .returns = OperationCall::Returns::Void, .saves = Saves::CallerSavedRegisters, .secondArgumentIn = value });

    isFunction.link(&jit);
    jit.returnToCaller();
}

static void stopAtFalseProofOfKinds(StubAssembler& jit, GPRReg value, GPRReg kindsWithSource, [[maybe_unused]] GPRReg scratch)
{
    ThrowSiteWord word { };
    word.registerNumber = static_cast<uint32_t>(value);
    word.kind = static_cast<uint32_t>(ThrowKind::ErrorWithMessage);
    word.representation = static_cast<uint32_t>(Typed::Representation::JSValue);
    word.hasWideA = true;
    word.calleeIsReadFromReceiver = true;
    jit.lshift64(TrustedImm32(32), kindsWithSource);
#if CPU(X86_64)
    jit.or64(TrustedImm32(static_cast<int32_t>(std::bit_cast<uint32_t>(word))), kindsWithSource);
#else
    jit.move(TrustedImm32(static_cast<int32_t>(std::bit_cast<uint32_t>(word))), scratch);
    jit.or64(scratch, kindsWithSource);
#endif
    jit.jumpToStub(throwAtSite(ThrowSiteSource::WordInRegister, CallerFrame::HasFrame));
}

StubSignature signatureOfCheckProofOfKinds(StubKey key)
{
    return StubSignature::withoutResult({ key.operandRegister().value_or(gprOf(StubRegister::R0)), throwWordGPR });
}

static void returnIfAmongKinds(StubAssembler& jit, GPRReg value, GPRReg words, GPRReg scratch, JumpList& isNotAmongThem)
{
    namespace Primitive = Typed::Primitive;
    auto returnIfAmongKinds = [&](uint32_t kind) {
        isNotAmongThem.append(jit.branchTest32(CCallHelpers::Zero, words, TrustedImm32(kind)));
        jit.returnToCaller();
    };

    Jump isNotCell = jit.branchIfNotCell(value);
    Jump isAbsent = jit.branchTest64(CCallHelpers::Zero, value);
    compareTypeOfCellOnFlags(jit, value, ObjectType, scratch);
    Jump isNotObject = jit.branchOnFlags(CCallHelpers::Below);
    returnIfAmongKinds(KindsOfProof::everyObject);

    isNotObject.link(&jit);
    static constexpr std::pair<JSType, uint32_t> kindsOfOtherCells[] = {
        { StringType, Primitive::String },
        { SymbolType, Primitive::Symbol },
        { HeapBigIntType, Primitive::BigInt },
    };
    for (auto [type, kind] : kindsOfOtherCells) {
        compareTypeOfCellOnFlags(jit, value, type, scratch);
        Jump isOfOtherType = jit.branchOnFlags(CCallHelpers::NotEqual);
        returnIfAmongKinds(kind);
        isOfOtherType.link(&jit);
    }
    isNotAmongThem.append(jit.jump());

    isAbsent.link(&jit);
    returnIfAmongKinds(Primitive::Absent);

    isNotCell.link(&jit);
    Jump isNotNumber = jit.branchIfNotNumber(value);
    returnIfAmongKinds(KindsOfProof::everyNumber);

    isNotNumber.link(&jit);
    static constexpr std::pair<int32_t, uint32_t> kindsOfOtherWords[] = {
        { JSValue::ValueUndefined, Primitive::Undefined },
        { JSValue::ValueNull, Primitive::Null },
        { JSValue::ValueTrue, Primitive::True },
        { JSValue::ValueFalse, Primitive::False },
    };
    for (auto [word, kind] : kindsOfOtherWords) {
        Jump isOtherWord = jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(word));
        returnIfAmongKinds(kind);
        isOtherWord.link(&jit);
    }
    isNotAmongThem.append(jit.jump());
}

void generateCheckProofOfKinds(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    OwnedScratchRegister words(jit, jit.operand(1));
    const GPRReg scratch = isX86_64() ? InvalidGPRReg : scratchImmediateGPR;
    JumpList proofIsFalse;
    returnIfAmongKinds(jit, value, words, scratch, proofIsFalse);
    proofIsFalse.link(&jit);
    stopAtFalseProofOfKinds(jit, value, words, scratch);
}

StubSignature signatureOfCheckProofOfType(StubKey key)
{
    GPRReg value = key.operandRegister().value_or(gprOf(StubRegister::R0));
    return StubSignature::withoutResult({ value, throwWordGPR, gprOf(value == gprOf(StubRegister::T9) ? StubRegister::T10 : StubRegister::T9) });
}

void generateCheckProofOfType(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg words = jit.operand(1);
    const GPRReg type = jit.operand(2);
    JumpList isNotAmongKinds;
    {
        OwnedScratchRegister owned(jit, words);
        returnIfAmongKinds(jit, value, owned, isX86_64() ? InvalidGPRReg : scratchImmediateGPR, isNotAmongKinds);
    }
    isNotAmongKinds.link(&jit);
    Jump isAbsent = jit.branchTest64(CCallHelpers::Zero, value);

    static_assert(areDistinctOnEveryISA(StubRegister::T9, StubRegister::T10, StubRegister::T11, StubRegister::T12, StubRegister::T13));
    Vector<GPRReg, 4> borrowed { words };
    for (StubRegister candidate : { StubRegister::T9, StubRegister::T10, StubRegister::T11, StubRegister::T12, StubRegister::T13 }) {
        if (gprOf(candidate) != value && gprOf(candidate) != type && borrowed.size() < 4)
            borrowed.append(gprOf(candidate));
    }
    auto giveBack = [&] {
        for (GPRReg reg : borrowed | std::views::reverse)
            jit.popToRestore(reg);
    };
    for (GPRReg reg : borrowed)
        jit.pushToSave(reg);
    JumpList isNotAccepted;
    branchUnlessTypeAccepts(jit, { value, type, borrowed[1], borrowed[2], borrowed[3] }, isNotAccepted, isNotAccepted);
    giveBack();
    jit.returnToCaller();

    isNotAccepted.link(&jit);
    giveBack();
    isAbsent.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStopUnlessValueHasType, { .returns = OperationCall::Returns::Void, .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = words, .thirdArgumentIn = type, .fourthArgumentIn = value });
}

StubSignature signatureOfThrowUnlessForwardedCallCanBeMade(StubKey)
{
    return StubSignature::withoutResult({ calleeGPR, thisGPR, argumentGPR(1) });
}

void generateThrowUnlessForwardedCallCanBeMade(StubAssembler& jit)
{
    const GPRReg callee = jit.operand(0);
    const GPRReg receiver = jit.operand(1);
    const GPRReg list = jit.operand(2);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    RELEASE_ASSERT(noOverlap(callee, receiver, list, scratch, instanceGPR));

    JumpList slowCases;
    slowCases.append(jit.branchIfNotCell(callee));
    slowCases.append(jit.branchIfNotCell(receiver));
    JumpList calleeIsFunction = branchIfFunction(jit, callee, scratch);
    slowCases.append(jit.jump());

    calleeIsFunction.link(&jit);
    JumpList receiverIsFunction = branchIfFunction(jit, receiver, scratch);
    slowCases.append(jit.jump());

    receiverIsFunction.link(&jit);
    JumpList canBeMade;
    Jump listIsNoCell = jit.branchIfNotCell(list);
    canBeMade.append(jit.branchIfObject(list));
    slowCases.append(jit.jump());
    listIsNoCell.link(&jit);
    canBeMade.append(jit.branchIfOther(list, scratch));

    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTThrowUnlessForwardedCallCanBeMade, { .returns = OperationCall::Returns::Void, .saves = Saves::CallerSavedRegisters, .secondArgumentIn = callee, .thirdArgumentIn = receiver, .fourthArgumentIn = list });

    canBeMade.link(&jit);
    jit.returnToCaller();
}

static OperationCall equalityOperationCall(bool isStrict, GPRReg left, GPRReg right, bool checksException)
{
    if (isStrict)
        return { .saves = Saves::CallerSavedRegisters, .checksException = checksException, .thirdArgumentIn = left };
    return { .saves = Saves::CallerSavedRegisters, .checksException = checksException, .secondArgumentIn = left, .thirdArgumentIn = right };
}

static void emitCompareStringImplsAndReturn(StubAssembler& jit, GPRReg leftImpl, GPRReg rightImpl, GPRReg result)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1));
    RELEASE_ASSERT(result == GPRInfo::returnValueGPR);
    const Equality form = static_cast<Equality>(jit.key().form());
    Vector<GPRReg, 8> saved;
    RegisterSet lost = registersClobberedByPreserveMostLeaves();
    lost.add(leafArgumentGPR(0));
    lost.add(leafArgumentGPR(1));
    lost.filter(RegisterSet::allGPRs());
    lost.exclude(RegisterSet::macroClobberedGPRs());
    lost.remove(result);
    for (StubRegister scratch : { StubRegister::T9, StubRegister::T10, StubRegister::T11, StubRegister::T12 })
        lost.remove(gprOf(scratch));
    lost.forEach([&](Reg reg) {
        saved.append(reg.gpr());
    });
    jit.preserving(saved.span(), [&] {
        const GPRReg firstArgument = jit.reuseScratch(StubRegister::A0);
        const GPRReg secondArgument = jit.reuseScratch(StubRegister::A1);
        RELEASE_ASSERT(noOverlap(leftImpl, rightImpl, firstArgument, secondArgument));
        jit.move(leftImpl, firstArgument);
        jit.move(rightImpl, secondArgument);
        jit.callLeaf(Entry::operationAOTStringImplsAreEqual, WTF::roundUpToMultipleOf<stackAlignmentBytes()>(saved.size() * sizeof(CPURegister)));
    });
    if (form == Equality::StrictInFlags)
        return jit.returnFlags32(CCallHelpers::Equal, result, TrustedImm32(1));
    jit.returnToCaller();
}

void generateEqual(StubAssembler& jit)
{
    const Equality form = static_cast<Equality>(jit.key().form());
    const bool isStrict = form != Equality::Loose;
    const bool answersInFlags = form == Equality::StrictInFlags;
    const GPRReg left = jit.operand(0);
    const GPRReg right = jit.operand(1);
    const GPRReg result = answersInFlags ? jit.clobberOperand(0) : jit.result();
    RELEASE_ASSERT(result == left);
    const GPRReg leftValue = jit.allocateScratch(StubRegister::T9);
    const GPRReg rightValue = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg flags = jit.allocateScratch(StubRegister::T12);

    JumpList isTrue;
    JumpList isFalse;
    JumpList needsContent;
    JumpList areBigInts;
    JumpList& kindsDiffer = isStrict ? isFalse : needsContent;

    auto returnConstant = [&](bool answer) {
        if (answersInFlags)
            return jit.returnFlagsConstant(answer);
        jit.move(TrustedImm32(answer), result);
        jit.returnToCaller();
    };
    auto callOperationAndReturn = [&](Entry entry, bool checksException) {
        OperationCall call = equalityOperationCall(isStrict, left, right, checksException);
        if (!answersInFlags)
            return jit.callOperationAndReturn(entry, call);
        call.valueDestination = leftValue;
        jit.callOperationAndReturn(entry, call, [&] {
            jit.compareOnFlags32(leftValue, TrustedImm32(1));
        });
    };

#if CPU(X86_64)
    const bool flagsOfLastTestAreAnswer = answersInFlags;
#else
    const bool flagsOfLastTestAreAnswer = false;
#endif
    JumpList flagsSayFalse;
    JumpList& isFalseWhereTestFoundBits = flagsOfLastTestAreAnswer ? flagsSayFalse : isFalse;

    Jump leftIsNoInt32;
    Jump rightIsNoInt32;
    if (!isStrict) {
        leftIsNoInt32 = jit.branch64(CCallHelpers::Below, left, numberTag);
        rightIsNoInt32 = jit.branch64(CCallHelpers::Below, right, numberTag);
        jit.compare32(CCallHelpers::Equal, left, right, result);
        jit.returnToCaller();
        leftIsNoInt32.link(&jit);
    }
    Jump haveSameBits = jit.branch64(CCallHelpers::Equal, left, right);
    if (!isStrict)
        jit.or64(left, right, leftValue);
    Jump leftIsNotCell;
    Jump areNotBothCells;
    Jump notStrings;
    if (isStrict) {
        static_assert(StringType + 1 == HeapBigIntType && HeapBigIntType == LastValueCompareCellType);
        static_assert(JSValue::ValueNull & JSValue::OtherTag && JSValue::ValueUndefined & JSValue::OtherTag && JSValue::ValueFalse & JSValue::OtherTag && JSValue::ValueTrue & JSValue::OtherTag);
        leftIsNotCell = jit.branchIfNotCell(left);
        isFalse.append(jit.branchTest64(CCallHelpers::Zero, left));
        if (flagsOfLastTestAreAnswer) {
            Jump isComparedByValue = jit.branch8(CCallHelpers::BelowOrEqual, Address(left, JSCell::typeInfoTypeOffset()), TrustedImm32(HeapBigIntType));
            jit.returnToCaller();
            isComparedByValue.link(&jit);
        } else
            isFalse.append(jit.branch8(CCallHelpers::Above, Address(left, JSCell::typeInfoTypeOffset()), TrustedImm32(HeapBigIntType)));
        notStrings = jit.branchOnFlags(CCallHelpers::Equal);
        isFalseWhereTestFoundBits.append(jit.branchIfNotCell(right));
        isFalse.append(jit.branchTest64(CCallHelpers::Zero, right));
        isFalse.append(jit.branch8(CCallHelpers::NotEqual, Address(right, JSCell::typeInfoTypeOffset()), TrustedImm32(StringType)));
    } else {
        areNotBothCells = jit.branchIfNotCell(leftValue);
        isFalse.append(jit.branchTest64(CCallHelpers::Zero, left));
        isFalse.append(jit.branchTest64(CCallHelpers::Zero, right));
        jit.load8(Address(left, JSCell::typeInfoTypeOffset()), leftValue);
        jit.load8(Address(right, JSCell::typeInfoTypeOffset()), rightValue);
        Jump leftIsNoObject = jit.branch32(CCallHelpers::Below, leftValue, TrustedImm32(ObjectType));
        kindsDiffer.append(jit.branch32(CCallHelpers::Below, rightValue, TrustedImm32(ObjectType)));
        returnConstant(false);

        leftIsNoObject.link(&jit);
        kindsDiffer.append(jit.branch32(CCallHelpers::NotEqual, leftValue, rightValue));
        notStrings = jit.branch32(CCallHelpers::NotEqual, leftValue, TrustedImm32(StringType));
    }
    jit.loadPtr(Address(left, JSString::offsetOfValue()), leftValue);
    jit.loadPtr(Address(right, JSString::offsetOfValue()), rightValue);
    jit.or64(leftValue, rightValue, length);
    Jump oneIsRope = jit.branchTestPtr(CCallHelpers::NonZero, length, TrustedImm32(JSString::isRopeInPointer));
    isTrue.append(jit.branchPtr(CCallHelpers::Equal, leftValue, rightValue));
    jit.load32(Address(leftValue, StringImpl::flagsOffset()), flags);
    jit.and32(Address(rightValue, StringImpl::flagsOffset()), flags);
    Jump mayHaveSameText = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(flagsOfUniqueTexts));
    Label returnsFalse = jit.label();
    returnConstant(false);

    haveSameBits.link(&jit);
    Jump isNumber = jit.branchTest64(CCallHelpers::NonZero, left, numberTag);
    if (flagsOfLastTestAreAnswer) {
        flagsSayFalse.link(&jit);
        jit.returnToCaller();
    } else
        returnConstant(true);

    notStrings.link(&jit);
    if (isStrict) {
        isFalse.append(jit.branchIfNotCell(right));
        isFalse.append(jit.branchTest64(CCallHelpers::Zero, right));
        areBigInts.append(jit.branch8(CCallHelpers::Equal, Address(right, JSCell::typeInfoTypeOffset()), TrustedImm32(HeapBigIntType)));
    } else
        areBigInts.append(jit.branch32(CCallHelpers::Equal, leftValue, TrustedImm32(HeapBigIntType)));
    isFalse.append(jit.jump());

    areBigInts.link(&jit);
    jit.load32(Address(left, JSBigInt::offsetOfLength()), leftValue);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, leftValue, Address(right, JSBigInt::offsetOfLength())));
    isTrue.append(jit.branchTest32(CCallHelpers::Zero, leftValue));
    jit.load64(Address(left, JSBigInt::offsetOfData()), rightValue);
    isFalse.append(jit.branch64(CCallHelpers::NotEqual, rightValue, Address(right, JSBigInt::offsetOfData())));
    needsContent.append(jit.branch32(CCallHelpers::NotEqual, leftValue, TrustedImm32(1)));
    jit.load8(Address(left, JSCell::typeInfoFlagsOffset()), leftValue);
    jit.load8(Address(right, JSCell::typeInfoFlagsOffset()), rightValue);
    jit.xor32(rightValue, leftValue);
    isFalse.append(jit.branchTest32(CCallHelpers::NonZero, leftValue, TrustedImm32(TypeInfoPerCellBit)));
    isTrue.append(jit.jump());

    const FPRReg leftNumber = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg rightNumber = jit.allocateScratch(FPRInfo::fpRegT1);
    JumpList areNumbers;
    JumpList oneIsNoNumber;
    if (isStrict) {
        leftIsNotCell.link(&jit);
        jit.or64(left, right, leftValue);
        isFalse.append(jit.branchTest64(CCallHelpers::Zero, leftValue, numberTag));
        Jump leftIsInt32 = jit.branch64(CCallHelpers::AboveOrEqual, left, numberTag);
        Jump rightIsInt32 = jit.branch64(CCallHelpers::AboveOrEqual, right, numberTag);
        isFalse.append(jit.branch64(CCallHelpers::NotEqual, leftValue, TrustedImm64(JSValue::encode(jsDoubleNumber(-0.0)))));
        areNumbers.append(jit.jump());
        leftIsInt32.link(&jit);
        isFalse.append(jit.branch64(CCallHelpers::AboveOrEqual, right, numberTag));
        isFalse.append(jit.branchTest64(CCallHelpers::Zero, right, numberTag));
        areNumbers.append(jit.jump());
        rightIsInt32.link(&jit);
        isFalse.append(jit.branchTest64(CCallHelpers::Zero, left, numberTag));
    } else {
        areNotBothCells.link(&jit);
        isNumber.link(&jit);
        rightIsNoInt32.link(&jit);
        oneIsNoNumber.append(jit.branchTest64(CCallHelpers::Zero, left, numberTag));
        oneIsNoNumber.append(jit.branchTest64(CCallHelpers::Zero, right, numberTag));
    }
    areNumbers.link(&jit);
    Label comparesNumbers = jit.label();
    toDouble(jit, left, length, leftNumber);
    toDouble(jit, right, length, rightNumber);
    Label comparesDoubles = jit.label();
    isTrue.append(jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, leftNumber, rightNumber));
    isFalse.append(jit.jump());

    if (!isStrict) {
        oneIsNoNumber.link(&jit);
        auto branchIfNullish = [&](GPRReg value, GPRReg scratch) {
            jit.move(value, scratch);
            jit.and64(TrustedImm32(~JSValue::UndefinedTag), scratch);
            return jit.branch64(CCallHelpers::Equal, scratch, TrustedImm32(JSValue::ValueNull));
        };
        auto convertNumberOrBoolean = [&](GPRReg value, GPRReg scratch, FPRReg number) {
            Jump isNumber = jit.branchTest64(CCallHelpers::NonZero, value, numberTag);
            jit.move(value, scratch);
            jit.and32(TrustedImm32(1), scratch);
            jit.convertInt32ToDouble(scratch, number);
            Jump isConverted = jit.jump();
            isNumber.link(&jit);
            toDouble(jit, value, length, number);
            isConverted.link(&jit);
        };
        auto branchIfCannotMasquerade = [&](GPRReg cell) {
            return jit.branch8(CCallHelpers::Below, Address(cell, JSCell::typeInfoTypeOffset()), TrustedImm32(ObjectType));
        };
        Jump leftIsCell = jit.branchIfCell(left);
        Jump leftIsNullish = branchIfNullish(left, leftValue);
        needsContent.append(jit.branchIfCell(right));
        isFalse.append(branchIfNullish(right, rightValue));
        convertNumberOrBoolean(left, leftValue, leftNumber);
        convertNumberOrBoolean(right, rightValue, rightNumber);
        jit.jump().linkTo(comparesDoubles, &jit);

        leftIsNullish.link(&jit);
        isTrue.append(branchIfNullish(right, rightValue));
        isFalse.append(jit.branchIfNotCell(right));
        isFalse.append(branchIfCannotMasquerade(right));
        needsContent.append(jit.jump());
        leftIsCell.link(&jit);
        Jump rightIsNullish = branchIfNullish(right, rightValue);
        needsContent.append(jit.jump());
        rightIsNullish.link(&jit);
        isFalse.append(branchIfCannotMasquerade(left));
        needsContent.append(jit.jump());
    }

    mayHaveSameText.link(&jit);
    jit.load32(Address(leftValue, StringImpl::lengthMemoryOffset()), length);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, length, Address(rightValue, StringImpl::lengthMemoryOffset())));
    JumpList needsLeaf;
    branchIfCharactersDiffer(jit, { leftValue, rightValue, length, flags, result }, isFalse, needsLeaf);
    isTrue.append(jit.jump());

    needsLeaf.link(&jit);
    emitCompareStringImplsAndReturn(jit, leftValue, rightValue, result);

    oneIsRope.link(&jit);
    loadStringLength(jit, left, leftValue, length);
    loadStringLength(jit, right, rightValue, flags);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, length, flags));
    JumpList needsOperation;
    branchIfCharactersInPlaceDiffer(jit, { left, right, leftValue, rightValue, length, flags, result }, isFalse, needsOperation);
    isTrue.append(jit.jump());

    needsOperation.link(&jit);
    callOperationAndReturn(Entry::operationAOTStringsAreEqual, false);

    if (isStrict) {
        isNumber.link(&jit);
        isTrue.append(jit.branch64(CCallHelpers::AboveOrEqual, left, numberTag));
        jit.jump().linkTo(comparesNumbers, &jit);
    }
    isTrue.link(&jit);
    returnConstant(true);

    needsContent.link(&jit);
    callOperationAndReturn(isStrict ? Entry::operationAOTCompareStrictEq : Entry::operationAOTCompareEq, true);

    isFalse.linkTo(returnsFalse, &jit);
}

StubSignature signatureOfToNumberAsDouble(StubKey)
{
    static_assert(FPRInfo::fpRegT0 == FPRInfo::returnValueFPR);
    StubSignature signature = StubSignature::withoutResult({ gprOf(StubRegister::A1) });
    signature.result = StubResult::FPR;
    signature.firstResult = FPRInfo::fpRegT0;
    return signature;
}

void generateToNumberAsDouble(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const FPRReg result = jit.resultFPR();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);

    Jump notInt32 = jit.branch64(CCallHelpers::Below, value, numberTag);
    jit.convertInt32ToDouble(value, result);
    jit.returnToCaller();

    notInt32.link(&jit);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, value, numberTag);
    unboxDouble(jit, value, scratch, result);
    jit.returnToCaller();

    notNumber.link(&jit);
    static_assert((JSValue::ValueFalse | 1) == JSValue::ValueTrue);
    jit.move(value, scratch);
    jit.and64(TrustedImm32(~1), scratch);
    Jump notBoolean = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm32(JSValue::ValueFalse));
    jit.move(value, scratch);
    jit.and32(TrustedImm32(1), scratch);
    jit.convertInt32ToDouble(scratch, result);
    jit.returnToCaller();

    notBoolean.link(&jit);
    JumpList isOther;
    JumpList isNaN;
    isNaN.append(jit.branch64(CCallHelpers::Equal, value, TrustedImm32(JSValue::ValueUndefined)));
    Jump isCell = jit.branchIfCell(value);
    jit.moveZeroToDouble(result);
    jit.returnToCaller();

    isCell.link(&jit);
    isOther.append(jit.branchIfNotString(value));
    jit.loadPtr(Address(value, JSString::offsetOfValue()), scratch);
    isOther.append(jit.branchIfRopeStringImpl(scratch));
    isOther.append(jit.branchTest32(CCallHelpers::Zero, Address(scratch, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    isOther.append(jit.branchTest32(CCallHelpers::Zero, Address(scratch, StringImpl::lengthMemoryOffset())));
    jit.loadPtr(Address(scratch, StringImpl::dataOffset()), scratch);
    jit.load8(Address(scratch), scratch);
    jit.or32(TrustedImm32('a' - 'A'), scratch);
    jit.sub32(TrustedImm32('a'), scratch);
    Jump isNoLetter = jit.branch32(CCallHelpers::Above, scratch, TrustedImm32('z' - 'a'));
    isOther.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32('i' - 'a')));
    isNaN.link(&jit);
    jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), scratch);
    jit.move64ToDouble(scratch, result);
    jit.returnToCaller();

    isNoLetter.link(&jit);
    constexpr int32_t mostCharacters = 9;
    static_assert(areDistinctOnEveryISA(StubRegister::T9, StubRegister::T10, StubRegister::T11, StubRegister::T12, StubRegister::T13));
    const GPRReg remaining = gprOf(StubRegister::T10);
    const GPRReg digit = gprOf(StubRegister::T9);
    const GPRReg digits = gprOf(StubRegister::T12);
    const GPRReg divisor = gprOf(StubRegister::T13);
    const std::array borrowed { remaining, digit, digits, divisor };
    auto giveBack = [&] {
        for (GPRReg reg : borrowed | std::views::reverse)
            jit.popToRestore(reg);
    };
    jit.loadPtr(Address(value, JSString::offsetOfValue()), scratch);
    isOther.append(jit.branchIfRopeStringImpl(scratch));
    isOther.append(jit.branch32(CCallHelpers::Above, Address(scratch, StringImpl::lengthMemoryOffset()), TrustedImm32(mostCharacters)));
    for (GPRReg reg : borrowed)
        jit.pushToSave(reg);
    jit.load32(Address(scratch, StringImpl::lengthMemoryOffset()), remaining);
    jit.loadPtr(Address(scratch, StringImpl::dataOffset()), scratch);
    jit.move(TrustedImm32(0), digits);
    jit.move(TrustedImm32(0), divisor);
    JumpList isNotPlain;
    Label nextCharacter = jit.label();
    jit.load8(Address(scratch), digit);
    jit.sub32(TrustedImm32('0'), digit);
    Jump isNoDigit = jit.branch32(CCallHelpers::Above, digit, TrustedImm32(9));
    jit.mul32(TrustedImm32(10), digits, digits);
    jit.add32(digit, digits);
    jit.mul32(TrustedImm32(10), divisor, divisor);
    Label isTaken = jit.label();
    jit.addPtr(TrustedImm32(1), scratch);
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(1), remaining).linkTo(nextCharacter, &jit);
    isNotPlain.append(jit.branch32(CCallHelpers::Equal, divisor, TrustedImm32(1)));
    jit.convertInt32ToDouble(digits, result);
    Jump isInteger = jit.branchTest32(CCallHelpers::Zero, divisor);
    const FPRReg divisorFPR = FPRInfo::fpRegT1;
    jit.saveFPR(divisorFPR, scratch);
    jit.convertInt32ToDouble(divisor, divisorFPR);
    jit.divDouble(divisorFPR, result);
    jit.restoreFPR(divisorFPR, scratch);
    isInteger.link(&jit);
    giveBack();
    jit.returnToCaller();

    isNoDigit.link(&jit);
    isNotPlain.append(jit.branch32(CCallHelpers::NotEqual, digit, TrustedImm32('.' - '0')));
    isNotPlain.append(jit.branchTest32(CCallHelpers::NonZero, divisor));
    jit.move(TrustedImm32(1), divisor);
    jit.jump().linkTo(isTaken, &jit);

    isNotPlain.link(&jit);
    giveBack();

    isOther.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTToNumber, { .returns = OperationCall::Returns::Double, .saves = Saves::CallerSavedRegisters });
}

StubSignature signatureOfNumberOfAnyValue(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1) }); }

void generateNumberOfAnyValue(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, value, numberTag);
    jit.move(value, jit.result());
    jit.returnToCaller();

    notNumber.link(&jit);
    Jump isCell = jit.branchIfCell(value);
    Jump isUndefined = jit.branch64(CCallHelpers::Equal, value, TrustedImm32(JSValue::ValueUndefined));
    jit.move(value, jit.result());
    jit.and32(TrustedImm32(1), jit.result());
    boxInt32(jit, jit.result(), jit.result());
    jit.returnToCaller();

    isUndefined.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(jsNaN())), jit.result());
    jit.returnToCaller();

    isCell.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNumberOfAnyValue, { });
}

static void returnStringFromFrontEnd(StubAssembler& jit, GPRReg string)
{
    jit.move(string, jit.result());
    jit.move(TrustedImm32(0), jit.secondResult());
    jit.returnToCaller();
}

StubSignature signatureOfToStringFrontEnd(StubKey)
{
    return operationFrontEndSignature(1);
}

static void emitFrontEndThatMakesText(StubAssembler& jit, Entry rawOperation)
{
    const GPRReg value = jit.operand(0);
    const GPRReg first = jit.allocateScratch(isARM64() ? StubRegister::T11 : StubRegister::R0);
    const GPRReg second = jit.allocateScratch(StubRegister::T10);
    const GPRReg high = jit.allocateScratch(isARM64() ? StubRegister::T9 : StubRegister::A2);
    static_assert(sizeof(NumericStrings::StringWithJSString) == 2 * sizeof(void*) && sizeof(NumericStrings::CacheEntryWithJSString<int>) == 3 * sizeof(void*));
#if CPU(X86_64)
    static_assert(gprOf(StubRegister::R0) == X86Registers::eax && gprOf(StubRegister::A2) == X86Registers::edx, "mul writes its product to rdx:rax");
#endif

    JumpList isNotKnown;
    Jump isNoInteger = jit.branch64(CCallHelpers::Below, value, numberTag);
    Jump isSmall = jit.branch32(CCallHelpers::Below, value, TrustedImm32(NumericStrings::cacheSize));
    jit.zeroExtend32ToWord(value, first);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(numbers.hashSecrets) + sizeof(uint64_t)), second);
    jit.xor64(first, second);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(numbers.hashSecrets)), high);
    jit.xor64(high, first);
#if CPU(ARM64)
    jit.uMulHigh64(first, second, high);
    jit.mul64(second, first);
#else
    jit.x86UMulHigh64(second, first, high);
#endif
    jit.xor32(high, first);
    jit.and32(TrustedImm32(NumericStrings::cacheSize - 1), first);
    jit.getEffectiveAddress(BaseIndex(first, first, CCallHelpers::TimesTwo), first);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.intStrings)), second);
    jit.getEffectiveAddress(BaseIndex(second, first, CCallHelpers::TimesEight), second);
    isNotKnown.append(jit.branch32(CCallHelpers::NotEqual, Address(second, NumericStrings::CacheEntryWithJSString<int>::offsetOfKey()), value));
    jit.loadPtr(Address(second, NumericStrings::CacheEntryWithJSString<int>::offsetOfJSString()), second);
    isNotKnown.append(jit.branchTestPtr(CCallHelpers::Zero, second));
    returnStringFromFrontEnd(jit, second);

    isSmall.link(&jit);
    jit.zeroExtend32ToWord(value, first);
    jit.lshift64(TrustedImm32(1), first);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.smallIntStrings)), second);
    jit.loadPtr(BaseIndex(second, first, CCallHelpers::TimesEight, NumericStrings::StringWithJSString::offsetOfJSString()), second);
    isNotKnown.append(jit.branchTestPtr(CCallHelpers::Zero, second));
    returnStringFromFrontEnd(jit, second);

    isNoInteger.link(&jit);
    Jump isDouble = jit.branchTest64(CCallHelpers::NonZero, value, numberTag);
    isNotKnown.append(jit.branch64(CCallHelpers::AboveOrEqual, value, TrustedImm32(RealmWords::numberOfImmediates)));
    jit.loadPtr(BaseIndex(instanceGPR, value, CCallHelpers::TimesEight, AOT_INSTANCE_OFFSET(realm.stringsOfImmediates)), second);
    isNotKnown.append(jit.branchTestPtr(CCallHelpers::Zero, second));
    returnStringFromFrontEnd(jit, second);

    isDouble.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.doubleStrings)), second);
    isNotKnown.append(jit.branchTestPtr(CCallHelpers::Zero, second));
    jit.move(TrustedImm64(static_cast<int64_t>(DoubleStringCache::multiplier)), first);
    jit.mul64(value, first);
    jit.urshift64(TrustedImm32(64 - DoubleStringCache::logOfNumberOfEntries), first);
    jit.lshift64(TrustedImm32(DoubleStringCache::logOfEntrySize), first);
    jit.addPtr(first, second);
    isNotKnown.append(jit.branch64(CCallHelpers::NotEqual, Address(second, OBJECT_OFFSETOF(DoubleStringCache::Entry, number)), value));
    jit.loadPtr(Address(second, OBJECT_OFFSETOF(DoubleStringCache::Entry, string)), second);
    returnStringFromFrontEnd(jit, second);

    isNotKnown.link(&jit);
    jit.tailCallOperation(rawOperation, OperationCall::FirstArgument::Instance);
}

StubSignature signatureOfStringOfAnyValueFrontEnd(StubKey) { return operationFrontEndSignature(1); }
void generateToStringFrontEnd(StubAssembler& jit) { emitFrontEndThatMakesText(jit, Entry::RawToString); }
void generateStringOfAnyValueFrontEnd(StubAssembler& jit) { emitFrontEndThatMakesText(jit, Entry::RawStringOfAnyValue); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
