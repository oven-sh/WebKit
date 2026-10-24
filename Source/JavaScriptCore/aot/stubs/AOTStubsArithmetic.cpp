/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArithmetic.h"

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsStrings.h"
#include "AOTStubsValues.h"
#include "JSCJSValueInlines.h"
#include "JSString.h"

namespace JSC { namespace AOT {

static constexpr bool binaryOperationFormsRoundTrip()
{
    constexpr uint8_t numberOfForms = descriptorOf(StubFamily::BinaryOperation).numberOfForms;
    static_assert(static_cast<uint8_t>(BinaryOperationForm::GreaterEqAfterInt32Check) + 1 == numberOfForms);
    for (uint8_t index = 0; index < numberOfForms; ++index) {
        BinaryOperationForm form = static_cast<BinaryOperationForm>(index);
        BinaryOperationShape shape = shapeOf(form);
        if (binaryOperationForm(shape.operation, shape.isAfterInt32Check) != form)
            return false;
    }
    return true;
}
static_assert(binaryOperationFormsRoundTrip());
static_assert(static_cast<uint8_t>(BinaryWithDoubleForm::DivWithNumberOnRight) + 1 == descriptorOf(StubFamily::BinaryWithDouble).numberOfForms);
static_assert(static_cast<uint8_t>(UnaryOperationForm::ToNumeric) + 1 == descriptorOf(StubFamily::UnaryOperation).numberOfForms);

struct BinaryRegisters {
    GPRReg left;
    GPRReg right;
    GPRReg first;
    GPRReg second;
    GPRReg scratch;
    FPRReg leftNumber;
    FPRReg rightNumber;
};

static BinaryRegisters allocateBinaryRegisters(StubAssembler& jit)
{
    return {
        jit.clobberOperand(0),
        jit.clobberOperand(1),
        jit.allocateScratch(StubRegister::T9),
        jit.allocateScratch(StubRegister::T10),
        jit.allocateScratch(StubRegister::T11),
        jit.allocateScratch(FPRInfo::fpRegT0),
        jit.allocateScratch(FPRInfo::fpRegT1),
    };
}

static Entry operationOf(BinaryOperator operation)
{
    switch (operation) {
    case BinaryOperator::Add: return Entry::operationAOTValueAdd;
    case BinaryOperator::Sub: return Entry::operationAOTValueSub;
    case BinaryOperator::Mul: return Entry::operationAOTValueMul;
    case BinaryOperator::Div: return Entry::operationAOTValueDiv;
    case BinaryOperator::BitAnd: return Entry::operationAOTValueBitAnd;
    case BinaryOperator::BitOr: return Entry::operationAOTValueBitOr;
    case BinaryOperator::BitXor: return Entry::operationAOTValueBitXor;
    case BinaryOperator::LShift: return Entry::operationAOTValueLShift;
    case BinaryOperator::RShift: return Entry::operationAOTValueRShift;
    case BinaryOperator::URShift: return Entry::operationAOTValueURShift;
    case BinaryOperator::Mod: return Entry::operationAOTValueMod;
    case BinaryOperator::Less: return Entry::operationAOTCompareLess;
    case BinaryOperator::LessEq: return Entry::operationAOTCompareLessEq;
    case BinaryOperator::Greater: return Entry::operationAOTCompareGreater;
    case BinaryOperator::GreaterEq: return Entry::operationAOTCompareGreaterEq;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Entry operationWithDoubleOf(BinaryWithDoubleForm form)
{
    switch (form) {
    case BinaryWithDoubleForm::Add:
        return Entry::operationAOTAddNumberAndValueAsNumber;
    case BinaryWithDoubleForm::Mul:
        return Entry::operationAOTMulNumberAndValueAsNumber;
    case BinaryWithDoubleForm::Sub:
    case BinaryWithDoubleForm::SubWithNumberOnRight:
        return Entry::operationAOTSubNumberAndValueAsNumber;
    case BinaryWithDoubleForm::Div:
    case BinaryWithDoubleForm::DivWithNumberOnRight:
        return Entry::operationAOTDivNumberAndValueAsNumber;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static CCallHelpers::RelationalCondition signedConditionOf(BinaryOperator operation)
{
    switch (operation) {
    case BinaryOperator::Less:
        return CCallHelpers::LessThan;
    case BinaryOperator::LessEq:
        return CCallHelpers::LessThanOrEqual;
    case BinaryOperator::Greater:
        return CCallHelpers::GreaterThan;
    case BinaryOperator::GreaterEq:
        return CCallHelpers::GreaterThanOrEqual;
    case BinaryOperator::Add:
    case BinaryOperator::Sub:
    case BinaryOperator::Mul:
    case BinaryOperator::Div:
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitOr:
    case BinaryOperator::BitXor:
    case BinaryOperator::LShift:
    case BinaryOperator::RShift:
    case BinaryOperator::URShift:
    case BinaryOperator::Mod:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static CCallHelpers::DoubleCondition doubleConditionOf(BinaryOperator operation)
{
    switch (operation) {
    case BinaryOperator::Less:
        return CCallHelpers::DoubleLessThanAndOrdered;
    case BinaryOperator::LessEq:
        return CCallHelpers::DoubleLessThanOrEqualAndOrdered;
    case BinaryOperator::Greater:
        return CCallHelpers::DoubleGreaterThanAndOrdered;
    case BinaryOperator::GreaterEq:
        return CCallHelpers::DoubleGreaterThanOrEqualAndOrdered;
    case BinaryOperator::Add:
    case BinaryOperator::Sub:
    case BinaryOperator::Mul:
    case BinaryOperator::Div:
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitOr:
    case BinaryOperator::BitXor:
    case BinaryOperator::LShift:
    case BinaryOperator::RShift:
    case BinaryOperator::URShift:
    case BinaryOperator::Mod:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void boxInt32AndReturn(StubAssembler& jit, GPRReg int32, GPRReg result)
{
    boxInt32(jit, int32, result);
    jit.returnToCaller();
}

#if CPU(X86_64)
static bool emitInt32CaseInPlace(StubAssembler& jit, const BinaryRegisters& registers, BinaryOperator operation, JumpList& slow)
{
    const GPRReg left = registers.left;
    const GPRReg right = registers.right;
    const GPRReg saved = registers.scratch;
    auto boxInPlaceAndReturn = [&] {
        boxInt32AndReturn(jit, left, left);
    };
    auto restoreAndTakeSlowPath = [&](JumpList& restores) {
        restores.link(&jit);
        jit.move(saved, left);
        slow.append(jit.jump());
    };
    switch (operation) {
    case BinaryOperator::Add: {
        Jump overflows = jit.branchAdd32(CCallHelpers::Overflow, right, left);
        boxInPlaceAndReturn();
        overflows.link(&jit);
        jit.sub32(right, left);
        boxInt32(jit, left, left);
        slow.append(jit.jump());
        return true;
    }
    case BinaryOperator::Sub: {
        Jump overflows = jit.branchSub32(CCallHelpers::Overflow, right, left);
        boxInPlaceAndReturn();
        overflows.link(&jit);
        jit.add32(right, left);
        boxInt32(jit, left, left);
        slow.append(jit.jump());
        return true;
    }
    case BinaryOperator::Mul: {
        JumpList restores;
        jit.move(left, saved);
        restores.append(jit.branchMul32(CCallHelpers::Overflow, right, left));
        restores.append(jit.branchTest32(CCallHelpers::Zero, left));
        boxInPlaceAndReturn();
        restoreAndTakeSlowPath(restores);
        return true;
    }
    case BinaryOperator::BitAnd:
        jit.and32(right, left);
        boxInPlaceAndReturn();
        return true;
    case BinaryOperator::BitOr:
        jit.or32(right, left);
        boxInPlaceAndReturn();
        return true;
    case BinaryOperator::BitXor:
        jit.xor32(right, left);
        boxInPlaceAndReturn();
        return true;
    case BinaryOperator::LShift:
        jit.lshift32(right, left);
        boxInPlaceAndReturn();
        return true;
    case BinaryOperator::RShift:
        jit.rshift32(right, left);
        boxInPlaceAndReturn();
        return true;
    case BinaryOperator::URShift: {
        JumpList restores;
        jit.move(left, saved);
        jit.urshift32(right, left);
        restores.append(jit.branch32(CCallHelpers::LessThan, left, TrustedImm32(0)));
        boxInPlaceAndReturn();
        restoreAndTakeSlowPath(restores);
        return true;
    }
    case BinaryOperator::Div:
    case BinaryOperator::Mod:
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}
#endif

static void emitInt32Remainder(StubAssembler& jit, const BinaryRegisters& registers, JumpList& slow)
{
    slow.append(jit.branch32(CCallHelpers::LessThan, registers.left, TrustedImm32(0)));
    slow.append(jit.branch32(CCallHelpers::LessThanOrEqual, registers.right, TrustedImm32(0)));
#if CPU(X86_64)
    static_assert(gprOf(StubRegister::R0) == X86Registers::eax && gprOf(StubRegister::A2) == X86Registers::edx);
    const GPRReg remainder = jit.allocateScratch(StubRegister::A2);
    jit.x86ConvertToDoubleWord32();
    jit.x86Div32(registers.right);
    boxInt32AndReturn(jit, remainder, registers.left);
#else
    jit.div32(registers.left, registers.right, registers.scratch);
    jit.multiplySub32(registers.scratch, registers.right, registers.left, registers.scratch);
    boxInt32AndReturn(jit, registers.scratch, registers.left);
#endif
}

static void emitInt32Case(StubAssembler& jit, const BinaryRegisters& registers, BinaryOperator operation, JumpList& slow)
{
    const GPRReg left = registers.left;
    const GPRReg right = registers.right;
    const GPRReg scratch = registers.scratch;
    switch (operation) {
    case BinaryOperator::Add:
        slow.append(jit.branchAdd32(CCallHelpers::Overflow, left, right, scratch));
        boxInt32AndReturn(jit, scratch, left);
        return;
    case BinaryOperator::Sub:
        slow.append(jit.branchSub32(CCallHelpers::Overflow, left, right, scratch));
        boxInt32AndReturn(jit, scratch, left);
        return;
    case BinaryOperator::Mul:
        slow.append(jit.branchMul32(CCallHelpers::Overflow, left, right, scratch));
        slow.append(jit.branchTest32(CCallHelpers::Zero, scratch));
        boxInt32AndReturn(jit, scratch, left);
        return;
    case BinaryOperator::Div:
        RELEASE_ASSERT_NOT_REACHED();
    case BinaryOperator::BitAnd:
        jit.and32(left, right, scratch);
        boxInt32AndReturn(jit, scratch, left);
        return;
    case BinaryOperator::BitOr:
        jit.or32(left, right, scratch);
        boxInt32AndReturn(jit, scratch, left);
        return;
    case BinaryOperator::BitXor:
        jit.xor32(left, right, scratch);
        boxInt32AndReturn(jit, scratch, left);
        return;
    case BinaryOperator::LShift:
        jit.lshift32(left, right, scratch);
        boxInt32AndReturn(jit, scratch, left);
        return;
    case BinaryOperator::RShift:
        jit.rshift32(left, right, scratch);
        boxInt32AndReturn(jit, scratch, left);
        return;
    case BinaryOperator::URShift:
        jit.urshift32(left, right, scratch);
        slow.append(jit.branch32(CCallHelpers::LessThan, scratch, TrustedImm32(0)));
        boxInt32AndReturn(jit, scratch, left);
        return;
    case BinaryOperator::Mod:
        emitInt32Remainder(jit, registers, slow);
        return;
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        jit.compare32(signedConditionOf(operation), left, right, left);
        jit.returnToCaller();
        return;
    }
}

static void emitDoubleArithmetic(StubAssembler& jit, const BinaryRegisters& registers, BinaryOperator operation)
{
    const FPRReg left = registers.leftNumber;
    const FPRReg right = registers.rightNumber;
    switch (operation) {
    case BinaryOperator::Add:
        jit.addDouble(left, right, left);
        return;
    case BinaryOperator::Sub:
        jit.subDouble(left, right, left);
        return;
    case BinaryOperator::Mul:
        jit.mulDouble(left, right, left);
        return;
    case BinaryOperator::Div:
        jit.divDouble(left, right, left);
        return;
    case BinaryOperator::Mod:
        static_assert(FPRInfo::fpRegT0 == FPRInfo::argumentFPR0 && FPRInfo::fpRegT1 == FPRInfo::argumentFPR1 && FPRInfo::fpRegT0 == FPRInfo::returnValueFPR);
        RELEASE_ASSERT(left == FPRInfo::fpRegT0 && right == FPRInfo::fpRegT1);
        jit.callLeaf(Entry::operationAOTFmod);
        return;
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitOr:
    case BinaryOperator::BitXor:
    case BinaryOperator::LShift:
    case BinaryOperator::RShift:
    case BinaryOperator::URShift:
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitDoubleCase(StubAssembler& jit, const BinaryRegisters& registers, BinaryOperator operation, JumpList& slow)
{
    slow.append(jit.branchTest64(CCallHelpers::Zero, registers.left, numberTag));
    slow.append(jit.branchTest64(CCallHelpers::Zero, registers.right, numberTag));
    toDouble(jit, registers.left, registers.scratch, registers.leftNumber);
    toDouble(jit, registers.right, registers.scratch, registers.rightNumber);
    if (isComparison(operation)) {
        jit.compareDouble(doubleConditionOf(operation), registers.leftNumber, registers.rightNumber, registers.left);
        jit.returnToCaller();
        return;
    }
    emitDoubleArithmetic(jit, registers, operation);
    JumpList notInt32;
    branchIfNotExactInt32(jit, registers.leftNumber, registers.scratch, notInt32, registers.rightNumber);
    boxInt32AndReturn(jit, registers.scratch, registers.left);
    notInt32.link(&jit);
    boxDouble(jit, PureDouble::resultOfArithmeticOnBoxedNumbers(registers.leftNumber), registers.left);
    jit.returnToCaller();
}

static void emitDoubleToInt32(StubAssembler& jit, FPRReg number, GPRReg result, JumpList& isOutsideInt64)
{
#if CPU(ARM64)
    if (jit.supportsDoubleToInt32ConversionUsingJavaScriptSemantics()) {
        jit.convertDoubleToInt32UsingJavaScriptSemantics(number, result);
        return;
    }
#endif
    isOutsideInt64.append(jit.branchTruncateDoubleToInt32ViaInt64(number, result));
}

static void emitToInt32(StubAssembler& jit, const BinaryRegisters& registers, GPRReg value, GPRReg result, JumpList& slow)
{
    jit.move(value, result);
    Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, value, numberTag);
    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, value, numberTag);
    unboxDouble(jit, value, registers.scratch, registers.leftNumber);
    emitDoubleToInt32(jit, registers.leftNumber, result, slow);
    Jump isTruncated = jit.jump();
    notNumber.link(&jit);
    slow.append(jit.branchIfCell(value));
    jit.compare64(CCallHelpers::Equal, value, TrustedImm32(JSValue::ValueTrue), result);
    isTruncated.link(&jit);
    isInt32.link(&jit);
}

static void emitBitCase(StubAssembler& jit, const BinaryRegisters& registers, BinaryOperator operation, JumpList& slow)
{
    const GPRReg first = registers.first;
    const GPRReg second = registers.second;
    const GPRReg scratch = registers.scratch;
    emitToInt32(jit, registers, registers.left, first, slow);
    emitToInt32(jit, registers, registers.right, second, slow);
    switch (operation) {
    case BinaryOperator::BitAnd:
        jit.and32(first, second, scratch);
        break;
    case BinaryOperator::BitOr:
        jit.or32(first, second, scratch);
        break;
    case BinaryOperator::BitXor:
        jit.xor32(first, second, scratch);
        break;
    case BinaryOperator::LShift:
        jit.lshift32(first, second, scratch);
        break;
    case BinaryOperator::RShift:
        jit.rshift32(first, second, scratch);
        break;
    case BinaryOperator::URShift: {
        jit.urshift32(first, second, scratch);
        Jump fitsInt32 = jit.branch32(CCallHelpers::GreaterThanOrEqual, scratch, TrustedImm32(0));
        jit.zeroExtend32ToWord(scratch, scratch);
        boxDouble(jit, PureDouble::convertInt64(jit, scratch, registers.leftNumber), registers.left);
        jit.returnToCaller();
        fitsInt32.link(&jit);
        break;
    }
    case BinaryOperator::Add:
    case BinaryOperator::Sub:
    case BinaryOperator::Mul:
    case BinaryOperator::Div:
    case BinaryOperator::Mod:
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        RELEASE_ASSERT_NOT_REACHED();
    }
    boxInt32AndReturn(jit, scratch, registers.left);
}

static void emitRopeCase(StubAssembler& jit, const BinaryRegisters& registers)
{
    jit.allocateScratch(StubRegister::T13);
    JumpList notTwoStrings;
    makeRopeOfTwoValuesAndReturn(jit, registers.left, registers.right, notTwoStrings);
    notTwoStrings.link(&jit);
}

static bool holdsFor(BinaryOperator comparison, int order)
{
    switch (comparison) {
    case BinaryOperator::Less:
        return order < 0;
    case BinaryOperator::LessEq:
        return order <= 0;
    case BinaryOperator::Greater:
        return order > 0;
    case BinaryOperator::GreaterEq:
        return order >= 0;
    case BinaryOperator::Add:
    case BinaryOperator::Sub:
    case BinaryOperator::Mul:
    case BinaryOperator::Div:
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitOr:
    case BinaryOperator::BitXor:
    case BinaryOperator::LShift:
    case BinaryOperator::RShift:
    case BinaryOperator::URShift:
    case BinaryOperator::Mod:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitStringOrder(StubAssembler& jit, const BinaryRegisters& registers, BinaryOperator operation)
{
    const GPRReg left = registers.left;
    const GPRReg right = registers.right;
    const GPRReg first = registers.first;
    const GPRReg second = registers.second;

    JumpList notTwoFlatStrings;
    notTwoFlatStrings.append(jit.branchIfNotCell(left));
    notTwoFlatStrings.append(jit.branchIfNotCell(right));
    notTwoFlatStrings.append(jit.branchIfNotString(left));
    notTwoFlatStrings.append(jit.branchIfNotString(right));
    jit.loadPtr(Address(left, JSString::offsetOfValue()), first);
    jit.loadPtr(Address(right, JSString::offsetOfValue()), second);
    notTwoFlatStrings.append(jit.branchIfRopeStringImpl(first));
    notTwoFlatStrings.append(jit.branchIfRopeStringImpl(second));

    JumpList needsLeaf;
    JumpList isLess;
    JumpList isEqual;
    auto returnAnswerFor = [&](int order) {
        jit.move(TrustedImm32(holdsFor(operation, order)), left);
        jit.returnToCaller();
    };
    branchOnOrderOfFlatStrings(jit, { left, right, first, second, registers.scratch, jit.allocateScratch(StubRegister::T12), jit.allocateScratch(StubRegister::T14) }, needsLeaf, isLess, isEqual);
    returnAnswerFor(1);
    isLess.link(&jit);
    returnAnswerFor(-1);
    isEqual.link(&jit);
    returnAnswerFor(0);

    needsLeaf.link(&jit);
    const GPRReg firstArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg secondArgument = jit.clobberOperand(1);
    RELEASE_ASSERT(firstArgument == leafArgumentGPR(0) && secondArgument == leafArgumentGPR(1));
    RELEASE_ASSERT(noOverlap(first, second, firstArgument, secondArgument));
    jit.move(first, firstArgument);
    jit.move(second, secondArgument);
    jit.callLeaf(Entry::operationAOTCompareStringImpls);
    jit.compare64(signedConditionOf(operation), GPRInfo::returnValueGPR, TrustedImm32(0), left);
    jit.returnToCaller();

    notTwoFlatStrings.link(&jit);
}

StubSignature signatureOfBinaryOperation(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1) });
}

void generateBinaryOperation(StubAssembler& jit)
{
    auto [operation, isAfterInt32Check] = shapeOf(static_cast<BinaryOperationForm>(jit.key().form()));
    const BinaryRegisters registers = allocateBinaryRegisters(jit);
    JumpList notBothInt32;
    JumpList slow;

    if (!isAfterInt32Check && operation != BinaryOperator::Div) {
        notBothInt32.append(jit.branch64(CCallHelpers::Below, registers.left, numberTag));
        notBothInt32.append(jit.branch64(CCallHelpers::Below, registers.right, numberTag));
#if CPU(X86_64)
        if (!emitInt32CaseInPlace(jit, registers, operation, notBothInt32))
#endif
            emitInt32Case(jit, registers, operation, notBothInt32);
    }
    static_assert(hasDoubleCase(BinaryOperator::Add) && hasDoubleCase(BinaryOperator::Sub) && hasDoubleCase(BinaryOperator::Mul) && hasDoubleCase(BinaryOperator::Mod) && isOnBits(BinaryOperator::URShift));
    notBothInt32.link(&jit);
    if (hasDoubleCase(operation))
        emitDoubleCase(jit, registers, operation, slow);
    if (isOnBits(operation))
        emitBitCase(jit, registers, operation, slow);
    slow.link(&jit);
    if (operation == BinaryOperator::Add)
        emitRopeCase(jit, registers);
    if (isComparison(operation))
        emitStringOrder(jit, registers, operation);
    callBinaryOperation(jit, operationOf(operation));
}

static void emitOperationWithDouble(StubAssembler& jit, BinaryWithDoubleForm form, FPRReg number, FPRReg other)
{
    switch (form) {
    case BinaryWithDoubleForm::Add:
        jit.addDouble(number, other, number);
        return;
    case BinaryWithDoubleForm::Mul:
        jit.mulDouble(number, other, number);
        return;
    case BinaryWithDoubleForm::Sub:
        jit.subDouble(number, other, number);
        return;
    case BinaryWithDoubleForm::SubWithNumberOnRight:
        jit.subDouble(other, number, other);
        jit.moveDouble(other, number);
        return;
    case BinaryWithDoubleForm::Div:
        jit.divDouble(number, other, number);
        return;
    case BinaryWithDoubleForm::DivWithNumberOnRight:
        jit.divDouble(other, number, other);
        jit.moveDouble(other, number);
        return;
    }
}

StubSignature signatureOfBinaryWithDouble(StubKey)
{
    StubSignature signature;
    signature.addOperand(FPRInfo::fpRegT0);
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    signature.result = StubResult::FPR;
    signature.firstResult = FPRInfo::fpRegT0;
    return signature;
}

void generateBinaryWithDouble(StubAssembler& jit)
{
    static_assert(FPRInfo::fpRegT0 == FPRInfo::argumentFPR0 && FPRInfo::fpRegT0 == FPRInfo::returnValueFPR);
    const BinaryWithDoubleForm form = static_cast<BinaryWithDoubleForm>(jit.key().form());
    const FPRReg number = jit.resultFPR();
    const GPRReg value = jit.operand(1);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const FPRReg other = jit.allocateScratch(FPRInfo::fpRegT1);

    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, value, numberTag);
    toDouble(jit, value, scratch, other);
    emitOperationWithDouble(jit, form, number, other);
    jit.returnToCaller();

    notNumber.link(&jit);
    jit.callOperationAndReturn(operationWithDoubleOf(form), { .returns = OperationCall::Returns::Double });
}

static Entry operationOf(UnaryOperationForm form)
{
    switch (form) {
    case UnaryOperationForm::Negate: return Entry::operationAOTValueNegate;
    case UnaryOperationForm::Increment: return Entry::operationAOTIncrement;
    case UnaryOperationForm::Decrement: return Entry::operationAOTDecrement;
    case UnaryOperationForm::BitNot: return Entry::operationAOTValueBitNot;
    case UnaryOperationForm::ToNumeric: return Entry::operationAOTToNumeric;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void returnNumber(StubAssembler& jit, double number, GPRReg result)
{
    jit.move(TrustedImm64(JSValue::encode(jsNumber(number))), result);
    jit.returnToCaller();
}

static void emitUnaryOnInt32(StubAssembler& jit, UnaryOperationForm form, GPRReg value)
{
    constexpr double pastLargestInt32 = 2147483648.0;
    switch (form) {
    case UnaryOperationForm::Negate: {
        Jump isZero = jit.branchTest32(CCallHelpers::Zero, value);
        Jump overflows = jit.branchNeg32(CCallHelpers::Overflow, value);
        boxInt32AndReturn(jit, value, value);
        isZero.link(&jit);
        returnNumber(jit, -0.0, value);
        overflows.link(&jit);
        returnNumber(jit, pastLargestInt32, value);
        return;
    }
    case UnaryOperationForm::Increment: {
        Jump overflows = jit.branchAdd32(CCallHelpers::Overflow, TrustedImm32(1), value);
        boxInt32AndReturn(jit, value, value);
        overflows.link(&jit);
        returnNumber(jit, pastLargestInt32, value);
        return;
    }
    case UnaryOperationForm::Decrement: {
        Jump overflows = jit.branchSub32(CCallHelpers::Overflow, TrustedImm32(1), value);
        boxInt32AndReturn(jit, value, value);
        overflows.link(&jit);
        returnNumber(jit, -pastLargestInt32 - 1, value);
        return;
    }
    case UnaryOperationForm::BitNot:
        jit.not32(value);
        boxInt32AndReturn(jit, value, value);
        return;
    case UnaryOperationForm::ToNumeric:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitUnaryOnDouble(StubAssembler& jit, UnaryOperationForm form, GPRReg value, JumpList& slow)
{
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg other = jit.allocateScratch(FPRInfo::fpRegT1);

    unboxDouble(jit, value, scratch, number);
    switch (form) {
    case UnaryOperationForm::Negate:
        jit.move(TrustedImm32(-1), scratch);
        jit.convertInt32ToDouble(scratch, other);
        jit.mulDouble(other, number);
        break;
    case UnaryOperationForm::Increment:
    case UnaryOperationForm::Decrement:
        jit.move(TrustedImm32(form == UnaryOperationForm::Increment ? 1 : -1), scratch);
        jit.convertInt32ToDouble(scratch, other);
        jit.addDouble(other, number);
        break;
    case UnaryOperationForm::BitNot:
        emitDoubleToInt32(jit, number, scratch, slow);
        jit.not32(scratch);
        boxInt32AndReturn(jit, scratch, value);
        return;
    case UnaryOperationForm::ToNumeric:
        RELEASE_ASSERT_NOT_REACHED();
    }
    JumpList notInt32;
    branchIfNotExactInt32(jit, number, scratch, notInt32, other);
    boxInt32AndReturn(jit, scratch, value);
    notInt32.link(&jit);
    boxDouble(jit, PureDouble::resultOfArithmeticOnBoxedNumbers(number), value);
    jit.returnToCaller();
}

StubSignature signatureOfUnaryOperation(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::R0) });
}

void generateUnaryOperation(StubAssembler& jit)
{
    const UnaryOperationForm form = static_cast<UnaryOperationForm>(jit.key().form());
    const GPRReg value = jit.clobberOperand(0);
    RELEASE_ASSERT(value == jit.result());
    JumpList slow;

    if (form == UnaryOperationForm::ToNumeric) {
        slow.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
        jit.returnToCaller();
    } else {
        Jump notInt32 = jit.branch64(CCallHelpers::Below, value, numberTag);
        emitUnaryOnInt32(jit, form, value);
        notInt32.link(&jit);
        slow.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
        emitUnaryOnDouble(jit, form, value, slow);
    }
    slow.link(&jit);
    jit.callOperationAndReturn(operationOf(form), { .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = value });
}

static constexpr bool isInFPR(BoxedNumberForm form)
{
    return form == BoxedNumberForm::Double || form == BoxedNumberForm::OptionalDouble;
}

StubSignature signatureOfBoxNumber(StubKey key)
{
    if (isInFPR(static_cast<BoxedNumberForm>(key.form())))
        return StubSignature::withGPRResult({ FPRInfo::argumentFPR0 });
    return StubSignature::withGPRResult({ gprOf(StubRegister::R0) });
}

static void boxMarkerAndReturn(StubAssembler& jit, GPRReg bitsAndResult, const SlotMarkers& markers)
{
    constexpr unsigned markerShift = 48;
    RELEASE_ASSERT(!(markers.undefined << (64 - markerShift)) && !(markers.null << (64 - markerShift)) && !(markers.absent << (64 - markerShift)));

    jit.urshift64(TrustedImm32(markerShift), bitsAndResult);
    Jump isUndefined = jit.branch32(CCallHelpers::Equal, bitsAndResult, TrustedImm32(static_cast<int32_t>(markers.undefined >> markerShift)));
    Jump isNull = jit.branch32(CCallHelpers::Equal, bitsAndResult, TrustedImm32(static_cast<int32_t>(markers.null >> markerShift)));
    jit.move(TrustedImm64(static_cast<int64_t>(boxedMarkers.absent)), bitsAndResult);
    jit.returnToCaller();

    isUndefined.link(&jit);
    jit.move(TrustedImm64(static_cast<int64_t>(boxedMarkers.undefined)), bitsAndResult);
    jit.returnToCaller();

    isNull.link(&jit);
    jit.move(TrustedImm64(static_cast<int64_t>(boxedMarkers.null)), bitsAndResult);
    jit.returnToCaller();
}

static void boxDoubleAndReturn(StubAssembler& jit, FPRReg number, GPRReg result, FPRReg scratch)
{
    JumpList isNotInt32;
    branchIfNotExactInt32(jit, number, result, isNotInt32, scratch);
    boxExactInt32(jit, result, result);
    jit.returnToCaller();

    isNotInt32.link(&jit);
    Jump isNaN;
    boxDouble(jit, PureDouble::branchIfNaN(jit, number, isNaN), result);
    jit.returnToCaller();

    isNaN.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(jsNaN())), result);
    jit.returnToCaller();
}

void generateBoxNumber(StubAssembler& jit)
{
    const BoxedNumberForm form = static_cast<BoxedNumberForm>(jit.key().form());
    const GPRReg result = jit.result();

    if (isInFPR(form)) {
        const FPRReg number = jit.operandFPR(0);
        const FPRReg scratch = jit.allocateScratch(FPRInfo::argumentFPR1);
        Jump isMarker;
        if (form == BoxedNumberForm::OptionalDouble) {
            jit.moveDoubleTo64(number, result);
            isMarker = jit.branch64(CCallHelpers::GreaterThanOrEqual, result, TrustedImm64(static_cast<int64_t>(doubleMarkers.undefined)));
        }
        boxDoubleAndReturn(jit, number, result, scratch);
        if (form == BoxedNumberForm::OptionalDouble) {
            isMarker.link(&jit);
            boxMarkerAndReturn(jit, result, doubleMarkers);
        }
        return;
    }

    const GPRReg integer = jit.clobberOperand(0);
    const GPRReg low = jit.allocateScratch(StubRegister::T9);
    const FPRReg number = jit.allocateScratch(FPRInfo::argumentFPR0);
    RELEASE_ASSERT(integer == result);
    jit.signExtend32To64(integer, low);
    Jump isNotInt32 = jit.branch64(CCallHelpers::NotEqual, low, integer);
    boxInt32AndReturn(jit, integer, result);

    isNotInt32.link(&jit);
    Jump isMarker;
    if (form == BoxedNumberForm::OptionalInteger)
        isMarker = jit.branch64(CCallHelpers::GreaterThanOrEqual, integer, TrustedImm64(static_cast<int64_t>(integerMarkers.undefined)));
    boxDouble(jit, PureDouble::convertInt64(jit, integer, number), result);
    jit.returnToCaller();
    if (form == BoxedNumberForm::OptionalInteger) {
        isMarker.link(&jit);
        boxMarkerAndReturn(jit, result, integerMarkers);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
