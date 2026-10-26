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
#include <limits>

namespace JSC { namespace AOT {

struct BinaryOperationShape {
    BinaryOperator operation;
    bool isAfterInt32Check;
};

static constexpr BinaryOperationShape shapeOfBinaryOperation(uint8_t form)
{
    if (form < numberOfBinaryOperators)
        return { static_cast<BinaryOperator>(form), false };
    form -= numberOfBinaryOperators;
    form += form >= static_cast<uint8_t>(BinaryOperator::Div);
    form += form >= static_cast<uint8_t>(BinaryOperator::Mod);
    return { static_cast<BinaryOperator>(form), true };
}

static_assert([] {
    for (uint8_t form = 0; form < descriptorOf(StubFamily::BinaryOperation).numberOfForms; ++form) {
        BinaryOperationShape shape = shapeOfBinaryOperation(form);
        if (binaryOperationForm(shape.operation, shape.isAfterInt32Check) != form)
            return false;
    }
    return true;
}());
static_assert(binaryOperationForm(BinaryOperator::GreaterEq, true) + 1 == descriptorOf(StubFamily::BinaryOperation).numberOfForms);
static_assert(static_cast<uint8_t>(UnaryOperationForm::ToNumeric) + 1 == descriptorOf(StubFamily::UnaryOperation).numberOfForms);

static constexpr Entry binaryOperations[numberOfBinaryOperators] = {
    Entry::operationAOTValueAdd, Entry::operationAOTValueSub, Entry::operationAOTValueMul, Entry::operationAOTValueDiv,
    Entry::operationAOTValueBitAnd, Entry::operationAOTValueBitOr, Entry::operationAOTValueBitXor,
    Entry::operationAOTValueLShift, Entry::operationAOTValueRShift, Entry::operationAOTValueURShift, Entry::operationAOTValueMod,
    Entry::operationAOTCompareLess, Entry::operationAOTCompareLessEq, Entry::operationAOTCompareGreater, Entry::operationAOTCompareGreaterEq,
};

struct Comparison {
    CCallHelpers::RelationalCondition ofIntegers;
    CCallHelpers::DoubleCondition ofDoubles;
    bool holdsForLess;
    bool holdsForEqual;
    bool holdsForGreater;
};

static const Comparison& comparisonOf(BinaryOperator operation)
{
    static constexpr Comparison comparisons[] = {
        { CCallHelpers::LessThan, CCallHelpers::DoubleLessThanAndOrdered, true, false, false },
        { CCallHelpers::LessThanOrEqual, CCallHelpers::DoubleLessThanOrEqualAndOrdered, true, true, false },
        { CCallHelpers::GreaterThan, CCallHelpers::DoubleGreaterThanAndOrdered, false, false, true },
        { CCallHelpers::GreaterThanOrEqual, CCallHelpers::DoubleGreaterThanOrEqualAndOrdered, false, true, true },
    };
    RELEASE_ASSERT(isComparison(operation));
    return comparisons[static_cast<uint8_t>(operation) - static_cast<uint8_t>(BinaryOperator::Less)];
}

struct BinaryRegisters {
    GPRReg left;
    GPRReg right;
    GPRReg first;
    GPRReg second;
    GPRReg scratch;
    FPRReg leftNumber;
    FPRReg rightNumber;
};

static void boxInt32AndReturn(StubAssembler& jit, GPRReg int32, GPRReg result)
{
    boxInt32(jit, int32, result);
    jit.returnToCaller();
}

static void boxResultOfArithmeticAndReturn(StubAssembler& jit, FPRReg number, FPRReg scratchNumber, GPRReg scratch, GPRReg result)
{
    JumpList notInt32;
    branchIfNotExactInt32(jit, number, scratch, notInt32, scratchNumber);
    boxInt32AndReturn(jit, scratch, result);
    notInt32.link(&jit);
    boxDouble(jit, PureDouble::resultOfArithmeticOnBoxedNumbers(number), result);
    jit.returnToCaller();
}

static void emitOnBits(StubAssembler& jit, BinaryOperator operation, GPRReg left, GPRReg right, GPRReg result)
{
    switch (operation) {
    case BinaryOperator::BitAnd:
        return jit.and32(left, right, result);
    case BinaryOperator::BitOr:
        return jit.or32(left, right, result);
    case BinaryOperator::BitXor:
        return jit.xor32(left, right, result);
    case BinaryOperator::LShift:
        return jit.lshift32(left, right, result);
    case BinaryOperator::RShift:
        return jit.rshift32(left, right, result);
    case BinaryOperator::URShift:
        return jit.urshift32(left, right, result);
    case BinaryOperator::Add:
    case BinaryOperator::Sub:
    case BinaryOperator::Mul:
    case BinaryOperator::Div:
    case BinaryOperator::Mod:
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The result is where the left number was.
static void emitOnDoubles(StubAssembler& jit, BinaryOperator operation, FPRReg left, FPRReg right)
{
    switch (operation) {
    case BinaryOperator::Add:
        return jit.addDouble(left, right, left);
    case BinaryOperator::Sub:
        return jit.subDouble(left, right, left);
    case BinaryOperator::Mul:
        return jit.mulDouble(left, right, left);
    case BinaryOperator::Div:
        return jit.divDouble(left, right, left);
    case BinaryOperator::Mod:
        static_assert(FPRInfo::fpRegT0 == FPRInfo::argumentFPR0 && FPRInfo::fpRegT1 == FPRInfo::argumentFPR1 && FPRInfo::fpRegT0 == FPRInfo::returnValueFPR);
        RELEASE_ASSERT(left == FPRInfo::fpRegT0 && right == FPRInfo::fpRegT1);
        return jit.callLeaf(Entry::operationAOTFmod);
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
    if (operation == BinaryOperator::Mod)
        return emitInt32Remainder(jit, registers, slow);
    if (isComparison(operation)) {
        jit.compare32(comparisonOf(operation).ofIntegers, left, right, left);
        jit.returnToCaller();
        return;
    }

    // x86-64 computes where the left operand is. The slow path gets the operand back.
    const GPRReg result = isX86_64() ? left : registers.scratch;
    const bool savesLeft = isX86_64() && (operation == BinaryOperator::Mul || operation == BinaryOperator::URShift);
    JumpList fails;
    if (savesLeft)
        jit.move(left, registers.scratch);
    switch (operation) {
    case BinaryOperator::Add:
        fails.append(jit.branchAdd32(CCallHelpers::Overflow, left, right, result));
        break;
    case BinaryOperator::Sub:
        fails.append(jit.branchSub32(CCallHelpers::Overflow, left, right, result));
        break;
    case BinaryOperator::Mul:
        fails.append(jit.branchMul32(CCallHelpers::Overflow, left, right, result));
        fails.append(jit.branchTest32(CCallHelpers::Zero, result));
        break;
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitOr:
    case BinaryOperator::BitXor:
    case BinaryOperator::LShift:
    case BinaryOperator::RShift:
    case BinaryOperator::URShift:
        emitOnBits(jit, operation, left, right, result);
        if (operation == BinaryOperator::URShift)
            fails.append(jit.branch32(CCallHelpers::LessThan, result, TrustedImm32(0)));
        break;
    case BinaryOperator::Div:
    case BinaryOperator::Mod:
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        RELEASE_ASSERT_NOT_REACHED();
    }
    boxInt32AndReturn(jit, result, left);
    if (!isX86_64() || fails.empty()) {
        slow.append(fails);
        return;
    }
    fails.link(&jit);
    if (savesLeft)
        jit.move(registers.scratch, left);
    else {
        operation == BinaryOperator::Add ? jit.sub32(right, left) : jit.add32(right, left);
        boxInt32(jit, left, left);
    }
    slow.append(jit.jump());
}

static void emitDoubleCase(StubAssembler& jit, const BinaryRegisters& registers, BinaryOperator operation, JumpList& slow)
{
    slow.append(jit.branchTest64(CCallHelpers::Zero, registers.left, numberTag));
    slow.append(jit.branchTest64(CCallHelpers::Zero, registers.right, numberTag));
    toDouble(jit, registers.left, registers.scratch, registers.leftNumber);
    toDouble(jit, registers.right, registers.scratch, registers.rightNumber);
    if (isComparison(operation)) {
        jit.compareDouble(comparisonOf(operation).ofDoubles, registers.leftNumber, registers.rightNumber, registers.left);
        jit.returnToCaller();
        return;
    }
    emitOnDoubles(jit, operation, registers.leftNumber, registers.rightNumber);
    boxResultOfArithmeticAndReturn(jit, registers.leftNumber, registers.rightNumber, registers.scratch, registers.left);
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
    const GPRReg scratch = registers.scratch;
    emitToInt32(jit, registers, registers.left, registers.first, slow);
    emitToInt32(jit, registers, registers.right, registers.second, slow);
    emitOnBits(jit, operation, registers.first, registers.second, scratch);
    if (operation == BinaryOperator::URShift) {
        Jump fitsInt32 = jit.branch32(CCallHelpers::GreaterThanOrEqual, scratch, TrustedImm32(0));
        jit.zeroExtend32ToWord(scratch, scratch);
        boxDouble(jit, PureDouble::convertInt64(jit, scratch, registers.leftNumber), registers.left);
        jit.returnToCaller();
        fitsInt32.link(&jit);
    }
    boxInt32AndReturn(jit, scratch, registers.left);
}

static void emitStringOrder(StubAssembler& jit, const BinaryRegisters& registers, BinaryOperator operation)
{
    const auto [left, right, first, second, scratch, leftNumber, rightNumber] = registers;
    const Comparison& comparison = comparisonOf(operation);

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
    auto returnAnswer = [&](bool answer) {
        jit.move(TrustedImm32(answer), left);
        jit.returnToCaller();
    };
    branchOnOrderOfFlatStrings(jit, { left, right, first, second, scratch, jit.allocateScratch(StubRegister::T12), jit.allocateScratch(StubRegister::T14) }, needsLeaf, isLess, isEqual);
    returnAnswer(comparison.holdsForGreater);
    isLess.link(&jit);
    returnAnswer(comparison.holdsForLess);
    isEqual.link(&jit);
    returnAnswer(comparison.holdsForEqual);

    needsLeaf.link(&jit);
    const GPRReg firstArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg secondArgument = jit.clobberOperand(1);
    RELEASE_ASSERT(firstArgument == leafArgumentGPR(0) && secondArgument == leafArgumentGPR(1));
    RELEASE_ASSERT(noOverlap(first, second, firstArgument, secondArgument));
    jit.move(first, firstArgument);
    jit.move(second, secondArgument);
    jit.callLeaf(Entry::operationAOTCompareStringImpls);
    jit.compare64(comparison.ofIntegers, GPRInfo::returnValueGPR, TrustedImm32(0), left);
    jit.returnToCaller();

    notTwoFlatStrings.link(&jit);
}

StubSignature signatureOfBinaryOperation(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1) }); }

void generateBinaryOperation(StubAssembler& jit)
{
    auto [operation, isAfterInt32Check] = shapeOfBinaryOperation(jit.key().form());
    const BinaryRegisters registers {
        jit.clobberOperand(0),
        jit.clobberOperand(1),
        jit.allocateScratch(StubRegister::T9),
        jit.allocateScratch(StubRegister::T10),
        jit.allocateScratch(StubRegister::T11),
        jit.allocateScratch(FPRInfo::fpRegT0),
        jit.allocateScratch(FPRInfo::fpRegT1),
    };
    JumpList notBothInt32;
    JumpList slow;

    if (!isAfterInt32Check && operation != BinaryOperator::Div) {
        notBothInt32.append(jit.branch64(CCallHelpers::Below, registers.left, numberTag));
        notBothInt32.append(jit.branch64(CCallHelpers::Below, registers.right, numberTag));
        emitInt32Case(jit, registers, operation, notBothInt32);
    }
    notBothInt32.link(&jit);
    if (hasDoubleCase(operation))
        emitDoubleCase(jit, registers, operation, slow);
    else
        emitBitCase(jit, registers, operation, slow);
    slow.link(&jit);
    if (operation == BinaryOperator::Add) {
        jit.allocateScratch(StubRegister::T13);
        JumpList notTwoStrings;
        makeRopeOfTwoValuesAndReturn(jit, registers.left, registers.right, notTwoStrings);
        notTwoStrings.link(&jit);
    }
    if (isComparison(operation))
        emitStringOrder(jit, registers, operation);
    callBinaryOperation(jit, binaryOperations[static_cast<uint8_t>(operation)]);
}

struct OperationWithDouble {
    BinaryOperator operation;
    bool numberIsOnRight;
    Entry entry;
};

static constexpr OperationWithDouble operationsWithDouble[] = {
    { BinaryOperator::Add, false, Entry::operationAOTAddNumberAndValueAsNumber },
    { BinaryOperator::Mul, false, Entry::operationAOTMulNumberAndValueAsNumber },
    { BinaryOperator::Sub, false, Entry::operationAOTSubNumberAndValueAsNumber },
    { BinaryOperator::Sub, true, Entry::operationAOTSubNumberAndValueAsNumber },
    { BinaryOperator::Div, false, Entry::operationAOTDivNumberAndValueAsNumber },
    { BinaryOperator::Div, true, Entry::operationAOTDivNumberAndValueAsNumber },
};
static_assert(std::size(operationsWithDouble) == descriptorOf(StubFamily::BinaryWithDouble).numberOfForms);
static_assert([] {
    for (size_t form = 0; form < std::size(operationsWithDouble); ++form) {
        if (static_cast<size_t>(binaryWithDoubleForm(operationsWithDouble[form].operation, operationsWithDouble[form].numberIsOnRight)) != form)
            return false;
    }
    return true;
}());

StubSignature signatureOfBinaryWithDouble(StubKey)
{
    StubSignature signature = StubSignature::withoutResult({ FPRInfo::fpRegT0, gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
    signature.result = StubResult::FPR;
    signature.firstResult = FPRInfo::fpRegT0;
    return signature;
}

void generateBinaryWithDouble(StubAssembler& jit)
{
    static_assert(FPRInfo::fpRegT0 == FPRInfo::argumentFPR0 && FPRInfo::fpRegT0 == FPRInfo::returnValueFPR);
    const auto [operation, numberIsOnRight, entry] = operationsWithDouble[jit.key().form()];
    const FPRReg number = jit.resultFPR();
    const GPRReg value = jit.operand(1);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const FPRReg other = jit.allocateScratch(FPRInfo::fpRegT1);

    Jump notNumber = jit.branchTest64(CCallHelpers::Zero, value, numberTag);
    toDouble(jit, value, scratch, other);
    if (numberIsOnRight) {
        emitOnDoubles(jit, operation, other, number);
        jit.moveDouble(other, number);
    } else
        emitOnDoubles(jit, operation, number, other);
    jit.returnToCaller();

    notNumber.link(&jit);
    jit.callOperationAndReturn(entry, { .returns = OperationCall::Returns::Double });
}

static void returnNumber(StubAssembler& jit, double number, GPRReg result)
{
    jit.move(TrustedImm64(JSValue::encode(jsNumber(number))), result);
    jit.returnToCaller();
}

static void emitUnaryOnInt32(StubAssembler& jit, UnaryOperationForm form, GPRReg value)
{
    constexpr double pastLargestInt32 = 2147483648.0;
    Jump isZero;
    Jump overflows;
    switch (form) {
    case UnaryOperationForm::Negate:
        isZero = jit.branchTest32(CCallHelpers::Zero, value);
        overflows = jit.branchNeg32(CCallHelpers::Overflow, value);
        break;
    case UnaryOperationForm::Increment:
        overflows = jit.branchAdd32(CCallHelpers::Overflow, TrustedImm32(1), value);
        break;
    case UnaryOperationForm::Decrement:
        overflows = jit.branchSub32(CCallHelpers::Overflow, TrustedImm32(1), value);
        break;
    case UnaryOperationForm::BitNot:
        jit.not32(value);
        break;
    case UnaryOperationForm::ToNumeric:
        RELEASE_ASSERT_NOT_REACHED();
    }
    boxInt32AndReturn(jit, value, value);
    if (isZero.isSet()) {
        isZero.link(&jit);
        returnNumber(jit, -0.0, value);
    }
    if (overflows.isSet()) {
        overflows.link(&jit);
        returnNumber(jit, form == UnaryOperationForm::Decrement ? -pastLargestInt32 - 1 : pastLargestInt32, value);
    }
}

static void emitUnaryOnDouble(StubAssembler& jit, UnaryOperationForm form, GPRReg value, JumpList& slow)
{
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg other = jit.allocateScratch(FPRInfo::fpRegT1);

    // The sign of a boxed double is the highest bit of the box. A NaN changes its sign too, which a product with -1 would keep.
    if (form == UnaryOperationForm::Negate)
        jit.xor64(TrustedImm64(std::numeric_limits<int64_t>::min()), value);
    unboxDouble(jit, value, scratch, number);
    if (form == UnaryOperationForm::BitNot) {
        emitDoubleToInt32(jit, number, scratch, slow);
        jit.not32(scratch);
        boxInt32AndReturn(jit, scratch, value);
        return;
    }
    if (form != UnaryOperationForm::Negate) {
        jit.move(TrustedImm32(form == UnaryOperationForm::Increment ? 1 : -1), scratch);
        jit.convertInt32ToDouble(scratch, other);
        jit.addDouble(other, number);
    }
    boxResultOfArithmeticAndReturn(jit, number, other, scratch, value);
}

StubSignature signatureOfUnaryOperation(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::R0) }); }

void generateUnaryOperation(StubAssembler& jit)
{
    static constexpr Entry operations[] = { Entry::operationAOTValueNegate, Entry::operationAOTIncrement, Entry::operationAOTDecrement, Entry::operationAOTValueBitNot, Entry::operationAOTToNumeric };
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
    jit.callOperationAndReturn(operations[jit.key().form()], { .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = value });
}

static constexpr bool isInFPR(BoxedNumberForm form) { return form == BoxedNumberForm::Double || form == BoxedNumberForm::OptionalDouble; }

StubSignature signatureOfBoxNumber(StubKey key)
{
    return StubSignature::withGPRResult({ isInFPR(static_cast<BoxedNumberForm>(key.form())) ? Reg(FPRInfo::argumentFPR0) : Reg(gprOf(StubRegister::R0)) });
}

static void boxMarkerAndReturn(StubAssembler& jit, GPRReg bitsAndResult, const SlotMarkers& markers)
{
    constexpr unsigned markerShift = 48;
    RELEASE_ASSERT(!(markers.undefined << (64 - markerShift)) && !(markers.null << (64 - markerShift)) && !(markers.absent << (64 - markerShift)));
    auto returnMarker = [&](uint64_t marker) {
        jit.move(TrustedImm64(static_cast<int64_t>(marker)), bitsAndResult);
        jit.returnToCaller();
    };

    jit.urshift64(TrustedImm32(markerShift), bitsAndResult);
    Jump isUndefined = jit.branch32(CCallHelpers::Equal, bitsAndResult, TrustedImm32(static_cast<int32_t>(markers.undefined >> markerShift)));
    Jump isNull = jit.branch32(CCallHelpers::Equal, bitsAndResult, TrustedImm32(static_cast<int32_t>(markers.null >> markerShift)));
    returnMarker(boxedMarkers.absent);
    isUndefined.link(&jit);
    returnMarker(boxedMarkers.undefined);
    isNull.link(&jit);
    returnMarker(boxedMarkers.null);
}

static constexpr bool isNaN(uint64_t bits) { return (bits >> 52 & 0x7ff) == 0x7ff && bits << 12; }
static_assert(isNaN(doubleMarkers.undefined) && isNaN(doubleMarkers.null) && isNaN(doubleMarkers.absent));

void generateBoxNumber(StubAssembler& jit)
{
    const BoxedNumberForm form = static_cast<BoxedNumberForm>(jit.key().form());
    const GPRReg result = jit.result();
    Jump isMarker;

    if (isInFPR(form)) {
        const FPRReg number = jit.operandFPR(0);
        const FPRReg scratch = jit.allocateScratch(FPRInfo::argumentFPR1);
        JumpList isNotInt32;
        branchIfNotExactInt32(jit, number, result, isNotInt32, scratch);
        boxExactInt32(jit, result, result);
        jit.returnToCaller();

        isNotInt32.link(&jit);
        Jump isNaN;
        boxDouble(jit, PureDouble::branchIfNaN(jit, number, isNaN), result);
        jit.returnToCaller();

        // Every marker is a NaN.
        isNaN.link(&jit);
        if (form == BoxedNumberForm::OptionalDouble) {
            jit.moveDoubleTo64(number, result);
            isMarker = jit.branch64(CCallHelpers::GreaterThanOrEqual, result, TrustedImm64(static_cast<int64_t>(doubleMarkers.undefined)));
        }
        boxNaN(jit, number, result);
        jit.returnToCaller();
    } else {
        const GPRReg integer = jit.clobberOperand(0);
        const GPRReg low = jit.allocateScratch(StubRegister::T9);
        const FPRReg number = jit.allocateScratch(FPRInfo::argumentFPR0);
        RELEASE_ASSERT(integer == result);
        jit.signExtend32To64(integer, low);
        Jump isNotInt32 = jit.branch64(CCallHelpers::NotEqual, low, integer);
        boxInt32AndReturn(jit, integer, result);

        isNotInt32.link(&jit);
        if (form == BoxedNumberForm::OptionalInteger)
            isMarker = jit.branch64(CCallHelpers::GreaterThanOrEqual, integer, TrustedImm64(static_cast<int64_t>(integerMarkers.undefined)));
        boxDouble(jit, PureDouble::convertInt64(jit, integer, number), result);
        jit.returnToCaller();
    }
    if (isMarker.isSet()) {
        isMarker.link(&jit);
        boxMarkerAndReturn(jit, result, isInFPR(form) ? doubleMarkers : integerMarkers);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
