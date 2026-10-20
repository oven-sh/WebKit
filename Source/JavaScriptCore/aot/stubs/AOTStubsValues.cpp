/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsValues.h"

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsStrings.h"
#include "JSCJSValueInlines.h"
#include "JSCell.h"
#include "JSString.h"
#include "JSType.h"
#include "JSTypeInfo.h"
#include "StackAlignment.h"
#include <initializer_list>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

using Saves = OperationCall::Saves;

StubSignature signatureOfToBoolean(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::R0) });
}

StubSignature signatureOfValueIsTruthy(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::R0));
    signature.result = StubResult::Flags;
    signature.trueCondition = MacroAssembler::NonZero;
    return signature;
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

static void appendIfDoubleIsFalsy(StubAssembler& jit, GPRReg boxed, GPRReg bits, GPRReg limit, JumpList& isFalsy)
{
    static_assert(!(JSValue::NumberTag + JSValue::DoubleEncodeOffset));
    jit.add64(numberTag, boxed, bits);
    jit.lshift64(bits, TrustedImm32(1), bits);
    isFalsy.append(jit.branchTest64(CCallHelpers::Zero, bits));
    jit.move(TrustedImm64(static_cast<int64_t>(0xffe0000000000000ULL)), limit);
    isFalsy.append(jit.branch64(CCallHelpers::Above, bits, limit));
}

void generateToBoolean(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg result = jit.result();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    const GPRReg limit = jit.allocateScratch(StubRegister::T10);

    auto returnConstant = [&](bool constant) {
        jit.move(TrustedImm32(constant), result);
        jit.returnToCaller();
    };

    static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined);
    static_assert(JSValue::ValueTrue & 1 && !((JSValue::ValueFalse | JSValue::ValueNull | JSValue::ValueUndefined | JSValue::ValueEmpty) & 1));
    Jump isCellOrNumber = jit.branch64(CCallHelpers::Above, value, TrustedImm32(JSValue::ValueUndefined));
    jit.and32(TrustedImm32(1), value, result);
    jit.returnToCaller();

    isCellOrNumber.link(&jit);
    Jump isCell = jit.branchIfCell(value);
    Jump notInt32 = jit.branch64(CCallHelpers::Below, value, numberTag);
    jit.test32(CCallHelpers::NonZero, value, value, result);
    jit.returnToCaller();

    notInt32.link(&jit);
    JumpList isFalsy;
    isFalsy.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
    appendIfDoubleIsFalsy(jit, value, scratch, limit, isFalsy);
    returnConstant(true);
    isFalsy.link(&jit);
    returnConstant(false);

    isCell.link(&jit);
    JumpList slow;
    jit.load8(Address(value, JSCell::typeInfoTypeOffset()), scratch);
    Jump notObject = jit.branch32(CCallHelpers::Below, scratch, TrustedImm32(ObjectType));
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(value, JSCell::typeInfoFlagsOffset()), TrustedImm32(MasqueradesAsUndefined)));
    returnConstant(true);

    notObject.link(&jit);
    Jump notString = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(StringType));
    jit.loadPtr(Address(value, JSString::offsetOfValue()), scratch);
    loadStringLength(jit, value, scratch, scratch);
    jit.test32(CCallHelpers::NonZero, scratch, scratch, result);
    jit.returnToCaller();

    notString.link(&jit);
    slow.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(SymbolType)));
    returnConstant(true);

    slow.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTToBoolean, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = value });
}

void generateValueIsTruthy(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    const GPRReg limit = jit.allocateScratch(StubRegister::T10);

    static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined);
    static_assert(JSValue::ValueTrue & 1 && !(JSValue::ValueFalse & 1) && !(JSValue::ValueNull & 1) && !(JSValue::ValueUndefined & 1));
    Jump isCellOrNumber = jit.branch64(CCallHelpers::Above, value, TrustedImm32(JSValue::ValueUndefined));
    jit.returnFlagsTest32(value, TrustedImm32(1));

    isCellOrNumber.link(&jit);
    Jump notCell = jit.branchIfNotCell(value);
    JumpList slow;
    jit.load8(Address(value, JSCell::typeInfoTypeOffset()), scratch);
    Jump notObject = jit.branch32(CCallHelpers::Below, scratch, TrustedImm32(ObjectType));
    slow.append(jit.branchTest8(CCallHelpers::NonZero, Address(value, JSCell::typeInfoFlagsOffset()), TrustedImm32(MasqueradesAsUndefined)));
    jit.returnFlagsConstant(true);

    notObject.link(&jit);
    Jump notString = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(StringType));
    jit.loadPtr(Address(value, JSString::offsetOfValue()), scratch);
    loadStringLength(jit, value, scratch, scratch);
    jit.returnFlagsTest32(scratch);

    notString.link(&jit);
    slow.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(SymbolType)));
    jit.returnFlagsConstant(true);

    notCell.link(&jit);
    Jump notInt32 = jit.branch64(CCallHelpers::Below, value, numberTag);
    jit.returnFlagsTest32(value);

    notInt32.link(&jit);
    JumpList isFalsy;
    isFalsy.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
    appendIfDoubleIsFalsy(jit, value, scratch, limit, isFalsy);
    jit.returnFlagsConstant(true);
    isFalsy.link(&jit);
    jit.returnFlagsConstant(false);

    slow.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTToBoolean, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = value, .valueDestination = scratch }, [&] {
        jit.compareOnFlags32(scratch, TrustedImm32(0));
    });
}

struct EqualityExits {
    JumpList isTrue;
    JumpList isFalse;
    JumpList needsContent;
};

static void compareNumbersAndReturn(StubAssembler& jit, GPRReg left, GPRReg right, GPRReg scratch, JumpList& kindsDiffer, EqualityExits& exits)
{
    const GPRReg result = jit.result();
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
    toDouble(jit, left, scratch, leftNumber);
    toDouble(jit, right, scratch, rightNumber);
    exits.isTrue.append(jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, leftNumber, rightNumber));
    exits.isFalse.append(jit.jump());
}

// A strict comparison passes its operands swapped: both operations it calls are symmetric, and the right operand already is the second C argument.
static OperationCall equalityOperationCall(bool isStrict, GPRReg left, GPRReg right, bool checksException)
{
    if (isStrict)
        return { .saves = Saves::CallerSavedRegisters, .checksException = checksException, .thirdArgumentIn = left };
    return { .checksException = checksException, .secondArgumentIn = left, .thirdArgumentIn = right };
}

static Vector<GPRReg, 8> registersToSaveForLeafCall(const StubAssembler& jit)
{
    // The loose form calls operations without saving a register, so its callers keep nothing in one that a leaf clobbers.
    if (static_cast<Equality>(jit.key().form()) == Equality::Loose)
        return { };

    RegisterSet lost = registersClobberedByPreserveMostLeaves();
    lost.add(leafArgumentGPR(0));
    lost.add(leafArgumentGPR(1));
    lost.filter(RegisterSet::allGPRs());
    lost.exclude(RegisterSet::macroClobberedGPRs());
    lost.remove(jit.result());
    for (StubRegister scratch : { StubRegister::T9, StubRegister::T10, StubRegister::T11, StubRegister::T12 })
        lost.remove(gprOf(scratch));

    Vector<GPRReg, 8> saved;
    lost.forEach([&](Reg reg) {
        saved.append(reg.gpr());
    });
    return saved;
}

static void emitCompareStringImplsAndReturn(StubAssembler& jit, GPRReg leftImpl, GPRReg rightImpl)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1));
    RELEASE_ASSERT(jit.result() == GPRInfo::returnValueGPR);
    const Vector<GPRReg, 8> saved = registersToSaveForLeafCall(jit);
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

    EqualityExits exits;
    JumpList& kindsDiffer = isStrict ? exits.isFalse : exits.needsContent;

    Jump haveSameBits = jit.branch64(CCallHelpers::Equal, left, right);
    jit.or64(left, right, leftValue);
    Jump areNotBothCells = jit.branchIfNotCell(leftValue);
    jit.load8(Address(left, JSCell::typeInfoTypeOffset()), leftValue);
    jit.load8(Address(right, JSCell::typeInfoTypeOffset()), rightValue);
    kindsDiffer.append(jit.branch32(CCallHelpers::NotEqual, leftValue, rightValue));
    Jump notStrings = jit.branch32(CCallHelpers::NotEqual, leftValue, TrustedImm32(StringType));
    jit.loadPtr(Address(left, JSString::offsetOfValue()), leftValue);
    jit.loadPtr(Address(right, JSString::offsetOfValue()), rightValue);
    jit.or64(leftValue, rightValue, length);
    Jump oneIsRope = jit.branchTestPtr(CCallHelpers::NonZero, length, TrustedImm32(JSString::isRopeInPointer));
    exits.isTrue.append(jit.branchPtr(CCallHelpers::Equal, leftValue, rightValue));
    jit.load32(Address(leftValue, StringImpl::flagsOffset()), flags);
    jit.and32(Address(rightValue, StringImpl::flagsOffset()), flags);
    Jump areNotBothAtoms = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(StringImpl::flagIsAtom()));
    Label isFalse = jit.label();
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();

    haveSameBits.link(&jit);
    Jump isNumber = jit.branchTest64(CCallHelpers::NonZero, left, numberTag);
    jit.move(TrustedImm32(1), result);
    jit.returnToCaller();

    notStrings.link(&jit);
    exits.needsContent.append(jit.branch32(CCallHelpers::Equal, leftValue, TrustedImm32(HeapBigIntType)));
    exits.isFalse.append(jit.jump());

    areNotBothCells.link(&jit);
    isNumber.link(&jit);
    compareNumbersAndReturn(jit, left, right, length, kindsDiffer, exits);

    areNotBothAtoms.link(&jit);
    jit.load32(Address(leftValue, StringImpl::lengthMemoryOffset()), length);
    exits.isFalse.append(jit.branch32(CCallHelpers::NotEqual, length, Address(rightValue, StringImpl::lengthMemoryOffset())));
    JumpList needsLeaf;
    // The left operand is in the result register, and nothing needs it any more.
    branchIfCharactersDiffer(jit, { leftValue, rightValue, length, flags, result }, exits.isFalse, needsLeaf);
    exits.isTrue.append(jit.jump());

    needsLeaf.link(&jit);
    emitCompareStringImplsAndReturn(jit, leftValue, rightValue);

    oneIsRope.link(&jit);
    loadStringLength(jit, left, leftValue, length);
    loadStringLength(jit, right, rightValue, flags);
    exits.isFalse.append(jit.branch32(CCallHelpers::NotEqual, length, flags));
    jit.callOperationAndReturn(Entry::operationAOTStringsAreEqual, equalityOperationCall(isStrict, left, right, false));

    exits.isTrue.link(&jit);
    jit.move(TrustedImm32(1), result);
    jit.returnToCaller();

    exits.needsContent.link(&jit);
    jit.callOperationAndReturn(isStrict ? Entry::operationAOTCompareStrictEq : Entry::operationAOTCompareEq, equalityOperationCall(isStrict, left, right, true));

    exits.isFalse.linkTo(isFalse, &jit);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
