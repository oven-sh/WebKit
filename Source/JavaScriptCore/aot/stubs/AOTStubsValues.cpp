/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsValues.h"

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTInstance.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubAssembler.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsStrings.h"
#include "AOTVector.h"
#include "JSCJSValueInlines.h"
#include "JSCell.h"
#include "JSString.h"
#include "JSType.h"
#include "JSTypeInfo.h"
#include "NumericStrings.h"
#include "StackAlignment.h"
#include <initializer_list>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

using Saves = OperationCall::Saves;

StubSignature signatureOfToBoolean(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::R0) });
}

StubSignature signatureOfValueIsTruthy(StubKey)
{
    return signatureWithFlagsResult({ gprOf(StubRegister::R0) }, MacroAssembler::NonZero);
}

StubSignature signatureOfValueIsCallable(StubKey)
{
    return signatureWithFlagsResult({ gprOf(StubRegister::R0) }, MacroAssembler::NonZero);
}

StubSignature signatureOfEqual(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1) });
}

void callBinaryOperation(StubAssembler& jit, Entry entry)
{
    ASSERT(jit.operand(0) == gprOf(StubRegister::R0) && jit.operand(1) == gprOf(StubRegister::A1));
    jit.callOperationAndReturn(entry, { .secondArgumentIn = jit.operand(0), .thirdArgumentIn = jit.operand(1) });
}

// The stub that answers in its result register looks for a number first, the one that answers in the flags for a cell.
static void generateTruthiness(StubAssembler& jit, bool answersInFlags)
{
    const GPRReg value = jit.operand(0);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    const GPRReg limit = jit.allocateScratch(StubRegister::T10);
    JumpList slow;

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
        JumpList isFalsy;
        isFalsy.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
        jit.add64(numberTag, value, scratch);
        jit.lshift64(scratch, TrustedImm32(1), scratch);
        isFalsy.append(jit.branchTest64(CCallHelpers::Zero, scratch));
        jit.move(TrustedImm64(static_cast<int64_t>(0xffe0000000000000ULL)), limit);
        isFalsy.append(jit.branch64(CCallHelpers::Above, scratch, limit));
        returnConstant(true);
        isFalsy.link(&jit);
        returnConstant(false);
    };
    auto answerForCell = [&] {
        jit.load8(Address(value, JSCell::typeInfoTypeOffset()), scratch);
        Jump notObject = jit.branch32(CCallHelpers::Below, scratch, TrustedImm32(ObjectType));
        slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(value, JSCell::typeInfoFlagsOffset()), TrustedImm32(MasqueradesAsUndefined)));
        returnConstant(true);

        notObject.link(&jit);
        Jump notString = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(StringType));
        jit.loadPtr(Address(value, JSString::offsetOfValue()), scratch);
        loadStringLength(jit, value, scratch, scratch);
        returnIsNotZero(scratch);

        notString.link(&jit);
        slow.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(SymbolType)));
        returnConstant(true);
    };

    static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined);
    static_assert(JSValue::ValueTrue & 1 && !((JSValue::ValueFalse | JSValue::ValueNull | JSValue::ValueUndefined | JSValue::ValueEmpty) & 1));
    Jump isCellOrNumber = jit.branch64(CCallHelpers::Above, value, TrustedImm32(JSValue::ValueUndefined));
    if (answersInFlags) {
        jit.returnFlagsTest32(value, TrustedImm32(1));

        isCellOrNumber.link(&jit);
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

    slow.link(&jit);
    OperationCall call { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = value };
    if (!answersInFlags)
        return jit.callOperationAndReturn(Entry::operationAOTToBoolean, call);
    call.valueDestination = scratch;
    jit.callOperationAndReturn(Entry::operationAOTToBoolean, call, [&] {
        jit.compareOnFlags32(scratch, TrustedImm32(0));
    });
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
    // A slim closure that is as it was created.
    isFunction.append(jit.branch8(CCallHelpers::GreaterThanOrEqual, Address(cell, offsetOfSlimClosureWord + ClosureWord::topByteShift / 8), TrustedImm32(ClosureWord::minTopByteOfCalleeTaking(ClosureWord::maxNumberOfParameters))));
    jit.load8(Address(cell, JSCell::typeInfoTypeOffset()), scratch);
    jit.sub32(TrustedImm32(functionTypes.first), scratch);
    isFunction.append(jit.branch32(CCallHelpers::BelowOrEqual, scratch, TrustedImm32(functionTypes.last - functionTypes.first)));
    jit.sub32(TrustedImm32(slimClosureTypes.first - functionTypes.first), scratch);
    isFunction.append(jit.branch32(CCallHelpers::BelowOrEqual, scratch, TrustedImm32(slimClosureTypes.last - slimClosureTypes.first)));
    return isFunction;
}

void generateValueIsCallable(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);

    Jump notCell = jit.branchIfNotCell(value);
    JumpList isFunction = branchIfFunction(jit, value, scratch);
    Jump hasOwnAnswer = jit.branchTest8(CCallHelpers::NonZero, Address(value, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetCallData));
    notCell.link(&jit);
    jit.returnFlagsConstant(false);

    isFunction.link(&jit);
    Label isCallable = jit.label();
    jit.returnFlagsConstant(true);

    // ProxyObject::createStructure() gives this flag to the Structure of those whose target is callable, and to no other.
    hasOwnAnswer.link(&jit);
    Jump isNoProxy = jit.branchIfNotType(value, ProxyObjectType);
    jit.branchTest8(CCallHelpers::NonZero, Address(value, JSCell::typeInfoFlagsOffset()), TrustedImm32(ImplementsDefaultHasInstance)).linkTo(isCallable, &jit);
    jit.returnFlagsConstant(false);

    isNoProxy.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTIsCallable, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = value, .valueDestination = scratch }, [&] {
        jit.compareOnFlags32(scratch, TrustedImm32(0));
    });
}

// The value is where the call that follows wants its callee.
StubSignature signatureOfThrowUnlessCallable(StubKey)
{
    return StubSignature::withoutResult({ calleeGPR });
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

StubSignature signatureOfThrowUnlessForwardedCalleeIsCallable(StubKey)
{
    return StubSignature::withoutResult({ calleeGPR, thisGPR });
}

void generateThrowUnlessForwardedCalleeIsCallable(StubAssembler& jit)
{
    const GPRReg callee = jit.operand(0);
    const GPRReg receiver = jit.operand(1);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);

    JumpList slowCases;
    slowCases.append(jit.branchIfNotCell(callee));
    slowCases.append(jit.branchIfNotCell(receiver));
    JumpList calleeIsFunction = branchIfFunction(jit, callee, scratch);
    slowCases.append(jit.jump());

    calleeIsFunction.link(&jit);
    JumpList receiverIsFunction = branchIfFunction(jit, receiver, scratch);
    slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTThrowUnlessForwardedCalleeIsCallable, { .returns = OperationCall::Returns::Void, .saves = Saves::CallerSavedRegisters, .secondArgumentIn = callee, .thirdArgumentIn = receiver });

    receiverIsFunction.link(&jit);
    jit.returnToCaller();
}

// A strict comparison passes its operands swapped: both operations it calls are symmetric, and the right operand already is the second C argument.
static OperationCall equalityOperationCall(bool isStrict, GPRReg left, GPRReg right, bool checksException)
{
    if (isStrict)
        return { .saves = Saves::CallerSavedRegisters, .checksException = checksException, .thirdArgumentIn = left };
    return { .checksException = checksException, .secondArgumentIn = left, .thirdArgumentIn = right };
}

static void emitCompareStringImplsAndReturn(StubAssembler& jit, GPRReg leftImpl, GPRReg rightImpl)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1));
    RELEASE_ASSERT(jit.result() == GPRInfo::returnValueGPR);
    Vector<GPRReg, 8> saved;
    // The loose form calls operations without saving a register, so its callers keep nothing in one that a leaf clobbers.
    if (static_cast<Equality>(jit.key().form()) != Equality::Loose) {
        RegisterSet lost = registersClobberedByPreserveMostLeaves();
        lost.add(leafArgumentGPR(0));
        lost.add(leafArgumentGPR(1));
        lost.filter(RegisterSet::allGPRs());
        lost.exclude(RegisterSet::macroClobberedGPRs());
        lost.remove(jit.result());
        for (StubRegister scratch : { StubRegister::T9, StubRegister::T10, StubRegister::T11, StubRegister::T12 })
            lost.remove(gprOf(scratch));
        lost.forEach([&](Reg reg) {
            saved.append(reg.gpr());
        });
    }
    jit.preserving(saved.span(), [&] {
        const GPRReg firstArgument = jit.reuseScratch(StubRegister::A0);
        const GPRReg secondArgument = jit.reuseScratch(StubRegister::A1);
        RELEASE_ASSERT(noOverlap(leftImpl, rightImpl, firstArgument, secondArgument));
        jit.move(leftImpl, firstArgument);
        jit.move(rightImpl, secondArgument);
        jit.callLeaf(Entry::operationAOTStringImplsAreEqual, WTF::roundUpToMultipleOf<stackAlignmentBytes()>(saved.size() * sizeof(CPURegister)));
    });
    jit.returnToCaller();
}

void generateEqual(StubAssembler& jit)
{
    const bool isStrict = static_cast<Equality>(jit.key().form()) == Equality::Strict;
    const GPRReg left = jit.operand(0);
    const GPRReg right = isStrict ? jit.operand(1) : jit.clobberOperand(1);
    const GPRReg result = jit.result();
    const GPRReg leftValue = jit.allocateScratch(StubRegister::T9);
    const GPRReg rightValue = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg flags = jit.allocateScratch(StubRegister::T12);

    JumpList isTrue;
    JumpList isFalse;
    JumpList needsContent;
    JumpList& kindsDiffer = isStrict ? isFalse : needsContent;

    Jump haveSameBits = jit.branch64(CCallHelpers::Equal, left, right);
    jit.or64(left, right, leftValue);
    Jump areNotBothCells = jit.branchIfNotCell(leftValue);
    Jump notStrings;
    if (isStrict) {
        // Two cells that are not one cell.
        static_assert(StringType + 1 == HeapBigIntType && HeapBigIntType == LastValueCompareCellType);
        isFalse.append(jit.branch8(CCallHelpers::Above, Address(left, JSCell::typeInfoTypeOffset()), TrustedImm32(HeapBigIntType)));
        notStrings = jit.branchOnFlags(CCallHelpers::Equal);
        isFalse.append(jit.branch8(CCallHelpers::NotEqual, Address(right, JSCell::typeInfoTypeOffset()), TrustedImm32(StringType)));
    } else {
        jit.load8(Address(left, JSCell::typeInfoTypeOffset()), leftValue);
        jit.load8(Address(right, JSCell::typeInfoTypeOffset()), rightValue);
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
    Jump areNotBothAtoms = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(StringImpl::flagIsAtom()));
    Label returnsFalse = jit.label();
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();

    haveSameBits.link(&jit);
    Jump isNumber = jit.branchTest64(CCallHelpers::NonZero, left, numberTag);
    jit.move(TrustedImm32(1), result);
    jit.returnToCaller();

    notStrings.link(&jit);
    if (isStrict)
        needsContent.append(jit.branch8(CCallHelpers::Equal, Address(right, JSCell::typeInfoTypeOffset()), TrustedImm32(HeapBigIntType)));
    else
        needsContent.append(jit.branch32(CCallHelpers::Equal, leftValue, TrustedImm32(HeapBigIntType)));
    isFalse.append(jit.jump());

    areNotBothCells.link(&jit);
    isNumber.link(&jit);
    const FPRReg leftNumber = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg rightNumber = jit.allocateScratch(FPRInfo::fpRegT1);
    Jump leftNotInt32 = jit.branch64(CCallHelpers::Below, left, numberTag);
    Jump rightNotInt32 = jit.branch64(CCallHelpers::Below, right, numberTag);
    jit.compare32(CCallHelpers::Equal, left, right, result);
    jit.returnToCaller();

    leftNotInt32.link(&jit);
    rightNotInt32.link(&jit);
    kindsDiffer.append(jit.branchTest64(CCallHelpers::Zero, left, numberTag));
    kindsDiffer.append(jit.branchTest64(CCallHelpers::Zero, right, numberTag));
    toDouble(jit, left, length, leftNumber);
    toDouble(jit, right, length, rightNumber);
    isTrue.append(jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, leftNumber, rightNumber));
    isFalse.append(jit.jump());

    areNotBothAtoms.link(&jit);
    jit.load32(Address(leftValue, StringImpl::lengthMemoryOffset()), length);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, length, Address(rightValue, StringImpl::lengthMemoryOffset())));
    JumpList needsLeaf;
    // The left operand is in the result register, and nothing needs it any more.
    branchIfCharactersDiffer(jit, { leftValue, rightValue, length, flags, result }, isFalse, needsLeaf);
    isTrue.append(jit.jump());

    needsLeaf.link(&jit);
    emitCompareStringImplsAndReturn(jit, leftValue, rightValue);

    oneIsRope.link(&jit);
    loadStringLength(jit, left, leftValue, length);
    loadStringLength(jit, right, rightValue, flags);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, length, flags));
    JumpList needsOperation;
    branchIfCharactersInPlaceDiffer(jit, { left, right, leftValue, rightValue, length, flags, result }, isFalse, needsOperation);
    isTrue.append(jit.jump());

    needsOperation.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTStringsAreEqual, equalityOperationCall(isStrict, left, right, false));

    isTrue.link(&jit);
    jit.move(TrustedImm32(1), result);
    jit.returnToCaller();

    needsContent.link(&jit);
    jit.callOperationAndReturn(isStrict ? Entry::operationAOTCompareStrictEq : Entry::operationAOTCompareEq, equalityOperationCall(isStrict, left, right, true));

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
    jit.callOperationAndReturn(Entry::operationAOTToNumber, { .returns = OperationCall::Returns::Double, .saves = Saves::CallerSavedRegisters });
}

static void returnString(StubAssembler& jit, GPRReg string)
{
    jit.move(string, jit.result());
    jit.move(TrustedImm32(0), jit.secondResult());
    jit.returnToCaller();
}

StubSignature signatureOfToStringFrontEnd(StubKey)
{
    return operationFrontEndSignature(1);
}

void generateToStringFrontEnd(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg first = jit.allocateScratch(isARM64() ? StubRegister::T11 : StubRegister::R0);
    const GPRReg second = jit.allocateScratch(StubRegister::T10);
    const GPRReg high = jit.allocateScratch(isARM64() ? StubRegister::T9 : StubRegister::A2);
    static_assert(sizeof(NumericStrings::StringWithJSString) == 2 * sizeof(void*) && sizeof(NumericStrings::CacheEntryWithJSString<int>) == 3 * sizeof(void*));

    JumpList isNotKnown;
    isNotKnown.append(jit.branchIfNotInt32(value));
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
    returnString(jit, second);

    isSmall.link(&jit);
    jit.zeroExtend32ToWord(value, first);
    jit.lshift64(TrustedImm32(1), first);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.smallIntStrings)), second);
    jit.loadPtr(BaseIndex(second, first, CCallHelpers::TimesEight, NumericStrings::StringWithJSString::offsetOfJSString()), second);
    isNotKnown.append(jit.branchTestPtr(CCallHelpers::Zero, second));
    returnString(jit, second);

    isNotKnown.link(&jit);
    jit.tailCallOperation(Entry::RawToString, OperationCall::FirstArgument::Instance);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
